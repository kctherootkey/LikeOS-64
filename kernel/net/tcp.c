// LikeOS-64 TCP (Transmission Control Protocol)
#include <kernel/net/net.h>
#include <kernel/io/console.h>
#include <kernel/io/tty.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h>
#include <kernel/dev/rand/random.h>
#include <kernel/net/softirq.h>
#include <kernel/net/ratelimit.h>
#include <kernel/uapi/bug.h>

// TCP connection table
tcp_conn_t tcp_connections[TCP_MAX_CONNECTIONS];
static spinlock_t tcp_lock = SPINLOCK_INIT("tcp");

// Forward decl: wake any task sleeping in poll/select/epoll_wait immediately
// when TCP socket data (or connection state change) becomes available.
extern void poll_notify_io_ready(void);

// ---------------------------------------------------------------------------
// IRQ-friendly blocking acquire of a per-connection spinlock.
//
// Plain spin_lock_irqsave() spins with IRQs disabled.  Under heavy SMP
// contention (e.g. ksoftirqd on CPU 0 holding conn->lock to deliver a
// burst of TCP segments while another CPU's user task tries to recv from
// the same conn) the waiter can sit IRQs-off long enough to miss a TLB
// shootdown IPI from a third CPU doing slab_free() — the symptom is
// `SMP: TLB shootdown sync timeout (ack=N expect=N+1)` followed by an
// OS-wide multi-second freeze.
//
// Mirrors the trylock-with-IRQ-window pattern in smp_tlb_shootdown_sync():
// while the lock is contended, IRQs are enabled briefly between attempts
// so this CPU can ACK any pending IPIs.  Once acquired, IRQs are disabled
// (matching spin_lock_irqsave semantics).  Safe to use from any process
// or softirq context — DO NOT use from hard-IRQ context, where you
// must already be using spin_trylock-and-skip (see tcp_timer_tick).
static inline void tcp_lock_acquire(spinlock_t *lock, uint64_t *flags_out)
{
	uint64_t f = local_irq_save();
	while (!spin_trylock(lock)) {
		local_irq_restore(f);
		__asm__ volatile("pause" ::: "memory");
		f = local_irq_save();
	}
	*flags_out = f;
}

static inline void tcp_lock_release(spinlock_t *lock, uint64_t flags)
{
	spin_unlock(lock);
	local_irq_restore(flags);
}

// Deferred-free queue for tcp_timer_tick.
//
// tcp_timer_tick runs in IRQ context (100Hz timer vector) on EVERY CPU
// with IRQs disabled.  If it called tcp_free_conn directly, slab_free
// could trigger an SMP TLB shootdown IPI that waits for ACKs from all
// CPUs — but those CPUs may simultaneously be running their own IRQ
// timer handler (IRQs off), unable to ACK, so the shootdown sync times
// out and the kernel logs "TLB shootdown sync timeout (ack=N expect=N+1)".
//
// To avoid this, IRQ-context callers (only tcp_timer_tick) push the
// to-be-freed conn pointer onto this small queue.  The queue is then
// drained from softirq / process context (where IRQs are enabled around
// handler invocations on at least one CPU, so IPIs can be serviced) by
// tcp_reap_pending(), which calls tcp_free_conn → slab_free safely.
static tcp_conn_t *tcp_pending_free[TCP_MAX_CONNECTIONS];
/* Generation of each queued conn, captured at defer time.  The drain frees
 * gen-validated: an entry that is dequeued-but-not-yet-freed can be re-deferred
 * by the reaper (the idempotency scan sees it already gone from the queue),
 * then the first free releases the slot, tcp_alloc_conn recycles it into a NEW
 * live conn, and the stale re-queued entry would free THAT live conn.  gen
 * mismatch makes the stale free a no-op. */
static uint32_t tcp_pending_free_gen[TCP_MAX_CONNECTIONS];
static uint32_t tcp_pending_free_count = 0;
static spinlock_t tcp_pending_free_lock = SPINLOCK_INIT("tcp_pf");
static void tcp_free_conn_gen(tcp_conn_t *conn, uint32_t gen); // forward

// Push a conn onto the deferred-free queue.  IRQ-safe (uses spinlock; the
// critical section is just an array append so it is bounded and very
// short — does NOT call slab).
//
// `gen` is the conn's generation captured by the caller UNDER conn->lock
// while it decided the conn is dead.  It must NOT be read here: this runs
// after the caller dropped conn->lock, and in that window the slot can be
// freed by another CPU (a concurrent timer tick deferring the same CLOSED
// conn with the correct gen + an immediate ksoftirqd drain) and recycled
// into a brand-new LIVE conn.  Reading conn->gen here would then queue the
// NEW generation, and the next drain would gen-"validate" and free that
// live connection out from under its socket (observed: freshly-accepted
// TLS server conn freed mid-handshake — client's 64 KB burst hits the
// no-conn RST path while its own end is still ESTABLISHED).
static void tcp_defer_free(tcp_conn_t *conn, uint32_t gen)
{
	uint64_t flags;
	spin_lock_irqsave(&tcp_pending_free_lock, &flags);
	WARN_ON(tcp_pending_free_count >=
		TCP_MAX_CONNECTIONS); /* deferred-free queue overflow: more items than slots exist */
	// Idempotent: avoid pushing the same conn twice (double-free risk).
	int already = 0;
	for (uint32_t i = 0; i < tcp_pending_free_count; i++) {
		if (tcp_pending_free[i] == conn) {
			already = 1;
			break;
		}
	}
	if (!already && tcp_pending_free_count < TCP_MAX_CONNECTIONS) {
		/* Capture identity so a slot recycled between defer and drain is
		 * not clobbered (see tcp_pending_free_gen). */
		tcp_pending_free_gen[tcp_pending_free_count] = gen;
		tcp_pending_free[tcp_pending_free_count++] = conn;
	}
	spin_unlock_irqrestore(&tcp_pending_free_lock, flags);
	// Wake ksoftirqd to drain the queue in process context.  Without
	// this, a workload that closes a burst of sockets and then idles
	// (no further connect/listen/close to trigger an opportunistic
	// drain) leaves freed-but-not-released slots on the queue forever
	// — they appear in netstat as stuck CLOSED entries and eventually
	// exhaust TCP_MAX_CONNECTIONS.  softirq_raise is IRQ-safe and only
	// sets a per-CPU bit + wakes ksoftirqd if needed.
	softirq_raise(SOFTIRQ_TIMER);
}

// Softirq handler bound to SOFTIRQ_TIMER (registered in tcp_init).
// Runs in process / ksoftirqd context with IRQs enabled, so calling
// tcp_free_conn → slab_free here is safe (TLB-shootdown IPIs can be
// serviced).
static void tcp_pending_softirq(void)
{
	tcp_reap_pending();
}

// Drain the deferred-free queue.  MUST be called only from process
// context (NOT from any IRQ handler, NOT from softirq_drain that was
// entered from an IRQ tail).  tcp_free_conn → slab_free can issue a TLB
// shootdown IPI that needs other CPUs to have IRQs enabled.
void tcp_reap_pending(void)
{
	for (;;) {
		tcp_conn_t *conn = NULL;
		uint32_t gen = 0;
		uint64_t flags;
		spin_lock_irqsave(&tcp_pending_free_lock, &flags);
		if (tcp_pending_free_count > 0) {
			conn = tcp_pending_free[--tcp_pending_free_count];
			gen = tcp_pending_free_gen[tcp_pending_free_count];
		}
		spin_unlock_irqrestore(&tcp_pending_free_lock, flags);
		if (!conn)
			break;
		/* Gen-validated: refuses to free a slot recycled into a live conn
		 * since this entry was queued (the dequeue-then-re-defer race). */
		tcp_free_conn_gen(conn, gen); // safe outside the pending-free lock
	}
}

// Forward declarations needed by helpers defined before their original sites.
static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size);
static uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t size);
int tcp_send_segment(net_device_t *dev, uint32_t src_ip, uint32_t dst_ip,
		     uint16_t src_port, uint16_t dst_port, uint32_t seq,
		     uint32_t ack, uint8_t flags, uint16_t window,
		     const uint8_t *data, uint16_t data_len);
static void tcp_send_ack(tcp_conn_t *conn);

// ISN secret key (generated once at init, 128-bit for SipHash-2-4)
static uint8_t tcp_isn_secret[16];

// SYN cookie secret (separate from ISN secret)
static uint8_t tcp_syncookie_secret[16];

static uint16_t tcp_local_mss(net_device_t *dev)
{
	uint16_t mtu = (dev && dev->mtu >= sizeof(ipv4_header_t) +
						   sizeof(tcp_header_t)) ?
			       dev->mtu :
			       NET_MTU_DEFAULT;
	uint16_t mss =
		(uint16_t)(mtu - sizeof(ipv4_header_t) - sizeof(tcp_header_t));
	if (mss > TCP_MSS)
		mss = TCP_MSS;
	if (mss < 536)
		mss = 536;
	return mss;
}

static uint16_t tcp_effective_mss(tcp_conn_t *conn)
{
	uint16_t mss = conn->max_seg_size ? conn->max_seg_size : TCP_MSS;
	if (mss > TCP_MSS)
		mss = TCP_MSS;
	if (mss < 536)
		mss = 536;
	return mss;
}

// ============================================================================
// RFC 7323 / RFC 2018 — Unified TCP option parser
// ============================================================================
typedef struct {
	uint16_t mss; // 0 if absent
	int8_t wscale; // -1 if absent (else 0..14, RFC 7323 caps at 14)
	uint8_t sack_perm; // 1 if SACK Permitted seen
	uint8_t ts_present; // 1 if Timestamp option seen
	uint32_t tsval;
	uint32_t tsecr;
	uint8_t sack_count; // number of (left,right) pairs found
	struct {
		uint32_t left, right;
	} sack[TCP_MAX_SACK_BLOCKS];
} tcp_parsed_opts_t;

static void tcp_parse_options(const tcp_header_t *tcp, uint8_t data_offset,
			      tcp_parsed_opts_t *out)
{
	out->mss = 0;
	out->wscale = -1;
	out->sack_perm = 0;
	out->ts_present = 0;
	out->tsval = 0;
	out->tsecr = 0;
	out->sack_count = 0;

	if (data_offset <= sizeof(tcp_header_t))
		return;
	const uint8_t *opts = ((const uint8_t *)tcp) + sizeof(tcp_header_t);
	uint8_t opt_len = (uint8_t)(data_offset - sizeof(tcp_header_t));
	uint8_t i = 0;
	while (i < opt_len) {
		uint8_t k = opts[i];
		if (k == TCP_OPT_END)
			break;
		if (k == TCP_OPT_NOP) {
			i++;
			continue;
		}
		if (i + 1 >= opt_len)
			break;
		uint8_t l = opts[i + 1];
		if (l < 2 || i + l > opt_len)
			break;
		switch (k) {
		case TCP_OPT_MSS:
			if (l == 4)
				out->mss = (uint16_t)((opts[i + 2] << 8) |
						      opts[i + 3]);
			break;
		case TCP_OPT_WSCALE:
			if (l == 3) {
				uint8_t s = opts[i + 2];
				if (s > 14)
					s = 14;
				out->wscale = (int8_t)s;
			}
			break;
		case TCP_OPT_SACK_PERM:
			if (l == 2)
				out->sack_perm = 1;
			break;
		case TCP_OPT_TIMESTAMP:
			if (l == 10) {
				out->ts_present = 1;
				out->tsval = ((uint32_t)opts[i + 2] << 24) |
					     ((uint32_t)opts[i + 3] << 16) |
					     ((uint32_t)opts[i + 4] << 8) |
					     ((uint32_t)opts[i + 5]);
				out->tsecr = ((uint32_t)opts[i + 6] << 24) |
					     ((uint32_t)opts[i + 7] << 16) |
					     ((uint32_t)opts[i + 8] << 8) |
					     ((uint32_t)opts[i + 9]);
			}
			break;
		case TCP_OPT_SACK: {
			// Each block is 8 bytes (left,right). Header is 2 bytes (kind,len).
			uint8_t blocks = (l >= 2) ? (uint8_t)((l - 2) / 8) : 0;
			if (blocks > TCP_MAX_SACK_BLOCKS)
				blocks = TCP_MAX_SACK_BLOCKS;
			for (uint8_t b = 0; b < blocks; b++) {
				uint8_t off = (uint8_t)(i + 2 + b * 8);
				out->sack[b].left =
					((uint32_t)opts[off + 0] << 24) |
					((uint32_t)opts[off + 1] << 16) |
					((uint32_t)opts[off + 2] << 8) |
					((uint32_t)opts[off + 3]);
				out->sack[b].right =
					((uint32_t)opts[off + 4] << 24) |
					((uint32_t)opts[off + 5] << 16) |
					((uint32_t)opts[off + 6] << 8) |
					((uint32_t)opts[off + 7]);
			}
			out->sack_count = blocks;
			break;
		}
		default:
			break;
		}
		i = (uint8_t)(i + l);
	}
}

// Backwards-compat wrapper used by old callers.
static uint16_t tcp_parse_mss_option(const tcp_header_t *tcp,
				     uint8_t data_offset)
{
	tcp_parsed_opts_t p;
	tcp_parse_options(tcp, data_offset, &p);
	return p.mss;
}

// Encode 32-bit big-endian
static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

// "Timestamp" we send: derive from timer_ticks so each tick is one TS unit
// (10ms, well within the RFC 7323 1ms-1s allowed range).
//
// RFC 7323 §5.4 / Appendix A: exposing the raw boot-relative tick counter
// in TSval lets any peer read this host's uptime and assists off-path
// blind-injection attacks that need to predict TSval. Mitigation is a
// random offset added to every emitted TSval; PAWS, RTTM and Karn rely on
// differences only, so the offset is invisible to the protocol.
//
// We use TWO offsets:
//   - a per-boot fallback (used when no conn context is available, e.g.
//     RST, SYN cookie SYN+ACK)
//   - a per-connection offset derived deterministically from the 4-tuple
//     via SipHash with the ISN secret. Per-conn offsets give each flow an
//     independent TS clock origin (best practice; defends against TS-based
//     uptime fingerprinting and cross-flow correlation), and the
//     deterministic 4-tuple derivation keeps SYN-cookie reconstruction
//     possible without storing extra state.
static uint32_t tcp_ts_offset = 0;
static int tcp_ts_offset_set = 0;

static uint32_t tcp_ts_offset_global(void)
{
	if (!tcp_ts_offset_set) {
		tcp_ts_offset = random_u32();
		tcp_ts_offset_set = 1;
	}
	return tcp_ts_offset;
}

// SipHash-2-4(secret, 4-tuple) truncated to 32 bits.  Reuses the ISN secret
// since both serve identical "unpredictable per 4-tuple, stable for the
// lifetime of the boot" requirements.
static uint32_t tcp_compute_ts_offset(uint32_t local_ip, uint32_t remote_ip,
				      uint16_t local_port, uint16_t remote_port)
{
	uint8_t data[12];
	data[0] = (uint8_t)(local_ip >> 24);
	data[1] = (uint8_t)(local_ip >> 16);
	data[2] = (uint8_t)(local_ip >> 8);
	data[3] = (uint8_t)(local_ip);
	data[4] = (uint8_t)(remote_ip >> 24);
	data[5] = (uint8_t)(remote_ip >> 16);
	data[6] = (uint8_t)(remote_ip >> 8);
	data[7] = (uint8_t)(remote_ip);
	data[8] = (uint8_t)(local_port >> 8);
	data[9] = (uint8_t)(local_port);
	data[10] = (uint8_t)(remote_port >> 8);
	data[11] = (uint8_t)(remote_port);
	// Mix in a constant tag so this hash is domain-separated from the ISN.
	uint64_t h = siphash_2_4(tcp_isn_secret, data, 12);
	return (uint32_t)(h ^ 0x54534F46U /* 'TSOF' */);
}

static uint32_t tcp_ts_now_for(const tcp_conn_t *conn)
{
	uint32_t off = (conn && conn->ts_offset) ? conn->ts_offset :
						   tcp_ts_offset_global();
	return (uint32_t)timer_ticks() + off;
}

// Backwards-compatible global accessor for paths with no conn (RST, etc.).
__attribute__((unused)) static uint32_t tcp_ts_now(void)
{
	return (uint32_t)timer_ticks() + tcp_ts_offset_global();
}

// Build TCP option block. Caller passes flags actually being sent.
//   - SYN (no ACK): always offer MSS, WSCALE, SACK_PERM, TS (TSecr=0)
//   - SYN+ACK:      mirror what we negotiated in conn (mss + ws/sack/ts iff peer offered)
//   - non-SYN:      TS (if ts_enabled) + SACK blocks for OOO (if sack_ok)
// Returns option byte count (already padded with NOPs to multiple of 4).
static uint8_t tcp_build_options(tcp_conn_t *conn, uint8_t flags,
				 uint16_t mss_to_advertise, uint8_t *buf)
{
	uint8_t n = 0;
	int is_syn = (flags & TCP_SYN) != 0;
	int is_synack = is_syn && (flags & TCP_ACK);

	// MSS only on SYN / SYN+ACK
	if (is_syn) {
		buf[n++] = TCP_OPT_MSS;
		buf[n++] = TCP_OPT_MSS_LEN;
		buf[n++] = (uint8_t)(mss_to_advertise >> 8);
		buf[n++] = (uint8_t)mss_to_advertise;
	}

	// SACK Permitted on SYN; on SYN+ACK only if peer offered (sack_ok set)
	if (is_syn) {
		if (!is_synack || (conn && conn->sack_ok)) {
			buf[n++] = TCP_OPT_NOP;
			buf[n++] = TCP_OPT_NOP;
			buf[n++] = TCP_OPT_SACK_PERM;
			buf[n++] = TCP_OPT_SACK_PERM_LEN;
		}
	}

	// Window Scale: SYN always offers; SYN+ACK only if peer offered (snd_wscale set via ws_enabled)
	if (is_syn) {
		if (!is_synack || (conn && conn->ws_enabled)) {
			buf[n++] = TCP_OPT_NOP;
			buf[n++] = TCP_OPT_WSCALE;
			buf[n++] = TCP_OPT_WSCALE_LEN;
			// We use rcv_wscale = 7 (128x scale → 8MB max window) by default
			uint8_t my_ws = (conn && conn->rcv_wscale) ?
						conn->rcv_wscale :
						7;
			buf[n++] = my_ws;
		}
	}

	// Timestamps: include on SYN unconditionally; on later segments only if negotiated
	if (is_syn || (conn && conn->ts_enabled)) {
		// Pad to 4-byte boundary first for clean TS layout
		// (RFC 7323 §3 recommends 2 NOPs to align 10-byte TS to 32-bit boundary)
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_TIMESTAMP;
		buf[n++] = TCP_OPT_TIMESTAMP_LEN;
		put_be32(buf + n, tcp_ts_now_for(conn));
		n += 4;
		uint32_t tsecr =
			(conn && conn->ts_enabled) ? conn->ts_recent : 0;
		put_be32(buf + n, tsecr);
		n += 4;
	}

	// SACK blocks on non-SYN when we have OOO data
	if (!is_syn && conn && conn->sack_ok && conn->ooo_count > 0) {
		// Build coalesced blocks from ooo[] (already sorted by seq on insert)
		uint32_t blocks_l[TCP_MAX_SACK_BLOCKS];
		uint32_t blocks_r[TCP_MAX_SACK_BLOCKS];
		uint8_t bn = 0;
		for (uint8_t i2 = 0;
		     i2 < conn->ooo_count && bn < TCP_MAX_SACK_BLOCKS; i2++) {
			uint32_t l = conn->ooo[i2].seq;
			uint32_t r = l + conn->ooo[i2].len;
			if (bn > 0 && blocks_r[bn - 1] == l) {
				blocks_r[bn - 1] = r;
			} else {
				blocks_l[bn] = l;
				blocks_r[bn] = r;
				bn++;
			}
		}
		if (bn > 0 && n + 2 + bn * 8 + 2 <= TCP_MAX_OPTIONS) {
			buf[n++] = TCP_OPT_NOP;
			buf[n++] = TCP_OPT_NOP;
			buf[n++] = TCP_OPT_SACK;
			buf[n++] = (uint8_t)(2 + bn * 8);
			for (uint8_t b = 0; b < bn; b++) {
				put_be32(buf + n, blocks_l[b]);
				n += 4;
				put_be32(buf + n, blocks_r[b]);
				n += 4;
			}
		}
	}

	// Pad to 4-byte boundary with NOPs
	while (n & 3)
		buf[n++] = TCP_OPT_NOP;
	return n;
}

// Window-scaling helper: clamp our advertised receive window so it fits in 16
// bits after the rcv_wscale shift.
static uint16_t tcp_advertised_window(tcp_conn_t *conn)
{
	uint32_t avail =
		ring_free(conn->rx_head, conn->rx_tail, conn->rx_buf_size);
	if (avail == 0)
		return 0;
	// Only apply the receive window scale when window scaling was actually
	// negotiated — mirror the send side, which gates the snd_wscale shift on
	// ws_enabled (see the ESTABLISHED inbound-window handling).  A conn that
	// never negotiated WS keeps the default rcv_wscale=7 from tcp_alloc_conn;
	// the stateless SYN-cookie path in particular leaves rcv_wscale=7 while
	// ws_enabled=0.  Shifting by it would advertise a 128x-too-small window to
	// a peer that reads the field unscaled, throttling that peer to ~1/128 of
	// the real buffer (the receive-side sibling of the tcp_syn_window bug).
	uint32_t shifted =
		conn->ws_enabled ? (avail >> conn->rcv_wscale) : avail;
	if (shifted > 0xFFFFu)
		shifted = 0xFFFFu;
	return (uint16_t)shifted;
}

