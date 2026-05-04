#!/usr/bin/env python3
"""
kumf_hnswlib_bench.py - HNSWLIB 向量检索 benchmark

天然冷热分离：
  - 索引结构 (HNSW graph): 热数据 — 每次查询都遍历
  - 原始向量数据: 冷数据 — 只在最后阶段精排时访问

用法:
  # 默认: 100万条 128维向量
  python3 kumf_hnswlib_bench.py

  # 大数据集: 1亿条 128维 (约50GB)
  python3 kumf_hnswlib_bench.py --num-elements 100000000 --dim 128

  # 查询测试
  python3 kumf_hnswlib_bench.py --num-elements 1000000 --queries 10000
"""

import argparse
import time
import os
import sys

def main():
    parser = argparse.ArgumentParser(description='KUMF HNSWLIB Benchmark')
    parser.add_argument('--num-elements', type=int, default=1000000, help='Number of vectors')
    parser.add_argument('--dim', type=int, default=128, help='Vector dimension')
    parser.add_argument('--m', type=int, default=16, help='HNSW M parameter')
    parser.add_argument('--ef-construction', type=int, default=200, help='HNSW ef_construction')
    parser.add_argument('--ef-search', type=int, default=50, help='HNSW ef_search')
    parser.add_argument('--queries', type=int, default=1000, help='Number of queries')
    parser.add_argument('--k', type=int, default=10, help='Top-K results')
    parser.add_argument('--seed', type=int, default=42)
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

    print("=" * 60)
    print("  KUMF HNSWLIB Benchmark")
    print("=" * 60)
    print(f"  Vectors: {args.num_elements:,} x {args.dim}D")
    print(f"  Data size: {args.num_elements * args.dim * 4 / 1024 / 1024:.1f} MB (float32)")
    print(f"  HNSW: M={args.m}, ef_construction={args.ef_construction}, ef_search={args.ef_search}")
    print(f"  Queries: {args.queries}, Top-{args.k}")
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

    # === Phase 3: 查询测试 ===
    print("── Phase 3: Query Benchmark ────────────────────────")
    
    query_data = np.random.randn(args.queries, args.dim).astype(np.float32)
    index.set_ef(args.ef_search)
    
    # Warmup
    _ = index.knn_query(query_data[:10], k=args.k)
    
    # 正式查询
    t0 = time.time()
    labels, distances = index.knn_query(query_data, k=args.k)
    t_query = time.time() - t0
    
    qps = args.queries / t_query
    latency_us = t_query / args.queries * 1e6
    
    print(f"  Queries: {args.queries}")
    print(f"  Total time: {t_query:.3f}s")
    print(f"  QPS: {qps:.1f}")
    print(f"  Avg latency: {latency_us:.1f} μs")
    print()

    # === 内存统计 ===
    print("── Memory Statistics ───────────────────────────────")
    try:
        # /proc/self/status
        with open('/proc/self/status', 'r') as f:
            for line in f:
                if line.startswith('VmRSS:') or line.startswith('VmSize:') or line.startswith('VmHWM:'):
                    print(f"  {line.strip()}")
    except:
        pass
    
    # 估算索引和数据大小
    index_size_mb = index.memory_used() / 1024 / 1024 if hasattr(index, 'memory_used') else 0
    data_size_mb = args.num_elements * args.dim * 4 / 1024 / 1024
    total_mb = index_size_mb + data_size_mb
    
    print(f"  Index size: {index_size_mb:.1f} MB (HOT — accessed every query)")
    print(f"  Data size:  {data_size_mb:.1f} MB (COLD — only accessed for final re-ranking)")
    print(f"  Total:      {total_mb:.1f} MB")
    print(f"  Hot ratio:  {index_size_mb/total_mb*100:.1f}%")
    print()

    # === NUMA 分布 ===
    try:
        print("── NUMA Memory Distribution ───────────────────────")
        pid = os.getpid()
        result = os.popen(f"numastat -p {pid} 2>/dev/null").read()
        if result:
            print(result)
    except:
        pass

    print("=" * 60)
    print(f"  SUMMARY: build={t_build:.2f}s query={t_query:.3f}s ({qps:.0f} QPS)")
    print(f"  Memory: index={index_size_mb:.0f}MB(HOT) + data={data_size_mb:.0f}MB(COLD)")
    print("=" * 60)


if __name__ == '__main__':
    main()
