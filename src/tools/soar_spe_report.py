#!/usr/bin/env python3
"""
KUMF ARM SPE Report Parser — 从 perf report --mem-mode 输出提取 page-level AOL

ARM SPE 在鲲鹏930上的特点：
- perf script 不输出数据地址（只有 PC）
- perf report --mem-mode 有数据地址和 cache 级别
- 846K+ 样本量足够做 page-level 分析

用法:
  perf report -i spe.data --mem-mode --stdio > spe_report.txt
  python3 soar_spe_report.py --report spe_report.txt --output aol.csv

输出:
  page_addr,accesses,llc_misses,l1d_misses,dram_accesses,avg_aol,aol_score,tier,symbols
"""

import argparse
import os
import re
import csv
import sys
import subprocess
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

    # Method 1: parse numactl -H
    try:
        result = subprocess.run(['numactl', '-H'], capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            nodes = []
            distances = {}
            current_node = None
            for line in result.stdout.strip().split('\n'):
                line = line.strip()
                if line.startswith('available:'):
                    # Parse available nodes from "available: 4 nodes (0-3)"
                    parts = line.split()
                    if len(parts) >= 3 and '(' in parts[2]:
                        rng = parts[2].strip('()')
                        if '-' in rng:
                            start, end = map(int, rng.split('-'))
                            nodes = list(range(start, end+1))
                    if nodes:
                        continue
                
                # Parse "node X distance: Y Y Y Y ..."
                if line.startswith('node ') and 'distance' in line:
                    parts = line.split()
                    src = int(parts[1])
                    vals = list(map(int, parts[3:]))
                    distances[src] = vals
            
            if nodes and distances:
                # Find local distance for first node
                local_dist = distances.get(nodes[0], [10])[0]
                fast_nodes = []
                slow_nodes = []
                for node_id in nodes:
                    d = distances.get(node_id, [999])[0]
                    if d <= local_dist:
                        fast_nodes.append(node_id)
                    elif d > local_dist:
                        slow_nodes.append(node_id)
                numa_distances = distances
                print(f"NUMA topology: {len(nodes)} nodes, fast={fast_nodes}, slow={slow_nodes}")
                return fast_nodes, slow_nodes, numa_distances
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # Method 2: parse /sys/devices/system/node/has_cpu
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

def parse_report(filepath: str, target_dso: str = "kumf_tiered") -> dict:
    """Parse perf report --mem-mode --stdio output"""
    pages = defaultdict(PageInfo)
    total_samples = 0
    target_samples = 0
    
    # 行格式:
    # 0.00%  0  L3 hit  [.] main  kumf_tiered  [.] 0x000040004aec5f20  [unknown]  N/A  Walker hit  No
    pattern = re.compile(
        r'\s*([\d.]+)%\s+'       # Overhead
        r'(\d+)\s+'              # Local Weight
        r'(\S+(?:\s+\S+)?)\s+'   # Memory access (e.g. "L3 hit", "L3 miss", "L1 miss")
        r'\[.\]\s+'              # [.] or [k]
        r'(\S+)\s+'              # Symbol
        r'(\S+)\s+'              # Shared Object (DSO)
        r'\[.\]\s+'              # [.] or [k]
        r'(0x[0-9a-fA-F]+)\s+'   # Data Symbol (data virtual address)
        r'(\S+)\s+'              # Data Object
        r'(\S+)\s+'              # Snoop
        r'(\S+(?:\s+\S+)?)\s+'   # TLB access
        r'(\S+)'                 # Locked
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
            
            # 只处理目标 DSO
            if target_dso and target_dso not in dso:
                continue
            
            target_samples += 1
            
            # 解析数据地址
            try:
                data_addr = int(data_addr_str, 16)
            except ValueError:
                continue
            
            if data_addr == 0:
                continue
            
            # 对齐到 page
            page_addr = data_addr & ~(PAGE_SIZE - 1)
            
            # 计算 sample 数量 (overhead 反映比例)
            # 每行代表一个 unique record，overhead 反映占比
            # 简化：每行算 1 次访问
            page = pages[page_addr]
            page.page_addr = page_addr
            page.accesses += 1
            page.symbols.add(symbol)
            
            # 按 Memory access 类型分类
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

def compute_aol(pages: dict, top_pct: float = 0.2):
    """Compute AOL score and classify tiers"""
    if not pages:
        print("WARNING: No pages found!")
        return
    
    for page in pages.values():
        if page.accesses == 0:
            continue
        
        # AOL score = (llc_miss_ratio * dram_penalty) * access_frequency
        llc_miss_ratio = page.l3_misses / page.accesses if page.accesses > 0 else 0
        l1_miss_ratio = page.l1d_misses / page.accesses if page.accesses > 0 else 0
        dram_ratio = page.dram_accesses / page.accesses if page.accesses > 0 else 0
        
        # 综合评分：LLC miss 权重高，DRAM 更高
        # 频繁访问 + 高延迟 = 应该放快层
        page.aol_score = (
            llc_miss_ratio * 2.0 +      # LLC miss 代价中等
            dram_ratio * 5.0 +           # DRAM access 代价最高
            l1_miss_ratio * 1.0 +        # L1 miss 代价低
            (page.accesses / 100.0) * 0.5 # 访问频率加权
        )
    
    # 按 aol_score 排序，top 20% → FAST tier
    sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)
    fast_count = max(1, int(len(sorted_pages) * top_pct))
    
    for i, page in enumerate(sorted_pages):
        if i < fast_count:
            page.tier = "FAST"
        else:
            page.tier = "SLOW"
    
    print(f"Classified {len(sorted_pages)} pages: {fast_count} FAST, {len(sorted_pages) - fast_count} SLOW")

def generate_interc_config(pages: dict, output_path: str, fast_nodes: list = None, slow_nodes: list = None):
    """Generate interc configuration from tier classification"""
    fast_pages = [p for p in pages.values() if p.tier == "FAST"]
    slow_pages = [p for p in pages.values() if p.tier == "SLOW"]

    if fast_nodes is None:
        fast_nodes = [0]
    if slow_nodes is None:
        slow_nodes = [1] if fast_nodes else [1]
    
    fast_node = fast_nodes[0]
    slow_node = slow_nodes[0] if slow_nodes else (fast_node + 1)

    with open(output_path, 'w') as f:
        f.write("# KUMF interc configuration - auto-generated from SOAR SPE analysis\n")
        f.write(f"# NUMA topology: fast={fast_nodes}, slow={slow_nodes}\n")
        f.write(f"# FAST tier → Node {fast_node} (local, low latency)\n")
        f.write(f"# SLOW tier → Node {slow_node} (remote, high latency)\n\n")

        f.write(f"# ===== FAST tier (hot pages) → Node {fast_node} =====\n")
        for page in sorted(fast_pages, key=lambda p: p.page_addr):
            addr_start = page.page_addr
            addr_end = page.page_addr + PAGE_SIZE - 1
            f.write(f"0x{addr_start:016x}-0x{addr_end:016x} = {fast_node}\n")

        f.write(f"\n# ===== SLOW tier (cold pages) → Node {slow_node} =====\n")
        for page in sorted(slow_pages, key=lambda p: p.page_addr):
            addr_start = page.page_addr
            addr_end = page.page_addr + PAGE_SIZE - 1
            f.write(f"0x{addr_start:016x}-0x{addr_end:016x} = {slow_node}\n")

def output_csv(pages: dict, output_path: str):
    """Output page-level AOL CSV"""
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
    parser = argparse.ArgumentParser(description='KUMF ARM SPE Report Parser')
    parser.add_argument('--report', required=True, help='perf report --mem-mode --stdio output')
    parser.add_argument('--output', required=True, help='Output AOL CSV file')
    parser.add_argument('--config', default=None, help='Output interc config file')
    parser.add_argument('--dso', default='kumf_tiered', help='Target DSO name')
    parser.add_argument('--top-pct', type=float, default=0.2, help='Top %% pages for FAST tier')
    args = parser.parse_args()
    
    # Detect NUMA topology
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
    
    # 统计
    fast = sum(1 for p in pages.values() if p.tier == "FAST")
    slow = sum(1 for p in pages.values() if p.tier == "SLOW")
    print(f"\nTier distribution: {fast} FAST, {slow} SLOW")
    
    # Top 10 热页
    sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)
    print("\nTop 10 hot pages:")
    for p in sorted_pages[:10]:
        print(f"  0x{p.page_addr:016x}  aol={p.aol_score:.3f}  acc={p.accesses}  "
              f"l3_miss={p.l3_misses}  dram={p.dram_accesses}  tier={p.tier}  "
              f"syms={';'.join(sorted(p.symbols))}")
    
    if args.config:
        generate_interc_config(pages, args.config, fast_nodes, slow_nodes)
        print(f"\ninterc config written to {args.config}")

if __name__ == '__main__':
    main()
