# likeos-input-devices - keeps the X server's touchpad configuration
# (/etc/X11/xorg.conf.d/50-likeos-touchpads.conf) in step with the touchpads
# the kernel reports.  Started by init from inittab.
# Copyright (C) 2026 The LikeOS Project
#
# Built and linked like the programs in user/bin: PIE, crt1.o, dynamically
# against libc.so through ld-likeos.so, and against libudev.so.1
# (user/lib/libudev), which finds and classifies the touchpads.
#
#   make             the program
#   make host-test   the configuration generator on the build machine, over
#                    the device descriptions in user/lib/libudev/tests/fixtures
#                    (ASan + UBSan), compared with tests/expected/

CC = gcc
LD = ld
HOSTCC = gcc

LIBC_DIR = ../../lib/libc
RTLD_DIR = ../../lib/rtld
LIBUDEV_DIR = ../../lib/libudev
BIN_DIR = ../../bin

DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -Werror=format-security -O2 -nostdlib \
	-fno-builtin -fstack-protector-strong \
	-mno-red-zone -ffreestanding -fPIE -D__LIKEOS__ -I$(LIBC_DIR)/include \
	-I$(LIBUDEV_DIR)/include -fno-tree-vectorize -fno-strict-aliasing

LDSCRIPT = $(BIN_DIR)/user_dyn.lds

LDFLAGS = -pie -nostdlib -T $(LDSCRIPT) --hash-style=gnu \
	-z now -z relro --eh-frame-hdr \
	-rpath /lib -dynamic-linker /lib/ld-likeos.so \
	-L$(LIBC_DIR) -L$(RTLD_DIR) -L$(LIBUDEV_DIR) \
	-rpath-link $(LIBC_DIR):$(LIBUDEV_DIR):$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o
LIBS = -ludev -lc -l:ld-likeos.so

SRCS = likeos-input-devices.c xconf.c
OBJS = $(SRCS:.c=.o)

all: likeos-input-devices

likeos-input-devices: $(CRT1) $(OBJS) $(LDSCRIPT) $(LIBUDEV_DIR)/libudev.so.1
	$(LD) $(LDFLAGS) $(CRT1) $(OBJS) $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- host tests -------------------------------------------------------------
HOST_TEST_DIR ?= /tmp/likeos-input-devices-host-test
LIBUDEV_SRC = $(LIBUDEV_DIR)/src/udev.c $(LIBUDEV_DIR)/src/input.c \
	$(LIBUDEV_DIR)/src/device.c $(LIBUDEV_DIR)/src/monitor.c \
	$(LIBUDEV_DIR)/src/enumerate.c
HOST_CFLAGS = -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-D_GNU_SOURCE -DLUDEV_TESTING -I. -I$(LIBUDEV_DIR)/include \
	-I$(LIBUDEV_DIR)/src -idirafter $(LIBC_DIR)/include

host-test: $(HOST_TEST_DIR)/test_xconf
	timeout 300 nice -n 19 $(HOST_TEST_DIR)/test_xconf \
		$(LIBUDEV_DIR)/tests/fixtures tests/expected

$(HOST_TEST_DIR)/test_xconf: tests/test_xconf.c xconf.c xconf.h $(LIBUDEV_SRC)
	mkdir -p $(HOST_TEST_DIR)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_xconf.c xconf.c $(LIBUDEV_SRC)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) likeos-input-devices

.PHONY: all clean host-test

-include $(wildcard *.d)
