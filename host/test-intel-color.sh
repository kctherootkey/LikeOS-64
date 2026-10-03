#!/bin/sh
# Host test for the colour management of the Intel display pipes
# (kernel/dev/gpu/i915/display/intel_color.c), compiled into the test over
# a recorded register file.  The kernel's locking header is replaced by
# the fence test's prelude.  Runs three times: the defaults, the
# multi-segmented gamma off, colour management off.  Extra compiler
# options may be given as arguments (then only that build runs).  Run
# from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-intel-color-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
run() {
	timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g "$@" \
		-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
		host/test-intel-color.c
	timeout 120 nice -n 19 "$TMP/test"
}
if [ $# -gt 0 ]; then
	run "$@"
else
	run
	run -DI915_FEAT_COLOR_ICL_MULTISEG=0
	run -DI915_FEAT_COLOR_MGMT=0
fi
