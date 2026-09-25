/*
 * System V IPC common definitions.
 *
 * Only what the shared memory calls need: this system has no message queues or
 * semaphores, and shm exists chiefly because the MIT-SHM X extension is built
 * on it.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

typedef int key_t;

#define IPC_PRIVATE ((key_t)0)

/* Flags for the *get() calls (combined with permission bits). */
#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000

/* Commands for the *ctl() calls. */
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2

/* A key from a file and a project id: the file's inode and device with
 * the id, as every implementation has made it. */
key_t ftok(const char *path, int id);

struct ipc_perm {
    key_t          key;
    unsigned int   uid, gid;
    unsigned int   cuid, cgid;
    unsigned int   mode;
    unsigned int   seq;
};

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IPC_H */
