// LikeOS TCP: output
//
// Segments are built from the write queue: the payload of each skb on the
// queue is the data, the header goes into its headroom -- of a CLONE, so
// the queued copy stays intact for retransmission (or of a private copy
// when an earlier transmission still holds the data, as the device may
// not have sent it yet).  A sent skb moves to the retransmit queue and
// leaves it when the peer acknowledges it.
//
// What may be sent is decided by the congestion window (segments in
// flight), the peer's receive window, Nagle / TCP_CORK, pacing (the
// earliest departure of the next segment at the pacing rate) and TSQ
// (bytes still below us in the device layer).  Every segment carries the
// options the connection negotiated and the window tcp_select_window()
// offers; new data of an ECN connection is ECT(0), with CWR after a
// reduction and ECE while echoing a CE mark.  All of this runs under
// conn->lock; transmission never enters a driver here (the device layer
// defers while interrupts are off).
//
// Pacing runs on the timer tick: a segment may leave while its departure
// time is no more than one tick ahead (the next chance to send is a tick
// away), so the rate holds on average and bursts stay within a tick's
// worth; the pacing timer resumes a paced queue.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/net/softirq.h>

// ---------------------------------------------------------------------------
// MSS.
// ---------------------------------------------------------------------------
/* Payload of a full-sized segment for a path MTU of `pmtu': the peer's MSS
 * at most, our TCP options deducted, never more than half the largest
 * window the peer offered (a peer with a tiny window must still get
 * several segments in flight). */
/* Payload of a full segment for an MTU, with this connection's options;
 * and the reverse. */
uint32_t tcp_mtu_to_mss(const tcp_conn_t *c, uint32_t pmtu)
{
	int32_t mss = (int32_t)pmtu - 20 - (int32_t)c->tcp_header_len;

	if (mss > (int32_t)c->mss_clamp)
		mss = c->mss_clamp;
	return mss < 48 ? 48 : (uint32_t)mss;
}

uint32_t tcp_mss_to_mtu(const tcp_conn_t *c, uint32_t mss)
{
	return mss + c->tcp_header_len + 20;
}

uint32_t tcp_sync_mss(tcp_conn_t *c, uint32_t pmtu)
{
	int32_t mss = (int32_t)pmtu - 20 - (int32_t)sizeof(tcp_header_t);

	/* Black-hole fallback: no larger than the probing floor while
	 * active. */
	if (c->mtup_enabled && c->mtup_search_low &&
	    (int32_t)c->mtup_search_low - 20 - (int32_t)sizeof(tcp_header_t) < mss)
		mss = (int32_t)c->mtup_search_low - 20 -
		      (int32_t)sizeof(tcp_header_t);
	if (mss > (int32_t)c->mss_clamp)
		mss = c->mss_clamp;
	if (mss < 48)
		mss = 48;
	mss -= (int32_t)c->tcp_header_len - (int32_t)sizeof(tcp_header_t);
	if (c->max_window) {
		uint32_t cutoff = c->max_window > TCP_MSS_DEFAULT ?
					  c->max_window >> 1 :
					  c->max_window;

		if (cutoff && (uint32_t)mss > cutoff)
			mss = (int32_t)tcp_max32(cutoff,
						 68u - c->tcp_header_len);
	}
	if (mss < 8)
		mss = 8;
	c->pmtu_cookie = pmtu;
	c->mss_cache = (uint16_t)mss;
	return (uint32_t)mss;
}

static uint32_t tcp_sack_opt_bytes(const tcp_conn_t *c)
{
	uint32_t n = (uint32_t)c->num_sacks + c->dsack;

	if (!c->sack_ok || n == 0)
		return 0;
	if (n > TCP_MAX_SACK_BLOCKS)
		n = TCP_MAX_SACK_BLOCKS;
	return 4 + 8 * n;
}

/* Option bytes of an established segment: timestamps, and SACK blocks as
 * many as fit (3 next to timestamps, 4 without). */
uint32_t tcp_options_size_established(const tcp_conn_t *c)
{
	uint32_t size = c->ts_enabled ? 12u : 0u;
	uint32_t sack = tcp_sack_opt_bytes(c);

	if (sack) {
		uint32_t room = TCP_MAX_OPTIONS - size;
		uint32_t blocks = (room - 4) / 8;
		uint32_t want = (sack - 4) / 8;

		if (want > blocks)
			want = blocks;
		size += 4 + 8 * want;
	}
	return size;
}

/* The MSS for data sent now: mss_cache, less the room SACK blocks take on
 * this segment. */
uint32_t tcp_current_mss(tcp_conn_t *c)
{
	uint32_t mss = c->mss_cache;
	uint32_t hdr = sizeof(tcp_header_t) + tcp_options_size_established(c);

	if (hdr > c->tcp_header_len) {
		uint32_t delta = hdr - c->tcp_header_len;

		mss = mss > delta + 8 ? mss - delta : 8;
	}
	return mss;
}

// ---------------------------------------------------------------------------
// Window selection (receiver SWS avoidance, RFC 1122 4.2.3.3).
// ---------------------------------------------------------------------------
uint32_t __tcp_select_window(tcp_conn_t *c)
{
	int32_t mss = (int32_t)c->rcv_mss;
	int32_t free_space = tcp_space(c);
	int32_t allowed_space = tcp_full_space(c);
	int32_t full_space, window;
	uint32_t ws = c->ws_enabled ? c->rcv_wscale : 0;

	full_space = (int32_t)tcp_min32(c->window_clamp, (uint32_t)allowed_space);
	if (mss > full_space) {
		mss = full_space;
		if (mss <= 0)
			return 0;
	}
	if (free_space < (full_space >> 1)) {
		c->ack_quick = 0;
		if (tcp_under_memory_pressure())
			c->rcv_ssthresh = tcp_min32(c->rcv_ssthresh,
						    4u * c->advmss);
		free_space &= ~((1 << ws) - 1);
		/* Too little to be worth offering: close the window rather
		 * than invite tiny segments; reading reopens it (with a
		 * window update, tcp_cleanup_rbuf). */
		if (free_space < (allowed_space >> 4) || free_space < mss)
			return 0;
	}
	if (free_space > (int32_t)c->rcv_ssthresh)
		free_space = (int32_t)c->rcv_ssthresh;
	if (ws) {
		window = free_space;
		window = (window + (1 << ws) - 1) & ~((1 << ws) - 1);
	} else {
		window = (int32_t)c->rcv_wnd;
		if (window <= free_space - mss || window > free_space)
			window = mss > 0 ? (free_space / mss) * mss : free_space;
		else if (mss == full_space && free_space > window + (full_space >> 1))
			window = free_space;
	}
	return window < 0 ? 0 : (uint32_t)window;
}

/* The window field of the next segment.  The window offered is never
 * taken back (RFC 9293 3.8.6.2.2): a smaller new value keeps the right
 * edge where it was.  Records what was offered (rcv_wnd, rcv_wup). */
uint16_t tcp_select_window(tcp_conn_t *c)
{
	uint32_t cur_win = tcp_receive_window(c);
	uint32_t new_win = __tcp_select_window(c);
	uint32_t ws = c->ws_enabled ? c->rcv_wscale : 0;

	if (new_win < cur_win)
		new_win = (cur_win + (1u << ws) - 1) & ~((1u << ws) - 1);
	c->rcv_wnd = new_win;
	c->rcv_wup = c->rcv_nxt;
	if (new_win > (65535u << ws))
		new_win = 65535u << ws;
	new_win >>= ws;
	return (uint16_t)new_win;
}

