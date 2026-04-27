# Phase 1 测试验证方案：SOAR ARM 适配 + 对象识别重写

> 目标平台: 鲲鹏930, 120核 ARM64, 4 NUMA node, openEuler 24.03 (Linux 6.6.0)
> 原版代码: /home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto/
> 方案版本: v1.0 | 2026-04-24

---

## Week 1: rdtsc→clock_gettime, rbp→x29, Makefile 条件编译

### 改动点摘要

| 文件 | x86 原始代码 | ARM 替换 |
|------|-------------|---------|
| `src/soar/prof/ldlib.c` L75-82 | `rdtscll(val)` 使用 `asm volatile("rdtsc")` | `clock_gettime(CLOCK_MONOTONIC_RAW, &ts)` |
| `src/soar/prof/ldlib.c` L87 | `asm("movq %%rbp, %0" : "=r" (bp) :)` | `asm("mov x29, %0" : "=r" (bp) :)` |
| `src/soar/interc/ldlib.c` L79-86 | 同上 rdtsc | 同上 clock_gettime |
| `src/soar/interc/ldlib.c` L91 | 同上 rbp | 同上 x29 |
| `src/soar/prof/Makefile` | 无架构判断 | 增加 `ARCH := $(shell uname -m)` 条件编译 |
| `src/soar/interc/Makefile` | 无架构判断 | 同上 |

### 测试用例

#### T1.1: rdtsc→clock_gettime 编译验证

**目的**: 确认 clock_gettime 替换后，两个 ldlib.c 均可在 ARM64 上编译通过。

**前置条件**:
- SSH 到鲲鹏930 (ssh kunpeng)
- GCC 12.3.1 已安装
- libnuma-dev 已安装

**步骤**:
```bash
cd /path/to/soar-arm/src/soar/prof/
make clean && make 2>&1 | tee /tmp/week1_prof_build.log
ls -la ldlib.so

cd /path/to/soar-arm/src/soar/interc/
make clean && make 2>&1 | tee /tmp/week1_interc_build.log
ls -la ldlib.so
```

**预期结果**:
- 编译零 error，零 warning（-Wall 级别）
- `ldlib.so` 文件生成，大小在 50KB-200KB 范围
- `file ldlib.so` 输出包含 `ELF 64-bit LSB shared object, ARM aarch64`

**通过条件**: 两个目录均编译成功，产物为 aarch64 ELF .so 文件

---

#### T1.2: clock_gettime 时间戳单调性验证

**目的**: 确认替换后时间戳单调递增，与原 rdtsc 行为一致。

**测试脚本**: `test_tsc_monotonic.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

#define ITERS 10000000

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main() {
    uint64_t prev = get_time_ns();
    uint64_t violations = 0;
    uint64_t min_delta = UINT64_MAX;
    uint64_t max_delta = 0;
    uint64_t total_delta = 0;

    for (int i = 0; i < ITERS; i++) {
        uint64_t curr = get_time_ns();
        if (curr < prev) {
            violations++;
        }
        uint64_t delta = curr - prev;
        if (delta < min_delta) min_delta = delta;
        if (delta > max_delta) max_delta = delta;
        total_delta += delta;
        prev = curr;
    }

    printf("Iterations: %d\n", ITERS);
    printf("Monotonicity violations: %lu\n", violations);
    printf("Min delta: %lu ns\n", min_delta);
    printf("Max delta: %lu ns\n", max_delta);
    printf("Avg delta: %lu ns\n", total_delta / ITERS);
    printf("Avg freq estimate: %.2f GHz\n",
           1.0 / ((double)(total_delta / ITERS) / 1e9));
    return violations > 0 ? 1 : 0;
}
```

**编译运行**:
```bash
gcc -O2 -o test_tsc_monotonic test_tsc_monotonic.c
numactl --cpunodebind=0 --membind=0 ./test_tsc_monotonic
```

**预期结果**:
- Monotonicity violations: **0**
- Min delta: **18-25 ns**（鲲鹏930 counter 频率约 100MHz，即 ~10ns 粒度，加上系统调用开销 ~15ns）
- Max delta: **< 10000 ns**（偶尔调度抖动）
- Avg delta: **30-50 ns**（clock_gettime 系统调用 vDSO 开销）
- Avg freq estimate: 不适用（不再是 TSC 频率，而是 ns 粒度）

**通过条件**: violations == 0

---

#### T1.3: clock_gettime 开销基准

**目的**: 量化 clock_gettime 相对 rdtsc 的额外开销，确认不影响 profiling 精度。

**测试脚本**: `test_clock_overhead.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define ITERS 100000000

static inline uint64_t clock_gettime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main() {
    uint64_t start, end;
    volatile uint64_t sink;

    start = clock_gettime_ns();
    for (int i = 0; i < ITERS; i++) {
        sink = clock_gettime_ns();
    }
    end = clock_gettime_ns();

    double ns_per_call = (double)(end - start) / ITERS;
    printf("clock_gettime overhead: %.1f ns/call\n", ns_per_call);
    printf("Total time: %.3f s\n", (double)(end - start) / 1e9);
    return 0;
}
```

**预期结果**:
- clock_gettime(CLOCK_MONOTONIC_RAW) via vDSO: **18-30 ns/call**
- 对比原版 x86 rdtsc: 约 20-30 ns/call（rdtsc 本身 ~20 cycles @ 2.1GHz ≈ 9.5ns，但有 serializing 开销）
- **结论**: clock_gettime 开销与 rdtsc 实际开销在同一个数量级，可接受

**通过条件**: clock_gettime 开销 < 50 ns/call

---

#### T1.4: rbp→x29 帧指针验证

**目的**: 确认 ARM64 帧指针读取正确，backtrace 可以正常工作。

**测试脚本**: `test_frame_pointer.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <execinfo.h>
#include <stdint.h>

#define CALLCHAIN_SIZE 5

static inline void get_bp(void **bp) {
    asm("mov x29, %0" : "=r" (*bp));
}

/* 故意构造多层调用，验证帧指针链 */
void __attribute__((noinline)) func_c(void) {
    void *chain[CALLCHAIN_SIZE];
    int size = backtrace(chain, CALLCHAIN_SIZE);

    printf("backtrace size: %d\n", size);
    char **syms = backtrace_symbols(chain, size);
    for (int i = 0; i < size; i++) {
        printf("  [%d] %s\n", i, syms[i]);
    }
    free(syms);

    /* 验证帧指针读取 */
    void *fp;
    get_bp(&fp);
    printf("Frame pointer (x29): %p\n", fp);
}

void __attribute__((noinline)) func_b(void) {
    func_c();
}

void __attribute__((noinline)) func_a(void) {
    func_b();
}

int main() {
    func_a();
    return 0;
}
```

**编译运行**:
```bash
# 必须加 -fno-omit-frame-pointer 确保帧指针保留
gcc -g -O0 -fno-omit-frame-pointer -rdynamic -o test_frame_pointer test_frame_pointer.c
numactl --cpunodebind=0 ./test_frame_pointer
```

**预期结果**:
- backtrace size >= 3（至少包含 func_c, func_b, func_a, main）
- 符号名可解析（显示函数名而非 ???）
- Frame pointer (x29): 非 NULL 且对齐到 16 字节边界

**通过条件**: backtrace 返回 >= 3 层，包含 func_a/func_b/func_c 函数名

---

#### T1.5: Makefile 条件编译验证

**目的**: 确认同一套 Makefile 在 x86 和 ARM 上都能正确编译。

