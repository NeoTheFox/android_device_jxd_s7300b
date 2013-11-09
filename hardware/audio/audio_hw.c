/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_primary"
#define LOG_NDEBUG 0
//#define LOG_NDEBUG_FUNCTION
#ifdef LOG_NDEBUG_FUNCTION
#define LOGFUNC(...) ((void)0)
#else
#define LOGFUNC(...) (ALOGD(__VA_ARGS__))
#endif

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <cutils/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>


#if defined(AML_AUDIO_RT5631)
#include "rt5631_mixer_ctrl.h"
#elif defined(AML_AUDIO_M3CODEC)
#include "m3codec_mixer_ctl.h"
#elif defined(AML_AUDIO_WM8960)
#include "wm8960_mixer_ctrl.h"
#else
struct route_setting
{
	char *ctl_name;
	int intval;
	char *strval;
};
struct route_setting mic_input[] = 
{
	{
		.ctl_name = NULL,
	},
};
struct route_setting line_input[] = 
{
	{
		.ctl_name = NULL,
	},
};
static struct route_setting output_headphone[] = 
{
	{
		.ctl_name = NULL,
	},
};static struct route_setting output_speaker[] = 
{
	{
		.ctl_name = NULL,
	},
};
#endif

/* ALSA cards for AML */
#define CARD_AMLOGIC_BOARD 0 
#define CARD_AMLOGIC_USB 1
#define CARD_AMLOGIC_DEFAULT CARD_AMLOGIC_BOARD 
/* ALSA ports for AML */
#define PORT_MM 0
/* number of frames per period */
#define PERIOD_SIZE 1024
/* number of periods for low power playback */
#define PLAYBACK_PERIOD_COUNT 4
/* number of periods for capture */
#define CAPTURE_PERIOD_COUNT 4

/* minimum sleep time in out_write() when write threshold is not reached */
#define MIN_WRITE_SLEEP_US 5000

#define RESAMPLER_BUFFER_FRAMES (PERIOD_SIZE * 6)
#define RESAMPLER_BUFFER_SIZE (4 * RESAMPLER_BUFFER_FRAMES)

#define DEFAULT_OUT_SAMPLING_RATE 48000

/* sampling rate when using MM low power port */
#define MM_LOW_POWER_SAMPLING_RATE 44100
/* sampling rate when using MM full power port */
#define MM_FULL_POWER_SAMPLING_RATE 48000
/* sampling rate when using VX port for narrow band */
#define VX_NB_SAMPLING_RATE 8000

struct pcm_config pcm_config_out = {
    .channels = 2,
    .rate = MM_FULL_POWER_SAMPLING_RATE,
    .period_size = PERIOD_SIZE,
    .period_count = PLAYBACK_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_in = {
    .channels = 2,
    .rate = MM_FULL_POWER_SAMPLING_RATE,
    .period_size = PERIOD_SIZE,
    .period_count = PLAYBACK_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};
struct mixer_ctls{   
	struct mixer_ctl *master_playback_volume;    
	struct mixer_ctl *capture_volume;
};

struct aml_audio_device {
    struct audio_hw_device hw_device;

	pthread_mutex_t lock;       /* see note below on mutex acquisition order */
	struct mixer *mixer;
	struct mixer_ctls mixer_ctls;
    int mode;
    int devices;
    int in_call;
    int cur_devices;
	struct pcm *pcm_modem_dl;
	struct pcm *pcm_modem_ul;
	struct aml_stream_in *active_input;
	struct aml_stream_out *active_output;

	bool mic_mute;
    struct echo_reference_itfe *echo_reference;
    bool bluetooth_nrec;
    bool low_power;

    /* RIL */
    //struct ril_handle ril;
};

struct aml_stream_out {
    struct audio_stream_out stream;
	pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    struct resampler_itfe *resampler;
    char *buffer;
    int standby;
    struct echo_reference_itfe *echo_reference;
    struct aml_audio_device *dev;
    int write_threshold;
    bool low_power;
};

#define MAX_PREPROCESSORS 3 /* maximum one AGC + one NS + one AEC per input stream */
struct aml_stream_in {
    struct audio_stream_in stream;
    pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    struct pcm_config config;
    struct pcm *pcm;
    int device;
    struct resampler_itfe *resampler;
	struct resampler_itfe *up_resampler;//shuai
    struct resampler_buffer_provider buf_provider;
	char *up_resampler_buffer;
	struct resampler_buffer_provider up_buf_provider;
    int16_t *buffer;
    size_t frames_in;
    unsigned int requested_rate;
    int standby;
    int source;
    struct echo_reference_itfe *echo_reference;
    bool need_echo_reference;
    effect_handle_t preprocessors[MAX_PREPROCESSORS];
    int num_preprocessors;
    int16_t *proc_buf;
    size_t proc_buf_size;
    size_t proc_frames_in;
    int16_t *ref_buf;
    size_t ref_buf_size;
    size_t ref_frames_in;
    int read_status;
    int voip_mode;

