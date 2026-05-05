/**
 * kumf_inverted_index_bench.c - 倒排索引搜索 benchmark (KUMF tiered memory)
 *
 * 模拟真实搜索引擎: 多 NUMA 并行构建 + 多线程并行查询
 *
 * 冷热分离:
 *   dict_buf:      词典 hash table (热) — 每条查询反复随机访问
 *   posting_buf:   Posting list 池 (温) — 每条查询扫描几个 term
 *   doc_buf:       文档原文存储 (冷) — 只取 top-K 时读几条
 *
 * KUMF 价值场景:
 *   构建: 多线程跨 4 个 NUMA node 并行 → first-touch 分散数据
 *   查询: 多线程跨 Socket 0 并行
 *
 *   无 KUMF: 词典被 first-touch 分散到 4 个 node
 *     → ~一半词典查找跨 socket (距离 35-40) → 慢 3x
 *   有 KUMF: 词典+posting 路由到 Socket 0, 文档存储路由到 Socket 1
 *     → 词典查找全在同 socket (距离 12) → 快
 *     → 文档读取跨 socket 但极少 → 可接受
 *
 * 用法:
 *   ./kumf_inverted_index -d 10000000 -n 500000 -q 50000
 *   kumf diagnose -o /tmp/kumf-ii -- ./kumf_inverted_index -d 10000000 -q 50000
 *   kumf run --conf /tmp/kumf-ii/kumf.conf -- ./kumf_inverted_index ...
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
#include <stdatomic.h>

/* ============================================================
 * 数据结构
 * ============================================================ */

typedef struct dict_entry {
    int term_id;
    int posting_offset;
    int posting_len;
    int df;
    struct dict_entry *next;
} dict_entry_t;

typedef struct posting {
    int doc_id;
    int tf;
} posting_t;

typedef struct {
    int doc_id;
    double score;
} scored_doc_t;

/* ============================================================
 * 全局内存区域 — 三个独立大 malloc
 * ============================================================ */

static dict_entry_t *dict_table = NULL;
static int dict_buckets = 0;
static dict_entry_t *dict_pool = NULL;
static int dict_pool_used = 0;
static pthread_mutex_t dict_lock = PTHREAD_MUTEX_INITIALIZER;

static posting_t *posting_buf = NULL;
static int posting_buf_cap = 0;
static int posting_buf_used = 0;
static pthread_mutex_t posting_lock = PTHREAD_MUTEX_INITIALIZER;

static char *doc_buf = NULL;
static size_t doc_buf_size = 0;
static int avg_doc_len = 0;

static size_t dict_bytes = 0;
static size_t posting_bytes = 0;
static size_t docstore_bytes = 0;

/* 统计 (per-thread, 避免 false sharing) */
typedef struct {
    long dict_lookups;
    long posting_scans;
    long doc_reads;
    long queries_done;
    double elapsed;
} thread_stats_t;

static thread_stats_t *thread_stats = NULL;

/* NUMA 拓扑 */
static int num_numa_nodes = 1;
static int num_sockets = 1;
static int nodes_per_socket = 1;  /* 鲲鹏930: 2 nodes per socket */
static int *node_first_cpu = NULL;

/* ============================================================
 * NUMA 拓扑检测
 * ============================================================ */

static void detect_numa_topology(void) {
    if (numa_available() < 0) {
        num_numa_nodes = 1; num_sockets = 1; nodes_per_socket = 1;
        node_first_cpu = calloc(1, sizeof(int));
        return;
    }

    num_numa_nodes = numa_num_configured_nodes();
    node_first_cpu = calloc(num_numa_nodes, sizeof(int));

    struct bitmask *cpus = numa_allocate_cpumask();
    for (int n = 0; n < num_numa_nodes; n++) {
        numa_node_to_cpus(n, cpus);
        node_first_cpu[n] = -1;
        for (unsigned int i = 0; i < cpus->size; i++) {
            if (numa_bitmask_isbitset(cpus, i)) {
                node_first_cpu[n] = (int)i;
                break;
            }
        }
        if (node_first_cpu[n] < 0) node_first_cpu[n] = 0;
    }
    numa_free_cpumask(cpus);

    /* 鲲鹏930: 4 nodes, 2 per socket (node 0+1 = socket 0, node 2+3 = socket 1) */
    /* 通过 NUMA distance 检测 socket 归属 */
    nodes_per_socket = 1;
    if (num_numa_nodes >= 4) {
        /* 检查 node 0 和 node 1 的距离, 如果 < 跨 socket 则同一 socket */
        int d01 = numa_distance(0, 1);
        int d02 = numa_distance(0, 2);
        if (d01 > 0 && d02 > 0 && d01 < d02) {
            nodes_per_socket = 2;
        }
    }
    num_sockets = num_numa_nodes / nodes_per_socket;

    printf("  NUMA: %d nodes, %d sockets, %d nodes/socket\n",
           num_numa_nodes, num_sockets, nodes_per_socket);
    for (int n = 0; n < num_numa_nodes; n++) {
        int sock = n / nodes_per_socket;
        printf("    Node %d: cpu_start=%d, socket=%d\n",
               n, node_first_cpu[n], sock);
    }
}

