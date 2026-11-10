#!/bin/sh
# fetch.sh - download the smartmontools release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the release asset the
# smartmontools project publishes on its GitHub page (RELEASE_7_5, the latest
# release); the project also publishes an MD5 sum and a signature beside it,
# and the recorded SHA-256 was taken from a download that matched that MD5.
# A mismatch against the recorded checksum fails and keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=7.5
file="smartmontools-$version.tar.gz"
url="https://github.com/smartmontools/smartmontools/releases/download/RELEASE_7_5/$file"

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