    struct aml_audio_device *dev;
};

static char cache_buffer_bytes[64];
static uint cached_len=0;
static void select_output_device(struct aml_audio_device *adev);
static void select_input_device(struct aml_audio_device *adev);
static int adev_set_voice_volume(struct audio_hw_device *dev, float volume);
static int do_input_standby(struct aml_stream_in *in);
static int do_output_standby(struct aml_stream_out *out);

/* The enable flag when 0 makes the assumption that enums are disabled by
 * "Off" and integers/booleans by 0 */
static int set_route_by_array(struct mixer *mixer, struct route_setting *route,
                              int enable)
{
    struct mixer_ctl *ctl;
    unsigned int i, j;

    /* Go through the route array and set each value */
    i = 0;
    while (route[i].ctl_name) {
        ctl = mixer_get_ctl_by_name(mixer, route[i].ctl_name);
        if (!ctl)
            return -EINVAL;

        //LOGFUNC("set_route_by_array().... index[%d] enable[%d]  : ctl_name=%s, strval=%s, intval=%d", 
        //    i, enable, route[i].ctl_name, route[i].strval,route[i].intval);
        if (route[i].strval) {
            if (enable)
                mixer_ctl_set_enum_by_string(ctl, route[i].strval);
            else
                mixer_ctl_set_enum_by_string(ctl, "Off");
        } else {
            /* This ensures multiple (i.e. stereo) values are set jointly */
            for (j = 0; j < mixer_ctl_get_num_values(ctl); j++) {
                if (enable)
                    mixer_ctl_set_value(ctl, j, route[i].intval);
                else
                    mixer_ctl_set_value(ctl, j, 0);
            }
        }
        i++;
    }

    return 0;
}

static void select_output_device(struct aml_audio_device *adev)
{
    LOGFUNC("%s(mode=%d, devices=%#x)", __FUNCTION__, adev->mode, adev->devices);
    int headset_on;
    int headphone_on;
    int speaker_on;
    adev->cur_devices = adev->devices;

    headset_on = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADSET;
    headphone_on = adev->devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE;
    speaker_on = adev->devices & AUDIO_DEVICE_OUT_SPEAKER;
    LOGFUNC("~~~~ %s : hs=%d , hp=%d, sp=%d", __func__, headset_on, headphone_on, speaker_on);
    if(headset_on|headphone_on)
        set_route_by_array(adev->mixer, output_headphone, headset_on | headphone_on);
    else
    {
        set_route_by_array(adev->mixer, output_speaker, speaker_on);
        LOGFUNC("speaker_on\n");
    }
}

static void select_input_device(struct aml_audio_device *adev)
{
    int mic_in = adev->devices & (AUDIO_DEVICE_IN_BUILTIN_MIC | AUDIO_DEVICE_IN_BACK_MIC);
	int headset_mic = adev->devices & AUDIO_DEVICE_IN_WIRED_HEADSET;
    LOGFUNC("~~~~ %s : devices(%#x), mic_in(%#x), headset_mic(%#x)", 
		                     __func__, adev->devices, mic_in, headset_mic);
	if(mic_in)
		set_route_by_array(adev->mixer, mic_input, mic_in);
	else if(headset_mic)
		set_route_by_array(adev->mixer, headset_input, headset_mic);
	else
		LOGFUNC("ERROR, no active input device available!\n");
	return;
}

static void force_all_standby(struct aml_audio_device *adev)
{
    struct aml_stream_in *in;
    struct aml_stream_out *out;

    LOGFUNC("%s(%p)", __FUNCTION__, adev);

    if (adev->active_output) {
        out = adev->active_output;
        pthread_mutex_lock(&out->lock);
        do_output_standby(out);
        pthread_mutex_unlock(&out->lock);
    }

    if (adev->active_input) {
        in = adev->active_input;
        pthread_mutex_lock(&in->lock);
        do_input_standby(in);
        pthread_mutex_unlock(&in->lock);
    }
}
static void select_mode(struct aml_audio_device *adev)
{
    LOGFUNC("%s(devices=%#x)", __FUNCTION__, adev->devices);
    return;
    force_all_standby(adev);
    /* force earpiece route for in call state if speaker is the
        only currently selected route. This prevents having to tear
        down the modem PCMs to change route from speaker to earpiece
        after the ringtone is played, but doesn't cause a route
        change if a headset or bt device is already connected. If
        speaker is not the only thing active, just remove it from
        the route. We'll assume it'll never be used initally during
        a call. This works because we're sure that the audio policy
        manager will update the output device after the audio mode
        change, even if the device selection did not change. */
    if ((adev->devices & AUDIO_DEVICE_OUT_ALL) == AUDIO_DEVICE_OUT_SPEAKER)
        adev->devices = AUDIO_DEVICE_OUT_EARPIECE |
                        AUDIO_DEVICE_IN_BUILTIN_MIC;
    else
        adev->devices &= ~AUDIO_DEVICE_OUT_SPEAKER;
	
	select_output_device(adev);
    select_input_device(adev);
	return;
}

/*
  type : 0 -> playback, 1 -> capture  
*/
#define MAX_CARD_NUM    2
int get_external_card(int type)
{
    int card_num = 1;   // start num, 0 is defualt sound card.
    
    struct stat card_stat;
    char fpath[256];
    int ret;


    while(card_num <= MAX_CARD_NUM)
    {
        snprintf(fpath, sizeof(fpath), "/proc/asound/card%d", card_num);

        ret = stat(fpath, &card_stat);
        //ALOGV("stat : %s : %d\n", fpath, ret);

        if(ret < 0)
        {
            ret = -1;
        }
        else
        {
            snprintf(fpath, sizeof(fpath), "/dev/snd/pcmC%uD0%c", card_num, 
                type ? 'c' : 'p');
            
            ret = stat(fpath, &card_stat);
            //ALOGV("stat : %s : %d\n", fpath, ret);

            if(ret == 0)
            {                
                return card_num;
            }

        }

        card_num++;
    }

    return ret;
}

static int check_output_stream(struct aml_stream_out *out)
{
    int ret = 0;
    unsigned int card = CARD_AMLOGIC_DEFAULT;
    unsigned int port = PORT_MM;
    int ext_card;
	LOGFUNC("*******%s*********", __FUNCTION__);

    ext_card = get_external_card(0);
    if(ext_card < 0)
    {
        card = CARD_AMLOGIC_DEFAULT;
    }
    else
    {
        card = ext_card;
    }

    out->config.start_threshold = PERIOD_SIZE * 2;
    out->config.avail_min = 0;//SHORT_PERIOD_SIZE;
	LOGFUNC("%s(out->config.rate=%d)", __FUNCTION__,out->config.rate);

    /* this assumes routing is done previously */
    out->pcm = pcm_open(card, port, PCM_OUT /*| PCM_MMAP | PCM_NOIRQ*/, &out->config);
    if (!pcm_is_ready(out->pcm)) {
        ALOGE("check_out_stream:cannot open pcm_out driver: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        return -ENOMEM;
    }

    pcm_close(out->pcm);

    return 0;
}


/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct aml_stream_out *out)
{
    struct aml_audio_device *adev = out->dev;
    unsigned int card = CARD_AMLOGIC_DEFAULT;
    unsigned int port = PORT_MM;
	int ret;

    adev->active_output = out;

    if (adev->mode != AUDIO_MODE_IN_CALL) {
        /* FIXME: only works if only one output can be active at a time */
        select_output_device(adev);
    }
    LOGFUNC("%s(adev->devices=%#x, adev->mode=%d)", __FUNCTION__, adev->devices, adev->mode);

	card = CARD_AMLOGIC_BOARD;
    port = PORT_MM;
	LOGFUNC("------------open on board audio-------");
	    out->config.rate = MM_FULL_POWER_SAMPLING_RATE;
    /* default to low power: will be corrected in out_write if necessary before first write to
     * tinyalsa.
     */
    out->write_threshold = PLAYBACK_PERIOD_COUNT * PERIOD_SIZE;
    out->config.start_threshold = PERIOD_SIZE * PLAYBACK_PERIOD_COUNT;
    out->config.avail_min = 0;//SHORT_PERIOD_SIZE;
    
	
    if(out->config.rate!=DEFAULT_OUT_SAMPLING_RATE){
	
		ret = create_resampler(DEFAULT_OUT_SAMPLING_RATE,
	            out->config.rate,
	            2,
	            RESAMPLER_QUALITY_DEFAULT,
	            NULL,
	            &out->resampler);
	    if (ret != 0)
	    {
	        ALOGE("cannot create resampler for output");
			return ENOMEM;
		}
		out->buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */
		if (out->buffer == NULL)
			return ENOMEM;
    }

	ALOGV("channels=%d---format=%d---period_count%d---period_size%d---rate=%d---",
		                 out->config.channels, out->config.format, out->config.period_count, 
		                 out->config.period_size, out->config.rate);
    out->pcm = pcm_open(card, port, PCM_OUT /*| PCM_MMAP | PCM_NOIRQ*/, &(out->config));

    if (!pcm_is_ready(out->pcm)) {
        ALOGE("cannot open pcm_out driver: %s", pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        adev->active_output = NULL;
        return -ENOMEM;
    }
    if (adev->echo_reference != NULL)
        out->echo_reference = adev->echo_reference;
    if (out->resampler){
		out->resampler->reset(out->resampler);
    }

    return 0;
}

static int check_input_parameters(uint32_t sample_rate, int format, int channel_count)
{
    LOGFUNC("%s(sample_rate=%d, format=%d, channel_count=%d)", __FUNCTION__, sample_rate, format, channel_count);

    if (format != AUDIO_FORMAT_PCM_16_BIT)
        return -EINVAL;

    if ((channel_count < 1) || (channel_count > 2))
        return -EINVAL;

    switch(sample_rate) {
    case 8000:
    case 11025:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static size_t get_input_buffer_size(uint32_t sample_rate, int format, int channel_count)
{
    size_t size;
    size_t device_rate;

    LOGFUNC("%s(sample_rate=%d, format=%d, channel_count=%d)", __FUNCTION__, sample_rate, format, channel_count);

    if (check_input_parameters(sample_rate, format, channel_count) != 0)
        return 0;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size = (pcm_config_in.period_size * sample_rate) / pcm_config_in.rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * sizeof(short);
}

static void add_echo_reference(struct aml_stream_out *out,
                               struct echo_reference_itfe *reference)
{
    pthread_mutex_lock(&out->lock);
    out->echo_reference = reference;
    pthread_mutex_unlock(&out->lock);
}

static void remove_echo_reference(struct aml_stream_out *out,
                                  struct echo_reference_itfe *reference)
{
    pthread_mutex_lock(&out->lock);
    if (out->echo_reference == reference) {
        /* stop writing to echo reference */
        reference->write(reference, NULL);
        out->echo_reference = NULL;
    }
    pthread_mutex_unlock(&out->lock);
}

static void put_echo_reference(struct aml_audio_device *adev,
                          struct echo_reference_itfe *reference)
{
    if (adev->echo_reference != NULL &&
            reference == adev->echo_reference) {
        if (adev->active_output != NULL)
            remove_echo_reference(adev->active_output, reference);
        release_echo_reference(reference);
        adev->echo_reference = NULL;
    }
}

static struct echo_reference_itfe *get_echo_reference(struct aml_audio_device *adev,
                                               audio_format_t format,
                                               uint32_t channel_count,
                                               uint32_t sampling_rate)
{
    put_echo_reference(adev, adev->echo_reference);
    if (adev->active_output != NULL) {
        struct audio_stream *stream = &adev->active_output->stream.common;
        uint32_t wr_channel_count = popcount(stream->get_channels(stream));
        uint32_t wr_sampling_rate = stream->get_sample_rate(stream);

        int status = create_echo_reference(AUDIO_FORMAT_PCM_16_BIT,
                                           channel_count,
                                           sampling_rate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           wr_channel_count,
                                           wr_sampling_rate,
                                           &adev->echo_reference);
        if (status == 0)
            add_echo_reference(adev->active_output, adev->echo_reference);
    }
    return adev->echo_reference;
}

static int get_playback_delay(struct aml_stream_out *out,
                       size_t frames,
                       struct echo_reference_buffer *buffer)
{

    size_t kernel_frames;
    int status;
    status = pcm_get_htimestamp(out->pcm, &kernel_frames, &buffer->time_stamp);
    if (status < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        ALOGV("get_playback_delay(): pcm_get_htimestamp error,"
                "setting playbackTimestamp to 0");
        return status;
    }
    kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;
	ALOGV("~~pcm_get_buffer_size(out->pcm)=%d",pcm_get_buffer_size(out->pcm));
    /* adjust render time stamp with delay added by current driver buffer.
     * Add the duration of current frame as we want the render time of the last
     * sample being written. */
    buffer->delay_ns = (long)(((int64_t)(kernel_frames + frames)* 1000000000)/
                            DEFAULT_OUT_SAMPLING_RATE);

	ALOGV("get_playback_delay time_stamp = [%ld].[%ld], delay_ns: [%d],"
		 "kernel_frames:[%d]",
	 	buffer->time_stamp.tv_sec , buffer->time_stamp.tv_nsec, buffer->delay_ns,
	 	kernel_frames);
    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
	//struct aml_stream_out *out = (struct aml_stream_out *)stream;
    LOGFUNC("%s(%p)", __FUNCTION__, stream);
	//return out->config.rate;
    return DEFAULT_OUT_SAMPLING_RATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, rate);

    return 0;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct aml_stream_out *out = (struct aml_stream_out *)stream;

    LOGFUNC("%s(out->config.rate=%d)", __FUNCTION__, out->config.rate);

    /* take resampling into account and return the closest majoring
         * multiple of 16 frames, as audioflinger expects audio buffers to
         * be a multiple of 16 frames
         */
    size_t size = (PERIOD_SIZE * DEFAULT_OUT_SAMPLING_RATE) / out->config.rate;
    //size_t size = (PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * DEFAULT_OUT_SAMPLING_RATE) / out->config.rate;
	size = ((size + 15) / 16) * 16;
    return size * audio_stream_frame_size((struct audio_stream *)stream);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    //LOGFUNC("%s(%p)", __FUNCTION__, stream);
    struct aml_stream_out *out = (struct aml_stream_out *)stream;

    if (out->config.channels == 1) {
        return AUDIO_CHANNEL_OUT_MONO;
    } else {
        return AUDIO_CHANNEL_OUT_STEREO;
    }
    
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
   // LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, int format)
{
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    return 0;
}

/* must be called with hw device and output stream mutexes locked */
static int do_output_standby(struct aml_stream_out *out)
{
    struct aml_audio_device *adev = out->dev;

    LOGFUNC("%s(%p)", __FUNCTION__, out);

    if (!out->standby) {
	    ALOGV("~~~~ do_output_standby()....  call pcm_close()");
        pcm_close(out->pcm);
        out->pcm = NULL;

        adev->active_output = 0;

        /* if in call, don't turn off the output stage. This will
        be done when the call is ended */
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            /* FIXME: only works if only one output can be active at a time */
            set_route_by_array(adev->mixer, output_headphone, 0);
            set_route_by_array(adev->mixer, output_speaker, 0);
        }

        /* stop writing to echo reference */
        if (out->echo_reference != NULL) {
            out->echo_reference->write(out->echo_reference, NULL);
            out->echo_reference = NULL;
        }

        out->standby = 1;
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct aml_stream_out *out = (struct aml_stream_out *)stream;
    int status;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, fd);
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct aml_stream_out *out = (struct aml_stream_out *)stream;
    struct aml_audio_device *adev = out->dev;
    struct aml_stream_in *in;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;
    bool force_input_standby = false;

    LOGFUNC("%s(kvpairs(%s), devices=%#x)", __FUNCTION__, kvpairs, adev->devices);
    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (((adev->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            if (out == adev->active_output) {
                do_output_standby(out);
                /* a change in output device may change the microphone selection */
                if (adev->active_input &&
                        adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION) {
                    force_input_standby = true;
                }
                /* force standby if moving to/from HDMI */
                if (((val & AUDIO_DEVICE_OUT_AUX_DIGITAL) ^
                        (adev->devices & AUDIO_DEVICE_OUT_AUX_DIGITAL)) ||
                        ((val & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET) ^
                        (adev->devices & AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET)))
                    do_output_standby(out);
            }
            adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
            adev->devices |= val;
            select_output_device(adev);
        }
        pthread_mutex_unlock(&out->lock);
        if (force_input_standby) {
            in = adev->active_input;
            pthread_mutex_lock(&in->lock);
            do_input_standby(in);
            pthread_mutex_unlock(&in->lock);
        }
        pthread_mutex_unlock(&adev->lock);
    }

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    //LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, keys);
    return strdup("");
}

#if 1
static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
	struct aml_stream_out *out = (struct aml_stream_out *)stream;    
	//LOGFUNC("%s(out->pcm=%p)", __FUNCTION__, out->pcm);    
	return (PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * 1000) / out->config.rate;
}
#else
static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct aml_stream_out *out = (struct aml_stream_out *)stream;
    LOGFUNC("%s(out->pcm=%p)", __FUNCTION__, out->pcm);

	if(out->pcm){
	    size_t kernel_frames;
	    int status;
	    struct timespec tstamp;
		LOGFUNC("%s(tstamp=%p)", __FUNCTION__, tstamp);

	    status = pcm_get_htimestamp(out->pcm, &kernel_frames, &tstamp);
	    if (status < 0) {
	        tstamp.tv_sec  = 0;
	        tstamp.tv_nsec = 0;

	        ALOGE(...)("out_get_latency(): pcm_get_htimestamp error,");
	        //return status;
	    }
	    kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;
		    LOGFUNC("%s(latency=%d)", __FUNCTION__, kernel_frames);
		return (kernel_frames * 1000) / out->config.rate;
	}
	return (PERIOD_SIZE * PLAYBACK_PERIOD_COUNT * 1000) / out->config.rate;
}
#endif

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{

		int ret;
		struct aml_stream_out *out = (struct aml_stream_out *)stream;
		struct aml_audio_device *adev = out->dev;
		size_t frame_size = audio_stream_frame_size(&out->stream.common);
		size_t in_frames = bytes / frame_size;
		size_t out_frames = RESAMPLER_BUFFER_SIZE / frame_size;
		bool force_input_standby = false;
		struct aml_stream_in *in;
		bool low_power;
		int kernel_frames;
		void *buf;
		char output_buffer_bytes[RESAMPLER_BUFFER_SIZE+128];
		uint ouput_len;
		char *data,  *data_dst;
		volatile char *data_src;
		short *dataprint;
		uint i, total_len;
		//LOGFUNC("entring:%s(out->echo_reference=%p, in_frames=%d)", __FUNCTION__, out->echo_reference, in_frames);
	
		/* acquiring hw device mutex systematically is useful if a low priority thread is waiting
		 * on the output stream mutex - e.g. executing select_mode() while holding the hw device
		 * mutex
		 */
		pthread_mutex_lock(&adev->lock);
		pthread_mutex_lock(&out->lock);
		if (out->standby) {
			ret = start_output_stream(out);
			if (ret != 0) {
				pthread_mutex_unlock(&adev->lock);
				goto exit;
			}
			out->standby = 0;
			/* a change in output device may change the microphone selection */
			if (adev->active_input &&
					adev->active_input->source == AUDIO_SOURCE_VOICE_COMMUNICATION)
				force_input_standby = true;
		}
		pthread_mutex_unlock(&adev->lock);
#if 0
FILE * fp=fopen("/data/audio_in","a+"); 
	if(fp){ 
		int flen=fwrite((char *)buffer,1,bytes,fp); 
		LOGFUNC("flen = %d---audio_in=%d ", flen, bytes);
		fclose(fp); 
	}else{
		LOGFUNC("could not open file: audio_in");
}	
#endif
		/* only use resampler if required */
		if (out->config.rate != DEFAULT_OUT_SAMPLING_RATE) {
	        if (!out->resampler) {
				
	            ret = create_resampler(DEFAULT_OUT_SAMPLING_RATE,
	                    out->config.rate,
	                    2,
	                    RESAMPLER_QUALITY_DEFAULT,
	                    NULL,
	                    &out->resampler);
	            if (ret != 0)
	                goto exit;
	            out->buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */
	            if (!out->buffer) {
	                ret = -ENOMEM;
	                goto exit;
	            }
	        }
			out->resampler->resample_from_input(out->resampler,
												(int16_t *)buffer,
												&in_frames,
												(int16_t *)out->buffer,
												&out_frames);
			buf = out->buffer;
		} else {
			out_frames = in_frames;
			buf = (void *)buffer;
		}
    if (out->echo_reference != NULL) {
        struct echo_reference_buffer b;
        b.raw = (void *)buffer;
        b.frame_count = in_frames;
        get_playback_delay(out, out_frames, &b);
        out->echo_reference->write(out->echo_reference, &b);
    }

#if 0
		/* do not allow more than out->write_threshold frames in kernel pcm driver buffer */
		do {
			struct timespec time_stamp;
			if (pcm_get_htimestamp(out->pcm, (unsigned int *)&kernel_frames, &time_stamp) < 0)
				break;
			kernel_frames = pcm_get_buffer_size(out->pcm) - kernel_frames;
	
			if (kernel_frames > out->write_threshold) {
				unsigned long time = (unsigned long)
						(((int64_t)(kernel_frames - out->write_threshold) * 1000000) /
								MM_FULL_POWER_SAMPLING_RATE);
								//VX_NB_SAMPLING_RATE);
				if (time < MIN_WRITE_SLEEP_US)
					time = MIN_WRITE_SLEEP_US;
				usleep(time);
			}
		} while (kernel_frames > out->write_threshold);
#endif
#if 0	
		FILE *fp1=fopen("/data/audio_out","a+"); 
		if(fp){ 
			int flen=fwrite((char *)buf,1,out_frames * frame_size,fp1); 
			LOGFUNC("flen = %d---outlen=%d ", flen, out_frames * frame_size);
			fclose(fp); 
		}else{
			LOGFUNC("could not open file:audio_out");
		}
#endif

#if 1

	if(out->config.rate != DEFAULT_OUT_SAMPLING_RATE) {
		total_len = out_frames*frame_size + cached_len;


		LOGFUNC("total_len(%d) = resampler_out_len(%d) + cached_len111(%d)", total_len, out_frames*frame_size, cached_len);

		data_src = (char *)cache_buffer_bytes;
		data_dst = (char *)output_buffer_bytes;


	
		/*write_back data from cached_buffer*/
		if(cached_len){
			memcpy((void *)data_dst, (void *)data_src, cached_len);
			data_dst += cached_len;
		}
		
		ouput_len = total_len &(~0x3f);
		data = (char*)buf;
		
		memcpy((void *)data_dst, (void *)data, ouput_len-cached_len);
		data += (ouput_len-cached_len);
		cached_len = total_len & 0x3f;
		data_src = (char *)cache_buffer_bytes;
		
		/*save data to cached_buffer*/
		if(cached_len){
			memcpy((void *)data_src, (void *)data, cached_len);
		}
		
		ret = pcm_write(out->pcm, (void *)output_buffer_bytes, ouput_len);

	}else{
		ret = pcm_write(out->pcm, (void *)buf, out_frames * frame_size);
		//ret = pcm_mmap_write(out->pcm, (void *)buf, out_frames * frame_size);
	}
#endif

	exit:
		pthread_mutex_unlock(&out->lock);
	
		if (ret != 0) {
			usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
				   out_get_sample_rate(&stream->common));
		}
	
		if (force_input_standby) {
			pthread_mutex_lock(&adev->lock);
			if (adev->active_input) {
				in = adev->active_input;
				pthread_mutex_lock(&in->lock);
				do_input_standby(in);
				pthread_mutex_unlock(&in->lock);
			}
			pthread_mutex_unlock(&adev->lock);
		}
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    LOGFUNC("%s(%p, %p)", __FUNCTION__, stream, dsp_frames);
    return -EINVAL;
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    LOGFUNC("%s(%p, %p)", __FUNCTION__, stream, effect);
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}
static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    return -EINVAL;
}