// Window for SYN+ACK segments.  RFC 7323 §2.2: the window field in a segment
// with the SYN bit set is NEVER scaled — the peer reads it raw (our own
// SYN_SENT handler does exactly that: "SYN window unscaled").  Advertising
// tcp_advertised_window() here shipped the >>rcv_wscale value (131071 >> 7 =
// 1023), so every peer started its transfer against a 1023-byte send window
// instead of 64 KB.  A 4 KB client burst then needed multiple ACK-clocked
// round-trips right at connection start, and one lost/late ACK under stress
// cascaded into RTO backoff and the intermittent teststress recv failures.
// The stateless SYN-cookie path always advertised TCP_WINDOW_SIZE correctly.
static uint16_t tcp_syn_window(tcp_conn_t *conn)
{
	uint32_t avail =
		ring_free(conn->rx_head, conn->rx_tail, conn->rx_buf_size);
	return avail > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)avail;
}

/* Linux-style TCP receive buffer auto-tuning.
 *
 * Throughput is BDP-limited: max_rate = rx_buf_size / RTT.  A fixed
 * 128 KB rx ring caps a 481 ms transcontinental flow at ~280 KB/s.
 * Grow on demand instead of pre-allocating big buffers for every
 * connection: only connections that actually fill their ring (peer is
 * sending faster than we drain) pay the larger memory cost.
 *
 * Doubles the ring size up to TCP_RX_BUF_MAX.  Caller must hold
 * conn->lock.  Returns 1 if the ring was grown, 0 otherwise.
 */
static int tcp_grow_rx_buf(tcp_conn_t *conn)
{
	if (!conn || !conn->rx_buf)
		return 0;
	if (conn->rx_buf_size >= TCP_RX_BUF_MAX)
		return 0;

	uint32_t new_size = conn->rx_buf_size * 2;
	if (new_size > TCP_RX_BUF_MAX)
		new_size = TCP_RX_BUF_MAX;

	uint8_t *new_buf = (uint8_t *)slab_alloc(new_size);
	if (!new_buf)
		return 0; // OOM — keep current buffer

	/* Linearize the ring contents from rx_head..rx_tail into the new
     * buffer starting at offset 0.  Handles the wrap case as two memcpys. */
	uint32_t used = (conn->rx_tail - conn->rx_head + conn->rx_buf_size) %
			conn->rx_buf_size;
	if (used > 0) {
		if (conn->rx_head + used <= conn->rx_buf_size) {
			mm_memcpy(new_buf, conn->rx_buf + conn->rx_head, used);
		} else {
			uint32_t first = conn->rx_buf_size - conn->rx_head;
			mm_memcpy(new_buf, conn->rx_buf + conn->rx_head, first);
			mm_memcpy(new_buf + first, conn->rx_buf, used - first);
		}
	}

	uint8_t *old_buf = conn->rx_buf;
	conn->rx_buf = new_buf;
	conn->rx_buf_size = new_size;
	conn->rx_head = 0;
	conn->rx_tail = used;

	slab_free(old_buf);
	return 1;
}

static int tcp_send_segment_ex(net_device_t *dev, uint32_t src_ip,
			       uint32_t dst_ip, uint16_t src_port,
			       uint16_t dst_port, uint32_t seq, uint32_t ack,
			       uint8_t flags, uint16_t window,
			       const uint8_t *data, uint16_t data_len,
			       const uint8_t *options, uint8_t options_len)
{
	uint8_t padded_options = options_len;
	if (padded_options & 3)
		padded_options = (uint8_t)((padded_options + 3) & ~3U);
	if (padded_options > TCP_MAX_OPTIONS)
		return -1;

	uint16_t tcp_len =
		(uint16_t)(sizeof(tcp_header_t) + padded_options + data_len);
	uint8_t pkt[sizeof(tcp_header_t) + TCP_MAX_OPTIONS + TCP_MSS];
	if (tcp_len > sizeof(pkt))
		return -1;

	tcp_header_t *tcp = (tcp_header_t *)pkt;
	tcp->src_port = net_htons(src_port);
	tcp->dst_port = net_htons(dst_port);
	tcp->seq_num = net_htonl(seq);
	tcp->ack_num = net_htonl(ack);
	tcp->data_offset =
		(uint8_t)(((sizeof(tcp_header_t) + padded_options) / 4) << 4);
	tcp->flags = flags;
	tcp->window = net_htons(window);
	tcp->checksum = 0;
	tcp->urgent_ptr = 0;

	uint8_t *opt_dst = pkt + sizeof(tcp_header_t);
	if (padded_options)
		mm_memset(opt_dst, 0, padded_options);
	if (options_len)
		mm_memcpy(opt_dst, options, options_len);

	if (data_len)
		mm_memcpy(pkt + sizeof(tcp_header_t) + padded_options, data,
			  data_len);

	uint8_t pseudo[12 + sizeof(tcp_header_t) + TCP_MAX_OPTIONS + TCP_MSS];
	pseudo[0] = (src_ip >> 24) & 0xFF;
	pseudo[1] = (src_ip >> 16) & 0xFF;
	pseudo[2] = (src_ip >> 8) & 0xFF;
	pseudo[3] = src_ip & 0xFF;
	pseudo[4] = (dst_ip >> 24) & 0xFF;
	pseudo[5] = (dst_ip >> 16) & 0xFF;
	pseudo[6] = (dst_ip >> 8) & 0xFF;
	pseudo[7] = dst_ip & 0xFF;
	pseudo[8] = 0;
	pseudo[9] = IP_PROTO_TCP;
	pseudo[10] = (tcp_len >> 8) & 0xFF;
	pseudo[11] = tcp_len & 0xFF;
	mm_memcpy(pseudo + 12, pkt, tcp_len);

	tcp->checksum = ipv4_checksum(pseudo, (uint16_t)(12 + tcp_len));

	return ipv4_send(dev, dst_ip, IP_PROTO_TCP, pkt, tcp_len);
}

static int tcp_queue_inflight(tcp_conn_t *conn, uint32_t seq, uint8_t flags,
			      const uint8_t *data, uint16_t len)
{
	if (conn->inflight_count >= TCP_MAX_INFLIGHT) {
		conn->tx_ready = 0;
		return -1;
	}

	tcp_inflight_segment_t *seg = &conn->inflight[conn->inflight_count++];
	WARN_ON(conn->inflight_count > TCP_MAX_INFLIGHT);
	seg->seq = seq;
	seg->len = len;
	seg->flags = flags;
	seg->retransmit_count = 0;
	seg->send_us = timer_get_precise_us();
	for (uint16_t i = 0; i < len; i++)
		seg->data[i] = data[i];
	conn->tx_ready = conn->inflight_count < TCP_MAX_INFLIGHT;
	return 0;
}

static void tcp_drop_first_inflight(tcp_conn_t *conn)
{
	WARN_ON(conn->inflight_count ==
		0); /* dropping from empty inflight queue: ACK accounting is off */
	if (conn->inflight_count == 0)
		return;
	for (uint8_t i = 1; i < conn->inflight_count; i++)
		conn->inflight[i - 1] = conn->inflight[i];
	conn->inflight_count--;
	conn->tx_ready = 1;
}

static void tcp_ack_inflight(tcp_conn_t *conn, uint32_t ack)
{
	while (conn->inflight_count > 0) {
		tcp_inflight_segment_t *seg = &conn->inflight[0];
		uint32_t seg_end =
			seg->seq + seg->len +
			((seg->flags & (TCP_SYN | TCP_FIN)) ? 1U : 0U);

		if (ack >= seg_end) {
			tcp_drop_first_inflight(conn);
			continue;
		}

		if (ack > seg->seq && seg->len > 0 &&
		    !(seg->flags & (TCP_SYN | TCP_FIN))) {
			uint16_t trim = (uint16_t)(ack - seg->seq);
			if (trim > seg->len) {
				WARN_ON_ONCE(
					trim >
					seg->len); /* trim larger than segment length: ACK acknowledged bytes we never sent */
				trim = seg->len;
			}
			for (uint16_t i = trim; i < seg->len; i++)
				seg->data[i - trim] = seg->data[i];
			seg->seq = ack;
			seg->len = (uint16_t)(seg->len - trim);
		}
		break;
	}
	conn->tx_ready = conn->inflight_count < TCP_MAX_INFLIGHT;
}

static int tcp_send_syn_packet(net_device_t *dev, uint32_t src_ip,
			       uint32_t dst_ip, uint16_t src_port,
			       uint16_t dst_port, uint32_t seq, uint32_t ack,
			       uint8_t flags, uint16_t window, tcp_conn_t *conn)
{
	uint8_t options[TCP_MAX_OPTIONS];
	// Pass conn so the SYN's TSval uses the per-connection ts_offset; otherwise
	// tcp_build_options(NULL, ...) falls back to the global offset and the peer
	// saves a ts_recent that does not match the offset our later data segments
	// will use, causing every data segment to be PAWS-rejected (RFC 7323 §5.3).
	uint8_t opt_len =
		tcp_build_options(conn, flags, tcp_local_mss(dev), options);
	return tcp_send_segment_ex(dev, src_ip, dst_ip, src_port, dst_port, seq,
				   ack, flags, window, NULL, 0, options,
				   opt_len);
}

// SYN+ACK from a known conn so we can mirror negotiated options.
static int tcp_send_synack_conn(tcp_conn_t *conn, uint16_t window)
{
	uint8_t options[TCP_MAX_OPTIONS];
	uint8_t opt_len = tcp_build_options(conn, TCP_SYN | TCP_ACK,
					    tcp_local_mss(conn->dev), options);
	return tcp_send_segment_ex(conn->dev, conn->local_ip, conn->remote_ip,
				   conn->local_port, conn->remote_port,
				   conn->iss, conn->rcv_nxt, TCP_SYN | TCP_ACK,
				   window, NULL, 0, options, opt_len);
}

static void tcp_fail_connection(tcp_conn_t *conn, int error)
{
	conn->state = TCP_STATE_CLOSED;
	conn->error = error;
	conn->connect_done = 1;
	conn->rx_ready = 1;
	conn->tx_ready = 1;
	conn->inflight_count = 0;
	/* Stamp the failure time.  last_rx_tick is unused once CLOSED (idle
	 * and keepalive checks are ESTABLISHED-family only), so it doubles as
	 * the grace-period base for the sentinel-owner reaper in
	 * tcp_timer_tick: a conn that was never claimed by accept() becomes
	 * reapable N seconds after failing, not immediately — leaving any
	 * in-flight tcp_accept→sock_accept claim time to attach its socket. */
	conn->last_rx_tick = timer_ticks();
	poll_notify_io_ready();
	/* Wake any sock_recv blocked on this conn's rx_ready channel.  Otherwise
     * a connection that fails (RST, timeout) leaves the reader sleeping
     * forever instead of returning the error. */
	sched_wake_channel((void *)&conn->rx_ready);
}

// RFC 6528: ISN = hash(secret, src_ip, dst_ip, src_port, dst_port) + time_counter
// Time counter advances ~64K per second
static uint32_t tcp_generate_isn(uint32_t src_ip, uint32_t dst_ip,
				 uint16_t src_port, uint16_t dst_port)
{
	uint8_t data[12];
	data[0] = (uint8_t)(src_ip >> 24);
	data[1] = (uint8_t)(src_ip >> 16);
	data[2] = (uint8_t)(src_ip >> 8);
	data[3] = (uint8_t)(src_ip);
	data[4] = (uint8_t)(dst_ip >> 24);
	data[5] = (uint8_t)(dst_ip >> 16);
	data[6] = (uint8_t)(dst_ip >> 8);
	data[7] = (uint8_t)(dst_ip);
	data[8] = (uint8_t)(src_port >> 8);
	data[9] = (uint8_t)(src_port);
	data[10] = (uint8_t)(dst_port >> 8);
	data[11] = (uint8_t)(dst_port);

	uint64_t hash = siphash_2_4(tcp_isn_secret, data, 12);

	// Time component: advance ~64K per second (timer is 100Hz, so ticks/4 * 256)
	uint32_t time_comp = (uint32_t)((timer_ticks() / 4) << 8);

	return (uint32_t)hash + time_comp;
}

// ============================================================================
// SYN Cookies - RFC 4987
// ============================================================================
// Encode MSS option as 3-bit index
static const uint16_t syncookie_mss_table[8] = { 536,  1024, 1460, 1480,
						 4312, 8960, 1440, 1452 };

static int mss_to_index(uint16_t mss)
{
	int best = 0;
	for (int i = 0; i < 8; i++) {
		if (syncookie_mss_table[i] <= mss &&
		    syncookie_mss_table[i] >= syncookie_mss_table[best])
			best = i;
	}
	return best;
}

static uint32_t tcp_syncookie_generate(uint32_t src_ip, uint32_t dst_ip,
				       uint16_t src_port, uint16_t dst_port,
				       uint32_t seq, uint16_t mss)
{
	(void)seq;
	uint8_t data[16];
	uint32_t time_slot = (uint32_t)(timer_ticks() / 600); // ~6 second slots

	data[0] = (uint8_t)(src_ip >> 24);
	data[1] = (uint8_t)(src_ip >> 16);
	data[2] = (uint8_t)(src_ip >> 8);
	data[3] = (uint8_t)(src_ip);
	data[4] = (uint8_t)(dst_ip >> 24);
	data[5] = (uint8_t)(dst_ip >> 16);
	data[6] = (uint8_t)(dst_ip >> 8);
	data[7] = (uint8_t)(dst_ip);
	data[8] = (uint8_t)(src_port >> 8);
	data[9] = (uint8_t)(src_port);
	data[10] = (uint8_t)(dst_port >> 8);
	data[11] = (uint8_t)(dst_port);
	data[12] = (uint8_t)(time_slot >> 24);
	data[13] = (uint8_t)(time_slot >> 16);
	data[14] = (uint8_t)(time_slot >> 8);
	data[15] = (uint8_t)(time_slot);

	uint64_t hash = siphash_2_4(tcp_syncookie_secret, data, 16);
	int mss_idx = mss_to_index(mss);
	// Cookie = hash[31:3] | mss_idx[2:0]
	return ((uint32_t)(hash) & ~7U) | (uint32_t)mss_idx;
}

static int tcp_syncookie_validate(uint32_t src_ip, uint32_t dst_ip,
				  uint16_t src_port, uint16_t dst_port,
				  uint32_t cookie, uint16_t *mss_out)
{
	int mss_idx = cookie & 7;

	// Try current and previous time slots
	for (int delta = 0; delta <= 1; delta++) {
		uint8_t data[16];
		uint32_t time_slot =
			(uint32_t)(timer_ticks() / 600) - (uint32_t)delta;

		data[0] = (uint8_t)(src_ip >> 24);
		data[1] = (uint8_t)(src_ip >> 16);
		data[2] = (uint8_t)(src_ip >> 8);
		data[3] = (uint8_t)(src_ip);
		data[4] = (uint8_t)(dst_ip >> 24);
		data[5] = (uint8_t)(dst_ip >> 16);
		data[6] = (uint8_t)(dst_ip >> 8);
		data[7] = (uint8_t)(dst_ip);
		data[8] = (uint8_t)(src_port >> 8);
		data[9] = (uint8_t)(src_port);
		data[10] = (uint8_t)(dst_port >> 8);
		data[11] = (uint8_t)(dst_port);
		data[12] = (uint8_t)(time_slot >> 24);
		data[13] = (uint8_t)(time_slot >> 16);
		data[14] = (uint8_t)(time_slot >> 8);
		data[15] = (uint8_t)(time_slot);

		uint64_t hash = siphash_2_4(tcp_syncookie_secret, data, 16);
		uint32_t expected =
			((uint32_t)(hash) & ~7U) | (uint32_t)mss_idx;
		if (expected == cookie) {
			if (mss_out)
				*mss_out = syncookie_mss_table[mss_idx];
			return 1; // Valid
		}
	}
	return 0; // Invalid
}

void tcp_init(void)
{
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_connections[i].active = 0;
		tcp_connections[i].state = TCP_STATE_CLOSED;
	}
	// Generate ISN and SYN cookie secrets from CSPRNG
	random_get_bytes(tcp_isn_secret, sizeof(tcp_isn_secret), 0);
	random_get_bytes(tcp_syncookie_secret, sizeof(tcp_syncookie_secret), 0);
	// Bind the deferred-free drain to a softirq vector so the timer
	// can hand work off to ksoftirqd from hard-IRQ context.
	softirq_register(SOFTIRQ_TIMER, tcp_pending_softirq);
}

// ============================================================================
// RFC 6298 RTT / RTO update.  Called whenever new ACK acknowledges a segment
// for which we have a clean send-time sample (Karn: skip retransmitted segs).
// All times in microseconds.
// ============================================================================
#define TCP_RTO_MIN_US (200000U) // 200 ms
#define TCP_RTO_MAX_US (60000000U) // 60 s
#define TCP_RTO_INITIAL_US (1000000U) // 1 s (RFC 6298 section 2.1)

static void tcp_update_rtt(tcp_conn_t *conn, uint32_t r_us)
{
	if (r_us == 0)
		return;
	if (conn->srtt_us == 0) {
		// First measurement (RFC 6298 section 2.2)
		conn->srtt_us = r_us;
		conn->rttvar_us = r_us / 2;
	} else {
		// RTT_VAR := (1-beta)*RTT_VAR + beta*|SRTT-R|, beta=1/4
		uint32_t diff = (conn->srtt_us > r_us) ?
					(conn->srtt_us - r_us) :
					(r_us - conn->srtt_us);
		conn->rttvar_us = (conn->rttvar_us * 3 + diff) / 4;
		// SRTT := (1-alpha)*SRTT + alpha*R, alpha=1/8
		conn->srtt_us = (conn->srtt_us * 7 + r_us) / 8;
	}
	// RTO = SRTT + max(G, 4*RTTVAR), G=10ms granularity (timer is 100Hz)
	uint64_t rto = (uint64_t)conn->srtt_us + 4ULL * conn->rttvar_us;
	if (rto < TCP_RTO_MIN_US)
		rto = TCP_RTO_MIN_US;
	if (rto > TCP_RTO_MAX_US)
		rto = TCP_RTO_MAX_US;
	conn->rto_us = (uint32_t)rto;
	conn->rto_backoff = 0;
}

static uint64_t tcp_rto_ticks(tcp_conn_t *conn)
{
	uint32_t rto = conn->rto_us ? conn->rto_us : TCP_RTO_INITIAL_US;
	if (conn->rto_backoff) {
		uint32_t shift = conn->rto_backoff > 6 ? 6 : conn->rto_backoff;
		if (rto > (TCP_RTO_MAX_US >> shift))
			rto = TCP_RTO_MAX_US;
		else
			rto <<= shift;
	}
	/* us_per_tick = 1000000 / hz; ticks = rto / us_per_tick = rto * hz / 1000000 */
	uint32_t hz = timer_get_frequency();
	if (hz == 0)
		hz = 100;
	uint64_t ticks = (uint64_t)rto * hz / 1000000ULL;
	if (ticks < 1)
		ticks = 1;
	return ticks;
}

