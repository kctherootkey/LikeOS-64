// LikeOS -- synchronisation objects.
//
// A syncobj is a handle a renderer passes between its submissions: a
// container for one fence (binary) or for a timeline of them, where each
// point on the timeline carries the fence of the submission that reaches
// it.  Its worth is that it can be created empty, filled later by a
// submission, waited for before it is filled (WAIT_FOR_SUBMIT), shared as
// a descriptor, and turned into or made from a sync_file.  Drivers attach
// their submission fences through drm_syncobj_add_point() and read the
// fences they must wait for through drm_syncobj_find_fence().
//
// Waits are polled: a sleeper re-checks on a short timer and on any
// attach/signal notification (the same sleep the fence wait uses, see
// drm_fence.c), so a fence signalled from an interrupt never has to know
// which syncobjs hold it.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

#define SYNCOBJ_MAX_POINTS 64 /* pending timeline points per object */
#define SYNCOBJ_MAX_HANDLES 65536 /* per file: one per submission while its fence lives */
#define SYNCOBJ_MAX_ARRAY 256 /* handles per ioctl */
#define SYNCOBJ_POLL_NS 250000ULL
#define SYNCOBJ_POLL_MAX_NS 2000000ULL

struct syncobj_point {
	uint64_t point;
	struct drm_fence *fence;
};

struct drm_syncobj {
	int refs;
	spinlock_t lock;
	struct drm_fence *fence; /* binary payload, point 0 */
	struct syncobj_point pts[SYNCOBJ_MAX_POINTS]; /* ascending */
	int npts;
	uint64_t signaled_point; /* the timeline has passed this */
	uint64_t submitted_point; /* highest point ever attached */
};

/* One queue for every waiter; waits are rare enough and short enough that
 * waking them all on any change costs nothing measurable. */
static struct wait_queue_head g_syncobj_wq;
static int g_syncobj_wq_ready;

static void syncobj_notify(void)
{
	if (g_syncobj_wq_ready)
		poll_notify_wq(&g_syncobj_wq);
}

/* ---- objects ------------------------------------------------------------ */

static struct drm_syncobj *syncobj_new(void)
{
	struct drm_syncobj *so = kalloc(sizeof(*so));

	if (!so)
		return NULL;
	mm_memset(so, 0, sizeof(*so));
	so->refs = 1;
	spinlock_init(&so->lock, "syncobj");
	if (!g_syncobj_wq_ready) {
		wq_head_init(&g_syncobj_wq, "syncobj");
		g_syncobj_wq_ready = 1;
	}
	return so;
}

void drm_syncobj_get(struct drm_syncobj *so)
{
	__atomic_fetch_add(&so->refs, 1, __ATOMIC_ACQ_REL);
}

