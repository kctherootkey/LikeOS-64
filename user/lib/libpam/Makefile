# Copyright (C) 2026 The LikeOS Project

CC = gcc
LD = ld

LIBC_DIR = ../libc
LIBCRYPT_DIR = ../libcrypt

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -fPIC \
         -Iinclude -I$(LIBC_DIR)/include

# DT_NEEDED libc.so (malloc/string/stdio, pwd/shadow db) and libcrypt.so (crypt).
LDFLAGS = -shared -nostdlib -Bsymbolic --hash-style=gnu -soname libpam.so \
          -L$(LIBC_DIR) -lc -L$(LIBCRYPT_DIR) -lcrypt

SRC = pam.c
OBJ = $(SRC:.c=.o)
OUT = libpam.so

all: $(OUT)

$(OUT): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c include/security/pam_appl.h
	$(CC) $(CFLAGS) -c $< -o $@

# ---- host tests -------------------------------------------------------------
# make host-test   pam.c built for the build machine and driven through its
#                  API (tests/test_pam.c, ASan + UBSan); output goes to
#                  HOST_TEST_DIR.  The password check reads /etc/shadow and is
#                  tested on LikeOS by test_libc.
HOSTCC = gcc
HOST_TEST_DIR ?= /tmp/libpam-host-test
HOST_CFLAGS = -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-omit-frame-pointer \
              -fsanitize=address,undefined -fno-sanitize-recover=undefined \
              -D_GNU_SOURCE -Iinclude

host-test: $(HOST_TEST_DIR)/test_pam
	timeout 300 nice -n 19 $(HOST_TEST_DIR)/test_pam

$(HOST_TEST_DIR)/test_pam: tests/test_pam.c pam.c include/security/pam_appl.h
	mkdir -p $(HOST_TEST_DIR)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_pam.c pam.c -lcrypt

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: all clean host-test
