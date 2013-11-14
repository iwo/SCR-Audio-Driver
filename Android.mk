LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_STRIP_MODULE := false

LOCAL_CFLAGS := -DSCR_SDK_VERSION=$(PLATFORM_SDK_VERSION)
LOCAL_MODULE := audio.scr_primary.default
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SRC_FILES := audio_hw.c
LOCAL_SHARED_LIBRARIES := liblog libcutils libhardware
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

