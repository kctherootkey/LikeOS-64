#!/bin/sh
# Host test for the display-mode library (kernel/dev/gpu/drm/drm_modes.c):
# the timing tables, CVT / GTF / analog television timings, matching,
# validation and sorting, and the client-form functions compared with
# their previous implementation.  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-drm-modes-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-drm-modes.c \
	kernel/dev/gpu/drm/drm_modes.c
"$TMP/test"
