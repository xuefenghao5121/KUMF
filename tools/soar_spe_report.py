#!/usr/bin/env python3
"""
KUMF ARM SPE Report Parser — 从 perf report --mem-mode 输出提取 page-level AOL
+ prof 数据交叉关联 → 生成 interc size-based 配置

Pipeline:
  1. perf record -e arm_spe/... -- ./workload          → SPE 采集
  2. perf report --mem-mode --stdio > spe_report.txt    → 生成报告
  3. LD_PRELOAD=prof/ldlib.so ./workload                → prof 记录分配
  4. soar_spe_report.py --report ... --prof ... --config out.conf
     → 交叉关联: prof 的 addr 属于哪个 page → page 的 tier → 归纳为 size 规则

ARM SPE 在鲲鹏930上的特点：
- perf script 不输出数据地址（只有 PC）
- perf report --mem-mode 有数据地址和 cache 级别

用法:
  # 完整 pipeline (需要 prof 数据):
  python3 soar_spe_report.py --report spe_report.txt --prof /tmp/kumf/ --output aol.csv --config kumf_auto.conf

  # 仅分析 (不生成配置):
  python3 soar_spe_report.py --report spe_report.txt --output aol.csv
"""

import argparse
import os
import re
import csv
import sys
import subprocess
import struct
from collections import defaultdict
from dataclasses import dataclass, field

PAGE_SIZE = 4096  # 4KB pages


