# KUMF 项目记忆

> 最后更新: 2026-04-28

## 项目基本信息

- **名称**: KUMF (Kunpeng Unified Memory Framework)
- **GitHub**: https://github.com/xuefenghao5121/KUMF
- **本地路径**: `/home/huawei/Desktop/home/xuefenghao/workspace/KUMF/`
- **灵感来源**: OSDI'25 "Tiered Memory Management Beyond Hotness"
- **原版代码**: `/home/huawei/Desktop/home/xuefenghao/workspace/SoarAlto/`
- **目标平台**: 鲲鹏930, 120核 ARM64, 4 NUMA node, Linux 6.6.0
- **SSH**: `ssh kunpeng`

---

## 🎉 里程碑：SOAR 真机验证成功 (2026-04-27)

### 端到端结果

| 测试 | 总耗时 | masses 在哪 | 说明 |
|------|--------|------------|------|
| 全快层 | 0.599s | Node 0 | 性能上限 |
| 全慢层 | 1.398s | Node 0 | 性能下限 |
| **SOAR** | **0.855s** | **Node 2 ✅** | **67.8% Gap Closing** |

### 带宽对比

| 指标 | 全快层 | SOAR | 全慢层 |
|------|--------|------|--------|
| 热数据写 BW | 2.25 GB/s | **2.23 GB/s** (-0.9%) | 1.00 GB/s |
| 冷数据读 BW | 3.95 GB/s | 1.36 GB/s | 1.49 GB/s |

### 验证流程

1. SPE 采集: `perf record -e arm_spe/load_filter=1,store_filter=1/`
2. SOAR 分析: `soar_spe_report.py --report spe_report.txt`
3. interc 路由: `KUMF_CONF=xxx LD_PRELOAD=interc/ldlib.so`
4. 四组对比: 全快 / 全慢 / first-touch / SOAR

---

## 鲲鹏930 环境

```
NUMA: 4 node × (40核 + ~64GB)
距离: 同 socket 12, 跨 socket 35-40 (~3x)
SPE: arm_spe_0 ✅ | SVE: ✅
内核: 6.6.0 (openEuler 24.03 LTS-SP3)
工具: GCC 12.3.1, libnuma 2.0.16, perf 6.6.0
```

### NUMA 差异实测 (kumf_stream, 256MB malloc)

| 访问模式 | Node 0 | Node 2 | 差异 |
|----------|--------|--------|------|
| 随机读 | 199ms | 337ms | 69% |
| 顺序读 | 72ms | 178ms | 146% |
| MD-stride 读 | 72ms | 178ms | 148% |
| MD 读写 | 119ms | 271ms | 127% |
| 指针追逐 | 2966ms | 5547ms | 87% |

---

## ARM SPE 采集踩坑记录 ★★★

1. **SPE 语法**: `arm_spe/load_filter=1,store_filter=1/min_latency=32/`（不是 x86 的 `arm_spe_0/Load+Store+min_latency=32/`）
2. **per-task 模式必须**: `perf record -e arm_spe/... -- ./workload`（`-C 0` 会采到内核和 perf 自身）
3. **perf script 无数据地址**: 只有 PC（指令地址），没有被访问的数据地址
4. **perf report --mem-mode 有数据地址**: 但输出是聚合的，不是逐条样本
5. **解决方案**: 用 `soar_spe_report.py` 解析 perf report 聚合数据做 page-level 分级
6. **min_latency 过滤**: 会过滤掉 L1 命中的热数据样本，不带过滤更全

---

## 鲲鹏930 PMU 事件实测 ★ 2026-04-28

### 可用事件（内存/stall 相关）

| 事件 | 可用 | 说明 |
|------|------|------|
| `mem_access` | ✅ | 内存访问计数 |
| `bus_access` | ✅ | 总线访问（offcore 请求） |
| `remote_access` | ✅ | 远端 NUMA 访问 |
| `remote_access_rd` | ✅ | 远端读访问 |
| `stalled-cycles-backend` | ✅ | 后端 stall 周期 |
| `stalled-cycles-frontend` | ✅ | 前端 stall 周期 |
| `stall_slot_backend` | ✅ | 后端 stall slot |
| `stall_slot_frontend` | ✅ | 前端 stall slot |
| `cycles` | ✅ | 总周期 |
| `LLC-loads` | ✅ | L3 加载 |
| `LLC-load-misses` | ✅ | L3 未命中 |
| `L1-dcache-loads/misses` | ✅ | L1 数据缓存 |
| `cache-misses/references` | ✅ | 缓存事件 |
| **`bus_cycles`** | **❌** | **不可用！计算 AOL 的关键缺失** |