/*
static int out_set_voip_mode(struct audio_stream *stream, int mode)
{
    struct aml_stream_out *out = (struct aml_stream_out *)stream;
    ALOGE("call out_set_voip_mode old:%d, new:%d", out->voip_mode, mode);

    if(out->voip_mode == mode)
        return 0;
    else if(mode == 0)
    {
        out->voip_mode = mode;
    }
    else
    {
        out->voip_mode = mode;    

        out_standby(stream);
    }

    return 0;
}
*/
static int get_next_buffer(struct resampler_buffer_provider *buffer_provider, 
                                                         struct resampler_buffer* buffer);
static void release_buffer(struct resampler_buffer_provider *buffer_provider,
	                                                     struct resampler_buffer* buffer);


/** audio_stream_in implementation **/

/* must be called with hw device and input stream mutexes locked */
static int start_input_stream(struct aml_stream_in *in)
{
    int ret = 0;
    unsigned int card = CARD_AMLOGIC_DEFAULT;
    unsigned int port = PORT_MM;

    struct aml_audio_device *adev = in->dev;
    LOGFUNC("%s(need_echo_reference=%d, channels=%d, rate=%d, requested_rate=%d, mode= %d)", 
		__FUNCTION__, in->need_echo_reference, in->config.channels, in->config.rate, in->requested_rate, adev->mode);
    adev->active_input = in;

    if (adev->mode != AUDIO_MODE_IN_CALL) {
        adev->devices &= ~AUDIO_DEVICE_IN_ALL;
        adev->devices |= in->device;
        select_input_device(adev);
    }
	
	ALOGV("%s(in->requested_rate=%d, in->config.rate=%d)", 
		     __FUNCTION__, in->requested_rate, in->config.rate);

	if (in->need_echo_reference && in->echo_reference == NULL) {
		in->echo_reference = get_echo_reference(adev,
                                        AUDIO_FORMAT_PCM_16_BIT,
                                        in->config.channels,
										VX_NB_SAMPLING_RATE);
   										//in->requested_rate);
		LOGFUNC("%s(after get_echo_ref.... now in->echo_reference = %p)", __FUNCTION__, in->echo_reference);
#if 1
		if (in->echo_reference != NULL){
	        in->buf_provider.get_next_buffer = get_next_buffer;
	        in->buf_provider.release_buffer = release_buffer;
	        ret = create_resampler(in->config.rate,
	                               VX_NB_SAMPLING_RATE,
	                               in->config.channels,
	                               RESAMPLER_QUALITY_DEFAULT,
	                               &in->buf_provider,
	                               &in->resampler);

	        if (ret != 0) {
	            ret = -EINVAL;
	            return ret;
	        }
	        ret = create_resampler(VX_NB_SAMPLING_RATE,
	                       in->config.rate,
	                       in->config.channels,
	                       RESAMPLER_QUALITY_DEFAULT,
	                       NULL,//&in->up_buf_provider,
	                       &in->up_resampler);
		
	        if (ret != 0) {
	            ret = -EINVAL;
	            return ret;
	        }
			in->up_resampler_buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */
			if (in->up_resampler_buffer == NULL)
				return ENOMEM;
		}	
#endif
    }
    /* this assumes routing is done previously */
    in->pcm = pcm_open(card, port, PCM_IN, &in->config);
    if (!pcm_is_ready(in->pcm)) {
        ALOGE("cannot open pcm_in driver: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        adev->active_input = NULL;
        return -ENOMEM;
    }
    ALOGD("pcm_open in: card(%d), port(%d)", card, port);

    /* if no supported sample rate is available, use the resampler */
    if (in->resampler) {
        in->resampler->reset(in->resampler);
        in->frames_in = 0;
    }
    return 0;
}

static int check_input_stream(struct aml_stream_in *in)
{
    int ret = 0;
    unsigned int card = CARD_AMLOGIC_BOARD;
    unsigned int port = 0;
    int ext_card;

    ext_card = get_external_card(1);
    if(ext_card < 0)
    {
        card = CARD_AMLOGIC_BOARD;
    }
    else
    {
        card = ext_card;
    }

    /* this assumes routing is done previously */
    in->pcm = pcm_open(card, port, PCM_IN, &in->config);
    if (!pcm_is_ready(in->pcm)) {
        ALOGE("check_input_stream:cannot open pcm_in driver: %s", pcm_get_error(in->pcm));
        pcm_close(in->pcm);
        return -ENOMEM;
    }

    pcm_close(in->pcm);

    return 0;
}
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);
    return in->requested_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, rate);
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;

    LOGFUNC("%s(%p)", __FUNCTION__, stream);
    return get_input_buffer_size(in->config.rate,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 in->config.channels);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;
    //LOGFUNC("%s(%p)", __FUNCTION__, stream);

    if (in->config.channels == 1) {
        return AUDIO_CHANNEL_IN_MONO;
    } else {
        return AUDIO_CHANNEL_IN_STEREO;
    }
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    //LOGFUNC("%s(%p)", __FUNCTION__, stream);
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, format);
    return 0;
}

