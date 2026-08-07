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
# .stamps still said they were built -- and the next netsurf build failed with
# "nsgenbind: not found".  This lives beside .stamps and .logs, which are the
# port's own build state and which the top-level clean does not touch, so the
# three cannot get out of step with each other.
HOSTTOOLS="$here/.hosttools"

mkdir -p "$stamps" "$logs" \
	"$SYSROOT/usr/include" "$SYSROOT/usr/lib/pkgconfig" \
	"$SYSROOT/usr/share/aclocal" "$SYSROOT/usr/share/pkgconfig" \
	"$SYSROOT/usr/bin"

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
		# fbdev + evdev only.  GLX/DRI/DRI3 are off, which is also what
		# keeps libxshmfence off the dependency list entirely.
		#
		# Also off: libdrm (nothing to talk to), pciaccess (the library
		# is built for its header, but nothing here can reach the bus),
		# vgahw (drives VGA registers through port I/O, which userspace
		# cannot do here, and the fbdev DDX does not use it),
		# int10 (real-mode BIOS calls, which pciaccess would have
		# carried), and the input thread (errno is not thread-local
		# here yet).
		echo "--disable-glx --disable-dri --disable-dri2 --disable-dri3 \
		      --disable-xvfb --disable-xnest --disable-xwayland \
		      --disable-xephyr --disable-dmx --disable-docs \
		      --disable-devel-docs --disable-selective-werror \
		      --disable-systemd-logind --disable-suid-wrapper \
		      --without-dtrace --disable-config-udev --disable-config-hal \
		      --enable-xorg --with-fontrootdir=/usr/share/fonts/X11 \
		      --disable-input-thread --disable-libdrm \
		      --disable-pciaccess --disable-int10-module \
		      --with-int10=stub --disable-glamor \
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
	gnutls)
		# p11-kit is smart-card and PKCS#11 support: another library, a
		# module directory and a daemon, for hardware nothing here can
		# reach.  The included unistring avoids porting libunistring for
		# the handful of Unicode routines GnuTLS uses.  The tools
		# (certtool, gnutls-cli) are diagnostics for a machine with a
		# terminal and time to spend; Claws links the library.
		echo "--disable-doc --disable-tests --disable-tools \
		      --without-p11-kit --with-included-unistring \
		      --without-tpm --without-tpm2 --disable-libdane \
		      --disable-guile"
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
	claws-mail)
		# Off: everything needing a desktop session bus, a system
		# service or a toolchain this system does not have.  --disable-svg
		# in particular keeps librsvg -- and with it a Rust toolchain --
		# out of the dependency graph, for the same reason the Adwaita
		# icon theme is pinned to its last PNG release.
		echo "--disable-dbus --disable-gnome --disable-libnotify \
		      --disable-gpgme --disable-compface --disable-ldap \
		      --disable-jpilot --disable-networkmanager-support \
		      --disable-svg --disable-valgrind --disable-manual \
		      --enable-gnutls --enable-enchant --enable-libetpan"
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
	xkeyboard-config) echo "-Dcompat-rules=true -Dxorg-rules-symlinks=true" ;;
	libdrm) echo "-Dtests=false -Dcairo-tests=disabled -Dman-pages=disabled" ;;

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
		echo "-Dglib=enabled -Dgobject=enabled -Dfreetype=enabled \
		      -Dcairo=disabled -Dicu=disabled -Dchafa=disabled \
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
		# EGL stays off: it is what the Wayland backend uses, and that
		# backend is not built.
		echo "-Dglx=yes -Degl=no -Dx11=true -Dtests=false -Ddocs=false"
		;;
	gtk)
		echo "-Dx11_backend=true -Dwayland_backend=false \
		      -Dbroadway_backend=false -Dprint_backends=file \
		      -Dintrospection=false -Dgtk_doc=false -Dman=true \
		      -Ddemos=false -Dexamples=false -Dtests=false \
		      -Dinstalled_tests=false -Dcolord=no \
		      -Dcloudproviders=false -Dprofiler=false -Dtracker3=false"
		;;
	*) echo "" ;;
	esac
}

