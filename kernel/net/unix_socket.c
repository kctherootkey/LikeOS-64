// LikeOS-64 UNIX Domain Sockets
#include <kernel/ke/waitq.h>
#include <kernel/net/net.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/io/tty.h> /* tty_printf for the Ctrl+N table dump */
#include <kernel/uapi/bug.h>
#include <kernel/fs/vfs.h> /* vfs_permission_parent, MAY_* */
#include <kernel/fs/file.h>

/* Wake tasks blocked in select()/poll()/epoll_wait().
 *
 * A blocking read() or write() on a socket parks on the socket itself and is
 * released by sched_wake_channel() below.  A task multiplexing with select()
 * cannot park there -- it is waiting on many descriptors at once -- so it puts
 * itself on the poll wait queue of every socket it asked about, and
 * poll_notify_wq() on THIS socket is what releases it.  Without that call such
 * a task slept until the poll layer's one-tick fallback expired: 10 ms per
 * event at the 100 Hz tick.
 *
 * That is the cost of one X11 round trip, and an X client makes hundreds of
 * them while starting up -- the display server sits in select() on this
 * transport, so every request it was sent waited a tick before it was even
 * looked at.  It is why an Xlib program took seconds to appear while a
 * request-batching xcb one did not.
 */
extern void poll_notify_wq(struct wait_queue_head *);
extern void poll_notify_io_ready(void);

/* Reference counting.  Defined together further down, next to the close path
 * they serve; see the block comment there for the rules. */
static void unix_hold(unix_socket_t *us);
static int unix_tryhold(unix_socket_t *us);
static void unix_put(unix_socket_t *us);
static void unix_hangup_and_put(unix_socket_t *s);

// UNIX socket table
static unix_socket_t unix_sockets[MAX_UNIX_SOCKETS];

/* Size of a connected socket's data ring.
 *
 * Large enough that a client pushing a screenful of pixels does not stall on
 * every few kilobytes: each stall costs a context switch, and an X client
 * repainting a window makes thousands of them.  Allocated per connected
 * socket, so an idle system pays nothing for it. */
#define UNIX_RING_SIZE 65536

/* "UNXSCK64" -- 64 bits so it cannot collide with the low half of a kernel
 * pointer, which is what sits at offset 0 of a vfs_file. */
#define UNIX_SOCK_MAGIC 0x554E5853434B3634ULL
static uint64_t s_unix_next_id = 1;

/* Give `s` a data ring if it has none.
 *
 * Allocated OUTSIDE the socket lock -- kalloc may sleep -- and installed under
 * it, so two senders racing to first-use the same peer cannot both install one.
 * The loser frees its spare and uses the winner's. */
static int unix_ring_ensure(unix_socket_t *s)
{
	uint8_t *nb;
	uint64_t f;

	if (!s)
		return -EINVAL;
	if (__atomic_load_n(&s->buf, __ATOMIC_ACQUIRE))
		return 0;

	nb = (uint8_t *)kalloc(UNIX_RING_SIZE);
	if (!nb)
		return -ENOMEM;

	spin_lock_irqsave(&s->lock, &f);
	if (s->buf == NULL) {
		s->buf = nb;
		s->bufsz = UNIX_RING_SIZE;
		s->head = 0;
		s->tail = 0;
		nb = NULL; /* installed */
	}
	spin_unlock_irqrestore(&s->lock, f);

	if (nb)
		kfree(nb); /* lost the race; the winner's ring is in place */
	return 0;
}

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

	/* The path runs to the first NUL, or to the end of the declared length
	 * if there is no NUL in it.  An unterminated path is NOT an error: the
	 * standard way to compute addrlen is
	 *
	 *     SUN_LEN(p) == offsetof(struct sockaddr_un, sun_path)
	 *                   + strlen((p)->sun_path)
	 *
	 * which deliberately does not count the terminator, so a caller using
	 * it declares exactly strlen bytes of path.  Rejecting that rejects the
	 * conventional spelling: it is what X11's xtrans uses, and it made the
	 * display server fail at bind() with EINVAL, reported four layers up as
	 * "Cannot establish any listening sockets".
	 *
	 * The name is returned as a (pointer, length) pair precisely because it
	 * may not be terminated; callers that need a C string must copy it. */
	int n = 0;
	while (n < avail && addr->sun_path[n])
		n++;
	return n;
}

/*
 * unix_getname - fill in a sockaddr_un for this socket or its peer.
 *
 * Needed by more than curiosity: X clients ask the socket what it is before
 * choosing an authorisation record.  libxcb calls getpeername(), falls back to
 * getsockname(), and if BOTH fail it gives up and sends no authorisation at
 * all -- the server then refuses the connection with "Authorization required,
 * but no authorization protocol specified", which names neither call.
 *
 * `peer` selects which end: 0 for this socket's own name, 1 for the name of
 * the socket it is connected to.
 *
 * An unnamed socket is not an error.  A client that connect()s without
 * bind() has no name, and the correct answer is a sockaddr_un with the family
 * set and no path -- length sizeof(sa_family_t).  That is what tells the
 * caller "this is a Unix socket" even when there is nothing else to say.
 *
 * *addrlen is in/out: in, the size of the caller's buffer; out, the size the
 * address ACTUALLY needs.  A caller whose buffer was too small gets a
 * truncated address and a length larger than it passed, which is how it knows
 * to try again with a bigger one (libxcb does exactly this).
 */
int unix_getname(unix_socket_t *us, int peer, struct sockaddr_un *addr,
		 socklen_t *addrlen)
{
	const char *name;
	socklen_t have, need;
	int nlen;

	if (!us)
		return -EBADF;
	if (!addr || !addrlen)
		return -EFAULT;

	if (peer) {
		/* The copy taken when the connection was made, NOT `us->peer`.
		 * A socket that is still open must be able to name the peer it
		 * was connected to even after that peer has closed, which is
		 * exactly when callers tend to ask. */
		if (!us->peer_valid)
			return -ENOTCONN;
		name = us->peer_path;
		nlen = us->peer_path_len;
	} else {
		name = us->path;
		nlen = us->path_len;
	}

	have = *addrlen;
	if (nlen < 0)
		nlen = 0;
	if (nlen > UNIX_PATH_MAX)
		nlen = UNIX_PATH_MAX;

	/* The declared length counts the family and the name, and NOT a
	 * terminator -- the same SUN_LEN convention the caller uses on the way
	 * in (see uds_name_from_addr). */
	need = (socklen_t)(sizeof(sa_family_t) + (socklen_t)nlen);

	addr->sun_family = AF_UNIX;
	for (int i = 0; i < UNIX_PATH_MAX; i++)
		addr->sun_path[i] = '\0';
	for (int i = 0; i < nlen; i++)
		addr->sun_path[i] = name[i];

	*addrlen = need;
	(void)have; /* truncation is the caller's business, not an error */
	return 0;
}

// ============================================================================
// unix_get - Get unix socket by index (from FD)
// ============================================================================
/* Is this descriptor-table value a UNIX socket?
 *
 * Safe to call on ANY value the table can hold.  The small tagged integers --
 * console streams, network sockets, epoll handles -- are rejected numerically
 * before anything is dereferenced, and the pointer is range-checked before its
 * first word is read.  The magic is 64 bits so it cannot collide with the low
 * half of a pointer, which is what a vfs_file keeps at offset 0. */
