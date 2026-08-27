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

/* Bytes the dynamic loader reserves at and above the thread pointer for this
 * structure.  The TCB lives INSIDE the TLS allocation so that %fs:0 is both
 * the ABI self-pointer and the start of struct __pthread; see rtld_init_tls()
 * in user/lib/rtld/rtld.c, where RTLD_TCB_RESERVE must hold the same value.
 * A static assertion below fails the build if the structure outgrows it. */
#define LIKEOS_TCB_RESERVE 2048

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
    size_t stack_guard;             // Stack canary — MUST stay at offset 0x28 (%fs:0x28)
    size_t stack_size;              // Total size including guard
    size_t guard_size;              // Guard page size
    
    /* Whoever CASes this 0 -> 1 owns freeing the stack, and nobody else may
     * touch it.
     *
     * Without a single claim, pthread_exit() and pthread_detach() each decided
     * independently and could BOTH decline: a detach arriving after
     * pthread_exit() had already read detach_state as JOINABLE, but before the
     * kernel cleared tid_futex, left the exit taking the joinable path (leave
     * the stack for a joiner) and the detach taking its "not finished yet"
     * path (leave it for the exit).  Since the detach is precisely the
     * statement that nobody will ever join, the stack was then orphaned -- 2 MB
     * plus a guard page per thread, for good.  A thread pool that detaches
     * each worker as it finishes hits that window constantly. */
    volatile int stack_claim;

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

    // Allocator per-thread state (owned by src/malloc/malloc.c)
    void* malloc_tcache;            // Per-thread allocation cache (or dead sentinel)
    void* malloc_arena;             // Arena this thread is attached to

    /* errno, per POSIX: one copy PER THREAD.
     *
     * It used to be a single process-wide global, which is not merely a
     * standards deviation -- it silently breaks any code that tests errno
     * after a failed call, because another thread's syscall (or one of the
     * libc functions that deliberately zero errno) can overwrite it in
     * between.  libxcb decides an X connection is dead unless a failing
     * read reports EAGAIN or EINTR, so a mail client with worker threads
     * lost its display at random: the read failed with EAGAIN as usual and
     * a different thread had already replaced the value by the time it was
     * read back.
     *
     * Carved out of the padding below, so every preceding offset is
     * unchanged -- stack_guard in particular must stay at %fs:0x28. */
    int errno_val;

    // Padding to ensure alignment
    char _pad[28];
	/* Thread name, pthread_setname_np().  Fixed 16 bytes INCLUDING the
	 * terminator, the size every conventional Unix uses, so ported code's
	 * truncation expectations hold.  At the END of the struct: the front
	 * is ABI (self at %fs:0, the canary at %fs:0x28 -- the static asserts
	 * below guard exactly that). */
	char name[16];
};

/* The loader reserves exactly LIKEOS_TCB_RESERVE bytes at the thread pointer
 * for this structure.  If it ever outgrows that, threads would scribble past
 * the end of their TLS allocation — fail the build instead. */
_Static_assert(sizeof(struct __pthread) <= LIKEOS_TCB_RESERVE,
	       "struct __pthread outgrew the loader's TCB reserve "
	       "(raise RTLD_TCB_RESERVE in user/lib/rtld/rtld.c and "
	       "LIKEOS_TCB_RESERVE here together)");

/* %fs:0 is the self-pointer and %fs:0x28 the stack canary: both are fixed by
 * the ABI and by compiler-generated code, so their offsets are not ours to
 * change. */
_Static_assert(__builtin_offsetof(struct __pthread, self) == 0,
	       "self must be at offset 0 (%fs:0)");
_Static_assert(__builtin_offsetof(struct __pthread, stack_guard) == 0x28,
	       "stack_guard must be at offset 0x28 (%fs:0x28)");

/* stack_guard must sit at exactly offset 0x28 so GCC's -fstack-protector
 * finds the canary via %fs:0x28.  Tighten the layout here if this fires. */
_Static_assert(__builtin_offsetof(struct __pthread, stack_guard) == 0x28,
               "stack_guard must be at offset 0x28 (%fs:0x28 canary slot)");

// Get current thread's TCB
static inline struct __pthread* __pthread_self(void) {
    struct __pthread* tcb;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

// TSD destructor caller (defined in pthread_tsd.c)
extern void __pthread_tsd_run_destructors(void);
extern volatile int __tsd_lock;

// Destructors for this thread's `thread_local` objects, registered through
// __cxa_thread_atexit_impl (defined in stdlib/cxa_atexit.c).  Distinct from the
// TSD destructors above: those belong to a pthread_key_t and are keyed by it,
// these belong to an object the compiler laid out in this thread's TLS block.
extern void __libc_thread_finalize(void);

#endif /* _PTHREAD_INTERNAL_H */
