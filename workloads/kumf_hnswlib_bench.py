#!/usr/bin/env python3
"""
kumf_hnswlib_bench.py - HNSWLIB 向量检索 benchmark (KUMF tiered memory)

HNSW 冷热分层逻辑（策略 A: 按 HNSW 层级分层）:
  - 上层 graph links (Level 1+): HOT — 少量节点但每条查询必走，导航路径
  - Level 0 graph links:         WARM — 大量节点，只访问查询附近局部区域
  - 原始向量数据 (data_level0):  WARM/COLD — 每步遍历都要算距离，但访问局部

interc 按 size 路由（hnswlib 内存分配特征）:
  - 小块 (< 1KB): Level 0 links，逐节点分配 → 快层 (Node 0)
  - 大块 (> 100MB): data_level0 连续向量 + 上层 links → 慢层 (Node 2)

  注意: 向量数据在 HNSW 搜索每一步都被访问（算距离），不是"最后才精排"。

用法:
  # 默认: 100万条 128维向量
  python3 kumf_hnswlib_bench.py

  # 大数据集: 5千万条 128维 (~24GB 向量 + ~6GB 索引)
  python3 kumf_hnswlib_bench.py --num-elements 50000000 --dim 128

  # 查询压测
  python3 kumf_hnswlib_bench.py --num-elements 50000000 --queries 10000 --ef-search 200

  # 用 KUMF interc 分层运行
  KUMF_CONF=kumf_hnswlib.conf LD_PRELOAD=build/libkumf_interc.so \
    python3 kumf_hnswlib_bench.py --num-elements 50000000

  # 对比: 全快层
  numactl --preferred=0 python3 kumf_hnswlib_bench.py --num-elements 50000000
"""

import argparse
import time
import os
import sys
import statistics


def run_queries(index, query_data, k, num_rounds=1):
    """执行多轮查询，返回每轮延迟列表"""
    latencies = []
    for _ in range(num_rounds):
        t0 = time.time()
        labels, distances = index.knn_query(query_data, k=k)
        t = time.time() - t0
        latencies.append(t)
    return latencies


