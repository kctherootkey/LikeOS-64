// LikeOS TCP: request socks and TIME_WAIT socks
//
// A SYN to a listener creates a request sock: the 4-tuple, both ISNs and
// the negotiated options, a timer for the SYN-ACK retransmissions -- no
// buffers and no full socket.  It sits in the established hash (so the
// handshake's final ACK finds it) and on its listener's list.  The final
// ACK creates the full socket (the child), which replaces the request sock
// in the hash and goes onto the accept queue.  When the SYN queue is full
// the listener answers with SYN cookies instead and keeps no state at all.
//
// A connection that reaches TIME_WAIT (or an orphan waiting in FIN_WAIT2
// for the peer's FIN) is handed over to a TIME_WAIT sock: the 4-tuple, the
// sequence numbers, the timestamp state and a timer; the full socket and
// its buffers are freed.
//
// Every request sock operation is serialised by its listener's lock;
// every TIME_WAIT sock operation by the sock's own lock (tcp.h has the
// order).  Both kinds are reference counted like full sockets: the hash
// table holds one reference, a pending timer one, a lookup one; the reap
// list frees them.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/mm/slab.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/net/softirq.h>

static volatile uint32_t g_tw_count;

uint32_t tcp_tw_count(void)
{
	return __atomic_load_n(&g_tw_count, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// TIME_WAIT sock reserve.
//
// A connection enters TIME_WAIT inside its receive path or its close, with
// its lock held and interrupts off -- where the slab allocator may not be
// called.  So TIME_WAIT socks come from a reserve that the reap softirq
// keeps filled (interrupts on, no lock).  An empty reserve only costs the
// TIME_WAIT state of that one connection (counted), never correctness of
// the others.
// ---------------------------------------------------------------------------
#define TW_RESERVE_TARGET 256
#define TW_RESERVE_LOW 64

static struct tcp_tw_sock *g_tw_reserve;
static uint32_t g_tw_reserve_n;
static spinlock_t g_tw_reserve_lock = SPINLOCK_INIT("tcp_tw_reserve");

static void tw_reserve_refill(void)
{
	uint64_t f;

	if (irqs_disabled())
		return;
	while (__atomic_load_n(&g_tw_reserve_n, __ATOMIC_RELAXED) <
	       TW_RESERVE_TARGET) {
		struct tcp_tw_sock *tw = slab_alloc(sizeof(*tw));

		if (!tw)
			return;
		spin_lock_irqsave(&g_tw_reserve_lock, &f);
		tw->skc_reap_next = (struct tcp_skc *)g_tw_reserve;
		g_tw_reserve = tw;
		g_tw_reserve_n++;
		spin_unlock_irqrestore(&g_tw_reserve_lock, f);
	}
}

static struct tcp_tw_sock *tw_reserve_take(void)
{
	struct tcp_tw_sock *tw;
	uint64_t f;
	uint32_t left;

	spin_lock_irqsave(&g_tw_reserve_lock, &f);
	tw = g_tw_reserve;
	if (tw) {
		g_tw_reserve = (struct tcp_tw_sock *)tw->skc_reap_next;
		g_tw_reserve_n--;
	}
	left = g_tw_reserve_n;
	spin_unlock_irqrestore(&g_tw_reserve_lock, f);
	if (left < TW_RESERVE_LOW)
		softirq_raise(SOFTIRQ_TIMER); // the reap softirq refills
	return tw;
}

/* Back onto the reserve -- also from under a socket lock, so never freed
 * here; tcp_minisocks_maintain() gives the excess back to the slab. */
static void tw_reserve_put(struct tcp_tw_sock *tw)
{
	uint64_t f;

	spin_lock_irqsave(&g_tw_reserve_lock, &f);
	tw->skc_reap_next = (struct tcp_skc *)g_tw_reserve;
	g_tw_reserve = tw;
	g_tw_reserve_n++;
	spin_unlock_irqrestore(&g_tw_reserve_lock, f);
}

void tcp_minisocks_init(void)
{
	tw_reserve_refill();
}

// Called from the reap softirq (interrupts on, no lock).
void tcp_minisocks_maintain(void)
{
	uint64_t f;

	if (irqs_disabled())
		return;
	if (__atomic_load_n(&g_tw_reserve_n, __ATOMIC_RELAXED) <
	    TW_RESERVE_TARGET) {
		tw_reserve_refill();
		return;
	}
	for (;;) {
		struct tcp_tw_sock *tw = NULL;

		spin_lock_irqsave(&g_tw_reserve_lock, &f);
		if (g_tw_reserve_n > TW_RESERVE_TARGET) {
			tw = g_tw_reserve;
			g_tw_reserve = (struct tcp_tw_sock *)tw->skc_reap_next;
			g_tw_reserve_n--;
		}
		spin_unlock_irqrestore(&g_tw_reserve_lock, f);
		if (!tw)
			return;
		slab_free(tw);
	}
}

// ---------------------------------------------------------------------------
// PAWS (RFC 7323 5) against a remembered ts_recent.
// ---------------------------------------------------------------------------
/* 1 when a segment with `tsval' must be rejected as an old duplicate.  A
 * ts_recent older than 24 days is too old to judge by (the peer's clock
 * may have wrapped), a zero one was never set, and a reset is judged only
 * within an MSL. */
int tcp_paws_reject_recent(uint32_t ts_recent, uint64_t ts_recent_stamp,
			   uint32_t tsval, int rst)
{
	uint64_t now = tcp_now();

	if ((int32_t)(ts_recent - tsval) <= 0)
		return 0;
	if (now - ts_recent_stamp >= timer_s_to_ticks(TCP_PAWS_24DAYS_S))
		return 0;
	if (!ts_recent)
		return 0;
	if (rst && now - ts_recent_stamp >= timer_s_to_ticks(TCP_PAWS_MSL_S))
		return 0;
	return 1;
}

static int tcp_in_window(uint32_t seq, uint32_t end_seq, uint32_t s_win,
			 uint32_t e_win)
{
	if (seq == s_win)
		return 1;
	if (after(end_seq, s_win) && before(seq, e_win))
		return 1;
	return seq == e_win && seq == end_seq;
}

// ---------------------------------------------------------------------------
// Request socks.
// ---------------------------------------------------------------------------
static void reqsk_timer_fn(net_timer_t *t);

void tcp_reqsk_free(struct tcp_request_sock *req)
{
	tcp_conn_t *l = req->listener;

	WARN_ON(req->skc_hashed);
	WARN_ON(net_timer_pending(&req->timer));
	req->skc_kind = 0;
	slab_free(req);
	tcp_conn_put(l);
}

/* Unhash and forget a request sock.  Under its listener's lock. */
void tcp_reqsk_drop(struct tcp_request_sock *req)
{
	tcp_conn_t *l = req->listener;
	struct tcp_request_sock **pp;

	if (req->dead)
		return;
	req->dead = 1;
	tcp_ehash_remove((struct tcp_skc *)req);
	for (pp = &l->reqsk_list; *pp; pp = &(*pp)->rsk_next) {
		if (*pp == req) {
			*pp = req->rsk_next;
			break;
		}
	}
	req->rsk_next = NULL;
	if (l->reqsk_qlen)
		l->reqsk_qlen--;
	tcp_skc_timer_del((struct tcp_skc *)req, &req->timer);
	tcp_skc_put((struct tcp_skc *)req); // the table's reference
}

// The listener closes: every request sock goes.  Under listener->lock.
void tcp_listen_stop(tcp_conn_t *l)
{
	while (l->reqsk_list)
		tcp_reqsk_drop(l->reqsk_list);
	l->reqsk_qlen = 0;
}

/* Window and scale a passive connection offers (the SYN-ACK carries them;
 * a cookie connection recomputes the same numbers from the listener). */
static void tcp_req_init_window(const tcp_conn_t *l,
				struct tcp_request_sock *req, uint32_t mss)
{
	/* The buffer the child inherits from the listener. */
	uint32_t space = tcp_win_from_space(l, l->rcvbuf);
	int locked = (l->userlocks & TCP_RCVBUF_LOCK) != 0;
	uint32_t rcv_wnd, clamp;
	uint8_t wscale;

	tcp_select_initial_window(space, mss, locked ? space : 0, locked,
				  &rcv_wnd, &clamp, &wscale);
	req->rcv_wnd = rcv_wnd;
	req->window_clamp = clamp;
	req->rcv_wscale = req->wscale_ok ? wscale : 0;
	req->advmss = (uint16_t)mss;
	if (!req->wscale_ok)
		req->window_clamp = tcp_min32(req->window_clamp, 65535u);
}

static uint32_t tcp_dev_mtu(const net_device_t *dev)
{
	return dev && dev->mtu >= 68 ? dev->mtu : NET_MTU_DEFAULT;
}

/* A SYN for a listener.  Called without locks (softirq, interrupts on),
 * the listener held by the caller. */
int tcp_conn_request(tcp_conn_t *l, sk_buff_t *skb, const tcp_parsed_opts_t *opt,
		     uint32_t saddr, uint32_t daddr, net_device_t *dev)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	const uint8_t *th = skb->data;
	struct tcp_request_sock *req, stack_req;
	uint16_t sport = tcp_get_be16(th), dport = tcp_get_be16(th + 2);
	uint32_t advmss = tcp_dev_mtu(dev) - 40;
	uint32_t isn_hint = 0;
	int want_cookie = 0;
	uint64_t f;

	if (l->state != TCP_STATE_LISTEN)
		return -1;
	if (l->user_mss && l->user_mss < advmss)
		advmss = l->user_mss;
	if (cb->first_tx_us >> 32) // a SYN that reused a TIME_WAIT tuple
		isn_hint = (uint32_t)cb->first_tx_us;
	/* The accept queue is full: nothing could be queued at the end of
	 * the handshake, so the SYN is dropped and the peer resends it. */
	if (__atomic_load_n(&l->ack_backlog, __ATOMIC_RELAXED) >
	    l->max_ack_backlog) {
		TCPX_INC(TCPX_LISTENOVERFLOWS);
		TCPX_INC(TCPX_LISTENDROPS);
		NET_STATS_INC(NET_MIB_TCP_ACCEPTQFULL);
		return -1;
	}
	/* The SYN queue is full: answer with a cookie, keep nothing. */
	if (__atomic_load_n(&l->reqsk_qlen, __ATOMIC_RELAXED) >=
	    tcp_max32(l->max_ack_backlog, 1)) {
		want_cookie = 1;
		TCPX_INC(TCPX_REQQFULLDOCOOKIES);
		l->synq_overflow_tick = tcp_now();
	}
	if (want_cookie) {
		req = &stack_req;
	} else {
		req = slab_alloc(sizeof(*req));
		if (!req) {
			TCPX_INC(TCPX_LISTENDROPS);
			return -1;
		}
	}
	mm_memset(req, 0, sizeof(*req));
	req->skc_kind = TCP_SKC_REQ;
	req->state = TCP_STATE_SYN_RECEIVED;
	req->refcount = 1; // the table's
	req->local_ip = daddr;
	req->local_port = dport;
	req->remote_ip = saddr;
	req->remote_port = sport;
	req->dev = dev;
	req->irs = cb->seq;
	req->mss = opt->mss ? opt->mss : TCP_MSS_DEFAULT;
	if (l->user_mss && l->user_mss < req->mss)
		req->mss = l->user_mss;
	if (req->mss < TCP_MIN_MSS)
		req->mss = TCP_MIN_MSS;
	req->ts_ok = opt->ts_present;
	req->ts_recent = opt->tsval;
	req->sack_ok = opt->sack_perm;
	req->wscale_ok = opt->wscale >= 0;
	req->snd_wscale = req->wscale_ok ? (uint8_t)opt->wscale : 0;
	/* ECN (RFC 3168 6.1.1): a SYN with ECE and CWR asks for it; it is
	 * accepted (passive mode by default) unless the SYN itself is ECT
	 * marked (a broken path that marks everything). */
	req->ecn_ok = tcp_sysctl_ecn &&
		      (cb->tcp_flags & (TCP_ECE | TCP_CWR)) == (TCP_ECE | TCP_CWR) &&
		      (cb->ip_dsfield & INET_ECN_MASK) == INET_ECN_NOT_ECT;
	req->ts_offset = tcp_ts_offset_for(daddr, saddr, dport, sport);
	tcp_req_init_window(l, req, advmss);
	net_timer_setup(&req->timer, reqsk_timer_fn);
	if (want_cookie) {
		uint16_t mss = req->mss;

		req->iss = tcp_cookie_init_sequence(saddr, daddr, sport, dport,
						    cb->seq, &mss);
		req->mss = mss;
		/* Without timestamps a cookie cannot carry the window scale
		 * or SACK: the connection does without them. */
		if (!req->ts_ok) {
			req->wscale_ok = 0;
			req->sack_ok = 0;
			req->ecn_ok = 0;
			req->snd_wscale = 0;
			req->rcv_wscale = 0;
			req->window_clamp = tcp_min32(req->window_clamp,
						      65535u);
		}
		req->listener = l;
		req->cookie = 1;
		tcp_make_synack_req(req, skb);
		TCPX_INC(TCPX_SYNCOOKIESENT);
		NET_STATS_INC(NET_MIB_TCP_SYNCOOKIESENT);
		return 0;
	}
	req->iss = isn_hint ? isn_hint :
			      tcp_isn(daddr, saddr, dport, sport);

	tcp_lock_acquire(&l->lock, &f);
	if (l->state != TCP_STATE_LISTEN || tcp_ehash_insert_req(req) < 0) {
		/* Closed meanwhile, or a SYN of the same tuple made its
		 * request sock first (that one answers). */
		tcp_lock_release(&l->lock, f);
		slab_free(req);
		return -1;
	}
	tcp_conn_hold(l);
	req->listener = l;
	req->rsk_next = l->reqsk_list;
	l->reqsk_list = req;
	l->reqsk_qlen++;
	tcp_skc_timer_mod((struct tcp_skc *)req, &req->timer,
			  tcp_now() + tcp_ms(TCP_TIMEOUT_INIT_MS));
	req->snt_synack_us = tcp_now_us();
	tcp_make_synack_req(req, skb);
	tcp_lock_release(&l->lock, f);
	return 0;
}

int tcp_synack_retransmit(struct tcp_request_sock *req)
{
	int rc = tcp_make_synack_req(req, NULL);

	if (rc == 0) {
		req->num_retrans++;
		TCPX_INC(TCPX_SYNRETRANS);
		NET_STATS_INC(NET_MIB_TCP_RETRANSSEGS);
	}
	return rc;
}

/* SYN-ACK retransmission (RFC 6298 5.5 backoff from 1 s), and expiry
 * after TCP_SYNACK_RETRIES.  With TCP_DEFER_ACCEPT a request sock whose
 * handshake completed without data is kept as long as the option asks,
 * its SYN-ACK resent only at the end to provoke the data. */
static void reqsk_timer_fn(net_timer_t *t)
{
	struct tcp_request_sock *req =
		(struct tcp_request_sock *)((char *)t -
					    __builtin_offsetof(
						    struct tcp_request_sock,
						    timer));
	tcp_conn_t *l = req->listener;
	uint32_t max_retries = TCP_SYNACK_RETRIES, defer;
	int expire, resend;
	uint64_t f;

	tcp_lock_acquire(&l->lock, &f);
	if (req->dead)
		goto out;
	if (req->completing) {
		/* A child is being made from it right now; should that
		 * fail, the request sock must still time out. */
		tcp_skc_timer_mod((struct tcp_skc *)req, &req->timer,
				  tcp_now() +
					  tcp_syn_rto_ticks(req->num_timeout));
		goto out;
	}
	if (l->state != TCP_STATE_LISTEN) {
		tcp_reqsk_drop(req);
		goto out;
	}
	defer = l->defer_accept;
	if (defer > max_retries)
		max_retries = defer;
	if (!defer) {
		expire = req->num_timeout >= max_retries;
		resend = 1;
	} else {
		expire = req->num_timeout >= max_retries &&
			 (!req->acked || req->num_timeout >= defer);
		resend = !req->acked || (uint32_t)req->num_timeout + 1 >= defer;
	}
	if (expire) {
		if (req->acked)
			TCPX_INC(TCPX_DEFERACCEPTDROP);
		NET_STATS_INC(NET_MIB_TCP_ATTEMPTFAILS);
		tcp_reqsk_drop(req);
		goto out;
	}
	if (resend)
		tcp_synack_retransmit(req);
	if (req->num_timeout < 255)
		req->num_timeout++;
	tcp_skc_timer_mod((struct tcp_skc *)req, &req->timer,
			  tcp_now() + tcp_syn_rto_ticks(req->num_timeout));
out:
	tcp_lock_release(&l->lock, f);
	tcp_skc_put((struct tcp_skc *)req); // the timer's reference
}

/* The full socket of a completed passive handshake, already ESTABLISHED
 * (it goes onto the accept queue right away; accept() must never see it
 * half made).  `seg' is the ACK that completed the handshake.  Process or
 * softirq context with interrupts on (it allocates); no lock held. */
static tcp_conn_t *tcp_create_openreq_child(tcp_conn_t *l,
					    const struct tcp_request_sock *req,
					    const sk_buff_t *seg,
					    const tcp_parsed_opts_t *opt)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(seg);
	tcp_conn_t *c = tcp_conn_alloc();
	uint32_t pmtu;

	if (!c)
		return NULL;
	c->local_ip = req->local_ip;
	c->local_port = req->local_port;
	c->remote_ip = req->remote_ip;
	c->remote_port = req->remote_port;
	c->dev = req->dev ? req->dev : l->dev;
	c->owner_socket = c; // the tcp layer's until accept()
	// What the application set on the listener.
	c->userlocks = l->userlocks;
	c->rcvbuf = l->rcvbuf;
	c->sndbuf = l->sndbuf;
	c->user_mss = l->user_mss;
	c->notsent_lowat = l->notsent_lowat;
	c->user_timeout_ms = l->user_timeout_ms;
	c->linger2_ms = l->linger2_ms;
	c->nodelay = l->nodelay;
	c->nonagle = l->nonagle & TCP_NAGLE_OFF;
	c->keepalive = l->keepalive;
	c->keepidle_ticks = l->keepidle_ticks;
	c->keepintvl_ticks = l->keepintvl_ticks;
	c->keepcnt = l->keepcnt;
	c->pacing_on = l->pacing_on;
	c->max_pacing_rate = l->max_pacing_rate;
	c->ca_ops = l->ca_ops ? l->ca_ops : tcp_ca_default();
	c->ca_setsockopt = l->ca_setsockopt;
	if (req->ecn_ok) {
		c->ecn_flags = TCP_ECN_OK;
		TCPX_INC(TCPX_ECNNEGOTIATED);
	}

	c->iss = req->iss;
	c->irs = req->irs;
	c->snd_una = c->snd_nxt = c->write_seq = c->pushed_seq = req->iss + 1;
	c->rcv_nxt = c->copied_seq = c->rcv_wup = req->irs + 1;
	c->ts_enabled = req->ts_ok;
	c->ts_recent = req->ts_recent;
	c->ts_recent_stamp = tcp_now();
	c->ts_offset = req->ts_offset;
	c->ws_enabled = req->wscale_ok;
	c->snd_wscale = req->snd_wscale;
	c->rcv_wscale = req->rcv_wscale;
	c->sack_ok = req->sack_ok;
	c->rcv_wnd = req->rcv_wnd;
	c->window_clamp = req->window_clamp;
	c->rcv_ssthresh = req->rcv_wnd;
	c->peer_mss = req->mss;
	c->mss_clamp = req->mss;
	c->tcp_header_len = sizeof(tcp_header_t) + (c->ts_enabled ? 12 : 0);

	/* The route, the path MTU and our MSS. */
	{
		struct ip_route rt;

		pmtu = tcp_dev_mtu(c->dev);
		if (ip_route_out(c->remote_ip, c->local_ip, NULL, &rt) == 0) {
			pmtu = rt.loopback ? rt.mtu :
					     ip_route_mtu(&rt, IP_PMTUDISC_WANT);
			if (rt.dst)
				dst_cache_set(&c->dst_cache, rt.dst,
					      c->remote_ip);
			ip_route_put(&rt);
		}
	}
	c->advmss = (uint16_t)tcp_min32(pmtu - 40, 65535u - 40u);
	if (c->user_mss && c->user_mss < c->advmss)
		c->advmss = c->user_mss;
	c->pmtu_cookie = pmtu;
	tcp_sync_mss(c, pmtu);

	/* The ACK that completed the handshake: its window (scaled now, it
	 * is no SYN), its timestamp, and the first RTT sample -- from the
	 * timestamp echo, or from the SYN-ACK's send time when it was never
	 * resent (Karn). */
	c->snd_wnd = (uint32_t)cb->window << (c->ws_enabled ? c->snd_wscale : 0);
	c->snd_wl1 = cb->seq;
	c->max_window = c->snd_wnd;
	if (c->ts_enabled && opt->ts_present)
		tcp_rtt_sample_ts(c, opt);
	else if (!req->num_retrans && req->snt_synack_us &&
		 tcp_now_us() > req->snt_synack_us)
		tcp_rtt_estimator(c, (uint32_t)(tcp_now_us() -
						req->snt_synack_us));
	c->state = TCP_STATE_ESTABLISHED;
	tcp_init_transfer(c);
	c->connect_done = 1;
	return c;
}

