#!/bin/bash
# hnswlib_numa_benchmark.sh
#
# 在鲲鹏930上用开源 hnswlib 验证 NUMA 分层效果
#
# 原理:
#   HNSW 索引构建时分配大量内存（图结构 + 向量数据）
#   搜索时频繁随机访问图节点（pointer-chasing，MLP 低，NUMA 敏感）
#   对比：索引全在本地 vs 索引全在远端，搜索 QPS 差异
#
# 依赖:
#   - cmake 3.14+, g++ 12+
#   - numactl (已安装)
#   - 无需 libnuma-devel（用 numactl 命令行控制 NUMA）
#
# 用法:
#   bash hnswlib_numa_benchmark.sh

set -e

WORKDIR="$HOME/hnswlib-numa-test"
HNWSLIB_VERSION="v0.8.0"
DIM=128            # 向量维度
NUM_ELEMENTS=200000  # 向量数量（~200MB 索引）
M=16               # HNSW 连接数
EF_CONSTRUCTION=200
EF_SEARCH=100
NUM_QUERIES=10000  # 搜索次数
K=10               # Top-K

echo "============================================="
echo "  hnswlib NUMA 分层基准测试"
echo "  鲲鹏930, $(date)"
echo "============================================="

# ===== Step 1: 安装 hnswlib =====
echo -e "\n--- Step 1: 下载编译 hnswlib ---"
mkdir -p "$WORKDIR" && cd "$WORKDIR"

if [ ! -d "hnswlib" ]; then
    git clone https://github.com/nmslib/hnswlib.git
    cd hnswlib
    git checkout "$HNWSLIB_VERSION" 2>/dev/null || true
else
    cd hnswlib
fi

echo "hnswlib 版本: $(git describe --tags 2>/dev/null || echo 'unknown')"
echo "目录: $(pwd)"

# ===== Step 2: 编译测试程序 =====
echo -e "\n--- Step 2: 编译 benchmark 程序 ---"

cat > "$WORKDIR/hnswlib_bench.cpp" << 'CPPEOF'
#include <chrono>
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <cassert>
#include <cstring>
#include <numa.h>
#include <numaif.h>

#include "hnswlib/hnswalg.h"
#include "hnswlib/space_ip.h"  // Inner Product (cosine similarity)

using namespace hnswlib;

