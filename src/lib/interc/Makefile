ARCH ?= $(shell uname -m)
NUMA_PREFIX ?= $(HOME)/kumf/numactl-install

CXX = g++ -std=c++14
CFLAGS = -Wall -g -ggdb3 -O0 -fno-omit-frame-pointer -rdynamic -I$(NUMA_PREFIX)/include

ifeq ($(ARCH),aarch64)
CFLAGS += -DARM64
endif

LDFLAGS = -L$(NUMA_PREFIX)/lib -lnuma -ldl -lpthread -Wl,-rpath,$(NUMA_PREFIX)/lib

.PHONY: all clean
all: ldlib.so

ldlib.so: ldlib.c
	$(CXX) -fPIC $(CFLAGS) -c ldlib.c -o ldlib.o
	$(CXX) -shared -Wl,-soname,libkumf_interc.so -o ldlib.so ldlib.o $(LDFLAGS)

clean:
	rm -f ldlib.o ldlib.so
