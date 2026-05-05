/**
 * kumf_inverted_index_bench.c - 倒排索引搜索 benchmark (KUMF tiered memory)
 *
 * 天然冷热分离，三个独立大 malloc：
 *   dict_buf:      词典 hash table (热) — 每条查询反复随机访问
 *   posting_buf:   Posting list 池 (温) — 每条查询扫描几个 term 的 list
 *   doc_buf:       文档原文存储 (冷) — 只取 top-K 时读几条
 *
 * KUMF 自动 pipeline 应该能：
 *   SPE 采样 → 词典区域高频访问 → 高 PAC → 快层
 *   SPE 采样 → 文档区域几乎无访问 → 低 PAC → 慢层
 *   PAC → interc 配置自动生成
 *
 * 用法:
 *   # 默认: 100万 term, 1000万文档, 1万条查询
 *   ./kumf_inverted_index_bench
 *
 *   # 大数据集 (~10GB 文档存储)
 *   ./kumf_inverted_index_bench -n 10000000 -d 10000000 -l 1024
 *
 *   # KUMF 自动诊断
 *   kumf diagnose -o /tmp/kumf -- ./kumf_inverted_index_bench -n 1000000 -d 10000000 -q 100000
 *
 *   # KUMF 分层运行
 *   kumf run --conf /tmp/kumf/kumf.conf -- ./kumf_inverted_index_bench -n 1000000 -d 10000000 -q 100000
 *
 *   # 对比
 *   kumf bench --conf /tmp/kumf/kumf.conf -- ./kumf_inverted_index_bench [args]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <getopt.h>
#include <math.h>

/* ============================================================
 * 数据结构
 * ============================================================ */

/* 词典条目: term_id → posting list 头 */
typedef struct dict_entry {
    int term_id;
    int posting_offset;    /* posting_buf 中的偏移量 */
    int posting_len;       /* posting list 长度 */
    int df;                /* document frequency */
    struct dict_entry *next; /* hash chain */
} dict_entry_t;

/* Posting list 条目: (doc_id, tf) 对 */
typedef struct posting {
    int doc_id;
    int tf;                /* term frequency in doc */
} posting_t;

/* Top-K 堆 */
typedef struct {
    int doc_id;
    double score;
} scored_doc_t;

/* ============================================================
 * 全局内存区域 — 三个独立大 malloc
 * ============================================================ */

/* 词典 hash table */
static dict_entry_t *dict_table = NULL;  /* hash bucket 数组 */
static int dict_buckets = 0;
static dict_entry_t *dict_pool = NULL;   /* 预分配 dict_entry 池 */
static int dict_pool_used = 0;

/* Posting list 存储 */
static posting_t *posting_buf = NULL;
static int posting_buf_cap = 0;
static int posting_buf_used = 0;

/* 文档原文存储 */
static char *doc_buf = NULL;
static int doc_buf_size = 0;
static int avg_doc_len = 0;

/* 统计 */
static long total_dict_lookups = 0;
static long total_posting_scans = 0;
static long total_doc_reads = 0;
static size_t dict_bytes = 0;
static size_t posting_bytes = 0;
static size_t docstore_bytes = 0;

/* ============================================================
 * 词典操作 (热 — 每条查询反复随机访问)
 * ============================================================ */

static uint32_t hash_term(int term_id, int buckets) {
    /* 简单但分散的 hash */
    uint32_t h = (uint32_t)term_id;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return h % buckets;
}

static dict_entry_t *dict_lookup(int term_id) {
    total_dict_lookups++;
    uint32_t idx = hash_term(term_id, dict_buckets);
    dict_entry_t *e = &dict_table[idx];
    /* 沿 hash chain 查找 */
    while (e && e->term_id != -1) {
        if (e->term_id == term_id)
            return e;
        if (e->next)
            e = e->next;
        else
            return NULL;
    }
    return NULL;
}