# Per-package CMake options.
cmake_opts() {
	case "$1" in
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
	netsurf)
		# The build installs the binary and its resources but not the
		# manual page, which sits unreferenced in docs/.  Installed
		# under the name the image ships the binary as, so `man netsurf`
		# matches the command the user actually has.
		mkdir -p "$SYSROOT/usr/share/man/man1" || return 1
		if [ -f docs/netsurf-fb.1 ]; then
			cp -f docs/netsurf-fb.1 \
				"$SYSROOT/usr/share/man/man1/netsurf.1" || return 1
		fi
		;;
	utf8proc)
		# NetSurf includes <libutf8proc/utf8proc.h>, but utf8proc
		# installs its header at the include root as <utf8proc.h>.
		# Both spellings are in use across distributions, and the
		# package offers no option to choose, so the second one is
		# provided here rather than patching every consumer.
		#
		# A copy, not a symlink: the sysroot is packed into the image
		# and copied between machines, where a dangling absolute link
		# would be worse than a duplicated 40KB header.
		mkdir -p "$SYSROOT/usr/include/libutf8proc" || return 1
		cp -f "$SYSROOT/usr/include/utf8proc.h" \
			"$SYSROOT/usr/include/libutf8proc/utf8proc.h" || return 1
		;;
	libXpm)
		# The .pc file is generated at the top level, so building only
		# src/ installs the library but not the description of it — and
		# the next package's configure then cannot find xpm.
		make install-pkgconfigDATA DESTDIR="$SYSROOT" || return 1
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
# Same problem nsgenbind has (see the netsurf arm below), one level worse: a
# cross-built generator is a target binary and cannot execute here.  GLib is the
# acute case -- glib-compile-resources, glib-genmarshal, glib-mkenums,
# glib-compile-schemas and gdbus-codegen generate source for nearly everything
# above it, and GTK's build alone invokes the first of those hundreds of times.
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
	hunspell-en_US) return 0 ;;
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
	xnedit) return 0 ;;
	esac
	return 1
}

