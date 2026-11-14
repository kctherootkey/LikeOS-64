#!/bin/sh
# The body of run_tests.sh, run inside one fakeroot session.
#
#   fakeroot_tests.sh TESTDIR EXTRACT_TEST WORKDIR
#
# Copyright (C) 2026 The LikeOS Project

set -u

here=$1
bin=$2
W=$3
nozstd=$(dirname "$bin")/extract_test_nozstd
pass=0
fail=0

ok() { pass=$((pass + 1)); echo "PASS $*"; }
bad() { fail=$((fail + 1)); echo "FAIL $*"; }

die() { echo "fakeroot_tests.sh: $*" >&2; exit 2; }

cd "$W" || die "no work dir"
SRC=$W/src
mkdir -p "$SRC" "$W/outside"

# ---- the source tree ------------------------------------------------
build_tree() {
	cd "$SRC" || die "no src"
	: > empty
	echo hello > small.txt
	head -c 3000000 /dev/urandom > big.bin
	echo setuid > suid
	chmod 4755 suid
	echo setgid > sgid
	chmod 2755 sgid
	mkdir sticky
	chmod 1777 sticky
	echo linked > hl1
	mkdir sub
	ln hl1 hl2
	ln hl1 sub/hl3
	ln -s small.txt rel_link
	ln -s /etc/hostname abs_link
	ln -s nowhere/at/all dangling
	ln -s sub dirlink
	mkfifo fifo
	mknod chr c 1 3
	mknod blk b 8 1
	long=$(printf 'n%.0s' $(seq 1 150))
	mkdir -p "long/$long/$long"
	echo long > "long/$long/$long/file_$long"
	ln -s "long/$long/$long/file_$long" "longlink_$long"
	echo umlaut > 'Grüße.txt'
	echo cjk > '日本語ファイル.txt'
	echo emoji > 'emoji_🎉.txt'
	d=deep
	for i in $(seq 1 40); do d=$d/level$i; done
	mkdir -p "$d"
	echo bottom > "$d/f"
	truncate -s 50M sparse.img
	printf 'start' | dd of=sparse.img conv=notrunc status=none
	printf 'middle' | dd of=sparse.img bs=1 seek=20971520 conv=notrunc status=none
	printf 'end' | dd of=sparse.img bs=1 seek=52428797 conv=notrunc status=none
	truncate -s 10M sparse_hole_end.img
	printf 'head' | dd of=sparse_hole_end.img conv=notrunc status=none
	mkdir ro
	echo inside > ro/inside
	mkdir ro/subdir
	echo deeper > ro/subdir/f
	chmod 0555 ro/subdir
	chmod 0555 ro
	mkdir priv
	echo secret > priv/f
	chmod 0700 priv
	echo acl > acl_file
	mkdir acl_dir
	echo owned > owned1
	echo owned > owned2
	echo owned > owned3
	echo big-id > bigid
	chown 1000:1000 small.txt owned1
	chown 0:29 owned2
	chown 65534:65534 owned3 sub
	chown -h 1000:1000 rel_link
	chown 1000:1000 chr
	if ! chown 3000000000:3000000000 bigid 2>/dev/null; then
		echo "note: fakeroot refused uid 3000000000"
	fi
	python3 -I - "$SRC" <<'PY' || die "xattrs could not be set"
import os, struct, sys
root = sys.argv[1]
def p(n): return os.path.join(root, n)
os.setxattr(p("small.txt"), "user.comment", b"a small file")
os.setxattr(p("big.bin"), "user.binary", bytes(range(256)) + b"\0\n\0")
os.setxattr(p("big.bin"), "user.long", b"x" * 1000)
os.setxattr(p("sub"), "user.dir", b"on a directory")
os.setxattr(p("suid"), "user.with=equals and space", b"v")
os.setxattr(p("sgid"), "user.percent%25and%3Dthings", b"w")
# ACLs as their binary attribute values (version 2; tag, perm, id)
def acl(ents):
    b = struct.pack("<I", 2)
    for t, perm, i in ents:
        b += struct.pack("<HHI", t, perm, i)
    return b
U = 0xffffffff
os.setxattr(p("acl_file"), "system.posix_acl_access",
            acl([(1, 6, U), (2, 4, 1000), (4, 4, U), (8, 6, 29), (16, 6, U), (32, 0, U)]))
os.setxattr(p("acl_dir"), "system.posix_acl_access",
            acl([(1, 7, U), (2, 5, 1000), (4, 5, U), (16, 5, U), (32, 5, U)]))
os.setxattr(p("acl_dir"), "system.posix_acl_default",
            acl([(1, 7, U), (2, 7, 1000), (4, 5, U), (16, 7, U), (32, 0, U)]))
PY
	# Time stamps with nanoseconds, the directories' last.
	find . ! -type d -exec touch -h -d '2023-05-06 07:08:09.123456789' {} +
	touch -h -d '2021-01-02 03:04:05.987654321' rel_link small.txt
	chmod u+w ro ro/subdir
	find . -depth -type d -exec touch -d '2022-02-03 04:05:06.555555555' {} +
	chmod 0555 ro/subdir ro
	cd "$W" || die "no work dir"
}