// ---------------------------------------------------------------------------
// Options.
// ---------------------------------------------------------------------------
/* The option block of a SYN or SYN-ACK.  It is laid out EXACTLY as the
 * most common stack lays out its own, byte for byte:
 *
 *   MSS(4)  SACK-permitted(2)+timestamps(10)  NOP WS(3)  = 20 bytes,
 *
 * SACK-permitted and timestamps packed into one aligned run when both go,
 * either one preceded by two NOPs when it goes alone.  The order is not
 * protocol, it is a fingerprint: large sites classify the client system
 * from the SYN and compare it with what the User-Agent claims, and answer
 * a mismatch with a degraded page.  A SYN-ACK answers an option only when
 * the peer offered it; a SYN offers all of them. */
static uint32_t tcp_syn_options_write(uint8_t *buf, uint16_t mss, int sack,
				      int ts, uint32_t tsval, uint32_t tsecr,
				      int ws, uint8_t wscale)
{
	uint32_t n = 0;

	buf[n++] = TCP_OPT_MSS;
	buf[n++] = TCP_OPT_MSS_LEN;
	tcp_put_be16(buf + n, mss);
	n += 2;
	if (ts) {
		if (sack) {
			buf[n++] = TCP_OPT_SACK_PERM;
			buf[n++] = TCP_OPT_SACK_PERM_LEN;
		} else {
			buf[n++] = TCP_OPT_NOP;
			buf[n++] = TCP_OPT_NOP;
		}
		buf[n++] = TCP_OPT_TIMESTAMP;
		buf[n++] = TCP_OPT_TIMESTAMP_LEN;
		tcp_put_be32(buf + n, tsval);
		n += 4;
		tcp_put_be32(buf + n, tsecr);
		n += 4;
	} else if (sack) {
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_SACK_PERM;
		buf[n++] = TCP_OPT_SACK_PERM_LEN;
	}
	if (ws) {
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_WSCALE;
		buf[n++] = TCP_OPT_WSCALE_LEN;
		buf[n++] = wscale;
	}
	while (n & 3)
		buf[n++] = TCP_OPT_NOP;
	return n;
}

/* Options of a synchronized connection's segment: NOP NOP timestamps,
 * then the SACK blocks -- a D-SACK first, then the most recently changed
 * block first (RFC 2018 4, RFC 2883).  Returns the bytes written. */
static uint32_t tcp_established_options_write(tcp_conn_t *c, uint8_t *buf)
{
	uint32_t n = 0;

	if (c->ts_enabled) {
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_TIMESTAMP;
		buf[n++] = TCP_OPT_TIMESTAMP_LEN;
		tcp_put_be32(buf + n, tcp_ts_now(c->ts_offset));
		n += 4;
		tcp_put_be32(buf + n, c->ts_recent);
		n += 4;
	}
	if (c->sack_ok && (c->num_sacks || c->dsack)) {
		uint32_t max = (TCP_MAX_OPTIONS - n - 4) / 8, k = 0;

		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_NOP;
		buf[n++] = TCP_OPT_SACK;
		uint8_t *lenp = &buf[n++];
		if (c->dsack && k < max) {
			tcp_put_be32(buf + n, c->duplicate_sack[0].start_seq);
			tcp_put_be32(buf + n + 4, c->duplicate_sack[0].end_seq);
			n += 8;
			k++;
		}
		for (uint32_t i = 0; i < c->num_sacks && k < max; i++, k++) {
			tcp_put_be32(buf + n, c->selective_acks[i].start_seq);
			tcp_put_be32(buf + n + 4, c->selective_acks[i].end_seq);
			n += 8;
		}
		*lenp = (uint8_t)(2 + 8 * k);
		if (c->dsack) {
			c->dsack = 0;
			TCPX_INC(TCPX_DSACKSENT);
		}
	}
	while (n & 3)
		buf[n++] = TCP_OPT_NOP;
	return n;
}

// ---------------------------------------------------------------------------
// TSQ: bytes of this socket still below us (device queue, driver ring).
// ---------------------------------------------------------------------------
static tcp_conn_t *g_tsq_list;
static spinlock_t g_tsq_lock = SPINLOCK_INIT("tcp_tsq");

/* The destructor of every transmitted clone: the device layer let go of
 * it.  Any context, also a hard interrupt (only atomics and leaf locks
 * here).  A socket that TSQ held back is queued for the TSQ softirq, which
 * sends on. */
static void tcp_wfree(sk_buff_t *skb)
{
	tcp_conn_t *c = (tcp_conn_t *)skb->sk;
	uint8_t old;
	uint64_t f;

	if (!c)
		return;
	__atomic_fetch_sub(&c->wmem_tx, 1, __ATOMIC_ACQ_REL);
	skb->sk = NULL;
	old = __atomic_load_n(&c->tsq_flags, __ATOMIC_ACQUIRE);
	if ((old & TSQ_THROTTLED) && !(old & TSQ_QUEUED)) {
		uint8_t prev = __atomic_fetch_or(&c->tsq_flags, TSQ_QUEUED,
						 __ATOMIC_ACQ_REL);

		if (!(prev & TSQ_QUEUED)) {
			__atomic_fetch_and(&c->tsq_flags,
					   (uint8_t)~TSQ_THROTTLED,
					   __ATOMIC_ACQ_REL);
			spin_lock_irqsave(&g_tsq_lock, &f);
			c->tsq_next = g_tsq_list;
			g_tsq_list = c;
			spin_unlock_irqrestore(&g_tsq_lock, f);
			softirq_raise(SOFTIRQ_TCP_TIMER);
			return; // the clone's reference moves to the list
		}
	}
	tcp_conn_put(c);
}

/* Resume a socket TSQ or pacing held back: lost segments first, then
 * new data. */
void tcp_tsq_write(tcp_conn_t *c)
{
	if (c->lost_out > c->retrans_out && c->cwnd > tcp_packets_in_flight(c))
		tcp_xmit_retransmit_queue(c);
	if (tcp_send_head(c) &&
	    tcp_write_xmit(c, tcp_current_mss(c), c->nonagle, 0))
		tcp_check_probe_timer(c);
	tcp_update_tx_ready(c);
}

static void tcp_tsq_handler(void)
{
	for (;;) {
		tcp_conn_t *c;
		uint64_t f;

		spin_lock_irqsave(&g_tsq_lock, &f);
		c = g_tsq_list;
		if (c)
			g_tsq_list = c->tsq_next;
		spin_unlock_irqrestore(&g_tsq_lock, f);
		if (!c)
			return;
		c->tsq_next = NULL;
		tcp_lock_acquire(&c->lock, &f);
		__atomic_fetch_and(&c->tsq_flags, (uint8_t)~TSQ_QUEUED,
				   __ATOMIC_ACQ_REL);
		if (c->state == TCP_STATE_ESTABLISHED ||
		    c->state == TCP_STATE_CLOSE_WAIT ||
		    c->state == TCP_STATE_FIN_WAIT_1 ||
		    c->state == TCP_STATE_LAST_ACK ||
		    c->state == TCP_STATE_CLOSING)
			tcp_tsq_write(c);
		tcp_lock_release(&c->lock, f);
		tcp_conn_put(c);
	}
}

