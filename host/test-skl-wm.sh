#!/bin/sh
# Host test for the display watermark arithmetic
# (kernel/dev/gpu/i915/display/intel_display.c).  A tiled surface is
# fetched in whole tile rows and needs a hundred times the buffered data
# a linear one does; computing it with the linear formula starves the
# plane and the panel shows stripes.  The computation is pure integer
# arithmetic, so the build host runs it against the numbers of the
# machine this was written for.  Run from the repository root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-sklwm-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
{
	echo '#include <stdint.h>'
	echo 'static uint32_t div_round_up(uint64_t a, uint64_t b) { return (uint32_t)((a + b - 1) / b); }'
	sed -n '/^#define WM_FP(x)/p' kernel/dev/gpu/i915/display/intel_display.c
	sed -n '/^static uint32_t wm_round_up/,/^}/p' kernel/dev/gpu/i915/display/intel_display.c
	sed -n '/^static uint32_t y_min_scanlines_for/,/^}/p' kernel/dev/gpu/i915/display/intel_display.c
	sed -n '/^struct skl_wm {/,/^};/p' kernel/dev/gpu/i915/display/intel_display.c
	sed -n '/^static struct skl_wm skl_wm_level/,/^}/p' kernel/dev/gpu/i915/display/intel_display.c
	cat host/test-skl-wm.c
} > "$TMP/test.c"
cc -std=gnu11 -Wall -Wextra -O1 -o "$TMP/test" "$TMP/test.c"
"$TMP/test"
