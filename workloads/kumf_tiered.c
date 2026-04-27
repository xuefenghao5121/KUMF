/*
 * kumf_tiered.c — KUMF 冷热分层验证 Workload
 *
 * 模拟真实应用的冷热数据访问模式：
 *   热数据 (hot): coordinates/forces — 每步全量读写，频繁随机访问
 *   冷数据 (cold): topology/masses  — 只读，顺序访问
 *
 * SOAR 期望行为：
 *   - prof 拦截 → 记录 hot/cold 的 malloc 地址和大小
 *   - SPE 分析 → hot 数组 page 的 AOL 分数高 → FAST tier
 *   - interc 路由 → hot 分配到 Node 0, cold 分配到 Node 2
 *   - 结果：接近 "全快层" 的性能
 *
 * 编译:
 *   gcc -O2 -o kumf_tiered kumf_tiered.c -lm -lnuma
 *
 * 运行:
 *   # 全快层 (上限)
 *   numactl --cpunodebind=0 --membind=0 ./kumf_tiered
 *   # 全慢层 (下限)
 *   numactl --cpunodebind=0 --membind=2 ./kumf_tiered
 *   # SOAR 分层 (interc 路由)
 *   KUMF_CONF=kumf_tiered.conf LD_PRELOAD=../src/lib/interc/ldlib.so ./kumf_tiered
 *   # first-touch (默认)
 *   numactl --cpunodebind=0 ./kumf_tiered
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>

#define NS_PER_SEC 1e9
#define N_TYPES 10

static double get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

int main(int argc, char *argv[]) {
    /* 默认参数：热数据 200MB + 冷数据 100MB = 300MB 总量 */
    size_t hot_mb  = 200;   /* 超过 L2 cache */
    size_t cold_mb = 100;
    int n_steps = 5;        /* 重复访问次数 */

    if (argc > 1) hot_mb  = atol(argv[1]);
    if (argc > 2) cold_mb = atol(argv[2]);
    if (argc > 3) n_steps = atoi(argv[3]);

    size_t hot_bytes  = hot_mb * 1024 * 1024;
    size_t cold_bytes = cold_mb * 1024 * 1024;
    size_t hot_n  = hot_bytes / sizeof(double);
    size_t cold_n = cold_bytes / sizeof(double);

    printf("============================================================\n");
    printf("  KUMF Tiered Memory Workload\n");
    printf("  热数据: %zu MB (%zu doubles) — 频繁随机读写\n", hot_mb, hot_n);
    printf("  冷数据: %zu MB (%zu doubles) — 只读顺序访问\n", cold_mb, cold_n);
    printf("  步数: %d\n", n_steps);

    if (numa_available() >= 0) {
        int node = -1;
        get_mempolicy(&node, NULL, 0, NULL, 0);
        printf("  NUMA: %d nodes, current policy: %s\n",
               numa_max_node() + 1, node >= 0 ? "bound" : "default");
    }
    printf("============================================================\n\n");

    /* ====== 分配：用 malloc 让 numactl/interc 控制 NUMA 位置 ====== */
    /* 
     * 关键：hot 和 cold 是独立的 malloc 调用
     * prof 会记录两次 malloc 的地址和大小
     * interc 可以根据调用者地址或大小路由到不同 NUMA node
     */

    /* 热数据：coordinates + forces (Vec3 模拟, stride-3) */
    double *coordinates = (double *)malloc(hot_bytes);
    double *forces      = (double *)malloc(hot_bytes);

    /* 冷数据：masses + topology */
    double *masses      = (double *)malloc(cold_bytes);
    double *topology    = (double *)malloc(N_TYPES * sizeof(double));  /* 很小 */

    /* 随机索引 (用于热数据的随机访问) */
    size_t *rand_idx = (size_t *)malloc(hot_n * sizeof(size_t));
    unsigned int seed = 42;
    for (size_t i = 0; i < hot_n; i++) {
        rand_idx[i] = rand_r(&seed) % hot_n;
    }

    /* 初始化 */
    for (size_t i = 0; i < hot_n; i++) {
        coordinates[i] = 1.0 + (double)(i % 100) / 100.0;
        forces[i] = 0.0;
    }
    for (size_t i = 0; i < cold_n; i++) {
        masses[i] = 1.0 + (double)(i % 5) * 0.2;
    }
    for (int i = 0; i < N_TYPES; i++) {
        topology[i] = 0.5 + 0.1 * i;
    }

    /* NUMA 位置确认 */
    printf("--- NUMA Distribution ---\n");
    void *objs[] = {coordinates, forces, masses, topology};
    const char *names[] = {"coordinates[HOT]", "forces[HOT]", "masses[COLD]", "topology[COLD]"};
    for (int i = 0; i < 4; i++) {
        int node = -1;
        get_mempolicy(&node, NULL, 0, objs[i], MPOL_F_ADDR);
        printf("  %-20s on Node %d  (%zu MB)\n",
               names[i], node,
               (i < 2) ? hot_mb : (i == 2 ? cold_mb : 0));
    }
    printf("\n");

    /* ====== 核心循环：模拟 MD 步进 ====== */
    double total_hot_read = 0, total_hot_write = 0;
    double total_cold_read = 0;

    double t_total_start = get_time_ns();

    for (int step = 0; step < n_steps; step++) {
        /* --- 热数据访问 (频繁随机 + 顺序) --- */

        /* 1. 随机读 coordinates (模拟 neighbor list 查找) */
        double t0 = get_time_ns();
        double sum = 0;
        for (size_t i = 0; i < hot_n; i++) {
            sum += coordinates[rand_idx[i]];
        }
        double t1 = get_time_ns();
        total_hot_read += t1 - t0;

        /* 2. 写 forces (模拟力更新) */
        t0 = get_time_ns();
        for (size_t i = 0; i < hot_n - 2; i += 3) {
            double fx = coordinates[i]   * 0.01;
            double fy = coordinates[i+1] * 0.01;
            double fz = coordinates[i+2] * 0.01;
            forces[i]   += fx;
            forces[i+1] += fy;
            forces[i+2] += fz;
        }
        t1 = get_time_ns();
        total_hot_write += t1 - t0;

        /* --- 冷数据访问 (只读顺序，访问频率低) --- */

        /* 3. 读 masses (模拟积分，每步一次顺序遍历) */
        t0 = get_time_ns();
        sum = 0;
        for (size_t i = 0; i < cold_n; i++) {
            sum += masses[i];
        }
        t1 = get_time_ns();
        total_cold_read += t1 - t0;

        /* 偶尔访问 topology (很小，几乎不影响) */
        for (int i = 0; i < N_TYPES; i++) {
            sum += topology[i];
        }

        printf("  Step %d/%d: hot_read=%.1fms hot_write=%.1fms cold_read=%.1fms [sum=%.0f]\n",
               step + 1, n_steps,
               (t1 - t0) / 1e6,  /* 这里用最后一次的值 */
               0.0, 0.0, sum);
    }

    double t_total_end = get_time_ns();
    double total_sec = (t_total_end - t_total_start) / NS_PER_SEC;

    /* ====== 结果汇总 ====== */
    printf("\n============================================================\n");
    printf("  结果汇总 (%d steps)\n", n_steps);
    printf("  总耗时:        %.3f 秒\n", total_sec);
    printf("  热数据读:      %.1f ms (随机读 coordinates)\n", total_hot_read / 1e6);
    printf("  热数据写:      %.1f ms (顺序写 forces)\n", total_hot_write / 1e6);
    printf("  冷数据读:      %.1f ms (顺序读 masses)\n", total_cold_read / 1e6);
    printf("  热数据占比:    %.1f%%\n",
           (total_hot_read + total_hot_write) / (total_hot_read + total_hot_write + total_cold_read) * 100);

    /* 性能指标 */
    double hot_read_bw = (hot_bytes / 1e9 * n_steps) / (total_hot_read / NS_PER_SEC);
    double hot_write_bw = (hot_bytes / 1e9 * n_steps) / (total_hot_write / NS_PER_SEC);
    double cold_read_bw = (cold_bytes / 1e9 * n_steps) / (total_cold_read / NS_PER_SEC);

    printf("\n  带宽:\n");
    printf("    热数据读 BW:  %.2f GB/s\n", hot_read_bw);
    printf("    热数据写 BW:  %.2f GB/s\n", hot_write_bw);
    printf("    冷数据读 BW:  %.2f GB/s\n", cold_read_bw);

    printf("\n  SOAR 期望:\n");
    printf("    全快层: 热数据读 BW 最高 (参考值)\n");
    printf("    全慢层: 热数据读 BW 最低 (参考值)\n");
    printf("    SOAR:   热数据在快层 → 接近全快层性能\n");
    printf("============================================================\n");

    free(coordinates);
    free(forces);
    free(masses);
    free(topology);
    free(rand_idx);
    return 0;
}
