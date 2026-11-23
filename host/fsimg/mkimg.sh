#!/bin/bash
# Test images for the filesystem harness, made with the host's own tools.
#
#   host/fsimg/mkimg.sh VARIANT OUT        make one image
#   host/fsimg/mkimg.sh --list             the variants
#
# Next to OUT it writes OUT.manifest (what the image holds, in the
# runner's manifest format) where that is exactly known, and OUT.info
# (one line: fstype sector-size notes).  Exit status 77 when a needed host
# tool is missing (the reason on stderr), 0 on success.  Staging trees are
# deterministic (mktree.py, seed 1); images carry the tools' own volume
# serials/timestamps, so they are not byte-identical between runs.
#
# Variants:
#   fat-basic fat-many fat-deep                    mkfs.fat -F 32 + mtools
#   ext4-basic ext4-1k ext4-many ext4-deep         mkfs.ext4 -d
#   ntfs-{512,4k,64k}-{basic,full,many,deep,big,empty}
#                                                  mkntfs + ntfs-populate
#     full = basic tree + compressed files, ADS, sparse file, EA, junction,
#            Interix link, hard links
#   ntfs-4k-{dirty,hiber,logdirty,mirrbad}         gate cases (basic tree)
#   ntfs-4k-win                                    a volume laid out like a
#            system drive: basic tree + Windows/Users/ProgramData folders,
#            pagefile.sys, swapfile.sys, zeroed hiberfil.sys, $UsnJrnl
#            ($Max, $J) under $Extend, DOS 8.3 names, junctions, directory
#            symlink, compressed / sparse files, ADS, EAs, hard links,
#            hidden/system attributes (no .manifest: read through ntfs-3g)
#   exfat-{4k,128k,32m}-{basic,many,deep,big,frag,empty}
#     (32m: basic is a small tree, many is refused; images are sparse)
#                                                  mkfs.exfat + exfat-populate
#   exfat-4k-{dirty,badboot}                       gate cases
#   iso-{rrj,rr,zisofs,deep,deeprel,hybrid,many}   xorriso (Rock Ridge; deeprel
#                                                  relocates deep directories:
#                                                  CL/PL/RE)
#   iso-joliet iso-plain                           genisoimage (no Rock Ridge)
#   iso-big                                        > 4 GB file (multi-extent;
#                                                  sparse on the host, needs
#                                                  ~9 GB free, kept sparse)
#
# Copyright (C) 2026 The LikeOS Project

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MKTREE="python3 $HERE/mktree.py"

need() {
	for t in "$@"; do
		if ! command -v "$t" >/dev/null 2>&1; then
			echo "mkimg: host tool $t is missing" >&2
			exit 77
		fi
	done
}

if [ "${1:-}" = "--list" ]; then
	sed -n '/^# Variants:/,/^# Copyright/p' "$0" | sed '$d;s/^# //'
	exit 0
fi
[ $# -eq 2 ] || { echo "usage: mkimg.sh VARIANT OUT | --list" >&2; exit 2; }
V=$1
OUT=$2
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mkimg.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT
rm -f "$OUT" "$OUT.manifest" "$OUT.info"

# stage PROFILE [mktree options]: a tree in $WORK/src
stage() {
	local p=$1
	shift
	rm -rf "$WORK/src"
	$MKTREE tree "$p" 1 "$WORK/src" "$@" || exit 2
}

info() {
	echo "$*" > "$OUT.info"
}

case $V in
fat-*)
	need mkfs.fat mcopy mmd
	case ${V#fat-} in
	basic) stage basic --no-symlinks --no-hardlinks ;;
	many) stage many --count 1500 ;;
	deep) stage deep ;;
	*) echo "mkimg: unknown variant $V" >&2; exit 2 ;;
	esac
	truncate -s 128M "$OUT"
	mkfs.fat -F 32 -n LIKEOSTEST "$OUT" >/dev/null || exit 1
	(cd "$WORK/src" && for e in *; do
		mcopy -s -i "$OUT" "$e" ::/ || exit 1
	done) || exit 1
	$MKTREE manifest "$WORK/src" > "$OUT.manifest"
	info "fat32 512"
	;;
