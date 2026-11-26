// LikeOS Socket Layer
// Provides kernel-side socket abstraction for syscall layer
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/waitq.h>
#include <kernel/net/net.h>
#include <kernel/net/ip.h>
#include <kernel/io/console.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/syscall.h>
#include <kernel/fs/namei.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/pipe.h>
#include <kernel/mm/memory.h>
#include <kernel/io/tty.h>
#include <kernel/dev/rand/random.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/cred.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/file.h>
#include <kernel/ke/uaccess.h>

/* "Operation already in progress" -- the conventional number, as in the
 * libc's <errno.h>; the kernel's errno list has no use for it elsewhere. */
#ifndef EALREADY
#define EALREADY 114
#endif

/* Same extern tcp.c uses; poll.c exports it without a header. */
extern void poll_notify_wq(struct wait_queue_head *);
extern void poll_notify_wq_key(struct wait_queue_head *, unsigned key);
extern void eventpoll_release(void *entry);

// Socket table
static net_socket_t sockets[NET_MAX_SOCKETS];
static spinlock_t socket_lock = SPINLOCK_INIT("socket");

// IRQ-friendly blocking acquire of a TCP per-connection spinlock.
// Mirrors tcp_lock_acquire() in tcp.c — between trylock attempts IRQs
// are re-enabled so this CPU keeps ACKing TLB shootdown IPIs.  Avoids
// the OS-wide multi-second freeze (`SMP: TLB shootdown sync timeout`)
// that occurs when sock_recv spins IRQs-off on conn->lock while another
// CPU's slab_free or sched_remove_task initiates a TLB shootdown.
static inline void sock_conn_lock(spinlock_t *lock, uint64_t *flags_out)
{
	uint64_t f = local_irq_save();
	while (!spin_trylock(lock)) {
		local_irq_restore(f);
		__asm__ volatile("pause" ::: "memory");
		f = local_irq_save();
	}
	*flags_out = f;
}

static inline void sock_conn_unlock(spinlock_t *lock, uint64_t flags)
{
	spin_unlock(lock);
	local_irq_restore(flags);
	// see tcp_lock_release(): transmits queued under the lock go now
	if (flags & (1ULL << 9))
		net_tx_flush_local();
}

/* ---------------------------------------------------------------------------
 * What the socket layer reads of a TCP connection for poll() and the queue
 * ioctls.  Kept to these few lines so the TCP side can change underneath:
 * each is a plain read of state the protocol maintains, safe without the
 * connection lock (a momentarily stale answer, never a torn pointer), on a
 * connection the caller holds a reference to.
 * ------------------------------------------------------------------------ */
/* Bytes queued in order and not yet read. */
static inline uint32_t sock_tcp_inq(const tcp_conn_t *c)
{
	return __atomic_load_n(&c->rcvq_bytes, __ATOMIC_ACQUIRE);
}

/* Bytes the application wrote that the peer has not acknowledged. */
static inline uint32_t sock_tcp_outq(const tcp_conn_t *c)
{
	return __atomic_load_n(&c->write_seq, __ATOMIC_RELAXED) -
	       __atomic_load_n(&c->snd_una, __ATOMIC_RELAXED);
}

/* ... of them, not yet sent at all. */
static inline uint32_t sock_tcp_outq_nsd(const tcp_conn_t *c)
{
	return __atomic_load_n(&c->write_seq, __ATOMIC_RELAXED) -
	       __atomic_load_n(&c->snd_nxt, __ATOMIC_RELAXED);
}

/* An urgent byte has arrived and not been read with MSG_OOB yet. */
static inline int sock_tcp_urg_pending(const tcp_conn_t *c)
{
	return __atomic_load_n(&c->urg_state, __ATOMIC_ACQUIRE) ==
	       TCP_URG_VALID;
}

/* The next byte read is the urgent one (rcv_up is its sequence). */
static inline int sock_tcp_atmark(const tcp_conn_t *c)
{
	return tcp_at_mark((tcp_conn_t *)c);
}

/* The socket's connection with a reference held (tcp_conn_release), or
 * NULL. */
static tcp_conn_t *sock_tcp_get(net_socket_t *s)
{
	uint64_t f;
	tcp_conn_t *c;

	spin_lock_irqsave(&s->lock, &f);
	c = s->active ? s->tcp : NULL;
	if (c)
		tcp_conn_get(c);
	spin_unlock_irqrestore(&s->lock, f);
	return c;
}

/* A write on a stream whose sending side is gone (EPIPE) also raises
 * SIGPIPE, unless the caller said MSG_NOSIGNAL. */
static void sock_sigpipe(void)
{
	task_t *cur = sched_current();

	if (cur)
		sched_signal_task(cur, SIGPIPE);
}

// Ephemeral port counter (49152-65535)
static uint16_t next_ephemeral_port = 49152;

// Serialises the whole ephemeral-port allocation: the scan for a free port
// and the reservation that publishes it on the socket must be atomic with
// respect to each other, or two callers on different CPUs can both observe
// the same port as free (neither has marked its socket bound yet) and hand it
// out twice.  The lock is a leaf — it may be taken while a caller already
// holds that socket's s->lock (the bind path), never the reverse.
static spinlock_t g_port_alloc_lock = SPINLOCK_INIT("portalloc");

void socket_init(void)
{
	for (int i = 0; i < NET_MAX_SOCKETS; i++) {
		sockets[i].active = 0;
	}
}

// Allocate a free ephemeral port and atomically RESERVE it on `s` by
// publishing it (local_addr.sin_port + bound=1) before releasing the port
// lock.  A concurrent allocator therefore sees the reservation and cannot
// pick the same port for another socket.  Callers set the remaining
// local_addr fields (family/addr) around this call; only sin_port + bound key
// the collision scan, so their ordering does not matter.  Returns host-order.
static uint16_t alloc_ephemeral_port(net_socket_t *s, uint32_t dst_ip,
				    uint16_t dst_port)
{
	// Draw the randomized starting point BEFORE taking the port lock —
	// random_get_bytes may take the entropy lock, which must not nest under
	// this IRQs-off spinlock.  Probe sequentially from there under the lock.
	uint16_t start = 49152 + (uint16_t)(random_u32() % 16384);

	uint64_t flags;
	spin_lock_irqsave(&g_port_alloc_lock, &flags);
	uint16_t port = 0;
	for (int attempt = 0; attempt < 128; attempt++) {
		uint16_t cand =
			(uint16_t)(49152 +
				   (((uint32_t)(start - 49152) + attempt) %
				    16384));
		int in_use = 0;
		for (int i = 0; i < NET_MAX_SOCKETS; i++) {
			if (sockets[i].active && sockets[i].bound &&
			    net_ntohs(sockets[i].local_addr.sin_port) == cand) {
				in_use = 1;
				break;
			}
		}
		/* The socket table is not enough: a connection in TIME_WAIT/
		 * FIN_WAIT still owns its 4-tuple after its socket is gone,
		 * and re-issuing it to a reconnect toward the SAME remote
		 * collides the handshake with the dying conn (sporadic
		 * "connection reset"/"broken pipe" during parallel browser
		 * loads).
		 *
		 * Only toward the same remote.  This used to reject a
		 * candidate if ANY connection held the port, which is a much
		 * stronger rule than the collision needs and it priced the
		 * ephemeral range in CONNECTIONS rather than in
		 * connections-per-peer: with every close lingering a minute
		 * in TIME_WAIT, a few thousand requests exhausted all 16384
		 * ports, and each allocation then walked all 128 candidates
		 * against the whole connection table -- with interrupts off,
		 * while the receive path needed the same lock.  Asking about
		 * the 4-tuple instead is what actually prevents the
		 * collision, and it lets one local port serve as many peers
		 * as there are peers. */
		if (!in_use) {
			if (dst_ip)
				in_use = tcp_4tuple_in_use(
					net_ntohl(s->local_addr.sin_addr.s_addr),
					cand, dst_ip, dst_port);
			else
				in_use = tcp_local_port_in_use(cand);
		}
		if (!in_use) {
			port = cand;
			break;
		}
	}
	if (!port) {
		// 128 consecutive ports all in use (near-exhaustion): fall back
		// to the sequential counter, still under the lock.
		port = next_ephemeral_port++;
		if (next_ephemeral_port < 49152)
			next_ephemeral_port = 49152;
	}
	// Publish the reservation.  sin_port before bound (with a release
	// fence) so a lock-free reader that sees bound=1 also sees the port.
	s->local_addr.sin_port = net_htons(port);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	s->bound = 1;
	spin_unlock_irqrestore(&g_port_alloc_lock, flags);
	return port;
}

/* The table slot of a socket (for the wakeup bitmaps of the datagram
 * delivery paths), -1 for anything else. */
int sock_slot(const net_socket_t *s)
{
	uintptr_t base = (uintptr_t)&sockets[0];
	uintptr_t p = (uintptr_t)s;

	if (p < base || p >= base + sizeof(sockets) ||
	    (p - base) % sizeof(net_socket_t))
		return -1;
	return (int)((p - base) / sizeof(net_socket_t));
}

void sock_wake_slots(const uint64_t *bits)
{
	for (int w = 0; w < NET_MAX_SOCKETS / 64; w++) {
		uint64_t m = bits[w];

		while (m) {
			int b = __builtin_ctzll(m);

			m &= m - 1;
			dgram_wake(&sockets[w * 64 + b]);
		}
	}
}

/* SO_RCVBUF / SO_SNDBUF: what the caller asks for, doubled for the
 * bookkeeping overhead, between a floor and twice the system maximum --
 * and that doubled value is what getsockopt() reports. */
#define SOCK_RMEM_MAX 212992
#define SOCK_WMEM_MAX 212992
#define SOCK_MIN_RCVBUF 2304
#define SOCK_MIN_SNDBUF 4608

// ============================================================================
// sock_create - Create a new socket
// Returns socket descriptor (index into socket table) or negative errno
// ============================================================================
int sock_create(int domain, int type, int protocol)
{
	if (domain != AF_INET)
		return -EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;
	/* Raw sockets bypass the transport layer (arbitrary packet crafting), so
	 * creating one is reserved to the privileged caller. */
	if (type == SOCK_RAW && !capable())
		return -EPERM;

	if (type == SOCK_RAW && (protocol < 0 || protocol > 255))
		return -EINVAL;

	uint64_t flags;
	spin_lock_irqsave(&socket_lock, &flags);

	int fd = -ENOMEM;
	for (int i = 0; i < NET_MAX_SOCKETS; i++) {
		/* A slot still on a lookup table is mid-close: its
		 * table linkage must not be zeroed under the table. */
		if (!sockets[i].active &&
		    !__atomic_load_n(&sockets[i].hashed, __ATOMIC_ACQUIRE)) {
			net_socket_t *s = &sockets[i];
			// Zero the entire struct first so any new fields default to 0.
			/* All but the poll queue, which survives the slot
			 * being reused -- see wq_zero_around(). */
			struct wait_queue_head *keep[] = { &s->poll_wq };

			wq_zero_around(s, sizeof(*s), keep, 1);
			wq_head_init_once(&s->poll_wq, "sock-poll");
			s->type = type;
			if (type == SOCK_STREAM)
				s->protocol = IPPROTO_TCP;
			else if (type == SOCK_DGRAM)
				s->protocol = IPPROTO_UDP;
			else
				s->protocol = (uint8_t)protocol;
			s->domain = domain;
			s->ref_count = 1;
			s->active = 1;
			// Sensible defaults
			s->ip_ttl = 64;
			s->mcast_ttl = 1;
			s->mcast_loop = 1;
			/* The defaults everybody else has: datagram
			 * sockets 208 KB both ways; a TCP socket the
			 * connection's initial buffers. */
			if (type == SOCK_STREAM) {
				s->sndbuf_size = 16384;
				s->rcvbuf_size = 131072;
			} else {
				s->sndbuf_size = SOCK_WMEM_MAX;
				s->rcvbuf_size = SOCK_RMEM_MAX;
			}
			s->pmtudisc = IP_PMTUDISC_WANT;
			s->mc_all = 1;
			s->rcvlowat = 1;
			s->max_pacing_rate = ~0ULL;
			s->lock = (spinlock_t)SPINLOCK_INIT("sock");
			skb_queue_init(&s->rx_queue, "sock_rxq");
			skb_queue_init(&s->err_queue, "sock_errq");
			s->local_addr.sin_family = AF_INET;
			s->remote_addr.sin_family = AF_INET;

			fd = i;
			break;
		}
	}

	spin_unlock_irqrestore(&socket_lock, flags);
	WARN_RATELIMIT(fd < 0, "sock_create: socket table full (%d slots)",
		       NET_MAX_SOCKETS);
	/* A raw socket receives its protocol's datagrams from now on. */
	if (fd >= 0 && type == SOCK_RAW)
		raw_hash(&sockets[fd]);
	return fd;
}

// ============================================================================
// sock_bind - Bind socket to a local address
// ============================================================================
int sock_bind(int sockfd, const struct sockaddr_in *addr)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	/* UDP has its own port space and tables (udp.c). */
	if (s->type == SOCK_DGRAM)
		return udp_bind(s, addr);
	if (s->bound)
		return -EINVAL;
	if (s->type == SOCK_RAW) {
		/* A raw socket has no port: binding names the local
		 * address it receives for (and sends from). */
		uint32_t a = net_ntohl(addr->sin_addr.s_addr);

		if (a && !ip_addr_is_local(a) && !ip_addr_is_broadcast(a) &&
		    (a & 0xF0000000u) != 0xE0000000u)
			return -EADDRNOTAVAIL;
		s->local_addr.sin_family = AF_INET;
		s->local_addr.sin_port = 0;
		s->local_addr.sin_addr.s_addr = addr->sin_addr.s_addr;
		s->udp_saddr = (a && ip_addr_is_local(a)) ? a : 0;
		s->bound = 1;
		return 0;
	}
	/* Reserved ports (<1024) may only be bound by the privileged caller. */
	uint16_t bind_port = net_ntohs(addr->sin_port);
	if (bind_port != 0 && bind_port < 1024 && !capable())
		return -EACCES;

	/* TCP: reject a bind that conflicts with an existing ACTIVE TCP socket on
	 * the same port with an overlapping local address.  POSIX/BSD: only
	 * SO_REUSEPORT (set on BOTH sockets) permits two live sockets to share a
	 * port; SO_REUSEADDR alone only permits reusing a port whose old
	 * connection is gone (TIME_WAIT, handled separately in the SYN path), NOT
	 * two concurrent listeners.  Without this check two processes that pick
	 * the same port (e.g. per-pid test ports that collide) both bind
	 * INADDR_ANY:port successfully, and tcp_find_listener then distributes
	 * SYNs across the two unrelated listeners — silently cross-wiring their
	 * connections. */
	if (addr->sin_port != 0 && s->type == SOCK_STREAM) {
		uint32_t bind_ip = net_ntohl(addr->sin_addr.s_addr);
		uint64_t gflags;
		spin_lock_irqsave(&socket_lock, &gflags);
		for (int i = 0; i < NET_MAX_SOCKETS; i++) {
			net_socket_t *o = &sockets[i];
			if (i == sockfd || !o->active || !o->bound ||
			    o->type != SOCK_STREAM)
				continue;
			if (o->local_addr.sin_port != addr->sin_port)
				continue;
			/* Addresses conflict when equal or either is the
			 * wildcard 0.0.0.0. */
			uint32_t o_ip =
				net_ntohl(o->local_addr.sin_addr.s_addr);
			if (bind_ip != o_ip && bind_ip != 0 && o_ip != 0)
				continue;
			/* Both explicitly opted into SO_REUSEPORT → allowed,
			 * but only within one user's group (conventional semantics):
			 * the joining bind must have the same effective UID as
			 * the socket already holding the port, unless the
			 * caller is privileged.  Without this, any user could
			 * join another user's reuseport group and hijack a
			 * share of its incoming connections.  The exception is
			 * on the CALLER only: an unprivileged user cannot join
			 * root's group, but root can join anyone's. */
			if (s->reuse_port && o->reuse_port &&
			    (capable() || current_euid() == o->bind_euid))
				continue;
			spin_unlock_irqrestore(&socket_lock, gflags);
			return -EADDRINUSE;
		}
		spin_unlock_irqrestore(&socket_lock, gflags);
	}

	uint64_t flags;
	spin_lock_irqsave(&s->lock, &flags);
	s->local_addr = *addr;
	if (s->local_addr.sin_port == 0) {
		// Reserves the port and sets s->bound under the port lock.
		alloc_ephemeral_port(s, 0, 0);
	}
	s->bind_euid = current_euid();
	s->bound = 1;
	spin_unlock_irqrestore(&s->lock, flags);

	return 0;
}

/* Apply the TCP options set on the socket (see net_socket_t::tcp_opt_set)
 * to a connection the socket has just come to own.  Only options set
 * explicitly are applied; the rest keep the connection's defaults. */
static void sock_apply_tcp_opts(const net_socket_t *s, tcp_conn_t *conn)
{
	uint8_t set = s->tcp_opt_set;

	if (!conn)
		return;
	if (s->sk_userlocks)
		tcp_set_buffers(conn, s->sndbuf_size, s->rcvbuf_size,
				s->sk_userlocks);
	if (s->pacing_set)
		tcp_set_max_pacing_rate(conn, s->max_pacing_rate);
	if (s->oobinline)
		tcp_set_oobinline(conn, 1);
	for (int i = 0; i < s->tcp_xopt_n; i++)
		tcp_setsockopt_int(conn, s->tcp_xopt_name[i],
				   s->tcp_xopt_val[i]);
	if (!set)
		return;
	if (set & SOCK_TCPOPT_NODELAY)
		tcp_setsockopt_int(conn, TCP_NODELAY, s->tcp_opt_nodelay);
	/* Under the connection's lock: it is live already (its timers and
	 * the receive path may be looking). */
	if (set & SOCK_TCPOPT_KEEPIDLE)
		tcp_setsockopt_int(conn, TCP_KEEPIDLE,
				   (int)timer_ticks_to_s(s->tcp_opt_keepidle_ticks));
	if (set & SOCK_TCPOPT_KEEPINTVL)
		tcp_setsockopt_int(conn, TCP_KEEPINTVL,
				   (int)timer_ticks_to_s(
					   s->tcp_opt_keepintvl_ticks));
	if (set & SOCK_TCPOPT_KEEPCNT)
		tcp_setsockopt_int(conn, TCP_KEEPCNT, (int)s->tcp_opt_keepcnt);
	if (set & SOCK_TCPOPT_KEEPALIVE)
		tcp_setsockopt_int(conn, TCP_SOCKOPT_KEEPALIVE,
				   s->tcp_opt_keepalive);
	if (set & SOCK_TCPOPT_CORK)
		tcp_set_cork(conn, s->tcp_opt_cork);
}

/* A connection option the TCP layer keeps (tcp_setsockopt_int): applied
 * to the connection when it exists, and remembered for one made later
 * (connect(), listen() and the listener's children). */
static int sock_tcp_xopt_set(net_socket_t *s, int name, int val)
{
	int rc = 0, i;
	tcp_conn_t *c = sock_tcp_get(s);

	if (c) {
		rc = tcp_setsockopt_int(c, name, val);
		tcp_conn_release(c);
		if (rc < 0)
			return rc;
	} else {
		switch (name) {
		case TCP_MAXSEG:
			if (val && (val < 88 || val > 65535 - 40))
				return -EINVAL;
			break;
		case TCP_SYNCNT:
			if (val < 1 || val > 127)
				return -EINVAL;
			break;
		case TCP_USER_TIMEOUT:
		case TCP_WINDOW_CLAMP:
			if (val < 0)
				return -EINVAL;
			break;
		default:
			break;
		}
	}
	if (name == TCP_QUICKACK)
		return rc; // a one-time request, nothing to remember
	for (i = 0; i < s->tcp_xopt_n; i++)
		if (s->tcp_xopt_name[i] == name)
			break;
	if (i == s->tcp_xopt_n) {
		if (i >= SOCK_TCP_XOPTS)
			return rc;
		s->tcp_xopt_n++;
		s->tcp_xopt_name[i] = name;
	}
	s->tcp_xopt_val[i] = val;
	return rc;
}