bool unix_sock_is(const void *p)
{
	if (!p)
		return false;
	if (!kptr_plausible((uint64_t)(uintptr_t)p))
		return false;
	return ((const unix_socket_t *)p)->magic == UNIX_SOCK_MAGIC;
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
/* Fill in `c' from the calling process.
 *
 * The THREAD GROUP id, not the thread id: SO_PEERCRED names the process on the
 * other end, and a peer that happens to have done its connect() from a worker
 * thread is still the same process.  The credentials are the effective ones,
 * which is what a permission decision on the far side would use. */
static void unix_cred_of_current(struct ucred *c)
{
	task_t *cur = sched_current();

	if (!cur) {
		/* No task context: the kernel itself.  Reported as pid 0,
		 * root, which is what it is. */
		c->pid = 0;
		c->uid = 0;
		c->gid = 0;
		return;
	}
	c->pid = cur->tgid ? cur->tgid : cur->id;
	c->uid = cur->cred.euid;
	c->gid = cur->cred.egid;
}

int unix_create(int type, unix_socket_t **out)
{
	might_sleep();
	if (type != SOCK_STREAM && type != SOCK_DGRAM &&
	    type != SOCK_SEQPACKET)
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
	/* The poll queue survives the slot being reused -- see
	 * wq_head_init_once(). */
	struct wait_queue_head saved_wq = us->poll_wq;
	uint8_t *p = (uint8_t *)us;

	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;
	us->poll_wq = saved_wq;

	us->active = 1;
	us->type = type;
	us->ref_count = 1;
	refcount_set(&us->refcount, 1);
	us->magic = UNIX_SOCK_MAGIC;
	us->id = s_unix_next_id++;
	unix_cred_of_current(&us->self_cred);
	spinlock_init(&us->lock, "unix_sock");
	wq_head_init_once(&us->poll_wq, "unix-poll");

	spin_unlock_irqrestore(&unix_table_lock, tflags);
	*out = us;
	return 0;
}

// ============================================================================
// unix_bind - Bind a UNIX socket to a pathname or abstract name
// ============================================================================
int unix_bind(unix_socket_t *us, const struct sockaddr_un *addr,
	      socklen_t addrlen)
{
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

	/* The VFS takes C strings, and `name` points into the caller's address
	 * structure where it may run to the end of the declared length with no
	 * terminator (see uds_name_from_addr).  Copy it out before the path
	 * reaches anything that expects a string. */
	char pathbuf[UNIX_PATH_MAX + 1];

	for (int i = 0; i < nlen; i++)
		pathbuf[i] = name[i];
	pathbuf[nlen] = '\0';

	/* A pathname socket occupies a name in the filesystem namespace, so
	 * binding it requires write+search permission on the containing
	 * directory — the same rule as creating a file there.  An abstract name
	 * (leading NUL) has no filesystem presence and is exempt.  The check
	 * runs before the table lock so it never holds a spinlock across the VFS
	 * stat. */
	if (name[0] != '\0') {
		int pr = vfs_permission_parent(pathbuf, MAY_WRITE | MAY_EXEC);
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
	us->has_node = 0;
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	/* A pathname bind must leave a real socket node behind.  Clients stat()
	 * the path and refuse to connect unless it reports S_IFSOCK, so a name
	 * that exists only in this table is unreachable from another process.
	 *
	 * Done AFTER dropping the table lock: creating the node is filesystem
	 * I/O that can block, and this lock is held with interrupts disabled.
	 * Abstract names have no filesystem presence and skip all of it. */
	if (name[0] != '\0') {
		/* 0777 minus the caller's umask, as for any other name a
		 * process creates.  It used to be a flat 0777, which made every
		 * bound socket world-writable -- and since connecting requires
		 * write permission on the node, that silently granted every
		 * user access to every service on the system.  A server that
		 * genuinely wants to be reachable by everyone (the X server
		 * does) chmods the node itself afterwards. */
		unsigned int smode = 0777 & ~task_umask(sched_current());
		int mr = vfs_mknod(pathbuf, S_IFSOCK | smode);
		if (mr != ST_OK) {
			/* Undo the registration so the name does not stay
			 * claimed by a socket nobody can reach. */
			spin_lock_irqsave(&unix_table_lock, &tflags);
			us->path_len = 0;
			us->bound = 0;
			spin_unlock_irqrestore(&unix_table_lock, tflags);
			if (mr == ST_EXISTS)
				return -EADDRINUSE;
			if (mr == ST_ACCESS)
				return -EACCES;
			if (mr == ST_PERM)
				return -EPERM;
			if (mr == ST_ROFS)
				return -EROFS;
			if (mr == ST_UNSUPPORTED)
				return 0; /* fs cannot hold nodes: name still bound */
			return -EIO;
		}
		us->has_node = 1;
	}

	return 0;
}

// ============================================================================
// unix_listen - Mark a UNIX socket as listening
// ============================================================================
int unix_listen(unix_socket_t *us, int backlog)
{
	if (!us)
		return -EBADF;
	/* The two connection-oriented types.  SEQPACKET listens and accepts
	 * exactly like a stream; only the data transfer differs. */
	if (us->type != SOCK_STREAM && us->type != SOCK_SEQPACKET)
		return -EOPNOTSUPP;
	if (!us->bound)
		return -EINVAL;

	uint64_t flags;
	spin_lock_irqsave(&us->lock, &flags);
	us->listening = 1;
	us->backlog = (backlog > 16) ? 16 : (backlog < 1 ? 1 : backlog);
	/* Refreshed here as well as at creation: a server that drops
	 * privileges between socket() and listen() should be reported as what
	 * it became, not as what it was. */
	unix_cred_of_current(&us->self_cred);
	spin_unlock_irqrestore(&us->lock, flags);

	return 0;
}

// ============================================================================
// unix_accept - Accept a connection on a listening socket
// ============================================================================
int unix_accept(unix_socket_t *us, struct sockaddr_un *addr, socklen_t *addrlen,
		unix_socket_t **out)
{
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
	unix_socket_t *server = NULL;
	if (us->accept_head != us->accept_tail) {
		int h = us->accept_head;
		if (h >= 0 && h < 16) {
			server = us->accept_queue[h];
			us->accept_queue[h] = NULL;
			us->accept_head = (h + 1) % 16;
		}
	}
	spin_unlock_irqrestore(&us->lock, flags);

	if (!server)
		return -EAGAIN;

	/* The dequeued entry carries the reference its queue slot held, and it
	 * is an ALREADY ESTABLISHED connection: connect() built both ends and
	 * linked them before making it visible here.  This function no longer
	 * creates anything -- it hands over what is already there, and the
	 * queue's reference becomes the new descriptor's.
	 *
	 * A connection whose client went away while it waited is skipped
	 * rather than handed out: it would be dead on arrival, and its
	 * teardown still has to happen. */
	if (server->closed) {
		unix_hangup_and_put(server);
		return -EAGAIN;
	}

	/* Fill addr if requested.  Report the client's own bound name, as raw
	 * bytes plus a matching addrlen — the connecting side is normally
	 * unnamed, which is reported as family-only (addrlen == sizeof
	 * sa_family_t), exactly as an unnamed peer should be. */
	if (addr && addrlen) {
		for (int i = 0; i < UNIX_PATH_MAX; i++)
			addr->sun_path[i] = 0;
		addr->sun_family = AF_UNIX;
		/* From the copy recorded at connect time, not by following the
		 * peer: the client may already have closed. */
		int cl = server->peer_path_len;

		if (cl < 0)
			cl = 0;
		if (cl > UNIX_PATH_MAX)
			cl = UNIX_PATH_MAX;
		for (int i = 0; i < cl; i++)
			addr->sun_path[i] = server->peer_path[i];
		*addrlen = (socklen_t)(sizeof(sa_family_t) + cl);
	}

	*out = server;
	return 0;
}

// ============================================================================
// unix_connect - Connect to a bound/listening UNIX socket
// ============================================================================
int unix_connect(unix_socket_t *us, const struct sockaddr_un *addr,
		 socklen_t addrlen)
{
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
	if (nlen > UNIX_PATH_MAX)
		return -EINVAL;

	/* Connecting to a pathname socket requires WRITE permission on the
	 * node, which is the rule everywhere else and was missing entirely:
	 * any user could connect to any bound socket regardless of its mode or
	 * owner.  An abstract name (leading NUL) has no filesystem presence and
	 * is exempt, as it is for bind.
	 *
	 * Checked here, before the table lock is taken: the VFS lookup can
	 * block, and that lock is held with interrupts disabled. */
	if (name[0] != '\0') {
		char cpathbuf[UNIX_PATH_MAX + 1];
		int pr;

		for (int i = 0; i < nlen; i++)
			cpathbuf[i] = name[i];
		cpathbuf[nlen] = '\0';
		pr = vfs_permission(cpathbuf, MAY_WRITE);
		if (pr != ST_OK)
			return (pr == ST_PERM) ? -EPERM : -EACCES;
	}

	/* Build the connection here, and hand the SERVER end to the listener.
	 *
	 * What this replaces queued the CLIENT and then waited for accept() to
	 * build the other end and link the two.  That made connect() depend on
	 * the listener getting round to accepting, so a program that connects
	 * and accepts on one thread -- a legitimate thing to do, and what a
	 * self-connection test does -- deadlocked: connect waited for an
	 * accept that could not run.  It also meant a half-built connection
	 * was visible to accept() before either end was linked.
	 *
	 * Conventionally connect() completes as soon as the connection is
	 * queued; accept() merely hands an established connection to the
	 * application.  So the server end is created here, the pair is linked
	 * in one step, and only then does it become visible to accept().
	 */
	unix_socket_t *server = NULL;
	unix_socket_t *listener = NULL;
	uint64_t tflags, flags;

	/* Take a reference on the listener before letting go of the table
	 * lock: everything below can sleep. */
	spin_lock_irqsave(&unix_table_lock, &tflags);
	listener = unix_find_by_name_locked(name, nlen);
	if (listener && !listener->listening)
		listener = NULL;
	if (listener && !unix_tryhold(listener))
		listener = NULL;
	spin_unlock_irqrestore(&unix_table_lock, tflags);
	if (!listener)
		return -ECONNREFUSED;

	/* A connection only makes sense between sockets speaking the same
	 * protocol: a byte stream wired to a record socket would lose the
	 * record boundaries one side is counting on.  The conventional error
	 * for asking is EPROTOTYPE. */
	if (listener->type != us->type) {
		unix_put(listener);
		return -EPROTOTYPE;
	}

	/* The rings are obtained with no lock held -- allocating can sleep. */
	if (unix_ring_ensure(us) != 0) {
		unix_put(listener);
		return -ENOMEM;
	}

	spin_lock_irqsave(&unix_table_lock, &tflags);
	{
		int new_idx = unix_alloc_locked();

		if (new_idx < 0) {
			spin_unlock_irqrestore(&unix_table_lock, tflags);
			unix_put(listener);
			return -ENOMEM;
		}
		server = &unix_sockets[new_idx];
		uint8_t *p = (uint8_t *)server;

		for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
			p[i] = 0;
		server->active = 1;
		/* The child speaks whatever the listener speaks: an accepted
		 * SEQPACKET connection keeps its record boundaries. */
		server->type = us->type;
		server->magic = UNIX_SOCK_MAGIC;
		server->id = s_unix_next_id++;
		/* One descriptor's worth, and one reference: accept() hands
		 * both to the caller.  Until then the queue holds the
		 * reference on the listener's behalf. */
		server->ref_count = 1;
		refcount_set(&server->refcount, 1);
		server->parent = listener;
		spinlock_init(&server->lock, "unix_sock");

		/* The accepted end reports the listener's name: it is the
		 * address the client connected TO.  `bound` stays clear -- the
		 * name belongs to the listener, this socket merely reports it,
		 * and a lookup only considers bound sockets. */
		{
			int pl = listener->path_len;

			if (pl < 0)
				pl = 0;
			if (pl > UNIX_PATH_MAX)
				pl = UNIX_PATH_MAX;
			for (int i = 0; i < pl; i++)
				server->path[i] = listener->path[i];
			server->path_len = pl;
		}

		/* Link both directions in one step, under the lock where the
		 * link is also severed.  Each direction holds a reference, so
		 * neither end can be destroyed while the other points at it. */
		server->peer = us;
		us->peer = server;
		unix_hold(us); /* for server->peer */
		unix_hold(server); /* for us->peer */
		server->connected = 1;
		us->connected = 1;

		/* Exchange credentials, once, here.
		 *
		 * The client's own are refreshed first: connect() is the
		 * moment it commits, and a process that dropped privileges
		 * after socket() must be reported as what it is now.  The
		 * server end inherits the LISTENER's, not the accepting
		 * thread's -- the accepted socket did not exist when the
		 * connection was made, and it is the listening process the
		 * client reached. */
		unix_cred_of_current(&us->self_cred);
		server->self_cred = listener->self_cred;
		us->peer_cred = listener->self_cred;
		server->peer_cred = us->self_cred;
		us->has_peer_cred = 1;
		server->has_peer_cred = 1;

		/* Each end's view of the other, recorded now and never chased
		 * again -- getpeername() must keep answering after the far end
		 * closes. */
		{
			int cl = us->path_len;

			if (cl < 0)
				cl = 0;
			if (cl > UNIX_PATH_MAX)
				cl = UNIX_PATH_MAX;
			for (int i = 0; i < cl; i++)
				server->peer_path[i] = us->path[i];
			server->peer_path_len = cl;
			server->peer_valid = 1;

			for (int i = 0; i < server->path_len; i++)
				us->peer_path[i] = server->path[i];
			us->peer_path_len = server->path_len;
			us->peer_valid = 1;
		}
	}
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	/* Publish it to the listener LAST, so accept() can never see a
	 * half-built connection.  A full backlog is refused rather than
	 * waited on, which is what this driver has always done. */
	spin_lock_irqsave(&listener->lock, &flags);
	{
		int next = (listener->accept_tail + 1) % 16;

		if (!listener->listening || next == listener->accept_head) {
			spin_unlock_irqrestore(&listener->lock, flags);
			/* Unwind: tear the pair down again and give the
			 * caller its socket back as it was.
			 *
			 * Severing the link stamps peer_closed on this end --
			 * correct for a connection that existed and ended, and
			 * wrong here, where none ever did.  Left set, the
			 * caller's next poll() reports a hangup and its next
			 * read() an end of file, on a socket that is simply
			 * unconnected.  connect() failed; the socket must be
			 * as good as new. */
			unix_hangup_and_put(server);
			spin_lock_irqsave(&us->lock, &flags);
			us->peer_closed = 0;
			us->connected = 0;
			us->ready = 0;
			spin_unlock_irqrestore(&us->lock, flags);
			unix_put(listener);
			return -ECONNREFUSED;
		}
		listener->accept_queue[listener->accept_tail] = server;
		listener->accept_tail = next;
		listener->accept_ready = 1;
	}
	spin_unlock_irqrestore(&listener->lock, flags);

	/* A listener multiplexing with select() -- which is what a display
	 * server does -- is waiting on its own queue, not on this socket. */
	sched_wake_channel(listener);
	poll_notify_wq(&listener->poll_wq); /* now acceptable */
	poll_notify_wq(&us->poll_wq); /* now connected, so writable */
	unix_put(listener);
	return 0;
}

/* ============================================================================
 * SEQPACKET record transfer.
 *
 * A SOCK_SEQPACKET connection is set up exactly like a stream -- listen,
 * accept, one peer, one ring -- but the data keeps its message boundaries:
 * each send is one record, each recv returns exactly one record (truncating
 * if the caller's buffer is smaller), and records never coalesce or split.
 * WebKit's process pairs speak this; it is what socketpair(AF_UNIX,
 * SOCK_SEQPACKET) is for.
 *
 * The framing is a 4-byte little-endian length ahead of each record in the
 * same byte ring the stream path uses.  A record goes into the ring in ONE
 * critical section -- header and payload together, or not at all -- so a
 * header in the ring proves its whole record is behind it, and the reader
 * never has to wait mid-record.  bytes_written/bytes_read count header and
 * payload both, on both sides, so the SCM_RIGHTS offset framing keeps
 * working unchanged: a descriptor attached before a send carries the offset
 * of that record's header.
 * ==========================================================================*/

/* One whole record into the peer's ring, or nothing.  The peer is held by
 * the caller.  Returns the payload length sent, or a negative error.
 *
 * fd_entries/nfds are in-band descriptors that belong to THIS record, and
 * they go into the peer's pending-fd queue inside the same critical section
 * that writes the record bytes -- all of it or none of it.  They used to be
 * pushed by the sendmsg layer BEFORE this was called, which broke two ways:
 *   - a full ring parks here, and a signal then returns EINTR with the fds
 *     already queued; the caller retries the sendmsg (that is what EINTR
 *     means) and the same descriptors were pushed AGAIN.  The receiver's
 *     descriptor stream gained an extra fd, and every later record's
 *     attachment resolved one descriptor off -- a multiplexed IPC peer
 *     (WebKit's process pairs) decoded garbage from that point on.
 *   - a full pending-fd queue was reported by unix_push_fd as -EAGAIN, and
 *     the sendmsg layer discarded that result, silently sending the record
 *     with its descriptors missing.
 * Doing both inserts under one lock makes the retry path safe (a failed
 * call queued nothing) and turns queue-full into the same wait the ring
 * uses. */
static int unix_send_record(unix_socket_t *us, unix_socket_t *peer,
			    const void *buf, size_t len,
			    void **fd_entries, int nfds)
{
	uint64_t irqflags;

	/* The record must fit the ring whole, with its header, and with one
	 * slot spare (a completely full ring is indistinguishable from an
	 * empty one).  Bigger is not "try later", it is "never": EMSGSIZE. */
	if (len > (size_t)(UNIX_RING_SIZE - 8))
		return -EMSGSIZE;
	if (nfds < 0 || nfds > UNIX_PENDING_FDS - 1)
		return -EINVAL; /* can never fit, waiting will not help */

	/* Bounce the payload through kernel memory first: user memory must
	 * never be touched under a socket lock (see unix_send), and the
	 * insert below has to be one atomic critical section. */
	uint8_t *kbuf = NULL;
	if (len) {
		kbuf = (uint8_t *)kalloc(len);
		if (!kbuf)
			return -ENOMEM;
		const uint8_t *src = (const uint8_t *)buf;
		smap_disable();
		for (size_t i = 0; i < len; i++)
			kbuf[i] = src[i];
		smap_enable();
	}

	if (unix_ring_ensure(peer) != 0) {
		if (kbuf)
			kfree(kbuf);
		return -ENOMEM;
	}

	size_t need = 4 + len;
	for (;;) {
		spin_lock_irqsave(&peer->lock, &irqflags);
		if (peer->closed) {
			spin_unlock_irqrestore(&peer->lock, irqflags);
			if (kbuf)
				kfree(kbuf);
			return -EPIPE;
		}
		size_t free_space = (size_t)((peer->head - peer->tail - 1 +
					      peer->bufsz) %
					     peer->bufsz);
		int fd_free = (peer->pending_fd_head - peer->pending_fd_tail -
			       1 + UNIX_PENDING_FDS) %
			      UNIX_PENDING_FDS;
		if (free_space >= need && fd_free >= nfds) {
			/* The record's start offset, which is what recvmsg
			 * matches pending descriptors against -- captured
			 * before the header advances it. */
			uint64_t rec_off = peer->bytes_written;
			/* Header, then payload, all under the one lock. */
			for (int i = 0; i < 4; i++) {
				peer->buf[peer->tail] =
					(uint8_t)((len >> (8 * i)) & 0xFF);
				peer->tail = (peer->tail + 1) % peer->bufsz;
			}
			for (size_t i = 0; i < len; i++) {
				peer->buf[peer->tail] = kbuf[i];
				peer->tail = (peer->tail + 1) % peer->bufsz;
			}
			/* This record's descriptors, same critical section.
			 * Space was checked above; peer->lock is what
			 * unix_push_fd would take, so push inline. */
			for (int i = 0; i < nfds; i++) {
				peer->pending_fds[peer->pending_fd_tail] =
					fd_entries[i];
				peer->pending_fd_off[peer->pending_fd_tail] =
					rec_off;
				peer->pending_fd_tail =
					(peer->pending_fd_tail + 1) %
					UNIX_PENDING_FDS;
			}
			peer->bytes_written += need;
			peer->ready = 1;
			spin_unlock_irqrestore(&peer->lock, irqflags);
			sched_wake_channel(peer);
			poll_notify_wq(&peer->poll_wq);
			if (kbuf)
				kfree(kbuf);
			return (int)len;
		}
		/* Not enough room for the whole record.  Nothing has been
		 * written, so returning EAGAIN or EINTR here is always
		 * clean. */
		if (us->nonblock) {
			spin_unlock_irqrestore(&peer->lock, irqflags);
			if (kbuf)
				kfree(kbuf);
			return -EAGAIN;
		}
		task_t *snd_cur = sched_current();
		if (snd_cur && signal_pending(snd_cur)) {
			spin_unlock_irqrestore(&peer->lock, irqflags);
			if (kbuf)
				kfree(kbuf);
			return -EINTR;
		}
		if (!snd_cur) {
			spin_unlock_irqrestore(&peer->lock, irqflags);
			sched_yield_in_kernel();
			continue;
		}
		/* Park on the peer, exactly as the stream path does; the
		 * reader wakes us after draining. */
		snd_cur->wait_channel = peer;
		snd_cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&peer->lock, irqflags);
		sched_schedule();
		snd_cur->wait_channel = NULL;
	}
}

/* sendmsg(2)'s entry: one SEQPACKET record and its descriptors, atomically.
 * Peer resolution mirrors unix_send. */
int unix_send_record_msg(unix_socket_t *us, const void *buf, size_t len,
			 void **fd_entries, int nfds)
{
	might_sleep();
	if (!us)
		return -EBADF;
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	unix_socket_t *peer = us->peer;
	if (peer && !unix_tryhold(peer))
		peer = NULL;
	spin_unlock_irqrestore(&unix_table_lock, tflags);
	if (!peer) {
		if (us->peer_closed)
			return -EPIPE;
		return -ENOTCONN;
	}
	if (peer->closed || peer->peer_closed) {
		unix_put(peer);
		return -EPIPE;
	}
	int r = unix_send_record(us, peer, buf, len, fd_entries, nfds);
	unix_put(peer);
	return r;
}

/* One whole record out of this socket's ring.  Data is known to be present
 * unless another reader raced us to it, in which case -ENODATA asks the
 * caller to go back to waiting.  Returns the number of payload bytes copied
 * out (the record truncates silently to the caller's buffer, per the
 * conventional SEQPACKET semantics). */
static int unix_recv_record(unix_socket_t *us, uint8_t *dst, size_t len)
{
	uint64_t irqflags;
	size_t cap = len;
	if (cap > UNIX_RING_SIZE)
		cap = UNIX_RING_SIZE;

	/* Sized to the caller's buffer, not the record: bytes past the
	 * caller's buffer are discarded either way, so they are never copied
	 * anywhere.  Allocated before the lock -- kalloc can sleep. */
	uint8_t *kbuf = NULL;
	if (cap) {
		kbuf = (uint8_t *)kalloc(cap);
		if (!kbuf)
			return -ENOMEM;
	}

	spin_lock_irqsave(&us->lock, &irqflags);
	size_t used = (size_t)((us->tail - us->head + us->bufsz) % us->bufsz);
	if (used < 4) {
		/* Another reader consumed it between the wait and here. */
		spin_unlock_irqrestore(&us->lock, irqflags);
		if (kbuf)
			kfree(kbuf);
		return -ENODATA;
	}
	uint32_t reclen = 0;
	for (int i = 0; i < 4; i++) {
		reclen |= (uint32_t)us->buf[us->head] << (8 * i);
		us->head = (us->head + 1) % us->bufsz;
	}
	/* The insert was atomic, so the whole record is here; anything else
	 * means the ring was corrupted. */
	if ((size_t)reclen + 4 > used) {
		WARN_ON(1);
		spin_unlock_irqrestore(&us->lock, irqflags);
		if (kbuf)
			kfree(kbuf);
		return -EIO;
	}
	size_t take = reclen;
	if (take > cap)
		take = cap;
	for (size_t i = 0; i < take; i++) {
		kbuf[i] = us->buf[us->head];
		us->head = (us->head + 1) % us->bufsz;
	}
	/* Discard whatever the caller's buffer could not hold: the record
	 * boundary, not the byte count, is the unit here. */
	us->head = (int)((us->head + (reclen - take)) % (size_t)us->bufsz);
	us->bytes_read += 4 + reclen;
	if (us->head == us->tail)
		us->ready = 0;
	spin_unlock_irqrestore(&us->lock, irqflags);

	/* Space was freed: wake a sender parked on us, and tell the peer's
	 * pollers it is writable again -- same idiom as the stream path. */
	sched_wake_channel(us);
	{
		unix_socket_t *pw = NULL;
		uint64_t pf;

		spin_lock_irqsave(&us->lock, &pf);
		if (us->peer) {
			pw = us->peer;
			unix_hold(pw);
		}
		spin_unlock_irqrestore(&us->lock, pf);
		if (pw) {
			poll_notify_wq(&pw->poll_wq);
			unix_put(pw);
		}
	}

	if (take) {
		smap_disable();
		for (size_t i = 0; i < take; i++)
			dst[i] = kbuf[i];
		smap_enable();
	}
	if (kbuf)
		kfree(kbuf);
	return (int)take;
}

int unix_send(unix_socket_t *us, const void *buf, size_t len, int flags)
{
	might_sleep();
	BUG_ON(buf == NULL && len > 0);
	(void)flags;
	if (!us)
		return -EBADF;

	/* Take a reference on the peer and keep it for the whole call.
	 *
	 * The pointer is read under the table lock, which is where the peer
	 * link is severed, so it cannot be captured just as the link is being
	 * torn down; and the reference then keeps that socket in existence
	 * until this send is finished with it, however soon its own descriptors
	 * go away.  What is being prevented is not the peer closing -- that is
	 * ordinary and reported as EPIPE -- but the slot being handed to a
	 * different socket while this call is still writing into it. */
	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);
	unix_socket_t *peer = us->peer;
	if (peer && !unix_tryhold(peer))
		peer = NULL; /* already being destroyed */
	spin_unlock_irqrestore(&unix_table_lock, tflags);
	if (!peer) {
		/* Peer already closed our side — return EPIPE (not ENOTCONN) */
		if (us->peer_closed)
			return -EPIPE;
		if (us->type == SOCK_DGRAM)
			return -EDESTADDRREQ;
		return -ENOTCONN;
	}

	if (peer->closed || peer->peer_closed) {
		unix_put(peer);
		return -EPIPE;
	}

	if (us->type == SOCK_SEQPACKET) {
		int r = unix_send_record(us, peer, buf, len, NULL, 0);
		unix_put(peer);
		return r;
	}

	const uint8_t *src = (const uint8_t *)buf;
	int sent = 0;
	uint64_t irqflags;
	/* Set when this call queued data, cleared when the poll layer has been
	 * told.  Not done per chunk: the loop runs once per 256 bytes, and a
	 * large write would otherwise sweep the run queue hundreds of times to
	 * announce the same thing.  Once per call is enough, plus once more
	 * before parking -- a poller that has not been told will not read, and
	 * this task is waiting for it to read. */
	int poll_pending = 0;

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

		/* The peer needs a ring before anything can be put in it.
		 * Done here, outside every lock, because it may sleep. */
		if (unix_ring_ensure(peer) != 0) {
			unix_put(peer);
			return sent > 0 ? sent : -ENOMEM;
		}

		/* Push into the peer's ring under its lock — kernel memory only. */
		spin_lock_irqsave(&peer->lock, &irqflags);
		size_t n = 0;
		while (n < chunk) {
			int next = (peer->tail + 1) % peer->bufsz;
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
		/* Wake a reader parked on this socket.  OUTSIDE the lock: the
		 * wake path takes scheduler locks of its own. */
		if (n) {
			sched_wake_channel(peer);
			poll_pending = 1;
		}
		sent += (int)n;

		if (n == chunk)
			continue; /* chunk placed; keep going */

		/* Ring is full.
		 *
		 * A BLOCKING stream socket must not stop here.  It used to:
		 * once anything had been placed, a full ring ended the call
		 * and the count written was returned, which is a short write
		 * on a socket whose caller was never told to expect one.
		 * POSIX permits that; Linux does not do it, and the ports in
		 * this tree are written against what Linux does -- its
		 * unix_stream_sendmsg() sleeps and carries on, and returns
		 * less than asked only when a signal cuts a transfer that had
		 * already begun.
		 *
		 * What it cost: luakit's IPC channel is unbuffered and passes
		 * NULL for the bytes-written out-parameter
		 * (g_io_channel_write_chars in common/ipc.c), so a short write
		 * is invisible to it and the tail of the message is simply
		 * dropped.  The receiver's framing then reads the NEXT
		 * message's bytes as the remainder of this one and the stream
		 * is desynchronised for good -- surfacing far away, in
		 * lua_deserialize_value, as a type byte matching no case and
		 * the assertion that a value was pushed failing by zero.
		 *
		 * It needed no huge message: the ring is 64K shared with
		 * whatever the reader has not consumed, so a reader kept busy
		 * -- rendering an advertisement-heavy page, say -- leaves a
		 * few free bytes and truncates the next small message.  That
		 * is why this only ever showed up on heavy sites.
		 *
		 * O_NONBLOCK is the one case where a short write IS the
		 * contract, and it keeps the old behaviour. */
		if (us->nonblock) {
			if (sent > 0)
				break; /* what fitted; the caller expects this */
			unix_put(peer);
			return -EAGAIN;
		}
		/* Wait for the reader to make space.
		 *
		 * PARKED, not spun.  This used to sched_yield in a loop, which
		 * burns a timeslice per turn and leaves the sender runnable the
		 * whole time -- with an X client pushing pixels that is most of
		 * the CPU, and it is why the display felt slow.  Now the sender
		 * sleeps on the socket and the reader wakes it after draining.
		 *
		 * The condition is re-tested under the lock after waking: a
		 * wake is a hint that something changed, never a promise that
		 * space is still there when this task runs again. */
		{
			task_t *snd_cur = sched_current();

			if (poll_pending) {
				poll_notify_wq(&peer->poll_wq);
				poll_pending = 0;
			}

			spin_lock_irqsave(&peer->lock, &irqflags);
			while ((peer->tail + 1) % peer->bufsz == peer->head) {
				if (peer->closed) {
					spin_unlock_irqrestore(&peer->lock,
							       irqflags);
					unix_put(peer);
					/* Same rule as the signal below: data
					 * already placed is reported, and the
					 * next call is the one that says
					 * EPIPE. */
					return sent > 0 ? sent : -EPIPE;
				}
				/* Interruptible.  EINTR only when nothing has
				 * gone yet: once bytes are in the peer's ring
				 * they cannot be taken back, so a signal that
				 * arrives mid-transfer reports the count --
				 * telling the caller EINTR after moving data
				 * would have it send those bytes twice. */
				if (snd_cur && signal_pending(snd_cur)) {
					spin_unlock_irqrestore(&peer->lock,
							       irqflags);
					unix_put(peer);
					return sent > 0 ? sent : -EINTR;
				}
				if (!snd_cur) {
					spin_unlock_irqrestore(&peer->lock,
							       irqflags);
					sched_yield_in_kernel();
					spin_lock_irqsave(&peer->lock,
							  &irqflags);
					continue;
				}
				snd_cur->wait_channel = peer;
				snd_cur->state = TASK_BLOCKED;
				spin_unlock_irqrestore(&peer->lock, irqflags);
				sched_schedule();
				spin_lock_irqsave(&peer->lock, &irqflags);
				/* sched_schedule() left us RUNNING; do not
				 * overwrite the state here. */
				snd_cur->wait_channel = NULL;
			}
			spin_unlock_irqrestore(&peer->lock, irqflags);
		}
	}

	if (poll_pending)
		poll_notify_wq(&peer->poll_wq);

	/* Let go of the peer.  If its descriptors have gone in the meantime,
	 * this is the last reference and the socket is destroyed here -- which
	 * is the point of holding one: the ring being written into above could
	 * not be freed while this call was still using it. */
	unix_put(peer);

	return sent > 0 ? sent : -EAGAIN;
}

