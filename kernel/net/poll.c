// LikeOS Poll / Select
// Multiplexed I/O over every kind of descriptor: sockets, pipes, terminals,
// devices and regular files.  epoll lives in eventpoll.c and shares the
// per-object readiness questions answered here (poll_entry()).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/eventpoll.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
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

/* ---------------------------------------------------------------------------
 * Registering a poller on the objects it is waiting for.
 *
 * While a poller scans its descriptors it is added to the wait queue of every
 * object it asks about, so a readiness change wakes exactly the tasks that
 * asked about THAT object -- never every poller on the machine.
 *
 * Each registration is a wait-queue entry with a wake CALLBACK (pollwake):
 * a wake marks the whole poll table `triggered' and makes the task runnable.
 * The poller clears `triggered' before it scans, and after a scan that found
 * nothing it publishes its sleep and only then looks at the flag.  An object
 * registers (poll_wait) BEFORE it reads its own state, and its producer
 * changes the state BEFORE it wakes the queue, so an event can only land in
 * one of two places: before the read (the scan sees it) or after the
 * registration (the callback sets the flag, or finds the task parked and
 * wakes it).  Nothing global is consulted: an event on an unrelated object
 * costs this poller nothing.
 *
 * The table is allocated once per call and reused across the scan/sleep
 * iterations; each iteration unregisters before it re-registers, because the
 * set of objects can change between scans (a descriptor closed and reopened)
 * and an entry may be on one queue at a time.
 * ------------------------------------------------------------------------ */

struct poll_table_entry {
	struct wait_queue_entry wait;
	struct wait_queue_head *head; /* the queue `wait' sits on */
	/* The descriptor entry that OWNS `head', with a reference held for
	 * as long as `wait' is linked there.  The scan's own hold ends before
	 * the sleep, but the registration outlives it: without this hold a
	 * sibling thread closing the last descriptor frees the object while
	 * this poller is still on its queue.  NULL for a queue in something
	 * that is never freed (the console tty). */
	void *held;
	/* The events asked for on this descriptor (plus the ones always
	 * reported): a wake whose key says something else is ignored. */
	unsigned key;
};

static int pollwake(struct wait_queue_entry *e, unsigned key,
		    struct wq_wake_ctx *ctx)
{
	struct poll_table *pt = (struct poll_table *)e->priv;
	struct poll_table_entry *pe = (struct poll_table_entry *)e;

	if (key && !(key & pe->key))
		return 0;
	/* The flag first, sequentially consistent: the poller publishes its
	 * sleep and then reads the flag, this sets the flag and then looks
	 * at the task's state -- one of the two sees the other. */
	__atomic_store_n(&pt->triggered, 1, __ATOMIC_SEQ_CST);
	wq_ctx_claim(ctx, e->task);
	return 1;
}

static void poll_table_init(struct poll_table *pt, int cap)
{
	pt->qproc = NULL;
	pt->nopoll = 0;
	pt->key = 0;
	pt->ents = NULL;
	pt->n = 0;
	pt->cap = 0;
	pt->unregistered = 0;
	pt->triggered = 0;
	if (cap <= 0)
		return;
	pt->ents = kalloc((size_t)cap * sizeof(*pt->ents));
	if (!pt->ents)
		return; /* cap stays 0: poll_wait refuses, see below */
	pt->cap = cap;
}

/* Take this poller off every queue it is on.  Must run before the table is
 * freed and before the caller returns, or a queue would keep a pointer into a
 * dead allocation. */
static void poll_table_reset(struct poll_table *pt)
{
	for (int i = 0; i < pt->n; i++) {
		/* Off the queue FIRST, then let go of what owns the queue:
		 * the other order can free the object -- and the head this is
		 * about to unlink from -- between the two lines. */
		wq_remove(pt->ents[i].head, &pt->ents[i].wait);
		fdput(pt->ents[i].held);
		pt->ents[i].held = NULL;
	}
	pt->n = 0;
	pt->unregistered = 0;
}

