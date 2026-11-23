#!/bin/sh
# Build and run host/test-spawn.c: posix_spawn / posix_spawnp and the
# environment they hand the child.  Run from the repository root.
#
# spawn.c is compiled against the LikeOS headers; every symbol it defines is
# renamed to an lk_ one with objcopy, so it runs beside the host C library,
# and its signal calls go to the test's hk_ stand-ins (the LikeOS signal
# types are not the host's).  The rest of what it calls resolves to the host.
#
# Copyright (C) 2026 The LikeOS Project

set -e

OUT=${TMPDIR:-/tmp}/likeos-spawn-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

cc -c -O1 -g -Wall -Wextra -Werror -nostdinc -ffreestanding -fno-builtin \
   -fno-stack-protector -Iuser/lib/libc/include \
   user/lib/libc/src/stdlib/spawn.c -o "$OUT/spawn.o"

nm -g --defined-only "$OUT/spawn.o" | awk '{ print $3, "lk_" $3 }' \
	> "$OUT/renames"
for s in sigaction sigprocmask sigfillset sigismember; do
	echo "$s hk_$s" >> "$OUT/renames"
done
objcopy --redefine-syms="$OUT/renames" "$OUT/spawn.o" "$OUT/spawn_lk.o"

cc -O1 -g -Wall -Wextra -o "$OUT/test-spawn" host/test-spawn.c \
   "$OUT/spawn_lk.o"

# Boxed: one core, nice, 1 GB of address space, one minute.
(ulimit -v 1048576; exec timeout -k 10 60 nice -n 19 taskset -c 0 \
	"$OUT/test-spawn")
