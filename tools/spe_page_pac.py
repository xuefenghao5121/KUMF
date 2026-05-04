#!/usr/bin/env python3
"""
spe_page_pac.py - 从 perf report -D 输出中提取 page 级 PAC 评分

用法:
  # 流式 pipe（边读边解析，内存友好）
  perf report -D -i perf.data | python3 spe_page_pac.py

  # 限制处理行数（快速验证）
  perf report -D -i perf.data | head -5000000 | python3 spe_page_pac.py

  # 从已有文件读取
  cat perf_raw.txt | python3 spe_page_pac.py

输出:
  - page_pac.csv:  page地址,访问次数,平均LAT_TOT,PAC评分
  - pac_summary.txt: 人类可读的 PAC 分析报告
  - kumf_report.json: 机器可读 JSON
  - kumf.conf: 自动生成的 interc 配置

SPE 记录结构（从鲲鹏930 perf report -D 实测确认）:

  典型一条完整记录（min_latency=32 过滤后）:
    PC  0x40000fc33d00 el0 ns=1        ← 指令地址
    LAT 90 ISSUE                       ← 发射延迟
    LAT 95 TOT                         ← ⭐ 总延迟（核心字段）
    EV  RETIRED TLB-ACCESS             ← 事件类型
    ST  GP-REG                         ← 操作类型
    PAD
    VA  0x40000fc59b90                 ← ⭐ 数据虚拟地址
    LAT 1 XLAT                         ← TLB 翻译延迟
    CONTEXT 0xe463b el2
    PAD
    TS  1664985821392                  ← 时间戳（记录结束标志）

  含 LLC miss 的记录:
    PC  0x40000fc2eb80
    LAT 8 ISSUE
    LAT 269 TOT                        ← ⭐ DRAM 访问 ~269 cycles
    EV  RETIRED L1D-ACCESS L1D-REFILL TLB-ACCESS LLC-ACCESS LLC-REFILL
    LD  GP-REG
    PAD
    VA  0x40000fc60e8c
    LAT 1 XLAT
    LAT 257 (0x9e)                     ← LLC 相关延迟
    PAD
    CONTEXT
    PAD
    TS

  SVE 操作记录:
    LD  SIMD-FP                        ← SVE 向量 load
"""

import sys
import os
import re
import json
import argparse
from collections import defaultdict
from datetime import datetime


# 正则预编译
RE_PC = re.compile(r'PC\s+(0x[0-9a-fA-F]+)')
RE_VA = re.compile(r'VA\s+(0x[0-9a-fA-F]+)')
RE_LAT = re.compile(r'LAT\s+(\d+)\s*(ISSUE|TOT|XLAT)?')
RE_EV = re.compile(r'EV\s+(.+)')
RE_OP = re.compile(r'(LD|ST)\s+(GP-REG|SIMD-FP|SVE|NEON)')
RE_TS = re.compile(r'TS\s+(\d+)')
RE_SOURCE = re.compile(r'(L1D-ACCESS|L1D-REFILL|TLB-ACCESS|TLB-REFILL|LLC-ACCESS|LLC-REFILL|REMOTE-ACCESS|RETIRED|NOT-RETIRED)')
RE_AUXTRACE = re.compile(r'PERF_RECORD_AUXTRACE')
RE_MMAP = re.compile(r'PERF_RECORD_MMAP')


