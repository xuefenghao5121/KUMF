CXX = g++ -std=c++14
CFLAGS = -Wall -g -O2 -fno-omit-frame-pointer -rdynamic
LDFLAGS = -ldl -lpthread

.PHONY: all clean
all: libkumf_mlock.so

libkumf_mlock.so: ldlib.c
	$(CXX) -fPIC $(CFLAGS) -shared -Wl,-soname,libkumf_mlock.so -o $@ $< $(LDFLAGS)

clean:
	rm -f libkumf_mlock.so