/* 获取 node 所属 socket */
static int node_to_socket(int node) {
    return node / nodes_per_socket;
}

/* ============================================================
 * 词典操作
 * ============================================================ */

static uint32_t hash_term(int term_id, int buckets) {
    uint32_t h = (uint32_t)term_id;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return h % buckets;
}

static dict_entry_t *dict_lookup(int term_id) {
    uint32_t idx = hash_term(term_id, dict_buckets);
    dict_entry_t *e = &dict_table[idx];
    while (e && e->term_id != -1) {
        if (e->term_id == term_id)
            return e;
        if (e->next) e = e->next;
        else return NULL;
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
    int target_node;
} build_args_t;

static void *build_worker(void *arg) {
    build_args_t *ba = (build_args_t *)arg;
    if (ba->target_node >= 0 && ba->target_node < num_numa_nodes) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(node_first_cpu[ba->target_node], &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    int terms_per = ba->num_terms / ba->num_threads;
    int start = ba->thread_id * terms_per;
    int end = (ba->thread_id == ba->num_threads - 1) ? ba->num_terms : start + terms_per;
    unsigned int seed = 42 + ba->thread_id;

    for (int t = start; t < end; t++) {
        int df = 1 + (int)(ba->num_docs * 0.001 / (1.0 + t * 0.001));
        if (df > ba->max_posting) df = ba->max_posting;
        if (df > ba->num_docs) df = ba->num_docs;
        pthread_mutex_lock(&posting_lock);
        if (posting_buf_used + df > posting_buf_cap)
            df = posting_buf_cap - posting_buf_used;
        int offset = posting_buf_used;
        posting_buf_used += df;
        pthread_mutex_unlock(&posting_lock);
        if (df <= 0) continue;
        for (int d = 0; d < df; d++) {
            posting_buf[offset + d].doc_id = rand_r(&seed) % ba->num_docs;
            posting_buf[offset + d].tf = 1 + rand_r(&seed) % 10;
        }
        dict_insert(t, offset, df, df);
    }
    return NULL;
}

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
        CPU_SET(node_first_cpu[da->target_node], &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }
    int docs_per = da->num_docs / da->num_threads;
    int start = da->thread_id * docs_per;
    int end = (da->thread_id == da->num_threads - 1) ? da->num_docs : start + docs_per;
    unsigned int seed = 99 + da->thread_id;
    for (int i = start; i < end; i++) {
        size_t off = (size_t)i * avg_doc_len;
        memset(&doc_buf[off], 'A' + (rand_r(&seed) % 26), avg_doc_len - 1);
        doc_buf[off + avg_doc_len - 1] = '\0';
    }
    return NULL;
}

static void build_index(int num_terms, int num_docs, int max_posting, int build_threads) {
    dict_buckets = num_terms * 2 + 1;
    dict_table = (dict_entry_t *)calloc(dict_buckets, sizeof(dict_entry_t));
    if (!dict_table) { perror("malloc dict"); exit(1); }
    for (int i = 0; i < dict_buckets; i++) dict_table[i].term_id = -1;

    dict_pool = (dict_entry_t *)malloc(num_terms * sizeof(dict_entry_t));
    if (!dict_pool) { perror("malloc dict_pool"); exit(1); }
    dict_pool_used = 0;
    dict_bytes = dict_buckets * sizeof(dict_entry_t) + num_terms * sizeof(dict_entry_t);

    posting_buf_cap = num_terms * max_posting;
    posting_buf = (posting_t *)malloc((size_t)posting_buf_cap * sizeof(posting_t));
    if (!posting_buf) { perror("malloc posting"); exit(1); }
    posting_buf_used = 0;

    doc_buf_size = (size_t)num_docs * avg_doc_len;
    doc_buf = (char *)malloc(doc_buf_size);
    if (!doc_buf) { perror("malloc doc_buf"); exit(1); }

    int n = build_threads;
    if (n > num_numa_nodes) n = num_numa_nodes;
    if (n < 1) n = 1;

    /* 并行构建索引 */
    pthread_t *tids = calloc(n, sizeof(pthread_t));
    build_args_t *bargs = calloc(n, sizeof(build_args_t));
    for (int i = 0; i < n; i++) {
        bargs[i] = (build_args_t){i, n, num_terms, num_docs, max_posting, i % num_numa_nodes};
    }
    for (int i = 0; i < n; i++) pthread_create(&tids[i], NULL, build_worker, &bargs[i]);
    for (int i = 0; i < n; i++) pthread_join(tids[i], NULL);

    /* 并行写入文档存储 */
    doc_build_args_t *dargs = calloc(n, sizeof(doc_build_args_t));
    for (int i = 0; i < n; i++) {
        dargs[i] = (doc_build_args_t){i, n, num_docs, i % num_numa_nodes};
    }
    for (int i = 0; i < n; i++) pthread_create(&tids[i], NULL, doc_build_worker, &dargs[i]);
    for (int i = 0; i < n; i++) pthread_join(tids[i], NULL);

    posting_bytes = (size_t)posting_buf_cap * sizeof(posting_t);
    docstore_bytes = doc_buf_size;
    free(tids); free(bargs); free(dargs);
}

/* ============================================================
 * 多线程并行查询
 * ============================================================ */

typedef struct {
    int thread_id;
    int num_query_threads;
    int target_node;
    int *all_query_terms;
    int num_queries;
    int terms_per_query;
    int num_docs;
    int top_k;
    int num_rounds;
    int *thread_to_cpu;    /* thread_id → CPU 映射 */
} query_args_t;

static void *query_worker(void *arg) {
    query_args_t *qa = (query_args_t *)arg;

    /* 绑定到指定 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(qa->thread_to_cpu[qa->thread_id], &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    /* 每个 thread 独立的 scores/seen buffer (避免 false sharing) */
    double *scores = (double *)malloc(qa->num_docs * sizeof(double));
    int *seen = (int *)malloc(qa->num_docs * sizeof(int));
    scored_doc_t *result_buf = (scored_doc_t *)malloc(qa->top_k * 10 * sizeof(scored_doc_t));
    if (!scores || !seen || !result_buf) {
        fprintf(stderr, "Thread %d: malloc failed\n", qa->thread_id);
        return NULL;
    }

    long my_lookups = 0, my_scans = 0, my_reads = 0, my_queries = 0;

    /* 每个 thread 处理一段查询 (round-robin 分配) */
    int queries_per_thread = qa->num_queries / qa->num_query_threads;
    int my_start = qa->thread_id * queries_per_thread;
    int my_end = (qa->thread_id == qa->num_query_threads - 1)
                 ? qa->num_queries : my_start + queries_per_thread;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int round = 0; round < qa->num_rounds; round++) {
        for (int q = my_start; q < my_end; q++) {
            int *qt = &qa->all_query_terms[q * qa->terms_per_query];
            memset(scores, 0, qa->num_docs * sizeof(double));
            memset(seen, 0, qa->num_docs * sizeof(int));

            for (int t = 0; t < qa->terms_per_query; t++) {
                dict_entry_t *entry = dict_lookup(qt[t]);
                my_lookups++;
                if (!entry) continue;

                double idf = log((double)qa->num_docs / (1 + entry->df));
                for (int p = 0; p < entry->posting_len; p++) {
                    posting_t *pe = &posting_buf[entry->posting_offset + p];
                    int did = pe->doc_id;
                    if (did < 0 || did >= qa->num_docs) continue;
                    scores[did] += pe->tf * idf;
                    seen[did] = 1;
                }
                my_scans += entry->posting_len;
            }

            /* 取 top-K (选择排序) */
            int n_res = 0;
            for (int d = 0; d < qa->num_docs && n_res < qa->top_k * 10; d++) {
                if (seen[d] && scores[d] > 0) {
                    result_buf[n_res].doc_id = d;
                    result_buf[n_res].score = scores[d];
                    n_res++;
                }
            }
            for (int i = 0; i < qa->top_k && i < n_res; i++) {
                int mx = i;
                for (int j = i + 1; j < n_res; j++)
                    if (result_buf[j].score > result_buf[mx].score) mx = j;
                if (mx != i) { scored_doc_t tmp = result_buf[i]; result_buf[i] = result_buf[mx]; result_buf[mx] = tmp; }
            }
            if (n_res > qa->top_k) n_res = qa->top_k;

            /* 读 top-K 文档原文 (冷) */
            for (int k = 0; k < n_res; k++) {
                int did = result_buf[k].doc_id;
                volatile const char *p = &doc_buf[(size_t)did * avg_doc_len];
                (void)p[0]; (void)p[avg_doc_len/2]; (void)p[avg_doc_len-2];
                my_reads++;
            }
            my_queries++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    /* 写回统计 */
    thread_stats[qa->thread_id].dict_lookups = my_lookups;
    thread_stats[qa->thread_id].posting_scans = my_scans;
    thread_stats[qa->thread_id].doc_reads = my_reads;
    thread_stats[qa->thread_id].queries_done = my_queries;
    thread_stats[qa->thread_id].elapsed = (ts_end.tv_sec - ts_start.tv_sec)
                                         + (ts_end.tv_nsec - ts_start.tv_nsec) * 1e-9;

    free(scores); free(seen); free(result_buf);
    return NULL;
}

/* ============================================================
 * NUMA 统计
 * ============================================================ */

static void print_numa_stats(void) {
    FILE *f = popen("numastat -p $$ 2>/dev/null", "r");
    if (f) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) printf("  %s", buf);
        pclose(f);
    }
}

