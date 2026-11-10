// LikeOS - kblockd: the storage stack's per-CPU worker pools
//
// Some of the storage stack's work must run in process context with
// interrupts enabled although the event that calls for it happens in an
// interrupt or in the block softirq: running a request queue after a
// completion freed a tag (the dispatch rings the controller's doorbell),
// the timeout scan of a queue's timer, a requeue.  On a virtual machine a
// doorbell write is an exit to the hypervisor, and an exit the host holds
// up must not be taken with the interrupt path of the processor waiting
// behind it.  Such work is a struct blk_work (blkdev.h) handed to kblockd.
//
// kblockd is one pool of worker threads per CPU.  Each pool has its own
// list under its own leaf lock, and its workers -- kblockd/N:M, pinned to
// CPU N, at nice -20, the highest priority of the fair scheduler -- run
// the items queued on it.  An item goes to the list of the CPU that queues
// it (kblockd_schedule_work) or of a chosen CPU, now or after a delay
// (kblockd_mod_delayed_work_on, with a high-resolution timer whose callback
// does nothing but queue).
//
// Items:
//
//   * PENDING (w->flags) is owned by the queuer.  It is set atomically
//     before the item is queued or its timer armed, and cleared by the
//     worker that claims the item, under the pool lock, before the function
//     runs.  Queueing a pending item does nothing, so a burst of requests
//     costs one run; a request arriving while the item runs queues it again
//     and is not lost;
//   * an item never runs twice at once.  Queued while it runs, it goes to
//     the pool that runs it (whatever CPU was asked for); claimed by a
//     worker of that pool while another worker of the pool runs it, it is
//     moved to that worker's own list and runs after the current run;
//   * re-aiming a pending item first takes PENDING back: from its timer (a
//     cancelled timer hands it over), or from the list it is on.  In
//     between -- its timer fired and the callback is queueing it on another
//     CPU right now -- the taker waits the few instructions that takes,
//     with interrupts off.
//
// Workers (concurrency management):
//
//   * a pool counts its workers that are running items (nr_running).  One
//     running worker is enough: it takes item after item.  When it blocks
//     inside an item the scheduler tells the pool (kblockd_worker_sleeping,
//     called by sched_schedule() for a worker that blocks), nr_running
//     drops, and if items are waiting an idle worker is woken to take them.
//     When the sleeper runs again (kblockd_worker_running) it counts again;
//     with two running, the one that finishes its item goes idle instead of
//     taking the next;
//   * a worker that has run one item for more than KBLOCKD_CPU_INTENSIVE_US
//     of processor time without blocking stops counting as running (it is
//     marked CPU-intensive from the timer tick), so the next item does not
//     wait for it;
//   * there is always an idle worker to wake: a worker about to start
//     working when no other is idle first creates one (the pool's manager;
//     it may sleep for memory).  If creating one takes longer than
//     KBLOCKD_MAYDAY_INITIAL_TICKS while items wait, the pool calls its
//     rescuer, a thread created at start for exactly this, which runs the
//     waiting items one by one until a worker can be made;
//   * idle workers beyond two (and beyond a quarter of the busy ones) are
//     retired after KBLOCKD_IDLE_WORKER_TIMEOUT_MS idle; their stacks are
//     released once their task is gone.
//
// Pre-start: before the workers run (boot) an item queued from process
// context runs inline in the caller, which is what the callers did before
// there were threads; one queued from an interrupt or the softirq (a timer
// callback, say) waits on its list for a worker or a poller.  Every item
// runs as a completion section (blk_cpl_begin, genhd.c), and the workers
// reap what interrupt context parked on the global deferred lists.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/dev/block/blk_softirq.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define KBLOCKD_STACK_SIZE (16u * 1024u)
/* Items a poller runs per call of blk_work_run_pending(): items may queue
 * each other again, the poller's own loop calls again. */
#define KBLOCKD_POLL_BATCH 256
/* Taking PENDING back spins while the item is between owners (a timer
 * callback queueing it on another CPU); this many rounds without progress
 * are reported. */
#define KBLOCKD_GRAB_WARN_SPINS (1u << 24)
/* The workers' nice level: the highest priority. */
#define KBLOCKD_HIGHPRI_NICE SCHED_MIN_NICE
/* Keep idle workers this long before retiring extra ones. */
#define KBLOCKD_IDLE_WORKER_TIMEOUT_MS 300000u
/* Of the busy workers, a quarter may be idle (beyond two). */
#define KBLOCKD_MAX_IDLE_WORKERS_RATIO 4
/* Call the rescuer when a worker could not be made within this many
 * ticks (a hundredth of a second, at least two ticks), and again every
 * tenth of a second while that lasts. */
#define KBLOCKD_MAYDAY_INITIAL_MS 10u
#define KBLOCKD_MAYDAY_INTERVAL_MS 100u
/* Pause between failed attempts to create a worker. */
#define KBLOCKD_CREATE_COOLDOWN_MS 1000u
/* How long a cull waits for the workers it retired to be gone. */
#define KBLOCKD_REAP_WAIT_MS 1000u
/* A run of one item longer than this, without blocking, stops counting
 * as running for concurrency management. */
#define KBLOCKD_CPU_INTENSIVE_US 10000u

/* Worker flags (pool lock). */
#define WORKER_DIE (1u << 1) /* retire */
#define WORKER_IDLE (1u << 2) /* on the idle list */
#define WORKER_PREP (1u << 3) /* not running items */
#define WORKER_CPU_INTENSIVE (1u << 6) /* hogs the processor */
#define WORKER_NOT_RUNNING (WORKER_PREP | WORKER_CPU_INTENSIVE)

/* A list of items: a pool's worklist or a worker's scheduled list. */
struct kblockd_list {
	struct blk_work *head, *tail;
};

struct kblockd_pool;

