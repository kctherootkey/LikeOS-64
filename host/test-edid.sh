#!/bin/sh
# Host test for the display-manager core's EDID parser and mode table
# (kernel/dev/gpu/drm/drm_edid_parse.c, drm_modes.c).  Both are pure --
# fixed-width types only -- so the build host's compiler runs them against
# recorded EDID blocks and published timings.  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-edid-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-edid.c \
	kernel/dev/gpu/drm/drm_edid_parse.c kernel/dev/gpu/drm/drm_modes.c
"$TMP/test"
