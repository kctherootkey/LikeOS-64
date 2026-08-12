// LikeOS-64 System Call Handler
#include <kernel/io/console.h>
#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/dev/input/keyboard.h>
#include <kernel/fs/vfs.h>
#include <kernel/uapi/status.h>
#include <kernel/ke/elf.h>
#include <kernel/ke/script_loader.h>
#include <kernel/ke/pipe.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/stat.h>
#include <kernel/io/tty.h>
#include <kernel/ke/signal.h>
#include <kernel/fs/devfs.h>
#include <kernel/mm/shm.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/uapi/dirent.h>
#include <kernel/hal/serial.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/futex.h>
#include <kernel/hal/acpi.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/net/net.h>
#include <kernel/hal/lapic.h>

/* Release tasks blocked in select()/poll()/epoll_wait().  A blocking read or
 * write parks on the object itself and sched_wake_channel() releases it; a
 * task multiplexing several descriptors parks on the poll layer's own channel
 * and only this releases it before its one-tick fallback expires. */
extern void poll_notify_io_ready(void);
#include <kernel/dev/rand/random.h>
#include <kernel/uapi/bug.h>

// Validate user pointer is in user space
static bool validate_user_ptr(uint64_t ptr, size_t len)
{
	if (ptr < 0x10000)
		return false; // Reject low addresses (NULL deref protection)
	if (ptr >= 0x7FFFFFFFFFFF)
		return false; // Beyond user space
	if (ptr + len < ptr)
		return false; // Overflow check
	return true;
}

// SMAP-aware copy from user space to kernel space
// Returns 0 on success, -EFAULT on failure
static int copy_from_user(void *kernel_dst, const void *user_src, size_t len)
{
	/* Destination must be a kernel address, not user space */
	WARN_ON((uint64_t)kernel_dst < 0x8000000000000000UL &&
		(uint64_t)kernel_dst >= 0x1000UL);
	if (!validate_user_ptr((uint64_t)user_src, len)) {
		return -EFAULT;
	}
	if (!kernel_dst || len == 0) {
		return (len == 0) ? 0 : -EFAULT;
	}

	/* Same kill-mid-syscall safety net as in copy_to_user — see the long
     * comment there.  If the current task can't own user memory (zombied,
     * exited, or pml4 freed), CR3 holds the kernel-only PML4 and any
     * dereference of user_src would page-fault into kernel_oops.  Return
     * -EFAULT so the caller's normal error path runs instead. */
	{
		task_t *cur = sched_current();
		if (!cur || cur->privilege != TASK_USER || cur->pml4 == NULL ||
		    cur->has_exited || cur->state == TASK_ZOMBIE) {
			return -EFAULT;
		}
	}

	// Temporarily allow supervisor access to user pages (SMAP bypass)
	smap_disable();
	mm_memcpy(kernel_dst, user_src, len);
	// Re-enable SMAP protection
	smap_enable();
	return 0;
}

// SMAP-aware copy from kernel space to user space
// Returns 0 on success, -EFAULT on failure
static int copy_to_user(void *user_dst, const void *kernel_src, size_t len)
{
	/* Destination must be a user-space address */
	WARN_ON((uint64_t)user_dst >= 0x8000000000000000UL);
	if (!validate_user_ptr((uint64_t)user_dst, len)) {
		return -EFAULT;
	}
	if (!kernel_src || len == 0) {
		return (len == 0) ? 0 : -EFAULT;
	}

	/* Safety net for the "task was killed mid-syscall" race:
     *
     * If a user task is SIGKILL'd (or otherwise zombied) while suspended
     * inside a syscall via sched_schedule(), its pml4 may have been freed
     * by mm_destroy_address_space() on the killer's CPU.  The scheduler's
     * switch_address_space() then falls back to g_kernel_pml4 on resume
     * (cur->pml4 ? cur->pml4 : g_kernel_pml4), so CR3 holds the kernel-only
     * PML4 with no user mappings.  The original syscall handler still has
     * the user pointer in registers and calls copy_to_user() — which would
     * page-fault in kernel mode on a user address (cr2 < kernel base),
     * with current_task pointing to a kernel-thread fallback so
     * exception_handler can't even route it to SIGSEGV: that path
     * matches "kernel-mode + user addr + current_task is kernel thread",
     * goes to kernel_oops, and we lose the whole system.
     *
     * sched_schedule()'s zombie self-check above is the primary guard;
     * this is a backstop for any path that somehow reaches us with a
     * current task that can't own user memory (no pml4, exited, or
     * zombied since the syscall began).  Fail with -EFAULT — the caller
     * just sees a normal copy failure instead of an oops. */
	{
		task_t *cur = sched_current();
		if (!cur || cur->privilege != TASK_USER || cur->pml4 == NULL ||
		    cur->has_exited || cur->state == TASK_ZOMBIE) {
			return -EFAULT;
		}
	}

	// Temporarily allow supervisor access to user pages (SMAP bypass)
	smap_disable();
	mm_memcpy(user_dst, kernel_src, len);
	// Re-enable SMAP protection
	smap_enable();
	return 0;
}

// Safe string length (bounded) from user space (SMAP-aware)
static int user_strnlen(const char *user_str, size_t max_len, size_t *out_len)
{
	if (!user_str || !out_len) {
		return -EFAULT;
	}
	// Validate entire potential range first
	if (!validate_user_ptr((uint64_t)user_str, max_len)) {
		return -EFAULT;
	}
	// Temporarily allow user memory access
	smap_disable();
	size_t i;
	for (i = 0; i < max_len; i++) {
		if (user_str[i] == '\0') {
			*out_len = i;
			smap_enable();
			return 0;
		}
	}
	smap_enable();
	return -EINVAL; // Too long
}

static int copy_user_string(const char *user_str, size_t max_len,
			    char **out_str, size_t *out_len)
{
	if (!user_str || !out_str) {
		return -EFAULT;
	}

	size_t len = 0;
	int ret = user_strnlen(user_str, max_len, &len);
	if (ret != 0) {
		return ret;
	}

	char *kstr = (char *)kalloc(len + 1);
	if (!kstr) {
		return -ENOMEM;
	}
	// Use copy_from_user for SMAP-aware copy
	if (copy_from_user(kstr, user_str, len) != 0) {
		kfree(kstr);
		return -EFAULT;
	}
	kstr[len] = '\0';

	*out_str = kstr;
	if (out_len) {
		*out_len = len;
	}
	return 0;
}

// Helper: Copy user path string directly into fixed kernel buffer (no allocation)
// Returns 0 on success, negative error on failure
static int copy_user_path(const char *user_path, char *kbuf, size_t kbuf_size)
{
	if (!user_path || !kbuf || kbuf_size < 2) {
		return -EINVAL;
	}
	char *kstr = NULL;
	size_t len = 0;
	int ret = copy_user_string(user_path, kbuf_size - 1, &kstr, &len);
	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0; i <= len; i++) {
		kbuf[i] = kstr[i];
	}
	kfree(kstr);
	return 0;
}

static void free_user_string_array(char **arr)
{
	if (!arr) {
		return;
	}
	for (size_t i = 0; arr[i]; i++) {
		kfree(arr[i]);
	}
	kfree(arr);
}

static int copy_user_string_array(const char *const *user_arr, size_t max_count,
				  size_t max_str_len, size_t max_total_bytes,
				  char ***out_arr)
{
	if (!out_arr) {
		return -EFAULT;
	}
	*out_arr = NULL;

	if (!user_arr) {
		return 0;
	}

	if (!validate_user_ptr((uint64_t)user_arr, sizeof(uint64_t))) {
		return -EFAULT;
	}

	char **karr = (char **)kalloc((max_count + 1) * sizeof(char *));
	if (!karr) {
		return -ENOMEM;
	}
	mm_memset(karr, 0, (max_count + 1) * sizeof(char *));

	size_t total = 0;
	for (size_t i = 0; i < max_count; i++) {
		// SMAP-aware read of user array element
		const char *user_str;
		smap_disable();
		user_str = user_arr[i];
		smap_enable();
		if (!user_str) {
			karr[i] = NULL;
			*out_arr = karr;
			return 0;
		}
		if (!validate_user_ptr((uint64_t)user_arr +
					       (i * sizeof(uint64_t)),
				       sizeof(uint64_t))) {
			free_user_string_array(karr);
			return -EFAULT;
		}

		char *kstr = NULL;
		size_t len = 0;
		int ret = copy_user_string(user_str, max_str_len, &kstr, &len);
		if (ret != 0) {
			free_user_string_array(karr);
			return ret;
		}

		total += len + 1;
		if (total > max_total_bytes) {
			kfree(kstr);
			free_user_string_array(karr);
			return -EINVAL;
		}

		karr[i] = kstr;
	}

	free_user_string_array(karr);
	return -EINVAL; // Too many entries
}

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
	poll_notify_io_ready();

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
	poll_notify_io_ready();

	return (int64_t)to_write;
}

/* The descriptor table is SHARED between the threads of a process
 * (CLONE_FILES), so the lock below serialises slot bookkeeping across them.
 * A task that owns its table privately has no files_struct and needs none. */
static void fds_lock(task_t *task, uint64_t *flags)
{
	if (task->files)
		spin_lock_irqsave(&task->files->lock, flags);
}

static void fds_unlock(task_t *task, uint64_t flags)
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
static int fd_install_from(task_t *task, vfs_file_t *file, int from)
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

static int fd_install(task_t *task, vfs_file_t *file)
{
	return fd_install_from(task, file, 0);
}

/* Take one more reference on whatever kind of thing `entry` is and return the
 * value to store in the new slot.  Console markers and epoll handles are not
 * refcounted and duplicate as themselves; a pipe end gets its own end object.
 * Returns NULL only when a pipe end could not be allocated. */
static vfs_file_t *fd_dup_entry(vfs_file_t *entry)
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
static vfs_file_t *fd_dup_entry_at(task_t *cur, int fd)
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

// Forward declarations for helper syscalls used before definition
static int64_t sys_getpid(void);
static void sys_exit(uint64_t status);

// Minimal uname struct (kernel-side)
typedef struct {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
} k_utsname_t;

typedef struct {
	long tv_sec;
	long tv_usec;
} k_timeval_t;

/* The mmap region table lives in the mm layer: mm_find_mmap_region(),
 * mm_alloc_mmap_region() and mm_unmap_range_and_regions() (kernel/mm/memory.c,
 * declared in kernel/mm/memory.h). */

// SYS_READ - read from file descriptor
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
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

	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}
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
	if (!file) {
		return -EBADF;
	}

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
/* Set-id stripping after a non-privileged modify (defined below with the
 * permission helpers). */
static unsigned setid_strip_bits(uint32_t mode);
static void strip_setid_file(vfs_file_t *file);

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
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

	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}
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
	if (!file) {
		return -EBADF;
	}

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

static int build_at_path(task_t *cur, int dirfd, const char *path, char *out,
			 size_t out_size);
static int normalize_path(const char *base, const char *path, char *out,
			  size_t out_size);

/*
 * Resolve a path against THIS task's working directory (and chroot), in place.
 *
 * Every syscall that names a file has to do this before the VFS sees the name.
 * A relative path that gets through unresolved is resolved much further down,
 * against a single "current directory" that the entire system shares -- so it
 * names whatever directory some other process happened to change into last.
 * The result is a call that operates on a completely different file than the
 * caller meant, or reports that a file it had just successfully stat'd does not
 * exist.  Rename was the one that showed it: a mail client found its
 * configuration directory, failed to rename it, and refused to start.
 */
static int canon_task_path(char *path, size_t size)
{
	task_t *cur = sched_current();
	char full[VFS_MAX_PATH];
	int ret;
	size_t i;

	if (!cur || !path || size < 2)
		return -EINVAL;

	ret = build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
	if (ret != 0)
		return ret;

	for (i = 0; i + 1 < size && full[i]; i++)
		path[i] = full[i];
	path[i] = '\0';
	/* Truncating would name a different file; refuse instead. */
	if (full[i] != '\0')
		return -ENAMETOOLONG;
	return 0;
}

// Convert VFS status codes to negative errno values
static int vfs_status_to_errno(int st)
{
	switch (st) {
	case ST_NOT_FOUND:
		return -ENOENT;
	case ST_NOMEM:
		return -ENOMEM;
	case ST_INVALID:
		return -EINVAL;
	case ST_IO:
		return -EIO;
	case ST_EXISTS:
		return -EEXIST;
	case ST_BUSY:
		return -EBUSY;
	case ST_AGAIN:
		return -EAGAIN;
	case ST_NOTEMPTY:
		return -ENOTEMPTY;
	case ST_ROFS:
		return -EROFS;
	case ST_NOSPC:
		return -ENOSPC;
	case ST_NODATA:
		return -ENODATA;
	case ST_RANGE:
		return -ERANGE;
	case ST_UNSUPPORTED:
		return -EOPNOTSUPP;
	case ST_ACCESS:
		return -EACCES;
	case ST_PERM:
		return -EPERM;
	default:
		return -EACCES;
	}
}

/* ---- Permission checks: thin adapters over the canonical VFS policy --------
 * The discretionary-access policy lives in the VFS now (vfs_permission and
 * friends — the one place every filesystem shares), so these are just adapters
 * that translate the VFS's ST_ result into the negative-errno the syscalls
 * return.  They let a credential-sensitive syscall screen an operation and
 * report a precise errno; the VFS re-checks authoritatively when the operation
 * actually runs, so removing any of these pre-checks would not weaken security. */
static int perm_st_errno(int st)
{
	return (st == ST_OK) ? 0 : vfs_status_to_errno(st);
}

/* Access check for a file whose stat is `st`, against the ACL then mode bits.
 * use_real selects the real vs effective/fs ids. */
static int perm_access(task_t *cur, const char *path, const struct kstat *st,
		       int want, int use_real)
{
	(void)cur; /* the VFS reads the current task's credentials itself */
	return perm_st_errno(vfs_check_access(path, st, want, use_real));
}

/* Search (x) permission on every ancestor directory of `path` (effective ids). */
static int perm_traverse(const char *rawpath)
{
	return perm_st_errno(vfs_permission_traverse(rawpath));
}

/* Like perm_traverse but with an explicit real(1)/effective(0) id selection,
 * for access(2)/faccessat which screen the prefix with the real ids. */
static int perm_traverse_cred(const char *rawpath, int use_real)
{
	return perm_st_errno(vfs_access_traverse(rawpath, use_real));
}

/* Write+search on the PARENT directory of `path` (create/remove/rename). */
static int perm_check_parent(const char *rawpath, int want)
{
	return perm_st_errno(vfs_permission_parent(rawpath, want));
}

/* Remove/rename gate: parent write+search plus the directory's sticky-bit rule.
 * The sticky-bit ownership check now lives in the VFS (vfs_permission_remove). */
static int perm_check_remove(const char *rawpath)
{
	return perm_st_errno(vfs_permission_remove(rawpath));
}

/* The set-user/-group-ID bits a successful modification (write/chown) by a
 * non-privileged caller must clear, to stop a set-id file outliving a change to
 * its contents or ownership.  S_ISUID is always cleared; S_ISGID only when the
 * file is group-executable (otherwise that bit is a mandatory-lock marker, not
 * a privilege).  Returns the bits to clear (0 = nothing to do). */
static unsigned setid_strip_bits(uint32_t mode)
{
	unsigned clr = 0;
	if (mode & S_ISUID)
		clr |= S_ISUID;
	if ((mode & S_ISGID) && (mode & S_IXGRP))
		clr |= S_ISGID;
	return clr;
}

/* Permission screen for /dev opens.  Devfs opens are dispatched directly to
 * devfs_open_for_task (they need the task context for /dev/tty), bypassing
 * vfs_open's canonical enforcement — so the DAC decision vfs_open would have
 * made is applied here instead: ancestor search plus the open mode against the
 * device node's reported ownership/mode bits. */
static int devfs_open_perm(task_t *cur, const char *path, uint64_t flags)
{
	if (cur->cred.euid == 0)
		return 0;
	int tr = perm_traverse(path);
	if (tr < 0)
		return tr;
	int want = 0, acc = (int)(flags & 3);
	if (acc == O_RDONLY || acc == O_RDWR)
		want |= MAY_READ;
	if (acc == O_WRONLY || acc == O_RDWR)
		want |= MAY_WRITE;
	if (flags & O_TRUNC)
		want |= MAY_WRITE;
	if (!want)
		return 0;
	struct kstat est;
	if (vfs_stat(path, &est) != ST_OK)
		return 0; /* unresolved: let the open report ENOENT */
	return perm_access(cur, path, &est, want, 0);
}

/* Drop the set-id bits from an open file after a content modification.  Caller
 * gates on non-root + success.  Runs at most once per INODE: the first call
 * evaluates the mode (one inode read) and clears any set-id bits, then marks
 * the inode "clean" (shared across every handle) so later writes short-circuit
 * — the reference's S_NOSEC amortisation.  The fs clears the hint on any mode
 * change, so a re-added set-id bit is re-evaluated even via another fd.  A
 * no-op when the fs can't report the mode (e.g. the perm-less FAT path). */
static void strip_setid_file(vfs_file_t *file)
{
	if (vfs_setid_clean(file))
		return; /* already evaluated for this inode */
	struct kstat st;
	if (vfs_fstat(file, &st) == ST_OK) {
		unsigned clr = setid_strip_bits((uint32_t)st.st_mode);
		if (clr)
			vfs_fchmod(file, (unsigned)st.st_mode & ~clr);
	}
	vfs_mark_setid_clean(file); /* mark AFTER fchmod's invalidation */
}

static int64_t sys_dup(uint64_t oldfd);

// SYS_OPEN - open a file
/* The mode a create-style syscall should hand the filesystem: the requested
 * permission bits minus the caller's umask, which is what POSIX specifies.
 * Every path that creates a name goes through here so the mask cannot be
 * applied in one place and forgotten in another. */
static unsigned int creat_mode(task_t *cur, uint64_t mode)
{
	return (unsigned int)mode & 0777 & ~task_umask(cur) & 0777;
}

static int64_t sys_open(uint64_t pathname, uint64_t flags, uint64_t mode)
{
	might_sleep();
	task_t *cur = sched_current();
	BUG_ON(cur == NULL);
	if (!cur)
		return -EFAULT;

	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	vfs_file_t *file = NULL;
	const char *path = kpath;
	char full[VFS_MAX_PATH];
	if (path[0] != '/' || cur->root[0]) {
		int brest =
			build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
		if (brest != 0)
			return brest;
		path = full;
	}

	/* /dev/fd/N and friends: open == duplicate the caller's descriptor */
	int devfd = devfs_fd_alias_target(path);
	if (devfd >= 0)
		return sys_dup((uint64_t)devfd);

	/* No pre-flight permission screening here: vfs_open() enforces the whole
     * policy authoritatively (ancestor search, read/write mode on an existing
     * target, parent write+search for O_CREAT, immutable/append flags) and its
     * ST_ status maps to the same errno.  The duplicate screening made every
     * non-root open re-resolve the path several extra times. */
	int ret;
	if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
	    path[3] == 'v' && (path[4] == '/' || path[4] == '\0')) {
		int pr = devfs_open_perm(cur, path, flags);
		if (pr < 0)
			return pr;
		ret = devfs_open_for_task(path, (int)flags,
					  creat_mode(cur, mode), &file, cur);
		if (ret == ST_OK && file) {
			file->refcount = 1;
			file->flags = (int)flags;
		}
	} else {
		/* vfs_open_mode, not vfs_open: an O_CREAT open must create the
		 * file with the mode the caller asked for.  Discarding it made
		 * every created file 0644 -- so mkstemp(), which asks for 0600
		 * precisely so its temporary file is private, produced a
		 * world-readable one. */
		ret = vfs_open_mode(path, (int)flags, creat_mode(cur, mode),
				    &file);
	}
	if (ret != ST_OK || file == NULL) {
		return vfs_status_to_errno(ret);
	}

	/* Open FIRST, claim the descriptor after: the slot is claimed and
	 * filled in one locked step so two threads of the same process cannot
	 * be handed the same number (see fd_install_from). */
	int fd = fd_install(cur, file);
	if (fd < 0) {
		fd_release_entry(file);
		return fd;
	}
	/* O_CLOEXEC must be recorded: exec now honours FD_CLOEXEC instead of
	 * closing every descriptor, so a descriptor opened with O_CLOEXEC only
	 * disappears across exec if the flag is stored here. */
	if (flags & O_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	/* O_TRUNC modifies contents → drop set-id bits for a non-root caller. */
	if ((flags & O_TRUNC) && cur->cred.euid != 0)
		strip_setid_file(file);
	return fd;
}

// SYS_OPENAT - open a file relative to dirfd
static int64_t sys_openat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
			  uint64_t mode)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path string to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	int ret;
	if (kpath[0] == '/') {
		size_t i = 0;
		for (; kpath[i] && i < sizeof(full) - 1; ++i)
			full[i] = kpath[i];
		full[i] = '\0';
		/* A jailed task's absolute paths must be canonicalised and
		 * prefixed with the jail root; build_at_path does both.  No-op
		 * (verbatim copy stands) when the task is not chrooted. */
		if (cur->root[0]) {
			int _cr = build_at_path(cur, AT_FDCWD, kpath, full,
						sizeof(full));
			if (_cr != 0)
				return _cr;
		}
	} else {
		ret = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
		if (ret != 0)
			return ret;
	}
	/* /dev/fd/N and friends: open == duplicate the caller's descriptor */
	int devfd = devfs_fd_alias_target(full);
	if (devfd >= 0)
		return sys_dup((uint64_t)devfd);

	vfs_file_t *file = NULL;
	if (full[0] == '/' && full[1] == 'd' && full[2] == 'e' &&
	    full[3] == 'v' && (full[4] == '/' || full[4] == '\0')) {
		int pr = devfs_open_perm(cur, full, flags);
		if (pr < 0)
			return pr;
		ret = devfs_open_for_task(full, (int)flags,
					  creat_mode(cur, mode), &file, cur);
		if (ret == ST_OK && file) {
			file->refcount = 1;
			file->flags = (int)flags;
		}
	} else {
		/* Same as sys_open: the creation mode must reach the fs. */
		ret = vfs_open_mode(full, (int)flags, creat_mode(cur, mode),
				    &file);
	}
	if (ret != ST_OK || file == NULL) {
		return vfs_status_to_errno(ret);
	}
	/* Claim the descriptor only once the object exists — see sys_open. */
	int fd = fd_install(cur, file);
	if (fd < 0) {
		fd_release_entry(file);
		return fd;
	}
	/* O_CLOEXEC must be recorded: exec now honours FD_CLOEXEC instead of
	 * closing every descriptor, so a descriptor opened with O_CLOEXEC only
	 * disappears across exec if the flag is stored here. */
	if (flags & O_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	/* O_TRUNC modifies contents → drop set-id bits for a non-root caller. */
	if ((flags & O_TRUNC) && cur->cred.euid != 0)
		strip_setid_file(file);
	return fd;
}

// SYS_CLOSE - close a file descriptor
static int64_t sys_close(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	if (fd >= TASK_MAX_FDS)
		return -EBADF;

	/* Closing a standard descriptor is ordinary and portable: a program
	 * about to hand itself a terminal does close(0) and then opens or dups
	 * the one it wants onto the descriptor that frees up.  Refusing it here
	 * made close() fail and the following dup() land on 3 instead, so the
	 * program kept the stdio it inherited (xterm's shell talked to the
	 * console rather than to its pty).
	 *
	 * The console has no object to release -- it IS the empty slot -- so
	 * the close is recorded in the flag byte instead. */
	if (fd < 3 && task_fds(cur)[fd] == NULL) {
		uint64_t cflags = 0;
		fds_lock(cur, &cflags);
		/* Re-tested under the lock: two threads of one process closing
		 * the same descriptor must not both be told they succeeded. */
		int already =
			task_get_fd_flags(cur, (unsigned)fd) & FD_STDIO_CLOSED;
		if (!already)
			task_set_fd_flags(cur, (unsigned)fd, FD_STDIO_CLOSED);
		fds_unlock(cur, cflags);
		return already ? -EBADF : 0;
	}

	/* Detach the descriptor from the table FIRST, under the shared-table
	 * lock, and release the object afterwards.  Two threads of the same
	 * process closing the same fd must not both reach the release (a
	 * double free), and a slot must never be observable as free while the
	 * object behind it is still being torn down — releasing can sleep
	 * (vfs_close → pagecache flush), which is exactly the window in which
	 * another thread's open() would claim the slot. */
	/* Before detaching: POSIX releases this process's record locks on the
	 * file when it closes ANY descriptor for it, even if others remain
	 * open.  Done here, while the descriptor is still valid, because the
	 * file's identity (dev/ino) is what the locks are keyed on. */
	{
		vfs_file_t *lf = task_fds(cur)[fd];
		if (lf && !IS_SOCKET_FD(lf) && !unix_sock_is(lf) &&
		    !IS_EPOLL_FD(lf) && !pipe_is_end(lf) && (uintptr_t)lf > 3)
			frlock_release_for_file(lf, (uint32_t)cur->tgid);
	}

	uint64_t lflags = 0;
	fds_lock(cur, &lflags);
	vfs_file_t *file = task_fds(cur)[fd];
	if (file) {
		task_fds(cur)[fd] = NULL;
		/* The slot is about to become free: drop its FD_CLOEXEC bit
		 * with it.  A stale bit left behind is inherited by whatever
		 * lands there next and makes that descriptor vanish across the
		 * next exec.
		 *
		 * For 0/1/2 the empty slot would otherwise read as the console
		 * again, silently reattaching a descriptor the process just
		 * closed (and one that had been redirected onto a pty at that,
		 * so the output would reappear on the terminal). */
		task_set_fd_flags(cur, (unsigned)fd,
				  fd < 3 ? FD_STDIO_CLOSED : 0);
	}
	fds_unlock(cur, lflags);

	if (!file)
		return -EBADF;

	/* Sockets report their own close status; everything else cannot fail
	 * in a way the caller could act on. */
	if (IS_SOCKET_FD(file))
		return sock_close(SOCKET_FD_IDX(file));
	if (unix_sock_is(file))
		return unix_close((unix_socket_t *)file);
	fd_release_entry(file);
	return 0;
}

// SYS_LSEEK - reposition file offset
/* Defined further down, but every fd syscall from here on needs it. */
static int fd_is_special(vfs_file_t *file);

static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence)
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

	if (fd >= TASK_MAX_FDS || task_fds(cur)[fd] == NULL) {
		return -EBADF;
	}

	vfs_file_t *file = task_fds(cur)[fd];

	/* Console dup markers, pipes, sockets and epoll instances are not
	 * seekable -- and must not be handed to vfs_seek, which would take the
	 * marker for a pointer. */
	if (fd_is_special(file)) {
		return -ESPIPE;
	}

	long result = vfs_seek(file, (long)offset, (int)whence);

	if (result < 0) {
		return -EINVAL;
	}

	return result;
}

static int64_t sys_fstat(uint64_t fd, uint64_t stat_buf);

/* The text a /dev/fd/N symlink resolves to.  A descriptor carries no pathname
 * here, so anything that is not a named file gets the conventional
 * "kind:[id]" form and a regular file is identified by inode.  Returns the
 * length written (never NUL-terminated in the caller's count), or -EBADF. */
static int fd_link_target(task_t *cur, int fd, char *out, size_t cap)
{
	if (fd < 0 || fd >= TASK_MAX_FDS)
		return -EBADF;
	if (cap < 2)
		return -EINVAL;
	vfs_file_t *entry = task_fds(cur)[fd];
	uint64_t marker = (uint64_t)entry;
	int n;
	/* 0/1/2 with an empty slot (and the explicit console markers dup'ed
	 * from them) are the caller's terminal. */
	if (!entry) {
		if (!task_fd_is_console(cur, fd))
			return -EBADF;
		n = ksnprintf(out, cap, "/dev/tty");
	} else if (marker >= 1 && marker <= 3) {
		n = ksnprintf(out, cap, "/dev/tty");
	} else if (IS_SOCKET_FD(entry)) {
		n = ksnprintf(out, cap, "socket:[%d]", SOCKET_FD_IDX(entry));
	} else if (unix_sock_is(entry)) {
		/* The socket's own small id, never its address: this string is
		 * handed to userspace, and the descriptor now holds a kernel
		 * pointer. */
		n = ksnprintf(out, cap, "socket:[%d]",
			      (int)((unix_socket_t *)entry)->id);
	} else if (IS_EPOLL_FD(entry)) {
		n = ksnprintf(out, cap, "anon_inode:[eventpoll]");
	} else if (pipe_is_end(entry)) {
		n = ksnprintf(out, cap, "pipe:[%d]", fd);
	} else {
		/* A device node reports the /dev path it was opened under —
		 * the handle's own type is not enough (/dev/tty, /dev/console
		 * and /dev/tty0 share one type), which is why this used to
		 * answer a bare "/dev" for every one of them. */
		n = devfs_fpath(entry, out, cap);
		if (n < 0) {
			struct kstat st;
			mm_memset(&st, 0, sizeof(st));
			if (vfs_fstat(entry, &st) == ST_OK)
				n = ksnprintf(out, cap, "file:[%lu]",
					      (unsigned long)st.st_ino);
			else
				n = ksnprintf(out, cap, "file:[0]");
		}
	}
	/* ksnprintf reports what the format WOULD have produced; clamp to what
	 * actually fits so the length never overruns the caller's buffer. */
	if (n < 0)
		n = 0;
	if ((size_t)n > cap - 1)
		n = (int)cap - 1;
	return n;
}

static int64_t sys_stat_common(const char *path, uint64_t stat_buf,
			       int validate_path)
{
	if (!path || !validate_user_ptr(stat_buf, sizeof(struct kstat))) {
		return -EFAULT;
	}
	if (validate_path && !validate_user_ptr((uint64_t)path, 1)) {
		return -EFAULT;
	}
	/* /dev/fd/N (and /dev/stdin|stdout|stderr) describe an open descriptor
	 * of the CALLER, so stat'ing one means fstat'ing that descriptor -
	 * programs handed such a path (process substitution) expect it to
	 * stat like the underlying object, not to be missing. */
	int devfd = devfs_fd_alias_target(path);
	if (devfd >= 0)
		return sys_fstat((uint64_t)devfd, stat_buf);
	// Security: Zero the struct to prevent leaking uninitialized kernel stack data
	struct kstat st;
	mm_memset(&st, 0, sizeof(st));
	/* vfs_stat runs the ancestor search itself, BEFORE resolving the target,
     * so an unsearchable prefix still yields EACCES (not ENOENT). */
	int ret = vfs_stat(path, &st);
	if (ret != ST_OK) {
		if (ret == ST_NOT_FOUND)
			return -ENOENT;
		if (ret == ST_IO)
			return -EIO; /* corrupt metadata, not absent */
		if (ret == ST_ACCESS || ret == ST_PERM)
			return vfs_status_to_errno(ret);
		return -EINVAL;
	}
	// Security: Use SMAP-aware copy to user
	if (copy_to_user((void *)stat_buf, &st, sizeof(st)) != 0) {
		return -EFAULT;
	}
	return 0;
}

static int64_t sys_stat(uint64_t pathname, uint64_t stat_buf)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	if (kpath[0] == '/' && cur->root[0] == '\0') {
		return sys_stat_common(kpath, stat_buf, 0);
	}
	char full[VFS_MAX_PATH];
	int ret = build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
	if (ret != 0)
		return ret;
	return sys_stat_common(full, stat_buf, 0);
}

static int64_t sys_lstat(uint64_t pathname, uint64_t stat_buf)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	const char *p = kpath;
	char full[VFS_MAX_PATH];
	if (kpath[0] != '/' || cur->root[0]) {
		int ret =
			build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
		if (ret != 0)
			return ret;
		p = full;
	}
	/* lstat must NOT follow a final symlink.  vfs_lstat dispatches to the
     * filesystem's no-follow stat; on a filesystem without symlinks it
     * transparently falls back to plain stat.  vfs_lstat runs the ancestor
     * search itself (before existence is revealed). */
	if (!validate_user_ptr(stat_buf, sizeof(struct kstat)))
		return -EFAULT;
	/* /dev/fd/N, /dev/stdin, /dev/stdout, /dev/stderr are SYMLINKS, as on
	 * every other Unix, and lstat reports the link itself rather than what
	 * it points at.  Reporting the target's type here instead made `ls -l
	 * /dev/fd` describe the descriptor a caller happened to hold — listing
	 * the directory made ls's own directory handle show up as a
	 * subdirectory of /dev/fd, which is nonsense.  stat() (sys_stat_common)
	 * still follows through to the descriptor. */
	int devfd = devfs_fd_alias_target(p);
	if (devfd >= 0) {
		char target[64];
		int tlen = fd_link_target(cur, devfd, target, sizeof(target));
		if (tlen < 0)
			return tlen;
		struct kstat lst;
		mm_memset(&lst, 0, sizeof(lst));
		lst.st_mode = S_IFLNK | 0777;
		lst.st_nlink = 1;
		lst.st_size = tlen;
		lst.st_blksize = 4096;
		if (copy_to_user((void *)stat_buf, &lst, sizeof(lst)) != 0)
			return -EFAULT;
		return 0;
	}
	struct kstat st;
	mm_memset(&st, 0, sizeof(st));
	int r = vfs_lstat(p, &st);
	if (r != ST_OK) {
		if (r == ST_ACCESS || r == ST_PERM)
			return vfs_status_to_errno(r);
		return (r == ST_NOT_FOUND) ? -ENOENT : -EINVAL;
	}
	if (copy_to_user((void *)stat_buf, &st, sizeof(st)) != 0)
		return -EFAULT;
	return 0;
}

static int64_t sys_fstat(uint64_t fd, uint64_t stat_buf)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(stat_buf, sizeof(struct kstat))) {
		return -EFAULT;
	}
	// Security: Zero the struct to prevent leaking uninitialized kernel stack data
	struct kstat st;
	mm_memset(&st, 0, sizeof(st));
	st.st_dev = 0;
	st.st_ino = 0;
	st.st_rdev = 0;
	st.st_nlink = 1;
	st.st_uid = 0;
	st.st_gid = 0;
	st.st_blksize = 4096;
	st.st_blocks = 0;
	st.st_atime = 0;
	st.st_mtime = 0;
	st.st_ctime = 0;
	if (task_fd_is_console(cur, fd)) {
		st.st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					S_IROTH | S_IWOTH);
		st.st_rdev = ((uint64_t)5 << 8) | (fd & 0xff); /* tty major=5 */
		st.st_size = 0;
		// Security: Use SMAP-aware copy to user
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}
	if (fd >= TASK_MAX_FDS || task_fds(cur)[fd] == NULL) {
		return -EBADF;
	}
	vfs_file_t *file = task_fds(cur)[fd];

	/* Classify the tagged fd-table MARKERS before anything dereferences
	 * `file'.  A socket, an AF_UNIX socket, an epoll instance and a dup'ed
	 * console descriptor are all stored as small integers, not pointers, so
	 * handing one to devfs_fstat() reads ->ops out of a bogus address and
	 * faults the KERNEL.  fstat() on an AF_UNIX socket did exactly that:
	 * 0x30009 is UNIX_SOCKET_FD_BASE + 9, and Claws Mail took the whole
	 * system down with it on an ordinary fstat of its own socket.
	 *
	 * fd_link_target() above already classifies in this order; this is the
	 * same set, and pipe_is_end() deliberately rejects every marker so it
	 * cannot be relied on to catch them. */
	uint64_t marker = (uint64_t)file;
	if (marker >= 1 && marker <= 3) {
		/* Console stdio marker planted by dup2. */
		st.st_mode = S_IFCHR | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					S_IROTH | S_IWOTH);
		st.st_rdev = ((uint64_t)5 << 8) | (marker - 1);
		st.st_size = 0;
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}
	if (IS_SOCKET_FD(file) || unix_sock_is(file)) {
		st.st_mode = S_IFSOCK | (S_IRUSR | S_IWUSR);
		/* A UNIX socket descriptor is a kernel pointer now, so the
		 * inode number comes from the socket's own small id.  The
		 * value goes to userspace; the address must not. */
		st.st_ino = unix_sock_is(file) ?
				    (unsigned long)((unix_socket_t *)file)->id :
				    marker;
		st.st_size = 0;
		st.st_blksize = 4096;
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}
	if (IS_EPOLL_FD(file)) {
		/* An anonymous inode: no type bits of its own, reported the way
		 * the conventional interface does -- a regular file the caller
		 * can neither read nor write through ordinary calls. */
		st.st_mode = S_IFREG | (S_IRUSR | S_IWUSR);
		st.st_ino = marker;
		st.st_size = 0;
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}

	if (pipe_is_end(file)) {
		st.st_mode = S_IFIFO | (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					S_IROTH | S_IWOTH);
		st.st_size = 0;
		// Security: Use SMAP-aware copy to user
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}
	if (devfs_fstat(file, &st) == 0) {
		// Security: Use SMAP-aware copy to user
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}
	/* Regular file: report the REAL inode metadata (mode/uid/gid/size/
	 * times) from the filesystem.  fstat() used to hardcode 0644 root:root,
	 * so a program that opens a file and enforces its permission bits via
	 * fstat — sshd rejecting a host key that is not 0600, for one — saw the
	 * wrong mode even though stat() on the path reported the truth. */
	if (vfs_fstat(file, &st) == ST_OK) {
		// Security: Use SMAP-aware copy to user
		return copy_to_user((void *)stat_buf, &st, sizeof(st));
	}
	/* Filesystem cannot report fd metadata: sane regular-file default. */
	st.st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	st.st_size = vfs_size(file);
	// Security: Use SMAP-aware copy to user
	return copy_to_user((void *)stat_buf, &st, sizeof(st));
}

static int64_t sys_fstatat(uint64_t dirfd, uint64_t pathname, uint64_t stat_buf,
			   uint64_t flags)
{
	(void)flags;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1) ||
	    !validate_user_ptr(stat_buf, sizeof(struct kstat))) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	if (kpath[0] == '/') {
		size_t i = 0;
		for (; kpath[i] && i < sizeof(full) - 1; ++i)
			full[i] = kpath[i];
		full[i] = '\0';
		/* A jailed task's absolute paths must be canonicalised and
		 * prefixed with the jail root; build_at_path does both.  No-op
		 * (verbatim copy stands) when the task is not chrooted. */
		if (cur->root[0]) {
			int _cr = build_at_path(cur, AT_FDCWD, kpath, full,
						sizeof(full));
			if (_cr != 0)
				return _cr;
		}
	} else {
		int ret = build_at_path(cur, (int)dirfd, kpath, full,
					sizeof(full));
		if (ret != 0)
			return ret;
	}
	return sys_stat_common(full, stat_buf, 0);
}

