/*
 * LikeOS-64 POSIX Threads Implementation
 *
 * Core thread functions: create, exit, join, detach, self, equal
 * Uses clone() with thread flags and futex for join synchronization.
 */

#include "../../include/pthread.h"
#include "../../include/sys/mman.h"
#include "../../include/sched.h"
#include "../../include/errno.h"
#include "../../include/stdlib.h"
#include "../../include/string.h"
#include "../../include/unistd.h"
#include "../syscalls/syscall.h"
#include "pthread_internal.h"

extern int errno;

// ============================================================================
// CONSTANTS
// ============================================================================

// Default stack size: 2MB with 4KB guard page
#define PTHREAD_STACK_MIN       (16 * 1024)         // 16 KB minimum
#define PTHREAD_STACK_DEFAULT   (2 * 1024 * 1024)   // 2 MB default
#define PTHREAD_GUARD_SIZE      (4 * 1024)          // 4 KB guard page

// TLS block layout (dynamic sizing)
// The TLS block is allocated at the high end of the thread's stack region
// Layout: [guard page] [stack grows down] ... [TLS block] [TCB at top]
#define PTHREAD_TLS_ALIGN       16
#define PTHREAD_TCB_SIZE        256                 // Thread control block

// Clone flags for thread creation
#define CLONE_THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                            CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | \
                            CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | \
                            CLONE_CHILD_CLEARTID)

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Main thread's TCB (statically allocated for the initial thread)
static struct __pthread __main_thread;
static int __pthread_initialized = 0;

// Thread list for cleanup (protected by spinlock in real implementation)
static struct __pthread* __thread_list_head = NULL;
static volatile int __thread_list_lock = 0;

// TSD key management
static void (*__tsd_destructors[PTHREAD_KEYS_MAX])(void*);
static volatile int __tsd_key_used[PTHREAD_KEYS_MAX];
static volatile int __tsd_next_key = 0;
static volatile int __tsd_lock = 0;

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Simple spinlock for internal use
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

// ============================================================================
// ZOMBIE STACK CLEANUP (deferred stack freeing)
// ============================================================================
// When a thread is joined, we can't immediately munmap its stack because
// the kernel may still be accessing it during the exit path (after tid_futex
// is cleared but before the kernel fully releases the thread). We queue
// stacks for deferred cleanup and free them on the next pthread_create.

#define ZOMBIE_STACK_MAX 64

struct zombie_stack {
    void*   base;
    size_t  size;
};

static struct zombie_stack __zombie_stacks[ZOMBIE_STACK_MAX];
static volatile int __zombie_count = 0;
static volatile int __zombie_lock = 0;

// Add a stack to the zombie list for deferred cleanup
static void __zombie_stack_add(void* base, size_t size) {
    if (!base || size == 0) return;
    
    __spin_lock(&__zombie_lock);
    
    if (__zombie_count < ZOMBIE_STACK_MAX) {
        __zombie_stacks[__zombie_count].base = base;
        __zombie_stacks[__zombie_count].size = size;
        __zombie_count++;
    } else {
        // List full - must free now (shouldn't happen in practice)
        // This is safe because by the time the list is full, earlier
        // entries have definitely finished exiting
        munmap(base, size);
    }
    
    __spin_unlock(&__zombie_lock);
}

// Cleanup all zombie stacks (called before creating new threads)
static void __zombie_stack_cleanup(void) {
    __spin_lock(&__zombie_lock);
    
    for (int i = 0; i < __zombie_count; i++) {
        if (__zombie_stacks[i].base) {
            munmap(__zombie_stacks[i].base, __zombie_stacks[i].size);
            __zombie_stacks[i].base = NULL;
            __zombie_stacks[i].size = 0;
        }
    }
    __zombie_count = 0;
    
    __spin_unlock(&__zombie_lock);
}

// ============================================================================
// MORE INTERNAL HELPERS
// ============================================================================

// Atomic compare-and-swap
static inline int __atomic_cas(volatile int* ptr, int old_val, int new_val) {
    return __sync_val_compare_and_swap(ptr, old_val, new_val);
}

