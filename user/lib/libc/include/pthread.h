/*
 * LikeOS POSIX Threads (pthreads) Header
 * 
 * Provides threading primitives: threads, mutexes, condition variables,
 * read-write locks, spinlocks, barriers, and thread-specific data.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sched.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The types (and the constants their fields take) are in
// <bits/pthreadtypes.h>, shared with <sys/types.h>.
#include <bits/pthreadtypes.h>

// Static initializers
#define PTHREAD_MUTEX_INITIALIZER     { 0, 0, 0, PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_STALLED, PTHREAD_PROCESS_PRIVATE, {0} }
#define PTHREAD_COND_INITIALIZER      { 0, 0, PTHREAD_PROCESS_PRIVATE, 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER    { 0, 0, 0, 0, PTHREAD_PROCESS_PRIVATE }

// Cancel state/type (stub values - cancellation not fully implemented)
#define PTHREAD_CANCEL_ENABLE  0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void*)-1)

// Scope
#define PTHREAD_SCOPE_SYSTEM  0
#define PTHREAD_SCOPE_PROCESS 1

// Inherit scheduler
#define PTHREAD_INHERIT_SCHED  0
#define PTHREAD_EXPLICIT_SCHED 1

// ============================================================================
// THREAD FUNCTIONS
// ============================================================================

// Thread creation and management
int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg);
void pthread_exit(void* retval) __attribute__((noreturn));
int pthread_join(pthread_t thread, void** retval);
/* Join without blocking: EBUSY while the thread still runs. */
int pthread_tryjoin_np(pthread_t thread, void** retval);
/* Handlers run around fork(): `prepare' in the parent before, `parent' and
 * `child' after, in the respective process. */
int pthread_atfork(void (*prepare)(void), void (*parent)(void),
		   void (*child)(void));
/* The clock that measures `thread`'s CPU consumption; pass the result to
 * clock_gettime().  POSIX interface; the id stays meaningful for the
 * thread's lifetime and answers EINVAL afterwards. */
int pthread_getcpuclockid(pthread_t thread, clockid_t *clockid);
/* Thread names, 15 characters + terminator.  Stored in the thread's own
 * control block; the whole process sees one namespace of them. */
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char *name, size_t len);
int pthread_detach(pthread_t thread);

// Thread identity
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);

// Thread attributes
int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setdetachstate(pthread_attr_t* attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detachstate);
int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* stacksize);
int pthread_attr_setstack(pthread_attr_t* attr, void* stackaddr, size_t stacksize);
int pthread_attr_getstack(const pthread_attr_t* attr, void** stackaddr, size_t* stacksize);
int pthread_attr_setguardsize(pthread_attr_t* attr, size_t guardsize);
int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize);
int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy);
int pthread_attr_getschedpolicy(const pthread_attr_t* attr, int* policy);
int pthread_attr_setschedparam(pthread_attr_t* attr, const struct sched_param* param);
int pthread_attr_getschedparam(const pthread_attr_t* attr, struct sched_param* param);
int pthread_attr_setinheritsched(pthread_attr_t* attr, int inheritsched);
int pthread_attr_getinheritsched(const pthread_attr_t* attr, int* inheritsched);
int pthread_attr_setscope(pthread_attr_t* attr, int scope);
int pthread_attr_getscope(const pthread_attr_t* attr, int* scope);

// Scheduling
int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param* param);
int pthread_getschedparam(pthread_t thread, int* policy, struct sched_param* param);
int pthread_setschedprio(pthread_t thread, int prio);

// CPU affinity (non-portable extension)
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t* cpuset);
int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* cpuset);
int pthread_attr_setaffinity_np(pthread_attr_t* attr, size_t cpusetsize, const cpu_set_t* cpuset);
int pthread_attr_getaffinity_np(const pthread_attr_t* attr, size_t cpusetsize, cpu_set_t* cpuset);

// Once initialization
int pthread_once(pthread_once_t* once_control, void (*init_routine)(void));

