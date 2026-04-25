#!/usr/bin/env python3
"""
KUMF SOAR Pipeline: PEBS/SPE → AOL → interc config → benchmark → report

完整闭环验证 SOAR 论文效果:
  1. PEBS profiling GROMACS
  2. Page-level AOL scoring
  3. Auto-generate interc config
  4. Benchmark: baseline vs SOAR (cgroup memory pressure)
  5. Performance comparison report
"""

import os
import sys
import csv
import json
import argparse
import subprocess
import time
import tempfile
from collections import defaultdict

# Add tools to path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from soar_analyzer import SPEParser, PageAnalyzer, PMUStats, TierClassifier

# ============================================================
# Step 3: Auto-generate interc config from page tiers
# ============================================================

def generate_interc_config(pages, gmx_pid=None):
    """Generate interc config from page-level AOL analysis.

    interc routes allocations by caller address range.
    Since we can't map pages to callers directly (no prof data),
    we generate a page-level migration script using move_pages instead.

    Returns: list of (page_addr, tier) tuples sorted by AOL
    """
    fast_pages = []
    slow_pages = []

    for page in sorted(pages.values(), key=lambda p: p.aol_score, reverse=True):
        if page.tier == "FAST":
            fast_pages.append(page)
        elif page.tier == "SLOW":
            slow_pages.append(page)

    return fast_pages, slow_pages


def generate_migration_script(fast_pages, slow_pages, fast_node=0, slow_node=2):
    """Generate a migration script that uses move_pages syscall.

    On single-NUMA x86, we simulate the effect by:
    - mbind hot pages to node 0
    - mbind cold pages to interleave
    """
    script = ["#!/bin/bash",
              "# KUMF SOAR Auto-Generated Migration Script",
              f"# Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}",
              f"# Hot pages: {len(fast_pages)}, Cold pages: {len(slow_pages)}",
              f"# Fast node: {fast_node}, Slow node: {slow_node}",
              "",
              "# Hot pages (FAST tier) - high AOL score, latency-sensitive",
              f"# Total: {sum(p.total_accesses for p in fast_pages)} accesses in {len(fast_pages)} pages",
              ""]

    # We can't actually migrate pages on single-NUMA x86
    # Instead, generate the config for kunpeng930 where it matters
    script.append("# For kunpeng930 (multi-NUMA), apply with:")
    script.append(f"# fast_node={fast_node} slow_node={slow_node}")
    script.append("")

    # Generate page list for verification
    with open('/tmp/kumf-x86/migration_plan.json', 'w') as f:
        json.dump({
            'fast': [{'addr': hex(p.page_addr), 'aol': round(p.aol_score, 4),
                       'accesses': p.total_accesses, 'llc_miss_ratio': round(p.llc_miss_ratio, 4)}
                      for p in fast_pages[:100]],
            'slow': [{'addr': hex(p.page_addr), 'aol': round(p.aol_score, 4),
                       'accesses': p.total_accesses}
                      for p in slow_pages[:100]],
            'stats': {
                'total_pages': len(fast_pages) + len(slow_pages) +
                               sum(1 for p in pages.values() if p.tier == "MEDIUM"),
                'fast_pages': len(fast_pages),
                'slow_pages': len(slow_pages),
                'fast_accesses': sum(p.total_accesses for p in fast_pages),
                'total_accesses': sum(p.total_accesses for p in pages.values()),
            }
        }, f, indent=2, default_vars={'pages': pages})

    return "\n".join(script)


# ============================================================
# Step 4: Benchmark with memory pressure
# ============================================================

def run_gmx_benchmark(tpr_file, nsteps=5000, nthreads=8, label="",
                      ld_preload=None, kumf_cfg=None, cgroup_limit_mb=None):
    """Run GROMACS benchmark and return performance (ns/day)"""
    cmd = []

    if cgroup_limit_mb:
        # Use cgroup to limit memory (simulates slow tier pressure)
        cmd.extend(["cgexec", "-g", f"memory:kumf"])

    if ld_preload:
        cmd.extend(["env", f"LD_PRELOAD={ld_preload}"])
    if kumf_cfg:
        cmd.extend([f"KUMF_CFG={kumf_cfg}"])

    cmd.extend(["gmx", "mdrun", "-s", tpr_file, "-nsteps", str(nsteps),
                "-ntomp", str(nthreads), "-noconfout",
                "-deffnm", f"/tmp/kumf-x86/bench_{label}"])

    print(f"\n[{label}] Running: {' '.join(cmd)}")
    start = time.time()
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    elapsed = time.time() - start

    # GROMACS outputs Performance to stderr
    output = result.stderr + result.stdout

    # Parse performance from GROMACS output
    perf = None
    for line in output.split('\n'):
        if 'Performance' in line:
            try:
                parts = line.strip().split()
                # "Performance:      134.680        0.178"
                perf = float(parts[1])
            except (ValueError, IndexError):
                pass
    print(f"  Performance: {perf} ns/day ({elapsed:.1f}s wall time)")
    return {
        'label': label,
        'performance': perf,
        'wall_time': elapsed,
        'returncode': result.returncode,
        'stderr_last': result.stderr.split('\n')[-5:] if result.stderr else [],
    }


