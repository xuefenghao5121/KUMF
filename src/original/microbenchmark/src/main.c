/*
 * Tiered Memory Management Microbenchmark
 *
 * This microbenchmark generates two distinct memory access patterns:
 * 1. Pointer-chasing: Random access pattern with low memory bandwidth
 * 2. Sequential read: Linear access pattern with high memory bandwidth
 *
 * These patterns are designed to evaluate tiered memory systems by creating
 * workloads with different memory access characteristics and intensities.
 */

#define _GNU_SOURCE

#include "utils.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>
#include <time.h>

#include <numa.h>
#include <numaif.h>

#include <stdint.h>
#include <stdbool.h>

#include <assert.h>
#include <immintrin.h>

pthread_barrier_t barrier;
pthread_barrier_t alloc_barrier;

void read_loop(void *array, size_t size)
{
    size_t *carray = (size_t *) array;
    size_t val = 0;
    size_t i;

    for (i = 0; i < size / sizeof(size_t); i++) {
        val += carray[i];
    }
    assert(val != 0xdeadbeef);
}

void swap(chase_t *n1, chase_t *n2)
{
    chase_t *temp;

    temp = n1->ptr_arr[0];
    n1->ptr_arr[0] = n2->ptr_arr[0];
    n2->ptr_arr[0] = temp;

    n1->ptr_arr[0]->ptr_arr[1] = n1;
    n2->ptr_arr[0]->ptr_arr[1] = n2;

    temp = n1->ptr_arr[1];
    n1->ptr_arr[1] = n2->ptr_arr[1];
    n2->ptr_arr[1] = temp;

    n1->ptr_arr[1]->ptr_arr[0] = n1;
    n2->ptr_arr[1]->ptr_arr[0] = n2;
}

void shuffle(header_t *header)
{
    srand(time(NULL));
    chase_t *curr_ptr = (chase_t *)header->start_addr_a;
    uint64_t i = 0;
    uint64_t k = 0;
    int interval = 1048576;

    while (i < header->num_chase_block) {
        uint64_t start = i;
        for (k = 0; k < interval; k += 1) {
            uint64_t j = (uint64_t)(rand() % interval);
            swap((chase_t *) &(curr_ptr[start + k]),
                 (chase_t *) &(curr_ptr[(start + j)]));
        }
        i += k;
    }
}

void verify(header_t *header)
{
    chase_t *curr_ptr;
    chase_t *next_ptr;
    uint64_t i = 0;

    curr_ptr = (chase_t *)header->start_addr_a;
    while (i < header->num_chase_block) {
        if ((int)(curr_ptr->ptr_arr[3]) != 0) {
            printf("ERROR %lu, %lu\n", i, (unsigned long)curr_ptr->ptr_arr[3]);
            break;
        }
        curr_ptr->ptr_arr[3] = (int)curr_ptr->ptr_arr[3] + 1;
        next_ptr = curr_ptr->ptr_arr[0];
        i += 1;
        curr_ptr = next_ptr;
    }
}

uint64_t init_ptr_buf(header_t *header)
{
    chase_t *curr_ptr;
    chase_t *next_ptr;

    curr_ptr = (chase_t *)header->start_addr_a;
    for (uint64_t i = 0; i < header->num_chase_block - 1; i++) {
        next_ptr = &(curr_ptr[1]);
        curr_ptr->ptr_arr[0] = next_ptr;
        next_ptr->ptr_arr[1] = curr_ptr;
        curr_ptr = next_ptr;
    }
    curr_ptr->ptr_arr[0] = (chase_t *)header->start_addr_a;
    curr_ptr->ptr_arr[0]->ptr_arr[1] = curr_ptr;

    if (header->random) {
        for (int i = 0; i < 2; i += 1) {
            shuffle(header);
        }
    }
    verify(header);
    return 0;
}

static void ptr_chase(char *addr, uint64_t num_chase_block)
{
    chase_t *curr_ptr;
    chase_t *next_ptr;
    uint64_t i = 0;
    int val = 0;

    curr_ptr = (chase_t *)addr;
    while (i < num_chase_block) {
        next_ptr = curr_ptr->ptr_arr[0];
        val += (int)curr_ptr->ptr_arr[3];
        i += 1;
        curr_ptr = next_ptr;
    }
    assert(val != 0xdeadbeef);
}

void pointer_chasing(header_t *header)
{
    int *records_buf = (int *) header->records_buf;
    uint64_t interval = header->chase_interval;

    if (header->start_addr_a == NULL) {
        printf("ERROR header->start_addr_a == NULL\n");
        return;
    }
    pthread_barrier_wait(&barrier);
    init_ptr_buf(header);
    for (int i = 0; i < header->op_iter; i += 1) {
        ptr_chase(header->start_addr_a, header->num_chase_block);
    }
    return;
}

