# KUMF 项目记忆

> 最后更新: 2026-04-27

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

## AOL ARM 重建状态

**❌ 未完成** — 当前用的是简化替代品，不是论文定义的 Amortized Offcore Latency

| 维度 | 状态 | 说明 |
|------|------|------|
| SPE 数据采集 | ✅ | 脚本 + 自 profiling |
| SPE 数据解析 | ✅ | soar_spe_report.py |
| **AOL ARM 重建** | **❌** | **缺少真正的 AOL 计算** |
| MLP 估算 | ⚠️ | 简化版，精度未验证 |
| SVE 感知评分 | ✅ | 创新点，原版没有 |
| DRAM stall density | ❌ | 原版核心指标，缺失 |
| 时间维度跟踪 | ❌ | 原版有，当前无 |

详见: `docs/reviews/AOL_ARM_REVIEW.md`

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

---

## 柱子哥审查

- 创新性 ⭐⭐⭐⭐ | 可行性 ⭐⭐⭐ | 价值 ⭐⭐⭐⭐
- "这个项目做成了，有 OSDI/ASPLOS 级别贡献潜力。"

---

## 待解决问题

- [ ] numactl-devel 安装（需 sudo）
- [ ] SPE min_latency 最优参数
- [ ] ARM PMU 事件可用性实测
- [ ] 鲲鹏930 L3 延迟实测值
- [ ] 4 NUMA node 拓扑自适应