ext4-*)
	need mkfs.ext4
	opt=""
	case ${V#ext4-} in
	basic) stage basic ;;
	1k) stage basic; opt="-b 1024" ;;
	many) stage many ;;
	deep) stage deep ;;
	*) echo "mkimg: unknown variant $V" >&2; exit 2 ;;
	esac
	mkfs.ext4 -q -F $opt -E root_owner=0:0 -d "$WORK/src" "$OUT" 64M >/dev/null 2>&1 || exit 1
	{ $MKTREE manifest "$WORK/src"; } | sed '1a d /lost+found' > "$OUT.manifest"
	info "ext4 512"
	;;
ntfs-*)
	need mkntfs gcc
	rest=${V#ntfs-}
	cl=${rest%%-*}
	content=${rest#*-}
	case $cl in
	512) csz=512 ;;
	4k) csz=4096 ;;
	64k) csz=65536 ;;
	*) echo "mkimg: unknown cluster size in $V" >&2; exit 2 ;;
	esac
	POP="${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-fs}/ntfs-populate"
	mkdir -p "$(dirname "$POP")"
	if [ ! -x "$POP" ] || [ "$HERE/ntfs-populate.c" -nt "$POP" ]; then
		lib=$(ldconfig -p 2>/dev/null | awk '/libntfs-3g\.so\./ {print $NF; exit}')
		if [ -z "$lib" ]; then
			echo "mkimg: libntfs-3g is missing (package ntfs-3g)" >&2
			exit 77
		fi
		gcc -O1 -o "$POP" "$HERE/ntfs-populate.c" "-l:$(basename "$lib")" || exit 77
	fi
	size=96M
	script="$WORK/script"
	: > "$script"
	tweak=""
	case $content in
	empty) ;;
	basic|dirty|hiber|logdirty|mirrbad)
		stage basic
		echo "copytree $WORK/src" >> "$script"
		case $content in
		dirty) tweak=dirty ;;
		hiber) echo "file /hiberfil.sys zero:65536" >> "$script"; tweak=hiber ;;
		logdirty) tweak=logfile-unclean ;;
		mirrbad) tweak=mftmirr-mismatch ;;
		esac
		;;
	full)
		stage basic
		cat >> "$script" <<-EOF
		copytree $WORK/src
		mkdir /extra
		file /extra/plain pat:1:100000
		ads /extra/plain stream.one pat:2:5000
		ads /extra/plain tiny pat:3:10
		ads /extra/plain big pat:9:300000
		compressed /extra/compressed pat:4:400000
		compressed /extra/compressed_small pat:5:3000
		compdir /extra/cdir
		file /extra/cdir/inherited pat:6:150000
		file /extra/cdir/empty zero:0
		sparse /extra/sparse 20000000 0:pat:7:5000 9000000:pat:8:70000 19999000:pat:10:1000
		sparse /extra/allhole 5000000
		ea /extra/plain USER.TEST hello
		junction /extra/junction C:\\Windows
		intxlink /extra/intx /extra/plain
		link /extra/plain /extra/plain_hardlink
		attrib /extra/plain 0x2
		EOF
		;;
	win)
		stage basic
		cat >> "$script" <<-EOF
		copytree $WORK/src
		mkdir /Windows
		mkdir /Windows/System32
		file /Windows/System32/ntoskrnl.exe pat:11:300000
		ads /Windows/System32/ntoskrnl.exe Zone.Identifier pat:16:30
		ea /Windows/System32/ntoskrnl.exe USER.KERNEL hello
		compressed /Windows/System32/driver_store.dat pat:13:200000
		sparse /Windows/System32/sparse.etl 8000000 0:pat:14:4096 7000000:pat:15:4096
		link /Windows/System32/ntoskrnl.exe /Windows/ntoskrnl_link.exe
		mkdir /Program_Files
				mkdir /Program_Files/Common_Files_Folder
		file /Program_Files/Common_Files_Folder/Application_Settings.json pat:19:5000
		link /Program_Files/Common_Files_Folder/Application_Settings.json /Program_Files/Common_Files_Folder/APPLIC~1.JSO
		mkdir /ProgramData
		mkdir /Users
		mkdir /Users/Default
		file /Users/Default/NTUSER.DAT pat:12:262144
		file /Users/Default/Long_Document_Name.docx pat:20:40000
		link /Users/Default/Long_Document_Name.docx /Users/Default/LONG_D~1.DOC
		junction /Documents_and_Settings C:\\Users
		symlink /Users/All_Users /ProgramData dir
		file /pagefile.sys zero:1048576
		file /swapfile.sys zero:262144
		file /hiberfil.sys zero:65536
		attrib /pagefile.sys 0x26
		attrib /swapfile.sys 0x26
		attrib /hiberfil.sys 0x26
		file /\$Extend/\$UsnJrnl zero:0
		ads /\$Extend/\$UsnJrnl \$Max pat:17:32
		ads /\$Extend/\$UsnJrnl \$J pat:18:65536
		EOF
		tweak="dospair-APPLIC~1.JSO dospair-LONG_D~1.DOC"
		;;
	many) stage many; echo "copytree $WORK/src" >> "$script" ;;
	deep) stage deep; echo "copytree $WORK/src" >> "$script" ;;
	big) stage big --big 80; size=200M; echo "copytree $WORK/src" >> "$script" ;;
	*) echo "mkimg: unknown variant $V" >&2; exit 2 ;;
	esac
	truncate -s "$size" "$OUT"
	mkntfs -F -f -q -c "$csz" -s 512 -L LIKEOSTEST "$OUT" >/dev/null 2>&1 || exit 1
	"$POP" "$OUT" "$script" || exit 1
	[ -n "$tweak" ] && { python3 "$HERE/fsparse.py" ntfs-tweak "$OUT" $tweak || exit 1; }
	if [ -d "$WORK/src" ]; then
		$MKTREE manifest "$WORK/src" > "$OUT.manifest"
		[ "$content" = hiber ] &&
			echo "f /hiberfil.sys 65536 ?" >> "$OUT.manifest"
		[ "$content" = win ] && rm -f "$OUT.manifest"
	fi
	info "ntfs 512 cluster=$csz content=$content"
	;;
