#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

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

/* Historic 4.4BSD short integer aliases. */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

typedef unsigned char  u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int   u_int32_t;
typedef unsigned long  u_int64_t;

typedef int64_t  off64_t;
typedef long     suseconds_t;
typedef long     clock_t;
typedef int      key_t;
typedef int      id_t;

typedef char*    caddr_t;

#endif
