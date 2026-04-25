/**
 * KUMF SPE Self-Profiler
 *
 * 在 perf_event_paranoid=2 下，进程可以 profiling 自己。
 * 不需要 sudo，不需要 perf 工具，直接 syscall 采集 ARM SPE 数据。
 *
 * 用法:
 *   1. 编译: gcc -o spe_self_profile spe_self_profile.c -lpthread
 *   2. LD_PRELOAD 方式: LD_PRELOAD=./spe_self_profile.so ./your_workload
 *   3. 或 wrapper 方式: ./spe_self_profile -- ./your_workload args...
 *
 * 输出: /tmp/kumf/spe_self_profile.txt
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/unistd.h>
#include <pthread.h>

#define MAX_CPUS 160
#define BUF_SIZE (256 * 1024)  /* 256KB per CPU buffer */
#define OUTPUT_PATH "/tmp/kumf/spe_self_profile.txt"

static int perf_fds[MAX_CPUS];
static int n_cpus = 0;
static FILE *output_file = NULL;
static volatile int profiling_active = 0;
static pthread_t profile_thread;

/* ---- ARM SPE perf_event_attr 配置 ---- */

/**
 * ARM SPE 通过 perf_event_open 采集:
 *   type = PERF_TYPE_HARDWARE (或动态分配的 type)
 *   config = 对应 SPE 事件配置
 *
 * 但在 Linux 6.6+ 上, ARM SPE 有专用的 PMU type.
 * 需要动态检测: 读 /sys/bus/event_source/devices/arm_spe_0/type
 */
static int get_spe_pmu_type(void) {
    FILE *f = fopen("/sys/bus/event_source/devices/arm_spe_0/type", "r");
    if (!f) {
        /* 尝试 SPE v1p1 */
        f = fopen("/sys/bus/event_source/devices/arm_spe_0/type", "r");
        if (!f) return -1;
    }
    int type = -1;
    fscanf(f, "%d", &type);
    fclose(f);
    return type;
}

/**
 * 读取 SPE 的 format 配置
 * /sys/bus/event_source/devices/arm_spe_0/format/
 * 常见: load_filter, store_filter, min_latency, branch_filter, event_filter
 */
static int read_format_bits(const char *name) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/arm_spe_0/format/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int bits = 0;
    /* Format: "config:N" or "config1:N" */
    char buf[64];
    if (fgets(buf, sizeof(buf), f)) {
        char *colon = strchr(buf, ':');
        if (colon) {
            bits = atoi(colon + 1);
        }
    }
    fclose(f);
    return bits;
}

static long long spe_event_config(int load, int store, int min_latency) {
    long long config = 0;

    /* 读取 format bits */
    int load_bit = read_format_bits("load_filter");
    int store_bit = read_format_bits("store_filter");
    int latency_bit = read_format_bits("min_latency");

    if (load) config |= (1LL << load_bit);
    if (store) config |= (1LL << store_bit);
    if (min_latency > 0) config |= ((long long)min_latency << latency_bit);

    return config;
}

/* ---- perf_event_open 封装 ---- */

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/* ---- 采集线程 ---- */

static void parse_spe_buffer(struct perf_event_mmap_page *header, int cpu) {
    if (!header) return;

    /* Read ring buffer */
    unsigned long head = header->data_head;
    unsigned long tail = header->data_tail;
    unsigned long mask = (BUF_SIZE - 1);

    if (head == tail) return;

    /* Memory barrier */
    __sync_synchronize();

    char *base = (char *)header + header->data_offset;
    unsigned long consumed = 0;
    int sample_count = 0;

    while (tail + consumed < head) {
        struct perf_event_header *peh =
            (struct perf_event_header *)(base + ((tail + consumed) & mask));

        if (peh->size == 0) break;

        if (peh->type == PERF_RECORD_SAMPLE) {
            /* ARM SPE sample structure (simplified):
             * - perf_event_header (8 bytes)
             * - sample_id (depends on attr.sample_type)
             * - SPE record data
             *
             * We dump raw hex for post-processing by soar_analyzer.py
             */
            unsigned char *data = (unsigned char *)peh;
            int size = peh->size;

            /* Write binary as hex to output */
            fprintf(output_file, "SPE_SAMPLE cpu=%d size=%d ", cpu, size);
            for (int i = 0; i < size && i < 128; i++) {
                fprintf(output_file, "%02x", data[i]);
            }
            fprintf(output_file, "\n");
            sample_count++;
        }

        consumed += peh->size;
    }

    /* Update tail to acknowledge consumption */
    header->data_tail = head;
}

