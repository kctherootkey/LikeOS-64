#!/bin/sh
# Host test for the Intel display's DisplayPort AUX channel and link
# training on the DisplayPort helper library (intel_dp_aux.c,
# intel_dp_link_training.c, intel_dp.c, with drm_dp_helper.c): a model of the AUX
# registers and of a sink with link-training repeaters stands behind the
# i915 register interface.  Built for each repeater mode and with and
# without the extended receiver caps.  Run from the repository root; "-v"
# prints the kernel messages.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-i915-dp-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
for opts in "-DI915_FEAT_DP_LTTPR=2" "-DI915_FEAT_DP_LTTPR=1" \
	    "-DI915_FEAT_DP_LTTPR=2 -DI915_FEAT_DP_EXT_CAPS=0"; do
	timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
		-DI915_FEAT_DP_HELPERS=1 $opts \
		-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
		host/test-i915-dp.c \
		$D/drm_edid_core.c $D/drm_edid_parse.c $D/drm_displayid.c \
		$D/drm_eld.c $D/drm_edid_tables.c $D/drm_modes.c $D/drm_edid.c \
		$D/drm_dsc_helper.c kernel/dev/gpu/i915/display/intel_dsc_caps.c \
		kernel/dev/gpu/i915/display/intel_qp_tables.c
	echo "[$opts]"
	timeout 120 nice -n 19 "$TMP/test" "$@"
done
