// LikeOS-64 UNIX Domain Sockets
#include <kernel/net/net.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/io/tty.h> /* tty_printf for the Ctrl+N table dump */
#include <kernel/uapi/bug.h>
#include <kernel/fs/vfs.h> /* vfs_permission_parent, MAY_* */

// UNIX socket table
static unix_socket_t unix_sockets[MAX_UNIX_SOCKETS];

// Global table lock — serialises slot allocation, path lookup, and peer
// linkage.  Without it, two concurrent socket(AF_UNIX,...) calls can both
// observe the same inactive slot and race on memset/active=1, producing
// two fds that alias the same unix_socket_t.  Subsequent close on either
// fd then reuses the slot under the other task's feet, and a later
// unix_send/recv dereferences a half-initialised struct (peer pointer
// stale, lock state corrupt) — a classic UAF that surfaces as a kernel
// `ret` to a small/garbage RIP after many iterations.
//
// Held only briefly across slot scans and pointer rewires; never held
// across copy_from/to_user, slab_alloc, or busy-waits.
static spinlock_t unix_table_lock = SPINLOCK_INIT("unix_table");

// ============================================================================
// Name helpers
//
// Socket names are raw byte strings, NOT C strings — an abstract name begins
// with NUL and may contain NULs.  There are deliberately no strcmp/strcpy
// helpers here: using them is what collapsed every abstract name to "" and let
// unrelated sockets find each other.
// ============================================================================
static int uds_memcmp(const void *a, const void *b, int n)
{
	const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
	for (int i = 0; i < n; i++)
		if (pa[i] != pb[i])
			return (int)pa[i] - (int)pb[i];
	return 0;
}

/* Decode a sockaddr_un + addrlen into a raw name.
 *
 * Returns the name length in bytes (0 == unnamed) or a negative errno.  On
 * success *name_out points at the first name byte inside addr->sun_path.
 *
 *   addrlen == sizeof(sa_family_t)     -> unnamed (no name supplied)
 *   sun_path[0] == '\0', addrlen > 2   -> abstract; the name is exactly the
 *                                         (addrlen - 2) bytes present, leading
 *                                         NUL included.  An abstract name is
 *                                         NOT NUL-terminated and may contain
 *                                         NULs, so its length can only come
 *                                         from addrlen.  addrlen == 3 gives the
 *                                         empty abstract name (length 1).
 *   sun_path[0] != '\0'                -> pathname; the name is the
 *                                         NUL-TERMINATED string.
 *
 * The pathname length is deliberately taken from the terminator rather than
 * from addrlen: callers legitimately pass either sizeof(struct sockaddr_un)
 * (both tmux and testlibc do) or SUN_LEN-style lengths, and bind/connect must
 * agree on the name regardless of which the caller chose.  Deriving it from
 * addrlen would make those two spellings name different sockets. */
static int uds_name_from_addr(const struct sockaddr_un *addr, socklen_t addrlen,
			      const char **name_out)
{
	*name_out = addr->sun_path;
	if (addrlen < sizeof(sa_family_t) ||
	    addrlen > sizeof(struct sockaddr_un))
		return -EINVAL;

	int avail = (int)(addrlen - sizeof(sa_family_t));
	if (avail <= 0)
		return 0; /* unnamed */

	if (addr->sun_path[0] == '\0')
		return avail; /* abstract: raw bytes, length from addrlen */

	int n = 0;
	while (n < avail && addr->sun_path[n])
		n++;
	if (n == avail)
		return -EINVAL; /* pathname not NUL-terminated within addrlen */
	return n;
}

// ============================================================================
// unix_get - Get unix socket by index (from FD)
// ============================================================================
unix_socket_t *unix_get(int usockfd)
{
	BUG_ON(usockfd < 0);
	int idx = UNIX_SOCKET_FD_IDX(usockfd);
	if (idx < 0 || idx >= MAX_UNIX_SOCKETS)
		return NULL;
	if (!unix_sockets[idx].active)
		return NULL;
	return &unix_sockets[idx];
}

