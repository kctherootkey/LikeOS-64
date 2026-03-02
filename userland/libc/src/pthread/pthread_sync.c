/*
 * LikeOS-64 POSIX Threads - Synchronization Primitives
 *
 * Read-write locks, spinlocks, and barriers.
 */

#include "../../include/pthread.h"
#include "../../include/sched.h"
#include "../../include/errno.h"
#include "../syscalls/syscall.h"
#include "pthread_internal.h"

extern int errno;

// Futex operations
extern int futex_wait(volatile int* uaddr, int val, const struct timespec* timeout);
extern int futex_wake(volatile int* uaddr, int count);

// ============================================================================
// READ-WRITE LOCKS
// ============================================================================

/*
 * RWLock state encoding:
 *   state > 0  : number of active readers
 *   state == 0 : unlocked
 *   state == -1: write-locked
 *
 * Waiters are tracked separately and use futex for blocking.
 * We use a simple writer-preference policy to prevent writer starvation.
 */

#define RWLOCK_UNLOCKED     0
#define RWLOCK_WRITELOCKED  (-1)

// Atomic operations
static inline int __atomic_cas(volatile int* ptr, int old_val, int new_val) {
    return __sync_val_compare_and_swap(ptr, old_val, new_val);
}

static inline int __atomic_add(volatile int* ptr, int val) {
    return __sync_fetch_and_add(ptr, val);
}

static inline int __atomic_sub(volatile int* ptr, int val) {
    return __sync_fetch_and_sub(ptr, val);
}

// RWLock Attributes

int pthread_rwlockattr_init(pthread_rwlockattr_t* attr) {
    if (!attr) return EINVAL;
    attr->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t* attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t* attr, int pshared) {
    if (!attr) return EINVAL;
    if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attr->pshared = pshared;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t* attr, int* pshared) {
    if (!attr || !pshared) return EINVAL;
    *pshared = attr->pshared;
    return 0;
}

// RWLock Operations