static int64_t sys_access(uint64_t pathname, uint64_t mode)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	const char *path = kpath;
	char full[VFS_MAX_PATH];
	if (path[0] != '/' || cur->root[0]) {
		int retb =
			build_at_path(cur, AT_FDCWD, path, full, sizeof(full));
		if (retb != 0)
			return retb;
		path = full;
	}
	/* Ancestor search first (so an unsearchable prefix → EACCES, not
     * ENOENT, and F_OK requires reachability), then existence, then the mode.
     * access(2) checks the REAL uid/gid; R_OK/W_OK/X_OK (4/2/1) == MAY_*. */
	int tr = perm_traverse_cred(path,
				    1); /* real-id search to match the check */
	if (tr < 0)
		return tr;
	struct kstat st;
	int ret = vfs_stat(path, &st);
	if (ret != ST_OK) {
		if (ret == ST_NOT_FOUND)
			return -ENOENT;
		if (ret == ST_IO)
			return -EIO; /* corrupt metadata, not absent */
		return -EINVAL;
	}
	int want = (int)(mode & 7);
	if (want == 0)
		return 0; /* F_OK: exists and reachable */
	return perm_access(cur, path, &st, want, 1);
}

static int64_t sys_faccessat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
			     uint64_t flags)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	if (kpath[0] == '/') {
		size_t i = 0;
		for (; kpath[i] && i < sizeof(full) - 1; ++i)
			full[i] = kpath[i];
		full[i] = '\0';
		/* A jailed task's absolute paths must be canonicalised and
		 * prefixed with the jail root; build_at_path does both.  No-op
		 * (verbatim copy stands) when the task is not chrooted. */
		if (cur->root[0]) {
			int _cr = build_at_path(cur, AT_FDCWD, kpath, full,
						sizeof(full));
			if (_cr != 0)
				return _cr;
		}
	} else {
		int ret = build_at_path(cur, (int)dirfd, kpath, full,
					sizeof(full));
		if (ret != 0)
			return ret;
	}
	/* AT_EACCESS checks with the effective/fs IDs; otherwise the real IDs — the
     * prefix search must use the same ids as the final check. */
	int use_real = (flags & AT_EACCESS) ? 0 : 1;
	int tr = perm_traverse_cred(
		full, use_real); /* ancestor search before existence */
	if (tr < 0)
		return tr;
	struct kstat st;
	int st_ret = vfs_stat(full, &st);
	if (st_ret != ST_OK) {
		if (st_ret == ST_NOT_FOUND)
			return -ENOENT;
		if (st_ret == ST_IO)
			return -EIO; /* corrupt metadata, not absent */
		return -EINVAL;
	}
	int want = (int)(mode & 7); /* R_OK/W_OK/X_OK == MAY_READ/WRITE/EXEC */
	if (want == 0)
		return 0; /* F_OK: exists and reachable */
	return perm_access(cur, full, &st, want, (flags & AT_EACCESS) ? 0 : 1);
}

static int64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (count == 0)
		return 0;
	if (!validate_user_ptr(dirp, count))
		return -EFAULT;
	if (fd >= TASK_MAX_FDS || task_fds(cur)[fd] == NULL)
		return -EBADF;
	vfs_file_t *file = task_fds(cur)[fd];
	/* Sockets and epoll instances were missing from this check, so
	 * getdents64() on one reached vfs_readdir with a marker. */
	if (fd_is_special(file))
		return -ENOTDIR;
	long ret = vfs_readdir(file, (void *)dirp, (long)count);
	if (ret == ST_UNSUPPORTED)
		return -ENOTDIR;
	return ret;
}

static int64_t sys_getdents(uint64_t fd, uint64_t dirp, uint64_t count)
{
	return sys_getdents64(fd, dirp, count);
}

static int64_t sys_chdir(uint64_t pathname)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	const char *cwd = (cur->cwd[0] != 0) ? cur->cwd : "/";
	int ret = normalize_path(cwd, kpath, full, sizeof(full));
	if (ret != 0)
		return ret;
	struct kstat st;
	int vret = vfs_stat(full, &st);
	if (vret == ST_NOT_FOUND)
		return -ENOENT;
	if (vret == ST_IO)
		return -EIO; /* corrupt metadata, not "not a dir" */
	if (vret != ST_OK)
		return -ENOTDIR;
	if ((st.st_mode & S_IFMT) != S_IFDIR)
		return -ENOTDIR;
	/* Entering a directory requires search on it and on every ancestor. */
	int tr = perm_traverse(full);
	if (tr < 0)
		return tr;
	int pr = perm_access(cur, full, &st, MAY_EXEC, 0);
	if (pr < 0)
		return pr;
	// Update FAT32 layer's cwd cluster
	vfs_chdir(full);
	// Update task cwd string with canonical absolute path
	mm_memset(cur->cwd, 0, sizeof(cur->cwd));
	size_t i = 0;
	for (; full[i] && i < sizeof(cur->cwd) - 1; ++i)
		cur->cwd[i] = full[i];
	cur->cwd[i] = '\0';
	return 0;
}

/* SYS_CHROOT — confine the calling task (and its future children) to a
 * subtree.  Privileged operation.  The target is resolved through the normal
 * path machinery, so a chroot INSIDE an existing jail nests correctly, and the
 * stored root is the real, canonical, already-jail-prefixed absolute path.
 * Enforcement happens in build_at_path/apply_chroot for every later textual
 * path; the caller is expected to chdir("/") afterwards, exactly as on other
 * Unix systems. */
/* Defined further down; the shared-memory calls below map and unmap through
 * them so attach/detach reuse the ordinary region bookkeeping. */
static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
			uint64_t flags, uint64_t fd, uint64_t offset);
static int64_t sys_munmap(uint64_t addr, uint64_t length);

/* ================= System V shared memory ============================
 *
 * These sit on top of the same objects /dev/shm exposes, so a segment is a
 * segment however it was created.  shmat() deliberately goes through the
 * filesystem path rather than mapping the object directly: that reuses the
 * region/vfs_file reference machinery, so detaching, exec and process exit all
 * release the segment through paths that already work, instead of needing
 * three new teardown hooks.
 */
/* SysV IPC permission check, the reference algorithm.
 *
 * `flag` carries the requested access in its low nine bits, as the shm* calls
 * define it.  Those are collapsed to one rwx triple, and compared against the
 * triple that applies to this caller -- owner, group, or other.  Root passes
 * regardless, as CAP_IPC_OWNER does there.
 *
 * Without this, shmget() on an existing key handed out an id no matter who
 * owned the segment or what its mode said.
 */
static int ipc_perm_ok(const shm_object_t *o, task_t *cur, unsigned int flag)
{
	unsigned int requested, granted;

	if (!o || !cur)
		return 0;
	if (cred_is_root(&cur->cred))
		return 1;

	requested = ((flag >> 6) | (flag >> 3) | flag) & 0007;
	granted = o->mode & 0777;

	if (cur->cred.euid == o->uid)
		granted >>= 6;
	else if (cred_in_group(&cur->cred, o->gid))
		granted >>= 3;

	return (requested & ~granted & 0007) == 0;
}

/* May this caller administer the segment (IPC_RMID / IPC_SET)?
 *
 * Ownership, not mode: the reference requires the effective uid to match the
 * segment's owner or creator, or the caller to be privileged.  Mode bits do
 * NOT grant this -- a world-writable segment is still only its owner's to
 * destroy. */
static int ipc_owner_ok(const shm_object_t *o, task_t *cur)
{
	if (!o || !cur)
		return 0;
	return cred_is_root(&cur->cred) || cur->cred.euid == o->uid;
}

static int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg)
{
	shm_object_t *o;
	int id = -1;
	int create = (shmflg & IPC_CREAT) != 0;
	int excl = (shmflg & IPC_EXCL) != 0;

	if (size > (unsigned long)SHM_MAX_PAGES * PAGE_SIZE)
		return -EINVAL;

	int existed = 0;
	o = shm_sysv_get((int)key, size, create, excl,
			 (unsigned)(shmflg & 0777), &id);
	if (!o) {
		if (excl && create)
			return -EEXIST;
		if (!create)
			return -ENOENT;
		return -ENOSPC;
	}
	/* The segment pre-existed if this caller did not just create it: a
	 * fresh one is owned by the caller and always passes the check below,
	 * so testing unconditionally is both correct and simpler. */
	(void)existed;
	if (!ipc_perm_ok(o, sched_current(), (unsigned)(shmflg & 0777))) {
		shm_put(o);
		return -EACCES;
	}
	/* An existing segment smaller than requested cannot satisfy the call. */
	if (size && o->size < size) {
		shm_put(o);
		return -EINVAL;
	}
	shm_put(o); /* the id keeps it findable; no reference is held here */
	return id;
}

static int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg)
{
	shm_object_t *o = shm_by_id_get((int)shmid);
	char name[SHM_NAME_MAX];
	char path[SHM_NAME_MAX + 16];
	unsigned long size;
	int prot;
	int64_t r;

	if (!o)
		return -EINVAL;
	if (shm_name_of(o, name, sizeof(name)) != 0) {
		/* Marked for removal: the name is gone, so it can no longer be
		 * attached — existing attachments are unaffected. */
		shm_put(o);
		return -EINVAL;
	}
	size = o->size;
	shm_put(o);
	if (size == 0)
		return -EINVAL;

	{
		static const char pfx[] = "/dev/shm/";
		size_t i = 0, n = 0;
		while (pfx[i])
			path[n++] = pfx[i++];
		i = 0;
		while (name[i] && n < sizeof(path) - 1)
			path[n++] = name[i++];
		path[n] = '\0';
	}

	prot = PROT_READ | ((shmflg & SHM_RDONLY) ? 0 : PROT_WRITE);

	/* Attaching needs read, and write unless SHM_RDONLY was asked for.
	 * The vfs_open below enforces the same thing through the node's mode,
	 * but only for the access it is opened with -- which is why the open
	 * must match the request rather than always being O_RDWR. */
	{
		unsigned int want = (shmflg & SHM_RDONLY) ? 0444 : 0666;
		shm_object_t *co = shm_by_id_get((int)shmid);
		int ok = ipc_perm_ok(co, sched_current(), want);
		if (co)
			shm_put(co);
		if (!ok)
			return -EACCES;
	}

	/* Open the object and map it.  sys_mmap takes the descriptor route, so
	 * install one, map through it, then drop it: the mapping keeps its own
	 * reference on the file, exactly as an mmap after close would. */
	{
		vfs_file_t *f = NULL;
		int fd;
		/* Opened to match the requested access.  It was always O_RDWR,
		 * so a legitimate read-only attach to a segment the caller may
		 * only read was refused by the VFS permission check. */
		int oflags = (shmflg & SHM_RDONLY) ? O_RDONLY : O_RDWR;
		if (vfs_open(path, oflags, &f) != ST_OK || !f)
			return -EINVAL;
		fd = fd_install(sched_current(), f);
		if (fd < 0) {
			vfs_close(f);
			return -EMFILE;
		}
		r = sys_mmap(shmaddr, size, (uint64_t)prot, MAP_SHARED,
			     (uint64_t)fd, 0);
		sys_close((uint64_t)fd);
	}
	return r;
}

static int64_t sys_shmdt(uint64_t shmaddr)
{
	task_t *cur = sched_current();
	mmap_region_t *region;

	if (!cur || shmaddr == 0)
		return -EINVAL;
	cur = task_mm_owner(cur);
	region = mm_find_mmap_region(cur, shmaddr);
	/* Only the exact attach address detaches, as elsewhere. */
	if (!region || region->start != shmaddr)
		return -EINVAL;
	return sys_munmap(shmaddr, region->length);
}

static int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf)
{
	shm_object_t *o = shm_by_id_get((int)shmid);
	char name[SHM_NAME_MAX];
	int rc = 0;

	if (!o)
		return -EINVAL;

	switch (cmd) {
	case IPC_RMID:
		/* Only the owner (or root) may destroy a segment.  There was no
		 * check at all: segment ids are small integers and trivially
		 * enumerated, so any user could tear down any other user's
		 * shared memory -- including the segments the X server and its
		 * clients share through MIT-SHM. */
		if (!ipc_owner_ok(o, sched_current())) {
			shm_put(o);
			return -EPERM;
		}
		/* Mark for destruction: the name goes now, the memory when the
		 * last attachment does.  Creating a segment and removing it
		 * straight away is the normal idiom — it is what stops one
		 * being left behind if the process dies. */
		if (shm_name_of(o, name, sizeof(name)) == 0)
			rc = shm_unlink_name(name);
		else
			rc = 0; /* already removed */
		break;
	case IPC_STAT: {
		struct k_shmid_ds ds;
		/* Reading the metadata needs read access to the segment. */
		if (!ipc_perm_ok(o, sched_current(), 0444)) {
			shm_put(o);
			return -EACCES;
		}
		if (!buf || !validate_user_ptr(buf, sizeof(ds))) {
			rc = -EFAULT;
			break;
		}
		mm_memset(&ds, 0, sizeof(ds));
		ds.shm_perm.uid = o->uid;
		ds.shm_perm.gid = o->gid;
		ds.shm_perm.cuid = o->uid;
		ds.shm_perm.cgid = o->gid;
		ds.shm_perm.mode = o->mode & 0777;
		ds.shm_segsz = o->size;
		ds.shm_nattch = (uint64_t)(o->refs > 0 ? o->refs - 1 : 0);
		if (copy_to_user((void *)buf, &ds, sizeof(ds)) != 0)
			rc = -EFAULT;
		break;
	}
	case IPC_SET:
		/* Nothing here is adjustable after creation, but the ownership
		 * rule still applies: reporting success to a caller who may not
		 * administer the segment would be misleading. */
		if (!ipc_owner_ok(o, sched_current())) {
			shm_put(o);
			return -EPERM;
		}
		rc = 0;
		break;
	default:
		rc = -EINVAL;
		break;
	}
	shm_put(o);
	return rc;
}

static int64_t sys_chroot(uint64_t pathname)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;
	if (!capable())
		return -EPERM;

	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	/* Resolve to a canonical absolute path WITH any current jail applied. */
	char full[VFS_MAX_PATH];
	int ret = build_at_path(cur, AT_FDCWD, kpath, full, sizeof(full));
	if (ret != 0)
		return ret;

	/* Target must exist and be a directory. */
	struct kstat st;
	int vret = vfs_stat(full, &st);
	if (vret == ST_NOT_FOUND)
		return -ENOENT;
	if (vret == ST_IO)
		return -EIO;
	if (vret != ST_OK)
		return -ENOTDIR;
	if ((st.st_mode & S_IFMT) != S_IFDIR)
		return -ENOTDIR;

	/* Store as the new jail root (drop a trailing slash; "/" clears it). */
	size_t n = 0;
	while (full[n])
		n++;
	while (n > 1 && full[n - 1] == '/')
		n--;
	if (n >= sizeof(cur->root))
		return -ENAMETOOLONG;
	mm_memset(cur->root, 0, sizeof(cur->root));
	if (!(n == 1 && full[0] == '/')) {
		for (size_t i = 0; i < n; i++)
			cur->root[i] = full[i];
	}
	return 0;
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(buf, size)) {
		return -EFAULT;
	}
	const char *src = (cur->cwd[0] != 0) ? cur->cwd : "/";
	size_t len = 0;
	while (src[len])
		len++;
	/* POSIX: EINVAL when size is 0, ERANGE when the path does not fit.
	 * Callers retry with a bigger buffer on ERANGE and give up on EINVAL,
	 * so reporting EINVAL for both made a long cwd unreadable. */
	if (size == 0)
		return -EINVAL;
	if (len + 1 > size)
		return -ERANGE;
	if (copy_to_user((void *)buf, src, len + 1) < 0) {
		return -EFAULT;
	}
	return (int64_t)buf;
}

static int64_t sys_umask(uint64_t mask)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	/* The mask is process-wide: set it through the accessor so every thread
	 * of this process sees the change. */
	return (int64_t)task_set_umask(cur, (uint32_t)mask);
}

/* Real credential syscalls.  Operate on the current task's embedded
 * cred; the set*-id permission rules live in cred.c.  Enforcement of file
 * permissions against these IDs is done by the perm_check and perm_traverse helpers above. */
static int64_t sys_getuid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.uid : 0;
}
static int64_t sys_geteuid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.euid : 0;
}
static int64_t sys_getgid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.gid : 0;
}
static int64_t sys_getegid(void)
{
	task_t *c = sched_current();
	return c ? (int64_t)c->cred.egid : 0;
}

static int64_t sys_setuid(uint64_t uid)
{
	task_t *c = sched_current();
	return c ? cred_setuid(&c->cred, (uint32_t)uid) : -EPERM;
}
static int64_t sys_setgid(uint64_t gid)
{
	task_t *c = sched_current();
	return c ? cred_setgid(&c->cred, (uint32_t)gid) : -EPERM;
}
static int64_t sys_seteuid(uint64_t uid)
{
	task_t *c = sched_current();
	return c ? cred_seteuid(&c->cred, (uint32_t)uid) : -EPERM;
}
static int64_t sys_setegid(uint64_t gid)
{
	task_t *c = sched_current();
	return c ? cred_setegid(&c->cred, (uint32_t)gid) : -EPERM;
}

static int64_t sys_getgroups(uint64_t size, uint64_t list)
{
	task_t *c = sched_current();
	uint32_t n = c ? c->cred.ngroups : 0;
	if (size == 0)
		return (int64_t)n; /* query count */
	if (size < n)
		return -EINVAL;
	if (n == 0)
		return 0;
	if (!validate_user_ptr(list, sizeof(int) * (size_t)n))
		return -EFAULT;
	int tmp[NGROUPS_MAX];
	for (uint32_t i = 0; i < n; i++)
		tmp[i] = (int)c->cred.groups[i];
	if (copy_to_user((void *)list, tmp, sizeof(int) * (size_t)n) < 0)
		return -EFAULT;
	return (int64_t)n;
}

static int64_t sys_setgroups(uint64_t size, uint64_t list)
{
	task_t *c = sched_current();
	if (!c)
		return -EPERM;
	if (c->cred.euid != 0)
		return -EPERM; /* privileged only */
	if (size > NGROUPS_MAX)
		return -EINVAL;
	if (size == 0) {
		c->cred.ngroups = 0;
		return 0;
	}
	if (!validate_user_ptr(list, sizeof(int) * (size_t)size))
		return -EFAULT;
	int tmp[NGROUPS_MAX];
	if (copy_from_user(tmp, (const void *)list,
			   sizeof(int) * (size_t)size) < 0)
		return -EFAULT;
	for (uint64_t i = 0; i < size; i++)
		c->cred.groups[i] = (uint32_t)tmp[i];
	c->cred.ngroups = (uint32_t)size;
	return 0;
}

static int64_t sys_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid)
{
	task_t *c = sched_current();
	return c ? cred_setresuid(&c->cred, (uint32_t)ruid, (uint32_t)euid,
				  (uint32_t)suid) :
		   -EPERM;
}
static int64_t sys_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid)
{
	task_t *c = sched_current();
	return c ? cred_setresgid(&c->cred, (uint32_t)rgid, (uint32_t)egid,
				  (uint32_t)sgid) :
		   -EPERM;
}
static int64_t sys_getresuid(uint64_t ruid, uint64_t euid, uint64_t suid)
{
	task_t *c = sched_current();
	if (!c)
		return -EFAULT;
	int vals[3] = { (int)c->cred.uid, (int)c->cred.euid,
			(int)c->cred.suid };
	uint64_t ptrs[3] = { ruid, euid, suid };
	for (int i = 0; i < 3; i++) {
		if (!ptrs[i])
			continue;
		if (!validate_user_ptr(ptrs[i], sizeof(int)))
			return -EFAULT;
		if (copy_to_user((void *)ptrs[i], &vals[i], sizeof(int)) < 0)
			return -EFAULT;
	}
	return 0;
}
static int64_t sys_getresgid(uint64_t rgid, uint64_t egid, uint64_t sgid)
{
	task_t *c = sched_current();
	if (!c)
		return -EFAULT;
	int vals[3] = { (int)c->cred.gid, (int)c->cred.egid,
			(int)c->cred.sgid };
	uint64_t ptrs[3] = { rgid, egid, sgid };
	for (int i = 0; i < 3; i++) {
		if (!ptrs[i])
			continue;
		if (!validate_user_ptr(ptrs[i], sizeof(int)))
			return -EFAULT;
		if (copy_to_user((void *)ptrs[i], &vals[i], sizeof(int)) < 0)
			return -EFAULT;
	}
	return 0;
}

static int64_t sys_gethostname(uint64_t name, uint64_t len)
{
	const char *host = net_get_hostname();
	size_t hlen = 0;
	while (host[hlen])
		hlen++;
	if (!validate_user_ptr(name, len))
		return -EFAULT;
	if (len < hlen + 1)
		return -EINVAL;
	if (copy_to_user((void *)name, host, hlen + 1) < 0) {
		return -EFAULT;
	}
	return 0;
}

static int64_t sys_uname(uint64_t buf)
{
	if (!validate_user_ptr(buf, sizeof(k_utsname_t)))
		return -EFAULT;
	k_utsname_t u;
	mm_memset(&u, 0, sizeof(u));
	const char *sys = "LikeOS";
	const char *node = net_get_hostname();
#ifdef LIKEOS_VERSION
	const char *rel = LIKEOS_VERSION;
#else
	const char *rel = "0.2";
#endif
#ifdef BUILD_DATE
	const char *ver = "preempt-smp " BUILD_DATE;
#else
	const char *ver = "preempt-smp";
#endif
	const char *mach = "x86_64";
	// Copy strings (including null terminators), capped to field size
	for (int i = 0; sys[i] && i < 64; i++)
		u.sysname[i] = sys[i];
	for (int i = 0; node[i] && i < 64; i++)
		u.nodename[i] = node[i];
	for (int i = 0; rel[i] && i < 64; i++)
		u.release[i] = rel[i];
	for (int i = 0; ver[i] && i < 64; i++)
		u.version[i] = ver[i];
	for (int i = 0; mach[i] && i < 64; i++)
		u.machine[i] = mach[i];
	if (copy_to_user((void *)buf, &u, sizeof(u)) < 0) {
		return -EFAULT;
	}
	return 0;
}

static int64_t sys_time(uint64_t tloc)
{
	uint64_t sec = timer_get_epoch();
	if (tloc && validate_user_ptr(tloc, sizeof(uint64_t))) {
		copy_to_user((void *)tloc, &sec, sizeof(sec));
	}
	return (int64_t)sec;
}

static int64_t sys_gettimeofday(uint64_t tv, uint64_t tz)
{
	(void)tz;
	if (!validate_user_ptr(tv, sizeof(k_timeval_t)))
		return -EFAULT;
	k_timeval_t kv;
	// Use timer_get_precise_us() which atomically reads g_ticks and the
	// ACPI PM Timer via a seqlock, eliminating the race where a BSP tick
	// fires between reading the two values and corrupts the sub-tick delta.
	uint64_t total_us = timer_get_precise_us();
	uint64_t boot_epoch = timer_get_boot_epoch();

	kv.tv_sec = (long)(boot_epoch + total_us / 1000000ULL);
	kv.tv_usec = (long)(total_us % 1000000ULL);

	if (copy_to_user((void *)tv, &kv, sizeof(kv)) < 0) {
		return -EFAULT;
	}
	return 0;
}

static int64_t sys_settimeofday(uint64_t tv_ptr, uint64_t tz)
{
	(void)tz;
	/* Setting the wall-clock is a system-wide change: privileged only. */
	if (!capable())
		return -EPERM;
	if (!validate_user_ptr(tv_ptr, sizeof(k_timeval_t)))
		return -EFAULT;
	k_timeval_t kv;
	if (copy_from_user(&kv, (const void *)tv_ptr, sizeof(kv)) < 0)
		return -EFAULT;
	/* Set the wall-clock time (adjusts boot_epoch and writes CMOS RTC) */
	timer_set_time((uint64_t)kv.tv_sec);
	return 0;
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
static int fd_is_special(vfs_file_t *file)
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

static int64_t sys_fsync(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (fd >= TASK_MAX_FDS || task_fds(cur)[fd] == NULL)
		return -EBADF;
	vfs_file_t *file = task_fds(cur)[fd];
	if (fd_is_special(file))
		return 0; /* nothing to flush */
	/* Dispatch to the file's own filesystem; a filesystem with nothing to
     * flush leaves the op NULL and fsync is a no-op. */
	if (file->ops && file->ops->fsync)
		return file->ops->fsync(file);
	return 0;
}

static int64_t sys_sync(void)
{
	pagecache_sync();
	vfs_sync(); /* flush fs metadata + clean the journal (no-op on FAT32) */
	return 0;
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t length)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (fd >= TASK_MAX_FDS || task_fds(cur)[fd] == NULL)
		return -EBADF;
	vfs_file_t *file = task_fds(cur)[fd];
	/* Only a real file has a length to set; POSIX gives EINVAL for the
	 * rest, and dereferencing a marker here would fault the kernel. */
	if (fd_is_special(file))
		return -EINVAL;
	int r = vfs_truncate(file, (unsigned long)length);
	/* Truncating contents drops set-id bits for a non-privileged caller,
     * same as write() (see strip_setid_file for the once-per-inode fast-path). */
	if (r >= 0 && cur->cred.euid != 0 && !vfs_setid_clean(file))
		strip_setid_file(file);
	return r;
}

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd);
static int64_t sys_dup_from(uint64_t oldfd, int from);

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
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

static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t argp)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}

	// Socket fd markers - route to network ioctl handler
	if (file && IS_SOCKET_FD(file)) {
		int idx = SOCKET_FD_IDX(file);
		size_t arg_len = 0;
		switch (req) {
		case SIOCGIFCONF:
			arg_len = sizeof(struct ifconf);
			break;
		case SIOCGIFFLAGS:
		case SIOCSIFFLAGS:
		case SIOCGIFADDR:
		case SIOCSIFADDR:
		case SIOCGIFNETMASK:
		case SIOCSIFNETMASK:
		case SIOCGIFBRDADDR:
		case SIOCSIFBRDADDR:
		case SIOCGIFMTU:
		case SIOCSIFMTU:
		case SIOCGIFHWADDR:
		case SIOCGIFINDEX:
		case SIOCGIFNAME:
			arg_len = sizeof(struct ifreq);
			break;
		case 0x5421: /* FIONBIO */
		case 0x541B: /* FIONREAD */
			arg_len = sizeof(int);
			break;
		default:
			arg_len = 0;
			break;
		}
		if (arg_len > 0) {
			if (!argp)
				return -EFAULT;
			if (!validate_user_ptr(argp, arg_len))
				return -EFAULT;
		}
		/* Mutating interface configuration (address, netmask, flags, MTU)
		 * is a privileged, system-wide change; the query ioctls are not. */
		switch (req) {
		case SIOCSIFFLAGS:
		case SIOCSIFADDR:
		case SIOCSIFNETMASK:
		case SIOCSIFBRDADDR:
		case SIOCSIFMTU:
			if (!capable())
				return -EPERM;
			break;
		default:
			break;
		}
		smap_disable();
		int64_t ret =
			sock_ioctl_net(idx, (unsigned long)req, (void *)argp);
		smap_enable();
		return ret;
	}

	if (task_fd_is_console(cur, fd)) {
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
		return tty_ioctl(tty, (unsigned long)req, (void *)argp, cur);
	}
	/* Duplicated stdio markers: dup()/SCM_RIGHTS store the marker value
     * (1, 2 or 3 = oldfd+1) into the fd_table.  These should still be
     * routed to the controlling TTY, otherwise isatty()/tcgetattr() on
     * a dup'd stdin/stdout/stderr would fail. */
	if (file) {
		uintptr_t mk = (uintptr_t)file;
		if (mk >= 1 && mk <= 3) {
			tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();
			return tty_ioctl(tty, (unsigned long)req, (void *)argp,
					 cur);
		}
	}
	if (!file) {
		return -EBADF;
	}

	/* AF_UNIX and epoll descriptors are fd-table MARKERS (small tagged
	 * integers), not vfs_file pointers, and a pipe end is a pipe object
	 * rather than a devfs file.  All three have to be classified HERE:
	 * the devfs fallthrough below dereferences whatever it is handed, so
	 * an ioctl() on a unix socket faulted the kernel on the marker value
	 * itself (scp hit this — ssh probes its socketpair with tcgetattr).
	 * The numeric marker tests come first because they dereference
	 * nothing. */
	if (unix_sock_is(file)) {
		unix_socket_t *us = (unix_socket_t *)file;
		if (!us)
			return -EBADF;
		if (req == 0x5421 /* FIONBIO */) {
			if (!argp || !validate_user_ptr(argp, sizeof(int)))
				return -EFAULT;
			int on = 0;
			if (copy_from_user(&on, (void *)argp, sizeof(on)) != 0)
				return -EFAULT;
			us->nonblock = on ? 1 : 0;
			return 0;
		}
		/* Not a terminal: what tcgetattr()/isatty() expect to see. */
		return -ENOTTY;
	}
	if (IS_EPOLL_FD(file))
		return -ENOTTY;
	if (pipe_is_end(file))
		return -ENOTTY;

	return devfs_ioctl(file, (unsigned long)req, (void *)argp, cur);
}

static int64_t sys_setpgid(uint64_t pid, uint64_t pgid)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (pid == 0) {
		pid = (uint64_t)cur->id;
	}
	if (pgid == 0) {
		pgid = pid;
	}
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t) {
		return -ESRCH;
	}
	t->pgid = (int)pgid;
	return 0;
}

static int64_t sys_getpgrp(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	return cur->pgid;
}

static int64_t sys_tcgetpgrp(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}
	tty_t *tty = NULL;
	if (task_fd_is_console(cur, fd)) {
		tty = cur->ctty ? cur->ctty : tty_get_console();
	}
	if (file) {
		uintptr_t mk = (uintptr_t)file;
		if (mk >= 1 && mk <= 3) {
			tty = cur->ctty ? cur->ctty : tty_get_console();
		} else {
			tty = devfs_get_tty(file);
		}
	}
	if (!tty) {
		return -ENOTTY;
	}
	return tty->fg_pgid;
}

static int64_t sys_tcsetpgrp(uint64_t fd, uint64_t pgrp)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}
	tty_t *tty = NULL;
	if (task_fd_is_console(cur, fd)) {
		tty = cur->ctty ? cur->ctty : tty_get_console();
	}
	if (file) {
		uintptr_t mk = (uintptr_t)file;
		if (mk >= 1 && mk <= 3) {
			tty = cur->ctty ? cur->ctty : tty_get_console();
		} else {
			tty = devfs_get_tty(file);
		}
	}
	if (!tty) {
		return -ENOTTY;
	}
	tty->fg_pgid = (int)pgrp;
	return 0;
}

// POSIX setsid(2) - create a new session.
// On success the calling process becomes session leader, gets a new
// process group, and is detached from any controlling tty.  Returns
// the new session ID (the pid).  Returns -EPERM if the caller is
// already a process-group leader.
static int64_t sys_setsid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (cur->pgid == (int)cur->id) {
		return -EPERM;
	}
	cur->sid = (int)cur->id;
	cur->pgid = (int)cur->id;
	cur->ctty = NULL;
	return cur->id;
}

// POSIX getsid(2) - return session ID of process pid (0 = self).
static int64_t sys_getsid(uint64_t pid)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (pid == 0)
		return cur->sid;
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t)
		return -ESRCH;
	return t->sid;
}

// POSIX getpgid(2) - return process-group ID of process pid (0 = self).
static int64_t sys_getpgid(uint64_t pid)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (pid == 0)
		return cur->pgid;
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t)
		return -ESRCH;
	return t->pgid;
}

// POSIX getrusage(2) - resource usage.
//
// The CPU times are real: the timer tick charges every task to utime_ticks or
// stime_ticks depending on the privilege level it interrupted (see
// timer_handle_tick), so the accounting already exists and only needs
// reporting.  wait4() has been reporting a dead child's times from the same
// counters all along; this is the same conversion for a live process.
//
// Resolution is one timer tick, so a process that has run for less than a tick
// reads as zero.  That is the honest answer at this sampling rate, not a bug.
//
// The remaining fields stay zero because nothing counts them yet: there is no
// per-task page-fault or resident-set accounting to report.
struct k_rusage_compat {
	int64_t ru_utime_sec;
	int64_t ru_utime_usec;
	int64_t ru_stime_sec;
	int64_t ru_stime_usec;
	int64_t ru_pad[14];
};

// Ticks -> seconds and microseconds, at whatever rate the timer is running.
static void ticks_to_timeval(uint64_t ticks, int64_t *sec, int64_t *usec)
{
	uint32_t freq = timer_get_frequency();
	if (freq == 0)
		freq = 100;
	*sec = (int64_t)(ticks / freq);
	*usec = (int64_t)((ticks % freq) * (1000000 / freq));
}

#define K_RUSAGE_SELF 0
#define K_RUSAGE_CHILDREN (-1)

static int64_t sys_getrusage(uint64_t who, uint64_t uptr)
{
	if (!uptr)
		return -EFAULT;
	if (!validate_user_ptr(uptr, sizeof(struct k_rusage_compat)))
		return -EFAULT;

	struct k_rusage_compat ru;
	for (size_t i = 0; i < sizeof(ru); i++)
		((uint8_t *)&ru)[i] = 0;

	task_t *cur = sched_current();
	switch ((int)(int32_t)who) {
	case K_RUSAGE_SELF:
		if (cur) {
			ticks_to_timeval(cur->utime_ticks, &ru.ru_utime_sec,
					 &ru.ru_utime_usec);
			ticks_to_timeval(cur->stime_ticks, &ru.ru_stime_sec,
					 &ru.ru_stime_usec);
		}
		break;
	case K_RUSAGE_CHILDREN:
		// Reaped children's times are not accumulated onto the parent,
		// so this is zero rather than wrong.  wait4() reports each
		// child's own usage as it is reaped, which is where a caller
		// can get the real figures today.
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void *)uptr, &ru, sizeof(ru)) != 0)
		return -EFAULT;
	return 0;
}

// POSIX writev(2) / readv(2) - scatter/gather I/O implemented as a loop
// over write(2) / read(2).  Per POSIX the implementation is allowed to
// process the iovecs sequentially; the only invariant is partial-write
// semantics on errors mid-stream.  Returns total bytes transferred.
struct k_iovec_compat {
	uint64_t iov_base;
	uint64_t iov_len;
};

static int64_t sys_writev(uint64_t fd, uint64_t iovp, uint64_t iovcnt)
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

static int64_t sys_readv(uint64_t fd, uint64_t iovp, uint64_t iovcnt)
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

// Forward declaration for signal functions
extern ktimer_t timer_create_internal(task_t *task, clockid_t clockid,
				      struct k_sigevent *sevp);
extern int timer_settime_internal(ktimer_t timerid, int flags,
				  const struct k_itimerspec *new_value,
				  struct k_itimerspec *old_value);
extern int timer_gettime_internal(ktimer_t timerid,
				  struct k_itimerspec *curr_value);
extern int timer_getoverrun_internal(ktimer_t timerid);
extern int timer_delete_internal(ktimer_t timerid);

static void kill_task(task_t *t, int sig)
{
	if (!t) {
		return;
	}
	// Use sched_signal_task which properly handles SIGKILL/SIGSTOP
	// and other signals with their default actions
	sched_signal_task(t, sig);
}

static int64_t sys_kill(uint64_t pid, uint64_t sig)
{
	if (sig > 64)
		return -EINVAL;
	task_t *self = sched_current();
	if (!self)
		return -EFAULT;
	/* POSIX pid forms:
	 *   pid  > 0   that process
	 *   pid == 0   every process in the CALLER's process group
	 *   pid <  -1  every process in process group -pid
	 * pid 0 used to be refused as "the kernel idle task", but 0 is not a
	 * pid here at all — it is the caller's own group.  A shell relies on
	 * this: when it finds itself in the background it does kill(0, SIGTTIN)
	 * to stop until it is moved to the foreground, and the EPERM made it
	 * spin and then switch job control off entirely. */
	if (pid == 0) {
		if (self->pgid <= 0)
			return -ESRCH;
		/* Our own group always contains us and we may always signal
		 * ourselves, so this cannot come back -EPERM; the check only
		 * skips members belonging to another user, which a group can
		 * acquire across a setuid exec. */
		return sched_signal_pgrp_checked(self->pgid, (int)sig);
	}
	if ((int64_t)pid < -1) {
		int64_t pgid = -(int64_t)pid;
		if (pgid > 0x7fffffff)
			return -ESRCH;
		return sched_signal_pgrp_checked((int)pgid, (int)sig);
	}
	if ((int64_t)pid == -1) {
		/* Broadcast: every process the caller may signal, except itself
		 * and init.  Treating -1 as "process group 1" (which is what
		 * negating it used to produce) signalled init's group instead
		 * of everything, which is both wrong and dangerous. */
		if (sig == 0)
			return 0;
		return sched_signal_all(self, (int)sig);
	}
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t)
		return -ESRCH;
	// Kernel tasks (idle, init, kernel threads) cannot be signalled
	if (t->privilege == TASK_KERNEL)
		return -EPERM;
	/* Credential check: an unprivileged caller may only signal a process
	 * with a matching uid (applies even to the sig==0 existence probe). */
	int perr = signal_permission(t, (int)sig);
	if (perr != 0)
		return perr;
	if (sig == 0)
		return 0;
	kill_task(t, (int)sig);
	return 0;
}

static int64_t sys_unlink(uint64_t pathname)
{
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_remove(
		kpath); /* parent write+search, + sticky bit */
	if (pr < 0)
		return pr;
	int st = vfs_unlink(kpath);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	return -EINVAL;
}

