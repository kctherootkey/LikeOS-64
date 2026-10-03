// LikeOS -- synchronisation objects.
//
// A syncobj is a handle a renderer passes between its submissions: a
// container for a fence pointer that may be empty.  It can be created
// empty, filled later by a submission, waited for before it is filled
// (WAIT_FOR_SUBMIT), shared as a descriptor, and turned into or made from a
// sync_file.  Drivers attach their submission fences through
// drm_syncobj_add_point() and read the fences they must wait for through
// drm_syncobj_find_fence().
//
// Binary and timeline objects are the same thing.  A binary object's fence
// is replaced by each signal operation.  A timeline object's fence is the
// newest link of a fence chain (drm_fence_chain.h): attaching point N puts
// a new link for N in front of the old fence, and "the fence for point N"
// is found by walking back from the newest link.  The chain cuts links
// whose fences have signalled as it is walked, so a timeline keeps only
// the points that are still pending, however many have been submitted.
//
// Waiting is driven by callbacks.  A waiter registers on each syncobj whose
// point has nothing standing for it yet (the object's wait list, run
// whenever its fence changes) and on each fence it waits for (a fence
// callback); either wakes it.  Not every device signals its fences on its
// own -- some only notice completed work when asked -- so a sleeping
// waiter also wakes on a short, backing-off timer and asks the devices of
// the fences it waits for (drv->fence_poll) before looking again.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Red Hat's and
// Advanced Micro Devices' code: MIT
// Portions Copyright 2017 Red Hat
// Portions Copyright 2016 Advanced Micro Devices, Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_fence_chain.h>
#include <kernel/dev/gpu/drm_fence_unwrap.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

/* DRM_IOCTL_SYNCOBJ_EVENTFD posts to a client's eventfd through the
 * eventfd code's in-kernel entry points (<kernel/fs/eventfd.h>): take a
 * reference to the counter behind a descriptor, add one to it from any
 * context, drop the reference.  0 makes the ioctl answer -EOPNOTSUPP. */
#ifndef DRM_SYNCOBJ_EVENTFD
#define DRM_SYNCOBJ_EVENTFD 1
#endif

#define SYNCOBJ_MAX_HANDLES 65536 /* per file: one per submission while its fence lives */
#define SYNCOBJ_MAX_ARRAY 8192 /* handles per ioctl */
/* The sleeping waiter's look at the devices: first after this long,
 * then backing off to the maximum. */
#define SYNCOBJ_POLL_NS 250000ULL
#define SYNCOBJ_POLL_MAX_NS 2000000ULL
/* How long a kernel-side WAIT_FOR_SUBMIT lookup waits for a fence. */
#define DRM_SYNCOBJ_WAIT_FOR_SUBMIT_TIMEOUT 5000000000ULL
/* A wait still open after this long is reported. */
#define SYNCOBJ_STUCK_NS (5ULL * 1000000000ULL)

/* The timeline flags of HANDLE_TO_FD / FD_TO_HANDLE and the argument as
 * current clients pass it: the original 16 bytes followed by the point
 * (the ioctl layer zero-fills what a shorter argument leaves out). */
#ifndef DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE (1 << 1)
#endif
#ifndef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE (1 << 1)
#endif

struct syncobj_handle_args {
	uint32_t handle;
	uint32_t flags;
	int32_t fd;
	uint32_t pad;
	uint64_t point;
};

#if DRM_SYNCOBJ_EVENTFD
#include <kernel/fs/eventfd.h>
#endif

struct drm_syncobj {
	int refs;
	struct drm_device *dev; /* the device it was made on */
	/* NULL, a plain fence or the newest link of a timeline; written
	 * under `lock', referenced. */
	struct drm_fence *fence;
	/* Waiters for a point that had nothing standing for it yet, run
	 * whenever `fence' changes (struct syncobj_wait_entry). */
	struct list_head cb_list;
	/* Registered eventfds whose point has not appeared yet. */
	struct list_head ev_fd_list;
	/* Guards the three above; never held across a sleep. */
	spinlock_t lock;
};

/* ---- waking a waiter ------------------------------------------------------ */

/* One sleeping task, woken by fence callbacks, by a syncobj's wait list and
 * by its own poll timer -- possibly on other processors, possibly while it
 * is still deciding to sleep.  `woken' is set under `lock' by every waker
 * before it wakes the queue, and the sleeper looks at it under the same
 * lock before it marks itself blocked, so no wake falls between the two. */
struct syncobj_waiter {
	spinlock_t lock;
	int woken;
	struct wait_queue_head wq;
	hrtimer_t timer;
	/* The poll timer may be running its callback: the waiter must not
	 * go away before it has finished (hrtimer_cancel() does not wait). */
	volatile int timer_busy;
};

static void syncobj_waiter_init(struct syncobj_waiter *w)
{
	mm_memset(w, 0, sizeof(*w));
	spinlock_init(&w->lock, "syncobj_waiter");
	wq_head_init(&w->wq, "syncobj_waiter");
}

/* From any context: fence callbacks (interrupts off), a syncobj's wait
 * list (its lock held), the poll timer. */
static void syncobj_waiter_wake(struct syncobj_waiter *w)
{
	uint64_t fl;

	spin_lock_irqsave(&w->lock, &fl);
	w->woken = 1;
	spin_unlock_irqrestore(&w->lock, fl);
	wq_wake_all(&w->wq);
}

static void syncobj_waiter_timer(hrtimer_t *t)
{
	struct syncobj_waiter *w = t->arg;

	syncobj_waiter_wake(w);
	/* the last touch of `w' */
	__atomic_store_n(&w->timer_busy, 0, __ATOMIC_RELEASE);
}

/* Sleep until woken, until `until' (monotonic ns) or until a signal; the
 * caller looks at everything again afterwards.  No locks held. */
static void syncobj_waiter_sleep(struct syncobj_waiter *w, uint64_t until)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	int highres = hrtimer_is_highres();
	uint64_t fl;

	spin_lock_irqsave(&w->lock, &fl);
	if (w->woken) {
		w->woken = 0;
		spin_unlock_irqrestore(&w->lock, fl);
		return;
	}
	wq_entry_init(&we, cur);
	wq_add(&w->wq, &we);
	cur->wait_channel = w;
	if (highres) {
		hrtimer_init(&w->timer, syncobj_waiter_timer, w);
		w->timer_busy = 1;
		hrtimer_start(&w->timer, until);
	} else {
		/* no fine-grained timer: the tick is the poll */
		cur->wakeup_tick = timer_ticks() + 1;
	}
	cur->state = TASK_BLOCKED;
	spin_unlock_irqrestore(&w->lock, fl);
	sched_schedule();
	if (highres) {
		if (hrtimer_cancel(&w->timer))
			w->timer_busy = 0; /* never fired */
		else
			while (__atomic_load_n(&w->timer_busy, __ATOMIC_ACQUIRE))
				__asm__ volatile("pause" ::: "memory");
	}
	/* This deadline belongs to this sleep and to nothing after it (see
	 * the note above sched_claim_wake()). */
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(&w->wq, &we);
	/* Consumed: the caller re-checks everything now. */
	spin_lock_irqsave(&w->lock, &fl);
	w->woken = 0;
	spin_unlock_irqrestore(&w->lock, fl);
}