def parse_stream(stream, max_records=None):
    """
    流式解析 perf report -D 输出。
    
    记录边界策略:
    - 跳过非 SPE 数据（MMAP, SWITCH, AUX, ITRACE 等）
    - SPE 数据以 AUXTRACE header 开始，后面跟着 packet 流
    - 每条 SPE 记录以 TS (timestamp) 结尾，或以新 PC 开头
    - 用 TS 作为最可靠的记录结束标志
    """
    
    page_stats = defaultdict(lambda: {
        'access_count': 0,
        'lat_tot_sum': 0,
        'lat_tot_max': 0,
        'lat_issue_sum': 0,
        'lat_xlat_sum': 0,
        'lat_9e_sum': 0,
        'lat_9e_count': 0,
        'llc_refill_count': 0,
        'llc_access_count': 0,
        'tlb_refill_count': 0,
        'ld_count': 0,
        'st_count': 0,
        'sve_count': 0,
    })
    
    in_spe_section = False
    current_sample = {}
    records_parsed = 0
    total_lines = 0
    
    for line in stream:
        line = line.strip()
        total_lines += 1
        
        if not line:
            continue
        
        # 检测 SPE 数据区域开始
        if RE_AUXTRACE.search(line):
            in_spe_section = True
            continue
        
        # 跳过非 SPE 数据
        if RE_MMAP.search(line) or line.startswith('PERF_RECORD_SWITCH') or \
           line.startswith('PERF_RECORD_ITRACE'):
            in_spe_section = False
            continue
        
        if not in_spe_section and not _is_spe_packet(line):
            continue
        
        in_spe_section = True
        
        # === 解析各 packet ===
        
        # TS → 记录结束，flush
        ts_match = RE_TS.search(line)
        if ts_match:
            if _sample_is_valid(current_sample):
                _flush_sample(current_sample, page_stats)
                records_parsed += 1
                if max_records and records_parsed >= max_records:
                    break
            current_sample = {}
            continue
        
        # PC → 新记录开始（如果在上一条记录之后）
        pc_match = RE_PC.search(line)
        if pc_match:
            if _sample_is_valid(current_sample):
                _flush_sample(current_sample, page_stats)
                records_parsed += 1
                if max_records and records_parsed >= max_records:
                    break
            current_sample = {'pc': pc_match.group(1)}
            continue
        
        # VA → 数据虚拟地址
        va_match = RE_VA.search(line)
        if va_match:
            current_sample['va'] = va_match.group(1)
            continue
        
        # LAT → 延迟字段
        lat_match = RE_LAT.search(line)
        if lat_match:
            lat_val = int(lat_match.group(1))
            lat_type = lat_match.group(2)
            
            if lat_type == 'TOT':
                current_sample['lat_tot'] = lat_val
            elif lat_type == 'ISSUE':
                current_sample['lat_issue'] = lat_val
            elif lat_type == 'XLAT':
                current_sample['lat_xlat'] = lat_val
            else:
                # 0x9e 的 LAT 类型（无标签，通常是 LLC 相关延迟）
                current_sample['lat_9e'] = lat_val
            continue
        
        # EV → 事件类型
        ev_match = RE_EV.search(line)
        if ev_match:
            ev_str = ev_match.group(1)
            current_sample['ev'] = ev_str
            # 提取关键事件
            if 'LLC-REFILL' in ev_str:
                current_sample['llc_refill'] = True
            if 'LLC-ACCESS' in ev_str:
                current_sample['llc_access'] = True
            if 'TLB-REFILL' in ev_str:
                current_sample['tlb_refill'] = True
            continue
        
        # OP → 操作类型
        op_match = RE_OP.search(line)
        if op_match:
            op_type = op_match.group(1)
            op_reg = op_match.group(2)
            current_sample['op'] = f"{op_type}_{op_reg}"
            if op_type == 'LD':
                current_sample['is_load'] = True
            elif op_type == 'ST':
                current_sample['is_store'] = True
            if op_reg in ('SIMD-FP', 'SVE', 'NEON'):
                current_sample['is_sve'] = True
            continue
    
    # Flush 最后一条
    if _sample_is_valid(current_sample):
        _flush_sample(current_sample, page_stats)
        records_parsed += 1
    
    return page_stats, records_parsed, total_lines


def _is_spe_packet(line):
    """判断一行是否是 SPE packet"""
    return bool(RE_PC.search(line) or RE_VA.search(line) or 
                 RE_LAT.search(line) or RE_EV.search(line) or 
                 RE_OP.search(line) or RE_TS.search(line))


def _sample_is_valid(sample):
    """一条有效的 SPE 记录至少要有 VA"""
    return 'va' in sample


