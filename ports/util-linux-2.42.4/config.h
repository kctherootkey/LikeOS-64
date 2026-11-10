/*
 * config.h - what util-linux's configure would write, for the LikeOS libc.
 *
 * Written by hand because util-linux is built here without its autotools
 * build (see Makefile.likeos).  Every HAVE_ below was checked against the
 * LikeOS libc: the header exists, the function is declared and libc.so
 * defines it.  Anything not listed is absent and util-linux takes its
 * fallback path -- that is how its sources are written.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_UL_CONFIG_H
#define LIKEOS_UL_CONFIG_H

#define PACKAGE "util-linux"
#define PACKAGE_NAME "util-linux"
#define PACKAGE_VERSION "2.42.4"
#define PACKAGE_STRING "util-linux 2.42.4"
#define PACKAGE_TARNAME "util-linux"
#define PACKAGE_BUGREPORT "kzak@redhat.com"
#define VERSION "2.42.4"

/* Where things live.  The same values the upstream build passes on the
 * compiler command line. */
#define LOCALEDIR "/usr/share/locale"
#define _PATH_RUNSTATEDIR "/run"
#define _PATH_LOCALSTATEDIR "/var"
#define _PATH_SYSCONFDIR "/etc"
#define _PATH_SYSCONFSTATICDIR "/usr/lib"

/* The libraries of this tree that are built with it. */
#define HAVE_LIBBLKID 1
#define HAVE_LIBMOUNT 1
#define HAVE_LIBUUID 1

/* Wide characters: column widths of non-ASCII text in lsblk/fdisk output. */
#define HAVE_WIDECHAR 1

/* fdisk and friends colour their output unless told not to. */
#define USE_COLORS_BY_DEFAULT 1

/* ---- headers ---- */
#define STDC_HEADERS 1
#define HAVE_BYTESWAP_H 1
#define HAVE_DLFCN_H 1
#define HAVE_ENDIAN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_ERR_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETOPT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LANGINFO_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MNTENT_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NET_IF_H 1
#define HAVE_PATHS_H 1
#define HAVE_SEMAPHORE_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_EXT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_AUXV_H 1
#define HAVE_SYS_ENDIAN_H 1
#define HAVE_SYS_FILE_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_PRCTL_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_SIGNALFD_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSCALL_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_SYS_TTYDEFAULTS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_VFS_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1

/* ---- types, declarations, language ---- */
#define HAVE_DECL_BLK_ZONE_REP_CAPACITY 0
#define HAVE_DECL_CPU_ALLOC 0
#define HAVE_DECL_PR_REP_CAPACITY 0
#define HAVE_DECL_TZNAME 1
#define HAVE_DECL_VMADDR_CID_LOCAL 0
#define HAVE_DECL__NL_TIME_WEEK_1STDAY 0
#define HAVE_ENVIRON_DECL 1
#define HAVE_SIGHANDLER_T 1
#define HAVE_STRSIGNAL_DECL 1
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
#define HAVE_TLS 1
#define HAVE_TZNAME 1
#define HAVE_PROGRAM_INVOCATION_SHORT_NAME 1

/* ---- functions ---- */
#define HAVE_CLEARENV 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DIRFD 1
#define HAVE_ERR 1
#define HAVE_ERRX 1
#define HAVE_EXPLICIT_BZERO 1
#define HAVE_FALLOCATE 1
#define HAVE_FNMATCH 1
#define HAVE_FSEEKO 1
#define HAVE_FSTATAT 1
#define HAVE_FSYNC 1
#define HAVE_FUTIMENS 1
#define HAVE_GETAUXVAL 1
#define HAVE_GETDTABLESIZE 1
#define HAVE_GETRANDOM 1
#define HAVE_GETRLIMIT 1
#define HAVE_INOTIFY_INIT 1
#define HAVE_INOTIFY_INIT1 1
#define HAVE_JRAND48 1
#define HAVE_LGETXATTR 1
#define HAVE_LLISTXATTR 1
#define HAVE_MKOSTEMP 1
#define HAVE_NANOSLEEP 1
#define HAVE_NEWLOCALE 1
#define HAVE_OPENAT 1
#define HAVE_OPEN_MEMSTREAM 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_PRCTL 1
#define HAVE_QSORT_R 1
#define HAVE_REALLOCARRAY 1
#define HAVE_SCHED_SETSCHEDULER 1
#define HAVE_SECURE_GETENV 1
#define HAVE_SEM_CLOSE 1
#define HAVE_SETRESGID 1
#define HAVE_SETRESUID 1
#define HAVE_SHM_OPEN 1
#define HAVE_SIGQUEUE 1
#define HAVE_SRANDOM 1
#define HAVE_STRNDUP 1
#define HAVE_STRNLEN 1
#define HAVE_SYSCONF 1
#define HAVE_SYSINFO 1
#define HAVE_TIMEGM 1
#define HAVE_TIMER_CREATE 1
#define HAVE_UNLINKAT 1
#define HAVE_USELOCALE 1
#define HAVE_USLEEP 1
#define HAVE_UTIMENSAT 1
#define HAVE_VWARNX 1
#define HAVE_WARN 1
#define HAVE_WARNX 1
#define HAVE___FPENDING 1
#define HAVE___FPURGE 1

#endif /* LIKEOS_UL_CONFIG_H */