/* must be called with hw device and input stream mutexes locked */
static int do_input_standby(struct aml_stream_in *in)
{
    struct aml_audio_device *adev = in->dev;

    LOGFUNC("%s(%p)", __FUNCTION__, in);
    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;

        adev->active_input = 0;
        if (adev->mode != AUDIO_MODE_IN_CALL) {
            adev->devices &= ~AUDIO_DEVICE_IN_ALL;
            select_input_device(adev);
        }

        if (in->echo_reference != NULL) {
            /* stop reading from echo reference */
            in->echo_reference->read(in->echo_reference, NULL);
            put_echo_reference(adev, in->echo_reference);
            in->echo_reference = NULL;
        }

        in->standby = 1;
    }
    return 0;
}
static int in_standby(struct audio_stream *stream)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;
    int status;
    LOGFUNC("%s(%p)", __FUNCTION__, stream);

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    status = do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, stream, fd);
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;
    struct aml_audio_device *adev = in->dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret, val = 0;
    bool do_standby = false;

    LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, kvpairs);
    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value));

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((in->source != val) && (val != 0)) {
            in->source = val;
            do_standby = true;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if ((in->device != val) && (val != 0)) {
            in->device = val;
            do_standby = true;
        }
    }

    if (do_standby)
        do_input_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&adev->lock);

    str_parms_destroy(parms);
    return ret;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    //LOGFUNC("%s(%p, %s)", __FUNCTION__, stream, keys);
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    LOGFUNC("%s(%p, %f)", __FUNCTION__, stream, gain);
    return 0;
}