/* Publish a child under its listener's lock: in the established hash in
 * the place of `req' (NULL for a cookie: inserted fresh), on the accept
 * queue.  Returns 0, or -1 when the tuple is taken or the listener left
 * (the child is then not published).  The queue takes its own
 * reference.
 *
 * The child is in the bind hash and the accept queue's reference is taken
 * before it can be found: a reset for it, processed on any CPU right after
 * the hash insertion, then finishes a complete socket (unbound, its
 * protocol reference dropped) that the accept queue still holds, and that
 * accept() then drops as dead. */
static int tcp_publish_child(tcp_conn_t *l, struct tcp_request_sock *req,
			     tcp_conn_t *child)
{
	int rc;

	if (l->state != TCP_STATE_LISTEN || (req && req->dead))
		return -1;
	tcp_bhash_add((struct tcp_skc *)child);
	tcp_conn_hold(child); // the accept queue's reference
	if (req)
		rc = tcp_ehash_replace((struct tcp_skc *)req,
				       (struct tcp_skc *)child);
	else
		rc = tcp_ehash_insert_req((struct tcp_request_sock *)child);
	if (rc < 0) {
		tcp_conn_put(child); // the proto ref keeps it
		tcp_bhash_remove((struct tcp_skc *)child);
		return -1;
	}
	if (req) {
		/* The request sock is off the table: finish it as dropped
		 * (it is no longer hashed, so only the list and the
		 * references remain). */
		req->dead = 1;
		{
			struct tcp_request_sock **pp;

			for (pp = &l->reqsk_list; *pp; pp = &(*pp)->rsk_next) {
				if (*pp == req) {
					*pp = req->rsk_next;
					break;
				}
			}
			req->rsk_next = NULL;
			if (l->reqsk_qlen)
				l->reqsk_qlen--;
		}
		tcp_skc_timer_del((struct tcp_skc *)req, &req->timer);
		tcp_skc_put((struct tcp_skc *)req); // the table's reference
	}
	child->accept_next = NULL;
	if (l->accept_tail)
		l->accept_tail->accept_next = child;
	else
		l->accept_head = child;
	l->accept_tail = child;
	l->ack_backlog++;
	l->accept_ready = 1;
	NET_STATS_INC(NET_MIB_TCP_PASSIVEOPENS);
	return 0;
}