// The remembered value of such an option, or `def'.
static int sock_tcp_xopt_get(const net_socket_t *s, int name, int def)
{
	for (int i = 0; i < s->tcp_xopt_n; i++)
		if (s->tcp_xopt_name[i] == name)
			return s->tcp_xopt_val[i];
	return def;
}

/* An accepted socket starts with its listener's TCP options, as it does
 * everywhere else: a server that sets TCP_NODELAY or SO_KEEPALIVE on its
 * listening socket means every connection it accepts. */
static void sock_inherit_tcp_opts(net_socket_t *dst, const net_socket_t *src)
{
	dst->tcp_opt_set = src->tcp_opt_set;
	dst->tcp_opt_keepalive = src->tcp_opt_keepalive;
	dst->tcp_opt_nodelay = src->tcp_opt_nodelay;
	dst->tcp_opt_cork = src->tcp_opt_cork;
	dst->tcp_opt_keepidle_ticks = src->tcp_opt_keepidle_ticks;
	dst->tcp_opt_keepintvl_ticks = src->tcp_opt_keepintvl_ticks;
	dst->tcp_opt_keepcnt = src->tcp_opt_keepcnt;
	dst->sk_userlocks = src->sk_userlocks;
	/* SO_SNDBUF / SO_RCVBUF set on the listener are the accepted
	 * socket's too (sock_apply_tcp_opts applies them from here -- with
	 * the defaults instead it undid the listener's sizes). */
	dst->sndbuf_size = src->sndbuf_size;
	dst->rcvbuf_size = src->rcvbuf_size;
	dst->tcp_xopt_n = src->tcp_xopt_n;
	for (int i = 0; i < src->tcp_xopt_n && i < SOCK_TCP_XOPTS; i++) {
		dst->tcp_xopt_name[i] = src->tcp_xopt_name[i];
		dst->tcp_xopt_val[i] = src->tcp_xopt_val[i];
	}
	/* And the socket-level options an accepted socket takes over. */
	dst->rcvlowat = src->rcvlowat;
	dst->sk_mark = src->sk_mark;
	dst->sk_priority = src->sk_priority;
	dst->rcv_tstamp = src->rcv_tstamp;
	dst->max_pacing_rate = src->max_pacing_rate;
	dst->pacing_set = src->pacing_set;
	dst->oobinline = src->oobinline;
}

// ============================================================================
// sock_listen - Mark socket as listening (TCP only)
// ============================================================================
int sock_listen(int sockfd, int backlog)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	if (s->type != SOCK_STREAM)
		return -EOPNOTSUPP;
	if (!s->bound)
		return -EINVAL;

	/* Already listening: only the backlog changes.  This used to start
	 * a second listener on the same port and put it in place of the
	 * first, which stayed in the lookup table: connections kept arriving
	 * on the first, where accept() never looked, and the port went on
	 * listening after close().  A socket that is connected or connecting
	 * cannot listen. */
	{
		tcp_conn_t *cur = sock_tcp_get(s);

		if (cur) {
			int rc = 0;

			if (s->listening &&
			    __atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) ==
				    TCP_STATE_LISTEN)
				tcp_listen_set_backlog(cur, backlog > 0 ?
								    backlog :
								    5);
			else
				rc = -EINVAL;
			tcp_conn_release(cur);
			return rc;
		}
	}

	net_device_t *dev = net_get_default_device();
	if (!dev)
		return -ENETDOWN;

	uint32_t local_ip = net_ntohl(s->local_addr.sin_addr.s_addr);
	uint16_t local_port = net_ntohs(s->local_addr.sin_port);

	tcp_conn_t *conn = tcp_listen(dev, local_ip, local_port,
				      backlog > 0 ? backlog : 5);
	if (!conn)
		return -ENOMEM;

	uint64_t flags;
	spin_lock_irqsave(&s->lock, &flags);
	lockdep_assert_held(&s->lock);
	if (s->tcp) {
		/* Another thread's listen() or connect() on this socket got
		 * there first: one connection per socket. */
		spin_unlock_irqrestore(&s->lock, flags);
		tcp_close(conn);
		tcp_conn_release(conn);
		return s->listening ? 0 : -EINVAL;
	}
	s->tcp = conn;
	conn->owner_socket = s;
	s->listening = 1;
	spin_unlock_irqrestore(&s->lock, flags);
	sock_apply_tcp_opts(s, conn);

	return 0;
}

/* Sleep on the socket's poll queue until `ready(arg)' holds, a signal is
 * pending, or the tick `deadline' (0: none) passes.  The caller re-checks
 * its own condition afterwards; this may return early for any reason.
 *
 * The TCP layer notifies this queue on every event a blocking call here
 * waits for -- a connection queued on a listener (tcp_listener_notify), a
 * handshake finished or failed, send space opening (tcp_poll_notify) -- so
 * the wait costs nothing until one happens.  These waits used to spin on
 * sched_yield_in_kernel(), which with nothing else queued returns at once:
 * a blocked accept() or connect() ran its loop a million times a second,
 * and each pass poked the network softirq on CPU0.
 *
 * Registration comes before the state is published and the condition is
 * re-checked after it, so an event between the caller's check and the park
 * is seen either here or by the waker's claim (see sock_recv's park). */
static void sock_wait_event(net_socket_t *s, int (*ready)(void *), void *arg,
			    uint64_t deadline)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;

	if (!cur) {
		sched_yield_in_kernel();
		return;
	}
	wq_entry_init(&we, cur);
	wq_add(&s->poll_wq, &we);
	cur->wait_channel = (void *)&s->poll_wq;
	cur->wakeup_tick = deadline;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	task_set_blocked(cur);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (ready(arg) || signal_pending(cur)) {
		task_state_t expected = TASK_BLOCKED;

		/* Un-park by CAS: if a waker claimed us first we are READY
		 * and being enqueued, and sched_schedule() picks us back. */
		if (__atomic_compare_exchange_n(&cur->state, &expected,
						TASK_RUNNING, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE)) {
			cur->wait_channel = NULL;
			cur->wakeup_tick = 0;
			wq_remove(&s->poll_wq, &we);
			return;
		}
	}
	sched_schedule();
	cur->wait_channel = NULL;
	cur->wakeup_tick = 0;
	wq_remove(&s->poll_wq, &we);
}

/* A child to take, or the listener stopped listening (accept() fails). */
static int sock_accept_ready(void *arg)
{
	tcp_conn_t *listener = arg;

	return __atomic_load_n(&listener->accept_ready, __ATOMIC_ACQUIRE) != 0 ||
	       __atomic_load_n(&listener->state, __ATOMIC_ACQUIRE) !=
		       TCP_STATE_LISTEN;
}

static int sock_connect_ready(void *arg)
{
	tcp_conn_t *conn = arg;

	return __atomic_load_n(&conn->connect_done, __ATOMIC_ACQUIRE) != 0;
}

/* Send space, or a state in which sending is over. */
static int sock_send_ready(void *arg)
{
	tcp_conn_t *conn = arg;
	int st = __atomic_load_n(&conn->state, __ATOMIC_ACQUIRE);

	return __atomic_load_n(&conn->tx_ready, __ATOMIC_ACQUIRE) ||
	       conn->error ||
	       (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_CLOSE_WAIT);
}

// ============================================================================
// sock_accept - Accept a connection (TCP only, blocking)
// ============================================================================
int sock_accept(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active || !s->listening)
		return -EINVAL;
	tcp_conn_t *lc = sock_tcp_get(s);
	if (!lc)
		return -EINVAL;

	// Wait for a connection
	uint64_t deadline =
		s->rcv_timeout_ticks ? timer_ticks() + s->rcv_timeout_ticks : 0;

	tcp_conn_t *new_conn = NULL;
	task_t *acc_cur = sched_current();
	int arc = 0;
	while (1) {
		new_conn = tcp_accept(lc);
		if (new_conn)
			break;
		/* No longer listening -- shutdown(SHUT_RD) stopped it, here
		 * or while a thread waited: EINVAL, as for a socket that
		 * never listened (the waiters are woken by the close). */
		if (__atomic_load_n(&lc->state, __ATOMIC_ACQUIRE) !=
		    TCP_STATE_LISTEN) {
			arc = -EINVAL;
			break;
		}
		if (s->nonblock) {
			arc = -EAGAIN;
			break;
		}
		if (deadline && timer_ticks() >= deadline) {
			arc = -ETIMEDOUT;
			break;
		}
		/* Interruptible: a pending (possibly fatal) signal must be able
		 * to take us out of this wait — POSIX accept() returns EINTR. */
		if (acc_cur && signal_pending(acc_cur)) {
			arc = -EINTR;
			break;
		}
		loopback_process_pending();
		sock_wait_event(s, sock_accept_ready, lc, deadline);
	}
	tcp_conn_release(lc);
	if (!new_conn)
		return arc;

	// Create a new socket for the accepted connection.  tcp_accept returned
	// new_conn with a reference (the accept-queue reference, transferred to
	// us), which becomes this socket's reference.
	int newfd = sock_create(AF_INET, SOCK_STREAM, 0);
	if (newfd < 0) {
		tcp_close(new_conn);
		tcp_conn_release(new_conn); // drop the reference we were handed
		return newfd;
	}

	net_socket_t *ns = &sockets[newfd];
	ns->tcp = new_conn;
	/* The peer may reset the child at any moment, also between
	 * tcp_accept() and here: the socket then reports it like any reset
	 * connection (no assertion on the state). */
	new_conn->owner_socket = ns;
	ns->connected = 1;
	ns->bind_euid = current_euid();
	ns->bound = 1;
	ns->local_addr.sin_family = AF_INET;
	ns->local_addr.sin_port = net_htons(new_conn->local_port);
	ns->local_addr.sin_addr.s_addr = net_htonl(new_conn->local_ip);
	ns->remote_addr.sin_family = AF_INET;
	ns->remote_addr.sin_port = net_htons(new_conn->remote_port);
	ns->remote_addr.sin_addr.s_addr = net_htonl(new_conn->remote_ip);
	sock_inherit_tcp_opts(ns, s);
	sock_apply_tcp_opts(ns, new_conn);

	// Fill in caller's address if requested
	if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
		*addr = ns->remote_addr;
		*addrlen = sizeof(struct sockaddr_in);
	}

	return newfd;
}

// ============================================================================
// sock_connect - Connect to remote host (TCP or set UDP default dest)
// ============================================================================
/* The source address for a connection to dst_ip leaving through dev.  On
 * the loopback device the IP layer stamps the destination itself as the
 * source (ipv4_send_common()), so the connection must use the same address
 * or its checksums -- computed over the pseudo header -- would not match
 * what the receiver sees. */
static uint32_t sock_source_ip(net_device_t *dev, uint32_t dst_ip)
{
	if (dev == net_get_loopback())
		return dst_ip;
	return dev->ip_addr;
}

/* Wait for the handshake of the socket's connection to end.  0 when it
 * did (successfully or not -- sock_connect_finish() tells), otherwise the
 * error the waiting connect() returns; the handshake carries on in the
 * background either way.  The wait is bounded by SO_SNDTIMEO, which is
 * what bounds connect() everywhere (it used to take SO_RCVTIMEO); its
 * expiry reports EINPROGRESS, as a non-blocking connect would. */
static int sock_connect_wait(net_socket_t *s, tcp_conn_t *conn)
{
	uint64_t deadline = s->snd_timeout_ticks ?
				    timer_ticks() + s->snd_timeout_ticks :
				    0;
	task_t *con_cur = sched_current();

	while (!__atomic_load_n(&conn->connect_done, __ATOMIC_ACQUIRE)) {
		if (s->nonblock)
			return -EINPROGRESS;
		if (deadline && timer_ticks() >= deadline)
			return -EINPROGRESS;
		/* Interruptible (POSIX: connect() returns EINTR; the handshake
		 * continues in the background if the caller survives). */
		if (con_cur && signal_pending(con_cur))
			return -EINTR;
		loopback_process_pending();
		sock_wait_event(s, sock_connect_ready, conn, deadline);
	}
	return 0;
}

/* The handshake of `conn' is over: report how it went.  A failed
 * connection is detached from the socket, which may then connect() again;
 * its error is returned once.  The caller holds a reference on conn. */
static int sock_connect_finish(net_socket_t *s, tcp_conn_t *conn, int refuse)
{
	uint64_t f;

	if (refuse || conn->error || conn->state == TCP_STATE_CLOSED ||
	    conn->state == TCP_STATE_SYN_SENT) {
		/* A failure whose error SO_ERROR already consumed reads as
		 * ECONNABORTED, as it does elsewhere. */
		int err = conn->error ? conn->error :
			  (refuse || conn->state == TCP_STATE_SYN_SENT) ?
					ECONNREFUSED :
					ECONNABORTED;
		int detached = 0;

		spin_lock_irqsave(&s->lock, &f);
		if (s->tcp == conn) {
			conn->owner_socket = NULL;
			s->tcp = NULL;
			detached = 1;
		}
		spin_unlock_irqrestore(&s->lock, f);
		if (detached) {
			tcp_close(conn); // drive teardown
			tcp_conn_release(conn); // drop the socket reference
		}
		return -err;
	}

	s->connected = 1;
	return 0;
}

