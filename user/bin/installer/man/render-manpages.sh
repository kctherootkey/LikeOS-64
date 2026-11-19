#!/bin/sh
# render-manpages.sh - render the manual pages of the disk, file system and
# installer tools into the pre-formatted pages man(1) reads.
#
# A maintenance tool (make installer-manpages, mk/installer.mk), not part of
# the build: the rendered pages are checked in, and groff is not a build
# dependency.  It writes
#
#   res/man/NAME.1    every command, section 1 and section 8 alike (the
#                     page keeps its own section in its header)
#   res/man5/NAME.5   the file formats
#   res/man-live/NAME.1  the installers' pages: staged on the live medium
#                     only (mk/installer.mk, INSTALLER_STAGE_LIVE), as the
#                     programs are
#
# from the upstream troff sources in the unpacked port trees (run each
# port's unpack.sh first) and the LikeOS pages in this directory.  Sources
# that configure would fill in (@VERSION@ ...) or filter are made the way
# the port's own build makes them, with the values of the LikeOS build.
# A program known by several names (mkfs.ext4 is mke2fs) gets a copy of its
# page under every name it is installed as.
#
# Usage:  render-manpages.sh ROOT [HOST_OS]
#   ROOT     the top of the source tree
#   HOST_OS  the operating system name groff puts into the footer of an mdoc
#            page whose .Os is empty (the top-level Makefile's
#            MANPAGE_HOST_OS); it is replaced by LikeOS, as MANPAGE_FIXUP
#            does for the other pages
#
# Copyright (C) 2026 The LikeOS Project

set -eu

root=$(cd "$1" && pwd)
host_os=${2:-}
here="$root/user/installer/man"
ports="$root/ports"
man1="$root/res/man"
man5="$root/res/man5"
manlive="$root/res/man-live"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/installer-man.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$man1" "$man5" "$manlive"
: >"$tmp/done"

need() {
	[ -f "$1" ] || {
		echo "render-manpages.sh: $1 is missing (unpack the port first)" >&2
		exit 1
	}
}

# The footer fix of the top-level MANPAGE_FIXUP: an mdoc page with an empty
# .Os is stamped with the build host's name; that word becomes LikeOS,
# padded to the same width so the footer keeps its columns.
fixup() {
	awk -v old="$host_os" -v new=LikeOS '
		BEGIN { if (old == "") old = "\001"; n = length(old);
			while (length(new) < n) new = new " ";
			new = substr(new, 1, n) }
		{ if (index($0, old) == 1) sub(/^[^ ]*/, new); print }'
}

# render SOURCE OUT: troff (from a file, or stdin when SOURCE is -) to a
# 78-column catman page.  The pages are ASCII, as all of res/man: text
# outside ASCII in a source (a name with an accent) is transliterated
# first, which the ASCII device would otherwise drop.
render() {
	src=$1 out=$2
	if [ "$src" = - ]; then
		src=/dev/stdin
	fi
	LC_ALL=C.UTF-8 iconv -f UTF-8 -t ASCII//TRANSLIT <"$src" >"$tmp/page"
	GROFF_NO_SGR=1 groff -t -e -mandoc -Tascii -rLL=78n -rLT=78n \
		"$tmp/page" 2>"$tmp/warn" | col -bx | fixup >"$out.tmp"
	if [ -s "$tmp/warn" ]; then
		sed "s|^|  groff: $(basename "$out"): |" "$tmp/warn" >&2
	fi
	if [ ! -s "$out.tmp" ]; then
		rm -f "$out.tmp"
		echo "render-manpages.sh: $out rendered empty" >&2
		exit 1
	fi
	mv "$out.tmp" "$out"
	echo "$out" >>"$tmp/done"
	echo "  $(echo "$out" | sed "s|^$root/||") ($(wc -l <"$out") lines)"
}