void bandwidth(header_t *header)
{
    char *src = header->start_addr_b;

    pthread_barrier_wait(&barrier);
    memset(src, 0xFF, header->buf_size_b);
    *((uint64_t *) &src[header->buf_size_b]) = 0;
    read_loop(src, header->buf_size_b);
    /* TODO: op_iter*26; */
    for (int k = 0; k < header->op_iter * 26; k += 1) {
        read_loop(src, header->buf_size_b);
    }
    return;
}

int stick_this_thread_to_core(int core_id)
{
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t cpuset;
    pthread_t current_thread = pthread_self();

    if (core_id < 0 || core_id >= num_cores) {
        fprintf(stderr, "ERROR core_id < 0 || core_id >= num_cores\n");
        return EINVAL;
    }
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

void *pc_thread(void *arg)
{
    header_t *header = (header_t *)arg;
    int ret = 0;

    ret = stick_this_thread_to_core(header->thread_idx);
    if (ret != 0) {
        printf("ERROR stick_this_thread_to_core\n");
        return NULL;
    }
    ret = init_buf_reg_alloc(header->buf_size_a, &(header->buf_a));
    /* fprintf(stderr, "init_buf_reg_alloc in pc_thread\n"); */
    if (ret < 0) {
        fprintf(stderr, "ERROR init_buf_reg_alloc in thread %d.\n",
                header->thread_idx);
        free(header->buf_a);
        return NULL;
    }
    header->start_addr_a = &(header->buf_a[0]);
    pthread_barrier_wait(&alloc_barrier);
    pointer_chasing(header);
    aligned_free(header->buf_a);
}

void *bw_thread(void *arg)
{
    header_t *header = (header_t *)arg;
    int ret = 0;

    ret = stick_this_thread_to_core(header->thread_idx);
    if (ret != 0) {
        printf("ERROR stick_this_thread_to_core\n");
        return NULL;
    }
    pthread_barrier_wait(&alloc_barrier);
    ret = init_buf_reg_alloc(header->buf_size_b, &(header->buf_b));
    /* fprintf(stderr, "init_buf_reg_alloc in bw_thread\n"); */
    if (ret < 0) {
        fprintf(stderr, "ERROR init_buf in thread %d.\n", header->thread_idx);
        free(header->buf_b);
        return NULL;
    }
    header->start_addr_b = &(header->buf_b[0]);
    bandwidth(header);
    aligned_free(header->buf_b);
}

int run_split(header_t *header)
{
    pthread_t *thread_arr;
    header_t *header_arr;
    header_t *curr_header;
    header_t *header_pc;
    header_t *header_bw;
    int ret, num_thread, ret1, ret2;

    ret = 0;
    if (header->ratio == 0.0 || header->ratio == 1.0) {
        num_thread = 1;
    } else {
        num_thread = 2;
    }
    thread_arr = malloc(num_thread * sizeof(pthread_t));
    header_arr = malloc(num_thread * sizeof(header_t));
    memset(header_arr, 0, num_thread * sizeof(header_t));
    pthread_barrier_init(&barrier, NULL, num_thread);
    pthread_barrier_init(&alloc_barrier, NULL, num_thread);

    if (header->ratio == 0.0) {
        curr_header = &(header_arr[0]);
        memcpy(curr_header, header, sizeof(header_t));
        curr_header->thread_idx = 0;
        curr_header->halt = 0;
        ret = pthread_create(&thread_arr[0], NULL, pc_thread,
                             (void *)curr_header);
    } else if (header->ratio == 1.0) {
        curr_header = &(header_arr[0]);
        memcpy(curr_header, header, sizeof(header_t));
        curr_header->thread_idx = 0;
        curr_header->halt = 0;
        ret = pthread_create(&thread_arr[0], NULL, bw_thread,
                             (void *)curr_header);
    } else {
        header_bw = &(header_arr[0]);
        header_pc = &(header_arr[1]);
        memcpy(header_bw, header, sizeof(header_t));
        memcpy(header_pc, header, sizeof(header_t));
        header_bw->thread_idx = 0;
        header_pc->thread_idx = 1;
        header_bw->halt = 0;
        header_pc->halt = 0;
        ret1 = pthread_create(&thread_arr[0], NULL, bw_thread,
                              (void *)header_bw);
        ret2 = pthread_create(&thread_arr[1], NULL, pc_thread,
                              (void *)header_pc);
    }
    if (header->ratio == 0.0 || header->ratio == 1.0) {
        pthread_join(thread_arr[0], NULL);
    } else {
        pthread_join(thread_arr[0], NULL);
        pthread_join(thread_arr[1], NULL);
    }
    pthread_barrier_destroy(&barrier);
    pthread_barrier_destroy(&alloc_barrier);
    return ret;
}

int main(int argc, char *argv[])
{
    int ret;
    header_t *header;

    header = malloc(sizeof(header_t));
    ret = parse_arg(argc, argv, header);
    if (ret < 0) {
        fprintf(stderr, "ERROR parse_arg\n");
        return 0;
    }
    run_split(header);
    return 0;
}
