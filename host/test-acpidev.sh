#!/bin/sh
# Host test of the ACPI device layer (kernel/hal/acpi_dev.c) over a model
# of the ACPICA namespace interfaces (host/kshim/acpica_mock.c), with
# AddressSanitizer and UndefinedBehaviorSanitizer.  Run from the
# repository root, through tmp/boxed.sh.  Extra compiler options may be
# given as arguments.
#
# Copyright (C) 2026 The LikeOS Project

set -e
cd "$(dirname "$0")/.."
TMP=${TMPDIR:-/tmp}/likeos-test-acpidev.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -ffreestanding -nostdinc -fno-builtin -I.
	-Ikernel/hal/acpica/include -D__LIKEOS__ -D__LIKEOS_KERNEL__
	-DACPI_USE_BUILTIN_STDARG -U__linux__ -U_LINUX -Ulinux
	-Wall -Wextra -Werror -Wno-unused-function -Wno-sign-compare $SAN
	-include host/kshim/prelude.h $*"
for f in kernel/hal/acpi_dev.c host/kshim/acpica_mock.c host/kshim/kshim.c \
	 host/acpidev/test_acpidev.c; do
	timeout 300 nice -n 10 cc $KFLAGS -c "$f" -o "$TMP/$(basename "$f" .c).o"
done
timeout 300 nice -n 10 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN \
	-c host/kshim/hrt.c -o "$TMP/hrt.o"
timeout 300 cc $SAN -pthread -rdynamic -o "$TMP/test-acpidev" "$TMP"/*.o
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
timeout 300 nice -n 10 "$TMP/test-acpidev"