exfat-*)
	need mkfs.exfat fsck.exfat
	rest=${V#exfat-}
	cl=${rest%%-*}
	content=${rest#*-}
	size=96M
	case $cl in
	4k) csz=4K ;;
	128k) csz=128K; size=512M ;;
	32m) csz=32M; size=4G ;;
	*) echo "mkimg: unknown cluster size in $V" >&2; exit 2 ;;
	esac
	flags=""
	tweak=""
	case $content in
	empty) ;;
	basic)
		# 32 MB clusters: one per file, so a small tree
		if [ "$cl" = 32m ]; then stage tiny --no-symlinks; else stage basic --no-symlinks --no-hardlinks; fi ;;
	many) stage many --count 2000; [ "$cl" = 32m ] && { echo "mkimg: $V would need 64 GB" >&2; exit 2; } ;;
	deep) stage deep ;;
	big) stage big --big 80; [ "$cl" = 32m ] || size=200M ;;
	frag) stage basic --no-symlinks --no-hardlinks; flags=--fragment ;;
	dirty) stage basic --no-symlinks --no-hardlinks; tweak=dirty ;;
	badboot) stage basic --no-symlinks --no-hardlinks; tweak=bad-boot-checksum ;;
	*) echo "mkimg: unknown variant $V" >&2; exit 2 ;;
	esac
	truncate -s "$size" "$OUT"
	mkfs.exfat -c "$csz" -L LIKEOSTEST "$OUT" >/dev/null 2>&1 || exit 1
	if [ -d "$WORK/src" ]; then
		python3 "$HERE/exfat-populate.py" "$OUT" "$WORK/src" $flags || exit 1
		$MKTREE manifest "$WORK/src" --no-symlinks > "$OUT.manifest"
	fi
	if ! fsck.exfat -n "$OUT" >/dev/null 2>&1; then
		echo "mkimg: fsck.exfat rejects the generated $V" >&2
		exit 1
	fi
	[ -n "$tweak" ] && { python3 "$HERE/fsparse.py" exfat-tweak "$OUT" $tweak || exit 1; }
	info "exfat 512 cluster=$csz content=$content"
	;;