int sock_connect(int sockfd, const struct sockaddr_in *addr)
{
	might_sleep();
	BUG_ON(addr == NULL);
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	/* A datagram socket may change (or, with AF_UNSPEC, drop) its
	 * peer at any time. */
	if (s->type == SOCK_DGRAM)
		return udp_connect(s, addr);
	if (s->type == SOCK_RAW) {
		if (addr->sin_family == 0) {
			s->connected = 0;
			s->remote_addr.sin_addr.s_addr = 0;
			return 0;
		}
		if (addr->sin_family != AF_INET)
			return -EAFNOSUPPORT;
		s->remote_addr = *addr;
		s->remote_addr.sin_port = 0;
		s->connected = 1;
		return 0;
	}
	if (s->connected)
		return -EISCONN;

	if (s->type == SOCK_STREAM) {
		uint64_t f;
		tcp_conn_t *prev;

		if (s->listening)
			return -EISCONN;
		/* connect() on a socket whose connection already exists: the
		 * second call of a non-blocking connect, or a retry after
		 * EINTR.  It used to start a SECOND connection, leaking the
		 * first and warning about it.  Answer from the existing one
		 * instead: still handshaking -- EALREADY, or wait for it when
		 * blocking; established -- success, once (EISCONN after);
		 * failed -- its error, after which the socket is unconnected
		 * and may connect() afresh. */
		spin_lock_irqsave(&s->lock, &f);
		prev = s->tcp;
		if (prev)
			tcp_conn_get(prev);
		spin_unlock_irqrestore(&s->lock, f);
		if (prev) {
			int rc = 0;

			if (!__atomic_load_n(&prev->connect_done,
					     __ATOMIC_ACQUIRE)) {
				if (s->nonblock)
					rc = -EALREADY;
				else
					rc = sock_connect_wait(s, prev);
				if (rc == -EINPROGRESS)
					rc = -EALREADY;
			}
			if (rc == 0)
				rc = sock_connect_finish(s, prev, 0);
			tcp_conn_release(prev);
			return rc;
		}
	}

	uint32_t dst_ip = net_ntohl(addr->sin_addr.s_addr);
	uint32_t next_hop = dst_ip;
	net_device_t *dev = route_lookup(dst_ip, &next_hop);
	if (!dev)
		dev = net_get_default_device();
	if (!dev)
		return -ENETDOWN;

	/* The local port was bound before connect() (by bind(), even to a
	 * port picked then), not chosen by connect() here: TCP may then take
	 * over a TIME_WAIT sock's tuple (with timestamps) at once. */
	uint8_t user_port = s->bound && s->local_addr.sin_port != 0;

	if (!s->bound) {
		s->local_addr.sin_addr.s_addr =
			net_htonl(sock_source_ip(dev, dst_ip));
		s->local_addr.sin_family = AF_INET;
		s->bind_euid = current_euid();
		/* Reserves the port and sets s->bound under the port lock.
		 * The destination goes in because the port only has to be
		 * free TOWARD IT -- see alloc_ephemeral_port(). */
		alloc_ephemeral_port(s, dst_ip, net_ntohs(addr->sin_port));
	} else if (s->type == SOCK_STREAM &&
		   s->local_addr.sin_addr.s_addr == 0) {
		/* Bound to the wildcard address: a connection needs a real
		 * one, the one its segments will carry -- the TCP checksum
		 * covers it, and a connection built on 0.0.0.0 sent segments
		 * every receiver rejects.  getsockname() reports it from now
		 * on, as it does everywhere. */
		s->local_addr.sin_addr.s_addr =
			net_htonl(sock_source_ip(dev, dst_ip));
	}

	s->remote_addr = *addr;

	// TCP: initiate connection
	uint16_t dst_port = net_ntohs(addr->sin_port);
	uint16_t src_port = net_ntohs(s->local_addr.sin_port);
	uint32_t local_ip = net_ntohl(s->local_addr.sin_addr.s_addr);

	struct tcp_conn_params cp = {
		.sndbuf = s->sndbuf_size,
		.rcvbuf = s->rcvbuf_size,
		.userlocks = s->sk_userlocks,
		.syn_retries = (uint8_t)sock_tcp_xopt_get(s, TCP_SYNCNT, 0),
		.user_mss = (uint16_t)sock_tcp_xopt_get(s, TCP_MAXSEG, 0),
		.window_clamp = (uint32_t)sock_tcp_xopt_get(s, TCP_WINDOW_CLAMP,
							    0),
		.user_port = user_port,
	};
	int cerr = 0;

	cp.err = &cerr;
	tcp_conn_t *conn = tcp_connect_opts(dev, local_ip, dst_ip, src_port,
					    dst_port, &cp);
	if (!conn)
		return cerr ? cerr : -ENOMEM;

	{
		uint64_t f;

		spin_lock_irqsave(&s->lock, &f);
		s->tcp = conn;
		conn->owner_socket = s;
		spin_unlock_irqrestore(&s->lock, f);
	}
	sock_apply_tcp_opts(s, conn);

	// Wait for connection to complete (blocking).  Our own transient
	// reference keeps conn alive across the wait whatever the socket does.
	tcp_conn_get(conn);
	int rc = sock_connect_wait(s, conn);
	if (rc) {
		tcp_conn_release(conn);
		return rc;
	}

	/* connect_done can be observed without this socket's connection actually
	 * reaching the requested peer: under heavy concurrency the conn slot may
	 * have been freed and recycled to a different connection (or had its
	 * 4-tuple rewritten) while we were blocked.  Trusting connect_done/!error
	 * blindly then lets a connect to a port with no listener wrongly succeed
	 * (it should always be refused — a SYN to a closed port only ever yields a
	 * RST, never a SYN+ACK).  Only report success if this is still OUR
	 * connection to the requested destination and it has left SYN_SENT. */
	int bogus = 0;
	if (!conn->error &&
	    (conn->owner_socket != s || conn->remote_ip != dst_ip ||
	     conn->remote_port != dst_port ||
	     conn->state == TCP_STATE_SYN_SENT)) {
		bogus = 1;
		kprintf("tcp: connect %u.%u.%u.%u:%u completed without establishing to "
			"it (state=%d rport=%u mine=%d) -> ECONNREFUSED\n",
			(dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
			(dst_ip >> 8) & 0xff, dst_ip & 0xff, dst_port,
			conn->state, conn->remote_port,
			conn->owner_socket == s);
	}
	rc = sock_connect_finish(s, conn, bogus);
	tcp_conn_release(conn);
	return rc;
}

// ============================================================================
// sock_sendto - Send data (UDP or unconnected)
// ============================================================================
int sock_sendto(int sockfd, const void *buf, size_t len, int flags,
		const struct sockaddr_in *dest_addr, socklen_t addrlen)
{
	(void)flags;
	(void)addrlen;
	mm_prefault_user_range((uint64_t)buf, len, 0); // see sock_send
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	/* A pending asynchronous error (an ICMP port unreachable on a
	 * connected datagram socket, ...) is reported once, by the next
	 * send or receive. */
	if (s->error) {
		int e = (s->type == SOCK_STREAM) ? s->error :
						     dgram_take_error(s);

		if (s->type == SOCK_STREAM)
			s->error = 0;
		if (e)
			return -e;
	}

	/* Raw and UDP: the datagram goes out whole (raw.c, udp.c). */
	if (s->type == SOCK_RAW)
		return raw_sendmsg(s, buf, len, dest_addr);
	if (s->type == SOCK_DGRAM)
		return udp_sendmsg(s, buf, len, flags, dest_addr);

	// TCP sendto - ignore dest_addr, use connection
	return sock_send(sockfd, buf, len, flags);
}

// ============================================================================
// sock_recvfrom - Receive data (UDP with source address)
// ============================================================================
/* What recvmsg() needs to know about the datagram a receive dequeued,
 * beyond its bytes. */
struct sock_dgram_meta {
	uint32_t real_len; // length of the datagram as it arrived
	uint32_t dst_ip;
	uint8_t ttl;
	uint8_t tos;
	uint8_t errqueue; // from the error queue: `ee' is valid
	uint8_t _pad;
	uint32_t ifindex;
	struct sock_extended_err ee;
	uint32_t offender; // host order
	uint64_t tstamp_ns; // receive time (monotonic), 0 unknown
};

/* A datagram socket's receive: the pending error first (reported once),
 * then the oldest datagram.  MSG_ERRQUEUE reads the error queue instead
 * and never waits. */
static int sock_dgram_recv(net_socket_t *s, void *buf, size_t len, int flags,
			   struct sockaddr_in *src_addr, socklen_t *addrlen,
			   struct sock_dgram_meta *meta)
{
	int peek = (flags & MSG_PEEK) != 0;
	int dontwait = (flags & MSG_DONTWAIT) != 0;
	uint64_t deadline = s->rcv_timeout_ticks ?
				    timer_ticks() + s->rcv_timeout_ticks :
				    0;
	task_t *cur = sched_current();
	sk_buff_t *skb;
	uint32_t n;

	if (flags & MSG_ERRQUEUE) {
		struct sock_exterr_cb *ec;

		skb = dgram_dequeue_err(s);
		if (!skb)
			return -EAGAIN;
		ec = SKB_EXT_ERR(skb);
		n = skb->len < len ? skb->len : (uint32_t)len;
		if (n) {
			smap_disable();
			mm_memcpy(buf, skb->data, n);
			smap_enable();
		}
		if (meta) {
			meta->real_len = skb->len;
			meta->errqueue = 1;
			meta->ee = ec->ee;
			meta->offender = ec->offender;
			meta->dst_ip = ec->addr;
		}
		if (src_addr && addrlen &&
		    *addrlen >= sizeof(struct sockaddr_in)) {
			mm_memset(src_addr, 0, sizeof(*src_addr));
			src_addr->sin_family = AF_INET;
			src_addr->sin_port = net_htons(ec->port);
			src_addr->sin_addr.s_addr = net_htonl(ec->addr);
			*addrlen = sizeof(struct sockaddr_in);
		}
		skb_put(skb);
		return (int)n;
	}

	for (;;) {
		int e = dgram_take_error(s);

		if (e)
			return -e;
		skb = dgram_dequeue(s, peek);
		if (skb)
			break;
		/* shutdown(SHUT_RD): what was queued is read, then 0. */
		if (__atomic_load_n(&s->rx_shutdown, __ATOMIC_ACQUIRE))
			return 0;
		if (s->nonblock || dontwait)
			return -EAGAIN;
		if (deadline && timer_ticks() >= deadline)
			return -ETIMEDOUT;
		/* Interruptible: recvfrom() returns EINTR on a signal. */
		if (cur && signal_pending(cur))
			return -EINTR;
		/* Deliver any datagram already queued on the loopback device
		 * ourselves (a same-task sender's datagram sits there until
		 * someone drains it), then park on &s->udp_rx_ready, which
		 * every delivery wakes.  Mark BLOCKED, re-check, and un-park
		 * by CAS so the waker's claim never double-enqueues us. */
		loopback_process_pending();
		if (!cur) {
			sched_yield_in_kernel();
			continue;
		}
		cur->wait_channel = (void *)&s->udp_rx_ready;
		cur->wakeup_tick = deadline; /* 0 = no timeout */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		task_set_blocked(cur);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (s->udp_rx_ready) {
			task_state_t expected = TASK_BLOCKED;

			if (__atomic_compare_exchange_n(
				    &cur->state, &expected, TASK_RUNNING, false,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				cur->wait_channel = NULL;
				cur->wakeup_tick = 0;
				continue;
			}
		}
		sched_schedule();
		cur->wait_channel = NULL;
		cur->wakeup_tick = 0;
	}

	/* The datagram is ours (dequeued) or held (peeked): copy with no
	 * lock held. */
	{
		struct dgram_cb *d = DGRAM_CB(skb);

		n = skb->len < len ? skb->len : (uint32_t)len;
		if (n) {
			smap_disable();
			mm_memcpy(buf, skb->data, n);
			smap_enable();
		}
		if (meta) {
			meta->real_len = skb->len;
			meta->dst_ip = d->daddr;
			meta->ttl = d->ttl;
			meta->tos = d->tos;
			meta->ifindex = (uint32_t)d->ifindex;
			meta->tstamp_ns = skb->tstamp_ns;
		}
		if (src_addr && addrlen &&
		    *addrlen >= sizeof(struct sockaddr_in)) {
			mm_memset(src_addr, 0, sizeof(*src_addr));
			src_addr->sin_family = AF_INET;
			src_addr->sin_port = net_htons(d->sport);
			src_addr->sin_addr.s_addr = net_htonl(d->saddr);
			*addrlen = sizeof(struct sockaddr_in);
		}
	}
	skb_put(skb);
	return (int)n;
}

/* sock_recvfrom() proper.  For a datagram socket the return value is the
 * number of bytes COPIED and *meta (when given) describes the datagram,
 * including its full length; sock_recvfrom() and sock_recvmsg() turn that
 * into MSG_TRUNC. */
static int sock_recvfrom_ex(int sockfd, void *buf, size_t len, int flags,
			    struct sockaddr_in *src_addr, socklen_t *addrlen,
			    struct sock_dgram_meta *meta)
{
	mm_prefault_user_range((uint64_t)buf, len, 1); // see sock_recv
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	if (s->type == SOCK_DGRAM || s->type == SOCK_RAW)
		return sock_dgram_recv(s, buf, len, flags, src_addr, addrlen,
				       meta);

	// TCP recvfrom
	return sock_recv(sockfd, buf, len, flags);
}

int sock_recvfrom(int sockfd, void *buf, size_t len, int flags,
		  struct sockaddr_in *src_addr, socklen_t *addrlen)
{
	struct sock_dgram_meta meta;

	mm_memset(&meta, 0, sizeof(meta));
	int ret = sock_recvfrom_ex(sockfd, buf, len, flags, src_addr, addrlen,
				   &meta);
	/* MSG_TRUNC on a datagram socket: report the datagram's real length
	 * even though only `len' bytes of it were stored. */
	if (ret >= 0 && (flags & MSG_TRUNC) && meta.real_len > (uint32_t)ret)
		return meta.real_len;
	return ret;
}

// ============================================================================
// sock_send - Send data on connected socket (TCP)
// ============================================================================
static int sock_send_impl(int sockfd, const void *buf, size_t len, int flags);

/* send() on any inet socket.  The socket-level part of a stream send is
 * here: MSG_MORE is handed to TCP (the data of this call may be held back
 * for more to come, as with TCP_CORK -- TCP reads it from the socket), and
 * a write to a stream whose sending side is gone raises SIGPIPE as well as
 * failing with EPIPE, unless MSG_NOSIGNAL. */
int sock_send(int sockfd, const void *buf, size_t len, int flags)
{
	net_socket_t *s = sock_get(sockfd);
	int r;

	if (s && s->type == SOCK_STREAM)
		__atomic_store_n(&s->tx_more, (flags & MSG_MORE) ? 1 : 0,
				 __ATOMIC_RELEASE);
	r = sock_send_impl(sockfd, buf, len, flags);
	if (r == -EPIPE && !(flags & MSG_NOSIGNAL) && s &&
	    s->type == SOCK_STREAM)
		sock_sigpipe();
	return r;
}

static int sock_send_impl(int sockfd, const void *buf, size_t len, int flags)
{
	might_sleep();
	BUG_ON(buf == NULL && len > 0);
	/* Demand paging shield: the copy below runs under conn->lock with
	 * IRQs off — fault-in the source buffer first. */
	mm_prefault_user_range((uint64_t)buf, len, 0);
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	if (s->type == SOCK_STREAM) {
		tcp_conn_t *pc = sock_tcp_get(s);

		if (!pc)
			return -ENOTCONN;
		// Allow sends when the TCP layer is established even if sock_connect()
		// returned EINPROGRESS (non-blocking path) before setting s->connected.
		//
		// ENOTCONN is for a connection that NEVER completed.  One that
		// established and then left (server closed a pooled keep-alive
		// conn before this socket's SO_ERROR check ever ran, so
		// s->connected was never set) must report ECONNRESET/EPIPE --
		// those are what a connection pool treats as "stale, retry on
		// a fresh connection"; ENOTCONN surfaces as a user-visible
		// failure.  conn->connect_done is the TCP layer's own record
		// of having completed the handshake.
		{
			int pst = __atomic_load_n(&pc->state, __ATOMIC_ACQUIRE);
			int perr = 0;

			if (!s->connected && !pc->connect_done &&
			    pst != TCP_STATE_ESTABLISHED && !s->tx_shutdown) {
				tcp_conn_release(pc);
				return -ENOTCONN;
			}
			/* Our own half-close (the write direction is gone,
			 * whatever state the connection is in), or a state
			 * that sends no more.  CLOSE_WAIT still sends: the
			 * peer's FIN closed only ITS direction (a server
			 * answers a client that half-closes after its
			 * request).  A pending error (a reset) is reported
			 * once, in place of the EPIPE. */
			if (s->tx_shutdown || (pst != TCP_STATE_ESTABLISHED &&
					       pst != TCP_STATE_CLOSE_WAIT)) {
				perr = tcp_sock_error(pc);
				tcp_conn_release(pc);
				return perr ? -perr : -EPIPE;
			}
			tcp_conn_release(pc);
		}

		/* Send the WHOLE buffer before returning, not just the part
		 * that happened to fit.
		 *
		 * tcp_send_data queues what the congestion window, the peer's
		 * receive window and the inflight ring allow, and reports that
		 * -- which for a large write is a fraction of it.  Returning
		 * that count is legal for send(2), but it is not what a
		 * BLOCKING socket does anywhere else, and a great deal of
		 * software is written against the behaviour rather than the
		 * standard: gdbserver's putpkt writes a packet with one
		 * write() and treats any short count as a failure, so a
		 * session died the moment gdb asked for anything larger than
		 * the congestion window -- reported as "putpkt(write): No such
		 * file or directory", the errno being stale because there was
		 * no error, only a short write.
		 *
		 * The 64K clamp below is the same trap in a second form:
		 * tcp_send_data takes a uint16_t, so a write larger than that
		 * was ALWAYS short, however open the window was.  It is now a
		 * chunk size rather than a limit.
		 *
		 * Three things end the loop early, and each returns the count
		 * so far rather than an error, because bytes already queued
		 * have been sent and the caller must be told so:
		 *   - an error from TCP: reported on the next call, when there
		 *     is nothing left to lose by reporting it;
		 *   - a nonblocking socket with a full window: EAGAIN only if
		 *     nothing at all went out;
		 *   - a pending signal.  The old loop had no escape at all: a
		 *     task whose peer stopped reading spun on
		 *     sched_yield_in_kernel() for ever and could not even be
		 *     killed. */
		if (len == 0)
			return 0;

		size_t total = 0;
		/* SO_SNDTIMEO bounds the wait for room, as everywhere: on
		 * expiry the bytes sent so far, or EAGAIN.  It was accepted
		 * and ignored. */
		uint64_t snd_deadline =
			s->snd_timeout_ticks ?
				timer_ticks() + s->snd_timeout_ticks :
				0;

		while (total < len) {
			size_t remain = len - total;
			uint16_t to_send = remain > 0xFFFF ? 0xFFFF :
							     (uint16_t)remain;

			// Re-snapshot s->tcp under s->lock each iteration and take
			// a reference on it, so a concurrent sock_close() (which
			// detaches s->tcp and drops the socket reference) cannot
			// free the connection out from under tcp_send_data.
			uint64_t sflags;
			spin_lock_irqsave(&s->lock, &sflags);
			tcp_conn_t *conn = s->active ? s->tcp : NULL;
			if (conn)
				tcp_conn_get(conn);
			int connected =
				s->connected ||
				(conn && (conn->connect_done ||
					  conn->state ==
						  TCP_STATE_ESTABLISHED));
			int nonblock = s->nonblock || (flags & MSG_DONTWAIT);
			spin_unlock_irqrestore(&s->lock, sflags);
			if (!conn || !connected) {
				if (conn)
					tcp_conn_release(conn);
				return total ? (int)total : -ENOTCONN;
			}
			if (conn->state != TCP_STATE_ESTABLISHED &&
			    conn->state != TCP_STATE_CLOSE_WAIT) {
				/* What was queued is reported; the error, if
				 * any, by the next call (once). */
				int err = total ? 0 : tcp_sock_error(conn);

				tcp_conn_release(conn);
				return total ? (int)total : -(err ? err : EPIPE);
			}

			/* MSG_OOB: the last byte of the write is the urgent
			 * one -- the urgent pointer goes just past it (see
			 * tcp_send_data_urg); set by the call that carries it. */
			int urg = (flags & MSG_OOB) && total + to_send == len;

			smap_disable();
			int ret = tcp_send_data_urg(
				conn, (const uint8_t *)buf + total, to_send, urg);
			smap_enable();

			if (ret < 0) {
				if (ret == -EPIPE && !total) {
					int err = tcp_sock_error(conn);

					if (err)
						ret = -err;
				}
				tcp_conn_release(conn);
				return total ? (int)total : ret;
			}
			if (ret > 0) {
				tcp_conn_release(conn);
				total += (size_t)ret;
				continue;
			}

			/* Nothing fit: the window is closed or the inflight
			 * ring is full. */
			if (nonblock) {
				tcp_conn_release(conn);
				return total ? (int)total : -EAGAIN;
			}

			task_t *tx_cur = sched_current();

			if (tx_cur && signal_pending(tx_cur)) {
				tcp_conn_release(conn);
				return total ? (int)total : -EINTR;
			}
			if (snd_deadline && timer_ticks() >= snd_deadline) {
				tcp_conn_release(conn);
				return total ? (int)total : -EAGAIN;
			}

			loopback_process_pending();
			/* Sleep until the window opens: tx_ready rising is
			 * notified.  A refusal with tx_ready still set has no
			 * event to wait for, so that case only yields. */
			if (__atomic_load_n(&conn->tx_ready, __ATOMIC_ACQUIRE))
				sched_yield_in_kernel();
			else
				sock_wait_event(s, sock_send_ready, conn,
						snd_deadline);
			tcp_conn_release(conn);
		}

		return (int)total;
	}

	// datagram sockets: to the connected peer
	return sock_sendto(sockfd, buf, len, flags, NULL, 0);
}

/* No more data will ever arrive on this connection: the peer's FIN has been
 * received (CLOSE_WAIT and everything after it) or the connection is dead.
 * NOT FIN_WAIT_1/FIN_WAIT_2 -- those mean WE closed our send direction and
 * the peer may still be transmitting; a receiver must keep waiting there
 * (shutdown(SHUT_WR) then read-the-response is the standard client pattern,
 * and treating those states as EOF cut the response off). */
static int tcp_rx_finished(int state)
{
	return state == TCP_STATE_CLOSED || state == TCP_STATE_CLOSE_WAIT ||
	       state == TCP_STATE_CLOSING || state == TCP_STATE_LAST_ACK ||
	       state == TCP_STATE_TIME_WAIT;
}

// ============================================================================
// sock_recv - Receive data on connected socket (TCP)
// ============================================================================
int sock_recv(int sockfd, void *buf, size_t len, int flags)
{
	might_sleep();
	BUG_ON(buf == NULL && len > 0);
	/* Demand paging shield: destination copy runs under conn->lock —
	 * fault-in (and COW-resolve) the buffer first. */
	mm_prefault_user_range((uint64_t)buf, len, 1);
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	int peek = (flags & MSG_PEEK) != 0;
	int dontwait = (flags & MSG_DONTWAIT) != 0;
	int waitall = (flags & MSG_WAITALL) != 0;

	if (s->type == SOCK_STREAM) {
		// Snapshot the connection under s->lock and hold a reference for
		// the whole recv, so a concurrent sock_close() (which detaches
		// s->tcp and drops the socket reference) cannot free the
		// connection while we park on conn->rx_ready or copy from its rx
		// ring.  Every exit of this path drops the reference (goto
		// recv_out).
		uint64_t hflags;
		spin_lock_irqsave(&s->lock, &hflags);
		tcp_conn_t *conn = s->active ? s->tcp : NULL;
		if (conn)
			tcp_conn_get(conn);
		spin_unlock_irqrestore(&s->lock, hflags);
		if (!conn)
			return -ENOTCONN;

		uint64_t deadline =
			s->rcv_timeout_ticks ?
				timer_ticks() + s->rcv_timeout_ticks :
				0;
		size_t total = 0;
		int rc;

		/* recv(MSG_OOB): the urgent byte, never waiting for it. */
		if (flags & MSG_OOB) {
			uint8_t byte = 0;
			int r = tcp_recv_urg(conn, peek, &byte);

			if (r == 1 && len) {
				if (copy_to_user(buf, &byte, 1))
					r = -EFAULT;
			} else if (r == 1) {
				r = 0; // no room: the byte stays (MSG_TRUNC)
			}
			rc = r;
			goto recv_out;
		}
again:
		while (1) {
			uint64_t sflags;
			spin_lock_irqsave(&s->lock, &sflags);
			int nonblock = s->nonblock;
			int detached = !s->active || s->tcp != conn;
			spin_unlock_irqrestore(&s->lock, sflags);
			if (detached) {
				// The socket released this connection; it is
				// failed/closing and will report EOF/error below.
				rc = total ? (int)total : -EBADF;
				goto recv_out;
			}
			/* Stop waiting when data is ready, the connection
			 * erred, no more data can arrive, or our own read
			 * direction was shut down.  A connection we half
			 * closed (FIN_WAIT_1/2) keeps waiting: the peer is
			 * still entitled to send. */
			/* SO_RCVLOWAT: a blocking read waits for that much
			 * (or for what fits the buffer); a non-blocking one
			 * takes what is there. */
			uint32_t lowat = s->rcvlowat > 1 ? (uint32_t)s->rcvlowat :
							   1;
			if (lowat > len - total)
				lowat = (uint32_t)(len - total);
			int rx_ok = (conn->rx_ready || tcp_inq(conn) > 0) &&
				    (nonblock || dontwait || peek ||
				     tcp_inq(conn) >= lowat);
			if (rx_ok || conn->error ||
			    tcp_rx_finished(conn->state) || s->rx_shutdown)
				break;
			if (nonblock || dontwait) {
				rc = total ? (int)total : -EAGAIN;
				goto recv_out;
			}
			if (deadline && timer_ticks() >= deadline) {
				rc = total ? (int)total : -ETIMEDOUT;
				goto recv_out;
			}
			/* Interruptible: without this, a task parked here with a
			 * pending fatal signal is re-woken by the timer sweep and
			 * immediately re-parks, forever — it can never die. */
			{
				task_t *rx_cur = sched_current();
				if (rx_cur && signal_pending(rx_cur)) {
					rc = total ? (int)total : -EINTR;
					goto recv_out;
				}
			}
			loopback_process_pending();

			/* Park on &conn->rx_ready instead of busy-yielding.  The previous
             * yield-based loop only checked rx_ready once per timer tick
             * (~10 ms at 100 Hz), capping throughput at ~146 KB/s for a
             * 1460-byte MSS.  The receive path now sched_wake_channel(&conn->rx_ready)
             * the moment data is delivered, eliminating the tick-quantum
             * latency.  Standard double-check pattern to avoid the race
             * where the waker fires between our check and our block. */
			task_t *cur = sched_current();
			if (cur) {
				cur->wait_channel = (void *)&conn->rx_ready;
				/* SO_RCVTIMEO: arm the sleep timer so the park
				 * wakes at the deadline.  Without this the
				 * deadline was only checked when something else
				 * happened to wake the task — a receiver whose
				 * peer went silent slept forever. */
				cur->wakeup_tick = deadline;
				__atomic_thread_fence(__ATOMIC_SEQ_CST);
				task_set_blocked(cur);
				__atomic_thread_fence(__ATOMIC_SEQ_CST);
				/* Re-check after marking blocked.  If the waker fired
                 * between the first check and now, rx_ready / state will
                 * already reflect the new value — unblock and loop.
                 * Un-park via CAS (mirror of sched_claim_wake): a blind
                 * state=RUNNING write here races the waker's claim and
                 * produces "BUG: enqueue RUNNING" at rq_enqueue_locked.
                 * If the CAS fails the waker already claimed us and is
                 * enqueueing: wait for the entry to land and reconcile
                 * through sched_schedule (it picks us straight back). */
				/* Waiting for more than is there: re-arm
				 * the wake on the next arrival (the TCP
				 * layer wakes readers on rx_ready rising).
				 * Store, full fence, then the count: pairs
				 * with tcp_wake_reader() (count stored, then
				 * rx_ready exchanged), so one of the two
				 * sides always sees the other. */
				if (lowat > 1 && tcp_inq(conn) < lowat) {
					__atomic_store_n(&conn->rx_ready, 0,
							 __ATOMIC_SEQ_CST);
					__atomic_thread_fence(__ATOMIC_SEQ_CST);
				}
				if (tcp_inq(conn) >= lowat ||
				    conn->error ||
				    tcp_rx_finished(conn->state) ||
				    s->rx_shutdown) {
					task_state_t expected = TASK_BLOCKED;
					if (__atomic_compare_exchange_n(
						    &cur->state, &expected,
						    TASK_RUNNING, false,
						    __ATOMIC_ACQ_REL,
						    __ATOMIC_ACQUIRE)) {
						cur->wait_channel = NULL;
						cur->wakeup_tick = 0;
						continue;
					}
					/* Waker won the claim: we are READY
					 * and it enqueues us.  Never write
					 * RUNNING over the claim and never
					 * wait for on_rq (unbounded under
					 * preemption) — fall through to
					 * sched_schedule() as READY current;
					 * the queue entry brings us straight
					 * back and the loop re-checks. */
				}
			}
			sched_schedule();
			if (cur) {
				cur->wait_channel = NULL;
				cur->wakeup_tick = 0;
			}
		}

		/* Data first: what arrived in order before a reset (or the
		 * peer's FIN) is delivered before either is reported.  With
		 * the queue empty, a pending error is returned -- once (it is
		 * taken), unless this call already got data; after it the
		 * connection reads end of file. */
		if (tcp_inq(conn) == 0) {
			if (__atomic_load_n(&conn->error, __ATOMIC_ACQUIRE)) {
				int err = total ? 0 : tcp_sock_error(conn);

				rc = total ? (int)total : (err ? -err : 0);
				goto recv_out;
			}
			// No more data will come (peer FIN received / conn
			// dead / SHUT_RD applied here): end of file.
			if (tcp_rx_finished(conn->state) || s->rx_shutdown) {
				rc = (int)total;
				goto recv_out;
			}
		}

		/* Copy from the receive queue (skbs).  The TCP layer takes
		 * its lock, frees what was read, advertises the window the
		 * read opened and tunes the receive buffer. */
		smap_disable();
		uint32_t copy = (uint32_t)tcp_recv_copy(
			conn, (uint8_t *)buf + total, len - total, peek);
		smap_enable();

		/* Defensive re-wait: rx_ready fired but the ring was empty (e.g. a
         * zero-window probe arrived on a full ring — nothing queued by the receive path).
         * Returning 0 here would look like EOF to OpenSSL.  Re-enter the
         * wait loop on a blocking ESTABLISHED socket instead. */
		if (copy == 0 &&
		    (conn->state == TCP_STATE_ESTABLISHED ||
		     conn->state == TCP_STATE_FIN_WAIT_1 ||
		     conn->state == TCP_STATE_FIN_WAIT_2) &&
		    !s->rx_shutdown && !peek) {
			uint64_t sfl;
			spin_lock_irqsave(&s->lock, &sfl);
			int nb2 = s->nonblock;
			spin_unlock_irqrestore(&s->lock, sfl);
			if (!nb2 && !dontwait)
				goto again;
			/* Nothing to read after all (only an out-of-band
			 * byte was queued): not end of file. */
			if (!total) {
				rc = -EAGAIN;
				goto recv_out;
			}
		}

		total += copy;

		if (waitall && !peek && total < len &&
		    conn->state == TCP_STATE_ESTABLISHED)
			goto again;
		rc = (int)total;
recv_out:
		// Drop the reference taken at entry.
		tcp_conn_release(conn);
		return rc;
	}

	// UDP recv (connected, no src addr)
	return sock_recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

// ============================================================================
// sock_close - Close a socket
// ============================================================================
int sock_close(int sockfd)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];

	// Atomically claim teardown ownership under s->lock:
	//   * The !active check, the ref_count decrement, and the s->tcp
	//     detach must be one critical section; otherwise two concurrent
	//     close()s on the same fd (e.g. dup2 in the parent racing with
	//     fork-child cleanup) can both pass the !active check, both
	//     decrement, and both proceed into teardown — observed as a
	//     ref_count wrap to negative followed by random kernel state
	//     corruption (since two CPUs end up walking the same connection
	//     in parallel through the close path).
	//   * The lock is released BEFORE calling tcp_close()/tcp_abort() so
	//     we never hold s->lock with IRQs disabled across slab_free /
	//     TLB-shootdown — see the comment further down.
	uint64_t flags;
	spin_lock_irqsave(&s->lock, &flags);
	if (!s->active) {
		spin_unlock_irqrestore(&s->lock, flags);
		return -EBADF;
	}
	int new_ref = __atomic_sub_fetch(&s->ref_count, 1, __ATOMIC_ACQ_REL);
	if (new_ref > 0) {
		spin_unlock_irqrestore(&s->lock, flags);
		return 0;
	}
	if (new_ref < 0) {
		// Defensive: should not happen with the active+lock guard above,
		// but if it ever does (corrupted ref_count from a sibling bug)
		// clamp and bail rather than tear down a slot we don't own.
		__atomic_store_n(&s->ref_count, 0, __ATOMIC_RELEASE);
		spin_unlock_irqrestore(&s->lock, flags);
		return -EBADF;
	}

	// Snapshot the teardown plan, then DETACH the connection from the
	// socket BEFORE releasing s->lock.  Concurrent sock_send/sock_recv
	// re-checks s->active and s->tcp under s->lock and will return
	// -EBADF/-ENOTCONN immediately, so they cannot race with the slab
	// frees inside tcp_close/tcp_abort.
	tcp_conn_t *tcp_to_teardown = s->tcp;
	uint8_t do_linger_block = 0;
	uint16_t linger_secs = 0;
	uint8_t use_abort = 0;
	if (tcp_to_teardown) {
		if (s->linger_onoff && s->linger_seconds == 0) {
			use_abort = 1;
		} else if (s->linger_onoff && s->linger_seconds > 0) {
			do_linger_block = 1;
			linger_secs = s->linger_seconds;
		}
	}
	// Detach the connection from the socket.  We still hold a reference on
	// it (the socket reference), so it stays alive across the teardown below
	// even though sock_send/recv (which re-check s->tcp under s->lock) can
	// no longer reach it.
	if (tcp_to_teardown)
		tcp_to_teardown->owner_socket = NULL;
	s->tcp = NULL;
	s->closed = 1;
	/* The delivery paths test `active' and `closed' under s->lock:
	 * nothing is queued to the socket after this.  The slot stays
	 * reserved (`hashed') until its teardown below is done -- for every
	 * type, since epoll items keyed on this slot are removed below and a
	 * new socket in it must not be seen by them. */
	s->hashed = 1;
	s->active = 0;
	spin_unlock_irqrestore(&s->lock, flags);

	/* epoll sets watching this socket let go of it, while its poll queue
	 * still belongs to it. */
	eventpoll_release(MAKE_SOCKET_FD(sockfd));

	/* A datagram socket leaves its lookup table, then its queues are
	 * emptied.  The slot is not reused (sock_create) before `hashed'
	 * is cleared at the end. */
	if (s->type == SOCK_DGRAM)
		udp_destroy(s);
	else if (s->type == SOCK_RAW)
		raw_unhash(s);
	if (s->type != SOCK_STREAM)
		dgram_purge(s, 0);
	/* Group memberships end with the socket. */
	for (int g = 0; g < NET_MAX_MCAST_GROUPS; g++) {
		if (s->mcast_groups[g]) {
			uint32_t grp = s->mcast_groups[g];
			net_device_t *gdev = s->mcast_devs[g];

			s->mcast_groups[g] = 0;
			s->mcast_devs[g] = NULL;
			if (gdev)
				igmp_leave(gdev, grp);
		}
	}
	__atomic_store_n(&s->hashed, 0, __ATOMIC_RELEASE);

	// Do NOT hold s->lock across tcp_close/tcp_abort (they send FIN/RST and
	// may free memory → TLB-shootdown IPIs).  We hold the socket reference,
	// so tcp_to_teardown is a stable, valid pointer here.
	if (tcp_to_teardown) {
		if (use_abort) {
			// SO_LINGER l_onoff=1, l_linger=0 → RST (RFC 1122 §4.2.2.13)
			tcp_abort(tcp_to_teardown);
		} else {
			tcp_close(tcp_to_teardown);
			if (do_linger_block) {
				uint64_t deadline =
					timer_ticks() +
					timer_s_to_ticks(linger_secs);
				// We hold a reference, so the deref is always
				// memory-safe.
				/* The socket has let go of the connection, so
				 * nothing notifies it any more: look again every
				 * millisecond rather than spinning on a yield. */
				while (tcp_to_teardown->state !=
					       TCP_STATE_CLOSED &&
				       tcp_to_teardown->state !=
					       TCP_STATE_TIME_WAIT &&
				       timer_ticks() < deadline) {
					if (hrtimer_sleep_until(hrtimer_now_ns() +
								1000000ULL,
								NULL))
						break;
				}
			}
		}
		// Drop the socket reference; the connection is freed once its
		// protocol reference is also gone (at teardown completion).
		tcp_conn_release(tcp_to_teardown);
	}
	return 0;
}

// ============================================================================
// sock_shutdown - Shutdown part of a connection
// ============================================================================
int sock_shutdown(int sockfd, int how)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
		return -EINVAL;

	/* shutdown() closes DIRECTIONS, not the socket.  This used to ignore
	 * `how` and do everything close() does -- detach the connection, clear
	 * `connected`, FIN both ways -- so shutdown(SHUT_WR), the half-close
	 * that TLS teardown and request/EOF-response clients use, destroyed
	 * the receive path too, and the next operation found no connection at
	 * all and failed ENOTCONN ("Transport endpoint is not connected" from
	 * a browser's connection pool was this).  The connection stays
	 * attached: tcp_close() runs the proper FIN state machine
	 * (ESTABLISHED -> FIN_WAIT_1, CLOSE_WAIT -> LAST_ACK), FIN_WAIT_1/2
	 * still consume inbound data, and close() later finishes an
	 * already-closing connection harmlessly (its switch treats the
	 * closing states as no-ops). */
	int rd = how == SHUT_RD || how == SHUT_RDWR;
	int wr = how == SHUT_WR || how == SHUT_RDWR;

	/* A datagram socket records the directions and wakes its readers
	 * (a read then returns 0).  Not connected, it is applied all the
	 * same, and ENOTCONN reported, as everywhere. */
	if (s->type != SOCK_STREAM) {
		if (rd)
			__atomic_store_n(&s->rx_shutdown, 1, __ATOMIC_RELEASE);
		if (wr)
			__atomic_store_n(&s->tx_shutdown, 1, __ATOMIC_RELEASE);
		dgram_wake(s);
		return s->connected ? 0 : -ENOTCONN;
	}

	tcp_conn_t *c = sock_tcp_get(s);
	int ret = 0;

	if (!c) {
		/* Never connected: ENOTCONN, the directions noted all the
		 * same (poll() reports them). */
		if (rd)
			__atomic_store_n(&s->rx_shutdown, 1, __ATOMIC_RELEASE);
		if (wr)
			__atomic_store_n(&s->tx_shutdown, 1, __ATOMIC_RELEASE);
		poll_notify_wq(&s->poll_wq);
		return -ENOTCONN;
	}
	if (s->listening) {
		/* A listener has no sending side: SHUT_WR does nothing.  Shutting
		 * its receiving side stops it listening, as close() would, and
		 * a thread in accept() is woken to fail. */
		if (rd && !s->tx_shutdown) {
			s->rx_shutdown = 1;
			s->tx_shutdown = 1;
			tcp_close(c);
			poll_notify_wq(&s->poll_wq);
		}
		tcp_conn_release(c);
		return 0;
	}
	/* Reset, or both FINs through: the connection is gone (ENOTCONN). */
	if (c->state == TCP_STATE_CLOSED || c->state == TCP_STATE_TIME_WAIT)
		ret = -ENOTCONN;
	if (rd) {
		s->rx_shutdown = 1;
		/* A reader parked in sock_recv must wake to see it. */
		sched_wake_channel((void *)&c->rx_ready);
		poll_notify_wq(&s->poll_wq);
	}
	if (wr && !s->tx_shutdown) {
		s->tx_shutdown = 1;
		if (!ret)
			tcp_shutdown(c);
		poll_notify_wq(&s->poll_wq);
	}
	tcp_conn_release(c);
	return ret;
}

