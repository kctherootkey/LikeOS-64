/*
 * LikeOS-64 POSIX Threads Implementation
 *
 * Core thread functions: create, exit, join, detach, self, equal
 * Uses clone() with thread flags and futex for join synchronization.
 */

#include <pthread.h>
#include <sys/mman.h>
#include <sched.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../syscalls/syscall.h"
#include "pthread_internal.h"


// ============================================================================
// CONSTANTS
// ============================================================================

// Default stack size: 2MB with 4KB guard page
#define PTHREAD_STACK_MIN (16 * 1024) // 16 KB minimum
#define PTHREAD_STACK_DEFAULT (2 * 1024 * 1024) // 2 MB default
#define PTHREAD_GUARD_SIZE (4 * 1024) // 4 KB guard page

// TLS block layout (dynamic sizing)
// The TLS block is allocated at the high end of the thread's stack region
// Layout: [guard page] [stack grows down] ... [TLS block] [TCB at top]
#define PTHREAD_TLS_ALIGN 16
#define PTHREAD_TCB_SIZE 256 // Thread control block

// Clone flags for thread creation
#define CLONE_THREAD_FLAGS                                                  \
	(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | \
	 CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |               \
	 CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Main thread's TCB (statically allocated for the initial thread)
/* Thread-pointer-relative TLS is laid out by ld-likeos.so, which is the only
 * component that knows which object owns which slice.  These are declared weak
 * so a program running without the loader still links; the wrappers then
 * report "no static TLS", which is exactly right for that configuration.
 * (See the TLS interface block in user/lib/rtld/rtld.c.) */
extern uint64_t _rtld_tls_size(void) __attribute__((weak));
extern uint64_t _rtld_tls_align(void) __attribute__((weak));
extern void _rtld_tls_init(void *tp) __attribute__((weak));

static size_t __rtld_tls_size(void)
{
	return _rtld_tls_size ? (size_t)_rtld_tls_size() : 0;
}

static size_t __rtld_tls_align(void)
{
	return _rtld_tls_align ? (size_t)_rtld_tls_align() : 16;
}

static void __rtld_tls_init(void *tp)
{
	if (_rtld_tls_init)
		_rtld_tls_init(tp);
}

/* Fallback control block, used only when no loader-provided one exists. */
static struct __pthread __main_thread;

/* The initial thread's control block.  It normally lives at the thread
 * pointer inside the loader's TLS allocation, NOT at __main_thread, so the
 * initial thread has to be recognised by this recorded pointer rather than
 * by comparing against that fallback object. */
static struct __pthread *__main_tcb;
static int __pthread_initialized = 0;

// Thread list for cleanup (protected by spinlock in real implementation)
static struct __pthread *__thread_list_head = NULL;
static volatile int __thread_list_lock = 0;

// TSD key management
static void (*__tsd_destructors[PTHREAD_KEYS_MAX])(void *);
static volatile int __tsd_key_used[PTHREAD_KEYS_MAX];
static volatile int __tsd_next_key = 0;
static volatile int __tsd_lock = 0;

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Simple spinlock for internal use
static inline void __spin_lock(volatile int *lock)
{
	while (__sync_lock_test_and_set(lock, 1)) {
		while (*lock) {
			__asm__ volatile("pause" ::: "memory");
		}
	}
}

static inline void __spin_unlock(volatile int *lock)
{
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
	void *base;
	size_t size;
	/* Liveness gate: points at the thread's tid_futex INSIDE this stack
	 * region (the kernel zeroes it via CLONE_CHILD_CLEARTID as the
	 * thread's very last act).  NULL means the thread is already known
	 * dead (joined).  A detached thread adds its own stack from inside
	 * pthread_exit and then KEEPS EXECUTING on it until the exit
	 * syscall; munmapping such an entry before the kernel cleared the
	 * futex yanks the stack from under the exiting thread — its next
	 * push faults on an unmapped page (observed as an intermittent
	 * SIGSEGV at RSP-8 in the detached thread whenever the main thread
	 * reached the next pthread_create within that window). */
	volatile int *alive;
};

static struct zombie_stack __zombie_stacks[ZOMBIE_STACK_MAX];
static volatile int __zombie_count = 0;
static volatile int __zombie_lock = 0;

/* ---- thread stack cache -----------------------------------------------
 *
 * Keep a few finished stacks mapped and hand them straight back to the next
 * pthread_create, instead of unmapping each one and mapping a fresh one.
 *
 * A thread stack is 2MB plus a guard page, so a create/join loop otherwise
 * asks the kernel to build and tear down a 2MB mapping every time round.  That
 * is expensive on its own, and it is also the churn that address-space races
 * live in: the more mapping and unmapping a program does, the more chances a
 * fault has to collide with one.  Reusing the mapping removes both.
 *
 * Only JOINED stacks are cached.  pthread_join has already waited for the
 * kernel to clear the thread's tid_futex, so the thread is provably no longer
 * executing on it.  A DETACHED thread is still running on its own stack when
 * it releases it, and must keep using __unmapself -- handing that stack to
 * another thread would let the new owner start writing to it while the old one
 * is still finishing its exit.
 *
 * The pages stay mapped and keep their contents, which is fine: a stack is
 * written before it is read, and the new thread's control block is placed over
 * the old one.  The guard page is already PROT_NONE and stays that way.
 */
#define STACK_CACHE_MAX 8
#define STACK_CACHE_MAX_BYTES (16 * 1024 * 1024)

struct cached_stack {
	void *base;
	size_t size;
};

static struct cached_stack __stack_cache[STACK_CACHE_MAX];
static int __stack_cache_count;
static size_t __stack_cache_bytes;

/* Caller holds __zombie_lock.  Returns a stack of exactly `size`, or NULL. */
static void *__stack_cache_take_locked(size_t size)
{
	for (int i = 0; i < __stack_cache_count; i++) {
		if (__stack_cache[i].size != size)
			continue;
		void *base = __stack_cache[i].base;

		__stack_cache_bytes -= __stack_cache[i].size;
		__stack_cache[i] = __stack_cache[--__stack_cache_count];
		__stack_cache[__stack_cache_count].base = NULL;
		__stack_cache[__stack_cache_count].size = 0;
		return base;
	}
	return NULL;
}

/* Caller holds __zombie_lock.  Returns 1 if the stack was kept (and so must
 * NOT be unmapped), 0 if the caller should unmap it. */
static int __stack_cache_put_locked(void *base, size_t size)
{
	if (!base || __stack_cache_count >= STACK_CACHE_MAX)
		return 0;
	if (__stack_cache_bytes + size > STACK_CACHE_MAX_BYTES)
		return 0;
	__stack_cache[__stack_cache_count].base = base;
	__stack_cache[__stack_cache_count].size = size;
	__stack_cache_count++;
	__stack_cache_bytes += size;
	return 1;
}

/* Free every entry whose thread has really exited (kernel cleared the
 * tid_futex, or no gate at all); keep the rest.  Caller holds
 * __zombie_lock.  Reading *alive is safe precisely because the entry has
 * not been munmapped yet. */
static void __zombie_stack_reap_locked(void)
{
	int keep = 0;
	for (int i = 0; i < __zombie_count; i++) {
		if (!__zombie_stacks[i].base)
			continue;
		if (__zombie_stacks[i].alive &&
		    *__zombie_stacks[i].alive != 0) {
			// Thread still finishing pthread_exit on this stack.
			__zombie_stacks[keep++] = __zombie_stacks[i];
			continue;
		}
		munmap(__zombie_stacks[i].base, __zombie_stacks[i].size);
	}
	for (int i = keep; i < __zombie_count; i++) {
		__zombie_stacks[i].base = NULL;
		__zombie_stacks[i].size = 0;
		__zombie_stacks[i].alive = NULL;
	}
	__zombie_count = keep;
}

// Add a stack to the zombie list for deferred cleanup
static void __zombie_stack_add(void *base, size_t size, volatile int *alive)
{
	if (!base || size == 0)
		return;

	__spin_lock(&__zombie_lock);

	if (__zombie_count >= ZOMBIE_STACK_MAX) {
		// Try to make room by reaping entries that are really dead.
		__zombie_stack_reap_locked();
	}
	if (__zombie_count < ZOMBIE_STACK_MAX) {
		__zombie_stacks[__zombie_count].base = base;
		__zombie_stacks[__zombie_count].size = size;
		__zombie_stacks[__zombie_count].alive = alive;
		__zombie_count++;
	} else if (!alive || *alive == 0) {
		// List still full but this thread is provably dead: free now.
		munmap(base, size);
	}
	/* else: list full AND the thread may still be running on this stack.
	 * Leak it — a leaked stack is recoverable, a munmapped live stack is
	 * a crash. */

	__spin_unlock(&__zombie_lock);
}

// Cleanup all zombie stacks (called before creating new threads)
static void __zombie_stack_cleanup(void)
{
	__spin_lock(&__zombie_lock);
	__zombie_stack_reap_locked();

	__spin_unlock(&__zombie_lock);
}

// ============================================================================
// MORE INTERNAL HELPERS
// ============================================================================

// Atomic compare-and-swap
static inline int __atomic_cas(volatile int *ptr, int old_val, int new_val)
{
	return __sync_val_compare_and_swap(ptr, old_val, new_val);
}

// Futex operations (from sched.c wrappers)
extern int futex_wait(volatile int *uaddr, int val,
		      const struct timespec *timeout);
extern int futex_wake(volatile int *uaddr, int count);

// Get current thread's TCB
static inline struct __pthread *__get_tcb(void)
{
	struct __pthread *tcb;
	// Read TCB pointer from FS:0 (self pointer stored at offset 0)
	__asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
	return tcb;
}

// Set TLS base (FS segment)
static inline int __set_tls(void *addr)
{
	return arch_prctl(ARCH_SET_FS, (unsigned long)addr);
}

/* ============================================================================
 * Fork hooks — called from fork() in unistd.c alongside the malloc hooks.
 *
 * All pthread global locks are acquired across the fork so the parent's
 * thread bookkeeping is consistent at the fork instant, and the state is
 * REINITIALISED in the child: after fork only the calling thread exists
 * there, and a lock captured mid-critical-section by another thread would
 * otherwise be inherited locked forever.  (Observed: a child forked from a
 * thread while another thread was inside pthread_exit's detached cleanup
 * spun forever on __thread_list_lock when its own pthread_exit ran.)
 * ============================================================================ */
void __pthread_fork_prepare(void)
{
	__spin_lock(&__thread_list_lock);
	__spin_lock(&__tsd_lock);
	__spin_lock(&__zombie_lock);
}

void __pthread_fork_parent(void)
{
	__spin_unlock(&__zombie_lock);
	__spin_unlock(&__tsd_lock);
	__spin_unlock(&__thread_list_lock);
}

void __pthread_fork_child(void)
{
	/* The child is single-threaded: force every pthread lock free. */
	__zombie_lock = 0;
	__tsd_lock = 0;
	__thread_list_lock = 0;

	if (__pthread_initialized) {
		/* Only the calling thread survives fork — reset the thread
		 * list to a self-linked singleton so pthread_exit /
		 * pthread_create in the child never walk the parent's stale
		 * sibling entries. */
		struct __pthread *self = __get_tcb();
		if (self) {
			self->next = self;
			self->prev = self;
			__thread_list_head = self;
		}
		/* Zombie stacks recorded in the parent belong to parent
		 * threads; drop them rather than munmap addresses the child
		 * did not create. */
		for (int zi = 0; zi < __zombie_count; zi++) {
			__zombie_stacks[zi].base = NULL;
			__zombie_stacks[zi].size = 0;
			__zombie_stacks[zi].alive = NULL;
		}
		__zombie_count = 0;

		/* The stack CACHE is deliberately kept.  Unlike a zombie entry,
		 * a cached stack has no thread running on it -- it was left by
		 * one that had already been joined -- and fork copied the
		 * mapping, so the address is just as valid here as in the
		 * parent.  Dropping it would leak those mappings in the child
		 * for no reason.  The two address spaces are independent from
		 * now on, so both may reuse their own copy. */
	}
}

// Initialize the main thread's TCB (called lazily)
static void __pthread_init_main(void)
{
	if (__pthread_initialized)
		return;

	/* Capture the existing %fs:0x28 canary before we change FS.
     * The rtld has placed a non-zero canary there; any stack-protected
     * frame entered before this call (e.g. main()) has saved that value
     * at [rbp-8].  We must keep the same value at offset 0x28 of the new
     * TCB so all pending epilogue checks still pass. */
	size_t old_canary;
	__asm__ volatile("mov %%fs:0x28, %0" : "=r"(old_canary));

	/* Adopt the thread pointer the loader already installed rather than
	 * pointing %fs somewhere else.
	 *
	 * The loader lays out [ static TLS ][ tp ][ TCB reserve ] and sets
	 * %fs = tp, so the reserved area at %fs IS where this structure
	 * belongs.  Putting it in .bss instead — which is what used to happen
	 * — moved %fs off the TLS block entirely, and every __thread access at
	 * %fs:-N then read whatever happened to precede that .bss object.
	 * There are no __thread variables in the tree today, which is the only
	 * reason this was survivable. */
	struct __pthread *main = __get_tcb();
	if (!main) {
		/* No loader-provided block (a statically linked program, or a
		 * loader too old to reserve one): fall back to the .bss copy.
		 * __thread data does not work in that configuration, but
		 * everything that does not use it still does. */
		main = &__main_thread;
	}

	// Zero out
	for (size_t i = 0; i < sizeof(*main); i++) {
		((char *)main)[i] = 0;
	}

	main->self = main;
	main->tid = gettid();
	main->tid_futex = main->tid;
	main->state = THREAD_STATE_RUNNING;
	main->detach_state = PTHREAD_CREATE_JOINABLE;
	main->stack_base = NULL; // Main thread's stack is special
	main->stack_guard =
		old_canary; // preserve canary at offset 0x28 (%fs:0x28)
	main->stack_size = 0;
	main->guard_size = 0;
	main->cancel_state = PTHREAD_CANCEL_ENABLE;
	main->cancel_type = PTHREAD_CANCEL_DEFERRED;

	// Initialize robust list
	main->robust_list.list.next = &main->robust_list.list;
	main->robust_list.futex_offset = (long)&((pthread_mutex_t *)0)->state;
	main->robust_list.list_op_pending = NULL;

	// Register robust list with kernel
	set_robust_list(&main->robust_list, sizeof(main->robust_list));

	/* %fs already points here when the loader supplied the block; only the
	 * fallback above needs the register changed. */
	if (main == &__main_thread)
		__set_tls(main);

	// Add to thread list
	main->next = main->prev = main;
	__thread_list_head = main;

	__main_tcb = main;
	__pthread_initialized = 1;
}

// Thread startup wrapper - called by clone trampoline
static int __pthread_start(void *arg)
{
	struct __pthread *tcb = (struct __pthread *)arg;

	// Set TLS to our TCB
	__set_tls(tcb);

	// Register robust futex list with kernel
	set_robust_list(&tcb->robust_list, sizeof(tcb->robust_list));

	// Apply CPU affinity if specified
	if (tcb->cpuset_valid) {
		sched_setaffinity(0, sizeof(cpu_set_t), &tcb->cpuset);
	}

	// Call the user's thread function
	void *retval = tcb->start_routine(tcb->start_arg);

	// Thread function returned - call pthread_exit
	pthread_exit(retval);

	// Never reached
	return 0;
}

// Call TSD destructors
static void __call_tsd_destructors(struct __pthread *tcb)
{
// POSIX requires up to PTHREAD_DESTRUCTOR_ITERATIONS attempts
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

	for (int iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
		int any_called = 0;

		for (unsigned int key = 0; key < PTHREAD_KEYS_MAX; key++) {
			void *value = tcb->tsd_values[key];
			void (*destructor)(void *) = __tsd_destructors[key];

			if (value && destructor && __tsd_key_used[key]) {
				tcb->tsd_values[key] = NULL;
				destructor(value);
				any_called = 1;
			}
		}

		if (!any_called)
			break;
	}
}

// ============================================================================
// PUBLIC API: THREAD CREATION AND MANAGEMENT
// ============================================================================

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
		   void *(*start_routine)(void *), void *arg)
{
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
	void *stack_addr = NULL;

	if (attr) {
		if (attr->stacksize >= PTHREAD_STACK_MIN) {
			stack_size = attr->stacksize;
		}
		detach_state = attr->detachstate;
		if (attr->stackaddr) {
			stack_addr = attr->stackaddr;
			guard_size = 0; // User-provided stack, no guard page
		}
		if (attr->cpuset_valid) {
			cpuset = attr->cpuset;
			cpuset_valid = 1;
		}
	}

	/* Calculate total allocation size.
	 *
	 * Layout: [guard] [stack grows down] [static TLS] [tp = TCB]
	 *
	 * The per-thread TLS area sits immediately below the TCB, because
	 * __thread variables are addressed at negative offsets from the thread
	 * pointer and the TCB is at that pointer.  A thread whose block lacked
	 * this room would read and write past the bottom of its own TCB. */
	size_t tls_size = __rtld_tls_size();
	size_t tls_align = __rtld_tls_align();
	if (tls_align < PTHREAD_TLS_ALIGN)
		tls_align = PTHREAD_TLS_ALIGN;

	size_t total_size = guard_size + stack_size + tls_size + tls_align +
			    sizeof(struct __pthread);
	total_size = (total_size + 4095) & ~4095UL; // Page align

	// Allocate stack + TCB region
	void *stack_base;
	if (stack_addr) {
		// User provided stack
		stack_base = stack_addr;
	} else {
		/* A stack left over from a joined thread of the same size is
		 * ready to use as-is: still mapped, guard page still
		 * PROT_NONE.  Reusing it saves building and tearing down a 2MB
		 * mapping for every thread. */
		__spin_lock(&__zombie_lock);
		stack_base = __stack_cache_take_locked(total_size);
		__spin_unlock(&__zombie_lock);

		if (!stack_base) {
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
	}

	// Place TCB at high end of allocation
	struct __pthread *tcb =
		(struct __pthread *)((char *)stack_base + total_size -
				     sizeof(struct __pthread));

	/* Align the thread pointer down.  The static TLS area is [tcb -
	 * tls_size, tcb), so the alignment has to satisfy the strictest
	 * __thread variable in the process, not just the TCB's own. */
	tcb = (struct __pthread *)((unsigned long)tcb & ~(tls_align - 1));

	// Initialize TCB
	for (size_t i = 0; i < sizeof(*tcb); i++) {
		((char *)tcb)[i] = 0;
	}

	/* Fill this thread's TLS slice with each object's initial image.  The
	 * loader owns the mapping of offsets to objects, so it does the copy. */
	if (tls_size)
		__rtld_tls_init(tcb);

	tcb->self = tcb;
	tcb->state = THREAD_STATE_RUNNING;
	tcb->retval = NULL;
	tcb->stack_base = stack_base;
	tcb->stack_guard =
		__get_tcb()->stack_guard; // inherit canary from creating thread
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
	tcb->robust_list.futex_offset = (long)&((pthread_mutex_t *)0)->state;
	tcb->robust_list.list_op_pending = NULL;

	/* Calculate the child's stack pointer.
	 *
	 * The allocation is laid out
	 *
	 *     [guard][stack grows down ...][static TLS][tp = TCB]
	 *                                  ^ child_stack
	 *
	 * so the stack must start BELOW the thread's static TLS area, not
	 * directly below the TCB.  Starting it at the TCB lets the stack grow
	 * straight through the TLS slice: the thread's __thread writes and its
	 * call frames then occupy the same bytes, which shows up as corrupted
	 * locals and a wild return address rather than as anything resembling a
	 * TLS problem. */
	void *child_stack =
		(void *)((((unsigned long)tcb) - tls_size) & ~15UL);

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
			__thread_list_head =
				(tcb->next != tcb) ? tcb->next : NULL;
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

void pthread_exit(void *retval)
{
	struct __pthread *tcb = __get_tcb();

	if (!tcb || tcb == __main_tcb || tcb == &__main_thread) {
		// Main thread exiting - exit the entire process
		_exit((long)retval);
	}

	// Store return value
	tcb->retval = retval;

	// Full memory barrier to ensure retval is visible to other CPUs
	// before we mark the thread as exited or clear tid_futex
	__sync_synchronize();

	// Destructors of this thread's thread_local objects, registered through
	// __cxa_thread_atexit_impl.  They run BEFORE the pthread_key_create
	// destructors below and well before the stack is released: what they
	// destroy is this thread's own storage, and once the thread has been
	// reaped there is nothing left to run them against.
	__libc_thread_finalize();

	// Call TSD destructors
	__call_tsd_destructors(tcb);

	// Mark as exited (before exit so joiners see it)
	tcb->state = THREAD_STATE_EXITED;

	// If this is a detached thread, free our OWN stack and exit without
	// ever touching it again.  The old approach queued the stack on a
	// shared list and let the next pthread_create munmap it, gated on our
	// tid_futex — but that flag lives INSIDE this stack, so the reaper had
	// to dereference a pointer into a region that could already be gone,
	// which faulted (SIGSEGV in __zombie_stack_reap_locked).  __unmapself
	// does munmap(stack)+exit() in raw asm using only registers, so no one
	// else ever reads into a stack a thread is/was running on.
	if (tcb->detach_state == PTHREAD_CREATE_DETACHED && tcb->stack_base) {
		// Remove from thread list first (its links are in the TCB, which
		// is inside the stack we are about to unmap).
		__spin_lock(&__thread_list_lock);
		tcb->prev->next = tcb->next;
		tcb->next->prev = tcb->prev;
		if (__thread_list_head == tcb) {
			__thread_list_head =
				(tcb->next != tcb) ? tcb->next : NULL;
		}
		__spin_unlock(&__thread_list_lock);

		/* Forward our retval as the exit code (matches the old
		 * _exit((long)retval)): SYS_EXIT ends only this thread, but if
		 * we are the last thread it becomes the process exit status. */
		extern void __unmapself(void *stack_base, size_t size,
					int exit_code);
		__unmapself(tcb->stack_base, tcb->stack_size,
			    (int)(long)retval); // never returns
	}

	// Joinable thread: keep the stack mapped so the joiner can read the
	// TCB; the kernel clears tid_futex (CLONE_CHILD_CLEARTID) and wakes the
	// joiner, which frees the stack via the zombie list (alive == NULL,
	// i.e. never dereferenced).
	//
	// __thread_exit, NOT _exit: this ends one thread.  _exit() ends the
	// whole process (it issues SYS_EXIT_GROUP), which is right for the main
	// thread above and fatal here.
	__thread_exit((int)(long)retval);

	// Never reached
	__builtin_unreachable();
}

int pthread_join(pthread_t thread, void **retval)
{
	if (!__pthread_initialized) {
		__pthread_init_main();
	}

	if (!thread) {
		return EINVAL;
	}

	struct __pthread *tcb = thread;
	struct __pthread *self = __get_tcb();

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
		if (val == 0)
			break;

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
	void *stack_base = tcb->stack_base;
	size_t stack_size = tcb->stack_size;

	// Remove from thread list
	__spin_lock(&__thread_list_lock);
	tcb->prev->next = tcb->next;
	tcb->next->prev = tcb->prev;
	if (__thread_list_head == tcb) {
		__thread_list_head = (tcb->next != tcb) ? tcb->next : NULL;
	}
	__spin_unlock(&__thread_list_lock);

	/*
	 * Free the stack NOW.
	 *
	 * The deferral this used to do exists for DETACHED threads, where the
	 * exiting thread is still running on its own stack and only __unmapself
	 * may take it away.  A JOINED thread is not in that position: the join
	 * above waited for tid_futex to reach 0, which the kernel clears once
	 * the thread is gone, so there is nothing left to run on it.
	 *
	 * Deferring anyway meant a joined stack sat on the zombie list until
	 * some LATER pthread_create came along to sweep it -- and if the program
	 * stopped making threads, up to ZOMBIE_STACK_MAX of them stayed mapped
	 * for good.  At 2MB plus a guard each that is 128MB of address space,
	 * and, worse, 64 kernel mmap-region slots: Claws Mail ran the table out
	 * at 512 entries, and the dlopen() that then failed was the spell
	 * checker, which is why it silently stopped working.
	 *
	 * Sweep the list as well, so stacks left by earlier detached threads do
	 * not wait for the next pthread_create either.
	 */
	if (stack_base) {
		/* Keep it for the next thread rather than giving it back.  Safe
		 * precisely because this thread is joined: the wait above saw
		 * tid_futex reach 0, so nothing is executing on it any more.
		 * If the cache is full it is unmapped as before. */
		int cached;

		__spin_lock(&__zombie_lock);
		cached = __stack_cache_put_locked(stack_base, stack_size);
		__spin_unlock(&__zombie_lock);
		if (!cached)
			munmap(stack_base, stack_size);
	}
	__zombie_stack_cleanup();

	return 0;
}

int pthread_detach(pthread_t thread)
{
	if (!thread) {
		return EINVAL;
	}

	struct __pthread *tcb = thread;

	// Atomically set detached state
	int old_state =
		__atomic_cas(&tcb->detach_state, PTHREAD_CREATE_JOINABLE,
			     PTHREAD_CREATE_DETACHED);

	if (old_state == PTHREAD_CREATE_DETACHED) {
		return EINVAL; // Already detached
	}

	// If the thread already exited while still joinable (exited before we
	// ran the CAS above), pthread_exit didn't enqueue the stack for cleanup.
	// Now that we own the DETACHED transition we must do it.
	// Use tid_futex == 0 as the authoritative "kernel has finished with
	// this thread" signal (CLONE_CHILD_CLEARTID writes 0 on exit).
	if (tcb->tid_futex == 0 && tcb->stack_base) {
		__spin_lock(&__thread_list_lock);
		tcb->prev->next = tcb->next;
		tcb->next->prev = tcb->prev;
		if (__thread_list_head == tcb) {
			__thread_list_head =
				(tcb->next != tcb) ? tcb->next : NULL;
		}
		__spin_unlock(&__thread_list_lock);

		/* We only get here with tid_futex == 0, i.e. the kernel has
		 * finished with this thread (CLONE_CHILD_CLEARTID wrote 0 on
		 * exit) and it is provably off its stack.  So the stack is safe
		 * to free now: enqueue with alive == NULL (the reaper frees it
		 * without ever dereferencing into the stack). */
		__zombie_stack_add(tcb->stack_base, tcb->stack_size, NULL);
	}

	return 0;
}

pthread_t pthread_self(void)
{
	if (!__pthread_initialized) {
		__pthread_init_main();
	}
	return __get_tcb();
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
	return t1 == t2;
}

// ============================================================================
// THREAD ATTRIBUTES
// ============================================================================

int pthread_attr_init(pthread_attr_t *attr)
{
	if (!attr)
		return EINVAL;

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

int pthread_attr_destroy(pthread_attr_t *attr)
{
	if (!attr)
		return EINVAL;
	// Nothing to free
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
	if (!attr)
		return EINVAL;
	if (detachstate != PTHREAD_CREATE_JOINABLE &&
	    detachstate != PTHREAD_CREATE_DETACHED) {
		return EINVAL;
	}
	attr->detachstate = detachstate;
	return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
	if (!attr || !detachstate)
		return EINVAL;
	*detachstate = attr->detachstate;
	return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
	if (!attr)
		return EINVAL;
	if (stacksize < PTHREAD_STACK_MIN)
		return EINVAL;
	attr->stacksize = stacksize;
	return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
	if (!attr || !stacksize)
		return EINVAL;
	*stacksize = attr->stacksize;
	return 0;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr,
			  size_t stacksize)
{
	if (!attr)
		return EINVAL;
	if (stacksize < PTHREAD_STACK_MIN)
		return EINVAL;
	attr->stackaddr = stackaddr;
	attr->stacksize = stacksize;
	return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
			  size_t *stacksize)
{
	if (!attr || !stackaddr || !stacksize)
		return EINVAL;
	*stackaddr = attr->stackaddr;
	*stacksize = attr->stacksize;
	return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
	if (!attr)
		return EINVAL;
	attr->guardsize = guardsize;
	return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize)
{
	if (!attr || !guardsize)
		return EINVAL;
	*guardsize = attr->guardsize;
	return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
	if (!attr)
		return EINVAL;
	attr->schedpolicy = policy;
	return 0;
}

int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy)
{
	if (!attr || !policy)
		return EINVAL;
	*policy = attr->schedpolicy;
	return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr,
			       const struct sched_param *param)
{
	if (!attr || !param)
		return EINVAL;
	attr->schedparam = *param;
	return 0;
}

int pthread_attr_getschedparam(const pthread_attr_t *attr,
			       struct sched_param *param)
{
	if (!attr || !param)
		return EINVAL;
	*param = attr->schedparam;
	return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched)
{
	if (!attr)
		return EINVAL;
	attr->inheritsched = inheritsched;
	return 0;
}

int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inheritsched)
{
	if (!attr || !inheritsched)
		return EINVAL;
	*inheritsched = attr->inheritsched;
	return 0;
}

int pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
	if (!attr)
		return EINVAL;
	if (scope != PTHREAD_SCOPE_SYSTEM && scope != PTHREAD_SCOPE_PROCESS) {
		return EINVAL;
	}
	attr->scope = scope;
	return 0;
}