iso-*)
	need xorriso
	x="xorriso -as mkisofs -quiet -V LIKEOSTEST"
	case ${V#iso-} in
	rrj) stage basic; $x -R -J -joliet-long -o "$OUT" "$WORK/src" ;;
	rr) stage basic; $x -R -o "$OUT" "$WORK/src" ;;
	# xorriso always adds Rock Ridge: Joliet-only and plain images come
	# from genisoimage
	joliet) need genisoimage; stage basic --no-symlinks
		genisoimage -quiet -V LIKEOSTEST -J -joliet-long -o "$OUT" "$WORK/src" 2>/dev/null ;;
	plain) need genisoimage; stage tiny --no-symlinks
		genisoimage -quiet -V LIKEOSTEST -o "$OUT" "$WORK/src" 2>/dev/null ;;
	deeprel) stage deep; $x -R -J -rr_reloc_dir RR_MOVED -o "$OUT" "$WORK/src" ;;
	deep) stage deep; $x -R -J -o "$OUT" "$WORK/src" ;;
	many) stage many --count 3000; $x -R -J -o "$OUT" "$WORK/src" ;;
	hybrid) stage basic; $x -R -J -partition_offset 16 -o "$OUT" "$WORK/src" ;;
	zisofs)
		need mkzftree
		stage basic
		mkzftree "$WORK/src" "$WORK/zsrc" || exit 1
		# keep the uncompressed tree for the manifest
		$x -R -J -z -o "$OUT" "$WORK/zsrc"
		;;
	big)
		need cp
		avail=$(df -k --output=avail "$(dirname "$OUT")" | tail -1)
		if [ "${avail:-0}" -lt 9500000 ]; then
			echo "mkimg: iso-big needs ~9 GB free next to $OUT" >&2
			exit 77
		fi
		stage tiny --no-symlinks
		truncate -s 4400M "$WORK/src/huge.bin"
		printf 'START-OF-HUGE' | dd of="$WORK/src/huge.bin" conv=notrunc status=none
		printf 'MIDDLE' | dd of="$WORK/src/huge.bin" bs=1 seek=$((4294967296 - 3)) conv=notrunc status=none
		printf 'END-OF-HUGE' | dd of="$WORK/src/huge.bin" bs=1 seek=$((4400 * 1048576 - 11)) conv=notrunc status=none
		$x -R -J -iso-level 3 -o - "$WORK/src" | cp --sparse=always /dev/stdin "$OUT" || exit 1
		;;
	*) echo "mkimg: unknown variant $V" >&2; exit 2 ;;
	esac
	[ -s "$OUT" ] || exit 1
	case ${V#iso-} in
	rrj|rr|deep|deeprel|many|hybrid|zisofs|big) $MKTREE manifest "$WORK/src" > "$OUT.manifest" ;;
	esac
	info "iso 2048 ${V#iso-}"
	;;
*)
	echo "mkimg: unknown variant $V (see --list)" >&2
	exit 2
	;;
esac
exit 0
