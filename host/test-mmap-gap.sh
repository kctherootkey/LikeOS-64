#!/bin/sh
# Build and run host/test-mmap-gap.c.  Run from the repository root.
#
# Exercises the mmap address selection from kernel/mm/mmap.c: the top-down
# first-fit search over the region table that decides where a mapping with
# no address of its own goes.  It replaced a cursor that never looked at the
# table and, after an hour of per-frame map/unmap churn, placed mappings on
# top of live shared libraries.  The code under test is cut out of the live
# kernel source at build time, so there is no copy to drift.
set -e

TMP=${TMPDIR:-/tmp}/likeos-mmap-gap-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

awk '/mmap address selection: BEGIN/{f=1} f{print} /mmap address selection: END/{f=0}' \
    kernel/mm/mmap.c > "$TMP/mmap-gap-under-test.h"
grep -q "mmap_gap_search" "$TMP/mmap-gap-under-test.h" || {
	echo "test-mmap-gap: could not cut mmap_gap_search out of kernel/mm/mmap.c" >&2
	exit 1
}

cc -std=gnu11 -Wall -Wextra -O1 -I"$TMP" -o "$TMP/test" host/test-mmap-gap.c
"$TMP/test"
