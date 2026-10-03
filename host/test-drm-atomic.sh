#!/bin/sh
# Host test for the display-manager core's atomic mode setting
# (kernel/dev/gpu/drm/drm_atomic.c) and plane handling (drm_plane.c): DPMS
# that keeps the mode and the planes, the enabled/connector/plane rules,
# the legacy calls as states, the cursor's object check and the cursor hot
# spot.  Runs over the real property, mode-object, blending, colour and
# plane-check code; fences, modes and framebuffers are stubs in the test.
# The kernel's locking header is replaced by the fence test's prelude.
# Run from the repository root.  Extra compiler options (e.g. a switch set
# to 0: -DDRM_ATOMIC_ENABLE_SEMANTICS=0) may be given as arguments.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-atomic-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g "$@" \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-drm-atomic.c kernel/dev/gpu/drm/drm_atomic.c \
	kernel/dev/gpu/drm/drm_atomic_helper.c kernel/dev/gpu/drm/drm_damage_helper.c \
	kernel/dev/gpu/drm/drm_rect.c kernel/dev/gpu/drm/drm_plane.c \
	kernel/dev/gpu/drm/drm_blend.c kernel/dev/gpu/drm/drm_color_mgmt.c \
	kernel/dev/gpu/drm/drm_property.c kernel/dev/gpu/drm/drm_mode_object.c
timeout 120 nice -n 19 "$TMP/test"
