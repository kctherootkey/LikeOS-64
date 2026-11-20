# devd host unit tests, built with the build machine's compiler and the
# address and undefined-behaviour sanitizers.  The pure parts of the daemon
# (protocol, names, probes, filesystem table, authorization decision) and
# probes of small filesystem images made in $(OUT) with the build machine's
# mkfs tools when they are there.  No devices, sockets or mounts.
#
# Copyright (C) 2026 The LikeOS Project

HOSTCC ?= cc
OUT ?= .

SRC_DIR = ..

HOSTCFLAGS = -std=gnu11 -O1 -g -D_GNU_SOURCE -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I$(SRC_DIR)

UNITS = $(SRC_DIR)/devproto.c $(SRC_DIR)/names.c $(SRC_DIR)/probe.c \
	$(SRC_DIR)/fstab.c $(SRC_DIR)/auth.c $(SRC_DIR)/util.c

all: $(OUT)/test_devd

$(OUT)/test_devd: test_devd.c $(UNITS) $(wildcard $(SRC_DIR)/*.h)
	$(HOSTCC) $(HOSTCFLAGS) test_devd.c $(UNITS) -o $@

run: $(OUT)/test_devd
	rm -rf $(OUT)/img && mkdir -p $(OUT)/img
	timeout 120 nice -n 19 $(OUT)/test_devd $(OUT)/img

clean:
	rm -f $(OUT)/test_devd
	rm -rf $(OUT)/img

.PHONY: all run clean