/* Undo a child that was made but never published. */
static void tcp_discard_child(tcp_conn_t *child)
{
	uint64_t f;

	tcp_lock_acquire(&child->lock, &f);
	child->owner_socket = NULL;
	child->state = TCP_STATE_CLOSED;
	tcp_clear_xmit_timers(child);
	if (child->proto_ref) {
		child->proto_ref = 0;
		tcp_conn_put(child); // the last reference: reaped
	}
	tcp_lock_release(&child->lock, f);
}

tcp_conn_t *tcp_check_req(struct tcp_request_sock *req, sk_buff_t *skb,
			  const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	tcp_conn_t *l = req->listener, *child;
	uint8_t flg = cb->tcp_flags;
	int paws_reject = 0;
	uint64_t f;

	tcp_lock_acquire(&l->lock, &f);
	if (req->dead || req->completing)
		goto drop_unlock;
	if (l->state != TCP_STATE_LISTEN) {
		tcp_reqsk_drop(req);
		tcp_lock_release(&l->lock, f);
		tcp_v4_send_reset(skb->dev, req->local_ip, req->remote_ip,
				  skb->data, skb->len);
		return NULL;
	}
	if (opt->ts_present && req->ts_ok)
		paws_reject = tcp_paws_reject_recent(req->ts_recent,
						     tcp_now() - 1, opt->tsval,
						     flg & TCP_RST);

	/* A retransmission of the SYN: our SYN-ACK was lost.  Answered at
	 * most once per 500 ms (an out-of-window reply), and the SYN-ACK
	 * timer then counts again from this resend, as after a fast
	 * retransmit -- the timer does not send another right behind it. */
	if (cb->seq == req->irs && (flg & (TCP_SYN | TCP_RST | TCP_ACK)) == TCP_SYN &&
	    !paws_reject) {
		uint64_t now = tcp_now();

		if (tcp_oow_rate_limited_stamp(&req->last_oow_tick, skb,
					       TCPX_ACKSKIPPEDSYNRECV))
			goto drop_unlock;
		if (tcp_synack_retransmit(req) == 0 &&
		    net_timer_pending(&req->timer))
			tcp_skc_timer_mod((struct tcp_skc *)req, &req->timer,
					  now + tcp_syn_rto_ticks(req->num_timeout));
		goto drop_unlock;
	}
	// An ACK for something else than our SYN-ACK: reset.
	if ((flg & TCP_ACK) && cb->ack_seq != req->iss + 1) {
		tcp_lock_release(&l->lock, f);
		tcp_v4_send_reset(skb->dev, req->local_ip, req->remote_ip,
				  skb->data, skb->len);
		return NULL;
	}
	// Outside the window we offered, or an old duplicate: ACK it.
	if (paws_reject ||
	    !tcp_in_window(cb->seq, cb->end_seq, req->irs + 1,
			   req->irs + 1 + req->rcv_wnd)) {
		if (paws_reject)
			TCPX_INC(TCPX_PAWSESTAB);
		if (!(flg & TCP_RST) &&
		    !tcp_oow_rate_limited_stamp(&req->last_oow_tick, skb,
						TCPX_ACKSKIPPEDSYNRECV))
			tcp_v4_send_ack(req->local_ip, req->remote_ip,
					req->local_port, req->remote_port,
					req->iss + 1, req->irs + 1,
					req->rcv_wnd >> req->rcv_wscale,
					req->ts_ok, tcp_ts_now(req->ts_offset),
					req->ts_recent);
		goto drop_unlock;
	}
	if (opt->ts_present && !after(cb->seq, req->irs + 1))
		req->ts_recent = opt->tsval;
	/* A reset (checked before the ACK, RFC 9293 3.10.7.4) or a SYN in
	 * the window: the embryonic connection is gone. */
	if (flg & (TCP_RST | TCP_SYN)) {
		NET_STATS_INC(NET_MIB_TCP_ATTEMPTFAILS);
		tcp_reqsk_drop(req);
		tcp_lock_release(&l->lock, f);
		if (!(flg & TCP_RST))
			tcp_v4_send_reset(skb->dev, req->local_ip,
					  req->remote_ip, skb->data, skb->len);
		return NULL;
	}
	if (!(flg & TCP_ACK))
		goto drop_unlock;
	/* TCP_DEFER_ACCEPT: a bare ACK does not complete the connection; the
	 * first data does. */
	if (req->num_timeout < l->defer_accept && cb->end_seq == req->irs + 1) {
		req->acked = 1;
		goto drop_unlock;
	}
	if (l->ack_backlog > l->max_ack_backlog) {
		/* No room on the accept queue: the ACK is dropped, the
		 * SYN-ACK timer resends and the peer acknowledges again. */
		TCPX_INC(TCPX_LISTENOVERFLOWS);
		TCPX_INC(TCPX_LISTENDROPS);
		NET_STATS_INC(NET_MIB_TCP_ACCEPTQFULL);
		req->acked = 1;
		goto drop_unlock;
	}
	req->completing = 1;
	tcp_skc_hold((struct tcp_skc *)req);
	tcp_lock_release(&l->lock, f);

	child = tcp_create_openreq_child(l, req, skb, opt);

	tcp_lock_acquire(&l->lock, &f);
	req->completing = 0;
	if (child && tcp_publish_child(l, req, child) == 0) {
		tcp_conn_hold(child); // the caller's
		tcp_lock_release(&l->lock, f);
		tcp_listener_notify(l);
		tcp_skc_put((struct tcp_skc *)req);
		return child;
	}
	/* Not published: the request sock stays (its timer keeps it from
	 * living for ever) and the peer's next segment tries again. */
	if (!req->dead && !net_timer_pending(&req->timer))
		tcp_skc_timer_mod((struct tcp_skc *)req, &req->timer,
				  tcp_now() +
					  tcp_syn_rto_ticks(req->num_timeout));
	tcp_lock_release(&l->lock, f);
	if (child)
		tcp_discard_child(child);
	tcp_skc_put((struct tcp_skc *)req);
	return NULL;
drop_unlock:
	tcp_lock_release(&l->lock, f);
	return NULL;
}

