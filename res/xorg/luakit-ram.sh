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
# PRIVACY: /ram is mode 01777 -- world-writable and sticky, like /tmp --
# so a tree placed there straight off would let every account on the
# machine read another's cookies, history and cached pages.  Each user
# therefore gets /ram/<name>, created 0700 and owned by them, with the
# data and cache trees inside it.  Two things matter about how that is
# done, and both are deliberate:
#
#   * The directory is created by `mkdir -m 700', which passes the mode
#     to mkdir(2) itself.  Creating it first and chmod'ing it afterwards
#     would leave a window in which it existed world-readable, and a
#     window is all a watcher on a shared machine needs.
#   * Because /ram is world-writable, the name may ALREADY EXIST and
#     belong to somebody else, who is then free to read everything put
#     into it.  So an existing directory is used only when it is a real
#     directory (not a symlink pointing somewhere else), owned by us.
#     Anything else makes this script fall back to running from the
#     disk rather than browse into a tree another account controls.
#
# Refusing costs this session the RAM speedup, not its privacy: it runs
# from the disk, exactly as it did before this launcher existed.  Nobody
# can engineer the opposite mistake either -- changing a file's owner is
# privileged (kernel/fs/vfs.c, vfs_chown), so a directory somebody else
# controls can never be made to look like ours.  And once the directory
# IS ours the sticky bit keeps it that way: vfs_permission_remove lets an
# unprivileged task remove only an entry it owns.  So the worst another
# account can do by sitting on the name is slow this session down.
#
# WHAT SURVIVES: the DATA tree is copied INTO /ram on the first start after
# boot and copied BACK to the disk every time luakit exits, so nothing is
# lost across reboots -- as long as luakit exits; a hard reset loses what
# changed since the last exit.  That tree is the one that matters: cookies
# are in it (soup.cookies_storage is luakit.data_dir .. "/cookies.db"), and
# so are history, bookmarks, the session and the settings.
#
# The CACHE tree is seeded in when a copy is already on the disk, but is NOT
# written back, because it is a cache: it holds WebKit's HTTP cache and
# favicons, all of it rebuildable, and it is the bulk of the bytes.  Copying
# it back would put a large write to the USB stick at the end of every
# session -- exactly the cost this launcher exists to avoid.  To keep it
# anyway, at that price:
#
#     LUAKIT_RAM_SAVE_CACHE=1 luakit
#
# THE SWITCH -- how to go back to running from disk:
#
#     LUAKIT_RAM=0 luakit           for one run, or
#     touch ~/.config/luakit/use-disk   for every run of this user
#
# Either makes this script exec /usr/bin/luakit untouched.  To make disk
# the default again for everyone, change the "1" in the LUAKIT_RAM
# default below to "0"; the RAM path then needs LUAKIT_RAM=1 to be chosen.
#
# Copyright (C) 2026 The LikeOS Project

real=/usr/bin/luakit
ram=/ram

case "${LUAKIT_RAM:-1}" in
0|no|off|false) exec "$real" "$@" ;;
esac
[ -n "$HOME" ] || exec "$real" "$@"
[ -e "$HOME/.config/luakit/use-disk" ] && exec "$real" "$@"
[ -d "$ram" ] || exec "$real" "$@"

# The tree is named for the user, as asked, so /ram says at a glance whose
# session is whose.  The name comes from the passwd entry; `id -un' prints
# the uid instead when there is no entry, which is a fine directory name
# too.  It still has to be checked before it is pasted into a path: "." and
# ".." would name /ram itself and its parent, and a name carrying a slash
# would escape /ram altogether.  Anything not a plain name falls back to
# the uid, which is always safe.
uid=$(id -u)
user=$(id -un 2>/dev/null)
case "$user" in
''|.|..|*[!A-Za-z0-9._-]*) user=$uid ;;
esac

base="$ram/$user"

# Everything below this line creates files that belong to one user only.
umask 077

if ! mkdir -m 700 "$base" 2>/dev/null; then
	# Already there, or uncreatable.  Trust it only if it is ours.  -L
	# first: -d follows symlinks, so a symlink to somebody else's
	# directory would otherwise pass the -d test.
	if [ -L "$base" ] || [ ! -d "$base" ] ||
	   [ "$(stat -c %u "$base" 2>/dev/null)" != "$uid" ]; then
		echo "luakit: cannot use $base -- it could not be created, or it" >&2
		echo "luakit: already exists and is not a directory owned by uid $uid." >&2
		echo "luakit: refusing to put this session's files there; running from disk" >&2
		exec "$real" "$@"
	fi
	# Ours, but it may have been left readable by an earlier run of an
	# older version of this script.  Only the owner may chmod, and we
	# just established we are the owner.
	chmod 700 "$base" || exec "$real" "$@"
fi

# The disk locations are the XDG defaults unless the session already points
# them somewhere else.
data_disk="${XDG_DATA_HOME:-$HOME/.local/share}"
cache_disk="${XDG_CACHE_HOME:-$HOME/.cache}"

mkdir -m 700 -p "$base/data" "$base/cache" || exec "$real" "$@"

# First start this boot: bring both trees along.  Later starts find the RAM
# copies already there, newer than the disk's.
if [ ! -e "$base/seeded" ]; then
	[ -d "$data_disk/luakit" ] && cp -a "$data_disk/luakit" "$base/data/"
	[ -d "$cache_disk/luakit" ] && cp -a "$cache_disk/luakit" "$base/cache/"
	: > "$base/seeded"
fi

XDG_DATA_HOME="$base/data" XDG_CACHE_HOME="$base/cache" "$real" "$@"
rc=$?

# Write a RAM tree back to the disk, replacing the disk copy in one rename
# so a crash in the middle leaves either the old tree or the new one, never
# a half-copied mix.  $1 is the RAM parent, $2 the disk parent.
save_back() {
	[ -d "$1/luakit" ] || return 0
	mkdir -p "$2" || return 1
	rm -rf "$2/luakit.new"
	if ! cp -a "$1/luakit" "$2/luakit.new"; then
		rm -rf "$2/luakit.new"
		return 1
	fi
	rm -rf "$2/luakit.old"
	[ -d "$2/luakit" ] && mv "$2/luakit" "$2/luakit.old"
	mv "$2/luakit.new" "$2/luakit" && rm -rf "$2/luakit.old"
}

save_back "$base/data" "$data_disk"
case "${LUAKIT_RAM_SAVE_CACHE:-0}" in
1|yes|on|true) save_back "$base/cache" "$cache_disk" ;;
esac
exit $rc