**修改后的 Makefile 模板**:
```makefile
ARCH := $(shell uname -m)

CFLAGS=-Wall -g -ggdb3 -O0

ifeq ($(ARCH),aarch64)
CFLAGS += -march=armv8.2-a+sve
LDFLAGS=-lnuma
else
CFLAGS += -march=native
LDFLAGS=-lnuma
endif

.PHONY: all clean
all: makefile.dep ldlib.so

makefile.dep: *.[Cch]
	for i in *.[Cc]; do $$(CXX) -MM "$${i}" ${CFLAGS}; done > $@

-include makefile.dep

ifeq ($(ARCH),aarch64)
CXX = g++
else
CXX = g++
endif

ldlib.so: ldlib.c
	$(CXX) -fPIC ${CFLAGS} -c ldlib.c
	$(CXX) -shared -Wl,-soname,libpmalloc.so -o ldlib.so ldlib.o -ldl -lpthread -lnuma

clean:
	rm -f *.o *.so makefile.dep
```

**测试步骤**:
```bash
# 在鲲鹏930上
cd src/soar/prof/ && make clean && make
echo "ARCH=$(uname -m)"  # 应输出 aarch64
file ldlib.so  # 应含 ARM aarch64

# 在 x86 开发机上（交叉验证）
cd src/soar/prof/ && make clean && make
echo "ARCH=$(uname -m)"  # 应输出 x86_64
file ldlib.so  # 应含 x86-64
```

**预期结果**:
- ARM 上: ARCH=aarch64, CFLAGS 含 -march=armv8.2-a+sve
- x86 上: ARCH=x86_64, CFLAGS 含 -march=native
- 两平台均可无 error 编译

**通过条件**: 两平台 make 均成功，产物架构匹配

---

#### T1.6: ldlib.so LD_PRELOAD 基本功能验证

**目的**: 确认编译出的 ldlib.so 可以通过 LD_PRELOAD 拦截 malloc/free。

**测试脚本**: `test_preload.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    /* 触发 calloc（printf 内部会调用） */
    printf("Test start\n");

    /* 小分配：应走 libc_malloc */
    void *p1 = malloc(128);
    printf("malloc(128) = %p\n", p1);

    /* 大分配：>4096，interc 版本会走 numa_alloc_onnode */
    void *p2 = malloc(8192);
    printf("malloc(8192) = %p\n", p2);

    free(p1);
    free(p2);
    printf("Test done\n");
    return 0;
}
```

**运行**:
```bash
# prof 版本
gcc -O0 -g -o test_preload test_preload.c
LD_PRELOAD=../prof/ldlib.so ./test_preload 2>&1 | tee /tmp/week1_preload_prof.log

# interc 版本
LD_PRELOAD=../interc/ldlib.so ./test_preload 2>&1 | tee /tmp/week1_preload_interc.log
```

**预期结果**:
- 无段错误，无 dlsym 循环
- prof 版本：程序正常退出，destructor 生成 `/mnt/sda4/data.raw.*` 文件（需确认路径存在或修改输出路径）
- interc 版本：程序正常退出，大分配可能走 numa_alloc_onnode

**通过条件**: 程序正常退出，exit code 0

---

### Week 1 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| clock_gettime vDSO 在 openEuler 上不可用，退化为系统调用 | 低 | 开销从 ~20ns 涨到 ~500ns | 使用 `CLOCK_MONOTONIC` 替代 `CLOCK_MONOTONIC_RAW`；如仍为 syscall，使用 `rdffr`/`cntvct_el0` 直接读 ARM generic timer |
| GCC -fno-omit-frame-pointer 在 ARM64 -O2 下被忽略 | 中 | backtrace 返回空 | 编译 ldlib.so 时强制 `-O0 -fno-omit-frame-pointer`；或改用 `backtrace()` (libgcc unwinder) 替代手动帧指针遍历 |
| ldlib.so 中 m_init 的 dlsym 循环（calloc 在 dlsym 前被调用） | 中 | 死循环/段错误 | 已有空 empty_data 缓冲区处理，但需验证 ARM 上 libc calloc 行为一致；如不一致，增加 `in_first_dlsym` 保护 |
| 原版 `syscall(186)` 获取 tid 在 ARM64 上号不同 | 高 | tid 获取失败 | ARM64 上 `gettid` 系统调用号为 178，不是 186。需改为 `syscall(__NR_gettid)` 或 `(int)gettid()` |

---

## Week 2: check_trace() 重写 + 4 NUMA node 拓扑支持

### 改动点摘要

| 文件 | 原始代码 | 目标代码 |
|------|---------|---------|
| `src/soar/interc/ldlib.c` L159-178 | `check_trace()` 硬编码 hex 地址 `{"405fb2","406d68",...}` | 函数名匹配 + 配置文件 `soar_obj.conf` |
| `src/soar/interc/ldlib.c` L200 | `numa_alloc_onnode(sz, ret)` 仅支持 0/1 | 扩展为 0-3，4 NUMA node |
| `src/soar/interc/ldlib.c` | `MAX_OBJECTS=30000`，`addr_segs[]` 线性搜索 | 支持 4 node 的分段管理 |
| 新增 | 无 | `soar_obj.conf` 配置文件解析器 |

### 配置文件格式设计

```ini
# soar_obj.conf - SOAR 对象放置配置
# 格式: 函数名 = numa_node
# numa_node: 0-3 对应 NUMA node, -1 表示默认分配

# 热对象 -> 本地 DRAM (node 0 或当前 node)
GAPBS_BC_PullEdge = 0
GAPBS_BC_BFS = 0

# 温对象 -> 同 socket 远端 (node 1)
GAPBS_BC_ProcessParent = 1

# 冷对象 -> 远 socket (node 2 或 node 3)
GAPBS_BC_MakeCSR = 2

# 默认: 不匹配的对象走 -1 (默认分配)
```

### 重写后的 check_trace 签名

```c
// 从配置文件加载的对象表
struct obj_placement {
    char func_name[256];
    int numa_node;  // -1 = default, 0-3 = specific node
};

static struct obj_placement obj_table[MAX_OBJECTS];
static int obj_table_size = 0;

// 初始化: 从 soar_obj.conf 加载
int load_obj_config(const char *conf_path);

// 新 check_trace: 函数名匹配
int check_trace(const char *func_name, size_t sz);
// 返回值: -1 = default, 0-3 = numa node
```

### 测试用例

#### T2.1: 配置文件解析验证

**目的**: 确认 `load_obj_config()` 正确解析 soar_obj.conf。

**测试配置文件**: `test_soar_obj.conf`
```ini
func_alpha = 0
func_beta = 1
func_gamma = 2
func_delta = 3
# 注释行
func_epsilon = -1
   func_zeta = 0   
```

**测试脚本**: `test_config_parse.c`
```c
#include <stdio.h>
#include <string.h>
#include <assert.h>

// 复用 ldlib.c 中的 load_obj_config 和 obj_table
extern int load_obj_config(const char *path);
extern struct obj_placement obj_table[];
extern int obj_table_size;

int main() {
    int ret = load_obj_config("test_soar_obj.conf");
    assert(ret == 0);
    assert(obj_table_size == 5);

    // 验证解析正确性
    assert(strcmp(obj_table[0].func_name, "func_alpha") == 0);
    assert(obj_table[0].numa_node == 0);

    assert(strcmp(obj_table[1].func_name, "func_beta") == 0);
    assert(obj_table[1].numa_node == 1);

    assert(strcmp(obj_table[2].func_name, "func_gamma") == 0);
    assert(obj_table[2].numa_node == 2);

    assert(strcmp(obj_table[3].func_name, "func_delta") == 0);
    assert(obj_table[3].numa_node == 3);

    assert(strcmp(obj_table[4].func_name, "func_epsilon") == 0);
    assert(obj_table[4].numa_node == -1);

    printf("Config parse: PASS (%d entries)\n", obj_table_size);
    return 0;
}
```

