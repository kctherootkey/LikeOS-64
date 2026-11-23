#!/bin/sh
# Build user/bin/ls.c for the host and check where it prints "total" lines:
# only above the contents of a directory, never above operands (files, or
# directories under -d).  Run from the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e

OUT=${TMPDIR:-/tmp}/likeos-ls-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
SRC=$(pwd)/user/bin/ls.c

# The host's nlink_t is wider than the LikeOS one: -Wno-format for that.
cc -O1 -g -Wall -Wextra -Wno-format -include stdint.h -o "$OUT/ls" "$SRC"

cd "$OUT"
mkdir -p d/sub e
echo hi > d/f
echo x > f1

FAIL=0
# expect N ARGS...: ls ARGS must print exactly N "total" lines
expect() {
	n=$1
	shift
	got=$(./ls "$@" 2>&1 | grep -c '^total ' || true)
	if [ "$got" = "$n" ]; then
		echo "  ok   ls $* -> $n total line(s)"
	else
		echo "  FAIL ls $* -> $got total line(s), want $n"
		./ls "$@" 2>&1 | sed 's/^/       | /'
		FAIL=$((FAIL + 1))
	fi
}

expect 0 -ld d
expect 0 -ld
expect 0 -ld d d/sub
expect 0 -ld d f1
expect 0 -l f1
expect 0 -l f1 d/f
expect 1 -l d
expect 1 -l e
expect 1 -l
expect 1 -l f1 d
expect 2 -l d d/sub
expect 2 -lR d
expect 0 d
expect 0 -d d

# Mixed operands: the files first, without a header, then each directory
# under its own.
want=$(printf 'f1\n\nd:\nf\nsub')
got=$(./ls -1 f1 d)
if [ "$got" = "$want" ]; then
	echo "  ok   ls -1 f1 d: files, then the directory under a header"
else
	echo "  FAIL ls -1 f1 d:"
	printf '%s\n' "$got" | sed 's/^/       | /'
	FAIL=$((FAIL + 1))
fi

if [ "$FAIL" -ne 0 ]; then
	echo "FAILED: $FAIL"
	exit 1
fi
echo "all passed"