def main():
    parser = argparse.ArgumentParser(description='KUMF HNSWLIB Benchmark')
    parser.add_argument('--num-elements', type=int, default=1000000,
                        help='Number of vectors (default: 1M; use 50M+ for pressure test)')
    parser.add_argument('--dim', type=int, default=128, help='Vector dimension')
    parser.add_argument('--m', type=int, default=16, help='HNSW M parameter')
    parser.add_argument('--ef-construction', type=int, default=200, help='HNSW ef_construction')
    parser.add_argument('--ef-search', type=int, default=50, help='HNSW ef_search')
    parser.add_argument('--queries', type=int, default=1000, help='Number of queries')
    parser.add_argument('--k', type=int, default=10, help='Top-K results')
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--rounds', type=int, default=3,
                        help='Number of query rounds (for stable P50/P99)')
    parser.add_argument('--no-index', action='store_true', help='Skip index build, only query')
    args = parser.parse_args()

    try:
        import hnswlib
        import numpy as np
    except ImportError:
        print("Installing hnswlib and numpy...")
        os.system(f"{sys.executable} -m pip install hnswlib numpy")
        import hnswlib
        import numpy as np

    data_size_mb = args.num_elements * args.dim * 4 / 1024 / 1024

    print("=" * 64)
    print("  KUMF HNSWLIB Benchmark — Tiered Memory Validation")
    print("=" * 64)
    print(f"  Vectors:      {args.num_elements:,} x {args.dim}D")
    print(f"  Data size:    {data_size_mb:.1f} MB (float32)")
    print(f"  HNSW params:  M={args.m}, ef_construction={args.ef_construction}, ef_search={args.ef_search}")
    print(f"  Queries:      {args.queries} x {args.rounds} rounds, Top-{args.k}")
    print()

    # KUMF 环境检测
    kumf_interc = os.environ.get('LD_PRELOAD', '')
    kumf_conf = os.environ.get('KUMF_CONF', '')
    numa_hint = os.environ.get('NUMA_HINT', '')
    if kumf_interc and 'kumf' in kumf_interc.lower():
        print(f"  KUMF interc:  ✅ {kumf_interc}")
        if kumf_conf:
            print(f"  KUMF conf:    {kumf_conf}")
    else:
        print(f"  KUMF interc:  ❌ (running bare, no tiered memory)")
    if numa_hint:
        print(f"  NUMA hint:    {numa_hint}")
    print()

    np.random.seed(args.seed)

    # === Phase 1: 生成数据 ===
    print("── Phase 1: Generate Data ──────────────────────────")
    t0 = time.time()
    data = np.random.randn(args.num_elements, args.dim).astype(np.float32)
    t_gen = time.time() - t0
    print(f"  Data generated: {t_gen:.2f}s")
    print(f"  Data shape: {data.shape}, dtype: {data.dtype}")
    print()

    # === Phase 2: 构建索引 ===
    print("── Phase 2: Build HNSW Index ───────────────────────")
    t0 = time.time()

    index = hnswlib.Index(space='l2', dim=args.dim)
    index.init_index(max_elements=args.num_elements,
                     ef_construction=args.ef_construction,
                     M=args.m)

    # 分批添加（减少内存峰值）
    batch_size = 100000
    for i in range(0, args.num_elements, batch_size):
        end = min(i + batch_size, args.num_elements)
        index.add_items(data[i:end], list(range(i, end)))
        if (end % 1000000 == 0) or end == args.num_elements:
            print(f"  Added {end:,}/{args.num_elements:,} vectors...")

    t_build = time.time() - t0
    print(f"  Index built: {t_build:.2f}s")
    print()

    # === Phase 3: 内存布局分析 ===
    print("── Phase 3: Memory Layout Analysis ─────────────────")

    # hnswlib 内存分配特征
    # - data_level0_: 一个巨大连续块 (num_elements * dim * 4 bytes) → 向量数据
    # - linkLists_:   每个节点一个 malloc (level 0 links: M*2*4 bytes)
    # - 上层 links:   少量节点，每个更大块
    index_size_mb = index.memory_used() / 1024 / 1024 if hasattr(index, 'memory_used') else 0
    total_mb = index_size_mb + data_size_mb

    # 估算 L0 links 大小 (每个节点: M * 2 * sizeof(int) = M * 8 bytes)
    l0_link_per_node = args.m * 2 * 4  # M * 2 * sizeof(uint32_t)
    l0_links_total_mb = args.num_elements * l0_link_per_node / 1024 / 1024

    # 上层节点数 ~sqrt(N)，每个节点的 links 更大
    upper_nodes = max(1, int(args.num_elements ** 0.5))
    upper_links_mb = index_size_mb - l0_links_total_mb if index_size_mb > l0_links_total_mb else 0

    print(f"  Total memory:  {total_mb:.1f} MB")
    print(f"  Index memory:  {index_size_mb:.1f} MB")
    print(f"  Vector data:   {data_size_mb:.1f} MB (1 big malloc → interc size 规则匹配)")
    print(f"  L0 links:      ~{l0_links_total_mb:.1f} MB ({args.num_elements:,} small mallocs, ~{l0_link_per_node} bytes each)")
    print(f"  Upper links:   ~{upper_links_mb:.1f} MB ({upper_nodes} nodes)")
    print()
    print(f"  Tiered routing (策略 A — 按 size):")
    print(f"    快层 (Node 0): L0 links — 小块 (<1KB), 导航必走, 访问频繁")
    print(f"    慢层 (Node 2): 向量数据 + 上层 links — 大块 (>100MB), 局部访问")
    print(f"    预期效果: 导航低延迟(快层) + 容量省(慢层存大数据)")
    print()

    # === Phase 4: 查询 benchmark ===
    print("── Phase 4: Query Benchmark ────────────────────────")

    query_data = np.random.randn(args.queries, args.dim).astype(np.float32)
    index.set_ef(args.ef_search)

    # Warmup
    _ = index.knn_query(query_data[:100], k=args.k)

    # 多轮查询，取稳定结果
    latencies = run_queries(index, query_data, args.k, num_rounds=args.rounds)

    qps_list = [args.queries / t for t in latencies]
    avg_qps = statistics.mean(qps_list)
    avg_latency_us = statistics.mean(latencies) / args.queries * 1e6

    # 单 query 粒度延迟（从总时间估算）
    per_query_us = [t / args.queries * 1e6 for t in latencies]

    print(f"  Rounds: {args.rounds}")
    for i, (t, qps, lat) in enumerate(zip(latencies, qps_list, per_query_us)):
        print(f"    Round {i+1}: {t:.3f}s, {qps:.0f} QPS, {lat:.1f} μs/query")
    print()
    print(f"  ┌──────────────────────────────────────┐")
    print(f"  │  Avg QPS:       {avg_qps:>10.0f}              │")
    print(f"  │  Avg latency:   {avg_latency_us:>10.1f} μs           │")
    print(f"  │  Best QPS:      {max(qps_list):>10.0f}              │")
    print(f"  │  Worst QPS:     {min(qps_list):>10.0f}              │")
    print(f"  └──────────────────────────────────────┘")
    print()

    # === Phase 5: NUMA 分布 ===
    print("── Phase 5: NUMA Memory Distribution ──────────────")
    try:
        pid = os.getpid()
        result = os.popen(f"numastat -p {pid} 2>/dev/null").read()
        if result.strip():
            print(result)
        else:
            print("  (numastat not available)")
    except Exception as e:
        print(f"  (numastat error: {e})")

    # /proc/self/status
    try:
        with open('/proc/self/status', 'r') as f:
            for line in f:
                if line.startswith('VmRSS:') or line.startswith('VmHWM:'):
                    print(f"  {line.strip()}")
    except:
        pass

    print()

    # === Summary ===
    print("=" * 64)
    print(f"  SUMMARY")
    print(f"  build={t_build:.2f}s  query={statistics.mean(latencies):.3f}s ({avg_qps:.0f} QPS)")
    print(f"  Memory: index={index_size_mb:.0f}MB + vectors={data_size_mb:.0f}MB = {total_mb:.0f}MB")
    print(f"  Routing: L0 links→快层(small malloc) | vectors+upper→慢层(big malloc)")
    print("=" * 64)


if __name__ == '__main__':
    main()