build_tree
echo "tree built: $(find "$SRC" | wc -l) objects"

# ---- archives -------------------------------------------------------
PAXOPT='--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime'
mk_posix() {
	out=$1
	shift
	tar --format=posix --sort=name --numeric-owner --xattrs \
	    --xattrs-include='*' --acls --sparse "$PAXOPT" "$@" \
	    -C "$SRC" -cf "$out" . || die "tar failed for $out"
}
mk_posix posix.tar
mk_posix sp00.tar --sparse-version=0.0
mk_posix sp01.tar --sparse-version=0.1
mk_posix sp10.tar --sparse-version=1.0
tar --format=gnu --sort=name --numeric-owner --sparse -C "$SRC" -cf gnu.tar . \
	2>gnu.err || { cat gnu.err; die "tar --format=gnu failed"; }
# for the unprivileged run of run_tests.sh: no device nodes
tar --format=posix --numeric-owner --sort=name --sparse \
	--exclude=./chr --exclude=./blk -C "$SRC" -cf nodev.tar . || die "nodev.tar"
for t in posix sp00 sp01 sp10 gnu; do
	zstd -19 -T0 -q -f "$t.tar" -o "$t.tar.zst" || die "zstd $t"
done
# two frames: the tar cut in two, each half compressed on its own
size=$(stat -c %s posix.tar)
half=$((size / 2 + 777))
head -c $half posix.tar | zstd -3 -q -c > multi.tar.zst
tail -c +$((half + 1)) posix.tar | zstd -3 -q -c >> multi.tar.zst
# a skippable frame in front
python3 -I -c 'import struct,sys; sys.stdout.buffer.write(struct.pack("<II", 0x184D2A50, 8) + b"LikeOS!!")' > skip.tar.zst
cat posix.tar.zst >> skip.tar.zst

sha() { sha256sum "$1" | cut -d' ' -f1; }

# ---- extraction + comparison ---------------------------------------
check_extract() {
	name=$1
	archive=$2
	tarfile=$3
	cmpopts=$4
	out=$W/out_$name
	mkdir -p "$out"
	if "$bin" --log "$W/$name.log" --sha "$(sha "$archive")" "$archive" "$out" > "$W/$name.out" 2>&1; then
		:
	else
		bad "$name: extraction failed"
		cat "$W/$name.out"
		tail -20 "$W/$name.log"
		return
	fi
	if grep -q "xattr_fail=0 acl_fail=0 owner_fail=0 time_fail=0 skipped=0" "$W/$name.out"; then
		:
	else
		bad "$name: failures counted"
		cat "$W/$name.out"
		grep -i warn "$W/$name.log" | head
	fi
	tar_x=
	case $cmpopts in *--no-xattrs*) ;; *) tar_x="--xattrs --xattrs-include=* --acls" ;; esac
	# shellcheck disable=SC2086
	if (cd "$out" && tar -d --numeric-owner $tar_x -f "$W/$tarfile") > "$W/$name.tard" 2>&1; then
		ok "$name: tar --compare"
	else
		bad "$name: tar --compare"
		head -30 "$W/$name.tard"
	fi
	# shellcheck disable=SC2086
	if python3 -I "$here/compare_trees.py" $cmpopts "$SRC" "$out" > "$W/$name.cmp" 2>&1; then
		ok "$name: compare_trees ($(tail -1 "$W/$name.cmp"))"
	else
		bad "$name: compare_trees"
		head -30 "$W/$name.cmp"
	fi
}

