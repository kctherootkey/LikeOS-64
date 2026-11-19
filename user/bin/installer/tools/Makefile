# Installer tools for LikeOS: likeos-gpt (GUID partition tables) and
# likeos-efiboot (the firmware's boot entries).
#
# Compiled and linked like the programs in user/bin (see ../../Makefile):
# freestanding against the LikeOS C library headers, dynamic PIE linked
# against libc.so through ld-likeos.so, with libgpt.a linked in statically.
#
# Copyright (C) 2026 The LikeOS Project

CC = gcc
LD = ld

LIBC_DIR = ../../../lib/libc
RTLD_DIR = ../../../lib/rtld
LIBGPT_DIR = ../../../lib/libgpt

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin \
	-fstack-protector-strong -mno-red-zone -ffreestanding -fPIE \
	-I$(LIBC_DIR)/include -I$(LIBGPT_DIR)/include \
	-fno-tree-vectorize -fno-strict-aliasing

LDSCRIPT = ../../user_dyn.lds

LDFLAGS = -pie -nostdlib -T $(LDSCRIPT) --hash-style=gnu \
	-z now -z relro --eh-frame-hdr \
	-rpath /lib -dynamic-linker /lib/ld-likeos.so \
	-L$(LIBC_DIR) -L$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o
LIBS = -lc -l:ld-likeos.so

PROGRAMS = likeos-gpt likeos-efiboot

# The device path and load option code shared with the boot loader and the
# kernel (freestanding C, compiled here like the tool itself).
BOOT_DIR = ../../../../boot

all: $(PROGRAMS)

# libgpt.a is built by its own Makefile, every time (it knows what is
# up to date); the FORCE keeps make from skipping that check.
$(LIBGPT_DIR)/libgpt.a: FORCE
	$(MAKE) -C $(LIBGPT_DIR) libgpt.a

likeos-gpt: $(CRT1) likeos-gpt.o $(LIBGPT_DIR)/libgpt.a $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) likeos-gpt.o $(LIBGPT_DIR)/libgpt.a $(LIBS) -o $@

likeos-efiboot: $(CRT1) likeos-efiboot.o efi_loadopt.o $(LIBGPT_DIR)/libgpt.a $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) likeos-efiboot.o efi_loadopt.o $(LIBGPT_DIR)/libgpt.a $(LIBS) -o $@

efi_loadopt.o: $(BOOT_DIR)/efi_loadopt.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(PROGRAMS) *.o *.d

-include $(wildcard *.d)

FORCE:

.PHONY: all clean FORCE
