#!/bin/sh
# Host test for the display-manager core's fence library: callbacks, fence
# arrays and chains, unwrapping and merging, reservation objects
# (kernel/dev/gpu/drm/drm_fence*.c, drm_resv.c).  The kernel's locking
# header is replaced by a prelude whose spinlocks abort on recursion; the
# few kernel services used are stubs.  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-fence-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-drm-fence.c host/test-drm-fence-stubs.c \
	kernel/dev/gpu/drm/drm_fence.c kernel/dev/gpu/drm/drm_fence_array.c \
	kernel/dev/gpu/drm/drm_fence_chain.c kernel/dev/gpu/drm/drm_fence_unwrap.c \
	kernel/dev/gpu/drm/drm_resv.c
timeout 120 nice -n 19 "$TMP/test"