struct kblockd_worker {
	struct kblockd_pool *pool;
	task_t *volatile task;
	void *stack;
	int stack_static;
	int id;
	int rescuer;
	uint32_t task_id;
	uint32_t flags; /* WORKER_* (pool lock) */
	volatile int sleeping; /* blocked inside an item */
	/* The item it runs (pool lock). */
	struct blk_work *current_work;
	void (*current_func)(void *arg);
	/* Items claimed for this worker, run before it looks at the pool's
	 * list again (pool lock). */
	struct kblockd_list scheduled;
	/* Idle list (LIFO), all workers, retired workers. */
	struct kblockd_worker *idle_next, *idle_prev;
	struct kblockd_worker *all_next;
	struct kblockd_worker *grave_next;
	uint64_t last_active; /* tick it went idle */
	/* Processor time while running items, sampled per tick, and its
	 * value when the current stretch without blocking began. */
	volatile uint64_t cpu_us;
	uint64_t current_at;
	/* RFLAGS with interrupts on, for unlocking the pool in the thread. */
	uint64_t irq_on_flags;
};

struct kblockd_pool {
	spinlock_t lock;
	int cpu;
	struct kblockd_list worklist;
	int nr_workers;
	int nr_idle;
	int nr_running;
	int manager_active;
	int next_id;
	struct kblockd_worker *idle_first, *idle_last;
	struct kblockd_worker *workers;
	struct kblockd_worker *graveyard;
	struct kblockd_worker *rescuer;
	volatile int mayday;
	hrtimer_t idle_timer;
	hrtimer_t mayday_timer;
	struct blk_work idle_cull_work;
	uint64_t runs, cm_wakeups, cpu_intensive, rescued, created, retired;
	char lock_name[16];
} __attribute__((aligned(64)));

static struct kblockd_pool g_kblockd[MAX_CPUS];
static volatile int g_kblockd_inited;
static volatile int g_kblockd_running;
/* No worker could be started at all: every item runs inline in the
 * queuer, in any context, as before there were workers. */
static volatile int g_kblockd_failed;
/* CPU 0's first worker has a static stack, so one worker can always be
 * started on a system short of memory (like kserial's and kahci's). */
static uint8_t g_kblockd_stack0[KBLOCKD_STACK_SIZE] __attribute__((aligned(16)));
static volatile int g_kblockd_stack0_used;

static void kblockd_delayed_timer_fn(hrtimer_t *t);
static void kblockd_worker_thread(void *arg);
static void kblockd_rescuer_thread(void *arg);

/* ---- context ----------------------------------------------------------- */

/* The calling CPU, or 0 before the per-CPU segment may be read. */
static inline uint32_t kblockd_cpu(void)
{
	return g_kblockd_inited ? this_cpu_id() : 0;
}

/* Process context for the pre-start rule: not inside a hard interrupt or
 * a block softirq run. */
static inline int kblockd_process_context(void)
{
	return !blk_in_hardirq() && !blk_in_softirq();
}

/* The pool a CPU number names; CPUs whose pool got no worker once the
 * workers run are served by CPU 0's. */
static inline int kblockd_pool_for(uint32_t cpu)
{
	if (cpu >= MAX_CPUS)
		cpu = 0;
	if (__atomic_load_n(&g_kblockd_running, __ATOMIC_ACQUIRE) &&
	    !g_kblockd[cpu].workers)
		cpu = 0;
	return (int)cpu;
}

/* Pool lock in a worker thread, which runs with interrupts on. */
static inline void pool_lock_irq(struct kblockd_pool *p)
{
	(void)local_irq_save();
	spin_lock(&p->lock);
}

static inline void pool_unlock_irq(struct kblockd_pool *p,
				   struct kblockd_worker *wk)
{
	spin_unlock(&p->lock);
	local_irq_restore(wk->irq_on_flags);
}

/* ---- PENDING ----------------------------------------------------------- */

/* Set PENDING; 1 when this caller set it (and owns the item now). */
static inline int kblockd_test_and_set_pending(struct blk_work *w)
{
	return !(__atomic_fetch_or(&w->flags, BLK_WORK_PENDING,
				   __ATOMIC_ACQ_REL) &
		 BLK_WORK_PENDING);
}

static inline void kblockd_clear_pending(struct blk_work *w)
{
	(void)__atomic_fetch_and(&w->flags, ~BLK_WORK_PENDING,
				 __ATOMIC_RELEASE);
}

/* ---- lists (pool lock) ------------------------------------------------- */

static void klist_add_tail(struct kblockd_list *l, struct blk_work *w)
{
	w->next = NULL;
	w->prev = l->tail;
	if (l->tail)
		l->tail->next = w;
	else
		l->head = w;
	l->tail = w;
	w->list = l;
}

static void klist_del(struct blk_work *w)
{
	struct kblockd_list *l = (struct kblockd_list *)w->list;

	if (WARN_ON(l == NULL))
		return;
	if (w->prev)
		w->prev->next = w->next;
	else
		l->head = w->next;
	if (w->next)
		w->next->prev = w->prev;
	else
		l->tail = w->prev;
	w->next = w->prev = NULL;
	w->list = NULL;
}

/* Off whatever list of this pool it is on. */
static void kblockd_unlink_locked(struct kblockd_pool *p, struct blk_work *w)
{
	lockdep_assert_held(&p->lock);
	klist_del(w);
	w->pool = -1;
	w->queued = 0;
}

/* ---- pool state (pool lock) -------------------------------------------- */

static inline int need_more_worker(struct kblockd_pool *p)
{
	return p->worklist.head != NULL && !p->nr_running;
}

/* Can I start working?  From a worker that is not running yet. */
static inline int may_start_working(struct kblockd_pool *p)
{
	return p->nr_idle;
}

/* Do I keep working?  From a running worker. */
static inline int keep_working(struct kblockd_pool *p)
{
	return p->worklist.head != NULL && p->nr_running <= 1;
}

/* Is a new worker needed?  From the manager. */
static inline int need_to_create_worker(struct kblockd_pool *p)
{
	return need_more_worker(p) && !may_start_working(p);
}

