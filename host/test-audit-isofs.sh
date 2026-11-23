#!/bin/bash
# Read-only and robustness audit of the ISO 9660 driver (kernel/fs/isofs.c)
# on the filesystem harness, on top of host/test-isofs.sh:
#
#   zerowrite   every image (tool-made and crafted: Rock Ridge, Joliet,
#               ISO 9660:1999, zisofs, multi-extent, interleaved, deep
#               relocation, CE chains, hidden/associated files, damaged
#               System Use data, multi-session) on 512-, 2048- and
#               4096-byte devices under the write trap: a read-write mount
#               request, walks, reads of every byte, readlink, xattrs,
#               statfs, sync, every modifying call (all refused with
#               EROFS), remounts and unmounts write NOTHING.
#   fuzz        the metadata fuzzer (host/fsimg/fuzz.py) with new seeds over
#               all of those images, every run under the write trap and with
#               a read-write mount attempt and modifying calls as well
#   suite       host/test-isofs.sh all (tools, crafted, checks,
#               regressions) and the > 4 GB multi-extent image
#
#   host/test-audit-isofs.sh [SECTION...]       default: all three
#   AUDIT_ISO_ITERS=N   fuzz runs per image and sector size (default 300)
#   AUDIT_ISO_SEED=N    first fuzz seed (default 9001)
#   AUDIT_JOBS=N        parallel fuzz runs (default 1; run the whole script
#                       inside a memory-capped scope, e.g. systemd-run
#                       --user --scope -p MemoryMax=...)
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
export TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-audit-isofs}
mkdir -p "$TESTFS_DIR" || exit 2
W=$(mktemp -d "$TESTFS_DIR/run.XXXXXX") || exit 2
ITERS=${AUDIT_ISO_ITERS:-300}
SEED0=${AUDIT_ISO_SEED:-9001}
JOBS=${AUDIT_JOBS:-1}
B=$(host/test-fs.sh isofs --build | tail -1) || exit 2
[ -x "$B" ] || { echo "test-audit-isofs: harness build failed"; exit 2; }
BIN="$W/test-fs-isofs"
cp "$B" "$BIN" || exit 2
# the fuzzer runs the binary it is given: this one always traps writes
cat > "$W/bin-trap" <<EOF
#!/bin/sh
exec "$BIN" --trap "\$@"
EOF
chmod +x "$W/bin-trap"

PASS=0
FAILED=0
FAILS=""
ok() { PASS=$((PASS + 1)); echo "PASS $1"; }
bad() {
	FAILED=$((FAILED + 1))
	FAILS="$FAILS $1"
	echo "FAIL $1"
	[ -f "$W/$1.out" ] && grep -E "FINDING|^FAIL|WRITE TRAP|BUG|runtime error|ERROR" "$W/$1.out" | head -20 | sed 's/^/    /'
}

IMGS=""
images() {
	local v
	mkdir -p "$W/img"
	for v in rr2048 rr1024 rr512 joliet plain iso1999 zisofs multi reloc ce special jcase damaged ceflood bigext ileave; do
		rm -rf "$W/img/cr-$v.stage"
		python3 -I host/test-isofs.py build "$v" "$W/img/cr-$v.iso" "$W/img/cr-$v.stage" > /dev/null 2>&1 &&
			IMGS="$IMGS $W/img/cr-$v.iso"
		rm -rf "$W/img/cr-$v.stage"
	done
	for v in rrj rr zisofs deep deeprel hybrid many joliet plain; do
		bash host/fsimg/mkimg.sh "iso-$v" "$W/img/iso-$v.iso" > /dev/null 2>&1 &&
			IMGS="$IMGS $W/img/iso-$v.iso"
	done
	# two sessions (genisoimage -C), as on a multi-session disc
	if command -v genisoimage > /dev/null; then
		local m="$W/img/ms"
		mkdir -p "$m/a" "$m/b"
		echo "session zero" > "$m/a/first.txt"
		echo "session one" > "$m/b/second.txt"
		genisoimage -quiet -R -J -o "$m/s1.iso" "$m/a" 2> /dev/null
		local s1=$(($(stat -c %s "$m/s1.iso") / 2048))
		local n=$(((s1 + 15) / 16 * 16 + 16))
		genisoimage -quiet -R -J -C "0,$n" -o "$m/s2.part" "$m/b" 2> /dev/null
		cp "$m/s1.iso" "$W/img/multisession.iso"
		truncate -s $((n * 2048)) "$W/img/multisession.iso"
		cat "$m/s2.part" >> "$W/img/multisession.iso"
		truncate -s $((($(stat -c %s "$W/img/multisession.iso") + 4095) / 4096 * 4096)) "$W/img/multisession.iso"
		IMGS="$IMGS $W/img/multisession.iso"
		rm -rf "$m"
	fi
}

# every modifying operation an ISO mount refuses, plus everything that
# reads; nothing may reach the device
ZW_CMDS="?mount rw; ?statfs; ?readall; ?manifest / +all; ?sync; =ROFS mkdir /audit_x; ?write /audit_f text:x; ?unlink /audit_f; ?rename /a /b; ?rmdir /audit_x; ?truncate /audit_f 0; ?symlink /a /audit_l; ?setxattr / user.a text:b; ?chmod / 0777; ?utimens / now now; ?remount rw; ?remount ro; ?sync; ?umount; mount ro; statfs; ?readall; ?manifest / +all; sync; ?remount ro; cycle; ?readall; umount"

