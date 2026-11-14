/*
 * config.h - what attr's configure would write into include/config.h, for
 * the LikeOS libc.  Makefile.likeos puts this directory first on the include
 * path, so the sources' #include "config.h" finds this file.
 *
 * Written by hand because attr is built here without its autotools build
 * (see Makefile.likeos).  Every HAVE_ below was checked against the LikeOS
 * libc: the header exists, the function is declared and libc.so defines it.
 * Anything not listed is absent and attr takes its fallback path:
 *
 *   no getxattrat/setxattrat/listxattrat/removexattrat: libmisc/xattrat.c
 *       stands in for them and answers ENOSYS (the system call numbers do
 *       not exist), so the tools use the path based calls on
 *       /proc/self/fd/N/<name> (libmisc/<call>_compat.c)
 *   no openat2 (USE_OPENAT2 unset): no unsafe-restore warnings
 *   no NLS: messages are the built-in English ones
 *
 * The libc has the xattr calls themselves (setxattr ... fremovexattr), so
 * libattr's old compatibility wrappers (libattr/syscalls.c, which call the
 * system call numbers directly) are not built.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_ATTR_CONFIG_H
#define LIKEOS_ATTR_CONFIG_H

#define PACKAGE "attr"
#define PACKAGE_NAME "attr"
#define PACKAGE_TARNAME "attr"
#define PACKAGE_VERSION "2.6.0"
#define PACKAGE_STRING "attr 2.6.0"
#define PACKAGE_BUGREPORT "acl-devel@nongnu.org"
#define PACKAGE_URL ""
#define VERSION "2.6.0"

/* ---- headers ---- */
#define STDC_HEADERS 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_DLFCN_H 1

/* ---- compiler ---- */
#define HAVE_VISIBILITY_ATTRIBUTE /**/
#define EXPORT __attribute__ ((visibility ("default"))) extern
#define HAVE_SYMVER_ATTRIBUTE /**/

#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif

/* <dirent.h> has the DT_ values (the conventional ones: the file type bits
 * of st_mode shifted down by 12) but not the macro converting a mode to
 * them, which libmisc/walk_tree.c uses. */
#ifndef IFTODT
# define IFTODT(mode) (((mode) & 0170000) >> 12)
#endif

/* There are no O_PATH descriptors.  As 0 the flag makes libmisc's
 * openat_with_flags() (used for names longer than PATH_MAX only) open for
 * reading, and need_slow_empty_paths() never sees an O_PATH descriptor. */
#ifndef O_PATH
# define O_PATH 0
#endif

#endif /* LIKEOS_ATTR_CONFIG_H */
