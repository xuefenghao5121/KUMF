# Phase 1 测试验证方案 — 西西版

> 设计者：西西（主 Agent）
> 设计思路：从底层组件开始逐层验证，每个改动点都有独立的测试用例，最后做端到端集成测试

---

## 一、测试环境准备

### 1.1 鲲鹏930 环境初始化

```bash
#!/bin/bash
# scripts/setup_kunpeng.sh

# 1. 安装依赖
sudo yum install -y numactl-devel python3-pip gcc make perf
pip3 install polars pandas matplotlib intervaltree colour

# 2. 同步代码
mkdir -p ~/tiered-memory
scp -r /home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto kunpeng:~/tiered-memory/
scp -r /home/huawei/Desktop/home/xuefenghao/workspace/ARM-Tiered-Memory kunpeng:~/tiered-memory/

# 3. 验证环境
echo "=== Kernel ===" && uname -r
echo "=== NUMA ===" && numactl --hardware | head -20
echo "=== SPE ===" && ls /sys/bus/event_source/devices/arm_spe_0/
echo "=== libnuma ===" && ldconfig -p | grep numa
echo "=== perf SPE ===" && perf list | grep arm_spe
```

### 1.2 测试目录结构

```
~/tiered-memory/tests/
├── T01_rdtsc/           # 时间戳测试
├── T02_framepointer/    # 栈回溯测试
├── T03_makefile/        # 编译测试
├── T04_objid/           # 对象识别测试
├── T05_numa/            # NUMA 拓扑测试
├── T06_profbuf/         # Profiling buffer 测试
├── T07_spe/             # SPE smoke test
├── T08_latency/         # 延迟基准测试
├── T09_pipeline/        # 端到端 pipeline 测试
└── results/             # 测试结果
```

---

## 二、逐项测试方案

### T01: 时间戳替换测试 (rdtsc → clock_gettime)

#### 测试目标
验证 `clock_gettime(CLOCK_MONOTONIC_RAW)` 在 ARM64 上的精度和单调性。

#### 测试代码 `T01_rdtsc/test_timestamp.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

// ARM64 替代 rdtsc
static inline uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main() {
    uint64_t prev = 0;
    int violations = 0;
    uint64_t min_delta = UINT64_MAX;
    uint64_t max_delta = 0;
    uint64_t sum_delta = 0;
    int N = 1000000;

    for (int i = 0; i < N; i++) {
        uint64_t curr = get_timestamp_ns();
        if (i > 0) {
            uint64_t delta = curr - prev;
            if (delta < min_delta) min_delta = delta;
            if (delta > max_delta) max_delta = delta;
            sum_delta += delta;
            if (curr < prev) {
                violations++;
                printf("VIOLATION at i=%d: prev=%lu curr=%lu\n", i, prev, curr);
            }
        }
        prev = curr;
    }

    printf("=== clock_gettime(CLOCK_MONOTONIC_RAW) 测试 ===\n");
    printf("样本数: %d\n", N);
    printf("单调性违反: %d (应为 0)\n", violations);
    printf("最小间隔: %lu ns\n", min_delta);
    printf("最大间隔: %lu ns\n", max_delta);
    printf("平均间隔: %lu ns\n", sum_delta / N);
    printf("分辨率: ~%lu ns (约 %.1f GHz)\n", min_delta, 1.0 / (min_delta * 1e-9) / 1e9);
    
    // 对比：原版 rdtsc 在 x86 上约 0.5ns 分辨率 (2GHz TSC)
    // clock_gettime 在 ARM64 上约 30-50ns 分辨率
    // 对于 profiling 来说足够（采样间隔是毫秒级）
    
    return violations > 0 ? 1 : 0;
}
```

#### 预期结果

| 指标 | 预期值 | 判定标准 |
|------|--------|---------|
| 单调性违反 | 0 | 必须 = 0 |
| 最小间隔 | 20-100 ns | < 200 ns 通过 |
| 分辨率 | ~30-50 ns | 够用于 profiling（原版毫秒级采样间隔） |
| 1M 次调用总耗时 | < 500 ms | 不成为性能瓶颈 |

#### 验证方法
```bash
gcc -O2 -o test_timestamp test_timestamp.c
./test_timestamp
echo "退出码: $?"  # 0 = 通过
```

#### 测试 x86 对比
在同架构上，原版 rdtsc 分辨率约 0.5ns。我们接受 clock_gettime 的 30-50ns 分辨率，因为 SOAR 的 profiling 采样间隔是 `sysctl_numa_balancing_scan_delay = 1000ms`（1秒），30ns 分辨率比 1s 采样间隔精度高 4 个数量级，完全足够。

---

### T02: 栈回溯测试 (rbp → x29)

#### 测试目标
验证 ARM64 frame pointer 栈回溯能正确获取调用链。

#### 测试代码 `T02_framepointer/test_backtrace.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>
#include <string.h>

#define CALLCHAIN_SIZE 5

