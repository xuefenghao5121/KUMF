#!/usr/bin/env python3
"""
KUMF Phase 2: SPE Data Parser + AOL Scoring Engine
For Kunpeng 930 (ARM SPEv1p1 - no latency/data_source fields)

Pipeline:
  1. Parse perf script SPE output → structured records
  2. Parse prof LD_PRELOAD output → allocation records
  3. Match SPE memory accesses to allocations (by VA range)
  4. Collect PMU stats for MLP estimation
  5. Compute AOL score per object

Usage:
  perf record -e arm_spe_0// -a -- ./workload
  perf script > spe_output.txt
  python3 spe_aol_parser.py --spe spe_output.txt --prof /tmp/kumf/data.raw.* --pmu pmu_stats.txt
"""

import argparse
import re
import sys
import os
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Optional, Dict, List, Tuple

# ---- Data Structures ----

@dataclass
class SPERecord:
    """One SPE sample"""
    timestamp: float = 0.0
    pid: int = 0
    tid: int = 0
    cpu: int = 0
    event_type: str = ""  # l1d-access, l1d-miss, llc-access, llc-miss, memory, branch-miss, tlb-access
    va: int = 0           # virtual address (data)
    pc: int = 0           # program counter (instruction)
    symbol: str = ""      # resolved symbol
    dso: str = ""         # dynamic shared object

@dataclass
class AllocRecord:
    """One allocation from prof/ldlib.so"""
    timestamp: int = 0    # nanoseconds
    size: int = 0
    addr: int = 0         # virtual address of allocation
    alloc_type: int = 0   # 0=free, 1=malloc, 2=munmap, >=100=mmap
    callchain: List[str] = field(default_factory=list)
    caller_symbol: str = ""

@dataclass
class ObjectStats:
    """Statistics for one identified object"""
    caller: str = ""
    total_size: int = 0
    alloc_count: int = 0
    # SPE access stats
    total_accesses: int = 0
    l1d_hits: int = 0
    l1d_misses: int = 0
    llc_accesses: int = 0
    llc_misses: int = 0
    memory_accesses: int = 0  # goes to DRAM
    branch_misses: int = 0
    tlb_accesses: int = 0
    # PMU-derived
    mlp_approx: float = 0.0
    # Computed scores
    llc_miss_ratio: float = 0.0
    aol_score: float = 0.0
    hotness: float = 0.0  # access frequency

@dataclass
class PMUStats:
    """Global PMU counter stats"""
    bus_access: int = 0
    llc_loads: int = 0
    llc_load_misses: int = 0
    l1d_cache: int = 0
    l1d_miss: int = 0
    stalled_backend: int = 0
    cycles: int = 0
    instructions: int = 0


# ---- SPE Parser ----

# Format: comm TID [CPU] TIMESTAMP: PERIOD EVENT_TYPE: VA SYMBOL (DSO)
# Note: VA may or may not have 0x prefix. No separate PID - TID is the only ID.
SPE_PATTERN = re.compile(
    r'\s*\S+\s+(\d+)\s+\[(\d+)\]\s+([\d.]+):\s+(\d+)\s+'
    r'([\w-]+):\s+([0-9a-fA-F]+)\s+'
    r'(.+?)\s+\((.+?)\)'
)

def parse_spe_file(filepath: str) -> List[SPERecord]:
    """Parse perf script output from SPE recording"""
    records = []
    parse_errors = 0
    
    with open(filepath, 'r', errors='replace') as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('Only') or line.startswith('Warning'):
                continue
            
            m = SPE_PATTERN.match(line)
            if not m:
                parse_errors += 1
                continue
            
            try:
                rec = SPERecord(
                    pid=int(m.group(1)),  # TID
                    tid=int(m.group(1)),
                    cpu=int(m.group(2)),
                    timestamp=float(m.group(3)),
                    event_type=m.group(5),
                    va=int(m.group(6), 16),
                    pc=0,
                    symbol=m.group(7).strip(),
                    dso=m.group(8).strip(),
                )
                records.append(rec)
            except (ValueError, IndexError):
                parse_errors += 1
    
    print(f"SPE: parsed {len(records)} records ({parse_errors} parse errors)")
    return records


