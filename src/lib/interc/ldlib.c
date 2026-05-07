/*
 * KUMF interc v2 — NUMA-aware thread affinity + optional allocation routing
 *
 * Three modes (auto-selected based on topology):
 *
 *   Mode 1 — Single-socket batch (nodes on same socket):
 *     Bind main thread to ALL CPUs in configured nodes → children inherit.
 *     NO set_mempolicy, NO pthread_create intercept, NO malloc intercept.
 *     Same as numactl --cpunodebind, via LD_PRELOAD. Zero ongoing overhead.
 *
 *   Mode 2 — Cross-socket per-thread (nodes span multiple sockets):
 *     Each thread bound to its own node + MPOL_PREFERRED(node).
 *     Intercepted pthread_create with compact/round-robin spread.
 *     Data placement follows thread binding via first-touch + mempolicy.
 *     No malloc routing unless KUMF_CONF is set.
 *
 *   Mode 3 — Config-driven routing (KUMF_CONF has rules):
 *     Per-thread binding (like Mode 2) + size/caller-based allocation routing.
 *     Matched allocs → numa_alloc_onnode(). Unmatched → libc.
 *
 * Auto-detection:
 *   If max NUMA distance between KUMF_NODES > 2× min distance → cross-socket
 *   (same-socket distance ~12, cross-socket ~35-40 on Kunpeng930)
 *
 * Env vars:
 *   KUMF_NODES     - Comma-separated node list (default: auto-detect all)
 *   KUMF_AFFINITY  - auto|compact|batch|per-thread|off (default: auto)
 *                    auto = topology-based decision
 *                    batch = force Mode 1 (parent-level bind only)
 *                    compact/per-thread = force Mode 2
 *   KUMF_CONF      - Config file path (triggers Mode 3 routing)
 *   KUMF_POLICY    - Memory policy (Mode 2/3): preferred|bind|interleave
 *   KUMF_DEBUG     - Enable debug logging
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
#include <poll.h>

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

/* Mode enum */
enum kumf_mode {
    KUMF_MODE_BATCH = 0,       /* Single-socket: parent bind, children inherit */
    KUMF_MODE_PER_THREAD = 1,  /* Cross-socket: per-thread bind + mempolicy */
    KUMF_MODE_ROUTING = 2,     /* Config-driven: per-thread + allocation routing */
    KUMF_MODE_PASSIVE = 3,      /* No daemon config -> zero overhead pass-through */
};

/* Memory policy enum */
enum kumf_mpolicy {
    KUMF_MPOL_PREFERRED = 0,
    KUMF_MPOL_BIND = 1,
    KUMF_MPOL_INTERLEAVE = 2,
};

/* Thread spread strategy */
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
 * Address hash map (Mode 3 routing only)
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
 * Config rules (Mode 3 — from KUMF_CONF file)
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
static int layer2_active = 0;  /* 1 = KUMF_CONF has routing rules */
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
 * Affinity State
 * ================================================================ */
static enum kumf_mode kumf_mode_val = KUMF_MODE_BATCH;
static int affinity_nodes[MAX_NUMA_NODES];
static int num_affinity_nodes = 0;
static int next_node_rr = 0;
static pthread_mutex_t affinity_rr_lock = PTHREAD_MUTEX_INITIALIZER;
static enum kumf_spread spread_strategy = KUMF_SPREAD_COMPACT; /* compact default */
static enum kumf_mpolicy mem_policy = KUMF_MPOL_PREFERRED;

/* Per-thread home node (TLS) — Mode 2/3 */
static __thread int thread_home_node = -1;

/* Global thread counter for compact spread — Mode 2/3 */
static int global_thread_seq = 0;
static pthread_mutex_t thread_seq_lock = PTHREAD_MUTEX_INITIALIZER;

/* Cached CPU counts per node */
static int node_cpu_counts[MAX_NUMA_NODES];
static int node_cpu_counts_loaded = 0;

/* Track if any thread ever used routing (to optimize free path) */
static int routing_ever_used = 0;

/* Per-thread stats */
static __thread long long tl_stat_local = 0;
static __thread long long tl_stat_config = 0;
static long long stat_local_allocs = 0;
static long long stat_config_allocs = 0;

static int kumf_debug = -1;

static int get_debug(void) {
    if (kumf_debug < 0)
        kumf_debug = getenv("KUMF_DEBUG") ? 1 : 0;
    return kumf_debug;
}