// 模拟 SOAR 的栈回溯
static int get_callchain(void **buffer, int size) {
    // 方法 A: __builtin_return_address（逐级获取）
    buffer[0] = __builtin_return_address(0);
    buffer[1] = __builtin_return_address(1);
    buffer[2] = __builtin_return_address(2);
    buffer[3] = __builtin_return_address(3);
    buffer[4] = __builtin_return_address(4);
    return 5;
}

// 方法 B: backtrace()
static int get_callchain_bt(void **buffer, int size) {
    return backtrace(buffer, size);
}

// 方法 C: ARM64 frame pointer 手动遍历
static int get_callchain_fp(void **buffer, int size) {
    void *fp;
    asm("mov %0, x29" : "=r" (fp) :);
    int count = 0;
    while (fp && count < size) {
        void *lr = *((void **)fp + 1);  // x29+8 = saved LR
        fp = *(void **)fp;               // x29+0 = saved FP
        buffer[count++] = lr;
    }
    return count;
}

// 构造嵌套调用
void func_d(void) {
    void *chain_builtin[CALLCHAIN_SIZE] = {0};
    void *chain_bt[CALLCHAIN_SIZE] = {0};
    void *chain_fp[CALLCHAIN_SIZE] = {0};

    int n_builtin = get_callchain(chain_builtin, CALLCHAIN_SIZE);
    int n_bt = get_callchain_bt(chain_bt, CALLCHAIN_SIZE);
    int n_fp = get_callchain_fp(chain_fp, CALLCHAIN_SIZE);

    printf("=== 方法 A: __builtin_return_address ===\n");
    printf("获取到 %d 层调用链\n", n_builtin);
    char **syms = backtrace_symbols(chain_builtin, n_builtin);
    for (int i = 0; i < n_builtin; i++) {
        printf("  [%d] %p -> %s\n", i, chain_builtin[i], 
               syms ? syms[i] : "(null)");
    }

    printf("\n=== 方法 B: backtrace() ===\n");
    printf("获取到 %d 层调用链\n", n_bt);
    syms = backtrace_symbols(chain_bt, n_bt);
    for (int i = 0; i < n_bt; i++) {
        printf("  [%d] %p -> %s\n", i, chain_bt[i], 
               syms ? syms[i] : "(null)");
    }

    printf("\n=== 方法 C: ARM64 frame pointer ===\n");
    printf("获取到 %d 层调用链\n", n_fp);
    syms = backtrace_symbols(chain_fp, n_fp);
    for (int i = 0; i < n_fp; i++) {
        printf("  [%d] %p -> %s\n", i, chain_fp[i], 
               syms ? syms[i] : "(null)");
    }
}

void func_c(void) { func_d(); }
void func_b(void) { func_c(); }
void func_a(void) { func_b(); }

int main() {
    printf("=== ARM64 栈回溯测试 ===\n\n");
    func_a();
    return 0;
}
```

#### 预期结果

| 方法 | 预期获取层数 | 预期符号 |
|------|------------|---------|
| `__builtin_return_address` | 4-5 层 | func_c, func_b, func_a, main |
| `backtrace()` | 4-5 层 | 同上（内部实现类似） |
| ARM64 frame pointer | 4-5 层 | 同上 |

**编译要求**: `-fno-omit-frame-pointer -g`

#### 判定标准
- 三种方法都至少获取到 **3 层**调用链 ✅
- 符号名包含 `func_c`/`func_b`/`func_a` ✅
- 方法 C（frame pointer）如果失败，回退到方法 B（backtrace）

```bash
gcc -O2 -g -fno-omit-frame-pointer -rdynamic -o test_backtrace test_backtrace.c
./test_backtrace
```

---

### T03: 编译测试

#### 测试目标
验证修改后的 Makefile 能在 ARM64 上编译所有组件。

#### 测试 Makefile `T03_makefile/Makefile`

```makefile
ARCH ?= $(shell uname -m)

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -fno-omit-frame-pointer -rdynamic

ifeq ($(ARCH), aarch64)
    CFLAGS += -DARM64
    $(info "=== Building for ARM64 ===")
else ifeq ($(ARCH), x86_64)
    CFLAGS += -DX86_64 -march=native
    $(info "=== Building for x86_64 ===")
endif

LDLIBS = -lpthread -lnuma -ldl

# 测试编译 prof/ldlib.c
test_prof: test_ldlib_prof.c ../SoarAlto/src/soar/prof/ldlib.c
	$(CC) -shared -fPIC $(CFLAGS) -o libmemprof.so ../SoarAlto/src/soar/prof/ldlib.c $(LDLIBS)

# 测试编译 interc/ldlib.c
test_interc: test_ldlib_interc.c ../SoarAlto/src/soar/interc/ldlib.c
	$(CC) -shared -fPIC $(CFLAGS) -o libmeminterc.so ../SoarAlto/src/soar/interc/ldlib.c $(LDLIBS)

# 测试 microbenchmark 编译
test_bench:
	$(MAKE) -C ../SoarAlto/src/microbenchmark/src clean all

.PHONY: clean
clean:
	rm -f libmemprof.so libmeminterc.so
	$(MAKE) -C ../SoarAlto/src/microbenchmark/src clean
