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
#include <kernel/net/stats.h>
#include <kernel/uapi/bug.h>

// Global lock protecting the connection list and publish/unlink operations.
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

// ---------------------------------------------------------------------------
// Connection lifetime: dynamic allocation + reference counting.
//
// Every connection is individually slab-allocated and linked into a single
// global list (g_tcp_conn_list) protected by tcp_lock.  A bare tcp_conn_t* is
// therefore a stable identity: the memory is never recycled into a different
// connection while any reference is held, which eliminates the whole class of
// "slot freed and reused out from under a pointer" races that the old fixed
// array + generation counter existed to paper over.
//
// A reference is held by:
//   * the protocol state machine, while the connection is not terminally dead
//     (the "proto ref", tracked by conn->proto_ref and dropped exactly once);
//   * the owning socket (net_socket_t::tcp);
//   * each accept-queue entry on a listener;
//   * transiently, any lookup (tcp_find_conn_hold) or the timer sweep.
//
// The last put queues the connection for physical free.  Because a connection
// at refcount 0 can never be revived (tcp_conn_tryhold refuses it), the free
// can be deferred to softirq/process context with no generation validation —
// this keeps slab_free (which may fire a TLB-shootdown IPI) out of any
// hard-IRQ path.
// ---------------------------------------------------------------------------

static tcp_conn_t *g_tcp_conn_list; // head of the all-connections list
static uint32_t g_tcp_conn_count; // number of live connections (bounded)

static void tcp_conn_final_free(tcp_conn_t *conn); // forward

// Reap list: connections whose refcount reached 0, awaiting physical free in
// softirq/process context.  Intrusive singly-linked list (chained by
// conn->reap_next) so it needs no fixed capacity.  No generation needed — a
// 0-refcount connection is stable (nothing can re-reference it).
static tcp_conn_t *g_tcp_reap_list;
static spinlock_t tcp_reap_lock = SPINLOCK_INIT("tcp_reap");

// Increment a reference the caller already holds one of.
static inline void tcp_conn_hold(tcp_conn_t *conn)
{
	__atomic_fetch_add(&conn->refcount, 1, __ATOMIC_ACQ_REL);
}