// ============================================================================
// unix_recv - Receive data from a connected UNIX socket
// ============================================================================
int unix_recv(unix_socket_t *us, void *buf, size_t len, int flags)
{
	might_sleep();
	BUG_ON(buf == NULL && len > 0);
	if (!us)
		return -EBADF;

	/* MSG_DONTWAIT makes this one call non-blocking without touching the
	 * socket's own O_NONBLOCK state.  recvmsg() filling a second iovec
	 * needs it: the first one may block waiting for the stream to start,
	 * but once any byte has been handed over the call must return what it
	 * has rather than waiting for more to arrive. */
	int dontwait = (flags & MSG_DONTWAIT) ? 1 : 0;

	uint8_t *dst = (uint8_t *)buf;

restart:
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
		if (*(volatile int *)&us->shut_rd)
			return 0; /* this end asked for no more reads */
		/* End of file is decided from THIS socket's own state and
		 * nothing else.
		 *
		 * What stood here chased us->peer and returned 0 if that socket
		 * looked closed -- with no lock and nothing keeping the peer in
		 * existence.  A peer whose slot had been recycled read as
		 * closed whenever its new occupant was, and a live reader was
		 * handed a clean end of file on a connection that was still
		 * open.  read() returning 0 sets no errno, so a program that
		 * trusted it exited quietly with whatever errno happened to be
		 * lying around -- which is precisely how a graphical client
		 * vanished mid-session reporting "Success".
		 *
		 * peer_closed above is the honest signal: the peer's own
		 * teardown sets it, under the lock, on the socket it is
		 * hanging up.  Nothing has to be inferred by looking. */
		if (!us->connected && us->type != SOCK_DGRAM)
			return -ENOTCONN;
		if (us->nonblock || dontwait)
			return -EAGAIN;
		/* Wait for data.
		 *
		 * PARKED, not spun: the old loop yielded and re-checked, which
		 * kept an idle reader permanently runnable.  Every X client has
		 * one of these blocked on its connection, so the waste was
		 * per-client and constant. */
		{
			task_t *rcv_cur = sched_current();
			uint64_t rf;

			if (rcv_cur && signal_pending(rcv_cur))
				return -EINTR;
			if (!rcv_cur) {
				sched_yield_in_kernel();
				continue;
			}
			spin_lock_irqsave(&us->lock, &rf);
			/* Re-test under the lock: data or a close may have
			 * arrived between the check above and here, and a
			 * sleeper that misses it would never be woken. */
			if (us->head == us->tail && !us->peer_closed &&
			    !us->closed) {
				rcv_cur->wait_channel = us;
				rcv_cur->state = TASK_BLOCKED;
				spin_unlock_irqrestore(&us->lock, rf);
				sched_schedule();
				rcv_cur->wait_channel = NULL;
			} else {
				spin_unlock_irqrestore(&us->lock, rf);
			}
		}
	}

	/* Record sockets hand over exactly one record.  -ENODATA means a
	 * sibling reader took it between the wait above and the pop; going
	 * back to the wait is correct and rare. */
	if (us->type == SOCK_SEQPACKET) {
		int r = unix_recv_record(us, dst, len);
		if (r == -ENODATA)
			goto restart;
		return r;
	}

	uint64_t irqflags;
	int received = 0;
	/* Space was freed for a sender; announce it to select()/poll() once for
	 * the whole call rather than once per 256-byte chunk. */
	int poll_freed_space = 0;

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
			us->head = (us->head + 1) % us->bufsz;
			us->bytes_read++;
		}
		if (us->head == us->tail)
			us->ready = 0;
		spin_unlock_irqrestore(&us->lock, irqflags);

		/* Space was freed: wake anyone blocked trying to send to us.
		 * Outside the lock, as on the send side. */
		if (n) {
			sched_wake_channel(us);
			poll_freed_space = 1;
		}

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

	if (poll_freed_space) {
		/* Space in THIS socket's ring is what makes the PEER writable,
		 * so the peer's queue is the one to wake.  Read under the lock
		 * and pinned across the wake: ->peer must never be chased
		 * unlocked, because a slot that has been recycled is a
		 * different socket by then. */
		unix_socket_t *pw = NULL;
		uint64_t pf;

		spin_lock_irqsave(&us->lock, &pf);
		if (us->peer) {
			pw = us->peer;
			unix_hold(pw);
		}
		spin_unlock_irqrestore(&us->lock, pf);
		if (pw) {
			poll_notify_wq(&pw->poll_wq);
			unix_put(pw);
		}
	}

	return received;
}