// ============================================================================
// unix_alloc - Allocate a new unix socket slot
// Caller MUST hold unix_table_lock.  Marks the slot active=1 atomically with
// the search so no second caller can claim the same index.
// ============================================================================
static int unix_alloc_locked(void)
{
	for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
		if (!unix_sockets[i].active) {
			// Claim the slot before releasing the table lock.  active=1
			// makes the slot opaque to other allocators and to
			// unix_find_by_path (which also requires bound=1).
			unix_sockets[i].active = 1;
			return i;
		}
	}
	return -1;
}

// ============================================================================
// unix_find_by_path - Find a bound & listening socket by path
// Caller MUST hold unix_table_lock so the returned pointer is stable
// against concurrent close (which clears active/bound under the lock).
// ============================================================================
/* Find a bound socket by RAW NAME.
 *
 * Compares (length, bytes) — never as a C string.  An abstract name starts
 * with NUL and may contain NULs, so strcmp would truncate every one of them to
 * "" and make them all match each other: that is precisely how two unrelated
 * listeners ended up answering to the same name, each holding the other's
 * queued client while both waited forever.
 *
 * Because a pathname can never begin with NUL, comparing raw bytes keeps the
 * pathname and abstract namespaces disjoint for free.
 *
 * len <= 0 means unnamed, which is not reachable by name at all. */
static unix_socket_t *unix_find_by_name_locked(const char *name, int len)
{
	if (!name || len <= 0)
		return NULL;
	for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
		if (!unix_sockets[i].active)
			continue;
		if (!unix_sockets[i].bound)
			continue;
		if (unix_sockets[i].path_len != len)
			continue;
		if (uds_memcmp(unix_sockets[i].path, name, len) == 0)
			return &unix_sockets[i];
	}
	return NULL;
}

// ============================================================================
// unix_create - Create a new UNIX domain socket
// ============================================================================
int unix_create(int type)
{
	might_sleep();
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return -EINVAL;

	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	int idx = unix_alloc_locked();
	if (idx < 0) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return -ENOMEM;
	}

	unix_socket_t *us = &unix_sockets[idx];
	// Zero out the struct, then re-establish active=1 (claimed by alloc).
	// The zeroing is done while we still hold unix_table_lock so that no
	// other allocator/finder can observe the half-zeroed state.
	uint8_t *p = (uint8_t *)us;
	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;

	us->active = 1;
	us->type = type;
	us->ref_count = 1;
	spinlock_init(&us->lock, "unix_sock");

	spin_unlock_irqrestore(&unix_table_lock, tflags);
	return MAKE_UNIX_SOCKET_FD(idx);
}

// ============================================================================
// unix_bind - Bind a UNIX socket to a pathname or abstract name
// ============================================================================
int unix_bind(int usockfd, const struct sockaddr_un *addr, socklen_t addrlen)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;
	if (us->bound)
		return -EINVAL;
	if (!addr || addr->sun_family != AF_UNIX)
		return -EINVAL;

	const char *name;
	int nlen = uds_name_from_addr(addr, addrlen, &name);
	if (nlen < 0)
		return nlen;
	/* Unnamed bind (addrlen carries no sun_path): there is no name to
	 * register, so there is nothing to bind to.  Auto-generating one is not
	 * supported; say so rather than registering the socket under a name
	 * every other unnamed socket would also answer to. */
	if (nlen == 0)
		return -EINVAL;
	if (nlen > UNIX_PATH_MAX)
		return -EINVAL;

	/* A pathname socket occupies a name in the filesystem namespace, so
	 * binding it requires write+search permission on the containing
	 * directory — the same rule as creating a file there.  An abstract name
	 * (leading NUL) has no filesystem presence and is exempt.  The check
	 * runs before the table lock so it never holds a spinlock across the VFS
	 * stat.  (Peer-credential passing / SO_PEERCRED is a follow-up.) */
	if (name[0] != '\0') {
		int pr = vfs_permission_parent(name, MAY_WRITE | MAY_EXEC);
		if (pr != ST_OK)
			return (pr == ST_PERM) ? -EPERM : -EACCES;
	}

	// Name lookup + write of bound=1 must be atomic against other binds
	// and against unix_close clearing bound — otherwise two binds can
	// both see the name free and both succeed, producing duplicates that
	// confuse subsequent unix_find_by_name callers.
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	if (unix_find_by_name_locked(name, nlen)) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return -EADDRINUSE;
	}
	for (int i = 0; i < nlen; i++)
		us->path[i] = name[i];
	us->path_len = nlen;
	us->bound = 1;
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	return 0;
}

