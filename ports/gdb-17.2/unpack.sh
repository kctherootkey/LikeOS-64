#!/bin/sh
# unpack.sh — unpack the vendored gdb tarball and apply the LikeOS patches.
#
# The tarball stays pristine and every deviation from upstream is one
# reviewable file under patches/.  Editing src/ by hand works right up until
# the next re-unpack throws the change away with nothing to say it ever
# existed, so changes belong in a patch or they do not belong at all.
#
# Unpacks to src/ rather than beside the tarball: the archive's own top-level
# directory is also called gdb-17.2, and ports/gdb-17.2/gdb-17.2/ reads badly
# enough to be worth one rename.  src/ and build/ are both generated and are
# ignored by git.
#
# Idempotent: an existing src/ is left alone.  Use ./unpack.sh --force to
# discard it and start from the tarball again, which is how you pick up an
# edited patch.
#
# Usage:  ./unpack.sh [--force]

set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

tarball="gdb-17.2.tar.gz"
src="src"

force=0
[ "${1:-}" = "--force" ] && force=1

if [ ! -f "$tarball" ]; then
	echo "unpack.sh: $tarball is missing" >&2
	exit 1
fi

if [ -d "$src" ]; then
	if [ "$force" = 0 ]; then
		echo "  have $src/  (./unpack.sh --force to re-unpack)"
		exit 0
	fi
	echo "  rm   $src/"
	rm -rf "$src"
fi

tmp=".unpack.$$"
rm -rf "$tmp"
mkdir -p "$tmp"

echo "  tar  $tarball"
if ! tar xf "$tarball" -C "$tmp"; then
	echo "unpack.sh: cannot unpack $tarball" >&2
	rm -rf "$tmp"
	exit 1
fi

# Exactly one top-level directory is expected.  Take whatever it is called so a
# re-roll of the archive under a different name does not need a change here.
inner=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -1)
if [ -z "$inner" ]; then
	echo "unpack.sh: no directory inside $tarball" >&2
	rm -rf "$tmp"
	exit 1
fi
mv "$inner" "$src"
rm -rf "$tmp"

# Patches apply in name order; number them so the order is visible.
if [ -d patches ]; then
	n=0
	for p in patches/*.patch; do
		[ -f "$p" ] || continue
		if ! (cd "$src" && patch -p1 --forward --silent <"../$p"); then
			echo "unpack.sh: FAILED to apply $(basename "$p")" >&2
			exit 1
		fi
		echo "  patch $(basename "$p")"
		n=$((n + 1))
	done
	[ "$n" = 0 ] && echo "  (no patches yet)"
fi

echo "unpack.sh: gdb-17.2 ready in $src/"
