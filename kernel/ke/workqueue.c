// LikeOS kernel - generic deferred work (work queues)
//
// A work queue is a list of pending items and a small fixed set of worker
// threads that take items off the list and run their functions.  The
// interface and its guarantees are described in workqueue.h; this file
// is about how they are kept.
//
// One lock.  Every item state change, every queue list and every worker's
// "what am I running" record is protected by g_wq_lock, a spinlock taken
// with interrupts off (queueing happens from interrupt handlers and timer
// callbacks).  Work queues here serve slow device bookkeeping (a touchpad
// reset, a GPIO event, a deferred power change), so one short critical
// section per state change costs nothing measurable, and a single lock
// leaves no transitional state for a canceller to race with: an item is
// idle, on a list (QUEUED), on its timer (TIMER) or held by a cancel
// (CANCELING), and PENDING is set in all but the first.
//
// Running items are found through the workers, never through the item:
// each worker records the item it runs (current_work, a pointer compared
// and never dereferenced once the function returned, so a function may
// free its own item) and the item's claim number at the time
// (current_claim; an item counts its runs in nr_claimed).  Busy workers
// are on g_busy.  This gives:
//
//   * non-reentrancy: a worker never claims an item that another worker
//     is running; it leaves it on the list and marks the running worker,
//     which kicks every queue when it is done so the item is picked up;
//   * cancel_work_sync: take the item off its list/timer, set PENDING and
//     CANCELING (queue_work then refuses it), wait until no worker runs
//     it, clear both;
//   * flush_work: the instance to wait for is the queued one (claim
//     nr_claimed + 1) or the running one (nr_claimed); done when that
//     claim is neither queued nor running any more -- a later requeue is
//     a later claim and is not waited for.
//
// Delayed items: queue_delayed_work arms the item's hrtimer under the
// lock and marks it TIMER; the timer callback queues it.  The hrtimer
// layer unlinks a timer before calling its callback without any lock, so
// a cancel can find the timer already fired but its callback not yet
// through the lock.  hrtimer_cancel() then returns 0 although TIMER is
// set: exactly one callback for that arming is on its way, and it is
// counted in stale_fires.  A callback that finds stale_fires non-zero
// consumes one and does nothing else.  cancel_delayed_work_sync waits for
// stale_fires to drop to zero, after which no callback can touch the
// item, and only then returns.
//
// Waiting: flushers and cancellers sleep on g_wq_done, which workers and
// timer callbacks wake after a change when g_wq_waiters says somebody
// waits; the count is raised under the lock before the waiter first looks
// at its condition, so a change is never missed.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/workqueue.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define WQ_WORKER_STACK (16u * 1024u)
#define WQ_DEFAULT_ACTIVE 4
/* How long destroy_workqueue waits for its retired threads to be gone
 * before it leaves their stacks behind. */
#define WQ_REAP_WAIT_MS 2000u

struct wq_worker {
	struct workqueue_struct *wq;
	task_t *task;
	uint32_t task_id;
	void *stack;
	struct list_head busy_node; /* on g_busy while running an item */
	struct work_struct *current_work; /* compared, never dereferenced */
	work_func_t current_func;
	uint32_t current_claim;
	uint64_t current_qseq; /* queue sequence of the running item */
	int id;
	int kick_all; /* an item was left behind because I ran it */
	int exited;
};

struct workqueue_struct {
	char name[24];
	unsigned int flags;
	int nworkers;
	struct list_head worklist; /* QUEUED items, FIFO */
	struct wait_queue_head more_work; /* idle workers */
	struct wq_worker *workers[WQ_MAX_ACTIVE];
	int destroying;
	int nexited;
	struct list_head node; /* on g_wqs */
	uint64_t seq; /* insertions so far (flush_workqueue) */
	uint64_t nr_executed;
	/* Delayed items whose timer is armed to queue them here: the queue
	 * must outlive them (destroy_workqueue checks). */
	int nr_delayed;
};

