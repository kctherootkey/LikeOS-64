#!/bin/bash
# Write-safety and zero-write audit of the exFAT driver (kernel/fs/exfat/)
# on the filesystem harness (host/test-fs.sh), on top of host/test-exfat.sh:
#
#   workloads   long randomized workloads with an exact model
#               (host/test-audit-exfat.py run): create / write / append /
#               pwrite with holes / truncate up and down / rename (same
#               directory, across directories, over an existing name,
#               case only) / unlink (also while open) / mkdir / rmdir /
#               failing operations / fsync / sync / writeback / cache
#               pressure, many seeds, every cluster size from 512 B to
#               32 MB, 512- and 4096-byte device sectors, 4096-byte
#               file-system sectors, > 4 GB sparse files, near-full
#               volumes, > 3000 files in one directory, deep trees, long
#               Unicode names.  After every workload: fsck.exfat -n clean,
#               host/test-exfat-check.py without a single error, leak or
#               dirty flag, PercentInUse right, and the whole tree equal
#               to the model after every remount.
#   powercut    every write of recorded workloads (all cluster sizes) as a
#               power-cut point, in order (prefix) and with the writes
#               since the last flush reordered and torn at sector
#               boundaries (reorder): only lost clusters / chain tails /
#               VolumeDirty may remain, nothing synced is lost, and the
#               volume mounts, reads and writes afterwards.
#   faults      a write error at every write, and an allocation failure
#               at every allocation, of recorded workloads: the error
#               reaches the caller (fsync / close / sync / unmount), the
#               volume is consistent, VolumeDirty stays set unless
#               everything was stored.
#   regress     regression cases (allocation failures, write errors,
#               full volumes, power cuts), each on its own
#   zerowrite   write trap: mounting read-write and read-only, walking,
#               reading every byte, statfs, sync, fsync of unmodified
#               files, O_RDWR opens without writing, remounts, failing
#               and no-op operations and unmounting write NOTHING, on
#               every image variant (cluster sizes, fragmented, many,
#               deep, big, dirty, backup boot region only, write-protected
#               device, media failure, 4 KB sectors, images laid out the
#               way other systems format them).
#
#   host/test-audit-exfat.sh [SECTION...]     default: all five (a
#                       section may also be one quoted configuration)
#   AUDIT_SEEDS=N    seeds per workload configuration (default 3)
#   AUDIT_OPS=N      operations per workload (default 20000)
#   AUDIT_JOBS=N     parallel harness runs (default 1; run the whole script
#                    inside a memory-capped scope, e.g. systemd-run --user
#                    --scope -p MemoryMax=...)
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
export TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-audit-exfat}
mkdir -p "$TESTFS_DIR" || exit 2
W=$(mktemp -d "$TESTFS_DIR/run.XXXXXX") || exit 2
SEEDS=${AUDIT_SEEDS:-3}
OPS=${AUDIT_OPS:-20000}
JOBS=${AUDIT_JOBS:-1}
PY="python3 -I $ROOT/host/test-audit-exfat.py"
CHECK="$ROOT/host/test-exfat-check.py"

for t in mkfs.exfat fsck.exfat gcc; do
	command -v $t >/dev/null 2>&1 || { echo "test-audit-exfat: $t missing"; exit 77; }
done
B=$(host/test-fs.sh exfat --build | tail -1) || exit 2
[ -x "$B" ] || { echo "test-audit-exfat: harness build failed"; exit 2; }
# a private copy: edits elsewhere in the tree rebuild the harness's binary
BIN="$W/test-fs-exfat"
cp "$B" "$BIN" || exit 2

PASS=0
FAILED=0
FAILS=""
ok() { PASS=$((PASS + 1)); echo "PASS $1"; }
bad() {
	FAILED=$((FAILED + 1))
	FAILS="$FAILS $1"
	echo "FAIL $1"
	[ -f "$W/$1.out" ] && grep -E "FINDING|^FAIL|WRITE TRAP|BUG|WARNING|runtime error|ERROR" "$W/$1.out" | head -20 | sed 's/^/    /'
}

# mkimg FILE SIZE [mkfs.exfat options]
mkimg() {
	local f=$1 size=$2
	shift 2
	rm -f "$f"
	truncate -s "$size" "$f" || return 1
	mkfs.exfat -q -L AUDIT "$@" "$f" >/dev/null 2>&1 || mkfs.exfat -L AUDIT "$@" "$f" >/dev/null || return 1
}