static void *profile_thread_fn(void *arg) {
    (void)arg;

    struct perf_event_mmap_page *mmap_headers[MAX_CPUS] = {NULL};

    /* Open SPE perf events for all target CPUs, targeting current PID */
    int spe_type = get_spe_pmu_type();
    if (spe_type < 0) {
        fprintf(stderr, "[SPE] ARM SPE PMU not found. Trying fallback...\n");

        /* Fallback: 用通用 hardware events 代替 SPE */
        spe_type = PERF_TYPE_HARDWARE;
    }

    long long config = spe_event_config(1, 1, 32);  /* Load+Store, min_latency=32 */

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = spe_type;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR |
                       PERF_SAMPLE_CPU | PERF_SAMPLE_WEIGHT |
                       PERF_SAMPLE_DATA_SRC;
    attr.sample_period = 10000;  /* Sample every N events */
    attr.disabled = 1;           /* Start disabled */
    attr.exclude_kernel = 1;     /* Only user-space */
    attr.exclude_hv = 1;
    attr.precise_ip = 0;

    n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cpus > MAX_CPUS) n_cpus = MAX_CPUS;

    for (int i = 0; i < n_cpus; i++) {
        /* PID=0 means "profile calling process" which is allowed at paranoid=2
         * Actually: pid = getpid() for self-profiling
         */
        perf_fds[i] = perf_event_open(&attr, 0 /* self */, i, -1, 0);
        if (perf_fds[i] < 0) {
            /* Only warn once */
            if (i == 0) {
                fprintf(stderr, "[SPE] perf_event_open failed on CPU 0: %s\n", strerror(errno));
                fprintf(stderr, "[SPE] Falling back to generic perf events\n");

                /* Fallback: CPU cycles sampling */
                attr.type = PERF_TYPE_HARDWARE;
                attr.config = PERF_COUNT_HW_CPU_CYCLES;
                attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
                attr.sample_period = 100000;
                attr.precise_ip = 0;

                perf_fds[i] = perf_event_open(&attr, 0, i, -1, 0);
            }
            if (perf_fds[i] < 0) continue;

            /* Re-try all CPUs with fallback config */
            for (int j = i + 1; j < n_cpus; j++) {
                perf_fds[j] = perf_event_open(&attr, 0, j, -1, 0);
            }
            break;
        }
    }

    /* mmap buffers */
    for (int i = 0; i < n_cpus; i++) {
        if (perf_fds[i] < 0) continue;
        void *buf = mmap(NULL, BUF_SIZE + getpagesize(),
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         perf_fds[i], 0);
        if (buf == MAP_FAILED) {
            perf_fds[i] = -1;
            continue;
        }
        mmap_headers[i] = (struct perf_event_mmap_page *)buf;
    }

    /* Enable all */
    for (int i = 0; i < n_cpus; i++) {
        if (perf_fds[i] >= 0) {
            ioctl(perf_fds[i], PERF_EVENT_IOC_ENABLE, 0);
        }
    }

    fprintf(stderr, "[SPE] Profiling started on %d CPUs\n", n_cpus);

    /* Poll loop */
    struct timespec ts = {1, 0};  /* 1 second interval */
    int total_samples = 0;

    while (profiling_active) {
        nanosleep(&ts, NULL);
        for (int i = 0; i < n_cpus; i++) {
            if (perf_fds[i] < 0 || !mmap_headers[i]) continue;
            parse_spe_buffer(mmap_headers[i], i);
        }
    }

    /* Final drain */
    for (int i = 0; i < n_cpus; i++) {
        if (perf_fds[i] < 0 || !mmap_headers[i]) continue;
        parse_spe_buffer(mmap_headers[i], i);
    }

    /* Cleanup */
    for (int i = 0; i < n_cpus; i++) {
        if (perf_fds[i] >= 0) {
            ioctl(perf_fds[i], PERF_EVENT_IOC_DISABLE, 0);
            if (mmap_headers[i])
                munmap(mmap_headers[i], BUF_SIZE + getpagesize());
            close(perf_fds[i]);
        }
    }

    fprintf(stderr, "[SPE] Profiling stopped. Total samples written.\n");
    return NULL;
}

/* ---- Public API ---- */

void spe_profile_start(void) {
    if (profiling_active) return;

    output_file = fopen(OUTPUT_PATH, "w");
    if (!output_file) {
        fprintf(stderr, "[SPE] Cannot open %s: %s\n", OUTPUT_PATH, strerror(errno));
        return;
    }

    fprintf(output_file, "# KUMF SPE Self-Profile Output\n");
    fprintf(output_file, "# Format: SPE_SAMPLE cpu=N size=N <hex_data>\n");
    fprintf(output_file, "# PID=%d\n", getpid());
    fprintf(output_file, "#\n");

    profiling_active = 1;
    pthread_create(&profile_thread, NULL, profile_thread_fn, NULL);
}

void spe_profile_stop(void) {
    if (!profiling_active) return;
    profiling_active = 0;
    pthread_join(profile_thread, NULL);
    if (output_file) {
        fclose(output_file);
        output_file = NULL;
    }
}

