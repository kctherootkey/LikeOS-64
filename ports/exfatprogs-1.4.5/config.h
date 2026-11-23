/*
 * config.h - what exfatprogs' configure would write into its config.h, for
 * the LikeOS libc.  Makefile.likeos force-includes it into every source, as
 * the release's own build does ("-include config.h").
 *
 * Written by hand because exfatprogs is built here without its autotools
 * build (see Makefile.likeos).  Every HAVE_ below was checked against the
 * LikeOS libc: the header exists and libc.so defines what it declares.
 *
 *   HAVE_BLKID: mkfs.exfat's check for an existing file system or partition
 *       table uses libblkid, which comes from the util-linux port
 *       (ports/util-linux-2.42.4) as a static archive
 *   EXFAT_NO_IOSTATS: no I/O statistics, configure's default
 *   no <fts.h>: only lsdosattr/chdosattr need it, and they are not built
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_EXFATPROGS_CONFIG_H
#define LIKEOS_EXFATPROGS_CONFIG_H

#define PACKAGE "exfatprogs"
#define PACKAGE_NAME "exfatprogs"
#define PACKAGE_TARNAME "exfatprogs"
#define PACKAGE_VERSION "1.4.5"
#define PACKAGE_STRING "exfatprogs 1.4.5"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL ""
#define VERSION "1.4.5"

#define EXFAT_NO_IOSTATS 1
#define HAVE_BLKID 1

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
#define HAVE_DLFCN_H 1

#endif /* LIKEOS_EXFATPROGS_CONFIG_H */
