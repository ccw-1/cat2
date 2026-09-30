CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2

SRCS = cat2.c tables.c
OBJS = cat2.o tables.o

all: cat2 cat2-mcp

libcat2.a: $(OBJS)
	ar rcs $@ $(OBJS)

cat2: main.o libcat2.a
	$(CC) $(CFLAGS) -o $@ main.o libcat2.a

cat2-mcp: mcp.o libcat2.a
	$(CC) $(CFLAGS) -o $@ mcp.o libcat2.a

test_runner: test.o libcat2.a
	$(CC) $(CFLAGS) -o $@ test.o libcat2.a

test: test_runner
	./test_runner

clean:
	rm -f *.o *.a cat2 cat2-mcp test_runner

.PHONY: all test clean
