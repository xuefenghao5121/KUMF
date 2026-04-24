# KUMF 项目记忆

> 项目级别的决策、发现、教训记录。每次会话开始时读取。

---

## 项目基本信息

- **名称**: KUMF (Kunpeng Unified Memory Framework)
- **GitHub**: https://github.com/xuefenghao5121/KUMF
- **本地路径**: `/home/huawei/Desktop/home/xuefenghao/workspace/KUMF/`
- **灵感来源**: OSDI'25 "Tiered Memory Management Beyond Hotness"
- **原版代码**: `/home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto/`
- **目标平台**: 鲲鹏930, 120核 ARM64, 4 NUMA node, Linux 6.6.0
- **SSH**: `ssh kunpeng`

---

## 鲲鹏930 环境快照 (2026-04-24)

```
NUMA: 4 node × (40核 + ~64GB)
距离: 同 socket 12, 跨 socket 35-40
SPE: arm_spe_0 (type=135) ✅ 可用
SVE: ✅ 支持 (sve, svebf16, svef32mm, svef64mm)
内核: 6.6.0 (openEuler 24.03 LTS-SP3)
工具: libnuma 2.0.16, perf 6.6.0, GCC 12.3.1, cmake 3.27.9
注意: numactl-devel 未安装（缺 numa.h），需要 sudo 安装
```

---

## 原版 SoarAlto 分析要点

### 核心架构
- SOAR: LD_PRELOAD 拦截 malloc/free → backtrace 获取调用链 → check_trace 匹配 → numa_alloc_onnode 分配
- ALTO: 内核 patch 在 should_numa_migrate_memory() 加比例控制
- AOL = Offcore Latency / MLP，用 Intel PMU 事件计算

### 必须改的（ARM 不兼容）
1. rdtsc → clock_gettime（ARM 没有 rdtsc）
2. rbp → x29（ARM64 帧指针）
3. syscall(186) → syscall(__NR_gettid)（ARM64 gettid=178）
4. check_trace 硬编码 x86 地址 → 函数名匹配
5. ARR_SIZE=950M → 缩减（950M×80B=76GB/线程）
6. 2 node → 4 node 拓扑
7. immintrin.h → 移除
8. PEBS → SPE 数据 pipeline

### 原版可复用的（~20%）
- LD_PRELOAD 拦截框架
- 分配/释放记录 + addr_seg 管理
- 对象评分和排名的思路（AOL × 访问频率 × 远程影响）
- 三阶段 pipeline: profiling → analysis → allocation

---

## 柱子哥审查结论 (2026-04-24)

### 整体评分
- 创新性 ⭐⭐⭐⭐ | 可行性 ⭐⭐⭐ | 价值 ⭐⭐⭐⭐

### 四个严重问题
1. check_trace 硬编码 x86 地址，不可移植
2. AOL 重建方案是空白（核心创新点）
3. 内核 patch 移植难度低估
4. 4 NUMA node vs 原版 2 node

### 关键建议
- 先纯用户态，内核是锦上添花
- 用 GROMACS/hnswlib 做 benchmark，不用 GAPBS
- AOL on ARM 要作为独立贡献点
- SPE data_source 比 PEBS 更强
- 最大风险是 AOL 重建精度

### 柱子哥原话
> "这个项目做成了，是有 OSDI/ASPLOS 级别贡献潜力的。不是移植，是重新实现+创新。"

---

## 验证 Workload

### 1. hnswlib（向量数据库）
- 开源库: https://github.com/nmslib/hnswlib
- 冷热分离: HNSW 图节点=热，原始向量=冷
- 场景: AI 基础设施，RAG/推荐/搜索
- 测试脚本: workloads/hnswlib_numa_benchmark.sh
- 状态: 脚本已就绪，待运行

### 2. mini_md（简化 MD 模拟）
- 文件: workloads/mini_md.c
- 冷热分离: coordinates/forces/velocities=热，masses/topology/bonds=冷
- 场景: HPC/超算
- 测试脚本: workloads/run_mini_md_test.sh
- 状态: 脚本已就绪，待运行

---

## 技术决策记录

| # | 日期 | 决策 | 理由 |
|---|------|------|------|
| D1 | 04-24 | 项目定名 KUMF | Kunpeng Unified Memory Framework |
| D2 | 04-24 | 先纯用户态 | 内核 patch 风险高，用户态覆盖 80% 创新 |
| D3 | 04-24 | 用开源 hnswlib 验证 | 第三方成熟库，不自己写 |
| D4 | 04-24 | AOL 方案 A 起步 | 快速验证，降低风险 |
| D5 | 04-24 | Phase 2 是 go/no-go 点 | AOL 精度决定项目成败 |
| D6 | 04-24 | 柱子哥模型改为 glm-5.1 | 用户要求 |

---

## 待解决问题

- [ ] numactl-devel 安装（需要 sudo 密码）
- [ ] 鲲鹏930 上 SPE 的 min_latency 最优参数
- [ ] openEuler 6.6 与 upstream 6.6 的差异点
- [ ] ARM SPE 是否支持 data_source 字段（需要实测确认）
- [ ] 鲲鹏930 的 L3 cache 延迟实测值（替换 SKX 常数 24.87）