static spinlock_t g_wq_lock = SPINLOCK_INIT("workqueue");
static struct list_head g_busy = LIST_HEAD_INIT(g_busy);
static struct list_head g_wqs = LIST_HEAD_INIT(g_wqs);
static struct wait_queue_head g_wq_done = WAIT_QUEUE_HEAD_INIT("wq_done");
static int g_wq_waiters; /* g_wq_lock */

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_freezable_wq;

/* ==========================================================================
 * Item state helpers (g_wq_lock held)
 * ========================================================================== */

static void work_check_init(const struct work_struct *work)
{
	WARN(work->magic != WORK_INIT_MAGIC,
	     "workqueue: item %p used before INIT_WORK", (const void *)work);
}

/* The busy worker running `work', or NULL. */
static struct wq_worker *work_running_locked(const struct work_struct *work)
{
	struct list_head *p;

	lockdep_assert_held(&g_wq_lock);
	for (p = g_busy.next; p != &g_busy; p = p->next) {
		struct wq_worker *w = container_of(p, struct wq_worker,
						   busy_node);

		if (w->current_work == work)
			return w;
	}
	return NULL;
}

static int work_running_claim_locked(const struct work_struct *work,
				     uint32_t claim)
{
	struct list_head *p;

	lockdep_assert_held(&g_wq_lock);
	for (p = g_busy.next; p != &g_busy; p = p->next) {
		struct wq_worker *w = container_of(p, struct wq_worker,
						   busy_node);

		if (w->current_work == work && w->current_claim == claim)
			return 1;
	}
	return 0;
}

static void insert_work_locked(struct workqueue_struct *wq,
			       struct work_struct *work)
{
	lockdep_assert_held(&g_wq_lock);
	BUG_ON(wq == NULL);
	WARN_ON_ONCE(wq->destroying);
	WARN_ON(work->state & (WORK_STATE_QUEUED | WORK_STATE_TIMER));
	list_add_tail(&work->entry, &wq->worklist);
	work->state |= WORK_STATE_QUEUED | WORK_STATE_PENDING;
	work->wq = wq;
	work->qseq = ++wq->seq;
}

/* Take PENDING from wherever it is: 1 it was pending (now off its list or
 * timer, PENDING clear), 0 it was idle, -ENOENT a cancel holds it. */
static int grab_pending_locked(struct work_struct *work)
{
	lockdep_assert_held(&g_wq_lock);
	if (work->state & WORK_STATE_CANCELING)
		return -ENOENT;
	if (!(work->state & WORK_STATE_PENDING))
		return 0;
	if (work->state & WORK_STATE_TIMER) {
		struct delayed_work *dw = to_delayed_work(work);

		BUG_ON(!work->is_delayed);
		/* Not armed any more although TIMER is set: its callback
		 * is on the way and will find the lock after us. */
		if (!hrtimer_cancel(&dw->timer))
			dw->stale_fires++;
		work->state &= ~WORK_STATE_TIMER;
		BUG_ON(dw->wq->nr_delayed <= 0);
		dw->wq->nr_delayed--;
	}
	if (work->state & WORK_STATE_QUEUED) {
		list_del(&work->entry);
		INIT_LIST_HEAD(&work->entry);
		work->state &= ~WORK_STATE_QUEUED;
	}
	/* PENDING without QUEUED/TIMER/CANCELING does not exist. */
	work->state &= ~WORK_STATE_PENDING;
	return 1;
}

/* Arm or queue a delayed item whose PENDING the caller just set.  1 when
 * it went onto the list (the queue's workers must be woken). */
static int queue_delayed_locked(struct workqueue_struct *wq,
				struct delayed_work *dw, unsigned long delay_ms)
{
	lockdep_assert_held(&g_wq_lock);
	if (delay_ms == 0) {
		dw->work.state &= ~WORK_STATE_PENDING; /* insert sets it */
		insert_work_locked(wq, &dw->work);
		return 1;
	}
	dw->wq = wq;
	dw->work.wq = wq;
	dw->work.state |= WORK_STATE_TIMER;
	wq->nr_delayed++;
	/* Armed under the lock: the callback takes the lock first thing,
	 * so it sees TIMER set. */
	hrtimer_start(&dw->timer,
		      hrtimer_now_ns() + (uint64_t)delay_ms * 1000000ull);
	return 0;
}

