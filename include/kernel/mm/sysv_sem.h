/*
 * Copyright (C) 2026 The LikeOS Project
 *
 * System V semaphore sets: semget(2), semop(2), semctl(2).
 *
 * A set is a small array of counting semaphores under one identifier,
 * named by a key or private, with the same owner/group/mode permissions
 * as the other System V objects.  semop() applies its whole array of
 * operations atomically or sleeps until it can; semctl() reads and sets
 * values and removes sets.  An operation made with SEM_UNDO is reversed
 * when the process ends.
 */
#ifndef _KERNEL_MM_SYSV_SEM_H_
#define _KERNEL_MM_SYSV_SEM_H_

#include <kernel/ke/syscall.h>

/* the commands beyond IPC_STAT/IPC_SET/IPC_RMID, and the operation flag */
#define SEM_GETPID 11
#define SEM_GETVAL 12
#define SEM_GETALL 13
#define SEM_GETNCNT 14
#define SEM_GETZCNT 15
#define SEM_SETVAL 16
#define SEM_SETALL 17
#define SEM_UNDO 0x1000

#define SEM_MAX_SETS 64 /* SEMMNI */
#define SEM_MAX_PER_SET 64 /* SEMMSL */
#define SEM_MAX_OPS 32 /* SEMOPM */
#define SEM_VALUE_MAX 32767 /* SEMVMX */

/* as the C library lays them out */
struct k_sembuf {
	uint16_t sem_num;
	int16_t sem_op;
	int16_t sem_flg;
};

struct k_semid_ds {
	struct k_ipc_perm sem_perm;
	int64_t sem_otime;
	int64_t sem_ctime;
	uint64_t sem_nsems;
};

struct task;

/* A process is gone: its SEM_UNDO adjustments are applied, its waits
 * are over. */
void sysv_sem_exit(struct task *task);

#endif
