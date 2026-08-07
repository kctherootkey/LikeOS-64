#!/bin/sh
# Build and run host/test-printf.c.  Run from the repository root.
#
# stdio.c defines printf, fopen and the rest of the standard names, so it
# cannot simply be linked next to glibc.  Rather than list the names in a -D
# sweep -- which goes stale the moment stdio.c grows a function -- it is
# compiled as it stands and then every symbol it DEFINES is renamed to an lk_
# one with objcopy.  Its undefined references (malloc, memcpy, write, ...) keep
# their names and resolve to the host libc, which is what we want: the
# formatter is the thing under test, not the allocator underneath it.
set -e

OUT=${TMPDIR:-/tmp}/likeos-printf-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

# errno is a plain int here and TLS in the host libc.
echo 'int likeos_errno;' > "$OUT/errno_stub.c"

cc -c -O1 -w -nostdinc -ffreestanding -Derrno=likeos_errno \
   -Iuser/lib/libc/include -Iuser/lib/libc/src \
   user/lib/libc/src/stdio/stdio.c -o "$OUT/stdio.o"

# Build the rename map from what the object actually defines.
nm -g --defined-only "$OUT/stdio.o" \
	| awk '{ print $3, "lk_" $3 }' > "$OUT/renames"
objcopy --redefine-syms="$OUT/renames" "$OUT/stdio.o" "$OUT/stdio_lk.o"

cc -O1 -g -Wall -Wextra -Wno-unused-parameter \
   -o "$OUT/test-printf" host/test-printf.c "$OUT/stdio_lk.o" \
   "$OUT/errno_stub.c"

"$OUT/test-printf"