/* Mark a socket dead and tell its peer.  Caller holds unix_table_lock and
 * us->lock; returns the peer so it can be woken once they are dropped.
 *
 * Split out from releasing the ring and the slot because the two have entirely
 * different constraints.  This half must happen the instant the descriptor is
 * closed, whatever else is in flight -- it is the only thing that makes the
 * peer's poll() report a hangup.  The other half has to wait for any sender
 * still writing into the ring.
 */
/* The peer link is a pair of references, one in each direction, and severing
 * it drops both.  The one this socket held on its peer is handed to the caller
 * along with the pointer -- it is what keeps the peer alive to be woken.  The
 * one the peer held on THIS socket has no such carrier, so *drop_self says it
 * is owed, to be paid once the locks are dropped. */
static unix_socket_t *unix_mark_closed_locked(unix_socket_t *us, int *drop_self)
{
	unix_socket_t *peer = us->peer;

	*drop_self = 0;
	us->closed = 1;
	if (peer) {
		peer->peer_closed = 1;
		peer->ready = 1; /* release readers parked on it */
		peer->peer = NULL;
		*drop_self = 1; /* peer->peer no longer references us */
	}
	us->peer = NULL;
	us->connected = 0;
	return peer;
}

/* ---- Reference counting -------------------------------------------------
 *
 * The slot a socket occupies is released when the last reference goes, and not
 * before.  That is the whole of it, and it is what makes a bare
 * unix_socket_t * a stable identity: while anyone holds one, the memory cannot
 * become a different socket.
 *
 * Before this, the only counter was of DESCRIPTORS, and everything else --
 * the peer link, entries queued on a listener, an operation in flight -- kept
 * a raw pointer with nothing behind it.  A socket that closed while one of
 * those pointers existed freed its slot, the next socket created took it, and
 * the holder of the stale pointer then read and wrote a stranger's state.
 * Nothing faulted, because the array is always mapped; what happened instead
 * was that an unrelated connection was told its peer had hung up.
 *
 * Rules:
 *   - unix_hold() needs a reference already held, so it cannot resurrect.
 *   - unix_tryhold() is for a pointer found by searching the table; it
 *     refuses a socket already at zero and must be called with the table
 *     lock held, which is what makes "found" and "still alive" one decision.
 *   - unix_put() must be called with NO unix lock held: the last one frees
 *     the ring, and kfree cannot run with interrupts off.  Callers that drop
 *     a reference while holding a lock stash the pointer and put it after.
 */