static void get_capture_delay(struct aml_stream_in *in,
                       size_t frames,
                       struct echo_reference_buffer *buffer)
{
    /* read frames available in kernel driver buffer */
    size_t kernel_frames;
    struct timespec tstamp;
    long buf_delay;
    long rsmp_delay;
    long kernel_delay;
    long delay_ns;
	int rsmp_mul = in->config.rate/VX_NB_SAMPLING_RATE;
    if (pcm_get_htimestamp(in->pcm, &kernel_frames, &tstamp) < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        ALOGW("read get_capture_delay(): pcm_htimestamp error");
        return;
    }

    /* read frames available in audio HAL input buffer
     * add number of frames being read as we want the capture time of first sample
     * in current buffer */
    buf_delay = (long)(((int64_t)(in->frames_in + in->proc_frames_in * rsmp_mul) * 1000000000)
                                    / in->config.rate);
    /* add delay introduced by resampler */
    rsmp_delay = 0;
    if (in->resampler) {
        rsmp_delay = in->resampler->delay_ns(in->resampler);
    }

    kernel_delay = (long)(((int64_t)kernel_frames * 1000000000) / in->config.rate);

    delay_ns = kernel_delay + buf_delay + rsmp_delay;

    buffer->time_stamp = tstamp;
    buffer->delay_ns   = delay_ns;
    ALOGV("get_capture_delay time_stamp = [%ld].[%ld], delay_ns: [%d],"
        " kernel_delay:[%ld], buf_delay:[%ld], rsmp_delay:[%ld], kernel_frames:[%d], "
         "in->frames_in:[%d], in->proc_frames_in:[%d], frames:[%d]",
         buffer->time_stamp.tv_sec , buffer->time_stamp.tv_nsec, buffer->delay_ns,
         kernel_delay, buf_delay, rsmp_delay, kernel_frames,
         in->frames_in, in->proc_frames_in, frames);

}

static int32_t update_echo_reference(struct aml_stream_in *in, size_t frames)
{
    struct echo_reference_buffer b;
    b.delay_ns = 0;

    ALOGV("update_echo_reference, frames = [%d], in->ref_frames_in = [%d],  "
          "b.frame_count = [%d]",
         frames, in->ref_frames_in, frames - in->ref_frames_in);
    if (in->ref_frames_in < frames) {
        if (in->ref_buf_size < frames) {
            in->ref_buf_size = frames;
            in->ref_buf = (int16_t *)realloc(in->ref_buf,
                                             in->ref_buf_size *
                                                 in->config.channels * sizeof(int16_t));
        }

        b.frame_count = frames - in->ref_frames_in;
        b.raw = (void *)(in->ref_buf + in->ref_frames_in * in->config.channels);

        get_capture_delay(in, frames, &b);
		LOGFUNC("update_echo_reference  return ::b.delay_ns=%d", b.delay_ns);

        if (in->echo_reference->read(in->echo_reference, &b) == 0)
        {
            in->ref_frames_in += b.frame_count;
            ALOGV("update_echo_reference: in->ref_frames_in:[%d], "
                    "in->ref_buf_size:[%d], frames:[%d], b.frame_count:[%d]",
                 in->ref_frames_in, in->ref_buf_size, frames, b.frame_count);
        }
    } else
        ALOGW("update_echo_reference: NOT enough frames to read ref buffer");
    return b.delay_ns;
}

static int set_preprocessor_param(effect_handle_t handle,
                           effect_param_t *param)
{
    uint32_t size = sizeof(int);
    uint32_t psize = ((param->psize - 1) / sizeof(int) + 1) * sizeof(int) +
                        param->vsize;

    int status = (*handle)->command(handle,
                                   EFFECT_CMD_SET_PARAM,
                                   sizeof (effect_param_t) + psize,
                                   param,
                                   &size,
                                   &param->status);
    if (status == 0)
        status = param->status;

    return status;
}