/* An ACK for a listener with no request sock: the handshake of a SYN we
 * answered with a cookie, if the cookie checks out. */
tcp_conn_t *tcp_cookie_child(tcp_conn_t *l, sk_buff_t *skb,
			     const tcp_parsed_opts_t *opt, uint32_t saddr,
			     uint32_t daddr, net_device_t *dev, int *reset)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	const uint8_t *th = skb->data;
	uint16_t sport = tcp_get_be16(th), dport = tcp_get_be16(th + 2);
	struct tcp_request_sock req;
	tcp_conn_t *child;
	uint16_t mss;
	uint64_t f, last = l->synq_overflow_tick;
	int wscale_ok = 0, sack_ok = 0, ecn_ok = 0;
	uint8_t snd_wscale = 0;

	*reset = 1;
	/* Cookies are only looked at while they may be out there: within
	 * two cookie lifetimes of the last SYN queue overflow. */
	if (!last || tcp_now() - last >
			     tcp_ms(TCP_SYNCOOKIE_VALID_MS))
		return NULL;
	mss = tcp_cookie_check(saddr, daddr, sport, dport, cb->seq - 1,
			       cb->ack_seq - 1);
	if (!mss) {
		TCPX_INC(TCPX_SYNCOOKIEFAILED);
		NET_STATS_INC(NET_MIB_TCP_SYNCOOKIEFAIL);
		return NULL;
	}
	mm_memset(&req, 0, sizeof(req));
	req.ts_offset = tcp_ts_offset_for(daddr, saddr, dport, sport);
	if (opt->ts_present) {
		if (!tcp_cookie_decode_options(opt->tsecr,
					       tcp_ts_now(req.ts_offset),
					       &wscale_ok, &snd_wscale,
					       &sack_ok, &ecn_ok)) {
			TCPX_INC(TCPX_SYNCOOKIEFAILED);
			NET_STATS_INC(NET_MIB_TCP_SYNCOOKIEFAIL);
			return NULL;
		}
	}
	TCPX_INC(TCPX_SYNCOOKIERECV);
	NET_STATS_INC(NET_MIB_TCP_SYNCOOKIERECV);
	req.skc_kind = TCP_SKC_REQ;
	req.local_ip = daddr;
	req.local_port = dport;
	req.remote_ip = saddr;
	req.remote_port = sport;
	req.dev = dev;
	req.irs = cb->seq - 1;
	req.iss = cb->ack_seq - 1;
	req.mss = mss;
	req.ts_ok = opt->ts_present;
	req.ts_recent = opt->tsval;
	req.wscale_ok = (uint8_t)wscale_ok;
	req.snd_wscale = snd_wscale;
	req.sack_ok = (uint8_t)sack_ok;
	req.ecn_ok = (uint8_t)(ecn_ok && tcp_sysctl_ecn);
	tcp_req_init_window(l, &req, tcp_dev_mtu(dev) - 40);

	*reset = 0; // a valid cookie: from here on a failure is a drop
	tcp_lock_acquire(&l->lock, &f);
	if (l->state != TCP_STATE_LISTEN || l->ack_backlog > l->max_ack_backlog) {
		if (l->state == TCP_STATE_LISTEN) {
			TCPX_INC(TCPX_LISTENOVERFLOWS);
			TCPX_INC(TCPX_LISTENDROPS);
		}
		tcp_lock_release(&l->lock, f);
		return NULL;
	}
	tcp_lock_release(&l->lock, f);
	child = tcp_create_openreq_child(l, &req, skb, opt);
	if (!child)
		return NULL;
	tcp_lock_acquire(&l->lock, &f);
	if (tcp_publish_child(l, NULL, child) < 0) {
		tcp_lock_release(&l->lock, f);
		tcp_discard_child(child);
		return NULL;
	}
	tcp_conn_hold(child); // the caller's
	tcp_lock_release(&l->lock, f);
	tcp_listener_notify(l);
	return child;
}

