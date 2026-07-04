/**
 * 编译: clang -o switcher switcher.c -lselinux
 * 用法: switcher <uid> <gid> <groups> <context> <caps> <target> [args...]
 *
 * 参数:
 *   uid     - 数字 UID
 *   gid     - 数字 GID
 *   groups  - 逗号分隔的附加 GID，空字符串跳过
 *   context - SELinux context 字符串（如 "u:r:su:s0"），空跳过
 *   caps    - 逗号分隔的能力名称，如 "cap_net_raw,cap_sys_admin"，空跳过
 *   target  - 目标程序路径，后续参数原样传递
 *
 * 特点:
 *   - 不检查 SELinux 是否启用，直接调用 setcon（失败仅警告）
 *   - 使用 Ambient 能力，确保目标程序（无文件能力）也能获得所需权限
 *   - 完整能力诊断，输出明确失败原因
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <grp.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <selinux/selinux.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <limits.h>

/* ---------- 能力名称映射 ---------- */
typedef struct {
    const char *name;
    int value;
} cap_map_t;

static const cap_map_t cap_table[] = {
    {"cap_chown", CAP_CHOWN},
    {"cap_dac_override", CAP_DAC_OVERRIDE},
    {"cap_dac_read_search", CAP_DAC_READ_SEARCH},
    {"cap_fowner", CAP_FOWNER},
    {"cap_fsetid", CAP_FSETID},
    {"cap_kill", CAP_KILL},
    {"cap_setgid", CAP_SETGID},
    {"cap_setuid", CAP_SETUID},
    {"cap_setpcap", CAP_SETPCAP},
    {"cap_linux_immutable", CAP_LINUX_IMMUTABLE},
    {"cap_net_bind_service", CAP_NET_BIND_SERVICE},
    {"cap_net_broadcast", CAP_NET_BROADCAST},
    {"cap_net_admin", CAP_NET_ADMIN},
    {"cap_net_raw", CAP_NET_RAW},
    {"cap_ipc_lock", CAP_IPC_LOCK},
    {"cap_ipc_owner", CAP_IPC_OWNER},
    {"cap_sys_module", CAP_SYS_MODULE},
    {"cap_sys_rawio", CAP_SYS_RAWIO},
    {"cap_sys_chroot", CAP_SYS_CHROOT},
    {"cap_sys_ptrace", CAP_SYS_PTRACE},
    {"cap_sys_pacct", CAP_SYS_PACCT},
    {"cap_sys_admin", CAP_SYS_ADMIN},
    {"cap_sys_boot", CAP_SYS_BOOT},
    {"cap_sys_nice", CAP_SYS_NICE},
    {"cap_sys_resource", CAP_SYS_RESOURCE},
    {"cap_sys_time", CAP_SYS_TIME},
    {"cap_sys_tty_config", CAP_SYS_TTY_CONFIG},
    {"cap_mknod", CAP_MKNOD},
    {"cap_lease", CAP_LEASE},
    {"cap_audit_write", CAP_AUDIT_WRITE},
    {"cap_audit_control", CAP_AUDIT_CONTROL},
    {"cap_setfcap", CAP_SETFCAP},
    {"cap_mac_override", CAP_MAC_OVERRIDE},
    {"cap_mac_admin", CAP_MAC_ADMIN},
    {"cap_syslog", CAP_SYSLOG},
    {"cap_wake_alarm", CAP_WAKE_ALARM},
    {"cap_block_suspend", CAP_BLOCK_SUSPEND},
    {"cap_audit_read", CAP_AUDIT_READ},
    {NULL, 0}
};

static int cap_name_to_value(const char *name) {
    for (const cap_map_t *p = cap_table; p->name; ++p)
        if (strcmp(p->name, name) == 0) return p->value;
    return -1;
}

static void print_cap_names(uint64_t mask, const char *prefix) {
    if (mask == 0) {
        printf("%s<none>", prefix);
        return;
    }
    int first = 1;
    for (const cap_map_t *p = cap_table; p->name; ++p) {
        if (mask & (1ULL << p->value)) {
            printf("%s%s", first ? prefix : ", ", p->name);
            first = 0;
        }
    }
    if (first) printf("%s<unknown bits: 0x%llx>", prefix, (unsigned long long)mask);
}

/* 获取当前 Permitted, Effective, Inheritable, Bound */
static int get_current_caps(uint64_t *cur_p, uint64_t *cur_e, uint64_t *cur_i, uint64_t *bound) {
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0
    };
    struct __user_cap_data_struct data[2] = {{0}};
    if (syscall(SYS_capget, &hdr, data) == -1) return -1;
    *cur_p = ((uint64_t)data[1].permitted << 32) | data[0].permitted;
    *cur_e = ((uint64_t)data[1].effective << 32) | data[0].effective;
    *cur_i = ((uint64_t)data[1].inheritable << 32) | data[0].inheritable;
    uint64_t b = 0;
    for (int cap = 0; cap < 64; ++cap)
        if (prctl(PR_CAPBSET_READ, cap, 0, 0, 0) == 1) b |= (1ULL << cap);
    *bound = b;
    return 0;
}

