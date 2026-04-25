#!/bin/bash
# ============================================================
# KUMF 鲲鹏930 SOAR 端到端验证脚本
#
# 用法:
#   1. 克隆仓库: git clone git@github.com:xuefenghao5121/KUMF.git
#   2. 编译: cd KUMF/src/tools && make && cd ../../src/lib/interc && make
#   3. 执行: bash tests/kunpeng_soar_e2e.sh [--sudo]
#
# 选项:
#   --sudo    同时执行 SPE 数据采集（需要 sudo 权限）
#
# 需要:
#   - 鲲鹏930, openEuler, GROMACS 已安装
#   - numactl, perf 工具
#   - 有 sudo 权限时可用 --sudo 启用 SPE 采集
# ============================================================
set -e

# ---- 路径自动检测 ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KUMF_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

INTERC_DIR="$KUMF_ROOT/src/lib/interc"
TOOLS_DIR="$KUMF_ROOT/src/tools"
INTERC_LIB="$INTERC_DIR/ldlib.so"
SPE_CAPTURE="$KUMF_ROOT/scripts/kumf_spe_capture.sh"

# GROMACS 路径检测
GMX_RC=""
for candidate in \
    "$HOME/pto_gromacs_test/install/gromacs-2024.3/bin/GMXRC.bash" \
    "$HOME/gromacs_kunpeng_test/install/bin/GMXRC.bash" \
    "/usr/local/gromacs/bin/GMXRC.bash"; do
    if [ -f "$candidate" ]; then
        GMX_RC="$candidate"
        break
    fi
done

