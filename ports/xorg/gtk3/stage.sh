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

# Mousepad: the editing widget and the XML parser it reads its syntax
# definitions with.  libmousepad is the program itself -- the binary in
# /usr/bin is a main() and nothing else, with every line of the editor in this
# library beside it.
stage_lib libxml2
stage_lib libgtksourceview-4
stage_lib libmousepad

# PCManFM: the file-management library, its GTK half, the XML/string helpers
# shared with menu-cache, and menu-cache itself.
stage_lib libfm
stage_lib libfm-gtk3
stage_lib libfm-extra
stage_lib libmenu-cache

# ---------------------------------------------------------------------------
# Programs.
# ---------------------------------------------------------------------------
if [ -f "$SYSROOT/usr/bin/claws-mail" ]; then
	cp "$SYSROOT/usr/bin/claws-mail" "$DEST/usr/bin/claws-mail"
	staged=$((staged + 1))
fi

# The file manager, the editor, and the two small programs libfm installs
# beside itself: libfm-pref-apps sets which application opens which kind of
# file, and lxshortcut edits a .desktop entry -- both are reachable from
# PCManFM's own menus, so leaving either out turns a menu item into nothing
# happening.
for b in pcmanfm mousepad libfm-pref-apps lxshortcut; do
	[ -f "$SYSROOT/usr/bin/$b" ] || continue
	cp "$SYSROOT/usr/bin/$b" "$DEST/usr/bin/$b"
	staged=$((staged + 1))
done

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
# Programs that other programs RUN, by a path compiled into the caller.
#
# None of these is ever typed at a prompt, and each one's absence is a feature
# that silently does not work -- or worse:
#
#   menu-cached        the menu server.  libmenu-cache forks it the first time
#                      anything asks for the application menu, and it does not
#                      cope with it being missing: the call is
#                      g_error("failed to find menu-cached"), and g_error
#                      ABORTS.  So a PCManFM on an image without this does not
#                      show an empty menu, it dies opening one.
#   menu-cache-gen     what menu-cached runs in turn to parse the .menu file
#                      and the .desktop files it names.
#   gio-launch-desktop GLib's own launcher.  Every "open this file with that
#                      application" in the system goes through it --
#                      g_desktop_app_info_launch_uris() spawns this rather
#                      than the target program directly, so that the child is
#                      reparented away from the caller.
#
# The paths are the ones compiled in (MENUCACHE_LIBEXECDIR, GLib's LIBEXECDIR),
# so these keep the layout they were built with rather than moving to /usr/bin.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/libexec" ]; then
	for h in menu-cache/menu-cached menu-cache/menu-cache-gen \
		gio-launch-desktop; do
		[ -f "$SYSROOT/usr/libexec/$h" ] || continue
		mkdir -p "$DEST/usr/libexec/$(dirname "$h")"
		cp "$SYSROOT/usr/libexec/$h" "$DEST/usr/libexec/$h"
		staged=$((staged + 1))
	done
fi

