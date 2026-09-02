// LikeOS-64 -- the futex and robust-list syscalls.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/futex.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>

// Futex operations
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFu

// SYS_FUTEX - fast userspace mutex operations (uses hash-bucket implementation)
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
		  uint64_t timeout, uint64_t uaddr2, uint64_t val3)
{
	(void)val3; // Used for FUTEX_CMP_REQUEUE comparison value

	/*
	 * Strip the modifier bits before dispatching.
	 *
	 * FUTEX_CLOCK_REALTIME selects which clock an ABSOLUTE timeout is
	 * measured against; it is not a command.  Leaving it in the switch
	 * value meant FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME (265) matched
	 * nothing and fell through to -ENOSYS -- which is what GLib issues for
	 * every g_cond_wait_until(), so every timed condition wait in every
	 * GLib program returned instantly instead of waiting.  A caller that
	 * treats that as "timed out" then spins; GTK's file chooser loads its
	 * folder through a GThreadPool that does exactly this, and hung.
	 */
	int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
	int abs_realtime = (op & FUTEX_CLOCK_REALTIME) != 0;

	if (!validate_user_ptr(uaddr, sizeof(uint32_t))) {
		return -EFAULT;
	}

	switch (cmd) {
	case FUTEX_WAIT: {
		// Convert timeout to nanoseconds
		uint64_t timeout_ns = 0;
		int have_timeout = 0;
		if (timeout) {
			// timeout points to struct timespec
			if (validate_user_ptr(timeout,
					      sizeof(struct k_timespec))) {
				smap_disable();
				struct k_timespec *ts =
					(struct k_timespec *)timeout;
				timeout_ns =
					(uint64_t)ts->tv_sec * 1000000000ULL +
					(uint64_t)ts->tv_nsec;
				smap_enable();
				have_timeout = 1;
			}
		}
		/* A timeout of zero is "do not wait", not "wait for ever".
		 * futex_wait() reads timeout_ns == 0 as no deadline at all, so
		 * passing it through turns the shortest possible wait into an
		 * unbounded one -- and a caller reaches it by ordinary means:
		 * libc converts an absolute POSIX deadline to a relative one,
		 * and a deadline that is merely reached rather than passed
		 * converts to exactly zero.  The BITSET arm below has always
		 * refused the equivalent case; this one did not. */
		if (have_timeout && timeout_ns == 0)
			return -ETIMEDOUT;
		return futex_wait(uaddr, (uint32_t)val, timeout_ns);
	}

	case FUTEX_WAIT_BITSET: {
		/*
		 * Like FUTEX_WAIT, except the timeout is ABSOLUTE -- against
		 * CLOCK_REALTIME when FUTEX_CLOCK_REALTIME is set, else
		 * CLOCK_MONOTONIC -- and waiters carry a bitset that a
		 * FUTEX_WAKE_BITSET must match.
		 *
		 * The bitset (val3) is carried by the waiter and consulted by
		 * FUTEX_WAKE_BITSET.
		 */
		uint64_t timeout_ns = 0;

		if (timeout) {
			if (validate_user_ptr(timeout,
					      sizeof(struct k_timespec))) {
				struct k_timespec ts;

				smap_disable();
				ts = *(struct k_timespec *)timeout;
				smap_enable();

				uint64_t deadline_ns =
					(uint64_t)ts.tv_sec * 1000000000ULL +
					(uint64_t)ts.tv_nsec;
				uint64_t now_ns =
					timer_get_precise_us() * 1000ULL;

				if (abs_realtime)
					now_ns +=
						(uint64_t)
							timer_get_boot_epoch() *
						1000000000ULL;

				/* Absolute -> relative, which is what
				 * futex_wait takes.  An already-past deadline
				 * must not become an infinite wait: the 0 that
				 * futex_wait reads as "no timeout" is exactly
				 * the wrong answer, so report the timeout
				 * without sleeping. */
				if (deadline_ns <= now_ns)
					return -ETIMEDOUT;
				timeout_ns = deadline_ns - now_ns;
			}
		}
		return futex_wait_bitset(uaddr, (uint32_t)val, timeout_ns,
					 (uint32_t)val3);
	}

	case FUTEX_WAKE:
		return futex_wake(uaddr, (int)val);

	case FUTEX_WAKE_BITSET:
		return futex_wake_bitset(uaddr, (int)val, (uint32_t)val3);

	case FUTEX_WAKE_OP: {
		/* Atomically apply an operation to *uaddr2, wake `val'
		 * waiters on uaddr, and -- if the OLD value of *uaddr2
		 * satisfies the comparison -- also wake `timeout' (reused as
		 * a count) waiters on uaddr2.  How a condition variable's
		 * broadcast wakes both its waiters and the mutex's in one
		 * call. */
		if (!validate_user_ptr(uaddr2, sizeof(uint32_t)))
			return -EFAULT;

		uint32_t enc = (uint32_t)val3;
		int op = (enc >> 28) & 0xF;
		int cmp = (enc >> 24) & 0xF;
		uint32_t oparg = (enc >> 12) & 0xFFF;
		uint32_t cmparg = enc & 0xFFF;
		int shift = op & FUTEX_OP_OPARG_SHIFT;

		op &= ~FUTEX_OP_OPARG_SHIFT;
		if (shift) {
			if (oparg > 31)
				return -EINVAL;
			oparg = 1u << oparg;
		}
		if (op > FUTEX_OP_XOR || cmp > FUTEX_OP_CMP_GE)
			return -ENOSYS;

		uint32_t oldval, newval;
		volatile uint32_t *p2 = (volatile uint32_t *)uaddr2;

		smap_disable();
		do {
			oldval = *p2;
			switch (op) {
			case FUTEX_OP_SET:
				newval = oparg;
				break;
			case FUTEX_OP_ADD:
				newval = oldval + oparg;
				break;
			case FUTEX_OP_OR:
				newval = oldval | oparg;
				break;
			case FUTEX_OP_ANDN:
				newval = oldval & ~oparg;
				break;
			default:
				newval = oldval ^ oparg;
				break;
			}
		} while (!__sync_bool_compare_and_swap(p2, oldval, newval));
		smap_enable();

		int hit;
		switch (cmp) {
		case FUTEX_OP_CMP_EQ:
			hit = oldval == cmparg;
			break;
		case FUTEX_OP_CMP_NE:
			hit = oldval != cmparg;
			break;
		case FUTEX_OP_CMP_LT:
			hit = oldval < cmparg;
			break;
		case FUTEX_OP_CMP_LE:
			hit = oldval <= cmparg;
			break;
		case FUTEX_OP_CMP_GT:
			hit = oldval > cmparg;
			break;
		default:
			hit = oldval >= cmparg;
			break;
		}

		int woken = futex_wake(uaddr, (int)val);
		if (hit)
			woken += futex_wake(uaddr2, (int)timeout);
		return woken;
	}

	case FUTEX_REQUEUE:
	case FUTEX_CMP_REQUEUE:
		if (!validate_user_ptr(uaddr2, sizeof(uint32_t))) {
			return -EFAULT;
		}
		// For CMP_REQUEUE, val3 is the expected value to compare
		if (cmd == FUTEX_CMP_REQUEUE) {
			smap_disable();
			uint32_t curval = *(volatile uint32_t *)uaddr;
			smap_enable();
			if (curval != (uint32_t)val3) {
				return -EAGAIN;
			}
		}
		// val = nr_wake, timeout = nr_requeue (reusing timeout arg)
		return futex_requeue(uaddr, uaddr2, (int)val, (int)timeout);

	default:
		return -ENOSYS;
	}
}

