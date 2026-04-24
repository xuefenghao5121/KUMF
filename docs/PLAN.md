# ARM Tiered Memory Allocator — 项目计划

> 基于 OSDI'25 论文《Tiered Memory Management Beyond Hotness》的 ARM 原生实现
>
> 创建日期: 2026-04-24 | 审查: 柱子哥 | 状态: 规划中

---

## 项目定位

**不是移植，是重新实现 + 创新。**

在 ARM64（鲲鹏930）平台上实现 AOL 感知的 tiered memory 分配器，验证 SVE 感知评分和 ARM SPE 精确采样，以 GROMACS 作为核心验证应用。

### 核心创新点

1. **AOL 指标的 ARM 重建** — 首次在 ARM 上用 SPE + cache PMU 重建 Amortized Offcore Latency
2. **SVE 感知评分** — 利用 SVE 向量 load 的天然高 MLP 特征，优化对象分级
3. **ARM SPE 精确采样** — SPE 的 data_source/latency 信息比 Intel PEBS 更丰富
4. **GROMACS HPC 验证** — 用 ns/day 真实性能指标验证，不是 microbenchmark

### 目标平台

| 项目 | 规格 |
|------|------|
| 服务器 | 鲲鹏930, 120核 ARM64 |
| NUMA | 4 node × (40核 + ~64GB) |
| 延迟 | 同 socket 12, 跨 socket 35-40 (~3x) |
| SPE | ✅ arm_spe_0 已确认可用 |
| SVE | ✅ 支持 |
| 内核 | Linux 6.6.0 (openEuler 24.03 LTS-SP3) |
| 工具 | libnuma 2.0.16, perf 6.6.0, GCC 12.3.1 |

---

## Phase 0: 环境验证 ✅ 已完成 (2026-04-24)

- [x] NUMA topology 确认（4 node, 延迟差异 3x）
- [x] ARM SPE 确认可用（arm_spe_0, type=135）
- [x] SVE 特性确认
- [x] libnuma / perf / GCC 确认
- [x] SoarAlto 代码克隆

---

## Phase 1: SOAR ARM 适配 + 对象识别重写 (4周)

> 目标：在鲲鹏930上跑通 SOAR profiling pipeline，输出 obj_stat.csv

### Week 1: 用户态基础设施适配

#### 1.1 时间戳采集替换
- **文件**: `src/soar/prof/ldlib.c`, `src/soar/interc/ldlib.c`
- **改动**: `rdtsc` → `clock_gettime(CLOCK_MONOTONIC_RAW)`
- **原因**: ARM 没有直接等价的 rdtsc 指令（ARM 有 PMU cycle counter 但需要内核权限）
- **实现**:
  ```c
  // 替换 rdtsc
  static inline void rdtscll(uint64_t *val) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
      *val = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  }
  ```
- **验证**: 编译通过 + 时间戳单调递增

#### 1.2 栈回溯替换
- **文件**: `src/soar/prof/ldlib.c`, `src/soar/interc/ldlib.c`
- **改动**: `get_bp` (x86 rbp) → ARM64 x29 frame pointer 或 `__builtin_return_address()`
- **实现**:
  ```c
  #ifdef __aarch64__
  #define get_bp(bp) asm("mov %0, x29" : "=r" (bp) :)
  #else
  #define get_bp(bp) asm("movq %%rbp, %0" : "=r" (bp) :)
  #endif
  ```
- **注意**: ARM64 需要 `-fno-omit-frame-pointer` 编译选项
- **备选**: `backtrace()` / `__builtin_return_address(n)` 逐级获取

#### 1.3 Makefile 条件编译
- **文件**: 所有 Makefile
- **改动**: 添加 `aarch64` 架构检测
- **实现**:
  ```makefile
  ARCH ?= $(shell uname -m)
  ifeq ($(ARCH), aarch64)
      CFLAGS += -DARM64 -fno-omit-frame-pointer
  else
      CFLAGS += -DX86_64
  endif
  ```
- **依赖**: 安装 `numactl-devel`（鲲鹏上已有）