// ---------------------------------------------------------------------------
// TIME_WAIT socks.
// ---------------------------------------------------------------------------
static void tw_timer_fn(net_timer_t *t);

void tcp_tw_free(struct tcp_tw_sock *tw)
{
	WARN_ON(tw->skc_hashed || tw->skc_bound);
	WARN_ON(net_timer_pending(&tw->timer));
	tw->skc_kind = 0;
	tw_reserve_put(tw);
}

/* The last steps of a TIME_WAIT sock that was taken off the established
 * hash by whoever set `dead' first. */
void tcp_tw_kill(struct tcp_tw_sock *tw)
{
	tcp_bhash_remove((struct tcp_skc *)tw);
	tcp_skc_timer_del((struct tcp_skc *)tw, &tw->timer);
	__atomic_fetch_sub(&g_tw_count, 1, __ATOMIC_RELAXED);
	TCPX_INC(TCPX_TWKILLED);
	tcp_skc_put((struct tcp_skc *)tw); // the table's reference
}

static int tw_claim(struct tcp_tw_sock *tw)
{
	return __atomic_exchange_n(&tw->dead, 1, __ATOMIC_ACQ_REL) == 0;
}

/* Hand a connection over to a TIME_WAIT sock (`substate' TIME_WAIT, or
 * FIN_WAIT_2 for an orphan waiting for the peer's FIN) for `timeo' ticks,
 * and finish the full socket.  Under conn->lock.
 *
 * The sock is complete before it becomes visible: whoever finds it in the
 * established hash may kill it at once -- the receive path (a reset, new
 * data, a SYN), a connect() over the tuple on another CPU -- and must find
 * it bound, counted and timed, with a reference for each of those.  Its
 * own lock is held until it is published, so the timer, should it expire
 * on another CPU before that, cannot finish a sock that is not out yet. */
