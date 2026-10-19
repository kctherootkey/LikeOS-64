// LikeOS -- firmware blobs for devices that need one loaded.
//
// A display controller's power-management microcode, a graphics device's
// scheduling firmware: files the driver reads from /lib/firmware/<name>
// on the root filesystem and hands to the hardware.  So this is only
// usable once storage is mounted -- a driver that initialises earlier
// asks from its late-init hook, and treats absence as a feature it does
// without rather than a failure.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_FIRMWARE_H
#define KERNEL_KE_FIRMWARE_H

#include <kernel/uapi/types.h>

#define FIRMWARE_DIR "/lib/firmware"
/* Larger than any blob a device here wants (the biggest scheduling
 * firmware is under a megabyte); a bound, not a budget. */
#define FIRMWARE_MAX_BYTES (8u << 20)

/* Read /lib/firmware/<name> whole.  On success *data is a buffer of
 * *len bytes owned by the caller until firmware_release().  -ENOENT
 * when the file is missing, -ENODEV before the root filesystem exists,
 * -EFBIG above the bound, -ENOMEM, -EIO. */
int firmware_request(const char *name, void **data, size_t *len);
void firmware_release(void *data);

#endif
