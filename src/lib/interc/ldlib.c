/*
 * KUMF interc - NUMA-aware allocation routing + thread affinity
 * 
 * Lightweight LD_PRELOAD library that:
 * 1. Binds threads to NUMA nodes in round-robin (eliminates scheduling variance)
 * 2. Routes allocations to the calling thread's local NUMA node by default
 * 3. Allows config overrides for specific allocation patterns
 *
 * Env vars:
 *   KUMF_CONF      - Config file path (default: ./kumf.conf)
 *   KUMF_AFFINITY  - Thread affinity: auto|off (default: auto)
 *   KUMF_NODES     - Comma-separated node list (default: all available nodes)
 *   KUMF_DEBUG     - Enable debug logging (any value = on)
 *
 * Config format (KUMF_CONF):
 *   size_gt:BYTES = NODE        — route allocations > BYTES to NODE
 *   size_lt:BYTES = NODE        — route allocations < BYTES to NODE
 *   size_range:MIN-MAX = NODE   — route MIN <= size < MAX to NODE
 *   0xADDR_START-0xADDR_END = NODE — route by caller address
 *   function_pattern = NODE     — route by function name (not implemented)
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
#include <pthread.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include <cerrno>

/* ================================================================
 * Configuration
 * ================================================================ */
#define MAX_ADDR_RULES 8192
#define MAX_NAME_RULES 256
#define MAX_FUNC_NAME 256
#define ADDR_HASH_BITS 16
#define ADDR_HASH_SIZE (1 << ADDR_HASH_BITS)
#define ADDR_HASH_MASK (ADDR_HASH_SIZE - 1)
#define ROUTE_THRESHOLD 4096  /* only route allocs > this size */
#define MAX_NUMA_NODES 128

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
 * Address hash map for numa_alloc tracking
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
 * Config rules (from KUMF_CONF file)
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
    size_t min_size;  /* inclusive, 0 = no lower bound */
    size_t max_size;  /* exclusive, 0 = no upper bound */
    int node;
};

static struct addr_rule addr_rules[MAX_ADDR_RULES];
static int num_addr_rules = 0;
static struct name_rule name_rules[MAX_NAME_RULES];
static int num_name_rules = 0;
static struct size_rule size_rules[MAX_NAME_RULES];
static int num_size_rules = 0;
static int rules_loaded = 0;
static pthread_mutex_t rules_lock = PTHREAD_MUTEX_INITIALIZER;

static void load_rules(void) {
    if (rules_loaded) return;
    pthread_mutex_lock(&rules_lock);
    if (rules_loaded) { pthread_mutex_unlock(&rules_lock); return; }

    const char *conf = getenv("KUMF_CONF");
    if (!conf) conf = "kumf.conf";
    FILE *f = fopen(conf, "r");
    if (!f) { rules_loaded = 1; pthread_mutex_unlock(&rules_lock); return; }

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
    rules_loaded = 1;
    pthread_mutex_unlock(&rules_lock);
}

/* ================================================================
 * Thread Affinity Management
 * 
 * When enabled (KUMF_AFFINITY=auto, default):
 * 1. Detect NUMA topology on startup
 * 2. Bind main thread to first node
 * 3. Intercept pthread_create → assign each new thread to a node
 *    in round-robin, set CPU affinity to that node's CPUs
 * 4. Allocations without config rules → route to thread's home node
 *    (per-thread NUMA locality)
 *
 * This eliminates the variance caused by OS randomly scheduling
 * threads across NUMA nodes, which is the #1 cause of unstable
 * performance on multi-socket platforms.
 * ================================================================ */

/* Affinity state */
static int affinity_enabled = 0;         /* 0=off, 1=on */
static int affinity_nodes[MAX_NUMA_NODES]; /* nodes to spread threads across */
static int num_affinity_nodes = 0;
static int next_node_rr = 0;             /* round-robin counter */
static pthread_mutex_t affinity_rr_lock = PTHREAD_MUTEX_INITIALIZER;

/* Per-thread home node (TLS) */
static __thread int thread_home_node = -1;

/* Allocation statistics (per-process, approximate) */
static long long stat_local_allocs = 0;   /* routed to thread's home node */
static long long stat_config_allocs = 0;  /* routed by config rules */
static long long stat_default_allocs = 0; /* fell through to libc */

static int kumf_debug = -1;

