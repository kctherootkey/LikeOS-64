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

all=0
[ "${1:-}" = "-a" ] && all=1

# Every directory here is an unpacked tarball EXCEPT the ones that are part of
# the port itself.  Listing what to keep rather than what to delete is the safe
# direction: a package added later is cleaned without anyone remembering to
# update this, whereas a new port file left off a delete-list is destroyed.
n=0
for d in "$here"/*/; do
	[ -d "$d" ] || continue
	case "$(basename "$d")" in
	toolchain | patches | .stamps | .logs) continue ;;
	esac
	rm -rf "$d"
	n=$((n + 1))
done

rm -rf "$here/.stamps" "$here/.logs"
rm -rf "$SYSROOT"

if [ "$all" = 1 ]; then
	rm -f "$here"/*.tar.* "$here/checksums.sha256"
	echo "ports/xorg: removed $n source trees, the sysroot and the tarballs"
else
	echo "ports/xorg: removed $n source trees and the sysroot (tarballs kept)"
fi

exit 0
