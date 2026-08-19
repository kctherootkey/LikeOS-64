// LikeOS-64 -- the file-descriptor table and fd-surgery syscalls.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>

static int64_t sys_dup_from(uint64_t oldfd, int from);

/* The descriptor table is SHARED between the threads of a process
 * (CLONE_FILES), so the lock below serialises slot bookkeeping across them.
 * A task that owns its table privately has no files_struct and needs none. */
void fds_lock(task_t *task, uint64_t *flags)
{
	if (task->files)
		spin_lock_irqsave(&task->files->lock, flags);
}

void fds_unlock(task_t *task, uint64_t flags)
{
	if (task->files)
		spin_unlock_irqrestore(&task->files->lock, flags);
}

/* Install `file` in the lowest free descriptor >= `from` and return it, or
 * -EMFILE (the caller still owns `file` then).  0/1/2 are eligible once the
 * process has closed them -- POSIX defines "lowest free descriptor" over the
 * whole table, and the classic way to put a terminal on stdin is close(0)
 * followed by open() or dup() landing back on 0.  Refusing to allocate there
 * left the pty on fd 3 and the program's stdio still on whatever it inherited
 * (this is what gave xterm's shell no prompt: its output went to the console).
 *
 * Claiming the slot and storing the object are ONE atomic step on purpose.
 * The old split — scan for a free number, then open the file, then store it —
 * handed the same number to two threads of the same process opening
 * concurrently, and one of the two objects was simply lost.  The object is
 * always built BEFORE this call, so nothing sleeps under the lock and no
 * half-installed slot is ever visible to another thread. */
int fd_install_from(task_t *task, vfs_file_t *file, int from)
{
	if (from < 0)
		from = 0;
	uint64_t flags = 0;
	fds_lock(task, &flags);
	struct vfs_file **fds = task_fds(task);
	int ret = -EMFILE;
	for (int i = from; i < TASK_MAX_FDS; i++) {
		if (task_fd_slot_free(task, (unsigned)i)) {
			fds[i] = file;
			/* A freed slot keeps its old flag byte, so clear it
			 * here rather than inheriting a stale FD_CLOEXEC --
			 * and, for 0/1/2, rather than staying marked closed. */
			task_set_fd_flags(task, (unsigned)i, 0);
			ret = i;
			break;
		}
	}
	fds_unlock(task, flags);
	return ret;
}

int fd_install(task_t *task, vfs_file_t *file)
{
	return fd_install_from(task, file, 0);
}

/* Take one more reference on whatever kind of thing `entry` is and return the
 * value to store in the new slot.  Console markers and epoll handles are not
 * refcounted and duplicate as themselves; a pipe end gets its own end object.
 * Returns NULL only when a pipe end could not be allocated. */
vfs_file_t *fd_dup_entry(vfs_file_t *entry)
{
	uint64_t marker = (uint64_t)entry;
	if (marker >= 1 && marker <= 3)
		return entry; /* console stdio marker */
	if (IS_SOCKET_FD(entry)) {
		net_socket_t *s = sock_get(SOCKET_FD_IDX(entry));
		if (s)
			__atomic_fetch_add(&s->ref_count, 1, __ATOMIC_ACQ_REL);
		return entry;
	}
	if (unix_sock_is(entry)) {
		/* Both counters together: the descriptor count that decides
		 * the hangup, and the reference that keeps the socket alive
		 * for as long as any descriptor names it. */
		unix_sock_fdget((unix_socket_t *)entry);
		return entry;
	}
	if (IS_EPOLL_FD(entry)) {
		/* The marker is the same value in both slots, but each slot is
		 * a descriptor and each descriptor holds a reference. */
		epoll_get(EPOLL_FD_IDX(entry));
		return entry;
	}
	if (pipe_is_end(entry))
		return (vfs_file_t *)pipe_dup_end((pipe_end_t *)entry);
	return vfs_dup(entry);
}

/* Drop the reference held by one descriptor slot.  Mirror of fd_dup_entry.
 *
 * Not static: a filesystem or socket object that has queued a descriptor of
 * its own -- an in-band descriptor sent over a socket and never received --
 * has to release it when it is destroyed, and it cannot know what kind of
 * thing the queued entry is. */