int pthread_attr_getscope(const pthread_attr_t *attr, int *scope)
{
	if (!attr || !scope)
		return EINVAL;
	*scope = attr->scope;
	return 0;
}

// ============================================================================
// SCHEDULING
// ============================================================================

int pthread_setschedparam(pthread_t thread, int policy,
			  const struct sched_param *param)
{
	if (!thread || !param)
		return EINVAL;

	struct __pthread *tcb = thread;
	return sched_setscheduler(tcb->tid, policy, param);
}

int pthread_getschedparam(pthread_t thread, int *policy,
			  struct sched_param *param)
{
	if (!thread || !policy || !param)
		return EINVAL;

	struct __pthread *tcb = thread;
	int ret_policy = sched_getscheduler(tcb->tid);
	if (ret_policy < 0)
		return errno;

	*policy = ret_policy;
	return sched_getparam(tcb->tid, param);
}

int pthread_setschedprio(pthread_t thread, int prio)
{
	struct sched_param param;
	int policy;

	int ret = pthread_getschedparam(thread, &policy, &param);
	if (ret != 0)
		return ret;

	param.sched_priority = prio;
	return pthread_setschedparam(thread, policy, &param);
}

// ============================================================================
// CPU AFFINITY
// ============================================================================

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
			   const cpu_set_t *cpuset)
{
	if (!thread || !cpuset)
		return EINVAL;

	struct __pthread *tcb = thread;
	int ret = sched_setaffinity(tcb->tid, cpusetsize, cpuset);
	if (ret == 0) {
		tcb->cpuset = *cpuset;
		tcb->cpuset_valid = 1;
	}
	return ret == 0 ? 0 : errno;
}