#### 1.4 编译验证
- [ ] `prof/ldlib.c` 编译为 `libmemprof.so`
- [ ] `interc/ldlib.c` 编译为 `libmeminterc.so`
- [ ] 简单测试程序跑通

### Week 2: 对象识别机制重写

#### 2.1 重写 check_trace() — 核心改动
- **问题**: 原版硬编码 x86 代码段偏移地址，ARM 上完全不可用
- **方案**: 函数名匹配 + 配置文件驱动
- **实现**:
  ```c
  // 新版 check_trace
  // 用 backtrace_symbols() 获取函数名
  // 用配置文件定义规则: 函数名/对象名 → NUMA tier
  
  typedef struct {
      char pattern[256];   // 函数名匹配模式（支持通配符）
      int target_tier;     // 0=local, 1=remote, -1=default
  } alloc_rule_t;
  
  // 从 /etc/tiered_alloc.conf 或环境变量加载规则
  // 匹配逻辑: strstr(callchain_symbol, rule.pattern)
  ```

#### 2.2 配置文件格式
- **文件**: `/etc/tiered_alloc.conf` 或 `TIERED_ALLOC_CONF` 环境变量指定
- **格式**:
  ```ini
  # tiered_alloc.conf
  # pattern = tier (0=fast/local, 1=slow/remote, -1=default)
  
  # GROMACS rules
  gromacs.*force* = 0
  gromacs.*coord* = 0
  gromacs.*state* = 0
  gromacs.*topology* = 1
  gromacs.*trajectory* = 1
  
  # Generic rules
  *.tmp* = 1
  *.log* = 1
  *.buffer* = -1
  ```
- **优先级**: 后匹配的规则覆盖先匹配的（类似 iptables）

#### 2.3 4 NUMA node 拓扑支持
- **问题**: 原版只有 2 node（local/remote），鲲鹏有 4 node
- **方案**: 基于距离的 tier 映射
  ```c
  typedef struct {
      int node_id;
      int distance;    // 从当前 CPU 的 NUMA 距离
      int tier;        // 0=fast, 1=medium, 2=slow
  } numa_tier_t;
  
  // 自动检测拓扑
  // distance <= 15: tier 0 (local + same socket)
  // distance <= 30: tier 1 (medium)
  // distance > 30:  tier 2 (remote socket)
  ```
- **鲲鹏930 预期**:
  - Tier 0: Node 0, Node 1（distance 10-12，同 socket）
  - Tier 2: Node 2, Node 3（distance 35-40，跨 socket）

### Week 3: Profiling Buffer 优化 + SPE Smoke Test

#### 3.1 Profiling Buffer 改造
- **问题**: 原版 ARR_SIZE=950M，每个线程 ~72GB，鲲鹏 node 只有 64GB
- **方案**: 改用 mmap 文件 ring buffer
  ```c
  // 替代: 用 mmap 文件做 ring buffer
  #define RING_BUFFER_SIZE (256 * 1024 * 1024)  // 256MB per thread
  
  // mmap 文件到 /tmp/tiered_prof_<tid>.buf
  // 环形写入，满了就刷新到磁盘
  // 分析阶段再读回来
  ```
- **或者**: 降低 ARR_SIZE 到 500000（~40MB/thread），限制最大线程数

#### 3.2 ARM SPE Smoke Test
- **目的**: 验证 SPE 采集数据可用性
- **测试**:
  ```bash
  # 基本功能测试
  perf record -e arm_spe// -p <pid> -- sleep 5
  perf script | head -50
  
  # 检查输出是否包含:
  # - 操作类型 (LOAD/STORE/BRANCH)
  # - 数据地址 (va)
  # - 延迟 (latency)
  # - 数据源 (data_source: L1/L2/L3/remote)
  ```
- **验收标准**: SPE 能输出包含地址和延迟的采样记录

#### 3.3 鲲鹏930 L3 延迟基准测量
- **目的**: 替换原版的 SKX L3 延迟常数 (24.87 cycles)
- **方法**: 用 microbenchmark 测量
  ```bash
  # 编译 microbenchmark（需去掉 x86 immintrin.h，改用 ARM SVE 或 generic pointer chase）
  # 分别在 local node 和 remote node 上跑 pointer-chasing
  # 测量 L1/L2/L3/remote DDR 延迟
  ```
