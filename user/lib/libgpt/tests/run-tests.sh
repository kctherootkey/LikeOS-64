#!/bin/sh
# libgpt host tests: image files checked against the host's sgdisk/sfdisk.
#
#   sh tests/run-tests.sh TEST_GPT LIKEOS_GPT
#
# Every image is a sparse file in a fresh scratch directory that is removed
# at the end.  Nothing here needs root or touches a block device.
#
# Copyright (C) 2026 The LikeOS Project

set -u

TEST_GPT=$(readlink -f "$1")
CLI=$(readlink -f "$2")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libgpt-test.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1

PASS=0
FAIL=0

ok() {
	PASS=$((PASS + 1))
	echo "ok   $*"
}

bad() {
	FAIL=$((FAIL + 1))
	echo "FAIL $*"
}

# expect DESCRIPTION COMMAND... : the command must succeed
expect() {
	d=$1
	shift
	if "$@" >"$WORK/out" 2>&1; then
		ok "$d"
	else
		bad "$d"
		sed 's/^/     | /' "$WORK/out"
	fi
}

# expect_fail DESCRIPTION STATUS COMMAND... : must fail with STATUS
expect_fail() {
	d=$1
	want=$2
	shift 2
	"$@" >"$WORK/out" 2>&1
	st=$?
	if [ "$st" -eq "$want" ]; then
		ok "$d"
	else
		bad "$d (status $st, wanted $want)"
		sed 's/^/     | /' "$WORK/out"
	fi
}

# sgdisk -v must report a clean table.
sgdisk_clean() {
	out=$(sgdisk -v "$1" 2>&1)
	case "$out" in
	*"No problems found"*) return 0 ;;
	*) echo "$out"; return 1 ;;
	esac
}

# Compares every partition of `test_gpt dump` with `sgdisk -i`.
compare_with_sgdisk() {
	img=$1
	rc=0
	"$TEST_GPT" dump "$img" >"$WORK/dump" || return 1
	disk_guid=$(awk '$1 == "disk" { print toupper($2) }' "$WORK/dump")
	sg_guid=$(sgdisk -p "$img" | awk -F': ' '/Disk identifier/ { print $2 }')
	if [ "$disk_guid" != "$sg_guid" ]; then
		echo "disk GUID $disk_guid vs sgdisk $sg_guid"
		rc=1
	fi
	while read -r n first last type uuid attrs name; do
		[ "$n" = "disk" ] && continue
		sgdisk -i "$n" "$img" >"$WORK/info" 2>&1
		s_type=$(awk '/Partition GUID code/ { print $4 }' "$WORK/info")
		s_uuid=$(awk '/Partition unique GUID/ { print $4 }' "$WORK/info")
		s_first=$(awk '/First sector/ { print $3 }' "$WORK/info")
		s_last=$(awk '/Last sector/ { print $3 }' "$WORK/info")
		s_attrs=$(awk '/Attribute flags/ { print $3 }' "$WORK/info")
		s_name=$(sed -n "s/^Partition name: '\(.*\)'$/\1/p" "$WORK/info")
		up_type=$(echo "$type" | tr a-f A-F)
		up_uuid=$(echo "$uuid" | tr a-f A-F)
		up_attrs=$(echo "$attrs" | tr a-f A-F)
		if [ "$s_type" != "$up_type" ] || [ "$s_uuid" != "$up_uuid" ] ||
		   [ "$s_first" != "$first" ] || [ "$s_last" != "$last" ] ||
		   [ "$s_attrs" != "$up_attrs" ]; then
			echo "partition $n: libgpt $first-$last $up_type $up_uuid $up_attrs"
			echo "            sgdisk $s_first-$s_last $s_type $s_uuid $s_attrs"
			rc=1
		fi
		# sgdisk shows a character outside the basic plane (a
		# surrogate pair) as replacement bytes; sfdisk is the check
		# for those names (compare_names_with_sfdisk).
		if printf '%s' "$name" | LC_ALL=C grep -q "$(printf '\360')"; then
			:
		elif [ "$s_name" != "$name" ]; then
			echo "partition $n: name '$name' vs sgdisk '$s_name'"
			rc=1
		fi
	done <"$WORK/dump"
	return $rc
}

