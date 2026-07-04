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

#include <vector>
#include <string>
#include <optional>
#include <fstream>
#include <memory>
#include <algorithm>

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

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

// ==================== RAII 资源管理类（无异常） ====================

// 1. DRM 设备文件描述符
class DrmFd {
public:
    DrmFd() {
        for (int i = 0; i < 8; ++i) {
            char path[64];
            snprintf(path, sizeof(path), DRM_DEVICE_TEMPLATE, i);
            fd_ = open(path, O_RDWR);
            if (fd_ >= 0) break;
        }
    }
    ~DrmFd() {
        if (fd_ >= 0) close(fd_);
    }
    DrmFd(const DrmFd&) = delete;
    DrmFd& operator=(const DrmFd&) = delete;
    DrmFd(DrmFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    DrmFd& operator=(DrmFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// 2. Dumb Buffer（使用工厂函数，返回 optional）
class DumbBuffer {
public:
    // 移动语义允许
    DumbBuffer(DumbBuffer&& other) noexcept
        : fd_(other.fd_), handle_(other.handle_), pitch_(other.pitch_),
          size_(other.size_), fb_id_(other.fb_id_), map_(other.map_),
          width_(other.width_), height_(other.height_), bpp_(other.bpp_) {
        other.handle_ = 0;
        other.fb_id_ = 0;
        other.map_ = MAP_FAILED;
        other.size_ = 0;
    }
    DumbBuffer& operator=(DumbBuffer&& other) noexcept {
        if (this != &other) {
            this->~DumbBuffer();
            new (this) DumbBuffer(std::move(other));
        }
        return *this;
    }
    ~DumbBuffer() {
        cleanup();
    }

    // 静态工厂函数
    static std::optional<DumbBuffer> create(int fd, uint32_t width, uint32_t height, uint32_t bpp) {
        DumbBuffer buf;
        buf.fd_ = fd;
        buf.width_ = width;
        buf.height_ = height;
        buf.bpp_ = bpp;

        // 创建 dumb
        struct drm_mode_create_dumb create = {};
        create.width = width;
        create.height = height;
        create.bpp = bpp;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
            perror("DRM_IOCTL_MODE_CREATE_DUMB");
            return std::nullopt;
        }
        buf.handle_ = create.handle;
        buf.pitch_ = create.pitch;
        buf.size_ = create.size;

        // 映射
        struct drm_mode_map_dumb map = {};
        map.handle = buf.handle_;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
            perror("DRM_IOCTL_MODE_MAP_DUMB");
            struct drm_mode_destroy_dumb destroy = {buf.handle_};
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            return std::nullopt;
        }
        buf.map_ = mmap(nullptr, buf.size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        if (buf.map_ == MAP_FAILED) {
            perror("mmap");
            struct drm_mode_destroy_dumb destroy = {buf.handle_};
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            return std::nullopt;
        }

        // 添加 FB
        struct drm_mode_fb_cmd fb = {};
        fb.width = width;
        fb.height = height;
        fb.pitch = buf.pitch_;
        fb.bpp = bpp;
        fb.depth = (bpp == 32) ? 24 : bpp;
        fb.handle = buf.handle_;
        if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) != 0) {
            perror("DRM_IOCTL_MODE_ADDFB");
            munmap(buf.map_, buf.size_);
            struct drm_mode_destroy_dumb destroy = {buf.handle_};
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
            return std::nullopt;
        }
        buf.fb_id_ = fb.fb_id;
        return buf;
    }

    // 禁止拷贝
    DumbBuffer(const DumbBuffer&) = delete;
    DumbBuffer& operator=(const DumbBuffer&) = delete;

    uint32_t fb_id() const { return fb_id_; }
    uint32_t pitch() const { return pitch_; }
    void* data() const { return map_; }
    uint32_t size() const { return size_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    DumbBuffer() = default; // 仅工厂使用

    void cleanup() {
        if (fb_id_) {
            ioctl(fd_, DRM_IOCTL_MODE_RMFB, &fb_id_);
        }
        if (map_ && map_ != MAP_FAILED) {
            munmap(map_, size_);
        }
        if (handle_) {
            struct drm_mode_destroy_dumb destroy = {handle_};
            ioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
    }

    int fd_ = -1;
    uint32_t handle_ = 0;
    uint32_t pitch_ = 0;
    uint32_t size_ = 0;
    uint32_t fb_id_ = 0;
    void* map_ = MAP_FAILED;
    uint32_t width_ = 0, height_ = 0, bpp_ = 0;
};

// 3. CRTC 保存（不再需要额外成员变量）
struct CrtcSaver {
    drm_mode_crtc orig;
    bool saved = false;

    static CrtcSaver save(int fd, uint32_t crtc_id) {
        CrtcSaver saver;
        saver.orig.crtc_id = crtc_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &saver.orig) == 0) {
            saver.saved = true;
        }
        return saver;
    }
};