static int set_preprocessor_echo_delay(effect_handle_t handle,
                                     int32_t delay_us)
{
    uint32_t buf[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *param = (effect_param_t *)buf;
	LOGFUNC("%s(%p, %d)", __FUNCTION__, handle, delay_us);

    param->psize = sizeof(uint32_t);
    param->vsize = sizeof(uint32_t);
    *(uint32_t *)param->data = AEC_PARAM_ECHO_DELAY;
    *((int32_t *)param->data + 1) = delay_us;

    return set_preprocessor_param(handle, param);
}

static void push_echo_reference(struct aml_stream_in *in, size_t frames)
{
    /* read frames from echo reference buffer and update echo delay
     * in->ref_frames_in is updated with frames available in in->ref_buf */
    int32_t delay_us = update_echo_reference(in, frames)/1000;
    int i;
    audio_buffer_t buf;

    if (in->ref_frames_in < frames)
        frames = in->ref_frames_in;

    buf.frameCount = frames;
    buf.raw = in->ref_buf;

    for (i = 0; i < in->num_preprocessors; i++) {
        if ((*in->preprocessors[i])->process_reverse == NULL)
            continue;

        (*in->preprocessors[i])->process_reverse(in->preprocessors[i],
                                               &buf,
                                               NULL);
        set_preprocessor_echo_delay(in->preprocessors[i], delay_us);
    }

    in->ref_frames_in -= buf.frameCount;
    if (in->ref_frames_in) {
        memcpy(in->ref_buf,
               in->ref_buf + buf.frameCount * in->config.channels,
               in->ref_frames_in * in->config.channels * sizeof(int16_t));
    }
}

static int get_next_buffer(struct resampler_buffer_provider *buffer_provider,
                                   struct resampler_buffer* buffer)
{
    struct aml_stream_in *in;

    //LOGFUNC("%s(%p, %p)", __FUNCTION__, buffer_provider, buffer);
    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (struct aml_stream_in *)((char *)buffer_provider -
                                   offsetof(struct aml_stream_in, buf_provider));

    if (in->pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->frames_in == 0) {
        in->read_status = pcm_read(in->pcm,
                                   (void*)in->buffer,
                                   in->config.period_size *
                                       audio_stream_frame_size(&in->stream.common));
        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->frames_in = in->config.period_size;
    }

    buffer->frame_count = (buffer->frame_count > in->frames_in) ?
                                in->frames_in : buffer->frame_count;
    buffer->i16 = in->buffer + (in->config.period_size - in->frames_in) *
                                                in->config.channels;

    return in->read_status;

}

static void release_buffer(struct resampler_buffer_provider *buffer_provider,
                                  struct resampler_buffer* buffer)
{
    struct aml_stream_in *in;

    //LOGFUNC("%s(%p, %p)", __FUNCTION__, buffer_provider, buffer);
    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (struct aml_stream_in *)((char *)buffer_provider -
                                   offsetof(struct aml_stream_in, buf_provider));

    in->frames_in -= buffer->frame_count;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
static ssize_t read_frames(struct aml_stream_in *in, void *buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    //LOGFUNC("%s(in->resampler=%p, %ld)", __FUNCTION__, in->resampler, frames);
    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                    (int16_t *)((char *)buffer +
                            frames_wr * audio_stream_frame_size(&in->stream.common)),
                    &frames_rd);
        } else {
            struct resampler_buffer buf = {
                    { raw : NULL, },
                    frame_count : frames_rd,
            };
            get_next_buffer(&in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                           frames_wr * audio_stream_frame_size(&in->stream.common),
                        buf.raw,
                        buf.frame_count * audio_stream_frame_size(&in->stream.common));
                frames_rd = buf.frame_count;
            }
            release_buffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}
#if 0
/* process_frames() reads frames from kernel driver (via read_frames()),
 * calls the active audio pre processings and output the number of frames requested
 * to the buffer specified */
static ssize_t process_frames(struct aml_stream_in *in, void* buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
    audio_buffer_t in_buf;
    audio_buffer_t out_buf;
    int i;

    //LOGFUNC("%s(%d, %p, %ld)", __FUNCTION__, in->num_preprocessors, buffer, frames);
    while (frames_wr < frames) {
        /* first reload enough frames at the end of process input buffer */
        if (in->proc_frames_in < (size_t)frames) {
            ssize_t frames_rd;

            if (in->proc_buf_size < (size_t)frames) {
                in->proc_buf_size = (size_t)frames;
                in->proc_buf = (int16_t *)realloc(in->proc_buf,
                                         in->proc_buf_size *
                                             in->config.channels * sizeof(int16_t));
                ALOGV("process_frames(): in->proc_buf %p size extended to %d frames",
                     in->proc_buf, in->proc_buf_size);
            }
            frames_rd = read_frames(in,
                                    in->proc_buf +
                                        in->proc_frames_in * in->config.channels,
                                    frames - in->proc_frames_in);
            if (frames_rd < 0) {
                frames_wr = frames_rd;
                break;
            }
            in->proc_frames_in += frames_rd;
        }

        if (in->echo_reference != NULL)
            push_echo_reference(in, in->proc_frames_in);

         /* in_buf.frameCount and out_buf.frameCount indicate respectively
          * the maximum number of frames to be consumed and produced by process() */
        in_buf.frameCount = in->proc_frames_in;
        in_buf.s16 = in->proc_buf;
        out_buf.frameCount = frames - frames_wr;
        out_buf.s16 = (int16_t *)buffer + frames_wr * in->config.channels;

        for (i = 0; i < in->num_preprocessors; i++)
            (*in->preprocessors[i])->process(in->preprocessors[i],
                                               &in_buf,
                                               &out_buf);

        /* process() has updated the number of frames consumed and produced in
         * in_buf.frameCount and out_buf.frameCount respectively
         * move remaining frames to the beginning of in->proc_buf */
        in->proc_frames_in -= in_buf.frameCount;
        if (in->proc_frames_in) {
            memcpy(in->proc_buf,
                   in->proc_buf + in_buf.frameCount * in->config.channels,
                   in->proc_frames_in * in->config.channels * sizeof(int16_t));
        }

        /* if not enough frames were passed to process(), read more and retry. */
        if (out_buf.frameCount == 0)
            continue;

        frames_wr += out_buf.frameCount;
    }
    return frames_wr;
}
#else
/* process_frames() reads frames from kernel driver (via read_frames()),
 * calls the active audio pre processings and output the number of frames requested
 * to the buffer specified */
static ssize_t process_frames(struct aml_stream_in *in, void* buffer, ssize_t frames)
{
    ssize_t frames_wr = 0;
	ssize_t proc_frames;
    audio_buffer_t in_buf;
	audio_buffer_t proc_buf;
    audio_buffer_t out_buf;
    int i, resp_mul;
	resp_mul = in->requested_rate/VX_NB_SAMPLING_RATE;

	proc_frames = frames/resp_mul ;

	//LOGFUNC("%s(%d, %ld, %ld)", __FUNCTION__, in->num_preprocessors, frames, proc_frames);
    while (frames_wr < proc_frames) {
        /* first reload enough frames at the end of process input buffer */
        if (in->proc_frames_in < (size_t)proc_frames) {
            ssize_t frames_rd;

            if (in->proc_buf_size < (size_t)proc_frames) {
                in->proc_buf_size = (size_t)proc_frames;
                in->proc_buf = (int16_t *)realloc(in->proc_buf,
                                         in->proc_buf_size *
                                             in->config.channels * sizeof(int16_t));
                ALOGV("process_frames(): in->proc_buf %p size extended to %d frames",
                     in->proc_buf, in->proc_buf_size);
            }
            frames_rd = read_frames(in,
                                    in->proc_buf +
                                        in->proc_frames_in * in->config.channels,
                                    proc_frames - in->proc_frames_in);
            if (frames_rd < 0) {
                frames_wr = frames_rd;
                break;
            }
            in->proc_frames_in += frames_rd;
        }

        if (in->echo_reference != NULL)
            push_echo_reference(in, in->proc_frames_in);

         /* in_buf.frameCount and out_buf.frameCount indicate respectively
               * the maximum number of frames to be consumed and produced by process() */
        in_buf.frameCount = in->proc_frames_in;
        in_buf.s16 = in->proc_buf;
        proc_buf.frameCount = proc_frames - frames_wr;
        proc_buf.s16 = (int16_t *)in->up_resampler_buffer+ frames_wr * in->config.channels;
        out_buf.frameCount = (proc_frames - frames_wr) * resp_mul;
        out_buf.s16 = (int16_t *)buffer + resp_mul * frames_wr * in->config.channels;
        ALOGV("first**in_buf.frameCount=%d***\n**proc_buf.frameCount=%d\n**out_buf.frameCount=%d***", in_buf.frameCount, proc_buf.frameCount, out_buf.frameCount);

        for (i = 0; i < in->num_preprocessors; i++)
            (*in->preprocessors[i])->process(in->preprocessors[i],
                                               &in_buf,
                                               &proc_buf);//&out_buf);
        if (in->up_resampler != NULL) {
            in->up_resampler->resample_from_input(in->up_resampler,
					proc_buf.s16,
					&(proc_buf.frameCount),
                    out_buf.s16,
                    &(out_buf.frameCount));
        }
        ALOGV("after**in_buf.frameCount=%d***\n**proc_buf.frameCount=%d\n**out_buf.frameCount=%d***", in_buf.frameCount, proc_buf.frameCount, out_buf.frameCount);

        /* process() has updated the number of frames consumed and produced in
              * in_buf.frameCount and out_buf.frameCount respectively
              * move remaining frames to the beginning of in->proc_buf */
        in->proc_frames_in -= in_buf.frameCount;
        if (in->proc_frames_in) {
            memcpy(in->proc_buf,
                   in->proc_buf + in_buf.frameCount * in->config.channels,
                   in->proc_frames_in * in->config.channels * sizeof(int16_t));
        }

        /* if not enough frames were passed to process(), read more and retry. */
        if (out_buf.frameCount == 0)
            continue;

        frames_wr += out_buf.frameCount/resp_mul;//out_buf.frameCount * VX_NB_SAMPLING_RATE/in->requested_rate;
    	ALOGV("***last*****frames_wr=%ld***********", frames_wr);
    }
    return frames_wr;
}
#endif
static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
		int ret = 0;
		struct aml_stream_in *in = (struct aml_stream_in *)stream;
		struct aml_audio_device *adev = in->dev;
		size_t frames_rq = bytes / audio_stream_frame_size(&stream->common);
    //LOGFUNC("%s(in->num_preprocessors=%d, frames_rq=%d)", __FUNCTION__, in->num_preprocessors, frames_rq);
    if(in->voip_mode)
    {
        int sleepTime;

        sleepTime = 1000*bytes/(in->config.channels * in->config.rate*2);
        usleep(sleepTime*1000);
        return bytes;
    }
		/* acquiring hw device mutex systematically is useful if a low priority thread is waiting
		 * on the input stream mutex - e.g. executing select_mode() while holding the hw device
		 * mutex
		 */
		pthread_mutex_lock(&adev->lock);
		pthread_mutex_lock(&in->lock);
		if (in->standby) {
			ret = start_input_stream(in);
			if (ret == 0)
				in->standby = 0;
		}
		pthread_mutex_unlock(&adev->lock);

		if (ret < 0)
			goto exit;
