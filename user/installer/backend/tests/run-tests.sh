#!/bin/sh
# libinstaller host tests: the unit tests, then whole installations in
# simulate mode on sparse image files, checked with the host's tools.
#
#   run-tests.sh BINDIR RES_ETC [WORKDIR]
#
# BINDIR holds test_backend and test_install (Makefile.host builds them),
# RES_ETC is res/etc of the source tree (the image's account files).
# Never run this as root; it touches nothing outside WORKDIR.
#
# Copyright (C) 2026 The LikeOS Project

set -u

bindir=$(cd "$1" && pwd)
resetc=$(cd "$2" && pwd)
work=${3:-${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/il-backend-test.XXXXXX")}}
pass=0
fail=0

ok() { pass=$((pass + 1)); echo "PASS $*"; }
bad() { fail=$((fail + 1)); echo "FAIL $*"; }
check() { # check DESCRIPTION COMMAND...
	d=$1
	shift
	if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi
}

if [ "$(id -u)" = 0 ]; then
	echo "run-tests.sh: refusing to run as root" >&2
	exit 2
fi
for t in sgdisk sfdisk mkfs.fat fsck.fat mke2fs e2fsck dumpe2fs tune2fs \
	 fakeroot tar zstd sha256sum iconv cmp truncate; do
	command -v $t >/dev/null 2>&1 || { echo "run-tests.sh: $t is missing" >&2; exit 2; }
done
mkdir -p "$work"
work=$(cd "$work" && pwd)
echo "work directory: $work"

# The library takes its tools from these (the LikeOS paths are /sbin/...).
IL_MKFS_FAT=$(command -v mkfs.fat)
IL_MKE2FS=$(command -v mke2fs)
IL_E2FSCK=$(command -v e2fsck)
IL_TUNE2FS=$(command -v tune2fs)
IL_EFIBOOT=/nonexistent/likeos-efiboot
export IL_MKFS_FAT IL_MKE2FS IL_E2FSCK IL_TUNE2FS IL_EFIBOOT

# ---- unit tests ----------------------------------------------------------
mkdir -p "$work/unit"
if "$bindir/test_backend" "$work/unit" "$resetc" > "$work/unit.out" 2>&1; then
	ok "unit tests: $(tail -n 1 "$work/unit.out")"
else
	bad "unit tests:"
	grep -E "^(FAIL|SKIP)|passed" "$work/unit.out"
fi
grep "^SKIP" "$work/unit.out" || true
libc_time=$(cd "$resetc/../../user/lib/libc/src/time" 2>/dev/null && pwd)/time.c
if [ -f "$libc_time" ]; then
	awk '/^} g_tz_table\[\] = \{/{f=1;next} f&&/^};/{exit} f' "$libc_time" |
		sed -n 's/^[[:space:]]*{ "\([^"]*\)".*/\1/p' | sort > "$work/zones.libc"
	sort "$work/unit/zones.txt" > "$work/zones.il"
	if cmp -s "$work/zones.libc" "$work/zones.il"; then
		ok "time zones: exactly the C library's $(wc -l < "$work/zones.il")"
	else
		bad "time zones differ from the C library's table"
		diff "$work/zones.libc" "$work/zones.il" | head
	fi
fi

# ---- a system image --------------------------------------------------------
src=$work/src
tree=$work/tree
mkdir -p "$src"
cat > "$work/mktree.sh" <<'EOF'
set -e
tree=$1; resetc=$2; out=$3
mkdir -p "$tree"
cd "$tree"
mkdir -p etc/skel/.config etc/likeos usr/bin usr/lib/likeos/efi \
	usr/share/zoneinfo/Europe var/lib/dbus var/log boot/efi home root tmp \
	install dev "usr/share/doc/caf\303\251"
cp "$resetc/passwd" "$resetc/group" "$resetc/shadow" "$resetc/hosts" etc/
chmod 0644 etc/passwd etc/group etc/hosts
chmod 0600 etc/shadow
printf 'export PS1=x\n' > etc/skel/.profile
printf 'set -o vi\n' > etc/skel/.config/inputrc
ln -s .profile etc/skel/.bash_profile
printf '# placeholder written by the release build\nUUID=@ROOT@ / ext4 defaults 0 1\n' > etc/fstab
printf 'live installer session\n' > etc/inittab.install
printf 'LAYOUT=us\nVARIANT=\nOPTIONS=\n' > etc/likeos/keyboard.conf
head -c 300000 /dev/urandom > usr/lib/likeos/efi/BOOTX64.EFI
printf '#!/bin/sh\necho hello\n' > usr/bin/hello
chmod 4755 usr/bin/hello
ln usr/bin/hello usr/bin/hello-link
ln -s usr/bin bin
printf 'TZif-ish\n' > usr/share/zoneinfo/Europe/Berlin
printf 'stale\n' > var/lib/dbus/machine-id
printf 'archive\n' > install/likeos-rootfs.tar.zst
printf 'docs\n' > "usr/share/doc/caf\303\251/README"
mknod dev/console c 5 1
mknod dev/null c 1 3
chmod 0700 root boot/efi
chmod 1777 tmp
chown -R 0:0 .
chown 0:29 usr/bin/hello
tar --format=posix --sort=name --numeric-owner --xattrs \
	--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
	-cf - . | zstd -19 -T0 -q > "$out/likeos-rootfs.tar.zst"
bytes=$(du -sb --apparent-size . | cut -f1)
files=$(find . | wc -l)
printf 'version=0.0-test\nbuild=2026-11-13\nbytes=%s\nfiles=%s\n' "$bytes" "$files" \
	> "$out/likeos-rootfs.tar.zst.info"
(cd "$out" && sha256sum likeos-rootfs.tar.zst > likeos-rootfs.tar.zst.sha256)
EOF
if fakeroot -- sh "$work/mktree.sh" "$tree" "$resetc" "$src" > "$work/mktree.out" 2>&1; then
	ok "test system image made ($(stat -c %s "$src/likeos-rootfs.tar.zst") bytes)"
else
	bad "test system image"
	cat "$work/mktree.out"
fi
archive=$src/likeos-rootfs.tar.zst

# ---- dry run and cancel: nothing may be written ------------------------------
for mode in dryrun cancel; do
	img=$work/$mode.img
	truncate -s 3G "$img"
	if "$bindir/test_install" $mode "$img" "$archive" "$work/$mode-target" \
		"$work/$mode.log" > "$work/$mode.out" 2>&1; then
		ok "$mode: $(grep '^RESULT' "$work/$mode.out")"
	else
		bad "$mode"
		cat "$work/$mode.out"
	fi
	check "$mode: the disk image is untouched" \
		cmp -n 4194304 "$img" /dev/zero
	check "$mode: no partition file systems were made" \
		test ! -e "$img.p1.fs" -a ! -e "$img.p2.fs"
	rm -f "$img"
done

# ---- a whole installation (in one fakeroot session with its checks) ---------
img=$work/disk.img
target=$work/target
truncate -s 3G "$img"
cat > "$work/install.sh" <<'EOF'
set -u
bindir=$1; img=$2; archive=$3; target=$4; work=$5
"$bindir/test_install" install "$img" "$archive" "$target" "$work/install.log" \
	> "$work/install.out" 2>&1
echo "install exit $?"
# owners and modes only fakeroot remembers
stat -c '%u:%g %a %n' "$target/home/ada" "$target/usr/bin/hello" \
	"$target/etc/shadow" "$target/etc/passwd" "$target/dev/console" \
	"$target/var/log/likeos-install.log" "$target/root" "$target/tmp" \
	> "$work/owners.out" 2>&1
stat -c '%t:%T' "$target/dev/console" >> "$work/owners.out" 2>&1
# everything the configuration leaves alone must equal the archive
zstd -dc "$archive" > "$work/archive.tar"
(cd "$target" && tar --compare --numeric-owner -f "$work/archive.tar" \
	--exclude=./etc/passwd --exclude=./etc/shadow --exclude=./etc/group \
	--exclude=./etc/hosts --exclude=./etc/fstab \
	--exclude=./etc/likeos/keyboard.conf --exclude=./etc/inittab.install \
	--exclude=./install --exclude=./var/lib/dbus/machine-id \
	> "$work/compare.out" 2>&1)
echo "compare exit $?"
EOF
fakeroot -- sh "$work/install.sh" "$bindir" "$img" "$archive" "$target" "$work" \
	> "$work/install-session.out" 2>&1
if grep -q "install exit 0" "$work/install-session.out"; then
	ok "install: $(grep '^REPORTS' "$work/install.out")"
else
	bad "install"
	cat "$work/install.out"
	tail -n 30 "$work/install.log"
fi
# Directories the installation added files to differ in their times only.
if grep -q "compare exit 0" "$work/install-session.out" ||
   ! grep -v -E "^\./(etc|var/lib/dbus|var/log|home|boot/efi|usr/share/zoneinfo/Europe)?: Mod time differs$|^\.: Mod time differs$|^tar: " \
	"$work/compare.out" | grep -q .; then
	ok "the unpacked files equal the archive (tar --compare)"
else
	bad "tar --compare:"
	head -n 20 "$work/compare.out"
fi
if sgdisk -v "$img" 2>&1 | grep -q "No problems found"; then
	ok "sgdisk -v: no problems"
else
	bad "sgdisk -v"
	sgdisk -v "$img"
fi
check "sfdisk --verify" sfdisk --verify "$img"
i1=$(sgdisk -i 1 "$img" 2>/dev/null)
i2=$(sgdisk -i 2 "$img" 2>/dev/null)
echo "$i1" | grep -q "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" &&
	echo "$i1" | grep -q "First sector: 2048 " &&
	echo "$i1" | grep -q "Partition size: 1048576 sectors" &&
	echo "$i1" | grep -q "Partition name: 'EFI system partition'" &&
	ok "partition 1: ESP, 1 MiB, 512 MiB" || { bad "partition 1"; echo "$i1"; }
echo "$i2" | grep -q "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709" &&
	echo "$i2" | grep -q "First sector: 1050624 " &&
	echo "$i2" | grep -q "Partition name: 'likeos-root'" &&
	ok "partition 2: LikeOS root after the ESP" || { bad "partition 2"; echo "$i2"; }
partuuid=$(echo "$i2" | sed -n 's/^Partition unique GUID: //p' | tr 'A-F' 'a-f')
efi=$target/boot/efi/EFI
check "cmdline.txt: root=PARTUUID of partition 2" \
	grep -qx "root=PARTUUID=$partuuid rootfstype=ext4" "$efi/LIKEOS/cmdline.txt"
check "cmdline.txt: comment header" grep -q "^# LikeOS kernel command line" \
	"$efi/LIKEOS/cmdline.txt"
check "BOOTX64.EFI in EFI/LIKEOS" cmp "$tree/usr/lib/likeos/efi/BOOTX64.EFI" \
	"$efi/LIKEOS/BOOTX64.EFI"
check "BOOTX64.EFI in EFI/BOOT" cmp "$tree/usr/lib/likeos/efi/BOOTX64.EFI" \
	"$efi/BOOT/BOOTX64.EFI"
if [ "$(head -c 2 "$efi/LIKEOS/BOOTX64.CSV" | od -An -tx1 | tr -d ' ')" = fffe ] &&
   [ "$(iconv -f UTF-16 -t UTF-8 "$efi/LIKEOS/BOOTX64.CSV")" = "$(printf 'BOOTX64.EFI,LikeOS,,This is the boot entry for LikeOS\r')" ]; then
	ok "BOOTX64.CSV: UTF-16LE with a byte order mark"
else
	bad "BOOTX64.CSV"
	od -c "$efi/LIKEOS/BOOTX64.CSV" | head
fi
check "no bootmenu.conf on the ESP" test ! -e "$efi/LIKEOS/bootmenu.conf"
check "ESP file system clean (fsck.fat)" fsck.fat -n "$img.p1.fs"
check "root file system clean (e2fsck)" e2fsck -fn "$img.p2.fs"
feat=$(dumpe2fs -h "$img.p2.fs" 2>/dev/null)
echo "$feat" | grep -q "^Filesystem volume name:[[:space:]]*likeos-root$" &&
	! echo "$feat" | grep "^Filesystem features:" | grep -qE "64bit|meta_bg|orphan_file|metadata_csum_seed" &&
	echo "$feat" | grep -q "^Block size:[[:space:]]*4096$" &&
	echo "$feat" | grep -q "^Inode size:[[:space:]]*256$" &&
	ok "root file system: label, no 64bit/meta_bg, 4 KiB blocks, 256-byte inodes" ||
	{ bad "root file system features"; echo "$feat" | head -n 12; }
rootuuid=$(echo "$feat" | sed -n "s/^Filesystem UUID:[[:space:]]*//p")
espid=$(dd if="$img.p1.fs" bs=1 skip=67 count=4 2>/dev/null | od -An -tx1 |
	awk '{printf "%s%s-%s%s", toupper($4), toupper($3), toupper($2), toupper($1)}')
check "fstab: the root by its UUID" grep -q "^UUID=$rootuuid  /  " "$target/etc/fstab"
check "fstab: the ESP by its volume id" grep -q "^UUID=$espid  /boot/efi  vfat  umask=0077,shortname=mixed  0  2$" "$target/etc/fstab"
check "the file systems were copied into the disk image" \
	cmp -n 1048576 -i 0:1048576 "$img.p1.fs" "$img"
check "hostname" grep -qx simbox "$target/etc/hostname"
check "keyboard.conf" grep -qx "LAYOUT=de,us" "$target/etc/likeos/keyboard.conf"
check "keyboard variants" grep -qx "VARIANT=nodeadkeys," "$target/etc/likeos/keyboard.conf"
check "time zone" grep -qx "Europe/Berlin" "$target/etc/timezone"
check "locale" grep -qx "LC_TIME=en_GB.UTF-8" "$target/etc/locale.conf"
check "account" grep -q "^ada:x:1000:1000:Ada Lovelace:/home/ada:/bin/bash$" "$target/etc/passwd"
check "live leftovers removed" test ! -e "$target/install" -a ! -e "$target/etc/inittab.install"
check "the log was copied into the new system" test -s "$target/var/log/likeos-install.log"
check "log lines carry the prefix" grep -q "likeos-install\|step 5/8" "$work/install.log"
if grep -q "^1000:1000 700 .*home/ada$" "$work/owners.out" &&
   grep -q "^0:29 4755 .*usr/bin/hello$" "$work/owners.out" &&
   grep -q "^0:0 600 .*etc/shadow$" "$work/owners.out" &&
   grep -q "^0:0 644 .*etc/passwd$" "$work/owners.out" &&
   grep -q "^0:0 600 .*likeos-install.log$" "$work/owners.out" &&
   grep -q "^0:0 1777 .*/tmp$" "$work/owners.out" &&
   grep -qx "5:1" "$work/owners.out"; then
	ok "owners and modes (setuid kept, device node, home 0700)"
else
	bad "owners and modes"
	cat "$work/owners.out"
fi

# ---- the same from an answer file ------------------------------------------
img2=$work/answers.img
truncate -s 3G "$img2"
cat > "$work/answers.conf" <<'EOF'
# test answers
disk=auto
whole_disk=yes
reboot=no
locale=en_US.UTF-8
timezone=America/New_York
keyboard=us
hostname=answered
root_password="a root password # with a hash"
user_fullname="Grace Hopper"
user_name=grace
user_password=cobol
EOF
chmod 600 "$work/answers.conf"
if fakeroot -- "$bindir/test_install" answers "$img2" "$archive" \
	"$work/target-answers" "$work/answers.log" "$work/answers.conf" \
	> "$work/answers.out" 2>&1; then
	ok "install from an answer file"
else
	bad "install from an answer file"
	cat "$work/answers.out"
fi
check "answers: hostname" grep -qx answered "$work/target-answers/etc/hostname"
check "answers: account" grep -q "^grace:x:1000:" "$work/target-answers/etc/passwd"
check "answers: sgdisk -v" sh -c "sgdisk -v '$img2' | grep -q 'No problems found'"

# ---- a damaged image must fail cleanly -----------------------------------------
bad_src=$work/bad
mkdir -p "$bad_src"
cp "$archive" "$archive.info" "$bad_src/"
echo "0000000000000000000000000000000000000000000000000000000000000000  likeos-rootfs.tar.zst" \
	> "$bad_src/likeos-rootfs.tar.zst.sha256"
img3=$work/bad.img
truncate -s 3G "$img3"
if fakeroot -- "$bindir/test_install" fail "$img3" "$bad_src/likeos-rootfs.tar.zst" \
	"$work/target-bad" "$work/bad.log" > "$work/bad.out" 2>&1; then
	ok "a wrong checksum fails the installation: $(grep '^ERROR' "$work/bad.out" | cut -c1-120)"
else
	bad "a wrong checksum must fail the installation"
	cat "$work/bad.out"
fi
check "the failure is in the log" grep -q "the installation failed in step" "$work/bad.log"

echo "run-tests.sh: $pass passed, $fail failed"
if [ $fail -eq 0 ] && [ -z "${KEEP_WORK:-}" ]; then
	chmod -R u+rwx "$work" 2>/dev/null
	rm -rf "$work"
fi
[ $fail -eq 0 ]