// Futex operations (from sched.c wrappers)
extern int futex_wait(volatile int* uaddr, int val, const struct timespec* timeout);
extern int futex_wake(volatile int* uaddr, int count);

// Get current thread's TCB
static inline struct __pthread* __get_tcb(void) {
    struct __pthread* tcb;
    // Read TCB pointer from FS:0 (self pointer stored at offset 0)
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

// Set TLS base (FS segment)
static inline int __set_tls(void* addr) {
    return arch_prctl(ARCH_SET_FS, (unsigned long)addr);
}

// Initialize the main thread's TCB (called lazily)
static void __pthread_init_main(void) {
    if (__pthread_initialized) return;
    
    struct __pthread* main = &__main_thread;
    
    // Zero out
    for (size_t i = 0; i < sizeof(*main); i++) {
        ((char*)main)[i] = 0;
    }
    
    main->self = main;
    main->tid = gettid();
    main->tid_futex = main->tid;
    main->state = THREAD_STATE_RUNNING;
    main->detach_state = PTHREAD_CREATE_JOINABLE;
    main->stack_base = NULL;        // Main thread's stack is special
    main->stack_size = 0;
    main->guard_size = 0;
    main->cancel_state = PTHREAD_CANCEL_ENABLE;
    main->cancel_type = PTHREAD_CANCEL_DEFERRED;
    
    // Initialize robust list
    main->robust_list.list.next = &main->robust_list.list;
    main->robust_list.futex_offset = (long)&((pthread_mutex_t*)0)->state;
    main->robust_list.list_op_pending = NULL;
    
    // Register robust list with kernel
    set_robust_list(&main->robust_list, sizeof(main->robust_list));
    
    // Set TLS to point to main thread's TCB
    __set_tls(main);
    
    // Add to thread list
    main->next = main->prev = main;
    __thread_list_head = main;
    
    __pthread_initialized = 1;
}

// Thread startup wrapper - called by clone trampoline
static int __pthread_start(void* arg) {
    struct __pthread* tcb = (struct __pthread*)arg;
    
    // Set TLS to our TCB
    __set_tls(tcb);
    
    // Register robust futex list with kernel
    set_robust_list(&tcb->robust_list, sizeof(tcb->robust_list));
    
    // Apply CPU affinity if specified
    if (tcb->cpuset_valid) {
        sched_setaffinity(0, sizeof(cpu_set_t), &tcb->cpuset);
    }
    
    // Call the user's thread function
    void* retval = tcb->start_routine(tcb->start_arg);
    
    // Thread function returned - call pthread_exit
    pthread_exit(retval);
    
    // Never reached
    return 0;
}

// Call TSD destructors
static void __call_tsd_destructors(struct __pthread* tcb) {
    // POSIX requires up to PTHREAD_DESTRUCTOR_ITERATIONS attempts
    #define PTHREAD_DESTRUCTOR_ITERATIONS 4
    
    for (int iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
        int any_called = 0;
        
        for (unsigned int key = 0; key < PTHREAD_KEYS_MAX; key++) {
            void* value = tcb->tsd_values[key];
            void (*destructor)(void*) = __tsd_destructors[key];
            
            if (value && destructor && __tsd_key_used[key]) {
                tcb->tsd_values[key] = NULL;
                destructor(value);
                any_called = 1;
            }
        }
        
        if (!any_called) break;
    }
}

// ============================================================================
// PUBLIC API: THREAD CREATION AND MANAGEMENT
// ============================================================================

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg) {
    // Initialize main thread if not done
    if (!__pthread_initialized) {
        __pthread_init_main();
    }
    
    if (!thread || !start_routine) {
        return EINVAL;
    }
    
    // Clean up any zombie stacks from previously joined threads
    // This is safe because those threads have definitely exited by now
    __zombie_stack_cleanup();
    
    // Determine stack size
    size_t stack_size = PTHREAD_STACK_DEFAULT;
    size_t guard_size = PTHREAD_GUARD_SIZE;
    int detach_state = PTHREAD_CREATE_JOINABLE;
    cpu_set_t cpuset;
    int cpuset_valid = 0;
    void* stack_addr = NULL;
    
    if (attr) {
        if (attr->stacksize >= PTHREAD_STACK_MIN) {
            stack_size = attr->stacksize;
        }
        detach_state = attr->detachstate;
        if (attr->stackaddr) {
            stack_addr = attr->stackaddr;
            guard_size = 0;  // User-provided stack, no guard page
        }
        if (attr->cpuset_valid) {
            cpuset = attr->cpuset;
            cpuset_valid = 1;
        }
    }
    
    // Calculate total allocation size
    // Layout: [guard] [stack grows down] [TCB at high address]
    size_t total_size = guard_size + stack_size + sizeof(struct __pthread);
    total_size = (total_size + 4095) & ~4095UL;  // Page align
    
    // Allocate stack + TCB region
    void* stack_base;
    if (stack_addr) {
        // User provided stack
        stack_base = stack_addr;
    } else {
        stack_base = mmap(NULL, total_size, 
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack_base == MAP_FAILED) {
            return EAGAIN;
        }
        
        // Set up guard page (make it non-accessible)
        if (guard_size > 0) {
            mprotect(stack_base, guard_size, PROT_NONE);
        }
    }
    
    // Place TCB at high end of allocation
    struct __pthread* tcb = (struct __pthread*)
        ((char*)stack_base + total_size - sizeof(struct __pthread));
    
    // Align TCB
    tcb = (struct __pthread*)((unsigned long)tcb & ~(PTHREAD_TLS_ALIGN - 1));
    
    // Initialize TCB
    for (size_t i = 0; i < sizeof(*tcb); i++) {
        ((char*)tcb)[i] = 0;
    }
    
    tcb->self = tcb;
    tcb->state = THREAD_STATE_RUNNING;
    tcb->retval = NULL;
    tcb->stack_base = stack_base;
    tcb->stack_size = total_size;
    tcb->guard_size = guard_size;
    tcb->detach_state = detach_state;
    tcb->start_routine = start_routine;
    tcb->start_arg = arg;
    tcb->cancel_state = PTHREAD_CANCEL_ENABLE;
    tcb->cancel_type = PTHREAD_CANCEL_DEFERRED;
    
    if (cpuset_valid) {
        tcb->cpuset = cpuset;
        tcb->cpuset_valid = 1;
    }
    
    // Initialize robust list
    tcb->robust_list.list.next = &tcb->robust_list.list;
    tcb->robust_list.futex_offset = (long)&((pthread_mutex_t*)0)->state;
    tcb->robust_list.list_op_pending = NULL;
    
    // Calculate child's stack pointer (below TCB, aligned)
    // Stack grows downward, so child_stack should point to the bottom of the TCB.
    // The first push will go to (child_stack - 8), which is safely below the TCB.
    // Note: tcb points to the START of TCB struct, so we use tcb directly (aligned).
    // When the child thread starts, it will push onto the stack BELOW this address.
    void* child_stack = (void*)(((unsigned long)tcb) & ~15UL);
    
    // Add to thread list
    __spin_lock(&__thread_list_lock);
    if (__thread_list_head) {
        tcb->next = __thread_list_head;
        tcb->prev = __thread_list_head->prev;
        __thread_list_head->prev->next = tcb;
        __thread_list_head->prev = tcb;
    } else {
        tcb->next = tcb->prev = tcb;
        __thread_list_head = tcb;
    }
    __spin_unlock(&__thread_list_lock);
    
    // Create the thread using clone()
    // The tid_futex will be:
    //   - Set to child TID by CLONE_PARENT_SETTID
    //   - Cleared and futex-woken by CLONE_CHILD_CLEARTID on thread exit
    pid_t tid = clone(__pthread_start, child_stack, CLONE_THREAD_FLAGS, tcb,
                      &tcb->tid, tcb, &tcb->tid_futex);
    
    if (tid < 0) {
        // Clone failed - cleanup
        __spin_lock(&__thread_list_lock);
        tcb->prev->next = tcb->next;
        tcb->next->prev = tcb->prev;
        if (__thread_list_head == tcb) {
            __thread_list_head = (tcb->next != tcb) ? tcb->next : NULL;
        }
        __spin_unlock(&__thread_list_lock);
        
        if (!stack_addr) {
            munmap(stack_base, total_size);
        }
        
        return errno;
    }
    
    *thread = tcb;
    return 0;
}

