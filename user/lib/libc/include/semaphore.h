/*
 * semaphore.h - POSIX semaphores (IEEE 1003.1-2001, §2.9.9)
 *
 * A counter that threads may decrement, blocking while it is zero, and
 * increment, releasing one waiter.  Unlike a mutex it has no owner: the thread
 * that posts need not be the one that waited, which is what makes it the right
 * primitive for a producer/consumer queue.  libetpan uses one to bound its
 * connection pool.
 *
 * Only UNNAMED semaphores -- sem_init() on memory the caller provides -- are
 * implemented.  The named ones (sem_open, sem_close, sem_unlink) are declared
 * but return ENOSYS: they live in a filesystem namespace shared between
 * processes, and nothing here provides one.  Declaring them keeps a program
 * that references them compiling and linking, and tells it the truth at the
 * point it actually asks.
 *
 * pshared is accepted and must be 0.  A semaphore shared between PROCESSES
 * would have to live in shared memory and be woken across address spaces;
 * asking for one gets ENOSYS rather than a semaphore that silently only works
 * within one process.
 */

#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest value a semaphore may hold.  sem_post() beyond this reports
 * EOVERFLOW rather than wrapping the count to a negative number. */
#define SEM_VALUE_MAX 2147483647

/* Returned by sem_open() on failure, as POSIX specifies. */
#define SEM_FAILED ((sem_t *)0)

/*
 * The count and the number of blocked waiters, in that order.
 *
 * Both are plain ints because the futex the implementation blocks on operates
 * on a 32-bit word, and the count IS that word -- a waiter sleeps on the
 * address of __val[0] and is woken when a post changes it.  Keeping the waiter
 * count beside it lets sem_post() skip the wake syscall when nobody is
 * blocked, which is the common case.
 *
 * Spelled as an array rather than named fields so that a program cannot come
 * to depend on the layout: it is nobody's business but this library's.
 */
typedef struct {
	volatile int __val[2];
} sem_t;

/* Prepare a semaphore with an initial count.  pshared must be 0. */
int sem_init(sem_t *sem, int pshared, unsigned int value);

/* Release one.  No memory is owned by a semaphore, so this only checks that
 * nobody is currently blocked on it -- destroying one that has waiters is
 * undefined, and reporting EBUSY is friendlier than the crash that follows. */
int sem_destroy(sem_t *sem);

/* Decrement, blocking while the count is zero.  Interruptible: a signal whose
 * handler returns makes it fail with EINTR rather than resuming, which is what
 * POSIX requires and what lets a program act on the signal. */
int sem_wait(sem_t *sem);

/* Decrement only if it can be done at once; EAGAIN otherwise. */
int sem_trywait(sem_t *sem);

/* Decrement, giving up at an ABSOLUTE time on CLOCK_REALTIME; ETIMEDOUT. */
int sem_timedwait(sem_t *sem, const struct timespec *abstime);

/* Increment, waking one waiter if any.  EOVERFLOW past SEM_VALUE_MAX. */
int sem_post(sem_t *sem);

/* The current count.  A snapshot: it may already be stale on return, which is
 * why POSIX says this is for diagnostics rather than for deciding anything. */
int sem_getvalue(sem_t *sem, int *sval);

/* Named semaphores: declared, and ENOSYS.  See the note at the top. */
sem_t *sem_open(const char *name, int oflag, ...);
int sem_close(sem_t *sem);
int sem_unlink(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _SEMAPHORE_H */
