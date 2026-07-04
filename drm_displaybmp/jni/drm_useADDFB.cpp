#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <signal.h>


#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

#define DRM_DEVICE_TEMPLATE "/dev/dri/card%d"

#pragma pack(push, 1)
struct BMPHeader {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;
    uint32_t dibSize;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bitsPerPixel;
    uint32_t compression;
    uint32_t imageSize;
    int32_t  xPixelsPerMeter;
    int32_t  yPixelsPerMeter;
    uint32_t colorsUsed;
    uint32_t colorsImportant;
};
#pragma pack(pop)

static int open_drm_device() {
    for (int i = 0; i < 8; ++i) {
        char path[64];
        snprintf(path, sizeof(path), DRM_DEVICE_TEMPLATE, i);
        int fd = open(path, O_RDWR);
        if (fd >= 0) return fd;
    }
    return -1;
}

static int create_dumb_buffer(int fd, uint32_t width, uint32_t height, uint32_t bpp,
                              uint32_t *handle, uint32_t *pitch, uint32_t *size) {
    struct drm_mode_create_dumb create = {};
    create.width = width;
    create.height = height;
    create.bpp = bpp;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) return -1;
    *handle = create.handle;
    *pitch = create.pitch;
    *size = create.size;
    return 0;
}

static void *map_dumb_buffer(int fd, uint32_t handle, uint32_t size) {
    struct drm_mode_map_dumb map = {};
    map.handle = handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) return MAP_FAILED;
    return mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
}

// 加载 BMP，返回 32 位像素数据（BGRX 格式，X=0），调用者 free
static uint8_t* load_bmp(const char *path, int *out_width, int *out_height) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return nullptr;
    }
    BMPHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        perror("fread header");
        fclose(f);
        return nullptr;
    }
    if (hdr.type != 0x4D42) {
        fprintf(stderr, "Not a BMP file\n");
        fclose(f);
        return nullptr;
    }
    if (hdr.bitsPerPixel != 24 && hdr.bitsPerPixel != 32) {
        fprintf(stderr, "Only 24/32-bit BMP supported, got %d\n", hdr.bitsPerPixel);
        fclose(f);
        return nullptr;
    }
    if (hdr.compression != 0) {
        fprintf(stderr, "Compressed BMP not supported\n");
        fclose(f);
        return nullptr;
    }

    int width = hdr.width;
    int height = hdr.height;
    int bpp = hdr.bitsPerPixel;
    int src_row_size = ((width * bpp + 31) / 32) * 4; // 源每行字节数（对齐）
    int dst_row_size = width * 4;                    // 目标每行字节数（32位）

    uint8_t *src_data = (uint8_t*)malloc(src_row_size * height);
    if (!src_data) {
        fclose(f);
        return nullptr;
    }
    fseek(f, hdr.offset, SEEK_SET);
    // 读入原始 BMP 数据（底部优先）
    for (int y = 0; y < height; ++y) {
        int src_y = (height - 1 - y);
        if (fread(src_data + src_y * src_row_size, src_row_size, 1, f) != 1) {
            perror("fread pixels");
            free(src_data);
            fclose(f);
            return nullptr;
        }
    }
    fclose(f);

    // 转换为 32 位 BGRX（X 填 0，Alpha 后面单独设）
    uint8_t *dst_data = (uint8_t*)malloc(dst_row_size * height);
    if (!dst_data) {
        free(src_data);
        return nullptr;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t *src = src_data + y * src_row_size;
        uint8_t *dst = dst_data + y * dst_row_size;
        for (int x = 0; x < width; ++x) {
            if (bpp == 24) {
                dst[x*4 + 0] = src[x*3 + 0]; // B
                dst[x*4 + 1] = src[x*3 + 1]; // G
                dst[x*4 + 2] = src[x*3 + 2]; // R
                dst[x*4 + 3] = 255;          // A
            } else { // 32-bit
                dst[x*4 + 0] = src[x*4 + 0]; // B
                dst[x*4 + 1] = src[x*4 + 1]; // G
                dst[x*4 + 2] = src[x*4 + 2]; // R
                dst[x*4 + 3] = src[x*4 + 3]; // A (若原图有Alpha，保留)
            }
        }
    }
    free(src_data);
    *out_width = width;
    *out_height = height;
    return dst_data;
}

// 将 32 位 BMP 数据（BGRX）复制到 dumb buffer
static void copy_bmp_to_fb(uint8_t *fb, uint32_t fb_pitch, int width, int height,
                           uint8_t *bmp_data, int bmp_row_size) {
    for (int y = 0; y < height; ++y) {
        uint8_t *dst = fb + y * fb_pitch;
        uint8_t *src = bmp_data + y * bmp_row_size;
        memcpy(dst, src, width * 4); // 一行直接复制
    }
}

static volatile int running = 1;  // 是否正在运行

