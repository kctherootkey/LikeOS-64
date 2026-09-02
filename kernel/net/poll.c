// LikeOS-64 Poll / Select / Epoll Implementation
// Multiplexed I/O for sockets, pipes, and regular file descriptors

#include <kernel/net/net.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/io/tty.h>
#include <kernel/fs/devfs.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h> /* kalloc/kfree for the poll table */
#include <kernel/ke/waitq.h>
#include <kernel/fs/file.h>

// ============================================================================
// Epoll instance table
// ============================================================================
epoll_instance_t epoll_instances[MAX_EPOLL_INSTANCES];
static spinlock_t epoll_lock = SPINLOCK_INIT("epoll");

// Stable address used as a wake channel for tasks sleeping in poll/select/
// epoll_wait.  Any I/O producer (TTY key press, TCP data arrival, pipe write)
// that should unblock a multiplexed waiter calls poll_notify_io_ready(), which
// fires sched_wake_channel on this address.  The value of the variable itself
// is never read; only its address matters as the channel key.
static int g_poll_io_ready;

// Wake all tasks currently parked in poll_sleep_until_next_tick.  Called from
// every I/O path that can make a polled fd ready: TTY input, TCP receive, pipe
// write, etc.  This replaces the old tick-granularity wakeup for interactive
// workloads, giving sub-millisecond response to keyboard input.
/* Bumped by every notify.  A poller samples it before scanning its fds and
 * re-checks it after publishing its blocked state; if it moved, an event
 * arrived during the scan and the poller re-scans instead of sleeping through
 * it.  That makes the wake below best-effort rather than load-bearing: no
 * event can be lost because a producer failed to find the poller parked. */
static volatile uint64_t g_poll_io_seq;

static inline uint64_t poll_io_seq(void)
{
	return __atomic_load_n(&g_poll_io_seq, __ATOMIC_ACQUIRE);
}

/* How many tasks are parked below.  Read first by the notify so the common
 * case -- an I/O completion with nobody multiplexing -- costs one atomic load
 * instead of a scan of the whole task list under its global lock.  That path
 * is now taken by every AF_UNIX and pipe transfer, so it has to be cheap. */
static volatile int g_poll_sleepers;

/* ---------------------------------------------------------------------------
 * Registering a poller on the objects it is waiting for.
 *
 * The conventional arrangement: while a poller scans its descriptors it is
 * added to the wait queue of every object it asks about, so a readiness change
 * wakes exactly the tasks that asked about THAT object.  The alternative --
 * one global channel that every poll(), select() and epoll_wait() parks on --
 * turns each I/O event into a wakeup for every poller on the machine and a
 * walk of the whole task list with interrupts disabled.  Measured on an
 * ordinary desktop: ten wakeups per event, sixteen thousand a second, each
 * followed by a full descriptor re-scan that found nothing.  The cost grew
 * with the number of PROCESSES, so an idle window cost as much as a busy one,
 * and the interrupt-off walks showed up as a stuttering mouse pointer.
 *
 * The table is allocated once per poll() call and reused across the scan/sleep
 * iterations; each iteration unregisters before it re-registers, because the
 * set of interesting objects can change between scans (a descriptor closed and
 * reopened) and an entry may be on only one queue at a time.
 * ------------------------------------------------------------------------ */

struct poll_table {
	struct wait_queue_head **heads; /* which queue entry i sits on */
	struct wait_queue_entry *ents;
	/* The descriptor entry that OWNS heads[i], with a reference held for
	 * as long as the wait-queue entry is linked to it.
	 *
	 * The scan holds the descriptor only while it is looking at it, and
	 * drops that hold before the sleep -- but the registration outlives
	 * the scan by design, which is the whole point of registering.  With
	 * only the scan's hold, a sibling thread closing the last descriptor
	 * frees the pipe or socket while this poller is still on its queue,
	 * and the wq_remove() below then writes through a pprev that points
	 * into freed memory.  So the registration takes a reference of its
	 * own, and the object cannot go away underneath it.
	 *
	 * NULL for a queue that belongs to something with no descriptor to
	 * hold -- the console tty, which is never freed. */
	void **held;
	int n;   /* entries in use */
	int cap; /* 0 when the allocation failed -- see poll_wait() */
};

static void poll_table_init(struct poll_table *pt, int cap)
{
	pt->heads = NULL;
	pt->ents = NULL;
	pt->held = NULL;
	pt->n = 0;
	pt->cap = 0;
	if (cap <= 0)
		return;
	pt->heads = kalloc((size_t)cap * sizeof(*pt->heads));
	pt->ents = kalloc((size_t)cap * sizeof(*pt->ents));
	pt->held = kalloc((size_t)cap * sizeof(*pt->held));
	if (!pt->heads || !pt->ents || !pt->held) {
		kfree(pt->heads);
		kfree(pt->ents);
		kfree(pt->held);
		pt->heads = NULL;
		pt->ents = NULL;
		pt->held = NULL;
		return; /* cap stays 0: poll_wait becomes a no-op */
	}
	pt->cap = cap;
}

/* Take this poller off every queue it is on.  Must run before the table is
 * freed and before the caller returns, or a queue would keep a pointer into a
 * dead allocation. */
