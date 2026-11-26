// LikeOS TCP (Transmission Control Protocol): sockets
//
// The socket side of TCP: allocation and lifetime of connections, the
// calls the socket layer makes (connect, listen, accept, send, receive,
// close, shutdown, options), memory accounting and auto-tuning of the
// send buffer, and the diagnostics.  The protocol itself is in
// tcp_input.c, tcp_output.c, tcp_timer.c, tcp_ipv4.c and tcp_minisocks.c;
// the lock order and the refcount rules are described in tcp.h.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/io/console.h>
#include <kernel/io/tty.h>
#include <kernel/mm/slab.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/sched.h>
#include <kernel/net/softirq.h>
#include <kernel/dev/rand/random.h>

extern void poll_notify_io_ready(void);
extern void poll_notify_wq(struct wait_queue_head *);
extern void poll_notify_wq_key(struct wait_queue_head *, unsigned key);

volatile uint64_t tcp_xmib[TCPX_MAX];
int tcp_sysctl_mtu_probing; // black-hole detection off, as by default
int tcp_sysctl_ecn = 2; // ECN accepted when the peer asks, never requested
/* TCP's own pacing for every connection (1), or only where
 * SO_MAX_PACING_RATE asks for it (0, the default: segments leave as the
 * window allows, a pacing qdisc's job is not TCP's). */
int tcp_sysctl_internal_pacing;
BUILD_BUG_ON(sizeof(struct tcp_info) != 248);

static const char *const tcp_xmib_names[TCPX_MAX] = {
	[TCPX_DELAYEDACKS] = "DelayedACKs",
	[TCPX_QUICKACKS] = "QuickACKs",
	[TCPX_HPHITS] = "TCPHPHits",
	[TCPX_HPACKS] = "TCPHPAcks",
	[TCPX_PUREACKS] = "TCPPureAcks",
	[TCPX_OFOQUEUE] = "TCPOFOQueue",
	[TCPX_OFODROP] = "TCPOFODrop",
	[TCPX_OFOMERGE] = "TCPOFOMerge",
	[TCPX_DSACKSENT] = "TCPDSACKOldSent",
	[TCPX_DSACKOFOSENT] = "TCPDSACKOfoSent",
	[TCPX_PRUNECALLED] = "PruneCalled",
	[TCPX_RCVPRUNED] = "RcvPruned",
	[TCPX_OFOPRUNED] = "OfoPruned",
	[TCPX_RCVCOLLAPSED] = "TCPRcvCollapsed",
	[TCPX_RCVQDROP] = "TCPRcvQDrop",
	[TCPX_ZEROWINDOWDROP] = "TCPZeroWindowDrop",
	[TCPX_LISTENOVERFLOWS] = "ListenOverflows",
	[TCPX_LISTENDROPS] = "ListenDrops",
	[TCPX_REQQFULLDOCOOKIES] = "TCPReqQFullDoCookies",
	[TCPX_REQQFULLDROP] = "TCPReqQFullDrop",
	[TCPX_SYNRETRANS] = "TCPSynRetrans",
	[TCPX_ABORTONDATA] = "TCPAbortOnData",
	[TCPX_ABORTONCLOSE] = "TCPAbortOnClose",
	[TCPX_ABORTONMEMORY] = "TCPAbortOnMemory",
	[TCPX_ABORTONTIMEOUT] = "TCPAbortOnTimeout",
	[TCPX_ABORTONLINGER] = "TCPAbortOnLinger",
	[TCPX_ABORTFAILED] = "TCPAbortFailed",
	[TCPX_MEMORYPRESSURES] = "TCPMemoryPressures",
	[TCPX_MEMRECLAIMS] = "TCPMemoryReclaims",
	[TCPX_TWCREATED] = "TW",
	[TCPX_TWKILLED] = "TWKilled",
	[TCPX_TWRECYCLED] = "TWRecycled",
	[TCPX_TWREUSED] = "TWReused",
	[TCPX_PAWSESTAB] = "PAWSEstab",
	[TCPX_PAWSACTIVE] = "PAWSActive",
	[TCPX_CHALLENGEACK] = "TCPChallengeACK",
	[TCPX_SYNCHALLENGE] = "TCPSYNChallenge",
	[TCPX_ACKSKIPPEDCHALLENGE] = "TCPACKSkippedChallenge",
	[TCPX_ACKSKIPPEDSYNRECV] = "TCPACKSkippedSynRecv",
	[TCPX_ACKSKIPPEDPAWS] = "TCPACKSkippedPAWS",
	[TCPX_ACKSKIPPEDSEQ] = "TCPACKSkippedSeq",
	[TCPX_ACKSKIPPEDFINWAIT2] = "TCPACKSkippedFinWait2",
	[TCPX_ACKSKIPPEDTIMEWAIT] = "TCPACKSkippedTimeWait",
	[TCPX_DEFERACCEPTDROP] = "TCPDeferAcceptDrop",
	[TCPX_TIMEOUTS] = "TCPTimeouts",
	[TCPX_FASTRETRANS] = "TCPFastRetrans",
	[TCPX_SLOWSTARTRETRANS] = "TCPSlowStartRetrans",
	[TCPX_PARTIALRECOVERY] = "TCPPartialUndo",
	[TCPX_TSQDEFER] = "TCPTSQDefer",
	[TCPX_AUTOCORKING] = "TCPAutoCorking",
	[TCPX_WINPROBE] = "TCPWinProbe",
	[TCPX_KEEPALIVE] = "TCPKeepAlive",
	[TCPX_RCVBUFGROW] = "TCPRcvBufGrow",
	[TCPX_SNDBUFGROW] = "TCPSndBufGrow",
	[TCPX_FINWAIT2TIMEOUT] = "TCPFinWait2Timeout",
	[TCPX_ORPHANS_KILLED] = "TCPOrphansKilled",
	[TCPX_MTUREDUCED] = "TCPMTUReduced",
	[TCPX_SYNCOOKIESENT] = "SyncookiesSent",
	[TCPX_SYNCOOKIERECV] = "SyncookiesRecv",
	[TCPX_SYNCOOKIEFAILED] = "SyncookiesFailed",
	[TCPX_LOSSPROBES] = "TCPLossProbes",
	[TCPX_LOSSPROBERECOVERY] = "TCPLossProbeRecovery",
	[TCPX_SACKRECOVERY] = "TCPSackRecovery",
	[TCPX_RENORECOVERY] = "TCPRenoRecovery",
	[TCPX_SACKRENEGING] = "TCPSACKReneging",
	[TCPX_DSACKRECV] = "TCPDSACKRecv",
	[TCPX_DSACKOFORECV] = "TCPDSACKOfoRecv",
	[TCPX_DSACKIGNOREDOLD] = "TCPDSACKIgnoredOld",
	[TCPX_DSACKIGNOREDNOUNDO] = "TCPDSACKIgnoredNoUndo",
	[TCPX_DSACKIGNOREDDUBIOUS] = "TCPDSACKIgnoredDubious",
	[TCPX_DSACKUNDO] = "TCPDSACKUndo",
	[TCPX_FULLUNDO] = "TCPFullUndo",
	[TCPX_LOSSUNDO] = "TCPLossUndo",
	[TCPX_SPURIOUSRTOS] = "TCPSpuriousRTOs",
	[TCPX_SACKREORDER] = "TCPSACKReorder",
	[TCPX_TSREORDER] = "TCPTSReorder",
	[TCPX_RENOREORDER] = "TCPRenoReorder",
	[TCPX_LOSTRETRANSMIT] = "TCPLostRetransmit",
	[TCPX_SACKDISCARD] = "TCPSackDiscard",
	[TCPX_HYSTARTDELAYDETECT] = "TCPHystartDelayDetect",
	[TCPX_HYSTARTDELAYCWND] = "TCPHystartDelayCwnd",
	[TCPX_HYSTARTCSSRESUME] = "TCPHystartCSSResume",
	[TCPX_DELIVEREDCE] = "TCPDeliveredCE",
	[TCPX_ECNNEGOTIATED] = "TCPECNNegotiated",
	[TCPX_PACINGDEFER] = "TCPPacingDefer",
	[TCPX_MTUPFAIL] = "TCPMTUPFail",
};

static int tcp_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

uint64_t tcp_xmib_read(const char *name)
{
	for (int i = 0; i < TCPX_MAX; i++)
		if (tcp_xmib_names[i] && tcp_streq(tcp_xmib_names[i], name))
			return __atomic_load_n(&tcp_xmib[i], __ATOMIC_RELAXED);
	return ~0ULL;
}

// ---------------------------------------------------------------------------
// The socket lock.
//
// Plain spin_lock_irqsave() spins with IRQs disabled.  Under heavy SMP
// contention (the receive softirq holding conn->lock to deliver a burst
// while another CPU's task tries to recv from the same conn) the waiter
// could sit with interrupts off long enough to miss a TLB shootdown IPI
// from a third CPU, which freezes the system.  While the lock is
// contended, interrupts are therefore enabled briefly between attempts.
// Process or softirq context only -- never from a hard interrupt.
// ---------------------------------------------------------------------------
void tcp_lock_acquire(spinlock_t *lock, uint64_t *flags_out)
{
	uint64_t f = local_irq_save();

	while (!spin_trylock(lock)) {
		local_irq_restore(f);
		__asm__ volatile("pause" ::: "memory");
		f = local_irq_save();
	}
	*flags_out = f;
}

void tcp_lock_release(spinlock_t *lock, uint64_t flags)
{
	spin_unlock(lock);
	local_irq_restore(flags);
	/* The outermost interrupts-off section is over: send now what the
	 * device layer had to queue while the lock was held (the transmit
	 * contract in netdevice.h). */
	if (flags & (1ULL << 9))
		net_tx_flush_local();
}

// ---------------------------------------------------------------------------
// References and the reap list.
// ---------------------------------------------------------------------------
static struct tcp_skc *g_reap_list;
static spinlock_t g_reap_lock = SPINLOCK_INIT("tcp_reap");
static volatile uint32_t g_conn_count; // full sockets in existence