# The volume flags word and PercentInUse of the main boot sector, and the
# allocated share of the cluster heap according to the bitmap.
percent_ok() {
	python3 -I - "$1" <<'PYEOF'
import struct, sys
f = open(sys.argv[1], "rb")
b = f.read(512)
bps = 1 << b[108]; spc = 1 << b[109]
heap, count, root = struct.unpack_from("<III", b, 88)
flags = struct.unpack_from("<H", b, 106)[0]
pct = b[112]
def off(c): return (heap + (c - 2) * spc) * bps
f.seek(off(root)); d = f.read(bps * spc)
for i in range(0, len(d), 32):
    if d[i] == 0x81:
        first, ln = struct.unpack_from("<IQ", d, i + 20)
        break
f.seek(off(first)); bm = f.read((count + 7) // 8)
used = sum(bin(x).count("1") for x in bm)
want = used * 100 // count
if flags & 2:
    sys.exit("VolumeDirty set")
if pct != 0xFF and pct not in (want, (used * 100 + count - 1) // count):
    sys.exit("PercentInUse %d, the bitmap says %d of %d clusters (%d %%)" % (pct, used, count, want))
PYEOF
}

# strict: fsck.exfat -n clean, the checker clean (no leak, not dirty),
# PercentInUse consistent with the bitmap
strict_ok() {
	local img=$1 out=$2
	{ fsck.exfat -n "$img" && python3 -I "$CHECK" "$img" && percent_ok "$img"; } >> "$out" 2>&1 &&
		grep -q "clean" "$out"
}

# A volume left marked dirty (an operation could not finish releasing
# clusters, or a write failed): consistent up to what a check repairs
# without losing a byte -- lost clusters, FAT chains longer than their
# files.  One marked clean: strict_ok.
dirty_flag() {
	python3 -I -c 'import sys,struct;f=open(sys.argv[1],"rb");f.seek(106);print(struct.unpack("<H",f.read(2))[0]&2)' "$1"
}
vol_ok() {
	local img=$1 out=$2
	if [ "$(dirty_flag "$img")" = 0 ]; then
		strict_ok "$img" "$out"
		return
	fi
	fsck.exfat -n "$img" > "$out.fsck" 2>&1
	cat "$out.fsck" >> "$out"
	if grep -q "ERROR" "$out.fsck" && grep "ERROR" "$out.fsck" | grep -vq "more clusters are allocated. truncate to"; then
		return 1
	fi
	python3 -I "$CHECK" "$img" --allow-leaks --allow-dirty >> "$out" 2>&1
}

# ---------------------------------------------------------------------------
# workloads

# wl NAME SIZE "MKFS OPTS" "TOOL OPTS" [to4k]
wl() {
	local name=$1 size=$2 mk=$3 opts=$4 conv=${5:-} img="$W/$1.img"
	if ! mkimg "$img" "$size" $mk; then
		echo "mkfs failed" > "$W/$name.out"
		bad "$name"
		return
	fi
	if [ "$conv" = to4k ] && ! $PY to4k "$img" > "$W/$name.out" 2>&1; then
		bad "$name"
		return
	fi
	mkdir -p "$W/$name.d"
	if $PY run "$BIN" "$img" $opts --tmpdir "$W/$name.d" > "$W/$name.out" 2>&1 &&
	   strict_ok "$img" "$W/$name.out"; then
		ok "$name"
		grep "^audit:" "$W/$name.out" | cut -c1-160 | sed 's/^/    /'
		rm -f "$img"
	else
		bad "$name"
	fi
	rm -rf "$W/$name.d"
}

# run the queued jobs ($@ = function calls as strings) JOBS at a time
parallel() {
	local n=0
	for j in "$@"; do
		eval "$j" > "$W/job.$n.log" 2>&1 &
		n=$((n + 1))
		while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
			wait -n
		done
	done
	wait
	# the jobs ran in subshells: their verdicts are in their logs
	local k=0 v
	while [ $k -lt $n ]; do
		cat "$W/job.$k.log"
		for v in $(sed -n 's/^PASS \([^ ]*\).*/\1/p' "$W/job.$k.log"); do
			PASS=$((PASS + 1))
		done
		for v in $(sed -n 's/^FAIL \([^ ]*\).*/\1/p' "$W/job.$k.log"); do
			FAILED=$((FAILED + 1))
			FAILS="$FAILS $v"
		done
		k=$((k + 1))
	done
}

workloads() {
	local s list=()
	for s in $(seq 1 "$SEEDS"); do
		list+=("wl wl-c512-s$s 96M '-c 512' '--seed $s --ops $OPS --cluster 512'")
		list+=("wl wl-c4k-s$s 256M '-c 4K' '--seed $((s + 100)) --ops $OPS --cluster 4096'")
		list+=("wl wl-c32k-s$s 512M '-c 32K' '--seed $((s + 200)) --ops $OPS --cluster 32768'")
		list+=("wl wl-c128k-s$s 1G '-c 128K' '--seed $((s + 300)) --ops $OPS --cluster 131072'")
		list+=("wl wl-c1m-s$s 4G '-c 1M' '--seed $((s + 400)) --ops $((OPS / 2)) --cluster 1048576'")
		list+=("wl wl-c32m-s$s 8G '-c 32M' '--seed $((s + 500)) --ops $((OPS / 10)) --cluster 33554432 --verify-every 200'")
		# 4096-byte device sectors under 512-byte file-system sectors
		list+=("wl wl-dev4k-c4k-s$s 256M '-c 4K' '--seed $((s + 600)) --ops $OPS --cluster 4096 --sector 4096'")
		# 4096-byte file-system sectors, on 4096- and 512-byte devices
		list+=("wl wl-fs4k-c32k-s$s 512M '-c 32K -b 1M' '--seed $((s + 700)) --ops $OPS --cluster 32768 --sector 4096' to4k")
		list+=("wl wl-fs4k-c4k-dev512-s$s 256M '-c 4K -b 1M' '--seed $((s + 800)) --ops $OPS --cluster 4096 --sector 512' to4k")
		# profiles
		list+=("wl wl-names-s$s 128M '-c 4K' '--seed $((s + 900)) --ops $((OPS / 2)) --profile names'")
		list+=("wl wl-deep-s$s 128M '-c 512' '--seed $((s + 1000)) --ops $((OPS / 2)) --profile deep --cluster 512'")
		list+=("wl wl-holes-s$s 2G '-c 32K' '--seed $((s + 1100)) --ops $((OPS / 4)) --profile holes --cluster 32768'")
		list+=("wl wl-full-c4k-s$s 24M '-c 4K' '--seed $((s + 1200)) --ops $((OPS / 2)) --profile full --maxfile 4000000'")
		list+=("wl wl-full-c512-s$s 8M '-c 512' '--seed $((s + 1300)) --ops $((OPS / 2)) --profile full --cluster 512 --maxfile 1000000'")
		list+=("wl wl-full-c128k-s$s 64M '-c 128K' '--seed $((s + 1400)) --ops $((OPS / 4)) --profile full --cluster 131072 --maxfile 8000000'")
	done
	list+=("wl wl-bigdir-c4k 256M '-c 4K' '--seed 2001 --ops 6000 --profile bigdir --verify-every 2000'")
	list+=("wl wl-bigdir-c512 256M '-c 512' '--seed 2002 --ops 6000 --profile bigdir --cluster 512 --verify-every 2000'")
	list+=("wl wl-bigdir-c128k 512M '-c 128K' '--seed 2003 --ops 6000 --profile bigdir --cluster 131072 --verify-every 2000'")
	list+=("wl wl-big-c128k 7G '-c 128K' '--seed 2004 --ops 400 --profile big --cluster 131072 --verify-every 200'")
	list+=("wl wl-pack-c32k 200M '-c 32K --pack-bitmap' '--seed 2005 --ops $OPS --cluster 32768'")
	parallel "${list[@]}"
}

# ---------------------------------------------------------------------------
# power cuts

SHIM="$W/wj.so"
build_shim() {
	[ -f "$SHIM" ] && return 0
	cat > "$W/wj.c" <<'CEOF'
/* Write journal for the power-cut sweeps: every pwrite and fdatasync on
 * the image named by AUDIT_WJ_IMAGE is appended to AUDIT_WJ_OUT as
 * 'W' off64 len32 data | 'F' 0 0. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int wj_fd = -2;
static dev_t img_dev;
static ino_t img_ino;

static int is_img(int fd)
{
	struct stat st;

	if (wj_fd == -2) {
		const char *o = getenv("AUDIT_WJ_OUT"), *i = getenv("AUDIT_WJ_IMAGE");

		wj_fd = -1;
		if (o && i && !stat(i, &st)) {
			img_dev = st.st_dev;
			img_ino = st.st_ino;
			wj_fd = open(o, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
		}
	}
	if (wj_fd < 0 || fstat(fd, &st))
		return 0;
	return st.st_dev == img_dev && st.st_ino == img_ino;
}

static void wj_put(char t, uint64_t off, uint32_t len, const void *p)
{
	char h[13];

	h[0] = t;
	memcpy(h + 1, &off, 8);
	memcpy(h + 9, &len, 4);
	if (write(wj_fd, h, 13) != 13 || (len && write(wj_fd, p, len) != (ssize_t)len))
		abort();
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	static ssize_t (*real)(int, const void *, size_t, off_t);

	if (!real)
		real = (ssize_t (*)(int, const void *, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
	if (is_img(fd))
		wj_put('W', (uint64_t)off, (uint32_t)n, buf);
	return real(fd, buf, n, off);
}

ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off)
{
	return pwrite(fd, buf, n, off);
}

int fdatasync(int fd)
{
	static int (*real)(int);

	if (!real)
		real = (int (*)(int))dlsym(RTLD_NEXT, "fdatasync");
	if (is_img(fd))
		wj_put('F', 0, 0, NULL);
	return real(fd);
}
CEOF
	gcc -O2 -shared -fPIC -o "$SHIM" "$W/wj.c" -ldl
}

# record NAME BASE TOOLOPTS SECTOR: a workload recorded on a copy of BASE
# (rec, snapshots), then replayed on another copy under the journal shim:
# NAME.wj (journal), NAME.at (write count at every sync), NAME.base (the
# untouched base for the sweeps).  The replay must end with the same
# image as the interactive run.
record() {
	local name=$1 base=$2 opts=$3 sec=$4 d="$W/$1.d"
	mkdir -p "$d"
	cp --sparse=always "$base" "$d/live.img"
	$PY run "$BIN" "$d/live.img" $opts --sector "$sec" --verify-every 0 \
		--record "$d/rec.txt" --snapshots "$d/snaps.json" --tmpdir "$d" > "$W/$name.out" 2>&1 ||
		return 1
	cp --sparse=always "$base" "$d/replay.img"
	AUDIT_WJ_OUT="$d/j.wj" AUDIT_WJ_IMAGE="$d/replay.img" LD_PRELOAD="$SHIM" \
	ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0:allocator_may_return_null=1 \
		timeout 3600 "$BIN" -v --sector "$sec" -s "$d/rec.txt" "$d/replay.img" > "$d/replay.out" 2>&1
	sed -n 's/^counts .* writes=\([0-9]*\) .*/\1/p' "$d/replay.out" > "$d/at"
	cmp -s "$d/live.img" "$d/replay.img" || { echo "replay differs from the recorded run" >> "$W/$name.out"; return 1; }
	cp --sparse=always "$base" "$d/base.img"
	return 0
}

# pc NAME SIZE "MKFS OPTS" "TOOL OPTS" SECTOR MODE STEP [to4k]
pc() {
	build_shim || { bad shim; return; }
	local name=$1 size=$2 mk=$3 opts=$4 sec=$5 mode=$6 step=$7 conv=${8:-} d="$W/$1.d"
	mkdir -p "$d"
	mkimg "$d/b0.img" "$size" $mk || { bad "$name"; return; }
	[ "$conv" = to4k ] && { $PY to4k "$d/b0.img" || { bad "$name"; return; }; }
	# something to lose: a populated volume first
	timeout 600 "$BIN" --sector "$sec" "$d/b0.img" -c "mount; mkdir /keep; write /keep/a pat:1:300000; write /keep/b pat:2:5000; mkdir /keep/sub; write /keep/sub/c pat:3:70000; umount" > "$d/prep.out" 2>&1 ||
		{ cat "$d/prep.out" > "$W/$name.out"; bad "$name"; return; }
	if ! record "$name" "$d/b0.img" "$opts" "$sec"; then
		bad "$name"
		return
	fi
	if $PY sweep "$BIN" "$d/base.img" "$d/j.wj" "$d/snaps.json" "$d/at" --mode "$mode" \
		--step "$step" --samples 3 --sector "$sec" --tmpdir "$d" --keep >> "$W/$name.out" 2>&1; then
		ok "$name"
		grep "^sweep" "$W/$name.out" | sed 's/^/    /'
		rm -rf "$d"
	else
		bad "$name"
		grep "^sweep" "$W/$name.out" | sed 's/^/    /'
	fi
}

powercut() {
	build_shim || { bad shim; return; }
	local list=() m
	for m in prefix reorder; do
		# 512-byte clusters: a request per cluster, so fewer
		# operations give as many cut points
		list+=("pc pc-$m-c512 32M '-c 512' '--seed 11 --ops 80 --cluster 512' 512 $m 1")
		list+=("pc pc-$m-c4k 64M '-c 4K' '--seed 12 --ops 300 --cluster 4096' 512 $m 1")
		list+=("pc pc-$m-c32k 128M '-c 32K' '--seed 13 --ops 300 --cluster 32768' 512 $m 1")
		list+=("pc pc-$m-c128k 256M '-c 128K' '--seed 14 --ops 250 --cluster 131072' 512 $m 1")
		list+=("pc pc-$m-c1m 1G '-c 1M' '--seed 15 --ops 150 --cluster 1048576' 512 $m 1")
		# 32 MB clusters: a new cluster is zeroed in hundreds of 64 KB
		# writes (equivalent cut points, nothing references it yet),
		# and every check is slow: every 8th write is a cut point
		list+=("pc pc-$m-c32m 2G '-c 32M' '--seed 16 --ops 60 --cluster 33554432' 512 $m 8")
		list+=("pc pc-$m-dev4k 64M '-c 4K' '--seed 17 --ops 300 --cluster 4096' 4096 $m 1")
		list+=("pc pc-$m-fs4k 128M '-c 32K -b 1M' '--seed 18 --ops 300 --cluster 32768' 4096 $m 1 to4k")
		list+=("pc pc-$m-full 8M '-c 4K' '--seed 19 --ops 300 --profile full --maxfile 2000000' 512 $m 1")
		list+=("pc pc-$m-names 64M '-c 4K' '--seed 20 --ops 250 --profile names' 512 $m 1")
	done
	parallel "${list[@]}"
}

# ---------------------------------------------------------------------------
# fault injection

# fault NAME SIZE "MKFS OPTS" "TOOL OPTS" KIND(write|kalloc) STEP
# A recorded workload, replayed with one failure injected after the mount:
# the Nth device write fails (once), or the Nth allocation does -- for
# every N the workload reaches (STEP > 1: every STEP-th).  Each run: no
# crash / sanitizer report / hang, the volume consistent (strictly when
# it was marked clean, up to lost clusters when it was left dirty), and a
# run in which no command reports an error has the baseline run's tree.
fault() {
	local name=$1 size=$2 mk=$3 opts=$4 kind=$5 step=$6 d="$W/$1.d"
	local n total failed=0 runs=0 clean=0 dirty=0 seen=0 st
	mkdir -p "$d"
	mkimg "$d/b0.img" "$size" $mk || { bad "$name"; return; }
	timeout 600 "$BIN" "$d/b0.img" -c "mount; mkdir /keep; write /keep/a pat:1:200000; umount" > "$d/prep.out" 2>&1 || { bad "$name"; return; }
	cp --sparse=always "$d/b0.img" "$d/live.img"
	$PY run "$BIN" "$d/live.img" $opts --verify-every 0 --record "$d/rec.txt" --tmpdir "$d" > "$W/$name.out" 2>&1 ||
		{ bad "$name"; return; }
	# the baseline run: the same lines (a no-op where the fault goes)
	sed "1a ?counts" "$d/rec.txt" > "$d/rec2.txt"
	echo "?counts" >> "$d/rec2.txt"
	cp --sparse=always "$d/b0.img" "$d/ref.img"
	timeout 1800 "$BIN" -v --sector 512 -s "$d/rec2.txt" "$d/ref.img" > "$d/ref.out" 2>&1
	grep '^= ' "$d/ref.out" | head -n -1 > "$d/ref.st"
	timeout 600 "$BIN" --cow "$d/ref.img" -c "mount ro; manifest /; umount" 2>/dev/null | grep -E '^[dl] |^f .* [0-9a-f]{64}$' > "$d/ref.man"
	if [ "$kind" = write ]; then
		total=$(sed -n 's/^counts .* writes=\([0-9]*\) .*/\1/p' "$d/ref.out" | tail -1)
	else
		# allocations the replay makes after the mount (the debugger
		# reads the harness's counter at the end of each)
		cp --sparse=always "$d/b0.img" "$d/cnt.img"
		head -1 "$d/rec.txt" > "$d/cnt1.txt"
		local a0 a1
		a0=$(ASAN_OPTIONS=detect_leaks=0 timeout 600 gdb -batch -ex 'break _exit' -ex run -ex 'print test_kalloc_count' \
			--args "$BIN" --sector 512 -s "$d/cnt1.txt" "$d/cnt.img" 2>/dev/null | sed -n 's/^\$1 = //p')
		cp --sparse=always "$d/b0.img" "$d/cnt.img"
		a1=$(ASAN_OPTIONS=detect_leaks=0 timeout 1800 gdb -batch -ex 'break _exit' -ex run -ex 'print test_kalloc_count' \
			--args "$BIN" --sector 512 -s "$d/rec.txt" "$d/cnt.img" 2>/dev/null | sed -n 's/^\$1 = //p')
		total=$(( ${a1:-0} - ${a0:-0} ))
	fi
	echo "fault: $kind: ${total:-?} fault points" >> "$W/$name.out"
	n=1
	while [ "$n" -le "${total:-0}" ]; do
		cp --sparse=always "$d/b0.img" "$d/f.img"
		if [ "$kind" = write ]; then
			sed "1a ?fault write $n" "$d/rec.txt" > "$d/frec.txt"
		else
			sed "1a ?kfault $n" "$d/rec.txt" > "$d/frec.txt"
		fi
		timeout 1800 "$BIN" -v --allow-warn --sector 512 -s "$d/frec.txt" "$d/f.img" > "$d/f.out" 2>&1
		st=$?
		runs=$((runs + 1))
		if [ $st -ge 2 ]; then
			echo "FINDING: $kind fault $n: runner status $st" >> "$W/$name.out"
			grep -E "ERROR|BUG|runtime error|DEADLOCK|FATAL|FAIL" "$d/f.out" | head -5 >> "$W/$name.out"
			cp "$d/f.out" "$d/finding-$n.out"
			failed=$((failed + 1))
		fi
		grep '^= ' "$d/f.out" > "$d/f.st"
		local changed=1
		cmp -s "$d/f.st" "$d/ref.st" && changed=0
		[ $changed = 1 ] && seen=$((seen + 1))
		if [ "$(dirty_flag "$d/f.img")" != 0 ]; then
			dirty=$((dirty + 1))
		else
			clean=$((clean + 1))
		fi
		if ! vol_ok "$d/f.img" "$d/fsck.out"; then
			echo "FINDING: $kind fault $n: volume inconsistent (VolumeDirty $(dirty_flag "$d/f.img")): $(grep -E "ERROR|error" "$d/fsck.out" | tr '\n' ' ' | cut -c1-400)" >> "$W/$name.out"
			cp "$d/f.img" "$d/finding-$n.img"
			failed=$((failed + 1))
		fi
		rm -f "$d/fsck.out" "$d/fsck.out.fsck"
		# nothing reported: then nothing may be missing either
		if [ $changed = 0 ] && ! cmp -s <(timeout 600 "$BIN" --cow "$d/f.img" -c "mount ro; manifest /; umount" 2>/dev/null | grep -E '^[dl] |^f .* [0-9a-f]{64}$') "$d/ref.man"; then
			echo "FINDING: $kind fault $n: no error reported, but the tree differs from the baseline run" >> "$W/$name.out"
			cp "$d/f.img" "$d/finding-$n.img"
			failed=$((failed + 1))
		fi
		[ $failed -gt 20 ] && break
		n=$((n + step))
	done
	echo "fault: $kind: $runs runs, $seen with an error reported, $clean left clean, $dirty left dirty, $failed finding(s)" >> "$W/$name.out"
	if [ $failed = 0 ] && [ "$runs" -gt 0 ]; then
		ok "$name"
		rm -rf "$d"
	else
		bad "$name"
	fi
	grep "^fault:" "$W/$name.out" | sed 's/^/    /'
}

faults() {
	local list=()
	list+=("fault fw-c4k 64M '-c 4K' '--seed 31 --ops 300' write 1")
	# 512-byte clusters write far more (every cluster a request): fewer
	# operations, every write still a fault point
	list+=("fault fw-c512 32M '-c 512' '--seed 32 --ops 70 --cluster 512' write 1")
	list+=("fault fw-c128k 256M '-c 128K' '--seed 33 --ops 200 --cluster 131072' write 1")
	list+=("fault fk-c4k 64M '-c 4K' '--seed 34 --ops 150' kalloc 1")
	list+=("fault fk-c512 32M '-c 512' '--seed 35 --ops 100 --cluster 512' kalloc 3")
	parallel "${list[@]}"
}

# ---------------------------------------------------------------------------
# regression cases

# st OUT N: the status of the Nth "?"-prefixed command of a -v transcript
# (only those print one)
st() { grep '^= ' "$1" | sed -n "${2}p" | cut -c3-; }

regress() {
	local img="$W/rg.img" n s1 s2 s3 out fails
	# an allocation failure in unlink: all or nothing (the name stays
	# with every byte, or it goes with its clusters) -- never the name
	# removed and the clusters kept with NOMEM reported
	mkimg "$img" 32M -c 4K && timeout 600 "$BIN" "$img" -c "mount; mkdir /d; write /d/a pat:1:200000; write /d/b pat:2:3000; umount" > /dev/null 2>&1 ||
		{ bad rg-prep; return; }
	fails=0
	out="$W/rg-unlink-nomem.out"
	: > "$out"
	for n in $(seq 1 40); do
		cp "$img" "$W/rg1.img"
		timeout 600 "$BIN" -v --allow-warn "$W/rg1.img" -c "mount; ?kfault $n; ?unlink /d/a; ?kfault 0; ?stat /d/a; ?check /d/a pat:1:200000; umount" > "$out.$n" 2>&1
		s1=$(st "$out.$n" 2); s2=$(st "$out.$n" 4); s3=$(st "$out.$n" 5)
		if ! { [ "$s1" = OK ] && [ "$s2" = NOT_FOUND ]; } && ! { [ "$s1" != OK ] && [ "$s2" = OK ] && [ "$s3" = OK ]; }; then
			echo "kfault $n: unlink $s1, stat $s2, content $s3" >> "$out"
			fails=$((fails + 1))
		elif ! vol_ok "$W/rg1.img" "$out.$n"; then
			echo "kfault $n: volume inconsistent" >> "$out"
			fails=$((fails + 1))
		fi
	done
	[ $fails = 0 ] && ok rg-unlink-nomem || { head "$out"; bad rg-unlink-nomem; }

	# an allocation failure in a shrinking truncate of a FAT-chained
	# file: all or nothing
	mkimg "$img" 32M -c 4K && timeout 600 "$BIN" "$img" -c "mount; mkdir /d; $(for i in 1 2 3 4 5 6; do echo -n "append /d/a pat:$i:5000; append /d/b pat:$i:5000; "; done) umount" > /dev/null 2>&1 ||
		{ bad rg-prep2; return; }
	timeout 600 "$BIN" --cow "$img" -c "mount ro; sha /d/a; umount" 2>/dev/null | grep '^sha ' > "$W/rg-a.sha"
	fails=0
	out="$W/rg-truncate-nomem.out"
	: > "$out"
	for n in $(seq 1 40); do
		cp "$img" "$W/rg1.img"
		timeout 600 "$BIN" -v --allow-warn "$W/rg1.img" -c "mount; ?kfault $n; ?truncate /d/a 7000; ?kfault 0; ?sha /d/a; umount" > "$out.$n" 2>&1
		s1=$(st "$out.$n" 2)
		local got
		got=$(grep '^sha ' "$out.$n" | cut -d' ' -f3)
		if [ "$s1" = OK ]; then
			[ "$got" = 7000 ] || { echo "kfault $n: truncate OK, size $got" >> "$out"; fails=$((fails + 1)); }
		elif ! cmp -s <(grep '^sha ' "$out.$n") "$W/rg-a.sha"; then
			echo "kfault $n: truncate $s1, but the file changed" >> "$out"
			fails=$((fails + 1))
		fi
		vol_ok "$W/rg1.img" "$out.$n" || { echo "kfault $n: volume inconsistent" >> "$out"; fails=$((fails + 1)); }
	done
	[ $fails = 0 ] && ok rg-truncate-nomem || { head "$out"; bad rg-truncate-nomem; }

	# a write error in a commit that frees clusters: they stay lost --
	# and the volume must stay marked dirty, so a check finds them (the
	# unmount must not mark it clean with lost clusters on it)
	mkimg "$img" 32M -c 4K && timeout 600 "$BIN" "$img" -c "mount; mkdir /d; write /d/a pat:1:200000; write /d/b pat:2:3000; umount" > /dev/null 2>&1 ||
		{ bad rg-prep3; return; }
	fails=0
	out="$W/rg-werr-dirty.out"
	: > "$out"
	for n in $(seq 1 12); do
		cp "$img" "$W/rg1.img"
		timeout 600 "$BIN" -v --allow-warn "$W/rg1.img" -c "mount; ?fault write $n; ?unlink /d/a; ?write /d/c pat:3:9000; ?sync; ?umount" > "$out.$n" 2>&1
		if grep -q "io_errors=[1-9]" "$out.$n"; then
			# a write failed: marked clean only when everything is
			# stored and nothing lost (strictly consistent), else
			# dirty and consistent up to lost clusters
			vol_ok "$W/rg1.img" "$out.$n" || { echo "fault $n: inconsistent, or lost clusters on a volume marked clean (VolumeDirty $(dirty_flag "$W/rg1.img"))" >> "$out"; fails=$((fails + 1)); }
		else
			strict_ok "$W/rg1.img" "$out.$n" || { echo "fault $n: no write failed, but not clean" >> "$out"; fails=$((fails + 1)); }
		fi
	done
	[ $fails = 0 ] && ok rg-werr-dirty || { head "$out"; bad rg-werr-dirty; }

	# rename on a full volume into a full directory: to a new name it
	# fails and changes nothing, over an existing name it succeeds (the
	# victim's slots take the new set)
	mkimg "$img" 8M -c 4K || { bad rg-prep4; return; }
	out="$W/rg-rename-full.out"
	if timeout 600 "$BIN" --allow-warn "$img" -c "mount; mkdir /t; $(for i in $(seq 1 40); do echo -n "write /t/file$i text:$i; "; done) write /src pat:5:7000; ?write /fill pat:1:9000000; =NOSPC create /t/newname; =NOSPC rename /src /t/othername; check /src pat:5:7000; check /t/file5 text:5; rename /src /t/FILE5; =NOT_FOUND stat /src; check /t/FILE5 pat:5:7000; cycle; check /t/file5 pat:5:7000; umount" > "$out" 2>&1 &&
	   strict_ok "$img" "$out"; then
		ok rg-rename-full
	else
		bad rg-rename-full
	fi

	# a read-only remount while an unlinked file is still open (read
	# only): refused -- its clusters are freed at its last close, which a
	# read-only mount could not store (they would stay allocated on a
	# volume marked clean); also when the handle wrote after the unlink
	mkimg "$img" 32M -c 4K || { bad rg-prep5; return; }
	out="$W/rg-orphan-remount.out"
	if timeout 600 "$BIN" "$img" -c "mount; write /o pat:1:90000; write /p text:1; fopen 1 /o r; unlink /o; =BUSY remount ro; fopen 2 /p +; unlink /p; fpwrite 2 0 pat:2:50000; fclose 1; =BUSY remount ro; fclose 2; remount ro; =NOT_FOUND stat /o; remount rw; umount" > "$out" 2>&1 &&
	   strict_ok "$img" "$out"; then
		ok rg-orphan-remount
	else
		bad rg-orphan-remount
	fi

	# a write error while a rename moves a file to another directory:
	# the file keeps one of its names, with all its bytes (the old set,
	# cleared in the cache, must not stay cleared when the commit that was
	# to store that failed: the file would lose its name)
	mkimg "$img" 32M -c 4K && timeout 600 "$BIN" "$img" -c "mount; mkdir /a; mkdir /b; write /a/x text:1; write /a/f pat:1:20000; umount" > /dev/null 2>&1 ||
		{ bad rg-prep6; return; }
	fails=0
	out="$W/rg-rename-werr.out"
	: > "$out"
	for n in $(seq 1 10); do
		cp "$img" "$W/rg1.img"
		timeout 600 "$BIN" -v --allow-warn "$W/rg1.img" -c "mount; ?fault write $n; ?rename /a/f /b/g; ?sync; ?cycle; ?check /a/f pat:1:20000; ?check /b/g pat:1:20000; ?umount" > "$out.$n" 2>&1
		s1=$(st "$out.$n" 5); s2=$(st "$out.$n" 6)
		if ! { [ "$s1" = OK ] && [ "$s2" = NOT_FOUND ]; } && ! { [ "$s1" = NOT_FOUND ] && [ "$s2" = OK ]; }; then
			echo "fault $n: /a/f $s1, /b/g $s2" >> "$out"
			fails=$((fails + 1))
		fi
		vol_ok "$W/rg1.img" "$out.$n" || { echo "fault $n: volume inconsistent" >> "$out"; fails=$((fails + 1)); }
	done
	[ $fails = 0 ] && ok rg-rename-werr || { head "$out"; bad rg-rename-werr; }

	# the root directory grows (512-byte clusters, its cluster full):
	# a power cut after every write of that must leave the root's FAT
	# chain whole -- it is the root's length (the new clusters' own
	# entries reach the device before the link into them, else fsck says
	# "the cluster chain of root is broken")
	# (a file after the root's cluster first: the root's next cluster
	# lies far from its last one, their FAT entries in different sectors)
	mkimg "$img" 32M -c 512 && timeout 600 "$BIN" "$img" -c "mount; write /big pat:1:600000; umount" > /dev/null 2>&1 ||
		{ bad rg-prep7; return; }
	fails=0
	out="$W/rg-root-grow-cut.out"
	: > "$out"
	local grow
	for n in $(seq 1 60); do
		grow="mount; fault cut $n; $(for i in $(seq 1 12); do echo -n "write /r$i text:$i; "; done) sync"
		cp "$img" "$W/rg1.img"
		timeout 600 "$BIN" --allow-warn "$W/rg1.img" -c "$grow; ?umount" > "$out.$n" 2>&1
		fsck.exfat -n "$W/rg1.img" > "$out.$n.fsck" 2>&1
		if grep -q "ERROR" "$out.$n.fsck" && grep "ERROR" "$out.$n.fsck" | grep -vq "more clusters are allocated. truncate to"; then
			echo "cut $n: $(grep ERROR "$out.$n.fsck" | head -1)" >> "$out"
			fails=$((fails + 1))
		elif ! python3 -I "$CHECK" "$W/rg1.img" --allow-leaks --allow-dirty > "$out.$n.chk" 2>&1; then
			echo "cut $n: $(tail -1 "$out.$n.chk")" >> "$out"
			fails=$((fails + 1))
		fi
	done
	[ $fails = 0 ] && ok rg-root-grow-cut || { head "$out"; bad rg-root-grow-cut; }
	rm -f "$img" "$W"/rg1.img "$W"/rg-*.out.*

	# files that grow on a nearly full volume, a power cut after every
	# write: a chain past a file's length (its entry set not stored yet)
	# must never run, through stale entries of clusters freed earlier,
	# into other files' clusters or free ones (the new clusters' own
	# entries reach the device before the link into them)
	pc rg-link-order 8M '-c 4K' '--seed 19 --ops 300 --profile full --maxfile 2000000' 512 prefix 1
}

# ---------------------------------------------------------------------------
# zero writes

# the operations that must not write: everything that only reads, and
# operations that change nothing or fail
ZW_READ="readall; ls /; statfs; options; sync; readall"
zw_noop() {
	cat <<EOF
mount rw
readall
statfs
sync
fsync /d/f
fsync /g
fopen 1 /g +
fread 1 100
fpread 1 0 50
ffsync 1
fstat 1
fclose 1
fopen 2 /d/f r
fclose 2
fopen 3 /g wc
fclose 3
fopen 4 /g +a
fclose 4
fopen 5 /empty wt
fclose 5
fopen 6 /d r
fclose 6
rename /g /g
=NOTEMPTY rename /e /d
=ISDIR rename /g /d
=NOTDIR rename /d /g
=INVALID rename /d /d/sub
=NOT_FOUND rename /nothing /x
=NOT_FOUND rename /nothing /g
utimens /g omit omit
truncate /empty 0
truncate /g 6
=EXISTS mkdir /d
=EXISTS mkdir /D
=EXISTS create /G
=NOTEMPTY rmdir /d
=NOTDIR rmdir /g
=ISDIR unlink /d
=NOT_FOUND unlink /none
=NOT_FOUND rmdir /none
=INVALID create /a:b
=INVALID mkdir /a*b
=NOT_FOUND stat /none/x
=NOTDIR stat /g/x
=NOTDIR create /g/x
chmod /g 0755
?link /g /g2
?symlink /g /s
?mknod /fifo fifo
?setxattr /g user.x text:1
?rmxattr /g user.x
?getxattr /g user.x
?listxattr /g
?chown /g 0 0
remount ro
remount rw
cycle
readall
tick 60000
wb
shrink 1000 dirty
dropcaches
sync
umount
mount ro
readall
umount
EOF
}

# zw NAME IMAGE [runner args]: the whole list under the write trap
zw() {
	local name=$1 img=$2
	shift 2
	zw_noop > "$W/$name.script"
	if timeout 900 "$BIN" --trap "$@" -s "$W/$name.script" "$img" > "$W/$name.out" 2>&1; then
		ok "$name"
	else
		bad "$name"
	fi
}

# zw_ro NAME IMAGE [runner args]: images that must be (or become)
# read-only: any mount, any operation, never a write
zw_ro() {
	local name=$1 img=$2 force=
	shift 2
	# `force' lifts the dirty gate only -- for the other reasons it must
	# change nothing either
	[ "${ZW_FORCE:-1}" = 1 ] && force="?remount rw force; ?sync; ?umount; ?mount rw force; ?readall; ?mkdir /y; ?umount; "
	if timeout 900 "$BIN" --trap --cow "$@" "$img" -c "?mount rw; ?readall; ?statfs; ?sync; ?mkdir /x; ?write /g text:zz; ?unlink /g; ?rename /d /d2; ?truncate /g 0; ?utimens /g now now; ?remount rw; ?sync; ?umount; ${force}mount ro; readall; umount" \
		> "$W/$name.out" 2>&1; then
		ok "$name"
	else
		bad "$name"
	fi
}

zw_prep() { # IMAGE [runner args]: the files zw_noop expects
	local img=$1
	shift
	timeout 600 "$BIN" "$@" "$img" -c "mount; mkdir /d; write /d/f pat:1:200000; mkdir /d/sub; write /d/sub/x text:1; mkdir /e; write /g text:abcdef; create /empty; umount" > /dev/null 2>&1
}

zerowrite() {
	local c img v
	for c in 512 4K 32K 128K 1M 32M; do
		img="$W/zw$c.img"
		local size=128M
		[ $c = 1M ] && size=512M
		[ $c = 32M ] && size=2G
		mkimg "$img" $size -c $c || { bad "zw-mkfs-$c"; continue; }
		zw_prep "$img" || { bad "zw-prep-$c"; continue; }
		zw "zw-c$c" "$img"
		[ $c = 4K ] && zw "zw-c$c-dev4k" "$img" --sector 4096
		rm -f "$img"
	done
	# 4 KB file-system sectors
	img="$W/zw4kfs.img"
	mkimg "$img" 256M -c 32K -b 1M && $PY to4k "$img" && zw_prep "$img" --sector 4096 &&
		zw zw-fs4k "$img" --sector 4096 && zw zw-fs4k-dev512 "$img" --sector 512
	# the harness's populated images (FAT-chained fragments, many, deep, big)
	for v in exfat-4k-basic exfat-4k-frag exfat-4k-many exfat-128k-many exfat-4k-deep exfat-4k-big exfat-32m-basic; do
		img="$W/$v.img"
		if bash host/fsimg/mkimg.sh "$v" "$img" > /dev/null 2>&1; then
			timeout 900 "$BIN" --trap "$img" -c "mount rw; $ZW_READ; remount ro; remount rw; cycle; readall; umount; mount ro; readall; umount" > "$W/zw-$v.out" 2>&1 &&
				ok "zw-$v" || bad "zw-$v"
			zw_ro "zw-$v-rodev" "$img" --ro-dev
			rm -f "$img" "$img.manifest"
		else
			echo "SKIP zw-$v (image)"
		fi
	done
	# laid out the way other systems format: 32 KB clusters, FAT and heap
	# on 1 MB boundaries, a volume GUID, the label, a populated tree with
	# FAT-chained files; also 128 KB clusters on a larger volume
	for spec in "512M -c 32K -b 1M -U 2f1a6e43-5e39-4c8f-9a51-1b2c3d4e5f60" "2G -c 128K -b 4M -U 11111111-2222-3333-4444-555555555555" "64M -c 4K -b 1M"; do
		img="$W/wl.img"
		mkimg "$img" $spec || { bad "zw-other-mkfs"; continue; }
		rm -rf "$W/wltree"
		python3 -I host/fsimg/mktree.py tree basic 77 "$W/wltree" --no-symlinks --no-hardlinks >/dev/null 2>&1
		python3 -I host/fsimg/exfat-populate.py "$img" "$W/wltree" --fragment > /dev/null 2>&1 || { bad "zw-other-populate"; continue; }
		python3 -I host/fsimg/mktree.py manifest "$W/wltree" --no-symlinks > "$W/wl.manifest"
		timeout 900 "$BIN" --trap "$img" -c "mount rw; manifest /; $ZW_READ; remount ro; remount rw; cycle; readall; umount" > "$W/zw-other.out" 2>&1
		local st=$?
		grep -E '^[dl] |^f .* [0-9a-f]{64}$' "$W/zw-other.out" > "$W/zw-other.man"
		if [ $st = 0 ] && python3 -I host/fsimg/mktree.py compare "$W/wl.manifest" "$W/zw-other.man" >> "$W/zw-other.out" 2>&1; then
			ok "zw-other-$(echo $spec | tr ' ' '_' | cut -c1-24)"
		else
			bad "zw-other-$(echo $spec | tr ' ' '_' | cut -c1-24)"
		fi
		rm -f "$img"
	done
	# dirty, backup boot region only, write-protected device, media failure,
	# own clusters marked free, corrupt entry set
	img="$W/zwg.img"
	mkimg "$img" 64M -c 4K && zw_prep "$img" || { bad zw-gate-prep; return; }
	cp "$img" "$W/zwg-dirty.img"
	python3 -I host/fsimg/fsparse.py exfat-tweak "$W/zwg-dirty.img" dirty
	ZW_FORCE=0 zw_ro zw-dirty "$W/zwg-dirty.img"
	# force on a dirty volume: writes are allowed, and it stays dirty
	cp "$W/zwg-dirty.img" "$W/zwg-dirty2.img"
	if timeout 900 "$BIN" "$W/zwg-dirty2.img" -c "mount rw force; mkdir /x; write /x/f pat:1:9000; umount" > "$W/zw-dirty-force.out" 2>&1 &&
	   python3 -I "$CHECK" "$W/zwg-dirty2.img" --allow-dirty >> "$W/zw-dirty-force.out" 2>&1 &&
	   grep -q "VolumeDirty set" "$W/zw-dirty-force.out"; then
		ok zw-dirty-force-stays-dirty
	else
		bad zw-dirty-force-stays-dirty
	fi
	cp "$img" "$W/zwg-bad.img"
	python3 -I host/fsimg/fsparse.py exfat-tweak "$W/zwg-bad.img" bad-boot-checksum
	zw_ro zw-backup-boot-only "$W/zwg-bad.img"
	zw_ro zw-rodev "$img" --ro-dev
	cp "$img" "$W/zwg-mf.img"
	python3 -I -c '
import sys, struct
f = open(sys.argv[1], "r+b"); f.seek(106)
v = struct.unpack("<H", f.read(2))[0] | 4
f.seek(106); f.write(struct.pack("<H", v))' "$W/zwg-mf.img"
	zw_ro zw-media-failure "$W/zwg-mf.img"
	# a corrupt entry set met while browsing a read-write mount: errors=
	# remount-ro latches read-only, nothing is written to record it
	cp "$img" "$W/zwg-cs.img"
	python3 -I - "$W/zwg-cs.img" <<'PYEOF'
import sys, struct
f = open(sys.argv[1], "r+b"); b = f.read(512)
bps = 1 << b[108]; spc = 1 << b[109]
heap = struct.unpack_from("<I", b, 88)[0]; root = struct.unpack_from("<I", b, 96)[0]
o = (heap + (root - 2) * spc) * bps
f.seek(o); d = f.read(bps * spc)
for i in range(0, len(d), 32):
    if d[i] == 0x85:
        f.seek(o + i + 2); f.write(bytes([d[i + 2] ^ 0x5A]))
        break
PYEOF
	zw_ro zw-corrupt-set "$W/zwg-cs.img" --allow-warn
	timeout 900 "$BIN" --trap --cow --allow-warn "$W/zwg-cs.img" -c "mount rw; ?readall; ?ls /; =ROFS mkdir /x; =ROFS write /new text:1; ?sync; umount" > "$W/zw-corrupt-set-rw-mount.out" 2>&1 &&
		ok zw-corrupt-set-rw-mount || bad zw-corrupt-set-rw-mount
}

# a section name, or one configuration as a quoted command line, e.g.
#   host/test-audit-exfat.sh "pc pc-prefix-c4k 64M '-c 4K' '--seed 12 --ops 300' 512 prefix 1"
[ $# -gt 0 ] || set -- regress zerowrite workloads faults powercut
for t in "$@"; do
	eval "$t"
done
echo "test-audit-exfat: $PASS passed, $FAILED failed"
[ -n "$FAILS" ] && echo "failed:$FAILS (outputs in $W)"
[ "$FAILED" = 0 ]