// ============================================================================
// sock_setsockopt / sock_getsockopt
// ============================================================================
/* SO_RCVTIMEO / SO_SNDTIMEO value -> ticks, *out unchanged when optlen
 * says nothing usable.  struct timeval is { int64_t tv_sec, int64_t
 * tv_usec } = 16 bytes, converted at the calibrated tick rate.  Legacy
 * callers passing a raw millisecond count (8 or 4 bytes) are converted
 * the same way. */
static void sock_parse_timeo(const void *optval, socklen_t optlen, int ival,
			     uint64_t *out)
{
	uint32_t hz = timer_get_frequency();
	if (hz == 0)
		hz = 100;
	if (optlen >= 2 * (int)sizeof(uint64_t)) {
		const int64_t *tv = (const int64_t *)optval;
		int64_t sec = tv[0] > 0 ? tv[0] : 0;
		int64_t usec = tv[1] > 0 ? tv[1] : 0;
		/* usec_per_tick = 1000000 / hz; avoid division: ticks =
		 * sec*hz + usec*hz/1000000 */
		*out = (uint64_t)sec * hz + (uint64_t)usec * hz / 1000000ULL;
	} else if (optlen >= (int)sizeof(uint64_t)) {
		*out = timer_ms_to_ticks(*(const uint64_t *)optval);
	} else if (optlen >= (int)sizeof(int)) {
		*out = timer_ms_to_ticks((uint64_t)ival);
	}
}

