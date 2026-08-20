/*
 * waitq.h - wait queues, one per thing that can be waited FOR.
 *
 * The conventional arrangement, and the reason for it:
 *
 * A task that blocks does so on some particular object -- this socket, that
 * pipe, this terminal.  The queue of waiters therefore belongs to the OBJECT,
 * not to the scheduler, and waking it walks only the tasks that asked about
 * that object.  Cost is proportional to the number of waiters on the thing
 * that became ready, and to nothing else.
 *
 * What this replaces in the poll layer was the opposite: every poll(), select()
 * and epoll_wait() in the system parked on one global channel, and every I/O
 * event anywhere woke ALL of them by scanning the entire task list with
 * interrupts disabled.  A machine with twenty pollers turned one byte on one
 * socket into twenty wakeups, twenty descriptor re-scans and a task-list walk,
 * so the cost of doing anything grew with the number of processes running --
 * measured at ten wakeups per event, sixteen thousand a second, on an
 * otherwise idle desktop.  An idle xterm cost as much as a busy one, because
 * what it added was a task to walk, not work to do.
 *
 * Structure: a doubly linked list threaded through entries the WAITER owns,
 * so unlinking is O(1) and the queue head allocates nothing.  `pprev' points
 * at whatever pointer refers to this entry -- the head's `first' for the first
 * entry, the previous entry's `next' otherwise -- which is what lets a removal
 * work without walking from the front.
 *
 * Locking: each head has its own spinlock, taken with interrupts disabled
 * because producers wake from interrupt context (an arriving packet, a
 * keystroke).  The lock covers the list only; task states are changed through
 * the scheduler's own claim protocol, which is what makes a concurrent wake
 * and timeout safe.
 */
#ifndef _KERNEL_WAITQ_H_
#define _KERNEL_WAITQ_H_

/* spinlock_t, the irqsave helpers, task_t, sched_claim_wake() and
 * sched_enqueue_ready() all come from here. */
#include <kernel/ke/sched.h>

struct wait_queue_entry {
	struct wait_queue_entry *next;
	struct wait_queue_entry **pprev; /* NULL when not queued */
	task_t *task;
};

struct wait_queue_head {
	struct wait_queue_entry *first;
	spinlock_t lock;
};

/* A head must be initialised before use.  Objects that are zeroed on
 * allocation get a usable head from that alone -- first NULL is the empty
 * list -- but the spinlock wants its name, so say so explicitly wherever the
 * object is created. */
static inline void wq_head_init(struct wait_queue_head *h, const char *name)
{
	h->first = NULL;
	spinlock_init(&h->lock, name);
}

/* Initialise a head that lives in a slot which gets RECYCLED.
 *
 * A queue must never be re-initialised while entries are on it: clearing
 * `first' orphans them, and the next wq_remove() then writes through a pprev
 * that refers to nothing, corrupting whichever list occupies the slot after
 * it.  A poller registers only for the length of one poll() call, but nothing
 * stops another thread closing the descriptor underneath it -- so a slot CAN
 * be reused with a waiter still attached.
 *
 * The head therefore outlives the object in the slot: the lock is named once
 * and the list is never cleared.  A stale waiter gets a spurious wake, which
 * it answers by re-scanning and finding nothing -- the cheap outcome.
 *
 * Callers that zero the whole containing struct must save and restore the head
 * around that; this cannot do it for them.
 */
static inline void wq_head_init_once(struct wait_queue_head *h,
				     const char *name)
{
	if (!h->lock.name)
		spinlock_init(&h->lock, name);
}

static inline void wq_entry_init(struct wait_queue_entry *e, task_t *t)
{
	e->next = NULL;
	e->pprev = NULL;
	e->task = t;
}

/* True when the entry is currently on some queue. */
static inline int wq_entry_queued(const struct wait_queue_entry *e)
{
	return e->pprev != NULL;
}

/* Put `e' on `h'.  Adding an entry that is already queued is a caller error;
 * it would corrupt whichever list it is on, so it is refused rather than
 * silently done twice. */
static inline void wq_add(struct wait_queue_head *h,
			  struct wait_queue_entry *e)
{
	uint64_t flags;

	if (wq_entry_queued(e))
		return;
	spin_lock_irqsave(&h->lock, &flags);
	e->next = h->first;
	if (e->next)
		e->next->pprev = &e->next;
	h->first = e;
	e->pprev = &h->first;
	spin_unlock_irqrestore(&h->lock, flags);
}

static inline void wq_remove(struct wait_queue_head *h,
			     struct wait_queue_entry *e)
{
	uint64_t flags;

	spin_lock_irqsave(&h->lock, &flags);
	if (e->pprev) {
		*e->pprev = e->next;
		if (e->next)
			e->next->pprev = e->pprev;
		e->next = NULL;
		e->pprev = NULL;
	}
	spin_unlock_irqrestore(&h->lock, flags);
}

/* Wake everything on `h'.  Returns how many tasks were made runnable.
 *
 * The tasks are collected under the lock and enqueued after it is dropped:
 * sched_enqueue_ready() takes run-queue locks, and taking those beneath a
 * wait-queue lock would fix an ordering between the two that every other
 * caller would then have to honour.  The batch is bounded and the walk
 * restarts, in the same shape as the scheduler's own channel wake -- an entry
 * whose task is claimed is unlinked in the SAME pass, so a waiter can never be
 * left claimed but not enqueued.
 *
 * A waiter that loses the claim (it timed out, or another waker got there
 * first) is skipped and left on the queue for its owner to remove.
 */
static inline int wq_wake_all(struct wait_queue_head *h)
{
	int total = 0;

	for (;;) {
		task_t *batch[16];
		int n = 0;
		uint64_t flags;
		struct wait_queue_entry *e, *next;

		spin_lock_irqsave(&h->lock, &flags);
		for (e = h->first; e && n < 16; e = next) {
			next = e->next;
			if (!e->task)
				continue;
			if (!sched_claim_wake(e->task, TASK_BLOCKED))
				continue;
			/* Claimed, so it must also leave the queue and be
			 * enqueued below -- see the note above. */
			if (e->pprev) {
				*e->pprev = e->next;
				if (e->next)
					e->next->pprev = e->pprev;
				e->next = NULL;
				e->pprev = NULL;
			}
			e->task->wait_channel = NULL;
			batch[n++] = e->task;
		}
		spin_unlock_irqrestore(&h->lock, flags);

		for (int i = 0; i < n; i++)
			sched_enqueue_ready(batch[i]);
		total += n;

		if (n < 16)
			break;
	}
	return total;
}

#endif /* _KERNEL_WAITQ_H_ */