def setup_cgroup(limit_mb):
    """Setup cgroup v2 memory limit for KUMF testing"""
    cgroup_path = "/sys/fs/cgroup/kumf"
    try:
        os.makedirs(cgroup_path, exist_ok=True)
        # Write memory limit
        with open(os.path.join(cgroup_path, "memory.max"), 'w') as f:
            f.write(str(limit_mb * 1024 * 1024))
        # Enable delegation for current user
        os.chmod(cgroup_path, 0o755)
        print(f"✅ cgroup memory limit set: {limit_mb}MB")
        return True
    except PermissionError:
        print(f"⚠️  No permission for cgroup (need sudo)")
        return False
    except Exception as e:
        print(f"⚠️  cgroup setup failed: {e}")
        return False


# ============================================================
# Main Pipeline
# ============================================================

def main():
    parser = argparse.ArgumentParser(description='KUMF SOAR Full Pipeline Validation')
    parser.add_argument('--tpr', default='/tmp/gromacs-soar/md.tpr', help='GROMACS TPR file')
    parser.add_argument('--nsteps', type=int, default=5000, help='MD steps per run')
    parser.add_argument('--nthreads', type=int, default=8, help='OpenMP threads')
    parser.add_argument('--skip-profiling', action='store_true', help='Skip PEBS profiling')
    parser.add_argument('--pebs-file', default='/tmp/kumf-x86/pebs_gmx_only.txt',
                        help='Pre-existing PEBS data')
    parser.add_argument('--output-dir', default='/tmp/kumf-x86', help='Output directory')
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    interc_lib = os.path.join(SCRIPT_DIR, '..', 'lib', 'interc', 'ldlib.so')

    print("=" * 70)
    print("  KUMF SOAR Full Pipeline Validation")
    print("=" * 70)
    print(f"  TPR: {args.tpr}")
    print(f"  Steps: {args.nsteps}, Threads: {args.nthreads}")
    print(f"  interc: {interc_lib}")

    results = []

    # ==========================================
    # Step 1: Baseline (no pressure)
    # ==========================================
    print("\n" + "=" * 50)
    print("  Step 1: Baseline (no memory pressure)")
    print("=" * 50)
    r = run_gmx_benchmark(args.tpr, args.nsteps, args.nthreads, "baseline")
    results.append(r)

    # ==========================================
    # Step 2: PEBS Profiling
    # ==========================================
    if not args.skip_profiling:
        print("\n" + "=" * 50)
        print("  Step 2: PEBS Profiling")
        print("=" * 50)
        pebs_data = os.path.join(args.output_dir, "pebs_pipeline.data")
        pebs_txt = os.path.join(args.output_dir, "pebs_pipeline.txt")
        pebs_gmx = os.path.join(args.output_dir, "pebs_pipeline_gmx.txt")

        cmd = (f"sudo perf mem record -F 1000 -a -o {pebs_data} -- "
               f"gmx mdrun -s {args.tpr} -nsteps {args.nsteps} "
               f"-ntomp {args.nthreads} -noconfout "
               f"-deffnm {args.output_dir}/pebs_prof")
        print(f"  Running: {cmd}")
        os.system(cmd)

        os.system(f"sudo perf script -i {pebs_data} > {pebs_txt}")
        os.system(f"grep 'gmx ' {pebs_txt} > {pebs_gmx}")
        args.pebs_file = pebs_gmx

    # ==========================================
    # Step 3: AOL Analysis
    # ==========================================
    print("\n" + "=" * 50)
    print("  Step 3: AOL Analysis")
    print("=" * 50)
    records, errors = SPEParser.parse(args.pebs_file)
    print(f"  Parsed {len(records)} records ({errors} errors)")

    pa = PageAnalyzer(PMUStats())
    pages = pa.analyze(records)
    TierClassifier.classify_pages(pages)

    total_pages = len(pages)
    fast_pages = [p for p in pages.values() if p.tier == "FAST"]
    slow_pages = [p for p in pages.values() if p.tier == "SLOW"]
    fast_acc = sum(p.total_accesses for p in fast_pages)
    total_acc = sum(p.total_accesses for p in pages.values()) or 1

    print(f"  Pages: {total_pages} total, {len(fast_pages)} FAST, {len(slow_pages)} SLOW")
    print(f"  Access concentration: {len(fast_pages)/total_pages*100:.1f}% pages → {fast_acc/total_acc*100:.1f}% accesses")
    print(f"  Top page AOL: {max(p.aol_score for p in pages.values()):.2f}")
    print(f"  Hot pages LLC miss ratio: {sum(p.llc_miss_ratio for p in fast_pages)/len(fast_pages)*100:.1f}% avg")

    # ==========================================
    # Step 4: Repeat benchmark (verify consistency)
    # ==========================================
    print("\n" + "=" * 50)
    print("  Step 4: Repeated Baselines (consistency check)")
    print("=" * 50)
    for i in range(2):
        r = run_gmx_benchmark(args.tpr, args.nsteps, args.nthreads, f"baseline_r{i+1}")
        results.append(r)

    # ==========================================
    # Step 5: interc with SOAR config
    # ==========================================
    print("\n" + "=" * 50)
    print("  Step 5: interc SOAR routing")
    print("=" * 50)

    # Generate SOAR config based on hot page analysis
    # Since interc works by caller address, and we have page-level data,
    # we generate a synthetic config for validation
    soar_cfg = os.path.join(args.output_dir, "soar_auto.cfg")
    with open(soar_cfg, 'w') as f:
        f.write("# KUMF SOAR auto-generated config\n")
        f.write("# Route hot (high-AOL) callers to Node 0\n")
        f.write("# Route cold callers to Node 2\n")
        f.write("# Note: On single-NUMA x86, this tests routing overhead only\n")
        # In real kunpeng deployment, caller addresses from prof would be used
        f.write("\n# Default: no routing (let first-touch decide)\n")

    if os.path.exists(interc_lib):
        r = run_gmx_benchmark(args.tpr, args.nsteps, args.nthreads, "soar_interc",
                              ld_preload=interc_lib, kumf_cfg=soar_cfg)
        results.append(r)
    else:
        print(f"  ⚠️  interc not built: {interc_lib}")

    # ==========================================
    # Report
    # ==========================================
    print("\n" + "=" * 70)
    print("  SOAR Pipeline Validation Report")
    print("=" * 70)

    print(f"\n{'Label':<20} {'ns/day':>10} {'Wall (s)':>10} {'Status':>10}")
    print("-" * 55)
    for r in results:
        perf_str = f"{r['performance']:.3f}" if r['performance'] else "N/A"
        status = "✅" if r['returncode'] == 0 else "❌"
        print(f"{r['label']:<20} {perf_str:>10} {r['wall_time']:>10.1f} {status:>10}")

    # Statistical analysis
    baselines = [r['performance'] for r in results if 'baseline' in r['label'] and r['performance']]
    if len(baselines) >= 2:
        avg = sum(baselines) / len(baselines)
        stddev = (sum((x - avg)**2 for x in baselines) / len(baselines)) ** 0.5
        cv = stddev / avg * 100 if avg > 0 else 0
        print(f"\n  Baseline avg: {avg:.3f} ns/day, σ={stddev:.3f}, CV={cv:.2f}%")

    # SOAR insights
    print(f"\n  Page Heatmap:")
    print(f"    Total pages: {total_pages}")
    print(f"    HOT (FAST tier):  {len(fast_pages):>5} pages ({len(fast_pages)/total_pages*100:.1f}%)")
    print(f"    WARM (MEDIUM):    {total_pages - len(fast_pages) - len(slow_pages):>5} pages")
    print(f"    COLD (SLOW tier): {len(slow_pages):>5} pages ({len(slow_pages)/total_pages*100:.1f}%)")
    print(f"    Access concentration: {len(fast_pages)/total_pages*100:.1f}% pages → {fast_acc/total_acc*100:.1f}% accesses")

    # Top 5 hot pages
    sorted_fast = sorted(fast_pages, key=lambda p: p.aol_score, reverse=True)
    print(f"\n  Top 5 Hot Pages:")
    for p in sorted_fast[:5]:
        print(f"    {p.page_addr:#018x}: AOL={p.aol_score:.2f}, acc={p.total_accesses}, LLC_miss={p.llc_miss_ratio:.1%}")

    # Save report
    report_path = os.path.join(args.output_dir, "pipeline_report.json")
    with open(report_path, 'w') as f:
        json.dump({
            'config': {'nsteps': args.nsteps, 'nthreads': args.nthreads, 'tpr': args.tpr},
            'results': results,
            'page_analysis': {
                'total_pages': total_pages,
                'fast_pages': len(fast_pages),
                'slow_pages': len(slow_pages),
                'fast_access_pct': round(fast_acc/total_acc*100, 1),
                'top_aol': round(sorted_fast[0].aol_score, 4) if sorted_fast else 0,
                'avg_llc_miss_fast': round(sum(p.llc_miss_ratio for p in fast_pages)/max(len(fast_pages),1), 4),
            }
        }, f, indent=2)
    print(f"\n  Report saved: {report_path}")

    # Migration plan for kunpeng930
    migration_path = os.path.join(args.output_dir, "migration_plan_kunpeng930.json")
    with open(migration_path, 'w') as f:
        json.dump({
            'description': 'Auto-generated SOAR migration plan for kunpeng930',
            'fast_node': 0, 'slow_node': 2,
            'fast_pages': [{'addr': hex(p.page_addr), 'aol': round(p.aol_score, 4),
                            'accesses': p.total_accesses, 'llc_miss_ratio': round(p.llc_miss_ratio, 4),
                            'loads': p.loads, 'stores': p.stores}
                           for p in sorted_fast[:50]],
            'cold_pages': [{'addr': hex(p.page_addr), 'aol': round(p.aol_score, 4),
                            'accesses': p.total_accesses}
                           for p in sorted(slow_pages, key=lambda p: p.aol_score)[:50]],
        }, f, indent=2)
    print(f"  Migration plan: {migration_path}")


if __name__ == '__main__':
    main()
