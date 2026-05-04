# KUMF - Kunpeng Unified Memory Framework

基于 OSDI'25 论文《Tiered Memory Management Beyond Hotness》的 **ARM 原生 tiered memory 分配器**。

不是移植，是重新实现 + 创新。

## 核心创新

1. **PAC 评分的 ARM 原生实现** — 用 ARM SPE LAT TOT 字段直接计算 page 级 Performance Awareness Cost，无需移植 x86 AOL 公式
2. **SVE 感知评分** — SVE 向量 load 天然高 MLP，PAC 自动调整，避免误判
3. **ARM SPE 精确采样** — LAT ISSUE/TOT/XLAT + data_source，信息量远超 Intel PEBS
4. **多场景验证** — HPC（GROMACS/mini_md）+ 通用计算（hnswlib 向量数据库）

## 项目结构

```
KUMF/
├── Makefile                     # 统一编译入口
├── tools/                       # Python 分析工具
│   ├── spe_page_pac.py          # ⭐ SPE LAT → page PAC 评分
│   ├── pac_to_interc.py         # ⭐ PAC → interc 配置
│   ├── soar_spe_report.py       # SPE data_source 解析
│   ├── soar_analyzer.py         # SOAR 对象分级
│   └── soar_pipeline.py         # 端到端 pipeline
├── src/
│   ├── lib/
│   │   ├── interc/              # NUMA 路由 LD_PRELOAD
│   │   ├── prof/                # 分配追踪 LD_PRELOAD
│   │   └── mlock/               # 热页锁定 LD_PRELOAD
│   └── tools/
│       ├── spe_preload.c        # SPE 自采集（不需要 perf 工具）
│       └── spe_self_profile.c   # SPE 自 profiling wrapper
├── workloads/
│   ├── kumf_tiered.c            # 冷热分离 benchmark
│   ├── kumf_stream.c            # NUMA 带宽测试
│   ├── mini_md_v2.c             # OpenMP MD benchmark
│   └── kumf_tiered.conf         # size-based interc 配置示例
└── README.md
```

## 编译

```bash
make                # 编译全部
make libs           # LD_PRELOAD 库
make workloads      # 测试程序
make tools          # SPE 工具
make clean
```

编译产物统一输出到 `build/`：
- `build/libkumf_interc.so` — NUMA 路由
- `build/libkumf_prof.so` — 分配追踪
- `build/libkumf_mlock.so` — 热页锁定
- `build/kumf_tiered` / `kumf_stream` / `mini_md_v2` — 测试程序
- `build/spe_self_profile` / `spe_preload.so` — SPE 工具

### 环境要求

- 鲲鹏930 / ARM64 + ARM SPE 支持
- openEuler 24.03+ / Linux 6.6+
- GCC 12+, libnuma-devel, perf

## 使用

### 安装 CLI

```bash
make install   # 安装 kumf 到 ~/.local/bin
```

### 一键诊断

```bash
# 一条命令完成：采样→分析→报告+配置
kumf diagnose -- ./build/kumf_tiered 200 100 50

# 自定义参数
kumf diagnose --min-lat 64 -o /tmp/my_test -- ./build/kumf_tiered 200 100 50
```

### 用配置运行

```bash
# 用 diagnose 生成的配置运行
kumf run --conf /tmp/kumf/kumf.conf -- ./build/kumf_tiered 200 100 50

# 开启调试输出
kumf run --conf /tmp/kumf/kumf.conf --debug -- ./build/kumf_tiered 200 100 50
```

### 性能对比

```bash
# 自动跑四组对比：全快/全慢/first-touch/分层
kumf bench --conf /tmp/kumf/kumf.conf -- ./build/kumf_tiered 200 100 50
```

### 只分析（不采样）

```bash
# 对已有的 perf.data 做分析
kumf analyze --perf-data perf_filtered.perf -o /tmp/kumf
```

