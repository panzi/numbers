CC=gcc
CFLAGS=-Wall -O2 -Wextra -Werror -std=gnu11 -lpthread

ifeq ($(DEBUG),ON)
	CFLAGS+=-g -DDEBUG
else
	CFLAGS+=-DNDEBUG
endif

.PHONY: all clean

all: build/numbers

build/numbers: build/numbers.o
	$(CC) $(CFLAGS) $< -o $@

build/numbers.o: src/numbers.c src/panic.h
	$(CC) $(CFLAGS) $< -o $@ -c

clean:
	rm -rv build/numbers.o build/numbers