// Caller MUST hold tcp_lock.  Buffers MUST be pre-allocated by the caller
// OUTSIDE tcp_lock and passed in via rx_buf / tx_buf — this function never
// invokes the slab allocator.  This is non-negotiable: slab_alloc /
// slab_free can trigger a TLB shootdown IPI that spins waiting for ACK
// from every CPU; if any other CPU is spinning on tcp_lock with IRQs off
// at that moment, it cannot service the IPI and the system hangs with
// "TLB shootdown sync timeout".
//
// Returns a slot with active=0 still set; the caller is responsible for
// filling in the 4-tuple (local_ip / local_port / remote_ip / remote_port)
// and any state-specific fields, then calling tcp_publish_conn() to
// atomically publish active=1 with a release barrier.  This split is
// required because tcp_rx / tcp_find_conn / tcp_find_listener /
// tcp_timer_tick walk the connection table WITHOUT taking tcp_lock, gating
// every access on conn->active alone — if active=1 were set before the
// 4-tuple was rewritten, another CPU could match the OLD 4-tuple of a
// recycled slot and dispatch a packet to the wrong (about-to-be-rewritten)
// connection.
//
// On allocation failure (table full) returns NULL; caller must slab_free
// the buffers it pre-allocated.
static tcp_conn_t *tcp_alloc_conn(uint8_t *rx_buf, uint8_t *tx_buf)
{
	BUG_ON(rx_buf ==
	       NULL); /* pre-allocated RX buffer must not be NULL — slab_alloc must be called before tcp_lock */
	BUG_ON(tx_buf ==
	       NULL); /* pre-allocated TX buffer must not be NULL — slab_alloc must be called before tcp_lock */
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		if (!tcp_connections[i].active) {
			tcp_conn_t *conn = &tcp_connections[i];
			// Slot is currently active=0.  We hold tcp_lock so no other
			// allocator can race us.  Initialise all per-conn state, but
			// leave active=0 — caller publishes after 4-tuple is written.
			//
			// Bump the generation FIRST: from this point the slot is a
			// different connection than anything that captured a pointer
			// to it earlier, and any (conn, gen) pair they hold must now
			// compare unequal.  Never reset to 0 — it must only ever
			// increase for this slot.
			conn->gen++;
			conn->lock = (spinlock_t)SPINLOCK_INIT("tcp_conn");
			conn->state = TCP_STATE_CLOSED;
			conn->error = 0;
			conn->rx_ready = 0;
			conn->tx_ready = 1;
			conn->accept_ready = 0;
			conn->connect_done = 0;
			conn->accept_head = 0;
			conn->accept_tail = 0;
			conn->backlog = 0;
			conn->parent = NULL;
			conn->retransmit_count = 0;
			conn->retransmit_tick = 0;
			conn->handshake_deadline = 0;
			conn->time_wait_tick = 0;
			conn->peer_mss = TCP_MSS;
			conn->max_seg_size = TCP_MSS;
			conn->inflight_count = 0;
			conn->detached = 0;
			// Self-pointer sentinel: "allocated, owned by tcp layer but
			// not yet bound to a socket".  The socket layer overwrites
			// with the real net_socket_t* at attach time (sock_listen,
			// sock_accept, sock_connect).  sock_close sets it back to
			// NULL.  Without this sentinel, a freshly accept()ed conn
			// that gets a peer RST in the window between tcp_accept
			// returning and sock_accept assigning owner_socket would be
			// reaped by the 100Hz timer (state=CLOSED, owner_socket=NULL),
			// leaving sock_accept with a dangling pointer.
			conn->owner_socket = conn;

			// RFC 6298 initial RTO (no measurement yet)
			conn->srtt_us = 0;
			conn->rttvar_us = 0;
			conn->rto_us = TCP_RTO_INITIAL_US;
			conn->rto_backoff = 0;

			// RFC 5681 NewReno: cwnd starts at 10 segments (RFC 6928 IW10)
			conn->cwnd = 10;
			conn->ssthresh = 0xFFFFFFFFu;
			conn->dup_acks = 0;
			conn->ca_ack_counter = 0;
			conn->total_retrans = 0;
			conn->rcv_adv_last_bytes = 0;

			/* Disable Nagle by default.  Nagle + 200 ms delayed-ACK on the
             * peer side causes the classic "Nagle deadlock" — a small first
             * segment (e.g. a TLS ClientHello split across two libc write()
             * calls) sits queued waiting for an ACK that the peer defers,
             * producing the multi-second delay observed before the server
             * response. */
			conn->nodelay = 1;
			conn->keepalive = 0;
			conn->keepidle_ticks = 7200 * 100; // 2 hours
			conn->keepintvl_ticks = 75 * 100; // 75 s
			conn->keepcnt = 9;
			conn->keep_probes_sent = 0;
			conn->keep_next_tick = 0;
			conn->last_rx_tick = timer_ticks();

			// RFC 7323 / 2018 — feature negotiation state (cleared until peer agrees)
			conn->ts_enabled = 0;
			conn->ts_recent = 0;
			conn->ts_recent_age = 0;
			conn->ws_enabled = 0;
			conn->snd_wscale = 0;
			conn->rcv_wscale =
				7; // we always offer 7 (128x); cleared if not negotiated
			conn->sack_ok = 0;
			conn->sack_block_count = 0;
			conn->ooo_count = 0;
			conn->delayed_ack_pending = 0;
			conn->segs_since_ack = 0;
			conn->delayed_ack_deadline = 0;
			conn->cork = 0;
			conn->cork_deadline = 0;
			conn->fin_wait_2_deadline = 0;
			conn->urgent_valid = 0;
			conn->urgent_byte = 0;
			conn->snd_up = 0;
			conn->rcv_up = 0;
			conn->snd_urg_pending = 0;

			// Adopt pre-allocated RX/TX buffers (allocator was caller, OUTSIDE tcp_lock).
			conn->rx_buf = rx_buf;
			conn->tx_buf = tx_buf;
			conn->rx_buf_size = TCP_RX_BUF_SIZE;
			conn->tx_buf_size = TCP_TX_BUF_SIZE;
			conn->rx_head = 0;
			conn->rx_tail = 0;
			conn->tx_head = 0;
			conn->tx_tail = 0;

			// Caller will set 4-tuple, then call tcp_publish_conn(conn).
			// Leave active=0 here.
			return conn;
		}
	}
	return NULL;
}

// Publish a freshly-allocated conn after the caller has written the
// 4-tuple.  MUST be called with tcp_lock held (so the publish is ordered
// w.r.t. tcp_alloc_conn / tcp_free_conn slot reuse).  The compiler barrier
// prevents the optimiser from reordering the active=1 store before the
// 4-tuple stores; on x86 store-store ordering is a hardware guarantee.
static inline void tcp_publish_conn(tcp_conn_t *conn)
{
	/* INVARIANT tripwire: at most ONE active non-LISTEN conn per 4-tuple.
	 * All publishers hold tcp_lock, so this scan is race-free.  A breach
	 * means the dup-conn guards missed a path: traffic then splits between
	 * two conns (handshake lands on one, data on the other) — the exact
	 * signature of the "handshake completes, ClientHello never arrives"
	 * TLS accept failures.  Diagnostic only: the conn is still published.
	 * O(N) per conn CREATION (not per packet) — negligible. */
	if (conn->remote_port != 0 && conn->state != TCP_STATE_LISTEN) {
		for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
			tcp_conn_t *o = &tcp_connections[i];
			if (o != conn && o->active &&
			    o->state != TCP_STATE_LISTEN &&
			    o->local_port == conn->local_port &&
			    o->remote_port == conn->remote_port &&
			    o->local_ip == conn->local_ip &&
			    o->remote_ip == conn->remote_ip) {
				WARN_RATELIMIT(
					1,
					"tcp: DUPLICATE conn published for :%u->:%u (existing state=%d gen=%u) - dup-guard breached",
					conn->local_port, conn->remote_port,
					o->state, o->gen);
				break;
			}
		}
	}
	__asm__ volatile("" ::: "memory");
	conn->active = 1;
}

// Reap any TIME_WAIT slots to recover capacity when the connection table
// is exhausted.  Called WITHOUT tcp_lock — tcp_free_conn takes only
// conn->lock and may invoke slab_free (which can fire a TLB-shootdown
// IPI), so it must not be done under tcp_lock.  Returns the number of
// slots freed; callers retry tcp_alloc_conn afterwards.  This bounds the
// damage when many short-lived connections fill the table with TIME_WAIT
// entries faster than the 60-second timer can expire them (e.g. repeated
// loopback handshakes from teststress).
static int tcp_reap_time_wait_slots(void)
{
	int reaped = 0;
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *tw = &tcp_connections[i];
		/* Snapshot gen BEFORE the state read; free is gen-validated so a
		 * slot recycled into a live conn between here and the free is
		 * never clobbered. */
		uint32_t g = tw->gen;
		if (tw->active && tw->state == TCP_STATE_TIME_WAIT) {
			tcp_free_conn_gen(tw, g);
			reaped++;
		}
	}
	return reaped;
}

static void tcp_detach_listener_children(tcp_conn_t *listener)
{
	if (!listener)
		return;

	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *conn = &tcp_connections[i];
		if (conn->active && conn->parent == listener)
			conn->parent = NULL;
	}
}

// Release a connection slot back to the free pool.
//
// LOCKING: takes ONLY conn->lock.  Does NOT take tcp_lock — doing so
// would extend a tcp_lock-held + IRQs-off section across the time we
// spend waiting for conn->lock, and conn->lock can be held for many
// milliseconds by tcp_send_data looping over segments doing device PIO.
// During that window any third CPU initiating a TLB shootdown via the
// slab allocator would time out, because every CPU spinning on tcp_lock
// has IRQs disabled and cannot service the shootdown IPI.
//
// The slot-reuse race is still safe without tcp_lock here:
//   - tcp_alloc_conn callers DO hold tcp_lock, so two simultaneous
//     allocators cannot both claim the same slot.
//   - We clear active=0 LAST under conn->lock, after NULLing rx_buf /
//     tx_buf and zeroing the 4-tuple.  An allocator that subsequently
//     sees active=0 (lock-free read inside tcp_lock) is therefore
//     guaranteed to see the slot fully quiesced.
//   - slab_free() is done OUTSIDE conn->lock to avoid a slab→conn
//     lock-order inversion (slab_free itself takes a global slab lock
//     and may trigger a TLB shootdown).
//
// Caller MUST NOT hold conn->lock.  Safe to call with or without
// tcp_lock held.
static void tcp_free_conn_impl(tcp_conn_t *conn, int has_gen, uint32_t gen)
{
	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);
	// Idempotent: if already freed (active=0), do nothing.  Two CPUs may
	// race to free the same slot (e.g. tcp_timer_tick TIME_WAIT expiry vs.
	// tcp_connect's TIME_WAIT recycle); whichever loses the race finds
	// active=0 and bails.
	if (!conn->active) {
		tcp_lock_release(&conn->lock, flags);
		return;
	}
	/* Generation guard for callers that selected this slot with a LOCK-FREE
	 * state read (the TIME_WAIT recyclers below).  Between that read and
	 * this lock the slot can be freed AND re-allocated by tcp_alloc_conn
	 * into a completely different, LIVE connection — active is true again,
	 * so the active-only check above would happily free that live conn
	 * (observed: a client's ESTABLISHED conn freed mid-transfer → its peer's
	 * data gets no-conn-RST'd → TLS 64 KB echo dies at 32 KB).  gen is
	 * bumped on every tcp_alloc_conn, so a mismatch means "not the conn you
	 * looked at" — refuse. */
	if (has_gen && conn->gen != gen) {
		WARN_RATELIMIT(
			1,
			"tcp_free_conn: slot recycled since TIME_WAIT check (gen %u != %u, state=%d) - stale free refused",
			conn->gen, gen, conn->state);
		tcp_lock_release(&conn->lock, flags);
		return;
	}
	/* INVARIANT: a conn must not be freed while a socket still references it.
	 * sock_close() clears owner_socket to NULL before calling tcp_close(), and
	 * the timer reapers only free conns with owner_socket==NULL.  The self-
	 * pointer sentinel (owner_socket==conn) means "allocated, not yet bound to
	 * a socket" and is fine.  A REAL net_socket_t* here means sock_recv/send on
	 * that socket will dereference this slot after we slab_free() its buffers
	 * and tcp_alloc_conn recycles it — a use-after-free that surfaces as an
	 * ECONNRESET / recv-timeout on a connection that "vanished" mid-flow. */
	/* Rate-limited, not ONCE: in a multi-hour stress run a single early
	 * firing would permanently silence the tripwire for exactly the bug
	 * class it exists to catch (live-conn free), leaving later incidents
	 * with no kernel-side evidence. */
	WARN_RATELIMIT(
		conn->owner_socket && conn->owner_socket != (void *)conn,
		"tcp_free_conn: freeing conn :%u->:%u (state=%d gen=%u) with live owner_socket - use-after-free for that socket",
		conn->local_port, conn->remote_port, conn->state, conn->gen);
	/* CULPRIT tracer: NO legitimate free happens while the conn is still
	 * in a synchronized data-bearing state — every valid path transitions
	 * to CLOSED/TIME_WAIT first (close paths, abort, fail_connection) or
	 * frees only TIME_WAIT/CLOSED slots (reapers, recyclers).  A free at
	 * ESTABLISHED/CLOSE_WAIT is therefore always a bug; print the call
	 * chain so the next occurrence identifies the freeing path directly
	 * (observed: client conn with 16 KB unread vanishing mid-TLS-echo —
	 * every state-based suspect ruled out, caller unknown).  Frame
	 * pointers are enabled in this kernel, so the nonzero-level
	 * __builtin_return_address walk is well-defined here. */
	WARN_RATELIMIT(
		conn->state == TCP_STATE_ESTABLISHED ||
			conn->state == TCP_STATE_CLOSE_WAIT,
		"tcp_free_conn: freeing SYNCHRONIZED conn :%u->:%u (state=%d gen=%u owner=%llx used=%u) ra1=%llx ra2=%llx",
		conn->local_port, conn->remote_port, conn->state, conn->gen,
		(uint64_t)(uintptr_t)conn->owner_socket,
		(unsigned)((conn->rx_tail - conn->rx_head + conn->rx_buf_size) %
			   conn->rx_buf_size),
		(uint64_t)(uintptr_t)__builtin_return_address(1),
		(uint64_t)(uintptr_t)__builtin_return_address(2));
	void *old_rx = conn->rx_buf;
	void *old_tx = conn->tx_buf;
	conn->rx_buf = NULL;
	conn->tx_buf = NULL;
	conn->parent = NULL;
	conn->state = TCP_STATE_CLOSED;
	// Zero the 4-tuple so a lock-free walker that races between our
	// active=0 store and a future tcp_alloc_conn cannot match a stale tuple.
	conn->local_ip = 0;
	conn->local_port = 0;
	conn->remote_ip = 0;
	conn->remote_port = 0;
	// Publish active=0 LAST under the lock so anyone re-checking active
	// under conn->lock will see CLOSED + NULL buffers + zero tuple
	// consistently.
	__asm__ volatile("" ::: "memory");
	conn->active = 0;
	tcp_lock_release(&conn->lock, flags);

	// slab_free OUTSIDE conn->lock — see comment above.
	if (old_rx)
		slab_free(old_rx);
	if (old_tx)
		slab_free(old_tx);
}

/* Gen-validated free — the ONLY way to free a conn.  `gen` must be the conn's
 * generation captured when the caller selected it (under conn->lock, or via a
 * lock-free read that this call re-validates).  Frees ONLY if the slot still
 * holds that exact instance (gen match); a slot recycled since the caller
 * looked is left untouched.  There is deliberately no non-gen free wrapper, so
 * no future caller can free a recycled slot by accident. */
static void tcp_free_conn_gen(tcp_conn_t *conn, uint32_t gen)
{
	tcp_free_conn_impl(conn, 1, gen);
}

// Find connection by 4-tuple
static tcp_conn_t *tcp_find_conn(uint32_t local_ip, uint16_t local_port,
				 uint32_t remote_ip, uint16_t remote_port)
{
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *c = &tcp_connections[i];
		if (c->active && c->state != TCP_STATE_LISTEN &&
		    c->local_port == local_port &&
		    c->remote_port == remote_port &&
		    (c->local_ip == local_ip || c->local_ip == 0) &&
		    c->remote_ip == remote_ip) {
			return c;
		}
	}
	return NULL;
}

// Find listening socket on port.
//
// When only one listener exists for the port (common case) returns it
// immediately at O(N) cost.  When multiple listeners share the same
// port — e.g. two concurrent teststress instances both calling bind()
// on port 20101 — distributes incoming SYNs across them by picking the
// listener with the fewest pending connections (accept-queue depth +
// SYN_RECEIVED children not yet enqueued).  This prevents all connections
// from piling up on the lowest-slot listener and starving the others.
static tcp_conn_t *tcp_find_listener(uint32_t local_ip, uint16_t local_port)
{
	// First pass: collect candidate counts.
	tcp_conn_t *first_exact = NULL;
	tcp_conn_t *first_wildcard = NULL;
	int exact_count = 0;
	int wildcard_count = 0;

	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *c = &tcp_connections[i];
		if (!c->active || c->state != TCP_STATE_LISTEN ||
		    c->local_port != local_port)
			continue;
		if (c->local_ip == local_ip) {
			if (!first_exact)
				first_exact = c;
			exact_count++;
		} else if (c->local_ip == 0) {
			if (!first_wildcard)
				first_wildcard = c;
			wildcard_count++;
		}
	}

	// Fast path: single listener — original O(N) behaviour.
	if (exact_count == 1)
		return first_exact;
	if (exact_count == 0 && wildcard_count <= 1)
		return first_wildcard;
	if (exact_count == 0 && wildcard_count == 0)
		return NULL;

	// Multiple listeners on the same port.  Second pass: select the one
	// with the smallest load (accept-queue depth + SYN_RECEIVED children
	// in flight).  Counting in-flight children catches the race where
	// both SYNs arrive before either 3WH completes and the accept queues
	// are both empty — the second SYN sees the first SYN's child (already
	// published with state=SYN_RECEIVED, parent=listener_A) and correctly
	// routes to listener_B instead.
	tcp_conn_t *best_exact = NULL;
	tcp_conn_t *best_wildcard = NULL;
	int best_exact_load = 0x7fffffff;
	int best_wildcard_load = 0x7fffffff;

	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *c = &tcp_connections[i];
		if (!c->active || c->state != TCP_STATE_LISTEN ||
		    c->local_port != local_port)
			continue;

		// Accept-queue occupancy.
		int load = (int)((c->accept_tail - c->accept_head + 16u) % 16u);
		// SYN_RECEIVED children not yet promoted to accept queue.
		for (int j = 0; j < TCP_MAX_CONNECTIONS; j++) {
			tcp_conn_t *ch = &tcp_connections[j];
			if (ch->active && ch->state == TCP_STATE_SYN_RECEIVED &&
			    ch->parent == c)
				load++;
		}

		if (c->local_ip == local_ip && load < best_exact_load) {
			best_exact = c;
			best_exact_load = load;
		} else if (c->local_ip == 0 && load < best_wildcard_load) {
			best_wildcard = c;
			best_wildcard_load = load;
		}
	}

	return best_exact ? best_exact : best_wildcard;
}

// Ring buffer helpers
static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size)
{
	WARN_ON(size == 0);
	return (tail - head + size) % size;
}

static uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t size)
{
	uint32_t used = ring_used(head, tail, size);
	WARN_ON(used + 1 > size); /* ring used > size-1: head/tail corrupt */
	return size - 1 - used;
}

// ============================================================================
// Send TCP Segment
// ============================================================================
int tcp_send_segment(net_device_t *dev, uint32_t src_ip, uint32_t dst_ip,
		     uint16_t src_port, uint16_t dst_port, uint32_t seq,
		     uint32_t ack, uint8_t flags, uint16_t window,
		     const uint8_t *data, uint16_t data_len)
{
	return tcp_send_segment_ex(dev, src_ip, dst_ip, src_port, dst_port, seq,
				   ack, flags, window, data, data_len, NULL, 0);
}

static void tcp_send_ack(tcp_conn_t *conn)
{
	BUG_ON(conn == NULL);
	BUG_ON(conn->dev == NULL);
	uint8_t opts[TCP_MAX_OPTIONS];
	uint8_t olen = tcp_build_options(conn, TCP_ACK, 0, opts);
	uint16_t win = tcp_advertised_window(conn);
	tcp_send_segment_ex(conn->dev, conn->local_ip, conn->remote_ip,
			    conn->local_port, conn->remote_port, conn->snd_nxt,
			    conn->rcv_nxt, TCP_ACK, win, NULL, 0, opts, olen);
	conn->delayed_ack_pending = 0;
	conn->segs_since_ack = 0;
	/* Remember what we advertised so sock_recv's RFC 813 silly-window
     * threshold is based on the most recent advertisement, not stale. */
	conn->rcv_adv_last_bytes = (uint32_t)((
		conn->rx_buf_size -
		((conn->rx_tail - conn->rx_head + conn->rx_buf_size) %
		 conn->rx_buf_size)));
}

/* tcp_queue_ack_locked() — used by tcp_rx to *defer* an ACK transmit.
 *
 * tcp_rx runs the entire RX state machine under conn->lock.  Calling
 * tcp_send_ack() inline transmits the ACK synchronously: build packet,
 * checksum, ipv4_send, eth_send, NIC tx_lock + MMIO doorbell — all with
 * conn->lock held.  Meanwhile sock_recv waits on conn->lock to drain
 * the rx ring.  At ~250 ACKs/sec and ~10-20 µs per ACK TX, this adds a
 * few ms/sec of lock-held time on the RX softirq, which feeds back
 * directly into how quickly the application can read data and how
 * quickly we can ACK the next segment.
 *
 * This function just updates the conn state.  The caller (tcp_rx) sets
 * a local ack_pending flag and, AFTER releasing conn->lock, snapshots
 * the ACK params and calls tcp_send_segment_ex directly.  Multiple
 * tcp_queue_ack_locked() calls during one tcp_rx collapse into a
 * single trailing ACK (cumulative — peer cares only about the latest
 * rcv_nxt). */
static void tcp_queue_ack_locked(tcp_conn_t *conn)
{
	BUG_ON(conn == NULL);
	BUG_ON(conn->dev == NULL);
	conn->delayed_ack_pending = 0;
	conn->segs_since_ack = 0;
	conn->rcv_adv_last_bytes = (uint32_t)((
		conn->rx_buf_size -
		((conn->rx_tail - conn->rx_head + conn->rx_buf_size) %
		 conn->rx_buf_size)));
}

// ============================================================================
// tcp_send_window_update - Proactively advertise a newly opened receive window.
//
// Called from sock_recv after the application drains data from the RX ring.
// Without this, a zero-window pause never self-recovers: when the receiver's
// buffer fills, it advertises window=0 and the sender stops.  The sender
// then has no in-flight segments, so the RTO retransmit path in
// tcp_timer_tick never fires a window probe.  Both sides block on
// sched_yield_in_kernel() indefinitely.
//
// Sending one ACK carrying the current (now larger) window breaks the stall
// immediately and lets the sender resume.
// ============================================================================
void tcp_send_window_update(tcp_conn_t *conn)
{
	if (!conn)
		return;
	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);
	if (conn->state == TCP_STATE_ESTABLISHED ||
	    conn->state == TCP_STATE_CLOSE_WAIT) {
		tcp_send_ack(conn);
	}
	tcp_lock_release(&conn->lock, flags);
}

static void tcp_send_rst(net_device_t *dev, uint32_t src_ip, uint32_t dst_ip,
			 uint16_t src_port, uint16_t dst_port, uint32_t seq,
			 uint32_t ack)
{
	tcp_send_segment(dev, src_ip, dst_ip, src_port, dst_port, seq, ack,
			 TCP_RST | TCP_ACK, 0, NULL, 0);
}

/* Consume an in-order data payload arriving in FIN_WAIT_1 / FIN_WAIT_2.
 * Half-close (RFC 793 §3.5): our FIN closed only OUR direction — the peer
 * may legitimately still deliver data, typically a TLS close_notify sent
 * in response to ours.  Ignoring it would leave rcv_nxt behind the peer's
 * FIN, which the in-order FIN rule then (correctly) keeps refusing — the
 * shutdown never completes cleanly.  Store what fits so a shutdown(SHUT_WR)
 * reader can still recv() it; bytes beyond the ring stay unACKed for
 * retransmission.  Caller holds conn->lock and guarantees seq == rcv_nxt. */
