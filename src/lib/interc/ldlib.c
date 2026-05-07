/*
 * KUMF interc v2 — NUMA-aware thread affinity + optional allocation routing
 *
 * Two-layer design:
 *   Layer 1 (Basic, no KUMF_CONF):
 *     - Bind main thread to ALL CPUs in configured nodes (parent process)
 *     - Children inherit CPU affinity via clone/fork — NO per-thread work
 *     - NO set_mempolicy — first-touch naturally places pages on local node
 *     - NO malloc/calloc/realloc/free interception
 *     - Effect = exactly numactl --cpunodebind, via LD_PRELOAD
 *     - ZERO ongoing overhead (one-time sched_setaffinity at init)
 *
 *   Layer 2 (Enhanced, KUMF_CONF has routing rules):
 *     - Per-thread binding + set_mempolicy for precise data placement
 *     - Intercepted pthread_create with round-robin/compact spread
 *     - Config-driven allocation routing on matched allocs
 *
 * Env vars:
 *   KUMF_CONF      - Config file path (triggers Layer 2 when set)
 *   KUMF_AFFINITY  - Thread affinity: auto|compact|off (default: auto)
 *                    Layer 1: auto = bind once; Layer 2: auto = round-robin
 *   KUMF_NODES     - Comma-separated node list (default: auto-detect all)
 *   KUMF_POLICY    - Memory policy (Layer 2 only): preferred|bind|interleave
 *   KUMF_DEBUG     - Enable debug logging (any value = on)
 *   KUMF_DAEMON    - Socket path for daemon communication
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define __USE_GNU
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <new>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include <cerrno>
#include <fcntl.h>

/* ================================================================
 * Constants
 * ================================================================ */
#define MAX_ADDR_RULES 8192
#define MAX_NAME_RULES 256
#define MAX_FUNC_NAME 256
#define ADDR_HASH_BITS 16
#define ADDR_HASH_SIZE (1 << ADDR_HASH_BITS)
#define ADDR_HASH_MASK (ADDR_HASH_SIZE - 1)
#define ROUTE_THRESHOLD 4096
#define MAX_NUMA_NODES 128
#define KUMF_SOCK_PATH "/tmp/kumf_daemon.sock"

/* Memory policy enum (Layer 2 only) */
enum kumf_mpolicy {
    KUMF_MPOL_PREFERRED = 0,
    KUMF_MPOL_BIND = 1,
    KUMF_MPOL_INTERLEAVE = 2,
};

/* Thread spread strategy (Layer 2 only) */
enum kumf_spread {
    KUMF_SPREAD_ROUNDROBIN = 0,
    KUMF_SPREAD_COMPACT = 1,
};

/* ================================================================
 * libc function pointers
 * ================================================================ */
static void *(*libc_malloc)(size_t);
static void *(*libc_calloc)(size_t, size_t);
static void *(*libc_realloc)(void *, size_t);
static void (*libc_free)(void *);
static int (*libc_pthread_create)(pthread_t *, const pthread_attr_t *,
                                   void *(*)(void *), void *);

/* ================================================================
 * Address hash map for numa_alloc tracking (Layer 2 only)
 * ================================================================ */
struct addr_entry {
    unsigned long addr;
    size_t size;
    struct addr_entry *next;
};
static struct addr_entry *addr_hash[ADDR_HASH_SIZE];

static inline unsigned addr_hash_fn(unsigned long addr) {
    return (addr >> 12) & ADDR_HASH_MASK;
}

static void record_seg(unsigned long addr, size_t size) {
    struct addr_entry *e = (struct addr_entry *)libc_malloc(sizeof(*e));
    if (!e) return;
    e->addr = addr;
    e->size = size;
    unsigned h = addr_hash_fn(addr);
    e->next = addr_hash[h];
    addr_hash[h] = e;
}

static size_t check_and_remove_seg(unsigned long addr) {
    unsigned h = addr_hash_fn(addr);
    struct addr_entry **pp = &addr_hash[h];
    while (*pp) {
        struct addr_entry *e = *pp;
        if (e->addr == addr) {
            size_t sz = e->size;
            *pp = e->next;
            libc_free(e);
            return sz;
        }
        pp = &e->next;
    }
    return 0;
}

