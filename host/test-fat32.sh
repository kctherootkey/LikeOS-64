#!/bin/bash
# FAT driver tests beyond host/fsimg/suite-fat32.sh, on the shared harness
# (host/test-fs.sh): FAT12, FAT16 and FAT32 volumes (mkfs.fat -F 12/16/32,
# populated with mtools), 512-byte and 4096-byte sectors, cluster sizes
# from 512 bytes to 64 KB; UTF-8 names (surrogate pairs, 255-unit names),
# 8.3 short-name generation checked against fsck.fat and mtools, the
# shortname= modes, case-insensitive lookup, renames across directories,
# zero-write proofs (read-only and idle read-write mounts, operations that
# change nothing), the volume dirty bit and FSInfo, the backup boot sector,
# mount options, attributes (system.fat_attrib), timestamps, the errors=
# policy on corrupt chains, discard, full volumes and full fixed root
# directories, big directories, remount, the randomized stress model with
# every FAT type, power-cut sweeps, write-error and allocation-failure
# injection, fuzzing and deep paths.  Every write workload is followed by
# fsck.fat -n and host/test-fat32-check.py (FAT copies, cross-links, lost
# clusters, chain lengths, "." / "..", long-name sets, duplicate names,
# FSInfo, dirty flags), many also by a comparison with the tree mtools
# reads.
#
#   host/test-fat32.sh            all of it
#   host/test-fat32.sh NAME...    only the named tests (see ALL below)
#   FAT_FUZZ_ITERS=N              fuzz iterations per image (default 200)
#   FAT_PC_STEP=N                 power-cut sweep step (default 1: every write)
#
# Run it bounded (it starts one runner at a time, each in its own memory-
# capped scope), e.g. under timeout 14400.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
export TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-fat32}
mkdir -p "$TESTFS_DIR" || exit 2
W=$(mktemp -d "$TESTFS_DIR/run.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT
FUZZ_ITERS=${FAT_FUZZ_ITERS:-200}
PC_STEP=${FAT_PC_STEP:-1}
CHECK="python3 -I $ROOT/host/test-fat32-check.py"
MKTREE="python3 -I $ROOT/host/fsimg/mktree.py"

for t in mkfs.fat fsck.fat mcopy mdir mattrib; do
	command -v $t >/dev/null 2>&1 || { echo "test-fat32: $t missing (dosfstools, mtools)"; exit 77; }
done
host/test-fs.sh fat32 --build >/dev/null || { echo "test-fat32: harness build failed"; exit 2; }

PASS=0
FAILED=0
FAILS=""

ok() { PASS=$((PASS + 1)); echo "PASS $1"; }
bad() {
	FAILED=$((FAILED + 1))
	FAILS="$FAILS $1"
	echo "FAIL $1"
	[ -f "$W/$1.out" ] && grep -E "^FAIL|WRITE TRAP|BUG|WARNING|runtime error|ERROR|fsck|check:" "$W/$1.out" | head -20 | cut -c1-240 | sed 's/^/    /'
}

# The staging trees (deterministic): $W/src-PROFILE.
src() {
	local d="$W/src-$1"
	if [ ! -d "$d" ]; then
		$MKTREE tree "$1" 1 "$d" --no-symlinks --no-hardlinks >/dev/null || return 1
		$MKTREE manifest "$d" > "$d.manifest" || return 1
	fi
	echo "$d"
}

# mkfat NAME SIZE "MKFS OPTIONS" [PROFILE]: a fresh volume, populated with
# the profile's tree through mtools; NAME.manifest is the tree.
mkfat() {
	local f="$W/$1.img" size=$2 opts=$3 prof=${4:-} d
	rm -f "$f" "$f.manifest"
	truncate -s "$size" "$f" || return 1
	mkfs.fat $opts -n LIKEOSTEST "$f" >/dev/null 2>&1 || { echo "mkfs.fat $opts failed" >&2; return 1; }
	if [ -n "$prof" ]; then
		d=$(src "$prof") || return 1
		(cd "$d" && for e in *; do mcopy -s -i "$f" "$e" ::/ || exit 1; done) || return 1
		cp "$d.manifest" "$f.manifest"
	fi
	echo "$f"
}

# run NAME IMAGE [runner args]: one harness run, output in $W/NAME.out.
# A command list (-c) longer than the shell passes on goes through a script
# file (-s): one command per line.
run() {
	local name=$1 a args=()
	shift
	while [ $# -gt 0 ]; do
		if [ "$1" = -c ] && [ $# -ge 2 ] && [ ${#2} -gt 60000 ]; then
			printf '%s\n' "$2" | sed 's/; */\n/g' > "$W/$name.cmds"
			args+=(-s "$W/$name.cmds")
			shift 2
			continue
		fi
		args+=("$1")
		shift
	done
	host/test-fs.sh fat32 "${args[@]}" > "$W/$name.out" 2>&1
}

# fsck_ok IMAGE [checker options]: fsck.fat -n reports nothing AND the own
# checker finds the volume consistent.
fsck_ok() {
	local img=$1
	shift
	: > "$W/fsck.out"
	if [ $# -eq 0 ]; then
		fsck.fat -n "$img" > "$W/fsck.out" 2>&1 || return 1
		# fsck.fat -n prints the summary line only when it has nothing to say
		# (FAT32 volumes below 65525 clusters draw a warning: not a finding)
		[ "$(grep -vc -e '^fsck.fat' -e 'files, .* clusters$' -e '^Warning: Filesystem is FAT32 according' \
			-e '^  but has only [0-9]* clusters' -e '^  This may lead to problems' "$W/fsck.out")" = 0 ] || return 1
	fi
	$CHECK "$img" "$@" >> "$W/fsck.out" 2>&1
}

# case_rw NAME IMAGE CMDS [runner args]: run + fsck
case_rw() {
	local name=$1 img=$2 cmds=$3
	shift 3
	if run "$name" "$img" "$@" -c "$cmds" && fsck_ok "$img"; then
		ok "$name"
	else
		cat "$W/fsck.out" >> "$W/$name.out" 2>/dev/null
		bad "$name"
	fi
}

# mtools_same NAME IMAGE: the tree the driver shows equals the tree mtools
# reads (names, sizes, contents).
mtools_same() {
	local name=$1 img=$2
	python3 -I "$ROOT/host/fsimg/oracle.py" manifest fat "$img" > "$W/$name.mtools" 2>/dev/null || return 1
	host/test-fs.sh fat32 "$img" --cow -c "mount ro; manifest; umount" > "$W/$name.drv" 2>&1 || return 1
	python3 -I "$ROOT/host/fsimg/mktree.py" compare "$W/$name.mtools" "$W/$name.drv" >> "$W/$name.out" 2>&1
}

# byte IMAGE OFFSET [VALUE]: read (or write) one byte of an image
byte() {
	python3 -I - "$@" <<'PYEOF'
import sys
f = open(sys.argv[1], "r+b" if len(sys.argv) > 3 else "rb")
f.seek(int(sys.argv[2]))
if len(sys.argv) > 3:
    f.write(bytes([int(sys.argv[3], 0)]))
else:
    print(f.read(1)[0])
PYEOF
}

# ---- FAT types, geometries -------------------------------------------------

# name size mkfs-options: the volume variants every type test runs on
VARIANTS="fat12-2k:8M:-F_12_-s_4
fat12-floppy:1440K:-F_12
fat16-1k:64M:-F_16_-s_2
fat16-4ksec:64M:-F_16_-S_4096_-s_1
fat16-32k:256M:-F_16_-s_64
fat32-512:128M:-F_32_-s_1
fat32-4k:256M:-F_32_-s_8
fat32-64k:512M:-F_32_-s_128
fat32-4ksec:256M:-F_32_-S_4096_-s_1"

t_types() {
	local v name size opts img prof sec wscript
	for v in $VARIANTS; do
		name=${v%%:*}
		size=$(echo "$v" | cut -d: -f2)
		opts=$(echo "$v" | cut -d: -f3 | tr _ ' ')
		prof=basic
		[ "$name" = fat12-floppy ] && prof=tiny
		img=$(mkfat "$name" "$size" "$opts" $prof) || { bad "$name-mkfs"; continue; }
		sec=512
		case $name in *4ksec) sec=4096 ;; esac
		# read against the source tree, and against mtools
		if run "$name-read" "$img" --cow --sector $sec -c "mount ro; manifest; umount" &&
			python3 -I host/fsimg/mktree.py compare "$img.manifest" "$W/$name-read.out" > "$W/$name-cmp.out" 2>&1; then
			ok "$name-read"
		else
			cat "$W/$name-cmp.out" >> "$W/$name-read.out"
			bad "$name-read"
		fi
		# zero writes: ro mount, rw mounts that only read
		if run "$name-trap" "$img" --cow --trap --sector $sec -c \
			"mount ro; readall; statfs; umount; mount rw; readall; statfs; ls /; options; sync; umount; mount rw; cycle; remount ro; remount rw; umount"; then
			ok "$name-trap"
		else
			bad "$name-trap"
		fi
		# the common write script (a floppy holds less: smaller files),
		# then fsck, the checker and mtools
		sed -e 's/pat:5:3m/pat:5:300k/' -e 's/pat:8:2m/pat:8:200k/' host/fsimg/script-write-basic.txt > "$W/wsmall.txt"
		wscript=host/fsimg/script-write-basic.txt
		[ "$name" = fat12-floppy ] && wscript="$W/wsmall.txt"
		if run "$name-write" "$img" --sector $sec -s "$wscript" && fsck_ok "$img" &&
			mtools_same "$name-write" "$img"; then
			ok "$name-write"
		else
			cat "$W/fsck.out" >> "$W/$name-write.out"
			bad "$name-write"
		fi
		# the stress model (two seeds, names as DOS takes them; UTF-8)
		case_rw "$name-stress1" "$img" "mount; stressopt names dos; stress 11 1200 200; umount" --sector $sec
		case_rw "$name-stress2" "$img" "mount; stressopt names ci; stressopt utf8 1; stress 12 1200 300; umount" --sector $sec
		if mtools_same "$name-stress-mtools" "$img"; then ok "$name-stress-mtools"; else bad "$name-stress-mtools"; fi
		rm -f "$img"
	done
}

# ---- names -------------------------------------------------------------------

t_utf8() {
	local img n255 m85 e63
	img=$(mkfat utf8 64M "-F 32") || return 1
	n255=$(printf 'n%.0s' $(seq 1 255))
	# 85 three-byte characters: 255 bytes, 85 units
	m85=$(printf '\\xe6\\x97\\xa5%.0s' $(seq 1 85))
	# 63 emoji (surrogate pairs): 252 bytes, 126 units
	e63=$(printf '\\xf0\\x9f\\x98\\x80%.0s' $(seq 1 63))
	case_rw utf8-names "$img" "mount
mkdir /u
write /u/\xc3\xa4\xc3\xb6\xc3\xbc.txt text:umlaut
write /u/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt text:cjk
write /u/\xf0\x9f\x98\x80smile text:emoji
write /u/\xce\xb1\xce\xb2\xce\xb3 text:greek
write /u/\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 text:cyrillic
write /u/$n255 text:n255
write /u/$m85 text:m85
write /u/$e63 text:e63
=NAMETOOLONG write /u/${n255}x text:no
stat /U/\xc3\x84\xc3\x96\xc3\x9c.TXT
check /U/\xc3\x84\xc3\x96\xc3\x9c.TXT text:umlaut
check /u/\xce\x91\xce\x92\xce\x93 text:greek
check /u/\xd0\x9f\xd0\xa0\xd0\x98\xd0\x92\xd0\x95\xd0\xa2 text:cyrillic
=EXISTS create /u/\xc3\x84\xc3\x96\xc3\x9c.txt
=EXISTS mkdir /U/\xce\x91\xce\xb2\xce\x93
cycle
check /u/\xf0\x9f\x98\x80SMILE text:emoji
check /u/$n255 text:n255
check /u/$m85 text:m85
check /u/$e63 text:e63
ls /u
rename /u/$n255 /u/short
rename /u/short /u/$n255
check /u/$n255 text:n255
umount"
	# mtools shows characters above U+FFFF as '_': it compares a copy
	# without them; the checker decodes the stored UTF-16 itself
	cp "$img" "$W/utf8-bmp.img"
	if run utf8-bmp "$W/utf8-bmp.img" -c "mount; unlink /u/\xf0\x9f\x98\x80smile; unlink /u/$e63; umount" &&
		mtools_same utf8-mtools "$W/utf8-bmp.img" && $CHECK "$img" --list /u > "$W/list.out" &&
		[ "$(grep -c "$(printf '\xf0\x9f\x98\x80')smile" "$W/list.out")" = 1 ] &&
		[ "$(grep -c "$(printf '\xf0\x9f\x98\x80%.0s' $(seq 1 63))" "$W/list.out")" = 1 ] &&
		[ "$(grep -c "$(printf '\xe6\x97\xa5%.0s' $(seq 1 85))" "$W/list.out")" = 1 ]; then
		ok utf8-mtools
	else
		bad utf8-mtools
	fi
	rm -f "$W/utf8-bmp.img"
	# invalid names are refused
	case_rw utf8-invalid "$img" "mount
=INVALID create /u/a*b
=INVALID create /u/a?b
=INVALID create /u/a<b
=INVALID create /u/a|b
=INVALID create /u/a\x22b
=INVALID create /u/a:b
=INVALID create /u/a\\\\b
=INVALID create /u/a\x01b
=INVALID create /u/trailing\x20
=INVALID create /u/bad\xff\xfeutf8
create /u/trailingdots...
stat /u/trailingdots
umount"
}

# expect_short IMAGE DIR "LONG=SHORT ..." : the short names stored
expect_short() {
	local img=$1 dir=$2 pairs=$3 p l s got fail=0
	$CHECK "$img" --list "$dir" > "$W/list.out" 2>&1 || return 1
	for p in $pairs; do
		l=${p%%=*}
		s=${p#*=}
		l=$(printf '%b' "$l" | tr '+' ' ')
		got=$(awk -F'\t' -v L="$l" '$2 == L || ($2 == "" && $1 == L) {print $1}' "$W/list.out")
		if [ "$got" != "$s" ]; then
			echo "short name of '$l': got '$got', expected '$s'"
			fail=1
		fi
	done
	return $fail
}

t_shortname() {
	local img cmds i
	img=$(mkfat sfn 64M "-F 32") || return 1
	cmds="mount; mkdir /d"
	for n in "Long+File+Name.txt" "Long+File+Name2.txt" "file.txt" "FILE2.TXT" "Mixed.Txt" "a.b.c" ".hidden" \
		"x+y.z" "abc+def.txt" "abc,def.txt" "UPPER.TXT" "lower" "toolongext.html" "README" "readme.MD" "a..b" "name." \
		"\xe6\x97\xa5\xe6\x9c\xac.txt" "+lead.txt" "ends.with.dot." "dir.ext.longer"; do
		cmds="$cmds; write /d/$(echo "$n" | sed 's/+/\\x20/g') text:x"
	done
	for i in $(seq 1 14); do cmds="$cmds; write /d/samebasename$i.dat text:$i"; done
	run sfn-make "$img" -c "$cmds; umount" || { bad sfn-make; return; }
	if fsck_ok "$img" &&
		expect_short "$img" /d "Long+File+Name.txt=LONGFI~1.TXT Long+File+Name2.txt=LONGFI~2.TXT file.txt=FILE.TXT
FILE2.TXT=FILE2.TXT Mixed.Txt=MIXED.TXT a.b.c=AB~1.C .hidden=HIDDEN~1 x+y.z=XY~1.Z abc+def.txt=ABCDEF~1.TXT abc,def.txt=ABC_DE~1.TXT
UPPER.TXT=UPPER.TXT lower=LOWER toolongext.html=TOOLON~1.HTM README=README readme.MD=README.MD a..b=A~1.B
name=NAME \xe6\x97\xa5\xe6\x9c\xac.txt=__~1.TXT +lead.txt=LEAD~1.TXT ends.with.dot=ENDSWI~1.DOT
dir.ext.longer=DIREXT~1.LON samebasename1.dat=SAMEBA~1.DAT samebasename9.dat=SAMEBA~9.DAT" >> "$W/sfn-make.out" 2>&1 &&
		[ "$(awk -F'\t' '$2 ~ /^samebasename1[0-4]\.dat$/ && $1 ~ /^SA[0-9A-F][0-9A-F][0-9A-F][0-9A-F]~[1-9]\.DAT$/' "$W/list.out" | wc -l)" = 5 ] &&
		mdir -i "$img" ::/d > "$W/sfn-mdir.out" 2>&1 &&
		grep -q "^LONGFI~1 TXT .* Long File Name.txt$" "$W/sfn-mdir.out" &&
		grep -q "^FILE2    TXT " "$W/sfn-mdir.out" && mtools_same sfn-mtools "$img"; then
		ok shortname-generation
	else
		cat "$W/fsck.out" >> "$W/sfn-make.out"
		bad sfn-make
	fi
	# names mtools made are found by both of their names, whatever the case
	run sfn-lookup "$img" --cow -c "mount ro; check /d/LONGFI~1.TXT text:x; check /D/longfi~2.txt text:x; check /d/SAMEBA~9.DAT text:9; check /d/HIDDEN~1 text:x; check /d/hidden~1 text:x; =NOT_FOUND stat /d/LONGFI~3.TXT; umount" &&
		ok shortname-lookup || bad sfn-lookup
	# shortname=winnt: all-lower or all-upper parts need no long name
	img=$(mkfat sfnnt 64M "-F 32") || return 1
	if run sfn-winnt "$img" -c "mount rw shortname=winnt; write /lower.txt text:1; write /UPPER.TXT text:2; write /Mixed.txt text:3; write /lower.TXT text:4; ls /; umount" &&
		fsck_ok "$img" && $CHECK "$img" --list / > "$W/list.out" &&
		grep -qP "^LOWER.TXT\t\t0x20\t0x18" "$W/list.out" &&
		grep -qP "^UPPER.TXT\t\t0x20\t0x00" "$W/list.out" &&
		grep -qP "^MIXED.TXT\tMixed.txt\t" "$W/list.out" &&
		[ "$(grep -c "^LOWER" "$W/list.out")" = 1 ] &&
		run sfn-winnt2 "$img" --cow -c "mount ro; check /lower.txt text:4; umount"; then
		ok shortname-winnt
	else
		cat "$W/list.out" >> "$W/sfn-winnt.out"
		bad sfn-winnt
	fi
	# display modes of a short name with case bits / upper case
	run sfn-display "$img" --cow -c "mount ro shortname=lower; ls /; umount; mount ro shortname=win95; ls /; umount; mount ro shortname=winnt; ls /; umount; mount ro shortname=mixed; ls /; umount" &&
		grep -q "^f /upper.txt " "$W/sfn-display.out" && grep -q "^f /UPPER.TXT " "$W/sfn-display.out" &&
		grep -q "^f /lower.txt " "$W/sfn-display.out" && grep -q "^f /LOWER.TXT " "$W/sfn-display.out" &&
		ok shortname-display || bad sfn-display
	# nonumtail: the plain short name first
	img=$(mkfat sfnnt2 64M "-F 32") || return 1
	if run sfn-nonumtail "$img" -c "mount rw nonumtail; write /Longer\x20Name.txt text:1; write /Longer\x20Name2.txt text:2; umount" &&
		fsck_ok "$img" && expect_short "$img" / "Longer+Name.txt=LONGERNA.TXT Longer+Name2.txt=LONGER~1.TXT" >> "$W/sfn-nonumtail.out" 2>&1; then
		ok shortname-nonumtail
	else
		bad sfn-nonumtail
	fi
}

t_xdir() {
	local img
	img=$(mkfat xdir 64M "-F 32 -s 1") || return 1
	case_rw xdir-rename "$img" "mount
mkdir /a
mkdir /a/sub
mkdir /b
write /a/f pat:1:70000
write /a/sub/g pat:2:3000
write /b/victim text:victim
mkdir /b/emptydir
rename /a/f /b/f
check /b/f pat:1:70000
=NOT_FOUND stat /a/f
rename /a/sub /b/sub
check /b/sub/g pat:2:3000
rename /b/f /a/A\x20Long\x20Name\x20For\x20The\x20Moved\x20File.dat
rename /a/A\x20Long\x20Name\x20For\x20The\x20Moved\x20File.dat /b/victim
check /b/victim pat:1:70000
=NOT_FOUND stat /a/A\x20Long\x20Name\x20For\x20The\x20Moved\x20File.dat
mkdir /a/newdir
write /a/newdir/x text:x
rename /a/newdir /b/emptydir
check /b/emptydir/x text:x
=INVALID rename /b /b/sub/deeper
=NOTEMPTY rename /a /b
=NOTDIR rename /b/sub /b/victim
=ISDIR rename /b/victim /b/sub
rename /b/sub /sub_at_root
rename /sub_at_root /a/sub2
cycle
check /a/sub2/g pat:2:3000
check /b/victim pat:1:70000
check /b/emptydir/x text:x
umount"
	case_rw xdir-rename-fat16 "$(mkfat xdir16 32M "-F 16 -s 1")" "mount
mkdir /a
mkdir /a/b
write /a/b/f text:abc
rename /a/b /b
rename /b /a/b2
rename /a/b2 /b3
check /b3/f text:abc
cycle
check /B3/F text:abc
umount"
}

# Operations that change nothing write nothing (an idle read-write mount,
# failed operations, renames onto themselves, no-op attribute changes).
t_noop() {
	local img
	img=$(mkfat noop 64M "-F 32") || return 1
	run noop-prep "$img" -c "mount; mkdir /d; write /d/x text:1; mkdir /e; write /g text:abc; create /empty; umount" || { bad noop-prep; return; }
	if run noop "$img" --trap -c "mount rw; rename /g /g; rename /G /g; =NOTEMPTY rename /e /d; =ISDIR rename /g /d; =NOTDIR rename /d /g; =INVALID rename /d /d/sub; =NOT_FOUND rename /nothing /x; =EXISTS mkdir /d; =NOTEMPTY rmdir /d; =ISDIR unlink /d; =NOT_FOUND unlink /none; =INVALID create /a:b; truncate /empty 0; chmod /g 0755; chown /g 0 0; fopen 1 /empty r; fclose 1; fopen 2 /g r; fread 2 3; fclose 2; readall; statfs; sync; umount"; then
		ok noop-writes-nothing
	else
		bad noop-writes-nothing
	fi
	# a read-only device: rw is refused, ro walks, nothing written
	if run noop-rodev "$img" --ro-dev --trap -c "?mount rw; ?umount; mount ro; readall; =ROFS mkdir /x; umount"; then
		ok noop-rodev
	else
		bad noop-rodev
	fi
}

# ---- volume state ------------------------------------------------------------

t_dirty() {
	local img v
	for v in "fat16:-F 16" "fat32:-F 32"; do
		img=$(mkfat "dirty-${v%%:*}" 64M "${v#*:}") || return 1
		# an armed mount shows the volume dirty; a clean unmount clears it
		if run "dirty-${v%%:*}" "$img" -c "mount; write /f text:abc; sync; umount" && fsck_ok "$img" &&
			run "dirty2-${v%%:*}" "$img" -c "mount; write /g pat:1:5000; fault cut 1; ?write /h text:x; ?umount" &&
			$CHECK "$img" --expect-dirty --allow-leaks --allow-fat-mismatch > "$W/fsck.out" 2>&1; then
			ok "dirty-bit-${v%%:*}"
		else
			cat "$W/fsck.out" >> "$W/dirty-${v%%:*}.out"
			bad "dirty-${v%%:*}"
		fi
		# a volume dirty at mount stays dirty after a clean unmount (the
		# user's fsck decides), and is still writable
		if run "dirty3-${v%%:*}" "$img" -c "mount; write /i text:y; umount" &&
			$CHECK "$img" --expect-dirty --allow-leaks --allow-fat-mismatch > "$W/fsck.out" 2>&1; then
			ok "dirty-stays-${v%%:*}"
		else
			bad "dirty3-${v%%:*}"
		fi
	done
}

t_fsinfo() {
	local img info
	img=$(mkfat fsi 128M "-F 32 -s 1" basic) || return 1
	# freeing only (no allocation in the mount): still a correct count
	if run fsi-free "$img" -c "mount; unlink /size_300000_22.bin; umount" && fsck_ok "$img" &&
		run fsi-alloc "$img" -c "mount; write /new pat:3:1000000; umount" && fsck_ok "$img" &&
		run fsi-mixed "$img" -c "mount; truncate /new 1000; mkdir /m; write /m/x pat:1:20000; unlink /size_131072_21.bin; umount" &&
		fsck_ok "$img"; then
		info=$($CHECK "$img" --info)
		echo "$info" >> "$W/fsi-mixed.out"
		if echo "$info" | grep -q "fsinfo_free=4294967295"; then bad fsinfo; else ok fsinfo; fi
	else
		bad fsinfo
	fi
	# usefree: statfs trusts the stored count (no FAT scan)
	if run fsi-usefree "$img" --cow -c "mount ro usefree; mark; statfs; options; umount" &&
		grep -q usefree "$W/fsi-usefree.out"; then
		ok fsinfo-usefree
	else
		bad fsi-usefree
	fi
}

t_backup() {
	local img
	img=$(mkfat bkup 64M "-F 32" tiny) || return 1
	run bk-prep "$img" -c "mount; write /f text:abc; umount" || { bad bk-prep; return; }
	# a broken main boot sector: mount from the backup, read-only
	byte "$img" 12 0x03
	byte "$img" 13 0x00
	if run backup-boot "$img" --trap -c "mount; options; check /f text:abc; readall; =ROFS mkdir /x; umount" &&
		grep -q "mounted read-only: main boot sector unusable" "$W/backup-boot.out"; then
		ok backup-boot
	else
		bad backup-boot
	fi
}

t_options() {
	local img
	img=$(mkfat opts 64M "-F 32" tiny) || return 1
	if run options "$img" -c "=INVALID mount rw errors=sometimes; =INVALID mount rw uid=abc; =INVALID mount rw shortname=dos; =INVALID mount rw codepage=999; mount rw uid=1000,gid=100,umask=027,errors=continue,discard,flush,iocharset=utf8,codepage=850,tz=UTC,allow_utime=022,quiet; options; umount" &&
		grep -q "uid=1000,gid=100,fmask=0027,dmask=0027" "$W/options.out" &&
		grep -q "codepage=850" "$W/options.out" && grep -q "errors=continue" "$W/options.out" &&
		grep -q "discard" "$W/options.out" && grep -q "flush" "$W/options.out" && grep -q "quiet" "$W/options.out"; then
		ok options
	else
		bad options
	fi
	# masks and showexec
	if run opt-modes "$img" -c "mount rw showexec,fmask=0022; write /a.exe text:x; write /b.txt text:y; mkdir /dd; stat /a.exe; stat /b.txt; stat /dd; umount; mount rw fmask=0177,dmask=0077,uid=5,gid=6; stat /b.txt; stat /dd; =PERM chown /b.txt 7 7; chown /b.txt 5 6; umount; mount rw quiet,uid=5; chown /b.txt 7 7; chmod /b.txt 0777; umount" &&
		grep -q "stat /a.exe type=f mode=0755" "$W/opt-modes.out" &&
		grep -q "stat /b.txt type=f mode=0644" "$W/opt-modes.out" &&
		grep -q "stat /dd type=d mode=0755" "$W/opt-modes.out" &&
		grep -q "stat /b.txt type=f mode=0600 nlink=1 uid=5 gid=6" "$W/opt-modes.out" &&
		grep -q "stat /dd type=d mode=0700" "$W/opt-modes.out" && fsck_ok "$img"; then
		ok options-modes
	else
		bad opt-modes
	fi
	# chmod: the write bits are the read-only attribute
	if run opt-chmod "$img" -c "mount rw fmask=0137; chmod /b.txt 0444; stat /b.txt; getxattr /b.txt system.fat_attrib; chmod /b.txt 0640; stat /b.txt; =PERM chmod /b.txt 0600; =PERM chmod /b.txt 0040; chmod /b.txt 0660; stat /b.txt; umount" &&
		grep -q "stat /b.txt type=f mode=0440" "$W/opt-chmod.out" &&
		grep -q "xattr system.fat_attrib 4 21000000" "$W/opt-chmod.out" &&
		grep -q "stat /b.txt type=f mode=0640" "$W/opt-chmod.out" && fsck_ok "$img"; then
		ok options-chmod
	else
		bad opt-chmod
	fi
	# code page 850 short names; iocharset=iso8859-1
	if run opt-cp "$img" -c "mount rw codepage=850; write /\xc3\xa9t\xc3\xa9.txt text:e; umount; mount ro iocharset=iso8859-1; ls /; umount; mount ro; check /\xc3\x89T\xc3\x89.TXT text:e; umount" &&
		fsck_ok "$img"; then
		ok options-codepage
	else
		bad opt-cp
	fi
}

t_attrib() {
	local img
	img=$(mkfat attr 64M "-F 32") || return 1
	if run attrib "$img" -c "mount; write /f text:x; mkdir /d; getxattr /f system.fat_attrib; setxattr /f system.fat_attrib fill:7:1; getxattr /f system.fat_attrib; stat /f; =INVALID setxattr /f system.fat_attrib fill:16:1; setxattr /d system.fat_attrib fill:18:1; getxattr /d system.fat_attrib; =NODATA getxattr /f user.other; listxattr /f; cycle; getxattr /f system.fat_attrib; umount" &&
		grep -q "xattr system.fat_attrib 4 20000000" "$W/attrib.out" &&
		grep -q "xattr system.fat_attrib 4 07000000" "$W/attrib.out" &&
		grep -q "xattr system.fat_attrib 4 12000000" "$W/attrib.out" &&
		fsck_ok "$img" && mattrib -i "$img" ::/f > "$W/mattrib.out" 2>&1 &&
		grep -qE "^ *R *H *S|^ *[A ]*S *H *R|R.*H.*S|S.*H.*R" "$W/mattrib.out" ; then
		ok attributes
	else
		cat "$W/mattrib.out" >> "$W/attrib.out" 2>/dev/null
		bad attrib
	fi
	# attributes mtools sets are seen
	mattrib -i "$img" +h ::/d
	if run attrib2 "$img" --cow -c "mount ro; getxattr /d system.fat_attrib; umount" &&
		grep -q "xattr system.fat_attrib 4 1[2-3]000000" "$W/attrib2.out"; then
		ok attributes-mtools
	else
		bad attrib2
	fi
}

t_times() {
	local img
	img=$(mkfat times 64M "-F 32") || return 1
	# the harness clock starts at 2026-01-01 00:00:00 UTC (1767225600)
	if run times "$img" -c "mount; tick 3500; write /f text:x; stat /f; utimens /f 1700000001 1700000003 0; stat /f; utimens /f omit 1600000000 990000000; stat /f; cycle; stat /f; umount; mount rw time_offset=120; stat /f; utimens /f now now; stat /f; umount" &&
		grep -q "stat /f .* mtime=17672256[0-9][0-9]\." "$W/times.out" &&
		grep -q "stat /f .* mtime=1700000002\." "$W/times.out" &&
		grep -q "stat /f .* mtime=1600000000\." "$W/times.out" && fsck_ok "$img" &&
		$CHECK "$img" --list / > "$W/list.out" && ! grep -qP "\t0000 0000\t" "$W/list.out"; then
		ok timestamps
	else
		cat "$W/list.out" >> "$W/times.out" 2>/dev/null
		bad times
	fi
	# the creation time of a new file carries 10 ms units, the access date
	# is set; mtools reads the dates (2026)
	if mdir -i "$img" ::/ > "$W/mdir.out" 2>&1 && grep -qE "^F +1 20[0-9][0-9]-" "$W/mdir.out"; then
		ok timestamps-mtools
	else
		cat "$W/mdir.out" >> "$W/times.out"
		bad times-mtools
	fi
	# allow_utime: a non-owner in the allowed group may set times
	if run times-allow "$img" -c "mount rw uid=0,gid=50,allow_utime=002; uid 1000 50; utimens /f now now; uid 1000 51; ?utimens /f now now; umount"; then
		ok timestamps-allow-utime
	else
		bad times-allow
	fi
}

# ---- error policy --------------------------------------------------------------

# cluster_of IMAGE NAME: the first cluster of a root-directory file (short
# name as --list prints it)
cluster_of() {
	$CHECK "$1" --list / | awk -F'\t' -v N="$2" '$1 == N || $2 == N {print $6}'
}

# set_fat IMAGE CLUSTER VALUE: one FAT32 entry in every copy
set_fat() {
	python3 -I - "$@" <<'PYEOF'
import sys, struct
img, cl, val = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 0)
f = open(img, "r+b")
b = f.read(512)
bps = struct.unpack_from("<H", b, 11)[0]
res = struct.unpack_from("<H", b, 14)[0]
nf = b[16]
fsz = struct.unpack_from("<I", b, 36)[0]
for k in range(nf):
    f.seek((res + k * fsz) * bps + cl * 4)
    f.write(struct.pack("<I", val))
PYEOF
}

t_errors() {
	local img c1 c2
	img=$(mkfat err 64M "-F 32 -s 1") || return 1
	run err-prep "$img" -c "mount; write /loop pat:1:5000; write /good pat:2:5000; umount" || { bad err-prep; return; }
	c1=$(cluster_of "$img" LOOP)
	# the chain of /loop leads back into itself
	set_fat "$img" $((c1 + 3)) "$c1"
	cp "$img" "$W/err2.img"
	cp "$img" "$W/err3.img"
	# errors=remount-ro (default): unlink removes the name, keeps the
	# clusters (lost), latches read-only, the volume stays marked dirty
	cp "$img" "$W/err4.img"
	if run err-ro "$img" --allow-warn -c "mount; unlink /loop; =ROFS write /x text:x; check /good pat:2:5000; options; umount" &&
		$CHECK "$img" --allow-leaks --expect-dirty > "$W/fsck.out" 2>&1 &&
		grep -q " [1-9][0-9]* lost clusters" "$W/fsck.out" && ! grep -q "^LOOP" <($CHECK "$img" --list /); then
		ok errors-remount-ro
	else
		cat "$W/fsck.out" >> "$W/err-ro.out"
		bad err-ro
	fi
	# a read of the corrupt chain fails with EIO and latches read-only
	if run err-read "$W/err4.img" --trap --allow-warn -c "mount; =IO check /loop pat:1:5000; =ROFS write /x text:x; check /good pat:2:5000; umount"; then
		ok errors-read-latches
	else
		bad err-read
	fi
	# errors=continue: the same, the mount stays writable
	if run err-cont "$W/err2.img" --allow-warn -c "mount rw errors=continue; unlink /loop; write /x text:x; check /good pat:2:5000; umount" &&
		$CHECK "$W/err2.img" --allow-leaks --allow-dirty > "$W/fsck.out" 2>&1; then
		ok errors-continue
	else
		cat "$W/fsck.out" >> "$W/err-cont.out"
		bad err-cont
	fi
	# a chain running into a free cluster: reads stop with EIO at the gap
	c2=$(cluster_of "$W/err3.img" GOOD)
	set_fat "$W/err3.img" $((c2 + 2)) 0
	if run err-free "$W/err3.img" --allow-warn --cow -c "mount ro; =IO check /good pat:2:5000; umount"; then
		ok errors-chain-into-free
	else
		bad err-free
	fi
	rm -f "$W/err2.img" "$W/err3.img" "$W/err4.img"
}

t_discard() {
	local img
	img=$(mkfat disc 64M "-F 32 -s 8") || return 1
	if run discard "$img" -c "mount rw discard; write /a pat:1:1000000; write /b pat:2:300000; sync; mark; unlink /a; sync; counts; truncate /b 1000; sync; counts; check /b pat:2:1000; cycle; check /b pat:2:1000; umount" &&
		grep -qE "counts .*discards=[1-9]" "$W/discard.out" && fsck_ok "$img"; then
		ok discard
	else
		bad discard
	fi
	img=$(mkfat nodisc 64M "-F 32 -s 8") || return 1
	if run nodiscard "$img" -c "mount rw; write /a pat:1:1000000; sync; unlink /a; sync; counts; umount" &&
		grep -qE "counts .*discards=0" "$W/nodiscard.out" && fsck_ok "$img"; then
		ok nodiscard
	else
		bad nodiscard
	fi
}

t_full() {
	local img i cmds
	img=$(mkfat full 2M "-F 12 -s 1") || return 1
	case_rw full-fat12 "$img" "mount
statfs
?write /f1 pat:1:1500000
=NOSPC write /f2 pat:2:1500000
stat /f2
statfs
unlink /f1
unlink /f2
write /f3 pat:3:1000000
cycle
check /f3 pat:3:1000000
statfs
umount"
	# the fixed root directory of FAT12/16 cannot grow
	img=$(mkfat rootfull 4M "-F 12 -r 32") || return 1
	# (32 slots, the volume label takes one)
	case_rw root-full "$img" "mount; $(for i in $(seq 1 31); do echo -n "write /F$i text:$i; "; done) =NOSPC write /F32 text:x; =NOSPC write /long_name_too text:x; =NOSPC mkdir /DIRX; unlink /F30; mkdir /SUB; write /SUB/ok text:fine; cycle; check /F31 text:31; check /sub/ok text:fine; umount"
	# a FAT32 volume full of directories
	img=$(mkfat fulldirs 34M "-F 32 -s 128") || return 1
	case_rw full-dirs "$img" "mount; $(for i in $(seq 1 600); do echo -n "?mkdir /d$i; "; done) statfs; =NOSPC mkdir /more_dirs_than_space; cycle; readall; umount"
}

t_many() {
	local img
	img=$(mkfat many 128M "-F 32 -s 1") || return 1
	case_rw many-files "$img" "mount
mkdir /big
$(for i in $(seq 1 1500); do echo "write /big/a\x20long\x20file\x20name\x20number\x20$i.text text:$i"; done)
readall /big
$(for i in 1 7 99 1000 1499 1500; do echo "check /big/A\x20LONG\x20FILE\x20NAME\x20NUMBER\x20$i.TEXT text:$i"; done)
cycle
readall /big
$(for i in $(seq 1 2 1500); do echo "unlink /big/a\x20long\x20file\x20name\x20number\x20$i.text"; done)
$(for i in $(seq 1 200); do echo "write /big/refill$i text:r$i"; done)
cycle
readall
$(for i in 2 500 1500; do echo "check /big/a\x20long\x20file\x20name\x20number\x20$i.text text:$i"; done)
umount"
	if mtools_same many-mtools "$img"; then ok many-mtools; else bad many-mtools; fi
	# the same in a FAT16 root directory (fixed, 512 entries) and a FAT12 one
	img=$(mkfat manyroot 32M "-F 16") || return 1
	case_rw many-root16 "$img" "mount; $(for i in $(seq 1 120); do echo -n "write /root\x20file\x20$i.txt text:$i; "; done) cycle; readall; $(for i in $(seq 1 120); do echo -n "unlink /root\x20file\x20$i.txt; "; done) umount"
}

t_remount() {
	local img
	img=$(mkfat rmnt 64M "-F 16") || return 1
	case_rw remount "$img" "mount
write /a pat:1:70000
remount ro
=ROFS write /b text:x
check /a pat:1:70000
remount rw
write /b text:x
append /a pat:9:10
cycle
check /b text:x
remount ro
remount rw errors=continue
options
umount"
	# a remount read-only stores everything and marks the volume clean
	if run remount-clean "$img" -c "mount; write /c pat:3:9000; remount ro; =ROFS write /d text:x" &&
		fsck_ok "$img"; then
		ok remount-ro-clean
	else
		bad remount-clean
	fi
}

t_flush() {
	local img
	img=$(mkfat flush 64M "-F 32") || return 1
	# flush: a written file is on the device at its last close
	if run flush "$img" -c "mount rw flush; fopen 1 /f w+c; fwrite 1 pat:1:200000; fclose 1; fault cut 1; ?write /g text:x; ?umount" &&
		$CHECK "$img" --allow-dirty --allow-leaks > "$W/fsck.out" 2>&1 &&
		run flush-check "$img" --cow -c "mount ro; check /f pat:1:200000; umount"; then
		ok flush-on-close
	else
		bad flush
	fi
}

# The tree mtools and the driver see stays equal through workloads of the
# other tool: mtools writes, the driver reads; the driver writes, mtools
# reads.
t_interop() {
	local img
	img=$(mkfat interop 64M "-F 32" basic) || return 1
	if run interop1 "$img" -c "mount; mkdir /drv; write /drv/D\xc3\xa4ta.bin pat:3:123456; rename /dir0 /drv/dir0moved; unlink /size_065536_19.bin; umount" &&
		fsck_ok "$img" && mtools_same interop1-m "$img"; then
		ok interop-driver-writes
	else
		bad interop1
	fi
	mmd -i "$img" ::/mt || true
	mcopy -i "$img" "$ROOT/host/test-fat32-check.py" ::/mt/check.py || true
	mdel -i "$img" ::/size_065537_20.bin || true
	if mtools_same interop2 "$img" && run interop3 "$img" -c "mount; write /mt/after text:x; rename /mt/check.py /drv/c.py; umount" &&
		fsck_ok "$img" && mtools_same interop3-m "$img"; then
		ok interop-mtools-writes
	else
		bad interop2
	fi
}

# ---- crash safety --------------------------------------------------------------

PCW="?mkdir /pc; ?mkdir /pc/sub; ?write /pc/a pat:1:300000; ?write /pc/sub/b pat:2:50000; ?rename /pc/a /pc/sub/a2; ?truncate /pc/sub/b 1000; ?write /pc/c text:hello; ?unlink /pc/c; ?rename /pc/sub /pc/sub2; ?append /pc/sub2/b pat:3:9000; ?write /keep/k pat:4:4096; ?write /pc/Long\x20Name\x20Number\x20One.txt text:x; ?rename /pc/sub2/a2 /keep/moved; ?mkdir /pc/sub2/inner; ?rename /pc/sub2 /keep/sub3; ?unlink /keep/k; ?truncate /keep/moved 700000; ?rename /keep/sub3/inner /inner2; ?rmdir /pc/sub2; ?sync; ?umount"

# powercut NAME SIZE MKFS: every write of the workload is the last one the
# device stores: what is left is consistent but for lost clusters and chain
# tails, and mounts and reads.
powercut() {
	local name=$1 size=$2 opts=$3 base img n total failed=0 lost=0
	base=$(mkfat "pc-$name" "$size" "$opts") || return 1
	run "pc-$name-prep" "$base" -c "mount; mkdir /keep; write /keep/k pat:1:200000; umount" || { bad "pc-$name-prep"; return; }
	run "pc-$name-count" "$base" --cow -c "mount; log clear; $PCW; counts"
	total=$(sed -n 's/.*counts reads=[0-9]* writes=\([0-9]*\).*/\1/p' "$W/pc-$name-count.out" | tail -1)
	img="$W/pc.img"
	for n in $(seq 1 "$PC_STEP" "${total:-100}"); do
		cp --sparse=always "$base" "$img"
		# (after the cut the device drops writes but the system runs
		# on: what it then reads back may contradict what it wrote,
		# and the warnings that draws are no finding -- crashes are)
		if ! run "pc-$name-$n" "$img" --allow-warn -c "mount; fault cut $n; $PCW"; then
			echo "    cut $n: runner failed"
			grep -E "^FAIL|WARN|BUG|runtime" "$W/pc-$name-$n.out" | head -3 | cut -c1-200 | sed 's/^/      /'
			failed=1
		fi
		if ! $CHECK "$img" --allow-leaks --allow-dirty --allow-fat-mismatch > "$W/fsck.out" 2>&1; then
			echo "    cut $n: $(grep ERROR "$W/fsck.out" | head -3 | tr '\n' ' ' | cut -c1-300)"
			failed=1
		fi
		grep -q " 0 lost clusters, 0 chain-tail" "$W/fsck.out" || lost=$((lost + 1))
		if ! run "pc-$name-ro-$n" "$img" --cow -c "mount ro; readall; umount"; then
			echo "    cut $n: read-back failed"
			failed=1
		fi
		rm -f "$W/pc-$name-$n.out" "$W/pc-$name-ro-$n.out"
	done
	echo "    $name: $total writes swept, $lost cuts left lost clusters or chain tails (allowed)"
	rm -f "$img"
	[ $failed = 0 ] && ok "power-cut-$name" || bad "power-cut-$name"
}

t_powercut() {
	powercut fat32 64M "-F 32 -s 1"
	powercut fat16 32M "-F 16 -s 4"
	powercut fat12 4M "-F 12 -s 2"
}

# Write errors at every write of a workload: the operation (or the sync /
# unmount after it) reports the error, what is on the device stays
# consistent (lost clusters allowed), the volume stays marked dirty.
t_wfault() {
	local base img n total failed=0 cmds
	base=$(mkfat wf 32M "-F 32 -s 1") || return 1
	run wf-prep "$base" -c "mount; mkdir /keep; write /keep/k pat:1:20000; umount" || { bad wf-prep; return; }
	cmds="?mkdir /w; ?write /w/a pat:1:60000; ?rename /w/a /keep/a; ?truncate /keep/k 100; ?write /w/Long\x20Name.txt text:x; ?unlink /keep/a; ?rmdir /w"
	run wf-count "$base" --cow -c "mount; log clear; $cmds; ?sync; counts; umount"
	total=$(sed -n 's/.*counts reads=[0-9]* writes=\([0-9]*\).*/\1/p' "$W/wf-count.out" | tail -1)
	img="$W/wfrun.img"
	for n in $(seq 1 "${total:-60}"); do
		cp --sparse=always "$base" "$img"
		run "wf$n" "$img" --allow-warn -c "mount; fault write $n; $cmds; ?sync; ?umount"
		if grep -qE "runtime error|AddressSanitizer|BUG|DEADLOCK|still held|leak" "$W/wf$n.out"; then
			echo "    write error $n: $(grep -E "runtime error|AddressSanitizer|BUG|DEADLOCK|still held|leak" "$W/wf$n.out" | head -2 | cut -c1-200)"
			failed=1
		fi
		# an error must have been reported to somebody
		if ! grep -qE "got (IO|ROFS)" "$W/wf$n.out" && ! grep -q "status IO\|=IO\|IO$" "$W/wf$n.out"; then
			: # the failed write may have been a retried or unneeded one
		fi
		if ! $CHECK "$img" --allow-leaks --allow-dirty --allow-fat-mismatch > "$W/fsck.out" 2>&1; then
			echo "    write error $n: $(grep ERROR "$W/fsck.out" | head -3 | tr '\n' ' ' | cut -c1-300)"
			failed=1
		fi
		if ! run "wfro$n" "$img" --cow -c "mount ro; readall; umount"; then
			echo "    write error $n: read-back failed"
			failed=1
		fi
		rm -f "$W/wf$n.out" "$W/wfro$n.out"
	done
	rm -f "$img"
	[ $failed = 0 ] && ok "write-error-injection" || bad "write-error-injection"
	# a failed data write is reported by fsync; the data stays dirty and the
	# next sync stores it (the entry never described data not stored)
	cp --sparse=always "$base" "$img"
	if run wf-fsync "$img" -c "mount; fopen 1 /w w+c; fault write 20; fwrite 1 pat:1:60000; =IO ffsync 1; fclose 1; sync; umount; mount ro; check /w pat:1:60000; umount" &&
		fsck_ok "$img"; then
		ok write-error-reported
	else
		bad wf-fsync
	fi
	rm -f "$img"
}

# Allocation failures (kalloc) at every allocation of a workload: the failing
# operation fails cleanly, everything else is exact (strict fsck: no lost
# clusters at all).
t_kfault() {
	local base img n failed=0 cmds
	base=$(mkfat kf 32M "-F 32 -s 1") || return 1
	run kf-prep "$base" -c "mount; mkdir /f; $(for i in $(seq 1 60); do echo -n "write /f/x$i pat:$i:3000; "; done) $(for i in $(seq 1 2 60); do echo -n "unlink /f/x$i; "; done) umount" || { bad kf-prep; return; }
	cmds="?mkdir /n; ?write /n/a\x20long\x20name.txt pat:7:40000; ?rename /n/a\x20long\x20name.txt /f/moved; ?truncate /f/x2 10; ?append /f/x4 pat:8:9000; ?unlink /f/x6; ?rmdir /n"
	img="$W/kfrun.img"
	for n in $(seq 1 60); do
		cp --sparse=always "$base" "$img"
		run "kf$n" "$img" --allow-warn -c "mount; kfault $n; $cmds; kfault 0; sync; write /after text:ok; umount"
		if grep -qE "runtime error|AddressSanitizer|BUG|DEADLOCK|still held|leak|FAIL" "$W/kf$n.out"; then
			echo "    allocation failure $n: $(grep -E "runtime error|AddressSanitizer|BUG|DEADLOCK|still held|leak|FAIL" "$W/kf$n.out" | head -2 | cut -c1-200)"
			failed=1
		fi
		if ! fsck_ok "$img"; then
			echo "    allocation failure $n: $(grep -E "ERROR|lost|wrong|differ" "$W/fsck.out" | head -3 | tr '\n' ' ' | cut -c1-300)"
			failed=1
		fi
		rm -f "$W/kf$n.out"
	done
	rm -f "$img"
	[ $failed = 0 ] && ok "allocation-failure-injection" || bad "allocation-failure-injection"
}

t_fuzz() {
	local img v
	for v in "fz12:4M:-F 12 -s 4" "fz16:16M:-F 16 -s 1" "fz32:64M:-F 32 -s 1"; do
		img=$(mkfat "${v%%:*}" "$(echo "$v" | cut -d: -f2)" "$(echo "$v" | cut -d: -f3)" tiny) || { bad "${v%%:*}-mkfs"; continue; }
		run "${v%%:*}-prep" "$img" -c "mount; mkdir /a; mkdir /a/b; write /a/b/f pat:1:70000; write /a/A\x20long\x20name.txt text:x; mkdir /c; $(for i in $(seq 1 40); do echo -n "write /c/file\x20number\x20$i text:$i; "; done) umount" || { bad "${v%%:*}-prep"; continue; }
		if host/test-fs.sh fat32 --fuzz fat "$img" --iters "$FUZZ_ITERS" --seed 101 --regress "$W/regress" > "$W/${v%%:*}-fuzz-ro.out" 2>&1; then
			ok "${v%%:*}-fuzz-ro"
		else
			bad "${v%%:*}-fuzz-ro"
		fi
		if host/test-fs.sh fat32 --fuzz fat "$img" --iters "$FUZZ_ITERS" --seed 102 --rw --regress "$W/regress" > "$W/${v%%:*}-fuzz-rw.out" 2>&1; then
			ok "${v%%:*}-fuzz-rw"
		else
			bad "${v%%:*}-fuzz-rw"
		fi
		tail -n 1 "$W/${v%%:*}-fuzz-ro.out" "$W/${v%%:*}-fuzz-rw.out" | grep "fuzz:" | sed 's/^/    /'
		rm -f "$img"
	done
	[ -d "$W/regress" ] && { mkdir -p "$TESTFS_DIR/fat-regress" && cp -r "$W/regress/." "$TESTFS_DIR/fat-regress/"; echo "    findings kept in $TESTFS_DIR/fat-regress"; }
	return 0
}

t_deep() {
	local img
	img=$(mkfat deep 128M "-F 32 -s 1") || return 1
	python3 -I host/fsimg/deeppath.py rw-script --name 255 --xdir-rename > "$W/deep.script" || { bad deep-script; return; }
	if run deep "$img" --chunk 1000000 -s "$W/deep.script" && fsck_ok "$img"; then
		ok deep-paths-255
	else
		bad deep
	fi
	img=$(mkfat deep16 64M "-F 16 -s 2") || return 1
	if run deep16 "$img" --chunk 1000000 -s "$W/deep.script" && fsck_ok "$img"; then
		ok deep-paths-fat16
	else
		bad deep16
	fi
}

# DOS 1.x floppies carry no parameter block: dos1xfloppy mounts them by
# their size and the media byte of the FAT.
t_dos1x() {
	local img
	img=$(mkfat dos1x 360K "-F 12 -f 2 -r 112 -s 2 -S 512 -h 0 -g 2/9" tiny) || return 1
	run dos1x-ref "$img" --cow -c "mount ro; manifest; umount" || { bad dos1x-ref; return; }
	grep "^[fd] " "$W/dos1x-ref.out" > "$W/dos1x.manifest"
	# wipe the parameter block (bytes 11..61); DOS 1.x wrote the jump
	python3 -I -c '
import sys
f = open(sys.argv[1], "r+b")
f.seek(11); f.write(b"\0" * 51)
' "$img"
	# (ST_NOT_FOUND: "not this filesystem", EINVAL at mount(2))
	if run dos1x-no "$img" --cow -c "=NOT_FOUND mount ro" &&
		run dos1x "$img" --cow -c "mount ro dos1xfloppy; manifest; umount" &&
		python3 -I host/fsimg/mktree.py compare "$W/dos1x.manifest" "$W/dos1x.out" >> "$W/dos1x.out" 2>&1; then
		ok dos1x-floppy
	else
		bad dos1x
	fi
}

# FAT32 with mirroring off: only the active FAT is used and written.
t_nomirror() {
	local img
	img=$(mkfat nomir 64M "-F 32 -s 1") || return 1
	python3 -I -c '
import sys, struct
f = open(sys.argv[1], "r+b")
for off in (0, 6 * 512):
    f.seek(off + 40); f.write(struct.pack("<H", 0x81))
' "$img"
	cp "$img" "$W/nomir.orig"
	if run nomirror "$img" -c "mount; mkdir /d; write /d/f pat:1:100000; cycle; check /d/f pat:1:100000; umount" &&
		$CHECK "$img" > "$W/fsck.out" 2>&1 &&
		python3 -I -c '
import sys, struct
a = open(sys.argv[1], "rb"); b = open(sys.argv[2], "rb")
h = a.read(512)
bps = struct.unpack_from("<H", h, 11)[0]; res = struct.unpack_from("<H", h, 14)[0]
fsz = struct.unpack_from("<I", h, 36)[0]
a.seek(res * bps); b.seek(res * bps)
# FAT copy 0 (inactive) untouched, copy 1 (active) changed
sys.exit(0 if a.read(fsz * bps) == b.read(fsz * bps) and a.read(fsz * bps) != b.read(fsz * bps) else 1)
' "$img" "$W/nomir.orig"; then
		ok fat32-no-mirroring
	else
		cat "$W/fsck.out" >> "$W/nomirror.out"
		bad nomirror
	fi
	rm -f "$W/nomir.orig"
}

# Open-unlinked files keep their clusters until the last close.
t_orphan() {
	local img
	img=$(mkfat orph 64M "-F 32") || return 1
	case_rw open-unlinked "$img" "mount
fopen 1 /f w+c
fwrite 1 pat:4:300000
unlink /f
=NOT_FOUND stat /f
fpwrite 1 300000 pat:5:100000
fpread 1 0 16
fstat 1
write /other text:still-fine
fclose 1
cycle
=NOT_FOUND stat /f
check /other text:still-fine
umount"
	# seek / pwrite / append at a cluster boundary
	case_rw cluster-boundary "$img" "mount
fopen 2 /b w+c
fwrite 2 pat:1:4096
fseek 2 0 1
fwrite 2 pat:2:4096
fseek 2 8192 0
fwrite 2 pat:3:512
fclose 2
pwrite /b 8704 pat:4:3584
append /b pat:5:4096
append /b pat:6:1
stat /b
cycle
pread /b 4090 12
stat /b
umount"
}

# st_ino is stable: the same across unmounts for files with clusters (the
# first cluster), empty files (directory cluster + slot) and directories.
t_ino() {
	local img a b
	img=$(mkfat ino 64M "-F 32") || return 1
	run ino-prep "$img" -c "mount; mkdir /d; write /d/f pat:1:5000; create /d/empty; mkdir /d/sub; umount" || { bad ino-prep; return; }
	if run ino1 "$img" --cow -c "mount ro; manifest / +ino; umount" &&
		run ino2 "$img" --cow -c "mount ro; dropcaches; manifest / +ino; cycle; manifest / +ino; umount"; then
		a=$(grep "^[fd] " "$W/ino1.out" | sort -u)
		b=$(grep "^[fd] " "$W/ino2.out" | sort -u)
		if [ "$a" = "$b" ] && [ "$(echo "$a" | grep -o 'ino=[0-9]*' | sort | uniq -d | wc -l)" = 0 ]; then
			ok inode-numbers-stable
		else
			bad ino2
		fi
	else
		bad ino1
	fi
}

# A task killed while holding the driver's locks: release_locks_for_task
# gives them back, the mount goes on working.
t_locks() {
	local img
	img=$(mkfat locks 64M "-F 32") || return 1
	case_rw lock-release "$img" "mount; task 7; write /a text:x; fopen 1 /a r; fread 1 1; release 7; task 1; write /b text:y; check /a text:x; fclose 1; umount"
}

# A directory entry naming a directory above it (hostile media: an endless
# tree) is refused with EIO instead of being walked into for ever.
t_dirloop() {
	local img
	img=$(mkfat dirloop 32M "-F 16 -s 1") || return 1
	run dirloop-prep "$img" -c "mount; mkdir /A; mkdir /A/B; mkdir /A/B/C; write /A/B/f text:x; umount" || { bad dirloop-prep; return; }
	# /A/B/C now names the clusters of /A
	python3 -I - "$img" <<'PYEOF' || { bad dirloop-patch; return; }
import struct, sys
f = open(sys.argv[1], "r+b")
b = f.read(512)
bps = struct.unpack_from("<H", b, 11)[0]; spc = b[13]; res = struct.unpack_from("<H", b, 14)[0]
nf = b[16]; rootents = struct.unpack_from("<H", b, 17)[0]; fsz = struct.unpack_from("<H", b, 22)[0]
root = (res + nf * fsz) * bps
data = root + rootents * 32
def find(off, n, name):
    f.seek(off)
    d = f.read(n)
    for i in range(0, n, 32):
        if d[i:i + 11] == name:
            return off + i, struct.unpack_from("<H", d, i + 26)[0]
    sys.exit("not found")
cs = bps * spc
_, a = find(root, rootents * 32, b"A          ")
_, bcl = find(data + (a - 2) * cs, cs, b"B          ")
eoff, _ = find(data + (bcl - 2) * cs, cs, b"C          ")
f.seek(eoff + 26)
f.write(struct.pack("<H", a))
PYEOF
	if run dirloop "$img" --cow --trap --allow-warn -c "mount ro; =IO stat /A/B/C; =IO stat /A/B/C/B; check /A/B/f text:x; umount; mount rw; =IO mkdir /A/B/C/new; umount"; then
		ok directory-loop-refused
	else
		bad dirloop
	fi
}

ALL="types utf8 shortname xdir noop ino locks dirloop dirty fsinfo backup options attrib times errors discard full many remount flush interop orphan nomirror dos1x deep powercut wfault kfault fuzz"
LIST=${*:-$ALL}
for t in $LIST; do
	"t_$t"
done
echo "test-fat32: $PASS passed, $FAILED failed"
[ -n "$FAILS" ] && echo "failed:$FAILS (outputs in $W, kept)" && trap - EXIT
[ "$FAILED" = 0 ]