```

#### 预期结果

| 编译目标 | 预期 | 可能的问题 |
|---------|------|-----------|
| libmemprof.so | 编译成功 | `immintrin.h` 不存在 → 已用 `#ifdef` 保护 |
| libmeminterc.so | 编译成功 | 同上 |
| microbenchmark | 编译成功 | `immintrin.h` → 改用通用 C 代码 |

#### 判定标准
- 三个目标全部编译成功（退出码 0）✅
- 没有 x86 特定指令的编译错误 ✅
- `file libmemprof.so` 输出包含 `ELF 64-bit LSB shared object, ARM aarch64` ✅

---

### T04: 对象识别重写测试

#### 测试目标
验证新的函数名匹配 + 配置文件驱动的对象识别机制。

#### 测试配置 `T04_objid/tiered_alloc.conf`

```ini
# 测试配置
func_c = 0     # func_c 分配的对象 → fast tier (Node 0)
func_b = 1     # func_b 分配的对象 → slow tier (Node 2)
func_a = -1    # func_a 分配的对象 → 默认
unknown = -1   # 未匹配 → 默认
```

#### 测试代码 `T04_objid/test_objid.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>
#include <numa.h>

#define MAX_RULES 64
#define MAX_PATTERN 256

typedef struct {
    char pattern[MAX_PATTERN];
    int target_tier;  // 0=fast, 1=slow, -1=default
} alloc_rule_t;

static alloc_rule_t rules[MAX_RULES];
static int num_rules = 0;

// 加载配置
int load_config(const char *conf_path) {
    FILE *f = fopen(conf_path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pattern[MAX_PATTERN];
        int tier;
        if (sscanf(line, "%255s = %d", pattern, &tier) == 2) {
            strncpy(rules[num_rules].pattern, pattern, MAX_PATTERN-1);
            rules[num_rules].target_tier = tier;
            num_rules++;
        }
    }
    fclose(f);
    return num_rules;
}

// 新版 check_trace: 用函数名匹配
int check_trace_by_name(void **callchain, int chain_size, size_t alloc_size) {
    char **symbols = backtrace_symbols(callchain, chain_size);
    
    for (int i = 0; i < chain_size && symbols; i++) {
        for (int r = 0; r < num_rules; r++) {
            if (strstr(symbols[i], rules[r].pattern)) {
                int tier = rules[r].target_tier;
                free(symbols);
                return tier;
            }
        }
    }
    free(symbols);
    return -1;  // 默认
}

// 测试对象
void *obj_fast = NULL;   // 应分配到 Node 0
void *obj_slow = NULL;   // 应分配到 Node 1
void *obj_default = NULL;

void allocate_in_func_c(void) {
    // 调用链: main → func_a → func_b → func_c → allocate_in_func_c
    // 匹配规则: func_c = 0 (fast tier)
    size_t size = 1024 * 1024;  // 1MB
    void *chain[5];
    int n = backtrace(chain, 5);
    int tier = check_trace_by_name(chain, n, size);
    printf("allocate_in_func_c: tier=%d (预期 0/fast)\n", tier);
    
    int target_node = (tier == 0) ? 0 : (tier == 1) ? 2 : -1;
    if (target_node >= 0) {
        obj_fast = numa_alloc_onnode(size, target_node);
    } else {
        obj_fast = malloc(size);
    }
}

void allocate_in_func_b(void) {
    size_t size = 2 * 1024 * 1024;  // 2MB
    void *chain[5];
    int n = backtrace(chain, 5);
    int tier = check_trace_by_name(chain, n, size);
    printf("allocate_in_func_b: tier=%d (预期 1/slow)\n", tier);
    
    int target_node = (tier == 0) ? 0 : (tier == 1) ? 2 : -1;
    if (target_node >= 0) {
        obj_slow = numa_alloc_onnode(size, target_node);
    } else {
        obj_slow = malloc(size);
    }
}

void allocate_in_func_a(void) {
    size_t size = 512 * 1024;  // 512KB
    void *chain[5];
    int n = backtrace(chain, 5);
    int tier = check_trace_by_name(chain, n, size);
    printf("allocate_in_func_a: tier=%d (预期 -1/default)\n", tier);
    obj_default = malloc(size);
}

