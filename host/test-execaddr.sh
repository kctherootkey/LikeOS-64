#!/bin/sh
# Host test for the address form the submission interface uses: a client
# hands the kernel a graphics address with bit 47 repeated through the
# top of the word (the form the hardware reads out of a command), and
# the address space itself is a flat 48 bits.  Taking a client's address
# at face value puts every buffer in the upper half of the space far out
# of range, which is what the driver did before.  Run from the
# repository root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-execaddr-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
{
	sed -n '/^#define I915_ADDR_BITS/,/^}/p' \
		kernel/dev/gpu/i915/i915_gem_execbuf.c
	sed -n '/^static uint64_t addr_to_user/,/^}/p' \
		kernel/dev/gpu/i915/i915_gem_execbuf.c
	cat host/test-execaddr.c
} > "$TMP/test.c"
cc -std=gnu11 -Wall -Wextra -O1 -include stdint.h -o "$TMP/test" "$TMP/test.c"
"$TMP/test"