static void tcp_consume_data_half_closed(tcp_conn_t *conn,
					 const uint8_t *payload,
					 uint16_t payload_len)
{
	if (!conn->rx_buf)
		return;
	uint32_t avail =
		ring_free(conn->rx_head, conn->rx_tail, conn->rx_buf_size);
	uint32_t copy = payload_len > avail ? avail : payload_len;
	if (copy > 0) {
		uint32_t first = conn->rx_buf_size - conn->rx_tail;
		if (first > copy)
			first = copy;
		mm_memcpy(conn->rx_buf + conn->rx_tail, payload, first);
		if (copy > first)
			mm_memcpy(conn->rx_buf, payload + first, copy - first);
		conn->rx_tail = (conn->rx_tail + copy) % conn->rx_buf_size;
		conn->rcv_nxt += copy;
		conn->rx_ready = 1;
		poll_notify_io_ready();
		sched_wake_channel((void *)&conn->rx_ready);
	}
}

// ============================================================================
// TCP Connect (active open)
// ============================================================================
tcp_conn_t *tcp_connect(net_device_t *dev, uint32_t local_ip, uint32_t dst_ip,
			uint16_t src_port, uint16_t dst_port)
{
	uint64_t flags;

	// Drain any TIME_WAIT slots deferred by tcp_timer_tick — process
	// context, slab_free is safe here.
	tcp_reap_pending();

	// Pre-allocate the new conn's RX/TX buffers BEFORE taking tcp_lock —
	// slab_alloc may trigger a TLB shootdown IPI, which would deadlock if
	// any other CPU were spinning on tcp_lock with IRQs off.
	uint8_t *new_rx = (uint8_t *)slab_alloc(TCP_RX_BUF_SIZE);
	uint8_t *new_tx = (uint8_t *)slab_alloc(TCP_TX_BUF_SIZE);
	if (!new_rx || !new_tx) {
		WARN_RATELIMIT(
			1, "tcp_connect: conn buffer alloc failed (rx=%d tx=%d)",
			new_rx != NULL, new_tx != NULL);
		if (new_rx)
			slab_free(new_rx);
		if (new_tx)
			slab_free(new_tx);
		return NULL;
	}

	// RFC 6191 / SO_REUSEADDR: recycle any TIME_WAIT slot for the same
	// 4-tuple.  Done WITHOUT tcp_lock — tcp_free_conn is idempotent and
	// takes only conn->lock.  Doing this under tcp_lock would extend the
	// tcp_lock critical section across slab_free (TLB-shootdown deadlock).
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *tw = &tcp_connections[i];
		/* Snapshot gen with the lock-free state read; the free is gen-
		 * validated so a slot recycled into a live conn between the read
		 * and the free (active true again) is NOT clobbered. */
		uint32_t g = tw->gen;
		if (tw->active && tw->state == TCP_STATE_TIME_WAIT &&
		    tw->local_port == src_port && tw->remote_port == dst_port &&
		    tw->local_ip == local_ip && tw->remote_ip == dst_ip) {
			tcp_free_conn_gen(tw, g);
		}
	}

	spin_lock_irqsave(&tcp_lock, &flags);

	tcp_conn_t *conn = tcp_alloc_conn(new_rx, new_tx);
	if (!conn) {
		// Table full.  Drop the lock and reap *any* TIME_WAIT slot to
		// recover capacity (RFC 6191 — a fresh SYN is allowed to evict
		// an unrelated TIME_WAIT entry once normal capacity is exhausted).
		// Without this, repeated short-lived loopback connections (e.g.
		// teststress) can fill all 64 slots with TIME_WAIT entries from
		// ephemeral source ports that the per-4-tuple recycle above
		// cannot match — leaving subsequent connect()s to fail with
		// -ENOMEM and starve the test's accept() peer.
		spin_unlock_irqrestore(&tcp_lock, flags);
		tcp_reap_time_wait_slots();
		spin_lock_irqsave(&tcp_lock, &flags);
		conn = tcp_alloc_conn(new_rx, new_tx);
		if (!conn) {
			spin_unlock_irqrestore(&tcp_lock, flags);
			WARN_RATELIMIT(
				1,
				"tcp_connect: conn table full even after TIME_WAIT reap");
			slab_free(new_rx);
			slab_free(new_tx);
			return NULL;
		}
	}

	conn->dev = dev;
	conn->local_ip = local_ip;
	conn->remote_ip = dst_ip;
	conn->local_port = src_port;
	conn->remote_port = dst_port;
	conn->ts_offset =
		tcp_compute_ts_offset(local_ip, dst_ip, src_port, dst_port);
	conn->iss = tcp_generate_isn(local_ip, dst_ip, src_port, dst_port);
	conn->snd_una = conn->iss;
	conn->snd_nxt = conn->iss + 1;
	conn->snd_wnd = TCP_WINDOW_SIZE;
	conn->rcv_wnd = TCP_WINDOW_SIZE;
	conn->peer_mss = TCP_MSS;
	conn->max_seg_size = tcp_local_mss(dev);
	conn->state = TCP_STATE_SYN_SENT;
	conn->retransmit_tick = timer_ticks() + TCP_SYN_RETRANSMIT_TICKS;
	conn->retransmit_count = 0;
	conn->handshake_deadline = timer_ticks() + TCP_HANDSHAKE_TIMEOUT_TICKS;

	// Publish: 4-tuple is set, now lock-free walkers may match this slot.
	tcp_publish_conn(conn);
	spin_unlock_irqrestore(&tcp_lock, flags);

	// Send SYN
	tcp_send_syn_packet(dev, local_ip, dst_ip, src_port, dst_port,
			    conn->iss, 0, TCP_SYN, TCP_WINDOW_SIZE, conn);

	return conn;
}

// ============================================================================
// TCP Listen (passive open)
// ============================================================================
tcp_conn_t *tcp_listen(net_device_t *dev, uint32_t local_ip,
		       uint16_t local_port, int backlog)
{
	uint64_t flags;

	// Drain deferred-free queue (process context — safe to slab_free).
	tcp_reap_pending();

	// Pre-allocate buffers BEFORE taking tcp_lock — slab_alloc may trigger
	// a TLB shootdown IPI that would deadlock against tcp_lock holders.
	uint8_t *new_rx = (uint8_t *)slab_alloc(TCP_RX_BUF_SIZE);
	uint8_t *new_tx = (uint8_t *)slab_alloc(TCP_TX_BUF_SIZE);
	if (!new_rx || !new_tx) {
		WARN_RATELIMIT(
			1, "tcp_listen: conn buffer alloc failed (rx=%d tx=%d)",
			new_rx != NULL, new_tx != NULL);
		if (new_rx)
			slab_free(new_rx);
		if (new_tx)
			slab_free(new_tx);
		return NULL;
	}

	spin_lock_irqsave(&tcp_lock, &flags);

	tcp_conn_t *conn = tcp_alloc_conn(new_rx, new_tx);
	if (!conn) {
		// Table full — reap TIME_WAIT slots and retry (RFC 6191).
		spin_unlock_irqrestore(&tcp_lock, flags);
		tcp_reap_time_wait_slots();
		spin_lock_irqsave(&tcp_lock, &flags);
		conn = tcp_alloc_conn(new_rx, new_tx);
	}
	if (!conn) {
		spin_unlock_irqrestore(&tcp_lock, flags);
		WARN_RATELIMIT(
			1, "tcp_listen: conn table full even after TIME_WAIT reap");
		slab_free(new_rx);
		slab_free(new_tx);
		return NULL;
	}

	conn->dev = dev;
	conn->local_ip = local_ip;
	conn->local_port = local_port;
	conn->remote_ip = 0;
	conn->remote_port = 0;
	conn->state = TCP_STATE_LISTEN;
	conn->backlog = backlog > 16 ? 16 : backlog;
	conn->max_seg_size = tcp_local_mss(dev);

	// Publish: 4-tuple is set, now lock-free walkers may match this slot.
	tcp_publish_conn(conn);
	spin_unlock_irqrestore(&tcp_lock, flags);
	return conn;
}

// ============================================================================
// TCP Accept (from listener)
// ============================================================================
tcp_conn_t *tcp_accept(tcp_conn_t *listener)
{
	if (!listener || listener->state != TCP_STATE_LISTEN)
		return NULL;

	uint64_t flags;
	tcp_lock_acquire(&listener->lock, &flags);

	/* accept_queue holds raw tcp_conn_t* into the fixed tcp_connections[]
	 * array, and tcp_free_conn() does NOT remove a conn from the queue when
	 * it frees the slot — it only clears parent/active.  So a conn that was
	 * queued on reaching ESTABLISHED and then reset by the peer leaves a
	 * dangling entry, and once tcp_alloc_conn recycles that slot the entry
	 * points at a COMPLETELY UNRELATED connection.  Returning it published a
	 * corpse (or a stranger) to userspace as a live socket: the server's
	 * first read fails, the client sees a reset it never caused, and
	 * sock_accept's ESTABLISHED/CLOSE_WAIT assertion fires.
	 *
	 * The entry's generation settles identity EXACTLY: conn->gen is bumped
	 * every time tcp_alloc_conn claims the slot, so gen still matching means
	 * this is literally the connection that was queued — no guessing from
	 * parent/owner_socket, which are set at several points for several
	 * purposes and cannot stand in for identity.
	 *
	 * A stale entry is SKIPPED, never closed: the slot may now host a
	 * perfectly good new connection that will be enqueued in its own right.
	 * A live conn whose entry we consume is still reachable via the
	 * orphan-recovery scan below (it keeps parent == listener). */
	while (listener->accept_head != listener->accept_tail) {
		struct tcp_accept_entry e =
			listener->accept_queue[listener->accept_head];
		listener->accept_queue[listener->accept_head].conn = NULL;
		listener->accept_queue[listener->accept_head].gen = 0;
		listener->accept_head = (listener->accept_head + 1) % 16;
		if (listener->accept_head == listener->accept_tail)
			listener->accept_ready = 0;

		tcp_conn_t *conn = e.conn;
		/* These three skips silently CONSUME the queue entry.  Each one
		 * means a child that completed its handshake (it was enqueued)
		 * died or was recycled before accept() could claim it — if the
		 * client is still alive and waiting, accept() then times out
		 * with no trace (observed: TLS loopback accept ETIMEDOUT while
		 * the client held an ESTABLISHED conn).  A client that RST'd
		 * right after connecting also lands here, so rate-limited. */
		if (!conn || conn->gen != e.gen) {
			WARN_RATELIMIT(
				conn != NULL,
				"tcp_accept: queued child slot recycled (gen %u != %u) - entry discarded",
				conn ? conn->gen : 0, e.gen);
			continue; /* slot recycled: a different connection now */
		}
		if (!conn->active) {
			WARN_RATELIMIT(
				1,
				"tcp_accept: queued child :%u->:%u freed before accept - entry discarded",
				conn->local_port, conn->remote_port);
			continue; /* freed before we got to it */
		}
		if (conn->state != TCP_STATE_ESTABLISHED &&
		    conn->state != TCP_STATE_CLOSE_WAIT) {
			WARN_RATELIMIT(
				1,
				"tcp_accept: queued child :%u->:%u in state %d (err=%d) - entry discarded",
				conn->local_port, conn->remote_port,
				conn->state, conn->error);
			continue; /* died between queueing and accept */
		}

		conn->parent = NULL;
		tcp_lock_release(&listener->lock, flags);
		return conn;
	}

	tcp_lock_release(&listener->lock, flags);

	// Fallback: if a child connection reached an accept-ready state but
	// was not linked into the explicit accept queue, return it directly.
	//
	// We accept two kinds of orphan match:
	//   (a) conn->parent == listener — the normal stale-parent recovery.
	//   (b) conn->parent == NULL with a 4-tuple that unambiguously
	//       belongs to this listener (matching local_port; local_ip
	//       matches exactly or listener is the wildcard 0.0.0.0).
	//       This recovers conns whose parent pointer was cleared by the
	//       SYN_RECEIVED→ESTABLISHED enqueue path after detecting that
	//       the original parent slot had been reused for an unrelated
	//       conn (see the validation in tcp_rx).  Without (b) such an
	//       orphan would be invisible to accept() forever, hanging the
	//       caller — observed sporadically under teststress network.
	uint64_t tcp_flags;
	spin_lock_irqsave(&tcp_lock, &tcp_flags);

	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *conn = &tcp_connections[i];
		if (!conn->active)
			continue;
		if (conn->state != TCP_STATE_ESTABLISHED &&
		    conn->state != TCP_STATE_CLOSE_WAIT)
			continue;

		int matches = 0;
		if (conn->parent == listener) {
			matches = 1;
		} else if (conn->parent == NULL &&
			   conn->owner_socket == (void *)conn &&
			   conn->local_port == listener->local_port &&
			   conn->remote_port != 0 &&
			   (listener->local_ip == 0 ||
			    conn->local_ip == listener->local_ip)) {
			// owner_socket == conn is the self-pointer sentinel set by
			// tcp_alloc_conn: still unclaimed by any socket.  Once
			// sock_accept() assigns the real net_socket_t*, owner_socket
			// != conn and we skip the connection to prevent double-accept.
			matches = 1;
		}
		if (!matches)
			continue;

		conn->parent = NULL;
		spin_unlock_irqrestore(&tcp_lock, tcp_flags);
		return conn;
	}

	spin_unlock_irqrestore(&tcp_lock, tcp_flags);
	return NULL;
}

// ============================================================================
// TCP Close
// ============================================================================
static int tcp_close_impl(tcp_conn_t *conn, int has_gen, uint32_t gen)
{
	if (!conn)
		return -1;

	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);

	/* Deferred-teardown identity check.  sock_close/sock_shutdown clear
	 * owner_socket under s->lock and only THEN call here — and the moment
	 * owner_socket is NULL, a conn that is already CLOSED (peer RST →
	 * tcp_fail_connection keeps the slot for SO_ERROR) becomes reapable by
	 * the 100Hz orphan reaper.  If the closing task is preempted in that
	 * window, the slot can be freed AND recycled for a brand-new connection
	 * before this runs; proceeding would then close/FIN the INNOCENT new
	 * conn (observed: recycled accept-child driven to FIN_WAIT_1 →
	 * accept() ETIMEDOUT; later freed with its new owner still attached →
	 * the tcp_free_conn owner WARN).  The gen captured under s->lock while
	 * owner_socket was still set settles identity exactly. */
	if (has_gen && (!conn->active || conn->gen != gen)) {
		WARN_RATELIMIT(
			conn->active,
			"tcp: deferred close hit recycled conn slot (gen %u != %u, state=%d) - stale close dropped",
			conn->gen, gen, conn->state);
		tcp_lock_release(&conn->lock, flags);
		return -1;
	}

	/* Snapshot identity while we hold the lock.  The synchronous-free cases
	 * below (LISTEN / SYN_SENT / already-CLOSED) set state=CLOSED, release
	 * the lock, then free — but the moment state is CLOSED and owner_socket
	 * is NULL the 100Hz reaper can grab the conn, defer it, and a drain can
	 * free AND recycle the slot into a new live conn before our free runs.
	 * Freeing gen-validated makes that stale free a no-op. */
	uint32_t self_gen = conn->gen;

	switch (conn->state) {
	case TCP_STATE_ESTABLISHED:
	case TCP_STATE_SYN_RECEIVED: {
		int fin_sent = tcp_send_segment(
			conn->dev, conn->local_ip, conn->remote_ip,
			conn->local_port, conn->remote_port, conn->snd_nxt,
			conn->rcv_nxt, TCP_FIN | TCP_ACK,
			(uint16_t)conn->rcv_wnd, NULL, 0);
		// Queue the FIN for retransmit whether or not the immediate
		// send succeeded.  A local drop (skb pool exhausted under load)
		// must not swallow the FIN: if it is not queued, snd_nxt never
		// advances, the retransmit timer has nothing to resend, and the
		// peer blocks in recv()/SSL_read forever waiting for a close
		// that never arrives.
		if (tcp_queue_inflight(conn, conn->snd_nxt, TCP_FIN | TCP_ACK,
				       NULL, 0) == 0) {
			conn->snd_nxt++;
			if (fin_sent != 0)
				conn->retransmit_tick =
					timer_ticks() +
					TCP_LOCAL_DROP_RETRY_TICKS;
		}
		conn->state = TCP_STATE_FIN_WAIT_1;
		tcp_lock_release(&conn->lock, flags);
		break;
	}

	case TCP_STATE_CLOSE_WAIT: {
		int fin_sent = tcp_send_segment(
			conn->dev, conn->local_ip, conn->remote_ip,
			conn->local_port, conn->remote_port, conn->snd_nxt,
			conn->rcv_nxt, TCP_FIN | TCP_ACK,
			(uint16_t)conn->rcv_wnd, NULL, 0);
		// Same local-drop safety as the ESTABLISHED case above.
		if (tcp_queue_inflight(conn, conn->snd_nxt, TCP_FIN | TCP_ACK,
				       NULL, 0) == 0) {
			conn->snd_nxt++;
			if (fin_sent != 0)
				conn->retransmit_tick =
					timer_ticks() +
					TCP_LOCAL_DROP_RETRY_TICKS;
		}
		conn->state = TCP_STATE_LAST_ACK;
		tcp_lock_release(&conn->lock, flags);
		break;
	}

	case TCP_STATE_LISTEN:
		tcp_lock_release(&conn->lock, flags);

		spin_lock_irqsave(&tcp_lock, &flags);
		tcp_detach_listener_children(conn);
		spin_unlock_irqrestore(&tcp_lock, flags);

		conn->state = TCP_STATE_CLOSED;
		tcp_free_conn_gen(conn, self_gen);
		break;

	case TCP_STATE_SYN_SENT:
		conn->state = TCP_STATE_CLOSED;
		tcp_lock_release(&conn->lock, flags);
		tcp_free_conn_gen(conn, self_gen);
		break;

	case TCP_STATE_CLOSED:
		// Connection already torn down by the protocol path (peer RST,
		// retransmit timeout via tcp_fail_connection, etc.) but the
		// slot is still active because tcp_fail_connection deliberately
		// does NOT free — the owning socket may still query SO_ERROR.
		// Now that the socket is releasing it, recover the slot.
		// Without this, every RST'd / timed-out client connect leaks
		// one slot until reboot, eventually exhausting
		// TCP_MAX_CONNECTIONS and silently dropping further SYNs.
		tcp_lock_release(&conn->lock, flags);
		tcp_free_conn_gen(conn, self_gen);
		break;

	default:
		// Mark the conn as owner-detached so any later protocol-side
		// transition to CLOSED (peer RST, retransmit timeout in
		// tcp_fail_connection, FIN_WAIT_2 timeout, LAST_ACK→CLOSED)
		// is reaped by tcp_timer_tick.  Otherwise an orphaned conn
		// that RSTs after we already sent FIN would sit in CLOSED
		// forever, leaking a slot.  Safe under conn->lock here.
		conn->detached = 1;
		tcp_lock_release(&conn->lock, flags);
		break;
	}

	// Opportunistic drain of slots deferred for free by tcp_timer_tick.
	// We are in process context (a syscall) — slab_free is safe here.
	tcp_reap_pending();
	return 0;
}

int tcp_close(tcp_conn_t *conn)
{
	return tcp_close_impl(conn, 0, 0);
}

/* Gen-validated close for DEFERRED teardown (sock_close/sock_shutdown call
 * this after dropping s->lock).  `gen` must be captured under s->lock while
 * owner_socket was still set (the conn cannot be reaped while owned).  Returns
 * -1 without touching the conn if the slot was freed/recycled in between. */
int tcp_close_gen(tcp_conn_t *conn, uint32_t gen)
{
	return tcp_close_impl(conn, 1, gen);
}

// ============================================================================
// TCP Send Data
// ============================================================================
int tcp_send_data(tcp_conn_t *conn, const uint8_t *data, uint16_t len)
{
	/* A raw -1 here reached userspace as errno 1 (EPERM) via sock_send —
	 * observed as "send failed: Operation not permitted" when the conn was
	 * reset mid-transfer.  Report the real condition: the conn's recorded
	 * error (ECONNRESET/ETIMEDOUT from tcp_fail_connection) or EPIPE. */
	if (!conn || conn->state != TCP_STATE_ESTABLISHED)
		return (conn && conn->error) ? -conn->error : -EPIPE;
	if (len == 0)
		return 0;
	WARN_ON(conn->local_port == 0 ||
		conn->remote_port ==
			0); /* sending on a connection without a bound 4-tuple */
	WARN_ON(conn->tx_buf ==
		NULL); /* TX ring buffer is NULL — tcp_alloc_conn invariant violated */

	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);

	/* TOCTOU re-check: state may have changed to CLOSED by tcp_fail_connection
     * on another CPU between the unlocked entry check above and here. */
	if (conn->state != TCP_STATE_ESTABLISHED) {
		int err = conn->error ? -conn->error : -EPIPE;
		tcp_lock_release(&conn->lock, flags);
		return err;
	}

	uint16_t sent = 0;
	uint16_t seg_mss = tcp_effective_mss(conn);
	// Reserve room for TS option (12 bytes after NOP padding) when negotiated
	if (conn->ts_enabled && seg_mss > 12)
		seg_mss -= 12;

	// RFC 5681: don't put more than min(cwnd, snd_wnd) bytes in flight.
	uint32_t flightsize = 0;
	for (uint8_t i = 0; i < conn->inflight_count; i++)
		flightsize += conn->inflight[i].len;
	uint32_t cwnd_bytes = conn->cwnd * seg_mss;
	uint32_t window_bytes = conn->snd_wnd ? conn->snd_wnd : seg_mss;
	uint32_t allowed =
		cwnd_bytes < window_bytes ? cwnd_bytes : window_bytes;
	uint32_t budget = (allowed > flightsize) ? (allowed - flightsize) : 0;

	while (sent < len) {
		if (conn->inflight_count >= TCP_MAX_INFLIGHT) {
			conn->tx_ready = 0;
			break;
		}
		if (budget == 0) {
			if (sent == 0)
				conn->tx_ready = 0;
			break;
		}

		uint16_t seg_len = (uint16_t)(len - sent);
		if (seg_len > seg_mss)
			seg_len = seg_mss;
		if (seg_len > budget)
			seg_len = (uint16_t)budget;

		// Nagle: when nodelay is off and any unacked data outstanding, only
		// send MSS-sized segments to coalesce small writes.
		if (!conn->nodelay && flightsize > 0 && seg_len < seg_mss &&
		    sent > 0)
			break;

		// TCP_CORK: hold partial segments up to ~200ms unless filled
		if (conn->cork && seg_len < seg_mss) {
			if (conn->cork_deadline == 0)
				conn->cork_deadline = timer_ticks() + 20;
			if (timer_ticks() < conn->cork_deadline)
				break;
			conn->cork_deadline = 0;
		}

		uint8_t opts[TCP_MAX_OPTIONS];
		uint8_t olen =
			tcp_build_options(conn, TCP_ACK | TCP_PSH, 0, opts);
		if (tcp_send_segment_ex(conn->dev, conn->local_ip,
					conn->remote_ip, conn->local_port,
					conn->remote_port, conn->snd_nxt,
					conn->rcv_nxt, TCP_ACK | TCP_PSH,
					tcp_advertised_window(conn),
					data + sent, seg_len, opts, olen) < 0) {
			break;
		}

		tcp_queue_inflight(conn, conn->snd_nxt, TCP_ACK | TCP_PSH,
				   data + sent, seg_len);
		conn->snd_nxt += seg_len;
		sent += seg_len;
		flightsize += seg_len;
		budget -= seg_len;
	}

	if (sent == 0) {
		conn->tx_ready = 0;
		tcp_lock_release(&conn->lock, flags);
		return 0;
	}

	conn->retransmit_tick = timer_ticks() + tcp_rto_ticks(conn);
	conn->retransmit_count = 0;

	tcp_lock_release(&conn->lock, flags);
	return sent;
}

