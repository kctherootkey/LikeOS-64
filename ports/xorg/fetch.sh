#!/bin/sh
# Fetch the X.Org source tarballs listed in packages.list.
#
# Sources, tried in order:
#
#   1. The canonical X.Org archive.  This is what upstream publishes and what
#      the version numbers in packages.list refer to.
#   2. The Debian source pool, for packages that name a fallback.  Debian's
#      .orig tarballs ARE the upstream ones (occasionally recompressed), so
#      this changes where the bytes come from, not what they contain.
#   3. X.Org's GitLab, by release tag.  This is the same upstream source at the
#      same tag, but exported from git rather than released: it has no
#      configure script, so the package has to be autoreconf'd first.
#      likeos-autogen.sh does that automatically when it finds autogen.sh and
#      no configure.  Several small X apps have no upstream tarball in Debian
#      at all (they are bundled into aggregate Debian-native packages), so
#      without this source they are unobtainable wherever x.org is blocked.
#
# Two sources exist because the canonical archive is not reachable from every
# network; the fallback keeps the port buildable where it is not.  A package
# with no fallback listed has no Debian equivalent worth using — Debian bundles
# several small X apps into aggregate source packages, and unpacking those
# would no longer be the upstream tree.
#
# Every download is recorded in checksums.sha256.  On a later run an existing
# tarball is verified against that file rather than re-fetched, so the set is
# reproducible and a corrupted or substituted tarball is caught.
#
# Usage:  ./fetch.sh [package ...]      (no arguments = everything)

set -u

here=$(cd "$(dirname "$0")" && pwd)
list="$here/packages.list"
sums="$here/checksums.sha256"

XORG_BASE="https://www.x.org/releases/individual"
DEBIAN_BASE="https://deb.debian.org/debian/pool/main"

# Packages that live outside the X.Org archive entirely.
freetype_url() {
	echo "https://download.savannah.gnu.org/releases/freetype/freetype-$1.tar.xz"
}
ctwm_url() {
	echo "https://www.ctwm.org/dist/ctwm-$1.tar.xz"
}

# Debian pools lib* under lib<4th letter>, everything else under its initial.
debian_dir() {
	case "$1" in
	lib?*) echo "lib$(printf '%s' "$1" | cut -c4)" ;;
	*) printf '%s' "$1" | cut -c1 ;;
	esac
}

GITLAB_BASE="https://gitlab.freedesktop.org"

# Source archive for a release TAG.  The result is a git export, not a release
# tarball: no configure, no generated parsers.  It unpacks to <project>-<tag>/,
# which is renamed to the <name>-<version>/ every other source produces so the
# rest of the build does not have to care where a package came from.
fetch_gitlab() {
	gname=$1
	gver=$2
	grepo=$3

	if [ -z "$grepo" ] || [ "$grepo" = "-" ]; then
		echo "  FAIL $gname-$gver (no source carries it)" >&2
		return 1
	fi
	gproj=$(basename "$grepo")
	gtag="$gname-$gver"
	gfile="$here/$gname-$gver.tar.gz"

	[ -f "$gfile" ] && { echo "  have $(basename "$gfile")"; return 0; }

	gurl="$GITLAB_BASE/$grepo/-/archive/$gtag/$gproj-$gtag.tar.gz"
	if curl -fsSL --connect-timeout 25 -o "$gfile.part" "$gurl"; then
		mv "$gfile.part" "$gfile"
		echo "  got  $(basename "$gfile")  [gitlab tag; needs autoreconf]"
		return 0
	fi
	rm -f "$gfile.part"
	echo "  FAIL $gname-$gver (not at x.org, Debian or GitLab)" >&2
	return 1
}

