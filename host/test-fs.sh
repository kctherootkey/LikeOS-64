#!/bin/bash
# Host harness for the kernel filesystem drivers.
#
# Compiles the driver (kernel/fs/<driver>.c, or every .c file of the
# directory kernel/fs/<driver>/) together with the REAL page cache, inode
# cache and dentry cache (kernel/fs/pagecache.c, icache.c, dcache.c), the
# kernel's mutex / wait queue / completion / rwsem / credential code and
# the kernel's own string formatter, against a file-backed block device,
# with AddressSanitizer and UndefinedBehaviorSanitizer, and runs it.
#
#   host/test-fs.sh <driver>                    run the driver's suite
#                                               (host/fsimg/suite-<driver>.sh)
#   host/test-fs.sh <driver> IMAGE [ARGS...]    one run of the runner on IMAGE
#   host/test-fs.sh <driver> --build            build only, print the binary
#   host/test-fs.sh <driver> --fuzz FSTYPE IMAGE [fuzz.py options]
#                                               metadata fuzzing (fsimg/fuzz.py)
#   host/test-fs.sh <driver> --replay           rerun host/fsimg/regress
#
# <driver> is the file or directory name under kernel/fs (fat32, ext4,
# ntfs, exfat, isofs); the fstype symbol is <driver>_fstype unless FSTYPE= says
# otherwise.  Environment:
#   TESTFS_DIR      build and temp directory (default $TMPDIR/likeos-test-fs)
#   TESTFS_TIMEOUT  seconds per run (default 600)
#   TESTFS_MEM      memory limit per run (default 2G)
#   TESTFS_NODEBUG  1: build without -DDEBUG
#   TESTFS_EXTRA    more kernel .c files to compile in
#   FSTYPE          fstype symbol override
# Run from the repository root.  See host/fsimg/README.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)

DRIVER=${1:-}
if [ -z "$DRIVER" ] || { [ ! -f "kernel/fs/$DRIVER.c" ] && [ ! -d "kernel/fs/$DRIVER" ]; }; then
	echo "usage: host/test-fs.sh <driver> [IMAGE [ARGS...] | --build]" >&2
	echo "  (driver = a file kernel/fs/<driver>.c or a directory kernel/fs/<driver>/)" >&2
	exit 2
fi
shift
FSTYPE=${FSTYPE:-${DRIVER}_fstype}
TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-fs}
TESTFS_TIMEOUT=${TESTFS_TIMEOUT:-600}
TESTFS_MEM=${TESTFS_MEM:-2G}
BUILD="$TESTFS_DIR/build-$DRIVER"
BIN="$BUILD/test-fs-$DRIVER"
mkdir -p "$BUILD" || exit 2