/* ================================================================
 * Config rules (Layer 2 — from KUMF_CONF file)
 * ================================================================ */
struct addr_rule {
    unsigned long start;
    unsigned long end;
    int node;
};

struct name_rule {
    char pattern[MAX_FUNC_NAME];
    int node;
};

struct size_rule {
    size_t min_size;
    size_t max_size;
    int node;
};

static struct addr_rule addr_rules[MAX_ADDR_RULES];
static int num_addr_rules = 0;
static struct name_rule name_rules[MAX_NAME_RULES];
static int num_name_rules = 0;
static struct size_rule size_rules[MAX_NAME_RULES];
static int num_size_rules = 0;
static int rules_loaded = 0;
static int layer2_active = 0;
static pthread_mutex_t rules_lock = PTHREAD_MUTEX_INITIALIZER;

static void load_rules(void) {
    if (rules_loaded) return;
    pthread_mutex_lock(&rules_lock);
    if (rules_loaded) { pthread_mutex_unlock(&rules_lock); return; }

    const char *conf = getenv("KUMF_CONF");
    if (!conf) conf = "kumf.conf";
    FILE *f = fopen(conf, "r");
    if (!f) {
        rules_loaded = 1;
        layer2_active = 0;
        pthread_mutex_unlock(&rules_lock);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long start, end;
        int node;
        if (sscanf(line, "0x%lx-0x%lx = %d", &start, &end, &node) == 3
            && num_addr_rules < MAX_ADDR_RULES) {
            addr_rules[num_addr_rules].start = start;
            addr_rules[num_addr_rules].end = end;
            addr_rules[num_addr_rules].node = node;
            num_addr_rules++;
        } else if (strncmp(line, "size_gt:", 9) == 0) {
            size_t threshold;
            if (sscanf(line + 9, "%zu = %d", &threshold, &node) == 2
                && num_size_rules < MAX_NAME_RULES) {
                size_rules[num_size_rules].min_size = threshold;
                size_rules[num_size_rules].max_size = 0;
                size_rules[num_size_rules].node = node;
                num_size_rules++;
            }
        } else if (strncmp(line, "size_lt:", 9) == 0) {
            size_t threshold;
            if (sscanf(line + 9, "%zu = %d", &threshold, &node) == 2
                && num_size_rules < MAX_NAME_RULES) {
                size_rules[num_size_rules].min_size = 0;
                size_rules[num_size_rules].max_size = threshold;
                size_rules[num_size_rules].node = node;
                num_size_rules++;
            }
        } else if (strncmp(line, "size_range:", 11) == 0) {
            size_t min_s, max_s;
            if (sscanf(line + 11, "%zu-%zu = %d", &min_s, &max_s, &node) == 3
                && num_size_rules < MAX_NAME_RULES) {
                size_rules[num_size_rules].min_size = min_s;
                size_rules[num_size_rules].max_size = max_s;
                size_rules[num_size_rules].node = node;
                num_size_rules++;
            }
        } else {
            char pat[MAX_FUNC_NAME];
            if (sscanf(line, "%255s = %d", pat, &node) == 2
                && num_name_rules < MAX_NAME_RULES) {
                strncpy(name_rules[num_name_rules].pattern, pat, MAX_FUNC_NAME-1);
                name_rules[num_name_rules].node = node;
                num_name_rules++;
            }
        }
    }
    fclose(f);

    layer2_active = (num_addr_rules + num_size_rules + num_name_rules) > 0;
    rules_loaded = 1;
    pthread_mutex_unlock(&rules_lock);
}

/* ================================================================
 * Thread Affinity Management
 *
 * Layer 1 (no KUMF_CONF):
 *   - Set CPU affinity of MAIN thread to ALL CPUs in configured nodes
 *   - NO per-thread binding, NO set_mempolicy, NO pthread_create intercept
 *   - Children inherit parent affinity via clone/fork
 *   - Exactly = numactl --cpunodebind, via LD_PRELOAD
 *   - ZERO ongoing overhead
 *
 * Layer 2 (KUMF_CONF has rules):
 *   - Per-thread binding + set_mempolicy for precise data placement
 *   - Intercepted pthread_create with round-robin/compact spread
 * ================================================================ */

