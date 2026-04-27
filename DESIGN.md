# KUMF 架构设计

> 最后更新: 2026-04-27

## 整体架构

```
┌─────────────────────────────────────────────────────┐
│                    Workload                          │
│          (kumf_tiered / GROMACS / ...)              │
└────────┬────────────────────────────┬───────────────┘
         │ LD_PRELOAD                 │ perf record
         ▼                            ▼
┌─────────────────┐         ┌─────────────────────┐
│   interc/ldlib  │         │   ARM SPE 采集       │
│  NUMA 路由分配   │         │  arm_spe// 格式      │
│  size/addr 规则  │         └──────────┬──────────┘
└────────┬────────┘                    │
         │                             ▼
         │              ┌──────────────────────────┐
         │              │  soar_spe_report.py      │
         │              │  SPE → Page-level 分级    │
         │              │  → interc 配置文件        │
         │              └──────────┬───────────────┘
         │                         │
         │    ┌────────────────────┘
         ▼    ▼
┌──────────────────────┐
│  KUMF 配置文件        │
│  size_gt:xxx = node  │
│  size_range:xxx = node│
│  0xADDR-0xADDR = node│
└──────────────────────┘
```

## 核心模块

### 1. prof/ldlib.c — 分配追踪

LD_PRELOAD 拦截 malloc/calloc/free，记录：
- 分配地址、大小、调用者 PC
- 输出到 /tmp/kumf/data.raw.PID

### 2. interc/ldlib.c — NUMA 路由

LD_PRELOAD 拦截大块 malloc（>4KB），按规则路由到指定 NUMA node：
- **size-based**: `size_gt:BYTES = NODE`, `size_range:MIN-MAX = NODE`
- **addr-based**: `0xSTART-0xEND = NODE`（调用者 PC 地址范围）
- **name-based**: `function_pattern = NODE`

### 3. soar_spe_report.py — SPE 分析

解析 `perf report --mem-mode --stdio` 输出：
1. 提取每个符号的数据地址和 cache 命中统计
2. 计算每个 page 的热度和延迟特征
3. 生成 interc 配置文件

### 4. kumf_tiered.c — 验证 Workload

模拟 HPC 冷热分离场景：
- 热数据 200MB（coordinates + forces）— 随机读写
- 冷数据 100MB（masses）— 顺序只读
- 报告 NUMA 分配位置 + 带宽 + 耗时
