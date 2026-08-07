#!/bin/sh
# Build and run host/test-string.c.  Run from the repository root.
#
# Compares libc's string.c against the host glibc function by function, with
# every destination placed against a guard page so an out-of-bounds write dies
# where it is made.  See the comment at the top of host/test-string.c.
set -e

OUT=${TMPDIR:-/tmp}/likeos-string-test.$$
trap 'rm -f "$OUT"' EXIT

# string.c is included by the test, so it needs the libc headers.  Unlike the
# unicode test this one keeps the host headers as well: the test itself uses
# mmap, stdio and glibc's own string functions as the reference.
# errno is a plain int here and a TLS symbol in the host libc, so the renamed
# reference needs a definition of its own rather than resolving to the host's.
cc -O1 -g -fno-builtin -Wall -Wextra -Wno-unused-parameter \
   -Derrno=likeos_errno \
   -I user/lib/libc/include \
   -o "$OUT" host/test-string.c

"$OUT"