static dict_entry_t *dict_insert(int term_id, int posting_offset, int posting_len, int df) {
    uint32_t idx = hash_term(term_id, dict_buckets);
    dict_entry_t *e = &dict_table[idx];

    if (e->term_id == -1) {
        /* 空 bucket */
        e->term_id = term_id;
        e->posting_offset = posting_offset;
        e->posting_len = posting_len;
        e->df = df;
        e->next = NULL;
        return e;
    }

    /* 冲突: 从 pool 分配新节点，插到链表头 */
    dict_entry_t *ne = &dict_pool[dict_pool_used++];
    ne->term_id = term_id;
    ne->posting_offset = posting_offset;
    ne->posting_len = posting_len;
    ne->df = df;
    ne->next = e->next;
    e->next = ne; /* 插到第二个位置（保持 bucket head 不变） */
    return ne;
}

/* ============================================================
 * 索引构建
 * ============================================================ */

static void build_index(int num_terms, int num_docs, int max_posting_per_term) {
    /* 分配词典 hash table */
    dict_buckets = num_terms * 2 + 1;
    dict_table = (dict_entry_t *)calloc(dict_buckets, sizeof(dict_entry_t));
    if (!dict_table) { perror("malloc dict_table"); exit(1); }
    /* 初始化为空 */
    for (int i = 0; i < dict_buckets; i++)
        dict_table[i].term_id = -1;

    dict_pool = (dict_entry_t *)malloc(num_terms * sizeof(dict_entry_t));
    if (!dict_pool) { perror("malloc dict_pool"); exit(1); }
    dict_pool_used = 0;

    dict_bytes = dict_buckets * sizeof(dict_entry_t) + num_terms * sizeof(dict_entry_t);

    /* 分配 posting buffer */
    posting_buf_cap = num_terms * max_posting_per_term;
    posting_buf = (posting_t *)malloc((size_t)posting_buf_cap * sizeof(posting_t));
    if (!posting_buf) { perror("malloc posting_buf"); exit(1); }
    posting_buf_used = 0;

    /* 分配文档存储 */
    doc_buf_size = num_docs * avg_doc_len;
    doc_buf = (char *)malloc(doc_buf_size);
    if (!doc_buf) { perror("malloc doc_buf"); exit(1); }
    /* 填充假文档内容 */
    memset(doc_buf, 'A' + (rand() % 26), doc_buf_size);
    /* 每个 doc 末尾加 \0 */
    for (int i = 0; i < num_docs; i++)
        doc_buf[i * avg_doc_len + avg_doc_len - 1] = '\0';

    /* 生成倒排索引 */
    srand(42);
    for (int t = 0; t < num_terms; t++) {
        /* 每个 term 的 posting list 长度: Zipf-like 分布 */
        int df = 1 + (int)(num_docs * 0.001 / (1.0 + t * 0.001));
        if (df > max_posting_per_term) df = max_posting_per_term;
        if (df > num_docs) df = num_docs;
        if (posting_buf_used + df > posting_buf_cap) df = posting_buf_cap - posting_buf_used;
        if (df <= 0) continue;

        int offset = posting_buf_used;
        for (int d = 0; d < df; d++) {
            /* 随机 doc_id, 避免重复 (简化: 随机但不严格去重) */
            posting_buf[posting_buf_used].doc_id = rand() % num_docs;
            posting_buf[posting_buf_used].tf = 1 + rand() % 10;
            posting_buf_used++;
        }

        dict_insert(t, offset, df, df);
    }

    posting_bytes = (size_t)posting_buf_cap * sizeof(posting_t);
    docstore_bytes = (size_t)doc_buf_size;
}

/* ============================================================
 * 查询处理
 * ============================================================ */

