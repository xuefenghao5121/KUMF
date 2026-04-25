#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stdbool.h>

#define TSC_FREQ_GHZ  2.1
#define PAGESZ        1 << 12
#define ALIGN         64

typedef struct chase_struct chase_t;

struct chase_struct {
    /* 64-bit addr, 64 * 8 = 512 bit (64bytes) per cacheline */
    chase_t *ptr_arr[8];
};

typedef struct header_struct {
    int print;
    uint64_t num_thread; /* number of threads */
    int buf_a_numa_node; /* which numa node for buffer */
    char *buf_a;
    uint64_t buf_size_a;
    char *buf_b;
    uint64_t buf_size_b;
    char *records_buf;

    double tsc_freq;
    int thread_idx;

    int core_a;
    char *start_addr_a;
    char *start_addr_b;
    uint64_t per_thread_size;

    int op_iter;
    int starting_core;
    bool random;

    uint64_t num_chase_block;
    uint64_t chase_interval;

    float ratio;

    volatile int halt;
} header_t;

int parse_arg(int argc, char *argv[], header_t *header);

int init_buf(uint64_t size, int node, char **alloc_ptr);
int init_buf_reg_alloc(uint64_t size, char **alloc_ptr);
void aligned_free(void *ptr);

void stop_threads(header_t *header_arr, int start_index);

#endif /* UTIL_H */
