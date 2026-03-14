# Makefile for ncurses.so - ncurses-compatible library for LikeOS
#
# Builds a shared library providing the ncurses API using ANSI escape sequences.
# Based on ncurses 6.5 API (source in ../ncurses-6.5/).

BASEDIR = ../../..
LIBC_DIR = $(BASEDIR)/userland/libc

CC = gcc
LD = ld

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -fPIC -I$(LIBC_DIR)/include -I.

SRC = curses.c
OBJ = curses.pic.o

TARGET = ncurses.so

all: $(TARGET)

curses.pic.o: curses.c curses.h ncurses.h term.h
	$(CC) $(CFLAGS) -c curses.c -o curses.pic.o

$(TARGET): $(OBJ)
	$(LD) -shared -nostdlib -Bsymbolic-functions --hash-style=gnu \
	    -soname ncurses.so -o $@ $^

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
