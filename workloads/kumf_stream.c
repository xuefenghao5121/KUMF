/*
 * kumf_stream.c — KUMF NUMA 延迟微基准
 *
 * 专门测试跨 NUMA 内存访问延迟，强制数据越出 L2 cache (160MB)
 * 
 * 用法:
 *   numactl --cpunodebind=0 --membind=0 ./kumf_stream          # 全快层
 *   numactl --cpunodebind=0 --membind=2 ./kumf_stream          # 全慢层
 *   numactl --cpunodebind=0 ./kumf_stream                      # first-touch
 *   ./kumf_stream 200                                          # 200MB 数据
 *
 * 测试模式:
 *   1. 顺序读 (Sequential Read) — 测带宽
 *   2. 随机读 (Random Read) — 测延迟
 *   3. 顺序写 (Sequential Write) — 测带宽
 *   4. 模拟 MD 热数据访问模式 (stride-3 读) — 最贴近 mini_md
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>

#define NS_PER_SEC 1e9

static double get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

int main(int argc, char *argv[]) {
    /* 默认 256MB，超过鲲鹏930 L2 cache (160MB) */
    size_t size_mb = 256;
    if (argc > 1) {
        size_mb = atol(argv[1]);
        if (size_mb < 10) {
            fprintf(stderr, "数据量至少 10MB\n");
            return 1;
        }
    }

    size_t bytes = size_mb * 1024 * 1024;
    size_t n_doubles = bytes / sizeof(double);
    int num_runs = 3;  // 每个测试跑3次取最好

    printf("============================================================\n");
    printf("  KUMF NUMA Stream Benchmark\n");
    printf("  数据量: %zu MB (%zu doubles)\n", size_mb, n_doubles);
    printf("  NUMA nodes: %d\n", numa_num_configured_nodes());
    
    /* 打印当前 NUMA 绑定 */
    int cpu_node = -1, mem_node = -1;
    if (getcpu(&cpu_node, NULL) == 0) {
        /* 检查内存策略 */
        int policy = -1;
        get_mempolicy(&policy, NULL, 0, 0, 0);
        const char *pol_str = "default";
        switch (policy) {
            case MPOL_DEFAULT:  pol_str = "default(first-touch)"; break;
            case MPOL_BIND:     pol_str = "bind"; break;
            case MPOL_INTERLEAVE: pol_str = "interleave"; break;
            case MPOL_PREFERRED: pol_str = "preferred"; break;
        }
        printf("  CPU node: %d | MEM policy: %s\n", cpu_node, pol_str);
    }
    printf("============================================================\n\n");

    /* 分配数组 — 用 malloc 让 numactl --membind 完全控制分配位置 */
    double *A = (double *)malloc(bytes);
    double *B = (double *)malloc(bytes);
    if (!A || !B) {
        fprintf(stderr, "分配失败 (%zu MB)\n", size_mb);
        return 1;
    }

    /* 初始化 */
    for (size_t i = 0; i < n_doubles; i++) {
        A[i] = 1.0 + (double)(i % 100) / 100.0;
        B[i] = 2.0 + (double)(i % 100) / 100.0;
    }

    /* ====== Test 1: 顺序读 ====== */
    {
        double best_time = 1e18;
        double sum = 0;
        for (int run = 0; run < num_runs; run++) {
            sum = 0;
            double t0 = get_time_ns();
            for (size_t i = 0; i < n_doubles; i++) {
                sum += A[i];
            }
            double t1 = get_time_ns();
            if ((t1 - t0) < best_time) best_time = t1 - t0;
        }
        double sec = best_time / NS_PER_SEC;
        double bw = (bytes / 1e9) / sec;  // GB/s
        printf("[1] 顺序读:       %8.2f ms | BW: %6.2f GB/s | sum=%.1f\n",
               sec * 1000, bw, sum);
    }

    /* ====== Test 2: 随机读 ====== */
    {
        /* 生成随机索引 */
        size_t *indices = (size_t *)malloc(n_doubles * sizeof(size_t));
        unsigned int seed = 42;
        for (size_t i = 0; i < n_doubles; i++) {
            indices[i] = rand_r(&seed) % n_doubles;
        }

        double best_time = 1e18;
        double sum = 0;
        for (int run = 0; run < num_runs; run++) {
            sum = 0;
            double t0 = get_time_ns();
            for (size_t i = 0; i < n_doubles; i++) {
                sum += A[indices[i]];
            }
            double t1 = get_time_ns();
            if ((t1 - t0) < best_time) best_time = t1 - t0;
        }
        double sec = best_time / NS_PER_SEC;
        double latency_ns = (sec * NS_PER_SEC) / n_doubles;
        printf("[2] 随机读:       %8.2f ms | 平均延迟: %6.1f ns | sum=%.1f\n",
               sec * 1000, latency_ns, sum);
        free(indices);
    }

    /* ====== Test 3: 顺序写 ====== */
    {
        double best_time = 1e18;
        for (int run = 0; run < num_runs; run++) {
            double t0 = get_time_ns();
            for (size_t i = 0; i < n_doubles; i++) {
                B[i] = (double)i * 0.5;
            }
            double t1 = get_time_ns();
            if ((t1 - t0) < best_time) best_time = t1 - t0;
        }
        double sec = best_time / NS_PER_SEC;
        double bw = (bytes / 1e9) / sec;
        printf("[3] 顺序写:       %8.2f ms | BW: %6.2f GB/s\n",
               sec * 1000, bw);
    }

    /* ====== Test 4: 模拟 MD 热数据 (stride-3 读, 模拟 x/y/z 坐标) ====== */
    {
        double best_time = 1e18;
        double sum = 0;
        for (int run = 0; run < num_runs; run++) {
            sum = 0;
            double t0 = get_time_ns();
            /* stride-3 访问，模拟 Vec3 坐标遍历 */
            for (size_t i = 0; i < n_doubles - 2; i += 3) {
                sum += A[i] + A[i+1] + A[i+2];  // x + y + z
            }
            double t1 = get_time_ns();
            if ((t1 - t0) < best_time) best_time = t1 - t0;
        }
        double sec = best_time / NS_PER_SEC;
        double bw = (bytes / 1e9) / sec;
        printf("[4] MD-stride读:  %8.2f ms | BW: %6.2f GB/s | sum=%.1f\n",
               sec * 1000, bw, sum);
    }

    /* ====== Test 5: 模拟 MD force 更新 (读坐标+写力) ====== */
    {
        double best_time = 1e18;
        for (int run = 0; run < num_runs; run++) {
            double t0 = get_time_ns();
            for (size_t i = 0; i < n_doubles - 2; i += 3) {
                double fx = A[i]   * 0.01;
                double fy = A[i+1] * 0.01;
                double fz = A[i+2] * 0.01;
                B[i]   += fx;
                B[i+1] += fy;
                B[i+2] += fz;
            }
            double t1 = get_time_ns();
            if ((t1 - t0) < best_time) best_time = t1 - t0;
        }
        double sec = best_time / NS_PER_SEC;
        double bw = (2 * bytes / 1e9) / sec;  // 读+写
        printf("[5] MD读写:       %8.2f ms | BW: %6.2f GB/s (R+W)\n",
               sec * 1000, bw);
    }

    /* ====== Test 6: 指针追逐 (Pointer Chase) — 纯延迟测试 ====== */
    {
        /* 构建链表式指针追逐，每次访问依赖上一次结果 */
        size_t *next = (size_t *)malloc(n_doubles * sizeof(size_t));
        /* 随机排列 */
        for (size_t i = 0; i < n_doubles; i++) next[i] = i;
        unsigned int seed = 123;
        for (size_t i = n_doubles - 1; i > 0; i--) {
            size_t j = rand_r(&seed) % (i + 1);
            size_t tmp = next[i]; next[i] = next[j]; next[j] = tmp;
        }
        /* next[i] 现在是随机排列；构建 chase 链 */
        size_t *chase = (size_t *)malloc(n_doubles * sizeof(size_t));
        for (size_t i = 0; i < n_doubles; i++) {
            chase[next[i]] = next[(i + 1) % n_doubles];
        }
        free(next);

        size_t n_chases = n_doubles;
        double best_time = 1e18;
        volatile size_t idx = 0;
        for (int run = 0; run < num_runs; run++) {
            idx = 0;
            double t0 = get_time_ns();
            for (size_t i = 0; i < n_chases; i++) {
                idx = chase[idx];
            }
            double t1 = get_time_ns();
            if ((t1 - t0) < best_time) best_time = t1 - t0;
        }
        double sec = best_time / NS_PER_SEC;
        double latency_ns = (sec * NS_PER_SEC) / n_chases;
        printf("[6] 指针追逐:    %8.2f ms | 平均延迟: %6.1f ns | final=%zu\n",
               sec * 1000, latency_ns, idx);
        free(chase);
    }

    printf("\n============================================================\n");
    printf("  对比方法: 在不同 NUMA 绑定下运行，对比延迟/带宽差异\n");
    printf("  期望: 同socket(快) vs 跨socket(慢) 差异 20-40%%\n");
    printf("============================================================\n");

    free(A);
    free(B);
    return 0;
}