// 简单计时
static double now_sec() {
    auto t = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

// 打印 NUMA 信息
void print_numa_info(void *ptr, const char *name) {
    int node = -1;
    get_mempolicy(&node, NULL, 0, ptr, MPOL_F_ADDR);
    std::cout << "  " << name << " on NUMA node " << node << std::endl;
}

int main(int argc, char **argv) {
    int dim         = (argc > 1) ? atoi(argv[1]) : 128;
    int num_elem    = (argc > 2) ? atoi(argv[2]) : 200000;
    int M           = (argc > 3) ? atoi(argv[3]) : 16;
    int ef_constr   = (argc > 4) ? atoi(argv[4]) : 200;
    int ef_search   = (argc > 5) ? atoi(argv[5]) : 100;
    int num_queries = (argc > 6) ? atoi(argv[6]) : 10000;
    int K           = (argc > 7) ? atoi(argv[7]) : 10;

    size_t data_size = (size_t)num_elem * dim * sizeof(float);
    size_t index_est = data_size * 2.5;  // HNSW 图结构开销约为数据的 1.5x

    std::cout << "=== hnswlib NUMA Benchmark ===" << std::endl;
    std::cout << "Dimension:   " << dim << std::endl;
    std::cout << "Elements:    " << num_elem << std::endl;
    std::cout << "M:           " << M << std::endl;
    std::cout << "EF build:    " << ef_constr << std::endl;
    std::cout << "EF search:   " << ef_search << std::endl;
    std::cout << "Queries:     " << num_queries << std::endl;
    std::cout << "Top-K:       " << K << std::endl;
    std::cout << "Data size:   " << data_size / (1024*1024) << " MB" << std::endl;
    std::cout << "Est. index:  " << index_est / (1024*1024) << " MB" << std::endl;

    if (numa_available() >= 0) {
        std::cout << "NUMA nodes:  " << numa_max_node() + 1 << std::endl;
    }

    // ===== 生成随机向量 =====
    std::cout << "\n--- Generating random vectors ---" << std::endl;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    float *data = new float[(size_t)num_elem * dim];
    for (size_t i = 0; i < (size_t)num_elem * dim; i++) {
        data[i] = dist(rng);
    }

    // 生成查询向量
    float *queries = new float[(size_t)num_queries * dim];
    for (size_t i = 0; i < (size_t)num_queries * dim; i++) {
        queries[i] = dist(rng);
    }

    // NUMA 信息
    print_numa_info(data, "data array");
    print_numa_info(queries, "queries array");

    // ===== 构建索引 =====
    std::cout << "\n--- Building HNSW index ---" << std::endl;
    double t0 = now_sec();

    InnerProductSpace space(dim);
    HierarchicalNSW<float> *index = new HierarchicalNSW<float>(space, num_elem, M, ef_constr, 42);

    // 逐个添加向量
    for (int i = 0; i < num_elem; i++) {
        index->addPoint(data + (size_t)i * dim, i);
        if (i > 0 && i % 50000 == 0) {
            std::cout << "  Added " << i << "/" << num_elem << " (" << (i*100/num_elem) << "%)" << std::endl;
        }
    }

    double t1 = now_sec();
    double build_time = t1 - t0;
    std::cout << "Build time:  " << build_time << " seconds" << std::endl;
    std::cout << "Build speed: " << num_elem / build_time << " vectors/sec" << std::endl;
    print_numa_info(index, "HNSW index");

    // ===== 搜索 =====
    std::cout << "\n--- Searching (EF=" << ef_search << ", K=" << K << ") ---" << std::endl;
    index->setEf(ef_search);

    // 预热
    for (int i = 0; i < std::min(100, num_queries); i++) {
        index->searchKnn(queries + (size_t)i * dim, K);
    }

    // 正式搜索
    double t2 = now_sec();
    std::vector<double> latencies(num_queries);
    size_t total_results = 0;

    for (int i = 0; i < num_queries; i++) {
        double q_start = now_sec();
        auto result = index->searchKnn(queries + (size_t)i * dim, K);
        double q_end = now_sec();
        latencies[i] = (q_end - q_start) * 1e6;  // 微秒
        total_results += result.size();
    }

    double t3 = now_sec();
    double search_time = t3 - t2;

    // 统计
    double total_lat = 0, min_lat = 1e9, max_lat = 0;
    for (int i = 0; i < num_queries; i++) {
        total_lat += latencies[i];
        if (latencies[i] < min_lat) min_lat = latencies[i];
        if (latencies[i] > max_lat) max_lat = latencies[i];
    }
    double avg_lat = total_lat / num_queries;

    // 排序求 P50/P99
    std::sort(latencies.begin(), latencies.end());
    double p50 = latencies[num_queries / 2];
    double p99 = latencies[(int)(num_queries * 0.99)];

    std::cout << "\n--- Search Results ---" << std::endl;
    std::cout << "Total queries:    " << num_queries << std::endl;
    std::cout << "Total time:       " << search_time << " seconds" << std::endl;
    std::cout << "QPS:              " << num_queries / search_time << std::endl;
    std::cout << "Avg latency:      " << avg_lat << " us" << std::endl;
    std::cout << "P50 latency:      " << p50 << " us" << std::endl;
    std::cout << "P99 latency:      " << p99 << " us" << std::endl;
    std::cout << "Min latency:      " << min_lat << " us" << std::endl;
    std::cout << "Max latency:      " << max_lat << " us" << std::endl;
    std::cout << "Results/query:    " << total_results / num_queries << std::endl;

    // ===== 清理 =====
    delete index;
    delete[] data;
    delete[] queries;

    std::cout << "\nDone." << std::endl;
    return 0;
}
CPPEOF

# 检查 numa.h 是否存在
NUMA_H=$(find /usr -name 'numa.h' -path '*/include/*' 2>/dev/null | head -1)
if [ -z "$NUMA_H" ]; then
    echo "⚠️  numa.h 未找到，尝试安装 numactl-devel..."
    sudo yum install -y numactl-devel 2>/dev/null || {
        echo "无法安装 numactl-devel，改用 numactl 命令行方案"
        echo "修改代码移除 libnuma 依赖..."
        # 移除 numa 相关头文件和函数
        sed -i '/numa.h/d; /numaif.h/d; /get_mempolicy/d; /print_numa_info/d' "$WORKDIR/hnswlib_bench.cpp"
        # 替换 print_numa_info 调用为注释
        sed -i 's/print_numa_info/\/\/ print_numa_info/g' "$WORKDIR/hnswlib_bench.cpp"
    }
fi

cd "$WORKDIR"
g++ -O3 -march=armv8.2-a -o hnswlib_bench \
    hnswlib_bench.cpp \
    -I hnswlib \
    -lnuma \
    -lm \
    2>&1

if [ ! -f hnswlib_bench ]; then
    echo "❌ 编译失败"
    exit 1
fi
echo "✅ 编译成功: $(file hnswlib_bench)"

# ===== Step 3: 运行 NUMA 对比测试 =====
echo -e "\n============================================="
echo "  NUMA 分层效果测试"
echo "============================================="

PARAMS="$DIM $NUM_ELEMENTS $M $EF_CONSTRUCTION $EF_SEARCH $NUM_QUERIES $K"

echo -e "\n--- Test 1: 全快层 (Node 0) — 性能上限 ---"
echo "命令: numactl --cpunodebind=0 --membind=0 ./hnswlib_bench $PARAMS"
numactl --cpunodebind=0 --membind=0 ./hnswlib_bench $PARAMS 2>&1 | tee "$WORKDIR/result_node0.log"

echo -e "\n--- Test 2: 全慢层 (Node 2) — 性能下限 ---"
echo "命令: numactl --cpunodebind=0 --membind=2 ./hnswlib_bench $PARAMS"
numactl --cpunodebind=0 --membind=2 ./hnswlib_bench $PARAMS 2>&1 | tee "$WORKDIR/result_node2.log"

echo -e "\n--- Test 3: 默认 (first-touch) ---"
echo "命令: numactl --cpunodebind=0 ./hnswlib_bench $PARAMS"
numactl --cpunodebind=0 ./hnswlib_bench $PARAMS 2>&1 | tee "$WORKDIR/result_default.log"

# ===== Step 4: 汇总对比 =====
echo -e "\n============================================="
echo "  结果汇总"
echo "============================================="

extract() { grep "$1" "$2" | awk -F': *' '{print $2}'; }

echo ""
echo "| 指标              | Node 0 (快) | Node 2 (慢) | 默认 (first-touch) |"
echo "|-------------------|-------------|-------------|---------------------|"

for metric in "Build time" "QPS" "Avg latency" "P50 latency" "P99 latency"; do
    v0=$(extract "$metric" "$WORKDIR/result_node0.log")
    v2=$(extract "$metric" "$WORKDIR/result_node2.log")
    vd=$(extract "$metric" "$WORKDIR/result_default.log")
    printf "| %-17s | %-11s | %-11s | %-19s |\n" "$metric" "$v0" "$v2" "$vd"
done

echo ""
echo "NUMA 分层优化空间 = Node2 相对 Node0 的性能差距"
echo "如果 QPS 差距 > 20%，说明 tiered memory 优化有显著收益"

echo -e "\n完整日志:"
echo "  $WORKDIR/result_node0.log"
echo "  $WORKDIR/result_node2.log"
echo "  $WORKDIR/result_default.log"