# the driver's sources: kernel/fs/<driver>.c, or the .c files of the
# directory kernel/fs/<driver>/ (their objects are named <driver>_<file>.o)
if [ -d "kernel/fs/$DRIVER" ]; then
	DSRC=$(ls kernel/fs/"$DRIVER"/*.c)
else
	DSRC="kernel/fs/$DRIVER.c"
fi
KSRC="$DSRC kernel/fs/pagecache.c kernel/fs/icache.c
      kernel/fs/dcache.c kernel/fs/pathbuf.c kernel/ke/task_path.c
      kernel/ke/mutex.c kernel/ke/waitq.c
      kernel/ke/completion.c kernel/mm/rwsem.c kernel/ke/cred.c
      ${TESTFS_EXTRA:-}"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
DBG="-DDEBUG=1"
[ "${TESTFS_NODEBUG:-0}" = 1 ] && DBG=""
KFLAGS="-std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin
	-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 $DBG
	-Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
	-Wno-discarded-qualifiers -Werror=implicit-function-declaration
	$SAN -include host/test-fs-prelude.h"
HFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN"

needs_build() {
	[ -x "$BIN" ] || return 0
	[ -n "$(find kernel host/test-fs-prelude.h host/test-fs-stubs.c \
		host/test-fs-main.c host/test-fs-bdev.c host/test-fs.sh \
		host/fsimg/kfmt.awk -newer "$BIN" \( -name '*.[ch]' -o \
		-name '*.sh' -o -name '*.awk' \) -print -quit 2>/dev/null)" ]
}

build() {
	local o objs="" defs="-DTEST_FSTYPE=$FSTYPE" f
	needs_build || return 0
	echo "test-fs: building $DRIVER ..." >&2
	# objects of an earlier layout of the driver must not be linked
	rm -f "$BUILD"/*.o
	if grep -q '^vfs_superblock_t \*g_root_sb' $DSRC; then
		defs="$defs -DTEST_HAVE_ROOT_SB"
	fi
	# the kernel's formatter, out of console.c
	{
		echo '#include <kernel/io/console.h>'
		echo '#define SIZE_MAX ((size_t)-1)'
		echo 'static void console_putchar_unlocked(char c) { (void)c; }'
		echo '#define va_start(ap, last) __builtin_va_start(ap, last)'
		echo '#define va_arg(ap, type) __builtin_va_arg(ap, type)'
		echo '#define va_end(ap) __builtin_va_end(ap)'
		awk -f host/fsimg/kfmt.awk kernel/io/console.c
	} > "$BUILD/kfmt.c"
	for f in $KSRC "$BUILD/kfmt.c"; do
		case $f in
		kernel/fs/"$DRIVER"/*) o="$BUILD/${DRIVER}_$(basename "$f" .c).o" ;;
		*) o="$BUILD/$(basename "$f" .c).o" ;;
		esac
		timeout 600 nice -n 10 gcc $KFLAGS $defs -c "$f" -o "$o" \
			2> "$o.log" || { cat "$o.log" >&2; echo "test-fs: $f does not compile" >&2; return 1; }
		grep -E 'warning|error' "$o.log" | head -20 >&2
		objs="$objs $o"
	done
	for f in host/test-fs-stubs.c host/test-fs-main.c; do
		o="$BUILD/$(basename "$f" .c).o"
		timeout 600 nice -n 10 gcc $KFLAGS -Werror $defs -c "$f" -o "$o" || return 1
		objs="$objs $o"
	done
	timeout 600 gcc $HFLAGS -c host/test-fs-bdev.c -o "$BUILD/test-fs-bdev.o" || return 1
	timeout 600 gcc $SAN -o "$BIN.tmp" $objs "$BUILD/test-fs-bdev.o" || return 1
	mv "$BIN.tmp" "$BIN"
}

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:handle_abort=1:handle_sigill=1:allocator_may_return_null=1:exitcode=4:detect_stack_use_after_return=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1:exitcode=5}

# One bounded run of the runner: tf_bin ARGS...
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

build || exit 2
if [ "${1:-}" = "--build" ]; then
	echo "$BIN"
	exit 0
fi
# the whole fuzz session in one bounded scope
tf_scope() {
	local t=$1
	shift
	if command -v systemd-run >/dev/null 2>&1 &&
	   systemd-run --user --scope -q -p MemoryMax=1M true >/dev/null 2>&1; then
		timeout -k 10 "$t" systemd-run --user --scope -q \
			-p MemoryMax="$TESTFS_MEM" -p MemorySwapMax=0 -- nice -n 10 "$@"
	else
		timeout -k 10 "$t" nice -n 10 "$@"
	fi
}
if [ "${1:-}" = "--fuzz" ]; then
	shift
	[ $# -ge 2 ] || { echo "usage: test-fs.sh DRIVER --fuzz FSTYPE IMAGE [options]" >&2; exit 2; }
	fst=$1 img=$2
	shift 2
	tf_scope "${TESTFS_FUZZ_TIMEOUT:-7200}" python3 host/fsimg/fuzz.py \
		--bin "$BIN" --fstype "$fst" --image "$img" "$@"
	exit $?
fi
if [ "${1:-}" = "--replay" ]; then
	shift
	tf_scope "${TESTFS_FUZZ_TIMEOUT:-7200}" bash host/fsimg/regress/replay.sh "$BIN" "$@"
	exit $?
fi
if [ $# -gt 0 ]; then
	tf_bin "$@"
	exit $?
fi

# ---- the suite ----
SUITE="host/fsimg/suite-$DRIVER.sh"
if [ ! -f "$SUITE" ]; then
	echo "test-fs: no suite $SUITE; run with an IMAGE instead" >&2
	exit 2
fi
TF_TMP=$(mktemp -d "$TESTFS_DIR/run-$DRIVER.XXXXXX") || exit 2
trap 'rm -rf "$TF_TMP"' EXIT
TF_PASS=0
TF_FAILED=0
TF_SKIP=0
TF_FAILS=""
. host/fsimg/common.sh

# tf_case NAME CMD...: a named check; nonzero status is a failure, the
# output is kept in $TF_TMP/NAME.out (shown when it fails).
tf_case() {
	local name=$1
	shift
	if "$@" > "$TF_TMP/$name.out" 2>&1; then
		TF_PASS=$((TF_PASS + 1))
		echo "PASS $name"
	else
		local rc=$?
		TF_FAILED=$((TF_FAILED + 1))
		TF_FAILS="$TF_FAILS $name"
		echo "FAIL $name (status $rc)"
		grep -E '^FAIL|ERROR|WRITE TRAP|FATAL|BUG|DEADLOCK|runtime error|WARNING|fsck|^test-fs' \
			"$TF_TMP/$name.out" | head -25 | sed 's/^/    /'
		cp "$TF_TMP/$name.out" "$TESTFS_DIR/last-fail-$DRIVER-$name.out" 2>/dev/null
	fi
}

# tf_xfail NAME REASON CMD...: a known bug of the driver.  Failing is
# expected (XFAIL, not counted as a failure); passing (XPASS) means the
# bug is gone and the case should become a tf_case.
TF_XFAIL=0
TF_XPASS=0
tf_xfail() {
	local name=$1 why=$2
	shift 2
	if "$@" > "$TF_TMP/$name.out" 2>&1; then
		TF_XPASS=$((TF_XPASS + 1))
		echo "XPASS $name (known bug fixed? $why)"
	else
		TF_XFAIL=$((TF_XFAIL + 1))
		echo "XFAIL $name: $why"
		cp "$TF_TMP/$name.out" "$TESTFS_DIR/last-xfail-$DRIVER-$name.out" 2>/dev/null
	fi
}

tf_skip() {
	TF_SKIP=$((TF_SKIP + 1))
	echo "SKIP $1: $2"
}

. "$SUITE"
suite
echo "test-fs $DRIVER: $TF_PASS passed, $TF_FAILED failed, $TF_SKIP skipped, $TF_XFAIL known bugs (xfail), $TF_XPASS xpass"
[ -n "$TF_FAILS" ] && echo "failed:$TF_FAILS (outputs in $TESTFS_DIR/last-fail-$DRIVER-*.out)"
[ "$TF_FAILED" = 0 ]
