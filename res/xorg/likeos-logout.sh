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
# Where the desktop ends up decides what signing out means.  A desktop the
# graphical login started (LIKEOS_SESSION=lightdm) simply ends, and the
# login screen comes back.  One started with startx from a console login
# ends that console login as well, and the login screen comes back in its
# place: likeos-dm is asked for it first (it waits for this desktop's X
# server to go before it hangs the console login up), then the desktop is
# ended as always.  A system without likeos-dm only loses the request.
#
# --console (the bar's "Console" in a desktop started with startx): the
# desktop ends and nothing else, so the console login it was started from
# is back -- no request for the graphical login.
#
# Copyright (C) 2026 The LikeOS Project

pidfile="$HOME/.ctwm.pid"

# One logout at a time: a second click does nothing.  The lock names the
# logout holding it, and one whose logout is gone is taken over: the
# session usually ends while this runs, before the lock could be removed,
# and /tmp is kept across restarts -- a lock left like that would otherwise
# stop every later logout of this user.
lock="${TMPDIR:-/tmp}/likeos-logout.$(id -u)"
holder_runs() {
	old=$(cat "$lock/pid" 2>/dev/null)
	case "$old" in
	'' | *[!0-9]*) return 1 ;;
	esac
	cmd=$({ tr '\000' ' ' <"/proc/$old/cmdline"; } 2>/dev/null)
	case "$cmd" in
	*likeos-logout*) return 0 ;;
	esac
	return 1
}
if ! mkdir "$lock" 2>/dev/null; then
	if holder_runs; then
		exit 0
	fi
	rm -rf "$lock"
	mkdir "$lock" 2>/dev/null || exit 0
fi
echo $$ >"$lock/pid"
trap 'rm -rf "$lock" 2>/dev/null' EXIT
trap 'exit 1' HUP INT TERM

# Without -w: returns as soon as audiod has the request.
if command -v eventsound >/dev/null 2>&1; then
	eventsound -q logout >/dev/null 2>&1
fi

to_console=0
if [ "$1" = "--console" ]; then
	to_console=1
fi

if [ "$to_console" = 0 ] && [ "$LIKEOS_SESSION" != "lightdm" ] &&
	[ -x /usr/libexec/likeos-session-switch ]; then
	/usr/libexec/likeos-session-switch graphical >/dev/null 2>&1
fi

pid=$(cat "$pidfile" 2>/dev/null)
case "$pid" in
'' | *[!0-9]*)
	echo "likeos-logout: no window manager pid in $pidfile" >&2
	exit 1
	;;
esac
kill -TERM "$pid"
