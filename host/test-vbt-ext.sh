#!/bin/sh
# Build and run the VBT parser's feature-field tests on the host.
# Copyright (C) 2026 The LikeOS Project
set -e
TMP=${TMPDIR:-/tmp}/likeos-vbt-ext-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-vbt-ext.c \
	kernel/dev/gpu/i915/display/intel_vbt_parse.c kernel/dev/gpu/drm/drm_modes.c
"$TMP/test"
