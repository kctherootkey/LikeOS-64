#!/bin/sh
# Copy the shipped subset of the X.Org sysroot into an image staging tree.
#
# The sysroot is a BUILD artifact: it also holds static archives, pkg-config
# descriptions, aclocal macros, developer headers and host-side tools, none of
# which belong on the image.  This picks out what the running system needs and
# nothing else -- the sysroot is roughly 200 MB, what ships is a fraction of it.
#
# Usage:  stage.sh <staging-root>

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
DEST=${1:?usage: stage.sh <staging-root>}

[ -d "$SYSROOT/usr/bin" ] || {
	echo "stage.sh: no X.Org sysroot at $SYSROOT (run ports/xorg/build.sh)" >&2
	exit 1
}

mkdir -p "$DEST/lib" "$DEST/usr/bin" "$DEST/etc/X11" \
	"$DEST/usr/lib/xorg/modules" "$DEST/usr/share/X11" \
	"$DEST/usr/share/fonts/X11/misc" "$DEST/var/log"

# ---------------------------------------------------------------------------
# Shared libraries.
#
# They go to /lib, not /usr/lib, because that is where this system keeps shared
# libraries and what every binary here is linked with (-rpath /lib).  Only the
# real files and their SONAME links are copied: the unversioned .so symlinks
# exist for the linker at build time and nothing loads them at runtime.
# ---------------------------------------------------------------------------
for f in "$SYSROOT"/usr/lib/*.so.*; do
	[ -e "$f" ] || continue
	# libstdc++ installs a debugger script beside the library, named
	# libstdc++.so.<version>-gdb.py, which this glob matches.  It teaches gdb
	# to print an std::string readably and is of no use to a system with
	# neither gdb nor Python on it.
	case "$f" in
	*-gdb.py) continue ;;
	esac
	base=$(basename "$f")
	if [ -L "$f" ]; then
		cp -a "$f" "$DEST/lib/$base"
	else
		cp "$f" "$DEST/lib/$base"
	fi
done

# ---------------------------------------------------------------------------
# Server modules.
#
# These are dlopen()ed by the server at the ModulePath in xorg.conf, so unlike
# the libraries above their location is fixed by configuration and they stay
# where they were built.  The .la files libtool leaves behind are not used --
# the server loads the .so directly.
# ---------------------------------------------------------------------------
(cd "$SYSROOT/usr/lib/xorg" && find . -name '*.so' -print) | while read -r m; do
	mkdir -p "$DEST/usr/lib/xorg/$(dirname "$m")"
	cp "$SYSROOT/usr/lib/xorg/$m" "$DEST/usr/lib/xorg/$m"
done

# ---------------------------------------------------------------------------
# Programs.
#
# Xorg and the keymap compiler are needed at runtime -- the server execs
# xkbcomp to turn the keyboard description into a keymap, so without it there
# is a display and no working keyboard.  The rest of the sysroot's bin/ is
# build-time tooling (ucs2any, bdftruncate) or diagnostics that run on the
# build host, and is left out.
# ---------------------------------------------------------------------------
cp "$SYSROOT/usr/bin/Xorg" "$DEST/usr/bin/Xorg"
cp "$SYSROOT/usr/bin/xkbcomp" "$DEST/usr/bin/xkbcomp"
ln -sfn Xorg "$DEST/usr/bin/X"

# Xorg is setuid root.
#
# It has to open the framebuffer, the event devices and its log, all of which
# are root-owned, and it cannot be given those any other way here: there is no
# seat manager to hand a logged-in user the devices for the duration of a
# session.  The alternative was to put the user in the `video` and `input`
# groups, which is a permanent grant of read/write access to the keyboard and
# the screen -- enough to read every keystroke typed into any session on the
# machine, root's included, and to read back what any session displays, for as
# long as the account exists.
#
# The server gives root up as soon as those descriptors are open, before it
# serves a single client (dix/main.c, patched).  Root-owned and NOT
# group-writable: the point is that only the setuid bit grants this, so nothing
# else needs to.
chown 0:0 "$DEST/usr/bin/Xorg" 2>/dev/null || true
chmod 4755 "$DEST/usr/bin/Xorg"

# cvt computes modelines; small, and the only way to work out a mode by hand.
[ -f "$SYSROOT/usr/bin/cvt" ] && cp "$SYSROOT/usr/bin/cvt" "$DEST/usr/bin/cvt"

# The session: startx/xinit start it, xauth authorises clients against the
# display, xsetroot paints the root window, xterm is the terminal, ctwm manages
# the windows, resize reports a terminal's size back to the shell.
#
# Then the extras: xset (server settings), xrandr (screen configuration),
# xclock, and twm as a fallback window manager -- something to fall back to if
# a change to system.ctwmrc leaves ctwm unable to start.
#
# Listed by name rather than copied wholesale, because the sysroot's bin/ also
# holds build tooling (ucs2any, bdftruncate) and host-side diagnostics.
for b in startx xinit xauth xsetroot xterm uxterm resize ctwm \
	 xset xrandr xclock xload xcalc xnedit xnc twm \
	 modetest vbltest proptest; do
	[ -f "$SYSROOT/usr/bin/$b" ] && cp "$SYSROOT/usr/bin/$b" "$DEST/usr/bin/$b"
done

# ---------------------------------------------------------------------------
# Desktop entries for the X programs that install none of their own.
#
# A .desktop file is the only place the system records that a program exists,
# what it is called and what it can open.  Three things read them and none of
# them existed until the GTK3 port: the applications menu, the file manager's
# "Open With" list, and the default-application lookup that decides what a
# double-clicked file opens with.  xterm is the program here that other
# software needs to be able to NAME -- libfm looks for xterm.desktop when
# asked to open a terminal -- and the browser's entry, which text/html has to
# resolve to, comes from luakit's own package in the GTK3 port.
#
# The index of this directory (mimeinfo.cache) is generated by the GTK3 port's
# staging script, which runs after this one, so these are indexed along with
# the entries that come from the sysroot.
if [ -d "$root/res/xorg/applications" ]; then
	mkdir -p "$DEST/usr/share/applications"
	cp "$root"/res/xorg/applications/*.desktop \
		"$root"/res/xorg/applications/mimeapps.list \
		"$DEST/usr/share/applications/" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Session configuration.
#
# xinit runs ~/.xinitrc if the user has one and this otherwise, so it defines
# what a bare `startx` gets.  ctwm falls back to system.ctwmrc the same way.
# ---------------------------------------------------------------------------
mkdir -p "$DEST/etc/X11/xinit" "$DEST/etc/X11/ctwm"
cp "$root/res/xorg/xinitrc" "$DEST/etc/X11/xinit/xinitrc"
chmod 755 "$DEST/etc/X11/xinit/xinitrc"

# The server command line.  startx execs this file, so it needs the exec bit as
# much as xinitrc does; it is what lets a non-root user start a session (the
# server's default log path is not writable for them).
cp "$root/res/xorg/xserverrc" "$DEST/etc/X11/xinit/xserverrc"
chmod 755 "$DEST/etc/X11/xinit/xserverrc"
cp "$root/res/xorg/system.ctwmrc" "$DEST/etc/X11/ctwm/system.ctwmrc"

# Our own resource file, pointed at by $XENVIRONMENT from xinitrc.  Kept
# separate from the app-defaults copied below, which are installed exactly as
# their packages ship them.
cp "$root/res/xorg/Xresources" "$DEST/etc/X11/Xresources"

# Toolkit resource files.  xterm reads its defaults (colours, key bindings,
# the menus on ctrl-click) from these; without them it starts with the bare
# built-in defaults and no menus at all.
if [ -d "$SYSROOT/usr/share/X11/app-defaults" ]; then
	mkdir -p "$DEST/usr/share/X11/app-defaults"
	cp "$SYSROOT"/usr/share/X11/app-defaults/* \
		"$DEST/usr/share/X11/app-defaults/" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Keyboard descriptions.
#
# xkbcomp reads these at every server start.  The layout is a real directory
# plus a symlink, because that is what xkeyboard-config installs and what the
# server's compiled-in XKB path expects to find.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/xkeyboard-config-2" ]; then
	cp -a "$SYSROOT/usr/share/xkeyboard-config-2" "$DEST/usr/share/"
fi
if [ -e "$SYSROOT/usr/share/X11/xkb" ]; then
	cp -a "$SYSROOT/usr/share/X11/xkb" "$DEST/usr/share/X11/xkb"
fi
# Where xkbcomp writes compiled keymaps.  It is created here rather than left
# to the server: it cannot create it itself, and the failure surfaces as a
# keymap compile error rather than a missing directory.
mkdir -p "$DEST/usr/share/X11/xkb/compiled"

# ---------------------------------------------------------------------------
# Fonts.  Uncompressed .pcf plus the generated index; see build.sh for why they
# are not gzipped.
# ---------------------------------------------------------------------------
cp "$SYSROOT"/usr/share/fonts/X11/misc/*.pcf "$DEST/usr/share/fonts/X11/misc/"
cp "$SYSROOT"/usr/share/fonts/X11/misc/fonts.dir "$DEST/usr/share/fonts/X11/misc/"
# fonts.alias is not shipped by any upstream package; it is what gives the
# names "fixed" and "cursor" something to resolve to, and the server refuses to
# start without them.
cp "$root/res/xorg/fonts.alias" "$DEST/usr/share/fonts/X11/misc/fonts.alias"

# ---------------------------------------------------------------------------
# Client-side data files.  XErrorDB turns protocol error codes into readable
# messages; the locale tree is what Xlib consults for compose sequences and
# character-set handling.
# ---------------------------------------------------------------------------
for f in XErrorDB Xcms.txt; do
	[ -f "$SYSROOT/usr/share/X11/$f" ] &&
		cp "$SYSROOT/usr/share/X11/$f" "$DEST/usr/share/X11/$f"
done
[ -d "$SYSROOT/usr/share/X11/locale" ] &&
	cp -a "$SYSROOT/usr/share/X11/locale" "$DEST/usr/share/X11/locale"

# ---------------------------------------------------------------------------
# fontconfig.
#
# Its configuration is NOT optional.  Without /etc/fonts/fonts.conf, fontconfig
# has no font directories to search and every client that renders text through
# Xft -- Motif widgets, xnedit -- comes up with no usable font.  Nothing reports
# an error: fontconfig simply matches nothing.
#
# conf.d holds the ordering and substitution rules (which family stands in for
# another, hinting and antialias defaults).  fonts.conf names the font path this
# build was configured with, /usr/share/fonts/X11/misc, which is where the .pcf
# files staged above actually are.
# ---------------------------------------------------------------------------
if [ -f "$SYSROOT/etc/fonts/fonts.conf" ]; then
	mkdir -p "$DEST/etc/fonts"
	cp "$SYSROOT/etc/fonts/fonts.conf" "$DEST/etc/fonts/fonts.conf"
	# -L, and it matters: upstream ships conf.d as SYMLINKS into
	# /usr/share/fontconfig/conf.avail, and `cp -a' copied the links
	# rather than what they point at.  Every one of the 22 rule files on
	# the image was therefore dangling, and fontconfig ran with no rules
	# at all -- no 45-latin to bind DejaVu Sans to the sans-serif generic,
	# no 30-metric-aliases to turn Arial into Liberation Sans, and no
	# 70-no-bitmaps, which left the fixed-width X11 PCF fonts staged above
	# as legitimate matches for any family a page names.  A web page whose
	# CSS asks for a font that is not installed -- which is nearly every
	# page, since they name Helvetica or Roboto -- was then rendered in a
	# MONOSPACE font, with the layout that follows from it: text wider than
	# the boxes drawn for it, overflowing backgrounds, and links whose
	# clickable area no longer matches where the words appear.
	#
	# GTK programs were unaffected, which is what made this hard to see:
	# Pango asks for "DejaVu Sans" by its exact name from settings.ini and
	# needs no rules to find it.
	if [ -d "$SYSROOT/etc/fonts/conf.d" ]; then
		mkdir -p "$DEST/etc/fonts/conf.d"
		cp -RL "$SYSROOT/etc/fonts/conf.d/." "$DEST/etc/fonts/conf.d/"
	fi
	# The library those links were drawn from.  Not needed now that the
	# rules above are real files, but it is what makes the convention
	# usable on the image: enabling another rule is a symlink into here,
	# exactly as it is on any other system.
	if [ -d "$SYSROOT/usr/share/fontconfig/conf.avail" ]; then
		mkdir -p "$DEST/usr/share/fontconfig"
		cp -a "$SYSROOT/usr/share/fontconfig/conf.avail" \
			"$DEST/usr/share/fontconfig/"
	fi
	# fonts.conf names this cache directory.  Created here because
	# fontconfig cannot create it itself, and without it every client
	# rescans every font directory at startup.
	mkdir -p "$DEST/var/cache/fontconfig"
fi
# The DTD fonts.conf declares.  fontconfig only validates against it when built
# for it, but a missing DTD makes any hand edit of fonts.conf unverifiable.
if [ -f "$SYSROOT/usr/share/xml/fontconfig/fonts.dtd" ]; then
	mkdir -p "$DEST/usr/share/xml/fontconfig"
	cp "$SYSROOT/usr/share/xml/fontconfig/fonts.dtd" \
		"$DEST/usr/share/xml/fontconfig/fonts.dtd"
fi

# ---------------------------------------------------------------------------
# Configuration.
# ---------------------------------------------------------------------------
cp "$root/res/xorg/xorg.conf" "$DEST/etc/X11/xorg.conf"

# The second configuration: the display-manager one, picked by xserverrc when
# /dev/dri/card0 exists.  Both are always installed -- which one runs is a
# question about the machine that booted the image, not about the image.
cp "$root/res/xorg/xorg.conf.modesetting" "$DEST/etc/X11/xorg.conf.modesetting"

# ---------------------------------------------------------------------------
# Drop debug information.
#
# It is more than half the total -- the server alone goes from 12.8 MB to 2.2 MB
# -- and it is unusable on the target, which has no debugger to read it.
#
# --strip-debug, not a full strip: the server is linked --export-dynamic so
# that the modules it dlopen()s can bind back to its symbols, and those live in
# .dynsym.  --strip-debug never touches .dynsym (verified: 2210 dynamic symbols
# before and after), whereas a full strip is a much blunter instrument to point
# at a file whose entire module system depends on its symbol table.
#
# Set NO_STRIP=1 to keep it, matching the kernel build.
# ---------------------------------------------------------------------------
if [ "${NO_STRIP:-0}" != "1" ]; then
	find "$DEST/lib" "$DEST/usr/lib/xorg" "$DEST/usr/bin" -type f \
		\( -name '*.so*' -o -perm -u+x \) \
		-exec strip --strip-debug {} + 2>/dev/null || true
fi

exit 0