int pthread_rwlock_init(pthread_rwlock_t* rwlock, const pthread_rwlockattr_t* attr) {
    if (!rwlock) return EINVAL;
    
    rwlock->state = RWLOCK_UNLOCKED;
    rwlock->waiters_readers = 0;
    rwlock->waiters_writers = 0;
    rwlock->writer_tid = 0;
    
    if (attr) {
        rwlock->pshared = attr->pshared;
    } else {
        rwlock->pshared = PTHREAD_PROCESS_PRIVATE;
    }
    
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t* rwlock) {
    if (!rwlock) return EINVAL;
    
    if (rwlock->state != RWLOCK_UNLOCKED) {
        return EBUSY;
    }
    
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t* rwlock) {
    if (!rwlock) return EINVAL;
    
    while (1) {
        int state = rwlock->state;
        
        // Can acquire read lock if:
        // - Not write-locked
        // - No waiting writers (writer preference)
        if (state >= 0 && rwlock->waiters_writers == 0) {
            if (__atomic_cas(&rwlock->state, state, state + 1) == state) {
                return 0;
            }
            continue;  // CAS failed, retry
        }
        
        // Need to wait
        __atomic_add(&rwlock->waiters_readers, 1);
        
        // Wait on the state variable
        futex_wait(&rwlock->state, state, NULL);
        
        __atomic_sub(&rwlock->waiters_readers, 1);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t* rwlock) {
    if (!rwlock) return EINVAL;
    
    int state = rwlock->state;
    
    // Can only acquire if not write-locked and no waiting writers
    if (state >= 0 && rwlock->waiters_writers == 0) {
        if (__atomic_cas(&rwlock->state, state, state + 1) == state) {
            return 0;
        }
    }
    
    return EBUSY;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t* rwlock, const struct timespec* abstime) {
    if (!rwlock) return EINVAL;
    if (!abstime) return pthread_rwlock_rdlock(rwlock);
    
    while (1) {
        int state = rwlock->state;
        
        if (state >= 0 && rwlock->waiters_writers == 0) {
            if (__atomic_cas(&rwlock->state, state, state + 1) == state) {
                return 0;
            }
            continue;
        }
        
        __atomic_add(&rwlock->waiters_readers, 1);
        
        int ret = futex_wait(&rwlock->state, state, abstime);
        
        __atomic_sub(&rwlock->waiters_readers, 1);
        
        if (ret < 0 && errno == ETIMEDOUT) {
            return ETIMEDOUT;
        }
    }
}

int pthread_rwlock_wrlock(pthread_rwlock_t* rwlock) {
    if (!rwlock) return EINVAL;
    
    pid_t tid = gettid();
    
    // Indicate we're waiting to write (gives us priority over new readers)
    __atomic_add(&rwlock->waiters_writers, 1);
    
    while (1) {
        int state = rwlock->state;
        
        // Can acquire write lock only if completely unlocked
        if (state == RWLOCK_UNLOCKED) {
            if (__atomic_cas(&rwlock->state, RWLOCK_UNLOCKED, RWLOCK_WRITELOCKED) == RWLOCK_UNLOCKED) {
                rwlock->writer_tid = tid;
                __atomic_sub(&rwlock->waiters_writers, 1);
                return 0;
            }
            continue;
        }
        
        // Wait
        futex_wait(&rwlock->state, state, NULL);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t* rwlock) {
    if (!rwlock) return EINVAL;
    
    if (__atomic_cas(&rwlock->state, RWLOCK_UNLOCKED, RWLOCK_WRITELOCKED) == RWLOCK_UNLOCKED) {
        rwlock->writer_tid = gettid();
        return 0;
    }
    
    return EBUSY;
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t* rwlock, const struct timespec* abstime) {
    if (!rwlock) return EINVAL;
    if (!abstime) return pthread_rwlock_wrlock(rwlock);
    
    pid_t tid = gettid();
    
    __atomic_add(&rwlock->waiters_writers, 1);
    
    while (1) {
        int state = rwlock->state;
        
        if (state == RWLOCK_UNLOCKED) {
            if (__atomic_cas(&rwlock->state, RWLOCK_UNLOCKED, RWLOCK_WRITELOCKED) == RWLOCK_UNLOCKED) {
                rwlock->writer_tid = tid;
                __atomic_sub(&rwlock->waiters_writers, 1);
                return 0;
            }
            continue;
        }
        
        int ret = futex_wait(&rwlock->state, state, abstime);
        
        if (ret < 0 && errno == ETIMEDOUT) {
            __atomic_sub(&rwlock->waiters_writers, 1);
            return ETIMEDOUT;
        }
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t* rwlock) {
    if (!rwlock) return EINVAL;
    
    int state = rwlock->state;
    
    if (state == RWLOCK_WRITELOCKED) {
        // Writer unlocking
        rwlock->writer_tid = 0;
        rwlock->state = RWLOCK_UNLOCKED;
        
        // Wake all waiters (both readers and writers can try to acquire)
        futex_wake(&rwlock->state, 0x7FFFFFFF);
    } else if (state > 0) {
        // Reader unlocking
        int new_state = __atomic_sub(&rwlock->state, 1);
        
        // If we were the last reader, wake waiting writers
        if (new_state == 1) {  // Was 1, now 0
            futex_wake(&rwlock->state, 0x7FFFFFFF);
        }
    } else {
        // Not locked
        return EPERM;
    }
    
    return 0;
}

// ============================================================================
// SPINLOCKS
// ============================================================================

/*
 * Simple test-and-set spinlock with exponential backoff hint (pause).
 * State: 0 = unlocked, 1 = locked
 */

int pthread_spin_init(pthread_spinlock_t* lock, int pshared) {
    if (!lock) return EINVAL;
    (void)pshared;  // Spinlocks work the same either way
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t* lock) {
    if (!lock) return EINVAL;
    if (*lock != 0) return EBUSY;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t* lock) {
    if (!lock) return EINVAL;
    
    while (__sync_lock_test_and_set(lock, 1)) {
        // Spin while locked
        while (*lock) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t* lock) {
    if (!lock) return EINVAL;
    
    if (__sync_lock_test_and_set(lock, 1) == 0) {
        return 0;
    }
    
    return EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t* lock) {
    if (!lock) return EINVAL;
    
    __sync_lock_release(lock);
    return 0;
}

// ============================================================================
// BARRIERS
// ============================================================================

/*
 * Barrier implementation using a count and generation number.
 * 
 * Protocol:
 * - Each thread increments count
 * - When count reaches threshold:
 *   - Reset count to 0
 *   - Increment generation
 *   - Wake all waiters
 *   - Return PTHREAD_BARRIER_SERIAL_THREAD for one thread
 * - Other threads wait on generation to change
 */

int pthread_barrierattr_init(pthread_barrierattr_t* attr) {
    if (!attr) return EINVAL;
    attr->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t* attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t* attr, int pshared) {
    if (!attr) return EINVAL;
    if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attr->pshared = pshared;
    return 0;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t* attr, int* pshared) {
    if (!attr || !pshared) return EINVAL;
    *pshared = attr->pshared;
    return 0;
}

int pthread_barrier_init(pthread_barrier_t* barrier,
                         const pthread_barrierattr_t* attr, unsigned int count) {
    if (!barrier || count == 0) return EINVAL;
    
    barrier->count = 0;
    barrier->generation = 0;
    barrier->threshold = count;
    
    if (attr) {
        barrier->pshared = attr->pshared;
    } else {
        barrier->pshared = PTHREAD_PROCESS_PRIVATE;
    }
    
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t* barrier) {
    if (!barrier) return EINVAL;
    
    // Check if there are waiting threads
    if (barrier->count != 0) {
        return EBUSY;
    }
    
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t* barrier) {
    if (!barrier) return EINVAL;
    
    unsigned int gen = barrier->generation;
    unsigned int new_count = __atomic_add(&barrier->count, 1) + 1;
    
    if (new_count == barrier->threshold) {
        // We're the last thread - release all others
        barrier->count = 0;
        __sync_fetch_and_add(&barrier->generation, 1);
        
        // Wake all waiting threads
        futex_wake((int*)&barrier->generation, 0x7FFFFFFF);
        
        // This thread returns special value
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    
    // Wait for generation to change
    while (barrier->generation == gen) {
        futex_wait((int*)&barrier->generation, gen, NULL);
    }
    
    return 0;
}