// ============================================================================
// MUTEX FUNCTIONS
// ============================================================================

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abstime);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutex_consistent(pthread_mutex_t* mutex);

// Mutex attributes
int pthread_mutexattr_init(pthread_mutexattr_t* attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t* attr);
int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type);
int pthread_mutexattr_setrobust(pthread_mutexattr_t* attr, int robust);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t* attr, int* robust);
int pthread_mutexattr_setpshared(pthread_mutexattr_t* attr, int pshared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t* attr, int* pshared);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t* attr, int protocol);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t* attr, int* protocol);
int pthread_mutexattr_setprioceiling(pthread_mutexattr_t* attr, int prioceiling);
int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t* attr, int* prioceiling);

// ============================================================================
// CONDITION VARIABLE FUNCTIONS
// ============================================================================

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);

// Condition variable attributes
int pthread_condattr_init(pthread_condattr_t* attr);
int pthread_condattr_destroy(pthread_condattr_t* attr);
int pthread_condattr_setpshared(pthread_condattr_t* attr, int pshared);
int pthread_condattr_getpshared(const pthread_condattr_t* attr, int* pshared);
int pthread_condattr_setclock(pthread_condattr_t* attr, int clock_id);
int pthread_condattr_getclock(const pthread_condattr_t* attr, int* clock_id);

// ============================================================================
// READ-WRITE LOCK FUNCTIONS
// ============================================================================

int pthread_rwlock_init(pthread_rwlock_t* rwlock, const pthread_rwlockattr_t* attr);
int pthread_rwlock_destroy(pthread_rwlock_t* rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_timedrdlock(pthread_rwlock_t* rwlock, const struct timespec* abstime);
int pthread_rwlock_wrlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t* rwlock);
int pthread_rwlock_timedwrlock(pthread_rwlock_t* rwlock, const struct timespec* abstime);
int pthread_rwlock_unlock(pthread_rwlock_t* rwlock);

// Read-write lock attributes
int pthread_rwlockattr_init(pthread_rwlockattr_t* attr);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t* attr);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t* attr, int pshared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t* attr, int* pshared);

// ============================================================================
// SPINLOCK FUNCTIONS
// ============================================================================

int pthread_spin_init(pthread_spinlock_t* lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t* lock);
int pthread_spin_lock(pthread_spinlock_t* lock);
int pthread_spin_trylock(pthread_spinlock_t* lock);
int pthread_spin_unlock(pthread_spinlock_t* lock);

// ============================================================================
// BARRIER FUNCTIONS
// ============================================================================

int pthread_barrier_init(pthread_barrier_t* barrier,
                         const pthread_barrierattr_t* attr, unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t* barrier);
int pthread_barrier_wait(pthread_barrier_t* barrier);

// Barrier attributes
int pthread_barrierattr_init(pthread_barrierattr_t* attr);
int pthread_barrierattr_destroy(pthread_barrierattr_t* attr);
int pthread_barrierattr_setpshared(pthread_barrierattr_t* attr, int pshared);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t* attr, int* pshared);

// ============================================================================
// THREAD-SPECIFIC DATA FUNCTIONS
// ============================================================================

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void* value);
void* pthread_getspecific(pthread_key_t key);

// ============================================================================
// CANCELLATION (STUB SUPPORT)
// ============================================================================

int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int* oldstate);
int pthread_setcanceltype(int type, int* oldtype);
void pthread_testcancel(void);

#include <signal.h>
/* Per-thread signal mask (threads are tasks here, so this is the calling
 * thread's own mask, reported the pthread way: return value, not errno). */
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset);

/* Direct a signal at ONE thread of this process, rather than at the process.
 * The pthread way of reporting: the error number comes back as the return
 * value and errno is left alone.  sig 0 probes that the thread still exists. */
int pthread_kill(pthread_t thread, int sig);

/* Report a live thread's actual attributes -- most importantly the stack's
 * base and size, which is how a garbage collector or an unwinder learns where
 * the stack it must walk begins and ends.  Works for any thread including the
 * main one, whose stack the kernel set up long before this library ran. */
int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */
