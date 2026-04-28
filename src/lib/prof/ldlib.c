#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define __USE_GNU
#include <stdio.h>
#include <sys/stat.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <execinfo.h>
#include <new>
#include <sys/types.h>
#include <sys/syscall.h>
#include <numa.h>
#include <dlfcn.h>

/* ---- State ---- */
static int prof_initialized = 0;
static pthread_mutex_t prof_cleanup_lock = PTHREAD_MUTEX_INITIALIZER;
static int prof_cleanup_done = 0;

#define ARR_SIZE 1000000              /* Max number of malloc per core (ARM: reduced from 950M, 1M×96B≈96MB/thread) */
#define MAX_TID 512                /* Max number of tids to profile */

#define USE_FRAME_POINTER   0      /* Use Frame Pointers to compute the stack trace (faster) */
#define CALLCHAIN_SIZE      16     /* stack trace length (ARM needs deeper for backtrace) */
#define RESOLVE_SYMBS       1      /* Resolve symbols at the end of the execution; quite costly */

/* Debug: set KUMF_PROF_DEBUG=1 to dump backtrace info to stderr at exit */
static int kumf_prof_debug = -1;
#define PROF_DEBUG(fmt, ...) do { if (kumf_prof_debug == 1) fprintf(stderr, "[PROF] " fmt "\n", ##__VA_ARGS__); } while(0)

#define NB_ALLOC_TO_IGNORE   0     /* Ignore the first X allocations. */
#define IGNORE_FIRST_PROCESS 0     /* Ignore the first process (and all its threads). Useful for processes */

#if IGNORE_FIRST_PROCESS
static int first_pid;
#endif
#if NB_ALLOC_TO_IGNORE > 0
static int __thread nb_allocs = 0;
#endif

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int __thread pid;
static int __thread tid;
static int __thread tid_index;
static int tids[MAX_TID];

struct log {
    uint64_t rdt;
    void *addr;
    size_t size;
    long entry_type; /* 0 free 1 malloc >=100 mmap */

    size_t callchain_size;
    void *callchain_strings[CALLCHAIN_SIZE];
    void *caller_addr;        /* __builtin_return_address(0) — direct caller of malloc, bypass PLT */
};
struct log *log_arr[MAX_TID];
static size_t log_index[MAX_TID];

void __attribute__((constructor)) m_init(void);

/* Timestamp: portable across x86 and ARM */
#include <time.h>
static inline void kumf_get_ts(uint64_t *val) {
    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &_ts);
    *val = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
}
#define rdtscll(val) kumf_get_ts(&(val))

static int __thread _in_trace = 0;
#ifdef __aarch64__
#define get_bp(bp) asm("mov %0, x29" : "=r" (bp) :)
#else
#define get_bp(bp) asm("movq %%rbp, %0" : "=r" (bp) :)
#endif

static __attribute__((unused)) int in_first_dlsym = 0;
static char empty_data[65536];  /* dlsym may need larger calloc during init */

static void *(*libc_malloc)(size_t);
static void *(*libc_calloc)(size_t, size_t) = nullptr;  // 避免重复初始化
static void *(*libc_realloc)(void *, size_t);
static void (*libc_free)(void *);

static void *(*libc_mmap)(void *, size_t, int, int, int, off_t);
static void *(*libc_mmap64)(void *, size_t, int, int, int, off_t);
static int (*libc_munmap)(void *, size_t);
static void *(*libc_memalign)(size_t, size_t);
static int (*libc_posix_memalign)(void **, size_t, size_t);

FILE *open_file(int tid)
{
    char buff[125];
    sprintf(buff, "/tmp/kumf/data.raw.%d", tid);

    FILE *dump = fopen(buff, "a+");
    if (!dump) {
        // Auto-create /tmp/kumf/ if missing
        mkdir("/tmp/kumf", 0755);
        dump = fopen(buff, "a+");
    }
    if (!dump) {
        fprintf(stderr, "open %s failed\n", buff);
        exit(-1);
    }
    return dump;
}

