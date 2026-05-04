# KUMF - Kunpeng Unified Memory Framework
# 顶层 Makefile

ARCH ?= $(shell uname -m)
BUILD_DIR ?= build

CXX ?= g++
CC ?= gcc
CXXSTD ?= c++14

# 通用编译选项
CFLAGS_COMMON = -Wall -g -ggdb3 -fno-omit-frame-pointer -rdynamic
CFLAGS_OPT ?= -O0

# libnuma 检测
ifeq ($(wildcard /usr/include/numa.h),/usr/include/numa.h)
  NUMA_CFLAGS =
  NUMA_LDFLAGS = -lnuma
else
  NUMA_PREFIX ?= $(HOME)/kumf/numactl-install
  NUMA_CFLAGS = -I$(NUMA_PREFIX)/include
  NUMA_LDFLAGS = -L$(NUMA_PREFIX)/lib -lnuma -Wl,-rpath,$(NUMA_PREFIX)/lib
endif

# ARM64 特殊标志
ifeq ($(ARCH),aarch64)
  CFLAGS_COMMON += -DARM64
endif

.PHONY: all libs workloads tools clean help

all: libs workloads tools

help:
	@echo "KUMF Build System"
	@echo ""
	@echo "  make          - Build everything"
	@echo "  make libs     - Build LD_PRELOAD libraries (interc, prof, mlock)"
	@echo "  make workloads - Build test workloads"
	@echo "  make tools    - Build SPE profiling tools"
	@echo "  make clean    - Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  ARCH=$(ARCH)"
	@echo "  CXX=$(CXX)"
	@echo "  CFLAGS_OPT=$(CFLAGS_OPT)  (change to -O2 for release)"

# ============================================================
# LD_PRELOAD Libraries
# ============================================================

libs: $(BUILD_DIR)/libkumf_interc.so $(BUILD_DIR)/libkumf_prof.so $(BUILD_DIR)/libkumf_mlock.so

# interc - NUMA 路由
$(BUILD_DIR)/libkumf_interc.so: src/lib/interc/ldlib.c | $(BUILD_DIR)
	$(CXX) -std=$(CXXSTD) -fPIC $(CFLAGS_COMMON) $(CFLAGS_OPT) $(NUMA_CFLAGS) \
		-c $< -o $(BUILD_DIR)/interc.o
	$(CXX) -shared -Wl,-soname,libkumf_interc.so \
		-o $@ $(BUILD_DIR)/interc.o $(NUMA_LDFLAGS) -ldl -lpthread

# prof - 分配追踪
$(BUILD_DIR)/libkumf_prof.so: src/lib/prof/ldlib.c | $(BUILD_DIR)
	$(CXX) -std=$(CXXSTD) -fPIC $(CFLAGS_COMMON) $(CFLAGS_OPT) $(NUMA_CFLAGS) \
		-c $< -o $(BUILD_DIR)/prof.o
	$(CXX) -shared -Wl,-soname,libkumf_prof.so \
		-o $@ $(BUILD_DIR)/prof.o $(NUMA_LDFLAGS) -ldl -lpthread

# mlock - 热页锁定
$(BUILD_DIR)/libkumf_mlock.so: src/lib/mlock/ldlib.c | $(BUILD_DIR)
	$(CXX) -std=$(CXXSTD) -fPIC $(CFLAGS_COMMON) -O2 \
		-c $< -o $(BUILD_DIR)/mlock.o
	$(CXX) -shared -Wl,-soname,libkumf_mlock.so \
		-o $@ $(BUILD_DIR)/mlock.o -ldl -lpthread

# ============================================================
# Test Workloads
# ============================================================

workloads: $(BUILD_DIR)/kumf_tiered $(BUILD_DIR)/kumf_stream $(BUILD_DIR)/mini_md_v2

$(BUILD_DIR)/kumf_tiered: workloads/kumf_tiered.c | $(BUILD_DIR)
	$(CC) -Wall -O2 -g -fopenmp -D_GNU_SOURCE $(NUMA_CFLAGS) \
		-o $@ $< $(NUMA_LDFLAGS) -lm -lpthread

$(BUILD_DIR)/kumf_stream: workloads/kumf_stream.c | $(BUILD_DIR)
	$(CC) -Wall -O2 -g -fopenmp -D_GNU_SOURCE $(NUMA_CFLAGS) \
		-o $@ $< $(NUMA_LDFLAGS) -lm -lpthread

$(BUILD_DIR)/mini_md_v2: workloads/mini_md_v2.c | $(BUILD_DIR)
	$(CC) -Wall -O2 -g -fopenmp -D_GNU_SOURCE $(NUMA_CFLAGS) \
		-o $@ $< $(NUMA_LDFLAGS) -lm -lpthread

# ============================================================
# SPE Profiling Tools
# ============================================================

tools: $(BUILD_DIR)/spe_self_profile $(BUILD_DIR)/spe_preload.so

$(BUILD_DIR)/spe_self_profile: src/tools/spe_self_profile.c | $(BUILD_DIR)
	$(CC) -Wall -O2 -g -D_GNU_SOURCE -o $@ $< -lpthread

$(BUILD_DIR)/spe_preload.so: src/tools/spe_preload.c src/tools/spe_self_profile.c | $(BUILD_DIR)
	$(CC) -Wall -O2 -g -D_GNU_SOURCE -shared -fPIC \
		-o $@ src/tools/spe_preload.c src/tools/spe_self_profile.c -lpthread

# ============================================================
# Utils
# ============================================================

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
