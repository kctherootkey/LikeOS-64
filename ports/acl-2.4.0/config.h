/*
 * config.h - what acl's configure would write into include/config.h, for
 * the LikeOS libc.  Makefile.likeos puts this directory first on the include
 * path, so the sources' #include "config.h" finds this file.
 *
 * Written by hand because acl is built here without its autotools build
 * (see Makefile.likeos).  Every HAVE_ below was checked against the LikeOS
 * libc: the header exists, the function is declared and libc.so defines it.
 * Anything not listed is absent and acl takes its fallback path:
 *
 *   no getxattrat/setxattrat/listxattrat/removexattrat: libmisc/xattrat.c
 *       stands in for them and answers ENOSYS (the system call numbers do
 *       not exist), so the library and the tools use the path based calls
 *       on the directory descriptor's path (libmisc/<call>_compat.c, with
 *       patches/0002)
 *   no openat2 (USE_OPENAT2 unset): no unsafe-restore warnings
 *   no NLS: messages are the built-in English ones
 *
 * The ACLs themselves are the system.posix_acl_access and
 * system.posix_acl_default extended attributes, read and written with the
 * libc's getxattr/setxattr family; libacl needs nothing else of the system.
 * <attr/error_context.h> (perm_copy_fd/perm_copy_file) is the attr port's.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_ACL_CONFIG_H
#define LIKEOS_ACL_CONFIG_H

#define PACKAGE "acl"
#define PACKAGE_NAME "acl"
#define PACKAGE_TARNAME "acl"
#define PACKAGE_VERSION "2.4.0"
#define PACKAGE_STRING "acl 2.4.0"
#define PACKAGE_BUGREPORT "acl-devel@nongnu.org"
#define PACKAGE_URL ""
#define VERSION "2.4.0"

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
#define HAVE_ATTR_ERROR_CONTEXT_H 1

/* ---- compiler, target ---- */
#define HAVE_VISIBILITY_ATTRIBUTE /**/
#define EXPORT __attribute__ ((visibility ("default"))) extern
/* x86-64: little endian, WORDS_BIGENDIAN stays undefined. */

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
 * openat_with_flags() and fchmodat_compat() (only after fchmodat() refused
 * the flags, which it does for a symbolic link with AT_SYMLINK_NOFOLLOW)
 * open for reading, and need_slow_empty_paths() never sees an O_PATH
 * descriptor. */
#ifndef O_PATH
# define O_PATH 0
#endif

#endif /* LIKEOS_ACL_CONFIG_H */
