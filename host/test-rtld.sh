#!/bin/sh
# Build and run host/test-rtld.c: the dynamic loader's dlopen failure paths.
# Run from the repository root.
#
# A copy of user/lib/rtld/rtld.c is built for the host with the few syscall
# numbers that differ patched in, plus host/test-rtld-hooks.inc, and every
# symbol it defines except the hk_ entry points is made local, so it runs
# beside the host's own loader without replacing anything of it.  The
# libraries it loads are built here with -nostdlib; dependencies are named
# by a path relative to the root (the loader tries "/lib/NAME" and then
# "/NAME"), so they are found in the scratch directory.
#
# Copyright (C) 2026 The LikeOS Project

set -e

OUT=${TMPDIR:-/tmp}/likeos-rtld-test.$$
mkdir -p "$OUT/rtld"
trap 'rm -rf "$OUT"' EXIT
OUT=$(cd "$OUT" && pwd -P)
R=${OUT#/}

# The loader, for the host.
cp user/lib/rtld/rtld.c "$OUT/rtld/rtld.c"
cat host/test-rtld-hooks.inc >> "$OUT/rtld/rtld.c"
sed -e 's/^#define __NR_mprotect .*/#define __NR_mprotect 10/' \
    -e 's/^#define __NR_gettid .*/#define __NR_gettid 186/' \
    -e 's/^#define __NR_futex .*/#define __NR_futex 202/' \
    -e 's/return (int)__syscall2(__NR_arch_prctl, code, (long)addr);/(void)code; (void)addr; return -1;/' \
    user/lib/rtld/rtld_syscall.h > "$OUT/rtld/rtld_syscall.h"
grep -q '__NR_mprotect 10' "$OUT/rtld/rtld_syscall.h"
grep -q 'return -1;' "$OUT/rtld/rtld_syscall.h"

cc -c -O1 -g -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
   -nostdinc -ffreestanding -fPIE \
   "$OUT/rtld/rtld.c" -o "$OUT/rtld.o"
objcopy -w --keep-global-symbol='hk_*' "$OUT/rtld.o" "$OUT/rtld_hk.o"

# The libraries: liba -> libb -> libgone; libok and libtls stand alone.
LIBCC="cc -shared -fPIC -nostdlib -O1 -Wl,-z,now -Wl,--hash-style=gnu"
cat > "$OUT/gone.c" <<'EOF'
int gone_fn(void) { return 7; }
EOF
cat > "$OUT/b.c" <<'EOF'
extern int gone_fn(void);
int b_value(void) { return gone_fn() + 1; }
EOF
cat > "$OUT/a.c" <<'EOF'
extern int b_value(void);
int a_value(void) { return b_value() * 10; }
EOF
cat > "$OUT/ok.c" <<'EOF'
int ok_value(void) { return 42; }
EOF
cat > "$OUT/tls.c" <<'EOF'
__attribute__((tls_model("initial-exec"))) __thread int tv[256];
int tls_get(void) { return tv[3]; }
EOF
$LIBCC -Wl,-soname,"$R/libgone.so" -o "$OUT/libgone.so.keep" "$OUT/gone.c"
cp "$OUT/libgone.so.keep" "$OUT/libgone.so"
$LIBCC -Wl,-soname,"$R/libb.so" -o "$OUT/libb.so" "$OUT/b.c" "$OUT/libgone.so"
$LIBCC -Wl,-soname,liba.so -o "$OUT/liba.so" "$OUT/a.c" "$OUT/libb.so"
$LIBCC -Wl,-soname,libok.so -o "$OUT/libok.so" "$OUT/ok.c"
$LIBCC -Wl,-soname,libtls.so -o "$OUT/libtls.so" "$OUT/tls.c"
rm "$OUT/libgone.so"

# A library cut off inside its writable segment: opened, given a slot and
# partly mapped before the read of its data fails.
cp "$OUT/libok.so" "$OUT/libtrunc.so"
OFF=$(readelf -lW "$OUT/libtrunc.so" |
      awk '$1 == "LOAD" && $7 ~ /W/ { off = $2 } END { print off }')
[ -n "$OFF" ]
truncate -s $(( OFF + 1 )) "$OUT/libtrunc.so"
echo "not an object" > "$OUT/libtext.so"

cc -O1 -g -Wall -Wextra -o "$OUT/test-rtld" host/test-rtld.c "$OUT/rtld_hk.o"

# Small and quick, but boxed all the same: one core, nice, 1 GB of address
# space, two minutes.
(ulimit -v 1048576; exec timeout -k 10 120 nice -n 19 taskset -c 0 \
	"$OUT/test-rtld" "$OUT")
