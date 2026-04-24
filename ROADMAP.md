# KUMF Roadmap & TODO

> 最后更新: 2026-04-24

## 里程碑

### Phase 0: 环境验证 ✅ 完成 (2026-04-24)

- [x] NUMA topology 确认（4 node, 延迟差异 ~3x）
- [x] ARM SPE 确认可用（arm_spe_0, type=135）
- [x] SVE 特性确认
- [x] libnuma / perf / GCC 确认
- [x] SoarAlto 代码克隆 + 深入分析
- [x] 柱子哥架构审查完成

**关键发现**:
- SPE 支持但需要 min_latency 参数调优
- 4 NUMA node 不是原版的 2 node 假设
- openEuler 内核可能跟 upstream 6.6 有差异
- 原版代码可复用度约 20%

---

### Phase 1: SOAR ARM 适配 + 对象识别重写 📋 规划中

**目标**: 在鲲鹏930上跑通 SOAR profiling pipeline，输出 obj_stat.csv

#### Week 1: 用户态基础设施适配
- [ ] rdtsc → clock_gettime(CLOCK_MONOTONIC_RAW) — prof/ldlib.c, interc/ldlib.c
- [ ] rbp → x29 (ARM64 frame pointer) — 同上
- [ ] syscall(186) → syscall(__NR_gettid) — ARM64 gettid=178
- [ ] Makefile 条件编译 (ARCH := $(shell uname -m))
- [ ] immintrin.h 移除（microbenchmark）
- [ ] 编译验证 + LD_PRELOAD 基本功能测试

**测试方案**: [docs/tests/PHASE1_TEST.md](docs/tests/PHASE1_TEST.md)

#### Week 2: 对象识别重写 + 4 NUMA node
- [ ] check_trace() 重写：硬编码 x86 地址 → 函数名匹配 + 配置文件
- [ ] 配置文件格式设计 (tiered_alloc.conf)
- [ ] 4 NUMA node 拓扑支持（原版只有 0/1）
- [ ] addr_seg 管理 4 node 适配

#### Week 3: Profiling Buffer + SPE Smoke Test
- [ ] ARR_SIZE 缩减（950M → 合理值，如 1M-10M）
- [ ] profiling buffer 改用 mmap ring buffer 或降低 ARR_SIZE
- [ ] SPE smoke test（验证 arm_spe_0 输出格式）
- [ ] 鲲鹏930 L3 延迟基准测量

#### Week 4: 集成测试 + 端到端 Pipeline
- [ ] 完整 pipeline: prof → 分析 → interc 端到端
- [ ] obj_stat.csv 格式和内容验证
- [ ] 用 mini_md 做对象放置效果验证
- [ ] 120 核压力测试

**Go/No-Go**: Phase 1 完成后评估是否进入 Phase 2

---

### Phase 2: SPE 集成 + AOL 重建 ⏳ 待 Phase 1

**目标**: 实现 ARM 上的 AOL 指标计算
**⚠️ 这是项目的 go/no-go 点**

#### 核心任务
- [ ] SPE 数据采集 pipeline（替换 Intel PEBS）
- [ ] SPE 记录解析器（spe_parser.py）
- [ ] AOL 重建方案 A（SPE 延迟 + cache PMU 近似 MLP）
- [ ] ARM PMU 事件映射表
- [ ] 对象评分算法（含 SVE 感知）
- [ ] AOL 精度验证

**关键风险**: MLP 估算精度不足 → AOL 不准 → 分级失效

---

### Phase 3: libkumf.so + 多场景验证 ⏳ 待 Phase 2

**目标**: 交付独立分配器库并用真实 workload 验证

#### 核心任务
- [ ] libkumf.so 统一库（整合 prof + interc）
- [ ] 运行时动态采样（ALTO 思想用户态化）
- [ ] hnswlib 端到端性能测试（QPS + 延迟对比）
- [ ] mini_md 端到端性能测试（ns/day 对比）
- [ ] SVE 感知评分效果分析
- [ ] 性能报告 + 论文级图表

**验证 Workload**:
1. **hnswlib** — 向量数据库 ANN 搜索（通用计算）
2. **mini_md** — 简化版 MD 模拟（HPC）

---

### Phase 4: 内核 ALTO Patch（可选）⏳

- [ ] NBT patch 移植到 Linux 6.6 (openEuler)
- [ ] sysctl 接口适配（6.6 注册方式）
- [ ] 4 node 拓扑内核适配
- [ ] 完整系统性能对比

---

## 当前 TODO（优先级排序）

### 🔴 高优先级
1. 在鲲鹏930上跑 hnswlib NUMA benchmark → 获取 baseline 数据
2. 在鲲鹏930上跑 mini_md NUMA test → 获取 baseline 数据
3. 开始 Phase 1 Week 1 编码：修改 prof/ldlib.c 和 interc/ldlib.c

### 🟡 中优先级
4. 安装 numactl-devel（鲲鹏930 需要 sudo）
5. SPE 数据格式详细分析（确定解析策略）

### 🟢 低优先级
6. 调研 Intel UMF 是否可以作为分配器基础
7. 调研 openEuler 6.6 内核的 memory tiering 差异

---

## 决策记录

| 日期 | 决策 | 理由 |
|------|------|------|
| 2026-04-24 | 项目定名 KUMF | Kunpeng Unified Memory Framework |
| 2026-04-24 | 先纯用户态，内核 patch 可选 | 柱子哥建议：用户态覆盖 80% 创新点 |
| 2026-04-24 | 用 hnswlib + mini_md 双场景验证 | HPC + 通用计算，覆盖面广 |
| 2026-04-24 | AOL 重建用方案 A 起步 | 快速验证可行性，不行再上 B/C |
| 2026-04-24 | 不用 GROMACS 做初始验证 | 编译复杂，先用 mini_md 验证概念 |
| 2026-04-24 | 用开源 hnswlib 验证向量数据库场景 | 第三方成熟库，结果可信 |
