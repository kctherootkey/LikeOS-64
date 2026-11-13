/*
 * <linux/fs.h> -- the block device requests, at the include path programs
 * that query disks use for them (BLKGETSIZE64, BLKSSZGET, BLKRRPART,
 * BLKDISCARD, BLKPG ...).
 *
 * On LikeOS the block device interface lives in <sys/mount.h> (a byte-for-
 * byte copy of the kernel's own definition); this header forwards to it and
 * to the fixed-width types, so both include paths give the same numbers.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_UAPI_FS_H
#define _LIKEOS_UAPI_FS_H

#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#endif /* _LIKEOS_UAPI_FS_H */
