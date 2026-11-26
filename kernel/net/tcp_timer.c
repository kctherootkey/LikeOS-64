// LikeOS TCP: timers
//
// Every connection has four timers on the net_timer wheel, armed only
// while there is something to time -- no periodic sweep over all
// connections:
//   retransmit  RTO of the oldest unacknowledged segment (data, SYN,
//               FIN), the tail loss probe, the end of RACK's reordering
//               window, the zero-window probe (persist), a retry after a
//               transmission that failed locally (pending_timer says
//               which);
//   delack      the delayed ACK;
//   keepalive   SO_KEEPALIVE probing, and an orphan's FIN_WAIT2 wait when
//               TCP_LINGER2 is longer than TIME_WAIT;
//   pacing      the next paced transmission.
// A pending timer holds a reference on its connection (tcp_skc_timer_mod
// takes it when the timer goes from idle to armed; the callback, or a
// successful del, drops it), so the connection outlives every pending
// callback without the owner ever waiting for one.  Callbacks run in
// softirq context, take the connection's lock, and check the state first:
// a connection that finished meanwhile is left alone.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/ke/syscall.h>

static void tcp_write_timer_fn(net_timer_t *t);
static void tcp_delack_timer_fn(net_timer_t *t);
static void tcp_keepalive_timer_fn(net_timer_t *t);
static void tcp_pacing_timer_fn(net_timer_t *t);

#define CONN_OF(t, member) \
	((tcp_conn_t *)((char *)(t) - __builtin_offsetof(tcp_conn_t, member)))

void tcp_skc_timer_mod(struct tcp_skc *skc, net_timer_t *t, uint64_t expires)
{
	if (!net_timer_mod(t, expires))
		tcp_skc_hold(skc); // the arming's reference
}

void tcp_skc_timer_del(struct tcp_skc *skc, net_timer_t *t)
{
	if (net_timer_del(t))
		tcp_skc_put(skc); // the arming's reference, unused
}

void tcp_init_xmit_timers(tcp_conn_t *c)
{
	net_timer_setup(&c->retransmit_timer, tcp_write_timer_fn);
	net_timer_setup(&c->delack_timer, tcp_delack_timer_fn);
	net_timer_setup(&c->keepalive_timer, tcp_keepalive_timer_fn);
	net_timer_setup(&c->pacing_timer, tcp_pacing_timer_fn);
}

void tcp_clear_xmit_timers(tcp_conn_t *c)
{
	c->pending_timer = 0;
	c->ack_pending = 0;
	tcp_skc_timer_del((struct tcp_skc *)c, &c->retransmit_timer);
	tcp_skc_timer_del((struct tcp_skc *)c, &c->delack_timer);
	tcp_skc_timer_del((struct tcp_skc *)c, &c->keepalive_timer);
	tcp_skc_timer_del((struct tcp_skc *)c, &c->pacing_timer);
}

void tcp_reset_pacing_timer(tcp_conn_t *c, uint64_t expires)
{
	tcp_skc_timer_mod((struct tcp_skc *)c, &c->pacing_timer, expires);
}

static uint64_t tcp_clamp_ticks(uint64_t when)
{
	uint64_t max = tcp_us_to_ticks(TCP_RTO_MAX_US);

	if (when > max)
		when = max;
	return when ? when : 1;
}

void tcp_reset_xmit_timer(tcp_conn_t *c, uint8_t what, uint64_t when)
{
	when = tcp_clamp_ticks(when);
	c->pending_timer = what;
	c->retransmit_expires = tcp_now() + when;
	tcp_skc_timer_mod((struct tcp_skc *)c, &c->retransmit_timer,
			  c->retransmit_expires);
}

void tcp_clear_xmit_timer(tcp_conn_t *c)
{
	c->pending_timer = 0;
	tcp_skc_timer_del((struct tcp_skc *)c, &c->retransmit_timer);
}

void tcp_reset_delack_timer(tcp_conn_t *c, uint64_t expires)
{
	tcp_skc_timer_mod((struct tcp_skc *)c, &c->delack_timer, expires);
}

