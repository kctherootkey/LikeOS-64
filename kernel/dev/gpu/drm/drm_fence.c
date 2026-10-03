// LikeOS -- display-manager fences.
//
// A fence is a point in the GPU's command stream; it signals when the
// device has passed it.  Userspace waits on it by handle (the backend's
// fence ioctls) or as a sync_file descriptor (drm_sync_file.c): poll()
// readable when signalled, mergeable with others into one fence for all.
//
// Inside the kernel a fence can also call back: drm_fence_add_callback()
// runs a function when it signals, which is what fence containers are made
// of -- an array (drm_fence_array.c) signals when its parts have, a chain
// link (drm_fence_chain.c) when its fence and every earlier point have.
// Containers are not on the device's list of live fences; they live by
// their reference count and borrow the lock of a device they wait on.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Canonical's and
// Texas Instruments' code: GPL-2.0-only
// Portions Copyright (C) 2012 Canonical Ltd
// Portions Copyright (C) 2012 Texas Instruments

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_fence_unwrap.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/uapi/bug.h>

/* ---- the callback list ------------------------------------------------- */

static inline void cbl_init(struct drm_fence_cb_node *n)
{
	n->next = n;
	n->prev = n;
}

static inline int cbl_empty(const struct drm_fence_cb_node *n)
{
	return n->next == n;
}

static inline void cbl_add_tail(struct drm_fence_cb_node *n,
				struct drm_fence_cb_node *head)
{
	n->prev = head->prev;
	n->next = head;
	head->prev->next = n;
	head->prev = n;
}

static inline void cbl_del_init(struct drm_fence_cb_node *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	cbl_init(n);
}

/* What every fence starts as; `f' is zeroed by the caller. */
static void fence_init_common(struct drm_fence *f, struct drm_device *dev)
{
	f->refs = 1;
	f->dev = dev;
	wq_head_init(&f->wq, "drm_fence");
	cbl_init(&f->cb_list);
}

/* Mark signalled, under dev->lock.  The time is written before the flag,
 * so whoever sees the flag also sees when. */
static inline void fence_mark_signaled_locked(struct drm_fence *f, uint64_t ts)
{
	f->signal_ns = ts;
	__atomic_store_n(&f->signaled, 1, __ATOMIC_RELEASE);
}

/* Run the callbacks of a fence that has just been marked signalled, by
 * whoever marked it (and holds a reference to it).
 *
 * One at a time, each taken off the list under the lock and called with
 * the lock dropped: a callback drops references (drm_fence_put() takes this
 * lock) and signals other fences (which takes it too) -- under the lock,
 * the first array whose last part signalled would deadlock.  Interrupts
 * stay off around each call, so a callback is never preempted half-way and
 * drm_fence_remove_callback() waiting for it on another processor waits
 * for a few instructions, not for a time slice.
 *
 * The unlocked look at the list first is safe: once the flag is set no
 * callback is added any more (drm_fence_add_callback() checks it under the
 * lock), and a callback being removed concurrently is found or not found
 * under the lock below. */