static void wake_waiters_if_any(int any)
{
	if (any)
		wake_up(&g_wq_done);
}

/* ==========================================================================
 * Initialisation of items
 * ========================================================================== */

static void delayed_work_timer_fn(hrtimer_t *t);

void __init_work(struct work_struct *work, work_func_t func, int delayed)
{
	BUG_ON(work == NULL || func == NULL);
	INIT_LIST_HEAD(&work->entry);
	work->func = func;
	work->wq = NULL;
	work->state = 0;
	work->nr_claimed = 0;
	work->qseq = 0;
	work->is_delayed = delayed ? 1u : 0u;
	__atomic_store_n(&work->magic, WORK_INIT_MAGIC, __ATOMIC_RELEASE);
}

void __init_delayed_work(struct delayed_work *dwork, work_func_t func)
{
	BUG_ON(dwork == NULL);
	__init_work(&dwork->work, func, 1);
	hrtimer_init(&dwork->timer, delayed_work_timer_fn, dwork);
	dwork->wq = NULL;
	dwork->stale_fires = 0;
}

/* ==========================================================================
 * Queueing (any context)
 * ========================================================================== */

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	uint64_t fl;
	bool queued = false;

	if (WARN_ON(wq == NULL || work == NULL))
		return false;
	work_check_init(work);
	spin_lock_irqsave(&g_wq_lock, &fl);
	if (!(work->state & WORK_STATE_PENDING)) {
		insert_work_locked(wq, work);
		queued = true;
	}
	spin_unlock_irqrestore(&g_wq_lock, fl);
	if (queued)
		wake_up(&wq->more_work);
	return queued;
}

bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay_ms)
{
	uint64_t fl;
	int wake = 0;
	bool queued = false;

	if (WARN_ON(wq == NULL || dwork == NULL))
		return false;
	work_check_init(&dwork->work);
	WARN_ON_ONCE(!dwork->work.is_delayed);
	spin_lock_irqsave(&g_wq_lock, &fl);
	if (!(dwork->work.state & WORK_STATE_PENDING)) {
		dwork->work.state |= WORK_STATE_PENDING;
		wake = queue_delayed_locked(wq, dwork, delay_ms);
		queued = true;
	}
	spin_unlock_irqrestore(&g_wq_lock, fl);
	if (wake)
		wake_up(&wq->more_work);
	return queued;
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
		      unsigned long delay_ms)
{
	uint64_t fl;
	int ret, wake = 0;

	if (WARN_ON(wq == NULL || dwork == NULL))
		return false;
	work_check_init(&dwork->work);
	spin_lock_irqsave(&g_wq_lock, &fl);
	ret = grab_pending_locked(&dwork->work);
	if (ret >= 0) {
		dwork->work.state |= WORK_STATE_PENDING;
		wake = queue_delayed_locked(wq, dwork, delay_ms);
	}
	spin_unlock_irqrestore(&g_wq_lock, fl);
	if (wake)
		wake_up(&wq->more_work);
	return ret == 1;
}

/* The timer of a delayed item fired (hard interrupt context). */
static void delayed_work_timer_fn(hrtimer_t *t)
{
	struct delayed_work *dw = t->arg;
	struct workqueue_struct *wq = NULL;
	uint64_t fl;
	int waiters = 0;

	spin_lock_irqsave(&g_wq_lock, &fl);
	if (dw->stale_fires) {
		/* The callback of an arming that was cancelled while it was
		 * already on its way: nothing to do but be counted out. */
		dw->stale_fires--;
		waiters = g_wq_waiters > 0;
	} else if (dw->work.state & WORK_STATE_TIMER) {
		dw->work.state &= ~(WORK_STATE_TIMER | WORK_STATE_PENDING);
		wq = dw->wq;
		wq->nr_delayed--;
		insert_work_locked(wq, &dw->work);
	}
	/* Last access to the item. */
	spin_unlock_irqrestore(&g_wq_lock, fl);
	if (wq)
		wake_up(&wq->more_work);
	wake_waiters_if_any(waiters);
}

