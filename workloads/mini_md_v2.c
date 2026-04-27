/*
 * mini_md_v2.c — 多线程 NUMA 感知 MD 微基准
 *
 * 改进:
 *   - OpenMP 并行化非键力计算（O(N²) 外层循环并行）
 *   - 数据量可超过 L2 cache（默认 200K 粒子 ≈ 32MB）
 *   - 用 malloc 让 numactl --membind 完全控制分配
 *   - 冷热对象分离：热数据 (coord/force/vel) + 冷数据 (mass/type/topology)
 *
 * 用法:
 *   numactl --cpunodebind=0 --membind=0 ./mini_md_v2 200000 50 32   # 全快层
 *   numactl --cpunodebind=0 --membind=2 ./mini_md_v2 200000 50 32   # 全慢层
 *   numactl --cpunodebind=0,1 ./mini_md_v2 200000 50 64              # first-touch 双socket
 *
 * 编译:
 *   gcc -O3 -fopenmp -o mini_md_v2 mini_md_v2.c -lm -lnuma
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <omp.h>
#include <numa.h>
#include <numaif.h>

/* ========== 数据结构 ========== */

typedef struct {
    double x, y, z;
} Vec3;

/* 热对象：每步全量读写 */
static Vec3 *coordinates;
static Vec3 *forces;
static Vec3 *velocities;

/* 冷对象：只读或少量访问 */
static double *masses;
static int    *particle_types;
static double *topology_params;
static int    *bond_list;
static int     num_bonds;

static int    N_particles;
static int    N_steps;
static int    N_threads;

static double cutoff = 2.0;
static double box_size;

/* ========== 初始化 ========== */

void init_data() {
    box_size = cbrt((double)N_particles) * 1.5;

    /* 用 malloc 分配，让 numactl --membind 控制 NUMA 位置 */
    coordinates    = (Vec3 *)malloc(N_particles * sizeof(Vec3));
    forces         = (Vec3 *)malloc(N_particles * sizeof(Vec3));
    velocities     = (Vec3 *)malloc(N_particles * sizeof(Vec3));
    masses         = (double *)malloc(N_particles * sizeof(double));
    particle_types = (int *)malloc(N_particles * sizeof(int));
    topology_params = (double *)malloc(10 * sizeof(double));

    /* 拓扑参数 */
    for (int i = 0; i < 10; i++) {
        topology_params[i] = 0.5 + 0.1 * i;
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

    /* 化学键列表 */
    num_bonds = N_particles * 2;
    bond_list = (int *)malloc(num_bonds * 2 * sizeof(int));
    for (int i = 0; i < num_bonds; i++) {
        int a = i % N_particles;
        int b = (a + 1) % N_particles;
        bond_list[i * 2]     = a;
        bond_list[i * 2 + 1] = b;
    }
}

void free_data() {
    free(coordinates);
    free(forces);
    free(velocities);
    free(masses);
    free(particle_types);
    free(topology_params);
    free(bond_list);
}

/* ========== 核心计算 ========== */

static inline double min_image(double dx, double box) {
    if (dx >  box * 0.5) dx -= box;
    if (dx < -box * 0.5) dx += box;
    return dx;
}

/* 非键力计算 — OpenMP 并行 */
void compute_nonbonded_forces() {
    double cutoff2 = cutoff * cutoff;

    /* 清零力 */
    memset(forces, 0, N_particles * sizeof(Vec3));

    /* O(N²) pair-wise, 外层循环并行化 */
    #pragma omp parallel for schedule(dynamic, 64) num_threads(N_threads)
    for (int i = 0; i < N_particles; i++) {
        Vec3 ri = coordinates[i];
        int ti = particle_types[i];
        double eps_i = topology_params[ti];
        Vec3 fi = {0.0, 0.0, 0.0};

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

                fi.x += f * dx;
                fi.y += f * dy;
                fi.z += f * dz;

                /* 减少写冲突：只写 forces[j]，forces[i] 用局部变量 */
                #pragma omp atomic
                forces[j].x -= f * dx;
                #pragma omp atomic
                forces[j].y -= f * dy;
                #pragma omp atomic
                forces[j].z -= f * dz;
            }
        }

        forces[i].x += fi.x;
        forces[i].y += fi.y;
        forces[i].z += fi.z;
    }
}

/* 键力计算 — OpenMP 并行 */
void compute_bond_forces() {
    double k_bond = 100.0;
    double r0 = 1.0;

    #pragma omp parallel for schedule(static) num_threads(N_threads)
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

        #pragma omp atomic
        forces[i].x += fx;
        #pragma omp atomic
        forces[i].y += fy;
        #pragma omp atomic
        forces[i].z += fz;
        #pragma omp atomic
        forces[j].x -= fx;
        #pragma omp atomic
        forces[j].y -= fy;
        #pragma omp atomic
        forces[j].z -= fz;
    }
}