/* Affinity state */
static int affinity_enabled = 0;
static int affinity_nodes[MAX_NUMA_NODES];
static int num_affinity_nodes = 0;
static int next_node_rr = 0;
static pthread_mutex_t affinity_rr_lock = PTHREAD_MUTEX_INITIALIZER;
static enum kumf_spread spread_strategy = KUMF_SPREAD_ROUNDROBIN;
static enum kumf_mpolicy mem_policy = KUMF_MPOL_PREFERRED;

/* Per-thread home node (TLS) — only used in Layer 2 mode */
static __thread int thread_home_node = -1;

/* Global thread counter for compact spread (Layer 2 only) */
static int global_thread_seq = 0;
static pthread_mutex_t thread_seq_lock = PTHREAD_MUTEX_INITIALIZER;

/* Cached CPU counts per node (populated at init) */
static int node_cpu_counts[MAX_NUMA_NODES];
static int node_cpu_counts_loaded = 0;

/* Per-thread allocation statistics */
static __thread long long tl_stat_local = 0;
static __thread long long tl_stat_config = 0;

/* Aggregated stats (at destructor only) */
static long long stat_local_allocs = 0;
static long long stat_config_allocs = 0;

/* Track if any thread ever used Layer 2 (to optimize free path) */
static int layer2_ever_used = 0;

static int kumf_debug = -1;

static int get_debug(void) {
    if (kumf_debug < 0)
        kumf_debug = getenv("KUMF_DEBUG") ? 1 : 0;
    return kumf_debug;
}

#define KUMF_LOG(fmt, ...) do { \
    if (get_debug()) fprintf(stderr, "[KUMF] " fmt "\n", ##__VA_ARGS__); \
} while(0)

static int node_cpu_count(int node) {
    if (node_cpu_counts_loaded && node < MAX_NUMA_NODES)
        return node_cpu_counts[node];

    int count = 0;
    struct bitmask *cpus = numa_allocate_cpumask();
    if (!cpus) return 0;
    if (numa_node_to_cpus(node, cpus) == 0) {
        for (unsigned long i = 0; i < cpus->size && i < CPU_SETSIZE; i++) {
            if (numa_bitmask_isbitset(cpus, i))
                count++;
        }
    }
    numa_free_cpumask(cpus);
    return count;
}

static void cache_node_cpu_counts(void) {
    if (node_cpu_counts_loaded) return;
    memset(node_cpu_counts, 0, sizeof(node_cpu_counts));
    for (int i = 0; i < num_affinity_nodes; i++) {
        int n = affinity_nodes[i];
        if (n < MAX_NUMA_NODES)
            node_cpu_counts[n] = node_cpu_count(n);
    }
    node_cpu_counts_loaded = 1;
}

/*
 * L1 bind: set CPU affinity of current thread to ALL CPUs in all configured nodes.
 * This is exactly what numactl --cpunodebind does: bind once, children inherit.
 * NO set_mempolicy — first-touch naturally places pages on local node.
 */
static int bind_to_all_nodes(void) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int i = 0; i < num_affinity_nodes; i++) {
        struct bitmask *cpus = numa_allocate_cpumask();
        if (!cpus) continue;
        if (numa_node_to_cpus(affinity_nodes[i], cpus) == 0) {
            for (unsigned long j = 0; j < cpus->size && j < CPU_SETSIZE; j++) {
                if (numa_bitmask_isbitset(cpus, j))
                    CPU_SET(j, &cpuset);
            }
        }
        numa_free_cpumask(cpus);
    }

    return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

/*
 * L2 bind: bind current thread to a specific single NUMA node.
 * Used only when Layer 2 is active (KUMF_CONF has routing rules).
 */