// ============================================================================
// unix_listen - Mark a UNIX socket as listening
// ============================================================================
int unix_listen(int usockfd, int backlog)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;
	if (us->type != SOCK_STREAM)
		return -EOPNOTSUPP;
	if (!us->bound)
		return -EINVAL;

	uint64_t flags;
	spin_lock_irqsave(&us->lock, &flags);
	us->listening = 1;
	us->backlog = (backlog > 16) ? 16 : (backlog < 1 ? 1 : backlog);
	spin_unlock_irqrestore(&us->lock, flags);

	return 0;
}

// ============================================================================
// unix_accept - Accept a connection on a listening socket
// ============================================================================
int unix_accept(int usockfd, struct sockaddr_un *addr, socklen_t *addrlen)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;
	if (!us->listening)
		return -EINVAL;

	// Wait for incoming connection.  Re-fetch the queue indices each
	// iteration via volatile reads so the compiler can't hoist them out
	// of the loop, and bail if the listener was closed under us.  We
	// MUST yield rather than just pause: in the common case the task
	// that will enqueue (a forked child running unix_connect) is on the
	// same CPU as the listener, and a tight pause loop here will starve
	// it forever — the test hangs with parent on accept and no child
	// visible because the child never gets CPU.
	task_t *acc_cur = sched_current();
	for (;;) {
		int h = *(volatile int *)&us->accept_head;
		int t = *(volatile int *)&us->accept_tail;
		if (h != t)
			break;
		if (!*(volatile int *)&us->active)
			return -EBADF;
		if (us->nonblock)
			return -EAGAIN;
		/* Interruptible: POSIX accept() returns EINTR; also lets a
		 * pending fatal signal terminate a listener whose client died
		 * before connecting (previously an unkillable forever-wait). */
		if (acc_cur && signal_pending(acc_cur))
			return -EINTR;
		sched_yield_in_kernel();
	}

	uint64_t flags;
	spin_lock_irqsave(&us->lock, &flags);
	unix_socket_t *client = NULL;
	if (us->accept_head != us->accept_tail) {
		int h = us->accept_head;
		if (h >= 0 && h < 16) {
			client = us->accept_queue[h];
			us->accept_queue[h] = NULL;
			us->accept_head = (h + 1) % 16;
		}
	}
	spin_unlock_irqrestore(&us->lock, flags);

	if (!client)
		return -EAGAIN;

	// Allocate server-side socket — must hold table lock across the slot
	// scan + memset + active=1 publish so no concurrent unix_create can
	// alias the same slot (the bug that caused random kernel UAF crashes
	// after many teststress iterations).
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	int new_idx = unix_alloc_locked();
	if (new_idx < 0) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		client->error = ECONNREFUSED;
		client->connected = 0;
		return -ENOMEM;
	}

	unix_socket_t *server = &unix_sockets[new_idx];
	uint8_t *p = (uint8_t *)server;
	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;
	server->active = 1;
	server->type = SOCK_STREAM;
	server->connected = 1;
	server->ref_count = 1;
	server->parent = us;
	spinlock_init(&server->lock, "unix_sock");

	// Link peers under the table lock so close cannot race with the
	// bidirectional pointer write (close clears peer->peer = NULL also
	// under unix_table_lock — see unix_close).
	server->peer = client;
	client->peer = server;
	client->connected = 1;
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	/* Fill addr if requested.  Report the client's own bound name, as raw
	 * bytes plus a matching addrlen — the connecting side is normally
	 * unnamed, which is reported as family-only (addrlen == sizeof
	 * sa_family_t), exactly as an unnamed peer should be. */
	if (addr && addrlen) {
		for (int i = 0; i < UNIX_PATH_MAX; i++)
			addr->sun_path[i] = 0;
		addr->sun_family = AF_UNIX;
		int cl = client->path_len;
		if (cl < 0)
			cl = 0;
		if (cl > UNIX_PATH_MAX)
			cl = UNIX_PATH_MAX;
		for (int i = 0; i < cl; i++)
			addr->sun_path[i] = client->path[i];
		*addrlen = (socklen_t)(sizeof(sa_family_t) + cl);
	}

	return MAKE_UNIX_SOCKET_FD(new_idx);
}