void pthread_exit(void* retval) {
    struct __pthread* tcb = __get_tcb();
    
    if (!tcb || tcb == &__main_thread) {
        // Main thread exiting - exit the entire process
        _exit((long)retval);
    }
    
    // Store return value
    tcb->retval = retval;
    
    // Full memory barrier to ensure retval is visible to other CPUs
    // before we mark the thread as exited or clear tid_futex
    __sync_synchronize();
    
    // Call TSD destructors
    __call_tsd_destructors(tcb);
    
    // Mark as exited (before exit so joiners see it)
    tcb->state = THREAD_STATE_EXITED;
    
    // If this is a detached thread, queue our stack for cleanup.
    // We can't munmap our own stack while running on it, but by adding
    // it to the zombie list now, it will be cleaned up by the next
    // pthread_create call (after we've fully exited).
    if (tcb->detach_state == PTHREAD_CREATE_DETACHED && tcb->stack_base) {
        // Remove from thread list
        __spin_lock(&__thread_list_lock);
        tcb->prev->next = tcb->next;
        tcb->next->prev = tcb->prev;
        if (__thread_list_head == tcb) {
            __thread_list_head = (tcb->next != tcb) ? tcb->next : NULL;
        }
        __spin_unlock(&__thread_list_lock);
        
        // Add to zombie list for deferred cleanup
        __zombie_stack_add(tcb->stack_base, tcb->stack_size);
    }
    
    // Exit this thread only (kernel handles CLONE_CHILD_CLEARTID)
    // This will write 0 to tid_futex and wake waiters
    _exit((long)retval);
    
    // Never reached
    __builtin_unreachable();
}