static void poll_table_reset(struct poll_table *pt)
{
	for (int i = 0; i < pt->n; i++) {
		/* Off the queue FIRST, then let go of what owns the queue.
		 * The other order drops the last reference while this entry is
		 * still linked, so the object -- and the head this is about to
		 * unlink from -- can be freed between the two lines. */
		wq_remove(pt->heads[i], &pt->ents[i]);
		fdput(pt->held[i]);
		pt->held[i] = NULL;
	}
	pt->n = 0;
}

static void poll_table_free(struct poll_table *pt)
{
	poll_table_reset(pt);
	kfree(pt->heads);
	kfree(pt->ents);
	kfree(pt->held);
	pt->heads = NULL;
	pt->ents = NULL;
	pt->held = NULL;
	pt->cap = 0;
}

/* Register the calling task on `h', which belongs to `owner'.
 *
 * `owner' is the descriptor entry the queue lives inside, and this takes a
 * reference on it that lasts until poll_table_reset() unlinks the entry
 * again -- see the note on poll_table::held.  Pass NULL only for a queue in
 * an object that is never freed.
 *
 * A no-op when there is no table (the caller only wants the readiness mask, as
 * epoll_ctl does) or when the table is full or could not be allocated.  That
 * is safe rather than merely tolerable: a poller that fails to register still
 * re-scans on the periodic wakeup below, so the worst outcome is the latency
 * this whole change is removing, not a missed event. */
void poll_wait(struct poll_table *pt, void *owner, struct wait_queue_head *h)
{
	task_t *cur;

	if (!pt || !h || pt->n >= pt->cap)
		return;
	cur = sched_current();
	if (!cur)
		return;
	/* Before the entry goes on the queue, never after: the moment it is
	 * linked, an unreferenced owner can be freed with this entry still on
	 * it.  A refused hold means the object is already gone, so there is
	 * nothing to register on. */
	if (owner && !fdhold((vfs_file_t *)owner))
		return;
	wq_entry_init(&pt->ents[pt->n], cur);
	pt->held[pt->n] = owner;
	wq_add(h, &pt->ents[pt->n]);
	pt->heads[pt->n] = h;
	pt->n++;
}

/* Report that `h' has become ready.
 *
 * The sequence counter is bumped for every notify, whether or not anything is
 * queued: a poller between its scan and its park is not yet findable on any
 * queue, and comparing this counter is how it learns to re-scan instead of
 * sleeping through the event.  The wake itself touches only this object's
 * waiters. */
void poll_notify_wq(struct wait_queue_head *h)
{
	__atomic_fetch_add(&g_poll_io_seq, 1, __ATOMIC_RELEASE);
	if (h)
		wq_wake_all(h);
}

/* The object-less form, for a readiness change with no queue to name.
 *
 * Bumping the counter is all it can do, so a poller learns about it on its
 * next scan rather than immediately.  Every caller that CAN name an object
 * should use poll_notify_wq() instead; this exists so that a path which has
 * not been converted still cannot lose an event outright. */
void poll_notify_io_ready(void)
{
	__atomic_fetch_add(&g_poll_io_seq, 1, __ATOMIC_RELEASE);
}

// Block the calling task until an I/O event fires or `deadline` is reached
// (whichever comes first).  Used by select/poll/epoll_wait between scan
// iterations to avoid busy-spinning while waiting on fds that are not yet
// ready.  Previously this slept for exactly one timer tick (up to 10 ms),
// which caused visible typing lag in programs like nc and openssl that use
// poll() to multiplex stdin and a TCP socket: keystrokes fired tty_wake_readers
// but the poll task had no wait_channel set, so it slept the full tick before
// noticing that stdin was ready.  Now the task parks on g_poll_io_ready so any
// I/O producer can wake it instantly.
static void poll_sleep_until_next_tick(uint64_t deadline_ticks,
				       int have_deadline, uint64_t seq_before)
{
	task_t *cur = sched_current();
	if (!cur) {
		__asm__ volatile("pause");
		return;
	}
	uint64_t now = timer_ticks();
	uint64_t wake = now + 1;
	if (have_deadline && deadline_ticks < wake) {
		wake = deadline_ticks;
	}
	if (wake <= now) {
		__asm__ volatile("pause");
		return;
	}

	__atomic_fetch_add(&g_poll_sleepers, 1, __ATOMIC_ACQ_REL);
	cur->wait_channel = (void *)&g_poll_io_ready;
	cur->wakeup_tick = wake;
	cur->state = TASK_BLOCKED;

	/* Re-check AFTER publishing TASK_BLOCKED, which is the whole point of
	 * the ordering: a producer that bumps the counter from here on finds
	 * this task parked and wakes it, and one that bumped it earlier -- while
	 * the caller was scanning its descriptors, when this task was still
	 * RUNNING and could not be found -- is caught right here. */
	if (poll_io_seq() != seq_before) {
		/* Un-park.  CAS rather than a plain store: a waker may already
		 * have claimed this task (BLOCKED -> READY) and be about to
		 * enqueue it, and overwriting its state would leave it READY
		 * and on a run queue while also running here.  If the CAS
		 * fails the waker won, so fall through to sched_schedule(),
		 * which handles being enqueued already. */
		task_state_t expected = TASK_BLOCKED;
		if (__atomic_compare_exchange_n(&cur->state, &expected,
						TASK_RUNNING, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE)) {
			cur->wakeup_tick = 0;
			cur->wait_channel = NULL;
			__atomic_fetch_sub(&g_poll_sleepers, 1,
					   __ATOMIC_ACQ_REL);
			return;
		}
	}

	sched_schedule();
	__atomic_fetch_sub(&g_poll_sleepers, 1, __ATOMIC_ACQ_REL);
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	if (cur->state != TASK_RUNNING)
		cur->state = TASK_RUNNING;
}

