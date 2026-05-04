/*
 * KUMF interc - NUMA-aware allocation routing
 * 
 * Lightweight LD_PRELOAD library that routes large allocations (>4KB)
 * to specific NUMA nodes based on caller address matching.
 * Uses __builtin_return_address(0) for zero-overhead caller identification.
 * 
 * Config format (KUMF_CONF env var or ./kumf.conf):
 *   0xADDR_START-0xADDR_END = NODE_ID
 *   function_name_pattern = NODE_ID
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
#include <numa.h>

/* ---- Configuration ---- */
#define MAX_ADDR_RULES 8192
#define MAX_NAME_RULES 256
#define MAX_FUNC_NAME 256
#define ADDR_HASH_BITS 16
#define ADDR_HASH_SIZE (1 << ADDR_HASH_BITS)
#define ADDR_HASH_MASK (ADDR_HASH_SIZE - 1)
#define ROUTE_THRESHOLD 4096  /* only route allocs > this size */

/* ---- libc function pointers (forward declarations) ---- */
static void *(*libc_malloc)(size_t);
static void *(*libc_calloc)(size_t, size_t);
static void *(*libc_realloc)(void *, size_t);
static void (*libc_free)(void *);

/* ---- Address hash map for numa_alloc tracking ---- */
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

/* ---- Rules ---- */
struct addr_rule {
    unsigned long start;
    unsigned long end;
    int node;
};

struct name_rule {
    char pattern[MAX_FUNC_NAME];
    int node;
};

/* Size-based rules: match allocation size to NUMA node */
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
    rules_loaded = 1;
    const char *conf = getenv("KUMF_CONF");
    if (!conf) conf = "kumf.conf";
    FILE *f = fopen(conf, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long start, end;
        int node;
        if (sscanf(line, "0x%lx-0x%lx = %d", &start, &end, &node) == 3 && num_addr_rules < MAX_ADDR_RULES) {
            addr_rules[num_addr_rules].start = start;
            addr_rules[num_addr_rules].end = end;
            addr_rules[num_addr_rules].node = node;
            num_addr_rules++;
        } else if (strncmp(line, "size_gt:", 9) == 0) {
            /* size_gt:BYTES = NODE — match allocations larger than BYTES */
            size_t threshold;
            if (sscanf(line + 9, "%zu = %d", &threshold, &node) == 2 && num_size_rules < MAX_NAME_RULES) {
                size_rules[num_size_rules].min_size = threshold;
                size_rules[num_size_rules].max_size = 0; /* no upper bound */
                size_rules[num_size_rules].node = node;
                num_size_rules++;
            }
        } else if (strncmp(line, "size_lt:", 9) == 0) {
            /* size_lt:BYTES = NODE — match allocations smaller than BYTES */
            size_t threshold;
            if (sscanf(line + 9, "%zu = %d", &threshold, &node) == 2 && num_size_rules < MAX_NAME_RULES) {
                size_rules[num_size_rules].min_size = 0; /* no lower bound */
                size_rules[num_size_rules].max_size = threshold;
                size_rules[num_size_rules].node = node;
                num_size_rules++;
            }
        } else if (strncmp(line, "size_range:", 11) == 0) {
            /* size_range:MIN-MAX = NODE — match MIN <= size < MAX */
            size_t min_s, max_s;
            if (sscanf(line + 11, "%zu-%zu = %d", &min_s, &max_s, &node) == 3 && num_size_rules < MAX_NAME_RULES) {
                size_rules[num_size_rules].min_size = min_s;
                size_rules[num_size_rules].max_size = max_s;
                size_rules[num_size_rules].node = node;
                num_size_rules++;
            }
        } else {
            char pat[MAX_FUNC_NAME];
            if (sscanf(line, "%255s = %d", pat, &node) == 2 && num_name_rules < MAX_NAME_RULES) {
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

static int kumf_debug = -1;

static int get_debug(void) {
    if (kumf_debug < 0)
        kumf_debug = getenv("KUMF_DEBUG") ? 1 : 0;
    return kumf_debug;
}

#define KUMF_LOG(fmt, ...) do { if (get_debug()) fprintf(stderr, "[KUMF] " fmt "\n", ##__VA_ARGS__); } while(0)

/* Resolve a node for an allocation: size rules first, then caller addr */
static int resolve_node(void *caller, size_t sz) {
    int node = match_size(sz);
    if (node >= 0) {
        KUMF_LOG("size_rule matched: sz=%zu -> node %d", sz, node);
        return node;
    }
    node = match_addr(caller);
    if (node >= 0) {
        KUMF_LOG("addr_rule matched: caller=%p sz=%zu -> node %d", caller, sz, node);
        return node;
    }
    return -1;
}

/* ---- LD_PRELOAD hooks ---- */
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
        /* Was a numa allocation - must manually realloc */
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