# ---- Prof Parser ----

def parse_prof_file(filepath: str) -> List[AllocRecord]:
    """Parse prof/ldlib.so output format:
    caller [addr] timestamp size addr type [callchain...]
    """
    records = []
    
    with open(filepath, 'r', errors='replace') as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) < 6:
                continue
            
            try:
                # caller is parts[0], [addr] is parts[1]
                timestamp = int(parts[2])
                size = int(parts[3])
                addr_str = parts[4]
                addr = int(addr_str, 16) if addr_str.startswith('0x') or addr_str.startswith('ffff') else int(addr_str)
                alloc_type = int(parts[5])
                callchain = parts[6:] if len(parts) > 6 else []
                
                rec = AllocRecord(
                    timestamp=timestamp,
                    size=size,
                    addr=addr,
                    alloc_type=alloc_type,
                    callchain=callchain,
                )
                # Extract caller symbol
                if callchain:
                    for cc in callchain:
                        if '(' in cc:
                            sym = cc.split('(')[1].split('+')[0].split(')')[0]
                            if sym:
                                rec.caller_symbol = sym
                                break
                records.append(rec)
            except (ValueError, IndexError):
                continue
    
    print(f"PROF: parsed {len(records)} allocation records from {filepath}")
    return records


# ---- Address Matching ----

def build_alloc_ranges(allocs: List[AllocRecord]) -> List[Tuple[int, int, str]]:
    """Build sorted list of (start, end, caller) for binary search"""
    ranges = []
    for a in allocs:
        if a.alloc_type == 1 and a.size > 0:  # malloc
            ranges.append((a.addr, a.addr + a.size, a.caller_symbol))
    ranges.sort()
    return ranges

def find_object(va: int, ranges: List[Tuple[int, int, str]]) -> Optional[str]:
    """Binary search for which allocation contains this VA"""
    lo, hi = 0, len(ranges) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        start, end, caller = ranges[mid]
        if va < start:
            hi = mid - 1
        elif va >= end:
            lo = mid + 1
        else:
            return caller if caller else f"obj_{start:#x}"
    return None


# ---- AOL Scoring ----

def compute_object_scores(
    spe_records: List[SPERecord], 
    alloc_ranges: List[Tuple[int, int, str]],
    pmu: PMUStats,
) -> Dict[str, ObjectStats]:
    """Compute per-object statistics and AOL scores"""
    
    objects: Dict[str, ObjectStats] = defaultdict(ObjectStats)
    unmatched = 0
    
    for rec in spe_records:
        # Only process memory-related events
        if rec.event_type not in ('l1d-access', 'l1d-miss', 'llc-access', 'llc-miss', 
                                   'memory', 'branch-miss', 'tlb-access'):
            continue
        
        # Find which allocation this address belongs to
        obj_name = find_object(rec.va, alloc_ranges)
        if not obj_name:
            unmatched += 1
            continue
        
        obj = objects[obj_name]
        obj.caller = obj_name
        obj.total_accesses += 1
        
        if rec.event_type == 'l1d-access':
            obj.l1d_hits += 1
        elif rec.event_type == 'l1d-miss':
            obj.l1d_misses += 1
        elif rec.event_type == 'llc-access':
            obj.llc_accesses += 1
        elif rec.event_type == 'llc-miss':
            obj.llc_misses += 1
        elif rec.event_type == 'memory':
            obj.memory_accesses += 1
        elif rec.event_type == 'branch-miss':
            obj.branch_misses += 1
        elif rec.event_type == 'tlb-access':
            obj.tlb_accesses += 1
    
    print(f"Matched SPE records: {sum(o.total_accesses for o in objects.values())} / "
          f"{sum(1 for r in spe_records if r.event_type in ('l1d-access','l1d-miss','llc-access','llc-miss','memory'))} "
          f"({unmatched} unmatched)")
    
    # Compute global MLP
    mlp_global = 0.0
    if pmu.stalled_backend > 0:
        mlp_global = pmu.bus_access / pmu.stalled_backend
    
    # Score each object
    for name, obj in objects.items():
        # LLC miss ratio (how often L3 misses → goes to DRAM/remote)
        if obj.llc_accesses > 0:
            obj.llc_miss_ratio = obj.llc_misses / obj.llc_accesses
        elif obj.l1d_misses > 0:
            # Approximate: if no llc_access events, use l1d_miss as proxy
            obj.llc_miss_ratio = obj.memory_accesses / max(obj.l1d_misses, 1)
        
        # MLP approximation for this object
        # High memory_accesses with few llc_misses → high MLP (good parallelism)
        if obj.llc_misses > 0:
            obj.mlp_approx = obj.memory_accesses / obj.llc_misses
        else:
            obj.mlp_approx = mlp_global if mlp_global > 0 else 1.0
        
        # AOL = LLC_miss_ratio / MLP (higher = more latency-sensitive)
        if obj.mlp_approx > 0:
            obj.aol_score = obj.llc_miss_ratio / obj.mlp_approx
        
        # Hotness = total accesses (normalized)
        obj.hotness = obj.total_accesses
    
    return objects


