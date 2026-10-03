#!/bin/sh
# Host test for the colour arithmetic of the Intel displays before DDI
# (kernel/dev/gpu/i915/display/intel_legacy_color.c): the pipe CSC
# coefficient format of Ironlake to Ivy Bridge, the two's complement
# matrices of Valleyview (WGC) and Cherryview (CGM), and the entry formats
# of the 10-bit palettes (gen2/3 slope format, gen4 10.6, Ironlake's
# precision palette).  The colour file's pure functions are cut out with
# sed and compiled with the build host's compiler.
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-intel-legacy-color-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
C=kernel/dev/gpu/i915/display/intel_legacy_color.c
{
	sed -n '/^#define LCOL_PALETTE_RED_SHIFT/,/^#define LCOL_PALETTE_BLUE_SHIFT/p' $C
	sed -n '/^#define LCOL_PREC_10_RED_SHIFT/,/^#define LCOL_PREC_10_BLUE_SHIFT/p' $C
	sed -n '/^struct lcol_state {/,/^};/p' $C
	sed -n '/^#define CTM_COEFF_SIGN/,/^#define CTM_COEFF_ABS/p' $C
	sed -n '/^static uint16_t ilk_csc_coeff_fp/,/^}/p' $C
	sed -n '/^#define ILK_CSC_COEFF_1_0/p' $C
	sed -n '/^static void ilk_csc_convert_ctm/,/^}/p' $C
	sed -n '/^static uint16_t ctm_to_twos_complement/,/^}/p' $C
	sed -n '/^#define CHV_CGM_CSC_COEFF_1_0/p' $C
	sed -n '/^static uint32_t i9xx_lut_10_ldw_1/,/^}/p' $C
	sed -n '/^static uint32_t i9xx_lut_10_ldw(/,/^}/p' $C
	sed -n '/^static uint32_t i9xx_lut_10_udw_1/,/^}/p' $C
	sed -n '/^static uint32_t i9xx_lut_10_udw(/,/^}/p' $C
	sed -n '/^static uint32_t i965_lut_10p6_ldw/,/^}/p' $C
	sed -n '/^static uint32_t i965_lut_10p6_udw/,/^}/p' $C
	sed -n '/^static uint32_t ilk_lut_10(/,/^}/p' $C
} > "$TMP/color_cut.h"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -Iinclude -I"$TMP" -o "$TMP/test" \
	host/test-intel-legacy-color.c
timeout 120 nice -n 19 "$TMP/test"
