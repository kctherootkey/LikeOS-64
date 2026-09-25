/*
 * The POSIX thread types: threads, attributes, mutexes, condition
 * variables, read-write locks, spinlocks, barriers, keys and once
 * controls, with the constants their fields take.
 *
 * <pthread.h> is where they belong; <sys/types.h> must define them as well
 * (POSIX lists every one of them there), and software that includes only
 * that header and X11's counts on it.  So they live here, included by both.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _BITS_PTHREADTYPES_H
#define _BITS_PTHREADTYPES_H

#include <stddef.h>
#include <stdint.h>
#include <bits/sched_param.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TYPE DEFINITIONS
// ============================================================================

// Thread identifier (pointer to thread control block)
typedef struct __pthread* pthread_t;

// Thread attributes
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    int     detachstate;
    size_t  stacksize;
    void*   stackaddr;
    size_t  guardsize;
    int     scope;
    int     inheritsched;
    int     schedpolicy;
    struct sched_param schedparam;
    cpu_set_t cpuset;
    int     cpuset_valid;
} pthread_attr_t;

// Mutex types
#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

// Mutex protocol (for priority inheritance)
#define PTHREAD_PRIO_NONE    0
#define PTHREAD_PRIO_INHERIT 1
#define PTHREAD_PRIO_PROTECT 2

// Robust mutex constants
#define PTHREAD_MUTEX_STALLED   0
#define PTHREAD_MUTEX_ROBUST    1

// Process-shared attribute
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

// Robust futex list entry (must match kernel expectations)
struct robust_list {
    struct robust_list* next;
};

struct robust_list_head {
    struct robust_list list;
    long futex_offset;
    struct robust_list* list_op_pending;
};

// Mutex structure
// State: 0 = unlocked, 1 = locked (no waiters), 2 = locked (waiters)
typedef struct {
    volatile int    state;          // Futex word: 0=unlocked, 1=locked, 2=contended
    volatile int    owner_tid;      // Owner thread ID (for recursive/errorcheck)
    volatile int    count;          // Recursion count
    int             type;           // PTHREAD_MUTEX_NORMAL/RECURSIVE/ERRORCHECK
    int             robust;         // PTHREAD_MUTEX_STALLED/ROBUST
    int             pshared;        // PTHREAD_PROCESS_PRIVATE/SHARED
    struct robust_list robust_node; // Node in robust list (for automatic cleanup)
} pthread_mutex_t;

// Mutex attributes
typedef struct {
    int type;
    int robust;
    int pshared;
    int protocol;
    int prioceiling;
} pthread_mutexattr_t;

// Condition variable
// Uses a sequence number for wait/signal coordination
typedef struct {
    volatile unsigned int seq;      // Sequence number (futex word)
    volatile int waiters;           // Number of waiters
    int pshared;                    // Process-shared attribute
    /* Clock the deadline passed to pthread_cond_timedwait() is measured
     * against, from pthread_condattr_setclock(); 0 (CLOCK_REALTIME) is the
     * default POSIX requires.  It sits in what was padding, so the struct is
     * the same size and layout as before and nothing has to be rebuilt.
     *
     * Honouring it matters more than it looks: a caller that selects
     * CLOCK_MONOTONIC passes deadlines measured in time since boot, and
     * comparing those against the wall clock puts every deadline decades in
     * the past -- so every timed wait returns ETIMEDOUT immediately. */
    int clock_id;
    pthread_mutex_t* mutex;         // Associated mutex (for broadcast)
} pthread_cond_t;

// Condition variable attributes
typedef struct {
    int pshared;
    int clock_id;
} pthread_condattr_t;

// Read-write lock
// State encoding: 0 = unlocked, >0 = reader count, -1 = write-locked
typedef struct {
    volatile int state;             // 0=unlocked, >0=readers, -1=writer
    volatile int waiters_readers;   // Pending readers
    volatile int waiters_writers;   // Pending writers
    volatile int writer_tid;        // Writer thread ID
    int pshared;
} pthread_rwlock_t;

// Read-write lock attributes
typedef struct {
    int pshared;
} pthread_rwlockattr_t;

// Spinlock (simple atomic flag)
typedef volatile int pthread_spinlock_t;

// Barrier
typedef struct {
    volatile unsigned int count;        // Current count
    volatile unsigned int generation;   // Generation counter
    unsigned int threshold;             // Required count
    int pshared;
} pthread_barrier_t;

// Barrier attributes  
typedef struct {
    int pshared;
} pthread_barrierattr_t;

// Return value for pthread_barrier_wait() for exactly one thread
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

// Thread-specific data key
typedef unsigned int pthread_key_t;

// Maximum number of TSD keys
#define PTHREAD_KEYS_MAX 128

// Once control
typedef volatile int pthread_once_t;
#define PTHREAD_ONCE_INIT 0


#ifdef __cplusplus
}
#endif

#endif
