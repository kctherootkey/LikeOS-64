// LikeOS-64 Futex (Fast Userspace Mutex) Subsystem
// ============================================================================
// Hash-bucket implementation for scalable futex operations.
//
// Futexes are the foundation for userspace synchronization primitives like
// pthread_mutex, condition variables, semaphores, and barriers.
//
// Architecture:
//   - 256-bucket hash table to reduce lock contention
//   - Per-bucket spinlock for waiters
//   - Physical address key for shared futexes, virtual+PML4 for private
//   - Robust futex support for pthread_mutex_t with PTHREAD_MUTEX_ROBUST
// ============================================================================

#include <kernel/ke/futex.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/types.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/percpu.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// FUTEX TRACE RING BUFFER (diagnostic)
// Uncomment the following define to enable futex tracing in debug dumps.
// ============================================================================

// #define FUTEX_TRACE_DEBUG

#ifdef FUTEX_TRACE_DEBUG

#define FTRACE_ENTRIES 128

enum ftrace_op {
	FT_WAIT_ENTER = 0, // futex_wait called
	FT_WAIT_EAGAIN, // futex_wait early EAGAIN (value mismatch before lock)
	FT_WAIT_EAGAIN2, // futex_wait EAGAIN under lock (double-check)
	FT_WAIT_BLOCK, // futex_wait: inserted waiter, going BLOCKED
	FT_WAIT_WOKE, // futex_wait: returned from sched_schedule
	FT_WAKE_ENTER, // futex_wake called
	FT_WAKE_FOUND, // futex_wake: found a matching waiter
	FT_WAKE_DONE, // futex_wake: finished, returning woken count
	FT_WAKE_TASK, // futex_wake: task exiting (clear_child_tid)
};

typedef struct {
	uint64_t tick;
	uint16_t tid; // caller's TID
	uint8_t cpu; // CPU
	uint8_t op; // ftrace_op
	uint64_t uaddr;
	uint32_t val; // expected_val / nr_wake / woken count
	uint32_t extra; // curval / target tid / bucket waiters / task state
	uint32_t key_lo; // low 32 bits of computed futex key
	uint16_t bucket; // bucket index
	uint16_t bkt_count; // entries in bucket at time of log
} ftrace_entry_t;

static ftrace_entry_t ftrace_ring[FTRACE_ENTRIES];
static volatile int ftrace_idx = 0;

static void ftrace_log(uint8_t op, uint64_t uaddr, uint32_t val, uint32_t extra)
{
	int idx = __atomic_fetch_add(&ftrace_idx, 1, __ATOMIC_RELAXED) %
		  FTRACE_ENTRIES;
	ftrace_entry_t *e = &ftrace_ring[idx];
	e->tick = timer_ticks();
	task_t *c = sched_current();
	e->tid = c ? (uint16_t)c->id : 0xFFFF;
	e->cpu = (uint8_t)this_cpu_id();
	e->op = op;
	e->uaddr = uaddr;
	e->val = val;
	e->extra = extra;
	e->key_lo = 0;
	e->bucket = 0;
	e->bkt_count = 0;
}

static void ftrace_log_key(uint8_t op, uint64_t uaddr, uint32_t val,
			   uint32_t extra, uint64_t key, uint32_t bucket_idx,
			   uint16_t bkt_count)
{
	int idx = __atomic_fetch_add(&ftrace_idx, 1, __ATOMIC_RELAXED) %
		  FTRACE_ENTRIES;
	ftrace_entry_t *e = &ftrace_ring[idx];
	e->tick = timer_ticks();
	task_t *c = sched_current();
	e->tid = c ? (uint16_t)c->id : 0xFFFF;
	e->cpu = (uint8_t)this_cpu_id();
	e->op = op;
	e->uaddr = uaddr;
	e->val = val;
	e->extra = extra;
	e->key_lo = (uint32_t)(key & 0xFFFFFFFF);
	e->bucket = (uint16_t)bucket_idx;
	e->bkt_count = bkt_count;
}

