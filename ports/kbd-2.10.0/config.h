/* config.h - kbd 2.10.0 configuration for LikeOS
 *
 * What configure would find on this system, written down for the hand
 * build of Makefile.likeos: no NLS (the messages stay in English), no
 * compressed keymaps (the keymaps are installed uncompressed), no XKB
 * conversion in loadkeys (the console keymaps are generated at build time).
 *
 * Copyright (C) 2026 The LikeOS Project
 */
#ifndef KBD_LIKEOS_CONFIG_H
#define KBD_LIKEOS_CONFIG_H

#define PACKAGE "kbd"
#define PACKAGE_NAME "kbd"
#define PACKAGE_VERSION "2.10.0"
#define PACKAGE_STRING "kbd 2.10.0"
#define PACKAGE_TARNAME "kbd"
#define PACKAGE_BUGREPORT "gladkov.alexey@gmail.com"
#define PACKAGE_URL ""
#define VERSION "2.10.0"

#ifndef DATADIR
#define DATADIR "/usr/share"
#endif
#define KEYMAPDIR "keymaps"
#define UNIMAPDIR "unimaps"
#define TRANSDIR "consoletrans"
#define VIDEOMODEDIR "videomodes"
#define FONTDIR "consolefonts"
#define PARTIALDIR "partial"
#ifndef DEFMAP
#define DEFMAP "defkeymap.map"
#endif
/* Where loadkeys also looks for a keymap: the same directory. */
#ifndef KERNDIR
#define KERNDIR "/usr/share/keymaps"
#endif
#define LOCALEDIR "/usr/share/locale"

#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_FCNTL_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_STRDUP 1
#define HAVE_STRNDUP 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_STRERROR 1
#define HAVE_MEMSET 1
#define HAVE_MALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FORK 1
#define HAVE_WORKING_FORK 1
#define HAVE_ALARM 1
#define STDC_HEADERS 1

/* The C library has the program name variable; <errno.h> does not declare
 * it here, as kbd expects, but <err.h>. */
extern char *program_invocation_short_name;

#endif
