#!/bin/sh
# Unpack the fetched tarballs into predictable directory names.
#
# The three sources fetch.sh uses do not agree on what a tarball unpacks to:
#
#   x.org / Debian   ->  <name>-<version>/          (what everything expects)
#   GitLab tag       ->  <project>-<name>-<version>/  e.g. macros-util-macros-1.20.2
#
# and the project name is not derivable from the package name (util-macros
# lives in a repo called "macros", font-misc-misc in "misc-misc").  Rather than
# teach every package's build about that, normalise here: after this runs,
# ports/xorg/<name>-<version>/ exists no matter where the bytes came from.
#
# A GitLab export also has no configure script; likeos-autogen.sh notices that
# and runs autogen.sh first, so nothing downstream has to care.
#
# Usage:  ./unpack.sh [package ...]      (no arguments = everything)

set -u

here=$(cd "$(dirname "$0")" && pwd)
list="$here/packages.list"

want=$*
done_n=0
fail_n=0

while read -r section name version deb repo rest; do
	case "$section" in
	'' | '#'*) continue ;;
	esac
	[ -n "${want}" ] && case " $want " in *" $name "*) ;; *) continue ;; esac

	# Take whatever version is actually on disk rather than insisting on the
	# pinned one: when the x.org archive is unreachable the Debian fallback
	# often supplies an older release, and the tree should still be
	# unpacked and built.  fetch.sh reports which version it took.
	tarball=""
	for cand in "$here/$name-"*.tar.xz "$here/$name-"*.tar.gz \
		"$here/$name-"*.tar.bz2; do
		[ -f "$cand" ] || continue
		tarball="$cand"
		break
	done
	if [ -z "$tarball" ]; then
		echo "  MISS $name (no tarball; run fetch.sh first)" >&2
		fail_n=$((fail_n + 1))
		continue
	fi

	# Recover the version from the filename so the directory matches the
	# tarball rather than the pin.
	base=$(basename "$tarball")
	have_ver=$(printf '%s' "$base" | sed "s/^$name-//; s/\.tar\..*$//")
	dest="$here/$name-$have_ver"
	if [ -d "$dest" ]; then
		echo "  have $name-$have_ver/"
		done_n=$((done_n + 1))
		continue
	fi

	tmp="$here/.unpack.$$"
	rm -rf "$tmp"
	mkdir -p "$tmp"
	if ! tar xf "$tarball" -C "$tmp" 2>/dev/null; then
		echo "  FAIL $name-$have_ver (cannot unpack $base)" >&2
		rm -rf "$tmp"
		fail_n=$((fail_n + 1))
		continue
	fi

	# Exactly one top-level directory is expected; take it whatever it is
	# called, which is what absorbs the GitLab naming difference.
	inner=$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -1)
	if [ -z "$inner" ]; then
		echo "  FAIL $name-$have_ver (no directory inside the tarball)" >&2
		rm -rf "$tmp"
		fail_n=$((fail_n + 1))
		continue
	fi
	mv "$inner" "$dest"
	rm -rf "$tmp"

	# Port changes to upstream source live in patches/<name>/ and are applied
	# here, never edited into the tree by hand.  The tarball stays pristine
	# and every deviation from upstream is one reviewable file that survives
	# a re-unpack -- an in-place edit would be silently lost by the next one.
	if [ -d "$here/patches/$name" ]; then
		patched_ac=0
		for p in "$here/patches/$name"/*.patch; do
			[ -f "$p" ] || continue
			if ! (cd "$dest" && patch -p1 --forward --silent <"$p"); then
				echo "  FAIL $name-$have_ver (patch $(basename "$p"))" >&2
				fail_n=$((fail_n + 1))
			fi
			grep -q '^+++ b/configure\.ac$' "$p" && patched_ac=1
		done

		# A patch to configure.ac does nothing on its own: release
		# tarballs ship a generated `configure`, and that is what runs.
		# Removing it makes likeos-autogen.sh regenerate from the
		# patched source.  Without this the patch applies cleanly,
		# the build fails anyway, and nothing says why.
		if [ "$patched_ac" = 1 ]; then
			rm -f "$dest/configure"
		fi
	fi

	if [ -f "$dest/configure" ]; then
		echo "  ok   $name-$have_ver/"
	else
		echo "  ok   $name-$have_ver/  (git export: will be autoreconf'd)"
	fi
	done_n=$((done_n + 1))
done <"$list"

echo "unpacked/present: $done_n   failed: $fail_n"
[ "$fail_n" -eq 0 ]
