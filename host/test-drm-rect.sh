#!/bin/sh
# Host test for the display-manager core's rectangle helpers
# (kernel/dev/gpu/drm/drm_rect.c).  The file is pure -- fixed-width types
# and integer arithmetic -- so the build host's compiler runs it against
# the published test vectors; kprintf and panic are stubs in the test.
# Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-rect-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -Iinclude -o "$TMP/test" \
	host/test-drm-rect.c kernel/dev/gpu/drm/drm_rect.c
timeout 120 nice -n 19 "$TMP/test"