static void unix_hold(unix_socket_t *us)
{
	BUG_ON(us == NULL);
	refcount_inc(&us->refcount);
}

static int unix_tryhold(unix_socket_t *us)
{
	lockdep_assert_held(&unix_table_lock);
	if (!us)
		return 0;
	return refcount_inc_not_zero(&us->refcount);
}

static void unix_put(unix_socket_t *us)
{
	uint64_t tflags, flags;
	uint8_t *dead_ring = NULL;

	if (!us)
		return;
	if (!refcount_dec_and_test(&us->refcount))
		return;

	/* Last reference gone, so nothing can reach this socket again: a
	 * lookup by name holds the table lock and refuses a zero count, and
	 * the descriptors, the peer link and every queue entry have all let
	 * go.  Only now does the slot go back. */
	/* Descriptors sent over this socket and never received belong to it,
	 * and are closed with it.
	 *
	 * Nothing did that: an in-band descriptor is queued with its reference
	 * already taken, and only a receiver ever took one off the queue.  A
	 * socket that closed with fds still queued -- the far end exited, or
	 * simply never read them -- left every one of those references held
	 * for the rest of the boot.  For a program that passes descriptors
	 * routinely, that is a file, a pipe or another socket kept alive by
	 * nobody, and the process that appeared to close it never finding out.
	 *
	 * Collected under the lock and released after it: releasing can close
	 * a socket or a pipe of its own, which takes locks and can free. */
	void *dead_fds[UNIX_PENDING_FDS];
	int n_dead = 0;

	spin_lock_irqsave(&unix_table_lock, &tflags);
	spin_lock_irqsave(&us->lock, &flags);
	WARN_ON(us->peer != NULL); /* destroyed still linked to a peer */
	WARN_ON(us->accept_head !=
		us->accept_tail); /* destroyed with connections queued */
	while (us->pending_fd_head != us->pending_fd_tail &&
	       n_dead < UNIX_PENDING_FDS) {
		dead_fds[n_dead++] = us->pending_fds[us->pending_fd_head];
		us->pending_fds[us->pending_fd_head] = NULL;
		us->pending_fd_head = (us->pending_fd_head + 1) % UNIX_PENDING_FDS;
	}
	dead_ring = us->buf;
	us->buf = NULL;
	us->bufsz = 0;
	us->head = 0;
	us->tail = 0;
	us->bound = 0;
	us->listening = 0;
	us->active = 0;
	/* Last, and before the slot can be handed out again: a stale pointer
	 * to this socket now fails identification rather than passing it. */
	us->magic = 0;
	spin_unlock_irqrestore(&us->lock, flags);
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	for (int i = 0; i < n_dead; i++)
		fd_release_entry((vfs_file_t *)dead_fds[i]);

	if (dead_ring)
		kfree(dead_ring);
}