// ============================================================================
// fd_poll_one - Poll a single fd for events (internal helper)
// Returns revents mask. Works for sockets, pipes, regular files, console.
// ============================================================================
static short fd_poll_one(int fd, short events, struct poll_table *pt)
{
	/* One exit, so the hold taken below is released whichever way this
	 * descriptor is answered.  `entry' is declared HERE rather than at the
	 * lookup: the early answers jump to that exit before the lookup runs,
	 * and the exit reads it. */
	void *entry = NULL;
	short rev_out;

	task_t *cur = sched_current();
	if (!cur) {
		rev_out = POLLNVAL;
		goto out;
	}
	if (fd < 0) {
		rev_out = POLLNVAL;
		goto out;
	}

	if ((unsigned)fd >= TASK_MAX_FDS) {
		rev_out = POLLNVAL;
		goto out;
	}

	/* Held while this descriptor is examined.  Every arm below reaches
	 * into the object -- a socket's state, a pipe end's magic, a wait
	 * queue to register on -- and a sibling thread closing the
	 * descriptor mid-scan would otherwise destroy it underneath. */
	entry = fdget(cur, fd);

	/* The console is an EMPTY fd_table slot at 0/1/2 (the 1..3 dup markers
	 * are handled further down).  It must be selected by the slot's
	 * CONTENT, never by the descriptor NUMBER: deciding by number polled
	 * the terminal even for a descriptor that had been redirected
	 * elsewhere, so a program that polls its own stdin waited on the
	 * console forever while its real input sat unread in a socket or pipe
	 * (this is what hung sftp-server, whose stdin is an AF_UNIX socket).
	 * /dev/console is bidirectional, so any of 0/1/2 may be polled for
	 * both POLLIN and POLLOUT. */
	if (!entry) {
		/* An empty slot at 0/1/2 is the console only while the process
		 * still has it open; once closed it is an invalid descriptor
		 * like any other, and must not be reported as ready. */
		if (!task_fd_is_console(cur, fd)) {
			rev_out = POLLNVAL;
			goto out;
		}
		short rev = 0;
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();

		if (tty)
			poll_wait(pt, NULL, &tty->poll_wq);
		if (events & (POLLIN | POLLRDNORM)) {
			if (tty && tty->read_count > 0)
				rev |= POLLIN | POLLRDNORM;
		}
		if (events & (POLLOUT | POLLWRNORM))
			rev |= POLLOUT | POLLWRNORM;
		rev_out = rev;
		goto out;
	}

	// Socket fd marker
	if (IS_SOCKET_FD(entry)) {
		net_socket_t *ns = sock_get(SOCKET_FD_IDX(entry));

		if (ns)
			poll_wait(pt, entry, &ns->poll_wq);
		rev_out = (short)sock_poll(SOCKET_FD_IDX(entry), events);
		goto out;
	}

	// UNIX socket fd marker
	if (unix_sock_is(entry)) {
		poll_wait(pt, entry, &((unix_socket_t *)entry)->poll_wq);
		rev_out = (short)unix_poll((unix_socket_t *)entry, events);
		goto out;
	}

	// Epoll fd marker
	if (IS_EPOLL_FD(entry)) {
		// Epoll fds are not themselves pollable in a meaningful way
		WARN_RATELIMIT(
			1,
			"poll: epoll fd %d passed to fd_poll_one - epoll instances cannot be nested via poll()",
			fd);
		rev_out = POLLNVAL;
		goto out;
	}

	// Console dup markers (1, 2, 3) — all reference the bidirectional
	// /dev/console device, so each is both readable and writable.
	uintptr_t marker = (uintptr_t)entry;
	if (marker >= 1 && marker <= 3) {
		short rev = 0;
		tty_t *tty = cur->ctty ? cur->ctty : tty_get_console();

		if (tty)
			poll_wait(pt, NULL, &tty->poll_wq);
		if (events & (POLLIN | POLLRDNORM)) {
			if (tty && tty->read_count > 0)
				rev |= POLLIN | POLLRDNORM;
		}
		if (events & (POLLOUT | POLLWRNORM))
			rev |= POLLOUT | POLLWRNORM;
		rev_out = rev;
		goto out;
	}

	// Pipe fd
	if (pipe_is_end(entry)) {
		pipe_end_t *pe = (pipe_end_t *)entry;
		pipe_t *p = pe->pipe;

		poll_wait(pt, entry, &p->poll_wq);
		WARN_ON(p->used >
			p->size); /* pipe ring buffer invariant violated: used > capacity */
		short rev = 0;
		if (pe->is_read) {
			if ((events & (POLLIN | POLLRDNORM)) && p->used > 0)
				rev |= POLLIN | POLLRDNORM;
			if (p->writers == 0)
				rev |= POLLHUP;
		} else {
			if ((events & (POLLOUT | POLLWRNORM)) &&
			    p->used < p->size)
				rev |= POLLOUT | POLLWRNORM;
			if (p->readers == 0)
				rev |= POLLERR;
		}
		rev_out = rev;
		goto out;
	}

	// Pty master (opened via /dev/ptmx): readable when slave wrote bytes,
	// writable when slave is open, HUP when slave closed and buffer empty.
	{
		int pid = devfs_get_pty_master_id((vfs_file_t *)entry);
		if (pid >= 0) {
			poll_wait(pt, entry, tty_pty_master_pollq(pid));
			rev_out = (short)tty_pty_master_poll(pid, events);
			goto out;
		}
	}

	// Event devices (/dev/input/eventN): readable when events are queued.
	{
		int unit = devfs_evdev_unit((vfs_file_t *)entry);
		if (unit >= 0) {
			poll_wait(pt, entry, evdev_pollq(unit));
			rev_out = evdev_poll(unit, events);
			goto out;
		}
	}

	// Tty/pty-slave (real terminal): always writable, readable when input is queued.
	{
		tty_t *tty = devfs_get_tty((vfs_file_t *)entry);
		if (tty) {
			short rev = 0;

			poll_wait(pt, entry, &tty->poll_wq);
			if ((events & (POLLIN | POLLRDNORM)) &&
			    tty->read_count > 0)
				rev |= POLLIN | POLLRDNORM;
			if (events & (POLLOUT | POLLWRNORM))
				rev |= POLLOUT | POLLWRNORM;
			rev_out = rev;
			goto out;
		}
	}

	// Registered device nodes and anonymous device files: the driver's
	// own poll hook (or always-ready when it has none).
	{
		short rev_dev;
		if (devfs_device_poll((vfs_file_t *)entry, events, pt,
				      &rev_dev) == 0) {
			rev_out = rev_dev;
			goto out;
		}
	}

	// Regular file - always ready for read/write
	short rev = 0;
	if (events & (POLLIN | POLLRDNORM))
		rev |= POLLIN | POLLRDNORM;
	if (events & (POLLOUT | POLLWRNORM))
		rev |= POLLOUT | POLLWRNORM;
	rev_out = rev;
	goto out;

out:
	if (entry)
		fdput(entry);
	return rev_out;
}

