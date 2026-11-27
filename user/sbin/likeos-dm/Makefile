# likeos-dm - runs the graphical login (LightDM) or a text login on the
# console, and switches between them; likeos-session-switch - the setuid
# helper that asks it to.  likeos-dm is started by init from inittab.
# Copyright (C) 2026 The LikeOS Project
#
# Built and linked like the programs in user/bin: PIE, crt1.o, dynamically
# against libc.so through ld-likeos.so.
#
#   make             both programs
#   make host-test   the decisions and the request format (dmcore.c) on the
#                    build machine (ASan + UBSan), tests/test_dmcore.c

CC = gcc
LD = ld
HOSTCC = gcc

LIBC_DIR = ../../lib/libc
RTLD_DIR = ../../lib/rtld
BIN_DIR = ../../bin

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -Werror=format-security -O2 -nostdlib \
	-fno-builtin -fstack-protector-strong \
	-mno-red-zone -ffreestanding -fPIE -D__LIKEOS__ -I$(LIBC_DIR)/include \
	-fno-tree-vectorize -fno-strict-aliasing

LDSCRIPT = $(BIN_DIR)/user_dyn.lds

LDFLAGS = -pie -nostdlib -T $(LDSCRIPT) --hash-style=gnu \
	-z now -z relro --eh-frame-hdr \
	-rpath /lib -dynamic-linker /lib/ld-likeos.so \
	-L$(LIBC_DIR) -L$(RTLD_DIR) \
	-rpath-link $(LIBC_DIR):$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o
LIBS = -lc -l:ld-likeos.so

DM_OBJS = likeos-dm.o dmcore.o
SWITCH_OBJS = session-switch.o dmcore.o

all: likeos-dm likeos-session-switch

likeos-dm: $(CRT1) $(DM_OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) $(DM_OBJS) $(LIBS) -o $@

likeos-session-switch: $(CRT1) $(SWITCH_OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) $(SWITCH_OBJS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- host tests -------------------------------------------------------------
HOST_TEST_DIR ?= /tmp/likeos-dm-host-test
HOST_CFLAGS = -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-D_GNU_SOURCE -I.

host-test: $(HOST_TEST_DIR)/test_dmcore
	timeout 300 nice -n 19 $(HOST_TEST_DIR)/test_dmcore

$(HOST_TEST_DIR)/test_dmcore: tests/test_dmcore.c dmcore.c dmcore.h
	mkdir -p $(HOST_TEST_DIR)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_dmcore.c dmcore.c

clean:
	rm -f *.o *.d likeos-dm likeos-session-switch

.PHONY: all clean host-test

-include $(wildcard *.d)