/* Too many idle workers: some may be retired. */
static inline int too_many_workers(struct kblockd_pool *p)
{
	int nr_idle = p->nr_idle + p->manager_active; /* manager counts idle */
	int nr_busy = p->nr_workers - nr_idle;

	return nr_idle > 2 &&
	       (nr_idle - 2) * KBLOCKD_MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/* Into NOT_RUNNING: the worker no longer counts as running. */
static inline void worker_set_flags(struct kblockd_worker *wk, uint32_t flags)
{
	struct kblockd_pool *p = wk->pool;

	lockdep_assert_held(&p->lock);
	if ((flags & WORKER_NOT_RUNNING) && !(wk->flags & WORKER_NOT_RUNNING))
		p->nr_running--;
	wk->flags |= flags;
}

/* Out of NOT_RUNNING (all of its flags): counts as running again. */
static inline void worker_clr_flags(struct kblockd_worker *wk, uint32_t flags)
{
	struct kblockd_pool *p = wk->pool;
	uint32_t oflags = wk->flags;

	lockdep_assert_held(&p->lock);
	wk->flags &= ~flags;
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(wk->flags & WORKER_NOT_RUNNING))
			p->nr_running++;
}

static inline struct kblockd_worker *first_idle_worker(struct kblockd_pool *p)
{
	return p->idle_first;
}

static void kblockd_idle_timer_fn(hrtimer_t *t);

static void worker_enter_idle(struct kblockd_worker *wk)
{
	struct kblockd_pool *p = wk->pool;

	lockdep_assert_held(&p->lock);
	if (WARN_ON_ONCE(wk->flags & WORKER_IDLE) ||
	    WARN_ON_ONCE(wk->current_work != NULL))
		return;
	/* Not worker_set_flags(): a new worker enters idle from here too. */
	wk->flags |= WORKER_IDLE;
	p->nr_idle++;
	wk->last_active = timer_ticks();
	/* LIFO: the most recently idle worker is woken first. */
	wk->idle_prev = NULL;
	wk->idle_next = p->idle_first;
	if (p->idle_first)
		p->idle_first->idle_prev = wk;
	else
		p->idle_last = wk;
	p->idle_first = wk;
	if (too_many_workers(p) && !hrtimer_is_armed(&p->idle_timer))
		hrtimer_start_rel(&p->idle_timer,
				  (uint64_t)KBLOCKD_IDLE_WORKER_TIMEOUT_MS *
					  1000000ULL);
	/* Sanity: no worker can be running when all are idle. */
	WARN_ON_ONCE(p->nr_workers == p->nr_idle && p->nr_running);
}

static void idle_list_del(struct kblockd_worker *wk)
{
	struct kblockd_pool *p = wk->pool;

	if (wk->idle_prev)
		wk->idle_prev->idle_next = wk->idle_next;
	else
		p->idle_first = wk->idle_next;
	if (wk->idle_next)
		wk->idle_next->idle_prev = wk->idle_prev;
	else
		p->idle_last = wk->idle_prev;
	wk->idle_next = wk->idle_prev = NULL;
}

static void worker_leave_idle(struct kblockd_worker *wk)
{
	struct kblockd_pool *p = wk->pool;

	lockdep_assert_held(&p->lock);
	if (WARN_ON_ONCE(!(wk->flags & WORKER_IDLE)))
		return;
	worker_clr_flags(wk, WORKER_IDLE);
	p->nr_idle--;
	idle_list_del(wk);
}

/* The worker of this pool that runs `w' now, if any. */
static struct kblockd_worker *find_worker_executing_work(struct kblockd_pool *p,
							 struct blk_work *w)
{
	lockdep_assert_held(&p->lock);
	for (struct kblockd_worker *wk = p->workers; wk; wk = wk->all_next)
		if (wk->current_work == w && wk->current_func == w->fn)
			return wk;
	if (p->rescuer && p->rescuer->current_work == w &&
	    p->rescuer->current_func == w->fn)
		return p->rescuer;
	return NULL;
}

/* Wake a parked worker task (it parks with wait_channel = its worker). */
static int kblockd_wake_worker(struct kblockd_worker *wk)
{
	task_t *t = wk->task;

	if (t && t->wait_channel == (void *)wk &&
	    sched_claim_wake(t, TASK_BLOCKED)) {
		t->wait_channel = NULL;
		t->wakeup_tick = 0;
		sched_enqueue_ready(t);
		return 1;
	}
	return 0;
}

/* Wake an idle worker if items wait and nobody runs them. */
static int kick_pool(struct kblockd_pool *p)
{
	struct kblockd_worker *wk = first_idle_worker(p);

	lockdep_assert_held(&p->lock);
	if (!need_more_worker(p) || !wk)
		return 0;
	/* A worker that has not parked yet (just created, or on its way to
	 * sleep) looks at the list itself before it parks. */
	(void)kblockd_wake_worker(wk);
	return 1;
}

/* Claim `w' (on the worklist) for `wk', or for the worker of this pool
 * that runs it already.  1 when it is `wk''s now. */
static int assign_work(struct blk_work *w, struct kblockd_worker *wk)
{
	struct kblockd_pool *p = wk->pool;
	struct kblockd_worker *collision;

	lockdep_assert_held(&p->lock);
	collision = find_worker_executing_work(p, w);
	if (collision) {
		klist_del(w);
		klist_add_tail(&collision->scheduled, w);
		return 0;
	}
	klist_del(w);
	klist_add_tail(&wk->scheduled, w);
	return 1;
}

/* ---- queueing ---------------------------------------------------------- */

/* Put an item whose PENDING the caller owns on a pool.  Interrupts off.
 * The pool is `cpu''s unless the item runs somewhere: then it is the
 * runner's, so the item never runs on two CPUs at once. */
static void kblockd_insert(uint32_t cpu, struct blk_work *w)
{
	int idx = kblockd_pool_for(cpu);
	int last = w->last_pool;
	struct kblockd_pool *p;

	if (last >= 0 && last < MAX_CPUS && last != idx) {
		struct kblockd_pool *lp = &g_kblockd[last];

		spin_lock(&lp->lock);
		if (w->run_cpu == last) {
			idx = last;
		} else {
			spin_unlock(&lp->lock);
			spin_lock(&g_kblockd[idx].lock);
		}
	} else {
		spin_lock(&g_kblockd[idx].lock);
	}
	p = &g_kblockd[idx];
	WARN_ON_ONCE(w->pool >= 0 || w->queued);
	klist_add_tail(&p->worklist, w);
	w->pool = idx;
	w->last_pool = idx;
	w->queued = 1;
	(void)kick_pool(p);
	spin_unlock(&p->lock);
}

/* Run `w', whose PENDING the caller owns, here and now (before the
 * workers exist).  The caller's interrupt state is restored for the run. */
