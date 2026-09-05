#!/bin/sh
# Build X.Org packages into the port sysroot, in dependency order.
#
# One driver rather than 46 near-identical Makefile.likeos files: the packages
# differ only in which configure options they need, so those live in the table
# below and everything else is shared.  A per-package file would mean copying
# the same cross-build arrangement 46 times and fixing it 46 times.
#
# Each package gets a stamp under .stamps/, so a re-run continues where it left
# off instead of rebuilding the world.
#
# Usage:
#   ./build.sh                 build everything not yet built
#   ./build.sh libX11 libXext  build just these (and rebuild them)
#   ./build.sh -f              force: ignore stamps

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

# Which package set this run drives.
#
# The X.Org manifest is the default.  A sub-port -- ports/xorg/gtk3 -- points
# LIKEOS_PORT_DIR at its own directory and reuses this driver unchanged, so
# there is one cross-build arrangement for two package sets rather than a second
# copy of it that has to be fixed twice.
#
# Only the package set moves.  $here stays ports/xorg throughout, because the
# toolchain wrappers, the host-tool directory and the sysroot are SHARED: a GTK
# program links against the same libX11 the X port built, with the same compiler
# driver, so there is nothing to separate.
port=$(cd "${LIKEOS_PORT_DIR:-$here}" && pwd)
list="$port/packages.list"
stamps="$port/.stamps"
logs="$port/.logs"
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
export LIKEOS_SYSROOT="$SYSROOT"

# Where BUILD-time tools go.
#
# NOT the sysroot: that describes the target, and a binary compiled for this
# machine sitting in it would be one careless copy away from the image.
#
# NOT under build/ either, which was the first attempt: `make clean` deletes
# everything in build/ except xorg-sysroot, so the tools vanished while
# .stamps still said they were built -- and the next build that needed one
# of them failed with "<tool>: not found".  This lives beside .stamps and .logs, which are the
# port's own build state and which the top-level clean does not touch, so the
# three cannot get out of step with each other -- and clean.sh removes all
# three together, keeping only the meson venv `make deps` installs here.
HOSTTOOLS="$here/.hosttools"

# $HOSTTOOLS/share/aclocal is made here rather than left to whichever
# build-host package first installs an m4 file: likeos-autogen.sh points
# aclocal at it, and automake 1.16 (Ubuntu 22.04) treats an -I on a directory
# that does not exist as a fatal error rather than a warning.  Made
# unconditionally so the search path is the same whether or not the GTK stack
# -- the only thing that builds intltool -- is part of this build.
mkdir -p "$stamps" "$logs" \
	"$SYSROOT/usr/include" "$SYSROOT/usr/lib/pkgconfig" \
	"$SYSROOT/usr/share/aclocal" "$SYSROOT/usr/share/pkgconfig" \
	"$SYSROOT/usr/bin" "$HOSTTOOLS/share/aclocal"

