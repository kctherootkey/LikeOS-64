/*
 * config.h - nvme-cli 2.16 configuration for LikeOS
 *
 * What meson's configure step would write (meson.build), answered once for
 * LikeOS instead of probed; every source file is compiled with
 * "-include config.h", as upstream does.
 *
 * Not built, as meson leaves them out under the same answers:
 *   json-c (CONFIG_JSONC)       -o json output, the solidigm plugin and the
 *                               JSON printers of the lm and ocp plugins;
 *                               -o normal and -o binary work
 *   sed-opal (HAVE_SED_OPAL)    the "sed" plugin needs the kernel's
 *                               self-encrypting-drive ioctls
 * tm_gmtoff: struct tm has no such member here, so HAVE_TM_GMTOFF stays
 * undefined and the vendor timestamp code takes the time zone from the
 * C library's `timezone'.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_NVME_CLI_CONFIG_H
#define LIKEOS_NVME_CLI_CONFIG_H

#define GIT_VERSION "2.16"
#define NVME_VERSION "2.16"
#define SYSCONFDIR "/etc"
#define RUNDIR "/run"

/* Persistent discovery controllers (fabrics; meson's default). */
#define DEFAULT_PDC_ENABLED 1

#define HAVE_BUILTIN_TYPES_COMPATIBLE_P 1
#define HAVE_TYPEOF 1
#define HAVE_BYTESWAP_H 1
#define HAVE_BSWAP_64 1
#define HAVE_LITTLE_ENDIAN 1
#define HAVE_BIG_ENDIAN 0
#define HAVE_ISBLANK 1
#define HAVE_SYS_RANDOM 1
#define HAVE_ATTRIBUTE_UNUSED 0
#define HAVE_SED_OPAL 0
#define HAVE_KEY_TYPE 0

#define fallthrough __attribute__((__fallthrough__))

#endif /* LIKEOS_NVME_CLI_CONFIG_H */
