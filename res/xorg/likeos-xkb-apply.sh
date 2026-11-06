#!/bin/sh
#
# likeos-xkb-apply - give the X session the keyboard layout of the system.
#
# Reads /etc/likeos/keyboard.conf (LAYOUT, VARIANT, OPTIONS: comma lists in
# XKB terms, written by likeos-keyboard-save) and has the X server on $DISPLAY
# load it with setxkbmap.  xinitrc runs it when the session starts, and the
# taskbar runs it whenever the file is replaced -- which is how a layout
# chosen in Settings, or loaded on the console with loadkeys, reaches a
# session that is already running.
#
# The options are reset first (-option ""): without that setxkbmap adds the
# new options to the ones the server already has, and an option switched off
# in Settings would stay on until the next session.
#
# A layout with no Latin letters (Russian, Greek, Arabic, Thai, ...; the list
# is made with the console keymaps, /usr/share/likeos/keyboard/nonlatin.list)
# is given the US layout as a second one, with Alt+Shift switching between
# the two, when it is the only layout configured.  Without it nothing typed
# in the session could be a web address, a file name or a command, and the
# only way back would be Settings, with the mouse.  The console keymaps are
# made the same way.
#
# The file is parsed, not sourced: it is root's, but a program that is run
# for every user has no business executing it.
#
# Copyright (C) 2026 The LikeOS Project

conf=/etc/likeos/keyboard.conf
catalogue=/usr/share/likeos/keyboard/layouts.tsv
nonlatin=/usr/share/likeos/keyboard/nonlatin.list

LAYOUT=us
VARIANT=
OPTIONS=

if [ -r "$conf" ]; then
	while IFS='=' read -r key value; do
		# Optional quotes around the value, and a trailing comment.
		value=${value%%#*}
		value=$(printf '%s' "$value" | tr -d "\"' \t\r")
		case "$key" in
		LAYOUT) LAYOUT=$value ;;
		VARIANT) VARIANT=$value ;;
		OPTIONS) OPTIONS=$value ;;
		esac
	done < "$conf"
fi
[ -n "$LAYOUT" ] || LAYOUT=us

# Only the characters XKB names are made of reach setxkbmap.
case "$LAYOUT$VARIANT$OPTIONS" in
*[!A-Za-z0-9_:,+-]*)
	echo "likeos-xkb-apply: $conf: invalid setting, using the US layout" >&2
	LAYOUT=us VARIANT= OPTIONS=
	;;
esac

# The catalogue id of a layout and variant, the name nonlatin.list uses.
layout_id() {
	[ -r "$catalogue" ] || return 1
	tab=$(printf '\t')
	while IFS="$tab" read -r id name lang lay var; do
		case "$id" in '#'* | '') continue ;; esac
		if [ "$lay" = "$1" ] && [ "$var" = "$2" ]; then
			printf '%s\n' "$id"
			return 0
		fi
	done < "$catalogue"
	return 1
}

case "$LAYOUT" in
*,*) ;;
*)
	id=$(layout_id "$LAYOUT" "$VARIANT")
	if [ -n "$id" ] && [ -r "$nonlatin" ] &&
	   grep -qxF -e "$id" "$nonlatin"; then
		LAYOUT="$LAYOUT,us"
		VARIANT="$VARIANT,"
		case ",$OPTIONS," in
		*,grp:*) ;;
		*) OPTIONS="${OPTIONS:+$OPTIONS,}grp:alt_shift_toggle" ;;
		esac
	fi
	;;
esac

exec setxkbmap -layout "$LAYOUT" -variant "$VARIANT" -option "" \
	${OPTIONS:+-option "$OPTIONS"}