void fd_release_entry(vfs_file_t *entry)
{
	uint64_t marker = (uint64_t)entry;
	if (!entry || (marker >= 1 && marker <= 3))
		return; /* console stdio marker — nothing to release */
	if (IS_SOCKET_FD(entry)) {
		sock_close(SOCKET_FD_IDX(entry));
		return;
	}
	if (unix_sock_is(entry)) {
		unix_close((unix_socket_t *)entry);
		return;
	}
	if (IS_EPOLL_FD(entry)) {
		/* Drop THIS descriptor's reference.  The instance is global and
		 * the descriptor is not: marking it inactive here tore it down
		 * for every other process that had inherited the fd. */
		epoll_put(EPOLL_FD_IDX(entry));
		return;
	}
	if (pipe_is_end(entry)) {
		pipe_close_end((pipe_end_t *)entry);
		return;
	}
	vfs_close(entry);
}

/* Read descriptor `fd` and take a reference on whatever it holds, with the
 * read and the reference-take in ONE locked region.
 *
 * fd_dup_entry() alone is not enough, because its caller has to read the slot
 * first: between that read and the reference being taken, a sibling thread
 * sharing this descriptor table can close the same descriptor and drop the
 * last reference.  The duplicate then names an object that is already being
 * destroyed.  While a socket was identified by a table index that was merely
 * re-checked for liveness, this was survivable; once it is a counted object,
 * this is the one remaining hole through which a dead one can be brought back.
 *
 * Every kind but one takes its reference with a single atomic, so the whole
 * operation fits inside the lock.  A pipe end is the exception: duplicating it
 * allocates a second end object, and the allocator must never run with this
 * lock held -- it can unmap a slab page and wait for every processor to
 * acknowledge the shootdown, which a processor spinning here with interrupts
 * off cannot do.  So that case allocates unlocked and then re-reads the slot
 * to confirm the descriptor still names the same object, discarding the
 * duplicate if it does not.  That check narrows the window rather than closing
 * it -- reading the candidate's magic is itself unsynchronised -- which is
 * exactly the behaviour pipes have today, and is left for a separate change.
 */
vfs_file_t *fd_dup_entry_at(task_t *cur, int fd)
{
	uint64_t flags;
	vfs_file_t *entry;
	vfs_file_t *copy = NULL;

	if (!cur || fd < 0 || fd >= TASK_MAX_FDS)
		return NULL;

	fds_lock(cur, &flags);
	entry = task_fds(cur)[fd];
	if (entry && !pipe_is_end(entry))
		copy = fd_dup_entry(entry);
	fds_unlock(cur, flags);

	if (copy || !entry)
		return copy;

	copy = fd_dup_entry(entry);
	if (!copy)
		return NULL;

	fds_lock(cur, &flags);
	int same = (task_fds(cur)[fd] == entry);
	fds_unlock(cur, flags);
	if (!same) {
		fd_release_entry(copy);
		return NULL;
	}
	return copy;
}

/* True for descriptors not backed by a real vfs_file_t: the stdio console dup
 * markers (1-3), pipe ends, epoll instances, and network / UNIX sockets.  These
 * carry no on-disk metadata, so fchmod/fchown/fsync are no-ops on them — and,
 * importantly, their "pointer" must never be dereferenced as a vfs_file_t.
 *
 * EVERY fd syscall that reaches into the VFS has to ask this first.  They did
 * not, and they did not agree with each other about which markers to look for:
 * fstat() checked none of them and took the kernel down on an ordinary fstat of
 * an AF_UNIX socket (0x30009 = UNIX_SOCKET_FD_BASE + 9), lseek() caught the
 * console markers but not sockets, ftruncate() checked nothing at all, and
 * epoll descriptors were absent from this predicate entirely. */
int fd_is_special(vfs_file_t *file)
{
	uint64_t marker = (uint64_t)file;
	if (marker >= 1 && marker <= 3)
		return 1;
	if (unix_sock_is(file))
		return 1;
	if (IS_SOCKET_FD(file))
		return 1;
	if (IS_EPOLL_FD(file))
		return 1;
	if (pipe_is_end(file))
		return 1;
	return 0;
}

