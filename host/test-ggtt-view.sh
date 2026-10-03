#!/bin/sh
# Host test for the page order of a rotated GGTT view
# (kernel/dev/gpu/i915/i915_ggtt_view.c): the view's pages against a
# model built from mapping runs, the turned geometry, and the checks of
# what an object can hold.  The file's pure functions are cut out with
# sed and compiled with the build host's compiler.
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-ggtt-view-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
H=include/kernel/dev/gpu/i915/i915_ggtt_view.h
C=kernel/dev/gpu/i915/i915_ggtt_view.c
{
	echo '#include <stdint.h>'
	sed -n '/^struct i915_rotation_plane {/,/^};/p' $H
	sed -n '/^#define I915_ROTATION_PLANES/p;/^#define I915_ROTATION_PADDING/p' $H
	sed -n '/^struct i915_rotation_info {/,/^};/p' $H
	sed -n '/^uint64_t i915_rotation_info_size/,/^}/p' $C
	sed -n '/^uint64_t i915_rotation_view_page/,/^}/p' $C
	sed -n '/^int i915_rotation_info_valid/,/^}/p' $C
	cat host/test-ggtt-view.c
} > "$TMP/test.c"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -o "$TMP/test" "$TMP/test.c"
timeout 120 nice -n 19 "$TMP/test"
