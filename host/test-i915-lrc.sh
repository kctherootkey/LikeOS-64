#!/bin/sh
# Host test for the Intel driver's logical ring context layout
# (kernel/dev/gpu/i915/i915_lrc.c), which is pure.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-i915-lrc-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-i915-lrc.c \
	kernel/dev/gpu/i915/i915_lrc.c kernel/dev/gpu/i915/i915_renderstate_gen9.c
"$TMP/test"