def _flush_sample(sample, page_stats):
    """将一条样本聚合到 page 级别"""
    try:
        va = int(sample['va'], 16)
    except (ValueError, KeyError):
        return
    
    # 4KB page 对齐
    page = va & ~0xFFF
    page_hex = hex(page)
    
    stats = page_stats[page_hex]
    stats['access_count'] += 1
    
    if 'lat_tot' in sample:
        lat = sample['lat_tot']
        stats['lat_tot_sum'] += lat
        stats['lat_tot_max'] = max(stats['lat_tot_max'], lat)
    
    if 'lat_issue' in sample:
        stats['lat_issue_sum'] += sample['lat_issue']
    
    if 'lat_xlat' in sample:
        stats['lat_xlat_sum'] += sample['lat_xlat']
    
    if 'lat_9e' in sample:
        stats['lat_9e_sum'] += sample['lat_9e']
        stats['lat_9e_count'] += 1
    
    if sample.get('llc_refill'):
        stats['llc_refill_count'] += 1
    if sample.get('llc_access'):
        stats['llc_access_count'] += 1
    if sample.get('tlb_refill'):
        stats['tlb_refill_count'] += 1
    
    if sample.get('is_load'):
        stats['ld_count'] += 1
    if sample.get('is_store'):
        stats['st_count'] += 1
    if sample.get('is_sve'):
        stats['sve_count'] += 1


def compute_pac_scores(page_stats, mlp_estimate=1.0):
    """
    计算 page 级 PAC 评分
    
    PAC(page) = access_count × avg_latency / MLP
    
    高 PAC → 访问多 + 延迟大 + MLP 低 → 必须放快层
    低 PAC → 访问少 或 MLP 高（如 SVE）→ 可以放慢层
    """
    pac_scores = {}
    
    for page_hex, stats in page_stats.items():
        if stats['access_count'] == 0:
            continue
        
        avg_lat_tot = stats['lat_tot_sum'] / stats['access_count'] if stats['lat_tot_sum'] > 0 else 0
        
        # SVE 感知 MLP 调整：SVE 向量 load 天然高 MLP，降低 PAC
        sve_ratio = stats['sve_count'] / stats['access_count'] if stats['access_count'] > 0 else 0
        effective_mlp = mlp_estimate * (1 + sve_ratio * 3)  # SVE load MLP 约 3-4x 标量
        
        # PAC = access_count × avg_latency / effective_MLP
        pac = stats['access_count'] * avg_lat_tot / effective_mlp
        
        pac_scores[page_hex] = {
            'page': page_hex,
            'access_count': stats['access_count'],
            'avg_lat_tot': round(avg_lat_tot, 1),
            'max_lat_tot': stats['lat_tot_max'],
            'lat_tot_sum': stats['lat_tot_sum'],
            'pac': round(pac, 1),
            'llc_refill_pct': round(stats['llc_refill_count'] / stats['access_count'] * 100, 1) if stats['access_count'] else 0,
            'sve_pct': round(stats['sve_count'] / stats['access_count'] * 100, 1) if stats['access_count'] else 0,
            'ld_st_ratio': f"{stats['ld_count']}:{stats['st_count']}",
        }
    
    return pac_scores