// ============================================================================
// TCP Receive Processing
// ============================================================================

// Idle connection timeout: close ESTABLISHED connections with no data for 5 minutes
#define TCP_IDLE_TIMEOUT_TICKS (300 * 100)

void tcp_rx(net_device_t *dev, uint32_t src_ip, uint32_t dst_ip,
	    const uint8_t *data, uint16_t len)
{
	if (len < sizeof(tcp_header_t))
		return;

	const tcp_header_t *tcp = (const tcp_header_t *)data;
	uint16_t src_port = net_ntohs(tcp->src_port);
	uint16_t dst_port = net_ntohs(tcp->dst_port);
	uint32_t seq = net_ntohl(tcp->seq_num);
	uint32_t ack = net_ntohl(tcp->ack_num);
	uint8_t tcp_flags = tcp->flags;
	uint16_t window = net_ntohs(tcp->window);
	uint8_t data_offset = (tcp->data_offset >> 4) * 4;
	tcp_parsed_opts_t pop;
	tcp_parse_options(tcp, data_offset, &pop);
	uint16_t peer_mss = pop.mss;
	uint16_t urg_ptr = net_ntohs(tcp->urgent_ptr);

	if (data_offset > len)
		return;
	const uint8_t *payload = data + data_offset;
	uint16_t payload_len = len - data_offset;

	uint32_t local_ip = dst_ip;

	// --- Illegal flag combinations ---
	// Null scan: no flags at all
	if (tcp_flags == 0)
		return;
	// SYN+FIN simultaneously
	if ((tcp_flags & (TCP_SYN | TCP_FIN)) == (TCP_SYN | TCP_FIN))
		return;
	// XMAS scan: SYN+FIN+RST+PSH+URG+ACK all set
	if ((tcp_flags & 0x3F) == 0x3F)
		return;

	// --- Land attack: SYN with src == dst 4-tuple ---
	if ((tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK) &&
	    src_ip == dst_ip && src_port == dst_port)
		return;

	// --- Per-source SYN rate limit (complements SYN cookies) ---
	if ((tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK)) {
		uint64_t rl_flags;
		spin_lock_irqsave(&g_ratelimit_lock, &rl_flags);
		int syn_ok = net_rl_src_allow(&g_tcp_syn_rl, src_ip);
		spin_unlock_irqrestore(&g_ratelimit_lock, rl_flags);
		if (!syn_ok)
			return;
	}

	// Find existing connection
	tcp_conn_t *conn = tcp_find_conn(local_ip, dst_port, src_ip, src_port);

	// RFC 1122 §4.2.2.13: a fresh SYN whose 4-tuple matches an existing
	// TIME_WAIT slot must be allowed to re-open the connection.  Without
	// this, rapid ephemeral-port reuse under stress (teststress network
	// loops) hits a leftover TIME_WAIT slot whose state-machine handler
	// only re-ACKs FINs and silently drops the SYN — no SYN+ACK is sent,
	// no SYN_RECEIVED conn is ever created, and the server's accept()
	// hangs forever waiting for a peer that has already given up.  Free
	// the TIME_WAIT slot here and fall through to the listener path so
	// the SYN is processed as a brand-new connection.
	WARN_ON(conn &&
		!conn->active); /* tcp_find_conn returned a non-active slot — race with tcp_free_conn */
	/* Same lock-free-select→free hazard as the TIME_WAIT recyclers: the
	 * top-of-tcp_rx tcp_find_conn is lock-free, so between it and the free
	 * this TIME_WAIT slot can be recycled into a live conn.  Gen-validate. */
	if (conn) {
		uint32_t g = conn->gen;
		if (conn->state == TCP_STATE_TIME_WAIT && (tcp_flags & TCP_SYN) &&
		    !(tcp_flags & TCP_ACK)) {
			tcp_free_conn_gen(conn, g);
			conn = NULL;
		}
	}

	if (!conn) {
		// Check for listener
		tcp_conn_t *listener = tcp_find_listener(local_ip, dst_port);
		if (listener && (tcp_flags & TCP_SYN) &&
		    !(tcp_flags & TCP_ACK)) {
			// Check if accept queue is full — use SYN cookies if so
			int queue_used = (listener->accept_tail -
					  listener->accept_head + 16) %
					 16;
			if (queue_used >= listener->backlog) {
				// Accept queue full: respond with SYN cookie instead of allocating state
				uint32_t cookie = tcp_syncookie_generate(
					src_ip, local_ip, src_port, dst_port,
					seq, TCP_MSS);
				tcp_send_segment(dev, local_ip, src_ip,
						 dst_port, src_port, cookie,
						 seq + 1, TCP_SYN | TCP_ACK,
						 TCP_WINDOW_SIZE, NULL, 0);
				return;
			}

			// New connection on listening socket.
			// Pre-allocate buffers BEFORE taking tcp_lock — slab_alloc may
			// trigger a TLB shootdown IPI that would deadlock against
			// tcp_lock holders.
			uint8_t *nc_rx = (uint8_t *)slab_alloc(TCP_RX_BUF_SIZE);
			uint8_t *nc_tx = (uint8_t *)slab_alloc(TCP_TX_BUF_SIZE);
			if (!nc_rx || !nc_tx) {
				if (nc_rx)
					slab_free(nc_rx);
				if (nc_tx)
					slab_free(nc_tx);
				// Fallback to SYN cookie
				uint32_t cookie = tcp_syncookie_generate(
					src_ip, local_ip, src_port, dst_port,
					seq, TCP_MSS);
				tcp_send_segment(dev, local_ip, src_ip,
						 dst_port, src_port, cookie,
						 seq + 1, TCP_SYN | TCP_ACK,
						 TCP_WINDOW_SIZE, NULL, 0);
				return;
			}

			uint64_t flags;
			spin_lock_irqsave(&tcp_lock, &flags);

			/* Duplicate-conn guard.  The tcp_find_conn() at the top
			 * of tcp_rx ran LOCK-FREE, so between it and acquiring
			 * tcp_lock here another CPU processing a racing SYN for
			 * the SAME 4-tuple (a client SYN retransmit, or the
			 * loopback self-connect delivering the peer's SYN on
			 * another CPU) may have already allocated + published a
			 * SYN_RECEIVED conn.  Re-check under the lock: without
			 * this we would allocate a SECOND conn for one 4-tuple.
			 * The client's data then lands on whichever conn
			 * tcp_find_conn returns first while accept() hands the
			 * listener the OTHER, empty conn — the server reads an
			 * immediate 0-byte EOF although the client sent and
			 * completed fine (intermittent "tcp eth0: recv 0/4096
			 * rc=0" under parallel teststress).  Drop this SYN; the
			 * existing conn's SYN-ACK (already sent, and rearmed by
			 * the retransmit timer) drives the handshake. */
			if (tcp_find_conn(local_ip, dst_port, src_ip,
					  src_port)) {
				spin_unlock_irqrestore(&tcp_lock, flags);
				slab_free(nc_rx);
				slab_free(nc_tx);
				return;
			}

			tcp_conn_t *new_conn = tcp_alloc_conn(nc_rx, nc_tx);
			if (!new_conn) {
				// Table full.  Reap TIME_WAIT slots (RFC 6191) before
				// falling back to stateless SYN cookies — cookies disable
				// TS/WSCALE/SACK option negotiation, so handshakes that
				// succeed via cookies have degraded performance for the
				// entire connection.  Reaping is done WITHOUT tcp_lock
				// because tcp_free_conn may slab_free → TLB shootdown.
				spin_unlock_irqrestore(&tcp_lock, flags);
				tcp_reap_time_wait_slots();
				spin_lock_irqsave(&tcp_lock, &flags);
				new_conn = tcp_alloc_conn(nc_rx, nc_tx);
			}
			if (!new_conn) {
				spin_unlock_irqrestore(&tcp_lock, flags);
				slab_free(nc_rx);
				slab_free(nc_tx);
				// Fallback to SYN cookie
				uint32_t cookie = tcp_syncookie_generate(
					src_ip, local_ip, src_port, dst_port,
					seq, TCP_MSS);
				tcp_send_segment(dev, local_ip, src_ip,
						 dst_port, src_port, cookie,
						 seq + 1, TCP_SYN | TCP_ACK,
						 TCP_WINDOW_SIZE, NULL, 0);
				return;
			}

			new_conn->dev = dev;
			new_conn->local_ip = local_ip;
			new_conn->remote_ip = src_ip;
			new_conn->local_port = dst_port;
			new_conn->remote_port = src_port;
			new_conn->ts_offset = tcp_compute_ts_offset(
				local_ip, src_ip, dst_port, src_port);
			new_conn->iss = tcp_generate_isn(local_ip, src_ip,
							 dst_port, src_port);
			new_conn->irs = seq;
			new_conn->snd_una = new_conn->iss;
			new_conn->snd_nxt = new_conn->iss + 1;
			new_conn->rcv_nxt = seq + 1;
			new_conn->snd_wnd =
				window; // not yet scaled (SYN window per RFC 7323)
			new_conn->rcv_wnd = TCP_WINDOW_SIZE;
			new_conn->peer_mss = peer_mss ? peer_mss : TCP_MSS;
			new_conn->max_seg_size = new_conn->peer_mss;
			new_conn->state = TCP_STATE_SYN_RECEIVED;
			new_conn->parent = listener;
			// Arm SYN+ACK retransmit deadline.  tcp_timer_tick now handles
			// SYN_RECEIVED retransmit/timeout; without this, a lost client
			// ACK leaves the slot wedged forever and hangs accept().
			new_conn->retransmit_count = 0;
			new_conn->retransmit_tick =
				timer_ticks() + TCP_SYN_RETRANSMIT_TICKS;
			new_conn->handshake_deadline =
				timer_ticks() + TCP_HANDSHAKE_TIMEOUT_TICKS;

			// RFC 7323/2018 — adopt peer-offered options
			if (pop.ts_present) {
				new_conn->ts_enabled = 1;
				new_conn->ts_recent = pop.tsval;
				new_conn->ts_recent_age =
					(uint32_t)timer_ticks();
			}
			if (pop.wscale >= 0) {
				new_conn->ws_enabled = 1;
				new_conn->snd_wscale = (uint8_t)pop.wscale;
				// rcv_wscale already set in alloc; will be advertised in SYN+ACK
			} else {
				new_conn->rcv_wscale = 0;
			}
			if (pop.sack_perm)
				new_conn->sack_ok = 1;

			// Publish: 4-tuple set, now lock-free walkers may match this slot.
			tcp_publish_conn(new_conn);
			spin_unlock_irqrestore(&tcp_lock, flags);

			// Send SYN+ACK with mirrored options
			tcp_send_synack_conn(new_conn,
					     tcp_syn_window(new_conn));
			return;
		}

		// SYN cookie validation: ACK for unknown connection with a listener
		if (listener && (tcp_flags & TCP_ACK) &&
		    !(tcp_flags & TCP_SYN)) {
			uint32_t cookie =
				ack -
				1; // The ISN we sent was cookie, client ACKs cookie+1
			uint16_t cookie_mss = 0;
			if (tcp_syncookie_validate(src_ip, local_ip, src_port,
						   dst_port, cookie,
						   &cookie_mss)) {
				// Valid SYN cookie - create connection directly in ESTABLISHED state.
				// Pre-allocate buffers BEFORE taking tcp_lock (TLB-shootdown safety).
				uint8_t *cc_rx =
					(uint8_t *)slab_alloc(TCP_RX_BUF_SIZE);
				uint8_t *cc_tx =
					(uint8_t *)slab_alloc(TCP_TX_BUF_SIZE);
				if (!cc_rx || !cc_tx) {
					if (cc_rx)
						slab_free(cc_rx);
					if (cc_tx)
						slab_free(cc_tx);
					return;
				}

				uint64_t flags;
				spin_lock_irqsave(&tcp_lock, &flags);

				/* Same duplicate-conn guard as the SYN path: the
				 * top-of-tcp_rx tcp_find_conn ran lock-free, so a
				 * racing cookie-ACK (retransmit, or the loopback
				 * self-connect delivering on another CPU) may have
				 * already created + published the conn for this
				 * 4-tuple.  Re-check under the lock so we never
				 * mint a SECOND conn — otherwise the client's data
				 * splits across two conns and accept() hands out
				 * the wrong (empty) one. */
				if (tcp_find_conn(local_ip, dst_port, src_ip,
						  src_port)) {
					spin_unlock_irqrestore(&tcp_lock, flags);
					slab_free(cc_rx);
					slab_free(cc_tx);
					return;
				}

				tcp_conn_t *new_conn =
					tcp_alloc_conn(cc_rx, cc_tx);
				if (!new_conn) {
					// Table full.  Reap TIME_WAIT slots and retry —
					// otherwise a valid SYN-cookie ACK is silently
					// dropped, the client's send() proceeds against a
					// half-open connection, and every data segment is
					// RST'd by the no-listener path below until the
					// client gives up.
					spin_unlock_irqrestore(&tcp_lock,
							       flags);
					tcp_reap_time_wait_slots();
					spin_lock_irqsave(&tcp_lock, &flags);
					new_conn = tcp_alloc_conn(cc_rx, cc_tx);
				}
				if (!new_conn) {
					spin_unlock_irqrestore(&tcp_lock,
							       flags);
					slab_free(cc_rx);
					slab_free(cc_tx);
					return;
				}

				new_conn->dev = dev;
				new_conn->local_ip = local_ip;
				new_conn->remote_ip = src_ip;
				new_conn->local_port = dst_port;
				new_conn->remote_port = src_port;
				// ts_offset deliberately left 0 here: the SYN+ACK that this
				// ACK echoes was emitted from the stateless cookie path with
				// the global TS offset, so RTT/PAWS for the rest of this
				// conn must continue using the global offset for consistency.
				new_conn->iss = cookie;
				new_conn->irs = seq - 1;
				new_conn->snd_una = cookie + 1;
				new_conn->snd_nxt = cookie + 1;
				new_conn->rcv_nxt = seq;
				new_conn->snd_wnd = window;
				new_conn->rcv_wnd = TCP_WINDOW_SIZE;
				new_conn->peer_mss =
					cookie_mss ? cookie_mss : TCP_MSS;
				new_conn->max_seg_size = new_conn->peer_mss;
				new_conn->state = TCP_STATE_ESTABLISHED;
				new_conn->parent = listener;

				// Publish new_conn FIRST (4-tuple is set; lock-free walkers
				// may now match it), THEN release tcp_lock, THEN take
				// listener->lock for the accept_queue enqueue.  We do NOT
				// hold tcp_lock and listener->lock simultaneously: doing so
				// would extend the IRQ-off section across two lock
				// acquisitions and contribute to TLB-shootdown timeouts
				// when other CPUs are already contending listener->lock
				// via tcp_accept().
				tcp_publish_conn(new_conn);
				spin_unlock_irqrestore(&tcp_lock, flags);

				// Enqueue to listener accept queue under listener->lock so
				// it serialises with tcp_accept (the reader).  Re-validate
				// that the slot still hosts a LISTEN-state conn matching
				// our 4-tuple expectations: between tcp_find_listener()
				// (lock-free) above and acquiring listener->lock here, the
				// listener may have been closed and the slot recycled as
				// an unrelated conn (visible under stress when the table
				// churns through TIME_WAIT reaping).  Writing into a
				// non-listener's accept_queue silently corrupts that
				// conn's state.  If validation fails, leave new_conn with
				// parent=NULL so the orphan-recovery fallback in
				// tcp_accept can still hand it to whichever LISTEN socket
				// actually owns the local port.
				uint64_t lflags;
				tcp_lock_acquire(&listener->lock, &lflags);
				if (listener->active &&
				    listener->state == TCP_STATE_LISTEN &&
				    listener->local_port ==
					    new_conn->local_port) {
					int next_tail =
						(listener->accept_tail + 1) %
						16;
					if (next_tail !=
					    listener->accept_head) {
						/* Capture the generation with the
						 * pointer: if this slot is freed
						 * and recycled before accept()
						 * dequeues it, the pair will not
						 * match and the entry is known to
						 * be stale. */
						listener->accept_queue
							[listener->accept_tail]
								.conn = new_conn;
						listener->accept_queue
							[listener->accept_tail]
								.gen =
							new_conn->gen;
						listener->accept_tail =
							next_tail;
						listener->accept_ready = 1;
					}
					tcp_lock_release(&listener->lock,
							 lflags);
				} else {
					tcp_lock_release(&listener->lock,
							 lflags);
					new_conn->parent = NULL;
				}
				return;
			}
		}

		// No connection - send RST (RFC 793: a segment to a nonexistent
		// connection elicits an RST so the peer stops).
		//
		// DIAGNOSTIC: a data/ACK segment (already-handshaken traffic)
		// that finds NO conn, while its PEER — the reverse 4-tuple, i.e.
		// the sender's own end of a loopback/self flow — is still a LIVE
		// conn, means one side of an active connection was freed/recycled
		// out from under a transfer that was mid-flight.  The RST we send
		// here then loops back and aborts that live peer (observed: TLS
		// 64 KB echo giving the client 32 KB then ETIMEDOUT; the in-window
		// RST is caught at the ESTABLISHED RST-received WARN).  This fires
		// ONLY on that premature-free signature — a genuinely stale
		// segment (peer also gone) does not warn, and a bare SYN is a
		// normal new-connection probe.  Lock-free peer peek: read-only,
		// diagnostic; a torn read at worst mislabels one rare warning.
		if ((tcp_flags & TCP_ACK) && !(tcp_flags & (TCP_SYN | TCP_RST))) {
			/* The PEER is the reverse 4-tuple: the sender's OWN end of
			 * the flow (local=this segment's SOURCE, remote=its
			 * DEST).  The gone conn (local=dst_port) is by definition
			 * not found; the peer is the one that should still be
			 * live if this is a mid-transfer premature free. */
			int peer_state = -1;
			for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
				tcp_conn_t *pc = &tcp_connections[i];
				if (pc->active && pc->local_port == src_port &&
				    pc->remote_port == dst_port &&
				    pc->local_ip == src_ip &&
				    pc->remote_ip == local_ip) {
					peer_state = pc->state;
					break;
				}
			}
			WARN_RATELIMIT(
				peer_state == TCP_STATE_ESTABLISHED ||
					peer_state == TCP_STATE_FIN_WAIT_1 ||
					peer_state == TCP_STATE_CLOSE_WAIT,
				"tcp_rx: RST'ing seg (seq=%u len=%u) to GONE conn "
				":%u->:%u whose peer :%u->:%u is ALIVE (state=%d) "
				"- a live flow's endpoint was freed mid-transfer",
				seq, payload_len, src_port, dst_port, dst_port,
				src_port, peer_state);
		}
		if (!(tcp_flags & TCP_RST)) {
			if (tcp_flags & TCP_ACK) {
				tcp_send_rst(dev, local_ip, src_ip, dst_port,
					     src_port, ack, 0);
			} else {
				tcp_send_rst(dev, local_ip, src_ip, dst_port,
					     src_port, 0,
					     seq + payload_len +
						     ((tcp_flags & TCP_SYN) ?
							      1 :
							      0));
			}
		}
		return;
	}

	uint64_t flags;
	/* Deferred-ACK pattern: instead of calling tcp_send_ack() inline
     * (which transmits under conn->lock), tcp_rx queues with
     * tcp_queue_ack_locked() and snapshots the params just before
     * tcp_lock_release().  The actual NIC TX happens AFTER release.
     * Multiple queue calls collapse into a single trailing ACK. */
	int ack_pending = 0;
	tcp_lock_acquire(&conn->lock, &flags);

	// TOCTOU re-validate: tcp_find_conn() above ran lock-free, so between
	// that lookup and acquiring conn->lock the slot may have been freed
	// (active=0) or recycled with a different 4-tuple by tcp_alloc_conn on
	// another CPU.  If anything no longer matches, drop the packet — the
	// caller's payload pointer would otherwise be applied to the wrong
	// connection (silent cross-stream corruption) or a freed buffer (UAF).
	if (!conn->active || conn->local_port != dst_port ||
	    conn->remote_port != src_port || conn->remote_ip != src_ip ||
	    (conn->local_ip != local_ip && conn->local_ip != 0)) {
		tcp_lock_release(&conn->lock, flags);
		return;
	}

	/* RFC 793: a synchronized connection must process RST in EVERY state,
	 * not just ESTABLISHED.  The closing states below had no RST handling
	 * at all, so a conn stuck in FIN_WAIT_1 whose peer was gone kept
	 * retransmitting its FIN forever while the peer's no-conn RSTs bounced
	 * off it (observed: FIN_WAIT_1 conn alive for minutes, RST'ing the
	 * same FIN seq repeatedly).  Same RFC 5961 in-window validation as the
	 * ESTABLISHED case.  TIME_WAIT deliberately still ignores RST
	 * (RFC 1337, TIME-WAIT assassination hazard). */
	if ((tcp_flags & TCP_RST) &&
	    (conn->state == TCP_STATE_FIN_WAIT_1 ||
	     conn->state == TCP_STATE_FIN_WAIT_2 ||
	     conn->state == TCP_STATE_CLOSING ||
	     conn->state == TCP_STATE_LAST_ACK)) {
		if ((int32_t)(seq - conn->rcv_nxt) >= 0 &&
		    (int32_t)(seq - (conn->rcv_nxt + conn->rcv_wnd)) <= 0) {
			tcp_fail_connection(conn, ECONNRESET);
			tcp_lock_release(&conn->lock, flags);
			return;
		}
		/* out-of-window RST: silently drop (RFC 5961) */
		tcp_lock_release(&conn->lock, flags);
		return;
	}

	// Process by state
	switch (conn->state) {
	case TCP_STATE_SYN_SENT:
		if ((tcp_flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
			if (ack == conn->snd_nxt) {
				conn->irs = seq;
				conn->rcv_nxt = seq + 1;
				conn->snd_una = ack;
				conn->snd_wnd = window; // SYN window unscaled
				conn->peer_mss = peer_mss ? peer_mss : TCP_MSS;
				conn->max_seg_size = conn->peer_mss;
				conn->state = TCP_STATE_ESTABLISHED;
				conn->connect_done = 1;

				// Negotiate TS / WS / SACK based on what the peer echoed
				if (pop.ts_present) {
					conn->ts_enabled = 1;
					conn->ts_recent = pop.tsval;
					conn->ts_recent_age =
						(uint32_t)timer_ticks();
				}
				if (pop.wscale >= 0) {
					conn->ws_enabled = 1;
					conn->snd_wscale = (uint8_t)pop.wscale;
				} else {
					conn->rcv_wscale =
						0; // peer didn't agree
				}
				if (pop.sack_perm)
					conn->sack_ok = 1;

				// Send ACK (now with TS option if negotiated, scaled window)
				tcp_queue_ack_locked(conn);
				ack_pending = 1;
			}
		} else if (tcp_flags & TCP_RST) {
			tcp_fail_connection(conn, ECONNREFUSED);
		} else if ((tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK)) {
			// RFC 793 §3.4 simultaneous open — both sides sent SYN
			conn->irs = seq;
			conn->rcv_nxt = seq + 1;
			conn->snd_wnd = window;
			if (pop.ts_present) {
				conn->ts_enabled = 1;
				conn->ts_recent = pop.tsval;
			}
			if (pop.wscale >= 0) {
				conn->ws_enabled = 1;
				conn->snd_wscale = (uint8_t)pop.wscale;
			}
			if (pop.sack_perm)
				conn->sack_ok = 1;
			conn->state = TCP_STATE_SYN_RECEIVED;
			// Arm SYN+ACK retransmit deadline (simultaneous-open path).
			conn->retransmit_count = 0;
			conn->retransmit_tick =
				timer_ticks() + TCP_SYN_RETRANSMIT_TICKS;
			tcp_send_synack_conn(conn, tcp_syn_window(conn));
		}
		break;

	case TCP_STATE_SYN_RECEIVED:
		// Duplicate SYN (no ACK) means our SYN+ACK was lost on the wire
		// (skb_alloc failure under sustained pressure, NIC TX-ring overrun,
		// or a real packet drop on a non-loopback path).  Without this
		// resend, the peer keeps retransmitting SYN forever and we sit
		// here ignoring it because nothing else in this state machine or
		// in tcp_timer_tick retransmits the SYN+ACK for SYN_RECEIVED.
		// The result is a wedged listener with an empty accept_queue and
		// a child slot stuck below ESTABLISHED — observed as a sporadic
		// accept() hang under teststress network loops.
		if ((tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK) &&
		    seq == conn->irs) {
			tcp_send_synack_conn(conn, tcp_syn_window(conn));
			tcp_lock_release(&conn->lock, flags);
			return;
		}
		if (tcp_flags & TCP_ACK) {
			if (ack == conn->snd_nxt) {
				conn->snd_una = ack;
				// First ACK after SYN+ACK: window is now scaled
				{
					uint32_t scaled = (uint32_t)window;
					if (conn->ws_enabled)
						scaled <<= conn->snd_wscale;
					conn->snd_wnd = scaled;
				}
				conn->max_seg_size = conn->peer_mss ?
							     conn->peer_mss :
							     TCP_MSS;
				conn->state = TCP_STATE_ESTABLISHED;
				// Disarm the SYN+ACK retransmit deadline armed at
				// SYN_RECEIVED entry.  ESTABLISHED reuses retransmit_tick
				// for inflight data RTO; leaving the SYN_RECEIVED deadline
				// armed could trip the SYN+ACK retransmit branch on the
				// very next timer tick before any data is in flight.
				conn->retransmit_tick = 0;
				conn->retransmit_count = 0;

				// Enqueue in parent's accept queue.  Re-validate under
				// p->lock that p is still a LISTEN-state conn on the
				// matching local port.  If the listener slot was freed
				// and reused (stress-induced churn through TIME_WAIT
				// reaping), p now points to an unrelated conn whose
				// accept_queue/accept_tail/accept_ready fields are
				// semantically meaningless and would be corrupted by
				// the writes below.  Drop the stale link so the
				// orphan-recovery fallback in tcp_accept can still pair
				// this conn with the real listener by 4-tuple.
				/* Read conn->parent ONCE into a local.  The previous code
                 * read it twice — once for the NULL test, once for the
                 * deref below — which is a TOCTOU race: another CPU can set
                 * conn->parent = NULL between the two reads (e.g.
                 * tcp_detach_listener_children() when the listener is
                 * closed, tcp_free_conn(), or the TIME_WAIT reaper), none of
                 * which serialise against this conn->lock.  The compiler
                 * reloaded the field, so the NULL test passed but the deref
                 * used NULL, computing &((tcp_conn_t*)0)->lock == 0x2e520 and
                 * page-faulting inside spin_lock_irqsave (observed as a
                 * kernel #PF in ksoftirqd's tcp_rx promotion path).  A stale
                 * but non-NULL parent is still handled — it is re-validated
                 * under p->lock below. */
				tcp_conn_t *p = conn->parent;
				if (p) {
					uint64_t pflags;
					spin_lock_irqsave(&p->lock, &pflags);
					if (p->active &&
					    p->state == TCP_STATE_LISTEN &&
					    p->local_port == conn->local_port) {
						int next =
							(p->accept_tail + 1) %
							16;
						if (next != p->accept_head) {
							/* Capture the generation
							 * with the pointer — see
							 * tcp_accept. */
							p->accept_queue
								[p->accept_tail]
									.conn =
								conn;
							p->accept_queue
								[p->accept_tail]
									.gen =
								conn->gen;
							p->accept_tail = next;
							p->accept_ready = 1;
						} else {
							/* accept_queue (16 slots)
							 * full: the just-established
							 * child cannot be published
							 * to accept().  It keeps its
							 * parent link so orphan
							 * recovery may still find it,
							 * but a blocking accept() can
							 * miss it and time out. */
							WARN_RATELIMIT(
								1,
								"tcp: accept queue full on listener :%u (backlog=%d) - established child :%u not enqueued",
								p->local_port,
								p->backlog,
								conn->remote_port);
						}
						spin_unlock_irqrestore(&p->lock,
								       pflags);
					} else {
						/* Parent is no longer a LISTEN conn
						 * on our port — it was closed and
						 * the slot recycled (TIME_WAIT churn)
						 * between tcp_find_listener and here.
						 * The child is orphaned and only
						 * reachable via tcp_accept's 4-tuple
						 * fallback; if that misses it,
						 * accept() times out. */
						WARN_RATELIMIT(
							1,
							"tcp: listener for established child :%u->:%u recycled before enqueue - relying on orphan recovery",
							conn->local_port,
							conn->remote_port);
						spin_unlock_irqrestore(&p->lock,
								       pflags);
						conn->parent = NULL;
					}
				} else if (!tcp_find_listener(conn->local_ip,
							      conn->local_port)) {
					/* The child completed its handshake but its
					 * listener is GONE (closed before the final
					 * ACK arrived — e.g. the server's accept()
					 * timed out and exited) and NO other listener
					 * on the port can adopt it via orphan
					 * recovery.  Nothing will ever accept this
					 * conn: keeping it leaves the peer talking to
					 * a socket-less ESTABLISHED zombie until its
					 * own timeout (observed: TLS client starving
					 * 120 s in SSL_connect), and the slot leaks.
					 * RFC-conformant behaviour is a reset — the
					 * peer gets ECONNRESET immediately. */
					WARN_RATELIMIT(
						1,
						"tcp: established child :%u->:%u has no listener - RST peer and fail conn",
						conn->local_port,
						conn->remote_port);
					tcp_send_rst(conn->dev, conn->local_ip,
						     conn->remote_ip,
						     conn->local_port,
						     conn->remote_port,
						     conn->snd_nxt,
						     conn->rcv_nxt);
					tcp_fail_connection(conn, ECONNRESET);
					tcp_lock_release(&conn->lock, flags);
					return;
				}
			}
		}
		if (tcp_flags & TCP_RST) {
			/* Safe today (SYN_RECEIVED child's owner_socket is the
			 * self-sentinel, so the reaper won't free it in the
			 * release→free window) but gen-validated for uniformity /
			 * future-proofing against that assumption changing. */
			uint32_t g = conn->gen;
			conn->state = TCP_STATE_CLOSED;
			tcp_lock_release(&conn->lock, flags);
			tcp_free_conn_gen(conn, g);
			return;
		}
		/* RFC 793 p.72 (SYN-RECEIVED, acceptable ACK): "enter ESTABLISHED
		 * state and continue processing".  The segment that completes the
		 * handshake may itself carry data: connect() returns as soon as
		 * the SYN+ACK is processed, so the client's first data segment
		 * (which also carries the ACK flag) races the deferred bare ACK
		 * onto the loopback queue and regularly arrives here first.
		 * Breaking out silently dropped that payload: rcv_nxt froze at
		 * iss+1, the follow-up segments piled into the OOO queue, the
		 * peer's FIN was refused as out-of-order (seq exactly
		 * transfer-size ahead of rcv_nxt in dmesg), and the transfer
		 * stalled until RTO retransmission — the intermittent teststress
		 * recv failures.  Process the payload/FIN in ESTABLISHED. */
		if (conn->state == TCP_STATE_ESTABLISHED &&
		    (payload_len > 0 || (tcp_flags & TCP_FIN)))
			goto established_segment;
		break;

	case TCP_STATE_ESTABLISHED:
established_segment:
		if (tcp_flags & TCP_RST) {
			// RFC 5961: validate RST seq within receive window
			if ((int32_t)(seq - conn->rcv_nxt) < 0 ||
			    (int32_t)(seq - (conn->rcv_nxt + conn->rcv_wnd)) >
				    0)
				break; // out-of-window RST, silently drop
			/* INVARIANT-ish: a valid RST is legitimate TCP, but one
			 * that aborts an ESTABLISHED conn with data still in
			 * flight or buffered means a transfer was reset mid-
			 * stream and bytes are lost.  On a self/loopback flow
			 * (no real network) an in-window RST should essentially
			 * never happen — surface it (rate-limited) so a spurious
			 * reset behind a TLS ECONNRESET is diagnosable. */
			WARN_RATELIMIT(conn->inflight_count > 0 || conn->rx_ready,
				       "tcp_rx: in-window RST aborted ESTABLISHED "
				       ":%u->:%u mid-transfer (inflight=%u "
				       "rx_ready=%d) - data lost",
				       conn->local_port, conn->remote_port,
				       conn->inflight_count, conn->rx_ready);
			tcp_fail_connection(conn, ECONNRESET);
			break;
		}
		conn->last_rx_tick = timer_ticks();
		conn->keep_probes_sent = 0;

		// RFC 7323 §5.3 PAWS — drop segments with TSval older than ts_recent
		// (only if seg has data and we have a ts_recent).
		if (conn->ts_enabled && pop.ts_present) {
			if ((int32_t)(pop.tsval - conn->ts_recent) < 0 &&
			    payload_len > 0) {
				// Silent data discard — should essentially never
				// fire on loopback; make it visible so timestamp
				// pollution is diagnosable and not just "recv
				// mysteriously came up short".
				WARN_RATELIMIT(
					1,
					"tcp_rx: PAWS drop (tsval=%u ts_recent=%u len=%u port %u->%u)",
					pop.tsval, conn->ts_recent, payload_len,
					src_port, dst_port);
				tcp_queue_ack_locked(conn);
				ack_pending = 1;
				break;
			}
			// Update ts_recent if seg covers ts_recent's ack point
			if ((int32_t)(seq - conn->rcv_nxt) <= 0 &&
			    (int32_t)(pop.tsval - conn->ts_recent) >= 0) {
				conn->ts_recent = pop.tsval;
				conn->ts_recent_age = (uint32_t)timer_ticks();
			}
		}

		// Apply RFC 7323 window scaling on inbound advertised window
		{
			uint32_t scaled = (uint32_t)window;
			if (conn->ws_enabled)
				scaled <<= conn->snd_wscale;
			conn->snd_wnd = scaled;
		}

		// RFC 6093: track urgent pointer for MSG_OOB / SIOCATMARK
		if (tcp_flags & TCP_URG) {
			uint32_t up = seq + urg_ptr;
			conn->rcv_up = up;
			// Save the OOB byte (last byte of urgent data per BSD semantics)
			if (urg_ptr > 0 && urg_ptr <= payload_len) {
				conn->urgent_byte = payload[urg_ptr - 1];
				conn->urgent_valid = 1;
			}
		}

		// Process ACK
		if (tcp_flags & TCP_ACK) {
			// Validate ACK is within snd_una..snd_nxt (out-of-window ACK flood)
			if ((int32_t)(ack - conn->snd_una) < 0 ||
			    (int32_t)(ack - conn->snd_nxt) > 1) {
				// Out-of-window ACK: send current ACK state back and drop
				tcp_queue_ack_locked(conn);
				ack_pending = 1;
				break;
			}
			// Store any peer-sent SACK blocks for the retransmit timer to use
			if (conn->sack_ok && pop.sack_count > 0) {
				conn->sack_block_count = pop.sack_count;
				for (uint8_t b = 0; b < pop.sack_count; b++) {
					conn->sack_blocks[b].left =
						pop.sack[b].left;
					conn->sack_blocks[b].right =
						pop.sack[b].right;
				}
			}
			if (ack > conn->snd_una && ack <= conn->snd_nxt) {
				// Prefer RFC 7323 TSecr for RTT (ignores Karn ambiguity for retrans)
				int rtt_sampled = 0;
				uint64_t now_us = timer_get_precise_us();
				if (conn->ts_enabled && pop.ts_present &&
				    pop.tsecr != 0) {
					uint32_t now_ts = tcp_ts_now_for(conn);
					uint32_t elapsed_ticks =
						now_ts - pop.tsecr;
					// each TS unit = one 100Hz tick = 10ms = 10000us
					tcp_update_rtt(conn,
						       elapsed_ticks * 10000U);
					rtt_sampled = 1;
				}
				if (!rtt_sampled) {
					for (uint8_t i = 0;
					     i < conn->inflight_count &&
					     !rtt_sampled;
					     i++) {
						tcp_inflight_segment_t *seg =
							&conn->inflight[i];
						uint32_t seg_end =
							seg->seq + seg->len +
							((seg->flags &
							  (TCP_SYN | TCP_FIN)) ?
								 1U :
								 0U);
						if (ack >= seg_end &&
						    seg->retransmit_count ==
							    0 &&
						    seg->send_us) {
							if (now_us >
							    seg->send_us)
								tcp_update_rtt(
									conn,
									(uint32_t)(now_us -
										   seg->send_us));
							rtt_sampled = 1;
						}
					}
				}
				conn->snd_una = ack;
				tcp_ack_inflight(conn, ack);
				conn->retransmit_count = 0;
				conn->retransmit_tick =
					timer_ticks() + tcp_rto_ticks(conn);
				conn->tx_ready =
					conn->inflight_count < TCP_MAX_INFLIGHT;
				conn->dup_acks = 0;

				// RFC 5681 NewReno congestion control on new ACK.
				if (conn->cwnd < conn->ssthresh) {
					conn->cwnd++;
					if (conn->cwnd > 65535U)
						conn->cwnd = 65535U;
				} else {
					/* Per-conn counter — previously this was a file-scope
                     * static, which let parallel flows clobber each
                     * other's congestion-avoidance accounting (one flow
                     * could "earn" cwnd growth driven entirely by another
                     * flow's ACKs). */
					conn->ca_ack_counter++;
					if (conn->ca_ack_counter >=
					    conn->cwnd) {
						conn->cwnd++;
						conn->ca_ack_counter = 0;
					}
				}
			} else if (ack == conn->snd_una && payload_len == 0 &&
				   conn->inflight_count > 0) {
				conn->dup_acks++;
				if (conn->dup_acks == 3) {
					uint32_t flight = conn->inflight_count;
					conn->ssthresh =
						flight > 2 ? flight / 2 : 2;
					conn->cwnd = conn->ssthresh + 3;
					if (conn->inflight_count > 0) {
						tcp_inflight_segment_t *seg =
							&conn->inflight[0];
						tcp_send_segment(
							conn->dev,
							conn->local_ip,
							conn->remote_ip,
							conn->local_port,
							conn->remote_port,
							seg->seq, conn->rcv_nxt,
							seg->flags,
							tcp_advertised_window(
								conn),
							seg->data, seg->len);
						seg->retransmit_count++;
						conn->total_retrans++;
					}
				} else if (conn->dup_acks > 3) {
					conn->cwnd++;
				}
			}
		}

		// Process data: in-order vs out-of-order
		if (payload_len > 0) {
			/* DESYNC tripwire: the FIRST data segment of a loopback
			 * flow is always in order (single FIFO queue, no loss).
			 * A conn that has never delivered a single in-order byte
			 * (rcv_nxt still == irs+1) receiving out-of-order data
			 * means its sequence numbering disagrees with the peer's
			 * — every byte the peer ever sends will silently park in
			 * the OOO queue or dup-ACK forever, starving the reader
			 * with NO error (observed: TLS ClientHello never delivered
			 * although the segment stream arrived; both ends
			 * ETIMEDOUT with zero warnings). */
			WARN_RATELIMIT(
				seq != conn->rcv_nxt &&
					conn->rcv_nxt == conn->irs + 1,
				"tcp_rx: FIRST data seg out of order :%u->:%u seq=%u rcv_nxt=%u irs=%u len=%u - sequence desync",
				conn->remote_port, conn->local_port, seq,
				conn->rcv_nxt, conn->irs, payload_len);
			if (seq == conn->rcv_nxt) {
				uint32_t avail =
					ring_free(conn->rx_head, conn->rx_tail,
						  conn->rx_buf_size);
				uint32_t copy = payload_len;
				if (copy > avail)
					copy = avail;
				/* Bulk-copy into the rx ring, splitting at the buffer wrap.
                 * Previously this was a per-byte loop under conn->lock —
                 * ~1460 dependent stores per segment, and the same pattern
                 * repeats in the OOO drain below. */
				if (copy > 0) {
					uint32_t first = conn->rx_buf_size -
							 conn->rx_tail;
					if (first > copy)
						first = copy;
					mm_memcpy(conn->rx_buf + conn->rx_tail,
						  payload, first);
					if (copy > first) {
						mm_memcpy(conn->rx_buf,
							  payload + first,
							  copy - first);
					}
					conn->rx_tail = (conn->rx_tail + copy) %
							conn->rx_buf_size;
				}
				conn->rcv_nxt += copy;
				/* Only wake the reader when at least one byte was stored.
                 * Setting rx_ready=1 with copy=0 (ring full, probe dropped)
                 * causes sock_recv to return 0 — a false EOF to OpenSSL. */
				if (copy > 0) {
					/* Only fire the (expensive) sched_wake_channel scan
                     * — which walks the entire task list under
                     * g_task_list_lock IRQ-off — when rx_ready actually
                     * transitions 0→1.  If the reader is already
                     * scheduled / running, repeating the wake per
                     * segment was burning 5-20 µs per packet under two
                     * spinlocks for no observable benefit. */
					int was_ready = conn->rx_ready;
					conn->rx_ready = 1;
					poll_notify_io_ready();
					if (!was_ready)
						sched_wake_channel(
							(void *)&conn
								->rx_ready);
				}

				/* Receive-buffer auto-tuning: if we just stored data into a
                 * ring that's now > 50% full, peer is filling faster than
                 * we drain.  Double the ring size (up to TCP_RX_BUF_MAX) so
                 * the BDP-limited throughput ceiling (rx_buf / RTT) keeps
                 * pace with the connection's actual demand.  Connections
                 * that never fill the buffer keep the small initial size. */
				uint32_t ring_used =
					(conn->rx_tail - conn->rx_head +
					 conn->rx_buf_size) %
					conn->rx_buf_size;
				if (ring_used * 2 > conn->rx_buf_size &&
				    conn->rx_buf_size < TCP_RX_BUF_MAX) {
					tcp_grow_rx_buf(conn);
				}

				// Drain any contiguous OOO segments
				int progress = 1;
				while (progress && conn->ooo_count > 0) {
					progress = 0;
					for (uint8_t k = 0; k < conn->ooo_count;
					     k++) {
						if (conn->ooo[k].seq ==
						    conn->rcv_nxt) {
							uint16_t l =
								conn->ooo[k]
									.len;
							uint32_t a2 = ring_free(
								conn->rx_head,
								conn->rx_tail,
								conn->rx_buf_size);
							if (l > a2)
								l = (uint16_t)
									a2;
							for (uint16_t j = 0;
							     j < l; j++) {
								conn->rx_buf
									[conn->rx_tail] =
									conn->ooo[k]
										.data[j];
								conn->rx_tail =
									(conn->rx_tail +
									 1) %
									conn->rx_buf_size;
							}
							conn->rcv_nxt += l;
							// Remove this entry
							for (uint8_t m = k + 1;
							     m <
							     conn->ooo_count;
							     m++)
								conn->ooo[m -
									  1] =
									conn->ooo
										[m];
							conn->ooo_count--;
							progress = 1;
							break;
						}
					}
				}

				/* Stretch-ACK: ACK every 4th segment instead of every 2nd
                 * during sustained in-order bulk receive.  Each ACK fires
                 * a full TX pipeline (skb_alloc × 2, NIC tx_lock + MMIO
                 * doorbell, multiple mm_memcpys) HELD UNDER conn->lock,
                 * which also blocks sock_recv on the user side from
                 * draining the rx ring.  At 1000 pkt/s with ACK-every-2
                 * that's 500 lock-held ACK builds/sec × ~40 µs each =
                 * a hard ceiling of ~1.7 MB/s on single-CPU receive.
                 * RFC 5681 §4.2 allows the stretch as long as the
                 * delay is bounded by the delayed-ACK timer below.
                 * OOO segments and PSH still force an immediate ACK to
                 * avoid hurting interactive / handshake latency. */
				conn->segs_since_ack++;
				if (conn->segs_since_ack >= 4 ||
				    conn->ooo_count > 0 ||
				    (tcp_flags & TCP_PSH)) {
					tcp_queue_ack_locked(conn);
					ack_pending = 1;
				} else if (!conn->delayed_ack_pending) {
					conn->delayed_ack_pending = 1;
					/* 40 ms — matches typical TCP_DELACK_MAX in modern
                     * stacks.  200 ms (the previous value) compounds with
                     * Nagle on the sending side and stalls TLS handshakes
                     * by half a second per round-trip. */
					conn->delayed_ack_deadline =
						timer_ticks() + 4;
				}
			} else if ((int32_t)(seq - conn->rcv_nxt) > 0 &&
				   payload_len <= TCP_MSS) {
				// Out-of-order: insert sorted, dedup
				int dup = 0;
				uint8_t pos = 0;
				for (; pos < conn->ooo_count; pos++) {
					if (conn->ooo[pos].seq == seq) {
						dup = 1;
						break;
					}
					if ((int32_t)(seq -
						      conn->ooo[pos].seq) < 0)
						break;
				}
				if (!dup && conn->ooo_count < TCP_MAX_OOO) {
					for (uint8_t k = conn->ooo_count;
					     k > pos; k--)
						conn->ooo[k] = conn->ooo[k - 1];
					conn->ooo[pos].seq = seq;
					conn->ooo[pos].len = payload_len;
					for (uint16_t j = 0; j < payload_len;
					     j++)
						conn->ooo[pos].data[j] =
							payload[j];
					conn->ooo_count++;
				} else if (!dup) {
					/* OOO queue full: this future segment is
					 * DROPPED and must be retransmitted.
					 * Persistent firing = the reassembly hole
					 * never fills = receive-side stall with no
					 * app-visible error.  Make it visible. */
					WARN_RATELIMIT(
						1,
						"tcp_rx: OOO queue full (%u), dropping seg seq=%u len=%u rcv_nxt=%u :%u->:%u",
						conn->ooo_count, seq,
						payload_len, conn->rcv_nxt,
						conn->remote_port,
						conn->local_port);
				}
				// Send immediate dup-ACK with SACK info (helps fast retransmit)
				tcp_queue_ack_locked(conn);
				ack_pending = 1;
			} else {
				/* seq < rcv_nxt: genuinely already-received data →
				 * dup-ACK below is correct and silent.  BUT a
				 * FUTURE segment (seq > rcv_nxt) that fell through
				 * the OOO branch because payload_len > TCP_MSS is
				 * NOT a duplicate — it is silently discarded here
				 * and only a retransmit can save it.  That is a
				 * silent receive stall; make it visible. */
				WARN_RATELIMIT(
					(int32_t)(seq - conn->rcv_nxt) > 0,
					"tcp_rx: oversize OOO seg silently dropped seq=%u len=%u (>MSS) rcv_nxt=%u :%u->:%u",
					seq, payload_len, conn->rcv_nxt,
					conn->remote_port, conn->local_port);
				// Already-received data — duplicate ACK
				tcp_queue_ack_locked(conn);
				ack_pending = 1;
			}
		}

		// Process FIN — only when it lands exactly in order (RFC 793:
		// the FIN occupies the sequence position one past the last data
		// byte).  Honoring an out-of-order FIN (earlier data still
		// missing or only partially stored) would jump rcv_nxt over the
		// hole — silently discarding the missing bytes, ACKing them as
		// received, and turning the reader's recv() into a false EOF
		// with truncated data.  Dup-ACK instead; the peer retransmits
		// the hole and the FIN gets re-processed in order.
		if (tcp_flags & TCP_FIN) {
			if (seq + payload_len == conn->rcv_nxt) {
				conn->rcv_nxt += 1;
				conn->state = TCP_STATE_CLOSE_WAIT;
				conn->rx_ready = 1; // Wake up reader (EOF)
				poll_notify_io_ready();
				sched_wake_channel((void *)&conn->rx_ready);
			} else {
				WARN_RATELIMIT(
					1,
					"tcp_rx: out-of-order FIN refused (seq=%u len=%u rcv_nxt=%u)",
					seq, payload_len, conn->rcv_nxt);
			}
			tcp_queue_ack_locked(conn);
			ack_pending = 1;
		}
		break;

	case TCP_STATE_FIN_WAIT_1:
		// Half-close: consume in-order data (e.g. the peer's TLS
		// close_notify) so its FIN lands exactly at rcv_nxt below.
		if (payload_len > 0 && seq == conn->rcv_nxt) {
			tcp_consume_data_half_closed(conn, payload,
						     payload_len);
			tcp_queue_ack_locked(conn);
			ack_pending = 1;
		}
		if (tcp_flags & TCP_ACK) {
			// Graceful close leaves the data the app wrote before
			// close() still in flight; the peer keeps ACKing it as
			// it arrives.  Those are PARTIAL (cumulative) ACKs
			// (snd_una < ack < snd_nxt) — process them exactly like
			// ESTABLISHED so the inflight queue drains and the
			// retransmit timer tracks the peer's real gap.  Without
			// this, snd_una freezes at close() time, inflight[0]
			// never advances, the RTO timer resends the already-
			// delivered head forever, retransmit_count never resets,
			// and the conn is failed with the tail still unsent —
			// the peer stalls mid-stream and times out (observed:
			// TLS 64 KB echo, client got 32 KB then ETIMEDOUT).
			if (ack > conn->snd_una && ack <= conn->snd_nxt) {
				conn->snd_una = ack;
				tcp_ack_inflight(conn, ack);
				conn->retransmit_count = 0;
				conn->retransmit_tick =
					timer_ticks() + tcp_rto_ticks(conn);
				conn->dup_acks = 0;
			} else if (ack == conn->snd_una && payload_len == 0 &&
				   conn->inflight_count > 0) {
				// Fast retransmit on 3 dup-ACKs (mirror
				// ESTABLISHED) so a gap recovers within an RTT
				// instead of a full RTO backoff during close.
				conn->dup_acks++;
				if (conn->dup_acks == 3) {
					tcp_inflight_segment_t *seg =
						&conn->inflight[0];
					tcp_send_segment(
						conn->dev, conn->local_ip,
						conn->remote_ip,
						conn->local_port,
						conn->remote_port, seg->seq,
						conn->rcv_nxt, seg->flags,
						tcp_advertised_window(conn),
						seg->data, seg->len);
					seg->retransmit_count++;
					conn->total_retrans++;
				}
			}
			if (ack == conn->snd_nxt) {
				// Our FIN is now ACKed.  Same in-order FIN rule
				// as ESTABLISHED: a peer FIN beyond rcv_nxt
				// means data is still missing — don't jump the
				// hole, dup-ACK and wait for the retransmit.
				if ((tcp_flags & TCP_FIN) &&
				    seq + payload_len == conn->rcv_nxt) {
					conn->rcv_nxt += 1;
					conn->state = TCP_STATE_TIME_WAIT;
					conn->time_wait_tick =
						timer_ticks() +
						TCP_TIME_WAIT_TICKS;
					tcp_queue_ack_locked(conn);
					ack_pending = 1;
				} else {
					conn->state = TCP_STATE_FIN_WAIT_2;
					conn->fin_wait_2_deadline =
						timer_ticks() + 6000; // 60s
					// An out-of-order FIN (data hole still
					// unfilled) just gets a dup-ACK; the
					// peer retransmits and the FIN is
					// re-processed in order in FIN_WAIT_2.
					if (tcp_flags & TCP_FIN) {
						tcp_queue_ack_locked(conn);
						ack_pending = 1;
					}
				}
			}
		}
		if ((tcp_flags & TCP_FIN) &&
		    conn->state == TCP_STATE_FIN_WAIT_1) {
			// In-order FIN only; an OOO FIN gets the dup-ACK and
			// is re-processed once the peer fills the hole.
			if (seq + payload_len == conn->rcv_nxt) {
				conn->rcv_nxt += 1;
				conn->state = TCP_STATE_CLOSING;
			}
			tcp_queue_ack_locked(conn);
			ack_pending = 1;
		}
		break;

	case TCP_STATE_FIN_WAIT_2:
		// Half-close: consume in-order data (see FIN_WAIT_1).
		if (payload_len > 0 && seq == conn->rcv_nxt)
			tcp_consume_data_half_closed(conn, payload,
						     payload_len);
		if (tcp_flags & TCP_FIN) {
			// In-order FIN only (see ESTABLISHED case).  An OOO
			// FIN just gets the dup-ACK below; the peer
			// retransmits the hole and the FIN is re-processed
			// in order.
			if (seq + payload_len == conn->rcv_nxt) {
				conn->rcv_nxt += 1;
				conn->state = TCP_STATE_TIME_WAIT;
				conn->time_wait_tick =
					timer_ticks() + TCP_TIME_WAIT_TICKS;
			}
		}
		if (payload_len > 0 || (tcp_flags & TCP_FIN)) {
			tcp_queue_ack_locked(conn);
			ack_pending = 1;
		}
		break;

	case TCP_STATE_CLOSING:
		if (tcp_flags & TCP_ACK) {
			// Drain inflight on partial ACKs (see FIN_WAIT_1) so any
			// data still unacked at simultaneous close is delivered
			// and retransmit tracks the real gap.
			if (ack > conn->snd_una && ack <= conn->snd_nxt) {
				conn->snd_una = ack;
				tcp_ack_inflight(conn, ack);
				conn->retransmit_count = 0;
				conn->retransmit_tick =
					timer_ticks() + tcp_rto_ticks(conn);
				conn->dup_acks = 0;
			}
			if (ack == conn->snd_nxt) {
				conn->state = TCP_STATE_TIME_WAIT;
				conn->time_wait_tick =
					timer_ticks() + TCP_TIME_WAIT_TICKS;
			}
		}
		break;

	case TCP_STATE_LAST_ACK:
		if (tcp_flags & TCP_ACK) {
			// Drain inflight on partial ACKs (see FIN_WAIT_1): a
			// server that read EOF, wrote a response, then close()d
			// enters LAST_ACK with that response still in flight —
			// it must be delivered before the conn is freed.
			if (ack > conn->snd_una && ack <= conn->snd_nxt) {
				conn->snd_una = ack;
				tcp_ack_inflight(conn, ack);
				conn->retransmit_count = 0;
				conn->retransmit_tick =
					timer_ticks() + tcp_rto_ticks(conn);
				conn->dup_acks = 0;
			}
			if (ack == conn->snd_nxt) {
				/* gen-validate: after we release the lock the
				 * reaper can defer+drain+recycle this slot before
				 * our free runs (it's CLOSED + owner-detached). */
				uint32_t g = conn->gen;
				conn->state = TCP_STATE_CLOSED;
				tcp_lock_release(&conn->lock, flags);
				tcp_free_conn_gen(conn, g);
				return;
			}
		}
		break;

	case TCP_STATE_TIME_WAIT:
		// Re-send ACK for any FIN
		if (tcp_flags & TCP_FIN) {
			tcp_queue_ack_locked(conn);
			ack_pending = 1;
			conn->time_wait_tick =
				timer_ticks() + TCP_TIME_WAIT_TICKS;
		}
		break;

	default:
		break;
	}

	/* Snapshot the deferred ACK params UNDER conn->lock, then release
     * the lock, then transmit.  This removes the NIC TX (skb build +
     * checksum + tx_lock + MMIO doorbell) from the conn->lock-held
     * critical section, so sock_recv can drain the rx ring in parallel
     * with the ACK going out. */
	uint8_t snap_opts[TCP_MAX_OPTIONS];
	uint8_t snap_olen = 0;
	uint16_t snap_win = 0;
	uint32_t snap_seq = 0;
	uint32_t snap_ack = 0;
	net_device_t *snap_dev = NULL;
	uint32_t snap_lip = 0, snap_rip = 0;
	uint16_t snap_lp = 0, snap_rp = 0;
	if (ack_pending) {
		snap_olen = tcp_build_options(conn, TCP_ACK, 0, snap_opts);
		snap_win = tcp_advertised_window(conn);
		snap_seq = conn->snd_nxt;
		snap_ack = conn->rcv_nxt;
		snap_dev = conn->dev;
		snap_lip = conn->local_ip;
		snap_rip = conn->remote_ip;
		snap_lp = conn->local_port;
		snap_rp = conn->remote_port;
	}

	tcp_lock_release(&conn->lock, flags);

	if (ack_pending) {
		tcp_send_segment_ex(snap_dev, snap_lip, snap_rip, snap_lp,
				    snap_rp, snap_seq, snap_ack, TCP_ACK,
				    snap_win, NULL, 0, snap_opts, snap_olen);
	}
}

