#!/usr/bin/env python3
"""
pac_to_interc.py - 将 page 级 PAC 评分聚合为 interc 配置

输入:
  - page_pac.csv: spe_page_pac.py 的输出
  - prof 日志: LD_PRELOAD=prof.so 采集的分配记录 (可选)

输出:
  - kumf.conf: interc 可直接使用的配置文件

策略:
  1. 如果有 prof 日志：交叉关联 page PAC → 分配 caller/size → 生成 addr/size 规则
  2. 如果没有 prof 日志：按 page PAC 聚合连续地址范围 → 生成 page_range 规则
     (需要 interc 支持 page_range，当前版本不支持，会 fallback 到 size 估算)
"""

import sys
import os
import json
import argparse
from collections import defaultdict
from datetime import datetime


def load_page_pac(csv_path):
    """加载 page_pac.csv"""
    pages = []
    with open(csv_path, 'r') as f:
        header = f.readline()  # skip header
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 9:
                continue
            page_hex = parts[0]
            access_count = int(parts[1])
            avg_lat_tot = float(parts[2])
            max_lat_tot = int(parts[3])
            pac = float(parts[4])
            llc_refill_pct = float(parts[5])
            sve_pct = float(parts[6])
            tier = parts[8]
            pages.append({
                'page': page_hex,
                'page_addr': int(page_hex, 16),
                'access_count': access_count,
                'avg_lat_tot': avg_lat_tot,
                'max_lat_tot': max_lat_tot,
                'pac': pac,
                'llc_refill_pct': llc_refill_pct,
                'sve_pct': sve_pct,
                'tier': tier,
            })
    return pages


def aggregate_page_ranges(pages):
    """
    将同 tier 的连续 page 合并为地址范围
    
    返回: [(start_addr, end_addr, tier, page_count, total_access, avg_pac)]
    """
    if not pages:
        return []
    
    # 按 tier 分组，每组内按地址排序
    by_tier = defaultdict(list)
    for p in pages:
        by_tier[p['tier']].append(p)
    
    ranges = []
    for tier, tier_pages in by_tier.items():
        tier_pages.sort(key=lambda x: x['page_addr'])
        
        start = tier_pages[0]['page_addr']
        end = start + 0x1000  # 4KB per page
        page_count = 1
        total_access = tier_pages[0]['access_count']
        total_pac = tier_pages[0]['pac']
        
        for p in tier_pages[1:]:
            if p['page_addr'] == end:  # 连续
                end += 0x1000
                page_count += 1
                total_access += p['access_count']
                total_pac += p['pac']
            else:
                avg_pac = total_pac / page_count
                ranges.append((start, end, tier, page_count, total_access, avg_pac))
                start = p['page_addr']
                end = start + 0x1000
                page_count = 1
                total_access = p['access_count']
                total_pac = p['pac']
        
        avg_pac = total_pac / page_count if page_count > 0 else 0
        ranges.append((start, end, tier, page_count, total_access, avg_pac))
    
    return ranges