**预期结果**:
- obj_table_size == 5（跳过注释行和空行）
- 每个 func_name 和 numa_node 正确解析
- 前后空格被 trim

**通过条件**: 所有 assert 通过

---

#### T2.2: check_trace 函数名匹配验证

**目的**: 确认重写后的 check_trace 可以匹配 backtrace_symbols 返回的函数名。

**测试脚本**: `test_check_trace.c`
```c
#include <stdio.h>
#include <string.h>
#include <assert.h>

extern struct obj_placement obj_table[];
extern int obj_table_size;
extern int check_trace(const char *func_name, size_t sz);

int main() {
    // 加载测试配置
    load_obj_config("test_soar_obj.conf");

    // 精确匹配
    assert(check_trace("func_alpha", 8192) == 0);
    assert(check_trace("func_beta", 8192) == 1);
    assert(check_trace("func_gamma", 8192) == 2);
    assert(check_trace("func_delta", 8192) == 3);

    // 不匹配 -> 返回 -1
    assert(check_trace("func_unknown", 8192) == -1);

    // 部分匹配（backtrace_symbols 格式可能是 "func_alpha+0x1a"）
    // 根据实现决定是否支持部分匹配
    assert(check_trace("func_alpha+0x1a", 8192) == 0);

    printf("check_trace: PASS\n");
    return 0;
}
```

**预期结果**:
- 精确匹配全部正确
- 部分匹配（含 +offset 后缀）全部正确（如果实现支持）
- 不匹配返回 -1

**通过条件**: 所有 assert 通过

---

#### T2.3: backtrace_symbols 格式验证

**目的**: 确认 ARM64 上 backtrace_symbols 输出格式与 x86 类似，函数名可解析。

**测试脚本**: `test_backtrace_format.c`
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>

void __attribute__((noinline)) inner_func(void) {
    void *buf[10];
    int size = backtrace(buf, 10);
    char **syms = backtrace_symbols(buf, size);

    printf("backtrace size: %d\n", size);
    for (int i = 0; i < size; i++) {
        printf("  [%d] raw: %s\n", i, syms[i]);
    }
    free(syms);
}

void __attribute__((noinline)) outer_func(void) {
    inner_func();
}

int main() {
    outer_func();
    return 0;
}
```

**编译**:
```bash
gcc -g -O0 -fno-omit-frame-pointer -rdynamic -o test_backtrace_format test_backtrace_format.c
```

**预期结果**:
- ARM64 backtrace_symbols 输出格式: `./test_backtrace_format(inner_func+0x...) [0x...]`
- 函数名可提取（包含 `inner_func`, `outer_func`, `main`）
- 如果显示 `??`，说明符号表缺失，需加 `-rdynamic` 或检查 strip

**通过条件**: 至少 3 层可解析函数名

---

#### T2.4: 4 NUMA node 分配验证

**目的**: 确认 malloc 拦截可以正确地将大对象分配到 4 个不同 NUMA node。

**测试脚本**: `test_numa_4node.c`
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <numa.h>
#include <numaif.h>
#include <stdint.h>

int main() {
    if (numa_available() < 0) {
        fprintf(stderr, "NUMA not available\n");
        return 1;
    }

    printf("NUMA nodes: %d\n", numa_num_configured_nodes());
    printf("CPUs: %d\n", numa_num_configured_cpus());

    // 打印 NUMA 距离矩阵
    int max_node = numa_max_node();
    printf("NUMA distance matrix:\n");
    printf("     ");
    for (int i = 0; i <= max_node; i++)
        printf("  N%d", i);
    printf("\n");
    for (int i = 0; i <= max_node; i++) {
        printf("  N%d ", i);
        for (int j = 0; j <= max_node; j++) {
            printf("  %2d", numa_distance(i, j));
        }
        printf("\n");
    }

    // 在每个 node 上分配并验证
    for (int node = 0; node <= max_node; node++) {
        size_t sz = 64 * 1024;  // 64KB
        void *ptr = numa_alloc_onnode(sz, node);
        if (!ptr) {
            fprintf(stderr, "numa_alloc_onnode(%zu, %d) failed\n", sz, node);
            continue;
        }

        // 写入数据触发实际分配
        memset(ptr, 0xAA, sz);

        // 查询分配所在 node
        int status;
        if (get_mempolicy(&status, NULL, 0, ptr, MPOL_F_ADDR) == 0) {
            printf("Allocated on node %d, actual node: %d %s\n",
                   node, status, status == node ? "✓" : "✗ MISMATCH");
        } else {
            perror("get_mempolicy");
        }

        numa_free(ptr, sz);
    }

    return 0;
}
```

**预期结果**:
- NUMA nodes: **4**
- NUMA 距离矩阵：
  ```
       N0  N1  N2  N3
  N0   10  12  35  35    (同 socket: 10/12, 跨 socket: 35-40)
  N1   12  10  35  35
  N2   35  35  10  12
  N3   35  35  12  10
  ```
  注：鲲鹏930 的具体距离值可能为 10/12/35/40 的变体
- 每个 node 分配成功，actual node 匹配

**通过条件**: 4 node 分配全部成功，actual == requested，距离矩阵符合预期拓扑

---

#### T2.5: interc/ldlib.so 完整拦截验证

**目的**: 端到端验证：LD_PRELOAD 加载 interc/ldlib.so + soar_obj.conf 后，大分配根据函数名路由到指定 NUMA node。

**测试脚本**: `test_interc_routing.c`
```c
#include <stdio.h>
#include <stdlib.h>
#include <numa.h>
#include <numaif.h>

/* 故意用与 soar_obj.conf 匹配的函数名 */
void __attribute__((noinline)) GAPBS_BC_PullEdge(void) {
    void *p = malloc(8192);
    if (p) {
        int status;
        get_mempolicy(&status, NULL, 0, p, MPOL_F_ADDR);
        printf("GAPBS_BC_PullEdge: alloc on node %d (expect 0)\n", status);
        free(p);
    }
}

void __attribute__((noinline)) GAPBS_BC_MakeCSR(void) {
    void *p = malloc(8192);
    if (p) {
        int status;
        get_mempolicy(&status, NULL, 0, p, MPOL_F_ADDR);
        printf("GAPBS_BC_MakeCSR: alloc on node %d (expect 2)\n", status);
        free(p);
    }
}

void __attribute__((noinline)) unknown_func(void) {
    void *p = malloc(8192);
    if (p) {
        int status;
        get_mempolicy(&status, NULL, 0, p, MPOL_F_ADDR);
        printf("unknown_func: alloc on node %d (expect default)\n", status);
        free(p);
    }
}

int main() {
    GAPBS_BC_PullEdge();
    GAPBS_BC_MakeCSR();
    unknown_func();
    return 0;
}
```

**运行**:
```bash
gcc -g -O0 -fno-omit-frame-pointer -rdynamic -o test_interc_routing test_interc_routing.c
SOAR_OBJ_CONF=./soar_obj.conf LD_PRELOAD=./interc/ldlib.so ./test_interc_routing
```

**预期结果**:
- `GAPBS_BC_PullEdge`: alloc on node **0**
- `GAPBS_BC_MakeCSR`: alloc on node **2**
- `unknown_func`: alloc on node **默认** (通常是当前进程所在 node)

**通过条件**: 匹配的函数名分配到正确 node，不匹配的走默认

---

#### T2.6: addr_seg 管理 4 node 验证

