// LikeOS - the device major registry: which driver owns which major number
//
// Device numbers are major:minor pairs (kernel/fs/kdev_t.h).  The fixed
// majors of the built-in devices (1 mem, 4/5 tty, 13 input, 14 sound,
// 29 fb, 136 pts, 226 drm; block 8/65-71 sd, 11 sr, 259 blkext) are known
// here from the start; a driver that needs a character major of its own
// at run time -- the NVMe controller nodes /dev/nvmeX, the generic
// namespace nodes /dev/ngXnY -- asks for a DYNAMIC one with
// chrdev_register(0, name).  Dynamic character majors come from 254 down
// to 234, then from 511 down to 384, skipping every major in use, the
// order the established allocator uses (so the numbers userland sees are
// the familiar ones).
//
// The registry is also what /proc/devices prints (chrdev_proc_devices_show,
// registered by procfs.c).
//
// Minor numbers within a major are the driver's business; struct ida
// (kernel/ke/ida.h) is the allocator to use for them.
//
// Locking: one internal spinlock; every function may be called from
// process context (none sleeps), the lookups from any context.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_CHRDEV_H
#define KERNEL_FS_CHRDEV_H

#include <kernel/uapi/types.h>

struct pfs_node;

#define CHRDEV_NAME_LEN 32
/* Dynamic character major ranges (inclusive), searched from the top. */
#define CHRDEV_DYN_FIRST 254
#define CHRDEV_DYN_LAST 234
#define CHRDEV_DYN_EXT_FIRST 511
#define CHRDEV_DYN_EXT_LAST 384
/* Registrations beyond the built-in table. */
#define CHRDEV_MAX_DYNAMIC 64

/* Register character major `major' for `name' (copied, at most
 * CHRDEV_NAME_LEN-1 characters); major 0 asks for a dynamic one.  Returns
 * the major (> 0), -EBUSY when that major is taken (by another name),
 * -ENOSPC when no dynamic major is left or the table is full, -EINVAL.
 * Registering the same major under the same name twice is allowed and
 * counted (each needs its unregister). */
int chrdev_register(uint32_t major, const char *name);
void chrdev_unregister(uint32_t major, const char *name);
/* The same for block majors (BLK_EXT_MAJOR and the fixed ones are
 * built in; a driver with a block major of its own registers it). */
int blkdev_major_register(uint32_t major, const char *name);
void blkdev_major_unregister(uint32_t major, const char *name);
/* Name of a registered major, NULL when free.  Any context; the string
 * stays valid while the major is registered. */
const char *chrdev_major_name(uint32_t major);
const char *blkdev_major_name(uint32_t major);

/* /proc/devices. */
long chrdev_proc_devices_show(struct pfs_node *n, char *buf, long cap);

#endif /* KERNEL_FS_CHRDEV_H */
