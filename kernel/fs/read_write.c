// LikeOS-64 -- read/write/readv/writev/lseek.
#include <kernel/ke/waitq.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/pipe.h>
#include <kernel/io/tty.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/fs/namei.h>

/* Release tasks blocked in select()/poll()/epoll_wait().  A blocking read or
 * write parks on the object itself and sched_wake_channel() releases it; a
 * task multiplexing several descriptors parks on the poll layer's own channel
 * and only this releases it before its one-tick fallback expires. */
extern void poll_notify_wq(struct wait_queue_head *);

// Pipe read/write helpers
static int64_t pipe_read_to_user(pipe_end_t *end, uint64_t buf, uint64_t count)
{
	if (!end || !end->pipe || !end->is_read) {
		return -EBADF;
	}
	pipe_t *pipe = end->pipe;

	if (count == 0) {
		return 0;
	}

	task_t *cur = sched_current();
	uint64_t flags;

	spin_lock_irqsave(&pipe->lock, &flags);

	// Block until data is available or all writers are gone
	while (pipe->used == 0) {
		if (pipe->writers == 0) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return 0; // EOF - no more writers
		}

		// Non-blocking mode: return EAGAIN immediately
		if (end->flags & O_NONBLOCK) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return -EAGAIN;
		}

		// Check for pending signals BEFORE blocking
		if (cur && signal_pending(cur)) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return -EINTR;
		}

		// Block waiting for data
		if (cur) {
			cur->state = TASK_BLOCKED;
			cur->wait_channel = pipe; // Wait on the pipe
			spin_unlock_irqrestore(&pipe->lock, flags);
			sched_schedule();
			spin_lock_irqsave(&pipe->lock, &flags);
			// NOTE: Do NOT set cur->state = TASK_READY here!
			// sched_schedule() already set us to TASK_RUNNING on return.
			// Overwriting with TASK_READY causes SMP double-scheduling.
			cur->wait_channel = NULL;

			// Check if we were woken by a signal
			if (signal_pending(cur)) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return -EINTR;
			}
		} else {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return -EAGAIN;
		}
	}

	size_t to_read = (count < pipe->used) ? count : pipe->used;
	size_t first = pipe->size - pipe->read_pos;
	if (first > to_read) {
		first = to_read;
	}

	// SMAP-aware copy to user buffer
	smap_disable();
	mm_memcpy((void *)buf, pipe->buffer + pipe->read_pos, first);
	if (to_read > first) {
		mm_memcpy((void *)(buf + first), pipe->buffer, to_read - first);
	}
	smap_enable();

	pipe->read_pos = (pipe->read_pos + to_read) % pipe->size;
	pipe->used -= to_read;

	spin_unlock_irqrestore(&pipe->lock, flags);

	// Wake up writers outside the lock, both the ones blocked on the pipe
	// itself and the ones multiplexing with select()/poll() -- those park
	// on the poll layer's channel and nothing here would otherwise release
	// them before its one-tick fallback expired.
	sched_wake_channel(pipe);
	poll_notify_wq(&pipe->poll_wq);

	return (int64_t)to_read;
}