static void poll_table_free(struct poll_table *pt)
{
	poll_table_reset(pt);
	kfree(pt->ents);
	pt->ents = NULL;
	pt->cap = 0;
}

/* Register the calling task on `h', which belongs to `owner'.
 *
 * `owner' is the descriptor entry the queue lives inside, and this takes a
 * reference on it that lasts until poll_table_reset() unlinks the entry again.
 * Pass NULL only for a queue in an object that is never freed.
 *
 * A no-op without a table (the caller only wants the readiness mask).  A
 * table that is full or could not be allocated refuses, and the poller then
 * re-scans on the next tick for that iteration instead of sleeping until a
 * wake that may never come.  An epoll table (qproc) does its own thing. */
void poll_wait(struct poll_table *pt, void *owner, struct wait_queue_head *h)
{
	task_t *cur;
	struct poll_table_entry *pe;

	if (!pt || !h)
		return;
	if (pt->qproc) {
		pt->qproc(pt, owner, h);
		return;
	}
	if (pt->n >= pt->cap) {
		pt->unregistered = 1;
		return;
	}
	cur = sched_current();
	if (!cur)
		return;
	/* Before the entry goes on the queue, never after: the moment it is
	 * linked, an unreferenced owner can be freed with this entry still on
	 * it.  A refused hold means the object is already gone. */
	if (owner && !fdhold((vfs_file_t *)owner)) {
		pt->unregistered = 1;
		return;
	}
	pe = &pt->ents[pt->n];
	wq_entry_init_func(&pe->wait, cur, pollwake, pt);
	pe->held = owner;
	pe->head = h;
	/* The events asked of this descriptor (fd_scan_one) and the ones
	 * always reported. */
	pe->key = pt->key | POLLERR | POLLHUP | POLLNVAL;
	wq_add(h, &pe->wait);
	pt->n++;
}

/* Report that `h' has become ready: wake its pollers (and one exclusive
 * epoll set).  `key' names the POLL* bits that changed, 0 when unknown. */
void poll_notify_wq_key(struct wait_queue_head *h, unsigned key)
{
	if (h)
		wq_wake_poll(h, key);
}

void poll_notify_wq(struct wait_queue_head *h)
{
	if (h)
		wq_wake_poll(h, 0);
}

/* The object-less form.  A readiness change with no queue to name concerns
 * nobody: a poller is registered on an object's queue before it reads the
 * object's state, so an object without a queue has no pollers to tell. */
void poll_notify_io_ready(void)
{
}

/* A timeout's absolute deadline on the nanosecond clock; one too large to
 * add is as good as forever. */
static uint64_t poll_deadline_ns(uint64_t timeout_ns)
{
	uint64_t now = hrtimer_now_ns();

	return timeout_ns > ~0ULL - now ? ~0ULL : now + timeout_ns;
}

/* Block the calling task until one of the queues it registered on during the
 * scan is woken, or the deadline.  A poller with no deadline sleeps until
 * woken (signals still end the sleep: the sleeper sweep checks
 * signal_pending() whatever the deadline).  The one-tick re-scan is used only
 * by a poller that could not register on every object it asked about. */
