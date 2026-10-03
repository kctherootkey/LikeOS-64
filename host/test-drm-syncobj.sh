#!/bin/sh
# Host test for synchronisation objects (kernel/dev/gpu/drm/drm_syncobj.c):
# timelines, lookups, the waits and their flags, transfers, queries.  Built
# on the fence library's prelude (spinlocks that abort on recursion) with
# stubs that simulate sleeping.  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-syncobj-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-include host/test-drm-syncobj-prelude.h -Iinclude -o "$TMP/test" \
	host/test-drm-syncobj.c host/test-drm-syncobj-stubs.c \
	kernel/dev/gpu/drm/drm_syncobj.c \
	kernel/dev/gpu/drm/drm_fence.c kernel/dev/gpu/drm/drm_fence_array.c \
	kernel/dev/gpu/drm/drm_fence_chain.c kernel/dev/gpu/drm/drm_fence_unwrap.c
timeout 120 nice -n 19 "$TMP/test"