static int64_t sys_rename(uint64_t oldpath, uint64_t newpath)
{
	if (!validate_user_ptr(oldpath, 1) || !validate_user_ptr(newpath, 1))
		return -EFAULT;

	// Copy user paths to kernel buffers first
	char koldpath[VFS_MAX_PATH], knewpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)oldpath, koldpath,
				  sizeof(koldpath));
	if (cret != 0)
		return cret;
	cret = copy_user_path((const char *)newpath, knewpath,
			      sizeof(knewpath));
	if (cret != 0)
		return cret;

	cret = canon_task_path(koldpath, sizeof(koldpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(knewpath, sizeof(knewpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_remove(
		koldpath); /* remove source (+ sticky)        */
	if (pr < 0)
		return pr;
	pr = perm_check_remove(
		knewpath); /* write dest (+ sticky on overwrite) */
	if (pr < 0)
		return pr;
	int st = vfs_rename(koldpath, knewpath);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	return -EINVAL;
}

static int64_t sys_mkdir(uint64_t pathname, uint64_t mode)
{
	task_t *cur = sched_current();
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_parent(kpath,
				   MAY_WRITE | MAY_EXEC); /* write the dir */
	if (pr < 0)
		return pr;
	/* umask applies to directories too; the raw mode was being passed
	 * straight through, so `mkdir -m 700` and a default 0777 mkdir both
	 * ignored the caller's mask. */
	int st = vfs_mkdir(kpath, creat_mode(cur, mode));
	if (st == ST_OK)
		return 0;
	if (st == ST_EXISTS)
		return -EEXIST;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	if (st == ST_NOMEM)
		return -ENOMEM;
	if (st == ST_IO)
		return -EIO;
	return -EINVAL;
}
static int64_t sys_rmdir(uint64_t pathname)
{
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;

	// Copy user path to kernel buffer first
	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;
	cret = canon_task_path(kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	int pr = perm_check_remove(
		kpath); /* parent write+search, + sticky bit */
	if (pr < 0)
		return pr;
	int st = vfs_rmdir(kpath);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	if (st == ST_NOTEMPTY)
		return -ENOTEMPTY;
	if (st == ST_NOMEM)
		return -ENOMEM;
	if (st == ST_IO)
		return -EIO;
	return -EINVAL;
}

/* unlinkat(dirfd, path, flags) -- remove a name relative to a directory fd.
 *
 * One syscall covering both unlink() and rmdir(), which is how POSIX defines
 * it: AT_REMOVEDIR selects the directory case.  Everything else -- the
 * relative-path resolution against dirfd (and the caller's chroot), the
 * parent write+search check and the sticky-bit rule -- is the same machinery
 * the non-at versions use, so the two cannot drift apart in policy. */
static int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (!validate_user_ptr(pathname, 1))
		return -EFAULT;
	/* Reject flags we do not implement rather than ignoring them: a caller
	 * that passes one is asking for behaviour we would not deliver. */
	if (flags & ~((uint64_t)AT_REMOVEDIR))
		return -EINVAL;

	char kpath[VFS_MAX_PATH];
	int cret = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (cret != 0)
		return cret;

	char full[VFS_MAX_PATH];
	int brest = build_at_path(cur, (int)dirfd, kpath, full, sizeof(full));
	if (brest != 0)
		return brest;

	int pr = perm_check_remove(full);
	if (pr < 0)
		return pr;

	int st = (flags & AT_REMOVEDIR) ? vfs_rmdir(full) : vfs_unlink(full);
	if (st == ST_OK)
		return 0;
	if (st == ST_NOT_FOUND)
		return -ENOENT;
	if (st == ST_NOTEMPTY)
		return -ENOTEMPTY;
	if (st == ST_NOMEM)
		return -ENOMEM;
	if (st == ST_IO)
		return -EIO;
	return -EINVAL;
}

static int64_t sys_link(uint64_t oldpath, uint64_t newpath)
{
	char kold[VFS_MAX_PATH], knew[VFS_MAX_PATH];
	int c = copy_user_path((const char *)oldpath, kold, sizeof(kold));
	if (c)
		return c;
	c = copy_user_path((const char *)newpath, knew, sizeof(knew));
	if (c)
		return c;
	c = canon_task_path(kold, sizeof(kold));
	if (c)
		return c;
	c = canon_task_path(knew, sizeof(knew));
	if (c)
		return c;
	int pr = perm_check_parent(
		knew, MAY_WRITE | MAY_EXEC); /* write the new dir */
	if (pr < 0)
		return pr;
	int r = vfs_link(kold, knew);
	if (r == ST_OK)
		return 0;
	if (r == ST_UNSUPPORTED)
		return -EPERM; /* filesystem has no hard links  */
	return vfs_status_to_errno(r);
}
static int64_t sys_symlink(uint64_t target, uint64_t linkpath)
{
	char ktarget[VFS_MAX_PATH], klink[VFS_MAX_PATH];
	int c = copy_user_path((const char *)target, ktarget, sizeof(ktarget));
	if (c)
		return c;
	c = copy_user_path((const char *)linkpath, klink, sizeof(klink));
	if (c)
		return c;
	/* Only the link's own name.  `target` is the link's CONTENT, stored
	 * verbatim: resolving it would turn a relative symlink into an absolute
	 * one naming a different file. */
	c = canon_task_path(klink, sizeof(klink));
	if (c)
		return c;
	int pr = perm_check_parent(
		klink, MAY_WRITE | MAY_EXEC); /* write the new dir */
	if (pr < 0)
		return pr;
	int r = vfs_symlink(ktarget, klink);
	if (r == ST_OK)
		return 0;
	if (r == ST_UNSUPPORTED)
		return -EPERM; /* filesystem has no symlinks    */
	return vfs_status_to_errno(r);
}
static int64_t sys_readlink(uint64_t pathname, uint64_t buf, uint64_t bufsiz)
{
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (c)
		return c;
	c = canon_task_path(kpath, sizeof(kpath));
	if (c)
		return c;
	int tr = perm_traverse(kpath); /* search on every ancestor dir */
	if (tr < 0)
		return tr;
	if (bufsiz == 0)
		return -EINVAL;
	if (!validate_user_ptr(buf, 1))
		return -EFAULT;
	/* /dev/fd/N and the standard-stream aliases are symlinks (see the
	 * lstat path); `ls -l` reads them to print the "-> target" and errors
	 * out if the read fails. */
	int devfd = devfs_fd_alias_target(kpath);
	if (devfd >= 0) {
		task_t *cur = sched_current();
		if (!cur)
			return -EFAULT;
		char target[64];
		int tlen = fd_link_target(cur, devfd, target, sizeof(target));
		if (tlen < 0)
			return tlen;
		if ((unsigned long)tlen > bufsiz)
			tlen = (int)bufsiz;
		if (copy_to_user((void *)buf, target, (size_t)tlen) != 0)
			return -EFAULT;
		return tlen;
	}
	char kbuf[256];
	unsigned long n = bufsiz;
	if (n > sizeof(kbuf))
		n = sizeof(kbuf);
	int r = vfs_readlink(kpath, kbuf,
			     n); /* <0 on error / not-a-symlink   */
	if (r < 0)
		return vfs_status_to_errno(r);
	if (copy_to_user((void *)buf, kbuf, (size_t)r) != 0)
		return -EFAULT;
	return r; /* byte count (no NUL)           */
}
/* chmod/chown persist on filesystems with UNIX perms; on one without them the
 * vfs layer succeeds silently (ST_OK) so legacy behavior is preserved. */
static int64_t sys_chmod(uint64_t pathname, uint64_t mode)
{
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (c)
		return c;
	c = canon_task_path(kpath, sizeof(kpath));
	if (c)
		return c;
	unsigned new_mode = (unsigned)mode;
	/* Only the file's owner (or root) may change its mode; and a non-root
     * caller not in the file's group cannot set the set-group-ID bit. */
	task_t *cur = sched_current();
	if (cur && cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
		struct kstat st;
		if (vfs_stat(kpath, &st) == ST_OK) {
			if ((uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
			if ((new_mode & S_ISGID) &&
			    !cred_in_group(&cur->cred, (uint32_t)st.st_gid))
				new_mode &= ~(unsigned)S_ISGID;
		}
	}
	int r = vfs_chmod(kpath, new_mode);
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}
static int64_t sys_fchmod(uint64_t fd, uint64_t mode)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	if (fd_is_special(task_fds(cur)[fd]))
		return 0; /* no perms to change */
	unsigned new_mode = (unsigned)mode;
	/* Only the owner (or root) may chmod, and a non-root caller not in the
     * file's group cannot set the set-group-ID bit.  Permissive if the fs can't
     * report the owner (vfs_fstat unsupported, e.g. the perm-less FAT path). */
	if (cur->cred.euid != 0) {
		struct kstat st;
		if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK) {
			if ((uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
			if ((new_mode & S_ISGID) &&
			    !cred_in_group(&cur->cred, (uint32_t)st.st_gid))
				new_mode &= ~(unsigned)S_ISGID;
		}
	}
	int r = vfs_fchmod(task_fds(cur)[fd], new_mode);
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}
static int64_t sys_chown(uint64_t pathname, uint64_t owner, uint64_t group)
{
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)pathname, kpath, sizeof(kpath));
	if (c)
		return c;
	c = canon_task_path(kpath, sizeof(kpath));
	if (c)
		return c;
	/* Changing the owner is root-only; a non-root owner may change the
     * group of their own file to one of their groups. */
	task_t *cur = sched_current();
	int new_uid = (int)owner, new_gid = (int)group;
	if (cur && cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
		struct kstat st;
		if (vfs_stat(kpath, &st) == ST_OK) {
			if (new_uid != -1 &&
			    (uint32_t)new_uid != (uint32_t)st.st_uid)
				return -EPERM; /* owner change: root only */
			if (new_gid != -1 &&
			    (uint32_t)new_gid != (uint32_t)st.st_gid) {
				if ((uint32_t)st.st_uid != cur->cred.fsuid)
					return -EPERM;
				if (!cred_in_group(&cur->cred,
						   (uint32_t)new_gid))
					return -EPERM;
			}
		}
	}
	int r = vfs_chown(kpath, new_uid, new_gid); /* -1 => leave unchanged */
	if (r == ST_OK && cur &&
	    cur->cred.euid != 0) { /* drop set-id on ownership change */
		struct kstat st;
		if (vfs_stat(kpath, &st) == ST_OK) {
			unsigned clr = setid_strip_bits((uint32_t)st.st_mode);
			if (clr)
				vfs_chmod(kpath, (unsigned)st.st_mode & ~clr);
		}
	}
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}
static int64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	if (fd_is_special(task_fds(cur)[fd]))
		return 0; /* no ownership to change */
	int new_uid = (int)owner, new_gid = (int)group;
	/* Owner change is root-only; a non-root owner may regroup to one of
     * their groups (same rule as path chown).  Permissive if owner unknown. */
	if (cur->cred.euid != 0) {
		struct kstat st;
		if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK) {
			if (new_uid != -1 &&
			    (uint32_t)new_uid != (uint32_t)st.st_uid)
				return -EPERM;
			if (new_gid != -1 &&
			    (uint32_t)new_gid != (uint32_t)st.st_gid) {
				if ((uint32_t)st.st_uid != cur->cred.fsuid)
					return -EPERM;
				if (!cred_in_group(&cur->cred,
						   (uint32_t)new_gid))
					return -EPERM;
			}
		}
	}
	int r = vfs_fchown(task_fds(cur)[fd], new_uid, new_gid);
	if (r == ST_OK &&
	    cur->cred.euid != 0) { /* drop set-id on ownership change */
		struct kstat st;
		if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK) {
			unsigned clr = setid_strip_bits((uint32_t)st.st_mode);
			if (clr)
				vfs_fchmod(task_fds(cur)[fd],
					   (unsigned)st.st_mode & ~clr);
		}
	}
	return (r == ST_OK) ? 0 : vfs_status_to_errno(r);
}
// utimensat: set a path's modification time via the owning filesystem.
static int64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
			     uint64_t flags)
{
	(void)flags;

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	/* Reject AT_EMPTY_PATH (0x1000) - not supported */
	if (flags & 0x1000)
		return -EINVAL;

	/* If pathname is NULL/empty, we'd need dirfd to be a real fd — not supported */
	if (!pathname)
		return -EFAULT;

	char kpath[VFS_MAX_PATH];
	size_t plen;
	int err = user_strnlen((const char *)pathname, VFS_MAX_PATH, &plen);
	if (err)
		return err;
	err = copy_from_user(kpath, (const void *)pathname, plen + 1);
	if (err)
		return err;

	/* Canonicalise against the task cwd / dirfd so the VFS gets an absolute
	 * path: a relative path skips the ancestor search-permission traversal
	 * (and trips a warning) for a non-root caller. */
	char full[VFS_MAX_PATH];
	if (kpath[0] == '/') {
		size_t i = 0;
		for (; kpath[i] && i < sizeof(full) - 1; ++i)
			full[i] = kpath[i];
		full[i] = '\0';
		/* A jailed task's absolute paths must be canonicalised and
		 * prefixed with the jail root; build_at_path does both.  No-op
		 * (verbatim copy stands) when the task is not chrooted. */
		if (cur->root[0]) {
			int _cr = build_at_path(cur, AT_FDCWD, kpath, full,
						sizeof(full));
			if (_cr != 0)
				return _cr;
		}
	} else {
		int bret = build_at_path(cur, (int)dirfd, kpath, full,
					 sizeof(full));
		if (bret != 0)
			return bret;
	}

	int64_t mtime_sec = 0;
	/* The UTIME_NOW / UTIME_OMIT sentinels (1073741823 / 1073741822) match the
     * VFS_UTIME_* protocol values, so the userspace nsec passes straight
     * through; default (no times given) means "set to now". */
	long mtime_nsec = VFS_UTIME_NOW;

	if (times) {
		/* times points to struct timespec[2]: [0]=atime, [1]=mtime */
		struct k_timespec ts[2];
		if (!validate_user_ptr(times, sizeof(ts)))
			return -EFAULT;
		err = copy_from_user(ts, (const void *)times, sizeof(ts));
		if (err)
			return err;

		mtime_sec = ts[1].tv_sec;
		mtime_nsec = (long)ts[1].tv_nsec;
	}

	/* vfs_utimensat routes to the owning filesystem (devfs has no timestamps,
     * so it succeeds silently). */
	int r = vfs_utimensat(full, mtime_sec, mtime_nsec);
	if (r == ST_NOT_FOUND || r == ST_INVALID)
		return -ENOENT;
	if (r == ST_NOMEM)
		return -ENOMEM;
	if (r == ST_IO)
		return -EIO;
	if (r != ST_OK)
		return -EIO;
	return 0;
}

// Userspace struct statfs layout (must match user/lib/libc/include/sys/vfs.h)
typedef struct {
	unsigned long f_type;
	unsigned long f_bsize;
	unsigned long f_blocks;
	unsigned long f_bfree;
	unsigned long f_bavail;
	unsigned long f_files;
	unsigned long f_ffree;
	unsigned long f_fsid;
	unsigned long f_namelen;
	unsigned long f_frsize;
	unsigned long f_flags;
	unsigned long f_spare[4];
} user_statfs_t;

// statfs: get filesystem statistics for the given path
static int64_t sys_statfs(uint64_t u_path, uint64_t u_buf)
{
	if (!validate_user_ptr(u_buf, sizeof(user_statfs_t)))
		return -EFAULT;

	char kpath[VFS_MAX_PATH];
	size_t plen;
	int err = user_strnlen((const char *)u_path, VFS_MAX_PATH, &plen);
	if (err)
		return err;
	err = copy_from_user(kpath, (const void *)u_path, plen + 1);
	if (err)
		return err;
	err = canon_task_path(kpath, sizeof(kpath));
	if (err)
		return err;

	/* Route to the filesystem owning the path (devfs reports unsupported). */
	struct vfs_statfs vsf;
	mm_memset(&vsf, 0, sizeof(vsf));
	int r = vfs_statfs(kpath, &vsf);
	if (r == ST_UNSUPPORTED)
		return -ENOSYS;
	if (r != ST_OK)
		return -EIO;

	// Translate the generic struct to userspace layout
	user_statfs_t uinfo;
	mm_memset(&uinfo, 0, sizeof(uinfo));
	uinfo.f_type = vsf.f_type;
	uinfo.f_bsize = vsf.f_bsize;
	uinfo.f_blocks = vsf.f_blocks;
	uinfo.f_bfree = vsf.f_bfree;
	uinfo.f_bavail = vsf.f_bavail;
	uinfo.f_files = vsf.f_files;
	uinfo.f_ffree = vsf.f_ffree;
	uinfo.f_fsid = vsf.f_fsid;
	uinfo.f_namelen = vsf.f_namelen;
	uinfo.f_frsize = vsf.f_frsize;
	uinfo.f_flags = 0;

	return copy_to_user((void *)u_buf, &uinfo, sizeof(uinfo));
}

// fstatfs: get filesystem statistics for an open file descriptor
static int64_t sys_fstatfs(uint64_t fd, uint64_t u_buf)
{
	if (!validate_user_ptr(u_buf, sizeof(user_statfs_t)))
		return -EFAULT;
	if (fd >= MAX_FDS)
		return -EBADF;

	task_t *cur = sched_current();
	if (!cur || !task_fds(cur)[fd])
		return -EBADF;

	/* Stats of the filesystem the descriptor's file lives on.  Descriptors not
     * backed by a real file (stdio/pipe/socket) have no filesystem of their
     * own, so report the root filesystem instead of dereferencing them. */
	struct vfs_statfs vsf;
	mm_memset(&vsf, 0, sizeof(vsf));
	vfs_file_t *file = task_fds(cur)[fd];
	int r = fd_is_special(file) ? vfs_statfs("/", &vsf) :
				      vfs_fstatfs(file, &vsf);
	if (r == ST_UNSUPPORTED)
		return -ENOSYS;
	if (r != ST_OK)
		return -EIO;

	user_statfs_t uinfo;
	mm_memset(&uinfo, 0, sizeof(uinfo));
	uinfo.f_type = vsf.f_type;
	uinfo.f_bsize = vsf.f_bsize;
	uinfo.f_blocks = vsf.f_blocks;
	uinfo.f_bfree = vsf.f_bfree;
	uinfo.f_bavail = vsf.f_bavail;
	uinfo.f_files = vsf.f_files;
	uinfo.f_ffree = vsf.f_ffree;
	uinfo.f_fsid = vsf.f_fsid;
	uinfo.f_namelen = vsf.f_namelen;
	uinfo.f_frsize = vsf.f_frsize;
	uinfo.f_flags = 0;

	return copy_to_user((void *)u_buf, &uinfo, sizeof(uinfo));
}

/* ===================================================================
 * Extended attributes (xattr).  The path ops take a trailing nofollow flag so
 * libc's l*-variants reuse the same syscall number.  Values/lists are bounced
 * through a kernel buffer capped at one block.  Permission model (root bypasses;
 * only non-root is checked): get/list need ancestor search; set/remove need
 * write on the target, and trusted.* is root-only.
 * =================================================================== */
#define XATTR_MAX_VALUE 4096 /* one block; covers ibody + future block   */

static int xattr_copy_name(uint64_t u_name, char *kname /*[256]*/)
{
	if (!validate_user_ptr(u_name, 1))
		return -EFAULT;
	size_t nl;
	int e = user_strnlen((const char *)u_name, 255, &nl);
	if (e)
		return e;
	e = copy_from_user(kname, (const void *)u_name, nl + 1);
	if (e)
		return e;
	kname[nl] = '\0';
	return 0;
}
static int xattr_has_prefix(const char *s, const char *pfx)
{
	while (*pfx) {
		if (*s++ != *pfx++)
			return 0;
	}
	return 1;
}

/* Namespace policy for a non-root caller setting/removing xattr `name` on a file
 * owned by `owner_uid`.  The `system.` namespace (which holds the POSIX ACLs) is
 * owner-controlled, like chmod — write permission is not sufficient.  Returns:
 *   0      -> allowed by ownership (system.*)
 *  -EPERM  -> denied (trusted.* at all; system.* when not the owner)
 *   1      -> defer: caller must still verify write permission (user.* etc.) */
static int xattr_ns_perm(task_t *cur, const char *name, uint32_t owner_uid)
{
	if (xattr_has_prefix(name, "trusted."))
		return -EPERM;
	if (xattr_has_prefix(name, "system."))
		return (owner_uid == cur->cred.fsuid) ? 0 : -EPERM;
	return 1;
}

/* The syscall ABI here passes at most 5 args, so setxattr's nofollow is carried
 * in a private high bit of `flags` (libc's lsetxattr sets it). */
#define XATTR_SYS_NOFOLLOW 0x40000000
static int64_t sys_setxattr(uint64_t u_path, uint64_t u_name, uint64_t u_val,
			    uint64_t size, uint64_t flags)
{
	int nofollow = (flags & XATTR_SYS_NOFOLLOW) ? 1 : 0;
	flags &= ~(uint64_t)XATTR_SYS_NOFOLLOW;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	char kname[256];
	c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (size > XATTR_MAX_VALUE)
		return -ENOSPC;
	uint8_t *kval = 0;
	if (size) {
		if (!validate_user_ptr(u_val, size))
			return -EFAULT;
		kval = (uint8_t *)kalloc(size);
		if (!kval)
			return -ENOMEM;
		if (copy_from_user(kval, (const void *)u_val, size)) {
			kfree(kval);
			return -EFAULT;
		}
	}
	if (cur->cred.euid != 0) {
		struct kstat st;
		int sr =
			nofollow ? vfs_lstat(kpath, &st) : vfs_stat(kpath, &st);
		if (sr == ST_OK) {
			int np = xattr_ns_perm(cur, kname, (uint32_t)st.st_uid);
			if (np < 0) {
				if (kval)
					kfree(kval);
				return np;
			}
			if (np > 0) { /* user.* etc.: need write perm */
				int pr = perm_access(cur, kpath, &st, MAY_WRITE,
						     0);
				if (pr < 0) {
					if (kval)
						kfree(kval);
					return pr;
				}
			}
		}
	}
	int r = vfs_setxattr(kpath, (int)nofollow, kname, kval, size,
			     (int)flags);
	if (kval)
		kfree(kval);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}

static int64_t sys_getxattr(uint64_t u_path, uint64_t u_name, uint64_t u_val,
			    uint64_t size, uint64_t nofollow)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	char kname[256];
	c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
	}
	if (size == 0) { /* query value size */
		int r = vfs_getxattr(kpath, (int)nofollow, kname, 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	uint8_t *kbuf = (uint8_t *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_getxattr(kpath, (int)nofollow, kname, kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_val, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}

static int64_t sys_listxattr(uint64_t u_path, uint64_t u_list, uint64_t size,
			     uint64_t nofollow)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		int tr = perm_traverse(kpath);
		if (tr < 0)
			return tr;
	}
	if (size == 0) {
		int r = vfs_listxattr(kpath, (int)nofollow, 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	char *kbuf = (char *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_listxattr(kpath, (int)nofollow, kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_list, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}

static int64_t sys_removexattr(uint64_t u_path, uint64_t u_name,
			       uint64_t nofollow)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	char kpath[VFS_MAX_PATH];
	int c = copy_user_path((const char *)u_path, kpath, sizeof(kpath));
	if (c)
		return c;
	char kname[256];
	c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		struct kstat st;
		int sr =
			nofollow ? vfs_lstat(kpath, &st) : vfs_stat(kpath, &st);
		if (sr == ST_OK) {
			int np = xattr_ns_perm(cur, kname, (uint32_t)st.st_uid);
			if (np < 0)
				return np;
			if (np > 0) { /* user.* etc.: need write perm */
				int pr = perm_access(cur, kpath, &st, MAY_WRITE,
						     0);
				if (pr < 0)
					return pr;
			}
		}
	}
	int r = vfs_removexattr(kpath, (int)nofollow, kname);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}

static int64_t sys_fsetxattr(uint64_t fd, uint64_t u_name, uint64_t u_val,
			     uint64_t size, uint64_t flags)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	/* An extended attribute belongs to an inode; a marker has none, and
	 * passing one to the VFS would dereference it. */
	if (fd_is_special(task_fds(cur)[fd]))
		return -EOPNOTSUPP;
	char kname[256];
	int c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (size > XATTR_MAX_VALUE)
		return -ENOSPC;
	if (cur->cred.euid != 0) {
		if (xattr_has_prefix(kname, "trusted."))
			return -EPERM;
		if (xattr_has_prefix(
			    kname,
			    "system.")) { /* incl. POSIX ACLs: owner-only */
			struct kstat st;
			if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK &&
			    (uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
		}
	}
	uint8_t *kval = 0;
	if (size) {
		if (!validate_user_ptr(u_val, size))
			return -EFAULT;
		kval = (uint8_t *)kalloc(size);
		if (!kval)
			return -ENOMEM;
		if (copy_from_user(kval, (const void *)u_val, size)) {
			kfree(kval);
			return -EFAULT;
		}
	}
	int r = vfs_fsetxattr(task_fds(cur)[fd], kname, kval, size, (int)flags);
	if (kval)
		kfree(kval);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}

static int64_t sys_fgetxattr(uint64_t fd, uint64_t u_name, uint64_t u_val,
			     uint64_t size)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	if (fd_is_special(task_fds(cur)[fd]))
		return -EOPNOTSUPP;
	char kname[256];
	int c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (size == 0) {
		int r = vfs_fgetxattr(task_fds(cur)[fd], kname, 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	uint8_t *kbuf = (uint8_t *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_fgetxattr(task_fds(cur)[fd], kname, kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_val, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}

static int64_t sys_flistxattr(uint64_t fd, uint64_t u_list, uint64_t size)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	if (size == 0) {
		int r = vfs_flistxattr(task_fds(cur)[fd], 0, 0);
		return (r >= 0) ? r : vfs_status_to_errno(r);
	}
	unsigned long cap = size > XATTR_MAX_VALUE ? XATTR_MAX_VALUE : size;
	char *kbuf = (char *)kalloc(cap);
	if (!kbuf)
		return -ENOMEM;
	int r = vfs_flistxattr(task_fds(cur)[fd], kbuf, cap);
	if (r >= 0 && copy_to_user((void *)u_list, kbuf, r)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return (r >= 0) ? r : vfs_status_to_errno(r);
}

static int64_t sys_fremovexattr(uint64_t fd, uint64_t u_name)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS || !task_fds(cur)[fd])
		return -EBADF;
	char kname[256];
	int c = xattr_copy_name(u_name, kname);
	if (c)
		return c;
	if (cur->cred.euid != 0) {
		if (xattr_has_prefix(kname, "trusted."))
			return -EPERM;
		if (xattr_has_prefix(
			    kname,
			    "system.")) { /* incl. POSIX ACLs: owner-only */
			struct kstat st;
			if (vfs_fstat(task_fds(cur)[fd], &st) == ST_OK &&
			    (uint32_t)st.st_uid != cur->cred.fsuid)
				return -EPERM;
		}
	}
	int r = vfs_fremovexattr(task_fds(cur)[fd], kname);
	return (r >= 0) ? 0 : vfs_status_to_errno(r);
}

static int normalize_path(const char *base, const char *path, char *out,
			  size_t out_size)
{
	if (!path || !out || out_size < 2)
		return -EINVAL;
	const char *base_path = (base && base[0]) ? base : "/";
	char combined[VFS_MAX_PATH];
	size_t ci = 0;

	if (path[0] == '/') {
		// Absolute path: copy as-is into combined
		while (path[ci] && ci < sizeof(combined) - 1) {
			combined[ci] = path[ci];
			ci++;
		}
	} else {
		// Relative path: base + '/' + path
		size_t bi = 0;
		while (base_path[bi] && ci < sizeof(combined) - 1) {
			combined[ci++] = base_path[bi++];
		}
		if (ci == 0 || combined[ci - 1] != '/') {
			if (ci < sizeof(combined) - 1)
				combined[ci++] = '/';
		}
		size_t pi = 0;
		while (path[pi] && ci < sizeof(combined) - 1) {
			combined[ci++] = path[pi++];
		}
	}
	combined[ci] = '\0';

	// Normalize combined into out
	size_t out_len = 0;
	size_t seg_stack[64];
	size_t seg_top = 0;

	out[out_len++] = '/';
	size_t i = 0;
	while (combined[i]) {
		while (combined[i] == '/')
			i++;
		if (!combined[i])
			break;
		/* One name, which POSIX allows to be NAME_MAX bytes.
		 *
		 * This buffer used to hold 64, and the loop below simply
		 * stopped copying when it filled -- without advancing past the
		 * rest of the name.  The remainder was then taken for the NEXT
		 * component, so "…/averylongname.ext" quietly became
		 * "…/averylongnam/e.ext": a different file, in a directory that
		 * does not exist.  Every path with a component over 63
		 * characters was affected, which is why creating one could
		 * succeed and removing it could not. */
		char segment[VFS_NAME_MAX + 1];
		size_t si = 0;
		while (combined[i] && combined[i] != '/') {
			if (si >= sizeof(segment) - 1)
				return -ENAMETOOLONG;
			segment[si++] = combined[i++];
		}
		segment[si] = '\0';

		if (segment[0] == '\0' ||
		    (segment[0] == '.' && segment[1] == '\0')) {
			continue;
		}
		if (segment[0] == '.' && segment[1] == '.' &&
		    segment[2] == '\0') {
			if (seg_top > 0) {
				out_len = seg_stack[--seg_top];
				out[out_len] = '\0';
			} else {
				out_len = 1;
				out[1] = '\0';
			}
			continue;
		}

		if (out_len > 1 && out[out_len - 1] != '/') {
			if (out_len < out_size - 1)
				out[out_len++] = '/';
		}
		if (out_len >= out_size - 1)
			return -EINVAL;
		seg_stack[seg_top++] = out_len;
		for (size_t j = 0; j < si && out_len < out_size - 1; ++j) {
			out[out_len++] = segment[j];
		}
		out[out_len] = '\0';
		if (seg_top >= (sizeof(seg_stack) / sizeof(seg_stack[0]))) {
			return -EINVAL;
		}
	}

	if (out_len > 1 && out[out_len - 1] == '/') {
		out[out_len - 1] = '\0';
	} else {
		out[out_len] = '\0';
	}
	return 0;
}

/* Prepend the task's chroot root to an already-canonical absolute path.
 * `abs` starts with '/', has no ".." (normalize_path guarantees both), so the
 * result stays inside the jail.  A no-op when the task is not chrooted. */
static int apply_chroot(task_t *cur, char *abs, size_t out_size)
{
	if (!cur || cur->root[0] == '\0')
		return 0;
	size_t rlen = 0;
	while (cur->root[rlen])
		rlen++;
	/* "/" inside the jail is just the jail root itself. */
	size_t alen = 0;
	while (abs[alen])
		alen++;
	int only_slash = (alen == 1 && abs[0] == '/');
	size_t need = rlen + (only_slash ? 0 : alen) + 1;
	if (need > out_size)
		return -ENAMETOOLONG;
	/* Shift abs right by rlen (unless it is bare "/"), then copy root in. */
	if (only_slash) {
		for (size_t i = 0; i <= rlen; i++)
			abs[i] = cur->root[i];
	} else {
		for (size_t i = alen + 1; i-- > 0;)
			abs[i + rlen] = abs[i];
		for (size_t i = 0; i < rlen; i++)
			abs[i] = cur->root[i];
	}
	return 0;
}

static int build_at_path(task_t *cur, int dirfd, const char *path, char *out,
			 size_t out_size)
{
	const char *base;

	if (!cur || !path || !out || out_size < 2)
		return -EINVAL;

	/* An ABSOLUTE path ignores dirfd entirely, as POSIX requires -- the
	 * descriptor is not even required to be valid in that case. */
	if (path[0] == '/' || dirfd == AT_FDCWD) {
		base = (cur->cwd[0] != 0) ? cur->cwd : "/";
	} else {
		/* Relative to the directory the descriptor refers to.
		 *
		 * This used to return ENOTDIR for every dirfd that was not
		 * AT_FDCWD, which meant the whole *at() family silently did
		 * not work: openat(), fstatat(), faccessat() and unlinkat()
		 * all fail the moment a caller passes a real descriptor, which
		 * is the entire reason those calls exist. */
		vfs_file_t *df;

		if (dirfd < 0 || dirfd >= (int)TASK_MAX_FDS)
			return -EBADF;
		df = task_fds(cur)[dirfd];
		if (!df)
			return -EBADF;
		/* The marker descriptors (sockets, epoll, pipes, the console)
		 * are not files and have no path; a directory is required. */
		if (IS_SOCKET_FD(df) || unix_sock_is(df) || IS_EPOLL_FD(df) ||
		    pipe_is_end(df) || (uintptr_t)df <= 3)
			return -ENOTDIR;
		if (!df->at_path)
			return -ENOTDIR;
		/* It must really be a directory: resolving "file" against a
		 * regular file would otherwise invent a path that looks valid
		 * and refers to nothing. */
		{
			struct kstat dst;
			if (vfs_fstat(df, &dst) != ST_OK)
				return -ENOTDIR;
			if (!S_ISDIR(dst.st_mode))
				return -ENOTDIR;
		}
		base = df->at_path;
	}

	int r = normalize_path(base, path, out, out_size);
	if (r != 0)
		return r;
	return apply_chroot(cur, out, out_size);
}

// SYS_BRK - set program break
/* ---- address-space syscalls -------------------------------------------
 *
 * Every syscall that changes the SHAPE of the address space runs with the
 * address-space semaphore held for writing, so it cannot race a page fault
 * (which holds it for reading) on the same address space.  Threads share one
 * page table, so without this an munmap and a fault on the same address ran
 * concurrently by default.
 *
 * The bodies are written as *_locked() helpers with the ordinary many-return
 * style, and the wrapper does the acquire/release exactly once -- adding an
 * unlock to every return path is how one gets missed.
 */
#define RUN_WRITE_LOCKED(call)                       \
	do {                                         \
		task_t *__cur = sched_current();     \
		task_t *__mm = task_mm_owner(__cur); \
		int64_t __ret;                       \
		if (!__mm)                           \
			return -EFAULT;              \
		mm_write_lock(&__mm->mmap_lock);     \
		__ret = (call);                      \
		mm_write_unlock(&__mm->mmap_lock);   \
		return __ret;                        \
	} while (0)

static int64_t sys_brk_locked(uint64_t new_brk)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	/* Threads share the leader's heap — all brk bookkeeping (and the
	 * fault handler's validity check) goes through the group leader. */
	cur = task_mm_owner(cur);

	// If new_brk is 0, return current break
	if (new_brk == 0) {
		return (int64_t)cur->brk;
	}

	// Validate new break is reasonable
	if (new_brk < cur->brk_start) {
		return (int64_t)cur->brk; // Can't shrink below start
	}

	// Don't let heap grow into stack area
	if (new_brk >= cur->user_stack_top - (2 * 1024 * 1024)) {
		return (int64_t)cur->brk; // Would collide with stack
	}

	/* Growing the heap: demand-paged — no pages are allocated here.  The
	 * page-fault handler zero-fills anything in [brk_start, brk) on first
	 * touch, so growing the break is just bookkeeping.  Shrinking keeps
	 * the pages mapped (as before). */
	cur->brk = new_brk;
	return (int64_t)new_brk;
}

static int64_t sys_brk(uint64_t new_brk)
{
	RUN_WRITE_LOCKED(sys_brk_locked(new_brk));
}

// SYS_MMAP - map memory
static int64_t sys_mmap_locked(uint64_t addr, uint64_t length, uint64_t prot,
			       uint64_t flags, uint64_t fd, uint64_t offset)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ENOMEM;
	/* fd lookup below stays on the calling task; the region table,
	 * mmap_base and pml4 belong to the thread-group leader. */
	task_t *caller = cur;
	cur = task_mm_owner(cur);

	// Security: Validate length - must be non-zero and reasonable
	if (length == 0) {
		return -EINVAL;
	}

	// Security: Prevent integer overflow when aligning length
	if (length > 0x7FFFFFFFFFFFFFF0ULL) {
		return -ENOMEM; // Would overflow during PAGE_ALIGN
	}

	// Round up length to page size
	length = PAGE_ALIGN(length);

	// Security: Prevent excessive allocation (max 2GB per mmap call)
	if (length > (2ULL * 1024 * 1024 * 1024)) {
		return -ENOMEM;
	}

	// Find a free mmap region slot
	mmap_region_t *region = mm_alloc_mmap_region(cur);
	if (!region) {
		/* Loud on purpose: a process that runs out of region slots
		 * fails every subsequent mmap, which downstream looks like a
		 * random allocation crash rather than a table limit -- a
		 * dlopen() failing here is reported by the loader as "cannot
		 * find", which sends the reader looking for a missing file.
		 *
		 * The breakdown says WHY the table is full, which the bare
		 * count does not: file-backed entries are libraries and their
		 * segments (four per shared object, so a large dependency graph
		 * alone accounts for hundreds), while anonymous ones are heap,
		 * thread stacks and large allocations.  Whichever dominates is
		 * where to look. */
		int n_file = 0, n_anon = 0, n_lazy = 0;
		uint64_t anon_bytes = 0;

		for (uint32_t i = 0; i < cur->mmap_capacity; i++) {
			mmap_region_t *r = &cur->mmap_regions[i];

			if (!r->in_use)
				continue;
			if (r->file) {
				n_file++;
			} else {
				n_anon++;
				anon_bytes += r->length;
			}
			if (r->lazy)
				n_lazy++;
		}
		WARN_RATELIMIT(
			1,
			"mmap: pid %d out of mmap regions (max %d): %d file-backed, %d anonymous (%llu KB), %d lazy",
			cur->id, TASK_MAX_MMAP, n_file, n_anon,
			(unsigned long long)(anon_bytes / 1024), n_lazy);
		return -ENOMEM;
	}

	// Determine virtual address
	uint64_t vaddr;
	if (flags & MAP_FIXED) {
		if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
			return -EINVAL; // Invalid fixed address
		}
		// Security: Reject mappings below 64KB to prevent NULL deref exploits
		if (addr < 0x10000) {
			return -EINVAL;
		}
		vaddr = addr;

		/* MAP_FIXED replaces whatever is already here, so tear the old
		 * mapping down properly: free its pages AND release its region
		 * records.  Doing this for every MAP_FIXED path (rather than
		 * only the lazy one, which is all that used to unmap anything)
		 * is what keeps the region table from filling up -- rtld maps
		 * every DSO segment this way.  It also stops a stale record
		 * from shadowing the new mapping in mm_find_mmap_region(), which
		 * would report the old file and protection for these pages. */
		mm_unmap_range_and_regions(cur, vaddr, length);
	} else {
		// Allocate from mmap area (grows down from below stack)
		// Move base down first, then return the new base as the start of the mapped region
		cur->mmap_base -= length;
		if (cur->mmap_base < cur->brk + (4 * 1024 * 1024)) {
			// Too close to heap
			cur->mmap_base += length; // Rollback
			return -ENOMEM;
		}
		// Security: Reject mappings below 64KB to prevent NULL deref exploits
		if (cur->mmap_base < 0x10000) {
			cur->mmap_base += length; // Rollback
			return -ENOMEM;
		}
		vaddr = cur->mmap_base;
	}

	// Calculate page flags
	uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
	if (prot & PROT_WRITE) {
		page_flags |= PAGE_WRITABLE;
	}
	if (!(prot & PROT_EXEC)) {
		page_flags |= PAGE_NO_EXECUTE;
	}

	bool is_anonymous = (flags & MAP_ANONYMOUS) || (int64_t)fd == -1;

	/* Resolve and validate the backing file up front (also needed for the
	 * lazy path).  Only real VFS files can back a mapping — socket/pipe/
	 * epoll fd markers and stdio placeholders cannot. */
	vfs_file_t *backing = NULL;
	if (!is_anonymous) {
		if (fd >= TASK_MAX_FDS || !task_fds(caller)[fd])
			return -EBADF;
		uint64_t marker = (uint64_t)task_fds(caller)[fd];
		if (marker <= 3 || IS_SOCKET_FD(task_fds(caller)[fd]) ||
		    unix_sock_is(task_fds(caller)[fd]) ||
		    IS_EPOLL_FD(task_fds(caller)[fd]) ||
		    pipe_is_end(task_fds(caller)[fd]))
			return -ENODEV;
		backing = task_fds(caller)[fd];
	}

	/* Device mapping: /dev/fb0 maps the framebuffer BAR itself.  Pages
	 * are mapped eagerly with PAGE_DEVICE PTEs (never freed back to the
	 * physical allocator, shared across fork) and write-combining
	 * caching (PWT selects PAT entry 1, programmed WC at boot). */
	/* Shared memory: map the OBJECT's own physical pages.  This is the one
	 * mapping in the system that is shared between processes with no fork
	 * relationship — the generic MAP_SHARED path below allocates fresh
	 * pages per process, which is fine for anonymous memory but would give
	 * every opener of a /dev/shm object its own private copy. */
	if (backing) {
		shm_object_t *sobj = devfs_shm_object(backing);
		if (sobj) {
			if (!(flags & MAP_SHARED)) {
				/* A private mapping of shared memory is legal
				 * but pointless here, and implementing it means
				 * copy-on-write over borrowed pages.  Say so
				 * rather than silently sharing. */
				return -EOPNOTSUPP;
			}
			if ((offset & (PAGE_SIZE - 1)) != 0)
				return -EINVAL;
			if (offset + length > sobj->size)
				return -EINVAL; /* past the object's length */

			/* PAGE_DEVICE marks these as pages this address space
			 * does not own: teardown must not hand them back to the
			 * physical allocator, and fork shares rather than copies
			 * them.  They belong to the shm object and are released
			 * only when it is destroyed.  Without this bit every
			 * munmap frees memory the object still owns — the pages
			 * get reused underneath it and are freed a second time
			 * later. */
			uint64_t shm_flags = page_flags | PAGE_DEVICE;

			for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
				uint64_t phys = shm_page_phys(
					sobj, (offset + off) / PAGE_SIZE);
				if (!phys || !mm_map_page_in_address_space(
						     cur->pml4, vaddr + off,
						     phys, shm_flags)) {
					for (uint64_t cl = 0; cl < off;
					     cl += PAGE_SIZE)
						mm_unmap_page_in_address_space(
							cur->pml4, vaddr + cl);
					if (!(flags & MAP_FIXED))
						cur->mmap_base += length;
					return -ENOMEM;
				}
			}
			/* Pin the object for the life of the mapping: the
			 * region holds a vfs reference, and unmapping releases
			 * it through the normal file teardown. */
			vfs_incref(backing);
			region->start = vaddr;
			region->length = length;
			region->prot = prot;
			region->flags = flags | MAP_SHARED;
			region->fd = (int)fd;
			region->offset = offset;
			region->lazy = false;
			region->file = backing;
			/* Marked as a device mapping so fork() shares the pages
			 * instead of copying them, and teardown never hands
			 * them back to the physical allocator — they belong to
			 * the shm object, not to this address space. */
			region->device = true;
			region->device_phys =
				shm_page_phys(sobj, offset / PAGE_SIZE);
			region->in_use = true;
			mm_merge_region_neighbours(cur, region);
			return (int64_t)vaddr;
		}
	}

	if (backing && devfs_is_fb0(backing)) {
		uint64_t dev_phys = fbdev_mmap_phys(offset, length);
		uint64_t dev_flags =
			page_flags | PAGE_DEVICE | PAGE_WRITE_THROUGH;

		if (!dev_phys) {
			if (!(flags & MAP_FIXED))
				cur->mmap_base += length; // Rollback
			return -ENODEV;
		}
		for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
			if (!mm_map_page_in_address_space(
				    cur->pml4, vaddr + off, dev_phys + off,
				    dev_flags)) {
				for (uint64_t cl = 0; cl < off; cl += PAGE_SIZE)
					mm_unmap_page_in_address_space(
						cur->pml4, vaddr + cl);
				if (!(flags & MAP_FIXED))
					cur->mmap_base += length; // Rollback
				return -ENOMEM;
			}
		}
		vfs_incref(backing);
		region->start = vaddr;
		region->length = length;
		region->prot = prot;
		/* Force shared semantics: fork must share the device pages,
		 * never COW them. */
		region->flags = flags | MAP_SHARED;
		region->fd = (int)fd;
		region->offset = offset;
		region->lazy = false;
		region->file = backing;
		region->device = true;
		region->device_phys = dev_phys;
		region->in_use = true;
		mm_merge_region_neighbours(cur, region);
		return (int64_t)vaddr;
	}

	/* Demand paging: PRIVATE mappings (anonymous or file-backed) are not
	 * populated here at all — the page-fault handler materialises pages
	 * on first touch (zero-fill / file page-in).  Only MAP_SHARED stays
	 * eager: fork() must find real pages to share.
	 *
	 * MAP_FIXED over an existing mapping must not leave stale pages in
	 * place (a lazy region would otherwise never fault there and expose
	 * the old contents).  That teardown now happens for every MAP_FIXED
	 * path where vaddr is settled, above, which also stops the eager path
	 * from silently overwriting live PTEs and leaking the pages. */
	/* PROT_NONE takes the lazy path even when MAP_SHARED is asked for: the
	 * mapping has no accessible contents, so there is nothing for the eager
	 * path to share, and the fault handler above already refuses PROT_NONE
	 * regions.  Mapping it eagerly would hand out PAGE_PRESENT|PAGE_USER
	 * pages that are freely readable — i.e. PROT_NONE would not protect. */
	bool prot_none = !(prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
	if (!(flags & MAP_SHARED) || prot_none) {
		/* No unmap here: MAP_FIXED already tore down the old mapping,
		 * pages and region records both, where vaddr was settled. */
		if (backing)
			vfs_incref(backing);
		region->start = vaddr;
		region->length = length;
		region->prot = prot;
		region->flags = flags;
		region->fd = is_anonymous ? -1 : (int)fd;
		region->offset = offset;
		region->lazy = true;
		region->file = backing;
		region->in_use = true;
		mm_merge_region_neighbours(cur, region);
		return (int64_t)vaddr;
	}

	// Map pages (eager, MAP_SHARED only)
	uint64_t pages_mapped = 0;

	for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
		uint64_t phys = mm_allocate_physical_page();
		if (!phys) {
			// Unmap already-mapped pages on failure
			for (uint64_t cleanup = 0; cleanup < off;
			     cleanup += PAGE_SIZE) {
				mm_unmap_page_in_address_space(cur->pml4,
							       vaddr + cleanup);
			}
			if (!(flags & MAP_FIXED)) {
				cur->mmap_base += length; // Rollback
			}
			return -ENOMEM;
		}

		/* No memset in production: mm_allocate_physical_page already
		 * zeroed the page (double-zeroing every anonymous page slowed
		 * each process start — rtld/malloc mmap hundreds of pages).
		 * DEBUG builds poison on alloc, so zero explicitly there:
		 * anon mmap pages must read as zero in userspace. */
#if DEBUG
		mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
#endif

		// For file-backed mappings, read content from file
		if (backing) {
			// Seek to the correct position and read into direct-mapped address
			long file_off = (long)(offset + off);
			if (vfs_seek(backing, file_off, SEEK_SET) >= 0) {
				vfs_read(backing, phys_to_virt(phys),
					 PAGE_SIZE);
			}
		}

		if (!mm_map_page_in_address_space(cur->pml4, vaddr + off, phys,
						  page_flags)) {
			mm_free_physical_page(phys);
			// Unmap already-mapped pages on failure
			for (uint64_t cleanup = 0; cleanup < off;
			     cleanup += PAGE_SIZE) {
				mm_unmap_page_in_address_space(cur->pml4,
							       vaddr + cleanup);
			}
			if (!(flags & MAP_FIXED)) {
				cur->mmap_base += length; // Rollback
			}
			return -ENOMEM;
		}
		pages_mapped++;
	}

	// Record the mapping
	region->start = vaddr;
	region->length = length;
	region->prot = prot;
	region->flags = flags;
	region->fd = is_anonymous ? -1 : (int)fd;
	region->offset = offset;
	region->lazy = false;
	region->file = NULL;
	region->in_use = true;
	mm_merge_region_neighbours(cur, region);

	return (int64_t)vaddr;
}

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
			uint64_t flags, uint64_t fd, uint64_t offset)
{
	RUN_WRITE_LOCKED(
		sys_mmap_locked(addr, length, prot, flags, fd, offset));
}