/* 检查 capset 可行性（按您要求输出详细原因）*/
static int check_capset_feasibility(uint64_t new_p, uint64_t new_e, uint64_t new_i) {
    uint64_t cur_p, cur_e, cur_i, bound;
    if (get_current_caps(&cur_p, &cur_e, &cur_i, &bound) == -1) {
        fprintf(stderr, "无法获取当前能力集\n");
        return -1;
    }
    int ok = 1;
    if ((new_e & ~new_p) != 0) {
        uint64_t bad = new_e & ~new_p;
        printf("[诊断 FAIL] Effective 包含不在 Permitted 中的能力: 0x%llx (", (unsigned long long)bad);
        print_cap_names(bad, ""); printf(")\n");
        ok = 0;
    }
    if ((new_p & ~bound) != 0) {
        uint64_t bad = new_p & ~bound;
        printf("[诊断 FAIL] Permitted 超出 Bound: 0x%llx (", (unsigned long long)bad);
        print_cap_names(bad, ""); printf(")\n");
        ok = 0;
    }
    int has_setpcap = (cur_e & (1ULL << CAP_SETPCAP)) != 0;
    if (!has_setpcap && (new_p & ~cur_p) != 0) {
        uint64_t added = new_p & ~cur_p;
        printf("[诊断 FAIL] 无 CAP_SETPCAP 且试图扩大 Permitted: 0x%llx (", (unsigned long long)added);
        print_cap_names(added, ""); printf(")\n");
        ok = 0;
    }
    if (!has_setpcap && (new_i & ~cur_i) != 0) {
        uint64_t added = new_i & ~cur_i;
        printf("[诊断 FAIL] 无 CAP_SETPCAP 且试图扩大 Inheritable: 0x%llx (", (unsigned long long)added);
        print_cap_names(added, ""); printf(")\n");
        ok = 0;
    }
    if ((new_i & ~new_p) != 0) {
        uint64_t orphan = new_i & ~new_p;
        printf("[诊断 WARN] Inheritable 包含不在 Permitted 中的能力: 0x%llx (", (unsigned long long)orphan);
        print_cap_names(orphan, ""); printf(")\n");
    }
    if (ok) printf("[诊断 OK] 满足 capset 前置条件\n");
    return ok ? 0 : -1;
}

/* 调用 capset 设置 P/E/I */
static int set_caps(uint64_t new_p, uint64_t new_e, uint64_t new_i) {
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0
    };
    struct __user_cap_data_struct data[2] = {{0}};
    data[0].permitted   = new_p & 0xFFFFFFFF;
    data[0].effective   = new_e & 0xFFFFFFFF;
    data[0].inheritable = new_i & 0xFFFFFFFF;
    data[1].permitted   = (new_p >> 32) & 0xFFFFFFFF;
    data[1].effective   = (new_e >> 32) & 0xFFFFFFFF;
    data[1].inheritable = (new_i >> 32) & 0xFFFFFFFF;
    if (syscall(SYS_capset, &hdr, data) == -1) {
        perror("capset");
        return -1;
    }
    return 0;
}

/* 验证能力是否真正生效 */
static int verify_caps(uint64_t expected_p, uint64_t expected_e, uint64_t expected_i) {
    uint64_t cur_p, cur_e, cur_i, bound;
    if (get_current_caps(&cur_p, &cur_e, &cur_i, &bound) == -1) return -1;
    int ok = 1;
    if (cur_p != expected_p) {
        fprintf(stderr, "[验证 FAIL] Permitted: 期望 0x%llx, 实际 0x%llx\n",
                (unsigned long long)expected_p, (unsigned long long)cur_p);
        ok = 0;
    }
    if (cur_e != expected_e) {
        fprintf(stderr, "[验证 FAIL] Effective: 期望 0x%llx, 实际 0x%llx\n",
                (unsigned long long)expected_e, (unsigned long long)cur_e);
        ok = 0;
    }
    if (cur_i != expected_i) {
        fprintf(stderr, "[验证 FAIL] Inheritable: 期望 0x%llx, 实际 0x%llx\n",
                (unsigned long long)expected_i, (unsigned long long)cur_i);
        ok = 0;
    }
    if (ok) printf("[验证 OK] 能力设置符合预期\n");
    else fprintf(stderr, "[验证 FAIL] 能力可能被内核覆盖\n");
    return ok ? 0 : -1;
}

/* 设置 Ambient 能力（关键！解决 exec 后能力丢失）*/
static void set_ambient_caps(uint64_t mask) {
    if (mask == 0) return;
    for (int cap = 0; cap < 64; ++cap) {
        if (mask & (1ULL << cap)) {
            if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) == 0) {
                printf("[Ambient] 已提升能力: %s\n", cap_table[cap].name ? cap_table[cap].name : "?");
            } else {
                perror("prctl(PR_CAP_AMBIENT_RAISE)");
                fprintf(stderr, "警告: 无法提升能力 %d，可能内核<4.3或缺少 CAP_SETPCAP\n", cap);
            }
        }
    }
}

