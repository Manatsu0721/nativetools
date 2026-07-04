LOCAL_PATH := $(call my-dir)

# 根据目标 ABI 选择对应的静态库文件
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
    SELINUX_LIB := libselinux_32.a
    SEPOL_LIB   := libsepol_32.a
else ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
    SELINUX_LIB := libselinux.a
    SEPOL_LIB   := libsepol.a
else
    # 若需支持其他 ABI（如 x86、x86_64），可在此增加条件，并准备对应库文件
    $(error Unsupported ABI: $(TARGET_ARCH_ABI))
endif

# libselinux 预编译库
include $(CLEAR_VARS)
LOCAL_MODULE := libselinux-prebuilt
LOCAL_SRC_FILES := selinux_prebuilt/libselinux/src/$(SELINUX_LIB)
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/selinux_prebuilt/libselinux/include
include $(PREBUILT_STATIC_LIBRARY)

# libsepol 预编译库
include $(CLEAR_VARS)
LOCAL_MODULE := libsepol-prebuilt
LOCAL_SRC_FILES := selinux_prebuilt/libsepol/src/$(SEPOL_LIB)
include $(PREBUILT_STATIC_LIBRARY)

# 主可执行文件
include $(CLEAR_VARS)
LOCAL_MODULE := uid-gid-con-cap_switcher
LOCAL_SRC_FILES := switcher.c
LOCAL_STATIC_LIBRARIES := libselinux-prebuilt libsepol-prebuilt
LOCAL_CFLAGS := -O2 -fvisibility=hidden -fdata-sections -ffunction-sections -fPIE
LOCAL_LDFLAGS := -static \
                 -Wl,--gc-sections \
                 -Wl,-z,max-page-size=4096 \
                 -Wl,-z,common-page-size=4096 \
                 -Wl,--no-undefined \
                 -Wl,--build-id=md5 \
                 -Wl,--allow-multiple-definition
include $(BUILD_EXECUTABLE)
