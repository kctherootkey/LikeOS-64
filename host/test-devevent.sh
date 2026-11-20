#!/bin/sh
# Host test for the device event ring behind /dev/devevents
# (kernel/fs/devevent_ring.h).  The ring code is pure -- no locks, no kernel
# services -- so the build host's compiler runs it.  Run from the
# repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-devevent-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -I. -o "$TMP/test" \
	host/test-devevent.c
timeout 120 nice -n 19 "$TMP/test"
