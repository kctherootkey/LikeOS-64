#!/bin/sh
# Build and run host/test-malloc.c.  Run from the repository root.
#
# The allocator defines malloc, free and the rest, so it cannot be linked next
# to glibc under those names.  It is compiled as it ships and then every symbol
# it DEFINES is renamed to an lk_ one with objcopy -- the same arrangement as
# host/test-printf.sh, and for the same reason.
#
# Usage: host/test-malloc.sh [threads] [iterations]
#
# THIS RUNS ON THE DEVELOPER'S OWN DESKTOP, so the run is boxed in below:
# a share of the cores, a hard address-space ceiling, and a wall-clock limit.
# An earlier version had none of that and defaulted to one worker per core,
# each writing every byte it allocated; it starved the desktop of CPU and
# pushed the machine into swap.  The knobs are LK_* environment variables --
# raise them deliberately for a long hunt, never by default.
set -e

OUT=${TMPDIR:-/tmp}/likeos-malloc-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

echo 'int likeos_errno;' > "$OUT/errno_stub.c"

# HOST headers only -- deliberately NOT -I user/lib/libc/include.
#
# The test and the allocator are one translation unit, so a header choice is a
# choice for both, and the test calls the host's pthread and stdio at runtime.
# With libc's headers ahead of the host's, pthread_mutex_t came out 32 bytes
# where the glibc that actually locks it writes 40, and every mutex in the test
# quietly overran the eight bytes after it -- which showed up as intermittent
# corruption that looked exactly like the allocator bug being hunted.
#
# What is under test is the allocator's CODE.  It needs nothing from libc's
# headers that the host's do not also declare.
#
# -Werror on implicit declarations: the lk_ names are only created by the
# objcopy pass below, so a missing prototype here silently truncates every
# returned pointer to 32 bits rather than failing to link.
cc -c -O1 -g -w -Werror=implicit-function-declaration -Derrno=likeos_errno \
   host/test-malloc.c -o "$OUT/test.o"

nm -g --defined-only "$OUT/test.o" \
	| awk '$2 == "T" || $2 == "W" { print $3 }' \
	| grep -Ev '^(main|__|_)' \
	| awk '{ print $1, "lk_" $1 }' > "$OUT/renames"
objcopy --redefine-syms="$OUT/renames" "$OUT/test.o" "$OUT/test_lk.o"

# -rdynamic so the fault handler's backtrace carries function names.
cc -O1 -g -rdynamic -o "$OUT/test-malloc" "$OUT/test_lk.o" "$OUT/errno_stub.c" -lpthread

# ---- resource box --------------------------------------------------------
#
# Cores: leave the machine most of them.  A quarter of the box, at least one.
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
THREADS=${1:-$(( NCPU / 4 > 0 ? NCPU / 4 : 1 ))}
ITERS=${2:-8000}

# Address space.  The allocator over-maps 2x a 64MB heap per arena and the
# test's stand-in brk is 256MB, so the ceiling has to clear those -- but once
# it is reached the allocator returns NULL and the test's `if (!p) continue`
# carries on, rather than the kernel reaching for the OOM killer.
ulimit -v "${LK_MALLOC_VLIMIT_KB:-2097152}" 2>/dev/null || true

# Wall clock.  A deadlock inside the allocator is one of the things being
# hunted, and without this it wedges the terminal instead of reporting.
LIMIT=${LK_MALLOC_TIMEOUT:-90}

# Pin to the TOP cores and run at the lowest priority, so the desktop keeps
# both its cores and its scheduling latency no matter what the test does.
RUN="nice -n 19"
if command -v taskset >/dev/null 2>&1 && [ "$THREADS" -lt "$NCPU" ]; then
	RUN="taskset -c $(( NCPU - THREADS ))-$(( NCPU - 1 )) $RUN"
fi
if command -v timeout >/dev/null 2>&1; then
	RUN="timeout -k 5 $LIMIT $RUN"
fi

echo "box: $THREADS/$NCPU cores, $(( ${LK_MALLOC_VLIMIT_KB:-2097152} / 1024 ))MB address space, ${LIMIT}s"

set +e
$RUN "$OUT/test-malloc" "$THREADS" "$ITERS"
rc=$?
set -e

if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
	echo "TIMEOUT after ${LIMIT}s -- the allocator did not finish." >&2
	echo "That is a result, not a flake: suspect a deadlock." >&2
fi
exit "$rc"
