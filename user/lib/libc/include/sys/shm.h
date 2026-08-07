/*
 * System V shared memory.
 *
 * These share their objects with the POSIX shm_open() interface — a segment is
 * the same thing however it was created, and both show up under /dev/shm.  The
 * identifier shmget() returns means the same thing in every process, which is
 * what lets one process create a segment and hand the id to another (exactly
 * how the MIT-SHM X extension works).
 */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/ipc.h>
#include <sys/types.h>
#include <stddef.h>

/* shmat() flags */
#define SHM_RDONLY 010000
#define SHM_RND    020000

typedef unsigned long shmatt_t;

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t          shm_segsz;   /* size in bytes */
    long            shm_atime;
    long            shm_dtime;
    long            shm_ctime;
    int             shm_cpid;    /* creator */
    int             shm_lpid;    /* last attach/detach */
    shmatt_t        shm_nattch;  /* current attaches */
};

int   shmget(key_t key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int   shmdt(const void *shmaddr);
int   shmctl(int shmid, int cmd, struct shmid_ds *buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SHM_H */