int sock_setsockopt(int sockfd, int level, int optname, const void *optval,
		    socklen_t optlen)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	int ival = 0;
	if (optval && optlen >= sizeof(int))
		ival = *(const int *)optval;

	if (level == SOL_SOCKET) {
		switch (optname) {
		case SO_REUSEADDR:
			s->reuse_addr = ival;
			return 0;
		case SO_REUSEPORT:
			s->reuse_port = ival;
			return 0;
		case SO_BROADCAST:
			s->broadcast = ival;
			return 0;
		case SO_OOBINLINE:
			s->oobinline = ival ? 1 : 0;
			if (s->type == SOCK_STREAM) {
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					tcp_set_oobinline(c, s->oobinline);
					tcp_conn_release(c);
				}
			}
			return 0;
		case SO_KEEPALIVE:
			s->tcp_opt_keepalive = ival ? 1 : 0;
			s->tcp_opt_set |= SOCK_TCPOPT_KEEPALIVE;
			if (s->type == SOCK_STREAM) {
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					tcp_setsockopt_int(c, TCP_SOCKOPT_KEEPALIVE,
							   ival ? 1 : 0);
					tcp_conn_release(c);
				}
			}
			return 0;
		case SO_SNDBUF: {
			int v = ival < 0 ? 0 :
				ival > SOCK_WMEM_MAX ? SOCK_WMEM_MAX : ival;

			s->sndbuf_size = v * 2 > SOCK_MIN_SNDBUF ?
						 v * 2 :
						 SOCK_MIN_SNDBUF;
			if (s->type == SOCK_STREAM) {
				/* Set by the application: no auto-tuning. */
				s->sk_userlocks |= TCP_SNDBUF_LOCK;
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					tcp_set_buffers(c, s->sndbuf_size, 0,
							TCP_SNDBUF_LOCK);
					tcp_conn_release(c);
				}
			}
			return 0;
		}
		case SO_RCVBUF: {
			int v = ival < 0 ? 0 :
				ival > SOCK_RMEM_MAX ? SOCK_RMEM_MAX : ival;

			s->rcvbuf_size = v * 2 > SOCK_MIN_RCVBUF ?
						 v * 2 :
						 SOCK_MIN_RCVBUF;
			if (s->type == SOCK_STREAM) {
				s->sk_userlocks |= TCP_RCVBUF_LOCK;
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					tcp_set_buffers(c, 0, s->rcvbuf_size,
							TCP_RCVBUF_LOCK);
					tcp_conn_release(c);
				}
			}
			return 0;
		}
		case SO_RCVLOWAT:
			if (optlen < sizeof(int))
				return -EINVAL;
			/* Negative: as large as there is; 0 means 1. */
			s->rcvlowat = ival < 0 ? 0x7fffffff : ival ? ival : 1;
			poll_notify_wq(&s->poll_wq);
			return 0;
		case SO_SNDLOWAT:
			/* Fixed at 1, and not settable, as everywhere. */
			return -ENOPROTOOPT;
		case SO_TIMESTAMP:
		case SO_TIMESTAMPNS:
			if (optlen < sizeof(int))
				return -EINVAL;
			/* The two are one switch: setting either picks the
			 * format, clearing either turns time stamps off. */
			s->rcv_tstamp = !ival ? 0 :
					optname == SO_TIMESTAMPNS ? 2 : 1;
			return 0;
		case SO_MARK:
			if (optlen < sizeof(int))
				return -EINVAL;
			if (!capable())
				return -EPERM;
			s->sk_mark = (uint32_t)ival;
			return 0;
		case SO_PRIORITY:
			if (optlen < sizeof(int))
				return -EINVAL;
			/* 0..6 for everybody, the rest privileged. */
			if ((ival < 0 || ival > 6) && !capable())
				return -EPERM;
			s->sk_priority = ival;
			return 0;
		case SO_PEEK_OFF:
			/* Peeking at an offset is not implemented for any
			 * protocol here; say so rather than ignore it. */
			return -EOPNOTSUPP;
		case SO_MAX_PACING_RATE: {
			/* Bytes per second: 32 bits (~0U: no cap), or 64
			 * with a 64-bit value.  Any value but "no cap" also
			 * makes TCP pace the connection itself (it does not
			 * by default). */
			uint64_t rate;

			if (!optval || optlen < sizeof(uint32_t))
				return -EINVAL;
			if (optlen >= sizeof(uint64_t))
				rate = *(const uint64_t *)optval;
			else
				rate = *(const uint32_t *)optval == ~0U ?
					       ~0ULL :
					       *(const uint32_t *)optval;
			s->max_pacing_rate = rate;
			if (rate != ~0ULL)
				s->pacing_set = 1;
			if (s->type == SOCK_STREAM && s->pacing_set) {
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					tcp_set_max_pacing_rate(c, rate);
					tcp_conn_release(c);
				}
			}
			return 0;
		}
		case SO_RCVTIMEO:
			sock_parse_timeo(optval, optlen, ival,
					 &s->rcv_timeout_ticks);
			return 0;
		case SO_SNDTIMEO:
			/* Bounds blocking send() and connect() -- see
			 * sock_send() and sock_connect_wait(). */
			sock_parse_timeo(optval, optlen, ival,
					 &s->snd_timeout_ticks);
			return 0;
		case SO_LINGER: {
			if (optlen >= sizeof(struct linger)) {
				const struct linger *l =
					(const struct linger *)optval;
				s->linger_onoff = (uint8_t)l->l_onoff;
				s->linger_seconds = (uint16_t)l->l_linger;
			}
			return 0;
		}
		case SO_BINDTODEVICE: {
			socklen_t n = optlen;
			char name[16];
			net_device_t *bd = NULL;

			if (n > 15)
				n = 15;
			for (socklen_t i = 0; i < n; i++)
				name[i] = ((const char *)optval)[i];
			name[n] = 0;
			if (name[0]) {
				// a device of that name, or ENODEV
				net_device_t *lo = net_get_loopback();
				int cnt = net_device_count();

				for (int d = -1; d < cnt && !bd; d++) {
					net_device_t *dd =
						d < 0 ? lo : net_get_device(d);
					int k = 0;

					if (!dd || !dd->name)
						continue;
					while (k < 15 && name[k] &&
					       dd->name[k] == name[k])
						k++;
					if (name[k] == 0 && dd->name[k] == 0)
						bd = dd;
				}
				if (!bd)
					return -ENODEV;
			}
			mm_memset(s->bindtodevice, 0, sizeof(s->bindtodevice));
			for (socklen_t i = 0; i < n; i++)
				s->bindtodevice[i] = name[i];
			s->bound_ifindex = bd ? bd->ifindex : 0;
			return 0;
		}
		default:
			return -ENOPROTOOPT;
		}
	}

	if (level == SOL_IP || level == IPPROTO_IP) {
		switch (optname) {
		case IP_TTL:
			// -1: the default; otherwise 1..255
			if (ival == -1)
				ival = 64;
			if (ival < 1 || ival > 255)
				return -EINVAL;
			s->ip_ttl = (uint8_t)ival;
			return 0;
		case IP_TOS:
			s->ip_tos = (uint8_t)ival;
			return 0;
		case IP_HDRINCL:
			s->ip_hdrincl = ival ? 1 : 0;
			return 0;
		case IP_PKTINFO:
			s->ip_recvpktinfo = ival ? 1 : 0;
			return 0;
		case IP_RECVTTL:
			s->ip_recvttl = ival ? 1 : 0;
			return 0;
		case IP_RECVTOS:
			s->ip_recvtos = ival ? 1 : 0;
			return 0;
		case IP_MTU_DISCOVER:
			if (ival < IP_PMTUDISC_DONT || ival > IP_PMTUDISC_OMIT)
				return -EINVAL;
			s->pmtudisc = (uint8_t)ival;
			return 0;
		case IP_RECVERR:
			s->ip_recverr = ival ? 1 : 0;
			if (!ival)
				dgram_purge(s, 1); // what was queued goes
			return 0;
		case IP_MULTICAST_ALL:
			s->mc_all = ival ? 1 : 0;
			return 0;
		case IP_OPTIONS:
			return 0; // accepted; not emitted
		case IP_MULTICAST_TTL:
			// -1: the default (1); otherwise 0..255
			if (optlen >= sizeof(int)) {
				if (ival == -1)
					ival = 1;
				if (ival < 0 || ival > 255)
					return -EINVAL;
			} else if (optval && optlen >= 1) {
				ival = *(const uint8_t *)optval;
			}
			s->mcast_ttl = (uint8_t)ival;
			return 0;
		case IP_MULTICAST_LOOP:
			s->mcast_loop = ival ? 1 : 0;
			return 0;
		case IP_MULTICAST_IF: {
			uint32_t a;

			if (optlen < sizeof(uint32_t))
				return -EINVAL;
			/* struct in_addr, or struct ip_mreq(n) whose second
			 * field is the interface address */
			a = net_ntohl(((const uint32_t *)optval)[
				optlen >= 2 * sizeof(uint32_t) ? 1 : 0]);
			if (a && !ip_dev_by_addr(a))
				return -EADDRNOTAVAIL;
			s->mcast_if = a;
			return 0;
		}
		case IP_ADD_MEMBERSHIP: {
			if (optlen < sizeof(struct ip_mreq))
				return -EINVAL;
			const struct ip_mreq *mr =
				(const struct ip_mreq *)optval;
			uint32_t group = net_ntohl(mr->imr_multiaddr.s_addr);
			uint32_t ifa = net_ntohl(mr->imr_interface.s_addr);
			net_device_t *dev;
			int rc;

			if ((group >> 28) != 0xE)
				return -EINVAL;
			/* the interface with that address, or the default
			 * one (the loopback device on a machine without) */
			dev = ifa ? ip_dev_by_addr(ifa) :
				    net_get_default_device();
			if (!dev && !ifa)
				dev = net_get_loopback();
			if (!dev)
				return ifa ? -EADDRNOTAVAIL : -ENODEV;
			// Track on socket so we can leave on close.
			int slot = -1;
			for (int i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
				if (s->mcast_groups[i] == group &&
				    s->mcast_devs[i] == dev)
					return -EADDRINUSE;
				if (slot < 0 && s->mcast_groups[i] == 0)
					slot = i;
			}
			if (slot < 0)
				return -ENOBUFS;
			rc = igmp_join(dev, group);
			if (rc < 0)
				return rc;
			s->mcast_groups[slot] = group;
			s->mcast_devs[slot] = dev;
			return 0;
		}
		case IP_DROP_MEMBERSHIP: {
			if (optlen < sizeof(struct ip_mreq))
				return -EINVAL;
			const struct ip_mreq *mr =
				(const struct ip_mreq *)optval;
			uint32_t group = net_ntohl(mr->imr_multiaddr.s_addr);
			uint32_t ifa = net_ntohl(mr->imr_interface.s_addr);
			net_device_t *want = ifa ? ip_dev_by_addr(ifa) : NULL;

			for (int i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
				if (s->mcast_groups[i] == group &&
				    (!want || s->mcast_devs[i] == want)) {
					net_device_t *dev = s->mcast_devs[i];

					s->mcast_groups[i] = 0;
					s->mcast_devs[i] = NULL;
					return dev ? igmp_leave(dev, group) : 0;
				}
			}
			return -EADDRNOTAVAIL;
		}
		default:
			return -ENOPROTOOPT;
		}
	}

	if (level == SOL_UDP) {
		if (s->type != SOCK_DGRAM)
			return -ENOPROTOOPT;
		return udp_setsockopt(s, optname, ival);
	}

	if (level == SOL_TCP || level == IPPROTO_TCP) {
		if (s->type != SOCK_STREAM)
			return -ENOPROTOOPT;
		/* Stored on the socket as well as applied to a connection
		 * that already exists, so a value set before connect() or
		 * listen() takes effect there -- see sock_apply_tcp_opts(). */
		switch (optname) {
		case TCP_NODELAY: {
			tcp_conn_t *c = sock_tcp_get(s);

			s->tcp_opt_nodelay = ival ? 1 : 0;
			s->tcp_opt_set |= SOCK_TCPOPT_NODELAY;
			if (c) {
				tcp_setsockopt_int(c, TCP_NODELAY, ival ? 1 : 0);
				tcp_conn_release(c);
			}
			return 0;
		}
		case TCP_CORK: {
			tcp_conn_t *c = sock_tcp_get(s);

			s->tcp_opt_cork = ival ? 1 : 0;
			s->tcp_opt_set |= SOCK_TCPOPT_CORK;
			if (c) {
				tcp_set_cork(c, ival);
				tcp_conn_release(c);
			}
			return 0;
		}
		case TCP_KEEPIDLE:
		case TCP_KEEPINTVL:
		case TCP_KEEPCNT: {
			/* Range as everywhere: 1 .. 32767 seconds, 1 .. 127
			 * probes; anything else is EINVAL, not silence. */
			if (optlen < sizeof(int) || ival < 1 ||
			    ival > (optname == TCP_KEEPCNT ? 127 : 32767))
				return -EINVAL;
			uint32_t t = (uint32_t)timer_s_to_ticks((uint64_t)ival);
			if (optname == TCP_KEEPIDLE) {
				s->tcp_opt_keepidle_ticks = t;
				s->tcp_opt_set |= SOCK_TCPOPT_KEEPIDLE;
			} else if (optname == TCP_KEEPINTVL) {
				s->tcp_opt_keepintvl_ticks = t;
				s->tcp_opt_set |= SOCK_TCPOPT_KEEPINTVL;
			} else {
				s->tcp_opt_keepcnt = (uint32_t)ival;
				s->tcp_opt_set |= SOCK_TCPOPT_KEEPCNT;
			}
			/* The connection's copy under its lock (a new idle
			 * time re-arms the keepalive timer there). */
			{
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					tcp_setsockopt_int(c, optname, ival);
					tcp_conn_release(c);
				}
			}
			return 0;
		}
		case TCP_MAXSEG:
		case TCP_QUICKACK:
		case TCP_USER_TIMEOUT:
		case TCP_NOTSENT_LOWAT:
		case TCP_DEFER_ACCEPT:
		case TCP_LINGER2:
		case TCP_SYNCNT:
		case TCP_WINDOW_CLAMP:
			if (optlen < sizeof(int))
				return -EINVAL;
			return sock_tcp_xopt_set(s, optname, ival);
		case TCP_CONGESTION: {
			/* The name, up to TCP_CA_NAME_MAX - 1 characters
			 * (it need not be terminated). */
			int id;

			if (!optval || optlen < 1)
				return -EINVAL;
			id = tcp_ca_find((const char *)optval,
					 optlen < TCP_CA_NAME_MAX - 1 ?
						 (size_t)optlen :
						 TCP_CA_NAME_MAX - 1);
			if (id < 0)
				return id;
			return sock_tcp_xopt_set(s, TCP_CONGESTION, id);
		}
		default:
			return -ENOPROTOOPT;
		}
	}

	return -ENOPROTOOPT;
}

int sock_getsockopt(int sockfd, int level, int optname, void *optval,
		    socklen_t *optlen)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	if (level == SOL_SOCKET) {
		switch (optname) {
		case SO_ERROR:
			if (*optlen >= sizeof(int)) {
				tcp_conn_t *c = s->type == SOCK_STREAM ?
							sock_tcp_get(s) :
							NULL;
				int e = __atomic_exchange_n(&s->error, 0,
							    __ATOMIC_ACQ_REL);

				/* The connection's hard error (a non-blocking
				 * connect's ECONNREFUSED, ETIMEDOUT, a reset),
				 * taken: reported once.  Without one, the soft
				 * error an ICMP message left (tcp_icmp_error),
				 * also taken. */
				if (e == 0 && c)
					e = tcp_sock_error(c);
				if (e == 0 && c)
					e = __atomic_exchange_n(&c->soft_error, 0,
								__ATOMIC_ACQ_REL);
				*(int *)optval = e;
				// If the non-blocking connect completed successfully, mark
				// the socket connected so sock_send() works.
				if (e == 0 && c &&
				    __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) ==
					    TCP_STATE_ESTABLISHED &&
				    !s->connected)
					s->connected = 1;
				if (c)
					tcp_conn_release(c);
				*optlen = sizeof(int);
			}
			return 0;
		case SO_TYPE:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->type;
				*optlen = sizeof(int);
			}
			return 0;
		case SO_ACCEPTCONN:
			/* Read-only: has listen() been applied. */
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->listening ? 1 : 0;
				*optlen = sizeof(int);
			}
			return 0;
		case SO_PROTOCOL:
		case SO_DOMAIN:
		case SO_RCVLOWAT:
		case SO_SNDLOWAT:
		case SO_TIMESTAMP:
		case SO_TIMESTAMPNS:
		case SO_MARK:
		case SO_PRIORITY: {
			int v = optname == SO_PROTOCOL ? s->protocol :
				optname == SO_DOMAIN   ? s->domain :
				optname == SO_RCVLOWAT ? s->rcvlowat :
				optname == SO_SNDLOWAT ? 1 :
				optname == SO_TIMESTAMP ? s->rcv_tstamp == 1 :
				optname == SO_TIMESTAMPNS ?
							 s->rcv_tstamp == 2 :
				optname == SO_MARK ? (int)s->sk_mark :
							 s->sk_priority;
			if (*optlen >= sizeof(int)) {
				*(int *)optval = v;
				*optlen = sizeof(int);
			}
			return 0;
		}
		case SO_PEEK_OFF:
			return -EOPNOTSUPP;
		case SO_MAX_PACING_RATE:
			if (*optlen >= sizeof(uint64_t)) {
				*(uint64_t *)optval = s->max_pacing_rate;
				*optlen = sizeof(uint64_t);
			} else if (*optlen >= sizeof(uint32_t)) {
				*(uint32_t *)optval =
					s->max_pacing_rate > ~0U ?
						~0U :
						(uint32_t)s->max_pacing_rate;
				*optlen = sizeof(uint32_t);
			}
			return 0;
		case SO_REUSEADDR:
		case SO_REUSEPORT:
		case SO_BROADCAST:
		case SO_OOBINLINE: {
			int v = (optname == SO_REUSEADDR) ? s->reuse_addr :
				(optname == SO_REUSEPORT) ? s->reuse_port :
				(optname == SO_BROADCAST) ? s->broadcast :
							    s->oobinline;
			if (*optlen >= sizeof(int)) {
				*(int *)optval = v;
				*optlen = sizeof(int);
			}
			return 0;
		}
		case SO_KEEPALIVE:
			if (*optlen >= sizeof(int)) {
				tcp_conn_t *c = s->type == SOCK_STREAM ?
							sock_tcp_get(s) :
							NULL;

				*(int *)optval = c ? (c->keepalive ? 1 : 0) :
						     s->tcp_opt_keepalive;
				if (c)
					tcp_conn_release(c);
				*optlen = sizeof(int);
			}
			return 0;
		case SO_RCVTIMEO:
		case SO_SNDTIMEO:
			if (*optlen >= 2 * sizeof(int64_t)) {
				uint64_t t = optname == SO_RCVTIMEO ?
						     s->rcv_timeout_ticks :
						     s->snd_timeout_ticks;
				uint64_t us = t * timer_us_per_tick();
				int64_t *tv = (int64_t *)optval;

				tv[0] = (int64_t)(us / 1000000ULL);
				tv[1] = (int64_t)(us % 1000000ULL);
				*optlen = 2 * sizeof(int64_t);
			}
			return 0;
		case SO_SNDBUF:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = (int)s->sndbuf_size;
				*optlen = sizeof(int);
			}
			return 0;
		case SO_RCVBUF:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = (int)s->rcvbuf_size;
				*optlen = sizeof(int);
			}
			return 0;
		case SO_LINGER:
			if (*optlen >= sizeof(struct linger)) {
				struct linger *l = (struct linger *)optval;
				l->l_onoff = s->linger_onoff;
				l->l_linger = s->linger_seconds;
				*optlen = sizeof(struct linger);
			}
			return 0;
		default:
			return -ENOPROTOOPT;
		}
	}

	if (level == SOL_IP || level == IPPROTO_IP) {
		switch (optname) {
		case IP_TTL:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->ip_ttl;
				*optlen = sizeof(int);
			}
			return 0;
		case IP_TOS:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->ip_tos;
				*optlen = sizeof(int);
			}
			return 0;
		case IP_HDRINCL:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->ip_hdrincl;
				*optlen = sizeof(int);
			}
			return 0;
		case IP_MULTICAST_TTL:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->mcast_ttl;
				*optlen = sizeof(int);
			}
			return 0;
		case IP_MULTICAST_LOOP:
			if (*optlen >= sizeof(int)) {
				*(int *)optval = s->mcast_loop;
				*optlen = sizeof(int);
			}
			return 0;
		case IP_MTU_DISCOVER:
		case IP_RECVERR:
		case IP_PKTINFO:
		case IP_RECVTTL:
		case IP_RECVTOS:
		case IP_MULTICAST_ALL:
			if (*optlen >= sizeof(int)) {
				*(int *)optval =
					optname == IP_MTU_DISCOVER ? s->pmtudisc :
					optname == IP_RECVERR	  ? s->ip_recverr :
					optname == IP_PKTINFO	  ? s->ip_recvpktinfo :
					optname == IP_RECVTTL	  ? s->ip_recvttl :
					optname == IP_RECVTOS	  ? s->ip_recvtos :
								    s->mc_all;
				*optlen = sizeof(int);
			}
			return 0;
		case IP_MULTICAST_IF:
			if (*optlen >= sizeof(uint32_t)) {
				*(uint32_t *)optval = net_htonl(s->mcast_if);
				*optlen = sizeof(uint32_t);
			}
			return 0;
		case IP_MTU: {
			/* The path MTU towards the connected peer. */
			struct ip_route rt;
			uint32_t mtu;

			if (!s->connected)
				return -ENOTCONN;
			if (ip_route_out(net_ntohl(s->remote_addr.sin_addr
							   .s_addr),
					 0, NULL, &rt) < 0)
				return -ENOTCONN;
			mtu = rt.mtu;
			ip_route_put(&rt);
			if (*optlen >= sizeof(int)) {
				*(int *)optval = (int)mtu;
				*optlen = sizeof(int);
			}
			return 0;
		}
		default:
			return -ENOPROTOOPT;
		}
	}

	if (level == SOL_UDP) {
		int v = 0, rc;

		if (s->type != SOCK_DGRAM)
			return -ENOPROTOOPT;
		rc = udp_getsockopt(s, optname, &v);
		if (rc == 0 && *optlen >= sizeof(int)) {
			*(int *)optval = v;
			*optlen = sizeof(int);
		}
		return rc;
	}

	if (level == SOL_TCP || level == IPPROTO_TCP) {
		if (s->type != SOCK_STREAM)
			return -ENOPROTOOPT;
		/* The connection is read with a reference held: a sibling
		 * thread's failed connect() may detach and release it
		 * (sock_connect_finish) at any moment. */
		switch (optname) {
		case TCP_NODELAY:
			if (*optlen >= sizeof(int)) {
				tcp_conn_t *c = sock_tcp_get(s);

				*(int *)optval =
					c ? (c->nodelay ? 1 : 0) :
					(s->tcp_opt_set & SOCK_TCPOPT_NODELAY) ?
						 s->tcp_opt_nodelay :
						 1; // the connection default
				if (c)
					tcp_conn_release(c);
				*optlen = sizeof(int);
			}
			return 0;
		case TCP_CORK:
			if (*optlen >= sizeof(int)) {
				tcp_conn_t *c = sock_tcp_get(s);

				*(int *)optval = c ? (c->cork ? 1 : 0) :
						     s->tcp_opt_cork;
				if (c)
					tcp_conn_release(c);
				*optlen = sizeof(int);
			}
			return 0;
		case TCP_INFO: {
			/* As much of the struct as the caller has room for:
			 * a program built against a shorter struct gets its
			 * prefix. */
			struct tcp_info ti;
			socklen_t n;
			tcp_conn_t *c = sock_tcp_get(s);

			if (!c)
				return -ENOTCONN;
			tcp_fill_info(c, &ti);
			tcp_conn_release(c);
			n = *optlen < (socklen_t)sizeof(ti) ? *optlen :
							      (socklen_t)sizeof(ti);
			if (n && optval)
				mm_memcpy(optval, &ti, (size_t)n);
			*optlen = n;
			return 0;
		}
		case TCP_CONGESTION: {
			char name[TCP_CA_NAME_MAX];
			const char *cur;
			socklen_t n;
			tcp_conn_t *c = sock_tcp_get(s);

			if (c) {
				cur = tcp_ca_name(tcp_ca_id(c));
				tcp_conn_release(c);
			} else {
				cur = tcp_ca_name(sock_tcp_xopt_get(
					s, TCP_CONGESTION, 0));
			}
			mm_memset(name, 0, sizeof(name));
			for (int i = 0; cur[i] && i < TCP_CA_NAME_MAX - 1; i++)
				name[i] = cur[i];
			n = *optlen < (socklen_t)sizeof(name) ? *optlen :
								(socklen_t)sizeof(name);
			if (n && optval)
				mm_memcpy(optval, name, (size_t)n);
			*optlen = n;
			return 0;
		}
		/* Before a connection exists: the value set on the socket,
		 * or the default a connection would get. */
		case TCP_KEEPIDLE:
		case TCP_KEEPINTVL:
		case TCP_KEEPCNT:
			if (*optlen >= sizeof(int)) {
				tcp_conn_t *c = sock_tcp_get(s);
				int v;

				if (optname == TCP_KEEPIDLE)
					v = c ? (int)timer_ticks_to_s(
							c->keepidle_ticks) :
					    (s->tcp_opt_set & SOCK_TCPOPT_KEEPIDLE) ?
						(int)timer_ticks_to_s(
							s->tcp_opt_keepidle_ticks) :
						7200;
				else if (optname == TCP_KEEPINTVL)
					v = c ? (int)timer_ticks_to_s(
							c->keepintvl_ticks) :
					    (s->tcp_opt_set & SOCK_TCPOPT_KEEPINTVL) ?
						(int)timer_ticks_to_s(
							s->tcp_opt_keepintvl_ticks) :
						75;
				else
					v = c ? (int)c->keepcnt :
					    (s->tcp_opt_set & SOCK_TCPOPT_KEEPCNT) ?
						(int)s->tcp_opt_keepcnt :
						9;
				if (c)
					tcp_conn_release(c);
				*(int *)optval = v;
				*optlen = sizeof(int);
			}
			return 0;
		case TCP_MAXSEG:
		case TCP_QUICKACK:
		case TCP_USER_TIMEOUT:
		case TCP_NOTSENT_LOWAT:
		case TCP_DEFER_ACCEPT:
		case TCP_LINGER2:
		case TCP_SYNCNT:
		case TCP_WINDOW_CLAMP:
			if (*optlen < sizeof(int))
				return -EINVAL;
			{
				int v = 0, rc = -ENOTCONN;
				tcp_conn_t *c = sock_tcp_get(s);

				if (c) {
					rc = tcp_getsockopt_int(c, optname, &v);
					tcp_conn_release(c);
				}
				if (rc < 0) {
					// no connection: the value it would get
					int def = optname == TCP_MAXSEG	 ? 536 :
						  optname == TCP_SYNCNT	 ? 6 :
						  optname == TCP_LINGER2 ? 60 :
						  optname == TCP_QUICKACK ? 1 :
									   0;
					v = sock_tcp_xopt_get(s, optname, def);
				}
				*(int *)optval = v;
				*optlen = sizeof(int);
			}
			return 0;
		default:
			return -ENOPROTOOPT;
		}
	}

	return -ENOPROTOOPT;
}

