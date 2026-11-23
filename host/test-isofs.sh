#!/bin/bash
# Host tests of the iso9660 driver (kernel/fs/isofs.c) on the filesystem
# harness (host/test-fs.sh): the driver compiled with the real page,
# inode and dentry caches, AddressSanitizer and UBSan, against
# file-backed devices of 512-, 2048- and 4096-byte sectors, in write-trap
# mode (any device write fails the run).
#
#   host/test-isofs.sh            everything below
#   host/test-isofs.sh tools      images of xorriso / genisoimage /
#                                 mkzftree, compared with their source
#                                 trees (and isoinfo where it applies)
#   host/test-isofs.sh crafted    images of host/test-isofs.py (multi-
#                                 extent, interleaving, XAR, CE chains,
#                                 relocation, zisofs, 512/1024 blocks...)
#   host/test-isofs.sh checks     targeted scripts (options, links,
#                                 hidden files, devices, EROFS, ...)
#   host/test-isofs.sh fuzz N     N mutated images per seed image
#   host/test-isofs.sh big        a > 4 GB multi-extent file (~9 GB free)
#   host/test-isofs.sh regress    regression cases (directory positions,
#                                 case, damaged SUSP, extent
#                                 bytes, scan budget, shared lock holds,
#                                 interleaved directories)
#
# Every run is bounded (timeout, and a memory-capped scope when systemd
# allows one).  Work files go to $TMPDIR/likeos-test-isofs.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
WHAT=${1:-all}
W=${TMPDIR:-/tmp}/likeos-test-isofs
mkdir -p "$W"
export TESTFS_DIR=${TESTFS_DIR:-$W/tfs}
BIN=$(host/test-fs.sh isofs --build 2>/dev/null | tail -1)
[ -x "$BIN" ] || { echo "test-isofs: the runner does not build" >&2; exit 2; }
PASS=0
FAIL=0
FAILS=""

ok() { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); FAILS="$FAILS $1"; echo "FAIL $1" >&2; }

run() { # run IMAGE SECTOR SCRIPT...
	local img=$1 ss=$2
	shift 2
	# --pages: the cache gives pages back like the kernel's under memory
	# pressure (unbounded, reading the > 4 GB file of "big" fills 2 GB)
	if systemd-run --user --scope -q -p MemoryMax=1M true >/dev/null 2>&1; then
		timeout -k 5 300 systemd-run --user --scope -q -p MemoryMax=2G \
			-p MemorySwapMax=0 -- "$BIN" --trap --quiet --pages 32768 \
			--sector "$ss" "$@" "$img"
	else
		timeout -k 5 300 "$BIN" --trap --quiet --pages 32768 \
			--sector "$ss" "$@" "$img"
	fi
}

# compare NAME IMAGE EXPECTED-MANIFEST [mount options]
compare() {
	local name=$1 img=$2 exp=$3 opts=${4:-}
	for ss in 512 2048 4096; do
		local out="$W/$name.$ss"
		run "$img" "$ss" -c "mount ro $opts; manifest /; umount" \
			> "$out.out" 2> "$out.err"
		local rc=$?
		grep -v '^test-fs:\|^\[k\]' "$out.out" > "$out.man"
		if [ $rc -ne 0 ] || grep -q '^FAIL' "$out.out"; then
			bad "$name/ss=$ss(run rc=$rc)"
			grep '^FAIL' "$out.out" | head -3 >&2
			continue
		fi
		if python3 host/fsimg/mktree.py compare "$exp" "$out.man" \
			> "$out.cmp" 2>&1; then
			ok
		else
			bad "$name/ss=$ss(manifest)"
			head -5 "$out.cmp" >&2
		fi
	done
}

