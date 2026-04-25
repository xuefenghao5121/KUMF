CFLAGS=-Wall -g -ggdb3 -O0 -lnuma
LDFLAGS=-lnuma

.PHONY: all clean
all: makefile.dep ldlib.so

makefile.dep: *.[Cch]
	for i in *.[Cc]; do gcc -MM "$${i}" ${CFLAGS}; done > $@

-include makefile.dep

ldlib.so: ldlib.c
	g++ -fPIC ${CFLAGS} -c ldlib.c
	g++ -shared -Wl,-soname,libpmalloc.so -o ldlib.so ldlib.o -ldl -lpthread -lnuma

clean:
	rm -f *.o *.so makefile.dep
