#!/bin/bash
# Host tests of the ext4 driver (kernel/fs/ext4/) on the filesystem harness
# (host/test-fs.sh): the driver compiled with the real page, inode and
# dentry caches, AddressSanitizer and UBSan, against file-backed devices.
# Beyond the driver's suite (host/fsimg/suite-ext4.sh) this covers:
#
#   host/test-ext4.sh            everything below except "big"
#   host/test-ext4.sh sector     devices of 512..4096-byte sectors: the
#                                superblock inside the first sector, the
#                                bytes around it kept, block sizes below
#                                the sector size refused
#   host/test-ext4.sh idle       write trap: a read-write mount that changes
#                                nothing writes nothing (also the first one,
#                                whose journal is still to become csum v3),
#                                on every variant, also after remount ro->rw
#   host/test-ext4.sh journal    transactions of more tags than one
#                                descriptor holds, batches near the size of
#                                the log, the csum-v3 upgrade with the first
#                                commit, a host e2fsck replay of our log
#   host/test-ext4.sh locks      unit test of shared-hold attribution and
#                                release_locks_for_task (test-ext4-rwsem.c);
#                                no user memory touched with a filesystem
#                                lock held (test-ext4-uaccess.c)
#   host/test-ext4.sh handles    several handles of one file: O_APPEND
#                                writes at the current end, reads and
#                                SEEK_END see the size another handle made
#   host/test-ext4.sh regress    regression cases (stale cached block tail
#                                after a truncate on 1 KB blocks, ...),
#                                badblk included
#   host/test-ext4.sh badblk     damaged block numbers (block maps, extents,
#                                xattr block, group descriptors, journal
#                                inode and tags, block bitmaps): never a
#                                write outside the filesystem or onto its
#                                metadata
#   host/test-ext4.sh errors     after an error: the journal aborted (the
#                                uncommitted batch never logged, a committed
#                                transaction kept for replay), a replay that
#                                fails refuses the mount (norecovery mounts
#                                read-only), a damaged block map fails the
#                                read with EIO, messages rate limited and
#                                the superblock written once (regress runs
#                                it too)
#   host/test-ext4.sh matrix     stress with model comparison + e2fsck -fn
#                                after every workload: 1/2/4 KB blocks,
#                                64bit or not, metadata_csum or not, ext3
#                                (block maps) and ext2 (no journal), 512-
#                                byte and native-size sectors
#   host/test-ext4.sh powercut   power cut after N writes over a stress run
#                                (N swept): our replay and the host's
#                                (e2fsck journal_only) must both leave a
#                                filesystem e2fsck -fn finds clean
#   host/test-ext4.sh fuzz [N]   metadata fuzzing, read-only and read-write,
#                                new seeds (N iterations each, default 300)
#
# Every run is bounded (timeout, and a memory-capped scope when systemd
# allows one).  Work files go to $TMPDIR/likeos-test-ext4.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
WHAT=${1:-all}
W=${TMPDIR:-/tmp}/likeos-test-ext4
mkdir -p "$W"
export TESTFS_DIR=${TESTFS_DIR:-$W/tfs}
BIN=$(host/test-fs.sh ext4 --build 2>/dev/null | tail -1)
[ -x "$BIN" ] || { echo "test-ext4: the runner does not build" >&2; exit 2; }
command -v mkfs.ext4 >/dev/null && command -v e2fsck >/dev/null || {
	echo "test-ext4: mkfs.ext4 / e2fsck missing" >&2
	exit 77
}
PASS=0
FAIL=0
FAILS=""
export LANG=C LC_ALL=C

ok() { PASS=$((PASS + 1)); echo "PASS $1"; }
bad() {
	FAIL=$((FAIL + 1))
	FAILS="$FAILS $1"
	echo "FAIL $1"
	[ -f "$W/$1.out" ] && grep -E '^FAIL|ERROR|WRITE TRAP|FATAL|BUG|DEADLOCK|runtime error|WARNING|test-fs:' \
		"$W/$1.out" | head -15 | sed 's/^/    /'
}

capped() { # capped SECONDS CMD...
	local t=$1
	shift
	if systemd-run --user --scope -q -p MemoryMax=1M true >/dev/null 2>&1; then
		timeout -k 5 "$t" systemd-run --user --scope -q -p MemoryMax=2G \
			-p MemorySwapMax=0 -- nice -n 10 "$@"
	else
		timeout -k 5 "$t" nice -n 10 "$@"
	fi
}

run() { # run NAME IMAGE ARGS... (output in $W/NAME.out)
	local name=$1
	shift
	capped 600 "$BIN" "$@" > "$W/$name.out" 2>&1
}

fsck_clean() { # fsck_clean NAME IMAGE
	e2fsck -fn "$2" > "$W/$1.fsck" 2>&1
}

# A populated image: mkimg VARIANT -> path (made once).  Variants are
# BLOCK-FEATURES: features 64csum (mkfs default: 64bit, metadata_csum),
# no64 (no 64bit), nocsum (no metadata_csum), no64nocsum, uninit (group
# descriptor checksums of the older kind: uninit_bg, no metadata_csum),
# ext3, ext2.
mkimg() {
	local v=$1 out="$W/img-$1.img" bs opt="" t=ext4
	if [ ! -f "$out" ]; then
		bs=${v%%-*}
		case ${v#*-} in
		64csum) ;;
		no64) opt="-O ^64bit" ;;
		nocsum) opt="-O ^metadata_csum" ;;
		no64nocsum) opt="-O ^64bit,^metadata_csum" ;;
		uninit) opt="-O ^metadata_csum,uninit_bg" ;;
		ext3) t=ext3 ;;
		ext2) t=ext2 ;;
		*) echo "test-ext4: unknown variant $v" >&2; return 2 ;;
		esac
		rm -rf "$W/src"
		python3 host/fsimg/mktree.py tree basic 1 "$W/src" > /dev/null || return 1
		mkfs.ext4 -q -F -t $t -b $bs $opt -E root_owner=0:0 -d "$W/src" \
			"$out.tmp" 64M > /dev/null 2>&1 || return 1
		mv "$out.tmp" "$out"
	fi
	echo "$out"
}

copy() { # copy IMAGE NAME -> a private copy
	cp --sparse=always "$1" "$W/$2.img" && echo "$W/$2.img"
}

VARIANTS="4096-64csum 1024-64csum 2048-64csum 4096-no64 1024-no64nocsum
	  4096-nocsum 2048-no64nocsum 4096-uninit 1024-uninit 4096-ext3
	  1024-ext3 1024-ext2 4096-ext2"

# ---- sector sizes ---------------------------------------------------------
t_sector() {
	local img c ss bs name
	for v in 4096-64csum 2048-64csum 1024-64csum 4096-no64; do
		img=$(mkimg $v) || { bad "sector-img-$v"; continue; }
		bs=${v%%-*}
		for ss in 512 1024 2048 4096; do
			name=sector-$v-$ss
			c=$(copy "$img" $name)
			if [ "$ss" -gt "$bs" ]; then
				# a block smaller than a sector cannot be addressed
				if run $name "$c" --cow --sector $ss -c "=INVALID mount ro" &&
				   grep -q "smaller than the device's sector size" "$W/$name.out"; then
					ok $name
				else
					bad $name
				fi
				rm -f "$c"
				continue
			fi
			# a boot area before the superblock must survive the
			# superblock's stores (read-modify-write of sector 0)
			printf 'LIKEOS-BOOT-AREA' | dd of="$c" bs=1 seek=16 conv=notrunc 2>/dev/null
			if run $name "$c" --sector $ss -c "mount; stress 7 600 200; umount" &&
			   fsck_clean $name "$c" &&
			   [ "$(dd if="$c" bs=1 skip=16 count=16 2>/dev/null)" = LIKEOS-BOOT-AREA ]; then
				ok $name
			else
				bad $name
			fi
			rm -f "$c"
		done
	done
}

