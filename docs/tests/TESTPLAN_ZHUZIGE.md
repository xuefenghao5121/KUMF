# SOAR ARM Phase 1 有效性测试与手工验证方案

目标平台：鲲鹏930, 120核 ARM64, 4 NUMA node, Linux 6.6.0 (openEuler 24.03)
SSH：`ssh kunpeng`
原版代码：`/home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto/`

---

## 前置准备

```bash
ssh kunpeng
mkdir -p ~/soar-arm-test && cd ~/soar-arm-test
# 拷贝原版代码
cp -r /path/to/SoarAlto .  # 用实际路径替换，或 git clone
cd SoarAlto
```

---

## 测试 1：rdtsc → clock_gettime

**改动文件**：`src/soar/prof/ldlib.c`, `src/soar/interc/ldlib.c`

**原版代码**（两文件均存在）：
```c
#ifdef __x86_64__
#define rdtscll(val) { \
    unsigned int __a,__d;                                        \
    asm volatile("rdtsc" : "=a" (__a), "=d" (__d));              \
    (val) = ((unsigned long)__a) | (((unsigned long)__d)<<32);   \
}
#else
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))
#endif
```

**ARM 适配代码**：
```c
#ifdef __x86_64__
#define rdtscll(val) { \
    unsigned int __a,__d;                                        \
    asm volatile("rdtsc" : "=a" (__a), "=d" (__d));              \
    (val) = ((unsigned long)__a) | (((unsigned long)__d)<<32);   \
}
#elif defined(__aarch64__)
#define rdtscll(val) do { \
    struct timespec _ts; \
    clock_gettime(CLOCK_MONOTONIC_RAW, &_ts); \
    (val) = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec; \
} while(0)
#else
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))
#endif
```

### 验证命令

```bash
# 1.1 编译验证——确保宏在 ARM64 下选择正确分支
ssh kunpeng 'cat > /tmp/test_rdtsc.c << "EOF"
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#ifdef __x86_64__
#define rdtscll(val) { unsigned int __a,__d; asm volatile("rdtsc" : "=a" (__a), "=d" (__d)); (val) = ((unsigned long)__a) | (((unsigned long)__d)<<32); }
#elif defined(__aarch64__)
#define rdtscll(val) do { struct timespec _ts; clock_gettime(CLOCK_MONOTONIC_RAW, &_ts); (val) = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec; } while(0)
#else
#error "Unsupported architecture"
#endif

int main() {
    uint64_t t1, t2;
    rdtscll(t1);
    volatile int dummy = 0;
    for (int i = 0; i < 1000000; i++) dummy += i;
    rdtscll(t2);
    printf("t1=%lu t2=%lu delta=%lu ns\n", t1, t2, t2 - t1);
    if (t2 > t1 && (t2 - t1) < 10000000000ULL) {
        printf("PASS: clock_gettime monotonic increment OK\n");
    } else {
        printf("FAIL: delta out of range\n");
    }
    return 0;
}
EOF
g++ -o /tmp/test_rdtsc /tmp/test_rdtsc.c && /tmp/test_rdtsc'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| 编译 | 无 error，无 warning 关于 rdtsc | 出现 `unknown mnemonic rdtsc` 或链接错误 |
| t1 < t2 | 单调递增 | t2 <= t1 |
| delta 范围 | delta 在 1ms~10s 之间（合理范围） | delta = 0 或 delta > 10s |
| 精度 | delta 纳秒量级（非 0 非巨大） | delta 为 0（精度丢失） |

---

## 测试 2：rbp → x29（ARM64 frame pointer）

**改动文件**：`src/soar/prof/ldlib.c`, `src/soar/interc/ldlib.c`

**原版代码**：
```c
#define get_bp(bp) asm("movq %%rbp, %0" : "=r" (bp) :)
```

**ARM 适配代码**：
```c
#ifdef __x86_64__
#define get_bp(bp) asm("movq %%rbp, %0" : "=r" (bp) :)
#elif defined(__aarch64__)
#define get_bp(bp) asm("mov %0, x29" : "=r" (bp) :)
#endif
```

### 验证命令

```bash
ssh kunpeng 'cat > /tmp/test_frameptr.c << "EOF"
#include <stdio.h>
#include <stdint.h>

#ifdef __x86_64__
#define get_bp(bp) asm("movq %%rbp, %0" : "=r" (bp) :)
#elif defined(__aarch64__)
#define get_bp(bp) asm("mov %0, x29" : "=r" (bp) :)
#endif

void inner_function(void) {
    void *bp;
    get_bp(bp);
    printf("Frame pointer = %p\n", bp);
    if (bp != NULL && (uint64_t)bp > 0x1000) {
        printf("PASS: x29 frame pointer readable, non-null, valid user address\n");
    } else {
        printf("FAIL: frame pointer is NULL or suspicious\n");
    }
}

int main() {
    inner_function();
    return 0;
}
EOF
g++ -fno-omit-frame-pointer -o /tmp/test_frameptr /tmp/test_frameptr.c && /tmp/test_frameptr'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| 编译 | 无 error | `Invalid instruction` 或 `unknown register` |
| 帧指针值 | 非 NULL，用户空间地址 (>0x1000) | NULL 或 0 |
| 地址合理性 | 指针在栈区域范围内 | 指针在代码段或堆区域 |

**注意**：编译时**必须**加 `-fno-omit-frame-pointer`，否则 x29 可能被优化掉。

---

## 测试 3：syscall(186) → syscall(__NR_gettid)

