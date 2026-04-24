/*
 * mini_md.c — 简化版分子动力学模拟
 *
 * 模拟 GROMACS 核心计算模式，包含明确的冷热数据对象：
 *   热对象: coordinates, forces, velocities — 每步都全量访问
 *   冷对象: topology, masses, bond_list — 只读或少量访问
 *
 * 用法:
 *   ./mini_md                           # 默认参数
 *   ./mini_md <粒子数> <步数> <线程数>
 *
 * 示例:
 *   numactl --cpunodebind=0 --membind=0 ./mini_md 100000 500 8    # 全快层
 *   numactl --cpunodebind=0 --membind=2 ./mini_md 100000 500 8    # 全慢层
 *   LD_PRELOAD=./libtiered.so ./mini_md 100000 500 8              # 智能分配
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>

/* ========== 数据结构 ========== */

typedef struct {
    double x, y, z;
} Vec3;

/* 热对象：每步全量读写 */
static Vec3 *coordinates;   // N 个粒子的坐标
static Vec3 *forces;        // N 个粒子的力
static Vec3 *velocities;    // N 个粒子的速度

/* 冷对象：只读或少量访问 */
static double *masses;          // N 个粒子质量（只读）
static int    *particle_types;  // N 个粒子类型（只读）
static double *topology_params; // 类型参数表（只读，很小）
static int    *bond_list;       // 化学键列表（只读，成对索引）
static int     num_bonds;

static int    N_particles;
static int    N_steps;
static int    N_threads;

static double cutoff = 2.0;
static double box_size;

/* ========== 分配器（支持 NUMA 绑定）========== */

static int alloc_node = -1;  // -1 = 默认 first-touch

void *numa_alloc(size_t sz) {
    if (alloc_node >= 0) {
        return numa_alloc_onnode(sz, alloc_node);
    }
    void *p = malloc(sz);
    if (p) memset(p, 0, sz);  // 触发 page fault，first-touch
    return p;
}

void numa_free_wrapper(void *p, size_t sz) {
    if (alloc_node >= 0) {
        numa_free(p, sz);
    } else {
        free(p);
    }
}

/* ========== 数据初始化 ========== */

void init_data() {
    box_size = cbrt((double)N_particles) * 1.5;

    /* 热对象 */
    coordinates = numa_alloc(N_particles * sizeof(Vec3));
    forces      = numa_alloc(N_particles * sizeof(Vec3));
    velocities  = numa_alloc(N_particles * sizeof(Vec3));

    /* 冷对象 */
    masses         = numa_alloc(N_particles * sizeof(double));
    particle_types = numa_alloc(N_particles * sizeof(int));
    topology_params = numa_alloc(10 * sizeof(double));  // 10 种类型参数

    /* 拓扑参数（LJ 参数 epsilon, sigma） */
    for (int i = 0; i < 10; i++) {
        topology_params[i] = 0.5 + 0.1 * i;  // epsilon
    }

    /* 粒子数据 */
    srand(42);
    for (int i = 0; i < N_particles; i++) {
        coordinates[i].x = (double)rand() / RAND_MAX * box_size;
        coordinates[i].y = (double)rand() / RAND_MAX * box_size;
        coordinates[i].z = (double)rand() / RAND_MAX * box_size;

        velocities[i].x = (double)rand() / RAND_MAX * 0.1 - 0.05;
        velocities[i].y = (double)rand() / RAND_MAX * 0.1 - 0.05;
        velocities[i].z = (double)rand() / RAND_MAX * 0.1 - 0.05;

        forces[i].x = forces[i].y = forces[i].z = 0.0;

        masses[i] = 1.0 + (double)(i % 5) * 0.2;
        particle_types[i] = i % 10;
    }

    /* 化学键列表（模拟约 2x 粒子数的键）*/
    num_bonds = N_particles * 2;
    bond_list = numa_alloc(num_bonds * 2 * sizeof(int));
    for (int i = 0; i < num_bonds; i++) {
        int a = i % N_particles;
        int b = (a + 1) % N_particles;
        bond_list[i * 2]     = a;
        bond_list[i * 2 + 1] = b;
    }
}

void free_data() {
    size_t v3_sz = N_particles * sizeof(Vec3);
    numa_free_wrapper(coordinates, v3_sz);
    numa_free_wrapper(forces, v3_sz);
    numa_free_wrapper(velocities, v3_sz);
    numa_free_wrapper(masses, N_particles * sizeof(double));
    numa_free_wrapper(particle_types, N_particles * sizeof(int));
    numa_free_wrapper(topology_params, 10 * sizeof(double));
    numa_free_wrapper(bond_list, num_bonds * 2 * sizeof(int));
}

