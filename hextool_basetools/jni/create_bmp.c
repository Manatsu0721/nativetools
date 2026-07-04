#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;      // "BM" (0x4D42)
    uint32_t bfSize;      // file size
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;   // offset to pixel data (54 bytes)
} BITMAPFILEHEADER;

typedef struct {
    uint32_t biSize;          // header size (40)
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;        // must be 1
    uint16_t biBitCount;      // bits per pixel (24)
    uint32_t biCompression;   // 0 = BI_RGB
    uint32_t biSizeImage;     // image size (can be 0 for BI_RGB)
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BITMAPINFOHEADER;
#pragma pack(pop)

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr,
                "Usage: %s <width> <height> <B> <G> <R> <output.bmp>\n"
                "  B, G, R : 0-255 (blue, green, red components)\n",
                argv[0]);
        return 1;
    }

    int width  = atoi(argv[1]);
    int height = atoi(argv[2]);
    int B = atoi(argv[3]);
    int G = atoi(argv[4]);
    int R = atoi(argv[5]);
    const char *outfile = argv[6];

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: width and height must be positive.\n");
        return 1;
    }
    if (B < 0 || B > 255 || G < 0 || G > 255 || R < 0 || R > 255) {
        fprintf(stderr, "Error: color components must be 0-255.\n");
        return 1;
    }

    // 每行字节数（24位RGB，按4字节对齐）
    int row_size = (width * 3 + 3) & ~3;
    int image_size = row_size * height;

    BITMAPFILEHEADER fileHeader = {
        .bfType      = 0x4D42,   // 'BM'
        .bfSize      = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + image_size,
        .bfReserved1 = 0,
        .bfReserved2 = 0,
        .bfOffBits   = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)
    };

    BITMAPINFOHEADER infoHeader = {
        .biSize          = sizeof(BITMAPINFOHEADER),
        .biWidth         = width,
        .biHeight        = height,   // 正高度：从下到上存储
        .biPlanes        = 1,
        .biBitCount      = 24,
        .biCompression   = 0,        // BI_RGB
        .biSizeImage     = 0,        // 可设为0（BI_RGB下可忽略）
        .biXPelsPerMeter = 0,
        .biYPelsPerMeter = 0,
        .biClrUsed       = 0,
        .biClrImportant  = 0
    };

    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        perror("Cannot create output file");
        return 1;
    }

    // 写入文件头和信息头
    fwrite(&fileHeader, sizeof(fileHeader), 1, fp);
    fwrite(&infoHeader, sizeof(infoHeader), 1, fp);

    // 构造一行像素数据 (BGR顺序)
    unsigned char *row = (unsigned char *)malloc(row_size);
    if (!row) {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(fp);
        return 1;
    }
    // 填充BGR三元组
    for (int x = 0; x < width; ++x) {
        row[x*3 + 0] = (unsigned char)B;   // B
        row[x*3 + 1] = (unsigned char)G;   // G
        row[x*3 + 2] = (unsigned char)R;   // R
    }
    // 填充行末尾的padding字节 (0)
    for (int x = width * 3; x < row_size; ++x) {
        row[x] = 0;
    }

    // BMP存储顺序：从最后一行（height-1）到第一行（0）
    // 这里因为纯色，顺序不影响结果，但按标准实现
    for (int y = height - 1; y >= 0; --y) {
        fwrite(row, 1, row_size, fp);
    }

    free(row);
    fclose(fp);

    printf("Generated %s: %dx%d, BGR(%d,%d,%d)\n",
           outfile, width, height, B, G, R);
    return 0;
}