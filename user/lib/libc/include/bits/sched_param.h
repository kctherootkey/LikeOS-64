/*
 * The scheduling parameters of a thread or process and the processor set
 * type (POSIX <sched.h>).  Kept apart so that the pthread attribute type,
 * which embeds both and which <sys/types.h> must also define, can be
 * declared without <sched.h>.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _BITS_SCHED_PARAM_H
#define _BITS_SCHED_PARAM_H

struct sched_param {
    int sched_priority;
};

/* A set of processors, for the affinity calls; the macros that work on
 * one are in <sched.h>. */
#define CPU_SETSIZE 64

typedef struct {
    unsigned long bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

#endif
