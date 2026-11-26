// LikeOS TCP: the congestion-control framework
//
// A connection's congestion window is grown and cut by its congestion
// control (struct tcp_congestion_ops): CUBIC by default, Reno selectable
// with TCP_CONGESTION.  The framework around them is the same for both:
//   * slow start and the additive increase helpers they build on;
//   * window validation (RFC 7661-like): the window only grows while it is
//     what limits the sender, and shrinks toward what was used after an
//     application-limited stretch of an RTO;
//   * restart after idle (RFC 5681 4.1): a connection that sent nothing for
//     an RTO starts again from the initial window, halved per RTO idle;
//   * the window reduction an ACK may call for (CWR and Recovery run PRR,
//     tcp_recovery.c) or else its growth (tcp_cong_control);
//   * the pacing rate: twice cwnd per smoothed RTT in slow start, 1.2 times
//     in congestion avoidance;
//   * the windowed minimum RTT (RACK's reordering window, TCP_INFO).
// All of it runs under conn->lock.  Window arithmetic is in segments,
// rate arithmetic in 64 bits; nothing divides by a value that may be 0.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/mm/memory.h>

// ---------------------------------------------------------------------------
// Reno (RFC 5681).
// ---------------------------------------------------------------------------
static uint32_t tcp_reno_ssthresh(tcp_conn_t *c)
{
	return tcp_max32(c->cwnd >> 1, 2);
}

static void tcp_reno_cong_avoid(tcp_conn_t *c, uint32_t ack, uint32_t acked)
{
	(void)ack;
	if (!tcp_is_cwnd_limited(c))
		return;
	if (tcp_in_slow_start(c)) {
		acked = tcp_slow_start(c, acked);
		if (!acked)
			return;
	}
	tcp_cong_avoid_ai(c, c->cwnd, acked);
}

static uint32_t tcp_reno_undo_cwnd(tcp_conn_t *c)
{
	return tcp_max32(c->cwnd, c->prior_cwnd);
}

const struct tcp_congestion_ops tcp_reno = {
	.name = "reno",
	.ssthresh = tcp_reno_ssthresh,
	.cong_avoid = tcp_reno_cong_avoid,
	.undo_cwnd = tcp_reno_undo_cwnd,
};

// ---------------------------------------------------------------------------
// The registry: TCP_CONGESTION ids are index + 1; the first is the default.
// ---------------------------------------------------------------------------
static const struct tcp_congestion_ops *const tcp_ca_list[] = {
	&tcp_cubic,
	&tcp_reno,
};
#define TCP_CA_COUNT ((int)(sizeof(tcp_ca_list) / sizeof(tcp_ca_list[0])))

const struct tcp_congestion_ops *tcp_ca_default(void)
{
	return tcp_ca_list[0];
}

int tcp_ca_find(const char *name, size_t len)
{
	if (!name)
		return -ENOENT;
	/* The name may come without its terminator, or with one (and
	 * anything after it): compare up to the first NUL. */
	for (size_t i = 0; i < len; i++) {
		if (!name[i]) {
			len = i;
			break;
		}
	}
	if (len == 0 || len >= TCP_CA_NAME_MAX)
		return -ENOENT;
	for (int i = 0; i < TCP_CA_COUNT; i++) {
		const char *n = tcp_ca_list[i]->name;
		size_t k = 0;

		while (k < len && n[k] && n[k] == name[k])
			k++;
		if (k == len && !n[k])
			return i + 1;
	}
	return -ENOENT;
}

const char *tcp_ca_name(int id)
{
	if (id < 1 || id > TCP_CA_COUNT)
		return tcp_ca_default()->name;
	return tcp_ca_list[id - 1]->name;
}

int tcp_ca_id(const tcp_conn_t *c)
{
	for (int i = 0; c && i < TCP_CA_COUNT; i++)
		if (tcp_ca_list[i] == c->ca_ops)
			return i + 1;
	return 1;
}

void tcp_ca_init(tcp_conn_t *c)
{
	mm_memset(c->ca_priv, 0, sizeof(c->ca_priv));
	if (c->ca_ops->init)
		c->ca_ops->init(c);
}

void tcp_ca_release(tcp_conn_t *c)
{
	if (c->ca_ops && c->ca_ops->release)
		c->ca_ops->release(c);
}

/* TCP_CONGESTION (under conn->lock).  A connection that is already
 * synchronized switches at once: the old control lets go, the new one
 * starts from a clean state; one still opening starts with it at the end
 * of the handshake. */
int tcp_set_congestion_control(tcp_conn_t *c, int id)
{
	const struct tcp_congestion_ops *ops;

	if (id < 1 || id > TCP_CA_COUNT)
		return -ENOENT;
	ops = tcp_ca_list[id - 1];
	c->ca_setsockopt = 1;
	if (ops == c->ca_ops)
		return 0;
	if (tcp_state_synchronized(c->state)) {
		tcp_ca_release(c);
		c->ca_ops = ops;
		tcp_ca_init(c);
		if (c->ca_ops->set_state)
			c->ca_ops->set_state(c, c->ca_state);
	} else {
		c->ca_ops = ops;
	}
	return 0;
}

