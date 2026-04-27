# KUMF AOL ARM 重建技术审查报告

> 审查人: 西西 | 日期: 2026-04-27 | 对照: 原版 SoarAlto proc_obj_e.py

---

## 一、原版 SoarAlto 的 AOL 计算（x86 Intel）

### 核心公式（原版第 574-576 行）

```python
# 4 个 Intel PMU 事件:
events = ["cycles",
          "CYCLE_ACTIVITY.STALLS_L3_MISS",           # L3 miss 导致的 stall 周期
          "OFFCORE_REQUESTS.DEMAND_DATA_RD",          # 发出的 offcore 读请求数
          "OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD"]  # 有 outstanding 请求的周期数

# 计算过程:
l3_stall_per_cyc = l3_stall / cyc                    # L3 miss stall 占总周期比例
a_lat = cyc_demand_data_rd / demand_data_rd           # ★ Amortized Offcore Latency (周期)
est_dram_sd = l3_stall_per_cyc / (24.67/a_lat + 0.87) # ★ DRAM stall density
```

### 对象评分公式（原版 rank_objs_r 函数，第 318-362 行）

```python
# 评分 = (对象访问占比 × DRAM stall density) / MLP_factor
obj_scores[obj] += (content[obj][0]/all_acc) * est_dram_sd[i] / factor

# MLP factor 基于 a_lat (amortized latency) 阈值:
# a_lat > 60: factor=2,   (低 MLP)
# 45 < a_lat ≤ 60: factor=4,  (中 MLP)
# a_lat > 40: factor=8,   (高 MLP)
# a_lat ≤ 40: factor=12,  (超高 MLP，如向量操作)
```

### 原版关键特征

1. **AOL 是全局指标**，不是 per-page/per-object 的
2. **4 个 Intel PMU 事件** 全部是 x86 独有的，ARM 不存在
3. **24.67 和 0.87** 是 SKX (Skylake-X) 的常数，来自论文 Table 1
4. **MLP 判断基于 a_lat 阈值**，不是直接测量
5. **对象评分 = 访问占比 × DRAM stall density / MLP factor**，按时段累加

---

## 二、KUMF 当前实现的 AOL 计算（soar_analyzer.py）

### _score_page 方法（第 230-265 行）

```python
# MLP 估算:
if p.llc_misses > 0:
    p.mlp_approx = max(1.0, p.memory_accesses / p.llc_misses)
else:
    p.mlp_approx = self.pmu.mlp_global   # = bus_access / stalled_backend

# SVE 调整:
effective_mlp = p.mlp_approx * (1.0 + p.sve_ratio * 2.0)

# AOL 评分:
p.aol_score = (p.llc_miss_ratio / effective_mlp) * log2(total_accesses)

# 延迟加成 (PEBS):
if p.avg_latency > 0:
    p.aol_score *= (1 + p.avg_latency / 100.0)
```

---

## 三、逐项对比审查

### 1. AOL 指标定义 ★★★ 核心差异

| | 原版 SoarAlto | KUMF 当前实现 | 判定 |
|--|--|--|--|
| **AOL 含义** | Amortized Offcore Latency (周期) | 没有真正计算 AOL | ❌ **缺失** |
| **AOL 公式** | `cyc_demand_data_rd / demand_data_rd` | 无对应计算 | ❌ **缺失** |
| **ARM 等价** | 应该用 SPE 延迟 + PMU 重建 | 用 llc_miss_ratio 近似 | ⚠️ **替代品** |
| **DRAM stall density** | `l3_stall/cyc / (24.67/AOL + 0.87)` | 无 | ❌ **缺失** |
| **SKX 常数** | 24.87, 0.87 | 无 | ❌ **缺失**（也不该直接用，是 x86 的） |

**结论**: KUMF **没有真正实现 AOL 重建**，用的是简化的 llc_miss_ratio / MLP_approx 替代品。

### 2. MLP 估算

