/*
 * System V shared memory syscall wrappers.  See include/sys/shm.h.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <sys/shm.h>
#include <sys/stat.h>
#include <errno.h>
#include "syscall.h"

key_t ftok(const char *path, int id)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return (key_t)-1;
	return (key_t)(((unsigned)id & 0xffu) << 24 | ((unsigned)st.st_dev & 0xffu) << 16 |
		       ((unsigned)st.st_ino & 0xffffu));
}

int shmget(key_t key, size_t size, int shmflg)
{
	long ret = syscall3(SYS_SHMGET, (long)key, (long)size, (long)shmflg);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (int)ret;
}

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
	long ret = syscall3(SYS_SHMAT, (long)shmid, (long)shmaddr,
			    (long)shmflg);
	/* Failure is reported as (void *)-1, as everywhere else; the error
	 * range is the usual last-page-of-errnos convention. */
	if (ret < 0 && ret > -4096) {
		errno = (int)-ret;
		return (void *)-1;
	}
	return (void *)ret;
}

int shmdt(const void *shmaddr)
{
	long ret = syscall1(SYS_SHMDT, (long)shmaddr);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	long ret = syscall3(SYS_SHMCTL, (long)shmid, (long)cmd, (long)buf);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (int)ret;
}