int pthread_join(pthread_t thread, void** retval) {
    if (!__pthread_initialized) {
        __pthread_init_main();
    }
    
    if (!thread) {
        return EINVAL;
    }
    
    struct __pthread* tcb = thread;
    struct __pthread* self = __get_tcb();
    
    // Can't join self
    if (tcb == self) {
        return EDEADLK;
    }
    
    // Can't join detached thread
    if (tcb->detach_state == PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
    
    // Wait for thread to exit using CLONE_CHILD_CLEARTID
    // The kernel writes 0 to tid_futex and does futex wake when thread exits
    while (tcb->tid_futex != 0) {
        int val = tcb->tid_futex;
        if (val == 0) break;
        
        // Futex wait until tid_futex changes
        futex_wait(&tcb->tid_futex, val, NULL);
    }
    
    // Full memory barrier to ensure we see the retval stored by the exiting thread
    // The exiting thread did a barrier after storing retval, before _exit()
    __sync_synchronize();
    
    // Get return value
    if (retval) {
        *retval = tcb->retval;
    }
    
    // Mark as joined
    tcb->state = THREAD_STATE_JOINED;
    
    // Save stack info before removing TCB from list
    void* stack_base = tcb->stack_base;
    size_t stack_size = tcb->stack_size;
    
    // Remove from thread list
    __spin_lock(&__thread_list_lock);
    tcb->prev->next = tcb->next;
    tcb->next->prev = tcb->prev;
    if (__thread_list_head == tcb) {
        __thread_list_head = (tcb->next != tcb) ? tcb->next : NULL;
    }
    __spin_unlock(&__thread_list_lock);
    
    // Queue the stack for deferred cleanup.
    // We don't munmap immediately because the kernel may still be accessing
    // the thread's user stack during the final exit path (even after tid_futex
    // is cleared). The stack will be freed on the next pthread_create call,
    // when we're certain all previously exited threads have fully terminated.
    if (stack_base) {
        __zombie_stack_add(stack_base, stack_size);
    }
    
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (!thread) {
        return EINVAL;
    }
    
    struct __pthread* tcb = thread;
    
    // Atomically set detached state
    int old_state = __atomic_cas(&tcb->detach_state, 
                                  PTHREAD_CREATE_JOINABLE, 
                                  PTHREAD_CREATE_DETACHED);
    
    if (old_state == PTHREAD_CREATE_DETACHED) {
        return EINVAL;  // Already detached
    }
    
    // If thread already exited, we should clean up
    // But we can't safely do that here since we don't know
    // if the thread is still using its stack
    
    return 0;
}

pthread_t pthread_self(void) {
    if (!__pthread_initialized) {
        __pthread_init_main();
    }
    return __get_tcb();
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

// ============================================================================
// THREAD ATTRIBUTES
// ============================================================================

int pthread_attr_init(pthread_attr_t* attr) {
    if (!attr) return EINVAL;
    
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->stacksize = PTHREAD_STACK_DEFAULT;
    attr->stackaddr = NULL;
    attr->guardsize = PTHREAD_GUARD_SIZE;
    attr->scope = PTHREAD_SCOPE_SYSTEM;
    attr->inheritsched = PTHREAD_INHERIT_SCHED;
    attr->schedpolicy = SCHED_OTHER;
    attr->schedparam.sched_priority = 0;
    attr->cpuset_valid = 0;
    
    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    if (!attr) return EINVAL;
    // Nothing to free
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int detachstate) {
    if (!attr) return EINVAL;
    if (detachstate != PTHREAD_CREATE_JOINABLE && 
        detachstate != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
    attr->detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detachstate) {
    if (!attr || !detachstate) return EINVAL;
    *detachstate = attr->detachstate;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stacksize) {
    if (!attr) return EINVAL;
    if (stacksize < PTHREAD_STACK_MIN) return EINVAL;
    attr->stacksize = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* stacksize) {
    if (!attr || !stacksize) return EINVAL;
    *stacksize = attr->stacksize;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t* attr, void* stackaddr, size_t stacksize) {
    if (!attr) return EINVAL;
    if (stacksize < PTHREAD_STACK_MIN) return EINVAL;
    attr->stackaddr = stackaddr;
    attr->stacksize = stacksize;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t* attr, void** stackaddr, size_t* stacksize) {
    if (!attr || !stackaddr || !stacksize) return EINVAL;
    *stackaddr = attr->stackaddr;
    *stacksize = attr->stacksize;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t* attr, size_t guardsize) {
    if (!attr) return EINVAL;
    attr->guardsize = guardsize;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize) {
    if (!attr || !guardsize) return EINVAL;
    *guardsize = attr->guardsize;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy) {
    if (!attr) return EINVAL;
    attr->schedpolicy = policy;
    return 0;
}

int pthread_attr_getschedpolicy(const pthread_attr_t* attr, int* policy) {
    if (!attr || !policy) return EINVAL;
    *policy = attr->schedpolicy;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t* attr, const struct sched_param* param) {
    if (!attr || !param) return EINVAL;
    attr->schedparam = *param;
    return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t* attr, struct sched_param* param) {
    if (!attr || !param) return EINVAL;
    *param = attr->schedparam;
    return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t* attr, int inheritsched) {
    if (!attr) return EINVAL;
    attr->inheritsched = inheritsched;
    return 0;
}

int pthread_attr_getinheritsched(const pthread_attr_t* attr, int* inheritsched) {
    if (!attr || !inheritsched) return EINVAL;
    *inheritsched = attr->inheritsched;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t* attr, int scope) {
    if (!attr) return EINVAL;
    if (scope != PTHREAD_SCOPE_SYSTEM && scope != PTHREAD_SCOPE_PROCESS) {
        return EINVAL;
    }
    attr->scope = scope;
    return 0;
}

int pthread_attr_getscope(const pthread_attr_t* attr, int* scope) {
    if (!attr || !scope) return EINVAL;
    *scope = attr->scope;
    return 0;
}

// ============================================================================
// SCHEDULING
// ============================================================================

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param* param) {
    if (!thread || !param) return EINVAL;
    
    struct __pthread* tcb = thread;
    return sched_setscheduler(tcb->tid, policy, param);
}

int pthread_getschedparam(pthread_t thread, int* policy, struct sched_param* param) {
    if (!thread || !policy || !param) return EINVAL;
    
    struct __pthread* tcb = thread;
    int ret_policy = sched_getscheduler(tcb->tid);
    if (ret_policy < 0) return errno;
    
    *policy = ret_policy;
    return sched_getparam(tcb->tid, param);
}

int pthread_setschedprio(pthread_t thread, int prio) {
    struct sched_param param;
    int policy;
    
    int ret = pthread_getschedparam(thread, &policy, &param);
    if (ret != 0) return ret;
    
    param.sched_priority = prio;
    return pthread_setschedparam(thread, policy, &param);
}

// ============================================================================
// CPU AFFINITY
// ============================================================================

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t* cpuset) {
    if (!thread || !cpuset) return EINVAL;
    
    struct __pthread* tcb = thread;
    int ret = sched_setaffinity(tcb->tid, cpusetsize, cpuset);
    if (ret == 0) {
        tcb->cpuset = *cpuset;
        tcb->cpuset_valid = 1;
    }
    return ret == 0 ? 0 : errno;
}

int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t* cpuset) {
    if (!thread || !cpuset) return EINVAL;
    
    struct __pthread* tcb = thread;
    int ret = sched_getaffinity(tcb->tid, cpusetsize, cpuset);
    return ret == 0 ? 0 : errno;
}