static void kblockd_run_inline(struct blk_work *w, uint64_t caller_flags)
{
	__atomic_add_fetch(&w->running, 1, __ATOMIC_ACQ_REL);
	kblockd_clear_pending(w);
	local_irq_restore(caller_flags);
	w->fn(w->arg);
	(void)local_irq_save();
	/* Last access to the item. */
	if ((int)__atomic_sub_fetch(&w->running, 1, __ATOMIC_RELEASE) < 0)
		WARN_ON_ONCE(1);
}

/* Queue an item whose PENDING the caller owns.  Interrupts off.  Returns
 * -1 when it ran inline (no workers yet, process context), 1 otherwise. */
static int kblockd_queue_owned(uint32_t cpu, struct blk_work *w,
			       uint64_t caller_flags)
{
	if (!__atomic_load_n(&g_kblockd_running, __ATOMIC_ACQUIRE) &&
	    (kblockd_process_context() ||
	     __atomic_load_n(&g_kblockd_failed, __ATOMIC_ACQUIRE))) {
		kblockd_run_inline(w, caller_flags);
		return -1;
	}
	kblockd_insert(cpu, w);
	return 1;
}

/* ---- taking PENDING back ----------------------------------------------- */

/* 1: PENDING was held (timer or list) and is the caller's now; 0: the item
 * was idle and the caller set PENDING; -EAGAIN: it is between owners, try
 * again.  Interrupts off. */
static int kblockd_try_grab_pending(struct blk_work *w)
{
	struct kblockd_pool *p;
	int idx;

	/* The timer: cancelled while armed, it never fires for this arming
	 * and its PENDING is ours.  Looked at without the timer lock first:
	 * an item that is not on its timer costs no lock here, and an arming
	 * that this read misses is caught by the -EAGAIN retry. */
	if (hrtimer_is_armed(&w->timer) && hrtimer_cancel(&w->timer))
		return 1;
	if (kblockd_test_and_set_pending(w))
		return 0;
	/* Queued, or being queued: steal it from its list (the pool's or a
	 * worker's), keeping PENDING. */
	idx = __atomic_load_n(&w->pool, __ATOMIC_ACQUIRE);
	if (idx < 0 || idx >= MAX_CPUS)
		return -EAGAIN;
	p = &g_kblockd[idx];
	spin_lock(&p->lock);
	if (w->pool == idx) {
		kblockd_unlink_locked(p, w);
		spin_unlock(&p->lock);
		return 1;
	}
	spin_unlock(&p->lock);
	return -EAGAIN;
}

/* Loop until PENDING is the caller's.  Interrupts off. */
static int kblockd_grab_pending(struct blk_work *w)
{
	uint32_t spins = 0;

	for (;;) {
		int ret = kblockd_try_grab_pending(w);

		if (ret != -EAGAIN)
			return ret;
		if (++spins == KBLOCKD_GRAB_WARN_SPINS)
			WARN(1, BLK_LOG_PREFIX
			     "work item %p stays between owners (pool %d, timer armed %d)",
			     (void *)w, w->pool, w->timer.armed);
		__asm__ volatile("pause" ::: "memory");
	}
}

/* ---- the interface ----------------------------------------------------- */

void blk_work_init(struct blk_work *w, void (*fn)(void *arg), void *arg)
{
	BUG_ON(w == NULL || fn == NULL);
	w->fn = fn;
	w->arg = arg;
	w->next = NULL;
	w->prev = NULL;
	w->list = NULL;
	w->flags = 0;
	w->pool = -1;
	w->last_pool = -1;
	w->run_cpu = -1;
	w->queued = 0;
	w->running = 0;
	w->delay_cpu = 0;
	hrtimer_init(&w->timer, kblockd_delayed_timer_fn, w);
}

int blk_kblockd_running(void)
{
	return __atomic_load_n(&g_kblockd_running, __ATOMIC_ACQUIRE);
}

int blk_in_kblockd(void)
{
	task_t *cur;

	if (!blk_kblockd_running())
		return 0;
	cur = sched_current();
	return cur != NULL && cur->wq_worker != NULL;
}

/* Queue on `cpu' unless pending.  1 queued, 0 pending already, -1 ran
 * inline. */
static int kblockd_queue_work_on(uint32_t cpu, struct blk_work *w)
{
	uint64_t fl;
	int ret = 0;

	if (WARN_ON(w == NULL || w->fn == NULL))
		return 0;
	fl = local_irq_save();
	if (kblockd_test_and_set_pending(w))
		ret = kblockd_queue_owned(cpu, w, fl);
	local_irq_restore(fl);
	return ret;
}

int blk_work_queue(struct blk_work *w)
{
	return kblockd_queue_work_on(kblockd_cpu(), w);
}

int kblockd_schedule_work(struct blk_work *w)
{
	return kblockd_queue_work_on(kblockd_cpu(), w) != 0;
}

/* The delay is over: queue on the CPU the delay was aimed at.  Hard
 * interrupt (the timer interrupt), interrupts off; PENDING is the
 * timer's and passes to the list. */
static void kblockd_delayed_timer_fn(hrtimer_t *t)
{
	struct blk_work *w = (struct blk_work *)t->arg;

	kblockd_insert(w->delay_cpu, w);
}

/* Aim an item whose PENDING the caller owns.  Interrupts off. */
static void kblockd_queue_delayed_owned(uint32_t cpu, struct blk_work *w,
					uint64_t delay_ns, uint64_t caller_flags)
{
	WARN_ON_ONCE(w->timer.fn != kblockd_delayed_timer_fn);
	WARN_ON_ONCE(hrtimer_is_armed(&w->timer));
	WARN_ON_ONCE(w->queued);
	/* No delay: now, without a timer round trip. */
	if (delay_ns == 0) {
		(void)kblockd_queue_owned(cpu, w, caller_flags);
		return;
	}
	w->delay_cpu = cpu;
	hrtimer_start_rel(&w->timer, delay_ns);
}

int kblockd_mod_delayed_work_on(uint32_t cpu, struct blk_work *w,
				uint64_t delay_ns)
{
	uint64_t fl;
	int ret;

	if (WARN_ON(w == NULL || w->fn == NULL))
		return 0;
	fl = local_irq_save();
	ret = kblockd_grab_pending(w);
	kblockd_queue_delayed_owned(cpu, w, delay_ns, fl);
	local_irq_restore(fl);
	return ret;
}