/* Queue an in-band descriptor for this socket's peer.
 *
 * The peer is found and pinned in here, rather than by the caller.  The
 * syscall layer used to read us->peer with no lock and no reference and then
 * hand the pointer on to be used later -- by which time it could name a
 * different socket entirely.  Which peer a socket has is this layer's
 * business, and so is keeping it alive for the length of the operation.
 */
int unix_send_fd(unix_socket_t *us, void *entry)
{
	uint64_t tflags;
	unix_socket_t *peer;
	int r;

	if (!us || !entry)
		return -EINVAL;

	spin_lock_irqsave(&unix_table_lock, &tflags);
	peer = us->peer;
	if (peer && !unix_tryhold(peer))
		peer = NULL;
	spin_unlock_irqrestore(&unix_table_lock, tflags);
	if (!peer)
		return -ENOTCONN;

	r = unix_push_fd(peer, entry);
	unix_put(peer);
	return r;
}

/* Resolve a descriptor marker to its socket AND take a reference, so the
 * caller can go on using the pointer even if the descriptor it came from is
 * closed underneath it.
 *
 * Without this, every syscall resolved its descriptor to a bare pointer and
 * then used it for the length of the call.  A sibling thread closing that same
 * descriptor in the meantime dropped the last reference and destroyed the
 * socket while the first thread was still working on it -- the one hole left
 * in the lifetime model once the peer, the queues and the descriptors all held
 * references of their own.
 *
 * Taken under the table lock and refusing a socket already at zero, so
 * "found" and "still alive" are one decision. */
unix_socket_t *unix_sock_lookup_hold(unix_socket_t *us)
{
	uint64_t flags;

	if (!us)
		return NULL;
	spin_lock_irqsave(&unix_table_lock, &flags);
	if (!unix_tryhold(us))
		us = NULL;
	spin_unlock_irqrestore(&unix_table_lock, flags);
	return us;
}

/* Release a reference taken by unix_sock_lookup_hold().  Must be called with
 * no unix lock held -- the last one destroys the socket. */
void unix_sock_put_ref(unix_socket_t *us)
{
	unix_put(us);
}

/* Tear a connection down from one end and release the caller's reference.
 *
 * The same two halves as closing a descriptor -- publish the hangup so the
 * peer's poll() and read() report it, then let go -- for the cases where there
 * is no descriptor to close: a connection built by connect() that the listener
 * never accepted, and one queued on a listener that is going away. */
static void unix_hangup_and_put(unix_socket_t *s)
{
	uint64_t tflags, flags;
	unix_socket_t *peer;
	int drop_self = 0;

	if (!s)
		return;

	spin_lock_irqsave(&unix_table_lock, &tflags);
	spin_lock_irqsave(&s->lock, &flags);
	peer = unix_mark_closed_locked(s, &drop_self);
	spin_unlock_irqrestore(&s->lock, flags);
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	sched_wake_channel(s);
	if (peer) {
		sched_wake_channel(peer);
		unix_put(peer); /* the reference s->peer held */
	}
	/* Before the puts below, not after: the last of them frees the socket,
	 * and the queue being woken lives inside it. */
	poll_notify_wq(&s->poll_wq);
	if (peer)
		poll_notify_wq(&peer->poll_wq);
	if (drop_self)
		unix_put(s); /* the reference peer->peer held on s */
	unix_put(s); /* the caller's own */
}

void unix_sock_fdget(unix_socket_t *us)
{
	uint64_t flags;

	if (!us)
		return;
	spin_lock_irqsave(&us->lock, &flags);
	us->ref_count++;
	spin_unlock_irqrestore(&us->lock, flags);
	unix_hold(us);
}