// ============================================================================
// TCP Timer (called from net_timer_tick, ~100Hz)
// ============================================================================
//
// IMPORTANT: tcp_timer_tick runs from the per-CPU timer IRQ (vector 32).  At
// 100Hz on every CPU, multiple instances can race with each other AND with
// sock_send/sock_recv/tcp_rx running on other CPUs.  Every per-conn body
// below mutates fields (inflight[], snd_nxt, rcv_nxt, cwnd, state, ...) that
// are also touched by the data path under conn->lock; we MUST take the same
// lock here or we corrupt the inflight array (torn struct copies during
// tcp_drop_first_inflight while tcp_queue_inflight writes the next slot),
// build retransmit packets from half-shifted segments (wrong bytes for a
// given seq → mysterious payload mismatches on the receiver), and free
// conn->rx_buf out from under a concurrent sock_recv.
void tcp_timer_tick(void)
{
	uint64_t now = timer_ticks();

	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *conn = &tcp_connections[i];
		// Lock-free pre-check is safe: ->active is only set/cleared with the
		// global tcp_lock held in alloc/free, and a stale read just causes
		// us to skip an entry one tick early or pick it up one tick late.
		if (!conn->active)
			continue;

		// CRITICAL: this function runs in IRQ context (100Hz timer vector)
		// on EVERY CPU.  We MUST NOT spin waiting for conn->lock here:
		// with 4 CPUs, three of them could be spin-waiting IRQ-off on the
		// same conn->lock that a fourth CPU holds (e.g. inside
		// tcp_send_data doing PIO).  If a fifth event (slab page release)
		// initiates a TLB shootdown in that window, none of the spinning
		// CPUs can ACK the IPI -> "TLB shootdown sync timeout".
		//
		// Use trylock and skip on contention -- the conn will be picked up
		// on the next tick (10 ms later).  Timer work is best-effort by
		// design; missing one tick has no correctness impact (RTO/keepalive
		// deadlines are absolute, not deltas).
		//
		// We are already in IRQ context so IRQs are disabled by the CPU's
		// IRQ delivery; no need for spin_lock_irqsave's flag save/restore.
		if (!spin_trylock(&conn->lock))
			continue;

		// Re-check after acquiring the lock: another CPU may have freed it.
		if (!conn->active) {
			spin_unlock(&conn->lock);
			continue;
		}

		int do_free = 0;
		uint32_t free_gen = 0;

		// Orphan reaper: a conn whose owning socket has detached (sock_close
		// ran tcp_close on it) and which has since reached state==CLOSED
		// via a protocol path that doesn't free (peer RST in ESTABLISHED
		// → tcp_fail_connection, retransmit-timeout-ETIMEDOUT,
		// FIN_WAIT_2 timeout that races with this same tick on another
		// CPU, etc.) has no owner left.  Without this reap the slot
		// would sit in CLOSED forever, leaking one of TCP_MAX_CONNECTIONS.
		// Safe: detached=1 means s->tcp was already cleared by sock_close
		// under s->lock, so no socket can dereference this slot.
		if (conn->detached && conn->state == TCP_STATE_CLOSED) {
			do_free = 1;
			goto unlock_conn;
		}

		// Back-pointer reaper: catches the orphan paths tcp_close() does
		// NOT mark detached (ESTABLISHED→FIN_WAIT_1 / CLOSE_WAIT→LAST_ACK
		// when the socket released its reference, then peer ACK or RST
		// drove the conn to CLOSED).  sock_close clears owner_socket
		// BEFORE nulling s->tcp, so owner_socket==NULL guarantees no
		// socket holds this conn anymore.  We still gate on
		// state==CLOSED so an in-flight tcp_rx / tcp_send_data on the
		// same conn cannot be racing us.
		if (!conn->owner_socket && conn->state == TCP_STATE_CLOSED) {
			do_free = 1;
			goto unlock_conn;
		}

		// Sentinel-owner reaper: a listener child that was never claimed
		// by accept() keeps the self-pointer sentinel in owner_socket.
		// If it reaches CLOSED (tcp_fail_connection: handshake orphaned,
		// idle timeout, no-listener RST), NEITHER reaper above can ever
		// free it — detached is never set and owner_socket is non-NULL —
		// so the slot leaked until reboot (observed: CLOSED conn with
		// err=ETIMEDOUT lingering in the table long after its test
		// exited).  Free it after a 10 s grace period from the failure
		// stamp (tcp_fail_connection sets last_rx_tick) so an in-flight
		// accept() claim — which replaces the sentinel with the real
		// socket within the same syscall — can never race the free.
		if (conn->owner_socket == (void *)conn &&
		    conn->state == TCP_STATE_CLOSED &&
		    (now - conn->last_rx_tick) > 1000) {
			do_free = 1;
			goto unlock_conn;
		}

		// TIME_WAIT expiry
		if (conn->state == TCP_STATE_TIME_WAIT &&
		    now >= conn->time_wait_tick) {
			conn->state = TCP_STATE_CLOSED;
			do_free = 1;
			goto unlock_conn;
		}

		// FIN_WAIT_2 timeout (RFC 1122 SHOULD discard after a while)
		if (conn->state == TCP_STATE_FIN_WAIT_2 &&
		    conn->fin_wait_2_deadline &&
		    now >= conn->fin_wait_2_deadline) {
			conn->state = TCP_STATE_CLOSED;
			do_free = 1;
			goto unlock_conn;
		}

		// Delayed ACK fire
		if (conn->state == TCP_STATE_ESTABLISHED &&
		    conn->delayed_ack_pending &&
		    now >= conn->delayed_ack_deadline) {
			tcp_send_ack(conn);
		}

		// Slow-loris / idle timeout: close connections that have received
		// nothing for TCP_IDLE_TIMEOUT_TICKS (5 minutes by default).
		// Covers the closing states too, not just ESTABLISHED: since
		// retransmit_count only advances on transmits that actually left
		// the machine, a FIN_WAIT_1/CLOSING/LAST_ACK conn whose sends
		// keep failing locally (skb exhaustion) would otherwise hold a
		// table slot forever.
		if ((conn->state == TCP_STATE_ESTABLISHED ||
		     conn->state == TCP_STATE_FIN_WAIT_1 ||
		     conn->state == TCP_STATE_CLOSING ||
		     conn->state == TCP_STATE_LAST_ACK) &&
		    conn->last_rx_tick != 0 &&
		    (now - conn->last_rx_tick) > TCP_IDLE_TIMEOUT_TICKS) {
			tcp_fail_connection(conn, ETIMEDOUT);
			goto unlock_conn;
		}

		// TCP_CORK deadline — wake send path by clearing tx_ready oscillation
		if (conn->cork && conn->cork_deadline &&
		    now >= conn->cork_deadline) {
			conn->cork_deadline = 0;
			conn->tx_ready = 1;
		}

		// Retransmission timeout
		if (conn->state == TCP_STATE_SYN_SENT &&
		    now >= conn->retransmit_tick) {
			if (conn->retransmit_count >= TCP_MAX_RETRANSMITS ||
			    (conn->handshake_deadline &&
			     now >= conn->handshake_deadline)) {
				tcp_fail_connection(conn, ETIMEDOUT);
				goto unlock_conn;
			}
			// Retransmit SYN.  A local send failure (skb pool empty
			// under load) is not network loss: retry quickly without
			// consuming the attempt budget.  handshake_deadline
			// bounds the total time either way.
			if (tcp_send_syn_packet(conn->dev, conn->local_ip,
						conn->remote_ip,
						conn->local_port,
						conn->remote_port, conn->iss, 0,
						TCP_SYN, TCP_WINDOW_SIZE,
						conn) >= 0) {
				conn->retransmit_count++;
				conn->retransmit_tick =
					now + TCP_SYN_RETRANSMIT_TICKS;
			} else {
				conn->retransmit_tick =
					now + TCP_LOCAL_DROP_RETRY_TICKS;
			}
		}

		// SYN+ACK retransmit for half-open server-side connections.  The
		// SYN_RECEIVED state has no other timer in this stack, so a lost
		// client ACK (NIC drop, slab pressure, scheduler-induced softirq
		// starvation under teststress) would otherwise leave the slot
		// wedged forever — the listener never sees it in its accept_queue
		// (enqueue happens on transition to ESTABLISHED) and accept()
		// hangs forever.  Resend the SYN+ACK up to TCP_MAX_RETRANSMITS,
		// then drop the slot so a fresh handshake from the same peer can
		// allocate a clean conn instead of finding the old SYN_RECEIVED
		// and being silently dropped by tcp_find_conn (which excludes
		// LISTEN but happily returns SYN_RECEIVED).
		if (conn->state == TCP_STATE_SYN_RECEIVED &&
		    conn->retransmit_tick && now >= conn->retransmit_tick) {
			if (conn->retransmit_count >= TCP_MAX_RETRANSMITS ||
			    (conn->handshake_deadline &&
			     now >= conn->handshake_deadline)) {
				/* A half-open server conn whose 3-way handshake
				 * never completed: the client's ACK never arrived
				 * (or its SYN-ACK never reached the client) before
				 * the deadline.  The child never reached the accept
				 * queue, so a blocking accept() on this listener
				 * times out (errno ETIMEDOUT) — exactly the "tcp
				 * eth0: accept" failure.  Rate-limited so a real SYN
				 * flood can't spam. */
				WARN_RATELIMIT(
					1,
					"tcp: SYN_RECEIVED :%u->:%u dropped after handshake timeout (retx=%u) - accept() will time out",
					conn->local_port, conn->remote_port,
					conn->retransmit_count);
				conn->state = TCP_STATE_CLOSED;
				do_free = 1;
				goto unlock_conn;
			}
			// Same local-drop rule as SYN_SENT: only transmits that
			// actually left the machine consume the attempt budget.
			if (tcp_send_synack_conn(conn, tcp_syn_window(conn)) >=
			    0) {
				conn->retransmit_count++;
				conn->retransmit_tick =
					now + TCP_SYN_RETRANSMIT_TICKS;
			} else {
				conn->retransmit_tick =
					now + TCP_LOCAL_DROP_RETRY_TICKS;
			}
		}

		if ((conn->state == TCP_STATE_ESTABLISHED ||
		     conn->state == TCP_STATE_FIN_WAIT_1 ||
		     conn->state == TCP_STATE_LAST_ACK ||
		     conn->state == TCP_STATE_CLOSING) &&
		    conn->inflight_count > 0 && now >= conn->retransmit_tick) {
			// Give-up budget.  The short limit is meant for closing
			// conns that only have control (a FIN) left to get
			// ACKed — nobody is waiting on payload, and the slot
			// shouldn't be pinned long under stress.  BUT a graceful
			// close (application wrote data then close()d) leaves
			// real DATA queued in FIN_WAIT_1/LAST_ACK/CLOSING that
			// the PEER IS STILL READING: close() must deliver it.
			// Using the short limit there drops that data after ~6 s
			// of backoff and frees the conn out from under the peer
			// (observed: TLS 64 KB echo delivered only 48 KB, then
			// the receiver timed out with the server conn already
			// gone).  So: if the oldest unacked segment carries
			// payload, use the full DATA budget regardless of
			// state; only a pure-control (FIN, len==0) head gets the
			// short limit.
			int head_has_data = conn->inflight[0].len > 0;
			uint32_t retrans_limit =
				(conn->state == TCP_STATE_ESTABLISHED ||
				 head_has_data) ?
					TCP_MAX_DATA_RETRANSMITS :
					TCP_MAX_RETRANSMITS;
			if (conn->retransmit_count >= retrans_limit) {
				tcp_fail_connection(conn, ETIMEDOUT);
				goto unlock_conn;
			}

			tcp_inflight_segment_t *seg = &conn->inflight[0];
			// Skip retransmission for segments fully covered by a SACK block
			if (conn->sack_ok && conn->sack_block_count > 0) {
				uint32_t s_start = seg->seq;
				uint32_t s_end = seg->seq + seg->len;
				for (uint8_t b = 0; b < conn->sack_block_count;
				     b++) {
					if ((int32_t)(conn->sack_blocks[b]
							      .left -
						      s_start) <= 0 &&
					    (int32_t)(conn->sack_blocks[b]
							      .right -
						      s_end) >= 0) {
						// Already SACKed — drop from inflight head and skip retx
						tcp_drop_first_inflight(conn);
						seg = NULL;
						break;
					}
				}
			}
			// A failed transmit here is a local drop (skb pool
			// exhausted under load), not evidence the network lost
			// the segment: it must consume neither the give-up
			// budget nor trigger the congestion penalty, or memory
			// pressure alone can kill a healthy connection without
			// a single packet reaching the peer.  Retry quickly —
			// the pool drains within milliseconds.
			if (seg &&
			    tcp_send_segment(conn->dev, conn->local_ip,
					     conn->remote_ip, conn->local_port,
					     conn->remote_port, seg->seq,
					     conn->rcv_nxt, seg->flags,
					     tcp_advertised_window(conn),
					     seg->data, seg->len) < 0) {
				conn->retransmit_tick =
					now + TCP_LOCAL_DROP_RETRY_TICKS;
			} else {
				if (seg) {
					// RFC 6298: on RTO, ssthresh =
					// max(flightsize/2, 2*MSS), cwnd = 1.
					// Karn: don't sample RTT on
					// retransmits.  Exponential backoff.
					uint32_t flight = conn->inflight_count;
					conn->ssthresh =
						flight > 2 ? flight / 2 : 2;
					conn->cwnd = 1;
					conn->dup_acks = 0;
					conn->rto_backoff++;
					seg->retransmit_count++;
					seg->send_us =
						0; // invalidate RTT sample for retransmitted seg
					conn->retransmit_count++;
					conn->total_retrans++;
				}
				conn->retransmit_tick =
					now + tcp_rto_ticks(conn);
			}
		}

		// SO_KEEPALIVE: send 0-byte probe at seq=snd_una-1 after idle.
		if (conn->keepalive && conn->state == TCP_STATE_ESTABLISHED &&
		    conn->inflight_count == 0) {
			uint64_t idle = now - conn->last_rx_tick;
			if (conn->keep_probes_sent == 0 &&
			    idle >= conn->keepidle_ticks) {
				tcp_send_segment(
					conn->dev, conn->local_ip,
					conn->remote_ip, conn->local_port,
					conn->remote_port, conn->snd_una - 1,
					conn->rcv_nxt, TCP_ACK,
					(uint16_t)conn->rcv_wnd, NULL, 0);
				conn->keep_probes_sent = 1;
				conn->keep_next_tick =
					now + conn->keepintvl_ticks;
			} else if (conn->keep_probes_sent > 0 &&
				   now >= conn->keep_next_tick) {
				if (conn->keep_probes_sent >= conn->keepcnt) {
					tcp_fail_connection(conn, ETIMEDOUT);
					goto unlock_conn;
				}
				tcp_send_segment(
					conn->dev, conn->local_ip,
					conn->remote_ip, conn->local_port,
					conn->remote_port, conn->snd_una - 1,
					conn->rcv_nxt, TCP_ACK,
					(uint16_t)conn->rcv_wnd, NULL, 0);
				conn->keep_probes_sent++;
				conn->keep_next_tick =
					now + conn->keepintvl_ticks;
			}
		}

unlock_conn:
		// Capture the generation BEFORE dropping conn->lock: gen is stable
		// under the lock (tcp_alloc_conn only bumps it on active==0 slots),
		// but the moment the lock drops, another CPU's tick can defer+drain
		// this same dead conn and the slot can be recycled into a NEW live
		// conn.  Reading conn->gen after the unlock (as tcp_defer_free
		// itself used to) would queue the new generation and the drain's
		// gen check would then "validate" a free of that live conn.
		if (do_free)
			free_gen = conn->gen;
		spin_unlock(&conn->lock);

		// CANNOT call tcp_free_conn here: we are in IRQ context (IRQs off)
		// on every CPU at 100Hz.  tcp_free_conn → slab_free → TLB
		// shootdown IPI would deadlock against other CPUs simultaneously
		// running their own timer-IRQ handler with IRQs disabled.  Defer
		// to softirq/process context where IRQs are enabled.
		if (do_free) {
			tcp_defer_free(conn, free_gen);
		}
	}
}

