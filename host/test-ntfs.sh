#!/bin/bash
# Suite of the NTFS driver (kernel/fs/ntfs/), on the shared
# filesystem harness (host/test-fs.sh, host/fsimg).  What it proves:
#
#   read side:
#   - every image variant reads exactly as ntfs-3g's own tools read it
#     (manifest: names, types, sizes, contents, link targets; oracle.py);
#   - mounting read-only AND read-write, walking, reading everything,
#     statfs and unmounting never writes (write-trap mode), on clean,
#     dirty, hibernated, unclean-$LogFile and $MFTMirr-mismatch volumes;
#   - alternate data streams, xattrs, 4096-byte device sectors, the
#     host unit test of internal helpers (host/test-ntfs.c);
#   - the metadata fuzzer finds no crash, hang, sanitizer report or write.
#
#   write side:
#   - write scripts (files, directories, truncate, rename, links,
#     symlinks, xattrs, modes, times, open-unlinked files) and randomized
#     stress with a model compared after every remount, on 512-byte,
#     4 KB and 64 KB clusters and on 4096-byte device sectors, each
#     followed by `ntfsfix -n', the strict checker host/test-ntfs-check.py
#     (cluster / record / index consistency, ERROR = corruption) and a
#     content comparison against what ntfs-3g reads back;
#   - large directories (index splits, push-down, merges), fragmented
#     files (attribute lists, extension records), $MFT growth, ENOSPC;
#   - the read-write gate (force overrides only the dirty flag), remount
#     ro <-> rw, a clean unmount leaves the volume clean, an idle
#     read-write mount of a written volume writes nothing;
#   - power-cut sweeps: every write prefix of a workload leaves a volume
#     the checker finds free of errors (leaks allowed) that mounts and
#     reads back;
#   - read-write metadata fuzzing.
#
#   host/test-ntfs.sh             read + write suites (no fuzzing)
#   host/test-ntfs.sh all         everything, fuzzing included
#   host/test-ntfs.sh unit        only the unit test of host/test-ntfs.c
#   host/test-ntfs.sh fuzz N      only the fuzz cases, N iterations each
#   host/test-ntfs.sh write       only the write suite
#   host/test-ntfs.sh powercut    only the power-cut sweeps
#   host/test-ntfs.sh n3          only the journal and feature cases (log
#                                 replay, clean log, label, security,
#                                 compression, streams, WOF, system-file
#                                 protection)
#   host/test-ntfs.sh regress     only the write-safety regression cases
#                                 (also part of the default run)
#   host/test-ntfs.sh audit       power cut after every write and failure
#                                 at every allocation / write of several
#                                 workloads (long)
#   host/test-ntfs.sh audit-long  20000-operation randomized workloads per
#                                 geometry and seed, fuzzing (very long)
#
# Environment as host/test-fs.sh (TESTFS_DIR, TESTFS_TIMEOUT, TESTFS_MEM);
# TF_ONLY=REGEX runs only the cases whose name matches;
# NTFS_IMGDIR keeps generated images between runs.  Every run is bounded
# (timeout + systemd-run --user --scope -p MemoryMax).
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-fs}
TESTFS_TIMEOUT=${TESTFS_TIMEOUT:-600}
TESTFS_MEM=${TESTFS_MEM:-2G}
export TESTFS_DIR TESTFS_TIMEOUT TESTFS_MEM
MODE=${1:-default}
FUZZ_N=${2:-}

BIN=$(host/test-fs.sh ntfs --build) || { echo "test-ntfs: build failed" >&2; exit 2; }

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:handle_abort=1:handle_sigill=1:allocator_may_return_null=1:exitcode=4:detect_stack_use_after_return=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1:exitcode=5}

tf_bin() {
	if command -v systemd-run >/dev/null 2>&1 &&
	   systemd-run --user --scope -q -p MemoryMax=1M true >/dev/null 2>&1; then
		timeout -k 10 "$TESTFS_TIMEOUT" systemd-run --user --scope -q \
			-p MemoryMax="$TESTFS_MEM" -p MemorySwapMax=0 -- \
			nice -n 10 "$BIN" "$@"
	else
		timeout -k 10 "$TESTFS_TIMEOUT" nice -n 10 "$BIN" "$@"
	fi
}

TF_TMP=$(mktemp -d "$TESTFS_DIR/run-ntfs-read.XXXXXX") || exit 2
trap 'rm -rf "$TF_TMP"' EXIT
TF_PASS=0
TF_FAILED=0
TF_SKIP=0
TF_FAILS=""
. host/fsimg/common.sh
if [ -n "${NTFS_IMGDIR:-}" ]; then
	TF_IMGDIR=$NTFS_IMGDIR
	mkdir -p "$TF_IMGDIR"
fi

tf_case() {
	local name=$1
	shift
	# TF_ONLY=REGEX: run only the cases whose name matches
	if [ -n "${TF_ONLY:-}" ] && ! [[ $name =~ $TF_ONLY ]]; then
		return 0
	fi
	if "$@" > "$TF_TMP/$name.out" 2>&1; then
		TF_PASS=$((TF_PASS + 1))
		echo "PASS $name"
	else
		local rc=$?
		TF_FAILED=$((TF_FAILED + 1))
		TF_FAILS="$TF_FAILS $name"
		echo "FAIL $name (status $rc)"
		grep -E '^FAIL|ERROR|WRITE TRAP|FATAL|BUG|DEADLOCK|runtime error|WARNING|^-|^\+|^test-fs' \
			"$TF_TMP/$name.out" | head -25 | sed 's/^/    /'
		cp "$TF_TMP/$name.out" "$TESTFS_DIR/last-fail-ntfs-read-$name.out" 2>/dev/null
	fi
}

tf_skip() {
	TF_SKIP=$((TF_SKIP + 1))
	echo "SKIP $1: $2"
}