static int get_debug(void) {
    if (kumf_debug < 0)
        kumf_debug = getenv("KUMF_DEBUG") ? 1 : 0;
    return kumf_debug;
}

#define KUMF_LOG(fmt, ...) do { \
    if (get_debug()) fprintf(stderr, "[KUMF] " fmt "\n", ##__VA_ARGS__); \
} while(0)

/*
 * Bind current thread to a specific NUMA node.
 * Sets both CPU affinity (sched_setaffinity) and preferred memory node
 * (set_mempolicy) so that all allocations default to this node.
 */
static int bind_thread_to_node(int node) {
    int ret = 0;

    /* 1. CPU affinity: restrict to this node's CPUs */
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

    /* 2. Memory policy: prefer allocating from this node */
    /* MPOL_PREFERRED with node mask = single node */
    unsigned long nodemask = 0;
    nodemask |= (1UL << node);
    ret = set_mempolicy(MPOL_PREFERRED, &nodemask, sizeof(nodemask) * 8);
    if (ret < 0) {
        KUMF_LOG("set_mempolicy to node %d failed: %s", node, strerror(errno));
        /* Non-fatal: CPU affinity is more important */
    }

    return 0;
}

/*
 * Get next node for round-robin thread assignment.
 */
static int get_next_node(void) {
    int idx;
    pthread_mutex_lock(&affinity_rr_lock);
    idx = next_node_rr++;
    pthread_mutex_unlock(&affinity_rr_lock);
    return affinity_nodes[idx % num_affinity_nodes];
}

/*
 * Thread wrapper: sets affinity before calling user's start routine.
 * 
 * We intercept pthread_create, wrap the user's start routine with this,
 * which binds the thread to its assigned NUMA node before executing
 * user code.
 */
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

    /* Use libc_free since we allocated with libc_malloc */
    libc_free(w);

    /* Set thread-local home node */
    thread_home_node = node;

    /* Bind this thread to the assigned node */
    if (bind_thread_to_node(node) == 0) {
        KUMF_LOG("Thread tid=%d bound to node %d", (int)syscall(SYS_gettid), node);
    } else {
        KUMF_LOG("Thread tid=%d FAILED to bind to node %d",
                 (int)syscall(SYS_gettid), node);
    }

    /* Execute user's thread function */
    return start(user_arg);
}

/*
 * Intercept pthread_create to assign NUMA nodes to new threads.
 */
extern "C" int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                               void *(*start)(void *), void *arg) {
    if (!libc_pthread_create) {
        libc_pthread_create = (int (*)(pthread_t *, const pthread_attr_t *,
                                       void *(*)(void *), void *))
                               dlsym(RTLD_NEXT, "pthread_create");
    }

    if (!affinity_enabled) {
        return libc_pthread_create(tid, attr, start, arg);
    }

    /* Assign this thread to a NUMA node (round-robin) */
    int node = get_next_node();

    /* Allocate wrapper args using libc_malloc (avoid recursion) */
    struct thread_wrap_args *w =
        (struct thread_wrap_args *)libc_malloc(sizeof(*w));
    if (!w) return ENOMEM;
    w->start_routine = start;
    w->arg = arg;
    w->assigned_node = node;

    return libc_pthread_create(tid, attr, thread_entry_wrapper, w);
}

/*
 * Initialize thread affinity system.
 * Called from constructor (before main).
 */