static void fence_run_callbacks(struct drm_fence *f)
{
	struct drm_device *dev = f->dev;
	uint64_t fl;

	if (cbl_empty(&f->cb_list))
		return;
	spin_lock_irqsave(&dev->lock, &fl);
	while (!cbl_empty(&f->cb_list)) {
		struct drm_fence_cb *cb = (struct drm_fence_cb *)f->cb_list.next;
		drm_fence_func_t fn = cb->func;

		cbl_del_init(&cb->node);
		f->cb_running = cb;
		spin_unlock(&dev->lock); /* interrupts stay disabled */
		fn(f, cb);
		spin_lock(&dev->lock);
		f->cb_running = NULL;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
}

struct drm_fence *drm_fence_create(struct drm_device *dev, uint32_t seqno,
				   uint32_t flags)
{
	struct drm_fence *f = kalloc(sizeof(*f));
	uint64_t fl;

	if (!f)
		return NULL;
	mm_memset(f, 0, sizeof(*f));
	fence_init_common(f, dev);
	f->seqno = seqno;
	f->flags = flags;
	spin_lock_irqsave(&dev->lock, &fl);
	f->next = dev->fences;
	dev->fences = f;
	/* Already passed?  (A fence created for a seqno the device has
	 * been seen to pass, or a signalled placeholder.) */
	if ((int32_t)(dev->fence_passed - seqno) >= 0)
		fence_mark_signaled_locked(f, hrtimer_now_ns());
	spin_unlock_irqrestore(&dev->lock, fl);
	return f;
}

struct drm_fence *drm_fence_create_ctx(struct drm_device *dev,
				       uint64_t context, uint64_t seqno64)
{
	struct drm_fence *f = kalloc(sizeof(*f));
	uint64_t fl;

	if (!f)
		return NULL;
	mm_memset(f, 0, sizeof(*f));
	fence_init_common(f, dev);
	f->context = context;
	f->seqno64 = seqno64;
	/* Out of the device-wide sequence's way: a stream fence is never
	 * "already passed" by that numbering. */
	f->seqno = dev->fence_passed;
	spin_lock_irqsave(&dev->lock, &fl);
	f->next = dev->fences;
	dev->fences = f;
	spin_unlock_irqrestore(&dev->lock, fl);
	return f;
}

/* ---- merged fences ------------------------------------------------------ */

/* How many merged fences are alive anywhere: while there are none, no
 * signalling pays for looking for them. */
static int g_merged_live;

/* Merged fences whose parts have both signalled are signalled, and
 * their waiters woken; a merged fence of merged fences settles over the
 * passes.  Called after anything on the device was signalled. */
static void fence_merged_update(struct drm_device *dev)
{
	if (!dev || !__atomic_load_n(&g_merged_live, __ATOMIC_ACQUIRE))
		return;
	for (int pass = 0; pass < 16; pass++) {
		struct drm_fence *wake[64];
		int nw = 0;
		uint64_t fl;

		spin_lock_irqsave(&dev->lock, &fl);
		for (struct drm_fence *f = dev->fences; f && nw < 64; f = f->next) {
			struct drm_fence *a = f->deps[0], *b = f->deps[1];
			if (f->signaled || !a)
				continue;
			if (!a->signaled || (b && !b->signaled))
				continue;
			f->error = a->error ? a->error : (b ? b->error : 0);
			fence_mark_signaled_locked(f, hrtimer_now_ns());
			drm_fence_get(f);
			wake[nw++] = f;
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		for (int i = 0; i < nw; i++) {
			poll_notify_wq(&wake[i]->wq);
			fence_run_callbacks(wake[i]);
			drm_fence_put(wake[i]);
		}
		if (nw == 0)
			break;
	}
}

/* A fence that signals once both `a' and `b' have: one of them when the
 * other has signalled already, else a new fence that holds both. */
struct drm_fence *drm_fence_merge(struct drm_fence *a, struct drm_fence *b)
{
	struct drm_device *dev = a->dev;
	uint64_t fl;

	/* A container among the parts must be watching its own parts, or
	 * nothing would ever mark it signalled for the walk below to see.
	 * (Nothing to do for a device's own fences.) */
	drm_fence_enable_signaling(a);
	drm_fence_enable_signaling(b);
	if (a == b || (b->signaled && !b->error)) {
		drm_fence_get(a);
		return a;
	}
	if (a->signaled && !a->error) {
		drm_fence_get(b);
		return b;
	}
	struct drm_fence *m = kalloc(sizeof(*m));
	if (!m)
		return NULL;
	mm_memset(m, 0, sizeof(*m));
	fence_init_common(m, dev);
	m->flags = a->flags | b->flags;
	/* no stream of its own: nothing but its parts signal it */
	m->context = 0;
	m->seqno = dev->fence_passed;
	drm_fence_get(a);
	drm_fence_get(b);
	m->deps[0] = a;
	m->deps[1] = b;
	__atomic_fetch_add(&g_merged_live, 1, __ATOMIC_ACQ_REL);
	spin_lock_irqsave(&dev->lock, &fl);
	m->next = dev->fences;
	dev->fences = m;
	spin_unlock_irqrestore(&dev->lock, fl);
	/* both may have signalled meanwhile */
	fence_merged_update(dev);
	return m;
}

void drm_fence_signal_upto_ctx(struct drm_device *dev, uint64_t context,
			       uint64_t passed)
{
	uint64_t fl;
	struct drm_fence *wake[64];
	int nw = 0;

	spin_lock_irqsave(&dev->lock, &fl);
	/* In batches, so that none is signalled without being woken: see
	 * drm_fence_signal_upto(). */
	for (;;) {
		nw = 0;
		for (struct drm_fence *f = dev->fences; f && nw < 64;
		     f = f->next) {
			if (!f->signaled && f->context == context &&
			    f->seqno64 <= passed) {
				fence_mark_signaled_locked(f, hrtimer_now_ns());
				drm_fence_get(f);
				wake[nw++] = f;
			}
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		for (int i = 0; i < nw; i++) {
			poll_notify_wq(&wake[i]->wq);
			fence_run_callbacks(wake[i]);
			drm_fence_put(wake[i]);
		}
		if (nw < 64)
			break;
		spin_lock_irqsave(&dev->lock, &fl);
	}
	fence_merged_update(dev);
	poll_notify_wq(&dev->vbl_wq);
}

struct drm_fence *drm_fence_signalled(struct drm_device *dev)
{
	struct drm_fence *f = drm_fence_create(dev, dev->fence_passed, 0);
	if (f) {
		f->signaled = 1;
		f->signal_ns = hrtimer_now_ns();
	}
	return f;
}

void drm_fence_get(struct drm_fence *f)
{
	__atomic_fetch_add(&f->refs, 1, __ATOMIC_ACQ_REL);
}

/* The end of a fence nobody refers to any more.  A callback still queued
 * on it means somebody added one without holding a reference, as the
 * contract asks: say so, since that somebody is about to be called never. */
static void fence_release(struct drm_fence *f)
{
	WARN_ON(!cbl_empty(&f->cb_list));
	if (f->ops && f->ops->release)
		f->ops->release(f);
	else
		kfree(f);
}

/* The last reference is dropped UNDER dev->lock, not before taking it.
 *
 * A fence is on dev->fences from the moment it is created, and the list is
 * walked by drm_fence_signal_upto() -- from the fence interrupt and from the
 * poll timer -- which takes a reference to every fence it is about to
 * signal.  Decrementing outside the lock leaves a window where the count is
 * already zero and the fence is still on the list: the walker finds it,
 * takes it from 0 to 1, signals it, drops it back to 0 and frees it, and
 * then this function frees it a second time.  That is what
 *
 *   SLAB: Double free at ... (caller=drm_fence_put ... size=128)
 *
 * was, and it became constant once the console started emitting a fence per
 * screen update -- every line of output was a chance to hit it.
 *
 * With the decrement inside the lock the two orders are the only ones
 * possible: the walker's get happens first, this decrement does not reach
 * zero, and the walker frees it; or the unlink happens first and the walker
 * never sees it. */
void drm_fence_put(struct drm_fence *f)
{
	if (!f)
		return;
	struct drm_device *dev = f->dev;
	uint64_t fl;

	/* A fence on no list (a container, a private stub) cannot be found
	 * by a walker with its count at zero, so the count alone decides --
	 * no lock, which also lets a callback drop the last reference to
	 * the container it belongs to. */
	if (__atomic_load_n(&f->sflags, __ATOMIC_ACQUIRE) & DRM_FENCE_SF_UNLISTED) {
		if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0)
			fence_release(f);
		return;
	}
	spin_lock_irqsave(&dev->lock, &fl);
	if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) != 0) {
		spin_unlock_irqrestore(&dev->lock, fl);
		return;
	}
	struct drm_fence **pp = &dev->fences;
	while (*pp) {
		if (*pp == f) {
			*pp = f->next;
			break;
		}
		pp = &(*pp)->next;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	if (f->deps[0]) {
		/* a merged fence lets go of its parts, outside the lock
		 * (dropping them takes it) */
		__atomic_fetch_sub(&g_merged_live, 1, __ATOMIC_ACQ_REL);
		drm_fence_put(f->deps[0]);
		drm_fence_put(f->deps[1]);
	}
	fence_release(f);
}

/* Signal one fence at `ts': -EINVAL when it had signalled already.  The
 * caller holds a reference.  The flag is set under the lock, so exactly
 * one signaller gets to run the callbacks. */
static int fence_signal_ts(struct drm_fence *f, uint64_t ts)
{
	struct drm_device *dev = f->dev;
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	if (f->signaled) {
		spin_unlock_irqrestore(&dev->lock, fl);
		return -EINVAL;
	}
	fence_mark_signaled_locked(f, ts);
	spin_unlock_irqrestore(&dev->lock, fl);
	poll_notify_wq(&f->wq);
	fence_run_callbacks(f);
	fence_merged_update(dev);
	/* whoever waits for "anything on the device" (a buffer's poll) */
	poll_notify_wq(&dev->vbl_wq);
	return 0;
}

void drm_fence_signal(struct drm_fence *f)
{
	if (f->signaled)
		return;
	fence_signal_ts(f, hrtimer_now_ns());
}

int drm_fence_signal_timestamp(struct drm_fence *f, uint64_t timestamp_ns)
{
	if (WARN_ON(!f))
		return -EINVAL;
	return fence_signal_ts(f, timestamp_ns);
}

void drm_fence_signal_upto(struct drm_device *dev, uint32_t passed)
{
	uint64_t fl;
	struct drm_fence *wake[64];
	int nw = 0;

	spin_lock_irqsave(&dev->lock, &fl);
	/* Nothing new: every fence this number covers is already signalled,
	 * so there is nothing to walk and nobody to wake.
	 *
	 * This is the common case by far -- the device is polled several
	 * hundred times a second and most of those find the same number as
	 * last time -- and the walk it skips is over every LIVE fence, of
	 * which a browser holds one per object it has not yet re-submitted.
	 * Walking that list, under this lock, with interrupts off, on every
	 * poll, is where the driver's own time was going.
	 *
	 * Safe because the invariant holds on the other side: a fence whose
	 * sequence has already passed is marked signalled by
	 * drm_fence_create() under this same lock, so an unsignalled fence
	 * always has a sequence ahead of `fence_passed'. */
	if ((int32_t)(passed - dev->fence_passed) <= 0) {
		spin_unlock_irqrestore(&dev->lock, fl);
		return;
	}
	dev->fence_passed = passed;
	/* In batches of what `wake' holds, until a pass finds no more.
	 *
	 * A fence used to be marked signalled whether or not there was room
	 * left to wake it, so the 65th and later of one pass were signalled
	 * in silence -- and the list is newest first, so the ones dropped
	 * were the OLDEST, the ones somebody had been waiting on longest.  A
	 * waiter inside the kernel polls and recovers; a poll() on a fence
	 * descriptor does not, since it sleeps until it is told (there is no
	 * per-tick rescan any more).  A browser's UI process waits for each
	 * frame exactly that way, so one burst of completions -- the device
	 * coming back from a long operation with everything queued behind it
	 * done at once -- left a frame that was never shown and a web view
	 * that never painted again.  Marking only what can be woken, and
	 * going round again for the rest, wakes every one. */
	for (;;) {
		nw = 0;
		for (struct drm_fence *f = dev->fences; f && nw < 64;
		     f = f->next) {
			/* the device-wide sequence only: a stream's fence
			 * and a merged one are signalled by their own */
			if (!f->signaled && !f->context && !f->deps[0] &&
			    (int32_t)(passed - f->seqno) >= 0) {
				fence_mark_signaled_locked(f, hrtimer_now_ns());
				drm_fence_get(f);
				wake[nw++] = f;
			}
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		for (int i = 0; i < nw; i++) {
			poll_notify_wq(&wake[i]->wq);
			fence_run_callbacks(wake[i]);
			drm_fence_put(wake[i]);
		}
		if (nw < 64)
			break;
		spin_lock_irqsave(&dev->lock, &fl);
	}
	fence_merged_update(dev);
	poll_notify_wq(&dev->vbl_wq); /* SYNCCPU / execbuf throttles */
}

/* How long to keep asking the device before giving the processor up.
 *
 * Host fences retire in tens of microseconds.  Sleeping for one instead costs
 * a whole timer tick -- 10ms at the default rate, and the wait used to ask for
 * two of them -- because that is the granularity of the only wake available
 * when the device has no fence interrupt.  Trading a bounded spin for that is
 * what keeps a per-frame wait from quantising the frame rate: a buffer map on
 * the display path waits on the fence of the batch that drew it, so one
 * unnecessary sleep per frame is the difference between 60fps and 50, and
 * several is the difference between usable and not. */
#define DRM_FENCE_SPIN_NS 200000ULL /* 200us */

/* Where the sleep starts, and how far it backs off.  Short enough that a
 * batch of a millisecond or two is not rounded up to a tick, long enough that
 * a wait of seconds does not spend itself waking up. */
#define DRM_FENCE_POLL_NS 250000ULL /* 250us */
#define DRM_FENCE_POLL_MAX_NS 500000ULL /* 500us */

/* Wake the sleeper when its poll deadline passes.  Runs from the timer, so it
 * claims the task the way every other waker must: a claim without an enqueue
 * strands it. */
static void fence_poll_wake(hrtimer_t *t)
{
	task_t *task = t->arg;

	if (sched_claim_wake(task, TASK_BLOCKED)) {
		task->wait_channel = NULL;
		sched_enqueue_ready(task);
	}
}

/* A container's parts may be on any device: ask the device of each part
 * still pending, once per run of parts on the same device and for at most
 * a few devices, then let the container look at its parts. */
static void fence_poll_parts(struct drm_fence *f)
{
	struct drm_fence_unwrap it;
	struct drm_fence *p;
	struct drm_device *last = NULL;
	int asked = 0;

	drm_fence_unwrap_for_each(p, &it, f) {
		if (p->signaled || p->dev == last)
			continue;
		last = p->dev;
		if (last && last->drv && last->drv->fence_poll)
			last->drv->fence_poll(last);
		if (++asked >= 4) {
			drm_fence_unwrap_end(&it);
			break;
		}
	}
	drm_fence_is_signaled(f);
}

static void drm_fence_poll(struct drm_fence *f)
{
	struct drm_device *dev = f->dev;

	if (f->ops) {
		if (!f->signaled)
			fence_poll_parts(f);
		return;
	}
	if (!f->signaled && dev && dev->drv && dev->drv->fence_poll)
		dev->drv->fence_poll(dev);
}

/* What waiting has cost, for whoever reports frame accounting.  Relaxed
 * atomics: a lost sample would skew a diagnostic and nothing else, and this
 * sits on the path whose cost is being measured. */

/* A wait still open after this long is said, with what the driver can
 * tell about the work behind the fence. */
#define DRM_FENCE_STUCK_NS (5ULL * 1000000000ULL)

void drm_fence_report_stuck(struct drm_fence *f, uint64_t waited_ns, const char *how)
{
	static uint32_t said;
	task_t *cur = sched_current();
	struct drm_device *dev = f->dev;

	if (said >= 12)
		return;
	said++;
	kprintf("[drm] %s: process %d (%s) has waited %u s in %s for a fence (stream %llx, seqno %llu%s), still not signalled\n",
		dev && dev->drv ? dev->drv->name : "drm", cur ? (int)cur->tgid : -1,
		cur ? cur->comm : "?", (unsigned)(waited_ns / 1000000000ULL), how,
		(unsigned long long)f->context,
		(unsigned long long)(f->context ? f->seqno64 : f->seqno),
		f->deps[0] ? ", merged" : "");
	if (f->deps[0]) {
		struct drm_fence *part = !f->deps[0]->signaled ? f->deps[0] : f->deps[1];
		if (part && !part->signaled)
			f = part;
	}
	if (f->ops) {
		/* a container: the driver can only speak for one of its
		 * parts -- the first still pending */
		struct drm_fence_unwrap it;
		struct drm_fence *p, *leaf = NULL;

		drm_fence_unwrap_for_each(p, &it, f) {
			if (!p->signaled) {
				drm_fence_get(p);
				leaf = p;
				drm_fence_unwrap_end(&it);
				break;
			}
		}
		if (!leaf)
			return;
		dev = leaf->dev;
		if (dev && dev->drv && dev->drv->fence_stuck)
			dev->drv->fence_stuck(dev, leaf);
		drm_fence_put(leaf);
		return;
	}
	if (dev && dev->drv && dev->drv->fence_stuck)
		dev->drv->fence_stuck(dev, f);
}

static int fence_wait_do(struct drm_fence *f, uint64_t timeout_ns, int intr)
{
	task_t *cur = sched_current();
	uint64_t start = hrtimer_now_ns();
	uint64_t deadline = start + timeout_ns;
	uint64_t spin_until = start + DRM_FENCE_SPIN_NS;
	uint64_t poll_ns = DRM_FENCE_POLL_NS;
	int stuck_said = 0;

	/* Before anything is decided: the fence may already have passed and
	 * nobody have noticed. */
	drm_fence_poll(f);

	while (!f->signaled) {
		/* Resumable, not failed.  Nothing has been consumed and no
		 * state has moved, so running the call again is exactly
		 * equivalent to never having been interrupted -- which is what
		 * a handler installed with SA_RESTART asked for.  The syscall
		 * return path decides; see ERESTARTSYS. */
		if (intr && signal_pending(cur))
			return -ERESTARTSYS;
		uint64_t now = hrtimer_now_ns();
		if (now >= deadline)
			return -ETIMEDOUT;
		if (!stuck_said && now - start >= DRM_FENCE_STUCK_NS) {
			stuck_said = 1;
			drm_fence_report_stuck(f, now - start, "a fence wait");
		}
		if (now < spin_until) {
			/* Still inside the window where asking is cheaper than
			 * sleeping. */
			for (int i = 0; i < 64; i++)
				__asm__ volatile("pause" ::: "memory");
			drm_fence_poll(f);
			continue;
		}
		struct wait_queue_entry we;
		hrtimer_t poll_timer;
		int highres = hrtimer_is_highres();
		uint64_t fl;

		if (highres)
			hrtimer_init(&poll_timer, fence_poll_wake, cur);
		fl = local_irq_save();

		wq_entry_init(&we, cur);
		wq_add(&f->wq, &we);
		if (f->signaled) {
			local_irq_restore(fl);
			wq_remove(&f->wq, &we);
			break;
		}
		cur->wait_channel = f;
		/* Sleep, and ask again on waking: the fence interrupt is not
		 * available on every host, and where it is missing this is the
		 * only thing that makes progress.
		 *
		 * The deadline comes from the high-resolution timer where there
		 * is one, because the periodic tick is 10ms and a batch that
		 * takes one or two milliseconds is the ordinary case -- sleeping
		 * a whole tick for it quantises the frame rate to the tick.
		 * (The old code asked for `timer_ms_to_ticks(4) + 1', which
		 * rounds 4ms up to one tick and then adds another: a wait the
		 * comment described as a few milliseconds slept for twenty.)
		 * The interval backs off so a genuinely long wait does not
		 * spend the whole time waking up. */
		if (highres) {
			hrtimer_start(&poll_timer, now + poll_ns);
			if (poll_ns < DRM_FENCE_POLL_MAX_NS)
				poll_ns *= 2;
		} else {
			cur->wakeup_tick = timer_ticks() + 1;
		}
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		if (highres)
			hrtimer_cancel(&poll_timer);
		/* Disarm: this deadline belongs to this poll and to nothing
		 * after it.  Left set, it is claimed on the next tick of
		 * whatever this task waits on next -- see the note above
		 * sched_claim_wake(). */
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		wq_remove(&f->wq, &we);
		/* Woken by the timer rather than by a signalling: nothing has
		 * looked at the device, so look now. */
		drm_fence_poll(f);
	}
	return 0;
}

int drm_fence_wait_flags(struct drm_fence *f, uint64_t timeout_ns, int intr)
{
	if (f->signaled)
		return 0;
	/* A container is signalled by its parts' callbacks, once it has
	 * been asked to watch them; the wait below then sleeps on its queue
	 * exactly as on a device's fence. */
	if (f->ops)
		drm_fence_enable_signaling(f);
	return fence_wait_do(f, timeout_ns, intr);
}

/* ---- callbacks, status, deadlines -------------------------------------- */

void drm_fence_enable_signaling(struct drm_fence *f)
{
	if (!f || !f->ops || !f->ops->enable_signaling)
		return;
	/* Once: whoever sets the bit does the arming. */
	if (__atomic_fetch_or(&f->sflags, DRM_FENCE_SF_ENABLE_SIGNAL,
			      __ATOMIC_ACQ_REL) & DRM_FENCE_SF_ENABLE_SIGNAL)
		return;
	if (f->signaled)
		return;
	/* Without the lock: arming adds callbacks to the parts, and the
	 * parts may share this fence's lock (one device's lock for all its
	 * fences). */
	if (!f->ops->enable_signaling(f))
		fence_signal_ts(f, hrtimer_now_ns());
}

int drm_fence_add_callback(struct drm_fence *f, struct drm_fence_cb *cb,
			   drm_fence_func_t func)
{
	uint64_t fl;
	int ret = 0;

	if (WARN_ON(!f || !cb || !func))
		return -EINVAL;
	if (f->signaled) {
		cbl_init(&cb->node);
		return -ENOENT;
	}
	drm_fence_enable_signaling(f);
	spin_lock_irqsave(&f->dev->lock, &fl);
	if (f->signaled) {
		cbl_init(&cb->node);
		ret = -ENOENT;
	} else {
		cb->func = func;
		cbl_add_tail(&cb->node, &f->cb_list);
	}
	spin_unlock_irqrestore(&f->dev->lock, fl);
	return ret;
}

bool drm_fence_remove_callback(struct drm_fence *f, struct drm_fence_cb *cb)
{
	uint64_t fl;

	spin_lock_irqsave(&f->dev->lock, &fl);
	if (!cbl_empty(&cb->node)) {
		cbl_del_init(&cb->node);
		spin_unlock_irqrestore(&f->dev->lock, fl);
		return true;
	}
	/* Off the list because it is being run: the caller is about to free
	 * or reuse it, so not before the runner is done with it.  The runner
	 * has interrupts off, so this is a matter of instructions. */
	while (f->cb_running == cb) {
		spin_unlock_irqrestore(&f->dev->lock, fl);
		for (int i = 0; i < 64; i++)
			__asm__ volatile("pause" ::: "memory");
		spin_lock_irqsave(&f->dev->lock, &fl);
	}
	spin_unlock_irqrestore(&f->dev->lock, fl);
	return false;
}

bool drm_fence_is_signaled(struct drm_fence *f)
{
	if (f->signaled)
		return true;
	if (f->ops && f->ops->signaled && f->ops->signaled(f)) {
		fence_signal_ts(f, hrtimer_now_ns());
		return true;
	}
	return false;
}

bool drm_fence_poll_signaled(struct drm_fence *f)
{
	if (f->signaled)
		return true;
	drm_fence_poll(f);
	return drm_fence_is_signaled(f);
}

int drm_fence_get_status(struct drm_fence *f)
{
	if (!drm_fence_is_signaled(f))
		return 0;
	return f->error < 0 ? f->error : 1;
}

void drm_fence_set_error(struct drm_fence *f, int error)
{
	WARN_ON(f->signaled);
	WARN_ON(error >= 0 || error < -4095);
	f->error = error;
}

void drm_fence_set_deadline(struct drm_fence *f, uint64_t deadline_ns)
{
	struct drm_device *dev = f->dev;

	if (drm_fence_is_signaled(f))
		return;
	if (f->ops) {
		if (f->ops->set_deadline)
			f->ops->set_deadline(f, deadline_ns);
		return;
	}
	if (f->deps[0]) {
		/* a merged fence: both halves are what is waited for */
		drm_fence_set_deadline(f->deps[0], deadline_ns);
		if (f->deps[1])
			drm_fence_set_deadline(f->deps[1], deadline_ns);
		return;
	}
	if (dev && dev->drv && dev->drv->fence_set_deadline)
		dev->drv->fence_set_deadline(dev, f, deadline_ns);
}

/* Stream numbers for fences that are not an engine's.  Engines number
 * their own streams from small values; these start far above them. */
static uint64_t g_fence_context_next = 1ULL << 32;

uint64_t drm_fence_context_alloc(unsigned num)
{
	WARN_ON(!num);
	return __atomic_fetch_add(&g_fence_context_next, num ? num : 1,
				  __ATOMIC_RELAXED);
}

/* Timelines derived from an address are tagged in the top bits, where no
 * allocated stream number reaches: bit 63 alone for a merged fence (a
 * timeline of its own), bits 63 and 62 for a device's device-wide
 * sequence. */
#define FENCE_TL_ADDR_MASK 0x0000FFFFFFFFFFFFULL
#define FENCE_TL_OWN (1ULL << 63)
#define FENCE_TL_DEVICE ((1ULL << 63) | (1ULL << 62))

uint64_t drm_fence_timeline(const struct drm_fence *f)
{
	if (f->context)
		return f->context;
	if (f->deps[0])
		return FENCE_TL_OWN | ((uint64_t)(uintptr_t)f & FENCE_TL_ADDR_MASK);
	return FENCE_TL_DEVICE | ((uint64_t)(uintptr_t)f->dev & FENCE_TL_ADDR_MASK);
}

/* On a stream the position is 64 bits and only grows; the device-wide
 * sequence is 32 bits and compared the way drm_fence_signal_upto()
 * compares it, across the wrap. */
bool drm_fence_is_later(const struct drm_fence *a, const struct drm_fence *b)
{
	if (a->context)
		return a->seqno64 > b->seqno64;
	if (a->deps[0])
		return false;
	return (int32_t)(a->seqno - b->seqno) > 0;
}

bool drm_fence_is_later_or_same(const struct drm_fence *a, const struct drm_fence *b)
{
	return a == b || !drm_fence_is_later(b, a);
}

const char *drm_fence_driver_name(struct drm_fence *f)
{
	if (f->ops && f->ops->get_driver_name)
		return f->ops->get_driver_name(f);
	if (f->dev && f->dev->drv && f->dev->drv->name)
		return f->dev->drv->name;
	return "drm";
}

void drm_fence_timeline_name(struct drm_fence *f, char *buf, unsigned len)
{
	if (!len)
		return;
	if (f->ops && f->ops->get_timeline_name)
		ksnprintf(buf, len, "%s", f->ops->get_timeline_name(f));
	else if (f->name[0])
		ksnprintf(buf, len, "%s", f->name);
	else if (f->deps[0])
		ksnprintf(buf, len, "merged");
	else if (f->context)
		ksnprintf(buf, len, "stream-%llx", (unsigned long long)f->context);
	else
		ksnprintf(buf, len, "%s", drm_fence_driver_name(f));
}

void drm_fence_init_unlisted(struct drm_fence *f, struct drm_device *dev,
			     const struct drm_fence_ops *ops, uint64_t context,
			     uint64_t seqno64)
{
	fence_init_common(f, dev);
	f->ops = ops;
	f->context = context;
	f->seqno64 = seqno64;
	f->seqno = (uint32_t)seqno64;
	f->sflags = DRM_FENCE_SF_UNLISTED;
}

static const char *fence_stub_name(struct drm_fence *f)
{
	(void)f;
	return "stub";
}

static const struct drm_fence_ops fence_stub_ops = {
	.get_driver_name = fence_stub_name,
	.get_timeline_name = fence_stub_name,
};

struct drm_fence *drm_fence_signalled_at(struct drm_device *dev,
					 uint64_t timestamp_ns)
{
	struct drm_fence *f = kalloc(sizeof(*f));

	if (!f)
		return NULL;
	mm_memset(f, 0, sizeof(*f));
	drm_fence_init_unlisted(f, dev, &fence_stub_ops, 0, 0);
	fence_mark_signaled_locked(f, timestamp_ns); /* nobody else sees it yet */
	return f;
}

/* ---- waiting for any of several ---------------------------------------- */

static int fence_any_signaled(struct drm_fence **fences, uint32_t count,
			      uint32_t *idx, int poll)
{
	for (uint32_t i = 0; i < count; i++) {
		if (poll ? drm_fence_poll_signaled(fences[i]) :
			   drm_fence_is_signaled(fences[i])) {
			if (idx)
				*idx = i;
			return 1;
		}
	}
	return 0;
}

/* The shape of fence_wait_do(), over several fences: a short spin asking
 * the devices, then sleeps on every fence's queue at once with a backing-
 * off poll deadline, since not every device interrupts. */
int drm_fence_wait_any_timeout(struct drm_fence **fences, uint32_t count,
			       int intr, uint64_t timeout_ns, uint32_t *idx)
{
	task_t *cur = sched_current();
	struct wait_queue_entry *we;
	uint64_t start, deadline, spin_until, poll_ns = DRM_FENCE_POLL_NS;
	int stuck_said = 0, ret = 0;

	if (WARN_ON(!fences || !count))
		return -EINVAL;
	if (fence_any_signaled(fences, count, idx, 0))
		return 0;
	if (!timeout_ns)
		return fence_any_signaled(fences, count, idx, 1) ? 0 : -ETIMEDOUT;
	for (uint32_t i = 0; i < count; i++)
		drm_fence_enable_signaling(fences[i]);
	we = kcalloc(count, sizeof(*we));
	if (!we)
		return -ENOMEM;
	start = hrtimer_now_ns();
	deadline = start + timeout_ns;
	spin_until = start + DRM_FENCE_SPIN_NS;
	for (;;) {
		if (fence_any_signaled(fences, count, idx, 1))
			break;
		if (intr && signal_pending(cur)) {
			ret = -ERESTARTSYS;
			break;
		}
		uint64_t now = hrtimer_now_ns();
		if (now >= deadline) {
			ret = -ETIMEDOUT;
			break;
		}
		if (!stuck_said && now - start >= DRM_FENCE_STUCK_NS) {
			stuck_said = 1;
			drm_fence_report_stuck(fences[0], now - start, "a wait for any fence");
		}
		if (now < spin_until) {
			for (int i = 0; i < 64; i++)
				__asm__ volatile("pause" ::: "memory");
			continue;
		}
		hrtimer_t poll_timer;
		int highres = hrtimer_is_highres();
		int done = 0;
		uint64_t fl;

		if (highres)
			hrtimer_init(&poll_timer, fence_poll_wake, cur);
		fl = local_irq_save();
		for (uint32_t i = 0; i < count; i++) {
			wq_entry_init(&we[i], cur);
			wq_add(&fences[i]->wq, &we[i]);
		}
		for (uint32_t i = 0; i < count && !done; i++)
			if (fences[i]->signaled)
				done = 1;
		if (done) {
			local_irq_restore(fl);
			for (uint32_t i = 0; i < count; i++)
				wq_remove(&fences[i]->wq, &we[i]);
			continue;
		}
		cur->wait_channel = fences[0];
		if (highres) {
			hrtimer_start(&poll_timer, now + poll_ns);
			if (poll_ns < DRM_FENCE_POLL_MAX_NS)
				poll_ns *= 2;
		} else {
			cur->wakeup_tick = timer_ticks() + 1;
		}
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		if (highres)
			hrtimer_cancel(&poll_timer);
		/* see fence_wait_do(): this deadline belongs to this poll */
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		for (uint32_t i = 0; i < count; i++)
			wq_remove(&fences[i]->wq, &we[i]);
	}
	kfree(we);
	return ret;
}

/* ---- per-file fence handles ------------------------------------------- */

int drm_fence_handle_create(struct drm_file *fp, struct drm_fence *f,
			    uint32_t *handle_out)
{
	uint64_t fl;

	spin_lock_irqsave(&fp->lock, &fl);
	for (uint32_t h = 1; h < fp->nfences; h++) {
		if (!fp->fences[h]) {
			drm_fence_get(f);
			fp->fences[h] = f;
			spin_unlock_irqrestore(&fp->lock, fl);
			*handle_out = h;
			return 0;
		}
	}
	uint32_t ncap = fp->nfences ? fp->nfences * 2 : 64;
	spin_unlock_irqrestore(&fp->lock, fl);
	if (ncap > 4096)
		return -ENOSPC;
	struct drm_fence **nt = kalloc(ncap * sizeof(*nt));
	if (!nt)
		return -ENOMEM;
	mm_memset(nt, 0, ncap * sizeof(*nt));
	spin_lock_irqsave(&fp->lock, &fl);
	if (fp->fences) {
		mm_memcpy(nt, fp->fences, fp->nfences * sizeof(*nt));
		kfree(fp->fences);
	}
	uint32_t h = fp->nfences ? fp->nfences : 1;
	fp->fences = nt;
	fp->nfences = ncap;
	drm_fence_get(f);
	fp->fences[h] = f;
	spin_unlock_irqrestore(&fp->lock, fl);
	*handle_out = h;
	return 0;
}

struct drm_fence *drm_fence_handle_lookup(struct drm_file *fp, uint32_t handle)
{
	uint64_t fl;
	struct drm_fence *f = NULL;

	spin_lock_irqsave(&fp->lock, &fl);
	if (handle && handle < fp->nfences && fp->fences[handle]) {
		f = fp->fences[handle];
		drm_fence_get(f);
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	return f;
}

int drm_fence_handle_delete(struct drm_file *fp, uint32_t handle)
{
	uint64_t fl;
	struct drm_fence *f = NULL;

	spin_lock_irqsave(&fp->lock, &fl);
	if (handle && handle < fp->nfences && fp->fences[handle]) {
		f = fp->fences[handle];
		fp->fences[handle] = NULL;
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	if (!f)
		return -EINVAL;
	drm_fence_put(f);
	return 0;
}

void drm_fence_handles_release(struct drm_file *fp)
{
	for (uint32_t h = 1; h < fp->nfences; h++) {
		if (fp->fences[h]) {
			struct drm_fence *f = fp->fences[h];
			fp->fences[h] = NULL;
			drm_fence_put(f);
		}
	}
	if (fp->fences)
		kfree(fp->fences);
	fp->fences = NULL;
	fp->nfences = 0;
}