def load_prof_log(prof_path):
    """
    加载 prof 的输出日志
    
    prof 实际输出格式（从 ldlib.c 确认）:
    每行: caller_symbol timestamp size addr entry_type
    
    caller_symbol 格式:
      - "func_name+0xNN"  (dladdr 解析成功)
      - "[0xADDR]"        (dladdr 解析失败，原始地址)
      - "[init]"          (初始化阶段)
    
    entry_type: 0=free, 1=malloc, >=100=mmap
    
    示例行:
      main+0x34 12345678 209715200 400003ab9000 1
      [0x400003ab9ad0] 12345680 52428800 40000fc60000 100
    """
    allocations = []
    with open(prof_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split()
            if len(parts) < 5:
                continue
            try:
                caller_sym = parts[0]
                timestamp = int(parts[1])
                size = int(parts[2])
                # addr: 十六进制，可能不带 0x 前缀 (如 bc102a0)
                try:
                    addr = int(parts[3], 16)
                except ValueError:
                    addr = int(parts[3])
                entry_type = int(parts[4])
                
                # 提取 caller_addr：从 [0xADDR] 格式解析
                caller_addr = 0
                if caller_sym.startswith('[0x') and caller_sym.endswith(']'):
                    caller_addr = int(caller_sym[1:-1], 16)
                elif caller_sym.startswith('[init]'):
                    caller_addr = 0
                else:
                    # func_name+0xNN 格式，无法直接恢复地址，用符号名做 key
                    caller_addr = 0
                
                allocations.append({
                    'caller_sym': caller_sym,
                    'caller_addr': caller_addr,
                    'timestamp': timestamp,
                    'size': size,
                    'addr': addr,
                    'entry_type': entry_type,  # 0=free, 1=malloc, >=100=mmap
                })
            except (ValueError, IndexError):
                continue
    return allocations


def cross_correlate(pages, allocations):
    """
    交叉关联 page PAC 和 prof 分配记录
    
    对每个 allocation:
    1. 计算它覆盖的 page 范围 [addr, addr+size)
    2. 从 pages 中找到这些 page 的 PAC 评分
    3. 聚合为该 allocation 的总 PAC
    """
    # 建立 page → PAC 索引
    page_pac = {}
    for p in pages:
        page_pac[p['page_addr']] = p
    
    # 对每个 allocation 计算覆盖 page 的 PAC
    alloc_pac = []
    for alloc in allocations:
        if alloc['entry_type'] == 0:  # skip free
            continue
        if alloc['size'] < 4096:  # 太小，不关心
            continue
        
        addr_start = alloc['addr'] & ~0xFFF  # page 对齐
        addr_end = (alloc['addr'] + alloc['size'] + 0xFFF) & ~0xFFF
        
        total_pac = 0
        total_access = 0
        hot_pages = 0
        cold_pages = 0
        total_pages = 0
        
        for page_addr in range(addr_start, addr_end, 0x1000):
            total_pages += 1
            if page_addr in page_pac:
                p = page_pac[page_addr]
                total_pac += p['pac']
                total_access += p['access_count']
                if p['tier'] == 'HOT':
                    hot_pages += 1
                elif p['tier'] == 'COLD':
                    cold_pages += 1
        
        if total_pages == 0:
            continue
        
        avg_pac = total_pac / total_pages if total_pages > 0 else 0
        hot_ratio = hot_pages / total_pages if total_pages > 0 else 0
        
        # 用 caller_sym 作为 key（比 caller_addr 更稳定）
        alloc_pac.append({
            'caller_sym': alloc['caller_sym'],
            'caller_addr': alloc['caller_addr'],
            'size': alloc['size'],
            'addr': alloc['addr'],
            'total_pac': total_pac,
            'avg_pac': avg_pac,
            'total_access': total_access,
            'hot_pages': hot_pages,
            'cold_pages': cold_pages,
            'total_pages': total_pages,
            'hot_ratio': hot_ratio,
            'tier': 'HOT' if hot_ratio > 0.7 else ('COLD' if hot_ratio < 0.3 else 'WARM'),
        })
    
    return alloc_pac


def generate_interc_conf_page_ranges(ranges, output_path, fast_node=0, slow_node=2):
    """生成基于 page_range 的 interc 配置（需要 interc 支持 page_range）"""
    
    hot_ranges = [(s, e, pc, ta) for s, e, t, pc, ta, ap in ranges if t == 'HOT']
    warm_ranges = [(s, e, pc, ta) for s, e, t, pc, ta, ap in ranges if t == 'WARM']
    cold_ranges = [(s, e, pc, ta) for s, e, t, pc, ta, ap in ranges if t == 'COLD']
    
    with open(output_path, 'w') as f:
        f.write(f"# KUMF interc configuration (page-range based)\n")
        f.write(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"# Fast node: {fast_node} | Slow node: {slow_node}\n\n")
        
        # Hot pages → fast node
        f.write(f"# HOT pages: {sum(pc for _,_,pc,_ in hot_ranges)} pages in {len(hot_ranges)} ranges → node {fast_node}\n")
        for start, end, pc, ta in hot_ranges:
            size_mb = (end - start) / 1024 / 1024
            f.write(f"0x{start:x}-0x{end:x} = {fast_node}  # {pc} pages, {size_mb:.1f}MB, access={ta}\n")
        
        f.write(f"\n# WARM pages: {sum(pc for _,_,pc,_ in warm_ranges)} pages in {len(warm_ranges)} ranges → node {fast_node}\n")
        for start, end, pc, ta in warm_ranges:
            size_mb = (end - start) / 1024 / 1024
            f.write(f"0x{start:x}-0x{end:x} = {fast_node}  # {pc} pages, {size_mb:.1f}MB, access={ta}\n")
        
        f.write(f"\n# COLD pages: {sum(pc for _,_,pc,_ in cold_ranges)} pages in {len(cold_ranges)} ranges → node {slow_node}\n")
        for start, end, pc, ta in cold_ranges:
            size_mb = (end - start) / 1024 / 1024
            f.write(f"0x{start:x}-0x{end:x} = {slow_node}  # {pc} pages, {size_mb:.1f}MB, access={ta}\n")


def generate_interc_conf_size_based(ranges, output_path, fast_node=0, slow_node=2):
    """
    生成基于 size 的 interc 配置（当前 interc 原生支持）
    
    策略：从 page 范围推断分配大小模式
    - 大段连续 HOT pages → 可能是一个大热分配 → size_gt
    - 大段连续 COLD pages → 可能是一个大冷分配 → size_range
    """
    hot_ranges_list = [(s, e, pc, ta, ap) for s, e, t, pc, ta, ap in ranges if t == 'HOT']
    cold_ranges_list = [(s, e, pc, ta, ap) for s, e, t, pc, ta, ap in ranges if t == 'COLD']
    
    # 统计各 tier 的内存大小分布
    hot_sizes = [(e - s) for s, e, pc, ta, ap in hot_ranges_list]
    cold_sizes = [(e - s) for s, e, pc, ta, ap in cold_ranges_list]
    
    total_hot_mb = sum(hot_sizes) / 1024 / 1024
    total_cold_mb = sum(cold_sizes) / 1024 / 1024
    
    # 找到最常见的大小段（模拟 kumf_tiered 的分配模式）
    # kumf_tiered 有 3 个分配：masses(200MB), positions(100MB), velocities(50MB)
    # HOT 的是 masses+positions(300MB)，COLD 的是 velocities(50MB)
    
    with open(output_path, 'w') as f:
        f.write(f"# KUMF interc configuration (size-based, estimated from PAC)\n")
        f.write(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"# Fast node: {fast_node} | Slow node: {slow_node}\n")
        f.write(f"# HOT memory: {total_hot_mb:.1f} MB in {len(hot_ranges_list)} ranges\n")
        f.write(f"# COLD memory: {total_cold_mb:.1f} MB in {len(cold_ranges_list)} ranges\n\n")
        
        # 如果有 prof 交叉关联数据，可以精确到 caller/size
        # 否则，用启发式：大的连续段是热数据，小的是冷数据
        
        # 按 range 大小排序，大的放 fast，小的放 slow
        all_ranges_sorted = sorted(
            [(s, e, t, pc, ta, ap) for s, e, t, pc, ta, ap in ranges],
            key=lambda x: x[1] - x[0],  # by size
            reverse=True
        )
        
        # 找 size 分界点：累计内存量前 N% 是热数据
        cumulative = 0
        total_mem = sum(e - s for s, e, t, pc, ta, ap in all_ranges_sorted)
        
        size_threshold = None
        for s, e, t, pc, ta, ap in all_ranges_sorted:
            cumulative += (e - s)
            if cumulative >= total_mem * 0.7:  # 70% 内存量
                size_threshold = e - s
                break
        
        if size_threshold:
            f.write(f"# Auto-detected hot/cold size threshold: {size_threshold/1024/1024:.1f} MB\n")
            f.write(f"# Allocations larger than this → fast node (hot data)\n")
            f.write(f"# Allocations smaller than this → slow node (cold data)\n\n")
            f.write(f"size_gt:{size_threshold} = {fast_node}  # HOT: large allocations\n")
            f.write(f"size_lt:{size_threshold} = {slow_node}  # COLD: small allocations\n")
        else:
            f.write("# Could not determine size threshold\n")


def generate_interc_conf_alloc_based(alloc_pac, output_path, fast_node=0, slow_node=2):
    """生成基于 prof 交叉关联的 interc 配置（最精确）"""
    
    # 按 caller_sym 聚合
    by_caller = defaultdict(lambda: {'sizes': [], 'total_pac': 0, 'count': 0, 'hot_count': 0})
    for a in alloc_pac:
        key = a['caller_sym']
        by_caller[key]['sizes'].append(a['size'])
        by_caller[key]['total_pac'] += a['avg_pac']
        by_caller[key]['count'] += 1
        if a['tier'] == 'HOT':
            by_caller[key]['hot_count'] += 1
    
    with open(output_path, 'w') as f:
        f.write(f"# KUMF interc configuration (prof cross-correlated)\n")
        f.write(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"# Fast node: {fast_node} | Slow node: {slow_node}\n\n")
        
        # 每个 caller 的典型大小和冷热判定
        for caller, info in sorted(by_caller.items(), key=lambda x: -x[1]['total_pac']):
            avg_pac = info['total_pac'] / info['count'] if info['count'] > 0 else 0
            hot_ratio = info['hot_count'] / info['count'] if info['count'] > 0 else 0
            tier = 'HOT' if hot_ratio > 0.7 else ('COLD' if hot_ratio < 0.3 else 'WARM')
            node = fast_node if tier in ('HOT', 'WARM') else slow_node
            typical_size = max(info['sizes'])
            
            f.write(f"# caller={caller} avg_PAC={avg_pac:.0f} tier={tier} ({info['count']} allocs, hot_ratio={hot_ratio:.1%})\n")
            
            # 如果有 caller_addr，用 addr_rule
            if info.get('caller_addr') and info['caller_addr'] != 0:
                ca = info['caller_addr']
                f.write(f"0x{ca:x}-0x{ca:x} = {node}  # {caller} size~{typical_size/1024/1024:.0f}MB {tier}\n")
            else:
                # 用 name_rule
                f.write(f"{caller} = {node}  # size~{typical_size/1024/1024:.0f}MB {tier}\n")


def main():
    parser = argparse.ArgumentParser(description='KUMF PAC → interc config generator')
    parser.add_argument('--pac-csv', required=True, help='page_pac.csv from spe_page_pac.py')
    parser.add_argument('--prof-log', default=None, help='prof allocation log (optional, for cross-correlation)')
    parser.add_argument('--output', '-o', default='kumf.conf', help='Output interc config file')
    parser.add_argument('--fast-node', type=int, default=0, help='NUMA node for hot data')
    parser.add_argument('--slow-node', type=int, default=2, help='NUMA node for cold data')
    parser.add_argument('--mode', choices=['page_range', 'size', 'alloc'], default='size',
                        help='Config generation mode: page_range (需要 interc 支持), size (当前可用), alloc (需要 prof)')
    args = parser.parse_args()
    
    print(f"🦐 KUMF PAC → interc config generator")
    print(f"   PAC CSV: {args.pac_csv}")
    print(f"   Mode: {args.mode}")
    print(f"   Fast node: {args.fast_node} | Slow node: {args.slow_node}")
    
    pages = load_page_pac(args.pac_csv)
    print(f"   Loaded {len(pages)} pages")
    
    if args.mode == 'alloc' and args.prof_log:
        # 最精确：prof 交叉关联
        allocations = load_prof_log(args.prof_log)
        print(f"   Loaded {len(allocations)} prof allocations")
        alloc_pac = cross_correlate(pages, allocations)
        print(f"   Cross-correlated {len(alloc_pac)} allocations")
        generate_interc_conf_alloc_based(alloc_pac, args.output, args.fast_node, args.slow_node)
    
    elif args.mode == 'page_range':
        # page 地址范围路由
        ranges = aggregate_page_ranges(pages)
        print(f"   Aggregated into {len(ranges)} page ranges")
        generate_interc_conf_page_ranges(ranges, args.output, args.fast_node, args.slow_node)
    
    else:
        # size-based 估算
        ranges = aggregate_page_ranges(pages)
        print(f"   Aggregated into {len(ranges)} page ranges")
        generate_interc_conf_size_based(ranges, args.output, args.fast_node, args.slow_node)
    
    print(f"\n✅ Config generated: {args.output}")


if __name__ == '__main__':
    main()
