#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define CONFIG_FILE "./time_config"
#define CHECK_INTERVAL 20  // 检查间隔
#define MAX_TIME_SLOTS 20  // 每天最多时间段数

// 时间段结构体
typedef struct {
    int start_hour;
    int start_min;
    int end_hour;
    int end_min;
} TimeSlot;

// 每日安排结构体
typedef struct {
    TimeSlot slots[MAX_TIME_SLOTS];
    int slot_count;
} DaySchedule;

// 全局变量
DaySchedule schedule[7];  // 0:Mon, 1:Tue, ..., 6:Sun

// 解析时间字符串 "HH:MM-HH:MM"
int parse_time_slot(const char *time_str, TimeSlot *slot) {
    int start_h, start_m, end_h, end_m;
    
    if (sscanf(time_str, "%d:%d-%d:%d", 
               &start_h, &start_m, &end_h, &end_m) != 4) {
        return 0;
    }
    
    // 验证时间有效性
    if (start_h < 0 || start_h > 23 || start_m < 0 || start_m > 59 ||
        end_h < 0 || end_h > 23 || end_m < 0 || end_m > 59) {
        return 0;
    }
    
    slot->start_hour = start_h;
    slot->start_min = start_m;
    slot->end_hour = end_h;
    slot->end_min = end_m;
    
    return 1;
}

// 解析配置文件
int parse_config_file() {
    FILE *file = fopen(CONFIG_FILE, "r");
    if (!file) {
        perror("无法打开配置文件");
        return 0;
    }
    
    char line[1024];
    char day_str[4];
    int day_index;
    
    // 初始化所有天的安排
    for (int i = 0; i < 7; i++) {
        schedule[i].slot_count = 0;
    }
    
    while (fgets(line, sizeof(line), file)) {
        // 去除换行符
        line[strcspn(line, "\n")] = 0;
        
        // 跳过空行
        if (strlen(line) == 0) {
            continue;
        }
        
        // 复制一行用于处理
        char line_copy[1024];
        strcpy(line_copy, line);
        
        // 解析星期缩写
        char *token = strtok(line_copy, " ");
        if (!token) {
            continue;
        }
        
        // 确定星期几
        if (strcmp(token, "Mon") == 0) day_index = 0;
        else if (strcmp(token, "Tue") == 0) day_index = 1;
        else if (strcmp(token, "Wed") == 0) day_index = 2;
        else if (strcmp(token, "Thu") == 0) day_index = 3;
        else if (strcmp(token, "Fri") == 0) day_index = 4;
        else if (strcmp(token, "Sat") == 0) day_index = 5;
        else if (strcmp(token, "Sun") == 0) day_index = 6;
        else {
            fprintf(stderr, "无效的星期缩写: %s\n", token);
            continue;
        }
        
        // 解析时间段
        int slot_count = 0;
        token = strtok(NULL, " ");
        
        while (token && slot_count < MAX_TIME_SLOTS) {
            // 检查是否为空（Sat情况）
            if (strlen(token) > 0) {
                TimeSlot slot;
                if (parse_time_slot(token, &slot)) {
                    schedule[day_index].slots[slot_count] = slot;
                    slot_count++;
                } else {
                    fprintf(stderr, "无效的时间格式: %s\n", token);
                }
            }
            token = strtok(NULL, " ");
        }
        
        schedule[day_index].slot_count = slot_count;
    }
    
    fclose(file);
    return 1;
}

// 检查当前时间是否在某个时间段内
int is_in_time_slot(int day, int hour, int min) {
    if (day < 0 || day > 6) {
        return 0;
    }
    
    DaySchedule *day_schedule = &schedule[day];
    
    for (int i = 0; i < day_schedule->slot_count; i++) {
        TimeSlot *slot = &day_schedule->slots[i];
        
        // 转换为分钟数便于比较
        int current_minutes = hour * 60 + min;
        int start_minutes = slot->start_hour * 60 + slot->start_min;
        int end_minutes = slot->end_hour * 60 + slot->end_min;
        
        if (current_minutes >= start_minutes && current_minutes <= end_minutes) {
            return 1;
        }
    }
    
    return 0;
}

// 执行time.sh脚本
void execute_script() {
    pid_t pid = fork();
    
    if (pid == 0) {
        // 子进程
        execl("./time.sh", "./time.sh", NULL);
        
        // 如果execl失败
        perror("执行time.sh失败");
        exit(1);
    } else if (pid > 0) {
        // 父进程，等待子进程完成
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status)) {
            printf("time.sh执行完成，退出码: %d\n", WEXITSTATUS(status));
        } else {
            printf("time.sh异常终止\n");
        }
    } else {
        perror("创建子进程失败");
    }
}

// 主循环
void main_loop() {
    const char *weekdays[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    
    while (1) {
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        
        int current_day = tm_now->tm_wday - 1;
        if (current_day < 0) current_day = 6;
        int current_hour = tm_now->tm_hour;
        int current_min = tm_now->tm_min;
        int current_sec = tm_now->tm_sec;
        
        int in_slot = is_in_time_slot(current_day, current_hour, current_min);
        
        char day_display[10];
        if (current_day >= 0 && current_day <= 6) {
            strcpy(day_display, weekdays[current_day]);
        } else {
            sprintf(day_display, "星期%d", current_day);
        }
        
        printf("时间: %02d:%02d:%02d, %s, 在时间段内: %s\n",
               current_hour, current_min, current_sec,
               day_display,
               in_slot ? "是" : "否");
        
        if (in_slot) {
            printf("满足条件，执行time.sh脚本...\n");
            execute_script();
        }
        
        sleep(CHECK_INTERVAL);
    }
}

// 打印配置信息（调试用）
void print_schedule() {
    const char *days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    
    for (int i = 0; i < 7; i++) {
        printf("%s: ", days[i]);
        
        if (schedule[i].slot_count == 0) {
            printf("无安排\n");
            continue;
        }
        
        for (int j = 0; j < schedule[i].slot_count; j++) {
            TimeSlot *slot = &schedule[i].slots[j];
            printf("%02d:%02d-%02d:%02d ", 
                   slot->start_hour, slot->start_min,
                   slot->end_hour, slot->end_min);
        }
        printf("\n");
    }
}

int main() {
    printf("ARMv7 时间检查程序启动\n");
    
    // 解析配置文件
    if (!parse_config_file()) {
        fprintf(stderr, "解析配置文件失败\n");
        return 1;
    }
    
    printf("配置文件解析成功:\n");
    print_schedule();
    
    printf("开始主循环，每%d秒检查一次\n", CHECK_INTERVAL);
    printf("检测到满足条件时，将执行./time.sh脚本\n");
    
    // 进入主循环
    main_loop();
    
    printf("程序退出\n");
    return 0;
}