int pthread_attr_setaffinity_np(pthread_attr_t* attr, size_t cpusetsize, const cpu_set_t* cpuset) {
    if (!attr || !cpuset || cpusetsize < sizeof(cpu_set_t)) return EINVAL;
    attr->cpuset = *cpuset;
    attr->cpuset_valid = 1;
    return 0;
}

int pthread_attr_getaffinity_np(const pthread_attr_t* attr, size_t cpusetsize, cpu_set_t* cpuset) {
    if (!attr || !cpuset || cpusetsize < sizeof(cpu_set_t)) return EINVAL;
    if (!attr->cpuset_valid) {
        // Return all CPUs
        CPU_ZERO(cpuset);
        for (int i = 0; i < CPU_SETSIZE; i++) {
            CPU_SET(i, cpuset);
        }
    } else {
        *cpuset = attr->cpuset;
    }
    return 0;
}

// ============================================================================
// ONCE INITIALIZATION
// ============================================================================

int pthread_once(pthread_once_t* once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    
    // States: 0 = not done, 1 = in progress, 2 = done
    if (*once_control == 2) {
        return 0;  // Already initialized
    }
    
    // Try to claim initialization
    if (__atomic_cas(once_control, 0, 1) == 0) {
        // We got it - run initializer
        init_routine();
        __sync_synchronize();
        *once_control = 2;
        // Wake any waiters
        futex_wake((int*)once_control, 0x7FFFFFFF);
    } else {
        // Wait for initialization to complete
        while (*once_control != 2) {
            futex_wait((int*)once_control, 1, NULL);
        }
    }
    
    return 0;
}

// ============================================================================
// CANCELLATION (STUB IMPLEMENTATION)
// ============================================================================

int pthread_cancel(pthread_t thread) {
    if (!thread) return EINVAL;
    
    struct __pthread* tcb = thread;
    tcb->canceled = 1;
    
    // In a full implementation, this would interrupt the thread
    // For now, just set the flag
    return 0;
}

int pthread_setcancelstate(int state, int* oldstate) {
    struct __pthread* tcb = __get_tcb();
    if (!tcb) return EINVAL;
    
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) {
        return EINVAL;
    }
    
    if (oldstate) *oldstate = tcb->cancel_state;
    tcb->cancel_state = state;
    return 0;
}

int pthread_setcanceltype(int type, int* oldtype) {
    struct __pthread* tcb = __get_tcb();
    if (!tcb) return EINVAL;
    
    if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS) {
        return EINVAL;
    }
    
    if (oldtype) *oldtype = tcb->cancel_type;
    tcb->cancel_type = type;
    return 0;
}

void pthread_testcancel(void) {
    struct __pthread* tcb = __get_tcb();
    if (tcb && tcb->canceled && tcb->cancel_state == PTHREAD_CANCEL_ENABLE) {
        pthread_exit(PTHREAD_CANCELED);
    }
}
