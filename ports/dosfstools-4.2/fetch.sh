#!/bin/sh
# fetch.sh - download the dosfstools release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the v4.2 release asset
# of the project's repository on GitHub (the url below); the recorded
# checksum is that of the asset downloaded on 2026-11-11, the same one the
# distributions record for dosfstools 4.2.  A mismatch fails and keeps
# nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=4.2
file="dosfstools-$version.tar.gz"
url="https://github.com/dosfstools/dosfstools/releases/download/v$version/$file"

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
