# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

$(call inherit-product, $(SRC_TARGET_DIR)/product/languages_full.mk)

DEVICE_PACKAGE_OVERLAYS := device/jxd/s7300b/overlay

# Ramdisk
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/ramdisk/mali.ko:root/boot/mali.ko \
    $(LOCAL_PATH)/ramdisk/ump.ko:root/boot/ump.ko \
    $(LOCAL_PATH)/ramdisk/init.rc:root/init.rc \
    $(LOCAL_PATH)/ramdisk/init.amlogic.rc:root/init.amlogic.rc \
    $(LOCAL_PATH)/ramdisk/ueventd.rc:root/ueventd.rc \
    $(LOCAL_PATH)/ramdisk/init:root/init \
    $(LOCAL_PATH)/fstab.amlogic:root/fstab.amlogic

# Prebuilt configs
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/asound.conf:system/etc/asound.conf \
    $(LOCAL_PATH)/configs/asound.state:system/etc/asound.state \
    $(LOCAL_PATH)/configs/media_profiles.xml:system/etc/media_profiles.xml \
    $(LOCAL_PATH)/configs/set_camera_clk.sh:system/bin/set_camera_clk.sh \
    $(LOCAL_PATH)/configs/set_display_mode.sh:system/bin/set_display_mode.sh \
    $(LOCAL_PATH)/configs/media_codecs.xml:system/etc/media_codecs.xml \
    $(LOCAL_PATH)/configs/audio_policy.conf:system/etc/audio_policy.conf
    #$(LOCAL_PATH)/configs/vold.fstab:system/etc/vold.fstab \

PRODUCT_PROPERTY_OVERRIDES += \
    ro.carrier=wifi-only

# Touch Screen
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/Vendor_222a_Product_0001.idc:system/usr/idc/Vendor_222a_Product_0001.idc \
    $(LOCAL_PATH)/configs/Vendor_0001_Product_0001.idc:system/usr/idc/Vendor_0001_Product_0001.idc

PRODUCT_COPY_FILES += $(LOCAL_PATH)/8188eu.ko:system/lib/8188eu.ko
PRODUCT_PACKAGES += wpa_supplicant.conf
PRODUCT_PACKAGES += hostapd

# Prebuilt proprietary stuff
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/proprietary/gralloc.default.so:system/lib/hw/gralloc.default.so \
    $(LOCAL_PATH)/proprietary/fbset:system/xbin/fbset

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/tablet_core_hardware.xml:system/etc/permissions/tablet_core_hardware.xml \
    frameworks/native/data/etc/android.hardware.camera.xml:system/etc/permissions/android.hardware.camera.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:system/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.location.xml:system/etc/permissions/android.hardware.location.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.software.sip.voip.xml:system/etc/permissions/android.software.sip.voip.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:system/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:system/etc/permissions/android.hardware.usb.host.xml \
    packages/wallpapers/LivePicker/android.software.live_wallpaper.xml:system/etc/permissions/android.software.live_wallpaper.xml

DEVICE_PACKAGE_OVERLAYS := device/jxd/s7300b/overlay

# AML HAL
PRODUCT_PACKAGES += \
    camera.amlogic.so \
    lights.amlogic \
    hwcomposer.amlogic \
    sensors.amlogic

# Mali GPU OpenGL libraries
PRODUCT_PACKAGES += \
    libEGL_mali.so \
    libGLESv1_CM_mali.so \
    libGLESv2_mali.so \
    libGLESv2_mali.so \
    libMali.so \
    libUMP.so

# AML RIL
PRODUCT_PACKAGES += \
    libaml-ril.so \
    init-pppd.sh \
    ip-up
#Phone \

# ALSA
PRODUCT_PACKAGES += \
    audio_policy.default \
    audio.primary.amlogic \
    alsa.default \
    acoustics.default \
    libasound \
    alsa_aplay \
    alsa_ctl \
    alsa_amixer \
    alsainit-00main \
    alsalib-alsaconf alsalib-pcmdefaultconf alsalib-cardsaliasesconf \
    libtinyalsa \
    libaudioutils \
    tinycap \
    tinyplay \
    tinymix

# AML Misc
PRODUCT_PACKAGES += \
    HdmiSwitch \
    libamplayerjni \
    libsubjni \
    amlogic.subtitle.xml \
    amlogic.libplayer.xml

# AML USB tools
PRODUCT_PACKAGES += \
    usbpower \
    usbtestpm \
    usbtestpm_mx \
    usbtestpm_mx_iddq \
    usbpower_mx_iddq

# wifi
PRODUCT_PACKAGES += \
    libnetcmdiface

# libemoji for Webkit
PRODUCT_PACKAGES += libemoji
    
PRODUCT_PROPERTY_OVERRIDES += \
    ro.secure=0 \
    ro.allow.mock.location=1 \
    ro.debuggable=1
#    service.adb.root=1 \
#    persist.sys.usb.config=adb\
#    persist.service.adb.enable=1 \

PRODUCT_PROPERTY_OVERRIDES += \
    ro.com.google.locationfeatures=1 \
    ro.setupwizard.enable_bypass=1 \
    dalvik.vm.execution-mode=int:jit \
    dalvik.vm.lockprof.threshold=500 \
    dalvik.vm.verify-bytecode=false \
    dalvik.vm.dexopt-flags=v=n,o=v \
    dalvik.vm.dexopt-data-only=1

PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.timezone=Europe/Athens \
    persist.sys.language=en \
    persist.sys.country=US \
    persist.sys.use_dithering=0 \
    persist.sys.purgeable_assets=0 \
    windowsmgr.max_events_per_sec=240 \
    view.touch_slop=2 \
    view.minimum_fling_velocity=25 \
    ro.additionalmounts=/storage/sdcard1 \
    ro.vold.switchablepair=/storage/sdcard0,/storage/sdcard1 \
    persist.sys.vold.switchexternal=0
   
PRODUCT_AAPT_CONFIG := xlarge mdpi
PRODUCT_AAPT_PREF_CONFIG := mdpi

PRODUCT_CHARACTERISTICS := tablet

PRODUCT_TAGS += dalvik.gc.type-precise

PRODUCT_BUILD_PROP_OVERRIDES += BUILD_UTC_DATE=0

$(call inherit-product, frameworks/native/build/tablet-7in-hdpi-1024-dalvik-heap.mk)
$(call inherit-product, build/target/product/full_base.mk)
$(call inherit-product-if-exists, vendor/jxd/s7300b/s7300b-vendor.mk)