// ============================================================================
// unix_connect - Connect to a bound/listening UNIX socket
// ============================================================================
int unix_connect(int usockfd, const struct sockaddr_un *addr,
		 socklen_t addrlen)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;
	if (us->connected)
		return -EISCONN;
	if (!addr || addr->sun_family != AF_UNIX)
		return -EINVAL;

	const char *name;
	int nlen = uds_name_from_addr(addr, addrlen, &name);
	if (nlen < 0)
		return nlen;
	if (nlen == 0)
		return -EINVAL; /* no name to connect to */

	// Look up listener and enqueue under unix_table_lock so the listener
	// cannot be closed (and its slot reused) between lookup and enqueue.
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	unix_socket_t *listener = unix_find_by_name_locked(name, nlen);
	if (!listener || !listener->listening) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return -ECONNREFUSED;
	}

	uint64_t flags;
	spin_lock_irqsave(&listener->lock, &flags);

	// Check accept queue capacity
	int next = (listener->accept_tail + 1) % 16;
	if (next == listener->accept_head) {
		spin_unlock_irqrestore(&listener->lock, flags);
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return -ECONNREFUSED;
	}

	// Enqueue ourselves
	listener->accept_queue[listener->accept_tail] = us;
	listener->accept_tail = next;
	listener->accept_ready = 1;
	spin_unlock_irqrestore(&listener->lock, flags);
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	// Wait for acceptance (peer link to be set up).  Re-read peer/error
	// each iteration via volatile so the compiler doesn't hoist.  Yield
	// rather than pause so the listener task on another CPU (or this
	// CPU) can actually run accept and link us.
	task_t *con_cur = sched_current();
	while (!*(volatile void **)&us->peer && !*(volatile int *)&us->error) {
		if (us->nonblock)
			return -EINPROGRESS;
		if (!*(volatile int *)&us->active)
			return -EBADF;
		/* Interruptible: POSIX connect() returns EINTR. */
		if (con_cur && signal_pending(con_cur))
			return -EINTR;
		sched_yield_in_kernel();
	}

	if (us->error) {
		int err = us->error;
		us->error = 0;
		return -err;
	}

	return 0;
}