static int64_t pipe_write_from_user(pipe_end_t *end, uint64_t buf,
				    uint64_t count)
{
	if (!end || !end->pipe || end->is_read) {
		return -EBADF;
	}
	pipe_t *pipe = end->pipe;

	if (count == 0) {
		return 0;
	}

	task_t *cur = sched_current();
	uint64_t flags;

	spin_lock_irqsave(&pipe->lock, &flags);

	if (pipe->readers == 0) {
		spin_unlock_irqrestore(&pipe->lock, flags);
		if (cur) {
			sched_signal_task(cur, SIGPIPE);
		}
		return -EPIPE;
	}

	// Block while pipe is full, waiting for readers to consume data
	while (pipe->used == pipe->size) {
		// Re-check readers (may have closed while we waited)
		if (pipe->readers == 0) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			if (cur) {
				sched_signal_task(cur, SIGPIPE);
			}
			return -EPIPE;
		}

		// Check for pending signals BEFORE blocking
		if (cur && signal_pending(cur)) {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return -EINTR;
		}

		// Block waiting for space
		if (cur) {
			cur->state = TASK_BLOCKED;
			cur->wait_channel = pipe;
			spin_unlock_irqrestore(&pipe->lock, flags);
			sched_schedule();
			spin_lock_irqsave(&pipe->lock, &flags);
			cur->wait_channel = NULL;

			// Check if we were woken by a signal
			if (signal_pending(cur)) {
				spin_unlock_irqrestore(&pipe->lock, flags);
				return -EINTR;
			}
		} else {
			spin_unlock_irqrestore(&pipe->lock, flags);
			return -EAGAIN;
		}
	}

	size_t space = pipe->size - pipe->used;
	size_t to_write = (count < space) ? count : space;
	size_t first = pipe->size - pipe->write_pos;
	if (first > to_write) {
		first = to_write;
	}

	// SMAP-aware copy from user buffer
	smap_disable();
	mm_memcpy(pipe->buffer + pipe->write_pos, (void *)buf, first);
	if (to_write > first) {
		mm_memcpy(pipe->buffer, (void *)(buf + first),
			  to_write - first);
	}
	smap_enable();

	pipe->write_pos = (pipe->write_pos + to_write) % pipe->size;
	pipe->used += to_write;
	WARN_ON(pipe->used >
		pipe->size); /* pipe ring buffer overflow: used > size after write, concurrent write without lock or size miscalculation */

	spin_unlock_irqrestore(&pipe->lock, flags);

	// Wake up any readers blocked on this pipe -- see the note in
	// pipe_read_to_user() for why the poll layer needs telling separately.
	sched_wake_channel(pipe);
	poll_notify_wq(&pipe->poll_wq);

	return (int64_t)to_write;
}

static int64_t read_held(task_t *cur, vfs_file_t *file, uint64_t buf,
			 uint64_t count);
static int64_t write_held(task_t *cur, vfs_file_t *file, uint64_t buf,
			  uint64_t count);

// SYS_READ - read from file descriptor
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
	might_sleep();
	task_t *cur = sched_current();
	BUG_ON(cur == NULL);
	if (!cur)
		return -EFAULT;

	// Security: Validate count to prevent excessive reads and overflow
	if (count == 0)
		return 0;
	if (count > (1024ULL * 1024 * 1024)) {
		return -EINVAL; // Max 1GB per read call
	}

	if (!validate_user_ptr(buf, count)) {
		return -EFAULT;
	}

	/* Demand paging shield: materialise lazy pages (and resolve COW) in
	 * the destination buffer NOW, before any FS/pipe/tty lock is taken —
	 * a page fault needing file I/O inside a lock-holding copy loop
	 * would deadlock. */
	mm_prefault_user_range(buf, count, 1);

	/* The console arm is decided by the descriptor being an unredirected
	 * standard one, which means an EMPTY slot -- so it is settled before
	 * any lookup, and a lookup would find nothing to hold anyway. */
	if (task_fd_is_console(cur, fd) && fd == STDIN_FD) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
		if (tty && tty->fg_pgid == 0) {
			tty->fg_pgid = cur->pgid;
		}
		if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid &&
		    (tty->term.c_lflag & ISIG)) {
			sched_signal_task(cur, SIGTTIN);
			return -EIO;
		}
		int nonblock = (cur->console_flags & O_NONBLOCK) ? 1 : 0;
		return tty_read(tty, (void *)buf, (long)count, nonblock);
	}
	/* From here the object is USED, so it is held for as long as that
	 * takes: every arm below can sleep, and a sibling thread closing this
	 * descriptor meanwhile must not be able to destroy what we are using.
	 * The single exit is what keeps the release paired with the hold. */
	{
		vfs_file_t *file = fdget(cur, (int)fd);
		int64_t ret;

		if (!file)
			return -EBADF;
		ret = read_held(cur, file, buf, count);
		fdput(file);
		return ret;
	}
}