/* ---- wait entries ---------------------------------------------------------- */

struct syncobj_wait_entry {
	struct list_head node; /* on a syncobj's cb_list; next == NULL: never was */
	struct syncobj_waiter *waiter;
	/* What stands for the point once known, referenced; written once,
	 * possibly by the syncobj's wait list on another processor. */
	struct drm_fence *fence;
	struct drm_fence_cb fence_cb;
	int cb_armed; /* fence_cb is on `fence' */
	uint64_t point;
};

static void syncobj_wait_syncobj_func(struct drm_syncobj *syncobj,
				      struct syncobj_wait_entry *wait);

struct syncobj_eventfd_entry {
	struct list_head node;
	struct drm_fence *fence;
	struct drm_fence_cb fence_cb;
	struct drm_syncobj *syncobj; /* not referenced: only while listed */
#if DRM_SYNCOBJ_EVENTFD
	struct eventfd_ctx *ev_fd_ctx;
#endif
	uint64_t point;
	uint32_t flags;
};

static void syncobj_eventfd_entry_func(struct drm_syncobj *syncobj,
				       struct syncobj_eventfd_entry *entry);

static struct drm_fence *entry_fence(struct syncobj_wait_entry *e)
{
	return __atomic_load_n(&e->fence, __ATOMIC_ACQUIRE);
}

/* ---- the "already signalled" fence ------------------------------------------ */

static const char *syncobj_stub_name(struct drm_fence *f)
{
	(void)f;
	return "stub";
}

static const struct drm_fence_ops syncobj_stub_ops = {
	.get_driver_name = syncobj_stub_name,
	.get_timeline_name = syncobj_stub_name,
};

/* What stands for a point the timeline has passed, inside a wait: one
 * fence, signalled from the start, never freed (its own reference is never
 * dropped), on no device.  Taking it allocates nothing, so it can be had
 * under a spinlock.  It never leaves this file: a caller that keeps the
 * fence -- or chains it -- gets a fence of the object's device instead
 * (syncobj_stub_alloc()). */
static struct drm_fence g_syncobj_stub = {
	.refs = 1,
	.signaled = 1,
	.wq = { .first = NULL, .lock = SPINLOCK_INIT("syncobj_stub") },
	.name = "stub",
	.ops = &syncobj_stub_ops,
	.cb_list = { &g_syncobj_stub.cb_list, &g_syncobj_stub.cb_list },
	.sflags = DRM_FENCE_SF_UNLISTED,
};

static struct drm_fence *syncobj_stub_get(void)
{
	drm_fence_get(&g_syncobj_stub);
	return &g_syncobj_stub;
}

/* A signalled fence of the object's device, referenced; NULL without
 * memory.  Process context. */
static struct drm_fence *syncobj_stub_alloc(struct drm_syncobj *syncobj)
{
	return drm_fence_signalled_at(syncobj->dev, hrtimer_now_ns());
}

/* ---- objects ------------------------------------------------------------- */

void drm_syncobj_get(struct drm_syncobj *so)
{
	__atomic_fetch_add(&so->refs, 1, __ATOMIC_ACQ_REL);
}

static void drm_syncobj_free(struct drm_syncobj *syncobj);

