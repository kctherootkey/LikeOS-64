#!/bin/sh
#
# Install the base LikeOS libraries -- zlib, OpenSSL, libcurl -- into the X
# sysroot.
#
# They are ports in their own right, built by the top-level Makefile into
# build/ and staged onto the image, but nothing ever placed their headers,
# libraries or pkg-config metadata where the X toolchain looks.  X clients
# may need all three: libcurl to fetch, OpenSSL for https, zlib for compressed
# transfers and for libpng.
#
# The .pc files in the source trees cannot be reused -- OpenSSL's say so in
# their first line ("should never be installed") because their prefix is the
# build directory on THIS machine.  Fresh ones are written here with
# prefix=/usr, which is what every other .pc in the sysroot uses and what
# likeos-pkg-config expects: it sets PKG_CONFIG_SYSROOT_DIR, so /usr/lib is
# rewritten to the sysroot at query time and the recorded path stays correct
# for the target.
#
# Re-runnable: everything is a copy or an overwrite.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
BUILD="$root/build"

inc="$SYSROOT/usr/include"
lib="$SYSROOT/usr/lib"
pc="$lib/pkgconfig"
mkdir -p "$inc" "$lib" "$pc"

zlib_dir=$(ls -d "$root"/ports/lib/zlib-* 2>/dev/null | head -1)
ssl_dir=$(ls -d "$root"/ports/openssl-* 2>/dev/null | head -1)
curl_dir=$(ls -d "$root"/ports/curl-* 2>/dev/null | head -1)

ver_of() { basename "$1" | sed 's/^[a-z]*-//'; }

write_pc() {
	# $1 name  $2 version  $3 description  $4 libs  $5 requires  $6 cflags
	cat >"$pc/$1.pc" <<PC
prefix=/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: $1
Description: $3
Version: $2
Requires: $5
Libs: -L\${libdir} $4
Cflags: -I\${includedir} $6
PC
}

# Headers go in only when their bytes changed.  A plain cp gives the copy a
# fresh mtime, and ninja in the WebKitGTK tree reads that as "input changed":
# one re-run of this script (every `make ports-xorg`) marked 527 of its
# objects stale -- zconf.h alone is reached by most of WebCore -- for a
# rebuild of hours over identical bytes.  The libraries keep cp -a, which
# preserves the build's own mtime, so a library that really was rebuilt still
# relinks what uses it.
copy_hdrs() { # copy_hdrs DIR FILE...
	dst=$1
	shift
	for f in "$@"; do
		[ -f "$f" ] || continue
		cmp -s "$f" "$dst/$(basename "$f")" 2>/dev/null ||
			cp -f "$f" "$dst/"
	done
}
copy_libs() {
	for f in "$BUILD"/$1; do
		[ -e "$f" ] || continue
		cp -a "$f" "$lib/"
	done
}

# ---- zlib ----------------------------------------------------------------
if [ -n "$zlib_dir" ]; then
	copy_hdrs "$inc" "$zlib_dir/zlib.h" "$zlib_dir/zconf.h"
	copy_libs 'libz.so*'
	write_pc zlib "$(ver_of "$zlib_dir")" "zlib compression library" "-lz" "" ""
	echo "  zlib     $(ver_of "$zlib_dir")"
fi

# ---- OpenSSL -------------------------------------------------------------
if [ -n "$ssl_dir" ]; then
	mkdir -p "$inc/openssl"
	copy_hdrs "$inc/openssl" "$ssl_dir"/include/openssl/*.h
	copy_libs 'libssl.so*'
	copy_libs 'libcrypto.so*'
	v=$(ver_of "$ssl_dir")
	write_pc libcrypto "$v" "OpenSSL cryptography library" "-lcrypto" "" ""
	write_pc libssl    "$v" "OpenSSL TLS library"          "-lssl"    "libcrypto" ""
	write_pc openssl   "$v" "OpenSSL"                      ""         "libssl libcrypto" ""
	echo "  openssl  $v"
fi

# ---- libcurl -------------------------------------------------------------
if [ -n "$curl_dir" ]; then
	mkdir -p "$inc/curl"
	copy_hdrs "$inc/curl" "$curl_dir"/include/curl/*.h
	copy_libs 'libcurl.so*'
	copy_libs 'libnghttp2.so*'
	write_pc libcurl "$(ver_of "$curl_dir")" "Library to transfer files with HTTP, FTP, etc." \
		"-lcurl" "libssl libcrypto zlib" ""
	echo "  libcurl  $(ver_of "$curl_dir")"
fi

echo "base libraries imported into $SYSROOT"