/* Is `w' being run by a kblockd thread that is the caller? */
static int kblockd_runs_own(struct blk_work *w)
{
	task_t *cur = sched_current();
	struct kblockd_worker *wk;

	if (!cur || !cur->wq_worker)
		return 0;
	wk = (struct kblockd_worker *)cur->wq_worker;
	return wk->current_work == w;
}

int blk_work_cancel_sync(struct blk_work *w, uint32_t timeout_ms)
{
	uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;
	uint64_t fl;

	if (!w)
		return 0;
	/* Take PENDING back from wherever it is and drop it: the item is
	 * idle from here on unless somebody queues it again. */
	fl = local_irq_save();
	(void)kblockd_grab_pending(w);
	kblockd_clear_pending(w);
	local_irq_restore(fl);

	for (;;) {
		if (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE) == 0)
			return 0;
		/* Freeing the item from inside its own function: the run
		 * touches the item once more when the function returns. */
		if (WARN(kblockd_runs_own(w),
			 BLK_LOG_PREFIX "work item cancelled from its own run"))
			return -EBUSY;
		if (waited_us >= limit_us)
			return -ETIMEDOUT;
		lapic_delay_us(10);
		waited_us += 10;
	}
}

/* Pollers run items off a pool's worklist that nobody runs. */
static struct blk_work *kblockd_poll_take_locked(struct kblockd_pool *p)
{
	struct blk_work *w;

	lockdep_assert_held(&p->lock);
	for (w = p->worklist.head; w; w = w->next)
		if (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE) == 0)
			break;
	if (!w)
		return NULL;
	kblockd_unlink_locked(p, w);
	w->run_cpu = p->cpu;
	__atomic_add_fetch(&w->running, 1, __ATOMIC_ACQ_REL);
	kblockd_clear_pending(w);
	return w;
}

static void kblockd_poll_finish(struct kblockd_pool *p, struct blk_work *w)
{
	uint64_t fl;

	spin_lock_irqsave(&p->lock, &fl);
	/* The last runner clears the attribution BEFORE its release-ordered
	 * decrement, which is its last access to the item: the owner may
	 * free it the moment blk_work_cancel_sync() reads running == 0. */
	if (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE) == 1)
		w->run_cpu = -1;
	(void)__atomic_sub_fetch(&w->running, 1, __ATOMIC_RELEASE);
	/* Queued again meanwhile (onto this pool): make sure a worker
	 * looks. */
	(void)kick_pool(p);
	spin_unlock_irqrestore(&p->lock, fl);
}

void blk_work_run_pending(void)
{
	uint32_t me = kblockd_cpu();
	int n = 0;

	for (uint32_t k = 0; k < MAX_CPUS && n < KBLOCKD_POLL_BATCH; k++) {
		struct kblockd_pool *p = &g_kblockd[(me + k) % MAX_CPUS];

		while (n < KBLOCKD_POLL_BATCH) {
			struct blk_work *w;
			uint64_t fl;

			if (!__atomic_load_n(&p->worklist.head, __ATOMIC_ACQUIRE))
				break;
			spin_lock_irqsave(&p->lock, &fl);
			w = kblockd_poll_take_locked(p);
			spin_unlock_irqrestore(&p->lock, fl);
			if (!w)
				break;
			w->fn(w->arg);
			kblockd_poll_finish(p, w);
			n++;
		}
	}
}

/* ---- scheduler hooks --------------------------------------------------- */

void kblockd_worker_sleeping(task_t *t)
{
	struct kblockd_worker *wk = (struct kblockd_worker *)t->wq_worker;
	struct kblockd_pool *p;
	uint64_t fl;

	/* Idle, preparing, rescuing or CPU-intensive: not counted. */
	if (!wk || (wk->flags & WORKER_NOT_RUNNING))
		return;
	p = wk->pool;
	/* Preempted before kblockd_worker_running() of the last sleep. */
	if (__atomic_load_n(&wk->sleeping, __ATOMIC_ACQUIRE))
		return;
	__atomic_store_n(&wk->sleeping, 1, __ATOMIC_RELEASE);
	spin_lock_irqsave(&p->lock, &fl);
	if (wk->flags & WORKER_NOT_RUNNING) {
		spin_unlock_irqrestore(&p->lock, fl);
		return;
	}
	p->nr_running--;
	if (kick_pool(p))
		p->cm_wakeups++;
	spin_unlock_irqrestore(&p->lock, fl);
}

void kblockd_worker_running(task_t *t)
{
	struct kblockd_worker *wk = (struct kblockd_worker *)t->wq_worker;
	uint64_t fl;

	if (!wk || !__atomic_load_n(&wk->sleeping, __ATOMIC_ACQUIRE))
		return;
	spin_lock_irqsave(&wk->pool->lock, &fl);
	if (!(wk->flags & WORKER_NOT_RUNNING))
		wk->pool->nr_running++;
	spin_unlock_irqrestore(&wk->pool->lock, fl);
	/* The stretch without blocking starts again. */
	wk->current_at = wk->cpu_us;
	__atomic_store_n(&wk->sleeping, 0, __ATOMIC_RELEASE);
}

void kblockd_worker_tick(task_t *t)
{
	struct kblockd_worker *wk = (struct kblockd_worker *)t->wq_worker;
	struct kblockd_pool *p;

	if (!wk || !wk->current_work)
		return;
	wk->cpu_us += timer_us_per_tick();
	if ((wk->flags & WORKER_NOT_RUNNING) ||
	    __atomic_load_n(&wk->sleeping, __ATOMIC_ACQUIRE) ||
	    wk->cpu_us - wk->current_at < KBLOCKD_CPU_INTENSIVE_US)
		return;
	p = wk->pool;
	spin_lock(&p->lock);
	worker_set_flags(wk, WORKER_CPU_INTENSIVE);
	p->cpu_intensive++;
	if (kick_pool(p))
		p->cm_wakeups++;
	spin_unlock(&p->lock);
}

/* ---- running items ----------------------------------------------------- */

/* Release what interrupt context parked on the global deferred lists
 * (heap requests, partitions, disks), so it does not wait for the next
 * submitter. */