# ---- unit test of internal helpers (host/test-ntfs.c) ----
ntfs_unit() {
	# Built like a kernel unit of the harness (prelude, sanitizers) and
	# linked with the harness objects of the last build, minus the runner
	# and the driver objects (test-ntfs.c includes the driver's .c files).
	local b u="$TESTFS_DIR/test-ntfs-unit" objs="" f
	b=$(dirname "$BIN")
	local san="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
	timeout 600 gcc -std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding \
		-nostdinc -fno-builtin -I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ \
		-DCRASH_VERBOSE=1 -DDEBUG=1 -Wall -Wextra -Wno-unused-function \
		-Wno-sign-compare -Werror=implicit-function-declaration $san \
		-include host/test-fs-prelude.h -c host/test-ntfs.c \
		-o "$u.o" || return 1
	# every harness object but the runner and the driver itself
	for f in "$b"/*.o; do
		case $(basename "$f") in
		test-fs-main.o | ntfs_*.o) ;;
		*) objs="$objs $f" ;;
		esac
	done
	timeout 600 gcc $san -o "$u" "$u.o" $objs || return 1
	timeout -k 10 300 "$u"
}

# ---- cases ----
ntfs_ads() {
	local img=$1 want got
	want=$(ntfscat -a 0x80 -n stream.one "$img" /extra/plain 2>/dev/null | sha256sum | cut -d' ' -f1)
	got=$(tf_run "$img" -c "mount ro; sha /extra/plain:stream.one; umount" 2>/dev/null | awk '/^sha /{print $2}')
	echo "ntfs-3g $want driver $got"
	[ -n "$want" ] && [ "$want" = "$got" ]
}

ntfs_ads_xattr() {
	local img=$1 want got
	want=$(ntfscat -a 0x80 -n stream.one "$img" /extra/plain 2>/dev/null | sha256sum | cut -d' ' -f1)
	got=$(tf_run "$img" -c "mount ro; getxattr /extra/plain user.stream.one; umount" 2>/dev/null)
	echo "$got"
	echo "$got" | grep -q "stream.one" || return 1
	[ -n "$want" ]
}

# A read-write mount request on a gate volume: comes up read-only, writes
# nothing, refuses changes with EROFS.
ntfs_gate() {
	local img=$1
	tf_run "$img" --cow --trap -c \
		"mount rw; readall; statfs; =ROFS mkdir /gate; =ROFS create /gate2; umount; mount ro; =NOT_FOUND stat /gate; umount"
}

ntfs_gate_manifest() {
	local img=$1 base exp="$TF_TMP/g-exp.$$" act="$TF_TMP/g-act.$$"
	base=$(tf_img ntfs-4k-basic) || return 1
	python3 "$FSIMG/oracle.py" manifest ntfs "$base" > "$exp" || return 1
	tf_run "$img" --trap -c "mount ro; manifest; umount" > "$act.all" || return 1
	# the hibernated image adds hiberfil.sys to the basic tree
	grep -v '^f /hiberfil\.sys ' "$act.all" > "$act"
	python3 "$FSIMG/mktree.py" compare "$exp" "$act"
}

ntfs_options() {
	local img=$1
	tf_run "$img" --cow --trap -c \
		"mount ro uid=1000,gid=100,fmask=0133,dmask=0022,show_sys_files,native_symlink=rel; options; ls /; readlink /abs_link; stat /\$Bitmap; umount; mount ro nocase,hide_dot_files; stat /MIXED\x20CASE\x20NAME.txt; umount"
}

# ---- write side ----

ntfs_check() {
	# the strict consistency checker (ERRORs fail; leaks allowed)
	timeout 900 python3 host/test-ntfs-check.py "$1" --quiet
}

ntfs_verify() {
	# ntfsfix -n, the strict checker, and ntfs-3g's view == the driver's
	local img=$1 exp="$TF_TMP/v-exp.$$" act="$TF_TMP/v-act.$$"
	tf_fsck ntfs "$img" || { echo "ntfsfix -n: not clean"; return 1; }
	ntfs_check "$img" || return 1
	python3 "$FSIMG/oracle.py" manifest ntfs "$img" > "$exp" || return 1
	tf_run "$img" --trap -c "mount ro; manifest; umount" > "$act" || return 1
	python3 "$FSIMG/mktree.py" compare "$exp" "$act"
}

# The volume flags of $Volume after a clean unmount: not dirty.
ntfs_clean_flag() {
	timeout 300 python3 host/test-ntfs-check.py "$1" | grep -q 'volume flags 0x0' ||
		{ echo "volume left dirty"; return 1; }
}

ntfs_w_script() {
	# ntfs_w_script VARIANT SCRIPT [RUNNER ARGS...]
	local v=$1 sc=$2 img c
	shift 2
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" "$@" -s "$sc" || { tf_drop "$c"; return 1; }
	ntfs_verify "$c" && ntfs_clean_flag "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

# A volume formatted for a native 4096-byte-sector device (mkntfs -s 4096;
# the generated images have 512-byte NTFS sectors, which the driver keeps
# read-only on a 4 KB device).
ntfs_img_4kn() {
	local cl=$1 out="$TF_IMGDIR/ntfs-4kn-$1.img"
	if [ ! -f "$out" ]; then
		truncate -s 128M "$out" || return 1
		mkntfs -F -Q -q -s 4096 -c "$cl" -p 0 -H 0 -S 0 "$out" > /dev/null 2>&1 ||
			{ rm -f "$out"; return 77; }
		echo "ntfs 4096" > "$out.info"
	fi
	echo "$out"
}

ntfs_w_4kn() {
	# ntfs_w_4kn CLUSTER SEED: stress + scripts on a 4Kn volume
	local cl=$1 seed=$2 img c
	img=$(ntfs_img_4kn "$cl") || return 77
	c=$(tf_copy "$img")
	tf_run "$c" --sector 4096 -s "$FSIMG/script-write-basic.txt" &&
		tf_run "$c" --sector 4096 -c "mount; stressopt utf8 1; stress $seed 1000 200; umount" &&
		tf_run "$c" --sector 4096 --trap -c "mount; readall; umount" &&
		tf_fsck ntfs "$c" && ntfs_check "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_w_stress() {
	# ntfs_w_stress VARIANT SEED OPS EVERY [RUNNER ARGS...]
	local v=$1 seed=$2 ops=$3 every=$4 img c
	shift 4
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" "$@" -c "mount; stressopt utf8 1; stress $seed $ops $every; umount" ||
		{ tf_drop "$c"; return 1; }
	ntfs_verify "$c" && ntfs_clean_flag "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

# Generated scripts (python, deterministic).
ntfs_gen_scripts() {
	python3 - "$TF_TMP" <<'PY'
import random, sys
d = sys.argv[1]
random.seed(5)
# a large directory: inserts in random order, deletions, renames,
# everything removed again (splits, push-down, merges, collapse)
names = ['n%05d_%s' % (i, 'x' * random.randint(1, 60)) for i in range(3000)]
order = names[:]
random.shuffle(order)
L = ['mount', 'mkdir /big']
L += ['create /big/' + n for n in order]
L.append('cycle')
dele = names[::2]
random.shuffle(dele)
L += ['unlink /big/' + n for n in dele]
L.append('cycle')
L += ['rename /big/%s /big/r%s' % (n, n) for n in names[1::4]]
L += ['stat /big/r' + names[1], '=NOT_FOUND stat /big/' + names[1]]
L.append('cycle')
L += ['unlink /big/' + n for n in names[3::4]]
L += ['unlink /big/r' + n for n in names[1::4]]
L += ['rmdir /big', 'umount']
open(d + '/bigdir.txt', 'w').write('\n'.join(L) + '\n')
# a directory kept large (left on the volume for ntfs-3g to read)
L = ['mount', 'mkdir /keep']
L += ['create /keep/' + n for n in order[:1500]]
L += ['unlink /keep/' + n for n in order[:1500:3]]
L += ['umount']
open(d + '/bigkeep.txt', 'w').write('\n'.join(L) + '\n')
# interleaved appends with a sync each: fragmented files, attribute
# lists, extension records; then shrink and delete
L = ['mount', 'mkdir /fr', 'create /fr/a', 'create /fr/b']
for i in range(700):
    L.append('append /fr/a fill:%d:512' % (i % 250 + 1))
    L.append('append /fr/b fill:%d:512' % ((i + 7) % 250 + 1))
    L.append('sync')
L += ['cycle', 'truncate /fr/a 100000', 'cycle', 'truncate /fr/a 1000',
      'cycle', 'unlink /fr/a', 'umount']
open(d + '/frag.txt', 'w').write('\n'.join(L) + '\n')
# many files: $MFT grows
L = ['mount', 'mkdir /m']
for i in range(5):
    L.append('mkdir /m/d%d' % i)
    # mostly resident files (64 KB clusters hold few non-resident ones)
    L += ['write /m/d%d/f%04d pat:%d:%d' % (i, j, j + 1,
          (j * 37) % 400 if j % 10 else 5000) for j in range(1000)]
L += ['cycle', 'umount']
open(d + '/mftgrow.txt', 'w').write('\n'.join(L) + '\n')
PY
}

ntfs_frag_expect() {
	# /fr/b after the fragmentation script, through ntfs-3g
	local img=$1 want got
	want=$(python3 -c "
import hashlib
print(hashlib.sha256(b''.join(bytes([(i+7)%250+1])*512 for i in range(700))).hexdigest())")
	got=$(ntfscat "$img" /fr/b 2>/dev/null | sha256sum | cut -d' ' -f1)
	echo "expect $want ntfs-3g $got"
	[ "$want" = "$got" ]
}

ntfs_w_frag() {
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -s "$TF_TMP/frag.txt" || { tf_drop "$c"; return 1; }
	ntfs_verify "$c" && ntfs_frag_expect "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_w_enospc() {
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; ?write /f zero:200m; statfs; mkdir /d; ?write /d/g pat:1:3m; ?write /d/h pat:2:2m; sync; cycle; unlink /f; sync; write /h pat:3:20m; cycle; check /h pat:3:20m; umount" ||
		{ tf_drop "$c"; return 1; }
	ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_w_misc() {
	# open-unlinked files, remount ro <-> rw, O_DIRECT, sparse option,
	# directory seek / O_DIRECTORY regressions
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; write /o pat:1:100000; fopen 1 /o r+; unlink /o; =NOT_FOUND stat /o; fpwrite 1 0 pat:2:5000; fread 1 100; fclose 1; remount ro; =ROFS mkdir /x; remount rw; mkdir /x; write /x/d pat:3:1m; fopen 2 /x/d wD; fpwrite 2 8192 pat:4:8192; fclose 2; fopen 3 /x/d rD; fpread 3 8192 8192; fclose 3; pread /x/d 8192 8192; =NOTDIR fopen 4 /x/d rd; fopen 5 /x rd; fseek 5 5 0; fseek 5 0 1; fseek 5 0 0; fclose 5; fopen 6 /x/d r; fseek 6 100 0; =INVALID fseek 6 9223372036854775807 1; fclose 6; umount; mount sparse; write /sp pat:5:10; pwrite /sp 10000000 pat:6:10; stat /sp; truncate /sp 50000000; cycle; zeros /sp 10 9999990; umount" ||
		{ tf_drop "$c"; return 1; }
	ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_w_idle() {
	# after a clean read-write session: an idle read-write mount and
	# walk writes nothing, and the volume is byte-identical afterwards
	local v=$1 img c c2
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; mkdir /i; write /i/f pat:1:70000; symlink f /i/l; umount" ||
		{ tf_drop "$c"; return 1; }
	c2=$(tf_copy "$c")
	tf_run "$c2" --trap -c "mount; readall; statfs; ls /i; readlink /i/l; umount; mount ro; readall; umount" ||
		{ tf_drop "$c"; tf_drop "$c2"; return 1; }
	cmp "$c" "$c2"
	local rc=$?
	tf_drop "$c"
	tf_drop "$c2"
	return $rc
}

ntfs_w_gate() {
	# `force' overrides the dirty flag only; never hibernation
	local img
	img=$(tf_img ntfs-4k-dirty) || return 77
	tf_run "$img" --cow -c "mount rw; =ROFS mkdir /g; =ROFS remount rw; remount rw force; mkdir /g; umount" || return 1
	img=$(tf_img ntfs-4k-hiber) || return 77
	tf_run "$img" --cow --trap -c "mount rw force; =ROFS mkdir /g; =ROFS remount rw force; umount" || return 1
	img=$(tf_img ntfs-4k-logdirty) || return 77
	tf_run "$img" --cow --trap -c "mount rw force; =ROFS mkdir /g; umount" || return 1
	img=$(tf_img ntfs-4k-mirrbad) || return 77
	tf_run "$img" --cow --trap -c "mount rw force; =ROFS mkdir /g; umount"
}

# Power cut after the Nth write, for every N (sampled for long
# workloads): the volume must be free of errors (leaks allowed), mount
# and read back.
NTFS_PC_WORK="mkdir /pc; write /pc/a pat:1:300000; write /pc/b pat:2:100; mkdir /pc/d; write /pc/d/c pat:3:70000; rename /pc/a /pc/d/a2; link /pc/b /pc/b2; unlink /pc/b; pwrite /pc/d/c 5000 pat:4:9000; truncate /pc/d/a2 1000; symlink x /pc/l; setxattr /pc/b2 user.k v; sync; unlink /pc/d/a2; rename /pc/d /pc/e; truncate /pc/e/c 200000"

ntfs_powercut() {
	local v=$1 img c total step n bad=0 out
	img=$(tf_img $v) || return 77
	c="$TF_TMP/pc-$v.img"
	cp --sparse=always "$img" "$c"
	out=$(tf_run "$c" -c "mount; $NTFS_PC_WORK; umount" 2>&1) || { echo "$out" | tail -5; return 1; }
	total=$(echo "$out" | sed -n 's/.* writes=\([0-9]*\) .*/\1/p')
	[ -n "$total" ] || return 1
	step=$(( total / 80 + 1 ))
	echo "workload: $total writes, cut every $step"
	for n in $(seq 0 $step $((total + 1))); do
		cp --sparse=always "$img" "$c"
		tf_run "$c" --quiet --allow-warn -c "mount; fault cut $n; $NTFS_PC_WORK; ?umount" > /dev/null 2>&1
		if ! ntfs_check "$c" > "$TF_TMP/pc.out" 2>&1; then
			echo "cut $n:"; head -5 "$TF_TMP/pc.out"; bad=$((bad + 1))
		fi
		if ! tf_run "$c" --cow --quiet -c "?mount; ?readall; ?umount" > "$TF_TMP/pc2.out" 2>&1; then
			echo "cut $n: remount/readall failed"; tail -3 "$TF_TMP/pc2.out"; bad=$((bad + 1))
		fi
	done
	rm -f "$c"
	[ $bad = 0 ]
}

