#!/bin/sh
# Host test for the display PLL's link-rate encoding
# (kernel/dev/gpu/i915/display/intel_dpll.c).  The rate field names half
# the port's clock; getting that wrong runs the link at twice the rate
# the sink was told, which is what a black panel looks like.  The two
# conversion functions are pure, so the build host's compiler runs them
# through a table of the rates the hardware has.  Run from the repository
# root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-dpll-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
# The two functions are file-static inside the driver; cut them out with
# their register defines, the way the other display harnesses do.
{
	sed -n '/^#define DPLL_CTRL1_LINK_RATE_2700/,/^#define DPLL_CTRL1_LINK_RATE_2160/p' \
		include/kernel/dev/gpu/i915/i915_reg.h
	sed -n '/^static int link_rate_code/,/^}/p' \
		kernel/dev/gpu/i915/display/intel_dpll.c
	sed -n '/^static uint32_t link_rate_of_code/,/^}/p' \
		kernel/dev/gpu/i915/display/intel_dpll.c
	cat host/test-dpll.c
} > "$TMP/test.c"
cc -std=gnu11 -Wall -Wextra -O1 -include stdint.h -o "$TMP/test" "$TMP/test.c"
"$TMP/test"
