#!/bin/sh
# Host test for the HDMI infoframe packer and CEA video-code lookup
# (kernel/dev/gpu/i915/display/intel_infoframe.c, on the pure mode table
# kernel/dev/gpu/drm/drm_modes.c).  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-hdmi-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-hdmi.c \
	kernel/dev/gpu/i915/display/intel_infoframe.c kernel/dev/gpu/drm/drm_modes.c
"$TMP/test"
