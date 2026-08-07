#!/bin/sh
# Copy the shipped subset of the GTK3 stack into an image staging tree.
#
# The counterpart of ../stage.sh, and the same reasoning: the sysroot is a BUILD
# artifact holding static archives, headers, pkg-config descriptions and host
# tooling, none of which belong on the image.  This picks out what the running
# system needs -- about 55MB of a sysroot that is several hundred.
#
# Every item is guarded, so the script is correct at any point in the port: what
# has been built is staged and what has not is skipped.  An image without GTK3
# is a working image; one that silently half-contains it is not.
#
# Usage:  stage.sh <staging-root>

set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
HOSTTOOLS="$here/../.hosttools"
DEST=${1:?usage: stage.sh <staging-root>}

[ -d "$SYSROOT/usr/lib" ] || exit 0

mkdir -p "$DEST/lib" "$DEST/usr/bin" "$DEST/usr/local/bin"

staged=0

# ---------------------------------------------------------------------------
# Shared libraries.
#
# To /lib, where this system keeps them and where every binary looks
# (-rpath /lib).  Only the real files and their SONAME links: the unversioned
# .so names exist for the linker at build time and nothing loads them.
#
# Named one by one rather than copied wholesale.  ../stage.sh already takes
# everything matching usr/lib/*.so.*, which is how the X libraries get there;
# repeating that here would copy each of them a second time, and would also
# sweep up the static archives' companions and the -gdb.py scripts.  A list is
# longer but says exactly what the image is expected to contain.
# ---------------------------------------------------------------------------
stage_lib() {
	for f in "$SYSROOT"/usr/lib/"$1".so.*; do
		[ -e "$f" ] || continue
		case "$f" in
		*-gdb.py) continue ;;
		esac
		base=$(basename "$f")
		if [ -L "$f" ]; then
			cp -a "$f" "$DEST/lib/$base"
		else
			cp "$f" "$DEST/lib/$base"
		fi
		staged=$((staged + 1))
	done
}

# The C++ runtime.  Needed by HarfBuzz -- which Pango cannot do without -- so
# by every GTK program whether or not it is itself C++.
stage_lib libstdc++

# GLib, and the object and I/O layers above it.
for l in libglib-2.0 libgobject-2.0 libgio-2.0 libgmodule-2.0 libgthread-2.0; do
	stage_lib "$l"
done

# What GLib itself needs: closures for GObject signals, and GRegex.
stage_lib libffi
stage_lib libpcre2-8

# Character sets and translations.  libiconv is the reason a mail client can
# read a message in an encoding it has never heard of; libintl is what every
# package above links for its own strings.
stage_lib libcharset
stage_lib libiconv
stage_lib libintl

# Text: shaping, bidirectional ordering, layout.
stage_lib libharfbuzz
stage_lib libharfbuzz-gobject
stage_lib libharfbuzz-subset
stage_lib libfribidi
for l in libpango-1.0 libpangocairo-1.0 libpangoft2-1.0; do stage_lib "$l"; done

# Drawing and images.
stage_lib libcairo
stage_lib libcairo-gobject
stage_lib libgdk_pixbuf-2.0
stage_lib libtiff

# The toolkit.
stage_lib libatk-1.0
stage_lib libepoxy
stage_lib libgdk-3
stage_lib libgtk-3

# Claws Mail's own dependencies: TLS, the mail protocols, spell checking, and
# the protocol that tells the window manager an application is starting.
for l in libgmp libnettle libhogweed libtasn1 libgnutls; do stage_lib "$l"; done
stage_lib libetpan
stage_lib libenchant-2
stage_lib libhunspell-1.7
stage_lib libstartup-notification-1

# ---------------------------------------------------------------------------
# Programs.
# ---------------------------------------------------------------------------
if [ -f "$SYSROOT/usr/bin/claws-mail" ]; then
	cp "$SYSROOT/usr/bin/claws-mail" "$DEST/usr/bin/claws-mail"
	staged=$((staged + 1))
fi

