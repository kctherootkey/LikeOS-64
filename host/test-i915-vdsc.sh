#!/bin/sh
# Host test for the Intel display's stream compression engine and its
# DisplayPort/eDP configuration (intel_vdsc.c, intel_dp_dsc.c): register
# packing against the PPS packet, the engines' and the packet's
# registers, FEC, power wells, the compression choice and the sink's
# switches, on a model register file and DPCD.  Run from the repository
# root; "-v" prints the kernel messages.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-i915-vdsc-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-DI915_FEAT_DP_HELPERS=1 -DI915_FEAT_DSC=1 \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-i915-vdsc.c \
	$D/drm_edid_core.c $D/drm_edid_parse.c $D/drm_displayid.c \
	$D/drm_eld.c $D/drm_edid_tables.c $D/drm_modes.c $D/drm_edid.c \
	$D/drm_dsc_helper.c kernel/dev/gpu/i915/display/intel_dsc_caps.c \
	kernel/dev/gpu/i915/display/intel_qp_tables.c
timeout 120 nice -n 19 "$TMP/test" "$@"
