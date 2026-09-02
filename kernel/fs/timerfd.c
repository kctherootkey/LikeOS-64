// LikeOS-64 -- timerfd: a timer whose expirations are read from a
// descriptor and waited for with poll().
//
// read() returns the number of expirations since the last read (8 bytes)
// and blocks, or says EAGAIN, when there are none.  Built on the
// high-resolution timer queue, so a one-shot or periodic deadline is met
// to the microsecond rather than to the scheduler tick.
#include <kernel/dev/device.h>
#include <kernel/uapi/anonfd.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

struct timerfd_ctx {
	spinlock_t lock;
	hrtimer_t timer;
	int clockid;
	uint64_t expires_ns; /* monotonic; 0 = disarmed */
	uint64_t interval_ns;
	uint64_t ticks; /* expirations not yet read */
	struct wait_queue_head wq;
	int refs; /* file + in-flight callback safety */
};

static void timerfd_fire(hrtimer_t *t)
{
	struct timerfd_ctx *c = (struct timerfd_ctx *)t->arg;
	uint64_t fl;

	spin_lock_irqsave(&c->lock, &fl);
	if (c->expires_ns) {
		uint64_t now = hrtimer_now_ns();
		if (c->interval_ns) {
			/* Count every period that went by, then re-arm on
			 * the original phase so drift does not accumulate. */
			uint64_t missed = 1;
			if (now > c->expires_ns)
				missed += (now - c->expires_ns) / c->interval_ns;
			c->ticks += missed;
			c->expires_ns += missed * c->interval_ns;
			hrtimer_start(&c->timer, c->expires_ns);
		} else {
			c->ticks += 1;
			c->expires_ns = 0;
		}
	}
	spin_unlock_irqrestore(&c->lock, fl);
	poll_notify_wq(&c->wq);
}

static long timerfd_read(vfs_file_t *f, void *buf, long bytes, int nonblock)
{
	struct timerfd_ctx *c = device_file_priv(f);
	task_t *cur = sched_current();
	uint64_t fl;

	if (bytes < 8)
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&c->lock, &fl);
		if (c->ticks) {
			uint64_t v = c->ticks;
			c->ticks = 0;
			spin_unlock_irqrestore(&c->lock, fl);
			if (copy_to_user(buf, &v, 8) != 0)
				return -EFAULT;
			return 8;
		}
		if (nonblock || !c->expires_ns) {
			spin_unlock_irqrestore(&c->lock, fl);
			return -EAGAIN;
		}
		if (signal_pending(cur)) {
			spin_unlock_irqrestore(&c->lock, fl);
			return -EINTR;
		}
		struct wait_queue_entry we;
		wq_entry_init(&we, cur);
		wq_add(&c->wq, &we);
		cur->wait_channel = c;
		cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&c->lock, fl);
		sched_schedule();
		wq_remove(&c->wq, &we);
	}
}

static short timerfd_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct timerfd_ctx *c = device_file_priv(f);

	poll_wait(pt, f, &c->wq);
	return (events & POLLIN) && c->ticks ? POLLIN : 0;
}

static void timerfd_release(vfs_file_t *f)
{
	struct timerfd_ctx *c = device_file_priv(f);

	if (!c)
		return;
	hrtimer_cancel(&c->timer);
	kfree(c);
}

static const struct device_ops timerfd_ops = {
	.read = timerfd_read,
	.poll = timerfd_poll,
	.release = timerfd_release,
};

int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags)
{
	task_t *cur = sched_current();

	if (!cur)
		return -EFAULT;
	if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK))
		return -EINVAL;
	switch (clockid) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_MONOTONIC_COARSE:
		break;
	default:
		return -EINVAL;
	}
	struct timerfd_ctx *c = kalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	mm_memset(c, 0, sizeof(*c));
	spinlock_init(&c->lock, "timerfd");
	wq_head_init(&c->wq, "timerfd");
	hrtimer_init(&c->timer, timerfd_fire, c);
	c->clockid = (int)clockid;

	vfs_file_t *file = device_anon_file(&timerfd_ops, c, "anon_inode:[timerfd]",
					    O_RDWR | ((flags & TFD_NONBLOCK) ?
							      O_NONBLOCK :
							      0));
	if (!file) {
		kfree(c);
		return -ENOMEM;
	}
	file->refcount = 1;
	int fd = fd_install(cur, file);
	if (fd < 0) {
		vfs_close(file);
		return fd;
	}
	if (flags & TFD_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}

