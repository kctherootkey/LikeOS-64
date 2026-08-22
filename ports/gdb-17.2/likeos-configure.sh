#!/bin/sh
# likeos-configure.sh — configure gdb 17.2 for the LikeOS-64 target.
#
# Drives the stock autoconf configure through the LikeOS cross wrappers.  gdb is
# C++, so it needs likeos-c++ from the xorg toolchain -- the only C++-capable
# wrapper in the tree -- which reads its target headers and libraries out of
# build/xorg-sysroot.  That makes gdb the first port outside ports/xorg to
# depend on that sysroot being built; the Makefile rule that invokes this says
# so in its prerequisites.
#
# Built out of tree, in build/, which is how gdb expects to be built and what
# keeps src/ clean enough to re-unpack and re-patch.
#
# Idempotent: re-running reconfigures from scratch.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

root=$(cd ../.. && pwd)
src="$here/src"
bld="$here/build"

toolchain="$root/ports/xorg/toolchain"
sysroot="$root/build/xorg-sysroot"

# --- prerequisites ----------------------------------------------------------
# Checked explicitly, because each of these otherwise fails deep inside
# configure with a message that does not name the real cause.
[ -d "$src" ] || {
	echo "likeos-configure.sh: src/ missing -- run ./unpack.sh first" >&2
	exit 1
}
[ -x "$toolchain/likeos-c++" ] || {
	echo "likeos-configure.sh: $toolchain/likeos-c++ missing" >&2
	exit 1
}
[ -d "$sysroot/usr/include" ] || {
	echo "likeos-configure.sh: $sysroot is not built -- run 'make ports-gtk3'" >&2
	echo "  (gdb links against the libstdc++/gmp/mpfr/expat/zlib built there)" >&2
	exit 1
}
[ -f "$sysroot/usr/include/mpfr.h" ] || {
	echo "likeos-configure.sh: MPFR is missing from the sysroot." >&2
	echo "  gdb 17 requires it outright -- there is a --with-mpfr but no" >&2
	echo "  --without-mpfr -- to do target floating-point arithmetic at the" >&2
	echo "  debuggee's precision rather than the host's." >&2
	echo "  Build it with: ports/xorg/gtk3/fetch.sh mpfr && ... build.sh mpfr" >&2
	exit 1
}
grep -q likeos "$src/config.sub" || {
	echo "likeos-configure.sh: src/ is unpatched (config.sub has no likeos" >&2
	echo "  triple).  Run ./unpack.sh --force; see patches/README." >&2
	exit 1
}

# The system's curses, which gdb links for terminal capabilities even with the
# TUI disabled.
#
# It is reached through a libncurses.so symlink in the sysroot pointing at
# build/ncurses.so, and that file does not survive `make clean'.  A dangling
# symlink is the dangerous case: configure does not stop, it quietly decides the
# system has no curses and links the BUILD HOST's libtinfo instead, which fails
# much later with a page of undefined glibc references and nothing connecting
# them to a deleted file.  So the link is refreshed here and its target checked.
ncurses_so="$root/build/ncurses.so"
if [ ! -f "$ncurses_so" ]; then
	echo "likeos-configure.sh: $ncurses_so is missing." >&2
	echo "  gdb needs the system's curses; build it with 'make ports-ncurses'." >&2
	echo "  (Left missing, configure would settle for the host's libtinfo and" >&2
	echo "  the link would fail with undefined glibc symbols.)" >&2
	exit 1
fi
ln -sfn "$ncurses_so" "$sysroot/usr/lib/libncurses.so"

# --- target-prefixed cross tools --------------------------------------------
#
# gdb's recursive makes look for $host-ar, $host-ranlib and friends by name, in
# several places, and fail with "command not found" rather than falling back to
# the plain names.  LikeOS objects are ordinary x86-64 ELF, so the host's
# binutils are exactly the right tools -- they only need to answer to the
# target-prefixed spelling.  Kept in the port rather than installed anywhere.
toolbin="$here/toolbin"
mkdir -p "$toolbin"
for t in ar ranlib nm strip objcopy objdump readelf as ld size strings addr2line; do
	p=$(command -v "$t" 2>/dev/null) || continue
	ln -sfn "$p" "$toolbin/x86_64-unknown-likeos-$t"
done
ln -sfn "$toolchain/likeos-cc" "$toolbin/x86_64-unknown-likeos-gcc"
ln -sfn "$toolchain/likeos-c++" "$toolbin/x86_64-unknown-likeos-g++"
ln -sfn "$toolchain/likeos-c++" "$toolbin/x86_64-unknown-likeos-c++"

# pkg-config under the target-prefixed name as well, and pointing at the
# sysroot-confined wrapper rather than the host's.
#
# autoconf's PKG_PROG_PKG_CONFIG looks for $host-pkg-config before plain
# pkg-config, and every sub-configure gdb runs at MAKE time -- bfd's among them
# -- searches this PATH.  Naming it here is what makes the confinement hold for
# those too, rather than only for the top-level run below where PKG_CONFIG is
# set in the environment.
ln -sfn "$toolchain/likeos-pkg-config" "$toolbin/x86_64-unknown-likeos-pkg-config"

PATH="$toolbin:$PATH"
export PATH

rm -rf "$bld"
mkdir -p "$bld"
cd "$bld"

# --without-python/--without-guile: neither language is ported, and gdb's
#   scripting is the only thing that wants them.  This is a configuration
#   upstream supports.
# --disable-tui: the TUI needs a real curses with a terminfo database; the
#   system's curses is an ANSI-escape shim.  The command-line debugger is
#   complete without it.
# --disable-sim/--disable-libctf: a CPU simulator and the CTF debug format,
#   neither of which anything here produces or needs.
#
# --without-zstd/--without-debuginfod: both default to `auto', and both are
#   probed with PKG_CHECK_MODULES -- pkg-config alone, with no test that
#   compiles anything.  That is the one probe shape a cross build cannot
#   survive by accident: every other optional dependency here (lzma, xxhash,
#   babeltrace ...) is probed by compiling a program that includes its header,
#   which fails under likeos-cc's -nostdinc and correctly comes out `no'.  The
#   pkg-config ones just see the BUILD host's libraries and say yes.  On a
#   machine with libzstd-dev installed that ends as
#
#     src/bfd/compress.c:24:10: fatal error: zstd.h: No such file or directory
#
#   two thousand files into the build, and on a machine without it the same
#   tree builds -- which is how this came in as "works in the VM, fails on the
#   laptop".  PKG_CONFIG above confines the probes to the sysroot and is the
#   general guard; these two say outright that the features are not built,
#   because neither library is ported.
CC="$toolchain/likeos-cc" \
CXX="$toolchain/likeos-c++" \
PKG_CONFIG="$toolchain/likeos-pkg-config" \
LIKEOS_SYSROOT="$sysroot" \
	"$src/configure" \
	--host=x86_64-unknown-likeos --build="$(gcc -dumpmachine)" \
	--prefix=/usr \
	--without-python --without-guile \
	--disable-sim --disable-libctf \
	--disable-tui --disable-nls --disable-werror \
	--with-expat --with-system-zlib \
	--without-zstd --without-debuginfod \
	--with-gmp="$sysroot/usr" --with-mpfr="$sysroot/usr"

echo "likeos-configure.sh: gdb configured for LikeOS-64 in build/"