int pthread_getaffinity_np(pthread_t thread, size_t cpusetsize,
			   cpu_set_t *cpuset)
{
	if (!thread || !cpuset)
		return EINVAL;

	struct __pthread *tcb = thread;
	int ret = sched_getaffinity(tcb->tid, cpusetsize, cpuset);
	return ret == 0 ? 0 : errno;
}

int pthread_attr_setaffinity_np(pthread_attr_t *attr, size_t cpusetsize,
				const cpu_set_t *cpuset)
{
	if (!attr || !cpuset || cpusetsize < sizeof(cpu_set_t))
		return EINVAL;
	attr->cpuset = *cpuset;
	attr->cpuset_valid = 1;
	return 0;
}

int pthread_attr_getaffinity_np(const pthread_attr_t *attr, size_t cpusetsize,
				cpu_set_t *cpuset)
{
	if (!attr || !cpuset || cpusetsize < sizeof(cpu_set_t))
		return EINVAL;
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

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
	if (!once_control || !init_routine)
		return EINVAL;

	// States: 0 = not done, 1 = in progress, 2 = done
	if (*once_control == 2) {
		return 0; // Already initialized
	}

	// Try to claim initialization
	if (__atomic_cas(once_control, 0, 1) == 0) {
		// We got it - run initializer
		init_routine();
		__sync_synchronize();
		*once_control = 2;
		// Wake any waiters
		futex_wake((int *)once_control, 0x7FFFFFFF);
	} else {
		// Wait for initialization to complete
		while (*once_control != 2) {
			futex_wait((int *)once_control, 1, NULL);
		}
	}

	return 0;
}