void drm_syncobj_put(struct drm_syncobj *so)
{
	if (!so)
		return;
	if (__atomic_sub_fetch(&so->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	drm_syncobj_free(so);
}

struct drm_fence *drm_syncobj_fence_get(struct drm_syncobj *so)
{
	struct drm_fence *f;
	uint64_t fl;

	spin_lock_irqsave(&so->lock, &fl);
	f = drm_fence_ref(so->fence);
	spin_unlock_irqrestore(&so->lock, fl);
	return f;
}

static void drm_syncobj_fence_add_wait(struct drm_syncobj *syncobj,
				       struct syncobj_wait_entry *wait)
{
	struct drm_fence *fence;
	uint64_t fl;

	if (entry_fence(wait))
		return;

	spin_lock_irqsave(&syncobj->lock, &fl);
	/* We have already tried once to get a fence and failed.  Now that we
	 * have the lock, try one more time just to be sure we don't add a
	 * callback when a fence has already been set. */
	fence = drm_fence_ref(syncobj->fence);
	if (!fence || drm_fence_chain_find_seqno(&fence, wait->point)) {
		drm_fence_put(fence);
		list_add_tail(&wait->node, &syncobj->cb_list);
	} else if (!fence) {
		__atomic_store_n(&wait->fence, syncobj_stub_get(), __ATOMIC_RELEASE);
	} else {
		__atomic_store_n(&wait->fence, fence, __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&syncobj->lock, fl);
}

static void drm_syncobj_remove_wait(struct drm_syncobj *syncobj,
				    struct syncobj_wait_entry *wait)
{
	uint64_t fl;

	if (!wait->node.next)
		return;

	spin_lock_irqsave(&syncobj->lock, &fl);
	list_del_init(&wait->node);
	spin_unlock_irqrestore(&syncobj->lock, fl);
}

static void syncobj_eventfd_entry_free(struct syncobj_eventfd_entry *entry)
{
#if DRM_SYNCOBJ_EVENTFD
	eventfd_ctx_put(entry->ev_fd_ctx);
#endif
	drm_fence_put(entry->fence);
	/* This happens either inside the syncobj lock, or after the node has
	 * already been removed from the list. */
	list_del(&entry->node);
	kfree(entry);
}

#if DRM_SYNCOBJ_EVENTFD
static void drm_syncobj_add_eventfd(struct drm_syncobj *syncobj,
				    struct syncobj_eventfd_entry *entry)
{
	uint64_t fl;

	spin_lock_irqsave(&syncobj->lock, &fl);
	list_add_tail(&entry->node, &syncobj->ev_fd_list);
	syncobj_eventfd_entry_func(syncobj, entry);
	spin_unlock_irqrestore(&syncobj->lock, fl);
}
#endif

/* Run the object's wait list and eventfd list against its new fence.
 * Caller holds the lock. */
static void syncobj_fence_changed_locked(struct drm_syncobj *syncobj)
{
	struct syncobj_wait_entry *wait_cur, *wait_tmp;
	struct syncobj_eventfd_entry *ev_fd_cur, *ev_fd_tmp;

	list_for_each_entry_safe(wait_cur, wait_tmp, &syncobj->cb_list, node)
		syncobj_wait_syncobj_func(syncobj, wait_cur);
	list_for_each_entry_safe(ev_fd_cur, ev_fd_tmp, &syncobj->ev_fd_list, node)
		syncobj_eventfd_entry_func(syncobj, ev_fd_cur);
}

void drm_syncobj_add_point_chain(struct drm_syncobj *syncobj,
				 struct drm_fence_chain *chain,
				 struct drm_fence *fence, uint64_t point)
{
	struct drm_fence *prev, *iter;
	uint64_t fl;

	drm_fence_get(fence);

	spin_lock_irqsave(&syncobj->lock, &fl);

	prev = drm_fence_ref(syncobj->fence);
	/* A point at or below the newest one makes the link start a stream
	 * of its own (see drm_fence_chain_init()); a later QUERY may then
	 * report 0 for the object.  The client's mistake, not an error. */
	drm_fence_chain_init(chain, prev, fence, point);
	/* The link took over the references to `prev' and `fence'; the
	 * object owns the one the link was born with. */
	syncobj->fence = &chain->base;

	syncobj_fence_changed_locked(syncobj);
	spin_unlock_irqrestore(&syncobj->lock, fl);

	/* Walk the chain once to cut what has signalled, then drop the
	 * reference the object held to its old fence. */
	drm_fence_chain_for_each(iter, prev)
		;
	drm_fence_put(prev);
}

int drm_syncobj_add_point(struct drm_syncobj *so, struct drm_fence *f,
			  uint64_t point)
{
	struct drm_fence_chain *chain;

	if (point == 0) {
		drm_syncobj_replace_fence(so, f);
		return 0;
	}
	chain = drm_fence_chain_alloc();
	if (!chain)
		return -ENOMEM;
	drm_syncobj_add_point_chain(so, chain, f, point);
	return 0;
}

void drm_syncobj_replace_fence(struct drm_syncobj *syncobj,
			       struct drm_fence *fence)
{
	struct drm_fence *old_fence;
	uint64_t fl;

	if (fence)
		drm_fence_get(fence);

	spin_lock_irqsave(&syncobj->lock, &fl);

	old_fence = syncobj->fence;
	syncobj->fence = fence;

	if (fence != old_fence)
		syncobj_fence_changed_locked(syncobj);

	spin_unlock_irqrestore(&syncobj->lock, fl);

	drm_fence_put(old_fence);
}

/* Give the object an already signalled fence. */
static int drm_syncobj_assign_null_handle(struct drm_syncobj *syncobj)
{
	struct drm_fence *fence = syncobj_stub_alloc(syncobj);

	if (!fence)
		return -ENOMEM;

	drm_syncobj_replace_fence(syncobj, fence);
	drm_fence_put(fence);
	return 0;
}

/* The fence for `point' of `syncobj', referenced, into *fence.  0; -EINVAL
 * when nothing stands for the point (an empty object, a point beyond the
 * newest, a point of a binary object); with WAIT_FOR_SUBMIT, first waits
 * up to five seconds for it to appear: -ETIME, -ERESTARTSYS.  A point the
 * timeline has passed gets a signalled fence of the object's device. */
static int syncobj_find_fence(struct drm_syncobj *syncobj, uint64_t point,
			      uint64_t flags, struct drm_fence **fence)
{
	struct syncobj_wait_entry wait;
	struct syncobj_waiter waiter;
	uint64_t deadline;
	int ret;

	*fence = NULL;
	if (flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)
		return -EINVAL;

	*fence = drm_syncobj_fence_get(syncobj);

	if (*fence) {
		ret = drm_fence_chain_find_seqno(fence, point);
		if (!ret) {
			/* If the requested seqno is already signaled the
			 * lookup leaves no fence.  To make sure the recipient
			 * gets signalled, use a new fence instead. */
			if (!*fence) {
				*fence = syncobj_stub_alloc(syncobj);
				if (!*fence)
					return -ENOMEM;
			}
			return 0;
		}
		drm_fence_put(*fence);
		*fence = NULL;
	} else {
		ret = -EINVAL;
	}

	if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))
		return ret;

	/* Waiting for userspace: no locks may be held here. */
	mm_memset(&wait, 0, sizeof(wait));
	syncobj_waiter_init(&waiter);
	wait.waiter = &waiter;
	wait.point = point;
	drm_syncobj_fence_add_wait(syncobj, &wait);

	deadline = hrtimer_now_ns() + DRM_SYNCOBJ_WAIT_FOR_SUBMIT_TIMEOUT;
	for (;;) {
		if (entry_fence(&wait)) {
			ret = 0;
			break;
		}
		if (hrtimer_now_ns() >= deadline) {
			ret = -ETIME;
			break;
		}
		if (signal_pending(sched_current())) {
			ret = -ERESTARTSYS;
			break;
		}
		syncobj_waiter_sleep(&waiter, deadline);
	}

	drm_syncobj_remove_wait(syncobj, &wait);
	/* Off the list: nothing writes the entry any more.  A fence that
	 * appeared at the last moment is a success after all. */
	*fence = entry_fence(&wait);
	if (*fence)
		ret = 0;
	if (*fence == &g_syncobj_stub) {
		drm_fence_put(*fence);
		*fence = syncobj_stub_alloc(syncobj);
		if (!*fence)
			ret = -ENOMEM;
	}
	return ret;
}

int drm_syncobj_find_fence(struct drm_syncobj *so, uint64_t point,
			   struct drm_fence **out)
{
	int ret = syncobj_find_fence(so, point, 0, out);

	return ret == -EINVAL ? -ENOENT : ret;
}

int drm_syncobj_find_fence_handle(struct drm_file *fp, uint32_t handle,
				  uint64_t point, uint64_t flags,
				  struct drm_fence **fence)
{
	struct drm_syncobj *syncobj = drm_syncobj_lookup(fp, handle);
	int ret;

	*fence = NULL;
	if (!syncobj)
		return -ENOENT;
	ret = syncobj_find_fence(syncobj, point, flags, fence);
	drm_syncobj_put(syncobj);
	return ret;
}

static void drm_syncobj_free(struct drm_syncobj *syncobj)
{
	struct syncobj_eventfd_entry *ev_fd_cur, *ev_fd_tmp;

	drm_syncobj_replace_fence(syncobj, NULL);

	list_for_each_entry_safe(ev_fd_cur, ev_fd_tmp, &syncobj->ev_fd_list, node)
		syncobj_eventfd_entry_free(ev_fd_cur);

	kfree(syncobj);
}

int drm_syncobj_create(struct drm_device *dev, struct drm_syncobj **out_syncobj,
		       uint32_t flags, struct drm_fence *fence)
{
	struct drm_syncobj *syncobj;
	int ret;

	syncobj = kcalloc(1, sizeof(*syncobj));
	if (!syncobj)
		return -ENOMEM;

	syncobj->refs = 1;
	syncobj->dev = dev;
	INIT_LIST_HEAD(&syncobj->cb_list);
	INIT_LIST_HEAD(&syncobj->ev_fd_list);
	spinlock_init(&syncobj->lock, "syncobj");

	if (flags & DRM_SYNCOBJ_CREATE_SIGNALED) {
		ret = drm_syncobj_assign_null_handle(syncobj);
		if (ret < 0) {
			drm_syncobj_put(syncobj);
			return ret;
		}
	}

	if (fence)
		drm_syncobj_replace_fence(syncobj, fence);

	*out_syncobj = syncobj;
	return 0;
}

/* ---- per-file handles ---------------------------------------------------- */

struct drm_syncobj *drm_syncobj_lookup(struct drm_file *fp, uint32_t handle)
{
	struct drm_syncobj *so = NULL;
	uint64_t fl;

	spin_lock_irqsave(&fp->lock, &fl);
	if (handle && handle < fp->nsyncobjs && fp->syncobjs[handle]) {
		so = fp->syncobjs[handle];
		drm_syncobj_get(so);
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	return so;
}

int drm_syncobj_get_handle(struct drm_file *fp, struct drm_syncobj *so,
			   uint32_t *handle_out)
{
	uint64_t fl;

	spin_lock_irqsave(&fp->lock, &fl);
	for (uint32_t h = 1; h < fp->nsyncobjs; h++) {
		if (!fp->syncobjs[h]) {
			drm_syncobj_get(so);
			fp->syncobjs[h] = so;
			spin_unlock_irqrestore(&fp->lock, fl);
			*handle_out = h;
			return 0;
		}
	}
	uint32_t n = fp->nsyncobjs ? fp->nsyncobjs * 2 : 64;
	if (n > SYNCOBJ_MAX_HANDLES) {
		spin_unlock_irqrestore(&fp->lock, fl);
		return -ENOSPC;
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	struct drm_syncobj **t = kalloc(n * sizeof(*t));
	if (!t)
		return -ENOMEM;
	mm_memset(t, 0, n * sizeof(*t));
	spin_lock_irqsave(&fp->lock, &fl);
	if (fp->nsyncobjs >= n) {
		/* Somebody grew it meanwhile; try again. */
		spin_unlock_irqrestore(&fp->lock, fl);
		kfree(t);
		return drm_syncobj_get_handle(fp, so, handle_out);
	}
	uint32_t h = fp->nsyncobjs ? fp->nsyncobjs : 1;
	if (fp->nsyncobjs) {
		mm_memcpy(t, fp->syncobjs, fp->nsyncobjs * sizeof(*t));
		kfree(fp->syncobjs);
	}
	fp->syncobjs = t;
	fp->nsyncobjs = n;
	drm_syncobj_get(so);
	fp->syncobjs[h] = so;
	spin_unlock_irqrestore(&fp->lock, fl);
	*handle_out = h;
	return 0;
}

static int drm_syncobj_create_as_handle(struct drm_device *dev,
					struct drm_file *fp, uint32_t *handle,
					uint32_t flags)
{
	struct drm_syncobj *syncobj;
	int ret;

	ret = drm_syncobj_create(dev, &syncobj, flags, NULL);
	if (ret)
		return ret;

	ret = drm_syncobj_get_handle(fp, syncobj, handle);
	drm_syncobj_put(syncobj);
	return ret;
}

static int drm_syncobj_destroy(struct drm_file *fp, uint32_t handle)
{
	struct drm_syncobj *so = NULL;
	uint64_t fl;

	spin_lock_irqsave(&fp->lock, &fl);
	if (handle && handle < fp->nsyncobjs && fp->syncobjs[handle]) {
		so = fp->syncobjs[handle];
		fp->syncobjs[handle] = NULL;
	}
	spin_unlock_irqrestore(&fp->lock, fl);
	if (!so)
		return -EINVAL;
	drm_syncobj_put(so);
	return 0;
}

void drm_syncobj_file_release(struct drm_file *fp)
{
	for (uint32_t h = 1; h < fp->nsyncobjs; h++) {
		if (fp->syncobjs[h]) {
			drm_syncobj_put(fp->syncobjs[h]);
			fp->syncobjs[h] = NULL;
		}
	}
	if (fp->syncobjs)
		kfree(fp->syncobjs);
	fp->syncobjs = NULL;
	fp->nsyncobjs = 0;
}

/* ---- descriptors --------------------------------------------------------- */

static void syncobj_fd_release(vfs_file_t *f)
{
	struct drm_syncobj *so = device_file_priv(f);
	if (so)
		drm_syncobj_put(so);
}

static const struct device_ops syncobj_fd_ops = {
	.release = syncobj_fd_release,
};

int drm_syncobj_get_fd(struct drm_syncobj *so, int *p_fd)
{
	task_t *cur = sched_current();

	drm_syncobj_get(so);
	vfs_file_t *file = device_anon_file(&syncobj_fd_ops, so, "syncobj",
					    O_RDWR);
	if (!file) {
		drm_syncobj_put(so);
		return -ENOMEM;
	}
	file->refcount = 1;
	int fd = fd_install(cur, file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	*p_fd = fd;
	return 0;
}

static int drm_syncobj_handle_to_fd(struct drm_file *fp, uint32_t handle,
				    int *p_fd)
{
	struct drm_syncobj *syncobj = drm_syncobj_lookup(fp, handle);
	int ret;

	if (!syncobj)
		return -EINVAL;

	ret = drm_syncobj_get_fd(syncobj, p_fd);
	drm_syncobj_put(syncobj);
	return ret;
}

static int drm_syncobj_fd_to_handle(struct drm_file *fp, int fd,
				    uint32_t *handle)
{
	task_t *cur = sched_current();
	vfs_file_t *f = fdget(cur, fd);
	struct drm_syncobj *syncobj;
	int ret;

	if (!f)
		return -EINVAL;
	if (device_file_ops(f) != &syncobj_fd_ops) {
		fdput(f);
		return -EINVAL;
	}
	/* a reference for as long as the handle is being made */
	syncobj = device_file_priv(f);
	drm_syncobj_get(syncobj);
	fdput(f);

	ret = drm_syncobj_get_handle(fp, syncobj, handle);
	drm_syncobj_put(syncobj);
	return ret;
}

static int drm_syncobj_import_sync_file_fence(struct drm_file *fp, int fd,
					      uint32_t handle, uint64_t point)
{
	struct drm_fence *fence = drm_fence_from_fd(fd);
	struct drm_syncobj *syncobj;

	if (!fence)
		return -EINVAL;

	syncobj = drm_syncobj_lookup(fp, handle);
	if (!syncobj) {
		drm_fence_put(fence);
		return -ENOENT;
	}

	if (point) {
		struct drm_fence_chain *chain = drm_fence_chain_alloc();

		/* A link holds a plain fence or an array, never another
		 * link: a descriptor exported from a timeline is flattened
		 * first. */
		if (chain && drm_fence_is_chain(fence)) {
			struct drm_fence *flat = drm_fence_unwrap_merge(1, &fence);

			drm_fence_put(fence);
			fence = flat;
		}
		if (!chain || !fence) {
			if (chain)
				drm_fence_chain_free(chain);
			drm_fence_put(fence);
			drm_syncobj_put(syncobj);
			return -ENOMEM;
		}

		drm_syncobj_add_point_chain(syncobj, chain, fence, point);
	} else {
		drm_syncobj_replace_fence(syncobj, fence);
	}

	drm_fence_put(fence);
	drm_syncobj_put(syncobj);
	return 0;
}

static int drm_syncobj_export_sync_file(struct drm_file *fp, uint32_t handle,
					uint64_t point, int *p_fd)
{
	struct drm_fence *fence;
	int ret, fd;

	ret = drm_syncobj_find_fence_handle(fp, handle, point, 0, &fence);
	if (ret)
		return ret;

	fd = drm_fence_export_fd(fence, 1);
	drm_fence_put(fence);
	if (fd < 0)
		return fd;

	*p_fd = fd;
	return 0;
}

/* ---- waiting -------------------------------------------------------------- */

static void syncobj_wait_fence_func(struct drm_fence *fence,
				    struct drm_fence_cb *cb)
{
	struct syncobj_wait_entry *wait =
		container_of(cb, struct syncobj_wait_entry, fence_cb);

	(void)fence;
	syncobj_waiter_wake(wait->waiter);
}

static void syncobj_wait_syncobj_func(struct drm_syncobj *syncobj,
				      struct syncobj_wait_entry *wait)
{
	struct drm_fence *fence;

	/* This happens inside the syncobj lock */
	fence = drm_fence_ref(syncobj->fence);
	if (!fence || drm_fence_chain_find_seqno(&fence, wait->point)) {
		drm_fence_put(fence);
		return;
	} else if (!fence) {
		__atomic_store_n(&wait->fence, syncobj_stub_get(), __ATOMIC_RELEASE);
	} else {
		__atomic_store_n(&wait->fence, fence, __ATOMIC_RELEASE);
	}

	list_del_init(&wait->node);
	syncobj_waiter_wake(wait->waiter);
}

/* Ask the devices behind the fences still waited for to bring their idea of
 * completed work up to date -- each device once per round, a container's
 * parts included.  Process context, no locks held. */
static void syncobj_poll_devices(struct syncobj_wait_entry *entries,
				 uint32_t count)
{
	struct drm_device *asked[4];
	unsigned nasked = 0;

	for (uint32_t i = 0; i < count; i++) {
		struct drm_fence *fence = entry_fence(&entries[i]);
		struct drm_fence_unwrap it;
		struct drm_fence *leaf;

		if (!fence || fence->signaled)
			continue;
		drm_fence_unwrap_for_each(leaf, &it, fence) {
			struct drm_device *dev = leaf->dev;
			unsigned k;

			if (leaf->signaled || !dev || !dev->drv ||
			    !dev->drv->fence_poll)
				continue;
			for (k = 0; k < nasked; k++)
				if (asked[k] == dev)
					break;
			if (k < nasked)
				continue;
			if (nasked == ARRAY_SIZE(asked)) {
				drm_fence_unwrap_end(&it);
				return;
			}
			asked[nasked++] = dev;
			dev->drv->fence_poll(dev);
		}
	}
}

/* The first fence of the wait that has not signalled, referenced (for the
 * report of a wait that does not end), or NULL. */
static struct drm_fence *syncobj_first_pending(struct syncobj_wait_entry *entries,
					       uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		struct drm_fence *f = entry_fence(&entries[i]);

		if (f && !f->signaled)
			return drm_fence_ref(f);
	}
	return NULL;
}

/* Wait for `count' points (`points' NULL: all zero).  0 with *idx the first
 * signalled one when not waiting for all; -EINVAL when a point has nothing
 * standing for it and the flags do not say to wait for that; -ETIME once
 * `timeout_abs_ns' (monotonic; zero or past: just look) has passed;
 * -ERESTARTSYS on a signal; -ENOMEM. */
static int drm_syncobj_array_wait_timeout(struct drm_syncobj **syncobjs,
					  const uint64_t *points, uint32_t count,
					  uint32_t flags, int64_t timeout_abs_ns,
					  uint32_t *idx, const uint64_t *deadline)
{
	struct syncobj_wait_entry *entries;
	struct syncobj_waiter waiter;
	struct drm_fence *fence;
	uint32_t signaled_count, i;
	uint64_t start, poll_ns = SYNCOBJ_POLL_NS;
	int stuck_said = 0;
	int ret = 0;

	entries = kcalloc(count, sizeof(*entries));
	if (!entries)
		return -ENOMEM;
	syncobj_waiter_init(&waiter);

	/* Walk the list of sync objects and initialize entries.  We do this
	 * up-front so that we can properly return -EINVAL if there is a
	 * syncobj with a missing fence and then never have the chance of
	 * returning -EINVAL again. */
	signaled_count = 0;
	for (i = 0; i < count; ++i) {
		entries[i].waiter = &waiter;
		entries[i].point = points ? points[i] : 0;
		fence = drm_syncobj_fence_get(syncobjs[i]);
		if (!fence || drm_fence_chain_find_seqno(&fence, entries[i].point)) {
			drm_fence_put(fence);
			if (flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
				continue;
			} else {
				ret = -EINVAL;
				goto cleanup_entries;
			}
		}

		if (fence)
			entries[i].fence = fence;
		else
			entries[i].fence = syncobj_stub_get();

		if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) ||
		    drm_fence_is_signaled(entries[i].fence)) {
			if (signaled_count == 0 && idx)
				*idx = i;
			signaled_count++;
		}
	}

	if (signaled_count == count ||
	    (signaled_count > 0 && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)))
		goto cleanup_entries;

	/* A fence may well have completed without anybody having noticed
	 * yet (a device that reports completion only when asked): the loop
	 * below asks before it decides, and asks again on every wake. */

	if (flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
		     DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
		for (i = 0; i < count; ++i)
			drm_syncobj_fence_add_wait(syncobjs[i], &entries[i]);
	}

	if (deadline) {
		for (i = 0; i < count; ++i) {
			fence = entry_fence(&entries[i]);
			if (!fence)
				continue;
			drm_fence_set_deadline(fence, *deadline);
		}
	}

	start = hrtimer_now_ns();
	for (;;) {
		uint64_t now, until;

		if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))
			syncobj_poll_devices(entries, count);

		signaled_count = 0;
		for (i = 0; i < count; ++i) {
			fence = entry_fence(&entries[i]);
			if (!fence)
				continue;

			if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) ||
			    drm_fence_is_signaled(fence) ||
			    (!entries[i].cb_armed &&
			     drm_fence_add_callback(fence, &entries[i].fence_cb,
						    syncobj_wait_fence_func))) {
				/* The fence has been signaled */
				if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) {
					signaled_count++;
				} else {
					if (idx)
						*idx = i;
					goto cleanup_entries;
				}
			} else {
				entries[i].cb_armed = 1;
			}
		}

		if (signaled_count == count)
			goto cleanup_entries;

		now = hrtimer_now_ns();
		if (timeout_abs_ns <= 0 || now >= (uint64_t)timeout_abs_ns) {
			ret = -ETIME;
			goto cleanup_entries;
		}

		if (signal_pending(sched_current())) {
			ret = -ERESTARTSYS;
			goto cleanup_entries;
		}

		if (!stuck_said && now - start >= SYNCOBJ_STUCK_NS) {
			struct drm_fence *f = syncobj_first_pending(entries, count);

			stuck_said = 1;
			if (f) {
				drm_fence_report_stuck(f, now - start, "a syncobj wait");
				drm_fence_put(f);
			}
		}

		until = now + poll_ns;
		if (until > (uint64_t)timeout_abs_ns)
			until = (uint64_t)timeout_abs_ns;
		if (poll_ns < SYNCOBJ_POLL_MAX_NS)
			poll_ns *= 2;
		syncobj_waiter_sleep(&waiter, until);
	}

