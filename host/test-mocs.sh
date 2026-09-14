#!/bin/sh
# Host test for the cache-control tables (kernel/dev/gpu/i915/i915_mocs.c).
# A batch names an entry of these tables for every surface it touches;
# the stack above relies on entries 0, 1 and 2 meaning uncached, "as the
# page table says" and cached everywhere.  A wrong table shows up as
# text and icons missing from an otherwise correct picture, so the
# values are pinned here.  The table code is pure; the driver's own
# functions are compiled on the host with the register access stubbed
# out.  Run from the repository root.
set -e
TMP=${TMPDIR:-/tmp}/likeos-mocs-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
{
	echo '#include <stdint.h>'
	echo '#include <stddef.h>'
	sed -n '/^#define GEN9_RCS_MOCS_BASE/,/^#define GEN9_LNCFCMOCS/p' include/kernel/dev/gpu/i915/i915_reg.h
	sed -n '/^#define MI_INSTR/p; /^#define MI_NOOP/p; /^#define MI_LOAD_REGISTER_IMM/p' include/kernel/dev/gpu/i915/i915_reg.h
	sed -n '/^#define MOCS_LE_CACHEABILITY/,/^}$/p' kernel/dev/gpu/i915/i915_mocs.c
	sed -n '/^static const struct mocs_entry \*mocs_entry/,/^}$/p' kernel/dev/gpu/i915/i915_mocs.c
	sed -n '/^uint32_t i915_mocs_control_value/,/^}$/p' kernel/dev/gpu/i915/i915_mocs.c
	sed -n '/^uint32_t i915_mocs_l3cc_value/,/^}$/p' kernel/dev/gpu/i915/i915_mocs.c
	echo 'struct intel_device_info { int gen; };'
	echo 'struct i915_device { const struct intel_device_info *info; };'
	sed -n '/^uint32_t i915_mocs_l3cc_emit/,/^}$/p' kernel/dev/gpu/i915/i915_mocs.c
	cat host/test-mocs.c
} > "$TMP/test.c"
cc -std=gnu11 -Wall -Wextra -O1 -o "$TMP/test" "$TMP/test.c"
"$TMP/test"