### 手动分步流程（高级）

```bash
# 1. 采集 SPE + prof（同时运行，地址对得上）
perf record -e arm_spe/load_filter=1,store_filter=1,min_latency=32/ \
  -o /tmp/kumf/spe.perf \
  -- env LD_PRELOAD=build/libkumf_prof.so \
  ./build/kumf_tiered 200 100 50

# 2. 解析 SPE → page PAC 评分
perf report -D -i /tmp/kumf/spe.perf | python3 tools/spe_page_pac.py -o /tmp/kumf

# 3. PAC + prof 交叉关联 → interc 配置
python3 tools/pac_to_interc.py \
  --pac-csv /tmp/kumf/page_pac.csv \
  --prof-log /tmp/kumf/data.raw.1 \
  --mode alloc -o /tmp/kumf/kumf_pac.conf

# 4. 用 PAC 配置运行
KUMF_CONF=/tmp/kumf/kumf_pac.conf LD_PRELOAD=build/libkumf_interc.so \
  ./build/kumf_tiered 200 100 50
```

### 快速诊断（不需要 prof）

```bash
# 只采集 SPE，生成 PAC 报告和 size-based 配置
perf record -e arm_spe/load_filter=1,store_filter=1,min_latency=32/ \
  -o /tmp/kumf/spe.perf -- ./build/kumf_tiered 200 100 50

perf report -D -i /tmp/kumf/spe.perf | python3 tools/spe_page_pac.py -o /tmp/kumf

python3 tools/pac_to_interc.py --pac-csv /tmp/kumf/page_pac.csv --mode size \
  -o /tmp/kumf/kumf.conf
```

### 性能对比

```bash
# 全快层
numactl --cpunodebind=0 --membind=0 ./build/kumf_tiered 200 100 50

# 全慢层
numactl --cpunodebind=0 --membind=2 ./build/kumf_tiered 200 100 50

# SOAR 分层
KUMF_CONF=/tmp/kumf/kumf.conf LD_PRELOAD=build/libkumf_interc.so \
  ./build/kumf_tiered 200 100 50
```

## PAC 评分原理

```
PAC(page) = access_count × avg_LAT_TOT / effective_MLP
```

- **高 PAC** → 访问多 + 延迟大 → 放快层
- **低 PAC** → 访问少 或 MLP 高（SVE）→ 放慢层
- **SVE 感知**：SVE 向量 load 天然高 MLP，PAC 自动下调

### ARM SPE LAT 字段

| Packet | 含义 | 用途 |
|--------|------|------|
| LAT ISSUE | 指令发射延迟 | 流水线分析 |
| **LAT TOT** | **操作总延迟** | **PAC 评分核心** |
| LAT XLAT | TLB 翻译延迟 | TLB 抖动分析 |
| LAT (0x9e) | LLC 相关延迟 | Cache 分析 |

### 冷热自动判定

按累计访问量自动划分三级：
- **HOT**（累计 80% 访问量的 pages）→ 快层
- **WARM**（累计 80%-95%）→ 快层（有空位时）
- **COLD**（剩余 5%）→ 慢层

## 已验证结果

| 测试 | 耗时 | 说明 |
|------|------|------|
| 全快层 (Node 0) | 0.608s | 性能上限 |
| 全慢层 (Node 2) | 1.407s | 性能下限 |
| **SOAR 自动** | **0.588s** | **比全快层快 3.3%** |
| Gap closing | 67.8% | — |

### SPE LAT TOT 分布（min_latency=32，kumf_tiered）

```
     32-50 (L3 hit):      0.0%
    50-100 (L3/cluster):  0.2%
   100-200 (local DRAM): 47.6% ███████████████████████
   200-350 (remote DRAM):52.3% ██████████████████████████
       >350 (far remote): 0.0%
```

## 目标平台

鲲鹏930, 120核 ARM64, 4 NUMA node, SVE 支持
