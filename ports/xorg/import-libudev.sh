#!/bin/sh
#
# Publish libudev -- this system's device-information library behind the
# libudev API -- into the X sysroot: <libudev.h>, libudev.so.1 with the
# libudev.so link the linker looks for, and a libudev.pc, so libinput (and
# anything else built against the sysroot) finds it with pkg-config.
#
# The library is built by the top-level Makefile (user/lib/libudev) and
# copied to build/; when build/ has no copy yet the one in the source
# directory is used.  Nothing is built here.
#
# Every file is copied only when its bytes changed, for the reason given in
# import-libaudio.sh: a fresh mtime on an unchanged header or library makes
# everything above it rebuild or relink for nothing.  The .pc is written with
# prefix=/usr like every other .pc in the sysroot.
#
# The image gets the library from the top-level Makefile (ROOT_LIBS); the
# copy here is identical, so stage.sh copying the sysroot's libraries as
# well changes nothing.
#
# Re-runnable.
#
# Copyright (C) 2026 The LikeOS Project

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
BUILD="$root/build"
SRC="$root/user/lib/libudev"

inc="$SYSROOT/usr/include"
lib="$SYSROOT/usr/lib"
pc="$lib/pkgconfig"

# This library's own version (see libudev.pc.in for why it is low).
VERSION="1.0"

install_if_changed() {
	if ! cmp -s "$1" "$2" 2>/dev/null; then
		cp -f $3 "$1" "$2"
		return 0
	fi
	return 1
}

so=""
for f in "$BUILD/libudev.so.1" "$SRC/libudev.so.1"; do
	if [ -f "$f" ]; then
		so="$f"
		break
	fi
done
if [ -z "$so" ]; then
	echo "import-libudev: libudev.so.1 not built yet (make CRASH_VERBOSE=1 userland-libudev)" >&2
	exit 1
fi

mkdir -p "$inc" "$lib" "$pc"

install_if_changed "$SRC/include/libudev.h" "$inc/libudev.h" || true
# cp -a keeps the build's mtime: a library that really was rebuilt still
# relinks what uses it, an identical one is not touched at all.
install_if_changed "$so" "$lib/libudev.so.1" -a || true
if [ "$(readlink "$lib/libudev.so" 2>/dev/null)" != "libudev.so.1" ]; then
	ln -sfn libudev.so.1 "$lib/libudev.so"
fi

tmp="$pc/.libudev.pc.$$"
sed "s/@VERSION@/$VERSION/g" "$SRC/libudev.pc.in" >"$tmp"
if cmp -s "$tmp" "$pc/libudev.pc" 2>/dev/null; then
	rm -f "$tmp"
else
	mv -f "$tmp" "$pc/libudev.pc"
fi

echo "  libudev $VERSION"
echo "libudev imported into $SYSROOT"
