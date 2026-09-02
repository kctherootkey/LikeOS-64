// LikeOS-64 — address-space read/write semaphore
//
// See <kernel/mm/rwsem.h> for what this lock covers and who takes it.
//
// The blocking discipline is the one the sleeping filesystem semaphores use:
// record the wait channel and the BLOCKED state under the semaphore's own
// spinlock, drop the spinlock, then schedule.  The wake side
// (sched_wake_channel) claims BLOCKED->READY atomically and sched_schedule()
// copes with being woken before it ever ran, so there is no lost-wakeup window
// between publishing the state and giving up the CPU.
//
// Fairness: writers register in `w_wait` and new readers stand behind them, so
// a stream of faults cannot starve an munmap for ever.  The exception is a task
// that ALREADY holds a shared reference (task->mm_rdepth > 0): its nested
// acquisition must jump the queue, because the writer it would queue behind is
// itself waiting for that first reference to drain.  Recursion under one's own
// EXCLUSIVE hold is granted as a depth increment for the same reason -- mmap()
// holds the lock for writing and then copies its arguments from user memory,
// which can fault straight back into a reader.

#include <kernel/mm/rwsem.h>
#include <kernel/uapi/bug.h>

void mm_rwsem_init(mm_rwsem_t *sem, const char *name)
{
	BUG_ON(sem == NULL);
	sem->readers = 0;
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	sem->w_wait = 0;
	spinlock_init(&sem->lock, name);
}

/* Park the caller on `sem`'s wait channel and give up the CPU.  Called with
 * the semaphore spinlock held and `flags` holding the saved IRQ state; returns
 * with neither held, so every caller must re-loop and re-test. */
static void mm_rwsem_park(mm_rwsem_t *sem, uint64_t *flags)
{
	task_t *cur = sched_current();

	if (cur) {
		cur->wait_channel = (void *)sem;
		cur->state = TASK_BLOCKED;
	}
	spin_unlock_irqrestore(&sem->lock, *flags);
	sched_schedule();
}

void mm_read_lock(mm_rwsem_t *sem)
{
	task_t *cur;
	uint64_t my_id;

	BUG_ON(sem == NULL);
	might_sleep();
	cur = sched_current();
	my_id = cur ? cur->id : 0;

	for (;;) {
		uint64_t flags;

		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == my_id && cur) {
			/* Already ours exclusively — count it as recursion so
			 * the matching unlock does not release the write hold. */
			sem->wdepth++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		{
			int defer = sem->w_wait && !(cur && cur->mm_rdepth);

			if (!sem->writer && !defer) {
				sem->readers++;
				if (cur)
					cur->mm_rdepth++;
				spin_unlock_irqrestore(&sem->lock, flags);
				return;
			}
		}
		mm_rwsem_park(sem, &flags);
	}
}

/* One attempt at the shared hold, never parking.
 *
 * For callers that cannot park -- a page fault taken with interrupts disabled
 * is the case this exists for.  It answers the same question mm_read_lock()
 * does and grants the same three outcomes, but where that one would sleep this
 * one says no, so the caller can refuse whatever it was about to do instead of
 * doing it without the lock.
 *
 * Note there is no might_sleep(): not sleeping is the entire point. */
bool mm_read_trylock(mm_rwsem_t *sem)
{
	task_t *cur;
	uint64_t my_id, flags;

	BUG_ON(sem == NULL);
	cur = sched_current();
	my_id = cur ? cur->id : 0;

	spin_lock_irqsave(&sem->lock, &flags);
	if (sem->writer && sem->owner == my_id && cur) {
		/* Already ours exclusively — recursion, as in mm_read_lock(). */
		sem->wdepth++;
		spin_unlock_irqrestore(&sem->lock, flags);
		return true;
	}
	{
		int defer = sem->w_wait && !(cur && cur->mm_rdepth);

		if (!sem->writer && !defer) {
			sem->readers++;
			if (cur)
				cur->mm_rdepth++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return true;
		}
	}
	spin_unlock_irqrestore(&sem->lock, flags);
	return false;
}

void mm_read_unlock(mm_rwsem_t *sem)
{
	task_t *cur;
	uint64_t my_id, flags;
	int wake;

	BUG_ON(sem == NULL);
	cur = sched_current();
	my_id = cur ? cur->id : 0;

	spin_lock_irqsave(&sem->lock, &flags);
	if (sem->writer && sem->owner == my_id && cur) {
		/* Matching a nested-under-exclusive acquisition. */
		WARN_ON(sem->wdepth <= 1);
		sem->wdepth--;
		spin_unlock_irqrestore(&sem->lock, flags);
		return;
	}
	WARN_ON(sem->readers <= 0);
	sem->readers--;
	if (cur && cur->mm_rdepth > 0)
		cur->mm_rdepth--;
	wake = (sem->readers == 0);
	spin_unlock_irqrestore(&sem->lock, flags);
	if (wake)
		sched_wake_channel((void *)sem);
}

void mm_write_lock(mm_rwsem_t *sem)
{
	task_t *cur;
	uint64_t my_id;
	int queued = 0;

	BUG_ON(sem == NULL);
	might_sleep();
	cur = sched_current();
	my_id = cur ? cur->id : 0;

	for (;;) {
		uint64_t flags;

		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == my_id) {
			sem->wdepth++; /* reentrant exclusive */
			if (queued)
				sem->w_wait--;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		if (!sem->writer && sem->readers == 0) {
			sem->writer = 1;
			sem->owner = my_id;
			sem->wdepth = 1;
			if (queued)
				sem->w_wait--;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		if (!queued) {
			sem->w_wait++;
			queued = 1;
		}
		mm_rwsem_park(sem, &flags);
	}
}

void mm_write_unlock(mm_rwsem_t *sem)
{
	uint64_t flags;

	BUG_ON(sem == NULL);
	spin_lock_irqsave(&sem->lock, &flags);
	WARN_ON(!sem->writer || sem->wdepth <= 0);
	if (sem->wdepth > 1) {
		sem->wdepth--;
		spin_unlock_irqrestore(&sem->lock, flags);
		return;
	}
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	spin_unlock_irqrestore(&sem->lock, flags);
	sched_wake_channel((void *)sem);
}

bool mm_rwsem_is_locked(const mm_rwsem_t *sem)
{
	if (!sem)
		return false;
	return sem->writer != 0 || sem->readers > 0;
}

bool mm_rwsem_is_write_locked(const mm_rwsem_t *sem)
{
	if (!sem)
		return false;
	return sem->writer != 0;
}