static int bind_thread_to_node(int node) {
    int ret;

    /* 1. CPU affinity: restrict to this single node's CPUs */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    struct bitmask *cpus = numa_allocate_cpumask();
    if (!cpus) return -1;

    if (numa_node_to_cpus(node, cpus) < 0) {
        numa_free_cpumask(cpus);
        return -1;
    }

    for (unsigned long i = 0; i < cpus->size && i < CPU_SETSIZE; i++) {
        if (numa_bitmask_isbitset(cpus, i))
            CPU_SET(i, &cpuset);
    }
    numa_free_cpumask(cpus);

    ret = sched_setaffinity(0, sizeof(cpuset), &cpuset);
    if (ret < 0) {
        KUMF_LOG("sched_setaffinity to node %d failed: %s", node, strerror(errno));
        return ret;
    }

    /* 2. Memory policy: precise page placement control */
    unsigned long nodemask = (1UL << node);

    switch (mem_policy) {
    case KUMF_MPOL_PREFERRED:
        ret = set_mempolicy(MPOL_PREFERRED, &nodemask, sizeof(nodemask) * 8);
        break;
    case KUMF_MPOL_BIND:
        ret = set_mempolicy(MPOL_BIND, &nodemask, sizeof(nodemask) * 8);
        break;
    case KUMF_MPOL_INTERLEAVE: {
        unsigned long all_mask = 0;
        for (int i = 0; i < num_affinity_nodes; i++)
            all_mask |= (1UL << affinity_nodes[i]);
        ret = set_mempolicy(MPOL_INTERLEAVE, &all_mask, sizeof(all_mask) * 8);
        break;
    }
    }

    if (ret < 0) {
        KUMF_LOG("set_mempolicy(mpol=%d) to node %d failed: %s",
                 mem_policy, node, strerror(errno));
    }

    return 0;
}

static int get_next_node(void) {
    if (spread_strategy == KUMF_SPREAD_COMPACT) {
        int seq;
        pthread_mutex_lock(&thread_seq_lock);
        seq = global_thread_seq++;
        pthread_mutex_unlock(&thread_seq_lock);

        int offset = 0;
        for (int i = 0; i < num_affinity_nodes; i++) {
            int n = affinity_nodes[i];
            int cpus = (n < MAX_NUMA_NODES && node_cpu_counts_loaded)
                       ? node_cpu_counts[n] : node_cpu_count(n);
            if (seq < offset + cpus) return n;
            offset += cpus;
        }
        return affinity_nodes[seq % num_affinity_nodes];
    } else {
        int idx;
        pthread_mutex_lock(&affinity_rr_lock);
        idx = next_node_rr++;
        pthread_mutex_unlock(&affinity_rr_lock);
        return affinity_nodes[idx % num_affinity_nodes];
    }
}

struct thread_wrap_args {
    void *(*start_routine)(void *);
    void *arg;
    int assigned_node;
};

static void *thread_entry_wrapper(void *arg) {
    struct thread_wrap_args *w = (struct thread_wrap_args *)arg;
    void *(*start)(void *) = w->start_routine;
    void *user_arg = w->arg;
    int node = w->assigned_node;

    libc_free(w);

    thread_home_node = node;

    if (bind_thread_to_node(node) == 0) {
        KUMF_LOG("Thread tid=%d bound to node %d (policy=%s)",
                 (int)syscall(SYS_gettid), node,
                 mem_policy == KUMF_MPOL_BIND ? "bind" :
                 mem_policy == KUMF_MPOL_INTERLEAVE ? "interleave" : "preferred");
    } else {
        KUMF_LOG("Thread tid=%d FAILED to bind to node %d",
                 (int)syscall(SYS_gettid), node);
    }

    return start(user_arg);
}

/*
 * Intercept pthread_create for Layer 2 mode only.
 * Layer 1: threads inherit parent's CPU affinity naturally — no interception.
 */