/* ========== 核心计算 ========== */

/* 周期性边界条件 */
static inline double min_image(double dx, double box) {
    if (dx >  box * 0.5) dx -= box;
    if (dx < -box * 0.5) dx += box;
    return dx;
}

/* 非键力计算（Lennard-Jones + cutoff）—— 计算密集，频繁访问坐标和力 */
void compute_nonbonded_forces() {
    double cutoff2 = cutoff * cutoff;
    double shift = 0.0;

    /* 清零力 */
    memset(forces, 0, N_particles * sizeof(Vec3));

    /* O(N^2) pair-wise（简化版，真实 MD 用 neighbor list）*/
    for (int i = 0; i < N_particles; i++) {
        Vec3 ri = coordinates[i];
        int ti = particle_types[i];
        double eps_i = topology_params[ti];

        for (int j = i + 1; j < N_particles; j++) {
            double dx = min_image(ri.x - coordinates[j].x, box_size);
            double dy = min_image(ri.y - coordinates[j].y, box_size);
            double dz = min_image(ri.z - coordinates[j].z, box_size);

            double r2 = dx*dx + dy*dy + dz*dz;
            if (r2 < cutoff2 && r2 > 0.01) {
                int tj = particle_types[j];
                double eps = sqrt(eps_i * topology_params[tj]);
                double sigma = 1.0;
                double sr2 = (sigma * sigma) / r2;
                double sr6 = sr2 * sr2 * sr2;
                double sr12 = sr6 * sr6;
                double f = 24.0 * eps * (2.0 * sr12 - sr6) / r2;

                double fx = f * dx;
                double fy = f * dy;
                double fz = f * dz;

                forces[i].x += fx; forces[i].y += fy; forces[i].z += fz;
                forces[j].x -= fx; forces[j].y -= fy; forces[j].z -= fz;
            }
        }
    }
}

/* 键力计算（harmonic bond）—— 访问 bond_list（冷对象）+ coordinates/forces（热对象）*/
void compute_bond_forces() {
    double k_bond = 100.0;
    double r0 = 1.0;

    for (int b = 0; b < num_bonds; b++) {
        int i = bond_list[b * 2];
        int j = bond_list[b * 2 + 1];

        double dx = coordinates[i].x - coordinates[j].x;
        double dy = coordinates[i].y - coordinates[j].y;
        double dz = coordinates[i].z - coordinates[j].z;
        double r = sqrt(dx*dx + dy*dy + dz*dz);
        if (r < 1e-10) continue;

        double f = -k_bond * (r - r0) / r;
        double fx = f * dx, fy = f * dy, fz = f * dz;

        forces[i].x += fx; forces[i].y += fy; forces[i].z += fz;
        forces[j].x -= fx; forces[j].y -= fy; forces[j].z -= fz;
    }
}

/* 速度 Verlet 积分 —— 全量访问 coordinates, velocities, forces, masses */
void integrate(double dt) {
    for (int i = 0; i < N_particles; i++) {
        double inv_m = 1.0 / masses[i];
        velocities[i].x += 0.5 * forces[i].x * inv_m * dt;
        velocities[i].y += 0.5 * forces[i].y * inv_m * dt;
        velocities[i].z += 0.5 * forces[i].z * inv_m * dt;

        coordinates[i].x += velocities[i].x * dt;
        coordinates[i].y += velocities[i].y * dt;
        coordinates[i].z += velocities[i].z * dt;

        /* PBC */
        coordinates[i].x = fmod(coordinates[i].x + box_size, box_size);
        coordinates[i].y = fmod(coordinates[i].y + box_size, box_size);
        coordinates[i].z = fmod(coordinates[i].z + box_size, box_size);
    }
}

/* 动能计算（用于验证） */
double compute_kinetic_energy() {
    double ke = 0.0;
    for (int i = 0; i < N_particles; i++) {
        double m = masses[i];
        ke += 0.5 * m * (velocities[i].x * velocities[i].x +
                         velocities[i].y * velocities[i].y +
                         velocities[i].z * velocities[i].z);
    }
    return ke;
}

/* ========== 主循环 ========== */