- **预期输出**: 鲲鹏930 的各层延迟基准值表

### Week 4: 集成测试 + obj_stat.csv 输出

#### 4.1 简单负载 Profiling
- 用简单程序（矩阵乘法 / 链表遍历）测试完整 pipeline:
  1. `LD_PRELOAD=libmemprof.so ./test_program` → 采集分配记录
  2. `perf record -e arm_spe// ...` → 采集内存访问
  3. `python3 proc_obj_e.py .` → 分析输出 obj_stat.csv
- **验收**: obj_stat.csv 包含对象 ID、大小、访问频率、评分

#### 4.2 NUMA 延迟验证
- 分别绑到不同 node 跑同一程序，测量性能差异
  ```bash
  # Fast tier (local)
  numactl --cpunodebind=0 --membind=0 ./test_program
  
  # Slow tier (remote)
  numactl --cpunodebind=0 --membind=2 ./test_program
  
  # 对比执行时间，确认 3x 延迟差异可观测
  ```

#### Phase 1 交付物
- [ ] ARM 适配后的 SOAR 用户态代码
- [ ] 配置文件驱动的对象识别机制
- [ ] 4 NUMA node 拓扑支持
- [ ] SPE smoke test 报告
- [ ] 鲲鹏930 延迟基准值表
- [ ] obj_stat.csv 输出验证

---

## Phase 2: ARM SPE 集成 + AOL 重建 (4周)

> 目标：实现 ARM 上的 AOL 指标计算，替换 Intel PMU 事件依赖
> ⚠️ 这是项目的 **go/no-go 点** — AOL 精度决定项目成败

### Week 5: SPE 数据 Pipeline

#### 5.1 SPE 采集 Pipeline 搭建
- **替代原版的 PEBS tracepoint 方式**
- **流程**:
  ```
  perf record -e arm_spe/freq=1,min_latency=10/ -a -- <workload>
  ↓
  perf script (或 perf data convert --to-json)
  ↓
  SPE 记录解析器 (Python/C)
  ↓
  {timestamp, va, pa, op_type, latency, data_source, pc, sve_width}
  ```

#### 5.2 SPE 记录解析器
- **文件**: `tools/spe_parser.py`（新建）
- **功能**: 解析 perf script 输出的 SPE 记录
- **字段提取**:
  | 字段 | 来源 | 用途 |
  |------|------|------|
  | timestamp | SPE TSC | 关联分配记录 |
  | va (虚拟地址) | SPE | 匹配对象 |
  | latency | SPE issue-to-complete | AOL 分子 |
  | data_source | SPE (L1/L2/L3/remote) | 区分 local/remote |
  | pc | SPE IP | 调用链分析 |
  | sve_width | SPE SVE 信息 | SVE 感知评分 |

#### 5.3 SPE 采样率调优
- **问题**: SPE 默认采样率极高，数据量远大于 PEBS
- **关键参数**: `min_latency=N`（只记录延迟 ≥ N cycle 的 load）
- **调优策略**:
  ```
  1. 先用 min_latency=30（L3 miss 级别）采集
  2. 检查数据量：如果 < 100MB/min → 可以降低到 20
  3. 如果 > 1GB/min → 提高到 50
  4. 目标：数据量控制在 500MB/min 以内
  ```

### Week 6: AOL 重建 — 方案 A 实现

#### 6.1 AOL 计算公式（ARM 版）
- **原版 (Intel)**: `AOL = OFFCORE_LATENCY / MLP`
- **ARM 重建**:
  ```
  AOL = avg_spe_latency / MLP_approx
  
  其中:
  - avg_spe_latency: SPE 报告的 load 平均延迟（只取 remote 访问）
  - MLP_approx = bus_access / bus_cycles（ARM cache PMU 事件）
  - 或者: MLP_approx = mem_access / stall_backend_cycles
  ```