tools() {
	for v in rrj rr deep many hybrid zisofs plain joliet; do
		local img="$W/iso-$v.iso"
		[ -s "$img" ] || bash host/fsimg/mkimg.sh "iso-$v" "$img" \
			>/dev/null 2>&1 || { bad "mkimg-$v"; continue; }
		[ -s "$img.manifest" ] && compare "tool-$v" "$img" "$img.manifest"
		# isoinfo's own view of the Rock Ridge tree
		if command -v isoinfo >/dev/null && [ -s "$img.manifest" ]; then
			python3 host/fsimg/oracle.py manifest iso "$img" \
				> "$W/tool-$v.oracle" 2>/dev/null &&
				compare "oracle-$v" "$img" "$W/tool-$v.oracle"
		fi
	done
	# genisoimage: deep-directory relocation (rr_moved, CL/PL/RE) and
	# zisofs of compressible data through mkzftree
	if command -v genisoimage >/dev/null; then
		local src="$W/geniso-src"
		rm -rf "$src" "$W/geniso-z"
		mkdir -p "$src"
		local d="$src"
		for i in $(seq -w 0 11); do
			d="$d/level$i"
			mkdir -p "$d"
			printf 'level %s\n' "$i" > "$d/file$i.txt"
		done
		yes 'compressible zisofs line of text' | head -c 600000 \
			> "$src/compressible.txt"
		head -c 300000 /dev/zero > "$src/zeros.bin"
		ln -s level00/level01 "$src/link_into"
		# the relocation directory is a real directory and stays visible
		rm -rf "$W/geniso-exp-src"
		cp -a "$src" "$W/geniso-exp-src"
		mkdir -p "$W/geniso-exp-src/rr_moved"
		python3 host/fsimg/mktree.py manifest "$W/geniso-exp-src" \
			> "$W/geniso.exp"
		rm -rf "$W/geniso-exp-src"
		genisoimage -quiet -R -o "$W/geniso-reloc.iso" "$src" 2>/dev/null &&
			compare geniso-reloc "$W/geniso-reloc.iso" "$W/geniso.exp"
		if command -v mkzftree >/dev/null; then
			mkzftree "$src" "$W/geniso-z" &&
				genisoimage -quiet -R -z -o "$W/geniso-z.iso" \
					"$W/geniso-z" 2>/dev/null &&
				compare geniso-zisofs "$W/geniso-z.iso" "$W/geniso.exp"
		fi
	fi
}

crafted() {
	for v in $(python3 host/test-isofs.py --list); do
		rm -rf "$W/cr-$v.stage"
		python3 -I host/test-isofs.py build "$v" "$W/cr-$v.iso" \
			"$W/cr-$v.stage" || { bad "craft-$v"; continue; }
		case $v in # checked by script (regress), not by manifest
		special | damaged | ceflood | bigext) continue ;;
		esac
		python3 host/fsimg/mktree.py manifest "$W/cr-$v.stage" \
			> "$W/cr-$v.exp"
		compare "crafted-$v" "$W/cr-$v.iso" "$W/cr-$v.exp"
	done
}

# script NAME IMAGE SECTOR 'CMDS' EXPECTED-REGEX...
script() {
	local name=$1 img=$2 ss=$3 cmds=$4
	shift 4
	local out="$W/chk-$name"
	run "$img" "$ss" ${EXTRA:-} -c "$cmds" > "$out.out" 2> "$out.err"
	local rc=$?
	if [ $rc -ne 0 ] || grep -q '^FAIL' "$out.out"; then
		bad "check-$name(rc=$rc)"
		grep '^FAIL' "$out.out" | head -5 >&2
		return
	fi
	for re in "$@"; do
		if ! grep -Eq -- "$re" "$out.out"; then
			bad "check-$name(missing: $re)"
			return
		fi
	done
	ok
}

