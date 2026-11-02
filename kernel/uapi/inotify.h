// LikeOS -- user-visible constants and the record read() returns for an
// inotify descriptor.  Mirrored by the libc's <sys/inotify.h>; the values and
// the record layout are the conventional ones, so programs written against
// the usual interface (GLib's file monitor among them) work unchanged.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_UAPI_INOTIFY_H
#define KERNEL_UAPI_INOTIFY_H

#include <kernel/uapi/types.h>

/* One event.  `len' counts the name field including its terminating NUL and
 * the zero padding that follows it, so the next record starts at
 * sizeof(struct inotify_event) + len, always a multiple of 16. */
struct inotify_event {
	int32_t wd;
	uint32_t mask;
	uint32_t cookie;
	uint32_t len;
	char name[];
};

/* Events. */
#define IN_ACCESS 0x00000001 /* read */
#define IN_MODIFY 0x00000002 /* written or truncated */
#define IN_ATTRIB 0x00000004 /* metadata: mode, owner, times, links, xattrs */
#define IN_CLOSE_WRITE 0x00000008 /* closed, had been opened for writing */
#define IN_CLOSE_NOWRITE 0x00000010 /* closed, had been opened read-only */
#define IN_OPEN 0x00000020
#define IN_MOVED_FROM 0x00000040 /* a name left the watched directory */
#define IN_MOVED_TO 0x00000080 /* a name arrived in the watched directory */
#define IN_CREATE 0x00000100
#define IN_DELETE 0x00000200
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF 0x00000800

#define IN_CLOSE (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_MOVE (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_ALL_EVENTS                                                     \
	(IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE |             \
	 IN_CLOSE_NOWRITE | IN_OPEN | IN_MOVED_FROM | IN_MOVED_TO |       \
	 IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF)

/* Reported, never asked for. */
#define IN_UNMOUNT 0x00002000
#define IN_Q_OVERFLOW 0x00004000 /* events were dropped (wd is -1) */
#define IN_IGNORED 0x00008000 /* the watch is gone */
#define IN_ISDIR 0x40000000 /* the subject is a directory */

/* inotify_add_watch() flags. */
#define IN_ONLYDIR 0x01000000
#define IN_DONT_FOLLOW 0x02000000
#define IN_EXCL_UNLINK 0x04000000
#define IN_MASK_CREATE 0x10000000
#define IN_MASK_ADD 0x20000000
#define IN_ONESHOT 0x80000000

/* inotify_init1() flags. */
#define IN_CLOEXEC 02000000
#define IN_NONBLOCK 04000

#endif