# ---- journal and features ("n3" cases): journal replay, clean log,
# security, compression, streams, WOF, label, system-file protection ----

# An image with an unclean $LogFile of scenario $2 (real records); prints
# the image path, the expected file and contents in $TF_TMP/lr-exp.
ntfs_lr_img() {
	local v=$1 s=$2 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	if [ "$s" = redo-create ]; then
		tf_run "$c" -c "mount; mkdir /logdir; write /logdir/aaa text:x; write /logdir/zzz text:y; umount" > /dev/null 2>&1 || return 1
	fi
	python3 "$FSIMG/fsparse.py" ntfs-tweak "$c" log-$s > "$TF_TMP/lr-exp.$$" || return 1
	echo "$c"
}

# Replay: a read-only mount writes nothing; a read-write mount replays,
# the result is what the log says (through ntfs-3g), the volume checks
# clean, its log is clean afterwards and a second mount writes nothing.
ntfs_replay() {
	local v=$1 s=$2 c want path got
	c=$(ntfs_lr_img $v $s) || return $?
	path=$(sed -n 's/^expect \([^|]*\)|.*$/\1/p' "$TF_TMP/lr-exp.$$")
	want=$(sed -n 's/^expect [^|]*|\(.*\)$/\1/p' "$TF_TMP/lr-exp.$$")
	tf_run "$c" --cow --trap -c "mount ro; readall; umount" || { tf_drop "$c"; return 1; }
	# without `recover': read-only, nothing written, `force' or not; a
	# remount read-write with `recover' is refused too
	tf_run "$c" --cow --trap --allow-warn -c "mount; =ROFS mkdir /x; umount; mount rw force; readall; =ROFS mkdir /x; =ROFS remount rw recover; umount" || { tf_drop "$c"; return 1; }
	# recovery=ask: a mount that could replay it is refused, writing
	# nothing; read-only it mounts as without the option
	tf_run "$c" --cow --trap --allow-warn -c "=UCLEAN mount recovery=ask; =UCLEAN mount rw force,recovery=ask; mount ro recovery=ask; readall; umount" || { tf_drop "$c"; return 1; }
	if [ "$s" = bad-op ]; then
		tf_run "$c" --cow --trap --allow-warn -c "mount recover; =ROFS mkdir /x; umount" || { tf_drop "$c"; return 1; }
		tf_drop "$c"
		return 0
	fi
	tf_run "$c" --allow-warn -c "mount recover; options; mkdir /after; umount" 2>&1 | tee "$TF_TMP/rp.$$" | grep -q 'replayed' ||
		{ echo "no replay"; tf_drop "$c"; return 1; }
	got=$(ntfscat "$c" "$path" 2>/dev/null | head -c ${#want} | tr -d '\0')
	[ "$s" = undo-resident ] && got=$(ntfscat "$c" "$path" 2>/dev/null | head -c 8)
	echo "want '$want' got '$got'"
	if [ "$s" != undo-resident ] && [ "$got" != "$want" ]; then tf_drop "$c"; return 1; fi
	if [ "$s" = undo-resident ] && [ "$got" = "UNDONE!!" ]; then tf_drop "$c"; return 1; fi
	tf_fsck ntfs "$c" && ntfs_check "$c" && ntfs_clean_flag "$c" &&
		timeout 60 ntfsrecover -n -v "$c" 2>&1 | grep -q ', clean' &&
		tf_run "$c" --trap -c "mount; readall; umount"
	local rc=$?
	tf_drop "$c"
	return $rc
}

# Power cut during the replay itself: every prefix of the replay's writes
# leaves a volume whose next mount replays again (or finds it done) with
# the same result.
ntfs_replay_cut() {
	local v=$1 c total n bad=0 base
	base=$(ntfs_lr_img $v redo-create) || return $?
	total=$(tf_run "$base" --cow --allow-warn -c "mount recover; umount" 2>&1 | sed -n 's/.* writes=\([0-9]*\) .*/\1/p')
	[ -n "$total" ] || return 1
	echo "replay: $total writes"
	for n in $(seq 0 $((total + 1))); do
		c=$(tf_copy "$base")
		tf_run "$c" --quiet --allow-warn -c "fault cut $n; ?mount recover; ?umount" > /dev/null 2>&1
		tf_run "$c" --quiet --allow-warn -c "mount recover; umount" > /dev/null 2>&1
		if [ "$(ntfscat "$c" /logdir/logged.txt 2>/dev/null)" != "hello log" ] ||
		   ! ntfs_check "$c" > /dev/null 2>&1 || ! tf_fsck ntfs "$c" > /dev/null 2>&1; then
			echo "cut $n: bad result"; bad=$((bad + 1))
		fi
		tf_drop "$c"
	done
	tf_drop "$base"
	[ $bad = 0 ]
}

# A clean unmount leaves a clean log that ntfs-3g's log tool reads as
# clean; the sequence continues on the next session.
ntfs_clean_log() {
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; mkdir /a; umount; mount; mkdir /b; umount; remount rw" > /dev/null 2>&1
	tf_run "$c" -c "mount; mkdir /c; remount ro; remount rw; mkdir /d; umount" || { tf_drop "$c"; return 1; }
	timeout 60 ntfsrecover -n -v "$c" 2>&1 | grep -E 'syncing|current_lsn' | head -3
	timeout 60 ntfsrecover -n -v "$c" 2>&1 | grep -q ', clean' && ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_label() {
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; setxattr / system.ntfs_volume_label Grüße-Vol; getxattr / system.ntfs_volume_label; uid 1000 1000; =PERM setxattr / system.ntfs_volume_label x; uid 0 0; =UNSUPPORTED setxattr /dir3 system.ntfs_volume_label x; umount" || { tf_drop "$c"; return 1; }
	[ "$(ntfslabel "$c" 2>/dev/null)" = "Grüße-Vol" ] && ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_streams() {
	local img c
	img=$(tf_img ntfs-4k-full) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; write /extra/plain:newone pat:3:100000; write /extra/plain:small pat:4:50; truncate /extra/plain:newone 5000; setxattr /extra/plain user.small hello; mkdir /sd; write /sd:dirstream pat:5:70000; unlink /extra/plain:stream.one; =NOT_FOUND stat /extra/plain:stream.one; =NOT_FOUND unlink /extra/plain:zzz; fopen 1 /extra/plain:newone r; =BUSY unlink /extra/plain:newone; fclose 1; rmxattr /extra/plain user.tiny; =NODATA getxattr /extra/plain user.tiny; cycle; sha /extra/plain:newone; umount" > "$TF_TMP/st.$$" || { tf_drop "$c"; return 1; }
	local want got
	want=$(awk '/^sha .*newone/{print $2}' "$TF_TMP/st.$$" | tail -1)
	got=$(ntfscat -a 0x80 -n newone "$c" /extra/plain 2>/dev/null | sha256sum | cut -d' ' -f1)
	[ "$(ntfscat -a 0x80 -n small "$c" /extra/plain 2>/dev/null)" = hello ] &&
		[ "$want" = "$got" ] && ! ntfscat -a 0x80 -n stream.one "$c" /extra/plain > /dev/null 2>&1 &&
		ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_security() {
	local v=$1 img c d="$TF_TMP/sx.$$" comp=""
	# compression needs clusters of at most 4 KB
	case $v in *64k*) comp="=UNSUPPORTED" ;; esac
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	mkdir -p "$d"
	printf '\x21\x00\x00\x00' > "$d/attr21"
	printf '\x01\x08\x00\x00' > "$d/attrcomp"
	python3 - "$d" <<'PY'
import struct, sys
d = sys.argv[1]
def acl(e): return struct.pack('<I', 2) + b''.join(struct.pack('<HHI', *x) for x in e)
open(d + '/def', 'wb').write(acl([(1, 7, 0), (4, 5, 0), (0x20, 5, 0)]))
open(d + '/acc', 'wb').write(acl([(1, 7, 0), (2, 6, 10000), (4, 5, 0), (0x10, 7, 0), (0x20, 4, 0)]))
# a descriptor of its own: owner S-1-5-18, DACL granting S-1-1-0 read
sid = bytes([1, 1, 0, 0, 0, 0, 0, 5]) + struct.pack('<I', 18)
ace = struct.pack('<BBHI', 0, 0, 20, 0x120089) + bytes([1, 1, 0, 0, 0, 0, 0, 1]) + struct.pack('<I', 0)
dacl = struct.pack('<BBHHH', 2, 0, 8 + len(ace), 1, 0) + ace
sd = struct.pack('<BBHIIII', 1, 0, 0x8004, 20 + len(dacl), 20 + len(dacl) + len(sid), 0, 20) + dacl + sid + sid
open(d + '/sd', 'wb').write(sd)
PY
	tf_run "$c" -c "mount acl; mkdir /s; write /s/a pat:1:100; write /s/b pat:2:5000; setxattr /s/a system.ntfs_attrib file:$d/attr21; getxattr /s/a system.ntfs_attrib; setxattr /s system.posix_acl_default file:$d/def; write /s/c pat:3:10; mkdir /s/sub; getxattr /s/sub system.posix_acl_default; setxattr /s/b system.posix_acl_access file:$d/acc; stat /s/b; chmod /s/b 0640; getxattr /s/b system.posix_acl_access; setxattr /s/c system.ntfs_security file:$d/sd; getxattr /s/c system.ntfs_security; write /s/e text:; $comp setxattr /s/e system.ntfs_attrib file:$d/attrcomp; write /s/e pat:9:200000; =UNSUPPORTED setxattr /s/b system.ntfs_attrib file:$d/attrcomp; setxattr /s/a security.selinux ctx; uid 1000 1000; =PERM setxattr /s/a system.ntfs_attrib file:$d/attr21; =PERM setxattr /s/a trusted.x y; uid 0 0; cycle; getxattr /s/c system.ntfs_security; umount" || { tf_drop "$c"; return 1; }
	ntfs_verify "$c"
	local rc=$?
	rm -rf "$d"
	tf_drop "$c"
	return $rc
}

# The volume's own files are never written.
ntfs_protect() {
	local img
	img=$(tf_img ntfs-4k-basic) || return 77
	tf_run "$img" --cow --trap -c "mount; =PERM write /\$Bitmap text:x; =PERM truncate /\$LogFile 0; =PERM mkdir /\$Extend/x; =PERM write /\$Extend/y text:x; =PERM rename /dir3 /\$Extend/d; =PERM link /size_000001_01.bin /\$Extend/h; =PERM unlink /\$UpCase; =PERM chmod /\$Bitmap 0777; =PERM setxattr /\$Volume user.a b; =PERM write /\$Boot:s text:x; umount"
}

ntfs_compress() {
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; pwrite /extra/compressed 100000 pat:7:150000; append /extra/compressed pat:8:70000; truncate /extra/compressed 123457; truncate /extra/compressed 300000; zeros /extra/compressed 123457 176543; pwrite /extra/compressed_small 1000 zero:1000; write /extra/cdir/new fill:65:500000; append /extra/cdir/new pat:3:5000; write /extra/cdir/big pat:4:3000000; pwrite /extra/cdir/big 1000000 zero:200000; truncate /extra/cdir/big 1500001; cycle; zeros /extra/compressed 123457 176543; sha /extra/compressed; sha /extra/cdir/new; sha /extra/cdir/big; stat /extra/cdir/new; umount" > "$TF_TMP/cz.$$" || { tf_drop "$c"; return 1; }
	local bad=0 f want got
	for f in /extra/compressed /extra/cdir/new /extra/cdir/big; do
		want=$(awk -v f="$f" '$1=="sha" && $4==f {print $2}' "$TF_TMP/cz.$$" | tail -1)
		got=$(ntfscat "$c" "$f" 2>/dev/null | sha256sum | cut -d' ' -f1)
		[ "$want" = "$got" ] || { echo "$f: driver $want ntfs-3g $got"; bad=1; }
	done
	# compressed for real: far fewer blocks than bytes
	awk '/^stat \/extra\/cdir\/new/ { for (i = 1; i <= NF; i++) if ($i ~ /^blocks=/) { split($i, b, "="); if (b[2] > 200) exit 1 } }' "$TF_TMP/cz.$$" || { echo "not compressed"; bad=1; }
	[ $bad = 0 ] && ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_wof() {
	local fmt img c src="$TF_TMP/wsrc.$$" sz want got bad=0
	img=$(tf_img ntfs-4k-basic) || return 77
	head -c 150000 /usr/bin/python3 > "$src"
	cat "$FSIMG/fsparse.py" >> "$src"
	head -c 70000 /dev/zero >> "$src"
	sz=$(stat -c %s "$src")
	want=$(sha256sum "$src" | cut -d' ' -f1)
	for fmt in 0 1 2 3; do
		c=$(tf_copy "$img")
		python3 "$FSIMG/fsparse.py" wof-blob "$src" "$src.$fmt" $fmt || return 1
		tf_run "$c" -c "mount sparse; write /wf.bin text:; write /wf.bin:WofCompressedData file:$src.$fmt; truncate /wf.bin $sz; umount" > /dev/null || return 1
		python3 "$FSIMG/fsparse.py" wof-reparse "$c" wf.bin $fmt || return 1
		got=$(tf_run "$c" --trap -c "mount; sha /wf.bin; =UNSUPPORTED pwrite /wf.bin 0 text:x; =UNSUPPORTED truncate /wf.bin 5; umount" | awk '/^sha /{print $2}')
		echo "format $fmt: $got"
		[ "$got" = "$want" ] || bad=1
		tf_drop "$c"
	done
	rm -f "$src" "$src".?
	[ $bad = 0 ]
}

# `discard': freed clusters are discarded (after the cleared bits are on
# the device), nothing in use is; the volume checks clean.
ntfs_discard() {
	local img c out
	img=$(tf_img ntfs-4k-basic) || return 77
	c=$(tf_copy "$img")
	out=$(tf_run "$c" -c "mount discard; write /d1 pat:1:1000000; write /d2 pat:2:300000; sync; unlink /d1; truncate /d2 5000; sync; write /d3 pat:3:800000; umount; mount ro; check /d2 pat:2:5000; check /d3 pat:3:800000; umount" 2>&1) || { echo "$out" | tail -5; tf_drop "$c"; return 1; }
	echo "$out" | tail -1
	echo "$out" | grep -q 'discards=[1-9]' && ntfs_verify "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

# Power cut sweeps over the new write paths.
NTFS_PC_N3="setxattr / system.ntfs_volume_label NEWLABEL; write /p3 pat:1:3000; write /p3:st pat:2:90000; truncate /p3:st 1000; unlink /p3:st; write /extra/cdir/z pat:3:200000; pwrite /extra/compressed 5000 pat:4:70000; truncate /extra/compressed 9000; mkdir /n3d; write /n3d/f text:x; chmod /n3d/f 0444"

ntfs_powercut_n3() {
	local v=$1 img c total step n bad=0 out
	img=$(tf_img $v) || return 77
	c="$TF_TMP/pc3-$v.img"
	cp --sparse=always "$img" "$c"
	out=$(tf_run "$c" --allow-warn -c "mount; $NTFS_PC_N3; umount" 2>&1)
	total=$(echo "$out" | sed -n 's/.* writes=\([0-9]*\) .*/\1/p')
	[ -n "$total" ] || { echo "$out" | tail -5; return 1; }
	step=$(( total / 60 + 1 ))
	echo "workload: $total writes, cut every $step"
	for n in $(seq 0 $step $((total + 1))); do
		cp --sparse=always "$img" "$c"
		tf_run "$c" --quiet --allow-warn -c "mount; fault cut $n; $(echo "$NTFS_PC_N3" | sed 's/\(^\|; \)/\1?/g'); ?umount" > /dev/null 2>&1
		if ! ntfs_check "$c" > "$TF_TMP/pc3.out" 2>&1; then
			echo "cut $n:"; head -5 "$TF_TMP/pc3.out"; bad=$((bad + 1))
		fi
		if ! tf_run "$c" --cow --quiet -c "?mount; ?readall; ?umount" > /dev/null 2>&1; then
			echo "cut $n: remount/readall failed"; bad=$((bad + 1))
		fi
	done
	rm -f "$c"
	[ $bad = 0 ]
}

n3_suite() {
	local v s
	for s in redo-resident undo-resident bad-op; do
		tf_case "replay-$s" ntfs_replay ntfs-4k-basic $s
	done
	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic; do
		tf_case "replay-create-$v" ntfs_replay $v redo-create
		tf_case "clean-log-$v" ntfs_clean_log $v
		tf_case "label-$v" ntfs_label $v
		tf_case "security-$v" ntfs_security $v
	done
	tf_case replay-cut-4k ntfs_replay_cut ntfs-4k-basic
	tf_case replay-cut-512 ntfs_replay_cut ntfs-512-basic
	tf_case streams ntfs_streams
	tf_case protect ntfs_protect
	tf_case compress-4k ntfs_compress ntfs-4k-full
	tf_case compress-512 ntfs_compress ntfs-512-full
	tf_case wof ntfs_wof
	tf_case discard ntfs_discard
	for v in ntfs-4k-full ntfs-512-full; do
		tf_case "powercut-n3-$v" ntfs_powercut_n3 $v
		img=$(tf_img $v) && tf_case "idle-trap-n3-$v" ntfs_n3_idle $v
	done
}

# After the journal/feature workload (NTFS_PC_N3), an idle read-write mount
# and walk writes nothing.
ntfs_n3_idle() {
	local v=$1 img c c2
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" --allow-warn -c "mount; $NTFS_PC_N3; umount" > /dev/null 2>&1 || { tf_drop "$c"; return 1; }
	c2=$(tf_copy "$c")
	tf_run "$c2" --trap -c "mount; readall; statfs; umount; mount ro; readall; umount" || { tf_drop "$c"; tf_drop "$c2"; return 1; }
	cmp "$c" "$c2"
	local rc=$?
	tf_drop "$c"
	tf_drop "$c2"
	return $rc
}

# ---- write safety: regressions and the long runs ----
#
#   regressions (in the default run): a fragmented $MFT with an attribute
#   list growing (records and $MFT:$BITMAP), named streams removed from
#   shared extension records, the volume label as the first change of a
#   mount, failing writes during the clean-unmount sequence, a volume laid
#   out like a system drive (ntfs-4k-win: DOS 8.3 names, junctions,
#   $UsnJrnl, pagefile/hiberfil) read with zero writes in every gate
#   state, its system files refused, names removed through their DOS
#   alias;
#   audit (host/test-ntfs.sh audit): power cut after EVERY write of
#   several workloads (B+tree merges, attribute lists, stream removal,
#   MFT growth, the journal and feature workload), allocation failure and
#   write error at every allocation / write, with the rule that anything
#   a cut or an error leaves behind is on a volume still marked dirty;
#   audit-long: randomized workloads of 20000+ operations per geometry
#   and seed, and fuzzing with fresh seeds.

# A power-cut / failure result: the checker finds no ERROR, and anything
# left behind (LEAK) is on a volume that is still marked dirty (so the next
# check reclaims it).
ntfs_pc_ok() {
	local out
	out=$(timeout 900 python3 host/test-ntfs-check.py "$1" 2>&1)
	if echo "$out" | grep -q '^ERROR'; then
		echo "$out" | grep '^ERROR' | head -5
		return 1
	fi
	if echo "$out" | grep -q '^LEAK' &&
	   ! echo "$out" | grep -q 'volume flags 0x[0-9a-f]*[13579bdf]$'; then
		echo "left-overs on a volume marked clean:"
		echo "$out" | grep '^LEAK' | head -3
		return 1
	fi
	echo "$out" | grep -q '^check:' || { echo "$out" | tail -3; return 1; }
}

# ntfs_sweep BASE "WORK" STEP [RUNNER ARGS]: a power cut after every
# STEP-th device write of WORK (from a mount of BASE), each result checked
# (ntfs_pc_ok) and mounted + read back.
ntfs_sweep() {
	local base=$1 work=$2 step=$3 c total n bad=0 out
	shift 3
	c="$TF_TMP/sw.img"
	cp --sparse=always "$base" "$c"
	out=$(tf_run "$c" "$@" --allow-warn -c "mount; $work; umount" 2>&1) ||
		{ echo "$out" | tail -5; rm -f "$c"; return 1; }
	total=$(echo "$out" | sed -n 's/.* writes=\([0-9]*\) .*/\1/p' | tail -1)
	[ -n "$total" ] || { rm -f "$c"; return 1; }
	echo "workload: $total writes, cut every $step"
	for n in $(seq 1 "$step" $((total + 1))); do
		cp --sparse=always "$base" "$c"
		tf_run "$c" "$@" --quiet --allow-warn -c "mount; fault cut $n; $(echo "$work" | sed 's/\(^\|; \)?*/\1?/g'); ?umount" > "$TF_TMP/sw1.out" 2>&1
		case $? in 0|1) ;; *) echo "cut $n: runner status $?"; tail -3 "$TF_TMP/sw1.out"; bad=$((bad + 1)) ;; esac
		if ! ntfs_pc_ok "$c" > "$TF_TMP/sw.out" 2>&1; then
			echo "cut $n:"; head -5 "$TF_TMP/sw.out"; bad=$((bad + 1))
		fi
		if ! tf_run "$c" "$@" --cow --quiet -c "?mount; ?readall; ?umount" > "$TF_TMP/sw2.out" 2>&1; then
			echo "cut $n: remount/readall failed"; tail -3 "$TF_TMP/sw2.out"; bad=$((bad + 1))
		fi
		[ $bad -gt 10 ] && break
	done
	rm -f "$c"
	[ $bad = 0 ]
}

# ntfs_faults BASE "WORK" KIND STEP: KIND kalloc = an allocation failure at
# every STEP-th allocation of WORK, write = a device write error at every
# STEP-th write.  Every outcome: no crash or sanitizer report, no ERROR,
# anything left behind (a $MFTMirr copy behind included) on a volume still
# marked dirty (ntfs_pc_ok).
ntfs_faults() {
	local base=$1 work=$2 kind=$3 step=$4 c total n bad=0 out rc
	c="$TF_TMP/fl.img"
	cp --sparse=always "$base" "$c"
	out=$(tf_run "$c" --allow-warn -c "mount; kcount; $work; kcount; umount" 2>&1) ||
		{ echo "$out" | tail -5; rm -f "$c"; return 1; }
	if [ "$kind" = kalloc ]; then
		total=$(echo "$out" | sed -n 's/^kcount \([0-9]*\)$/\1/p' | awk 'NR==1{a=$1} NR==2{print $1-a}')
	else
		total=$(echo "$out" | sed -n 's/.* writes=\([0-9]*\) .*/\1/p' | tail -1)
	fi
	[ -n "$total" ] || { rm -f "$c"; return 1; }
	echo "workload: $total ${kind}s, a failure at every $step"
	for n in $(seq 1 "$step" "$total"); do
		cp --sparse=always "$base" "$c"
		if [ "$kind" = kalloc ]; then
			tf_run "$c" --quiet --allow-warn -c "mount; kfault $n; $(echo "$work" | sed 's/\(^\|; \)?*/\1?/g'); ?umount; ?mount; ?readall; ?umount" > "$TF_TMP/fl1.out" 2>&1
		else
			tf_run "$c" --quiet --allow-warn -c "mount; fault write $n; $(echo "$work" | sed 's/\(^\|; \)?*/\1?/g'); ?umount" > "$TF_TMP/fl1.out" 2>&1
		fi
		rc=$?
		if [ $rc -gt 1 ]; then
			echo "fault $n: runner status $rc"; grep -E 'ERROR|runtime error|BUG|DEADLOCK|FATAL' "$TF_TMP/fl1.out" | head -3
			bad=$((bad + 1))
		fi
		if ! ntfs_pc_ok "$c" > "$TF_TMP/fl.out" 2>&1; then
			echo "fault $n:"; head -5 "$TF_TMP/fl.out"; bad=$((bad + 1))
		fi
		[ $bad -gt 10 ] && break
	done
	rm -f "$c"
	[ $bad = 0 ]
}

# A volume whose $MFT is fragmented enough to carry an attribute list:
# libntfs-3g grows $MFT while every other run of 8 free clusters is held
# back (fsparse.py fragfree), then the held clusters are given back.
ntfs_img_mftal() {
	local out="$TF_IMGDIR/ntfs-512-mftal.img" pop="$TESTFS_DIR/ntfs-populate" i lib
	if [ ! -f "$out" ]; then
		if [ ! -x "$pop" ]; then
			lib=$(ldconfig -p 2>/dev/null | awk '/libntfs-3g\.so\./ {print $NF; exit}')
			[ -n "$lib" ] || return 77
			gcc -O1 -o "$pop" "$FSIMG/ntfs-populate.c" "-l:$(basename "$lib")" || return 77
		fi
		rm -f "$out.tmp"
		truncate -s 24M "$out.tmp" &&
			mkntfs -F -Q -q -c 512 -s 512 -p 0 -H 0 -S 0 "$out.tmp" > /dev/null 2>&1 ||
			{ rm -f "$out.tmp"; return 77; }
		python3 "$FSIMG/fsparse.py" ntfs-tweak "$out.tmp" fragfree-8 || return 1
		{ echo "mkdir /m"; for i in $(seq 1 1500); do echo "file /m/f$i pat:$i:20"; done; } > "$TF_TMP/mftal.txt"
		"$pop" "$out.tmp" "$TF_TMP/mftal.txt" > /dev/null 2>&1 || return 1
		python3 "$FSIMG/fsparse.py" ntfs-tweak "$out.tmp" unfragfree || return 1
		rm -f "$out.tmp.fragstate"
		ntfsinfo -i 0 "$out.tmp" 2>/dev/null | grep -q 'ATTRIBUTE_LIST' ||
			{ echo "mftal: no attribute list on \$MFT" >&2; rm -f "$out.tmp"; return 1; }
		mv "$out.tmp" "$out"
		echo "ntfs 512 fragmented \$MFT" > "$out.info"
	fi
	echo "$out"
}

# $MFT with an attribute list (record 0 full, the list non-resident, the
# last extent in an extension record) fills up: $MFT:$BITMAP grows in
# record 0; $MFT itself cannot grow in one record write there, so new
# files get ENOSPC once every record is in use -- and the volume stays
# consistent.
ntfs_a_mftal() {
	local img c i
	img=$(ntfs_img_mftal) || return $?
	c=$(tf_copy "$img")
	{ echo "mount"; echo "mkdir /n"; for i in $(seq 1 300); do echo "?write /n/g$i pat:$i:30"; done
	  echo "=NOSPC create /n/last"; echo "cycle"; echo "check /n/g1 pat:1:30"; echo "umount"; } > "$TF_TMP/mftal-grow.txt"
	tf_run "$c" -s "$TF_TMP/mftal-grow.txt" || { tf_drop "$c"; return 1; }
	ntfs_verify "$c" && ntfs_clean_flag "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

ntfs_a_mftal_cut() {
	local img work="mkdir /n" i
	img=$(ntfs_img_mftal) || return $?
	# every record used: the last creates get ENOSPC ($MFT does not grow)
	for i in $(seq 1 200); do work="$work; ?write /n/g$i pat:$i:30"; done
	ntfs_sweep "$img" "$work" "${1:-1}"
}

# Named streams removed from extension records that hold others: the
# records are rewritten (a kept record would still map the removed
# stream's clusters), then the clusters are reused.
NTFS_A_STREAMS=""
for _i in $(seq 1 40); do NTFS_A_STREAMS="$NTFS_A_STREAMS; write /s:st$_i pat:$_i:3000"; done
NTFS_A_STREAMS="create /s$NTFS_A_STREAMS; cycle; unlink /s:st20; unlink /s:st33; truncate /s:st7 0; setxattr /s user.x yz; cycle; write /t pat:99:400000"
unset _i

ntfs_a_streams() {
	local v=$1 img c
	img=$(tf_img $v) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; $NTFS_A_STREAMS; umount" || { tf_drop "$c"; return 1; }
	ntfs_check "$c" && tf_fsck ntfs "$c" &&
		[ "$(ntfscat -a 0x80 -n st19 "$c" /s 2>/dev/null | wc -c)" = 3000 ] &&
		[ "$(ntfscat -a 0x80 -n st7 "$c" /s 2>/dev/null | wc -c)" = 0 ] &&
		tf_run "$c" --trap -c "mount ro; check /s:st19 pat:19:3000; check /s:st40 pat:40:3000; =NOT_FOUND stat /s:st20; =NOT_FOUND stat /s:st33; check /t pat:99:400000; umount" &&
		ntfs_clean_flag "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

# The label as the first change of a mount, then the power goes: the
# volume must say dirty (arming marks it dirty before the label's record
# write, which must not store the old flag word again).
ntfs_a_label_first() {
	local v sec img c n
	for v in ntfs-4k-basic 4kn; do
		if [ $v = 4kn ]; then
			img=$(ntfs_img_4kn 4096) || return 77
			sec=4096
		else
			img=$(tf_img $v) || return 77
			sec=512
		fi
		for n in 1 2 3 4 5 6 7 8; do
			c=$(tf_copy "$img")
			tf_run "$c" --sector $sec --quiet --allow-warn -c "mount; fault cut $n; ?setxattr / system.ntfs_volume_label NEWLABEL; ?mkdir /x; ?umount" > /dev/null 2>&1
			ntfs_pc_ok "$c" || { tf_drop "$c"; return 1; }
			if [ $n -ge 3 ] && ! timeout 300 python3 host/test-ntfs-check.py "$c" |
				grep -q 'volume flags 0x[0-9a-f]*[13579bdf]$'; then
				echo "$v cut $n: changed volume marked clean"
				tf_drop "$c"
				return 1
			fi
			tf_drop "$c"
		done
		# the same label again: nothing written at all
		tf_run "$img" --sector $sec --cow -c "mount; setxattr / system.ntfs_volume_label SAME; umount; trap on; mount; setxattr / system.ntfs_volume_label SAME; umount" || return 1
	done
}

# A device write error during the clean-unmount sequence: the volume
# keeps its dirty flag (the record that clears it is written after its
# mirror copy, and only when everything before succeeded).
ntfs_a_umount_fail() {
	local v=$1 img c k
	img=$(tf_img $v) || return 77
	for k in $(seq 1 24); do
		c=$(tf_copy "$img")
		tf_run "$c" --quiet --allow-warn -c "mount; mkdir /u; write /u/f pat:1:20000; sync; fault write $k; ?umount" > "$TF_TMP/uf.out" 2>&1
		if grep -q 'io_errors=[1-9]' "$TF_TMP/uf.out" &&
		   ! timeout 300 python3 host/test-ntfs-check.py "$c" | grep -q 'volume flags 0x[0-9a-f]*[13579bdf]$'; then
			echo "write $k of the unmount failed, the volume is marked clean"
			tf_drop "$c"
			return 1
		fi
		ntfs_pc_ok "$c" || { tf_drop "$c"; return 1; }
		tf_drop "$c"
	done
}

# Every read-only path over a volume laid out like a system drive, in
# every gate state: zero device writes.  Also failing / refused changes.
NTFS_A_IDLE="readall; statfs; ls /; ls /Windows/System32; ls /Users/Default; listxattr /Windows/System32/ntoskrnl.exe; ?getxattr / system.ntfs_volume_label; ?getxattr /Windows system.ntfs_security; ?getxattr /pagefile.sys system.ntfs_attrib; sha /Windows/System32/ntoskrnl.exe:Zone.Identifier; readlink /Documents_and_Settings; readlink /Users/All_Users; stat /Users/Default/LONG_D~1.DOC; stat /\$Extend/\$UsnJrnl:\$J; sha /\$Extend/\$UsnJrnl:\$Max; ?fopen 1 /Users/Default/NTUSER.DAT r+; ?fread 1 4096; ?fstat 1; ?ffsync 1; ?fclose 1; fopen 2 /pagefile.sys rD; fpread 2 0 4096; fclose 2; ?fopen 3 /Windows/System32/driver_store.dat r+; ?fclose 3; ?pwrite /\$MFT 0 text:x; stat /\$MFT; fsync /Users/Default/NTUSER.DAT; sync; ?truncate /Users/Default/NTUSER.DAT 262144; ?rename /Users /Users; ?utimens /ProgramData omit omit"
NTFS_A_REFUSE="=PERM write /\$Bitmap text:x; =PERM truncate /\$LogFile 0; =PERM pwrite /\$MFT 0 text:x; =PERM write /\$Extend/\$UsnJrnl:\$J text:x; =PERM truncate /\$Extend/\$UsnJrnl 0; =PERM unlink /\$Extend/\$UsnJrnl; =PERM rename /\$Extend/\$UsnJrnl /u; =PERM link /\$Extend/\$UsnJrnl /u; =PERM mkdir /\$Extend/x; =PERM rename /ProgramData /\$Extend/p; =PERM setxattr /\$Extend/\$UsnJrnl user.a b; =PERM chmod /\$Extend/\$UsnJrnl 0777; =PERM unlink /\$UpCase; =PERM link /Documents_and_Settings /j2; =PERM link /Users/All_Users /a2; =EXISTS mkdir /Windows; =NOTEMPTY rmdir /Users; =NOT_FOUND unlink /nothing; =INVALID rename /Users /Users/Default/u"

ntfs_a_win_variant() {
	# ntfs_a_win_variant TWEAK: idle walks with the write trap on the
	# Windows-like volume in that state (rw, rw force, ro, remounts)
	local tw=$1 img c refuse=""
	img=$(tf_img ntfs-4k-win) || return 77
	c=$(tf_copy "$img")
	[ "$tw" = none ] || python3 "$FSIMG/fsparse.py" ntfs-tweak "$c" $tw || { tf_drop "$c"; return 1; }
	# refused changes: on the writable volume (the others say ROFS)
	[ "$tw" = none ] && refuse="; $NTFS_A_REFUSE"
	tf_run "$c" --trap -c "mount; $NTFS_A_IDLE$refuse; umount; mount rw force; $NTFS_A_IDLE; umount; mount ro; $NTFS_A_IDLE; ?remount rw; ?remount rw force; readall; remount ro; umount; mount rw; cycle; umount"
	local rc=$?
	tf_drop "$c"
	return $rc
}

# Names of the other system's Win32 + DOS pairs: removed and renamed
# through either name, both go; then the volume checks clean and reads
# back the same through ntfs-3g.
ntfs_a_win_ops() {
	local img c
	img=$(tf_img ntfs-4k-win) || return 77
	c=$(tf_copy "$img")
	tf_run "$c" -c "mount; unlink /Users/Default/LONG_D~1.DOC; =NOT_FOUND stat /Users/Default/Long_Document_Name.docx; =NOT_FOUND stat /Users/Default/LONG_D~1.DOC; rename /Program_Files/Common_Files_Folder/APPLIC~1.JSO /Program_Files/app.json; =NOT_FOUND stat /Program_Files/Common_Files_Folder/Application_Settings.json; stat /Program_Files/app.json; write /pagefile.sys zero:2m; mkdir /Windows/Temp; write /Windows/Temp/x pat:3:300000; unlink /swapfile.sys; rename /Windows/ntoskrnl_link.exe /Windows/System32/k2.exe; truncate /Windows/System32/driver_store.dat 70000; cycle; stat /Windows/System32/k2.exe; umount" ||
		{ tf_drop "$c"; return 1; }
	ntfs_verify "$c" && ntfs_clean_flag "$c"
	local rc=$?
	tf_drop "$c"
	return $rc
}

regress_suite() {
	local v
	img=$(ntfs_img_mftal) && {
		TF_ORACLE_MANIFEST=1 tf_case read-ntfs-512-mftal tf_expect_manifest ntfs "$img"
		tf_case trap-ntfs-512-mftal tf_trap_walk "$img"
	}
	tf_case a-mftal-grow ntfs_a_mftal
	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic; do
		tf_case "a-streams-al-$v" ntfs_a_streams $v
	done
	tf_case a-label-first ntfs_a_label_first
	tf_case a-umount-fail-4k ntfs_a_umount_fail ntfs-4k-basic
	tf_case a-umount-fail-512 ntfs_a_umount_fail ntfs-512-basic
	img=$(tf_img ntfs-4k-win) && {
		TF_ORACLE_MANIFEST=1 tf_case read-ntfs-4k-win tf_expect_manifest ntfs "$img"
		tf_case trap-ntfs-4k-win tf_trap_walk "$img"
	}
	for v in none dirty hiber vflags-0x8000 vflags-0x4000 vflags-0x2 \
		vflags-0x4 vflags-0x10 vflags-0x20 logfile-unclean mftmirr-mismatch; do
		tf_case "a-win-idle-$v" ntfs_a_win_variant $v
	done
	tf_case a-win-ops ntfs_a_win_ops
}

# Power-cut sweeps over every write, fault sweeps over every allocation /
# write.
NTFS_A_MERGE=""
for _i in $(seq -w 100 175) $(seq -w 300 320); do NTFS_A_MERGE="$NTFS_A_MERGE; unlink /mg/m${_i}_abcdefghijklmnopqrstuvwxyz0123456789"; done
for _i in $(seq -w 0 30); do NTFS_A_MERGE="$NTFS_A_MERGE; create /mg/n${_i}_zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"; done
NTFS_A_MERGE=${NTFS_A_MERGE#; }
unset _i

ntfs_a_mergebase() {
	# a directory of 400 long names (a two-level B+tree), unmounted clean
	local v=$1 img out="$TF_TMP/mg-$1.img" i s="mount; mkdir /mg"
	img=$(tf_img $v) || return 77
	if [ ! -f "$out" ]; then
		cp --sparse=always "$img" "$out"
		for i in $(seq -w 0 399); do s="$s; create /mg/m${i}_abcdefghijklmnopqrstuvwxyz0123456789"; done
		tf_run "$out" --quiet -c "$s; umount" > /dev/null 2>&1 || { rm -f "$out"; return 1; }
	fi
	echo "$out"
}

ntfs_a_sweep_v() {
	# ntfs_a_sweep_v VARIANT WORK STEP
	local img
	img=$(tf_img $1) || return 77
	ntfs_sweep "$img" "$2" "$3"
}

ntfs_a_sweep_merge() {
	local base
	base=$(ntfs_a_mergebase $1) || return $?
	ntfs_sweep "$base" "$NTFS_A_MERGE" "${2:-1}"
}

ntfs_a_faults_v() {
	# ntfs_a_faults_v VARIANT WORK KIND STEP
	local img
	img=$(tf_img $1) || return 77
	ntfs_faults "$img" "$2" "$3" "$4"
}

NTFS_A_FRAG="mkdir /fr; create /fr/a; create /fr/b"
for _i in $(seq 1 60); do NTFS_A_FRAG="$NTFS_A_FRAG; append /fr/a fill:$_i:512; append /fr/b fill:$_i:512; sync"; done
NTFS_A_FRAG="$NTFS_A_FRAG; truncate /fr/a 3000; unlink /fr/b"
unset _i

audit_suite() {
	local v k
	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic; do
		tf_case "a-cut-pc-$v" ntfs_a_sweep_v $v "$NTFS_PC_WORK" 1
		tf_case "a-cut-merge-$v" ntfs_a_sweep_merge $v 1
		tf_case "a-cut-streams-$v" ntfs_a_sweep_v $v "$NTFS_A_STREAMS" 1
	done
	tf_case a-cut-frag-512 ntfs_a_sweep_v ntfs-512-basic "$NTFS_A_FRAG" 1
	tf_case a-cut-n3-4k-full ntfs_a_sweep_v ntfs-4k-full "$NTFS_PC_N3" 1
	tf_case a-cut-n3-512-full ntfs_a_sweep_v ntfs-512-full "$NTFS_PC_N3" 1
	tf_case a-cut-mftal ntfs_a_mftal_cut 1
	tf_case a-cut-win ntfs_a_sweep_v ntfs-4k-win "unlink /Users/Default/LONG_D~1.DOC; rename /Program_Files/Common_Files_Folder/APPLIC~1.JSO /Program_Files/app.json; write /pagefile.sys zero:300k; mkdir /Windows/Temp; write /Windows/Temp/x pat:3:30000; unlink /swapfile.sys; truncate /Windows/System32/driver_store.dat 70000" 1
	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic; do
		k=3
		[ $v = ntfs-4k-basic ] && k=1
		tf_case "a-wfault-pc-$v" ntfs_a_faults_v $v "$NTFS_PC_WORK" write 1
		tf_case "a-kfault-pc-$v" ntfs_a_faults_v $v "$NTFS_PC_WORK" kalloc $k
	done
	tf_case a-wfault-n3-4k-full ntfs_a_faults_v ntfs-4k-full "$NTFS_PC_N3" write 1
	tf_case a-kfault-n3-4k-full ntfs_a_faults_v ntfs-4k-full "$NTFS_PC_N3" kalloc 1
	tf_case a-wfault-streams-4k ntfs_a_faults_v ntfs-4k-basic "$NTFS_A_STREAMS" write 1
	tf_case a-kfault-streams-4k ntfs_a_faults_v ntfs-4k-basic "$NTFS_A_STREAMS" kalloc 10
	tf_case a-kfault-merge-4k ntfs_a_kfault_merge 10
}

ntfs_a_kfault_merge() {
	local base
	base=$(ntfs_a_mergebase ntfs-4k-basic) || return $?
	ntfs_faults "$base" "$NTFS_A_MERGE" kalloc "${1:-1}"
}

# Long randomized workloads: 20000 operations per run (remount every 500),
# the model compared at every remount, then ntfsfix -n, the checker and
# ntfs-3g's view.
ntfs_a_stress() {
	local v=$1 seed=$2 ops=$3 img c
	shift 3
	if [ "${v#4kn-}" != "$v" ]; then
		img=$(ntfs_img_4kn "${v#4kn-}") || return 77
		set -- --sector 4096 "$@"
	else
		img=$(tf_img $v) || return 77
	fi
	c=$(tf_copy "$img")
	tf_run "$c" "$@" -c "mount; stressopt utf8 1; stress $seed $ops 500; umount" ||
		{ tf_drop "$c"; return 1; }
	if [ "${v#4kn-}" != "$v" ]; then
		tf_fsck ntfs "$c" && ntfs_check "$c" &&
			tf_run "$c" "$@" --trap -c "mount ro; readall; umount"
	else
		ntfs_verify "$c" && ntfs_clean_flag "$c"
	fi
	local rc=$?
	tf_drop "$c"
	return $rc
}

audit_long_suite() {
	local v s
	for v in ntfs-512-basic ntfs-4k-basic ntfs-64k-basic ntfs-4k-full ntfs-4k-win 4kn-4096 4kn-65536; do
		for s in 101 202 303; do
			tf_case "a-stress-$v-$s" ntfs_a_stress $v $s 20000
		done
	done
	for v in ntfs-4k-win ntfs-4k-full ntfs-512-full ntfs-64k-basic ntfs-4k-many; do
		img=$(tf_img $v) || continue
		tf_case "a-fuzz-ro-$v" tf_fuzz ntfs "$img" "${FUZZ_N:-1000}" 911
		tf_case "a-fuzz-rw-$v" tf_fuzz ntfs "$img" "${FUZZ_N:-1000}" 912 --rw
	done
	img=$(ntfs_img_mftal) && {
		tf_case a-fuzz-ro-mftal tf_fuzz ntfs "$img" "${FUZZ_N:-1000}" 913
		tf_case a-fuzz-rw-mftal tf_fuzz ntfs "$img" "${FUZZ_N:-1000}" 914 --rw
	}
}

write_suite() {
	local v sec
	ntfs_gen_scripts
	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic; do
		tf_case "w-basic-$v" ntfs_w_script $v "$FSIMG/script-write-basic.txt"
		tf_case "w-posix-$v" ntfs_w_script $v "$FSIMG/script-write-posix.txt"
		tf_case "w-misc-$v" ntfs_w_misc $v
		tf_case "w-idle-$v" ntfs_w_idle $v
		tf_case "w-stress-$v" ntfs_w_stress $v 71 1500 300
	done
	tf_case w-stress-full ntfs_w_stress ntfs-4k-full 72 1500 250
	tf_case w-4kn-4k ntfs_w_4kn 4096 73
	tf_case w-4kn-64k ntfs_w_4kn 65536 74
	# 512-byte NTFS sectors on a 4 KB device: read-only, no write
	img=$(tf_img ntfs-4k-basic) && tf_case w-sector4096-ro tf_run "$img" \
		--sector 4096 --cow --trap -c "mount rw; readall; =ROFS mkdir /x; umount"
	tf_case w-stress-512-every ntfs_w_stress ntfs-512-basic 75 600 7
	tf_case w-bigdir ntfs_w_script ntfs-4k-basic "$TF_TMP/bigdir.txt"
	tf_case w-bigkeep-64k ntfs_w_script ntfs-64k-basic "$TF_TMP/bigkeep.txt"
	tf_case w-frag-512 ntfs_w_frag ntfs-512-basic
	tf_case w-frag-4k ntfs_w_frag ntfs-4k-basic
	tf_case w-mftgrow-512 ntfs_w_script ntfs-512-basic "$TF_TMP/mftgrow.txt"
	tf_case w-mftgrow-64k ntfs_w_script ntfs-64k-basic "$TF_TMP/mftgrow.txt"
	tf_case w-enospc-4k ntfs_w_enospc ntfs-4k-basic
	tf_case w-enospc-64k ntfs_w_enospc ntfs-64k-basic
	tf_case w-gate ntfs_w_gate
	# the driver itself refuses every change on a read-only mount (the
	# runner's VFS gate bypassed): no write reaches the device
	img=$(tf_img ntfs-4k-basic) && tf_case w-rawro tf_run "$img" --cow --trap -c \
		"mount ro; rawro on; =ROFS mkdir /x; =ROFS create /y; =ROFS unlink /size_000001_01.bin; =ROFS rename /dir3 /d4; =ROFS truncate /size_000512_04.bin 3; =ROFS chmod /dir3 0777; =ROFS symlink a /l; =ROFS link /size_000001_01.bin /h; =ROFS setxattr /dir3 user.a b; =ROFS utimens /dir3 now now; umount"
	for v in ntfs-4k-full ntfs-4k-many ntfs-4k-deep ntfs-512-full; do
		img=$(tf_img $v) || continue
		tf_case "w-idle-trap-$v" tf_idle_rw "$img"
	done
}

powercut_suite() {
	local v
	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic; do
		tf_case "powercut-$v" ntfs_powercut $v
	done
}

suite() {
	local img v

	tf_case unit ntfs_unit
	[ "$MODE" = unit ] && return
	if [ "$MODE" = write ]; then
		write_suite
		return
	fi
	if [ "$MODE" = powercut ]; then
		powercut_suite
		return
	fi
	if [ "$MODE" = n3 ]; then
		n3_suite
		return
	fi
	if [ "$MODE" = regress ]; then
		regress_suite
		return
	fi
	if [ "$MODE" = audit ]; then
		audit_suite
		return
	fi
	if [ "$MODE" = audit-long ]; then
		audit_long_suite
		return
	fi
	if [ "$MODE" != fuzz ]; then
		for v in ntfs-512-basic ntfs-4k-basic ntfs-64k-basic \
			ntfs-512-full ntfs-4k-full ntfs-64k-full ntfs-4k-many \
			ntfs-4k-deep ntfs-4k-big ntfs-4k-empty ntfs-512-many \
			ntfs-64k-deep; do
			img=$(tf_img $v) || { tf_skip $v "image"; continue; }
			TF_ORACLE_MANIFEST=1 tf_case "read-$v" tf_expect_manifest ntfs "$img"
			tf_case "trap-$v" tf_trap_walk "$img"
		done
		img=$(tf_img ntfs-4k-full) || return
		tf_case read-ads ntfs_ads "$img"
		tf_case read-ads-xattr ntfs_ads_xattr "$img"
		tf_case read-xattr tf_run "$img" -c "mount ro; listxattr /extra/plain; umount"
		tf_case read-sector4096 tf_run "$img" --sector 4096 --trap -c "mount ro; readall; umount"
		img=$(tf_img ntfs-64k-basic) && TF_ORACLE_MANIFEST=1 \
			tf_case read-sector4096-64k tf_expect_manifest ntfs "$img" --sector 4096
		img=$(tf_img ntfs-512-basic) && tf_case read-sector4096-512 \
			tf_run "$img" --sector 4096 -c "=NOT_FOUND mount ro"
		img=$(tf_img ntfs-4k-full) || return
		tf_case read-options ntfs_options "$img"
		tf_case read-seek-data-hole tf_run "$img" --trap -c \
			"mount ro; fopen 0 /extra/sparse r; fseek 0 0 3; fseek 0 0 4; fseek 0 19999999 4; fclose 0; fopen 1 /extra/allhole r; =NXIO fseek 1 0 3; fseek 1 0 4; fclose 1; umount"
		tf_case read-streams-windows tf_run "$img" --trap -c \
			"mount ro streams_interface=windows; sha /extra/plain:STREAM.ONE; =NOT_FOUND stat /extra/plain::\$DATA; umount; mount ro streams_interface=none; =NOT_FOUND stat /extra/plain:stream.one; umount"
		img=$(tf_img ntfs-4k-many) && tf_case read-readdir-small tf_run "$img" --trap -c "chunk 512; mount ro; manifest; umount"
		img=$(tf_img ntfs-4k-full) || return
		for v in ntfs-4k-dirty ntfs-4k-hiber ntfs-4k-logdirty ntfs-4k-mirrbad; do
			img=$(tf_img $v) || { tf_skip $v "image"; continue; }
			tf_case "gate-$v" ntfs_gate "$img"
			# ntfs-3g refuses these volumes: they hold the basic
			# tree, so ntfs-3g's view of ntfs-4k-basic is what the
			# driver must show.
			tf_case "read-$v" ntfs_gate_manifest "$img"
		done
	fi
	if [ "$MODE" != fuzz ]; then
		write_suite
		powercut_suite
		n3_suite
		regress_suite
	fi
	[ "$MODE" = fuzz ] || [ "$MODE" = all ] || return
	for v in ntfs-4k-full ntfs-512-full ntfs-64k-basic ntfs-4k-many; do
		img=$(tf_img $v) || continue
		tf_case "fuzz-ro-$v" tf_fuzz ntfs "$img" "${FUZZ_N:-${TF_FUZZ_ITERS:-300}}" 81
		tf_case "fuzz-rw-$v" tf_fuzz ntfs "$img" "${FUZZ_N:-${TF_FUZZ_ITERS:-300}}" 82 --rw
	done
	# an unclean $LogFile (real records) and a volume with $Secure ids and
	# compressed data written by the driver: mutated metadata, replayed
	# or refused, never a crash or a write outside the rules
	img=$(ntfs_lr_img ntfs-4k-basic redo-create) &&
		tf_case fuzz-rw-logfile tf_fuzz ntfs "$img" "${FUZZ_N:-${TF_FUZZ_ITERS:-300}}" 83 --rw \
			--cmds "mount recover; readall; ?mkdir /fz_dir; ?write /fz_dir/f pat:7:70000; ?unlink /fz_dir/f; ?rmdir /fz_dir; readall; umount; ?mount ro; readall; umount"
	img=$(tf_img ntfs-4k-full) && {
		c=$(tf_copy "$img")
		tf_run "$c" --allow-warn -c "mount; $NTFS_PC_N3; umount" > /dev/null 2>&1
		tf_case fuzz-rw-n3 tf_fuzz ntfs "$c" "${FUZZ_N:-${TF_FUZZ_ITERS:-300}}" 84 --rw
	}
}

suite
echo "test-ntfs: $TF_PASS passed, $TF_FAILED failed, $TF_SKIP skipped"
[ -n "$TF_FAILS" ] && echo "failed:$TF_FAILS (outputs in $TESTFS_DIR/last-fail-ntfs-read-*.out)"
[ "$TF_FAILED" = 0 ]