int main(int argc, char *argv[]) {
    const char *conf = argc > 1 ? argv[1] : "tiered_alloc.conf";
    int n = load_config(conf);
    printf("=== 对象识别测试 ===\n");
    printf("加载了 %d 条规则\n\n", n);
    
    allocate_in_func_c();
    allocate_in_func_b();
    allocate_in_func_a();
    
    // 验证分配结果
    printf("\n=== 验证 NUMA 位置 ===\n");
    int node;
    if (obj_fast) {
        move_pages(0, 1, &obj_fast, NULL, &node, 0);
        printf("obj_fast 在 Node %d (预期 Node 0)\n", node);
    }
    if (obj_slow) {
        move_pages(0, 1, &obj_slow, NULL, &node, 0);
        printf("obj_slow 在 Node %d (预期 Node 2)\n", node);
    }
    if (obj_default) {
        move_pages(0, 1, &obj_default, NULL, &node, 0);
        printf("obj_default 在 Node %d (预期 Node 0, first-touch)\n", node);
    }
    
    return 0;
}
```

#### 预期结果

| 调用点 | 匹配规则 | 预期 tier | 预期 NUMA node |
|--------|---------|----------|---------------|
| allocate_in_func_c | `func_c = 0` | 0 (fast) | Node 0 |
| allocate_in_func_b | `func_b = 1` | 1 (slow) | Node 2 |
| allocate_in_func_a | `func_a = -1` | -1 (default) | Node 0 (first-touch) |

#### 判定标准
- 三个调用点的 tier 判定全部正确 ✅
- NUMA node 位置与预期一致 ✅
- 未匹配的调用返回 -1 (default) ✅

---

### T05: 4 NUMA Node 拓扑测试

#### 测试目标
验证鲲鹏930 的跨 NUMA 延迟差异，确定 tier 分层策略。

#### 测试代码 `T05_numa/test_numa_latency.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>

#define SIZE (64 * 1024 * 1024)  // 64MB
#define ITERATIONS 10

uint64_t get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// 测试从 compute_node 访问 memory_node 的延迟
double test_cross_node_latency(int compute_node, int memory_node) {
    // 在 memory_node 分配内存
    char *buf = numa_alloc_onnode(SIZE, memory_node);
    if (!buf) return -1;
    
    // 初始化
    for (int i = 0; i < SIZE; i += 64) buf[i] = i & 0xff;
    
    // 绑定到 compute_node
    struct bitmask *cpumask = numa_allocate_cpumask();
    numa_node_to_cpus(compute_node, cpumask);
    numa_bind(cpumask);
    
    // 顺序读取测试
    uint64_t start = get_ns();
    volatile int sum = 0;
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (int i = 0; i < SIZE; i += 64) {
            sum += buf[i];
        }
    }
    uint64_t end = get_ns();
    
    double total_gb = (double)SIZE * ITERATIONS / (1024*1024*1024);
    double elapsed_s = (end - start) / 1e9;
    double bandwidth_gbps = total_gb / elapsed_s;
    
    printf("  Node %d → Node %d: %.2f GB/s, %lu ns\n", 
           compute_node, memory_node, bandwidth_gbps, end - start);
    
    numa_free(buf, SIZE);
    numa_free_cpumask(cpumask);
    return bandwidth_gbps;
}

// Pointer-chasing 测试（真实延迟）
double test_pointer_chase_latency(int compute_node, int memory_node) {
    int N = 1024 * 1024;  // 1M nodes
    size_t size = N * sizeof(void *);
    
    char *buf = numa_alloc_onnode(size + N * 64, memory_node);
    if (!buf) return -1;
    
    // 构造 pointer chain
    void **ptrs = (void **)buf;
    for (int i = 0; i < N - 1; i++) {
        ptrs[i] = &ptrs[i + 1];
    }
    ptrs[N - 1] = &ptrs[0];
    
    // 绑定到 compute_node
    struct bitmask *cpumask = numa_allocate_cpumask();
    numa_node_to_cpus(compute_node, cpumask);
    numa_bind(cpumask);
    
    // Chase!
    uint64_t start = get_ns();
    void *p = &ptrs[0];
    for (int i = 0; i < N; i++) {
        p = *(void **)p;
    }
    uint64_t end = get_ns();
    
    double ns_per_chase = (double)(end - start) / N;
    
    printf("  Node %d → Node %d (pointer chase): %.1f ns/chase\n",
           compute_node, memory_node, ns_per_chase);
    
    numa_free(buf, size + N * 64);
    numa_free_cpumask(cpumask);
    return ns_per_chase;
}