// SYS_MUNMAP - unmap memory
static int64_t sys_munmap_locked(uint64_t addr, uint64_t length)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	cur = task_mm_owner(cur); // regions/pml4 live on the group leader

	if (addr == 0 || length == 0) {
		return -EINVAL;
	}
	if (addr & (PAGE_SIZE - 1)) {
		return -EINVAL;
	}

	length = PAGE_ALIGN(length);

	/* Handles the common case of a single munmap call covering multiple
	 * contiguous MAP_FIXED regions (e.g. the full span of a DSO). */
	return mm_unmap_range_and_regions(cur, addr, length) ? 0 : -EINVAL;
}

static int64_t sys_munmap(uint64_t addr, uint64_t length)
{
	RUN_WRITE_LOCKED(sys_munmap_locked(addr, length));
}

// SYS_PIPE - create a pipe
static int64_t sys_pipe(uint64_t pipefd_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	if (!validate_user_ptr(pipefd_ptr, sizeof(int) * 2)) {
		return -EFAULT;
	}

	pipe_t *pipe = pipe_create(4096);
	if (!pipe) {
		return -ENOMEM;
	}

	pipe_end_t *read_end = pipe_create_end(pipe, true);
	if (!read_end) {
		if (pipe->buffer) {
			kfree(pipe->buffer);
		}
		kfree(pipe);
		return -ENOMEM;
	}

	pipe_end_t *write_end = pipe_create_end(pipe, false);
	if (!write_end) {
		pipe_close_end(read_end);
		return -ENOMEM;
	}

	/* Installing the read end also reserves its slot, so the write end
	 * cannot land on the same number. */
	int fd_read = fd_install(cur, (vfs_file_t *)read_end);
	if (fd_read < 0) {
		pipe_close_end(read_end);
		pipe_close_end(write_end);
		return fd_read;
	}

	int fd_write = fd_install(cur, (vfs_file_t *)write_end);
	if (fd_write < 0) {
		task_fds(cur)[fd_read] = NULL;
		pipe_close_end(read_end);
		pipe_close_end(write_end);
		return fd_write;
	}

	// SMAP-aware write to user array
	int *user_pipefd = (int *)pipefd_ptr;
	smap_disable();
	user_pipefd[0] = fd_read;
	user_pipefd[1] = fd_write;
	smap_enable();

	return 0;
}

// SYS_EXIT - exit task
__attribute__((noreturn)) static void sys_exit(uint64_t status)
{
	task_t *cur = sched_current();
	if (cur) {
		sched_mark_task_exited(cur, (int)status);
	}
	/* Switch away for good.  sched_schedule() normally never returns for an
     * exited task — it switches to another task (or idle) and this frame is
     * abandoned.  But it CAN return in rare scheduler edge cases (e.g. the
     * idle task was momentarily unselectable on this CPU, so the zombie is
     * left as current_task).  In that case we must NOT park in `cli; hlt`:
     * a CPU halted with interrupts disabled can no longer ack TLB-shootdown
     * IPIs, which permanently wedges every other CPU spinning in
     * smp_tlb_shootdown_sync() — observed as endless
     * "TLB shootdown sync timeout (gen=...)" with this CPU's gen frozen and
     * a diagnostic NMI sampling it stuck at this very loop.
     *
     * Keep interrupts ENABLED and retry: with IRQs on, the TLB-shootdown IPI
     * is serviced (unblocking the rest of the system) and the timer's
     * sched_preempt() evacuates this zombie to the idle task via its own
     * idle fallback, so we are switched off this CPU at the next tick. */
	sched_exit_park();
}

// DEPRECATED: These global externs are no longer used directly.
// Syscall context is now stored per-CPU in percpu_t and copied to task->syscall_*.
// Kept for backward compatibility but should not be referenced in new code.
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;

// External: user_mode_iret_trampoline from syscall.asm
extern void user_mode_iret_trampoline(void);
extern void fork_child_return(void);

// SYS_FORK - fork current process
static int64_t sys_fork(void)
{
	task_t *cur = sched_current();
	if (!cur || cur->privilege != TASK_USER) {
		return -1;
	}

	// Use task-local copies of user context, not globals!
	// Globals can be overwritten if preemption switches to another task.
	uint64_t user_rip = cur->syscall_rip; // Where to resume execution
	uint64_t user_rsp = cur->syscall_rsp; // User stack pointer
	uint64_t user_rflags = cur->syscall_rflags; // Saved RFLAGS

	// Create child with cloned address space and file descriptors
	task_t *child = sched_fork_current();
	if (!child) {
		return -1; // Fork failed
	}

	// Set up child's kernel stack to return to userspace
	// When the child is scheduled, it will resume at user_rip with fork() returning 0
	//
	// Stack layout (from top to bottom):
	// 1. Saved user callee-saved registers (RBP, RBX, R12-R15)
	// 2. IRET frame: SS, RSP, RFLAGS, CS, RIP (to return to userspace)
	// 3. RAX value (0 for child's fork return value)
	// 4. Saved callee-saved registers for ctx_switch_asm (r15-rbp)
	// 5. Return address (fork_child_return trampoline)

	uint64_t *k_sp = (uint64_t *)child->kernel_stack_top;
	k_sp = (uint64_t *)((uint64_t)k_sp & ~0xFUL); // Align to 16 bytes

	// Push user callee-saved registers (fork_child_return will restore these)
	// IMPORTANT: Use task-local copies (cur->syscall_*) not globals!
	// Globals can be overwritten by preemption switching to another task.
	*(--k_sp) = cur->syscall_r15;
	*(--k_sp) = cur->syscall_r14;
	*(--k_sp) = cur->syscall_r13;
	*(--k_sp) = cur->syscall_r12;
	*(--k_sp) = cur->syscall_rbx;
	*(--k_sp) = cur->syscall_rbp;

	// Push IRET frame (used by fork_child_return to return to userspace)
	*(--k_sp) = 0x1B; // SS: user data segment
	*(--k_sp) = user_rsp; // User stack pointer
	*(--k_sp) = user_rflags | 0x200; // RFLAGS with interrupts enabled
	*(--k_sp) = 0x23; // CS: user code segment
	*(--k_sp) = user_rip; // Resume at parent's fork() call site

	// Push fork return value for child (0)
	*(--k_sp) = 0; // RAX = 0 (child sees fork() return 0)

	// Push callee-saved registers (ctx_switch_asm will restore these)
	*(--k_sp) = (uint64_t)
		fork_child_return; // Return address: sets RAX=0 and does IRET
	// RFLAGS restored by ctx_switch_asm's popfq.  IF set, matching the state a
	// fresh child used to inherit from the switching path's sti (sched_schedule
	// / sched_run_ready both sti *before* ctx_switch_asm).  fork_child_return
	// only requires that sched_after_fork_child run before its iretq, not that
	// interrupts be off.
	*(--k_sp) = 0x202; // RFLAGS (kernel): reserved bit 1 + IF
	*(--k_sp) = 0; // RBP (kernel)
	*(--k_sp) = 0; // RBX (kernel)
	*(--k_sp) = 0; // R12 (kernel)
	*(--k_sp) = 0; // R13 (kernel)
	*(--k_sp) = 0; // R14 (kernel)
	*(--k_sp) = 0; // R15 (kernel)

	child->sp = k_sp;

	// CRITICAL: Save child PID BEFORE enqueueing!
	// On SMP, another CPU might run the child, it exits, and dead_thread_reap
	// frees it before we return from sched_enqueue_ready. Accessing child->id
	// after enqueue would be use-after-free.
	int32_t child_pid = child->id;

	sched_enqueue_ready(child);

	// Set need_resched so the parent yields at the next opportunity,
	// giving the new child process a chance to start promptly.
	cur->need_resched = 1;

	// Parent returns child's PID (saved before enqueue to avoid use-after-free)
	return child_pid;
}

// SYS_WAIT4/SYS_WAITPID - wait for child process
// In a preemptive kernel, this BLOCKS until a child exits (unless WNOHANG)
static int64_t sys_waitpid(int64_t pid, uint64_t status_ptr, uint64_t options,
			   uint64_t rusage_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Check if there are any children at all
	/* Children hang off the thread-group leader: they belong to the process,
	 * so any thread of it may wait for them.  Only the child list is read
	 * through the leader -- everything else here, above all the blocking,
	 * belongs to the thread that actually called. */
	task_t *owner = cur->group_leader ? cur->group_leader : cur;

	if (!owner->first_child) {
		return -ECHILD;
	}

	// Loop until we find a reportable child or get interrupted
	while (1) {
		task_t *child = NULL; /* zombie to reap */
		task_t *stopped = NULL; /* WUNTRACED: freshly stopped child */
		task_t *continued = NULL; /* WCONTINUED: freshly continued */
		int matched_any = 0;

		/* One scan of the child list handles every pid form:
		 *   pid > 0   that child only
		 *   pid == -1 any child
		 *   pid == 0  any child in the caller's process group
		 *   pid < -1  any child in process group -pid */
		/* Under g_wait_lock: the list is edited from other CPUs (a
		 * child exiting, a dying ancestor reparenting a whole list into
		 * ours) and walking it unlocked both reads half-spliced links
		 * and can miss the very child we are about to sleep for.  Only
		 * the walk is inside -- the copy_to_user()s below must not run
		 * with a spinlock held. */
		uint64_t scan_flags;

		spin_lock_irqsave(&g_wait_lock, &scan_flags);
		for (task_t *c = owner->first_child; c; c = c->next_sibling) {
			if (pid > 0 && c->id != (uint32_t)pid)
				continue;
			if (pid == 0 && c->pgid != cur->pgid)
				continue;
			if (pid < -1 && c->pgid != (int)(-pid))
				continue;
			matched_any = 1;
			if (c->has_exited) {
				child = c;
				break;
			}
			if ((options & 2 /* WUNTRACED */) && c->jc_stop_signo &&
			    !stopped)
				stopped = c;
			if ((options & 8 /* WCONTINUED */) && c->jc_continued &&
			    !continued)
				continued = c;
		}
		spin_unlock_irqrestore(&g_wait_lock, scan_flags);

		if (!matched_any)
			return -ECHILD;

		/* Job-control events: report without reaping. */
		if (!child && stopped) {
			int signo = stopped->jc_stop_signo;
			stopped->jc_stop_signo = 0;
			int status = ((signo & 0xFF) << 8) | 0x7F;
			if (status_ptr &&
			    validate_user_ptr(status_ptr, sizeof(int)))
				copy_to_user((void *)status_ptr, &status,
					     sizeof(status));
			return stopped->id;
		}
		if (!child && continued) {
			continued->jc_continued = 0;
			int status = 0xFFFF; /* conventional "continued" code */
			if (status_ptr &&
			    validate_user_ptr(status_ptr, sizeof(int)))
				copy_to_user((void *)status_ptr, &status,
					     sizeof(status));
			return continued->id;
		}

		if (child) {
			// Found a zombie child - reap it
			int status = 0;
			if (child->term_sig != 0) {
				// Killed by a signal: WIFSIGNALED, low 7 bits =
				// signal number.  Distinguished from exit(128+N)
				// by the explicit term_sig flag — exit_code alone
				// is ambiguous (a program exiting with a status
				// >= 128, e.g. ssh's 255, is NOT a signal death).
				status = child->term_sig & 0x7F;
			} else {
				// Normal exit: exit_code << 8 (WIFEXITED)
				status = (child->exit_code & 0xFF) << 8;
			}

			if (status_ptr &&
			    validate_user_ptr(status_ptr, sizeof(int))) {
				copy_to_user((void *)status_ptr, &status,
					     sizeof(status));
			}

			/* Fill in resource usage from the child's accounting data */
			if (rusage_ptr && validate_user_ptr(rusage_ptr, 56)) {
				uint32_t freq = timer_get_frequency();
				if (freq == 0)
					freq = 100;
				struct {
					long ru_utime_sec;
					long ru_utime_usec;
					long ru_stime_sec;
					long ru_stime_usec;
					long ru_maxrss;
					long ru_minflt;
					long ru_majflt;
				} ru;
				ru.ru_utime_sec =
					(long)(child->utime_ticks / freq);
				ru.ru_utime_usec =
					(long)((child->utime_ticks % freq) *
					       (1000000 / freq));
				ru.ru_stime_sec =
					(long)(child->stime_ticks / freq);
				ru.ru_stime_usec =
					(long)((child->stime_ticks % freq) *
					       (1000000 / freq));
				ru.ru_maxrss = 0;
				ru.ru_minflt = 0;
				ru.ru_majflt = 0;
				copy_to_user((void *)rusage_ptr, &ru,
					     sizeof(ru));
			}

			int child_pid = child->id;
			/* Heavy destruction (address space, kernel stack, TLB
			 * shootdown, still-running spin) is deferred to the
			 * dead-thread reaper; running it synchronously here
			 * cost 7-9 ms per reaped child. */
			sched_defer_reap(child);
			return child_pid;
		}

		// No zombie child yet
		if (options & 1) { // WNOHANG
			return 0; // No child exited yet, return immediately
		}

		// Check for pending signal BEFORE blocking - avoids race condition
		if (signal_pending(cur)) {
			return -EINTR;
		}

		/* Block until a child exits or we get a signal.
		 *
		 * The re-check and the transition to BLOCKED happen under
		 * g_wait_lock, which is the same lock a child takes to look at
		 * whether its parent is asleep here.  That is what makes the
		 * decision safe: if a child exits during this window it either
		 * gets the lock first — and we see has_exited below and never
		 * sleep — or it gets it after us, and finds us already BLOCKED
		 * with wait_channel set, so it wakes us.
		 *
		 * Interrupts-off alone (what this used to do) excludes a timer
		 * on THIS cpu and nothing at all on the others, so on SMP the
		 * wake could fall in the gap.  Nothing recovers from that: a
		 * waitpid sleeper arms no timeout, and SIGCHLD's default action
		 * is ignore so it is never left pending — the periodic sweep
		 * that re-wakes blocked tasks has no reason to touch it, and it
		 * sleeps for good.
		 *
		 * Also caught here: a child that vanished entirely (a parent
		 * ignoring SIGCHLD has its children reaped as they exit).  The
		 * list can go empty while we are in this loop, and the scan at
		 * the top of it turns that into the ECHILD it should be. */
		uint64_t irq_flags;
		spin_lock_irqsave(&g_wait_lock, &irq_flags);

		bool found_zombie = false;
		task_t *zombie_check = owner->first_child;
		while (zombie_check) {
			if (zombie_check->has_exited ||
			    ((options & 2) && zombie_check->jc_stop_signo) ||
			    ((options & 8) && zombie_check->jc_continued)) {
				found_zombie = true;
				break;
			}
			zombie_check = zombie_check->next_sibling;
		}

		if (found_zombie || !owner->first_child) {
			/* Something to report, or nothing left to wait for:
			 * either way the top of the loop decides, not us. */
			spin_unlock_irqrestore(&g_wait_lock, irq_flags);
			continue;
		}

		/* wait_channel before state: a reader that sees BLOCKED must
		 * also see the channel it is blocked on.  (Both stores are
		 * under the lock, so this only matters for the lock-free
		 * glances other wakers take.) */
		cur->wait_channel = cur; // Waiting for our own children
		cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&g_wait_lock, irq_flags);

		sched_schedule();
		// NOTE: Do NOT set cur->state = TASK_READY here!
		// When sched_schedule() returns, the scheduler has already set us
		// to TASK_RUNNING.  Overwriting with TASK_READY causes a race on SMP
		// where sched_wake_expired_sleepers sees READY + !on_rq and enqueues
		// us on another CPU while we're still running → double scheduling.
		cur->wait_channel = 0;

		// Check if we were woken by a signal
		if (signal_pending(cur)) {
			return -EINTR;
		}

		// Check if we were terminated
		if (cur->has_exited) {
			return -EINTR;
		}

		// Loop back to check for zombie children again
	}
}

// SYS_EXECVE - execute a new program, replacing current process image
// This is the POSIX-compliant version that replaces the current task
/* Per-exec-level DAC screen: stat the target (also needed later for set-id
 * application) and, for a non-root caller, require search on every ancestor
 * directory plus execute on the file itself.  Permissive if it can't be
 * stat'd (e.g. a relative path elf_exec_replace resolves itself).  Run once
 * for the exec target and once per shebang interpreter level. */
static int execve_check_exec(const char *kpath, struct kstat *xst,
			     int *have_xst)
{
	*have_xst = (vfs_stat(kpath, xst) == ST_OK);
	task_t *cur = sched_current();
	if (cur && cur->cred.euid != 0) {
		int pr = perm_traverse(kpath); /* search on ancestor dirs */
		if (pr == 0 && *have_xst) /* + execute on the file itself */
			pr = perm_access(cur, kpath, xst, MAY_EXEC, 0);
		if (pr < 0)
			return pr;
	}
	return 0;
}

static int64_t sys_execve(uint64_t pathname, uint64_t argv_ptr,
			  uint64_t envp_ptr)
{
	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	const char *user_path = (const char *)pathname;
	const char *const *user_argv = (const char *const *)argv_ptr;
	const char *const *user_envp = (const char *const *)envp_ptr;

	char *kpath = NULL;
	char **kargv = NULL;
	char **kenvp = NULL;

	int ret = copy_user_string(user_path, VFS_MAX_PATH, &kpath, NULL);
	if (ret != 0) {
		return ret;
	}

	ret = copy_user_string_array(user_argv, 128, 4096, 16384, &kargv);
	if (ret != 0) {
		kfree(kpath);
		return ret;
	}

	ret = copy_user_string_array(user_envp, 128, 4096, 16384, &kenvp);
	if (ret != 0) {
		free_user_string_array(kargv);
		kfree(kpath);
		return ret;
	}

	/* Exec-permission screen + shebang (#!) resolution.  Each iteration
	 * checks DAC on the current target (denying BEFORE the image is
	 * replaced, after which an error can no longer be returned) and then
	 * asks the script loader to rewrite path/argv one interpreter level.
	 * The loop ends at the first non-script target; xst then describes
	 * the FINAL binary, so set-id bits on scripts are naturally ignored
	 * while an interpreter's own set-id bits still apply.  kenvp is never
	 * touched: the environment passes through unchanged. */
	struct kstat xst;
	int have_xst = 0;
	for (int depth = 0;; depth++) {
		ret = execve_check_exec(kpath, &xst, &have_xst);
		if (ret < 0)
			goto out_err;
		int sr = script_load_rewrite(&kpath, &kargv, depth);
		if (sr == 0)
			break; /* not a script: kpath is the final binary */
		if (sr < 0) {
			ret = sr;
			goto out_err;
		}
		/* sr == 1: kpath now names the interpreter; re-check it */
	}
	ret = script_check_stack_fit(kargv, kenvp);
	if (ret < 0)
		goto out_err;

	uint64_t new_stack_ptr = 0;
	uint64_t entry_point =
		elf_exec_replace(kpath, kargv, kenvp, &new_stack_ptr);

	if (entry_point == 0) {
		// exec failed, return error to caller
		ret = -ENOEXEC;
		goto out_err;
	}

	/* POSIX: a successful exec resets caught signal handlers to their
	 * default disposition (ignored signals stay ignored).  The old handler
	 * addresses belong to the previous program image and would crash if a
	 * signal were delivered to them in the new one. */
	{
		task_t *cur = sched_current();
		if (cur)
			signal_reset_on_exec(cur);
	}

	/* Set-user-ID / set-group-ID on a successful exec (04000 / 02000).
     * The real IDs are unchanged; effective+saved+fs IDs take the file's. */
	{
		task_t *cur = sched_current();
		if (cur && have_xst) {
			if (xst.st_mode & 04000) { /* S_ISUID */
				cur->cred.euid = cur->cred.suid =
					cur->cred.fsuid = (uint32_t)xst.st_uid;
			}
			if (xst.st_mode & 02000) { /* S_ISGID */
				cur->cred.egid = cur->cred.sgid =
					cur->cred.fsgid = (uint32_t)xst.st_gid;
			}
		}
	}

	// Set task comm from basename of path (or argv[0])
	{
		task_t *cur = sched_current();
		if (cur) {
			const char *src = kpath;
			// Use basename
			const char *p = src;
			while (*p) {
				if (*p == '/')
					src = p + 1;
				p++;
			}
			int i;
			for (i = 0; i < 255 && src[i]; i++)
				cur->comm[i] = src[i];
			cur->comm[i] = '\0';
			// Build cmdline from argv (space-separated)
			int pos = 0;
			if (kargv) {
				for (int a = 0; kargv[a] && pos < 1023; a++) {
					if (a > 0 && pos < 1023)
						cur->cmdline[pos++] = ' ';
					for (int c = 0;
					     kargv[a][c] && pos < 1023; c++)
						cur->cmdline[pos++] =
							kargv[a][c];
				}
			}
			cur->cmdline[pos] = '\0';
			// Build environ from envp (space-separated)
			pos = 0;
			if (kenvp) {
				for (int a = 0; kenvp[a] && pos < 2047; a++) {
					if (a > 0 && pos < 2047)
						cur->environ[pos++] = ' ';
					for (int c = 0;
					     kenvp[a][c] && pos < 2047; c++)
						cur->environ[pos++] =
							kenvp[a][c];
				}
			}
			cur->environ[pos] = '\0';
		}
	}

	free_user_string_array(kenvp);
	free_user_string_array(kargv);
	kfree(kpath);

	// Success! Jump to the new program
	// We need to return to userspace at the new entry point with the new stack
	// Load the new FS base (TLS / stack canary) before entering user space.
	task_load_tls(sched_current());
	// Use inline assembly to set up IRET frame and jump
	__asm__ volatile(
		"cli\n\t"
		// Set up IRET frame on current stack
		"push $0x1B\n\t" // SS (user data segment)
		"push %0\n\t" // RSP (new user stack)
		"pushfq\n\t" // RFLAGS
		"orq $0x200, (%%rsp)\n\t" // Enable interrupts in RFLAGS
		"push $0x23\n\t" // CS (user code segment)
		"push %1\n\t" // RIP (entry point)
		// Clear registers for clean start
		"xor %%rax, %%rax\n\t"
		"xor %%rbx, %%rbx\n\t"
		"xor %%rcx, %%rcx\n\t"
		"xor %%rdx, %%rdx\n\t"
		"xor %%rsi, %%rsi\n\t"
		"xor %%rdi, %%rdi\n\t"
		"xor %%rbp, %%rbp\n\t"
		"xor %%r8, %%r8\n\t"
		"xor %%r9, %%r9\n\t"
		"xor %%r10, %%r10\n\t"
		"xor %%r11, %%r11\n\t"
		"xor %%r12, %%r12\n\t"
		"xor %%r13, %%r13\n\t"
		"xor %%r14, %%r14\n\t"
		"xor %%r15, %%r15\n\t"
		"iretq\n\t"
		:
		: "r"(new_stack_ptr), "r"(entry_point)
		: "memory");

	// Should never reach here
	__builtin_unreachable();

out_err:
	free_user_string_array(kenvp);
	free_user_string_array(kargv);
	kfree(kpath);
	return ret;
}

// SYS_GETPPID - get parent process ID
static int64_t sys_getppid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return 0;
	return sched_get_ppid(cur);
}

// SYS_DUP - duplicate file descriptor
static int64_t sys_dup(uint64_t oldfd)
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
static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd)
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
static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags)
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

// SYS_GETPID - get process ID (thread group ID)
// With thread groups, getpid() returns the tgid (thread group leader's ID)
// which is the same for all threads in the process.
static int64_t sys_getpid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -1;
	// Return tgid (thread group ID) which equals id for single-threaded processes
	return cur->tgid;
}

// SYS_YIELD - yield CPU to other runnable tasks
// Moves current task to back of run queue and immediately reschedules.
// Returns 0 on success. In a preemptive kernel this is a hint to the
// scheduler that the caller is willing to give up its remaining timeslice.
static int64_t sys_yield(void)
{
	task_t *cur = sched_current();
	if (!cur) {
		return 0;
	}

	// Reset time slice - we're voluntarily giving it up
	cur->remaining_ticks = 0;
	cur->state = TASK_READY;

	// Immediate reschedule
	sched_schedule();

	return 0;
}

// ============================================================================
// Signal Syscalls
// ============================================================================

// SYS_RT_SIGACTION - set signal handler
static int64_t sys_rt_sigaction(uint64_t sig, uint64_t act_ptr,
				uint64_t oldact_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}
	if (sig <= 0 || sig >= NSIG) {
		return -EINVAL;
	}
	if (sig_kernel_only(sig)) {
		return -EINVAL; // Can't change SIGKILL/SIGSTOP
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_sigaction *kact = &cur->signals.action[sig];

	// Copy old action if requested
	if (oldact_ptr) {
		if (copy_to_user((void *)oldact_ptr, kact,
				 sizeof(struct k_sigaction)) != 0) {
			return -EFAULT;
		}
	}

	// Set new action if provided
	if (act_ptr) {
		struct k_sigaction newact;
		if (copy_from_user(&newact, (void *)act_ptr,
				   sizeof(struct k_sigaction)) != 0) {
			return -EFAULT;
		}
		/* sa_mask is applied to the blocked mask for the duration of the
		 * handler.  Strip the unblockable signals here, at the point they
		 * enter the kernel, so a filled sa_mask cannot make the task
		 * unkillable while its handler runs. */
		sig_strip_unblockable(&newact.sa_mask);
		mm_memcpy(kact, &newact, sizeof(struct k_sigaction));
	}

	return 0;
}

// SYS_RT_SIGPROCMASK - change blocked signals
static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
				  uint64_t oldset_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	kernel_sigset_t *blocked = &cur->signals.blocked;

	// Copy old mask if requested
	if (oldset_ptr) {
		if (copy_to_user((void *)oldset_ptr, blocked,
				 sizeof(kernel_sigset_t)) != 0) {
			return -EFAULT;
		}
	}

	// Set new mask if provided
	if (set_ptr) {
		kernel_sigset_t newset;
		if (copy_from_user(&newset, (void *)set_ptr,
				   sizeof(kernel_sigset_t)) != 0) {
			return -EFAULT;
		}

		switch (how) {
		case SIG_BLOCK:
			sigorset_k(blocked, blocked, &newset);
			break;
		case SIG_UNBLOCK:
			signandset_k(blocked, blocked, &newset);
			break;
		case SIG_SETMASK:
			*blocked = newset;
			break;
		default:
			return -EINVAL;
		}

		// Can't block SIGKILL or SIGSTOP
		sig_strip_unblockable(blocked);
	}

	return 0;
}

// SYS_RT_SIGPENDING - get pending signals
static int64_t sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	if (copy_to_user((void *)set_ptr, &cur->signals.pending,
			 sizeof(kernel_sigset_t)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_RT_SIGTIMEDWAIT - wait for signal with timeout
static int64_t sys_rt_sigtimedwait(uint64_t set_ptr, uint64_t info_ptr,
				   uint64_t timeout_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	kernel_sigset_t wait_set;
	if (copy_from_user(&wait_set, (void *)set_ptr,
			   sizeof(kernel_sigset_t)) != 0) {
		return -EFAULT;
	}

	struct k_timespec timeout;
	uint64_t deadline = 0;
	if (timeout_ptr) {
		if (copy_from_user(&timeout, (void *)timeout_ptr,
				   sizeof(struct k_timespec)) != 0) {
			return -EFAULT;
		}
		uint32_t freq = timer_get_frequency();
		uint64_t ticks =
			timeout.tv_sec * freq +
			(uint64_t)timeout.tv_nsec * freq / 1000000000ULL;
		deadline = timer_ticks() + ticks;
	}

	// Check if any signals in wait_set are already pending
	while (1) {
		for (int sig = 1; sig < NSIG; sig++) {
			if (sigismember_k(&wait_set, sig) &&
			    sigismember_k(&cur->signals.pending, sig)) {
				// Found a signal
				siginfo_t info;
				signal_dequeue(cur, &wait_set, &info);

				if (info_ptr) {
					if (copy_to_user(
						    (void *)info_ptr, &info,
						    sizeof(siginfo_t)) != 0) {
						return -EFAULT;
					}
				}
				return sig;
			}
		}

		// Check timeout
		if (timeout_ptr && timer_ticks() >= deadline) {
			return -EAGAIN;
		}

		// Block task and wait
		cur->state = TASK_BLOCKED;
		sched_schedule();

		// Check if we should exit
		if (cur->has_exited) {
			return -EINTR;
		}
	}
}

/* Shared gate for the signal syscalls that reach a task straight from its id
 * (rt_sigqueueinfo, tkill, tgkill).  Without it they bypass both guards
 * sys_kill applies: kernel threads are not signallable at all, and an
 * unprivileged caller may only signal a task whose credentials match
 * (signal_permission()).  sig == 0 is the probe form, so the check runs for it
 * too — that probe IS the permission answer. */
static int64_t signal_target_check(task_t *target, int sig)
{
	if (target->privilege == TASK_KERNEL)
		return -EPERM;
	return signal_permission(target, sig);
}

// SYS_RT_SIGQUEUEINFO - queue signal with info
static int64_t sys_rt_sigqueueinfo(uint64_t pid, uint64_t sig,
				   uint64_t info_ptr)
{
	if (sig <= 0 || sig >= NSIG) {
		return -EINVAL;
	}

	task_t *target = sched_find_task_by_id((uint32_t)pid);
	if (!target) {
		return -ESRCH;
	}

	int64_t perr = signal_target_check(target, (int)sig);
	if (perr != 0)
		return perr;

	siginfo_t info;
	if (copy_from_user(&info, (void *)info_ptr, sizeof(siginfo_t)) != 0) {
		return -EFAULT;
	}

	// Enforce that si_code indicates user-originated
	info.si_code = SI_QUEUE;

	return signal_send(target, (int)sig, &info);
}

// SYS_RT_SIGSUSPEND - suspend until signal
static int64_t sys_rt_sigsuspend(uint64_t mask_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	kernel_sigset_t newmask;
	if (copy_from_user(&newmask, (void *)mask_ptr,
			   sizeof(kernel_sigset_t)) != 0) {
		return -EFAULT;
	}

	// Save current mask and set new one
	cur->signals.saved_mask = cur->signals.blocked;
	cur->signals.blocked = newmask;
	cur->signals.in_sigsuspend = 1;

	// Can't block SIGKILL/SIGSTOP
	sig_strip_unblockable(&cur->signals.blocked);

	// Block until signal
	cur->state = TASK_BLOCKED;

	while (!signal_pending(cur)) {
		sched_schedule();
		if (cur->has_exited) {
			cur->signals.in_sigsuspend = 0;
			cur->signals.blocked = cur->signals.saved_mask;
			return -EINTR;
		}
	}

	// Restore mask
	cur->signals.in_sigsuspend = 0;
	cur->signals.blocked = cur->signals.saved_mask;

	return -EINTR; // sigsuspend always returns EINTR
}

// SYS_RT_SIGRETURN - return from signal handler
static int64_t sys_rt_sigreturn(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Restore context from the signal frame
	if (signal_restore_frame(cur) < 0) {
		kprintf("sys_rt_sigreturn: failed to restore frame\n");
		return -EFAULT;
	}

	// The return value will be ignored - we're restoring the original
	// context which includes the original RAX value
	return 0;
}

// SYS_SIGALTSTACK - set/get alternate signal stack
static int64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Copy old stack if requested
	if (old_ss_ptr) {
		if (copy_to_user((void *)old_ss_ptr, &cur->signals.altstack,
				 sizeof(stack_t)) != 0) {
			return -EFAULT;
		}
	}

	// Set new stack if provided
	if (ss_ptr) {
		stack_t newss;
		if (copy_from_user(&newss, (void *)ss_ptr, sizeof(stack_t)) !=
		    0) {
			return -EFAULT;
		}

		// Validate
		if (!(newss.ss_flags & SS_DISABLE)) {
			if (newss.ss_size < MINSIGSTKSZ) {
				return -ENOMEM;
			}
		}

		cur->signals.altstack = newss;
	}

	return 0;
}

// SYS_TKILL - send signal to specific thread
static int64_t sys_tkill(uint64_t tid, uint64_t sig)
{
	// sig == 0 is the existence/permission probe; only negatives are errors
	if ((int64_t)tid <= 0 || (int64_t)sig < 0 || sig >= NSIG) {
		return -EINVAL;
	}

	task_t *target = sched_find_task_by_id((uint32_t)tid);
	if (!target) {
		return -ESRCH;
	}

	int64_t perr = signal_target_check(target, (int)sig);
	if (perr != 0)
		return perr;

	if (sig == 0) {
		return 0;
	}

	task_t *self = sched_current();
	siginfo_t info;
	mm_memset(&info, 0, sizeof(info));
	info.si_signo = (int)sig;
	info.si_code = SI_TKILL;
	// si_pid is the sending *process*, i.e. the sender's tgid, not its tid
	info.si_pid = self ? self->tgid : 0;
	info.si_uid = self ? self->cred.uid : 0;

	return signal_send(target, (int)sig, &info);
}

// SYS_TGKILL - send signal to thread in specific thread group
// This is the secure way to send signals to threads - validates that
// the target thread belongs to the specified thread group.
static int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig)
{
	if ((int64_t)tgid <= 0 || (int64_t)tid <= 0) {
		return -EINVAL;
	}

	if ((int64_t)sig < 0 || sig >= NSIG) {
		return -EINVAL;
	}

	// Find the target thread
	task_t *target = sched_find_task_by_id((uint32_t)tid);
	if (!target) {
		return -ESRCH;
	}

	// Validate that target belongs to the specified thread group
	if (target->tgid != (int)tgid) {
		return -ESRCH; // Thread not in specified group
	}

	int64_t perr = signal_target_check(target, (int)sig);
	if (perr != 0)
		return perr;

	// sig == 0 is a permission check only
	if (sig == 0) {
		return 0;
	}

	task_t *self = sched_current();
	// Build siginfo
	siginfo_t info;
	mm_memset(&info, 0, sizeof(info));
	info.si_signo = (int)sig;
	info.si_code = SI_TKILL;
	info.si_pid = self ? self->tgid : 0;
	info.si_uid = self ? self->cred.uid : 0;

	return signal_send(target, (int)sig, &info);
}

