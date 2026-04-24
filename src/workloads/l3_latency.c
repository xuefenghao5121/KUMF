/*
 * L3 cache latency microbenchmark for ARM64
 * Pointer chase with compiler barrier to prevent optimization
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define CACHE_LINE 64

static inline uint64_t ns_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Force the compiler to actually dereference */
static inline volatile char *chase(volatile char *p) {
    volatile char *next = *(volatile char * volatile *)p;
    /* ARM64 data memory barrier to prevent speculation/optimization */
    __asm__ volatile("dmb ish" ::: "memory");
    return next;
}

int main(int argc, char *argv[]) {
    int iterations = 5;
    
    printf("L3 Latency Benchmark (ARM64)\n");
    printf("==========================================\n\n");
    
    /* Cache hierarchy scan */
    size_t test_sizes_kb[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    int ntests = sizeof(test_sizes_kb) / sizeof(test_sizes_kb[0]);
    
    printf("%8s %12s %12s\n", "Size", "ns/access", "est.cycles");
    printf("------------------------------------------\n");
    
    for (int t = 0; t < ntests; t++) {
        size_t size = test_sizes_kb[t] * 1024;
        size_t n = size / CACHE_LINE;
        if (n < 10) continue;
        
        char *buf = NULL;
        if (posix_memalign((void **)&buf, CACHE_LINE, size) != 0) {
            printf("%8zu KB:  alloc failed\n", test_sizes_kb[t]);
            continue;
        }
        
        /* Build shuffled pointer chain */
        size_t *idx = malloc(n * sizeof(size_t));
        for (size_t i = 0; i < n; i++) idx[i] = i;
        
        unsigned int seed = 42;
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = rand_r(&seed) % (i + 1);
            size_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
        }
        
        for (size_t i = 0; i < n - 1; i++) {
            *(volatile char **)(buf + idx[i] * CACHE_LINE) = buf + idx[i+1] * CACHE_LINE;
        }
        *(volatile char **)(buf + idx[n-1] * CACHE_LINE) = buf + idx[0] * CACHE_LINE;
        
        volatile char *p = buf + idx[0] * CACHE_LINE;
        
        /* Warmup: bring into cache */
        for (size_t i = 0; i < n; i++) p = chase(p);
        
        /* Measure multiple iterations */
        double best_ns = 1e9;
        for (int iter = 0; iter < iterations; iter++) {
            uint64_t start = ns_time();
            for (size_t i = 0; i < n; i++) {
                p = chase(p);
            }
            uint64_t end = ns_time();
            double ns = (double)(end - start) / n;
            if (ns < best_ns) best_ns = ns;
        }
        
        /* Estimate cycles: Kunpeng 930 typical 2.6 GHz */
        double cycles = best_ns * 2.6;
        
        printf("%8zu KB: %8.2f ns  ~%.1f cycles\n", test_sizes_kb[t], best_ns, cycles);
        
        free(buf);
        free(idx);
    }
    
    printf("\nNote: cycles estimated at 2.6 GHz (Kunpeng 930 typical)\n");
    return 0;
}