//////////////////////////////////////////////////////////////////		
		if (in->need_echo_reference && in->echo_reference == NULL) {
			in->echo_reference = get_echo_reference(adev,
											AUDIO_FORMAT_PCM_16_BIT,
											in->config.channels,
											VX_NB_SAMPLING_RATE);
											//in->requested_rate);
			LOGFUNC("%s(after get_echo_ref.... now in->echo_reference = %p)", __FUNCTION__, in->echo_reference);
#if 1
			if (in->echo_reference != NULL){
				in->buf_provider.get_next_buffer = get_next_buffer;
				in->buf_provider.release_buffer = release_buffer;
				ret = create_resampler(in->config.rate,
									   VX_NB_SAMPLING_RATE,
									   in->config.channels,
									   RESAMPLER_QUALITY_DEFAULT,
									   &in->buf_provider,
									   &in->resampler);
		
				if (ret != 0) {
					ret = -EINVAL;
					return ret;
				}
				ret = create_resampler(VX_NB_SAMPLING_RATE,
							   in->config.rate,
							   in->config.channels,
							   RESAMPLER_QUALITY_DEFAULT,
							   NULL,//&in->up_buf_provider,
							   &in->up_resampler);
		
				if (ret != 0) {
					ret = -EINVAL;
					return ret;
				}
				in->up_resampler_buffer = malloc(RESAMPLER_BUFFER_SIZE); /* todo: allow for reallocing */
				if (in->up_resampler_buffer == NULL)
					return ENOMEM;
			} 
#endif
		}
	  
/////////////////////////////////////////////////////////////////////
		if (in->num_preprocessors != 0 && in->echo_reference != NULL)
		//if (in->num_preprocessors != 0)
			ret = process_frames(in, buffer, frames_rq);
		else if (in->resampler != NULL)
			ret = read_frames(in, buffer, frames_rq);
		else
			ret = pcm_read(in->pcm, buffer, bytes);
	
		if (ret > 0)
			ret = 0;
	
		if (ret == 0 && adev->mic_mute){
			LOGFUNC("%s(adev->mic_mute = %d)", __FUNCTION__, adev->mic_mute);
			memset(buffer, 0, bytes);
		}
		
#if 0
    FILE *dump_fp = NULL;

    dump_fp = fopen("/data/dump_in.pcm", "a+");
    if (dump_fp != NULL) {
        fwrite(buffer, bytes, 1, dump_fp);
        fclose(dump_fp);
    }
    else {
        ALOGW("[Error] Can't write to /data/dump_in.pcm");
    }
#endif

	exit:
		if (ret < 0)
			usleep(bytes * 1000000 / audio_stream_frame_size(&stream->common) /
				   in_get_sample_rate(&stream->common));
	
		pthread_mutex_unlock(&in->lock);
		return bytes;

}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream,
                               effect_handle_t effect)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;
    int status;
    effect_descriptor_t desc;
    LOGFUNC("*********%s", __FUNCTION__);

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors >= MAX_PREPROCESSORS) {
        status = -ENOSYS;
        goto exit;
    }

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        goto exit;

    in->preprocessors[in->num_preprocessors++] = effect;

    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        in->need_echo_reference = true;
        do_input_standby(in);
    }

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}

static int in_remove_audio_effect(const struct audio_stream *stream,
                                  effect_handle_t effect)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;
    int i;
    int status = -EINVAL;
    bool found = false;
    effect_descriptor_t desc;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->num_preprocessors <= 0) {
        status = -ENOSYS;
        goto exit;
    }

    for (i = 0; i < in->num_preprocessors; i++) {
        if (found) {
            in->preprocessors[i - 1] = in->preprocessors[i];
            continue;
        }
        if (in->preprocessors[i] == effect) {
            in->preprocessors[i] = NULL;
            status = 0;
            found = true;
        }
    }

    if (status != 0)
        goto exit;

    in->num_preprocessors--;

    status = (*effect)->get_descriptor(effect, &desc);
    if (status != 0)
        goto exit;
    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        in->need_echo_reference = false;
        do_input_standby(in);
    }

