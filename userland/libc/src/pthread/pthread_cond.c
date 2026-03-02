/*
 * LikeOS-64 POSIX Threads - Condition Variable Implementation
 *
 * Futex-based condition variables using sequence number protocol.
 * 
 * Protocol:
 * - seq is a monotonically increasing sequence number (futex word)
 * - wait: save seq, unlock mutex, futex_wait on seq, relock mutex
 * - signal: increment seq, futex_wake(1)
 * - broadcast: increment seq, futex_wake(all) or use requeue
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

// Futex requeue operation
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4

static inline long __futex_requeue(int* uaddr, int count, int* uaddr2, int count2, int val) {
    // syscall(SYS_FUTEX, uaddr, FUTEX_CMP_REQUEUE, count, count2, uaddr2, val)
    register long r10 __asm__("r10") = (long)count2;
    register long r8 __asm__("r8") = (long)uaddr2;
    register long r9 __asm__("r9") = (long)val;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(315), "D"((long)uaddr), "S"(FUTEX_CMP_REQUEUE), "d"(count), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// ============================================================================
// CONDITION VARIABLE ATTRIBUTES
// ============================================================================

int pthread_condattr_init(pthread_condattr_t* attr) {
    if (!attr) return EINVAL;
    attr->pshared = PTHREAD_PROCESS_PRIVATE;
    attr->clock_id = 0;  // CLOCK_REALTIME
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t* attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t* attr, int pshared) {
    if (!attr) return EINVAL;
    if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attr->pshared = pshared;
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t* attr, int* pshared) {
    if (!attr || !pshared) return EINVAL;
    *pshared = attr->pshared;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t* attr, int clock_id) {
    if (!attr) return EINVAL;
    attr->clock_id = clock_id;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t* attr, int* clock_id) {
    if (!attr || !clock_id) return EINVAL;
    *clock_id = attr->clock_id;
    return 0;
}

// ============================================================================
// CONDITION VARIABLE OPERATIONS
// ============================================================================

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    if (!cond) return EINVAL;
    
    cond->seq = 0;
    cond->waiters = 0;
    cond->mutex = NULL;
    
    if (attr) {
        cond->pshared = attr->pshared;
    } else {
        cond->pshared = PTHREAD_PROCESS_PRIVATE;
    }
    
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    
    // Check for waiters
    if (cond->waiters != 0) {
        return EBUSY;
    }
    
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!cond || !mutex) return EINVAL;
    
    // Record the associated mutex (for debugging / broadcast optimization)
    cond->mutex = mutex;
    
    // Save current sequence number before releasing mutex
    unsigned int seq = cond->seq;
    
    // Increment waiter count
    __sync_fetch_and_add(&cond->waiters, 1);
    
    // Release the mutex
    int ret = pthread_mutex_unlock(mutex);
    if (ret != 0) {
        __sync_fetch_and_sub(&cond->waiters, 1);
        return ret;
    }
    
    // Wait for signal (seq to change)
    // Loop to handle spurious wakeups
    while (cond->seq == seq) {
        futex_wait((int*)&cond->seq, seq, NULL);
    }
    
    // Decrement waiter count
    __sync_fetch_and_sub(&cond->waiters, 1);
    
    // Reacquire the mutex
    ret = pthread_mutex_lock(mutex);
    
    return ret;
}

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime) {
    if (!cond || !mutex) return EINVAL;
    if (!abstime) return pthread_cond_wait(cond, mutex);
    
    cond->mutex = mutex;
    unsigned int seq = cond->seq;
    
    __sync_fetch_and_add(&cond->waiters, 1);
    
    int ret = pthread_mutex_unlock(mutex);
    if (ret != 0) {
        __sync_fetch_and_sub(&cond->waiters, 1);
        return ret;
    }
    
    // Wait with timeout
    int timeout_ret = 0;
    while (cond->seq == seq) {
        int fret = futex_wait((int*)&cond->seq, seq, abstime);
        if (fret < 0 && errno == ETIMEDOUT) {
            timeout_ret = ETIMEDOUT;
            break;
        }
    }
    
    __sync_fetch_and_sub(&cond->waiters, 1);
    
    // Reacquire mutex
    ret = pthread_mutex_lock(mutex);
    if (ret != 0) return ret;
    
    // Return timeout error if that's why we woke up and seq didn't change
    if (timeout_ret && cond->seq == seq) {
        return ETIMEDOUT;
    }
    
    return 0;
}

int pthread_cond_signal(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    
    // Increment sequence number (atomically)
    __sync_fetch_and_add(&cond->seq, 1);
    
    // Wake one waiter
    if (cond->waiters > 0) {
        futex_wake((int*)&cond->seq, 1);
    }
    
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    if (!cond) return EINVAL;
    
    // Increment sequence number
    unsigned int old_seq = __sync_fetch_and_add(&cond->seq, 1);
    
    // If there are waiters, wake all of them
    if (cond->waiters > 0) {
        // Option 1: Simple wake-all
        futex_wake((int*)&cond->seq, 0x7FFFFFFF);
        
        // Option 2: Use FUTEX_REQUEUE to move waiters to mutex futex
        // This is more efficient as it avoids thundering herd on mutex
        // But requires knowing the mutex - left for future optimization
        /*
        if (cond->mutex) {
            __futex_requeue((int*)&cond->seq, 1, &cond->mutex->state, 
                           0x7FFFFFFF, old_seq);
        } else {
            futex_wake((int*)&cond->seq, 0x7FFFFFFF);
        }
        */
    }
    
    return 0;
}