checks() {
	local rr="$W/cr-rr2048.iso" ml="$W/cr-multi.iso"
	[ -s "$rr" ] || crafted >/dev/null 2>&1
	script readonly "$rr" 2048 \
		'mount rw; =ROFS mkdir /x; =ROFS unlink /small.txt; =ROFS rename /small.txt /y; =ROFS rmdir /empty_dir; =ROFS chmod /small.txt 0777; =ROFS write /small.txt text:abc; =ROFS create /new; =ROFS truncate /small.txt 0; =ROFS symlink /a /lnk; =ROFS setxattr /small.txt user.a text:b; options; umount'
	script links "$rr" 2048 \
		'mount ro; readlink /abs_link; readlink /rel_link; readlink /dot_link; readlink /ce_link; lstat /rel_link; sha /rel_link; sha /dot_link; sha /dir_link/target.txt; =NOT_FOUND stat /dangling; lstat /dangling; =INVALID readlink /small.txt; umount' \
		'/dir/sub/target.txt' 'size=18'
	EXTRA=--no-linkhop script nolinkhop "$rr" 2048 \
		'mount ro; sha /abs_link; sha /dot_link; sha /dir_link/target.txt; stat /dir_link; =NOT_FOUND stat /dangling; lstat /dangling; umount' \
		'sha [0-9a-f]+ 11' 'type=d'
	script options "$rr" 512 \
		'mount ro uid=5,gid=6,mode=0600,dmode=0700,check=strict,map=off; options; stat /small.txt; remount ro uid=7; options; =INVALID remount ro norock; =ROFS remount rw; umount; mount ro norock; ls /; umount; mount ro overriderockperms,uid=9,gid=9,mode=0400,dmode=0500; stat /small.txt; stat /dir; umount' \
		'options uid=5,gid=6,mode=0600,dmode=0700,check=strict,map=off' \
		'options uid=7,gid=6' 'mode=0400 nlink=1 uid=9 gid=9' \
		'type=d mode=0500'
	script statfs "$rr" 2048 \
		'mount ro; statfs; umount' '9660'
	script multi "$ml" 4096 \
		'mount ro; pread /unaligned.bin 2990 20; pread /unaligned.bin 7999 2; pread /aligned.bin 12287 2; sha /lba0.bin; sha /lba3.bin; stat /many_ext.bin; umount' \
		'pread 20 ' 'size=81930'
	script nocompress "$W/cr-zisofs.iso" 2048 \
		'mount ro nocompress; stat /z15.txt; umount; mount ro; stat /z15.txt; umount' \
		'size=100000'
	script cruft "$rr" 2048 \
		'mount ro cruft; sha /pages.bin; umount' 'sha [0-9a-f]+ 12305'
	script session "$rr" 2048 \
		'mount ro sbsector=0; ls /dir; umount' 'in_dir.bin'
	script xattr "$rr" 2048 \
		'mount ro; =NODATA getxattr /small.txt user.x; listxattr /small.txt; =NOT_FOUND getxattr /nope user.x; umount'
	local sp="$W/cr-special.iso"
	script special "$sp" 2048 \
		'mount ro; stat /chr; stat /blk; stat /fifo; stat /sock; stat /hidden.txt; sha /hidden.txt; =NOT_FOUND stat /assoc.txt; stat /sparse.bin; =IO sha /sparse.bin; ls /; umount; mount ro unhide; sha /assoc.txt; umount; mount ro hide; =NOT_FOUND stat /hidden.txt; =NOT_FOUND stat /assoc.txt; stat /normal.txt; umount' \
		'stat /chr type=c mode=0640' 'stat /blk type=b mode=0640' \
		'stat /fifo type=p' 'stat /sock type=s' 'size=1048576' \
		'sha [0-9a-f]+ 11 /assoc.txt' 'b /blk rdev=2049 ' 'c /chr rdev=259 '
	# two sessions on one image: the second recorded with absolute
	# block numbers after the first (genisoimage -C), as on a disc
	if command -v genisoimage >/dev/null; then
		local m="$W/ms"
		rm -rf "$m"
		mkdir -p "$m/a" "$m/b"
		echo "session zero" > "$m/a/first.txt"
		echo "session one" > "$m/b/second.txt"
		genisoimage -quiet -R -o "$m/s1.iso" "$m/a" 2>/dev/null
		local s1=$(($(stat -c %s "$m/s1.iso") / 2048))
		local n=$(((s1 + 15) / 16 * 16 + 16))
		genisoimage -quiet -R -C "0,$n" -o "$m/s2.part" "$m/b" 2>/dev/null
		cp "$m/s1.iso" "$m/ms.iso"
		truncate -s $((n * 2048)) "$m/ms.iso"
		cat "$m/s2.part" >> "$m/ms.iso"
		truncate -s $((($(stat -c %s "$m/ms.iso") + 4095) / 4096 * 4096)) \
			"$m/ms.iso"
		script multisession "$m/ms.iso" 2048 \
			"mount ro; cat /first.txt; umount; mount ro session=1; cat /second.txt; options; umount; mount ro sbsector=$n; cat /second.txt; umount; =NOT_FOUND mount ro session=2" \
			'session zero' 'session one' 'options session=1'
	fi
	script dirseek "$W/iso-many.iso" 2048 \
		'mount ro; chunk 512; ls /many; umount' 'file_02999'
}

# > 4 GB file in two extents (xorriso; ~9 GB free needed, kept sparse)
big() {
	local img="$W/iso-big.iso"
	[ -s "$img" ] || bash host/fsimg/mkimg.sh iso-big "$img" \
		>/dev/null 2>&1 || { echo "test-isofs: iso-big skipped" >&2; return; }
	script big "$img" 2048 \
		'mount ro; stat /huge.bin; pread /huge.bin 0 13; pread /huge.bin 4294965240 16; pread /huge.bin 4294967293 6; pread /huge.bin 4613734389 11; umount' \
		'size=4613734400'
	compare big "$img" "$img.manifest"
}

