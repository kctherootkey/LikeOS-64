#!/bin/sh
# Return ports/xorg/ to its checked-in state.
#
# Unlike the other ports, the source trees here are NOT in the repository: they
# are unpacked from tarballs by unpack.sh, so cleaning means deleting them
# outright rather than running each package's own clean rule.  Forty-seven
# `make distclean` invocations would also be slower and less thorough -- a
# half-configured tree that no longer has a Makefile cannot clean itself.
#
# The TARBALLS are kept.  They are the sources for this port, in the same sense
# that the checked-in trees are the sources for the others, and re-downloading
# forty-seven of them to rebuild would make `make distclean && make` need a
# network connection.  Pass -a to remove those too.
#
# Usage:
#   clean.sh      remove unpacked trees, stamps, logs and the sysroot
#   clean.sh -a   also remove the downloaded tarballs

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"

# Which package set to clean; see the comment in build.sh.  A sub-port cleans
# its own trees and leaves the sysroot alone: the two share it, and removing it
# from under the X.Org port would turn "rebuild GTK" into "rebuild everything".
port=$(cd "${LIKEOS_PORT_DIR:-$here}" && pwd)
sub=0
[ "$port" = "$here" ] || sub=1

all=0
aflag=""
if [ "${1:-}" = "-a" ]; then
	all=1
	aflag="-a"
fi

# Clean the sub-ports first, while this directory still exists to find them in.
# Each is invoked through its own wrapper, which knows which directory it is.
if [ "$sub" = 0 ]; then
	for c in "$here"/*/clean.sh; do
		[ -x "$c" ] || continue
		"$c" $aflag
	done
fi

# Every directory here is an unpacked tarball EXCEPT the ones that are part of
# the port itself.  Listing what to keep rather than what to delete is the safe
# direction: a package added later is cleaned without anyone remembering to
# update this, whereas a new port file left off a delete-list is destroyed.
#
# A directory holding a packages.list is a sub-port, not an unpacked tarball --
# it was just cleaned by its own script above, and deleting it here would take
# its manifest, patches and tarballs with it.
n=0
for d in "$port"/*/; do
	[ -d "$d" ] || continue
	case "$(basename "$d")" in
	toolchain | patches | .stamps | .logs) continue ;;
	# man/ holds manual pages the port WRITES, for programs that publish
	# none of their own (Mousepad).  Like patches/ it is ours and no tarball
	# can put it back -- and losing it is quiet: the next build simply
	# produces an image with one page missing.
	man) continue ;;
	esac
	[ -f "$d/packages.list" ] && continue
	rm -rf "$d"
	n=$((n + 1))
done

rm -rf "$port/.stamps" "$port/.logs"

# The sysroot is shared, so only the top-level clean removes it.
[ "$sub" = 0 ] && rm -rf "$SYSROOT"

label="ports/${port#"$root"/ports/}"
also="the sysroot"
[ "$sub" = 1 ] && also="stamps (the shared sysroot is kept)"
if [ "$all" = 1 ]; then
	rm -f "$port"/*.tar.* "$port/checksums.sha256"
	echo "$label: removed $n source trees, $also and the tarballs"
else
	echo "$label: removed $n source trees and $also (tarballs kept)"
fi

exit 0