// ============================================================================
// TCP_INFO sockopt — fill struct tcp_info from a connection's runtime state.
// ============================================================================
void tcp_fill_info(tcp_conn_t *conn, struct tcp_info *info)
{
	if (!conn || !info)
		return;
	for (size_t i = 0; i < sizeof(*info); i++)
		((uint8_t *)info)[i] = 0;

	info->tcpi_state = (uint8_t)conn->state;
	info->tcpi_ca_state = (conn->dup_acks >= 3) ? 3 /* recovery */ : 0;
	info->tcpi_retransmits = (uint8_t)conn->retransmit_count;
	info->tcpi_backoff = conn->rto_backoff;
	info->tcpi_options = 0;
	if (conn->ts_enabled)
		info->tcpi_options |= 1; // TIMESTAMPS
	if (conn->sack_ok)
		info->tcpi_options |= 2; // SACK
	if (conn->ws_enabled)
		info->tcpi_options |= 4; // WSCALE
	info->tcpi_snd_wscale_rcv_wscale =
		(uint8_t)((conn->snd_wscale & 0x0F) |
			  ((conn->rcv_wscale & 0x0F) << 4));
	info->tcpi_rto = conn->rto_us ? conn->rto_us : TCP_RTO_INITIAL_US;
	info->tcpi_ato = 40000;
	info->tcpi_snd_mss = conn->max_seg_size ? conn->max_seg_size : TCP_MSS;
	info->tcpi_rcv_mss = conn->peer_mss ? conn->peer_mss : TCP_MSS;
	info->tcpi_unacked = conn->inflight_count;
	info->tcpi_sacked = 0;
	info->tcpi_lost = 0;
	info->tcpi_retrans = conn->retransmit_count;
	info->tcpi_pmtu = conn->dev ? conn->dev->mtu : NET_MTU_DEFAULT;
	info->tcpi_rtt = conn->srtt_us;
	info->tcpi_rttvar = conn->rttvar_us;
	info->tcpi_snd_ssthresh = conn->ssthresh;
	info->tcpi_snd_cwnd = conn->cwnd;
	info->tcpi_advmss = conn->max_seg_size ? conn->max_seg_size : TCP_MSS;
	info->tcpi_reordering = 3;
	info->tcpi_rcv_space = conn->rx_buf_size;
	info->tcpi_total_retrans = conn->total_retrans;
}