# ---- PMU Parser ----

def parse_pmu_file(filepath: str) -> PMUStats:
    """Parse perf stat output"""
    pmu = PMUStats()
    
    patterns = {
        'bus_access': (r'[\d,]+\.?\d*\s+bus_access', 'bus_access'),
        'LLC-loads': (r'[\d,]+\.?\d*\s+LLC-loads', 'llc_loads'),
        'LLC-load-misses': (r'[\d,]+\.?\d*\s+LLC-load-misses', 'llc_load_misses'),
        'l1d_cache': (r'[\d,]+\.?\d*\s+l1d_cache', 'l1d_cache'),
        'l1d_cache_lmiss_rd': (r'[\d,]+\.?\d*\s+l1d_cache_lmiss_rd', 'l1d_miss'),
        'stalled-cycles-backend': (r'[\d,]+\.?\d*\s+stalled-cycles-backend', 'stalled_backend'),
        'cycles': (r'[\d,]+\.?\d*\s+cycles', 'cycles'),
        'instructions': (r'[\d,]+\.?\d*\s+instructions', 'instructions'),
    }
    
    with open(filepath, 'r') as f:
        for line in f:
            for name, (pattern, attr) in patterns.items():
                m = re.search(pattern, line)
                if m:
                    val_str = line.strip().split()[0].replace(',', '')
                    try:
                        setattr(pmu, attr, int(val_str))
                    except ValueError:
                        pass
    
    return pmu


# ---- Main ----

