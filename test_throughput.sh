#!/bin/bash
# KUMF Multi-Instance Throughput — Real Application Test
#
# Compares:
#   A: cpunodebind=0,1 → 2 instances × 60 threads → 80 cores used, 40 idle
#   B: per-node pin     → 4 instances × 30 threads → 120 cores fully used
#   C: KUMF per-node    → same as B, via KUMF not numactl
#
# Key metric: throughput (inst/s)
#   B/A = full-machine speedup (expected ~2x)
#   C/B = KUMF parity with optimal manual placement

set -e

APP="${1:-./XSBench}"
ARGS="-m history -G hash -s large -g 50000 -p 10000000"
INTERC_SO="./build/libkumf_interc.so"

if [ ! -f "$INTERC_SO" ]; then
    echo "Error: $INTERC_SO not found. Run: make libs"
    exit 1
fi

echo "=============================================="
echo " KUMF Multi-Instance Throughput"
echo " App: $APP"
echo "=============================================="

# ─── A: Baseline — cpunodebind=0,1, 2 instances ───
echo ""
echo "── A: cpunodebind=0,1 ──"
echo "   2 instances × 60 threads = 120 threads on 80 cores (socket 0 only)"
echo "   40 cores idle (socket 1)"
rm -f /tmp/kumf_tput_a*.log
A_START=$(date +%s.%N)
numactl --cpunodebind=0,1 $APP $ARGS -t 60 > /tmp/kumf_tput_a1.log 2>&1 &
numactl --cpunodebind=0,1 $APP $ARGS -t 60 > /tmp/kumf_tput_a2.log 2>&1 &
wait
A_END=$(date +%s.%N)
A_TIME=$(echo "$A_END - $A_START" | bc -l)
A_TPUT=$(echo "scale=4; 2 / $A_TIME" | bc -l)
printf "   Wall:      %.2fs\n" "$A_TIME"
printf "   Throughput: %.4f inst/s\n" "$A_TPUT"
for i in 1 2; do
    rt=$(grep -oP 'Runtime:\s*\K[0-9.]+' /tmp/kumf_tput_a${i}.log 2>/dev/null || echo "?")
    echo "   Instance $i: ${rt}s"
done

# ─── B: Optimal manual — one instance per node ───
echo ""
echo "── B: per-node numactl ──"
echo "   4 instances × 30 threads = 120 threads on 120 cores (full machine)"
rm -f /tmp/kumf_tput_b*.log
B_START=$(date +%s.%N)
numactl --cpunodebind=0 $APP $ARGS -t 30 > /tmp/kumf_tput_b1.log 2>&1 &
numactl --cpunodebind=1 $APP $ARGS -t 30 > /tmp/kumf_tput_b2.log 2>&1 &
numactl --cpunodebind=2 $APP $ARGS -t 30 > /tmp/kumf_tput_b3.log 2>&1 &
numactl --cpunodebind=3 $APP $ARGS -t 30 > /tmp/kumf_tput_b4.log 2>&1 &
wait
B_END=$(date +%s.%N)
B_TIME=$(echo "$B_END - $B_START" | bc -l)
B_TPUT=$(echo "scale=4; 4 / $B_TIME" | bc -l)
printf "   Wall:      %.2fs\n" "$B_TIME"
printf "   Throughput: %.4f inst/s\n" "$B_TPUT"
for i in 1 2 3 4; do
    rt=$(grep -oP 'Runtime:\s*\K[0-9.]+' /tmp/kumf_tput_b${i}.log 2>/dev/null || echo "?")
    echo "   Instance $i: ${rt}s"
done

# ─── C: KUMF per-node — same placement, via KUMF ───
echo ""
echo "── C: KUMF per-node ──"
echo "   4 instances × 30 threads, KUMF batch mode (= cpunodebind per instance)"
rm -f /tmp/kumf_tput_c*.log
C_START=$(date +%s.%N)
KUMF_NODES=0 KUMF_AFFINITY=batch LD_PRELOAD="$INTERC_SO" $APP $ARGS -t 30 > /tmp/kumf_tput_c1.log 2>&1 &
KUMF_NODES=1 KUMF_AFFINITY=batch LD_PRELOAD="$INTERC_SO" $APP $ARGS -t 30 > /tmp/kumf_tput_c2.log 2>&1 &
KUMF_NODES=2 KUMF_AFFINITY=batch LD_PRELOAD="$INTERC_SO" $APP $ARGS -t 30 > /tmp/kumf_tput_c3.log 2>&1 &
KUMF_NODES=3 KUMF_AFFINITY=batch LD_PRELOAD="$INTERC_SO" $APP $ARGS -t 30 > /tmp/kumf_tput_c4.log 2>&1 &
wait
C_END=$(date +%s.%N)
C_TIME=$(echo "$C_END - $C_START" | bc -l)
C_TPUT=$(echo "scale=4; 4 / $C_TIME" | bc -l)
printf "   Wall:      %.2fs\n" "$C_TIME"
printf "   Throughput: %.4f inst/s\n" "$C_TPUT"
for i in 1 2 3 4; do
    rt=$(grep -oP 'Runtime:\s*\K[0-9.]+' /tmp/kumf_tput_c${i}.log 2>/dev/null || echo "?")
    echo "   Instance $i: ${rt}s"
done

# ─── Summary ───
echo ""
echo "=============================================="
echo " Summary"
echo "=============================================="
printf "%-30s %10s %12s %8s\n" "Config" "Instances" "Wall(s)" "Tput(inst/s)"
printf "%-30s %10d %12.2f %8.4f\n" "A: cpunodebind=0,1" 2 "$A_TIME" "$A_TPUT"
printf "%-30s %10d %12.2f %8.4f\n" "B: per-node numactl" 4 "$B_TIME" "$B_TPUT"
printf "%-30s %10d %12.2f %8.4f\n" "C: KUMF per-node" 4 "$C_TIME" "$C_TPUT"

SPEEDUP_BA=$(echo "scale=2; $A_TPUT > 0 ? $B_TPUT / $A_TPUT : 0" | bc -l)
SPEEDUP_CB=$(echo "scale=2; $B_TPUT > 0 ? $C_TPUT / $B_TPUT : 0" | bc -l)
echo ""
echo " Throughput speedup (B/A): ${SPEEDUP_BA}x  ← full-machine vs half-machine"
echo " KUMF parity     (C/B):   ${SPEEDUP_CB}x  ← KUMF vs optimal manual (target = 1.0x)"

rm -f /tmp/kumf_tput_*.log
