/*
 * config.h - what dosfstools' configure would pass on the compiler command
 * line (DEFS), for the LikeOS libc.  Makefile.likeos force-includes it into
 * every source.
 *
 * Written by hand because dosfstools is built here without its autotools
 * build (see Makefile.likeos).  Every HAVE_ below was checked against the
 * LikeOS libc: the header exists, the function is declared and libc.so
 * defines it.  Anything not listed is absent and dosfstools takes its
 * fallback path:
 *
 *   no <linux/fd.h>, <linux/loop.h>: no floppy geometry requests and no
 *       loop devices; a disk's geometry comes from HDIO_GETGEO, the start
 *       of a partition from /sys/dev/block/M:m/start
 *   no iconv: the libc's iconv converts the Unicode encodings and Latin-1/9
 *       but not the DOS code pages, so dosfstools uses its built-in CP850
 *       table (the default code page; other -c/--codepage values are
 *       refused with a message, as upstream's --without-iconv build does)
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_DOSFS_CONFIG_H
#define LIKEOS_DOSFS_CONFIG_H

#define PACKAGE "dosfstools"
#define PACKAGE_NAME "dosfstools"
#define PACKAGE_TARNAME "dosfstools"
#define PACKAGE_VERSION "4.2"
#define PACKAGE_STRING "dosfstools 4.2"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL ""

/* ---- headers ---- */
#define STDC_HEADERS 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ERR_H 1
#define HAVE_LINUX_HDREG_H 1
#define HAVE_SYS_QUEUE_H 1
#define HAVE_ENDIAN_H 1

/* ---- functions and declarations ---- */
#define HAVE_VASPRINTF 1
#define HAVE_DECL_GETMNTENT 1
#define HAVE_DECL_GETMNTINFO 0

#endif /* LIKEOS_DOSFS_CONFIG_H */