static int score_cmp(const void *a, const void *b) {
    double sa = ((scored_doc_t *)a)->score;
    double sb = ((scored_doc_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

/**
 * 执行一条查询: 查词典 → 扫描 posting → 评分 → 取 top-K 文档原文
 *
 * @param query_terms  查询中的 term_id 数组
 * @param num_terms    查询 term 数
 * @param num_docs     总文档数
 * @param top_k        返回 top-K
 * @param result_buf   结果缓冲区
 * @return             命中文档数
 */
static int execute_query(int *query_terms, int num_query_terms, int num_docs,
                         int top_k, scored_doc_t *result_buf) {
    /* 临时评分数组 (栈上分配, 小量) */
    double *scores = (double *)calloc(num_docs, sizeof(double));
    if (!scores) return 0;

    int *seen = (int *)calloc(num_docs, sizeof(int));
    if (!seen) { free(scores); return 0; }

    int total_hits = 0;

    for (int t = 0; t < num_query_terms; t++) {
        /* Step 1: 词典查找 (热!) */
        dict_entry_t *entry = dict_lookup(query_terms[t]);
        if (!entry) continue;

        /* Step 2: 扫描 posting list (温) */
        double idf = log((double)num_docs / (1 + entry->df));
        for (int p = 0; p < entry->posting_len; p++) {
            posting_t *pe = &posting_buf[entry->posting_offset + p];
            int did = pe->doc_id;
            if (did < 0 || did >= num_docs) continue;
            scores[did] += pe->tf * idf;
            if (!seen[did]) {
                seen[did] = 1;
                total_hits++;
            }
        }
        total_posting_scans += entry->posting_len;
    }

    /* Step 3: 收集评分 > 0 的文档, 排序取 top-K */
    int n_results = 0;
    for (int d = 0; d < num_docs && n_results < top_k * 10; d++) {
        if (seen[d] && scores[d] > 0) {
            result_buf[n_results].doc_id = d;
            result_buf[n_results].score = scores[d];
            n_results++;
        }
    }

    qsort(result_buf, n_results, sizeof(scored_doc_t), score_cmp);
    if (n_results > top_k)
        n_results = top_k;

    /* Step 4: 读 top-K 文档原文 (冷!) */
    for (int k = 0; k < n_results; k++) {
        int did = result_buf[k].doc_id;
        /* 实际读文档内容 — 触发 doc_buf 的内存访问 */
        volatile char c = doc_buf[did * avg_doc_len];
        (void)c;
        total_doc_reads++;
    }

    free(scores);
    free(seen);
    return n_results;
}

/* ============================================================
 * NUMA 统计
 * ============================================================ */

static void print_numa_stats(void) {
    FILE *f = popen("numastat -p $$ 2>/dev/null", "r");
    if (f) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f))
            printf("  %s", buf);
        pclose(f);
    }
}

static void print_proc_status(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "VmRSS:", 6) == 0 ||
                strncmp(buf, "VmHWM:", 6) == 0 ||
                strncmp(buf, "VmSize:", 7) == 0)
                printf("  %s", buf);
        }
        fclose(f);
    }
}

/* ============================================================
 * 主函数
 * ============================================================ */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -n, --num-terms     Number of unique terms in dictionary (default: 1000000)\n");
    printf("  -d, --num-docs      Number of documents (default: 10000000)\n");
    printf("  -l, --doc-len       Average document length in bytes (default: 1024)\n");
    printf("  -q, --queries       Number of queries to execute (default: 10000)\n");
    printf("  -t, --terms-per-q   Terms per query (default: 3)\n");
    printf("  -k, --top-k         Return top-K results (default: 10)\n");
    printf("  -r, --rounds        Number of query rounds (default: 3)\n");
    printf("  -h, --help          Show this help\n");
    printf("\n");
    printf("Memory estimates (default params):\n");
    printf("  Dictionary:     ~%zu MB\n", (size_t)(2000000 * sizeof(dict_entry_t)) / 1024 / 1024);
    printf("  Posting lists:  ~%zu MB\n", (size_t)(1000000 * 20 * sizeof(posting_t)) / 1024 / 1024);
    printf("  Document store: ~%zu MB\n", (size_t)10000000 * 1024 / 1024 / 1024);
}

