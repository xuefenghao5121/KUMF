#!/usr/bin/env python3
"""
KUMF SOAR Original Pipeline Adapter

适配原版 proc_obj_e.py 的分析逻辑到我们的数据格式。
不依赖 prof LD_PRELOAD，直接用 perf mem record 的 PEBS 数据。

关键改动:
  - 输入: perf script 输出 (PEBS) 替代原版 pebs:pebs 格式
  - 分析粒度: page-level (4KB) 替代 object-level
  - AOL 计算: 使用原版公式 (CYCLE_ACTIVITY.STALLS_L3_MISS / OFFCORE_REQUESTS)
  - 排名: 使用原版的 rank_objs_r 逻辑

输出: obj_stat.csv (page-level 排名), 迁移计划
"""

import sys
import os
import re
import csv
import math
import json
import numpy as np
from collections import defaultdict, OrderedDict
from bisect import bisect_left, bisect_right

PAGE_SHIFT = 12
PAGE_SIZE = 1 << PAGE_SHIFT

def read_pebs(filepath):
    """读取 perf script PEBS 输出，返回 [(time_ns, data_addr, event_type, latency_hint)]"""
    records = []
    errors = 0
    
    with open(filepath, 'r', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split()
            if len(parts) < 7:
                errors += 1
                continue
            
            try:
                # timestamp
                ts_str = parts[3].rstrip(':')
                time_ns = int(float(ts_str) * 1e9)  # sec → ns
                
                # data address
                addr_str = parts[6]
                if len(addr_str) < 12:
                    errors += 1
                    continue
                data_addr = int(addr_str, 16)
                if data_addr == 0:
                    errors += 1
                    continue
                
                # event type
                event = 'load' if 'mem-loads' in line else 'store'
                
                # Parse LVL for cache level
                lvl_match = re.search(r'\|LVL\s+([^|]+)\|', line)
                cache_level = 'hit'
                if lvl_match:
                    lvl = lvl_match.group(1).strip()
                    if 'L3 miss' in lvl:
                        cache_level = 'l3_miss'
                    elif 'RAM' in lvl:
                        cache_level = 'dram'
                    elif 'L1 hit' in lvl:
                        cache_level = 'l1_hit'
                    elif 'L2 hit' in lvl or 'L3 hit' in lvl:
                        cache_level = 'l2_l3_hit'
                    elif 'LFB' in lvl:
                        cache_level = 'lfb'
                
                records.append((time_ns, data_addr, event, cache_level))
            except (ValueError, IndexError):
                errors += 1
    
    print(f"PEBS: {len(records)} records loaded ({errors} errors)")
    return records


def compute_aol_timeseries(records, interval_ns=1_000_000_000):
    """
    原版 SOAR 的 AOL 计算:
      a_lat = CYCLE_ACTIVITY.STALLS_L3_MISS / OFFCORE_REQUESTS.DEMAND_DATA_RD
      est_dram_sd = (l3_stall/cyc) / (24.67/a_lat + 0.87)
    
    我们用 PEBS 数据近似:
      - l3_miss_count / total_loads ≈ l3_stall/cyc
      - a_lat ≈ 1/(l3_miss_ratio + eps) (高 miss ratio → 高延迟)
      - est_dram_sd = l3_miss_ratio * some_factor
    """
    if not records:
        return [], [], []
    
    # Sort by time
    records.sort(key=lambda x: x[0])
    min_time = records[0][0]
    max_time = records[-1][0]
    
    # Create time bins
    num_bins = max(1, int((max_time - min_time) / interval_ns))
    
    bins = []  # (time_start, loads, stores, l3_misses, dram_accesses)
    for i in range(num_bins):
        t_start = min_time + i * interval_ns
        t_end = t_start + interval_ns
        loads = 0
        stores = 0
        l3_misses = 0
        dram_accesses = 0
        
        # Binary search for records in this bin
        lo = bisect_left(records, (t_start, 0, '', ''))
        hi = bisect_right(records, (t_end, 0, '', ''))
        
        for j in range(lo, min(hi, len(records))):
            _, _, event, cache = records[j]
            if event == 'load':
                loads += 1
            else:
                stores += 1
            if cache in ('l3_miss', 'dram'):
                l3_misses += 1
            if cache == 'dram':
                dram_accesses += 1
        
        bins.append((t_start, loads, stores, l3_misses, dram_accesses))
    
    # Compute AOL metrics per bin (原版公式近似)
    a_lat = []
    est_dram_sd = []
    new_ts = []
    
    for t, loads, stores, l3_misses, dram in bins:
        total_accesses = loads + stores
        if total_accesses == 0:
            a_lat.append(0)
            est_dram_sd.append(0)
        else:
            l3_miss_ratio = l3_misses / max(loads, 1)
            # Approximate amortized offcore latency (原版用 PMU counter)
            # 高 miss ratio → 高 a_lat
            a_lat_val = 30 + l3_miss_ratio * 200  # 30ns baseline + miss contribution
            a_lat.append(a_lat_val)
            
            # Approximate DRAM stall density
            # est_dram_sd = (l3_stall/cyc) / (24.67/a_lat + 0.87)
            l3_stall_per_cyc = l3_miss_ratio * 0.5  # rough
            if a_lat_val > 0:
                sd = l3_stall_per_cyc / (24.67 / a_lat_val + 0.87)
            else:
                sd = 0
            est_dram_sd.append(sd)
        
        new_ts.append(t)
    
    return new_ts, a_lat, est_dram_sd


def rank_pages(records, new_ts, est_dram_sd, a_lat):
    """
    原版 rank_objs_r 的 page-level 版本。
    按原版逻辑计算 page 得分。
    """
    # Assign records to time bins
    records.sort(key=lambda x: x[0])
    
    # Build page → access counts per bin
    page_bins = defaultdict(lambda: defaultdict(int))  # page → bin_idx → count
    
    for i, t_start in enumerate(new_ts):
        t_end = t_start + (new_ts[1] - new_ts[0]) if i + 1 < len(new_ts) else t_start + 1e9
        lo = bisect_left(records, (t_start, 0, '', ''))
        hi = bisect_right(records, (t_end, 0, '', ''))
        
        for j in range(lo, min(hi, len(records))):
            _, addr, _, _ = records[j]
            page = addr >> PAGE_SHIFT
            page_bins[page][i] += 1
    
    # Rank using original SOAR formula
    all_pages = list(page_bins.keys())
    obj_scores = {}
    
    for page in all_pages:
        score = 0
        for i in range(len(new_ts)):
            cnt = page_bins[page].get(i, 0)
            # Total accesses in this bin
            all_acc = sum(page_bins[p].get(i, 0) for p in all_pages)
            if all_acc == 0:
                continue
            
            ratio = cnt / all_acc
            
            # Original SOAR MLP-aware scoring
            is_mlp = True
            if est_dram_sd[i] > 0:
                factor = 1
                min_ratio = 0.03
                max_ratio = 0.7
                if a_lat[i] <= 80 and a_lat[i] > 60:
                    factor = 2
                    min_ratio = 0.4
                    max_ratio = 0.6
                elif a_lat[i] <= 60 and a_lat[i] > 45:
                    factor = 4
                elif a_lat[i] > 40:
                    factor = 8
                else:
                    factor = 12
                
                if ratio >= max_ratio:
                    score += (ratio * est_dram_sd[i]) / factor
                elif ratio >= min_ratio:
                    score += (ratio * est_dram_sd[i])
                else:
                    score += (ratio * est_dram_sd[i]) * factor
            else:
                score += cnt
        
        obj_scores[page] = score
    
    return obj_scores


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 soar_original_adapter.py <pebs_file> [output_dir]")
        sys.exit(1)
    
    pebs_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "/tmp/soar-original"
    os.makedirs(output_dir, exist_ok=True)
    
    print("=" * 70)
    print("  KUMF SOAR Original Pipeline Adapter")
    print("=" * 70)
    
    # Step 1: Read PEBS
    print("\n[Step 1] Read PEBS data...")
    records = read_pebs(pebs_file)
    
    # Step 2: Compute AOL timeseries (原版公式)
    print("\n[Step 2] Compute AOL timeseries...")
    new_ts, a_lat, est_dram_sd = compute_aol_timeseries(records)
    print(f"  Time bins: {len(new_ts)}")
    if a_lat:
        print(f"  Avg AOL: {np.mean(a_lat):.2f}, Max: {max(a_lat):.2f}")
    
    # Step 3: Rank pages (原版 rank_objs_r)
    print("\n[Step 3] Rank pages (original SOAR formula)...")
    page_scores = rank_pages(records, new_ts, est_dram_sd, a_lat)
    
    # Sort by score
    ranked = sorted(page_scores.items(), key=lambda x: x[1], reverse=True)
    
    print(f"  Total pages: {len(ranked)}")
    print(f"  Top 5 pages:")
    for page, score in ranked[:5]:
        print(f"    {page:#018x}: score={score:.6f}")
    
    # Step 4: Classify tiers (原版逻辑)
    total_pages = len(ranked)
    fast_threshold = int(total_pages * 0.2)
    
    tier_data = []
    for i, (page, score) in enumerate(ranked):
        if i < fast_threshold:
            tier = "FAST"
        elif score > 0:
            tier = "MEDIUM"
        else:
            tier = "SLOW"
        tier_data.append({
            'page_addr': hex(page << PAGE_SHIFT),
            'page_key': hex(page),
            'score': score,
            'tier': tier,
            'rank': i + 1,
        })
    
    # Step 5: Output (原版格式)
    print("\n[Step 5] Generate output...")
    
    # obj_stat.csv (原版格式)
    csv_path = os.path.join(output_dir, "obj_stat.csv")
    with open(csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['rank', 'page_addr', 'score', 'tier'])
        for d in tier_data:
            writer.writerow([d['rank'], d['page_addr'], d['score'], d['tier']])
    print(f"  Written: {csv_path}")
    
    # obj_scores.csv
    scores_path = os.path.join(output_dir, "obj_scores.csv")
    with open(scores_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['obj_name', 'value'])
        for page, score in ranked:
            writer.writerow([hex(page), score])
    print(f"  Written: {scores_path}")
    
    # AOL CSV
    aol_path = os.path.join(output_dir, "obj_aol.csv")
    with open(aol_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['time', 'lat'])
        for t, lat in zip(new_ts, a_lat):
            writer.writerow([t, lat])
    print(f"  Written: {aol_path}")
    
    # Migration plan for interc
    fast_pages = [d for d in tier_data if d['tier'] == 'FAST']
    slow_pages = [d for d in tier_data if d['tier'] == 'SLOW']
    
    print(f"\n[Report]")
    print(f"  FAST:  {len(fast_pages)} pages")
    print(f"  MEDIUM: {len([d for d in tier_data if d['tier'] == 'MEDIUM'])} pages")
    print(f"  SLOW:  {len(slow_pages)} pages")
    
    # interc 配置 (需要 caller 地址，这里给出 page 范围)
    interc_path = os.path.join(output_dir, "interc_config.txt")
    with open(interc_path, 'w') as f:
        f.write("# SOAR interc config (page-level)\n")
        f.write(f"# FAST pages: {len(fast_pages)}\n")
        f.write(f"# Generated by soar_original_adapter.py\n\n")
        for d in fast_pages[:30]:
            f.write(f"# FAST: {d['page_addr']} score={d['score']:.6f}\n")
    print(f"  Written: {interc_path}")


if __name__ == '__main__':
    main()
