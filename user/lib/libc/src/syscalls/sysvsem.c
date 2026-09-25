/*
 * Copyright (C) 2026 The LikeOS Project
 *
 * System V semaphore sets: the three calls, straight to the kernel.
 */
#include <sys/sem.h>
#include <errno.h>
#include <stdarg.h>
#include "syscall.h"

int semget(key_t key, int nsems, int semflg)
{
	long ret = syscall3(SYS_SEMGET, (long)key, (long)nsems, (long)semflg);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (int)ret;
}

int semop(int semid, struct sembuf *sops, size_t nsops)
{
	long ret = syscall3(SYS_SEMOP, (long)semid, (long)sops, (long)nsops);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return 0;
}

/* The fourth argument is the caller's union semun, passed by value: an
 * int, or a pointer, in one machine word either way. */
int semctl(int semid, int semnum, int cmd, ...)
{
	va_list ap;
	long arg = 0;

	switch (cmd) {
	case SETVAL:
	case GETALL:
	case SETALL:
	case IPC_STAT:
	case IPC_SET:
		va_start(ap, cmd);
		arg = va_arg(ap, long);
		va_end(ap);
		break;
	default:
		break;
	}
	long ret = syscall4(SYS_SEMCTL, (long)semid, (long)semnum, (long)cmd, arg);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return (int)ret;
}
