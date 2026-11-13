#!/bin/sh
# fetch.sh - download the nvme-cli release tarball and check it against
# checksums.sha256.
#
# The tarball is kept in this directory (it is what unpack.sh unpacks), so
# this is only needed to recreate it.  The source is the release tag v2.16
# of the project's repository on GitHub (the url below), as
# the archive GitHub generates for the tag; the recorded checksum is that of
# the archive downloaded on 2026-11-11.  2.16 is the newest release built on
# the stand-alone libnvme (ports/libnvme-1.16.2); the 3.x series carries its
# own copy of the library.  A mismatch fails and keeps nothing.
#
# Usage:  ./fetch.sh
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

version=2.16
file="nvme-cli-$version.tar.gz"
url="https://github.com/linux-nvme/nvme-cli/archive/refs/tags/v$version.tar.gz"

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