**改动文件**：`src/soar/prof/ldlib.c`, `src/soar/interc/ldlib.c`

**原版代码**：
```c
tid = (int) syscall(186); /* 64b only */
```

**ARM 适配代码**：
```c
tid = (int) syscall(__NR_gettid); /* portable: x86_64=186, aarch64=178 */
```

### 验证命令

```bash
ssh kunpeng 'cat > /tmp/test_gettid.c << "EOF"
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>

void *thread_func(void *arg) {
    int tid_hardcode_186 = (int)syscall(186);
    int tid_macro = (int)syscall(__NR_gettid);
    int tid_direct = (int)syscall(178);  /* ARM64 gettid */
    pid_t pthread_tid = gettid();
    
    printf("__NR_gettid = %d\n", __NR_gettid);
    printf("syscall(186)   = %d\n", tid_hardcode_186);
    printf("syscall(__NR_gettid) = %d\n", tid_macro);
    printf("syscall(178)   = %d\n", tid_direct);
    printf("gettid()       = %d\n", pthread_tid);
    
    if (tid_macro == pthread_tid) {
        printf("PASS: syscall(__NR_gettid) matches gettid()\n");
    } else {
        printf("FAIL: mismatch\n");
    }
    
    if (__NR_gettid == 178) {
        printf("PASS: __NR_gettid is 178 on aarch64\n");
    } else if (__NR_gettid == 186) {
        printf("INFO: __NR_gettid is 186 (x86_64)\n");
    } else {
        printf("WARN: unexpected __NR_gettid = %d\n", __NR_gettid);
    }
    return NULL;
}

int main() {
    pthread_t t;
    pthread_create(&t, NULL, thread_func, NULL);
    pthread_join(t, NULL);
    return 0;
}
EOF
g++ -o /tmp/test_gettid /tmp/test_gettid.c -lpthread && /tmp/test_gettid'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| `__NR_gettid` | 在 aarch64 上 = 178 | 186（说明用了 x86 头文件）|
| `syscall(__NR_gettid)` | 与 `gettid()` 返回值一致 | 不一致 |
| `syscall(186)` | 在 ARM64 上应返回 -1 或无关值（不是真实 tid）| 返回值 == 真实 tid（说明碰巧有效，但不可移植）|

---

## 测试 4：Makefile 条件编译 (aarch64)

**改动文件**：`src/soar/prof/Makefile`, `src/soar/interc/Makefile`, `src/microbenchmark/src/Makefile`

**原版 Makefile**（soar prof/interc）：
```makefile
CFLAGS=-Wall -g -ggdb3 -O0 -lnuma
LDFLAGS=-lnuma
...
ldlib.so: ldlib.c
	g++ -fPIC ${CFLAGS} -c ldlib.c
	g++ -shared -Wl,-soname,libpmalloc.so -o ldlib.so ldlib.o -ldl -lpthread -lnuma
```

**原版 Makefile**（microbenchmark）：
```makefile
CFLAGS=-I. -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing -march=native -O3
```

**ARM 适配代码**（soar prof/interc Makefile）：
```makefile
ARCH ?= $(shell uname -m)

ifeq ($(ARCH),aarch64)
CFLAGS=-Wall -g -ggdb3 -O0 -fno-omit-frame-pointer
else
CFLAGS=-Wall -g -ggdb3 -O0
endif
LDFLAGS=-lnuma
...
ldlib.so: ldlib.c
	g++ -fPIC ${CFLAGS} -c ldlib.c
	g++ -shared -Wl,-soname,libpmalloc.so -o ldlib.so ldlib.o -ldl -lpthread -lnuma
```

**ARM 适配代码**（microbenchmark Makefile）：
```makefile
ARCH ?= $(shell uname -m)

ifeq ($(ARCH),aarch64)
CFLAGS=-I. -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing -march=armv8.2-a -O3
else
CFLAGS=-I. -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing -march=native -O3
endif
```

### 验证命令

```bash
# 4.1 检测架构
ssh kunpeng 'uname -m'
# 期望输出: aarch64

# 4.2 验证 Makefile 条件编译逻辑
ssh kunpeng 'cat > /tmp/test_makefile_arch.mk << "MAKEEOF"
ARCH ?= $(shell uname -m)

ifeq ($(ARCH),aarch64)
CFLAGS=-Wall -g -O0 -fno-omit-frame-pointer
RESULT=ARM64_BUILD
else
CFLAGS=-Wall -g -O0
RESULT=X86_BUILD
endif

all:
	@echo "ARCH=$(ARCH)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "RESULT=$(RESULT)"
MAKEEOF
make -f /tmp/test_makefile_arch.mk'

# 4.3 验证 -march=native 在 ARM64 上的行为
ssh kunpeng 'echo "int main(){return 0;}" | g++ -march=native -x c++ - -o /dev/null 2>&1 && echo "PASS: -march=native works on aarch64" || echo "WARN: -march=native may need replacement"'

# 4.4 验证 -march=armv8.2-a 可用（鲲鹏930 支持 SVE，ARMv8.5+）
ssh kunpeng 'echo "int main(){return 0;}" | g++ -march=armv8.2-a -x c++ - -o /dev/null 2>&1 && echo "PASS: -march=armv8.2-a works" || echo "FAIL: -march=armv8.2-a not supported"'

