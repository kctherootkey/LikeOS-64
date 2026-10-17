# audiod - the LikeOS audio service
# Copyright (C) 2026 The LikeOS Project
#
# Built and linked exactly like the programs in user/bin: PIE, crt1.o,
# dynamically against libc.so through ld-likeos.so.

CC = gcc
LD = ld

LIBC_DIR = ../../lib/libc
RTLD_DIR = ../../lib/rtld
LIBAUDIO_DIR = ../../lib/libaudio
BIN_DIR = ../../bin

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin -fstack-protector-strong \
	-mno-red-zone -ffreestanding -fPIE -I$(LIBC_DIR)/include \
	-I$(LIBAUDIO_DIR)/include \
	-fno-tree-vectorize -fno-strict-aliasing

LDSCRIPT = $(BIN_DIR)/user_dyn.lds

LDFLAGS = -pie -nostdlib -T $(LDSCRIPT) --hash-style=gnu \
	-z now -z relro --eh-frame-hdr \
	-rpath /lib -dynamic-linker /lib/ld-likeos.so \
	-L$(LIBC_DIR) -L$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o
LIBS = -lc -l:ld-likeos.so

SRCS = audiod.c device.c clients.c control.c events.c config.c state.c \
	log.c mixer.c convert.c resample.c stream.c proto.c wav.c
OBJS = $(SRCS:.c=.o)

all: audiod

audiod: $(CRT1) $(OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(CRT1) $(OBJS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Host unit tests (sanitizers; run on the build machine, touch nothing)
test:
	$(MAKE) -C tests run

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) audiod
	$(MAKE) -C tests clean

.PHONY: all clean test

-include $(wildcard *.d)