/* ==========================================================================
 * Cancelling and flushing
 * ========================================================================== */

bool current_is_work(const struct work_struct *work)
{
	task_t *cur = sched_current();
	struct wq_worker *w;
	uint64_t fl;
	bool r = false;

	if (!cur)
		return false;
	spin_lock_irqsave(&g_wq_lock, &fl);
	w = work_running_locked(work);
	if (w && w->task == cur)
		r = true;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

bool current_is_workqueue_worker(void)
{
	task_t *cur = sched_current();
	struct list_head *p;
	uint64_t fl;
	bool r = false;

	if (!cur)
		return false;
	spin_lock_irqsave(&g_wq_lock, &fl);
	for (p = g_wqs.next; p != &g_wqs && !r; p = p->next) {
		struct workqueue_struct *wq =
			container_of(p, struct workqueue_struct, node);

		for (int i = 0; i < wq->nworkers; i++)
			if (wq->workers[i] && wq->workers[i]->task == cur)
				r = true;
	}
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

bool current_is_worker_of(const struct workqueue_struct *wq)
{
	task_t *cur = sched_current();
	uint64_t fl;
	bool r = false;

	if (!cur || !wq)
		return false;
	spin_lock_irqsave(&g_wq_lock, &fl);
	for (int i = 0; i < wq->nworkers; i++)
		if (wq->workers[i] && wq->workers[i]->task == cur)
			r = true;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

bool cancel_work(struct work_struct *work)
{
	uint64_t fl;
	int ret;

	if (WARN_ON(work == NULL))
		return false;
	work_check_init(work);
	spin_lock_irqsave(&g_wq_lock, &fl);
	ret = grab_pending_locked(work);
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return ret == 1;
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
	if (WARN_ON(dwork == NULL))
		return false;
	return cancel_work(&dwork->work);
}

/* Conditions of the waits below (evaluated by wait_event, lock taken). */
static int cancel_owner_gone(struct work_struct *work)
{
	uint64_t fl;
	int r;

	spin_lock_irqsave(&g_wq_lock, &fl);
	r = !(work->state & WORK_STATE_CANCELING);
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

static int work_quiesced(struct work_struct *work)
{
	uint64_t fl;
	int r;

	spin_lock_irqsave(&g_wq_lock, &fl);
	r = work_running_locked(work) == NULL;
	if (r && work->is_delayed)
		r = to_delayed_work(work)->stale_fires == 0;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

/* Wait for `cond(work)' as one of the g_wq_done waiters.  Before the
 * scheduler there is nobody to wait for: a worker cannot have run yet. */
static void wq_wait_for(int (*cond)(struct work_struct *),
			struct work_struct *work)
{
	uint64_t fl;

	if (cond(work))
		return;
	if (WARN_ON_ONCE(sched_current() == NULL))
		return;
	spin_lock_irqsave(&g_wq_lock, &fl);
	g_wq_waiters++;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	wait_event(g_wq_done, cond(work));
	spin_lock_irqsave(&g_wq_lock, &fl);
	BUG_ON(g_wq_waiters <= 0);
	g_wq_waiters--;
	spin_unlock_irqrestore(&g_wq_lock, fl);
}

static bool __cancel_work_sync(struct work_struct *work)
{
	uint64_t fl;
	int ret, waiters;

	might_sleep();
	work_check_init(work);
	if (WARN(current_is_work(work),
		 "workqueue: item %p cancels itself synchronously",
		 (void *)work))
		return false;
	spin_lock_irqsave(&g_wq_lock, &fl);
	for (;;) {
		ret = grab_pending_locked(work);
		if (ret != -ENOENT)
			break;
		/* Another cancel holds it: wait until it is through, then
		 * take our own turn (the item may have been queued again
		 * in between). */
		spin_unlock_irqrestore(&g_wq_lock, fl);
		wq_wait_for(cancel_owner_gone, work);
		spin_lock_irqsave(&g_wq_lock, &fl);
	}
	/* Hold PENDING: queue_work refuses the item until we are done, so a
	 * running instance cannot queue itself again behind our back. */
	work->state |= WORK_STATE_PENDING | WORK_STATE_CANCELING;
	spin_unlock_irqrestore(&g_wq_lock, fl);

	wq_wait_for(work_quiesced, work);

	spin_lock_irqsave(&g_wq_lock, &fl);
	BUG_ON(work->state & (WORK_STATE_QUEUED | WORK_STATE_TIMER));
	work->state &= ~(WORK_STATE_PENDING | WORK_STATE_CANCELING);
	waiters = g_wq_waiters > 0;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	wake_waiters_if_any(waiters); /* a second canceller */
	return ret == 1;
}

bool cancel_work_sync(struct work_struct *work)
{
	if (WARN_ON(work == NULL))
		return false;
	return __cancel_work_sync(work);
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	if (WARN_ON(dwork == NULL))
		return false;
	return __cancel_work_sync(&dwork->work);
}

/* flush_work's target claim travels through this pair. */
struct flush_target {
	struct work_struct *work;
	uint32_t claim;
};

static int flush_done(struct flush_target *ft)
{
	struct work_struct *work = ft->work;
	uint64_t fl;
	int r;

	spin_lock_irqsave(&g_wq_lock, &fl);
	r = !((work->state & WORK_STATE_QUEUED) &&
	      work->nr_claimed + 1u == ft->claim) &&
	    !work_running_claim_locked(work, ft->claim);
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

bool flush_work(struct work_struct *work)
{
	struct flush_target ft;
	uint64_t fl;

	if (WARN_ON(work == NULL))
		return false;
	might_sleep();
	work_check_init(work);
	if (WARN(current_is_work(work),
		 "workqueue: item %p flushes itself", (void *)work))
		return false;
	spin_lock_irqsave(&g_wq_lock, &fl);
	ft.work = work;
	if (work->state & WORK_STATE_QUEUED) {
		ft.claim = work->nr_claimed + 1u;
	} else if (work_running_locked(work)) {
		ft.claim = work->nr_claimed;
	} else {
		spin_unlock_irqrestore(&g_wq_lock, fl);
		return false;
	}
	if (sched_current() == NULL) {
		/* Boot: no worker runs yet, nothing can complete. */
		spin_unlock_irqrestore(&g_wq_lock, fl);
		WARN_ON_ONCE(1);
		return false;
	}
	g_wq_waiters++;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	wait_event(g_wq_done, flush_done(&ft));
	spin_lock_irqsave(&g_wq_lock, &fl);
	g_wq_waiters--;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return true;
}

bool flush_delayed_work(struct delayed_work *dwork)
{
	struct workqueue_struct *wq = NULL;
	uint64_t fl;

	if (WARN_ON(dwork == NULL))
		return false;
	work_check_init(&dwork->work);
	spin_lock_irqsave(&g_wq_lock, &fl);
	if ((dwork->work.state & WORK_STATE_TIMER) &&
	    !(dwork->work.state & WORK_STATE_CANCELING)) {
		/* Run it now rather than at its deadline. */
		if (!hrtimer_cancel(&dwork->timer))
			dwork->stale_fires++;
		dwork->work.state &= ~(WORK_STATE_TIMER | WORK_STATE_PENDING);
		wq = dwork->wq;
		wq->nr_delayed--;
		insert_work_locked(wq, &dwork->work);
	}
	spin_unlock_irqrestore(&g_wq_lock, fl);
	if (wq)
		wake_up(&wq->more_work);
	return flush_work(&dwork->work);
}

/* ==========================================================================
 * Workers
 * ========================================================================== */

/* The next item a worker of `wq' may run: the first one no other worker
 * runs (on an ordered queue: the first one, or nothing).  Marks the
 * worker running a skipped item so it kicks the queues when done. */
static struct work_struct *find_claimable_locked(struct workqueue_struct *wq)
{
	struct list_head *p;

	lockdep_assert_held(&g_wq_lock);
	for (p = wq->worklist.next; p != &wq->worklist; p = p->next) {
		struct work_struct *work =
			container_of(p, struct work_struct, entry);
		struct wq_worker *runner = work_running_locked(work);

		if (!runner)
			return work;
		runner->kick_all = 1;
		if (wq->flags & WQ_ORDERED)
			return NULL;
	}
	return NULL;
}

static int worker_should_wake(struct wq_worker *w)
{
	struct workqueue_struct *wq = w->wq;
	uint64_t fl;
	int r;

	spin_lock_irqsave(&g_wq_lock, &fl);
	r = find_claimable_locked(wq) != NULL ||
	    (wq->destroying && list_empty(&wq->worklist));
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

/* Wake the idle workers of every queue (g_wq_lock held: the wait queue
 * locks nest inside it -- no path takes g_wq_lock under a wait queue
 * lock). */
static void kick_all_queues_locked(void)
{
	struct list_head *p;

	lockdep_assert_held(&g_wq_lock);
	for (p = g_wqs.next; p != &g_wqs; p = p->next)
		wake_up(&container_of(p, struct workqueue_struct, node)
				 ->more_work);
}

static void worker_main(void *arg)
{
	struct wq_worker *w = arg;
	struct workqueue_struct *wq = w->wq;

	for (;;) {
		struct work_struct *work;
		work_func_t fn;
		uint64_t fl;
		int waiters;

		wait_event(wq->more_work, worker_should_wake(w));
		spin_lock_irqsave(&g_wq_lock, &fl);
		work = find_claimable_locked(wq);
		if (!work) {
			if (wq->destroying && list_empty(&wq->worklist)) {
				w->exited = 1;
				wq->nexited++;
				spin_unlock_irqrestore(&g_wq_lock, fl);
				wake_up(&g_wq_done);
				/* Last access to the worker and the queue:
				 * the destroyer frees them once this task is
				 * gone. */
				return;
			}
			spin_unlock_irqrestore(&g_wq_lock, fl);
			continue;
		}
		list_del(&work->entry);
		INIT_LIST_HEAD(&work->entry);
		BUG_ON(!(work->state & WORK_STATE_PENDING));
		/* PENDING goes before the run: an event during the run
		 * queues the item again. */
		work->state &= ~(WORK_STATE_QUEUED | WORK_STATE_PENDING);
		work->nr_claimed++;
		fn = work->func;
		w->current_work = work;
		w->current_func = fn;
		w->current_claim = work->nr_claimed;
		w->current_qseq = work->qseq;
		w->kick_all = 0;
		list_add_tail(&w->busy_node, &g_busy);
		spin_unlock_irqrestore(&g_wq_lock, fl);

		fn(work); /* may free `work' */

		WARN_RATELIMIT(irqs_disabled(),
			       "workqueue %s: %p left interrupts disabled",
			       wq->name, (void *)fn);
		spin_lock_irqsave(&g_wq_lock, &fl);
		list_del(&w->busy_node);
		INIT_LIST_HEAD(&w->busy_node);
		w->current_work = NULL;
		w->current_func = NULL;
		wq->nr_executed++;
		if (w->kick_all) {
			w->kick_all = 0;
			kick_all_queues_locked();
		}
		waiters = g_wq_waiters > 0;
		spin_unlock_irqrestore(&g_wq_lock, fl);
		wake_waiters_if_any(waiters);
	}
}

/* ==========================================================================
 * Queues
 * ========================================================================== */

static void wq_name_copy(char *dst, size_t cap, const char *src)
{
	size_t i = 0;

	if (src)
		for (; src[i] && i + 1 < cap; i++)
			dst[i] = src[i];
	dst[i] = '\0';
}

static int start_worker(struct workqueue_struct *wq, int id)
{
	struct wq_worker *w;

	w = kcalloc(1, sizeof(*w));
	if (!w)
		return -ENOMEM;
	w->wq = wq;
	w->id = id;
	INIT_LIST_HEAD(&w->busy_node);
	w->stack = mm_alloc_guarded_kstack(WQ_WORKER_STACK);
	if (!w->stack) {
		kfree(w);
		return -ENOMEM;
	}
	/* Published before the thread can run. */
	wq->workers[id] = w;
	w->task = sched_add_task(worker_main, w, w->stack, WQ_WORKER_STACK);
	if (!w->task) {
		wq->workers[id] = NULL;
		/* The task may have been half set up: the stack stays. */
		kfree(w);
		return -ENOMEM;
	}
	w->task_id = (uint32_t)w->task->id;
	ksnprintf(w->task->comm, sizeof(w->task->comm), "kworker/%s:%d",
		  wq->name, id);
	if (wq->flags & WQ_HIGHPRI)
		sched_set_user_nice(w->task, SCHED_MIN_NICE);
	return 0;
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
					 int max_active)
{
	struct workqueue_struct *wq;
	uint64_t fl;
	int n;

	might_sleep();
	if (sched_current() == NULL) {
		WARN(1, "workqueue: %s created before the scheduler",
		     name ? name : "?");
		return NULL;
	}
	if (flags & WQ_ORDERED)
		n = 1;
	else if (max_active <= 0)
		n = WQ_DEFAULT_ACTIVE;
	else
		n = max_active > WQ_MAX_ACTIVE ? WQ_MAX_ACTIVE : max_active;
	wq = kcalloc(1, sizeof(*wq));
	if (!wq)
		return NULL;
	wq_name_copy(wq->name, sizeof(wq->name), name);
	wq->flags = flags;
	INIT_LIST_HEAD(&wq->worklist);
	INIT_LIST_HEAD(&wq->node);
	wq_head_init(&wq->more_work, "wq_more_work");
	spin_lock_irqsave(&g_wq_lock, &fl);
	list_add_tail(&wq->node, &g_wqs);
	spin_unlock_irqrestore(&g_wq_lock, fl);
	for (int i = 0; i < n; i++) {
		if (start_worker(wq, i) != 0)
			break;
		spin_lock_irqsave(&g_wq_lock, &fl);
		wq->nworkers = i + 1;
		spin_unlock_irqrestore(&g_wq_lock, fl);
	}
	if (wq->nworkers == 0) {
		spin_lock_irqsave(&g_wq_lock, &fl);
		list_del(&wq->node);
		spin_unlock_irqrestore(&g_wq_lock, fl);
		kfree(wq);
		return NULL;
	}
	if (wq->nworkers < n)
		kprintf("workqueue: %s runs with %d of %d workers\n", wq->name,
			wq->nworkers, n);
	return wq;
}

/* Everything inserted into `wq' up to insertion sequence `seq' has run:
 * no item on the list was inserted at or before it, and no worker of the
 * queue runs one.  Only the list (whose items are alive while queued) and
 * the workers' own records are read: an item that ran may be gone. */
struct wq_flush_state {
	struct workqueue_struct *wq;
	uint64_t seq;
};

static int wq_flushed(struct wq_flush_state *fs)
{
	struct workqueue_struct *wq = fs->wq;
	struct list_head *p;
	uint64_t fl;
	int r = 1;

	spin_lock_irqsave(&g_wq_lock, &fl);
	for (p = wq->worklist.next; r && p != &wq->worklist; p = p->next)
		if (container_of(p, struct work_struct, entry)->qseq <= fs->seq)
			r = 0;
	for (int i = 0; r && i < wq->nworkers; i++) {
		struct wq_worker *w = wq->workers[i];

		if (w && w->current_work && w->current_qseq <= fs->seq)
			r = 0;
	}
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

void flush_workqueue(struct workqueue_struct *wq)
{
	struct wq_flush_state fs;
	uint64_t fl;

	if (WARN_ON(wq == NULL))
		return;
	might_sleep();
	spin_lock_irqsave(&g_wq_lock, &fl);
	fs.wq = wq;
	fs.seq = wq->seq;
	if (sched_current() == NULL) {
		spin_unlock_irqrestore(&g_wq_lock, fl);
		return;
	}
	g_wq_waiters++;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	wait_event(g_wq_done, wq_flushed(&fs));
	spin_lock_irqsave(&g_wq_lock, &fl);
	g_wq_waiters--;
	spin_unlock_irqrestore(&g_wq_lock, fl);
}

static int wq_idle(struct workqueue_struct *wq)
{
	uint64_t fl;
	int r;

	spin_lock_irqsave(&g_wq_lock, &fl);
	r = list_empty(&wq->worklist);
	for (int i = 0; r && i < wq->nworkers; i++)
		if (wq->workers[i] && wq->workers[i]->current_work)
			r = 0;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

void drain_workqueue(struct workqueue_struct *wq)
{
	unsigned int rounds = 0;

	if (WARN_ON(wq == NULL))
		return;
	might_sleep();
	while (!wq_idle(wq)) {
		flush_workqueue(wq);
		if (++rounds == 100)
			kprintf("workqueue %s: still busy after %u flushes\n",
				wq->name, rounds);
	}
}

static int wq_all_exited(struct workqueue_struct *wq)
{
	uint64_t fl;
	int r;

	spin_lock_irqsave(&g_wq_lock, &fl);
	r = wq->nexited == wq->nworkers;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	return r;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	uint64_t fl;

	if (WARN_ON(wq == NULL))
		return;
	might_sleep();
	WARN_ON(wq == system_wq || wq == system_highpri_wq ||
		wq == system_long_wq);
	drain_workqueue(wq);
	spin_lock_irqsave(&g_wq_lock, &fl);
	if (WARN(wq->nr_delayed,
		 "workqueue %s: destroyed with %d delayed item(s) armed",
		 wq->name, wq->nr_delayed)) {
		/* Their timers would queue into freed memory: keep the
		 * queue (and its workers) rather than free it. */
		spin_unlock_irqrestore(&g_wq_lock, fl);
		return;
	}
	wq->destroying = 1;
	g_wq_waiters++;
	spin_unlock_irqrestore(&g_wq_lock, fl);
	wake_up(&wq->more_work);
	wait_event(g_wq_done, wq_all_exited(wq));
	spin_lock_irqsave(&g_wq_lock, &fl);
	g_wq_waiters--;
	list_del(&wq->node);
	spin_unlock_irqrestore(&g_wq_lock, fl);
	for (int i = 0; i < wq->nworkers; i++) {
		struct wq_worker *w = wq->workers[i];
		unsigned int waited = 0;

		if (!w)
			continue;
		/* The thread returned from worker_main; its stack is free
		 * once the task itself is gone. */
		while (sched_find_task_by_id(w->task_id) != NULL &&
		       waited < WQ_REAP_WAIT_MS) {
			msleep(1);
			waited++;
		}
		if (waited < WQ_REAP_WAIT_MS)
			mm_free_guarded_kstack(w->stack, WQ_WORKER_STACK);
		else
			kprintf("workqueue %s: worker %d not reaped, stack kept\n",
				wq->name, i);
		kfree(w);
	}
	kfree(wq);
}

void workqueue_init(void)
{
	BUG_ON(sched_current() == NULL);
	if (system_wq)
		return;
	system_wq = alloc_workqueue("events", 0, WQ_DEFAULT_ACTIVE);
	system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 2);
	system_long_wq = alloc_workqueue("events_long", 0, 2);
	/* Every queue here is unbound and nothing is frozen. */
	system_unbound_wq = system_wq;
	system_freezable_wq = system_wq;
	if (!system_wq || !system_highpri_wq || !system_long_wq)
		kprintf("workqueue: system queues incomplete (%p %p %p)\n",
			(void *)system_wq, (void *)system_highpri_wq,
			(void *)system_long_wq);
	if (!system_highpri_wq)
		system_highpri_wq = system_wq;
	if (!system_long_wq)
		system_long_wq = system_wq;
}
