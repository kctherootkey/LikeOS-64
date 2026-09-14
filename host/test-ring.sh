#!/bin/sh
# Host test for the command ring's reservation arithmetic
# (kernel/dev/gpu/i915/i915_request.c).  Everything between the head and
# the tail is work the engine has not finished; writing there overwrites
# commands before they run, which is invisible until half a picture is
# missing.  The arithmetic is pure, so the build host exercises it
# against a ring that fills, wraps and drains.  Run from the repository
# root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-ring-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -o "$TMP/test" host/test-ring.c
"$TMP/test"