**目的**: 确认 `record_seg()` / `check_seg()` 在 4 node 场景下正确追踪 NUMA 分配和释放。

**测试脚本**: `test_addr_seg.c`
```c
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <numa.h>

// 复用 ldlib.c 中的 record_seg/check_seg
extern void record_seg(unsigned long addr, size_t size);
extern size_t check_seg(unsigned long addr);

int main() {
    // 模拟多 node 分配
    void *p0 = numa_alloc_onnode(8192, 0);
    void *p1 = numa_alloc_onnode(16384, 1);
    void *p2 = numa_alloc_onnode(32768, 2);
    void *p3 = numa_alloc_onnode(65536, 3);

    // 记录
    record_seg((unsigned long)p0, 8192);
    record_seg((unsigned long)p1, 16384);
    record_seg((unsigned long)p2, 32768);
    record_seg((unsigned long)p3, 65536);

    // 验证 check_seg
    assert(check_seg((unsigned long)p0) == 8192);
    assert(check_seg((unsigned long)p1) == 16384);
    assert(check_seg((unsigned long)p2) == 32768);
    assert(check_seg((unsigned long)p3) == 65536);

    // 验证中间地址
    assert(check_seg((unsigned long)p2 + 4096) == 32768 - 4096);

    // 验证未知地址
    assert(check_seg(0xDEADBEEF) == 0);

    printf("addr_seg 4-node: PASS\n");

    numa_free(p0, 8192);
    numa_free(p1, 16384);
    numa_free(p2, 32768);
    numa_free(p3, 65536);
    return 0;
}
```

**预期结果**: 所有 assert 通过

**通过条件**: addr_seg 正确追踪所有 4 node 分配

---

### Week 2 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| backtrace_symbols 在 ARM64 返回 `??` 而非函数名 | 中 | 函数名匹配失败 | 1) 确保编译加 `-rdynamic`；2) 考虑使用 `dladdr()` 替代 `backtrace_symbols`；3) 使用 `addr2line` 做离线解析；4) 实现基于地址范围而非函数名的 fallback |
| 配置文件路径不确定 | 低 | load_obj_config 找不到文件 | 1) 支持环境变量 `SOAR_OBJ_CONF`；2) 默认搜索 `/etc/soar/`, `./`, `~/`；3) 编译时嵌入默认路径 |
| 4 NUMA node 上 numa_alloc_onnode 对 node 2/3 失败 | 低 | 大分配走 default | 检查 `numa_available()` 和 node 内存容量；确认 hugetlb/cgroup 未限制 |
| backtrace 在 ldlib.so 构造函数中不可用 | 中 | m_init 阶段崩溃 | 确保不在 m_init 中调用 backtrace；加载配置文件延迟到第一次 malloc 时 |
| 函数名匹配性能 | 低 | 每次 malloc 都遍历 obj_table | 使用 hash table 替代线性搜索；30000 条目线性搜索 ~15μs，对 >4096 的分配可接受 |

---

## Week 3: profiling buffer 优化 + SPE smoke test + 鲲鹏 L3 延迟基准

### 改动点摘要

| 文件 | 原始代码 | 目标 |
|------|---------|------|
| `src/soar/prof/ldlib.c` L17 | `ARR_SIZE 950000000` (950M entries/core × 40B = 38GB/core) | 改为动态分配或缩小到合理值 |
| `src/soar/interc/ldlib.c` L17 | `ARR_SIZE 550000` (550K entries/core × 40B = 22MB/core) | 可接受，但需验证 120 核场景 |
| `src/soar/run/prof.sh` | Intel PEBS 事件 | ARM SPE 事件 |
| `src/soar/run/proc_obj_e.py` | 解析 Intel PEBS 格式 | 解析 ARM SPE 格式 |
| 新增 | 无 | L3 延迟基准测试脚本 |

### 测试用例

#### T3.1: profiling buffer 内存占用验证

**目的**: 确认 ARR_SIZE 调整后，120 核并发时不会 OOM。

**计算**:
```
原版 prof/ldlib.c:
  ARR_SIZE = 950000000
  sizeof(struct log) = 8(rdt) + 8(addr) + 8(size) + 8(entry_type) + 8(callchain_size) + 8*5(callchain_strings) = 80 bytes
  每核内存 = 950000000 × 80 = 76 GB  ← 完全不可行

合理值计算:
  鲲鹏930 总内存 = 约 512GB DDR
  预留 60% 给应用 = 307GB
  可用于 profiling = 约 100GB
  120 核 → 每核 833MB
  ARR_SIZE = 833MB / 80B ≈ 10,000,000 (1000 万条/核)
```

**测试步骤**:
```bash
# 在鲲鹏930上
cat /proc/meminfo | grep MemTotal
numactl --hardware | grep "size"

# 修改 ARR_SIZE 为 10000000，编译
# 运行测试程序 120 线程
LD_PRELOAD=./prof/ldlib.so stress-ng --vm 120 --vm-bytes 1G --timeout 30s

# 监控内存
watch -n 1 "free -h; ps aux --sort=-rss | head -5"
```

**预期结果**:
- 120 线程 × 800MB/线程 ≈ 96GB profiling buffer 总占用
- 系统可用内存 > 100GB，不触发 OOM
- 无 SIGKILL (oom-killer)

**通过条件**: 120 核场景下稳定运行 30s，无 OOM

---

#### T3.2: ARM SPE 可用性验证

**目的**: 确认鲲鹏930 上 ARM SPE 硬件和内核支持。

**测试脚本**: `test_spe_avail.sh`
```bash
#!/bin/bash
echo "=== ARM SPE Availability Check ==="

# 1. 检查 SPE 设备节点
echo -n "SPE device: "
if [ -d /sys/bus/event_source/devices/arm_spe_0 ]; then
    echo "FOUND (arm_spe_0)"
    ls -la /sys/bus/event_source/devices/arm_spe_0/
    cat /sys/bus/event_source/devices/arm_spe_0/format/* 2>/dev/null
else
    echo "NOT FOUND"
fi

# 2. 检查 perf 对 SPE 的支持
echo -n "perf SPE support: "
perf list 2>/dev/null | grep -i "arm_spe"
if [ $? -eq 0 ]; then
    echo "YES"
else
    echo "NO"
fi

# 3. 检查内核配置
echo -n "Kernel SPE config: "
grep -i "ARM_SPE\|SPE_PMU" /boot/config-$(uname -r) 2>/dev/null || echo "not found in config"

# 4. 尝试一次 SPE 记录
echo -n "SPE record test: "
perf record -e arm_spe_0/period=4096/ -o /tmp/spe_test.data -- sleep 0.1 2>&1 | head -5
if [ -f /tmp/spe_test.data ]; then
    echo "SUCCESS"
    ls -la /tmp/spe_test.data
    perf report -i /tmp/spe_test.data --stdio 2>&1 | head -20
    rm -f /tmp/spe_test.data
else
    echo "FAILED"
fi
```

**预期结果**:
- `/sys/bus/event_source/devices/arm_spe_0/` 存在
- `perf list` 包含 `arm_spe_0` 相关事件
- `perf record -e arm_spe_0/...` 成功，生成 data 文件

**通过条件**: SPE record 成功，perf report 可解析

---

#### T3.3: ARM SPE 数据格式验证

**目的**: 确认 ARM SPE 输出的数据字段与 Intel PEBS 对应，proc_obj_e.py 可以适配。

