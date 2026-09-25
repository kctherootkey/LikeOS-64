#!/bin/sh
# Host test for the fence register encoding and the tiled address arithmetic
# of the Intel graphics driver's aperture mappings.  Run from the repository
# root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-i915-fence-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-i915-fence.c
"$TMP/test"
