#!/bin/sh
# Host test for the DisplayPort helpers (kernel/dev/gpu/drm/drm_dp_helper.c):
# link status and training delays, link rate codes, DPCD access with its
# retries, the extended receiver caps, I2C over AUX against a scripted fake
# sink (DEFER, NACK, short replies, bare address packets), identification
# quirks, branch-device limits, LTTPR mode setting, DSC caps, SDP packing,
# the eDP backlight and the bandwidth arithmetic.  The helper file is
# compiled into the test with the fence test's prelude standing in for the
# scheduler; the EDID library provides the CEA modes.  Run from the
# repository root; "-v" prints the helpers' messages.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-dp-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
for opts in "-DDRM_DP_DEBUG=0 -DDRM_DP_SLEEPING_DELAYS=1" \
	    "-DDRM_DP_DEBUG=1 -DDRM_DP_SLEEPING_DELAYS=0"; do
	timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g $opts \
		-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
		host/test-drm-dp.c \
		$D/drm_edid_core.c $D/drm_edid_parse.c $D/drm_displayid.c \
		$D/drm_eld.c $D/drm_edid_tables.c $D/drm_modes.c
	timeout 120 nice -n 19 "$TMP/test" "$@"
done