#### 6.2 ARM PMU 事件映射
- **Intel → ARM 事件对照表**:

  | Intel 事件 | ARM 替代 | 用途 |
  |-----------|---------|------|
  | OFFCORE_REQUESTS.DEMAND_DATA_RD | armv8_pmuv3/bus_access/ | 内存访问次数 |
  | OFFCORE_REQUESTS_OUTSTANDING.* | armv8_pmuv3/stall_backend/ | 后端停顿周期 |
  | CYCLE_ACTIVITY.STALLS_L3_MISS | SPE data_source=remote 的 latency | L3 miss 延迟 |
  | CPU_CLK_UNHALTED.THREAD | cycles | 总周期 |

- **采集方式**:
  ```bash
  perf stat -e armv8_pmuv3/bus_access/,armv8_pmuv3/stall_backend/,cycles \
      -e arm_spe/freq=1,min_latency=30/ -a -- <workload>
  ```

#### 6.3 AOL 验证
- **方法**: 用 microbenchmark 制造已知 MLP 场景
  - 低 MLP: pointer-chasing（串行依赖访问）
  - 高 MLP: 顺序大块读取（SVE 向量 load）
- **预期**: 低 MLP 场景 AOL 高（数据应放快层），高 MLP 场景 AOL 低（可放慢层）
- **验收**: AOL 指标能正确区分两种场景

### Week 7: 对象评分算法 ARM 化

#### 7.1 proc_obj_e.py 重写
- **原版依赖**: PEBS tracepoint 输出 + perf PEBS 事件
- **重写为**: SPE 解析 + ARM PMU 事件
- **核心改动**:
  ```python
  def read_spe_file(directory):
      """替代原版的 read_file()，解析 SPE 数据"""
      # 读取 perf script 输出
      # 提取: timestamp, va, latency, data_source, pc
      # 按 va 匹配到对象（通过 LD_PRELOAD 的分配记录）
      # 计算每个对象的:
      #   - avg_latency (平均访问延迟)
      #   - remote_ratio (远程访问比例)
      #   - mlp_contribution (MLP 贡献估算)
      #   - aol_score = avg_latency / mlp_contribution
  ```

#### 7.2 对象评分公式
- **原版**: `score = access_ratio × predicted_slowdown × K(aol)`
- **ARM 版**: `score = access_count × aol × remote_impact_ratio`
  ```python
  def compute_object_score(obj, spe_data, pmu_stats):
      # 1. 从 SPE 数据统计该对象的访问模式
      avg_lat = spe_data[obj].avg_latency
      remote_ratio = spe_data[obj].remote_access_count / spe_data[obj].total_count
      
      # 2. 从 PMU 统计估算 MLP
      mlp = pmu_stats.bus_access / pmu_stats.bus_cycles
      
      # 3. 计算 AOL
      aol = avg_lat / max(mlp, 1.0)
      
      # 4. 评分 = 访问量 × AOL × 远程影响
      score = spe_data[obj].total_count * aol * remote_ratio
      
      # 5. SVE 加权（如果有 SVE 访问，降低 MLP 评分）
      if spe_data[obj].has_sve_access:
          sve_width = spe_data[obj].max_sve_width  # 128/256/512
          score = score / (sve_width / 128.0)  # SVE 越宽，MLP 越高，评分越低
      
      return score
  ```

### Week 8: 集成测试 + 端到端 Pipeline 验证

#### 8.1 完整 Pipeline 端到端测试
```
Step 1: LD_PRELOAD=libmemprof.so ./matrix_multiply
        → 输出: data.raw.* (分配记录)

Step 2: perf record -e arm_spe/freq=1,min_latency=30/ -a -- ./matrix_multiply
        → 输出: perf.data (SPE 采样)

Step 3: perf script > spe_output.txt
        → 输出: SPE 记录文本

Step 4: python3 proc_obj_e.py . --spe spe_output.txt
        → 输出: obj_stat.csv (对象评分)

Step 5: 用评分结果生成 tiered_alloc.conf
        → 高分对象 → fast tier (Node 0/1)
        → 低分对象 → slow tier (Node 2/3)

Step 6: LD_PRELOAD=libmeminterc.so TIERED_ALLOC_CONF=./tiered_alloc.conf ./matrix_multiply
        → 验证性能提升
```

