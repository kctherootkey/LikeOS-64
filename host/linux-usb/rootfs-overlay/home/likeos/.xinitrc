#!/bin/sh
# Minimal X11 session that jumps straight into the LikeOS QEMU instance
DEBUG=${HOBBY_DEBUG:-1}
LOGDIR=/home/likeos/.local/share
mkdir -p "$LOGDIR"

xsetroot -solid "#000000"
xset -dpms
xset s off

if [ "$DEBUG" = "1" ]; then
	# Run QEMU in background so we can keep an xterm visible for debugging
	/usr/local/bin/likeos-qemu >>"$LOGDIR/likeos-qemu.log" 2>&1 &
	if command -v xterm >/dev/null 2>&1; then
		xterm -fa Monospace -fs 10 -geometry 100x30+20+20 &
		wait
	else
		# Fallback: keep X alive showing a message
		xmessage -center "xterm not installed; QEMU running in background. Press Ctrl+Alt+F2 for console." &
		wait
	fi
else
	exec /usr/local/bin/likeos-qemu
fi
