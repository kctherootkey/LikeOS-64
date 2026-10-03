#!/bin/sh
# Host test for the Intel display's plane arithmetic
# (kernel/dev/gpu/i915/display/intel_plane.c): the rotation and flip
# encodings of PLANE_CTL, the tile-aligned surface offset and its
# re-adjustment, the layout of a rotated GGTT view, and the clipping of a
# 180-degree plane through the rectangle helpers
# (kernel/dev/gpu/drm/drm_rect.c).  The plane file's pure functions are cut
# out with sed and compiled with the build host's compiler.
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-intel-plane-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
P=kernel/dev/gpu/i915/display/intel_plane.c
{
	sed -n '/^struct intel_rotated_view_info {/,/^};/p' include/kernel/dev/gpu/i915/intel_plane.h
	sed -n '/^#define Y_TILE_BYTES/p;/^#define Y_TILE_ROWS/p;/^#define TILE_SIZE/p' $P
	sed -n '/^static uint32_t skl_plane_ctl_rotate/,/^}/p' $P
	sed -n '/^static uint32_t icl_plane_ctl_flip/,/^}/p' $P
	sed -n '/^static void adjust_tile_offset/,/^}/p' $P
	sed -n '/^static uint32_t compute_aligned_offset_tiled/,/^}/p' $P
	sed -n '/^static void rotated_view_layout/,/^}/p' $P
} > "$TMP/plane_cut.h"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -Iinclude -I"$TMP" -o "$TMP/test" \
	host/test-intel-plane.c kernel/dev/gpu/drm/drm_rect.c
timeout 120 nice -n 19 "$TMP/test"