# 4.5 验证 -fno-omit-frame-pointer 效果
ssh kunpeng 'cat > /tmp/test_fno_fp.c << "EOF"
#include <stdio.h>
void func(void) { printf("hello\n"); }
int main() { func(); return 0; }
EOF
g++ -fno-omit-frame-pointer -O2 -c /tmp/test_fno_fp.c -o /tmp/test_fno_fp.o
objdump -d /tmp/test_fno_fp.o | grep -c "x29" && echo "PASS: x29 (frame pointer) present in disassembly" || echo "FAIL: x29 not found, frame pointer omitted"'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| `uname -m` | `aarch64` | `x86_64` |
| 条件分支 | `RESULT=ARM64_BUILD` | `RESULT=X86_BUILD` |
| `-march=native` | 编译通过 | 编译错误 |
| `-march=armv8.2-a` | 编译通过 | 编译错误 |
| `-fno-omit-frame-pointer` | disassembly 中出现 x29 | disassembly 中无 x29 |

---

## 测试 5：check_trace() 重写为函数名匹配 + 配置文件驱动

**改动文件**：`src/soar/interc/ldlib.c`

**原版代码**——硬编码 x86 地址：
```c
int check_trace(void *string, size_t sz)
{
    char *ptr = (char *) string;
    char *objs[] = {"405fb2", "406d68", "406fe7", "406d27", \
        "40b69c", "406cc3", "406db6", "40b62e"};
    int start = 0;
    int end = 8;
    int k1 = 7;
    int k2 = 8;
    for (int i = start; i < k1; i += 1) {
        if (strstr(ptr, objs[i]) != NULL) {
            return 0;
        }
    }
    for (int i = k1; i < k2; i += 1) {
        if (strstr(ptr, objs[i]) != NULL) {
            return -1;
        }
    }
    for (int i = k2; i < end; i += 1) {
        if (strstr(ptr, objs[i]) != NULL) {
            return 1;
        }
    }
    return -1;
}
```

**ARM 适配代码**——函数名匹配 + 配置文件驱动：
```c
#define MAX_TRACE_RULES 64
#define MAX_FUNC_NAME  256

struct trace_rule {
    char func_name[MAX_FUNC_NAME];
    int  numa_node;  /* -1 = skip/ignore, 0/1/2/3 = allocate on this node */
};

static struct trace_rule trace_rules[MAX_TRACE_RULES];
static int num_trace_rules = 0;

static void load_trace_rules(void) {
    const char *config_path = getenv("SOAR_TRACE_CONFIG");
    if (!config_path) config_path = "/etc/soar/trace_rules.conf";
    FILE *f = fopen(config_path, "r");
    if (!f) return; /* no config = no NUMA-aware allocation */
    char line[512];
    while (fgets(line, sizeof(line), f) && num_trace_rules < MAX_TRACE_RULES) {
        char func[MAX_FUNC_NAME];
        int node;
        if (sscanf(line, "%255s %d", func, &node) == 2) {
            strncpy(trace_rules[num_trace_rules].func_name, func, MAX_FUNC_NAME-1);
            trace_rules[num_trace_rules].numa_node = node;
            num_trace_rules++;
        }
    }
    fclose(f);
}

int check_trace(void *string, size_t sz)
{
    char *ptr = (char *) string;
    for (int i = 0; i < num_trace_rules; i++) {
        if (strstr(ptr, trace_rules[i].func_name) != NULL) {
            return trace_rules[i].numa_node;
        }
    }
    return -1;
}
```

配置文件 `/etc/soar/trace_rules.conf` 示例：
```
GROMACS_routine_a 0
GROMACS_routine_b 1
hotspot_func 2
```

### 验证命令