cleanup_entries:
	for (i = 0; i < count; ++i) {
		drm_syncobj_remove_wait(syncobjs[i], &entries[i]);
		if (entries[i].cb_armed)
			drm_fence_remove_callback(entries[i].fence,
						  &entries[i].fence_cb);
		drm_fence_put(entries[i].fence);
	}
	kfree(entries);

	return ret;
}

/* ---- argument arrays -------------------------------------------------------- */

static int copy_u32_array(uint64_t uptr, uint32_t n, uint32_t **out)
{
	if (n > SYNCOBJ_MAX_ARRAY)
		return -EINVAL;
	if (!uptr || !validate_user_ptr(uptr, (uint64_t)n * sizeof(uint32_t)))
		return -EFAULT;
	uint32_t *a = kalloc(n * sizeof(uint32_t));
	if (!a)
		return -ENOMEM;
	if (drm_copy_from_user(a, (const void *)uptr, n * sizeof(uint32_t))) {
		kfree(a);
		return -EFAULT;
	}
	*out = a;
	return 0;
}

/* The points of a timeline wait or signal.  A null pointer means every
 * point is zero -- the binary-object form of the timeline calls, which is
 * how the graphics library waits for an imported fence to appear ("wait
 * available" with no points at all). */
static int copy_u64_array(uint64_t uptr, uint32_t n, uint64_t **out)
{
	if (n > SYNCOBJ_MAX_ARRAY)
		return -EINVAL;
	if (uptr && !validate_user_ptr(uptr, (uint64_t)n * sizeof(uint64_t)))
		return -EFAULT;
	uint64_t *a = kalloc(n * sizeof(uint64_t));
	if (!a)
		return -ENOMEM;
	if (!uptr) {
		for (uint32_t i = 0; i < n; i++)
			a[i] = 0;
	} else if (drm_copy_from_user(a, (const void *)uptr, n * sizeof(uint64_t))) {
		kfree(a);
		return -EFAULT;
	}
	*out = a;
	return 0;
}