// SYS_ALARM - set alarm clock
static int64_t sys_alarm(uint64_t seconds)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	uint64_t old_remaining = 0;

	// Calculate remaining time from old alarm
	uint32_t freq = timer_get_frequency();
	if (cur->signals.alarm_ticks > 0) {
		uint64_t now = timer_ticks();
		if (cur->signals.alarm_ticks > now) {
			old_remaining = (cur->signals.alarm_ticks - now) / freq;
		}
	}

	// Set new alarm
	if (seconds > 0) {
		cur->signals.alarm_ticks = timer_ticks() + seconds * freq;
	} else {
		cur->signals.alarm_ticks = 0; // Cancel
	}

	return (int64_t)old_remaining;
}

// SYS_SETITIMER - set interval timer
static int64_t sys_setitimer(uint64_t which, uint64_t new_value_ptr,
			     uint64_t old_value_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_itimerval *timer;
	switch (which) {
	case ITIMER_REAL:
		timer = &cur->signals.itimer_real;
		break;
	case ITIMER_VIRTUAL:
		timer = &cur->signals.itimer_virtual;
		break;
	case ITIMER_PROF:
		timer = &cur->signals.itimer_prof;
		break;
	default:
		return -EINVAL;
	}

	// Copy old value if requested
	if (old_value_ptr) {
		if (copy_to_user((void *)old_value_ptr, timer,
				 sizeof(struct k_itimerval)) != 0) {
			return -EFAULT;
		}
	}

	// Set new value if provided
	if (new_value_ptr) {
		if (copy_from_user(timer, (void *)new_value_ptr,
				   sizeof(struct k_itimerval)) != 0) {
			return -EFAULT;
		}
	}

	return 0;
}

// SYS_GETITIMER - get interval timer
static int64_t sys_getitimer(uint64_t which, uint64_t curr_value_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_itimerval *timer;
	switch (which) {
	case ITIMER_REAL:
		timer = &cur->signals.itimer_real;
		break;
	case ITIMER_VIRTUAL:
		timer = &cur->signals.itimer_virtual;
		break;
	case ITIMER_PROF:
		timer = &cur->signals.itimer_prof;
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void *)curr_value_ptr, timer,
			 sizeof(struct k_itimerval)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_TIMER_CREATE - create POSIX timer
static int64_t sys_timer_create(uint64_t clockid, uint64_t sevp_ptr,
				uint64_t timerid_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_sigevent sevp;
	if (sevp_ptr) {
		if (copy_from_user(&sevp, (void *)sevp_ptr,
				   sizeof(struct k_sigevent)) != 0) {
			return -EFAULT;
		}
	} else {
		// Default
		mm_memset(&sevp, 0, sizeof(sevp));
		sevp.sigev_notify = SIGEV_SIGNAL;
		sevp.sigev_signo = SIGALRM;
	}

	ktimer_t tid = timer_create_internal(cur, (clockid_t)clockid, &sevp);
	if (tid < 0) {
		return -EAGAIN;
	}

	if (copy_to_user((void *)timerid_ptr, &tid, sizeof(ktimer_t)) != 0) {
		timer_delete_internal(tid);
		return -EFAULT;
	}

	return 0;
}

// SYS_TIMER_SETTIME - set POSIX timer
static int64_t sys_timer_settime(uint64_t timerid, uint64_t flags,
				 uint64_t new_value_ptr, uint64_t old_value_ptr)
{
	struct k_itimerspec new_value, old_value;

	if (copy_from_user(&new_value, (void *)new_value_ptr,
			   sizeof(struct k_itimerspec)) != 0) {
		return -EFAULT;
	}

	int ret = timer_settime_internal((ktimer_t)timerid, (int)flags,
					 &new_value,
					 old_value_ptr ? &old_value : NULL);
	if (ret < 0) {
		return ret;
	}

	if (old_value_ptr) {
		if (copy_to_user((void *)old_value_ptr, &old_value,
				 sizeof(struct k_itimerspec)) != 0) {
			return -EFAULT;
		}
	}

	return 0;
}

// SYS_TIMER_GETTIME - get POSIX timer
static int64_t sys_timer_gettime(uint64_t timerid, uint64_t curr_value_ptr)
{
	struct k_itimerspec curr_value;

	int ret = timer_gettime_internal((ktimer_t)timerid, &curr_value);
	if (ret < 0) {
		return ret;
	}

	if (copy_to_user((void *)curr_value_ptr, &curr_value,
			 sizeof(struct k_itimerspec)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_TIMER_GETOVERRUN - get timer overrun count
static int64_t sys_timer_getoverrun(uint64_t timerid)
{
	return timer_getoverrun_internal((ktimer_t)timerid);
}

// SYS_TIMER_DELETE - delete POSIX timer
static int64_t sys_timer_delete(uint64_t timerid)
{
	return timer_delete_internal((ktimer_t)timerid);
}

// SYS_PAUSE - suspend until signal
static int64_t sys_pause(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Block until any signal arrives
	cur->state = TASK_BLOCKED;

	while (!signal_pending(cur)) {
		sched_schedule();
		if (cur->has_exited) {
			return -EINTR;
		}
	}

	return -EINTR; // pause always returns EINTR
}

// SYS_NANOSLEEP - sleep with nanosecond precision
// Uses timer-based wakeup: set wakeup_tick and block, timer IRQ wakes us
static int64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_timespec req;
	if (copy_from_user(&req, (void *)req_ptr, sizeof(struct k_timespec)) !=
	    0) {
		return -EFAULT;
	}

	// Calculate ticks to sleep using measured timer frequency.
	//
	// Two boundary corrections vs. the obvious floor division:
	//
	//   1. ROUND UP nsec→ticks.  For sub-tick requests (e.g. usleep(1500)
	//      at 100 Hz) floor gives 0; we'd then clamp to 1 tick which is
	//      fine, but for requests that fall between tick multiples (e.g.
	//      15 ms at 100 Hz, floor = 1) the original code returned after
	//      only 10 ms — less than requested.  Ceiling math fixes that.
	//
	//   2. ADD ONE EXTRA TICK to absorb the partial-tick uncertainty at
	//      the start of the sleep.  timer_ticks() was read at some
	//      unknown fraction ε ∈ [0, 1 tick) past the most recent timer
	//      IRQ; the next timer IRQ fires after (1 tick − ε) wall time,
	//      and subsequent IRQs are 1 tick apart.  Without the +1, a 100
	//      ms request at 100 Hz could return after as little as 9·10 ms
	//      = 90 ms of wall time (when ε ≈ 0).  Combined with TSC vs PIT
	//      calibration drift in clock_gettime, this is why
	//      "Timer accuracy under CPU load" reports 79 ms for a 100 ms
	//      usleep and fails the >= 80 ms check.
	uint32_t freq = timer_get_frequency();
	if (freq == 0)
		freq = 100;
	uint64_t total_ns =
		(uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
	uint64_t ticks =
		(total_ns * (uint64_t)freq + 999999999ULL) / 1000000000ULL;
	if (ticks == 0 && total_ns > 0) {
		ticks = 1;
	}
	if (ticks > 0) {
		ticks +=
			1; // Partial-tick boundary compensation (see comment above).
	}

	uint64_t start = timer_ticks();
	uint64_t end = start + ticks;

	// Loop until sleep time expires or interrupted by signal
	while (timer_ticks() < end) {
		// Check for pending signal BEFORE blocking
		if (signal_pending(cur)) {
			if (rem_ptr) {
				uint64_t now = timer_ticks();
				uint64_t elapsed = now - start;
				uint64_t remaining =
					(elapsed < ticks) ? ticks - elapsed : 0;
				struct k_timespec rem;
				rem.tv_sec = remaining / freq;
				rem.tv_nsec = (uint64_t)(remaining % freq) *
					      1000000000ULL / freq;
				copy_to_user((void *)rem_ptr, &rem,
					     sizeof(struct k_timespec));
			}
			cur->wakeup_tick = 0;
			return -EINTR;
		}

		// Set wakeup timer and block - timer IRQ will wake us
		cur->wakeup_tick = end;
		cur->state = TASK_BLOCKED;
		sched_schedule();

		// Woken up - either by timer expiry or signal
		// Loop will check timer and signal conditions
	}

	cur->wakeup_tick = 0;
	return 0;
}

// SYS_CLOCK_GETTIME - get time from specified clock
static int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr)
{
	if (!validate_user_ptr(tp_ptr, sizeof(struct k_timespec))) {
		return -EFAULT;
	}

	struct k_timespec tp;
	uint64_t total_us = timer_get_precise_us();

	uint64_t total_secs = total_us / 1000000ULL;
	uint64_t frac_ns = (total_us % 1000000ULL) * 1000ULL;

	switch (clk_id) {
	case 0: // CLOCK_REALTIME
		tp.tv_sec = timer_get_boot_epoch() + total_secs;
		tp.tv_nsec = frac_ns;
		break;
	case 1: // CLOCK_MONOTONIC
		tp.tv_sec = total_secs;
		tp.tv_nsec = frac_ns;
		break;
	case 2: // CLOCK_PROCESS_CPUTIME_ID
	case 3: // CLOCK_THREAD_CPUTIME_ID
		// CPU time consumed, not time elapsed.  These used to return
		// uptime, which is the same number only for a process that
		// never sleeps -- so anything measuring its own cost (clock(),
		// a profiler, a benchmark) was reading wall time and calling it
		// CPU time.  The tick handler already charges user and system
		// ticks per task, so report those.
		//
		// PROCESS and THREAD are the same figure here because the
		// accounting is per task and a thread IS a task; for a
		// single-threaded process the two agree exactly.
		{
			task_t *cur = sched_current();
			uint64_t ticks =
				cur ? cur->utime_ticks + cur->stime_ticks : 0;
			uint32_t freq = timer_get_frequency();
			if (freq == 0)
				freq = 100;
			tp.tv_sec = (int64_t)(ticks / freq);
			tp.tv_nsec = (int64_t)((ticks % freq) *
					       (1000000000ULL / freq));
		}
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void *)tp_ptr, &tp, sizeof(tp)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_CLOCK_GETRES - get clock resolution
static int64_t sys_clock_getres(uint64_t clk_id, uint64_t res_ptr)
{
	if (clk_id > 3) {
		return -EINVAL;
	}

	if (res_ptr) {
		if (!validate_user_ptr(res_ptr, sizeof(struct k_timespec))) {
			return -EFAULT;
		}

		struct k_timespec res;
		// Resolution = 1 tick in nanoseconds
		res.tv_sec = 0;
		res.tv_nsec = 1000000000 / timer_get_frequency();

		if (copy_to_user((void *)res_ptr, &res, sizeof(res)) != 0) {
			return -EFAULT;
		}
	}

	return 0;
}

// SYS_SIGNALFD / SYS_SIGNALFD4 - create signalfd (simplified stub)
static int64_t sys_signalfd(uint64_t fd, uint64_t mask_ptr, uint64_t flags)
{
	(void)fd;
	(void)mask_ptr;
	(void)flags;
	// signalfd is complex to implement fully - return ENOSYS for now
	return -ENOSYS;
}

// ============================================================================
// SMP/THREADING SYSCALLS - FULL IMPLEMENTATION
// ============================================================================

// Clone flags (conventional Unix ABI values)
#define CLONE_VM 0x00000100 // Share memory space
#define CLONE_FS 0x00000200 // Share filesystem info
#define CLONE_FILES 0x00000400 // Share file descriptors
#define CLONE_SIGHAND 0x00000800 // Share signal handlers
#define CLONE_PTRACE 0x00002000 // Allow tracing child
#define CLONE_VFORK 0x00004000 // vfork() semantics
#define CLONE_PARENT 0x00008000 // Same parent as cloner
#define CLONE_THREAD 0x00010000 // Same thread group
#define CLONE_NEWNS 0x00020000 // New mount namespace
#define CLONE_SYSVSEM 0x00040000 // Share SysV semaphore undo
#define CLONE_SETTLS 0x00080000 // Set TLS
#define CLONE_PARENT_SETTID 0x00100000 // Set parent's TID
#define CLONE_CHILD_CLEARTID 0x00200000 // Clear child's TID on exit
#define CLONE_DETACHED 0x00400000 // Unused
#define CLONE_UNTRACED 0x00800000 // Cannot force trace
#define CLONE_CHILD_SETTID 0x01000000 // Set child's TID
#define CLONE_STOPPED 0x02000000 // Start in TASK_STOPPED
#define CLONE_NEWUTS 0x04000000 // New UTS namespace
#define CLONE_NEWIPC 0x08000000 // New IPC namespace

// Forward declare from sched.c
extern void thread_group_add(task_t *leader, task_t *thread);
extern void thread_group_init(task_t *leader);
extern mm_struct_t *mm_struct_create(uint64_t *pml4);
extern void mm_struct_get(mm_struct_t *mm);
extern files_struct_t *files_struct_create(void);
extern files_struct_t *files_struct_clone(files_struct_t *src);
extern void files_struct_get(files_struct_t *files);
extern sighand_struct_t *sighand_struct_create(void);
extern sighand_struct_t *sighand_struct_clone(sighand_struct_t *src);
extern void sighand_struct_get(sighand_struct_t *sighand);
extern void task_set_fs_base(task_t *task, uint64_t base);

// PID allocator (from sched.c)
extern int g_next_id;
extern spinlock_t g_task_list_lock;

// SYS_CLONE - create a new thread or process
// Full implementation with all CLONE_* flags
static int64_t sys_clone(uint64_t flags, uint64_t child_stack,
			 uint64_t parent_tidptr, uint64_t child_tidptr,
			 uint64_t tls)
{
	task_t *cur = sched_current();
	if (!cur || cur->privilege != TASK_USER) {
		return -EPERM;
	}

	// Validate flag combinations
	// CLONE_THREAD requires CLONE_SIGHAND which requires CLONE_VM
	if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) {
		return -EINVAL;
	}
	if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) {
		return -EINVAL;
	}

	// Extract flag meanings
	bool share_vm = (flags & CLONE_VM) != 0;
	bool share_files = (flags & CLONE_FILES) != 0;
	bool share_sighand = (flags & CLONE_SIGHAND) != 0;
	bool is_thread = (flags & CLONE_THREAD) != 0;
	bool set_tls = (flags & CLONE_SETTLS) != 0;
	bool set_parent_tid = (flags & CLONE_PARENT_SETTID) != 0;
	bool set_child_tid = (flags & CLONE_CHILD_SETTID) != 0;
	bool clear_child_tid = (flags & CLONE_CHILD_CLEARTID) != 0;

	// For threads, child_stack is required
	if (is_thread && child_stack == 0) {
		return -EINVAL;
	}

	// Allocate child task structure
	task_t *child = (task_t *)kalloc(sizeof(task_t));
	if (!child) {
		return -ENOMEM;
	}

	// Allocate kernel stack for child (guarded: not-present guard page below)
	uint8_t *k_stack_mem =
		(uint8_t *)mm_alloc_guarded_kstack(KERNEL_STACK_SIZE);
	if (!k_stack_mem) {
		kfree(child);
		return -ENOMEM;
	}
	uint64_t k_stack_top =
		((uint64_t)(k_stack_mem + KERNEL_STACK_SIZE)) & ~0xFUL;

	// Initialize child from parent
	mm_memcpy(child, cur, sizeof(task_t));

	/* The copy above duplicated the POINTER to the parent's region table,
	 * not the table.  A CLONE_VM thread gets an empty one of its own --
	 * its bookkeeping is owner-routed to the group leader and it must hold
	 * no references of its own -- while a fork child gets a copy of the
	 * parent's.  Sharing the pointer would have the two free one
	 * allocation twice.
	 *
	 * Clone from the OWNER, not from the calling thread.  Because a
	 * CLONE_VM thread keeps an empty table on purpose, forking FROM one
	 * copied nothing: the child came up with no regions at all and died on
	 * its first write to a lazy page, with no region to explain the
	 * address.  Only a threaded program could hit it -- forking from a
	 * single-threaded one, the caller IS the owner. */
	task_t *mm_src = task_mm_owner(cur);

	if (share_vm ? !mm_regions_init(child) :
		       !mm_regions_clone(child, mm_src)) {
		mm_free_guarded_kstack(k_stack_mem, KERNEL_STACK_SIZE);
		kfree(child);
		return -ENOMEM;
	}

	/* The rest of the leader-only address-space state, for the same reason
	 * and from the same place.  A thread's own copies of these are stale by
	 * design; a fork child becomes its own owner and must start from the
	 * values that were actually in use. */
	if (!share_vm) {
		child->brk_start = mm_src->brk_start;
		child->brk = mm_src->brk;
		child->mmap_base = mm_src->mmap_base;
	}

	/* A fresh address space needs a fresh lock.  The wholesale copy took
	 * the parent's, which another thread in the group may have been
	 * holding at that instant -- inherited locked, it would wedge the
	 * child on its first fault.  (A thread's own copy is never used, so
	 * resetting it unconditionally costs nothing.) */
	mm_rwsem_init(&child->mmap_lock, "mmap_lock");
	child->mm_rdepth = 0;

	/* Fresh kernel-stack canary — same rationale as sched_fork_current:
	 * the wholesale copy duplicated the parent's canary; the child's only
	 * kernel context is the hand-built fork_child_return frame, so no live
	 * frame carries the old value and regenerating is safe. */
	child->stack_canary = generate_stack_canary();

	/* Demand paging: a fork-like clone (no CLONE_VM) copies the region
	 * table by value and is its own thread-group leader, so it needs its
	 * own reference on every file-backed lazy region.  CLONE_VM threads
	 * carry only a stale copy (bookkeeping is owner-routed to the group
	 * leader) and must NOT take references — the leader-only release at
	 * exit would not balance them. */
	if (!share_vm) {
		for (uint32_t i = 0; i < child->mmap_capacity; i++) {
			if (child->mmap_regions[i].in_use &&
			    child->mmap_regions[i].file)
				vfs_incref(child->mmap_regions[i].file);
		}
	}

	// Assign unique ID (atomic; no lock needed just for the counter)
	uint64_t irq_flags;
	child->id = sched_alloc_task_id();

	// Basic child setup
	/* No unreported stop/continue of its own: the wholesale copy above
	 * duplicated the parent's, which would make the parent's next
	 * waitpid(WUNTRACED/WCONTINUED) report this running child as stopped. */
	child->jc_stop_signo = 0;
	child->term_sig = 0;
	child->sigmask_restore_pending = 0;
	child->jc_continued = 0;
	child->state = TASK_READY;
	child->kernel_stack_top = k_stack_top;
	child->kernel_stack_base = k_stack_mem;
	child->rq_next = NULL;
	child->on_rq = false;
	child->wait_next = NULL;
	child->wait_channel = NULL;
	child->wakeup_tick = 0;
	child->need_resched = 0;
	child->remaining_ticks = SCHED_TIME_SLICE;
	child->preempt_frame = NULL;
	child->exit_code = 0;
	child->has_exited = false;
	child->exit_lock = 0;
	child->is_fork_child = true;
	child->first_child = NULL;
	child->next_sibling = NULL;

	// Handle CLONE_VM (share address space)
	if (share_vm) {
		// Share parent's address space with reference counting
		if (cur->mm) {
			// Parent already uses mm_struct
			mm_struct_get(cur->mm);
			child->mm = cur->mm;
			child->pml4 = cur->mm->pml4;
		} else {
			// First time: create mm_struct from parent's legacy pml4
			cur->mm = mm_struct_create(cur->pml4);
			if (!cur->mm) {
				mm_free_guarded_kstack(k_stack_mem,
						       KERNEL_STACK_SIZE);
				kfree(child);
				return -ENOMEM;
			}
			cur->mm->brk = cur->brk;
			cur->mm->brk_start = cur->brk_start;
			cur->mm->mmap_base = cur->mmap_base;

			mm_struct_get(cur->mm);
			child->mm = cur->mm;
			child->pml4 = cur->pml4;
		}
	} else {
		/* COW clone of address space.  Held for writing for the same
		 * reason as the fork path in the scheduler: marking the parent
		 * copy-on-write and taking the child's reference must not be
		 * interleaved with a fault in another thread resolving the very
		 * page being marked. */
		task_t *mm_owner = task_mm_owner(cur);
		uint64_t *child_pml4;

		mm_write_lock(&mm_owner->mmap_lock);
		child_pml4 = mm_clone_address_space(cur->pml4);
		mm_write_unlock(&mm_owner->mmap_lock);
		if (!child_pml4) {
			mm_free_guarded_kstack(k_stack_mem, KERNEL_STACK_SIZE);
			kfree(child);
			return -ENOMEM;
		}
		child->pml4 = child_pml4;
		child->mm = NULL; // Use legacy fields
	}

	// Handle CLONE_FILES (share file descriptors)
	if (share_files) {
		if (cur->files) {
			files_struct_get(cur->files);
			child->files = cur->files;
		} else {
			/* First CLONE_FILES in this process: promote the
			 * caller's private table to a shared one.  The
			 * descriptors are MOVED, not duplicated — the shared
			 * table becomes the single owner of each reference, so
			 * a close() by any thread really closes the object and
			 * the reader on the other end of a pipe sees EOF.  (The
			 * previous code duplicated every reference into a table
			 * no lookup ever consulted; those copies were only
			 * released when the last thread exited, so nothing a
			 * threaded process closed was ever truly closed.) */
			cur->files = files_struct_create();
			if (!cur->files) {
				if (!share_vm && child->pml4) {
					mm_destroy_address_space(child->pml4);
				}
				mm_free_guarded_kstack(k_stack_mem,
						       KERNEL_STACK_SIZE);
				kfree(child);
				return -ENOMEM;
			}
			for (int i = 0; i < TASK_MAX_FDS; i++) {
				cur->files->fd_table[i] = cur->fd_table[i];
				/* Carry the close-on-exec bits over with the
				 * descriptors: task_get/set_fd_flags switch to
				 * the shared array the moment ->files is set,
				 * so without this every FD_CLOEXEC bit in the
				 * process silently vanished at the first
				 * pthread_create. */
				cur->files->fd_flags[i] = cur->fd_flags[i];
				cur->fd_table[i] = NULL;
				cur->fd_flags[i] = 0;
			}

			files_struct_get(cur->files);
			child->files = cur->files;
		}
		/* The wholesale task copy above duplicated the caller's private
		 * table into the child; it is not the child's to own — the
		 * shared files_struct holds every reference now. */
		for (int i = 0; i < TASK_MAX_FDS; i++) {
			child->fd_table[i] = NULL;
			child->fd_flags[i] = 0;
		}
	} else {
		// Clone file descriptors
		child->files = NULL;
		for (int i = 0; i < TASK_MAX_FDS; i++) {
			vfs_file_t *src_fd = task_fds(cur)[i];
			/* The child owns a PRIVATE table, so the flags have to
			 * be copied out of whichever array is the caller's
			 * effective one — the wholesale task copy duplicated
			 * cur->fd_flags, which is the empty legacy array once
			 * the caller became part of a thread group. */
			child->fd_flags[i] =
				task_get_fd_flags(cur, (unsigned)i);
			/* Every descriptor KIND has to be duplicated the way its
			 * own type demands.  This used to fall through to
			 * vfs_dup() for anything that was not a console marker
			 * or a pipe end — but a socket, unix-socket or epoll
			 * descriptor is a small tagged integer, not a pointer,
			 * so vfs_dup dereferenced it.  fd_dup_entry classifies
			 * first (and takes the socket refcounts fork already
			 * took). */
			child->fd_table[i] =
				src_fd ? fd_dup_entry(src_fd) : NULL;
		}
	}

	// Handle CLONE_SIGHAND (share signal handlers)
	if (share_sighand) {
		if (cur->sighand) {
			sighand_struct_get(cur->sighand);
			child->sighand = cur->sighand;
		} else {
			// Create sighand_struct from parent's legacy signal handlers
			cur->sighand = sighand_struct_create();
			if (!cur->sighand) {
				// Cleanup and fail
				if (share_files && child->files) {
					// files_struct_put would be called in cleanup
				}
				if (!share_vm && child->pml4) {
					mm_destroy_address_space(child->pml4);
				}
				mm_free_guarded_kstack(k_stack_mem,
						       KERNEL_STACK_SIZE);
				kfree(child);
				return -ENOMEM;
			}
			// Copy signal handlers
			for (int i = 0; i < 65; i++) {
				cur->sighand->action[i] =
					cur->signals.action[i];
			}

			sighand_struct_get(cur->sighand);
			child->sighand = cur->sighand;
		}
	} else {
		// Copy signal handlers (already done by memcpy)
		child->sighand = NULL;
		signal_fork_copy(child, cur);
	}

	// Handle CLONE_THREAD (same thread group)
	if (is_thread) {
		// Add to parent's thread group
		thread_group_add(cur->group_leader, child);
		child->exit_signal = 0; // Threads don't send exit signal
		/* A thread is nobody's child: the PROCESS is, and the process
		 * is the group leader.
		 *
		 * This used to copy cur->parent, which gave every thread a
		 * pointer to a task it was never linked under -- nothing ever
		 * added it to that parent's child list.  Two things follow, and
		 * both are fatal:
		 *
		 *   - sched_reparent_children() repoints children by walking
		 *     the child LIST, so it never saw these.  When the parent
		 *     was freed, every thread of every child process was left
		 *     holding a dangling pointer to it.
		 *   - sched_remove_task() then did
		 *     sched_remove_child(task->parent, task) on the way out,
		 *     which walks that parent's sibling list looking for a
		 *     thread that was never on it -- straight through the freed
		 *     task's poison.  RAX = 0xfeedfacefeedface in
		 *     sched_remove_child, under a spinlock with interrupts off,
		 *     so the processor wedges holding the lock and the whole
		 *     machine halts.  Seen on every session teardown, where
		 *     parents die while other processes still have live threads.
		 *
		 * NULL is the honest value.  Anything that wants the process's
		 * parent asks the group leader (see sched_get_ppid). */
		child->parent = NULL;
	} else {
		/* New process (new thread group).
		 *
		 * The child belongs to the forking PROCESS, not to the thread
		 * that happened to call fork().  Hanging it off `cur' put a
		 * child forked by a worker thread on that thread's own child
		 * list, while sys_waitpid() looks for children on the thread
		 * group LEADER's list -- so nothing in the process could ever
		 * wait for it: waitpid() answered ECHILD and the child stayed a
		 * zombie for good.  It also made getppid() in the child report
		 * the forking thread's tid instead of the parent's pid.
		 *
		 * Any threaded program that spawns from a worker thread hits
		 * this, which is most of them -- GLib's g_spawn family forks
		 * and then waits for the intermediate child. */
		task_t *proc = cur->group_leader ? cur->group_leader : cur;

		thread_group_init(child);
		child->exit_signal = SIGCHLD;
		child->parent = proc;
		sched_add_child(proc, child);
	}

	// Handle CLONE_SETTLS
	if (set_tls) {
		task_set_fs_base(child, tls);
	} else {
		child->fs_base = 0;
	}

	// Handle CLONE_CHILD_CLEARTID
	if (clear_child_tid && child_tidptr) {
		if (validate_user_ptr(child_tidptr, sizeof(int))) {
			child->clear_child_tid = (uint64_t *)child_tidptr;
		}
	} else {
		child->clear_child_tid = NULL;
	}

	// Handle CLONE_CHILD_SETTID
	if (set_child_tid && child_tidptr) {
		if (validate_user_ptr(child_tidptr, sizeof(int))) {
			child->set_child_tid = (uint64_t *)child_tidptr;
			// This will be written when child starts
		}
	} else {
		child->set_child_tid = NULL;
	}

	// Handle CLONE_PARENT_SETTID
	if (set_parent_tid && parent_tidptr) {
		if (validate_user_ptr(parent_tidptr, sizeof(int))) {
			smap_disable();
			*(int *)parent_tidptr = child->id;
			smap_enable();
		}
	}

	// Clear robust list (not inherited)
	child->robust_list = NULL;
	child->robust_list_len = 0;

	/* The region table was given to the child by mm_regions_clone() right
	 * after the task_t copy, which is the only place it can be done. */

	// Assign child to parent's CPU (same rationale as sched_fork_current)
	child->on_cpu = cur->on_cpu;

	// Set up child's kernel stack to return to userspace
	uint64_t user_rip = cur->syscall_rip;
	uint64_t user_rsp = child_stack ? child_stack : cur->syscall_rsp;
	uint64_t user_rflags = cur->syscall_rflags;

	uint64_t *k_sp = (uint64_t *)child->kernel_stack_top;
	k_sp = (uint64_t *)((uint64_t)k_sp & ~0xFUL);

	// Push user callee-saved registers
	*(--k_sp) = cur->syscall_r15;
	*(--k_sp) = cur->syscall_r14;
	*(--k_sp) = cur->syscall_r13;
	*(--k_sp) = cur->syscall_r12;
	*(--k_sp) = cur->syscall_rbx;
	*(--k_sp) = cur->syscall_rbp;

	// IRET frame
	*(--k_sp) = 0x1B; // SS
	*(--k_sp) = user_rsp; // RSP
	*(--k_sp) = user_rflags | 0x200; // RFLAGS with IF
	*(--k_sp) = 0x23; // CS
	*(--k_sp) = user_rip; // RIP

	// Return value for child (0 or TID depending on CLONE_CHILD_SETTID)
	*(--k_sp) = 0; // RAX = 0 for child

	// Kernel callee-saved for context switch
	*(--k_sp) = (uint64_t)fork_child_return;
	// RFLAGS restored by ctx_switch_asm's popfq; IF set (see sys_fork).
	*(--k_sp) = 0x202; // RFLAGS (kernel): reserved bit 1 + IF
	*(--k_sp) = 0; // RBP
	*(--k_sp) = 0; // RBX
	*(--k_sp) = 0; // R12
	*(--k_sp) = 0; // R13
	*(--k_sp) = 0; // R14
	*(--k_sp) = 0; // R15

	child->sp = k_sp;

	// Add to global task list
	spin_lock_irqsave(&g_task_list_lock, &irq_flags);
	extern void task_list_add(task_t * t);
	task_list_add(child);
	spin_unlock_irqrestore(&g_task_list_lock, irq_flags);

	// Handle CLONE_CHILD_SETTID: write TID to child's address space
	// This must be done after child is set up but before scheduling
	if (set_child_tid && child->set_child_tid) {
		smap_disable();
		*(int *)(child->set_child_tid) = child->id;
		smap_enable();
	}

	// IMPORTANT: Save child->id BEFORE enqueueing!
	// Once enqueued, another CPU might run and free the child before we read it.
	int64_t child_pid = child->id;

	// Enqueue child
	sched_enqueue_ready(child);

	// Set need_resched so the parent yields at the next opportunity,
	// giving the newly created thread a chance to start promptly.
	// This is the standard behavior: thread creation is a reschedule point.
	cur->need_resched = 1;

	return child_pid;
}

// SYS_VFORK - create child that shares parent's memory until exec/exit
static int64_t sys_vfork(void)
{
	// For now, implement as regular fork
	// True vfork semantics would suspend parent until child execs or exits
	return sys_fork();
}

// SYS_EXIT_GROUP - terminate all threads in the process
static void sys_exit_group(uint64_t status)
{
	task_t *cur = sched_current();
	if (!cur) {
		while (1) {
			__asm__ volatile("hlt");
		}
	}

	task_t *leader = cur->group_leader;
	if (!leader)
		leader = cur;

	/* Same check, and for the same reason, as sched_kill_thread_group():
	 * group_leader is a bare pointer that nothing clears in the surviving
	 * threads, and the writes just below are made with no lock held.  A
	 * leader that has already been freed is written into poisoned memory
	 * and then walked from, which faults non-recoverably once the lock is
	 * taken.  Fall back to this thread's own group of one. */
	if (leader != cur && !task_ptr_ok(leader)) {
		WARN_ON_ONCE(1);
		kprintf("BUG: pid %d group_leader is unusable (%p) - exiting this thread only\n",
			cur->id, (void *)leader);
		leader = cur;
	}

	// Mark the group as exiting to prevent new threads
	leader->group_exiting = true;
	leader->group_exit_code = (int)status;

	/*
	 * Signal every other thread in the group to exit.
	 *
	 * COLLECT under g_task_list_lock, SIGNAL after releasing it.
	 *
	 * The list is maintained by thread_group_remove(), which unlinks a
	 * dying thread under that lock -- and the task_t is freed and poisoned
	 * shortly afterwards.  Walking it unlocked therefore races a thread
	 * exiting on another CPU and follows `thread_group_next' straight into
	 * freed memory: observed as a general protection fault in here with
	 * t == 0xdeadbeefdeadbeef, taking the whole system down.  It went
	 * unnoticed for as long as it did because nothing routed an ordinary
	 * process exit through exit_group; now that _exit() does, every exit
	 * of a threaded program runs this loop.
	 *
	 * Signalling cannot happen under the lock -- sched_signal_task() takes
	 * scheduler locks and may queue a siginfo -- hence the two passes.
	 */
	/* The batch has to hold the WHOLE group.
	 *
	 * A fixed array used to cap it, and the threads past the cap were
	 * dropped with a warning -- nothing ever came back for them.  That
	 * leaves live threads running in a process whose parent has already
	 * been told it finished, still holding its descriptors and its
	 * address space.  Size the batch from the group instead, and fall
	 * back to the on-stack array only when it fits, so the common case
	 * allocates nothing on the exit path. */
	task_t *stack_targets[TASK_GROUP_KILL_MAX];
	task_t **targets = stack_targets;
	int capacity = TASK_GROUP_KILL_MAX;
	int ntargets = 0;
	int overflow = 0;
	uint64_t tg_flags;

	if (leader->nr_threads >= TASK_GROUP_KILL_MAX) {
		/* +2: one for the leader, one for a thread that appeared
		 * between reading the count and taking the lock. */
		int want = leader->nr_threads + 2;
		task_t **big = kalloc((size_t)want * sizeof(task_t *));

		if (big) {
			targets = big;
			capacity = want;
		}
	}

	spin_lock_irqsave(&g_task_list_lock, &tg_flags);
	{
		task_t *t = leader;
		int guard = 0;

		do {
			if (t != cur && !t->has_exited) {
				if (ntargets < capacity)
					targets[ntargets++] = t;
				else
					overflow++;
			}
			t = t->thread_group_next;
			/* Same guard as sched_kill_thread_group: a broken ring
			 * followed with this lock held and interrupts off
			 * wedges the processor rather than the process. */
			if (!task_ptr_ok(t) ||
			    ++guard > capacity + TASK_GROUP_KILL_MAX) {
				WARN_ON_ONCE(1);
				kprintf("BUG: thread group ring of tgid %d broken at %p (guard %d)\n",
					leader->id, (void *)t, guard);
				break;
			}
		} while (t != leader);
	}
	spin_unlock_irqrestore(&g_task_list_lock, tg_flags);

	/* Only reachable if the group grew past the count we sized against,
	 * which group_exiting is supposed to prevent. */
	WARN_ON(overflow != 0);

	for (int i = 0; i < ntargets; i++)
		sched_signal_task(targets[i], SIGKILL);

	if (targets != stack_targets)
		kfree(targets);

	// Now exit ourselves
	sched_mark_task_exited(cur, (int)status);

	/* Same hardened park as sys_exit: sched_schedule() can return in the
	 * rare zombie edge case, and a bare `hlt` here (without STI and without
	 * retrying the switch) parks the CPU for a full timer tick per exit —
	 * and with IRQs off would wedge TLB shootdowns (see sys_exit). */
	sched_exit_park();
}

// SYS_GETTID - get thread ID (unique per thread)
static int64_t sys_gettid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;
	return cur->id; // TID is always the unique task ID
}

// SYS_SET_TID_ADDRESS - set address for clear-on-exit notification
static int64_t sys_set_tid_address(uint64_t tidptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	if (tidptr && validate_user_ptr(tidptr, sizeof(int))) {
		cur->clear_child_tid = (uint64_t *)tidptr;
	} else {
		cur->clear_child_tid = NULL;
	}

	return cur->id;
}

// Futex operations
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFu

// SYS_FUTEX - fast userspace mutex operations (uses hash-bucket implementation)
static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
			 uint64_t timeout, uint64_t uaddr2, uint64_t val3)
{
	(void)val3; // Used for FUTEX_CMP_REQUEUE comparison value

	/*
	 * Strip the modifier bits before dispatching.
	 *
	 * FUTEX_CLOCK_REALTIME selects which clock an ABSOLUTE timeout is
	 * measured against; it is not a command.  Leaving it in the switch
	 * value meant FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME (265) matched
	 * nothing and fell through to -ENOSYS -- which is what GLib issues for
	 * every g_cond_wait_until(), so every timed condition wait in every
	 * GLib program returned instantly instead of waiting.  A caller that
	 * treats that as "timed out" then spins; GTK's file chooser loads its
	 * folder through a GThreadPool that does exactly this, and hung.
	 */
	int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
	int abs_realtime = (op & FUTEX_CLOCK_REALTIME) != 0;

	if (!validate_user_ptr(uaddr, sizeof(uint32_t))) {
		return -EFAULT;
	}

	switch (cmd) {
	case FUTEX_WAIT: {
		// Convert timeout to nanoseconds
		uint64_t timeout_ns = 0;
		if (timeout) {
			// timeout points to struct timespec
			if (validate_user_ptr(timeout,
					      sizeof(struct k_timespec))) {
				smap_disable();
				struct k_timespec *ts =
					(struct k_timespec *)timeout;
				timeout_ns =
					(uint64_t)ts->tv_sec * 1000000000ULL +
					(uint64_t)ts->tv_nsec;
				smap_enable();
			}
		}
		return futex_wait(uaddr, (uint32_t)val, timeout_ns);
	}

	case FUTEX_WAIT_BITSET: {
		/*
		 * Like FUTEX_WAIT, except the timeout is ABSOLUTE -- against
		 * CLOCK_REALTIME when FUTEX_CLOCK_REALTIME is set, else
		 * CLOCK_MONOTONIC -- and waiters carry a bitset that a
		 * FUTEX_WAKE_BITSET must match.
		 *
		 * The bitset is accepted and ignored: every waiter here is
		 * created with all bits set, and the only caller in this system
		 * (GLib) passes FUTEX_BITSET_MATCH_ANY, for which "match all"
		 * IS the correct answer.  A selective bitset would need the
		 * value plumbed into futex_wait/futex_wake; there is nothing to
		 * exercise it yet, and guessing at it would be untested code.
		 */
		uint64_t timeout_ns = 0;

		if (timeout) {
			if (validate_user_ptr(timeout,
					      sizeof(struct k_timespec))) {
				struct k_timespec ts;

				smap_disable();
				ts = *(struct k_timespec *)timeout;
				smap_enable();

				uint64_t deadline_ns =
					(uint64_t)ts.tv_sec * 1000000000ULL +
					(uint64_t)ts.tv_nsec;
				uint64_t now_ns =
					timer_get_precise_us() * 1000ULL;

				if (abs_realtime)
					now_ns +=
						(uint64_t)
							timer_get_boot_epoch() *
						1000000000ULL;

				/* Absolute -> relative, which is what
				 * futex_wait takes.  An already-past deadline
				 * must not become an infinite wait: the 0 that
				 * futex_wait reads as "no timeout" is exactly
				 * the wrong answer, so report the timeout
				 * without sleeping. */
				if (deadline_ns <= now_ns)
					return -ETIMEDOUT;
				timeout_ns = deadline_ns - now_ns;
			}
		}
		return futex_wait(uaddr, (uint32_t)val, timeout_ns);
	}

	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		/* The bitset (val3) is ignored for the reason given above: all
		 * waiters match, which is right for FUTEX_BITSET_MATCH_ANY. */
		return futex_wake(uaddr, (int)val);

	case FUTEX_REQUEUE:
	case FUTEX_CMP_REQUEUE:
		if (!validate_user_ptr(uaddr2, sizeof(uint32_t))) {
			return -EFAULT;
		}
		// For CMP_REQUEUE, val3 is the expected value to compare
		if (cmd == FUTEX_CMP_REQUEUE) {
			smap_disable();
			uint32_t curval = *(volatile uint32_t *)uaddr;
			smap_enable();
			if (curval != (uint32_t)val3) {
				return -EAGAIN;
			}
		}
		// val = nr_wake, timeout = nr_requeue (reusing timeout arg)
		return futex_requeue(uaddr, uaddr2, (int)val, (int)timeout);

	default:
		return -ENOSYS;
	}
}

