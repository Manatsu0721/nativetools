LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := key_listener
LOCAL_SRC_FILES := keylistener.c

LOCAL_CFLAGS	:= -Wall -Wextra
# LOCAL_CPPFLAGS	:= 
# LOCAL_LDLIBS    :=

include $(BUILD_EXECUTABLE)