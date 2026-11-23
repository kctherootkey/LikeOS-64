/*
 * config.h - what the ntfs-3g configure would write into its config.h, for
 * the LikeOS libc, configured the way Makefile.likeos builds it: ntfsprogs
 * only (no FUSE driver, no mount helper), libntfs-3g as a static archive,
 * no crypto (ntfsdecrypt), no external libraries but util-linux's libuuid.
 * Makefile.likeos compiles every source with -DHAVE_CONFIG_H, as the
 * release's build does.
 *
 * Written by hand because the package is built here without its autotools
 * build.  Every HAVE_ below was checked against the LikeOS libc: the header
 * exists, the function is declared and libc.so defines it.  Absent, so the
 * sources take their fallback paths:
 *
 *   <libintl.h>                 no message catalogues
 *   the floppy headers fd.h and major.h
 *                               no floppy geometry; a disk's geometry comes
 *                               from HDIO_GETGEO
 *   <sys/byteorder.h>, <sys/disk.h>, <sys/endian.h>, <machine/endian.h>,
 *   <windows.h>                 other systems' headers
 *   ENABLE_HD                   no libhd: mkntfs takes the geometry from
 *                               HDIO_GETGEO
 *   ENABLE_CRYPTO               no gnutls/libgcrypt: ntfsdecrypt is not built
 *
 *   ENABLE_UUID: mkntfs gives the volume a DCE GUID with uuid_generate()
 *       from util-linux's libuuid (ports/util-linux-2.42.4).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_NTFS3G_CONFIG_H
#define LIKEOS_NTFS3G_CONFIG_H

#define PACKAGE "ntfs-3g"
#define PACKAGE_NAME "ntfs-3g"
#define PACKAGE_TARNAME "ntfs-3g"
#define PACKAGE_VERSION "2026.9.28"
#define PACKAGE_STRING "ntfs-3g 2026.9.28"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL ""
#define VERSION "2026.9.28"

#define ENABLE_UUID 1

/* x86-64 */
#define WORDS_LITTLEENDIAN 1

/* ---- headers ---- */
#define STDC_HEADERS 1
#define HAVE_BYTESWAP_H 1
#define HAVE_CTYPE_H 1
#define HAVE_DLFCN_H 1
#define HAVE_ENDIAN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FEATURES_H 1
#define HAVE_GETOPT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIBGEN_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LINUX_FS_H 1
#define HAVE_LINUX_HDREG_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MALLOC_H 1
#define HAVE_MNTENT_H 1
#define HAVE_PWD_H 1
#define HAVE_REGEX_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE__BOOL 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_VFS_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_WCHAR_H 1
/* major()/minor() come from <sys/sysmacros.h> */
#define MAJOR_IN_SYSMACROS 1

/* ---- types and structures ---- */
#define HAVE_STRUCT_STAT_ST_ATIM 1
#define HAVE_STRUCT_STAT_ST_BLOCKS 1
#define HAVE_STRUCT_STAT_ST_RDEV 1
#define HAVE_ST_BLOCKS 1
#define LSTAT_FOLLOWS_SLASHED_SYMLINK 1

/* ---- functions ---- */
#define HAVE_ATEXIT 1
#define HAVE_BASENAME 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DAEMON 1
#define HAVE_DUP2 1
#define HAVE_FDATASYNC 1
#define HAVE_FFS 1
#define HAVE_FORK 1
#define HAVE_GETMNTENT 1
#define HAVE_GETOPT_LONG 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_HASMNTOPT 1
#define HAVE_MBRTOWC 1
#define HAVE_MBSINIT 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_RANDOM 1
#define HAVE_REALPATH 1
#define HAVE_REGCOMP 1
#define HAVE_SETLOCALE 1
#define HAVE_SETXATTR 1
#define HAVE_SNPRINTF 1
#define HAVE_STRCASECMP 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRNLEN 1
#define HAVE_STRSEP 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_SYSCONF 1
#define HAVE_UTIME 1
#define HAVE_UTIMENSAT 1
#define HAVE_UTIME_NULL 1
#define HAVE_VPRINTF 1

#endif /* LIKEOS_NTFS3G_CONFIG_H */