# copies PAGE NAME...: the page of a program installed under more names.
copies() {
	page=$1
	shift
	for name in "$@"; do
		cp "$page" "$(dirname "$page")/$name.${page##*.}"
		echo "$name" >>"$tmp/done"
		echo "  $(echo "$(dirname "$page")/$name.${page##*.}" | sed "s|^$root/||") (copy of $(basename "$page"))"
	done
}

# ---- GNU tar, zstd, attr, acl ------------------------------------------

need "$ports/tar-1.35/src/doc/tar.1"
render "$ports/tar-1.35/src/doc/tar.1" "$man1/tar.1"

need "$ports/zstd-1.5.7/src/programs/zstd.1"
# Its SYNOPSIS is a one-cell table that tbl rejects (the generator wrote
# no format line); as the plain line it holds it renders as intended.
sed -e '/^\.TS$/,/^allbox;$/d' -e '/^\.TE$/d' \
	"$ports/zstd-1.5.7/src/programs/zstd.1" | render - "$man1/zstd.1"
copies "$man1/zstd.1" unzstd zstdcat

for p in getfattr setfattr attr; do
	need "$ports/attr-2.6.0/src/man/man1/$p.1"
	render "$ports/attr-2.6.0/src/man/man1/$p.1" "$man1/$p.1"
done
for p in getfacl setfacl chacl; do
	need "$ports/acl-2.4.0/src/man/man1/$p.1"
	render "$ports/acl-2.4.0/src/man/man1/$p.1" "$man1/$p.1"
done
need "$ports/acl-2.4.0/src/man/man5/acl.5"
render "$ports/acl-2.4.0/src/man/man5/acl.5" "$man5/acl.5"

# ---- e2fsprogs ----------------------------------------------------------
# configure's substitutions (util/subst.conf): the version and its date
# from version.h, the journal device documented (JDEV empty), the tdb
# scratch files of e2fsck.conf documented (the port builds tdb).

e2="$ports/e2fsprogs-1.47.4/src"
need "$e2/version.h"
e2ver=$(sed -n 's/^#define E2FSPROGS_VERSION "\(.*\)"/\1/p' "$e2/version.h")
e2date=$(sed -n 's/^#define E2FSPROGS_DATE "\(.*\)"/\1/p' "$e2/version.h")
e2year=$(echo "$e2date" | awk -F- '{ y = $3; if (y < 100) y += (y < 90 ? 2000 : 1900); print y }')
e2month=$(echo "$e2date" | awk -F- '{
	split("January February March April May June July August September October November December", m, " ")
	split("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec", a, " ")
	for (i = 1; i <= 12; i++) if (a[i] == $2) print m[i] }')
e2subst() {
	sed -e "s|@E2FSPROGS_VERSION@|$e2ver|g" -e "s|@E2FSPROGS_MONTH@|$e2month|g" \
	    -e "s|@E2FSPROGS_YEAR@|$e2year|g" -e "s|@JDEV@||g" \
	    -e "s|@TDB_MAN_COMMENT@||g" -e "s|@root_sysconfdir@|/etc|g" \
	    -e 's|\\\[mc\]|u|g' "$1"
}
for p in misc/mke2fs e2fsck/e2fsck misc/tune2fs misc/dumpe2fs resize/resize2fs \
	 debugfs/debugfs misc/badblocks misc/e2label; do
	need "$e2/$p.8.in"
	e2subst "$e2/$p.8.in" | render - "$man1/$(basename "$p").1"
done
copies "$man1/mke2fs.1" mkfs.ext2 mkfs.ext3 mkfs.ext4
copies "$man1/e2fsck.1" fsck.ext2 fsck.ext3 fsck.ext4
for p in misc/mke2fs.conf e2fsck/e2fsck.conf misc/ext4; do
	need "$e2/$p.5.in"
	e2subst "$e2/$p.5.in" | render - "$man5/$(basename "$p").5"
done

# ---- dosfstools ---------------------------------------------------------
# The version and release date of configure.ac, as Makefile.likeos puts
# them into version.h.

dosfs="$ports/dosfstools-4.2"
dver=$(sed -n 's/^VERSION := //p' "$dosfs/Makefile.likeos")
ddate=$(sed -n 's/^RELEASE_DATE := //p' "$dosfs/Makefile.likeos")
for p in mkfs.fat fsck.fat fatlabel; do
	need "$dosfs/src/manpages/$p.8.in"
	sed -e "s|@PACKAGE_VERSION@|$dver|g" -e "s|@RELEASE_DATE@|$ddate|g" \
	    "$dosfs/src/manpages/$p.8.in" | render - "$man1/$p.1"
done
copies "$man1/mkfs.fat.1" mkfs.vfat mkfs.msdos mkdosfs
copies "$man1/fsck.fat.1" fsck.vfat fsck.msdos dosfsck
copies "$man1/fatlabel.1" dosfslabel

# ---- util-linux: the tools of the disktools port ------------------------

ul="$ports/util-linux-2.42.4/src"
for p in misc-utils/lsblk misc-utils/blkid disk-utils/fdisk disk-utils/sfdisk \
	 misc-utils/wipefs disk-utils/blockdev sys-utils/blkdiscard \
	 misc-utils/findfs disk-utils/partx disk-utils/addpart \
	 disk-utils/delpart disk-utils/resizepart; do
	need "$ul/$p.8"
	render "$ul/$p.8" "$man1/$(basename "$p").1"
done

# ---- hdparm, nvme ---------------------------------------------------------

need "$ports/hdparm-9.65/src/hdparm.8"
render "$ports/hdparm-9.65/src/hdparm.8" "$man1/hdparm.1"
need "$ports/nvme-cli-2.16/src/Documentation/nvme.1"
render "$ports/nvme-cli-2.16/src/Documentation/nvme.1" "$man1/nvme.1"

# ---- smartmontools ------------------------------------------------------
# Upstream's MAN_FILTER (src/Makefile.am) with the settings of the port:
# configuration and the warning script in /etc, the drive database in
# /usr/share/smartmontools (no separate install location, no
# update-smart-drivedb), no init script, no state or attribute log files,
# no plugin directory, no libcap-ng, no systemd, NVMe devices scanned.  The
# sections for other systems' device backends are commented out: the one
# kept is the one configure pairs with the backend the port builds
# (OS_DEPS in its Makefile.likeos).

sm="$ports/smartmontools-7.5"
need "$sm/src/smartctl.8.in"
smver=$(sed -n 's/^AC_INIT(\[smartmontools\], *\[\([0-9.]*\)\].*/\1/p' "$sm/src/configure.ac")
smdbver=$(sed -n 's/^smartmontools_drivedb_version=//p' "$sm/src/configure.ac" | head -1)
smbranch=$(echo "$smdbver" | sed -n 's/^\([0-9]*\)\.\([0-9]*\).*/RELEASE_\1_\2_DRIVEDB/p')
# The revision stamp, made as the port's Makefile.likeos makes it.
(cd "$sm/src" && sh ./getversion.sh -i ids) >"$tmp/svnversion.h"
smdate=$(sed -n 's,^.*DATE[^"]*"\([^"]*\)".*$,\1,p' "$tmp/svnversion.h")
smrev=$(sed -n 's,^.*REV[^"]*"\([^"]*\)".*$,r\1,p' "$tmp/svnversion.h")
backend=$(sed -n 's/^OS_DEPS = \([A-Za-z0-9_]*\).*/\1/p' "$sm/Makefile.likeos")
osfilter=$(awk -v dep="$backend.o" '
	/os_deps=/ { inb = index($0, dep) > 0 }
	inb && /os_man_filter=/ { sub(/.*os_man_filter=/, ""); print; exit }' \
	"$sm/src/configure.ac")
[ -n "$osfilter" ] || { echo "render-manpages.sh: no man filter for $backend" >&2; exit 1; }
smfilter() {
	sed -e "s|CURRENT_SVN_VERSION|smartmontools-$smver|g" \
	    -e "s|CURRENT_SVN_DATE|$smdate|g" \
	    -e "s|CURRENT_SVN_REV|$smrev|g" \
	    -e 's|/usr/local/share/man/|/usr/share/man/|g' \
	    -e 's|/usr/local/sbin/|/usr/sbin/|g' \
	    -e 's|/usr/local/share/doc/smartmontools/examplescripts/|!exampledir!|g' \
	    -e 's|/usr/local/share/doc/smartmontools/|/usr/share/doc/smartmontools/|g' \
	    -e 's|!exampledir!|/usr/share/doc/smartmontools/examplescripts/|g' \
	    -e 's|/usr/local/etc/smartd\.conf|/etc/smartd.conf|g' \
	    -e 's|/usr/local/etc/smart_drivedb\.h|/etc/smart_drivedb.h|g' \
	    -e 's|/usr/local/etc/smartd_warning\.sh|/etc/smartd_warning.sh|g' \
	    -e "s|RELEASE_6_0_DRIVEDB|$smbranch|g" \
	    -e 's|/usr/local/share/smartmontools/drivedb\.h|/usr/share/smartmontools/drivedb.h|g' \
	    -e 's|/usr/local/var/lib/smartmontools/drivedb\.h|/usr/share/smartmontools/drivedb.h|g' "$1" |
	sed '/^\.\\" %IF ENABLE_DB_INSTALL/,/^\.\\" %ENDIF ENABLE_DB_INSTALL/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_UPDATE_SMART_DRIVEDB/,/^\.\\" %ENDIF ENABLE_UPDATE_SMART_DRIVEDB/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_INITSCRIPT/,/^\.\\" %ENDIF ENABLE_INITSCRIPT/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_SAVESTATES/,/^\.\\" %ENDIF ENABLE_SAVESTATES/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_ATTRIBUTELOG/,/^\.\\" %ENDIF ENABLE_ATTRIBUTELOG/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_SMARTDPLUGINDIR/,/^\.\\" %ENDIF ENABLE_SMARTDPLUGINDIR/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_CAPABILITIES/,/^\.\\" %ENDIF ENABLE_CAPABILITIES/ s,^,.\\"# ,' |
	sed '/^\.\\" %IF ENABLE_SYSTEMD_NOTIFY/,/^\.\\" %ENDIF ENABLE_SYSTEMD_NOTIFY/ s,^,.\\"# ,' |
	sed -e "s,OS_MAN_FILTER,$osfilter,g" \
	    -e "/^\.\\\\\" %IF NOT OS .*$osfilter/,/^.\\\\\" %ENDIF NOT OS .*$osfilter/ s,^,.\\\\\"# ," \
	    -e "/^\.\\\\\" %IF OS .*$osfilter/,/^\.\\\\\" %ENDIF OS .*$osfilter/ s,^,!!," \
	    -e '/^\.\\" %IF OS ./,/^\.\\" %ENDIF OS ./ s,^,.\\"# ,' \
	    -e '/^!*\.\\" %IF NOT OS ./,/^!*\.\\" %ENDIF NOT OS ./ s,^,!!,' \
	    -e 's,^!!!*\.\\"! \(.*\)$,\1 \\"#,' \
	    -e 's,^!!!*,,'
}
smfilter "$sm/src/smartctl.8.in" | render - "$man1/smartctl.1"
smfilter "$sm/src/smartd.8.in" | render - "$man1/smartd.1"
smfilter "$sm/src/smartd.conf.5.in" | render - "$man5/smartd.conf.5"

# ---- the LikeOS pages ---------------------------------------------------

for p in likeos-gpt likeos-efiboot mount umount; do
	render "$here/$p.1" "$man1/$p.1"
done
for p in likeos-install likeos-installer; do
	render "$here/$p.1" "$manlive/$p.1"
done
for p in fstab likeos-install.conf likeos-bootmenu.conf; do
	render "$here/$p.5" "$man5/$p.5"
done

echo "render-manpages.sh: $(wc -l <"$tmp/done") pages in res/man, res/man5 and res/man-live"
