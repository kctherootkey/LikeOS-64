#!/bin/sh
# Build and run host/test-math.c.  Run from the repository root.
#
# Compares libc's hyperbolic functions against the host glibc across their
# whole domains.  See the comment at the top of host/test-math.c.
set -e

TMP=${TMPDIR:-/tmp}/likeos-math-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# math.c defines sqrt, log, fabs and fifty more under the names glibc uses, so
# it is compiled on its own and every symbol it defines or calls is given an
# lk_ prefix in one step.  It calls nothing outside itself, so the renaming
# stays self-consistent and the object still links.
#
# -fno-builtin matters: without it the compiler recognises the shape of, say,
# the sqrt implementation and replaces the body with a call to sqrt -- which
# after the rename is lk_sqrt calling itself.
#
# -fno-stack-protector for the same reason: the host builds with the protector
# on by default, and the renaming turns its __stack_chk_fail into an
# lk___stack_chk_fail that nothing defines.
cc -O2 -g -fno-builtin -fno-stack-protector -Wall -Wextra \
   -I user/lib/libc/include \
   -c user/lib/libc/src/math/math.c -o "$TMP/math.o"

# fenv.c goes through the same treatment, and it is here for a reason: its
# constants are hardware register layouts, and getting one wrong is not a wrong
# answer but an instruction fault.  MXCSR's exception-mask field was shifted by
# 12 instead of 7, which set two RESERVED bits, and LDMXCSR raises #GP on those
# -- so feholdexcept() killed the process.  Nothing on the build host would
# have noticed either, until this file ran it.
cc -O2 -g -fno-builtin -fno-stack-protector -Wall -Wextra \
   -I user/lib/libc/include \
   -c user/lib/libc/src/math/fenv.c -o "$TMP/fenv.o"

objcopy --prefix-symbols=lk_ "$TMP/math.o" "$TMP/math-renamed.o"
objcopy --prefix-symbols=lk_ "$TMP/fenv.o" "$TMP/fenv-renamed.o"

# The test itself uses the HOST headers: glibc's asinh is the reference.
cc -O1 -g -Wall -Wextra -o "$TMP/test-math" \
   host/test-math.c "$TMP/math-renamed.o" "$TMP/fenv-renamed.o" -lm

"$TMP/test-math"
