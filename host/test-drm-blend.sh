#!/bin/sh
# Host test for the display-manager core's plane blending and colour
# management helpers (kernel/dev/gpu/drm/drm_blend.c, drm_color_mgmt.c),
# run over the real property table (drm_property.c, drm_mode_object.c).
# The kernel's locking header is replaced by the fence test's prelude
# (spinlocks that abort on recursion); the few kernel services used are
# stubs in the test.  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-blend-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-drm-blend.c kernel/dev/gpu/drm/drm_blend.c \
	kernel/dev/gpu/drm/drm_color_mgmt.c kernel/dev/gpu/drm/drm_property.c \
	kernel/dev/gpu/drm/drm_mode_object.c
timeout 120 nice -n 19 "$TMP/test"
