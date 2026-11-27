#!/bin/bash
# Host harness for the input core: kernel/dev/input/input.c, input-mt.c and
# evdev.c compiled as they are, freestanding against the kernel's headers,
# with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection,
# linked to simulated kernel services (host/input/kstubs.c) that run real
# threads, and driven by host/input/tests.c: a simulated five-slot touchpad,
# the built-in keyboard and mouse streams, every event-device request,
# /proc/bus/input/devices against the dumps in kernel/spec, and races between
# readers, producers and device removal.  The keyboard driver
# (kernel/dev/input/keyboard.c, keymap.c) is linked in too, with a simulated
# console terminal (host/input/kbstubs.c): who gets the keys and the LEDs.
# The work queue (software autorepeat) is the HID harness's simulated one
# (host/hid/wqshim.c): delayed items run when a test calls test_wq_run()
# and the harness clock has passed their deadline.
#
#   host/test-input.sh [GROUP]       run all groups, or one of:
#                                    builtin filter mt ioctl clients proc
#                                    lifecycle race handles keyboard
#   host/test-input.sh --build       build only, print the binary
#
# Environment:
#   TESTINPUT_DIR        build directory (default $TMPDIR/likeos-test-input)
#   TESTINPUT_RACE_SECS  length of the race phase (default 4)
#   TESTINPUT_VERBOSE    1: kernel log to stderr
# Run from anywhere (the tests read kernel/spec relative to the repository
# root, which the script changes to).  Run it boxed (tmp/boxed.sh).
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2

TESTINPUT_DIR=${TESTINPUT_DIR:-${TMPDIR:-/tmp}/likeos-test-input}
BIN="$TESTINPUT_DIR/test-input"
mkdir -p "$TESTINPUT_DIR" || exit 2

KSRC="kernel/dev/input/input.c kernel/dev/input/input-mt.c
      kernel/dev/input/evdev.c kernel/dev/input/keyboard.c
      kernel/dev/input/keymap.c host/input/kstubs.c host/input/kbstubs.c
      host/hid/wqshim.c host/input/tests.c"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin
	-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1
	-Wall -Wextra -Wno-unused-function -Wno-unused-parameter
	-Werror=implicit-function-declaration
	$SAN -include host/input/prelude.h"
HFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN"

build() {
	local f o objs=""

	if [ -x "$BIN" ] && [ -z "$(find kernel/dev/input kernel/uapi \
		kernel/ke/sched.h kernel/ke/mutex.h kernel/ke/waitq.h kernel/io/tty.h \
		kernel/dev/video/fbdev.h kernel/dev/video/console_owner.h \
		kernel/dev/input/keyboard.h \
		kernel/ke/workqueue.h kernel/ke/hrtimer.h host/hid/wqshim.c \
		host/hid/wqshim.h host/input host/test-input.sh -newer "$BIN" -print -quit)" ]; then
		return 0
	fi
	echo "test-input: building ..." >&2
	for f in $KSRC; do
		o="$TESTINPUT_DIR/$(basename "$f" .c).o"
		timeout 600 nice -n 10 gcc $KFLAGS -c "$f" -o "$o" \
			2> "$o.log" || { cat "$o.log" >&2; echo "test-input: $f does not compile" >&2; return 1; }
		if grep -q 'warning:' "$o.log"; then
			cat "$o.log" >&2
			case $f in
			kernel/*) echo "test-input: warnings in $f" >&2; return 1 ;;
			esac
		fi
		objs="$objs $o"
	done
	timeout 600 gcc $HFLAGS -c host/input/hostlib.c -o "$TESTINPUT_DIR/hostlib.o" || return 1
	timeout 600 gcc $SAN -pthread -rdynamic -o "$BIN.tmp" $objs "$TESTINPUT_DIR/hostlib.o" || return 1
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
timeout --kill-after=10 600 "$BIN" "${1:-all}"
