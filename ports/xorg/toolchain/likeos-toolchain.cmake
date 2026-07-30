# CMake cross-compilation description for LikeOS-64.
#
# The counterpart of likeos-cross.ini (meson) and likeos-autogen.sh (autotools).
# Only one package here uses CMake -- ctwm -- but without this it configures
# against the BUILD HOST: it probes the host compiler, looks for X11 in
# /usr/lib, and either fails with "missing lib or -devel package" or, if the
# host happens to have X development files installed, succeeds and produces a
# binary for the wrong system.
#
# Point CMake at it with -DCMAKE_TOOLCHAIN_FILE=<this file>.  It reads
# LIKEOS_SYSROOT and LIKEOS_TOOLCHAIN from the environment so the paths are not
# baked in.

# A real system name, with a platform module next to this file describing it.
#
# The tempting shortcut is CMAKE_SYSTEM_NAME "Generic", which does turn on
# CMAKE_CROSSCOMPILING (so CMake stops trying to RUN its test programs) but
# describes a bare-metal target: it does not set UNIX, and FindX11.cmake is
# wrapped entirely in `if(UNIX)`.  That produces "Can't find X libs" against a
# sysroot that plainly has them.
#
# CMAKE_MODULE_PATH must be set BEFORE the name, because the platform module is
# loaded through include(), which searches that path.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
set(CMAKE_SYSTEM_NAME LikeOS)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "$ENV{LIKEOS_TOOLCHAIN}/likeos-cc")
set(CMAKE_CXX_COMPILER "$ENV{LIKEOS_TOOLCHAIN}/likeos-cc")

set(CMAKE_SYSROOT "$ENV{LIKEOS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "$ENV{LIKEOS_SYSROOT}/usr")

# Where CMake is allowed to look for each kind of thing.
#
# Programs come from the BUILD HOST -- pkg-config, sed, python and so on run
# here, and looking for them in the sysroot would find nothing.  Everything
# else must come ONLY from the sysroot: a header or library found outside it
# belongs to the build host and is the wrong architecture's idea of the same
# name.  That distinction is the entire point of these four variables, and
# getting the library one wrong is how a cross-build silently links the host's
# libX11.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CMake's own module cache is per-sysroot, not per-host.
set(ENV{PKG_CONFIG_LIBDIR} "$ENV{LIKEOS_SYSROOT}/usr/lib/pkgconfig:$ENV{LIKEOS_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "$ENV{LIKEOS_SYSROOT}")

# likeos-cc supplies its own include path, library path, startup files and
# linker script; CMake must not add a second set of its own.  Leaving these
# empty is what keeps the link line under the wrapper's control.
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")

