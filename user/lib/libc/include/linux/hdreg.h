/*
 * <linux/hdreg.h> -- the hard-disk requests (HDIO_GETGEO, HDIO_GET_IDENTITY,
 * HDIO_DRIVE_CMD ...) and struct hd_geometry, at the include path programs
 * that query a disk's geometry use for them (mkfs.fat, partitioners).
 *
 * On LikeOS the interface lives in <sys/hdreg.h> (a byte-for-byte copy of
 * the kernel's own definition); this header forwards to it, so both include
 * paths give the same numbers and layouts.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_UAPI_HDREG_PATH_H
#define _LIKEOS_UAPI_HDREG_PATH_H

#include <sys/hdreg.h>

#endif /* _LIKEOS_UAPI_HDREG_PATH_H */
