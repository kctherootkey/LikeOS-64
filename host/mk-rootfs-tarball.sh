#!/bin/bash
# mk-rootfs-tarball.sh -- pack the installed system's root filesystem
# (made by host/stage-rootfs.sh) into likeos-rootfs.tar.zst, the image the
# LikeOS installer unpacks onto a disk.
#
#   host/mk-rootfs-tarball.sh [-e EPOCH] [-l LEVEL] [-T THREADS] [-V VERSION]
#                             [-F FAKEROOT_STATE] ROOTFS OUT.tar.zst
#
# Writes OUT.tar.zst and beside it
#   OUT.tar.zst.sha256   "<sha256>  <name>", as sha256sum writes it
#   OUT.tar.zst.info     version=, build= (the time of EPOCH, UTC), bytes=
#                        (the file data the archive unpacks to), files=
#                        (its entries), read by the installer for the
#                        space it needs and for its progress
#
# The archive (GNU tar, POSIX pax format) keeps owners and groups as
# numbers, modes with the setuid, setgid and sticky bits, symbolic links,
# hard links (as links), device nodes, FIFOs, extended attributes and
# ACLs.  The staging tree is the build user's; every file in the archive
# is root's (0:0, tar's --owner and --group; no fakeroot needed), which is
# also what mkfs.ext4 -d records for the live medium under fakeroot.  A
# tree staged inside a fakeroot session of its own (device nodes made with
# mknod, owners set with chown, attributes set there) is packed in that
# session, with its state file (-F), so the archive has what the session
# recorded; only then is fakeroot needed on the build host.  (Not in
# fakeroot otherwise: it keeps extended attributes and ACLs to itself, and
# the real ones of the tree would be lost.)
#
# Reproducible: the same tree gives the same bytes.  Members are sorted by
# name, no time is later than EPOCH (SOURCE_DATE_EPOCH: the time of the
# last commit), the pax headers carry no access or change times and no
# process id, and zstd's multi-threaded format does not depend on the
# number of threads.  -l 19 keeps the window at 8 MiB (no --long), which
# is what the installer's decompressor is prepared for.
#
# A rebuild whose tar stream is byte for byte the previous one (nothing in
# the tree changed) keeps the compressed archive instead of compressing it
# again: the tar stream is hashed first, and the hash is kept with the
# archive (OUT.tar.zst.key, with the level, the threads and the zstd
# version).
#
# Copyright (C) 2026 The LikeOS Project

set -euo pipefail

usage()
{
	echo "usage: $0 [-e EPOCH] [-l LEVEL] [-T THREADS] [-V VERSION]" \
	     "[-F FAKEROOT_STATE] ROOTFS OUT.tar.zst" >&2
	exit 2
}

die()
{
	echo "mk-rootfs-tarball: $*" >&2
	exit 1
}

epoch=${SOURCE_DATE_EPOCH:-}
level=19
threads=0
version=
state=
while getopts e:l:T:V:F: opt; do
	case $opt in
	F) state=$OPTARG ;;
	e) epoch=$OPTARG ;;
	l) level=$OPTARG ;;
	T) threads=$OPTARG ;;
	V) version=$OPTARG ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 2 ] || usage
rootfs=$1
out=$2

[ -d "$rootfs" ] || die "$rootfs: no such directory"
[ -z "$state" ] || [ -f "$state" ] || die "$state: no such fakeroot state file"
[ -f "$rootfs/boot/kernel.elf" ] || die "$rootfs has no /boot/kernel.elf: not a root filesystem tree"
case $epoch in
''|*[!0-9]*) die "the time stamp (-e or SOURCE_DATE_EPOCH) must be seconds since 1970, not '$epoch'" ;;
esac
case $level in
''|*[!0-9]*) die "bad zstd level '$level'" ;;
esac
# Beyond 19 zstd's window grows past 8 MiB.
[ "$level" -ge 1 ] && [ "$level" -le 19 ] || die "zstd level $level: 1 to 19"
case $threads in
''|*[!0-9]*) die "bad thread count '$threads'" ;;
esac

# fakeroot only for a tree staged in a fakeroot session (-F); otherwise
# tar's --owner=0 --group=0 make every file root's by itself.
tools="tar zstd sha256sum"
[ -z "$state" ] || tools="$tools fakeroot"
for t in $tools; do
	command -v "$t" >/dev/null || die "$t is not installed on the build host"