// ============================================================================
// sys_select_internal - select() implementation
// Scans readfds/writefds/exceptfds for ready file descriptors.
// timeout_ticks: 0 = poll (non-blocking), (uint64_t)-1 = block forever
// Returns number of ready fds, or negative errno.
// ============================================================================
/* The body runs with a poll table; this wrapper owns it.
 *
 * Split so the table is released in exactly ONE place: the body returns from a
 * dozen points, and a queue left holding a pointer into a freed table is a use
 * after free on the next wakeup. */
static int sys_select_locked(int nfds, fd_set *readfds, fd_set *writefds,
			     fd_set *exceptfds, uint64_t timeout_ticks,
			     struct poll_table *pt);

int sys_select_internal(int nfds, fd_set *readfds, fd_set *writefds,
			fd_set *exceptfds, uint64_t timeout_ticks)
{
	struct poll_table pt;
	int r;

	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;
	/* Three sets, so at worst three registrations per descriptor -- but a
	 * descriptor is only ever registered once per scan, so nfds is the
	 * bound. */
	poll_table_init(&pt, nfds);
	r = sys_select_locked(nfds, readfds, writefds, exceptfds, timeout_ticks,
			      &pt);
	poll_table_free(&pt);
	return r;
}

static int sys_select_locked(int nfds, fd_set *readfds, fd_set *writefds,
			     fd_set *exceptfds, uint64_t timeout_ticks,
			     struct poll_table *pt)
{

	fd_set r_in, w_in, e_in;
	if (readfds)
		r_in = *readfds;
	else
		FD_ZERO(&r_in);
	if (writefds)
		w_in = *writefds;
	else
		FD_ZERO(&w_in);
	if (exceptfds)
		e_in = *exceptfds;
	else
		FD_ZERO(&e_in);

	uint64_t deadline = 0;
	if (timeout_ticks == 0) {
		// Non-blocking poll
	} else if (timeout_ticks != (uint64_t)-1) {
		/* +1 for the tick already in progress: timer_ticks() was read
		 * at some unknown fraction into the current tick, so without it
		 * the wake lands up to one tick early and the timeout expires
		 * short of what was asked for.  A timeout may fire late, never
		 * early; the futex and sleep paths arm their deadlines the same
		 * way. */
		deadline = timer_ticks() + timeout_ticks + 1;
	}