// SYS_SET_ROBUST_LIST - set robust futex list head
static int64_t sys_set_robust_list(uint64_t head, uint64_t len)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	// Validate the length matches expected structure size
	if (len != sizeof(struct robust_list_head)) {
		return -EINVAL;
	}

	if (head && !validate_user_ptr(head, len)) {
		return -EFAULT;
	}

	cur->robust_list = (struct robust_list_head *)head;
	cur->robust_list_len = len;

	return 0;
}

// SYS_GET_ROBUST_LIST - get robust futex list head
static int64_t sys_get_robust_list(uint64_t pid, uint64_t head_ptr,
				   uint64_t len_ptr)
{
	task_t *target;

	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target)
		return -ESRCH;

	// Permission check: can only get own robust list or if privileged
	task_t *cur = sched_current();
	/* Through the target's PROCESS: a thread's own ->parent is NULL, so
	 * comparing it directly would refuse a parent asking about a thread of
	 * its own child -- which this used to allow. */
	task_t *tproc = target->group_leader ? target->group_leader : target;

	if (target != cur && tproc->parent != cur) {
		return -EPERM;
	}

	if (!validate_user_ptr(head_ptr, sizeof(void *)) ||
	    !validate_user_ptr(len_ptr, sizeof(size_t))) {
		return -EFAULT;
	}

	smap_disable();
	*(struct robust_list_head **)head_ptr = target->robust_list;
	*(size_t *)len_ptr = target->robust_list_len;
	smap_enable();

	return 0;
}

// arch_prctl codes
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

// SYS_ARCH_PRCTL - architecture-specific thread state
static int64_t sys_arch_prctl(uint64_t code, uint64_t addr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	switch (code) {
	case ARCH_SET_FS: {
		/* Reject any address whose top bit (bit 47) is 1.  A user
             * task may only set FS to USER-space addresses.  Canonical
             * checking alone (allowing top == 0x1FFFF, i.e. kernel half)
             * would let user code point FS at an arbitrary kernel VA,
             * which then poisons every libc stack-canary read
             * (`mov %fs:0x28, %rax`) and any other FS-relative TLS
             * access.  The resulting #PF can land in kernel mode if
             * triggered while the kernel is between SWAPGS / sysret —
             * a wild fault that is almost impossible to reproduce. */
		if (addr != 0 && (addr >> 47) != 0)
			return -EINVAL;
		task_set_fs_base(cur, addr);
		// Apply immediately
		task_load_tls(cur);
		return 0;
	}

	case ARCH_GET_FS:
		if (!validate_user_ptr(addr, sizeof(uint64_t))) {
			return -EFAULT;
		}
		smap_disable();
		*(uint64_t *)addr = cur->fs_base;
		smap_enable();
		return 0;

	case ARCH_SET_GS: {
		/* Reject kernel addresses (top bit set) — same rationale as
             * ARCH_SET_FS above.  Even though task_load_tls today does
             * not push gs_base into MSR_GS_BASE (the kernel manages %gs
             * exclusively for per-CPU data), the field is exposed by
             * ARCH_GET_GS and could be picked up by future code; refuse
             * the bad value at the API boundary. */
		if (addr != 0 && (addr >> 47) != 0)
			return -EINVAL;
		cur->gs_base = addr;
		return 0;
	}

	case ARCH_GET_GS:
		if (!validate_user_ptr(addr, sizeof(uint64_t))) {
			return -EFAULT;
		}
		smap_disable();
		*(uint64_t *)addr = cur->gs_base;
		smap_enable();
		return 0;

	default:
		return -EINVAL;
	}
}

// Scheduling policies
#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6

// CPU set for affinity
#define CPU_SETSIZE 64
typedef struct {
	uint64_t bits[CPU_SETSIZE / 64];
} cpu_set_t;

// SYS_SCHED_SETAFFINITY - bind thread to specific CPUs
static int64_t sys_sched_setaffinity(uint64_t pid, uint64_t cpusetsize,
				     uint64_t mask_ptr)
{
	(void)cpusetsize;

	if (!validate_user_ptr(mask_ptr, sizeof(uint64_t))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// Read affinity mask from user
	smap_disable();
	uint64_t mask = *(uint64_t *)mask_ptr;
	smap_enable();

	// Validate: at least one CPU must be set
	if (mask == 0) {
		return -EINVAL;
	}

	// Store the full affinity mask (0 means all CPUs allowed)
	target->cpu_affinity = mask;

	/* Migration to an allowed CPU.
	 *
	 * NEVER rewrite target->on_cpu here: on_cpu names the run queue the
	 * task is (or will be) linked on, and the scheduler's re-enqueue paths
	 * use this_cpu while rq_remove uses on_cpu.  Flipping on_cpu on a
	 * RUNNING or queued task desynchronises the two, and a later rq_remove
	 * then operates on the wrong queue — which used to truncate the real
	 * queue and permanently strand every task linked behind (unkillable
	 * READY tasks; showed up as rare teststress hangs).
	 *
	 * Also never wake-and-retarget a BLOCKED task onto another CPU from
	 * here: a task can be BLOCKED but still executing on its old CPU (the
	 * window between state=TASK_BLOCKED and its context switch completing).
	 * Every waker in the kernel re-enqueues to the task's OWN on_cpu so
	 * that window resolves locally; handing the task to a different CPU in
	 * that window lets two CPUs act on one context (observed as an idle
	 * task IRET-ing into user code + shifted-frame kernel stack smashes).
	 *
	 * So: only record the mask and request a reschedule.  The load
	 * balancer migrates the task lazily with both run-queue locks held,
	 * honouring cpu_affinity — the one protocol that is safe.
	 */
	if (!(mask & (1ULL << target->on_cpu)))
		target->need_resched = 1;

	return 0;
}

// SYS_SCHED_GETAFFINITY - get CPU affinity mask
static int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
				     uint64_t mask_ptr)
{
	(void)cpusetsize;

	if (!validate_user_ptr(mask_ptr, sizeof(uint64_t))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// Return stored affinity mask, or all CPUs if not set
	uint64_t mask = target->cpu_affinity;
	if (mask == 0) {
		// Affinity not set = all CPUs allowed, return mask with all online CPUs
		uint32_t cpu_count = smp_get_cpu_count();
		mask = (1ULL << cpu_count) - 1;
		if (mask == 0)
			mask = 1; // At least CPU 0
	}

	smap_disable();
	*(uint64_t *)mask_ptr = mask;
	smap_enable();

	return sizeof(uint64_t);
}

// Scheduling parameters
struct sched_param {
	int sched_priority;
};

// SYS_SCHED_SETSCHEDULER - set scheduling policy
static int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
				      uint64_t param_ptr)
{
	(void)param_ptr;

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// We only support SCHED_NORMAL for now
	if (policy != SCHED_NORMAL && policy != SCHED_RR) {
		return -EINVAL;
	}

	return 0;
}

// SYS_SCHED_GETSCHEDULER - get scheduling policy
static int64_t sys_sched_getscheduler(uint64_t pid)
{
	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	return SCHED_NORMAL; // We use round-robin by default
}

// SYS_SCHED_SETPARAM - set scheduling parameters
static int64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr)
{
	(void)param_ptr;

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// Accept but ignore (we use fixed round-robin)
	return 0;
}

// SYS_SCHED_GETPARAM - get scheduling parameters
static int64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr)
{
	if (!validate_user_ptr(param_ptr, sizeof(struct sched_param))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	struct sched_param param = { .sched_priority = 0 };

	smap_disable();
	*(struct sched_param *)param_ptr = param;
	smap_enable();

	return 0;
}

// SYS_SCHED_GET_PRIORITY_MAX - get max priority for policy
static int64_t sys_sched_get_priority_max(uint64_t policy)
{
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		return 99;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
		return 0;
	default:
		return -EINVAL;
	}
}

// SYS_SCHED_GET_PRIORITY_MIN - get min priority for policy
static int64_t sys_sched_get_priority_min(uint64_t policy)
{
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		return 1;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
		return 0;
	default:
		return -EINVAL;
	}
}

// SYS_SCHED_RR_GET_INTERVAL - get round-robin time quantum
static int64_t sys_sched_rr_get_interval(uint64_t pid, uint64_t tp_ptr)
{
	if (!validate_user_ptr(tp_ptr, sizeof(struct k_timespec))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// Return time slice (at 100Hz, 2 ticks = 20ms)
	struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 }; // 20ms

	smap_disable();
	*(struct k_timespec *)tp_ptr = ts;
	smap_enable();

	return 0;
}

// SYS_MPROTECT - change memory protection
/*
 * madvise(2).  MADV_DONTNEED releases the pages of a range and leaves the
 * mapping alone, so the next touch reads a fresh zero page; every other advice
 * is a hint this kernel has nothing to do about, and returns success because
 * that is what advisory means.
 *
 * The one that matters is DONTNEED.  Without it, an allocator wanting to give
 * physical memory back has only munmap, which cuts the mapping in two and
 * spends a region record on every trim.
 */
static int64_t sys_madvise_locked(uint64_t addr, uint64_t length,
				  uint64_t advice)
{
	task_t *cur = task_mm_owner(sched_current());

	if (!cur)
		return -EFAULT;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL; /* the address must be page aligned */
	if (length == 0)
		return 0;
	if (addr >= 0x0000800000000000ULL || addr + length < addr)
		return -EINVAL;

	switch (advice) {
	case MADV_DONTNEED:
		mm_dontneed_range(cur, addr, length);
		return 0;
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
	case MADV_WILLNEED:
	case MADV_DONTDUMP:
	case MADV_DODUMP:
		return 0;
	default:
		return -EINVAL;
	}
}

static int64_t sys_madvise(uint64_t addr, uint64_t length, uint64_t advice)
{
	RUN_WRITE_LOCKED(sys_madvise_locked(addr, length, advice));
}

static int64_t sys_mprotect_locked(uint64_t addr, uint64_t len, uint64_t prot)
{
	task_t *cur = sched_current();
	if (!cur) {
		return -ESRCH;
	}

	// Validate alignment
	if (addr & (PAGE_SIZE - 1)) {
		return -EINVAL;
	}

	// Round up length to page boundary
	uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

	// Build page flags
	uint64_t flags = PAGE_PRESENT | PAGE_USER;
	if (prot & 0x2) { // PROT_WRITE
		flags |= PAGE_WRITABLE;
	}
	if (!(prot & 0x4)) { // !PROT_EXEC
		flags |= PAGE_NO_EXECUTE;
	}

	// Update page table entries
	uint64_t *pml4 = cur->pml4;
	if (!pml4) {
		return -EFAULT;
	}

	/* Keep the region records describing the protection the pages actually
	 * have -- including when the range covers only PART of a region, which
	 * has to SPLIT it.
	 *
	 * This used to honour a full-region cover only, leaving a partial
	 * mprotect to change the page tables while the record kept the old
	 * protection for the whole span.  Two things follow, and the second is
	 * expensive:
	 *
	 *   - a lazy page in the changed part faults in with the RECORD's
	 *     protection, undoing the mprotect;
	 *   - the record still looks like its neighbours, so mappings that are
	 *     no longer alike merge with each other.  A thread stack is exactly
	 *     this shape -- one mmap of guard+stack, then mprotect(PROT_NONE)
	 *     over the guard alone -- so every stack stayed one RW record and
	 *     each new one coalesced onto the last.  The region count then
	 *     stays flat while the mapped total climbs by a stack per thread,
	 *     which reads as a leak and hides the guard page from the records
	 *     entirely.
	 *
	 * Split into up to three: the part before the range keeps the old
	 * protection, the covered part takes the new one, and any tail keeps
	 * the old.  Out of slots, the region is left whole with its protection
	 * unchanged -- the page tables below are still updated, which is the
	 * same partial state as before, but it is now the rare failure case
	 * rather than the normal path. */
	{
		task_t *mm = task_mm_owner(cur);
		uint64_t end = addr + pages * PAGE_SIZE;

		for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
			mmap_region_t *r = &mm->mmap_regions[i];
			uint64_t r_end;

			if (!r->in_use)
				continue;
			r_end = r->start + r->length;
			if (end <= r->start || addr >= r_end)
				continue; /* no overlap */

			if (addr <= r->start && end >= r_end) {
				r->prot = prot; /* whole region */
				continue;
			}

			/* Carve the tail off first, so `r' can then be trimmed
			 * to the head and the middle handled by the next loop
			 * iteration or by this one's own adjustment. */
			if (end < r_end) {
				size_t ridx = (size_t)(r - mm->mmap_regions);
				mmap_region_t *tail =
					mm_alloc_mmap_region(mm);

				/* Claiming a slot can grow -- and move -- the
				 * table, so the pointer is rebuilt. */
				r = &mm->mmap_regions[ridx];
				if (!tail)
					continue;
				*tail = *r;
				tail->start = end;
				tail->length = r_end - end;
				tail->offset = r->offset + (end - r->start);
				if (tail->file)
					vfs_incref(tail->file);
				tail->in_use = true;
				r->length = end - r->start;
				r_end = end;
			}

			if (addr > r->start) {
				size_t ridx = (size_t)(r - mm->mmap_regions);
				mmap_region_t *mid =
					mm_alloc_mmap_region(mm);

				r = &mm->mmap_regions[ridx];
				if (!mid)
					continue;
				*mid = *r;
				mid->start = addr;
				mid->length = r_end - addr;
				mid->offset = r->offset + (addr - r->start);
				mid->prot = prot;
				if (mid->file)
					vfs_incref(mid->file);
				mid->in_use = true;
				r->length = addr - r->start;
			} else {
				r->prot = prot;
			}
		}
	}

	for (uint64_t i = 0; i < pages; i++) {
		uint64_t vaddr = addr + i * PAGE_SIZE;
		uint64_t page_flags = flags;

		// Get current PTE
		uint64_t phys = mm_get_physical_address(vaddr);
		if (phys == 0) {
			// Page not mapped
			continue;
		}

		/* Device MMIO PTEs (/dev/fb0): the marker and caching bits
		 * must survive protection changes — losing PAGE_DEVICE would
		 * make a later unmap free BAR memory into the allocator. */
		{
			uint64_t *pte =
				mm_get_page_table_from_pml4(pml4, vaddr, false);
			if (pte && (*pte & PAGE_DEVICE))
				page_flags |= PAGE_DEVICE |
					      (*pte & (PAGE_WRITE_THROUGH |
						       PAGE_CACHE_DISABLE));
		}

		// Remap with new protection
		mm_map_page_in_address_space(pml4, vaddr, phys, flags);
	}

	// Flush TLB for modified pages on local CPU
	// Use virt_to_phys() for the PML4 pointer itself (not mm_get_physical_address)
	__asm__ volatile("mov %0, %%cr3" : : "r"(virt_to_phys(pml4)));

	// TLB shootdown: threads sharing this address space (CLONE_VM) may be running
	// on other CPUs with stale TLB entries. Broadcast invalidation to all CPUs.
	smp_tlb_shootdown_sync();

	return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
	RUN_WRITE_LOCKED(sys_mprotect_locked(addr, len, prot));
}

// reboot() magic numbers and commands
#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793 // 0x28121969
#define LINUX_REBOOT_MAGIC2A 85072278
#define LINUX_REBOOT_MAGIC2B 369367448
#define LINUX_REBOOT_MAGIC2C 537993216

#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123
#define LINUX_REBOOT_CMD_CAD_ON 0x89ABCDEF
#define LINUX_REBOOT_CMD_CAD_OFF 0x00000000
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC
#define LINUX_REBOOT_CMD_RESTART2 0xA1B2C3D4

static int g_cad_enabled = 0; // Ctrl-Alt-Del behaviour

static int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd,
			  uint64_t arg)
{
	// Validate magic numbers
	if ((uint32_t)magic1 != LINUX_REBOOT_MAGIC1)
		return -EINVAL;

	uint32_t m2 = (uint32_t)magic2;
	if (m2 != LINUX_REBOOT_MAGIC2 && m2 != LINUX_REBOOT_MAGIC2A &&
	    m2 != LINUX_REBOOT_MAGIC2B && m2 != LINUX_REBOOT_MAGIC2C)
		return -EINVAL;

	// Halting, powering off or rebooting the machine is privileged: only a
	// process with an effective uid of 0 (root) may do it.
	if (!capable())
		return -EPERM;

	// Flush filesystems (pagecache + journal) before going down so a journalled
	// root isn't left dirty (which would force a replay on the next boot).
	switch ((uint32_t)cmd) {
	case LINUX_REBOOT_CMD_RESTART:
	case LINUX_REBOOT_CMD_HALT:
	case LINUX_REBOOT_CMD_POWER_OFF:
	case LINUX_REBOOT_CMD_RESTART2:
		sys_sync();
		break;
	default:
		break;
	}

	switch ((uint32_t)cmd) {
	case LINUX_REBOOT_CMD_RESTART:
		kprintf("[REBOOT] System is going down for reboot NOW!\n");
		__asm__ volatile("cli");
		smp_halt_others();
		acpi_reset();
		for (;;)
			__asm__ volatile("hlt");

	case LINUX_REBOOT_CMD_HALT:
		kprintf("[HALT] System halted.\n");
		__asm__ volatile("cli");
		smp_halt_others();
		for (;;)
			__asm__ volatile("hlt");

	case LINUX_REBOOT_CMD_POWER_OFF:
		kprintf("[POWEROFF] Power down.\n");
		__asm__ volatile("cli");
		smp_halt_others();
		acpi_poweroff();
		for (;;)
			__asm__ volatile("hlt");

	case LINUX_REBOOT_CMD_CAD_ON:
		g_cad_enabled = 1;
		return 0;

	case LINUX_REBOOT_CMD_CAD_OFF:
		g_cad_enabled = 0;
		return 0;

	case LINUX_REBOOT_CMD_RESTART2: {
		// arg is a pointer to a command string (ignored in our impl)
		kprintf("[REBOOT] System is going down for reboot NOW!\n");
		__asm__ volatile("cli");
		smp_halt_others();
		acpi_reset();
		for (;;)
			__asm__ volatile("hlt");
	}

	default:
		return -EINVAL;
	}
}

// SYS_GETPROCINFO - retrieve info about all processes
// a1 = pointer to user-space procinfo_t array
// a2 = max number of entries the array can hold
// Returns: number of entries filled, or negative error
/* SYS_GETPROCMAPS - report one process's address space.
 *
 * ps can only show a total, and a total cannot tell a table filling up with
 * records from a few records that are growing, which are different faults with
 * different fixes.  This hands out the region table itself plus the brk span,
 * so the question "what exactly is growing" is answerable from userspace
 * instead of by rebuilding the kernel with a printf in it.
 *
 * Reported for the PROCESS: the bookkeeping lives on the thread group leader
 * (task_mm_owner), so asking about any thread answers for the address space it
 * shares.
 */
static int64_t sys_getprocmaps(uint64_t pid, uint64_t info_ptr,
			       uint64_t buf_ptr, uint64_t max_count)
{
	procmapinfo_t kinfo;
	procmap_t *kbuf = NULL;
	size_t buf_size = 0;
	uint64_t flags;
	int count = 0;
	int64_t ret;

	if (!validate_user_ptr(info_ptr, sizeof(procmapinfo_t)))
		return -EFAULT;
	if (max_count > 65536)
		max_count = 65536;
	if (max_count) {
		if (!validate_user_ptr(buf_ptr, max_count * sizeof(procmap_t)))
			return -EFAULT;
		buf_size = (size_t)max_count * sizeof(procmap_t);
		kbuf = (procmap_t *)kalloc(buf_size);
		if (!kbuf)
			return -ENOMEM;
		mm_memset(kbuf, 0, buf_size);
	}
	mm_memset(&kinfo, 0, sizeof(kinfo));

	spin_lock_irqsave(&g_task_list_lock, &flags);
	{
		task_t *t = sched_find_task_by_id_locked((uint32_t)pid);
		task_t *mm = t ? task_mm_owner(t) : NULL;

		if (!t || !mm) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			if (kbuf)
				kfree(kbuf);
			return -ESRCH;
		}
		kinfo.pid = (int)t->id;
		kinfo.tgid = t->tgid;
		kinfo.brk_start = mm->brk_start;
		kinfo.brk = mm->brk;
		kinfo.mmap_base = mm->mmap_base;
		kinfo.capacity = mm->mmap_capacity;

		for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
			mmap_region_t *r = &mm->mmap_regions[i];

			if (!r->in_use)
				continue;
			kinfo.n_regions++;
			kinfo.total_bytes += r->length;
			if (kbuf && count < (int)max_count) {
				kbuf[count].start = r->start;
				kbuf[count].length = r->length;
				kbuf[count].prot = r->prot;
				kbuf[count].flags = r->flags;
				kbuf[count].offset = r->offset;
				kbuf[count].file_backed = r->file ? 1 : 0;
				kbuf[count].lazy = r->lazy ? 1 : 0;
				kbuf[count].device = r->device ? 1 : 0;
				count++;
			}
		}
	}
	spin_unlock_irqrestore(&g_task_list_lock, flags);

	ret = count;
	if (copy_to_user((void *)info_ptr, &kinfo, sizeof(kinfo)) < 0)
		ret = -EFAULT;
	else if (kbuf && count &&
		 copy_to_user((void *)buf_ptr, kbuf,
			      (size_t)count * sizeof(procmap_t)) < 0)
		ret = -EFAULT;
	if (kbuf)
		kfree(kbuf);
	return ret;
}

static int64_t sys_getprocinfo(uint64_t buf_ptr, uint64_t max_count)
{
	if (max_count == 0)
		return 0;
	if (!validate_user_ptr(buf_ptr, max_count * sizeof(procinfo_t)))
		return -EFAULT;

	// Allocate a kernel-side buffer (limit to prevent DoS)
	if (max_count > 4096)
		max_count = 4096;
	size_t buf_size = max_count * sizeof(procinfo_t);
	procinfo_t *kbuf = (procinfo_t *)kalloc(buf_size);
	if (!kbuf)
		return -ENOMEM;
	mm_memset(kbuf, 0, buf_size);

	uint64_t freq = timer_get_frequency();
	if (freq == 0)
		freq = 100;

	uint64_t flags;
	int count = 0;
	spin_lock_irqsave(&g_task_list_lock, &flags);

	// g_task_list_head is declared static in sched.c, but we can
	// iterate using sched_find_task_by_id or we use extern.
	// Actually we declared g_task_list_lock extern in sched.h,
	// but not g_task_list_head. Let's just use a different approach:
	// iterate IDs from 0 upward.
	// Actually, let's access the list directly. We need to declare it extern.
	// For now, use the approach of iterating via sched_find_task_by_id
	// which acquires its own lock... but we already hold the lock.
	// Better: we declared an extern iterator in the header or iterate by PID.

	// We'll iterate PIDs. Not ideal but safe. sched_find_task_by_id
	// acquires the lock internally, so we must NOT hold it here.
	spin_unlock_irqrestore(&g_task_list_lock, flags);

	// Iterate all possible PIDs (g_next_id is the next ID to assign)
	extern int g_next_id;
	int max_pid = g_next_id;

	for (int pid = 0; pid < max_pid && count < (int)max_count; pid++) {
		spin_lock_irqsave(&g_task_list_lock, &flags);
		task_t *t = sched_find_task_by_id_locked(pid);
		if (!t || sched_task_hidden(t)) {
			/* Skip empty slots and the kernel's swapper-class tasks
			 * (bootstrap + idle), which are not real processes. */
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			continue;
		}

		procinfo_t *p = &kbuf[count];
		p->pid = t->id;
		p->ppid = sched_get_ppid(t);
		p->tgid = t->tgid;
		p->pgid = t->pgid;
		p->sid = t->sid;
		p->state = (int)t->state;
		p->nice = 0;
		p->nr_threads =
			t->group_leader ? t->group_leader->nr_threads : 1;
		p->on_cpu = t->on_cpu;
		p->exit_code = t->exit_code;
		/* Encode tty_nr consistently with ps/top:
         *   0        = no controlling terminal
         *   1        = console (g_console_tty.id == 1, is_pty == 0)
         *   2+       = pts/(tty_nr - 2)  (PTY slave id is 0-based, +2 avoids
         *              collision with 0="none" and 1="console") */
		if (!t->ctty)
			p->tty_nr = 0;
		else if (t->ctty->is_pty)
			p->tty_nr = t->ctty->id + 2;
		else
			p->tty_nr = t->ctty->id;
		p->is_kernel = (t->privilege == TASK_KERNEL) ? 1 : 0;
		p->start_tick = t->start_tick;
		p->utime_ticks = t->utime_ticks;
		p->stime_ticks = t->stime_ticks;
		/* Only meaningful while the task is actually asleep; a running
		 * task's last blocking site would be a stale answer. */
		p->wchan = (t->state == TASK_BLOCKED) ? t->wchan_rip : 0;

		// Real and effective credentials of the process.
		p->uid = (int)t->cred.uid;
		p->gid = (int)t->cred.gid;
		p->euid = (int)t->cred.euid;
		p->egid = (int)t->cred.egid;

		// VSZ: count pages mapped in user space (rough estimate)
		p->vsz = 0;
		p->rss = 0;
		if (t->privilege == TASK_USER) {
			// Estimate from brk and mmap
			if (t->brk > t->brk_start)
				p->vsz += (t->brk - t->brk_start);
			// User stack (assume 2MB)
			p->vsz += 2 * 1024 * 1024;
			// mmap regions
			for (uint32_t i = 0; i < t->mmap_capacity; i++) {
				if (t->mmap_regions[i].in_use)
					p->vsz += t->mmap_regions[i].length;
			}
			/* RSS: the pages actually resident, counted from the
			 * page tables -- not VSZ restated, which is what this
			 * used to report.  A process that returns physical
			 * memory but keeps its address space looked like an
			 * unbounded leak under the old number. */
			p->rss = mm_count_resident_pages(t->pml4);
		}

		// Copy comm
		for (int i = 0; i < 255 && t->comm[i]; i++)
			p->comm[i] = t->comm[i];
		p->comm[255] = '\0';

		// Copy cmdline
		for (int i = 0; i < 1023 && t->cmdline[i]; i++)
			p->cmdline[i] = t->cmdline[i];
		p->cmdline[1023] = '\0';

		// Copy environ
		for (int i = 0; i < 2047 && t->environ[i]; i++)
			p->environ[i] = t->environ[i];
		p->environ[2047] = '\0';

		// Copy cwd
		for (int i = 0; i < 255 && t->cwd[i]; i++)
			p->cwd[i] = t->cwd[i];
		p->cwd[255] = '\0';

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		count++;
	}

	// Copy to user space
	int err =
		copy_to_user((void *)buf_ptr, kbuf, count * sizeof(procinfo_t));
	kfree(kbuf);

	if (err)
		return err;
	return count;
}

// ============================================================================
// SYS_SYSINFO - Return system information (memory, uptime, load averages)
// ============================================================================
static int64_t sys_sysinfo(uint64_t info_ptr)
{
	if (!validate_user_ptr(info_ptr, sizeof(k_sysinfo_t)))
		return -EFAULT;

	k_sysinfo_t info;
	kmemset(&info, 0, sizeof(info));

	// Uptime
	info.uptime = (long)timer_get_uptime();

	// Load averages
	sched_get_loadavg(info.loads);

	// Memory stats
	memory_stats_t mstats;
	mm_get_memory_stats(&mstats);
	info.totalram = mstats.total_memory;
	info.freeram = mstats.free_memory;
	info.sharedram = 0; // no tmpfs/shm accounting
	/* buff/cache, matching what free(1)/top expect the fields to mean:
	 *   bufferram — block/metadata buffers filesystem drivers reported
	 *               via mm_buffercache_account()
	 *   cached    — page cache plus the reclaimable entry caches
	 *               (inode + dentry caches, both LRU-evictable) */
	info.bufferram = mm_buffercache_bytes();
	info.totalswap = 0;
	info.freeswap = 0;
	info.procs = (unsigned short)sched_get_nr_procs();
	info.totalhigh = 0;
	info.freehigh = 0;
	info.mem_unit = 1; // byte granularity
	info.cached = mstats.pagecache_pages * PAGE_SIZE + icache_mem_bytes() +
		      dcache_mem_bytes();
	info.available = info.freeram + info.bufferram + info.cached;

	if (copy_to_user((void *)info_ptr, &info, sizeof(info)) != 0)
		return -EFAULT;
	return 0;
}

// ============================================================================
// SYS_KLOGCTL - Kernel ring buffer operations (for dmesg)
// ============================================================================
static int64_t sys_klogctl(uint64_t type, uint64_t bufp, uint64_t len)
{
	/* Reading or clearing the kernel log is privileged (dmesg is root-only). */
	if (!capable())
		return -EPERM;
	switch ((int)type) {
	case SYSLOG_ACTION_READ:
	case SYSLOG_ACTION_READ_ALL: {
		if (len == 0)
			return 0;
		if (!bufp || !validate_user_ptr(bufp, (size_t)len))
			return -EFAULT;
		int ksize = klog_size();
		int rlen = (int)len;
		if (rlen > ksize)
			rlen = ksize;
		// Allocate kernel temp buffer
		char *tmp = (char *)kalloc(rlen + 1);
		if (!tmp)
			return -ENOMEM;
		int got = klog_read(tmp, rlen);
		if (copy_to_user((void *)bufp, tmp, got) != 0) {
			kfree(tmp);
			return -EFAULT;
		}
		kfree(tmp);
		return got;
	}
	case SYSLOG_ACTION_READ_CLEAR: {
		if (len == 0)
			return 0;
		if (!bufp || !validate_user_ptr(bufp, (size_t)len))
			return -EFAULT;
		int ksize = klog_size();
		int rlen = (int)len;
		if (rlen > ksize)
			rlen = ksize;
		char *tmp = (char *)kalloc(rlen + 1);
		if (!tmp)
			return -ENOMEM;
		int got = klog_read_clear(tmp, rlen);
		if (copy_to_user((void *)bufp, tmp, got) != 0) {
			kfree(tmp);
			return -EFAULT;
		}
		kfree(tmp);
		return got;
	}
	case SYSLOG_ACTION_CLEAR:
		klog_clear();
		return 0;
	case SYSLOG_ACTION_SIZE_BUFFER:
		return klog_size();
	default:
		return -EINVAL;
	}
}

// Helper: extract socket index from a process fd (via fd_table marker)
/* Hand an accepted peer address back to userspace.
 *
 * addrlen is IN/OUT and both halves matter: on the way in it states how big
 * the caller's buffer is, and NOTHING may be written past it; on the way out it
 * reports the address's true size, which may be larger -- that is how a caller
 * learns the answer was truncated.
 *
 * The accept arms used to ignore the incoming value entirely and copy a whole
 * sizeof(struct sockaddr_un) -- 110 bytes -- into whatever the caller passed.
 * `struct sockaddr' is 16, and passing one is completely ordinary:
 * menu-cached does exactly that, so every client connection wrote 94 bytes
 * past a stack buffer.  It flattened the saved registers and return addresses
 * below it and the function returned into the wreckage, which showed up as
 * SIGSEGV at RIP 0 with no call frame to walk.  validate_user_ptr() cannot
 * catch it: the stack beyond the buffer is perfectly writable memory, it just
 * belongs to somebody else. */
static void sock_put_peer_addr(uint64_t uaddr, uint64_t ulenp,
			       const void *kaddr, socklen_t kaddrlen)
{
	socklen_t ulen = 0;

	if (!uaddr || !ulenp)
		return;
	if (!validate_user_ptr(ulenp, sizeof(socklen_t)))
		return;
	if (copy_from_user(&ulen, (const void *)ulenp, sizeof(socklen_t)) < 0)
		return;

	if (ulen > 0) {
		socklen_t n = (kaddrlen < ulen) ? kaddrlen : ulen;

		if (n && validate_user_ptr(uaddr, n))
			copy_to_user((void *)uaddr, kaddr, n);
	}
	/* The real length, not the truncated one. */
	copy_to_user((void *)ulenp, &kaddrlen, sizeof(socklen_t));
}

static int sock_idx_from_fd(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS)
		return -EBADF;
	void *entry = task_fds(cur)[fd];
	if (!entry)
		return -EBADF;
	if (!IS_SOCKET_FD(entry))
		return -ENOTSOCK;
	return SOCKET_FD_IDX(entry);
}

/* Resolve a descriptor to a UNIX socket, and hold the socket for the rest of
 * this syscall.
 *
 * The descriptor read and the reference are taken together under the
 * descriptor-table lock, so a sibling thread closing the same descriptor
 * cannot slip between them.  The reference is parked on the task and released
 * by syscall_handler() once the call returns -- one acquire, one release,
 * rather than a release on each of the many early returns these arms have. */
static unix_socket_t *unix_sock_from_fd(uint64_t fd)
{
	task_t *cur = sched_current();
	uint64_t lflags;
	void *entry;

	if (!cur || fd >= TASK_MAX_FDS)
		return NULL;

	fds_lock(cur, &lflags);
	entry = task_fds(cur)[fd];
	if (!entry || !unix_sock_is(entry)) {
		fds_unlock(cur, lflags);
		return NULL;
	}
	/* One resolution per syscall: every arm resolves a1 once.  A second
	 * would strand the first reference, so say so rather than leak. */
	WARN_ON_ONCE(cur->syscall_unix_ref != NULL);
	if (!cur->syscall_unix_ref)
		cur->syscall_unix_ref =
			unix_sock_lookup_hold((unix_socket_t *)entry);
	fds_unlock(cur, lflags);

	return cur->syscall_unix_ref;
}

// Helper: extract epoll index from a process fd
static int epoll_idx_from_fd(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur || fd >= TASK_MAX_FDS)
		return -EBADF;
	void *entry = task_fds(cur)[fd];
	if (!entry)
		return -EBADF;
	if (!IS_EPOLL_FD(entry))
		return -EBADF;
	return EPOLL_FD_IDX(entry);
}

// ---------------------------------------------------------------------------
// Noinline helpers for syscalls with large stack-allocated buffers.
// Keeping these out of syscall_handler_inner prevents the compiler from
// reserving stack space for ALL local arrays at function entry, which was
// blowing past the 8 KB kernel stack.
// ---------------------------------------------------------------------------

/* ppoll()/pselect() take a signal mask that must be installed for exactly the
 * duration of the wait and restored afterwards.  Ignoring it broke the
 * standard "block the signal, then let ppoll unblock it while waiting" idiom:
 * the signal stayed blocked, signal_pending() correctly skipped it, the wait
 * was never interrupted and the handler never ran.  sshd uses precisely that
 * idiom for SIGCHLD, so exited sessions were left unreaped as zombies until
 * some unrelated event happened to wake the listener.
 *
 * Returns 1 if a mask was installed (caller must restore `saved`), 0 if none
 * was supplied, or a negative errno. */