int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (fd >= TASK_MAX_FDS)
		return -EBADF;

	/* F_GETFD/F_SETFD are per-descriptor-slot flags stored in fd_flags[].
	 * They must still REJECT a descriptor that is not open: programs probe
	 * with fcntl(fd, F_GETFD, 0) == -1 to find out whether a descriptor
	 * exists (a shell does this to decide whether a redirection has to
	 * save the previous descriptor or merely close it afterwards).
	 * Answering "open" for every slot below the table size sent bash down
	 * the save path for a closed fd, where the following F_DUPFD failed
	 * with EBADF and aborted the whole redirection. */
	if (cmd == F_GETFD || cmd == F_SETFD) {
		if (!task_fds(cur)[fd] && !task_fd_is_console(cur, fd))
			return -EBADF;
		if (cmd == F_GETFD)
			/* Masked: FD_STDIO_CLOSED shares this byte and is
			 * kernel-internal, not part of the fd flags POSIX
			 * defines. */
			return (int64_t)(task_get_fd_flags(cur, (unsigned)fd) &
					 FD_CLOEXEC);
		task_set_fd_flags(cur, (unsigned)fd,
				  (uint8_t)((uint32_t)arg & FD_CLOEXEC));
		return 0;
	}

	/* Advisory record locks.  Placed here, ahead of the per-descriptor-type
	 * blocks, because locking is a property of the FILE and applies to any
	 * descriptor that has one -- and because the marker descriptors below
	 * (sockets, epoll, pipes) have no file to lock and must fall through to
	 * their own handling rather than reach this. */
	if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
		vfs_file_t *lf = task_fds(cur)[fd];
		if (!lf || IS_SOCKET_FD(lf) || unix_sock_is(lf) ||
		    IS_EPOLL_FD(lf) || pipe_is_end(lf) || (uintptr_t)lf <= 3)
			return -EBADF;
		k_flock_t kfl;
		if (!arg || !validate_user_ptr(arg, sizeof(kfl)))
			return -EFAULT;
		if (copy_from_user(&kfl, (void *)arg, sizeof(kfl)) != 0)
			return -EFAULT;
		int fr = frlock_fcntl(lf, (int)cmd, &kfl, cur);
		if (fr == 0 && cmd == F_GETLK &&
		    copy_to_user((void *)arg, &kfl, sizeof(kfl)) != 0)
			return -EFAULT;
		return fr;
	}

	/* F_DUPFD/F_DUPFD_CLOEXEC: duplicate onto the lowest free descriptor
	 * >= arg.  Handled here, ahead of the per-descriptor-type blocks
	 * below, so it works for EVERY kind of descriptor - sys_dup2 already
	 * knows how to duplicate console markers, sockets, pipes and files.
	 * A shell saves a standard descriptor this way (fcntl(1, F_DUPFD, 10))
	 * before pointing it somewhere else for a builtin, so without this
	 * every redirection in the current shell fails. */
	if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
		if ((int64_t)arg < 0 || arg >= TASK_MAX_FDS)
			return -EINVAL;
		/* Source must be open.  0/1/2 stay open as console markers
		 * even when their fd_table slot is NULL. */
		if (!task_fds(cur)[fd] && !task_fd_is_console(cur, fd))
			return -EBADF;
		int64_t newfd = sys_dup_from(fd, (int)arg);
		if (newfd < 0)
			return newfd;
		/* POSIX: the copy does NOT inherit FD_CLOEXEC; F_DUPFD clears
		 * it (sys_dup_from already did) and F_DUPFD_CLOEXEC sets it. */
		if (cmd == F_DUPFD_CLOEXEC)
			task_set_fd_flags(cur, (unsigned)newfd, FD_CLOEXEC);
		return newfd;
	}

	vfs_file_t *file = task_fds(cur)[fd];

	// Handle console markers: only when fd_table entry is NULL
	if (task_fd_is_console(cur, fd)) {
		if (cmd == F_GETFL) {
			uint32_t fl = (fd == STDIN_FD) ? O_RDONLY : O_WRONLY;
			fl |= (cur->console_flags & O_NONBLOCK);
			return fl;
		}
		if (cmd == F_SETFL) {
			cur->console_flags =
				(cur->console_flags & ~O_NONBLOCK) |
				((uint32_t)arg & O_NONBLOCK);
			return 0;
		}
		return -EINVAL;
	}

	if (!file)
		return -EBADF;

	// Console markers stored in fd_table by dup2 (oldfd+1: 1=stdin, 2=stdout, 3=stderr)
	{
		uintptr_t mv = (uintptr_t)file;
		if (mv >= 1 && mv <= 3) {
			if (cmd == F_GETFL) {
				uint32_t fl = (mv == 1) ? O_RDONLY : O_WRONLY;
				fl |= (cur->console_flags & O_NONBLOCK);
				return fl;
			}
			if (cmd == F_SETFL) {
				cur->console_flags =
					(cur->console_flags & ~O_NONBLOCK) |
					((uint32_t)arg & O_NONBLOCK);
				return 0;
			}
			return -EINVAL;
		}
	}

	// Socket fd markers
	if (IS_SOCKET_FD(file)) {
		int idx = SOCKET_FD_IDX(file);
		return sock_fcntl_net(idx, (int)cmd, (unsigned long)arg);
	}

	// UNIX socket fd markers
	if (unix_sock_is(file)) {
		unix_socket_t *us = (unix_socket_t *)file;
		if (!us)
			return -EBADF;
		if (cmd == F_GETFL) {
			uint32_t fl = O_RDWR;
			if (us->nonblock)
				fl |= O_NONBLOCK;
			return fl;
		}
		if (cmd == F_SETFL) {
			us->nonblock = ((uint32_t)arg & O_NONBLOCK) ? 1 : 0;
			return 0;
		}
		return -EINVAL;
	}

	// Epoll fd markers
	if (IS_EPOLL_FD(file)) {
		if (cmd == F_GETFL)
			return O_RDWR;
		return -EINVAL;
	}

	if (pipe_is_end(file)) {
		pipe_end_t *end = (pipe_end_t *)file;
		if (cmd == 3) {
			uint32_t fl = end->is_read ? O_RDONLY : O_WRONLY;
			if (end->flags & O_NONBLOCK)
				fl |= O_NONBLOCK;
			return fl;
		}
		if (cmd == 4) {
			end->flags = (end->flags & ~O_NONBLOCK) |
				     ((uint32_t)arg & O_NONBLOCK);
			return 0;
		}
		return -EINVAL;
	}

	if (cmd == 3) { // F_GETFL
		return file->flags;
	}
	if (cmd == 4) { // F_SETFL
		// POSIX: F_SETFL can change O_APPEND, O_NONBLOCK, O_ASYNC, O_DIRECT,
		// O_NOATIME.  Access mode and creation flags are ignored.
		const uint32_t SETTABLE = O_APPEND | O_NONBLOCK;
		file->flags =
			(file->flags & ~SETTABLE) | ((uint32_t)arg & SETTABLE);
		return 0;
	}
	return -EINVAL;
}