extern "C" int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                               void *(*start)(void *), void *arg) {
    if (!libc_pthread_create) {
        libc_pthread_create = (int (*)(pthread_t *, const pthread_attr_t *,
                                       void *(*)(void *), void *))
                               dlsym(RTLD_NEXT, "pthread_create");
    }

    /* L1 (no config): inherit parent affinity, pass through */
    if (!layer2_active) {
        return libc_pthread_create(tid, attr, start, arg);
    }

    /* L2 (has config rules): per-thread NUMA assignment */
    if (!affinity_enabled) {
        return libc_pthread_create(tid, attr, start, arg);
    }

    int node = get_next_node();

    struct thread_wrap_args *w =
        (struct thread_wrap_args *)libc_malloc(sizeof(*w));
    if (!w) return ENOMEM;
    w->start_routine = start;
    w->arg = arg;
    w->assigned_node = node;

    return libc_pthread_create(tid, attr, thread_entry_wrapper, w);
}

/* ================================================================
 * Daemon Communication
 * ================================================================ */
static int daemon_sock = -1;

static void connect_daemon(void) {
    const char *sock_path = getenv("KUMF_DAEMON");
    /* Only connect if explicitly configured (KUMF_DAEMON env var set) */
    if (!sock_path) return;

    /* Quick existence check: skip if socket file doesn't exist */
    if (access(sock_path, F_OK) < 0) return;

    daemon_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (daemon_sock < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    char msg[128];
    snprintf(msg, sizeof(msg), "REG:%d:%d", getpid(), num_affinity_nodes);
    sendto(daemon_sock, msg, strlen(msg), MSG_DONTWAIT,
           (struct sockaddr *)&addr, sizeof(addr));

    KUMF_LOG("Registered with daemon at %s (pid=%d)", sock_path, getpid());
}

static void disconnect_daemon(void) {
    if (daemon_sock >= 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "DEREG:%d", getpid());
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, KUMF_SOCK_PATH, sizeof(addr.sun_path) - 1);
        sendto(daemon_sock, msg, strlen(msg), MSG_DONTWAIT,
               (struct sockaddr *)&addr, sizeof(addr));
        close(daemon_sock);
        daemon_sock = -1;
    }
}

/* ================================================================
 * Node resolution: config rules only (Layer 2)
 *
 * Only called when Layer 2 is active AND size > ROUTE_THRESHOLD.
 * Layer 1 never enters this code path — malloc goes directly to libc.
 * ================================================================ */

static int match_addr(void *caller) {
    unsigned long addr = (unsigned long)caller;
    for (int i = 0; i < num_addr_rules; i++) {
        if (addr >= addr_rules[i].start && addr < addr_rules[i].end)
            return addr_rules[i].node;
    }
    return -1;
}

static int match_size(size_t sz) {
    for (int i = 0; i < num_size_rules; i++) {
        int min_ok = (size_rules[i].min_size == 0) || (sz >= size_rules[i].min_size);
        int max_ok = (size_rules[i].max_size == 0) || (sz < size_rules[i].max_size);
        if (min_ok && max_ok)
            return size_rules[i].node;
    }
    return -1;
}

static int resolve_node(void *caller, size_t sz) {
    if (!layer2_active) return -1;

    int node = match_size(sz);
    if (node >= 0) {
        KUMF_LOG("L2 config(size): sz=%zu -> node %d", sz, node);
        tl_stat_config++;
        layer2_ever_used = 1;
        return node;
    }

    node = match_addr(caller);
    if (node >= 0) {
        KUMF_LOG("L2 config(addr): caller=%p sz=%zu -> node %d", caller, sz, node);
        tl_stat_config++;
        layer2_ever_used = 1;
        return node;
    }

    tl_stat_local++;
    return -1;
}

/* ================================================================
 * LD_PRELOAD hooks: malloc/calloc/realloc/free
 *
 * Layer 1 (no KUMF_CONF):
 *   - All calls pass through to libc directly
 *   - Only overhead: one bool check (layer2_active) + branch
 *   - Locality from sched_setaffinity + first-touch, not interception
 *
 * Layer 2 (KUMF_CONF with rules):
 *   - Large allocs (>ROUTE_THRESHOLD) check rules
 *   - Matched → numa_alloc_onnode()
 *   - Unmatched → libc (first-touch)
 * ================================================================ */