# The names as sfdisk reads them (UTF-8 through its own decoder).
compare_names_with_sfdisk() {
	img=$1
	"$TEST_GPT" dump "$img" | awk 'NR > 1 { $1=$2=$3=$4=$5=$6=""; sub(/^ +/, ""); print }' >"$WORK/n1"
	sfdisk --json "$img" | python3 -c '
import json, sys
for p in json.load(sys.stdin)["partitiontable"]["partitions"]:
    print(p.get("name", ""))' >"$WORK/n2"
	cmp -s "$WORK/n1" "$WORK/n2" || { diff "$WORK/n1" "$WORK/n2"; return 1; }
}

status_has() {
	"$TEST_GPT" status "$1" ${3:-} | grep -q -- "$2"
}

# Writes `count' bytes of 0xff at byte offset `off' of an image.
smash() {
	printf '\377\377\377\377\377\377\377\377' |
		dd of="$1" bs=1 seek="$2" count=8 conv=notrunc 2>/dev/null
}

echo "== unit tests"
expect "in-memory tests" "$TEST_GPT" unit

echo "== libgpt-written table, 512-byte sectors"
truncate -s 2G a.img
expect "build + reread" "$TEST_GPT" build a.img 512
expect "sgdisk -v is clean" sgdisk_clean a.img
expect "sfdisk --verify is clean" sfdisk --verify a.img
expect "every partition matches sgdisk -i" compare_with_sgdisk a.img
expect "UTF-8 names match sfdisk --json" compare_names_with_sfdisk a.img
expect "sfdisk sees a gpt label" sh -c "sfdisk --json a.img | python3 -c 'import json,sys; assert json.load(sys.stdin)[\"partitiontable\"][\"label\"] == \"gpt\"'"
expect "libgpt verify is clean" "$TEST_GPT" verify a.img
"$TEST_GPT" dump a.img >a.dump

echo "== libgpt-written table, 4096-byte sectors"
truncate -s 2G b4k.img
expect "build + reread (4K)" "$TEST_GPT" build b4k.img 4096
expect "first usable 6 on 4K" sh -c "'$TEST_GPT' dump b4k.img 4096 | awk '\$1==\"disk\" { exit !(\$3 == 6 && \$4 == 524282) }'"
expect "libgpt verify is clean (4K)" "$TEST_GPT" verify b4k.img 4096
if command -v fdisk >/dev/null 2>&1; then
	# fdisk can be told the sector size of an image (sfdisk cannot).
	expect "fdisk -b 4096 reads the 4K table" sh -c "
		'$TEST_GPT' dump b4k.img 4096 | awk 'NR > 1 { print \$1, \$2, \$3 }' | sort >s1
		LC_ALL=C fdisk -b 4096 -l b4k.img | sed -n 's/^b4k\.img\([0-9]*\) *\([0-9]*\) *\([0-9]*\) .*/\1 \2 \3/p' | sort >s2
		[ -s s2 ] && cmp -s s1 s2"
	expect "fdisk sees a gpt label on the 4K image" sh -c "LC_ALL=C fdisk -b 4096 -l b4k.img | grep -q 'Disklabel type: gpt'"
fi
expect "4K image read as 512 finds no GPT" sh -c "'$TEST_GPT' status b4k.img 512 | grep -q 'read=-2 '"

echo "== table written by sgdisk"
truncate -s 1G s.img
sgdisk -o -n 1:2048:+100M -t 1:ef00 -c 1:"EFI system partition" \
	-n 2:0:+300M -t 2:8304 -c 2:"likeos-root" -A 2:set:63 \
	-n 3:0:0 -t 3:8200 -c 3:"swap space" s.img >/dev/null 2>&1