void tcp_tsq_init(void)
{
	softirq_register(SOFTIRQ_TCP_TIMER, tcp_tsq_handler);
}

/* Too much of this socket is already in the device layer: about a
 * millisecond at the connection's rate, at least two segments, at most
 * TCP_LIMIT_OUTPUT_BYTES.  (wmem_tx counts the segments down there.)  The
 * socket is marked; the destructor of the next segment the device lets
 * go of resumes it. */
static int tcp_small_queue_check(tcp_conn_t *c, const sk_buff_t *skb)
{
	uint64_t bytes = 0, limit;
	uint32_t mss = c->mss_cache ? c->mss_cache : TCP_MSS_DEFAULT;

	(void)skb;
	if (c->pacing_rate)
		bytes = c->pacing_rate >> 10; // about a millisecond's worth
	else if (c->srtt_us)
		bytes = (2ULL * c->cwnd * mss * 1000000ULL / c->srtt_us) >> 10;
	if (bytes > TCP_LIMIT_OUTPUT_BYTES)
		bytes = TCP_LIMIT_OUTPUT_BYTES;
	limit = bytes / mss;
	/* At least what one burst under the socket lock can leave behind
	 * in the device layer's deferral list before the lock is dropped
	 * and it is flushed. */
	if (limit < 16)
		limit = 16;
	if (__atomic_load_n(&c->wmem_tx, __ATOMIC_ACQUIRE) <= limit)
		return 0;
	__atomic_fetch_or(&c->tsq_flags, TSQ_THROTTLED, __ATOMIC_ACQ_REL);
	/* The device may have let go of everything between the test and
	 * the mark: look again. */
	if (__atomic_load_n(&c->wmem_tx, __ATOMIC_ACQUIRE) <= limit)
		return 0;
	TCPX_INC(TCPX_TSQDEFER);
	return 1;
}

// ---------------------------------------------------------------------------
// Building and sending one segment.
// ---------------------------------------------------------------------------
static void tcp_event_ack_sent(tcp_conn_t *c)
{
	if (c->ack_quick) {
		uint32_t pkts = (c->ack_pending & TCP_ACK_SCHED) ? 1 : 0;

		if (pkts >= c->ack_quick) {
			c->ack_quick = 0;
			c->ack_ato = (uint32_t)tcp_ms(TCP_ATO_MIN_MS);
		} else {
			c->ack_quick -= (uint8_t)pkts;
		}
	}
	c->ack_pending = 0;
	tcp_clear_delack_timer(c);
}

static void tcp_event_data_sent(tcp_conn_t *c)
{
	uint64_t now = tcp_now();

	if (tcp_packets_in_flight(c) == 0 && c->ca_ops->cwnd_event)
		c->ca_ops->cwnd_event(c, CA_EVENT_TX_START);
	c->lsndtime = now;
	/* An answer within the ACK timeout of the data it answers: an
	 * interactive exchange, where the ACK rides on the answer. */
	if (c->ack_ato && now - c->ack_lrcvtime < c->ack_ato)
		c->ack_pingpong = 1;
}

/* Transmit `skb' (data at the payload; cb filled in).  With clone_it the
 * queued skb stays where it is and a clone (or copy) goes out; without,
 * the skb itself is consumed (ACKs, probes).  Returns 0, or a negative
 * errno when it could not leave this host (no buffer, queue full) -- the
 * caller retries. */
int tcp_transmit_skb(tcp_conn_t *c, sk_buff_t *skb, int clone_it)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint8_t opts[TCP_MAX_OPTIONS];
	uint32_t olen, hlen, len, payload = skb->len;
	uint8_t flags = cb->tcp_flags, *th, tos = 0;
	uint16_t window, check, urg = 0;
	uint64_t prior_wstamp = c->wstamp_ns;
	int new_data = payload && !before(cb->seq, c->snd_nxt);
	sk_buff_t *n;
	int rc;

	if (clone_it) {
		uint64_t now_us = tcp_now_us(), now_ns = tcp_now_us() * 1000u;

		if (skb_cloned(skb))
			n = skb_copy(skb); // an earlier copy is still below us
		else
			n = skb_clone(skb);
		if (!n)
			return -ENOBUFS;
		cb->tx_us = now_us;
		if (!cb->first_tx_us)
			cb->first_tx_us = now_us;
		tcp_tsorted_move_tail(c, skb); // the latest transmission
		if (c->wstamp_ns < now_ns)
			c->wstamp_ns = now_ns;
		tcp_rate_skb_sent(c, skb);
	} else {
		n = skb;
	}
	if (flags & TCP_SYN) {
		int synack = (flags & TCP_ACK) != 0;

		olen = tcp_syn_options_write(
			opts, c->advmss, synack ? c->sack_ok : 1,
			synack ? c->ts_enabled : 1, tcp_ts_now(c->ts_offset),
			synack ? c->ts_recent : 0, synack ? c->ws_enabled : 1,
			c->rcv_wscale);
		window = (uint16_t)tcp_min32(c->rcv_wnd, 65535u);
	} else {
		olen = tcp_established_options_write(c, opts);
		window = tcp_select_window(c);
		flags |= TCP_ACK;
		/* ECN (RFC 3168 6.1): ECT(0) on new data, never on
		 * retransmissions or pure ACKs; CWR once after a reduction;
		 * ECE while a CE mark waits for the peer's CWR. */
		if ((c->ecn_flags & TCP_ECN_OK) && !(flags & TCP_RST)) {
			if (new_data) {
				tos = INET_ECN_ECT_0;
				if (c->ecn_flags & TCP_ECN_QUEUE_CWR) {
					c->ecn_flags &= (uint8_t)~TCP_ECN_QUEUE_CWR;
					flags |= TCP_CWR;
				}
			}
			if (c->ecn_flags & TCP_ECN_DEMAND_CWR)
				flags |= TCP_ECE;
		}
	}
	if (flags & TCP_RST)
		olen = 0;
	hlen = sizeof(tcp_header_t) + olen;
	if (skb_headroom(n) < hlen + 20 + ETH_HLEN) {
		n = skb_realloc_headroom(n, TCP_MAX_HEADER);
		if (!n)
			return -ENOBUFS;
	}
	th = skb_push(n, hlen);
	len = n->len;
	tcp_put_be16(th, c->local_port);
	tcp_put_be16(th + 2, c->remote_port);
	tcp_put_be32(th + 4, cb->seq);
	tcp_put_be32(th + 8, (flags & TCP_ACK) ? c->rcv_nxt : 0);
	th[12] = (uint8_t)((hlen / 4) << 4);
	/* Urgent mode (RFC 6093): every segment that starts before the
	 * urgent pointer carries URG and the offset to it. */
	flags &= (uint8_t)~TCP_URG;
	if (c->snd_urg_pending && after(c->snd_up, cb->seq) &&
	    !(flags & TCP_SYN)) {
		uint32_t up = c->snd_up - cb->seq;

		urg = up > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)up;
		flags |= TCP_URG;
	}
	th[13] = flags;
	tcp_put_be16(th + 14, window);
	th[16] = th[17] = 0;
	tcp_put_be16(th + 18, urg);
	if (olen)
		mm_memcpy(th + sizeof(tcp_header_t), opts, olen);
	check = csum_fold(csum_tcpudp_nofold(c->local_ip, c->remote_ip,
					     (uint16_t)len, IP_PROTO_TCP,
					     csum_partial(th, len, 0)));
	mm_memcpy(th + 16, &check, 2);
	skb_reset_transport_header(n);

	if (flags & TCP_ACK)
		tcp_event_ack_sent(c);
	if (cb->end_seq != cb->seq && !(flags & TCP_SYN))
		tcp_event_data_sent(c);
	NET_STATS_INC(NET_MIB_TCP_OUTSEGS);
	if (flags & TCP_RST)
		NET_STATS_INC(NET_MIB_TCP_OUTRSTS);
	c->segs_out++;
	if (payload) {
		c->data_segs_out++;
		if (new_data)
			c->bytes_sent += payload;
	}

	if (clone_it) {
		// TSQ accounting: the clone's lifetime in the device layer
		tcp_conn_hold(c);
		n->sk = c;
		n->destructor = tcp_wfree;
		__atomic_fetch_add(&c->wmem_tx, 1, __ATOMIC_ACQ_REL);
		/* Pacing: the next segment departs len/rate later; time
		 * this one waited beyond its departure (timer jitter) earns
		 * back up to half of that.  The first ten go unpaced. */
		if (tcp_needs_internal_pacing(c) && c->pacing_rate && payload &&
		    c->data_segs_out > 10) {
			uint64_t len_ns = (uint64_t)payload * 1000000000ULL /
					  c->pacing_rate;
			uint64_t credit = c->wstamp_ns - prior_wstamp;

			if (prior_wstamp > c->wstamp_ns)
				credit = 0;
			len_ns -= len_ns / 2 < credit ? len_ns / 2 : credit;
			c->wstamp_ns += len_ns;
		}
	}
	rc = ip_queue_xmit(n, c->local_ip, c->remote_ip, IP_PROTO_TCP, 0, tos,
			   &c->dst_cache, &c->inet_id);
	return rc < 0 ? rc : 0;
}

