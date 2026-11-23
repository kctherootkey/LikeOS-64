#!/bin/sh
# Build and run host/test-env.c.  Run from the repository root.
#
# env.c is compiled against the LikeOS headers, with AddressSanitizer and
# UndefinedBehaviorSanitizer, and every symbol it defines is renamed to an
# lk_ one with objcopy, so it runs next to the host C library without
# replacing the host's own getenv/environ.  Its undefined references (malloc,
# strcmp, sched_yield, __errno_location) resolve to the host.
#
#   host/test-env.sh [STEPS]     random steps per seed (default 60000)
#
# The run is bounded: timeout, and a 1 GB memory cap when systemd-run works.
#
# Copyright (C) 2026 The LikeOS Project

set -e

OUT=${TMPDIR:-/tmp}/likeos-env-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

cc -c -O1 -g -Wall -Wextra -Werror -nostdinc -ffreestanding $SAN \
   -Iuser/lib/libc/include \
   user/lib/libc/src/stdlib/env.c -o "$OUT/env.o"

nm -g --defined-only "$OUT/env.o" | awk '$3 !~ /^__(asan|ubsan|odr)/ { print $3, "lk_" $3 }' \
	> "$OUT/renames"
objcopy --redefine-syms="$OUT/renames" "$OUT/env.o" "$OUT/env_lk.o"

cc -O1 -g -Wall -Wextra -Wno-unused-parameter $SAN -pthread \
   -o "$OUT/test-env" host/test-env.c "$OUT/env_lk.o"

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:exitcode=4}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1:exitcode=5}

if command -v systemd-run >/dev/null 2>&1 &&
   systemd-run --user --scope -q -p MemoryMax=1M true >/dev/null 2>&1; then
	timeout -k 10 900 systemd-run --user --scope -q -p MemoryMax=1G \
		-p MemorySwapMax=0 -- nice -n 10 "$OUT/test-env" "$@"
else
	timeout -k 10 900 nice -n 10 "$OUT/test-env" "$@"
fi