// ============================================================================
// unix_close - Close a UNIX domain socket
// ============================================================================
int unix_close(unix_socket_t *us)
{
	if (!us)
		return -EBADF;

	/* One descriptor of possibly several.  dup(), and every fork() that
	 * inherits the descriptor table, counts here -- so this is NOT the last
	 * reference merely because one close arrived, and until it is the
	 * socket must carry on exactly as before.  Hanging up here instead was
	 * enough to stop the X server accepting connections: xkbcomp exits
	 * moments after the server forks it, and its inherited copy of the
	 * server's descriptors is closed on the way out. */
	uint64_t flags;
	spin_lock_irqsave(&us->lock, &flags);
	int old = us->ref_count--;
	WARN_ON(old <= 0); /* unix_socket ref_count underflow */
	spin_unlock_irqrestore(&us->lock, flags);
	if (old > 1) {
		/* Still other descriptors: this close only gives back the
		 * reference that this one descriptor held. */
		unix_put(us);
		return 0;
	}

	uint64_t tflags;
	spin_lock_irqsave(&unix_table_lock, &tflags);

	spin_lock_irqsave(&us->lock, &flags);

	/* Last descriptor gone: mark it dead and tell the peer NOW, whether or
	 * not a sender is still inside unix_send().
	 *
	 * Both peer-link writes happen under unix_table_lock so a concurrent
	 * unix_send observing us->peer cannot capture the pointer just before
	 * peer is freed.
	 *
	 * This used to be skipped whenever a sender was inside unix_send(),
	 * deferring the whole teardown "to the caller that drops the last ref"
	 * -- and no sender ever did it.  The socket stayed marked open for
	 * good, and since unix_poll() reports a hangup from peer_closed, the
	 * PEER never saw one either.  A display server polling a client that
	 * had already exited kept that client's window mapped and unresponsive
	 * for the rest of the session; a server blocked in unix_send() to it
	 * waited for ring space nobody would ever free.  Both happened only
	 * when a send was in flight as the socket closed -- which is exactly
	 * what closing a window is, since the request to close is a message
	 * sent to the client. */
	int drop_self = 0;
	unix_socket_t *peer = unix_mark_closed_locked(us, &drop_self);

	/* The ring and the slot are a different matter, and they are no longer
	 * decided here at all: whoever still holds a reference -- a sender
	 * writing into the ring, a listener holding this in its queue -- keeps
	 * the socket alive, and the last of them to let go destroys it.  What
	 * used to stand here was a hand-rolled version of that for the one case
	 * that had been noticed, and it could not cover the others. */

	/* Anything still queued on this socket belongs to it and must be
	 * released with it.  A listener closing with connections waiting used
	 * to abandon them: nothing ever removed a queue entry except accept(),
	 * so the reference was lost and the slot never came back. */
	unix_socket_t *orphans[16];
	int n_orphans = 0;
	while (us->accept_head != us->accept_tail) {
		unix_socket_t *q = us->accept_queue[us->accept_head];

		us->accept_queue[us->accept_head] = NULL;
		us->accept_head = (us->accept_head + 1) % 16;
		if (q && n_orphans < 16)
			orphans[n_orphans++] = q;
	}

	/* Capture the pathname before releasing the slot: the node has to be
	 * removed from the filesystem, but that is blocking I/O and cannot run
	 * with these spinlocks held (interrupts are off). */
	char node_path[UNIX_PATH_MAX + 1];
	int node_len = 0;
	if (us->has_node && us->path_len > 0 && us->path[0] != '\0') {
		node_len = us->path_len;
		if (node_len > UNIX_PATH_MAX)
			node_len = UNIX_PATH_MAX;
		for (int i = 0; i < node_len; i++)
			node_path[i] = (char)us->path[i];
		node_path[node_len] = '\0';
	}
	us->has_node = 0;
	us->path_len = 0; /* name released with the slot */

	spin_unlock_irqrestore(&us->lock, flags);
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	/* Wake anyone parked on either endpoint.  A sender blocked for space,
	 * or a reader blocked for data, must be released now -- with the peer
	 * marked closed, nothing else will ever wake them and they would sleep
	 * for good.  Done outside the locks, as everywhere else, because the
	 * wake path takes scheduler locks. */
	sched_wake_channel(us);
	poll_notify_wq(&us->poll_wq);
	if (peer) {
		sched_wake_channel(peer);
		poll_notify_wq(&peer->poll_wq);
	}

	/* Tell each abandoned client the connection is not coming, then let go
	 * of the reference its queue entry held.  Outside the locks: waking
	 * takes scheduler locks, and the last put frees a ring. */
	/* Each queued entry is a fully built connection that was never
	 * accepted.  Tearing it down is what tells its client the far end has
	 * gone -- reads return end of file, writes EPIPE -- which is what a
	 * connection reset before it was ever served should look like. */
	for (int i = 0; i < n_orphans; i++)
		unix_hangup_and_put(orphans[i]);
	if (n_orphans)
		poll_notify_wq(&us->poll_wq);

	/* Stale sockets otherwise accumulate in /tmp and every later bind to
	 * the same name fails with EADDRINUSE — which is exactly how a display
	 * server refuses to restart after an unclean exit. */
	if (node_len > 0)
		vfs_unlink(node_path);

	/* The peer link's two references, now that no lock is held. */
	if (peer)
		unix_put(peer); /* the one us->peer held */
	if (drop_self)
		unix_put(us); /* the one peer->peer held on us */

	/* And finally the one this descriptor held.  Last, so that every step
	 * above ran on a socket that was certainly still there. */
	unix_put(us);
	return 0;
}

// ============================================================================
// unix_socketpair - Create a pair of connected UNIX domain sockets
// ============================================================================
int unix_socketpair(int type, unix_socket_t *sv[2])
{
	if (type != SOCK_STREAM && type != SOCK_DGRAM &&
	    type != SOCK_SEQPACKET)
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
	struct wait_queue_head saved_s0_wq = s0->poll_wq;
	uint8_t *p = (uint8_t *)s0;

	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;
	s0->poll_wq = saved_s0_wq; /* survives slot reuse */
	s0->active = 1;
	s0->type = type;
	s0->connected = 1;
	s0->ref_count = 1;
	refcount_set(&s0->refcount, 1);
	s0->magic = UNIX_SOCK_MAGIC;
	s0->id = s_unix_next_id++;
	spinlock_init(&s0->lock, "unix_sock");
	wq_head_init_once(&s0->poll_wq, "unix-poll");

	// Initialize s1
	struct wait_queue_head saved_s1_wq = s1->poll_wq;

	p = (uint8_t *)s1;
	for (int i = 0; i < (int)sizeof(unix_socket_t); i++)
		p[i] = 0;
	s1->poll_wq = saved_s1_wq; /* survives slot reuse */
	s1->active = 1;
	s1->type = type;
	s1->connected = 1;
	s1->ref_count = 1;
	refcount_set(&s1->refcount, 1);
	s1->magic = UNIX_SOCK_MAGIC;
	s1->id = s_unix_next_id++;
	spinlock_init(&s1->lock, "unix_sock");
	wq_head_init_once(&s1->poll_wq, "unix-poll");

	// Link peers.  Each direction holds a reference, so closing one end
	// cannot free it while the other still points at it.
	s0->peer = s1;
	s1->peer = s0;
	unix_hold(s1); /* for s0->peer */
	unix_hold(s0); /* for s1->peer */

	/* Both ends belong to the process that made the pair, so each end's
	 * peer is that same process -- which stays true after a fork hands one
	 * end to a child, because the credentials were sampled here. */
	unix_cred_of_current(&s0->self_cred);
	s1->self_cred = s0->self_cred;
	s0->peer_cred = s0->self_cred;
	s1->peer_cred = s0->self_cred;
	s0->has_peer_cred = 1;
	s1->has_peer_cred = 1;
	/* Both ends are connected, and both are unnamed -- a pair has no
	 * pathname.  peer_valid still has to be set, or getpeername() on either
	 * end would report ENOTCONN instead of the correct family-only answer. */
	s0->peer_path_len = 0;
	s0->peer_valid = 1;
	s1->peer_path_len = 0;
	s1->peer_valid = 1;
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	sv[0] = s0;
	sv[1] = s1;

	return 0;
}

// ============================================================================
// unix_shutdown - Shutdown part of a UNIX socket connection
// ============================================================================
int unix_shutdown(unix_socket_t *us, int how)
{
	if (!us)
		return -EBADF;
	if (!us->connected)
		return -ENOTCONN;

	if (how == SHUT_WR || how == SHUT_RDWR) {
		/* Tell the peer this end will send no more.
		 *
		 * The peer is pinned first.  This used to write straight
		 * through us->peer with no lock and no check that it was still
		 * a socket at all -- and shutdown(SHUT_RDWR) is what a client
		 * library calls on its way out, so it ran at exactly the moment
		 * sockets were being torn down around it. */
		uint64_t tflags, flags;
		unix_socket_t *peer;

		spin_lock_irqsave(&unix_table_lock, &tflags);
		peer = us->peer;
		if (peer && !unix_tryhold(peer))
			peer = NULL;
		spin_unlock_irqrestore(&unix_table_lock, tflags);

		if (peer) {
			spin_lock_irqsave(&peer->lock, &flags);
			peer->peer_closed = 1;
			peer->ready = 1;
			spin_unlock_irqrestore(&peer->lock, flags);
			sched_wake_channel(peer);
			poll_notify_wq(&peer->poll_wq);
			unix_put(peer);
		}
	}
	if (how == SHUT_RD || how == SHUT_RDWR) {
		/* Stop reading.
		 *
		 * peer_closed is deliberately NOT set here.  It means "the far
		 * end hung up", and poll() reports a hangup from it -- so
		 * setting it on ourselves made this socket report POLLHUP to
		 * its own owner for ever after, on a connection the peer was
		 * still happily using.  A client library that shuts down its
		 * read side and keeps writing was thereby told its connection
		 * had died.
		 *
		 * What SHUT_RD owes the caller is that reads return end of
		 * file, which shut_rd below expresses without claiming
		 * anything about the peer. */
		uint64_t flags;

		spin_lock_irqsave(&us->lock, &flags);
		us->shut_rd = 1;
		spin_unlock_irqrestore(&us->lock, flags);
		sched_wake_channel(us);
		poll_notify_wq(&us->poll_wq);
	}

	return 0;
}

// ============================================================================
// unix_setsockopt / unix_getsockopt - socket options on a UNIX socket
// ============================================================================
/* AF_UNIX sockets answer SOL_SOCKET options too, and until this existed the
 * syscall layer sent every getsockopt/setsockopt on one straight to the
 * INTERNET socket table, where the descriptor is not a socket at all: the
 * answer to any option on any UNIX socket was -ENOTSOCK.
 *
 * That is not the harmless "option not supported" a program tests for.  A
 * caller that treats setsockopt failing as fatal never reaches the call after
 * it -- PCManFM sets SO_REUSEADDR on its single-instance socket and gives up
 * on the bind() in the same expression, so it exited with status 1, silently,
 * before opening a window.
 *
 * The options below are the ones that mean something here.  SO_REUSEADDR and
 * friends are accepted and ignored, which is also what a conventional Unix
 * does with them on this address family: a UNIX socket's name is a filesystem
 * entry, and unlink() -- not a socket option -- is what makes it reusable.
 * Anything genuinely not implemented gets -ENOPROTOOPT, the error that says
 * "not this option" rather than "not a socket"; a program can tell the two
 * apart and only the second one is a reason to give up on the socket.
 *
 * Note that SO_RCVTIMEO/SO_SNDTIMEO are refused rather than accepted: the
 * blocking paths here park without a deadline, so accepting them would promise
 * a timeout that never fires -- a program that relies on one to bound a read
 * would hang instead of failing at the call it made. */