def detect_knee(pac_scores):
    """
    自动检测 PAC 分布的拐点（冷热分界线）
    
    使用 K-means 思路：找到将 pages 分成 3 类的最优分割点
    """
    if not pac_scores:
        return 0.5, 0.5, 0.5
    
    # 按 PAC 降序排序
    sorted_pages = sorted(pac_scores.values(), key=lambda x: x['pac'], reverse=True)
    pac_values = [p['pac'] for p in sorted_pages]
    
    if len(pac_values) < 3:
        mid = pac_values[len(pac_values)//2] if pac_values else 0
        return mid, mid * 0.5, mid * 0.25
    
    # 方法：按累计访问量找到 80/20 分界线
    total_access = sum(p['access_count'] for p in sorted_pages)
    
    cumulative = 0
    hot_idx = len(pac_values)  # 默认全热
    for i, p in enumerate(sorted_pages):
        cumulative += p['access_count']
        if cumulative >= total_access * 0.8:
            hot_idx = i + 1
            break
    
    warm_idx = len(pac_values)
    for i, p in enumerate(sorted_pages):
        cumulative += p['access_count']
        if cumulative >= total_access * 0.95:
            warm_idx = i + 1
            break
    
    hot_threshold = sorted_pages[min(hot_idx - 1, len(sorted_pages) - 1)]['pac']
    warm_threshold = sorted_pages[min(warm_idx - 1, len(sorted_pages) - 1)]['pac']
    cold_threshold = sorted_pages[-1]['pac']
    
    return hot_threshold, warm_threshold, cold_threshold


def generate_report(pac_scores, hot_th, warm_th, cold_th, records_parsed, total_lines, output_dir):
    """生成人类可读报告 + CSV + JSON + kumf.conf"""
    
    os.makedirs(output_dir, exist_ok=True)
    
    # 按 PAC 降序排序
    sorted_pages = sorted(pac_scores.values(), key=lambda x: x['pac'], reverse=True)
    
    # 分类
    hot_pages = [p for p in sorted_pages if p['pac'] >= hot_th]
    warm_pages = [p for p in sorted_pages if warm_th <= p['pac'] < hot_th]
    cold_pages = [p for p in sorted_pages if p['pac'] < warm_th]
    
    total_access = sum(p['access_count'] for p in sorted_pages)
    hot_access = sum(p['access_count'] for p in hot_pages)
    warm_access = sum(p['access_count'] for p in warm_pages)
    cold_access = sum(p['access_count'] for p in cold_pages)
    
    # === CSV 输出 ===
    csv_path = os.path.join(output_dir, 'page_pac.csv')
    with open(csv_path, 'w') as f:
        f.write('page,access_count,avg_lat_tot,max_lat_tot,pac,llc_refill_pct,sve_pct,ld_st_ratio,tier\n')
        for p in sorted_pages:
            tier = 'HOT' if p['pac'] >= hot_th else ('WARM' if p['pac'] >= warm_th else 'COLD')
            f.write(f"{p['page']},{p['access_count']},{p['avg_lat_tot']},{p['max_lat_tot']},{p['pac']},{p['llc_refill_pct']},{p['sve_pct']},{p['ld_st_ratio']},{tier}\n")
    
    # === JSON 输出 ===
    json_path = os.path.join(output_dir, 'kumf_report.json')
    report_json = {
        'version': '0.1.0',
        'timestamp': datetime.now().isoformat(),
        'sampling': {
            'method': 'arm_spe',
            'records_parsed': records_parsed,
            'total_lines': total_lines,
            'pages_total': len(sorted_pages),
        },
        'pac_thresholds': {
            'hot': hot_th,
            'warm': warm_th,
            'cold': cold_th,
        },
        'summary': {
            'hot_pages': len(hot_pages),
            'warm_pages': len(warm_pages),
            'cold_pages': len(cold_pages),
            'hot_access_pct': round(hot_access / total_access * 100, 1) if total_access else 0,
            'warm_access_pct': round(warm_access / total_access * 100, 1) if total_access else 0,
            'cold_access_pct': round(cold_access / total_access * 100, 1) if total_access else 0,
            'hot_memory_mb': len(hot_pages) * 4 / 256,   # 4KB per page
            'warm_memory_mb': len(warm_pages) * 4 / 256,
            'cold_memory_mb': len(cold_pages) * 4 / 256,
        },
        'top_pages': sorted_pages[:100],
    }
    with open(json_path, 'w') as f:
        json.dump(report_json, f, indent=2, default=str)
    
    # === 人类可读报告 ===
    report_path = os.path.join(output_dir, 'pac_summary.txt')
    with open(report_path, 'w') as f:
        f.write("=" * 70 + "\n")
        f.write("  KUMF SPE Page-Level PAC Analysis Report\n")
        f.write(f"  Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write("=" * 70 + "\n\n")
        
        f.write(f"Input lines processed:  {total_lines:,}\n")
        f.write(f"SPE records parsed:     {records_parsed:,}\n")
        f.write(f"Unique pages:           {len(sorted_pages):,}\n")
        f.write(f"Total memory accesses:  {total_access:,}\n")
        f.write(f"Memory footprint:       {len(sorted_pages) * 4 / 1024:.1f} MB ({len(sorted_pages)} pages × 4KB)\n\n")
        
        f.write("-" * 70 + "\n")
        f.write("  PAC Thresholds (auto-detected by cumulative access)\n")
        f.write("-" * 70 + "\n")
        f.write(f"  HOT  (PAC >= {hot_th:>8.1f}): {len(hot_pages):6d} pages")
        f.write(f"  ({hot_access/total_access*100:5.1f}% access)")
        f.write(f"  ({len(hot_pages)*4/1024:.1f} MB)  → FAST tier ⚡\n")
        f.write(f"  WARM (PAC >= {warm_th:>8.1f}): {len(warm_pages):6d} pages")
        f.write(f"  ({warm_access/total_access*100:5.1f}% access)")
        f.write(f"  ({len(warm_pages)*4/1024:.1f} MB)  → FAST if space\n")
        f.write(f"  COLD (PAC <  {warm_th:>8.1f}): {len(cold_pages):6d} pages")
        f.write(f"  ({cold_access/total_access*100:5.1f}% access)")
        f.write(f"  ({len(cold_pages)*4/1024:.1f} MB)  → SLOW tier 🧊\n\n")
        
        # Top pages
        f.write("-" * 70 + "\n")
        f.write("  Top 30 Pages by PAC Score\n")
        f.write("-" * 70 + "\n")
        f.write(f"  {'#':>3}  {'Page':>18}  {'Access':>8}  {'AvgLAT':>7}  {'MaxLAT':>7}")
        f.write(f"  {'PAC':>9}  {'LLC%':>5}  {'SVE%':>5}  {'L:S':>6}  {'Tier':>5}\n")
        f.write(f"  {'---':>3}  {'---':>18}  {'---':>8}  {'---':>7}  {'---':>7}")
        f.write(f"  {'---':>9}  {'---':>5}  {'---':>5}  {'---':>6}  {'---':>5}\n")
        
        for i, p in enumerate(sorted_pages[:30]):
            tier = 'HOT' if p['pac'] >= hot_th else ('WARM' if p['pac'] >= warm_th else 'COLD')
            f.write(f"  {i+1:3d}  {p['page']:>18}  {p['access_count']:8d}  {p['avg_lat_tot']:7.1f}  {p['max_lat_tot']:7d}")
            f.write(f"  {p['pac']:9.1f}  {p['llc_refill_pct']:5.1f}  {p['sve_pct']:5.1f}  {p['ld_st_ratio']:>6}  {tier:>5}\n")
        
        f.write(f"\n  ... ({len(sorted_pages) - 30} more pages)\n\n")
        
        # LAT TOT 分布
        lat_ranges = {
            '32-50 (L3 hit)': 0, 
            '50-100 (L3/cluster)': 0,
            '100-200 (local DRAM)': 0, 
            '200-350 (remote DRAM)': 0,
            '>350 (far remote)': 0
        }
        for p in sorted_pages:
            avg = p['avg_lat_tot']
            if avg < 50: lat_ranges['32-50 (L3 hit)'] += p['access_count']
            elif avg < 100: lat_ranges['50-100 (L3/cluster)'] += p['access_count']
            elif avg < 200: lat_ranges['100-200 (local DRAM)'] += p['access_count']
            elif avg < 350: lat_ranges['200-350 (remote DRAM)'] += p['access_count']
            else: lat_ranges['>350 (far remote)'] += p['access_count']
        
        f.write("-" * 70 + "\n")
        f.write("  LAT TOT Distribution (by access count, min_latency=32)\n")
        f.write("-" * 70 + "\n")
        for rng, count in lat_ranges.items():
            pct = count / total_access * 100 if total_access else 0
            bar = '█' * int(pct / 2)
            f.write(f"  {rng:>24}: {count:>10,} ({pct:5.1f}%) {bar}\n")
        
        f.write(f"\n")
        
        # SVE 分析
        total_sve = sum(p['access_count'] for p in sorted_pages if p['sve_pct'] > 50)
        total_llc_refill = sum(p['access_count'] for p in sorted_pages if p['llc_refill_pct'] > 50)
        
        f.write("-" * 70 + "\n")
        f.write("  Access Pattern Insights\n")
        f.write("-" * 70 + "\n")
        f.write(f"  Pages with >50% SVE accesses:  {total_sve:,} ({total_sve/total_access*100:.1f}% of access)\n")
        f.write(f"  Pages with >50% LLC refills:    {total_llc_refill:,} ({total_llc_refill/total_access*100:.1f}% of access)\n")
        f.write(f"  SVE pages have natural high MLP → PAC adjusted down for tiered placement\n")
    
    # === kumf.conf 生成 ===
    conf_path = os.path.join(output_dir, 'kumf.conf')
    with open(conf_path, 'w') as f:
        f.write("# KUMF auto-generated interc configuration\n")
        f.write(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"# Pages: {len(sorted_pages)} (HOT:{len(hot_pages)} WARM:{len(warm_pages)} COLD:{len(cold_pages)})\n\n")
        
        # Page-range routing
        hot_pages_sorted = sorted([int(p['page'], 16) for p in hot_pages])
        if hot_pages_sorted:
            ranges = _find_contiguous_ranges(hot_pages_sorted)
            f.write(f"# Hot page ranges: {len(ranges)} ranges covering {len(hot_pages)} pages ({len(hot_pages)*4/1024:.1f} MB)\n")
            for start, end in ranges[:200]:
                f.write(f"page_range:0x{start:x}-0x{end:x} = 0  # FAST tier\n")
            if len(ranges) > 200:
                f.write(f"# ... {len(ranges) - 200} more ranges (see page_pac.csv for full list)\n")
    
    print(f"\n✅ Analysis complete!")
    print(f"   Lines processed:  {total_lines:,}")
    print(f"   Records parsed:   {records_parsed:,}")
    print(f"   Unique pages:     {len(sorted_pages):,}")
    print(f"   HOT: {len(hot_pages)}  WARM: {len(warm_pages)}  COLD: {len(cold_pages)}")
    print(f"\n   Output files:")
    print(f"   📄 Report:  {report_path}")
    print(f"   📊 CSV:     {csv_path}")
    print(f"   📋 JSON:    {json_path}")
    print(f"   ⚙️  Config:  {conf_path}")


def _find_contiguous_ranges(pages):
    """将排序的 page 列表合并为连续范围"""
    if not pages:
        return []
    
    ranges = []
    start = pages[0]
    end = pages[0]
    
    for p in pages[1:]:
        if p == end + 0x1000:  # 连续 page
            end = p
        else:
            ranges.append((start, end + 0xFFF))
            start = p
            end = p
    
    ranges.append((start, end + 0xFFF))
    return ranges


def main():
    parser = argparse.ArgumentParser(description='KUMF SPE Page-Level PAC Analyzer')
    parser.add_argument('--output', '-o', default='/tmp/kumf', help='Output directory')
    parser.add_argument('--max-records', type=int, default=None, help='Max records to parse')
    parser.add_argument('--mlp', type=float, default=1.0, help='MLP estimate for PAC calculation')
    parser.add_argument('--hot-threshold', type=float, default=None, help='Override hot PAC threshold')
    parser.add_argument('--warm-threshold', type=float, default=None, help='Override warm PAC threshold')
    args = parser.parse_args()
    
    print(f"🦐 KUMF SPE Page-Level PAC Analyzer v0.2")
    print(f"   Reading from stdin (pipe perf report -D output)...")
    print(f"   MLP estimate: {args.mlp}")
    if args.max_records:
        print(f"   Max records: {args.max_records:,}")
    
    page_stats, records_parsed, total_lines = parse_stream(sys.stdin, max_records=args.max_records)
    
    if not page_stats:
        print("❌ No SPE records found! Check input format.")
        print("   Make sure you're piping: perf report -D -i perf.data | python3 spe_page_pac.py")
        sys.exit(1)
    
    pac_scores = compute_pac_scores(page_stats, mlp_estimate=args.mlp)
    
    # 自动检测拐点
    hot_th, warm_th, cold_th = detect_knee(pac_scores)
    
    # 允许覆盖
    if args.hot_threshold is not None:
        hot_th = args.hot_threshold
    if args.warm_threshold is not None:
        warm_th = args.warm_threshold
    
    generate_report(pac_scores, hot_th, warm_th, cold_th, records_parsed, total_lines, args.output)


if __name__ == '__main__':
    main()
