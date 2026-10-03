#!/bin/sh
# Host test for the HDMI library: infoframes (kernel/dev/gpu/drm/
# drm_hdmi_infoframe.c), the HDMI helpers (drm_hdmi_helper.c), SCDC
# (drm_scdc_helper.c) and DP dual-mode adaptors (drm_dp_dual_mode_helper.c),
# with the EDID parser and its corpus filling the connectors' display info
# and a fake I2C bus standing in for the sink and the adaptor.  The
# kernel's locking header is replaced by the fence test's prelude.  Run
# from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-hdmi-lib-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-drm-hdmi-lib.c host/test-edid-corpus.c \
	$D/drm_hdmi_infoframe.c $D/drm_hdmi_helper.c $D/drm_scdc_helper.c \
	$D/drm_dp_dual_mode_helper.c \
	$D/drm_edid_core.c $D/drm_edid_parse.c $D/drm_displayid.c $D/drm_eld.c \
	$D/drm_edid_tables.c $D/drm_modes.c
timeout 120 nice -n 19 "$TMP/test"
