// LikeOS-64 -- display-manager fences and sync_file descriptors.
//
// A fence is a point in the GPU's command stream; it signals when the
// device has passed it.  Userspace waits on it by handle (the backend's
// fence ioctls) or as a sync_file descriptor: poll() readable when
// signalled, mergeable with another into a fence for both.
#include <kernel/dev/gpu/drm.h>
#include <kernel/uapi/drm/sync_file.h>
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

struct drm_fence *drm_fence_create(struct drm_device *dev, uint32_t seqno,
				   uint32_t flags)
{
	struct drm_fence *f = kalloc(sizeof(*f));
	uint64_t fl;

	if (!f)
		return NULL;
	mm_memset(f, 0, sizeof(*f));
	f->refs = 1;
	f->dev = dev;
	f->seqno = seqno;
	f->flags = flags;
	wq_head_init(&f->wq, "drm_fence");
	spin_lock_irqsave(&dev->lock, &fl);
	f->next = dev->fences;
	dev->fences = f;
	/* Already passed?  (A fence created for a seqno the device has
	 * been seen to pass, or a signalled placeholder.) */
	if ((int32_t)(dev->fence_passed - seqno) >= 0) {
		f->signaled = 1;
		f->signal_ns = hrtimer_now_ns();
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	return f;
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
	kfree(f);
}

void drm_fence_signal(struct drm_fence *f)
{
	if (f->signaled)
		return;
	f->signaled = 1;
	f->signal_ns = hrtimer_now_ns();
	poll_notify_wq(&f->wq);
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
	for (struct drm_fence *f = dev->fences; f; f = f->next) {
		if (!f->signaled && (int32_t)(passed - f->seqno) >= 0) {
			f->signaled = 1;
			f->signal_ns = hrtimer_now_ns();
			if (nw < 64) {
				drm_fence_get(f);
				wake[nw++] = f;
			}
		}
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	for (int i = 0; i < nw; i++) {
		poll_notify_wq(&wake[i]->wq);
		drm_fence_put(wake[i]);
	}
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

static void drm_fence_poll(struct drm_fence *f)
{
	struct drm_device *dev = f->dev;

	if (!f->signaled && dev && dev->drv && dev->drv->fence_poll)
		dev->drv->fence_poll(dev);
}

/* What waiting has cost, for whoever reports frame accounting.  Relaxed
 * atomics: a lost sample would skew a diagnostic and nothing else, and this
 * sits on the path whose cost is being measured. */

static int fence_wait_do(struct drm_fence *f, uint64_t timeout_ns, int intr)
{
	task_t *cur = sched_current();
	uint64_t deadline = hrtimer_now_ns() + timeout_ns;
	uint64_t spin_until = hrtimer_now_ns() + DRM_FENCE_SPIN_NS;
	uint64_t poll_ns = DRM_FENCE_POLL_NS;

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
	return fence_wait_do(f, timeout_ns, intr);
}

/* ---- sync_file ------------------------------------------------------- */

struct sync_file_ctx {
	struct drm_fence *fence;
	char name[32];
};

static long sync_file_ioctl(vfs_file_t *f, unsigned long req, void *argp,
			    struct task *cur)
{
	struct sync_file_ctx *c = device_file_priv(f);
	(void)cur;

	if (_IOC_TYPE(req) != SYNC_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(req) == 3) { /* SYNC_IOC_MERGE */
		struct sync_merge_data md;

		if (copy_from_user(&md, argp, sizeof(md)) != 0)
			return -EFAULT;
		struct drm_fence *other = drm_fence_from_fd(md.fd2);
		if (!other)
			return -EINVAL;
		/* The merged fence: the later of the two seqnos on one
		 * device (the only case here), signalled when both are. */
		struct drm_fence *a = c->fence;
		struct drm_fence *later = a;
		if (other->dev == a->dev && (int32_t)(other->seqno - a->seqno) > 0)
			later = other;
		struct drm_fence *m = drm_fence_create(later->dev, later->seqno,
						       a->flags | other->flags);
		if (!m) {
			drm_fence_put(other);
			return -ENOMEM;
		}
		if (a->signaled && other->signaled)
			drm_fence_signal(m);
		int fd = drm_fence_export_fd(m, (md.flags & 1) ? 1 : 0);
		drm_fence_put(m);
		drm_fence_put(other);
		if (fd < 0)
			return fd;
		md.fence = fd;
		if (copy_to_user(argp, &md, sizeof(md)) != 0)
			return -EFAULT;
		return 0;
	}
	if (_IOC_NR(req) == 4) { /* SYNC_IOC_FILE_INFO */
		struct sync_file_info info;

		if (copy_from_user(&info, argp, sizeof(info)) != 0)
			return -EFAULT;
		mm_memset(info.name, 0, sizeof(info.name));
		for (int i = 0; c->name[i] && i < 31; i++)
			info.name[i] = c->name[i];
		info.status = c->fence->signaled ? 1 : 0;
		info.flags = 0;
		if (info.num_fences >= 1 && info.sync_fence_info) {
			struct sync_fence_info fi;

			mm_memset(&fi, 0, sizeof(fi));
			for (int i = 0; c->name[i] && i < 31; i++)
				fi.obj_name[i] = c->name[i];
			fi.driver_name[0] = 'd';
			fi.driver_name[1] = 'r';
			fi.driver_name[2] = 'm';
			fi.status = info.status;
			fi.timestamp_ns = c->fence->signal_ns;
			if (copy_to_user((void *)(uintptr_t)info.sync_fence_info,
					 &fi, sizeof(fi)) != 0)
				return -EFAULT;
		}
		info.num_fences = 1;
		if (copy_to_user(argp, &info, sizeof(info)) != 0)
			return -EFAULT;
		return 0;
	}
	return -ENOTTY;
}

static short sync_file_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct sync_file_ctx *c = device_file_priv(f);

	poll_wait(pt, f, &c->fence->wq);
	return (events & POLLIN) && c->fence->signaled ? POLLIN : 0;
}

static void sync_file_release(vfs_file_t *f)
{
	struct sync_file_ctx *c = device_file_priv(f);

	if (c) {
		drm_fence_put(c->fence);
		kfree(c);
	}
}

static const struct device_ops sync_file_ops = {
	.ioctl = sync_file_ioctl,
	.poll = sync_file_poll,
	.release = sync_file_release,
};

int drm_fence_export_fd(struct drm_fence *f, int cloexec)
{
	task_t *cur = sched_current();
	struct sync_file_ctx *c = kalloc(sizeof(*c));

	if (!c)
		return -ENOMEM;
	mm_memset(c, 0, sizeof(*c));
	drm_fence_get(f);
	c->fence = f;
	ksnprintf(c->name, sizeof(c->name), "%s:%u", f->dev->drv->name, f->seqno);
	vfs_file_t *file = device_anon_file(&sync_file_ops, c, "sync_file", O_RDWR);
	if (!file) {
		drm_fence_put(f);
		kfree(c);
		return -ENOMEM;
	}
	file->refcount = 1;
	int fd = fd_install(cur, file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	if (cloexec)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}

struct drm_fence *drm_fence_from_fd(int fd)
{
	task_t *cur = sched_current();
	vfs_file_t *f = fdget(cur, fd);

	if (!f)
		return NULL;
	if (device_file_ops(f) != &sync_file_ops) {
		fdput(f);
		return NULL;
	}
	struct sync_file_ctx *c = device_file_priv(f);
	struct drm_fence *fence = c->fence;
	drm_fence_get(fence);
	fdput(f);
	return fence;
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