static void poll_sleep(uint64_t deadline_ticks, uint64_t deadline_ns,
		       int have_deadline, struct poll_table *pt)
{
	task_t *cur = sched_current();
	if (!cur) {
		__asm__ volatile("pause");
		return;
	}
	uint64_t now = timer_ticks();
	uint64_t wake = 0; /* no deadline: sleep until woken */
	/* The tick count is advanced by one processor's tick and can stand
	 * behind the clock; only the nanosecond deadline ends a wait early,
	 * the tick is its backstop. */
	if (have_deadline && deadline_ticks <= now &&
	    hrtimer_now_ns() < deadline_ns)
		deadline_ticks = now + 1;
	if (pt->unregistered)
		wake = now + 1; /* fallback: re-scan next tick */
	if (have_deadline && (wake == 0 || deadline_ticks < wake))
		wake = deadline_ticks;
	if (wake != 0 && wake <= now) {
		__asm__ volatile("pause");
		return;
	}

	cur->wait_channel = (void *)pt;
	cur->wakeup_tick = wake;
	task_set_blocked(cur);

	/* Re-check AFTER publishing TASK_BLOCKED: a wake that came during the
	 * scan, while this task was still running and could not be claimed,
	 * left the flag behind. */
	if (__atomic_load_n(&pt->triggered, __ATOMIC_SEQ_CST)) {
		/* Un-park by CAS: a waker may already have claimed this task
		 * (BLOCKED -> READY) and be about to enqueue it.  If the CAS
		 * fails the waker won; sched_schedule() consumes that. */
		task_state_t expected = TASK_BLOCKED;
		if (__atomic_compare_exchange_n(&cur->state, &expected,
						TASK_RUNNING, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE)) {
			cur->wakeup_tick = 0;
			cur->wait_channel = NULL;
			return;
		}
	}

	/* The timeout's own deadline, to the microsecond; the tick deadline
	 * above only backs it up. */
	if (have_deadline)
		hrtimer_wait_arm(cur, deadline_ns);
	sched_schedule();
	if (have_deadline)
		hrtimer_wait_disarm(cur);
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	if (cur->state != TASK_RUNNING)
		cur->state = TASK_RUNNING;
}

// ============================================================================
// poll_entry - the readiness of one descriptor-table entry
// ============================================================================

/* The console: an empty slot at 0/1/2 that the process still has open, or one
 * of the 1..3 dup markers -- all the bidirectional /dev/console. */
static short poll_console(task_t *cur, short events, struct poll_table *pt)
{
	short rev = 0;
	tty_t *tty = cur && cur->ctty ? cur->ctty : tty_get_console();

	if (tty)
		poll_wait(pt, NULL, &tty->poll_wq);
	if (events & (POLLIN | POLLRDNORM)) {
		if (tty && tty->read_count > 0)
			rev |= POLLIN | POLLRDNORM;
	}
	if (events & (POLLOUT | POLLWRNORM))
		rev |= POLLOUT | POLLWRNORM;
	return rev;
}

short poll_entry(void *entry, int fd, short events, struct poll_table *pt,
		 int depth)
{
	task_t *cur = sched_current();

	/* The console is an EMPTY fd_table slot at 0/1/2.  It must be
	 * selected by the slot's CONTENT, never by the descriptor number:
	 * deciding by number polled the terminal for a descriptor that had
	 * been redirected elsewhere (this hung sftp-server, whose stdin is an
	 * AF_UNIX socket). */
	if (!entry) {
		if (!cur || fd < 0 || !task_fd_is_console(cur, (uint64_t)fd))
			return POLLNVAL;
		return poll_console(cur, events, pt);
	}

	if (IS_SOCKET_FD(entry)) {
		net_socket_t *ns = sock_get(SOCKET_FD_IDX(entry));

		if (ns)
			poll_wait(pt, entry, &ns->poll_wq);
		return (short)sock_poll(SOCKET_FD_IDX(entry), events);
	}

	if (unix_sock_is(entry)) {
		poll_wait(pt, entry, &((unix_socket_t *)entry)->poll_wq);
		return (short)unix_poll((unix_socket_t *)entry, events);
	}

	/* An epoll set: readable while it has an event to report.  It may
	 * itself watch epoll sets; `depth' bounds how far that is followed. */
	if (IS_EPOLL_FD(entry))
		return ep_eventpoll_poll(EPOLL_FD_IDX(entry), events, pt,
					 depth);

	uintptr_t marker = (uintptr_t)entry;
	if (marker >= 1 && marker <= 3)
		return poll_console(cur, events, pt);

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
		return rev;
	}

	// Pty master (opened via /dev/ptmx): readable when slave wrote bytes,
	// writable when slave is open, HUP when slave closed and buffer empty.
	{
		int pid = devfs_get_pty_master_id((vfs_file_t *)entry);
		if (pid >= 0) {
			poll_wait(pt, entry, tty_pty_master_pollq(pid));
			return (short)tty_pty_master_poll(pid, events);
		}
	}

	// Event devices (/dev/input/eventN): readable when a whole packet is
	// queued for this descriptor, writable while the device exists,
	// POLLHUP|POLLERR once it is gone or the descriptor was revoked.
	{
		struct evdev_client *ec =
			devfs_evdev_client((vfs_file_t *)entry);
		if (ec) {
			poll_wait(pt, entry, evdev_pollq(ec));
			return evdev_poll(ec, events);
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
			return rev;
		}
	}

	// Registered device nodes and anonymous device files: the driver's
	// own poll hook (or always-ready when it has none).
	{
		short rev_dev;
		if (devfs_device_poll((vfs_file_t *)entry, events, pt,
				      &rev_dev) == 0)
			return rev_dev;
	}

	/* A regular file: always ready for read and write, with nothing to
	 * wait on.  epoll refuses such a file (EPERM), as everywhere. */
	if (pt)
		pt->nopoll = 1;
	short rev = 0;
	if (events & (POLLIN | POLLRDNORM))
		rev |= POLLIN | POLLRDNORM;
	if (events & (POLLOUT | POLLWRNORM))
		rev |= POLLOUT | POLLWRNORM;
	return rev;
}