/* 速度 Verlet 积分 — OpenMP 并行 */
void integrate(double dt) {
    #pragma omp parallel for schedule(static) num_threads(N_threads)
    for (int i = 0; i < N_particles; i++) {
        double inv_m = 1.0 / masses[i];
        velocities[i].x += 0.5 * forces[i].x * inv_m * dt;
        velocities[i].y += 0.5 * forces[i].y * inv_m * dt;
        velocities[i].z += 0.5 * forces[i].z * inv_m * dt;

        coordinates[i].x += velocities[i].x * dt;
        coordinates[i].y += velocities[i].y * dt;
        coordinates[i].z += velocities[i].z * dt;

        coordinates[i].x = fmod(coordinates[i].x + box_size, box_size);
        coordinates[i].y = fmod(coordinates[i].y + box_size, box_size);
        coordinates[i].z = fmod(coordinates[i].z + box_size, box_size);
    }
}

/* ========== 主循环 ========== */

double run_simulation() {
    double dt = 0.001;
    double t0 = omp_get_wtime();

    for (int step = 0; step < N_steps; step++) {
        /* Velocity Verlet: half-kick + drift */
        integrate(dt);

        /* Force computation */
        compute_nonbonded_forces();
        compute_bond_forces();

        /* Velocity Verlet: half-kick */
        #pragma omp parallel for schedule(static) num_threads(N_threads)
        for (int i = 0; i < N_particles; i++) {
            double inv_m = 1.0 / masses[i];
            velocities[i].x += 0.5 * forces[i].x * inv_m * dt;
            velocities[i].y += 0.5 * forces[i].y * inv_m * dt;
            velocities[i].z += 0.5 * forces[i].z * inv_m * dt;
        }
    }

    double t1 = omp_get_wtime();
    return t1 - t0;
}

/* ========== 主函数 ========== */

int main(int argc, char *argv[]) {
    N_particles = (argc > 1) ? atoi(argv[1]) : 200000;
    N_steps     = (argc > 2) ? atoi(argv[2]) : 50;
    N_threads   = (argc > 3) ? atoi(argv[3]) : 1;

    if (N_particles < 100) {
        fprintf(stderr, "Usage: %s [particles] [steps] [threads]\n", argv[0]);
        fprintf(stderr, "  推荐: 200000 50 32 (32MB数据, 32线程)\n");
        return 1;
    }

    /* 计算内存 */
    size_t hot_mem = N_particles * sizeof(Vec3) * 3;  // coord + force + vel
    size_t cold_mem = N_particles * (sizeof(double) + sizeof(int))
                    + N_particles * 4 * sizeof(int)   // bond_list
                    + 10 * sizeof(double);              // topology
    size_t total_mem = hot_mem + cold_mem;

    printf("============================================================\n");
    printf("  mini_md_v2: OpenMP-parallel MD micro-benchmark\n");
    printf("  Particles: %d | Steps: %d | Threads: %d\n",
           N_particles, N_steps, N_threads);
    printf("  Box: %.1f | Cutoff: %.1f\n", cbrt((double)N_particles) * 1.5, cutoff);
    printf("  Memory: %.1f MB (hot: %.1f MB, cold: %.1f MB)\n",
           (double)total_mem / (1024*1024),
           (double)hot_mem / (1024*1024),
           (double)cold_mem / (1024*1024));

    /* NUMA 信息 */
    if (numa_available() >= 0) {
        printf("  NUMA: %d nodes\n", numa_max_node() + 1);
    }
    printf("============================================================\n\n");

    omp_set_num_threads(N_threads);

    /* 初始化 */
    init_data();

    /* 预热一步（让 page fault 完成）*/
    printf("[预热] 1 step ... ");
    fflush(stdout);
    compute_nonbonded_forces();
    compute_bond_forces();
    printf("done\n");

    /* 主模拟 */
    printf("[运行] %d steps with %d threads ... \n", N_steps, N_threads);
    double elapsed = run_simulation();

    /* 结果 */
    double ke = 0.0;
    #pragma omp parallel for reduction(+:ke) num_threads(N_threads)
    for (int i = 0; i < N_particles; i++) {
        ke += 0.5 * masses[i] * (velocities[i].x * velocities[i].x +
                                  velocities[i].y * velocities[i].y +
                                  velocities[i].z * velocities[i].z);
    }

    printf("\n============================================================\n");
    printf("  Elapsed:     %.3f seconds\n", elapsed);
    printf("  Steps/sec:   %.1f\n", N_steps / elapsed);
    printf("  ns/day:      %.1f\n", N_steps * 1e-6 / elapsed * 86400);
    printf("  KE:          %.2f\n", ke);

    /* NUMA 分布 */
    if (numa_available() >= 0) {
        printf("\n  --- NUMA Distribution ---\n");
        void *objs[] = {coordinates, forces, velocities, masses, particle_types, bond_list};
        const char *names[] = {"coordinates","forces","velocities","masses","particle_types","bond_list"};
        int hot[] = {1, 1, 1, 0, 0, 0};
        for (int i = 0; i < 6; i++) {
            int node = -1;
            get_mempolicy(&node, NULL, 0, objs[i], MPOL_F_ADDR);
            printf("    %-15s on Node %d  %s\n", names[i], node, hot[i] ? "[HOT]" : "[COLD]");
        }
    }
    printf("============================================================\n");

    free_data();
    return 0;
}
