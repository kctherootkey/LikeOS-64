#!/bin/bash
# Host harness for the HID core and hid-multitouch: kernel/dev/hid/core/
# hid-core.c, hid-input.c, hid-quirks.c and kernel/dev/hid/hid-multitouch.c
# compiled as they are, freestanding against the kernel's headers, together
# with the real input core (kernel/dev/input/input.c, input-mt.c, evdev.c),
# with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection.  The
# kernel services come from the input harness (host/input/kstubs.c,
# hostlib.c, prelude.h) plus a simulated work queue (host/hid/wqshim.c);
# host/hid/tests.c drives a mock transport with the descriptor corpus of
# host/hid/corpus.h and reads the results back through event-device clients.
#
#   host/test-hid.sh [GROUP]       run all groups, or one of:
#                                  parse elan cirque generic touchscreen
#                                  malformed race fuzz
#   host/test-hid.sh --build       build only, print the binary
#
# Environment:
#   TESTHID_DIR         build directory (default $TMPDIR/likeos-test-hid)
#   TESTHID_FUZZ_SECS   length of the fuzz phase (default 60)
#   TESTHID_FUZZ_SEED   fuzz seed (default 0x51d)
#   TESTHID_RACE_SECS   length of the race phase (default 3)
#   TESTINPUT_VERBOSE   1: kernel log to stderr
# Run it boxed (tmp/boxed.sh).
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2

TESTHID_DIR=${TESTHID_DIR:-${TMPDIR:-/tmp}/likeos-test-hid}
BIN="$TESTHID_DIR/test-hid"
mkdir -p "$TESTHID_DIR" || exit 2

KSRC="kernel/dev/hid/core/hid-core.c kernel/dev/hid/core/hid-input.c
      kernel/dev/hid/core/hid-quirks.c kernel/dev/hid/hid-multitouch.c
      kernel/dev/input/input.c kernel/dev/input/input-mt.c
      kernel/dev/input/evdev.c kernel/dev/input/keyboard.c
      kernel/dev/input/keymap.c host/input/kstubs.c host/input/kbstubs.c
      host/hid/wqshim.c host/hid/tests.c"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin
	-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1
	-Wall -Wextra -Wno-unused-function -Wno-unused-parameter
	-Werror=implicit-function-declaration
	$SAN -include host/input/prelude.h"
HFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN"

build() {
	local f o objs=""

	if [ -x "$BIN" ] && [ -z "$(find kernel/dev/hid kernel/dev/input \
		kernel/uapi kernel/ke/sched.h kernel/ke/mutex.h \
		kernel/ke/waitq.h kernel/ke/workqueue.h host/input host/hid \
		host/test-hid.sh -newer "$BIN" -print -quit)" ]; then
		return 0
	fi
	echo "test-hid: building ..." >&2
	for f in $KSRC; do
		o="$TESTHID_DIR/$(basename "$f" .c).o"
		timeout 600 nice -n 10 gcc $KFLAGS -c "$f" -o "$o" \
			2> "$o.log" || { cat "$o.log" >&2; echo "test-hid: $f does not compile" >&2; return 1; }
		if grep -q 'warning:' "$o.log"; then
			cat "$o.log" >&2
			case $f in
			kernel/dev/hid/*) echo "test-hid: warnings in $f" >&2; return 1 ;;
			esac
		fi
		objs="$objs $o"
	done
	timeout 600 gcc $HFLAGS -c host/input/hostlib.c -o "$TESTHID_DIR/hostlib.o" || return 1
	timeout 600 gcc $SAN -pthread -rdynamic -o "$BIN.tmp" $objs "$TESTHID_DIR/hostlib.o" || return 1
	mv "$BIN.tmp" "$BIN"
}

build || exit 1
if [ "${1:-}" = "--build" ]; then
	echo "$BIN"
	exit 0
fi

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:handle_abort=1:allocator_may_return_null=1:exitcode=4:detect_stack_use_after_return=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
ulimit -c 0
timeout --kill-after=10 900 "$BIN" "${1:-all}"
