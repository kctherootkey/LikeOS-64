/* inotify: filesystem change events behind a descriptor (<sys/inotify.h>).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "syscall.h"
#include <sys/inotify.h>
#include <errno.h>

static long ret_or_errno(long ret)
{
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return ret;
}

int inotify_init1(int flags)
{
	return (int)ret_or_errno(syscall1(SYS_INOTIFY_INIT1, (long)flags));
}

int inotify_init(void)
{
	return inotify_init1(0);
}

int inotify_add_watch(int fd, const char *pathname, uint32_t mask)
{
	return (int)ret_or_errno(syscall3(SYS_INOTIFY_ADD_WATCH, (long)fd,
					  (long)pathname, (long)mask));
}

int inotify_rm_watch(int fd, int wd)
{
	return (int)ret_or_errno(syscall2(SYS_INOTIFY_RM_WATCH, (long)fd,
					  (long)wd));
}
