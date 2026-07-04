LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := drm_displaybmp
LOCAL_SRC_FILES := drm_useADDFB.cpp

LOCAL_CFLAGS := -O2 -fvisibility=hidden -fdata-sections -ffunction-sections -fPIE -Wall -Wextra
LOCAL_CPPFLAGS := -std=c++11
LOCAL_LDFLAGS := -static \
                 -Wl,--gc-sections \
                 -Wl,-z,max-page-size=4096 \
                 -Wl,-z,common-page-size=4096 \
                 -Wl,--build-id=md5

include $(BUILD_EXECUTABLE)
