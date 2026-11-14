#!/bin/sh
# Host test for boot/efi_loadopt.c, the device path, boot load option and
# BOOTX64.CSV code shared by the boot loader, the kernel's firmware
# variable files and likeos-efiboot.  The file is freestanding C, so the
# build host's compiler runs it -- under AddressSanitizer and
# UndefinedBehaviorSanitizer when the compiler has them -- against golden
# byte layouts, then fuzzes every parser.  Run from the repository root;
# an optional argument sets the number of fuzz rounds (default 300000).
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-efi-loadopt-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
if ! echo 'int main(void){return 0;}' | \
	cc $SAN -x c - -o "$TMP/probe" 2>/dev/null; then
	SAN=
fi
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g $SAN \
	-o "$TMP/test" host/test-efi-loadopt.c boot/efi_loadopt.c
# ASan reserves a large shadow mapping, so the address space limit only
# applies to the uninstrumented build.
if [ -n "$SAN" ]; then
	ASAN_OPTIONS=detect_leaks=1 timeout 300 nice -n 19 "$TMP/test" "$@"
else
	(ulimit -v 2500000; timeout 300 nice -n 19 "$TMP/test" "$@")
fi