/* A bare segment of the connection (ACK, probe, RST) at `seq'. */
static int tcp_send_bare(tcp_conn_t *c, uint32_t seq, uint8_t flags)
{
	sk_buff_t *skb = skb_alloc_headroom(TCP_MAX_HEADER, 0);
	struct tcp_skb_cb *cb;

	if (!skb)
		return -ENOBUFS;
	cb = TCP_SKB_CB(skb);
	mm_memset(cb, 0, sizeof(*cb));
	cb->seq = seq;
	cb->end_seq = seq;
	cb->tcp_flags = flags;
	return tcp_transmit_skb(c, skb, 0);
}

// ---------------------------------------------------------------------------
// ACKs.
// ---------------------------------------------------------------------------
void tcp_send_ack(tcp_conn_t *c)
{
	if (c->state == TCP_STATE_CLOSED)
		return;
	if (tcp_send_bare(c, c->snd_nxt, TCP_ACK) < 0) {
		/* No buffer: owe the ACK and try again from the delayed-ACK
		 * timer shortly. */
		c->ack_pending |= TCP_ACK_SCHED | TCP_ACK_TIMER;
		c->ack_timeout = tcp_now() + tcp_ms(TCP_ATO_MIN_MS);
		tcp_reset_delack_timer(c, c->ack_timeout);
	}
}

/* Delay the ACK (RFC 1122 4.2.3.2, RFC 5681 4.2) by the ACK timeout
 * estimate, at most 200 ms (interactive) or 500 ms, and never longer than
 * an RTT. */
void tcp_send_delayed_ack(tcp_conn_t *c)
{
	uint64_t ato = c->ack_ato, timeout, now = tcp_now();

	if (ato > tcp_ms(TCP_DELACK_MIN_MS)) {
		uint64_t max_ato = tcp_ms(500);

		if (c->ack_pingpong || (c->ack_pending & TCP_ACK_PUSHED))
			max_ato = tcp_ms(TCP_DELACK_MAX_MS);
		if (c->srtt_us) {
			uint64_t rtt = tcp_us_to_ticks(c->srtt_us);

			if (rtt < tcp_ms(TCP_DELACK_MIN_MS))
				rtt = tcp_ms(TCP_DELACK_MIN_MS);
			if (rtt < max_ato)
				max_ato = rtt;
		}
		if (ato > max_ato)
			ato = max_ato;
	}
	/* No estimate yet (nothing received): the next tick. */
	timeout = now + (ato ? ato : 1);
	if (c->ack_pending & TCP_ACK_TIMER) {
		/* Already armed: an ACK delayed by more than half its
		 * timeout goes now; else the earlier deadline stays. */
		if (c->ack_timeout <= now + (ato >> 1)) {
			tcp_send_ack(c);
			return;
		}
		if (timeout > c->ack_timeout)
			timeout = c->ack_timeout;
	}
	c->ack_pending |= TCP_ACK_SCHED | TCP_ACK_TIMER;
	c->ack_timeout = timeout;
	tcp_reset_delack_timer(c, timeout);
}

/* At most one ACK to rejected segments per TCP_INVALID_RATELIMIT_MS for a
 * connection or minisock (RFC 5961 5, and the dup-ACK storms between two
 * confused stacks).  A segment with data is answered regardless: data is
 * no part of an ACK loop, and after lost ACKs the peer's retransmissions
 * of old data must each learn where we are. */
int tcp_oow_rate_limited_stamp(uint64_t *last_tick, const sk_buff_t *skb,
			       enum tcp_xmib mib)
{
	uint64_t now = tcp_now();

	if (skb) {
		const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

		if (cb->seq != cb->end_seq && !(cb->tcp_flags & TCP_SYN))
			return 0;
	}
	if (*last_tick && now - *last_tick < tcp_ms(TCP_INVALID_RATELIMIT_MS)) {
		TCPX_INC(mib);
		return 1;
	}
	*last_tick = now ? now : 1;
	return 0;
}

int tcp_oow_rate_limited(tcp_conn_t *c, const sk_buff_t *skb,
			 enum tcp_xmib mib)
{
	return tcp_oow_rate_limited_stamp(&c->last_oow_ack_tick, skb, mib);
}

void tcp_send_challenge_ack(tcp_conn_t *c)
{
	if (tcp_oow_rate_limited(c, NULL, TCPX_ACKSKIPPEDCHALLENGE))
		return;
	if (!tcp_challenge_ack_allowed())
		return;
	TCPX_INC(TCPX_CHALLENGEACK);
	NET_STATS_INC(NET_MIB_TCP_CHALLENGEACK);
	tcp_send_ack(c);
}

/* A duplicate: ACK it at once, with a D-SACK for data we already had. */
void tcp_send_dupack(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (cb->end_seq != cb->seq && before(cb->seq, c->rcv_nxt)) {
		uint32_t end = after(cb->end_seq, c->rcv_nxt) ? c->rcv_nxt :
								cb->end_seq;

		tcp_enter_quickack_mode(c, TCP_MAX_QUICKACKS);
		tcp_dsack_set(c, cb->seq, end);
	}
	tcp_send_ack(c);
}

