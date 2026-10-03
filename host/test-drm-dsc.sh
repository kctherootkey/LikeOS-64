#!/bin/sh
# Host test for the Display Stream Compression library
# (kernel/dev/gpu/drm/drm_dsc_helper.c), the display engine's QP tables
# (kernel/dev/gpu/i915/display/intel_qp_tables.c) and the DSC capability,
# verdict and configuration code (kernel/dev/gpu/i915/display/
# intel_dsc_caps.c).  The files are pure -- fixed-width types and integer
# arithmetic -- so the build host's compiler runs them against known-good
# vectors; kprintf and panic are stubs in the test, and the helper
# library's DSC capability decoders are restated there.
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-dsc-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -Iinclude -o "$TMP/test" \
	host/test-drm-dsc.c kernel/dev/gpu/drm/drm_dsc_helper.c \
	kernel/dev/gpu/i915/display/intel_qp_tables.c \
	kernel/dev/gpu/i915/display/intel_dsc_caps.c
timeout 120 nice -n 19 "$TMP/test"
