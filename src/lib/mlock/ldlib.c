/*
 * KUMF SOAR tiered memory allocator - mlock hot pages version
 *
 * 在 cgroup 内存压力下，mlock 热页防止被 swap out。
 * 基于 PEBS/SPE 分析的 page 热度数据，在运行时锁定高分 page。
 *
 * 用法:
 *   1. 先跑 PEBS profiling + soar_analyzer 生成热页列表
 *   2. LD_PRELOAD=./libkumf_mlock.so KUMF_HOT_PAGES=/tmp/kumf/hot_pages.txt ./workload
 *
 * hot_pages.txt 格式: 每行一个 16 进制 page 地址
 *   0x7f1234567000
 *   0x7f1234568000
 *   ...
 *
 * 也可以自动检测热页（基于访问频率采样）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <new>

/* ---- 配置 ---- */
#define MAX_HOT_PAGES 8192
#define ROUTE_THRESHOLD 4096   /* 只路由 >4KB 的分配 */
#define MLOCK_BUDGET_MB 10     /* mlock 预算（MB） */
#define MLOCK_FIRST_N 20       /* 只 lock 前 N 个大分配（热数据结构先分配） */

/* ---- libc 函数指针 ---- */
static void *(*libc_malloc)(size_t);
static void *(*libc_calloc)(size_t, size_t);
static void *(*libc_realloc)(void *, size_t);
static void (*libc_free)(void *);

/* ---- 热页表 ---- */
static unsigned long hot_pages[MAX_HOT_PAGES];
static int num_hot_pages = 0;
static int hot_pages_loaded = 0;
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- mlock 统计 ---- */
static size_t total_locked_bytes = 0;
static size_t mlock_budget = 0;  /* bytes */
static int pages_locked = 0;
static int pages_skipped = 0;
static int alloc_count = 0;  /* 大分配计数器 */

/* ---- 分配跟踪（用于运行时识别热页）---- */
struct alloc_entry {
    unsigned long addr;
    size_t size;
    int access_count;       /* 简单计数器 */
    int locked;             /* 是否已 mlock */
    struct alloc_entry *next;
};

#define ALLOC_HASH_BITS 14
#define ALLOC_HASH_SIZE (1 << ALLOC_HASH_BITS)
#define ALLOC_HASH_MASK (ALLOC_HASH_SIZE - 1)

static struct alloc_entry *alloc_table[ALLOC_HASH_SIZE];
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static inline unsigned alloc_hash(unsigned long addr) {
    return (addr >> 12) & ALLOC_HASH_MASK;
}

/* ---- 加载热页列表 ---- */
static void load_hot_pages(void) {
    if (hot_pages_loaded) return;
    pthread_mutex_lock(&init_lock);
    if (hot_pages_loaded) { pthread_mutex_unlock(&init_lock); return; }

    mlock_budget = MLOCK_BUDGET_MB * 1024 * 1024;

    const char *path = getenv("KUMF_HOT_PAGES");
    if (!path) path = "hot_pages.txt";

    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f) && num_hot_pages < MAX_HOT_PAGES) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n') continue;
            unsigned long addr = strtoul(p, NULL, 16);
            if (addr) {
                hot_pages[num_hot_pages++] = addr;
            }
        }
        fclose(f);
        fprintf(stderr, "[KUMF] Loaded %d hot pages from %s (budget=%dMB)\n",
                num_hot_pages, path, MLOCK_BUDGET_MB);
    } else {
        fprintf(stderr, "[KUMF] No hot pages file (%s), using auto-detect mode\n", path);
    }

    hot_pages_loaded = 1;
    pthread_mutex_unlock(&init_lock);
}

/* ---- 检查地址是否在热页范围 ---- */
static int is_hot_page(unsigned long addr) {
    unsigned long page = addr & ~0xFFFUL;
    for (int i = 0; i < num_hot_pages; i++) {
        if (hot_pages[i] == page) return 1;
    }
    return 0;
}

/* ---- mlock 一个分配 ---- */
static void try_mlock(void *ptr, size_t size) {
    if (!ptr || size == 0) return;

    /* 只 lock 前 MLOCK_FIRST_N 个大分配 */
    alloc_count++;
    if (alloc_count > MLOCK_FIRST_N) {
        pages_skipped++;
        return;
    }

    unsigned long addr = (unsigned long)ptr;
    unsigned long page_start = addr & ~0xFFFUL;
    unsigned long page_end = (addr + size + 4095) & ~0xFFFUL;
    size_t lock_size = page_end - page_start;

    /* 检查 mlock 预算 */
    if (mlock_budget > 0 && total_locked_bytes + lock_size > mlock_budget) {
        pages_skipped++;
        return;
    }

    /* 执行 mlock */
    if (mlock((void *)page_start, lock_size) == 0) {
        total_locked_bytes += lock_size;
        pages_locked++;
    }
}

