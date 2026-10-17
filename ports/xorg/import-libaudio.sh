#!/bin/sh
#
# Publish libaudio -- the LikeOS audio client library -- into the X sysroot:
# the headers (<audio/audio.h>, <audio/protocol.h>), libaudio.so and a
# libaudio.pc, so ports such as the GStreamer audio sink can find it with
# pkg-config and link it.
#
# The library is built by the top-level Makefile (user/lib/libaudio) and
# copied to build/; when build/ has no copy yet the one in the source
# directory is used.  Nothing is built here.
#
# Every file is copied only when its bytes changed.  A fresh mtime on an
# unchanged header makes ninja rebuild everything that includes it, and an
# unchanged library would needlessly relink its users.  The .pc is written
# with prefix=/usr like every other .pc in the sysroot: likeos-pkg-config
# sets PKG_CONFIG_SYSROOT_DIR, so /usr is rewritten to the sysroot at query
# time and the recorded paths stay right for the target.
#
# Re-runnable.
#
# Copyright (C) 2026 The LikeOS Project

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
BUILD="$root/build"
SRC="$root/user/lib/libaudio"

inc="$SYSROOT/usr/include/audio"
lib="$SYSROOT/usr/lib"
pc="$lib/pkgconfig"

# The library version, from the public header.
ver_major=$(sed -n 's/^#define LA_VERSION_MAJOR[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
	"$SRC/include/audio/audio.h")
ver_minor=$(sed -n 's/^#define LA_VERSION_MINOR[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
	"$SRC/include/audio/audio.h")
VERSION="${ver_major:-1}.${ver_minor:-0}"

# install_if_changed SRC DST [cp-flags]
install_if_changed() {
	if ! cmp -s "$1" "$2" 2>/dev/null; then
		cp -f $3 "$1" "$2"
		return 0
	fi
	return 1
}

# The .so: build/ first (what the image carries), else the source tree.
so=""
for f in "$BUILD/libaudio.so" "$SRC/libaudio.so"; do
	if [ -f "$f" ]; then
		so="$f"
		break
	fi
done
if [ -z "$so" ]; then
	echo "import-libaudio: libaudio.so not built yet (make userland-libaudio)" >&2
	exit 1
fi

mkdir -p "$inc" "$lib" "$pc"

for h in audio.h protocol.h; do
	install_if_changed "$SRC/include/audio/$h" "$inc/$h" || true
done

# cp -a keeps the build's mtime: a library that really was rebuilt still
# relinks what uses it, an identical one is not touched at all.
install_if_changed "$so" "$lib/libaudio.so" -a || true

# write_pc: generated from libaudio.pc.in into a temporary file, installed
# only when it differs.
write_pc() {
	tmp="$pc/.libaudio.pc.$$"
	sed "s/@VERSION@/$VERSION/g" "$SRC/libaudio.pc.in" >"$tmp"
	if cmp -s "$tmp" "$pc/libaudio.pc" 2>/dev/null; then
		rm -f "$tmp"
	else
		mv -f "$tmp" "$pc/libaudio.pc"
	fi
}
write_pc

echo "  libaudio $VERSION"
echo "libaudio imported into $SYSROOT"
