ARCH ?= $(shell uname -m)

CXX = g++ -std=c++14
CFLAGS = -Wall -g -ggdb3 -O0 -fno-omit-frame-pointer -rdynamic

# 优先使用本地系统 libnuma，fallback 到自编译版本
ifeq ($(wildcard /usr/include/numa.h),/usr/include/numa.h)
  # 系统 libnuma-dev 已安装
  LDFLAGS = -lnuma -ldl -lpthread
else
  NUMA_PREFIX ?= $(HOME)/kumf/numactl-install
  CFLAGS += -I$(NUMA_PREFIX)/include
  LDFLAGS = -L$(NUMA_PREFIX)/lib -lnuma -ldl -lpthread -Wl,-rpath,$(NUMA_PREFIX)/lib
endif

ifeq ($(ARCH),aarch64)
CFLAGS += -DARM64
endif

.PHONY: all clean
all: ldlib.so

ldlib.so: ldlib.c
	$(CXX) -fPIC $(CFLAGS) -c ldlib.c -o ldlib.o
	$(CXX) -shared -Wl,-soname,libkumf_interc.so -o ldlib.so ldlib.o $(LDFLAGS)

clean:
	rm -f ldlib.o ldlib.so