#### 8.2 AOL 精度评估
- **方法**: 人工标注几个已知"性能关键"和"非关键"的对象
- **验收**: 评分排名与人工标注的吻合度 > 80%

#### Phase 2 交付物
- [ ] SPE 数据采集 pipeline
- [ ] SPE 记录解析器
- [ ] AOL 重建实现（方案 A）
- [ ] ARM PMU 事件映射表
- [ ] 对象评分算法（含 SVE 感知）
- [ ] obj_stat.csv 输出验证
- [ ] AOL 精度评估报告
- [ ] ⚠️ Go/No-Go 决策点

---

## Phase 3: 独立分配器 + GROMACS 验证 (4周)

> 目标：交付 libtiered_alloc.so 并用 GROMACS 验证性能提升

### Week 9: libtiered_alloc.so 核心实现

#### 9.1 统一库架构
- **整合**: prof + interc → 单一 `libtiered_alloc.so`
- **接口设计**:
  ```c
  // libtiered_alloc.h
  typedef enum {
      TIERED_MODE_PROFILING,    // 采集模式
      TIERED_MODE_ALLOCATION,   // 分配模式
      TIERED_MODE_AUTO          // 自动模式（先采集后分配）
  } tiered_mode_t;

  int tiered_init(tiered_mode_t mode, const char *conf_path);
  void *tiered_malloc(size_t size);
  void tiered_free(void *ptr);
  void *tiered_calloc(size_t nmemb, size_t size);
  void *tiered_realloc(void *ptr, size_t size);
  int tiered_rebalance(void);  // 运行时重新平衡（基于动态采样）
  void tiered_stats(FILE *out); // 输出统计信息
  void tiered_fini(void);
  ```

#### 9.2 内部架构
  ```
  libtiered_alloc.so
  ├── malloc/free/calloc/realloc 拦截 (LD_PRELOAD)
  ├── 对象识别器 (函数名匹配)
  ├── 评分引擎 (AOL + SVE 感知)
  ├── NUMA 分配器 (numa_alloc_onnode / mbind)
  ├── Profiler (分配记录 + SPE 采样)
  └── 配置管理器 (conf 文件解析)
  ```

#### 9.3 运行时动态采样（ALTO 思想用户态化）
- **不修改内核，在用户态实现 ALTO 的核心思想**
- **方法**: 定期（每 N 秒）用 SPE 采样当前内存访问模式
- **动态调整**: 如果某对象的 AOL 从低变高，触发迁移建议
  ```c
  // 简化版 ALTO 用户态
  void tiered_periodic_check() {
      for (obj in monitored_objects) {
          current_aol = sample_aol(obj);  // SPE 采样
          if (current_aol > threshold_high && obj->tier != FAST) {
              migrate_object(obj, FAST_TIER);  // 建议迁移
          } else if (current_aol < threshold_low && obj->tier != SLOW) {
              migrate_object(obj, SLOW_TIER);  // 建议迁移
          }
      }
  }
  ```

### Week 10: GROMACS 对象分析 + Profiling 规则

#### 10.1 GROMACS 内存分配模式分析
- **方法**: 在 GROMACS 源码中标注关键数据结构
  ```c
  // GROMACS 核心数据结构（需要分析的对象）
  t_forcerec   → force computation, 频繁访问, 应在 fast tier
  t_state      → coordinates + velocities, 高频更新, 应在 fast tier
  t_idef       → topology definition, 读取为主, 可放 slow tier
  t_mdatoms    → atom properties, 中等频率, 视 AOL 决定
  t_graph      → neighbor graph, 访问模式复杂, 需要 profiling
  t_commrec    → MPI communication buffers, 突发性, 可放 slow tier
  ```

#### 10.2 GROMACS Profiling 规则生成
- 用 Phase 2 的 pipeline 对 GROMACS 做 profiling
- 生成 GROMACS 专用的 `tiered_alloc.conf`
- **关键**: 识别 force computation 阶段 vs I/O 阶段的不同访问模式

