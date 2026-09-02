// LikeOS-64 -- eventfd: a 64-bit counter behind a descriptor.
//
// write() adds to the counter, read() returns it and zeroes it (or, in
// semaphore mode, returns 1 and decrements), poll() says readable when it
// is non-zero and writable while it can still be added to.  The primitive
// event loops (GLib's GWakeup, a compositor's frame clock) use to wake
// themselves from another thread through the same poll() they already sit
// in.
#include <kernel/dev/device.h>
#include <kernel/uapi/anonfd.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

struct eventfd_ctx {
	spinlock_t lock;
	uint64_t count;
	int semaphore;
	struct wait_queue_head wq;
};

#define EVENTFD_MAX 0xFFFFFFFFFFFFFFFEULL

/* Sleep until `cond' (evaluated under the lock) or a signal. */
static int eventfd_wait(struct eventfd_ctx *c, int want_read)
{
	task_t *cur = sched_current();

	for (;;) {
		uint64_t fl;
		int ready;

		spin_lock_irqsave(&c->lock, &fl);
		ready = want_read ? (c->count != 0) : (c->count < EVENTFD_MAX);
		if (ready) {
			spin_unlock_irqrestore(&c->lock, fl);
			return 0;
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

static long eventfd_read(vfs_file_t *f, void *buf, long bytes, int nonblock)
{
	struct eventfd_ctx *c = device_file_priv(f);
	uint64_t val, fl;

	if (bytes < 8)
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&c->lock, &fl);
		if (c->count) {
			if (c->semaphore) {
				val = 1;
				c->count--;
			} else {
				val = c->count;
				c->count = 0;
			}
			spin_unlock_irqrestore(&c->lock, fl);
			poll_notify_wq(&c->wq);
			if (copy_to_user(buf, &val, 8) != 0)
				return -EFAULT;
			return 8;
		}
		spin_unlock_irqrestore(&c->lock, fl);
		if (nonblock)
			return -EAGAIN;
		int rc = eventfd_wait(c, 1);
		if (rc)
			return rc;
	}
}

static long eventfd_write(vfs_file_t *f, const void *buf, long bytes,
			  int nonblock)
{
	struct eventfd_ctx *c = device_file_priv(f);
	uint64_t add, fl;

	if (bytes < 8)
		return -EINVAL;
	if (copy_from_user(&add, buf, 8) != 0)
		return -EFAULT;
	if (add == 0xFFFFFFFFFFFFFFFFULL)
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&c->lock, &fl);
		if (EVENTFD_MAX - c->count >= add) {
			c->count += add;
			spin_unlock_irqrestore(&c->lock, fl);
			poll_notify_wq(&c->wq);
			return 8;
		}
		spin_unlock_irqrestore(&c->lock, fl);
		if (nonblock)
			return -EAGAIN;
		int rc = eventfd_wait(c, 0);
		if (rc)
			return rc;
	}
}

static short eventfd_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct eventfd_ctx *c = device_file_priv(f);
	short rev = 0;

	poll_wait(pt, f, &c->wq);
	if ((events & POLLIN) && c->count)
		rev |= POLLIN;
	if ((events & POLLOUT) && c->count < EVENTFD_MAX)
		rev |= POLLOUT;
	return rev;
}

static void eventfd_release(vfs_file_t *f)
{
	struct eventfd_ctx *c = device_file_priv(f);
	if (c)
		kfree(c);
}

static const struct device_ops eventfd_ops = {
	.read = eventfd_read,
	.write = eventfd_write,
	.poll = eventfd_poll,
	.release = eventfd_release,
};

int64_t sys_eventfd2(uint64_t initval, uint64_t flags)
{
	task_t *cur = sched_current();

	if (!cur)
		return -EFAULT;
	if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK))
		return -EINVAL;
	struct eventfd_ctx *c = kalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	mm_memset(c, 0, sizeof(*c));
	spinlock_init(&c->lock, "eventfd");
	wq_head_init(&c->wq, "eventfd");
	c->count = (uint32_t)initval;
	c->semaphore = (flags & EFD_SEMAPHORE) != 0;

	/* O_RDWR: the write path refuses a file whose flags carry no write
	 * bit, and an eventfd is written to -- that is how it is posted. */
	vfs_file_t *file = device_anon_file(&eventfd_ops, c, "anon_inode:[eventfd]",
					    O_RDWR | ((flags & EFD_NONBLOCK) ?
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
	if (flags & EFD_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}
