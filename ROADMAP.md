# KUMF Roadmap

> 最后更新: 2026-04-27

## ✅ Phase 0: 环境验证 (2026-04-24)

- [x] 鲲鹏930 NUMA topology 确认（4 node, 延迟 ~3x）
- [x] ARM SPE 确认可用（arm_spe_0, type=135）
- [x] SVE 特性确认
- [x] 柱子哥架构审查完成
- [x] hnswlib NUMA baseline（跨 NUMA QPS 差 28%, 延迟差 39%）

## ⚠️ Phase 1: SOAR ARM 适配 (65%)

- [x] prof/ldlib.c ARM64 适配（clock_gettime, gettid, ARR_SIZE 缩减）
- [x] interc/ldlib.c 轻量级 NUMA 路由（配置文件驱动，size-based routing）
- [x] SPE 采集脚本 + 自 profiling 工具
- [ ] 4 NUMA node 拓扑自适应（当前 hardcode 2 node）
- [ ] ARM backtrace 鲲鹏930 验证

## ⚠️ Phase 2: SPE+AOL 分析 (70%)

- [x] soar_analyzer.py — SPE/PEBS 解析 + Page-level 评分 + Tier 分类
- [x] soar_spe_report.py — ARM SPE perf report 聚合数据解析
- [x] soar_pipeline.py — 完整闭环 pipeline
- [x] SVE 感知评分（创新点）
- [ ] 真正的 AOL ARM 重建（当前是简化版，详见 `docs/reviews/AOL_ARM_REVIEW.md`）
- [ ] MLP 精度验证（ARM PMU 事件映射）

## ✅ Phase 3: 端到端验证 (2026-04-27)

**🎉 SOAR 分层在鲲鹏930 真机验证成功！**

| 测试 | 总耗时 | NUMA 分配 | 说明 |
|------|--------|----------|------|
| 全快层 | 0.599s | 全 Node 0 | 上限 |
| 全慢层 | 1.398s | 全 Node 0 | 下限 |
| SOAR 分层 | 0.855s | 热→Node 0, 冷→Node 2 | **67.8% Gap Closing** |

关键验证：
- ✅ interc size-based routing 正确路由
- ✅ coordinates/forces (200MB) → Node 0（快层）
- ✅ masses (100MB) → Node 2（慢层）
- ✅ 热数据写 BW 损失仅 0.9%

## ⏳ Phase 4: AOL ARM 精确重建

- [ ] ARM PMU 事件可用性实测（bus_access, LLC-loads 等）
- [ ] 基于 SPE 延迟的真正 AOL 计算
- [ ] SKX 常数替换为鲲鹏930 实测值
- [ ] 时间维度跟踪（按时段跟踪 AOL 变化）
- [ ] 验证简化版 vs 精确版分级一致性

## ⏳ Phase 5: 真实 Workload 验证

- [ ] GROMACS 端到端 SOAR 验证
- [ ] hnswlib 端到端 SOAR 验证
- [ ] 性能报告 + 论文级图表

## ⏳ Phase 6: 内核 ALTO Patch（可选）

- [ ] NBT patch 移植到 Linux 6.6
- [ ] 4 node 拓扑内核适配
- [ ] 完整系统性能对比
