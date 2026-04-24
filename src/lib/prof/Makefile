ARCH ?= $(shell uname -m)

CXX = g++ -std=c++14
CFLAGS = -Wall -g -ggdb3 -O0 -fno-omit-frame-pointer -rdynamic

ifeq ($(ARCH),aarch64)
CFLAGS += -DARM64
else
CFLAGS += -DX86_64
endif

LDFLAGS = -ldl -lpthread

.PHONY: all clean
all: ldlib.so

ldlib.so: ldlib.c
	$(CXX) -fPIC $(CFLAGS) -c ldlib.c -o ldlib.o
	$(CXX) -shared -Wl,-soname,libkumf_prof.so -o ldlib.so ldlib.o $(LDFLAGS)

clean:
	rm -f ldlib.o ldlib.so