| | 原版 | KUMF | 判定 |
|--|--|--|--|
| **MLP 来源** | a_lat 阈值推断 (40/45/60/80) | memory_accesses / llc_misses 或 PMU global | ⚠️ **不同方法** |
| **ARM PMU** | 不需要 (x86) | bus_access / stalled_backend | ⚠️ **未验证** |
| **SVE 感知** | 无 | sve_ratio × 2.0 乘数 | ✅ **创新点** |

**结论**: MLP 估算方法不同但合理。SVE 感知是新贡献。但 **ARM PMU 事件映射未验证**。

### 3. 对象评分公式

| | 原版 | KUMF | 判定 |
|--|--|--|--|
| **评分公式** | (占比 × DRAM_stall_density) / MLP_factor | (llc_miss_ratio / MLP) × log2(accesses) | ⚠️ **完全不同的公式** |
| **时间维度** | 按时间段累加，跟踪 AOL 随时间变化 | 无时间维度，一次性计算 | ❌ **缺失** |
| **访问占比** | content[obj]/all_acc | log2(accesses) | ⚠️ **不同但合理** |
| **DRAM stall density** | est_dram_sd_2 | 无 | ❌ **缺失** |

**结论**: 评分公式跟原版不同，但可能合理——因为 ARM 上的指标体系本来就不一样。关键是**缺乏验证**。

### 4. SPE 数据采集

| | 原版 PEBS | KUMF SPE | 判定 |
|--|--|--|--|
| **采集工具** | perf mem record (Intel) | perf record arm_spe_0 (ARM) | ✅ 正确 |
| **采集脚本** | 手动 | kumf_spe_capture.sh | ✅ 有 |
| **自 profiling** | 不需要 (sudo perf) | spe_self_profile.c | ✅ **创新** |
| **SPE 解析** | 不需要 | SPEParser._parse_arm() | ⚠️ **格式匹配可能有误** |
| **data_source** | PEBS LVL 字段 | SPE event_type 映射 | ⚠️ **SPE data_source 未确认** |

### 5. PMU 辅助统计

| | 原版 | KUMF | 判定 |
|--|--|--|--|
| **PMU 事件** | cycles, STALLS_L3_MISS, OFFCORE_REQUESTS*, 4 个 | bus_access, LLC-loads, LLC-load-misses, l1d_cache*, 8 个 | ⚠️ **映射关系未确认** |
| **ARM PMU 可用性** | N/A | 未在鲲鹏930实测确认 | ❌ **未验证** |
| **全局 MLP** | 不需要 | bus_access / stalled_backend | ⚠️ **公式是否正确？** |

---

## 四、核心结论

### ❌ AOL ARM 重建**没有完成**

具体来说：

1. **没有计算真正的 AOL** — 原版 `a_lat = cyc_demand_data_rd / demand_data_rd` 在 ARM 上没有对应实现
2. **没有 DRAM stall density** — 原版的核心评分指标 `est_dram_sd` 完全缺失
3. **SKX 常数没有替换** — 需要用鲲鹏930 的实测值替换 24.87 和 0.87
4. **时间维度缺失** — 原版按时段跟踪 AOL 变化，当前是一次性计算
5. **评分公式不等价** — 虽然不同平台可能需要不同公式，但需要验证分级效果一致

### ✅ 已完成的部分

1. **SPE 数据采集框架** — 采集脚本 + 自 profiling 工具
2. **SPE 解析器** — 基本的 ARM SPE 格式解析
3. **Page-level 评分** — 用简化公式 (llc_miss_ratio / MLP) × log2(accesses)
4. **SVE 感知** — 创新点，原版没有
5. **Tier 分类器** — Top 20% → FAST, 底部 50% → SLOW
6. **Pipeline 框架** — soar_pipeline.py 完整闭环框架

### ⚠️ 需要验证的部分