struct log *get_log()
{
#if IGNORE_FIRST_PROCESS
    if (!first_pid)
        first_pid = _pid;
    if (_pid == first_pid)
        return NULL;
#endif

#if NB_ALLOC_TO_IGNORE > 0
    nb_allocs++;
    if (nb_allocs < NB_ALLOC_TO_IGNORE)
        return NULL;
#endif
    int i;
    if (!tid) {
        tid = (int) syscall(__NR_gettid); /* portable: x86_64=186, aarch64=178 */
        pthread_mutex_lock(&lock);
        for (i = 1; i < MAX_TID; i++) {
            if (tids[i] == 0) {
                tids[i] = tid;
                tid_index = i;
                break;
            }
        }
        if (tid_index == 0) {
            fprintf(stderr, "Too many threads!\n");
            exit(-1);
        }
        pthread_mutex_unlock(&lock);
    }
    if (!log_arr[tid_index])
        log_arr[tid_index] = (struct log*) libc_malloc(sizeof(*log_arr[tid_index]) * ARR_SIZE);
    if (log_index[tid_index] >= ARR_SIZE)
        return NULL;

    struct log *l = &log_arr[tid_index][log_index[tid_index]];
    /* printf("%d %d \n", tid_index, (int)log_index[tid_index]); */
    log_index[tid_index]++;
    return l;
}

int get_trace(size_t *size, void **strings)
{
    if (_in_trace)
        return 1;
    _in_trace = 1;

#if USE_FRAME_POINTER
    int i;
    struct stack_frame *frame;
    get_bp(frame);
    for (i = 0; i < CALLCHAIN_SIZE; i++) {
        strings[i] = (void*)frame->return_address;
        *size = i + 1;
        frame = frame->next_frame;
        if (!frame)
            break;
    }
#else
    *size = backtrace(strings, CALLCHAIN_SIZE);
#endif
    _in_trace = 0;
    return 0;
}

int _getpid()
{
    if (pid)
        return pid;
    return pid = getpid();
}