// ---------------------------------------------------------------------------
// Handshake segments.
// ---------------------------------------------------------------------------
/* connect(): queue the SYN (it is retransmitted from the retransmit queue
 * like data) and send it. */
int tcp_send_syn(tcp_conn_t *c)
{
	sk_buff_t *skb = skb_alloc_headroom(TCP_MAX_HEADER, 0);
	struct tcp_skb_cb *cb;
	int rc;

	if (!skb) {
		// no buffer: the retransmit timer tries again shortly
		c->retrans_stamp = c->retrans_stamp ? c->retrans_stamp :
						      tcp_now();
		tcp_reset_xmit_timer(c, TCP_TIME_RETRANS,
				     tcp_ms(TCP_LOCAL_DROP_RETRY_MS));
		return -ENOBUFS;
	}
	cb = TCP_SKB_CB(skb);
	mm_memset(cb, 0, sizeof(*cb));
	cb->seq = c->iss;
	cb->end_seq = c->iss + 1;
	cb->tcp_flags = TCP_SYN;
	/* ECN is requested on an active open only in mode 1; by default it
	 * is only accepted when a peer asks. */
	if (tcp_sysctl_ecn == 1) {
		cb->tcp_flags |= TCP_ECE | TCP_CWR;
		c->ecn_flags = TCP_ECN_OK;
	}
	if (!c->syn_tsval)
		c->syn_tsval = tcp_ts_now(c->ts_offset);
	c->snd_nxt = c->iss + 1;
	c->write_seq = c->iss + 1;
	c->pushed_seq = c->write_seq;
	tcp_wmem_charge(c, skb);
	__skb_queue_tail(&c->rtx_queue, skb);
	c->packets_out = 1;
	if (!c->retrans_stamp)
		c->retrans_stamp = tcp_now();
	rc = tcp_transmit_skb(c, skb, 1);
	tcp_reset_xmit_timer(c, TCP_TIME_RETRANS,
			     rc < 0 ? tcp_ms(TCP_LOCAL_DROP_RETRY_MS) :
				      tcp_syn_rto_ticks(0));
	return rc;
}

/* Simultaneous open: the SYN on the retransmit queue becomes a SYN-ACK
 * and is sent again (and retransmitted as one). */
int tcp_send_synack(tcp_conn_t *c)
{
	sk_buff_t *skb = skb_peek(&c->rtx_queue);

	if (!skb || !(TCP_SKB_CB(skb)->tcp_flags & TCP_SYN))
		return -1;
	TCP_SKB_CB(skb)->tcp_flags |= TCP_ACK;
	TCP_SKB_CB(skb)->tcp_flags &= (uint8_t)~TCP_CWR;
	if (!(c->ecn_flags & TCP_ECN_OK))
		TCP_SKB_CB(skb)->tcp_flags &= (uint8_t)~TCP_ECE;
	return tcp_transmit_skb(c, skb, 1);
}

/* The SYN-ACK of a request sock (and of a SYN answered with a cookie). */
int tcp_make_synack_req(struct tcp_request_sock *req, const sk_buff_t *syn)
{
	uint8_t opts[TCP_MAX_OPTIONS];
	uint32_t olen, hlen, tsval = tcp_ts_now(req->ts_offset);
	sk_buff_t *skb;
	uint8_t *th;
	uint16_t check, mss;
	int cookie = req->cookie;

	(void)syn;
	if (cookie && req->ts_ok)
		tsval = tcp_cookie_init_timestamp(tsval, req->wscale_ok,
						  req->snd_wscale, req->sack_ok,
						  req->ecn_ok);
	mss = req->advmss ? req->advmss : TCP_MSS;
	olen = tcp_syn_options_write(opts, mss, req->sack_ok, req->ts_ok, tsval,
				     req->ts_recent, req->wscale_ok,
				     req->rcv_wscale);
	hlen = sizeof(tcp_header_t) + olen;
	skb = skb_alloc_headroom(TCP_MAX_HEADER, 0);
	if (!skb)
		return -ENOBUFS;
	th = skb_push(skb, hlen);
	mm_memset(th, 0, sizeof(tcp_header_t));
	tcp_put_be16(th, req->local_port);
	tcp_put_be16(th + 2, req->remote_port);
	tcp_put_be32(th + 4, req->iss);
	tcp_put_be32(th + 8, req->irs + 1);
	th[12] = (uint8_t)((hlen / 4) << 4);
	// ECN accepted (RFC 3168 6.1.1): ECE alone
	th[13] = TCP_SYN | TCP_ACK | (req->ecn_ok ? TCP_ECE : 0);
	tcp_put_be16(th + 14, (uint16_t)tcp_min32(req->rcv_wnd, 65535u));
	mm_memcpy(th + sizeof(tcp_header_t), opts, olen);
	check = csum_fold(csum_tcpudp_nofold(req->local_ip, req->remote_ip,
					     (uint16_t)hlen, IP_PROTO_TCP,
					     csum_partial(th, hlen, 0)));
	mm_memcpy(th + 16, &check, 2);
	skb_reset_transport_header(skb);
	NET_STATS_INC(NET_MIB_TCP_OUTSEGS);
	return ip_queue_xmit(skb, req->local_ip, req->remote_ip, IP_PROTO_TCP,
			     0, 0, NULL, NULL) < 0 ?
		       -ENOBUFS :
		       0;
}

// ---------------------------------------------------------------------------
// Splitting.
// ---------------------------------------------------------------------------
static void tcp_queue_insert_after(skb_queue_t *q, sk_buff_t *prev,
				   sk_buff_t *skb)
{
	skb->prev = prev;
	skb->next = prev->next;
	if (prev->next)
		prev->next->prev = skb;
	else
		q->tail = skb;
	prev->next = skb;
	q->len++;
}

/* Split `skb' (on queue `q') after `len' bytes: the rest goes into a new
 * skb right behind it.  FIN and PSH stay with the end.  Works on an skb
 * whose data a clone still shares (only this header is trimmed; the bytes
 * are copied out).  -ENOBUFS when no buffer was available. */
int tcp_fragment(tcp_conn_t *c, skb_queue_t *q, sk_buff_t *skb, uint32_t len)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb), *ncb;
	uint32_t rest;
	sk_buff_t *n;

	if (len == 0 || len >= skb->len)
		return 0;
	rest = skb->len - len;
	n = skb_alloc_headroom(TCP_MAX_HEADER, rest);
	if (!n)
		return -ENOBUFS;
	mm_memcpy(skb_append(n, rest), skb->data + len, rest);
	ncb = TCP_SKB_CB(n);
	*ncb = *cb;
	ncb->seq = cb->seq + len;
	cb->end_seq = cb->seq + len;
	cb->tcp_flags &= (uint8_t)~(TCP_FIN | TCP_PSH);
	skb_trim(skb, len);
	tcp_wmem_charge(c, n);
	tcp_queue_insert_after(q, skb, n);
	tcp_tsorted_insert_after(c, skb, n); // sent together
	if (q == &c->rtx_queue) {
		c->packets_out++;
		if (ncb->sacked & TCPCB_SACKED_RETRANS)
			c->retrans_out++;
		if (ncb->sacked & TCPCB_LOST)
			c->lost_out++;
		if (ncb->sacked & TCPCB_SACKED_ACKED)
			c->sacked_out++;
	}
	return 0;
}

