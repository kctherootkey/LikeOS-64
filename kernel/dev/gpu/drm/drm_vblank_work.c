// LikeOS -- display-manager core: work that runs at a vblank.
//
// A driver schedules a function for a vblank count; when the crtc's counter
// reaches it (drm_handle_vblank_works, called from the vblank path, often an
// interrupt) the work moves to the run list and one kernel thread, the vblank
// worker, runs it in process context.  Until then the work holds a vblank
// reference, so that the counter keeps counting towards it.
//
// One worker for every crtc of every device: works are short and rare (a
// driver uses them for timing-critical register writes that do not fit an
// interrupt handler), and one thread keeps the order in which vblanks
// released them.  The worker is started the first time a work is initialised,
// so a system whose drivers use none has no thread at all.
//
// Locking: g_vbw_lock covers the pending list, the run list and which work
// is running.  It may be taken with the vblank lock's users nested inside
// (drm_crtc_vblank_get/put are called under it), never the other way round:
// the vblank path calls in here only after dropping its own lock.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the DRM vblank work code: MIT
// (the files those portions derive from carry no copyright line)

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_vblank.h>
#include <kernel/dev/gpu/drm_vblank_work.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/timer.h>
#include <kernel/io/console.h>

static spinlock_t g_vbw_lock = SPINLOCK_INIT("drm_vbw");
/* works waiting for their vblank, every crtc */
static LIST_HEAD(g_vbw_pending);
/* works whose vblank came, in the order it did */
static LIST_HEAD(g_vbw_run);
/* the one being run now */
static struct drm_vblank_work *g_vbw_running;
/* the worker sleeps here for something on the run list ... */
static struct wait_queue_head g_vbw_wq = { .first = NULL, .lock = SPINLOCK_INIT("drm_vbw_wq") };
/* ... and flushes and cancels for works to leave the lists */
static struct wait_queue_head g_vbw_done_wq = { .first = NULL,
						.lock = SPINLOCK_INIT("drm_vbw_done") };
/* the worker thread, once started */
static task_t *g_vbw_task;
static int g_vbw_started;
static uint8_t g_vbw_stack[16384] __attribute__((aligned(16)));

/* Sleep on `wq' until `done(arg)' or `slice_ms' has passed.  The condition
 * is checked after joining the queue, so a wake between the check and the
 * sleep is not lost; the slice bounds what a wake that is lost anyway can
 * cost.  Process context only. */
static void vbw_wait(struct wait_queue_head *wq, int (*done)(void *), void *arg,
		     unsigned slice_ms)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	uint64_t fl;

	if (!cur)
		return;
	fl = local_irq_save();
	wq_entry_init(&we, cur);
	wq_add(wq, &we);
	if (done(arg)) {
		local_irq_restore(fl);
		wq_remove(wq, &we);
		return;
	}
	cur->wait_channel = wq;
	cur->wakeup_tick = timer_ticks() + timer_ms_to_ticks(slice_ms) + 1;
	cur->state = TASK_BLOCKED;
	local_irq_restore(fl);
	sched_schedule();
	/* A deadline left armed is claimed by whatever this task waits on
	 * next (see vbl_wait_until in drm_vblank.c): cleared here. */
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(wq, &we);
}

static int vbw_run_ready(void *arg)
{
	uint64_t fl;
	int ready;

	(void)arg;
	spin_lock_irqsave(&g_vbw_lock, &fl);
	ready = !list_empty(&g_vbw_run);
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	return ready;
}

static void vbw_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct drm_vblank_work *w = NULL;
		uint64_t fl;

		spin_lock_irqsave(&g_vbw_lock, &fl);
		if (!list_empty(&g_vbw_run)) {
			w = list_first_entry(&g_vbw_run, struct drm_vblank_work, run_node);
			list_del_init(&w->run_node);
			g_vbw_running = w;
		}
		spin_unlock_irqrestore(&g_vbw_lock, fl);
		if (!w) {
			vbw_wait(&g_vbw_wq, vbw_run_ready, NULL, 100);
			continue;
		}
		w->func(w);
		spin_lock_irqsave(&g_vbw_lock, &fl);
		g_vbw_running = NULL;
		spin_unlock_irqrestore(&g_vbw_lock, fl);
		wq_wake_all(&g_vbw_done_wq);
	}
}

static void vbw_start(void)
{
	if (__atomic_exchange_n(&g_vbw_started, 1, __ATOMIC_ACQ_REL))
		return;
	task_t *t = sched_add_task(vbw_thread, NULL, g_vbw_stack, sizeof(g_vbw_stack));
	if (t) {
		const char *nm = "drm-vblank";
		unsigned i = 0;

		for (; nm[i] && i < sizeof(t->comm) - 1; i++)
			t->comm[i] = nm[i];
		t->comm[i] = '\0';
	} else {
		kprintf("[drm] the vblank worker thread could not be started; vblank works will not run\n");
	}
	__atomic_store_n(&g_vbw_task, t, __ATOMIC_RELEASE);
}