def detect_numa_topology():
    """Auto-detect NUMA topology from numactl -H or /sys.
    Returns (fast_nodes, slow_nodes, numa_distances).
    Strategy: same-socket nodes = fast, cross-socket = slow.
    """
    fast_nodes = [0]
    slow_nodes = [1]
    numa_distances = {}

    try:
        result = subprocess.run(['numactl', '-H'], capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            nodes = []
            distances = {}
            for line in result.stdout.strip().split('\n'):
                line = line.strip()
                if line.startswith('available:'):
                    parts = line.split()
                    if len(parts) >= 3 and '(' in parts[2]:
                        rng = parts[2].strip('()')
                        if '-' in rng:
                            start, end = map(int, rng.split('-'))
                            nodes = list(range(start, end+1))
                    if nodes:
                        continue

                if line.startswith('node ') and 'distance' in line:
                    parts = line.split()
                    try:
                        src = int(parts[1])
                    except ValueError:
                        continue
                    vals = []
                    for p in parts[2:]:
                        if p.endswith(':'):
                            continue
                        try:
                            vals.append(int(p))
                        except ValueError:
                            break
                    if vals:
                        distances[src] = vals

            if nodes and distances:
                local_dist = distances.get(nodes[0], [10])[0]
                fast_nodes = []
                slow_nodes = []
                for node_id in nodes:
                    d = distances.get(node_id, [999])[0]
                    if d <= local_dist:
                        fast_nodes.append(node_id)
                    else:
                        slow_nodes.append(node_id)
                numa_distances = distances
                print(f"NUMA topology: {len(nodes)} nodes, fast={fast_nodes}, slow={slow_nodes}")
                return fast_nodes, slow_nodes, numa_distances
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    try:
        sys_nodes = []
        for entry in os.listdir('/sys/devices/system/node/'):
            if entry.startswith('node') and entry[4:].isdigit():
                sys_nodes.append(int(entry[4:]))
        if sys_nodes:
            nodes = sorted(sys_nodes)
            fast_nodes = [nodes[0]]
            slow_nodes = nodes[1:2] if len(nodes) > 1 else []
            print(f"NUMA topology (fallback /sys): {len(nodes)} nodes, fast={fast_nodes}, slow={slow_nodes}")
    except (FileNotFoundError, ValueError):
        pass

    return fast_nodes, slow_nodes, numa_distances


@dataclass
class PageInfo:
    page_addr: int = 0
    accesses: int = 0
    l1d_misses: int = 0
    l3_hits: int = 0
    l3_misses: int = 0
    dram_accesses: int = 0
    l1d_hits: int = 0
    symbols: set = field(default_factory=set)
    aol_score: float = 0.0
    tier: str = ""


@dataclass
class ProfEntry:
    """One allocation record from prof /tmp/kumf/data.raw.TID"""
    caller: str = ""
    timestamp: int = 0
    size: int = 0
    addr: int = 0
    entry_type: int = 0  # 1=malloc, 2=free, >=100=mmap


def parse_report(filepath: str, target_dso: str = "kumf_tiered") -> dict:
    """Parse perf report --mem-mode --stdio output into page-level info"""
    pages = defaultdict(PageInfo)
    total_samples = 0
    target_samples = 0

    # 行格式:
    # 0.00%  0  L3 hit  [.] main  kumf_tiered  [.] 0x000040004aec5f20  [unknown]  N/A  Walker hit  No
    pattern = re.compile(
        r'\s*([\d.]+)%\s+'
        r'(\d+)\s+'
        r'(\S+(?:\s+\S+)?)\s+'
        r'\[.\]\s+'
        r'(\S+)\s+'
        r'(\S+)\s+'
        r'\[.\]\s+'
        r'(0x[0-9a-fA-F]+)\s+'
        r'(\S+)\s+'
        r'(\S+)\s+'
        r'(\S+(?:\s+\S+)?)\s+'
        r'(\S+)'
    )

    with open(filepath, 'r', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('Overhead'):
                continue

            m = pattern.match(line)
            if not m:
                continue

            overhead = float(m.group(1))
            weight = int(m.group(2))
            mem_access = m.group(3).strip()
            symbol = m.group(4)
            dso = m.group(5)
            data_addr_str = m.group(6)

            total_samples += 1

            if target_dso and target_dso not in dso:
                continue

            target_samples += 1

            try:
                data_addr = int(data_addr_str, 16)
            except ValueError:
                continue

            if data_addr == 0:
                continue

            page_addr = data_addr & ~(PAGE_SIZE - 1)

            page = pages[page_addr]
            page.page_addr = page_addr
            page.accesses += 1
            page.symbols.add(symbol)

            mem_lower = mem_access.lower()
            if 'l3 miss' in mem_lower or 'ram' in mem_lower:
                page.l3_misses += 1
                page.dram_accesses += 1
            elif 'l3 hit' in mem_lower:
                page.l3_hits += 1
            elif 'l1 miss' in mem_lower:
                page.l1d_misses += 1
            elif 'l1 hit' in mem_lower:
                page.l1d_hits += 1
            elif 'remote' in mem_lower:
                page.dram_accesses += 1
                page.l3_misses += 1

    print(f"Total samples: {total_samples}, Target DSO samples: {target_samples}")
    return pages


def parse_prof_dir(prof_dir: str) -> list:
    """Parse all /tmp/kumf/data.raw.* files from prof LD_PRELOAD"""
    entries = []
    if not os.path.isdir(prof_dir):
        print(f"WARNING: prof dir {prof_dir} not found")
        return entries

    for fname in sorted(os.listdir(prof_dir)):
        if not fname.startswith('data.raw.'):
            continue
        fpath = os.path.join(prof_dir, fname)
        with open(fpath, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                # Format: caller timestamp size addr entry_type
                # e.g.: main+0x11c 5289950202890 209715200 40003a2fb010 1
                parts = line.split()
                if len(parts) < 5:
                    continue
                try:
                    e = ProfEntry()
                    e.caller = parts[0]
                    e.timestamp = int(parts[1])
                    e.size = int(parts[2])
                    e.addr = int(parts[3], 16)
                    e.entry_type = int(parts[4])
                    # 只保留 malloc (type=1), 跳过 free(2) 和 mmap(>=100)
                    if e.entry_type == 1 and e.size > 0:
                        entries.append(e)
                except (ValueError, IndexError):
                    continue

    print(f"Loaded {len(entries)} malloc entries from prof")
    return entries


def compute_aol(pages: dict, top_pct: float = 0.2):
    """Compute AOL score and classify tiers.
    High aol_score = hot page (frequently accessed + high latency) → FAST tier.
    Low aol_score = cold page → SLOW tier.
    """
    if not pages:
        print("WARNING: No pages found!")
        return

    for page in pages.values():
        if page.accesses == 0:
            continue

        llc_miss_ratio = page.l3_misses / page.accesses if page.accesses > 0 else 0
        l1_miss_ratio = page.l1d_misses / page.accesses if page.accesses > 0 else 0
        dram_ratio = page.dram_accesses / page.accesses if page.accesses > 0 else 0

        # High aol_score = should be in FAST tier (hot + high latency)
        page.aol_score = (
            llc_miss_ratio * 2.0 +
            dram_ratio * 5.0 +
            l1_miss_ratio * 1.0 +
            (page.accesses / 100.0) * 0.5
        )

    # Sort by aol_score DESC: top pages are HOT → FAST
    sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)
    fast_count = max(1, int(len(sorted_pages) * top_pct))

    for i, page in enumerate(sorted_pages):
        if i < fast_count:
            page.tier = "FAST"
        else:
            page.tier = "SLOW"

    print(f"Classified {len(sorted_pages)} pages: {fast_count} FAST, {len(sorted_pages) - fast_count} SLOW")


def generate_config_with_prof(pages: dict, prof_entries: list, output_path: str,
                              fast_nodes: list, slow_nodes: list):
    """Cross-reference SPE page tiers with prof allocations → generate size-based interc config.

    Strategy:
    1. For each prof allocation, check what % of its pages are FAST vs SLOW
    2. Group allocations by (caller, size) → compute hot_ratio
    3. Generate size-based rules: hot allocations → fast_node, cold → slow_node
    """
    fast_node = fast_nodes[0] if fast_nodes else 0
    slow_node = slow_nodes[0] if slow_nodes else (fast_node + 1)

    # Build page_tier lookup: page_addr → tier
    page_tier = {}
    for addr, p in pages.items():
        page_tier[addr] = p.tier

    # For each prof allocation, compute hot page ratio
    alloc_info = []  # (caller, size, addr, hot_ratio, n_pages)
    for e in prof_entries:
        if e.size < PAGE_SIZE:
            continue  # Skip tiny allocations

        n_pages = (e.size + PAGE_SIZE - 1) // PAGE_SIZE
        n_fast = 0
        n_total = 0
        for page_idx in range(n_pages):
            pa = (e.addr + page_idx * PAGE_SIZE) & ~(PAGE_SIZE - 1)
            tier = page_tier.get(pa)
            if tier:
                n_total += 1
                if tier == "FAST":
                    n_fast += 1

        hot_ratio = n_fast / n_total if n_total > 0 else 0.5  # default if no SPE data
        alloc_info.append((e.caller, e.size, e.addr, hot_ratio, n_total))
        print(f"  alloc: caller={e.caller} size={e.size/(1024*1024):.1f}MB "
              f"hot_ratio={hot_ratio:.2f} (SPE pages: {n_total}/{n_pages})")

    if not alloc_info:
        print("WARNING: No prof allocations found, generating default config")
        with open(output_path, 'w') as f:
            f.write(f"# Default: all allocations to fast node\n")
            f.write(f"* = {fast_node}\n")
        return

    # Generate size-based rules with hot_ratio threshold
    # hot_ratio >= 0.5 → FAST (hot data)
    # hot_ratio < 0.5 → SLOW (cold data)
    HOT_THRESHOLD = 0.5

    # Group similar-sized allocations
    size_groups = defaultdict(lambda: {"hot": [], "cold": [], "callers": set()})
    for caller, size, addr, hot_ratio, n_spe_pages in alloc_info:
        size_mb = size // (1024 * 1024) * (1024 * 1024)  # Round to MB
        size_groups[size_mb]["callers"].add(caller)
        if hot_ratio >= HOT_THRESHOLD:
            size_groups[size_mb]["hot"].append(size)
        else:
            size_groups[size_mb]["cold"].append(size)

    with open(output_path, 'w') as f:
        f.write("# KUMF interc configuration - auto-generated from SOAR + prof cross-reference\n")
        f.write(f"# NUMA topology: fast={fast_nodes}, slow={slow_nodes}\n")
        f.write(f"# FAST tier → Node {fast_node} (same socket, low latency)\n")
        f.write(f"# SLOW tier → Node {slow_node} (cross socket, high latency)\n")
        f.write(f"# Hot ratio threshold: {HOT_THRESHOLD}\n\n")

        # Generate size_gt rules for hot allocations
        f.write(f"# ===== HOT allocations → Node {fast_node} =====\n")
        hot_sizes = set()
        for size_mb, info in sorted(size_groups.items(), reverse=True):
            if info["hot"]:
                hot_sizes.add(size_mb)
                size_str = f"{size_mb}" if size_mb >= 1024*1024 else f"{size_mb}"
                f.write(f"# size={size_mb/(1024*1024):.0f}MB callers={','.join(info['callers'])} hot_ratio={len(info['hot'])/(len(info['hot'])+len(info['cold'])):.2f}\n")
                f.write(f"size_gt:{size_mb - 1} = {fast_node}\n")

        f.write(f"\n# ===== COLD allocations → Node {slow_node} =====\n")
        cold_sizes = set()
        for size_mb, info in sorted(size_groups.items()):
            if info["cold"] and size_mb not in hot_sizes:
                cold_sizes.add(size_mb)
                f.write(f"# size={size_mb/(1024*1024):.0f}MB callers={','.join(info['callers'])} cold\n")
                f.write(f"size_range:{size_mb}-{size_mb + 1024*1024 - 1} = {slow_node}\n")

        # Default rule
        f.write(f"\n# ===== Default → Node {fast_node} =====\n")
        f.write(f"* = {fast_node}\n")

    print(f"\ninterc config written to {output_path}")
    print(f"  Hot sizes → Node {fast_node}: {[f'{s/(1024*1024):.0f}MB' for s in sorted(hot_sizes)]}")
    print(f"  Cold sizes → Node {slow_node}: {[f'{s/(1024*1024):.0f}MB' for s in sorted(cold_sizes)]}")


def generate_interc_config(pages: dict, output_path: str, fast_nodes=None, slow_nodes=None):
    """Legacy: generate page-address-based config (doesn't work with malloc-time routing)"""
    fast_pages = [p for p in pages.values() if p.tier == "FAST"]
    slow_pages = [p for p in pages.values() if p.tier == "SLOW"]

    if fast_nodes is None:
        fast_nodes = [0]
    if slow_nodes is None:
        slow_nodes = [1] if fast_nodes else [1]

    fast_node = fast_nodes[0]
    slow_node = slow_nodes[0] if slow_nodes else (fast_node + 1)

    with open(output_path, 'w') as f:
        f.write("# KUMF interc config - page-address-based (LEGACY, may not work with malloc routing)\n")
        f.write(f"# FAST tier → Node {fast_node}, SLOW tier → Node {slow_node}\n\n")

        f.write(f"# FAST tier (hot pages) → Node {fast_node}\n")
        for page in sorted(fast_pages, key=lambda p: p.page_addr):
            addr_start = page.page_addr
            addr_end = page.page_addr + PAGE_SIZE - 1
            f.write(f"0x{addr_start:016x}-0x{addr_end:016x} = {fast_node}\n")

        f.write(f"\n# SLOW tier (cold pages) → Node {slow_node}\n")
        for page in sorted(slow_pages, key=lambda p: p.page_addr):
            addr_start = page.page_addr
            addr_end = page.page_addr + PAGE_SIZE - 1
            f.write(f"0x{addr_start:016x}-0x{addr_end:016x} = {slow_node}\n")


def output_csv(pages: dict, output_path: str):
    sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)

    with open(output_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow([
            'page_addr', 'accesses', 'l1d_misses', 'l3_hits', 'l3_misses',
            'dram_accesses', 'aol_score', 'tier', 'symbols'
        ])
        for page in sorted_pages:
            writer.writerow([
                f"0x{page.page_addr:016x}",
                page.accesses,
                page.l1d_misses,
                page.l3_hits,
                page.l3_misses,
                page.dram_accesses,
                f"{page.aol_score:.4f}",
                page.tier,
                ';'.join(sorted(page.symbols))
            ])


def main():
    parser = argparse.ArgumentParser(description='KUMF ARM SPE Report Parser + prof cross-reference')
    parser.add_argument('--report', required=True, help='perf report --mem-mode --stdio output')
    parser.add_argument('--output', required=True, help='Output AOL CSV file')
    parser.add_argument('--config', default=None, help='Output interc config file')
    parser.add_argument('--prof', default=None, help='prof data directory (e.g. /tmp/kumf)')
    parser.add_argument('--dso', default='kumf_tiered', help='Target DSO name')
    parser.add_argument('--top-pct', type=float, default=0.2, help='Top %% pages for FAST tier')
    args = parser.parse_args()

    fast_nodes, slow_nodes, numa_distances = detect_numa_topology()

    print(f"Parsing {args.report} for DSO={args.dso}...")
    pages = parse_report(args.report, args.dso)

    if not pages:
        print("ERROR: No pages found. Check DSO name and report format.")
        sys.exit(1)

    print(f"Found {len(pages)} unique pages")
    compute_aol(pages, args.top_pct)
    output_csv(pages, args.output)
    print(f"AOL CSV written to {args.output}")

    fast = sum(1 for p in pages.values() if p.tier == "FAST")
    slow = sum(1 for p in pages.values() if p.tier == "SLOW")
    print(f"\nTier distribution: {fast} FAST, {slow} SLOW")

    sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)
    print("\nTop 10 hot pages:")
    for p in sorted_pages[:10]:
        print(f"  0x{p.page_addr:016x}  aol={p.aol_score:.3f}  acc={p.accesses}  "
              f"l3_miss={p.l3_misses}  dram={p.dram_accesses}  tier={p.tier}  "
              f"syms={';'.join(sorted(p.symbols))}")

    if args.config:
        if args.prof:
            # New: cross-reference with prof data → size-based config
            print(f"\nCross-referencing with prof data from {args.prof}...")
            prof_entries = parse_prof_dir(args.prof)
            generate_config_with_prof(pages, prof_entries, args.config, fast_nodes, slow_nodes)
        else:
            # Legacy: page-address-based (won't work for malloc routing)
            print("\nWARNING: No --prof provided, generating page-address-based config (may not work)")
            generate_interc_config(pages, args.config, fast_nodes, slow_nodes)


if __name__ == '__main__':
    main()