void drm_syncobj_put(struct drm_syncobj *so)
{
	if (!so)
		return;
	if (__atomic_sub_fetch(&so->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	if (so->fence)
		drm_fence_put(so->fence);
	for (int i = 0; i < so->npts; i++)
		drm_fence_put(so->pts[i].fence);
	kfree(so);
}

/* Retire signalled points: the timeline has passed the highest of them
 * and they need not be kept.  Caller holds so->lock. */
static void syncobj_gc_locked(struct drm_syncobj *so)
{
	int keep = 0;
	uint64_t passed = so->signaled_point;

	for (int i = 0; i < so->npts; i++) {
		if (so->pts[i].fence->signaled && so->pts[i].point > passed)
			passed = so->pts[i].point;
	}
	so->signaled_point = passed;
	for (int i = 0; i < so->npts; i++) {
		if (so->pts[i].point <= passed) {
			drm_fence_put(so->pts[i].fence);
			continue;
		}
		so->pts[keep++] = so->pts[i];
	}
	so->npts = keep;
}

struct drm_fence *drm_syncobj_fence_get(struct drm_syncobj *so)
{
	struct drm_fence *f;
	uint64_t fl;

	spin_lock_irqsave(&so->lock, &fl);
	f = so->fence;
	if (f)
		drm_fence_get(f);
	spin_unlock_irqrestore(&so->lock, fl);
	return f;
}

void drm_syncobj_replace_fence(struct drm_syncobj *so, struct drm_fence *f)
{
	struct drm_fence *old;
	uint64_t fl;

	if (f)
		drm_fence_get(f);
	spin_lock_irqsave(&so->lock, &fl);
	old = so->fence;
	so->fence = f;
	spin_unlock_irqrestore(&so->lock, fl);
	if (old)
		drm_fence_put(old);
	syncobj_notify();
}

int drm_syncobj_add_point(struct drm_syncobj *so, struct drm_fence *f,
			  uint64_t point)
{
	uint64_t fl;
	int i;

	if (point == 0) {
		drm_syncobj_replace_fence(so, f);
		return 0;
	}
	drm_fence_get(f);
	spin_lock_irqsave(&so->lock, &fl);
	syncobj_gc_locked(so);
	if (point > so->submitted_point)
		so->submitted_point = point;
	if (f->signaled && point > so->signaled_point)
		so->signaled_point = point;
	if (point <= so->signaled_point) {
		/* Already passed: nothing to keep. */
		spin_unlock_irqrestore(&so->lock, fl);
		drm_fence_put(f);
		syncobj_notify();
		return 0;
	}
	/* Replace an entry for the same point; else insert in order. */
	for (i = 0; i < so->npts; i++) {
		if (so->pts[i].point == point) {
			struct drm_fence *old = so->pts[i].fence;
			so->pts[i].fence = f;
			spin_unlock_irqrestore(&so->lock, fl);
			drm_fence_put(old);
			syncobj_notify();
			return 0;
		}
		if (so->pts[i].point > point)
			break;
	}
	if (so->npts >= SYNCOBJ_MAX_POINTS) {
		spin_unlock_irqrestore(&so->lock, fl);
		drm_fence_put(f);
		return -ENOSPC;
	}
	for (int j = so->npts; j > i; j--)
		so->pts[j] = so->pts[j - 1];
	so->pts[i].point = point;
	so->pts[i].fence = f;
	so->npts++;
	spin_unlock_irqrestore(&so->lock, fl);
	syncobj_notify();
	return 0;
}

int drm_syncobj_find_fence(struct drm_syncobj *so, uint64_t point,
			   struct drm_fence **out)
{
	uint64_t fl;

	*out = NULL;
	spin_lock_irqsave(&so->lock, &fl);
	if (point == 0) {
		if (!so->fence) {
			spin_unlock_irqrestore(&so->lock, fl);
			return -ENOENT;
		}
		*out = so->fence;
		drm_fence_get(*out);
		spin_unlock_irqrestore(&so->lock, fl);
		return 0;
	}
	syncobj_gc_locked(so);
	if (point <= so->signaled_point) {
		spin_unlock_irqrestore(&so->lock, fl);
		*out = drm_fence_signalled(so->fence ? so->fence->dev : NULL);
		return *out ? 0 : -ENOMEM;
	}
	for (int i = 0; i < so->npts; i++) {
		if (so->pts[i].point >= point) {
			*out = so->pts[i].fence;
			drm_fence_get(*out);
			spin_unlock_irqrestore(&so->lock, fl);
			return 0;
		}
	}
	spin_unlock_irqrestore(&so->lock, fl);
	return -ENOENT;
}

/* Is `point' signalled?  0 no, 1 yes; -ENOENT when nothing stands for it
 * yet (not submitted). */
static int syncobj_point_signaled(struct drm_syncobj *so, uint64_t point,
				  int *submitted)
{
	uint64_t fl;
	int rc = -ENOENT;

	*submitted = 0;
	spin_lock_irqsave(&so->lock, &fl);
	if (point == 0) {
		if (so->fence) {
			*submitted = 1;
			rc = so->fence->signaled ? 1 : 0;
		}
		spin_unlock_irqrestore(&so->lock, fl);
		return rc;
	}
	syncobj_gc_locked(so);
	if (point <= so->signaled_point) {
		*submitted = 1;
		rc = 1;
	} else {
		for (int i = 0; i < so->npts; i++) {
			if (so->pts[i].point >= point) {
				*submitted = 1;
				rc = so->pts[i].fence->signaled ? 1 : 0;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&so->lock, fl);
	return rc;
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

static int syncobj_handle_create(struct drm_file *fp, struct drm_syncobj *so,
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
		return syncobj_handle_create(fp, so, handle_out);
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

static int syncobj_handle_delete(struct drm_file *fp, uint32_t handle)
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

static int syncobj_export_fd(struct drm_syncobj *so)
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
	return fd;
}

static struct drm_syncobj *syncobj_from_fd(int fd)
{
	task_t *cur = sched_current();
	vfs_file_t *f = fdget(cur, fd);

	if (!f)
		return NULL;
	if (device_file_ops(f) != &syncobj_fd_ops) {
		fdput(f);
		return NULL;
	}
	struct drm_syncobj *so = device_file_priv(f);
	drm_syncobj_get(so);
	fdput(f);
	return so;
}

/* ---- waiting -------------------------------------------------------------- */

static void syncobj_poll_wake(hrtimer_t *t)
{
	task_t *task = t->arg;

	if (sched_claim_wake(task, TASK_BLOCKED)) {
		task->wait_channel = NULL;
		sched_enqueue_ready(task);
	}
}

/* The condition of a wait: for each handle, its point; satisfied when
 * all (or any) are signalled, or -- WAIT_FOR_SUBMIT off -- immediately
 * -EINVAL when a point has nothing standing for it.  Returns 1 satisfied,
 * 0 not yet, negative errno. */
static int syncobj_wait_check(struct drm_syncobj **sos, const uint64_t *pts,
			      uint32_t n, uint32_t flags, uint32_t *first)
{
	int all = !!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL);
	int for_submit = !!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
	int available = !!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
	uint32_t done = 0;

	for (uint32_t i = 0; i < n; i++) {
		int submitted;
		int s = syncobj_point_signaled(sos[i], pts ? pts[i] : 0,
					       &submitted);
		if (!submitted) {
			if (!for_submit && !available)
				return -EINVAL;
			continue;
		}
		if (available || s == 1) {
			done++;
			if (!all) {
				*first = i;
				return 1;
			}
		}
	}
	if (all && done == n)
		return 1;
	return 0;
}

static int syncobj_wait(struct drm_syncobj **sos, const uint64_t *pts,
			uint32_t n, uint32_t flags, int64_t timeout_abs_ns,
			uint32_t *first)
{
	task_t *cur = sched_current();
	uint64_t poll_ns = SYNCOBJ_POLL_NS;

	for (;;) {
		int rc = syncobj_wait_check(sos, pts, n, flags, first);
		if (rc)
			return rc < 0 ? rc : 0;
		if (signal_pending(cur))
			return -ERESTARTSYS;
		uint64_t now = hrtimer_now_ns();
		if (timeout_abs_ns <= 0 || (int64_t)now >= timeout_abs_ns)
			return -ETIME;

		struct wait_queue_entry we;
		hrtimer_t poll_timer;
		int highres = hrtimer_is_highres();
		uint64_t fl;

		if (highres)
			hrtimer_init(&poll_timer, syncobj_poll_wake, cur);
		fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&g_syncobj_wq, &we);
		cur->wait_channel = &g_syncobj_wq;
		if (highres) {
			uint64_t until = now + poll_ns;
			if ((int64_t)until > timeout_abs_ns)
				until = (uint64_t)timeout_abs_ns;
			hrtimer_start(&poll_timer, until);
			if (poll_ns < SYNCOBJ_POLL_MAX_NS)
				poll_ns *= 2;
		} else {
			cur->wakeup_tick = timer_ticks() + 1;
		}
		cur->state = TASK_BLOCKED;
		local_irq_restore(fl);
		sched_schedule();
		if (highres)
			hrtimer_cancel(&poll_timer);
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		wq_remove(&g_syncobj_wq, &we);
	}
}

/* ---- ioctls --------------------------------------------------------------- */

int drm_syncobj_is_ioctl(unsigned nr)
{
	return (nr >= 0xBF && nr <= 0xC5) || (nr >= 0xCA && nr <= 0xCD) ||
	       nr == 0xCF;
}

static int copy_u32_array(uint64_t uptr, uint32_t n, uint32_t **out)
{
	if (n == 0 || n > SYNCOBJ_MAX_ARRAY)
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
 * point is zero -- the binary-object form of the timeline calls, which
 * is how the graphics library waits for an imported fence to appear
 * ("wait available" with no points at all); a null pointer to a query,
 * which writes the points back, is a fault. */
static int copy_u64_array(uint64_t uptr, uint32_t n, int null_is_zero, uint64_t **out)
{
	if (n == 0 || n > SYNCOBJ_MAX_ARRAY)
		return -EINVAL;
	if (!uptr && !null_is_zero)
		return -EFAULT;
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

/* Resolve `n' handles into references; on failure nothing is held. */
static int lookup_array(struct drm_file *fp, const uint32_t *handles,
			uint32_t n, struct drm_syncobj ***out)
{
	struct drm_syncobj **sos = kalloc(n * sizeof(*sos));

	if (!sos)
		return -ENOMEM;
	for (uint32_t i = 0; i < n; i++) {
		sos[i] = drm_syncobj_lookup(fp, handles[i]);
		if (!sos[i]) {
			for (uint32_t j = 0; j < i; j++)
				drm_syncobj_put(sos[j]);
			kfree(sos);
			return -ENOENT;
		}
	}
	*out = sos;
	return 0;
}

static void put_array(struct drm_syncobj **sos, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		drm_syncobj_put(sos[i]);
	kfree(sos);
}

long drm_syncobj_ioctl(struct drm_device *dev, struct drm_file *fp,
		       unsigned nr, void *kb, unsigned size, int *handled)
{
	(void)size;
	*handled = 1;

	switch (nr) {
	case 0xBF: { /* CREATE */
		struct drm_syncobj_create *a = kb;
		if (a->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED)
			return -EINVAL;
		struct drm_syncobj *so = syncobj_new();
		if (!so)
			return -ENOMEM;
		if (a->flags & DRM_SYNCOBJ_CREATE_SIGNALED) {
			struct drm_fence *f = drm_fence_signalled(dev);
			if (f) {
				so->fence = f; /* the create reference */
			}
		}
		int rc = syncobj_handle_create(fp, so, &a->handle);
		drm_syncobj_put(so);
		return rc;
	}
	case 0xC0: { /* DESTROY */
		struct drm_syncobj_destroy *a = kb;
		return syncobj_handle_delete(fp, a->handle);
	}
	case 0xC1: { /* HANDLE_TO_FD */
		struct drm_syncobj_handle *a = kb;
		struct drm_syncobj *so = drm_syncobj_lookup(fp, a->handle);
		int fd;
		if (!so)
			return -ENOENT;
		if (a->flags & ~DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) {
			drm_syncobj_put(so);
			return -EINVAL;
		}
		if (a->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) {
			struct drm_fence *f = drm_syncobj_fence_get(so);
			drm_syncobj_put(so);
			if (!f)
				return -EINVAL;
			fd = drm_fence_export_fd(f, 1);
			drm_fence_put(f);
		} else {
			fd = syncobj_export_fd(so);
			drm_syncobj_put(so);
		}
		if (fd < 0)
			return fd;
		a->fd = fd;
		return 0;
	}
	case 0xC2: { /* FD_TO_HANDLE */
		struct drm_syncobj_handle *a = kb;
		if (a->flags & ~DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)
			return -EINVAL;
		if (a->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {
			struct drm_syncobj *so = drm_syncobj_lookup(fp, a->handle);
			if (!so)
				return -ENOENT;
			struct drm_fence *f = drm_fence_from_fd(a->fd);
			if (!f) {
				drm_syncobj_put(so);
				return -EINVAL;
			}
			drm_syncobj_replace_fence(so, f);
			drm_fence_put(f);
			drm_syncobj_put(so);
			return 0;
		}
		struct drm_syncobj *so = syncobj_from_fd(a->fd);
		if (!so)
			return -EINVAL;
		int rc = syncobj_handle_create(fp, so, &a->handle);
		drm_syncobj_put(so);
		return rc;
	}
	case 0xC3: { /* WAIT */
		struct drm_syncobj_wait *a = kb;
		uint32_t *handles;
		struct drm_syncobj **sos;
		int rc;
		if (a->flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
				 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE))
			return -EINVAL;
		rc = copy_u32_array(a->handles, a->count_handles, &handles);
		if (rc)
			return rc;
		rc = lookup_array(fp, handles, a->count_handles, &sos);
		kfree(handles);
		if (rc)
			return rc;
		a->first_signaled = 0;
		rc = syncobj_wait(sos, NULL, a->count_handles, a->flags,
				  a->timeout_nsec, &a->first_signaled);
		put_array(sos, a->count_handles);
		return rc;
	}
	case 0xC4: /* RESET */
	case 0xC5: { /* SIGNAL */
		struct drm_syncobj_array *a = kb;
		uint32_t *handles;
		struct drm_syncobj **sos;
		int rc = copy_u32_array(a->handles, a->count_handles, &handles);
		if (rc)
			return rc;
		rc = lookup_array(fp, handles, a->count_handles, &sos);
		kfree(handles);
		if (rc)
			return rc;
		for (uint32_t i = 0; i < a->count_handles; i++) {
			if (nr == 0xC4) {
				drm_syncobj_replace_fence(sos[i], NULL);
			} else {
				struct drm_fence *f = drm_fence_signalled(dev);
				if (f) {
					drm_syncobj_replace_fence(sos[i], f);
					drm_fence_put(f);
				}
			}
		}
		put_array(sos, a->count_handles);
		return 0;
	}
	case 0xCA: { /* TIMELINE_WAIT */
		struct drm_syncobj_timeline_wait *a = kb;
		uint32_t *handles;
		uint64_t *points;
		struct drm_syncobj **sos;
		int rc;
		if (a->flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
				 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT |
				 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE |
				 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE))
			return -EINVAL;
		rc = copy_u32_array(a->handles, a->count_handles, &handles);
		if (rc)
			return rc;
		rc = copy_u64_array(a->points, a->count_handles, 1, &points);
		if (rc) {
			kfree(handles);
			return rc;
		}
		rc = lookup_array(fp, handles, a->count_handles, &sos);
		kfree(handles);
		if (rc) {
			kfree(points);
			return rc;
		}
		a->first_signaled = 0;
		rc = syncobj_wait(sos, points, a->count_handles, a->flags,
				  a->timeout_nsec, &a->first_signaled);
		put_array(sos, a->count_handles);
		kfree(points);
		return rc;
	}
	case 0xCB: /* QUERY */
	case 0xCD: { /* TIMELINE_SIGNAL */
		struct drm_syncobj_timeline_array *a = kb;
		uint32_t *handles;
		uint64_t *points;
		struct drm_syncobj **sos;
		int rc;
		if (nr == 0xCB && (a->flags & ~DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED))
			return -EINVAL;
		if (nr == 0xCD && a->flags)
			return -EINVAL;
		rc = copy_u32_array(a->handles, a->count_handles, &handles);
		if (rc)
			return rc;
		rc = copy_u64_array(a->points, a->count_handles, nr == 0xCD, &points);
		if (rc) {
			kfree(handles);
			return rc;
		}
		rc = lookup_array(fp, handles, a->count_handles, &sos);
		kfree(handles);
		if (rc) {
			kfree(points);
			return rc;
		}
		for (uint32_t i = 0; i < a->count_handles; i++) {
			struct drm_syncobj *so = sos[i];
			uint64_t fl;
			if (nr == 0xCB) {
				spin_lock_irqsave(&so->lock, &fl);
				syncobj_gc_locked(so);
				points[i] = (a->flags &
					     DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) ?
						    so->submitted_point :
						    so->signaled_point;
				spin_unlock_irqrestore(&so->lock, fl);
			} else {
				struct drm_fence *f = drm_fence_signalled(dev);
				if (f) {
					drm_syncobj_add_point(so, f, points[i]);
					drm_fence_put(f);
				}
			}
		}
		if (nr == 0xCB &&
		    drm_copy_to_user((void *)a->points, points,
				     a->count_handles * sizeof(uint64_t)))
			rc = -EFAULT;
		put_array(sos, a->count_handles);
		kfree(points);
		return rc;
	}
	case 0xCC: { /* TRANSFER */
		struct drm_syncobj_transfer *a = kb;
		if (a->flags)
			return -EINVAL;
		struct drm_syncobj *src = drm_syncobj_lookup(fp, a->src_handle);
		struct drm_syncobj *dst = drm_syncobj_lookup(fp, a->dst_handle);
		struct drm_fence *f = NULL;
		int rc = 0;
		if (!src || !dst) {
			rc = -ENOENT;
			goto transfer_out;
		}
		rc = drm_syncobj_find_fence(src, a->src_point, &f);
		if (rc)
			goto transfer_out;
		rc = drm_syncobj_add_point(dst, f, a->dst_point);
transfer_out:
		if (f)
			drm_fence_put(f);
		if (src)
			drm_syncobj_put(src);
		if (dst)
			drm_syncobj_put(dst);
		return rc;
	}
	case 0xCF: /* EVENTFD */
		return -ENOTTY;
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