zerowrite() {
	local img ss name
	[ -n "$IMGS" ] || images
	for img in $IMGS; do
		for ss in 512 2048 4096; do
			name="zw-$(basename "$img" .iso)-$ss"
			local cmds=$ZW_CMDS
			# 10000 names behind 30-area CE chains each: every lookup
			# is a budgeted scan, so a walk that stats each name is
			# quadratic -- a listing and a few names only
			case $img in
			*ceflood*) cmds="?mount rw; fopen 1 / rd; $(for k in 1 2 3 4 5 6 7 8; do echo -n "freaddir 1; "; done)fclose 1; stat /f00000; ?stat /f09999; =ROFS mkdir /x; ?sync; umount; mount ro; ?stat /f05000; umount" ;;
			esac
			if timeout 900 "$BIN" --trap --cow --allow-warn --sector "$ss" "$img" -c "$cmds" > "$W/$name.out" 2>&1; then
				ok "$name"
			else
				# a damaged image may refuse the mount, and items made
				# unreadable on purpose (damaged System Use data, a
				# sparse-file entry) fail to read: what counts is that
				# nothing was written and nothing crashed
				if grep -qE "WRITE TRAP|AddressSanitizer|runtime error|BUG|DEADLOCK" "$W/$name.out" ||
				   ! grep -q "^test-fs:.* writes=0 discards=0 " "$W/$name.out"; then
					bad "$name"
				elif grep -q "FAIL line [0-9]*: mount ro" "$W/$name.out" && [ "$ss" = 4096 ]; then
					# a 4096-byte device cannot hold 2048-byte blocks
					# at odd positions: a refused mount is right
					ok "$name"
				elif ! grep "^FAIL line" "$W/$name.out" | grep -vqE ": +\?(readall|manifest / \+all): (read|readlink|stat|readdir|listxattr|getxattr) .*: (IO|UNSUPPORTED|INVALID|NOT_FOUND|LOOP|NAMETOOLONG)$"; then
					ok "$name (items unreadable on purpose)"
				else
					bad "$name"
				fi
			fi
		done
	done
}

fuzz() {
	[ -n "$IMGS" ] || images
	local img ss seed=$SEED0 list=() n=0
	for img in $IMGS; do
		case $img in *ceflood*) continue ;; esac # see zerowrite
		for ss in 2048 512; do
			list+=("$img $ss $seed")
			seed=$((seed + 1))
		done
	done
	# both read-only walks and read-write attempts, under the trap
	local cmds="mount ro; readall; manifest / +all; umount; mount rw; readall; mkdir /fz; write /fz/f text:x; rename /a /b; unlink /a; umount"
	for j in "${list[@]}"; do
		set -- $j
		local name="fuzz-$(basename "$1" .iso)-$2-s$3"
		(
			if TESTFS_FUZZ_TIMEOUT=7200 timeout 7300 \
				python3 host/fsimg/fuzz.py --bin "$W/bin-trap" --fstype iso --image "$1" \
				--iters "$ITERS" --seed "$3" --sector "$2" --cmds "$cmds" --regress "$W/regress" \
				> "$W/$name.out" 2>&1; then
				echo "PASS $name $(tail -1 "$W/$name.out" | cut -c1-80)"
			else
				echo "FAIL $name"
				grep -A6 "^FINDING" "$W/$name.out" | head -30 | sed 's/^/    /'
			fi
		) > "$W/fjob.$n.log" 2>&1 &
		n=$((n + 1))
		while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
			wait -n
		done
	done
	wait
	local k=0
	while [ $k -lt $n ]; do
		cat "$W/fjob.$k.log"
		if grep -q "^PASS" "$W/fjob.$k.log"; then
			PASS=$((PASS + 1))
		else
			FAILED=$((FAILED + 1))
			FAILS="$FAILS $(head -1 "$W/fjob.$k.log" | cut -d' ' -f2)"
		fi
		k=$((k + 1))
	done
}

suite() {
	if TMPDIR="$W" bash host/test-isofs.sh all > "$W/suite.out" 2>&1; then
		ok "test-isofs-all"
	else
		bad "test-isofs-all"
		tail -20 "$W/suite.out"
	fi
	tail -1 "$W/suite.out" | sed 's/^/    /'
	if TMPDIR="$W" bash host/test-isofs.sh big > "$W/big.out" 2>&1; then
		ok "test-isofs-big"
	else
		bad "test-isofs-big"
		tail -20 "$W/big.out"
	fi
	rm -f "$W/likeos-test-isofs/iso-big.iso"
}

LIST=${*:-"zerowrite fuzz suite"}
for t in $LIST; do
	"$t"
done
echo "test-audit-isofs: $PASS passed, $FAILED failed"
[ -n "$FAILS" ] && echo "failed:$FAILS (outputs in $W)"
[ "$FAILED" = 0 ]
