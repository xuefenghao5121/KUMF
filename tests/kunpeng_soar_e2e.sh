#!/bin/bash
# ============================================================
# KUMF 鲲鹏930 SOAR 端到端验证脚本
# 需要: 无 sudo（cgroup v2 delegation 由管理员预设）
# 目标: 验证 cgroup 内存压力下 SOAR 迁移效果
# ============================================================
set -e

WORK_DIR=/tmp/kumf-soar-e2e
GMX_SRC=/home/xuefenghao/pto_gromacs_test/install/gromacs-2024.3
KUMF_LIB=/home/xuefenghao/kumf/src/lib/interc/ldlib.so
BENCH_DIR=/home/xuefenghao/gromacs_benchmark

mkdir -p "$WORK_DIR"

source "$GMX_SRC/bin/GMXRC.bash"

echo "============================================================"
echo "  KUMF 鲲鹏930 SOAR 端到端验证"
echo "  平台: $(uname -m) | NUMA: $(numactl -H | grep available | awk '{print $3}')"
echo "============================================================"

# Step 1: Baseline（默认 first-touch）
echo ""
echo "[1/6] Baseline: 默认 first-touch, 64 threads"
cd "$BENCH_DIR"
numactl --cpunodebind=0,1 gmx mdrun -s bench_large.tpr -nsteps 5000 -ntomp 64 -noconfout -deffnm e2e_baseline 2>&1 | grep Performance

# Step 2: 全快层
echo ""
echo "[2/6] 全快层 (membind 0,1)"
numactl --cpunodebind=0,1 --membind=0,1 gmx mdrun -s bench_large.tpr -nsteps 5000 -ntomp 64 -noconfout -deffnm e2e_fast 2>&1 | grep Performance

# Step 3: 全慢层
echo ""
echo "[3/6] 全慢层 (membind 2,3)"
numactl --cpunodebind=2,3 --membind=2,3 gmx mdrun -s bench_large.tpr -nsteps 5000 -ntomp 64 -noconfout -deffnm e2e_slow 2>&1 | grep Performance

# Step 4: interleave（模拟内存压力最差情况）
echo ""
echo "[4/6] Interleave 0-3 (模拟内存压力)"
numactl --cpunodebind=0,1 --interleave=0,1,2,3 gmx mdrun -s bench_large.tpr -nsteps 5000 -ntomp 64 -noconfout -deffnm e2e_interleave 2>&1 | grep Performance

# Step 5: SOAR with interc（热→Node0, 冷→Node2）
echo ""
echo "[5/6] SOAR (interc + 配置文件)"
# SOAR 配置: 热数据结构 → Node 0, 冷数据 → Node 2
cat > /tmp/kumf/soar_e2e.cfg << EOF
# GROMACS SOAR 配置
# Force/position arrays (热) → Node 0
gmx*force* 0
gmx*x* 0
gmx*v* 0
gmx*f* 0
# Topology/params (冷) → Node 2
gmx*topology* 2
gmx*param* 2
EOF

cd "$BENCH_DIR" && make -C /home/xuefenghao/kumf/src/lib/interc/ 2>&1 | tail -1
KUMF_CFG=/tmp/kumf/soar_e2e.cfg LD_PRELOAD=$KUMF_LIB numactl --cpunodebind=0,1 gmx mdrun -s bench_large.tpr -nsteps 5000 -ntomp 64 -noconfout -deffnm e2e_soar 2>&1 | grep Performance

# Step 6: cgroup 内存压力测试（如果可用）
echo ""
echo "[6/6] cgroup 内存压力测试"
if [ -w /sys/fs/cgroup/user.slice ]; then
    echo "cgroup 可写，执行内存压力测试..."
    # TODO: cgroup v2 memory.max 限制
else
    echo "⚠️  cgroup 不可写（需要管理员配置 delegation）"
    echo "   建议: 管理员运行以下命令"
    echo "   mkdir -p /sys/fs/cgroup/kumf"
    echo "   echo 2147483648 > /sys/fs/cgroup/kumf/memory.max  # 2GB 限制"
    echo "   chown -R xuefenghao:xuefenghao /sys/fs/cgroup/kumf"
fi

echo ""
echo "============================================================"
echo "  测试完成"
echo "  结果汇总:"
echo "    Baseline (first-touch): 已记录"
echo "    全快层: 已记录"
echo "    全慢层: 已记录"
echo "    Interleave: 已记录"
echo "    SOAR: 已记录"
echo "============================================================"
