ARCH ?= $(shell uname -m)
NUMA_PREFIX ?= $(HOME)/kumf/numactl-install

CXX = g++ -std=c++14
CFLAGS = -Wall -g -ggdb3 -O0 -fno-omit-frame-pointer -rdynamic

# 优先使用系统 libnuma
ifeq ($(wildcard /usr/include/numa.h),/usr/include/numa.h)
  LDFLAGS = -lnuma -ldl -lpthread
else
  NUMA_PREFIX ?= $(HOME)/kumf/numactl-install
  CFLAGS += -I$(NUMA_PREFIX)/include
  LDFLAGS = -L$(NUMA_PREFIX)/lib -lnuma -ldl -lpthread -Wl,-rpath,$(NUMA_PREFIX)/lib
endif

.PHONY: all clean
all: ldlib.so

ldlib.so: ldlib.c
	$(CXX) -fPIC $(CFLAGS) -c ldlib.c -o ldlib.o
	$(CXX) -shared -Wl,-soname,libkumf_prof.so -o ldlib.so ldlib.o $(LDFLAGS)

clean:
	rm -f ldlib.o ldlib.so
