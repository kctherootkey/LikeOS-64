#!/bin/sh
#
# Publish libpam -- LikeOS's own PAM (user/lib/libpam) -- into the X sysroot:
# the headers (<security/pam_appl.h>) and libpam.so, so the ports that
# authenticate users -- LightDM -- find it the conventional way, with
# #include <security/pam_appl.h> and -lpam.
#
# The library is built by the top-level Makefile (user/lib/libpam) and copied
# to build/; when build/ has no copy yet the one in the source directory is
# used.  Nothing is built here.  On the image the library is /lib/libpam.so
# (ROOT_LIBS), staged with the base system, not from the sysroot.
#
# Every file is copied only when its bytes changed, as import-libaudio.sh
# does: a fresh mtime on an unchanged header rebuilds whatever includes it.
#
# Re-runnable.
#
# Copyright (C) 2026 The LikeOS Project

set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
BUILD="$root/build"
SRC="$root/user/lib/libpam"

inc="$SYSROOT/usr/include/security"
lib="$SYSROOT/usr/lib"

# install_if_changed SRC DST [cp-flags]
install_if_changed() {
	if ! cmp -s "$1" "$2" 2>/dev/null; then
		cp -f $3 "$1" "$2"
		return 0
	fi
	return 1
}

so=""
for f in "$BUILD/libpam.so" "$SRC/libpam.so"; do
	if [ -f "$f" ]; then
		so="$f"
		break
	fi
done
if [ -z "$so" ]; then
	echo "import-libpam: libpam.so not built yet (make userland-libpam)" >&2
	exit 1
fi

mkdir -p "$inc" "$lib"

for h in "$SRC"/include/security/*.h; do
	install_if_changed "$h" "$inc/$(basename "$h")" || true
done

# cp -a keeps the build's mtime: a library that really was rebuilt still
# relinks what uses it, an identical one is not touched at all.
install_if_changed "$so" "$lib/libpam.so" -a || true

echo "libpam imported into $SYSROOT"