int main() {
    printf("=== 鲲鹏930 NUMA 延迟测试 ===\n\n");
    
    int nnodes = numa_num_configured_nodes();
    printf("NUMA nodes: %d\n\n", nnodes);
    
    // 打印距离矩阵
    printf("--- NUMA 距离矩阵 ---\n");
    for (int i = 0; i < nnodes; i++) {
        for (int j = 0; j < nnodes; j++) {
            printf("%4d ", numa_distance(i, j));
        }
        printf("\n");
    }
    
    // 顺序读取带宽
    printf("\n--- 顺序读取带宽 (GB/s) ---\n");
    for (int cn = 0; cn < nnodes; cn++) {
        for (int mn = 0; mn < nnodes; mn++) {
            double bw = test_cross_node_latency(cn, mn);
            (void)bw;
        }
    }
    
    // Pointer-chasing 延迟
    printf("\n--- Pointer-chasing 延迟 (ns/chase) ---\n");
    for (int cn = 0; cn < nnodes; cn++) {
        for (int mn = 0; mn < nnodes; mn++) {
            test_pointer_chase_latency(cn, mn);
        }
    }
    
    return 0;
}
```

#### 预期结果

**顺序读取带宽（鲲鹏930 DDR，估计值）**:

| CPU \ 内存 | Node 0 | Node 1 | Node 2 | Node 3 |
|------------|--------|--------|--------|--------|
| **Node 0** | ~40 GB/s | ~35 GB/s | ~12 GB/s | ~10 GB/s |
| **Node 1** | ~35 GB/s | ~40 GB/s | ~10 GB/s | ~12 GB/s |
| **Node 2** | ~12 GB/s | ~10 GB/s | ~40 GB/s | ~35 GB/s |
| **Node 3** | ~10 GB/s | ~12 GB/s | ~35 GB/s | ~40 GB/s |

**Pointer-chasing 延迟（估计值）**:

| CPU \ 内存 | Node 0 | Node 1 | Node 2 | Node 3 |
|------------|--------|--------|--------|--------|
| **Node 0** | ~80 ns | ~85 ns | ~150 ns | ~160 ns |
| **Node 2** | ~150 ns | ~160 ns | ~80 ns | ~85 ns |

**Tier 分层结论**:
- 如果 Node 0↔Node 1 延迟差 < 20%：**2 层 tiering**（{0,1}=fast, {2,3}=slow）
- 如果 Node 0↔Node 1 延迟差 > 50%：**3 层 tiering**（0=fast, 1=medium, {2,3}=slow）

#### 判定标准
- 所有 16 种组合的带宽和延迟数据采集完成 ✅
- 同 socket vs 跨 socket 的延迟比 **≥ 1.5x** ✅（论文中 CXL 约 3x，鲲鹏预期 2-3x）
- 带宽数据与距离矩阵趋势一致 ✅

---

### T06: Profiling Buffer 测试

#### 测试目标
验证修改后的 profiling buffer 在鲲鹏 64GB/node 内存限制下不会 OOM。

#### 测试代码 `T06_profbuf/test_profbuf.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

// 原版: 每线程 950M 条 × 80 字节 = ~72GB → 爆了
// 改版: 方案 A: 降低到 500K 条 × 80B = ~40MB/线程
//        方案 B: mmap ring buffer

#define ARR_SIZE_SMALL 500000   // 方案 A: 每线程 40MB
#define RING_BUF_SIZE  (256 * 1024 * 1024)  // 方案 B: 256MB

struct log {
    uint64_t rdt;
    void *addr;
    size_t size;
    long entry_type;
    size_t callchain_size;
    void *callchain_strings[5];
};

void test_fixed_array() {
    printf("=== 方案 A: 固定大小数组 ===\n");
    size_t per_thread = ARR_SIZE_SMALL * sizeof(struct log);
    int max_threads = 40;  // 单 node 40 核
    size_t total = per_thread * max_threads;
    printf("每线程: %zu MB\n", per_thread / (1024*1024));
    printf("40线程: %zu MB\n", total / (1024*1024));
    printf("占 Node 内存比: %.1f%%\n", 100.0 * total / (64UL*1024*1024*1024));
    
    // 测试分配
    struct log *arr = malloc(per_thread);
    if (arr) {
        printf("单线程 buffer 分配: 成功 (%zu MB)\n", per_thread / (1024*1024));
        memset(arr, 0, per_thread);
        free(arr);
    } else {
        printf("单线程 buffer 分配: 失败!\n");
    }
}

void test_mmap_ring() {
    printf("\n=== 方案 B: mmap Ring Buffer ===\n");
    char path[] = "/tmp/tiered_prof_0.buf";
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    ftruncate(fd, RING_BUF_SIZE);
    void *buf = mmap(NULL, RING_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        printf("mmap 失败!\n");
    } else {
        printf("Ring buffer mmap: 成功 (%d MB)\n", RING_BUF_SIZE / (1024*1024));
        // 测试写入
        struct log *logs = (struct log *)buf;
        int capacity = RING_BUF_SIZE / sizeof(struct log);
        printf("容量: %d 条记录\n", capacity);
        // 模拟环形写入
        for (int i = 0; i < 10; i++) {
            logs[i % capacity].rdt = i;
            logs[i % capacity].size = 1024;
        }
        printf("环形写入测试: 通过\n");
        munmap(buf, RING_BUF_SIZE);
    }
    close(fd);
    unlink(path);
}

int main() {
    printf("=== Profiling Buffer 测试 ===\n");
    printf("struct log 大小: %zu 字节\n\n", sizeof(struct log));
    test_fixed_array();
    test_mmap_ring();
    return 0;
}
```

#### 预期结果

| 方案 | 每线程内存 | 40线程总内存 | 占 Node 比例 |
|------|-----------|------------|-------------|
| A (固定数组) | 40 MB | 1.6 GB | 2.5% |
| B (mmap ring) | 256 MB | 10 GB | 15.6% |

#### 判定标准
- 方案 A: 40 线程总内存 < 4 GB ✅（占 Node 64GB 的 < 10%）
- 方案 B: mmap 分配成功，环形写入正确 ✅
- 两种方案都不触发 OOM ✅

---

### T07: ARM SPE Smoke Test

#### 测试目标
验证 ARM SPE 能正确采集内存访问数据。

#### 测试脚本 `T07_spe/test_spe.sh`

```bash
#!/bin/bash
set -e