// ============================================================================
// unix_send - Send data on a connected UNIX socket
// ============================================================================
int unix_send(int usockfd, const void *buf, size_t len, int flags)
{
	might_sleep();
	BUG_ON(buf == NULL && len > 0);
	(void)flags;
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;

	// Snapshot peer pointer under unix_table_lock so it cannot be set
	// to a freed-and-reused slot mid-deref.  Once we have a reference
	// (peer->ref_count++), the slot cannot be freed under us even if
	// unix_close clears us->peer afterwards.
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	unix_socket_t *peer = us->peer;
	if (!peer || !peer->active) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		/* Peer already closed our side — return EPIPE (not ENOTCONN) */
		if (us->peer_closed)
			return -EPIPE;
		if (us->type == SOCK_DGRAM)
			return -EDESTADDRREQ;
		return -ENOTCONN;
	}
	__atomic_fetch_add(&peer->ref_count, 1, __ATOMIC_ACQ_REL);
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	if (peer->closed || peer->peer_closed) {
		__atomic_fetch_sub(&peer->ref_count, 1, __ATOMIC_ACQ_REL);
		return -EPIPE;
	}

	const uint8_t *src = (const uint8_t *)buf;
	int sent = 0;
	uint64_t irqflags;

	/* Bounce through a kernel buffer.  User memory must NEVER be touched
	 * while peer->lock is held: a user page can be demand-paged (lazily
	 * mapped text/rodata — a string literal passed to write() is exactly
	 * that — or anon), and a fault taken with interrupts disabled cannot
	 * re-enable them to sleep for the page-in.  The demand-fault path only
	 * sti's when the faulting context had interrupts on (see
	 * exception_handler), so faulting under a spinlock waits for a disk
	 * completion IRQ that can never be delivered on this CPU — wedged
	 * forever, still holding the lock.  Chunked to keep the stack small. */
	uint8_t kbuf[256];

	while ((size_t)sent < len) {
		size_t chunk = len - (size_t)sent;
		if (chunk > sizeof(kbuf))
			chunk = sizeof(kbuf);

		/* Read user memory with NO lock held — safe to fault and sleep. */
		smap_disable();
		for (size_t i = 0; i < chunk; i++)
			kbuf[i] = src[(size_t)sent + i];
		smap_enable();

		/* Push into the peer's ring under its lock — kernel memory only. */
		spin_lock_irqsave(&peer->lock, &irqflags);
		size_t n = 0;
		while (n < chunk) {
			int next = (peer->tail + 1) % (int)sizeof(peer->buf);
			if (next == peer->head)
				break; /* ring full */
			peer->buf[peer->tail] = kbuf[n];
			peer->tail = next;
			peer->bytes_written++;
			n++;
		}
		if (n)
			peer->ready = 1;
		spin_unlock_irqrestore(&peer->lock, irqflags);
		sent += (int)n;

		if (n == chunk)
			continue; /* chunk placed; keep going */

		/* Ring is full. */
		if (sent > 0)
			break; /* partial write — report what we sent */
		if (us->nonblock) {
			__atomic_fetch_sub(&peer->ref_count, 1,
					   __ATOMIC_ACQ_REL);
			return -EAGAIN;
		}
		// Wait for space — yield so the receiver gets CPU and drains.
		while ((peer->tail + 1) % (int)sizeof(peer->buf) ==
		       peer->head) {
			if (peer->closed) {
				__atomic_fetch_sub(&peer->ref_count, 1,
						   __ATOMIC_ACQ_REL);
				return -EPIPE;
			}
			/* Interruptible: send() returns EINTR (no data was
			 * written yet at this point). */
			task_t *snd_cur = sched_current();
			if (snd_cur && signal_pending(snd_cur)) {
				__atomic_fetch_sub(&peer->ref_count, 1,
						   __ATOMIC_ACQ_REL);
				return -EINTR;
			}
			sched_yield_in_kernel();
		}
	}

	// Drop the reference we took above.  If this was the last ref and
	// peer was already closed, unix_close's deferred-free logic (or
	// our own dec dropping ref to 0) will tear it down on next close.
	__atomic_fetch_sub(&peer->ref_count, 1, __ATOMIC_ACQ_REL);

	return sent > 0 ? sent : -EAGAIN;
}