void futex_dump_trace(void)
{
	static const char *op_names[] = {
		"WAIT_ENTER", "WAIT_EAGN1", "WAIT_EAGN2",
		"WAIT_BLOCK", "WAIT_WOKE ", "WAKE_ENTER",
		"WAKE_FOUND", "WAKE_DONE ", "WAKE_TASK ",
	};
	int total = __atomic_load_n(&ftrace_idx, __ATOMIC_RELAXED);
	int start = (total > FTRACE_ENTRIES) ? (total - FTRACE_ENTRIES) : 0;
	int count = (total > FTRACE_ENTRIES) ? FTRACE_ENTRIES : total;

	kprintf("\n=== Futex Trace (last %d of %d) ===\n", count, total);
	kprintf("  Tick   TID CPU Op          Addr             Val    Extra   Key_lo   Bkt  N\n");
	for (int i = 0; i < count; i++) {
		int idx = (start + i) % FTRACE_ENTRIES;
		ftrace_entry_t *e = &ftrace_ring[idx];
		const char *name =
			(e->op <= FT_WAKE_TASK) ? op_names[e->op] : "???";
		kprintf("  %5lu %4u  %u  %s  %lx  %5u  %5u  %08x %3u %2u\n",
			(unsigned long)e->tick, e->tid, e->cpu, name,
			(unsigned long)e->uaddr, e->val, e->extra, e->key_lo,
			e->bucket, e->bkt_count);
	}
	kprintf("=================================\n");
}

#else /* !FUTEX_TRACE_DEBUG */

#define ftrace_log(op, uaddr, val, extra) ((void)0)
#define ftrace_log_key(op, uaddr, val, extra, key, bidx, bcnt) ((void)0)
void futex_dump_trace(void)
{
}

#endif /* FUTEX_TRACE_DEBUG */

// ============================================================================
// INTERNAL STRUCTURES
// ============================================================================

// Futex wait queue entry
typedef struct futex_waiter {
	uint64_t uaddr; // User address being waited on
	uint64_t
		key; // Hash key (physical address for shared, virtual for private)
	task_t *task; // Waiting task
	struct futex_waiter *next; // Next waiter in bucket
	uint32_t bitset; // For FUTEX_WAIT_BITSET
	volatile bool removed_by_wake; // Set by futex_wake under bucket lock
} futex_waiter_t;

// Hash bucket
typedef struct futex_bucket {
	spinlock_t lock;
	futex_waiter_t *head;
} futex_bucket_t;

#ifdef FUTEX_TRACE_DEBUG
// Count entries in a bucket list (must hold bucket lock)
static uint16_t bucket_count(futex_waiter_t *head)
{
	uint16_t n = 0;
	futex_waiter_t *w = head;
	while (w) {
		n++;
		w = w->next;
	}
	return n;
}
#endif

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Global futex hash table
static futex_bucket_t futex_hash[FUTEX_HASH_BUCKETS];
static bool futex_initialized = false;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Validate user pointer is in user space
static inline bool validate_user_ptr(uint64_t ptr, size_t len)
{
	if (ptr < 0x10000)
		return false; // Reject low addresses (NULL deref protection)
	if (ptr >= 0x7FFFFFFFFFFF)
		return false; // Beyond user space
	if (ptr + len < ptr)
		return false; // Overflow check
	return true;
}

// Hash function for futex addresses (MurmurHash3 finalizer)
static inline uint32_t futex_hash_fn(uint64_t key)
{
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccd;
	key ^= key >> 33;
	key *= 0xc4ceb9fe1a85ec53;
	key ^= key >> 33;
	return (uint32_t)(key % FUTEX_HASH_BUCKETS);
}

/*
 * Which process does a private futex belong to?
 *
 * The thread-group id, mixed into the user address.  Every thread that shares
 * an address space shares a tgid, which is exactly the scope a private futex
 * has, and a fork child gets its own.
 *
 * This used to be the address of the process's top-level page table.  That is
 * a stable identifier only for as long as the page is not handed to somebody
 * else, and page tables are recycled from a stack -- most-recently-freed
 * first -- so the next process to start is the one most likely to be given the
 * very same page, and with it the very same futex keys.  Any waiter left
 * behind by the process that died then matches wakes belonging to the new one.
 * Releasing address spaces promptly, rather than leaving them to a reaper,
 * turns that from unlikely into the common case.
 *
 * DO NOT use mm_get_physical_address() for private keys: it walks from the
 * current CR3 and can return inconsistent results (including 0) depending on
 * timing and which CPU the thread runs on.
 */