extern "C" void *malloc(size_t sz)
{
    if (!libc_malloc)
        m_init();
    void *addr;
    struct log *log_arr;
    addr = libc_malloc(sz);
    if (!_in_trace) {
        log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = addr;
            log_arr->size = sz;
            log_arr->entry_type = 1;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return addr;
}

extern "C" void *calloc(size_t nmemb, size_t size)
{
    void *addr;
    if (!libc_calloc) {
        memset(empty_data, 0, sizeof(empty_data));
        addr = empty_data;
    } else {
        addr = libc_calloc(nmemb, size);
    }
    if (!_in_trace && libc_calloc) {
        struct log *log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = addr;
            log_arr->size = nmemb * size;
            log_arr->entry_type = 3;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return addr;
}

extern "C" void *realloc(void *ptr, size_t size)
{
    void *addr = libc_realloc(ptr, size);
    if (!_in_trace) {
        struct log *log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = addr;
            log_arr->size = size;
            log_arr->entry_type = 4;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return addr;
}

extern "C" void *memalign(size_t align, size_t sz)
{
    void *addr = libc_memalign(align, sz);
    if (!_in_trace) {
        struct log *log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = addr;
            log_arr->size = sz;
            log_arr->entry_type = 5;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return addr;
}

extern "C" int posix_memalign(void **ptr, size_t align, size_t sz)
{
    int ret = libc_posix_memalign(ptr, align, sz);
    if (!_in_trace) {
        struct log *log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = *ptr;
            log_arr->size = sz;
            log_arr->entry_type = 6;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return ret;
}

extern "C" void free(void *p)
{
    struct log *log_arr;
    if (!_in_trace && libc_free) {
        log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = p;
            log_arr->size = 0;
            log_arr->entry_type = 2;
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    libc_free(p);
}

void *operator new(size_t sz) noexcept(false)
{
    return malloc(sz);
}

void *operator new(size_t sz, const std::nothrow_t &) noexcept
{
    return malloc(sz);
}

void *operator new[](size_t sz) noexcept(false)
{
    return malloc(sz);
}

void *operator new[](size_t sz, const std::nothrow_t &) noexcept
{
    return malloc(sz);
}

void operator delete(void *ptr)
{
    free(ptr);
}

void operator delete[](void *ptr)
{
    free(ptr);
}

extern "C" void *mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *addr = libc_mmap(start, length, prot, flags, fd, offset);

    if (!_in_trace) {
        struct log *log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = addr;
            log_arr->size = length;
            log_arr->entry_type = flags + 100;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return addr;
}

extern "C" void *mmap64(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *addr = libc_mmap64(start, length, prot, flags, fd, offset);

    if (!_in_trace) {
        struct log *log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->addr = addr;
            log_arr->size = length;
            log_arr->entry_type = flags + 200;
            log_arr->caller_addr = __builtin_return_address(0);
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return addr;
}

extern "C" int munmap(void *start, size_t length)
{
    int ret = libc_munmap(start, length);
    if (!_in_trace && libc_free) {
        struct log *l = get_log();
        if (l) {
            rdtscll(l->rdt);
            l->addr = start;
            l->size = length;
            l->entry_type = 90;
            l->caller_addr = __builtin_return_address(0);
            get_trace(&l->callchain_size, l->callchain_strings);
        }
    }
    return ret;
}

int __thread bye_done = 0;

static void prof_cleanup(void)
{
    pthread_mutex_lock(&prof_cleanup_lock);
    if (prof_cleanup_done) { pthread_mutex_unlock(&prof_cleanup_lock); return; }
    prof_cleanup_done = 1;
    pthread_mutex_unlock(&prof_cleanup_lock);

    /* Init debug flag */
    if (kumf_prof_debug < 0)
        kumf_prof_debug = getenv("KUMF_PROF_DEBUG") ? 1 : 0;

    unsigned int i, j, k;
    int backtrace_ok = 0;
    for (i = 1; i < MAX_TID; i++) {
        if (tids[i] == 0)
            break;
        FILE *dump = open_file(tids[i]);
        for (j = 0; j < log_index[i]; j++) {
            struct log *l = &log_arr[i][j];
            #if RESOLVE_SYMBS
            _in_trace = 1;
            char **strings = (l->callchain_size > 0) ? backtrace_symbols(l->callchain_strings, l->callchain_size) : NULL;
            /* Output: caller_addr symbol (via __builtin_return_address, bypass PLT) */
            char caller_sym[128] = {0};
            if (l->caller_addr && l->caller_addr != (void*)-1L) {
                Dl_info info;
                if (dladdr(l->caller_addr, &info) && info.dli_sname) {
                    snprintf(caller_sym, sizeof(caller_sym), "%s+0x%lx", info.dli_sname,
                             (unsigned long)((char*)l->caller_addr - (char*)info.dli_saddr));
                } else {
                    snprintf(caller_sym, sizeof(caller_sym), "[%p]", l->caller_addr);
                }
            } else {
                snprintf(caller_sym, sizeof(caller_sym), "[init]");
            }
            fprintf(dump, "%s ", caller_sym);
            backtrace_ok = 1;

            if (strings) libc_free(strings);
            #else
            char caller_sym[128] = {0};
            if (l->caller_addr && l->caller_addr != (void*)-1L) {
                Dl_info info;
                if (dladdr(l->caller_addr, &info) && info.dli_sname) {
                    snprintf(caller_sym, sizeof(caller_sym), "%s+0x%lx", info.dli_sname,
                             (unsigned long)((char*)l->caller_addr - (char*)info.dli_saddr));
                } else {
                    snprintf(caller_sym, sizeof(caller_sym), "[%p]", l->caller_addr);
                }
            } else {
                snprintf(caller_sym, sizeof(caller_sym), "[init]");
            }
            fprintf(dump, "%s ", caller_sym);
            #endif
            fprintf(dump, "%lu %lu %lx %d\n", l->rdt, (long unsigned)l->size, (long unsigned)l->addr, (int)l->entry_type);
        }
        fclose(dump);
    }

    /* Debug: report backtrace status */
    if (kumf_prof_debug) {
        PROF_DEBUG("Total threads: %d, backtrace_ok=%d", i-1, backtrace_ok);
        if (!backtrace_ok) {
            PROF_DEBUG("WARNING: backtrace returned < 4 frames.");
            PROF_DEBUG("ARM64 needs -fno-omit-frame-pointer or libunwind.");
            PROF_DEBUG("Try: gcc -fno-omit-frame-pointer ...");
        }
    }
}

void __attribute__((constructor)) m_init(void)
{
    if (prof_initialized) return;
    prof_initialized = 1;

    libc_malloc = (void * ( *)(size_t))dlsym(RTLD_NEXT, "malloc");
    libc_realloc = (void * ( *)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    libc_calloc = (void * ( *)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    libc_free = (void ( *)(void *))dlsym(RTLD_NEXT, "free");
    libc_mmap = (void * ( *)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    libc_munmap = (int ( *)(void *, size_t))dlsym(RTLD_NEXT, "munmap");
    libc_mmap64 = (void * ( *)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap64");
    libc_memalign = (void * ( *)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
    libc_posix_memalign = (int ( *)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");

    /* Register cleanup via atexit (safer than destructor during glibc unwind) */
    atexit(prof_cleanup);
}