/* The peer acknowledged the first `len' bytes of `skb': drop them from
 * this header (the data area may still be shared with a clone). */
int tcp_trim_head(tcp_conn_t *c, sk_buff_t *skb, uint32_t len)
{
	(void)c;
	if (len > skb->len)
		len = skb->len;
	skb_pull(skb, len);
	TCP_SKB_CB(skb)->seq += len;
	return 0;
}

// ---------------------------------------------------------------------------
// The write queue.
// ---------------------------------------------------------------------------
uint32_t tcp_packets_in_flight(const tcp_conn_t *c)
{
	uint32_t left = c->sacked_out + c->lost_out;

	return (c->packets_out > left ? c->packets_out - left : 0) +
	       c->retrans_out;
}

/* Minshall's variant of Nagle: hold a small segment only while an earlier
 * small segment is unacknowledged. */
static int tcp_minshall_check(const tcp_conn_t *c)
{
	return after(c->snd_sml, c->snd_una) && !after(c->snd_sml, c->snd_nxt);
}

static int tcp_nagle_test(const tcp_conn_t *c, const sk_buff_t *skb,
			  uint32_t mss_now, int nonagle)
{
	int partial = skb->len < mss_now;

	if (nonagle & TCP_NAGLE_PUSH)
		return 1;
	if (c->snd_urg_pending ||
	    (TCP_SKB_CB(skb)->tcp_flags & TCP_FIN))
		return 1;
	if (partial && ((nonagle & TCP_NAGLE_CORK) ||
			(!nonagle && c->packets_out && tcp_minshall_check(c))))
		return 0;
	return 1;
}

static int tcp_snd_wnd_test(const tcp_conn_t *c, const sk_buff_t *skb,
			    uint32_t mss_now)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t end = cb->seq + tcp_min32(skb->len, mss_now);

	if (cb->tcp_flags & TCP_FIN && skb->len == 0)
		return 1;
	return !after(end, c->snd_una + c->snd_wnd);
}

static uint32_t tcp_cwnd_test(const tcp_conn_t *c, const sk_buff_t *skb)
{
	uint32_t in_flight;

	if ((TCP_SKB_CB(skb)->tcp_flags & TCP_FIN) && skb->len == 0)
		return 1;
	in_flight = tcp_packets_in_flight(c);
	return in_flight < c->cwnd ? c->cwnd - in_flight : 0;
}

/* The RTO timer runs for the oldest unacknowledged segment: armed when
 * data goes out with nothing outstanding, never pushed back by new data
 * (RFC 6298 5.1); a pending TLP or zero-window probe makes way for it. */
static void tcp_event_new_data_sent(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t prior_packets = c->packets_out;

	__skb_unlink(skb, &c->write_queue);
	__skb_queue_tail(&c->rtx_queue, skb);
	tcp_rtx_idx_add(c, skb);
	c->snd_nxt = cb->end_seq;
	c->packets_out++;
	if (!prior_packets || c->pending_timer == TCP_TIME_PROBE0 ||
	    c->pending_timer == TCP_TIME_LOSS_PROBE)
		tcp_rearm_rto(c);
}

/* Pacing: may the next segment leave now?  It may while its departure
 * time is at most a tick ahead; else the pacing timer is armed for the
 * tick it may (and 1 is returned). */
int tcp_pacing_check(tcp_conn_t *c)
{
	uint64_t now_ns, horizon_ns, ahead_ns, ticks;

	if (!tcp_needs_internal_pacing(c) || !c->pacing_rate || !c->wstamp_ns)
		return 0;
	now_ns = tcp_now_us() * 1000u;
	horizon_ns = timer_us_per_tick() * 1000u;
	if (c->wstamp_ns <= now_ns + horizon_ns)
		return 0;
	ahead_ns = c->wstamp_ns - now_ns - horizon_ns;
	ticks = tcp_us_to_ticks((ahead_ns + 999u) / 1000u);
	if (!net_timer_pending(&c->pacing_timer))
		tcp_reset_pacing_timer(c, tcp_now() + ticks);
	TCPX_INC(TCPX_PACINGDEFER);
	return 1;
}

/* Send what the windows, Nagle/cork, pacing and TSQ allow from the write
 * queue.  push_one: 1 = at most one segment, 2 = one segment even beyond
 * cwnd (a tail loss probe).  Returns 1 when data waits that nothing in
 * flight will clock out (the caller arms the probe timer). */
int tcp_write_xmit(tcp_conn_t *c, uint32_t mss_now, int nonagle, int push_one)
{
	sk_buff_t *skb;
	int sent = 0, is_cwnd_limited = 0, paced = 0, failed = 0;

	if (c->state == TCP_STATE_CLOSED || c->state == TCP_STATE_LISTEN ||
	    c->state == TCP_STATE_SYN_SENT ||
	    c->state == TCP_STATE_SYN_RECEIVED)
		return 0;
	while ((skb = tcp_send_head(c)) != NULL) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		uint32_t limit;

		if (tcp_pacing_check(c)) {
			paced = 1;
			break;
		}
		if (!tcp_cwnd_test(c, skb)) {
			if (push_one != 2) {
				is_cwnd_limited = 1;
				break;
			}
		}
		if (!tcp_snd_wnd_test(c, skb, mss_now))
			break;
		if (skb->len <= mss_now &&
		    !tcp_nagle_test(c, skb, mss_now,
				    skb->next ? TCP_NAGLE_PUSH : nonagle))
			break;
		limit = mss_now;
		if (skb->len > limit &&
		    tcp_fragment(c, &c->write_queue, skb, limit) < 0)
			break;
		if (tcp_small_queue_check(c, skb))
			break;
		if (tcp_transmit_skb(c, skb, 1) < 0) {
			failed = 1;
			break;
		}
		if (skb->len < mss_now)
			c->snd_sml = cb->end_seq;
		tcp_event_new_data_sent(c, skb);
		sent++;
		if (push_one)
			break;
	}
	if (tcp_packets_in_flight(c) >= c->cwnd)
		is_cwnd_limited = 1;
	/* Held back by pacing with window to spare: says nothing about
	 * whether cwnd limits this window -- the paced send that follows
	 * tells (a validation now would call the window unused). */
	if ((sent || is_cwnd_limited) && !(paced && !is_cwnd_limited))
		tcp_cwnd_validate(c, is_cwnd_limited);
	if (sent) {
		if (tcp_in_cwnd_reduction(c))
			c->prr_out += (uint32_t)sent;
		// one loss probe per tail episode
		if (push_one != 2)
			tcp_schedule_loss_probe(c, 0);
		if (c->notsent_lowat)
			tcp_update_tx_ready(c);
	}
	if (failed) {
		/* Could not leave this host (no buffer, device queue full):
		 * not a loss.  Retry soon. */
		if (!c->packets_out)
			tcp_reset_xmit_timer(c, TCP_TIME_PROBE0,
					     tcp_ms(TCP_LOCAL_DROP_RETRY_MS));
		return 0;
	}
	if (sent)
		return 0;
	return !paced && !c->packets_out && tcp_send_head(c) != NULL;
}

/* Data waits and nothing is in flight to clock it out: the zero-window
 * probe timer (it also ends a TCP_CORK hold after the RTO, at least
 * 200 ms). */