/* Resolve the client's `count_handles' handles into references; on
 * failure nothing is held. */
static int drm_syncobj_array_find(struct drm_file *fp, uint64_t user_handles,
				  uint32_t count_handles,
				  struct drm_syncobj ***syncobjs_out)
{
	struct drm_syncobj **syncobjs;
	uint32_t i, *handles;
	int ret;

	ret = copy_u32_array(user_handles, count_handles, &handles);
	if (ret)
		return ret;

	syncobjs = kalloc(count_handles * sizeof(*syncobjs));
	if (!syncobjs) {
		ret = -ENOMEM;
		goto err_free_handles;
	}

	for (i = 0; i < count_handles; i++) {
		syncobjs[i] = drm_syncobj_lookup(fp, handles[i]);
		if (!syncobjs[i]) {
			ret = -ENOENT;
			goto err_put_syncobjs;
		}
	}

	kfree(handles);
	*syncobjs_out = syncobjs;
	return 0;

err_put_syncobjs:
	while (i-- > 0)
		drm_syncobj_put(syncobjs[i]);
	kfree(syncobjs);
err_free_handles:
	kfree(handles);
	return ret;
}

static void drm_syncobj_array_free(struct drm_syncobj **syncobjs,
				   uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		drm_syncobj_put(syncobjs[i]);
	kfree(syncobjs);
}

