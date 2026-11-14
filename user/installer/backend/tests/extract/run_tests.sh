#!/bin/sh
# Runs the extractor tests: builds a tree with every kind of object, packs
# it with the host's GNU tar in the formats the release build and other
# tools produce, unpacks each archive with the libinstaller extractor and
# compares the result with `tar --compare` and compare_trees.py.  Then the
# hostile and damaged archives.
#
# Everything that needs owners and device nodes runs inside ONE fakeroot
# session (fakeroot_tests.sh), so the tree, the archives, the extraction
# and the comparison all see the same faked ownership.
#
#   run_tests.sh EXTRACT_TEST [WORKDIR]
#
# Never run this as root: it does not need it, and it must not touch
# anything outside WORKDIR.
#
# Copyright (C) 2026 The LikeOS Project

set -eu

here=$(cd "$(dirname "$0")" && pwd)
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=${2:-${EXTRACT_TEST_WORK:-$(dirname "$bin")/extract-work}}

if [ "$(id -u)" = 0 ]; then
	echo "run_tests.sh: refusing to run as root" >&2
	exit 2
fi
for t in fakeroot tar zstd python3 sha256sum; do
	command -v $t >/dev/null 2>&1 || { echo "run_tests.sh: $t is missing" >&2; exit 2; }
done
if [ -d "$work" ]; then
	chmod -R u+rwx "$work" 2>/dev/null || true
	rm -rf "$work"
fi
mkdir -p "$work"
rc=0
fakeroot -- sh "$here/fakeroot_tests.sh" "$here" "$bin" "$work" || rc=$?


# Once more without fakeroot, as the plain user the tests run as: owners
# cannot be set (counted, not fatal unless asked), device nodes cannot be
# made (fatal), read-only directories must still be filled.
if [ $rc -eq 0 ]; then
	out=$work/out_user
	mkdir -p "$out"
	"$bin" --log "$work/user.log" "$work/sp10.tar.zst" "$out" > "$work/user.out" 2>&1 || true
	if grep -q "^error: .*chr" "$work/user.out" ||
	   grep -q "rc=-1 " "$work/user.out"; then
		echo "PASS unprivileged: device nodes refused ($(grep '^error:' "$work/user.out"))"
	else
		echo "FAIL unprivileged: expected the device node to be refused"
		cat "$work/user.out"
		rc=1
	fi
	# without the devices (nodev.tar: made inside the fakeroot session,
	# so it carries the modes fakeroot showed, 0555 included)
	out2=$work/out_user2
	mkdir -p "$out2"
	if "$bin" --log "$work/user2.log" "$work/nodev.tar" "$out2" > "$work/user2.out" 2>&1 &&
	   [ "$(stat -c %a "$out2/ro")" = 555 ] && [ "$(cat "$out2/ro/subdir/f")" = deeper ] &&
	   [ "$(stat -c %a "$out2/suid")" = 4755 ] && [ "$(stat -c %a "$out2/sticky")" = 1777 ]; then
		echo "PASS unprivileged: $(grep -o 'owner_fail=[0-9]*' "$work/user2.out") counted, read-only directories filled, set-id bits kept"
	else
		echo "FAIL unprivileged extraction"
		cat "$work/user2.out"
		rc=1
	fi
	chmod -R u+rwx "$out" "$out2" 2>/dev/null || true
fi
if [ $rc -eq 0 ]; then
	chmod -R u+rwx "$work" 2>/dev/null || true
	rm -rf "$work"
fi
exit $rc