static void kblockd_reap(void)
{
	blk_reap_deferred_requests();
	genhd_reap_deferred();
}

/* Run one item of the worker's scheduled list.  Pool lock held on entry
 * and on return; dropped for the run. */
static void process_one_work(struct kblockd_worker *wk, struct blk_work *w)
{
	struct kblockd_pool *p = wk->pool;
	struct blk_cpl_ctx cpl;

	lockdep_assert_held(&p->lock);
	/* Claim and dequeue. */
	wk->current_work = w;
	wk->current_func = w->fn;
	wk->current_at = wk->cpu_us;
	w->run_cpu = p->cpu;
	__atomic_add_fetch(&w->running, 1, __ATOMIC_ACQ_REL);
	kblockd_unlink_locked(p, w);
	/* Nothing for a pool whose running worker is this one; wakes an
	 * idle worker when this one does not count as running. */
	(void)kick_pool(p);
	kblockd_clear_pending(w);
	p->runs++;
	pool_unlock_irq(p, wk);

	blk_cpl_begin(&cpl);
	wk->current_func(w->arg);

	pool_lock_irq(p);
	/* The run is over: a CPU-intensive mark ends with it. */
	worker_clr_flags(wk, WORKER_CPU_INTENSIVE);
	wk->current_work = NULL;
	wk->current_func = NULL;
	/* Last access to the item: see kblockd_poll_finish(). */
	if (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE) == 1)
		w->run_cpu = -1;
	(void)__atomic_sub_fetch(&w->running, 1, __ATOMIC_RELEASE);
	pool_unlock_irq(p, wk);
	/* No longer counted as running, so a release that frees the item's
	 * owner may cancel it: release what the item parked. */
	blk_cpl_end(&cpl);
	kblockd_reap();
	if (sched_need_resched())
		sched_yield_in_kernel();
	pool_lock_irq(p);
}

static void process_scheduled_works(struct kblockd_worker *wk)
{
	struct blk_work *w;

	while ((w = wk->scheduled.head) != NULL)
		process_one_work(wk, w);
}

/* ---- creating and retiring workers ------------------------------------- */

static void kblockd_name(task_t *t, struct kblockd_worker *wk)
{
	if (wk->rescuer)
		ksnprintf(t->comm, sizeof(t->comm), "kblockd/%d:R",
			  wk->pool->cpu);
	else
		ksnprintf(t->comm, sizeof(t->comm), "kblockd/%d:%d",
			  wk->pool->cpu, wk->id);
}

/* Free retired workers whose task is gone (process context). */
static void kblockd_reap_graveyard(struct kblockd_pool *p)
{
	for (;;) {
		struct kblockd_worker **pp, *wk = NULL;
		uint64_t fl;

		spin_lock_irqsave(&p->lock, &fl);
		for (pp = &p->graveyard; *pp; pp = &(*pp)->grave_next) {
			if (sched_find_task_by_id((*pp)->task_id) == NULL) {
				wk = *pp;
				*pp = wk->grave_next;
				break;
			}
		}
		spin_unlock_irqrestore(&p->lock, fl);
		if (!wk)
			return;
		if (!wk->stack_static)
			mm_free_guarded_kstack(wk->stack, KBLOCKD_STACK_SIZE);
		kfree(wk);
	}
}

/* A new worker for `p', idle and started.  Process context, pool lock not
 * held; may sleep for memory.  NULL when it could not be made. */
static struct kblockd_worker *create_worker(struct kblockd_pool *p,
					    int rescuer)
{
	struct kblockd_worker *wk;
	task_t *t;
	uint64_t fl;

	wk = (struct kblockd_worker *)kalloc(sizeof(*wk));
	if (!wk)
		return NULL;
	mm_memset(wk, 0, sizeof(*wk));
	wk->pool = p;
	wk->rescuer = rescuer;
	/* Born preparing: it counts as running only once it takes items. */
	wk->flags = WORKER_PREP;
	if (p->cpu == 0 && !rescuer &&
	    !__atomic_exchange_n(&g_kblockd_stack0_used, 1, __ATOMIC_ACQ_REL)) {
		wk->stack = g_kblockd_stack0;
		wk->stack_static = 1;
	} else {
		wk->stack = mm_alloc_guarded_kstack(KBLOCKD_STACK_SIZE);
	}
	if (!wk->stack) {
		kfree(wk);
		return NULL;
	}
	spin_lock_irqsave(&p->lock, &fl);
	wk->id = rescuer ? -1 : p->next_id++;
	if (!rescuer) {
		/* Attach and enter idle before the thread exists: it starts
		 * as a woken idle worker would. */
		wk->all_next = p->workers;
		p->workers = wk;
		p->nr_workers++;
		worker_enter_idle(wk);
	} else {
		p->rescuer = wk;
	}
	spin_unlock_irqrestore(&p->lock, fl);

	t = sched_add_task_on_cpu(rescuer ? kblockd_rescuer_thread :
					    kblockd_worker_thread,
				  wk, wk->stack, KBLOCKD_STACK_SIZE,
				  (uint32_t)p->cpu);
	if (!t) {
		spin_lock_irqsave(&p->lock, &fl);
		if (!rescuer) {
			struct kblockd_worker **pp;

			for (pp = &p->workers; *pp; pp = &(*pp)->all_next) {
				if (*pp == wk) {
					*pp = wk->all_next;
					break;
				}
			}
			p->nr_workers--;
			p->nr_idle--;
			idle_list_del(wk);
		} else {
			p->rescuer = NULL;
		}
		spin_unlock_irqrestore(&p->lock, fl);
		if (!wk->stack_static)
			mm_free_guarded_kstack(wk->stack, KBLOCKD_STACK_SIZE);
		kfree(wk);
		return NULL;
	}
	/* The highest priority of the fair scheduler. */
	sched_set_user_nice(t, KBLOCKD_HIGHPRI_NICE);
	kblockd_name(t, wk);
	wk->task_id = (uint32_t)t->id;
	__atomic_store_n(&wk->task, t, __ATOMIC_RELEASE);
	p->created++;
	return wk;
}

static void kblockd_mayday_timer_fn(hrtimer_t *t);

/* Create workers until one is idle, with the pool lock dropped (the
 * caller holds it on entry and gets it back).  The rescuer is called if
 * this takes too long. */