void tcp_set_ca_state(tcp_conn_t *c, uint8_t st)
{
	if (c->ca_ops->set_state)
		c->ca_ops->set_state(c, st);
	c->ca_state = st;
}

void tcp_ca_event(tcp_conn_t *c, int ev)
{
	if (c->ca_ops->cwnd_event)
		c->ca_ops->cwnd_event(c, ev);
}

// ---------------------------------------------------------------------------
// Window helpers.
// ---------------------------------------------------------------------------
void tcp_init_cwnd(tcp_conn_t *c)
{
	/* RFC 5681 3.1: a SYN or SYN-ACK that had to be resent starts the
	 * connection with one segment. */
	c->cwnd = c->total_retrans > 1 ? 1 : TCP_INIT_CWND;
	c->cwnd_cnt = 0;
	c->snd_cwnd_stamp = tcp_now();
}

uint32_t tcp_slow_start(tcp_conn_t *c, uint32_t acked)
{
	uint64_t want = (uint64_t)c->cwnd + acked;
	uint32_t cwnd = want < c->ssthresh ? (uint32_t)want : c->ssthresh;

	if (cwnd < c->cwnd) // ssthresh below cwnd: nothing to grow
		return acked;
	acked -= cwnd - c->cwnd;
	c->cwnd = tcp_min32(cwnd, TCP_CWND_CLAMP);
	return acked;
}

void tcp_cong_avoid_ai(tcp_conn_t *c, uint32_t w, uint32_t acked)
{
	if (w == 0)
		w = 1;
	// Credits gathered at a larger w are applied gently.
	if (c->cwnd_cnt >= w) {
		c->cwnd_cnt = 0;
		c->cwnd++;
	}
	c->cwnd_cnt += acked;
	if (c->cwnd_cnt >= w) {
		uint32_t delta = c->cwnd_cnt / w;

		c->cwnd_cnt -= delta * w;
		c->cwnd += delta;
	}
	c->cwnd = tcp_min32(c->cwnd, TCP_CWND_CLAMP);
}

/* Is the window what limits the sender?  In slow start the window may
 * grow to twice what the flight used. */
int tcp_is_cwnd_limited(const tcp_conn_t *c)
{
	if (c->is_cwnd_limited)
		return 1;
	if (tcp_in_slow_start(c))
		return c->cwnd < 2 * c->max_packets_out;
	return 0;
}

uint32_t tcp_current_ssthresh(const tcp_conn_t *c)
{
	if (tcp_in_cwnd_reduction(c))
		return c->ssthresh;
	return tcp_max32(c->ssthresh, (c->cwnd >> 1) + (c->cwnd >> 2));
}

/* RFC 2861 / RFC 7661: the window was not fully used for an RTO -- it
 * moves halfway toward what was used (never below the initial window),
 * remembering the old one in ssthresh. */
static void tcp_cwnd_application_limited(tcp_conn_t *c)
{
	if (c->ca_state == TCP_CA_OPEN) {
		uint32_t win_used = tcp_max32(c->snd_cwnd_used, TCP_INIT_CWND);

		if (win_used < c->cwnd) {
			c->ssthresh = tcp_current_ssthresh(c);
			c->cwnd = (c->cwnd + win_used) >> 1;
		}
		c->snd_cwnd_used = 0;
	}
	c->snd_cwnd_stamp = tcp_now();
}

/* After a transmission burst: remember how much of the window this
 * window's flight used (tcp_is_cwnd_limited), and shrink a window the
 * application left unused for an RTO. */
void tcp_cwnd_validate(tcp_conn_t *c, int is_cwnd_limited)
{
	if (!before(c->snd_una, c->cwnd_usage_seq) || is_cwnd_limited ||
	    (!c->is_cwnd_limited && c->packets_out > c->max_packets_out)) {
		c->is_cwnd_limited = (uint8_t)(is_cwnd_limited ? 1 : 0);
		c->max_packets_out = c->packets_out;
		c->cwnd_usage_seq = c->snd_nxt;
	}
	if (tcp_is_cwnd_limited(c)) {
		c->snd_cwnd_used = 0;
		c->snd_cwnd_stamp = tcp_now();
	} else {
		if (c->packets_out > c->snd_cwnd_used)
			c->snd_cwnd_used = c->packets_out;
		if (tcp_now() - c->snd_cwnd_stamp >= tcp_rto_ticks(c))
			tcp_cwnd_application_limited(c);
	}
}

/* RFC 5681 4.1: new data after an idle period longer than an RTO starts
 * from the restart window (the initial window), the window halved for
 * every RTO of the idle time down to it. */
void tcp_slow_start_after_idle_check(tcp_conn_t *c)
{
	uint64_t delta, rto;
	uint32_t cwnd, restart;

	if (c->packets_out || !c->lsndtime)
		return;
	delta = tcp_now() - c->lsndtime;
	rto = tcp_rto_ticks(c);
	if (delta <= rto)
		return;
	tcp_ca_event(c, CA_EVENT_CWND_RESTART);
	c->ssthresh = tcp_current_ssthresh(c);
	cwnd = c->cwnd;
	restart = tcp_min32(TCP_INIT_CWND, cwnd);
	while (delta > rto && cwnd > restart) {
		delta -= rto;
		cwnd >>= 1;
	}
	c->cwnd = tcp_max32(cwnd, restart);
	c->snd_cwnd_stamp = tcp_now();
	c->snd_cwnd_used = 0;
}

