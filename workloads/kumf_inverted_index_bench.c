/*
 * kumf_inverted_index_bench.c - 倒排索引搜索 benchmark (KUMF tiered memory)
 *
 * 天然冷热分离:
 *   词典 (dictionary):    HOT — 每条查询必查，随机访问
 *   Posting list pool:     WARM — 每条查询读几个 term 的 list
 *   文档原文 (doc_store):  COLD — 只取 top-K 时才读，极少访问
 *
 * 三个区域是三个独立的 malloc，interc 按 size 路由:
 *   词典 (~200MB)  → 快层 (Node 0)
 *   Posting (~2GB)  → 温层 (first-touch)
 *   文档 (~10GB)    → 慢层 (Node 2)
 *
 * 用法:
 *   make workloads    # 编译
 *
 *   # 默认: 1千万文档, 1KB/文档 (~10GB doc_store)
 *   ./build/kumf_inverted_index_bench
 *
 *   # 小规模测试
 *   ./build/kumf_inverted_index_bench 1000000 100000 1000
 *
 *   # 对比: 全快层
 *   numactl --membind=0 ./build/kumf_inverted_index_bench
 *
 *   # 对比: KUMF 分层
 *   KUMF_CONF=kumf_inverted.conf LD_PRELOAD=build/libkumf_interc.so \
 *     ./build/kumf_inverted_index_bench
 *
 * interc 配置 (kumf_inverted.conf):
 *   # 词典 (较小的大块) → 快层
 *   size_range:67108864-536870912 = 0
 *   # 文档原文 (最大块) → 慢层
 *   size_gt:536870912 = 2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

/* ============================================================
 * 配置
 * ============================================================ */

#define DEFAULT_NUM_DOCS      10000000   /* 1千万文档 */
#define DEFAULT_NUM_TERMS     1000000    /* 100万词 */
#define DEFAULT_NUM_QUERIES   10000      /* 1万条查询 */
#define DEFAULT_TOP_K         10
#define AVG_DOC_LEN           100        /* 平均每文档 100 个词 */
#define DOC_CONTENT_LEN       1024       /* 每文档 1KB 原文 */
#define HASH_TABLE_LOAD       0.7        /* hash table 负载因子 */
#define POSTING_AVG_LEN       50         /* 平均 posting list 长度 */

/* ============================================================
 * 简易 hash table (开放寻址)
 * ============================================================ */

typedef struct {
    int64_t key;          /* term ID */
    int     posting_off;  /* posting list 在 pool 中的偏移 */
    int     posting_len;  /* posting list 长度 */
    int     doc_freq;     /* 文档频率 (DF) */
} dict_entry_t;

typedef struct {
    dict_entry_t *entries;
    int64_t       capacity;
    int64_t       size;
    int64_t       mask;
} dict_t;

static dict_t *dict_create(int64_t capacity) {
    dict_t *d = (dict_t *)malloc(sizeof(dict_t));
    int64_t real_cap = 1;
    while (real_cap < (int64_t)(capacity / HASH_TABLE_LOAD))
        real_cap *= 2;
    d->entries = (dict_entry_t *)calloc(real_cap, sizeof(dict_entry_t));
    d->capacity = real_cap;
    d->size = 0;
    d->mask = real_cap - 1;
    /* 标记空槽: key = -1 */
    for (int64_t i = 0; i < real_cap; i++)
        d->entries[i].key = -1;
    return d;
}

static void dict_insert(dict_t *d, int64_t key, int off, int len, int df) {
    int64_t idx = (key * 2654435761ULL) & d->mask;
    while (d->entries[idx].key != -1) {
        idx = (idx + 1) & d->mask;
    }
    d->entries[idx].key = key;
    d->entries[idx].posting_off = off;
    d->entries[idx].posting_len = len;
    d->entries[idx].doc_freq = df;
    d->size++;
}

static dict_entry_t *dict_lookup(dict_t *d, int64_t key) {
    int64_t idx = (key * 2654435761ULL) & d->mask;
    while (d->entries[idx].key != -1) {
        if (d->entries[idx].key == key)
            return &d->entries[idx];
        idx = (idx + 1) & d->mask;
    }
    return NULL;
}

/* ============================================================
 * 查询结果 (top-K)
 * ============================================================ */