/* Onto the run list unless it is already there.  Caller holds g_vbw_lock. */
static int vbw_queue_run_locked(struct drm_vblank_work *work)
{
	if (!list_empty(&work->run_node))
		return 0;
	list_add_tail(&work->run_node, &g_vbw_run);
	return 1;
}

void drm_vblank_work_init(struct drm_vblank_work *work, struct drm_device *dev,
			  int crtc, drm_vblank_work_func_t func)
{
	work->func = func;
	work->dev = dev;
	work->pipe = crtc;
	work->count = 0;
	work->cancelling = 0;
	INIT_LIST_HEAD(&work->node);
	INIT_LIST_HEAD(&work->run_node);
	vbw_start();
}

void drm_handle_vblank_works(struct drm_device *dev, int crtc)
{
	struct drm_vblank_work *work, *next;
	uint64_t fl, count;
	int nput = 0, queued = 0;

	if (!__atomic_load_n(&g_vbw_started, __ATOMIC_ACQUIRE))
		return;
	spin_lock_irqsave(&g_vbw_lock, &fl);
	/* read under this lock: a work scheduled concurrently either sees
	 * the new count (and runs at once) or is on the list now */
	count = drm_crtc_vblank_count(dev, crtc);
	list_for_each_entry_safe(work, next, &g_vbw_pending, node) {
		if (work->dev != dev || work->pipe != crtc)
			continue;
		if (!drm_vblank_passed(count, work->count))
			continue;
		list_del_init(&work->node);
		nput++;
		queued += vbw_queue_run_locked(work);
	}
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	for (int i = 0; i < nput; i++)
		drm_crtc_vblank_put(dev, crtc);
	if (queued)
		wq_wake_all(&g_vbw_wq);
	if (queued || nput)
		wq_wake_all(&g_vbw_done_wq);
}

void drm_vblank_cancel_pending_works(struct drm_device *dev, int crtc)
{
	struct drm_vblank_work *work, *next;
	uint64_t fl;
	int nput = 0;

	if (!__atomic_load_n(&g_vbw_started, __ATOMIC_ACQUIRE))
		return;
	spin_lock_irqsave(&g_vbw_lock, &fl);
	list_for_each_entry_safe(work, next, &g_vbw_pending, node) {
		if (work->dev != dev || work->pipe != crtc)
			continue;
		list_del_init(&work->node);
		nput++;
	}
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	/* a driver that switches a crtc off with works pending for it has
	 * lost them */
	WARN_ON_ONCE(nput);
	for (int i = 0; i < nput; i++)
		drm_crtc_vblank_put(dev, crtc);
	wq_wake_all(&g_vbw_done_wq);
}

int drm_vblank_work_schedule(struct drm_vblank_work *work, uint64_t count,
			     bool nextonmiss)
{
	struct drm_device *dev = work->dev;
	int pipe = work->pipe;
	uint64_t fl, cur_vbl;
	bool passed, rescheduling = false, wake = false, put = false;
	int ret = 0, queued = 0;

	spin_lock_irqsave(&g_vbw_lock, &fl);
	if (work->cancelling)
		goto out;
	/* the crtc is off: no vblank will come */
	if (__atomic_load_n(&dev->vbl[pipe].inmodeset, __ATOMIC_ACQUIRE))
		goto out;

	if (list_empty(&work->node)) {
		ret = drm_crtc_vblank_get(dev, pipe);
		if (ret < 0)
			goto out;
	} else if (work->count == count) {
		/* already scheduled for that vblank */
		goto out;
	} else {
		rescheduling = true;
	}

	work->count = count;
	cur_vbl = drm_crtc_vblank_count(dev, pipe);
	passed = drm_vblank_passed(cur_vbl, count);

	if (!nextonmiss && passed) {
		/* its vblank has been: straight to the worker */
		put = true;
		queued = vbw_queue_run_locked(work);
		ret = queued;
		if (rescheduling) {
			list_del_init(&work->node);
			wake = true;
		}
	} else {
		/* a passed count with nextonmiss is reached at the next
		 * vblank, whatever it is */
		if (!rescheduling)
			list_add_tail(&work->node, &g_vbw_pending);
		ret = 1;
	}

out:
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	if (put)
		drm_crtc_vblank_put(dev, pipe);
	if (queued)
		wq_wake_all(&g_vbw_wq);
	if (wake)
		wq_wake_all(&g_vbw_done_wq);
	return ret;
}

