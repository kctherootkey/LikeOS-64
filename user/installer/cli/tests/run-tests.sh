#!/bin/sh
# likeos-install host tests: whole installations in simulate mode on sparse
# image files, driven the two ways the program is used -- from an answer
# file (unattended) and through its questions (a script typing the
# answers) -- and checked with the host's tools.
#
#   run-tests.sh BINDIR RES [WORKDIR]
#
# BINDIR holds likeos-install (Makefile.host builds it), RES is res/ of the
# source tree (the image's account files, the keyboard catalogue).  Never
# run this as root; it touches nothing outside WORKDIR.
#
# Copyright (C) 2026 The LikeOS Project

set -u

bindir=$(cd "$1" && pwd)
res=$(cd "$2" && pwd)
work=${3:-${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/likeos-install-test.XXXXXX")}}
prog=$bindir/likeos-install
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
for t in sgdisk mkfs.fat fsck.fat mke2fs e2fsck dumpe2fs tune2fs fakeroot \
	 tar zstd sha256sum cmp truncate; do
	command -v $t >/dev/null 2>&1 || { echo "run-tests.sh: $t is missing" >&2; exit 2; }
done
mkdir -p "$work"
work=$(cd "$work" && pwd)
echo "work directory: $work"

IL_MKFS_FAT=$(command -v mkfs.fat)
IL_MKE2FS=$(command -v mke2fs)
IL_E2FSCK=$(command -v e2fsck)
IL_TUNE2FS=$(command -v tune2fs)
IL_EFIBOOT=/nonexistent/likeos-efiboot
LIKEOS_KEYBOARD_CATALOGUE=$res/keyboard/layouts.tsv
# no colours in the transcripts
NO_COLOR=1
TERM=dumb
export IL_MKFS_FAT IL_MKE2FS IL_E2FSCK IL_TUNE2FS IL_EFIBOOT \
	LIKEOS_KEYBOARD_CATALOGUE NO_COLOR TERM

# ---- a system image ---------------------------------------------------------
src=$work/src
tree=$work/tree
mkdir -p "$src"
cat > "$work/mktree.sh" <<'EOF'
set -e
tree=$1; resetc=$2; out=$3
mkdir -p "$tree"
cd "$tree"
mkdir -p etc/skel etc/likeos usr/bin usr/lib/likeos/efi usr/sbin \
	usr/share/zoneinfo/Europe var/lib/dbus var/log boot/efi home root tmp
cp "$resetc/passwd" "$resetc/group" "$resetc/shadow" "$resetc/hosts" etc/
cp "$resetc/fstab.rootfs" etc/fstab
chmod 0644 etc/passwd etc/group etc/hosts etc/fstab
chmod 0600 etc/shadow
printf 'export PS1=x\n' > etc/skel/.profile
printf 'LAYOUT=us\nVARIANT=\nOPTIONS=\n' > etc/likeos/keyboard.conf
head -c 200000 /dev/urandom > usr/lib/likeos/efi/BOOTX64.EFI
printf '#!/bin/sh\necho hello\n' > usr/bin/hello
chmod 0755 usr/bin/hello
printf 'TZif-ish\n' > usr/share/zoneinfo/Europe/Berlin
chmod 0700 root boot/efi
chmod 1777 tmp
chown -R 0:0 .
tar --format=posix --sort=name --numeric-owner \
	--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
	-cf - . | zstd -19 -T0 -q > "$out/likeos-rootfs.tar.zst"
bytes=$(du -sb --apparent-size . | cut -f1)
files=$(find . | wc -l)
printf 'version=0.0-clitest\nbuild=2026-11-13\nbytes=%s\nfiles=%s\n' "$bytes" "$files" \
	> "$out/likeos-rootfs.tar.zst.info"
(cd "$out" && sha256sum likeos-rootfs.tar.zst > likeos-rootfs.tar.zst.sha256)
EOF
if fakeroot -- sh "$work/mktree.sh" "$tree" "$res/etc" "$src" > "$work/mktree.out" 2>&1; then
	ok "test system image made"
else
	bad "test system image"
	cat "$work/mktree.out"
fi
archive=$src/likeos-rootfs.tar.zst

# The checks of an installed disk image: $1 image, $2 target dir, $3 name,
# $4 user name expected ("" for none), $5 keyboard layouts, $6 zone,
# $7 time format.
check_install() {
	img=$1; target=$2; name=$3; user=$4; layouts=$5; zone=$6; tf=$7
	if sgdisk -v "$img" 2>&1 | grep -q "No problems found"; then
		ok "$name: sgdisk -v finds no problems"
	else
		bad "$name: sgdisk -v"
		sgdisk -v "$img" | head
	fi
	i1=$(sgdisk -i 1 "$img" 2>/dev/null)
	i2=$(sgdisk -i 2 "$img" 2>/dev/null)
	echo "$i1" | grep -q "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" &&
		ok "$name: partition 1 is the ESP" || bad "$name: partition 1"
	echo "$i2" | grep -q "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709" &&
		ok "$name: partition 2 is the LikeOS root" || bad "$name: partition 2"
	check "$name: ESP clean (fsck.fat)" fsck.fat -n "$img.p1.fs"
	check "$name: root clean (e2fsck)" e2fsck -fn "$img.p2.fs"
	rootuuid=$(dumpe2fs -h "$img.p2.fs" 2>/dev/null | sed -n "s/^Filesystem UUID:[[:space:]]*//p")
	check "$name: fstab mounts / by the root's UUID" \
		grep -q "^UUID=$rootuuid  /  " "$target/etc/fstab"
	check "$name: fstab mounts /boot/efi (vfat)" \
		grep -q "^UUID=[0-9A-F-]*  /boot/efi  vfat  " "$target/etc/fstab"
	# Every line of the template is a comment: nothing of it is kept.
	check "$name: fstab has no template lines" \
		sh -c "! grep -q 'Kept from the system image' '$target/etc/fstab'"
	check "$name: hostname" grep -qx simbox "$target/etc/hostname"
	check "$name: /etc/hosts names the computer" grep -q "simbox" "$target/etc/hosts"
	check "$name: keyboard.conf" grep -qx "LAYOUT=$layouts" "$target/etc/likeos/keyboard.conf"
	check "$name: time zone" grep -qx "$zone" "$target/etc/timezone"
	check "$name: locale" grep -qx "LC_TIME=$tf" "$target/etc/locale.conf"
	check "$name: root password set" sh -c "grep '^root:' '$target/etc/shadow' | grep -q '^root:\\\$y\\\$'"
	if [ -n "$user" ]; then
		check "$name: account $user" grep -q "^$user:x:1000:1000:" "$target/etc/passwd"
		check "$name: home of $user" test -d "$target/home/$user"
	fi
	check "$name: boot loader on the ESP" test -s "$target/boot/efi/EFI/BOOT/BOOTX64.EFI"
	check "$name: log copied into the system" test -s "$target/var/log/likeos-install.log"
}

# ---- --help, --version, usage errors ----------------------------------------------
check "--help" sh -c "'$prog' --help | grep -q 'answers FILE'"
check "--version" sh -c "'$prog' --version | grep -q '^likeos-install '"
"$prog" --bogus >/dev/null 2>&1
[ $? -eq 1 ] && ok "unknown option: exit 1" || bad "unknown option exit status"
"$prog" --simulate "$work/none.img" </dev/null >/dev/null 2>&1
[ $? -eq 4 ] && ok "no terminal and no answers: exit 4" || bad "no terminal: exit status"

# ---- --list-disks ------------------------------------------------------------------
img=$work/list.img
truncate -s 3G "$img"
"$prog" --simulate "$img" --list-disks --log "$work/list.log" > "$work/list.out" 2>&1
if [ $? -eq 0 ] && grep -q "list.img" "$work/list.out" && grep -q "3.2 GB" "$work/list.out"; then
	ok "--list-disks shows the image (3.2 GB)"
else
	bad "--list-disks"
	cat "$work/list.out"
fi
rm -f "$img"

# ---- unattended: an answer file --------------------------------------------------------
cat > "$work/answers.conf" <<'EOF'
# likeos-install test answers
disk=auto
whole_disk=yes
reboot=no
locale=en_US.UTF-8
time_format=en_GB.UTF-8
timezone=Europe/Berlin
keyboard=de,us
keyboard_variant=nodeadkeys,
hostname=simbox
root_password="Sim root #1"
user_fullname="Ada Lovelace"
user_name=ada
user_password=ada-sim-pw-42
EOF
img=$work/answers.img
target=$work/answers-target
truncate -s 3G "$img"
# Without --yes and without a terminal it must refuse and write nothing.
"$prog" --simulate "$img" --answers "$work/answers.conf" --source "$archive" \
	--target "$target" --log "$work/answers-refuse.log" </dev/null \
	> "$work/answers-refuse.out" 2>&1
rc=$?
if [ $rc -eq 1 ] && grep -q -- "--yes" "$work/answers-refuse.out" &&
   cmp -s -n 4194304 "$img" /dev/zero; then
	ok "answers without --yes on no terminal: refused, disk untouched"
else
	bad "answers without --yes (exit $rc)"
	cat "$work/answers-refuse.out"
fi
# A dry run writes nothing and saves the answers.
"$prog" --simulate "$img" --answers "$work/answers.conf" --source "$archive" \
	--target "$target" --log "$work/answers-dry.log" --dry-run \
	--save-answers "$work/saved.conf" </dev/null > "$work/answers-dry.out" 2>&1
rc=$?
if [ $rc -eq 0 ] && grep -q "Dry run finished" "$work/answers-dry.out" &&
   cmp -s -n 4194304 "$img" /dev/zero; then
	ok "answers --dry-run: valid, disk untouched"
else
	bad "answers --dry-run (exit $rc)"
	cat "$work/answers-dry.out"
fi
check "--save-answers: hashes, not passwords" sh -c \
	"grep -q '^root_password_hash=' '$work/saved.conf' && ! grep -q 'Sim root' '$work/saved.conf' && test \$(stat -c %a '$work/saved.conf') = 600"
check "--save-answers: the settings" sh -c \
	"grep -qx 'keyboard=de,us' '$work/saved.conf' && grep -qx 'timezone=Europe/Berlin' '$work/saved.conf' && grep -qx 'hostname=simbox' '$work/saved.conf'"
# The real (simulated) installation, in one fakeroot session with its checks.
cat > "$work/answers-install.sh" <<EOF
"$prog" --simulate "$img" --answers "$work/answers.conf" --source "$archive" \
	--target "$target" --log "$work/answers.log" --yes </dev/null \
	> "$work/answers.out" 2>&1
echo "install exit \$?"
stat -c '%u:%g %a %n' "$target/home/ada" "$target/etc/shadow" > "$work/answers-owners.out" 2>&1
EOF
fakeroot -- sh "$work/answers-install.sh" > "$work/answers-session.out" 2>&1
if grep -q "install exit 0" "$work/answers-session.out" &&
   grep -q "^LikeOS is installed on img0\.$" "$work/answers.out"; then
	ok "answers: installed"
else
	bad "answers: install"
	cat "$work/answers.out"
	tail -n 30 "$work/answers.log"
fi
check "answers: progress lines in the transcript" grep -q '^\[100%\]' "$work/answers.out"
check "answers: every step reported done" sh -c "test \$(grep -c '^\[done\]' '$work/answers.out') -eq 8"
check "answers: home 0700 owned by ada" grep -q "^1000:1000 700 " "$work/answers-owners.out"
check_install "$img" "$target" "answers" ada "de,us" Europe/Berlin en_GB.UTF-8
rm -f "$img" "$img".p*.fs

# ---- interactive: the questions answered by a script ------------------------------------
# Enter, language 1, format 2, keyboard: the exact id "de" (listed first),
# then "us" added, disk 1, the whole disk, Berlin, the name, the root
# password twice, an account (the suggested user name), its password
# twice, YES.
img=$work/wizard.img
target=$work/wizard-target
truncate -s 3G "$img"
cat > "$work/wizard.in" <<'EOF'

1
2
de
1
y
us
1
n
1
1
berlin
simbox
Wizard-root-pw-1
Wizard-root-pw-1
y
Grace Hopper

Wizard-user-pw-2
Wizard-user-pw-2
YES
EOF
cat > "$work/wizard-install.sh" <<EOF
LIKEOS_INSTALL_PIPE=1 "$prog" --simulate "$img" --source "$archive" \
	--target "$target" --log "$work/wizard.log" \
	--save-answers "$work/wizard-saved.conf" \
	< "$work/wizard.in" > "$work/wizard.out" 2>&1
echo "install exit \$?"
EOF
fakeroot -- sh "$work/wizard-install.sh" > "$work/wizard-session.out" 2>&1
if grep -q "install exit 0" "$work/wizard-session.out" &&
   grep -q "^LikeOS is installed on img0\.$" "$work/wizard.out"; then
	ok "interactive: installed"
else
	bad "interactive: install"
	tail -n 60 "$work/wizard.out"
	tail -n 20 "$work/wizard.log"
fi
check "interactive: the summary was shown" grep -q "Ready to install" "$work/wizard.out"
check "interactive: the user name was suggested" grep -q "User name (to log in with) \[grace\]" "$work/wizard.out"
check "interactive: passwords never echoed" sh -c "! grep -q 'Wizard-root-pw-1\|Wizard-user-pw-2' '$work/wizard.out'"
check "interactive: answers saved" grep -qx "user_name=grace" "$work/wizard-saved.conf"
check_install "$img" "$target" "interactive" grace "de,us" Europe/Berlin en_GB.UTF-8
rm -f "$img" "$img".p*.fs

# ---- interactive: the input ends before the confirmation ---------------------------------
img=$work/cancel.img
truncate -s 3G "$img"
head -n 12 "$work/wizard.in" > "$work/cancel.in"
LIKEOS_INSTALL_PIPE=1 "$prog" --simulate "$img" --source "$archive" \
	--target "$work/cancel-target" --log "$work/cancel.log" \
	< "$work/cancel.in" > "$work/cancel.out" 2>&1
rc=$?
if [ $rc -eq 3 ] && grep -q "Nothing was written" "$work/cancel.out" &&
   cmp -s -n 4194304 "$img" /dev/zero && [ ! -e "$img.p1.fs" ]; then
	ok "interactive: cancelled at the end of the input, exit 3, disk untouched"
else
	bad "interactive cancel (exit $rc)"
	tail -n 20 "$work/cancel.out"
fi
rm -f "$img"

# ---- interactive: wrong answers are asked again ----------------------------------------
img=$work/retry.img
truncate -s 3G "$img"
cat > "$work/retry.in" <<'EOF'

9
1
1
nosuchlayout

n
7

1
1
nowhere
utc
1
-bad-name-
simbox
x
x
n
short-pw-A1
short-pw-B2
Retry-root-pw-3
Retry-root-pw-3
n
EOF
LIKEOS_INSTALL_PIPE=1 "$prog" --simulate "$img" --source "$archive" \
	--target "$work/retry-target" --log "$work/retry.log" --dry-run \
	< "$work/retry.in" > "$work/retry.out" 2>&1
rc=$?
if [ $rc -eq 0 ] && grep -q "Dry run finished" "$work/retry.out"; then
	ok "interactive dry run with wrong answers in between"
else
	bad "interactive retries (exit $rc)"
	tail -n 40 "$work/retry.out"
fi
check "retry: a number out of range is refused" grep -q "Please type a number from 1 to" "$work/retry.out"
check "retry: an unknown layout is said so" grep -q 'No layout matches "nosuchlayout"' "$work/retry.out"
check "retry: an unknown disk number is refused" grep -q "Please type a disk number" "$work/retry.out"
check "retry: an unknown zone is said so" grep -q 'No time zone matches "nowhere"' "$work/retry.out"
check "retry: a bad host name is refused" grep -q "^Error: " "$work/retry.out"
check "retry: differing passwords are refused" grep -q "The two passwords differ" "$work/retry.out"
check "retry: a short password is warned about" grep -q "^Note: This password is short" "$work/retry.out"
check "retry: no account when declined" sh -c "grep -q 'Accounts:       root\$' '$work/retry.out'"
check "retry: dry run wrote nothing" cmp -s -n 4194304 "$img" /dev/zero
rm -f "$img"

# ---- interactive: partitions by hand (dry run) -------------------------------------------
# A disk with an EFI system partition that has no file system and a data
# partition: format the ESP, delete the data partition, make a 2 GB one in
# the free space and put LikeOS on it.
img=$work/manual.img
truncate -s 4G "$img"
sgdisk -n 1:0:+300M -t 1:ef00 -n 2:0:+1G -t 2:8300 "$img" >/dev/null 2>&1
cp "$img" "$work/manual.orig"
cat > "$work/manual.in" <<'EOF'

1
1

n
1
2
format 1
delete 2
new F1 2G
y
done

simbox
Manual-root-pw-7
Manual-root-pw-7
n
EOF
LIKEOS_INSTALL_PIPE=1 "$prog" --simulate "$img" --source "$archive" \
	--target "$work/manual-target" --log "$work/manual.log" --dry-run \
	< "$work/manual.in" > "$work/manual.out" 2>&1
rc=$?
if [ $rc -eq 0 ] && grep -q "Dry run finished" "$work/manual.out"; then
	ok "by hand: dry run of a hand-made layout"
else
	bad "by hand (exit $rc)"
	tail -n 40 "$work/manual.out"
fi
check "by hand: the ESP is formatted" grep -q "Format partition 1 as FAT32" "$work/manual.out"
check "by hand: the old partition is deleted" grep -q "Delete partition 2" "$work/manual.out"
check "by hand: the new root partition" grep -q "Create partition 2 (2.0 GB, LikeOS root)" "$work/manual.out"
check "by hand: dry run left the disk as it was" cmp -s "$img" "$work/manual.orig"
rm -f "$img" "$work/manual.orig"

# ---- the terminal ---------------------------------------------------------------------------
# On a pseudo-terminal (tty-test.py, the host's python3): --reset-tty, and
# a SIGTERM at the password prompt that leaves the echo back on.
if command -v python3 >/dev/null 2>&1; then
	tests=$(cd "$(dirname "$0")" && pwd)
	img=$work/tty.img
	truncate -s 3G "$img"
	head -n 13 "$work/wizard.in" > "$work/tty.in"
	for mode in reset sigterm; do
		if [ $mode = reset ]; then
			python3 -I "$tests/tty-test.py" reset "$prog" \
				> "$work/tty-$mode.out" 2>&1
		else
			python3 -I "$tests/tty-test.py" sigterm "$prog" \
				"$work/tty.in" --simulate "$img" --source "$archive" \
				--target "$work/tty-target" --log "$work/tty.log" \
				--dry-run > "$work/tty-$mode.out" 2>&1
		fi
		while read -r verdict what; do
			case $verdict in
			PASS) ok "tty: $what" ;;
			FAIL) bad "tty: $what" ;;
			esac
		done < "$work/tty-$mode.out"
		grep -q '^PASS ' "$work/tty-$mode.out" || {
			bad "tty: $mode test did not run"
			tail -n 20 "$work/tty-$mode.out"
		}
	done
	check "tty: the SIGTERM test wrote nothing" cmp -s -n 4194304 "$img" /dev/zero
	rm -f "$img"
else
	echo "SKIP tty: no python3 on the build host"
fi

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