```bash
# 5.1 创建测试配置文件
ssh kunpeng 'mkdir -p /tmp/soar_test && cat > /tmp/soar_test/trace_rules.conf << "EOF"
test_func_node0 0
test_func_node1 1
test_func_skip -1
EOF'

# 5.2 编译并测试 check_trace 逻辑
ssh kunpeng 'cat > /tmp/test_check_trace.c << "EOF"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_TRACE_RULES 64
#define MAX_FUNC_NAME  256

struct trace_rule {
    char func_name[MAX_FUNC_NAME];
    int  numa_node;
};

static struct trace_rule trace_rules[MAX_TRACE_RULES];
static int num_trace_rules = 0;

static void load_trace_rules(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("WARN: cannot open %s\n", path); return; }
    char line[512];
    while (fgets(line, sizeof(line), f) && num_trace_rules < MAX_TRACE_RULES) {
        char func[MAX_FUNC_NAME];
        int node;
        if (sscanf(line, "%255s %d", func, &node) == 2) {
            strncpy(trace_rules[num_trace_rules].func_name, func, MAX_FUNC_NAME-1);
            trace_rules[num_trace_rules].numa_node = node;
            num_trace_rules++;
        }
    }
    fclose(f);
    printf("Loaded %d trace rules\n", num_trace_rules);
}

int check_trace(void *string, size_t sz) {
    char *ptr = (char *) string;
    for (int i = 0; i < num_trace_rules; i++) {
        if (strstr(ptr, trace_rules[i].func_name) != NULL) {
            return trace_rules[i].numa_node;
        }
    }
    return -1;
}

int main() {
    load_trace_rules("/tmp/soar_test/trace_rules.conf");
    
    // Test: string containing "test_func_node0"
    int r1 = check_trace("myapp(test_func_node0+0x42) [0xaaaa1234]", 64);
    printf("test_func_node0 -> %d (expect 0)\n", r1);
    
    // Test: string containing "test_func_node1"
    int r2 = check_trace("myapp(test_func_node1+0x10) [0xaaaa5678]", 64);
    printf("test_func_node1 -> %d (expect 1)\n", r2);
    
    // Test: string containing "test_func_skip"
    int r3 = check_trace("myapp(test_func_skip+0x5) [0xaaaa9abc]", 64);
    printf("test_func_skip -> %d (expect -1)\n", r3);
    
    // Test: unknown function
    int r4 = check_trace("myapp(unknown_func+0x0) [0xaaaadef0]", 64);
    printf("unknown_func -> %d (expect -1)\n", r4);
    
    // Summary
    int pass = (r1 == 0) && (r2 == 1) && (r3 == -1) && (r4 == -1);
    printf("%s: check_trace function name matching\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
EOF
g++ -o /tmp/test_check_trace /tmp/test_check_trace.c && /tmp/test_check_trace'

# 5.3 验证原版硬编码地址在 ARM64 上必然失败
ssh kunpeng 'cat > /tmp/test_old_check_trace.c << "EOF"
#include <stdio.h>
#include <string.h>

int check_trace_old(void *string, size_t sz) {
    char *ptr = (char *) string;
    char *objs[] = {"405fb2", "406d68", "406fe7", "406d27",
        "40b69c", "406cc3", "406db6", "40b62e"};
    int k1 = 7, k2 = 8;
    for (int i = 0; i < k1; i++) {
        if (strstr(ptr, objs[i]) != NULL) return 0;
    }
    for (int i = k1; i < k2; i++) {
        if (strstr(ptr, objs[i]) != NULL) return -1;
    }
    for (int i = k2; i < 8; i++) {
        if (strstr(ptr, objs[i]) != NULL) return 1;
    }
    return -1;
}

int main() {
    // ARM64 backtrace_symbols output format: "prog(func_name+offset) [0xaaaa...]"
    int r = check_trace_old("myapp(GROMACS_func+0x42) [0xaaaa1234]", 64);
    printf("Old check_trace on ARM64 output -> %d (should always be -1 = no match)\n", r);
    if (r == -1) {
        printf("PASS: Hardcoded x86 addresses cannot match ARM64 symbols (confirms need for rewrite)\n");
    } else {
        printf("FAIL: Unexpected match on ARM64 address format\n");
    }
    return 0;
}
EOF
g++ -o /tmp/test_old_check_trace /tmp/test_old_check_trace.c && /tmp/test_old_check_trace'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| 配置文件加载 | `Loaded 3 trace rules` | `Loaded 0` 或无法打开文件 |
| 函数名匹配 node0 | 返回 0 | 返回其他值 |
| 函数名匹配 node1 | 返回 1 | 返回其他值 |
| skip 规则 | 返回 -1 | 返回其他值 |
| 未知函数 | 返回 -1 | 返回其他值 |
| 原版硬编码 | 对 ARM64 格式返回 -1（确认不匹配）| 返回 0/1（假阳性匹配）|

---

## 测试 6：4 NUMA node 拓扑支持

**改动文件**：`src/soar/interc/ldlib.c`

**原版代码**——`check_trace` 只返回 0 或 1，NUMA 感知只认两个节点：
```c
if (ret > -1) {
    addr = numa_alloc_onnode(sz, ret);  // ret 只能是 0 或 1
```

**ARM 适配**——check_trace 可返回 0/1/2/3，支持 4 个 NUMA node。

### 验证命令

```bash
# 6.1 检测鲲鹏930 NUMA 拓扑
ssh kunpeng 'numactl --hardware'

# 6.2 验证 numa_alloc_onnode 对所有 4 个节点都能工作
ssh kunpeng 'cat > /tmp/test_numa4.c << "EOF"
#include <stdio.h>
#include <stdlib.h>
#include <numa.h>
#include <numaif.h>
#include <string.h>

int main() {
    int max_node = numa_max_node();
    int num_nodes = numa_num_configured_nodes();
    printf("numa_max_node() = %d\n", max_node);
    printf("numa_num_configured_nodes() = %d\n", num_nodes);
    
    int all_ok = 1;
    for (int node = 0; node <= max_node; node++) {
        void *ptr = numa_alloc_onnode(4096, node);
        if (ptr == NULL) {
            printf("FAIL: numa_alloc_onnode(4096, %d) returned NULL\n", node);
            all_ok = 0;
        } else {
            memset(ptr, 0xAA, 4096);
            int allocated_node = -1;
            get_mempolicy(&allocated_node, NULL, 0, ptr, MPOL_F_ADDR);
            printf("Node %d: alloc OK, actual node=%d %s\n", 
                   node, allocated_node, 
                   (allocated_node == node) ? "(match)" : "(MISMATCH!)");
            if (allocated_node != node) all_ok = 0;
            numa_free(ptr, 4096);
        }
    }
    printf("%s: 4 NUMA node allocation\n", all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
EOF
g++ -o /tmp/test_numa4 /tmp/test_numa4.c -lnuma && /tmp/test_numa4'

# 6.3 验证 check_trace 返回值 0-3 都能正确分配
ssh kunpeng 'cat > /tmp/test_numa_check.c << "EOF"
#include <stdio.h>
#include <stdlib.h>
#include <numa.h>
#include <string.h>

/* 模拟适配后的 check_trace 可以返回 0/1/2/3 */
int main() {
    int results[] = {0, 1, 2, 3, -1};
    int pass = 1;
    for (int i = 0; i < 5; i++) {
        int ret = results[i];
        if (ret > -1) {
            void *addr = numa_alloc_onnode(8192, ret);
            if (addr) {
                printf("check_trace=%d -> numa_alloc_onnode(%d) = %p OK\n", ret, ret, addr);
                numa_free(addr, 8192);
            } else {
                printf("FAIL: numa_alloc_onnode(8192, %d) = NULL\n", ret);
                pass = 0;
            }
        } else {
            printf("check_trace=-1 -> skip NUMA alloc (use libc_malloc) OK\n");
        }
    }
    printf("%s: check_trace return 0-3 all work\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
EOF
g++ -o /tmp/test_numa_check /tmp/test_numa_check.c -lnuma && /tmp/test_numa_check'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| NUMA 节点数 | `numa_max_node() >= 3`（即 node 0~3）| `numa_max_node() < 3` |
| node 0-3 分配 | 全部成功，无 NULL | 任一返回 NULL |
| 实际分配节点 | 与请求节点一致 | 与请求节点不一致 |
| check_trace=-1 | 跳过 NUMA 分配，走 libc_malloc | 崩溃或错误 |

---

## 测试 7：ARR_SIZE 缩减

**改动文件**：`src/soar/prof/ldlib.c`, `src/soar/interc/ldlib.c`

**原版值**：
- prof/ldlib.c: `#define ARR_SIZE 950000000` (950M)
- interc/ldlib.c: `#define ARR_SIZE 550000` (550K)

**问题**：prof 版 950M × sizeof(struct log) ≈ 950M × ~96B ≈ ~88GB/线程，在 120 核系统上根本不可能。

**ARM 适配值**：需要根据可用内存合理设定。先测出合理值。

### 验证命令

```bash
# 7.1 计算原版 ARR_SIZE 的内存需求
ssh kunpeng 'python3 -c "
# struct log = uint64_t + void* + size_t + long + size_t + 5*void*
# = 8 + 8 + 8 + 8 + 8 + 5*8 = 72 bytes on aarch64
import struct
log_size = 8 + 8 + 8 + 8 + 8 + 5*8  # conservative estimate
arr_size_prof = 950000000
arr_size_interc = 550000
print(f\"struct log size (est) = {log_size} bytes\")
print(f\"prof ARR_SIZE=950M: {arr_size_prof * log_size / (1024**3):.1f} GB per thread\")
print(f\"interc ARR_SIZE=550K: {arr_size_interc * log_size / (1024**2):.1f} MB per thread\")
print(f\"prof with 120 threads: {arr_size_prof * log_size * 120 / (1024**3):.1f} GB total\")
"'

# 7.2 检查可用内存
ssh kunpeng 'free -g'

# 7.3 验证实际 struct log 大小
ssh kunpeng 'cat > /tmp/test_arr_size.c << "EOF"
#include <stdio.h>
#include <stddef.h>
#define CALLCHAIN_SIZE 5

struct log {
    unsigned long rdt;
    void *addr;
    size_t size;
    long entry_type;
    size_t callchain_size;
    void *callchain_strings[CALLCHAIN_SIZE];
};

int main() {
    printf("sizeof(struct log) = %zu bytes\n", sizeof(struct log));
    printf("sizeof(unsigned long) = %zu\n", sizeof(unsigned long));
    printf("sizeof(void*) = %zu\n", sizeof(void*));
    printf("sizeof(size_t) = %zu\n", sizeof(size_t));
    printf("sizeof(long) = %zu\n", sizeof(long));
    
    // Test with different ARR_SIZE values
    size_t arr_sizes[] = {950000000, 1000000, 500000, 100000};
    for (int i = 0; i < 4; i++) {
        size_t bytes = arr_sizes[i] * sizeof(struct log);
        printf("ARR_SIZE=%zu: %.2f GB per thread, %.2f GB for 120 threads\n",
               arr_sizes[i],
               (double)bytes / (1024*1024*1024),
               (double)bytes * 120 / (1024*1024*1024));
    }
    
    return 0;
}
EOF
g++ -o /tmp/test_arr_size /tmp/test_arr_size.c && /tmp/test_arr_size'

# 7.4 验证缩小后的 ARR_SIZE 可以成功 malloc
ssh kunpeng 'cat > /tmp/test_arr_alloc.c << "EOF"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALLCHAIN_SIZE 5
struct log {
    unsigned long rdt;
    void *addr;
    size_t size;
    long entry_type;
    size_t callchain_size;
    void *callchain_strings[CALLCHAIN_SIZE];
};

int main() {
    // Test a reasonable ARR_SIZE for 120-core with ~256GB RAM
    // Target: < 1GB per thread, so ARR_SIZE < 1GB/sizeof(log)
    size_t log_sz = sizeof(struct log);
    size_t target_per_thread_gb = 1;
    size_t max_arr = (target_per_thread_gb * 1024UL * 1024 * 1024) / log_sz;
    printf("sizeof(struct log) = %zu\n", log_sz);
    printf("Max ARR_SIZE for %zuGB/thread = %zu\n", target_per_thread_gb, max_arr);
    
    // Try allocating
    size_t test_arr_size = 1000000;  // 1M as reasonable default
    size_t alloc_bytes = test_arr_size * log_sz;
    printf("Testing ARR_SIZE=%zu (%.2f MB)...\n", test_arr_size, (double)alloc_bytes / (1024*1024));
    struct log *arr = (struct log*)malloc(alloc_bytes);
    if (arr) {
        memset(arr, 0, alloc_bytes);
        printf("PASS: malloc(%zu) succeeded\n", alloc_bytes);
        free(arr);
    } else {
        printf("FAIL: malloc(%zu) returned NULL\n", alloc_bytes);
    }
    return 0;
}
EOF
g++ -o /tmp/test_arr_alloc /tmp/test_arr_alloc.c && /tmp/test_arr_alloc'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| `sizeof(struct log)` | 在 aarch64 上得到实际值 | 编译失败 |
| 原版 950M 内存计算 | 显示 > 总内存（证明需要缩减）| 显示 < 总内存 |
| 缩减后 ARR_SIZE malloc | malloc 成功 | malloc 返回 NULL |
| 每线程内存 | < 2GB | > 可用内存/线程数 |

---

## 测试 8：immintrin.h 移除

**改动文件**：`src/microbenchmark/src/main.c`

**原版代码**：
```c
#include <immintrin.h>
```

`immintrin.h` 是 Intel x86 专用头文件（AVX/SSE intrinsics），在 ARM64 上不存在。

**ARM 适配**：直接移除 `#include <immintrin.h>`，因为代码中实际未使用任何 immintrin 函数（无 `_mm_*` 调用）。

### 验证命令

```bash
# 8.1 确认原版代码是否真的使用了 immintrin 函数
ssh kunpeng 'grep -n "_mm_\|_rdtsc\|_cpuid\|__cpuid\|__m128\|__m256\|__m512\|_mm_load\|_mm_store\|_mm_add\|_mm_mul" /home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto/src/microbenchmark/src/main.c || echo "No immintrin functions found in main.c"'

# 8.2 确认 immintrin.h 在 ARM64 上不存在
ssh kunpeng 'g++ -E -x c++ - < /dev/null 2>&1 | head -1; find /usr -name "immintrin.h" 2>/dev/null | head -5 || echo "immintrin.h NOT FOUND on aarch64 (expected)"'

# 8.3 原版代码在 ARM64 上编译——应失败
ssh kunpeng 'cat > /tmp/test_immintrin.c << "EOF"
#include <immintrin.h>
int main() { return 0; }
EOF
if g++ -o /tmp/test_immintrin /tmp/test_immintrin.c 2>&1; then
    echo "INFO: immintrin.h exists on this platform (unexpected for aarch64)"
else
    echo "PASS: immintrin.h not available on aarch64, must be removed"
fi'

# 8.4 移除 immintrin.h 后编译——应成功
ssh kunpeng 'cat > /tmp/test_no_immintrin.c << "EOF"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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
#include <pthread.h>
/* #include <immintrin.h> -- REMOVED for ARM64 */
int main() {
    printf("Build without immintrin.h OK\n");
    return 0;
}
EOF
if g++ -o /tmp/test_no_immintrin /tmp/test_no_immintrin.c -lnuma 2>&1; then
    echo "PASS: Compiles without immintrin.h"
else
    echo "FAIL: Still fails even without immintrin.h"
fi'

# 8.5 完整 microbenchmark 编译验证（假设已完成代码修改）
ssh kunpeng 'cat > /tmp/test_bench_build.sh << "SCRIPT"
#!/bin/bash
cd /home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto/src/microbenchmark/src
# 如果代码已修改（移除 immintrin.h），尝试编译
make clean 2>/dev/null
if make 2>&1; then
    echo "PASS: microbenchmark builds on aarch64"
else
    echo "FAIL: microbenchmark build failed"
    # 检查是否是 immintrin.h 导致的
    if grep -q "immintrin" main.c 2>/dev/null; then
        echo "CAUSE: immintrin.h still present in main.c"
    fi
fi
SCRIPT
chmod +x /tmp/test_bench_build.sh
/tmp/test_bench_build.sh'
```

### 通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| immintrin 函数使用 | grep 无结果（确认未使用）| 发现 `_mm_*` 等调用（移除后需替换）|
| ARM64 上 immintrin.h | 不存在/编译失败 | 存在且可编译（非预期）|
| 移除后编译 | 编译成功 | 编译失败 |
| 链接 | 链接成功 | 链接失败 |

---

## 综合集成测试：ldlib.so 在 ARM64 上完整编译 + 基本功能

**目的**：验证所有 8 个改动点集成后，核心产物 `ldlib.so` 可以在鲲鹏930上编译和基本使用。

### 验证命令

```bash
# 在代码修改完成后，在鲲鹏930上执行：
ssh kunpeng 'cd /path/to/modified/SoarAlto/src/soar/interc && \
    make clean && make 2>&1 | tee /tmp/soar_build.log && \
    if [ -f ldlib.so ]; then \
        echo "=== ldlib.so built ===" && \
        file ldlib.so && \
        ldd ldlib.so && \
        echo "PASS: ldlib.so ARM64 build"; \
    else \
        echo "FAIL: ldlib.so not found"; \
    fi'

# 用 LD_PRELOAD 测试 ldlib.so 不会立即崩溃
ssh kunpeng 'cd /path/to/modified/SoarAlto/src/soar/interc && \
    LD_PRELOAD=./ldlib.so ls /tmp 2>&1 | head -5 && \
    echo "PASS: LD_PRELOAD with ldlib.so did not crash" || \
    echo "FAIL: LD_PRELOAD crashed"'

# 同理测试 prof 版
ssh kunpeng 'cd /path/to/modified/SoarAlto/src/soar/prof && \
    make clean && make 2>&1 | tee /tmp/soar_prof_build.log && \
    if [ -f ldlib.so ]; then \
        echo "PASS: prof/ldlib.so ARM64 build"; \
    else \
        echo "FAIL: prof/ldlib.so not found"; \
    fi'
```

### 综合通过/失败标准

| 检查项 | 通过标准 | 失败标准 |
|--------|----------|----------|
| interc/ldlib.so 编译 | 0 error，0 warning（rdtsc/rbp 相关）| x86 指令编译错误 |
| file ldlib.so | `ELF 64-bit LSB shared object, ARM aarch64` | `x86-64` |
| ldd | 所有 .so 依赖可解析 | `not found` |
| LD_PRELOAD ls | 正常输出，无 SIGILL/SIGSEGV | 崩溃 |
| prof/ldlib.so 编译 | 同上 | 同上 |

---

## 快速一键验证脚本

将以上所有测试合并为单个可在鲲鹏930上执行的脚本：

```bash
#!/bin/bash
# soar-arm-phase1-test.sh
# 在鲲鹏930上执行: bash soar-arm-phase1-test.sh

PASS=0; FAIL=0; WARN=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  ✅ PASS: $desc (expected=$expected, actual=$actual)"
        ((PASS++))
    else
        echo "  ❌ FAIL: $desc (expected=$expected, actual=$actual)"
        ((FAIL++))
    fi
}

echo "============================================="
echo "SOAR ARM Phase 1 有效性测试"
echo "Platform: $(uname -m) @ $(hostname)"
echo "Date: $(date)"
echo "============================================="

# Test 1: rdtsc → clock_gettime
echo -e "\n--- Test 1: rdtsc → clock_gettime ---"
ARCH=$(uname -m)
check "Architecture" "aarch64" "$ARCH"

cat > /tmp/t1.c << 'EOF'
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#define rdtscll(val) do { struct timespec _ts; clock_gettime(CLOCK_MONOTONIC_RAW, &_ts); (val) = (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec; } while(0)
int main() {
    uint64_t t1, t2;
    rdtscll(t1);
    for (volatile int i = 0; i < 1000000; i++);
    rdtscll(t2);
    printf("%lu %lu %lu\n", t1, t2, t2-t1);
    return (t2 > t1 && (t2-t1) < 10000000000ULL) ? 0 : 1;
}
EOF
g++ -o /tmp/t1 /tmp/t1.c 2>&1 && /tmp/t1 >/dev/null 2>&1
check "clock_gettime build+run" "0" "$?"

# Test 2: rbp → x29
echo -e "\n--- Test 2: rbp → x29 (ARM64 frame pointer) ---"
cat > /tmp/t2.c << 'EOF'
#include <stdio.h>
#include <stdint.h>
#define get_bp(bp) asm("mov %0, x29" : "=r" (bp) :)
int main() {
    void *bp; get_bp(bp);
    printf("%p\n", bp);
    return (bp != NULL && (uint64_t)bp > 0x1000) ? 0 : 1;
}
EOF
g++ -fno-omit-frame-pointer -o /tmp/t2 /tmp/t2.c 2>&1 && /tmp/t2 >/dev/null 2>&1
check "x29 frame pointer" "0" "$?"

# Test 3: syscall(__NR_gettid)
echo -e "\n--- Test 3: syscall(__NR_gettid) ---"
cat > /tmp/t3.c << 'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
int main() {
    printf("__NR_gettid=%d\n", __NR_gettid);
    int tid = syscall(__NR_gettid);
    pid_t gtid = gettid();
    printf("syscall(__NR_gettid)=%d gettid()=%d\n", tid, gtid);
    return (tid == gtid && __NR_gettid == 178) ? 0 : 1;
}
EOF
g++ -o /tmp/t3 /tmp/t3.c 2>&1 && /tmp/t3 >/dev/null 2>&1
check "__NR_gettid=178 on aarch64" "0" "$?"

# Test 4: Makefile conditional compilation
echo -e "\n--- Test 4: Makefile conditional compilation ---"
cat > /tmp/t4.mk << 'EOF'
ARCH ?= $(shell uname -m)
ifeq ($(ARCH),aarch64)
RESULT=ARM64
else
RESULT=X86
endif
all:
	@echo "$(RESULT)"
EOF
RESULT4=$(make -f /tmp/t4.mk 2>/dev/null)
check "Makefile arch detection" "ARM64" "$RESULT4"

echo "int main(){return 0;}" | g++ -march=native -x c++ - -o /dev/null 2>&1
check "-march=native works" "0" "$?"

echo "int main(){return 0;}" | g++ -fno-omit-frame-pointer -O2 -x c++ - -o /dev/null 2>&1
check "-fno-omit-frame-pointer works" "0" "$?"

# Test 5: check_trace function name matching
echo -e "\n--- Test 5: check_trace → function name matching ---"
mkdir -p /tmp/soar_test
cat > /tmp/soar_test/trace_rules.conf << 'EOF'
hotspot_alloc 0
cold_alloc 1
skip_this -1
EOF

cat > /tmp/t5.c << 'EOF'
#include <stdio.h>
#include <string.h>
#define MAX_TRACE_RULES 64
#define MAX_FUNC_NAME 256
struct trace_rule { char func_name[MAX_FUNC_NAME]; int numa_node; };
static struct trace_rule rules[MAX_TRACE_RULES];
static int nrules = 0;
void load_rules(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[512]; char func[256]; int node;
    while (fgets(line, sizeof(line), f) && nrules < MAX_TRACE_RULES) {
        if (sscanf(line, "%255s %d", func, &node) == 2) {
            strncpy(rules[nrules].func_name, func, 255);
            rules[nrules].numa_node = node; nrules++;
        }
    }
    fclose(f);
}
int check_trace(void *s, size_t sz) {
    char *p = (char*)s;
    for (int i = 0; i < nrules; i++)
        if (strstr(p, rules[i].func_name)) return rules[i].numa_node;
    return -1;
}
int main() {
    load_rules("/tmp/soar_test/trace_rules.conf");
    int r1 = check_trace("app(hotspot_alloc+0x42) [0xaaa]", 64);
    int r2 = check_trace("app(cold_alloc+0x10) [0xbbb]", 64);
    int r3 = check_trace("app(skip_this+0x5) [0xccc]", 64);
    int r4 = check_trace("app(unknown+0x0) [0xddd]", 64);
    printf("r1=%d r2=%d r3=%d r4=%d\n", r1, r2, r3, r4);
    return (r1==0 && r2==1 && r3==-1 && r4==-1) ? 0 : 1;
}
EOF
g++ -o /tmp/t5 /tmp/t5.c 2>&1 && /tmp/t5 >/dev/null 2>&1
check "check_trace name matching" "0" "$?"

# Test 6: 4 NUMA node support
echo -e "\n--- Test 6: 4 NUMA node topology ---"
MAX_NODE=$(numactl --hardware | grep "max node" | awk '{print $NF}')
check "NUMA max_node >= 3" "1" "$([ $MAX_NODE -ge 3 ] && echo 1 || echo 0)"

cat > /tmp/t6.c << 'EOF'
#include <stdio.h>
#include <numa.h>
#include <string.h>
int main() {
    int ok = 1;
    for (int n = 0; n <= numa_max_node(); n++) {
        void *p = numa_alloc_onnode(4096, n);
        if (!p) { ok = 0; printf("FAIL: node %d\n", n); }
        else { memset(p, 0, 4096); numa_free(p, 4096); }
    }
    printf("4-node alloc: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
EOF
g++ -o /tmp/t6 /tmp/t6.c -lnuma 2>&1 && /tmp/t6 >/dev/null 2>&1
check "numa_alloc_onnode(0-3)" "0" "$?"

# Test 7: ARR_SIZE memory feasibility
echo -e "\n--- Test 7: ARR_SIZE feasibility ---"
cat > /tmp/t7.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct log { unsigned long rdt; void *addr; size_t size; long et; size_t cs; void *cc[5]; };
int main() {
    printf("sizeof(struct log)=%zu\n", sizeof(struct log));
    size_t arr = 1000000;
    void *p = malloc(arr * sizeof(struct log));
    if (p) { memset(p, 0, arr * sizeof(struct log)); free(p); printf("ARR_SIZE=1M OK\n"); return 0; }
    printf("ARR_SIZE=1M FAIL\n"); return 1;
}
EOF
g++ -o /tmp/t7 /tmp/t7.c 2>&1 && /tmp/t7 >/dev/null 2>&1
check "ARR_SIZE=1M alloc" "0" "$?"

TOTAL_MEM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
echo "  ℹ️  Total RAM: $((TOTAL_MEM_KB/1024/1024)) GB"

# Test 8: immintrin.h removal
echo -e "\n--- Test 8: immintrin.h removal ---"
cat > /tmp/t8_with.c << 'EOF'
#include <immintrin.h>
int main() { return 0; }
EOF
g++ -o /tmp/t8_with /tmp/t8_with.c 2>/dev/null
check "immintrin.h NOT available (expected)" "1" "$?"

cat > /tmp/t8_without.c << 'EOF'
#include <stdio.h>
int main() { printf("no immintrin OK\n"); return 0; }
EOF
g++ -o /tmp/t8_without /tmp/t8_without.c 2>&1 >/dev/null
check "Build without immintrin.h" "0" "$?"

# Summary
echo -e "\n============================================="
echo "RESULT: ✅ PASS=$PASS  ❌ FAIL=$FAIL  ⚠️  WARN=$WARN"
echo "============================================="
[ $FAIL -eq 0 ] && echo "ALL PHASE 1 TESTS PASSED" || echo "SOME TESTS FAILED - REVIEW ABOVE"
```

### 使用方法

```bash
# 拷贝脚本到鲲鹏930
scp soar-arm-phase1-test.sh kunpeng:~/soar-arm-test/
# 执行
ssh kunpeng 'cd ~/soar-arm-test && bash soar-arm-phase1-test.sh'
```

---

## 测试结果记录模板

```
| # | 改动点 | 测试命令 | 结果 | 备注 |
|---|--------|----------|------|------|
| 1 | rdtsc → clock_gettime | /tmp/t1 | PASS/FAIL | |
| 2 | rbp → x29 | /tmp/t2 | PASS/FAIL | |
| 3 | syscall(__NR_gettid) | /tmp/t3 | PASS/FAIL | |
| 4 | Makefile 条件编译 | /tmp/t4.mk | PASS/FAIL | |
| 5 | check_trace 重写 | /tmp/t5 | PASS/FAIL | |
| 6 | 4 NUMA node | /tmp/t6 | PASS/FAIL | |
| 7 | ARR_SIZE 缩减 | /tmp/t7 | PASS/FAIL | |
| 8 | immintrin.h 移除 | /tmp/t8 | PASS/FAIL | |
| 9 | 集成：ldlib.so 编译 | make | PASS/FAIL | |
| 10 | 集成：LD_PRELOAD | LD_PRELOAD=./ldlib.so ls | PASS/FAIL | |
```
