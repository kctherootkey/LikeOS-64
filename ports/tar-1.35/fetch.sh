#!/bin/sh
# fetch.sh - download the GNU tar release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the GNU release area,
# where 1.35 is the latest release (2026-11-13); the recorded checksum is
# that of the tarball whose detached signature (tar-1.35.tar.xz.sig, checked
# against the GNU keyring) verified on 2026-11-13.  A mismatch fails and
# keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=1.35
file="tar-$version.tar.xz"
url="https://ftp.gnu.org/gnu/tar/$file"

if [ ! -f "$file" ]; then
	echo "  get  $url"
	if ! curl -fsSL --connect-timeout 20 -o "$file.part" "$url"; then
		rm -f "$file.part"
		echo "fetch.sh: download failed: $url" >&2
		exit 1
	fi
	mv "$file.part" "$file"
fi

if ! sha256sum -c --quiet checksums.sha256; then
	echo "fetch.sh: $file does not match checksums.sha256" >&2
	mv "$file" "$file.bad"
	exit 1
fi
echo "fetch.sh: $file ok"
