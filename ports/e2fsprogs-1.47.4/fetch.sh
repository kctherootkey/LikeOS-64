#!/bin/sh
# fetch.sh - download the e2fsprogs release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the project's release
# area on kernel.org; the recorded checksum is the one its signed
# sha256sums.asc lists for this file (checked 2026-11-11).  A mismatch fails
# and keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=1.47.4
file="e2fsprogs-$version.tar.xz"
url="https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$version/$file"

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