// SYS_DUP - duplicate file descriptor
int64_t sys_dup(uint64_t oldfd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	/* Floor 0, not 3: POSIX defines dup() as "the lowest numbered
	 * available file descriptor", and close(fd)+dup(x) is the classic way
	 * to put x on a standard descriptor. */
	return sys_dup_from(oldfd, 0);
}

/* dup(oldfd) onto the lowest free descriptor >= `from`.  Shared by SYS_DUP
 * and fcntl(F_DUPFD), which differ only in that floor. */
static int64_t sys_dup_from(uint64_t oldfd, int from)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	/* Console-marker shortcut - ONLY when the descriptor really still is
	 * the console.  fds 0-2 are routinely redirected (a shell points
	 * stdout at a pipe for command substitution, then saves and restores
	 * it around a builtin's own redirection); taking this branch on the
	 * fd NUMBER alone duplicated a console marker instead of the pipe, so
	 * the restore handed stdout back to the terminal and the captured
	 * output was lost.  A non-NULL slot falls through to the normal
	 * per-type duplication below. */
	if (task_fd_is_console(cur, oldfd))
		return fd_install_from(cur, (vfs_file_t *)(oldfd + 1), from);

	if (oldfd >= TASK_MAX_FDS || task_fds(cur)[oldfd] == NULL)
		return -EBADF;

	/* Take the reference BEFORE claiming a slot, so the install step stays
	 * a single locked store (and so a failed install releases cleanly).
	 * Read and reference together, so a sibling thread closing this same
	 * descriptor cannot slip between the two. */
	vfs_file_t *copy = fd_dup_entry_at(cur, (int)oldfd);
	if (!copy)
		return -EBADF; /* closed under us, or a pipe end would not dup */

	/* fd_install_from clears FD_CLOEXEC on the new slot, which is what
	 * POSIX requires of a duplicate — and what a slot recycled from a
	 * close-on-exec descriptor would otherwise have kept, making the copy
	 * vanish across the next exec. */
	int newfd = fd_install_from(cur, copy, from);
	if (newfd < 0) {
		fd_release_entry(copy);
		return newfd;
	}
	return newfd;
}