/* ---- ioctls --------------------------------------------------------------- */

static int drm_syncobj_create_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *fp)
{
	struct drm_syncobj_create *args = data;

	/* no valid flags yet */
	if (args->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;

	return drm_syncobj_create_as_handle(dev, fp, &args->handle, args->flags);
}

static int drm_syncobj_destroy_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *fp)
{
	struct drm_syncobj_destroy *args = data;

	(void)dev;
	/* make sure padding is empty */
	if (args->pad)
		return -EINVAL;
	return drm_syncobj_destroy(fp, args->handle);
}

static int drm_syncobj_handle_to_fd_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *fp)
{
	struct syncobj_handle_args *args = data;
	unsigned int valid_flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE |
				   DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	uint64_t point = 0;

	(void)dev;
	if (args->pad)
		return -EINVAL;

	if (args->flags & ~valid_flags)
		return -EINVAL;

	if (args->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE)
		point = args->point;

	if (args->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)
		return drm_syncobj_export_sync_file(fp, args->handle, point,
						    &args->fd);

	if (args->point)
		return -EINVAL;

	return drm_syncobj_handle_to_fd(fp, args->handle, &args->fd);
}

static int drm_syncobj_fd_to_handle_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *fp)
{
	struct syncobj_handle_args *args = data;
	unsigned int valid_flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE |
				   DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	uint64_t point = 0;

	(void)dev;
	if (args->pad)
		return -EINVAL;

	if (args->flags & ~valid_flags)
		return -EINVAL;

	if (args->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE)
		point = args->point;

	if (args->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)
		return drm_syncobj_import_sync_file_fence(fp, args->fd,
							  args->handle, point);

	if (args->point)
		return -EINVAL;

	return drm_syncobj_fd_to_handle(fp, args->fd, &args->handle);
}

static int drm_syncobj_transfer_to_timeline(struct drm_file *fp,
					    struct drm_syncobj_transfer *args)
{
	struct drm_syncobj *timeline_syncobj = NULL;
	struct drm_fence *fence, *tmp;
	struct drm_fence_chain *chain;
	int ret;

	timeline_syncobj = drm_syncobj_lookup(fp, args->dst_handle);
	if (!timeline_syncobj)
		return -ENOENT;
	ret = drm_syncobj_find_fence_handle(fp, args->src_handle,
					    args->src_point, args->flags, &tmp);
	if (ret)
		goto err_put_timeline;

	fence = drm_fence_unwrap_merge(1, &tmp);
	drm_fence_put(tmp);
	if (!fence) {
		ret = -ENOMEM;
		goto err_put_timeline;
	}

	chain = drm_fence_chain_alloc();
	if (!chain) {
		ret = -ENOMEM;
		goto err_free_fence;
	}

	drm_syncobj_add_point_chain(timeline_syncobj, chain, fence, args->dst_point);
err_free_fence:
	drm_fence_put(fence);
err_put_timeline:
	drm_syncobj_put(timeline_syncobj);

	return ret;
}