static uint64_t ts_to_ns(const struct k_timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static void ns_to_ts(uint64_t ns, struct k_timespec *ts)
{
	ts->tv_sec = ns / 1000000000ULL;
	ts->tv_nsec = ns % 1000000000ULL;
}

static struct timerfd_ctx *timerfd_of(task_t *cur, int fd, vfs_file_t **fp)
{
	vfs_file_t *f = fdget(cur, fd);

	if (!f)
		return NULL;
	if (device_file_ops(f) != &timerfd_ops) {
		fdput(f);
		return NULL;
	}
	*fp = f;
	return device_file_priv(f);
}

int64_t sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_ptr,
			    uint64_t old_ptr)
{
	task_t *cur = sched_current();
	struct k_itimerspec nv, ov;
	vfs_file_t *f;

	if (!cur)
		return -EFAULT;
	if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
		return -EINVAL;
	if (copy_from_user(&nv, (void *)new_ptr, sizeof(nv)) != 0)
		return -EFAULT;
	if (nv.it_value.tv_nsec < 0 || nv.it_value.tv_nsec >= 1000000000L ||
	    nv.it_interval.tv_nsec < 0 ||
	    nv.it_interval.tv_nsec >= 1000000000L)
		return -EINVAL;

	struct timerfd_ctx *c = timerfd_of(cur, (int)fd, &f);
	if (!c)
		return -EINVAL;

	uint64_t fl;
	spin_lock_irqsave(&c->lock, &fl);
	uint64_t now = hrtimer_now_ns();
	/* The old setting, relative. */
	mm_memset(&ov, 0, sizeof(ov));
	if (c->expires_ns)
		ns_to_ts(c->expires_ns > now ? c->expires_ns - now : 0,
			 &ov.it_value);
	ns_to_ts(c->interval_ns, &ov.it_interval);

	hrtimer_cancel(&c->timer);
	c->ticks = 0;
	uint64_t value = ts_to_ns(&nv.it_value);
	c->interval_ns = ts_to_ns(&nv.it_interval);
	if (value == 0) {
		c->expires_ns = 0; /* disarm */
	} else {
		if (flags & TFD_TIMER_ABSTIME) {
			if (c->clockid == CLOCK_REALTIME ||
			    c->clockid == CLOCK_REALTIME_COARSE) {
				uint64_t epoch_ns =
					timer_get_boot_epoch() * 1000000000ULL;
				value = value > epoch_ns ? value - epoch_ns : 0;
			}
			c->expires_ns = value;
		} else {
			c->expires_ns = now + value;
		}
		hrtimer_start(&c->timer, c->expires_ns);
	}
	spin_unlock_irqrestore(&c->lock, fl);
	fdput(f);

	if (old_ptr && copy_to_user((void *)old_ptr, &ov, sizeof(ov)) != 0)
		return -EFAULT;
	return 0;
}

int64_t sys_timerfd_gettime(uint64_t fd, uint64_t cur_ptr)
{
	task_t *cur = sched_current();
	struct k_itimerspec ov;
	vfs_file_t *f;

	if (!cur)
		return -EFAULT;
	struct timerfd_ctx *c = timerfd_of(cur, (int)fd, &f);
	if (!c)
		return -EINVAL;
	uint64_t fl;
	spin_lock_irqsave(&c->lock, &fl);
	uint64_t now = hrtimer_now_ns();
	mm_memset(&ov, 0, sizeof(ov));
	if (c->expires_ns)
		ns_to_ts(c->expires_ns > now ? c->expires_ns - now : 0,
			 &ov.it_value);
	ns_to_ts(c->interval_ns, &ov.it_interval);
	spin_unlock_irqrestore(&c->lock, fl);
	fdput(f);
	if (copy_to_user((void *)cur_ptr, &ov, sizeof(ov)) != 0)
		return -EFAULT;
	return 0;
}
