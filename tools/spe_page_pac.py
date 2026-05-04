#!/usr/bin/env python3
"""
spe_page_pac.py - 从 perf report -D 输出中提取 page 级 PAC 评分

用法:
  # 方案 A: 流式 pipe（边读边解析，内存友好）
  perf report -D -i perf.data | python3 spe_page_pac.py

  # 方案 B: 限制处理行数（快速验证）
  perf report -D -i perf.data | head -5000000 | python3 spe_page_pac.py

  # 方案 C: 从已有文件读取
  cat perf_raw.txt | python3 spe_page_pac.py

  # 方案 D: 直接解析 perf.data 二进制（最快，需要 pyperf）
  python3 spe_page_pac.py --binary perf.data

输出:
  - page_pac.csv: page地址,访问次数,平均LAT_TOT,平均LAT_ISSUE,平均LAT_XLAT,PAC评分
  - pac_summary.txt: 人类可读的 PAC 分析报告
  - kumf.conf: 自动生成的 interc 配置
"""

import sys
import os
import re
import json
import argparse
from collections import defaultdict
from datetime import datetime


def parse_stream(stream, max_records=None):
    """
    流式解析 perf report -D 输出，提取每条 SPE 样本的 VA + LAT 信息
    
    SPE 记录结构（从实际数据确认）:
      VA    (0xb2)  数据虚拟地址
      LAT   (0x9a)  XLAT - 地址翻译延迟
      LAT   (0x99)  ISSUE - 发射延迟  
      LAT   (0x98)  TOT - 总延迟 ← 核心
      LAT   (0x9e)  未知延迟字段
      PC    (0xb0)  指令地址
      EV    (0x62)  事件类型 (L1D-ACCESS, LLC-ACCESS, etc.)
      OP    (0x49)  操作类型 (LD/ST)
      TS    (0x71)  时间戳
      CONTEXT (0x65) 上下文
    """
    
    # 当前样本状态
    current_sample = {}
    records_parsed = 0
    page_stats = defaultdict(lambda: {
        'access_count': 0,
        'lat_tot_sum': 0,
        'lat_tot_max': 0,
        'lat_issue_sum': 0,
        'lat_xlat_sum': 0,
        'lat_9e_sum': 0,
        'events': defaultdict(int),
        'ops': defaultdict(int),
    })
    
    # VA 和 LAT 可能跨多行属于同一条记录
    # 策略：遇到新的 VA (0xb2) 或 PC (0xb0) 视为新样本开始
    # 实际上新样本以 VA 或 PC 开头
    
    va_pattern = re.compile(r'VA (0x[0-9a-fA-F]+)')
    lat_pattern = re.compile(r'LAT (\d+)\s*(ISSUE|TOT|XLAT)?')
    ev_pattern = re.compile(r'EV\s+(.+)')
    op_pattern = re.compile(r'(LD|ST)\s+(GP-REG|SVE)')
    pc_pattern = re.compile(r'PC (0x[0-9a-fA-F]+)')
    
    new_record_markers = {'VA', 'PC', 'TS'}  # 新记录开始的标志
    
    pending_va = None
    sample_data = {}
    
    for line in stream:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        
        # 检测 VA 字段
        va_match = va_pattern.search(line)
        if va_match:
            # 如果之前有未处理的样本，先保存
            if pending_va and sample_data:
                _flush_sample(pending_va, sample_data, page_stats)
                records_parsed += 1
                if max_records and records_parsed >= max_records:
                    break
            
            pending_va = va_match.group(1)
            sample_data = {}
            continue
        
        # 检测 PC 字段（也是新记录开始，但某些样本只有 PC 没有 VA）
        pc_match = pc_pattern.search(line)
        if pc_match and not va_match:
            # PC 出现且没有 VA → 可能在 VA 之前出现 PC
            # 不 flush，因为 VA 可能在前面已经出现
            if 'pc' not in sample_data:
                sample_data['pc'] = pc_match.group(1)
            continue
        
        # 检测 LAT 字段
        lat_match = lat_pattern.search(line)
        if lat_match and pending_va:
            lat_val = int(lat_match.group(1))
            lat_type = lat_match.group(2)
            
            if lat_type == 'TOT':
                sample_data['lat_tot'] = lat_val
            elif lat_type == 'ISSUE':
                sample_data['lat_issue'] = lat_val
            elif lat_type == 'XLAT':
                sample_data['lat_xlat'] = lat_val
            else:
                # 0x9e 的未知 LAT 类型
                sample_data['lat_9e'] = lat_val
            continue
        
        # 检测 EV 字段
        ev_match = ev_pattern.search(line)
        if ev_match and pending_va:
            ev_str = ev_match.group(1)
            sample_data['events'] = ev_str
            continue
        
        # 检测 OP 字段
        op_match = op_pattern.search(line)
        if op_match and pending_va:
            sample_data['op'] = f"{op_match.group(1)}_{op_match.group(2)}"
            continue
    
    # 处理最后一个样本
    if pending_va and sample_data:
        _flush_sample(pending_va, sample_data, page_stats)
        records_parsed += 1
    
    return page_stats, records_parsed


