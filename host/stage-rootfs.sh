#!/bin/sh
# stage-rootfs.sh -- the root filesystem of an INSTALLED LikeOS, made from
# the common staging tree of the release build.
#
#   host/stage-rootfs.sh -f FSTAB COMMON ROOTFS
#
# COMMON is the tree everything is staged into first (build/ext4_staging,
# before anything that belongs to the live medium alone is added to it).
# ROOTFS is created as an exact copy of it -- modes, setuid bits, symbolic
# and hard links, extended attributes and times kept -- and then made into
# the system the installer puts on a disk:
#
#   /etc/fstab      FSTAB (res/etc/fstab.rootfs): the template the installer
#                   completes with the root and the EFI system partition
#   removed         what is state of one machine or one session rather than
#                   part of the system: the SSH host keys (every
#                   installation makes its own at its first start, see
#                   /etc/inittab), a machine id (init makes one at the first
#                   start), and the contents of /tmp, /var/tmp, /var/run and
#                   /var/log (only /tmp/.X11-unix stays, the X server needs
#                   the directory)
#
# What the live medium alone has -- its own fstab, the installer, its session and inittab, the
# installers' manual pages, /install, an ADD_USER account -- is staged into COMMON only after this copy was
# taken.  Finding any of it in COMMON means the build staged it in the
# wrong place: that is an error, never silently cleaned up.
#
# The tree is a staging tree: owned by the user running the build, made
# root's by the fakeroot that packs it (host/mk-rootfs-tarball.sh), as
# mkfs.ext4 -d does for the live medium.
#
# Copyright (C) 2026 The LikeOS Project

set -eu

usage()
{
	echo "usage: $0 -f FSTAB COMMON ROOTFS" >&2
	exit 2
}

fstab=
while getopts f: opt; do
	case $opt in
	f) fstab=$OPTARG ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 2 ] && [ -n "$fstab" ] || usage
common=$1
rootfs=$2

die()
{
	echo "stage-rootfs: $*" >&2
	exit 1
}

[ -f "$fstab" ] || die "$fstab: no such file"
[ -d "$common" ] || die "$common: no such directory"
[ -f "$common/boot/kernel.elf" ] || die "$common has no /boot/kernel.elf: not a staging tree"
case $(cd "$common" && pwd -P)/ in
"$(mkdir -p "$rootfs" && cd "$rootfs" && pwd -P)"/*)
	die "$rootfs and $common are the same tree" ;;
esac
case $(cd "$rootfs" && pwd -P)/ in
"$(cd "$common" && pwd -P)"/*)
	die "$rootfs lies inside $common" ;;
esac

# The live medium's own files: none of them may be in the common tree.
live_only="
	install
	etc/inittab.install
	etc/likeos/live
	LIKEOS.LIVE
	usr/bin/likeos-installer
	usr/sbin/likeos-install
	usr/libexec/likeos-installer-session
	usr/share/likeos/installer
	usr/share/man/man1/likeos-install.1
	usr/share/man/man1/likeos-installer.1
"
bad=
for p in $live_only; do
	if [ -e "$common/$p" ] || [ -L "$common/$p" ]; then
		bad="$bad /$p"
	fi
done
# /home is empty on every installed system: the installer makes the
# accounts.  Something in it is the ADD_USER account of the live medium.
if [ -d "$common/home" ] && [ -n "$(ls -A "$common/home")" ]; then
	bad="$bad /home/$(ls -A "$common/home" | head -n 1)"
fi
[ -z "$bad" ] || die "the common staging tree $common holds files of the live medium:$bad"

# A fresh copy.  cp -a keeps the modes (setuid included: the files are the
# user's own), links, extended attributes and times; --reflink=auto
# shares the blocks where the build host's filesystem can.
rm -rf "$rootfs"
cp -a --reflink=auto "$common" "$rootfs"

install -m 0644 "$fstab" "$rootfs/etc/fstab"
# The root directory: 0755, as mkfs.ext4 -d makes it on the live medium
# (it takes the mode of every directory below from the staging tree, not
# that of the top one).
chmod 0755 "$rootfs"

removed=0
remove()
{
	for f in "$@"; do
		if [ -e "$f" ] || [ -L "$f" ]; then
			rm -rf "$f"
			removed=$((removed + 1))
		fi
	done
}

# One machine's identity.
remove "$rootfs"/etc/ssh/ssh_host_*_key "$rootfs"/etc/ssh/ssh_host_*_key.pub
remove "$rootfs/etc/machine-id" "$rootfs/var/lib/dbus/machine-id"
# Temporary files and runtime state.  The directories stay (with their
# modes); what is in them goes.
for d in tmp var/tmp var/run var/log; do
	[ -d "$rootfs/$d" ] || continue
	for f in "$rootfs/$d"/* "$rootfs/$d"/.[!.]* "$rootfs/$d"/..?*; do
		case $f in
		"$rootfs/tmp/.X11-unix") continue ;;
		esac
		remove "$f"
	done
done
# /tmp/.X11-unix is a directory the X server puts its socket in, empty.
if [ -d "$rootfs/tmp/.X11-unix" ]; then
	for f in "$rootfs/tmp/.X11-unix"/* "$rootfs/tmp/.X11-unix"/.[!.]*; do
		remove "$f"
	done
fi

# What an installation cannot do without.
for p in boot/kernel.elf sbin/init etc/fstab etc/inittab etc/passwd \
	 etc/group etc/shadow usr/lib/likeos/efi/BOOTX64.EFI; do
	[ -f "$rootfs/$p" ] || die "$rootfs/$p is missing"
done

echo "stage-rootfs: $rootfs: $(find "$rootfs" -printf . | wc -c) entries," \
     "$(du -sh "$rootfs" | cut -f1); $removed host keys/runtime files left out"