// ==================== BMP 加载 ====================

struct Bitmap {
    int width;
    int height;
    std::vector<uint8_t> data; // 32-bit BGRX
};

static std::optional<Bitmap> load_bmp(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        perror("open bmp");
        return std::nullopt;
    }

    BMPHeader hdr;
    if (!file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        perror("read header");
        return std::nullopt;
    }

    if (hdr.type != 0x4D42) {
        fprintf(stderr, "Not a BMP file\n");
        return std::nullopt;
    }
    if (hdr.bitsPerPixel != 24 && hdr.bitsPerPixel != 32) {
        fprintf(stderr, "Only 24/32-bit BMP supported, got %d\n", hdr.bitsPerPixel);
        return std::nullopt;
    }
    if (hdr.compression != 0) {
        fprintf(stderr, "Compressed BMP not supported\n");
        return std::nullopt;
    }

    int width = hdr.width;
    int height = hdr.height;
    int bpp = hdr.bitsPerPixel;
    int src_row_size = ((width * bpp + 31) / 32) * 4;
    int dst_row_size = width * 4;

    std::vector<uint8_t> src_data(src_row_size * height);
    file.seekg(hdr.offset, std::ios::beg);
    for (int y = 0; y < height; ++y) {
        int src_y = height - 1 - y;
        if (!file.read(reinterpret_cast<char*>(src_data.data() + src_y * src_row_size), src_row_size)) {
            perror("read pixels");
            return std::nullopt;
        }
    }

    Bitmap bmp;
    bmp.width = width;
    bmp.height = height;
    bmp.data.resize(dst_row_size * height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* src = src_data.data() + y * src_row_size;
        uint8_t* dst = bmp.data.data() + y * dst_row_size;
        for (int x = 0; x < width; ++x) {
            if (bpp == 24) {
                dst[x*4 + 0] = src[x*3 + 0]; // B
                dst[x*4 + 1] = src[x*3 + 1]; // G
                dst[x*4 + 2] = src[x*3 + 2]; // R
                dst[x*4 + 3] = 255;
            } else {
                dst[x*4 + 0] = src[x*4 + 0];
                dst[x*4 + 1] = src[x*4 + 1];
                dst[x*4 + 2] = src[x*4 + 2];
                dst[x*4 + 3] = src[x*4 + 3];
            }
        }
    }
    return bmp;
}

// ==================== 辅助函数 ====================

static std::optional<std::tuple<uint32_t, drm_mode_modeinfo, uint32_t>>
find_connected_connector(int fd, const std::vector<uint32_t>& connector_ids,
                         const std::vector<uint32_t>& crtc_ids) {
    for (uint32_t cid : connector_ids) {
        struct drm_mode_get_connector get_conn = {};
        get_conn.connector_id = cid;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &get_conn) != 0)
            continue;
        if (get_conn.connection != DRM_MODE_CONNECTED || get_conn.count_modes == 0)
            continue;

        std::vector<drm_mode_modeinfo> modes(get_conn.count_modes);
        std::vector<uint32_t> props(get_conn.count_props);
        std::vector<uint64_t> prop_vals(get_conn.count_props);
        std::vector<uint32_t> encoders(get_conn.count_encoders);
        get_conn.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
        get_conn.props_ptr = reinterpret_cast<uint64_t>(props.data());
        get_conn.prop_values_ptr = reinterpret_cast<uint64_t>(prop_vals.data());
        get_conn.encoders_ptr = reinterpret_cast<uint64_t>(encoders.data());

        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &get_conn) == 0) {
            uint32_t crtc_id = crtc_ids.empty() ? 0 : crtc_ids[0];
            return std::make_tuple(cid, modes[0], crtc_id);
        }
    }
    return std::nullopt;
}