// ============================================================================
// unix_recv - Receive data from a connected UNIX socket
// ============================================================================
int unix_recv(int usockfd, void *buf, size_t len, int flags)
{
	might_sleep();
	BUG_ON(buf == NULL && len > 0);
	(void)flags;
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;

	uint8_t *dst = (uint8_t *)buf;

	// Wait for data.  Re-read indices and peer-state via volatile each
	// iteration so the compiler can't hoist them out of the loop.  Also
	// bail if our own slot is closed under us (e.g. dup'd fd in another
	// task closed twice).
	for (;;) {
		int h = *(volatile int *)&us->head;
		int t = *(volatile int *)&us->tail;
		if (h != t)
			break;
		if (!*(volatile int *)&us->active)
			return -EBADF;
		if (*(volatile int *)&us->peer_closed)
			return 0;
		volatile unix_socket_t *const *peer_slot =
			(volatile unix_socket_t *const *)&us->peer;
		unix_socket_t *p = (unix_socket_t *)*peer_slot;
		if (p && *(volatile int *)&p->closed)
			return 0;
		if (!us->connected && us->type == SOCK_STREAM)
			return -ENOTCONN;
		if (us->nonblock)
			return -EAGAIN;
		/* Interruptible: recv() returns EINTR when a signal is pending. */
		{
			task_t *rcv_cur = sched_current();
			if (rcv_cur && signal_pending(rcv_cur))
				return -EINTR;
		}
		sched_yield_in_kernel();
	}

	uint64_t irqflags;
	int received = 0;

	/* Same rule as unix_send: never touch user memory under us->lock.  Drain
	 * the ring into a kernel buffer with the lock held, then copy out to the
	 * user with it released, where a demand fault is free to sleep. */
	uint8_t kbuf[256];

	while ((size_t)received < len) {
		size_t chunk = len - (size_t)received;
		if (chunk > sizeof(kbuf))
			chunk = sizeof(kbuf);

		spin_lock_irqsave(&us->lock, &irqflags);
		size_t n = 0;
		while (n < chunk && us->head != us->tail) {
			kbuf[n++] = us->buf[us->head];
			us->head = (us->head + 1) % (int)sizeof(us->buf);
			us->bytes_read++;
		}
		if (us->head == us->tail)
			us->ready = 0;
		spin_unlock_irqrestore(&us->lock, irqflags);

		if (n == 0)
			break; /* ring empty */

		/* Copy to user with NO lock held — safe to fault and sleep. */
		smap_disable();
		for (size_t i = 0; i < n; i++)
			dst[(size_t)received + i] = kbuf[i];
		smap_enable();

		received += (int)n;
		if (n < chunk)
			break; /* ring drained */
	}

	return received;
}

// ============================================================================
// unix_close - Close a UNIX domain socket
// ============================================================================
int unix_close(int usockfd)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;

	int old = __atomic_fetch_sub(&us->ref_count, 1, __ATOMIC_ACQ_REL);
	WARN_ON(old <= 0); /* unix_socket ref_count underflow */
	if (old > 1)
		return 0;

	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);

	// If a unix_send between dec and lock acquire took a reference, it
	// bumped ref_count back above 0.  Defer teardown to the caller that
	// drops the last ref; we just return.
	if (__atomic_load_n(&us->ref_count, __ATOMIC_ACQUIRE) > 0) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return 0;
	}

	uint64_t flags;
	spin_lock_irqsave(&us->lock, &flags);

	us->closed = 1;

	// Notify peer.  Both peer-link writes happen under unix_table_lock
	// so a concurrent unix_send observing us->peer cannot capture the
	// pointer just before peer is freed.
	unix_socket_t *peer = us->peer;
	if (peer) {
		peer->peer_closed = 1;
		peer->ready = 1; // Wake up readers
		peer->peer = NULL;
	}

	us->active = 0;
	us->bound = 0;
	us->listening = 0;
	us->connected = 0;
	us->peer = NULL;
	us->head = 0;
	us->tail = 0;
	us->path_len = 0; /* name released with the slot */

	spin_unlock_irqrestore(&us->lock, flags);
	spin_unlock_irqrestore(&unix_table_lock, tflags);
	return 0;
}

