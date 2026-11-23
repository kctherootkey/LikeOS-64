#!/bin/bash
# Whole paths of the kernel's full length on every filesystem driver, on the
# filesystem harness (host/test-fs.sh): a tree whose deepest file has an
# absolute path of exactly 4095 bytes (VFS_MAX_PATH - 1), a name of 255
# bytes, long symbolic link targets (an absolute one of ~3900 bytes, a
# relative one of ~3000), renames in and across deep directories, a mount
# cycle in between, everything removed again -- then the host's fsck.  ISO
# 9660 gets the same tree as a Rock Ridge image, read only.
#
#   host/test-deep-paths.sh [DRIVER...]    default: fat32 ext4 exfat isofs
#                                          (ntfs too when named)
# Environment as host/test-fs.sh (TESTFS_DIR, TESTFS_TIMEOUT, TESTFS_MEM).
# The generator is host/fsimg/deeppath.py.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-fs}
mkdir -p "$TESTFS_DIR" || exit 2
W=$(mktemp -d "$TESTFS_DIR/deep.XXXXXX") || exit 2
trap '[ "${KEEP:-0}" = 1 ] || rm -rf "$W"' EXIT
GEN="python3 -I $ROOT/host/fsimg/deeppath.py"
FSIMG="$ROOT/host/fsimg"
PASS=0
FAIL=0
SKIP=0

ok() { PASS=$((PASS + 1)); echo "PASS $1"; }
bad() {
	FAIL=$((FAIL + 1))
	echo "FAIL $1"
	[ -f "$W/$1.out" ] && grep -E '^FAIL|ERROR|runtime error|WARNING|test-fs:' "$W/$1.out" | head -20 | sed 's/^/    /'
}
skip() { SKIP=$((SKIP + 1)); echo "SKIP $1: $2"; }

# img VARIANT -> path (made once)
img() {
	local out="$W/$1.img"
	if [ ! -f "$out" ]; then
		TMPDIR="$W" bash "$FSIMG/mkimg.sh" "$1" "$out" > "$W/mkimg-$1.log" 2>&1 || {
			rm -f "$out"
			return 1
		}
	fi
	echo "$out"
}

# rw DRIVER VARIANT FSCK_TYPE [deeppath options]
rw() {
	local drv=$1 var=$2 fst=$3 name
	shift 3
	name="$drv-rw"
	local im
	im=$(img "$var") || { skip "$name" "image $var (host tool missing?)"; return; }
	$GEN rw-script "$@" > "$W/$name.script"
	$GEN expect "$@" > "$W/$name.expect"
	if host/test-fs.sh "$drv" "$im" --chunk 1000000 -s "$W/$name.script" > "$W/$name.out" 2>&1; then
		ok "$name"
	else
		bad "$name"
	fi
	# every readlink printed the whole target (twice: before and after
	# the mount cycle)
	if [ -s "$W/$name.expect" ]; then
		local miss=0
		while IFS= read -r l; do
			[ "$(grep -cxF -- "$l" "$W/$name.out")" -ge 2 ] || miss=1
		done < "$W/$name.expect"
		[ $miss = 0 ] && ok "$name-readlink" || bad "$name-readlink"
	fi
	if [ -n "$fst" ]; then
		if python3 "$FSIMG/oracle.py" fsck "$fst" "$im" > "$W/$name-fsck.out" 2>&1; then
			ok "$name-fsck"
		else
			cp "$W/$name-fsck.out" "$W/$name-fsck.txt"
			bad "$name-fsck"
			head -20 "$W/$name-fsck.out" | sed 's/^/    /'
		fi
	fi
}

iso() {
	local name=isofs-ro
	command -v xorriso > /dev/null || { skip "$name" "xorriso missing"; return; }
	mkdir -p "$W/isosrc"
	# The host's ISO tools take Rock Ridge paths up to 1024 bytes (libisofs);
	# the driver itself takes whole paths (its walk buffer and symbolic
	# link targets are VFS_MAX_PATH long).
	$GEN mktree "$W/isosrc" --symlinks --total 1000 || { bad "$name-tree"; return; }
	xorriso -as mkisofs -quiet -V LIKEOSDEEP -R -J -joliet-long \
		-o "$W/deep.iso" "$W/isosrc" > "$W/xorriso.log" 2>&1 ||
		{ cat "$W/xorriso.log"; bad "$name-image"; return; }
	$GEN ro-script --symlinks --total 1000 > "$W/$name.script"
	$GEN expect --symlinks --total 1000 > "$W/$name.expect"
	if host/test-fs.sh isofs "$W/deep.iso" --sector 2048 --trap -s "$W/$name.script" > "$W/$name.out" 2>&1; then
		ok "$name"
	else
		bad "$name"
	fi
	local miss=0
	while IFS= read -r l; do
		grep -qxF -- "$l" "$W/$name.out" || miss=1
	done < "$W/$name.expect"
	[ $miss = 0 ] && ok "$name-readlink" || bad "$name-readlink"
}

DRIVERS=${*:-fat32 ext4 exfat isofs}
for d in $DRIVERS; do
	case $d in
	# FAT: no symbolic links
	fat32) rw fat32 fat-basic fat --name 255 --xdir-rename ;;
	ext4) rw ext4 ext4-basic ext4 --symlinks --xdir-rename ;;
	exfat) rw exfat exfat-4k-basic exfat --xdir-rename ;;
	ntfs) rw ntfs ntfs-4k-basic ntfs --xdir-rename --symlinks ;;
	isofs) iso ;;
	*) echo "test-deep-paths: unknown driver $d" >&2; exit 2 ;;
	esac
done
echo "test-deep-paths: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL = 0 ]
