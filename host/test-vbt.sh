#!/bin/sh
# Host test for the Intel graphics driver's VBT parser
# (kernel/dev/gpu/i915/display/intel_vbt_parse.c), which is pure.  Builds a
# synthetic table with the blocks the driver reads and checks what comes out.
set -e
TMP=${TMPDIR:-/tmp}/likeos-vbt-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-vbt.c \
	kernel/dev/gpu/i915/display/intel_vbt_parse.c kernel/dev/gpu/drm/drm_modes.c
"$TMP/test"