1. ARM SPE 的 data_source 字段是否能区分 L1/L2/L3/Remote（论文声称 SPE 比 PEBS 更强，但没实测）
2. ARM PMU 事件 `bus_access`, `LLC-loads` 等在鲲鹏930 上是否真实可用
3. `bus_access / stalled_backend` 作为 MLP 近似是否合理

---

## 五、修复建议（按优先级）

### 🔴 P0: 在鲲鹏930 上实测 ARM PMU 事件可用性

```bash
ssh kunpeng
perf list | grep -E "bus_access|LLC|l1d_cache|stalled|bus_cycles"
perf stat -e bus_access,LLC-loads,LLC-load-misses,bus_cycles,cycles,stalled-cycles-backend \
    -C 0-39 sleep 5
```

这是重建 AOL 的基础——先确认 ARM 上有哪些 PMU 事件可用。

### 🔴 P0: 实现 ARM 版的 AOL 计算

参考论文公式，在 ARM 上的重建路径：

**方案 A (SPE 延迟直推)**:
```python
# ARM SPE 提供 per-sample latency，这是 Intel PEBS 没有的优势
# 可以直接用 SPE 延迟计算 page-level AOL
aol_page = avg_spe_latency_for_page / mlp_page
```

**方案 B (PMU 事件重建)**:
```python
# 类似原版，用 ARM PMU 事件
# ARM 等价:
#   OFFCORE_REQUESTS.DEMAND_DATA_RD → bus_access_rd (如果可用)
#   CYCLES_WITH_DEMAND_DATA_RD → bus_cycles (如果可用)
#   STALLS_L3_MISS → l3d_cache_lmiss_rd (如果可用)
a_lat_arm = bus_cycles / bus_access_rd   # 需要验证
```

**建议**: 先用方案 A（SPE 延迟直推），这是 ARM 的天然优势，SPE 直接给出每次访问的延迟。

### 🟡 P1: 替换 SKX 常数为鲲鹏930 实测值

```bash
# 测量鲲鹏930 的 L3 延迟
# 在 Node 0 上跑 L3 延迟测试
numactl --cpunodebind=0 --membind=0 ./l3_latency_bench
# 在 Node 2 上跑
numactl --cpunodebind=0 --membind=2 ./l3_latency_bench
```

### 🟡 P1: 添加时间维度跟踪

原版按时段（每 10 亿周期）跟踪 AOL 变化，这对于捕捉 workload 的阶段性特征很重要。
当前实现是一次性汇总，丢失了时间信息。

### 🟢 P2: 验证分级效果一致性

即使公式不同，只要最终的分级排序与原版一致（热页→FAST，冷页→SLOW），就证明有效。
需要在鲲鹏930 上跑同一个 workload，对比简化版 vs SPE 延迟版的分级结果。

---

## 六、总结

| 维度 | 状态 | 说明 |
|------|------|------|
| SPE 数据采集 | ✅ 框架完成 | 采集脚本 + 自 profiling |
| SPE 数据解析 | ⚠️ 基本完成 | 格式匹配需实测确认 |
| **AOL ARM 重建** | **❌ 未完成** | **没有计算真正的 AOL** |
| MLP 估算 | ⚠️ 简化版 | 方法不同但可能合理 |
| SVE 感知评分 | ✅ 创新完成 | 原版没有 |
| 对象评分公式 | ⚠️ 简化版 | 与原版不等价，需验证 |
| DRAM stall density | ❌ 缺失 | 原版核心指标 |
| 时间维度跟踪 | ❌ 缺失 | 原版有，当前无 |
| Tier 分类器 | ✅ 完成 | 简单百分比切分 |
| Pipeline 框架 | ✅ 完成 | 完整闭环框架 |

**一句话结论**: AOL ARM 重建的**框架搭好了**，但**核心算法还没有实现**。当前用的是简化替代品 (llc_miss_ratio / MLP)，不是论文定义的 Amortized Offcore Latency。需要在鲲鹏930 上实测 ARM PMU 事件后，实现基于 SPE 延迟的真正 AOL 计算。