void tcp_clear_delack_timer(tcp_conn_t *c)
{
	tcp_skc_timer_del((struct tcp_skc *)c, &c->delack_timer);
}

void tcp_reset_keepalive_timer(tcp_conn_t *c, uint64_t when)
{
	tcp_skc_timer_mod((struct tcp_skc *)c, &c->keepalive_timer,
			  tcp_now() + (when ? when : 1));
}

void tcp_delete_keepalive_timer(tcp_conn_t *c)
{
	tcp_skc_timer_del((struct tcp_skc *)c, &c->keepalive_timer);
}

void tcp_set_keepalive(tcp_conn_t *c, int on)
{
	if (on && c->state != TCP_STATE_CLOSED && c->state != TCP_STATE_LISTEN &&
	    c->state != TCP_STATE_SYN_SENT)
		tcp_reset_keepalive_timer(c, c->keepidle_ticks);
	else if (!on)
		tcp_delete_keepalive_timer(c);
}

// ---------------------------------------------------------------------------
// Timeouts.
// ---------------------------------------------------------------------------
uint64_t tcp_rto_ticks(const tcp_conn_t *c)
{
	uint64_t rto = c->rto_us ? c->rto_us : TCP_RTO_INITIAL_US;
	uint32_t shift = c->rto_backoff > 16 ? 16 : c->rto_backoff;

	rto <<= shift;
	if (rto > TCP_RTO_MAX_US)
		rto = TCP_RTO_MAX_US;
	return tcp_us_to_ticks(rto);
}

/* The wait after `sent' resends of a SYN or SYN-ACK: 1, 2, 4, ... s
 * (RFC 6298 2.1 and 5.5). */
uint64_t tcp_syn_rto_ticks(uint32_t sent)
{
	uint64_t ms;

	if (sent > 16)
		sent = 16;
	ms = (uint64_t)TCP_TIMEOUT_INIT_MS << sent;
	if (ms > TCP_RTO_MAX_US / 1000u)
		ms = TCP_RTO_MAX_US / 1000u;
	return tcp_ms(ms);
}

/* Zero-window probes: from max(RTO, RTO_MIN), backing off. */
uint64_t tcp_probe0_when(const tcp_conn_t *c)
{
	uint64_t base = c->rto_us > TCP_RTO_MIN_US ? c->rto_us : TCP_RTO_MIN_US;
	uint32_t shift = c->rto_backoff > 16 ? 16 : c->rto_backoff;

	base <<= shift;
	if (base > TCP_RTO_MAX_US)
		base = TCP_RTO_MAX_US;
	return tcp_us_to_ticks(base);
}

/* The whole backoff series of `retries' handshake resends: the hard
 * bound on a handshake whose transmissions keep failing locally (those
 * do not count as attempts). */
static uint64_t tcp_handshake_timeout_ticks(uint32_t retries)
{
	if (retries > 16)
		retries = 16;
	return tcp_ms(((2ULL << retries) - 1) * TCP_TIMEOUT_INIT_MS);
}

/* The RTO with its backoff, capped at TCP_RTO_MAX: below the cap the
 * connection still "is alive" (it sent something within the last
 * maximal RTO). */
static uint64_t tcp_rto_backoff_us(const tcp_conn_t *c)
{
	uint64_t rto = c->rto_us ? c->rto_us : TCP_RTO_INITIAL_US;
	uint32_t shift = c->rto_backoff > 16 ? 16 : c->rto_backoff;

	rto <<= shift;
	return rto > TCP_RTO_MAX_US ? TCP_RTO_MAX_US : rto;
}

/* How many probes / retransmissions an orphan gets.  None once its backoff
 * reached the maximal RTO (it is not making progress), some while it
 * still is -- fewer when an ICMP error said something is wrong. */
static int tcp_orphan_retries(const tcp_conn_t *c, int alive)
{
	int retries = 0; // no administrator override

	if (c->soft_error && !alive)
		retries = 0;
	if (retries == 0 && alive)
		retries = TCP_ORPHAN_RETRIES_ALIVE;
	return retries;
}