static int poll_sigmask_install(uint64_t umask_ptr, kernel_sigset_t *saved)
{
	task_t *cur = sched_current();
	if (!cur || umask_ptr == 0)
		return 0;
	if (!validate_user_ptr(umask_ptr, sizeof(kernel_sigset_t)))
		return -EFAULT;
	kernel_sigset_t newset;
	if (copy_from_user(&newset, (void *)umask_ptr,
			   sizeof(kernel_sigset_t)) != 0)
		return -EFAULT;
	*saved = cur->signals.blocked;
	/* Park the caller's mask for the deferred restore (see the field
	 * comment in struct task): it must stay OFF until signal delivery has
	 * had its chance, otherwise the signal the caller unblocked for the
	 * wait is re-blocked before its handler can run. */
	cur->sigmask_saved = *saved;
	cur->sigmask_restore_pending = 1;
	cur->signals.blocked = newset;
	sig_strip_unblockable(&cur->signals.blocked);
	return 1;
}

/* Put the caller's mask back if nothing else already did (i.e. no handler was
 * set up, which would have handed the restore to sigreturn). */
void poll_sigmask_restore_pending(task_t *cur)
{
	if (!cur || !cur->sigmask_restore_pending)
		return;
	cur->sigmask_restore_pending = 0;
	cur->signals.blocked = cur->sigmask_saved;
	sig_strip_unblockable(&cur->signals.blocked);
}

__attribute__((noinline)) static int64_t
sys_select_wrapper(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
		   uint64_t a5)
{
	fd_set kr, kw, ke;
	fd_set *rp = NULL, *wp = NULL, *ep = NULL;
	if (a2 && validate_user_ptr(a2, sizeof(fd_set))) {
		copy_from_user(&kr, (void *)a2, sizeof(fd_set));
		rp = &kr;
	}
	if (a3 && validate_user_ptr(a3, sizeof(fd_set))) {
		copy_from_user(&kw, (void *)a3, sizeof(fd_set));
		wp = &kw;
	}
	if (a4 && validate_user_ptr(a4, sizeof(fd_set))) {
		copy_from_user(&ke, (void *)a4, sizeof(fd_set));
		ep = &ke;
	}
	uint64_t timeout_ticks = (uint64_t)-1;
	if (a5 && validate_user_ptr(a5, 16)) {
		uint64_t tv_sec = 0, tv_usec = 0;
		copy_from_user(&tv_sec, (void *)a5, 8);
		copy_from_user(&tv_usec, (void *)(a5 + 8), 8);
		/* Converted at the measured tick rate.  This used to assume
		 * 100Hz, so on a machine whose calibrated rate is ~200Hz every
		 * select() timeout expired in half the requested time. */
		timeout_ticks =
			timer_us_to_ticks(tv_sec * 1000000ULL + tv_usec);
		if (tv_sec == 0 && tv_usec == 0)
			timeout_ticks = 0;
	}
	int ret = sys_select_internal((int)a1, rp, wp, ep, timeout_ticks);
	if (rp && a2)
		copy_to_user((void *)a2, rp, sizeof(fd_set));
	if (wp && a3)
		copy_to_user((void *)a3, wp, sizeof(fd_set));
	if (ep && a4)
		copy_to_user((void *)a4, ep, sizeof(fd_set));
	return ret;
}

__attribute__((noinline)) static int64_t
sys_pselect6_wrapper(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
		     uint64_t a5, uint64_t a6)
{
	fd_set kr, kw, ke;
	fd_set *rp = NULL, *wp = NULL, *ep = NULL;
	if (a2 && validate_user_ptr(a2, sizeof(fd_set))) {
		copy_from_user(&kr, (void *)a2, sizeof(fd_set));
		rp = &kr;
	}
	if (a3 && validate_user_ptr(a3, sizeof(fd_set))) {
		copy_from_user(&kw, (void *)a3, sizeof(fd_set));
		wp = &kw;
	}
	if (a4 && validate_user_ptr(a4, sizeof(fd_set))) {
		copy_from_user(&ke, (void *)a4, sizeof(fd_set));
		ep = &ke;
	}
	uint64_t timeout_ticks = (uint64_t)-1;
	if (a5 && validate_user_ptr(a5, 16)) {
		uint64_t tv_sec = 0;
		long tv_nsec = 0;
		copy_from_user(&tv_sec, (void *)a5, 8);
		copy_from_user(&tv_nsec, (void *)(a5 + 8), 8);
		/* Measured tick rate, rounded up.  `tv_sec * 100 + tv_nsec/1e7'
		 * assumed a 10ms tick and truncated the remainder, so this
		 * expired early on both counts. */
		timeout_ticks = timer_ns_to_ticks(tv_sec * 1000000000ULL +
						  (uint64_t)tv_nsec);
		if (tv_sec == 0 && tv_nsec == 0)
			timeout_ticks = 0;
	}
	kernel_sigset_t saved_mask;
	int have_mask = poll_sigmask_install(a6, &saved_mask);
	if (have_mask < 0)
		return have_mask;
	int ret = sys_select_internal((int)a1, rp, wp, ep, timeout_ticks);
	/* Mask restored after signal delivery — see the ppoll wrapper. */
	(void)saved_mask;
	if (rp && a2)
		copy_to_user((void *)a2, rp, sizeof(fd_set));
	if (wp && a3)
		copy_to_user((void *)a3, wp, sizeof(fd_set));
	if (ep && a4)
		copy_to_user((void *)a4, ep, sizeof(fd_set));
	return ret;
}

__attribute__((noinline)) static int64_t
sys_poll_wrapper(uint64_t a1, uint64_t a2, uint64_t a3)
{
	int nfds = (int)a2;
	if (nfds < 0 || nfds > 256)
		return -EINVAL;
	size_t sz = (size_t)nfds * sizeof(struct pollfd);
	if (!validate_user_ptr(a1, sz))
		return -EFAULT;
	struct pollfd kfds[256];
	copy_from_user(kfds, (void *)a1, sz);
	int timeout_ms = (int)(int64_t)a3;
	uint64_t timeout_ticks;
	if (timeout_ms < 0)
		timeout_ticks = (uint64_t)-1;
	else if (timeout_ms == 0)
		timeout_ticks = 0;
	else
		/* Measured tick rate, rounded up: `ms / 10' assumed a 10ms tick
		 * AND discarded the remainder, so this returned early twice
		 * over -- a 200ms poll() came back in about 129ms. */
		timeout_ticks = timer_ms_to_ticks((uint64_t)timeout_ms);
	int ret = sys_poll_internal(kfds, nfds, timeout_ticks);
	copy_to_user((void *)a1, kfds, sz);
	return ret;
}

__attribute__((noinline)) static int64_t
sys_ppoll_wrapper(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	int nfds = (int)a2;
	if (nfds < 0 || nfds > 256)
		return -EINVAL;
	size_t sz = (size_t)nfds * sizeof(struct pollfd);
	if (!validate_user_ptr(a1, sz))
		return -EFAULT;
	struct pollfd kfds[256];
	copy_from_user(kfds, (void *)a1, sz);
	uint64_t timeout_ticks = (uint64_t)-1;
	if (a3 && validate_user_ptr(a3, 16)) {
		uint64_t tv_sec = 0;
		long tv_nsec = 0;
		copy_from_user(&tv_sec, (void *)a3, 8);
		copy_from_user(&tv_nsec, (void *)(a3 + 8), 8);
		/* Measured tick rate, rounded up.  `tv_sec * 100 + tv_nsec/1e7'
		 * assumed a 10ms tick and truncated the remainder, so this
		 * expired early on both counts. */
		timeout_ticks = timer_ns_to_ticks(tv_sec * 1000000000ULL +
						  (uint64_t)tv_nsec);
		if (tv_sec == 0 && tv_nsec == 0)
			timeout_ticks = 0;
	}
	kernel_sigset_t saved_mask;
	int have_mask = poll_sigmask_install(a4, &saved_mask);
	if (have_mask < 0)
		return have_mask;
	int ret = sys_poll_internal(kfds, nfds, timeout_ticks);
	/* The mask stays installed on purpose; it is put back after signal
	 * delivery (poll_sigmask_restore_pending). */
	(void)saved_mask;
	copy_to_user((void *)a1, kfds, sz);
	return ret;
}

__attribute__((noinline)) static int64_t
sys_epoll_wait_wrapper(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	int ep_idx = epoll_idx_from_fd(a1);
	if (ep_idx < 0)
		return ep_idx;
	int maxevents = (int)a3;
	if (maxevents <= 0 || maxevents > 256)
		return -EINVAL;
	size_t sz = (size_t)maxevents * sizeof(struct epoll_event);
	if (!validate_user_ptr(a2, sz))
		return -EFAULT;
	struct epoll_event kevs[256];
	int timeout_ms = (int)(int64_t)a4;
	uint64_t timeout_ticks;
	if (timeout_ms < 0)
		timeout_ticks = (uint64_t)-1;
	else if (timeout_ms == 0)
		timeout_ticks = 0;
	else
		/* Measured tick rate, rounded up: `ms / 10' assumed a 10ms tick
		 * AND discarded the remainder, so this returned early twice
		 * over -- a 200ms poll() came back in about 129ms. */
		timeout_ticks = timer_ms_to_ticks((uint64_t)timeout_ms);
	int ret = epoll_wait_internal(ep_idx, kevs, maxevents, timeout_ticks);
	if (ret > 0)
		copy_to_user((void *)a2, kevs,
			     (size_t)ret * sizeof(struct epoll_event));
	return ret;
}

// ---------------------------------------------------------------------------
// UNIX-domain sendmsg / recvmsg helpers.
// Both carry large stack objects (iov[256], 4 KB data buffer) so they are
// kept out of syscall_handler_inner to avoid bloating the kernel stack.
// ---------------------------------------------------------------------------

__attribute__((noinline)) static int unix_do_sendmsg(unix_socket_t *ufd,
						     struct msghdr *kmsg)
{
	unix_socket_t *us = ufd;
	if (!us)
		return -EBADF;
	/* The peer is deliberately NOT resolved here.  Reading it once and
	 * using it further down is what let an in-band descriptor be queued on
	 * a socket that had been replaced in the meantime; unix_send_fd()
	 * finds and pins it for the length of the operation instead. */
	if (!us->connected)
		return -ENOTCONN;

	/* Process control data first so the fd arrives before (or with) the
     * byte that the receiver associates it with.  The imsg framing tmux
     * uses sends one fd per message and the receiver pops the next pending
     * fd when it parses each imsg header. */
	if (kmsg->msg_control &&
	    kmsg->msg_controllen >= sizeof(struct cmsghdr)) {
		size_t clen = kmsg->msg_controllen;
		if (clen > 256)
			clen = 256;
		unsigned char cbuf[256];
		if (!validate_user_ptr((uint64_t)kmsg->msg_control, clen))
			return -EFAULT;
		copy_from_user(cbuf, kmsg->msg_control, clen);
		size_t off = 0;
		task_t *cur = sched_current();
		if (!cur)
			return -EFAULT;
		while (off + sizeof(struct cmsghdr) <= clen) {
			struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + off);
			if (cmsg->cmsg_len < sizeof(struct cmsghdr))
				break;
			if (off + cmsg->cmsg_len > clen)
				break;
			if (cmsg->cmsg_level == SOL_SOCKET &&
			    cmsg->cmsg_type == SCM_RIGHTS) {
				size_t hdr_align =
					CMSG_ALIGN(sizeof(struct cmsghdr));
				int n = (int)((cmsg->cmsg_len - hdr_align) /
					      sizeof(int));
				int *fds = (int *)(cbuf + off + hdr_align);
				for (int i = 0; i < n; i++) {
					int sfd = fds[i];
					if (sfd < 0 || sfd >= TASK_MAX_FDS)
						continue;
					/* Take the reference that keeps the
					 * entry alive until the peer receives
					 * it -- the sender may close its own
					 * descriptor first.
					 *
					 * Read and reference happen together:
					 * this open-coded the whole of
					 * fd_dup_entry, including its race
					 * against a sibling thread closing the
					 * same descriptor between the two. */
					void *entry = fd_dup_entry_at(cur, sfd);
					if (!entry)
						continue;
					(void)unix_send_fd(us, entry);
				}
			}
			off += CMSG_ALIGN(cmsg->cmsg_len);
		}
	}

	if (kmsg->msg_iovlen <= 0)
		return 0;
	/* Cap the iov count so the on-stack copy of the array is bounded. */
	int kiovcnt = kmsg->msg_iovlen;
	if (kiovcnt > 256)
		kiovcnt = 256;
	struct iovec iov[256];
	if (!validate_user_ptr((uint64_t)kmsg->msg_iov,
			       sizeof(struct iovec) * (size_t)kiovcnt))
		return -EFAULT;
	copy_from_user(iov, kmsg->msg_iov,
		       sizeof(struct iovec) * (size_t)kiovcnt);

	/* Send each iovec in turn, stopping at the first short write, exactly as
	 * sys_writev does.  A stream socket has no message boundaries, so this
	 * is indistinguishable from one big write -- and unix_send already
	 * bounces user memory through its own small on-stack buffer, so no
	 * flattening buffer is needed here at all.
	 *
	 * It used to flatten into a `static uint8_t sbuf[4096]`: ONE buffer for
	 * the whole system, with no lock.  Two processes sending on unix sockets
	 * at the same time overwrote each other's bytes, so each peer received a
	 * stream with someone else's data spliced into it.  Every byte an X
	 * client writes goes through here, which is how a window resize -- the
	 * window manager and the terminal both bursting at once -- ended in
	 * "[xcb] Unknown sequence number while processing queue".  Same bug
	 * class as the ext4 xattr static scratch list. */
	int64_t sent_total = 0;
	for (int i = 0; i < kiovcnt; i++) {
		size_t want = iov[i].iov_len;
		if (want == 0)
			continue;
		/* Bounded per call so the byte count cannot overflow the int this
		 * returns.  A caller that asked for more sees a short write and
		 * comes back for the rest, which is what it must already do for a
		 * full peer ring. */
		if (want > 65536)
			want = 65536;
		if (!validate_user_ptr((uint64_t)iov[i].iov_base, want))
			return sent_total ? (int)sent_total : -EFAULT;
		int r = unix_send(ufd, iov[i].iov_base, want, 0);
		if (r < 0)
			return sent_total ? (int)sent_total : r;
		sent_total += r;
		if ((size_t)r < want)
			break; /* peer's ring is full; report what went */
	}
	return (int)sent_total;
}

__attribute__((noinline)) static int unix_do_recvmsg(unix_socket_t *ufd,
						     struct msghdr *kmsg)
{
	unix_socket_t *us = ufd;
	if (!us)
		return -EBADF;

	if (kmsg->msg_iovlen <= 0)
		return 0;
	int riovcnt = kmsg->msg_iovlen;
	if (riovcnt > 256)
		riovcnt = 256;
	struct iovec iov[256];
	if (!validate_user_ptr((uint64_t)kmsg->msg_iov,
			       sizeof(struct iovec) * (size_t)riovcnt))
		return -EFAULT;
	copy_from_user(iov, kmsg->msg_iov,
		       sizeof(struct iovec) * (size_t)riovcnt);
	size_t total = 0;
	for (int i = 0; i < riovcnt; i++)
		total += iov[i].iov_len;
	if (total == 0)
		return 0;

	/* Stream-mode SCM_RIGHTS framing: if a pending fd is queued at byte
     * offset N (in the receiver's bytes_read coordinate system), clamp
     * this recvmsg to either:
     *   - bytes_read < N: deliver only N - bytes_read bytes (no fd this
     *     round; fd waits for the next call);
     *   - bytes_read == N: deliver the fd and at most up to the next
     *     pending fd's offset bytes.
     * This keeps fds aligned with the imsg frame the sender attached them to. */
	int deliver_fd_now = 0;
	uint64_t fd_off = 0;
	int has_fd = (unix_peek_fd_offset(us, &fd_off) == 0);
	if (has_fd) {
		uint64_t br = us->bytes_read;
		if (br < fd_off) {
			size_t cap = (size_t)(fd_off - br);
			if (total > cap)
				total = cap;
		} else {
			deliver_fd_now = 1;
			/* Clamp to the offset of the next pending fd, if any, so we
             * don't accidentally pull data past it.  Look one slot ahead. */
			uint64_t irq_flags;
			spin_lock_irqsave(&us->lock, &irq_flags);
			int nxt = (us->pending_fd_head + 1) % 16;
			if (nxt != us->pending_fd_tail) {
				uint64_t nxt_off = us->pending_fd_off[nxt];
				if (nxt_off > br) {
					size_t cap = (size_t)(nxt_off - br);
					if (total > cap)
						total = cap;
				}
			}
			spin_unlock_irqrestore(&us->lock, irq_flags);
		}
	}

	/* Receive straight into the caller's iovecs.  unix_recv copies to user
	 * memory itself (with no lock held, so a demand fault may sleep), so
	 * there is nothing to stage through.
	 *
	 * This used to drain into a `static uint8_t rbuf[4096]`: ONE buffer for
	 * the whole system, with no lock.  Two processes reading unix sockets at
	 * the same time overwrote each other's bytes.  libxcb reads EVERY byte
	 * of the X protocol through recvmsg(), so a client got a stream with
	 * another client's data spliced into it and aborted with "[xcb] Unknown
	 * sequence number while processing queue" -- which needed simultaneous
	 * traffic to show up, hence a window resize triggering it.
	 *
	 * Only the first read may block.  Once any byte has been handed over the
	 * call must return what it has, so later iovecs use MSG_DONTWAIT and a
	 * drained ring ends the loop. */
	int got = 0;
	size_t off = 0;
	for (int i = 0; i < riovcnt && off < total; i++) {
		size_t want = iov[i].iov_len;
		if (want == 0)
			continue;
		if (off + want > total)
			want = total - off;
		if (!validate_user_ptr((uint64_t)iov[i].iov_base, want))
			return off ? (int)off : -EFAULT;
		int n = unix_recv(ufd, iov[i].iov_base, want,
				  off ? MSG_DONTWAIT : 0);
		if (n < 0) {
			if (off)
				break; /* keep what was already delivered */
			return n;
		}
		if (n == 0)
			break; /* peer closed, or nothing more queued */
		off += (size_t)n;
		if ((size_t)n < want)
			break; /* ring drained */
	}
	got = (int)off;

	/* Deliver one pending fd via SCM_RIGHTS, but only at the correct
     * byte boundary. */
	kmsg->msg_flags = 0;
	if (deliver_fd_now && kmsg->msg_control &&
	    kmsg->msg_controllen >= CMSG_SPACE(sizeof(int))) {
		void *entry = NULL;
		if (unix_pop_fd(us, &entry) == 0 && entry) {
			task_t *cur = sched_current();
			/* fd_install_from, not a bare scan: it claims the slot
			 * under the descriptor-table lock (so two threads of one
			 * process cannot be handed the same number) and clears
			 * the slot's flag byte -- a recycled slot that kept a
			 * stale FD_CLOEXEC made the received descriptor vanish
			 * at the next exec. */
			int newfd = cur ? fd_install_from(cur, entry, 0) : -1;
			if (newfd < 0) {
				/* No descriptor slot: give the reference back.
				 *
				 * Exactly the mirror of the fd_dup_entry_at()
				 * that took it, which is the point of routing
				 * it through the same place.  Hand-written, it
				 * unwound only some of what had been taken --
				 * it knew a socket had a descriptor count but
				 * not that the socket also had a lifetime
				 * reference, and it had no case at all for a
				 * regular file, so those were simply kept
				 * forever. */
				fd_release_entry((vfs_file_t *)entry);
				kmsg->msg_flags |= MSG_CTRUNC;
				kmsg->msg_controllen = 0;
			} else {
				unsigned char cbuf[CMSG_SPACE(sizeof(int))];
				struct cmsghdr *c = (struct cmsghdr *)cbuf;
				c->cmsg_len = CMSG_LEN(sizeof(int));
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = SCM_RIGHTS;
				*(int *)CMSG_DATA(c) = newfd;
				if (!validate_user_ptr(
					    (uint64_t)kmsg->msg_control,
					    CMSG_SPACE(sizeof(int))))
					return -EFAULT;
				copy_to_user(kmsg->msg_control, cbuf,
					     CMSG_SPACE(sizeof(int)));
				kmsg->msg_controllen = CMSG_SPACE(sizeof(int));
			}
		} else {
			kmsg->msg_controllen = 0;
		}
	} else {
		kmsg->msg_controllen = 0;
	}
	return got;
}

/* SYS_DEBUG_DUMP: root-only.  Emit the same diagnostic tables as the Ctrl+N /
 * Ctrl+D debug hotkeys (TCP connection table, AF_UNIX socket table, PTY table,
 * and the scheduler task list) to the active tty.  Lets userspace capture the
 * kernel state at a chosen moment — e.g. a watchdog that fires when an accept()
 * has hung — without needing a physical keypress.  All four dumps are lock-free
 * best-effort reads with no side effects. */
static int64_t sys_debug_dump(void)
{
	if (!capable())
		return -EPERM;
	extern void tcp_dump_table(struct tty * tty);
	extern void unix_dump_sockets(struct tty * tty);
	extern void tty_dump_ptys(struct tty * tty);
	extern void sched_dump_tasks(struct tty * tty);
	tty_t *t = tty_get_active();
	tcp_dump_table(t);
	unix_dump_sockets(t);
	tty_dump_ptys(t);
	sched_dump_tasks(t);
	return 0;
}