# Descriptions for things LikeOS satisfies natively, so a package's dependency
# check succeeds for the right reason instead of being switched off.
for pc in "$here"/toolchain/sysroot-pc/*.pc; do
	[ -f "$pc" ] || continue
	cp -n "$pc" "$SYSROOT/usr/lib/pkgconfig/" 2>/dev/null || true
done

# Publish the system's own curses into the sysroot.
#
# It is not an X.Org package and is not built here -- it lives in
# ports/lib/ncurses-likeos and ships on the image already -- but xterm needs it
# and looks for it the ordinary way, with <curses.h> and -lncurses.  Neither
# spelling resolves to a library called ncurses.so, so the header is published
# and a conventional libncurses.so name is pointed at the real file.
#
# The SONAME recorded in that file is "ncurses.so", so linking through the
# alias still produces a NEEDED of ncurses.so -- which is exactly what is on
# the image.  The alias exists for the build only.
ncurses_dir="$root/ports/lib/ncurses-likeos"
if [ -f "$ncurses_dir/curses.h" ]; then
	for h in curses.h ncurses.h term.h; do
		[ -f "$ncurses_dir/$h" ] &&
			cp -u "$ncurses_dir/$h" "$SYSROOT/usr/include/$h"
	done
	[ -f "$root/build/ncurses.so" ] &&
		ln -sfn "$root/build/ncurses.so" "$SYSROOT/usr/lib/libncurses.so"
fi

force=0
case "${1:-}" in
-f)
	force=1
	shift
	;;
esac
want=$*

# ---------------------------------------------------------------------------
# Per-package configure options.
#
# The recurring themes:
#   --disable-specs/--without-xmlto/--without-fop  documentation needs a
#       toolchain (xmlto, fop, xsltproc) that produces nothing we ship.
#   --disable-malloc0returnsnull  a cross build cannot run the test program
#       that decides this, and guessing wrong corrupts every zero-size alloc.
#   --disable-static  we link everything dynamically; static archives just
#       double the build time.
# ---------------------------------------------------------------------------
# Build the helper programs a package RUNS during its own build.
#
# A package that generates source with a tool it builds itself has a problem
# under cross compilation: configure hands that tool the cross compiler, so the
# result cannot execute on the build host.  It fails as "cannot execute:
# required file not found" -- the target's dynamic loader is missing, not the
# tool.
#
# The tool has to be built for the HOST instead.  Both the object and the
# executable are produced here so make finds them newer than the source and does
# not relink them with the cross compiler.
host_tools() {
	case "$1" in
	motif)
		# config/util/makestrs turns xmstring.list into XmStrDefs.[ch],
		# and every widget in libXm includes those.  It only needs
		# X11/Xos.h, which the build host has.
		cc -O2 -I. -Iconfig/util -c -o config/util/makestrs.o \
			config/util/makestrs.c || return 1
		cc -O2 -o config/util/makestrs config/util/makestrs.o || return 1
		;;
	esac
	return 0
}

pkg_opts() {
	case "$1" in
	xorgproto) echo "--disable-specs" ;;
	xtrans) echo "--disable-docs" ;;
	libXau | libXdmcp) echo "--disable-docs --without-xmlto --without-fop" ;;
	libxcb) echo "--disable-docs --without-doxygen --enable-xinput --enable-xkb" ;;
	libX11)
		echo "--disable-specs --without-xmlto --without-fop \
		      --disable-xf86bigfont --enable-xthreads \
		      --disable-malloc0returnsnull"
		;;
	libXext | libXrender | libXfixes | libXdamage | libXcomposite | \
		libXrandr | libXcursor | libXi | libXtst | libXmu)
		echo "--disable-specs --without-xmlto --without-fop \
		      --disable-malloc0returnsnull"
		;;
	libICE | libSM) echo "--disable-specs --without-xmlto --without-fop --disable-docs" ;;
	libXt) echo "--disable-specs --without-xmlto --without-fop --disable-malloc0returnsnull" ;;
	libxkbfile) echo "--disable-docs" ;;
	libdrm)
		# The test tools (modetest, vbltest, proptest) publish no manual
		# pages -- upstream treats them as developer programs and
		# documents them through their own -h output.  Here they are the
		# first thing anyone reaches for when a display does not come up,
		# so the port carries pages for them, written from their option
		# tables; drminfo's page rides along, since it belongs to the
		# same set of tools even though the program is this system's own.
		# Installed into the sysroot's man1 so that the repository's
		# xorg-manpages target renders them exactly like the pages that
		# did come from a source tree.
		if [ -d "$here/man" ]; then
			mkdir -p "$SYSROOT/usr/share/man/man1" || return 1
			for m in "$here/man"/*.1; do
				[ -f "$m" ] || continue
				cp -f "$m" "$SYSROOT/usr/share/man/man1/" || return 1
			done
		fi
		;;
	libXpm) echo "--disable-docs --without-xmlto --without-fop --disable-open-zfile" ;;
	libXaw) echo "--disable-specs --without-xmlto --without-fop --disable-docs" ;;
	libfontenc) echo "--disable-docs" ;;
	libXfont2) echo "--disable-docs --disable-devel-docs --without-xmlto --without-fop" ;;
	libpciaccess) echo "--disable-docs" ;;
	libxcvt) echo "" ;;
	xkeyboard-config) echo "--disable-runtime-deps --with-xkb-rules-symlink=xorg" ;;
	xkbcomp) echo "--disable-docs" ;;
	font-util) echo "--with-fontrootdir=/usr/share/fonts/X11" ;;
	font-misc-misc | font-cursor-misc)
		# Uncompressed .pcf, because libXfont2 here is built without zlib
		# ("checking for zlib... no") and cannot read a .pcf.gz.  The
		# failure that causes is not obvious from the other end: the
		# server finds the font directory, then dies with "could not
		# open default font 'fixed'" as though the file were missing.
		echo "--with-compression=no --with-fontrootdir=/usr/share/fonts/X11"
		;;
	xorg-server)
		# Two shapes of server, decided by what the sysroot holds.
		#
		# GLX is on when Mesa is in the sysroot, and only then: its
		# configure needs gl.pc, and Mesa is built by the GTK3 port,
		# which comes AFTER this one -- a fresh build in manifest order
		# gets a server without GLX, and `./build.sh xorg-server` after
		# gtk3/build.sh mesa gets one with it.  The driver path is given
		# explicitly (patches/xorg-server/0006): asked of pkg-config it
		# comes back with the host sysroot prefixed.
		#
		# The display-manager path (libdrm, DRI2, DRI3, Present, glamor,
		# the modesetting driver) is on when libdrm AND Mesa's libgbm
		# are there.  glamor is the GL-backed acceleration the
		# modesetting driver draws with, through EGL on a GBM device;
		# DRI3 hands clients a render node and takes their buffers as
		# descriptors, which needs xtrans fd passing (configure only
		# knows the systems it can assume that on, so it is stated) and
		# libxshmfence.  Without them the server is the fbdev + software
		# GLX one, which is also what /etc/X11/xorg.conf selects on a
		# machine without /dev/dri/card0 (res/xorg/xserverrc).
		if [ -f "$SYSROOT/usr/lib/pkgconfig/gl.pc" ]; then
			glx="--enable-glx --with-dri-driver-path=/usr/lib/dri"
		else
			glx="--disable-glx"
		fi
		if [ -f "$SYSROOT/usr/lib/pkgconfig/libdrm.pc" ] && \
		   [ -f "$SYSROOT/usr/lib/pkgconfig/gbm.pc" ]; then
			drm="--enable-libdrm --enable-dri2 --enable-dri3 \
			     --enable-present --enable-glamor \
			     --enable-xshmfence --enable-xtrans-send-fds"
		else
			drm="--disable-libdrm --disable-dri2 --disable-dri3 \
			     --disable-glamor"
		fi
		#
		# Also off: DRI1 (the pre-DRI2 protocol, needs the drivers
		# nothing here has), pciaccess (the library is built for its
		# header, but nothing here can reach the bus -- the modesetting
		# driver opens the node it is told in xorg.conf), vgahw (VGA
		# registers through port I/O, which userspace cannot do here),
		# int10 (real-mode BIOS calls), and the input thread (errno
		# is thread-local now, but the thread has not been revalidated).
		echo "$glx $drm --disable-dri \
		      --disable-xvfb --disable-xnest --disable-xwayland \
		      --disable-xephyr --disable-dmx --disable-docs \
		      --disable-devel-docs --disable-selective-werror \
		      --disable-systemd-logind --disable-suid-wrapper \
		      --without-dtrace --disable-config-udev --disable-config-hal \
		      --enable-xorg --with-fontrootdir=/usr/share/fonts/X11 \
		      --disable-input-thread \
		      --disable-pciaccess --disable-int10-module \
		      --with-int10=stub \
		      --with-sha1=libcrypto --with-default-xkb-rules=evdev \
		      --disable-vgahw"
		;;
	xf86-input-evdev | xf86-video-fbdev)
		# Install directories must be given as TARGET paths.  Left to
		# themselves these packages read them back with
		# `pkg-config --variable=sdkdir xorg-server`, and pkg-config --
		# correctly, for compiler flags -- prefixes every path it
		# reports with PKG_CONFIG_SYSROOT_DIR.  Adding DESTDIR on top of
		# that installs into $SYSROOT/$SYSROOT/usr/... , which succeeds
		# silently and puts the files somewhere nothing will look.
		echo "--with-sdkdir=/usr/include/xorg \
		      --with-xorg-module-dir=/usr/lib/xorg/modules"
		;;
	xinit) echo "--with-xinitdir=/etc/X11/xinit" ;;
	xauth | xsetroot | xset | xrandr | twm) echo "" ;;
	xclock)
		# Without Xft, which would mean porting fontconfig and expat
		# first -- a lot of machinery for a clock.  configure defaults
		# it to "try", but the try is fatal: PKG_CHECK_MODULES aborts
		# rather than falling back, so it has to be said explicitly.
		#
		# The analogue clock face is unaffected; only the digital mode's
		# antialiased text falls back to a core bitmap font.
		#
		# (fontconfig and expat ARE built now, for Motif; this could be
		# revisited, but it changes how the clock renders and the current
		# rendering is what is wanted.)
		#
		# --with-appdefaultdir for the same reason as xload and xcalc
		# below -- and it has to be HERE rather than in that case arm,
		# because a shell `case` takes the FIRST match and this arm would
		# otherwise shadow it.  That is exactly what happened: xclock was
		# the one app whose app-defaults still went to the doubled path
		# after the other two were fixed.
		echo "--without-xft \
		      --with-appdefaultdir=/usr/share/X11/app-defaults"
		;;
	xterm)
		echo "--disable-imake --enable-256-color --disable-desktop \
		      --with-app-defaults=/usr/share/X11/app-defaults"
		;;
	fontconfig)
		# Docs need docbook and python; --disable-docs covers both.
		#
		# The default font path is stated because fontconfig's guess is a
		# list of directories this system does not have, and a font path
		# that points nowhere means every fontconfig client falls back to
		# whatever the X core protocol offers.
		#
		# Its install step wants to run fc-cache, which is a TARGET
		# binary and could not run on the build host -- but upstream
		# already guards that with `test -z "$(DESTDIR)"` and we always
		# install with a DESTDIR, so no flag is needed for it.
		echo "--disable-docs --disable-docbook \
		      --with-default-fonts=/usr/share/fonts/X11/misc"
		;;
	libXft)
		echo "--disable-specs --without-xmlto --without-fop"
		;;
	# ---- the TLS chain, for Claws Mail ---------------------------------
	#
	# --disable-doc throughout, for the reason every X.Org package here has
	# its documentation off: the manual is Texinfo, building it needs
	# makeinfo on the BUILD host, and what it produces is a .info file that
	# nothing on the image can read -- this system's manual is a flat
	# directory of preformatted man pages.  libtasn1 is where that first
	# stopped a build, and only at `make install', after everything real had
	# already compiled.
	libtasn1) echo "--disable-doc --disable-gtk-doc" ;;
	mpfr)
		# GMP is named explicitly rather than left to the include path:
		# MPFR's configure links a probe against it, and if it resolves
		# gmp.h from the sysroot while the link picks up the BUILD host's
		# library the probe passes and the resulting library is subtly
		# the wrong one.  --disable-doc for the reason libtasn1 has it.
		echo "--disable-doc --with-gmp=$SYSROOT/usr"
		;;
	gnutls)
		# p11-kit is smart-card and PKCS#11 support: another library, a
		# module directory and a daemon, for hardware nothing here can
		# reach.  The included unistring avoids porting libunistring for
		# the handful of Unicode routines GnuTLS uses.  The tools
		# (certtool, gnutls-cli) are diagnostics for a machine with a
		# terminal and time to spend; Claws links the library.
		# The trust store is named at build time because that is the
		# only moment GnuTLS accepts one: its verification API asks
		# for "the system trust", and with no default configured that
		# answer is empty -- glib-networking (WebKit's TLS) then fails
		# every https:// certificate, and Claws can only trust servers
		# the user has clicked through.  The path is where the image
		# has always shipped its CA bundle.
		echo "--disable-doc --disable-tests --disable-tools \
		      --without-p11-kit --with-included-unistring \
		      --without-tpm --without-tpm2 --disable-libdane \
		      --disable-guile \
		      --with-default-trust-store-file=/etc/ssl/certs/ca-certificates.crt"
		;;
	enchant)
		# hunspell is the backend, and the only one: aspell, nuspell,
		# hspell, voikko and zemberek are not ported, and Enchant tries
		# every provider it was built with before giving up.  Claws asks
		# for an en_US speller the moment a compose window opens and
		# treats a refusal as an error, so "no backend" is not a quiet
		# degradation -- it is a dialog box every time.
		echo "--enable-hunspell --disable-aspell --disable-nuspell \
		      --disable-applespell --disable-hspell --disable-voikko \
		      --disable-zemberek"
		;;
	hunspell)
		# The tools (hunspell, munch, unmunch, hzip) are not staged, but
		# their build is cheap and turning it off is not an option the
		# package offers.  ncurses and readline are, and neither is
		# wanted for a library nothing types at interactively.
		echo "--without-ui --disable-static"
		;;
	libetpan)
		# The mail protocols.  OpenSSL is already on the image and is
		# what libetpan uses for its own TLS; GnuTLS is what Claws uses
		# for its.  Both are built, so both are enabled and the two
		# halves of the application agree about which connections are
		# encrypted.
		echo "--with-openssl --with-gnutls --without-sasl \
		      --disable-db --disable-dependency-tracking"
		;;
	libwebp)
		# The demux component is what WebKit asks pkg-config for; mux
		# costs nothing beside it.  Everything else is the tools'
		# optional image I/O -- the LIBRARY needs none of it, and each
		# would drag a host-side dependency probe into the build.
		echo "--enable-libwebpdemux --enable-libwebpmux \
		      --disable-gl --disable-sdl --disable-png --disable-jpeg \
		      --disable-tiff --disable-gif --disable-wic"
		;;
	libgpg-error)
		echo "--disable-doc --disable-tests --disable-nls \
		      --enable-install-gpg-error-config"
		;;
	libgcrypt)
		# --disable-asm: the assembler modules probe the platform by
		# OS name and this system is on nobody's list; the C fallbacks
		# are what every port here would get anyway, and WebCrypto is
		# not a throughput path.
		#
		# --enable-random=getentropy names the one entropy gatherer
		# this system has.  Auto-detection builds the whole museum --
		# including the legacy device-polling module, which includes
		# <sys/syscall.h> and stops the build on a header this libc
		# does not carry.  getentropy(3) is in this libc precisely for
		# this consumer.
		echo "--disable-doc --disable-asm \
		      --enable-random=getentropy \
		      --with-libgpg-error-prefix=$SYSROOT/usr"
		;;
	nghttp2)
		# Only libnghttp2: the bundled tools (nghttpx, h2load) are
		# C++23 programs nothing here ships, and --enable-lib-only is
		# upstream's switch for exactly this case.
		echo "--enable-lib-only --disable-examples"
		;;
	libxslt)
		echo "--without-python --without-debug --without-profiler \
		      --with-crypto"
		;;
	libxml2)
		# GtkSourceView reads its syntax definitions with the tree and
		# reader APIs; nothing here wants the rest.  Python needs an
		# interpreter on the target, lzma a compression library that is
		# not ported, and modules the dynamic-extension machinery for
		# XSLT plugins.  zlib IS ported, so compressed documents work.
		echo "--without-python --without-lzma --without-modules \
		      --with-zlib --without-debug"
		;;
	libfm-extra | libfm)
		# The same tarball twice; the difference is the first option.
		#
		# --with-extra-only builds libfm-extra alone -- the XML parser
		# and string helpers menu-cache links -- and nothing that needs
		# menu-cache, which is what breaks the circle between them.
		# See the manifest.
		#
		# Off in both halves:
		#   udisks   a system service reached over a session bus, for
		#            mounting removable media.  Neither exists here, and
		#            GIO's own mount monitoring covers what does.
		#   exif     libexif, for reading a JPEG's embedded thumbnail
		#            instead of scaling the image.  Not ported; the
		#            thumbnails are produced through gdk-pixbuf either
		#            way, just without that shortcut.
		#   gtk-doc  the reference manual, as everywhere else.
		#   demo     libfm's own toy file manager, which exists to
		#            exercise the library.  PCManFM is the real one.
		#   old-actions
		#            the SUPERSEDED implementation of custom file-menu
		#            actions, written in Vala.  The feature is not lost
		#            with it: 1.4 carries a C implementation
		#            (src/base/fm-action.c) that is built
		#            unconditionally, and the menu module that shows
		#            those actions uses that one.  What the option
		#            controls is whether the old Vala copy is compiled
		#            as well -- and a tag export ships the .vala
		#            sources without the generated C a release tarball
		#            would have, so keeping it would mean a Vala
		#            compiler in the build-host requirements for code
		#            that duplicates code already built.
		opts="--disable-udisks --disable-exif --disable-gtk-doc \
		      --disable-demo --disable-old-actions"
		case "$1" in
		libfm-extra) echo "$opts --with-extra-only" ;;
		*) echo "$opts --with-gtk=3" ;;
		esac
		;;
	pcmanfm)
		# GTK3, to match the libfm-gtk3 built above: libfm installs one
		# library per toolkit version and pcmanfm has to ask for the
		# same one, or its configure finds no libfm-gtk at all.
		echo "--with-gtk=3"
		;;
	gtk+)
		# GTK 2, for HexChat.  (GTK 3 is the meson package `gtk'; this
		# is the one whose tarball is still called gtk+.)
		#
		# X11 and nothing else: the other gdktargets are quartz, win32
		# and directfb.
		#
		# Off: Xinerama, because libXinerama is not in the X port and a
		# single-head desktop is what this is; CUPS and PAPI, which are
		# printing systems reached over a network; libjasper, a JPEG2000
		# decoder with a long history of its own; and gtk-doc, whose
		# toolchain produces nothing shipped here.
		#
		# --disable-rebuilds is what keeps a cross build honest.  GTK 2
		# regenerates several sources with tools it has just built --
		# gdk-pixbuf-csource over the stock icons, the marshaller
		# tables -- and those binaries are compiled for the TARGET, so
		# running them here fails.  The tarball ships the generated
		# copies for exactly this case; the option says to use them.
		#
		# --enable-explicit-deps=no keeps the .pc file to the libraries
		# a caller actually links.  With it on, gtk+-2.0.pc lists every
		# transitive X library by absolute build-host path.
		echo "--with-gdktarget=x11 --disable-xinerama \
		      --disable-cups --disable-papi --without-libjasper \
		      --disable-gtk-doc --disable-gtk-doc-html \
		      --disable-glibtest --disable-introspection \
		      --disable-rebuilds --enable-explicit-deps=no \
		      --disable-static"
		;;
	claws-mail)
		# Off: everything needing a desktop session bus, a system
		# service or a toolchain this system does not have.  --disable-svg
		# in particular keeps librsvg -- and with it a Rust toolchain --
		# out of the dependency graph, for the same reason the Adwaita
		# icon theme is pinned to its last PNG release.
		# --enable-fancy-plugin is stated rather than left to its
		# auto-detection: the plugin is the HTML mail viewer and the
		# reason claws-mail sits BELOW webkitgtk in the manifest, and
		# an auto probe that quietly found nothing would ship a mail
		# client that renders HTML as a wall of tags with no error
		# anywhere.  Stated, a missing WebKit stops the build at
		# configure, which is where a broken order should surface.
		echo "--disable-dbus --disable-gnome --disable-libnotify \
		      --disable-gpgme --disable-compface --disable-ldap \
		      --disable-jpilot --disable-networkmanager-support \
		      --disable-svg --disable-valgrind --disable-manual \
		      --enable-gnutls --enable-enchant --enable-libetpan \
		      --enable-fancy-plugin"
		;;
	gettext)
		# The bindings for languages this system has no runtime for, and
		# the pieces of the tools that would otherwise be configured.
		# --without-emacs: the Lisp mode is for editing .po files on the
		# build host.  --disable-openmp: it would link the host's libgomp
		# and with it the host libc, which then collides with ours over
		# errno -- the same reason pixman has it off.
		#
		# NOT --disable-nls, which is the obvious-looking flag and is
		# exactly wrong here.  For an ordinary package it means "do not
		# translate this program"; for gettext itself it means "do not
		# build libintl", and the build then completes, reports success
		# and installs nothing -- USE_NLS=no leaves libintl.la as a
		# noinst convenience library that never reaches the sysroot.
		# The next package to want -lintl is where that surfaces.
		echo "--disable-java --disable-csharp --disable-d \
		      --disable-modula2 --disable-openmp --disable-curses \
		      --disable-rpath --without-emacs --disable-acl"
		;;
	motif)
		# --disable-printing avoids libXp, the old X print extension:
		# Motif only needs it for print-to-Xp, and porting a dead
		# extension for that is not worth it.
		#
		# Xft the other way round: Motif defaults it OFF, and with
		# libXft built there is no reason for its widgets to draw with
		# core bitmap fonts.
		echo "--disable-printing --enable-xft"
		;;
	xload | xcalc)
		# These ask pkg-config for libXt's appdefaultdir and install to
		# $(DESTDIR)$(appdefaultdir).  pkg-config prefixes
		# PKG_CONFIG_SYSROOT_DIR when reporting a .pc VARIABLE, so the
		# answer already contains the sysroot and DESTDIR then doubles
		# it: the files landed in <sysroot>/<sysroot>/usr/share/X11/
		# app-defaults and never reached the image.
		#
		# Nothing complained, because an app-defaults file that is not
		# there is not an error -- the program starts with its built-in
		# defaults.  For xcalc that means no buttons at all, since its
		# entire keypad is defined in resources.
		#
		# Stating the path removes the guess.  xterm above does the same
		# thing, which is why xterm's were the only ones installed
		# correctly.
		echo "--with-appdefaultdir=/usr/share/X11/app-defaults"
		;;
	*) echo "" ;;
	esac
}

# Options for the NATIVE half of a needs_host_build() package.  Everything the
# generators do not need is switched off: this build exists to produce five
# small programs, not a second copy of the library to develop against.
meson_host_opts() {
	case "$1" in
	glib)
		echo "-Dtests=false -Dinstalled_tests=false -Dnls=disabled \
		      -Dman-pages=disabled -Dintrospection=disabled \
		      -Dselinux=disabled -Dlibmount=disabled -Ddtrace=false \
		      -Dsystemtap=false -Dsysprof=disabled -Dglib_debug=disabled \
		      -Ddefault_library=static"
		;;
	*) echo "" ;;
	esac
}

# Per-package environment for the configure run.
#
# Almost always empty.  This is for the case where a package decides something
# about the platform from a hardcoded list of operating-system names rather than
# by testing for it -- a list this system will never be on.  Autoconf caches
# such answers in a gt_cv_/ac_cv_ variable, and a cache variable already set in
# the environment is taken as given and the test skipped, so the answer can be
# supplied without patching a generated configure script.
#
# Only for answers that are TRUE here.  Setting one of these to defeat a test
# that is telling the truth would hide a real gap rather than fill it.
pkg_env() {
	# Answers that hold for EVERY package, because they are facts about this
	# system rather than about any one of them.  A cache variable a package
	# does not use is simply ignored, so stating them once is safe and keeps
	# the next gnulib-bearing package from failing the same way.
	#
	# gt_cv_locale_fake: gnulib calls a locale_t "fake" when it is a token
	# rather than a description -- when newlocale() cannot produce distinct
	# locales and the object carries no per-category names.  That is exactly
	# what this libc has: one locale, one singleton object, and newlocale()
	# returns it whatever it is asked for.
	#
	# gnulib decides this from a list of OS names (OpenBSD, Android) and
	# answers "no" for anything it does not recognise, which then selects
	# code that reads names out of the locale object.  There are none to
	# read, so the build stops at "#error Please port gnulib
	# getlocalename_l-unsafe.c to your platform".  Saying yes selects
	# gnulib's own name-tracking path, written for precisely this situation.
	# gettext hit it first; GnuTLS, carrying its own copy of gnulib, hit the
	# identical wall three packages later.
	common="gt_cv_locale_fake=yes"

	case "$1" in
	startup-notification)
		# "Does realloc(NULL, n) behave as malloc(n)?"  Answered by
		# RUNNING a test program, which a cross build cannot do, and
		# with no cached fallback -- so configure stops rather than
		# guessing.
		#
		# It does: realloc() in this libc opens with
		# `if (!oldmem) return malloc(bytes);`, which is what C89
		# required and every standard since has repeated.
		echo "$common lf_cv_sane_realloc=yes"
		;;
	*) echo "$common" ;;
	esac
}

# Per-package meson options, the counterpart of pkg_opts() above.
meson_opts() {
	case "$1" in
	glu)
		# The default provider is glvnd; this system's GL is Mesa's
		# classic libGL, whose pkg-config name is plain "gl".
		echo "-Dgl_provider=gl"
		;;
	mesa)
		# GL for the desktop.  svga is the driver for the VMware SVGA3D
		# device behind /dev/dri (the kernel's vmwgfx interface);
		# llvmpipe is the renderer wherever there is no such device
		# (and the fallback the loader picks when the device has no 3D),
		# softpipe the no-JIT fallback a debug session can force with
		# GALLIUM_DRIVER=softpipe.  system 'likeos' counts as a KMS/DRM
		# system (patches/mesa/0003), so libdrm, DRI3 and GBM are in.
		# The llvm dependency is answered by toolchain/llvm-config (a
		# wrapper; the sysroot's real llvm-config is a target binary the
		# host cannot run).
		echo "-Dgallium-drivers=svga,llvmpipe,softpipe -Dvulkan-drivers=[] \
		      -Dplatforms=x11 -Dglx=dri -Degl=enabled -Dgbm=enabled \
		      -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
		      -Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=true \
		      -Dglvnd=disabled -Dzstd=disabled -Dlmsensors=disabled \
		      -Dlibunwind=disabled -Dvalgrind=disabled \
		      -Dbuild-tests=false -Dtools=[] -Dvideo-codecs=[]"
		;;
	pixman)
		# OpenMP drags in the host's libgomp, and with it the host libc,
		# which then collides with ours over errno.  The tests are the
		# only thing that used it.
		echo "-Dopenmp=disabled -Dtests=disabled -Ddemos=disabled \
		      -Dgtk=disabled -Dlibpng=disabled"
		;;
	libxkbcommon)
		echo "-Denable-docs=false -Denable-wayland=false \
		      -Denable-xkbregistry=false -Denable-tools=false \
		      -Denable-bash-completion=false"
		;;
	libpciaccess) echo "-Dzlib=disabled" ;;
	# libxshmfence: futexes through this libc (patches/libxshmfence), the
	# fence files in /dev/shm.
	libxshmfence) echo "--enable-futex --with-shared-memory-dir=/dev/shm" ;;
	xkeyboard-config) echo "-Dcompat-rules=true -Dxorg-rules-symlinks=true" ;;
	# libdrm: only the driver this kernel has (vmwgfx); the test programs
	# (modetest, vbltest, proptest) are installed as the display driver's
	# diagnostics; no udev here.
	libdrm) echo "-Dvmwgfx=enabled -Dintel=disabled -Dradeon=disabled \
		      -Damdgpu=disabled -Dnouveau=disabled -Domap=disabled \
		      -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled \
		      -Detnaviv=disabled -Dudev=false -Dvalgrind=disabled \
		      -Dcairo-tests=disabled -Dman-pages=disabled \
		      -Dtests=true -Dinstall-test-programs=true" ;;

	# ---- the GTK3 stack ------------------------------------------------
	#
	# Two themes run through all of these.  Tests and benchmarks are off
	# everywhere: they are built for the TARGET and so cannot be run here,
	# and they reach for interfaces the libraries themselves do not (it was
	# HarfBuzz's GPU test, not HarfBuzz, that first wanted std::nan).
	# Introspection and documentation are off for the same reason
	# throughout the X port -- both need a toolchain whose output nothing
	# on the image consumes.
	glib)
		echo "-Dlibmount=disabled -Dselinux=disabled -Dxattr=false \
		      -Dintrospection=disabled -Ddtrace=false -Dsystemtap=false \
		      -Dsysprof=disabled -Dtests=false -Dinstalled_tests=false \
		      -Dman-pages=disabled -Dglib_debug=disabled -Dnls=enabled"
		;;
	fribidi) echo "-Ddocs=false -Dbin=false -Dtests=false" ;;
	harfbuzz)
		# cairo is disabled deliberately: HarfBuzz uses it only for its
		# own view/trace utilities, and enabling it would make HarfBuzz
		# and Cairo mutually dependent -- Cairo wants HarfBuzz for
		# cairo-ft.  The utilities go with it, which is no loss; nothing
		# on the image runs hb-view.
		# icu=enabled, for WebKit: its cmake requires HarfBuzz WITH
		# the ICU integration (find_package(HarfBuzz REQUIRED
		# COMPONENTS ICU)), which is a separate small library
		# (libharfbuzz-icu) built only when ICU is in the sysroot --
		# and ICU is, because the manifest builds it above this.
		echo "-Dglib=enabled -Dgobject=enabled -Dfreetype=enabled \
		      -Dcairo=disabled -Dicu=enabled -Dchafa=disabled \
		      -Dtests=disabled -Ddocs=disabled -Dbenchmark=disabled \
		      -Dutilities=disabled -Dintrospection=disabled"
		;;
	cairo)
		# symbol-lookup wants libbfd, which is a build-host debugging
		# convenience; tee and the platform backends are for systems
		# this is not.
		echo "-Dxlib=enabled -Dxcb=enabled -Dfreetype=enabled \
		      -Dfontconfig=enabled -Dpng=enabled -Dzlib=enabled \
		      -Dglib=enabled -Dtests=disabled -Dgtk_doc=false \
		      -Dspectre=disabled -Dsymbol-lookup=disabled \
		      -Dquartz=disabled -Dtee=disabled -Dxlib-xcb=disabled"
		;;
	pango)
		echo "-Dintrospection=disabled -Dgtk_doc=false \
		      -Dbuild-testsuite=false -Dcairo=enabled \
		      -Dfreetype=enabled -Dfontconfig=enabled \
		      -Dlibthai=disabled -Dsysprof=disabled"
		;;
	gdk-pixbuf)
		# builtin_loaders=all is the important one: it compiles the
		# image loaders INTO the library, so no loaders.cache has to be
		# produced by running a target binary -- neither at build time,
		# where it could not run, nor on first boot, where it would need
		# a writable cache directory and a program to write it.
		#
		# legacy_xpm and others BOTH default to disabled, and a mail
		# client built around XPM icons needs both.  Claws Mail compiles
		# its icon set in as XPM data arrays and builds every one of
		# them through gdk_pixbuf_new_from_xpm_data(), which is served
		# by the `legacy-xpm' loader and nothing else -- without it the
		# program starts with no icons at all, several hundred
		# "Image type \"legacy-xpm\" is not supported" warnings, and a
		# NULL pixbuf handed to every GTK call that expected one.
		# `others' covers the loaders for .xpm and .xbm FILES (and pnm,
		# tga, icns, qtif), which are what a theme or a user-supplied
		# icon arrives as.
		echo "-Dbuiltin_loaders=all -Dpng=enabled -Djpeg=enabled \
		      -Dtiff=enabled -Dlegacy_xpm=enabled -Dothers=enabled \
		      -Dintrospection=disabled -Dman=false \
		      -Dtests=false -Dinstalled_tests=false \
		      -Dgio_sniffing=false -Ddocumentation=false"
		;;
	atk) echo "-Dintrospection=false -Ddocs=false" ;;
	libepoxy)
		# GLX stays ON even though there is no OpenGL on this system.
		#
		# It is tempting to switch it off, and wrong: -Dglx=no omits
		# epoxy/glx.h and the GLX entry points altogether, and GTK's X11
		# backend includes that header unconditionally
		# (gdkglcontext-x11.h) -- so GTK then does not compile at all.
		#
		# Leaving it on costs nothing at runtime.  Epoxy is a DISPATCH
		# library: it resolves GL entry points lazily through dlopen, so
		# a build with GLX support does not require libGL to exist.
		# Where there is none, epoxy_has_glx() answers no, GDK reports
		# that it cannot create a GL context, and GTK draws through
		# cairo -- which is what happens here.
		#
		# EGL is ON for the same reason GLX is, and it was previously
		# off on a wrong premise: that EGL is only the Wayland
		# backend's business.  WebKitGTK needs <epoxy/egl.h> --
		# PlatformDisplay.cpp and the six platform/graphics/egl/*.cpp
		# files include it unconditionally, find_package(Epoxy) is
		# REQUIRED in OptionsGTK.cmake, and SourcesGTK.txt lists those
		# sources behind no feature flag.  Without this the engine does
		# not compile at all.
		#
		# It costs nothing to enable and needs no EGL to exist:
		#   - meson.build asks for `dependency('egl', required: false)`,
		#     so no EGL package has to be found;
		#   - the entry points are generated from the registry/egl.xml
		#     that epoxy SHIPS, so no Khronos headers are needed;
		#   - <epoxy/egl.h> is self-contained -- it pulls in only
		#     epoxy/common.h and the generated header, and it #defines
		#     __egl_h_ / __eglext_h_ specifically to keep the system's
		#     <EGL/egl.h> OUT.
		#
		# At run time epoxy dlopens libEGL.so.1 lazily, exactly as it
		# does libGL for GLX.  Nothing here calls an EGL entry point:
		# the display is created only by
		# initializePlatformDisplayIfNeeded(), whose two call sites are
		# DrawingAreaCoordinatedGraphics (guarded by
		# acceleratedCompositingEnabled, which is false because
		# ENABLE_WEBGL=OFF turns canUseHardwareAcceleration off) and
		# WebChromeClient (inside ENABLE(WEBGL), not compiled).  The
		# WebProcessGLib call sites are inside PLATFORM(WPE).
		#
		# If that ever stops holding, the symptom is loud and specific:
		# "Couldn't open libEGL.so.1" from epoxy, or WebKit's "Could
		# not create default EGL display ... Aborting".  Either means
		# real EGL is needed and Mesa swrast is the next step -- a stub
		# libEGL does NOT help, because PlatformDisplayDefault::create()
		# turns a null display into CRASH().
		echo "-Dglx=yes -Degl=yes -Dx11=true -Dtests=false -Ddocs=false"
		;;
	gtksourceview)
		# The editing widget.  gir and vapi generate bindings for
		# language runtimes this system does not have, and both need a
		# generator that would have to RUN here while being built for
		# the target.  The Glade catalog describes the widget to an
		# interface designer, which is a development-machine program.
		echo "-Dgir=false -Dvapi=false -Dgtk_doc=false \
		      -Dinstall_tests=false -Dglade_catalog=false"
		;;
	mousepad)
		# GtkSourceView 4 explicitly rather than by detection: the
		# fallback is version 3, which is not built here, and `auto`
		# would silently take it if the 4 lookup ever failed.
		#
		# Off: polkit (a privileged helper reached over a system bus,
		# for editing root-owned files), gspell (a second spell-checking
		# stack beside the Enchant one already built, and one Mousepad
		# only uses through a plugin), the shortcuts editor (it needs
		# libxfce4ui, i.e. the rest of the Xfce desktop) and the test
		# plugin, which is for Mousepad's own developers.
		#
		# keyfile-settings is ON, and it is what makes the program
		# remember anything.  GSettings normally writes through dconf,
		# which is a daemon reached over a session bus -- neither is
		# ported -- and with no dconf GLib falls back to the memory
		# backend: every setting works for the length of one run and is
		# gone at the next start, with one warning on stderr that
		# nobody running under X ever sees.  This option is upstream's
		# answer for exactly that situation: the settings go to
		# ~/.config/Mousepad/settings.conf through GLib's keyfile
		# backend instead.
		echo "-Dgtksourceview4=enabled -Dpolkit=disabled \
		      -Dgspell-plugin=disabled -Dshortcuts-plugin=disabled \
		      -Dtest-plugin=disabled -Dkeyfile-settings=true"
		;;
	dbus)
		# The session bus.
		#
		# On: the daemon itself, the command-line tools (dbus-send,
		# dbus-monitor, dbus-launch, dbus-uuidgen -- each of which is
		# how a bus problem is diagnosed from a shell), traditional
		# activation (starting a service from its .service file, which
		# is exactly how xfconfd is reached), and X11 autolaunch, so
		# dbus-launch can publish the bus address on the root window
		# for anything started later in the session.
		#
		# Off: the system bus's init integration and every access
		# control framework -- systemd units, launchd, SELinux,
		# AppArmor and libaudit have nothing to attach to here.  epoll
		# and inotify likewise: this kernel has neither, and dbus falls
		# back to poll() and to not watching its configuration for
		# changes, both of which are correct, just less efficient.
		# The documentation needs Doxygen and ducktype; the test suites
		# are for dbus's own developers.
		#
		# The session socket goes in /tmp, which is where the default
		# address `unix:tmpdir=/tmp' looks for it.
		echo "-Dmessage_bus=true -Dtools=true \
		      -Dtraditional_activation=true -Dx11_autolaunch=enabled \
		      -Dsystemd=disabled -Dlaunchd=disabled \
		      -Dselinux=disabled -Dapparmor=disabled \
		      -Dlibaudit=disabled -Depoll=disabled -Dinotify=disabled \
		      -Ddoxygen_docs=disabled -Dducktype_docs=disabled \
		      -Dqt_help=disabled -Dxml_docs=disabled \
		      -Dmodular_tests=disabled -Dintrusive_tests=false \
		      -Dinstalled_tests=false -Dstats=false \
		      -Dsession_socket_dir=/tmp \
		      -Drelocation=disabled"
		;;
	vte)
		# The terminal widget, for xfce4-terminal.
		#
		# GTK 3 only.  The same tree builds a GTK 4 widget by default
		# and there is no GTK 4 here; asking for it produces a
		# configure failure rather than a skipped target.
		#
		# On, and each one is a feature the terminal would visibly
		# lose: a11y is the accessible text interface, fribidi is
		# bidirectional text (already built for Pango), and terminfo
		# installs the xterm-256color description VTE sets TERM to --
		# without it every curses program inside the terminal starts by
		# failing to find its terminal type.
		#
		# Off: the GTK 4 widget; `app`, which is VTE's own demo
		# terminal and not something to ship beside a real one; gir and
		# vapi, which generate bindings for language runtimes this
		# system does not have; glade, a catalog describing the widget
		# to an interface designer; docs, needing gi-docgen; ICU, which
		# is only used to decode the legacy non-UTF-8 encodings and is
		# a very large dependency for that; and systemd, for scope
		# registration on a system with no service manager.
		#
		# gnutls is ON, and it is not optional in any useful sense.
		# VTE does not keep the scrollback in memory -- it writes it to
		# a file in /tmp and encrypts it on the way out, and GnuTLS is
		# the cipher.  Built without it, every line that scrolls off
		# the top of a terminal is left in cleartext in a temporary
		# file, and VTE says so by printing
		#
		#   WARNING: GnuTLS not enabled; data will be written to disk
		#   unencrypted!
		#
		# into the terminal at startup.  GnuTLS is already in this
		# sysroot for Claws Mail, so it costs nothing to link.
		echo "-Dgtk3=true -Dgtk4=false -Da11y=true -Dfribidi=true \
		      -Dterminfo=true -Dapp=false -Dgir=false -Dvapi=false \
		      -Dglade=false -Ddocs=false -Dicu=false \
		      -D_systemd=false -Dgnutls=true"
		;;
	hexchat)
		# The IRC client.  Everything that can be built here is on.
		#
		# TLS is OpenSSL, which the X.Org port already imports into this
		# sysroot, and it is what makes the client usable at all: the
		# networks it ships in its server list are TLS-only now.  The
		# checksum and fishlim plugins are C and need nothing else, so
		# they are built.
		#
		# Off, and each for a missing runtime rather than a choice:
		# the Lua, Perl and Python plugins are bindings to language
		# runtimes that are not ported, so there is nothing for them to
		# bind to; sysinfo reads the PCI ID database through pciutils,
		# which is a different library from the libpciaccess the X
		# server uses; libcanberra plays sound through an audio stack
		# this system does not have; and the D-Bus interface is
		# single-instance signalling and remote scripting, which needs
		# dbus-glib -- the old bindings, not GDBus.
		#
		# The exec, upd and winamp plugins are Windows-only and their
		# options exist so a Windows build can turn them on.
		echo "-Dgtk-frontend=true -Dtext-frontend=false \
		      -Dtheme-manager=false -Dtls=enabled -Dplugin=true \
		      -Ddbus=disabled -Dlibcanberra=disabled \
		      -Dwith-checksum=true -Dwith-fishlim=true \
		      -Dwith-lua=false -Dwith-perl=false -Dwith-python=false \
		      -Dwith-sysinfo=false -Dinstall-appdata=true"
		;;
	gtk)
		echo "-Dx11_backend=true -Dwayland_backend=false \
		      -Dbroadway_backend=false -Dprint_backends=file \
		      -Dintrospection=false -Dgtk_doc=false -Dman=true \
		      -Ddemos=false -Dexamples=false -Dtests=false \
		      -Dinstalled_tests=false -Dcolord=no \
		      -Dcloudproviders=false -Dprofiler=false -Dtracker3=false"
		;;

	# ---- the WebKit chain ----------------------------------------------
	libpsl)
		# IDNA through the ICU built at the head of this manifest --
		# the alternative runtime is libidn2, which is not ported.
		echo "-Druntime=libicu -Dtests=false -Ddocs=false"
		;;
	libsoup)
		# tls_check=false because the check RUNS a target program to
		# see whether GIO has a TLS backend; the backend is
		# glib-networking, built right after this, and the check
		# cannot run under cross-compilation anyway.
		echo "-Dtests=false -Ddocs=disabled -Dintrospection=disabled \
		      -Dvapi=disabled -Dsysprof=disabled -Dtls_check=false \
		      -Dbrotli=enabled -Dntlm=disabled -Dgssapi=disabled \
		      -Dautobahn=disabled -Dpkcs11_tests=disabled"
		;;
	glib-networking)
		# The GnuTLS backend and nothing else.  This is the TLS that
		# every GIO user -- WebKitGTK/libsoup (luakit, MiniBrowser),
		# anything speaking https through GLib -- runs on.  GnuTLS is
		# upstream's default and the backend libsoup is developed and
		# tested against; glib-networking's own meson_options.txt keeps
		# OpenSSL off and says so ("General-purpose Linux distros
		# should leave it disabled").  The port ran the OpenSSL backend
		# between 2026-08-27 and 2026-09-05 and is back on GnuTLS.
		# Exactly ONE backend may be installed: both modules register
		# the same GTlsBackend extension point at equal priority and
		# GIO would pick whichever it loaded first -- so a switch must
		# also delete the other module from the sysroot, because
		# `meson install' only adds, and stage.sh copies every .so it
		# finds in /usr/lib/gio/modules.  GnuTLS is already in the
		# manifest ahead of this (over GMP, nettle and libtasn1);
		# Claws Mail links it directly.  libproxy and the desktop
		# proxy portal are services this system does not run.
		echo "-Dgnutls=enabled -Dopenssl=disabled -Dlibproxy=disabled \
		      -Dgnome_proxy=disabled -Dinstalled_tests=false"
		;;
	*) echo "" ;;
	esac
}

# Per-package CMake options.
cmake_opts() {
	case "$1" in
	libjpeg-turbo)
		# The version-8 API and soname: that is what everything here
		# links against (see packages.list).  The TurboJPEG wrapper is
		# a second, simpler API that nothing here uses, and the static
		# library has no consumer either.  SIMD stays on -- it is the
		# whole point of this library and the build host has the
		# assembler for it.
		echo "-DWITH_JPEG8=ON -DENABLE_STATIC=OFF -DENABLE_SHARED=ON \
		      -DWITH_TURBOJPEG=OFF -DWITH_SIMD=ON -DWITH_JAVA=OFF \
		      -DCMAKE_INSTALL_LIBDIR=/usr/lib"
		;;
	ctwm)
		# Off: the m4 preprocessor for config files (needs m4 on the
		# target), session management (no session manager here), and
		# rplay (a sound daemon that does not exist).  The XPM and
		# regex support stay on -- both libraries are built here.
		# Off: m4 (the config-file preprocessor would have to exist on
		# the TARGET), XSMP session management (no session manager
		# here), rplay (a network sound daemon), and libjpeg (not
		# ported -- it is only used for JPEG desktop backgrounds).
		#
		# On: XPM, which ctwm needs for icons and which is built here;
		# EWMH, so the window manager advertises itself the way modern
		# clients expect; and the libc regex, which is a real POSIX
		# implementation here rather than the bundled copy.
		#
		# ETCDIR is where ctwm looks for system.ctwmrc, and it defaults
		# to ${prefix}/etc -- /usr/etc, which is not a directory this
		# system has.  Missing that file is not an error to ctwm: it
		# falls back to a configuration COMPILED INTO the binary, so it
		# starts and works and silently ignores everything in ours.
		#
		# MANDIR is pinned for the same reason the font packages pin
		# their directories: left alone, ctwm does find_file(MANDIR man
		# ...), which searches the SYSROOT and returns an absolute path
		# into it -- and DESTDIR then prefixes that a second time, so
		# the page lands in $SYSROOT/$SYSROOT/usr/share/man.  Install
		# directories have to be given as target paths.
		echo "-DUSE_M4=OFF -DUSE_SESSION=OFF -DUSE_RPLAY=OFF \
		      -DUSE_JPEG=OFF -DUSE_XPM=ON -DUSE_EWMH=ON \
		      -DUSE_SREGEX=ON -DUSE_XRANDR=ON \
		      -DMANDIR=/usr/share/man \
		      -DETCDIR=/etc/X11/ctwm"
		;;
	fmt)
		# The C++ formatting library, for VTE.  Shared, because VTE is
		# not the only thing that will want it and a static libfmt
		# would be copied into every user of it.
		#
		# FMT_TEST builds a test suite that downloads and builds
		# googletest; FMT_DOC needs Doxygen and Python.  Neither
		# produces anything shipped.
		echo "-DBUILD_SHARED_LIBS=ON -DFMT_TEST=OFF -DFMT_DOC=OFF \
		      -DFMT_INSTALL=ON"
		;;
	brotli)
		echo "-DBUILD_SHARED_LIBS=ON -DBROTLI_DISABLE_TESTS=ON"
		;;
	woff2)
		# NO_CXX_FLAGS keeps woff2's hand-rolled -fno-exceptions set
		# from fighting the wrapper's; brotli is found through the
		# sysroot like everything else.
		echo "-DBUILD_SHARED_LIBS=ON -DWOFF2_BUILD_NO_CXX_FLAGS=ON"
		;;
	simdutf)
		# UTF-8 validation and transcoding, for VTE.
		#
		# The benchmarks pull in several comparison libraries and the
		# tools are development programs.  Neither is shipped, and the
		# benchmarks do not cross-compile: they RUN during the build to
		# choose an implementation.
		echo "-DBUILD_SHARED_LIBS=ON -DSIMDUTF_TESTS=OFF \
		      -DSIMDUTF_BENCHMARKS=OFF -DSIMDUTF_TOOLS=OFF \
		      -DSIMDUTF_CXX_STANDARD=17"
		;;
	*) echo "" ;;
	esac
}

# Some packages build a demo or test program we do not want and cannot switch
# off through configure.  Naming the subdirectories to build skips them without
# patching the package.
make_dirs() {
	case "$1" in
	# sxpm is a viewer demo; only the library is wanted, and its link drags
	# in host libraries through the toolkit.
	# include/ has the public header, src/ the library; only sxpm (the
	# viewer demo) is skipped.
	libXpm) echo "-C include -C src" ;;
	# libtasn1: the library, not the tools and not the tests.
	#
	# `SUBDIRS += fuzz tests' is unconditional -- there is no configure flag
	# for it -- and tests/ generates its fixtures by RUNNING asn1Parser,
	# which has just been cross-compiled.  It fails as "cannot execute:
	# required file not found", which names the program rather than the
	# reason: what is missing is this system's dynamic loader, on the build
	# host, where it was never going to be.
	#
	# lib/ alone is what GnuTLS links, and it installs libtasn1.pc with it.
	# src/ is the three command-line ASN.1 tools, which nothing here runs.
	libtasn1) echo "-C lib" ;;
	# gettext: libintl and nothing else.
	#
	# The tarball is four projects.  gettext-runtime holds libintl -- the
	# library every package above links for its translations, and the only
	# part of this that belongs on the target.  gettext-tools (msgfmt,
	# msgmerge, xgettext) are BUILD-host programs: they turn .po files into
	# .mo at build time, the build host already has them, and a copy
	# compiled for the target could not be run by the thing that needs it.
	# libtextstyle exists only to make those tools' output colourful.
	gettext) echo "-C gettext-runtime" ;;
	# Motif: the library and its headers, not the development toolchain.
	#
	# lib/ is libXm and libMrm, include/ the public headers -- that is
	# everything a Motif PROGRAM links against, and all xnedit asks for
	# (-lXm).  Skipped:
	#   tools/    the UIL compiler and its table generators.  These are HOST
	#             programs the build runs to generate source, and unlike
	#             makestrs they link against fontconfig/freetype/Xft, so
	#             building them for the host would mean host copies of the
	#             whole font stack.  UIL compiles .uil interface
	#             descriptions at DEVELOPMENT time; nothing on this system
	#             does that.
	#   clients/  mwm and friends -- there is already a window manager.
	#   doc/      manual sources, and man1 pages come from the sysroot.
	#   bindings/localized/  virtual-key and message-catalogue data for
	#             hardware and locales this system does not have.
	motif) echo "-C lib -C include" ;;
	*) echo "" ;;
	esac
}

# Build and install, either the whole tree or just the named subdirectories.
# Anything that has to exist in the sysroot BEFORE a package installs into it.
#
# The counterpart of post_install(), and needed for the same kind of reason: a
# package's own install rules can assume a directory that only exists because
# something was installed there, and with a feature switched off nothing is.
pre_install() {
	case "$1" in
	enchant)
		# providers/Makefile ends its install with
		# `cd $(pkglibdir) && rm -f *.a', tidying away static copies of
		# the spell-checking backends.  Creating the directory first
		# keeps that cd from failing after the library itself has been
		# installed correctly -- it used to fail outright, when no
		# backend was built and nothing ever created the directory.
		#
		# It is also where Enchant looks for provider modules at run
		# time, so it has to exist either way.
		mkdir -p "$SYSROOT/usr/lib/enchant-2" || return 1
		;;
	esac
	return 0
}

build_subdirs() {
	dirs=$(make_dirs "$1" | sed 's/-C //g')
	pre_install "$1" || return 1
	if [ -z "$dirs" ]; then
		make -j"$(nproc)" && make install DESTDIR="$SYSROOT"
		return $?
	fi
	for d in $dirs; do
		make -C "$d" -j"$(nproc)" || return 1
		make -C "$d" install DESTDIR="$SYSROOT" || return 1
	done
	return 0
}

# Anything a restricted build (see make_dirs) would otherwise leave behind.
post_install() {
	case "$1" in
	mesa)
		# Mesa's libEGL/libGL are real now.  The marker tells
		# import-egl-headers.sh to stop installing the stub libEGL the
		# port carried while there was no GL at all -- reinstalling the
		# stub over Mesa would put every GL program back on the
		# "Couldn't open libEGL" path the stub existed to silence.
		touch "$SYSROOT/usr/lib/.mesa-egl" || return 1
		;;
	glib-networking)
		# Only ONE GIO TLS module may end up on the image.  Both
		# backends register the GTlsBackend extension point at the same
		# priority, so with two present GIO picks whichever it loaded
		# first -- and stage.sh copies every .so out of this directory.
		# `meson install' only ever adds files, so the module left over
		# from the other backend has to go explicitly; without this,
		# flipping -Dgnutls/-Dopenssl leaves the old one behind and the
		# backend actually used is decided by readdir order.  Deleting
		# the one we did not build keeps the choice in meson_opts().
		if [ -d "$SYSROOT/usr/lib/gio/modules" ]; then
			case "$(meson_opts glib-networking)" in
			*-Dgnutls=enabled*) other=libgioopenssl.so ;;
			*-Dopenssl=enabled*) other=libgiognutls.so ;;
			*) other= ;;
			esac
			if [ -n "$other" ]; then
				rm -f "$SYSROOT/usr/lib/gio/modules/$other" \
				      "$SYSROOT/usr/lib/gio/modules/${other%.so}.la"
			fi
		fi
		;;
	libXpm)
		# The .pc file is generated at the top level, so building only
		# src/ installs the library but not the description of it — and
		# the next package's configure then cannot find xpm.
		make install-pkgconfigDATA DESTDIR="$SYSROOT" || return 1
		;;
	mousepad)
		# Mousepad publishes no manual page at all -- upstream documents
		# the program through its Help menu -- so the port carries one,
		# written from the program's own option table.  Installed into
		# the sysroot's man1 here so that the repository's gtk3-manpages
		# target renders it exactly like the pages that did come from a
		# source tree, rather than needing a case of its own.  This is
		# what the xnedit arm below does with pod2man, one step shorter.
		if [ -f "$here/gtk3/man/mousepad.1" ]; then
			mkdir -p "$SYSROOT/usr/share/man/man1" || return 1
			cp -f "$here/gtk3/man/mousepad.1" \
				"$SYSROOT/usr/share/man/man1/mousepad.1" || return 1
		fi
		;;
	xnedit)
		# Its own install target ships only the binaries, an icon and a
		# .desktop file -- no manual page, because the page is generated
		# from POD by a target upstream keeps out of the default build
		# ("users may not have Perl installed").
		#
		# Generated here, into the sysroot's man1, so the repository's
		# xorg-manpages target renders it alongside every other page
		# instead of this one being a special case.  pod2man runs on the
		# BUILD host, which is where it belongs -- it is documentation,
		# not code.
		if command -v pod2man >/dev/null 2>&1; then
			mkdir -p "$SYSROOT/usr/share/man/man1"
			for pg in xnedit xnc; do
				[ -f "doc/$pg.pod" ] || continue
				pod2man --section=1 --center="User Commands" \
					--name="$(echo "$pg" | tr a-z A-Z)" \
					"doc/$pg.pod" \
					>"$SYSROOT/usr/share/man/man1/$pg.1" ||
					return 1
			done
		fi
		;;
	esac
	return 0
}

# Which build system a package uses is DETECTED, not listed.  Upstream moves
# packages from autotools to meson between releases, so a hand-kept list goes
# stale silently — and the failure it produces ("./configure: not found") does
# not obviously say why.
is_meson() {
	[ -f "$2/meson.build" ] && [ ! -f "$2/configure" ] && [ ! -f "$2/autogen.sh" ]
}

# ...with one exception, by name: a package that ships BOTH and whose meson
# build is the one upstream maintains.  is_meson() answers "autotools" for all
# of these, because a generated `configure` is present and it cannot know that
# the autotools side is the deprecated one.  GTK's in particular is a stub that
# no longer builds several of its own subdirectories.
prefers_meson() {
	case "$1" in
	gtk | harfbuzz | fribidi) return 0 ;;
	esac
	return 1
}

is_cmake() {
	[ -f "$2/CMakeLists.txt" ] && [ ! -f "$2/meson.build" ] &&
		[ ! -f "$2/configure" ] && [ ! -f "$2/autogen.sh" ]
}

# Packages whose build produces programs that packages ABOVE them have to RUN.
#
# A cross-built generator is a target binary and cannot execute here.  GLib is
# the only such package left -- glib-compile-resources, glib-genmarshal,
# glib-mkenums, glib-compile-schemas and gdbus-codegen generate source for
# nearly everything above it, and GTK's build alone invokes the first of those
# hundreds of times.
#
# So GLib is built TWICE from one tree: natively into $HOSTTOOLS for the
# generators, then cross for the library.  Both from the same tarball, which is
# what keeps the generator and the library in step -- the build host's own GLib
# is a different version, and on this machine is not installed at all.
#
# One manifest line and one stamp: the two builds are two halves of porting
# GLib, not two packages, and nothing else can use half of it.
needs_host_build() {
	case "$1" in
	glib) return 0 ;;
	esac
	return 1
}

# Packages that build nothing at all: a tarball of finished files, where
# "installing" means copying the ones that are wanted into the sysroot.
#
# By name, because there is nothing to detect: an absent configure script means
# a data package here and a git export needing autoreconf three lines below.
is_dataonly() {
	case "$1" in
	dejavu-fonts-ttf) return 0 ;;
	liberation-fonts-ttf) return 0 ;;
	NotoSans) return 0 ;;
	hunspell-en_US) return 0 ;;
	shared-mime-info) return 0 ;;
	esac
	return 1
}

# Packages built for the BUILD machine, into $HOSTTOOLS rather than the
# sysroot: tools the packages ABOVE them run during their own build.
#
# What comes out must never be a candidate for the image -- it
# is compiled for this machine -- so the prefix is $HOSTTOOLS and the cross
# wrappers are not used at all.
is_hosttool() {
	case "$1" in
	intltool) return 0 ;;
	esac
	return 1
}

# What such a package contributes.  Run with the source tree as the working
# directory; $SYSROOT is where it goes.
install_data() {
	case "$1" in
	dejavu-fonts-ttf)
		# Twelve faces of the twenty-two shipped: the regular, bold,
		# oblique and bold-oblique of each of the three families a user
		# interface asks for by generic name -- sans-serif, monospace
		# and serif.
		#
		# The rest are left out deliberately.  Condensed and ExtraLight
		# are design variants nothing here selects, and MathTeXGyre is
		# for typesetting mathematics; carrying all of them would put
		# 9.8MB on the image to make 4MB of difference to what can be
		# displayed.
		mkdir -p "$SYSROOT/usr/share/fonts/truetype/dejavu" || return 1
		for face in DejaVuSans DejaVuSansMono DejaVuSerif; do
			for style in "" -Bold -Oblique -BoldOblique; do
				f="ttf/$face$style.ttf"
				[ -f "$f" ] || continue
				cp -f "$f" \
					"$SYSROOT/usr/share/fonts/truetype/dejavu/" ||
					return 1
			done
		done
		;;
	liberation-fonts-ttf)
		# All twelve faces: the regular, bold, italic and bold-italic of
		# Sans, Serif and Mono.  Unlike DejaVu there is nothing here to
		# leave out -- each one is the metric substitute for a face the
		# web asks for by name (Arial, Times New Roman, Courier New),
		# and a missing italic is a page that falls back to a synthetic
		# slant with different widths, which is the problem this font
		# was added to solve.
		mkdir -p "$SYSROOT/usr/share/fonts/truetype/liberation" || return 1
		for f in *.ttf; do
			[ -f "$f" ] || continue
			cp -f "$f" \
				"$SYSROOT/usr/share/fonts/truetype/liberation/" ||
				return 1
		done
		;;
	NotoSans)
		# Four faces of the several hundred in the archive: regular,
		# bold, italic and bold-italic at normal width.  The condensed
		# and semi-condensed widths and the nine weights are variable-
		# font source material that no CSS here selects, and the OTF
		# copies duplicate the TTFs.
		#
		# HINTED, because everything else on this image is: the display
		# is a virtual framebuffer at 96dpi where unhinted small text
		# smears.  The nested NotoSans/ level is the zip's own layout.
		mkdir -p "$SYSROOT/usr/share/fonts/truetype/noto" || return 1
		for face in Regular Bold Italic BoldItalic; do
			f="NotoSans/hinted/ttf/NotoSans-$face.ttf"
			[ -f "$f" ] || continue
			cp -f "$f" \
				"$SYSROOT/usr/share/fonts/truetype/noto/" ||
				return 1
		done
		;;
	shared-mime-info)
		# The MIME database, which is data and stays data: nothing in
		# this package is compiled for the target.
		#
		# What ships is the XML source and the binary form the readers
		# actually use.  Two programs read it here, both through the
		# same code: GLib's g_content_type_guess(), which is how libfm
		# decides an icon, a description and a default application, and
		# GTK's own file chooser.  Neither has a built-in table --
		# without this database every file is application/octet-stream.
		#
		# The binary form is produced by update-mime-database, which
		# scans the XML and writes mime.cache, globs2, magic and the
		# rest beside it.  Run from the BUILD host: the output is
		# ordinary data with no architecture to it (the format fixes its
		# own byte order), and a copy compiled for the target could not
		# be run by the thing that needs it, exactly as with
		# glib-compile-schemas.
		#
		# The untranslated XML is used deliberately.  Upstream merges
		# 90-odd languages of <comment> into it with itstool; this image
		# has one locale, and the merge would add 6MB of comments no
		# code path can select.
		mkdir -p "$SYSROOT/usr/share/mime/packages" || return 1
		cp -f data/freedesktop.org.xml.in \
			"$SYSROOT/usr/share/mime/packages/freedesktop.org.xml" ||
			return 1

		if ! command -v update-mime-database >/dev/null 2>&1; then
			echo "install_data: no update-mime-database on the build host." >&2
			echo "  It writes the binary form every reader of this" >&2
			echo "  database uses; without it the XML alone does" >&2
			echo "  nothing.  Install it with:  make deps" >&2
			return 1
		fi
		# Exits non-zero on any malformed entry, which is what makes it
		# worth checking: a half-written cache reads as an empty one.
		update-mime-database "$SYSROOT/usr/share/mime" || return 1
		[ -s "$SYSROOT/usr/share/mime/mime.cache" ] || {
			echo "install_data: mime.cache was not produced" >&2
			return 1
		}
		;;
	hunspell-en_US)
		# /usr/share/hunspell is not a choice: Enchant's hunspell
		# provider builds its search list from g_get_system_data_dirs()
		# with the provider's own name appended, so it looks in
		# <datadir>/hunspell and nowhere else.  A dictionary installed
		# anywhere more imaginative is a dictionary hunspell never finds.
		#
		# The pair is what a dictionary IS: the .dic holds the words and
		# the .aff the affix rules that generate the rest of the forms
		# from them.  Either one alone is useless.
		mkdir -p "$SYSROOT/usr/share/hunspell" || return 1
		for f in en_US.aff en_US.dic; do
			[ -f "$f" ] || {
				echo "install_data: $f missing" >&2
				return 1
			}
			cp -f "$f" "$SYSROOT/usr/share/hunspell/" || return 1
		done

		# The matching rules that come with the fonts: the 57-* files
		# make DejaVu what the generic family names resolve to, which is
		# what a toolkit asking for "Sans" ends up with, and the 20-*
		# ones turn hinting off at the sizes where it hurts.
		#
		# Into conf.d rather than conf.avail: that split exists so a
		# package manager can enable a rule by symlinking it, and there
		# is no package manager here -- a rule in conf.avail alone would
		# simply never take effect.
		mkdir -p "$SYSROOT/etc/fonts/conf.d" || return 1
		for c in fontconfig/*.conf; do
			[ -f "$c" ] || continue
			cp -f "$c" "$SYSROOT/etc/fonts/conf.d/" || return 1
		done
		;;
	esac
	return 0
}

# Packages whose tree must NOT be distcleaned between builds, because their
# sources are generated and the generator is not installed here.
#
# Enchant is written in Vala.  Its tarball ships both the .vala originals and
# the .c files valac produced from them, exactly so that building it does not
# require valac -- and `make distclean' deletes the .c files, because upstream
# assumes anyone running it has the compiler to regenerate them.  The result is
# a tree that built once and cannot build again: "cc1: fatal error: ./pwl.c: No
# such file or directory", naming a file that was there an hour ago.
#
# (This also corrects an assumption in the port's plan, which had Enchant down
# as a C++ package needing the STL.  It is C++ only in the sense that valac
# emits C; nothing in it uses the standard C++ library.)
keeps_generated_sources() {
	case "$1" in
	enchant) return 0 ;;
	esac
	return 1
}

# Packages with no configure at all: a hand-written Makefile per platform, built
# by naming the platform as a target.  There is nothing to detect from the tree
# (a Makefile is present in almost every package), so this is by name.
is_plainmake() {
	case "$1" in
	xnedit | lz4) return 0 ;;
	esac
	return 1
}

# The make target a plain-Makefile package is built with, and the variables it
# needs on the command line.
#
# On the command line rather than in the environment: a make variable set that
# way overrides any assignment inside the Makefile, and is inherited by every
# sub-make.  Set in the environment instead, the package's own `CC = gcc' wins
# and the whole tree is built for the build host.
plainmake_args() {
	case "$1" in
	xnedit)
		# The platform target creates the Makefile.<platform> symlinks
		# and then builds util/, Xlt/, Microline/ and source/ in order.
		echo "likeos CC=$here/toolchain/likeos-cc \
		      PKG_CONFIG=$here/toolchain/likeos-pkg-config"
		;;
	lz4)
		# Only the library: the default target also builds the lz4,
		# lz4c and unlz4 programs, and nothing here is a compression
		# tool -- VTE links liblz4 to compress its scrollback and that
		# is the whole of why this is in the tree.
		#
		# BUILD_STATIC=no because everything here links dynamically,
		# and a static archive doubles the build for nobody.
		#
		# PREFIX on the BUILD line as well as the install one, which is
		# not redundant: liblz4.pc is generated during the build, from
		# whatever PREFIX is set then.  Given only at install time, the
		# library lands in /usr/lib and its .pc says prefix=/usr/local
		# -- so every later package is told to look for the headers in
		# a directory that does not exist, and VTE stopped on exactly
		# that.
		echo "-C lib PREFIX=/usr CC=$here/toolchain/likeos-cc \
		      BUILD_STATIC=no"
		;;
	esac
}

# ...and the arguments its `make install' needs, beyond DESTDIR and PREFIX.
plainmake_install_args() {
	case "$1" in
	lz4) echo "-C lib BUILD_STATIC=no" ;;
	esac
}


# Packages that compile nothing and so need no cross toolchain: building them
# with the host compiler avoids a pointless cross-configure.
is_noarch() {
	case "$1" in
	util-macros | xorgproto | xtrans | xcb-proto)
		return 0
		;;
	esac
	return 1
}

# Turn absolute symlinks in the staging tree into equivalent relative ones.
#
# A package that installs a symlink to an absolute target is describing where
# the file will be ON THE TARGET, so inside the staging tree it dangles -- and
# a dangling symlink in a path component makes `mkdir -p` fail outright.  That
# is how xkeyboard-config's /usr/share/X11/xkb link broke xorg-server's install
# rather than its own, which is why this runs BEFORE each build as well as
# after: the package that leaves the link is never the one that trips over it.
#
# The relative form resolves correctly in both places, so nothing is lost: the
# link still points where it should once the tree is copied onto the image.
relativise_sysroot_symlinks() {
	find "$SYSROOT" -type l | while read -r link; do
		target=$(readlink "$link")
		case "$target" in
		/*) ;;
		*) continue ;;
		esac
		# Only links that point INSIDE the staging tree.  A link to a
		# file elsewhere on the build host is naming a real path on this
		# machine, and rewriting it relative to the sysroot root turns it
		# into a path that does not exist -- which is how the alias for
		# the system's curses came to dangle, and -lncurses then found
		# the build host's copy instead.
		case "$target" in
		"$SYSROOT"/*) continue ;;
		esac
		[ -e "$SYSROOT$target" ] || continue
		# How deep the link sits below the sysroot decides how many
		# levels to climb before following the target.
		reldir=${link#"$SYSROOT"/}
		reldir=$(dirname "$reldir")
		up=""
		IFS=/
		for _ in $reldir; do up="../$up"; done
		unset IFS
		ln -sfn "$up${target#/}" "$link"
	done
}

build_one() {
	name=$1
	dir=$2
	log="$logs/$name.log"

	printf '%-22s ' "$name"
	relativise_sysroot_symlinks

	# meson install builds only what installation needs.  Running
	# `meson compile` first would also build the test programs, which
	# several packages link against host-only helpers — failing the build
	# over binaries that are never shipped.
	if [ "$name" = webkitgtk ]; then
		# The engine.  CMake with ninja, the port's cross toolchain
		# file, and both tool sources on PATH: the port's native GLib
		# tools (glib-compile-resources runs hundreds of times) and
		# the port's own ruby, which the JavaScriptCore generators are
		# written in.
		#
		# The option block reads long because WebKit defaults to a
		# desktop this is not.  The shape of the port:
		#   - the GTK3 API line (webkit2gtk-4.1), which is what both
		#     consumers (luakit, Claws fancy) link;
		#   - Skia rendering (USE_SKIA=ON): the engine's own renderer,
		#     with its GPU backend on the GL that Mesa's svga driver
		#     now provides through the kernel's display manager.  Where
		#     there is no GPU it falls back to its CPU backend and
		#     draws in software, so the same build is correct on both
		#     machines;
		#   - the GPU process, GBM and libdrm: rendered frames are
		#     handed between processes as buffer descriptors rather
		#     than copied through shared memory, which is what
		#     webkit://gpu calls the DMABuf renderer;
		#   - the JavaScriptCore JIT, all four tiers, with WebAssembly
		#     and the sampling profiler.  It needs an executable
		#     mapping, a real ucontext_t in the signal frame and
		#     XSAVE-preserved vector state across a context switch --
		#     all of which this kernel now has (see the WTF patches,
		#     which is where OS(LIKEOS) is declared);
		#   - system malloc: bmalloc's virtual-memory gymnastics
		#     (gigacage reservations in the terabytes) assume address
		#     space this kernel does not hand out;
		#   - the media stack OFF entirely (USE_GSTREAMER,
		#     ENABLE_VIDEO, ENABLE_WEB_AUDIO and the rest below).
		#     There is no audio driver in this kernel, so nothing a
		#     decoder produced could ever be heard, and the engine
		#     composites video frames itself rather than through a
		#     GStreamer sink -- so the framework, its plugin sets and
		#     the codec libraries under them bought nothing but build
		#     time and image size.  They were removed from the port
		#     with this switch;
		#   - the supervisor extras still off: journald wants a
		#     logging daemon, bubblewrap a sandbox built on namespaces
		#     and seccomp, neither of which exists here.
		#
		# Accessibility (USE_ATSPI) is LEFT ON, upstream's setting, and
		# the "Could NOT find ATSPI" line in the configure output is
		# expected -- do not "fix" it by making USE_ATSPI conditional
		# on that find, which is where this port spent a build.  The
		# GTK port has no working accessibility-off configuration:
		# AXCoreObject declares m_wrapper only for Cocoa, Windows,
		# PlayStation/Haiku and ATSPI, while wrapper(), setWrapper()
		# and detachWrapper() are unguarded, so WebCore does not
		# compile without one of them -- and a dozen AXObjectCache
		# platform hooks live only in the atspi sources.
		#
		# Leaving it on costs nothing, because nothing in the build
		# actually consumes the library: ATSPI_LIBRARIES and
		# ATSPI_INCLUDE_DIRS are referenced only inside
		# Source/cmake/FindATSPI.cmake, and no source includes
		# <atspi/...>.  WebKit speaks the AT-SPI D-Bus protocol
		# directly over GDBus, from an interface description it
		# generates with gdbus-codegen (the port's host GLib tools
		# provide it, and PATH below finds them).
		#
		# At run time it is dormant: AccessibilityAtspi::connect()
		# returns immediately when the bus address is empty, which is
		# what it is with no accessibility bus on the session -- the
		# same state a conventional desktop is in when a11y is off.
		# MiniBrowser is kept: it is the reference client, and the
		# first thing to run when a page misbehaves in luakit.
		#
		# The last four options are all about fitting the build on a
		# machine rather than about the engine:
		#   -O2 rather than the Release default of -O3.  The peak
		#     memory of a WebCore unified compile is dominated by the
		#     optimiser, -O3 buys inlining decisions that matter to a
		#     JIT-less interpreter build very little, and it is what
		#     the distributions ship WebKit as.
		#   --no-keep-memory tells GNU ld to re-read input sections
		#     instead of holding every one in memory.  libWebKit.so
		#     is linked from something over half a gigabyte of
		#     objects; without it that link alone is the high-water
		#     mark of the entire build.
		#   the link job pool holds link steps to one at a time, so
		#     the linker never has to share the machine with another
		#     copy of itself.
		#
		# The job count comes from MEMORY, not from the core count.
		#
		# WebCore's unified sources are the largest translation units
		# in this whole tree -- each bundles eight .cpp files behind a
		# header graph reaching most of the engine -- and a compiler
		# on one of them peaks past two gigabytes.  A job count taken
		# from nproc ignores that entirely: eight compilers on a box
		# with eight gigabytes do not get a clean OOM kill, they get a
		# swap storm, and the machine stops answering long before the
		# kernel picks a victim.  This port lost a machine that way
		# twice.
		#
		# So the count is MemTotal divided by three gigabytes per job,
		# clamped to at least one and never above the core count --
		# two jobs on the 8-core/7.9 GB machine this was ported on,
		# which is what fits there alongside a desktop session.
		# LIKEOS_WEBKIT_JOBS overrides it when the arithmetic is known
		# to be wrong for a particular machine.
		#
		# A memory-capped cgroup is worth having either way, and is
		# the only thing that keeps a misjudged count from taking the
		# session down with it:
		#
		#   systemd-run --user --scope -p MemoryHigh=4000M \
		#       -p MemoryMax=5000M ./gtk3/build.sh webkitgtk
		#
		# Note what it does and does not do.  MemoryHigh throttles and
		# reclaims INSIDE the build, which protects the desktop -- but
		# only if the cap leaves the desktop room.  Set too close to
		# total memory it simply pins the cgroup at its limit while
		# the host swaps anyway, which is what 5500M did here.
		(
			cd "$dir" || exit 1
			PATH="$here/toolchain:$HOSTTOOLS/bin:$PATH"
			export PATH
			if ! command -v ruby >/dev/null 2>&1; then
				echo "webkitgtk: no ruby on the build host." >&2
				echo "  JavaScriptCore's generators are ruby;" >&2
				echo "  install it with:  make deps" >&2
				exit 1
			fi
			# unifdef, which the header generation runs ON THE BUILD
			# HOST.  The tree bundles its source but would compile it
			# with the cross compiler -- a binary this machine cannot
			# execute -- so the port compiles that same source
			# natively into .hosttools, where configure finds it
			# on PATH.
			if ! command -v unifdef >/dev/null 2>&1; then
				cc -O2 -std=gnu99 -D_XOPEN_SOURCE=700 \
					-o "$HOSTTOOLS/bin/unifdef" \
					Source/ThirdParty/unifdef/unifdef.c ||
					exit 1
			fi
			# MemTotal is in kB; 3 GB per job.  Clamped to at
			# least one, and never more than there are cores.
			memkb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
			jobs=$((memkb / (3 * 1024 * 1024)))
			[ "$jobs" -lt 1 ] && jobs=1
			[ "$jobs" -gt "$(nproc)" ] && jobs=$(nproc)
			[ -n "${LIKEOS_WEBKIT_JOBS:-}" ] &&
				jobs=$LIKEOS_WEBKIT_JOBS
			echo "webkitgtk: $jobs compile job(s)" >&2

			# Configure only when there is nothing to continue.
			#
			# Every other package here is rebuilt from scratch on a
			# re-run, which costs seconds and buys certainty.  This
			# one is hours, and an interrupted build -- a hang, a
			# reboot, a Ctrl-C -- would otherwise start again at
			# object one.  ninja already decides what is stale from
			# the graph it wrote, so a build directory that carries
			# a finished configure is worth continuing.  `-f`
			# means "build this again from nothing" and so
			# reconfigures, which is also how a changed option
			# block above takes effect.
			[ "$force" = "1" ] && rm -rf .likeos-build
			if [ ! -f .likeos-build/build.ninja ]; then
			rm -rf .likeos-build
			LIKEOS_TOOLCHAIN="$here/toolchain" \
			cmake -S . -B .likeos-build -G Ninja \
				-DCMAKE_TOOLCHAIN_FILE="$here/toolchain/likeos-toolchain.cmake" \
				-DCMAKE_INSTALL_PREFIX=/usr \
				-DCMAKE_INSTALL_LIBDIR=lib \
				-DCMAKE_BUILD_TYPE=Release \
				-DPORT=GTK -DUSE_GTK4=OFF \
				-DENABLE_X11_TARGET=ON \
				-DENABLE_WAYLAND_TARGET=OFF \
				-DENABLE_QUARTZ_TARGET=OFF \
				-DUSE_SKIA=ON \
				-DENABLE_JIT=ON -DENABLE_C_LOOP=OFF \
				-DENABLE_DFG_JIT=ON -DENABLE_FTL_JIT=ON \
				-DENABLE_WEBASSEMBLY=ON \
				-DENABLE_SAMPLING_PROFILER=ON \
				-DUSE_SYSTEM_MALLOC=ON \
				-DENABLE_MINIBROWSER=ON \
				-DENABLE_INTROSPECTION=OFF \
				-DENABLE_DOCUMENTATION=OFF \
				-DENABLE_JOURNALD_LOG=OFF \
				-DENABLE_BUBBLEWRAP_SANDBOX=OFF \
				-DUSE_GBM=ON -DUSE_LIBDRM=ON \
				-DENABLE_GPU_PROCESS=ON \
				-DUSE_GSTREAMER=OFF -DENABLE_VIDEO=OFF \
				-DUSE_GSTREAMER_GL=OFF \
				-DENABLE_MEDIA_SOURCE=OFF \
				-DENABLE_WEB_AUDIO=OFF \
				-DENABLE_MEDIA_STREAM=OFF \
				-DENABLE_MEDIA_RECORDER=OFF \
				-DENABLE_WEB_CODECS=OFF \
				-DENABLE_MEDIA_SESSION=OFF \
				-DENABLE_WEB_RTC=OFF \
				-DENABLE_ENCRYPTED_MEDIA=OFF \
				-DENABLE_SPEECH_SYNTHESIS=OFF \
				-DUSE_FLITE=OFF -DUSE_SPIEL=OFF \
				-DENABLE_GAMEPAD=OFF -DENABLE_WEBGL=ON \
				-DENABLE_WEBDRIVER=OFF -DENABLE_WEBXR=OFF \
				-DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LCMS=OFF \
				-DUSE_LIBHYPHEN=OFF -DUSE_LIBSECRET=OFF \
				-DUSE_LIBBACKTRACE=OFF \
				-DUSE_SYSPROF_CAPTURE=OFF \
				-DENABLE_SPELLCHECK=ON -DENABLE_PDFJS=ON \
				-DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
				-DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
				-DCMAKE_SHARED_LINKER_FLAGS="-Wl,--no-keep-memory" \
				-DCMAKE_JOB_POOLS="link=1" \
				-DCMAKE_JOB_POOL_LINK=link || exit 1
			fi

			cmake --build .likeos-build -j"$jobs" &&
			DESTDIR="$SYSROOT" cmake --install .likeos-build &&
			post_install "$name"
		) >"$log" 2>&1
	elif is_cmake "$name" "$dir"; then
		(
			cd "$dir" || exit 1
			rm -rf .likeos-build
			LIKEOS_TOOLCHAIN="$here/toolchain" \
				PATH="$here/toolchain:$PATH" \
				cmake -S . -B .likeos-build \
				-DCMAKE_TOOLCHAIN_FILE="$here/toolchain/likeos-toolchain.cmake" \
				-DCMAKE_INSTALL_PREFIX=/usr \
				-DCMAKE_BUILD_TYPE=Release \
				$(cmake_opts "$name") &&
				cmake --build .likeos-build -j"$(nproc)" &&
				DESTDIR="$SYSROOT" cmake --install .likeos-build &&
				post_install "$name"
		) >"$log" 2>&1
	elif [ "$name" = gcc ]; then
		# Not GCC: only libstdc++-v3 out of its tree, which is an
		# autotools project of its own and configures standalone.
		#
		# The compiler stays the build host's.  What is needed here is
		# the LIBRARY built for this system -- against this libc, with
		# this libc's headers -- and the host g++ produces that
		# perfectly well through the wrappers.  Building a whole cross
		# GCC would produce a second compiler that generates the same
		# code as the one already installed.
		#
		# --enable-clocale=generic matters more than it looks.  The
		# glibc locale model wants the entire *_l family (strtod_l,
		# isalpha_l, per-category newlocale ...), which this libc does
		# not have; `generic' uses the C locale only and drops the
		# dependency.  The counterpart for the ctype table is a patch to
		# configure.host, since that choice has no configure flag.
		#
		# The host triple names this system in the VENDOR field --
		# x86_64-likeos-gnu -- for two reasons.  It differs from the
		# build triple, so configure treats this as a cross build and
		# never tries to run a test program; and libstdc++'s
		# crossconfig.m4 has no arm for an unknown system, so the OS
		# field has to be one it recognises.
		(
			cd "$dir" || exit 1
			rm -rf .likeos-build
			mkdir -p .likeos-build

			# The PREVIOUS libstdc++'s installed headers, out of
			# the way before this one is built against them.
			#
			# likeos-c++ puts $SYSROOT/usr/include/c++/<ver> on the
			# include path whenever it exists, which is right for
			# every other package and wrong for this one: the build
			# tree has its own copy of the same headers, and two
			# copies of a header that guards itself is one copy too
			# many.  libstdc++ ships compatibility wrappers -- its
			# fenv.h, complex.h and several more -- that exist only
			# to `#include_next` the C library's header of that
			# name.  Reached twice, the second one is silenced by
			# its own include guard, the #include_next never
			# happens, and the C header is never read at all.  That
			# is how a rebuild meant to pick up a newly added
			# <fenv.h> failed on the very names it was adding.
			#
			# Safe to delete: this build compiles against its own
			# source tree and the headers it generates (the comment
			# in likeos-c++ says as much), and `make install' below
			# puts a complete set back.
			rm -rf "$SYSROOT/usr/include/c++"

			cd .likeos-build || exit 1

			cfg="../libstdc++-v3/configure"
			args="--host=x86_64-likeos-gnu
			      --build=$(../config.guess)
			      --prefix=/usr
			      --disable-multilib --disable-nls
			      --disable-libstdcxx-pch
			      --enable-clocale=generic"

			# Configure runs TWICE, and the second run is not
			# belt-and-braces.
			#
			# The gthreads probe compiles a program that includes
			# gthr-default.h -- the threading layer, generated from
			# libgcc/gthr-posix.h by config.status at the END of a
			# configure run.  On a first run it does not exist yet,
			# the probe fails to compile, and libstdc++ concludes the
			# system has no threads: no std::mutex, no std::thread,
			# and tzdb.cc then fails to build because it uses them.
			#
			# In a full GCC build the top level supplies that header
			# from the libgcc build directory, which is configured
			# first.  Standalone there is nothing to supply it, so
			# the first run generates it and the second is told where
			# it is.  Both levels are needed: bits/ so the probe's
			# own #include "gthr.h" finds the generated one, and its
			# parent so that file's #include <bits/gthr-default.h>
			# resolves.
			CC="$here/toolchain/likeos-cc" \
			CXX="$here/toolchain/likeos-c++" \
			AR=ar RANLIB=ranlib \
				$cfg $args >/dev/null || exit 1

			g="$PWD/include/x86_64-likeos-gnu"
			[ -f "$g/bits/gthr-default.h" ] || {
				echo "no gthr-default.h after configure" >&2
				exit 1
			}
			CC="$here/toolchain/likeos-cc" \
			CXX="$here/toolchain/likeos-c++" \
			AR=ar RANLIB=ranlib \
			CXXFLAGS="-O2 -I$g/bits -I$g" \
				$cfg $args || exit 1

			grep -q '^#define _GLIBCXX_HAS_GTHREADS' config.h || {
				echo "gthreads still off; std::mutex would be missing" >&2
				exit 1
			}

			make -j"$(nproc)" &&
				make install DESTDIR="$SYSROOT" ||
				exit 1

			# libatomic, from the same tree and for the same reason
			# libstdc++ is here: it is the piece of the C++ runtime
			# that carries the 16-byte atomic operations
			# (__atomic_load_16 and friends).  GCC outlines those
			# into calls, WebKit's lock-free machinery does 128-bit
			# compare-and-swap throughout, and its configure stops
			# outright when neither the builtins nor -latomic can
			# satisfy them.  A standalone autotools project like
			# libstdc++-v3, and a far simpler one.
			cd "$dir" || exit 1
			rm -rf .likeos-atomic
			mkdir -p .likeos-atomic
			cd .likeos-atomic || exit 1
			CC="$here/toolchain/likeos-cc" \
				../libatomic/configure \
				--host=x86_64-likeos-gnu \
				--build="$(../config.guess)" \
				--prefix=/usr --disable-multilib \
				>/dev/null &&
				make -j"$(nproc)" &&
				make install DESTDIR="$SYSROOT" &&
				post_install "$name"
		) >"$log" 2>&1
	elif [ "$name" = sqlite-autoconf ]; then
		# SQLite's amalgamation moved from autoconf to autosetup, which
		# looks like a configure script and is not one: it REFUSES any
		# option it does not know (--cache-file was the first casualty),
		# so likeos-autogen.sh's arrangement cannot drive it.  What it
		# does share with autoconf is config.sub validation, under its
		# own name in autosetup/ -- replaced here exactly as the autogen
		# script replaces the autoconf copies.  The shared-object flags
		# need no help: autosetup's defaults are the ELF/GNU ones and
		# the platform switch only overrides them for systems this is
		# not.
		#
		# --soname=legacy is NOT optional here, whatever the name
		# suggests.  SQLite defaults to --soname=none on purpose
		# (autosetup/sqlite-config.tcl: "this project has no direct use
		# for soname"), and a shared library with no DT_SONAME poisons
		# everything that links it: with no soname to record, the
		# linker writes into DT_NEEDED whatever string it used to find
		# the file.  For anything located by absolute path -- which is
		# how CMake and meson pass libraries -- that is the BUILD HOST's
		# path, baked into a target binary:
		#
		#   libwebkit2gtk-4.1.so.0: NEEDED /home/.../xorg-sysroot/usr/lib/libsqlite3.so
		#
		# On the target that path does not exist, the library cannot be
		# loaded, and every symbol in it comes out undefined -- which is
		# how luakit came to start with a hundred lines of "undefined
		# symbol: webkit_*" and then jump to a null slot.  libsoup was
		# hit the same way.  Consumers found through -lsqlite3 instead
		# record the bare "libsqlite3.so", the unversioned development
		# symlink, which is not staged onto the image either.
		#
		# "legacy" is upstream's name for libsqlite3.so.0, the soname
		# every distribution ships and the one the sysroot's own
		# symlink already points at.
		(
			cd "$dir" || exit 1
			[ -f Makefile ] && make distclean >/dev/null 2>&1
			cp -f "$here/toolchain/config.sub" \
				autosetup/autosetup-config.sub
			cp -f "$here/toolchain/config.guess" \
				autosetup/autosetup-config.guess
			CC="$here/toolchain/likeos-cc" \
			CXX="$here/toolchain/likeos-c++" \
			./configure --host=x86_64-unknown-likeos \
				--build="$(gcc -dumpmachine)" \
				--prefix=/usr --libdir=/usr/lib \
				--soname=legacy \
				--disable-static-shell &&
			make -j"$(nproc)" &&
			make install DESTDIR="$SYSROOT" &&
			post_install "$name"
		) >"$log" 2>&1
	elif [ "$name" = icu ]; then
		# ICU builds twice from one tree, exactly like GLib: the cross
		# build packages its data library by RUNNING genrb/pkgdata, so
		# a native copy of the tools is built first and named with
		# --with-cross-build.  Its configure is autoconf but lives in
		# source/, and the platform makefile fragment is chosen from a
		# fixed list of host_os names -- an autoconf cache variable, so
		# the answer is supplied rather than the list patched: mh-linux
		# is upstream's fragment for a GNU toolchain producing ELF
		# shared objects, which is exactly what the wrapper drives.
		(
			cd "$dir" || exit 1
			if [ ! -x .likeos-host/bin/pkgdata ]; then
				rm -rf .likeos-host
				mkdir -p .likeos-host
				( cd .likeos-host &&
				  ../source/configure --prefix="$HOSTTOOLS" \
					--disable-samples --disable-tests \
					--enable-static --disable-shared \
					>/dev/null &&
				  make -j"$(nproc)" ) || exit 1
			fi

			# The tree's config.sub predates this system; the
			# toolchain's copy knows the triple.  likeos-autogen.sh
			# does the same for the ordinary autotools packages.
			cp -f "$here/toolchain/config.sub" source/config.sub
			cp -f "$here/toolchain/config.guess" source/config.guess

			rm -rf .likeos-build
			mkdir -p .likeos-build
			cd .likeos-build || exit 1
			CC="$here/toolchain/likeos-cc" \
			CXX="$here/toolchain/likeos-c++" \
			AR=ar RANLIB=ranlib \
			icu_cv_host_frag=mh-linux \
			../source/configure \
				--host=x86_64-unknown-likeos \
				--build="$(gcc -dumpmachine)" \
				--prefix=/usr --libdir=/usr/lib \
				--with-cross-build="$dir/.likeos-host" \
				--disable-samples --disable-tests \
				--disable-extras --disable-icuio \
				--disable-layoutex \
				--enable-shared --disable-static &&
			make -j"$(nproc)" &&
			make install DESTDIR="$SYSROOT" &&
			post_install "$name"
		) >"$log" 2>&1
	elif [ "$name" = lua ]; then
		# Lua 5.1, twice from one tree like GLib and ICU: the HOST
		# interpreter runs luakit's code generators (gentokens.lua),
		# the TARGET library is what luakit links -- plus the
		# interpreter itself for the image, since a scriptable system
		# might as well ship the language its browser embeds.
		#
		# `generic` with the POSIX and dlopen features named
		# explicitly, because the platform targets are a hardcoded OS
		# list this system is not on.  LUA_USE_DLOPEN is what lets
		# require() load a C module -- LuaFileSystem below is one --
		# and needs no -ldl here: dlopen lives in this libc.
		(
			cd "$dir" || exit 1
			make clean >/dev/null 2>&1 || true
			make -j"$(nproc)" generic \
				MYCFLAGS="-DLUA_USE_POSIX -DLUA_USE_DLOPEN" &&
			make install INSTALL_TOP="$HOSTTOOLS" || exit 1
			make clean >/dev/null 2>&1 || true
			# `all' in src/, NOT the `generic' target, and the
			# reason is not style: src/Makefile's platform targets
			# are all of the form
			#
			#     generic:
			#             $(MAKE) all MYCFLAGS=
			#
			# so `generic' CLEARS MYCFLAGS in its recursive make.
			# Every flag named here was silently discarded -- the
			# interpreter was built with none of them, and said so
			# only when a C module was finally required:
			#
			#     dynamic libraries not enabled; check your Lua
			#     installation
			#
			# LUA_ROOT has to agree with INSTALL_TOP below.
			# luaconf.h derives package.path and package.cpath from
			# it and this port installs under /usr, so with
			# upstream's /usr/local/ default require() searched
			# /usr/local/lib/lua/5.1/ while LuaFileSystem sat in
			# /usr/lib/lua/5.1/.  luakit stopped on its first line
			# with "module 'lfs' not found" and a list of a dozen
			# candidates, none of them where the file is.  The
			# definition needs patch 0001 to be overridable at all:
			# luaconf.h defines it unconditionally, so a -D on the
			# command line loses to the header.
			#
			# The .lua list looks longer than the .so list only
			# because luakit appends its own directories to
			# package.path.  Nothing appends to cpath, so a C
			# module is found through this default or not at all.
			#
			# -Wl,-E is what upstream's own `linux' target passes:
			# it exports the interpreter's symbols so a dlopen'd C
			# module can resolve lua_* against the binary that
			# loaded it.  luakit does not rely on this (it links
			# liblua itself and already passes --export-dynamic),
			# but the standalone interpreter does.  No -ldl: dlopen
			# is in this libc.
			make -j"$(nproc)" -C src all \
				CC="$here/toolchain/likeos-cc" \
				AR="ar rcu" RANLIB=ranlib \
				MYCFLAGS="-DLUA_USE_POSIX -DLUA_USE_DLOPEN -DLUA_ROOT='\"/usr/\"'" \
				MYLIBS="-Wl,-E" &&
			make install INSTALL_TOP="$SYSROOT/usr" \
				INSTALL_MAN="$SYSROOT/usr/share/man/man1" || \
				exit 1
			# The package predates pkg-config manifests; luakit
			# finds Lua through one, so it is written here.  The
			# paths are target paths -- likeos-pkg-config prefixes
			# the sysroot when answering.
			mkdir -p "$SYSROOT/usr/lib/pkgconfig"
			cat >"$SYSROOT/usr/lib/pkgconfig/lua-5.1.pc" <<'LUAPC'
prefix=/usr
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: Lua
Description: An extensible embeddable scripting language
Version: 5.1.5
Libs: -L${libdir} -llua -lm
Cflags: -I${includedir}
LUAPC
		) >"$log" 2>&1
	elif [ "$name" = luafilesystem ]; then
		# One C file compiled into the module directory Lua 5.1's
		# require() searches (/usr/lib/lua/5.1).  Its references to the
		# Lua API stay undefined in the .so and resolve from the
		# embedding program, which is why luakit links with
		# --export-dynamic -- the arrangement every Lua C module uses.
		(
			cd "$dir" || exit 1
			"$here/toolchain/likeos-cc" -O2 -fPIC -shared \
				-o lfs.so src/lfs.c &&
			mkdir -p "$SYSROOT/usr/lib/lua/5.1" &&
			cp -f lfs.so "$SYSROOT/usr/lib/lua/5.1/lfs.so" &&
			post_install "$name"
		) >"$log" 2>&1
	elif [ "$name" = luakit ]; then
		# Plain make, with everything its config.mk would otherwise
		# probe stated on the command line: the cross compiler, the
		# confined pkg-config, the port's host Lua for the generators,
		# and USE_LUAJIT=0 so nothing goes looking for a JIT.
		#
		# VERSION is stated for the same reason.  config.mk derives it
		# by asking git, and falls back to a commit hash that `git
		# archive' substituted into build-utils/getversion.sh -- which
		# is what a tag export like this tarball carries.  The version
		# goes into -DVERSION, into `luakit --version', and into the
		# manual page; a hash there says nothing a reader can match
		# against the manifest, which names the release.  Taken from
		# the source directory's own suffix, which unpack.sh named
		# from that manifest line.
		(
			cd "$dir" || exit 1
			PATH="$HOSTTOOLS/bin:$PATH"
			export PATH
			ver=${dir##*-}

			# `make clean' needs the SAME variables as the build,
			# and it has to run after PATH is set.  config.mk
			# probes for the Lua binary before it will do anything
			# at all -- clean included -- and with none of this
			# stated it stops with
			#
			#   config.mk:91: *** Cannot find the Lua binary name.
			#
			# which `|| true' then swallowed.  Nothing was cleaned,
			# make found the binary newer than its prerequisites
			# and relinked nothing, and `install' shipped a stale
			# luakit.  That is how a rebuilt libsqlite3 left luakit
			# still naming the old unversioned soname while every
			# other consumer had been corrected.
			make clean \
				CC="$here/toolchain/likeos-cc" \
				PKG_CONFIG="$here/toolchain/likeos-pkg-config" \
				LUA_PKG_NAME=lua-5.1 LUA_BIN_NAME=lua \
				USE_LUAJIT=0 DEVELOPMENT_PATHS=0 \
				VERSION="$ver" \
				PREFIX=/usr XDGPREFIX=/etc/xdg \
				>/dev/null 2>&1 || true
			make -j"$(nproc)" \
				CC="$here/toolchain/likeos-cc" \
				PKG_CONFIG="$here/toolchain/likeos-pkg-config" \
				LUA_PKG_NAME=lua-5.1 LUA_BIN_NAME=lua \
				USE_LUAJIT=0 DEVELOPMENT_PATHS=0 \
				VERSION="$ver" \
				PREFIX=/usr XDGPREFIX=/etc/xdg &&
			make install DESTDIR="$SYSROOT" \
				CC="$here/toolchain/likeos-cc" \
				PKG_CONFIG="$here/toolchain/likeos-pkg-config" \
				LUA_PKG_NAME=lua-5.1 LUA_BIN_NAME=lua \
				USE_LUAJIT=0 DEVELOPMENT_PATHS=0 \
				VERSION="$ver" \
				PREFIX=/usr XDGPREFIX=/etc/xdg &&
			post_install "$name"
		) >"$log" 2>&1
	elif is_hosttool "$name"; then
		# For THIS machine: the host compiler, the host's own headers,
		# and no cross wrappers anywhere near it.
		(
			cd "$dir" || exit 1
			[ -f Makefile ] && make distclean >/dev/null 2>&1
			./configure --prefix="$HOSTTOOLS" &&
				make -j"$(nproc)" &&
				make install &&
				post_install "$name"
		) >"$log" 2>&1
	elif is_dataonly "$name"; then
		(
			cd "$dir" || exit 1
			install_data "$name" && post_install "$name"
		) >"$log" 2>&1
	elif is_plainmake "$name"; then
		(
			cd "$dir" || exit 1
			# Never build on top of the last build, for the same
			# reason the autotools arm distcleans: generated files
			# carry the settings they were generated WITH.  lz4
			# makes liblz4.pc from a template with the prefix baked
			# in, as an ordinary file target -- so a rebuild with a
			# corrected prefix found the old one up to date, left
			# it alone, and reported success while shipping a .pc
			# pointing at /usr/local.
			#
			# Failure ignored: a tree that has never been built has
			# nothing to clean, and some of these have no clean
			# target until their platform target has run.
			make clean >/dev/null 2>&1 || true
			make -j"$(nproc)" $(plainmake_args "$name") &&
				make install DESTDIR="$SYSROOT" PREFIX=/usr \
					$(plainmake_install_args "$name") &&
				post_install "$name"
		) >"$log" 2>&1
	elif [ "$name" = llvm ]; then
		# LLVM for llvmpipe: the JIT runs inside TARGET processes, so
		# the library is cross-compiled like everything else.  Two
		# stages: a native tree that exists only to provide runnable
		# tblgen (and a native llvm-config for the mesa wrapper to
		# delegate to), then the target build -- LLVM only, X86
		# backend only, one shared libLLVM, RTTI on because mesa
		# subclasses LLVM types.  Link jobs pinned to 1: the dylib
		# link peaks at several GB.
		(
			cd "$dir" || exit 1
			if [ ! -x .likeos-native/bin/llvm-tblgen ] ||
				[ ! -x .likeos-native/bin/llvm-config ]; then
				cmake -S llvm -B .likeos-native -G Ninja \
					-DCMAKE_BUILD_TYPE=Release \
					-DLLVM_TARGETS_TO_BUILD=X86 \
					-DLLVM_INCLUDE_TESTS=OFF \
					-DLLVM_INCLUDE_BENCHMARKS=OFF \
					-DLLVM_INCLUDE_EXAMPLES=OFF \
					-DLLVM_INCLUDE_DOCS=OFF &&
					ninja -C .likeos-native \
						llvm-tblgen llvm-min-tblgen \
						llvm-config || exit 1
			fi
			memkb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
			jobs=$((memkb / (1536 * 1024)))
			[ "$jobs" -lt 1 ] && jobs=1
			[ "$jobs" -gt "$(nproc)" ] && jobs=$(nproc)
			echo "llvm: $jobs compile job(s)" >&2
			if [ ! -f .likeos-build/build.ninja ]; then
				rm -rf .likeos-build
				LIKEOS_TOOLCHAIN="$here/toolchain" \
				cmake -S llvm -B .likeos-build -G Ninja \
					-DCMAKE_TOOLCHAIN_FILE="$here/toolchain/likeos-toolchain.cmake" \
					-DCMAKE_INSTALL_PREFIX=/usr \
					-DCMAKE_BUILD_TYPE=Release \
					-DLLVM_TARGETS_TO_BUILD=X86 \
					-DLLVM_HOST_TRIPLE=x86_64-likeos-gnu \
					-DLLVM_TABLEGEN="$PWD/.likeos-native/bin/llvm-tblgen" \
					-DLLVM_NATIVE_TOOL_DIR="$PWD/.likeos-native/bin" \
					-DLLVM_BUILD_LLVM_DYLIB=ON \
					-DLLVM_LINK_LLVM_DYLIB=ON \
					-DLLVM_ENABLE_RTTI=ON \
					-DLLVM_ENABLE_THREADS=ON \
					-DLLVM_ENABLE_ZLIB=OFF \
					-DLLVM_ENABLE_ZSTD=OFF \
					-DLLVM_ENABLE_LIBXML2=OFF \
					-DLLVM_ENABLE_LIBEDIT=OFF \
					-DLLVM_ENABLE_LIBPFM=OFF \
					-DLLVM_ENABLE_OCAMLDOC=OFF \
					-DLLVM_ENABLE_BINDINGS=OFF \
					-DLLVM_INCLUDE_TESTS=OFF \
					-DLLVM_INCLUDE_BENCHMARKS=OFF \
					-DLLVM_INCLUDE_EXAMPLES=OFF \
					-DLLVM_INCLUDE_DOCS=OFF \
					-DLLVM_INCLUDE_UTILS=OFF \
					-DLLVM_BUILD_TOOLS=OFF \
					-DLLVM_PARALLEL_LINK_JOBS=1 || exit 1
			fi
			ninja -C .likeos-build -j"$jobs" &&
				DESTDIR="$SYSROOT" ninja -C .likeos-build install &&
				post_install "$name"
		) >"$log" 2>&1
	elif prefers_meson "$name" || is_meson "$name" "$dir"; then
		(
			cd "$dir" || exit 1

			# The native half first, for packages whose generators
			# the rest of the stack runs.  Its prefix is $HOSTTOOLS,
			# NOT the sysroot: what comes out is built for this
			# machine and must never be a candidate for the image.
			if needs_host_build "$name"; then
				rm -rf .likeos-host
				# Same PATH as the cross half below: it is
				# what finds the port's own meson, which on a
				# distribution that refuses a system-wide pip
				# install is the only one new enough.
				PATH="$HOSTTOOLS/bin:$PATH" \
					meson setup .likeos-host \
					--prefix="$HOSTTOOLS" \
					--libdir="$HOSTTOOLS/lib" \
					$(meson_host_opts "$name") &&
					PATH="$HOSTTOOLS/bin:$PATH" \
						meson install -C .likeos-host ||
					exit 1
			fi

			rm -rf .likeos-build
			PATH="$here/toolchain:$HOSTTOOLS/bin:$PATH" \
				meson setup .likeos-build \
				--cross-file "$here/toolchain/likeos-cross.ini" \
				--prefix=/usr --libdir=/usr/lib \
				-Ddefault_library=shared \
				$(meson_opts "$name") &&
				DESTDIR="$SYSROOT" PATH="$here/toolchain:$HOSTTOOLS/bin:$PATH" \
					meson install -C .likeos-build &&
					post_install "$name"
		) >"$log" 2>&1
	elif is_noarch "$name"; then
		(
			cd "$dir" || exit 1
			if [ ! -f configure ] && [ -f autogen.sh ]; then
				NOCONFIGURE=1 ./autogen.sh || exit 1
			fi
			ACLOCAL_PATH="$SYSROOT/usr/share/aclocal" \
				PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig" \
				./configure --prefix=/usr --datarootdir=/usr/share \
				$(pkg_opts "$name") &&
				make -j"$(nproc)" &&
				make install DESTDIR="$SYSROOT"
		) >"$log" 2>&1
	else
		(
			cd "$dir" || exit 1
			# Never build on top of a previous configuration.
			#
			# libtool records per-object decisions in the .lo files,
			# including whether a PIC object exists at all.  Objects
			# compiled while libtool believed this platform could not
			# build shared libraries carry pic_object=none, and a
			# later run -- after that was fixed -- reuses the .lo and
			# links the non-PIC object into the shared library, where
			# it contributes NOTHING.  libXt came out that way with
			# XtStrings undefined: the library built, installed, and
			# only failed when a program finally referenced the
			# symbol, several packages later.
			#
			# Reconfiguring is cheap; finding that class of bug is
			# not.
			#
			# ...except where distclean removes SOURCE.  A package
			# whose sources are generated from something else ships
			# the generated copies in its release tarball so the
			# generator is not a build dependency -- and its
			# distclean deletes them, on the assumption that
			# whoever runs it can make them again.  Here nobody
			# can.  See keeps_generated_sources().
			if ! keeps_generated_sources "$name"; then
				[ -f Makefile ] && make distclean >/dev/null 2>&1
			fi
			# $HOSTTOOLS/bin ahead of the rest, as in the meson arm:
			# it is where the tools built FOR THIS MACHINE live, and a
			# package that runs one during its own build has to find
			# that copy rather than a target binary or nothing at all.
			# intltool is why this is here -- the LXDE packages'
			# configure stops outright when intltool-update is not on
			# the path, and their Makefiles then run it again to build
			# the .desktop and .menu files themselves.
			#
			# Exported for the whole subshell rather than prefixed to
			# the configure line: the build below has to see it too,
			# and it is a function call, which is not a command a
			# variable assignment can be prefixed to portably.
			PATH="$HOSTTOOLS/bin:$PATH"
			export PATH

			env $(pkg_env "$name") \
				"$here/toolchain/likeos-autogen.sh" $(pkg_opts "$name") &&
				host_tools "$name" &&
				build_subdirs "$name" &&
				post_install "$name"
		) >"$log" 2>&1
	fi

	if [ $? -eq 0 ]; then
		# Drop libtool's .la files.  Each records libdir='/usr/lib',
		# which is where the library will live ON THE TARGET — but
		# libtool reads that literally and goes looking on the build
		# host, where it finds either nothing or the host's own copy.
		# Nothing needs them: linking goes through pkg-config and -l.
		rm -f "$SYSROOT"/usr/lib/*.la

		relativise_sysroot_symlinks
		echo "ok"
		: >"$stamps/$name"
		return 0
	fi
	echo "FAILED  (see .logs/$name.log)"
	return 1
}

built=0
failed=0
skipped=0

while read -r section name version deb repo rest; do
	case "$section" in
	'' | '#'*) continue ;;
	esac
	[ -n "$want" ] && case " $want " in *" $name "*) ;; *) continue ;; esac

	if [ "$force" = "0" ] && [ -z "$want" ] && [ -f "$stamps/$name" ]; then
		skipped=$((skipped + 1))
		continue
	fi

	# Resolve this package's source tree.
	#
	# `$name-*` on its own is ambiguous: xcb-util also matches
	# xcb-util-image, xcb-util-keysyms and xcb-util-wm.  find returns
	# directory order -- whatever the filesystem hands back, which differs
	# between machines -- so `head -1` was picking xcb-util-image on a fresh
	# clone and configuring the wrong tree, which then failed looking for
	# the xcb-util it had just been asked to build.
	#
	# Take the manifest's version when that tree is present.  It is not
	# always: fetch.sh falls back to Debian and X.Org GitLab, whose versions
	# differ from the manifest's.  So the fallback accepts only a suffix
	# that looks like a version (starts with a digit), which is what rules
	# out the sibling packages, and sorts so the pick is the same
	# everywhere.
	if [ -d "$port/$name-$version" ]; then
		dir="$port/$name-$version"
	else
		dir=$(find "$port" -maxdepth 1 -type d -name "$name-[0-9]*" |
			sort -V | tail -1)
	fi
	if [ -z "$dir" ]; then
		printf '%-22s no source tree (run unpack.sh)\n' "$name"
		failed=$((failed + 1))
		continue
	fi

	if build_one "$name" "$dir"; then
		built=$((built + 1))
	else
		failed=$((failed + 1))
		# Stop at the first failure: these are dependency-ordered, so
		# everything after it would fail for a reason that is not its own.
		break
	fi
done <"$list"

echo "built: $built   already done: $skipped   failed: $failed"
[ "$failed" -eq 0 ]