# ---- idle read-write mounts write nothing --------------------------------
t_idle() {
	local img name
	for v in $VARIANTS; do
		img=$(mkimg $v) || { bad "idle-img-$v"; continue; }
		name=idle-$v
		if run $name "$img" --cow --trap -c \
			"mount rw; readall; statfs; ls /; umount; mount rw; readall; cycle; umount; mount ro; remount rw; readall; sync; umount"; then
			ok $name
		else
			bad $name
		fi
	done
	# the first write afterwards upgrades the journal to csum v3
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" idle-upgrade)
	if run idle-upgrade "$c" -c "mount; write /new text:x; sync; umount" &&
	   grep -q "journal to csum-v3" "$W/idle-upgrade.out" &&
	   dumpe2fs -h "$c" 2>/dev/null | grep -q 'Journal features:.*journal_checksum_v3' &&
	   dumpe2fs -h "$c" 2>/dev/null | grep -q 'Journal checksum type:.*crc32c' &&
	   fsck_clean idle-upgrade "$c"; then
		ok idle-upgrade
	else
		bad idle-upgrade
	fi
	rm -f "$c"
}

# ---- journal: big transactions ------------------------------------------
t_journal() {
	local img c v name
	# 1 KB blocks: one descriptor holds 62 csum-v3 tags; a batch of a
	# few hundred blocks (many directories and files in one operation
	# set) spans several descriptors.  No WARN may fire (a WARN marks a
	# fallback to writing outside the journal).
	for v in 1024-64csum 1024-no64nocsum 2048-64csum 4096-64csum; do
		img=$(mkimg $v) || continue
		name=journal-big-$v
		c=$(copy "$img" $name)
		cmds="mount"
		for i in $(seq 1 40); do cmds="$cmds; mkdir /jb$i; write /jb$i/f pat:$i:3000; setxattr /jb$i/f user.k text:v$i"; done
		cmds="$cmds; sync; cycle; tree /; umount"
		if run $name "$c" -c "$cmds" && fsck_clean $name "$c"; then
			ok $name
		else
			bad $name
		fi
		rm -f "$c"
	done
	# a tiny journal (1024 blocks, the minimum) under a stress run whose
	# batches come close to the log's size
	img=$(mkimg 1024-64csum)
	c=$(copy "$img" journal-tiny)
	tune2fs -O ^has_journal "$c" > /dev/null 2>&1
	tune2fs -J size=1 "$c" > /dev/null 2>&1
	if run journal-tiny "$c" -c "mount; stressopt budget 4M; stress 21 1200 300; umount" &&
	   fsck_clean journal-tiny "$c"; then
		ok journal-tiny
	else
		bad journal-tiny
	fi
	rm -f "$c"
	# our log, replayed by the host: a big batch is committed by a sync,
	# and the power is cut at every write of that sync in turn -- some
	# cuts land between the commit and the end of the checkpoint, where
	# the log holds the whole batch.  The host's e2fsck replays it, our
	# driver replays it: both must leave a clean filesystem.
	for v in 1024-64csum 4096-64csum 1024-no64nocsum; do
		img=$(mkimg $v) || continue
		name=journal-host-replay-$v
		c=$(copy "$img" $name)
		cmds="mount"
		for i in $(seq 1 30); do cmds="$cmds; mkdir /hr$i; write /hr$i/f pat:$i:2000"; done
		run $name-log "$c" --cow -c "$cmds; log on; sync; log dump $W/$name.log; umount" ||
			{ bad $name; continue; }
		local nw n failed=0 replays=0
		nw=$(awk '$2 == "W"' "$W/$name.log" | wc -l)
		for n in $(seq 1 "$nw"); do
			cp --sparse=always "$img" "$c"
			capped 300 "$BIN" "$c" --quiet --allow-warn -c "$cmds; fault cut $n; ?sync; ?umount" > /dev/null 2>&1
			cp --sparse=always "$c" "$c.host"
			e2fsck -E journal_only -y "$c.host" > "$W/$name.jo" 2>&1
			fsck_clean $name "$c.host" || { failed=1; echo "    host replay after $n of $nw writes: not clean"; }
			if ! run $name-ours "$c" -c "mount; tree /; umount" || ! fsck_clean $name-ours "$c"; then
				failed=1
				echo "    our replay after $n of $nw writes: not clean"
			fi
			grep -q "journal replay complete ([1-9]" "$W/$name-ours.out" && replays=$((replays + 1))
		done
		echo "    $v: $nw writes in the sync, $replays cut points replayed a transaction"
		if [ $failed = 0 ] && [ $replays -gt 0 ]; then
			ok $name
		else
			bad $name
		fi
		rm -f "$c" "$c.host"
	done
	# a committed transaction of more tags than one descriptor holds (1 KB
	# blocks: 62), caught in the log by a power cut after its commit
	# block: debugfs must see two descriptors and the commit, and the
	# host's replay and ours must both leave a clean filesystem
	if command -v debugfs >/dev/null; then
		img=$(mkimg 1024-64csum)
		name=journal-multidesc
		c="$W/$name.img"
		cmds="mount"
		for i in $(seq 1 25); do cmds="$cmds; mkdir /hr$i; mkdir /hr$i/a; mkdir /hr$i/b; write /hr$i/f pat:$i:5000; setxattr /hr$i/f user.k text:v$i"; done
		local found=0 log
		for n in $(seq 1 400); do
			cp --sparse=always "$img" "$c"
			capped 300 "$BIN" "$c" --quiet --allow-warn -c "$cmds; fault cut $n; ?sync; ?umount" > /dev/null 2>&1
			log=$(debugfs -R logdump "$c" 2>/dev/null)
			[ "$(echo "$log" | grep -c 'descriptor block')" -ge 2 ] &&
				echo "$log" | grep -q 'commit block' || continue
			found=1
			break
		done
		if [ $found = 1 ]; then
			cp --sparse=always "$c" "$c.host"
			e2fsck -E journal_only -y "$c.host" > /dev/null 2>&1
			if fsck_clean $name-host "$c.host" &&
			   run $name "$c" -c "mount; tree /; umount" &&
			   grep -q "journal replay complete ([1-9]" "$W/$name.out" &&
			   fsck_clean $name "$c"; then
				echo "    cut after $n writes: $(echo "$log" | grep -c 'descriptor block') descriptors, both replays clean"
				ok $name
			else
				bad $name
			fi
		else
			echo "    no cut point left a committed multi-descriptor transaction"
			bad $name
		fi
		rm -f "$c" "$c.host"
	fi
}