extern "C" void *malloc(size_t sz)
{
    if (!libc_malloc) { libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc"); }

    if (layer2_active && sz > ROUTE_THRESHOLD) {
        load_rules();
        void *caller = __builtin_return_address(0);
        int node = resolve_node(caller, sz);
        if (node >= 0) {
            void *addr = numa_alloc_onnode(sz, node);
            if (addr) {
                KUMF_LOG("malloc(%zu) -> node %d addr=%p", sz, node, addr);
                record_seg((unsigned long)addr, sz);
                return addr;
            }
            KUMF_LOG("malloc(%zu) node %d failed, fallback to libc", sz, node);
        }
    }

    return libc_malloc(sz);
}

extern "C" void *calloc(size_t nmemb, size_t size)
{
    if (!libc_calloc) { libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc"); }

    size_t total = nmemb * size;
    if (layer2_active && total > ROUTE_THRESHOLD) {
        load_rules();
        void *caller = __builtin_return_address(0);
        int node = resolve_node(caller, total);
        if (node >= 0) {
            void *addr = numa_alloc_onnode(total, node);
            if (addr) {
                /* mmap returns zero-filled pages, no memset needed */
                record_seg((unsigned long)addr, total);
                return addr;
            }
        }
    }

    return libc_calloc(nmemb, size);
}

extern "C" void *realloc(void *ptr, size_t size)
{
    if (!libc_realloc) { libc_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc"); }
    if (!ptr) return malloc(size);
    size_t old_sz = check_and_remove_seg((unsigned long)ptr);
    if (old_sz > 0) {
        void *new_addr = malloc(size);
        if (new_addr) {
            memcpy(new_addr, ptr, old_sz < size ? old_sz : size);
            numa_free(ptr, old_sz);
        }
        return new_addr;
    }
    return libc_realloc(ptr, size);
}

extern "C" void free(void *p)
{
    if (!libc_free) { libc_free = (void (*)(void *))dlsym(RTLD_NEXT, "free"); }
    if (!p) return;

    /* Fast path: Layer 1 only (no numa_alloc ever used) → skip hash lookup */
    if (!layer2_ever_used) {
        libc_free(p);
        return;
    }

    size_t sz = check_and_remove_seg((unsigned long)p);
    if (sz > 0) {
        numa_free(p, sz);
    } else {
        libc_free(p);
    }
}

/* C++ operators */
void *operator new(size_t sz) noexcept(false) { return malloc(sz); }
void *operator new(size_t sz, const std::nothrow_t &) noexcept { return malloc(sz); }
void *operator new[](size_t sz) noexcept(false) { return malloc(sz); }
void *operator new[](size_t sz, const std::nothrow_t &) noexcept { return malloc(sz); }
void operator delete(void *ptr) noexcept { free(ptr); }
void operator delete[](void *ptr) noexcept { free(ptr); }

/* ================================================================
 * Initialization
 * ================================================================ */

static void init_affinity(void) {
    const char *env = getenv("KUMF_AFFINITY");

    if (!env || strcmp(env, "auto") == 0 || strcmp(env, "1") == 0 || strcmp(env, "on") == 0) {
        affinity_enabled = 1;
    } else if (strcmp(env, "compact") == 0) {
        affinity_enabled = 1;
        spread_strategy = KUMF_SPREAD_COMPACT;
    } else if (strcmp(env, "off") == 0 || strcmp(env, "0") == 0) {
        affinity_enabled = 0;
        return;
    } else {
        affinity_enabled = 1;
    }

    if (numa_available() < 0) {
        KUMF_LOG("NUMA not available, affinity disabled");
        affinity_enabled = 0;
        return;
    }

    const char *nodes_env = getenv("KUMF_NODES");
    if (nodes_env) {
        char *buf = strdup(nodes_env);
        char *tok = strtok(buf, ",");
        while (tok && num_affinity_nodes < MAX_NUMA_NODES) {
            int n = atoi(tok);
            if (numa_node_size(n, NULL) >= 0) {
                affinity_nodes[num_affinity_nodes++] = n;
            } else {
                KUMF_LOG("KUMF_NODES: node %d not available, skipping", n);
            }
            tok = strtok(NULL, ",");
        }
        free(buf);
    } else {
        int max_node = numa_max_node();
        for (int i = 0; i <= max_node && num_affinity_nodes < MAX_NUMA_NODES; i++) {
            if (numa_node_size(i, NULL) > 0) {
                affinity_nodes[num_affinity_nodes++] = i;
            }
        }
    }

    if (num_affinity_nodes == 0) {
        KUMF_LOG("No NUMA nodes available, affinity disabled");
        affinity_enabled = 0;
        return;
    }

    const char *policy_env = getenv("KUMF_POLICY");
    if (policy_env) {
        if (strcmp(policy_env, "bind") == 0) {
            mem_policy = KUMF_MPOL_BIND;
        } else if (strcmp(policy_env, "interleave") == 0) {
            mem_policy = KUMF_MPOL_INTERLEAVE;
        }
    }

    if (layer2_active) {
        /*
         * Layer 2 mode (KUMF_CONF has rules): per-thread binding.
         * Each thread gets its own node for precise data placement.
         */
        cache_node_cpu_counts();

        thread_home_node = affinity_nodes[0];
        if (bind_thread_to_node(thread_home_node) == 0) {
            KUMF_LOG("L2: Main thread bound to node %d (policy=%s, spread=%s)",
                     thread_home_node,
                     mem_policy == KUMF_MPOL_BIND ? "bind" :
                     mem_policy == KUMF_MPOL_INTERLEAVE ? "interleave" : "preferred",
                     spread_strategy == KUMF_SPREAD_COMPACT ? "compact" : "round-robin");
        }
    } else {
        /*
         * Layer 1 mode (default, no config): batch affinity only.
         * Bind parent to ALL CPUs in configured nodes.
         * Children inherit via clone/fork — NO per-thread work.
         * NO set_mempolicy — first-touch handles data placement.
         * This = exactly numactl --cpunodebind, via LD_PRELOAD.
         */
        if (bind_to_all_nodes() == 0) {
            KUMF_LOG("L1: bound to %d nodes, children inherit, "
                     "first-touch memory (zero overhead)",
                     num_affinity_nodes);
        } else {
            KUMF_LOG("L1: bind_to_all_nodes failed: %s", strerror(errno));
        }

        /* Per-thread tracking disabled in L1 */
        affinity_enabled = 0;

        /* Close daemon socket in L1 mode (no daemon needed) */
        if (daemon_sock >= 0) { close(daemon_sock); daemon_sock = -1; }
    }

    KUMF_LOG("Affinity: %d nodes [%s] mode=%s",
             num_affinity_nodes,
             nodes_env ? nodes_env : "auto",
             layer2_active ? "L2(per-thread)" : "L1(batch inherit)");

    for (int i = 0; i < num_affinity_nodes; i++) {
        int n = affinity_nodes[i];
        long free_mem;
        long total_mem = numa_node_size(n, &free_mem);
        int cpus = node_cpu_count(n);
        KUMF_LOG("  Node %d: %d CPUs, %ld MB total, %ld MB free",
                 n, cpus, total_mem / (1024*1024), free_mem / (1024*1024));
    }
}

__attribute__((constructor))
static void kumf_init(void) {
    libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    libc_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    libc_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    libc_pthread_create = (int (*)(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *))
                           dlsym(RTLD_NEXT, "pthread_create");

    load_rules();
    init_affinity();
    connect_daemon();

    KUMF_LOG("KUMF interc v2 loaded (L1=batch-inherit, L2=routing:%s, rules=%d)",
             layer2_active ? "on" : "off",
             num_size_rules + num_addr_rules);
}

__attribute__((destructor))
static void kumf_fini(void) {
    disconnect_daemon();

    stat_local_allocs += tl_stat_local;
    stat_config_allocs += tl_stat_config;

    if (stat_local_allocs + stat_config_allocs > 0) {
        KUMF_LOG("Stats: L1_local=%lld L2_config=%lld",
                 stat_local_allocs, stat_config_allocs);
    }
}