/* The rate segments are paced at (bytes per second): cwnd per smoothed
 * RTT, doubled in slow start (until half way to ssthresh), 1.2 times in
 * congestion avoidance, capped by SO_MAX_PACING_RATE.  0 until there is
 * an RTT.  Computed for every connection (TSQ sizes its device queue from
 * it); segments are held back by it only where TCP paces itself
 * (tcp_needs_internal_pacing). */
void tcp_update_pacing_rate(tcp_conn_t *c)
{
	uint64_t rate;
	uint32_t ratio;

	if (!c->srtt_us || !c->mss_cache) {
		c->pacing_rate = 0;
		return;
	}
	ratio = c->cwnd < c->ssthresh / 2 ? TCP_PACING_SS_RATIO :
					     TCP_PACING_CA_RATIO;
	rate = (uint64_t)c->mss_cache * tcp_max32(c->cwnd, c->packets_out);
	rate = rate * ratio * 10000u / c->srtt_us;
	if (rate > c->max_pacing_rate)
		rate = c->max_pacing_rate; // SO_MAX_PACING_RATE
	c->pacing_rate = rate ? rate : 1;
}

/* Should the window grow on this ACK?  Only for data delivered in order
 * (RFC 5681), unless the path reorders a lot -- then for any delivery. */
static int tcp_may_raise_cwnd(const tcp_conn_t *c, int flag)
{
	if (c->reordering > TCP_REORDERING)
		return flag & FLAG_FORWARD_PROGRESS;
	return flag & FLAG_DATA_ACKED;
}

void tcp_cong_control(tcp_conn_t *c, uint32_t ack, uint32_t acked_sacked,
		      int flag, const struct tcp_rate_sample *rs)
{
	if (tcp_in_cwnd_reduction(c))
		tcp_cwnd_reduction(c, (int)acked_sacked, rs ? rs->losses : 0,
				   flag);
	else if (tcp_may_raise_cwnd(c, flag) && acked_sacked)
		c->ca_ops->cong_avoid(c, ack, acked_sacked);
	if (c->cwnd == 0)
		c->cwnd = 1;
	tcp_update_pacing_rate(c);
}

// ---------------------------------------------------------------------------
// The windowed minimum RTT (Kathleen Nichols' algorithm: the best three
// samples of a sliding window, so the minimum is forgotten when the path
// changes).
// ---------------------------------------------------------------------------
uint32_t tcp_min_rtt(const tcp_conn_t *c)
{
	return c->min_rtt[0].v;
}

static void tcp_minmax_reset(tcp_conn_t *c, uint32_t t, uint32_t v)
{
	for (int i = 0; i < 3; i++) {
		c->min_rtt[i].t = t;
		c->min_rtt[i].v = v;
	}
}

void tcp_update_rtt_min(tcp_conn_t *c, uint32_t rtt_us)
{
	uint32_t t = (uint32_t)(tcp_now_us() / 1000u), win = TCP_MIN_RTT_WLEN_MS;
	uint32_t dt;

	if (rtt_us == 0)
		rtt_us = 1;
	if (rtt_us <= c->min_rtt[0].v || t - c->min_rtt[2].t > win) {
		tcp_minmax_reset(c, t, rtt_us); // a new minimum / all aged
		return;
	}
	if (rtt_us <= c->min_rtt[1].v) {
		c->min_rtt[1].t = c->min_rtt[2].t = t;
		c->min_rtt[1].v = c->min_rtt[2].v = rtt_us;
	} else if (rtt_us <= c->min_rtt[2].v) {
		c->min_rtt[2].t = t;
		c->min_rtt[2].v = rtt_us;
	}
	dt = t - c->min_rtt[0].t;
	if (dt > win) {
		// the best left the window: the second becomes the best
		c->min_rtt[0] = c->min_rtt[1];
		c->min_rtt[1] = c->min_rtt[2];
		c->min_rtt[2].t = t;
		c->min_rtt[2].v = rtt_us;
		if (t - c->min_rtt[0].t > win) {
			c->min_rtt[0] = c->min_rtt[1];
			c->min_rtt[1] = c->min_rtt[2];
			c->min_rtt[2].t = t;
			c->min_rtt[2].v = rtt_us;
		}
	} else if (c->min_rtt[1].t == c->min_rtt[0].t && dt > win / 4) {
		// a quarter window without a new best: a second choice
		c->min_rtt[1].t = c->min_rtt[2].t = t;
		c->min_rtt[1].v = c->min_rtt[2].v = rtt_us;
	} else if (c->min_rtt[2].t == c->min_rtt[1].t && dt > win / 2) {
		c->min_rtt[2].t = t;
		c->min_rtt[2].v = rtt_us;
	}
}