# ---- locks ------------------------------------------------------------------
t_locks() {
	local B SAN KF
	B=$(dirname "$BIN")
	SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
	KF="-std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin -I.
	    -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1 -U__linux__
	    -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
	    -Wno-discarded-qualifiers -Werror=implicit-function-declaration
	    $SAN -include host/test-fs-prelude.h"
	# 1. shared holds attributed and released for a dead task
	if gcc $KF -c host/test-ext4-rwsem.c -o "$W/rwsem.o" > "$W/locks-rwsem.out" 2>&1 &&
	   gcc $SAN -o "$W/test-ext4-rwsem" "$W/rwsem.o" \
		$(ls "$B"/*.o | grep -v '/ext4_[^/]*\.o$\|/test-fs-main\.o$') >> "$W/locks-rwsem.out" 2>&1 &&
	   capped 60 "$W/test-ext4-rwsem" >> "$W/locks-rwsem.out" 2>&1; then
		ok locks-rwsem
	else
		bad locks-rwsem
	fi
	# 2. user memory is never touched with a filesystem lock held: the
	#    driver with smap_disable() checked, through reads, writes,
	#    appends, directory listings, page cache pressure and stress
	#    (the harness names the driver's own objects ext4_<file>.o)
	local f uok=1
	rm -rf "$W/ext4chk"
	mkdir -p "$W/ext4chk"
	: > "$W/locks-uaccess.out"
	for f in kernel/fs/ext4/*.c; do
		gcc $KF -DTEST_FSTYPE=ext4_fstype -Dsmap_disable=ext4_chk_smap_disable \
			-c "$f" -o "$W/ext4chk/$(basename "$f" .c).o" \
			>> "$W/locks-uaccess.out" 2>&1 || uok=0
	done
	if [ "$uok" = 1 ] &&
	   gcc $KF -c host/test-ext4-uaccess.c -o "$W/uchk.o" >> "$W/locks-uaccess.out" 2>&1 &&
	   gcc $SAN -o "$W/test-ext4-uchk" "$W"/ext4chk/*.o "$W/uchk.o" \
		$(ls "$B"/*.o | grep -v '/ext4_[^/]*\.o$') >> "$W/locks-uaccess.out" 2>&1; then
		local img c
		img=$(mkimg 1024-64csum)
		c=$(copy "$img" locks-uaccess)
		if capped 600 "$W/test-ext4-uchk" "$c" -c "mount; readall; tree /; write /u pat:1:300000; append /u pat:2:70000; sha /u; pread /u 1000 9000; fopen 0 /u w+a; fwrite 0 text:tail; fpread 0 0 64; fclose 0; mkdir /ud; write /ud/a text:x; ls /ud; shrink 1000; readall; stress 3 300 100; umount" >> "$W/locks-uaccess.out" 2>&1; then
			ok locks-uaccess
		else
			bad locks-uaccess
		fi
		rm -f "$c"
	else
		bad locks-uaccess
	fi
}

# ---- several handles of one file -----------------------------------------
t_handles() {
	local img c
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" handles)
	if run handles "$c" -c "mount
		fopen 0 /ap wca
		fopen 1 /ap wa
		fwrite 0 text:AAAA
		fwrite 1 text:BBBB
		fwrite 0 text:CC
		fclose 0
		fclose 1
		check /ap text:AAAABBBBCC
		fopen 2 /grow +c
		fopen 3 /grow r
		fwrite 2 text:hello-world
		fread 3 64
		fseek 3 0 2
		fopen 4 /grow w
		fpwrite 4 0 text:J
		fclose 4
		fstat 2
		check /grow text:Jello-world
		fclose 2
		fclose 3
		fopen 5 /sz +c
		fopen 6 /sz w
		fwrite 5 text:0123456789
		fpwrite 6 2 text:xy
		fpread 5 0 64
		fclose 6
		fclose 5
		check /sz text:01xy456789
		cycle
		check /sz text:01xy456789
		check /ap text:AAAABBBBCC
		umount" &&
	   grep -q '^fread 11 ' "$W/handles.out" &&
	   grep -q '^fseek 11' "$W/handles.out" &&
	   grep -q '^fread 10 ' "$W/handles.out" &&
	   fsck_clean handles "$c"; then
		ok handles
	else
		bad handles
	fi
	rm -f "$c"
}

# ---- regressions -------------------------------------------------------------
t_regress() {
	local img c
	# 1 KB blocks: a truncate that ends the file inside a page must drop
	# the cached bytes of the blocks it freed, or a write further out
	# leaves them readable inside the new hole (stress seed 12 hits it at
	# byte 141312).
	img=$(mkimg 1024-64csum)
	c=$(copy "$img" regress-trunc-tail)
	if run regress-trunc-tail "$c" -c "mount
		write /t pat:5:20000
		sha /t
		truncate /t 9300
		pwrite /t 14000 text:end
		zeros /t 9300 4700
		cycle
		zeros /t 9300 4700
		stress 12 400
		umount" && fsck_clean regress-trunc-tail "$c"; then
		ok regress-trunc-tail
	else
		bad regress-trunc-tail
	fi
	rm -f "$c"

	# rename: onto another name of the same file does nothing (both
	# names stay, POSIX); a directory replaces an empty directory (not
	# EEXIST), never a non-empty one, a file or a directory by the other
	# kind
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" regress-rename)
	if run regress-rename "$c" -c "mount
		write /a text:x
		link /a /b
		mv /a /b
		stat /a
		mkdir /d1
		mkdir /d1/sub
		write /d1/sub/f text:y
		mkdir /d2
		mv /d1 /d2
		check /d2/sub/f text:y
		=NOT_FOUND stat /d1
		mkdir /d3
		=NOTEMPTY mv /d3 /d2
		=ISDIR mv /b /d3
		=NOTDIR mv /d3 /b
		mkdir /d2/e
		mkdir /d4
		mv /d4 /d2/e
		cycle
		stat /d2
		stat /b
		umount" &&
	   grep -q '^stat /a .*nlink=2' "$W/regress-rename.out" &&
	   grep -q '^stat /b .*nlink=2' "$W/regress-rename.out" &&
	   grep -q '^stat /d2 .*nlink=4' "$W/regress-rename.out" &&
	   fsck_clean regress-rename "$c"; then
		ok regress-rename
	else
		bad regress-rename
	fi
	rm -f "$c"

	# ext2/ext3 (no extents feature): files and directories get the
	# classic block map (an extent tree there is what every other reader
	# takes for corruption) -- through the double-indirect range
	# on 1 KB blocks, truncated back into the direct blocks, freed
	for v in 1024-ext3 1024-ext2 4096-ext3; do
		img=$(mkimg $v)
		c=$(copy "$img" regress-blockmap-$v)
		if run regress-blockmap-$v "$c" -c "mount
			write /big pat:3:1200000
			mkdir /d
			write /d/f pat:4:300000
			symlink /a-target-long-enough-for-a-block-of-its-own-xxxxxxxxxxxxxxxxxxxx /d/l
			truncate /big 300000
			check /big pat:3:300000
			cycle
			check /big pat:3:300000
			check /d/f pat:4:300000
			readlink /d/l
			truncate /big 5000
			check /big pat:3:5000
			write /big2 pat:5:700000
			rm /d/f
			cycle
			check /big2 pat:5:700000
			umount" && fsck_clean regress-blockmap-$v "$c" &&
		   ! debugfs -R "stat /big2" "$c" 2>/dev/null | grep -q 'EXTENTS'; then
			ok regress-blockmap-$v
		else
			bad regress-blockmap-$v
			tail -5 "$W/regress-blockmap-$v.fsck" 2>/dev/null | sed 's/^/    /'
		fi
		rm -f "$c"
	done

	# a file an older driver mapped classically on a filesystem that has
	# extents now (tune2fs -O extents): it keeps its block map when it
	# grows or shrinks (rewriting i_block as an extent root would lose
	# every block it has); a new file gets an extent tree
	img=$(mkimg 1024-ext3)
	c=$(copy "$img" regress-legacy-map)
	if run regress-legacy-map-1 "$c" -c "mount; write /old pat:5:400000; write /old2 pat:6:5000; umount" &&
	   tune2fs -O extents "$c" > /dev/null 2>&1 &&
	   run regress-legacy-map "$c" -c "mount
		check /old pat:5:400000
		truncate /old 200000
		check /old pat:5:200000
		write /ref pat:9:70000
		sha /ref
		append /old pat:9:70000
		pread /old 200000 70000
		write /new pat:7:300000
		rm /old2
		cycle
		=NOT_FOUND stat /old2
		check /new pat:7:300000
		umount"; then
		ref=$(awk '$1 == "sha" {print $2}' "$W/regress-legacy-map.out" | head -1)
	else
		ref=""
	fi
	if [ -n "$ref" ] &&
	   grep -q "^pread 70000 $ref" "$W/regress-legacy-map.out" &&
	   fsck_clean regress-legacy-map "$c" &&
	   ! debugfs -R "stat /old" "$c" 2>/dev/null | grep -q 'EXTENTS' &&
	   debugfs -R "stat /new" "$c" 2>/dev/null | grep -q 'EXTENTS'; then
		ok regress-legacy-map
	else
		bad regress-legacy-map
	fi
	rm -f "$c"

	# group descriptors with the older (gdt_csum / uninit_bg) checksum:
	# kept valid, the UNINIT flags and bg_itable_unused honoured (stale
	# ones make the filesystem unmountable elsewhere and e2fsck drops the
	# new inodes)
	img=$(mkimg 1024-uninit)
	c=$(copy "$img" regress-gdt-csum)
	if run regress-gdt-csum "$c" -c "mount; stress 5 600 150; umount" &&
	   fsck_clean regress-gdt-csum "$c"; then
		ok regress-gdt-csum
	else
		bad regress-gdt-csum
	fi
	rm -f "$c"

	# inline_data (small files inside the inode) is not supported: the
	# mount is refused (rather than reading those files as I/O errors);
	# read-only-compatible features writes would not keep
	# (quota here) make the mount read-only: every change refused, and
	# nothing at all written (write trap)
	rm -rf "$W/src"
	python3 host/fsimg/mktree.py tree basic 1 "$W/src" > /dev/null
	mkfs.ext4 -q -F -O inline_data -E root_owner=0:0 -d "$W/src" "$W/regress-features.img" 64M > /dev/null 2>&1
	if run regress-inline-data "$W/regress-features.img" --cow -c "=UNSUPPORTED mount ro" &&
	   grep -q 'unsupported incompat features 0x8000' "$W/regress-inline-data.out"; then
		ok regress-inline-data
	else
		bad regress-inline-data
	fi
	mkfs.ext4 -q -F -O quota -E root_owner=0:0 -d "$W/src" "$W/regress-features.img" 64M > /dev/null 2>&1
	if run regress-rocompat "$W/regress-features.img" --cow --trap -c "mount rw
		readall
		=ROFS write /new text:x
		=ROFS write /dir1/sub0/f00.dat text:x
		=ROFS mkdir /nd
		=ROFS rmdir /dir1/sub0
		=ROFS rm /dir1/sub0/f00.dat
		=ROFS mv /dir1 /dir9
		=ROFS link /dir1/sub0/f00.dat /l
		=ROFS symlink x /s
		=ROFS chmod /dir1 700
		=ROFS truncate /dir1/sub0/f00.dat 1
		=ROFS setxattr /dir1 user.a text:b
		=ROFS remount rw
		sync
		umount"; then
		ok regress-rocompat
	else
		bad regress-rocompat
	fi
	rm -f "$W/regress-features.img"

	# link counts are 16 bits: a file takes at most 65000 links, and so
	# does a directory without dir_nlink (ext3); with dir_nlink (ext4) a
	# directory past 65000 counts 1.  None of them wraps to 0.
	img=$(mkimg 1024-ext3)
	c=$(copy "$img" regress-nlink-ext3)
	run regress-nlink-ext3-1 "$c" -c "mount; mkdir /p; write /f text:x; umount" &&
	debugfs -w -R "sif /p links_count 65000" "$c" > /dev/null 2>&1 &&
	debugfs -w -R "sif /f links_count 65000" "$c" > /dev/null 2>&1
	if run regress-nlink-ext3 "$c" -c "mount
		=NOSPC mkdir /p/x
		=NOSPC link /f /g
		mkdir /q
		=NOSPC mv /q /p/q
		stat /p
		umount" && grep -q '^stat /p .*nlink=65000' "$W/regress-nlink-ext3.out"; then
		ok regress-nlink-ext3
	else
		bad regress-nlink-ext3
	fi
	rm -f "$c"
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" regress-nlink-ext4)
	run regress-nlink-ext4-1 "$c" -c "mount; mkdir /p; umount" &&
	debugfs -w -R "sif /p links_count 65000" "$c" > /dev/null 2>&1
	if run regress-nlink-ext4 "$c" -c "mount
		mkdir /p/x
		stat /p
		mkdir /p/y
		rmdir /p/x
		cycle
		stat /p
		umount" && [ "$(grep -c '^stat /p .*nlink=1 ' "$W/regress-nlink-ext4.out")" = 2 ]; then
		ok regress-nlink-ext4
	else
		bad regress-nlink-ext4
	fi
	rm -f "$c"

	t_badblk
	t_errors
}

# ---- block numbers from the disk ------------------------------------------
# A damaged block number -- in a block map, an extent, the extended-
# attribute field, a group descriptor, the journal inode, a journal tag, a
# block bitmap -- must never become a write outside the filesystem (the
# runner fails a run on any write past the end of the device) or onto its
# metadata: the mount is refused (group layout, journal inode), or the
# corruption is reported (errors=remount-ro) and the operation fails.
# Each case damaged one value with debugfs / python on a fresh image.
badblk_case() { # badblk_case NAME IMAGE GREP-PATTERN ARGS...
	local name=$1 c=$2 pat=$3
	shift 3
	if run "$name" "$c" "$@" && grep -q "$pat" "$W/$name.out" &&
	   ! grep -q 'beyond the end\|beyond device end' "$W/$name.out"; then
		ok "$name"
	else
		bad "$name"
	fi
	rm -f "$c"
}

t_badblk() {
	local img c pb bb n

	# the journal inode maps a block past the end (the fuzzer's find:
	# ext3-style block map, i_block[1] of inode 8), or an extent of it
	# does: the mount is refused, read-only too, and nothing is written
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" badblk-journal-map)
	debugfs -w -R "sif <8> block[1] 525072" "$c" > /dev/null 2>&1
	badblk_case badblk-journal-map "$c" 'journal inode 8 block map is corrupt; not mounted' \
		--trap -c "=INVALID mount; =INVALID mount ro"
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" badblk-journal-extent)
	debugfs -w -R "sif <8> block[5] 99999" "$c" > /dev/null 2>&1
	badblk_case badblk-journal-extent "$c" 'journal inode 8 block map is corrupt; not mounted' \
		--trap -c "=INVALID mount ro"
	# the journal superblock says the log is longer than the inode
	img=$(mkimg 1024-ext3)
	c=$(copy "$img" badblk-journal-len)
	pb=$(debugfs -R "bmap <8> 0" "$c" 2>/dev/null)
	python3 - "$c" "$pb" 1024 <<'EOF'
import struct, sys
img, pb, bs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(img, "r+b") as f:
    f.seek(pb * bs + 0x10)           # s_maxlen
    f.write(struct.pack(">I", 1 << 30))
EOF
	badblk_case badblk-journal-len "$c" 'do not fit the journal inode' \
		--trap -c "=INVALID mount ro"

	# group descriptors: a bitmap past the end, an inode table on the
	# descriptor table
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" badblk-gd-bitmap)
	debugfs -w -R "set_bg 0 block_bitmap 99999" "$c" > /dev/null 2>&1
	badblk_case badblk-gd-bitmap "$c" 'block bitmap of group 0 (block 99999) is not in its group' \
		--trap -c "=INVALID mount ro"
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" badblk-gd-itable)
	debugfs -w -R "set_bg 0 inode_table 1" "$c" > /dev/null 2>&1
	debugfs -w -R "set_bg 0 checksum calc" "$c" > /dev/null 2>&1
	badblk_case badblk-gd-itable "$c" 'inode table of group 0 (block 1) is on the superblock' \
		--trap -c "=INVALID mount ro"

	# a committed journal transaction whose second tag names a block past
	# the end: the replay stops there, writing nothing outside
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" badblk-journal-tag)
	head -c 8192 /dev/zero | tr '\0' 'j' > "$W/badblk-data"
	printf 'jo\njw -b 2900,2901 %s\njc\n' "$W/badblk-data" > "$W/badblk-jw"
	debugfs -w -f "$W/badblk-jw" "$c" > /dev/null 2>&1
	pb=$(debugfs -R "bmap <8> 1" "$c" 2>/dev/null)
	python3 - "$c" "$pb" 4096 <<'EOF'
import struct, sys
img, pb, bs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(img, "r+b") as f:
    f.seek(pb * bs)
    b = bytearray(f.read(bs))
    i = b.find(struct.pack(">I", 2901), 12)   # the second tag's block
    assert i > 0
    b[i:i + 4] = struct.pack(">I", 99999)
    f.seek(pb * bs)
    f.write(b)
EOF
	# not mounted: neither read-write nor read-only (RECOVER stays set, so
	# the second attempt replays again); norecovery mounts it read-only
	# without writing
	badblk_case badblk-journal-tag "$c" 'names block 99999, beyond the filesystem' \
		-c "=IO mount; =IO mount ro; trap on; =INVALID mount norecovery
		    mount ro norecovery; readall; umount"

	# file block maps: an indirect block past the end, a direct block on
	# the inode table, the xattr block on the inode table (ext3); an
	# extent past the end (ext4).  Reading reports it, the filesystem
	# turns read-only, and nothing reaches those blocks.
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" badblk-map-ext3)
	run badblk-map-ext3-1 "$c" -c "mount; write /f pat:3:200000; write /g pat:4:30000; setxattr /g user.big pat:5:3000; write /h pat:6:30000; umount"
	debugfs -w -R "sif /f block[IND] 99999999" "$c" > /dev/null 2>&1
	debugfs -w -R "sif /h block[2] 8" "$c" > /dev/null 2>&1
	debugfs -w -R "sif /g file_acl 9" "$c" > /dev/null 2>&1
	badblk_case badblk-map-ext3 "$c" 'ERROR: invalid block number' \
		--allow-warn -c "mount
		?pread /f 60000 100
		?pread /h 8192 100
		?getxattr /g user.big
		?append /f pat:4:70000
		?setxattr /g user.c text:y
		?rm /f
		?rm /g
		=ROFS write /new text:x
		umount"
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" badblk-map-extent)
	run badblk-map-extent-1 "$c" -c "mount; write /f pat:3:200000; umount"
	debugfs -w -R "sif /f block[5] 99999" "$c" > /dev/null 2>&1
	badblk_case badblk-map-extent "$c" 'extent: block 99999' \
		--allow-warn -c "mount; ?pread /f 8192 100; ?append /f pat:4:9000; ?truncate /f 0; =ROFS write /new text:x; umount"

	# block bitmaps: the padding past the last block of the (only) group
	# shown free must not let an allocation past the end; a metadata block
	# shown free is reported, not handed out
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" badblk-bitmap-pad)
	bb=$(dumpe2fs "$c" 2>/dev/null | sed -n 's/^ *Block bitmap at \([0-9]*\).*/\1/p' | head -1)
	n=$(dumpe2fs -h "$c" 2>/dev/null | sed -n 's/^Block count: *//p')
	dd if=/dev/zero of="$c" bs=1 seek=$((bb * 4096 + n / 8)) count=$((4096 - n / 8)) conv=notrunc 2> /dev/null
	badblk_case badblk-bitmap-pad "$c" 'test-fs: ok' \
		--allow-warn -c "mount; ?write /big pat:1:80000000; ?rm /big; umount"
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" badblk-bitmap-meta)
	bb=$(dumpe2fs "$c" 2>/dev/null | sed -n 's/^ *Block bitmap at \([0-9]*\).*/\1/p' | head -1)
	printf '\001' | dd of="$c" bs=1 seek=$((bb * 4096)) count=1 conv=notrunc 2> /dev/null
	badblk_case badblk-bitmap-meta "$c" 'block bitmap marks metadata free' \
		--allow-warn -c "mount; ?write /new pat:1:70000; =ROFS mkdir /d; umount"
}

# ---- errors found on the filesystem ------------------------------------------
# What follows an error (errors=remount-ro, the default): the journal is
# aborted -- the batch not yet committed, which may hold half of the
# operation that met the damage, never reaches the log, while a transaction
# the log already holds stays there and the next mount replays it; later
# writes get EROFS, sync and fsync EIO.  A journal whose replay fails
# refuses the mount (norecovery mounts it read-only, unreplayed, writing
# nothing).  A damaged block map (pointer, extent, extent block checksum)
# fails the read with EIO -- it never reads as a hole -- also when read
# again.  The error messages are rate limited, and the superblock is written
# once when the first error is recorded (the details of later ones at
# unmount), not once per block read.

# The span of the journal of image $1 (first to last block, with the
# pointer blocks of an ext3-style journal between them) as "FIRST_SECTOR
# SECTORS" in 512-byte sectors.
jrange() {
	local bs j n l
	bs=$(dumpe2fs -h "$1" 2>/dev/null | sed -n 's/^Block size: *//p')
	j=$(debugfs -R "bmap <8> 0" "$1" 2>/dev/null)
	n=$(( $(debugfs -R "stat <8>" "$1" 2>/dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1) / bs ))
	l=$(debugfs -R "bmap <8> $((n - 1))" "$1" 2>/dev/null)
	[ -n "$j" ] && [ -n "$l" ] && [ "$n" -gt 0 ] && [ "$l" -ge $((j + n - 1)) ] || return 1
	echo "$((j * bs / 512)) $(((l - j + 1) * bs / 512))"
}

# Writes in the dump $1 (from "log on"), counted, up to and including the
# first one of a checkpoint: the first write outside the journal (sectors
# $2, $3 sectors) after writes into it.
ckpt_nth() {
	awk -v j="$2" -v n="$3" '$2 == "W" { c++; if ($3 >= j && $3 < j + n) seen = 1; else if (seen) { print c; exit } }' "$1"
}

t_errors() {
	local img c v jr k ind n

	# -- the journal aborted: nothing of the batch reaches the log -------
	for v in 4096-ext3 4096-64csum; do
		img=$(mkimg $v) || { bad errors-abort-img-$v; continue; }
		c=$(copy "$img" errors-abort-$v)
		run errors-abort-$v-1 "$c" -c "mount; write /f pat:3:200000; umount"
		if [ $v = 4096-ext3 ]; then
			debugfs -w -R "sif /f block[IND] 99999999" "$c" > /dev/null 2>&1
		else
			debugfs -w -R "sif /f block[5] 99999" "$c" > /dev/null 2>&1
		fi
		jr=$(jrange "$c") || { bad errors-abort-$v; rm -f "$c"; continue; }
		# the append meets the damage half way into the operation;
		# whatever it and the operations before it changed is only in
		# the batch -- sync and unmount must not log it
		if run errors-abort-$v "$c" -c "mount
			mkdir /d1
			write /d1/x text:hello
			log on
			=IO append /f pat:4:70000
			=ROFS mkdir /d2
			=ROFS write /d1/y text:x
			?sync
			=IO fsync /d1/x
			umount
			logcount W $jr" &&
		   grep -q "^logcount W $jr 0\$" "$W/errors-abort-$v.out" &&
		   grep -q 'journal aborted' "$W/errors-abort-$v.out" &&
		   ! grep -q 'checkpoint incomplete\|journal commit failed' "$W/errors-abort-$v.out" &&
		   run errors-abort-$v-2 "$c" -c "mount; =NOT_FOUND stat /d1; =NOT_FOUND stat /d2; umount" &&
		   ! grep -q 'replaying journal' "$W/errors-abort-$v-2.out"; then
			ok errors-abort-$v
		else
			bad errors-abort-$v
		fi
		rm -f "$c"
	done

	# -- a transaction already in the log stays there for the replay -----
	# The checkpoint of the first sync fails (a device write error), so
	# the committed transaction is all there is of some of its blocks;
	# the error then aborts the journal, and the next mount replays the
	# transaction: e2fsck finds the result clean (the damaged pointer put
	# back first).
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" errors-replay)
	run errors-replay-1 "$c" -c "mount; write /f pat:3:200000; umount"
	ind=$(debugfs -R "stat /f" "$c" 2>/dev/null | sed -n 's/.*(IND):\([0-9]*\).*/\1/p')
	debugfs -w -R "sif /f block[IND] 99999999" "$c" > /dev/null 2>&1
	jr=$(jrange "$c")
	cp --sparse=always "$c" "$c.probe"
	run errors-replay-probe "$c.probe" -c "mount; mkdir /d1; write /d1/x text:hello; log on; sync; log dump $W/errors-replay.log; umount"
	k=$(ckpt_nth "$W/errors-replay.log" $jr)
	rm -f "$c.probe"
	if [ -n "$ind" ] && [ -n "$k" ] &&
	   run errors-replay "$c" --allow-warn -c "mount
		mkdir /d1
		write /d1/x text:hello
		fault write $k
		?sync
		fault write 0
		=IO append /f pat:4:70000
		=ROFS mkdir /d2
		?sync
		=IO fsync /d1/x
		umount" &&
	   grep -q 'checkpoint incomplete' "$W/errors-replay.out" &&
	   grep -q 'journal aborted' "$W/errors-replay.out" &&
	   run errors-replay-2 "$c" -c "mount; stat /d1; check /d1/x text:hello; =NOT_FOUND stat /d2; umount" &&
	   grep -q 'journal replay complete' "$W/errors-replay-2.out" &&
	   debugfs -w -R "sif /f block[IND] $ind" "$c" > /dev/null 2>&1 &&
	   fsck_clean errors-replay "$c"; then
		ok errors-replay
	else
		bad errors-replay
		[ -f "$W/errors-replay.fsck" ] && tail -5 "$W/errors-replay.fsck" | sed 's/^/    /'
	fi
	rm -f "$c"

	# -- a commit that fails keeps the log, and fsync says so -------------
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" errors-fsync)
	jr=$(jrange "$c")
	cp --sparse=always "$c" "$c.probe"
	run errors-fsync-probe "$c.probe" -c "mount; mkdir /d1; write /d1/x text:hello; log on; fsync /d1/x; log dump $W/errors-fsync.log; umount"
	k=$(ckpt_nth "$W/errors-fsync.log" $jr)
	rm -f "$c.probe"
	if [ -n "$k" ] &&
	   run errors-fsync "$c" --allow-warn -c "mount
		mkdir /d1
		write /d1/x text:hello
		fault write $k
		=IO fsync /d1/x
		fault write 0
		fsync /d1/x
		umount" &&
	   fsck_clean errors-fsync "$c" &&
	   run errors-fsync-2 "$c" -c "mount ro; check /d1/x text:hello; umount"; then
		ok errors-fsync
	else
		bad errors-fsync
	fi
	rm -f "$c"

	# -- the replay: refused when it fails, norecovery, write protection --
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" errors-norecovery)
	head -c 8192 /dev/zero | tr '\0' 'j' > "$W/errors-data"
	printf 'jo\njw -b 2900,2901 %s\njc\n' "$W/errors-data" > "$W/errors-jw"
	debugfs -w -f "$W/errors-jw" "$c" > /dev/null 2>&1
	if run errors-norecovery "$c" --trap -c "=INVALID mount norecovery
		mount ro norecovery
		readall
		=ROFS remount rw
		umount" &&
	   grep -q 'journal not replayed (norecovery)' "$W/errors-norecovery.out" &&
	   ! grep -q 'replaying journal' "$W/errors-norecovery.out" &&
	   run errors-norecovery-wp "$c" --ro-dev -c "=ROFS mount; =ROFS mount ro; mount ro norecovery; readall; umount" &&
	   run errors-norecovery-2 "$c" -c "mount; umount" &&
	   grep -q 'journal replay complete (2 block' "$W/errors-norecovery-2.out"; then
		ok errors-norecovery
	else
		bad errors-norecovery
	fi
	rm -f "$c"

	# -- recovery=ask: a journal to replay refuses the mount, writing
	# nothing; a mount without the option replays it ---------------------
	c=$(copy "$img" errors-askreplay)
	debugfs -w -f "$W/errors-jw" "$c" > /dev/null 2>&1
	if run errors-askreplay "$c" --trap -c "=UCLEAN mount recovery=ask
		=UCLEAN mount ro recovery=ask" &&
	   grep -q 'journal needs to be replayed: not mounted' "$W/errors-askreplay.out" &&
	   ! grep -q 'replaying journal' "$W/errors-askreplay.out" &&
	   run errors-askreplay-wp "$c" --ro-dev -c "=ROFS mount recovery=ask" &&
	   run errors-askreplay-2 "$c" -c "mount; umount; mount recovery=ask; umount" &&
	   grep -q 'journal replay complete (2 block' "$W/errors-askreplay-2.out" &&
	   [ "$(grep -c 'replaying journal' "$W/errors-askreplay-2.out")" = 1 ] &&
	   fsck_clean errors-askreplay "$c"; then
		ok errors-askreplay
	else
		bad errors-askreplay
	fi
	rm -f "$c"

	# -- read-only for a read-only compatible feature this driver does not
	# write (here orphan_present): the journal is still replayed ---------
	c=$(copy "$img" errors-roreplay)
	debugfs -w -f "$W/errors-jw" "$c" > /dev/null 2>&1
	python3 - "$c" <<'EOF2'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(1024 + 0x64)
    v = struct.unpack("<I", f.read(4))[0] | 0x10000
    f.seek(1024 + 0x64)
    f.write(struct.pack("<I", v))
EOF2
	if run errors-roreplay "$c" -c "mount
		=ROFS mkdir /x
		readall
		umount
		mount
		umount" &&
	   grep -q '(orphan_present) not supported for writing -> mounting read-only' "$W/errors-roreplay.out" &&
	   grep -q 'orphan_present: the filesystem was not unmounted cleanly' "$W/errors-roreplay.out" &&
	   grep -q 'journal replay complete (2 block' "$W/errors-roreplay.out" &&
	   [ "$(grep -c 'replaying journal' "$W/errors-roreplay.out")" = 1 ]; then
		ok errors-roreplay
	else
		bad errors-roreplay
	fi
	rm -f "$c"

	# -- a damaged block map fails the read, it does not read as zeros ---
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" errors-eio-ext3)
	run errors-eio-ext3-1 "$c" -c "mount; write /f pat:3:200000; write /h pat:6:30000; mkdir /dd; write /dd/x text:hello; umount"
	debugfs -w -R "sif /f block[IND] 99999999" "$c" > /dev/null 2>&1
	debugfs -w -R "sif /h block[2] 8" "$c" > /dev/null 2>&1
	debugfs -w -R "sif /dd block[0] 8" "$c" > /dev/null 2>&1
	if run errors-eio-ext3 "$c" -c "mount
		pread /f 0 100
		=IO pread /f 60000 100
		=IO pread /f 60000 100
		=IO pread /f 199999 1
		=IO pread /h 8192 100
		=IO pread /h 8192 100
		pread /h 4096 100
		fopen 1 /dd r
		=IO freaddir 1
		=IO freaddir 1
		fclose 1
		=IO stat /dd/x
		umount"; then
		ok errors-eio-ext3
	else
		bad errors-eio-ext3
	fi
	rm -f "$c"
	img=$(mkimg 4096-64csum)
	c=$(copy "$img" errors-eio-extent)
	# /sp: eight blocks with holes between them, eight extents: more than
	# the inode holds, so the tree has a leaf block
	run errors-eio-extent-1 "$c" -c "mount; write /f pat:3:200000
		pwrite /sp 0 pat:1:4096; pwrite /sp 8192 pat:2:4096
		pwrite /sp 16384 pat:3:4096; pwrite /sp 24576 pat:4:4096
		pwrite /sp 32768 pat:5:4096; pwrite /sp 40960 pat:6:4096
		pwrite /sp 49152 pat:7:4096; pwrite /sp 57344 pat:8:4096
		umount"
	debugfs -w -R "sif /f block[5] 99999" "$c" > /dev/null 2>&1
	n=$(debugfs -R "stat /sp" "$c" 2>/dev/null | grep -o '(ETB0):[0-9]*' | cut -d: -f2 | tr '\n' ' ')
	if [ -n "$n" ] && python3 - "$c" $n <<'EOF'
import sys
img = sys.argv[1]
with open(img, "r+b") as f:
    for blk in map(int, sys.argv[2:]):
        f.seek(blk * 4096 + 4092)          # the leaf's checksum tail
        b = f.read(4)
        f.seek(blk * 4096 + 4092)
        f.write(bytes(x ^ 0xFF for x in b))
EOF
	   run errors-eio-extent "$c" -c "mount
		=IO pread /f 8192 100
		=IO pread /f 8192 100
		=IO pread /sp 0 100
		=IO pread /sp 40960 100
		=IO pread /sp 40960 100
		umount" &&
	   grep -q 'extent: block 99999' "$W/errors-eio-extent.out" &&
	   grep -q 'extent block checksum mismatch' "$W/errors-eio-extent.out"; then
		ok errors-eio-extent
	else
		bad errors-eio-extent
	fi
	rm -f "$c"

	# -- rate-limited messages, the superblock written once ---------------
	img=$(mkimg 4096-ext3)
	c=$(copy "$img" errors-ratelimit)
	run errors-ratelimit-1 "$c" -c "mount; write /f pat:3:200000; umount"
	debugfs -w -R "sif /f block[IND] 99999999" "$c" > /dev/null 2>&1
	k=""
	for n in $(seq 0 29); do
		k="$k
		=IO pread /f $((49152 + n * 4096)) 10"
	done
	if run errors-ratelimit "$c" -c "mount
		log on
		$k
		logcount W 2 2
		tick 6000
		=IO pread /f 60000 10
		umount
		logcount W 2 2" &&
	   grep -q '^logcount W 2 2 1$' "$W/errors-ratelimit.out" &&
	   grep -q '^logcount W 2 2 2$' "$W/errors-ratelimit.out" &&
	   [ "$(sed '/error message(s) suppressed/q' "$W/errors-ratelimit.out" | grep -c 'ERROR: invalid block number')" -le 10 ] &&
	   grep -q 'error message(s) suppressed' "$W/errors-ratelimit.out" &&
	   dumpe2fs -h "$c" > "$W/errors-ratelimit.sb" 2>/dev/null &&
	   [ "$(sed -n 's/^FS Error count: *//p' "$W/errors-ratelimit.sb")" -ge 31 ] &&
	   grep -q '^First error function: *ext4_' "$W/errors-ratelimit.sb" &&
	   grep -q '^Last error block #: *99999999' "$W/errors-ratelimit.sb" &&
	   grep -q '^Filesystem state: *not clean with errors' "$W/errors-ratelimit.sb"; then
		ok errors-ratelimit
	else
		bad errors-ratelimit
	fi
	rm -f "$c"
}

# ---- stress matrix ------------------------------------------------------------
t_matrix() {
	local img c name ss seed=31
	for v in $VARIANTS; do
		img=$(mkimg $v) || { bad "matrix-img-$v"; continue; }
		for ss in 512 ${v%%-*}; do
			[ "$ss" = 512 ] || [ "${v%%-*}" -le 4096 ] || continue
			seed=$((seed + 1))
			name=matrix-$v-$ss
			c=$(copy "$img" $name)
			if run $name-w "$c" --sector $ss -s host/fsimg/script-write-basic.txt &&
			   fsck_clean $name-w "$c" &&
			   run $name "$c" --sector $ss -c "mount; stressopt budget 6M; stress $seed 1500 250; umount" &&
			   fsck_clean $name "$c" &&
			   run $name-2 "$c" --sector $ss -c "mount; stressopt maxfile 300000; stress $((seed + 100)) 800 0; sync; umount" &&
			   fsck_clean $name-2 "$c"; then
				ok $name
			else
				bad $name
				[ -f "$W/$name.fsck" ] && tail -5 "$W/$name.fsck" | sed 's/^/    /'
			fi
			rm -f "$c"
		done
	done
	# the POSIX script on the variants with a journal
	for v in 4096-64csum 1024-no64nocsum 4096-ext3; do
		img=$(mkimg $v)
		c=$(copy "$img" posix-$v)
		if run posix-$v "$c" -s host/fsimg/script-write-posix.txt && fsck_clean posix-$v "$c"; then
			ok posix-$v
		else
			bad posix-$v
		fi
		rm -f "$c"
	done
}

# ---- power cuts --------------------------------------------------------------
t_powercut() {
	local img c v total n step failed name maxrec
	for v in 1024-64csum 4096-64csum 1024-no64nocsum 4096-ext3; do
		img=$(mkimg $v) || continue
		c=$(copy "$img" pc-count-$v)
		run pc-count-$v "$c" --quiet -c "mount; stress 12 400; umount" || { bad pc-count-$v; continue; }
		total=$(grep -o 'writes=[0-9]*' "$W/pc-count-$v.out" | tail -1 | cut -d= -f2)
		rm -f "$c"
		step=$((total / ${PC_POINTS:-90} + 1))
		failed=0
		maxrec=0
		for n in $(seq 1 $step "$total"); do
			name=pc-$v-$n
			c=$(copy "$img" pc-$v)
			capped 300 "$BIN" "$c" --quiet --allow-warn -c "mount; fault cut $n; ?stress 12 400; ?umount" > /dev/null 2>&1
			cp --sparse=always "$c" "$c.host"
			# our replay
			if ! run pc-ours "$c" -c "mount; readall; umount" || ! fsck_clean pc-ours "$c"; then
				echo "    $v: power cut after $n writes: our replay: $(grep -E 'FAIL|WARNING' "$W/pc-ours.out" | head -2 | tr '\n' ' ') fsck: $(grep -v '^Pass\|^e2fsck\|^/' "$W/pc-ours.fsck" | head -3 | tr '\n' ' ')"
				failed=1
			fi
			r=$(grep -o 'complete ([0-9]* block' "$W/pc-ours.out" | grep -o '[0-9]*')
			[ -n "$r" ] && [ "$r" -gt "$maxrec" ] && maxrec=$r
			# the host's replay of the same log
			e2fsck -E journal_only -y "$c.host" > /dev/null 2>&1
			if ! fsck_clean pc-host "$c.host"; then
				echo "    $v: power cut after $n writes: host replay: $(grep -v '^Pass\|^e2fsck\|^/' "$W/pc-host.fsck" | head -3 | tr '\n' ' ')"
				failed=1
			fi
			rm -f "$c" "$c.host"
		done
		echo "    $v: $total writes, cut every $step, largest replay $maxrec blocks"
		[ $failed = 0 ] && ok powercut-$v || bad powercut-$v
	done
}

# ---- fuzzing -------------------------------------------------------------------
# host/fsimg/fuzz.py with the runner behind a wrapper that bounds its page
# cache (--pages): with AddressSanitizer the default bound (1 GB of pages)
# does not fit the memory cap, and a fuzzed inode may well be terabytes
# long.  Without metadata checksums nothing catches such an i_size, and
# reading every byte of it (all holes) is legitimate but endless: there
# the runs list and stat the tree and make their changes, but do not read
# whole files (readall); with checksums they do.
t_fuzz() {
	local n=${1:-300} img v seed=900 cro crw
	cat > "$W/fuzz-runner" <<RUNNER
#!/bin/sh
img=\$1
shift
exec "$BIN" "\$img" --pages 32768 "\$@"
RUNNER
	chmod +x "$W/fuzz-runner"
	for v in 4096-64csum 1024-no64nocsum 4096-ext3; do
		img=$(mkimg $v) || continue
		seed=$((seed + 7))
		cro="" crw=""
		case $v in
		*64csum) ;;
		*)
			cro="mount ro; tree /; umount"
			crw="mount; tree /; ?mkdir /fz_dir; ?write /fz_dir/f pat:7:70000; ?append /fz_dir/f pat:8:5000; ?rename /fz_dir/f /fz_dir/g; ?unlink /fz_dir/g; ?rmdir /fz_dir; tree /; umount; ?mount ro; tree /; umount"
			;;
		esac
		if capped 7200 python3 host/fsimg/fuzz.py --bin "$W/fuzz-runner" --fstype ext4 \
			--image "$img" --iters "$n" --seed $seed --regress "$W/regress" \
			${cro:+--cmds "$cro"} > "$W/fuzz-ro-$v.out" 2>&1; then
			ok fuzz-ro-$v
		else
			bad fuzz-ro-$v
		fi
		tail -n 2 "$W/fuzz-ro-$v.out" | sed 's/^/    /'
		if capped 7200 python3 host/fsimg/fuzz.py --bin "$W/fuzz-runner" --fstype ext4 \
			--image "$img" --iters "$n" --seed $((seed + 1)) --rw --regress "$W/regress" \
			${crw:+--cmds "$crw"} > "$W/fuzz-rw-$v.out" 2>&1; then
			ok fuzz-rw-$v
		else
			bad fuzz-rw-$v
		fi
		tail -n 2 "$W/fuzz-rw-$v.out" | sed 's/^/    /'
	done
}

