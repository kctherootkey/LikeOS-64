/*
 * LikeOS-64 POSIX Threads (pthreads) Header
 * 
 * Provides threading primitives: threads, mutexes, condition variables,
 * read-write locks, spinlocks, barriers, and thread-specific data.
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

// Static initializers
#define PTHREAD_MUTEX_INITIALIZER     { 0, 0, 0, PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_STALLED, PTHREAD_PROCESS_PRIVATE, {0} }
#define PTHREAD_COND_INITIALIZER      { 0, 0, PTHREAD_PROCESS_PRIVATE, 0 }
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

#ifdef __cplusplus
}
#endif

#endif /* _PTHREAD_H */
