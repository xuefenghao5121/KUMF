# Phase 1 有效性测试方案

> 只验证 Phase 1 改动点本身是否正确工作，不延伸到 Phase 2+。
>
> Phase 1 改动清单：
> 1. rdtsc → clock_gettime
> 2. rbp → x29 (ARM64 frame pointer)
> 3. syscall(186) → syscall(__NR_gettid)（ARM64 gettid=178）
> 4. Makefile 条件编译 (aarch64)
> 5. check_trace() 重写为函数名匹配 + 配置文件驱动
> 6. 4 NUMA node 拓扑支持（原版只有 0/1）
> 7. ARR_SIZE 缩减（950M → 合理值）
> 8. immintrin.h 移除（microbenchmark）

---

## 一、自动化测试用例

每个测试对应一个具体改动点，判定标准明确。

### T1: 编译通过测试

**改动点**: 1, 2, 3, 4, 8（全部改动点的基础）

**步骤**:
```bash
cd ~/tiered-memory/soar-arm/src/soar/prof/ && make clean && make 2>&1
cd ~/tiered-memory/soar-arm/src/soar/interc/ && make clean && make 2>&1
cd ~/tiered-memory/soar-arm/src/microbenchmark/src/ && make clean && make 2>&1
```

**判定**:
- 三个组件编译零 error ✅
- `file ldlib.so` 包含 `ARM aarch64` ✅
- 无 x86 特定指令编译错误 ✅

---

### T2: 时间戳单调性测试

**改动点**: 1（rdtsc → clock_gettime）

**代码** `test_tsc.c`:
```c
#include <stdio.h>
#include <stdint.h>
#include <time.h>

int main() {
    struct timespec ts;
    uint64_t prev = 0, violations = 0;
    for (int i = 0; i < 1000000; i++) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        uint64_t curr = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        if (curr < prev) violations++;
        prev = curr;
    }
    printf("violations=%lu\n", violations);
    return violations > 0 ? 1 : 0;
}
```

**判定**: `violations=0` ✅

---

### T3: 栈回溯测试

**改动点**: 2（rbp → x29）

**代码** `test_bp.c`:
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <execinfo.h>

void __attribute__((noinline)) func_c(void) {
    void *buf[5];
    int n = backtrace(buf, 5);
    char **syms = backtrace_symbols(buf, n);
    for (int i = 0; i < n; i++) printf("[%d] %s\n", i, syms[i]);
}
void __attribute__((noinline)) func_b(void) { func_c(); }
void __attribute__((noinline)) func_a(void) { func_b(); }
int main() { func_a(); return 0; }
```

**编译**: `gcc -g -O0 -fno-omit-frame-pointer -rdynamic -o test_bp test_bp.c`

**判定**: 输出包含 `func_c`、`func_b`、`func_a` 三个函数名 ✅

---

### T4: gettid 系统调用号测试

**改动点**: 3（syscall(186) → __NR_gettid）

**代码** `test_tid.c`:
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>

void *thread(void *arg) {
    int tid1 = (int)syscall(186);        // 原版 x86 方式
    int tid2 = (int)syscall(__NR_gettid); // 修正后
    int tid3 = (int)gettid();             // glibc 封装
    printf("syscall(186)=%d __NR_gettid=%d gettid()=%d\n", tid1, tid2, tid3);
    return NULL;
}

int main() {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread, NULL);
    pthread_create(&t2, NULL, thread, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
```

**判定**:
- `__NR_gettid` 值 == 178（ARM64）✅
- `syscall(186)` 结果 != `gettid()`（证明原版在 ARM 上错误）✅
- `syscall(__NR_gettid)` == `gettid()`（修正后正确）✅

---

### T5: 配置文件解析 + 函数名匹配测试

**改动点**: 5（check_trace 重写）

**配置** `test.conf`:
```ini
func_hot = 0
func_cold = 2
func_warm = 1
```

**代码** `test_check_trace.c`:
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>
#include <numa.h>
#include <numaif.h>

#define MAX_RULES 256
static struct { char pattern[256]; int tier; } rules[MAX_RULES];
static int nrules = 0;

int load_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pat[256]; int tier;
        if (sscanf(line, "%255s = %d", pat, &tier) == 2) {
            strcpy(rules[nrules].pattern, pat);
            rules[nrules].tier = tier;
            nrules++;
        }
    }
    fclose(f);
    return nrules;
}

int check_trace_name(void **chain, int n) {
    char **syms = backtrace_symbols(chain, n);
    for (int i = 0; i < n; i++) {
        for (int r = 0; r < nrules; r++) {
            if (strstr(syms[i], rules[r].pattern)) {
                free(syms);
                return rules[r].tier;
            }
        }
    }
    free(syms);
    return -1;
}

void __attribute__((noinline)) func_hot(void) {
    void *chain[5]; int n = backtrace(chain, 5);
    int tier = check_trace_name(chain, n);
    printf("func_hot: tier=%d (expect 0)\n", tier);
}
void __attribute__((noinline)) func_cold(void) {
    void *chain[5]; int n = backtrace(chain, 5);
    int tier = check_trace_name(chain, n);
    printf("func_cold: tier=%d (expect 2)\n", tier);
}
void __attribute__((noinline)) func_warm(void) {
    void *chain[5]; int n = backtrace(chain, 5);
    int tier = check_trace_name(chain, n);
    printf("func_warm: tier=%d (expect 1)\n", tier);
}
void __attribute__((noinline)) func_unknown(void) {
    void *chain[5]; int n = backtrace(chain, 5);
    int tier = check_trace_name(chain, n);
    printf("func_unknown: tier=%d (expect -1)\n", tier);
}

int main(int argc, char *argv[]) {
    const char *conf = argc > 1 ? argv[1] : "test.conf";
    printf("Loaded %d rules from %s\n", load_conf(conf), conf);
    func_hot();
    func_cold();
    func_warm();
    func_unknown();
    return 0;
}
```

**判定**:
- func_hot → tier=0 ✅
- func_cold → tier=2 ✅
- func_warm → tier=1 ✅
- func_unknown → tier=-1 ✅

---

### T6: 4 NUMA Node 分配测试

**改动点**: 6（4 node 拓扑支持）

**代码** `test_numa4.c`:
```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <numa.h>
#include <numaif.h>

int main() {
    if (numa_available() < 0) { printf("NUMA not available\n"); return 1; }
    int max = numa_max_node();
    printf("NUMA nodes: %d\n", max + 1);
    for (int n = 0; n <= max; n++) {
        size_t sz = 64 * 1024;
        void *p = numa_alloc_onnode(sz, n);
        memset(p, 0xAA, sz);
        int loc;
        get_mempolicy(&loc, NULL, 0, p, MPOL_F_ADDR);
        printf("  Requested node %d, actual node %d %s\n",
               n, loc, loc == n ? "✓" : "✗");
        numa_free(p, sz);
    }
    return 0;
}
```

**判定**: 4 个 node 都能成功分配，actual == requested ✅

---

### T7: ARR_SIZE 内存测试

**改动点**: 7（buffer 缩减）

**代码** `test_arrsize.c`:
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 模拟 ARR_SIZE=10000000 时的每线程内存
#define NEW_ARR_SIZE 10000000
struct log { uint64_t rdt; void *addr; size_t size; long type; size_t ccs; void *cc[5]; };

int main() {
    size_t per_thread = NEW_ARR_SIZE * sizeof(struct log);
    printf("sizeof(struct log)=%zu\n", sizeof(struct log));
    printf("per thread: %zu MB\n", per_thread / (1024*1024));
    printf("40 threads: %zu MB\n", 40 * per_thread / (1024*1024));
    printf("120 threads: %zu MB\n", 120 * per_thread / (1024*1024));

    void *buf = malloc(per_thread);
    if (!buf) { printf("FAIL: single-thread alloc failed\n"); return 1; }
    memset(buf, 0, per_thread);
    free(buf);
    printf("Single-thread alloc: OK\n");
    return 0;
}
```

**判定**:
- 单线程分配成功 ✅
- 120 线程总内存 < 100GB（不 OOM）✅

---

### T8: LD_PRELOAD 拦截功能测试

**改动点**: 全部改动点的集成验证

**代码** `test_preload.c`:
```c
#include <stdio.h>
#include <stdlib.h>

int main() {
    void *p1 = malloc(128);
    void *p2 = malloc(8192);
    void *p3 = malloc(1024*1024);
    printf("small=%p large=%p huge=%p\n", p1, p2, p3);
    free(p1); free(p2); free(p3);
    printf("OK\n");
    return 0;
}
```

**步骤**:
```bash
gcc -O0 -g -fno-omit-frame-pointer -rdynamic -o test_preload test_preload.c
LD_PRELOAD=./prof/ldlib.so ./test_preload
LD_PRELOAD=./interc/ldlib.so SOAR_OBJ_CONF=./test.conf ./test_preload
```

**判定**: 两次运行均正常退出，exit code 0 ✅

---

## 二、手工验证方案

> 以下步骤在鲲鹏930上手动执行，不需要写代码，用现成工具验证。

### 验证 1: SPE 可用性确认

```bash
ssh kunpeng

# 检查 SPE 设备
ls /sys/bus/event_source/devices/arm_spe_0/
# 应该看到: caps cpumask format type ...

# 检查 perf 支持
perf list | grep arm_spe
# 应该看到: arm_spe_0//  [Kernel PMU event]

# 试采集 3 秒
perf record -e arm_spe// -a -o /tmp/spe_test.data -- sleep 3
# 应该成功，显示 [ perf record: Woken up ... ]

# 查看输出格式
perf script -i /tmp/spe_test.data | head -20
# 应该看到包含地址和时间戳的记录

# 检查数据量
ls -lh /tmp/spe_test.data
# 3 秒采集应该在 1-100 MB 范围

rm /tmp/spe_test.data
```

**通过标准**: perf record 成功，perf script 有输出

---

### 验证 2: NUMA 延迟差异确认

```bash
ssh kunpeng

# 查看拓扑
numactl --hardware
# 确认 4 个 node，距离矩阵中有 10/12/35-40

# 在本地 node 跑一个内存密集任务
time numactl --cpunodebind=0 --membind=0 dd if=/dev/zero bs=1M count=1024 2>/dev/null | md5sum
# 记录时间

# 在远端 node 跑同样的任务
time numactl --cpunodebind=0 --membind=2 dd if=/dev/zero bs=1M count=1024 2>/dev/null | md5sum
# 记录时间

# 对比：远端应该更慢（如果内存操作是瓶颈的话）
```

**通过标准**: numactl --hardware 显示 4 node + 距离差异

---

### 验证 3: LD_PRELOAD 编译产物检查

```bash
ssh kunpeng
cd ~/tiered-memory/soar-arm/

# 编译
cd src/soar/prof/ && make && cd -
cd src/soar/interc/ && make && cd -

# 检查产物
file src/soar/prof/ldlib.so
# 应输出: ELF 64-bit LSB shared object, ARM aarch64, ...

file src/soar/interc/ldlib.so
# 同上

# 检查是否链接了 libnuma
ldd src/soar/interc/ldlib.so | grep numa
# 应输出: libnuma.so.1 => /lib64/libnuma.so.1

# 检查是否还有 x86 指令引用
objdump -d src/soar/prof/ldlib.so | grep rdtsc
# 应该无输出（已替换）

objdump -d src/soar/prof/ldlib.so | grep -i "rdtsc\|immintrin\|rbp"
# 应该无输出
```

**通过标准**: ELF aarch64 + 有 libnuma + 无 x86 指令残留

---

### 验证 4: profiling 输出文件检查

