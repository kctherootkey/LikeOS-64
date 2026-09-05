#!/bin/sh
#
# Install the two Khronos EGL platform headers into the X sysroot.
#
# libepoxy is built with -Degl=yes because WebKitGTK does not compile without
# <epoxy/egl.h>: PlatformDisplay.cpp and the six platform/graphics/egl/*.cpp
# files include it unconditionally, find_package(Epoxy) is REQUIRED in
# OptionsGTK.cmake, and SourcesGTK.txt lists those sources behind no feature
# flag.
#
# Enabling EGL in epoxy needs no EGL implementation -- meson asks for
# `dependency('egl', required: false)` and the entry points are generated from
# the registry/egl.xml that epoxy ships.  But the GENERATED header is not
# self-contained the way the hand-written epoxy/egl.h is:
#
#     include/epoxy/egl_generated.h:11:  #include "EGL/eglplatform.h"
#
# and eglplatform.h in turn includes <KHR/khrplatform.h>.  Those two headers
# carry no code -- they are the Khronos type definitions (EGLNativeDisplayType,
# khronos_int32_t and friends) that say what an EGL handle IS on this platform.
# For the X11 arm eglplatform.h includes <X11/Xlib.h> and <X11/Xutil.h>, which
# the X.Org port has already put in the sysroot.
#
# So this places the types, and nothing else.  There is still no libEGL here:
# epoxy dlopens it lazily at run time, exactly as it does libGL for GLX, and
# nothing in this configuration calls an EGL entry point (the display is only
# created by initializePlatformDisplayIfNeeded(), which is gated behind
# acceleratedCompositingEnabled -- false, because ENABLE_WEBGL=OFF turns
# canUseHardwareAcceleration off).
#
# PROVENANCE: copied verbatim from the Khronos headers vendored in WebKit's
# bundled ANGLE, Source/ThirdParty/ANGLE/include/{EGL,KHR}/.  They are upstream
# Khronos files -- checked to contain no ANGLE-specific content -- and they are
# kept here rather than read out of the webkitgtk tree because libepoxy is built
# long before webkitgtk is unpacked.
#
#     sha256(EGL/eglplatform.h) b748729767798d85ecf8e1923552879328a76d572327b641ce737549b391cc9c
#     sha256(KHR/khrplatform.h) e206a6931f98ffe1c5c7ece69c4f94bbe1c9279243f40cbe7782848a0d3fa2de
#
# Re-runnable: everything is a copy or an overwrite.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$root/build/xorg-sysroot}"

mkdir -p "$SYSROOT/usr/include/EGL" "$SYSROOT/usr/include/KHR"
# Copied only when the bytes changed: a fresh mtime on either header marks
# most of WebKitGTK's objects stale (see copy_hdrs in import-base-libs.sh).
for h in EGL/eglplatform.h KHR/khrplatform.h; do
	cmp -s "$here/egl-headers/$h" "$SYSROOT/usr/include/$h" 2>/dev/null ||
		cp "$here/egl-headers/$h" "$SYSROOT/usr/include/$h"
done

# The stub libEGL.so.1.  Headers alone are not enough: epoxy resolves EGL
# entry points by dlopen'ing libEGL.so.1 when one is CALLED, and abort()s the
# process when the file is absent -- so a caller that only wanted to ASK
# whether EGL exists was killed instead of answered.  See egl-stub/ for the
# whole reasoning; in short, every entry point returns 0, which is the failure
# value for all of EGL's return types, and callers take their no-EGL path.
#
# Built here rather than as a manifest package because it has no upstream: it
# is generated from the EGL registry libepoxy ships, and it belongs beside the
# headers it answers for.
mkdir -p "$SYSROOT/usr/lib"
# Once Mesa is installed (its post_install leaves this marker), the stub must
# never be written again: it would overwrite the REAL libEGL.
if [ -e "$SYSROOT/usr/lib/.mesa-egl" ]; then
	echo "egl stub          skipped (Mesa libEGL present)"
	exit 0
fi
"$here/toolchain/likeos-cc" -shared -fPIC -O2 \
	-Wl,-soname,libEGL.so.1 \
	-o "$SYSROOT/usr/lib/libEGL.so.1.0.0" \
	"$here/egl-stub/libegl-stub.c" || exit 1
ln -sfn libEGL.so.1.0.0 "$SYSROOT/usr/lib/libEGL.so.1"
ln -sfn libEGL.so.1 "$SYSROOT/usr/lib/libEGL.so"

echo "egl headers      ok"
echo "egl stub         ok"
