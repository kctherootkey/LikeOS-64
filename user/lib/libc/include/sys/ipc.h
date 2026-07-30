/*
 * System V IPC common definitions.
 *
 * Only what the shared memory calls need: this system has no message queues or
 * semaphores, and shm exists chiefly because the MIT-SHM X extension is built
 * on it.
 */
#ifndef _SYS_IPC_H
#define _SYS_IPC_H

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

struct ipc_perm {
    key_t          key;
    unsigned int   uid, gid;
    unsigned int   cuid, cgid;
    unsigned int   mode;
    unsigned int   seq;
};

#endif /* _SYS_IPC_H */