void tcp_check_probe_timer(tcp_conn_t *c)
{
	if (!c->packets_out && !net_timer_pending(&c->retransmit_timer))
		tcp_reset_xmit_timer(c, TCP_TIME_PROBE0, tcp_probe0_when(c));
}

static void __tcp_push_pending_frames(tcp_conn_t *c, uint32_t mss_now,
				      int nonagle)
{
	if (c->state == TCP_STATE_CLOSED)
		return;
	if (tcp_write_xmit(c, mss_now, nonagle, 0))
		tcp_check_probe_timer(c);
}

void tcp_push_pending_frames(tcp_conn_t *c)
{
	if (tcp_send_head(c))
		__tcp_push_pending_frames(c, tcp_current_mss(c), c->nonagle);
}

/* Autocorking: a short write while earlier segments of this socket are
 * still in the device queue waits for them to leave, gathering what the
 * application writes meanwhile into one segment. */
static int tcp_should_autocork(const tcp_conn_t *c, const sk_buff_t *skb,
			       uint32_t size_goal)
{
	return skb->len < size_goal && !tcp_rtx_queue_empty(c) &&
	       __atomic_load_n(&c->wmem_tx, __ATOMIC_ACQUIRE) > 0;
}

/* The end of a write: PSH on its last segment (RFC 9293 3.9.1: a push at
 * every write boundary, and only there), then send.  `more': the
 * application said more follows (MSG_MORE): no push, and a partial
 * segment is held as under TCP_CORK. */
void tcp_push(tcp_conn_t *c, int nonagle, int more)
{
	sk_buff_t *skb = skb_peek_tail(&c->write_queue);
	uint32_t mss_now;

	if (!skb)
		return;
	mss_now = tcp_current_mss(c);
	if (!more || after(c->write_seq, c->pushed_seq + (c->max_window >> 1))) {
		TCP_SKB_CB(skb)->tcp_flags |= TCP_PSH;
		c->pushed_seq = c->write_seq;
	}
	if (more)
		nonagle = TCP_NAGLE_CORK;
	if (tcp_should_autocork(c, skb, mss_now)) {
		if (!(__atomic_load_n(&c->tsq_flags, __ATOMIC_ACQUIRE) &
		      TSQ_THROTTLED)) {
			TCPX_INC(TCPX_AUTOCORKING);
			__atomic_fetch_or(&c->tsq_flags, TSQ_THROTTLED,
					  __ATOMIC_ACQ_REL);
		}
		// The device may have let go meanwhile: look again.
		if (__atomic_load_n(&c->wmem_tx, __ATOMIC_ACQUIRE) > 0)
			return;
	}
	__tcp_push_pending_frames(c, mss_now, nonagle);
}

void tcp_data_snd_check(tcp_conn_t *c)
{
	tcp_push_pending_frames(c);
	tcp_update_tx_ready(c);
}

// ---------------------------------------------------------------------------
// FIN and reset.
// ---------------------------------------------------------------------------
/* Our FIN, after the queued data: on the last unsent skb, or in an skb of
 * its own.  With no buffer it is retried from the retransmit timer. */
void tcp_send_fin(tcp_conn_t *c)
{
	sk_buff_t *tail = skb_peek_tail(&c->write_queue);

	if (tail && !(TCP_SKB_CB(tail)->tcp_flags & TCP_FIN)) {
		TCP_SKB_CB(tail)->tcp_flags |= TCP_FIN;
		TCP_SKB_CB(tail)->end_seq++;
		c->write_seq++;
		c->fin_pending = 0;
	} else if (!tail) {
		sk_buff_t *skb = skb_alloc_headroom(TCP_MAX_HEADER, 0);
		struct tcp_skb_cb *cb;

		if (!skb) {
			c->fin_pending = 1;
			tcp_reset_xmit_timer(c, TCP_TIME_RETRANS,
					     tcp_ms(TCP_LOCAL_DROP_RETRY_MS));
			return;
		}
		c->fin_pending = 0;
		cb = TCP_SKB_CB(skb);
		mm_memset(cb, 0, sizeof(*cb));
		cb->seq = c->write_seq;
		cb->end_seq = c->write_seq + 1;
		cb->tcp_flags = TCP_ACK | TCP_FIN;
		c->write_seq++;
		tcp_wmem_charge(c, skb);
		__skb_queue_tail(&c->write_queue, skb);
	}
	__tcp_push_pending_frames(c, tcp_current_mss(c), TCP_NAGLE_OFF);
}

/* An abortive reset of the connection (close with unread data, linger 0,
 * too many orphans).  No options, like every reset. */
void tcp_send_active_reset(tcp_conn_t *c)
{
	if (c->state == TCP_STATE_LISTEN)
		return;
	tcp_send_bare(c, c->snd_nxt, TCP_RST | TCP_ACK);
}

// ---------------------------------------------------------------------------
// Retransmission.
// ---------------------------------------------------------------------------
/* Resend `skb' (trimmed to snd_una, cut to the current MSS).  The
 * accounting every resend shares; tcp_retransmit_skb adds the scoreboard
 * part a TLP probe leaves out.  0, or -errno when nothing left. */
int __tcp_retransmit_skb(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t cur_mss;
	int rc;

	if (!after(cb->end_seq, c->snd_una))
		return -EAGAIN; // acknowledged meanwhile
	if (before(cb->seq, c->snd_una))
		tcp_trim_head(c, skb, c->snd_una - cb->seq);
	if (!(cb->tcp_flags & TCP_SYN)) {
		cur_mss = tcp_current_mss(c);
		if (skb->len > cur_mss &&
		    tcp_fragment(c, &c->rtx_queue, skb, cur_mss) < 0)
			return -ENOBUFS;
		/* Beyond the peer's window (it shrank): not now, unless it
		 * is the head, which goes as a probe. */
		if (!before(cb->seq, c->snd_una + c->snd_wnd) &&
		    cb->seq != c->snd_una)
			return -EAGAIN;
	} else if (c->state == TCP_STATE_SYN_SENT && (c->ecn_flags & TCP_ECN_OK)) {
		/* A resent SYN goes without the ECN request (a middlebox
		 * may have dropped it for that). */
		cb->tcp_flags &= (uint8_t)~(TCP_ECE | TCP_CWR);
		c->ecn_flags = 0;
	}
	rc = tcp_transmit_skb(c, skb, 1);
	// No RTT sample from it, whatever happened.
	cb->sacked |= TCPCB_EVER_RETRANS;
	if (rc == 0) {
		c->total_retrans++;
		c->bytes_retrans += skb->len;
		NET_STATS_INC(NET_MIB_TCP_RETRANSSEGS);
	}
	return rc;
}