#### 10.3 GROMACS 编译适配
- 确保 GROMACS 在鲲鹏930上编译运行（可能已有，PTO-Gromacs 项目）
- 确认 `LD_PRELOAD` 与 GROMACS 的兼容性

### Week 11-12: GROMACS 端到端性能测试

#### 11.1 测试矩阵
  ```
  | 配置 | CPU 绑定 | 内存绑定 | 说明 |
  |------|---------|---------|------|
  | Baseline | Node 0 | All nodes (默认) | Linux 默认 NUMA 策略 |
  | Fast-only | Node 0 | Node 0 | 全在快层（上限） |
  | Slow-only | Node 0 | Node 2 | 全在慢层（下限） |
  | SOAR | Node 0 | 按 obj_stat.csv 分配 | 我们的方法 |
  | SOAR+ALTO | Node 0 | SOAR + 动态调整 | 完整方案 |
  ```

#### 11.2 性能指标
- **主指标**: ns/day（GROMACS 标准性能指标）
- **辅助指标**:
  - L3 cache miss rate
  - 远程 NUMA 访问次数/比例
  - 内存带宽利用率
  - wall-clock time

#### 11.3 测试负载
- 使用 PTO-Gromacs 已有的 benchmark .tpr 文件
- 不同规模：small (2.5K atoms), medium (21K atoms), large (98K atoms)
- 每个配置跑 3 次取平均

#### 11.4 性能报告
- 生成对比图表：Baseline vs SOAR vs SOAR+ALTO
- 分析：哪些对象被正确放到了 fast tier
- 分析：SVE 感知评分是否有效降低了不必要的数据迁移

#### Phase 3 交付物
- [ ] libtiered_alloc.so 完整实现
- [ ] GROMACS profiling 规则
- [ ] GROMACS 性能对比报告（ns/day）
- [ ] SVE 感知评分效果分析
- [ ] 论文级性能图表

---

## Phase 4: 内核 ALTO Patch（可选，8+周）

> ⚠️ 依赖 Phase 2 的 Go 决策。如果 AOL 重建精度不足，此 Phase 无意义。
> 建议：Phase 1-3 完成后，根据论文需要决定是否执行。

### Week 13-16: NBT Patch 移植到 Linux 6.6

#### 13.1 基于 NBT 的 ALTO 移植（最简路径）
- **选择 NBT 的原因**: patch 最小（277行），且 Linux 6.6 已内置 NUMA balancing tiering
- **核心改动**:
  1. `should_numa_migrate_memory()` 加比例控制（ALTO 核心）
  2. sysctl 接口注册（6.6 用 `register_sysctl()` 替代 `kernel/sysctl.c`）
  3. 4 node 拓扑适配（替换 2 node 硬编码）

#### 13.2 openEuler 兼容性验证
- openEuler 24.03 的 NUMA tiering 框架可能与 upstream 不同
- 需要确认：`numa_promotion_tiered_enabled` 在 openEuler 6.6 中的实现

#### 13.3 内核编译和测试
- 在鲲鹏930上编译自定义内核
- 用 GROMACS 跑完整对比（Baseline / SOAR / ALTO / SOAR+ALTO）

### Week 17-20: 完整系统验证 + 论文数据

#### 17.1 多 benchmark 测试
- 除了 GROMACS，增加其他 HPC benchmark：
  - Graph500（图计算，不同访问模式）
  - STREAM（内存带宽测试）
  - SPEC CPU 2017（如果有 license）

#### 17.2 与论文原版数据对比
- 对比我们的 ARM 结果与原论文的 x86 结果
- 分析：ARM SPE 的 AOL 是否比 Intel PMU 的 AOL 更精确

#### Phase 4 交付物
- [ ] ALTO 内核 patch for Linux 6.6 (openEuler)
- [ ] 完整系统性能报告
- [ ] 论文级对比数据

---

## 项目文件结构

