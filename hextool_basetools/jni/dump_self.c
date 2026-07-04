#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define MAX_LINE 512
#define MAX_SIZE_50MB (50 * 1024 * 1024)   // 50 MiB

// 安全写入文件：从指定内存地址读取数据并写入文件
static int dump_region(unsigned long start, size_t size, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Error: cannot create file '%s': %s\n", filename, strerror(errno));
        return -1;
    }

    // 直接从内存地址写入文件
    size_t written = fwrite((const void*)start, 1, size, fp);
    if (written != size) {
        fprintf(stderr, "Warning: only wrote %zu of %zu bytes from 0x%lx\n",
                written, size, start);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int main(void) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        perror("fopen /proc/self/maps");
        return 1;
    }

    char line[MAX_LINE];
    int region_count = 0;          // 只统计我们处理过的（可读的）

    while (fgets(line, sizeof(line), maps)) {
        unsigned long start, end;
        char perms[5] = {0};       // 例如 r-xp，最多4+1
        // 解析格式: 地址-地址 权限 偏移 设备 inode 路径名
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) {
            // 解析失败，跳过该行
            continue;
        }

        // 检查是否可读（第一个字符必须是 'r'）
        if (perms[0] != 'r')
            continue;

        size_t len = end - start;
        region_count++;   // 这是一个可读区域，无论大小都计数一次

        // 打印基本信息：次数、起始地址、长度
        printf("%d 0x%lx %zu ", region_count, start, len);

        if (len > MAX_SIZE_50MB) {
            // 大于50MB，跳过，并给出原因
            printf("SKIPPED (size > 50MB) SKIPPED\n");
            continue;
        }

        // 生成文件名，这里使用序号，如 dump_1.bin
        char filename[64];
        snprintf(filename, sizeof(filename), "dump_%d.bin", region_count);

        // 执行转储
        if (dump_region(start, len, filename) == 0) {
            printf("%s\n", filename);
        } else {
            // 转储失败，打印失败信息（但仍算一次尝试）
            printf("FAILED (%s) FAILED\n", filename);
        }
    }

    fclose(maps);
    return 0;
}