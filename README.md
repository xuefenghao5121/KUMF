# KUMF — Kunpeng Unified Memory Framework

> 基于 OSDI'25 论文《Tiered Memory Management Beyond Hotness》的 ARM 原生 tiered memory 分配器

## 项目定位

**不是移植，是重新实现 + 创新。**

在 ARM64（鲲鹏930）平台上实现 AOL 感知的 tiered memory 分配器，验证 SVE 感知评分和 ARM SPE 精确采样。

### 核心创新点

1. **AOL 指标的 ARM 重建** — 首次在 ARM 上用 SPE + cache PMU 重建 Amortized Offcore Latency
2. **SVE 感知评分** — 利用 SVE 向量 load 的天然高 MLP 特征，优化对象分级
3. **ARM SPE 精确采样** — SPE 的 data_source/latency 信息比 Intel PEBS 更丰富
4. **多场景验证** — HPC（GROMACS）+ 通用计算（向量数据库 hnswlib）

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

## 项目结构

```
KUMF/
├── README.md                # 本文件
├── ROADMAP.md               # 里程碑和 TODO
├── DESIGN.md                # 架构设计文档
├── MEMORY.md                # 项目记忆和决策记录
├── docs/
│   ├── PLAN.md              # 详细 Phase 计划
│   ├── tests/               # 测试方案
│   │   ├── PHASE1_TEST.md       # Phase 1 有效性测试（西西）
│   │   ├── TESTPLAN_XIXI.md     # Phase 1 详细测试（西西）
│   │   └── TESTPLAN_ZHUZIGE.md  # Phase 1 详细测试（柱子哥）
│   └── reviews/             # 架构审查
├── workloads/               # 验证用 workload
│   ├── mini_md.c                # 简化版 MD 模拟（HPC 场景）
│   ├── run_mini_md_test.sh      # MD 测试脚本
│   └── hnswlib_numa_benchmark.sh # 向量数据库测试脚本
├── src/                     # 源码（Phase 1 开始填充）
├── tests/                   # 自动化测试
├── scripts/                 # 工具脚本
└── results/                 # 实验结果
```

## 快速开始

```bash
# 克隆
git clone https://github.com/xuefenghao5121/KUMF.git

# 在鲲鹏930上运行向量数据库 NUMA 测试
bash workloads/hnswlib_numa_benchmark.sh

# 在鲲鹏930上运行 MD 模拟 NUMA 测试
bash workloads/run_mini_md_test.sh
```

## 路线图

| Phase | 内容 | 时间 | 状态 |
|-------|------|------|------|
| **0** | 环境验证 | 1天 | ✅ 完成 |
| **1** | SOAR ARM 适配 + 对象识别重写 | 4周 | 📋 规划中 |
| **2** | SPE 集成 + AOL 重建 | 4周 | ⏳ 待 Phase 1 |
| **3** | libtiered_alloc.so + 多场景验证 | 4周 | ⏳ 待 Phase 2 |
| **4** | 内核 ALTO patch（可选） | 8+周 | ⏳ 可选 |

详见 [ROADMAP.md](ROADMAP.md) 和 [docs/PLAN.md](docs/PLAN.md)

## 团队

| 角色 | 谁来执行 |
|------|---------|
| 项目负责人 | 鸡你太美 |
| 架构审查 | 柱子哥（OpenClaw subagent） |
| 项目管理/执行 | 西西（主 Agent） |

## 参考

- 论文: OSDI'25 "Tiered Memory Management Beyond Hotness"
- 代码: https://github.com/MoatLab/SoarAlto
- ARM SPE: https://developer.arm.com/documentation/109649/latest
- hnswlib: https://github.com/nmslib/hnswlib

## License

MIT