static int vbw_not_running(void *arg)
{
	struct drm_vblank_work *work = arg;
	uint64_t fl;
	int done;

	spin_lock_irqsave(&g_vbw_lock, &fl);
	done = g_vbw_running != work;
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	return done;
}

/* The worker itself must not wait for the work it is running. */
static int vbw_on_worker(void)
{
	task_t *t = __atomic_load_n(&g_vbw_task, __ATOMIC_ACQUIRE);

	return t && sched_current() == t;
}

bool drm_vblank_work_cancel_sync(struct drm_vblank_work *work)
{
	struct drm_device *dev = work->dev;
	uint64_t fl;
	bool ret = false, put = false;

	spin_lock_irqsave(&g_vbw_lock, &fl);
	if (!list_empty(&work->node)) {
		list_del_init(&work->node);
		put = true;
		ret = true;
	}
	if (!list_empty(&work->run_node)) {
		list_del_init(&work->run_node);
		ret = true;
	}
	work->cancelling++;
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	if (put)
		drm_crtc_vblank_put(dev, work->pipe);
	wq_wake_all(&g_vbw_done_wq);

	/* a run in progress finishes first */
	if (!vbw_on_worker())
		while (!vbw_not_running(work))
			vbw_wait(&g_vbw_done_wq, vbw_not_running, work, 10);

	spin_lock_irqsave(&g_vbw_lock, &fl);
	work->cancelling--;
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	return ret;
}

static int vbw_not_pending(void *arg)
{
	struct drm_vblank_work *work = arg;
	uint64_t fl;
	int done;

	spin_lock_irqsave(&g_vbw_lock, &fl);
	done = list_empty(&work->node);
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	return done;
}

static int vbw_idle(void *arg)
{
	struct drm_vblank_work *work = arg;
	uint64_t fl;
	int done;

	spin_lock_irqsave(&g_vbw_lock, &fl);
	done = list_empty(&work->run_node) && g_vbw_running != work;
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	return done;
}

/* Wait for `done(arg)', for at most `ms': a vblank that never comes must not
 * park the caller for good.  False on time out. */
static bool vbw_wait_capped(int (*done)(void *), void *arg, unsigned ms)
{
	uint64_t deadline = timer_ticks() + timer_ms_to_ticks(ms) + 1;

	while (!done(arg)) {
		if ((int64_t)(timer_ticks() - deadline) >= 0)
			return false;
		vbw_wait(&g_vbw_done_wq, done, arg, 10);
	}
	return true;
}

void drm_vblank_work_flush(struct drm_vblank_work *work)
{
	static int said;

	/* its vblank first ... */
	if (!vbw_wait_capped(vbw_not_pending, work, 3000)) {
		if (said < 4) {
			said++;
			kprintf("[drm] a vblank work on crtc %d waited 3 s for vblank %llu and is still pending\n",
				work->pipe, (unsigned long long)work->count);
		}
		return;
	}
	/* ... then its run */
	if (!vbw_on_worker())
		(void)vbw_wait_capped(vbw_idle, work, 3000);
}

struct vbw_crtc {
	struct drm_device *dev;
	int pipe;
	int pending; /* also count the works still waiting for a vblank */
};

static int vbw_crtc_idle(void *arg)
{
	struct vbw_crtc *c = arg;
	struct drm_vblank_work *work;
	uint64_t fl;
	int done = 1;

	spin_lock_irqsave(&g_vbw_lock, &fl);
	if (c->pending) {
		list_for_each_entry(work, &g_vbw_pending, node)
			if (work->dev == c->dev && work->pipe == c->pipe)
				done = 0;
	}
	list_for_each_entry(work, &g_vbw_run, run_node)
		if (work->dev == c->dev && work->pipe == c->pipe)
			done = 0;
	if (g_vbw_running && g_vbw_running->dev == c->dev && g_vbw_running->pipe == c->pipe)
		done = 0;
	spin_unlock_irqrestore(&g_vbw_lock, fl);
	return done;
}

void drm_vblank_work_flush_all(struct drm_device *dev, int crtc)
{
	struct vbw_crtc c = { dev, crtc, 1 };

	if (!__atomic_load_n(&g_vbw_started, __ATOMIC_ACQUIRE) || vbw_on_worker())
		return;
	(void)vbw_wait_capped(vbw_crtc_idle, &c, 3000);
}

void drm_vblank_flush_worker(struct drm_device *dev, int crtc)
{
	struct vbw_crtc c = { dev, crtc, 0 };

	if (!__atomic_load_n(&g_vbw_started, __ATOMIC_ACQUIRE) || vbw_on_worker())
		return;
	(void)vbw_wait_capped(vbw_crtc_idle, &c, 3000);
}