exit:

    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return status;
}
/*
static int in_set_voip_mode(struct audio_stream *stream, int mode)
{
    struct aml_stream_in *in= (struct aml_stream_in *)stream;
    ALOGE("call in_set_voip_mode old:%d, new:%d", in->voip_mode, mode);

    if(in->voip_mode == mode)
        return 0;
    else if(mode == 0)
    {
        in->voip_mode = mode;
    }
    else
    {
        in_standby(stream);

        in->voip_mode = mode;    
    }

    return 0;
}
*/
static int adev_open_output_stream(struct audio_hw_device *dev,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct aml_audio_device *ladev = (struct aml_audio_device *)dev;
    struct aml_stream_out *out;
    int channel_count = popcount(config->channel_mask);
    int ret;

    LOGFUNC("%s(devices=0x%04x,format=%d, ch=0x%04x, SR=%d)", __FUNCTION__, devices,
                        config->format, channel_count, config->sample_rate);

    out = (struct aml_stream_out *)calloc(1, sizeof(struct aml_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    //out->stream.common.set_voip_mode = out_set_voip_mode;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->config = pcm_config_out;

    ret = check_output_stream(out);
    if(ret < 0)
    {
        ALOGE("fail to open out stream, change channel count frome %d to %d", out->config.channels, channel_count);
        out->config.channels = channel_count;
    }

    if(out->config.channels == 1)
    {
        config->channel_mask = AUDIO_CHANNEL_OUT_MONO;
    }
    else if(out->config.channels == 2)
    {       
        config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    }
    else
    {
        ALOGE("Bad value of channel count : %d", out->config.channels);
    }
    out->dev = ladev;
    out->standby = 1;

   /* FIXME: when we support multiple output devices, we will want to
      * do the following:
      * adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
      * adev->devices |= out->device;
      * select_output_device(adev);
      * This is because out_set_parameters() with a route is not
      * guaranteed to be called after an output stream is opened.
      */

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);
    LOGFUNC("%s(devices=0x%04x,format=%d, ch=0x%04x, SR=%d)", __FUNCTION__, devices,
                        config->format, config->channel_mask, config->sample_rate);

    *stream_out = &out->stream;
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct aml_stream_out *out = (struct aml_stream_out *)stream;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, dev, stream);
    out_standby(&stream->common);
    if (out->buffer)
        free(out->buffer);
    if (out->resampler)
        release_resampler(out->resampler);

    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    LOGFUNC("%s(%p, %s)", __FUNCTION__, dev, kvpairs);
	
    struct aml_audio_device *adev = (struct aml_audio_device *)dev;
    struct str_parms *parms;
    char *str;
    char value[32];
    int ret;

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));
    if (ret >= 0) {
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            adev->low_power = false;
        else
            adev->low_power = true;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
                                  const char *keys)
{
    LOGFUNC("%s(%p, %s)", __FUNCTION__, dev, keys);
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    LOGFUNC("%s(%p)", __FUNCTION__, dev);
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    struct aml_audio_device *adev = (struct aml_audio_device *)dev;

    //adev->voice_volume = volume;

    LOGFUNC("%s(%p, %f)", __FUNCTION__, dev, volume);
    //if (adev->mode == AUDIO_MODE_IN_CALL)
    //    ril_set_call_volume(&adev->ril, SOUND_TYPE_VOICE, volume);

    return 0;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    LOGFUNC("%s(%p, %f)", __FUNCTION__, dev, volume);
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev,
                                  float *volume)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct aml_audio_device *adev = (struct aml_audio_device *)dev;
    LOGFUNC("%s(%p, %d)", __FUNCTION__, dev, mode);

    pthread_mutex_lock(&adev->lock);
    if (adev->mode != mode) {
        adev->mode = mode;
        select_mode(adev);
    }
    pthread_mutex_unlock(&adev->lock);

    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct aml_audio_device *adev = (struct aml_audio_device *)dev;

    LOGFUNC("%s(%p, %d)", __FUNCTION__, dev, state);
    adev->mic_mute = state;

    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    struct aml_audio_device *adev = (struct aml_audio_device *)dev;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, dev, state);
    *state = adev->mic_mute;

    return 0;

}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
                                         const struct audio_config *config)
{
    size_t size;
	int channel_count = popcount(config->channel_mask);

    LOGFUNC("%s(%p, %d, %d, %d)", __FUNCTION__, dev, config->sample_rate,
                                config->format, channel_count);
    if (check_input_parameters(config->sample_rate, config->format, channel_count) != 0)
        return 0;

    return get_input_buffer_size(config->sample_rate,
                                config->format, channel_count);

}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct aml_audio_device *ladev = (struct aml_audio_device *)dev;
    struct aml_stream_in *in;
    int ret;
	int channel_count = popcount(config->channel_mask);
    LOGFUNC("**********%s(%#x, %d, 0x%04x, %d)", __FUNCTION__,
        devices, config->format, config->channel_mask, config->sample_rate);
    if (check_input_parameters(config->sample_rate, config->format, channel_count) != 0)
        return -EINVAL;

    in = (struct aml_stream_in *)calloc(1, sizeof(struct aml_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    //in->stream.common.set_voip_mode = in_set_voip_mode;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->requested_rate = config->sample_rate;

    memcpy(&in->config, &pcm_config_in, sizeof(pcm_config_in));
    ret = check_input_stream(in);
    if(ret < 0)
    {
        ALOGE("fail to open input stream, change channel count from %d to %d", in->config.channels, channel_count);
        in->config.channels = channel_count;
    }

    if(in->config.channels == 1)
    {
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
    }
    else if(in->config.channels == 2)
    {
        config->channel_mask = AUDIO_CHANNEL_IN_STEREO;
    }
    else
    {
        ALOGE("Bad value of channel count : %d", in->config.channels);
    }
    in->buffer = malloc(in->config.period_size *
                        audio_stream_frame_size(&in->stream.common));
    if (!in->buffer) {
        ret = -ENOMEM;
        goto err_open;
    }

    if (in->requested_rate != in->config.rate) {
		LOGFUNC("%s(in->requested_rate=%d, in->config.rate=%d)", 
			           __FUNCTION__, in->requested_rate, in->config.rate);
        in->buf_provider.get_next_buffer = get_next_buffer;
        in->buf_provider.release_buffer = release_buffer;
        ret = create_resampler(in->config.rate,
                               in->requested_rate,
                               in->config.channels,
                               RESAMPLER_QUALITY_DEFAULT,
                               &in->buf_provider,
                               &in->resampler);

        if (ret != 0) {
            ret = -EINVAL;
            goto err_open;
        }
    }

    in->dev = ladev;
    in->standby = 1;
    in->device = devices;

    *stream_in = &in->stream;
    return 0;

err_open:
    if (in->resampler)
        release_resampler(in->resampler);

    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
                                   struct audio_stream_in *stream)
{
    struct aml_stream_in *in = (struct aml_stream_in *)stream;

    LOGFUNC("%s(%p, %p)", __FUNCTION__, dev, stream);
    in_standby(&stream->common);

    if (in->resampler) {
        free(in->buffer);
        release_resampler(in->resampler);
    }
	if (in->up_resampler) {
        free(in->up_resampler_buffer);
        release_resampler(in->up_resampler);
	}
    if (in->proc_buf)
        free(in->proc_buf);
    if (in->ref_buf)
        free(in->ref_buf);

    free(stream);

    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    LOGFUNC("%s(%p, %d)", __FUNCTION__, device, fd);
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct aml_audio_device *adev = (struct aml_audio_device *)device;

    LOGFUNC("%s(%p)", __FUNCTION__, device);
    /* RIL */
    //ril_close(&adev->ril);

    mixer_close(adev->mixer);

    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *dev)
{
    LOGFUNC("%s", __FUNCTION__);
    return (/* OUT */
            AUDIO_DEVICE_OUT_EARPIECE |
            AUDIO_DEVICE_OUT_SPEAKER |
            AUDIO_DEVICE_OUT_WIRED_HEADSET |
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE |
            AUDIO_DEVICE_OUT_AUX_DIGITAL |
            AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET |
            AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET |
            AUDIO_DEVICE_OUT_ALL_SCO |
            AUDIO_DEVICE_OUT_DEFAULT |
            /* IN */
            AUDIO_DEVICE_IN_COMMUNICATION |
            AUDIO_DEVICE_IN_AMBIENT |
            AUDIO_DEVICE_IN_BUILTIN_MIC |
            AUDIO_DEVICE_IN_WIRED_HEADSET |
            AUDIO_DEVICE_IN_AUX_DIGITAL |
            AUDIO_DEVICE_IN_BACK_MIC |
            AUDIO_DEVICE_IN_ALL_SCO |
            AUDIO_DEVICE_IN_DEFAULT);
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    struct aml_audio_device *adev;
    int ret;
    LOGFUNC("%s(%p, %s, %p)", __FUNCTION__, module, name, device);
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct aml_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_1_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;

    adev->hw_device.get_supported_devices = adev_get_supported_devices;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->mixer = mixer_open(0);
    if (!adev->mixer) {
        free(adev);
        ALOGE("Unable to open the mixer, aborting.");
        return -EINVAL;
    }

#if 0
{
    struct mixer *mixer = adev->mixer;
    struct mixer_ctl *ctl;
    unsigned int i, j;
    char name[128];

    ALOGE("ctl_name...   adev_open");

    for(i = 0 ; i < mixer_get_num_ctls(mixer) ; i++) {
        ctl = mixer_get_ctl(mixer, i);
        if (ctl != NULL) {
            j = mixer_ctl_get_name(ctl, name, 128);
            ALOGE("ctl_name[%02d]: %s, type : %s", i, name, mixer_ctl_get_type_string(ctl));
            if(mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_ENUM)
            {
                for(j=0; j<mixer_ctl_get_num_enums(ctl); j++)
                {
                    mixer_ctl_get_enum_string(ctl, j, name, 128);
                    ALOGE("\t ENUM(%d) : %s", j, name);
                }
            }
            
        }
        else 
            ALOGE("ctl_name[%02d]: can not get control pointer", i);
    }
}
#endif

    /* Set the default route before the PCM stream is opened */
    adev->mode = AUDIO_MODE_NORMAL;
    adev->devices = AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_IN_BUILTIN_MIC;
    select_output_device(adev);

    *device = &adev->hw_device.common;
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "aml audio HW HAL",
        .author = "amlogic, Corp.",
        .methods = &hal_module_methods,
    },
};