**测试脚本**: `test_spe_format.sh`
```bash
#!/bin/bash
# 运行一个简单程序并采集 SPE 数据
cat > /tmp/spe_test_prog.c << 'EOF'
#include <stdlib.h>
#include <string.h>
int main() {
    char *buf = malloc(64 * 1024 * 1024);  // 64MB
    for (int i = 0; i < 100; i++) {
        memset(buf, i & 0xff, 64 * 1024 * 1024);
    }
    free(buf);
    return 0;
}
EOF

gcc -O2 -o /tmp/spe_test_prog /tmp/spe_test_prog.c

# 采集 SPE 数据
perf record -e arm_spe_0/period=4096,load_filter=1,store_filter=1/ \
    -o /tmp/spe_test.data -- /tmp/spe_test_prog

# 解析
perf script -i /tmp/spe_test.data > /tmp/spe_test.txt
head -50 /tmp/spe_test.txt
wc -l /tmp/spe_test.txt

# 检查关键字段
echo "=== Sample line analysis ==="
head -1 /tmp/spe_test.txt | tr ' ' '\n' | cat -n
```

**预期结果**:
- ARM SPE perf script 输出包含：
  - **地址** (data virtual/physical address)
  - **时间戳** (TSC timestamp)
  - **PC** (program counter)
  - **操作类型** (load/store/branch)
- 样本行格式示例: ` 1234 5678/arm_spe_0/: ... addr 0x7fff12345678 ...`
- 行数 > 1000（取决于 period 和工作负载）

**通过条件**: SPE 数据包含地址、时间戳、PC 字段，行数 > 100

---

#### T3.4: SPE 替代 PEBS 的 proc_obj_e.py 适配验证

**目的**: 确认修改后的 proc_obj_e.py 可以解析 ARM SPE 格式的 perf 数据。

**改动要点**:
```python
# 原 proc_obj_e.py read_file() 解析 Intel PEBS 格式:
#   "pebs:pebs" 行，第 8 列是地址(hex)，第 10 列是时间(hex)
#
# ARM SPE 格式不同，需适配:
#   SPE 行包含 "arm_spe_0" 标识
#   地址字段: "addr 0x..." 或 "data_addr 0x..."
#   时间字段: "time 12345678" (十进制或十六进制)
```

**测试脚本**: `test_proc_obj_spe.py`
```python
import sys
sys.path.insert(0, '/path/to/soar-arm/src/soar/run')
from proc_obj_e import read_file

# 使用 T3.3 生成的 SPE 数据
data = read_file("/tmp/spe_test.txt")
print(f"Read {len(data)} data streams")
for i, stream in enumerate(data):
    print(f"  Stream {i}: {stream[2]} samples")

# 验证数据有效性
assert len(data) > 0
assert data[0][2] > 0, "No SPE samples found"
```

**预期结果**:
- read_file() 正确解析 SPE 格式
- 样本数 > 0
- 地址和时间戳字段有效（非全零）

**通过条件**: 解析成功，样本数 > 100

---

#### T3.5: 鲲鹏930 L3 缓存延迟基准测量

**目的**: 测量鲲鹏930 各 NUMA 节点间 L3 命中/未命中延迟，作为 SOAR 对象排名的基准数据。

**测试脚本**: `test_l3_latency.c`
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>

#define CACHE_LINE_SIZE 64
#define L3_SIZE (64 * 1024 * 1024)  // 估计鲲鹏930 L3 约 64MB per socket
#define STRIDE CACHE_LINE_SIZE
#define ITERS 1000000

static inline uint64_t clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Pointer chasing with L3-sized buffer */
uint64_t measure_latency(void *buf, size_t size, int iter) {
    volatile char *p = (char *)buf;
    uint64_t start = clock_ns();
    for (int i = 0; i < iter; i++) {
        p = *(char **)p;
    }
    uint64_t end = clock_ns();
    return (end - start) / iter;
}