def _flush_sample(va_str, sample_data, page_stats):
    """将一条样本聚合到 page 级别"""
    try:
        va = int(va_str, 16)
    except ValueError:
        return
    
    # 4KB page 对齐
    page = va & ~0xFFF
    page_hex = hex(page)
    
    stats = page_stats[page_hex]
    stats['access_count'] += 1
    
    if 'lat_tot' in sample_data:
        lat = sample_data['lat_tot']
        stats['lat_tot_sum'] += lat
        stats['lat_tot_max'] = max(stats['lat_tot_max'], lat)
    
    if 'lat_issue' in sample_data:
        stats['lat_issue_sum'] += sample_data['lat_issue']
    
    if 'lat_xlat' in sample_data:
        stats['lat_xlat_sum'] += sample_data['lat_xlat']
    
    if 'lat_9e' in sample_data:
        stats['lat_9e_sum'] += sample_data['lat_9e']
    
    if 'events' in sample_data:
        for ev in sample_data['events'].split():
            stats['events'][ev] += 1
    
    if 'op' in sample_data:
        stats['ops'][sample_data['op']] += 1


def compute_pac_scores(page_stats, mlp_estimate=1.0):
    """
    计算 page 级 PAC 评分
    
    PAC(page) = access_count × avg_latency / MLP
    
    高 PAC → 访问多 + 延迟大 → 必须放快层
    低 PAC → 访问少 或 MLP 高 → 可以放慢层
    """
    pac_scores = {}
    
    for page_hex, stats in page_stats.items():
        if stats['access_count'] == 0:
            continue
        
        avg_lat_tot = stats['lat_tot_sum'] / stats['access_count'] if stats['lat_tot_sum'] > 0 else 0
        
        # PAC = access_count × avg_latency / MLP
        pac = stats['access_count'] * avg_lat_tot / mlp_estimate
        
        pac_scores[page_hex] = {
            'page': page_hex,
            'access_count': stats['access_count'],
            'avg_lat_tot': avg_lat_tot,
            'max_lat_tot': stats['lat_tot_max'],
            'lat_tot_sum': stats['lat_tot_sum'],
            'pac': pac,
            'events': dict(stats['events']),
            'ops': dict(stats['ops']),
        }
    
    return pac_scores