// ============================================================================
// RFC 6093 / 793 — Urgent (OOB) send: emit a single byte with URG=1.
// ============================================================================
int tcp_send_oob(tcp_conn_t *conn, uint8_t byte)
{
	if (!conn || conn->state != TCP_STATE_ESTABLISHED)
		return -1;
	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);

	// Build TCP packet manually since urgent_ptr is in header
	uint8_t opts[TCP_MAX_OPTIONS];
	uint8_t olen =
		tcp_build_options(conn, TCP_ACK | TCP_URG | TCP_PSH, 0, opts);

	// Use tcp_send_segment_ex with manual urgent_ptr override — but our helper
	// does not expose it. Send a one-byte payload with URG flag and rely on
	// the receiver tracking urgent pointer = seq+1 via raw header field.
	// For simplicity: send via segment_ex then patch checksum is too much; use
	// a small inline path.

	// Build packet
	uint16_t tcp_len = (uint16_t)(sizeof(tcp_header_t) + olen + 1);
	uint8_t pkt[sizeof(tcp_header_t) + TCP_MAX_OPTIONS + 1];
	tcp_header_t *tcp = (tcp_header_t *)pkt;
	tcp->src_port = net_htons(conn->local_port);
	tcp->dst_port = net_htons(conn->remote_port);
	tcp->seq_num = net_htonl(conn->snd_nxt);
	tcp->ack_num = net_htonl(conn->rcv_nxt);
	tcp->data_offset = (uint8_t)(((sizeof(tcp_header_t) + olen) / 4) << 4);
	tcp->flags = TCP_ACK | TCP_URG | TCP_PSH;
	tcp->window = net_htons(tcp_advertised_window(conn));
	tcp->checksum = 0;
	tcp->urgent_ptr = net_htons(1); // points one past last urgent byte
	for (uint8_t i = 0; i < olen; i++)
		pkt[sizeof(tcp_header_t) + i] = opts[i];
	pkt[sizeof(tcp_header_t) + olen] = byte;

	// Pseudo-header checksum
	uint8_t pseudo[12 + sizeof(tcp_header_t) + TCP_MAX_OPTIONS + 1];
	uint32_t s = net_htonl(conn->local_ip);
	uint32_t d = net_htonl(conn->remote_ip);
	pseudo[0] = (s >> 24) & 0xFF;
	pseudo[1] = (s >> 16) & 0xFF;
	pseudo[2] = (s >> 8) & 0xFF;
	pseudo[3] = s & 0xFF;
	pseudo[4] = (d >> 24) & 0xFF;
	pseudo[5] = (d >> 16) & 0xFF;
	pseudo[6] = (d >> 8) & 0xFF;
	pseudo[7] = d & 0xFF;
	pseudo[8] = 0;
	pseudo[9] = IP_PROTO_TCP;
	pseudo[10] = (tcp_len >> 8) & 0xFF;
	pseudo[11] = tcp_len & 0xFF;
	for (uint16_t i = 0; i < tcp_len; i++)
		pseudo[12 + i] = pkt[i];
	tcp->checksum = ipv4_checksum(pseudo, (uint16_t)(12 + tcp_len));

	int rv = ipv4_send(conn->dev, conn->remote_ip, IP_PROTO_TCP, pkt,
			   tcp_len);
	if (rv == 0) {
		conn->snd_up = conn->snd_nxt + 1;
		conn->snd_nxt += 1;
		tcp_queue_inflight(conn, conn->snd_nxt - 1,
				   TCP_ACK | TCP_URG | TCP_PSH, &byte, 1);
	}
	tcp_lock_release(&conn->lock, flags);
	return rv;
}

// SIOCATMARK equivalent: returns 1 when next byte to read is the urgent mark.
int tcp_at_mark(tcp_conn_t *conn)
{
	if (!conn)
		return 0;
	return (conn->rcv_nxt == conn->rcv_up) ? 1 : 0;
}

// On-demand connection-table snapshot for the Ctrl+N debug dump.
// Lock-free best-effort read — values may tear, but this is purely
// diagnostic and never used for correctness.
void tcp_dump_table(struct tty *tty)
{
	static const char *sn[] = { "CLOSED",  "LISTEN",  "SYN_SNT", "SYN_RCV",
				    "ESTAB",   "FIN_W1",  "FIN_W2",  "CLS_WT",
				    "CLOSING", "LST_ACK", "TM_WAIT" };
	tty_printf(tty, "=== TCP table ===\n");
	/* Field legend:
     *   rc/tr      = retransmit_count / total_retrans (cumulative)
     *   cw/ss      = our cwnd / ssthresh (segments)
     *   if         = our inflight segments (send side)
     *   snd_wnd    = peer's advertised window to us (bytes)
     *   rcv_buf    = current rx ring size (auto-tuned)
     *   rcv_adv    = bytes of OUR rx-buf free space (= peer's effective window after WSCALE)
     *   ws=R/S     = rcv/snd window-scale shift (0/0 caps window at 65535)
     *   srtt/rto   = smoothed RTT / current RTO (microseconds) */
	tty_printf(
		tty,
		"slot st       laddr:lport            raddr:rport       p= ar rc tr cw ss if snd_wnd rcv_buf rcv_adv ws ts sack srtt rto rnxt-snxt-suna\n");
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *c = &tcp_connections[i];
		if (!c->active)
			continue;
		const char *s = (c->state <= TCP_STATE_TIME_WAIT) ?
					sn[c->state] :
					"???";
		uint32_t li = c->local_ip, ri = c->remote_ip;
		int parent_slot = -1;
		if (c->parent) {
			for (int j = 0; j < TCP_MAX_CONNECTIONS; j++)
				if (c->parent == &tcp_connections[j]) {
					parent_slot = j;
					break;
				}
		}
		uint32_t used = (c->rx_tail - c->rx_head + c->rx_buf_size) %
				c->rx_buf_size;
		uint32_t free_b = c->rx_buf_size - used;
		tty_printf(
			tty,
			"%3d %-7s %u.%u.%u.%u:%u %u.%u.%u.%u:%u p=%d ar=%u rc=%u tr=%u cw=%u ss=%u if=%u snd_wnd=%u rcv_buf=%u rcv_adv=%u ws=%d/%d ts=%d sack=%d srtt=%u rto=%u rnxt=%u snxt=%u suna=%u rxh=%u rxt=%u rxrdy=%d used=%u ooo=%u txrdy=%d\n",
			i, s, (li >> 24) & 0xff, (li >> 16) & 0xff,
			(li >> 8) & 0xff, li & 0xff, c->local_port,
			(ri >> 24) & 0xff, (ri >> 16) & 0xff, (ri >> 8) & 0xff,
			ri & 0xff, c->remote_port, parent_slot,
			(unsigned)c->accept_ready,
			(unsigned)c->retransmit_count,
			(unsigned)c->total_retrans, (unsigned)c->cwnd,
			(unsigned)c->ssthresh, (unsigned)c->inflight_count,
			(unsigned)c->snd_wnd, (unsigned)c->rx_buf_size,
			(unsigned)free_b, (int)c->rcv_wscale,
			(int)c->snd_wscale, (int)c->ts_enabled, (int)c->sack_ok,
			(unsigned)c->srtt_us, (unsigned)c->rto_us, c->rcv_nxt,
			c->snd_nxt, c->snd_una, (unsigned)c->rx_head,
			(unsigned)c->rx_tail, (int)c->rx_ready, (unsigned)used,
			(unsigned)c->ooo_count, (int)c->tx_ready);
	}

	/* Network-RX / softirq state — to localise a loopback delivery stall:
     *   - an ESTAB conn above with used>0 && rxrdy=0  => recv lost wakeup
     *     (data sits in the ring but sock_recv is asleep).
     *   - rx_q>0 with the NET_RX softirq bit pending and ksoftirqd not
     *     RUNNING => softirq lost wakeup (the packet never reaches tcp_rx).
     * (ksoftirqd_state matches the scheduler dump's State column.) */
	extern uint32_t net_rx_queue_len(uint32_t cpu);
	extern int net_rx_cpu(void);
	extern uint32_t softirq_pending_get(uint32_t cpu);
	extern int ksoftirqd_state_get(uint32_t cpu);
	extern uint32_t percpu_get_online_count(void);
	uint32_t ncpu = percpu_get_online_count();
	if (ncpu > 64)
		ncpu = 64;
	tty_printf(tty, "--- net rx (NET_RX_CPU=%d) ---\n", net_rx_cpu());
	for (uint32_t cpu = 0; cpu < ncpu; cpu++) {
		tty_printf(
			tty,
			"  CPU%u rx_q=%u softirq_pending=0x%x ksoftirqd_state=%d\n",
			cpu, net_rx_queue_len(cpu), softirq_pending_get(cpu),
			ksoftirqd_state_get(cpu));
	}
	tty_printf(tty, "=================\n");
}

// RFC 1191: clamp a connection's effective MSS when an ICMP frag-needed
// arrives carrying the next-hop MTU.
void tcp_handle_pmtu(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
		     uint16_t remote_port, uint16_t new_mtu)
{
	if (new_mtu < 68)
		return;
	uint16_t new_mss = (uint16_t)(new_mtu - sizeof(ipv4_header_t) -
				      sizeof(tcp_header_t));
	if (new_mss < 256)
		new_mss = 256;
	for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
		tcp_conn_t *c = &tcp_connections[i];
		if (!c->active)
			continue;
		if (c->local_port != local_port ||
		    c->remote_port != remote_port)
			continue;
		if (c->local_ip != local_ip || c->remote_ip != remote_ip)
			continue;
		if (c->max_seg_size > new_mss)
			c->max_seg_size = new_mss;
		if (c->peer_mss > new_mss)
			c->peer_mss = new_mss;
	}
}

// RFC 793 §3.5 abort: send RST and tear down connection immediately.
// Used by SO_LINGER l_onoff=1 l_linger=0.
static void tcp_abort_impl(tcp_conn_t *conn, int has_gen, uint32_t gen)
{
	if (!conn)
		return;
	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);
	/* Same recycled-slot identity check as tcp_close_impl — aborting a
	 * recycled conn would RST and FREE an innocent new connection. */
	if (has_gen && (!conn->active || conn->gen != gen)) {
		WARN_RATELIMIT(
			conn->active,
			"tcp: deferred abort hit recycled conn slot (gen %u != %u, state=%d) - stale abort dropped",
			conn->gen, gen, conn->state);
		tcp_lock_release(&conn->lock, flags);
		return;
	}
	uint32_t self_gen = conn->gen; /* free gen-validated (see tcp_close_impl) */
	if (conn->state != TCP_STATE_CLOSED && conn->dev) {
		tcp_send_rst(conn->dev, conn->local_ip, conn->remote_ip,
			     conn->local_port, conn->remote_port, conn->snd_nxt,
			     conn->rcv_nxt);
	}
	conn->state = TCP_STATE_CLOSED;
	tcp_lock_release(&conn->lock, flags);
	tcp_free_conn_gen(conn, self_gen);
}

void tcp_abort(tcp_conn_t *conn)
{
	tcp_abort_impl(conn, 0, 0);
}

/* Gen-validated abort for deferred teardown — see tcp_close_gen. */
void tcp_abort_gen(tcp_conn_t *conn, uint32_t gen)
{
	tcp_abort_impl(conn, 1, gen);
}
