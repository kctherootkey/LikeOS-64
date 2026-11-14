# libgpt -- GUID partition tables (static library for LikeOS programs).
#
# Built like the other LikeOS libraries (see ../libcrypt/Makefile): the host
# gcc, freestanding, against the LikeOS C library headers.  The result is a
# static archive that likeos-gpt and libinstaller link in.
#
# Copyright (C) 2026 The LikeOS Project

CC = gcc
AR = ar

LIBC_DIR = ../libc

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin \
	-fstack-protector-strong -mno-red-zone -ffreestanding -fPIC \
	-fno-tree-vectorize -fno-strict-aliasing \
	-Iinclude -I. -I$(LIBC_DIR)/include

SRC = crc32.c guid.c blkio.c mbr.c gpt.c layout.c types.c
OBJ = $(SRC:.c=.o)
OUT = libgpt.a

all: $(OUT)

$(OUT): $(OBJ)
	rm -f $@
	$(AR) rcs $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(OUT)
	$(MAKE) -f Makefile.host clean

-include $(OBJ:.o=.d)

.PHONY: all clean