/* An orphan when the host is short of resources: too many orphans (the
 * limit tightened for one that has been silent long, or got ICMP errors)
 * or TCP memory over its hard limit while it holds send data.  It is then
 * finished -- with a reset when it sent recently or the peer's window is
 * shut (the peer is still there to hear it).  1 when it was finished. */
static int tcp_out_of_resources(tcp_conn_t *c, int do_reset)
{
	uint64_t now = tcp_now(), idle = now - c->lsndtime;
	int shift = 0;

	if (idle > 2 * tcp_us_to_ticks(TCP_RTO_MAX_US) || !do_reset)
		shift++;
	if (c->soft_error)
		shift++;
	if (((uint64_t)tcp_orphan_count() << shift) > TCP_MAX_ORPHANS ||
	    (c->wmem_queued > TCP_WMEM_MIN && tcp_mem_over_limit())) {
		if (idle <= tcp_ms(TCP_TIMEWAIT_LEN_MS) ||
		    (!c->snd_wnd && !c->packets_out))
			do_reset = 1;
		if (do_reset)
			tcp_send_active_reset(c);
		TCPX_INC(TCPX_ABORTONMEMORY);
		tcp_done(c);
		return 1;
	}
	return 0;
}

static void tcp_write_err(tcp_conn_t *c)
{
	int err = c->soft_error ? c->soft_error : ETIMEDOUT;

	TCPX_INC(TCPX_ABORTONTIMEOUT);
	if (c->state == TCP_STATE_SYN_SENT ||
	    c->state == TCP_STATE_SYN_RECEIVED)
		NET_STATS_INC(NET_MIB_TCP_ATTEMPTFAILS);
	tcp_reset(c, err);
}

/* Has the connection run out of retransmissions?  Then it fails. */
static int tcp_write_timeout(tcp_conn_t *c)
{
	uint64_t now = tcp_now();
	int expired = 0;

	if (c->state == TCP_STATE_SYN_SENT ||
	    c->state == TCP_STATE_SYN_RECEIVED) {
		uint32_t retries = c->syn_retries ? c->syn_retries :
				   c->state == TCP_STATE_SYN_SENT ?
						    TCP_SYN_RETRIES :
						    TCP_SYNACK_RETRIES;

		expired = c->retransmits >= retries ||
			  (c->retrans_stamp &&
			   now - c->retrans_stamp >=
				   tcp_handshake_timeout_ticks(retries));
	} else {
		sk_buff_t *head = skb_peek(&c->rtx_queue);
		/* Data at stake gets the full budget, in every state: a
		 * graceful close must still deliver what was written before
		 * it.  Only a FIN alone at the head gets the short one. */
		int data = head && head->len > 0;
		uint32_t retries = (c->state == TCP_STATE_ESTABLISHED || data) ?
					   TCP_RETRIES2 :
					   TCP_ORPHAN_RETRIES;

		expired = c->retransmits >= retries;
		/* An orphan on a host short of orphan slots or memory goes
		 * first. */
		if (!expired && tcp_is_orphan(c)) {
			int alive = tcp_rto_backoff_us(c) < TCP_RTO_MAX_US;

			if (tcp_out_of_resources(c, alive || c->retransmits <
								     retries))
				return 1;
		}
		/* Black-hole detection: after a few timeouts the path may
		 * drop large segments silently (off by default), and the
		 * cached route is looked up again. */
		if (!expired && c->retransmits >= TCP_RETRIES1) {
			tcp_mtu_probing(c);
			dst_cache_reset(&c->dst_cache);
		}
		if ((c->state == TCP_STATE_FIN_WAIT_1 ||
		     c->state == TCP_STATE_CLOSING ||
		     c->state == TCP_STATE_LAST_ACK) &&
		    now - c->last_rx_tick > tcp_ms(TCP_CLOSING_IDLE_MS))
			expired = 1;
	}
	if (c->user_timeout_ms && c->retrans_stamp &&
	    now - c->retrans_stamp >= tcp_ms(c->user_timeout_ms))
		expired = 1;
	if (expired)
		tcp_write_err(c);
	return expired;
}