#define KUMF_LOG(fmt, ...) do { \
    if (get_debug()) fprintf(stderr, "[KUMF] " fmt "\n", ##__VA_ARGS__); \
} while(0)

/* ================================================================
 * Topology helpers
 * ================================================================ */

static int node_cpu_count(int node) {
    if (node_cpu_counts_loaded && node < MAX_NUMA_NODES)
        return node_cpu_counts[node];
    int count = 0;
    struct bitmask *cpus = numa_allocate_cpumask();
    if (!cpus) return 0;
    if (numa_node_to_cpus(node, cpus) == 0) {
        for (unsigned long i = 0; i < cpus->size && i < CPU_SETSIZE; i++) {
            if (numa_bitmask_isbitset(cpus, i)) count++;
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
 * Detect cross-socket topology by checking NUMA distance matrix.
 * Returns 1 if any pair of affinity nodes has distance > 2× the minimum.
 * (Kunpeng930: same-socket ~12, cross-socket ~35-40)
 */
static int is_cross_socket(void) {
    if (num_affinity_nodes <= 1) return 0;

    int min_dist = INT32_MAX, max_dist = 0;
    for (int i = 0; i < num_affinity_nodes; i++) {
        for (int j = i + 1; j < num_affinity_nodes; j++) {
            int d = numa_distance(affinity_nodes[i], affinity_nodes[j]);
            if (d > 0 && d < min_dist) min_dist = d;
            if (d > max_dist) max_dist = d;
        }
    }

    /* If max distance > 2× min, we have cross-socket access */
    return (max_dist > min_dist * 2) ? 1 : 0;
}

/* ================================================================
 * Binding functions
 * ================================================================ */

/*
 * Mode 1 (batch): bind current thread to ALL CPUs in all configured nodes.
 * Same as numactl --cpunodebind: one sched_setaffinity, children inherit.
 * NO set_mempolicy — first-touch naturally local.
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
 * Mode 2/3 (per-thread): bind current thread to a specific NUMA node.
 * CPU affinity + set_mempolicy for precise data placement.
 */
static int bind_thread_to_node(int node) {
    int ret;
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
        KUMF_LOG("set_mempolicy(mpol=%d) node %d failed: %s",
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

/* ================================================================
 * Daemon config query — transparent auto-detection
 *
 * When interc is system-wide preloaded (via .bashrc LD_PRELOAD),
 * it queries the daemon on startup to check if the current binary
 * has been registered (via "kumf daemon profile -- CMD").
 *
 * If registered → apply learned config (Mode 2/3)
 * If not registered → PASSIVE mode (zero overhead pass-through)
 * ================================================================ */

#include <poll.h>

static void query_daemon_for_config(void) {
    /* Quick check: daemon socket exists? */
    if (access(KUMF_SOCK_PATH, F_OK) < 0) {
        KUMF_LOG("No daemon socket, passive mode");
        kumf_mode_val = KUMF_MODE_PASSIVE;
        return;
    }

    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { kumf_mode_val = KUMF_MODE_PASSIVE; return; }

    /* Get executable path */
    char exe_path[256];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len < 0) { close(sock); kumf_mode_val = KUMF_MODE_PASSIVE; return; }
    exe_path[len] = '\0';

    /* Bind to temp address — AF_UNIX DGRAM requires explicit bind
     * so the daemon has a return address to send the response to */
    char client_path[64];
    snprintf(client_path, sizeof(client_path), "/tmp/kumf_lookup_%d", getpid());
    unlink(client_path);

    struct sockaddr_un client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sun_family = AF_UNIX;
    strncpy(client_addr.sun_path, client_path, sizeof(client_addr.sun_path) - 1);
    if (bind(sock, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
        close(sock);
        kumf_mode_val = KUMF_MODE_PASSIVE;
        return;
    }

    /* Send LOOKUP to daemon */
    char msg[512];
    snprintf(msg, sizeof(msg), "LOOKUP:%d:%s", getpid(), exe_path);

    struct sockaddr_un daemon_addr;
    memset(&daemon_addr, 0, sizeof(daemon_addr));
    daemon_addr.sun_family = AF_UNIX;
    strncpy(daemon_addr.sun_path, KUMF_SOCK_PATH, sizeof(daemon_addr.sun_path) - 1);

    sendto(sock, msg, strlen(msg), MSG_DONTWAIT,
           (struct sockaddr *)&daemon_addr, sizeof(daemon_addr));

    /* Poll for response (50ms max) */
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 50);

    if (ret <= 0) {
        close(sock);
        unlink(client_path);
        KUMF_LOG("Daemon query timeout, passive mode");
        kumf_mode_val = KUMF_MODE_PASSIVE;
        return;
    }

    char buf[1024];
    ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
    close(sock);
    unlink(client_path);

    if (n <= 0 || strncmp(buf, "NOCONFIG", 8) == 0) {
        KUMF_LOG("No registered config for %s, passive mode", exe_path);
        kumf_mode_val = KUMF_MODE_PASSIVE;
        return;
    }

    buf[n] = '\0';

    /* Parse CONFIG:nodes:path */
    /* Format: "CONFIG:0,1,2,3:/var/lib/kumf/lookup_XXX.conf" */
    const char *nodes_start = buf + 7;  /* skip "CONFIG:" */
    const char *nodes_end = strchr(nodes_start, ':');
    if (!nodes_end) { kumf_mode_val = KUMF_MODE_PASSIVE; return; }

    /* Set KUMF_NODES from daemon */
    char nodes_str[64];
    size_t nl = nodes_end - nodes_start;
    if (nl > 0 && nl < sizeof(nodes_str)) {
        memcpy(nodes_str, nodes_start, nl);
        nodes_str[nl] = '\0';
        setenv("KUMF_NODES", nodes_str, 1);
        KUMF_LOG("Daemon config: nodes=%s", nodes_str);
    }

    /* Set KUMF_CONF to the daemon-provided config path */
    const char *conf_path = nodes_end + 1;
    if (*conf_path && strlen(conf_path) > 0) {
        setenv("KUMF_CONF", conf_path, 1);
        KUMF_LOG("Daemon config: conf=%s", conf_path);
    }

    /* Now load_rules() and init_affinity() will use these env vars */
    KUMF_LOG("Daemon config applied, proceeding with auto-mode selection");
}

/* ================================================================
 * pthread_create intercept (Mode 2/3 only)
 *
 * Mode 1 (batch): children inherit parent affinity, pass through.
 * Mode 2/3 (per-thread): each thread gets its own node assignment.
 * ================================================================ */

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
    bind_thread_to_node(node);
    KUMF_LOG("Thread tid=%d -> node %d", (int)syscall(SYS_gettid), node);
    return start(user_arg);
}

extern "C" int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                               void *(*start)(void *), void *arg) {
    if (!libc_pthread_create) {
        libc_pthread_create = (int (*)(pthread_t *, const pthread_attr_t *,
                                       void *(*)(void *), void *))
                               dlsym(RTLD_NEXT, "pthread_create");
    }

    /* Passive or Batch mode: pass through to libc */
    if (kumf_mode_val == KUMF_MODE_PASSIVE || kumf_mode_val == KUMF_MODE_BATCH) {
        return libc_pthread_create(tid, attr, start, arg);
    }

    /* Mode 2/3 (per-thread): assign node, wrap thread entry */
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
    if (!sock_path) return;
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
 * Node resolution (Mode 3 routing only)
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
        KUMF_LOG("routing(size): sz=%zu -> node %d", sz, node);
        tl_stat_config++;
        routing_ever_used = 1;
        return node;
    }

    node = match_addr(caller);
    if (node >= 0) {
        KUMF_LOG("routing(addr): caller=%p sz=%zu -> node %d", caller, sz, node);
        tl_stat_config++;
        routing_ever_used = 1;
        return node;
    }

    tl_stat_local++;
    return -1;
}

/* ================================================================
 * LD_PRELOAD hooks: malloc/calloc/realloc/free
 *
 * Mode 1 (batch): all calls pass through to libc. Overhead = one bool check.
 * Mode 2 (per-thread): same as Mode 1 — no routing.
 * Mode 3 (routing): large allocs check rules, matched → numa_alloc_onnode().
 * ================================================================ */

extern "C" void *malloc(size_t sz)
{
    if (!libc_malloc) { libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc"); }

    if (kumf_mode_val == KUMF_MODE_PASSIVE)
        return libc_malloc(sz);

    if (layer2_active && sz > ROUTE_THRESHOLD) {
        load_rules();
        void *caller = __builtin_return_address(0);
        int node = resolve_node(caller, sz);
        if (node >= 0) {
            void *addr = numa_alloc_onnode(sz, node);
            if (addr) {
                record_seg((unsigned long)addr, sz);
                return addr;
            }
        }
    }

    return libc_malloc(sz);
}

extern "C" void *calloc(size_t nmemb, size_t size)
{
    if (!libc_calloc) { libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc"); }

    if (kumf_mode_val == KUMF_MODE_PASSIVE)
        return libc_calloc(nmemb, size);

    size_t total = nmemb * size;
    if (layer2_active && total > ROUTE_THRESHOLD) {
        load_rules();
        void *caller = __builtin_return_address(0);
        int node = resolve_node(caller, total);
        if (node >= 0) {
            void *addr = numa_alloc_onnode(total, node);
            if (addr) {
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

    if (kumf_mode_val == KUMF_MODE_PASSIVE) {
        if (!ptr) return libc_malloc(size);
        return libc_realloc(ptr, size);
    }

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

    if (kumf_mode_val == KUMF_MODE_PASSIVE) {
        libc_free(p);
        return;
    }

    /* Fast path: no routing ever used → skip hash lookup entirely */
    if (!routing_ever_used) {
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
 * Initialization — auto-select mode based on topology
 * ================================================================ */

static void init_affinity(void) {
    if (numa_available() < 0) {
        KUMF_LOG("NUMA not available, disabled");
        kumf_mode_val = KUMF_MODE_BATCH;
        return;
    }

    /* Parse KUMF_NODES */
    const char *nodes_env = getenv("KUMF_NODES");
    if (nodes_env) {
        char *buf = strdup(nodes_env);
        char *tok = strtok(buf, ",");
        while (tok && num_affinity_nodes < MAX_NUMA_NODES) {
            int n = atoi(tok);
            if (numa_node_size(n, NULL) >= 0) {
                affinity_nodes[num_affinity_nodes++] = n;
            } else {
                KUMF_LOG("KUMF_NODES: node %d not available", n);
            }
            tok = strtok(NULL, ",");
        }
        free(buf);
    } else {
        /* Auto-detect: all available NUMA nodes */
        int max_node = numa_max_node();
        for (int i = 0; i <= max_node && num_affinity_nodes < MAX_NUMA_NODES; i++) {
            if (numa_node_size(i, NULL) > 0)
                affinity_nodes[num_affinity_nodes++] = i;
        }
    }

    if (num_affinity_nodes == 0) {
        KUMF_LOG("No NUMA nodes, disabled");
        kumf_mode_val = KUMF_MODE_BATCH;
        return;
    }

    /* Parse KUMF_POLICY */
    const char *policy_env = getenv("KUMF_POLICY");
    if (policy_env) {
        if (strcmp(policy_env, "bind") == 0)
            mem_policy = KUMF_MPOL_BIND;
        else if (strcmp(policy_env, "interleave") == 0)
            mem_policy = KUMF_MPOL_INTERLEAVE;
    }

    /* Parse KUMF_AFFINITY: auto|compact|per-thread|batch|off */
    const char *aff_env = getenv("KUMF_AFFINITY");
    int force_mode = -1;  /* -1 = auto */

    if (aff_env) {
        if (strcmp(aff_env, "off") == 0 || strcmp(aff_env, "0") == 0) {
            KUMF_LOG("Affinity explicitly disabled");
            return;
        } else if (strcmp(aff_env, "batch") == 0) {
            force_mode = KUMF_MODE_BATCH;
        } else if (strcmp(aff_env, "per-thread") == 0 || strcmp(aff_env, "compact") == 0) {
            force_mode = KUMF_MODE_PER_THREAD;
            if (strcmp(aff_env, "compact") == 0)
                spread_strategy = KUMF_SPREAD_COMPACT;
        }
        /* "auto" or unrecognized → auto-detect */
    }

    /* Auto-select mode based on topology */
    if (force_mode >= 0) {
        kumf_mode_val = (enum kumf_mode)force_mode;
    } else if (layer2_active) {
        /* KUMF_CONF with rules → Mode 3 */
        kumf_mode_val = KUMF_MODE_ROUTING;
    } else if (is_cross_socket()) {
        /* Cross-socket → per-thread binding for data locality */
        kumf_mode_val = KUMF_MODE_PER_THREAD;
    } else {
        /* Single-socket → batch inherit (like numactl) */
        kumf_mode_val = KUMF_MODE_BATCH;
    }

    /* Execute binding based on mode */
    switch (kumf_mode_val) {
    case KUMF_MODE_BATCH:
        bind_to_all_nodes();
        KUMF_LOG("Mode 1 (batch): %d nodes, parent bind, children inherit",
                 num_affinity_nodes);
        break;

    case KUMF_MODE_PER_THREAD:
        cache_node_cpu_counts();
        thread_home_node = affinity_nodes[0];
        bind_thread_to_node(thread_home_node);
        KUMF_LOG("Mode 2 (per-thread): %d nodes, compact spread, policy=%s",
                 num_affinity_nodes,
                 mem_policy == KUMF_MPOL_BIND ? "bind" :
                 mem_policy == KUMF_MPOL_INTERLEAVE ? "interleave" : "preferred");
        break;

    case KUMF_MODE_ROUTING:
        cache_node_cpu_counts();
        thread_home_node = affinity_nodes[0];
        bind_thread_to_node(thread_home_node);
        KUMF_LOG("Mode 3 (routing): %d nodes, %d size rules + %d addr rules, policy=%s",
                 num_affinity_nodes, num_size_rules, num_addr_rules,
                 mem_policy == KUMF_MPOL_BIND ? "bind" :
                 mem_policy == KUMF_MPOL_INTERLEAVE ? "interleave" : "preferred");
        break;

    case KUMF_MODE_PASSIVE:
        /* Should never reach here — passive mode exits early in constructor */
        break;
    }

    /* Log topology summary */
    KUMF_LOG("Nodes: [%s], cross-socket: %s",
             nodes_env ? nodes_env : "auto",
             is_cross_socket() ? "yes" : "no");

    for (int i = 0; i < num_affinity_nodes; i++) {
        int n = affinity_nodes[i];
        long free_mem;
        long total_mem = numa_node_size(n, &free_mem);
        int cpus = node_cpu_count(n);
        KUMF_LOG("  Node %d: %d CPUs, %ld MB total, %ld MB free",
                 n, cpus, total_mem / (1024*1024), free_mem / (1024*1024));
    }

    /* Print distance matrix for cross-socket detection debug */
    if (get_debug() && num_affinity_nodes > 1) {
        KUMF_LOG("Distance matrix:");
        for (int i = 0; i < num_affinity_nodes; i++) {
            char line[256] = "";
            for (int j = 0; j < num_affinity_nodes; j++) {
                char cell[16];
                snprintf(cell, sizeof(cell), "%4d",
                         numa_distance(affinity_nodes[i], affinity_nodes[j]));
                strcat(line, cell);
            }
            KUMF_LOG("  node %d: %s", affinity_nodes[i], line);
        }
    }
}

__attribute__((constructor))
static void kumf_init(void) {
    /* Always load libc pointers — needed for intercepts even in passive mode */
    libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    libc_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    libc_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    libc_pthread_create = (int (*)(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *))
                           dlsym(RTLD_NEXT, "pthread_create");

    /* Priority: explicit env vars > daemon auto-config > passive
     * If user set KUMF_AFFINITY / KUMF_NODES / KUMF_CONF → use them directly.
     * Otherwise, query daemon for a registered config.
     * No config means PASSIVE (zero overhead for non-registered binaries). */
    int explicit_env =
        getenv("KUMF_AFFINITY") || getenv("KUMF_NODES") || getenv("KUMF_CONF");

    if (!explicit_env) {
        query_daemon_for_config();
    }

    /* If passive (no config and no env vars), skip everything */
    if (kumf_mode_val == KUMF_MODE_PASSIVE) {
        KUMF_LOG("KUMF v2 loaded: PASSIVE (no config for this binary)");
        return;
    }

    /* Apply config (from env vars or daemon) */
    load_rules();
    init_affinity();
    connect_daemon();

    KUMF_LOG("KUMF v2 loaded: mode=%s, nodes=%d, routing=%s",
             kumf_mode_val == KUMF_MODE_BATCH ? "batch" :
             kumf_mode_val == KUMF_MODE_PER_THREAD ? "per-thread" :
             kumf_mode_val == KUMF_MODE_ROUTING ? "routing" : "passive",
             num_affinity_nodes,
             layer2_active ? "on" : "off");
}

__attribute__((destructor))
static void kumf_fini(void) {
    disconnect_daemon();

    stat_local_allocs += tl_stat_local;
    stat_config_allocs += tl_stat_config;

    if (stat_local_allocs + stat_config_allocs > 0) {
        KUMF_LOG("Stats: local=%lld routed=%lld",
                 stat_local_allocs, stat_config_allocs);
    }
}