/* ---- Wrapper Mode (main) ---- */

static void sigchld_handler(int sig) {
    (void)sig;
    spe_profile_stop();
}

int main(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "--") == 0 && argc < 3) {
        fprintf(stderr, "Usage: %s -- <command> [args...]\n", argv[0]);
        fprintf(stderr, "       %s --pid <PID>           # attach to running process\n", argv[0]);
        return 1;
    }

    mkdir("/tmp/kumf", 0755);

    if (strcmp(argv[1], "--pid") == 0 && argc >= 3) {
        /* Attach mode: profile existing process by PID
         * Note: paranoid=2 allows profiling own processes (same UID)
         */
        pid_t target_pid = atoi(argv[2]);
        fprintf(stderr, "[SPE] Attaching to PID %d (must be same user)\n", target_pid);

        /* For attach mode, we'd need to set pid in perf_event_open */
        /* This is a simplified version - full implementation would modify profile_thread_fn */
        fprintf(stderr, "[SPE] Attach mode: use LD_PRELOAD instead for full SPE capture\n");
        return 1;
    }

    /* Wrapper mode: fork + exec child, profile it */
    signal(SIGCHLD, sigchld_handler);

    pid_t child = fork();
    if (child == 0) {
        /* Child: exec the target workload */
        int cmd_start = (strcmp(argv[1], "--") == 0) ? 2 : 1;
        execvp(argv[cmd_start], &argv[cmd_start]);
        perror("execvp");
        return 1;
    }

    /* Parent: profile the child */
    fprintf(stderr, "[SPE] Child PID=%d, starting profiling...\n", child);

    output_file = fopen(OUTPUT_PATH, "w");
    if (!output_file) {
        perror("fopen");
        return 1;
    }
    fprintf(output_file, "# KUMF SPE Self-Profile (wrapper mode)\n");
    fprintf(output_file, "# Target PID=%d\n", child);
    fprintf(output_file, "#\n");

    /* For wrapper mode, we profile the child PID
     * perf_event_paranoid=2 allows this for same-UID processes
     */
    profiling_active = 1;

    /* Open SPE for child PID */
    int spe_type = get_spe_pmu_type();
    long long config = spe_event_config(1, 1, 32);

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = (spe_type >= 0) ? spe_type : PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = (spe_type >= 0) ? config : PERF_COUNT_HW_CPU_CYCLES;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR |
                       PERF_SAMPLE_CPU | PERF_SAMPLE_WEIGHT;
    attr.sample_period = 10000;
    attr.disabled = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (n_cpus > MAX_CPUS) n_cpus = MAX_CPUS;

    struct perf_event_mmap_page *mmap_headers[MAX_CPUS] = {NULL};
    int active_cpus = 0;

    for (int i = 0; i < n_cpus; i++) {
        perf_fds[i] = perf_event_open(&attr, child, i, -1, 0);
        if (perf_fds[i] < 0) {
            if (i == 0) {
                fprintf(stderr, "[SPE] perf_event_open child PID failed: %s\n", strerror(errno));
                fprintf(stderr, "[SPE] Try: same-UID process, or paranoid <= 1\n");
            }
            continue;
        }
        void *buf = mmap(NULL, BUF_SIZE + getpagesize(),
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         perf_fds[i], 0);
        if (buf == MAP_FAILED) {
            close(perf_fds[i]);
            perf_fds[i] = -1;
            continue;
        }
        mmap_headers[i] = buf;
        active_cpus++;
    }

    fprintf(stderr, "[SPE] Active on %d/%d CPUs\n", active_cpus, n_cpus);

    /* Poll until child exits */
    int status;
    while (1) {
        /* Drain buffers every 500ms */
        struct timespec ts = {0, 500000000};
        nanosleep(&ts, NULL);

        for (int i = 0; i < n_cpus; i++) {
            if (perf_fds[i] < 0 || !mmap_headers[i]) continue;
            parse_spe_buffer(mmap_headers[i], i);
        }

        /* Check if child still running */
        pid_t ret = waitpid(child, &status, WNOHANG);
        if (ret == child) break;
        if (ret < 0 && errno == ECHILD) break;
    }

    /* Final drain */
    for (int i = 0; i < n_cpus; i++) {
        if (perf_fds[i] >= 0 && mmap_headers[i]) {
            parse_spe_buffer(mmap_headers[i], i);
            ioctl(perf_fds[i], PERF_EVENT_IOC_DISABLE, 0);
            munmap(mmap_headers[i], BUF_SIZE + getpagesize());
            close(perf_fds[i]);
        }
    }

    fclose(output_file);
    fprintf(stderr, "[SPE] Done. Output: %s\n", OUTPUT_PATH);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
