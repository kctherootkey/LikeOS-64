#!/bin/sh
# fetch.sh - download the exfatprogs release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the 1.4.5 release asset
# of the project's repository on GitHub (the url below); the recorded
# checksum is that of the asset downloaded on 2026-11-20, the same one the
# release publishes beside it (exfatprogs-1.4.5.tar.xz.sha256).  A mismatch
# fails and keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=1.4.5
file="exfatprogs-$version.tar.xz"
url="https://github.com/exfatprogs/exfatprogs/releases/download/$version/$file"

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
