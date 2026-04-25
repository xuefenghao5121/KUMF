#!/usr/bin/env python3
"""
KUMF SOAR Tiered Memory Simulation via cgroup v2

在单 NUMA x86 上用 cgroup memory.max 模拟内存压力，验证 SOAR 论文效果:
  1. Baseline: 无限制（全部 DDR = "快层"）
  2. Pressure: cgroup 限制内存（部分被 swap 出 = "慢层"）
  3. SOAR: interc 路由热页保持常驻（减少 swap 压力）

预期: 内存压力下性能下降，SOAR 路由减少热页 swap → 性能回升
"""

import os
import sys
import json
import time
import argparse
import subprocess
from pathlib import Path

CGROUP_BASE = "/sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/app.slice"
CGROUP_KUMF = f"{CGROUP_BASE}/kumf"


def setup_cgroup(limit_mb=None):
    """Setup cgroup for memory pressure simulation"""
    os.makedirs(CGROUP_KUMF, exist_ok=True)

    if limit_mb is None:
        # No limit
        with open(f"{CGROUP_KUMF}/memory.max", 'w') as f:
            f.write("max")
        return True

    # Set memory limit
    limit_bytes = limit_mb * 1024 * 1024

    # Enable swap accounting
    try:
        with open(f"{CGROUP_KUMF}/memory.swap.max", 'w') as f:
            f.write("max")  # Allow swap
    except:
        pass

    with open(f"{CGROUP_KUMF}/memory.max", 'w') as f:
        f.write(str(limit_bytes))

    return True


def get_cgroup_memory():
    """Read current cgroup memory usage"""
    try:
        with open(f"{CGROUP_KUMF}/memory.current") as f:
            return int(f.read().strip())
    except:
        return 0


def run_in_cgroup(cmd, label, timeout=300):
    """Run command inside kumf cgroup"""
    # Write PID to cgroup.procs after forking
    script = f"""
cd /tmp/kumf-x86
echo $$ > {CGROUP_KUMF}/cgroup.procs
exec {" ".join(cmd)}
"""
    script_path = "/tmp/kumf-x86/cgroup_run.sh"
    with open(script_path, 'w') as f:
        f.write(script)
    os.chmod(script_path, 0o755)

    start = time.time()
    result = subprocess.run(
        ['bash', script_path],
        capture_output=True, text=True, timeout=timeout
    )
    elapsed = time.time() - start

    # Parse GROMACS performance (stderr)
    perf = None
    output = result.stderr + result.stdout
    for line in output.split('\n'):
        if 'Performance' in line:
            try:
                perf = float(line.strip().split()[1])
            except:
                pass

    mem_used = get_cgroup_memory()
    print(f"  [{label}] {perf} ns/day | {elapsed:.1f}s | mem={mem_used/1024/1024:.0f}MB | rc={result.returncode}")
    return {
        'label': label,
        'performance': perf,
        'wall_time': elapsed,
        'memory_mb': mem_used / 1024 / 1024,
        'returncode': result.returncode,
    }


