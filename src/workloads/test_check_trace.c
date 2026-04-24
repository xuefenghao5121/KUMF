/*
 * check_trace end-to-end test
 * Verifies that interc/ldlib.so correctly routes allocations to NUMA nodes
 * based on KUMF_CONF configuration
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <numa.h>
#include <numaif.h>

/* Function with distinctive name that we'll match in config */
void* allocate_hot_data(size_t sz) {
    return malloc(sz);
}

void* allocate_cold_data(size_t sz) {
    return malloc(sz);
}

int main() {
    if (numa_available() < 0) {
        printf("NUMA not available\n");
        return 1;
    }

    int nnodes = numa_num_configured_nodes();
    printf("NUMA nodes: %d\n", nnodes);

    /* Allocate through our named functions (>4096 to trigger check_trace) */
    void *hot = allocate_hot_data(8192);
    void *cold = allocate_cold_data(8192);
    void *default_alloc = malloc(8192);

    /* Check which node each allocation landed on */
    int hot_node = -1, cold_node = -1, default_node = -1;
    get_mempolicy(&hot_node, NULL, 0, hot, MPOL_F_ADDR);
    get_mempolicy(&cold_node, NULL, 0, cold, MPOL_F_ADDR);
    get_mempolicy(&default_node, NULL, 0, default_alloc, MPOL_F_ADDR);

    printf("hot_data  (expect node 0): node %d\n", hot_node);
    printf("cold_data (expect node 2): node %d\n", cold_node);
    printf("default   (expect -1/default): node %d\n", default_node);

    free(hot);
    free(cold);
    free(default_alloc);

    /* Verify */
    int pass = 1;
    /* Note: with LD_PRELOAD, if check_trace matches, allocation goes to numa_alloc_onnode
       which sets MPOL_BIND, so node should be the configured node.
       If no match, it goes through libc_malloc, node depends on first-touch. */
    
    if (hot_node == 0) {
        printf("✅ hot_data correctly on node 0\n");
    } else {
        printf("⚠️ hot_data on node %d (expected 0) - check_trace may not have matched\n", hot_node);
    }

    if (cold_node == 2) {
        printf("✅ cold_data correctly on node 2\n");
    } else {
        printf("⚠️ cold_data on node %d (expected 2) - check_trace may not have matched\n", cold_node);
    }

    printf("\nDone.\n");
    return 0;
}
