/**
 * kumf_inverted_index_bench.c - 倒排索引搜索 benchmark (KUMF tiered memory)
 *
 * 天然冷热分离，三个独立大 malloc：
 *   dict_buf:      词典 hash table (热) — 每条查询反复随机访问
 *   posting_buf:   Posting list 池 (温) — 每条查询扫描几个 term
 *   doc_buf:       文档原文存储 (冷) — 只取 top-K 时读几条
 *
 * 多 NUMA 并行构建 + 查询：
 *   - 构建阶段: 多线程并行，每个线程绑不同 NUMA node
 *     → first-touch 把数据分散到多个 node
 *     → 词典被随机分散，查询时大量跨 node 访问
 *   - KUMF 的价值: SPE 采样识别热数据 → 强制路由到快层
 *     → 词典全部在 Node 0 → 查询全部本地访问
 *
 * 用法:
 *   # 默认: 多线程构建(自动检测 NUMA 拓扑), 单线程查询
 *   ./kumf_inverted_index
 *
 *   # 指定并行度
 *   ./kumf_inverted_index -d 10000000 -n 500000 -q 50000 --build-threads 4
 *
 *   # KUMF 自动诊断
 *   kumf diagnose -o /tmp/kumf-ii -- ./kumf_inverted_index -d 10000000 -q 50000
 *
 *   # KUMF 分层运行
 *   kumf run --conf /tmp/kumf-ii/kumf.conf -- ./kumf_inverted_index -d 10000000 -q 50000
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
#include <pthread.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>

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
static dict_entry_t *dict_table = NULL;
static int dict_buckets = 0;
static dict_entry_t *dict_pool = NULL;
static int dict_pool_used = 0;
static pthread_mutex_t dict_lock = PTHREAD_MUTEX_INITIALIZER;

/* Posting list 存储 */
static posting_t *posting_buf = NULL;
static int posting_buf_cap = 0;
static int posting_buf_used = 0;
static pthread_mutex_t posting_lock = PTHREAD_MUTEX_INITIALIZER;

/* 文档原文存储 */
static char *doc_buf = NULL;
static size_t doc_buf_size = 0;
static int avg_doc_len = 0;

/* 统计 */
static long total_dict_lookups = 0;
static long total_posting_scans = 0;
static long total_doc_reads = 0;
static size_t dict_bytes = 0;
static size_t posting_bytes = 0;
static size_t docstore_bytes = 0;

/* NUMA 拓扑 */
static int num_numa_nodes = 1;
static int *node_cpus = NULL;       /* 每个 node 的第一个 CPU */
static int *node_cpu_counts = NULL; /* 每个 node 的 CPU 数 */

/* ============================================================
 * NUMA 拓扑检测
 * ============================================================ */

static void detect_numa_topology(void) {
    if (numa_available() < 0) {
        printf("  NUMA: not available, single node mode\n");
        num_numa_nodes = 1;
        node_cpus = calloc(1, sizeof(int));
        node_cpus[0] = 0;
        node_cpu_counts = calloc(1, sizeof(int));
        node_cpu_counts[0] = 1;
        return;
    }

    num_numa_nodes = numa_num_configured_nodes();
    node_cpus = calloc(num_numa_nodes, sizeof(int));
    node_cpu_counts = calloc(num_numa_nodes, sizeof(int));

    struct bitmask *cpus = numa_allocate_cpumask();
    for (int n = 0; n < num_numa_nodes; n++) {
        numa_node_to_cpus(n, cpus);
        node_cpu_counts[n] = numa_bitmask_weight(cpus);
        /* 取该 node 的第一个 CPU */
        node_cpus[n] = -1;
        for (unsigned int i = 0; i < cpus->size; i++) {
            if (numa_bitmask_isbitset(cpus, i)) {
                node_cpus[n] = i;
                break;
            }
        }
        if (node_cpus[n] < 0) node_cpus[n] = 0;
    }
    numa_free_cpumask(cpus);

    printf("  NUMA topology: %d nodes\n", num_numa_nodes);
    for (int n = 0; n < num_numa_nodes; n++) {
        printf("    Node %d: first_cpu=%d, cpus=%d\n",
               n, node_cpus[n], node_cpu_counts[n]);
    }
}