/* 解析能力字符串 */
static uint64_t parse_cap_string(const char *cap_str) {
    if (!cap_str || cap_str[0] == '\0') return 0;
    char *work = strdup(cap_str);
    if (!work) return 0;
    uint64_t mask = 0;
    char *saveptr;
    char *token = strtok_r(work, ",", &saveptr);
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';
        if (*token) {
            int val = cap_name_to_value(token);
            if (val >= 0 && val < 64) mask |= (1ULL << val);
            else fprintf(stderr, "警告: 跳过未知能力 '%s'\n", token);
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    free(work);
    return mask;
}

/* 清理危险环境变量 */
static void sanitize_environment(void) {
    const char *danger[] = {
        "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "LD_DEBUG",
        "LD_PROFILE", "LD_SHOW_AUXV", "LD_USE_LOAD_BIAS",
        "MALLOC_CHECK_", "RESOLV_HOST_CONF", "HOSTALIASES",
        "GCONV_PATH", "GETCONF_DIR", "GLIBC_TUNABLES", NULL
    };
    for (int i = 0; danger[i]; ++i) unsetenv(danger[i]);
    setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 1);
}

/* ---------- 主程序 ---------- */
int main(int argc, char *argv[]) {
    if (argc < 7) {
        fprintf(stderr, "用法: %s <uid> <gid> <groups> <context> <caps> <target> [args...]\n"
                        "示例: %s 1000 1000 1001,1002 u:r:su:s0 cap_net_raw,cap_sys_admin /bin/sh\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char *uid_str    = argv[1];
    const char *gid_str    = argv[2];
    const char *groups_str = argv[3];
    const char *context_str= argv[4];
    const char *caps_str   = argv[5];
    char *target           = argv[6];
    char **target_argv     = &argv[6];

    sanitize_environment();

    /* 1. SELinux context – 不检查是否启用，直接调用，失败仅警告 */
    if (context_str && context_str[0]) {
        if (setcon(context_str) == 0) {
            printf("[SELinux] context 已设置为: %s\n", context_str);
        } else {
            perror("[SELinux] setcon 失败, 跳过");
        }
    }

    /* 2. 允许降权后保留 Permitted 能力 */
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == -1) {
        perror("prctl(PR_SET_KEEPCAPS)");
    }

    /* 3. 附加组 */
    if (groups_str && groups_str[0]) {
        char *work = strdup(groups_str);
        if (work) {
            gid_t groups[64];
            int cnt = 0;
            char *save, *tok = strtok_r(work, ",", &save);
            while (tok && cnt < 64) {
                while (*tok == ' ' || *tok == '\t') tok++;
                char *end;
                gid_t g = strtoul(tok, &end, 10);
                if (*end == '\0') groups[cnt++] = g;
                else fprintf(stderr, "跳过无效组: %s\n", tok);
                tok = strtok_r(NULL, ",", &save);
            }
            if (cnt > 0 && setgroups(cnt, groups) == -1) {
                perror("setgroups 失败, 跳过");
            } else {
                printf("[Groups] 已设置 %d 个附加组\n", cnt);
            }
            free(work);
        }
    }

    /* 4. GID */
    if (gid_str && gid_str[0]) {
        char *end;
        gid_t gid = strtoul(gid_str, &end, 10);
        if (*end == '\0' && setgid(gid) == 0) {
            printf("[GID] %d\n", gid);
        } else {
            perror("[GID] setgid 失败, 跳过");
        }
    }

    /* 5. UID */
    if (uid_str && uid_str[0]) {
        char *end;
        uid_t uid = strtoul(uid_str, &end, 10);
        if (*end == '\0' && setuid(uid) == 0) {
            printf("[UID] %d\n", uid);
        } else {
            perror("[UID] setuid 失败, 跳过");
        }
    }

    /* 6. 能力设置 (关键：降权后设置 Ambient) */
    if (caps_str && caps_str[0]) {
        uint64_t want = parse_cap_string(caps_str);
        if (want == 0) {
            printf("[Caps] 无有效能力\n");
        } else {
            printf("[Caps] 目标掩码: 0x%llx\n", (unsigned long long)want);
            // 我们希望 P/E/I 都等于 want，以便能设置 Ambient（需要 P 和 I 同时包含）
            if (check_capset_feasibility(want, want, want) == 0) {
                if (set_caps(want, want, want) == 0) {
                    if (verify_caps(want, want, want) == 0) {
                        // 成功设置 P/E/I 后，提升 Ambient 能力
                        set_ambient_caps(want);
                        printf("[Caps] 完成\n");
                    } else {
                        fprintf(stderr, "[Caps] capset 成功但验证失败\n");
                    }
                } else {
                    fprintf(stderr, "[Caps] capset 调用失败\n");
                }
            } else {
                fprintf(stderr, "[Caps] 前置检查不通过，跳过设置\n");
            }
        }
    }

    /* 7. 执行目标 */
    printf("[Exec] %s\n", target);
    execv(target, target_argv);
    perror("execv 失败");
    return EXIT_FAILURE;
}