// SYS_DUP2 - duplicate file descriptor to specific fd
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (newfd >= TASK_MAX_FDS)
		return -EBADF;
	if (oldfd == newfd)
		return newfd;

	// POSIX: validate oldfd BEFORE touching newfd — "if oldfd is not a
	// valid file descriptor, the call fails and newfd is not closed".
	// fds 0-2 count as valid even when they hold console markers.
	if (!task_fd_is_console(cur, oldfd) &&
	    (oldfd >= TASK_MAX_FDS || task_fds(cur)[oldfd] == NULL))
		return -EBADF;

	/* Build the duplicate BEFORE closing newfd: a dup that cannot be made
	 * (a pipe end that fails to allocate) must not have destroyed the
	 * descriptor it was going to overwrite.
	 *
	 * Console-marker shortcut - ONLY when the descriptor really still is
	 * the console.  fds 0-2 are routinely redirected (a shell points
	 * stdout at a pipe for command substitution, then saves and restores
	 * it around a builtin's own redirection); taking this branch on the
	 * fd NUMBER alone duplicated a console marker instead of the pipe, so
	 * the restore handed stdout back to the terminal and the captured
	 * output was lost. */
	vfs_file_t *copy;
	if (task_fd_is_console(cur, oldfd)) {
		copy = (vfs_file_t *)(oldfd + 1); /* console stdio marker */
	} else {
		copy = fd_dup_entry_at(cur, (int)oldfd);
		if (!copy)
			return -EBADF;
	}

	// POSIX: dup2 implicitly closes newfd if it is open — INCLUDING fds
	// 0-2.  In a pty session those hold real refcounted vfs files (the
	// pts slave), not console markers; the old `newfd >= 3` guard (there
	// only because sys_close refuses fds 0-2) leaked one slave reference
	// every time a pipeline child dup2'ed a pipe end over its stdio.
	// Enough leaked references kept the slave alive after every process
	// on the pty had exited, the master never saw POLLHUP, and a tmux
	// window that had ever run a pipeline could not close.
	uint64_t lflags = 0;
	fds_lock(cur, &lflags);
	vfs_file_t *old = task_fds(cur)[newfd];
	task_fds(cur)[newfd] = copy;
	/* POSIX: the copy never inherits FD_CLOEXEC — dup2 always leaves the
	 * flag clear on newfd.  Set only here, AFTER oldfd was validated, so a
	 * failed dup2 (bad oldfd) leaves an open newfd's flags untouched.
	 * Without it a slot that previously held a close-on-exec descriptor
	 * kept the bit and the duplicate disappeared across the next exec. */
	task_set_fd_flags(cur, (unsigned)newfd, 0);
	fds_unlock(cur, lflags);
	/* Released last, and only after the slot already points at the copy:
	 * fd_release_entry can sleep (vfs_close → pagecache flush), so newfd
	 * must never be observable as empty in between. */
	fd_release_entry(old);
	return newfd;
}

// SYS_DUP3 - duplicate file descriptor with flags
int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags)
{
	if (oldfd == newfd)
		return -EINVAL;
	int64_t r = sys_dup2(oldfd, newfd);
	if (r >= 0) {
		task_t *c = sched_current();
		if (c)
			task_set_fd_flags(c, (unsigned)newfd,
					  (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
	}
	return r;
}