	while (1) {
		/* Sampled BEFORE the scan below.  An event that arrives while
		 * the scan is running finds this task still RUNNING and cannot
		 * park it, so the wake would be lost; the sleep path compares
		 * this against the counter after publishing its blocked state
		 * and re-scans instead of sleeping through the event. */
		/* Off every queue from the previous iteration before going
		 * back on: an entry may be on one queue at a time, and the set
		 * of interesting descriptors can change between scans. */
		poll_table_reset(pt);

		uint64_t seq_before = poll_io_seq();
		int count = 0;
		fd_set r_out, w_out, e_out;
		FD_ZERO(&r_out);
		FD_ZERO(&w_out);
		FD_ZERO(&e_out);

		for (int fd = 0; fd < nfds; fd++) {
			short events = 0;
			if (readfds && FD_ISSET(fd, &r_in))
				events |= POLLIN;
			if (writefds && FD_ISSET(fd, &w_in))
				events |= POLLOUT;
			if (exceptfds && FD_ISSET(fd, &e_in))
				events |= POLLPRI;

			if (events == 0)
				continue;

			short rev = fd_poll_one(fd, events, pt);

			if ((rev & (POLLIN | POLLRDNORM | POLLHUP | POLLERR)) &&
			    readfds && FD_ISSET(fd, &r_in)) {
				FD_SET(fd, &r_out);
				count++;
			}
			if ((rev & (POLLOUT | POLLWRNORM)) && writefds &&
			    FD_ISSET(fd, &w_in)) {
				FD_SET(fd, &w_out);
				count++;
			}
			if ((rev & (POLLERR | POLLPRI)) && exceptfds &&
			    FD_ISSET(fd, &e_in)) {
				FD_SET(fd, &e_out);
				count++;
			}
		}

		if (count > 0 || timeout_ticks == 0) {
			if (readfds)
				*readfds = r_out;
			if (writefds)
				*writefds = w_out;
			if (exceptfds)
				*exceptfds = e_out;
			return count;
		}

		// Block until deadline or forever
		if (timeout_ticks != (uint64_t)-1 &&
		    timer_ticks() >= deadline) {
			if (readfds)
				FD_ZERO(readfds);
			if (writefds)
				FD_ZERO(writefds);
			if (exceptfds)
				FD_ZERO(exceptfds);
			return 0;
		}

		/* A deliverable signal must break the wait: a signal handler only
		 * runs on the way back to user mode, so a task that just keeps
		 * sleeping here never runs it.  This silently broke every
		 * event-driven program — a terminal app blocked in poll() never
		 * saw SIGWINCH (window resizes were ignored) and a server never
		 * saw SIGCHLD (children were never reaped, so a session never
		 * closed).  signal_pending() ignores masked signals, so a
		 * blocked signal cannot spin us here. */
		{
			task_t *_cur = sched_current();
			if (_cur && signal_pending(_cur))
				return -EINTR;
		}

		poll_sleep_until_next_tick(deadline,
					   timeout_ticks != (uint64_t)-1,
					   seq_before);
	}
}

// ============================================================================
// sys_poll_internal - poll() implementation
// Scans array of pollfd structs for ready fds.
// timeout_ticks: 0 = non-blocking, (uint64_t)-1 = block forever
// Returns number of ready fds, or negative errno.
// ============================================================================
/* The body runs with a poll table; this wrapper owns it.
 *
 * Split so the table is released in exactly ONE place: the body returns from a
 * dozen points, and a queue left holding a pointer into a freed table is a use
 * after free on the next wakeup. */
static int sys_poll_locked(struct pollfd *fds, int nfds,
			   uint64_t timeout_ticks, struct poll_table *pt);

int sys_poll_internal(struct pollfd *fds, int nfds, uint64_t timeout_ticks)
{
	struct poll_table pt;
	int r;

	if (nfds < 0 || !fds)
		return -EINVAL;
	poll_table_init(&pt, nfds);
	r = sys_poll_locked(fds, nfds, timeout_ticks, &pt);
	poll_table_free(&pt);
	return r;
}

static int sys_poll_locked(struct pollfd *fds, int nfds,
			   uint64_t timeout_ticks, struct poll_table *pt)
{
	WARN_ON(nfds >
		TASK_MAX_FDS); /* poll() with nfds > TASK_MAX_FDS: kernel cannot have that many open fds */

	uint64_t deadline = 0;
	if (timeout_ticks == 0) {
		// Non-blocking poll
	} else if (timeout_ticks != (uint64_t)-1) {
		/* +1 for the tick already in progress: timer_ticks() was read
		 * at some unknown fraction into the current tick, so without it
		 * the wake lands up to one tick early and the timeout expires
		 * short of what was asked for.  A timeout may fire late, never
		 * early; the futex and sleep paths arm their deadlines the same
		 * way. */
		deadline = timer_ticks() + timeout_ticks + 1;
	}

	while (1) {
		/* Sampled BEFORE the scan below.  An event that arrives while
		 * the scan is running finds this task still RUNNING and cannot
		 * park it, so the wake would be lost; the sleep path compares
		 * this against the counter after publishing its blocked state
		 * and re-scans instead of sleeping through the event. */
		/* Off every queue from the previous iteration before going
		 * back on: an entry may be on one queue at a time, and the set
		 * of interesting descriptors can change between scans. */
		poll_table_reset(pt);

		uint64_t seq_before = poll_io_seq();
		int count = 0;

		for (int i = 0; i < nfds; i++) {
			fds[i].revents = fd_poll_one(fds[i].fd, fds[i].events, pt);
			if (fds[i].revents != 0)
				count++;
		}

		if (count > 0 || timeout_ticks == 0)
			return count;

		if (timeout_ticks != (uint64_t)-1 && timer_ticks() >= deadline)
			return 0;

		/* A deliverable signal must break the wait: a signal handler only
		 * runs on the way back to user mode, so a task that just keeps
		 * sleeping here never runs it.  This silently broke every
		 * event-driven program — a terminal app blocked in poll() never
		 * saw SIGWINCH (window resizes were ignored) and a server never
		 * saw SIGCHLD (children were never reaped, so a session never
		 * closed).  signal_pending() ignores masked signals, so a
		 * blocked signal cannot spin us here. */
		{
			task_t *_cur = sched_current();
			if (_cur && signal_pending(_cur))
				return -EINTR;
		}

		poll_sleep_until_next_tick(deadline,
					   timeout_ticks != (uint64_t)-1,
					   seq_before);
	}
}

