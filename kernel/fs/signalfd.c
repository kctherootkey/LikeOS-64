// LikeOS-64 -- signalfd: receive signals by reading a descriptor.
//
// The signals in the mask are expected to be BLOCKED by the caller; the
// descriptor then dequeues them into signalfd_siginfo records instead of
// their being delivered to a handler, and poll() reports the descriptor
// readable while one is pending.  An event loop that already sits in
// poll() thus handles SIGCHLD or SIGTERM in line with everything else.
#include <kernel/dev/device.h>
#include <kernel/uapi/anonfd.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

struct signalfd_ctx {
	kernel_sigset_t mask;
};

struct wait_queue_head *signalfd_task_wq(task_t *task)
{
	if (!task->signals.sigfd_wq) {
		struct wait_queue_head *h = kalloc(sizeof(*h));
		if (!h)
			return NULL;
		wq_head_init(h, "sigfd");
		/* Publish once; a racing sibling on another CPU that also
		 * allocated one loses and frees its copy. */
		if (!__sync_bool_compare_and_swap(&task->signals.sigfd_wq, NULL, h))
			kfree(h);
	}
	return task->signals.sigfd_wq;
}

static int signalfd_any_pending(task_t *t, const kernel_sigset_t *mask)
{
	for (int s = 1; s < NSIG; s++)
		if (sigismember_k(mask, s) &&
		    sigismember_k(&t->signals.pending, s))
			return 1;
	return 0;
}

static void signalfd_fill(struct signalfd_siginfo *out, const siginfo_t *in)
{
	mm_memset(out, 0, sizeof(*out));
	out->ssi_signo = (uint32_t)in->si_signo;
	out->ssi_errno = in->si_errno;
	out->ssi_code = in->si_code;
	out->ssi_pid = (uint32_t)in->si_pid;
	out->ssi_uid = (uint32_t)in->si_uid;
	out->ssi_status = in->si_status;
	out->ssi_addr = (uint64_t)(uintptr_t)in->si_addr;
	out->ssi_int = in->si_int;
	out->ssi_ptr = (uint64_t)(uintptr_t)in->si_ptr;
}

static long signalfd_read(vfs_file_t *f, void *buf, long bytes, int nonblock)
{
	struct signalfd_ctx *c = device_file_priv(f);
	task_t *cur = sched_current();
	long done = 0;

	if (bytes < (long)sizeof(struct signalfd_siginfo))
		return -EINVAL;
	for (;;) {
		siginfo_t info;
		int sig;

		mm_memset(&info, 0, sizeof(info));
		sig = signal_dequeue_wanted(cur, &c->mask, &info);
		if (sig > 0) {
			struct signalfd_siginfo ssi;

			signalfd_fill(&ssi, &info);
			ssi.ssi_signo = (uint32_t)sig;
			if (copy_to_user((char *)buf + done, &ssi,
					 sizeof(ssi)) != 0)
				return done ? done : -EFAULT;
			done += (long)sizeof(ssi);
			if (done + (long)sizeof(ssi) > bytes)
				return done;
			continue;
		}
		if (done)
			return done;
		if (nonblock)
			return -EAGAIN;
		/* Nothing of ours pending: wait for a signal to arrive.  A
		 * signal outside the mask that needs handling ends the wait
		 * with EINTR, as any blocking read. */
		if (signal_pending(cur))
			return -EINTR;
		struct wait_queue_head *wq = signalfd_task_wq(cur);
		if (!wq)
			return -ENOMEM;
		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();

		wq_entry_init(&we, cur);
		wq_add(wq, &we);
		if (!signalfd_any_pending(cur, &c->mask)) {
			cur->wait_channel = c;
			cur->state = TASK_BLOCKED;
			local_irq_restore(fl);
			sched_schedule();
		} else {
			local_irq_restore(fl);
		}
		wq_remove(wq, &we);
	}
}

static short signalfd_poll(vfs_file_t *f, short events, struct poll_table *pt)
{
	struct signalfd_ctx *c = device_file_priv(f);
	task_t *cur = sched_current();

	struct wait_queue_head *wq = signalfd_task_wq(cur);
	if (wq)
		poll_wait(pt, f, wq);
	return (events & POLLIN) && signalfd_any_pending(cur, &c->mask) ?
		       POLLIN :
		       0;
}

static void signalfd_release(vfs_file_t *f)
{
	struct signalfd_ctx *c = device_file_priv(f);
	if (c)
		kfree(c);
}

static const struct device_ops signalfd_ops = {
	.read = signalfd_read,
	.poll = signalfd_poll,
	.release = signalfd_release,
};

int64_t sys_signalfd4(uint64_t fd, uint64_t mask_ptr, uint64_t sizemask,
		      uint64_t flags)
{
	task_t *cur = sched_current();
	kernel_sigset_t mask;

	if (!cur)
		return -EFAULT;
	if (sizemask != sizeof(kernel_sigset_t))
		return -EINVAL;
	if (flags & ~(SFD_CLOEXEC | SFD_NONBLOCK))
		return -EINVAL;
	if (copy_from_user(&mask, (void *)mask_ptr, sizeof(mask)) != 0)
		return -EFAULT;
	sigdelset_k(&mask, SIGKILL);
	sigdelset_k(&mask, SIGSTOP);

	if ((int64_t)fd >= 0) {
		/* Replace the mask of an existing signalfd. */
		vfs_file_t *f = fdget(cur, (int)fd);
		if (!f)
			return -EBADF;
		if (device_file_ops(f) != &signalfd_ops) {
			fdput(f);
			return -EINVAL;
		}
		struct signalfd_ctx *c = device_file_priv(f);
		c->mask = mask;
		fdput(f);
		return (int64_t)fd;
	}

	struct signalfd_ctx *c = kalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->mask = mask;
	vfs_file_t *file = device_anon_file(&signalfd_ops, c,
					    "anon_inode:[signalfd]",
					    O_RDWR | ((flags & SFD_NONBLOCK) ?
							      O_NONBLOCK :
							      0));
	if (!file) {
		kfree(c);
		return -ENOMEM;
	}
	file->refcount = 1;
	int nfd = fd_install(cur, file);
	if (nfd < 0) {
		vfs_close(file);
		return nfd;
	}
	if (flags & SFD_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)nfd, FD_CLOEXEC);
	return nfd;
}

int64_t sys_signalfd(uint64_t fd, uint64_t mask_ptr, uint64_t sizemask)
{
	return sys_signalfd4(fd, mask_ptr, sizemask, 0);
}