static int drm_syncobj_transfer_to_binary(struct drm_file *fp,
					  struct drm_syncobj_transfer *args)
{
	struct drm_syncobj *binary_syncobj = NULL;
	struct drm_fence *fence;
	int ret;

	binary_syncobj = drm_syncobj_lookup(fp, args->dst_handle);
	if (!binary_syncobj)
		return -ENOENT;
	ret = drm_syncobj_find_fence_handle(fp, args->src_handle,
					    args->src_point, args->flags, &fence);
	if (ret)
		goto err;
	drm_syncobj_replace_fence(binary_syncobj, fence);
	drm_fence_put(fence);
err:
	drm_syncobj_put(binary_syncobj);

	return ret;
}

static int drm_syncobj_transfer_ioctl(struct drm_device *dev, void *data,
				      struct drm_file *fp)
{
	struct drm_syncobj_transfer *args = data;

	(void)dev;
	if (args->pad)
		return -EINVAL;

	if (args->dst_point)
		return drm_syncobj_transfer_to_timeline(fp, args);
	return drm_syncobj_transfer_to_binary(fp, args);
}

static int drm_syncobj_array_wait(struct drm_syncobj **syncobjs,
				  const uint64_t *points, uint32_t count,
				  uint32_t flags, int64_t timeout_nsec,
				  uint32_t *first_signaled,
				  const uint64_t *deadline)
{
	uint32_t first = ~0u;
	int ret;

	ret = drm_syncobj_array_wait_timeout(syncobjs, points, count, flags,
					     timeout_nsec, &first, deadline);
	if (ret < 0)
		return ret;
	*first_signaled = first;
	return 0;
}

static int drm_syncobj_wait_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *fp)
{
	struct drm_syncobj_wait *args = data;
	struct drm_syncobj **syncobjs;
	unsigned int possible_flags;
	uint64_t t, *tp = NULL;
	int ret = 0;

	(void)dev;
	possible_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;

	if (args->flags & ~possible_flags)
		return -EINVAL;

	if (args->count_handles == 0)
		return 0;

	ret = drm_syncobj_array_find(fp, args->handles, args->count_handles,
				     &syncobjs);
	if (ret < 0)
		return ret;

	if (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE) {
		t = args->deadline_nsec;
		tp = &t;
	}

	ret = drm_syncobj_array_wait(syncobjs, NULL, args->count_handles,
				     args->flags, args->timeout_nsec,
				     &args->first_signaled, tp);

	drm_syncobj_array_free(syncobjs, args->count_handles);

	return ret;
}

static int drm_syncobj_timeline_wait_ioctl(struct drm_device *dev, void *data,
					   struct drm_file *fp)
{
	struct drm_syncobj_timeline_wait *args = data;
	struct drm_syncobj **syncobjs;
	unsigned int possible_flags;
	uint64_t t, *tp = NULL;
	uint64_t *points;
	int ret = 0;

	(void)dev;
	possible_flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
			 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE;

	if (args->flags & ~possible_flags)
		return -EINVAL;

	if (args->count_handles == 0)
		return 0;

	ret = drm_syncobj_array_find(fp, args->handles, args->count_handles,
				     &syncobjs);
	if (ret < 0)
		return ret;

	ret = copy_u64_array(args->points, args->count_handles, &points);
	if (ret)
		goto out;

	if (args->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE) {
		t = args->deadline_nsec;
		tp = &t;
	}

	ret = drm_syncobj_array_wait(syncobjs, points, args->count_handles,
				     args->flags, args->timeout_nsec,
				     &args->first_signaled, tp);
	kfree(points);
out:
	drm_syncobj_array_free(syncobjs, args->count_handles);

	return ret;
}

#if DRM_SYNCOBJ_EVENTFD
static void syncobj_eventfd_entry_fence_func(struct drm_fence *fence,
					     struct drm_fence_cb *cb)
{
	struct syncobj_eventfd_entry *entry =
		container_of(cb, struct syncobj_eventfd_entry, fence_cb);

	(void)fence;
	eventfd_signal(entry->ev_fd_ctx);
	syncobj_eventfd_entry_free(entry);
}
#endif

static void syncobj_eventfd_entry_func(struct drm_syncobj *syncobj,
				       struct syncobj_eventfd_entry *entry)
{
#if DRM_SYNCOBJ_EVENTFD
	struct drm_fence *fence;
	int ret;

	/* This happens inside the syncobj lock */
	fence = drm_fence_ref(syncobj->fence);
	if (!fence)
		return;

	ret = drm_fence_chain_find_seqno(&fence, entry->point);
	if (ret != 0) {
		/* The given seqno has not been submitted yet. */
		drm_fence_put(fence);
		return;
	} else if (!fence) {
		/* The lookup succeeded but left no fence: the given seqno
		 * has signalled and a later one has been submitted already.
		 * Stand a signalled fence for it so that the eventfd still
		 * gets signalled below. */
		fence = syncobj_stub_get();
	}

	list_del_init(&entry->node);
	entry->fence = fence;

	if (entry->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) {
		eventfd_signal(entry->ev_fd_ctx);
		syncobj_eventfd_entry_free(entry);
	} else {
		ret = drm_fence_add_callback(fence, &entry->fence_cb,
					     syncobj_eventfd_entry_fence_func);
		if (ret == -ENOENT) {
			eventfd_signal(entry->ev_fd_ctx);
			syncobj_eventfd_entry_free(entry);
		}
	}
#else
	/* nothing is ever registered without the eventfd entry points */
	(void)syncobj;
	(void)entry;
#endif
}

static int drm_syncobj_eventfd_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *fp)
{
	struct drm_syncobj_eventfd *args = data;
	struct drm_syncobj *syncobj;

	(void)dev;
	if (args->flags & ~DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)
		return -EINVAL;

	if (args->pad)
		return -EINVAL;

	syncobj = drm_syncobj_lookup(fp, args->handle);
	if (!syncobj)
		return -ENOENT;

#if DRM_SYNCOBJ_EVENTFD
	struct eventfd_ctx *ev_fd_ctx;
	struct syncobj_eventfd_entry *entry;
	int ret;

	ret = eventfd_ctx_fdget(args->fd, &ev_fd_ctx);
	if (ret)
		goto err_fdget;

	entry = kcalloc(1, sizeof(*entry));
	if (!entry) {
		ret = -ENOMEM;
		goto err_kzalloc;
	}
	entry->syncobj = syncobj;
	entry->ev_fd_ctx = ev_fd_ctx;
	entry->point = args->point;
	entry->flags = args->flags;

	drm_syncobj_add_eventfd(syncobj, entry);
	drm_syncobj_put(syncobj);

	return 0;

err_kzalloc:
	eventfd_ctx_put(ev_fd_ctx);
err_fdget:
	drm_syncobj_put(syncobj);
	return ret;
#else
	drm_syncobj_put(syncobj);
	return -EOPNOTSUPP;
#endif
}