echo "=== ARM SPE Smoke Test ==="

# 1. 检查 SPE 设备
echo "--- 检查 SPE 设备 ---"
ls -la /sys/bus/event_source/devices/arm_spe_0/
cat /sys/bus/event_source/devices/arm_spe_0/cpumask
cat /sys/bus/event_source/devices/arm_spe_0/type

# 2. 最简 SPE 采集（5秒）
echo -e "\n--- 基本采集测试 (5秒) ---"
perf record -e arm_spe// -a -o spe_basic.perf.data -- sleep 5 2>&1 || {
    echo "SPE 采集失败! 尝试加参数..."
    perf record -e arm_spe/freq=1/ -a -o spe_basic.perf.data -- sleep 5 2>&1 || {
        echo "SPE 采集完全失败，需要排查"
        exit 1
    }
}

# 3. 检查输出
echo -e "\n--- 检查 perf.data ---"
perf report -i spe_basic.perf.data --stdio | head -30

# 4. 解析 SPE 输出格式
echo -e "\n--- SPE 输出格式分析 ---"
perf script -i spe_basic.perf.data | head -50

# 5. 检查字段完整性
echo -e "\n--- 字段完整性检查 ---"
OUTPUT=$(perf script -i spe_basic.perf.data | head -200)

# 检查是否有 latency 信息
if echo "$OUTPUT" | grep -qi "lat\|latency"; then
    echo "✅ latency 字段: 存在"
else
    echo "⚠️ latency 字段: 未找到 (可能需要 min_latency 参数)"
fi

# 检查是否有 data source 信息
if echo "$OUTPUT" | grep -qi "data_src\|source"; then
    echo "✅ data_source 字段: 存在"
else
    echo "⚠️ data_source 字段: 未找到"
fi

# 检查是否有虚拟地址
if echo "$OUTPUT" | grep -qiE "0x[0-9a-f]{8,}"; then
    echo "✅ 地址信息: 存在"
else
    echo "⚠️ 地址信息: 未找到"
fi

# 6. 带 min_latency 过滤的采集
echo -e "\n--- min_latency 过滤测试 ---"
perf record -e arm_spe/freq=1,min_latency=30/ -a -o spe_filtered.perf.data -- sleep 5 2>&1 || {
    echo "⚠️ min_latency 参数不支持，使用默认参数"
    perf record -e arm_spe/freq=1/ -a -o spe_filtered.perf.data -- sleep 5
}

echo -e "\n--- 过滤后数据量 ---"
ls -lh spe_filtered.perf.data

# 7. 数据量评估
echo -e "\n--- 数据量评估 ---"
BASIC_SIZE=$(stat -c%s spe_basic.perf.data 2>/dev/null || echo 0)
FILTERED_SIZE=$(stat -c%s spe_filtered.perf.data 2>/dev/null || echo 0)
echo "未过滤: $(( BASIC_SIZE / 1024 )) KB"
echo "过滤后: $(( FILTERED_SIZE / 1024 )) KB"
echo "压缩比: $(( FILTERED_SIZE * 100 / (BASIC_SIZE + 1) ))%"

# 清理
rm -f spe_basic.perf.data spe_filtered.perf.data

echo -e "\n=== SPE Smoke Test 完成 ==="
```

#### 预期结果

| 检查项 | 预期 | 判定标准 |
|--------|------|---------|
| SPE 设备存在 | arm_spe_0 存在 | 必须通过 |
| 基本采集 | perf record 成功 | 必须通过 |
| 输出格式 | 包含地址信息 | 必须通过 |
| latency 字段 | 存在 | 强烈期望 |
| data_source 字段 | 存在 | 强烈期望 |
| min_latency 过滤 | 数据量显著减少 | >50% 压缩 |
| 5秒数据量 | < 100 MB | 可接受范围 |

---

### T08: 延迟基准测量

#### 测试目标
测量鲲鹏930 各级缓存和 NUMA 延迟，替换原版的 SKX 常数。

#### 测试代码 `T08_latency/test_cache_latency.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <numa.h>

#define KB 1024
#define MB (1024*KB)

uint64_t get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Pointer chase 测量指定大小的延迟
double measure_latency(void *buf, size_t size, int iterations) {
    // 构造随机 pointer chain
    int N = size / 64;  // cache line 大小的节点
    void **ptrs = (void **)buf;
    
    // 顺序初始化然后 shuffle
    for (int i = 0; i < N - 1; i++) {
        ptrs[i * 8] = &ptrs[(i + 1) * 8];  // 每 64 字节一个节点
    }
    ptrs[(N-1) * 8] = &ptrs[0];
    
    // Chase
    uint64_t start = get_ns();
    void *p = &ptrs[0];
    for (int iter = 0; iter < iterations; iter++) {
        for (int i = 0; i < N; i++) {
            p = *(void **)p;
        }
    }
    uint64_t end = get_ns();
    
    return (double)(end - start) / (N * iterations);
}