expect "libgpt reads the sgdisk table" compare_with_sgdisk s.img
expect "libgpt verify of the sgdisk table" "$TEST_GPT" verify s.img
expect "sgdisk ESP starts at 2048" sh -c "'$TEST_GPT' dump s.img | awk '\$1==1 { exit !(\$2 == 2048 && \$4 == \"c12a7328-f81f-11d2-ba4b-00a0c93ec93b\") }'"
expect "attribute bit 63 read" sh -c "'$TEST_GPT' dump s.img | awk '\$1==2 { exit !(\$6 == \"8000000000000000\") }'"

echo "== damaged copies"
for what in primary-header primary-array backup-header backup-array; do
	cp --sparse=always a.img d.img
	size=$(stat -c %s d.img)
	case $what in
	primary-header) off=$((512 + 40)) ;;
	primary-array) off=$((1024 + 200)) ;;
	backup-header) off=$((size - 512 + 40)) ;;
	backup-array) off=$((size - 512 - 16384 + 300)) ;;
	esac
	smash d.img $off
	case $what in
	primary-header)
		expect "$what: detected" status_has d.img "primary_header=0"
		expect "$what: backup used" status_has d.img "using_backup=1" ;;
	primary-array)
		expect "$what: detected" status_has d.img "primary_entries=0"
		expect "$what: backup used" status_has d.img "using_backup=1" ;;
	backup-header)
		expect "$what: detected" status_has d.img "backup_header=0" ;;
	backup-array)
		expect "$what: detected" status_has d.img "backup_entries=0" ;;
	esac
	expect_fail "$what: libgpt verify reports it" 1 "$TEST_GPT" verify d.img
	expect_fail "$what: CLI verify reports it" 1 "$CLI" verify d.img
	# sgdisk rebuilds a damaged header in memory before it verifies,
	# so the damage shows in its warnings rather than in the verdict.
	if sgdisk -v d.img 2>&1 | grep -qE "ERROR|[Ii]nvalid|don't match|corrupt"; then
		ok "$what: sgdisk -v sees the damage"
	else
		bad "$what: sgdisk -v should see the damage"
	fi
	expect "$what: repair" "$TEST_GPT" repair d.img
	expect "$what: all copies good" status_has d.img \
		"primary_header=1 primary_entries=1 backup_header=1 backup_entries=1 backup_not_at_end=0"
	expect "$what: sgdisk -v clean after repair" sgdisk_clean d.img
	"$TEST_GPT" dump d.img >d.dump
	expect "$what: same layout as before" cmp -s a.dump d.dump
done
cp --sparse=always a.img d.img
size=$(stat -c %s d.img)
smash d.img $((512 + 40))
smash d.img $((size - 512 + 40))
expect "both headers damaged: read fails with EBADMSG" status_has d.img "read=-74 "
expect_fail "both headers damaged: repair refuses" 1 "$TEST_GPT" repair d.img

echo "== grown disk"
cp --sparse=always a.img g.img
truncate -s 3G g.img
expect "backup not at the end detected" status_has g.img "backup_not_at_end=1"
expect "primary still used" status_has g.img "using_backup=0"
expect "repair moves the backup" "$TEST_GPT" repair g.img
expect "backup at the end now" status_has g.img "backup_not_at_end=0"
expect "sgdisk -v clean after the move" sgdisk_clean g.img
expect "usable area widened" sh -c "'$TEST_GPT' dump g.img | awk '\$1==\"disk\" { exit !(\$4 == 3*1024*1024*2 - 34) }'"
expect "CLI repair on a clean disk" "$CLI" repair g.img