void tcp_time_wait(tcp_conn_t *c, int substate, uint64_t timeo)
{
	struct tcp_tw_sock *tw = NULL;
	uint64_t f;

	if (tcp_tw_count() < TCP_MAX_TW_BUCKETS)
		tw = tw_reserve_take();
	if (!tw) {
		TCPX_INC(TCPX_TWKILLED);
		tcp_done(c);
		return;
	}
	mm_memset(tw, 0, sizeof(*tw));
	tw->skc_kind = TCP_SKC_TW;
	tw->state = substate;
	tw->refcount = 1; // the table's (the hash takes it over below)
	tw->local_ip = c->local_ip;
	tw->local_port = c->local_port;
	tw->remote_ip = c->remote_ip;
	tw->remote_port = c->remote_port;
	tw->dev = c->dev;
	tw->lock = (spinlock_t)SPINLOCK_INIT("tcp_tw");
	net_timer_setup(&tw->timer, tw_timer_fn);
	tw->substate = (uint8_t)substate;
	tw->snd_nxt = c->snd_nxt;
	tw->rcv_nxt = c->rcv_nxt;
	tw->rcv_wnd = tcp_receive_window(c);
	tw->rcv_wscale = c->ws_enabled ? c->rcv_wscale : 0;
	tw->ts_enabled = c->ts_enabled;
	tw->ts_recent = c->ts_recent;
	tw->ts_recent_stamp = c->ts_recent_stamp;
	tw->ts_offset = c->ts_offset;
	tw->entry_stamp = tcp_now();

	/* conn->lock -> tw->lock: the sock is new, nobody else can hold its
	 * lock yet (tcp.h). */
	tcp_lock_acquire(&tw->lock, &f);
	tcp_bhash_add((struct tcp_skc *)tw);
	__atomic_fetch_add(&g_tw_count, 1, __ATOMIC_RELAXED);
	tcp_skc_timer_mod((struct tcp_skc *)tw, &tw->timer, tcp_now() + timeo);
	if (tcp_ehash_replace((struct tcp_skc *)c, (struct tcp_skc *)tw) < 0) {
		/* `c' was no longer hashed: never published.  Undone in
		 * reverse; a timer callback already waiting for the lock
		 * finds it dead and drops only its own reference. */
		tw->dead = 1;
		tcp_skc_timer_del((struct tcp_skc *)tw, &tw->timer);
		tcp_bhash_remove((struct tcp_skc *)tw);
		__atomic_fetch_sub(&g_tw_count, 1, __ATOMIC_RELAXED);
		tcp_lock_release(&tw->lock, f);
		tcp_skc_put((struct tcp_skc *)tw); // the table's: reaped
		tcp_done(c);
		return;
	}
	tcp_lock_release(&tw->lock, f);
	tcp_bhash_remove((struct tcp_skc *)c);
	TCPX_INC(TCPX_TWCREATED);
	NET_STATS_INC(NET_MIB_TCP_TWCREATED);
	tcp_done(c);
}