int tcp_retransmit_skb(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	int rc = __tcp_retransmit_skb(c, skb);

	if (rc == 0) {
		if (!(cb->sacked & TCPCB_SACKED_RETRANS))
			c->retrans_out++;
		cb->sacked |= TCPCB_SACKED_RETRANS;
		/* The give-up clocks (TCP_USER_TIMEOUT, the closing-state
		 * bound) run from the first transmission of what is now
		 * being resent; the undo clock (Eifel) from this resend. */
		if (!c->retrans_stamp) {
			uint64_t now_us = tcp_now_us(), age = 0;

			if (cb->first_tx_us && now_us > cb->first_tx_us)
				age = tcp_us_to_ticks(now_us - cb->first_tx_us);
			c->retrans_stamp = tcp_now() > age ? tcp_now() - age : 1;
		}
		/* 0 means "not set"; a TSval of 0 is kept as one tick
		 * earlier, which no echo of this resend can precede. */
		if (!c->retrans_ts) {
			uint32_t ts = tcp_ts_now(c->ts_offset);

			c->retrans_ts = ts ? ts : 0xFFFFFFFFu;
		}
		if (c->undo_retrans < 0)
			c->undo_retrans = 0;
		c->undo_retrans++;
	}
	return rc;
}

/* Resend what the scoreboard marks lost (and not resent yet), in order,
 * as the window allows (RFC 6675 NextSeg rule 1; RACK decides what is
 * lost).  In Recovery each resend counts against PRR. */
void tcp_xmit_retransmit_queue(tcp_conn_t *c)
{
	sk_buff_t *skb, *rtx_head, *hole = NULL;
	int rearm_timer = 0;

	if (!c->packets_out)
		return;
	rtx_head = skb_peek(&c->rtx_queue);
	skb = c->retransmit_skb_hint ? c->retransmit_skb_hint : rtx_head;
	for (; skb; skb = skb->next) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		uint8_t sacked;

		if (tcp_pacing_check(c))
			break;
		if (!hole)
			c->retransmit_skb_hint = skb;
		if (tcp_packets_in_flight(c) >= c->cwnd)
			break;
		sacked = cb->sacked;
		if (c->retrans_out >= c->lost_out)
			break; // everything lost is resent
		if (!(sacked & TCPCB_LOST)) {
			if (!hole && !(sacked & (TCPCB_SACKED_RETRANS |
						 TCPCB_SACKED_ACKED)))
				hole = skb;
			continue;
		}
		if (sacked & (TCPCB_SACKED_ACKED | TCPCB_SACKED_RETRANS))
			continue;
		if (tcp_small_queue_check(c, skb))
			break;
		if (tcp_retransmit_skb(c, skb))
			break;
		TCPX_INC(c->ca_state == TCP_CA_LOSS ? TCPX_SLOWSTARTRETRANS :
						      TCPX_FASTRETRANS);
		if (tcp_in_cwnd_reduction(c))
			c->prr_out++;
		if (skb == rtx_head && c->pending_timer != TCP_TIME_REO_TIMEOUT)
			rearm_timer = 1;
	}
	if (rearm_timer)
		tcp_reset_xmit_timer(c, TCP_TIME_RETRANS, tcp_rto_ticks(c));
}

// ---------------------------------------------------------------------------
// Probes.
// ---------------------------------------------------------------------------
/* Something to elicit an ACK: new data if the window has room for some of
 * it (a partial segment), else an ACK with an old sequence number (RFC
 * 9293 3.8.6.1).  Returns > 0 when nothing left this host. */
int tcp_write_wakeup(tcp_conn_t *c)
{
	sk_buff_t *skb;

	if (c->state == TCP_STATE_CLOSED)
		return -1;
	skb = tcp_send_head(c);
	if (skb && before(TCP_SKB_CB(skb)->seq, c->snd_una + c->snd_wnd)) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		uint32_t mss = tcp_current_mss(c);
		uint32_t seg = tcp_min32(c->snd_una + c->snd_wnd - cb->seq, mss);

		if (skb->len > seg &&
		    tcp_fragment(c, &c->write_queue, skb, seg) < 0)
			return 1;
		cb->tcp_flags |= TCP_PSH;
		if (tcp_transmit_skb(c, skb, 1) < 0)
			return 1;
		tcp_event_new_data_sent(c, skb);
		return 0;
	}
	if (tcp_send_bare(c, c->snd_una - 1, TCP_ACK) < 0)
		return 1;
	return 0;
}

void tcp_send_probe0(tcp_conn_t *c)
{
	int err = tcp_write_wakeup(c);
	uint64_t timeout;

	if (c->packets_out || !tcp_send_head(c)) {
		c->probes_out = 0;
		c->rto_backoff = 0;
		c->probes_tstamp = 0;
		return;
	}
	if (c->probes_out < 255)
		c->probes_out++;
	NET_STATS_INC(NET_MIB_TCP_PERSISTPROBES);
	TCPX_INC(TCPX_WINPROBE);
	if (err <= 0) {
		if (c->rto_backoff < TCP_RETRIES2)
			c->rto_backoff++;
		timeout = tcp_probe0_when(c);
	} else {
		timeout = tcp_ms(TCP_LOCAL_DROP_RETRY_MS);
	}
	/* TCP_USER_TIMEOUT: the next probe no later than the moment the
	 * episode must end (at least a jiffy-sized step ahead). */
	if (c->user_timeout_ms && c->probes_tstamp) {
		uint64_t lim = tcp_ms(c->user_timeout_ms);
		uint64_t used = tcp_now() - c->probes_tstamp;
		uint64_t left = used < lim ? lim - used : 0;

		if (left < tcp_ms(2))
			left = tcp_ms(2);
		if (timeout > left)
			timeout = left;
	}
	tcp_reset_xmit_timer(c, TCP_TIME_PROBE0, timeout);
}

// ---------------------------------------------------------------------------
// Path MTU.
// ---------------------------------------------------------------------------
/* The path got narrower (RFC 1191; the IP layer recorded the new MTU):
 * segments are sized for it, and what is outstanding and too large was
 * dropped on the way -- resent at once at the new size, without a
 * congestion signal (tcp_simple_retransmit). */
void tcp_mtu_reduced(tcp_conn_t *c, uint32_t mtu)
{
	if (c->state == TCP_STATE_LISTEN || c->state == TCP_STATE_CLOSED)
		return;
	if (mtu < 68 || mtu >= c->pmtu_cookie)
		return;
	TCPX_INC(TCPX_MTUREDUCED);
	tcp_sync_mss(c, mtu);
	if (c->state == TCP_STATE_SYN_SENT || c->state == TCP_STATE_SYN_RECEIVED)
		return; // the SYN carries no data; the MSS is used from now on
	tcp_simple_retransmit(c);
}

/* Black-hole detection (RFC 4821 fallback): repeated timeouts may mean
 * the path drops large segments without telling.  With tcp_sysctl_mtu_probing
 * on, the first time this enables the fallback at TCP_BASE_MSS, the next
 * times it halves the MSS (not below the minimum). */
void tcp_mtu_probing(tcp_conn_t *c)
{
	uint32_t mss;

	if (!tcp_sysctl_mtu_probing)
		return;
	if (!c->mtup_enabled) {
		c->mtup_enabled = 1;
		mss = tcp_min32(TCP_BASE_MSS, tcp_mtu_to_mss(c, c->pmtu_cookie));
	} else {
		mss = tcp_mtu_to_mss(c, c->mtup_search_low) >> 1;
		mss = tcp_min32(TCP_BASE_MSS, mss);
	}
	if (mss < TCP_MIN_MSS)
		mss = TCP_MIN_MSS;
	c->mtup_search_low = tcp_mss_to_mtu(c, mss);
	TCPX_INC(TCPX_MTUPFAIL);
	tcp_sync_mss(c, c->pmtu_cookie);
}
