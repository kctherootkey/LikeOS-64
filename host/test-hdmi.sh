#!/bin/sh
# Host test for the HDMI infoframes of the Intel display
# (kernel/dev/gpu/i915/display/intel_infoframe.c): the driver's own AVI
# packer and CEA video-code lookup (on the pure mode table
# kernel/dev/gpu/drm/drm_modes.c), and the infoframe library's frames in
# the transmitter's buffer form (kernel/dev/gpu/drm/drm_hdmi_infoframe.c,
# with the EDID parser and its corpus filling the sinks' display info).
# The kernel's locking header is replaced by the fence test's prelude.
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-hdmi-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -O1 \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-hdmi.c host/test-edid-corpus.c \
	kernel/dev/gpu/i915/display/intel_infoframe.c \
	$D/drm_hdmi_infoframe.c $D/drm_edid_core.c $D/drm_edid_parse.c \
	$D/drm_displayid.c $D/drm_eld.c $D/drm_edid_tables.c $D/drm_modes.c
timeout 120 nice -n 19 "$TMP/test"