/* Retransmissions into a window the peer shut under our data: give up
 * when TCP_USER_TIMEOUT has passed since the first of them (the peer's
 * window answers do not count as progress), else only after twice the
 * maximal RTO with neither an answer nor progress. */
static int tcp_rtx_probe0_timed_out(const tcp_conn_t *c)
{
	uint64_t now = tcp_now(), timeout = 2 * tcp_us_to_ticks(TCP_RTO_MAX_US);
	uint64_t rtx_delta = c->retrans_stamp ? now - c->retrans_stamp : 0;

	if (c->user_timeout_ms) {
		uint64_t ut = tcp_ms(c->user_timeout_ms);

		if (rtx_delta > ut)
			return 1;
		if (ut < timeout)
			timeout = ut;
	}
	if (now - c->last_rx_tick <= timeout)
		return 0;
	return rtx_delta > timeout;
}

/* The RTO fired (RFC 6298 5.4-5.6): resend the oldest unacknowledged
 * segment, back off, and treat the window as lost (RFC 5681 3.1). */
static void tcp_retransmit_timer(tcp_conn_t *c)
{
	sk_buff_t *head;
	int handshake = c->state == TCP_STATE_SYN_SENT ||
			c->state == TCP_STATE_SYN_RECEIVED;
	int rc;

	if (c->fin_pending &&
	    (c->state == TCP_STATE_FIN_WAIT_1 || c->state == TCP_STATE_LAST_ACK ||
	     c->state == TCP_STATE_CLOSING))
		tcp_send_fin(c);
	if (!c->packets_out) {
		if (c->state == TCP_STATE_SYN_SENT &&
		    skb_queue_empty(&c->rtx_queue)) {
			// the SYN never got a buffer
			if (!tcp_write_timeout(c))
				tcp_send_syn(c);
		}
		return;
	}
	head = skb_peek(&c->rtx_queue);
	if (!head)
		return;
	if (!c->snd_wnd && !handshake && !tcp_is_orphan(c) &&
	    !(TCP_SKB_CB(head)->tcp_flags & (TCP_SYN | TCP_FIN))) {
		/* The peer shrank its window to nothing under our data:
		 * the retransmissions are window probes, and the connection
		 * does not time out while the peer answers them -- unless
		 * TCP_USER_TIMEOUT says how long it may go without
		 * progress.  (An orphan takes the normal path below: it
		 * may not hang on for ever.) */
		if (tcp_rtx_probe0_timed_out(c)) {
			tcp_write_err(c);
			return;
		}
		tcp_enter_loss(c);
		tcp_retransmit_skb(c, head);
		dst_cache_reset(&c->dst_cache);
		goto out_reset_timer;
	}
	TCPX_INC(TCPX_TIMEOUTS);
	if (tcp_write_timeout(c))
		return;
	if (c->total_rto < 0xFFFF)
		c->total_rto++;
	if (!handshake)
		tcp_enter_loss(c);
	rc = tcp_retransmit_skb(c, head);
	if (rc < 0 && rc != -EAGAIN) {
		/* Not lost -- it could not leave this host (no buffer).
		 * Neither the give-up budget nor the backoff advance; try
		 * again shortly. */
		tcp_reset_xmit_timer(c, TCP_TIME_RETRANS,
				     tcp_ms(TCP_LOCAL_DROP_RETRY_MS));
		return;
	}
	if (handshake)
		TCPX_INC(TCPX_SYNRETRANS);
	if (c->rto_backoff < 255)
		c->rto_backoff++;
	if (c->retransmits < 255)
		c->retransmits++;
out_reset_timer:
	{
		uint64_t when = handshake ? tcp_syn_rto_ticks(c->retransmits) :
					    tcp_rto_ticks(c);

		/* TCP_USER_TIMEOUT: the next timeout no later than the
		 * moment the connection must fail. */
		if (c->user_timeout_ms && c->retrans_stamp) {
			uint64_t lim = tcp_ms(c->user_timeout_ms);
			uint64_t used = tcp_now() - c->retrans_stamp;
			uint64_t left = used < lim ? lim - used : 1;

			if (when > left)
				when = left;
		}
		tcp_reset_xmit_timer(c, TCP_TIME_RETRANS, when);
	}
}

