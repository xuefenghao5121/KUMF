#!/usr/bin/env python3
"""
SOAR Phase 2: Page-level heatmap analysis from PEBS data
按论文设计：SPE/PEBS → VA → 对齐到4KB page → 统计热度 → AOL评分
"""
import sys
from collections import defaultdict

def parse_pebs_script(input_file):
    """解析 perf script 的 PEBS 输出，提取 data address"""
    page_stats = defaultdict(lambda: {"loads": 0, "stores": 0, "latency_sum": 0, "latency_max": 0})
    total_samples = 0
    gmx_samples = 0
    
    with open(input_file) as f:
        for line in f:
            if "gmx" not in line:
                continue
            gmx_samples += 1
            
            # 格式: gmx PID [CPU]: WEIGHT EVENT: DATA_ADDR ...
            parts = line.split()
            if len(parts) < 8:
                continue
            
            # 找到 data address（冒号后的第一个字段）
            try:
                colon_idx = None
                for i, p in enumerate(parts):
                    if p.endswith(":") and i > 2:
                        colon_idx = i
                        break
                if colon_idx is None:
                    continue
                
                addr_str = parts[colon_idx + 1]
                addr = int(addr_str, 16)
                
                if addr == 0:
                    continue
                    
                total_samples += 1
                
                # Page-aligned (4KB)
                page = addr >> 12
                
                # 判断 load/store
                is_load = "mem-loads" in line
                is_store = "mem-stores" in line
                
                # 提取 latency（weight 字段）
                weight = int(parts[4]) if parts[4].isdigit() else 0
                
                stats = page_stats[page]
                if is_load:
                    stats["loads"] += 1
                if is_store:
                    stats["stores"] += 1
                stats["latency_sum"] += weight
                stats["latency_max"] = max(stats["latency_max"], weight)
                
            except (ValueError, IndexError):
                continue
    
    return page_stats, total_samples, gmx_samples

def compute_aol_scores(page_stats):
    """计算每个 page 的 AOL 评分（论文公式简化版）"""
    results = []
    for page, stats in page_stats.items():
        total_accesses = stats["loads"] + stats["stores"]
        avg_latency = stats["latency_sum"] / total_accesses if total_accesses > 0 else 0
        
        # AOL = Access_count × (1 - hit_rate_proxy) × latency_factor
        # 简化：用 avg_latency 作为 locality 代理
        # 高频 + 高延迟 = 热点（需要放快层）
        # 高频 + 低延迟 = 已经在缓存里（可以放慢层）
        
        aol_score = total_accesses * (1 + avg_latency / 100.0)
        
        results.append({
            "page": page,
            "page_addr": f"0x{page << 12:x}",
            "loads": stats["loads"],
            "stores": stats["stores"],
            "total": total_accesses,
            "avg_latency": avg_latency,
            "max_latency": stats["latency_max"],
            "aol_score": aol_score
        })
    
    results.sort(key=lambda x: x["aol_score"], reverse=True)
    return results

def classify_pages(results, fast_ratio=0.3):
    """分类 pages: FAST_TIER (热) vs SLOW_TIER (冷)"""
    total_pages = len(results)
    fast_count = max(1, int(total_pages * fast_ratio))
    
    for i, r in enumerate(results):
        if i < fast_count:
            r["tier"] = "FAST"
        else:
            r["tier"] = "SLOW"
    
    return results

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 page_heatmap.py <perf_script_output>")
        sys.exit(1)
    
    input_file = sys.argv[1]
    
    print("=" * 60)
    print("  SOAR Phase 2: Page-Level Heatmap Analysis")
    print("=" * 60)
    
    page_stats, total_samples, gmx_samples = parse_pebs_script(input_file)
    results = compute_aol_scores(page_stats)
    results = classify_pages(results)
    
    print(f"\nGMX samples: {gmx_samples}")
    print(f"Valid data addr samples: {total_samples}")
    print(f"Unique pages: {len(results)}")
    
    # 统计
    fast_pages = [r for r in results if r["tier"] == "FAST"]
    slow_pages = [r for r in results if r["tier"] == "SLOW"]
    fast_accesses = sum(r["total"] for r in fast_pages)
    slow_accesses = sum(r["total"] for r in slow_pages)
    
    print(f"\n--- Tier Classification ---")
    print(f"FAST tier: {len(fast_pages)} pages, {fast_accesses} accesses ({100*fast_accesses/(fast_accesses+slow_accesses):.1f}%)")
    print(f"SLOW tier: {len(slow_pages)} pages, {slow_accesses} accesses ({100*slow_accesses/(fast_accesses+slow_accesses):.1f}%)")
    
    print(f"\n--- Top 30 HOT Pages (FAST_TIER candidates) ---")
    print(f"{'Page Addr':<20} {'Loads':>6} {'Stores':>6} {'Total':>6} {'AvgLat':>7} {'MaxLat':>7} {'AOL':>10} {'Tier'}")
    print("-" * 80)
    for r in results[:30]:
        print(f"{r['page_addr']:<20} {r['loads']:>6} {r['stores']:>6} {r['total']:>6} {r['avg_latency']:>7.1f} {r['max_latency']:>7} {r['aol_score']:>10.1f} {r['tier']}")
    
    print(f"\n--- Top 10 COLD Pages (SLOW_TIER candidates) ---")
    print(f"{'Page Addr':<20} {'Loads':>6} {'Stores':>6} {'Total':>6} {'AvgLat':>7} {'MaxLat':>7} {'AOL':>10} {'Tier'}")
    print("-" * 80)
    for r in results[-10:]:
        print(f"{r['page_addr']:<20} {r['loads']:>6} {r['stores']:>6} {r['total']:>6} {r['avg_latency']:>7.1f} {r['max_latency']:>7} {r['aol_score']:>10.1f} {r['tier']}")
    
    # 输出 SOAR 配置
    print(f"\n--- SOAR Migration Plan ---")
    print(f"# Auto-generated by SOAR Phase 2")
    print(f"# Hot pages → FAST tier, Cold pages → SLOW tier")
    print(f"#")
    print(f"# Top hot pages (migrate to fast tier):")
    for r in results[:10]:
        print(f"#   {r['page_addr']} ({r['total']} accesses, AOL={r['aol_score']:.0f})")
    
    # 保存 CSV
    csv_file = input_file.replace(".data", "_aol.csv")
    with open(csv_file, "w") as f:
        f.write("page_addr,loads,stores,total,avg_latency,max_latency,aol_score,tier\n")
        for r in results:
            f.write(f"{r['page_addr']},{r['loads']},{r['stores']},{r['total']},{r['avg_latency']:.1f},{r['max_latency']},{r['aol_score']:.1f},{r['tier']}\n")
    print(f"\nCSV saved to: {csv_file}")

if __name__ == "__main__":
    main()
