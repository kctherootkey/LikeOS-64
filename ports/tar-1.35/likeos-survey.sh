#!/bin/sh
# likeos-survey.sh - record what GNU tar's configure decides for the LikeOS
# libc.  A maintenance tool, not part of the build.
#
# The port is built without running configure (see Makefile.likeos): what
# configure would decide is kept in this directory as two reviewed files,
#
#   config.h       the C side (the HAVE_..., the replacement switches)
#   config.subst   the Makefile side: every value config.status substitutes
#                  into gnu/, lib/ and src/Makefile.in and that gnulib's
#                  header rules put into its replacement headers
#
# This script produces both afresh, for a new tar release or after the libc
# gained or lost a function.  It runs tar's configure once, cross, against
# the LikeOS libc headers and libc.so (the compiler wrapper below compiles
# and links exactly as Makefile.likeos does), in a scratch directory.  What
# configure cannot test without running a program on LikeOS it takes from
# likeos-survey.cache (each answer is explained there); everything it can
# test by compiling and linking it tests.
#
# The results land in OUTDIR (default ./survey); compare them with the files
# in this directory and take them over by hand -- config.h keeps its LikeOS
# header comment (and two of autoheader's comments are worded for this
# system), config.subst is used as it comes out.
#
# Needs: src/ unpacked (./unpack.sh), the acl and attr ports built (their
# headers and libacl.a are what configure finds), the libc built.
#
# Usage:  ./likeos-survey.sh [OUTDIR]
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-$here/survey}
case "$out" in /*) ;; *) out=$(pwd)/$out ;; esac

libc="$root/user/lib/libc"
rtld="$root/user/lib/rtld"
acl="$root/ports/acl-2.4.0/build"
attr="$root/ports/attr-2.6.0/build"
configsub="$root/ports/xorg/toolchain/config.sub"

[ -d "$here/src" ] || { echo "likeos-survey.sh: run ./unpack.sh first" >&2; exit 1; }
[ -f "$acl/lib/libacl.a" ] || { echo "likeos-survey.sh: build ports/acl-2.4.0 first" >&2; exit 1; }
[ -f "$libc/libc.so" ] || { echo "likeos-survey.sh: build the libc first" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/tar-survey.XXXXXX")
trap 'rm -rf "$work"' EXIT

# A copy of the (patched) sources, with a config.sub that knows the
# x86_64-unknown-likeos triple.
cp -a "$here/src" "$work/src"
cp "$configsub" "$work/src/build-aux/config.sub"

# The compiler: the host gcc with only the LikeOS libc headers, linking a PIE
# against crt1.o, libc.so and ld-likeos.so, as Makefile.likeos does.
mkdir -p "$work/bin"
cat >"$work/bin/likeos-gcc" <<EOF
#!/bin/sh
gccinc=\$(gcc -print-file-name=include)
inc="-nostdinc -isystem $libc/include -isystem \$gccinc -I$acl/include -I$attr/include"
fl="-fno-builtin -ffreestanding -mno-red-zone -fstack-protector-strong -fPIE -D__LIKEOS__ -U__linux__ -U__linux -Ulinux -U__gnu_linux__"
link=1
for a in "\$@"; do case "\$a" in -c|-E|-S|-M|-MM) link=0 ;; esac; done
[ \$link = 0 ] && exec gcc \$inc \$fl "\$@"
exec gcc \$inc \$fl -nostdlib -nostartfiles -pie \\
	-Wl,-T,$root/user/bin/user_dyn.lds -Wl,--hash-style=gnu -Wl,--build-id=none \\
	-Wl,-z,now -Wl,-dynamic-linker,/lib/ld-likeos.so \\
	-L$libc -L$rtld -L$acl/lib $libc/crt1.o "\$@" \\
	-lc -l:ld-likeos.so \$(gcc -print-libgcc-file-name)
EOF
chmod 755 "$work/bin/likeos-gcc"

mkdir -p "$work/build"
cp "$here/likeos-survey.cache" "$work/build/config.cache"
cd "$work/build"
"$work/src/configure" -C \
	--host=x86_64-unknown-likeos --build="$(gcc -dumpmachine)" \
	--prefix=/usr --libexecdir=/usr/libexec --sysconfdir=/etc \
	--localstatedir=/var --disable-nls --without-selinux --disable-rpath \
	--with-rmt=/usr/libexec/rmt \
	CC="$work/bin/likeos-gcc" CFLAGS="-O2" >"$work/configure.log" 2>&1 || {
	tail -30 "$work/configure.log" >&2
	echo "likeos-survey.sh: configure failed" >&2
	exit 1
}

mkdir -p "$out"
cp "$work/configure.log" "$out/configure.log"
cp config.h "$out/config.h"

# Every substituted variable with its value, as config.status itself
# writes it: a template with one "NAME<tab>@NAME@" line per variable is
# run through config.status.  The directory variables differ per Makefile
# and are made by Makefile.likeos; the survey's scratch paths are written as
# the Makefiles' own $(abs_top_srcdir) / $(abs_top_builddir).
sed -n 's/^S\["\([A-Za-z_][A-Za-z_0-9]*\)"\]=.*/\1/p' config.status | sort -u |
	grep -vxE 'srcdir|abs_srcdir|top_srcdir|abs_top_srcdir|builddir|abs_builddir|top_builddir|abs_top_builddir|top_build_prefix|configure_input|INSTALL|MKDIR_P' |
	awk '{ printf "%s\t@%s@\n", $1, $1 }' >subst.in
./config.status --file=subst.out:subst.in >/dev/null
{
	sed -n '1,/^# Copyright/p' "$here/config.subst"
	sed -e "s|$work/bin/likeos-gcc|gcc|g" \
	    -e "s|$work/src|\$(abs_top_srcdir)|g" \
	    -e "s|$work/build|\$(abs_top_builddir)|g" subst.out
} >"$out/config.subst"

echo "likeos-survey.sh: results in $out (config.h, config.subst, configure.log)"