// ============================================================================
// sock_getpeername / sock_getsockname
// ============================================================================
int sock_getpeername(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;
	/* A non-blocking connect() never comes back to set `connected': the
	 * connection completes in the background and the program learns of
	 * it from poll().  It used to stay ENOTCONN here until SO_ERROR was
	 * read, which many programs never do.  The peer is known once the
	 * handshake got past SYN_SENT, as it is everywhere. */
	if (!s->connected && s->type == SOCK_STREAM && !s->listening) {
		tcp_conn_t *c = sock_tcp_get(s);

		if (c) {
			int st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);

			if (st != TCP_STATE_SYN_SENT && st != TCP_STATE_CLOSED &&
			    st != TCP_STATE_LISTEN && c->connect_done &&
			    !c->error)
				s->connected = 1;
			tcp_conn_release(c);
		}
	}
	if (!s->connected)
		return -ENOTCONN;

	if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
		*addr = s->remote_addr;
		*addrlen = sizeof(struct sockaddr_in);
	}
	return 0;
}

int sock_getsockname(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
		*addr = s->local_addr;
		*addrlen = sizeof(struct sockaddr_in);
	}
	return 0;
}

// ============================================================================
// sock_get - Get socket structure by index (for external use)
// ============================================================================
int sock_tryhold(int sockfd)
{
	int old;

	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return 0;
	/* ref_count reaches 0 exactly once per socket (sock_close, which then
	 * tears it down); taking a reference only while it is above 0 can
	 * never revive one being torn down, and needs no lock. */
	old = __atomic_load_n(&sockets[sockfd].ref_count, __ATOMIC_RELAXED);
	while (old > 0) {
		if (__atomic_compare_exchange_n(&sockets[sockfd].ref_count,
						&old, old + 1, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

net_socket_t *sock_get(int sockfd)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return NULL;
	net_socket_t *s = &sockets[sockfd];
	return s->active ? s : NULL;
}

// ============================================================================
// sock_socketpair - Create a pair of connected sockets
// Only AF_INET + SOCK_DGRAM supported (connected UDP pair on loopback)
// ============================================================================
int sock_socketpair(int domain, int type, int protocol, int sv[2])
{
	(void)protocol;
	if (domain != AF_INET)
		return -EAFNOSUPPORT;
	int real_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (real_type != SOCK_DGRAM && real_type != SOCK_STREAM)
		return -ESOCKTNOSUPPORT;

	int fd0 = sock_create(domain, real_type, 0);
	if (fd0 < 0)
		return fd0;
	int fd1 = sock_create(domain, real_type, 0);
	if (fd1 < 0) {
		sock_close(fd0);
		return fd1;
	}

	net_socket_t *s0 = &sockets[fd0];
	net_socket_t *s1 = &sockets[fd1];

	if (real_type == SOCK_DGRAM) {
		/* Two UDP sockets on 127.0.0.1, each connected to the other
		 * (each bind reserves its own port, so the two differ). */
		struct sockaddr_in a0, a1;
		int rc;

		mm_memset(&a0, 0, sizeof(a0));
		a0.sin_family = AF_INET;
		a0.sin_addr.s_addr = net_htonl(0x7F000001);
		a1 = a0;
		rc = udp_bind(s0, &a0);
		if (rc == 0)
			rc = udp_bind(s1, &a1);
		a0.sin_port = s0->local_addr.sin_port;
		a1.sin_port = s1->local_addr.sin_port;
		if (rc == 0)
			rc = udp_connect(s0, &a1);
		if (rc == 0)
			rc = udp_connect(s1, &a0);
		if (rc < 0) {
			sock_close(fd0);
			sock_close(fd1);
			return rc;
		}
		if (type & SOCK_NONBLOCK) {
			s0->nonblock = 1;
			s1->nonblock = 1;
		}
		sv[0] = fd0;
		sv[1] = fd1;
		return 0;
	}

	// Reserve each local port with alloc_ephemeral_port(), which publishes
	// sin_port+bound on the socket under the port lock before returning.
	// Reserving s0 before drawing s1's port guarantees the two differ —
	// otherwise a collision (both sockets sharing a local port) makes
	// sock_find_udp() match the lower-fd socket (the sender) for the peer's
	// datagram, so the peer's recv() never sees data and blocks forever.
	// Set family/addr first so the socket is fully described the instant it
	// becomes bound.
	s0->local_addr.sin_family = AF_INET;
	s0->local_addr.sin_addr.s_addr = net_htonl(0x7F000001);
	s0->bind_euid = current_euid();
	uint16_t port0 = alloc_ephemeral_port(s0, 0, 0);

	s1->local_addr.sin_family = AF_INET;
	s1->local_addr.sin_addr.s_addr = net_htonl(0x7F000001);
	s1->bind_euid = current_euid();
	uint16_t port1 = alloc_ephemeral_port(s1, 0, 0);

	// Cross-connect the pair now that both local ports are fixed.
	s0->remote_addr.sin_family = AF_INET;
	s0->remote_addr.sin_port = net_htons(port1);
	s0->remote_addr.sin_addr.s_addr = net_htonl(0x7F000001);
	s0->connected = 1;

	s1->remote_addr.sin_family = AF_INET;
	s1->remote_addr.sin_port = net_htons(port0);
	s1->remote_addr.sin_addr.s_addr = net_htonl(0x7F000001);
	s1->connected = 1;

	if (type & SOCK_NONBLOCK) {
		s0->nonblock = 1;
		s1->nonblock = 1;
	}

	sv[0] = fd0;
	sv[1] = fd1;
	return 0;
}

// ============================================================================
// sock_accept4 - Accept with flags (SOCK_NONBLOCK, SOCK_CLOEXEC)
// ============================================================================
int sock_accept4(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen,
		 int flags)
{
	if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
		return -EINVAL;
	int newfd = sock_accept(sockfd, addr, addrlen);
	if (newfd < 0)
		return newfd;

	if (flags & SOCK_NONBLOCK) {
		net_socket_t *ns = &sockets[newfd];
		ns->nonblock = 1;
	}
	/* SOCK_CLOEXEC is a property of the DESCRIPTOR, not of the socket, so
	 * it is applied by the syscall layer once the descriptor exists.  The
	 * note that used to stand here -- that it needed no enforcing because
	 * nothing execs -- has not been true for a long time. */
	return newfd;
}

// ============================================================================
// sock_sendmsg - Send message with scatter/gather
// ============================================================================
/* The most one sendmsg/recvmsg moves, as for read()/write(). */
#define SOCK_MSG_MAX_BYTES 0x7FFFF000UL

/* msg->msg_iov is the syscall layer's KERNEL copy of the iovec array
 * (msghdr_prepare()); the buffers it points to, msg_name and msg_control
 * are user memory, each range already validated.
 *
 * Both directions used to bounce through a 2048-byte stack buffer and
 * silently cut every message to that size: a 64 KB write() through
 * sendmsg() sent 2 KB and reported 2 KB, a UDP datagram larger than 2 KB
 * went out truncated, and recvmsg() never returned more than 2 KB of
 * anything. */
int sock_sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	if (!msg)
		return -EINVAL;
	net_socket_t *s = sock_get(sockfd);
	if (!s)
		return -EBADF;

	size_t total = 0;
	for (int i = 0; i < msg->msg_iovlen; i++) {
		size_t l = msg->msg_iov[i].iov_len;

		if (l > SOCK_MSG_MAX_BYTES - total)
			return -EINVAL;
		total += l;
	}

	const struct sockaddr_in *dest = NULL;
	struct sockaddr_in dest_copy;
	if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
		if (copy_from_user(&dest_copy, msg->msg_name,
				   sizeof(dest_copy)))
			return -EFAULT;
		dest = &dest_copy;
	}

	if (s->type == SOCK_STREAM) {
		/* A byte stream: each iovec goes to sock_send() in turn,
		 * straight from user memory.  A short send ends the walk --
		 * the window filled on a non-blocking socket, or a signal --
		 * and reports what went out, as a single send() would. */
		size_t sent = 0;
		int last = -1;

		for (int i = 0; i < msg->msg_iovlen; i++)
			if (msg->msg_iov[i].iov_len)
				last = i;
		for (int i = 0; i < msg->msg_iovlen; i++) {
			size_t l = msg->msg_iov[i].iov_len;
			int f = flags;

			if (l == 0)
				continue;
			/* One message: TCP may join the iovecs into full
			 * segments (more follows), and only a send that
			 * moved nothing raises SIGPIPE. */
			if (i < last)
				f |= MSG_MORE;
			if (sent)
				f |= MSG_NOSIGNAL;
			int r = sock_send(sockfd, msg->msg_iov[i].iov_base, l,
					  f);
			if (r < 0)
				return sent ? (int)sent : r;
			sent += (size_t)r;
			if ((size_t)r < l)
				break;
		}
		return (int)sent;
	}

	/* A datagram is ONE unit: gather the iovecs into one buffer and
	 * send it whole.  Its size is checked by the protocol (UDP: 65507
	 * bytes of payload). */
	if (total > 0xFFFFU)
		return -EMSGSIZE;
	uint8_t *kbuf = (uint8_t *)kalloc(total ? total : 1);
	if (!kbuf)
		return -ENOMEM;
	size_t off = 0;
	for (int i = 0; i < msg->msg_iovlen; i++) {
		size_t l = msg->msg_iov[i].iov_len;

		if (l && copy_from_user(kbuf + off, msg->msg_iov[i].iov_base,
					l)) {
			kfree(kbuf);
			return -EFAULT;
		}
		off += l;
	}
	int ret = sock_sendto(sockfd, kbuf, total, flags, dest,
			      dest ? sizeof(struct sockaddr_in) : 0);
	kfree(kbuf);
	return ret;
}

// ============================================================================
// sock_recvmsg - Receive message with scatter/gather
// ============================================================================
/* Copy `len' bytes of a kernel buffer out across the iovecs, in order. */
static int sock_scatter_to_iov(const struct msghdr *msg, const uint8_t *src,
			       size_t len)
{
	size_t off = 0;

	for (int i = 0; i < msg->msg_iovlen && off < len; i++) {
		size_t chunk = msg->msg_iov[i].iov_len;

		if (chunk > len - off)
			chunk = len - off;
		if (chunk && copy_to_user(msg->msg_iov[i].iov_base, src + off,
					  chunk))
			return -EFAULT;
		off += chunk;
	}
	return 0;
}

int sock_recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	if (!msg)
		return -EINVAL;
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	size_t total = 0;
	for (int i = 0; i < msg->msg_iovlen; i++) {
		size_t l = msg->msg_iov[i].iov_len;

		if (l > SOCK_MSG_MAX_BYTES - total)
			return -EINVAL;
		total += l;
	}
	msg->msg_flags = 0;

	if (s->type == SOCK_STREAM) {
		/* A stream has no sender address and no ancillary data here. */
		msg->msg_namelen = 0;
		msg->msg_controllen = 0;
		if (total == 0)
			return 0;
		/* The urgent byte: one byte into the first buffer. */
		if (flags & MSG_OOB) {
			for (int i = 0; i < msg->msg_iovlen; i++) {
				if (!msg->msg_iov[i].iov_len)
					continue;
				int r = sock_recv(sockfd, msg->msg_iov[i].iov_base,
						  msg->msg_iov[i].iov_len, flags);
				if (r >= 0)
					msg->msg_flags |= MSG_OOB;
				return r;
			}
		}

		/* MSG_PEEK cannot be split across calls -- every call would
		 * peek at the same first bytes -- so a peek into several
		 * iovecs goes through one kernel buffer, as large as the
		 * receive ring can be. */
		if ((flags & MSG_PEEK) && msg->msg_iovlen > 1) {
			size_t cap = total > TCP_RX_BUF_MAX ? TCP_RX_BUF_MAX :
							      total;
			uint8_t *kbuf = (uint8_t *)kalloc(cap);
			if (!kbuf)
				return -ENOMEM;
			int r = sock_recv(sockfd, kbuf, cap, flags);
			if (r > 0 && sock_scatter_to_iov(msg, kbuf, (size_t)r))
				r = -EFAULT;
			kfree(kbuf);
			return r;
		}

		/* Otherwise each iovec is filled straight from the ring.  The
		 * first may block as the caller asked; the rest only take
		 * what is already there (all of it with MSG_WAITALL), as one
		 * read() into one large buffer would. */
		size_t got = 0;
		for (int i = 0; i < msg->msg_iovlen; i++) {
			size_t l = msg->msg_iov[i].iov_len;

			if (l == 0)
				continue;
			int f = flags;
			if (got && !(flags & MSG_WAITALL))
				f |= MSG_DONTWAIT;
			int r = sock_recv(sockfd, msg->msg_iov[i].iov_base, l,
					  f);
			if (r < 0)
				return got ? (int)got : r;
			got += (size_t)r;
			if ((size_t)r < l)
				break;
		}
		return (int)got;
	}

	/* Datagram: one datagram, scattered across the iovecs.  The part
	 * that does not fit is discarded and MSG_TRUNC set in msg_flags;
	 * with MSG_TRUNC in `flags' the real length is returned. */
	struct sockaddr_in src_addr;
	socklen_t addrlen = sizeof(src_addr);
	struct sock_dgram_meta meta;
	size_t cap = total > 0xFFFFU ? 0xFFFFU : total;
	uint8_t *kbuf = (uint8_t *)kalloc(cap ? cap : 1);
	if (!kbuf)
		return -ENOMEM;
	mm_memset(&src_addr, 0, sizeof(src_addr));
	src_addr.sin_family = AF_INET;
	mm_memset(&meta, 0, sizeof(meta));

	int ret = sock_recvfrom_ex(sockfd, kbuf, cap, flags, &src_addr,
				   &addrlen, &meta);
	if (ret < 0) {
		kfree(kbuf);
		return ret;
	}
	if (sock_scatter_to_iov(msg, kbuf, (size_t)ret)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	if (meta.real_len > (uint32_t)ret) {
		msg->msg_flags |= MSG_TRUNC;
		if (flags & MSG_TRUNC)
			ret = (int)meta.real_len;
	}
	if (meta.errqueue)
		msg->msg_flags |= MSG_ERRQUEUE;
	uint32_t pkt_dst_ip = meta.dst_ip;
	uint8_t pkt_ttl = meta.ttl, pkt_tos = meta.tos;
	uint32_t pkt_ifindex = meta.ifindex;

	// Return source address if requested
	if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
		if (copy_to_user(msg->msg_name, &src_addr, sizeof(src_addr)))
			return -EFAULT;
		msg->msg_namelen = sizeof(struct sockaddr_in);
	}

	smap_disable();
	// RFC 2292 ancillary data
	size_t cmsg_avail = msg->msg_controllen;
	size_t cmsg_used = 0;
	if (msg->msg_control && cmsg_avail > 0 && meta.errqueue) {
		/* The extended error, then who reported it. */
		unsigned char *cp = (unsigned char *)msg->msg_control;
		size_t dlen = sizeof(struct sock_extended_err) +
			      sizeof(struct sockaddr_in);

		if (CMSG_SPACE(dlen) <= cmsg_avail) {
			struct cmsghdr *c = (struct cmsghdr *)cp;
			struct sockaddr_in off;

			c->cmsg_len = CMSG_LEN(dlen);
			c->cmsg_level = IPPROTO_IP;
			c->cmsg_type = IP_RECVERR;
			mm_memcpy(CMSG_DATA(c), &meta.ee, sizeof(meta.ee));
			mm_memset(&off, 0, sizeof(off));
			off.sin_family = meta.offender ? AF_INET : 0;
			off.sin_addr.s_addr = net_htonl(meta.offender);
			mm_memcpy(CMSG_DATA(c) + sizeof(meta.ee), &off,
				  sizeof(off));
			cmsg_used += CMSG_SPACE(dlen);
		} else {
			msg->msg_flags |= MSG_CTRUNC;
		}
	} else if (msg->msg_control && cmsg_avail > 0) {
		unsigned char *cp = (unsigned char *)msg->msg_control;
		/* SO_TIMESTAMP / SO_TIMESTAMPNS: when the datagram arrived,
		 * on the wall clock, first as everywhere. */
		if (s->rcv_tstamp) {
			uint64_t mono = meta.tstamp_ns ? meta.tstamp_ns :
							 hrtimer_now_ns();
			uint64_t rt = mono + timer_realtime_offset_ns(mono);
			int64_t ts[2];
			size_t dlen = sizeof(ts);

			ts[0] = (int64_t)(rt / 1000000000ULL);
			ts[1] = s->rcv_tstamp == 2 ?
					(int64_t)(rt % 1000000000ULL) :
					(int64_t)(rt % 1000000000ULL / 1000ULL);
			if (cmsg_used + CMSG_SPACE(dlen) <= cmsg_avail) {
				struct cmsghdr *c =
					(struct cmsghdr *)(cp + cmsg_used);

				c->cmsg_len = CMSG_LEN(dlen);
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = s->rcv_tstamp == 2 ?
						       SCM_TIMESTAMPNS :
						       SCM_TIMESTAMP;
				mm_memcpy(CMSG_DATA(c), ts, dlen);
				cmsg_used += CMSG_SPACE(dlen);
			} else {
				msg->msg_flags |= MSG_CTRUNC;
			}
		}
		if (s->ip_recvpktinfo &&
		    cmsg_used + CMSG_SPACE(sizeof(struct in_pktinfo)) <=
			    cmsg_avail) {
			struct cmsghdr *c = (struct cmsghdr *)(cp + cmsg_used);
			c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
			c->cmsg_level = IPPROTO_IP;
			c->cmsg_type = IP_PKTINFO;
			struct in_pktinfo pi = { 0 };
			pi.ipi_ifindex = (int)pkt_ifindex;
			pi.ipi_addr.s_addr = net_htonl(pkt_dst_ip);
			pi.ipi_spec_dst.s_addr = net_htonl(pkt_dst_ip);
			for (size_t k = 0; k < sizeof(pi); k++)
				CMSG_DATA(c)[k] = ((unsigned char *)&pi)[k];
			cmsg_used += CMSG_SPACE(sizeof(struct in_pktinfo));
		}
		if (s->ip_recvttl &&
		    cmsg_used + CMSG_SPACE(sizeof(int)) <= cmsg_avail) {
			struct cmsghdr *c = (struct cmsghdr *)(cp + cmsg_used);
			c->cmsg_len = CMSG_LEN(sizeof(int));
			c->cmsg_level = IPPROTO_IP;
			c->cmsg_type = IP_RECVTTL;
			int v = pkt_ttl;
			for (size_t k = 0; k < sizeof(v); k++)
				CMSG_DATA(c)[k] = ((unsigned char *)&v)[k];
			cmsg_used += CMSG_SPACE(sizeof(int));
		}
		if (s->ip_recvtos &&
		    cmsg_used + CMSG_SPACE(sizeof(int)) <= cmsg_avail) {
			struct cmsghdr *c = (struct cmsghdr *)(cp + cmsg_used);
			c->cmsg_len = CMSG_LEN(sizeof(int));
			c->cmsg_level = IPPROTO_IP;
			c->cmsg_type = IP_RECVTOS;
			int v = pkt_tos;
			for (size_t k = 0; k < sizeof(v); k++)
				CMSG_DATA(c)[k] = ((unsigned char *)&v)[k];
			cmsg_used += CMSG_SPACE(sizeof(int));
		}
	}
	msg->msg_controllen = cmsg_used;
	smap_enable();

	return ret;
}

