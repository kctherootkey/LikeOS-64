#!/bin/sh
# Host test for the Ice Lake+ combo PLL solver
# (kernel/dev/gpu/i915/display/intel_dpll_calc.c).  Run from the
# repository root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-dpll-icl-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cc -std=gnu11 -Wall -Wextra -O1 -Iinclude -o "$TMP/test" \
	kernel/dev/gpu/i915/display/intel_dpll_calc.c host/test-dpll-icl.c
"$TMP/test"