# The GLib and fontconfig command-line tools.  Small, and each answers a
# question that is otherwise unanswerable on a running system: what settings a
# schema holds, what a font name resolves to, what a URI scheme maps to.
for b in gio gsettings gdbus gapplication fc-list fc-match fc-cache; do
	[ -f "$SYSROOT/usr/bin/$b" ] || continue
	cp "$SYSROOT/usr/bin/$b" "$DEST/usr/bin/$b"
	staged=$((staged + 1))
done

# The C++ runtime's own test program -- the first thing to run when a C++
# program misbehaves in a way that looks like the language rather than the
# program.  Alongside the other diagnostics in /usr/local/bin.
if [ -f "$root/build/testcxx" ]; then
	cp "$root/build/testcxx" "$DEST/usr/local/bin/testcxx"
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# GTK's input modules.
#
# dlopen'd by name from a fixed path, so unlike the libraries above they stay
# where they were built.  These are the input methods for scripts a keyboard
# cannot type directly; without them GTK still accepts ordinary typing.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/lib/gtk-3.0" ]; then
	(cd "$SYSROOT/usr/lib" && find gtk-3.0 -name '*.so' -print) |
		while read -r m; do
			mkdir -p "$DEST/usr/lib/$(dirname "$m")"
			cp "$SYSROOT/usr/lib/$m" "$DEST/usr/lib/$m"
		done
fi

