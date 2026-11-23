#!/bin/bash
# exFAT driver tests beyond host/fsimg/suite-exfat.sh, on the shared
# harness (host/test-fs.sh): the scenarios of the system's mount test
# (Unicode and 255-unit names, case-insensitive lookup, a 3 MiB file over
# remounts, truncate shrink/extend with zero fill, 300 files and a
# directory rename), zero-write proofs for idle read-write mounts and
# remounts, the dirty/force gate, discard, open-unlinked files, remount
# ro/rw, cluster sizes below a page, 4 KB device sectors, > 4 GB sizes, a
# full volume, mount options, power-cut sweeps and fuzzing -- every write
# workload followed by fsck.exfat -n and host/test-exfat-check.py (which
# checks the allocation bitmap against the files, cross-links, checksums
# and name hashes: fsck.exfat -n does not compare the bitmap at all).
#
#   host/test-exfat.sh            all of it
#   host/test-exfat.sh NAME...    only the named tests (see the list below)
#   EXFAT_FUZZ_ITERS=N            fuzz iterations (default 200)
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
export TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-exfat}
mkdir -p "$TESTFS_DIR" || exit 2
W=$(mktemp -d "$TESTFS_DIR/run.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT
FUZZ_ITERS=${EXFAT_FUZZ_ITERS:-200}

for t in mkfs.exfat fsck.exfat; do
	command -v $t >/dev/null 2>&1 || { echo "test-exfat: $t missing (exfatprogs)"; exit 77; }
done
host/test-fs.sh exfat --build >/dev/null || { echo "test-exfat: harness build failed"; exit 2; }

PASS=0
FAILED=0
FAILS=""

ok() { PASS=$((PASS + 1)); echo "PASS $1"; }
bad() {
	FAILED=$((FAILED + 1))
	FAILS="$FAILS $1"
	echo "FAIL $1"
	[ -f "$W/$1.out" ] && grep -E "^FAIL|WRITE TRAP|BUG|WARNING|runtime error|ERROR|corrupt|fsck" "$W/$1.out" | head -20 | sed 's/^/    /'
}

# mkimg NAME SIZE [mkfs.exfat options]: a fresh image
mkimg() {
	local f="$W/$1.img"
	rm -f "$f"
	truncate -s "$2" "$f" || return 1
	shift 2
	mkfs.exfat -q -L LIKEOS "$@" "$f" >/dev/null 2>&1 || mkfs.exfat -L LIKEOS "$@" "$f" >/dev/null || return 1
	echo "$f"
}

# run NAME IMAGE [runner args]: one harness run, output in $W/NAME.out
run() {
	local name=$1
	shift
	host/test-fs.sh exfat "$@" > "$W/$name.out" 2>&1
}

# fsck_ok IMAGE [checker options]: fsck.exfat -n clean AND the own checker
# (bitmap vs. owners, cross-links, checksums, NameHash -- what fsck.exfat
# -n does not look at) consistent; strict unless options allow leaks or
# the dirty flag.
fsck_ok() {
	local img=$1
	shift
	fsck.exfat -n "$img" > "$W/fsck.out" 2>&1 && grep -q "clean" "$W/fsck.out" &&
		python3 -I "$ROOT/host/test-exfat-check.py" "$img" "$@" >> "$W/fsck.out" 2>&1
}

# case NAME IMAGE CMDS [runner args]: run + fsck
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

# The volume flags word of the main boot sector.
vol_flags() {
	python3 -I -c 'import sys,struct;f=open(sys.argv[1],"rb");f.seek(106);print(struct.unpack("<H",f.read(2))[0])' "$1"
}

set_dirty() {
	python3 -I -c '
import sys, struct
f = open(sys.argv[1], "r+b")
f.seek(106)
v = struct.unpack("<H", f.read(2))[0] | 2
f.seek(106)
f.write(struct.pack("<H", v))
' "$1"
}

t_mounttest() {
	local img
	img=$(mkimg mt 64M) || return 1
	local long
	# A long name (the VFS passes names of at most 255 bytes):
	# 250 units, 17 name entries -- a set of 19 entries.
	long=$(printf 'n%.0s' $(seq 1 250))
	case_rw mounttest "$img" "mount rw uid=1000,gid=1000,fmask=0177,dmask=0077
mkdir /u
write /u/\xc3\xa4\xc3\xb6\xc3\xbc.txt text:umlaut
write /u/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt text:cjk
write /u/\xf0\x9f\x98\x80smile text:emoji
write /$long text:long
stat /U/\xc3\x84\xc3\x96\xc3\x9c.TXT
check /U/\xc3\x84\xc3\x96\xc3\x9c.TXT text:umlaut
=EXISTS create /u/\xc3\x84\xc3\x96\xc3\x9c.txt
write /big pat:7:3145728
cycle
check /BIG pat:7:3145728
check /u/\xf0\x9f\x98\x80SMILE text:emoji
check /$long text:long
unlink /$long
write /t pat:3:100000
truncate /t 5000
check /t pat:3:5000
truncate /t 90000
zeros /t 5000 85000
pread /t 4990 20
cycle
zeros /t 5000 85000
truncate /t 5000
check /t pat:3:5000
mkdir /many
echo 300 files
$(for i in $(seq 1 300); do echo "write /many/file$i text:$i"; done)
rename /many /many2
cycle
$(for i in 1 2 150 299 300; do echo "check /many2/FILE$i text:$i"; done)
=NOT_FOUND stat /many
statfs
options
umount"
}

# More streams than the driver keeps cached (LRU eviction, handle reuse,
# dcache keys of evicted directories).
t_many() {
	local img
	img=$(mkimg many 256M) || return 1
	case_rw many-streams "$img" "mount
$(for d in 1 2 3 4 5; do echo "mkdir /d$d"; for i in $(seq 1 600); do echo "write /d$d/f$i text:$d-$i"; done; done)
readall
rename /d3 /d1/moved
$(for i in 1 300 600; do echo "check /d1/moved/F$i text:3-$i"; done)
readall
$(for i in $(seq 1 600); do echo "unlink /d2/f$i"; done)
rmdir /d2
cycle
readall
$(for i in 7 599; do echo "check /d5/f$i text:5-$i"; done)
statfs
umount"
}

# set_vdl IMAGE NAME VDL: give the root-directory file NAME (ASCII) a
# ValidDataLength of VDL, entry-set checksum recomputed.
set_vdl() {
	python3 -I - "$@" <<'PYEOF'
import sys, struct
img, name, vdl = sys.argv[1], sys.argv[2], int(sys.argv[3])
f = open(img, "r+b")
b = f.read(512)
bps = 1 << b[108]; spc = 1 << b[109]
heap = struct.unpack_from("<I", b, 88)[0]; fat = struct.unpack_from("<I", b, 80)[0]
root = struct.unpack_from("<I", b, 96)[0]
cs = bps * spc
def cl_off(c): return (heap + (c - 2) * spc) * bps
def fat_next(c):
    f.seek(fat * bps + c * 4); return struct.unpack("<I", f.read(4))[0]
c = root
while 2 <= c < 0xFFFFFFF7:
    f.seek(cl_off(c)); d = bytearray(f.read(cs))
    for i in range(0, cs, 32):
        if d[i] != 0x85: continue
        sc = d[i + 1]; s = d[i:i + 32 * (sc + 1)]
        if len(s) < 32 * (sc + 1): continue
        n = s[64 + 3] if False else s[32 + 3]
        chars = []
        for k in range(sc - 1):
            e = s[64 + 32 * k:96 + 32 * k]
            chars += list(struct.unpack_from("<15H", e, 2))
        if "".join(map(chr, chars[:n])) != name: continue
        struct.pack_into("<Q", s, 32 + 8, vdl)
        sm = 0
        for j, x in enumerate(s):
            if j in (2, 3): continue
            sm = ((sm >> 1) | ((sm & 1) << 15)) + x & 0xFFFF
        struct.pack_into("<H", s, 2, sm)
        f.seek(cl_off(c) + i); f.write(s); sys.exit(0)
    c = fat_next(c)
sys.exit("not found")
PYEOF
}

t_nofill() {
	local img c
	for c in 512 4K 32K; do
		img=$(mkimg "nf$c" 64M -c $c) || return 1
		run "nofill-prep-$c" "$img" -c "mount; write /f pat:5:20000; write /g pat:6:9000; umount" || { bad "nofill-prep-$c"; continue; }
		set_vdl "$img" f 5003 || { bad "nofill-patch-$c"; continue; }
		set_vdl "$img" g 0 || { bad "nofill-patch-$c"; continue; }
		case_rw "nofill-$c" "$img" "mount ro
stat /f
pread /f 4990 20
zeros /f 5003 14997
zeros /g 0 9000
umount
mount
zeros /f 5003 14997
pwrite /f 12000 pat:7:100
zeros /f 5003 6997
zeros /f 12100 7900
append /g text:tail
zeros /g 0 9000
cycle
zeros /f 5003 6997
zeros /f 12100 7900
pread /f 11995 110
truncate /f 4000
truncate /f 30000
zeros /f 4000 26000
cycle
zeros /f 4000 26000
umount"
	done
}

t_trap_idle() {
	local img
	img=$(mkimg idle 64M) || return 1
	run idle-prep "$img" -c "mount; mkdir /d; write /d/f pat:1:200000; write /g text:x; umount" || { bad idle-prep; return; }
	# read-write mount, walk, read, stat, statfs, sync, options, remounts:
	# zero writes, zero discards
	if run trap-idle-rw "$img" --trap -c "mount rw discard; readall; ls /; stat /d/f; statfs; options; sync; remount ro; remount rw; sync; cycle; readall; umount; mount ro; readall; umount"; then
		ok trap-idle-rw
	else
		bad trap-idle-rw
	fi
	# a write after an idle period arms; then clean again on unmount
	if run trap-arm "$img" -c "mount rw; readall; mark; write /h text:y; umount" && fsck_ok "$img" &&
		[ $(( $(vol_flags "$img") & 2 )) -eq 0 ]; then
		ok trap-arm-clean
	else
		bad trap-arm-clean
	fi
	# read-only device: rw is refused by the gate, nothing written
	if run trap-rodev "$img" --ro-dev --trap -c "=ROFS mount rw; mount ro; readall; =ROFS mkdir /x; umount"; then
		ok trap-rodev
	else
		bad trap-rodev
	fi
}

t_dirty() {
	local img
	img=$(mkimg dirty 64M) || return 1
	run dirty-prep "$img" -c "mount; write /f text:abc; umount" || { bad dirty-prep; return; }
	set_dirty "$img"
	if run dirty-gate "$img" --trap -c "mount rw; =ROFS mkdir /x; =ROFS write /f text:z; check /f text:abc; options; =ROFS remount rw; umount"; then
		ok dirty-gate
	else
		bad dirty-gate
	fi
	# MediaFailure: read-only whatever the options say
	cp "$img" "$W/mf.img"
	python3 -I -c '
import sys, struct
f = open(sys.argv[1], "r+b")
f.seek(106)
v = (struct.unpack("<H", f.read(2))[0] | 4) & ~2
f.seek(106)
f.write(struct.pack("<H", v))
' "$W/mf.img"
	if run media-failure "$W/mf.img" --trap -c "mount rw force; =ROFS mkdir /x; check /f text:abc; =ROFS remount rw force; umount"; then
		ok media-failure-gate
	else
		bad media-failure-gate
	fi
	if run dirty-force "$img" -c "mount rw force; mkdir /x; write /x/y pat:2:50000; umount" &&
		fsck_ok "$img" --allow-dirty && [ $(( $(vol_flags "$img") & 2 )) -ne 0 ]; then
		ok dirty-force-stays-dirty
	else
		bad dirty-force-stays-dirty
	fi
}

t_discard() {
	local img
	img=$(mkimg disc 64M) || return 1
	if run discard "$img" --log -c "mount rw discard; write /a pat:1:1000000; sync; log clear; unlink /a; sync; counts; umount" &&
		grep -E "discards=[1-9]" "$W/discard.out" >/dev/null && fsck_ok "$img"; then
		ok discard
	else
		bad discard
	fi
	img=$(mkimg nodisc 64M) || return 1
	if run nodiscard "$img" -c "mount rw; write /a pat:1:1000000; sync; unlink /a; sync; umount" &&
		grep -E "discards=0" "$W/nodiscard.out" >/dev/null && fsck_ok "$img"; then
		ok nodiscard
	else
		bad nodiscard
	fi
}

t_orphan() {
	local img
	img=$(mkimg orph 64M) || return 1
	case_rw open-unlinked "$img" "mount
statfs
fopen 1 /f w+c
fwrite 1 pat:4:300000
unlink /f
=NOT_FOUND stat /f
fpwrite 1 300000 pat:5:100000
fpread 1 0 16
fstat 1
write /other text:still-fine
fclose 1
statfs
cycle
=NOT_FOUND stat /f
check /other text:still-fine
umount"
}

t_remount() {
	local img
	img=$(mkimg rmnt 64M) || return 1
	case_rw remount "$img" "mount
write /a pat:1:70000
remount ro
=ROFS write /b text:x
check /a pat:1:70000
remount rw
write /b text:x
append /a pat:9:10
chmod /a 0444
cycle
stat /a
chmod /a 0644
utimens /a 1700000000 1700000001 500000000
stat /a
umount
mount rw time=local,time_offset=60
options
utimens /b now now
umount"
}

t_options() {
	local img
	img=$(mkimg opts 64M) || return 1
	if run options "$img" -c "=INVALID mount rw errors=sometimes; =INVALID mount rw uid=abc; =INVALID mount rw iocharset=latin1; mount rw uid=1000,gid=100,umask=027,errors=continue,discard,iocharset=utf8; options; umount" &&
		grep -q "uid=1000,gid=100,fmask=0027,dmask=0027" "$W/options.out" &&
		grep -q "errors=continue,discard" "$W/options.out"; then
		ok options
	else
		bad options
	fi
}

t_clusters() {
	local c img
	for c in 512 1K 2K 4K 64K 1M; do
		# 1 MB clusters: every file and directory takes one, so the
		# volume must hold far more of them than the stress budget says.
		local size=96M
		[ $c = 1M ] && size=2G
		img=$(mkimg "cl$c" $size -c $c) || continue
		case_rw "stress-c$c" "$img" "mount; stressopt names ci; stressopt utf8 1; stress 3${c%[KM]} 1200 200"
	done
	img=$(mkimg sec4k 128M -c 8K) || return 1
	case_rw stress-sector4096 "$img" "mount; stressopt names ci; stress 77 1200 200" --sector 4096
	img=$(mkimg pack 200M --pack-bitmap -c 32K) || return 1
	case_rw stress-packbitmap "$img" "mount; stressopt names ci; stress 78 1200 200"
}

t_big() {
	local img
	img=$(mkimg big 6G -c 128K) || return 1
	case_rw big-sizes "$img" "mount
write /b pat:1:5000000
truncate /b 4500000000
stat /b
pread /b 4499999990 10
zeros /b 5000000 1000000
cycle
stat /b
pread /b 4999990 20
zeros /b 4499000000 1000000
truncate /b 4294967297
cycle
stat /b
truncate /b 0
statfs
umount"
}

t_full() {
	local img
	img=$(mkimg full 8M -c 4K) || return 1
	case_rw near-full "$img" "mount
statfs
?write /f1 pat:1:4000000
?write /f2 pat:2:4000000
=NOSPC write /f3 pat:3:4000000
statfs
unlink /f1
write /f3 pat:3:1000000
cycle
check /f3 pat:3:1000000
statfs
umount"
}

t_powercut() {
	local n img base failed=0
	base=$(mkimg pcbase 64M) || return 1
	run pc-prep "$base" -c "mount; mkdir /keep; write /keep/k pat:1:200000; umount" || { bad pc-prep; return; }
	for n in 1 2 3 4 5 6 7 8 9 10 12 14 16 20 24 28 32 40 48 64 96 128; do
		img="$W/pc.img"
		cp "$base" "$img"
		run "pc$n" "$img" -c "mount; fault cut $n; ?mkdir /pc; ?mkdir /pc/sub; ?write /pc/a pat:1:300000; ?write /pc/sub/b pat:2:50000; ?rename /pc/a /pc/sub/a2; ?truncate /pc/sub/b 1000; ?write /pc/c text:hello; ?unlink /pc/c; ?rename /pc/sub /pc/sub2; ?append /pc/sub2/b pat:3:9000; ?write /keep/k pat:4:4096; ?sync; ?umount"
		if ! fsck_ok "$img" --allow-leaks --allow-dirty; then
			echo "    power cut after $n writes: fsck: $(tr '\n' ' ' < "$W/fsck.out")"
			failed=1
		fi
		grep -o "[0-9]* lost clusters, [0-9]* chain-tail" "$W/fsck.out" | sed "s/^/    cut $n: /"
		# what is left must mount (read-only: the volume is dirty) and read
		if ! run "pcm$n" "$img" --cow -c "mount ro; readall; umount"; then
			echo "    power cut after $n writes: read-back failed"
			failed=1
		fi
	done
	[ $failed = 0 ] && ok power-cut-sweep || bad power-cut-sweep
}

t_fuzz() {
	local img
	img=$(mkimg fz 32M) || return 1
	run fz-prep "$img" -c "mount; mkdir /a; mkdir /a/b; write /a/b/f pat:1:70000; write /a/g text:x; mkdir /c; $(for i in $(seq 1 40); do echo "write /c/f$i text:$i;"; done) umount" || { bad fz-prep; return; }
	if host/test-fs.sh exfat --fuzz exfat "$img" --iters "$FUZZ_ITERS" --seed 91 > "$W/fuzz-ro.out" 2>&1; then ok fuzz-ro; else bad fuzz-ro; fi
	if host/test-fs.sh exfat --fuzz exfat "$img" --iters "$FUZZ_ITERS" --seed 92 --rw > "$W/fuzz-rw.out" 2>&1; then ok fuzz-rw; else bad fuzz-rw; fi
	tail -n 3 "$W/fuzz-ro.out" "$W/fuzz-rw.out"
}

# ---- write safety: allocation failures, write errors, power cuts ----------

# A failing append (allocation done, then out of memory) must leave the
# stream exactly as it was: no FAT link into clusters given back to the
# bitmap (they would be handed out twice).  Every allocation failure
# point of a large append, on a fragmented volume and on the root
# directory's growth.
t_appendfail() {
	local img base n failed=0
	base=$(mkimg afbase 32M -c 4K) || return 1
	# fragment the free space: many small files, every other one deleted
	run af-prep "$base" -c "mount; mkdir /f; $(for i in $(seq 1 300); do echo -n "write /f/x$i pat:$i:6000; "; done) $(for i in $(seq 1 2 300); do echo -n "unlink /f/x$i; "; done) umount" || { bad af-prep; return; }
	for n in $(seq 1 24); do
		img="$W/af.img"
		cp "$base" "$img"
		run "af$n" "$img" --allow-warn -c "mount; fopen 1 /big w+c; fwrite 1 pat:7:20000; kfault $n; ?fwrite 1 pat:8:100000; kfault 0; fwrite 1 pat:8:12000; fclose 1; write /other pat:9:200000; umount"
		if ! fsck_ok "$img"; then
			echo "    allocation failure $n: $(tr '\n' ' ' < "$W/fsck.out" | cut -c1-300)"
			failed=1
		fi
	done
	# the root directory's growth (FAT-chained, its length is its chain)
	base=$(mkimg afroot 32M -c 512) || return 1
	for n in $(seq 1 16); do
		img="$W/afr.img"
		cp "$base" "$img"
		run "afr$n" "$img" --allow-warn -c "mount; $(for i in $(seq 1 40); do echo -n "write /root_file_$i text:$i; "; done) kfault $n; ?mkdir /grow_dir_with_a_long_name; ?write /another_file_in_the_root text:x; kfault 0; mkdirp /later; umount"
		if ! fsck_ok "$img"; then
			echo "    root growth failure $n: $(tr '\n' ' ' < "$W/fsck.out" | cut -c1-300)"
			failed=1
		fi
	done
	[ $failed = 0 ] && ok append-failure-all-or-nothing || bad append-failure-all-or-nothing
}

# More changed entry sets than the directory write queue holds, in one
# commit (a 128-handle runner): never a nested commit, every set stored,
# and a power cut anywhere in it leaves no error.
t_nested() {
	local bin="$W/test-fs-exfat128" b img base n failed=0 total cmds
	b=$(dirname "$(host/test-fs.sh exfat --build)")
	sed -e 's/g_h\[16\]/g_h[128]/' -e 's/return n < 16 ? (int)n : -1;/return n < 128 ? (int)n : -1;/' \
		-e 's/for (int h = 0; h < 16; h++)/for (int h = 0; h < 128; h++)/' \
		host/test-fs-main.c > "$W/main128.c"
	if ! grep -q 'g_h\[128\]' "$W/main128.c" ||
	   ! gcc -std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin \
		-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1 -U__linux__ \
		-DTEST_FSTYPE=exfat_fstype -Wno-discarded-qualifiers \
		-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined \
		-fno-omit-frame-pointer -include host/test-fs-prelude.h \
		-c "$W/main128.c" -o "$W/main128.o" 2> "$W/main128.log" ||
	   ! gcc -fsanitize=address,undefined -o "$bin" "$W/main128.o" \
		$(ls "$b"/*.o | grep -v '/test-fs-main\.o$') 2>> "$W/main128.log"; then
		cat "$W/main128.log"
		bad nested-commit-build
		return
	fi
	base=$(mkimg ncbase 64M -c 4K) || return 1
	cmds="mount; $(for i in $(seq 1 60); do echo -n "mkdir /d$i; "; done) umount"
	run nc-prep "$base" -c "$cmds" || { bad nc-prep; return; }
	# 60 files in 60 directories open and written, a file truncated in
	# between (its clusters freed by the same commit): one sync commits
	# all of it.  $1 is the prefix of every command ("" or "?").
	nc_cmds() {
		local q=$1 i
		for i in $(seq 1 60); do echo -n "${q}fopen $i /d$i/f w+c; ${q}fwrite $i pat:$i:9000; "; done
		echo -n "${q}write /victim pat:1:300000; ${q}truncate /victim 10; "
		for i in $(seq 1 60); do echo -n "${q}fpwrite $i 9000 pat:$i:5000; "; done
		echo -n "${q}sync; "
		for i in $(seq 1 60); do echo -n "${q}fclose $i; "; done
		echo -n "${q}umount"
	}
	cmds="mount; $(nc_cmds "")"
	img="$W/nc.img"
	cp "$base" "$img"
	if ASAN_OPTIONS=detect_leaks=0:exitcode=4 timeout 600 "$bin" "$img" -c "$cmds" > "$W/nested.out" 2>&1 &&
	   fsck_ok "$img"; then
		ok nested-commit
	else
		cat "$W/fsck.out" >> "$W/nested.out"
		bad nested-commit
	fi
	total=$(ASAN_OPTIONS=detect_leaks=0 timeout 600 "$bin" "$base" --cow -c "$cmds; counts" 2>&1 |
		sed -n 's/.*counts reads=[0-9]* writes=\([0-9]*\).*/\1/p' | tail -1)
	for n in $(seq 1 3 ${total:-150}); do
		cp "$base" "$img"
		ASAN_OPTIONS=detect_leaks=0:exitcode=4 timeout 600 "$bin" "$img" --allow-warn -c "mount; fault cut $n; $(nc_cmds "?")" > "$W/ncpc.out" 2>&1
		if ! fsck_ok "$img" --allow-leaks --allow-dirty; then
			echo "    power cut after $n writes: $(tr '\n' ' ' < "$W/fsck.out" | cut -c1-300)"
			failed=1
		fi
	done
	[ $failed = 0 ] && ok nested-commit-power-cut || bad nested-commit-power-cut
}

# Write errors: fsync, close and sync report them; the entry set keeps
# the old size until the data is stored; VolumeDirty stays set while
# anything failed; blocks smaller than a page keep failed pages dirty.
t_werr() {
	local c img
	for c in 4K 512; do
		img=$(mkimg "we$c" 32M -c $c) || return 1
		# the first write of the mount is the boot sector (arming)
		if run "werr-arm-$c" "$img" --allow-warn -c "mount; fopen 1 /f w+c; fwrite 1 pat:1:100000; fault write 1; =IO ffsync 1; ffsync 1; fclose 1; check /f pat:1:100000; umount; mount ro; check /f pat:1:100000; umount" &&
		   fsck_ok "$img"; then
			ok "werr-arm-$c"
		else
			bad "werr-arm-$c"
		fi
		# a data write fails: fsync says so, the old (empty) entry set
		# stays on the device, the retry stores everything
		if run "werr-data-$c" "$img" --allow-warn -c "mount; fopen 1 /g w+c; ffsync 1; fwrite 1 pat:2:100000; fault write 2; =IO ffsync 1; stat /g; ffsync 1; fclose 1; cycle; check /g pat:2:100000; umount" &&
		   fsck_ok "$img"; then
			ok "werr-data-$c"
		else
			bad "werr-data-$c"
		fi
		# close reports an error of its data, and the volume stays dirty
		# until a clean store
		if run "werr-close-$c" "$img" --allow-warn -c "mount; fopen 1 /h w+c; fwrite 1 pat:3:50000; fault write 2; =IO fclose 1; sync; check /h pat:3:50000; umount" &&
		   fsck_ok "$img"; then
			ok "werr-close-$c"
		else
			bad "werr-close-$c"
		fi
	done
	# A write of sync's store fails (every one in turn: the failure is
	# one-shot) while a handle holds unstored data; the device is saved
	# right after.  When that image says clean (VolumeDirty clear), all of
	# it must be stored and consistent; some failures must leave it dirty.
	local n dirty_seen=0 bad_sync=0 total
	img=$(mkimg wescount 32M) || return 1
	run werr-sync-count "$img" -c "mount; fopen 1 /u w+c; fwrite 1 pat:4:30000; mkdir /dd; mark; sync; counts; fclose 1; umount"
	total=$(sed -n 's/.*writes=\([0-9]*\).*/\1/p' "$W/werr-sync-count.out" | head -1)
	for n in $(seq 1 $(( ${total:-20} + 1 ))); do
		img=$(mkimg "wes$n" 32M) || return 1
		run "werr-sync-$n" "$img" --allow-warn -c "mount; fopen 1 /u w+c; fwrite 1 pat:4:30000; mkdir /dd; fault write $n; ?sync; save $W/snap.img; fclose 1; umount"
		if [ $(( $(vol_flags "$W/snap.img") & 2 )) -ne 0 ]; then
			dirty_seen=1
			fsck_ok "$W/snap.img" --allow-dirty --allow-leaks || bad_sync=1
		elif ! fsck_ok "$W/snap.img" ||
		     ! run "werr-sync-check-$n" "$W/snap.img" --cow -c "mount ro; check /u pat:4:30000; stat /dd; umount"; then
			echo "    failure $n: marked clean but not all stored"
			bad_sync=1
		fi
		fsck_ok "$img" || bad_sync=1
	done
	[ $bad_sync = 0 ] && [ $dirty_seen = 1 ] && ok werr-sync-clean-only-when-stored ||
		bad werr-sync-clean-only-when-stored
	# failed pages of small blocks kept dirty: the page cache's own test
	if sh host/test-pagecache-ext.sh > "$W/pagecache-ext.out" 2>&1; then
		ok pagecache-ext
	else
		tail -20 "$W/pagecache-ext.out"
		bad pagecache-ext
	fi
}

# Every read-only reason is kept: force lifts the dirty one only.
t_gates() {
	local img
	img=$(mkimg gt 32M) || return 1
	run gt-prep "$img" -c "mount; write /f text:abc; umount" || { bad gt-prep; return; }
	python3 -I - "$img" dirty upcase <<'PYEOF'
import sys, struct
img, what = sys.argv[1], sys.argv[2:]
f = open(img, "r+b")
b = f.read(512)
bps = 1 << b[108]; spc = 1 << b[109]
heap = struct.unpack_from("<I", b, 88)[0]; root = struct.unpack_from("<I", b, 96)[0]
def cl_off(c): return (heap + (c - 2) * spc) * bps
for w in what:
    if w == "dirty":
        f.seek(106); v = struct.unpack("<H", f.read(2))[0] | 2; f.seek(106); f.write(struct.pack("<H", v))
    elif w == "upcase":
        f.seek(cl_off(root)); d = bytearray(f.read(bps * spc))
        for i in range(0, len(d), 32):
            if d[i] == 0x82:
                f.seek(cl_off(root) + i + 4)
                f.write(struct.pack("<I", struct.unpack_from("<I", d, i + 4)[0] ^ 1))
                break
PYEOF
	if run gates "$img" --trap -c "mount rw force; =ROFS mkdir /x; check /f text:abc; =ROFS remount rw force; =ROFS remount rw; umount; mount rw; =ROFS mkdir /x; =ROFS remount rw force; umount"; then
		ok gates-all-reasons
	else
		bad gates-all-reasons
	fi
}

# A bitmap marking the volume's own clusters free (the root directory
# here) makes the mount read-only before anything is written.
t_ownfree() {
	local img
	img=$(mkimg of 32M) || return 1
	run of-prep "$img" -c "mount; mkdir /d; write /d/f text:abc; umount" || { bad of-prep; return; }
	python3 -I - "$img" <<'PYEOF'
import sys, struct
f = open(sys.argv[1], "r+b"); b = f.read(512)
bps = 1 << b[108]; spc = 1 << b[109]
heap = struct.unpack_from("<I", b, 88)[0]; root = struct.unpack_from("<I", b, 96)[0]
f.seek((heap + (root - 2) * spc) * bps); d = f.read(bps * spc)
for i in range(0, len(d), 32):
    if d[i] == 0x81:
        bm = struct.unpack_from("<I", d, i + 20)[0]; break
off = (heap + (bm - 2) * spc) * bps
idx = root - 2
f.seek(off + idx // 8); v = f.read(1)[0]
f.seek(off + idx // 8); f.write(bytes([v & ~(1 << (idx % 8))]))
PYEOF
	if run ownfree "$img" --trap -c "mount rw; check /d/f text:abc; =ROFS mkdir /x; =ROFS write /d/g text:x; =ROFS remount rw; umount"; then
		ok ownfree-readonly
	else
		bad ownfree-readonly
	fi
}

# A task that dies holding the metadata lock shared: release_locks_for_task
# gives its holds back (unit test of the driver's rwsem, the driver
# included so its static functions can be called).
t_rwsem() {
	local b
	b=$(dirname "$(host/test-fs.sh exfat --build)")
	cat > "$W/rwsem.c" <<'CEOF'
#include "kernel/fs/exfat/super.c"
#include "kernel/fs/exfat/misc.c"
#include "kernel/fs/exfat/cache.c"
#include "kernel/fs/exfat/fatent.c"
#include "kernel/fs/exfat/balloc.c"
#include "kernel/fs/exfat/nls.c"
#include "kernel/fs/exfat/inode.c"
#include "kernel/fs/exfat/dir.c"
#include "kernel/fs/exfat/namei.c"
#include "kernel/fs/exfat/file.c"

void test_task_init(uint32_t uid, uint32_t gid);
void test_task_set_id(uint64_t id);
extern long test_warnings;

int vfs_link_hop(vfs_mount_t *m, const char *path, unsigned long cs,
		 unsigned long ce, const char *target)
{
	(void)m; (void)path; (void)cs; (void)ce; (void)target;
	return 0;
}

static int failures;
#define CHECK(c) do { if (!(c)) { h_printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
	exfat_rwsem_t *s = (exfat_rwsem_t *)kcalloc(1, sizeof(*s));
	task_t *t;

	test_task_init(0, 0);
	t = sched_current();
	exfat_rwsem_init(s, "exfat_test");
	test_task_set_id(5);
	exfat_rwsem_read_lock(s);
	exfat_rwsem_read_lock(s);
	test_task_set_id(6);
	exfat_rwsem_read_lock(s);
	CHECK(s->readers == 3 && s->locked == 3);
	/* task 5 dies inside the driver: both its shared holds come back */
	CHECK(exfat_rwsem_release_owner(s, 5) == 1);
	CHECK(s->readers == 1 && s->locked == 1);
	CHECK(exfat_rwsem_release_owner(s, 99) == 0);
	test_task_set_id(6);
	exfat_rwsem_read_unlock(s);
	CHECK(s->readers == 0 && s->locked == 0);
	/* an exclusive holder's death */
	test_task_set_id(7);
	exfat_rwsem_write_lock(s);
	exfat_rwsem_write_lock(s);
	exfat_rwsem_read_lock(s); /* shared under its exclusive: recursion */
	CHECK(exfat_rwsem_held_excl(s) && s->wdepth == 3);
	CHECK(exfat_rwsem_release_owner(s, 7) == 1);
	CHECK(!s->writer && s->locked == 0);
	/* more readers than recorded: the rest are counted and released by
	 * their holders */
	for (uint64_t id = 100; id < 100 + EXFAT_RWSEM_TRACK + 5; id++) {
		test_task_set_id(id);
		exfat_rwsem_read_lock(s);
	}
	CHECK(s->untracked == 5);
	CHECK(exfat_rwsem_release_owner(s, 100) == 1);
	for (uint64_t id = 101; id < 100 + EXFAT_RWSEM_TRACK + 5; id++) {
		test_task_set_id(id);
		exfat_rwsem_read_unlock(s);
	}
	CHECK(s->readers == 0 && s->untracked == 0 && s->locked == 0);
	t->fs_rdepth = 0;
	t->fs_held = 0;
	test_task_set_id(1);
	exfat_rwsem_write_lock(s);
	exfat_rwsem_write_unlock(s);
	CHECK(test_warnings == 0);
	kfree(s);
	h_printf("test-exfat-rwsem: %s (%d failure(s))\n", failures ? "FAILED" : "ok", failures);
	return failures ? 1 : 0;
}
CEOF
	if gcc -std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin \
		-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1 -U__linux__ \
		-Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
		-Wno-discarded-qualifiers -Werror=implicit-function-declaration \
		-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined \
		-fno-omit-frame-pointer -include host/test-fs-prelude.h \
		-c "$W/rwsem.c" -o "$W/rwsem.o" > "$W/rwsem.out" 2>&1 &&
	   gcc -fsanitize=address,undefined -o "$W/test-exfat-rwsem" "$W/rwsem.o" \
		$(ls "$b"/*.o | grep -v '/exfat_[^/]*\.o$\|/test-fs-main\.o$') >> "$W/rwsem.out" 2>&1 &&
	   timeout 60 "$W/test-exfat-rwsem" >> "$W/rwsem.out" 2>&1; then
		ok rwsem-shared-hold-release
	else
		tail -20 "$W/rwsem.out"
		bad rwsem-shared-hold-release
	fi
}

# Operations that change nothing write nothing (the boot sector
# included): no-op and failing renames, utimens omit/omit, O_TRUNC of an
# empty file, failing creates and removals.
t_noop() {
	local img
	img=$(mkimg noop 32M) || return 1
	run noop-prep "$img" -c "mount; mkdir /d; write /d/x text:1; mkdir /e; write /g text:abc; create /empty; umount" || { bad noop-prep; return; }
	if run noop "$img" --trap -c "mount rw; rename /g /g; =NOTEMPTY rename /e /d; =ISDIR rename /g /d; =NOTDIR rename /d /g; =INVALID rename /d /d/sub; =NOT_FOUND rename /nothing /x; utimens /g omit omit; fopen 1 /empty wt; fclose 1; =EXISTS mkdir /d; =NOTEMPTY rmdir /d; =ISDIR unlink /d; =NOT_FOUND unlink /none; =INVALID create /a:b; truncate /empty 0; chmod /g 0755; umount"; then
		ok noop-writes-nothing
	else
		bad noop-writes-nothing
	fi
}

# The root directory's growth on 512-byte clusters under power cuts
# (zeroed clusters before the FAT link); the page cache's side of the
# write ordering is its own test, run by t_werr.
t_rootgrow() {
	local base img n total failed=0 cmds
	base=$(mkimg rgbase 32M -c 512) || return 1
	cmds="$(for i in $(seq 1 60); do echo -n "?write /root_entry_number_$i text:$i; "; done) ?sync; ?umount"
	run rg-count "$base" --cow --log -c "mount; log clear; $cmds; counts"
	total=$(sed -n 's/.*counts reads=[0-9]* writes=\([0-9]*\).*/\1/p' "$W/rg-count.out" | tail -1)
	for n in $(seq 1 2 ${total:-200}); do
		img="$W/rg.img"
		cp "$base" "$img"
		run "rg$n" "$img" -c "mount; fault cut $n; $cmds"
		if ! fsck_ok "$img" --allow-leaks --allow-dirty; then
			echo "    power cut after $n writes: $(tr '\n' ' ' < "$W/fsck.out" | cut -c1-300)"
			failed=1
		fi
	done
	[ $failed = 0 ] && ok root-growth-power-cut || bad root-growth-power-cut
}

ALL="mounttest many nofill trap_idle dirty discard orphan remount options clusters big full powercut fuzz appendfail nested werr gates ownfree rwsem noop rootgrow"
LIST=${*:-$ALL}
for t in $LIST; do
	"t_$t"
done
echo "test-exfat: $PASS passed, $FAILED failed"
[ -n "$FAILS" ] && echo "failed:$FAILS (outputs in $W, kept: rerun with TESTFS_DIR to inspect)" && trap - EXIT
[ "$FAILED" = 0 ]
