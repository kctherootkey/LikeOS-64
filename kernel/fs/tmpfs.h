// LikeOS -- tmpfs: a filesystem kept entirely in RAM, mounted at /ram.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_TMPFS_H
#define LIKEOS_TMPFS_H

/* Where the kernel mounts its boot-time instance.  Anything created below
 * this path lives in memory and is gone at the next boot. */
#define TMPFS_MOUNT "/ram"

/* Register the "tmpfs" filesystem type with the mount table and mount the
 * boot-time instance at TMPFS_MOUNT.  Called once at boot, after
 * vfs_init(); needs the physical allocator, nothing else.  mount(2) makes
 * further instances; anything created in one lives in memory and is gone
 * at the next boot. */
void tmpfs_init(void);

/* The filesystem type mount(2) instantiates for "tmpfs". */
#include <kernel/fs/vfs.h>
extern const vfs_fstype_t tmpfs_fstype;

#endif /* LIKEOS_TMPFS_H */