int unix_setsockopt(unix_socket_t *us, int level, int optname,
		    const void *optval, socklen_t optlen)
{
	if (!us)
		return -EBADF;
	if (level != SOL_SOCKET)
		return -ENOPROTOOPT;

	switch (optname) {
	case SO_REUSEADDR:
	case SO_REUSEPORT:
	case SO_KEEPALIVE:
	case SO_BROADCAST:
	case SO_OOBINLINE:
	case SO_LINGER:
		/* Meaningless on a local socket; accepted so that setting them
		 * is not mistaken for the socket being unusable. */
		return 0;

	case SO_SNDBUF:
	case SO_RCVBUF:
		/* Advisory.  The ring is sized by the socket layer and is not
		 * resized on request, but the request itself is legal. */
		if (!optval || optlen < (socklen_t)sizeof(int))
			return -EINVAL;
		return 0;

	default:
		return -ENOPROTOOPT;
	}
}

int unix_getsockopt(unix_socket_t *us, int level, int optname, void *optval,
		    socklen_t *optlen)
{
	int val;

	if (!us)
		return -EBADF;
	if (!optval || !optlen)
		return -EFAULT;
	if (level != SOL_SOCKET)
		return -ENOPROTOOPT;
	/* SO_PEERCRED is the one option here that does not return an int, so
	 * it is answered before the int-shaped machinery below. */
	if (optname == SO_PEERCRED) {
		struct ucred c;
		uint64_t cflags;

		if (*optlen < (socklen_t)sizeof(struct ucred))
			return -EINVAL;
		spin_lock_irqsave(&us->lock, &cflags);
		if (!us->has_peer_cred) {
			spin_unlock_irqrestore(&us->lock, cflags);
			/* Never connected: there is nobody to describe.  The
			 * error conventional Unix gives for this. */
			return -ENOTCONN;
		}
		c = us->peer_cred;
		spin_unlock_irqrestore(&us->lock, cflags);
		*(struct ucred *)optval = c;
		*optlen = (socklen_t)sizeof(struct ucred);
		return 0;
	}

	/* Checked before the switch, not after: SO_ERROR below consumes the
	 * pending error, and a call that cannot deliver it must not clear it. */
	if (*optlen < (socklen_t)sizeof(int))
		return -EINVAL;

	switch (optname) {
	case SO_TYPE:
		val = us->type;
		break;

	case SO_ACCEPTCONN:
		/* Read-only by definition: reports whether listen() has been
		 * applied.  libsoup's server asks this of every socket it is
		 * handed before it will serve on it. */
		val = us->listening ? 1 : 0;
		break;

	case SO_ERROR: {
		/* Read-and-clear, as POSIX requires: the pending error is
		 * reported once.  This is how a program that connected in
		 * non-blocking mode learns whether it succeeded. */
		uint64_t flags;
		spin_lock_irqsave(&us->lock, &flags);
		val = us->error;
		us->error = 0;
		spin_unlock_irqrestore(&us->lock, flags);
		break;
	}

	case SO_SNDBUF:
	case SO_RCVBUF:
		/* The real ring size when one has been allocated; a socket that
		 * has not carried data yet reports the size it would get. */
		val = us->bufsz ? us->bufsz : UNIX_RING_SIZE;
		break;

	case SO_REUSEADDR:
	case SO_REUSEPORT:
	case SO_KEEPALIVE:
	case SO_BROADCAST:
	case SO_OOBINLINE:
		/* Accepted by unix_setsockopt but not retained: they do
		 * nothing, so they always read back off. */
		val = 0;
		break;

	default:
		return -ENOPROTOOPT;
	}

	*(int *)optval = val;
	*optlen = (socklen_t)sizeof(int);
	return 0;
}

// ============================================================================
// unix_poll - Poll a UNIX socket for events
// ============================================================================
/* Report what this socket can do right now.
 *
 * Everything is read into locals first, under the appropriate lock, and the
 * answer is computed from those.  That is not tidiness -- it is the fix.
 *
 * This function used to take no lock at all and dereference us->peer five
 * separate times, with nothing keeping that socket in existence.  A peer whose
 * slot had been recycled answered for whoever now occupied it, so an unrelated
 * socket closing anywhere in the system could make this report POLLHUP on a
 * healthy connection.  A program multiplexing with poll() -- which every
 * graphical client does -- treats a hangup it did not ask for as the
 * connection dying and closes it on the spot, and the poll layer hands our
 * answer to userspace unmasked, so a single invented bit was a lost session.
 *
 * The two reads of the peer's ring size were their own hazard: a release
 * setting it to zero between them turned the modulo into a division by zero
 * in the kernel.  One snapshot cannot disagree with itself.
 */
int unix_poll(unix_socket_t *us, short events)
{
	if (!us)
		return 0;

	short revents = 0;
	uint64_t flags, tflags;
	unix_socket_t *peer;
	int have_data, listening, has_pending, closed, error, peer_closed;
	int shut_rd;

	spin_lock_irqsave(&us->lock, &flags);
	have_data = (us->head != us->tail);
	listening = us->listening;
	has_pending = (us->accept_head != us->accept_tail);
	closed = us->closed;
	error = us->error;
	peer_closed = us->peer_closed;
	shut_rd = us->shut_rd;
	spin_unlock_irqrestore(&us->lock, flags);

	/* The peer is pinned for exactly as long as it is looked at.  Taken
	 * under the table lock because that is where the link is severed. */
	spin_lock_irqsave(&unix_table_lock, &tflags);
	peer = us->peer;
	if (peer && !unix_tryhold(peer))
		peer = NULL;
	spin_unlock_irqrestore(&unix_table_lock, tflags);

	int peer_gone = 1, peer_writable = 0;

	if (peer) {
		spin_lock_irqsave(&peer->lock, &flags);
		peer_gone = peer->closed;
		if (!peer_gone) {
			/* A peer with no ring yet has never been written to,
			 * so it is trivially writable -- the ring is allocated
			 * on the first send.  Testing the modulo first would
			 * be a divide by zero, and a client polls before it
			 * writes. */
			int bufsz = peer->bufsz;

			peer_writable =
				(bufsz == 0) ||
				((peer->tail + 1) % bufsz != peer->head);
		}
		spin_unlock_irqrestore(&peer->lock, flags);
	}

	if (events & POLLIN) {
		if (have_data)
			revents |= POLLIN;
		if (peer_closed)
			revents |= POLLIN | POLLHUP;
		/* Readable, because a read returns end of file at once -- but
		 * NOT a hangup: this end asked to stop reading, the connection
		 * is still up, and a caller that is told otherwise drops it. */
		if (shut_rd)
			revents |= POLLIN;
		if (listening && has_pending)
			revents |= POLLIN;
	}

	if (events & POLLOUT) {
		if (peer && !peer_gone && peer_writable)
			revents |= POLLOUT;
		if (!peer || peer_gone)
			revents |= POLLHUP;
	}

	if (closed)
		revents |= POLLHUP;
	if (error)
		revents |= POLLERR;

	if (peer)
		unix_put(peer);

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
// The queue holds UNIX_PENDING_FDS entries.  tmux, the original user,
// sends at most one fd per imsg and drains promptly; WebKit's SEQPACKET
// IPC attaches every shared buffer of a message at once, which is what
// the size is set for.  When full we return -EAGAIN so the sender can
// retry; this preserves ordering with data bytes already accepted by
// unix_send.
int unix_push_fd(unix_socket_t *sock, void *entry)
{
	if (!sock)
		return -EBADF;
	uint64_t flags;
	spin_lock_irqsave(&sock->lock, &flags);
	int next = (sock->pending_fd_tail + 1) % UNIX_PENDING_FDS;
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

/* Wake a sender parked for pending-fd queue space on `sock`, and tell the
 * peer's pollers it is writable again.  Called by recvmsg after it pops this
 * record's descriptors: the wake the sender got from the record read fires
 * BEFORE the pop, so it re-checks a still-full queue and parks again -- and
 * with the sender parked there are no further reads to wake it, ever. */
void unix_fd_space_wake(unix_socket_t *sock)
{
	unix_socket_t *pw = NULL;
	uint64_t pf;

	if (!sock)
		return;
	sched_wake_channel(sock);
	spin_lock_irqsave(&sock->lock, &pf);
	if (sock->peer) {
		pw = sock->peer;
		unix_hold(pw);
	}
	spin_unlock_irqrestore(&sock->lock, pf);
	if (pw) {
		poll_notify_wq(&pw->poll_wq);
		unix_put(pw);
	}
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
	sock->pending_fd_head = (sock->pending_fd_head + 1) % UNIX_PENDING_FDS;
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
		unix_socket_t *peer =
			(unix_socket_t *)*(
				volatile unix_socket_t * *const *)&u->peer;
		unix_socket_t *par =
			(unix_socket_t *)*(
				volatile unix_socket_t * *const *)&u->parent;
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
			u->connected, u->closed, u->peer_closed, u->accept_head,
			u->accept_tail, u->head, u->tail,
			peer ? (int)(peer - unix_sockets) : -1,
			par ? (int)(par - unix_sockets) : -1, nlen, nm);
	}

	tty_printf(tty, "=====================\n");
}