/* The body of read(2), with the descriptor's object already held. */
static int64_t read_held(task_t *cur, vfs_file_t *file, uint64_t buf,
			 uint64_t count)
{
	// Check for console dup markers (magic pointers 1, 2, 3).
	// All three reference the same bidirectional /dev/console device,
	// so any of them can be read from (matches Unix dup-of-tty semantics).
	uint64_t marker = (uint64_t)file;
	if (marker >= 1 && marker <= 3) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
		if (tty && tty->fg_pgid == 0) {
			tty->fg_pgid = cur->pgid;
		}
		if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid &&
		    (tty->term.c_lflag & ISIG)) {
			sched_signal_task(cur, SIGTTIN);
			return -EIO;
		}
		int nonblock = (cur->console_flags & O_NONBLOCK) ? 1 : 0;
		return tty_read(tty, (void *)buf, (long)count, nonblock);
	}

	// UNIX socket fd - read via unix_recv
	if (unix_sock_is(file)) {
		return unix_recv((unix_socket_t *)file, (void *)buf,
				 (size_t)count, 0);
	}

	// Network socket fd - read via sock_recv
	if (IS_SOCKET_FD(file)) {
		return sock_recv(SOCKET_FD_IDX(file), (void *)buf,
				 (size_t)count, 0);
	}

	if (pipe_is_end(file)) {
		if (!validate_user_ptr(buf, count)) {
			return -EFAULT;
		}
		return pipe_read_to_user((pipe_end_t *)file, buf, count);
	}

	// Respect open flags (deny read on write-only)
	if (file->flags & O_WRONLY) {
		return -EBADF;
	}

	return vfs_read(file, (void *)buf, (long)count);
}

// SYS_WRITE - write to file descriptor

int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
	might_sleep();
	task_t *cur = sched_current();
	BUG_ON(cur == NULL);
	if (!cur)
		return -EFAULT;

	// Security: Validate count to prevent excessive writes and overflow
	if (count == 0)
		return 0;
	if (count > (1024ULL * 1024 * 1024)) {
		return -EINVAL; // Max 1GB per write call
	}

	if (!validate_user_ptr(buf, count)) {
		return -EFAULT;
	}

	/* Demand paging shield: fault-in the SOURCE buffer before FS/pipe/
	 * tty locks are taken (see sys_read).  Read-only touch: no COW. */
	mm_prefault_user_range(buf, count, 0);

	/* As in sys_read: an unredirected standard descriptor is an EMPTY
	 * slot, so this arm is settled without a lookup. */
	if (task_fd_is_console(cur, fd) && fd != STDIN_FD) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
		if (tty && tty->fg_pgid == 0) {
			tty->fg_pgid = cur->pgid;
		}
		if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid &&
		    (tty->term.c_lflag & TOSTOP)) {
			sched_signal_task(cur, SIGTTOU);
			return -EIO;
		}
		return tty_write(tty, (const void *)buf, (long)count);
	}
	/* Held across the dispatch below, for the reason given in sys_read. */
	{
		vfs_file_t *file = fdget(cur, (int)fd);
		int64_t ret;

		if (!file)
			return -EBADF;
		ret = write_held(cur, file, buf, count);
		fdput(file);
		return ret;
	}
}

/* The body of write(2), with the descriptor's object already held. */
static int64_t write_held(task_t *cur, vfs_file_t *file, uint64_t buf,
			  uint64_t count)
{
	// Check for console dup markers (magic pointers 1, 2, 3).
	// All three reference the same bidirectional /dev/console device.
	uint64_t marker = (uint64_t)file;
	if (marker >= 1 && marker <= 3) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
		if (tty && tty->fg_pgid == 0) {
			tty->fg_pgid = cur->pgid;
		}
		if (tty && tty->fg_pgid != 0 && tty->fg_pgid != cur->pgid &&
		    (tty->term.c_lflag & TOSTOP)) {
			sched_signal_task(cur, SIGTTOU);
			return -EIO;
		}
		return tty_write(tty, (const void *)buf, (long)count);
	}

	// UNIX socket fd - write via unix_send
	if (unix_sock_is(file)) {
		return unix_send((unix_socket_t *)file, (const void *)buf,
				 (size_t)count, 0);
	}

	// Network socket fd - write via sock_send
	if (IS_SOCKET_FD(file)) {
		return sock_send(SOCKET_FD_IDX(file), (const void *)buf,
				 (size_t)count, 0);
	}

	if (pipe_is_end(file)) {
		if (!validate_user_ptr(buf, count)) {
			return -EFAULT;
		}
		return pipe_write_from_user((pipe_end_t *)file, buf, count);
	}

	// Respect open flags (deny write on read-only)
	if ((file->flags & (O_WRONLY | O_RDWR | O_APPEND)) == 0) {
		return -EBADF;
	}

	// Write to filesystem if supported
	long wret = vfs_write(file, (const void *)buf, (long)count);
	if (wret < 0) {
		return wret;
	}
	/* A write by a non-privileged task drops the file's set-user/-group-ID
     * bits, so a set-id program can't outlive a change to its contents.  Root
     * (the whole live system) skips this entirely; for non-root the per-inode
     * clean hint keeps the steady state cheap (no per-write inode read). */
	if (wret > 0 && cur->cred.euid != 0 && !vfs_setid_clean(file))
		strip_setid_file(file);
	return wret;
}