# ---------------------------------------------------------------------------
# Spell checking.
#
# Two pieces, and both have to be present or neither does anything.
#
# The Enchant PROVIDER is dlopen'd from /usr/lib/enchant-2 by name, so like the
# GTK input modules it stays where it was built rather than moving to /lib.
#
# The DICTIONARY goes to /usr/share/hunspell because that is the only place the
# provider looks: it builds its search list from g_get_system_data_dirs() with
# its own name appended.  Claws asks Enchant for an en_US speller as soon as a
# compose window opens and reports a refusal as an error, so a missing
# dictionary is not a quietly absent feature -- it is a dialog box every time.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/lib/enchant-2" ]; then
	mkdir -p "$DEST/usr/lib/enchant-2"
	for m in "$SYSROOT"/usr/lib/enchant-2/*.so; do
		[ -e "$m" ] || continue
		cp "$m" "$DEST/usr/lib/enchant-2/"
		staged=$((staged + 1))
	done
fi
if [ -d "$SYSROOT/usr/share/hunspell" ]; then
	mkdir -p "$DEST/usr/share/hunspell"
	cp "$SYSROOT"/usr/share/hunspell/* "$DEST/usr/share/hunspell/" 2>/dev/null
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# GSettings schemas.
#
# NOT optional: GTK reads org.gtk.Settings.* at start-up and aborts if the
# compiled database is missing, so a GTK program on an image without this does
# not start at all.
#
# Compiled HERE with the host's glib-compile-schemas -- the port's own, built
# from the same GLib tarball as the target library -- because the compiled form
# is what GTK reads and nothing on the image can produce it.  The XML sources
# are shipped alongside so that a schema can be inspected on the running
# system.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/glib-2.0/schemas" ]; then
	mkdir -p "$DEST/usr/share/glib-2.0/schemas"
	cp "$SYSROOT"/usr/share/glib-2.0/schemas/*.xml \
		"$DEST/usr/share/glib-2.0/schemas/" 2>/dev/null || true
	if [ -x "$HOSTTOOLS/bin/glib-compile-schemas" ]; then
		"$HOSTTOOLS/bin/glib-compile-schemas" \
			"$DEST/usr/share/glib-2.0/schemas" >/dev/null
		staged=$((staged + 1))
	else
		echo "gtk3/stage.sh: no host glib-compile-schemas;" \
		     "GTK programs will abort at start-up" >&2
	fi
fi

# ---------------------------------------------------------------------------
# Fonts.
#
# Also not optional.  The image otherwise carries bitmap PCF fonts only, and
# Pango renders through FreeType: with no scalable font installed, every GTK
# program comes up with blank or boxed text and reports nothing.
#
# The fontconfig rules that come with them are already in the sysroot's
# /etc/fonts/conf.d, which ../stage.sh copies wholesale.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/fonts/truetype" ]; then
	mkdir -p "$DEST/usr/share/fonts"
	cp -a "$SYSROOT/usr/share/fonts/truetype" "$DEST/usr/share/fonts/"
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# Icon themes.
#
# hicolor is the fallback every icon lookup ends at, and is tiny.  Adwaita is
# where the icons actually come from, and is 42MB shipped whole -- so it is
# not shipped whole:
#
#   cursors/          12MB.  A complete X cursor theme, which the X server and
#                     ctwm already provide; GTK does not read it.
#   scalable/         SVG, which needs librsvg to render.  That is not ported
#                     (it would bring a Rust toolchain with it), so gdk-pixbuf
#                     cannot load a single one of these files.
#   256x256, 512x512  Only ever used by something explicitly asking for an icon
#   96x96, 64x64      that large.  Nothing here does.
#
# What stays is 8 through 48, which is every size GtkIconSize maps to: MENU and
# BUTTON at 16, LARGE_TOOLBAR at 24, DND at 32, DIALOG at 48.  GTK scales from
# the nearest available size, so a request outside the set is still answered.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/icons/hicolor" ]; then
	mkdir -p "$DEST/usr/share/icons"
	cp -a "$SYSROOT/usr/share/icons/hicolor" "$DEST/usr/share/icons/"
	staged=$((staged + 1))
fi
if [ -d "$SYSROOT/usr/share/icons/Adwaita" ]; then
	mkdir -p "$DEST/usr/share/icons/Adwaita"
	cp "$SYSROOT/usr/share/icons/Adwaita/index.theme" \
		"$DEST/usr/share/icons/Adwaita/" 2>/dev/null || true
	for sz in 8x8 16x16 22x22 24x24 32x32 48x48; do
		[ -d "$SYSROOT/usr/share/icons/Adwaita/$sz" ] || continue
		cp -a "$SYSROOT/usr/share/icons/Adwaita/$sz" \
			"$DEST/usr/share/icons/Adwaita/"
	done
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# Themes and other shared data a GTK program looks for by absolute path.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/themes" ]; then
	mkdir -p "$DEST/usr/share"
	cp -a "$SYSROOT/usr/share/themes" "$DEST/usr/share/"
fi

# ---------------------------------------------------------------------------
# Configuration, from this repository rather than from the build.
# ---------------------------------------------------------------------------
if [ -f "$root/res/xorg/gtk3/settings.ini" ]; then
	mkdir -p "$DEST/etc/gtk-3.0"
	cp "$root/res/xorg/gtk3/settings.ini" "$DEST/etc/gtk-3.0/settings.ini"
fi
if [ -f "$root/res/xorg/gtk3/local.conf" ]; then
	mkdir -p "$DEST/etc/fonts"
	cp "$root/res/xorg/gtk3/local.conf" "$DEST/etc/fonts/local.conf"
fi

# Claws Mail's skeleton configuration.
#
# Copied to ~/.claws-mail on a user's first run (copy_dir of SYSCONFDIR/skel/
# .claws-mail in src/main.c).  Claws survives its absence -- it falls back to
# creating an empty directory -- but then complains about it on every first
# start, and takes upstream's font defaults, which name fontconfig aliases
# rather than the one scalable family this image actually carries.
if [ -d "$root/res/xorg/gtk3/skel-claws-mail" ]; then
	mkdir -p "$DEST/etc/skel/.claws-mail"
	cp "$root"/res/xorg/gtk3/skel-claws-mail/* \
		"$DEST/etc/skel/.claws-mail/"
fi

# ---------------------------------------------------------------------------
# Drop debug information.
#
# The single largest saving here: these libraries are 172MB with it and 31MB
# without, and it is unusable on a system with no debugger to read it.
#
# --strip-debug, not a full strip, for the reason ../stage.sh gives: it never
# touches .dynsym, which is what the loader resolves against.  Set NO_STRIP=1
# to keep it, matching the kernel build.
# ---------------------------------------------------------------------------
if [ "${NO_STRIP:-0}" != "1" ] && [ "$staged" -gt 0 ]; then
	find "$DEST/lib" "$DEST/usr/bin" "$DEST/usr/local/bin" \
		"$DEST/usr/lib/gtk-3.0" "$DEST/usr/lib/enchant-2" -type f \
		\( -name '*.so*' -o -perm -u+x \) \
		-exec strip --strip-debug {} + 2>/dev/null || true
fi

exit 0
