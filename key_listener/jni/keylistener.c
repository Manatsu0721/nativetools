/*
 * 按键监听器 (keylistener) - 配置驱动版
 * 编译: armv7a-linux-androideabi21-clang -O2 -static -Wl,--gc-sections -o keylistener keylistener.c
 * 精简: llvm-strip --strip-all keylistener
 */
/*
 * 配置文件格式例子：
 * /dev/input/event1   0x0072        press      ./task.sh
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/input.h>

/* 配置常量 */
#define CONFIG_FILE "./key_config.txt"   // 配置文件路径
#define MAX_CONFIGS 64               // 最大规则数
#define MAX_DEVICES 16               // 最大设备数
#define MAX_DEVICE_PATH 128          // 设备路径最大长度
#define MAX_SCRIPT_PATH 256          // 脚本路径最大长度
#define POLL_TIMEOUT_MS 100          // poll 超时(ms)，用于检测长按时钟

/* 触发方式枚举 */
typedef enum {
    TRIGGER_PRESS,
    TRIGGER_RELEASE,
    TRIGGER_LONG_PRESS
} TriggerType;

/* 一条按键规则 */
typedef struct {
    char device_path[MAX_DEVICE_PATH];
    int keycode;
    TriggerType trigger;
    char script_path[MAX_SCRIPT_PATH];
    int long_press_ms;               // 仅用于 LONG_PRESS
} KeyConfig;

/* 规则对应的运行时状态 (用于长按检测) */
typedef struct {
    int active;                      // 当前按键是否正被按下
    struct timespec press_time;      // 按下的时间点
    int long_triggered;              // 长按是否已经触发过
} KeyState;

/* 已打开的设备文件 */
typedef struct {
    char path[MAX_DEVICE_PATH];
    int fd;
} OpenDevice;

/* 全局数据 */
static KeyConfig configs[MAX_CONFIGS];
static KeyState states[MAX_CONFIGS];
static int config_count = 0;

static OpenDevice devices[MAX_DEVICES];
static int device_count = 0;

static volatile sig_atomic_t should_exit = 0;

/* 获取当前单调时间 (毫秒) */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 信号处理函数 */
static void signal_handler(int sig) {
    (void)sig;
    should_exit = 1;
}

/* 执行脚本 (fork + execl) */
static void execute_script(const char *script) {
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程: 执行脚本
        execl(script, script, NULL);
        // 如果执行失败，记录错误并退出
        perror("execl");
        exit(1);
    } else if (pid < 0) {
        perror("fork");
    }
    // 父进程直接返回，不等待子进程 (避免阻塞)
    // 同时忽略子进程退出信号，防止僵尸 (或者可注册 SIGCHLD 忽略)
}

/* 打开一个设备，如果已经打开则返回其 fd */
static int open_device(const char *path) {
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].path, path) == 0)
            return devices[i].fd;
    }
    if (device_count >= MAX_DEVICES) {
        fprintf(stderr, "超过最大设备数: %s\n", path);
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open device");
        return -1;
    }
    strcpy(devices[device_count].path, path);
    devices[device_count].fd = fd;
    device_count++;
    printf("已打开设备: %s (fd=%d)\n", path, fd);
    return fd;
}

/* 解析配置文件 */
static int parse_config(void) {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) {
        perror("打开配置文件失败");
        return 0;
    }

    char line[512];
    int line_num = 0;
    config_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        // 去除结尾换行和前后空白
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        // 移除末尾换行符
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        // 格式: 设备路径 键码(十六进制) 触发方式 脚本路径 [长按时间ms]
        char device[MAX_DEVICE_PATH];
        char keycode_str[16];
        char trigger_str[16];
        char script[MAX_SCRIPT_PATH];
        int long_ms = 0;

        int n = sscanf(p, "%s %s %s %s %d", device, keycode_str, trigger_str, script, &long_ms);
        if (n < 4) {
            fprintf(stderr, "配置文件行 %d 格式错误，跳过: %s\n", line_num, p);
            continue;
        }

        // 转换键码 (十六进制)
        int keycode = (int)strtol(keycode_str, NULL, 16);
        if (keycode == 0 && errno == EINVAL) {
            fprintf(stderr, "行 %d 键码无效: %s\n", line_num, keycode_str);
            continue;
        }

        // 解析触发方式
        TriggerType trigger;
        if (strcmp(trigger_str, "press") == 0)
            trigger = TRIGGER_PRESS;
        else if (strcmp(trigger_str, "release") == 0)
            trigger = TRIGGER_RELEASE;
        else if (strcmp(trigger_str, "long_press") == 0)
            trigger = TRIGGER_LONG_PRESS;
        else {
            fprintf(stderr, "行 %d 触发方式无效: %s (press/release/long_press)\n", line_num, trigger_str);
            continue;
        }

        // 长按时必须提供时间参数
        if (trigger == TRIGGER_LONG_PRESS && n < 5) {
            fprintf(stderr, "行 %d 长按模式需要指定长按时间(ms)\n", line_num);
            continue;
        }

        // 保存配置
        strcpy(configs[config_count].device_path, device);
        configs[config_count].keycode = keycode;
        configs[config_count].trigger = trigger;
        strcpy(configs[config_count].script_path, script);
        configs[config_count].long_press_ms = long_ms;
        // 初始化状态
        states[config_count].active = 0;
        states[config_count].long_triggered = 0;
        config_count++;

        printf("配置 %d: %s 0x%02X %s -> %s", config_count, device, keycode, trigger_str, script);
        if (trigger == TRIGGER_LONG_PRESS)
            printf(" (长按 %d ms)", long_ms);
        printf("\n");
    }

    fclose(fp);
    return config_count > 0;
}