// ==================== main ====================

int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char *bmp_path = (argc >= 2) ? argv[1] : "./image.bmp";

    auto bmp_opt = load_bmp(bmp_path);
    if (!bmp_opt) {
        fprintf(stderr, "Failed to load BMP\n");
        return 1;
    }
    const Bitmap& bmp = *bmp_opt;
    printf("Loaded BMP: %dx%d, 32-bit converted\n", bmp.width, bmp.height);

    DrmFd drm_fd;
    if (!drm_fd) {
        fprintf(stderr, "Failed to open DRM device\n");
        return 1;
    }
    int fd = drm_fd.get();

    struct drm_mode_card_res res = {};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        perror("DRM_IOCTL_MODE_GETRESOURCES");
        return 1;
    }
    std::vector<uint32_t> crtc_ids(res.count_crtcs);
    std::vector<uint32_t> connector_ids(res.count_connectors);
    std::vector<uint32_t> encoder_ids(res.count_encoders);
    std::vector<uint32_t> fb_ids(res.count_fbs);
    res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtc_ids.data());
    res.connector_id_ptr = reinterpret_cast<uint64_t>(connector_ids.data());
    res.encoder_id_ptr = reinterpret_cast<uint64_t>(encoder_ids.data());
    res.fb_id_ptr = reinterpret_cast<uint64_t>(fb_ids.data());
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        perror("DRM_IOCTL_MODE_GETRESOURCES second");
        return 1;
    }

    auto conn_info = find_connected_connector(fd, connector_ids, crtc_ids);
    if (!conn_info) {
        fprintf(stderr, "No connected connector with modes\n");
        return 1;
    }
    auto [conn_id, mode, crtc_id] = *conn_info;
    printf("Using connector %u, mode %dx%d\n", conn_id, mode.hdisplay, mode.vdisplay);

    if (bmp.width != mode.hdisplay || bmp.height != mode.vdisplay) {
        fprintf(stderr, "Warning: image size (%dx%d) != screen mode (%dx%d)\n",
                bmp.width, bmp.height, mode.hdisplay, mode.vdisplay);
    }

    auto dumb_opt = DumbBuffer::create(fd, mode.hdisplay, mode.vdisplay, 32);
    if (!dumb_opt) {
        fprintf(stderr, "Failed to create dumb buffer\n");
        return 1;
    }
    DumbBuffer dumb = std::move(*dumb_opt);
    uint32_t fb_id = dumb.fb_id();
    uint32_t pitch = dumb.pitch();

    int bmp_row_size = bmp.width * 4;
    uint8_t* fb_ptr = static_cast<uint8_t*>(dumb.data());
    for (int y = 0; y < bmp.height && y < (int)mode.vdisplay; ++y) {
        uint8_t* dst = fb_ptr + y * pitch;
        const uint8_t* src = bmp.data.data() + y * bmp_row_size;
        int copy_width = std::min(bmp.width, (int)mode.hdisplay) * 4;
        memcpy(dst, src, copy_width);
    }

    CrtcSaver crtc_saver = CrtcSaver::save(fd, crtc_id);
    if (!crtc_saver.saved) {
        fprintf(stderr, "Warning: failed to get original CRTC\n");
    }
    drm_mode_crtc original_crtc = crtc_saver.orig;

    struct drm_mode_crtc set_crtc = {};
    set_crtc.crtc_id = crtc_id;
    set_crtc.fb_id = fb_id;
    set_crtc.x = 0;
    set_crtc.y = 0;
    set_crtc.mode = mode;
    set_crtc.mode_valid = 1;
    set_crtc.set_connectors_ptr = reinterpret_cast<uint64_t>(&conn_id);
    set_crtc.count_connectors = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set_crtc) != 0) {
        perror("DRM_IOCTL_MODE_SETCRTC");
        return 1;   // dumb 析构会自动清理
    }

    printf("Displaying BMP image. Press Ctrl+C to exit.\n");
    while (g_running) {
        pause();
    }

    if (original_crtc.mode_valid) {
        original_crtc.set_connectors_ptr = reinterpret_cast<uint64_t>(&conn_id);
        original_crtc.count_connectors = 1;
        ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &original_crtc);
    }

    return 0;
}