check_extract posix_zst posix.tar.zst posix.tar ""
check_extract posix_plain posix.tar posix.tar ""
check_extract sparse00 sp00.tar.zst sp00.tar ""
check_extract sparse01 sp01.tar.zst sp01.tar ""
check_extract sparse10 sp10.tar.zst sp10.tar ""
check_extract gnu gnu.tar.zst gnu.tar "--seconds --no-xattrs"
check_extract multiframe multi.tar.zst posix.tar ""
check_extract skippable skip.tar.zst posix.tar ""

# sparse files really are sparse again
alloc=$(du -k "$W/out_posix_zst/sparse.img" | cut -f1)
if [ "$alloc" -lt 4096 ]; then
	ok "sparse.img unpacked with holes (${alloc} KiB allocated of 51200)"
else
	bad "sparse.img unpacked without holes (${alloc} KiB allocated)"
fi

# entry types in the archive headers
types=$(python3 -I -c '
import tarfile,sys
t = tarfile.open(sys.argv[1])
print(sorted({m.type.decode() if isinstance(m.type, bytes) else m.type for m in t.getmembers()}))' gnu.tar)
echo "gnu.tar member types: $types"
if tar -tvf gnu.tar --numeric-owner | grep -q 3000000000; then
	ok "gnu.tar carries uid 3000000000 (base-256)"
else
	echo "note: no uid 3000000000 in the archive"
fi

# ---- a second extraction over the first (replacing everything) ------
if "$bin" --log "$W/again.log" posix.tar.zst "$W/out_posix_zst" > "$W/again.out" 2>&1 &&
   python3 -I "$here/compare_trees.py" "$SRC" "$W/out_posix_zst" > "$W/again.cmp" 2>&1; then
	ok "extracting again over an existing tree"
else
	bad "extracting again over an existing tree"
	cat "$W/again.out" "$W/again.cmp" | head -30
fi

# ---- pax records GNU tar does not write here --------------------------
python3 -I "$here/pax_records.py" make pax.tar || die "pax.tar"
mkdir -p "$W/out_pax"
if "$bin" --log "$W/pax.log" pax.tar "$W/out_pax" > "$W/pax.out" 2>&1 &&
   python3 -I "$here/pax_records.py" check "$W/out_pax" > "$W/pax.cmp" 2>&1; then
	ok "pax records: ACL text, LIBARCHIVE.xattr, fractions, global header"
else
	bad "pax records"
	cat "$W/pax.out" "$W/pax.cmp" | head -20
fi

# ---- negative tests -------------------------------------------------
python3 -I "$here/make_bad_archives.py" "$W" "$W/outside" || die "bad archives"

expect() {
	name=$1
	err=$2
	shift 2
	out=$W/neg_$name
	mkdir -p "$out"
	if "$bin" --log "$W/neg_$name.log" --expect "$err" "$@" "$out" > "$W/neg_$name.out" 2>&1; then
		ok "$name: fails with $err ($(grep '^error:' "$W/neg_$name.out" | head -1))"
	else
		bad "$name: expected $err"
		cat "$W/neg_$name.out"
	fi
}

cp posix.tar corrupt.tar
printf 'Z' | dd of=corrupt.tar bs=1 seek=3 conv=notrunc status=none
expect corrupt EBADMSG corrupt.tar
head -c $((size / 2)) posix.tar > trunc.tar
expect truncated EBADMSG trunc.tar
zsize=$(stat -c %s posix.tar.zst)
head -c $((zsize / 2)) posix.tar.zst > trunc.tar.zst
expect truncated_zst EBADMSG trunc.tar.zst
expect wrong_sha EBADMSG --sha 0000000000000000000000000000000000000000000000000000000000000000 posix.tar.zst
expect cancel ECANCELED --cancel-after 5 posix.tar.zst
expect dotdot EPERM dotdot.tar
if [ -e "$W/escape.txt" ] || [ -e "$W/neg_dotdot/../escape.txt" ]; then
	bad "dotdot: a file was written outside the target"
fi
expect symlink_escape ELOOP symesc.tar
expect symlink_escape2 ELOOP symesc2.tar
expect hardlink_escape EPERM hlesc.tar
mkdir -p "$W/neg_planted_symlink"
ln -s "$W/outside" "$W/neg_planted_symlink/pre"
expect planted_symlink ELOOP pre.tar
if [ -n "$(ls -A "$W/outside")" ]; then
	bad "something was written outside the target: $(ls -A "$W/outside")"
else
	ok "nothing was written outside the target"
fi

# absolute names are unpacked below the target
mkdir -p "$W/abs_out"
if "$bin" --log "$W/abs.log" abs.tar "$W/abs_out" > "$W/abs.out" 2>&1 &&
   [ -f "$W/abs_out/abs/file.txt" ]; then
	ok "absolute name unpacked below the target"
else
	bad "absolute name"
	cat "$W/abs.out"
fi

# a planted symbolic link at a file's name is replaced, not written through
echo "original" > "$W/outside_victim"
mkdir -p "$W/over_out"
ln -s "$W/outside_victim" "$W/over_out/victim"
if "$bin" --log "$W/over.log" over.tar "$W/over_out" > "$W/over.out" 2>&1 &&
   [ "$(cat "$W/outside_victim")" = original ] &&
   [ -f "$W/over_out/victim" ] && [ ! -L "$W/over_out/victim" ]; then
	ok "planted symlink replaced, its target untouched"
else
	bad "planted symlink"
	cat "$W/over.out"
fi

# a pax size record replaces the header's size field
mkdir -p "$W/paxsize_out"
if "$bin" --log "$W/paxsize.log" paxsize.tar "$W/paxsize_out" > "$W/paxsize.out" 2>&1 &&
   [ "$(cat "$W/paxsize_out/big.txt")" = "thirteen byte" ] &&
   [ "$(cat "$W/paxsize_out/after.txt")" = after ]; then
	ok "pax size record used"
else
	bad "pax size record"
	cat "$W/paxsize.out"
fi

# an owner out of range is not set, and then neither is a set-id bit
mkdir -p "$W/bigsuid_out"
if "$bin" --log "$W/bigsuid.log" bigsuid.tar "$W/bigsuid_out" > "$W/bigsuid.out" 2>&1 &&
   [ "$(stat -c %a "$W/bigsuid_out/bigsuid")" = 755 ]; then
	ok "set-id bit dropped with the owner out of range"
else
	bad "set-id bit with the owner out of range: $(stat -c %a "$W/bigsuid_out/bigsuid" 2>&1)"
	cat "$W/bigsuid.out"
fi

# a build without zstd: plain tar works, zstd is refused clearly
if [ -x "$nozstd" ]; then
	mkdir -p "$W/nz_plain" "$W/nz_zst"
	if "$nozstd" --log "$W/nz.log" posix.tar "$W/nz_plain" > "$W/nz.out" 2>&1 &&
	   python3 -I "$here/compare_trees.py" "$SRC" "$W/nz_plain" > "$W/nz.cmp" 2>&1; then
		ok "built without zstd: plain tar"
	else
		bad "built without zstd: plain tar"
		cat "$W/nz.out" "$W/nz.cmp" | head -20
	fi
	if "$nozstd" --log "$W/nz2.log" --expect ENOSYS posix.tar.zst "$W/nz_zst" > "$W/nz2.out" 2>&1; then
		ok "built without zstd: $(grep '^error:' "$W/nz2.out")"
	else
		bad "built without zstd: zstd archive"
		cat "$W/nz2.out"
	fi
fi

echo "extractor tests: $pass passed, $fail failed"
[ $fail -eq 0 ]