// SYS_SET_ROBUST_LIST - set robust futex list head
int64_t sys_set_robust_list(uint64_t head, uint64_t len)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	// Validate the length matches expected structure size
	if (len != sizeof(struct robust_list_head)) {
		return -EINVAL;
	}

	if (head && !validate_user_ptr(head, len)) {
		return -EFAULT;
	}

	cur->robust_list = (struct robust_list_head *)head;
	cur->robust_list_len = len;

	return 0;
}

// SYS_GET_ROBUST_LIST - get robust futex list head
int64_t sys_get_robust_list(uint64_t pid, uint64_t head_ptr,
			    uint64_t len_ptr)
{
	task_t *target;

	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target)
		return -ESRCH;

	// Permission check: can only get own robust list or if privileged
	task_t *cur = sched_current();
	/* Through the target's PROCESS: a thread's own ->parent is NULL, so
	 * comparing it directly would refuse a parent asking about a thread of
	 * its own child -- which this used to allow. */
	task_t *tproc = target->group_leader ? target->group_leader : target;

	if (target != cur && tproc->parent != cur) {
		return -EPERM;
	}

	if (!validate_user_ptr(head_ptr, sizeof(void *)) ||
	    !validate_user_ptr(len_ptr, sizeof(size_t))) {
		return -EFAULT;
	}

	smap_disable();
	*(struct robust_list_head **)head_ptr = target->robust_list;
	*(size_t *)len_ptr = target->robust_list_len;
	smap_enable();

	return 0;
}
