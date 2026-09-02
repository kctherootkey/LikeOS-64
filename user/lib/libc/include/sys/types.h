#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef int64_t  off_t;
typedef int32_t  pid_t;
typedef uint32_t mode_t;
typedef int64_t  ssize_t;
typedef uint64_t size_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef long     time_t;
typedef uint32_t nlink_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t blksize_t;
typedef int64_t  blkcnt_t;

/* Signal set type (canonical definition shared with <signal.h>).  Exposed
 * here too because portable software expects sigset_t after including only
 * <sys/types.h>. */
#ifndef __likeos_sigset_t_defined
#define __likeos_sigset_t_defined
typedef unsigned long sigset_t;
#endif

/* Historic 4.4BSD short integer aliases. */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

/*
 * The System V spellings of the same three, without the underscore.
 *
 * Not standard either, and the two sets have coexisted for so long that
 * software picks whichever its author learned first -- Claws Mail's key-binding
 * code uses `uint' in a file that has compiled everywhere for twenty years.
 * Both sets are declared here for the same reason both are declared on every
 * other system: the alternative is a build that stops on a type name rather
 * than on anything meaningful.
 */
typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned long  ulong;

typedef unsigned char  u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int   u_int32_t;
typedef unsigned long  u_int64_t;

typedef int64_t  off64_t;
typedef long     suseconds_t;
/* The argument type of usleep(3) and ualarm(3): a count of microseconds,
 * unsigned and at least 32 bits.  POSIX dropped the type in 2008 along with
 * those two functions, but code written against the older standard still
 * declares variables with it, and every C library still defines it. */
typedef unsigned int useconds_t;
typedef long     clock_t;
typedef int      key_t;
typedef int      id_t;

typedef char*    caddr_t;


/* fd_set and the FD_* macros belong to <sys/select.h>, but <sys/types.h> has
 * carried them since long before that header existed, and a great deal of code
 * still expects to get them here — libXaw's text widget among it.  Included at
 * the end so the types above are already defined. */
#include <sys/select.h>

#ifdef __cplusplus
}
#endif

#endif