# NetSurf and its libraries use the project's OWN build system: no configure,
# no meson -- a set of shared makefiles (the "buildsystem" package) that every
# component includes and drives with make variables.  By name for the same
# reason as is_plainmake: a bare Makefile is present in nearly every package
# and so detects nothing.
is_nsbuild() {
	case "$1" in
	buildsystem | libwapcaplet | libparserutils | libhubbub | libcss | \
		libdom | libnsutils | libnslog | libnsgif | libnsbmp | \
		libnspsl | libnsfb | nsgenbind | netsurf)
		return 0
		;;
	esac
	return 1
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
	if is_cmake "$name" "$dir"; then
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
	elif is_nsbuild "$name"; then
		(
			cd "$dir" || exit 1

			# Where the shared makefiles live once `buildsystem` has
			# been installed.  It has to be given explicitly: the
			# components default NSSHARED to $(PREFIX)/share/..., and
			# PREFIX is /usr here, which would point at the HOST's
			# /usr instead of the sysroot.  This is a build-time
			# include path, so it is an absolute host path -- unlike
			# PREFIX, which is where things end up on the target.
			nsshared="$SYSROOT/usr/share/netsurf-buildsystem"

			if [ "$name" = buildsystem ]; then
				# Copies makefiles into place; compiles nothing.
				make install PREFIX=/usr DESTDIR="$SYSROOT" &&
					post_install "$name"
			elif [ "$name" = nsgenbind ]; then
				# Built for the BUILD machine, so the host
				# compiler and NO cross wrappers: this binary
				# runs here, during netsurf's build, and would
				# be useless as a LikeOS executable.
				make -j"$(nproc)" \
					PREFIX="$HOSTTOOLS" \
					NSSHARED="$nsshared" &&
					make install PREFIX="$HOSTTOOLS" \
						NSSHARED="$nsshared"
			elif [ "$name" = netsurf ]; then
				# The browser, not a library.
				#
				# TARGET=framebuffer is the frontend that draws
				# through libnsfb, whose X surface makes it an
				# ordinary X client -- the GTK frontend would
				# drag in GTK, which is not ported.
				#
				# JavaScript is ON.  The bindings between the
				# bundled Duktape engine and the DOM are
				# generated by nsgenbind, which is built just
				# above for THIS machine and found on PATH.
				PATH="$HOSTTOOLS/bin:$PATH" \
				make TARGET=framebuffer -j"$(nproc)" \
					PREFIX=/usr DESTDIR="$SYSROOT" \
					NSSHARED="$nsshared" \
					CC="$here/toolchain/likeos-cc" \
					AR="ar" BUILD_CC=cc \
					PKGCONFIG="$here/toolchain/likeos-pkg-config" \
					PKG_CONFIG="$here/toolchain/likeos-pkg-config" &&
					PATH="$HOSTTOOLS/bin:$PATH" \
					make install TARGET=framebuffer \
						PREFIX=/usr DESTDIR="$SYSROOT" \
						NSSHARED="$nsshared" \
						CC="$here/toolchain/likeos-cc" \
						AR="ar" BUILD_CC=cc \
						PKGCONFIG="$here/toolchain/likeos-pkg-config" \
						PKG_CONFIG="$here/toolchain/likeos-pkg-config" &&
					post_install "$name"
			else
				# Static libraries.  Nothing but netsurf links
				# them, so a shared build would ship eleven
				# libraries to the image for one consumer, and
				# add eleven chances for a soname mismatch.
				#
				# CC/AR come from the command line so they win:
				# Makefile.tools only assigns CC when its origin
				# is `default`, and a command-line variable is
				# never overridden by a makefile.
				#
				# PKGCONFIG is the name this build system uses --
				# NOT PKG_CONFIG, which it ignores.  Its default
				# is `PKG_CONFIG_PATH=$(PREFIX)/lib/pkgconfig
				# pkg-config`, and PREFIX is /usr here, so the
				# default asks the HOST about the HOST's
				# libraries.  That does not fail loudly: feature
				# detection just answers "no" and the component
				# quietly builds without the feature.  It cost an
				# entire libnsfb build -- every X surface was
				# omitted because xcb-icccm and friends were
				# looked for on the build machine.  Both spellings
				# are passed so neither name can go stale.
				make -j"$(nproc)" \
					PREFIX=/usr DESTDIR="$SYSROOT" \
					NSSHARED="$nsshared" \
					COMPONENT_TYPE=lib-static \
					CC="$here/toolchain/likeos-cc" \
					AR="ar" \
					PKGCONFIG="$here/toolchain/likeos-pkg-config" \
					PKG_CONFIG="$here/toolchain/likeos-pkg-config" &&
					make install \
						PREFIX=/usr DESTDIR="$SYSROOT" \
						NSSHARED="$nsshared" \
						COMPONENT_TYPE=lib-static \
						CC="$here/toolchain/likeos-cc" \
						AR="ar" \
						PKGCONFIG="$here/toolchain/likeos-pkg-config" \
						PKG_CONFIG="$here/toolchain/likeos-pkg-config" &&
					post_install "$name"
			fi
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
				make install DESTDIR="$SYSROOT" &&
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
			# The platform target creates the Makefile.<platform>
			# symlinks and then builds util/, Xlt/, Microline/ and
			# source/ in order.  CC and PKG_CONFIG are passed on the
			# command line, which overrides the arm's defaults and is
			# inherited by every sub-make.
			make likeos -j"$(nproc)" \
				CC="$here/toolchain/likeos-cc" \
				PKG_CONFIG="$here/toolchain/likeos-pkg-config" &&
				make install DESTDIR="$SYSROOT" PREFIX=/usr &&
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
