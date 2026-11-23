#!/bin/sh
# fetch.sh - download the ntfs-3g release tarball (ntfs-3g and ntfsprogs) and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the 2026.9.28 stable
# release tarball the project links from its GitHub release page (the url
# below); the recorded checksum is the SHA-256 that release page publishes,
# and the asset downloaded on 2026-11-20 matches it.  A mismatch fails and
# keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=2026.9.28
file="ntfs-3g_ntfsprogs-$version.tgz"
url="https://tuxera.com/opensource/$file"

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
