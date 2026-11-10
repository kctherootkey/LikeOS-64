/* config.h - what smartmontools' configure would determine for LikeOS.
 *
 * Written by hand, as the kbd port does: the build is a plain list of C++
 * files (Makefile.likeos) and the answers below are fixed properties of the
 * LikeOS C library and compiler, the same ones a cross configure run against
 * them finds.  Names and meanings are those of config.h.in in the release;
 * an entry left out there is one that does not apply (no systemd, no
 * capability or security-label libraries, no 3ware/ciss/BSD headers).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_SMARTMONTOOLS_CONFIG_H
#define LIKEOS_SMARTMONTOOLS_CONFIG_H

/* Package identity (configure.ac AC_INIT and the release stamp). */
#define PACKAGE "smartmontools"
#define VERSION "7.5"
#define PACKAGE_NAME "smartmontools"
#define PACKAGE_TARNAME "smartmontools"
#define PACKAGE_VERSION "7.5"
#define PACKAGE_STRING "smartmontools 7.5"
#define PACKAGE_BUGREPORT "smartmontools-support@listi.jpberlin.de"
#define PACKAGE_URL "https://www.smartmontools.org/"
#define SMARTMONTOOLS_RELEASE_DATE "2025-04-30"
#define SMARTMONTOOLS_RELEASE_TIME "07:42:55 UTC"
#define CONFIG_H_CVSID "$Id: configure.ac 5714 2025-04-30 07:43:44Z chrfranke $"
#define SMARTMONTOOLS_BUILD_HOST "x86_64-unknown-likeos"
#define SMARTMONTOOLS_CONFIGURE_ARGS "(none: config.h written for LikeOS)"

/* Headers and functions of the C library. */
#define STDC_HEADERS 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_BYTESWAP_H 1
#define HAVE_GETOPT_LONG 1
#define HAVE_REGCOMP 1
#define HAVE_POSIX_API 1

/* Compiler (x86-64 gcc). */
#define HAVE_ATTR_PACKED 1
#define HAVE___INT128 1
#define HAVE_LONG_DOUBLE_WIDER 1
/* WORDS_BIGENDIAN: not defined, x86-64 is little-endian. */

/* close_range(): not in the LikeOS C library; smartd closes descriptors one
 * by one instead, its portable path. */
/* #undef HAVE_CLOSE_RANGE */

/* NVMe devices in smartd's DEVICESCAN: upstream's default for this OS
 * backend is on; LikeOS has no NVMe driver yet, so the scan finds nothing. */
#define WITH_NVME_DEVICESCAN 1

#endif /* LIKEOS_SMARTMONTOOLS_CONFIG_H */
