#!/bin/bash
# Rerun every saved fuzzer finding (host/fsimg/regress/*.cmd) against a
# harness binary: host/test-fs.sh <driver> --replay [FSTYPE]
#
#   replay.sh TEST_FS_BINARY [FSTYPE]
#
# Each entry is the base image (base-*.img.xz, decompressed to a temporary
# file), the patch the fuzzer applied and the commands it ran.  An entry
# passes when the run now ends without a finding (fuzz.py's rules).
#
# Copyright (C) 2026 The LikeOS Project
set -u
BIN=$1
ONLY=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/replay.XXXXXX") || exit 2
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
for c in "$HERE"/*.cmd; do
	[ -e "$c" ] || continue
	fst=$(sed -n 's/^fstype=//p' "$c")
	[ -n "$ONLY" ] && [ "$fst" != "$ONLY" ] && continue
	base=$(sed -n 's/^base=//p' "$c")
	sector=$(sed -n 's/^sector=//p' "$c")
	cmds=$(sed -n 's/^cmds=//p' "$c")
	name=$(basename "$c" .cmd)
	img="$TMP/${base%.xz}"
	if [ ! -f "$img" ]; then
		if [ ! -f "$HERE/$base" ]; then
			echo "SKIP $name: base image $base not saved"
			continue
		fi
		xz -dc "$HERE/$base" > "$img" || continue
	fi
	out=$(timeout -k 5 60 "$BIN" "$img" --cow --patch "$HERE/$name.patch" \
		--allow-warn --sector "$sector" -c "$cmds" 2>&1)
	rc=$?
	why=$(printf '%s' "$out" | python3 -B -c 'import sys
sys.path.insert(0, sys.argv[2])
import fuzz
print(fuzz.classify(int(sys.argv[1]), sys.stdin.read()) or "")' "$rc" "$HERE/..")
	if [ -z "$why" ]; then
		echo "PASS $name"
		pass=$((pass + 1))
	else
		echo "FAIL $name: $why"
		fail=$((fail + 1))
	fi
done
echo "replay: $pass passed, $fail failed"
[ "$fail" = 0 ]