```bash
ssh kunpeng
cd ~/tiered-memory/soar-arm/

# 编译测试程序
cat > /tmp/test_simple.c << 'EOF'
#include <stdlib.h>
#include <string.h>
int main() {
    for (int i = 0; i < 100; i++) {
        void *p = malloc(65536);
        memset(p, i, 65536);
        free(p);
    }
    return 0;
}
EOF
gcc -O0 -g -fno-omit-frame-pointer -rdynamic -o /tmp/test_simple /tmp/test_simple.c

# 用 prof 版本运行
mkdir -p /tmp/soar_test
cd /tmp/soar_test
LD_PRELOAD=~/tiered-memory/soar-arm/src/soar/prof/ldlib.so /tmp/test_simple

# 检查输出
ls -la data.raw.* 2>/dev/null || ls -la /tmp/soar_test/
# 应该有 data.raw.* 文件生成

# 检查文件内容（应该有时间戳和地址）
head -5 data.raw.* 2>/dev/null
# 应该能看到记录
```

**通过标准**: data.raw 文件生成 + 内容非空

---

### 验证 5: SPE + 工作负载联合采集

```bash
ssh kunpeng

# 后台跑一个简单内存密集程序
numactl --cpunodebind=0 --membind=0 dd if=/dev/urandom bs=1M count=512 of=/dev/null &
PID=$!

# 用 SPE 采集它的内存访问
perf record -e arm_spe/freq=1,load_filter=1/ -p $PID -o /tmp/spe_workload.data -- sleep 5

# 停止工作负载
kill $PID 2>/dev/null; wait $PID 2>/dev/null

# 查看结果
perf report -i /tmp/spe_workload.data --stdio | head -30
perf script -i /tmp/spe_workload.data | wc -l
# 应该有几千到几万条记录

# 检查是否包含地址信息
perf script -i /tmp/spe_workload.data | head -5
# 应该能看到地址字段

rm /tmp/spe_workload.data
```

**通过标准**: perf report 有数据 + perf script 输出含地址

---

### 验证 6: NUMA 绑定实际效果验证

```bash
ssh kunpeng

# 编译一个简单指针追踪程序
cat > /tmp/lat_test.c << 'EOF'
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
int main() {
    int N = 1024*1024;
    int *arr = malloc(N * sizeof(int));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int iter = 0; iter < 1000; iter++)
        for (int i = 0; i < N; i++) arr[i] += 1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec)/1e9;
    printf("%.3f sec\n", sec);
    free(arr);
    return 0;
}
EOF
gcc -O2 -o /tmp/lat_test /tmp/lat_test.c

# 本地内存
echo -n "Node 0: "; numactl --cpunodebind=0 --membind=0 /tmp/lat_test

# 远端内存
echo -n "Node 2: "; numactl --cpunodebind=0 --membind=2 /tmp/lat_test

# 对比：Node 2 应该比 Node 0 慢
```

**通过标准**: Node 2 耗时 > Node 0（证明 NUMA 延迟差异可观测）

---

## 三、通过标准汇总

| 测试 | 改动点 | 通过条件 |
|------|--------|---------|
| T1 编译 | 1,2,3,4,8 | 零 error，aarch64 ELF |
| T2 时间戳 | 1 | violations=0 |
| T3 栈回溯 | 2 | ≥3 层函数名可解析 |
| T4 gettid | 3 | __NR_gettid=178，结果正确 |
| T5 函数名匹配 | 5 | 4 种匹配全部正确 |
| T6 NUMA 分配 | 6 | 4 node 分配到正确位置 |
| T7 内存占用 | 7 | 单线程分配成功，120 线程 <100GB |
| T8 LD_PRELOAD | 全部 | 正常退出 |
| 手工验证 1 | SPE | perf record 成功 |
| 手工验证 2 | NUMA | 距离矩阵正确 |
| 手工验证 3 | 编译产物 | aarch64 + libnuma + 无 x86 残留 |
| 手工验证 4 | profiling | data.raw 生成 |
| 手工验证 5 | SPE+负载 | 有地址的采样记录 |
| 手工验证 6 | NUMA 效果 | 远端比本地慢 |