/* One descriptor of a poll()/select() scan.  Held while it is examined:
 * every arm reaches into the object, and a sibling thread closing the
 * descriptor mid-scan would otherwise destroy it underneath. */
static short fd_scan_one(int fd, short events, struct poll_table *pt)
{
	task_t *cur = sched_current();
	void *entry;
	short rev;

	if (!cur || fd < 0 || (unsigned)fd >= TASK_MAX_FDS)
		return POLLNVAL;
	entry = fdget(cur, fd);
	pt->key = (unsigned)(unsigned short)events;
	rev = poll_entry(entry, fd, events, pt, 0);
	if (entry)
		fdput(entry);
	return rev;
}

// ============================================================================
// The lifetime of the object behind an entry, for epoll items
// ============================================================================

int poll_obj_tryhold(void *entry)
{
	uintptr_t marker = (uintptr_t)entry;

	if (!entry)
		return 0;
	if (marker >= 1 && marker <= 3)
		return 1; /* the console: never freed */
	if (IS_SOCKET_FD(entry))
		return sock_tryhold(SOCKET_FD_IDX(entry));
	if (IS_EPOLL_FD(entry))
		return epoll_tryget(EPOLL_FD_IDX(entry));
	/* A UNIX socket's memory is kept by the item itself (see
	 * poll_obj_item_hold); what is asked here is whether any descriptor
	 * still names it. */
	if (unix_sock_is(entry))
		return !poll_obj_closed(entry);
	if (pipe_is_end(entry))
		return pipe_end_tryhold((pipe_end_t *)entry);
	return vfs_tryget((vfs_file_t *)entry);
}

void poll_obj_put(void *entry)
{
	uintptr_t marker = (uintptr_t)entry;

	if (!entry || (marker >= 1 && marker <= 3))
		return;
	if (!IS_SOCKET_FD(entry) && !IS_EPOLL_FD(entry) &&
	    unix_sock_is(entry))
		return; /* nothing was taken, see poll_obj_tryhold */
	fdput((vfs_file_t *)entry);
}

/* What an epoll item keeps.  Every kind of object removes the items watching
 * it from its final release (eventpoll_release) -- except a UNIX socket,
 * whose code has no such hook; for it the item holds the socket's memory
 * (not a descriptor: closing every descriptor still hangs it up), and the set
 * drops the item once poll_obj_closed() says the last descriptor is gone. */
int poll_obj_item_hold(void *entry)
{
	if (!entry || IS_SOCKET_FD(entry) || IS_EPOLL_FD(entry))
		return 1;
	if ((uintptr_t)entry <= 3)
		return 1;
	if (unix_sock_is(entry))
		return unix_sock_lookup_hold((unix_socket_t *)entry) ? 2 : 0;
	return 1;
}

