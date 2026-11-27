# libudev - the LikeOS device-information library behind the libudev API
# (input device classification for libinput and its tools; see
# include/libudev.h).
#
# Builds libudev.so.1 (the soname every libudev user records; linked against
# libc.so, so DT_NEEDED records it) and the libudev.so link the linker looks
# for with -ludev.
#
#   make             libudev.so.1 and libudev.so
#   make host-test   build and run the tests on the build machine (fixture
#                    files from the kernel's device descriptions, ASan +
#                    UBSan); output goes to HOST_TEST_DIR
#
# Copyright (C) 2026 The LikeOS Project

CC = gcc
LD = ld
HOSTCC = gcc

LIBC_DIR = ../libc

# Userland flags plus -fPIC.  Symbols are hidden by default; the udev_* API
# is exported explicitly (LUDEV_EXPORT), so internals never become ABI.
CFLAGS = -Wall -Wextra -Werror -O2 -nostdlib -fno-builtin -ffreestanding \
         -fstack-protector-strong -mno-red-zone -fPIC -fvisibility=hidden \
         -fno-strict-aliasing -D__LIKEOS__ -D_GNU_SOURCE \
         -Iinclude -Isrc -I$(LIBC_DIR)/include

LDFLAGS = -shared -nostdlib -Bsymbolic --hash-style=gnu -z relro \
          -soname libudev.so.1 -L$(LIBC_DIR) -lc

SRC = src/udev.c src/input.c src/device.c src/monitor.c src/enumerate.c
OBJ = $(SRC:.c=.o)
HDRS = include/libudev.h src/ludev.h

all: libudev.so.1 libudev.so

libudev.so.1: $(OBJ)
	$(LD) -o $@ $(OBJ) $(LDFLAGS)

libudev.so: libudev.so.1
	ln -sf libudev.so.1 $@

$(OBJ): $(HDRS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- host tests -------------------------------------------------------------
# The library's sources compiled for the build machine with LUDEV_TESTING
# (fixture paths instead of /proc and /dev), against the build machine's
# <linux/input.h>; <sys/devevent.h> comes from this system's libc headers,
# searched last so they never shadow the build machine's own.
HOST_TEST_DIR ?= /tmp/libudev-host-test
HOST_CFLAGS = -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer \
              -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -D_GNU_SOURCE -DLUDEV_TESTING -Iinclude -Isrc \
              -idirafter $(LIBC_DIR)/include

host-test: $(HOST_TEST_DIR)/test_libudev
	timeout 300 nice -n 19 $(HOST_TEST_DIR)/test_libudev tests/fixtures

$(HOST_TEST_DIR)/test_libudev: tests/test_libudev.c $(SRC) $(HDRS)
	mkdir -p $(HOST_TEST_DIR)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_libudev.c $(SRC)

clean:
	rm -f $(OBJ) libudev.so.1 libudev.so

.PHONY: all clean host-test
