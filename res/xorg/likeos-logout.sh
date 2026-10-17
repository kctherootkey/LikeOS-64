#!/bin/sh
#
# likeos-logout - end the desktop session.
#
# Starts the log-out sound while the desktop is still on the screen, then asks
# the window manager to quit at once rather than waiting for the sound: audiod
# is a system service, not part of the session, so the sound plays on while
# the desktop goes.  ctwm shuts down on SIGTERM; xinitrc then returns and
# xinit stops the server.
#
# Started by the "Exit session" entry of the desktop menu and by the taskbar.
# The window manager's pid comes from ~/.ctwm.pid, which xinitrc writes.
#
# Copyright (C) 2026 The LikeOS Project

pidfile="$HOME/.ctwm.pid"

# One logout at a time: a second click does nothing.
lock="${TMPDIR:-/tmp}/likeos-logout.$(id -u)"
if ! mkdir "$lock" 2>/dev/null; then
	exit 0
fi
trap 'rmdir "$lock" 2>/dev/null' EXIT

# Without -w: returns as soon as audiod has the request.
if command -v eventsound >/dev/null 2>&1; then
	eventsound -q logout >/dev/null 2>&1
fi

pid=$(cat "$pidfile" 2>/dev/null)
case "$pid" in
'' | *[!0-9]*)
	echo "likeos-logout: no window manager pid in $pidfile" >&2
	exit 1
	;;
esac
kill -TERM "$pid"