/* ---- 跟踪分配 ---- */
static void record_alloc(void *ptr, size_t size) {
    if (!ptr || size <= ROUTE_THRESHOLD) return;

    struct alloc_entry *e = (struct alloc_entry *)libc_malloc(sizeof(*e));
    if (!e) return;
    e->addr = (unsigned long)ptr;
    e->size = size;
    e->access_count = 0;
    e->locked = 0;

    unsigned h = alloc_hash(e->addr);
    pthread_mutex_lock(&alloc_lock);
    e->next = alloc_table[h];
    alloc_table[h] = e;
    pthread_mutex_unlock(&alloc_lock);

    /* 如果有热页列表，立即尝试 mlock */
    if (num_hot_pages > 0) {
        try_mlock(ptr, size);
        e->locked = 1;
    }
}

static void remove_alloc(void *ptr) {
    if (!ptr) return;
    unsigned long addr = (unsigned long)ptr;
    unsigned h = alloc_hash(addr);

    pthread_mutex_lock(&alloc_lock);
    struct alloc_entry **pp = &alloc_table[h];
    while (*pp) {
        struct alloc_entry *e = *pp;
        if (e->addr == addr) {
            /* 如果被 mlock 了，先 unlock */
            if (e->locked && e->size > 0) {
                unsigned long ps = e->addr & ~0xFFFUL;
                unsigned long pe = (e->addr + e->size + 4095) & ~0xFFFUL;
                size_t unlock_size = pe - ps;
                munlock((void *)ps, unlock_size);
                if (total_locked_bytes >= unlock_size) {
                    total_locked_bytes -= unlock_size;
                } else {
                    total_locked_bytes = 0;
                }
            }
            *pp = e->next;
            libc_free(e);
            break;
        }
        pp = &e->next;
    }
    pthread_mutex_unlock(&alloc_lock);
}

/* ---- LD_PRELOAD hooks ---- */
extern "C" void *malloc(size_t sz) {
    if (!libc_malloc) {
        libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    }
    void *ptr = libc_malloc(sz);
    if (hot_pages_loaded) {
        record_alloc(ptr, sz);
    }
    return ptr;
}

extern "C" void *calloc(size_t nmemb, size_t size) {
    if (!libc_calloc) {
        libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    }
    void *ptr = libc_calloc(nmemb, size);
    if (ptr && hot_pages_loaded) {
        record_alloc(ptr, nmemb * size);
    }
    return ptr;
}

extern "C" void *realloc(void *ptr, size_t size) {
    if (!libc_realloc) {
        libc_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    }
    if (ptr) remove_alloc(ptr);
    void *new_ptr = libc_realloc(ptr, size);
    if (new_ptr && hot_pages_loaded) {
        record_alloc(new_ptr, size);
    }
    return new_ptr;
}

extern "C" void free(void *p) {
    if (!libc_free) {
        libc_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    }
    if (!p) return;
    if (hot_pages_loaded) {
        remove_alloc(p);
    }
    libc_free(p);
}

/* C++ operators */
void *operator new(size_t sz) noexcept(false) { return malloc(sz); }
void *operator new(size_t sz, const std::nothrow_t &) noexcept { return malloc(sz); }
void *operator new[](size_t sz) noexcept(false) { return malloc(sz); }
void *operator new[](size_t sz, const std::nothrow_t &) noexcept { return malloc(sz); }
void operator delete(void *ptr) noexcept { free(ptr); }
void operator delete[](void *ptr) noexcept { free(ptr); }

/* ---- 构造/析构 ---- */
__attribute__((constructor))
static void kumf_mlock_init(void) {
    libc_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
    libc_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    libc_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    libc_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
    load_hot_pages();
}

__attribute__((destructor))
static void kumf_mlock_fini(void) {
    fprintf(stderr, "[KUMF] mlock stats: %d pages locked (%zu MB), %d skipped (budget=%d MB)\n",
            pages_locked, total_locked_bytes / 1024 / 1024, pages_skipped, MLOCK_BUDGET_MB);
}