int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// The console is not seekable.  Tested by slot CONTENT, not by
	// descriptor number: 0/1/2 are routinely redirected onto a regular
	// file, and lseek on that must work (a shell tracks its own read
	// offset on a redirected stdin this way).
	if (task_fd_is_console(cur, fd)) {
		return -ESPIPE;
	}

	vfs_file_t *file = fdget(cur, (int)fd);
	long result;

	if (!file) {
		return -EBADF;
	}

	/* Console dup markers, pipes, sockets and epoll instances are not
	 * seekable -- and must not be handed to vfs_seek, which would take the
	 * marker for a pointer. */
	if (fd_is_special(file)) {
		fdput(file);
		return -ESPIPE;
	}

	result = vfs_seek(file, (long)offset, (int)whence);
	fdput(file);

	if (result < 0) {
		return -EINVAL;
	}

	return result;
}

// POSIX writev(2) / readv(2) - scatter/gather I/O implemented as a loop
// over write(2) / read(2).  Per POSIX the implementation is allowed to
// process the iovecs sequentially; the only invariant is partial-write
// semantics on errors mid-stream.  Returns total bytes transferred.
struct k_iovec_compat {
	uint64_t iov_base;
	uint64_t iov_len;
};

int64_t sys_writev(uint64_t fd, uint64_t iovp, uint64_t iovcnt)
{
	if (iovcnt > 1024)
		return -EINVAL;
	if (!iovp)
		return -EFAULT;
	if (!validate_user_ptr(iovp, iovcnt * sizeof(struct k_iovec_compat)))
		return -EFAULT;
	int64_t total = 0;
	for (uint64_t i = 0; i < iovcnt; i++) {
		struct k_iovec_compat iov;
		if (copy_from_user(&iov, (void *)(iovp + i * sizeof(iov)),
				   sizeof(iov)) != 0)
			return total ? total : -EFAULT;
		if (iov.iov_len == 0)
			continue;
		int64_t r = sys_write(fd, iov.iov_base, iov.iov_len);
		if (r < 0)
			return total ? total : r;
		total += r;
		if ((uint64_t)r < iov.iov_len)
			break; // short write
	}
	return total;
}

int64_t sys_readv(uint64_t fd, uint64_t iovp, uint64_t iovcnt)
{
	if (iovcnt > 1024)
		return -EINVAL;
	if (!iovp)
		return -EFAULT;
	if (!validate_user_ptr(iovp, iovcnt * sizeof(struct k_iovec_compat)))
		return -EFAULT;
	int64_t total = 0;
	for (uint64_t i = 0; i < iovcnt; i++) {
		struct k_iovec_compat iov;
		if (copy_from_user(&iov, (void *)(iovp + i * sizeof(iov)),
				   sizeof(iov)) != 0)
			return total ? total : -EFAULT;
		if (iov.iov_len == 0)
			continue;
		int64_t r = sys_read(fd, iov.iov_base, iov.iov_len);
		if (r < 0)
			return total ? total : r;
		total += r;
		if (r == 0)
			break; // EOF
		if ((uint64_t)r < iov.iov_len)
			break; // short read
	}
	return total;
}
