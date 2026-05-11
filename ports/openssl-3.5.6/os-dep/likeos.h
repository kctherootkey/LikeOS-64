/*
 * LikeOS-64 platform shim for OpenSSL 3.x
 *
 * Included via -include in every compilation unit when building with the
 * likeos-x86_64 Configure target.  Provides the handful of POSIX/Linux-isms
 * that OpenSSL assumes but that LikeOS exposes under slightly different names
 * or that need small compile-time adjustments.
 */

#ifndef _OPENSSL_OS_DEP_LIKEOS_H
#define _OPENSSL_OS_DEP_LIKEOS_H

/* Pull in our own libc headers first so they win over any stale system
 * headers that gcc might find on the build host. */
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>

/* PATH_MAX might not be defined on all LikeOS header variants */
#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* OpenSSL uses MAXPATHLEN in some paths */
#ifndef MAXPATHLEN
#  define MAXPATHLEN PATH_MAX
#endif

/* Needed by threads code */
#ifndef _REENTRANT
#  define _REENTRANT 1
#endif

/* getrandom flags (mirrors <sys/random.h> on many platforms) */
#ifndef GRND_NONBLOCK
#  define GRND_NONBLOCK 0x0001u
#endif
#ifndef GRND_RANDOM
#  define GRND_RANDOM   0x0002u
#endif

/* Some OpenSSL internals test for a writable /dev/urandom via stat(). */
#define DEVRANDOM "/dev/urandom"

/* Suppress AF_ALG and kernel TLS paths */
#undef OPENSSL_HAVE_DEVRANDOM_WAIT
#define OPENSSL_DEVRANDOM "/dev/urandom"

/* We do not have getcontext/makecontext/swapcontext (used by no-async) */
#undef OPENSSL_SYS_LINUX

/* Identify the platform for OpenSSL's feature guards */
#define OPENSSL_SYS_LIKEOS  1

#endif /* _OPENSSL_OS_DEP_LIKEOS_H */