### PMU 校准数据 (kumf_tiered 200 100 50)

| 指标 | 本地 (Node 0) | 远端 (Node 2) | 倍率 |
|------|-------------|-------------|------|
| cycles | 16.84B | 38.98B | **2.31x** |
| stall_backend/cycles | 70.2% | 69.9% | ~1.0x |
| mem_access | 4,253M | 4,280M | ~1.0x |
| bus_access | 805M | 692M | 0.86x |
| LLC-load-misses | 366.5M (89%) | 226.6M (55%) | — |
| remote_access | 36K | **232.3M** | — |

### 关键发现：stall_backend 不是内存主导 ⭐⭐⭐

- 本地和远端的 `stall_backend/cycles` 几乎相同（70.2% vs 69.9%）
- 总周期差 2.31x，但 stall 占比不变
- **~70% 的后端 stall 不是内存导致的**（流水线依赖、分支预测等）
- SoarAlto 的 est_dram_sd 公式在 ARM 上不适用：
  - x86 的 `CYCLE_ACTIVITY.STALLS_L3_MISS` 是内存 stall 精确计数
  - ARM 的 `stall_backend` 是所有后端 stall 总和
  - 求解 ARM 常数: C_arm ≈ 0.07, K_arm ≈ 0.697（K >> C/a_lat，99% 与 offcore 无关）
- **这是论文级发现：ARM stall_backend 不可直接替代 x86 STALLS_L3_MISS**

### NUMA 惩罚计算

```
额外周期 = 38.98B - 16.84B = 22.14B cycles
远端访问数 = 232.3M remote_access
NUMA 互连开销 = 22.14B / 232.3M = 48.2 cycles/remote_access
远端 DRAM 延迟 ≈ 本地 DRAM (~200) + NUMA (~48) ≈ 250 cycles
```

---

## ARM SPE data_source 完整编码 ★ 2026-04-28 学习

### 鲲鹏930 使用 HiSilicon HIP 编码

从 Linux 内核源码 `tools/perf/util/arm-spe.c` 确认，鲲鹏使用 `arm_spe__synth_data_source_hisi_hip` 解码：

| SPE source 编码 | perf mem-mode 输出 | 含义 | 估算延迟(cycles) |
|----------------|-------------------|------|-----------------|
| `HISI_HIP_L1` | L1 hit | L1 数据缓存命中 | ~4 |
| `HISI_HIP_L2` | L2 hit | L2 缓存命中 | ~12 |
| `HISI_HIP_L2_HITM` | L2 hit + HITM | L2 命中但Modified | ~12 |
| `HISI_HIP_PEER_CPU` | L2 hit + PEER | 同 socket 其他核 L2 | ~20 |
| `HISI_HIP_PEER_CPU_HITM` | L2 hit + HITM + PEER | 跨核 Modified | ~25 |
| `HISI_HIP_L3` | L3 hit | L3 缓存命中 | ~40 |
| `HISI_HIP_L3_HITM` | L3 hit + HITM | L3 命中但Modified | ~45 |
| `HISI_HIP_PEER_CLUSTER` | REM_CCE1 + PEER | 同 socket 跨 cluster L3 | ~60 |
| `HISI_HIP_REMOTE_SOCKET` | REM_CCE2 + REMOTE | 跨 socket 远端缓存 | ~150 |
| `HISI_HIP_REMOTE_SOCKET_HITM` | REM_CCE2 + HITM + REMOTE | 跨 socket Modified | ~200 |
| `HISI_HIP_LOCAL_MEM` | LOC_RAM | 本地 DRAM | ~200 |
| `HISI_HIP_REMOTE_MEM` | REM_RAM1 + REMOTE | 远端 DRAM | ~300 |
| `HISI_HIP_NC_DEV` | IO | Non-Coherent 设备 | N/A |

### SPE latency 字段

- **SPE 硬件记录 latency**（从采样开始到操作完成的总周期数）
- **perf 存储位置**: `sample.weight = record->latency`
- **`perf report --mem-mode --stdio` 不显示 latency**
- **`perf script -F pid,ip,addr,weight` 应该能看到**
- **待验证**: 鲲鹏930 上 `perf script` 是否输出 weight 列