void poll_obj_item_put(void *entry)
{
	if (!entry || IS_SOCKET_FD(entry) || IS_EPOLL_FD(entry) ||
	    (uintptr_t)entry <= 3)
		return;
	if (unix_sock_is(entry))
		unix_sock_put_ref((unix_socket_t *)entry);
}

int poll_obj_closed(void *entry)
{
	if (!entry || IS_SOCKET_FD(entry) || IS_EPOLL_FD(entry) ||
	    (uintptr_t)entry <= 3)
		return 0;
	if (unix_sock_is(entry))
		return __atomic_load_n(&((unix_socket_t *)entry)->ref_count,
				       __ATOMIC_ACQUIRE) <= 0;
	return 0;
}

// ============================================================================
// sys_select_internal - select() implementation
// timeout_ns: 0 = poll (non-blocking), (uint64_t)-1 = block forever
// Returns number of ready fds, or negative errno.
// ============================================================================
/* The body runs with a poll table; this wrapper owns it.  Split so the table
 * is released in exactly ONE place: a queue left holding a pointer into a
 * freed table is a use after free on the next wakeup. */
static int sys_select_locked(int nfds, fd_set *readfds, fd_set *writefds,
			     fd_set *exceptfds, uint64_t timeout_ns,
			     struct poll_table *pt);

int sys_select_internal(int nfds, fd_set *readfds, fd_set *writefds,
			fd_set *exceptfds, uint64_t timeout_ns)
{
	struct poll_table pt;
	int r;

	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;
	/* One queue per descriptor is the rule; a few objects name two (a
	 * sound device), and a table that runs out only costs the tick
	 * re-scan for that wait. */
	poll_table_init(&pt, nfds + 4);
	r = sys_select_locked(nfds, readfds, writefds, exceptfds, timeout_ns,
			      &pt);
	poll_table_free(&pt);
	return r;
}

static int sys_select_locked(int nfds, fd_set *readfds, fd_set *writefds,
			     fd_set *exceptfds, uint64_t timeout_ns,
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

	uint64_t deadline = 0, deadline_ns = 0;
	if (timeout_ns != 0 && timeout_ns != (uint64_t)-1) {
		/* +1 for the tick already in progress: a timeout may fire
		 * late, never early. */
		deadline_ns = poll_deadline_ns(timeout_ns);
		deadline = timer_ticks() + timer_ns_to_ticks(timeout_ns) + 1;
	}

	while (1) {
		/* Off every queue from the previous iteration before going
		 * back on, and the wake flag cleared BEFORE the scan: a wake
		 * from here on is either seen by the scan or leaves the flag
		 * set for the sleep to find. */
		poll_table_reset(pt);
		__atomic_store_n(&pt->triggered, 0, __ATOMIC_SEQ_CST);

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

			short rev = fd_scan_one(fd, events, pt);

			/* The sets as everywhere: readable is data, a
			 * hangup or an error; writable is room or an error;
			 * exceptional is urgent data. */
			if ((rev & (POLLIN | POLLRDNORM | POLLRDBAND |
				    POLLHUP | POLLERR)) &&
			    readfds && FD_ISSET(fd, &r_in)) {
				FD_SET(fd, &r_out);
				count++;
			}
			if ((rev & (POLLOUT | POLLWRNORM | POLLWRBAND |
				    POLLERR)) &&
			    writefds && FD_ISSET(fd, &w_in)) {
				FD_SET(fd, &w_out);
				count++;
			}
			if ((rev & POLLPRI) && exceptfds &&
			    FD_ISSET(fd, &e_in)) {
				FD_SET(fd, &e_out);
				count++;
			}
		}

		if (count > 0 || timeout_ns == 0) {
			if (readfds)
				*readfds = r_out;
			if (writefds)
				*writefds = w_out;
			if (exceptfds)
				*exceptfds = e_out;
			return count;
		}

		if (timeout_ns != (uint64_t)-1 &&
		    hrtimer_now_ns() >= deadline_ns) {
			if (readfds)
				FD_ZERO(readfds);
			if (writefds)
				FD_ZERO(writefds);
			if (exceptfds)
				FD_ZERO(exceptfds);
			return 0;
		}

		/* A deliverable signal must break the wait: a handler only
		 * runs on the way back to user mode (SIGWINCH, SIGCHLD).
		 * signal_pending() ignores masked signals. */
		{
			task_t *_cur = sched_current();
			if (_cur && signal_pending(_cur))
				return -EINTR;
		}

		poll_sleep(deadline, deadline_ns, timeout_ns != (uint64_t)-1,
			   pt);
	}
}