// Increment iff the connection is still referenced (refcount > 0).  MUST be
// called with tcp_lock held so the connection cannot be unlinked/freed under
// the caller: physical free removes the connection from the list under
// tcp_lock, so a list walker holding tcp_lock sees only valid memory, and a
// refcount that has already reached 0 makes this refuse (return 0).
static inline int tcp_conn_tryhold(tcp_conn_t *conn)
{
	int old = __atomic_load_n(&conn->refcount, __ATOMIC_RELAXED);
	while (old > 0) {
		if (__atomic_compare_exchange_n(&conn->refcount, &old, old + 1,
						1, __ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

// Drop a reference.  On reaching 0, queue the connection for physical free.
// Safe from ANY context (never blocks, never slab_frees inline): a 0-refcount
// connection cannot be re-referenced, so deferring the free is race-free.
static void tcp_conn_put(tcp_conn_t *conn)
{
	if (!conn)
		return;
	if (__atomic_sub_fetch(&conn->refcount, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	uint64_t flags;
	spin_lock_irqsave(&tcp_reap_lock, &flags);
	if (!conn->on_reap_queue) {
		conn->on_reap_queue = 1;
		conn->reap_next = g_tcp_reap_list;
		g_tcp_reap_list = conn;
	}
	spin_unlock_irqrestore(&tcp_reap_lock, flags);
	// Wake ksoftirqd to drain the reap list in process/softirq context.
	softirq_raise(SOFTIRQ_TIMER);
}

// Public reference-count wrappers for the socket layer (which cannot see the
// static primitives).  tcp_conn_get adds the owning socket's reference;
// tcp_conn_release drops it.
void tcp_conn_get(tcp_conn_t *conn)
{
	tcp_conn_hold(conn);
}

void tcp_conn_release(tcp_conn_t *conn)
{
	tcp_conn_put(conn);
}

// Softirq handler bound to SOFTIRQ_TIMER (registered in tcp_init).  Runs in
// process / ksoftirqd context with IRQs enabled, so slab_free (which can fire
// a TLB-shootdown IPI) is safe here.
static void tcp_pending_softirq(void)
{
	tcp_reap_pending();
}

// Drain the reap list: unlink each 0-refcount connection from the global list
// and physically free it.  Process/softirq context only (slab_free).
void tcp_reap_pending(void)
{
	for (;;) {
		tcp_conn_t *conn = NULL;
		uint64_t flags;
		spin_lock_irqsave(&tcp_reap_lock, &flags);
		if (g_tcp_reap_list) {
			conn = g_tcp_reap_list;
			g_tcp_reap_list = conn->reap_next;
			conn->reap_next = NULL;
		}
		spin_unlock_irqrestore(&tcp_reap_lock, flags);
		if (!conn)
			break;
		tcp_conn_final_free(conn);
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

/* Adaptive TCP receive buffer auto-tuning.
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

	NET_STATS_INC(NET_MIB_TCP_OUTSEGS);
	if (flags & TCP_RST)
		NET_STATS_INC(NET_MIB_TCP_OUTRSTS);
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

// Forward decl: drop the protocol self-reference exactly once when a
// connection becomes terminally dead.
static void tcp_conn_kill(tcp_conn_t *conn);

// Fail a connection: record the error, wake any waiters, and drop the
// protocol self-reference.  The owning socket's reference (if any) keeps the
// connection alive so it can still read SO_ERROR; the connection is freed once
// that reference is also dropped.  Called under conn->lock.
static void tcp_fail_connection(tcp_conn_t *conn, int error)
{
	conn->error = error;
	conn->connect_done = 1;
	conn->rx_ready = 1;
	conn->tx_ready = 1;
	conn->inflight_count = 0;
	conn->last_rx_tick = timer_ticks();
	tcp_conn_kill(conn); // sets state=CLOSED + drops the protocol reference
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
	g_tcp_conn_list = NULL;
	g_tcp_conn_count = 0;
	// Generate ISN and SYN cookie secrets from CSPRNG
	random_get_bytes(tcp_isn_secret, sizeof(tcp_isn_secret), 0);
	random_get_bytes(tcp_syncookie_secret, sizeof(tcp_syncookie_secret), 0);
	// Bind the deferred-free drain to a softirq vector so the timer
	// can hand work off to ksoftirqd from hard-IRQ context.
	softirq_register(SOFTIRQ_TIMER, tcp_pending_softirq);
	// The per-connection timer sweep runs in softirq context (raised once
	// per tick by the lowest online CPU) rather than in the hard-IRQ timer
	// handler on every CPU: softirq context has interrupts enabled, so the
	// sweep may block on conn->lock and slab_free directly.
	softirq_register(SOFTIRQ_TCP_TIMER, tcp_timer_tick);
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
// Allocate the de-inlined per-connection inflight + ooo arrays.  Both are
// slab-allocated by the caller OUTSIDE tcp_lock (same TLB-shootdown-safety
// discipline as the rx/tx ring buffers).  Returns 0 with both pointers set on
// success, or -1 with neither leaked on failure.
static int tcp_alloc_seg_arrays(tcp_inflight_segment_t **inflight_out,
				tcp_ooo_segment_t **ooo_out)
{
	tcp_inflight_segment_t *inflight = (tcp_inflight_segment_t *)slab_alloc(
		sizeof(tcp_inflight_segment_t) * TCP_MAX_INFLIGHT);
	tcp_ooo_segment_t *ooo = (tcp_ooo_segment_t *)slab_alloc(
		sizeof(tcp_ooo_segment_t) * TCP_MAX_OOO);
	if (!inflight || !ooo) {
		if (inflight)
			slab_free(inflight);
		if (ooo)
			slab_free(ooo);
		return -1;
	}
	*inflight_out = inflight;
	*ooo_out = ooo;
	return 0;
}

static inline void tcp_free_seg_arrays(tcp_inflight_segment_t *inflight,
				       tcp_ooo_segment_t *ooo)
{
	if (inflight)
		slab_free(inflight);
	if (ooo)
		slab_free(ooo);
}

// On allocation failure (table full) returns NULL; caller must slab_free
// the buffers it pre-allocated.
//
// rx_buf/tx_buf and the inflight/ooo arrays are all allocated by the caller
// OUTSIDE tcp_lock (slab_alloc may fire a TLB-shootdown IPI, illegal while
// holding tcp_lock IRQs-off) and adopted here.  The inflight/ooo arrays used
// to be inlined in the connection block (~280 KB/conn); pointing at
// caller-allocated arrays shrinks the block enough to allocate dynamically.
static tcp_conn_t *tcp_conn_alloc(uint8_t *rx_buf, uint8_t *tx_buf,
				  tcp_inflight_segment_t *inflight,
				  tcp_ooo_segment_t *ooo)
{
	BUG_ON(rx_buf == NULL);
	BUG_ON(tx_buf == NULL);
	BUG_ON(inflight == NULL);
	BUG_ON(ooo == NULL);

	// There is no fixed connection cap: the number of live connections is
	// bounded only by available memory (slab_alloc returns NULL on
	// exhaustion) and, above this layer, by the number of open sockets.
	tcp_conn_t *conn = (tcp_conn_t *)slab_alloc(sizeof(*conn));
	if (!conn)
		return NULL;
	for (size_t b = 0; b < sizeof(*conn); b++)
		((uint8_t *)conn)[b] = 0;
	__atomic_fetch_add(&g_tcp_conn_count, 1, __ATOMIC_ACQ_REL);

	conn->lock = (spinlock_t)SPINLOCK_INIT("tcp_conn");
	conn->state = TCP_STATE_CLOSED;
	conn->tx_ready = 1;
	conn->peer_mss = TCP_MSS;
	conn->max_seg_size = TCP_MSS;
	conn->owner_socket = NULL;

	// Reference counting: one protocol self-reference, held until the
	// connection becomes terminally dead.
	conn->refcount = 1;
	conn->proto_ref = 1;
	conn->on_reap_queue = 0;
	conn->list_next = NULL;

	// RFC 6298 initial RTO (no measurement yet)
	conn->rto_us = TCP_RTO_INITIAL_US;

	// RFC 5681 NewReno: cwnd starts at 10 segments (RFC 6928 IW10)
	conn->cwnd = 10;
	conn->ssthresh = 0xFFFFFFFFu;

	/* Disable Nagle by default (see the Nagle-deadlock note in the design). */
	conn->nodelay = 1;
	conn->keepidle_ticks = 7200 * 100; // 2 hours
	conn->keepintvl_ticks = 75 * 100; // 75 s
	conn->keepcnt = 9;
	conn->last_rx_tick = timer_ticks();

	// RFC 7323 — we always offer window scale 7 (128x); cleared if the peer
	// does not negotiate it.
	conn->rcv_wscale = 7;

	// Adopt pre-allocated RX/TX buffers + de-inlined segment arrays.
	conn->rx_buf = rx_buf;
	conn->tx_buf = tx_buf;
	conn->inflight = inflight;
	conn->ooo = ooo;
	conn->rx_buf_size = TCP_RX_BUF_SIZE;
	conn->tx_buf_size = TCP_TX_BUF_SIZE;

	// Not yet linked/published — caller writes the 4-tuple then calls
	// tcp_publish_conn() under tcp_lock.
	return conn;
}

// Free a connection that was allocated but never published (a lost duplicate-
// conn race, or a post-alloc failure).  It is not on the global list and has
// no references beyond the birth reference, so free directly.
static void tcp_conn_free_unpublished(tcp_conn_t *conn)
{
	if (!conn)
		return;
	if (conn->rx_buf)
		slab_free(conn->rx_buf);
	if (conn->tx_buf)
		slab_free(conn->tx_buf);
	if (conn->inflight)
		slab_free(conn->inflight);
	if (conn->ooo)
		slab_free(conn->ooo);
	slab_free(conn);
	__atomic_fetch_sub(&g_tcp_conn_count, 1, __ATOMIC_ACQ_REL);
}

// Publish a freshly-allocated conn after the caller has written the 4-tuple:
// link it into the global connection list and mark it active.  MUST be called
// with tcp_lock held so the link + the 4-tuple are visible atomically to
// lock-holding walkers.  The compiler barrier keeps the active=1 store from
// being reordered before the 4-tuple/link stores.
static inline void tcp_publish_conn(tcp_conn_t *conn)
{
	conn->list_next = g_tcp_conn_list;
	g_tcp_conn_list = conn;
	__asm__ volatile("" ::: "memory");
	conn->active = 1;
}

// Physically free a connection whose refcount has reached 0: unlink it from
// the global list and release all its memory.  Process/softirq context only
// (slab_free may fire a TLB-shootdown IPI).  A 0-refcount connection is
// stable — nothing can re-reference it — so no further validation is needed.
static void tcp_conn_final_free(tcp_conn_t *conn)
{
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	tcp_conn_t **pp = &g_tcp_conn_list;
	while (*pp) {
		if (*pp == conn) {
			*pp = conn->list_next;
			break;
		}
		pp = &(*pp)->list_next;
	}
	conn->active = 0;
	spin_unlock_irqrestore(&tcp_lock, flags);

	if (conn->rx_buf)
		slab_free(conn->rx_buf);
	if (conn->tx_buf)
		slab_free(conn->tx_buf);
	if (conn->inflight)
		slab_free(conn->inflight);
	if (conn->ooo)
		slab_free(conn->ooo);
	slab_free(conn);
	__atomic_fetch_sub(&g_tcp_conn_count, 1, __ATOMIC_ACQ_REL);
}

// Drive a connection to a terminally-dead state and drop its protocol self-
// reference exactly once.  Safe to call under conn->lock: tcp_conn_put only
// decrements the refcount and, on reaching 0, queues the physical free for
// softirq context — it never blocks or slab_frees inline.  Idempotent.
static void tcp_conn_kill(tcp_conn_t *conn)
{
	conn->state = TCP_STATE_CLOSED;
	if (conn->proto_ref) {
		conn->proto_ref = 0;
		tcp_conn_put(conn);
	}
}

// Recover capacity when the live-connection count is at its cap by killing
// TIME_WAIT connections (they hold only their protocol reference, so killing
// them drops them to 0 and frees them).  Returns the number killed.
static int tcp_reap_time_wait_slots(void)
{
	int reaped = 0;
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
		if (c->active && c->state == TCP_STATE_TIME_WAIT &&
		    c->proto_ref) {
			c->proto_ref = 0;
			c->state = TCP_STATE_CLOSED;
			tcp_conn_put(c); // drop protocol ref → reaped when it hits 0
			reaped++;
		}
	}
	spin_unlock_irqrestore(&tcp_lock, flags);
	return reaped;
}

// Clear the parent back-pointer of every child of a closing listener.  parent
// is a weak reference (no refcount), so this is just a pointer clear.
static void tcp_detach_listener_children(tcp_conn_t *listener)
{
	if (!listener)
		return;
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
		if (c->active && c->parent == listener)
			c->parent = NULL;
	}
	spin_unlock_irqrestore(&tcp_lock, flags);
}

// Find a connection by 4-tuple.  Caller MUST hold tcp_lock; the returned
// pointer is only valid while tcp_lock is held (no reference is taken).  Used
// for existence checks under the lock (e.g. the duplicate-connection guard).
static tcp_conn_t *tcp_find_conn_locked(uint32_t local_ip, uint16_t local_port,
					uint32_t remote_ip, uint16_t remote_port)
{
	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
		// Skip LISTEN (separate lookup) and CLOSED (dead — a killed
		// TIME_WAIT awaiting reap, or a peer-reset connection kept only
		// for the owning socket's SO_ERROR; neither should demux a
		// packet).
		if (c->active && c->state != TCP_STATE_LISTEN &&
		    c->state != TCP_STATE_CLOSED &&
		    c->local_port == local_port &&
		    c->remote_port == remote_port &&
		    (c->local_ip == local_ip || c->local_ip == 0) &&
		    c->remote_ip == remote_ip) {
			return c;
		}
	}
	return NULL;
}

// Find a connection by 4-tuple and take a reference on it, so the caller can
// use it after dropping tcp_lock.  Caller MUST tcp_conn_put() the result.
static tcp_conn_t *tcp_find_conn_hold(uint32_t local_ip, uint16_t local_port,
				      uint32_t remote_ip, uint16_t remote_port)
{
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	tcp_conn_t *c = tcp_find_conn_locked(local_ip, local_port, remote_ip,
					     remote_port);
	if (c && !tcp_conn_tryhold(c))
		c = NULL;
	spin_unlock_irqrestore(&tcp_lock, flags);
	return c;
}

// Find a listening connection on a port.  Caller MUST hold tcp_lock; the
// returned pointer is valid only while tcp_lock is held (no reference taken).
//
// When only one listener exists for the port (common case) returns it
// immediately.  When multiple listeners share the same port distributes
// incoming SYNs across them by picking the one with the fewest pending
// connections (accept-queue depth + SYN_RECEIVED children not yet enqueued),
// so connections do not all pile up on one listener and starve the others.
static tcp_conn_t *tcp_find_listener_locked(uint32_t local_ip,
					    uint16_t local_port)
{
	// First pass: collect candidate counts.
	tcp_conn_t *first_exact = NULL;
	tcp_conn_t *first_wildcard = NULL;
	int exact_count = 0;
	int wildcard_count = 0;

	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
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

	// Fast path: single listener.
	if (exact_count == 1)
		return first_exact;
	if (exact_count == 0 && wildcard_count <= 1)
		return first_wildcard;
	if (exact_count == 0 && wildcard_count == 0)
		return NULL;

	// Multiple listeners on the same port.  Second pass: select the one
	// with the smallest load (accept-queue depth + SYN_RECEIVED children
	// in flight).
	tcp_conn_t *best_exact = NULL;
	tcp_conn_t *best_wildcard = NULL;
	int best_exact_load = 0x7fffffff;
	int best_wildcard_load = 0x7fffffff;

	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
		if (!c->active || c->state != TCP_STATE_LISTEN ||
		    c->local_port != local_port)
			continue;

		int load = (int)((c->accept_tail - c->accept_head + 16u) % 16u);
		for (tcp_conn_t *ch = g_tcp_conn_list; ch; ch = ch->list_next) {
			if (ch->active &&
			    ch->state == TCP_STATE_SYN_RECEIVED &&
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

// Find a listener and take a reference on it, so the caller can use it after
// dropping tcp_lock.  Caller MUST tcp_conn_put() the result.
static tcp_conn_t *tcp_find_listener_hold(uint32_t local_ip,
					  uint16_t local_port)
{
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	tcp_conn_t *c = tcp_find_listener_locked(local_ip, local_port);
	if (c && !tcp_conn_tryhold(c))
		c = NULL;
	spin_unlock_irqrestore(&tcp_lock, flags);
	return c;
}

// Does any listener exist for this local endpoint?  Does its own locking, so
// it is safe to call while holding a conn->lock (lock order conn->lock →
// tcp_lock is respected — nothing acquires them the other way round).
static int tcp_listener_exists(uint32_t local_ip, uint16_t local_port)
{
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	int found = tcp_find_listener_locked(local_ip, local_port) != NULL;
	spin_unlock_irqrestore(&tcp_lock, flags);
	return found;
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
	tcp_inflight_segment_t *new_if = NULL;
	tcp_ooo_segment_t *new_ooo = NULL;
	if (!new_rx || !new_tx ||
	    tcp_alloc_seg_arrays(&new_if, &new_ooo) != 0) {
		WARN_RATELIMIT(
			1, "tcp_connect: conn buffer alloc failed (rx=%d tx=%d)",
			new_rx != NULL, new_tx != NULL);
		if (new_rx)
			slab_free(new_rx);
		if (new_tx)
			slab_free(new_tx);
		return NULL;
	}

	// Allocate the connection object OUTSIDE tcp_lock (slab_alloc may fire a
	// TLB-shootdown IPI).  On success it adopts the buffers above.
	tcp_conn_t *conn = tcp_conn_alloc(new_rx, new_tx, new_if, new_ooo);
	if (!conn) {
		// At the live-connection cap.  Reap TIME_WAIT connections to
		// recover capacity (RFC 6191), drain the reap queue, and retry.
		tcp_reap_time_wait_slots();
		tcp_reap_pending();
		conn = tcp_conn_alloc(new_rx, new_tx, new_if, new_ooo);
	}
	if (!conn) {
		NET_STATS_INC(NET_MIB_TCP_CONNTABLEFULL);
		WARN_RATELIMIT(
			1, "tcp_connect: connection cap reached even after reap");
		slab_free(new_rx);
		slab_free(new_tx);
		tcp_free_seg_arrays(new_if, new_ooo);
		return NULL;
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
	NET_STATS_INC(NET_MIB_TCP_ACTIVEOPENS);

	// Take the reference the caller (sock_connect) will own as s->tcp.  The
	// connection is returned with refcount 2: the protocol self-reference
	// plus this socket reference.
	tcp_conn_hold(conn);

	spin_lock_irqsave(&tcp_lock, &flags);
	// RFC 6191: a fresh connection may reuse a TIME_WAIT 4-tuple.  Kill any
	// TIME_WAIT connection with the same 4-tuple (mark it CLOSED so it is no
	// longer found) before publishing this one, so demux resolves this
	// 4-tuple to the new connection.
	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
		if (c->active && c->state == TCP_STATE_TIME_WAIT &&
		    c->proto_ref && c->local_port == src_port &&
		    c->remote_port == dst_port && c->local_ip == local_ip &&
		    c->remote_ip == dst_ip) {
			c->proto_ref = 0;
			c->state = TCP_STATE_CLOSED;
			tcp_conn_put(c);
		}
	}
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
	tcp_inflight_segment_t *new_if = NULL;
	tcp_ooo_segment_t *new_ooo = NULL;
	if (!new_rx || !new_tx ||
	    tcp_alloc_seg_arrays(&new_if, &new_ooo) != 0) {
		WARN_RATELIMIT(
			1, "tcp_listen: conn buffer alloc failed (rx=%d tx=%d)",
			new_rx != NULL, new_tx != NULL);
		if (new_rx)
			slab_free(new_rx);
		if (new_tx)
			slab_free(new_tx);
		return NULL;
	}

	// Allocate the connection object OUTSIDE tcp_lock (slab_alloc safety).
	tcp_conn_t *conn = tcp_conn_alloc(new_rx, new_tx, new_if, new_ooo);
	if (!conn) {
		tcp_reap_time_wait_slots();
		tcp_reap_pending();
		conn = tcp_conn_alloc(new_rx, new_tx, new_if, new_ooo);
	}
	if (!conn) {
		NET_STATS_INC(NET_MIB_TCP_CONNTABLEFULL);
		WARN_RATELIMIT(
			1, "tcp_listen: connection cap reached even after reap");
		slab_free(new_rx);
		slab_free(new_tx);
		tcp_free_seg_arrays(new_if, new_ooo);
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

	// Reference the caller (sock_listen) will own as s->tcp (refcount 2:
	// protocol self-reference + socket reference).
	tcp_conn_hold(conn);

	spin_lock_irqsave(&tcp_lock, &flags);
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

	// Each accept-queue entry holds a reference on its child, so the child's
	// memory is guaranteed valid here (no recycle possible).  The entry's
	// reference transfers to the caller (becomes the accepted socket's
	// reference) when we return the child; if the child died before accept()
	// (peer reset → CLOSED), we drop the entry's reference and discard it.
	while (listener->accept_head != listener->accept_tail) {
		struct tcp_accept_entry e =
			listener->accept_queue[listener->accept_head];
		listener->accept_queue[listener->accept_head].conn = NULL;
		listener->accept_head = (listener->accept_head + 1) % 16;
		if (listener->accept_head == listener->accept_tail)
			listener->accept_ready = 0;

		tcp_conn_t *conn = e.conn;
		if (!conn)
			continue;
		if (conn->state != TCP_STATE_ESTABLISHED &&
		    conn->state != TCP_STATE_CLOSE_WAIT) {
			// Child died between queueing and accept — drop the
			// entry's reference and move on.
			tcp_lock_release(&listener->lock, flags);
			tcp_conn_put(conn);
			tcp_lock_acquire(&listener->lock, &flags);
			continue;
		}

		conn->parent = NULL;
		tcp_lock_release(&listener->lock, flags);
		return conn; // entry reference transfers to the caller
	}

	tcp_lock_release(&listener->lock, flags);

	// Fallback: a child that reached an accept-ready state but is not on the
	// explicit accept queue (its parent link was cleared when the original
	// listener slot could not be validated at enqueue time).  Recover it by
	// 4-tuple.  Take a reference before returning so the caller owns one.
	uint64_t tcp_flags;
	spin_lock_irqsave(&tcp_lock, &tcp_flags);
	for (tcp_conn_t *conn = g_tcp_conn_list; conn; conn = conn->list_next) {
		if (!conn->active)
			continue;
		if (conn->state != TCP_STATE_ESTABLISHED &&
		    conn->state != TCP_STATE_CLOSE_WAIT)
			continue;

		int matches = 0;
		if (conn->parent == listener) {
			matches = 1;
		} else if (conn->parent == NULL && conn->owner_socket == NULL &&
			   conn->local_port == listener->local_port &&
			   conn->remote_port != 0 &&
			   (listener->local_ip == 0 ||
			    conn->local_ip == listener->local_ip)) {
			// Unowned (owner_socket==NULL) child on this listener's
			// port: not yet claimed by any accept().
			matches = 1;
		}
		if (!matches)
			continue;

		if (!tcp_conn_tryhold(conn))
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
// Drive a connection toward teardown.  The caller must hold a reference on
// `conn` (the socket reference), so the connection is guaranteed alive here —
// no identity/generation check is needed.  This never frees the connection;
// it only drives the state machine (and, for states with no closing handshake,
// drops the protocol self-reference via tcp_conn_kill).  The caller drops the
// socket reference separately; physical free happens when the refcount reaches
// zero.
int tcp_close(tcp_conn_t *conn)
{
	if (!conn)
		return -1;

	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);

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

	case TCP_STATE_LISTEN: {
		// Drain the accept queue, dropping the reference each entry holds
		// on its unaccepted child.  Collect them under the listener's
		// lock, then put them after releasing it (put may queue a free).
		tcp_conn_t *pending[16];
		int npending = 0;
		while (conn->accept_head != conn->accept_tail) {
			tcp_conn_t *child =
				conn->accept_queue[conn->accept_head].conn;
			conn->accept_queue[conn->accept_head].conn = NULL;
			conn->accept_head = (conn->accept_head + 1) % 16;
			if (child && npending < 16)
				pending[npending++] = child;
		}
		conn->accept_ready = 0;
		tcp_lock_release(&conn->lock, flags);
		for (int pi = 0; pi < npending; pi++)
			tcp_conn_put(pending[pi]);

		tcp_detach_listener_children(conn);
		tcp_lock_acquire(&conn->lock, &flags);
		// No closing handshake for a listener: drop the protocol
		// self-reference now.  When the caller also drops the socket
		// reference, the connection is freed.
		tcp_conn_kill(conn);
		tcp_lock_release(&conn->lock, flags);
		break;
	}

	case TCP_STATE_SYN_SENT:
		tcp_conn_kill(conn);
		tcp_lock_release(&conn->lock, flags);
		break;

	case TCP_STATE_CLOSED:
		// Already terminally dead (peer RST / timeout).  The protocol
		// self-reference was already dropped; nothing to do here — the
		// caller's socket-reference drop frees it.
		tcp_lock_release(&conn->lock, flags);
		break;

	default:
		// A closing state (FIN_WAIT_*, CLOSING, LAST_ACK, TIME_WAIT):
		// the handshake is already in progress and the protocol
		// self-reference keeps the connection alive until it completes.
		tcp_lock_release(&conn->lock, flags);
		break;
	}

	// Opportunistic drain of the reap queue (process context — slab_free ok).
	tcp_reap_pending();
	return 0;
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
	// A closed peer window (snd_wnd == 0) blocks new data: the zero-window
	// persist timer (armed below, fired in tcp_timer_tick) probes instead
	// of sending data the peer would drop.  RFC 9293 §3.8.6.
	uint32_t window_bytes = conn->snd_wnd;
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
		// Arm the zero-window persist timer if the block is a closed peer
		// window with nothing in flight: the retransmit timer only probes
		// when there is inflight data, so without this a receiver that
		// advertised window 0 and then lost its re-open ACK would stall
		// this sender forever.  RFC 1122 §4.2.2.17.
		if (conn->snd_wnd == 0 && conn->inflight_count == 0 &&
		    conn->persist_tick == 0) {
			conn->persist_backoff = 0;
			conn->persist_tick =
				timer_ticks() + TCP_PERSIST_MIN_TICKS;
		}
		tcp_lock_release(&conn->lock, flags);
		return 0;
	}

	// Data went out — no window stall, so disarm any persist probe.
	conn->persist_tick = 0;
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

// Disposition of an inbound segment as decided by tcp_validate_incoming().
typedef enum {
	TCP_SEG_OK, // passed validation; continue to the state machine
	TCP_SEG_DROP, // discard silently
	TCP_SEG_DROP_ACK, // discard, but emit a current-state ACK
	TCP_SEG_RESET, // a valid reset at rcv_nxt; fail the connection
} tcp_seg_verdict_t;

// Central inbound-segment validation, run exactly once before the per-state
// switch for every fully-synchronized data-transfer state (ESTABLISHED,
// FIN_WAIT_1/2, CLOSE_WAIT, CLOSING, LAST_ACK).  It unifies three checks that
// were previously scattered across individual states, duplicated, or missing
// entirely (CLOSE_WAIT had none): reset processing (RFC 5961 §3), blind-SYN
// mitigation (RFC 5961 §4), and timestamp/PAWS validation (RFC 7323 §5.3).
//
// It performs NO transmit and mutates only ts_recent on an accepted in-window
// segment, preserving the deferred-ACK discipline: the caller emits any owed
// ACK after releasing conn->lock.  TIME_WAIT and the handshake states
// (SYN_SENT/SYN_RECEIVED) keep their own bespoke validation and are not
// routed here.
static tcp_seg_verdict_t tcp_validate_incoming(tcp_conn_t *conn, uint8_t flags,
					       uint32_t seq, uint32_t ack,
					       uint16_t payload_len,
					       const tcp_parsed_opts_t *pop)
{
	(void)ack;
	int in_window =
		(int32_t)(seq - conn->rcv_nxt) >= 0 &&
		(int32_t)(seq - (conn->rcv_nxt + conn->rcv_wnd)) <= 0;

	// RFC 5961 §3 — reset processing.  Only a reset landing exactly at
	// rcv_nxt is honored; an in-window-but-not-exact reset gets a
	// challenge ACK (a blind-reset probe cannot then tear us down), and an
	// out-of-window reset is ignored.
	if (flags & TCP_RST) {
		if (!in_window)
			return TCP_SEG_DROP;
		if (seq == conn->rcv_nxt) {
			if (conn->inflight_count > 0 || conn->rx_ready)
				NET_STATS_INC(NET_MIB_TCP_RSTDATALOSS);
			NET_STATS_INC(NET_MIB_TCP_ESTABRESETS);
			WARN_RATELIMIT(
				conn->inflight_count > 0 || conn->rx_ready,
				"tcp_rx: in-window RST aborted :%u->:%u mid-transfer (inflight=%u rx_ready=%d) - data lost",
				conn->local_port, conn->remote_port,
				conn->inflight_count, conn->rx_ready);
			return TCP_SEG_RESET;
		}
		NET_STATS_INC(NET_MIB_TCP_CHALLENGEACK);
		return TCP_SEG_DROP_ACK;
	}

	// RFC 5961 §4 — a SYN inside the window on an already-synchronized
	// connection is answered with a challenge ACK, never an abort.  (True
	// simultaneous open is handled only in SYN_SENT.)
	if (flags & TCP_SYN) {
		if (in_window) {
			NET_STATS_INC(NET_MIB_TCP_CHALLENGEACK);
			return TCP_SEG_DROP_ACK;
		}
		return TCP_SEG_DROP;
	}

	// RFC 7323 §5.3 — protection against wrapped sequence numbers.  Applied
	// to every synchronized state now, not just ESTABLISHED.
	if (conn->ts_enabled && pop->ts_present) {
		if ((int32_t)(pop->tsval - conn->ts_recent) < 0 &&
		    payload_len > 0) {
			NET_STATS_INC(NET_MIB_TCP_PAWSDROP);
			return TCP_SEG_DROP_ACK;
		}
		if ((int32_t)(seq - conn->rcv_nxt) <= 0 &&
		    (int32_t)(pop->tsval - conn->ts_recent) >= 0) {
			conn->ts_recent = pop->tsval;
			conn->ts_recent_age = (uint32_t)timer_ticks();
		}
	}

	return TCP_SEG_OK;
}

static inline int tcp_state_is_synchronized(int state)
{
	return state == TCP_STATE_ESTABLISHED ||
	       state == TCP_STATE_FIN_WAIT_1 || state == TCP_STATE_FIN_WAIT_2 ||
	       state == TCP_STATE_CLOSE_WAIT || state == TCP_STATE_CLOSING ||
	       state == TCP_STATE_LAST_ACK;
}

void tcp_rx(net_device_t *dev, uint32_t src_ip, uint32_t dst_ip,
	    const uint8_t *data, uint16_t len)
{
	if (len < sizeof(tcp_header_t)) {
		NET_STATS_INC(NET_MIB_TCP_INERRS);
		return;
	}
	NET_STATS_INC(NET_MIB_TCP_INSEGS);

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

	// Find the existing connection and take a reference on it, so it cannot
	// be freed under us while we process the segment.  Released at every
	// exit of the segment-processing path below (search: tcp_conn_put(conn)).
	tcp_conn_t *conn =
		tcp_find_conn_hold(local_ip, dst_port, src_ip, src_port);

	// RFC 1122 §4.2.2.13: a fresh SYN whose 4-tuple matches an existing
	// TIME_WAIT connection re-opens the connection.  Kill the TIME_WAIT
	// connection and fall through to the listener path so the SYN is
	// processed as a brand-new connection.
	if (conn && conn->state == TCP_STATE_TIME_WAIT &&
	    (tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK)) {
		uint64_t twflags;
		tcp_lock_acquire(&conn->lock, &twflags);
		tcp_conn_kill(conn);
		tcp_lock_release(&conn->lock, twflags);
		tcp_conn_put(conn); // drop our transient reference
		conn = NULL;
	}

	if (!conn) {
		// Check for a listener.  Hold a reference so it cannot be freed
		// (by a concurrent sock_close on the listening socket) while we
		// use it to create a child below.  Every exit of this block
		// jumps to rx_no_conn_done, which drops this reference.
		tcp_conn_t *listener =
			tcp_find_listener_hold(local_ip, dst_port);
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
				goto rx_no_conn_done;
			}

			// New connection on listening socket.
			// Pre-allocate buffers BEFORE taking tcp_lock — slab_alloc may
			// trigger a TLB shootdown IPI that would deadlock against
			// tcp_lock holders.
			uint8_t *nc_rx = (uint8_t *)slab_alloc(TCP_RX_BUF_SIZE);
			uint8_t *nc_tx = (uint8_t *)slab_alloc(TCP_TX_BUF_SIZE);
			tcp_inflight_segment_t *nc_if = NULL;
			tcp_ooo_segment_t *nc_ooo = NULL;
			if (!nc_rx || !nc_tx ||
			    tcp_alloc_seg_arrays(&nc_if, &nc_ooo) != 0) {
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
				goto rx_no_conn_done;
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
			if (tcp_find_conn_locked(local_ip, dst_port, src_ip,
					  src_port)) {
				spin_unlock_irqrestore(&tcp_lock, flags);
				slab_free(nc_rx);
				slab_free(nc_tx);
				tcp_free_seg_arrays(nc_if, nc_ooo);
				goto rx_no_conn_done;
			}

			tcp_conn_t *new_conn =
				tcp_conn_alloc(nc_rx, nc_tx, nc_if, nc_ooo);
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
				new_conn = tcp_conn_alloc(nc_rx, nc_tx, nc_if,
							  nc_ooo);
			}
			if (!new_conn) {
				spin_unlock_irqrestore(&tcp_lock, flags);
				slab_free(nc_rx);
				slab_free(nc_tx);
				tcp_free_seg_arrays(nc_if, nc_ooo);
				NET_STATS_INC(NET_MIB_TCP_CONNTABLEFULL);
				// Fallback to SYN cookie
				uint32_t cookie = tcp_syncookie_generate(
					src_ip, local_ip, src_port, dst_port,
					seq, TCP_MSS);
				tcp_send_segment(dev, local_ip, src_ip,
						 dst_port, src_port, cookie,
						 seq + 1, TCP_SYN | TCP_ACK,
						 TCP_WINDOW_SIZE, NULL, 0);
				goto rx_no_conn_done;
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
			NET_STATS_INC(NET_MIB_TCP_PASSIVEOPENS);
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
			goto rx_no_conn_done;
		}

		// SYN cookie validation: ACK for unknown connection with a listener
		if (listener && (tcp_flags & TCP_ACK) &&
		    !(tcp_flags & TCP_SYN)) {
			uint32_t cookie =
				ack -
				1; // The ISN we sent was cookie, client ACKs cookie+1
			uint16_t cookie_mss = 0;
			int cookie_ok = tcp_syncookie_validate(
				src_ip, local_ip, src_port, dst_port, cookie,
				&cookie_mss);
			NET_STATS_INC(cookie_ok ? NET_MIB_TCP_SYNCOOKIERECV :
						  NET_MIB_TCP_SYNCOOKIEFAIL);
			if (cookie_ok) {
				// Valid SYN cookie - create connection directly in ESTABLISHED state.
				// Pre-allocate buffers BEFORE taking tcp_lock (TLB-shootdown safety).
				uint8_t *cc_rx =
					(uint8_t *)slab_alloc(TCP_RX_BUF_SIZE);
				uint8_t *cc_tx =
					(uint8_t *)slab_alloc(TCP_TX_BUF_SIZE);
				tcp_inflight_segment_t *cc_if = NULL;
				tcp_ooo_segment_t *cc_ooo = NULL;
				if (!cc_rx || !cc_tx ||
				    tcp_alloc_seg_arrays(&cc_if, &cc_ooo) != 0) {
					if (cc_rx)
						slab_free(cc_rx);
					if (cc_tx)
						slab_free(cc_tx);
					goto rx_no_conn_done;
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
				if (tcp_find_conn_locked(local_ip, dst_port, src_ip,
						  src_port)) {
					spin_unlock_irqrestore(&tcp_lock, flags);
					slab_free(cc_rx);
					slab_free(cc_tx);
					tcp_free_seg_arrays(cc_if, cc_ooo);
					goto rx_no_conn_done;
				}

				tcp_conn_t *new_conn = tcp_conn_alloc(
					cc_rx, cc_tx, cc_if, cc_ooo);
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
					new_conn = tcp_conn_alloc(cc_rx, cc_tx,
								  cc_if, cc_ooo);
				}
				if (!new_conn) {
					spin_unlock_irqrestore(&tcp_lock,
							       flags);
					slab_free(cc_rx);
					slab_free(cc_tx);
					tcp_free_seg_arrays(cc_if, cc_ooo);
					NET_STATS_INC(NET_MIB_TCP_CONNTABLEFULL);
					goto rx_no_conn_done;
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
				NET_STATS_INC(NET_MIB_TCP_PASSIVEOPENS);
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
						// The accept-queue entry holds a
						// reference on the child so it
						// cannot be freed before accept()
						// claims it.
						tcp_conn_hold(new_conn);
						listener->accept_queue
							[listener->accept_tail]
								.conn = new_conn;
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
				goto rx_no_conn_done;
			}
		}

		// No connection - send RST (RFC 793: a segment to a nonexistent
		// connection elicits an RST so the peer stops).
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
		goto rx_no_conn_done;

	rx_no_conn_done:
		// Single exit for the no-existing-connection path: drop the
		// listener reference (NULL-safe) and return.
		tcp_conn_put(listener);
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
		tcp_conn_put(conn);
		return;
	}

	// Central inbound validation for the fully-synchronized states
	// (RST / blind-SYN / PAWS in one place — see tcp_validate_incoming).
	// The handshake states and TIME_WAIT keep their own handling below.
	if (tcp_state_is_synchronized(conn->state)) {
		switch (tcp_validate_incoming(conn, tcp_flags, seq, ack,
					      payload_len, &pop)) {
		case TCP_SEG_DROP:
			tcp_lock_release(&conn->lock, flags);
			tcp_conn_put(conn);
			return;
		case TCP_SEG_DROP_ACK:
			tcp_queue_ack_locked(conn);
			ack_pending = 1;
			goto deferred_ack_out;
		case TCP_SEG_RESET:
			tcp_fail_connection(conn, ECONNRESET);
			tcp_lock_release(&conn->lock, flags);
			tcp_conn_put(conn);
			return;
		case TCP_SEG_OK:
			break;
		}
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
			NET_STATS_INC(NET_MIB_TCP_ATTEMPTFAILS);
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
			tcp_conn_put(conn);
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
							// The accept-queue entry
							// holds a reference on the
							// child (see tcp_accept).
							tcp_conn_hold(conn);
							p->accept_queue
								[p->accept_tail]
									.conn =
								conn;
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
							NET_STATS_INC(
								NET_MIB_TCP_ACCEPTQFULL);
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
				} else if (!tcp_listener_exists(
						   conn->local_ip,
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
					NET_STATS_INC(NET_MIB_TCP_LISTENERGONE);
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
					tcp_conn_put(conn);
					return;
				}
			}
		}
		if (tcp_flags & TCP_RST) {
			// Peer reset the half-open connection: kill it (drop the
			// protocol reference).  It is freed once our transient
			// reference is released at the rx exit.
			tcp_conn_kill(conn);
			tcp_lock_release(&conn->lock, flags);
			tcp_conn_put(conn);
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
		// RST, blind-SYN, and PAWS were already handled by
		// tcp_validate_incoming() before the switch.
		conn->last_rx_tick = timer_ticks();
		conn->keep_probes_sent = 0;

		// Apply RFC 7323 window scaling on inbound advertised window
		{
			uint32_t scaled = (uint32_t)window;
			if (conn->ws_enabled)
				scaled <<= conn->snd_wscale;
			conn->snd_wnd = scaled;
			// Peer re-opened its window — cancel any persist probe and
			// wake the send path.
			if (scaled > 0 && conn->persist_tick) {
				conn->persist_tick = 0;
				conn->tx_ready = 1;
			}
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
						NET_STATS_INC(
							NET_MIB_TCP_RETRANSSEGS);
					}
				} else if (conn->dup_acks > 3) {
					conn->cwnd++;
				}
			}
		}

		// Process data: in-order vs out-of-order
		if (payload_len > 0) {
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
					// Reassembly queue full: this future
					// segment is dropped and must be
					// retransmitted by the peer.
					NET_STATS_INC(NET_MIB_TCP_OOOQUEUEFULL);
				}
				// Send immediate dup-ACK with SACK info (helps fast retransmit)
				tcp_queue_ack_locked(conn);
				ack_pending = 1;
			} else {
				// A future segment (seq > rcv_nxt) larger than
				// one MSS falls here and is dropped — not a
				// duplicate; only a retransmit recovers it.
				if ((int32_t)(seq - conn->rcv_nxt) > 0)
					NET_STATS_INC(NET_MIB_TCP_OOOOVERSIZE);
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
				NET_STATS_INC(NET_MIB_TCP_OOOFINREFUSED);
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
					NET_STATS_INC(NET_MIB_TCP_RETRANSSEGS);
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

	case TCP_STATE_CLOSE_WAIT:
		// The peer half-closed (sent its FIN); our application may still
		// be sending, and the peer keeps ACKing that data.  Without
		// processing those ACKs here the inflight queue never drains, the
		// RTO timer resends the already-delivered head forever, and the
		// peer stalls (this case previously fell through to default and
		// did nothing).  Update the send window and drain inflight on
		// partial ACKs, mirroring the closing-state handlers.
		conn->last_rx_tick = timer_ticks();
		{
			uint32_t scaled = (uint32_t)window;
			if (conn->ws_enabled)
				scaled <<= conn->snd_wscale;
			conn->snd_wnd = scaled;
			if (scaled > 0 && conn->persist_tick) {
				conn->persist_tick = 0;
				conn->tx_ready = 1;
			}
		}
		if (tcp_flags & TCP_ACK) {
			if (ack > conn->snd_una && ack <= conn->snd_nxt) {
				conn->snd_una = ack;
				tcp_ack_inflight(conn, ack);
				conn->retransmit_count = 0;
				conn->retransmit_tick =
					timer_ticks() + tcp_rto_ticks(conn);
				conn->dup_acks = 0;
			} else if (ack == conn->snd_una && payload_len == 0 &&
				   conn->inflight_count > 0) {
				// 3-dup-ACK fast retransmit, same as the closing
				// states, so a gap recovers within an RTT.
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
					NET_STATS_INC(NET_MIB_TCP_RETRANSSEGS);
				}
			}
		}
		// A retransmitted FIN (peer didn't see our ACK) just gets re-ACKed.
		if (tcp_flags & TCP_FIN) {
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
				// Final ACK of our FIN — connection is done.
				// Kill it (drop the protocol reference); freed
				// once our transient reference is released.
				tcp_conn_kill(conn);
				tcp_lock_release(&conn->lock, flags);
				tcp_conn_put(conn);
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

deferred_ack_out:;
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

	// Release the transient reference taken by tcp_find_conn_hold at entry.
	tcp_conn_put(conn);
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

	// Walk the connection list holding a reference on each connection while
	// it is processed — no fixed-size snapshot, so no cap on live
	// connections.  Reference-counted safe traversal: keep the current
	// connection held while finding and holding the next one, so the current
	// node's list_next stays valid across the unlocked processing window.
	// Runs in softirq context (net_timer_tick / SOFTIRQ_TCP_TIMER), so
	// blocking on conn->lock and freeing via tcp_conn_put are both legal.
	uint64_t lflags;
	spin_lock_irqsave(&tcp_lock, &lflags);
	tcp_conn_t *conn = g_tcp_conn_list;
	while (conn && !tcp_conn_tryhold(conn))
		conn = conn->list_next;
	spin_unlock_irqrestore(&tcp_lock, lflags);

	while (conn) {
		uint64_t flags;
		tcp_lock_acquire(&conn->lock, &flags);

		// Terminally-dead connections are reclaimed by reference counting
		// (tcp_conn_kill drops the protocol reference; the connection is
		// freed once no reference remains), so the timer no longer has any
		// orphan-reaper clauses — it only drives deadlines.

		// TIME_WAIT expiry
		if (conn->state == TCP_STATE_TIME_WAIT &&
		    now >= conn->time_wait_tick) {
			tcp_conn_kill(conn);
			goto unlock_conn;
		}

		// FIN_WAIT_2 timeout (RFC 1122 SHOULD discard after a while)
		if (conn->state == TCP_STATE_FIN_WAIT_2 &&
		    conn->fin_wait_2_deadline &&
		    now >= conn->fin_wait_2_deadline) {
			tcp_conn_kill(conn);
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
				tcp_conn_kill(conn);
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
					NET_STATS_INC(NET_MIB_TCP_RETRANSSEGS);
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

		// Zero-window persist probe (RFC 1122 §4.2.2.17).  Armed by
		// tcp_send_data when a send stalls on a closed peer window with
		// nothing in flight.  Send a 1-octet-back probe (seq snd_una-1)
		// to force the peer to re-advertise its window; back off
		// exponentially to the ceiling.  Disarmed the moment the window
		// reopens (see the ACK-processing paths that clear persist_tick).
		if (conn->persist_tick && now >= conn->persist_tick &&
		    conn->snd_wnd == 0 && conn->inflight_count == 0 &&
		    (conn->state == TCP_STATE_ESTABLISHED ||
		     conn->state == TCP_STATE_CLOSE_WAIT)) {
			tcp_send_segment(conn->dev, conn->local_ip,
					 conn->remote_ip, conn->local_port,
					 conn->remote_port, conn->snd_una - 1,
					 conn->rcv_nxt, TCP_ACK,
					 (uint16_t)conn->rcv_wnd, NULL, 0);
			NET_STATS_INC(NET_MIB_TCP_PERSISTPROBES);
			if (conn->persist_backoff < 7)
				conn->persist_backoff++;
			uint64_t interval = (uint64_t)TCP_PERSIST_MIN_TICKS
					    << conn->persist_backoff;
			if (interval > TCP_PERSIST_MAX_TICKS)
				interval = TCP_PERSIST_MAX_TICKS;
			conn->persist_tick = now + interval;
		}

unlock_conn:
		tcp_lock_release(&conn->lock, flags);

		// Advance: find and hold the next connection while still holding
		// `conn` (so conn->list_next is valid), then drop `conn`.  If the
		// drop was the last reference, the connection is queued for free.
		spin_lock_irqsave(&tcp_lock, &lflags);
		tcp_conn_t *next = conn->list_next;
		while (next && !tcp_conn_tryhold(next))
			next = next->list_next;
		spin_unlock_irqrestore(&tcp_lock, lflags);
		tcp_conn_put(conn);
		conn = next;
	}
}

// Snapshot the connection table for netstat (SYS_NET_GETINFO).  Best-effort
// lock-free walk (a torn read at worst garbles one row).
int net_get_tcp_connections(net_tcp_info_t *entries, int max_entries)
{
	int count = 0;
	for (tcp_conn_t *c = g_tcp_conn_list; c && count < max_entries;
	     c = c->list_next) {
		if (!c->active)
			continue;
		entries[count].local_ip = c->local_ip;
		entries[count].local_port = c->local_port;
		entries[count].remote_ip = c->remote_ip;
		entries[count].remote_port = c->remote_port;
		entries[count].state = c->state;
		uint32_t rx_used = 0;
		if (c->rx_buf) {
			if (c->rx_tail >= c->rx_head)
				rx_used = c->rx_tail - c->rx_head;
			else
				rx_used = c->rx_buf_size - c->rx_head +
					  c->rx_tail;
		}
		entries[count].rx_queue = rx_used;
		uint32_t tx_used = 0;
		if (c->inflight) {
			for (uint8_t seg = 0; seg < c->inflight_count; seg++) {
				tx_used += c->inflight[seg].len;
				if (c->inflight[seg].flags &
				    (TCP_SYN | TCP_FIN))
					tx_used++;
			}
		}
		entries[count].tx_queue = tx_used;
		count++;
	}
	return count;
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
	int i = 0;
	// Best-effort lock-free walk (diagnostic): a torn read at worst
	// garbles one line.
	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next, i++) {
		if (!c->active)
			continue;
		const char *s = (c->state <= TCP_STATE_TIME_WAIT) ?
					sn[c->state] :
					"???";
		uint32_t li = c->local_ip, ri = c->remote_ip;
		int parent_slot = c->parent ? 1 : -1;
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

	// Nonzero protocol counters — surfaces silent-drop paths at a glance.
	tty_printf(tty, "--- net counters (nonzero) ---\n");
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
		{ "OOWSeqDrop", NET_MIB_TCP_OOWSEQDROP },
		{ "ChallengeAck", NET_MIB_TCP_CHALLENGEACK },
		{ "OOOQueueFull", NET_MIB_TCP_OOOQUEUEFULL },
		{ "OOOOversize", NET_MIB_TCP_OOOOVERSIZE },
		{ "OOOFinRefused", NET_MIB_TCP_OOOFINREFUSED },
		{ "AcceptQFull", NET_MIB_TCP_ACCEPTQFULL },
		{ "ListenerGone", NET_MIB_TCP_LISTENERGONE },
		{ "RstDataLoss", NET_MIB_TCP_RSTDATALOSS },
		{ "ConnTableFull", NET_MIB_TCP_CONNTABLEFULL },
		{ "BacklogDrop", NET_MIB_TCP_BACKLOGDROP },
		{ "PersistProbes", NET_MIB_TCP_PERSISTPROBES },
		{ "TWReused", NET_MIB_TCP_TWREUSED },
		{ "UdpNoPorts", NET_MIB_UDP_NOPORTS },
		{ "UdpRcvBufErr", NET_MIB_UDP_RCVBUFERRORS },
	};
	for (unsigned i = 0; i < sizeof(mib_names) / sizeof(mib_names[0]); i++) {
		uint64_t v = net_stats_read(mib_names[i].idx);
		if (v)
			tty_printf(tty, "  %s=%llu\n", mib_names[i].name, v);
	}
	uint64_t skb_fail = skb_get_alloc_failures();
	if (skb_fail)
		tty_printf(tty, "  SkbAllocFail=%llu\n", skb_fail);
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
	uint64_t flags;
	spin_lock_irqsave(&tcp_lock, &flags);
	for (tcp_conn_t *c = g_tcp_conn_list; c; c = c->list_next) {
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
	spin_unlock_irqrestore(&tcp_lock, flags);
}

// RFC 793 §3.5 abort: send RST and tear down the connection immediately.
// Used by SO_LINGER l_onoff=1 l_linger=0.  The caller holds the socket
// reference, so the connection is alive here; this drops the protocol
// self-reference (tcp_conn_kill) and the caller drops the socket reference.
void tcp_abort(tcp_conn_t *conn)
{
	if (!conn)
		return;
	uint64_t flags;
	tcp_lock_acquire(&conn->lock, &flags);
	if (conn->state != TCP_STATE_CLOSED && conn->dev) {
		tcp_send_rst(conn->dev, conn->local_ip, conn->remote_ip,
			     conn->local_port, conn->remote_port, conn->snd_nxt,
			     conn->rcv_nxt);
	}
	tcp_conn_kill(conn);
	tcp_lock_release(&conn->lock, flags);
}
