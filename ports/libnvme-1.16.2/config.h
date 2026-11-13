/*
 * config.h - libnvme 1.16.2 configuration for LikeOS
 *
 * What meson's configure step would write into the build directory's
 * internal config.h (meson.build), answered once for LikeOS instead of
 * probed: every source file of the library is compiled with
 * "-include config.h", as upstream does.
 *
 * Optional dependencies, all off:
 *   json-c     (CONFIG_JSONC)    the JSON configuration file of the fabrics
 *                                code; no-json.c is built instead
 *   OpenSSL    (CONFIG_OPENSSL)  NVMe/TCP TLS PSK derivation (fabrics)
 *   keyutils   (CONFIG_KEYUTILS) the kernel keyring for TLS keys (fabrics)
 *   liburing   (CONFIG_LIBURING) io_uring pass-through for get-log batches
 *   libdbus    (CONFIG_DBUS)     MCTP endpoint discovery through mctpd
 * None of them changes PCIe NVMe support: the ioctl interface, the sysfs
 * topology scan and the Management Interface encoder all build without
 * them.  NVMe over Fabrics is not part of LikeOS; the fabrics entry points
 * are compiled and fail at run time when /dev/nvme-fabrics is absent.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_LIBNVME_CONFIG_H
#define LIKEOS_LIBNVME_CONFIG_H

#define GIT_VERSION "1.16.2"
#define PROJECT_VERSION "1.16.2"
#define SYSCONFDIR "/etc"

/* The compiler and C library (gcc, LikeOS libc). */
#define HAVE_BUILTIN_TYPES_COMPATIBLE_P 1
#define HAVE_TYPEOF 1
#define HAVE_BYTESWAP_H 1
#define HAVE_BSWAP_64 1
#define HAVE_LITTLE_ENDIAN 1
#define HAVE_BIG_ENDIAN 0
#define HAVE_STATEMENT_EXPR 1
#define HAVE_ISBLANK 1
/* No MCTP socket header: its HAVE_ macro stays undefined (false in #if),
 * so mi-mctp.c uses its own compatible definitions; socket(AF_MCTP) then
 * fails with EAFNOSUPPORT at run time. */
#define HAVE_NETDB 1
#define HAVE_LIBC_DLSYM 1

#define fallthrough __attribute__((__fallthrough__))

#endif /* LIKEOS_LIBNVME_CONFIG_H */