done
# (Read whole: grep -q stopping early would kill tar with SIGPIPE.)
tar_version=$(LC_ALL=C tar --version)
tar_help=$(LC_ALL=C tar --help)
case $tar_version in
*'GNU tar'*) ;;
*) die "tar is not GNU tar" ;;
esac
case $tar_help in
*--clamp-mtime*) ;;
*) die "GNU tar 1.29 or later is needed (--clamp-mtime)" ;;
esac
case $tar_help in
*--acls*) ;;
*) die "this GNU tar has no ACL support (--acls)" ;;
esac

# Sockets cannot be archived (tar leaves them out with a warning), and
# nothing in a staging tree should be one.
sock=$(find "$rootfs" -type s -print -quit)
[ -z "$sock" ] || die "$sock is a socket: runtime state in the staging tree"

dir=$(dirname "$out")
name=$(basename "$out")
mkdir -p "$dir"

# The one tar command: every member name starts with "./", the first is
# "./" itself (the root directory's mode and owner).  No --sparse: whether
# the build host reports holes depends on its filesystem, and the staging
# tree has no sparse files to save.  Every extended attribute, the ACLs
# (system.posix_acl_*) once, as --acls stores them.
if [ -n "$state" ]; then
	run=(fakeroot -i "$state" --)
	owner=()
else
	run=()
	owner=(--owner=0 --group=0)
fi
pack()
{
	"${run[@]}" tar --create --file=- --directory="$rootfs" \
		--format=posix --numeric-owner "${owner[@]}" --sort=name \
		--mtime="@$epoch" --clamp-mtime \
		--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
		--xattrs --xattrs-include='*' \
		--xattrs-exclude='system.posix_acl_*' --acls \
		.
}

# Members: one per path.
files=$(cd "$rootfs" && find . -printf . | wc -c)
# Data: the size of every regular file, a file with several hard links
# once (the archive holds its data once, its other names are links).
bytes=$(cd "$rootfs" && find . -type f -printf '%i %s\n' |
	LC_ALL=C sort -u -k1,1n | awk '{ s += $2 } END { printf "%d\n", s }')

zver=$(zstd --version | sed -n 's/.* v\([0-9.]*\).*/\1/p')
tar_sum=$(pack | sha256sum | cut -d' ' -f1)
key="tar=$tar_sum level=$level threads=$threads zstd=$zver"

if [ -f "$out" ] && [ -f "$out.sha256" ] && [ -f "$out.key" ] &&
   [ "$(cat "$out.key")" = "$key" ]; then
	echo "mk-rootfs-tarball: $rootfs is unchanged; $name kept"
	touch "$out"
else
	echo "mk-rootfs-tarball: packing $rootfs ($files entries," \
	     "$((bytes / 1048576)) MiB of data) into $name, zstd -$level ..."
	rm -f "$out.key"
	pack | zstd -q -"$level" -T"$threads" -o "$out.tmp" -f
	# The same stream twice, or a tree that changed while it was read.
	sum2=$(zstd -q -d -c "$out.tmp" | sha256sum | cut -d' ' -f1)
	[ "$sum2" = "$tar_sum" ] || die "$rootfs changed while it was packed"
	printf '%s  %s\n' "$(sha256sum < "$out.tmp" | cut -d' ' -f1)" \
		"$name" > "$out.sha256.tmp"
	mv -f "$out.sha256.tmp" "$out.sha256"
	mv -f "$out.tmp" "$out"
	echo "$key" > "$out.key"
fi

{
	echo "# $name -- the LikeOS system image the installer unpacks"
	echo "version=$version"
	echo "build=$(date -u -d "@$epoch" +%Y-%m-%dT%H:%M:%SZ)"
	echo "bytes=$bytes"
	echo "files=$files"
} > "$out.info.tmp"
mv -f "$out.info.tmp" "$out.info"
touch "$out"

echo "mk-rootfs-tarball: $out: $(du -h "$out" | cut -f1) ($files entries," \
     "$((bytes / 1048576)) MiB unpacked), SHA-256 $(cut -d' ' -f1 "$out.sha256")"
