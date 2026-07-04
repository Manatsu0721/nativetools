LOCAL_PATH := $(call my-dir)

SRC_FILES := hextool.c dump_self.c _test.c create_bmp.c randomhex2ascii_16Byte.c print_all_hex.c

$(foreach file,$(SRC_FILES), \
    $(eval include $(CLEAR_VARS)) \
    $(eval LOCAL_MODULE := $(basename $(file))) \
    $(eval LOCAL_SRC_FILES := $(file)) \
    $(eval LOCAL_CFLAGS := -O2 -fPIE -Wall) \
    $(eval LOCAL_LDFLAGS := -fPIE -pie) \
    $(eval include $(BUILD_EXECUTABLE)) \
)