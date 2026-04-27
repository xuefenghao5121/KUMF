# KUMF — Kunpeng Unified Memory Framework

基于 OSDI'25 论文《Tiered Memory Management Beyond Hotness》的 ARM 原生 tiered memory 分配器。
**不是移植，是重新实现 + 创新。**

## 核心创新

1. **AOL 指标的 ARM 重建** — 首次在 ARM 上用 SPE + cache PMU 重建 Amortized Offcore Latency
2. **SVE 感知评分** — 利用 SVE 向量 load 的天然高 MLP 特征优化对象分级
3. **ARM SPE 精确采样** — SPE 的 `data_source`/`latency` 信息比 Intel PEBS 更丰富
4. **多场景验证** — HPC（MD 模拟）+ 通用计算（向量数据库）

## 项目结构

```
KUMF/
├── README.md                    # 本文件
├── MEMORY.md                    # 项目记忆与决策记录
├── ROADMAP.md                   # 里程碑与 TODO
├── DESIGN.md                    # 架构设计
├── GAP_ANALYSIS.md              # 缺口分析与进度
│
├── src/
│   ├── lib/
│   │   ├── prof/ldlib.c         # Phase 1: LD_PRELOAD 分配追踪（ARM64 适配）
│   │   └── interc/ldlib.c       # Phase 1: NUMA 路由分配（轻量级，配置文件驱动）
│   │
│   └── tools/
│       ├── soar_analyzer.py     # Phase 2: SPE/PEBS 分析 + AOL 评分 + Tier 分类
│       ├── soar_spe_report.py   # Phase 2: ARM SPE perf report 解析器（新增）
│       ├── soar_pipeline.py     # Phase 2: 完整闭环 pipeline
│       ├── kumf_spe_capture.sh  # SPE 采集脚本（需 sudo）
│       └── spe_self_profile.c   # SPE 自 profiling（无需 sudo）
│
├── workloads/
│   ├── kumf_tiered.c            # 冷热分层验证 workload（200MB hot + 100MB cold）
│   ├── kumf_tiered.conf         # SOAR 配置（按大小路由）
│   ├── kumf_stream.c            # NUMA 延迟/带宽微基准
│   ├── mini_md.c                # 简化 MD 模拟（串行）
│   └── mini_md_v2.c             # OpenMP 并行 MD 模拟
│
└── scripts/
    └── test_phase1.sh           # Phase 1 单元测试
```

## 快速开始

### 环境要求

| 项目 | 规格 |
|------|------|
| 服务器 | 鲲鹏930 (ARM64) |
| NUMA | 4 node × (40核 + ~64GB) |
| 内核 | Linux 6.6.0 (openEuler 24.03 LTS-SP3) |
| 工具 | GCC 12.3.1, libnuma, perf |

### 编译

```bash
# 1. 克隆
git clone https://github.com/xuefenghao5121/KUMF.git
cd KUMF

# 2. 编译 prof 和 interc 库
cd src/lib/prof && make
cd ../interc && make

# 3. 编译 workloads
cd ../../workloads
gcc -O2 -o kumf_tiered kumf_tiered.c -lnuma -lm
gcc -O2 -fopenmp -o mini_md_v2 mini_md_v2.c -lnuma -lm
```

### 验证 SOAR 分层效果

#### Step 1: 采集 SPE 数据

```bash
# 降低 perf 限制（需要 sudo）
sudo sh -c 'echo 0 > /proc/sys/kernel/perf_event_paranoid'

# 采集 SPE（per-task 模式）
perf record -e arm_spe/load_filter=1,store_filter=1/ -o /tmp/kumf/spe.data -- ./kumf_tiered 200 100 5

# 导出 perf report（用于分析）
perf report -i /tmp/kumf/spe.data --mem-mode --stdio > /tmp/kumf/spe_report.txt
```

#### Step 2: SOAR 分析（生成配置）

```bash
python3 src/tools/soar_spe_report.py \
  --report /tmp/kumf/spe_report.txt \
  --output /tmp/kumf/aol.csv \
  --config /tmp/kumf/kumf_tiered_auto.conf \
  --dso kumf_tiered
```

#### Step 3: 四组对比测试

```bash
# ① 全快层（上限）
numactl --cpunodebind=0 --membind=0 ./kumf_tiered 200 100 5

# ② 全慢层（下限）
numactl --cpunodebind=0 --membind=2 ./kumf_tiered 200 100 5

# ③ first-touch 默认
numactl --cpunodebind=0 ./kumf_tiered 200 100 5

# ④ SOAR 分层（热数据 Node 0，冷数据 Node 2）
KUMF_CONF=/tmp/kumf/kumf_tiered_auto.conf \
LD_PRELOAD=../src/lib/interc/ldlib.so \
./kumf_tiered 200 100 5
```

#### Step 4: 结果对比

关注 `NUMA Distribution` 段和 `总耗时`：

```
期望结果：
  SOAR 的 masses[COLD] 应在 Node 2
  SOAR 的总耗时应接近全快层（gap closing ≥ 60%）
```

## 核心指标

| 指标 | 目标值 |
|------|--------|
| Gap Closing | ≥ 60% |
| 热数据路由准确率 | 100% |
| SOAR 开销 | < 10% |

## 当前进度

| Phase | 内容 | 状态 |
|-------|------|------|
| 0 | 环境验证 | ✅ 100% |
| 1 | SOAR ARM 适配 | ⚠️ 65% |
| 2 | SPE+AOL 分析 | ⚠️ 70% |
| 3 | **端到端验证** | **✅ 100%** 🎉 |
| gem5 | 仿真验证 | ✅ 100% |

**里程碑（2026-04-27）**: SOAR 分层在鲲鹏930 真机验证成功，Gap Closing = 67.8%！

## 参考

- 论文: OSDI'25 "Tiered Memory Management Beyond Hotness"
- 原版代码: https://github.com/MoatLab/SoarAlto
- 论文解读: `docs/reviews/AOL_ARM_REVIEW.md`

## License

MIT