def main():
    parser = argparse.ArgumentParser(description='KUMF SPE+AOL Analysis')
    parser.add_argument('--spe', required=True, help='perf script SPE output file')
    parser.add_argument('--prof', nargs='+', required=True, help='prof data.raw files')
    parser.add_argument('--pmu', help='perf stat PMU output file')
    parser.add_argument('--output', default='obj_stat.csv', help='Output CSV file')
    parser.add_argument('--pid', type=int, default=0, help='Filter by PID (0=all)')
    args = parser.parse_args()
    
    print("=" * 60)
    print("  KUMF Phase 2: SPE+AOL Analysis Engine")
    print("=" * 60)
    
    # Step 1: Parse SPE data
    print("\n--- Step 1: Parse SPE data ---")
    spe_records = parse_spe_file(args.spe)
    
    if args.pid > 0:
        spe_records = [r for r in spe_records if r.pid == args.pid]
        print(f"Filtered to PID {args.pid}: {len(spe_records)} records")
    
    # Step 2: Parse prof data
    print("\n--- Step 2: Parse allocation data ---")
    all_allocs = []
    for prof_file in args.prof:
        all_allocs.extend(parse_prof_file(prof_file))
    
    # Only keep large allocs (>4KB)
    large_allocs = [a for a in all_allocs if a.size > 4096]
    print(f"Large allocations (>4KB): {len(large_allocs)}")
    
    alloc_ranges = build_alloc_ranges(large_allocs)
    print(f"Address ranges built: {len(alloc_ranges)}")
    
    # Step 3: Parse PMU
    pmu = PMUStats()
    if args.pmu:
        print("\n--- Step 3: Parse PMU stats ---")
        pmu = parse_pmu_file(args.pmu)
        print(f"  bus_access: {pmu.bus_access:,}")
        print(f"  LLC-loads: {pmu.llc_loads:,}")
        print(f"  LLC-load-misses: {pmu.llc_load_misses:,}")
        print(f"  stalled_backend: {pmu.stalled_backend:,}")
        print(f"  cycles: {pmu.cycles:,}")
        if pmu.stalled_backend > 0:
            print(f"  MLP_approx (bus_access/stalled): {pmu.bus_access/pmu.stalled_backend:.4f}")
    
    # Step 4: Compute scores
    print("\n--- Step 4: Compute AOL scores ---")
    objects = compute_object_scores(spe_records, alloc_ranges, pmu)
    
    # Step 5: Output results
    print(f"\n--- Step 5: Output results ({args.output}) ---")
    
    # Sort by AOL score descending
    sorted_objects = sorted(objects.items(), key=lambda x: x[1].aol_score, reverse=True)
    
    with open(args.output, 'w') as f:
        f.write("object,alloc_count,total_size,total_accesses,l1d_hits,l1d_misses,"
                "llc_accesses,llc_misses,memory_accesses,llc_miss_ratio,mlp_approx,"
                "aol_score,hotness,recommendation\n")
        
        for name, obj in sorted_objects:
            # Recommendation based on AOL score
            if obj.aol_score > 0.1:
                rec = "FAST_TIER"  # latency-sensitive → fast tier
            elif obj.aol_score > 0.01:
                rec = "MEDIUM"
            else:
                rec = "SLOW_TIER"  # high MLP, not latency-sensitive → slow tier
            
            f.write(f"{name},{obj.alloc_count},{obj.total_size},{obj.total_accesses},"
                    f"{obj.l1d_hits},{obj.l1d_misses},{obj.llc_accesses},{obj.llc_misses},"
                    f"{obj.memory_accesses},{obj.llc_miss_ratio:.6f},{obj.mlp_approx:.4f},"
                    f"{obj.aol_score:.6f},{obj.hotness:.1f},{rec}\n")
    
    # Print summary
    print(f"\n{'Object':<45} {'Accesses':>10} {'LLC_miss%':>10} {'MLP':>8} {'AOL':>10} {'Rec':>12}")
    print("-" * 100)
    for name, obj in sorted_objects[:20]:
        rec = "FAST" if obj.aol_score > 0.1 else ("MED" if obj.aol_score > 0.01 else "SLOW")
        print(f"{name[:45]:<45} {obj.total_accesses:>10} {obj.llc_miss_ratio:>10.4f} "
              f"{obj.mlp_approx:>8.4f} {obj.aol_score:>10.6f} {rec:>12}")
    
    print(f"\n✅ Output written to {args.output}")
    print(f"   Total objects: {len(objects)}")
    fast = sum(1 for _, o in objects.items() if o.aol_score > 0.1)
    slow = sum(1 for _, o in objects.items() if o.aol_score <= 0.01)
    med = len(objects) - fast - slow
    print(f"   FAST_TIER: {fast}, MEDIUM: {med}, SLOW_TIER: {slow}")


if __name__ == '__main__':
    main()
