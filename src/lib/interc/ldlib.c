#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define __USE_GNU
#include <stdio.h>
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

#define ARR_SIZE 550000            /* Max number of malloc per core */
#define MAX_TID 512                /* Max number of tids to profile */

#define USE_FRAME_POINTER   0      /* Use Frame Pointers to compute the stack trace (faster) */
#define CALLCHAIN_SIZE      5      /* stack trace length */
#define RESOLVE_SYMBS       1      /* Resolve symbols at the end of the execution; quite costly */

#define NB_ALLOC_TO_IGNORE   0     /* Ignore the first X allocations. */
#define IGNORE_FIRST_PROCESS 0     /* Ignore the first process (and all its threads). Useful for processes */

#define MAX_OBJECTS          30000

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

struct addr_seg {
    long unsigned start;
    long unsigned end;
};

static struct addr_seg addr_segs[MAX_OBJECTS];

struct log {
    uint64_t rdt;
    void *addr;
    size_t size;
    long entry_type; /* 0 free 1 malloc >=100 mmap */
    size_t callchain_size;
    void *callchain_strings[CALLCHAIN_SIZE];
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
static char empty_data[32];

static void *(*libc_malloc)(size_t);
static void *(*libc_calloc)(size_t, size_t);
static void *(*libc_realloc)(void *, size_t);
static void (*libc_free)(void *);

static void *(*libc_mmap)(void *, size_t, int, int, int, off_t);
static void *(*libc_mmap64)(void *, size_t, int, int, int, off_t);
static int (*libc_munmap)(void *, size_t);
static void *(*libc_memalign)(size_t, size_t);
static int (*libc_posix_memalign)(void **, size_t, size_t);

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
    *size = backtrace(strings, 10);
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

/* KUMF: configuration-driven object placement */
#define MAX_TRACE_RULES 256
#define MAX_FUNC_NAME 256

struct kumf_rule {
    char func_name[MAX_FUNC_NAME];
    int numa_node;  /* -1=default, 0-3=specific node */
};

static struct kumf_rule kumf_rules[MAX_TRACE_RULES];
static int kumf_num_rules = 0;
static int kumf_rules_loaded = 0;

static void kumf_load_rules(void) {
    if (kumf_rules_loaded) return;
    kumf_rules_loaded = 1;
    const char *conf = getenv("KUMF_CONF");
    if (!conf) conf = "kumf.conf";
    FILE *f = fopen(conf, "r");
    if (!f) return;  /* no config = no NUMA-aware allocation */
    char line[512];
    while (fgets(line, sizeof(line), f) && kumf_num_rules < MAX_TRACE_RULES) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char func[MAX_FUNC_NAME];
        int node;
        if (sscanf(line, "%255s = %d", func, &node) == 2) {
            strncpy(kumf_rules[kumf_num_rules].func_name, func, MAX_FUNC_NAME-1);
            kumf_rules[kumf_num_rules].numa_node = node;
            kumf_num_rules++;
        }
    }
    fclose(f);
}

/* check_trace: returns NUMA node (0-3) or -1 for default */
int check_trace(void *string, size_t sz)
{
    kumf_load_rules();
    char *ptr = (char *) string;
    for (int i = 0; i < kumf_num_rules; i++) {
        if (strstr(ptr, kumf_rules[i].func_name) != NULL) {
            return kumf_rules[i].numa_node;
        }
    }
    return -1;
}

void record_seg(unsigned long addr, size_t size)
{
    int i;
    for (i = 0; i < MAX_OBJECTS; i += 1) {
        struct addr_seg *seg = &addr_segs[i];
        if (seg->start == 0 && seg->end == 0) {
            seg->start = addr;
            seg->end = addr + size;
            return;
        }
    }
}

size_t check_seg(unsigned long addr)
{
    int i;
    size_t size_to_free;
    size_to_free = 0;
    for (i = 0; i < MAX_OBJECTS; i += 1) {
        struct addr_seg *seg = &addr_segs[i];
        if (seg->start <= addr && seg->end > addr) {
            size_to_free = (size_t) (seg->end - addr);
            if (seg->start == addr) {
                seg->start = 0;
                seg->end = 0;
            } else {
                seg->end = addr;
            }
            break;
        }
    }
    return size_to_free;
}

extern "C" void *malloc(size_t sz)
{
    if (!libc_malloc)
        m_init();
    void *addr;
    struct log *log_arr;
    if (!_in_trace) {
        log_arr = get_log();
        if (log_arr) {
            rdtscll(log_arr->rdt);
            log_arr->size = sz;
            log_arr->entry_type = 1;
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    if (sz > 4096 && !_in_trace && log_arr) {
        if (log_arr->callchain_size >= 4) {
            char **strings = backtrace_symbols (log_arr->callchain_strings, log_arr->callchain_size);
            int ret = check_trace(strings[3], sz);
            libc_free(strings);
            if (ret > -1) {
                addr = numa_alloc_onnode(sz, ret);
                record_seg((unsigned long)addr, sz);
            } else {
                addr = libc_malloc(sz);
            }
        } else {
            addr = libc_malloc(sz);
        }
    } else {
        addr = libc_malloc(sz);
    }
    return addr;
}

extern "C" void *calloc(size_t nmemb, size_t size)
{
    void *addr;
    if (!libc_calloc) {
        memset(empty_data, 0, sizeof(*empty_data));
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
            log_arr->entry_type = 1;
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
            log_arr->entry_type = 1;
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
            log_arr->entry_type = 1;
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
            log_arr->entry_type = 1;
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }
    return ret;
}

extern "C" void free(void *p)
{
    if (!_in_trace && libc_free) {
        size_t size_to_free = check_seg((unsigned long) p);
        if (size_to_free > 0) {
            numa_free(p, size_to_free);
            return;
        } else {
            libc_free(p);
            return;
        }
    } else {
        libc_free(p);
    }
    /* libc_free(p); */
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
            log_arr->size = length * 4 * 1024;
            log_arr->entry_type = flags + 100;
            get_trace(&log_arr->callchain_size, log_arr->callchain_strings);
        }
    }

    return addr;
}

extern "C" int munmap(void *start, size_t length)
{
    int addr = libc_munmap(start, length);
    struct log log_arr;
    rdtscll(log_arr.rdt);
    log_arr.addr = start;
    log_arr.size = length;
    log_arr.entry_type = 2;
    return addr;
}

int __thread bye_done = 0;
void __attribute__((destructor)) bye(void)
{
    if (bye_done)
        return;
    bye_done = 1;
}

void __attribute__((constructor)) m_init(void)
{
    libc_malloc = (void * ( *)(size_t))dlsym(RTLD_NEXT, "malloc");
    libc_realloc = (void * ( *)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    libc_calloc = (void * ( *)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    libc_free = (void ( *)(void *))dlsym(RTLD_NEXT, "free");
    libc_mmap = (void * ( *)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    libc_munmap = (int ( *)(void *, size_t))dlsym(RTLD_NEXT, "munmap");
    libc_mmap64 = (void * ( *)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap64");
    libc_memalign = (void * ( *)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
    libc_posix_memalign = (int ( *)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
}