double run_simulation() {
    double dt = 0.001;
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int step = 0; step < N_steps; step++) {
        /* Velocity Verlet: half-kick */
        for (int i = 0; i < N_particles; i++) {
            double inv_m = 1.0 / masses[i];
            velocities[i].x += 0.5 * forces[i].x * inv_m * dt;
            velocities[i].y += 0.5 * forces[i].y * inv_m * dt;
            velocities[i].z += 0.5 * forces[i].z * inv_m * dt;
        }

        /* Drift */
        for (int i = 0; i < N_particles; i++) {
            coordinates[i].x += velocities[i].x * dt;
            coordinates[i].y += velocities[i].y * dt;
            coordinates[i].z += velocities[i].z * dt;
            coordinates[i].x = fmod(coordinates[i].x + box_size, box_size);
            coordinates[i].y = fmod(coordinates[i].y + box_size, box_size);
            coordinates[i].z = fmod(coordinates[i].z + box_size, box_size);
        }

        /* Force computation */
        compute_nonbonded_forces();
        compute_bond_forces();

        /* Velocity Verlet: half-kick */
        for (int i = 0; i < N_particles; i++) {
            double inv_m = 1.0 / masses[i];
            velocities[i].x += 0.5 * forces[i].x * inv_m * dt;
            velocities[i].y += 0.5 * forces[i].y * inv_m * dt;
            velocities[i].z += 0.5 * forces[i].z * inv_m * dt;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed = (t_end.tv_sec - t_start.tv_sec) +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1e9;
    return elapsed;
}

/* ========== 主函数 ========== */

void print_usage(const char *prog) {
    printf("Usage: %s [particles] [steps] [threads]\n", prog);
    printf("  particles: number of particles (default 5000)\n");
    printf("  steps:     number of MD steps (default 100)\n");
    printf("  threads:   (reserved for future OpenMP)\n");
    printf("\nMemory per configuration:\n");
    printf("  ~10 MB for 5K particles, ~200 MB for 100K particles\n");
}

int main(int argc, char *argv[]) {
    N_particles = (argc > 1) ? atoi(argv[1]) : 5000;
    N_steps     = (argc > 2) ? atoi(argv[2]) : 100;
    N_threads   = (argc > 3) ? atoi(argv[3]) : 1;

    if (N_particles < 10) { print_usage(argv[0]); return 1; }

    printf("=== mini_md: Simplified Molecular Dynamics ===\n");
    printf("Particles: %d\n", N_particles);
    printf("Steps:     %d\n", N_steps);
    printf("Box size:  %.1f\n", cbrt((double)N_particles) * 1.5);

    size_t total_mem = 0;
    total_mem += N_particles * sizeof(Vec3) * 3;  // coord + force + vel
    total_mem += N_particles * sizeof(double);     // masses
    total_mem += N_particles * sizeof(int);        // particle_types
    total_mem += N_particles * 4 * sizeof(int);    // bond_list
    total_mem += 10 * sizeof(double);              // topology_params
    printf("Memory:    %.1f MB\n", (double)total_mem / (1024*1024));

    /* 检测 NUMA 信息 */
    if (numa_available() >= 0) {
        int max_node = numa_max_node();
        int cur_node = -1;
        get_mempolicy(&cur_node, NULL, 0, (void*)&total_mem, MPOL_F_ADDR);
        printf("NUMA:      %d nodes, first-touch on node %d\n", max_node + 1, cur_node);
    } else {
        printf("NUMA:      not available\n");
    }

    /* 初始化 */
    init_data();

    /* 预热一步 */
    compute_nonbonded_forces();
    compute_bond_forces();

    /* 主模拟 */
    double elapsed = run_simulation();

    /* 结果 */
    double ke = compute_kinetic_energy();
    printf("\n--- Results ---\n");
    printf("Elapsed:   %.3f seconds\n", elapsed);
    printf("Steps/sec: %.1f\n", N_steps / elapsed);
    printf("ns/day:    %.1f  (assuming dt=0.001 = 1fs)\n",
           N_steps * 1e-6 / elapsed * 86400);
    printf("KE:        %.2f\n", ke);
    printf("Coord[0]:  (%.3f, %.3f, %.3f)\n",
           coordinates[0].x, coordinates[0].y, coordinates[0].z);

    /* NUMA 分布检查 */
    if (numa_available() >= 0) {
        printf("\n--- NUMA Distribution ---\n");
        void *objects[] = {
            coordinates, forces, velocities,
            masses, particle_types, bond_list
        };
        const char *names[] = {
            "coordinates", "forces", "velocities",
            "masses", "particle_types", "bond_list"
        };
        int hot[] = {1, 1, 1, 0, 0, 0};  // 1=热, 0=冷
        for (int i = 0; i < 6; i++) {
            int node;
            get_mempolicy(&node, NULL, 0, objects[i], MPOL_F_ADDR);
            printf("  %-15s on Node %d  %s\n",
                   names[i], node, hot[i] ? "[HOT]" : "[COLD]");
        }
    }

    free_data();
    printf("\nDone.\n");
    return 0;
}
