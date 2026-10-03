#!/bin/sh
# Host test for the variable refresh timing generator of the Intel display
# transcoders (kernel/dev/gpu/i915/display/intel_vrr.c), compiled into the
# test over a recorded register file.  The kernel's locking header is
# replaced by the fence test's prelude.  Runs twice: the defaults, and
# variable refresh off.  Extra compiler options may be given as arguments
# (then only that build runs).  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-intel-vrr-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
run() {
	timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g "$@" \
		-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
		host/test-intel-vrr.c
	timeout 120 nice -n 19 "$TMP/test"
}
if [ $# -gt 0 ]; then
	run "$@"
else
	run
	run -DI915_FEAT_VRR=0
fi
