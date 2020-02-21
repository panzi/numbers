CC=gcc
CFLAGS=-Wall -O2 -Wextra -Werror -std=gnu11 -lpthread

ifeq ($(DEBUG),ON)
	CFLAGS+=-g -DDEBUG
else
	CFLAGS+=-DNDEBUG
endif

.PHONY: all clean test

all: build/numbers

test: build/numbers
	./test.py

build/numbers: build/numbers.o
	$(CC) $(CFLAGS) $< -o $@

build/numbers.o: src/numbers.c src/panic.h
	$(CC) $(CFLAGS) $< -o $@ -c

clean:
	rm -rv build/numbers.o build/numbers
