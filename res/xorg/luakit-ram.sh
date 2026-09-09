#!/bin/sh
# luakit launcher: keep the browser's files in RAM instead of on the disk.
#
# Installed as /usr/local/bin/luakit, ahead of /usr/bin/luakit on the PATH,
# so the window manager's menu, the desktop entry and a shell all reach it.
#
# WHY: everything luakit touches while it browses -- WebKit's HTTP cache,
# the cookie and HSTS databases, luakit's own history, session, settings
# and bookmark files -- lives under ~/.cache and ~/.local/share, on the
# USB stick, through ext4, with an fsync behind every SQLite commit.  To
# see how much of luakit's slowness is that, this launcher moves both
# directories to /ram, the kernel's RAM filesystem (kernel/fs/tmpfs.c),
# for the life of the boot.  GLib reads the locations from XDG_DATA_HOME
# and XDG_CACHE_HOME, so luakit and WebKit's helper processes (which
# inherit the environment) need no change of their own.
#
# WHAT SURVIVES: the data directory (history, bookmarks, cookies, session,
# settings, adblock lists) is copied INTO /ram on the first start after
# boot and copied BACK to the disk every time luakit exits, so nothing is
# lost across reboots -- as long as luakit exits; a hard reset loses what
# changed since the last exit.  The cache directory is not copied either
# way: it is a cache.
#
# THE SWITCH -- how to go back to running from disk:
#
#     LUAKIT_RAM=0 luakit           for one run, or
#     touch ~/.config/luakit/use-disk   for every run of this user
#
# Either makes this script exec /usr/bin/luakit untouched.  To make disk
# the default again for everyone, change the "1" in the LUAKIT_RAM
# default below to "0"; the RAM path then needs LUAKIT_RAM=1 to be chosen.

real=/usr/bin/luakit
ram=/ram

case "${LUAKIT_RAM:-1}" in
0|no|off|false) exec "$real" "$@" ;;
esac
[ -e "$HOME/.config/luakit/use-disk" ] && exec "$real" "$@"
[ -d "$ram" ] || exec "$real" "$@"

# One tree per user; the disk locations are the XDG defaults unless the
# session already points them somewhere else.
base="$ram/luakit-$(id -u)"
data_disk="${XDG_DATA_HOME:-$HOME/.local/share}"
cache_disk="${XDG_CACHE_HOME:-$HOME/.cache}"

mkdir -p "$base/data" "$base/cache" || exec "$real" "$@"

# First start this boot: bring the persistent data along.  Later starts
# find the RAM copy already there, newer than the disk's.
if [ ! -e "$base/seeded" ]; then
	if [ -d "$data_disk/luakit" ]; then
		cp -a "$data_disk/luakit" "$base/data/" || exit 1
	fi
	: > "$base/seeded"
fi

XDG_DATA_HOME="$base/data" XDG_CACHE_HOME="$base/cache" "$real" "$@"
rc=$?

# Write the data directory back, replacing the disk copy in one rename so
# a crash in the middle leaves either the old or the new tree, never a mix.
if [ -d "$base/data/luakit" ]; then
	mkdir -p "$data_disk"
	rm -rf "$data_disk/luakit.new"
	if cp -a "$base/data/luakit" "$data_disk/luakit.new"; then
		rm -rf "$data_disk/luakit.old"
		[ -d "$data_disk/luakit" ] && mv "$data_disk/luakit" "$data_disk/luakit.old"
		mv "$data_disk/luakit.new" "$data_disk/luakit" && rm -rf "$data_disk/luakit.old"
	fi
fi
exit $rc