static void maybe_create_worker(struct kblockd_worker *self)
{
	struct kblockd_pool *p = self->pool;

restart:
	pool_unlock_irq(p, self);
	hrtimer_start_rel(&p->mayday_timer,
			  (uint64_t)KBLOCKD_MAYDAY_INITIAL_MS * 1000000ULL);
	for (;;) {
		if (create_worker(p, 0) || !need_to_create_worker(p))
			break;
		(void)hrtimer_sleep_until(hrtimer_now_ns() +
						  (uint64_t)KBLOCKD_CREATE_COOLDOWN_MS *
							  1000000ULL,
					  NULL);
		if (!need_to_create_worker(p))
			break;
	}
	(void)hrtimer_cancel(&p->mayday_timer);
	kblockd_reap_graveyard(p);
	pool_lock_irq(p);
	/* Another worker may have taken the idle one meanwhile. */
	if (need_to_create_worker(p))
		goto restart;
}

/* 1 when this worker managed the pool (and the lock was dropped). */
static int manage_workers(struct kblockd_worker *wk)
{
	struct kblockd_pool *p = wk->pool;

	if (p->manager_active)
		return 0;
	p->manager_active = 1;
	maybe_create_worker(wk);
	p->manager_active = 0;
	return 1;
}

/* Not enough workers could be made: call the rescuer.  Timer interrupt. */
static void kblockd_mayday_timer_fn(hrtimer_t *t)
{
	struct kblockd_pool *p = (struct kblockd_pool *)t->arg;
	int call = 0;

	spin_lock(&p->lock);
	if (need_to_create_worker(p) && p->rescuer) {
		p->mayday = 1;
		call = 1;
		(void)kblockd_wake_worker(p->rescuer);
	}
	spin_unlock(&p->lock);
	if (call || need_to_create_worker(p))
		hrtimer_start_rel(&p->mayday_timer,
				  (uint64_t)KBLOCKD_MAYDAY_INTERVAL_MS * 1000000ULL);
}

/* Idle workers beyond the allowance have been idle long enough: queue the
 * culling.  Timer interrupt. */
static void kblockd_idle_timer_fn(hrtimer_t *t)
{
	struct kblockd_pool *p = (struct kblockd_pool *)t->arg;
	int do_cull = 0;

	if (p->idle_cull_work.flags & BLK_WORK_PENDING)
		return;
	spin_lock(&p->lock);
	if (too_many_workers(p)) {
		/* LIFO: the last one has been idle longest. */
		struct kblockd_worker *wk = p->idle_last;
		uint64_t expires = wk->last_active +
				   timer_ms_to_ticks(KBLOCKD_IDLE_WORKER_TIMEOUT_MS);

		do_cull = (int64_t)(timer_ticks() - expires) >= 0;
		if (!do_cull)
			hrtimer_start_rel(&p->idle_timer,
					  (expires - timer_ticks()) *
						  timer_us_per_tick() * 1000ULL);
	}
	spin_unlock(&p->lock);
	/* The culling runs in another CPU's pool when there is one: a
	 * worker of this pool running it would itself be one idle worker
	 * fewer to retire. */
	if (do_cull && kblockd_test_and_set_pending(&p->idle_cull_work)) {
		uint32_t n = smp_get_cpu_count();
		uint32_t to = n > 1 ? ((uint32_t)p->cpu + 1) % n : 0;

		kblockd_insert(to, &p->idle_cull_work);
	}
}

/* Retire the idle workers beyond the allowance that have been idle long
 * enough.  A kblockd item. */
static void kblockd_idle_cull_fn(void *arg)
{
	struct kblockd_pool *p = (struct kblockd_pool *)arg;
	struct kblockd_worker *dying = NULL;
	uint64_t fl;

	spin_lock_irqsave(&p->lock, &fl);
	while (too_many_workers(p)) {
		struct kblockd_worker *wk = p->idle_last, **pp;
		uint64_t expires = wk->last_active +
				   timer_ms_to_ticks(KBLOCKD_IDLE_WORKER_TIMEOUT_MS);

		if ((int64_t)(timer_ticks() - expires) < 0) {
			hrtimer_start_rel(&p->idle_timer,
					  (expires - timer_ticks()) *
						  timer_us_per_tick() * 1000ULL);
			break;
		}
		if (WARN_ON(wk->current_work) || WARN_ON(wk->scheduled.head) ||
		    WARN_ON(!(wk->flags & WORKER_IDLE)))
			break;
		p->nr_workers--;
		p->nr_idle--;
		idle_list_del(wk);
		for (pp = &p->workers; *pp; pp = &(*pp)->all_next) {
			if (*pp == wk) {
				*pp = wk->all_next;
				break;
			}
		}
		wk->flags |= WORKER_DIE;
		wk->grave_next = dying;
		dying = wk;
		p->retired++;
	}
	spin_unlock_irqrestore(&p->lock, fl);
	/* Wake them to exit, then wait for them to be gone and free what
	 * they leave (their stacks): each returns from its thread function and
	 * is reaped by the scheduler.  Bounded: one that is not gone in time
	 * stays in the graveyard for the next manager or cull. */
	while (dying) {
		struct kblockd_worker *wk = dying;

		dying = wk->grave_next;
		spin_lock_irqsave(&p->lock, &fl);
		wk->grave_next = p->graveyard;
		p->graveyard = wk;
		(void)kblockd_wake_worker(wk);
		spin_unlock_irqrestore(&p->lock, fl);
	}
	for (uint32_t waited_ms = 0;; waited_ms++) {
		int left;

		kblockd_reap_graveyard(p);
		spin_lock_irqsave(&p->lock, &fl);
		left = p->graveyard != NULL;
		spin_unlock_irqrestore(&p->lock, fl);
		if (!left || waited_ms >= KBLOCKD_REAP_WAIT_MS)
			break;
		(void)hrtimer_sleep_until(hrtimer_now_ns() + 1000000ULL, NULL);
	}
}

/* ---- the threads ------------------------------------------------------- */

/* Park the calling worker (pool lock held, released): it is woken by
 * kblockd_wake_worker(). */