/* 根据配置索引执行脚本 (带简单防重复触发标志重置) */
static void trigger_config(int idx) {
    printf("触发规则 %d: 执行 %s\n", idx + 1, configs[idx].script_path);
    execute_script(configs[idx].script_path);
}

/* 处理输入事件 */
static void process_event(const struct input_event *ev, int fd) {
    // 确定事件属于哪个设备路径 (根据fd反查)
    char dev_path[MAX_DEVICE_PATH] = {0};
    for (int i = 0; i < device_count; i++) {
        if (devices[i].fd == fd) {
            strcpy(dev_path, devices[i].path);
            break;
        }
    }
    if (dev_path[0] == '\0')
        return;  // 未知设备

    // 只处理按键事件
    if (ev->type != EV_KEY)
        return;

    // 查找匹配的配置项
    for (int i = 0; i < config_count; i++) {
        if (strcmp(configs[i].device_path, dev_path) == 0 && configs[i].keycode == ev->code) {
            // 匹配到规则
            if (ev->value == 1) {   // 按键按下
                switch (configs[i].trigger) {
                    case TRIGGER_PRESS:
                        trigger_config(i);
                        break;
                    case TRIGGER_LONG_PRESS:
                        // 记录按下状态和时间，重置长按触发标志
                        states[i].active = 1;
                        clock_gettime(CLOCK_MONOTONIC, &states[i].press_time);
                        states[i].long_triggered = 0;
                        break;
                    default: // release 触发在按下时无动作
                        break;
                }
            } else if (ev->value == 0) { // 按键释放
                switch (configs[i].trigger) {
                    case TRIGGER_RELEASE:
                        trigger_config(i);
                        break;
                    case TRIGGER_LONG_PRESS:
                        if (states[i].active) {
                            // 检查是否已经触发过长按
                            if (!states[i].long_triggered) {
                                // 未触发长按，计算按下持续时间
                                struct timespec now;
                                clock_gettime(CLOCK_MONOTONIC, &now);
                                int elapsed_ms = (now.tv_sec - states[i].press_time.tv_sec) * 1000 +
                                                 (now.tv_nsec - states[i].press_time.tv_nsec) / 1000000;
                                if (elapsed_ms >= configs[i].long_press_ms) {
                                    // 达到长按阈值，但尚未触发，说明 poll 的超时检查还没来得及触发？
                                    // 这里也执行一次（安全兜底）
                                    trigger_config(i);
                                }
                            }
                            states[i].active = 0;
                            states[i].long_triggered = 0;
                        }
                        break;
                    default:
                        break;
                }
            }
            // 对于长按模式，释放后重置标志已经在上面完成
        }
    }
}

/* 检查所有长按配置的超时 */
static void check_long_press_timeouts(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < config_count; i++) {
        if (configs[i].trigger != TRIGGER_LONG_PRESS)
            continue;
        if (states[i].active && !states[i].long_triggered) {
            int elapsed_ms = (now.tv_sec - states[i].press_time.tv_sec) * 1000 +
                             (now.tv_nsec - states[i].press_time.tv_nsec) / 1000000;
            if (elapsed_ms >= configs[i].long_press_ms) {
                // 长按条件满足，执行脚本并标记已触发
                trigger_config(i);
                states[i].long_triggered = 1;
                // 注意：不重置 active，等待释放时清理
            }
        }
    }
}

/* 主循环：使用 poll 监听多个设备 */
static void event_loop(void) {
    if (device_count == 0) {
        fprintf(stderr, "没有可用的输入设备\n");
        return;
    }

    struct pollfd *pfds = malloc(sizeof(struct pollfd) * device_count);
    if (!pfds) {
        perror("malloc");
        return;
    }

    for (int i = 0; i < device_count; i++) {
        pfds[i].fd = devices[i].fd;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    printf("开始监听 %d 个设备...\n", device_count);

    while (!should_exit) {
        int ret = poll(pfds, device_count, POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        // 处理可读事件
        for (int i = 0; i < device_count; i++) {
            if (pfds[i].revents & POLLIN) {
                struct input_event ev;
                ssize_t n = read(pfds[i].fd, &ev, sizeof(ev));
                if (n == sizeof(ev)) {
                    process_event(&ev, pfds[i].fd);
                } else if (n < 0 && errno != EAGAIN) {
                    perror("read");
                }
            }
        }

        // 检查长按超时
        check_long_press_timeouts();
    }

    free(pfds);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // 检查 root 权限
    if (getuid() != 0) {
        fprintf(stderr, "需要 root 权限运行\n");
        return 1;
    }

    // 注册信号
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // 忽略子进程退出信号，避免僵尸进程
    signal(SIGCHLD, SIG_IGN);

    // 解析配置
    if (!parse_config()) {
        fprintf(stderr, "解析配置失败或无有效规则\n");
        return 1;
    }

    // 打开所有用到的设备
    for (int i = 0; i < config_count; i++) {
        int fd = open_device(configs[i].device_path);
        if (fd < 0) {
            fprintf(stderr, "警告: 无法打开设备 %s，相关规则将失效\n", configs[i].device_path);
        }
    }

    // 如果没有任何设备打开成功，退出
    if (device_count == 0) {
        fprintf(stderr, "无法打开任何输入设备\n");
        return 1;
    }

    // 进入事件循环
    event_loop();

    // 清理
    for (int i = 0; i < device_count; i++) {
        close(devices[i].fd);
    }
    printf("程序正常退出\n");
    return 0;
}