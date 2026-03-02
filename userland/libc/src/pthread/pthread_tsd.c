/*
 * LikeOS-64 POSIX Threads - Thread-Specific Data (TSD)
 *
 * Also known as Thread-Local Storage (TLS) keys.
 * Provides pthread_key_create, pthread_key_delete, 
 * pthread_getspecific, pthread_setspecific.
 */

#include "../../include/pthread.h"
#include "../../include/errno.h"
#include "pthread_internal.h"

extern int errno;

// ============================================================================
// GLOBAL TSD STATE
// ============================================================================

// Maximum number of keys (must match PTHREAD_KEYS_MAX in pthread.h)
#define MAX_KEYS 128

// Key state
static struct {
    void (*destructor)(void*);  // Destructor function (NULL if no destructor)
    volatile int in_use;        // 1 if key is allocated, 0 if free
} __tsd_keys[MAX_KEYS];

// Spinlock for key allocation
static volatile int __tsd_lock = 0;

// Next key to try allocating (circular search)
static unsigned int __tsd_next = 0;

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static inline void __spin_lock(volatile int* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

static inline void __spin_unlock(volatile int* lock) {
    __sync_lock_release(lock);
}

// Get current thread's TCB
static inline struct __pthread* __get_tcb(void) {
    struct __pthread* tcb;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

// ============================================================================
// TSD API FUNCTIONS
// ============================================================================

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    if (!key) return EINVAL;
    
    __spin_lock(&__tsd_lock);
    
    // Search for a free key slot
    unsigned int start = __tsd_next;
    unsigned int k = start;
    
    do {
        if (!__tsd_keys[k].in_use) {
            // Found a free slot
            __tsd_keys[k].in_use = 1;
            __tsd_keys[k].destructor = destructor;
            __tsd_next = (k + 1) % MAX_KEYS;
            
            __spin_unlock(&__tsd_lock);
            
            *key = k;
            return 0;
        }
        k = (k + 1) % MAX_KEYS;
    } while (k != start);
    
    __spin_unlock(&__tsd_lock);
    
    // No free keys
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= MAX_KEYS) return EINVAL;
    
    __spin_lock(&__tsd_lock);
    
    if (!__tsd_keys[key].in_use) {
        __spin_unlock(&__tsd_lock);
        return EINVAL;
    }
    
    // Mark key as free
    // Note: POSIX says we don't need to call destructors here,
    // and existing values in threads become undefined
    __tsd_keys[key].in_use = 0;
    __tsd_keys[key].destructor = (void (*)(void*))0;
    
    __spin_unlock(&__tsd_lock);
    
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    if (key >= MAX_KEYS) return EINVAL;
    
    // Don't check in_use - POSIX allows this race
    // (key could be deleted by another thread after we check)
    
    struct __pthread* tcb = __get_tcb();
    if (!tcb) {
        // No TCB - this shouldn't happen
        return EINVAL;
    }
    
    tcb->tsd_values[key] = (void*)value;
    return 0;
}

void* pthread_getspecific(pthread_key_t key) {
    if (key >= MAX_KEYS) return (void*)0;
    
    struct __pthread* tcb = __get_tcb();
    if (!tcb) {
        return (void*)0;
    }
    
    return tcb->tsd_values[key];
}

// ============================================================================
// INTERNAL: DESTRUCTOR CALLING
// ============================================================================

/*
 * Called during pthread_exit to invoke destructors for all TSD values.
 * POSIX requires up to PTHREAD_DESTRUCTOR_ITERATIONS attempts because
 * destructors might set new TSD values.
 */

#define PTHREAD_DESTRUCTOR_ITERATIONS 4

void __pthread_tsd_run_destructors(void) {
    struct __pthread* tcb = __get_tcb();
    if (!tcb) return;
    
    for (int iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
        int any_called = 0;
        
        for (unsigned int key = 0; key < MAX_KEYS; key++) {
            // Get value and destructor atomically-ish
            // (there's an inherent race with pthread_key_delete, but POSIX allows it)
            void* value = tcb->tsd_values[key];
            void (*destructor)(void*) = __tsd_keys[key].destructor;
            int in_use = __tsd_keys[key].in_use;
            
            if (value && destructor && in_use) {
                // Clear value before calling destructor
                tcb->tsd_values[key] = (void*)0;
                
                // Call destructor
                destructor(value);
                any_called = 1;
            }
        }
        
        // If no destructors were called, we're done
        if (!any_called) break;
    }
}
