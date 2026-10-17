# audiod host unit tests, built with the build machine's compiler and the
# address and undefined-behaviour sanitizers.  They only compute: no files,
# sockets or devices are touched.
#
# Copyright (C) 2026 The LikeOS Project

HOSTCC ?= cc
OUT ?= .

SRC_DIR = ..
LIBAUDIO_INC = ../../../lib/libaudio/include

HOSTCFLAGS = -std=gnu11 -O1 -g -D_GNU_SOURCE -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I$(LIBAUDIO_INC) -I$(SRC_DIR)

UNITS = $(SRC_DIR)/convert.c $(SRC_DIR)/resample.c $(SRC_DIR)/stream.c \
	$(SRC_DIR)/mixer.c $(SRC_DIR)/proto.c $(SRC_DIR)/wav.c \
	$(SRC_DIR)/config.c $(SRC_DIR)/state.c $(SRC_DIR)/log.c

all: $(OUT)/test_audiod

$(OUT)/test_audiod: test_audiod.c $(UNITS) $(wildcard $(SRC_DIR)/*.h)
	$(HOSTCC) $(HOSTCFLAGS) test_audiod.c $(UNITS) -lm -o $@

run: $(OUT)/test_audiod
	timeout 300 nice -n 19 taskset -c 0 $(OUT)/test_audiod

clean:
	rm -f $(OUT)/test_audiod $(OUT)/audiod_host $(OUT)/test_daemon
	rm -rf $(OUT)/shim

.PHONY: all run clean

# ---- integration test: the whole daemon, built for the host ----------------
#
# Every path the daemon uses is moved under $(ITEST_DIR) (AUDIOD_HOST_TEST),
# there is no sound device on the build machine so the null sink plays, and
# the daemon runs unprivileged.  <sys/audio.h> comes from the kernel's
# interface header through a one-line shim generated in $(OUT).

# (a socket path must fit in 108 bytes: keep this directory short)
ITEST_DIR ?= $(abspath $(OUT))/itest
UAPI = $(abspath ../../../../include/kernel/uapi/sound.h)
DAEMON_SRCS = $(wildcard $(SRC_DIR)/*.c)

$(OUT)/shim/sys/audio.h: $(UAPI)
	mkdir -p $(OUT)/shim/sys
	echo '#include "$(UAPI)"' > $@

$(OUT)/audiod_host: $(DAEMON_SRCS) $(wildcard $(SRC_DIR)/*.h) $(OUT)/shim/sys/audio.h
	$(HOSTCC) $(HOSTCFLAGS) -I$(OUT)/shim -DAUDIOD_HOST_TEST='"$(ITEST_DIR)"' \
		$(DAEMON_SRCS) -lm -o $@

$(OUT)/test_daemon: test_daemon.c
	$(HOSTCC) -std=gnu11 -O1 -g -D_GNU_SOURCE -Wall -Wextra -Werror \
		-I$(LIBAUDIO_INC) test_daemon.c -lm -o $@

itest: $(OUT)/audiod_host $(OUT)/test_daemon
	rm -rf $(ITEST_DIR)
	mkdir -p $(ITEST_DIR)
	timeout 180 nice -n 19 taskset -c 0 \
		$(OUT)/test_daemon $(OUT)/audiod_host $(ITEST_DIR)

.PHONY: itest
# ITEST_DIR is compiled in: always rebuild the host daemon
.PHONY: $(OUT)/audiod_host