echo "== legacy MBR with logical partitions"
truncate -s 1G m.img
sfdisk -q m.img >/dev/null 2>&1 <<'EOF'
label: dos
label-id: 0x12345678
start=2048, size=200MiB, type=83, bootable
size=100MiB, type=7
type=5
size=100MiB, type=83
size=50MiB, type=82
type=c
EOF
"$TEST_GPT" mbr m.img >m.out
expect "kind legacy, signature" grep -q "kind=legacy signature=12345678 chain_broken=0" m.out
sfdisk -d m.img | sed -n 's/^m\.img\([0-9]*\) : start= *\([0-9]*\), size= *\([0-9]*\), type=\([0-9a-f]*\).*/\1 \2 \3 \4/p' >m.sf
awk '{ print $1, $2, $3, $4 }' m.out | sed 1d >m.lib
expect "MBR partitions match sfdisk -d" cmp -s m.sf m.lib
expect "three logical partitions" sh -c "[ \$(grep -c logical m.out) -eq 3 ]"
expect "CLI prints the MBR" sh -c "'$CLI' print m.img | grep -q 'MBR (legacy)'"
expect "CLI MBR JSON parses" sh -c "'$CLI' --json print m.img | python3 -c 'import json,sys; d=json.load(sys.stdin); assert len(d[\"partitions\"]) == 6'"
expect_fail "MBR disk: add refuses (no GPT)" 1 "$CLI" add m.img --size 1M

echo "== protective MBR"
expect "a.img MBR is protective" sh -c "'$TEST_GPT' mbr a.img | grep -q 'kind=protective'"
expect "0xee entry covers the disk" sh -c "'$TEST_GPT' mbr a.img | awk 'NR==2 { exit !(\$2 == 1 && \$3 == 2*1024*1024*2 - 1 && \$4 == \"ee\") }'"

