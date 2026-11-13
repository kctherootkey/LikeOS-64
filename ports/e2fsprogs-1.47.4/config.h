/*
 * config.h - what e2fsprogs' configure would write into lib/config.h and
 * lib/dirpaths.h, for the LikeOS libc.
 *
 * Written by hand because e2fsprogs is built here without its autotools
 * build (see Makefile.likeos).  Every HAVE_ below was checked against the
 * LikeOS libc: the header exists, the function is declared and libc.so
 * defines it.  Anything not listed is absent and e2fsprogs takes its
 * fallback path -- that is how its sources are written.  The notable
 * absences, and what e2fsprogs does instead:
 *
 *   no *64 entry points (open64, lseek64, pread64, fstat64, ...): off_t is
 *       64 bits wide, the plain calls are the large-file calls
 *   no posix_fadvise / POSIX_FADV_*: the unix I/O manager's readahead hint
 *       reports EXT2_ET_OP_NOT_SUPPORTED and e2fsck reads without it
 *   no backtrace(): e2fsck's fatal-signal report prints no stack
 *   no EXT2_IOC_* / FS_IOC_* file flag or online resize requests in the
 *       kernel: HAVE_EXT2_IOCTLS stays off, lsattr-style flag reads report
 *       "operation not supported", resize2fs on a mounted file system says
 *       that on-line resizing is not supported
 *   no <linux/fd.h>, <linux/loop.h>, <linux/major.h>, <linux/fsmap.h>,
 *       <linux/fsverity.h>: the floppy, loop, fsmap and verity paths are
 *       compiled out.  <linux/falloc.h> is there: zero-out and discard on
 *       image files (and zero-out on block devices) use fallocate()'s
 *       ZERO_RANGE / PUNCH_HOLE modes
 *
 * libuuid and libblkid are util-linux's (ports/util-linux-2.42.4), as on
 * the distributions: e2fsprogs' own copies are not built.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_E2FS_CONFIG_H
#define LIKEOS_E2FS_CONFIG_H

#define PACKAGE "e2fsprogs"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME ""
#define PACKAGE_STRING ""
#define PACKAGE_TARNAME ""
#define PACKAGE_URL ""
#define PACKAGE_VERSION ""
#define VERSION "0.14.1"

/* lib/dirpaths.h */
#define LOCALEDIR "/usr/share/locale"
#define ROOT_SYSCONFDIR "/etc"

/* ---- the configure feature switches, at their upstream defaults ---- */
#define CONFIG_MMP 1
#define CONFIG_TDB 1
#define CONFIG_TESTIO_DEBUG 1
#define ENABLE_BMAP_STATS 1
/* No libarchive: mke2fs -d takes a directory, not a tarball. */
#define CONFIG_DISABLE_LIBARCHIVE 1

/* util-linux's libblkid: the probing and topology entry points mke2fs and
 * e2fsck look for. */
#define HAVE_BLKID_PROBE_GET_TOPOLOGY 1
#define HAVE_BLKID_PROBE_ENABLE_PARTITIONS 1
#define HAVE_BLKID_TOPOLOGY_GET_DAX 1

/* ---- headers ---- */
#define STDC_HEADERS 1
#define HAVE_DIRENT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_GETOPT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LINUX_TYPES_H 1
#define HAVE_MALLOC_H 1
#define HAVE_MNTENT_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NET_IF_H 1
#define HAVE_PATHS_H 1
#define HAVE_PTHREAD_H 1
#define HAVE_PWD_H 1
#define HAVE_SEMAPHORE_H 1
#define HAVE_SETJMP_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_FILE_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_SYS_PRCTL_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSCALL_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_WCHAR_H 1

/* ---- functions ---- */
#define HAVE_DLOPEN 1
#define HAVE_FALLOCATE 1
#define HAVE_LINUX_FALLOC_H 1
#define HAVE_FCHOWN 1
#define HAVE_FCNTL 1
#define HAVE_FDATASYNC 1
#define HAVE_FSYNC 1
#define HAVE_GETCWD 1
#define HAVE_GETDTABLESIZE 1
#define HAVE_GETENTROPY 1
#define HAVE_GETHOSTNAME 1
#define HAVE_GETPAGESIZE 1
#define HAVE_GETPWUID_R 1
#define HAVE_GETRANDOM 1
#define HAVE_GETRLIMIT 1
#define HAVE_GETRUSAGE 1
#define HAVE_GNU_QSORT_R 1
#define HAVE_QSORT_R 1
#define HAVE_JRAND48 1
#define HAVE_LLISTXATTR 1
#define HAVE_MALLINFO 1
#define HAVE_MALLINFO2 1
#define HAVE_MBSTOWCS 1
#define HAVE_MEMALIGN 1
#define HAVE_MMAP 1
#define HAVE_MSYNC 1
#define HAVE_NANOSLEEP 1
#define HAVE_PATHCONF 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_PRCTL 1
#define HAVE_PREAD 1
#define HAVE_PTHREAD 1
#define HAVE_PWRITE 1
#define HAVE_SECURE_GETENV 1
#define HAVE_SEM_INIT 1
#define HAVE_SETMNTENT 1
#define HAVE_SETRESGID 1
#define HAVE_SETRESUID 1
#define HAVE_SNPRINTF 1
#define HAVE_SRANDOM 1
#define HAVE_STPCPY 1
#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRNLEN 1
#define HAVE_STRPTIME 1
#define HAVE_STRTOULL 1
#define HAVE_SYSCONF 1
#define HAVE_USLEEP 1
#define HAVE_UTIME 1
#define HAVE_UTIMES 1
#define HAVE_VALLOC 1
#define HAVE_VPRINTF 1

/* ---- types and structures ---- */
#define HAVE_INTPTR_T 1
#define HAVE_TYPE_SSIZE_T 1
#define HAVE_RECLEN_DIRENT 1
#define HAVE_STRUCT_STAT_ST_ATIM 1

#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_SHORT 2
#define SIZEOF_TIME_T 8
#define SIZEOF_VOID_P 8

/* x86-64: little-endian, WORDS_BIGENDIAN stays undefined. */

/* Thread-local storage (com_err's message buffer). */
#define TLS _Thread_local

#endif /* LIKEOS_E2FS_CONFIG_H */