static int drm_syncobj_reset_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *fp)
{
	struct drm_syncobj_array *args = data;
	struct drm_syncobj **syncobjs;
	uint32_t i;
	int ret;

	(void)dev;
	if (args->pad != 0)
		return -EINVAL;

	if (args->count_handles == 0)
		return -EINVAL;

	ret = drm_syncobj_array_find(fp, args->handles, args->count_handles,
				     &syncobjs);
	if (ret < 0)
		return ret;

	for (i = 0; i < args->count_handles; i++)
		drm_syncobj_replace_fence(syncobjs[i], NULL);

	drm_syncobj_array_free(syncobjs, args->count_handles);

	return 0;
}

static int drm_syncobj_signal_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *fp)
{
	struct drm_syncobj_array *args = data;
	struct drm_syncobj **syncobjs;
	uint32_t i;
	int ret;

	(void)dev;
	if (args->pad != 0)
		return -EINVAL;

	if (args->count_handles == 0)
		return -EINVAL;

	ret = drm_syncobj_array_find(fp, args->handles, args->count_handles,
				     &syncobjs);
	if (ret < 0)
		return ret;

	for (i = 0; i < args->count_handles; i++) {
		ret = drm_syncobj_assign_null_handle(syncobjs[i]);
		if (ret < 0)
			break;
	}

	drm_syncobj_array_free(syncobjs, args->count_handles);

	return ret;
}

static int drm_syncobj_timeline_signal_ioctl(struct drm_device *dev, void *data,
					     struct drm_file *fp)
{
	struct drm_syncobj_timeline_array *args = data;
	struct drm_syncobj **syncobjs;
	struct drm_fence_chain **chains;
	struct drm_fence **stubs;
	uint64_t *points;
	uint32_t i, j;
	int ret;

	(void)dev;
	if (args->flags != 0)
		return -EINVAL;

	if (args->count_handles == 0)
		return -EINVAL;

	ret = drm_syncobj_array_find(fp, args->handles, args->count_handles,
				     &syncobjs);
	if (ret < 0)
		return ret;

	ret = copy_u64_array(args->points, args->count_handles, &points);
	if (ret)
		goto out;

	chains = kcalloc(args->count_handles, sizeof(*chains));
	stubs = kcalloc(args->count_handles, sizeof(*stubs));
	if (!chains || !stubs) {
		ret = -ENOMEM;
		goto err_chains;
	}
	/* Everything allocated before anything is changed: all or none. */
	for (i = 0; i < args->count_handles; i++) {
		chains[i] = drm_fence_chain_alloc();
		stubs[i] = syncobj_stub_alloc(syncobjs[i]);
		if (!chains[i] || !stubs[i]) {
			for (j = 0; j <= i; j++) {
				if (chains[j])
					drm_fence_chain_free(chains[j]);
				drm_fence_put(stubs[j]);
			}
			ret = -ENOMEM;
			goto err_chains;
		}
	}

	for (i = 0; i < args->count_handles; i++) {
		drm_syncobj_add_point_chain(syncobjs[i], chains[i], stubs[i],
					    points[i]);
		drm_fence_put(stubs[i]);
	}
err_chains:
	kfree(chains);
	kfree(stubs);
	kfree(points);
out:
	drm_syncobj_array_free(syncobjs, args->count_handles);

	return ret;
}

static int drm_syncobj_query_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *fp)
{
	struct drm_syncobj_timeline_array *args = data;
	struct drm_syncobj **syncobjs;
	uint64_t *points = (uint64_t *)(uintptr_t)args->points;
	uint32_t i;
	int ret;

	(void)dev;
	if (args->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED)
		return -EINVAL;

	if (args->count_handles == 0)
		return -EINVAL;

	ret = drm_syncobj_array_find(fp, args->handles, args->count_handles,
				     &syncobjs);
	if (ret < 0)
		return ret;

	if (!points || !validate_user_ptr(args->points,
					  (uint64_t)args->count_handles * sizeof(uint64_t))) {
		ret = -EFAULT;
		goto out;
	}

	for (i = 0; i < args->count_handles; i++) {
		struct drm_fence_chain *chain;
		struct drm_fence *fence;
		uint64_t point;

		fence = drm_syncobj_fence_get(syncobjs[i]);
		chain = to_drm_fence_chain(fence);
		if (chain) {
			struct drm_fence *iter, *last_signaled =
				drm_fence_ref(fence);

			if (args->flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
				point = fence->seqno64;
			} else {
				drm_fence_chain_for_each(iter, fence) {
					if (iter->context != fence->context) {
						drm_fence_put(iter);
						/* It is most likely that the
						 * timeline has unordered
						 * points. */
						break;
					}
					drm_fence_put(last_signaled);
					last_signaled = drm_fence_ref(iter);
				}
				if (drm_fence_is_signaled(last_signaled))
					point = last_signaled->seqno64;
				else if (to_drm_fence_chain(last_signaled))
					point = to_drm_fence_chain(last_signaled)->prev_seqno;
				else
					point = 0;
			}
			drm_fence_put(last_signaled);
		} else {
			point = 0;
		}
		drm_fence_put(fence);
		if (drm_copy_to_user(&points[i], &point, sizeof(uint64_t))) {
			ret = -EFAULT;
			break;
		}
	}
out:
	drm_syncobj_array_free(syncobjs, args->count_handles);

	return ret;
}

/* ---- dispatch --------------------------------------------------------------- */

int drm_syncobj_is_ioctl(unsigned nr)
{
	return (nr >= 0xBF && nr <= 0xC5) || (nr >= 0xCA && nr <= 0xCD) ||
	       nr == 0xCF;
}

long drm_syncobj_ioctl(struct drm_device *dev, struct drm_file *fp,
		       unsigned nr, void *kb, unsigned size, int *handled)
{
	(void)size; /* the ioctl layer zero-fills a shorter argument */
	*handled = 1;

	switch (nr) {
	case 0xBF:
		return drm_syncobj_create_ioctl(dev, kb, fp);
	case 0xC0:
		return drm_syncobj_destroy_ioctl(dev, kb, fp);
	case 0xC1:
		return drm_syncobj_handle_to_fd_ioctl(dev, kb, fp);
	case 0xC2:
		return drm_syncobj_fd_to_handle_ioctl(dev, kb, fp);
	case 0xC3:
		return drm_syncobj_wait_ioctl(dev, kb, fp);
	case 0xC4:
		return drm_syncobj_reset_ioctl(dev, kb, fp);
	case 0xC5:
		return drm_syncobj_signal_ioctl(dev, kb, fp);
	case 0xCA:
		return drm_syncobj_timeline_wait_ioctl(dev, kb, fp);
	case 0xCB:
		return drm_syncobj_query_ioctl(dev, kb, fp);
	case 0xCC:
		return drm_syncobj_transfer_ioctl(dev, kb, fp);
	case 0xCD:
		return drm_syncobj_timeline_signal_ioctl(dev, kb, fp);
	case 0xCF:
		return drm_syncobj_eventfd_ioctl(dev, kb, fp);
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