int tcp_skc_tryhold(struct tcp_skc *skc)
{
	int old = __atomic_load_n(&skc->refcount, __ATOMIC_RELAXED);

	while (old > 0) {
		if (__atomic_compare_exchange_n(&skc->refcount, &old, old + 1,
						1, __ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

/* Drop a reference.  At zero the object goes on the reap list; the memory
 * is freed by tcp_reap_pending() in process or softirq context.  Safe in
 * any context and under any TCP lock (it never frees inline). */
void tcp_skc_put(struct tcp_skc *skc)
{
	uint64_t f;
	int n;

	if (!skc)
		return;
	n = __atomic_sub_fetch(&skc->refcount, 1, __ATOMIC_ACQ_REL);
	BUG_ON(n < 0);
	if (n != 0)
		return;
	spin_lock_irqsave(&g_reap_lock, &f);
	if (!skc->skc_reaping) {
		skc->skc_reaping = 1;
		skc->skc_reap_next = g_reap_list;
		g_reap_list = skc;
	}
	spin_unlock_irqrestore(&g_reap_lock, f);
	softirq_raise(SOFTIRQ_TIMER);
}

void tcp_conn_get(tcp_conn_t *conn)
{
	tcp_conn_hold(conn);
}

void tcp_conn_release(tcp_conn_t *conn)
{
	tcp_conn_put(conn);
}

void tcp_purge_queues(tcp_conn_t *c)
{
	sk_buff_t *skb;

	while ((skb = __skb_dequeue(&c->write_queue)) != NULL) {
		tcp_wmem_uncharge(c, skb);
		skb_put(skb);
	}
	while ((skb = __skb_dequeue(&c->rtx_queue)) != NULL) {
		tcp_wmem_uncharge(c, skb);
		skb_put(skb);
	}
	while ((skb = __skb_dequeue(&c->rcv_queue)) != NULL) {
		tcp_rmem_uncharge(c, skb);
		skb_put(skb);
	}
	c->rcvq_bytes = 0;
	tcp_ofo_purge(c);
	c->packets_out = 0;
	c->retrans_out = 0;
	c->sacked_out = 0;
	c->lost_out = 0;
	c->retransmit_skb_hint = NULL;
	tcp_tsorted_reset(c);
	tcp_rtx_idx_reset(c);
}

static void tcp_conn_free(tcp_conn_t *c)
{
	BUG_ON(c->skc_hashed || c->skc_bound);
	WARN_ON(net_timer_pending(&c->retransmit_timer) ||
		net_timer_pending(&c->delack_timer) ||
		net_timer_pending(&c->keepalive_timer) ||
		net_timer_pending(&c->pacing_timer));
	tcp_ca_release(c);
	tcp_purge_queues(c);
	dst_cache_reset(&c->dst_cache);
	c->active = 0;
	c->skc_kind = 0;
	slab_free(c);
	__atomic_fetch_sub(&g_conn_count, 1, __ATOMIC_ACQ_REL);
}

static void tcp_pending_softirq(void)
{
	tcp_reap_pending();
	tcp_minisocks_maintain();
	tcp_mem_reclaim();
}

// Free everything whose last reference is gone.  Process/softirq context
// with interrupts enabled (slab_free).
void tcp_reap_pending(void)
{
	for (;;) {
		struct tcp_skc *skc;
		uint64_t f;

		spin_lock_irqsave(&g_reap_lock, &f);
		skc = g_reap_list;
		if (skc)
			g_reap_list = skc->skc_reap_next;
		spin_unlock_irqrestore(&g_reap_lock, f);
		if (!skc)
			return;
		skc->skc_reap_next = NULL;
		switch (skc->skc_kind) {
		case TCP_SKC_FULL:
			tcp_conn_free((tcp_conn_t *)skc);
			break;
		case TCP_SKC_REQ:
			tcp_reqsk_free((struct tcp_request_sock *)skc);
			break;
		case TCP_SKC_TW:
			tcp_tw_free((struct tcp_tw_sock *)skc);
			break;
		default:
			WARN_ON_ONCE(1);
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Allocation.
// ---------------------------------------------------------------------------
tcp_conn_t *tcp_conn_alloc(void)
{
	tcp_conn_t *c;

	WARN_ON_ONCE(irqs_disabled());
	c = (tcp_conn_t *)slab_alloc(sizeof(*c));
	if (!c)
		return NULL;
	mm_memset(c, 0, sizeof(*c));
	__atomic_fetch_add(&g_conn_count, 1, __ATOMIC_ACQ_REL);
	c->skc_kind = TCP_SKC_FULL;
	c->state = TCP_STATE_CLOSED;
	c->refcount = 1; // the protocol's reference
	c->proto_ref = 1;
	c->lock = (spinlock_t)SPINLOCK_INIT("tcp_conn");
	c->active = 1;
	skb_queue_init(&c->write_queue, "tcp_wq");
	skb_queue_init(&c->rtx_queue, "tcp_rtxq");
	skb_queue_init(&c->rcv_queue, "tcp_rcvq");
	c->ooo_queue.rb_node = NULL;
	tcp_init_xmit_timers(c);

	c->tx_ready = 1;
	c->rto_us = TCP_RTO_INITIAL_US;
	c->cwnd = TCP_INIT_CWND;
	c->ssthresh = TCP_INFINITE_SSTHRESH;
	c->ca_ops = tcp_ca_default();
	c->reordering = TCP_REORDERING;
	c->rack.reo_wnd_steps = 1;
	for (int i = 0; i < 3; i++)
		c->min_rtt[i].v = ~0u;
	c->inet_id = (uint16_t)random_u32();
	c->peer_mss = TCP_MSS_DEFAULT;
	c->mss_clamp = TCP_MSS_DEFAULT;
	c->mss_cache = TCP_MSS_DEFAULT;
	c->advmss = TCP_MSS;
	c->tcp_header_len = sizeof(tcp_header_t);
	c->rcv_mss = TCP_MSS_DEFAULT;
	c->rcvbuf = TCP_RMEM_DEFAULT;
	c->sndbuf = TCP_WMEM_DEFAULT;
	c->scaling_ratio = TCP_DEFAULT_SCALING_RATIO;
	/* Nagle is off by default here, as it always was: applications on
	 * this system are written for a stack that never held a small
	 * write back (TCP_NODELAY=0 turns it on). */
	c->nodelay = 1;
	c->nonagle = TCP_NAGLE_OFF;
	c->keepidle_ticks = (uint32_t)timer_s_to_ticks(TCP_KEEPALIVE_TIME_S);
	c->keepintvl_ticks = (uint32_t)timer_s_to_ticks(TCP_KEEPALIVE_INTVL_S);
	c->keepcnt = TCP_KEEPALIVE_PROBES;
	c->last_rx_tick = tcp_now();
	c->notsent_lowat = TCP_NOTSENT_LOWAT_DEFAULT;
	c->max_pacing_rate = ~0ULL;
	return c;
}

// ---------------------------------------------------------------------------
// Notifications.
// ---------------------------------------------------------------------------
/* Wake whoever is polling the SOCKET this connection belongs to.  A
 * connection has no waiters of its own; owner_socket is the socket, or the
 * self sentinel / NULL when none can be polling it -- then only the global
 * poll sequence is bumped, which a poller between its scan and its park
 * compares. */
/* `key': the POLL* events that became true (0: a state change, every
 * poller looks).  Edge-triggered epoll items only fire for their own
 * events: a reader is not woken by an ACK that freed send space. */
void tcp_poll_notify_key(tcp_conn_t *c, unsigned key)
{
	net_socket_t *s = c ? (net_socket_t *)c->owner_socket : NULL;

	if (s && (void *)s != (void *)c)
		poll_notify_wq_key(&s->poll_wq, key);
	else
		poll_notify_io_ready();
}

void tcp_poll_notify(tcp_conn_t *c)
{
	tcp_poll_notify_key(c, 0);
}

/* New data is on the receive queue (rcvq_bytes already counts it).  A
 * reader waiting for SO_RCVLOWAT clears rx_ready without the lock, then
 * looks at rcvq_bytes (sock_recv); here rcvq_bytes was stored, then
 * rx_ready is exchanged -- a full barrier -- so either the reader sees
 * the new count, or the exchange sees its 0 and wakes it. */
void tcp_wake_reader(tcp_conn_t *c)
{
	int was = __atomic_exchange_n(&c->rx_ready, 1, __ATOMIC_SEQ_CST);

	tcp_poll_notify_key(c, POLLIN | POLLRDNORM);
	if (!was)
		sched_wake_channel((void *)&c->rx_ready);
}

void tcp_listener_notify(tcp_conn_t *l)
{
	tcp_poll_notify_key(l, POLLIN | POLLRDNORM);
}

// ---------------------------------------------------------------------------
// Orphans: connections the application closed that still finish their
// protocol.  They are bounded (TCP_MAX_ORPHANS); beyond it a closing
// connection is reset instead of lingering.
// ---------------------------------------------------------------------------
static volatile uint32_t g_orphans;

uint32_t tcp_orphan_count(void)
{
	return __atomic_load_n(&g_orphans, __ATOMIC_RELAXED);
}

int tcp_orphan_too_many(void)
{
	return tcp_orphan_count() > TCP_MAX_ORPHANS;
}

void tcp_orphan_inc(tcp_conn_t *c)
{
	if (!c->orphan_counted) {
		c->orphan_counted = 1;
		__atomic_fetch_add(&g_orphans, 1, __ATOMIC_RELAXED);
	}
}

void tcp_orphan_dec(tcp_conn_t *c)
{
	if (c->orphan_counted) {
		c->orphan_counted = 0;
		__atomic_fetch_sub(&g_orphans, 1, __ATOMIC_RELAXED);
	}
}

// ---------------------------------------------------------------------------
// State changes.
// ---------------------------------------------------------------------------
void tcp_set_state(tcp_conn_t *c, int state)
{
	int old = c->state;

	if (state == TCP_STATE_CLOSED && old != TCP_STATE_CLOSED)
		tcp_unhash(c);
	c->state = state;
	if (state == TCP_STATE_CLOSED)
		tcp_orphan_dec(c);
}

/* The connection is over: CLOSED, off every table, no timer pending, the
 * protocol's reference dropped.  Waiters are woken.  Called with
 * conn->lock held by a caller that holds its own reference. */
void tcp_done(tcp_conn_t *c)
{
	tcp_set_state(c, TCP_STATE_CLOSED);
	tcp_clear_xmit_timers(c);
	c->connect_done = 1;
	c->tx_ready = 1;
	c->rx_ready = 1;
	tcp_poll_notify(c);
	sched_wake_channel((void *)&c->rx_ready);
	if (c->proto_ref) {
		c->proto_ref = 0;
		tcp_conn_put(c); // the caller's own reference keeps it alive
	}
}

/* The pending hard error, taken: reported once (by recv() with nothing
 * left to read, send(), or SO_ERROR), after which a reset connection reads
 * end of file and refuses writes with EPIPE.  0 when none is pending.
 * Any context, no lock. */
int tcp_sock_error(tcp_conn_t *c)
{
	if (!c || !__atomic_load_n(&c->error, __ATOMIC_ACQUIRE))
		return 0;
	return __atomic_exchange_n(&c->error, 0, __ATOMIC_ACQ_REL);
}

/* The connection failed (reset by the peer, timed out, refused): record
 * the error the application sees, drop what was still to be sent, and
 * finish it.  What was received in order stays readable: the reader gets
 * it before the error.  Caller holds conn->lock and a reference. */
void tcp_reset(tcp_conn_t *c, int err)
{
	__atomic_store_n(&c->error, err, __ATOMIC_RELEASE);
	while (!skb_queue_empty(&c->write_queue) ||
	       !skb_queue_empty(&c->rtx_queue)) {
		sk_buff_t *skb = __skb_dequeue(&c->write_queue);

		if (!skb)
			skb = __skb_dequeue(&c->rtx_queue);
		tcp_wmem_uncharge(c, skb);
		skb_put(skb);
	}
	c->packets_out = c->retrans_out = c->sacked_out = c->lost_out = 0;
	c->retransmit_skb_hint = NULL;
	tcp_tsorted_reset(c);
	tcp_rtx_idx_reset(c);
	c->last_rx_tick = tcp_now();
	tcp_done(c);
}

// ---------------------------------------------------------------------------
// Memory accounting.
//
// Every skb on a TCP queue is charged by its truesize to its connection
// (rmem_alloc for the receive and out-of-order queues, wmem_queued for the
// write and retransmit queues) and to the global total.  The global total
// has three thresholds: below TCP_MEM_LOW nothing is limited; above
// TCP_MEM_PRESSURE the buffers stop growing, the receive windows are held
// at what they are and out-of-order data is pruned first -- the pressure
// state is entered there and left only below TCP_MEM_LOW (hysteresis);
// above TCP_MEM_HIGH new data is refused, except what keeps every
// connection going: in-order data onto an empty receive queue, and up to
// TCP_RMEM_MIN of it on any connection (tcp_input.c).
//
// Out-of-order data parked by peers that never fill the hole could hold
// the total above the hard limit for as long as their connections live
// (idle ones do not prune themselves: nothing arrives for them).  Above
// TCP_MEM_HIGH the reclaim therefore takes the out-of-order queues of
// every connection, from the reap softirq, until the total is below
// TCP_MEM_PRESSURE again.
// ---------------------------------------------------------------------------
static volatile int64_t g_tcp_mem;
static volatile int g_tcp_mem_pressure;
static volatile int g_tcp_reclaim_pending;
static uint64_t g_tcp_reclaim_tick; // the last reclaim pass (reap softirq)
static net_timer_t g_tcp_reclaim_timer; // a deferred pass

static void tcp_mem_reclaim_timer_fn(net_timer_t *t)
{
	(void)t;
	softirq_raise(SOFTIRQ_TIMER);
}

void tcp_mem_charge(int64_t bytes)
{
	int64_t v = __atomic_add_fetch(&g_tcp_mem, bytes, __ATOMIC_RELAXED);

	if (v > (int64_t)TCP_MEM_PRESSURE) {
		if (!__atomic_exchange_n(&g_tcp_mem_pressure, 1,
					 __ATOMIC_RELAXED))
			TCPX_INC(TCPX_MEMORYPRESSURES);
		if (bytes > 0 && v > (int64_t)TCP_MEM_HIGH)
			tcp_mem_reclaim_kick();
	} else if (v < (int64_t)TCP_MEM_LOW) {
		if (__atomic_load_n(&g_tcp_mem_pressure, __ATOMIC_RELAXED))
			__atomic_store_n(&g_tcp_mem_pressure, 0,
					 __ATOMIC_RELAXED);
	}
}

/* Ask for a reclaim pass (any context, under any TCP lock). */
void tcp_mem_reclaim_kick(void)
{
	if (!__atomic_exchange_n(&g_tcp_reclaim_pending, 1, __ATOMIC_ACQ_REL))
		softirq_raise(SOFTIRQ_TIMER);
}

static int tcp_mem_reclaim_one(tcp_conn_t *c, void *arg)
{
	uint64_t f;

	(void)arg;
	tcp_lock_acquire(&c->lock, &f);
	if (c->state != TCP_STATE_CLOSED && c->state != TCP_STATE_LISTEN)
		tcp_ofo_reclaim(c);
	tcp_lock_release(&c->lock, f);
	/* Done once below the pressure mark: what is left is ordinary
	 * use; the hysteresis keeps the pressure state on until LOW. */
	return tcp_mem_allocated() < TCP_MEM_PRESSURE;
}

/* The reclaim pass: from the reap softirq (interrupts on, no lock held),
 * at most once per TCP_MEM_RECLAIM_MS -- a peer that keeps pushing the
 * total over the limit costs one table walk per interval. */
void tcp_mem_reclaim(void)
{
	uint64_t now = tcp_now();

	if (!__atomic_load_n(&g_tcp_reclaim_pending, __ATOMIC_ACQUIRE) ||
	    irqs_disabled())
		return;
	if (g_tcp_reclaim_tick &&
	    now - g_tcp_reclaim_tick < tcp_ms(TCP_MEM_RECLAIM_MS)) {
		// the pass is still owed: when the interval is over
		net_timer_mod(&g_tcp_reclaim_timer,
			      g_tcp_reclaim_tick + tcp_ms(TCP_MEM_RECLAIM_MS));
		return;
	}
	__atomic_store_n(&g_tcp_reclaim_pending, 0, __ATOMIC_RELEASE);
	g_tcp_reclaim_tick = now ? now : 1;
	if (!tcp_mem_over_limit())
		return;
	TCPX_INC(TCPX_MEMRECLAIMS);
	tcp_ehash_for_each_ofo(tcp_mem_reclaim_one, NULL);
}

int tcp_under_memory_pressure(void)
{
	return __atomic_load_n(&g_tcp_mem_pressure, __ATOMIC_RELAXED);
}

int tcp_mem_over_limit(void)
{
	return __atomic_load_n(&g_tcp_mem, __ATOMIC_RELAXED) >
	       (int64_t)TCP_MEM_HIGH;
}

uint64_t tcp_mem_allocated(void)
{
	int64_t v = __atomic_load_n(&g_tcp_mem, __ATOMIC_RELAXED);

	return v > 0 ? (uint64_t)v : 0;
}

void tcp_rmem_charge(tcp_conn_t *c, sk_buff_t *skb)
{
	c->rmem_alloc += skb->truesize;
	tcp_mem_charge(skb->truesize);
}

void tcp_rmem_uncharge(tcp_conn_t *c, sk_buff_t *skb)
{
	WARN_ON_ONCE(c->rmem_alloc < skb->truesize);
	c->rmem_alloc -= tcp_min32(c->rmem_alloc, skb->truesize);
	tcp_mem_charge(-(int64_t)skb->truesize);
}

void tcp_wmem_charge(tcp_conn_t *c, sk_buff_t *skb)
{
	c->wmem_queued += skb->truesize;
	tcp_mem_charge(skb->truesize);
}

void tcp_wmem_uncharge(tcp_conn_t *c, sk_buff_t *skb)
{
	WARN_ON_ONCE(c->wmem_queued < skb->truesize);
	c->wmem_queued -= tcp_min32(c->wmem_queued, skb->truesize);
	tcp_mem_charge(-(int64_t)skb->truesize);
}

/* Room to queue more data: the send buffer is not full, not much is
 * waiting unsent beyond TCP_NOTSENT_LOWAT, and the global limit holds. */
int tcp_stream_memory_free(const tcp_conn_t *c)
{
	if (c->wmem_queued >= (uint32_t)c->sndbuf)
		return 0;
	if (c->notsent_lowat && tcp_notsent_bytes(c) >= c->notsent_lowat)
		return 0;
	if (tcp_mem_over_limit() && c->wmem_queued > (uint32_t)TCP_WMEM_MIN)
		return 0;
	return 1;
}

/* Writable for poll(): a third of the send buffer free (what sockets
 * everywhere promise a POLLOUT means), and under the not-sent mark. */
static int tcp_stream_is_writeable(const tcp_conn_t *c)
{
	uint32_t q = c->wmem_queued;

	if ((uint64_t)q * 3 > (uint64_t)c->sndbuf * 2)
		return 0;
	if (c->notsent_lowat && tcp_notsent_bytes(c) >= c->notsent_lowat)
		return 0;
	return 1;
}

/* The send queue shrank (an ACK, a reset) or the buffer grew: tell a
 * writer parked on POLLOUT.  Caller holds conn->lock. */
void tcp_update_tx_ready(tcp_conn_t *c)
{
	int st = c->state;

	if (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_CLOSE_WAIT) {
		if (!c->tx_ready) {
			c->tx_ready = 1;
			tcp_poll_notify_key(c, POLLOUT | POLLWRNORM);
		}
		return;
	}
	if (!c->tx_ready && tcp_stream_is_writeable(c)) {
		c->tx_ready = 1;
		tcp_poll_notify_key(c, POLLOUT | POLLWRNORM);
	}
}

static uint32_t tcp_truesize_for(uint32_t payload)
{
	/* What skb_alloc charges for a buffer of this payload: one slot of
	 * the class it falls in (skb.h). */
	uint32_t slot = payload + TCP_MAX_HEADER <= SKB_SMALL_DATA ?
				SKB_SMALL_DATA :
				SKB_JUMBO_DATA;

	return slot + (uint32_t)sizeof(skb_slot_t);
}

/* The send buffer follows the congestion window: room for two windows of
 * full-sized segments (one in flight, one being filled), unless the
 * application set SO_SNDBUF. */
void tcp_sndbuf_expand(tcp_conn_t *c)
{
	uint32_t per_mss, nr_segs;
	uint64_t sndmem;

	if (c->userlocks & TCP_SNDBUF_LOCK)
		return;
	per_mss = tcp_truesize_for(tcp_max32(c->mss_clamp, c->mss_cache));
	nr_segs = tcp_max32(TCP_INIT_CWND, c->cwnd);
	nr_segs = tcp_max32(nr_segs, TCP_REORDERING + 1);
	sndmem = 2ULL * nr_segs * per_mss;
	if (sndmem > TCP_WMEM_MAX)
		sndmem = TCP_WMEM_MAX;
	if ((uint64_t)c->sndbuf < sndmem) {
		c->sndbuf = (int)sndmem;
		TCPX_INC(TCPX_SNDBUFGROW);
	}
}

uint32_t tcp_win_from_space(const tcp_conn_t *c, int space)
{
	if (space <= 0)
		return 0;
	return (uint32_t)(((uint64_t)space * c->scaling_ratio) >> 8);
}

int tcp_space_from_win(const tcp_conn_t *c, uint32_t win)
{
	uint64_t v = ((uint64_t)win << 8) / (c->scaling_ratio ? c->scaling_ratio :
								 1);

	return v > 0x7FFFFFFF ? 0x7FFFFFFF : (int)v;
}

int tcp_space(const tcp_conn_t *c)
{
	int free = c->rcvbuf - (int)c->rmem_alloc;

	return (int)tcp_win_from_space(c, free);
}

int tcp_full_space(const tcp_conn_t *c)
{
	return (int)tcp_win_from_space(c, c->rcvbuf);
}

/* The handshake completed: size the buffers for the connection (window
 * clamp, the first auto-tuning measurement, the send buffer).  Caller
 * holds conn->lock. */
void tcp_init_buffer_space(tcp_conn_t *c)
{
	uint32_t maxwin;

	tcp_sndbuf_expand(c);
	c->rcvq_space.space = tcp_min32(tcp_min32(c->rcv_ssthresh, c->rcv_wnd),
					TCP_INIT_CWND * (uint32_t)c->advmss);
	c->rcvq_space.time_us = tcp_now_us();
	c->rcvq_space.seq = c->copied_seq;
	maxwin = (uint32_t)tcp_full_space(c);
	if (c->window_clamp >= maxwin)
		c->window_clamp = maxwin;
	/* Keep one segment of the buffer in reserve. */
	if (c->window_clamp > 2u * c->advmss &&
	    c->window_clamp + c->advmss > maxwin)
		c->window_clamp = tcp_max32(2u * c->advmss, maxwin - c->advmss);
	c->rcv_ssthresh = tcp_min32(c->rcv_ssthresh, c->window_clamp);
}

// ---------------------------------------------------------------------------
// Active open.
// ---------------------------------------------------------------------------
/* The initial receive window and window scale (the SYN offers them).
 * The window is the free space rounded down to whole segments and to the
 * 16-bit field; the scale is chosen from the largest buffer the
 * connection may ever grow to -- tcp_rmem's maximum, or the size the
 * application fixed with SO_RCVBUF. */
void tcp_select_initial_window(uint32_t space, uint32_t mss, uint32_t clamp,
			       int rcvbuf_locked, uint32_t *rcv_wnd,
			       uint32_t *window_clamp, uint8_t *rcv_wscale)
{
	uint32_t wspace;
	int wscale = 0;

	if (!clamp)
		clamp = 65535u << TCP_MAX_WSCALE;
	space = tcp_min32(clamp, space);
	if (mss && space > mss)
		space = (space / mss) * mss;
	*rcv_wnd = tcp_min32(space, 65535u);
	wspace = rcvbuf_locked ? space : tcp_max32(space, TCP_RMEM_MAX);
	wspace = tcp_min32(wspace, clamp);
	/* The scale that is just enough: the smallest with
	 * 65535 << scale >= space, which for the default tcp_rmem maximum is
	 * 7 -- what the common stack offers. */
	while (wscale < TCP_MAX_WSCALE && (65535u << wscale) < wspace)
		wscale++;
	*rcv_wscale = (uint8_t)wscale;
	*window_clamp = tcp_min32(65535u << wscale, clamp);
}

static void tcp_select_initial_window_conn(tcp_conn_t *c, uint32_t space,
					   uint32_t mss)
{
	tcp_select_initial_window(space, mss, c->window_clamp,
				  c->userlocks & TCP_RCVBUF_LOCK, &c->rcv_wnd,
				  &c->window_clamp, &c->rcv_wscale);
	c->rcv_ssthresh = c->rcv_wnd;
}

/* Route, MSS and window of a connection about to send its SYN (active)
 * or SYN-ACK (passive child).  `pmtu' is the path MTU. */
static void tcp_connect_init(tcp_conn_t *c, uint32_t pmtu)
{
	uint32_t mss;

	if (c->userlocks & TCP_RCVBUF_LOCK) {
		uint32_t full = (uint32_t)tcp_full_space(c);

		if (!c->window_clamp || c->window_clamp > full)
			c->window_clamp = full;
	}
	c->tcp_header_len = sizeof(tcp_header_t) + 12; // with timestamps
	mss = pmtu > 40 + TCP_MIN_MSS ? pmtu - 40 : TCP_MIN_MSS;
	if (mss > 65535u - 40u)
		mss = 65535u - 40u;
	if (c->user_mss && c->user_mss < mss)
		mss = c->user_mss;
	c->advmss = (uint16_t)mss;
	c->mss_clamp = c->user_mss ? c->user_mss : (uint16_t)mss;
	c->pmtu_cookie = pmtu;
	tcp_sync_mss(c, pmtu);
	tcp_select_initial_window_conn(c, (uint32_t)tcp_full_space(c), c->advmss);
	c->rcv_nxt = 0;
	c->rcv_wup = 0;
	c->copied_seq = 0;
}

/* Path MTU toward `daddr' (and the route into the connection's cache). */
static uint32_t tcp_route_mtu(tcp_conn_t *c, uint32_t daddr, uint32_t saddr,
			      net_device_t *dev)
{
	struct ip_route rt;
	uint32_t mtu;

	if (ip_route_out(daddr, saddr, NULL, &rt) == 0) {
		mtu = rt.loopback ? rt.mtu : ip_route_mtu(&rt, IP_PMTUDISC_WANT);
		if (rt.dst)
			dst_cache_set(&c->dst_cache, rt.dst, daddr);
		if (!c->dev)
			c->dev = rt.dev;
		ip_route_put(&rt);
	} else {
		mtu = dev && dev->mtu ? dev->mtu : NET_MTU_DEFAULT;
	}
	if (mtu < 68)
		mtu = 68;
	return mtu;
}

tcp_conn_t *tcp_connect(net_device_t *dev, uint32_t local_ip, uint32_t dst_ip,
			uint16_t src_port, uint16_t dst_port)
{
	return tcp_connect_opts(dev, local_ip, dst_ip, src_port, dst_port,
				NULL);
}

tcp_conn_t *tcp_connect_opts(net_device_t *dev, uint32_t local_ip,
			     uint32_t dst_ip, uint16_t src_port,
			     uint16_t dst_port, const struct tcp_conn_params *p)
{
	uint64_t f;
	tcp_conn_t *c;
	uint32_t pmtu;
	int rc;

	might_sleep();
	tcp_reap_pending();
	c = tcp_conn_alloc();
	if (!c) {
		NET_STATS_INC(NET_MIB_TCP_CONNTABLEFULL);
		return NULL;
	}
	if (p) {
		/* What the socket set before connect(): the SYN's window
		 * scale and MSS depend on it. */
		c->userlocks = p->userlocks;
		if ((p->userlocks & TCP_SNDBUF_LOCK) && p->sndbuf > 0)
			c->sndbuf = p->sndbuf > TCP_WMEM_MIN ? p->sndbuf :
							       TCP_WMEM_MIN;
		if ((p->userlocks & TCP_RCVBUF_LOCK) && p->rcvbuf > 0)
			c->rcvbuf = p->rcvbuf > TCP_RMEM_MIN ? p->rcvbuf :
							       TCP_RMEM_MIN;
		c->user_mss = p->user_mss;
		c->syn_retries = p->syn_retries;
		c->window_clamp = p->window_clamp;
	}
	c->dev = dev;
	c->local_ip = local_ip;
	c->remote_ip = dst_ip;
	c->local_port = src_port;
	c->remote_port = dst_port;
	c->active_open = 1;
	c->owner_socket = c; // the tcp layer's until the socket attaches
	c->ts_offset = tcp_ts_offset_for(local_ip, dst_ip, src_port, dst_port);
	pmtu = tcp_route_mtu(c, dst_ip, local_ip, dev);
	tcp_connect_init(c, pmtu);

	c->iss = tcp_isn(local_ip, dst_ip, src_port, dst_port);
	c->snd_una = c->iss;
	c->snd_nxt = c->iss;
	c->write_seq = c->iss;
	c->pushed_seq = c->iss;
	c->snd_wnd = 0;
	c->state = TCP_STATE_SYN_SENT;

	// The caller (sock_connect) owns this reference as s->tcp.
	tcp_conn_hold(c);

	/* Into the tables: the 4-tuple must be free, or held by a TIME_WAIT
	 * sock that may be reused (tcp_hash_connect decides). */
	rc = tcp_hash_connect(c, p && p->user_port);
	if (rc < 0) {
		/* The 4-tuple is taken (by a connection, or a TIME_WAIT sock
		 * that may not be reused): the address is not available. */
		if (p && p->err)
			*p->err = -EADDRNOTAVAIL;
		c->state = TCP_STATE_CLOSED;
		c->proto_ref = 0;
		tcp_conn_put(c); // protocol reference
		tcp_conn_put(c); // the caller's
		return NULL;
	}
	NET_STATS_INC(NET_MIB_TCP_ACTIVEOPENS);

	tcp_lock_acquire(&c->lock, &f);
	tcp_send_syn(c);
	tcp_lock_release(&c->lock, f);
	return c;
}

// ---------------------------------------------------------------------------
// Passive open.
// ---------------------------------------------------------------------------
tcp_conn_t *tcp_listen(net_device_t *dev, uint32_t local_ip,
		       uint16_t local_port, int backlog)
{
	tcp_conn_t *c;

	might_sleep();
	tcp_reap_pending();
	c = tcp_conn_alloc();
	if (!c) {
		NET_STATS_INC(NET_MIB_TCP_CONNTABLEFULL);
		return NULL;
	}
	c->dev = dev;
	c->local_ip = local_ip;
	c->local_port = local_port;
	c->owner_socket = c;
	if (backlog < 0)
		backlog = 0;
	if (backlog > TCP_SOMAXCONN)
		backlog = TCP_SOMAXCONN;
	c->backlog = backlog;
	c->max_ack_backlog = (uint32_t)backlog;
	c->advmss = (uint16_t)(dev && dev->mtu > 40 ? dev->mtu - 40 : TCP_MSS);
	c->state = TCP_STATE_LISTEN;
	tcp_conn_hold(c); // the caller's (sock_listen) reference
	tcp_hash_listener(c);
	return c;
}

/* listen() on a socket that is already listening changes its backlog and
 * nothing else: the same listener, the same queues.  Connections already
 * queued beyond a smaller new limit stay queued; only new ones are held
 * to it. */
void tcp_listen_set_backlog(tcp_conn_t *l, int backlog)
{
	uint64_t f;

	if (backlog < 0)
		backlog = 0;
	if (backlog > TCP_SOMAXCONN)
		backlog = TCP_SOMAXCONN;
	tcp_lock_acquire(&l->lock, &f);
	l->backlog = backlog;
	__atomic_store_n(&l->max_ack_backlog, (uint32_t)backlog,
			 __ATOMIC_RELEASE);
	tcp_lock_release(&l->lock, f);
}

/* Take a connection off the accept queue.  The queue's reference moves to
 * the caller.  A child that died on the queue (reset by its peer) is
 * dropped and the next one taken. */
tcp_conn_t *tcp_accept(tcp_conn_t *l)
{
	uint64_t f;

	if (!l)
		return NULL;
	for (;;) {
		tcp_conn_t *child;
		int st;

		tcp_lock_acquire(&l->lock, &f);
		if (l->state != TCP_STATE_LISTEN) {
			tcp_lock_release(&l->lock, f);
			return NULL;
		}
		child = l->accept_head;
		if (child) {
			l->accept_head = child->accept_next;
			if (!l->accept_head)
				l->accept_tail = NULL;
			child->accept_next = NULL;
			l->ack_backlog--;
		}
		if (!l->accept_head)
			l->accept_ready = 0;
		tcp_lock_release(&l->lock, f);
		if (!child)
			return NULL;
		st = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
		if (st == TCP_STATE_ESTABLISHED || st == TCP_STATE_CLOSE_WAIT)
			return child;
		tcp_conn_put(child); // died before accept()
	}
}

// ---------------------------------------------------------------------------
// Close and shutdown.
// ---------------------------------------------------------------------------
/* The state a close (or shutdown(SHUT_WR)) moves to; 1 when a FIN must be
 * sent (RFC 9293 3.10.4). */
static int tcp_close_state(tcp_conn_t *c)
{
	switch (c->state) {
	case TCP_STATE_ESTABLISHED:
	case TCP_STATE_SYN_RECEIVED:
		tcp_set_state(c, TCP_STATE_FIN_WAIT_1);
		return 1;
	case TCP_STATE_CLOSE_WAIT:
		tcp_set_state(c, TCP_STATE_LAST_ACK);
		return 1;
	case TCP_STATE_SYN_SENT:
		tcp_set_state(c, TCP_STATE_CLOSED);
		return 0;
	default:
		return 0;
	}
}

/* An orphan in FIN_WAIT2 waits for the peer's FIN at most TCP_LINGER2 (the
 * fin timeout); it does so as a TIME_WAIT sock, which frees the full
 * socket.  Caller holds conn->lock. */
static void tcp_orphan_fin_wait2(tcp_conn_t *c)
{
	uint64_t tmo;

	if (c->linger2_ms < 0) {
		tcp_set_state(c, TCP_STATE_CLOSED);
		tcp_send_active_reset(c);
		TCPX_INC(TCPX_ABORTONLINGER);
		tcp_done(c);
		return;
	}
	tmo = tcp_ms(c->linger2_ms ? (uint64_t)c->linger2_ms :
				     TCP_FIN_TIMEOUT_MS);
	if (tmo > tcp_ms(TCP_TIMEWAIT_LEN_MS))
		tcp_reset_keepalive_timer(c, tmo - tcp_ms(TCP_TIMEWAIT_LEN_MS));
	else
		tcp_time_wait(c, TCP_STATE_FIN_WAIT_2, tmo);
}

/* Shut down a listener: the request socks go, the children nobody
 * accepted are reset.  Caller holds a reference on l. */
static void tcp_close_listener(tcp_conn_t *l)
{
	tcp_conn_t *list;
	uint64_t f;

	tcp_lock_acquire(&l->lock, &f);
	list = l->accept_head;
	l->accept_head = NULL;
	l->accept_tail = NULL;
	l->ack_backlog = 0;
	l->accept_ready = 0;
	tcp_listen_stop(l); // request socks
	tcp_done(l);
	tcp_lock_release(&l->lock, f);

	while (list) {
		tcp_conn_t *child = list;

		list = child->accept_next;
		child->accept_next = NULL;
		tcp_lock_acquire(&child->lock, &f);
		child->owner_socket = NULL;
		if (child->state != TCP_STATE_CLOSED) {
			tcp_send_active_reset(child);
			tcp_done(child);
		}
		tcp_lock_release(&child->lock, f);
		tcp_conn_put(child); // the accept queue's reference
	}
}

/* close(): the application is done with the connection.  The caller holds
 * a reference (the socket's, dropped after this returns).  Unread data
 * turns the close into a reset (RFC 2525 2.17: the peer must learn that
 * what it sent was not read); otherwise the FIN goes after the queued
 * data and the connection finishes as an orphan. */
int tcp_close(tcp_conn_t *c)
{
	uint64_t f;
	uint32_t unread;

	if (!c)
		return -1;
	if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == TCP_STATE_LISTEN) {
		tcp_close_listener(c);
		tcp_reap_pending();
		return 0;
	}
	tcp_lock_acquire(&c->lock, &f);
	c->owner_socket = NULL;
	unread = c->rcvq_bytes;
	{
		sk_buff_t *skb;

		while ((skb = __skb_dequeue(&c->rcv_queue)) != NULL) {
			tcp_rmem_uncharge(c, skb);
			skb_put(skb);
		}
		c->rcvq_bytes = 0;
		c->rx_ready = 0;
	}
	if (c->state == TCP_STATE_CLOSED) {
		// already over (reset, timed out, TIME_WAIT handed over)
	} else if (unread) {
		tcp_set_state(c, TCP_STATE_CLOSED);
		tcp_send_active_reset(c);
		TCPX_INC(TCPX_ABORTONCLOSE);
		tcp_done(c);
	} else {
		if (tcp_close_state(c))
			tcp_send_fin(c);
		if (c->state == TCP_STATE_CLOSED) {
			tcp_done(c); // SYN_SENT: nothing to finish
		} else {
			tcp_orphan_inc(c);
			if (c->state == TCP_STATE_FIN_WAIT_2) {
				tcp_orphan_fin_wait2(c);
			} else if (tcp_orphan_too_many()) {
				tcp_set_state(c, TCP_STATE_CLOSED);
				tcp_send_active_reset(c);
				TCPX_INC(TCPX_ORPHANS_KILLED);
				tcp_done(c);
			}
		}
	}
	tcp_lock_release(&c->lock, f);
	tcp_reap_pending();
	return 0;
}

/* shutdown(SHUT_WR): our direction ends with a FIN after the queued data;
 * the connection and its receive side stay. */
int tcp_shutdown(tcp_conn_t *c)
{
	uint64_t f;

	if (!c)
		return -1;
	tcp_lock_acquire(&c->lock, &f);
	if (c->state == TCP_STATE_ESTABLISHED ||
	    c->state == TCP_STATE_SYN_SENT ||
	    c->state == TCP_STATE_SYN_RECEIVED ||
	    c->state == TCP_STATE_CLOSE_WAIT) {
		if (tcp_close_state(c))
			tcp_send_fin(c);
		else if (c->state == TCP_STATE_CLOSED)
			tcp_done(c);
	}
	tcp_lock_release(&c->lock, f);
	return 0;
}

/* Abortive close (SO_LINGER with a zero timeout): a reset, nothing kept.
 * The caller holds a reference. */
void tcp_abort(tcp_conn_t *c)
{
	uint64_t f;

	if (!c)
		return;
	if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == TCP_STATE_LISTEN) {
		tcp_close_listener(c);
		return;
	}
	tcp_lock_acquire(&c->lock, &f);
	c->owner_socket = NULL;
	if (c->state != TCP_STATE_CLOSED) {
		if (c->state != TCP_STATE_SYN_SENT)
			tcp_send_active_reset(c);
		TCPX_INC(TCPX_ABORTONLINGER);
		tcp_purge_queues(c);
		tcp_done(c);
	}
	tcp_lock_release(&c->lock, f);
}

// ---------------------------------------------------------------------------
// Sending.
// ---------------------------------------------------------------------------
/* One caller at a time copies between user memory and a connection's
 * queues with the lock dropped -- per direction (rx_busy, tx_busy): one
 * send()'s data stays together in the stream, two readers never take the
 * same bytes.  Called and returns with conn->lock held (interrupts off);
 * waits with the lock dropped and interrupts on. */
static void tcp_user_begin(tcp_conn_t *c, uint8_t *busy, uint64_t *f)
{
	while (*busy) {
		tcp_lock_release(&c->lock, *f);
		sched_yield_in_kernel();
		tcp_lock_acquire(&c->lock, f);
	}
	*busy = 1;
}

/* Bytes the tail segment of the write queue can still take (it is not
 * full, not sent, not a SYN or FIN).  Under conn->lock. */
static uint32_t tcp_tail_room(tcp_conn_t *c, uint32_t mss_now)
{
	sk_buff_t *tail = skb_peek_tail(&c->write_queue);

	if (!tail || tail->len >= mss_now || skb_cloned(tail) ||
	    (TCP_SKB_CB(tail)->tcp_flags & (TCP_FIN | TCP_SYN)))
		return 0;
	return tcp_min32(mss_now - tail->len, skb_tailroom(tail));
}

/* Put `skb' (payload at data, not charged) at the end of the write queue:
 * as much as fits into the tail segment's room goes there (a small write
 * after a small write makes one segment), the rest as a segment of its
 * own.  Under conn->lock; consumes the skb. */
static void tcp_queue_user_skb(tcp_conn_t *c, sk_buff_t *skb, uint32_t mss_now)
{
	sk_buff_t *tail = skb_peek_tail(&c->write_queue);
	uint32_t room = tcp_tail_room(c, mss_now);

	if (room) {
		uint32_t k = tcp_min32(room, skb->len);

		if (k) {
			mm_memcpy(skb_append(tail, k), skb->data, k);
			TCP_SKB_CB(tail)->end_seq += k;
			c->write_seq += k;
			skb_pull(skb, k);
		}
	}
	if (!skb->len) {
		skb_put(skb);
		return;
	}
	/* New data after an idle period restarts the window (RFC 5681
	 * 4.1). */
	tcp_slow_start_after_idle_check(c);
	TCP_SKB_CB(skb)->seq = c->write_seq;
	TCP_SKB_CB(skb)->end_seq = c->write_seq + skb->len;
	TCP_SKB_CB(skb)->tcp_flags = TCP_ACK;
	c->write_seq += skb->len;
	tcp_wmem_charge(c, skb);
	__skb_queue_tail(&c->write_queue, skb);
}

/* Queue `len' bytes for sending.  Returns what was queued (all of it,
 * unless the send buffer filled), 0 when the buffer is full (tx_ready is
 * then 0 and becomes 1 when ACKs free room), or -errno.  The caller has
 * made the source resident and opened user access.
 *
 * The user memory is copied with interrupts on and no lock held -- into
 * a buffer of its own, one segment's worth at a time, which then goes
 * onto the write queue under the lock.  A fault on the source (a sibling
 * thread unmapped it since the caller made it resident) is then an
 * ordinary fault, not one inside an interrupts-off section. */
int tcp_send_data_urg(tcp_conn_t *c, const uint8_t *data, uint16_t len,
		      int urg)
{
	uint64_t f;
	uint32_t copied = 0, mss_now = 0;
	int err = 0, first = 1;

	if (!c)
		return -EPIPE;
	if (len == 0)
		return 0;
	tcp_lock_acquire(&c->lock, &f);
	tcp_user_begin(c, &c->tx_busy, &f);
	for (;;) {
		sk_buff_t *skb;
		uint32_t want, room;

		if (c->state != TCP_STATE_ESTABLISHED &&
		    c->state != TCP_STATE_CLOSE_WAIT) {
			/* The socket layer reports a pending error in its
			 * place (once, tcp_sock_error). */
			err = -EPIPE;
			break;
		}
		if (copied >= len)
			break;
		if (first) {
			// is the sender application-limited? (rate samples)
			tcp_rate_check_app_limited(c);
			first = 0;
		}
		mss_now = tcp_current_mss(c);
		/* Room left in the tail segment is filled whatever the
		 * buffer says; a new segment needs send-buffer space. */
		room = tcp_tail_room(c, mss_now);
		if (!room && !tcp_stream_memory_free(c))
			break;
		want = tcp_min32(room ? room : mss_now, len - copied);
		tcp_lock_release(&c->lock, f);
		/* Interrupts on, no lock: the allocation may use the slab,
		 * the copy may fault. */
		skb = skb_alloc_headroom(TCP_MAX_HEADER, want);
		if (skb) {
			mm_memset(skb->cb, 0, sizeof(struct tcp_skb_cb));
			mm_memcpy(skb_append(skb, want), data + copied, want);
		}
		tcp_lock_acquire(&c->lock, &f);
		if (!skb)
			break;
		if (c->state != TCP_STATE_ESTABLISHED &&
		    c->state != TCP_STATE_CLOSE_WAIT) {
			skb_put(skb);
			continue; // the state check above reports it
		}
		mss_now = tcp_current_mss(c);
		tcp_queue_user_skb(c, skb, mss_now);
		copied += want;
		/* A full segment ready: send what the windows allow now,
		 * rather than after the whole write was copied. */
		if (copied < len && tcp_send_head(c) &&
		    skb_peek_tail(&c->write_queue)->len >= mss_now)
			tcp_write_xmit(c, mss_now, c->nonagle, 0);
	}
	if (copied && (c->state == TCP_STATE_ESTABLISHED ||
		       c->state == TCP_STATE_CLOSE_WAIT)) {
		net_socket_t *s = (net_socket_t *)c->owner_socket;
		int more = s && (void *)s != (void *)c &&
			   __atomic_load_n(&s->tx_more, __ATOMIC_ACQUIRE);

		if (urg) {
			c->snd_up = c->write_seq;
			c->snd_urg_pending = 1;
		}
		tcp_push(c, c->nonagle, more && !urg);
	}
	if (!tcp_stream_memory_free(c) || !tcp_stream_is_writeable(c))
		c->tx_ready = 0;
	else if (!c->tx_ready)
		c->tx_ready = 1;
	if (copied && !c->tx_ready && tcp_stream_is_writeable(c))
		c->tx_ready = 1;
	c->tx_busy = 0;
	tcp_lock_release(&c->lock, f);
	if (copied)
		return (int)copied;
	return err;
}

int tcp_send_data(tcp_conn_t *c, const uint8_t *data, uint16_t len)
{
	return tcp_send_data_urg(c, data, len, 0);
}

int tcp_send_oob(tcp_conn_t *c, uint8_t byte)
{
	return tcp_send_data_urg(c, &byte, 1, 1);
}

/* SO_MAX_PACING_RATE.  The rate is capped at once; any value but "no
 * cap" also turns TCP's own pacing on for the connection (tcp.h). */
void tcp_set_max_pacing_rate(tcp_conn_t *c, uint64_t rate)
{
	uint64_t f;

	if (!c)
		return;
	tcp_lock_acquire(&c->lock, &f);
	c->max_pacing_rate = rate;
	if (rate != ~0ULL)
		c->pacing_on = 1;
	if (c->pacing_rate > rate)
		c->pacing_rate = rate ? rate : 1;
	tcp_lock_release(&c->lock, f);
}

/* TCP_CORK on or off.  Off releases what the cork held back. */
void tcp_set_cork(tcp_conn_t *c, int on)
{
	uint64_t f;

	if (!c)
		return;
	tcp_lock_acquire(&c->lock, &f);
	c->cork = on ? 1 : 0;
	if (on) {
		c->nonagle |= TCP_NAGLE_CORK;
	} else {
		c->nonagle &= (uint8_t)~TCP_NAGLE_CORK;
		c->cork_deadline = 0;
		if (c->state == TCP_STATE_ESTABLISHED ||
		    c->state == TCP_STATE_CLOSE_WAIT) {
			/* What the cork held goes now; with Nagle on, only
			 * what Nagle lets through. */
			if (c->nonagle & TCP_NAGLE_OFF)
				c->nonagle |= TCP_NAGLE_PUSH;
			tcp_push_pending_frames(c);
			c->nonagle &= (uint8_t)~TCP_NAGLE_PUSH;
		}
		tcp_update_tx_ready(c);
	}
	tcp_lock_release(&c->lock, f);
}

// ---------------------------------------------------------------------------
// Receiving.
// ---------------------------------------------------------------------------
/* Received data to user memory.  Under the lock the segments to copy
 * are chosen and pinned (a reference each) -- up to the urgent mark, past
 * an out-of-band urgent byte; the copy runs with the lock dropped and
 * interrupts on (the receive path may append meanwhile, a collapse may
 * replace the pinned segments on the queue: their data stays valid and
 * unchanged); under the lock again the read is committed by sequence
 * number.  rx_busy keeps a second reader out for the while. */
#define TCP_RECV_BATCH 16
int tcp_recv_copy(tcp_conn_t *c, void *buf, size_t len, int peek)
{
	uint64_t f;
	size_t copied = 0;
	uint32_t seq;

	if (!c || !len)
		return 0;
	tcp_lock_acquire(&c->lock, &f);
	if (!c->active) {
		tcp_lock_release(&c->lock, f);
		return 0;
	}
	tcp_user_begin(c, &c->rx_busy, &f);
	seq = c->copied_seq;
	while (copied < len) {
		struct {
			sk_buff_t *skb;
			uint32_t off, n;
		} pin[TCP_RECV_BATCH];
		uint32_t npin = 0, start = seq, want = 0, skipped = 0;
		size_t room = len - copied;
		sk_buff_t *skb;

		/* At the urgent mark: a read that got data stops before it;
		 * otherwise an out-of-band byte is stepped over. */
		if (c->urg_state != TCP_URG_NONE && c->rcv_up == seq) {
			if (copied)
				break;
			if (!c->urg_inline &&
			    before(seq, c->copied_seq + c->rcvq_bytes)) {
				seq++;
				skipped = 1;
			}
		}
		if (c->urg_state != TCP_URG_NONE && after(c->rcv_up, seq) &&
		    c->rcv_up - seq < room)
			room = c->rcv_up - seq; // up to the mark
		for (skb = skb_peek(&c->rcv_queue);
		     skb && want < room && npin < TCP_RECV_BATCH;
		     skb = skb->next) {
			struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
			uint32_t off = seq + want - cb->seq, n;

			if (off >= skb->len)
				continue; // read already (a peek walks past)
			n = skb->len - off;
			if (n > room - want)
				n = (uint32_t)(room - want);
			pin[npin].skb = skb_get(skb);
			pin[npin].off = off;
			pin[npin].n = n;
			npin++;
			want += n;
		}
		if (!npin && !skipped)
			break;
		tcp_lock_release(&c->lock, f);
		for (uint32_t i = 0; i < npin; i++) {
			mm_memcpy((uint8_t *)buf + copied,
				  pin[i].skb->data + pin[i].off, pin[i].n);
			copied += pin[i].n;
		}
		tcp_lock_acquire(&c->lock, &f);
		seq += want;
		if (!peek && c->copied_seq == start) {
			uint32_t done = seq - start;
			sk_buff_t *head;

			c->copied_seq = seq;
			c->rcvq_bytes -= tcp_min32(c->rcvq_bytes, done);
			while ((head = skb_peek(&c->rcv_queue)) != NULL &&
			       !before(seq, TCP_SKB_CB(head)->seq + head->len)) {
				__skb_unlink(head, &c->rcv_queue);
				tcp_rmem_uncharge(c, head);
				skb_put(head);
			}
			/* Read past the urgent byte: the mark is over
			 * (header prediction resumes with the next
			 * segment's slow-path check). */
			if (c->urg_state != TCP_URG_NONE &&
			    c->urg_state != TCP_URG_NOTYET &&
			    after(seq, c->rcv_up))
				c->urg_state = TCP_URG_NONE;
		} else if (!peek) {
			/* The queue was taken away under us (close) --
			 * what was copied is reported, nothing else. */
			for (uint32_t i = 0; i < npin; i++)
				skb_put(pin[i].skb);
			break;
		}
		for (uint32_t i = 0; i < npin; i++)
			skb_put(pin[i].skb);
	}
	if (!peek && copied) {
		if (!c->rcvq_bytes)
			c->rx_ready = 0;
		tcp_rcv_space_adjust(c);
		tcp_cleanup_rbuf(c, (int)copied);
	} else if (!peek && !c->rcvq_bytes) {
		c->rx_ready = 0;
	}
	c->rx_busy = 0;
	tcp_lock_release(&c->lock, f);
	return (int)copied;
}

int tcp_recv_urg(tcp_conn_t *c, int peek, uint8_t *byte)
{
	uint64_t f;
	int rc;

	if (!c)
		return -ENOTCONN;
	tcp_lock_acquire(&c->lock, &f);
	if (c->urg_inline || c->urg_state == TCP_URG_NONE ||
	    c->urg_state == TCP_URG_READ) {
		rc = -EINVAL;
	} else if (c->urg_state == TCP_URG_VALID) {
		*byte = c->urgent_byte;
		if (!peek)
			c->urg_state = TCP_URG_READ;
		rc = 1;
	} else if (c->state == TCP_STATE_CLOSED ||
		   c->state == TCP_STATE_CLOSE_WAIT ||
		   c->state == TCP_STATE_CLOSING ||
		   c->state == TCP_STATE_LAST_ACK) {
		rc = 0; // it will never come
	} else {
		rc = -EAGAIN; // never waits for it
	}
	tcp_lock_release(&c->lock, f);
	return rc;
}

void tcp_set_oobinline(tcp_conn_t *c, int on)
{
	uint64_t f;

	if (!c)
		return;
	tcp_lock_acquire(&c->lock, &f);
	c->urg_inline = on ? 1 : 0;
	tcp_lock_release(&c->lock, f);
}

uint32_t tcp_inq(tcp_conn_t *c)
{
	if (!c)
		return 0;
	return __atomic_load_n(&c->rcvq_bytes, __ATOMIC_ACQUIRE);
}

// SIOCATMARK: the next byte to read is the urgent byte.
int tcp_at_mark(tcp_conn_t *c)
{
	if (!c)
		return 0;
	return __atomic_load_n(&c->urg_state, __ATOMIC_ACQUIRE) !=
		       TCP_URG_NONE &&
	       __atomic_load_n(&c->rcv_up, __ATOMIC_RELAXED) ==
		       __atomic_load_n(&c->copied_seq, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// Socket options.
// ---------------------------------------------------------------------------
void tcp_set_buffers(tcp_conn_t *c, int sndbuf, int rcvbuf, uint8_t locks)
{
	uint64_t f;

	if (!c)
		return;
	tcp_lock_acquire(&c->lock, &f);
	if (locks & TCP_SNDBUF_LOCK) {
		c->userlocks |= TCP_SNDBUF_LOCK;
		c->sndbuf = sndbuf > TCP_WMEM_MIN ? sndbuf : TCP_WMEM_MIN;
		tcp_update_tx_ready(c);
	}
	if (locks & TCP_RCVBUF_LOCK) {
		c->userlocks |= TCP_RCVBUF_LOCK;
		c->rcvbuf = rcvbuf > TCP_RMEM_MIN ? rcvbuf : TCP_RMEM_MIN;
		/* A smaller buffer than the window offered so far: the
		 * window is not taken back, it shrinks as data arrives. */
		if (c->state != TCP_STATE_LISTEN &&
		    c->state != TCP_STATE_CLOSED) {
			uint32_t full = (uint32_t)tcp_full_space(c);

			if (c->window_clamp > full)
				c->window_clamp = tcp_max32(full,
							    c->advmss);
			c->rcv_ssthresh = tcp_min32(c->rcv_ssthresh,
						    c->window_clamp);
		}
	}
	tcp_lock_release(&c->lock, f);
}

int tcp_setsockopt_int(tcp_conn_t *c, int optname, int val)
{
	uint64_t f;
	int rc = 0;

	if (!c)
		return -ENOTCONN;
	tcp_lock_acquire(&c->lock, &f);
	switch (optname) {
	case TCP_NODELAY:
		c->nodelay = val ? 1 : 0;
		if (val) {
			/* What Nagle held back goes now (a one-time push;
			 * TCP_CORK still holds what it holds). */
			c->nonagle |= TCP_NAGLE_OFF;
			if (c->state == TCP_STATE_ESTABLISHED ||
			    c->state == TCP_STATE_CLOSE_WAIT) {
				c->nonagle |= TCP_NAGLE_PUSH;
				tcp_push_pending_frames(c);
				c->nonagle &= (uint8_t)~TCP_NAGLE_PUSH;
			}
		} else {
			c->nonagle &= (uint8_t)~(TCP_NAGLE_OFF | TCP_NAGLE_PUSH);
		}
		break;
	case TCP_SOCKOPT_KEEPALIVE:
		c->keepalive = val ? 1 : 0;
		tcp_set_keepalive(c, c->keepalive);
		break;
	case TCP_KEEPIDLE:
	case TCP_KEEPINTVL:
	case TCP_KEEPCNT:
		if (val < 1 || val > (optname == TCP_KEEPCNT ? 127 : 32767)) {
			rc = -EINVAL;
			break;
		}
		if (optname == TCP_KEEPIDLE) {
			c->keepidle_ticks = (uint32_t)timer_s_to_ticks((uint64_t)val);
			if (c->keepalive)
				tcp_set_keepalive(c, 1);
		} else if (optname == TCP_KEEPINTVL) {
			c->keepintvl_ticks =
				(uint32_t)timer_s_to_ticks((uint64_t)val);
		} else {
			c->keepcnt = (uint32_t)val;
		}
		break;
	case TCP_MAXSEG:
		if (val && (val < TCP_MIN_MSS || val > 65535 - 40)) {
			rc = -EINVAL;
			break;
		}
		c->user_mss = (uint16_t)val;
		break;
	case TCP_QUICKACK:
		if (val) {
			c->ack_pingpong = 0;
			if (tcp_state_synchronized(c->state) &&
			    (c->ack_pending & TCP_ACK_SCHED)) {
				tcp_enter_quickack_mode(c, TCP_MAX_QUICKACKS);
				tcp_send_ack(c);
			} else {
				tcp_enter_quickack_mode(c, TCP_MAX_QUICKACKS);
			}
		} else {
			c->ack_pingpong = 1;
		}
		break;
	case TCP_USER_TIMEOUT:
		if (val < 0) {
			rc = -EINVAL;
			break;
		}
		c->user_timeout_ms = (uint32_t)val;
		break;
	case TCP_NOTSENT_LOWAT:
		c->notsent_lowat = val > 0 ? (uint32_t)val : 0;
		tcp_update_tx_ready(c);
		break;
	case TCP_DEFER_ACCEPT: {
		/* Seconds -> SYN-ACK retransmissions that cover them (1, 2,
		 * 4, ... s), as the common stack rounds it. */
		uint32_t secs = val > 0 ? (uint32_t)val : 0, n = 0, t = 1;

		while (secs && n < 255) {
			n++;
			if (t >= secs)
				break;
			secs -= t;
			t <<= 1;
		}
		c->defer_accept = n;
		break;
	}
	case TCP_LINGER2:
		if (val < 0)
			c->linger2_ms = -1;
		else if (val == 0)
			c->linger2_ms = 0;
		else
			c->linger2_ms = val > TCP_FIN_TIMEOUT_MS / 1000 ?
						TCP_FIN_TIMEOUT_MS :
						val * 1000;
		break;
	case TCP_SYNCNT:
		if (val < 1 || val > 127)
			rc = -EINVAL;
		else
			c->syn_retries = (uint8_t)val;
		break;
	case TCP_CONGESTION:
		rc = tcp_set_congestion_control(c, val);
		break;
	case TCP_WINDOW_CLAMP:
		if (val < 0) {
			rc = -EINVAL;
			break;
		}
		if (val == 0) {
			if (c->state != TCP_STATE_CLOSED)
				rc = -EINVAL;
			else
				c->window_clamp = 0;
		} else {
			c->window_clamp = tcp_max32((uint32_t)val,
						    TCP_MIN_MSS * 2u);
			c->rcv_ssthresh = tcp_min32(c->rcv_ssthresh,
						    c->window_clamp);
		}
		break;
	default:
		rc = -ENOPROTOOPT;
		break;
	}
	tcp_lock_release(&c->lock, f);
	return rc;
}

int tcp_getsockopt_int(tcp_conn_t *c, int optname, int *val)
{
	if (!c)
		return -ENOTCONN;
	switch (optname) {
	case TCP_MAXSEG:
		if (c->state == TCP_STATE_LISTEN ||
		    c->state == TCP_STATE_CLOSED || !c->mss_cache)
			*val = c->user_mss ? c->user_mss : TCP_MSS_DEFAULT;
		else
			*val = c->mss_cache;
		return 0;
	case TCP_QUICKACK:
		*val = c->ack_quick && !c->ack_pingpong;
		return 0;
	case TCP_USER_TIMEOUT:
		*val = (int)c->user_timeout_ms;
		return 0;
	case TCP_NOTSENT_LOWAT:
		*val = (int)c->notsent_lowat;
		return 0;
	case TCP_DEFER_ACCEPT: {
		uint32_t secs = 0, t = 1;

		for (uint32_t i = 0; i < c->defer_accept; i++) {
			secs += t;
			t <<= 1;
		}
		*val = (int)secs;
		return 0;
	}
	case TCP_LINGER2:
		*val = c->linger2_ms < 0 ? -1 :
		       c->linger2_ms	 ? c->linger2_ms / 1000 :
					   TCP_FIN_TIMEOUT_MS / 1000;
		return 0;
	case TCP_SYNCNT:
		*val = c->syn_retries ? c->syn_retries : TCP_SYN_RETRIES;
		return 0;
	case TCP_WINDOW_CLAMP:
		*val = (int)c->window_clamp;
		return 0;
	case TCP_CONGESTION:
		*val = tcp_ca_id(c);
		return 0;
	default:
		return -ENOPROTOOPT;
	}
}

// ---------------------------------------------------------------------------
// TCP_INFO.
// ---------------------------------------------------------------------------
/* tcpi_state in the ABI's numbering (netinet/tcp.h: ESTABLISHED 1 ...
 * CLOSING 11), not TCP_STATE_*. */
static uint8_t tcp_info_state(int st)
{
	switch (st) {
	case TCP_STATE_ESTABLISHED:
		return 1;
	case TCP_STATE_SYN_SENT:
		return 2;
	case TCP_STATE_SYN_RECEIVED:
		return 3;
	case TCP_STATE_FIN_WAIT_1:
		return 4;
	case TCP_STATE_FIN_WAIT_2:
		return 5;
	case TCP_STATE_TIME_WAIT:
		return 6;
	case TCP_STATE_CLOSE_WAIT:
		return 8;
	case TCP_STATE_LAST_ACK:
		return 9;
	case TCP_STATE_LISTEN:
		return 10;
	case TCP_STATE_CLOSING:
		return 11;
	default:
		return 7; // CLOSE
	}
}

void tcp_fill_info(tcp_conn_t *c, struct tcp_info *info)
{
	uint64_t f, now;

	if (!c || !info)
		return;
	mm_memset(info, 0, sizeof(*info));
	tcp_lock_acquire(&c->lock, &f);
	now = tcp_now();
	info->tcpi_state = tcp_info_state(c->state);
	info->tcpi_ca_state = c->ca_state;
	info->tcpi_retransmits = c->retransmits;
	info->tcpi_probes = c->probes_out;
	info->tcpi_backoff = c->rto_backoff;
	if (c->ts_enabled)
		info->tcpi_options |= TCPI_OPT_TIMESTAMPS;
	if (c->sack_ok)
		info->tcpi_options |= TCPI_OPT_SACK;
	if (c->ws_enabled)
		info->tcpi_options |= TCPI_OPT_WSCALE;
	if (c->ecn_flags & TCP_ECN_OK)
		info->tcpi_options |= TCPI_OPT_ECN;
	if (c->ecn_flags & TCP_ECN_SEEN)
		info->tcpi_options |= TCPI_OPT_ECN_SEEN;
	info->tcpi_snd_wscale_rcv_wscale =
		(uint8_t)((c->snd_wscale & 0x0F) | ((c->rcv_wscale & 0x0F) << 4));
	info->tcpi_pad0 = c->rate_app_limited ? 1 : 0;
	{
		// the RTO with its backoff
		uint64_t rto = c->rto_us ? c->rto_us : TCP_RTO_INITIAL_US;

		rto <<= c->rto_backoff > 16 ? 16 : c->rto_backoff;
		info->tcpi_rto = (uint32_t)(rto > TCP_RTO_MAX_US ? TCP_RTO_MAX_US :
								  rto);
	}
	info->tcpi_ato = (uint32_t)(tcp_ticks_to_ms(c->ack_ato) * 1000u);
	info->tcpi_snd_mss = c->mss_cache;
	info->tcpi_rcv_mss = c->rcv_mss;
	info->tcpi_unacked = c->packets_out;
	info->tcpi_sacked = c->sacked_out;
	info->tcpi_lost = c->lost_out;
	info->tcpi_retrans = c->retrans_out;
	info->tcpi_last_data_sent =
		c->lsndtime ? (uint32_t)tcp_ticks_to_ms(now - c->lsndtime) : 0;
	info->tcpi_last_data_recv =
		c->ack_lrcvtime ?
			(uint32_t)tcp_ticks_to_ms(now - c->ack_lrcvtime) :
			0;
	info->tcpi_last_ack_recv =
		(uint32_t)tcp_ticks_to_ms(now - c->last_rx_tick);
	info->tcpi_pmtu = c->pmtu_cookie;
	info->tcpi_rcv_ssthresh = c->rcv_ssthresh;
	info->tcpi_rtt = c->srtt_us;
	info->tcpi_rttvar = c->rttvar_us;
	info->tcpi_snd_ssthresh = c->ssthresh;
	info->tcpi_snd_cwnd = c->cwnd;
	info->tcpi_advmss = c->advmss;
	info->tcpi_reordering = c->reordering;
	info->tcpi_rcv_rtt = c->rcv_rtt_est.rtt_us >> 3;
	info->tcpi_rcv_space = c->rcvq_space.space;
	info->tcpi_total_retrans = c->total_retrans;
	info->tcpi_pacing_rate = c->pacing_rate ? c->pacing_rate : ~0ULL;
	info->tcpi_max_pacing_rate = c->max_pacing_rate;
	info->tcpi_bytes_acked = c->bytes_acked;
	info->tcpi_bytes_received = c->bytes_received;
	info->tcpi_segs_out = c->segs_out;
	info->tcpi_segs_in = c->segs_in;
	info->tcpi_notsent_bytes =
		after(c->write_seq, c->snd_nxt) ? c->write_seq - c->snd_nxt : 0;
	info->tcpi_min_rtt = tcp_min_rtt(c);
	info->tcpi_data_segs_in = c->data_segs_in;
	info->tcpi_data_segs_out = c->data_segs_out;
	info->tcpi_delivery_rate = tcp_delivery_rate(c);
	info->tcpi_delivered = c->delivered;
	info->tcpi_delivered_ce = c->delivered_ce;
	info->tcpi_bytes_sent = c->bytes_sent;
	info->tcpi_bytes_retrans = c->bytes_retrans;
	info->tcpi_dsack_dups = c->dsack_dups;
	info->tcpi_reord_seen = c->reord_seen;
	info->tcpi_rcv_ooopack = c->rcv_ooopack;
	info->tcpi_snd_wnd = c->snd_wnd;
	info->tcpi_rcv_wnd = c->rcv_wnd;
	info->tcpi_total_rto = c->total_rto;
	info->tcpi_total_rto_recoveries = c->total_rto_recoveries;
	info->tcpi_total_rto_time = c->total_rto_time +
				    (c->rto_stamp ? (uint32_t)tcp_ticks_to_ms(
							    now - c->rto_stamp) :
						    0);
	tcp_lock_release(&c->lock, f);
}

// ---------------------------------------------------------------------------
// ICMP and path MTU.
// ---------------------------------------------------------------------------
/* An ICMP error quoting one of our segments (RFC 1122 4.2.3.9, RFC 5927).
 * An error quoting something we never sent -- outside snd_una..snd_nxt --
 * is ignored.  During the handshake every error is final; on a
 * synchronized connection none is: it becomes the soft error, which
 * SO_ERROR reports and a retransmission timeout fails the connection
 * with. */
void tcp_icmp_error(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
		    uint16_t remote_port, uint32_t seq, int err)
{
	struct tcp_skc *skc = tcp_lookup_established(local_ip, local_port,
						     remote_ip, remote_port);
	uint64_t f;

	if (!skc)
		return;
	if (skc->skc_kind == TCP_SKC_REQ) {
		struct tcp_request_sock *req = (struct tcp_request_sock *)skc;
		tcp_conn_t *l = req->listener;

		/* A half-open connection nobody holds yet: forget it when
		 * the error quotes its SYN-ACK. */
		tcp_lock_acquire(&l->lock, &f);
		if (seq == req->iss && !req->dead && !req->completing)
			tcp_reqsk_drop(req);
		tcp_lock_release(&l->lock, f);
		tcp_skc_put(skc);
		return;
	}
	if (skc->skc_kind != TCP_SKC_FULL) {
		tcp_skc_put(skc);
		return;
	}
	tcp_conn_t *c = (tcp_conn_t *)skc;

	tcp_lock_acquire(&c->lock, &f);
	if (!tcp_seq_between(seq, c->snd_una, c->snd_nxt)) {
		NET_STATS_INC(NET_MIB_TCP_OUTOFWINDOWICMPS);
		goto out;
	}
	switch (c->state) {
	case TCP_STATE_SYN_SENT:
	case TCP_STATE_SYN_RECEIVED:
		NET_STATS_INC(NET_MIB_TCP_ATTEMPTFAILS);
		tcp_reset(c, err);
		break;
	case TCP_STATE_ESTABLISHED:
	case TCP_STATE_FIN_WAIT_1:
	case TCP_STATE_FIN_WAIT_2:
	case TCP_STATE_CLOSE_WAIT:
	case TCP_STATE_CLOSING:
	case TCP_STATE_LAST_ACK:
		c->soft_error = err;
		break;
	default:
		break;
	}
out:
	tcp_lock_release(&c->lock, f);
	tcp_conn_put(c);
}

/* RFC 1191 / RFC 5927: "fragmentation needed" quoting segment `seq' of
 * a connection, the next hop's MTU `info'.  Believed only when the quoted
 * sequence is one we have outstanding (snd_una..snd_nxt) -- a forged
 * report for some other sequence must not shrink the path; then the
 * route's path MTU is lowered (the IP layer), and the connection sizes its
 * segments for it and resends what was dropped.  Softirq context, no lock
 * held. */
void tcp_v4_mtu_report(uint32_t local_ip, uint16_t local_port,
		       uint32_t remote_ip, uint16_t remote_port, uint32_t seq,
		       uint32_t info)
{
	struct tcp_skc *skc = tcp_lookup_established(local_ip, local_port,
						     remote_ip, remote_port);
	tcp_conn_t *c;
	uint32_t pmtu;
	uint64_t f;
	int locked, ok;

	if (!skc)
		return;
	if (skc->skc_kind != TCP_SKC_FULL) {
		tcp_skc_put(skc);
		return;
	}
	c = (tcp_conn_t *)skc;
	tcp_lock_acquire(&c->lock, &f);
	ok = c->state != TCP_STATE_LISTEN && c->state != TCP_STATE_CLOSED &&
	     tcp_seq_between(seq, c->snd_una, c->snd_nxt);
	if (!ok && c->state != TCP_STATE_LISTEN && c->state != TCP_STATE_CLOSED)
		NET_STATS_INC(NET_MIB_TCP_OUTOFWINDOWICMPS);
	tcp_lock_release(&c->lock, f);
	/* The route's exception is made without the socket lock (the IP
	 * layer's locks are not leaves of it). */
	if (ok) {
		pmtu = ip_update_pmtu(remote_ip, info, &locked);
		if (pmtu) {
			tcp_lock_acquire(&c->lock, &f);
			tcp_mtu_reduced(c, pmtu);
			tcp_lock_release(&c->lock, f);
		}
	}
	tcp_conn_put(c);
}

// ---------------------------------------------------------------------------
// Diagnostics.
// ---------------------------------------------------------------------------
uint32_t tcp_full_sock_count(void)
{
	return __atomic_load_n(&g_conn_count, __ATOMIC_RELAXED);
}

void tcp_dump_table(struct tty *tty)
{
	extern void tcp_ipv4_dump(struct tty * tty);
	static const struct {
		const char *name;
		enum net_mib_idx idx;
	} mib_names[] = {
		{ "InSegs", NET_MIB_TCP_INSEGS },
		{ "OutSegs", NET_MIB_TCP_OUTSEGS },
		{ "RetransSegs", NET_MIB_TCP_RETRANSSEGS },
		{ "OutRsts", NET_MIB_TCP_OUTRSTS },
		{ "InErrs", NET_MIB_TCP_INERRS },
		{ "ActiveOpens", NET_MIB_TCP_ACTIVEOPENS },
		{ "PassiveOpens", NET_MIB_TCP_PASSIVEOPENS },
		{ "AttemptFails", NET_MIB_TCP_ATTEMPTFAILS },
		{ "EstabResets", NET_MIB_TCP_ESTABRESETS },
		{ "PAWSDrop", NET_MIB_TCP_PAWSDROP },
		{ "ChallengeAck", NET_MIB_TCP_CHALLENGEACK },
		{ "OOOQueueFull", NET_MIB_TCP_OOOQUEUEFULL },
		{ "AcceptQFull", NET_MIB_TCP_ACCEPTQFULL },
		{ "RstDataLoss", NET_MIB_TCP_RSTDATALOSS },
		{ "ConnTableFull", NET_MIB_TCP_CONNTABLEFULL },
		{ "PersistProbes", NET_MIB_TCP_PERSISTPROBES },
		{ "UdpNoPorts", NET_MIB_UDP_NOPORTS },
		{ "UdpRcvBufErr", NET_MIB_UDP_RCVBUFERRORS },
	};

	tty_printf(tty, "=== TCP table ===\n");
	tcp_ipv4_dump(tty);
	tty_printf(tty, "--- tcp: %u sockets, %u orphans, %u TIME_WAIT, mem %llu%s ---\n",
		   (unsigned)__atomic_load_n(&g_conn_count, __ATOMIC_RELAXED),
		   (unsigned)tcp_orphan_count(), (unsigned)tcp_tw_count(),
		   (unsigned long long)tcp_mem_allocated(),
		   tcp_under_memory_pressure() ? " (pressure)" : "");
	tty_printf(tty, "--- net counters (nonzero) ---\n");
	for (unsigned i = 0; i < sizeof(mib_names) / sizeof(mib_names[0]); i++) {
		uint64_t v = net_stats_read(mib_names[i].idx);

		if (v)
			tty_printf(tty, "  %s=%llu\n", mib_names[i].name,
				   (unsigned long long)v);
	}
	for (int i = 0; i < TCPX_MAX; i++) {
		uint64_t v = __atomic_load_n(&tcp_xmib[i], __ATOMIC_RELAXED);

		if (v && tcp_xmib_names[i])
			tty_printf(tty, "  %s=%llu\n", tcp_xmib_names[i],
				   (unsigned long long)v);
	}
	{
		uint64_t skb_fail = skb_get_alloc_failures();

		if (skb_fail)
			tty_printf(tty, "  SkbAllocFail=%llu\n",
				   (unsigned long long)skb_fail);
	}
	tty_printf(tty, "=================\n");
}

void tcp_init(void)
{
	tcp_hash_init();
	net_timer_setup(&g_tcp_reclaim_timer, tcp_mem_reclaim_timer_fn);
	tcp_minisocks_init();
	tcp_tsq_init();
	softirq_register(SOFTIRQ_TIMER, tcp_pending_softirq);
}
