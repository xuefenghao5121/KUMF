/**
 * KUMF SPE LD_PRELOAD Profiler
 *
 * 用法: LD_PRELOAD=./spe_preload.so ./your_workload
 *
 * 在 perf_event_paranoid=2 下自动采集当前进程的 SPE 数据。
 * 进程退出时自动保存到 /tmp/kumf/spe_self_profile_<pid>.txt
 *
 * 不需要 sudo，不需要 perf 工具。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* spe_self_profile.c 中的函数 */
extern void spe_profile_start(void);
extern void spe_profile_stop(void);

__attribute__((constructor))
static void kumf_init(void) {
    const char *skip = getenv("KUMF_SKIP");
    if (skip && strcmp(skip, "1") == 0) return;

    fprintf(stderr, "[KUMF] SPE self-profiling enabled (no sudo needed)\n");
    fprintf(stderr, "[KUMF] Output: /tmp/kumf/spe_self_profile_%d.txt\n", getpid());
    spe_profile_start();
}

__attribute__((destructor))
static void kumf_fini(void) {
    spe_profile_stop();
    fprintf(stderr, "[KUMF] SPE profiling complete\n");
}
