/*
 * LikeOS-64 POSIX Threads - Mutex Implementation
 *
 * Futex-based mutexes with support for:
 * - Normal mutexes (fast path via atomic CAS)
 * - Recursive mutexes (owner tracking + count)
 * - Error-checking mutexes (deadlock detection)
 * - Robust mutexes (automatic cleanup on owner death)
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
// MUTEX STATE ENCODING
// ============================================================================

// Mutex state values:
//   0 = unlocked
//   1 = locked, no waiters
//   2 = locked, waiters present (need to wake on unlock)
//
// For robust mutexes, the owner TID is also tracked separately.
// If owner dies, kernel sets FUTEX_OWNER_DIED bit in the futex word.

#define MUTEX_UNLOCKED      0
#define MUTEX_LOCKED        1
#define MUTEX_CONTENDED     2

// Robust futex flags (must match kernel)
#define FUTEX_WAITERS       0x80000000
#define FUTEX_OWNER_DIED    0x40000000
#define FUTEX_TID_MASK      0x3FFFFFFF

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Atomic compare-and-swap
static inline int __atomic_cas(volatile int* ptr, int old_val, int new_val) {
    return __sync_val_compare_and_swap(ptr, old_val, new_val);
}

// Atomic exchange
static inline int __atomic_xchg(volatile int* ptr, int new_val) {
    return __sync_lock_test_and_set(ptr, new_val);
}

// Get current thread ID
static inline pid_t __gettid(void) {
    return gettid();
}

// Add mutex to robust list
static void __robust_list_add(pthread_mutex_t* mutex) {
    // Get current thread's robust list head via TLS
    struct __pthread* tcb;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
    
    if (!tcb) return;
    
    struct robust_list_head* head = &tcb->robust_list;
    struct robust_list* entry = &mutex->robust_node;
    
    // Insert at head of list
    entry->next = head->list.next;
    head->list.next = entry;
}

// Remove mutex from robust list
static void __robust_list_remove(pthread_mutex_t* mutex) {
    struct __pthread* tcb;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
    
    if (!tcb) return;
    
    struct robust_list_head* head = &tcb->robust_list;
    struct robust_list* prev = &head->list;
    struct robust_list* curr = prev->next;
    struct robust_list* entry = &mutex->robust_node;
    
    // Find and remove entry
    while (curr != &head->list) {
        if (curr == entry) {
            prev->next = curr->next;
            entry->next = NULL;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// ============================================================================
// MUTEX ATTRIBUTES
// ============================================================================

int pthread_mutexattr_init(pthread_mutexattr_t* attr) {
    if (!attr) return EINVAL;
    
    attr->type = PTHREAD_MUTEX_DEFAULT;
    attr->robust = PTHREAD_MUTEX_STALLED;
    attr->pshared = PTHREAD_PROCESS_PRIVATE;
    attr->protocol = PTHREAD_PRIO_NONE;
    attr->prioceiling = 0;
    
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
    if (!attr) return EINVAL;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    if (!attr) return EINVAL;
    if (type != PTHREAD_MUTEX_NORMAL && 
        type != PTHREAD_MUTEX_RECURSIVE &&
        type != PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type) {
    if (!attr || !type) return EINVAL;
    *type = attr->type;
    return 0;
}

int pthread_mutexattr_setrobust(pthread_mutexattr_t* attr, int robust) {
    if (!attr) return EINVAL;
    if (robust != PTHREAD_MUTEX_STALLED && robust != PTHREAD_MUTEX_ROBUST) {
        return EINVAL;
    }
    attr->robust = robust;
    return 0;
}

int pthread_mutexattr_getrobust(const pthread_mutexattr_t* attr, int* robust) {
    if (!attr || !robust) return EINVAL;
    *robust = attr->robust;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t* attr, int pshared) {
    if (!attr) return EINVAL;
    if (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attr->pshared = pshared;
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t* attr, int* pshared) {
    if (!attr || !pshared) return EINVAL;
    *pshared = attr->pshared;
    return 0;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t* attr, int protocol) {
    if (!attr) return EINVAL;
    if (protocol != PTHREAD_PRIO_NONE && 
        protocol != PTHREAD_PRIO_INHERIT &&
        protocol != PTHREAD_PRIO_PROTECT) {
        return EINVAL;
    }
    attr->protocol = protocol;
    return 0;
}

int pthread_mutexattr_getprotocol(const pthread_mutexattr_t* attr, int* protocol) {
    if (!attr || !protocol) return EINVAL;
    *protocol = attr->protocol;
    return 0;
}

int pthread_mutexattr_setprioceiling(pthread_mutexattr_t* attr, int prioceiling) {
    if (!attr) return EINVAL;
    attr->prioceiling = prioceiling;
    return 0;
}

int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t* attr, int* prioceiling) {
    if (!attr || !prioceiling) return EINVAL;
    *prioceiling = attr->prioceiling;
    return 0;
}

// ============================================================================
// MUTEX OPERATIONS
// ============================================================================

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    if (!mutex) return EINVAL;
    
    mutex->state = MUTEX_UNLOCKED;
    mutex->owner_tid = 0;
    mutex->count = 0;
    mutex->robust_node.next = NULL;
    
    if (attr) {
        mutex->type = attr->type;
        mutex->robust = attr->robust;
        mutex->pshared = attr->pshared;
    } else {
        mutex->type = PTHREAD_MUTEX_DEFAULT;
        mutex->robust = PTHREAD_MUTEX_STALLED;
        mutex->pshared = PTHREAD_PROCESS_PRIVATE;
    }
    
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    
    // Check if mutex is locked
    if (mutex->state != MUTEX_UNLOCKED) {
        return EBUSY;
    }
    
    // Remove from robust list if it was there
    if (mutex->robust_node.next) {
        __robust_list_remove(mutex);
    }
    
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    
    pid_t tid = __gettid();
    
    // Handle recursive and error-checking mutexes
    if (mutex->type != PTHREAD_MUTEX_NORMAL) {
        // Check if we already own it
        if (mutex->owner_tid == tid) {
            if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
                mutex->count++;
                return 0;
            } else {
                // ERRORCHECK: deadlock
                return EDEADLK;
            }
        }
    }
    
    // Fast path: try to acquire unlocked mutex
    int old = __atomic_cas(&mutex->state, MUTEX_UNLOCKED, MUTEX_LOCKED);
    if (old == MUTEX_UNLOCKED) {
        // Got the lock
        mutex->owner_tid = tid;
        mutex->count = 1;
        
        // Add to robust list if robust mutex
        if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
            __robust_list_add(mutex);
        }
        
        return 0;
    }
    
    // Check for owner death (robust mutex)
    if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
        if (old & FUTEX_OWNER_DIED) {
            // Previous owner died - try to acquire
            int expected = old;
            int desired = (tid & FUTEX_TID_MASK) | FUTEX_OWNER_DIED;
            if (__atomic_cas(&mutex->state, expected, desired) == expected) {
                mutex->owner_tid = tid;
                mutex->count = 1;
                __robust_list_add(mutex);
                return EOWNERDEAD;  // Caller must call pthread_mutex_consistent
            }
        }
    }
    
    // Slow path: need to wait
    // First, mark mutex as contended
    if (old == MUTEX_LOCKED) {
        old = __atomic_xchg(&mutex->state, MUTEX_CONTENDED);
    }
    
    // Loop until we acquire the lock
    while (old != MUTEX_UNLOCKED) {
        // Wait on futex
        futex_wait(&mutex->state, MUTEX_CONTENDED, NULL);
        
        // Try to acquire, setting contended since others might be waiting
        old = __atomic_xchg(&mutex->state, MUTEX_CONTENDED);
        
        // Check for owner death
        if (mutex->robust == PTHREAD_MUTEX_ROBUST && (old & FUTEX_OWNER_DIED)) {
            mutex->owner_tid = tid;
            mutex->count = 1;
            __robust_list_add(mutex);
            return EOWNERDEAD;
        }
    }
    
    // Got the lock
    mutex->owner_tid = tid;
    mutex->count = 1;
    
    if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
        __robust_list_add(mutex);
    }
    
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    
    pid_t tid = __gettid();
    
    // Handle recursive mutex
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner_tid == tid) {
        mutex->count++;
        return 0;
    }
    
    // Handle error-checking mutex (self-deadlock)
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK && mutex->owner_tid == tid) {
        return EBUSY;  // POSIX says EBUSY for trylock on self-owned mutex
    }
    
    // Try to acquire
    int old = __atomic_cas(&mutex->state, MUTEX_UNLOCKED, MUTEX_LOCKED);
    if (old != MUTEX_UNLOCKED) {
        // Check for owner death
        if (mutex->robust == PTHREAD_MUTEX_ROBUST && (old & FUTEX_OWNER_DIED)) {
            int expected = old;
            int desired = (tid & FUTEX_TID_MASK) | FUTEX_OWNER_DIED;
            if (__atomic_cas(&mutex->state, expected, desired) == expected) {
                mutex->owner_tid = tid;
                mutex->count = 1;
                __robust_list_add(mutex);
                return EOWNERDEAD;
            }
        }
        return EBUSY;
    }
    
    mutex->owner_tid = tid;
    mutex->count = 1;
    
    if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
        __robust_list_add(mutex);
    }
    
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abstime) {
    if (!mutex) return EINVAL;
    if (!abstime) return pthread_mutex_lock(mutex);
    
    pid_t tid = __gettid();
    
    // Handle recursive
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner_tid == tid) {
        mutex->count++;
        return 0;
    }
    
    // Handle error-checking
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK && mutex->owner_tid == tid) {
        return EDEADLK;
    }
    
    // Fast path
    int old = __atomic_cas(&mutex->state, MUTEX_UNLOCKED, MUTEX_LOCKED);
    if (old == MUTEX_UNLOCKED) {
        mutex->owner_tid = tid;
        mutex->count = 1;
        if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
            __robust_list_add(mutex);
        }
        return 0;
    }
    
    // Slow path with timeout
    if (old == MUTEX_LOCKED) {
        old = __atomic_xchg(&mutex->state, MUTEX_CONTENDED);
    }
    
    while (old != MUTEX_UNLOCKED) {
        // Convert absolute time to relative for futex
        // (Simplified: real implementation would compute remaining time)
        int ret = futex_wait(&mutex->state, MUTEX_CONTENDED, abstime);
        if (ret < 0 && errno == ETIMEDOUT) {
            return ETIMEDOUT;
        }
        
        old = __atomic_xchg(&mutex->state, MUTEX_CONTENDED);
    }
    
    mutex->owner_tid = tid;
    mutex->count = 1;
    
    if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
        __robust_list_add(mutex);
    }
    
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    
    pid_t tid = __gettid();
    
    // Ownership check for error-checking and recursive mutexes
    if (mutex->type != PTHREAD_MUTEX_NORMAL) {
        if (mutex->owner_tid != tid) {
            return EPERM;
        }
    }
    
    // Handle recursive unlock
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        if (--mutex->count > 0) {
            return 0;  // Still owned, not fully unlocked
        }
    }
    
    // Remove from robust list
    if (mutex->robust == PTHREAD_MUTEX_ROBUST) {
        __robust_list_remove(mutex);
    }
    
    // Clear owner
    mutex->owner_tid = 0;
    mutex->count = 0;
    
    // Unlock and wake waiters if needed
    int old = __atomic_xchg(&mutex->state, MUTEX_UNLOCKED);
    if (old == MUTEX_CONTENDED) {
        // There are waiters, wake one
        futex_wake(&mutex->state, 1);
    }
    
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t* mutex) {
    if (!mutex) return EINVAL;
    
    if (mutex->robust != PTHREAD_MUTEX_ROBUST) {
        return EINVAL;
    }
    
    // Clear the OWNER_DIED flag
    // Caller must have acquired the mutex with EOWNERDEAD
    int old = mutex->state;
    if (!(old & FUTEX_OWNER_DIED)) {
        return EINVAL;  // Not in inconsistent state
    }
    
    // Clear the flag, keep ownership
    mutex->state = MUTEX_LOCKED;
    
    return 0;
}
