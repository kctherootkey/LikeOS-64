# libaudio - the LikeOS audio client library (talks to audiod).
#
# Builds libaudio.so (linked against libc.so, so DT_NEEDED records it) and a
# static libaudio.a from the same position-independent objects.  The public
# headers are include/audio/audio.h and include/audio/protocol.h.
#
#   make             libaudio.so and libaudio.a
#   make host-test   build and run the protocol tests on the build machine
#                    (fake service over a socketpair, ASan + UBSan); output
#                    goes to HOST_TEST_DIR
#
# Copyright (C) 2026 The LikeOS Project

CC = gcc
LD = ld
AR = ar
HOSTCC = gcc

LIBC_DIR = ../libc

# Userland flags plus -fPIC.  Symbols are hidden by default; the la_* API is
# exported explicitly (LA_EXPORT), so internals never become ABI.
CFLAGS = -Wall -Wextra -Werror -O2 -nostdlib -fno-builtin -ffreestanding \
         -fstack-protector-strong -mno-red-zone -fPIC -fvisibility=hidden \
         -Iinclude -Isrc -I$(LIBC_DIR)/include

LDFLAGS = -shared -nostdlib -Bsymbolic --hash-style=gnu -soname libaudio.so \
          -L$(LIBC_DIR) -lc

SRC = src/common.c src/stream.c src/control.c src/event.c
OBJ = $(SRC:.c=.o)
HDRS = include/audio/audio.h include/audio/protocol.h src/la_internal.h

all: libaudio.so libaudio.a

libaudio.so: $(OBJ)
	$(LD) -o $@ $(OBJ) $(LDFLAGS)

libaudio.a: $(OBJ)
	rm -f $@
	$(AR) rcs $@ $(OBJ)

# Objects follow header changes (protocol structures are compiled in).
$(OBJ): $(HDRS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- host tests -------------------------------------------------------------
HOST_TEST_DIR ?= /tmp/libaudio-host-test
HOST_CFLAGS = -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer \
              -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -Iinclude -Isrc

host-test: $(HOST_TEST_DIR)/test_libaudio
	timeout 300 nice -n 19 $(HOST_TEST_DIR)/test_libaudio

$(HOST_TEST_DIR)/test_libaudio: tests/test_libaudio.c $(SRC) $(HDRS)
	mkdir -p $(HOST_TEST_DIR)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_libaudio.c $(SRC) -lpthread

clean:
	rm -f $(OBJ) libaudio.so libaudio.a

.PHONY: all clean host-test
