#include "utils.h"
#include <stdio.h>
#include <numa.h>
#include <numaif.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>

#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

void set_default(header_t *header)
{
    header->num_thread = 1;
    header->buf_size_a = (1 << 30); /* pc */
    header->buf_size_b = (1 << 30); /* bw */
    header->buf_a_numa_node = 0;
    header->op_iter = 5;
    header->random = true;
    header->halt = 0;
    header->tsc_freq = TSC_FREQ_GHZ;
    header->num_chase_block = header->buf_size_a / sizeof(chase_t);
    assert(header->num_chase_block > 0);
    header->chase_interval = header->num_chase_block;
    header->ratio = 0.0;
}

int parse_arg(int argc, char *argv[], header_t *header)
{
    int opt;
    set_default(header);
    while ((opt = getopt(argc, argv, "t:i:r:I:R:A:B:")) != -1) {
        switch (opt) {
        case 't':
            header->num_thread = atoi(optarg);
            assert(header->num_thread > 0);
            break;
        case 'i':
            header->op_iter = atoi(optarg);
            break;
        case 'r':
            header->buf_a_numa_node = atoi(optarg);
            break;
        case 'I':
            header->chase_interval = atoi(optarg);
            assert(header->num_chase_block % header->chase_interval == 0);
            break;
        case 'R':
            header->ratio = atof(optarg);
            break;
        case 'A': /* pc */
            header->buf_size_a = ((uint64_t)atoi(optarg) * ((1 << 20)));
            header->num_chase_block = header->buf_size_a / sizeof(chase_t);
            break;
        case 'B': /* bw */
            header->buf_size_b = ((uint64_t)atoi(optarg) * ((1 << 20)));
            break;
        }
    }
    return 0;
}

int init_buf(uint64_t size, int node, char **alloc_ptr)
{
    char *ptr;
    unsigned long page_size;
    uint64_t page_cnt;
    uint64_t idx;
    if ((ptr = (char *)numa_alloc_onnode(size, node)) == NULL) {
        fprintf(stderr,"ERROR: numa_alloc_onnode\n");
        return -1;
    }
    page_size = (unsigned long)getpagesize();
    page_cnt = (size / page_size);
    idx = 0;
    for (uint64_t i = 0; i < page_cnt; i++) {
        ptr[idx] = 0;
        idx += page_size;
    }
    *alloc_ptr = ptr;
    return 0;
}

int init_buf_reg_alloc(uint64_t size, char **alloc_ptr)
{
    char *ptr;
    void *mem;
    unsigned long page_size;
    uint64_t page_cnt;
    uint64_t idx;
    if ((mem = (char *)malloc(size + sizeof(void*) + (ALIGN - 1))) == NULL) {
        fprintf(stderr,"ERROR: malloc\n");
        return -1;
    }
    ptr = (void**)((uintptr_t) (mem + (ALIGN - 1) + sizeof(void*)) & ~(ALIGN - 1));
    ((void **) ptr)[-1] = mem;
    page_size = (unsigned long)getpagesize();
    page_cnt = (size / page_size);
    idx = 0;
    size_t val = 0;
    for (uint64_t i = 0; i < page_cnt; i++) {
        val += ptr[idx];
        idx += page_size;
    }
    assert(val != 0xdeadbeef);
    *alloc_ptr = ptr;
    return 0;
}

void aligned_free(void *ptr)
{
    free(((void**) ptr)[-1]);
}

void stop_threads(header_t *header_arr, int start_index)
{
    int num_thread = header_arr[0].num_thread;
    for (int i = start_index; i < num_thread; i++) {
        header_arr[i].halt = 1;
    }
}