// ============================================================================
// Epoll implementation
// ============================================================================

int epoll_create_internal(int flags)
{
	(void)flags;
	uint64_t fl;
	spin_lock_irqsave(&epoll_lock, &fl);

	int idx = -1;
	for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
		if (!epoll_instances[i].active) {
			idx = i;
			break;
		}
	}

	if (idx < 0) {
		spin_unlock_irqrestore(&epoll_lock, fl);
		return -ENFILE;
	}

	epoll_instance_t *ep = &epoll_instances[idx];
	ep->active = 1;
	ep->ref_count = 1; /* the descriptor about to be returned */
	ep->nentries = 0;
	ep->lock = (spinlock_t)SPINLOCK_INIT("epoll_inst");

	spin_unlock_irqrestore(&epoll_lock, fl);
	return idx;
}

/*
 * Take and drop a reference on an epoll instance.
 *
 * Called from the descriptor lifetime paths -- fork duplicates a task's fd
 * table, close releases one entry, exec closes the FD_CLOEXEC ones.  The
 * instance outlives any single descriptor and is only released when the last
 * one goes.
 */
void epoll_get(int idx)
{
	uint64_t fl;

	if (idx < 0 || idx >= MAX_EPOLL_INSTANCES)
		return;
	spin_lock_irqsave(&epoll_lock, &fl);
	if (epoll_instances[idx].active)
		epoll_instances[idx].ref_count++;
	spin_unlock_irqrestore(&epoll_lock, fl);
}

void epoll_put(int idx)
{
	uint64_t fl;

	if (idx < 0 || idx >= MAX_EPOLL_INSTANCES)
		return;
	spin_lock_irqsave(&epoll_lock, &fl);
	if (epoll_instances[idx].active) {
		if (--epoll_instances[idx].ref_count <= 0) {
			epoll_instances[idx].active = 0;
			epoll_instances[idx].nentries = 0;
			epoll_instances[idx].ref_count = 0;
		}
	}
	spin_unlock_irqrestore(&epoll_lock, fl);
}

int epoll_ctl_internal(int epfd_idx, int op, int fd, struct epoll_event *event)
{
	if (epfd_idx < 0 || epfd_idx >= MAX_EPOLL_INSTANCES)
		return -EBADF;
	epoll_instance_t *ep = &epoll_instances[epfd_idx];
	if (!ep->active)
		return -EBADF;

	uint64_t fl;
	spin_lock_irqsave(&ep->lock, &fl);

	switch (op) {
	case EPOLL_CTL_ADD: {
		if (!event) {
			spin_unlock_irqrestore(&ep->lock, fl);
			return -EINVAL;
		}
		// Check for duplicate
		for (int i = 0; i < ep->nentries; i++) {
			if (ep->entries[i].fd == fd) {
				spin_unlock_irqrestore(&ep->lock, fl);
				return -EEXIST;
			}
		}
		if (ep->nentries >= MAX_EPOLL_ENTRIES) {
			spin_unlock_irqrestore(&ep->lock, fl);
			return -ENOMEM;
		}
		int idx = ep->nentries++;
		ep->entries[idx].fd = fd;
		ep->entries[idx].events = event->events;
		ep->entries[idx].data = event->data.u64;
		ep->entries[idx].oneshot_triggered = 0;
		break;
	}
	case EPOLL_CTL_DEL: {
		int found = -1;
		for (int i = 0; i < ep->nentries; i++) {
			if (ep->entries[i].fd == fd) {
				found = i;
				break;
			}
		}
		if (found < 0) {
			spin_unlock_irqrestore(&ep->lock, fl);
			return -ENOENT;
		}
		// Shift entries down
		for (int i = found; i < ep->nentries - 1; i++)
			ep->entries[i] = ep->entries[i + 1];
		ep->nentries--;
		WARN_ON(ep->nentries <
			0); /* epoll nentries went negative after DEL - double-delete or accounting bug */
		break;
	}
	case EPOLL_CTL_MOD: {
		if (!event) {
			spin_unlock_irqrestore(&ep->lock, fl);
			return -EINVAL;
		}
		int found = -1;
		for (int i = 0; i < ep->nentries; i++) {
			if (ep->entries[i].fd == fd) {
				found = i;
				break;
			}
		}
		if (found < 0) {
			spin_unlock_irqrestore(&ep->lock, fl);
			return -ENOENT;
		}
		ep->entries[found].events = event->events;
		ep->entries[found].data = event->data.u64;
		ep->entries[found].oneshot_triggered = 0;
		break;
	}
	default:
		spin_unlock_irqrestore(&ep->lock, fl);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&ep->lock, fl);
	return 0;
}