fetch_one() {
	section=$1
	name=$2
	version=$3
	deb=$4
	repo=$5

	# A fourth source: an explicit URL, given where the Debian source-package
	# field would go and recognised by its scheme.  %VERSION% in it expands to
	# the version column.
	#
	# Needed for two kinds of package that the three sources above cannot
	# reach.  One is upstream-only software (xnedit is published on GitHub and
	# is in neither the x.org archive nor Debian).  The other is a package
	# whose Debian tarball has the wrong SHAPE: Debian's expat orig tarball is
	# the project's git layout, with configure.ac one level down in expat/,
	# and every build script here expects the configure script at the root of
	# the unpacked tree.  The upstream release tarball is laid out normally.
	case "$deb" in
	http://* | https://*)
		url=$(printf '%s' "$deb" | sed "s|%VERSION%|$version|g")
		ext=$(printf '%s' "$url" | sed -n 's|.*\(\.tar\.[a-z]*\)$|\1|p')
		[ -n "$ext" ] || ext=.tar.gz
		file="$here/$name-$version$ext"
		[ -f "$file" ] && { echo "  have $(basename "$file")"; return 0; }
		if curl -fsSL --connect-timeout 20 -o "$file.part" "$url"; then
			mv "$file.part" "$file"
			echo "  got  $(basename "$file")  [upstream]"
			return 0
		fi
		rm -f "$file.part"
		echo "  FAIL $name-$version  (upstream URL)" >&2
		return 1
		;;
	esac

	# Already have it?
	#
	# Checked by NAME rather than by the exact file name each source would
	# produce, because the sources disagree about that: the Debian pool
	# carries libevdev as 1.13.6+dfsg and the x.org archive would call the
	# same tarball 1.13.6.  Testing only the canonical name meant a tarball
	# fetched from the pool was never recognised again, so every build
	# re-probed the archive, got a 404, and fell back to the pool -- eight
	# pointless network round-trips per build, and a build that needed a
	# network connection to tell it had everything already.
	#
	# unpack.sh takes the version from the tarball on disk for the same
	# reason, so matching on the name here is consistent with it.
	existing=$(ls "$here/$name"-*.tar.* 2>/dev/null | head -1)
	if [ -n "$existing" ]; then
		echo "  have $(basename "$existing")"
		return 0
	fi

	# The archive uses .tar.xz for everything current; a few older packages
	# are .tar.gz, so try both before giving up on a source.
	for ext in tar.xz tar.gz; do
		case "$name" in
		freetype) url=$(freetype_url "$version") ;;
		ctwm) url=$(ctwm_url "$version") ;;
		*) url="$XORG_BASE/$section/$name-$version.$ext" ;;
		esac
		file="$here/$(basename "$url")"

		if [ -f "$file" ]; then
			echo "  have $(basename "$file")"
			return 0
		fi
		if curl -fsSL --connect-timeout 20 -o "$file.part" "$url"; then
			mv "$file.part" "$file"
			echo "  got  $(basename "$file")  [x.org]"
			return 0
		fi
		rm -f "$file.part"
		# freetype/ctwm have exactly one URL; do not retry with .gz
		case "$name" in
		freetype | ctwm) break ;;
		esac
	done

	if [ -z "$deb" ] || [ "$deb" = "-" ]; then
		fetch_gitlab "$name" "$version" "$repo"
		return $?
	fi

	d=$(debian_dir "$deb")

	# Try the pinned version first, then whatever the pool actually has.
	# Debian frequently trails upstream by a release, and the point of the
	# fallback is to get a buildable tree rather than to insist on a version
	# this source does not carry.  The version actually taken is reported,
	# and lands in checksums.sha256.
	avail=$(curl -fsSL --connect-timeout 20 "$DEBIAN_BASE/$d/$deb/" 2>/dev/null |
		grep -oE "${deb}_[0-9][0-9a-zA-Z.+~:-]*\\.orig\\.tar\\.[a-z]+" |
		sed "s/^${deb}_//; s/\\.orig\\.tar\\..*//" |
		sort -uV)
	newest=$(printf '%s\n' "$avail" | tail -1)

	for v in "$version" "$newest"; do
		[ -z "$v" ] && continue
		for ext in tar.xz tar.gz tar.bz2; do
			url="$DEBIAN_BASE/$d/$deb/${deb}_${v}.orig.$ext"
			file="$here/$name-$v.$ext"
			[ -f "$file" ] && { echo "  have $(basename "$file")"; return 0; }
			if curl -fsSL --connect-timeout 20 -o "$file.part" "$url"; then
				mv "$file.part" "$file"
				if [ "$v" != "$version" ]; then
					echo "  got  $(basename "$file")  [debian; $version not carried there]"
				else
					echo "  got  $(basename "$file")  [debian]"
				fi
				return 0
			fi
			rm -f "$file.part"
		done
	done

	echo "  FAIL $name (wanted $version; pool has: $(printf '%s' "$avail" | tr '\\n' ' '))" >&2
	return 1
}

want=$*
failed=0
got=0

while read -r section name version deb repo rest; do
	case "$section" in
	'' | '#'*) continue ;;
	esac
	[ -n "${want}" ] && case " $want " in *" $name "*) ;; *) continue ;; esac
	deb=${deb:-}
	repo=${repo:-}
	echo "$name $version"
	if fetch_one "$section" "$name" "$version" "$deb" "$repo"; then
		got=$((got + 1))
	else
		failed=$((failed + 1))
	fi
done <"$list"

# Record what we have, so a later run verifies instead of re-downloading and a
# substituted tarball is noticed.
( cd "$here" && sha256sum *.tar.* 2>/dev/null | sort -k2 ) >"$sums.new"
if [ -s "$sums.new" ]; then
	mv "$sums.new" "$sums"
	echo "checksums written to $(basename "$sums")"
else
	rm -f "$sums.new"
fi

echo "fetched/present: $got   failed: $failed"
[ "$failed" -eq 0 ]