// ============================================================================
// sock_poll - the readiness of a socket, as poll()/select()/epoll report it
// ============================================================================
/* No more data will ever arrive: the peer's FIN came in (CLOSE_WAIT and
 * every state after it) or the connection is gone. */
static int sock_tcp_rcv_done(int st)
{
	return st == TCP_STATE_CLOSE_WAIT || st == TCP_STATE_CLOSING ||
	       st == TCP_STATE_LAST_ACK || st == TCP_STATE_TIME_WAIT ||
	       st == TCP_STATE_CLOSED;
}

/* Our sending side is finished: our FIN went out, or the connection is
 * gone. */
static int sock_tcp_snd_done(int st)
{
	return st == TCP_STATE_FIN_WAIT_1 || st == TCP_STATE_FIN_WAIT_2 ||
	       st == TCP_STATE_CLOSING || st == TCP_STATE_LAST_ACK ||
	       st == TCP_STATE_TIME_WAIT || st == TCP_STATE_CLOSED;
}

/* A stream socket:
 *   listening       POLLIN while a connection waits to be accepted;
 *   not connected   POLLOUT | POLLHUP (nothing to wait for);
 *   connecting      nothing until the handshake ends;
 *   connected       POLLIN with SO_RCVLOWAT bytes queued, POLLOUT with
 *                   room to send, POLLPRI for an unread urgent byte;
 *   either side shut or the peer's FIN in: POLLIN | POLLRDHUP (a read
 *                   returns end of file); our send side shut: POLLOUT (a
 *                   write fails at once with EPIPE and must be woken);
 *   both sides finished, or the connection gone: POLLHUP;
 *   a pending error: POLLERR. */
static int sock_poll_stream(net_socket_t *s)
{
	int mask = 0;
	tcp_conn_t *c = sock_tcp_get(s);
	int rx_shut = __atomic_load_n(&s->rx_shutdown, __ATOMIC_ACQUIRE);
	int tx_shut = __atomic_load_n(&s->tx_shutdown, __ATOMIC_ACQUIRE);

	if (c && __atomic_load_n(&s->listening, __ATOMIC_ACQUIRE)) {
		if (__atomic_load_n(&c->accept_ready, __ATOMIC_ACQUIRE))
			mask |= POLLIN | POLLRDNORM;
		tcp_conn_release(c);
		return mask;
	}
	if (!c) {
		/* Never connected (or the connection was taken away): the
		 * state everywhere else calls CLOSE. */
		mask = POLLOUT | POLLWRNORM | POLLHUP;
		if (rx_shut)
			mask |= POLLIN | POLLRDNORM | POLLRDHUP;
		if (s->error)
			mask |= POLLERR;
		return mask;
	}

	int st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
	int rcv_done = rx_shut || sock_tcp_rcv_done(st);
	int snd_done = tx_shut || sock_tcp_snd_done(st);

	if ((rcv_done && snd_done) || st == TCP_STATE_CLOSED)
		mask |= POLLHUP;
	if (rcv_done)
		mask |= POLLIN | POLLRDNORM | POLLRDHUP;
	if (st != TCP_STATE_SYN_SENT && st != TCP_STATE_SYN_RECEIVED) {
		int lowat = s->rcvlowat > 1 ? s->rcvlowat : 1;
		uint32_t inq = sock_tcp_inq(c);

		/* At the urgent mark an out-of-band byte is not data: one
		 * byte more is needed to read anything. */
		if (!c->urg_inline && sock_tcp_atmark(c))
			lowat++;
		if (lowat == 1 ? (inq > 0 || c->rx_ready) :
				 inq >= (uint32_t)lowat)
			mask |= POLLIN | POLLRDNORM;
		if (snd_done ||
		    __atomic_load_n(&c->tx_ready, __ATOMIC_ACQUIRE))
			mask |= POLLOUT | POLLWRNORM;
		if (sock_tcp_urg_pending(c))
			mask |= POLLPRI;
	}
	if (__atomic_load_n(&c->error, __ATOMIC_ACQUIRE) || s->error)
		mask |= POLLERR;
	tcp_conn_release(c);
	return mask;
}

/* A datagram socket (UDP, raw): readable with a datagram queued, always
 * writable; an error, or one waiting on the error queue, is POLLERR. */
static int sock_poll_dgram(net_socket_t *s)
{
	int mask = POLLOUT | POLLWRNORM | POLLWRBAND;
	int rx_shut = __atomic_load_n(&s->rx_shutdown, __ATOMIC_ACQUIRE);
	int tx_shut = __atomic_load_n(&s->tx_shutdown, __ATOMIC_ACQUIRE);

	if (!skb_queue_empty(&s->rx_queue))
		mask |= POLLIN | POLLRDNORM;
	if (rx_shut)
		mask |= POLLIN | POLLRDNORM | POLLRDHUP;
	if (rx_shut && tx_shut)
		mask |= POLLHUP;
	if (s->error || !skb_queue_empty(&s->err_queue))
		mask |= POLLERR;
	return mask;
}

int sock_poll(int sockfd, short events)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return POLLNVAL;
	net_socket_t *s = &sockets[sockfd];
	if (!__atomic_load_n(&s->active, __ATOMIC_ACQUIRE))
		return POLLNVAL;

	int mask = s->type == SOCK_STREAM ? sock_poll_stream(s) :
					    sock_poll_dgram(s);

	/* What was asked, and what is always reported. */
	return mask & ((unsigned short)events | POLLERR | POLLHUP);
}

// ============================================================================
// sock_ioctl_net - Handle network ioctls on a socket fd
// ============================================================================

/* Socket-level ioctl codes (same values as sys/ioctl.h in userland) */
#define SOCK_FIONBIO 0x5421
#define SOCK_FIONREAD 0x541B

/* Interface, ARP and route ioctls that modify kernel network state.  These are
 * privileged: only a process with an effective uid of 0 may issue them.  All
 * read-only ioctls (SIOCGIF*, SIOCGARP, table dumps) are omitted and stay
 * available to every user. */
static int siocmd_is_privileged(unsigned long cmd)
{
	switch (cmd) {
	case SIOCSIFFLAGS:
	case SIOCSIFADDR:
	case SIOCSIFNETMASK:
	case SIOCSIFBRDADDR:
	case SIOCSIFDSTADDR:
	case SIOCSIFMTU:
	case SIOCSIFMETRIC:
	case SIOCSIFHWADDR:
	case SIOCSIFMEM:
	case SIOCSIFNAME:
	case SIOCSIFENCAP:
	case SIOCSIFSLAVE:
	case SIOCSIFPFLAGS:
	case SIOCSIFHWBROADCAST:
	case SIOCSIFLINK:
	case SIOCSIFVLAN:
	case SIOCSARP:
	case SIOCDARP:
	case SIOCADDRT:
	case SIOCDELRT:
		return 1;
	default:
		return 0;
	}
}

int sock_ioctl_net(int sockfd, unsigned long request, void *argp)
{
	/* Reject privileged interface/ARP/route changes from non-root callers.
	 * Reads pass through unchanged. */
	if (siocmd_is_privileged(request) && !capable())
		return -EPERM;

	/* Handle socket-level ioctls that operate on the socket state itself */
	if (request == SOCK_FIONBIO) {
		if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
			return -EBADF;
		net_socket_t *s = &sockets[sockfd];
		if (!s->active)
			return -EBADF;
		if (!argp)
			return -EFAULT;
		s->nonblock = (*((int *)argp) != 0) ? 1 : 0;
		return 0;
	}
	/* The queue ioctls.  SIOCINQ is FIONREAD: a stream's unread bytes, a
	 * datagram socket's first datagram (what one read returns).  SIOCOUTQ:
	 * bytes sent and not yet acknowledged; SIOCOUTQNSD: not yet sent;
	 * SIOCATMARK: the next byte read is the urgent one.  The TCP queue
	 * questions are EINVAL on a listener and 0 while connecting. */
	if (request == SIOCINQ || request == SIOCOUTQ ||
	    request == SIOCOUTQNSD || request == SIOCATMARK) {
		if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
			return -EBADF;
		net_socket_t *s = &sockets[sockfd];
		if (!s->active)
			return -EBADF;
		if (!argp)
			return -EFAULT;
		int answ = 0;
		if (s->type == SOCK_STREAM) {
			tcp_conn_t *c = sock_tcp_get(s);
			int st = c ? c->state : TCP_STATE_CLOSED;

			if (s->listening && request != SIOCATMARK) {
				if (c)
					tcp_conn_release(c);
				return -EINVAL;
			}
			if (c && st != TCP_STATE_SYN_SENT &&
			    st != TCP_STATE_SYN_RECEIVED) {
				uint32_t v = 0;

				if (request == SIOCINQ)
					v = sock_tcp_inq(c);
				else if (request == SIOCOUTQ)
					v = sock_tcp_outq(c);
				else if (request == SIOCOUTQNSD)
					v = sock_tcp_outq_nsd(c);
				else
					v = (uint32_t)sock_tcp_atmark(c);
				answ = v > 0x7fffffffu ? 0x7fffffff : (int)v;
			}
			if (c)
				tcp_conn_release(c);
		} else if (request == SIOCINQ) {
			answ = (int)dgram_first_len(s);
		}
		*((int *)argp) = answ;
		return 0;
	}
	/* Delegate interface-level ioctls */
	return net_ioctl(request, argp);
}

// ============================================================================
// net_find_dev_by_name - Find a device by interface name (includes loopback)
// ============================================================================
static net_device_t *net_find_dev_by_name(const char *name)
{
	// Check loopback first
	net_device_t *lo = net_get_loopback();
	if (lo) {
		const char *n = lo->name;
		int match = 1;
		for (int j = 0; j < IFNAMSIZ; j++) {
			if (name[j] != n[j]) {
				match = 0;
				break;
			}
			if (!n[j])
				break;
		}
		if (match)
			return lo;
	}
	// Check registered devices
	for (int i = 0; i < net_device_count(); i++) {
		net_device_t *d = net_get_device(i);
		if (!d)
			continue;
		const char *n = d->name;
		int match = 1;
		for (int j = 0; j < IFNAMSIZ; j++) {
			if (name[j] != n[j]) {
				match = 0;
				break;
			}
			if (!n[j])
				break;
		}
		if (match)
			return d;
	}
	return NULL;
}

// ============================================================================
// net_dev_is_loopback - Check if device is the loopback interface
// ============================================================================
static int net_dev_is_loopback(net_device_t *dev)
{
	return dev == net_get_loopback();
}

/* The interface flags user space sees: the kind of interface, and IFF_UP
 * and IFF_RUNNING while it is administratively up (SIOCSIFFLAGS). */
static uint32_t sock_if_flags(net_device_t *dev)
{
	uint32_t f = net_dev_is_loopback(dev) ? IFF_LOOPBACK :
						IFF_BROADCAST | IFF_MULTICAST;

	if (!dev->core_ready ||
	    (__atomic_load_n(&dev->flags, __ATOMIC_ACQUIRE) & IFF_UP))
		f |= IFF_UP | IFF_RUNNING;
	return f;
}