static uint64_t futex_private_key(uint64_t tgid, uint64_t uaddr)
{
	/* Multiplied rather than shifted so that two processes cannot collide
	 * merely by using the same address, whatever the id happens to be. */
	return (tgid * 0x9E3779B97F4A7C15ULL) ^ uaddr;
}

// Get the key for a futex address
// - Shared futexes: physical address (same across processes)
// - Private futexes: the owning process, combined with the virtual address
static uint64_t futex_get_key(uint64_t uaddr, bool shared)
{
	if (shared) {
		// For shared futexes, use physical address as key
		return mm_get_physical_address(uaddr);
	} else {
		task_t *cur = sched_current();

		return futex_private_key(cur ? (uint64_t)cur->tgid : 0, uaddr);
	}
}

// Variant of futex_get_key that uses a specific task instead of
// sched_current().  Needed when performing futex operations on behalf of
// a different task (e.g. sched_mark_task_exited called from a cross-CPU
// SIGKILL sender where sched_current() is the sender, not the victim).
static uint64_t futex_get_key_for_task(uint64_t uaddr, bool shared,
				       task_t *task)
{
	if (shared) {
		return mm_get_physical_address(uaddr);
	} else {
		return futex_private_key(task ? (uint64_t)task->tgid : 0,
					 uaddr);
	}
}

// ============================================================================
// FUTEX OPERATIONS
// ============================================================================

void futex_init(void)
{
	if (futex_initialized)
		return;

	for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) {
		spinlock_init(&futex_hash[i].lock, "futex_bucket");
		futex_hash[i].head = NULL;
	}

	futex_initialized = true;
}

