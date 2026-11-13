#!/bin/sh
# fetch.sh - download the libnvme release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the release tag v1.16.2
# of the project's repository on GitHub (the url below), as
# the archive GitHub generates for the tag; the recorded checksum is that of
# the archive downloaded on 2026-11-11.  1.16.2 is the newest release of the
# stand-alone libnvme (from 3.0 on the library lives inside the nvme-cli
# tree), and the one nvme-cli 2.16 is built against.  A mismatch fails and
# keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=1.16.2
file="libnvme-$version.tar.gz"
url="https://github.com/linux-nvme/libnvme/archive/refs/tags/v$version.tar.gz"

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