# ---------------------------------------------------------------------------
# libfm's modules.
#
# dlopen'd from /usr/lib/libfm/modules, and not optional decoration: this is
# where several of the file manager's visible features actually live --
# vfs-menu is the applications menu as a browsable folder, vfs-search is the
# search results view, gtk-menu-trash the trash entries in a file's context
# menu, gtk-menu-actions the custom actions, and the two fileprop modules the
# extra Properties pages for .desktop files and shortcuts.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/lib/libfm/modules" ]; then
	mkdir -p "$DEST/usr/lib/libfm/modules"
	for m in "$SYSROOT"/usr/lib/libfm/modules/*.so; do
		[ -e "$m" ] || continue
		cp "$m" "$DEST/usr/lib/libfm/modules/"
		staged=$((staged + 1))
	done
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
# What a file IS: the shared MIME database.
#
# Both the XML source and the binary forms produced from it, because they are
# read by different things -- GLib's g_content_type_guess() (and so every icon,
# description and default application in the file manager) reads mime.cache and
# the glob/magic tables, while the XML is what anyone regenerating the database
# would start from.  Copied whole: it is 2MB and picking through it would mean
# knowing which of the eleven files each reader opens.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/mime" ]; then
	mkdir -p "$DEST/usr/share"
	cp -a "$SYSROOT/usr/share/mime" "$DEST/usr/share/"
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# What can OPEN it: the desktop entries.
#
# One file per installed application, and the only place the system records
# that a program exists, what it is called, which icon it uses and which MIME
# types it handles.  Everything user-visible about "open with" is built from
# this directory: the Open With list, the default application for a type, the
# applications menu, and the entries PCManFM's desktop shows.
#
# Not staged before this port, because until there was a file manager nothing
# read them.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/applications" ]; then
	mkdir -p "$DEST/usr/share/applications"
	cp "$SYSROOT"/usr/share/applications/*.desktop \
		"$DEST/usr/share/applications/" 2>/dev/null || true
	staged=$((staged + 1))
fi

# The index of that directory, WITHOUT which none of it has any effect.
#
# GLib does not read the MimeType= lines of the desktop files when it is asked
# what can open a file -- it reads mimeinfo.cache, which its own source
# describes as "just a cached copy of what we would find in the MimeTypes=
# lines of all of the desktop files".  With no cache the answer is nothing:
# double-clicking a text file offers no application and the Open With list is
# empty, with every desktop file present and correct.
#
# Generated here rather than by update-desktop-database, which is a build-host
# program this tree would otherwise have to require.  The format is one line
# per MIME type naming the desktop files that claim it, and the output of this
# is compared against that tool's byte for byte in the repository's own
# checks.  Only Type=Application entries count, which is what the tool does.
if [ -d "$DEST/usr/share/applications" ]; then
	(
		cd "$DEST/usr/share/applications" || exit 0
		set -- *.desktop
		[ "$1" = '*.desktop' ] && exit 0

		# One awk per file and a sort between the halves, rather than
		# one clever pass: ENDFILE and asorti are gawk's, and the awk
		# on the next machine to build this may be mawk.
		for f in *.desktop; do
			awk -v fn="$f" '
			/^\[/ { entry = ($0 == "[Desktop Entry]"); next }
			!entry { next }
			/^Type[ \t]*=/ {
				sub(/^Type[ \t]*=[ \t]*/, ""); type = $0; next
			}
			/^MimeType[ \t]*=/ {
				sub(/^MimeType[ \t]*=[ \t]*/, ""); mime = $0; next
			}
			END {
				if (type != "Application" || mime == "") exit
				n = split(mime, t, ";")
				for (i = 1; i <= n; i++)
					if (t[i] != "") print t[i], fn
			}' "$f"
		done | LC_ALL=C sort -u | awk '
			BEGIN { print "[MIME Cache]" }
			{
				if ($1 != cur) {
					if (cur != "") printf "\n"
					cur = $1
					printf "%s=", cur
				}
				printf "%s;", $2
			}
			END { if (cur != "") printf "\n" }' > mimeinfo.cache
	)
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# How they are ARRANGED: the menu layout and the category names.
#
# The .menu file is an XML description of which categories exist and in what
# order; the .directory files give each category its display name and icon.
# menu-cache reads the first and menu-cache-gen resolves the second.  With
# neither, the application list is not merely unsorted -- it is empty, because
# there is no menu to walk.
# ---------------------------------------------------------------------------
if [ -d "$SYSROOT/usr/share/desktop-directories" ]; then
	mkdir -p "$DEST/usr/share/desktop-directories"
	cp "$SYSROOT"/usr/share/desktop-directories/*.directory \
		"$DEST/usr/share/desktop-directories/" 2>/dev/null || true
	staged=$((staged + 1))
fi
if [ -d "$SYSROOT/etc/xdg/menus" ]; then
	mkdir -p "$DEST/etc/xdg/menus"
	cp "$SYSROOT"/etc/xdg/menus/*.menu "$DEST/etc/xdg/menus/" 2>/dev/null || true
	staged=$((staged + 1))
fi

# ---------------------------------------------------------------------------
# Each program's own resources.
#
#   libfm/       the GtkBuilder .ui files for every dialog libfm opens, the
#                images it draws its own placeholders from, and the two lists
#                that tell it how to run an archiver and how to run a terminal.
#   pcmanfm/     the same for PCManFM's windows.
#   gtksourceview-4/  the syntax definitions and colour schemes -- 172 language
#                files, and the reason Mousepad can highlight anything.
#
# All three are looked for under the compiled-in datadir, so they keep their
# names; a GTK program whose .ui file is missing fails when the menu item that
# opens that dialog is clicked, not at start-up.
# ---------------------------------------------------------------------------
for d in libfm pcmanfm gtksourceview-4; do
	[ -d "$SYSROOT/usr/share/$d" ] || continue
	mkdir -p "$DEST/usr/share"
	cp -a "$SYSROOT/usr/share/$d" "$DEST/usr/share/"
	staged=$((staged + 1))
done

# ---------------------------------------------------------------------------
# Their system-wide defaults, from the sysroot rather than this repository:
# these are upstream's own files and there is nothing about this system to
# change in them.  The two that DO need changing are copied over them further
# down (see the res/xorg section below).
# ---------------------------------------------------------------------------
for c in libfm/libfm.conf pcmanfm/default/pcmanfm.conf; do
	[ -f "$SYSROOT/etc/xdg/$c" ] || continue
	mkdir -p "$DEST/etc/xdg/$(dirname "$c")"
	cp "$SYSROOT/etc/xdg/$c" "$DEST/etc/xdg/$c"
	staged=$((staged + 1))
done

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

# The file manager's two configuration files, over the upstream copies staged
# further up: which terminal to run and what colour the desktop is are answers
# about THIS system, and the files say so in their own comments.
if [ -f "$root/res/xorg/gtk3/libfm.conf" ]; then
	mkdir -p "$DEST/etc/xdg/libfm"
	cp "$root/res/xorg/gtk3/libfm.conf" "$DEST/etc/xdg/libfm/libfm.conf"
fi
if [ -f "$root/res/xorg/gtk3/pcmanfm.conf" ]; then
	mkdir -p "$DEST/etc/xdg/pcmanfm/default"
	cp "$root/res/xorg/gtk3/pcmanfm.conf" \
		"$DEST/etc/xdg/pcmanfm/default/pcmanfm.conf"
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
		"$DEST/usr/lib/gtk-3.0" "$DEST/usr/lib/enchant-2" \
		"$DEST/usr/lib/libfm" "$DEST/usr/libexec" -type f \
		\( -name '*.so*' -o -perm -u+x \) \
		-exec strip --strip-debug {} + 2>/dev/null || true
fi

exit 0
