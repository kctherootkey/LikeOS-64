# likeos-install, the text-mode installer, for LikeOS.
#
# Compiled and linked like the programs in user/bin (see ../../Makefile):
# freestanding against the LikeOS C library headers, dynamic PIE linked
# against libc.so through ld-likeos.so.  The installer backend, libgpt and
# libzstd are linked in statically, libcrypt dynamically (the backend
# hashes the passwords with it).
#
# The top-level build (user/bin/installer/installer.mk, section "cli") passes
#   BACKEND_LIBS   the static libraries to link: libinstaller.a libgpt.a
#                  [libzstd.a], absolute paths
# The host build and tests are in Makefile.host.
#
# Copyright (C) 2026 The LikeOS Project

CC = gcc
LD = ld

LIBC_DIR = ../../../lib/libc
RTLD_DIR = ../../../lib/rtld
LIBCRYPT_DIR = ../../../lib/libcrypt
BACKEND_DIR = ../backend

BACKEND_LIBS ?=

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin \
	-fstack-protector-strong -mno-red-zone -ffreestanding -fPIE \
	-I$(LIBC_DIR)/include -I$(BACKEND_DIR)/include \
	-fno-tree-vectorize -fno-strict-aliasing

LDSCRIPT = ../../user_dyn.lds

LDFLAGS = -pie -nostdlib -T $(LDSCRIPT) --hash-style=gnu \
	-z now -z relro --eh-frame-hdr \
	-rpath /lib -dynamic-linker /lib/ld-likeos.so \
	-L$(LIBC_DIR) -L$(RTLD_DIR) -L$(LIBCRYPT_DIR) \
	-rpath-link $(LIBC_DIR):$(LIBCRYPT_DIR):$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o
LIBS = -lcrypt -lc -l:ld-likeos.so

SRC = likeos-install.c cli-ui.c cli-keyboard.c
OBJ = $(SRC:.c=.o)

all: likeos-install

likeos-install: $(CRT1) $(OBJ) $(BACKEND_LIBS) $(LDSCRIPT)
	@test -n "$(BACKEND_LIBS)" || { echo "likeos-install: BACKEND_LIBS is not set (build it through user/bin/installer/installer.mk: make build/likeos-install)" >&2; exit 1; }
	$(LD) $(LDFLAGS) $(CRT1) $(OBJ) $(BACKEND_LIBS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f likeos-install $(OBJ) $(OBJ:.o=.d)

-include $(wildcard *.d)

.PHONY: all clean