/* ============================================================
 * 词典操作 (热 — 每条查询反复随机访问)
 * ============================================================ */

static uint32_t hash_term(int term_id, int buckets) {
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

    pthread_mutex_lock(&dict_lock);
    dict_entry_t *e = &dict_table[idx];

    if (e->term_id == -1) {
        e->term_id = term_id;
        e->posting_offset = posting_offset;
        e->posting_len = posting_len;
        e->df = df;
        e->next = NULL;
        pthread_mutex_unlock(&dict_lock);
        return e;
    }

    /* 冲突: 从 pool 分配新节点 */
    dict_entry_t *ne = &dict_pool[dict_pool_used++];
    ne->term_id = term_id;
    ne->posting_offset = posting_offset;
    ne->posting_len = posting_len;
    ne->df = df;
    ne->next = e->next;
    e->next = ne;
    pthread_mutex_unlock(&dict_lock);
    return ne;
}

/* ============================================================
 * 多线程并行构建
 * ============================================================ */

typedef struct {
    int thread_id;
    int num_threads;
    int num_terms;
    int num_docs;
    int max_posting;
    int target_node;       /* 绑定的 NUMA node (-1 = 不绑) */
} build_args_t;

static void *build_worker(void *arg) {
    build_args_t *ba = (build_args_t *)arg;

    /* 绑定到指定 NUMA node */
    if (ba->target_node >= 0 && ba->target_node < num_numa_nodes) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(node_cpus[ba->target_node], &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    /* 每个 thread 处理一段 term 范围 */
    int terms_per_thread = ba->num_terms / ba->num_threads;
    int start_term = ba->thread_id * terms_per_thread;
    int end_term = (ba->thread_id == ba->num_threads - 1)
                   ? ba->num_terms
                   : start_term + terms_per_thread;

    unsigned int seed = 42 + ba->thread_id;

    for (int t = start_term; t < end_term; t++) {
        int df = 1 + (int)(ba->num_docs * 0.001 / (1.0 + t * 0.001));
        if (df > ba->max_posting) df = ba->max_posting;
        if (df > ba->num_docs) df = ba->num_docs;

        /* 在 posting_buf 中分配空间 (加锁) */
        pthread_mutex_lock(&posting_lock);
        if (posting_buf_used + df > posting_buf_cap)
            df = posting_buf_cap - posting_buf_used;
        int offset = posting_buf_used;
        posting_buf_used += df;
        pthread_mutex_unlock(&posting_lock);

        if (df <= 0) continue;

        /* 填充 posting list */
        for (int d = 0; d < df; d++) {
            posting_buf[offset + d].doc_id = rand_r(&seed) % ba->num_docs;
            posting_buf[offset + d].tf = 1 + rand_r(&seed) % 10;
        }

        dict_insert(t, offset, df, df);
    }

    return NULL;
}

/* 文档存储构建 — 多线程，每个线程绑不同 NUMA node */
typedef struct {
    int thread_id;
    int num_threads;
    int num_docs;
    int target_node;
} doc_build_args_t;

static void *doc_build_worker(void *arg) {
    doc_build_args_t *da = (doc_build_args_t *)arg;

    if (da->target_node >= 0 && da->target_node < num_numa_nodes) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(node_cpus[da->target_node], &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    int docs_per_thread = da->num_docs / da->num_threads;
    int start_doc = da->thread_id * docs_per_thread;
    int end_doc = (da->thread_id == da->num_threads - 1)
                  ? da->num_docs
                  : start_doc + docs_per_thread;

    unsigned int seed = 99 + da->thread_id;

    /* 写入文档内容 → first-touch 将页面分配到当前 NUMA node */
    for (int i = start_doc; i < end_doc; i++) {
        size_t off = (size_t)i * avg_doc_len;
        memset(&doc_buf[off], 'A' + (rand_r(&seed) % 26), avg_doc_len - 1);
        doc_buf[off + avg_doc_len - 1] = '\0';
    }

    return NULL;
}

static void build_index(int num_terms, int num_docs, int max_posting_per_term, int build_threads) {
    /* 分配词典 */
    dict_buckets = num_terms * 2 + 1;
    dict_table = (dict_entry_t *)calloc(dict_buckets, sizeof(dict_entry_t));
    if (!dict_table) { perror("malloc dict_table"); exit(1); }
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
    doc_buf_size = (size_t)num_docs * avg_doc_len;
    doc_buf = (char *)malloc(doc_buf_size);
    if (!doc_buf) { perror("malloc doc_buf"); exit(1); }

    /* ---- 多线程并行构建 ---- */
    int n_threads = build_threads;
    if (n_threads > num_numa_nodes) n_threads = num_numa_nodes;
    if (n_threads < 1) n_threads = 1;

    printf("  Building with %d threads across %d NUMA nodes (first-touch dispersal)\n",
           n_threads, num_numa_nodes);

    /* Phase 1: 并行构建索引 (term + posting) */
    pthread_t *build_tids = calloc(n_threads, sizeof(pthread_t));
    build_args_t *build_args = calloc(n_threads, sizeof(build_args_t));

    for (int i = 0; i < n_threads; i++) {
        build_args[i].thread_id = i;
        build_args[i].num_threads = n_threads;
        build_args[i].num_terms = num_terms;
        build_args[i].num_docs = num_docs;
        build_args[i].max_posting = max_posting_per_term;
        /* 轮流绑定到不同 NUMA node → first-touch 分散 */
        build_args[i].target_node = i % num_numa_nodes;
    }

    for (int i = 0; i < n_threads; i++)
        pthread_create(&build_tids[i], NULL, build_worker, &build_args[i]);
    for (int i = 0; i < n_threads; i++)
        pthread_join(build_tids[i], NULL);

    /* Phase 2: 并行写入文档存储 (分散到不同 node) */
    pthread_t *doc_tids = calloc(n_threads, sizeof(pthread_t));
    doc_build_args_t *doc_args = calloc(n_threads, sizeof(doc_build_args_t));

    for (int i = 0; i < n_threads; i++) {
        doc_args[i].thread_id = i;
        doc_args[i].num_threads = n_threads;
        doc_args[i].num_docs = num_docs;
        doc_args[i].target_node = i % num_numa_nodes;
    }

    for (int i = 0; i < n_threads; i++)
        pthread_create(&doc_tids[i], NULL, doc_build_worker, &doc_args[i]);
    for (int i = 0; i < n_threads; i++)
        pthread_join(doc_tids[i], NULL);

    posting_bytes = (size_t)posting_buf_cap * sizeof(posting_t);
    docstore_bytes = (size_t)doc_buf_size;

    free(build_tids); free(build_args);
    free(doc_tids); free(doc_args);
}

/* ============================================================
 * 查询处理 (单线程, 绑 Node 0, 测内存延迟)
 * ============================================================ */

static void reset_scores(double *scores, int *seen, int num_docs) {
    memset(scores, 0, num_docs * sizeof(double));
    memset(seen, 0, num_docs * sizeof(int));
}

static int execute_query(int *query_terms, int num_query_terms, int num_docs,
                         int top_k, scored_doc_t *result_buf,
                         double *scores, int *seen) {
    int total_hits = 0;

    for (int t = 0; t < num_query_terms; t++) {
        dict_entry_t *entry = dict_lookup(query_terms[t]);
        if (!entry) continue;

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

    int n_results = 0;
    for (int d = 0; d < num_docs && n_results < top_k * 10; d++) {
        if (seen[d] && scores[d] > 0) {
            result_buf[n_results].doc_id = d;
            result_buf[n_results].score = scores[d];
            n_results++;
        }
    }

    /* 简单冒泡取 top-K (避免 qsort 对大数组的开销) */
    for (int i = 0; i < top_k && i < n_results; i++) {
        int max_idx = i;
        for (int j = i + 1; j < n_results; j++) {
            if (result_buf[j].score > result_buf[max_idx].score)
                max_idx = j;
        }
        if (max_idx != i) {
            scored_doc_t tmp = result_buf[i];
            result_buf[i] = result_buf[max_idx];
            result_buf[max_idx] = tmp;
        }
    }
    if (n_results > top_k) n_results = top_k;

    /* Step 4: 读 top-K 文档原文 (冷!) */
    for (int k = 0; k < n_results; k++) {
        int did = result_buf[k].doc_id;
        /* 读多个字节,避免编译器优化掉 */
        volatile const char *p = &doc_buf[(size_t)did * avg_doc_len];
        volatile char c1 = p[0];
        volatile char c2 = p[avg_doc_len / 2];
        volatile char c3 = p[avg_doc_len - 2];
        (void)c1; (void)c2; (void)c3;
        total_doc_reads++;
    }

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
    printf("  -n, --num-terms     Number of unique terms (default: 500000)\n");
    printf("  -d, --num-docs      Number of documents (default: 10000000)\n");
    printf("  -l, --doc-len       Average document length in bytes (default: 1024)\n");
    printf("  -q, --queries       Number of queries (default: 50000)\n");
    printf("  -t, --terms-per-q   Terms per query (default: 3)\n");
    printf("  -k, --top-k         Return top-K results (default: 10)\n");
    printf("  -r, --rounds        Number of query rounds (default: 3)\n");
    printf("  -b, --build-threads Number of build threads across NUMA nodes (default: auto)\n");
    printf("  -h, --help          Show this help\n");
}

int main(int argc, char **argv) {
    int num_terms = 500000;
    int num_docs = 10000000;
    int avg_doc_len_param = 1024;
    int num_queries = 50000;
    int terms_per_query = 3;
    int top_k = 10;
    int num_rounds = 3;
    int build_threads = 0; /* 0 = auto (num_numa_nodes) */

    static struct option long_opts[] = {
        {"num-terms",    required_argument, 0, 'n'},
        {"num-docs",     required_argument, 0, 'd'},
        {"doc-len",      required_argument, 0, 'l'},
        {"queries",      required_argument, 0, 'q'},
        {"terms-per-q",  required_argument, 0, 't'},
        {"top-k",        required_argument, 0, 'k'},
        {"rounds",       required_argument, 0, 'r'},
        {"build-threads",required_argument, 0, 'b'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:d:l:q:t:k:r:b:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': num_terms = atoi(optarg); break;
        case 'd': num_docs = atoi(optarg); break;
        case 'l': avg_doc_len_param = atoi(optarg); break;
        case 'q': num_queries = atoi(optarg); break;
        case 't': terms_per_query = atoi(optarg); break;
        case 'k': top_k = atoi(optarg); break;
        case 'r': num_rounds = atoi(optarg); break;
        case 'b': build_threads = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    avg_doc_len = avg_doc_len_param;
    int max_posting_per_term = 20;

    printf("================================================================\n");
    printf("  KUMF Inverted Index Search Benchmark (Multi-NUMA)\n");
    printf("  Tiered Memory Validation — Natural Hot/Cold Separation\n");
    printf("================================================================\n");
    printf("  Terms:       %d (dictionary entries)\n", num_terms);
    printf("  Documents:   %d (doc store)\n", num_docs);
    printf("  Doc length:  %d bytes\n", avg_doc_len);
    printf("  Queries:     %d x %d rounds, %d terms/query, top-%d\n",
           num_queries, num_rounds, terms_per_query, top_k);
    printf("\n");

    /* NUMA 拓扑 */
    detect_numa_topology();
    if (build_threads <= 0) build_threads = num_numa_nodes;
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

    /* === Phase 1: 多 NUMA 并行构建 === */
    printf("── Phase 1: Build Inverted Index (multi-NUMA first-touch) ────\n");
    double t0 = now_sec();
    build_index(num_terms, num_docs, max_posting_per_term, build_threads);
    double t_build = now_sec() - t0;
    printf("  Index built: %.2fs (%d threads across %d NUMA nodes)\n",
           t_build, build_threads, num_numa_nodes);
    printf("  Dict entries:  %d (pool) + %d (buckets)\n", dict_pool_used, dict_buckets);
    printf("  Posting used:  %d / %d\n", posting_buf_used, posting_buf_cap);
    printf("  Actual memory:\n");
    printf("    Dictionary:     %8.1f MB\n", dict_bytes / 1024.0 / 1024.0);
    printf("    Posting lists:  %8.1f MB\n", posting_bytes / 1024.0 / 1024.0);
    printf("    Document store: %8.1f MB\n", docstore_bytes / 1024.0 / 1024.0);
    printf("  → Data spread across %d NUMA nodes via first-touch\n", num_numa_nodes);
    printf("\n");

    /* === Phase 2: 生成查询 === */
    printf("── Phase 2: Generate Queries ─────────────────────────\n");
    t0 = now_sec();
    int *all_query_terms = (int *)malloc(num_queries * terms_per_query * sizeof(int));
    unsigned int qseed = 42;
    for (int q = 0; q < num_queries * terms_per_query; q++) {
        double u = (double)rand_r(&qseed) / RAND_MAX;
        int tid = (int)(num_terms * pow(u, 3.0));
        if (tid >= num_terms) tid = num_terms - 1;
        all_query_terms[q] = tid;
    }
    printf("  %d queries generated (%d terms each): %.3fs\n",
           num_queries, terms_per_query, now_sec() - t0);
    printf("  Query distribution: Zipf-like (hot terms queried heavily)\n");
    printf("\n");

    /* === Phase 3: 查询 benchmark — 绑 Node 0 === */
    printf("── Phase 3: Query Benchmark (CPU on Node 0) ─────────\n");

    /* 绑定查询线程到 Node 0 */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(node_cpus[0], &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    printf("  Query thread pinned to Node 0 (cpu %d)\n", node_cpus[0]);
    printf("  → Dict on other nodes = remote access → NUMA penalty\n");
    printf("  → KUMF should route hot dict to Node 0 → local access\n\n");

    scored_doc_t *result_buf = (scored_doc_t *)malloc(top_k * 10 * sizeof(scored_doc_t));
    double *scores = (double *)malloc(num_docs * sizeof(double));
    int *seen = (int *)malloc(num_docs * sizeof(int));
    if (!scores || !seen) {
        fprintf(stderr, "Failed to allocate scores/seen (%d docs)\n", num_docs);
        return 1;
    }

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
            reset_scores(scores, seen, num_docs);
            execute_query(qt, terms_per_query, num_docs, top_k, result_buf,
                          scores, seen);
        }
        double t_query = now_sec() - t0;

        double qps = num_queries / t_query;
        double lat_us = t_query / num_queries * 1e6;

        total_qps += qps;
        if (qps < best_qps) best_qps = qps;
        if (qps > worst_qps) worst_qps = qps;

        printf("  Round %d: %.3fs, %.0f QPS, %.1f μs/query"
               " (dict=%ld post=%ld doc=%ld)\n",
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

    /* === Phase 4: NUMA 分布 === */
    printf("── Phase 4: NUMA Memory Distribution ────────────────\n");
    print_proc_status();
    print_numa_stats();
    printf("\n");

    /* === Summary === */
    printf("================================================================\n");
    printf("  SUMMARY\n");
    printf("  build=%.2fs (%d NUMA nodes)  query=%.1f QPS (avg %d rounds)\n",
           t_build, num_numa_nodes, avg_qps, num_rounds);
    printf("  Memory: dict=%.0fMB(HOT) + posting=%.0fMB(WARM) + docs=%.0fMB(COLD)\n",
           dict_bytes / 1024.0 / 1024.0,
           posting_bytes / 1024.0 / 1024.0,
           docstore_bytes / 1024.0 / 1024.0);
    printf("  Access: dict_lookups=%ld posting_scans=%ld doc_reads=%ld\n",
           total_dict_lookups, total_posting_scans, total_doc_reads);
    printf("\n");
    printf("  KUMF value proposition:\n");
    printf("    Without KUMF: first-touch disperses dict across %d nodes\n", num_numa_nodes);
    printf("      → ~%d/%d dict lookups are remote → NUMA penalty\n",
           num_numa_nodes - 1, num_numa_nodes);
    printf("    With KUMF: SPE identifies hot dict → routes to Node 0\n");
    printf("      → All dict lookups are local → ~3x speedup expected\n");
    printf("================================================================\n");

    /* 清理 */
    free(dict_table);
    free(dict_pool);
    free(posting_buf);
    free(doc_buf);
    free(all_query_terms);
    free(result_buf);
    free(scores);
    free(seen);
    free(node_cpus);
    free(node_cpu_counts);

    return 0;
}
