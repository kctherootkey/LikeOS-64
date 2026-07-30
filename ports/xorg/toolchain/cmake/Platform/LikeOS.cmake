# CMake platform description for LikeOS-64.
#
# CMake loads Platform/${CMAKE_SYSTEM_NAME}.cmake to learn how a system builds
# and links.  There is no module for this one, and the obvious workaround --
# CMAKE_SYSTEM_NAME "Generic" -- is wrong in a way that is easy to miss: Generic
# describes a bare-metal target, so it does not set UNIX, and FindX11.cmake is
# wrapped entirely in `if(UNIX)`.  The result is find_package(X11) reporting
# "Can't find X libs" on a sysroot that visibly contains them.
#
# This system IS a Unix with an ELF dynamic linker, so it says so, and inherits
# the standard Unix search paths.  Those get re-rooted into the sysroot by
# CMAKE_FIND_ROOT_PATH (see likeos-toolchain.cmake), so naming them here does
# not let the build host's /usr in.

set(UNIX 1)

# dlopen/dlsym live in libc here, so there is no separate library to add.  An
# unset value would make CMake link -ldl, which does not exist.
set(CMAKE_DL_LIBS "")

set(CMAKE_SHARED_LIBRARY_C_FLAGS "-fPIC")
set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "-shared")
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "-Wl,-rpath,")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP ":")
set(CMAKE_SHARED_LIBRARY_SONAME_C_FLAG "-Wl,-soname,")

# The X server needs this on the executable so its dlopen()ed modules can bind
# back to it; harmless elsewhere.
set(CMAKE_EXE_EXPORTS_C_FLAG "-Wl,--export-dynamic")

set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".so" ".a")

set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_EXECUTABLE_SUFFIX "")

include(Platform/UnixPaths)
