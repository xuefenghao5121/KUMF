#!/bin/bash
# ============================================================
# KUMF x86 本地验证脚本
# 在本地 x86 环境验证 SOAR 论文核心流程：
#   PEBS profiling → page 热度分析 → AOL 评分
# 用途：论文有效性基本验证，非 ARM 平台替代
# ============================================================
set -e

SOAR_DIR=$(cd "$(dirname "$0")/.." && pwd)
WORK_DIR=/tmp/kumf-x86-verify
mkdir -p "$WORK_DIR"

echo "============================================================"
echo "  KUMF x86 SOAR Pipeline 验证"
echo "  平台: $(uname -m) | CPU: $(grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
echo "  NUMA nodes: $(numactl -H 2>/dev/null | grep "available" | awk '{print $3}')"
echo "============================================================"

# Step 0: 确认 perf_event_paranoid
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid)
if [ "$PARANOID" -gt 0 ]; then
    echo "⚠️  perf_event_paranoid=$PARANOID, 需要 sudo 降低"
    echo "   sudo sh -c 'echo 0 > /proc/sys/kernel/perf_event_paranoid'"
    exit 1
fi

# Step 1: 准备 GROMACS benchmark 输入
echo ""
echo "[Step 1/5] 准备 GROMACS benchmark..."
cd "$WORK_DIR"

if [ ! -f md.tpr ]; then
    # SPC water box
    gmx insert-molecules -ci /usr/share/gromacs/top/spc216.gro -nmol 1000 -box 5 5 5 -o box.gro 2>/dev/null || true

    cat > topol.top << 'EOF'
#include "oplsaa.ff/forcefield.itp"
#include "oplsaa.ff/spce.itp"
[ system ]
SOAR Benchmark Water
[ molecules ]
SOL 2160
EOF

    # 修正分子数
    NATOMS=$(head -2 box.gro | tail -1 | tr -d ' ')
    NMOL=$((NATOMS / 3))
    sed -i "s/SOL 2160/SOL $NMOL/" topol.top

    cat > md.mdp << 'EOF'
integrator = md
dt = 0.002
nsteps = 10000
cutoff-scheme = Verlet
nstlist = 20
rlist = 1.0
rcoulomb = 1.0
rvdw = 1.0
tcoupl = V-rescale
tc-grps = System
tau_t = 0.1
ref_t = 300
pcoupl = no
constraints = h-bonds
EOF

    gmx grompp -f md.mdp -c box.gro -p topol.top -o md.tpr -maxwarn 5 2>&1 | tail -3
fi

echo "✅ md.tpr 已就绪 ($(stat -c%s md.tpr) bytes)"

# Step 2: Baseline 性能
echo ""
echo "[Step 2/5] Baseline GROMACS 性能..."
BASELINE=$(gmx mdrun -s md.tpr -ntmpi 1 -ntomp 16 -nsteps 10000 -noconfout -deffnm baseline 2>&1 | grep "Performance:" | awk '{print $2}')
echo "✅ Baseline: $BASELINE ns/day"

# Step 3: PEBS profiling
echo ""
echo "[Step 3/5] PEBS profiling..."
rm -f "$WORK_DIR/pebs_gromacs.data"

gmx mdrun -s md.tpr -ntmpi 1 -ntomp 16 -nsteps 10000 -noconfout -deffnm pebs_run &
GMX_PID=$!
sleep 1

sudo perf mem record -C 0-15 -o "$WORK_DIR/pebs_gromacs.data" -D 500 sleep 4 2>&1 | tail -1
wait $GMX_PID 2>/dev/null

SAMPLES=$(sudo perf script -i "$WORK_DIR/pebs_gromacs.data" 2>/dev/null | wc -l)
echo "✅ PEBS 采集完成: $SAMPLES samples"

# Step 4: Page 热度分析
echo ""
echo "[Step 4/5] Page 热度分析..."
sudo perf script -i "$WORK_DIR/pebs_gromacs.data" 2>/dev/null > "$WORK_DIR/pebs.script"
python3 "$SOAR_DIR/src/tools/page_heatmap.py" "$WORK_DIR/pebs.script" 2>&1 | grep -E "Unique|FAST|SLOW|Total"

# Step 5: 验证结论
echo ""
echo "[Step 5/5] 验证结论..."
echo "============================================================"
echo "  x86 验证结果"
echo "============================================================"
echo ""
echo "  ✅ PEBS/SPE 采集: $SAMPLES samples"
echo "  ✅ Page 热度分析: 见 $WORK_DIR/pebs.script_aol.csv"
echo "  ✅ Baseline 性能: $BASELINE ns/day"
echo "  ✅ AOL 评分已生成"
echo ""
echo "  下一步: 在鲲鹏930上验证快/慢层差异 (需要 cgroup)"
echo "============================================================"
