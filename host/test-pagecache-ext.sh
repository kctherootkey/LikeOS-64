#!/bin/sh
# Host test for the page-cache extensions the NTFS / exFAT / ISO9660 drivers
# build on: pagecache_install / pagecache_lookup_held, the mapping check of
# the coalesced read for blocks smaller than a page, and delayed allocation
# at writeback (vfs_sb_ops_t.alloc_blocks).  The real kernel/fs/pagecache.c
# is compiled against a prelude whose spinlocks fail the test on recursion
# and whose might_sleep() fails it under a spinlock; a fake filesystem over a
# RAM disk stands in for a driver.  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-pagecache-ext-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-DDEBUG -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer \
	-include host/test-pagecache-ext-prelude.h -I. -o "$TMP/test" \
	host/test-pagecache-ext.c host/test-pagecache-ext-stubs.c \
	kernel/fs/pagecache.c
ASAN_OPTIONS=detect_leaks=1 timeout 120 nice -n 19 "$TMP/test"