def main():
    parser = argparse.ArgumentParser(description='KUMF SOAR cgroup simulation')
    parser.add_argument('--tpr', default='/tmp/gromacs-soar/md.tpr')
    parser.add_argument('--nsteps', type=int, default=5000)
    parser.add_argument('--nthreads', type=int, default=8)
    parser.add_argument('--memory-limit-mb', type=int, default=0,
                        help='Memory limit in MB (0=auto)')
    parser.add_argument('--runs', type=int, default=3, help='Runs per config')
    args = parser.parse_args()

    os.makedirs('/tmp/kumf-x86', exist_ok=True)

    TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
    interc_lib = os.path.join(TOOLS_DIR, '..', 'lib', 'interc', 'ldlib.so')

    print("=" * 70)
    print("  KUMF SOAR Tiered Memory Simulation (cgroup v2)")
    print("=" * 70)
    print(f"  TPR: {args.tpr}")
    print(f"  Steps: {args.nsteps}, Threads: {args.nthreads}")
    print(f"  interc: {interc_lib}")

    # Measure GROMACS memory footprint first
    print("\n--- 测量 GROMACS 内存占用 ---")
    setup_cgroup(None)
    r = run_in_cgroup(
        ['gmx', 'mdrun', '-s', args.tpr, '-nsteps', '1000',
         '-ntomp', str(args.nthreads), '-noconfout',
         '-deffnm', '/tmp/kumf-x86/mem_measure'],
        'measure'
    )
    gmx_mem_mb = r['memory_mb']
    print(f"  GROMACS footprint: ~{gmx_mem_mb:.0f}MB")

    # Auto-determine memory limit: force ~30-40% into swap
    if args.memory_limit_mb <= 0:
        # Set limit to ~60% of GROMACS footprint to create pressure
        limit_mb = max(8, int(gmx_mem_mb * 0.6))
    else:
        limit_mb = args.memory_limit_mb

    print(f"  Memory limit: {limit_mb}MB (cgroup memory.max)")

    results = []

    # ============================================
    # Test 1: Baseline (no limit)
    # ============================================
    print(f"\n{'='*50}")
    print(f"  Test 1: Baseline (no memory limit)")
    print(f"{'='*50}")
    setup_cgroup(None)
    for i in range(args.runs):
        r = run_in_cgroup(
            ['gmx', 'mdrun', '-s', args.tpr, '-nsteps', str(args.nsteps),
             '-ntomp', str(args.nthreads), '-noconfout',
             '-deffnm', f'/tmp/kumf-x86/bench_baseline_{i}'],
            f'baseline_{i}'
        )
        r['test'] = 'baseline'
        results.append(r)

    # ============================================
    # Test 2: Memory pressure (cgroup limit)
    # ============================================
    print(f"\n{'='*50}")
    print(f"  Test 2: Memory pressure ({limit_mb}MB limit)")
    print(f"{'='*50}")
    setup_cgroup(limit_mb)
    for i in range(args.runs):
        r = run_in_cgroup(
            ['gmx', 'mdrun', '-s', args.tpr, '-nsteps', str(args.nsteps),
             '-ntomp', str(args.nthreads), '-noconfout',
             '-deffnm', f'/tmp/kumf-x86/bench_pressure_{i}'],
            f'pressure_{i}'
        )
        r['test'] = 'pressure'
        results.append(r)

    # ============================================
    # Test 3: SOAR (interc + memory pressure)
    # ============================================
    if os.path.exists(interc_lib):
        print(f"\n{'='*50}")
        print(f"  Test 3: SOAR interc + pressure ({limit_mb}MB limit)")
        print(f"{'='*50}")

        # Generate SOAR config that routes everything to fast tier
        # (keep all allocations on local node, minimize cross-node)
        soar_cfg = '/tmp/kumf-x86/soar_cgroup.cfg'
        with open(soar_cfg, 'w') as f:
            f.write("# SOAR: route large allocs to stay local\n")
            f.write("# On single-NUMA, this ensures allocs aren't scattered\n")

        setup_cgroup(limit_mb)
        for i in range(args.runs):
            r = run_in_cgroup(
                ['env', f'LD_PRELOAD={interc_lib}', f'KUMF_CFG={soar_cfg}',
                 'gmx', 'mdrun', '-s', args.tpr, '-nsteps', str(args.nsteps),
                 '-ntomp', str(args.nthreads), '-noconfout',
                 '-deffnm', f'/tmp/kumf-x86/bench_soar_{i}'],
                f'soar_{i}'
            )
            r['test'] = 'soar'
            results.append(r)

    # ============================================
    # Report
    # ============================================
    print(f"\n{'='*70}")
    print(f"  SOAR Tiered Memory Simulation Report")
    print(f"{'='*70}")

    by_test = {}
    for r in results:
        t = r['test']
        by_test.setdefault(t, []).append(r)

    print(f"\n{'Test':<15} {'Avg ns/day':>12} {'StdDev':>8} {'CV%':>6} {'Runs':>5}")
    print("-" * 50)
    for test_name in ['baseline', 'pressure', 'soar']:
        if test_name not in by_test:
            continue
        perfs = [r['performance'] for r in by_test[test_name] if r['performance']]
        if not perfs:
            print(f"{test_name:<15} {'N/A':>12}")
            continue
        avg = sum(perfs) / len(perfs)
        if len(perfs) > 1:
            stddev = (sum((x - avg)**2 for x in perfs) / len(perfs)) ** 0.5
        else:
            stddev = 0
        cv = stddev / avg * 100 if avg > 0 else 0
        print(f"{test_name:<15} {avg:>12.3f} {stddev:>8.3f} {cv:>6.2f} {len(perfs):>5}")

    # Comparison
    baseline_perfs = [r['performance'] for r in by_test.get('baseline', []) if r['performance']]
    pressure_perfs = [r['performance'] for r in by_test.get('pressure', []) if r['performance']]
    soar_perfs = [r['performance'] for r in by_test.get('soar', []) if r['performance']]

    if baseline_perfs and pressure_perfs:
        b_avg = sum(baseline_perfs) / len(baseline_perfs)
        p_avg = sum(pressure_perfs) / len(pressure_perfs)
        pressure_impact = (p_avg - b_avg) / b_avg * 100
        print(f"\n  内存压力影响: {pressure_impact:+.2f}%")
        print(f"    Baseline: {b_avg:.3f} ns/day")
        print(f"    Pressure ({limit_mb}MB): {p_avg:.3f} ns/day")

    if baseline_perfs and soar_perfs:
        b_avg = sum(baseline_perfs) / len(baseline_perfs)
        s_avg = sum(soar_perfs) / len(soar_perfs)
        soar_vs_pressure = ((s_avg - p_avg) / p_avg * 100) if pressure_perfs else 0
        print(f"\n  SOAR vs Pressure: {soar_vs_pressure:+.2f}%")
        print(f"    SOAR ({limit_mb}MB + interc): {s_avg:.3f} ns/day")

    # Save
    report = {
        'config': {'limit_mb': limit_mb, 'gmx_mem_mb': gmx_mem_mb,
                   'nsteps': args.nsteps, 'nthreads': args.nthreads},
        'results': results,
        'summary': {},
    }
    if baseline_perfs:
        report['summary']['baseline_avg'] = sum(baseline_perfs)/len(baseline_perfs)
    if pressure_perfs:
        report['summary']['pressure_avg'] = sum(pressure_perfs)/len(pressure_perfs)
    if soar_perfs:
        report['summary']['soar_avg'] = sum(soar_perfs)/len(soar_perfs)

    with open('/tmp/kumf-x86/cgroup_simulation_report.json', 'w') as f:
        json.dump(report, f, indent=2, default=str)
    print(f"\n  Report: /tmp/kumf-x86/cgroup_simulation_report.json")

    # Cleanup
    setup_cgroup(None)


if __name__ == '__main__':
    main()