// ============================================================================
// unix_socketpair - Create a pair of connected UNIX domain sockets
// ============================================================================
int unix_socketpair(int type, int sv[2])
{
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return -EINVAL;

	// Both slot allocations + memsets + active=1 publish must happen
	// under unix_table_lock so no concurrent unix_create can alias the
	// same slot.
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	int idx0 = unix_alloc_locked();
	if (idx0 < 0) {
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return -ENOMEM;
	}

	int idx1 = unix_alloc_locked();
	if (idx1 < 0) {
		unix_sockets[idx0].active = 0;
		spin_unlock_irqrestore(&unix_table_lock, tflags);
		return -ENOMEM;
	}

	unix_socket_t *s0 = &unix_sockets[idx0];
	unix_socket_t *s1 = &unix_sockets[idx1];

	// Initialize s0
	uint8_t *p = (uint8_t *)s0;
	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;
	s0->active = 1;
	s0->type = type;
	s0->connected = 1;
	s0->ref_count = 1;
	spinlock_init(&s0->lock, "unix_sock");

	// Initialize s1
	p = (uint8_t *)s1;
	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;
	s1->active = 1;
	s1->type = type;
	s1->connected = 1;
	s1->ref_count = 1;
	spinlock_init(&s1->lock, "unix_sock");

	// Link peers
	s0->peer = s1;
	s1->peer = s0;
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	sv[0] = MAKE_UNIX_SOCKET_FD(idx0);
	sv[1] = MAKE_UNIX_SOCKET_FD(idx1);

	return 0;
}

// ============================================================================
// unix_shutdown - Shutdown part of a UNIX socket connection
// ============================================================================
int unix_shutdown(int usockfd, int how)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return -EBADF;
	if (!us->connected)
		return -ENOTCONN;

	if (how == SHUT_WR || how == SHUT_RDWR) {
		// Stop writing — signal peer
		if (us->peer) {
			us->peer->peer_closed = 1;
			us->peer->ready = 1;
		}
	}
	if (how == SHUT_RD || how == SHUT_RDWR) {
		// Stop reading
		us->peer_closed = 1;
	}

	return 0;
}

// ============================================================================
// unix_poll - Poll a UNIX socket for events
// ============================================================================
int unix_poll(int usockfd, short events)
{
	unix_socket_t *us = unix_get(usockfd);
	if (!us)
		return 0;

	short revents = 0;

	if (events & POLLIN) {
		if (us->head != us->tail)
			revents |= POLLIN;
		if (us->peer_closed || (us->peer && us->peer->closed))
			revents |= POLLIN | POLLHUP;
		if (us->listening && us->accept_head != us->accept_tail)
			revents |= POLLIN;
	}

	if (events & POLLOUT) {
		if (us->peer && !us->peer->closed) {
			int next = (us->peer->tail + 1) %
				   (int)sizeof(us->peer->buf);
			if (next != us->peer->head)
				revents |= POLLOUT;
		}
		if (!us->peer || us->peer->closed)
			revents |= POLLHUP;
	}

	if (us->closed)
		revents |= POLLHUP;
	if (us->error)
		revents |= POLLERR;

	return revents;
}

// ============================================================================
// SCM_RIGHTS file-descriptor passing
// ============================================================================
// Per-socket FIFO of pending in-band file descriptors.  Sender pushes the
// fd_table entry (an opaque void* — could be a vfs_file_t*, a socket
// marker, a pipe end, or a stdio marker 1..3) onto the *peer's* queue
// after taking the appropriate reference.  Receiver pops the head entry
// in recvmsg and installs it in its own fd_table.
//
// The queue is small (16) because the imsg framing tmux uses sends at
// most one fd per message and the peer drains promptly.  When full we
// return -EAGAIN so the sender can retry; this preserves ordering with
// data bytes already accepted by unix_send.
int unix_push_fd(unix_socket_t *sock, void *entry)
{
	if (!sock)
		return -EBADF;
	uint64_t flags;
	spin_lock_irqsave(&sock->lock, &flags);
	int next = (sock->pending_fd_tail + 1) % 16;
	if (next == sock->pending_fd_head) {
		spin_unlock_irqrestore(&sock->lock, flags);
		return -EAGAIN;
	}
	sock->pending_fds[sock->pending_fd_tail] = entry;
	/* Associate this fd with the byte offset that immediately precedes
     * the data the sender is about to push.  recvmsg uses this to clamp
     * the byte count it returns so the cmsg lines up with the right
     * imsg frame on the peer side. */
	sock->pending_fd_off[sock->pending_fd_tail] = sock->bytes_written;
	sock->pending_fd_tail = next;
	spin_unlock_irqrestore(&sock->lock, flags);
	return 0;
}

