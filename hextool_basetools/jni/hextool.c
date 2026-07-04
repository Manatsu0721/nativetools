#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#define BUFFER_SIZE 4096

// 模式枚举
typedef enum {
    MODE_SWAP_NIBBLES = 1,   // 高低四位交换
    MODE_BIT_REVERSE,        // 位反转
    MODE_ROT_LEFT_3,         // 循环左移3位
    MODE_ROT_RIGHT_4,        // 循环右移4位
    MODE_XOR_0x55,           // 异或0x55
    MODE_NOT,                // 按位取反
    MODE_ADD_0x0F,           // 加0x0F (模256)
    MODE_MUL_2               // 乘2 (模256)
} transform_mode_t;

// 模式对应的变换函数指针
typedef unsigned char (*transform_func)(unsigned char);

// 高低四位交换
static unsigned char swap_nibbles(unsigned char c) {
    return ((c & 0x0F) << 4) | ((c & 0xF0) >> 4);
}

// 位反转
static unsigned char bit_reverse(unsigned char c) {
    c = ((c & 0xF0) >> 4) | ((c & 0x0F) << 4); // 交换高低四位
    c = ((c & 0xCC) >> 2) | ((c & 0x33) << 2); // 交换每两位中的高2位和低2位
    c = ((c & 0xAA) >> 1) | ((c & 0x55) << 1); // 交换相邻位
    return c;
}

// 循环左移3位
static unsigned char rot_left_3(unsigned char c) {
    return (c << 3) | (c >> 5);
}

// 循环右移4位
static unsigned char rot_right_4(unsigned char c) {
    return (c >> 4) | (c << 4);
}

// 异或0x55
static unsigned char xor_0x55(unsigned char c) {
    return c ^ 0x55;
}

// 按位取反
static unsigned char not_byte(unsigned char c) {
    return ~c;
}

// 加0x0F
static unsigned char add_0x0f(unsigned char c) {
    return c + 0x0F;
}

// 乘2
static unsigned char mul_2(unsigned char c) {
    return c * 2;
}

// 根据模式获取变换函数
static transform_func get_transform(transform_mode_t mode) {
    switch (mode) {
        case MODE_SWAP_NIBBLES: return swap_nibbles;
        case MODE_BIT_REVERSE:  return bit_reverse;
        case MODE_ROT_LEFT_3:   return rot_left_3;
        case MODE_ROT_RIGHT_4:  return rot_right_4;
        case MODE_XOR_0x55:     return xor_0x55;
        case MODE_NOT:          return not_byte;
        case MODE_ADD_0x0F:     return add_0x0f;
        case MODE_MUL_2:        return mul_2;
        default:                return NULL;
    }
}

// 打印帮助信息
static void print_help(const char *progname) {
    printf("用法: %s [选项] <文件名>\n", progname);
    printf("对文件每个字节进行有规律的十六进制变换，原地修改文件。\n\n");
    printf("选项:\n");
    printf("  -m, --mode <数字>   选择变换模式 (1-8, 默认1)\n");
    printf("  -h, --help          显示此帮助信息\n\n");
    printf("模式列表:\n");
    printf("  1: 高低四位交换 (原hextool1功能)\n");
    printf("     例如: 0x02 -> 0x20, 0xdf -> 0xfd\n");
    printf("  2: 位反转 (bitwise reverse)\n");
    printf("     例如: 0xb2 (10110010) -> 0x4d (01001101)\n");
    printf("  3: 循环左移3位\n");
    printf("     例如: 0xf0 (11110000) -> 0x87 (10000111)\n");
    printf("  4: 循环右移4位\n");
    printf("     例如: 0xab (10101011) -> 0xba (10111010)\n");
    printf("  5: 异或 0x55\n");
    printf("     例如: 0xaa -> 0xff, 0x00 -> 0x55\n");
    printf("  6: 按位取反\n");
    printf("     例如: 0x00 -> 0xff, 0x55 -> 0xaa\n");
    printf("  7: 加 0x0F (模256)\n");
    printf("     例如: 0xf0 -> 0xff, 0xfc -> 0x0b\n");
    printf("  8: 乘2 (模256)\n");
    printf("     例如: 0x80 -> 0x00, 0x7f -> 0xfe\n\n");
    printf("示例:\n");
    printf("  %s test.bmp                  # 使用模式1(高低四位交换)\n", progname);
    printf("  %s -m 2 test.bmp             # 位反转\n", progname);
    printf("  %s --mode 5 data.bin         # 异或0x55\n", progname);
}

int main(int argc, char *argv[]) {
    int opt;
    transform_mode_t mode = MODE_SWAP_NIBBLES; // 默认模式1
    const char *filename = NULL;

    // 长选项定义
    static struct option long_options[] = {
        {"mode", required_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    // 解析命令行参数
    while ((opt = getopt_long(argc, argv, "m:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'm':
                {
                    int m = atoi(optarg);
                    if (m >= 1 && m <= 8) {
                        mode = (transform_mode_t)m;
                    } else {
                        fprintf(stderr, "错误: 无效的模式 '%s'，请输入1-8之间的数字。\n", optarg);
                        print_help(argv[0]);
                        return EXIT_FAILURE;
                    }
                }
                break;
            case 'h':
                print_help(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_help(argv[0]);
                return EXIT_FAILURE;
        }
    }

    // 获取文件名参数
    if (optind < argc) {
        filename = argv[optind];
    } else {
        fprintf(stderr, "错误: 未指定文件名。\n");
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    // 获取变换函数
    transform_func func = get_transform(mode);
    if (func == NULL) {
        fprintf(stderr, "内部错误: 无效的变换模式。\n");
        return EXIT_FAILURE;
    }

    // 打开文件
    FILE *file = fopen(filename, "rb+");
    if (file == NULL) {
        perror("打开文件失败");
        return EXIT_FAILURE;
    }

    unsigned char buffer[BUFFER_SIZE];
    long pos;
    size_t bytes_read;

    while (1) {
        pos = ftell(file);
        if (pos == -1) {
            perror("获取文件位置失败");
            fclose(file);
            return EXIT_FAILURE;
        }

        bytes_read = fread(buffer, 1, BUFFER_SIZE, file);
        if (bytes_read == 0) {
            if (feof(file)) break;
            perror("读取文件失败");
            fclose(file);
            return EXIT_FAILURE;
        }

        // 应用变换
        for (size_t i = 0; i < bytes_read; ++i) {
            buffer[i] = func(buffer[i]);
        }

        if (fseek(file, pos, SEEK_SET) != 0) {
            perror("定位文件位置失败");
            fclose(file);
            return EXIT_FAILURE;
        }

        size_t bytes_written = fwrite(buffer, 1, bytes_read, file);
        if (bytes_written != bytes_read) {
            perror("写入文件失败");
            fclose(file);
            return EXIT_FAILURE;
        }

        if (fflush(file) == EOF) {
            perror("刷新文件缓冲区失败");
            fclose(file);
            return EXIT_FAILURE;
        }

        if (fseek(file, pos + bytes_read, SEEK_SET) != 0) {
            perror("定位到写入末尾失败");
            fclose(file);
            return EXIT_FAILURE;
        }
    }

    fclose(file);
    printf("转换完成 (模式 %d): %s\n", mode, filename);
    return EXIT_SUCCESS;
}