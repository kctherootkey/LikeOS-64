# devd - the LikeOS device manager, and devctl, its command-line client
# Copyright (C) 2026 The LikeOS Project
#
# Built and linked exactly like the programs in user/bin: PIE, crt1.o,
# dynamically against libc.so through ld-likeos.so.

CC = gcc
LD = ld

LIBC_DIR = ../../lib/libc
RTLD_DIR = ../../lib/rtld
BIN_DIR = ../../bin

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -Werror=format-security -O2 -nostdlib \
	-fno-builtin -fstack-protector-strong \
	-mno-red-zone -ffreestanding -fPIE -I$(LIBC_DIR)/include \
	-fno-tree-vectorize -fno-strict-aliasing

LDSCRIPT = $(BIN_DIR)/user_dyn.lds

LDFLAGS = -pie -nostdlib -T $(LDSCRIPT) --hash-style=gnu \
	-z now -z relro --eh-frame-hdr \
	-rpath /lib -dynamic-linker /lib/ld-likeos.so \
	-L$(LIBC_DIR) -L$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o
LIBS = -lc -l:ld-likeos.so

DEVD_SRCS = devd.c devproto.c names.c probe.c fstab.c devtable.c mount.c \
	format.c auth.c events.c clients.c util.c
DEVCTL_SRCS = devctl.c devproto.c
DEVD_OBJS = $(DEVD_SRCS:.c=.o)
DEVCTL_OBJS = $(DEVCTL_SRCS:.c=.o)

all: devd devctl

devd: $(CRT1) $(DEVD_OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) $(DEVD_OBJS) $(LIBS) -o $@

devctl: $(CRT1) $(DEVCTL_OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) $(DEVCTL_OBJS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Host unit tests (sanitizers; run on the build machine, touch nothing)
test:
	$(MAKE) -C tests run

clean:
	rm -f $(DEVD_OBJS) $(DEVCTL_OBJS) devctl.d $(DEVD_OBJS:.o=.d) devd devctl
	$(MAKE) -C tests clean

.PHONY: all clean test

-include $(wildcard *.d)
