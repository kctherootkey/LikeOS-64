/*
 * LikeOS-64 semaphore.c - POSIX semaphores
 *
 * Built on the futex, which is what a semaphore wants: the count IS the word a
 * blocked thread sleeps on, so a waiter is woken by the same write that makes
 * the semaphore available rather than by a separate condition variable that
 * would have to be kept in step with it.
 *
 * The fast paths take no lock at all.  A wait that finds a positive count
 * claims it with one compare-exchange, and a post with no waiters is a single
 * atomic increment -- neither enters the kernel.  Only actually blocking, and
 * waking someone who is blocked, costs a syscall.
 *
 * See <semaphore.h> for what is deliberately not implemented.
 */

#include <semaphore.h>
#include <errno.h>
#include <stddef.h>
#include <time.h>

extern int futex_wait(volatile int *uaddr, int val,
		      const struct timespec *timeout);
extern int __futex_wait_until(volatile int *uaddr, int val,
			      const struct timespec *abstime);
extern int futex_wake(volatile int *uaddr, int count);

/* __val[0] is the count and the futex word; __val[1] counts blocked threads. */
#define SEM_COUNT(s)   (&(s)->__val[0])
#define SEM_WAITERS(s) (&(s)->__val[1])

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
	if (!sem) {
		errno = EINVAL;
		return -1;
	}
	if (value > SEM_VALUE_MAX) {
		errno = EINVAL;
		return -1;
	}
	if (pshared) {
		/* Would have to live in shared memory and be woken across
		 * address spaces.  Refusing is better than returning a
		 * semaphore that works only within one process, which would
		 * fail as lost wakeups somewhere far away. */
		errno = ENOSYS;
		return -1;
	}
	__atomic_store_n(SEM_COUNT(sem), (int)value, __ATOMIC_RELEASE);
	__atomic_store_n(SEM_WAITERS(sem), 0, __ATOMIC_RELEASE);
	return 0;
}

int sem_destroy(sem_t *sem)
{
	if (!sem) {
		errno = EINVAL;
		return -1;
	}
	/* Destroying a semaphore with waiters is undefined behaviour; they
	 * would sleep on memory the caller is about to reuse.  Reporting it is
	 * more use than the fault that would otherwise follow. */
	if (__atomic_load_n(SEM_WAITERS(sem), __ATOMIC_ACQUIRE) != 0) {
		errno = EBUSY;
		return -1;
	}
	return 0;
}

int sem_trywait(sem_t *sem)
{
	int v;

	if (!sem) {
		errno = EINVAL;
		return -1;
	}
	v = __atomic_load_n(SEM_COUNT(sem), __ATOMIC_RELAXED);
	while (v > 0) {
		/* Weak is right in a loop: a spurious failure just goes round
		 * again, and v is reloaded by the compare-exchange itself. */
		if (__atomic_compare_exchange_n(SEM_COUNT(sem), &v, v - 1, 1,
						__ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return 0;
	}
	errno = EAGAIN;
	return -1;
}

/* The blocking half of sem_wait and sem_timedwait.  `abstime' is NULL to wait
 * indefinitely. */
static int sem_wait_common(sem_t *sem, const struct timespec *abstime)
{
	if (!sem) {
		errno = EINVAL;
		return -1;
	}

	for (;;) {
		int rc;

		if (sem_trywait(sem) == 0)
			return 0;

		/* Announce the intention to sleep BEFORE re-reading the count.
		 * A post that happens after this increment is guaranteed to
		 * see a non-zero waiter count and issue the wake; one that
		 * happens before it is caught by the zero-check below, which
		 * skips the sleep entirely.  Getting these two in the other
		 * order is the classic lost-wakeup. */
		__atomic_add_fetch(SEM_WAITERS(sem), 1, __ATOMIC_SEQ_CST);

		if (__atomic_load_n(SEM_COUNT(sem), __ATOMIC_SEQ_CST) > 0) {
			__atomic_sub_fetch(SEM_WAITERS(sem), 1,
					   __ATOMIC_SEQ_CST);
			continue; /* Something was posted; go and claim it. */
		}

		/* Sleeps only while the count is still zero.  If a post lands
		 * between the check above and this call, the kernel sees the
		 * mismatch and returns immediately rather than sleeping. */
		rc = __futex_wait_until(SEM_COUNT(sem), 0, abstime);
		__atomic_sub_fetch(SEM_WAITERS(sem), 1, __ATOMIC_SEQ_CST);

		if (rc < 0) {
			/* Only a wait that HAS a deadline can time out.  The
			 * kernel used to answer ETIMEDOUT for any wake it had
			 * not issued itself, which made a plain sem_wait()
			 * fail with an error POSIX does not define for it; the
			 * futex tells the reasons apart now, and this check
			 * keeps sem_wait() honest either way. */
			if (errno == ETIMEDOUT && abstime)
				return -1;
			/* A malformed deadline is the caller's error and
			 * cannot improve by being retried. */
			if (errno == EINVAL)
				return -1;
			if (errno == EINTR) {
				/* POSIX: a signal whose handler returns makes
				 * sem_wait fail rather than resume, so the
				 * program can act on the signal. */
				return -1;
			}
			/* EAGAIN means the count changed under us, which is
			 * not an error -- go round and try to claim it. */
		}
	}
}

int sem_wait(sem_t *sem)
{
	return sem_wait_common(sem, NULL);
}

int sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
	if (!abstime) {
		errno = EINVAL;
		return -1;
	}
	return sem_wait_common(sem, abstime);
}

int sem_post(sem_t *sem)
{
	int v;

	if (!sem) {
		errno = EINVAL;
		return -1;
	}

	v = __atomic_load_n(SEM_COUNT(sem), __ATOMIC_RELAXED);
	do {
		if (v >= SEM_VALUE_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
	} while (!__atomic_compare_exchange_n(SEM_COUNT(sem), &v, v + 1, 1,
					      __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED));

	/* Only enter the kernel when somebody is actually blocked.  The
	 * increment above is already visible, so a thread that registers as a
	 * waiter after this read will see the new count and not sleep. */
	if (__atomic_load_n(SEM_WAITERS(sem), __ATOMIC_SEQ_CST) > 0)
		futex_wake(SEM_COUNT(sem), 1);
	return 0;
}

int sem_getvalue(sem_t *sem, int *sval)
{
	if (!sem || !sval) {
		errno = EINVAL;
		return -1;
	}
	/* A snapshot, and POSIX says so: any other thread may change it before
	 * the caller looks at the answer.  It is for diagnostics. */
	*sval = __atomic_load_n(SEM_COUNT(sem), __ATOMIC_ACQUIRE);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Named semaphores.
 *
 * These identify a semaphore by a name in a namespace shared between
 * processes, so that unrelated programs can synchronise through it.  There is
 * no such namespace here, and inventing a private one would produce a
 * semaphore that looks shared and is not -- the failure would appear as two
 * processes never seeing each other's posts.
 *
 * ENOSYS, at the point of asking.
 * ------------------------------------------------------------------ */

sem_t *sem_open(const char *name, int oflag, ...)
{
	(void)name;
	(void)oflag;
	errno = ENOSYS;
	return SEM_FAILED;
}

int sem_close(sem_t *sem)
{
	(void)sem;
	errno = ENOSYS;
	return -1;
}

int sem_unlink(const char *name)
{
	(void)name;
	errno = ENOSYS;
	return -1;
}