static void tw_timer_fn(net_timer_t *t)
{
	struct tcp_tw_sock *tw =
		(struct tcp_tw_sock *)((char *)t -
				       __builtin_offsetof(struct tcp_tw_sock,
							  timer));
	uint64_t f;
	int mine;

	tcp_lock_acquire(&tw->lock, &f);
	mine = tw_claim(tw);
	if (mine && tw->substate == TCP_STATE_FIN_WAIT_2)
		TCPX_INC(TCPX_FINWAIT2TIMEOUT);
	tcp_lock_release(&tw->lock, f);
	if (mine) {
		tcp_ehash_remove((struct tcp_skc *)tw);
		tcp_bhash_remove((struct tcp_skc *)tw);
		__atomic_fetch_sub(&g_tw_count, 1, __ATOMIC_RELAXED);
		tcp_skc_put((struct tcp_skc *)tw); // the table's
	}
	tcp_skc_put((struct tcp_skc *)tw); // the timer's
}

static void tw_send_ack(struct tcp_tw_sock *tw)
{
	tcp_v4_send_ack(tw->local_ip, tw->remote_ip, tw->local_port,
			tw->remote_port, tw->snd_nxt, tw->rcv_nxt,
			tw->rcv_wnd >> tw->rcv_wscale, tw->ts_enabled,
			tcp_ts_now(tw->ts_offset), tw->ts_recent);
}

/* The answer to a segment a TIME_WAIT sock rejects (out of window, PAWS,
 * an old duplicate): an ACK, at most one per TCP_INVALID_RATELIMIT_MS
 * unless the segment carries data (RFC 5961 5 -- two confused stacks, or
 * a reflector behind a spoofed source, cannot keep an exchange going at
 * line rate).  Under tw->lock. */
static void tw_send_ack_limited(struct tcp_tw_sock *tw, const sk_buff_t *skb,
				enum tcp_xmib mib)
{
	if (!tcp_oow_rate_limited_stamp(&tw->last_oow_tick, skb, mib))
		tw_send_ack(tw);
}

static void tw_kill_locked(struct tcp_tw_sock *tw)
{
	if (tw_claim(tw)) {
		tcp_ehash_remove((struct tcp_skc *)tw);
		tcp_tw_kill(tw);
	}
}