/* The zero-window probe timer: the peer's window is closed and nothing is
 * in flight to learn when it opens (RFC 9293 3.8.6.1).
 *
 * RFC 1122 4.2.2.17: the sender keeps the connection open for as long as
 * the receiver answers the probes -- every answer resets probes_out
 * (tcp_ack), so a live peer is probed indefinitely.  Two cases are
 * bounded all the same: TCP_USER_TIMEOUT, counted from the first probe of
 * the episode (answers that leave the window shut are no progress), and an
 * orphan, which nobody will ever read from or write to again -- it gives up
 * once its backoff reached the maximal RTO, and earlier when the host is
 * short of orphan slots or memory. */
static void tcp_probe_timer(tcp_conn_t *c)
{
	uint64_t now = tcp_now();
	int max_probes;

	if (c->packets_out || !tcp_send_head(c)) {
		c->probes_out = 0;
		c->probes_tstamp = 0;
		return;
	}
	if (!c->probes_tstamp) {
		c->probes_tstamp = now ? now : 1;
	} else if (c->user_timeout_ms &&
		   now - c->probes_tstamp >= tcp_ms(c->user_timeout_ms)) {
		goto abort;
	}
	max_probes = TCP_RETRIES2;
	if (tcp_is_orphan(c)) {
		int alive = tcp_rto_backoff_us(c) < TCP_RTO_MAX_US;

		max_probes = tcp_orphan_retries(c, alive);
		if (!alive && c->rto_backoff >= max_probes)
			goto abort;
		if (tcp_out_of_resources(c, 1))
			return;
	}
	if (c->probes_out >= max_probes)
		goto abort;
	tcp_send_probe0(c);
	return;
abort:
	tcp_write_err(c);
}

static void tcp_write_timer_fn(net_timer_t *t)
{
	tcp_conn_t *c = CONN_OF(t, retransmit_timer);
	uint64_t f;
	uint8_t ev;

	tcp_lock_acquire(&c->lock, &f);
	if (c->state == TCP_STATE_CLOSED || c->state == TCP_STATE_LISTEN ||
	    !c->pending_timer)
		goto out;
	if (tcp_now() < c->retransmit_expires) {
		tcp_skc_timer_mod((struct tcp_skc *)c, &c->retransmit_timer,
				  c->retransmit_expires);
		goto out;
	}
	ev = c->pending_timer;
	c->pending_timer = 0;
	switch (ev) {
	case TCP_TIME_PROBE0:
		tcp_probe_timer(c);
		break;
	case TCP_TIME_LOSS_PROBE:
		tcp_send_loss_probe(c);
		break;
	case TCP_TIME_REO_TIMEOUT:
		tcp_rack_reo_timeout(c);
		break;
	default:
		tcp_retransmit_timer(c);
		break;
	}
out:
	tcp_lock_release(&c->lock, f);
	tcp_conn_put(c); // the arming's reference
}

static void tcp_delack_timer_fn(net_timer_t *t)
{
	tcp_conn_t *c = CONN_OF(t, delack_timer);
	uint64_t f;

	tcp_lock_acquire(&c->lock, &f);
	if (c->state == TCP_STATE_CLOSED || !(c->ack_pending & TCP_ACK_TIMER))
		goto out;
	if (tcp_now() < c->ack_timeout) {
		tcp_skc_timer_mod((struct tcp_skc *)c, &c->delack_timer,
				  c->ack_timeout);
		goto out;
	}
	c->ack_pending &= (uint8_t)~TCP_ACK_TIMER;
	if (c->ack_pending & TCP_ACK_SCHED) {
		if (!c->ack_pingpong) {
			// the delayed ACK came too late: ACK sooner next time
			uint64_t ato = (uint64_t)c->ack_ato << 1;
			uint64_t rto = tcp_rto_ticks(c);

			c->ack_ato = (uint32_t)(ato < rto ? ato : rto);
		} else {
			c->ack_pingpong = 0;
			c->ack_ato = (uint32_t)tcp_ms(TCP_ATO_MIN_MS);
		}
		tcp_send_ack(c);
		TCPX_INC(TCPX_DELAYEDACKS);
	}
out:
	tcp_lock_release(&c->lock, f);
	tcp_conn_put(c);
}

