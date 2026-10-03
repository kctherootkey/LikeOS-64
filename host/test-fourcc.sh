#!/bin/sh
# Host test for the pixel format table (kernel/dev/gpu/drm/drm_fourcc.c).
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-fourcc-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-fourcc.c \
	kernel/dev/gpu/drm/drm_fourcc.c
"$TMP/test"
