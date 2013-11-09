#
# Copyright (C) 2012 The Android Open-Source Project
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
#

DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay

# ALSA (Advanced Linux Sound Architecture)
BOARD_ALSA_AUDIO := tiny
BUILD_WITH_ALSA_UTILS := false
BOARD_AUDIO_CODEC = rt5631

# Bluetooth
BOARD_HAVE_BLUETOOTH := false

# Camera
USE_CAMERA_STUB := false
BOARD_HAVE_FRONT_CAM := true

# Amlogic stuff
BUILD_WITH_AMLOGIC_PLAYER := true
BOARD_USE_DEFAULT_APPINSTALL := true
BOARD_USE_DEFAULT_HDMISWITCH := true
BOARD_USE_AML_STANDARD_RIL := true
BOARD_USES_USB_PM := true
BOARD_PROVIDES_MALI := true
TARGET_USE_AMLOGIC_MKYAFFS_TOOL := true
TARGET_AMLOGIC_MKYAFFSIMG_TOOL := mkyaffsimage4K.dat

# Sensors
BOARD_USES_SENSOR_BMA250 := true

# Wifi
WIFI_DRIVER := rtl8188eu
BOARD_WIFI_VENDOR := realtek
BOARD_WLAN_DEVICE := rtl8189es
WIFI_DRIVER_MODULE_PATH := /system/lib/8188eu.ko
WIFI_DRIVER_MODULE_NAME := 8188eu

WIFI_DRIVER_FW_PATH_AP := none
WIFI_DRIVER_FW_PATH_P2P := none
WIFI_DRIVER_FW_PATH_STA := none
WIFI_DRIVER_MODULE_ARG := "ifname=wlan0 if2name=p2p0"

WPA_SUPPLICANT_VERSION := VER_0_8_X
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
CONFIG_DRIVER_WEXT := y
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_rtl
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_rtl

#CPU
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_SMP := true
TARGET_CPU_VARIANT := cortex-a9
TARGET_ARCH_VARIANT := armv7-a-neon
ARCH_ARM_HAVE_VFP := true
TARGET_ARCH := arm
TARGET_ARCH_VARIANT_CPU := cortex-a9
TARGET_ARCH_VARIANT_FPU := neon
ARCH_ARM_HAVE_NEON := true
ARCH_ARM_HAVE_TLS_REGISTER := true
ARCH_ARM_HAVE_ARMV7A := true
TARGET_GLOBAL_CFLAGS += -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp
TARGET_GLOBAL_CPPFLAGS += -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp

TARGET_BOARD_PLATFORM := meson6
TARGET_BOOTLOADER_BOARD_NAME := softwin
TARGET_NO_BOOTLOADER := true
#TARGET_NO_KERNEL := false
TARGET_PREBUILT_KERNEL := device/jxd/s7300b/kernel
#BOARD_KERNEL_BASE := 0x80000000
#BOARD_KERNEL_CMDLINE := console=ttyS0,115200 rw init=/init loglevel=8
TARGET_NO_RADIOIMAGE := true
TARGET_SIMULATOR := false
TARGET_PROVIDES_INIT_RC := true
TARGET_RECOVERY_INITRC = device/jxd/s7300b/recovery.rc

BOARD_EGL_CFG := device/jxd/s7300b/egl.cfg
BOARD_EGL_NEEDS_LEGACY_FB := true
USE_OPENGL_RENDERER := true
ENABLE_WEBGL := true
BOARD_USE_SKIA_LCDTEXT := true

TARGET_BOOTANIMATION_PRELOAD := true

TARGET_USERIMAGES_USE_EXT4 := true
BOARD_BOOTIMAGE_PARTITION_SIZE := 8225280
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 939524096
BOARD_USERDATAIMAGE_PARTITION_SIZE := 6280970240
BOARD_FLASH_BLOCK_SIZE := 4096

TARGET_BOOTANIMATION_NAME := horizontal-1024x600

#Audio
TARGET_PROVIDES_LIBAUDIO := true
COMMON_GLOBAL_CFLAGS += -DMR0_AUDIO_BLOB -DEGL_NEEDS_FNW -DICS_CAMERA_BLOB
#-DDONT_USE_FENCE_SYNC

#CWM
RECOVERY_NAME := CWM-based Recovery by giannoug
BOARD_CUSTOM_RECOVERY_KEYMAPPING = := ../../device/jxd/s7300b/recovery_keys.c
TARGET_USE_CUSTOM_LUN_FILE_PATH := "/sys/class/android_usb/android0/f_mass_storage/lun%d/file"
BOARD_UMS_LUNFILE := "/sys/class/android_usb/android0/f_mass_storage/lun0/file"
BOARD_UMS_2ND_LUNFILE := "/sys/class/android_usb/android0/f_mass_storage/lun1/file"
TARGET_RECOVERY_FSTAB := device/jxd/s7300b/fstab.amlogic
RECOVERY_FSTAB_VERSION := 2

#TWRP
DEVICE_RESOLUTION := 1024x600
TARGET_RECOVERY_PIXEL_FORMAT := "RGBA_565"
RECOVERY_GRAPHICS_USE_LINELENGTH := true
TW_INTERNAL_STORAGE_PATH := "/emmc"
TW_INTERNAL_STORAGE_MOUNT_POINT := "emmc"
TW_EXTERNAL_STORAGE_PATH := "/sdcard"
TW_EXTERNAL_STORAGE_MOUNT_POINT := "sdcard"
TW_NO_REBOOT_BOOTLOADER := true
TW_DEFAULT_EXTERNAL_STORAGE := true
TW_FLASH_FROM_STORAGE := true