```
ARM-Tiered-Memory/
├── PLAN.md                    # 本文件
├── README.md
├── docs/
│   ├── architecture.md        # 架构设计文档
│   ├── aol-arm-design.md      # AOL ARM 重建设计
│   ├── spe-pipeline.md        # SPE 数据 pipeline 设计
│   └── gromacs-rules.md       # GROMACS 分配规则
├── src/
│   ├── lib/
│   │   ├── tiered_alloc.h     # 公共接口
│   │   ├── tiered_alloc.c     # 核心实现
│   │   ├── object_identify.c  # 对象识别（函数名匹配）
│   │   ├── scorer.c           # 评分引擎（AOL + SVE）
│   │   ├── numa_placement.c   # NUMA 分配（4 node 感知）
│   │   ├── profiler.c         # 分配记录采集
│   │   ├── spe_collector.c    # SPE 采样采集
│   │   ├── config.c           # 配置文件解析
│   │   └── Makefile
│   ├── tools/
│   │   ├── spe_parser.py      # SPE 记录解析器
│   │   ├── obj_analyzer.py    # 对象分析（替代 proc_obj_e.py）
│   │   ├── latency_bench.c    # 延迟基准测试
│   │   └── generate_conf.py   # 从 obj_stat.csv 生成配置
│   ├── microbenchmark/
│   │   ├── pointer_chase.c    # 串行访问（低 MLP）
│   │   ├── stream_access.c    # 顺序访问（高 MLP）
│   │   ├── sve_stream.c       # SVE 向量访问
│   │   └── Makefile
│   └── patches/
│       ├── alto-nbt-6.6.patch # ALTO 内核 patch（Phase 4）
│       └── nbt-6.6.patch      # NBT 基础 patch（Phase 4）
├── configs/
│   ├── tiered_alloc.conf.example
│   ├── gromacs_tiered.conf
│   └── kunpeng_numa.conf
├── results/
│   ├── phase1/
│   ├── phase2/
│   ├── phase3/
│   └── phase4/
└── scripts/
    ├── setup_kunpeng.sh       # 鲲鹏930 环境初始化
    ├── run_profiling.sh       # 一键 profiling
    ├── run_gromacs_test.sh    # GROMACS 测试脚本
    └── analyze_results.py     # 结果分析
```

---

## 关键里程碑和决策点

| 时间 | 里程碑 | 决策 |
|------|--------|------|
| Week 4 | Phase 1 完成 | SPE 是否可用？profiling pipeline 是否能输出评分？ |
| **Week 8** | **Phase 2 完成** | **⚠️ Go/No-Go: AOL 重建精度是否满足要求？** |
| Week 12 | Phase 3 完成 | GROMACS 是否有可测量的性能提升？ |
| Week 20 | Phase 4 完成（可选） | 是否需要内核 patch？论文需要完整系统数据吗？ |

### Go/No-Go 标准（Phase 2 结束时）
- ✅ **Go**: AOL 能区分高 MLP 和低 MLP 场景（评分差异 > 2x）
- ✅ **Go**: 对象评分与人工标注吻合度 > 80%
- ❌ **No-Go**: AOL 在 ARM 上无法可靠重建，评分随机
- ❌ **No-Go**: SPE 数据量过大导致采集不可行

---

## 风险管理

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| SPE MLP 估算不精确 | 中 | 高 | 先用方案 A 验证，不行上方案 B/C |
| SPE 数据量爆炸 | 中 | 中 | 精调 min_latency，动态调整采样率 |
| 内核 patch 移植失败 | 高 | 低 | Phase 4 可选，用户态已覆盖核心创新 |
| GROMACS malloc 模式复杂 | 中 | 中 | 先做 GROMACS 内存分析（Week 10） |
| openEuler 内核与 upstream 不兼容 | 中 | 中 | Phase 4 前做兼容性验证 |
| 鲲鹏930 访问受限（共享机器） | 低 | 高 | 提前预约测试时间段 |

---

## 参考资料

- 论文: OSDI'25 "Tiered Memory Management Beyond Hotness"
- 代码: https://github.com/MoatLab/SoarAlto
- ARM SPE: https://developer.arm.com/documentation/109649/latest
- Intel UMF: https://github.com/oneapi-src/unified-memory-framework
- GROMACS: https://gitlab.com/gromacs/gromacs
- Linux Memory Tiering: `mm/memory-tiers.c` (Linux 6.6+)
