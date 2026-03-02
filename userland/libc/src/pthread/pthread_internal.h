/*
 * LikeOS-64 POSIX Threads - Internal Definitions
 *
 * Shared internal structures between pthread implementation files.
 * Not part of the public API.
 */

#ifndef _PTHREAD_INTERNAL_H
#define _PTHREAD_INTERNAL_H

#include "../../include/pthread.h"
#include "../../include/sched.h"

// Maximum number of TSD keys (must match PTHREAD_KEYS_MAX)
#define MAX_TSD_KEYS 128

// Thread states
#define THREAD_STATE_RUNNING    0
#define THREAD_STATE_EXITED     1
#define THREAD_STATE_DETACHED   2
#define THREAD_STATE_JOINED     3

/*
 * Thread Control Block (TCB) - Internal structure for pthread_t
 * 
 * This structure is allocated at the high end of each thread's stack region.
 * The 'self' pointer at offset 0 allows FS:0 to point to the TCB itself.
 */
struct __pthread {
    // Self pointer (must be first for TLS access via FS:[0])
    struct __pthread* self;
    
    // Thread identity
    pid_t tid;                      // Thread ID from kernel
    volatile int tid_futex;         // Futex for join (cleared on exit by CLONE_CHILD_CLEARTID)
    
    // Thread state
    volatile int state;             // THREAD_STATE_*
    void* retval;                   // Return value from thread function
    
    // Stack information
    void* stack_base;               // mmap'd region base
    size_t stack_size;              // Total size including guard
    size_t guard_size;              // Guard page size
    
    // Attributes (copied from pthread_attr_t at creation)
    int detach_state;
    cpu_set_t cpuset;
    int cpuset_valid;
    
    // TLS/TSD support
    void* tsd_values[MAX_TSD_KEYS];  // Thread-specific data values
    
    // Robust mutex list
    struct robust_list_head robust_list;
    
    // Linked list of all threads (for cleanup)
    struct __pthread* next;
    struct __pthread* prev;
    
    // Start routine and argument (saved for stack traces/debugging)
    void* (*start_routine)(void*);
    void* start_arg;
    
    // Cancellation state (stub)
    int cancel_state;
    int cancel_type;
    int canceled;
    
    // Padding to ensure alignment
    char _pad[32];
};

// Get current thread's TCB
static inline struct __pthread* __pthread_self(void) {
    struct __pthread* tcb;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

// TSD destructor caller (defined in pthread_tsd.c)
extern void __pthread_tsd_run_destructors(void);

#endif /* _PTHREAD_INTERNAL_H */
