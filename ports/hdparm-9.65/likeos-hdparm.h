/*
 * likeos-hdparm.h - the kernel interface hdparm needs, from where LikeOS keeps it.
 *
 * hdparm reaches the kernel through a handful of system headers that carry the
 * fixed-width __u8 .. __u64 types, the block device requests, PATH_MAX and the
 * byte order helpers.  On LikeOS the requests live in the C library
 * (<sys/mount.h> for BLK*, <sys/hdreg.h> for HDIO_*, <scsi/sg.h> for SG_IO);
 * patch 0001 includes this file in place of those system headers.
 *
 * FIBMAP and FIGETBSZ are the two file requests of `hdparm --fibmap'.  Their
 * numbers are those of the established interface, so the program asks the
 * right question; the LikeOS kernel answers neither yet (ioctl fails with
 * ENOTTY).  --fibmap asks FIGETBSZ first and so stops with "Unable to
 * determine block size, aborting." until the kernel implements it (and then
 * FIEMAP, defined in fibmap.c, with FIBMAP as the fallback).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_HDPARM_H
#define LIKEOS_HDPARM_H

#include <stdint.h>
#include <limits.h>
#include <endian.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

#define __cpu_to_le16(x) htole16(x)
#define __cpu_to_le32(x) htole32(x)
#define __cpu_to_le64(x) htole64(x)
#define __le16_to_cpu(x) le16toh(x)
#define __le32_to_cpu(x) le32toh(x)
#define __le64_to_cpu(x) le64toh(x)

#ifndef FIBMAP
#define FIBMAP   _IO(0x00, 1)	/* file block -> device block (int *) */
#endif
#ifndef FIGETBSZ
#define FIGETBSZ _IO(0x00, 2)	/* file system block size (int *) */
#endif

#endif /* LIKEOS_HDPARM_H */