static void print_proc_status(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "VmRSS:", 6) == 0 || strncmp(buf, "VmHWM:", 6) == 0)
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

int main(int argc, char **argv) {
    int num_terms = 500000;
    int num_docs = 10000000;
    int avg_doc_len_param = 1024;
    int num_queries = 50000;
    int terms_per_query = 3;
    int top_k = 10;
    int num_rounds = 3;
    int build_threads = 0;
    int query_threads = 0;  /* 0 = auto (Socket 0 的 CPU 数) */

    static struct option long_opts[] = {
        {"num-terms",    required_argument, 0, 'n'},
        {"num-docs",     required_argument, 0, 'd'},
        {"doc-len",      required_argument, 0, 'l'},
        {"queries",      required_argument, 0, 'q'},
        {"terms-per-q",  required_argument, 0, 't'},
        {"top-k",        required_argument, 0, 'k'},
        {"rounds",       required_argument, 0, 'r'},
        {"build-threads",required_argument, 0, 'b'},
        {"query-threads",required_argument, 0, 'j'},
        {"help",         no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:d:l:q:t:k:r:b:j:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': num_terms = atoi(optarg); break;
        case 'd': num_docs = atoi(optarg); break;
        case 'l': avg_doc_len_param = atoi(optarg); break;
        case 'q': num_queries = atoi(optarg); break;
        case 't': terms_per_query = atoi(optarg); break;
        case 'k': top_k = atoi(optarg); break;
        case 'r': num_rounds = atoi(optarg); break;
        case 'b': build_threads = atoi(optarg); break;
        case 'j': query_threads = atoi(optarg); break;
        case 'h':
            printf("Usage: %s [options]\n", argv[0]);
            printf("  -n  num-terms     (default 500000)\n");
            printf("  -d  num-docs      (default 10000000)\n");
            printf("  -l  doc-len       (default 1024)\n");
            printf("  -q  queries       (default 50000)\n");
            printf("  -t  terms-per-q   (default 3)\n");
            printf("  -k  top-k         (default 10)\n");
            printf("  -r  rounds        (default 3)\n");
            printf("  -b  build-threads (default: auto=numa_nodes)\n");
            printf("  -j  query-threads (default: auto=socket0_cpus)\n");
            return 0;
        default: return 1;
        }
    }

    avg_doc_len = avg_doc_len_param;
    int max_posting = 20;

    printf("================================================================\n");
    printf("  KUMF Inverted Index Benchmark (Multi-NUMA Realistic)\n");
    printf("================================================================\n");
    printf("  Terms: %d | Docs: %d | DocLen: %d | Queries: %dx%d | Top-%d\n",
           num_terms, num_docs, avg_doc_len, num_queries, num_rounds, top_k);
    printf("\n");

    detect_numa_topology();
    if (build_threads <= 0) build_threads = num_numa_nodes;

    /* 查询线程: Socket 0 上所有 CPU */
    if (query_threads <= 0) {
        /* 计算 Socket 0 的 CPU 数 */
        int s0_cpus = 0;
        struct bitmask *cpus = numa_allocate_cpumask();
        for (int n = 0; n < nodes_per_socket && n < num_numa_nodes; n++) {
            numa_node_to_cpus(n, cpus);
            s0_cpus += numa_bitmask_weight(cpus);
        }
        numa_free_cpumask(cpus);
        query_threads = s0_cpus > 0 ? s0_cpus : 1;
    }

    printf("\n  Build: %d threads across %d NUMA nodes (first-touch dispersal)\n",
           build_threads, num_numa_nodes);
    printf("  Query: %d threads on Socket 0 (same-socket = fast tier)\n",
           query_threads);

    /* 估算内存 */
    size_t est_dict = (size_t)(num_terms * 2 + 1) * sizeof(dict_entry_t) + num_terms * sizeof(dict_entry_t);
    size_t est_posting = (size_t)num_terms * max_posting * sizeof(posting_t);
    size_t est_docs = (size_t)num_docs * avg_doc_len;
    printf("\n  Memory estimate:\n");
    printf("    Dictionary:     %8.1f MB  (HOT)\n", est_dict / 1024.0 / 1024.0);
    printf("    Posting lists:  %8.1f MB  (WARM)\n", est_posting / 1024.0 / 1024.0);
    printf("    Document store: %8.1f MB  (COLD)\n", est_docs / 1024.0 / 1024.0);
    printf("    Total:          %8.1f MB\n", (est_dict + est_posting + est_docs) / 1024.0 / 1024.0);

    const char *ld_preload = getenv("LD_PRELOAD");
    if (ld_preload && strstr(ld_preload, "kumf"))
        printf("\n  KUMF interc:  ✅ %s\n", ld_preload);
    else
        printf("\n  KUMF interc:  ❌ (bare run)\n");
    printf("\n");

    /* === Phase 1: 多 NUMA 并行构建 === */
    printf("── Phase 1: Build Index (multi-NUMA first-touch) ──────────────\n");
    double t0 = now_sec();
    build_index(num_terms, num_docs, max_posting, build_threads);
    double t_build = now_sec() - t0;
    printf("  Built in %.2fs: dict=%dKB posting=%dKB docs=%zuMB\n",
           t_build,
           (int)(dict_bytes/1024), (int)(posting_bytes/1024), docstore_bytes/1024/1024);
    printf("  Data spread across %d NUMA nodes by first-touch\n\n", num_numa_nodes);

    /* === Phase 2: 生成查询 === */
    printf("── Phase 2: Generate Queries ─────────────────────────\n");
    int *all_query_terms = (int *)malloc(num_queries * terms_per_query * sizeof(int));
    unsigned int qseed = 42;
    for (int q = 0; q < num_queries * terms_per_query; q++) {
        double u = (double)rand_r(&qseed) / RAND_MAX;
        int tid = (int)(num_terms * pow(u, 3.0));
        if (tid >= num_terms) tid = num_terms - 1;
        all_query_terms[q] = tid;
    }
    printf("  %d queries (Zipf-like distribution)\n\n", num_queries);

    /* === Phase 3: 多线程并行查询 === */
    printf("── Phase 3: Query Benchmark (%d threads on Socket 0) ─────\n", query_threads);

    /* 构建 thread → CPU 映射 (Socket 0 的 CPU) */
    int *thread_to_cpu = calloc(query_threads, sizeof(int));
    {
        struct bitmask *cpus = numa_allocate_cpumask();
        int ti = 0;
        for (int n = 0; n < nodes_per_socket && n < num_numa_nodes; n++) {
            numa_node_to_cpus(n, cpus);
            for (unsigned int i = 0; i < cpus->size && ti < query_threads; i++) {
                if (numa_bitmask_isbitset(cpus, i)) {
                    thread_to_cpu[ti++] = (int)i;
                }
            }
        }
        numa_free_cpumask(cpus);
        /* 如果 Socket 0 CPU 不够, 补充其他 node */
        while (ti < query_threads) {
            thread_to_cpu[ti] = ti % num_numa_nodes == 0
                                ? node_first_cpu[ti % num_numa_nodes]
                                : thread_to_cpu[ti-1] + 1;
            ti++;
        }
    }

    thread_stats = calloc(query_threads, sizeof(thread_stats_t));
    pthread_t *qtids = calloc(query_threads, sizeof(pthread_t));
    query_args_t *qargs = calloc(query_threads, sizeof(query_args_t));

    for (int i = 0; i < query_threads; i++) {
        qargs[i] = (query_args_t){
            .thread_id = i,
            .num_query_threads = query_threads,
            .target_node = (i < nodes_per_socket) ? i : 0,
            .all_query_terms = all_query_terms,
            .num_queries = num_queries,
            .terms_per_query = terms_per_query,
            .num_docs = num_docs,
            .top_k = top_k,
            .num_rounds = num_rounds,
            .thread_to_cpu = thread_to_cpu,
        };
    }

    t0 = now_sec();
    for (int i = 0; i < query_threads; i++)
        pthread_create(&qtids[i], NULL, query_worker, &qargs[i]);
    for (int i = 0; i < query_threads; i++)
        pthread_join(qtids[i], NULL);
    double t_total = now_sec() - t0;

    /* 汇总统计 */
    long total_lookups = 0, total_scans = 0, total_reads = 0, total_qdone = 0;
    double max_elapsed = 0;
    for (int i = 0; i < query_threads; i++) {
        total_lookups += thread_stats[i].dict_lookups;
        total_scans += thread_stats[i].posting_scans;
        total_reads += thread_stats[i].doc_reads;
        total_qdone += thread_stats[i].queries_done;
        if (thread_stats[i].elapsed > max_elapsed) max_elapsed = thread_stats[i].elapsed;
    }

    double total_qps = total_qdone / max_elapsed;  /* 吞吐量 = 总查询数 / 最慢线程时间 */
    double per_thread_qps = total_qps / query_threads;

    printf("  Wall time:     %.3fs\n", t_total);
    printf("  Slowest thread: %.3fs\n", max_elapsed);
    printf("  Total queries:  %ld (%d rounds x %d queries x %d threads)\n",
           total_qdone, num_rounds, num_queries, query_threads);
    printf("\n");
    printf("  ┌──────────────────────────────────────────────────┐\n");
    printf("  │  Throughput:  %10.0f QPS (aggregate)          │\n", total_qps);
    printf("  │  Per-thread:  %10.0f QPS                      │\n", per_thread_qps);
    printf("  │  Latency:     %10.1f μs/query (per thread)    │\n",
           1e6 / per_thread_qps);
    printf("  └──────────────────────────────────────────────────┘\n");
    printf("\n");
    printf("  Access breakdown:\n");
    printf("    Dict lookups:    %12ld  (HOT — should be same-socket)\n", total_lookups);
    printf("    Posting scans:   %12ld  (WARM — should be same-socket)\n", total_scans);
    printf("    Doc reads:       %12ld  (COLD — cross-socket OK)\n", total_reads);
    printf("\n");

    /* === Phase 4: NUMA 分布 === */
    printf("── Phase 4: NUMA Memory Distribution ────────────────\n");
    print_proc_status();
    print_numa_stats();
    printf("\n");

    /* === Summary === */
    printf("================================================================\n");
    printf("  SUMMARY\n");
    printf("  build=%.2fs  query=%.0f QPS (%d threads x %d rounds)\n",
           t_build, total_qps, query_threads, num_rounds);
    printf("  Memory: dict=%.0fMB(HOT) + posting=%.0fMB(WARM) + docs=%.0fMB(COLD)\n",
           dict_bytes/1024.0/1024.0, posting_bytes/1024.0/1024.0, docstore_bytes/1024.0/1024.0);
    printf("\n");
    printf("  KUMF value (multi-NUMA realistic):\n");
    printf("    First-touch: dict scattered across %d nodes\n", num_numa_nodes);
    printf("      → ~%d%% dict lookups cross-socket (distance %d) → SLOW\n",
           (num_numa_nodes - nodes_per_socket) * 100 / num_numa_nodes,
           numa_distance(0, num_numa_nodes > 1 ? nodes_per_socket : 0));
    printf("    KUMF tiered: dict+posting on Socket 0, docs on Socket 1\n");
    printf("      → ALL dict lookups same-socket (distance %d) → FAST\n",
           num_numa_nodes > 1 ? numa_distance(0, 1) : 10);
    printf("      → doc reads cross-socket but rare → acceptable\n");
    printf("================================================================\n");

    free(dict_table); free(dict_pool); free(posting_buf); free(doc_buf);
    free(all_query_terms); free(thread_stats); free(qtids); free(qargs);
    free(thread_to_cpu); free(node_first_cpu);
    return 0;
}