static int futex_wait_common(uint64_t uaddr, uint32_t expected_val,
			     uint64_t timeout_ns, uint32_t bitset)
{
	might_sleep();
	if (!validate_user_ptr(uaddr, sizeof(uint32_t))) {
		return -EFAULT;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	// Check current value
	smap_disable();
	uint32_t curval = *(volatile uint32_t *)uaddr;
	smap_enable();

	ftrace_log(FT_WAIT_ENTER, uaddr, expected_val, curval);

	if (curval != expected_val) {
		ftrace_log(FT_WAIT_EAGAIN, uaddr, expected_val, curval);
		return -EAGAIN; // Value changed, don't wait
	}

	// Compute hash key and bucket
	uint64_t key = futex_get_key(uaddr, false); // Private futex
	uint32_t bucket_idx = futex_hash_fn(key);
	BUG_ON(bucket_idx >= FUTEX_HASH_BUCKETS);
	futex_bucket_t *bucket = &futex_hash[bucket_idx];

	// Allocate waiter
	futex_waiter_t *waiter =
		(futex_waiter_t *)kalloc(sizeof(futex_waiter_t));
	if (!waiter)
		return -ENOMEM;

	waiter->uaddr = uaddr;
	waiter->key = key;
	waiter->task = cur;
	waiter->bitset = bitset; /* FUTEX_BITSET_MATCH_ANY for plain waits */
	waiter->removed_by_wake = false;

	// Add to wait queue
	uint64_t flags;
	spin_lock_irqsave(&bucket->lock, &flags);

	// Re-check value under lock (double-check pattern)
	smap_disable();
	curval = *(volatile uint32_t *)uaddr;
	smap_enable();

	if (curval != expected_val) {
		ftrace_log(FT_WAIT_EAGAIN2, uaddr, expected_val, curval);
		spin_unlock_irqrestore(&bucket->lock, flags);
		kfree(waiter);
		return -EAGAIN;
	}

	// Insert at head of bucket list
	waiter->next = bucket->head;
	bucket->head = waiter;

	/* Say WHERE this task sleeps before saying THAT it is asleep.
	 *
	 * TASK_BLOCKED is the thing that makes a task claimable: every waker,
	 * on every processor, CAS's BLOCKED -> READY and enqueues, and each
	 * finds its target through one of the two fields set below --
	 * sched_wake_channel() matches `wait_channel', the timer's
	 * expired-sleeper scan matches `wakeup_tick'.  Neither takes this
	 * bucket's lock, so holding it here holds off neither.
	 *
	 * Storing the state first published this task as claimable while both
	 * fields still described its PREVIOUS sleep.  The wide window is the
	 * timer's: a thread just out of a timed wait carries a `wakeup_tick'
	 * that is already in the past, so the next tick on any processor
	 * claimed it -- before the `else' branch below, written for precisely
	 * that hazard, had run to clear it.  The task was then enqueued while
	 * it was still executing here, which is what the WARN reported, and it
	 * came straight back out of sched_schedule() having never slept.  For
	 * a process with a dozen threads on condition variables that is a
	 * spurious wakeup per wait, absorbed by the caller's retry loop and
	 * paid for in syscalls.
	 *
	 * Identity first and state last is the order every other parking site
	 * in this kernel uses -- sched_bsp_park(), and the shape quoted in the
	 * note above sched_request_stop(). */
	cur->wait_channel = (void *)uaddr;

	// Set wakeup time if timeout specified
	uint64_t deadline_tick = 0;
	if (timeout_ns > 0) {
		/* Against the MEASURED tick rate, not an assumed 10ms one: the
		 * boot calibration routinely settles near 200Hz on a virtual
		 * machine, and dividing by a hardcoded 10ms then expired every
		 * timeout in half the time asked for.  The +1 covers the tick
		 * already in progress, which is otherwise credited in full. */
		uint64_t ticks = timer_ns_to_ticks(timeout_ns) + 1;
		WARN_ON(ticks ==
			0); /* futex timeout ticks is zero - underflow in timeout conversion */
		deadline_tick = timer_ticks() + ticks;
		cur->wakeup_tick = deadline_tick;
	} else {
		/* No timeout: make sure a value left over from an earlier
		 * timed sleep cannot wake this task straight back up. */
		cur->wakeup_tick = 0;
	}

	/* Checked while the state is still RUNNING, which is what makes it an
	 * invariant: rq_enqueue_locked() refuses a RUNNING task, so a queued
	 * one here means the flag and the queues disagree.  After the store
	 * below the same test would fire on a legitimate concurrent wake --
	 * sched_schedule() documents that race and handles it -- which is what
	 * it was doing where it used to stand. */
	WARN_ON(cur->on_rq);

	// Block the task
	cur->state = TASK_BLOCKED;

	ftrace_log_key(FT_WAIT_BLOCK, uaddr, expected_val, (uint32_t)cur->id,
		       key, bucket_idx, bucket_count(bucket->head));

	spin_unlock_irqrestore(&bucket->lock, flags);

	// Schedule away - will return when woken or timed out
	sched_schedule();

	// We've been woken up - but we might have been woken by:
	// 1. futex_wake() - waiter->removed_by_wake is set, waiter unlinked from bucket
	// 2. Timeout - waiter is still in the bucket list
	// 3. Signal - waiter is still in the bucket list
	//
	// IMPORTANT: We always free the waiter ourselves.  futex_wake only
	// unlinks and marks it; it never kfrees, to avoid an ABA race where
	// the freed memory is recycled into a *new* waiter at the same address
	// and we then accidentally remove the new entry from the bucket.

	ftrace_log(FT_WAIT_WOKE, uaddr, (uint32_t)waiter->removed_by_wake,
		   (uint32_t)cur->id);

	// Re-acquire the lock and check if waiter was removed by futex_wake
	spin_lock_irqsave(&bucket->lock, &flags);

	if (!waiter->removed_by_wake) {
		// Still in list - we were woken by timeout/signal, not futex_wake
		// Remove ourselves from the bucket
		futex_waiter_t **pp = &bucket->head;
		while (*pp) {
			if (*pp == waiter) {
				*pp = waiter->next;
				break;
			}
			pp = &(*pp)->next;
		}
	}

	bool was_woken = waiter->removed_by_wake;

	spin_unlock_irqrestore(&bucket->lock, flags);

	kfree(waiter);

	if (was_woken)
		return 0;

	/*
	 * Not woken by futex_wake, so this is a timeout, a signal, or a
	 * spurious wake -- and they are three different answers.  Reporting
	 * them all as ETIMEDOUT (which is what this used to do) is wrong in
	 * both directions: a caller waiting without any timeout at all was
	 * told its wait "timed out", and a caller with a timeout was told the
	 * deadline had passed the moment any signal arrived, however long was
	 * actually left.  sem_wait() turning a stray SIGALRM into a failure,
	 * and sem_timedwait() returning ETIMEDOUT after no measurable wait,
	 * are both that bug.
	 *
	 * The deadline is the authority on whether time ran out, so it is what
	 * decides: if it exists and has passed, this is ETIMEDOUT.  Everything
	 * else -- a signal, a spurious wake, and a wait that had no deadline to
	 * miss in the first place -- is EINTR, which tells the caller to
	 * re-check its condition and go round again.  Signals and spurious
	 * wakes are not distinguished from each other because the caller must
	 * do the same thing for both, and every futex user here already does.
	 */
	if (deadline_tick != 0 && timer_ticks() >= deadline_tick)
		return -ETIMEDOUT;

	return -EINTR;
}

static int futex_wake_common(uint64_t uaddr, int nr_wake, uint32_t bitset)
{
	BUG_ON(!futex_initialized);
	if (nr_wake <= 0)
		return 0;

	uint64_t key = futex_get_key(uaddr, false);
	uint32_t bucket_idx = futex_hash_fn(key);
	futex_bucket_t *bucket = &futex_hash[bucket_idx];

	int woken = 0;

// Deferred wakeup: collect tasks while holding lock, wake after releasing
// This allows using irqsave (needed for scheduler consistency) while
// keeping the critical section short enough to avoid TLB shootdown timeouts
#define MAX_DEFERRED_WAKE 32
	uint64_t flags;

	/* Batched, and it LOOPS until the requested number really have been
	 * woken.
	 *
	 * The batch exists so the bucket lock is not held across the enqueues;
	 * that part is right and unchanged.  What was wrong was treating the
	 * batch size as the end of the job: the walk simply stopped once 32
	 * tasks had been collected and returned, leaving every waiter past the
	 * 32nd linked in the bucket and still TASK_BLOCKED.
	 *
	 * pthread_cond_broadcast() asks for 0x7FFFFFFF -- wake everyone -- and
	 * then increments the sequence, so the event those waiters were
	 * sleeping for is consumed.  No later wake is coming for them.  Above
	 * 32 threads on one condition variable, a broadcast woke 32 and put
	 * the rest to sleep for the life of the process.
	 *
	 * That is not a corner case for this workload: a browser tab under
	 * load runs seventy-odd threads, nearly all of them parked in
	 * pthread_cond_wait on a handful of worker-pool condition variables.
	 * The observed end state was every thread in futex_wait or poll, four
	 * of them on mutexes whose owners had been broadcast away, and not one
	 * thread runnable.
	 *
	 * So: fill a batch, drop the lock, enqueue, and if the batch filled up
	 * go back for more.  Unlinking as it goes makes restarting from the
	 * head correct, and the walk only repeats while a full batch says
	 * there may be more. */
	for (;;) {
		task_t *wake_list[MAX_DEFERRED_WAKE];
		int wake_count = 0;

		spin_lock_irqsave(&bucket->lock, &flags);

		ftrace_log_key(FT_WAKE_ENTER, uaddr, (uint32_t)nr_wake, 0, key,
			       bucket_idx, bucket_count(bucket->head));

		futex_waiter_t **pp = &bucket->head;
		while (*pp && woken < nr_wake &&
		       wake_count < MAX_DEFERRED_WAKE) {
			futex_waiter_t *w = *pp;

			if (w->key == key && (w->bitset & bitset)) {
				// Remove from list
				*pp = w->next;

				/* Mark as removed so futex_wait cleanup knows
				 * not to scan the list.  The waiter memory is
				 * freed by the waiting thread, not us, to
				 * prevent ABA races with kalloc recycling the
				 * same address. */
				w->removed_by_wake = true;

				/* Collect task for deferred wakeup.  The
				 * BLOCKED->READY claim must be atomic: the
				 * sleep-timeout and signal wakers run under
				 * g_task_list_lock (not this bucket lock) and
				 * can claim the same task concurrently -- a
				 * plain check-then-set let both sides enqueue
				 * it. */
				task_t *task = w->task;

				ftrace_log_key(FT_WAKE_FOUND, uaddr,
					       (uint32_t)task->id,
					       (uint32_t)task->state, w->key,
					       bucket_idx, 0);
				if (sched_claim_wake(task, TASK_BLOCKED)) {
					task->wait_channel = NULL;
					task->wakeup_tick = 0;
					wake_list[wake_count] = task;
					wake_count++;
					woken++;
				}
				/* A waiter that is NOT blocked (already woken
				 * by a signal, or a zombie) is still unlinked
				 * but does not count against nr_wake: counting
				 * it would consume a wake slot that a real
				 * sleeper needed. */
			} else {
				pp = &w->next;
			}
		}

		int filled = (wake_count == MAX_DEFERRED_WAKE);

		spin_unlock_irqrestore(&bucket->lock, flags);

		ftrace_log(FT_WAKE_DONE, uaddr, (uint32_t)woken,
			   (uint32_t)wake_count);

		// Now enqueue tasks outside the lock
		for (int i = 0; i < wake_count; i++)
			sched_enqueue_ready(wake_list[i]);

		/* Only go round again if the batch filled: that is the one
		 * case where waiters this call was asked to wake may still be
		 * linked.  Any other exit means the walk reached the end of
		 * the bucket or satisfied nr_wake. */
		if (!filled || woken >= nr_wake)
			break;
	}

	return woken;
}

int futex_wait(uint64_t uaddr, uint32_t expected_val, uint64_t timeout_ns)
{
	return futex_wait_common(uaddr, expected_val, timeout_ns,
				 FUTEX_BITSET_MATCH_ANY);
}

int futex_wait_bitset(uint64_t uaddr, uint32_t expected_val,
		      uint64_t timeout_ns, uint32_t bitset)
{
	if (bitset == 0)
		return -EINVAL;
	return futex_wait_common(uaddr, expected_val, timeout_ns, bitset);
}

int futex_wake(uint64_t uaddr, int nr_wake)
{
	return futex_wake_common(uaddr, nr_wake, FUTEX_BITSET_MATCH_ANY);
}

/* Wake only the waiters whose bitset intersects `bitset'.  A plain
 * FUTEX_WAIT sleeps with every bit set and so matches any wake; a
 * FUTEX_WAIT_BITSET sleeper is woken only by a wake naming one of its
 * bits -- how a condition variable wakes one class of waiter and not
 * another on the same word. */
int futex_wake_bitset(uint64_t uaddr, int nr_wake, uint32_t bitset)
{
	if (bitset == 0)
		return -EINVAL;
	return futex_wake_common(uaddr, nr_wake, bitset);
}

// Like futex_wake but uses a specific task's PML4 for key computation.
// This is needed when waking futex waiters on behalf of a different task
// (e.g. during sched_mark_task_exited called cross-CPU via SIGKILL).
int futex_wake_for_task(uint64_t uaddr, int nr_wake, task_t *on_behalf_of)
{
	if (nr_wake <= 0)
		return 0;

	ftrace_log(FT_WAKE_TASK, uaddr, (uint32_t)nr_wake,
		   on_behalf_of ? (uint32_t)on_behalf_of->id : 0xFFFF);

	uint64_t key = futex_get_key_for_task(uaddr, false, on_behalf_of);
	uint32_t bucket_idx = futex_hash_fn(key);
	futex_bucket_t *bucket = &futex_hash[bucket_idx];

	int woken = 0;

#define MAX_DEFERRED_WAKE_TASK 32
	task_t *wake_list[MAX_DEFERRED_WAKE_TASK];
	int wake_count = 0;

	uint64_t flags;
	spin_lock_irqsave(&bucket->lock, &flags);

	futex_waiter_t **pp = &bucket->head;
	while (*pp && woken < nr_wake && wake_count < MAX_DEFERRED_WAKE_TASK) {
		futex_waiter_t *w = *pp;

		if (w->key == key) {
			*pp = w->next;
			w->removed_by_wake = true;

			task_t *task = w->task;
			/* Atomic claim — see the comment in futex_wake. */
			if (sched_claim_wake(task, TASK_BLOCKED)) {
				task->wait_channel = NULL;
				task->wakeup_tick = 0;
				wake_list[wake_count] = task;
				wake_count++;
				woken++;
			}
		} else {
			pp = &w->next;
		}
	}

	spin_unlock_irqrestore(&bucket->lock, flags);

	for (int i = 0; i < wake_count; i++) {
		sched_enqueue_ready(wake_list[i]);
	}

	return woken;
}

int futex_requeue(uint64_t uaddr, uint64_t uaddr2, int nr_wake, int nr_requeue)
{
	if (nr_wake < 0 || nr_requeue < 0)
		return -EINVAL;

	uint64_t key1 = futex_get_key(uaddr, false);
	uint64_t key2 = futex_get_key(uaddr2, false);
	uint32_t bucket_idx1 = futex_hash_fn(key1);
	uint32_t bucket_idx2 = futex_hash_fn(key2);

	futex_bucket_t *bucket1 = &futex_hash[bucket_idx1];
	futex_bucket_t *bucket2 = &futex_hash[bucket_idx2];

// Deferred wakeup: collect tasks while holding lock, wake after releasing
#define MAX_DEFERRED_WAKE_REQUEUE 32
	task_t *wake_list[MAX_DEFERRED_WAKE_REQUEUE];
	int wake_count = 0;

	int woken = 0;
	int requeued = 0;

	// Lock ordering: always lock lower bucket index first to prevent deadlock
	uint64_t flags;
	if (bucket_idx1 < bucket_idx2) {
		spin_lock_irqsave(&bucket1->lock, &flags);
		spin_lock(&bucket2->lock);
	} else if (bucket_idx1 > bucket_idx2) {
		spin_lock_irqsave(&bucket2->lock, &flags);
		spin_lock(&bucket1->lock);
	} else {
		// Same bucket - only lock once
		spin_lock_irqsave(&bucket1->lock, &flags);
	}

	futex_waiter_t **pp = &bucket1->head;
	while (*pp) {
		futex_waiter_t *w = *pp;

		if (w->key == key1) {
			// Remove from bucket1
			*pp = w->next;

			if (woken < nr_wake &&
			    wake_count < MAX_DEFERRED_WAKE_REQUEUE) {
				// Mark as removed (waiter freed by the waiting thread)
				w->removed_by_wake = true;

				// Collect for deferred wakeup (only count if actually
				// blocked).  Atomic claim — see futex_wake.
				task_t *task = w->task;
				if (sched_claim_wake(task, TASK_BLOCKED)) {
					task->wait_channel = NULL;
					task->wakeup_tick = 0;
					wake_list[wake_count] = task;
					wake_count++;
					woken++;
				}
				// Non-blocked stale entries: removed but not counted
			} else if (requeued < nr_requeue) {
				// Requeue to bucket2
				w->uaddr = uaddr2;
				w->key = key2;
				w->task->wait_channel = (void *)uaddr2;
				w->next = bucket2->head;
				bucket2->head = w;
				requeued++;
			} else {
				// Already hit limits, re-insert at head
				w->next = *pp;
				*pp = w;
				pp = &w->next;
			}
		} else {
			pp = &w->next;
		}
	}

	// Unlock in reverse order
	if (bucket_idx1 < bucket_idx2) {
		spin_unlock(&bucket2->lock);
		spin_unlock_irqrestore(&bucket1->lock, flags);
	} else if (bucket_idx1 > bucket_idx2) {
		spin_unlock(&bucket1->lock);
		spin_unlock_irqrestore(&bucket2->lock, flags);
	} else {
		spin_unlock_irqrestore(&bucket1->lock, flags);
	}

	// Deferred wakeups - outside the lock
	for (int i = 0; i < wake_count; i++) {
		sched_enqueue_ready(wake_list[i]);
	}

	return woken + requeued;
}

// ============================================================================
// TASK EXIT CLEANUP
// ============================================================================

// Remove all futex waiter entries belonging to the given task.
// Called from sched_mark_task_exited() for tasks that are killed (e.g. via
// SIGKILL) while BLOCKED in futex_wait.  Without this, the waiter entry
// stays in the hash bucket forever.  On a later futex_wake, it silently
// consumes a wake slot (the task is no longer BLOCKED so the wake is
// discarded), which steals the wake from a legitimate waiter and causes
// a deadlock.
void futex_cleanup_task(task_t *task)
{
	if (!task || !futex_initialized)
		return;

	for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) {
		futex_bucket_t *bucket = &futex_hash[i];
		uint64_t flags;
		spin_lock_irqsave(&bucket->lock, &flags);

		futex_waiter_t **pp = &bucket->head;
		while (*pp) {
			futex_waiter_t *w = *pp;
			if (w->task == task) {
				*pp = w->next;
				kfree(w);
			} else {
				pp = &(*pp)->next;
			}
		}

		spin_unlock_irqrestore(&bucket->lock, flags);
	}
}