// ============================================================================
// sys_poll_internal - poll() implementation
// timeout_ns: 0 = non-blocking, (uint64_t)-1 = block forever
// Returns number of ready fds, or negative errno.
// ============================================================================
static int sys_poll_locked(struct pollfd *fds, int nfds,
			   uint64_t timeout_ns, struct poll_table *pt);

int sys_poll_internal(struct pollfd *fds, int nfds, uint64_t timeout_ns)
{
	struct poll_table pt;
	int r;

	if (nfds < 0 || !fds)
		return -EINVAL;
	poll_table_init(&pt, nfds + 4);
	r = sys_poll_locked(fds, nfds, timeout_ns, &pt);
	poll_table_free(&pt);
	return r;
}

static int sys_poll_locked(struct pollfd *fds, int nfds,
			   uint64_t timeout_ns, struct poll_table *pt)
{
	WARN_ON(nfds >
		TASK_MAX_FDS); /* poll() with nfds > TASK_MAX_FDS: kernel cannot have that many open fds */

	uint64_t deadline = 0, deadline_ns = 0;
	if (timeout_ns != 0 && timeout_ns != (uint64_t)-1) {
		deadline_ns = poll_deadline_ns(timeout_ns);
		deadline = timer_ticks() + timer_ns_to_ticks(timeout_ns) + 1;
	}

	while (1) {
		poll_table_reset(pt);
		__atomic_store_n(&pt->triggered, 0, __ATOMIC_SEQ_CST);

		int count = 0;

		for (int i = 0; i < nfds; i++) {
			/* A negative descriptor is skipped, revents 0. */
			if (fds[i].fd < 0) {
				fds[i].revents = 0;
				continue;
			}
			short rev = fd_scan_one(fds[i].fd, fds[i].events, pt);
			fds[i].revents = rev;
			if (fds[i].revents != 0)
				count++;
		}

		if (count > 0 || timeout_ns == 0)
			return count;

		if (timeout_ns != (uint64_t)-1 && hrtimer_now_ns() >= deadline_ns)
			return 0;

		{
			task_t *_cur = sched_current();
			if (_cur && signal_pending(_cur))
				return -EINTR;
		}

		poll_sleep(deadline, deadline_ns, timeout_ns != (uint64_t)-1,
			   pt);
	}
}

// ---------------------------------------------------------------------------
// Noinline helpers for syscalls with large stack-allocated buffers, kept out
// of syscall_handler_inner so its frame stays small.
// ---------------------------------------------------------------------------

/* ppoll()/pselect()/epoll_pwait() take a signal mask that must be installed
 * for exactly the duration of the wait and restored afterwards (the "block
 * the signal, then let ppoll unblock it while waiting" idiom; sshd uses it
 * for SIGCHLD).
 *
 * Returns 1 if a mask was installed (the restore happens after signal
 * delivery), 0 if none was supplied, or a negative errno. */