static void kblockd_park_locked(struct kblockd_worker *wk)
{
	task_t *cur = sched_current();

	cur->wait_channel = (void *)wk;
	cur->wakeup_tick = 0;
	task_set_blocked(cur);
	pool_unlock_irq(wk->pool, wk);
	sched_schedule();
	cur->wait_channel = NULL;
}

static void kblockd_worker_thread(void *arg)
{
	struct kblockd_worker *wk = (struct kblockd_worker *)arg;
	struct kblockd_pool *p = wk->pool;
	task_t *cur = sched_current();

	BUG_ON(cur == NULL || p == NULL);
	WARN_ON_ONCE(this_cpu_id() != (uint32_t)p->cpu); /* pinned */
	{
		uint64_t fl = local_irq_save();

		wk->irq_on_flags = fl;
		local_irq_restore(fl);
	}
	WARN_ON_ONCE(!(wk->irq_on_flags & (1ULL << 9)));
	cur->wq_worker = wk;

	for (;;) {
		pool_lock_irq(p);
		if (wk->flags & WORKER_DIE) {
			pool_unlock_irq(p, wk);
			cur->wq_worker = NULL;
			return; /* the task exits; its stack waits in the
				 * graveyard */
		}
		worker_leave_idle(wk);
recheck:
		/* Nothing to do, or another worker runs the items. */
		if (!need_more_worker(p))
			goto sleep;
		/* Keep an idle worker in reserve before starting. */
		if (!may_start_working(p) && manage_workers(wk))
			goto recheck;
		worker_clr_flags(wk, WORKER_PREP);
		do {
			struct blk_work *w = p->worklist.head;

			if (assign_work(w, wk))
				process_scheduled_works(wk);
		} while (keep_working(p));
		worker_set_flags(wk, WORKER_PREP);
sleep:
		worker_enter_idle(wk);
		kblockd_park_locked(wk);
	}
}

/* The pool's rescuer: runs waiting items one at a time while no worker
 * can be made.  It never counts as running. */
static void kblockd_rescuer_thread(void *arg)
{
	struct kblockd_worker *rs = (struct kblockd_worker *)arg;
	struct kblockd_pool *p = rs->pool;
	task_t *cur = sched_current();

	BUG_ON(cur == NULL || p == NULL);
	{
		uint64_t fl = local_irq_save();

		rs->irq_on_flags = fl;
		local_irq_restore(fl);
	}
	cur->wq_worker = rs;
	for (;;) {
		pool_lock_irq(p);
		while (p->mayday) {
			p->mayday = 0;
			while (p->worklist.head &&
			       (need_to_create_worker(p) || p->manager_active)) {
				if (assign_work(p->worklist.head, rs)) {
					p->rescued++;
					process_scheduled_works(rs);
				}
			}
			/* Leave the pool to its workers. */
			(void)kick_pool(p);
		}
		WARN_ON_ONCE(!(rs->flags & WORKER_NOT_RUNNING));
		kblockd_park_locked(rs);
	}
}

/* ---- set-up ------------------------------------------------------------ */

void kblockd_init(void)
{
	if (g_kblockd_inited)
		return;
	for (uint32_t i = 0; i < MAX_CPUS; i++) {
		struct kblockd_pool *p = &g_kblockd[i];

		mm_memset(p, 0, sizeof(*p));
		ksnprintf(p->lock_name, sizeof(p->lock_name), "kblockd%u", i);
		spinlock_init(&p->lock, p->lock_name);
		p->cpu = (int)i;
		hrtimer_init(&p->idle_timer, kblockd_idle_timer_fn, p);
		hrtimer_init(&p->mayday_timer, kblockd_mayday_timer_fn, p);
		blk_work_init(&p->idle_cull_work, kblockd_idle_cull_fn, p);
	}
	__atomic_store_n(&g_kblockd_inited, 1, __ATOMIC_RELEASE);
}

void blk_kblockd_start(void)
{
	uint32_t ncpus = smp_get_cpu_count(), started = 0;

	if (blk_kblockd_running())
		return;
	if (sched_current() == NULL) {
		WARN(1, BLK_LOG_PREFIX "kblockd started before the scheduler");
		return;
	}
	might_sleep();
	if (!g_kblockd_inited)
		kblockd_init();
	if (ncpus == 0)
		ncpus = 1;
	if (ncpus > MAX_CPUS)
		ncpus = MAX_CPUS;
	for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
		struct kblockd_pool *p = &g_kblockd[cpu];

		if (!percpu_get(cpu))
			continue;
		/* One idle worker per pool to start with, and the rescuer. */
		if (!create_worker(p, 0)) {
			kprintf(BLK_LOG_PREFIX
				"kblockd/%u: no worker; its work goes to CPU 0's pool\n",
				cpu);
			continue;
		}
		if (!create_worker(p, 1))
			kprintf(BLK_LOG_PREFIX
				"kblockd/%u: no rescuer for that pool\n", cpu);
		started++;
	}
	if (!g_kblockd[0].workers) {
		kprintf(BLK_LOG_PREFIX "cannot start kblockd; deferred storage work runs inline\n");
		__atomic_store_n(&g_kblockd_failed, 1, __ATOMIC_RELEASE);
		blk_work_run_pending();
		return;
	}
	/* Items queued from interrupt context before now on pools that got
	 * no worker: hand them to CPU 0's. */
	for (uint32_t cpu = 1; cpu < MAX_CPUS; cpu++) {
		struct kblockd_pool *p = &g_kblockd[cpu];
		uint64_t fl;

		if (p->workers)
			continue;
		for (;;) {
			struct blk_work *w;

			/* Interrupts off from the unlink through the insert:
			 * an item between two lists is invisible to a
			 * PENDING taker, which spins until it lands. */
			fl = local_irq_save();
			spin_lock(&p->lock);
			w = p->worklist.head;
			if (w)
				kblockd_unlink_locked(p, w);
			spin_unlock(&p->lock);
			if (w) {
				w->last_pool = -1;
				kblockd_insert(0, w); /* PENDING stays: owned */
			}
			local_irq_restore(fl);
			if (!w)
				break;
		}
	}
	/* From here on queueing defers.  Whatever waits on a list already is
	 * found there by its pool's first worker. */
	__atomic_store_n(&g_kblockd_running, 1, __ATOMIC_RELEASE);
}