int tcp_timewait_state_process(struct tcp_tw_sock *tw, sk_buff_t *skb,
			       const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint8_t flg = cb->tcp_flags;
	int paws_reject = 0, rc = 0;
	uint64_t f;

	tcp_lock_acquire(&tw->lock, &f);
	if (tw->dead)
		goto out;
	if (opt->ts_present && tw->ts_enabled && tw->ts_recent_stamp)
		paws_reject = tcp_paws_reject_recent(tw->ts_recent,
						     tw->ts_recent_stamp,
						     opt->tsval, flg & TCP_RST);

	if (tw->substate == TCP_STATE_FIN_WAIT_2) {
		// The checks of the full state machine, in short.
		if (paws_reject ||
		    !tcp_in_window(cb->seq, cb->end_seq, tw->rcv_nxt,
				   tw->rcv_nxt + tw->rcv_wnd)) {
			if (!(flg & TCP_RST))
				tw_send_ack_limited(tw, skb,
						    TCPX_ACKSKIPPEDFINWAIT2);
			goto out;
		}
		if (flg & TCP_RST) {
			tw_kill_locked(tw);
			goto out;
		}
		if ((flg & TCP_SYN) && !before(cb->seq, tw->rcv_nxt))
			goto kill_with_rst;
		// a duplicate ACK
		if (!(flg & TCP_ACK) || !after(cb->end_seq, tw->rcv_nxt) ||
		    cb->end_seq == cb->seq)
			goto out;
		/* New data after our side closed: nobody can read it. */
		if (!(flg & TCP_FIN) || cb->end_seq != tw->rcv_nxt + 1)
			goto kill_with_rst;
		// The FIN: now the real TIME_WAIT.
		tw->substate = TCP_STATE_TIME_WAIT;
		tw->state = TCP_STATE_TIME_WAIT;
		tw->rcv_nxt = cb->end_seq;
		if (opt->ts_present) {
			tw->ts_recent = opt->tsval;
			tw->ts_recent_stamp = tcp_now();
		}
		tcp_skc_timer_mod((struct tcp_skc *)tw, &tw->timer,
				  tcp_now() + tcp_ms(TCP_TIMEWAIT_LEN_MS));
		tw_send_ack(tw);
		goto out;
	}

	// TIME_WAIT proper (RFC 9293 3.10.7.4, RFC 1337 aside).
	if (!paws_reject && cb->seq == tw->rcv_nxt &&
	    (cb->seq == cb->end_seq || (flg & TCP_RST))) {
		// in window: only a reset or a bare ACK can be here
		if (flg & TCP_RST) {
			tw_kill_locked(tw);
			goto out;
		}
		tcp_skc_timer_mod((struct tcp_skc *)tw, &tw->timer,
				  tcp_now() + tcp_ms(TCP_TIMEWAIT_LEN_MS));
		if (opt->ts_present) {
			tw->ts_recent = opt->tsval;
			tw->ts_recent_stamp = tcp_now();
		}
		goto out;
	}
	/* A new SYN may reopen the tuple when it cannot be an old
	 * duplicate: a higher sequence number, or with timestamps a newer
	 * TSval (RFC 6191).  The new connection's ISN goes beyond the old
	 * sequence space. */
	if ((flg & (TCP_SYN | TCP_RST | TCP_ACK)) == TCP_SYN && !paws_reject &&
	    (after(cb->seq, tw->rcv_nxt) ||
	     (opt->ts_present && tw->ts_enabled &&
	      (int32_t)(tw->ts_recent - opt->tsval) < 0))) {
		uint32_t isn = tw->snd_nxt + 65535u + 2u;

		if (!isn)
			isn = 1;
		cb->first_tx_us = (1ULL << 32) | isn;
		tw_kill_locked(tw);
		TCPX_INC(TCPX_TWRECYCLED);
		rc = 1;
		goto out;
	}
	if (paws_reject)
		TCPX_INC(TCPX_PAWSESTAB);
	if (!(flg & TCP_RST)) {
		if (paws_reject || (flg & TCP_ACK))
			tcp_skc_timer_mod((struct tcp_skc *)tw, &tw->timer,
					  tcp_now() +
						  tcp_ms(TCP_TIMEWAIT_LEN_MS));
		/* A retransmitted FIN (it occupies sequence space) is always
		 * answered: the peer waits for this ACK to finish. */
		tw_send_ack_limited(tw, skb, TCPX_ACKSKIPPEDTIMEWAIT);
	}
	goto out;

kill_with_rst:
	tw_kill_locked(tw);
	tcp_lock_release(&tw->lock, f);
	tcp_v4_send_reset(skb->dev, tw->local_ip, tw->remote_ip, skb->data,
			  skb->len);
	return 0;
out:
	tcp_lock_release(&tw->lock, f);
	return rc;
}

int tcp_sysctl_tw_reuse = 2; // 0 never, 1 always, 2 loopback only

static int tcp_ipv4_is_loopback(uint32_t addr)
{
	return (addr >> 24) == 127;
}

/* connect() to the 4-tuple of a TIME_WAIT sock (`c' is the new
 * connection, not visible yet).  Only an old connection that used
 * timestamps qualifies -- PAWS then keeps its delayed segments out of the
 * new one (RFC 6191) -- and only from TIME_WAIT proper:
 *   * a port the application bound itself: reused at once;
 *   * an ephemeral port: per tcp_sysctl_tw_reuse (by default only on
 *     loopback addresses), and not before TCP_TW_REUSE_DELAY_MS after the
 *     sock was made.
 * The new connection's sequence space starts beyond the old one's and it
 * inherits the timestamp state.  Called under the established-hash
 * bucket lock: claims the sock (dead) when it may be reused; the caller
 * unlinks it and calls tcp_tw_kill. */
int tcp_twsk_unique(struct tcp_tw_sock *tw, int user_port, tcp_conn_t *c)
{
	int reuse = tcp_sysctl_tw_reuse;
	uint64_t stamp = tw->ts_recent_stamp;
	uint32_t seq;

	if (tw->substate == TCP_STATE_FIN_WAIT_2)
		reuse = 0;
	if (reuse == 2 && !tcp_ipv4_is_loopback(tw->remote_ip) &&
	    !tcp_ipv4_is_loopback(tw->local_ip))
		reuse = 0;
	if (!tw->ts_enabled || !stamp)
		return 0;
	if (!user_port &&
	    (!reuse ||
	     tcp_now() - tw->entry_stamp <= tcp_ms(TCP_TW_REUSE_DELAY_MS)))
		return 0;
	if (!tw_claim(tw))
		return 0;
	seq = tw->snd_nxt + 65535u + 2u;
	if (!seq)
		seq = 1;
	c->iss = seq;
	c->snd_una = c->snd_nxt = c->write_seq = c->pushed_seq = seq;
	c->ts_recent = tw->ts_recent;
	c->ts_recent_stamp = stamp;
	return 1;
}