// Main syscall dispatcher (inner function)
static int64_t syscall_handler_inner(uint64_t num, uint64_t a1, uint64_t a2,
				     uint64_t a3, uint64_t a4, uint64_t a5,
				     uint64_t a6)
{
	/* All syscalls run in process context with IRQs enabled — any path
     * that allocates, blocks on I/O, or sleeps must be reachable.  If
     * we ever enter with IRQs off it means a kernel caller bypassed the
     * syscall entry stub; almost every syscall would deadlock. */
	WARN_ON_ONCE(irqs_disabled());
	switch (num) {
	case SYS_READ:
		return sys_read(a1, a2, a3);

	case SYS_WRITE:
		return sys_write(a1, a2, a3);

	case SYS_OPEN:
		return sys_open(a1, a2, a3);

	case SYS_CLOSE:
		return sys_close(a1);

	case SYS_LSEEK:
		return sys_lseek(a1, (int64_t)a2, a3);

	case SYS_MMAP:
		return sys_mmap(a1, a2, a3, a4, a5, a6);

	case SYS_MUNMAP:
		return sys_munmap(a1, a2);

	case SYS_BRK:
		return sys_brk(a1);

	case SYS_GETPID:
		return sys_getpid();

	case SYS_FORK:
		return sys_fork();

	case SYS_WAIT4:
		return sys_waitpid((int64_t)a1, a2, a3, a4);

	case SYS_GETPPID:
		return sys_getppid();

	case SYS_EXECVE:
		return sys_execve(a1, a2, a3);

	case SYS_DUP:
		return sys_dup(a1);

	case SYS_DUP2:
		return sys_dup2(a1, a2);

	case SYS_EXIT:
		sys_exit(a1);
		// Never returns

	case SYS_PIPE:
		return sys_pipe(a1);

	case SYS_YIELD:
		return sys_yield();

	case SYS_STAT:
		return sys_stat(a1, a2);

	case SYS_LSTAT:
		return sys_lstat(a1, a2);

	case SYS_FSTAT:
		return sys_fstat(a1, a2);

	case SYS_ACCESS:
		return sys_access(a1, a2);

	case SYS_CHDIR:
		return sys_chdir(a1);

	case SYS_SHMGET:
		return sys_shmget(a1, a2, a3);
	case SYS_SHMAT:
		return sys_shmat(a1, a2, a3);
	case SYS_SHMDT:
		return sys_shmdt(a1);
	case SYS_SHMCTL:
		return sys_shmctl(a1, a2, a3);

	case SYS_CHROOT:
		return sys_chroot(a1);

	case SYS_GETCWD:
		return sys_getcwd(a1, a2);

	case SYS_UMASK:
		return sys_umask(a1);

	case SYS_GETUID:
		return sys_getuid();

	case SYS_GETGID:
		return sys_getgid();

	case SYS_GETEUID:
		return sys_geteuid();

	case SYS_GETEGID:
		return sys_getegid();

	case SYS_SETUID:
		return sys_setuid(a1);

	case SYS_SETGID:
		return sys_setgid(a1);

	case SYS_SETEUID:
		return sys_seteuid(a1);

	case SYS_SETEGID:
		return sys_setegid(a1);

	case SYS_GETGROUPS:
		return sys_getgroups(a1, a2);

	case SYS_SETGROUPS:
		return sys_setgroups(a1, a2);

	case SYS_SETRESUID:
		return sys_setresuid(a1, a2, a3);
	case SYS_GETRESUID:
		return sys_getresuid(a1, a2, a3);
	case SYS_SETRESGID:
		return sys_setresgid(a1, a2, a3);
	case SYS_GETRESGID:
		return sys_getresgid(a1, a2, a3);
	case SYS_SETXATTR:
		return sys_setxattr(a1, a2, a3, a4, a5);
	case SYS_GETXATTR:
		return sys_getxattr(a1, a2, a3, a4, a5);
	case SYS_LISTXATTR:
		return sys_listxattr(a1, a2, a3, a4);
	case SYS_REMOVEXATTR:
		return sys_removexattr(a1, a2, a3);
	case SYS_FSETXATTR:
		return sys_fsetxattr(a1, a2, a3, a4, a5);
	case SYS_FGETXATTR:
		return sys_fgetxattr(a1, a2, a3, a4);
	case SYS_FLISTXATTR:
		return sys_flistxattr(a1, a2, a3);
	case SYS_FREMOVEXATTR:
		return sys_fremovexattr(a1, a2);

	case SYS_DEBUG_DUMP:
		return sys_debug_dump();

	case SYS_GETHOSTNAME:
		return sys_gethostname(a1, a2);

	case SYS_UNAME:
		return sys_uname(a1);

	case SYS_TIME:
		return sys_time(a1);

	case SYS_GETTIMEOFDAY:
		return sys_gettimeofday(a1, a2);

	case SYS_SETTIMEOFDAY:
		return sys_settimeofday(a1, a2);

	case SYS_FSYNC:
		return sys_fsync(a1);

	case SYS_FTRUNCATE:
		return sys_ftruncate(a1, a2);

	case SYS_FCNTL:
		return sys_fcntl(a1, a2, a3);

	case SYS_IOCTL:
		return sys_ioctl(a1, a2, a3);

	case SYS_SETPGID:
		return sys_setpgid(a1, a2);

	case SYS_GETPGRP:
		return sys_getpgrp();

	case SYS_TCGETPGRP:
		return sys_tcgetpgrp(a1);

	case SYS_TCSETPGRP:
		return sys_tcsetpgrp(a1, a2);

	case SYS_KILL:
		return sys_kill(a1, a2);

	case SYS_UNLINK:
		return sys_unlink(a1);

	case SYS_RENAME:
		return sys_rename(a1, a2);

	case SYS_MKDIR:
		return sys_mkdir(a1, a2);

	case SYS_RMDIR:
		return sys_rmdir(a1);

	case SYS_LINK:
		return sys_link(a1, a2);

	case SYS_SYMLINK:
		return sys_symlink(a1, a2);

	case SYS_READLINK:
		return sys_readlink(a1, a2, a3);

	case SYS_CHMOD:
		return sys_chmod(a1, a2);

	case SYS_FCHMOD:
		return sys_fchmod(a1, a2);

	case SYS_CHOWN:
		return sys_chown(a1, a2, a3);
	case SYS_OPENAT:
		return sys_openat(a1, a2, a3, a4);
	case SYS_UNLINKAT:
		return sys_unlinkat(a1, a2, a3);
	case SYS_FSTATAT:
		return sys_fstatat(a1, a2, a3, a4);
	case SYS_FACCESSAT:
		return sys_faccessat(a1, a2, a3, a4);
	case SYS_GETDENTS64:
		return sys_getdents64(a1, a2, a3);
	case SYS_GETDENTS:
		return sys_getdents(a1, a2, a3);

	case SYS_FCHOWN:
		return sys_fchown(a1, a2, a3);

	case SYS_UTIMENSAT:
		return sys_utimensat(a1, a2, a3, a4);

	case SYS_STATFS:
		return sys_statfs(a1, a2);
	case SYS_FSTATFS:
		return sys_fstatfs(a1, a2);

	// Signal syscalls
	case SYS_RT_SIGACTION:
		return sys_rt_sigaction(a1, a2, a3, a4);
	case SYS_RT_SIGPROCMASK:
		return sys_rt_sigprocmask(a1, a2, a3, a4);
	case SYS_RT_SIGPENDING:
		return sys_rt_sigpending(a1, a2);
	case SYS_RT_SIGTIMEDWAIT:
		return sys_rt_sigtimedwait(a1, a2, a3, a4);
	case SYS_RT_SIGQUEUEINFO:
		return sys_rt_sigqueueinfo(a1, a2, a3);
	case SYS_RT_SIGSUSPEND:
		return sys_rt_sigsuspend(a1, a2);
	case SYS_RT_SIGRETURN:
		return sys_rt_sigreturn();
	case SYS_SIGALTSTACK:
		return sys_sigaltstack(a1, a2);
	case SYS_TKILL:
		return sys_tkill(a1, a2);
	case SYS_TGKILL:
		return sys_tgkill(a1, a2, a3);
	case SYS_ALARM:
		return sys_alarm(a1);
	case SYS_SETITIMER:
		return sys_setitimer(a1, a2, a3);
	case SYS_GETITIMER:
		return sys_getitimer(a1, a2);
	case SYS_TIMER_CREATE:
		return sys_timer_create(a1, a2, a3);
	case SYS_TIMER_SETTIME:
		return sys_timer_settime(a1, a2, a3, a4);
	case SYS_TIMER_GETTIME:
		return sys_timer_gettime(a1, a2);
	case SYS_TIMER_GETOVERRUN:
		return sys_timer_getoverrun(a1);
	case SYS_TIMER_DELETE:
		return sys_timer_delete(a1);
	case SYS_SIGNALFD:
		return sys_signalfd(a1, a2, a3);
	case SYS_PAUSE:
		return sys_pause();
	case SYS_NANOSLEEP:
		return sys_nanosleep(a1, a2);
	case SYS_CLOCK_GETTIME:
		return sys_clock_gettime(a1, a2);
	case SYS_CLOCK_GETRES:
		return sys_clock_getres(a1, a2);

	// SMP/Threading syscalls
	case SYS_CLONE:
		return sys_clone(a1, a2, a3, a4, a5);
	case SYS_VFORK:
		return sys_vfork();
	case SYS_EXIT_GROUP:
		sys_exit_group(a1);
		return 0; // Never reached
	case SYS_GETTID:
		return sys_gettid();
	case SYS_SET_TID_ADDRESS:
		return sys_set_tid_address(a1);
	case SYS_FUTEX:
		return sys_futex(a1, a2, a3, a4, a5, 0);
	case SYS_SET_ROBUST_LIST:
		return sys_set_robust_list(a1, a2);
	case SYS_GET_ROBUST_LIST:
		return sys_get_robust_list(a1, a2, a3);
	case SYS_ARCH_PRCTL:
		return sys_arch_prctl(a1, a2);
	case SYS_SCHED_SETAFFINITY:
		return sys_sched_setaffinity(a1, a2, a3);
	case SYS_SCHED_GETAFFINITY:
		return sys_sched_getaffinity(a1, a2, a3);
	case SYS_SCHED_SETSCHEDULER:
		return sys_sched_setscheduler(a1, a2, a3);
	case SYS_SCHED_GETSCHEDULER:
		return sys_sched_getscheduler(a1);
	case SYS_SCHED_SETPARAM:
		return sys_sched_setparam(a1, a2);
	case SYS_SCHED_GETPARAM:
		return sys_sched_getparam(a1, a2);
	case SYS_SCHED_GET_PRIORITY_MAX:
		return sys_sched_get_priority_max(a1);
	case SYS_SCHED_GET_PRIORITY_MIN:
		return sys_sched_get_priority_min(a1);
	case SYS_SCHED_RR_GET_INTERVAL:
		return sys_sched_rr_get_interval(a1, a2);
	case SYS_MPROTECT:
		return sys_mprotect(a1, a2, a3);

	case SYS_MADVISE:
		return sys_madvise(a1, a2, a3);

	case SYS_REBOOT:
		return sys_reboot(a1, a2, a3, a4);

	case SYS_GETPROCINFO:
		return sys_getprocinfo(a1, a2);

	case SYS_GETPROCMAPS:
		return sys_getprocmaps(a1, a2, a3, a4);

	case SYS_MEMSTATS: {
		if (!a1)
			return -EFAULT;
		memory_stats_t stats;
		mm_get_memory_stats(&stats);
		if (copy_to_user((void *)a1, &stats, sizeof(stats)) != 0)
			return -EFAULT;
		return 0;
	}

	case SYS_SYSINFO:
		return sys_sysinfo(a1);

	case SYS_KLOGCTL:
		return sys_klogctl(a1, a2, a3);

	case SYS_SYNC:
		return sys_sync();

	// ====== Socket syscalls ======
	case SYS_SOCKET: {
		/* Both descriptor flags are honoured here.
		 *
		 * SOCK_CLOEXEC was masked off the type and then forgotten, so
		 * a socket asked to close on exec did not -- it was inherited
		 * by every program the process went on to run.  socketpair()
		 * and accept4() both honour it; only this one did not.
		 *
		 * The slot is claimed from 3 upward, as it always has been.
		 * fd_install() would start at 0 and hand out a freed stdio
		 * descriptor, which is correct by the letter of the standard
		 * and a behaviour change this call has never had -- not
		 * something to introduce in passing while fixing a flag. */
		int real_type = (int)a2 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
		task_t *cur = sched_current();
		int newfd;

		if (!cur)
			return -EFAULT;

		if ((int)a1 == AF_UNIX) {
			unix_socket_t *ufd = NULL;
			int rc = unix_create(real_type, &ufd);

			if (rc < 0)
				return rc;
			if ((int)a2 & SOCK_NONBLOCK) {
				ufd->nonblock = 1;
			}
			newfd = fd_install_from(cur, (vfs_file_t *)ufd, 3);
			if (newfd < 0) {
				unix_close(ufd);
				return newfd;
			}
		} else {
			int sock_idx = sock_create((int)a1, real_type, (int)a3);

			if (sock_idx < 0)
				return sock_idx;
			if ((int)a2 & SOCK_NONBLOCK) {
				net_socket_t *_s = sock_get(sock_idx);

				if (_s)
					_s->nonblock = 1;
			}
			newfd = fd_install_from(cur, MAKE_SOCKET_FD(sock_idx),
						3);
			if (newfd < 0) {
				sock_close(sock_idx);
				return newfd;
			}
		}
		if ((int)a2 & SOCK_CLOEXEC)
			task_set_fd_flags(cur, (unsigned)newfd, FD_CLOEXEC);
		return newfd;
	}

	case SYS_BIND: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			/* Validate and copy only the DECLARED length, into a
			 * zeroed structure.
			 *
			 * Requiring the full sizeof(struct sockaddr_un) to be
			 * readable rejects a caller that allocated exactly
			 * SUN_LEN bytes -- which is legal, and which the
			 * address is normally sized by.  Zeroing first means
			 * the untouched tail is deterministic rather than
			 * stack garbage. */
			socklen_t alen = (socklen_t)a3;

			if (alen < sizeof(sa_family_t))
				return -EINVAL;
			if (alen > sizeof(struct sockaddr_un))
				alen = sizeof(struct sockaddr_un);
			if (!validate_user_ptr(a2, alen))
				return -EFAULT;
			struct sockaddr_un kaddr;
			mm_memset(&kaddr, 0, sizeof(kaddr));
			if (copy_from_user(&kaddr, (const void *)a2, alen) != 0)
				return -EFAULT;
			return unix_bind(ufd, &kaddr, alen);
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		struct sockaddr_in kaddr;
		if (!validate_user_ptr(a2, sizeof(struct sockaddr_in)))
			return -EFAULT;
		copy_from_user(&kaddr, (const void *)a2,
			       sizeof(struct sockaddr_in));
		return sock_bind(idx, &kaddr);
	}

	case SYS_LISTEN: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd)
			return unix_listen(ufd, (int)a2);
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		return sock_listen(idx, (int)a2);
	}

	case SYS_ACCEPT: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			struct sockaddr_un kaddr;
			socklen_t kaddrlen = sizeof(struct sockaddr_un);
			unix_socket_t *new_ufd = NULL;
			int arc = unix_accept(ufd, &kaddr, &kaddrlen, &new_ufd);

			if (arc < 0)
				return arc;
			task_t *cur = sched_current();
			if (!cur) {
				unix_close(new_ufd);
				return -EFAULT;
			}
			/* fd_install_from, not a hand-rolled scan: claiming
			 * the slot and storing the socket must be one locked
			 * step, or two threads accepting on the same listener
			 * at the same moment are handed the same number and
			 * one of the two connections is simply lost.  From 3,
			 * as this call has always allocated. */
			int newfd =
				fd_install_from(cur, (vfs_file_t *)new_ufd, 3);

			if (newfd < 0) {
				unix_close(new_ufd);
				return newfd;
			}
			sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
			return newfd;
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		struct sockaddr_in kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_in);
		int new_sock_idx = sock_accept(idx, &kaddr, &kaddrlen);
		if (new_sock_idx < 0)
			return new_sock_idx;
		// Allocate fd for the new accepted socket
		task_t *cur = sched_current();
		if (!cur) {
			sock_close(new_sock_idx);
			return -EFAULT;
		}
		{
			int newfd = fd_install_from(
				cur, MAKE_SOCKET_FD(new_sock_idx), 3);

			if (newfd < 0) {
				sock_close(new_sock_idx);
				return newfd;
			}
			sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
			return newfd;
		}
	}

	case SYS_CONNECT: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			/* Validate and copy only the DECLARED length, into a
			 * zeroed structure.
			 *
			 * Requiring the full sizeof(struct sockaddr_un) to be
			 * readable rejects a caller that allocated exactly
			 * SUN_LEN bytes -- which is legal, and which the
			 * address is normally sized by.  Zeroing first means
			 * the untouched tail is deterministic rather than
			 * stack garbage. */
			socklen_t alen = (socklen_t)a3;

			if (alen < sizeof(sa_family_t))
				return -EINVAL;
			if (alen > sizeof(struct sockaddr_un))
				alen = sizeof(struct sockaddr_un);
			if (!validate_user_ptr(a2, alen))
				return -EFAULT;
			struct sockaddr_un kaddr;
			mm_memset(&kaddr, 0, sizeof(kaddr));
			if (copy_from_user(&kaddr, (const void *)a2, alen) != 0)
				return -EFAULT;
			return unix_connect(ufd, &kaddr, alen);
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		struct sockaddr_in kaddr;
		if (!validate_user_ptr(a2, sizeof(struct sockaddr_in)))
			return -EFAULT;
		copy_from_user(&kaddr, (const void *)a2,
			       sizeof(struct sockaddr_in));
		return sock_connect(idx, &kaddr);
	}

	case SYS_SENDTO: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			if (!validate_user_ptr(a2, a3))
				return -EFAULT;
			return unix_send(ufd, (const void *)a2, (size_t)a3,
					 (int)a4);
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		struct sockaddr_in kaddr;
		const struct sockaddr_in *dest = NULL;
		if (a5 && validate_user_ptr(a5, sizeof(struct sockaddr_in))) {
			copy_from_user(&kaddr, (const void *)a5,
				       sizeof(struct sockaddr_in));
			dest = &kaddr;
		}
		return sock_sendto(idx, (const void *)a2, (size_t)a3, (int)a4,
				   dest, dest ? sizeof(struct sockaddr_in) : 0);
	}

	case SYS_RECVFROM: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			if (!validate_user_ptr(a2, a3))
				return -EFAULT;
			return unix_recv(ufd, (void *)a2, (size_t)a3, (int)a4);
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		struct sockaddr_in kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_in);
		int ret = sock_recvfrom(idx, (void *)a2, (size_t)a3, (int)a4,
					&kaddr, &kaddrlen);
		if (ret >= 0 && a5 &&
		    validate_user_ptr(a5, sizeof(struct sockaddr_in)))
			copy_to_user((void *)a5, &kaddr,
				     sizeof(struct sockaddr_in));
		return ret;
	}

	case SYS_SEND: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			if (!validate_user_ptr(a2, a3))
				return -EFAULT;
			return unix_send(ufd, (const void *)a2, (size_t)a3,
					 (int)a4);
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		return sock_send(idx, (const void *)a2, (size_t)a3, (int)a4);
	}

	case SYS_RECV: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			if (!validate_user_ptr(a2, a3))
				return -EFAULT;
			return unix_recv(ufd, (void *)a2, (size_t)a3, (int)a4);
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		if (!validate_user_ptr(a2, a3))
			return -EFAULT;
		return sock_recv(idx, (void *)a2, (size_t)a3, (int)a4);
	}

	case SYS_SHUTDOWN: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd)
			return unix_shutdown(ufd, (int)a2);
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		return sock_shutdown(idx, (int)a2);
	}

	case SYS_SETSOCKOPT: {
		/* AF_UNIX first, as in every other socket arm.  Going straight
		 * to sock_idx_from_fd() answered -ENOTSOCK for every option on
		 * every local socket, and a caller cannot read that as "option
		 * unsupported" -- it says the descriptor is not a socket, so
		 * the sensible reaction is to abandon it.  PCManFM sets
		 * SO_REUSEADDR on its single-instance socket and bails out of
		 * the same expression as its bind(), which made it exit with
		 * status 1 and no message instead of opening a window. */
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		int idx = 0;

		if (!ufd) {
			idx = sock_idx_from_fd(a1);
			if (idx < 0)
				return idx;
		}
		socklen_t koptlen = (socklen_t)a5;
		uint8_t koptbuf[256] = { 0 };
		if (koptlen > 0) {
			size_t copy_len = koptlen;
			if (!a4)
				return -EFAULT;
			if (!validate_user_ptr(a4, copy_len))
				return -EFAULT;
			if (copy_len > sizeof(koptbuf))
				copy_len = sizeof(koptbuf);
			int copy_rc = copy_from_user(koptbuf, (const void *)a4,
						     copy_len);
			if (copy_rc < 0)
				return copy_rc;
		}
		if (ufd)
			return unix_setsockopt(ufd, (int)a2, (int)a3,
					       koptlen > 0 ?
						       (const void *)koptbuf :
						       NULL,
					       koptlen);
		return sock_setsockopt(
			idx, (int)a2, (int)a3,
			koptlen > 0 ? (const void *)koptbuf : NULL, koptlen);
	}

	case SYS_GETSOCKOPT: {
		/* AF_UNIX first — see SYS_SETSOCKOPT above. */
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		int idx = 0;

		if (!ufd) {
			idx = sock_idx_from_fd(a1);
			if (idx < 0)
				return idx;
		}
		socklen_t koptlen = 0;
		uint8_t koptbuf[256] = { 0 };
		if (a5 && validate_user_ptr(a5, sizeof(socklen_t)))
			copy_from_user(&koptlen, (const void *)a5,
				       sizeof(socklen_t));
		if (koptlen > 0) {
			if (!a4)
				return -EFAULT;
			if (!validate_user_ptr(a4, koptlen))
				return -EFAULT;
			if (koptlen > sizeof(koptbuf))
				koptlen = sizeof(koptbuf);
		}
		int ret = ufd ? unix_getsockopt(ufd, (int)a2, (int)a3,
						koptlen > 0 ? (void *)koptbuf :
							      NULL,
						&koptlen) :
				sock_getsockopt(idx, (int)a2, (int)a3,
						koptlen > 0 ? (void *)koptbuf :
							      NULL,
						&koptlen);
		if (ret == 0 && a4 && koptlen > 0)
			copy_to_user((void *)a4, koptbuf, koptlen);
		if (ret == 0 && a5 && validate_user_ptr(a5, sizeof(socklen_t)))
			copy_to_user((void *)a5, &koptlen, sizeof(socklen_t));
		return ret;
	}

	case SYS_GETPEERNAME: {
		/* AF_UNIX first: sock_idx_from_fd() only knows AF_INET, so a
		 * Unix socket used to fail here -- and an X client that cannot
		 * name its own socket cannot choose an authorisation record,
		 * so it sends none and the server rejects it with
		 * "Authorization required, but no authorization protocol
		 * specified".  Neither message mentions getpeername(). */
		unix_socket_t *ufd = unix_sock_from_fd(a1);

		if (ufd) {
			struct sockaddr_un ukaddr;
			socklen_t ulen = sizeof(struct sockaddr_un);
			int uret;

			if (a3 && validate_user_ptr(a3, sizeof(socklen_t)))
				copy_from_user(&ulen, (const void *)a3,
					       sizeof(socklen_t));
			uret = unix_getname(ufd, 1, &ukaddr, &ulen);
			if (uret < 0)
				return uret;
			/* Copy only as much as the caller's buffer holds, but
			 * report the length the address really needs -- that
			 * is how a caller learns to retry with a bigger one. */
			{
				socklen_t cap = ulen;

				if (a3 &&
				    validate_user_ptr(a3, sizeof(socklen_t))) {
					socklen_t given = 0;

					copy_from_user(&given, (const void *)a3,
						       sizeof(socklen_t));
					if (given < cap)
						cap = given;
				}
				if (a2 && cap > 0 && validate_user_ptr(a2, cap))
					copy_to_user((void *)a2, &ukaddr, cap);
				if (a3 &&
				    validate_user_ptr(a3, sizeof(socklen_t)))
					copy_to_user((void *)a3, &ulen,
						     sizeof(socklen_t));
			}
			return 0;
		}

		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		struct sockaddr_in kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_in);
		int ret = sock_getpeername(idx, &kaddr, &kaddrlen);
		if (ret == 0 && a2 &&
		    validate_user_ptr(a2, sizeof(struct sockaddr_in)))
			copy_to_user((void *)a2, &kaddr,
				     sizeof(struct sockaddr_in));
		if (ret == 0 && a3 && validate_user_ptr(a3, sizeof(socklen_t)))
			copy_to_user((void *)a3, &kaddrlen, sizeof(socklen_t));
		return ret;
	}

	case SYS_GETSOCKNAME: {
		/* AF_UNIX first: sock_idx_from_fd() only knows AF_INET, so a
		 * Unix socket used to fail here -- and an X client that cannot
		 * name its own socket cannot choose an authorisation record,
		 * so it sends none and the server rejects it with
		 * "Authorization required, but no authorization protocol
		 * specified".  Neither message mentions getsockname(). */
		unix_socket_t *ufd = unix_sock_from_fd(a1);

		if (ufd) {
			struct sockaddr_un ukaddr;
			socklen_t ulen = sizeof(struct sockaddr_un);
			int uret;

			if (a3 && validate_user_ptr(a3, sizeof(socklen_t)))
				copy_from_user(&ulen, (const void *)a3,
					       sizeof(socklen_t));
			uret = unix_getname(ufd, 0, &ukaddr, &ulen);
			if (uret < 0)
				return uret;
			/* Copy only as much as the caller's buffer holds, but
			 * report the length the address really needs -- that
			 * is how a caller learns to retry with a bigger one. */
			{
				socklen_t cap = ulen;

				if (a3 &&
				    validate_user_ptr(a3, sizeof(socklen_t))) {
					socklen_t given = 0;

					copy_from_user(&given, (const void *)a3,
						       sizeof(socklen_t));
					if (given < cap)
						cap = given;
				}
				if (a2 && cap > 0 && validate_user_ptr(a2, cap))
					copy_to_user((void *)a2, &ukaddr, cap);
				if (a3 &&
				    validate_user_ptr(a3, sizeof(socklen_t)))
					copy_to_user((void *)a3, &ulen,
						     sizeof(socklen_t));
			}
			return 0;
		}

		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		struct sockaddr_in kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_in);
		int ret = sock_getsockname(idx, &kaddr, &kaddrlen);
		if (ret == 0 && a2 &&
		    validate_user_ptr(a2, sizeof(struct sockaddr_in)))
			copy_to_user((void *)a2, &kaddr,
				     sizeof(struct sockaddr_in));
		if (ret == 0 && a3 && validate_user_ptr(a3, sizeof(socklen_t)))
			copy_to_user((void *)a3, &kaddrlen, sizeof(socklen_t));
		return ret;
	}

	case SYS_SOCKETPAIR: {
		if (!validate_user_ptr(a4, 2 * sizeof(int)))
			return -EFAULT;
		if ((int)a1 == AF_UNIX) {
			int real_type =
				(int)a2 & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
			unix_socket_t *usv[2];
			int ret = unix_socketpair(real_type, usv);
			if (ret < 0)
				return ret;
			task_t *cur = sched_current();
			if (!cur) {
				unix_close(usv[0]);
				unix_close(usv[1]);
				return -EFAULT;
			}
			/* Install through fd_install: it claims each slot and
			 * clears the slot's stale FD_CLOEXEC.  The old
			 * hand-rolled scan did neither, so a pair could inherit
			 * close-on-exec from whatever previously used those
			 * slots and vanish across the next exec. */
			int pfd[2];
			pfd[0] = fd_install(cur, (vfs_file_t *)usv[0]);
			if (pfd[0] < 0) {
				unix_close(usv[0]);
				unix_close(usv[1]);
				return pfd[0];
			}
			pfd[1] = fd_install(cur, (vfs_file_t *)usv[1]);
			if (pfd[1] < 0) {
				task_fds(cur)[pfd[0]] = NULL;
				unix_close(usv[0]);
				unix_close(usv[1]);
				return pfd[1];
			}
			if ((int)a2 & SOCK_CLOEXEC) {
				task_set_fd_flags(cur, (unsigned)pfd[0],
						  FD_CLOEXEC);
				task_set_fd_flags(cur, (unsigned)pfd[1],
						  FD_CLOEXEC);
			}
			if ((int)a2 & SOCK_NONBLOCK) {
				usv[0]->nonblock = 1;
				usv[1]->nonblock = 1;
			}
			copy_to_user((void *)a4, pfd, 2 * sizeof(int));
			return 0;
		}
		int sv[2];
		int ret = sock_socketpair((int)a1, (int)a2, (int)a3, sv);
		if (ret < 0)
			return ret;
		// Allocate two process fds
		task_t *cur = sched_current();
		if (!cur) {
			sock_close(sv[0]);
			sock_close(sv[1]);
			return -EFAULT;
		}
		/* Same as the AF_UNIX path above: install via fd_install so the
		 * slots are claimed atomically and their stale FD_CLOEXEC is
		 * cleared. */
		int ufd[2];
		ufd[0] = fd_install(cur, MAKE_SOCKET_FD(sv[0]));
		if (ufd[0] < 0) {
			sock_close(sv[0]);
			sock_close(sv[1]);
			return ufd[0];
		}
		ufd[1] = fd_install(cur, MAKE_SOCKET_FD(sv[1]));
		if (ufd[1] < 0) {
			task_fds(cur)[ufd[0]] = NULL;
			sock_close(sv[0]);
			sock_close(sv[1]);
			return ufd[1];
		}
		if ((int)a2 & SOCK_CLOEXEC) {
			task_set_fd_flags(cur, (unsigned)ufd[0], FD_CLOEXEC);
			task_set_fd_flags(cur, (unsigned)ufd[1], FD_CLOEXEC);
		}
		copy_to_user((void *)a4, ufd, 2 * sizeof(int));
		return 0;
	}

	case SYS_ACCEPT4: {
		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			struct sockaddr_un kaddr;
			socklen_t kaddrlen = sizeof(struct sockaddr_un);
			unix_socket_t *new_ufd = NULL;
			int arc = unix_accept(ufd, &kaddr, &kaddrlen, &new_ufd);

			if (arc < 0)
				return arc;
			task_t *cur = sched_current();
			if (!cur) {
				unix_close(new_ufd);
				return -EFAULT;
			}
			if ((int)a4 & SOCK_NONBLOCK)
				new_ufd->nonblock = 1;
			/* One locked step, and both flags honoured.
			 * SOCK_CLOEXEC was accepted and then ignored here, so
			 * a connection asked to close on exec was inherited by
			 * every program the process went on to run. */
			int newfd =
				fd_install_from(cur, (vfs_file_t *)new_ufd, 3);

			if (newfd < 0) {
				unix_close(new_ufd);
				return newfd;
			}
			if ((int)a4 & SOCK_CLOEXEC)
				task_set_fd_flags(cur, (unsigned)newfd,
						  FD_CLOEXEC);
			sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
			return newfd;
		}
		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		struct sockaddr_in kaddr;
		socklen_t kaddrlen = sizeof(struct sockaddr_in);
		int new_sock_idx =
			sock_accept4(idx, &kaddr, &kaddrlen, (int)a4);
		if (new_sock_idx < 0)
			return new_sock_idx;
		task_t *cur = sched_current();
		if (!cur) {
			sock_close(new_sock_idx);
			return -EFAULT;
		}
		{
			int newfd = fd_install_from(
				cur, MAKE_SOCKET_FD(new_sock_idx), 3);

			if (newfd < 0) {
				sock_close(new_sock_idx);
				return newfd;
			}
			if ((int)a4 & SOCK_CLOEXEC)
				task_set_fd_flags(cur, (unsigned)newfd,
						  FD_CLOEXEC);
			sock_put_peer_addr(a2, a3, &kaddr, kaddrlen);
			return newfd;
		}
	}

	case SYS_SENDMSG: {
		if (!validate_user_ptr(a2, sizeof(struct msghdr)))
			return -EFAULT;
		struct msghdr kmsg;
		copy_from_user(&kmsg, (const void *)a2, sizeof(struct msghdr));

		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd)
			return unix_do_sendmsg(ufd, &kmsg);

		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		return sock_sendmsg(idx, &kmsg, (int)a3);
	}

	case SYS_RECVMSG: {
		if (!validate_user_ptr(a2, sizeof(struct msghdr)))
			return -EFAULT;
		struct msghdr kmsg;
		copy_from_user(&kmsg, (const void *)a2, sizeof(struct msghdr));

		unix_socket_t *ufd = unix_sock_from_fd(a1);
		if (ufd) {
			int ret = unix_do_recvmsg(ufd, &kmsg);
			if (ret >= 0)
				copy_to_user((void *)a2, &kmsg,
					     sizeof(struct msghdr));
			return ret;
		}

		int idx = sock_idx_from_fd(a1);
		if (idx < 0)
			return idx;
		int ret = sock_recvmsg(idx, &kmsg, (int)a3);
		if (ret >= 0)
			copy_to_user((void *)a2, &kmsg, sizeof(struct msghdr));
		return ret;
	}

	case SYS_SENDFILE: {
		int64_t koffset = 0;
		int64_t *koffp = NULL;
		if (a3) {
			if (!validate_user_ptr(a3, sizeof(int64_t)))
				return -EFAULT;
			copy_from_user(&koffset, (void *)a3, sizeof(int64_t));
			koffp = &koffset;
		}
		int ret = sock_sendfile((int)a1, (int)a2, koffp, (size_t)a4);
		if (a3 && ret >= 0) {
			copy_to_user((void *)a3, &koffset, sizeof(int64_t));
		}
		return ret;
	}

	case SYS_SELECT:
		return sys_select_wrapper(a1, a2, a3, a4, a5);

	case SYS_PSELECT6:
		return sys_pselect6_wrapper(a1, a2, a3, a4, a5, a6);

	case SYS_POLL:
		return sys_poll_wrapper(a1, a2, a3);

	case SYS_PPOLL:
		return sys_ppoll_wrapper(a1, a2, a3, a4);

	case SYS_EPOLL_CREATE: {
		int ep_idx = epoll_create_internal(0);
		if (ep_idx < 0)
			return ep_idx;
		task_t *cur = sched_current();
		if (!cur)
			return -EFAULT;
		for (int _fd = 3; _fd < TASK_MAX_FDS; _fd++) {
			if (task_fds(cur)[_fd] == NULL) {
				task_fds(cur)[_fd] = MAKE_EPOLL_FD(ep_idx);
				return _fd;
			}
		}
		return -EMFILE;
	}

	case SYS_EPOLL_CREATE1: {
		int ep_idx = epoll_create_internal((int)a1);
		if (ep_idx < 0)
			return ep_idx;
		task_t *cur = sched_current();
		if (!cur)
			return -EFAULT;
		for (int _fd = 3; _fd < TASK_MAX_FDS; _fd++) {
			if (task_fds(cur)[_fd] == NULL) {
				task_fds(cur)[_fd] = MAKE_EPOLL_FD(ep_idx);
				/* EPOLL_CLOEXEC has to be RECORDED, not just
				 * accepted.  An epoll set is private to the
				 * process that built it, and every caller asks
				 * for it -- letting the descriptor survive
				 * exec() hands an unrelated program a handle
				 * onto it, and the reference it drops on exit
				 * is one the creator was still using. */
				if ((int)a1 & EPOLL_CLOEXEC)
					task_set_fd_flags(cur, (unsigned)_fd,
							  FD_CLOEXEC);
				return _fd;
			}
		}
		return -EMFILE;
	}

	case SYS_EPOLL_CTL: {
		int ep_idx = epoll_idx_from_fd(a1);
		if (ep_idx < 0)
			return ep_idx;
		struct epoll_event kev;
		if (a4 && validate_user_ptr(a4, sizeof(struct epoll_event)))
			copy_from_user(&kev, (void *)a4,
				       sizeof(struct epoll_event));
		return epoll_ctl_internal(ep_idx, (int)a2, (int)a3,
					  a4 ? &kev : NULL);
	}

	case SYS_EPOLL_WAIT:
		return sys_epoll_wait_wrapper(a1, a2, a3, a4);

	case SYS_EPOLL_PWAIT:
		return sys_epoll_wait_wrapper(a1, a2, a3, a4);

	case SYS_DUP3:
		return sys_dup3(a1, a2, a3);

	case SYS_DNS_RESOLVE: {
		if (!validate_user_ptr(a1, 1))
			return -EFAULT;
		if (!validate_user_ptr(a2, sizeof(uint32_t)))
			return -EFAULT;
		// Copy hostname from user space (max 255 chars).
		// Copy page-by-page to avoid faulting across page boundaries.
		char khost[256];
		size_t off = 0;
		khost[0] = '\0';
		smap_disable();
		while (off < 255) {
			uintptr_t addr = a1 + off;
			// Check that the page containing this byte is mapped
			if (!mm_is_page_mapped(addr))
				break;
			// Copy up to end of this page (or remaining buffer)
			size_t page_end = (addr | 0xFFF) + 1;
			size_t chunk = page_end - addr;
			if (off + chunk > 255)
				chunk = 255 - off;
			for (size_t j = 0; j < chunk; j++) {
				khost[off] = ((const char *)a1)[off];
				if (khost[off] == '\0')
					goto dns_str_done;
				off++;
			}
		}
dns_str_done:
		smap_enable();
		khost[off] = '\0';
		if (off == 0)
			return -EFAULT;
		uint32_t ip = 0;
		int ret = dns_resolve(khost, &ip);
		if (ret == 0) {
			copy_to_user((void *)a2, &ip, sizeof(uint32_t));
		}
		return ret;
	}

	case SYS_SETHOSTNAME: {
		/* Setting the hostname is a system-wide change: privileged only. */
		if (!capable())
			return -EPERM;
		if (!validate_user_ptr(a1, 1))
			return -EFAULT;
		size_t len = (size_t)a2;
		if (len == 0 || len > 63)
			return -EINVAL;
		char khost[64];
		if (copy_from_user(khost, (const void *)a1, len) < 0)
			return -EFAULT;
		khost[len] = '\0';
		net_set_hostname(khost);
		return 0;
	}

	case SYS_NET_GETINFO: {
		int subcmd = (int)a1;
		if (!validate_user_ptr(a2, 1))
			return -EFAULT;
		int max_entries = (int)a3;
		if (max_entries <= 0)
			return -EINVAL;

		switch (subcmd) {
		case NET_GET_ARP_TABLE: {
			size_t sz =
				(size_t)max_entries * sizeof(net_arp_info_t);
			if (!validate_user_ptr(a2, sz))
				return -EFAULT;
			net_arp_info_t kbuf[64];
			int n = max_entries > 64 ? 64 : max_entries;
			int count = net_get_arp_table(kbuf, n);
			copy_to_user((void *)a2, kbuf,
				     (size_t)count * sizeof(net_arp_info_t));
			return count;
		}
		case NET_GET_ROUTE_TABLE: {
			size_t sz =
				(size_t)max_entries * sizeof(net_route_info_t);
			if (!validate_user_ptr(a2, sz))
				return -EFAULT;
			net_route_info_t kbuf[32];
			int n = max_entries > 32 ? 32 : max_entries;
			int count = net_get_route_table(kbuf, n);
			copy_to_user((void *)a2, kbuf,
				     (size_t)count * sizeof(net_route_info_t));
			return count;
		}
		case NET_GET_TCP_CONNECTIONS: {
			size_t sz =
				(size_t)max_entries * sizeof(net_tcp_info_t);
			if (!validate_user_ptr(a2, sz))
				return -EFAULT;
			net_tcp_info_t kbuf[64];
			int n = max_entries > 64 ? 64 : max_entries;
			int count = net_get_tcp_connections(kbuf, n);
			copy_to_user((void *)a2, kbuf,
				     (size_t)count * sizeof(net_tcp_info_t));
			return count;
		}
		case NET_GET_UDP_SOCKETS: {
			size_t sz =
				(size_t)max_entries * sizeof(net_udp_info_t);
			if (!validate_user_ptr(a2, sz))
				return -EFAULT;
			net_udp_info_t kbuf[64];
			int n = max_entries > 64 ? 64 : max_entries;
			int count = net_get_udp_sockets(kbuf, n);
			copy_to_user((void *)a2, kbuf,
				     (size_t)count * sizeof(net_udp_info_t));
			return count;
		}
		case NET_GET_IFACE_STATS: {
			size_t sz =
				(size_t)max_entries * sizeof(net_iface_info_t);
			if (!validate_user_ptr(a2, sz))
				return -EFAULT;
			net_iface_info_t kbuf[8];
			int n = max_entries > 8 ? 8 : max_entries;
			int count = net_get_iface_info(kbuf, n);
			copy_to_user((void *)a2, kbuf,
				     (size_t)count * sizeof(net_iface_info_t));
			return count;
		}
		case NET_GET_NETSTATS: {
			if (!validate_user_ptr(a2, sizeof(net_stats_info_t)))
				return -EFAULT;
			net_stats_info_t kbuf;
			if (net_get_stats(&kbuf) != 0)
				return -EINVAL;
			copy_to_user((void *)a2, &kbuf,
				     sizeof(net_stats_info_t));
			return 1;
		}
		case NET_DNS_QUERY: {
			if (!validate_user_ptr(a2, sizeof(dns_query_buf_t)))
				return -EFAULT;
			dns_query_buf_t kbuf;
			copy_from_user(&kbuf, (void *)a2,
				       sizeof(dns_query_buf_t));
			kbuf.name[255] = '\0';
			int rlen = dns_query_raw(kbuf.name, kbuf.qtype,
						 kbuf.response, 512);
			kbuf.response_len = rlen;
			copy_to_user((void *)a2, &kbuf,
				     sizeof(dns_query_buf_t));
			return rlen > 0 ? 0 : rlen;
		}
		default:
			return -EINVAL;
		}
	}

	case SYS_DHCP_CONTROL: {
		int subcmd = (int)a1;
		net_device_t *dev = net_get_default_device();
		if (!dev)
			return -ENETDOWN;
		/* DISCOVER/RELEASE/RENEW reconfigure the interface address and
		 * are privileged; STATUS is read-only and open to all. */
		if (subcmd != DHCP_CMD_STATUS && !capable())
			return -EPERM;
		switch (subcmd) {
		case DHCP_CMD_DISCOVER:
			return dhcp_discover(dev);
		case DHCP_CMD_RELEASE:
			return dhcp_release(dev);
		case DHCP_CMD_RENEW:
			return dhcp_renew(dev);
		case DHCP_CMD_STATUS:
			return dhcp_get_status();
		default:
			return -EINVAL;
		}
	}

	case SYS_RAW_SEND: {
		// a1 = subcmd (1=ICMP echo, 2=ARP request)
		// a2 = dst_ip, a3 = id/seq packed, a4 = ttl, a5 = data_ptr (optional)
		int subcmd = (int)a1;
		net_device_t *dev = net_get_default_device();
		if (!dev)
			return -ENETDOWN;
		if (subcmd == 1) {
			// ICMP echo: a2=dst_ip, a3=id<<16|seq, a4=ttl
			uint32_t dst_ip = (uint32_t)a2;
			uint16_t id = (uint16_t)(a3 >> 16);
			uint16_t seq = (uint16_t)(a3 & 0xFFFF);
			uint8_t ttl = (uint8_t)a4;
			if (ttl == 0)
				ttl = 64;
			// 56 bytes of padding data
			uint8_t pad[56];
			for (int pi = 0; pi < 56; pi++)
				pad[pi] = (uint8_t)pi;
			int send_ret = icmp_send_echo(dev, dst_ip, id, seq, pad,
						      56, ttl);
			loopback_process_pending();
			return send_ret;
		} else if (subcmd == 2) {
			// ARP request: a2=target_ip
			uint32_t target_ip = (uint32_t)a2;
			return arp_send_request(dev, target_ip);
		}
		return -EINVAL;
	}

	case SYS_RAW_RECV: {
		// a1 = subcmd (1=ICMP reply, 2=ARP reply)
		// a2 = ptr to result struct, a3 = expected_id or target_ip,
		// a4 = timeout in MILLISECONDS (0 = default).
		//
		// Milliseconds, not ticks: the tick rate is measured at boot
		// and is not a number userspace can know, yet this used to take
		// a tick count -- so ping, arping and traceroute each did their
		// own `seconds * 100' and every one of them waited for the
		// wrong length of time on a machine whose rate was not 100Hz.
		int subcmd = (int)a1;
		if (subcmd == 1) {
			// ICMP reply
			if (!validate_user_ptr(a2, 24))
				return -EFAULT;
			uint32_t src_ip = 0;
			uint8_t type = 0, code = 0, recv_ttl = 0;
			uint16_t seq = 0;
			uint16_t expected_id = (uint16_t)(a3 >> 16);
			uint16_t expected_seq = (uint16_t)(a3 & 0xFFFF);
			uint64_t timeout = timer_ms_to_ticks(a4 ? a4 : 5000);
			uint64_t rtt_us = 0;
			int ret = icmp_recv_reply(&src_ip, expected_id, &type,
						  &code, &seq, timeout, &rtt_us,
						  expected_seq, &recv_ttl);
			if (ret == 0) {
				// Pack result: [src_ip(4), type(1), code(1), seq(2), rtt_us(8), ttl(1), pad(7)]
				uint8_t result[24];
				for (int i = 0; i < 24; i++)
					result[i] = 0;
				result[0] = (src_ip >> 24) & 0xFF;
				result[1] = (src_ip >> 16) & 0xFF;
				result[2] = (src_ip >> 8) & 0xFF;
				result[3] = src_ip & 0xFF;
				result[4] = type;
				result[5] = code;
				result[6] = (seq >> 8) & 0xFF;
				result[7] = seq & 0xFF;
				// Pack RTT in microseconds (little-endian uint64_t)
				for (int i = 0; i < 8; i++)
					result[8 + i] =
						(rtt_us >> (i * 8)) & 0xFF;
				result[16] = recv_ttl;
				copy_to_user((void *)a2, result, 24);
			}
			return ret;
		} else if (subcmd == 2) {
			// ARP reply
			if (!validate_user_ptr(a2, 6))
				return -EFAULT;
			uint32_t target_ip = (uint32_t)a3;
			uint64_t timeout = timer_ms_to_ticks(a4 ? a4 : 5000);
			uint8_t mac[6];
			int ret = arp_recv_reply(target_ip, mac, timeout);
			if (ret == 0) {
				copy_to_user((void *)a2, mac, 6);
			}
			return ret;
		}
		return -EINVAL;
	}

	case SYS_DNS_RESOLVE_REVERSE: {
		// a1 = IP address in network byte order
		// a2 = pointer to output hostname buffer (user)
		// a3 = max length of output buffer
		uint32_t ip_nbo = (uint32_t)a1;
		int maxlen = (int)a3;
		if (maxlen <= 0 || maxlen > 256)
			return -EINVAL;
		if (!validate_user_ptr(a2, (size_t)maxlen))
			return -EFAULT;

		char kbuf[256];
		int ret = dns_resolve_reverse(ip_nbo, kbuf, sizeof(kbuf));
		if (ret == 0) {
			// Copy result to user space
			size_t slen = 0;
			while (kbuf[slen])
				slen++;
			if ((int)(slen + 1) > maxlen)
				return -ENAMETOOLONG;
			copy_to_user((void *)a2, kbuf, slen + 1);
		}
		return ret;
	}

	case SYS_SET_DNS_SERVER: {
		// a1 = ifname (user, NUL-terminated, may be NULL/empty for "all")
		// a2 = IPv4 address in network byte order (0 to clear)
		// RFC 3493: install resolver server.  Used by the userland
		// /etc/resolv.conf parser at boot before DHCP completes, and
		// by `dhclient` for manual overrides.
		uint32_t ip_nbo = (uint32_t)a2;
		char ifname[16] = { 0 };
		int have_name = 0;
		if (a1) {
			if (!validate_user_ptr(a1, 1))
				return -EFAULT;
			copy_from_user(ifname, (void *)a1, sizeof(ifname) - 1);
			ifname[sizeof(ifname) - 1] = 0;
			if (ifname[0])
				have_name = 1;
		}
		int updated = 0;
		for (int i = 0; i < 16; i++) {
			net_device_t *d = net_get_device(i);
			if (!d)
				continue;
			if (have_name) {
				int match = 1;
				for (int k = 0; k < 16; k++) {
					if (d->name[k] != ifname[k]) {
						match = 0;
						break;
					}
					if (!ifname[k])
						break;
				}
				if (!match)
					continue;
			}
			d->dns_server = ip_nbo;
			updated++;
		}
		// Loopback too, so test_libc on loopback still has a resolver.
		net_device_t *lo = net_get_loopback();
		if (lo &&
		    (!have_name || (ifname[0] == 'l' && ifname[1] == 'o' &&
				    ifname[2] == 0))) {
			lo->dns_server = ip_nbo;
			updated++;
		}
		return updated > 0 ? 0 : -ENODEV;
	}

	case SYS_SETSID:
		return sys_setsid();
	case SYS_GETSID:
		return sys_getsid(a1);
	case SYS_GETPGID:
		return sys_getpgid(a1);
	case SYS_GETRUSAGE:
		return sys_getrusage(a1, a2);
	case SYS_READV:
		return sys_readv(a1, a2, a3);
	case SYS_WRITEV:
		return sys_writev(a1, a2, a3);

	case SYS_GETRANDOM: {
		void *buf = (void *)a1;
		size_t buflen = (size_t)a2;
		/* flags: GRND_NONBLOCK=0x1, GRND_RANDOM=0x2, GRND_INSECURE=0x4 */
		unsigned flags = (unsigned)a3;
		if (buflen == 0)
			return 0;
		if (!validate_user_ptr((uint64_t)buf, buflen))
			return -EFAULT;
		/* GRND_RANDOM(0x2) = blocking pool; default = urandom (non-blocking) */
		int blocking = (flags & 0x2) ? 1 : 0;
		/* Use a kernel-side staging buffer to keep SMAP integrity */
		uint8_t *kbuf = (uint8_t *)kalloc(buflen);
		if (!kbuf)
			return -ENOMEM;
		int n = random_get_bytes(kbuf, buflen, blocking);
		if (n < 0) {
			kfree(kbuf);
			return (blocking == 0) ? -EAGAIN : -EIO;
		}
		int cret = copy_to_user(buf, kbuf, (size_t)n);
		kfree(kbuf);
		if (cret < 0)
			return cret;
		return (int64_t)n;
	}

	default:
		return -ENOSYS;
	}
}

// Wrapper that handles signal delivery after syscall
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
			uint64_t a4, uint64_t a5, uint64_t a6)
{
	// CRITICAL: Interrupts are DISABLED when we enter (syscall_entry no longer does sti)
	// This prevents a race where:
	// 1. Task A enters syscall, writes to per-CPU storage
	// 2. Timer fires, preempts to task B
	// 3. Task B makes syscall, overwrites per-CPU storage
	// 4. Task A resumes, reads corrupted values from per-CPU
	//
	// We snapshot the per-CPU values to task-local storage before enabling interrupts.
	task_t *cur = sched_current();
	percpu_t *cpu = this_cpu();
	BUG_ON(cpu == NULL);

	/* Track current syscall number for Oops/panic reporting */
	cpu->current_syscall_nr = (int)num;

	if (cur && cur->privilege == TASK_USER) {
		// Read from per-CPU storage (set by syscall_entry in assembly)
		cur->syscall_rsp = cpu->syscall_user_rsp;
		cur->syscall_rip = cpu->syscall_saved_user_rip;
		cur->syscall_rflags = cpu->syscall_saved_user_rflags;
		cur->syscall_rbp = cpu->syscall_saved_user_rbp;
		cur->syscall_rbx = cpu->syscall_saved_user_rbx;
		cur->syscall_r12 = cpu->syscall_saved_user_r12;
		cur->syscall_r13 = cpu->syscall_saved_user_r13;
		cur->syscall_r14 = cpu->syscall_saved_user_r14;
		cur->syscall_r15 = cpu->syscall_saved_user_r15;
	}

	// NOW enable interrupts - per-CPU values are safely copied to task struct
	__asm__ volatile("sti" ::: "memory");

	int64_t ret = syscall_handler_inner(num, a1, a2, a3, a4, a5, a6);

	/* Release the socket this syscall held, if it resolved one.  Here
	 * rather than in each arm: the arms return from many places, and a
	 * missed release permanently claims the socket. */
	if (cur && cur->syscall_unix_ref) {
		struct unix_socket *held = cur->syscall_unix_ref;

		cur->syscall_unix_ref = NULL;
		unix_sock_put_ref(held);
	}

	// Check for pending signals before returning to userspace.
	// Skip for:
	//   * SYS_EXIT       — task is already being torn down.
	//   * SYS_RT_SIGRETURN — just restored a signal-frame context; another
	//                        delivery here would clobber it.
	//   * has_exited / TASK_ZOMBIE — task has already died inside the
	//     syscall (e.g. SIGKILL from another CPU mid-syscall, or the
	//     syscall handler called sched_mark_task_exited).  Calling
	//     signal_deliver on a zombie tripped a WARN_ON at signal.c:619
	//     and the subsequent code paths there are racy on a half-torn-down
	//     task.  Just fall through; sched_schedule below will pick the
	//     next task and we'll never return to userspace.
	if (num != SYS_EXIT && num != SYS_RT_SIGRETURN) {
		cur = sched_current(); // Re-fetch in case of fork
		if (cur && cur->privilege == TASK_USER && !cur->has_exited &&
		    cur->state != TASK_ZOMBIE && signal_pending(cur)) {
			// Save syscall return value so sigreturn can restore it
			cur->syscall_rax = (uint64_t)ret;
			signal_deliver(cur);
			// Check if signal_deliver terminated the task (e.g., SIG_DFL for SIGTERM)
			if (cur->has_exited || cur->state == TASK_ZOMBIE) {
				sched_schedule();
				// Should not return here
			}
		}
	}

	/* ppoll()/pselect() leave their temporary mask installed across the
	 * delivery above so the signal the caller unblocked can actually run
	 * its handler.  Put the caller's mask back now — unless a handler was
	 * set up, in which case signal_setup_frame already took ownership and
	 * sigreturn restores it after the handler returns. */
	{
		task_t *mcur = sched_current();
		if (mcur)
			poll_sigmask_restore_pending(mcur);
	}

	/* Clear syscall tracking on return.  Re-derive the percpu pointer:
	 * the syscall may have blocked and this task may have been resumed on
	 * a DIFFERENT CPU — `cpu` from function entry would then point at the
	 * old CPU and clobber ITS current_syscall_nr mid-syscall. */
	this_cpu()->current_syscall_nr = -1;

	/* A task that DIED inside this syscall must never sysret to user mode.
	 * sched_mark_task_exited() has already closed its descriptors and
	 * released its demand-paging region table, so the moment it executes
	 * user code again the first not-yet-paged-in text page faults with no
	 * region backing it — a bogus SIGSEGV report (with an empty region
	 * list) for a process that was already dead.  That is exactly what
	 * `kill -TERM $$` produced: sys_kill → SIG_DFL_TERM on the caller
	 * itself → marked exited, and the signal-delivery block above is
	 * skipped precisely BECAUSE has_exited is set, so control fell
	 * straight through to `return ret`.  Park here instead and let the
	 * scheduler take us off this CPU for good.
	 *
	 * IRQs stay ENABLED in the retry loop (`sti; hlt`, same as sys_exit):
	 * a CPU halted with IRQs off can no longer ack TLB-shootdown IPIs and
	 * wedges every other CPU spinning in smp_tlb_shootdown_sync(). */
	cur = sched_current();
	if (cur && cur->privilege == TASK_USER &&
	    (cur->has_exited || cur->state == TASK_ZOMBIE)) {
		/* This is where the threads of an exiting group actually end
		 * up: signalling a thread never tears it down in place, so
		 * each one unwinds its own syscall and arrives here, in its
		 * own context with interrupts on -- the right place to give
		 * its address space back. */
		sched_exit_park();
	}
	return ret;
}
