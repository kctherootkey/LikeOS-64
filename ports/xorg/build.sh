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
list="$here/packages.list"
stamps="$here/.stamps"
logs="$here/.logs"
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"
export LIKEOS_SYSROOT="$SYSROOT"

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
		echo "--without-xft"
		;;
	xterm)
		echo "--disable-imake --enable-256-color --disable-desktop \
		      --with-app-defaults=/usr/share/X11/app-defaults"
		;;
	*) echo "" ;;
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
	*) echo "" ;;
	esac
}

# Build and install, either the whole tree or just the named subdirectories.
build_subdirs() {
	dirs=$(make_dirs "$1" | sed 's/-C //g')
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
	libXpm)
		# The .pc file is generated at the top level, so building only
		# src/ installs the library but not the description of it — and
		# the next package's configure then cannot find xpm.
		make install-pkgconfigDATA DESTDIR="$SYSROOT" || return 1
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

is_cmake() {
	[ -f "$2/CMakeLists.txt" ] && [ ! -f "$2/meson.build" ] &&
		[ ! -f "$2/configure" ] && [ ! -f "$2/autogen.sh" ]
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
				DESTDIR="$SYSROOT" cmake --install .likeos-build
		) >"$log" 2>&1
	elif is_meson "$name" "$dir"; then
		(
			cd "$dir" || exit 1
			rm -rf .likeos-build
			PATH="$here/toolchain:$PATH" \
				meson setup .likeos-build \
				--cross-file "$here/toolchain/likeos-cross.ini" \
				--prefix=/usr --libdir=/usr/lib \
				-Ddefault_library=shared \
				$(meson_opts "$name") &&
				DESTDIR="$SYSROOT" PATH="$here/toolchain:$PATH" \
					meson install -C .likeos-build
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
			[ -f Makefile ] && make distclean >/dev/null 2>&1
			"$here/toolchain/likeos-autogen.sh" $(pkg_opts "$name") &&
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

	dir=$(find "$here" -maxdepth 1 -type d -name "$name-*" | head -1)
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
