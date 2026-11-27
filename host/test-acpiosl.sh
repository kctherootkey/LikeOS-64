#!/bin/sh
# Host test of the ACPICA OS layer's threading part
# (kernel/hal/acpica/oslikeos_sync.c) over the real work queues
# (kernel/ke/workqueue.c) and the kernel shim (host/kshim), with
# AddressSanitizer and UndefinedBehaviorSanitizer.  Run from the
# repository root, through tmp/boxed.sh.  Extra compiler options may be
# given as arguments.
#
# Copyright (C) 2026 The LikeOS Project

set -e
cd "$(dirname "$0")/.."
TMP=${TMPDIR:-/tmp}/likeos-test-acpiosl.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -ffreestanding -nostdinc -fno-builtin -I.
	-Ikernel/hal/acpica/include -D__LIKEOS__ -D__LIKEOS_KERNEL__
	-DACPI_USE_BUILTIN_STDARG -U__linux__ -U_LINUX -Ulinux
	-Wall -Wextra -Werror -Wno-unused-function -Wno-sign-compare $SAN
	-include host/kshim/prelude.h $*"
for f in kernel/hal/acpica/oslikeos_sync.c kernel/ke/workqueue.c \
	 host/kshim/kshim.c host/acpiosl/test_acpiosl.c; do
	timeout 300 nice -n 10 cc $KFLAGS -Wno-unused-parameter \
		-c "$f" -o "$TMP/$(basename "$f" .c).o"
done
timeout 300 nice -n 10 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN \
	-c host/kshim/hrt.c -o "$TMP/hrt.o"
timeout 300 cc $SAN -pthread -rdynamic -o "$TMP/test-acpiosl" "$TMP"/*.o
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
timeout 600 nice -n 10 "$TMP/test-acpiosl"
