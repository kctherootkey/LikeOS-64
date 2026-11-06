#!/bin/sh
# Host test of the console keymap engine (kernel/dev/input/keymap.c) and of
# the kbd port (ports/kbd-2.10.0: loadkeys, dumpkeys) against it.
#
# Builds the patched kbd tree, the unchanged engine and host/test-keymap.c
# (which plays the kernel: KD* ioctls, the engine state between programs)
# for the host, then:
#   - checks the compiled-in keymap types what the console always typed
#     (US layout, xterm sequences of the cursor block and F1-F12, the Ctrl+D
#     and Ctrl+N hotkeys, Caps Lock as a Shift lock);
#   - loads generated keymaps (build/keymaps, `make keymaps') with the real
#     loadkeys and checks the UTF-8 of scancode sequences: de, fr, ru, jp-kana,
#     de-nodeadkeys, us;
#   - checks loadkeys' system-setting behaviour (helper calls, aliases,
#     --no-save, the hand-over when the keymap may not be loaded);
#   - checks that dumpkeys output loads back to the same tables.
#
# Run from the repository root.  Boxed: address space capped, everything in
# a temporary directory.
#
# Copyright (C) 2026 The LikeOS Project

set -eu
ulimit -v 2500000

ROOT=$(pwd)
KEYMAPS=$ROOT/build/keymaps
PORT=$ROOT/ports/kbd-2.10.0
TMP=$(mktemp -d "${TMPDIR:-/tmp}/likeos-keymap-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

if [ ! -f "$KEYMAPS/de.map" ]; then
	echo "test-keymap: $KEYMAPS has no keymaps; run 'make keymaps' first" >&2
	exit 1
fi

# ---- build ------------------------------------------------------------------
"$PORT/unpack.sh" "$TMP" >/dev/null
SRC=$TMP/src/src
mkdir -p "$TMP/inc/sys"
cp kernel/uapi/kd.h "$TMP/inc/sys/kd.h"
cp kernel/uapi/keyboard.h "$TMP/inc/sys/keyboard.h"

cat >"$TMP/helper" <<EOF
#!/bin/sh
echo "\$*" >>"$TMP/helper.log"
exit 0
EOF
chmod +x "$TMP/helper"
printf 'LAYOUT=us\nVARIANT=\nOPTIONS=grp:alt_shift_toggle\n' >"$TMP/keyboard.conf"

CFLAGS="-std=gnu11 -O1 -w -D_GNU_SOURCE -D__LIKEOS__ -I$TMP/inc -I$PORT \
	-I$SRC/include -I$SRC/libcommon -I$SRC/libkeymap -I$ROOT \
	-DLIKEOS_KEYBOARD_CATALOGUE=\"$ROOT/res/keyboard/layouts.tsv\" \
	-DLIKEOS_KEYBOARD_CONF=\"$TMP/keyboard.conf\" \
	-DLIKEOS_KEYBOARD_SAVE=\"$TMP/helper\" \
	-DLIKEOS_KEYMAP_DIR=\"$KEYMAPS\""
LIBS="$SRC/libkeymap/analyze.c $SRC/libkeymap/array.c $SRC/libkeymap/common.c \
	$SRC/libkeymap/diacr.c $SRC/libkeymap/dump.c $SRC/libkeymap/func.c \
	$SRC/libkeymap/kernel.c $SRC/libkeymap/kmap.c $SRC/libkeymap/ksyms.c \
	$SRC/libkeymap/loadkeys.c $SRC/libkeymap/modifiers.c \
	$SRC/libkeymap/parser.c $SRC/libkeymap/summary.c \
	$SRC/libkbdfile/init.c $SRC/libkbdfile/kbdfile.c \
	$SRC/libcommon/error.c $SRC/libcommon/getfd.c $SRC/libcommon/version.c"
ENGINE="kernel/dev/input/keymap.c host/test-keymap.c"
# shellcheck disable=SC2086
cc $CFLAGS -o "$TMP/loadkeys" $LIBS $SRC/loadkeys.c $SRC/likeos-keyboard.c $ENGINE
# shellcheck disable=SC2086
cc $CFLAGS -o "$TMP/dumpkeys" $LIBS $SRC/dumpkeys.c $ENGINE
cc -std=gnu11 -Wall -Wextra -Werror -O1 -I. -DKBDTEST_FEED_MAIN \
	-o "$TMP/feed" kernel/dev/input/keymap.c host/test-keymap.c

# ---- helpers ----------------------------------------------------------------
fail=0
pass=0

# feed STATE EXPECTED SCANCODE...  (STATE "-" = the compiled-in keymap)
feed() {
	state=$1
	want=$2
	shift 2
	if [ "$state" = - ]; then
		got=$(env -u KBDTEST_STATE "$TMP/feed" "$@")
	else
		got=$(KBDTEST_STATE="$state" "$TMP/feed" "$@")
	fi
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL [$state] $*: want '$want' got '$got'"
	fi
}

# load NAME [loadkeys args...]: load into a fresh engine, state -> $TMP/NAME.st
load() {
	name=$1
	shift
	if ! KBDTEST_STATE_OUT="$TMP/$name.st" LOADKEYS_KEYMAP_PATH="$KEYMAPS" \
		"$TMP/loadkeys" -C /dev/null "$@" >"$TMP/$name.out" 2>&1; then
		fail=$((fail + 1))
		echo "FAIL loadkeys $*:"
		cat "$TMP/$name.out"
	else
		pass=$((pass + 1))
	fi
}

expect_log() {
	what=$1
	want=$2
	got=$(cat "$TMP/helper.log" 2>/dev/null || true)
	rm -f "$TMP/helper.log"
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $what: helper called with '$got', want '$want'"
	fi
}

# ---- the compiled-in keymap: what the console always typed -------------------
D=-
feed $D 'a leds=0' 1e 9e
feed $D 'A leds=0' 2a 1e 9e aa
feed $D 'A leds=0' 36 1e 9e b6
feed $D '1@#$%^&*()-_=+ leds=0' 02 82 2a 03 83 aa 2a 04 84 aa 2a 05 85 aa 2a 06 86 aa 2a 07 87 aa 2a 08 88 aa 2a 09 89 aa 2a 0a 8a aa 2a 0b 8b aa 0c 8c 2a 0c 8c aa 0d 8d 2a 0d 8d aa
feed $D '`~[]{}\\|;:'"'"'",<>/? leds=0' 29 a9 2a 29 a9 aa 1a 9a 1b 9b 2a 1a 9a 1b 9b aa 2b ab 2a 2b ab aa 27 a7 2a 27 a7 aa 28 a8 2a 28 a8 aa 33 b3 2a 33 b3 34 b4 aa 35 b5 2a 35 b5 aa
feed $D 'A! leds=4' 3a ba 1e 9e 02 82
feed $D 'aa leds=0' 3a ba 3a ba 1e 9e 1e 9e
feed $D 'A leds=4' 3a 3a 3a ba 1e 9e
feed $D '\x01\x1a leds=0' 1d 1e 9e 2c ac 9d
feed $D '\x01 leds=0' 2a 1d 1e 9e 9d aa
feed $D '\x00 leds=0' 1d 39 b9 9d
feed $D '[ leds=0' 1d 1a 9a 9d
feed $D '\x01 leds=0' e0 1d 1e 9e e0 9d
feed $D '\ea\eA leds=0' 38 1e 9e 2a 1e 9e aa b8
feed $D '\e\x01\x00 leds=0' 38 1d 1e 9e 39 b9 9d b8
feed $D '\e\e\e\x08 leds=0' 38 01 81 0e 8e b8
feed $D '[TASKS] leds=0' 1d 20 a0 9d
feed $D '[TASKS] leds=0' 2a 1d 20 a0 9d aa
feed $D '\e\x04 leds=0' 38 1d 20 a0 9d b8
feed $D '[NET] leds=0' 1d 31 b1 9d
feed $D '\e\x08\x09\x0d leds=0' 01 81 0e 8e 0f 8f 1c 9c
feed $D '  leds=0' 39 b9
feed $D '\e[A\e[B\e[C\e[D leds=0' e0 48 e0 c8 e0 50 e0 d0 e0 4d e0 cd e0 4b e0 cb
feed $D '\e[1;2A\e[1;5C\e[1;6D leds=0' 2a e0 48 e0 c8 aa 1d e0 4d e0 cd 9d 1d 2a e0 4b e0 cb aa 9d
feed $D '\e[H\e[F\e[5~\e[6~\e[2~\e[3~ leds=0' e0 47 e0 c7 e0 4f e0 cf e0 49 e0 c9 e0 51 e0 d1 e0 52 e0 d2 e0 53 e0 d3
feed $D '\e[1;2H\e[1;5F\e[5;2~\e[6;5~\e[3;6~ leds=0' 2a e0 47 e0 c7 aa 1d e0 4f e0 cf 9d 2a e0 49 e0 c9 aa 1d e0 51 e0 d1 9d 1d 2a e0 53 e0 d3 aa 9d
feed $D '\e[A leds=0' e0 2a e0 48 e0 c8 e0 aa
feed $D '\eOP\eOQ\eOR\eOS\e[15~\e[17~\e[18~\e[19~\e[20~\e[21~\e[23~\e[24~ leds=0' 3b bb 3c bc 3d bd 3e be 3f bf 40 c0 41 c1 42 c2 43 c3 44 c4 57 d7 58 d8
feed $D '\eOP\eOP\eOP leds=0' 2a 3b bb aa 1d 3b bb 9d 38 3b bb b8
feed $D '\e[H\e[A\e[5~\e[D5\e[C\e[F\e[B\e[6~\e[2~\e[3~ leds=0' 47 c7 48 c8 49 c9 4b cb 4c cc 4d cd 4f cf 50 d0 51 d1 52 d2 53 d3
feed $D '*-+ leds=0' 37 b7 4a ca 4e ce
feed $D '\e[H\e[A leds=0' 2a 47 c7 aa 1d 48 c8 9d
feed $D '7894561230. leds=2' 45 c5 47 c7 48 c8 49 c9 4b cb 4c cc 4d cd 4f cf 50 d0 51 d1 52 d2 53 d3
feed $D '/\x0d leds=0' e0 35 e0 b5 e0 1c e0 9c
feed $D 'aaa leds=0' 1e 1e 1e 9e
feed $D ' leds=1' 46 c6
feed $D ' leds=0' e1 1d 45 e1 9d c5

# ---- generated keymaps through the real loadkeys --------------------------------
load us --no-save us
load de --no-save de
load fr --no-save fr
load ru --no-save ru
load dend --no-save de-nodeadkeys
load jpk --no-save jp-kana

U=$TMP/us.st
feed $U 'aA leds=0' 1e 9e 2a 1e 9e aa
feed $U '\e[A\e[1;2A leds=0' e0 48 e0 c8 2a e0 48 e0 c8 aa
feed $U '\eOP\e[15~\e[24~\e[H\e[F\e[5~ leds=0' 3b bb 3f bf 58 d8 e0 47 e0 c7 e0 4f e0 cf e0 49 e0 c9
feed $U '\e[1;2P\e[1;5P leds=0' 2a 3b bb aa 1d 3b bb 9d
feed $U '\x7f\x0d\x09 leds=0' 0e 8e 1c 9c 0f 8f
feed $U 'A1 leds=4' 3a ba 1e 9e 02 82
feed $U '\e leds=0' 1d 1a 9a 9d
feed $U '[TASKS][NET] leds=0' 1d 20 a0 31 b1 9d
feed $U '\ex leds=0' 38 2d ad b8

G=$TMP/de.st
feed $G 'üäö leds=0' 1a 9a 28 a8 27 a7
feed $G 'ÜÄÖ leds=0' 2a 1a 9a 28 a8 27 a7 aa
feed $G 'ÜÄÖ leds=4' 3a ba 1a 9a 28 a8 27 a7
feed $G 'üäö leds=4' 3a ba 2a 1a 9a 28 a8 27 a7 aa
feed $G 'ß? leds=0' 0c 8c 2a 0c 8c aa
feed $G '@€ leds=0' e0 38 10 90 12 92 e0 b8
feed $G 'zy leds=0' 15 95 2c ac
feed $G 'ê leds=0' 29 a9 12 92
feed $G 'Ê leds=0' 29 a9 2a 12 92 aa
feed $G '^ leds=0' 29 a9 39 b9
feed $G 'é leds=0' 0d 8d 12 92
feed $G 'è leds=0' 2a 0d 8d aa 12 92
feed $G '<>| leds=0' 56 d6 2a 56 d6 aa e0 38 56 d6 e0 b8
feed $G '{[]}\\~ leds=0' e0 38 08 88 09 89 0a 8a 0b 8b 0c 8c 1b 9b e0 b8
feed $G '^x leds=0' 29 a9 2d ad
feed $G '\e[A\eOP leds=0' e0 48 e0 c8 3b bb

F=$TMP/fr.st
feed $F 'aqwz leds=0' 10 90 1e 9e 2c ac 11 91
feed $F '&é"1 leds=0' 02 82 03 83 04 84 2a 02 82 aa
feed $F 'm leds=0' 27 a7
feed $F 'â leds=0' 1a 9a 10 90
feed $F 'ë leds=0' 2a 1a 9a aa 12 92
feed $F 'AQ leds=4' 3a ba 10 90 1e 9e

R=$TMP/ru.st
feed $R 'q leds=0' 10 90
feed $R 'йцукен leds=0' 38 2a aa b8 10 90 11 91 12 92 13 93 14 94 15 95
feed $R 'Й leds=0' 38 2a aa b8 2a 10 90 aa
feed $R 'ЙЦ leds=4' 38 2a aa b8 3a ba 10 90 11 91
feed $R 'йq leds=0' 38 2a aa b8 10 90 38 2a aa b8 10 90
feed $R 'фыва leds=0' 2a 38 b8 aa 1e 9e 1f 9f 20 a0 21 a1
feed $R 'ё leds=0' 38 2a aa b8 29 a9

N=$TMP/dend.st
feed $N '^e leds=0' 29 a9 12 92
feed $N "'e\`e leds=0" 0d 8d 12 92 2a 0d 8d aa 12 92
feed $N 'ü leds=0' 1a 9a

J=$TMP/jpk.st
feed $J 'q leds=0' 10 90
feed $J 'タチ leds=0' 38 2a aa b8 10 90 1e 9e
feed $J 'ロー leds=0' 38 2a aa b8 73 f3 7d fd

load brai --no-save brai
B=$TMP/brai.st
feed $B '⠉⠁ leds=0' 38 2a aa b8 21 24 a1 a4 21 a1

# Typing a character by its number (Alt + keypad digits) and Compose.
feed $U 'Aé leds=0' 38 4d cd 4c cc b8 38 50 d0 51 d1 51 d1 b8
printf 'keycode 127 = Compose\n' >"$TMP/compose.map"
load comp --no-save "$TMP/compose.map"
C=$TMP/comp.st
feed $C 'æé leds=0' e0 5d e0 dd 1e 9e 12 92 e0 5d e0 dd 28 a8 12 92

# ---- every generated keymap loads without a complaint --------------------------
n=0
for f in "$KEYMAPS"/*.map; do
	id=$(basename "$f" .map)
	if ! KBDTEST_STATE_OUT="$TMP/all.st" "$TMP/loadkeys" -C /dev/null --no-save \
		"$f" >"$TMP/all.out" 2>&1 || [ -s "$TMP/all.out" ]; then
		fail=$((fail + 1))
		echo "FAIL loadkeys $id:"
		head -5 "$TMP/all.out"
	else
		n=$((n + 1))
	fi
done
pass=$((pass + 1))
echo "test-keymap: $n keymaps loaded cleanly"

# ---- the system keyboard setting -------------------------------------------------
rm -f "$TMP/helper.log"
load s1 de
expect_log "loadkeys de" "--console de"
load s2 de-latin1
expect_log "loadkeys de-latin1 (alias)" "--console de"
load s3 -q us
expect_log "loadkeys -q us" "--console us"
load s4 --no-save fr
expect_log "loadkeys --no-save fr" ""
cp "$KEYMAPS/de.map" "$TMP/de.map"
load s5 "$TMP/de.map"
expect_log "loadkeys of a file outside the keymap directory" ""
load s6 "$KEYMAPS/fr.map"
expect_log "loadkeys of the keymap's own path" "--console fr"

# Not allowed to load: the helper's LAYOUT VARIANT OPTIONS form, options kept.
KBDTEST_EPERM=1 load s7 de
expect_log "loadkeys de without permission" "de  grp:alt_shift_toggle"
KBDTEST_EPERM=1 load s8 jp-kana
expect_log "loadkeys jp-kana without permission" "jp kana grp:alt_shift_toggle"
KBDTEST_NOCONSOLE=1 load s9 ru
expect_log "loadkeys ru without a console" "ru  grp:alt_shift_toggle"
if KBDTEST_EPERM=1 LOADKEYS_KEYMAP_PATH="$KEYMAPS" "$TMP/loadkeys" -C /dev/null \
	--no-save de >"$TMP/np.out" 2>&1; then
	fail=$((fail + 1))
	echo "FAIL loadkeys --no-save without permission succeeded"
else
	pass=$((pass + 1))
fi
expect_log "loadkeys --no-save without permission" ""

# A missing helper is a warning; the keymap is loaded all the same.
mv "$TMP/helper" "$TMP/helper.away"
load s10 de
if grep -q "not saved" "$TMP/s10.out" && cmp -s "$TMP/s10.st" "$TMP/de.st"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL missing helper: no warning or keymap not loaded"
	cat "$TMP/s10.out"
fi
mv "$TMP/helper.away" "$TMP/helper"
rm -f "$TMP/helper.log"

# ---- dumpkeys round trip ----------------------------------------------------------
for m in default us de fr ru jpk; do
	if [ $m = default ]; then
		env -u KBDTEST_STATE KBDTEST_STATE_OUT="$TMP/default.st" \
			"$TMP/dumpkeys" -C /dev/null >"$TMP/$m.dump"
	else
		KBDTEST_STATE="$TMP/$m.st" "$TMP/dumpkeys" -C /dev/null >"$TMP/$m.dump"
	fi
	KBDTEST_STATE_OUT="$TMP/$m.rt" "$TMP/loadkeys" -C /dev/null --no-save \
		"$TMP/$m.dump" >/dev/null 2>&1
	KBDTEST_STATE="$TMP/$m.rt" "$TMP/dumpkeys" -C /dev/null >"$TMP/$m.dump2"
	if cmp -s "$TMP/$m.st" "$TMP/$m.rt" && cmp -s "$TMP/$m.dump" "$TMP/$m.dump2"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL dumpkeys round trip of $m"
		diff "$TMP/$m.dump" "$TMP/$m.dump2" | head -20
	fi
done
expect_log "round trip loads" ""

# The compiled-in keymap is shipped as /usr/share/keymaps/defkeymap.map, the
# file `loadkeys -d' loads: it must be what dumpkeys prints for it.
# KBDTEST_UPDATE=1 rewrites it.
if [ "${KBDTEST_UPDATE:-0}" = 1 ]; then
	{
		echo "# defkeymap.map - the console's compiled-in keymap (US)"
		echo "#"
		echo "# What dumpkeys prints for the keymap the kernel starts with;"
		echo "# 'loadkeys -d' loads it.  Written by host/test-keymap.sh"
		echo "# (KBDTEST_UPDATE=1), which also checks it is current."
		echo "#"
		echo "# Copyright (C) 2026 The LikeOS Project"
		cat "$TMP/default.dump"
	} >"$PORT/defkeymap.map"
fi
if grep -v '^#' "$PORT/defkeymap.map" | cmp -s - "$TMP/default.dump"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
	echo "FAIL $PORT/defkeymap.map is not the compiled-in keymap (KBDTEST_UPDATE=1)"
fi

echo "test-keymap: $pass passed, $fail failed"
[ "$fail" = 0 ]