int main() {
    if (numa_available() < 0) return 1;

    int max_node = numa_max_node();
    size_t sizes[] = {
        4 * 1024,           // 4KB - L1 fit
        64 * 1024,          // 64KB - L2 fit
        512 * 1024,         // 512KB - L3 fit (per core)
        8 * 1024 * 1024,    // 8MB - L3 shared hit
        64 * 1024 * 1024,   // 64MB - L3 miss, local DRAM
        256 * 1024 * 1024,  // 256MB - local DRAM
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    printf("Compute node: %d\n\n", 0);
    printf("%-15s", "Size");
    for (int n = 0; n <= max_node; n++)
        printf("  Node%-3d", n);
    printf("\n");

    for (int s = 0; s < nsizes; s++) {
        printf("%-15zu", sizes[s]);
        for (int n = 0; n <= max_node; n++) {
            void *buf = numa_alloc_onnode(sizes[s], n);
            if (!buf) { printf("  FAIL   "); continue; }

            /* 初始化指针链 */
            char **ptr = (char **)buf;
            size_t count = sizes[s] / STRIDE;
            for (size_t i = 0; i < count - 1; i++) {
                ptr[i * (STRIDE/sizeof(char*))] = (char *)&ptr[(i+1) * (STRIDE/sizeof(char*))];
            }
            ptr[(count-1) * (STRIDE/sizeof(char*))] = (char *)&ptr[0];

            /* 绑定到 node 0 的 CPU */
            numa_run_on_node(0);

            /* 预热 */
            measure_latency(buf, sizes[s], 1000);

            /* 测量 */
            uint64_t lat = measure_latency(buf, sizes[s], ITERS);
            printf("  %4luns ", lat);

            numa_free(buf, sizes[s]);
        }
        printf("\n");
    }

    return 0;
}
```

**预期结果** (鲲鹏930 参考):

| Buffer Size | Node 0 (本地) | Node 1 (同socket) | Node 2 (跨socket) | Node 3 (跨socket) |
|-------------|-------------|-----------------|-----------------|-----------------|
| 4KB (L1) | 2-4 ns | 2-4 ns | 2-4 ns | 2-4 ns |
| 64KB (L2) | 5-8 ns | 5-8 ns | 5-8 ns | 5-8 ns |
| 512KB (L3) | 15-20 ns | 15-20 ns | 15-20 ns | 15-20 ns |
| 8MB (L3共享) | 20-30 ns | 20-30 ns | 60-80 ns | 60-80 ns |
| 64MB (本地DRAM) | 80-120 ns | 90-130 ns | 150-200 ns | 150-200 ns |
| 256MB (远程DRAM) | 100-140 ns | 110-150 ns | 180-250 ns | 180-250 ns |

**关键指标**:
- L3 命中延迟: **15-20 ns**
- 本地 DRAM 延迟: **80-120 ns**
- 同 socket 远端 DRAM: **90-130 ns**
- 跨 socket DRAM: **150-250 ns**
- 跨/本比值: **~2x**

**通过条件**: 测量结果在上述范围 ±30% 内

---

#### T3.6: SPE 与 PEBS 数据覆盖率对比

**目的**: 量化 ARM SPE 相对 Intel PEBS 的数据覆盖率差异。

**测试方法**:
```bash
# 在 x86 上采集 PEBS (如果可复用之前数据)
# x86: perf record -e mem_load_retired.l3_miss:P -c 3001 -p $pid -o pebs.data

# 在 ARM 上采集 SPE
# ARM: perf record -e arm_spe_0/period=4096,load_filter=1/ -p $pid -o spe.data

# 对比样本数
echo "PEBS samples:"; perf script -i pebs.data | wc -l
echo "SPE samples:"; perf script -i spe.data | wc -l

# 对比 L3 miss 覆盖
# SPE 可以过滤: arm_spe_0/period=4096,load_filter=1,min_latency=30/
# (min_latency=30 过滤出 L3 miss 级别的访问)
```

**预期结果**:
- SPE period=4096 时，样本密度约为每 4096 次操作采样一次
- SPE 可配置 `min_latency` 过滤 L3 miss，类似 PEBS 的 `l3_miss` 事件
- 样本覆盖率: SPE 约 PEBS 的 50-80%（取决于 period 配置）

**通过条件**: SPE 可采集 L3 miss 级别数据，样本数 > 1000

---

### Week 3 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| ARM SPE 内核模块未加载 | 中 | `arm_spe_0` 设备不存在 | `modprobe arm_spe_pmu`；检查内核 CONFIG_ARM_SPE_PMU=y；如不支持则 fallback 到 perf mem |
| SPE period 太小导致采样风暴 | 中 | 性能下降 >10% | period 最小 4096，推荐 65536；先测单线程开销再开多线程 |
| SPE 不支持 min_latency 过滤 | 中 | 无法区分 L3 hit/miss | 使用 `perf mem` 替代（基于 PEBS-like 的 software sampling）；或后处理时根据延迟阈值过滤 |
| proc_obj_e.py 解析 SPE 格式工作量大 | 高 | 延期 | 分两步: 1) 先用 perf script 输出文本格式解析; 2) 后续优化为直接解析 perf.data 二进制 |
| 鲲鹏930 L3 延迟测量受 SVE 自动向量化干扰 | 低 | 延迟数据不准 | 编译测试程序时 `-O0`，避免自动向量化；使用 `volatile` 阻止优化 |
| 120核并发 profiling buffer 总内存超限 | 中 | OOM kill | ARR_SIZE 从 950M 降到 10M；实现按需增长 buffer；或限制 profiling 线程数 |

---

## Week 4: 集成测试 + obj_stat.csv 输出验证

### 改动点摘要

| 组件 | 目标 |
|------|------|
| 完整 pipeline | prof → proc_obj_e.py → interc 三阶段串联 |
| obj_stat.csv | 4 NUMA node 场景下的对象排名正确 |
| SPE perf 事件 | proc_obj_e.py 完整适配 ARM SPE |
| config.sh | 适配 ARM 平台（无 intel_pstate，无 HT） |

### 测试用例

#### T4.1: 完整 Pipeline 端到端测试

**目的**: 验证 Phase 1 改动后的完整 SOAR 工作流在鲲鹏930 上可用。

**步骤**:
```bash
# ===== Phase 1: Profiling =====
cd /path/to/soar-arm/src/soar/

# 修改 prof.sh 适配 ARM
# - 替换 PERF 路径
# - 替换 perf 事件为 SPE
# - 调整输出路径

# 使用 microbenchmark 作为测试工作负载
cd /path/to/soar-arm/src/microbenchmark/src/
make clean && make  # 编译 microbenchmark (需适配 ARM)

# 运行 profiling
export OMP_NUM_THREADS=8
source /path/to/soar-arm/src/soar/run/config.sh
LD_PRELOAD=/path/to/soar-arm/src/soar/prof/ldlib.so \
    numactl --cpunodebind=0 --membind=0 \
    ./bench -t 8 -A 64 -B 64 -i 5 -r 0 -R 0.0

# 采集 SPE 数据 (并行)
perf record -e arm_spe_0/period=4096,load_filter=1,store_filter=1/ \
    -o rst/spe.data -p $! -- sleep 30

# ===== Phase 2: Analysis =====
cd rst/
perf script -i spe.data > spe.txt
python3 /path/to/soar-arm/src/soar/run/proc_obj_e.py alloc/

# ===== Phase 3: Allocation =====
# 检查生成的 obj_stat.csv
cat obj_stat.csv | head -20

# 根据 obj_stat.csv 生成 soar_obj.conf
python3 gen_config_from_stat.py obj_stat.csv > soar_obj.conf

# 使用 interc 运行
SOAR_OBJ_CONF=./soar_obj.conf \
LD_PRELOAD=/path/to/soar-arm/src/soar/interc/ldlib.so \
    numactl --cpunodebind=0 --membind=0 \
    ./bench -t 8 -A 64 -B 64 -i 5 -r 0 -R 0.0
```

**预期结果**:
- Phase 1: 生成 `data.raw.*` 文件，每个线程一个
- Phase 2: 生成 `obj_stat.csv`, `obj_rank.csv`, `obj_scores.csv`
- Phase 3: interc 根据 soar_obj.conf 将对象分配到指定 NUMA node

**通过条件**: 三个阶段全部完成，无崩溃，obj_stat.csv 非空

---

#### T4.2: obj_stat.csv 格式和内容验证

**目的**: 确认 obj_stat.csv 在 ARM 平台上的输出格式正确，内容合理。

**验证脚本**: `test_obj_stat.py`
```python
import pandas as pd
import sys

def validate_obj_stat(csv_path):
    df = pd.read_csv(csv_path)

    # 1. 列名检查
    required_cols = ['obj_name', 'scores', 'max_range', 'score_per_range', 'time_span']
    for col in required_cols:
        assert col in df.columns, f"Missing column: {col}"

    # 2. 行数检查
    assert len(df) > 0, "Empty CSV"
    assert len(df) <= 30000, f"Too many objects: {len(df)}"

    # 3. 类型检查
    assert df['scores'].dtype in ['float64', 'int64'], f"Wrong type for scores: {df['scores'].dtype}"
    assert df['time_span'].dtype in ['int64', 'float64'], f"Wrong type for time_span"

    # 4. 值域检查
    assert (df['scores'] >= 0).all(), "Negative scores found"
    assert (df['time_span'] >= 0).all(), "Negative time_span found"
    assert (df['max_range'] >= 0).all(), "Negative max_range found"

    # 5. 排名一致性
    # score_per_range 高的对象应该排在前面
    top5 = df.nlargest(5, 'score_per_range')
    print("Top 5 objects by score_per_range:")
    print(top5[['obj_name', 'scores', 'score_per_range']].to_string())

    # 6. 对象名非空
    assert df['obj_name'].notna().all(), "NULL obj_name found"
    assert (df['obj_name'].str.len() > 0).all(), "Empty obj_name found"

    # 7. ARM 特定检查: obj_name 不应是 hex 地址格式
    hex_count = df['obj_name'].str.match(r'^[0-9a-fA-F]+$').sum()
    if hex_count > 0:
        print(f"WARNING: {hex_count} objects have hex-address names (ARM backtrace may not resolve symbols)")

    print(f"\nValidation PASSED: {len(df)} objects, top score = {df['scores'].max():.2f}")
    return True

if __name__ == '__main__':
    validate_obj_stat(sys.argv[1])
```

**预期结果**:
- 5 列: obj_name, scores, max_range, score_per_range, time_span
- 对象数: 5-500（microbenchmark 应产生少量对象）
- scores >= 0，score_per_range 有差异（区分冷热对象）
- obj_name 为可读函数名（非 hex 地址）

**通过条件**: 所有 assert 通过，无 WARNING

---

#### T4.3: 对象放置效果验证

**目的**: 量化 SOAR 对象放置对应用性能的影响。

**测试脚本**: `test_placement_effect.sh`
```bash
#!/bin/bash
BENCH="/path/to/microbenchmark/bench"
THREADS=8
SIZE=64  # MB
ITERS=10

echo "=== Performance Comparison ==="

# 1. Baseline: 全部 node 0 (本地 DRAM)
echo "--- All on Node 0 ---"
for i in $(seq 1 $ITERS); do
    numactl --cpunodebind=0 --membind=0 $BENCH -t $THREADS -A $SIZE -B $SIZE -i 5 -r 0 -R 0.0
done 2>&1 | grep -E "time|elapsed"

# 2. Baseline: 全部 node 2 (远端 DRAM)
echo "--- All on Node 2 ---"
for i in $(seq 1 $ITERS); do
    numactl --cpunodebind=0 --membind=2 $BENCH -t $THREADS -A $SIZE -B $SIZE -i 5 -r 0 -R 0.0
done 2>&1 | grep -E "time|elapsed"

# 3. SOAR: 根据排名放置
echo "--- SOAR Placement ---"
for i in $(seq 1 $ITERS); do
    SOAR_OBJ_CONF=./soar_obj.conf \
    LD_PRELOAD=/path/to/soar-arm/src/soar/interc/ldlib.so \
    numactl --cpunodebind=0 $BENCH -t $THREADS -A $SIZE -B $SIZE -i 5 -r 0 -R 0.0
done 2>&1 | grep -E "time|elapsed"
```

**预期结果**:
- Node 0 (本地): baseline 最快
- Node 2 (远端): 比 Node 0 慢 **1.5-2.5x**
- SOAR: 接近 Node 0 水平（热对象在本地，冷对象在远端）

**通过条件**: SOAR 性能 >= Node 0 的 80%

---

#### T4.4: 配置文件热加载验证

**目的**: 确认 soar_obj.conf 修改后，重新加载不影响正在运行的程序。

**测试方法**:
```bash
# 启动长时间运行的测试程序
SOAR_OBJ_CONF=./soar_obj.conf LD_PRELOAD=./interc/ldlib.so \
    ./long_running_test &

# 运行中修改配置文件
cp soar_obj.conf soar_obj.conf.bak
echo "new_hot_func = 0" >> soar_obj.conf

# 发送 SIGHUP 触发重载（如果实现了信号处理）
kill -HUP $!

# 验证新配置生效
# （查看日志或行为变化）
```

**注意**: 如果未实现信号处理，此测试标记为 TODO。

**通过条件**: 配置重载后行为改变（如实现）或不影响运行（如未实现）

---

#### T4.5: 120 核满载压力测试

**目的**: 在鲲鹏930 全部 120 核上运行，验证无死锁、无崩溃。

**测试脚本**: `test_120core_stress.sh`
```bash
#!/bin/bash

# 编译多线程测试程序
cat > /tmp/stress_120.c << 'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <numa.h>

#define NTHREADS 120
#define ITERS 10000

void *thread_func(void *arg) {
    int tid = *(int *)arg;
    for (int i = 0; i < ITERS; i++) {
        // 小分配
        void *p1 = malloc(64);
        // 大分配 (触发 NUMA aware)
        void *p2 = malloc(8192);
        // 释放
        free(p1);
        free(p2);
    }
    return NULL;
}

int main() {
    pthread_t threads[NTHREADS];
    int tids[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("120-core stress: PASS\n");
    return 0;
}
EOF

gcc -O2 -pthread -o /tmp/stress_120 /tmp/stress_120.c -lnuma

# prof 版本压力测试
echo "=== prof/ldlib.so 120-core stress ==="
LD_PRELOAD=/path/to/soar-arm/src/soar/prof/ldlib.so /tmp/stress_120

# interc 版本压力测试
echo "=== interc/ldlib.so 120-core stress ==="
SOAR_OBJ_CONF=./soar_obj.conf LD_PRELOAD=/path/to/soar-arm/src/soar/interc/ldlib.so /tmp/stress_120
```

**预期结果**:
- 120 线程全部正常退出
- 无段错误、无死锁、无 OOM
- prof 版本: data.raw 文件数量 = 活跃线程数 (≤MAX_TID=512)
- interc 版本: 所有 malloc/free 配对正确

**通过条件**: 两个版本均正常退出，exit code 0

---

#### T4.6: proc_obj_e.py ARM SPE 适配完整验证

**目的**: 确认 proc_obj_e.py 适配 ARM SPE 后，输出与 x86 PEBS 版本可比。

**验证清单**:
```python
# 验证点:
# 1. read_file() 解析 SPE 格式 → addr_1, time_1 列表非空
# 2. process_file() 解析 alloc 数据 → data 列表非空
# 3. get_alloc_data() 关联 alloc/dealloc → df_obj_deletes 非空
# 4. check_obj_accesses_perf_with_addr_range_p() → accesses_perf_with_addr_range 非空
# 5. rank_objs_r() → obj_scores 字典非空
# 6. create_obj_stat_csv() → obj_stat.csv 生成且格式正确
```

**具体测试**:
```bash
# 使用 T4.1 生成的数据
cd /path/to/rst/

# 运行分析
python3 /path/to/soar-arm/src/soar/run/proc_obj_e.py alloc/ 2>&1 | tee /tmp/week4_analysis.log

# 检查输出文件
for f in obj_stat.csv obj_rank.csv obj_scores.csv obj_aol.csv obj_acc.csv obj_data.csv; do
    if [ -f "$f" ]; then
        lines=$(wc -l < "$f")
        echo "$f: $lines lines"
    else
        echo "$f: MISSING"
    fi
done

# 验证 obj_stat.csv
python3 test_obj_stat.py obj_stat.csv
```

**预期结果**:
- 所有 6 个 CSV 文件生成
- obj_stat.csv: 5+ 行数据
- obj_rank.csv: 对象排名有效
- 无 Python 异常

**通过条件**: 所有文件生成，obj_stat.csv 验证通过

---

#### T4.7: config.sh ARM 适配验证

**目的**: 确认 config.sh 在鲲鹏930 上不会执行 x86 专有命令。

**需修改的函数**:
```bash
# 原版 config.sh 问题:
disable_turbo() → 写 intel_pstate/no_turbo → ARM 上不存在
disable_ht() → 写 smt/control → ARM 上可能不存在
configure_cxl_exp_cores() → 硬编码 node1 offline → ARM 4 node 不同
check_pmqos() → pmqos 二进制需重新编译
```

**测试**:
```bash
source config.sh
# 逐个调用，确认无 error
get_sysinfo
disable_thp
disable_numa_balancing
disable_ksm
disable_swap
set_performance_mode  # ARM 上可能有 cpufreq
# disable_turbo → 跳过（ARM 无 intel_pstate）
# disable_ht → 检查是否存在 smt/control
```

**预期结果**:
- 所有函数无 error 退出
- disable_turbo 在 ARM 上优雅跳过（或改为 ARM 频率锁定方式）
- configure_cxl_exp_cores 改为 4 node 配置

**通过条件**: source config.sh 无报错

---

### Week 4 风险与应对

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| proc_obj_e.py 处理 SPE 数据时内存不足 | 中 | 分析阶段 OOM | 减少 alloc 目录数据量；增加 swap；分批处理 |
| obj_stat.csv 中对象名全部为 `??` | 中 | 无法生成有效的 soar_obj.conf | 1) 编译应用时加 `-rdynamic -fno-omit-frame-pointer`；2) 使用 addr2line 后处理；3) 实现 dladdr 实时解析 |
| 120 核并发时 tids[] 竞争 | 低 | tid_index 错误 | 原 pthread_mutex_lock 保护已足够；但 120 核竞争严重，考虑 per-thread lock-free 方案 |
| microbenchmark 在 ARM 上编译失败 | 低 | 无法运行工作负载 | 修改 Makefile: -march=native → -march=armv8.2-a+sve；移除 `#include <immintrin.h>` |
| SPE 数据量远超 PEBS | 中 | 磁盘空间不足 | 调大 period (4096→65536)；限制采集时间；使用压缩 |
| numa_alloc_onnode 对跨 socket 的 node 2/3 分配慢 | 低 | malloc 延迟增加 | 这是预期行为；测量并记录跨 socket 分配开销，确认 < 10μs |

---

## 测试工具总览

### 自研测试程序

| 文件名 | 用途 | 代码行 |
|--------|------|--------|
| `test_tsc_monotonic.c` | clock_gettime 单调性 | ~40 |
| `test_clock_overhead.c` | clock_gettime 开销 | ~25 |
| `test_frame_pointer.c` | rbp→x29 帧指针 | ~40 |
| `test_preload.c` | LD_PRELOAD 基本功能 | ~25 |
| `test_config_parse.c` | 配置文件解析 | ~30 |
| `test_check_trace.c` | 函数名匹配 | ~25 |
| `test_backtrace_format.c` | backtrace 格式 | ~25 |
| `test_numa_4node.c` | 4 NUMA node 分配 | ~50 |
| `test_interc_routing.c` | 完整拦截路由 | ~50 |
| `test_addr_seg.c` | addr_seg 4 node | ~35 |
| `test_l3_latency.c` | L3 延迟基准 | ~80 |
| `test_obj_stat.py` | CSV 格式验证 | ~40 |
| `stress_120.c` | 120 核压力测试 | ~30 |

### 辅助脚本

| 文件名 | 用途 |
|--------|------|
| `test_spe_avail.sh` | SPE 可用性检查 |
| `test_spe_format.sh` | SPE 数据格式验证 |
| `test_placement_effect.sh` | 放置效果对比 |
| `test_120core_stress.sh` | 120 核压力测试 |
| `gen_config_from_stat.py` | obj_stat.csv → soar_obj.conf 生成器 |

### 外部工具

| 工具 | 用途 | 安装 |
|------|------|------|
| `perf` 6.6.0 | SPE 数据采集 | 系统自带 |
| `numactl` | NUMA 绑定和查询 | `yum install numactl` |
| `stress-ng` | 压力测试 | `yum install stress-ng` |
| `python3` + polars/pandas | 数据分析 | `pip3 install -r requirements.txt` |
| `valgrind` | 内存错误检查 | `yum install valgrind`（可选） |

---

## 通过标准总览

| Week | 测试 | 通过条件 | 关键指标 |
|------|------|---------|---------|
| W1 | T1.1 | 编译零 error | aarch64 ELF .so |
| W1 | T1.2 | 时间戳单调 | violations == 0 |
| W1 | T1.3 | clock_gettime < 50ns/call | 18-30 ns |
| W1 | T1.4 | backtrace >= 3 层 | 含函数名 |
| W1 | T1.5 | 两平台编译成功 | ARCH 匹配 |
| W1 | T1.6 | LD_PRELOAD 正常 | exit 0 |
| W2 | T2.1 | 配置解析正确 | 5 entries |
| W2 | T2.2 | 函数名匹配正确 | 精确+部分 |
| W2 | T2.3 | backtrace 格式正确 | 含函数名 |
| W2 | T2.4 | 4 node 分配成功 | actual == requested |
| W2 | T2.5 | 路由到正确 node | 匹配→指定, 不匹配→默认 |
| W2 | T2.6 | addr_seg 追踪正确 | 所有 assert |
| W3 | T3.1 | 120 核不 OOM | 30s 稳定 |
| W3 | T3.2 | SPE 可用 | arm_spe_0 存在 |
| W3 | T3.3 | SPE 数据有效 | 地址+时间戳+PC |
| W3 | T3.4 | proc_obj_e.py 解析成功 | 样本 > 100 |
| W3 | T3.5 | L3 延迟在范围 | 本地 80-120ns, 远端 150-250ns |
| W3 | T3.6 | SPE 覆盖率 ≥ PEBS 50% | 样本 > 1000 |
| W4 | T4.1 | Pipeline 端到端成功 | 3 阶段完成 |
| W4 | T4.2 | obj_stat.csv 格式正确 | 所有 assert |
| W4 | T4.3 | SOAR 性能 ≥ 本地 80% | 对比基准 |
| W4 | T4.4 | 热加载不崩溃 | 行为改变或无影响 |
| W4 | T4.5 | 120 核压力通过 | exit 0 |
| W4 | T4.6 | 完整分析输出 | 6 个 CSV |
| W4 | T4.7 | config.sh 无报错 | source 成功 |

---

## 里程碑检查清单

### Week 1 完成标准
- [ ] prof/ldlib.c 和 interc/ldlib.c 在 ARM64 上编译通过
- [ ] clock_gettime 替换 rdtsc 后，时间戳单调递增
- [ ] x29 帧指针可正确读取
- [ ] Makefile 条件编译在两平台工作
- [ ] LD_PRELOAD 功能正常

### Week 2 完成标准
- [ ] check_trace() 重写为函数名匹配
- [ ] soar_obj.conf 解析器工作
- [ ] 4 NUMA node 分配正确
- [ ] 端到端拦截路由到正确 node
- [ ] addr_seg 管理 4 node 正确

### Week 3 完成标准
- [ ] ARR_SIZE 调整，120 核不 OOM
- [ ] ARM SPE 可用并采集数据
- [ ] proc_obj_e.py 适配 SPE 格式
- [ ] L3 延迟基准测量完成
- [ ] SPE 数据质量满足分析需求

### Week 4 完成标准
- [ ] 完整 Pipeline 在 ARM 上端到端运行
- [ ] obj_stat.csv 格式和内容正确
- [ ] SOAR 对象放置效果接近本地 DRAM
- [ ] 120 核压力测试通过
- [ ] config.sh 适配 ARM 平台

---

## 附录 A: syscall(186) 问题

原版代码 `tid = (int) syscall(186)` 在 ARM64 上错误：
- x86_64: `__NR_gettid = 186`
- aarch64: `__NR_gettid = 178`

**修复**:
```c
#include <sys/syscall.h>
tid = (int) syscall(__NR_gettid);
// 或更简洁:
#include <unistd.h>
tid = (int) gettid();
```

## 附录 B: ARM SPE perf 事件映射

| Intel PEBS 事件 | ARM SPE 等价 | 说明 |
|----------------|-------------|------|
| `mem_load_retired.l3_miss` | `arm_spe_0/min_latency=30/` | SPE 无直接 L3 miss 事件，用延迟阈值过滤 |
| `CYCLE_ACTIVITY.STALLS_L3_MISS` | 无直接等价 | 需用 `arm_spe_0/` 全量采样 + 后处理 |
| `OFFCORE_REQUESTS.DEMAND_DATA_RD` | 无直接等价 | SPE 可统计 load 操作数 |
| `OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD` | 无直接等价 | 需从 SPE 时间戳推算 |

**proc_obj_e.py 适配策略**:
1. `read_file()`: 改为解析 SPE 格式，提取 `(timestamp, data_addr)` 对
2. `process_perf()`: SPE 无结构化计数器，改用 SPE 采样数据推算
3. `a_lat` (amortized latency): 从 SPE 样本的延迟字段直接计算
4. `est_dram_sd_2`: 调整公式，使用 ARM SPE 的延迟信息

## 附录 C: 鲲鹏930 NUMA 拓扑参考

```
Socket 0 (Node 0 + Node 1):
  ├─ Node 0: CPU 0-59, DDR 128GB
  └─ Node 1: CPU 60-119? (待确认)

Socket 1 (Node 2 + Node 3):
  ├─ Node 2: CPU ?, DDR
  └─ Node 3: CPU ?, DDR

NUMA 距离:
  同 node: 10
  同 socket 不同 node: 12
  跨 socket: 35-40
```

**注意**: 具体拓扑需在鲲鹏930 上 `numactl --hardware` 确认。120 核可能为 2 socket × 60 核/socket，每个 socket 分为 2 个 NUMA node (共 4 node)。