typedef struct {
    int    doc_id;
    double score;
} result_t;

/* ============================================================
 * 时间工具
 * ============================================================ */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ============================================================
 * 主逻辑
 * ============================================================ */

int main(int argc, char *argv[]) {
    int64_t num_docs    = (argc > 1) ? atoll(argv[1]) : DEFAULT_NUM_DOCS;
    int64_t num_terms   = (argc > 2) ? atoll(argv[2]) : DEFAULT_NUM_TERMS;
    int64_t num_queries = (argc > 3) ? atoll(argv[3]) : DEFAULT_NUM_QUERIES;
    int     top_k       = (argc > 4) ? atoi(argv[4])  : DEFAULT_TOP_K;

    printf("============================================================\n");
    printf("  KUMF Inverted Index Search Benchmark\n");
    printf("  Tiered Memory Validation\n");
    printf("============================================================\n");
    printf("  Documents:     %lld\n", (long long)num_docs);
    printf("  Terms:         %lld\n", (long long)num_terms);
    printf("  Queries:       %lld\n", (long long)num_queries);
    printf("  Top-K:         %d\n", top_k);
    printf("\n");

    /* ---- 估算内存 ---- */
    int64_t dict_bytes = (int64_t)(num_terms / HASH_TABLE_LOAD) * sizeof(dict_entry_t);
    int64_t posting_bytes = num_terms * POSTING_AVG_LEN * sizeof(int);
    int64_t doc_store_bytes = num_docs * DOC_CONTENT_LEN;

    double dict_mb = dict_bytes / (1024.0 * 1024.0);
    double posting_mb = posting_bytes / (1024.0 * 1024.0);
    double doc_mb = doc_store_bytes / (1024.0 * 1024.0);
    double total_mb = dict_mb + posting_mb + doc_mb;

    printf("  Memory estimate:\n");
    printf("    Dictionary:    %8.1f MB  (HOT  — 每条查询随机访问)\n", dict_mb);
    printf("    Posting pool:  %8.1f MB  (WARM — 顺序扫描几个 term)\n", posting_mb);
    printf("    Doc store:     %8.1f MB  (COLD — 只取 top-K 时读)\n", doc_mb);
    printf("    Total:         %8.1f MB\n", total_mb);
    printf("\n");

    /* KUMF 环境检测 */
    const char *kumf_conf = getenv("KUMF_CONF");
    const char *ld_preload = getenv("LD_PRELOAD");
    if (kumf_conf || (ld_preload && strstr(ld_preload, "kumf"))) {
        printf("  KUMF interc:  ✅ enabled\n");
        if (kumf_conf) printf("  KUMF conf:    %s\n", kumf_conf);
    } else {
        printf("  KUMF interc:  ❌ (bare run, no tiered memory)\n");
    }
    printf("\n");

    /* ---- Phase 1: 构建词典 + posting list ---- */
    printf("── Phase 1: Build Dictionary + Posting Lists ──────\n");
    double t0 = now_sec();

    /* 1a. 创建词典 */
    dict_t *dict = dict_create(num_terms);

    /* 1b. 分配 posting pool */
    int *posting_pool = (int *)malloc(posting_bytes);
    if (!posting_pool) {
        fprintf(stderr, "ERROR: failed to allocate posting pool (%lld bytes)\n",
                (long long)posting_bytes);
        return 1;
    }

    /* 1c. 填充词典和 posting list (模拟真实分布: Zipf) */
    srand(42);
    int posting_offset = 0;

    for (int64_t t = 0; t < num_terms; t++) {
        /* Zipf 分布: 高频词的 posting list 更长 */
        double zipf = 1.0 / pow((double)(t + 1), 0.8);
        int list_len = (int)(POSTING_AVG_LEN * zipf * num_terms / (num_terms * 0.5));
        if (list_len < 1) list_len = 1;
        if (list_len > num_docs) list_len = (int)num_docs;
        if (posting_offset + list_len > posting_bytes / (int)sizeof(int)) {
            list_len = (posting_bytes / (int)sizeof(int)) - posting_offset;
            if (list_len <= 0) break;
        }

        /* 填充 posting list (随机文档 ID，已排序) */
        int *list = posting_pool + posting_offset;
        for (int i = 0; i < list_len; i++) {
            list[i] = (int)(rand() % num_docs);
        }
        /* posting list in real search engine is sorted, simulation ok without */

        dict_insert(dict, t, posting_offset, list_len, list_len);
        posting_offset += list_len;
    }

    double t_build_dict = now_sec() - t0;
    printf("  Dictionary: %lld entries, %.1f MB\n", (long long)dict->size, dict_mb);
    printf("  Posting pool: %d entries used, %.1f MB\n", posting_offset, posting_mb);
    printf("  Build time: %.2fs\n", t_build_dict);
    printf("\n");

    /* ---- Phase 2: 构建文档存储 ---- */
    printf("── Phase 2: Build Document Store ───────────────────\n");
    t0 = now_sec();

    char *doc_store = (char *)malloc(doc_store_bytes);
    if (!doc_store) {
        fprintf(stderr, "ERROR: failed to allocate doc store (%lld bytes)\n",
                (long long)doc_store_bytes);
        return 1;
    }

    /* 填充模拟文档内容 (随机字节) */
    for (int64_t i = 0; i < doc_store_bytes; i++) {
        doc_store[i] = (char)('A' + (rand() % 26));
    }

    double t_build_docs = now_sec() - t0;
    printf("  Doc store: %lld docs x %d bytes = %.1f MB\n",
           (long long)num_docs, DOC_CONTENT_LEN, doc_mb);
    printf("  Build time: %.2fs\n", t_build_docs);
    printf("\n");

    /* ---- Phase 3: 查询 benchmark ---- */
    printf("── Phase 3: Query Benchmark ───────────────────────\n");

    /* 生成随机查询 (每个查询 2-5 个 term) */
    int *query_terms = (int *)malloc(num_queries * 5 * sizeof(int));
    int *query_lens = (int *)malloc(num_queries * sizeof(int));
    for (int64_t q = 0; q < num_queries; q++) {
        query_lens[q] = 2 + (rand() % 4);  /* 2-5 terms per query */
        for (int t = 0; t < query_lens[q]; t++) {
            /* 查询偏向高频词 (真实搜索模式) */
            query_terms[q * 5 + t] = (int)(rand() % (num_terms / 10));
        }
    }

    /* Warmup */
    int warmup = 100;
    for (int q = 0; q < warmup && q < num_queries; q++) {
        for (int t = 0; t < query_lens[q]; t++) {
            int64_t tid = query_terms[q * 5 + t];
            dict_entry_t *e = dict_lookup(dict, tid);
            if (e) {
                volatile int *p = posting_pool + e->posting_off;
                (void)p[0];
            }
        }
    }

    /* 正式查询: 3 轮取稳定结果 */
    int rounds = 3;
    double latencies[3];
    int64_t total_docs_retrieved = 0;

    printf("  Running %d rounds x %lld queries...\n", rounds, (long long)num_queries);

    for (int r = 0; r < rounds; r++) {
        t0 = now_sec();
        total_docs_retrieved = 0;

        for (int64_t q = 0; q < num_queries; q++) {
            /* Step 1: 词典查找 (HOT — 随机访问) */
            result_t results[64];
            int n_results = 0;

            for (int t = 0; t < query_lens[q]; t++) {
                int64_t tid = query_terms[q * 5 + t];
                dict_entry_t *e = dict_lookup(dict, tid);
                if (!e) continue;

                /* Step 2: 扫描 posting list (WARM — 顺序读) */
                int *list = posting_pool + e->posting_off;
                for (int i = 0; i < e->posting_len && n_results < 64; i++) {
                    int doc_id = list[i];
                    /* TF-IDF 简易评分 */
                    double tf = 1.0;
                    double idf = log((double)num_docs / (e->doc_freq + 1));
                    double score = tf * idf;

                    /* 插入结果 (简单实现) */
                    int inserted = 0;
                    for (int j = 0; j < n_results; j++) {
                        if (results[j].doc_id == doc_id) {
                            results[j].score += score;
                            inserted = 1;
                            break;
                        }
                    }
                    if (!inserted && n_results < 64) {
                        results[n_results].doc_id = doc_id;
                        results[n_results].score = score;
                        n_results++;
                    }
                }
            }

            /* Step 3: 排序取 top-K */
            for (int i = 0; i < n_results - 1; i++) {
                for (int j = i + 1; j < n_results; j++) {
                    if (results[j].score > results[i].score) {
                        result_t tmp = results[i];
                        results[i] = results[j];
                        results[j] = tmp;
                    }
                }
            }

            /* Step 4: 读取 top-K 文档原文 (COLD — 极少访问) */
            for (int i = 0; i < top_k && i < n_results; i++) {
                int doc_id = results[i].doc_id;
                if (doc_id >= 0 && doc_id < num_docs) {
                    char *doc = doc_store + (int64_t)doc_id * DOC_CONTENT_LEN;
                    /* 模拟读取: 访问前 64 字节 (标题/摘要) */
                    volatile char first_char = doc[0];
                    (void)first_char;
                    total_docs_retrieved++;
                }
            }
        }

        latencies[r] = now_sec() - t0;
    }

    double total_time = 0;
    double best_time = 1e9, worst_time = 0;
    for (int r = 0; r < rounds; r++) {
        double qps = num_queries / latencies[r];
        double lat_us = latencies[r] / num_queries * 1e6;
        printf("    Round %d: %.3fs, %.0f QPS, %.1f μs/query\n",
               r + 1, latencies[r], qps, lat_us);
        total_time += latencies[r];
        if (latencies[r] < best_time) best_time = latencies[r];
        if (latencies[r] > worst_time) worst_time = latencies[r];
    }

    double avg_time = total_time / rounds;
    double avg_qps = num_queries / avg_time;
    double avg_lat_us = avg_time / num_queries * 1e6;

    printf("\n");
    printf("  ┌──────────────────────────────────────────┐\n");
    printf("  │  Avg QPS:       %10.0f               │\n", avg_qps);
    printf("  │  Avg latency:   %10.1f μs            │\n", avg_lat_us);
    printf("  │  Best QPS:      %10.0f               │\n", num_queries / best_time);
    printf("  │  Worst QPS:     %10.0f               │\n", num_queries / worst_time);
    printf("  │  Docs retrieved: %10lld             │\n", (long long)total_docs_retrieved);
    printf("  └──────────────────────────────────────────┘\n");
    printf("\n");

    /* ---- Phase 4: NUMA 分布 ---- */
    printf("── Phase 4: NUMA Memory Distribution ──────────────\n");
    {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "numastat -p %d 2>/dev/null", getpid());
        int ret = system(cmd);
        if (ret != 0) {
            printf("  (numastat not available)\n");
        }
    }

    /* /proc/self/status */
    {
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "VmRSS:", 6) == 0 ||
                    strncmp(line, "VmHWM:", 6) == 0) {
                    printf("  %s", line);
                }
            }
            fclose(f);
        }
    }
    printf("\n");

    /* ---- Summary ---- */
    printf("============================================================\n");
    printf("  SUMMARY\n");
    printf("  build_dict=%.2fs  build_docs=%.2fs\n", t_build_dict, t_build_docs);
    printf("  query=%.3fs (%.0f QPS)\n", avg_time, avg_qps);
    printf("  Memory: dict=%.0fMB(HOT) + posting=%.0fMB(WARM) + docs=%.0fMB(COLD)\n",
           dict_mb, posting_mb, doc_mb);
    printf("  Routing: dict→快层(small big malloc) | docs→慢层(biggest malloc)\n");
    printf("============================================================\n");

    /* ---- 输出 interc 配置建议 ---- */
    printf("\n");
    printf("── Suggested interc config (kumf_inverted.conf) ────\n");
    printf("# 词典 (%.0fMB) → 快层\n", dict_mb);
    if (dict_bytes < (1LL << 30)) {
        printf("size_range:%lld-%lld = 0\n",
               (long long)dict_bytes,
               (long long)(dict_bytes * 2));
    } else {
        printf("# 词典 > 1GB, use size_range to match\n");
    }
    printf("# 文档原文 (%.0fMB) → 慢层\n", doc_mb);
    printf("size_gt:%lld = 2\n", (long long)(doc_store_bytes / 2));
    printf("\n");

    /* 清理 */
    free(doc_store);
    free(posting_pool);
    free(dict->entries);
    free(dict);
    free(query_terms);
    free(query_lens);

    return 0;
}