// ============================================================================
// ROBUST FUTEX HANDLING
// ============================================================================

void exit_robust_list(task_t *task)
{
	if (!task || !task->robust_list)
		return;

	// The robust_list_head structure format (the conventional ABI):
	// struct robust_list_head {
	//     struct robust_list *list;           // Pointer to first entry
	//     long futex_offset;                  // Offset of futex word in entry
	//     struct robust_list *list_op_pending; // Currently being acquired
	// };
	//
	// Each entry in the list contains:
	//   - Pointer to next entry
	//   - Futex word at futex_offset from entry start
	//
	// On thread death, we walk the list and for each futex we own:
	//   1. Set FUTEX_OWNER_DIED bit (bit 30)
	//   2. Wake one waiter

	// Verify the head itself is present in the current page tables before
	// touching any of its fields.  The process may have been killed while
	// the heap/library pages were demand-paged and some pages may not be
	// backed yet (or already torn down).
	if (!mm_user_addr_mapped((uint64_t)task->robust_list,
				 sizeof(struct robust_list_head))) {
		task->robust_list = NULL;
		return;
	}

	smap_disable();
	struct robust_list_head *head = task->robust_list;

	// Limit iterations to prevent infinite loops from corrupted lists
	int count = 0;
	const int max_entries = 1024;

	// Process pending futex first (thread died while acquiring)
	if (head->list_op_pending &&
	    head->list_op_pending != (struct robust_list *)head) {
		uint64_t futex_addr =
			(uint64_t)head->list_op_pending + head->futex_offset;
		if (validate_user_ptr(futex_addr, sizeof(uint32_t)) &&
		    mm_user_addr_mapped(futex_addr, sizeof(uint32_t))) {
			uint32_t *futex = (uint32_t *)futex_addr;
			// Only mark if we're the owner
			if ((*futex & FUTEX_TID_MASK) == (uint32_t)task->id) {
				*futex |= FUTEX_OWNER_DIED;
				smap_enable();
				futex_wake_for_task(futex_addr, 1, task);
				smap_disable();
			}
		}
	}

	// Walk the circular list
	struct robust_list *entry = (struct robust_list *)head->list;
	while (entry && entry != (struct robust_list *)head &&
	       count < max_entries) {
		// The futex is at offset futex_offset from the list entry
		uint64_t futex_addr = (uint64_t)entry + head->futex_offset;

		if (validate_user_ptr(futex_addr, sizeof(uint32_t)) &&
		    mm_user_addr_mapped(futex_addr, sizeof(uint32_t))) {
			uint32_t *futex = (uint32_t *)futex_addr;
			// Check if we own this futex (TID in low 30 bits)
			if ((*futex & FUTEX_TID_MASK) == (uint32_t)task->id) {
				*futex |= FUTEX_OWNER_DIED;
				smap_enable();
				futex_wake_for_task(futex_addr, 1, task);
				smap_disable();
			}
		}

		// Move to next entry
		if (!validate_user_ptr((uint64_t)entry,
				       sizeof(struct robust_list)) ||
		    !mm_user_addr_mapped((uint64_t)entry,
					 sizeof(struct robust_list))) {
			break;
		}
		entry = entry->next;
		count++;
	}

	smap_enable();
}
