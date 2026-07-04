LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := time_control
LOCAL_SRC_FILES := timectl.c

LOCAL_CFLAGS	:= -Wall -Wextra
# LOCAL_CPPFLAGS	:= 
# LOCAL_LDLIBS    :=

include $(BUILD_EXECUTABLE)