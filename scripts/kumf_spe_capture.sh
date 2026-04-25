#!/bin/bash
# ============================================================
# KUMF SPE 数据采集脚本 — 需要管理员(root)执行
# 
# 用途: 为 KUMF Phase 2 AOL 重建采集 ARM SPE 数据
# 执行: sudo bash kumf_spe_capture.sh
# 输出: /tmp/kumf/spe_gromacs_*.txt + /tmp/kumf/pmu_gromacs_*.txt
# ============================================================

set -e
echo "KUMF SPE 数据采集 — 管理员专用"

# 0. 降低 perf 限制（仅本次会话有效，重启恢复）
echo 0 > /proc/sys/kernel/perf_event_paranoid
echo 0 > /proc/sys/kernel/kptr_restrict
echo "✅ perf_event_paranoid=0"

# 1. 准备环境
source /home/xuefenghao/pto_gromacs_test/install/gromacs-2024.3/bin/GMXRC.bash
cd /home/xuefenghao/gromacs_benchmark
mkdir -p /tmp/kumf

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SPE_OUT="/tmp/kumf/spe_gromacs_${TIMESTAMP}.txt"
PMU_OUT="/tmp/kumf/pmu_gromacs_${TIMESTAMP}.txt"
PROF_OUT="/tmp/kumf/prof_gromacs_${TIMESTAMP}.txt"

echo ""
echo "=== Step 1/3: 启动 GROMACS (large, 5000 steps, 64 threads, Node 0-1) ==="
numactl --cpunodebind=0,1 gmx mdrun -s bench_large.tpr -nsteps 5000 -ntomp 64 -noconfout -deffnm spe_capture &
GMX_PID=$!
sleep 2
echo "GROMACS PID=$GMX_PID"

# 2. SPE 采集 (CPU 0-39 = Node 0-1 的核)
echo ""
echo "=== Step 2/3: ARM SPE profiling (30秒) ==="
timeout 30 perf record -e arm_spe_0/Load+Store+min_latency=32/ -C 0-39 -o /tmp/kumf/spe_${TIMESTAMP}.data sleep 30 2>&1 | tail -1

# 转为可读文本
perf script -i /tmp/kumf/spe_${TIMESTAMP}.data > "$SPE_OUT" 2>/dev/null
echo "✅ SPE 输出: $SPE_OUT ($(wc -l < "$SPE_OUT") 行)"

# 3. PMU 采集
echo ""
echo "=== Step 3/3: PMU 统计 ==="
timeout 30 perf stat -e bus_access,LLC-loads,LLC-load-misses,l1d_cache,l1d_cache_lmiss_rd,stalled-cycles-backend,cycles,instructions \
    -C 0-39 -p $GMX_PID sleep 30 > "$PMU_OUT" 2>&1 || true
echo "✅ PMU 输出: $PMU_OUT"

# 等待 GROMACS 结束
wait $GMX_PID 2>/dev/null
echo ""
echo "✅ 采集完成！输出文件:"
echo "   SPE: $SPE_OUT"
echo "   PMU: $PMU_OUT"
echo ""
echo "请将以上文件发给 xuefenghao 用户，或运行:"
echo "   chown xuefenghao:xuefenghao /tmp/kumf/spe_gromacs_${TIMESTAMP}.* /tmp/kumf/pmu_gromacs_${TIMESTAMP}.*"
echo "   cp /tmp/kumf/spe_gromacs_${TIMESTAMP}.txt /tmp/kumf/spe_gromacs_latest.txt"
echo "   cp /tmp/kumf/pmu_gromacs_${TIMESTAMP}.txt /tmp/kumf/pmu_gromacs_latest.txt"
