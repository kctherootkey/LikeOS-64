#!/bin/sh
# Host test for the DMC firmware package parser
# (kernel/dev/gpu/i915/display/intel_dmc_parse.c), run against synthetic
# packages.  Run from the repository root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-dmc-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" host/test-dmc.c \
	kernel/dev/gpu/i915/display/intel_dmc_parse.c
"$TMP/test"