/* The body runs with a poll table; this wrapper owns it -- see the note on
 * sys_select_internal() for why the split. */
static int epoll_wait_locked(epoll_instance_t *ep, struct epoll_event *events,
			     int maxevents, uint64_t timeout_ticks,
			     struct poll_table *pt);

int epoll_wait_internal(int epfd_idx, struct epoll_event *events, int maxevents,
			uint64_t timeout_ticks)
{
	struct poll_table pt;
	int r;

	if (epfd_idx < 0 || epfd_idx >= MAX_EPOLL_INSTANCES)
		return -EBADF;
	epoll_instance_t *ep = &epoll_instances[epfd_idx];
	if (!ep->active)
		return -EBADF;
	if (maxevents <= 0 || !events)
		return -EINVAL;

	/* Sized to the whole watch list rather than to maxevents: the scan
	 * registers on every watched descriptor, not just the ones it will
	 * report. */
	poll_table_init(&pt, ep->nentries > 0 ? ep->nentries : 1);
	r = epoll_wait_locked(ep, events, maxevents, timeout_ticks, &pt);
	poll_table_free(&pt);
	return r;
}

static int epoll_wait_locked(epoll_instance_t *ep, struct epoll_event *events,
			     int maxevents, uint64_t timeout_ticks,
			     struct poll_table *pt)
{

	uint64_t deadline = 0;
	if (timeout_ticks == 0) {
		// Non-blocking
	} else if (timeout_ticks != (uint64_t)-1) {
		/* +1 for the tick already in progress: timer_ticks() was read
		 * at some unknown fraction into the current tick, so without it
		 * the wake lands up to one tick early and the timeout expires
		 * short of what was asked for.  A timeout may fire late, never
		 * early; the futex and sleep paths arm their deadlines the same
		 * way. */
		deadline = timer_ticks() + timeout_ticks + 1;
	}

	while (1) {
		/* Sampled BEFORE the scan below.  An event that arrives while
		 * the scan is running finds this task still RUNNING and cannot
		 * park it, so the wake would be lost; the sleep path compares
		 * this against the counter after publishing its blocked state
		 * and re-scans instead of sleeping through the event. */
		/* Off every queue from the previous iteration before going
		 * back on: an entry may be on one queue at a time, and the set
		 * of interesting descriptors can change between scans. */
		poll_table_reset(pt);

		uint64_t seq_before = poll_io_seq();
		int count = 0;

		uint64_t fl;
		spin_lock_irqsave(&ep->lock, &fl);

		for (int i = 0; i < ep->nentries && count < maxevents; i++) {
			if (ep->entries[i].oneshot_triggered &&
			    (ep->entries[i].events & EPOLLONESHOT))
				continue;

			short poll_events = 0;
			if (ep->entries[i].events & (EPOLLIN | EPOLLRDNORM))
				poll_events |= POLLIN;
			if (ep->entries[i].events & (EPOLLOUT | EPOLLWRNORM))
				poll_events |= POLLOUT;
			if (ep->entries[i].events & EPOLLPRI)
				poll_events |= POLLPRI;

			short rev = fd_poll_one(ep->entries[i].fd, poll_events, pt);
			if (rev == 0)
				continue;

			uint32_t ep_events = 0;
			if (rev & (POLLIN | POLLRDNORM))
				ep_events |= EPOLLIN;
			if (rev & (POLLOUT | POLLWRNORM))
				ep_events |= EPOLLOUT;
			if (rev & POLLERR)
				ep_events |= EPOLLERR;
			if (rev & POLLHUP)
				ep_events |= EPOLLHUP;
			if (rev & POLLPRI)
				ep_events |= EPOLLPRI;

			ep_events &=
				(ep->entries[i].events | EPOLLERR | EPOLLHUP);
			if (ep_events == 0)
				continue;

			events[count].events = ep_events;
			events[count].data.u64 = ep->entries[i].data;
			count++;

			if (ep->entries[i].events & EPOLLONESHOT)
				ep->entries[i].oneshot_triggered = 1;
		}

		spin_unlock_irqrestore(&ep->lock, fl);

		WARN_ON(count >
			maxevents); /* epoll_wait returned more events than maxevents - loop bound violated */
		if (count > 0 || timeout_ticks == 0)
			return count;

		if (timeout_ticks != (uint64_t)-1 && timer_ticks() >= deadline)
			return 0;

		/* A deliverable signal must break the wait: a signal handler only
		 * runs on the way back to user mode, so a task that just keeps
		 * sleeping here never runs it.  This silently broke every
		 * event-driven program — a terminal app blocked in poll() never
		 * saw SIGWINCH (window resizes were ignored) and a server never
		 * saw SIGCHLD (children were never reaped, so a session never
		 * closed).  signal_pending() ignores masked signals, so a
		 * blocked signal cannot spin us here. */
		{
			task_t *_cur = sched_current();
			if (_cur && signal_pending(_cur))
				return -EINTR;
		}

		poll_sleep_until_next_tick(deadline,
					   timeout_ticks != (uint64_t)-1,
					   seq_before);
	}
}