### SPE 事件过滤位 (FEAT_SPEv1p4)

| Bit | 事件 | 用途 |
|-----|------|------|
| 3 | L1D refill | L1 未命中 |
| 5 | TLB refill | TLB 未命中 |
| 8 | Last level cache access | L3 访问 |
| **9** | **Last level cache miss** | **L3 未命中 → 关键** |
| **10** | **Remote access** | **远端 NUMA 访问 → 关键** |
| 17 | Partial/empty SVE predicate | SVE 向量不完整 |
| 19 | L2D access | L2 访问 |
| 20 | L2D miss | L2 未命中 |

---

## PAC ARM 重建方案 ★ 2026-04-28 设计

### 核心思路

不移植 x86 的 AOL 公式，改用 PACT (ASPLOS'26) 的 PAC 思路：

```
PAC(page) = access_count × stall_per_access / MLP
```

- **高 PAC** → 必须放快层（访问多 + 延迟大 + MLP 低）
- **低 PAC** → 可以放慢层（访问少 或 MLP 高如 SVE）

### 三篇论文对比

| | SoarAlto (OSDI'25) | PACT (ASPLOS'26) | TierBPF (arXiv 2026) |
|--|-------------------|------------------|---------------------|
| 核心指标 | AOL + DRAM stall density | PAC (性能关键性) | 迁移准入控制 |
| 采集手段 | 4 x86 PMU 事件 | eBPF + PEBS | eBPF hooks |
| 粒度 | 时段级 | **page 级** | page 级 |
| 适配 ARM | ❌ 完全不可移植 | ✅ 可用 SPE 替代 PEBS | ✅ eBPF 可用 |
| 与 KUMF 关系 | 基础思路 | **核心借鉴** | 辅助参考 |

### PAC 计算的三个输入

| 输入 | 来源 | 鲲鹏930 实现 |
|------|------|-------------|
| **access_count** | SPE + eBPF | `perf report --mem-mode` 统计每个 page 的访问次数 |
| **stall_per_access** | SPE latency 或 data_source 代理 | 方案 A: `perf script -F weight`（如果可用）<br>方案 B: data_source 延迟级别加权 |
| **MLP** | PMU 全局基线 | `mem_access / stall_slot_backend` 或 `bus_access / stall_slot_backend` |

### SVE 感知评分（KUMF 创新点）

- SVE 向量 load 天然高 MLP（一个 512-bit load = 8 个标量 load 的带宽）
- SPE 可检测 SVE 操作（`ARM_SPE_OP_SVE` flag，bit 17/18 检测 predicate）
- SVE 高 MLP → 降低 PAC → 适当放宽快层要求
- 这是原版 SoarAlto 和 PACT 都没有的

### 两条实现路径

**路径 A: SPE latency 直推（首选）**
```python
# 如果 perf script 输出 weight 列
for page in pages:
    avg_latency = mean(spe_weight for sample in page.samples)
    pac[page] = page.access_count * avg_latency / mlp_estimate
```

**路径 B: data_source 延迟级别代理（备选）**
```python
# 用 HiSilicon HIP 编码的延迟权重
latency_weights = {L1: 4, L2: 12, L3: 40, LOCAL_MEM: 200, REMOTE_MEM: 300}
for page in pages:
    weighted_latency = sum(count * latency_weights[level] for level, count in page.access_breakdown)
    stall_per_access = weighted_latency / page.access_count
    pac[page] = page.access_count * stall_per_access / mlp_estimate
```

---

## prof 修复记录 ★ 2026-04-28

### Bug: mmap 拦截器缺少 caller_addr + 重复记录

**根因**: glibc 对 >128KB 的 malloc 内部走 mmap，mmap 拦截器再次记录，caller 变成 glibc 内部地址

**修复**:
1. 新增 `_in_our_alloc` 线程局部变量
2. `malloc/calloc/realloc/memalign/posix_memalign/free` 在调用 `libc_*` 前设置 `_in_our_alloc = 1`
3. `mmap/mmap64/munmap` 拦截器检查 `!_in_our_alloc`，跳过 glibc 内部调用
4. 大块分配的 caller 从 `[init]` 变成 `main+0xNN` ✅

### 修复: soar_spe_report.py NUMA 拓扑解析

**Bug**: `numactl -H` 输出的 `node distances:` 表头行被错误匹配为数据行
**修复**: 解析时 try/except 跳过非数字的 node ID

### 修复: soar_spe_report.py 配置生成

**Bug**: 旧的 `generate_config` 输出 page 地址范围规则，但 interc 在 malloc 时做决策，地址还不存在
**修复**: 新增 `--prof /tmp/kumf` 参数，交叉关联 prof 数据，生成 size-based 规则

---

## 2026-04-28 验证结果

### 五组对比测试 (kumf_tiered)

| 测试 | 总耗时 | 热数据写 | 冷数据读 | 热占比 |
|------|--------|---------|---------|--------|
| 全快层 | 0.608s | 471.1ms | 137.1ms | 77.5% |
| 全慢层 | 1.407s | 1050.1ms | 356.6ms | 74.6% |
| First-touch | 0.608s | — | — | — |
| **SOAR 自动** | **0.588s** | **465.8ms** | **122.1ms** | **79.2%** |
| SOAR 手动 | 0.852s | 462.1ms | 389.9ms | 54.2% |

**关键发现**: SOAR 自动配置比全快层还快 3.3%（0.588 vs 0.608s）
- 原因: 冷数据分离到其他 node，减少 Node 0 带宽争抢
- 但 KUMF_DEBUG 显示冷热数据都在 Node 0（配置规则未正确匹配）

### SPE data_source 分布

| 级别 | 样本数 |
|------|--------|
| L3 hit | 4,527,330 |
| L1 hit | 3,696,721 |
| N/A | 664,832 |

**注意**: 几乎没有 L3 miss/DRAM/Remote 样本（运行时间短，大 L3 cache）

---

## interc 路由方式 ★★★

### 按大小路由（kumf_tiered 使用）

```ini
# kumf_tiered.conf
size_gt:157286400 = 0       # >150MB → Node 0 (热数据)
size_range:52428800-157286400 = 2  # 50-150MB → Node 2 (冷数据)
```

### 按调用者地址路由（原版 SoarAlto 方式）

```ini
0xADDR_START-0xADDR_END = NODE_ID
function_name_pattern = NODE_ID
```

### 调试

```bash
KUMF_DEBUG=1 KUMF_CONF=xxx LD_PRELOAD=interc/ldlib.so ./workload
# 输出: [KUMF] size_rule matched: sz=209715200 -> node 0
```

---

## 技术决策

| # | 日期 | 决策 | 理由 |
|---|------|------|------|
| D1 | 04-24 | 项目定名 KUMF | Kunpeng Unified Memory Framework |
| D2 | 04-24 | 先纯用户态 | 内核 patch 风险高，覆盖 80% 创新 |
| D3 | 04-24 | 用 hnswlib + mini_md 双场景 | HPC + 通用计算 |
| D4 | 04-24 | AOL 方案 A 起步 | 快速验证 |
| D5 | 04-24 | Phase 2 是 go/no-go 点 | AOL 精度决定成败 |
| D6 | 04-27 | interc 加 size-based routing | kumf_tiered 同函数无法按 caller 区分冷热 |
| D7 | 04-27 | 用 perf report 替代 perf script | SPE 数据地址只在 report 中可用 |
| D8 | 04-28 | 放弃 est_dram_sd 公式 | ARM stall_backend 含大量非内存 stall，不适用 |
| D9 | 04-28 | 采用 PACT 的 PAC 思路 | 性能关键性 > 访问频率，SPE 可替代 PEBS |
| D10 | 04-28 | 优先验证 SPE latency（weight 字段） | 如果可用则 page 级精度远超 data_source 代理 |

---

## 柱子哥审查

- 创新性 ⭐⭐⭐⭐ | 可行性 ⭐⭐⭐ | 价值 ⭐⭐⭐⭐
- "这个项目做成了，有 OSDI/ASPLOS 级别贡献潜力。"

---

## 待解决问题

- [ ] **验证 SPE latency (weight 字段)** ← P0 最高优先级
- [ ] 实现 PAC 评分替换简化 AOL
- [ ] SPE 采集加长步数（50步），让冷热差异更显著
- [ ] eBPF page 访问跟踪（补充 SPE 采样盲区）
- [ ] 延迟权重校准（鲲鹏930 实测各级缓存延迟）
- [ ] 4 NUMA node 拓扑自适应