/* The pacing timer: the next segment's departure time came. */
static void tcp_pacing_timer_fn(net_timer_t *t)
{
	tcp_conn_t *c = CONN_OF(t, pacing_timer);
	uint64_t f;

	tcp_lock_acquire(&c->lock, &f);
	if (c->state == TCP_STATE_ESTABLISHED ||
	    c->state == TCP_STATE_CLOSE_WAIT ||
	    c->state == TCP_STATE_FIN_WAIT_1 ||
	    c->state == TCP_STATE_LAST_ACK || c->state == TCP_STATE_CLOSING)
		tcp_tsq_write(c);
	tcp_lock_release(&c->lock, f);
	tcp_conn_put(c);
}

/* SO_KEEPALIVE (RFC 1122 4.2.3.6): after keepidle without anything from
 * the peer, a probe every keepintvl; keepcnt unanswered probes (or
 * TCP_USER_TIMEOUT) end the connection.  Also the FIN_WAIT2 wait of an
 * orphan whose TCP_LINGER2 exceeds the TIME_WAIT length. */
static void tcp_keepalive_timer_fn(net_timer_t *t)
{
	tcp_conn_t *c = CONN_OF(t, keepalive_timer);
	uint64_t f, now = tcp_now(), elapsed, when;

	tcp_lock_acquire(&c->lock, &f);
	if (c->state == TCP_STATE_CLOSED || c->state == TCP_STATE_LISTEN)
		goto out;
	if (c->state == TCP_STATE_FIN_WAIT_2 && tcp_is_orphan(c)) {
		if (c->linger2_ms >= 0) {
			uint64_t tmo = tcp_ms(c->linger2_ms ?
						      (uint64_t)c->linger2_ms :
						      TCP_FIN_TIMEOUT_MS);

			if (tmo > tcp_ms(TCP_TIMEWAIT_LEN_MS)) {
				tcp_time_wait(c, TCP_STATE_FIN_WAIT_2,
					      tcp_ms(TCP_TIMEWAIT_LEN_MS));
				goto out;
			}
		}
		tcp_send_active_reset(c);
		TCPX_INC(TCPX_FINWAIT2TIMEOUT);
		tcp_done(c);
		goto out;
	}
	if (!c->keepalive || c->state == TCP_STATE_SYN_SENT)
		goto out;
	elapsed = now - c->last_rx_tick;
	if (c->packets_out || tcp_send_head(c)) {
		// alive without keepalive: the retransmissions watch it
		when = c->keepidle_ticks;
		goto resched;
	}
	if (elapsed >= c->keepidle_ticks) {
		if ((c->user_timeout_ms && c->probes_out &&
		     elapsed >= tcp_ms(c->user_timeout_ms)) ||
		    (!c->user_timeout_ms && c->probes_out >= c->keepcnt)) {
			tcp_send_active_reset(c);
			tcp_write_err(c);
			goto out;
		}
		if (tcp_write_wakeup(c) <= 0) {
			if (c->probes_out < 255)
				c->probes_out++;
			TCPX_INC(TCPX_KEEPALIVE);
			when = c->keepintvl_ticks;
		} else {
			when = tcp_ms(TCP_LOCAL_DROP_RETRY_MS);
		}
	} else {
		when = c->keepidle_ticks - elapsed;
	}
resched:
	tcp_reset_keepalive_timer(c, when);
out:
	tcp_lock_release(&c->lock, f);
	tcp_conn_put(c);
}
