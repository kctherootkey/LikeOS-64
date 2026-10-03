#!/bin/sh
# Host test for the display-manager core's damage iterator and plane-state
# checks (kernel/dev/gpu/drm/drm_damage_helper.c, drm_atomic_helper.c,
# with drm_rect.c under them).  They are libraries without locks or
# allocation, so the build host's compiler runs them against the published
# test vectors; the few kernel functions they call (kprintf, panic,
# mm_memset, drm_fb_lookup) are stubs in the test.  Run from the repository
# root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-damage-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -Iinclude -o "$TMP/test" \
	host/test-drm-damage.c kernel/dev/gpu/drm/drm_damage_helper.c \
	kernel/dev/gpu/drm/drm_atomic_helper.c kernel/dev/gpu/drm/drm_rect.c
timeout 120 nice -n 19 "$TMP/test"