static void init_affinity(void) {
    const char *env = getenv("KUMF_AFFINITY");

    /* Default: auto (enabled) when KUMF_CONF is set, off otherwise */
    if (!env) {
        const char *conf = getenv("KUMF_CONF");
        if (conf && conf[0] != '\0') {
            affinity_enabled = 1;  /* auto when config is present */
        } else {
            affinity_enabled = 0;  /* off by default without config */
        }
    } else if (strcmp(env, "off") == 0 || strcmp(env, "0") == 0) {
        affinity_enabled = 0;
        return;
    } else {
        /* "auto", "1", "on", "spread" — all enable affinity */
        affinity_enabled = 1;
    }

    /* Check NUMA availability */
    if (numa_available() < 0) {
        KUMF_LOG("NUMA not available, affinity disabled");
        affinity_enabled = 0;
        return;
    }

    /* Parse KUMF_NODES or auto-detect all available nodes */
    const char *nodes_env = getenv("KUMF_NODES");
    if (nodes_env) {
        char *buf = strdup(nodes_env);
        char *tok = strtok(buf, ",");
        while (tok && num_affinity_nodes < MAX_NUMA_NODES) {
            int n = atoi(tok);
            /* Verify node exists */
            if (numa_node_size(n, NULL) >= 0) {
                affinity_nodes[num_affinity_nodes++] = n;
            } else {
                KUMF_LOG("KUMF_NODES: node %d not available, skipping", n);
            }
            tok = strtok(NULL, ",");
        }
        free(buf);
    } else {
        /* Auto-detect: use all available NUMA nodes */
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

    /* Bind main thread to first node */
    thread_home_node = affinity_nodes[0];
    if (bind_thread_to_node(thread_home_node) == 0) {
        KUMF_LOG("Main thread bound to node %d", thread_home_node);
    }

    KUMF_LOG("Affinity enabled: %d nodes [%s]",
             num_affinity_nodes,
             nodes_env ? nodes_env : "auto");

    /* Print node info */
    for (int i = 0; i < num_affinity_nodes; i++) {
        int n = affinity_nodes[i];
        long free_mem;
        long total_mem = numa_node_size(n, &free_mem);
        KUMF_LOG("  Node %d: %ld MB total, %ld MB free",
                 n, total_mem / (1024*1024), free_mem / (1024*1024));
    }
}

/* ================================================================
 * Node resolution: config rules → thread-local → -1 (libc fallback)
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

/*
 * Resolve which NUMA node an allocation should go to.
 * Priority:
 *   1. Config rules (size-based, addr-based) — explicit routing
 *   2. Thread home node — per-thread NUMA locality
 *   3. -1 — fall through to libc (first-touch by OS)
 */
static int resolve_node(void *caller, size_t sz) {
    /* 1. Config rules take priority */
    int node = match_size(sz);
    if (node >= 0) {
        KUMF_LOG("config(size): sz=%zu -> node %d", sz, node);
        __sync_fetch_and_add(&stat_config_allocs, 1);
        return node;
    }
    node = match_addr(caller);
    if (node >= 0) {
        KUMF_LOG("config(addr): caller=%p sz=%zu -> node %d", caller, sz, node);
        __sync_fetch_and_add(&stat_config_allocs, 1);
        return node;
    }

    /* 2. Thread-local node (affinity mode) */
    if (affinity_enabled && thread_home_node >= 0) {
        KUMF_LOG("thread_local: sz=%zu -> node %d", sz, thread_home_node);
        __sync_fetch_and_add(&stat_local_allocs, 1);
        return thread_home_node;
    }

    /* 3. No rule, no affinity — let libc decide */
    __sync_fetch_and_add(&stat_default_allocs, 1);
    return -1;
}

/* ================================================================
 * LD_PRELOAD hooks: malloc/calloc/realloc/free
 * ================================================================ */

extern "C" void *malloc(size_t sz)
{
    if (!libc_malloc) { libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc"); }
    
    if (sz > ROUTE_THRESHOLD) {
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
    if (total > ROUTE_THRESHOLD) {
        load_rules();
        void *caller = __builtin_return_address(0);
        int node = resolve_node(caller, total);
        if (node >= 0) {
            void *addr = numa_alloc_onnode(total, node);
            if (addr) {
                memset(addr, 0, total);
                record_seg((unsigned long)addr, total);
                return addr;
            }
            KUMF_LOG("calloc(%zu) node %d failed, fallback to libc", total, node);
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
 * Constructor / Destructor
 * ================================================================ */

__attribute__((constructor))
static void kumf_init(void) {
    /* Resolve libc functions early (before any allocation) */
    libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    libc_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    libc_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    libc_pthread_create = (int (*)(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *))
                           dlsym(RTLD_NEXT, "pthread_create");

    /* Load config rules */
    load_rules();

    /* Initialize thread affinity */
    init_affinity();

    KUMF_LOG("KUMF interc loaded (affinity=%s, rules=%d size + %d addr)",
             affinity_enabled ? "on" : "off",
             num_size_rules, num_addr_rules);
}

__attribute__((destructor))
static void kumf_fini(void) {
    if (stat_local_allocs + stat_config_allocs + stat_default_allocs > 0) {
        KUMF_LOG("Stats: local=%lld config=%lld default=%lld",
                 stat_local_allocs, stat_config_allocs, stat_default_allocs);
    }
}