int poll_sigmask_install(uint64_t umask_ptr, kernel_sigset_t *saved)
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
	uint64_t timeout_ns = (uint64_t)-1;
	if (a5 && validate_user_ptr(a5, 16)) {
		uint64_t tv_sec = 0, tv_usec = 0;
		copy_from_user(&tv_sec, (void *)a5, 8);
		copy_from_user(&tv_usec, (void *)(a5 + 8), 8);
		timeout_ns = (tv_sec * 1000000ULL + tv_usec) * 1000ULL;
		if (tv_sec == 0 && tv_usec == 0)
			timeout_ns = 0;
	}
	int ret = sys_select_internal((int)a1, rp, wp, ep, timeout_ns);
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
	uint64_t timeout_ns = (uint64_t)-1;
	if (a5 && validate_user_ptr(a5, 16)) {
		uint64_t tv_sec = 0;
		long tv_nsec = 0;
		copy_from_user(&tv_sec, (void *)a5, 8);
		copy_from_user(&tv_nsec, (void *)(a5 + 8), 8);
		timeout_ns = tv_sec * 1000000000ULL + (uint64_t)tv_nsec;
		if (tv_sec == 0 && tv_nsec == 0)
			timeout_ns = 0;
	}
	kernel_sigset_t saved_mask;
	int have_mask = poll_sigmask_install(a6, &saved_mask);
	if (have_mask < 0)
		return have_mask;
	int ret = sys_select_internal((int)a1, rp, wp, ep, timeout_ns);
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

/* The descriptor set of poll()/ppoll().  The limit is the descriptor table
 * itself (a browser's network process polls hundreds of connections): a
 * small set stays on the stack, a larger one is copied through the heap. */
#define POLL_STACK_FDS 256

struct poll_fds {
	struct pollfd stack[POLL_STACK_FDS];
	struct pollfd *heap;
	struct pollfd *fds;
	size_t sz;
};

static int poll_fds_get(struct poll_fds *pf, uint64_t uptr, int nfds)
{
	pf->heap = NULL;
	pf->fds = pf->stack;
	if (nfds < 0 || nfds > TASK_MAX_FDS)
		return -EINVAL;
	pf->sz = (size_t)nfds * sizeof(struct pollfd);
	if (!validate_user_ptr(uptr, pf->sz))
		return -EFAULT;
	if (nfds > POLL_STACK_FDS) {
		pf->heap = kalloc(pf->sz);
		if (!pf->heap)
			return -ENOMEM;
		pf->fds = pf->heap;
	}
	copy_from_user(pf->fds, (void *)uptr, pf->sz);
	return 0;
}

static void poll_fds_put(struct poll_fds *pf, uint64_t uptr)
{
	copy_to_user((void *)uptr, pf->fds, pf->sz);
	kfree(pf->heap);
	pf->heap = NULL;
}

__attribute__((noinline)) int64_t
sys_poll(uint64_t a1, uint64_t a2, uint64_t a3)
{
	int nfds = (int)a2;
	struct poll_fds pf;
	int r = poll_fds_get(&pf, a1, nfds);
	if (r < 0)
		return r;
	int timeout_ms = (int)(int64_t)a3;
	uint64_t timeout_ns;
	if (timeout_ms < 0)
		timeout_ns = (uint64_t)-1;
	else if (timeout_ms == 0)
		timeout_ns = 0;
	else
		timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
	int ret = sys_poll_internal(pf.fds, nfds, timeout_ns);
	poll_fds_put(&pf, a1);
	return ret;
}

__attribute__((noinline)) int64_t
sys_ppoll(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	int nfds = (int)a2;
	struct poll_fds pf;
	int r = poll_fds_get(&pf, a1, nfds);
	if (r < 0)
		return r;
	uint64_t timeout_ns = (uint64_t)-1;
	if (a3 && validate_user_ptr(a3, 16)) {
		uint64_t tv_sec = 0;
		long tv_nsec = 0;
		copy_from_user(&tv_sec, (void *)a3, 8);
		copy_from_user(&tv_nsec, (void *)(a3 + 8), 8);
		timeout_ns = tv_sec * 1000000000ULL + (uint64_t)tv_nsec;
		if (tv_sec == 0 && tv_nsec == 0)
			timeout_ns = 0;
	}
	kernel_sigset_t saved_mask;
	int have_mask = poll_sigmask_install(a4, &saved_mask);
	if (have_mask < 0) {
		kfree(pf.heap);
		return have_mask;
	}
	int ret = sys_poll_internal(pf.fds, nfds, timeout_ns);
	/* The mask stays installed on purpose; it is put back after signal
	 * delivery (poll_sigmask_restore_pending). */
	(void)saved_mask;
	poll_fds_put(&pf, a1);
	return ret;
}
