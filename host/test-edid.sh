#!/bin/sh
# Host test for the display-manager core's EDID parser and mode table
# (kernel/dev/gpu/drm/drm_edid_core.c, drm_edid_parse.c, drm_displayid.c,
# drm_eld.c, drm_edid_tables.c on drm_modes.c).  They are pure --
# fixed-width types only -- so the build host's compiler runs them against
# built and recorded EDID blocks, and against the previous parser
# (host/test-edid-legacy.c).  Run from the repository root; "-v" lists the
# modes the current parser adds over the previous one.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-edid-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-edid.c \
	host/test-edid-corpus.c host/test-edid-legacy.c \
	$D/drm_edid_core.c $D/drm_edid_parse.c $D/drm_displayid.c $D/drm_eld.c \
	$D/drm_edid_tables.c $D/drm_modes.c
(ulimit -v 2000000; timeout 120 nice -n 19 "$TMP/test" "$@")