echo "== likeos-gpt command line"
truncate -s 4G c.img
expect "new-table" "$CLI" new-table c.img
expect "add ESP" "$CLI" add c.img --type esp --size 512M --name "EFI system partition"
expect "add root (rest)" "$CLI" add c.img -t likeos-root -c "likeos-root"
expect "sgdisk -v clean" sgdisk_clean c.img
expect "ESP at 2048, 1048576 sectors" sh -c "'$CLI' --json print c.img | python3 -c '
import json, sys
d = json.load(sys.stdin)
p = d[\"partitions\"]
assert d[\"label\"] == \"gpt\" and d[\"sector_size\"] == 512
assert p[0][\"start\"] == 2048 and p[0][\"sectors\"] == 1048576, p[0]
assert p[0][\"code\"] == \"ef00\" and p[0][\"name\"] == \"EFI system partition\"
assert p[1][\"start\"] == 2048 + 1048576 and p[1][\"end\"] == d[\"last_usable\"]
assert p[1][\"type\"] == \"4f68bce3-e8cd-4db1-96e7-fbcaf984b709\"
assert d[\"free\"] == []
'"
expect "print (text)" sh -c "'$CLI' print c.img | grep -q 'EF00  EFI system partition'"
expect "verify" "$CLI" verify c.img
expect "verify --json" sh -c "'$CLI' --json verify c.img | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"errors\"] == 0'"
expect "resize 2 to 1G" "$CLI" resize c.img 2 1G
expect "resized size" sh -c "'$CLI' --json info c.img 2 | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"sectors\"] == 2097152, d'"
expect "add into the freed space" "$CLI" add c.img -n 5 --type swap --size 256M --name swap
expect "name with umlauts" "$CLI" name c.img 2 "Wurzel äöü"
expect "type 2 data" "$CLI" type c.img 2 8300
expect "attrs 1" "$CLI" attrs c.img 1 0x1
expect "sgdisk agrees after edits" compare_with_sgdisk c.img
expect "resize 2 max" "$CLI" resize c.img 2 max
expect "delete 1" "$CLI" delete c.img 1
expect "info 1 is gone" sh -c "! '$CLI' info c.img 1 2>/dev/null"
expect "add at an explicit start" "$CLI" add c.img -n 1 --start 2048 --size 100M --type esp --uuid 01234567-89ab-cdef-0123-456789abcdef
expect "explicit GUID kept" sh -c "'$CLI' --json info c.img 1 | grep -q '01234567-89ab-cdef-0123-456789abcdef'"
expect "sgdisk -v clean after edits" sgdisk_clean c.img
expect "sfdisk --verify clean after edits" sfdisk --verify c.img
expect_fail "overlapping add fails" 1 "$CLI" add c.img --start 4096 --size 1M
expect_fail "duplicate number fails" 1 "$CLI" add c.img -n 2 --size 1M
expect_fail "new-table over partitions needs --yes" 1 "$CLI" new-table c.img
expect_fail "wipe needs --yes" 1 "$CLI" wipe c.img
expect_fail "unknown command" 2 "$CLI" frobnicate c.img
expect_fail "unknown type" 2 "$CLI" type c.img 2 nosuchtype
expect_fail "missing device" 2 "$CLI" print
expect "types --json" sh -c "'$CLI' --json types | python3 -c 'import json,sys; t=json.load(sys.stdin); assert any(x[\"alias\"] == \"likeos-root\" for x in t)'"
expect "types (text)" sh -c "'$CLI' types | grep -q '^EF00'"
expect "new-table --yes" "$CLI" --yes new-table c.img
expect "empty after new-table" sh -c "'$CLI' --json print c.img | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"partitions\"] == [] and len(d[\"free\"]) == 1'"
expect "wipe --yes" "$CLI" wipe --yes c.img
expect "no table after wipe" sh -c "'$CLI' print c.img | grep -q 'No partition table'"
expect "4K image via --sector-size" sh -c "'$CLI' --sector-size 4096 --json print b4k.img | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"sector_size\"] == 4096 and len(d[\"partitions\"]) == 3'"

echo "== regressions"
truncate -s 64M h.img
expect "crafted headers: read, repair, wipe" "$TEST_GPT" headers h.img 512
truncate -s 64M h4k.img
expect "crafted headers: read, repair, wipe (4K)" "$TEST_GPT" headers h4k.img 4096
# 40 sectors: room for gpt_wipe, too small for a table.  A failing
# new-table must leave the disk as it was.
truncate -s 20K small.img
printf 'LikeOS marker' | dd of=small.img conv=notrunc 2>/dev/null
cp small.img small.orig
expect_fail "new-table on a too small disk fails" 1 "$CLI" new-table small.img
expect "a failed new-table leaves the disk untouched" cmp -s small.img small.orig
truncate -s 64M r.img
expect "new-table (r.img)" "$CLI" new-table r.img
expect "add 8M (r.img)" "$CLI" add r.img --size 8M
expect_fail "add --size 0 is refused" 2 "$CLI" add r.img --size 0
expect_fail "add --size 100B (0 sectors) is refused" 2 "$CLI" add r.img --size 100B
expect_fail "add --start -1 is refused" 2 "$CLI" add r.img --start -1 --size 1M
expect_fail "add --size -1 is refused" 2 "$CLI" add r.img --size -1
expect_fail "resize to 0 is refused" 2 "$CLI" resize r.img 1 0
# After "--" a leading '-' is a value, not an option.
expect_fail "resize to -0 is refused" 2 "$CLI" resize r.img 1 -- -0
expect_fail "attrs -1 is refused" 2 "$CLI" attrs r.img 1 -- -1
expect "r.img unchanged by the refused commands" sh -c "'$CLI' --json print r.img | python3 -c '
import json, sys
d = json.load(sys.stdin)
p = d[\"partitions\"]
assert len(p) == 1 and p[0][\"start\"] == 2048 and p[0][\"sectors\"] == 16384, p
assert p[0][\"attributes\"] == \"0000000000000000\", p
'"

echo
echo "libgpt host tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