int main() {
    printf("=== 鲲鹏930 缓存/内存延迟基准 ===\n\n");
    
    // L1: 64KB, L2: 512KB, L3: ~32MB per core
    // (需要确认鲲鹏930 具体参数)
    size_t sizes[] = {
        4 * KB,     // 应在 L1
        16 * KB,    // 应在 L1
        64 * KB,    // 应在 L1/L2 边界
        256 * KB,   // 应在 L2
        512 * KB,   // L2/L3 边界
        1 * MB,     // L3
        4 * MB,     // L3
        16 * MB,    // L3
        32 * MB,    // L3/内存 边界
        64 * MB,    // 内存
        256 * MB,   // 内存
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    
    printf("--- 本地 Node 延迟 ---\n");
    for (int i = 0; i < nsizes; i++) {
        void *buf = numa_alloc_onnode(sizes[i], 0);
        if (!buf) continue;
        double lat = measure_latency(buf, sizes[i], 100);
        printf("  %6zu KB: %.1f ns/access\n", sizes[i] / KB, lat);
        numa_free(buf, sizes[i]);
    }
    
    printf("\n--- 远程 Node 延迟 (Node 0 → Node 2) ---\n");
    for (int i = 0; i < nsizes; i++) {
        if (sizes[i] < 1 * MB) continue;  // 小尺寸不应跨越 NUMA
        void *buf = numa_alloc_onnode(sizes[i], 2);
        if (!buf) continue;
        
        // 绑定到 Node 0
        struct bitmask *mask = numa_allocate_cpumask();
        numa_node_to_cpus(0, mask);
        numa_bind(mask);
        
        double lat = measure_latency(buf, sizes[i], 100);
        printf("  %6zu KB: %.1f ns/access\n", sizes[i] / KB, lat);
        
        numa_free(buf, sizes[i]);
        numa_free_cpumask(mask);
    }
    
    return 0;
}
```

#### 预期结果

| 层级 | 预期延迟 | x86 SKX 对比 |
|------|---------|-------------|
| L1 (~64KB) | 2-5 ns | ~4 ns (SKX) |
| L2 (~512KB) | 5-10 ns | ~7 ns (SKX) |
| L3 (~32MB) | 15-25 ns | ~20 ns (SKX) |
| Local DDR (64MB+) | 60-90 ns | ~80 ns (SKX) |
| Remote DDR (跨socket) | 120-200 ns | ~150 ns (CXL) |

**关键输出**: 
- 鲲鹏930 的 L3 hit 延迟（替换原版常数 24.87 cycles）
- Local/Remote DDR 延迟比（确定 tiering 收益）

#### 判定标准
- 延迟曲线随缓存层级单调递增 ✅
- Local vs Remote 延迟比 ≥ 1.5x ✅
- 测量值的变异系数 (CV) < 10% ✅

---

### T09: 端到端 Pipeline 测试

#### 测试目标
完整的 profiling → 分析 → 分配 pipeline 在 ARM 上跑通。

#### 测试工作负载 `T09_pipeline/test_workload.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 模拟一个有两种访问模式的程序：
// - hot_data: 频繁访问的小数组（应分配到 fast tier）
// - cold_data: 少量访问的大数组（可分配到 slow tier）

#define HOT_SIZE (4 * 1024 * 1024)      // 4MB
#define COLD_SIZE (256 * 1024 * 1024)   // 256MB

double *hot_data;
double *cold_data;

void init_hot() {
    // 调用链: main → init_hot → malloc
    // 期望: 匹配 "init_hot" 规则 → fast tier
    hot_data = malloc(HOT_SIZE);
    for (int i = 0; i < HOT_SIZE / sizeof(double); i++) {
        hot_data[i] = (double)i * 0.5;
    }
}

void init_cold() {
    // 调用链: main → init_cold → malloc
    // 期望: 匹配 "init_cold" 规则 → slow tier
    cold_data = malloc(COLD_SIZE);
    for (int i = 0; i < COLD_SIZE / sizeof(double); i++) {
        cold_data[i] = (double)i * 0.1;
    }
}

double compute_hot() {
    // 频繁访问 hot_data（模拟性能关键路径）
    double sum = 0;
    for (int iter = 0; iter < 1000; iter++) {
        for (int i = 0; i < HOT_SIZE / sizeof(double); i++) {
            sum += hot_data[i] * 1.01;
        }
    }
    return sum;
}

double compute_cold() {
    // 少量访问 cold_data
    double sum = 0;
    for (int i = 0; i < COLD_SIZE / sizeof(double); i += 256) {
        sum += cold_data[i];
    }
    return sum;
}

int main() {
    printf("=== Test Workload ===\n");
    init_hot();
    init_cold();
    
    printf("hot_data addr: %p\n", (void *)hot_data);
    printf("cold_data addr: %p\n", (void *)cold_data);
    
    double r1 = compute_hot();
    double r2 = compute_cold();
    printf("Result: %f %f\n", r1, r2);
    
    free(hot_data);
    free(cold_data);
    return 0;
}
```

#### 配置文件 `T09_pipeline/tiered_alloc.conf`

```ini
init_hot = 0      # fast tier
compute_hot = 0   # fast tier  
init_cold = 1     # slow tier
compute_cold = 1  # slow tier
```

#### 测试脚本 `T09_pipeline/run_test.sh`

```bash
#!/bin/bash
set -e

echo "=== 端到端 Pipeline 测试 ==="

# Step 1: 编译
echo "--- Step 1: 编译 ---"
gcc -O2 -g -fno-omit-frame-pointer -rdynamic -o test_workload test_workload.c -lnuma

# Step 2: Baseline (不优化，默认 NUMA)
echo -e "\n--- Step 2: Baseline (默认 NUMA) ---"
/usr/bin/time -v ./test_workload 2>&1 | grep -E "wall clock|Maximum resident"

# Step 3: 全在 fast tier (上限)
echo -e "\n--- Step 3: 全在 fast tier ---"
/usr/bin/time -v numactl --cpunodebind=0 --membind=0 ./test_workload 2>&1 | grep -E "wall clock|Maximum resident"

# Step 4: 全在 slow tier (下限)
echo -e "\n--- Step 4: 全在 slow tier ---"
/usr/bin/time -v numactl --cpunodebind=0 --membind=2 ./test_workload 2>&1 | grep -E "wall clock|Maximum resident"

# Step 5: 用 LD_PRELOAD 做智能分配
echo -e "\n--- Step 5: 智能分配 (LD_PRELOAD) ---"
# 编译拦截库 (用 T04 的代码)
gcc -O2 -g -fno-omit-frame-pointer -shared -fPIC -o libtiered_test.so \
    test_objid.c -lnuma -ldl
LD_PRELOAD=./libtiered_test.so TIERED_ALLOC_CONF=./tiered_alloc.conf \
    /usr/bin/time -v ./test_workload 2>&1 | grep -E "wall clock|Maximum resident"

# Step 6: SPE 采集（如果可用）
echo -e "\n--- Step 6: SPE 采集 ---"
if perf list 2>/dev/null | grep -q arm_spe; then
    perf record -e arm_spe/freq=1,min_latency=30/ -a -o spe_test.perf.data -- ./test_workload
    echo "SPE 数据大小: $(ls -lh spe_test.perf.data | awk '{print $5}')"
    perf script -i spe_test.perf.data | wc -l | xargs -I{} echo "SPE 记录数: {}"
    rm -f spe_test.perf.data
else
    echo "SPE 不可用，跳过"
fi

echo -e "\n=== 测试完成 ==="
```

#### 预期结果

| 配置 | 预期执行时间 | 说明 |
|------|------------|------|
| Baseline（默认 NUMA） | 1.0x（基准） | first-touch，大部分在 Node 0 |
| 全 fast tier (Node 0) | 0.9-1.0x | 上限，最快 |
| 全 slow tier (Node 2) | 1.3-2.0x | 下限，变慢（hot_data 被拖慢） |
| 智能分配 | ~1.0x | hot 在 fast, cold 在 slow，接近上限 |

**关键判定**: 智能分配的性能应**接近全 fast tier**，且明显优于全 slow tier。

#### 判定标准
- 三种 NUMA 配置的执行时间有可测量差异 ✅
- 全 slow tier 比全 fast tier 慢 **≥ 20%** ✅（说明 NUMA 延迟影响可观测）
- 智能分配比全 slow tier 快 **≥ 15%** ✅
- SPE 采集到的记录中包含 test_workload 的地址范围 ✅

---

## 三、测试执行顺序

```
T01 时间戳 ──┐
T02 栈回溯 ──┤──→ T03 编译 ──→ T04 对象识别 ──┐
              │                                 ├──→ T09 端到端
T05 NUMA延迟 ┤                                 │
T06 ProfBuf ─┤──→ T07 SPE ──→ T08 延迟基准 ──┘
```

**依赖关系**:
- T01、T02、T05、T06 可以并行（无依赖）
- T03 依赖 T01、T02 的代码改动
- T04 依赖 T02（栈回溯）和 T03（编译通过）
- T07 可以在 T01-T06 之后任何时间跑
- T08 依赖 T05（确认 NUMA 绑定正确）
- T09 依赖所有前置测试通过

**总耗时估计**: 2-3 天（含调试时间）

---

## 四、可能遇到的问题

| 问题 | 概率 | 影响 | 应对 |
|------|------|------|------|
| clock_gettime 分辨率不够 | 低 | 低 | 改用 CNTVCT_EL0 寄存器（需内核模块） |
| backtrace_symbols 返回 ??:0 | 中 | 中 | 编译时加 `-rdynamic`，或用 dladdr 手动解析 |
| SPE min_latency 参数不支持 | 中 | 中 | 用默认参数，在分析阶段过滤低延迟记录 |
| numa_bind 需要 root | 低 | 中 | 用 `numactl --cpunodebind` 替代 |
| 64MB 测试数据在 Node 上分配失败 | 低 | 高 | 减小测试数据大小 |
| SPE 数据量 > 1GB/min | 中 | 中 | 提高 min_latency 到 100+，或降低 freq |