// ============================================================================
// CANCELLATION (STUB IMPLEMENTATION)
// ============================================================================

int pthread_cancel(pthread_t thread)
{
	if (!thread)
		return EINVAL;

	struct __pthread *tcb = thread;
	tcb->canceled = 1;

	// In a full implementation, this would interrupt the thread
	// For now, just set the flag
	return 0;
}

int pthread_setcancelstate(int state, int *oldstate)
{
	struct __pthread *tcb = __get_tcb();
	if (!tcb)
		return EINVAL;

	if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) {
		return EINVAL;
	}

	if (oldstate)
		*oldstate = tcb->cancel_state;
	tcb->cancel_state = state;
	return 0;
}

int pthread_setcanceltype(int type, int *oldtype)
{
	struct __pthread *tcb = __get_tcb();
	if (!tcb)
		return EINVAL;

	if (type != PTHREAD_CANCEL_DEFERRED &&
	    type != PTHREAD_CANCEL_ASYNCHRONOUS) {
		return EINVAL;
	}

	if (oldtype)
		*oldtype = tcb->cancel_type;
	tcb->cancel_type = type;
	return 0;
}

void pthread_testcancel(void)
{
	struct __pthread *tcb = __get_tcb();
	if (tcb && tcb->canceled &&
	    tcb->cancel_state == PTHREAD_CANCEL_ENABLE) {
		pthread_exit(PTHREAD_CANCELED);
	}
}

/* pthread_sigmask(): examine or change the calling thread's signal mask.
 *
 * Signal masks are per-task in this kernel and a pthread IS a task (threads
 * are created with clone()), so sigprocmask() already operates on exactly the
 * calling thread.  The two calls therefore do the same work; they differ only
 * in how they report failure — pthread_sigmask returns the error number and
 * leaves errno alone, which is what POSIX specifies for the pthread_* family. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
	int saved = errno;
	int rc;

	errno = 0;
	rc = sigprocmask(how, set, oldset);
	if (rc != 0) {
		int err = errno ? errno : EINVAL;
		errno = saved;
		return err;
	}
	errno = saved;
	return 0;
}