// ============================================================================
// net_ioctl - Handle interface-level network ioctls
// ============================================================================
int net_ioctl(unsigned long request, void *argp)
{
	if (!argp)
		return -EFAULT;

	switch (request) {
	case SIOCGIFCONF: {
		struct ifconf *ifc = (struct ifconf *)argp;
		int count = net_device_count();
		int total = count + (net_get_loopback() ? 1 : 0);
		int needed = total * (int)sizeof(struct ifreq);
		if (ifc->ifc_len < needed || !ifc->ifc_buf) {
			ifc->ifc_len = needed;
			return 0;
		}
		int idx = 0;
		// Add loopback first
		net_device_t *lo = net_get_loopback();
		if (lo) {
			struct ifreq *ifr = &ifc->ifc_req[idx];
			for (int j = 0; j < IFNAMSIZ; j++)
				ifr->ifr_name[j] = 0;
			const char *n = lo->name;
			for (int j = 0; n[j] && j < IFNAMSIZ - 1; j++)
				ifr->ifr_name[j] = n[j];
			struct sockaddr_in *sin =
				(struct sockaddr_in *)&ifr->ifr_addr;
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = net_htonl(lo->ip_addr);
			sin->sin_port = 0;
			idx++;
		}
		for (int i = 0; i < count; i++) {
			net_device_t *dev = net_get_device(i);
			if (!dev)
				continue;
			struct ifreq *ifr = &ifc->ifc_req[idx];
			for (int j = 0; j < IFNAMSIZ; j++)
				ifr->ifr_name[j] = 0;
			const char *n = dev->name;
			for (int j = 0; n[j] && j < IFNAMSIZ - 1; j++)
				ifr->ifr_name[j] = n[j];
			struct sockaddr_in *sin =
				(struct sockaddr_in *)&ifr->ifr_addr;
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = net_htonl(dev->ip_addr);
			sin->sin_port = 0;
			idx++;
		}
		ifc->ifc_len = idx * (int)sizeof(struct ifreq);
		return 0;
	}

	case SIOCGIFFLAGS: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		ifr->ifr_flags = (short)sock_if_flags(dev);
		return 0;
	}

	case SIOCSIFFLAGS: {
		/* IFF_UP takes the interface up or down; the other flags are
		 * accepted and not changed. */
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		(void)dev_change_flags(dev, (uint16_t)ifr->ifr_flags & IFF_UP);
		return 0;
	}

	case SIOCGIFADDR: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = net_htonl(dev->ip_addr);
		sin->sin_port = 0;
		return 0;
	}

	case SIOCSIFADDR: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
		uint32_t addr = net_ntohl(sin->sin_addr.s_addr);
		if (addr != dev->ip_addr) {
			dev->ip_addr = addr;
			// neighbours and routes of the old address are stale
			call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
		}
		return 0;
	}

	case SIOCGIFNETMASK: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		struct sockaddr_in *sin =
			(struct sockaddr_in *)&ifr->ifr_netmask;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = net_htonl(dev->netmask);
		sin->sin_port = 0;
		return 0;
	}

	case SIOCSIFNETMASK: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		struct sockaddr_in *sin =
			(struct sockaddr_in *)&ifr->ifr_netmask;
		uint32_t mask = net_ntohl(sin->sin_addr.s_addr);
		if (mask != dev->netmask) {
			dev->netmask = mask;
			call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
		}
		return 0;
	}

	case SIOCGIFBRDADDR: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		struct sockaddr_in *sin =
			(struct sockaddr_in *)&ifr->ifr_broadaddr;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = net_htonl(dev->ip_addr | ~dev->netmask);
		sin->sin_port = 0;
		return 0;
	}

	case SIOCSIFBRDADDR:
		return 0; // Accept but ignore

	case SIOCGIFMTU: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		ifr->ifr_mtu = dev->mtu;
		return 0;
	}

	case SIOCSIFMTU: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		if (ifr->ifr_mtu < 68 || ifr->ifr_mtu > 9000)
			return -EINVAL;
		if (dev->mtu != (uint16_t)ifr->ifr_mtu) {
			dev->mtu = (uint16_t)ifr->ifr_mtu;
			// every route's MTU is looked up again
			call_netdevice_notifiers(NETDEV_CHANGEMTU, dev);
		}
		return 0;
	}

	case SIOCGIFHWADDR: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		ifr->ifr_hwaddr.sa_family = 1; // ARPHRD_ETHER
		for (int j = 0; j < ETH_ALEN; j++)
			ifr->ifr_hwaddr.sa_data[j] = (char)dev->mac_addr[j];
		return 0;
	}

	case SIOCGIFINDEX: {
		struct ifreq *ifr = (struct ifreq *)argp;
		net_device_t *dev = net_find_dev_by_name(ifr->ifr_name);
		if (!dev)
			return -ENOENT;
		if (net_dev_is_loopback(dev)) {
			ifr->ifr_ifindex = 1; // lo is always index 1
		} else {
			for (int i = 0; i < net_device_count(); i++) {
				if (net_get_device(i) == dev) {
					ifr->ifr_ifindex = i + 2;
					break;
				}
			}
		}
		return 0;
	}

	case SIOCGIFNAME: {
		struct ifreq *ifr = (struct ifreq *)argp;
		int idx = ifr->ifr_ifindex - 1;
		net_device_t *dev = net_get_device(idx);
		if (!dev)
			return -ENOENT;
		for (int j = 0; j < IFNAMSIZ; j++)
			ifr->ifr_name[j] = 0;
		const char *n = dev->name;
		for (int j = 0; n[j] && j < IFNAMSIZ - 1; j++)
			ifr->ifr_name[j] = n[j];
		return 0;
	}

	case SIOCGIFMETRIC: {
		struct ifreq *ifr = (struct ifreq *)argp;
		ifr->ifr_metric = 0;
		return 0;
	}

	case SIOCSIFMETRIC:
	case SIOCSIFHWADDR:
	case SIOCGIFDSTADDR:
	case SIOCSIFDSTADDR:
	case SIOCGIFMEM:
	case SIOCSIFMEM:
	case SIOCSIFNAME:
	case SIOCGIFENCAP:
	case SIOCSIFENCAP:
	case SIOCGIFSLAVE:
	case SIOCSIFSLAVE:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
	case SIOCSIFPFLAGS:
	case SIOCGIFPFLAGS:
	case SIOCDIFADDR:
	case SIOCSIFHWBROADCAST:
	case SIOCSIFLINK:
		return -EOPNOTSUPP;

	case SIOCGIFCOUNT: {
		int *countp = (int *)argp;
		*countp = net_device_count() + (net_get_loopback() ? 1 : 0);
		return 0;
	}

	// ARP ioctls
	case SIOCGARP: {
		struct arpreq *req = (struct arpreq *)argp;
		struct sockaddr_in *sin = (struct sockaddr_in *)&req->arp_pa;
		uint32_t ip = net_ntohl(sin->sin_addr.s_addr);
		uint8_t mac[ETH_ALEN];
		net_device_t *dev = net_get_default_device();
		if (!dev)
			return -ENETDOWN;
		if (arp_resolve(dev, ip, mac) == 0) {
			for (int j = 0; j < ETH_ALEN; j++)
				req->arp_ha.sa_data[j] = (char)mac[j];
			req->arp_ha.sa_family = 1; // ARPHRD_ETHER
			req->arp_flags = ATF_COM;
			return 0;
		}
		return -ENOENT;
	}

	case SIOCSARP: {
		struct arpreq *req = (struct arpreq *)argp;
		struct sockaddr_in *sin = (struct sockaddr_in *)&req->arp_pa;
		uint32_t ip = net_ntohl(sin->sin_addr.s_addr);
		uint8_t mac[ETH_ALEN];
		for (int j = 0; j < ETH_ALEN; j++)
			mac[j] = (uint8_t)req->arp_ha.sa_data[j];
		// ATF_PERM: a permanent entry; otherwise one that ages (STALE)
		if (arp_set_entry(ip, mac, (req->arp_flags & ATF_PERM) != 0) < 0)
			return -ENETUNREACH;
		return 0;
	}

	case SIOCDARP: {
		struct arpreq *req = (struct arpreq *)argp;
		struct sockaddr_in *sin = (struct sockaddr_in *)&req->arp_pa;

		arp_del_entry(net_ntohl(sin->sin_addr.s_addr));
		return 0;
	}

	// Routing ioctls
	case SIOCADDRT: {
		if (!argp)
			return -EFAULT;
		smap_disable();
		struct rtentry *rt = (struct rtentry *)argp;
		struct sockaddr_in *dst = (struct sockaddr_in *)&rt->rt_dst;
		struct sockaddr_in *gw = (struct sockaddr_in *)&rt->rt_gateway;
		struct sockaddr_in *mask =
			(struct sockaddr_in *)&rt->rt_genmask;
		uint32_t dst_ip = net_ntohl(dst->sin_addr.s_addr);
		uint32_t gw_ip = net_ntohl(gw->sin_addr.s_addr);
		uint32_t netmask = net_ntohl(mask->sin_addr.s_addr);
		uint16_t flags = (uint16_t)rt->rt_flags;
		smap_enable();

		net_device_t *rdev = net_get_default_device();
		return route_add(dst_ip, netmask, gw_ip, rdev, 0, flags);
	}
	case SIOCDELRT: {
		if (!argp)
			return -EFAULT;
		smap_disable();
		struct rtentry *rt = (struct rtentry *)argp;
		struct sockaddr_in *dst = (struct sockaddr_in *)&rt->rt_dst;
		struct sockaddr_in *gw = (struct sockaddr_in *)&rt->rt_gateway;
		struct sockaddr_in *mask =
			(struct sockaddr_in *)&rt->rt_genmask;
		uint32_t dst_ip = net_ntohl(dst->sin_addr.s_addr);
		uint32_t gw_ip = net_ntohl(gw->sin_addr.s_addr);
		uint32_t netmask = net_ntohl(mask->sin_addr.s_addr);
		(void)netmask;
		smap_enable();
		return route_del(dst_ip, netmask, gw_ip);
	}
	case SIOCRTMSG:
		return -EOPNOTSUPP;

	case SIOCGSTAMP:
	case SIOCGSTAMPNS:
		return -EOPNOTSUPP;

	// Bridge / VLAN - not supported
	case SIOCBRADDBR:
	case SIOCBRDELBR:
	case SIOCBRADDIF:
	case SIOCBRDELIF:
	case SIOCGIFVLAN:
	case SIOCSIFVLAN:
		return -EOPNOTSUPP;

	default:
		return -EINVAL;
	}
}

// ============================================================================
// sock_fcntl_net - Handle fcntl on a socket
// ============================================================================
int sock_fcntl_net(int sockfd, int cmd, unsigned long arg)
{
	if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS)
		return -EBADF;
	net_socket_t *s = &sockets[sockfd];
	if (!s->active)
		return -EBADF;

	switch (cmd) {
	case F_GETFL: {
		int flags = 0;
		if (s->nonblock)
			flags |= O_NONBLOCK;
		flags |= O_RDWR;
		return flags;
	}
	case F_SETFL:
		s->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
		return 0;
	case F_GETFD:
		return 0; // No CLOEXEC tracking
	case F_SETFD:
		return 0; // Accept but ignore
	default:
		return -EINVAL;
	}
}

// ============================================================================
// sock_sendfile - Send file contents to a socket/pipe/file fd
// Reads from in_fd (must be a regular file) and writes to out_fd
// (socket, pipe, or file). If offset is non-NULL, use it as the read
// position without modifying the file's seek position.
// Returns number of bytes transferred, or negative errno.
// ============================================================================
int sock_sendfile(int out_fd, int in_fd, int64_t *offset, size_t count)
{
	/* One exit: two descriptors are held here, and every way out has to
	 * release both. */
	struct vfs_file *in_file = NULL;
	struct vfs_file *out_entry = NULL;
	int ret;

	task_t *cur = sched_current();
	if (!cur) {
		ret = -EFAULT;
		goto out;
	}
	if (count == 0) {
		ret = 0;
		goto out;
	}

	// ---- Resolve in_fd: must be a regular VFS file (seekable) ----
	if (in_fd < 0 || in_fd >= TASK_MAX_FDS) {
		ret = -EBADF;
		goto out;
	}
	/* Both descriptors are held for the whole transfer: the loop below
	 * reads from one and writes to the other, sleeping on each, and a
	 * sibling thread closing either one meanwhile must not be able to
	 * destroy what the transfer is using. */
	in_file = fdget(cur, in_fd);
	if (!in_file) {
		ret = -EBADF;
		goto out;
	}
	uintptr_t in_marker = (uintptr_t)in_file;
	// Reject sockets, pipes, console markers as input
	//
	// AF_UNIX belongs in this list and was missing from it.  Its marker is
	// a small tagged integer that is neither an AF_INET socket, an epoll
	// handle, a console stream nor a pipe end, so it passed every test
	// here and fell through to the regular-file path -- where the value
	// was handed to the filesystem layer as a vfs_file pointer and
	// dereferenced.  A descriptor kind that is merely unrecognised must be
	// refused, never assumed to be a file.
	if (IS_SOCKET_FD(in_file) || IS_EPOLL_FD(in_file) ||
	    unix_sock_is(in_file)) {
		ret = -EINVAL;
		goto out;
	}
	if (in_marker <= 3) { // console markers
		ret = -EINVAL;
		goto out;
	}
	if (pipe_is_end(in_file)) {
		ret = -EINVAL;
		goto out;
	}
	/* A file is read through a descriptor opened for reading only: the
	 * open checked the file's permissions for the mode it was asked. */
	if (!file_may_read(in_file)) {
		ret = -EBADF;
		goto out;
	}

	// ---- Resolve out_fd ----
	if (out_fd < 0 || out_fd >= TASK_MAX_FDS) {
		ret = -EBADF;
		goto out;
	}
	out_entry = fdget(cur, out_fd);
	if (!out_entry && (out_fd == 1 || out_fd == 2)) {
		// stdout/stderr without explicit fd_table entry - treat as console
	} else if (!out_entry) {
		ret = -EBADF;
		goto out;
	}

	// Determine output type
	int out_is_socket = 0;
	int out_sock_idx = -1;
	int out_is_pipe = 0;
	int out_is_console = 0;
	uintptr_t out_marker = out_entry ? (uintptr_t)out_entry : 0;

	if (out_entry && IS_SOCKET_FD(out_entry)) {
		out_is_socket = 1;
		out_sock_idx = SOCKET_FD_IDX(out_entry);
	} else if (out_entry && pipe_is_end(out_entry)) {
		out_is_pipe = 1;
	} else if (out_entry && unix_sock_is(out_entry)) {
		/* Refused explicitly, for the same reason as the input side:
		 * the fall-through below treats everything it does not
		 * recognise as a regular file. */
		ret = -EINVAL;
		goto out;
	} else if (out_marker == 2 || out_marker == 3 ||
		   (!out_entry && (out_fd == 1 || out_fd == 2))) {
		out_is_console = 1;
	} else if (out_entry && out_marker > 3 && !IS_EPOLL_FD(out_entry)) {
		// Regular file - handled by the else branch in the write loop.
		// Written only through a descriptor opened for writing.
		if (!file_may_write(out_entry)) {
			ret = -EBADF;
			goto out;
		}
	} else {
		ret = -EBADF;
		goto out;
	}

	// ---- Handle offset: if non-NULL, save and restore file position ----
	int64_t saved_pos = -1;
	if (offset) {
		// Save current file position
		saved_pos = vfs_seek(in_file, 0, 1); // SEEK_CUR
		if (saved_pos < 0) {
			ret = -ESPIPE;
			goto out;
		}
		// Seek to the requested offset
		int64_t sret = vfs_seek(in_file, *offset, 0); // SEEK_SET
		if (sret < 0) {
			ret = (int)sret;
			goto out;
		}
	}

// ---- Transfer loop ----
// Heap-allocated bounce buffer (kernel stack is only 8KB).
#define SENDFILE_BUF_SIZE 4096
	uint8_t *sendfile_buf = (uint8_t *)kalloc(SENDFILE_BUF_SIZE);
	if (!sendfile_buf) {
		if (offset)
			vfs_seek(in_file, saved_pos, 0);
		ret = -ENOMEM;
		goto out;
	}
	size_t total = 0;
	int err = 0;
	// Yield the CPU every 64 KB to stay cooperative
	size_t since_yield = 0;
#define SENDFILE_YIELD_BYTES (64 * 1024)

	while (total < count) {
		// Check for pending signals so the transfer is interruptible
		if (cur && signal_pending(cur)) {
			err = -EINTR;
			break;
		}

		size_t chunk = count - total;
		if (chunk > SENDFILE_BUF_SIZE)
			chunk = SENDFILE_BUF_SIZE;

		// Read from input file
		long nread = vfs_read(in_file, sendfile_buf, (long)chunk);
		if (nread <= 0) {
			if (nread < 0)
				err = (int)nread;
			break; // EOF or error
		}

		// Write to output
		size_t written = 0;
		while (written < (size_t)nread) {
			long nw;
			if (out_is_socket) {
				nw = sock_send(out_sock_idx,
					       sendfile_buf + written,
					       (size_t)nread - written, 0);
			} else if (out_is_pipe) {
				// Direct pipe buffer write (kernel-to-kernel)
				pipe_end_t *pe = (pipe_end_t *)out_entry;
				pipe_t *pp = pe->pipe;
				if (!pp || pe->is_read) {
					err = -EBADF;
					break;
				}
				uint64_t pflags;
				spin_lock_irqsave(&pp->lock, &pflags);
				if (pp->readers == 0) {
					spin_unlock_irqrestore(&pp->lock,
							       pflags);
					err = -EPIPE;
					break;
				}
				size_t space = pp->size - pp->used;
				size_t todo = (size_t)nread - written;
				if (todo > space)
					todo = space;
				if (todo == 0) {
					// Pipe full — wake readers and yield, then retry
					spin_unlock_irqrestore(&pp->lock,
							       pflags);
					sched_wake_channel(pp);
					poll_notify_wq(&pp->poll_wq);
					sched_schedule();
					continue;
				}
				size_t first = pp->size - pp->write_pos;
				if (first > todo)
					first = todo;
				mm_memcpy(pp->buffer + pp->write_pos,
					  sendfile_buf + written, first);
				if (todo > first)
					mm_memcpy(pp->buffer,
						  sendfile_buf + written +
							  first,
						  todo - first);
				pp->write_pos =
					(pp->write_pos + todo) % pp->size;
				pp->used += todo;
				spin_unlock_irqrestore(&pp->lock, pflags);
				sched_wake_channel(pp);
				poll_notify_wq(&pp->poll_wq);
				nw = (long)todo;
			} else if (out_is_console) {
				tty_t *tty = cur->ctty ? cur->ctty :
							 tty_get_console();
				nw = tty_write(tty, sendfile_buf + written,
					       (long)((size_t)nread - written));
			} else {
				// Regular file output
				nw = vfs_write(out_entry,
					       sendfile_buf + written,
					       (long)((size_t)nread - written));
				/* Writing drops the set-user/-group-ID bits
				 * for a non-privileged caller, as write()
				 * does (read_write.c). */
				if (nw > 0 && cur->cred.euid != 0 &&
				    !vfs_setid_clean(out_entry))
					strip_setid_file(out_entry);
			}
			if (nw <= 0) {
				if (nw < 0)
					err = (int)nw;
				break;
			}
			written += (size_t)nw;
		}
		total += written;
		if (err != 0 || written < (size_t)nread)
			break;

		// Yield periodically so we don't starve other tasks
		since_yield += written;
		if (since_yield >= SENDFILE_YIELD_BYTES) {
			since_yield = 0;
			sched_schedule();
		}
	}

#undef SENDFILE_YIELD_BYTES
#undef SENDFILE_BUF_SIZE

	kfree(sendfile_buf);

	// ---- Restore / update offset ----
	if (offset) {
		// Update offset to reflect bytes read
		*offset += (int64_t)total;
		// Restore original file position
		vfs_seek(in_file, saved_pos, 0); // SEEK_SET
	}

	ret = total > 0 ? (int)total : err;
	goto out;

out:
	if (in_file)
		fdput(in_file);
	if (out_entry)
		fdput(out_entry);
	return ret;
}

// ============================================================================
// Network info queries for userspace commands (netstat, ifconfig, etc.)
// ============================================================================

// Get TCP connection info
// net_get_tcp_connections lives in tcp.c (it needs the internal connection
// list); declared in net.h.

// Get UDP socket info
int net_get_udp_sockets(net_udp_info_t *entries, int max_entries)
{
	int count = 0;
	for (int i = 0; i < NET_MAX_SOCKETS && count < max_entries; i++) {
		net_socket_t *s = &sockets[i];
		if (!s->active || s->type != SOCK_DGRAM)
			continue;
		entries[count].local_ip =
			net_ntohl(s->local_addr.sin_addr.s_addr);
		entries[count].local_port = net_ntohs(s->local_addr.sin_port);
		entries[count].remote_ip =
			net_ntohl(s->remote_addr.sin_addr.s_addr);
		entries[count].remote_port = net_ntohs(s->remote_addr.sin_port);
		entries[count].rx_queue = skb_queue_len(&s->rx_queue);
		count++;
	}
	return count;
}

// Get interface info
int net_get_iface_info(net_iface_info_t *entries, int max_entries)
{
	int count = 0;
	// Loopback first
	net_device_t *lo = net_get_loopback();
	if (lo && count < max_entries) {
		const char *n = lo->name;
		int j = 0;
		while (n[j] && j < 15) {
			entries[count].name[j] = n[j];
			j++;
		}
		entries[count].name[j] = '\0';
		for (int m = 0; m < 6; m++)
			entries[count].mac[m] = lo->mac_addr[m];
		entries[count].mtu = lo->mtu;
		entries[count].ip_addr = lo->ip_addr;
		entries[count].netmask = lo->netmask;
		entries[count].gateway = lo->gateway;
		entries[count].dns_server = lo->dns_server;
		entries[count].flags = sock_if_flags(lo);
		entries[count].pad = 0;
		entries[count].rx_packets = lo->rx_packets;
		entries[count].tx_packets = lo->tx_packets;
		entries[count].rx_bytes = lo->rx_bytes;
		entries[count].tx_bytes = lo->tx_bytes;
		entries[count].rx_errors = lo->rx_errors;
		entries[count].tx_errors = lo->tx_errors;
		entries[count].rx_dropped = lo->rx_dropped;
		count++;
	}
	// Hardware devices
	for (int i = 0; i < net_device_count() && count < max_entries; i++) {
		net_device_t *dev = net_get_device(i);
		if (!dev)
			continue;
		const char *n = dev->name;
		int j = 0;
		while (n[j] && j < 15) {
			entries[count].name[j] = n[j];
			j++;
		}
		entries[count].name[j] = '\0';
		for (int m = 0; m < 6; m++)
			entries[count].mac[m] = dev->mac_addr[m];
		entries[count].mtu = dev->mtu;
		entries[count].ip_addr = dev->ip_addr;
		entries[count].netmask = dev->netmask;
		entries[count].gateway = dev->gateway;
		entries[count].dns_server = dev->dns_server;
		entries[count].flags = sock_if_flags(dev);
		entries[count].pad = 0;
		entries[count].rx_packets = dev->rx_packets;
		entries[count].tx_packets = dev->tx_packets;
		entries[count].rx_bytes = dev->rx_bytes;
		entries[count].tx_bytes = dev->tx_bytes;
		entries[count].rx_errors = dev->rx_errors;
		entries[count].tx_errors = dev->tx_errors;
		entries[count].rx_dropped = dev->rx_dropped;
		count++;
	}
	return count;
}