void sig_handler(int) {
    running = 0; // 收到信号时置标志位
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    const char *bmp_path = "./image.bmp";
    if (argc >= 2) bmp_path = argv[1];

    int img_w, img_h;
    uint8_t *bmp_pixels = load_bmp(bmp_path, &img_w, &img_h);
    if (!bmp_pixels) return 1;
    printf("Loaded BMP: %dx%d, 32-bit converted\n", img_w, img_h);

    int fd = open_drm_device();
    if (fd < 0) {
        fprintf(stderr, "Failed to open DRM device\n");
        free(bmp_pixels);
        return 1;
    }

    // 获取 DRM 资源
    struct drm_mode_card_res res = {};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        perror("DRM_IOCTL_MODE_GETRESOURCES");
        close(fd); free(bmp_pixels);
        return 1;
    }
    uint32_t *crtc_ids = new uint32_t[res.count_crtcs];
    uint32_t *connector_ids = new uint32_t[res.count_connectors];
    uint32_t *encoder_ids = new uint32_t[res.count_encoders];
    uint32_t *fb_ids = new uint32_t[res.count_fbs];
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connector_ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encoder_ids;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fb_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        perror("DRM_IOCTL_MODE_GETRESOURCES second");
        delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
        close(fd); free(bmp_pixels);
        return 1;
    }

    // 查找已连接的 connector
    uint32_t conn_id = 0;
    struct drm_mode_modeinfo mode = {};
    for (int i = 0; i < res.count_connectors; ++i) {
        uint32_t cid = connector_ids[i];
        struct drm_mode_get_connector get_conn = {};
        get_conn.connector_id = cid;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &get_conn) != 0) continue;
        if (get_conn.connection != DRM_MODE_CONNECTED || get_conn.count_modes == 0) continue;

        struct drm_mode_modeinfo *modes = new drm_mode_modeinfo[get_conn.count_modes];
        uint32_t *props = new uint32_t[get_conn.count_props];
        uint64_t *prop_vals = new uint64_t[get_conn.count_props];
        uint32_t *encs = new uint32_t[get_conn.count_encoders];
        get_conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        get_conn.props_ptr = (uint64_t)(uintptr_t)props;
        get_conn.prop_values_ptr = (uint64_t)(uintptr_t)prop_vals;
        get_conn.encoders_ptr = (uint64_t)(uintptr_t)encs;

        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &get_conn) == 0) {
            conn_id = cid;
            mode = modes[0];
            delete[] modes; delete[] props; delete[] prop_vals; delete[] encs;
            break;
        }
        delete[] modes; delete[] props; delete[] prop_vals; delete[] encs;
    }
    if (conn_id == 0) {
        fprintf(stderr, "No connected connector with modes\n");
        delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
        close(fd); free(bmp_pixels);
        return 1;
    }
    printf("Using connector %u, mode %dx%d\n", conn_id, mode.hdisplay, mode.vdisplay);
    uint32_t crtc_id = crtc_ids[0];

    // 检查图片尺寸是否匹配屏幕模式（不匹配时警告，但仍然复制，可能只显示一部分）
    if (img_w != mode.hdisplay || img_h != mode.vdisplay) {
        fprintf(stderr, "Warning: image size (%dx%d) != screen mode (%dx%d), will display partial/corrupted\n",
                img_w, img_h, mode.hdisplay, mode.vdisplay);
    }

    // 创建 dumb buffer
    uint32_t handle, pitch, size;
    if (create_dumb_buffer(fd, mode.hdisplay, mode.vdisplay, 32, &handle, &pitch, &size) != 0) {
        perror("create_dumb_buffer");
        delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
        close(fd); free(bmp_pixels);
        return 1;
    }
    void *fb_map = map_dumb_buffer(fd, handle, size);
    if (fb_map == MAP_FAILED) {
        perror("map_dumb_buffer");
        delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
        close(fd); free(bmp_pixels);
        return 1;
    }

    // 将 BMP 复制到 framebuffer
    int bmp_row_size = img_w * 4; // 32位每行字节数
    copy_bmp_to_fb((uint8_t*)fb_map, pitch, mode.hdisplay, mode.vdisplay,
                   bmp_pixels, bmp_row_size);

    // 添加 framebuffer (legacy ADDFB)
    struct drm_mode_fb_cmd fb_cmd = {};
    fb_cmd.width = mode.hdisplay;
    fb_cmd.height = mode.vdisplay;
    fb_cmd.pitch = pitch;
    fb_cmd.bpp = 32;
    fb_cmd.depth = 24;
    fb_cmd.handle = handle;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) != 0) {
        perror("DRM_IOCTL_MODE_ADDFB");
        munmap(fb_map, size);
        delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
        close(fd); free(bmp_pixels);
        return 1;
    }
    uint32_t fb_id = fb_cmd.fb_id;

    // 保存原 CRTC 并设置新 CRTC
    struct drm_mode_crtc original_crtc = {};
    original_crtc.crtc_id = crtc_id;
    ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &original_crtc);

    struct drm_mode_crtc set_crtc = {};
    set_crtc.crtc_id = crtc_id;
    set_crtc.fb_id = fb_id;
    set_crtc.x = 0;
    set_crtc.y = 0;
    set_crtc.mode = mode;
    set_crtc.mode_valid = 1;
    set_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
    set_crtc.count_connectors = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set_crtc) != 0) {
        perror("DRM_IOCTL_MODE_SETCRTC");
        ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
        munmap(fb_map, size);
        delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
        close(fd); free(bmp_pixels);
        return 1;
    }

    printf("Displaying BMP image. Press Ctrl+C to exit.\n");
    while(running) {
        pause(); // 保持进程，且每次醒来检查标志位
    }

    // 恢复，保存原 connector ID 和原 FB ID
    uint32_t orig_conn_id = conn_id;
    uint32_t orig_fb_id = original_crtc.fb_id;
    
    // 主循环结束，执行恢复
    if (original_crtc.mode_valid) {
        struct drm_mode_crtc restore = original_crtc;
        restore.set_connectors_ptr = (uint64_t)(uintptr_t)&orig_conn_id;
        restore.count_connectors = 1;
        restore.fb_id = orig_fb_id;           // 使用原来的 FB（可能是 0，表示关闭显示）
        // 如果原来的 FB 为 0，可能表示该 CRTC 未启用，也要保留
        ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &restore);
    }
    // 释放资源
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
    munmap(fb_map, size);
    delete[] crtc_ids; delete[] connector_ids; delete[] encoder_ids; delete[] fb_ids;
    close(fd);
    free(bmp_pixels);
    return 0;
}