// Helper: extract epoll index from a process fd
/* Resolve an epoll descriptor, taking a reference on the instance so it
 * survives the operation.  Release it with epoll_idx_release().
 *
 * The reference is the point: the index alone says nothing about whether the
 * instance is still there, and a sibling thread closing the descriptor while
 * the operation ran would destroy it underneath. */
static int epoll_idx_from_fd(uint64_t fd)
{
	task_t *cur = sched_current();
	void *entry;
	int idx;

	if (!cur || fd >= TASK_MAX_FDS)
		return -EBADF;
	entry = fdget(cur, (int)fd);
	if (!entry)
		return -EBADF;
	if (!IS_EPOLL_FD(entry)) {
		fdput(entry);
		return -EBADF;
	}
	idx = EPOLL_FD_IDX(entry);
	/* fdget's hold IS the instance reference: fdput on an epoll marker is
	 * epoll_put, so the pairing below releases exactly this. */
	return idx;
}

static void epoll_idx_release(int idx)
{
	epoll_put(idx);
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

__attribute__((noinline)) int64_t
sys_select(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
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

__attribute__((noinline)) int64_t
sys_pselect6(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
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

__attribute__((noinline)) int64_t
sys_poll(uint64_t a1, uint64_t a2, uint64_t a3)
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

__attribute__((noinline)) int64_t
sys_ppoll(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
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

__attribute__((noinline)) int64_t
sys_epoll_wait(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	int64_t ret;
	/* Resolving the descriptor takes a reference on the epoll instance, so
	 * from here every way out has to give it back -- but NOT this one:
	 * a failed resolution holds nothing, and releasing a negative index
	 * would drop a reference belonging to some unrelated instance. */
	int ep_idx = epoll_idx_from_fd(a1);

	if (ep_idx < 0)
		return ep_idx;
	int maxevents = (int)a3;
	if (maxevents <= 0 || maxevents > 256) {
		ret = -EINVAL;
		goto out;
	}
	size_t sz = (size_t)maxevents * sizeof(struct epoll_event);
	if (!validate_user_ptr(a2, sz)) {
		ret = -EFAULT;
		goto out;
	}
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
	int got = epoll_wait_internal(ep_idx, kevs, maxevents, timeout_ticks);

	if (got > 0)
		copy_to_user((void *)a2, kevs,
			     (size_t)got * sizeof(struct epoll_event));
	ret = got;

out:
	epoll_idx_release(ep_idx);
	return ret;
}

int64_t sys_epoll_create(void)
{
	int ep_idx = epoll_create_internal(0);
	if (ep_idx < 0)
		return ep_idx;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	/* fd_install_from, not a hand-rolled scan: it claims the slot under
	 * the descriptor-table lock, so two threads creating epoll sets at the
	 * same moment cannot be handed the same number -- and it clears the
	 * slot's stale flag byte, which a recycled slot would otherwise keep. */
	{
		int fd = fd_install_from(cur, MAKE_EPOLL_FD(ep_idx), 3);

		if (fd < 0) {
			/* No descriptor for it: give the instance back rather
			 * than leaving one nothing can ever reach. */
			epoll_put(ep_idx);
			return fd;
		}
		return fd;
	}
}

int64_t sys_epoll_create1(uint64_t a1)
{
	int ep_idx = epoll_create_internal((int)a1);
	if (ep_idx < 0)
		return ep_idx;
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	{
		int fd = fd_install_from(cur, MAKE_EPOLL_FD(ep_idx), 3);

		if (fd < 0) {
			epoll_put(ep_idx);
			return fd;
		}
		/* EPOLL_CLOEXEC has to be RECORDED, not just accepted.  An
		 * epoll set is private to the process that built it, and every
		 * caller asks for it -- letting the descriptor survive exec()
		 * hands an unrelated program a handle onto it, and the
		 * reference it drops on exit is one the creator was still
		 * using. */
		if ((int)a1 & EPOLL_CLOEXEC)
			task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
		return fd;
	}
}

int64_t sys_epoll_ctl(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	int ep_idx = epoll_idx_from_fd(a1);
	struct epoll_event kev;
	int64_t ret;

	if (ep_idx < 0)
		return ep_idx;
	if (a4 && validate_user_ptr(a4, sizeof(struct epoll_event)))
		copy_from_user(&kev, (void *)a4,
			       sizeof(struct epoll_event));
	ret = epoll_ctl_internal(ep_idx, (int)a2, (int)a3, a4 ? &kev : NULL);
	epoll_idx_release(ep_idx);
	return ret;
}
