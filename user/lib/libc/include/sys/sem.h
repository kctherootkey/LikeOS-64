/*
 * Copyright (C) 2026 The LikeOS Project
 *
 * System V semaphore sets.  The fourth argument of semctl() is the
 * caller's union semun, which this header does not define: the program
 * declares it, as the standard has it.
 */
#ifndef _SYS_SEM_H
#define _SYS_SEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/ipc.h>
#include <sys/types.h>
#include <stddef.h>

/* the operation flag, with IPC_NOWAIT from <sys/ipc.h> */
#define SEM_UNDO 0x1000

/* the commands, with IPC_STAT, IPC_SET and IPC_RMID from <sys/ipc.h> */
#define GETPID  11
#define GETVAL  12
#define GETALL  13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL  16
#define SETALL  17

/* the limits */
#define SEMMNI 64    /* sets */
#define SEMMSL 64    /* semaphores per set */
#define SEMOPM 32    /* operations per semop() call */
#define SEMVMX 32767 /* the largest value */

struct sembuf {
    unsigned short sem_num; /* which semaphore of the set */
    short          sem_op;  /* the operation */
    short          sem_flg; /* IPC_NOWAIT, SEM_UNDO */
};

struct semid_ds {
    struct ipc_perm sem_perm;
    long            sem_otime; /* last semop() */
    long            sem_ctime; /* last change */
    unsigned long   sem_nsems; /* semaphores in the set */
};

int semget(key_t key, int nsems, int semflg);
int semop(int semid, struct sembuf *sops, size_t nsops);
int semctl(int semid, int semnum, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SEM_H */