int unix_pop_fd(unix_socket_t *sock, void **out_entry)
{
	if (!sock || !out_entry)
		return -EINVAL;
	uint64_t flags;
	spin_lock_irqsave(&sock->lock, &flags);
	if (sock->pending_fd_head == sock->pending_fd_tail) {
		spin_unlock_irqrestore(&sock->lock, flags);
		return -EAGAIN;
	}
	*out_entry = sock->pending_fds[sock->pending_fd_head];
	sock->pending_fd_head = (sock->pending_fd_head + 1) % 16;
	spin_unlock_irqrestore(&sock->lock, flags);
	return 0;
}

int unix_peek_fd_offset(unix_socket_t *sock, uint64_t *out_off)
{
	if (!sock || !out_off)
		return -EINVAL;
	uint64_t flags;
	spin_lock_irqsave(&sock->lock, &flags);
	if (sock->pending_fd_head == sock->pending_fd_tail) {
		spin_unlock_irqrestore(&sock->lock, flags);
		return -EAGAIN;
	}
	*out_off = sock->pending_fd_off[sock->pending_fd_head];
	spin_unlock_irqrestore(&sock->lock, flags);
	return 0;
}

// ============================================================================
// unix_dump_sockets - On-demand AF_UNIX table snapshot for the Ctrl+N dump.
//
// Lock-free best-effort read, exactly like tcp_dump_table: this runs from the
// keyboard interrupt handler, so it must never take unix_table_lock (a holder
// on another CPU would wedge the box).  Values may tear; it is purely
// diagnostic and never used for correctness.
//
// Reading a hang: a listener stuck in accept() has lsn=1 with ah==at (nothing
// queued), while its client stuck in connect() has peer=-1 (never linked).  If
// both are true at once, the client enqueued into a DIFFERENT socket than the
// listener is polling — compare the two `path` columns and the u<idx> of each.
// ============================================================================
void unix_dump_sockets(struct tty *tty)
{
	tty_printf(tty, "=== AF_UNIX table ===\n");
	for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
		unix_socket_t *u = &unix_sockets[i];
		if (!*(volatile int *)&u->active)
			continue;
		unix_socket_t *peer = (unix_socket_t *)*(volatile unix_socket_t *
							 *const *)&u->peer;
		unix_socket_t *par = (unix_socket_t *)*(volatile unix_socket_t *
						       *const *)&u->parent;
		/* Render the raw name.  An abstract name starts with NUL and is
		 * not a C string, so print it as @<rest> with NULs shown as '.'
		 * rather than handing printf a string that stops at byte 0. */
		char nm[UNIX_PATH_MAX + 2];
		int nlen = u->path_len;
		if (nlen < 0)
			nlen = 0;
		if (nlen > UNIX_PATH_MAX)
			nlen = UNIX_PATH_MAX;
		if (nlen == 0) {
			nm[0] = '-';
			nm[1] = '\0';
		} else if (u->path[0] == '\0') {
			nm[0] = '@';
			for (int j = 1; j < nlen; j++)
				nm[j] = u->path[j] ? u->path[j] : '.';
			nm[nlen] = '\0';
		} else {
			for (int j = 0; j < nlen; j++)
				nm[j] = u->path[j];
			nm[nlen] = '\0';
		}
		tty_printf(
			tty,
			"u%d ref=%d ty=%d bnd=%d lsn=%d con=%d cls=%d pcls=%d ah=%d at=%d h=%d t=%d peer=%d par=%d nlen=%d name=%s\n",
			i, u->ref_count, u->type, u->bound, u->listening,
			u->connected, u->closed, u->peer_closed,
			u->accept_head, u->accept_tail, u->head, u->tail,
			peer ? (int)(peer - unix_sockets) : -1,
			par ? (int)(par - unix_sockets) : -1, nlen, nm);
	}
	tty_printf(tty, "=====================\n");
}