# Regression tests of the driver's edge cases, one per numbered item below.
regress() {
	local C="$W"
	for v in multi plain jcase damaged ceflood bigext ileave; do
		[ -s "$C/cr-$v.iso" ] && continue
		rm -rf "$C/cr-$v.stage"
		python3 -I host/test-isofs.py build "$v" "$C/cr-$v.iso" \
			"$C/cr-$v.stage" || { bad "craft-$v"; return; }
		[ -d "$C/cr-$v.stage" ] &&
			python3 host/fsimg/mktree.py manifest "$C/cr-$v.stage" \
				> "$C/cr-$v.exp"
	done

	# 1. readdir at positions it never handed out (lseek / seekdir to
	#    every byte offset): only real names, nothing cached from
	#    garbage, multi-extent files keep their size.
	local cmds="mount ro; fopen 0 / rd" o
	for o in $(seq 0 2100); do
		cmds="$cmds; fseek 0 $o 0; freaddir 0"
	done
	cmds="$cmds; fclose 0; stat /many_ext.bin; stat /unaligned.bin; manifest /; umount"
	local out="$W/reg-seek"
	run "$C/cr-multi.iso" 2048 -c "$cmds" > "$out.out" 2> "$out.err"
	if [ $? -ne 0 ] || grep -q '^FAIL' "$out.out"; then
		bad "regress-1-seek(run)"
	elif grep -o 'name=[^ ]*' "$out.out" | sed 's/^name=//' | sort -u |
		grep -vxE '\.|\.\.|aligned\.bin|unaligned\.bin|onebyte_ext\.bin|many_ext\.bin|lba0\.bin|lba3\.bin' |
		grep -q .; then
		bad "regress-1-seek(garbage names)"
	elif ! grep -q 'stat /many_ext.bin .*size=81930' "$out.out" ||
		! grep -q 'stat /unaligned.bin .*size=17100' "$out.out"; then
		bad "regress-1-seek(sizes)"
	else
		grep -v '^  ent\|^freaddir\|^stat\|^test-fs' "$out.out" \
			> "$out.man"
		if python3 host/fsimg/mktree.py compare "$C/cr-multi.exp" \
			"$out.man" > "$out.cmp" 2>&1; then ok; else
			bad "regress-1-seek(manifest after)"; fi
	fi

	# 2. names that differ only in case (Joliet): each its own file,
	#    before and after a listing; plain ISO names: case-blind only
	#    as a fallback, and not with check=strict.
	script regress-2-case "$C/cr-jcase.iso" 2048 \
		'mount ro; sha /case.txt; sha /Case.txt; sha /CASE.TXT; =NOT_FOUND stat /cASE.txt; ls /; sha /case.txt; sha /Case.txt; sha /CASE.TXT; =NOT_FOUND stat /cASE.txt; umount' \
		'sha b908e4daaf9d57fe9cb551a689a35c9a9e0fac85fdf11faaa0a1ba0e5efc06fd 6 /case.txt' \
		'sha 84ad50521b3279c2a69e85fc3ff20bee58af2a17a0b491a50d1e06a03f7eb088 14 /Case.txt' \
		'sha 0b09fd53e2f43d52f1db4645f0c2f071174f0ae09995380be965fb7c6efec211 13 /CASE.TXT'
	script regress-2-plain "$C/cr-plain.iso" 2048 \
		'mount ro; stat /README.TXT; ls /; stat /Readme.Txt; stat /readme.txt; umount; mount ro check=strict; stat /readme.txt; =NOT_FOUND stat /README.TXT; umount' \
		'stat /README.TXT type=f' 'stat /Readme.Txt type=f'

	# 3. damaged System Use data: an unreadable CE (NM beyond the
	#    device), a CE that continues into itself (under a symlink), an
	#    NM that announces more than comes.  No negative entries (a
	#    missing name stays EIO), the link is EIO, nothing damaged cached.
	script regress-3-damaged "$C/cr-damaged.iso" 2048 \
		'warnings ok; mount ro; stat /good.txt; =IO stat /nonexistent; fopen 0 / rd; freaddir 0; fclose 0; =IO stat /nonexistent; =IO readlink /loop_link; =IO stat /loop_link; =IO sha /lostname.txt; sha /f0002.dat; stat /unterminated.txt; =IO stat /nonexistent; umount' \
		'name=f0002.dat' 'name=loop_link' 'sha [0-9a-f]+ 5 /f0002.dat'

	# 4. extent arrays held by unused streams are bounded in bytes: 60
	#    files of 4000 extents (13 MB of arrays if all were kept).
	cmds="mount ro; ls /"
	for o in $(seq -f %03g 0 59); do cmds="$cmds; sha /m$o.bin"; done
	cmds="$cmds; umount"
	out="$W/reg-bigext"
	run "$C/cr-bigext.iso" 2048 -c "$cmds" > "$out.out" 2> "$out.err"
	local peak
	peak=$(sed -n 's/.*heap_peak=\([0-9]*\).*/\1/p' "$out.err" "$out.out" | tail -1)
	if [ "$(grep -c '^sha [0-9a-f]* 1 /m' "$out.out")" != 60 ]; then
		bad "regress-4-bigext(reads)"
	elif [ -z "$peak" ] || [ "$peak" -gt 8000000 ]; then
		bad "regress-4-bigext(heap peak ${peak:-?})"
	else
		ok
	fi

	# 5. a directory whose every record walks a 30-area CE chain: a
	#    lookup that misses ends in EIO within the scan budget (and the
	#    whole run within its time limit) instead of grinding for ever.
	script regress-5-ceflood "$C/cr-ceflood.iso" 2048 \
		'warnings ok; mount ro; stat /f00000; stat /f00100; =IO stat /missing; =IO stat /missing2; umount' \
		'stat /f00100 type=f'

	# 6. a task that dies holding the metadata lock shared: its holds
	#    are given back by release_locks_for_task (unit test).
	local B
	B=$(dirname "$BIN")
	local SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
	if gcc -std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc \
		-fno-builtin -I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ \
		-DCRASH_VERBOSE=1 -DDEBUG=1 -U__linux__ -Wall -Wextra \
		-Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
		-Wno-discarded-qualifiers -Werror=implicit-function-declaration \
		$SAN -include host/test-fs-prelude.h \
		-c host/test-isofs-rwsem.c -o "$W/rwsem.o" &&
		gcc $SAN -o "$W/test-isofs-rwsem" "$W/rwsem.o" \
			$(ls "$B"/*.o | grep -v '/isofs\.o$\|/test-fs-main\.o$') &&
		timeout 60 "$W/test-isofs-rwsem" > "$W/rwsem.out" 2>&1; then
		ok
	else
		bad "regress-6-rwsem"
		cat "$W/rwsem.out" >&2
	fi

	# 7. an interleaved directory with another directory in its gaps:
	#    every file's st_ino is its record's real place on the volume
	#    (unique), and the data is right.
	out="$W/reg-ileave"
	run "$C/cr-ileave.iso" 2048 -c 'mount ro; manifest / +ino -sha; umount' \
		> "$out.out" 2> "$out.err"
	if python3 - "$C/cr-ileave.iso.ino" "$out.out" <<'PYEOF'
import sys
exp = {}
for l in open(sys.argv[1]):
    p, i = l.rsplit(" ", 1)
    exp[p] = int(i)
got = {}
for l in open(sys.argv[2]):
    if l.startswith("f "):
        got[l.split()[1]] = int(l.split("ino=")[1].split()[0])
bad = [p for p in exp if got.get(p) != exp[p]]
sys.exit(1 if bad or len(set(got.values())) != len(got) else 0)
PYEOF
	then ok; else bad "regress-7-ileave(st_ino)"; fi
	compare regress-7-ileave-data "$C/cr-ileave.iso" "$C/cr-ileave.exp"
}

fuzz() {
	local n=${1:-200}
	[ -s "$W/cr-rr2048.iso" ] || crafted >/dev/null 2>&1
	[ -s "$W/iso-rrj.iso" ] || tools >/dev/null 2>&1
	for seed in cr-rr2048 cr-zisofs cr-multi cr-reloc cr-ce cr-joliet iso-rrj; do
		[ -s "$W/$seed.iso" ] || continue
		if TESTFS_FUZZ_TIMEOUT=3600 host/test-fs.sh isofs --fuzz iso \
			"$W/$seed.iso" --iters "$n" --sector 2048 \
			> "$W/fuzz-$seed.log" 2>&1; then
			ok
		else
			bad "fuzz-$seed"
			tail -5 "$W/fuzz-$seed.log" >&2
		fi
	done
}

case $WHAT in
tools) tools ;;
big) big ;;
regress) regress ;;
crafted) crafted ;;
checks) checks ;;
fuzz) fuzz "${2:-200}" ;;
all)
	tools
	crafted
	checks
	regress
	;;
*)
	echo "usage: host/test-isofs.sh [tools|crafted|checks|fuzz N|all]" >&2
	exit 2
	;;
esac
echo "test-isofs: $PASS passed, $FAIL failed${FAILS:+:$FAILS}"
[ $FAIL -eq 0 ]
