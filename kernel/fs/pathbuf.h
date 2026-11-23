// LikeOS -- path buffers: where the kernel keeps a whole path name.
//
// A path is up to VFS_MAX_PATH bytes (PATH_MAX, the terminating NUL
// included), which is a quarter of a kernel stack.  So no path ever lives on
// the stack: every buffer that holds a whole path -- a copy of the caller's
// argument, the path made absolute against the working directory, the target
// of a symbolic link, the path a link leads to -- comes from here and goes
// back here.  Single names between two slashes (VFS_NAME_MAX) may still be
// kept on the stack.
//
//   char *p = pathbuf_get();        PATHBUF_SIZE bytes, or NULL (ENOMEM)
//   ...
//   pathbuf_put(p);                 NULL is fine
//
// Context: both never sleep and may be called from any context -- with
// spinlocks held, with interrupts off -- except from NMI context.  The
// contents of a new buffer are undefined (not zeroed).  A buffer is owned by
// whoever got it until it is put back; putting one twice, or one that did not
// come from pathbuf_get(), corrupts the free list (DEBUG builds check).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_PATHBUF_H
#define KERNEL_FS_PATHBUF_H

#include <kernel/fs/vfs.h>

#define PATHBUF_SIZE VFS_MAX_PATH

char *pathbuf_get(void);
void pathbuf_put(char *buf);

/* Buffers handed out and not put back yet (for leak checks in tests). */
unsigned long pathbuf_in_use(void);

/* Fill the reserve the first buffers come from (vfs_init); optional. */
void pathbuf_init(void);

#endif /* KERNEL_FS_PATHBUF_H */
