ARCH ?= $(shell uname -m)

CC = gcc
CFLAGS = -I. -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing -O3

ifeq ($(ARCH),aarch64)
CFLAGS += -march=armv8.2-a
else
CFLAGS += -march=native
endif

LDLIBS = -lpthread -lm

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
