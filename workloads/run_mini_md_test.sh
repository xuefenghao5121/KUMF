#!/bin/bash
# run_mini_md_test.sh — 在鲲鹏930上验证 NUMA 分层效果
#
# 用法: bash run_mini_md_test.sh
#
# 三种配置对比:
#   1. 全快层 (Node 0) — 性能上限
#   2. 全慢层 (Node 2) — 性能下限
#   3. 默认 (first-touch) — Linux 默认行为

set -e

echo "============================================="
echo "  mini_md 真实 Workload NUMA 分层测试"
echo "  鲲鹏930, $(date)"
echo "============================================="

# 编译
echo -e "\n--- 编译 mini_md ---"
gcc -O2 -o mini_md mini_md.c -lm -lnuma
echo "编译完成"

# 参数
PARTICLES=5000
STEPS=200

echo -e "\n--- 测试参数 ---"
echo "粒子数: $PARTICLES"
echo "步数:   $STEPS"
echo "预计单次运行: 10-60 秒"

# ===== Test 1: 全快层 (Node 0) =====
echo -e "\n============================================="
echo "Test 1: 全快层 (Node 0) — 性能上限"
echo "============================================="
numactl --cpunodebind=0 --membind=0 ./mini_md $PARTICLES $STEPS

# ===== Test 2: 全慢层 (Node 2) =====
echo -e "\n============================================="
echo "Test 2: 全慢层 (Node 2) — 性能下限"
echo "============================================="
numactl --cpunodebind=0 --membind=2 ./mini_md $PARTICLES $STEPS

# ===== Test 3: 默认 (first-touch) =====
echo -e "\n============================================="
echo "Test 3: 默认 (first-touch) — Linux 默认"
echo "============================================="
numactl --cpunodebind=0 ./mini_md $PARTICLES $STEPS

# ===== Test 4: 大规模测试 (20K 粒子) =====
echo -e "\n============================================="
echo "Test 4: 大规模 (20K 粒子, 50 步)"
echo "============================================="
echo "--- Node 0 ---"
numactl --cpunodebind=0 --membind=0 ./mini_md 20000 50 2>&1 | grep -E "Elapsed|NUMA Dist|on Node"

echo "--- Node 2 ---"
numactl --cpunodebind=0 --membind=2 ./mini_md 20000 50 2>&1 | grep -E "Elapsed|NUMA Dist|on Node"

echo -e "\n============================================="
echo "  测试完成"
echo "  对比以上 Elapsed 时间即可看到 NUMA 差异"
echo "  Node 0(快) vs Node 2(慢) 的差异就是 tiering 的优化空间"
echo "============================================="