# statfs: the total leaves out the filesystem's own structures (what
# mke2fs records as the overhead), available leaves out the blocks reserved
# for root and, with extents, min(blocks / 50, 4096) more.
t_statfs() {
	local v f n=0 cnt res free ov ext want_b want_a out r2
	for v in "4096:-t ext4" "1024:-t ext4" "4096:-t ext3" \
		 "4096:-t ext4 -O meta_bg,^resize_inode" "4096:-t ext4 -O ^flex_bg" \
		 "4096:-t ext4 -O sparse_super2" "1024:-t ext4 -O ^64bit,^metadata_csum"; do
		n=$((n + 1))
		f="$W/statfs-$n.img"
		rm -f "$f"
		truncate -s 600M "$f"
		mkfs.ext4 -q -F -b ${v%%:*} ${v#*:} "$f" > /dev/null 2>&1 || { bad statfs-$n; continue; }
		out=$(dumpe2fs -h "$f" 2>/dev/null)
		cnt=$(echo "$out" | sed -n 's/^Block count: *//p')
		res=$(echo "$out" | sed -n 's/^Reserved block count: *//p')
		free=$(echo "$out" | sed -n 's/^Free blocks: *//p')
		ov=$(echo "$out" | sed -n 's/^Overhead clusters: *//p')
		ext=0; echo "$out" | grep -q '^Filesystem features:.*extent' && ext=1
		[ -n "$ov" ] || { echo "    $v: mke2fs recorded no overhead"; bad statfs-$n; continue; }
		python3 - "$f" <<'EOF2'
import sys
with open(sys.argv[1], "r+b") as f:      # our sum, not mke2fs's figure
    f.seek(1024 + 0x248); f.write(b"\0\0\0\0")
EOF2
		want_b=$((cnt - ov))
		want_a=$((free - res))
		if [ $ext = 1 ]; then
			r2=$((cnt / 50)); [ $r2 -gt 4096 ] && r2=4096
			want_a=$((want_a - r2))
		fi
		[ $want_a -lt 0 ] && want_a=0
		run statfs-$n "$f" -c "mount ro norecovery; statfs; umount"
		if grep -q "blocks=$want_b bfree=$free bavail=$want_a " "$W/statfs-$n.out"; then
			ok "statfs-$n"
		else
			echo "    $v: want blocks=$want_b bfree=$free bavail=$want_a, got: $(grep '^statfs' "$W/statfs-$n.out")"
			bad "statfs-$n"
		fi
		rm -f "$f"
	done
}

case $WHAT in
all)
	t_sector; t_idle; t_journal; t_locks; t_handles; t_regress
	t_matrix; t_powercut; t_statfs; t_fuzz 150 ;;
sector) t_sector ;;
idle) t_idle ;;
journal) t_journal ;;
locks) t_locks ;;
handles) t_handles ;;
regress) t_regress ;;
badblk) t_badblk ;;
errors) t_errors ;;
statfs) t_statfs ;;
matrix) t_matrix ;;
powercut) t_powercut ;;
fuzz) t_fuzz "${2:-300}" ;;
*) echo "usage: host/test-ext4.sh [all|sector|idle|journal|locks|handles|regress|badblk|errors|statfs|matrix|powercut|fuzz [N]]" >&2; exit 2 ;;
esac
echo "test-ext4: $PASS passed, $FAIL failed${FAILS:+ (}${FAILS# }${FAILS:+)}"
[ $FAIL = 0 ]
