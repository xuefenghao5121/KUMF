ARCH ?= $(shell uname -m)
NUMA_PREFIX ?= $(HOME)/kumf/numactl-install

CC = gcc
CFLAGS = -I. -I$(NUMA_PREFIX)/include -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing -O3

ifeq ($(ARCH),aarch64)
CFLAGS += -march=armv8.2-a
else
CFLAGS += -march=native
endif

LDLIBS = -L$(NUMA_PREFIX)/lib -lpthread -lnuma -lm -Wl,-rpath,$(NUMA_PREFIX)/lib

DEPS = utils.h
OBJ = utils.o main.o

.PHONY: all
all: bench

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

bench: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LDLIBS)

.PHONY: clean
clean:
	rm -f *~ *.o bench
