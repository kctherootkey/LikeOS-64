#!/bin/sh
# Host test for the range bookkeeping of a graphics address space
# (kernel/dev/gpu/i915/i915_vma.c).  A client recycles the addresses of
# an object it is done with before the driver hears of it; the object
# that lands on them must not lose its translations when the old one is
# finally torn down.  The list logic is pure; the driver's own functions
# are compiled on the host.  Run from the repository root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-vma-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
{
	echo '#include <stdint.h>'
	echo '#include <stddef.h>'
	echo 'struct i915_vm;'
	sed -n '/^struct i915_vma {/,/^};/p' include/kernel/dev/gpu/i915/i915_drv.h
	sed -n '/^void i915_vma_list_attach/,/^}$/p' kernel/dev/gpu/i915/i915_vma.c
	sed -n '/^int i915_vma_list_detach/,/^}$/p' kernel/dev/gpu/i915/i915_vma.c
	sed -n '/^unsigned i915_vma_list_supersede/,/^}$/p' kernel/dev/gpu/i915/i915_vma.c
	cat host/test-vma.c
} > "$TMP/test.c"
cc -std=gnu11 -Wall -Wextra -O1 -o "$TMP/test" "$TMP/test.c"
"$TMP/test"
