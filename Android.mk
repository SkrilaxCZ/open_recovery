LOCAL_PATH := $(call my-dir)
commands_recovery_local_path := $(LOCAL_PATH)

# Open Recovery
#================================================================================================================
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    recovery.c \
    qwerty.c \
    console.c \
    properties.c \
    bootloader.c \
    install.c \
    mounts.c \
    roots.c \
    ui.c

LOCAL_MODULE := recovery
LOCAL_FORCE_STATIC_EXECUTABLE := true

RECOVERY_API_VERSION := 3
LOCAL_CFLAGS += -DRECOVERY_API_VERSION=$(RECOVERY_API_VERSION)

LOCAL_STATIC_LIBRARIES :=

# This binary is in the recovery ramdisk, which is otherwise a copy of root.
# It gets copied there in config/Makefile.  LOCAL_MODULE_TAGS suppresses
# a (redundant) copy of the binary in /system/bin for user builds.
# TODO: Build the ramdisk image in a more principled way.

LOCAL_MODULE_TAGS := eng

LOCAL_STATIC_LIBRARIES += libmake_f2fs libext4_utils libz
LOCAL_STATIC_LIBRARIES += libminzip libunz libmincrypt
LOCAL_STATIC_LIBRARIES += libminui libpixelflinger_static libpng libcutils
LOCAL_STATIC_LIBRARIES += libstdc++ libc

LOCAL_C_INCLUDES += external/f2fs-tools/include system/extras/ext4_utils

include $(BUILD_EXECUTABLE)

include $(commands_recovery_local_path)/minui/Android.mk
include $(commands_recovery_local_path)/minelf/Android.mk
include $(commands_recovery_local_path)/minzip/Android.mk
include $(commands_recovery_local_path)/tools/Android.mk
include $(commands_recovery_local_path)/edify/Android.mk
include $(commands_recovery_local_path)/sideloader/Android.mk
include $(commands_recovery_local_path)/updater/Android.mk
include $(commands_recovery_local_path)/applypatch/Android.mk
include $(commands_recovery_local_path)/interactive/Android.mk
commands_recovery_local_path :=