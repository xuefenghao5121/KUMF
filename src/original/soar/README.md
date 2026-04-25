# SOAR: Ranking-based Static Object Allocation for Tiered Memory Architectures

```
  __|   _ \    \    _ \
\__ \  (   |  _ \     /
____/ \___/ _/  _\ _|_\
```

SOAR is a tiered memory management policy that performs static object
allocation based on ranking. It profiles per-object performance contribution to
determine optimal object placement across memory tiers for near-optimal
performance.

## Overview

SOAR consists of three main phases:
1. **Profiling**: Track allocation/deallocation patterns, memory access behavior, and AOL-based performance prediction
2. **Analysis**: Process profiling data to rank objects by performance contributions
3. **Allocation**: Apply ranking results to guide object placement in tiered memory systems

## Directory Structure

```
soar/
├── README.md           # This file
├── prof/              # Profiling infrastructure
│   ├── ldlib.c        # Memory allocation/deallocation tracker
│   └── Makefile       # Build configuration
├── interc/            # Object placement controller
│   ├── ldlib.c        # Memory allocation interceptor with placement logic
│   └── Makefile       # Build configuration
├── run/               # Execution scripts and utilities
│   ├── prof.sh        # Profiling script template
│   ├── proc_obj_e.py  # Analysis script for processing profiling data
│   └── config.sh      # CXL configuration settings
└── patches/           # Kernel and application patches
    ├── gapbs.patch    # GAPBS benchmark patch
    └── nbt.patch      # Kernel patch to collect PEBS records with timestamps and fix tiering bugs
```

## Setup

1. **Apply kernel patch (nbt.patch)** (for Linux v5.18):

Refer to `./setup.sh`


## Usage

### Phase 1: Profiling

Profile your application to collect allocation patterns and memory access data.

**Example with GAPBS benchmark:**

1. **Prepare the benchmark**:
   ```bash
   cd /path/to/gapbs
   patch -p1 < /path/to/soar/patches/gapbs.patch
   make
   ```

2. **Run profiling**:
   ```bash
   cd soar/run
   # Edit prof.sh to configure your application
   ./prof.sh # modify this template script to profile your application
   ```

   The `prof/` directory contains the allocation/deallocation tracking infrastructure that will be dynamically linked with your application.

3. **Profiling outputs**:
   - Raw allocation data: `data.raw.*` files
   - Performance counters: perf output files
   - Memory access patterns: recorded in profiling logs

### Phase 2: Analysis

Process the collected profiling data to generate object rankings.

1. **Prepare analysis environment**:
   ```bash
   # Copy the analysis script to your profiling output directory
   cp run/proc_obj_e.py /path/to/profiling/output/
   cd /path/to/profiling/output/
   ```

2. **Run analysis**:
   ```bash
   python3 proc_obj_e.py [directory]
   ```

   Where `[directory]` contains the raw profiling data. Ensure perf output files are in the parent directory.

3. **Analysis outputs**:

- `obj_stat.csv`: Object ranking results
  - **Object ID**: Unique identifier for tracked objects
  - **Access Frequency**: Number of memory accesses
  - **Allocation Size**: Total memory allocated
  - **Object Score**: ranking score based on performance contribution

### Phase 3: Allocation

Apply the ranking results to guide object placement in your application.

1. **Configure object placement**:

   Edit `interc/ldlib.c` and modify the `check_trace` function to implement your placement policy:
   - Return `0`: Allocate on fast/local memory tier
   - Return `1`: Allocate on slow/remote memory tier
   - Return `-1`: Use default allocation (partly local, partly remote)

2. **Build the allocation controller**:
   ```bash
   cd interc
   make
   ```
3. **Run with controlled allocation**:

- Assign object placement with the ranking result. The `check_trace` in
  `interc/ldlib.c`: 0 is local | 1 is remote | -1 is `partly local and partly
  remote`.
- Compile files in `interc`.
- Use scripts in [run](../../run) to run the workload.