int main(int argc, char **argv) {
    int num_terms = 1000000;
    int num_docs = 10000000;
    int avg_doc_len_param = 1024;
    int num_queries = 10000;
    int terms_per_query = 3;
    int top_k = 10;
    int num_rounds = 3;

    static struct option long_opts[] = {
        {"num-terms",    required_argument, 0, 'n'},
        {"num-docs",     required_argument, 0, 'd'},
        {"doc-len",      required_argument, 0, 'l'},
        {"queries",      required_argument, 0, 'q'},
        {"terms-per-q",  required_argument, 0, 't'},
        {"top-k",        required_argument, 0, 'k'},
        {"rounds",       required_argument, 0, 'r'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:d:l:q:t:k:r:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': num_terms = atoi(optarg); break;
        case 'd': num_docs = atoi(optarg); break;
        case 'l': avg_doc_len_param = atoi(optarg); break;
        case 'q': num_queries = atoi(optarg); break;
        case 't': terms_per_query = atoi(optarg); break;
        case 'k': top_k = atoi(optarg); break;
        case 'r': num_rounds = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    avg_doc_len = avg_doc_len_param;
    int max_posting_per_term = 20; /* 每个 term 最多 20 个 posting */

    printf("================================================================\n");
    printf("  KUMF Inverted Index Search Benchmark\n");
    printf("  Tiered Memory Validation — Natural Hot/Cold Separation\n");
    printf("================================================================\n");
    printf("  Terms:       %d (dictionary entries)\n", num_terms);
    printf("  Documents:   %d (doc store)\n", num_docs);
    printf("  Doc length:  %d bytes\n", avg_doc_len);
    printf("  Queries:     %d x %d rounds, %d terms/query, top-%d\n",
           num_queries, num_rounds, terms_per_query, top_k);
    printf("\n");

    /* 估算内存 */
    size_t est_dict = (size_t)(num_terms * 2 + 1) * sizeof(dict_entry_t) + num_terms * sizeof(dict_entry_t);
    size_t est_posting = (size_t)num_terms * max_posting_per_term * sizeof(posting_t);
    size_t est_docs = (size_t)num_docs * avg_doc_len;
    size_t est_total = est_dict + est_posting + est_docs;

    printf("  Estimated memory:\n");
    printf("    Dictionary:     %8.1f MB  (HOT — every query hits)\n", est_dict / 1024.0 / 1024.0);
    printf("    Posting lists:  %8.1f MB  (WARM — scan per term)\n", est_posting / 1024.0 / 1024.0);
    printf("    Document store: %8.1f MB  (COLD — only read top-K docs)\n", est_docs / 1024.0 / 1024.0);
    printf("    Total:          %8.1f MB\n", est_total / 1024.0 / 1024.0);
    printf("\n");

    /* KUMF 环境检测 */
    const char *ld_preload = getenv("LD_PRELOAD");
    const char *kumf_conf = getenv("KUMF_CONF");
    if (ld_preload && strstr(ld_preload, "kumf")) {
        printf("  KUMF interc:  ✅ %s\n", ld_preload);
        if (kumf_conf)
            printf("  KUMF conf:    %s\n", kumf_conf);
    } else {
        printf("  KUMF interc:  ❌ (bare run, no tiered memory)\n");
    }
    printf("\n");

    /* === Phase 1: 构建索引 === */
    printf("── Phase 1: Build Inverted Index ─────────────────────\n");
    double t0 = now_sec();
    build_index(num_terms, num_docs, max_posting_per_term);
    double t_build = now_sec() - t0;
    printf("  Index built: %.2fs\n", t_build);
    printf("  Dict entries used: %d (pool) + %d (buckets)\n", dict_pool_used, dict_buckets);
    printf("  Posting entries:   %d / %d\n", posting_buf_used, posting_buf_cap);
    printf("  Actual memory:\n");
    printf("    Dictionary:     %8.1f MB\n", dict_bytes / 1024.0 / 1024.0);
    printf("    Posting lists:  %8.1f MB\n", posting_bytes / 1024.0 / 1024.0);
    printf("    Document store: %8.1f MB\n", docstore_bytes / 1024.0 / 1024.0);
    printf("\n");

    /* === Phase 2: 生成查询 === */
    printf("── Phase 2: Generate Queries ─────────────────────────\n");
    t0 = now_sec();
    int *all_query_terms = (int *)malloc(num_queries * terms_per_query * sizeof(int));
    for (int q = 0; q < num_queries * terms_per_query; q++) {
        /* Zipf-like: 少量热 term 被大量查询, 大量冷 term 很少被查 */
        /* 这和搜索引擎的真实分布一致 */
        double u = (double)rand() / RAND_MAX;
        int tid = (int)(num_terms * pow(u, 3.0)); /* u^3 偏向小 term_id */
        if (tid >= num_terms) tid = num_terms - 1;
        all_query_terms[q] = tid;
    }
    double t_qgen = now_sec() - t0;
    printf("  %d queries generated (%d terms each): %.3fs\n", num_queries, terms_per_query, t_qgen);
    printf("  Query distribution: Zipf-like (hot terms queried heavily)\n");
    printf("\n");

    /* === Phase 3: 查询 benchmark === */
    printf("── Phase 3: Query Benchmark ─────────────────────────\n");

    scored_doc_t *result_buf = (scored_doc_t *)malloc(top_k * 10 * sizeof(scored_doc_t));

    double total_qps = 0;
    double best_qps = 1e18;
    double worst_qps = 0;

    for (int round = 0; round < num_rounds; round++) {
        total_dict_lookups = 0;
        total_posting_scans = 0;
        total_doc_reads = 0;

        t0 = now_sec();
        for (int q = 0; q < num_queries; q++) {
            int *qt = &all_query_terms[q * terms_per_query];
            execute_query(qt, terms_per_query, num_docs, top_k, result_buf);
        }
        double t_query = now_sec() - t0;

        double qps = num_queries / t_query;
        double lat_us = t_query / num_queries * 1e6;

        total_qps += qps;
        if (qps < best_qps) best_qps = qps;
        if (qps > worst_qps) worst_qps = qps;

        printf("  Round %d: %.3fs, %.0f QPS, %.1f μs/query"
               " (dict=%ld post=%ld doc_read=%ld)\n",
               round + 1, t_query, qps, lat_us,
               total_dict_lookups, total_posting_scans, total_doc_reads);
    }

    double avg_qps = total_qps / num_rounds;
    double avg_lat = num_queries / avg_qps / num_queries * 1e6;

    printf("\n");
    printf("  ┌──────────────────────────────────────────────┐\n");
    printf("  │  Avg QPS:       %10.0f                  │\n", avg_qps);
    printf("  │  Avg latency:   %10.1f μs                │\n", avg_lat);
    printf("  │  Best QPS:      %10.0f                  │\n", worst_qps);
    printf("  │  Worst QPS:     %10.0f                  │\n", best_qps);
    printf("  └──────────────────────────────────────────────┘\n");
    printf("\n");

    /* === Phase 4: 内存 / NUMA 统计 === */
    printf("── Phase 4: Memory & NUMA Statistics ────────────────\n");
    print_proc_status();
    print_numa_stats();
    printf("\n");

    /* === Summary === */
    printf("================================================================\n");
    printf("  SUMMARY\n");
    printf("  build=%.2fs  query=%.1f QPS (avg over %d rounds)\n",
           t_build, avg_qps, num_rounds);
    printf("  Memory: dict=%.0fMB(HOT) + posting=%.0fMB(WARM) + docs=%.0fMB(COLD)\n",
           dict_bytes / 1024.0 / 1024.0,
           posting_bytes / 1024.0 / 1024.0,
           docstore_bytes / 1024.0 / 1024.0);
    printf("  Access: dict_lookups=%ld posting_scans=%ld doc_reads=%ld\n",
           total_dict_lookups, total_posting_scans, total_doc_reads);
    printf("  Expected KUMF behavior:\n");
    printf("    SPE → dict region: HIGH frequency → HIGH PAC → fast tier (Node 0)\n");
    printf("    SPE → posting region: MEDIUM frequency → MEDIUM PAC\n");
    printf("    SPE → doc_store region: LOW frequency → LOW PAC → slow tier (Node 2)\n");
    printf("================================================================\n");

    /* 清理 */
    free(dict_table);
    free(dict_pool);
    free(posting_buf);
    free(doc_buf);
    free(all_query_terms);
    free(result_buf);

    return 0;
}
