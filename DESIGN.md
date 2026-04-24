# KUMF 架构设计

> 版本: v0.1 | 日期: 2026-04-24 | 审查: 柱子哥

---

## 整体架构

```
┌─────────────────────────────────────────────────┐
│              用户应用                            │
│   hnswlib (ANN 搜索) / mini_md (MD 模拟) / ...  │
├─────────────────────────────────────────────────┤
│           libkumf.so (LD_PRELOAD)               │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ Profiler  │ │ Scorer   │ │  Allocator       │ │
│  │ (SPE/PMU) │ │ (AOL-ish)│ │  (numa/mbind)    │ │
│  └─────┬────┘ └─────┬────┘ └────────┬─────────┘ │
│        │             │               │            │
│  ┌─────▼─────────────▼───────────────▼─────────┐ │
│  │          Policy Engine                       │ │
│  │   - 对象打分（性能贡献排序）                  │ │
│  │   - 自适应迁移控制（ALTO 思想）              │ │
│  │   - SVE 感知评分                             │ │
│  │   - 配置文件驱动 (kumf.conf)                 │ │
│  └──────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────┤
│       Linux 6.6 (NUMA + Memory Tiering)          │
│   Node 0,1 (快层) ←→ Node 2,3 (慢层)            │
└─────────────────────────────────────────────────┘
```

## 组件设计

### Layer 1: Profiler（采样层）

**ARM SPE 采集器**:
- 使用 perf record -e arm_spe// 采集
- 提取: 虚拟地址, 延迟, data_source(L1/L2/L3/remote), PC, SVE 宽度
- 不需要内核 patch（SPE 是标准 perf 事件）

**LD_PRELOAD 分配追踪器**:
- 拦截 malloc/free/calloc/realloc/mmap
- 记录: 时间戳(clock_gettime), 调用链(backtrace), 大小, 地址
- 输出到 mmap ring buffer（避免 OOM）

### Layer 2: Scorer（评分层）

**AOL ARM 重建**:
```
AOL = avg_spe_latency / MLP_approx

MLP_approx 方案:
  A: bus_access / bus_cycles (ARM PMU)
  B: mem_access / stall_backend_cycles
  C: SPE 时间戳推算 outstanding 请求数
```

**SVE 感知评分**:
- 检测 SVE 向量 load（SPE 报告 sve_width）
- SVE load 天然高 MLP → 降低评分 → 可以放慢层
- 评分公式: `score = access_count × aol × remote_ratio / sve_factor`

### Layer 3: Allocator（分配层）

**配置文件驱动**:
```ini
# kumf.conf
# 函数名 = tier (0=fast, 1=slow, -1=default)

# hnswlib 规则
HNSW:build  = 0
HNSW:search = 0
HNSW:data   = 1

# 通用规则
*buffer* = -1
*log*    = 1
```

**4 NUMA node 拓扑映射**:
```c
// 基于 NUMA 距离自动分层
// distance <= 15: fast tier (Node 0, Node 1)
// distance > 30: slow tier (Node 2, Node 3)
```

## 数据流

```
1. LD_PRELOAD=libkumf.so ./workload
   ↓ 拦截 malloc → 记录分配 (时间, 调用链, 大小, 地址)
2. perf record -e arm_spe/ -p $PID
   ↓ SPE 采样 → (时间, 地址, 延迟, data_source, PC)
3. kumf-analyze profile_data/ spe_data/
   ↓ 关联分配记录 + SPE 记录 → 计算 AOL → 对象评分
4. 输出 kumf.conf
   ↓ 高分对象 → fast tier, 低分对象 → slow tier
5. LD_PRELOAD=libkumf.so KUMF_CONF=kumf.conf ./workload
   ↓ 按 conf 分配 → 验证性能提升
```

## Workload 验证矩阵

| Workload | 领域 | 冷对象 | 热对象 | 指标 |
|----------|------|--------|--------|------|
| hnswlib | AI/通用 | 原始向量, payload | HNSW 图节点, centroids | QPS, P50/P99 延迟 |
| mini_md | HPC | masses, topology, bonds | coordinates, forces, velocities | ns/day |

## 预期性能提升

基于论文数据和鲲鹏 NUMA 延迟比（~3x）:
- SOAR 静态分配：接近全快层性能的 80-90%
- SOAR + ALTO 动态调整：接近全快层性能的 90-95%
- 内存节省：30-50% 快层内存释放给其他应用