def detect_knee(pac_scores):
    """
    自动检测 PAC 分布的拐点（冷热分界线）
    
    使用方法：按 PAC 排序，找到累计访问量占比变化最大的点
    """
    if not pac_scores:
        return 0.5, 0.5, 0.5
    
    # 按 PAC 降序排序
    sorted_pages = sorted(pac_scores.values(), key=lambda x: x['pac'], reverse=True)
    
    total_access = sum(p['access_count'] for p in sorted_pages)
    if total_access == 0:
        return 0.5, 0.5, 0.5
    
    # 累计访问量
    cumulative = 0
    max_gradient = 0
    knee_idx = 0
    
    for i, page in enumerate(sorted_pages):
        cumulative += page['access_count']
        ratio = cumulative / total_access
        
        # 前后 10 个点的梯度
        window = 10
        if i >= window and i < len(sorted_pages) - window:
            before = sum(p['access_count'] for p in sorted_pages[max(0, i-window):i])
            after = sum(p['access_count'] for p in sorted_pages[i:i+window])
            gradient = (after - before) / (2 * window * total_access) if total_access > 0 else 0
            
            if gradient < max_gradient:
                max_gradient = gradient
                knee_idx = i
    
    # 三级阈值
    hot_threshold = sorted_pages[min(knee_idx, len(sorted_pages)-1)]['pac'] if sorted_pages else 0
    cold_threshold = sorted_pages[min(len(sorted_pages)*3//4, len(sorted_pages)-1)]['pac'] if sorted_pages else 0
    
    return hot_threshold, hot_threshold * 0.5, cold_threshold


def generate_report(pac_scores, hot_th, warm_th, cold_th, records_parsed, output_dir):
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
        f.write('page,access_count,avg_lat_tot,max_lat_tot,pac,tier\n')
        for p in sorted_pages:
            tier = 'HOT' if p['pac'] >= hot_th else ('WARM' if p['pac'] >= warm_th else 'COLD')
            f.write(f"{p['page']},{p['access_count']},{p['avg_lat_tot']:.1f},{p['max_lat_tot']},{p['pac']:.1f},{tier}\n")
    
    # === JSON 输出 ===
    json_path = os.path.join(output_dir, 'kumf_report.json')
    report_json = {
        'version': '0.1.0',
        'timestamp': datetime.now().isoformat(),
        'sampling': {
            'method': 'arm_spe',
            'records_parsed': records_parsed,
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
            'hot_access_pct': hot_access / total_access * 100 if total_access else 0,
            'warm_access_pct': warm_access / total_access * 100 if total_access else 0,
            'cold_access_pct': cold_access / total_access * 100 if total_access else 0,
        },
        'top_pages': sorted_pages[:50],
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
        
        f.write(f"Records parsed: {records_parsed:,}\n")
        f.write(f"Unique pages:   {len(sorted_pages):,}\n")
        f.write(f"Total accesses: {total_access:,}\n\n")
        
        f.write("-" * 50 + "\n")
        f.write("  PAC Thresholds (auto-detected knee)\n")
        f.write("-" * 50 + "\n")
        f.write(f"  HOT  (>= {hot_th:.0f}):  {len(hot_pages):6d} pages  ({hot_access/total_access*100:.1f}% access)  → FAST tier\n")
        f.write(f"  WARM (>= {warm_th:.0f}):  {len(warm_pages):6d} pages  ({warm_access/total_access*100:.1f}% access)  → FAST if space\n")
        f.write(f"  COLD (<  {warm_th:.0f}):  {len(cold_pages):6d} pages  ({cold_access/total_access*100:.1f}% access)  → SLOW tier\n\n")
        
        f.write("-" * 50 + "\n")
        f.write("  Top 30 Pages by PAC Score\n")
        f.write("-" * 50 + "\n")
        f.write(f"  {'#':>3}  {'Page':>18}  {'Access':>10}  {'AvgLAT':>8}  {'MaxLAT':>8}  {'PAC':>10}  {'Tier':>5}\n")
        f.write(f"  {'---':>3}  {'---':>18}  {'---':>10}  {'---':>8}  {'---':>8}  {'---':>10}  {'---':>5}\n")
        
        for i, p in enumerate(sorted_pages[:30]):
            tier = 'HOT' if p['pac'] >= hot_th else ('WARM' if p['pac'] >= warm_th else 'COLD')
            f.write(f"  {i+1:3d}  {p['page']:>18}  {p['access_count']:10d}  {p['avg_lat_tot']:8.1f}  {p['max_lat_tot']:8d}  {p['pac']:10.1f}  {tier:>5}\n")
        
        f.write(f"\n  ... ({len(sorted_pages) - 30} more pages)\n\n")
        
        # LAT TOT 分布
        lat_ranges = {'<10': 0, '10-50': 0, '50-100': 0, '100-200': 0, '200-500': 0, '>500': 0}
        for p in sorted_pages:
            avg = p['avg_lat_tot']
            if avg < 10: lat_ranges['<10'] += p['access_count']
            elif avg < 50: lat_ranges['10-50'] += p['access_count']
            elif avg < 100: lat_ranges['50-100'] += p['access_count']
            elif avg < 200: lat_ranges['100-200'] += p['access_count']
            elif avg < 500: lat_ranges['200-500'] += p['access_count']
            else: lat_ranges['>500'] += p['access_count']
        
        f.write("-" * 50 + "\n")
        f.write("  LAT TOT Distribution (by access count)\n")
        f.write("-" * 50 + "\n")
        for rng, count in lat_ranges.items():
            pct = count / total_access * 100 if total_access else 0
            bar = '█' * int(pct / 2)
            f.write(f"  {rng:>8}: {count:>10,} ({pct:5.1f}%) {bar}\n")
    
    # === kumf.conf 生成 ===
    conf_path = os.path.join(output_dir, 'kumf.conf')
    with open(conf_path, 'w') as f:
        f.write("# KUMF auto-generated interc configuration\n")
        f.write(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"# Pages: {len(sorted_pages)} (HOT:{len(hot_pages)} WARM:{len(warm_pages)} COLD:{len(cold_pages)})\n\n")
        
        # 当前只有 size-based routing，后续加 page-range routing
        f.write("# Tier assignment based on PAC score\n")
        f.write("# TODO: page-range routing after prof cross-correlation\n\n")
        
        # 如果有连续的 page 范围，生成 page-range 规则
        hot_pages_sorted = sorted([int(p['page'], 16) for p in hot_pages])
        if hot_pages_sorted:
            ranges = _find_contiguous_ranges(hot_pages_sorted)
            f.write(f"# Hot page ranges ({len(ranges)} ranges, {len(hot_pages)} pages)\n")
            for start, end in ranges[:100]:  # 最多 100 条规则
                f.write(f"page_range:0x{start:x}-0x{end:x} = 0  # FAST tier\n")
    
    print(f"\n✅ Analysis complete!")
    print(f"   Records parsed: {records_parsed:,}")
    print(f"   Unique pages:   {len(sorted_pages):,}")
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
            ranges.append((start, end + 0xFFF))  # 包含最后一个 page 的全部地址
            start = p
            end = p
    
    ranges.append((start, end + 0xFFF))
    return ranges


def main():
    parser = argparse.ArgumentParser(description='KUMF SPE Page-Level PAC Analyzer')
    parser.add_argument('--output', '-o', default='/tmp/kumf', help='Output directory')
    parser.add_argument('--max-records', type=int, default=None, help='Max records to parse')
    parser.add_argument('--mlp', type=float, default=1.0, help='MLP estimate for PAC calculation')
    parser.add_argument('--hot-threshold', type=float, default=None, help='Override hot threshold')
    parser.add_argument('--warm-threshold', type=float, default=None, help='Override warm threshold')
    args = parser.parse_args()
    
    print(f"🦐 KUMF SPE Page-Level PAC Analyzer")
    print(f"   Reading from stdin (pipe perf report -D output)...")
    print(f"   MLP estimate: {args.mlp}")
    if args.max_records:
        print(f"   Max records: {args.max_records:,}")
    
    page_stats, records_parsed = parse_stream(sys.stdin, max_records=args.max_records)
    
    if not page_stats:
        print("❌ No SPE records found! Check input format.")
        sys.exit(1)
    
    pac_scores = compute_pac_scores(page_stats, mlp_estimate=args.mlp)
    
    # 自动检测拐点
    hot_th, warm_th, cold_th = detect_knee(pac_scores)
    
    # 允许覆盖
    if args.hot_threshold is not None:
        hot_th = args.hot_threshold
    if args.warm_threshold is not None:
        warm_th = args.warm_threshold
    
    generate_report(pac_scores, hot_th, warm_th, cold_th, records_parsed, args.output)


if __name__ == '__main__':
    main()