# Benchmark 路径检测
BENCH_DIR=""
for candidate in \
    "$HOME/gromacs_benchmark" \
    "$HOME/bench_gromacs"; do
    if [ -d "$candidate" ] && ls "$candidate"/*.tpr >/dev/null 2>&1; then
        BENCH_DIR="$candidate"
        break
    fi
done

# 找 .tpr 文件（优先 large）
TPR_FILE=""
if [ -n "$BENCH_DIR" ]; then
    for pref in bench_large bench_medium bench_small md; do
        for f in "$BENCH_DIR"/${pref}.tpr "$BENCH_DIR"/${pref}_*.tpr; do
            if [ -f "$f" ]; then
                TPR_FILE="$f"
                break 2
            fi
        done
    done
    # fallback: 任意 .tpr
    if [ -z "$TPR_FILE" ]; then
        TPR_FILE="$(ls "$BENCH_DIR"/*.tpr 2>/dev/null | head -1)"
    fi
fi

# 线程数自动检测
NTHREADS=$(nproc)
if [ "$NTHREADS" -gt 64 ]; then
    NTHREADS=64  # 限制为单 socket
fi

WORK_DIR="/tmp/kumf-soar-e2e"
DO_SPE=0

# 解析参数
for arg in "$@"; do
    case "$arg" in
        --sudo) DO_SPE=1 ;;
        --help|-h)
            echo "用法: bash $0 [--sudo]"
            echo "  --sudo    同时执行 SPE 数据采集（需要 sudo）"
            exit 0 ;;
    esac
done

# ---- 前置检查 ----
echo "============================================================"
echo "  KUMF 鲲鹏930 SOAR 端到端验证"
echo "  平台: $(uname -m) | Kernel: $(uname -r)"
echo "  NUMA nodes: $(numactl -H | grep available | awk '{print $3}')"
echo "  KUMF root: $KUMF_ROOT"
echo "============================================================"

if [ -z "$GMX_RC" ]; then
    echo "❌ 未找到 GROMACS (GMXRC.bash)，请设置环境变量:"
    echo "   export GMXRC=/path/to/GMXRC.bash"
    exit 1
fi
if [ -z "$TPR_FILE" ]; then
    echo "❌ 未找到 .tpr benchmark 文件"
    echo "   请在 ~/gromacs_benchmark/ 下放置 .tpr 文件"
    exit 1
fi

# 加载 GROMACS
source "$GMX_RC"
echo "✅ GROMACS: $GMX_RC"
echo "✅ TPR: $TPR_FILE"
echo "✅ Threads: $NTHREADS"

# 编译 interc
if [ ! -f "$INTERC_LIB" ]; then
    echo "[编译] interc..."
    make -C "$INTERC_DIR" 2>&1 | tail -2
fi
if [ ! -f "$INTERC_LIB" ]; then
    echo "❌ interc 编译失败"
    exit 1
fi
echo "✅ interc: $INTERC_LIB"

# 编译 SPE 工具
if [ ! -f "$TOOLS_DIR/spe_self_profile" ]; then
    echo "[编译] SPE 工具..."
    make -C "$TOOLS_DIR" 2>&1 | tail -2
fi

mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

NSTEPS=5000
NUMA_FAST="0,1"
NUMA_SLOW="2,3"

# ---- 测试函数 ----
run_gmx() {
    local label="$1"
    shift
    echo ""
    echo "[$label] $@"
    "$@" 2>&1 | grep -E "Performance|Time" || true
}

# ---- Step 1-4: NUMA baseline 对比 ----
echo ""
echo "========== NUMA Baseline =========="

run_gmx "1/5" numactl --cpunodebind="$NUMA_FAST" \
    gmx mdrun -s "$TPR_FILE" -nsteps $NSTEPS -ntomp $NTHREADS -noconfout -deffnm e2e_baseline

run_gmx "2/5" numactl --cpunodebind="$NUMA_FAST" --membind="$NUMA_FAST" \
    gmx mdrun -s "$TPR_FILE" -nsteps $NSTEPS -ntomp $NTHREADS -noconfout -deffnm e2e_fast

run_gmx "3/5" numactl --cpunodebind="$NUMA_FAST" --membind="$NUMA_SLOW" \
    gmx mdrun -s "$TPR_FILE" -nsteps $NSTEPS -ntomp $NTHREADS -noconfout -deffnm e2e_slow

run_gmx "4/5" numactl --cpunodebind="$NUMA_FAST" --interleave="$NUMA_FAST,$NUMA_SLOW" \
    gmx mdrun -s "$TPR_FILE" -nsteps $NSTEPS -ntomp $NTHREADS -noconfout -deffnm e2e_interleave

# ---- Step 5: interc SOAR ----
echo ""
echo "========== SOAR (interc) =========="

# 生成 SOAR 配置
mkdir -p /tmp/kumf
cat > /tmp/kumf/soar_e2e.cfg << 'EOF'
# GROMACS SOAR 配置 - 热数据→快层, 冷数据→慢层
# 热数据 (force/position/velocity)
gmx*force* 0
gmx*x* 0
gmx*v* 0
gmx*f* 0
# 冷数据 (topology/parameters)
gmx*topology* 2
gmx*param* 2
EOF

run_gmx "5/5" \
    env KUMF_CFG=/tmp/kumf/soar_e2e.cfg LD_PRELOAD="$INTERC_LIB" \
    numactl --cpunodebind="$NUMA_FAST" \
    gmx mdrun -s "$TPR_FILE" -nsteps $NSTEPS -ntomp $NTHREADS -noconfout -deffnm e2e_soar

# ---- Step 6 (可选): SPE 数据采集 ----
if [ "$DO_SPE" -eq 1 ]; then
    echo ""
    echo "========== SPE 数据采集 =========="
    echo "[SPE] 需要 sudo 权限..."
    sudo bash "$SPE_CAPTURE"
    echo "✅ SPE 数据已保存到 /tmp/kumf/"
    echo "   分析命令: python3 $TOOLS_DIR/soar_analyzer.py --spe /tmp/kumf/spe_gromacs_latest.txt --mode page --output /tmp/kumf/soar_aol.csv"
fi

# ---- 结果汇总 ----
echo ""
echo "============================================================"
echo "  测试完成"
echo ""
echo "  结果文件:"
ls -la "$WORK_DIR"/e2e_*.log* 2>/dev/null || echo "  (日志在终端输出中)"
echo ""
echo "  性能对比:"
echo "    1. Baseline (first-touch, CPU $NUMA_FAST)"
echo "    2. 全快层   (CPU+MEM $NUMA_FAST)"
echo "    3. 全慢层   (CPU $NUMA_FAST, MEM $NUMA_SLOW)"
echo "    4. Interleave (CPU $NUMA_FAST, MEM 交错)"
echo "    5. SOAR     (interc 热页路由)"
if [ "$DO_SPE" -eq 1 ]; then
echo "    6. SPE 数据已采集 → 可用 soar_analyzer.py 分析"
fi
echo "============================================================"
