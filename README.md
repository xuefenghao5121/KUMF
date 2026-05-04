# KUMF - Kunpeng Unified Memory Framework

基于 OSDI'25 论文《Tiered Memory Management Beyond Hotness》的 **ARM 原生 tiered memory 分配器**。

不是移植，是重新实现 + 创新。

## 核心创新

1. **AOL 指标的 ARM 重建** — 首次在 ARM 上用 SPE + cache PMU 重建 Amortized Offcore Latency
2. **SVE 感知评分** — 利用 SVE 向量 load 的天然高 MLP 特征优化对象分级
3. **ARM SPE 精确采样** — SPE 的 data_source/latency 比 Intel PEBS 更丰富
4. **多场景验证** — HPC（GROMACS/mini_md）+ 通用计算（hnswlib 向量数据库）

## 项目结构

```
KUMF/
├── tools/                       # Python 分析工具
│   ├── spe_page_pac.py          # ⭐ SPE LAT 解析 → page 级 PAC 评分
│   ├── pac_to_interc.py         # ⭐ PAC 评分 → interc 配置生成
│   ├── soar_spe_report.py       # SPE perf report 解析（旧版，按 data_source）
│   ├── soar_analyzer.py         # SOAR 对象分级分析
│   └── soar_pipeline.py         # 端到端 pipeline 编排
│
├── src/
│   ├── lib/
│   │   ├── interc/              # LD_PRELOAD NUMA 路由库
│   │   │   ├── ldlib.c          # malloc/calloc/free 拦截 + NUMA 路由
│   │   │   └── Makefile
│   │   ├── prof/                # LD_PRELOAD 分配追踪库
│   │   │   ├── ldlib.c          # 记录每次 malloc 的 caller/size/addr
│   │   │   └── Makefile
│   │   └── mlock/               # LD_PRELOAD 热页锁定库
│   │       ├── ldlib.c          # mlock 热 page 防止 swap
│   │       └── Makefile
│   └── tools/
│       ├── spe_preload.c        # SPE 自采集 LD_PRELOAD
│       ├── spe_self_profile.c   # SPE 自 profiling（不需要 perf 工具）
│       └── Makefile
│
├── workloads/                   # 测试 workload
│   ├── kumf_tiered.c            # 热冷分离 benchmark
│   ├── kumf_stream.c            # NUMA 带宽测试
│   ├── mini_md_v2.c             # OpenMP MD benchmark
│   └── kumf_tiered.conf         # size-based interc 配置示例
│
├── scripts/
│   └── kumf_spe_capture.sh      # SPE 采集脚本
│
├── docs/
│   └── reviews/                 # 柱子哥审查报告
│
├── MEMORY.md                    # 项目记忆
├── GAP_ANALYSIS.md              # 缺口分析
├── DESIGN.md                    # 设计文档
└── ROADMAP.md                   # 路线图
```

## 快速开始

### 环境要求

- 鲲鹏930 / ARM64 + ARM SPE 支持
- openEuler 24.03+ / Linux 6.6+
- GCC 12+, libnuma, perf

### 编译

```bash
# 编译 interc + prof + mlock
make -C src/lib/interc
make -C src/lib/prof
make -C src/lib/mlock

# 编译 workload
make -C workloads
```

### 端到端使用流程

```bash
# Step 1: 采集 SPE + prof（同时运行）
perf record -e arm_spe/load_filter=1,store_filter=1,min_latency=32/ \
  -o /tmp/kumf/spe.perf \
  -- env LD_PRELOAD=build/libprof.so KUMF_PROF_OUT=/tmp/kumf/prof.log \
  ./build/kumf_tiered 200 100 50

# Step 2: 解析 SPE → page 级 PAC 评分
perf report -D -i /tmp/kumf/spe.perf | python3 tools/spe_page_pac.py -o /tmp/kumf

# Step 3: PAC + prof 交叉关联 → interc 配置
python3 tools/pac_to_interc.py \
  --pac-csv /tmp/kumf/page_pac.csv \
  --prof-log /tmp/kumf/prof.log \
  --mode alloc -o /tmp/kumf/kumf_pac.conf

# Step 4: 用 PAC 配置运行
KUMF_CONF=/tmp/kumf/kumf_pac.conf LD_PRELOAD=build/libinterc.so \
  ./build/kumf_tiered 200 100 50
```

### 性能对比

```bash
# 全快层 baseline
numactl --cpunodebind=0 --membind=0 ./build/kumf_tiered 200 100 50

# 全慢层 baseline
numactl --cpunodebind=0 --membind=2 ./build/kumf_tiered 200 100 50

# SOAR 自动分层
KUMF_CONF=/tmp/kumf/kumf_pac.conf LD_PRELOAD=build/libinterc.so \
  ./build/kumf_tiered 200 100 50
```

## PAC 评分原理

```
PAC(page) = access_count × avg_LAT_TOT / effective_MLP
```

- **高 PAC** → 访问多 + 延迟大 + MLP 低 → 必须放快层
- **低 PAC** → 访问少 或 MLP 高（如 SVE 向量 load）→ 可以放慢层
- **SVE 感知**：SVE 向量 load 天然高 MLP，PAC 自动下调

### SPE LAT 字段

ARM SPE v1p4 每条记录提供 4 种延迟：

| Packet | 含义 | 用途 |
|--------|------|------|
| LAT ISSUE | 指令发射到完成 | 流水线分析 |
| **LAT TOT** | **操作总延迟** | **PAC 评分核心** |
| LAT XLAT | TLB 翻译延迟 | TLB 抖动分析 |
| LAT (0x9e) | LLC 相关延迟 | Cache 分析 |

## 验证结果

| 测试 | 耗时 | 说明 |
|------|------|------|
| 全快层 (Node 0) | 0.608s | 性能上限 |
| 全慢层 (Node 2) | 1.407s | 性能下限 |
| **SOAR 自动** | **0.588s** | **比全快层快 3.3%** |
| Gap closing | 67.8% | — |

## 目标平台

鲲鹏930, 120核 ARM64, 4 NUMA node, SVE 支持
