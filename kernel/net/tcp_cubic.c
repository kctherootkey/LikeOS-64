// LikeOS TCP: CUBIC congestion control (RFC 9438) with HyStart++ (RFC 9406)
//
// The window follows W(t) = C * (t - K)^3 + W_max after a reduction, t the
// time since the reduction began, W_max the window before it and K the
// time W(t) needs to get back there: K = cbrt(W_max * (1 - beta) / C).
// C = 0.4, beta = 0.7 (the reduction leaves 70 % of the window).  Fast
// convergence: a W_max below the previous one is lowered further, so a
// flow releases bandwidth to newcomers.  In the TCP-friendly region the
// window grows at least as fast as Reno with the same beta would
// (RFC 9438 4.3).  The growth is computed in integers: time in units of
// 1/1024 s from the millisecond clock, the cube in 64 bits with the scale
// folded into constants, the cube root by Newton's method.
//
// Slow start ends at the first loss or ECN echo, or earlier by HyStart++:
// the minimum RTT of each round (window of data) is compared with the
// previous round's; a rise of an eighth of it (4 to 16 ms) means a queue
// is building, and slow start moves into Conservative Slow Start (growth
// at a quarter of the rate) for up to five rounds -- back to slow start if
// the RTT drops below where CSS began, on to congestion avoidance after
// the five rounds.  HyStart++ runs in the first slow start only (while
// ssthresh is still unset), as RFC 9406 recommends; segments are paced, so
// the per-ACK growth is not limited (L = infinity).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>

#define CUBIC_BETA 717 // beta = 717 / 1024 ~ 0.7
#define CUBIC_BETA_SCALE 1024
#define CUBIC_C_SCALED 410 // C * 1024 = 0.4 * 1024 (rounded): c/rtt scale
#define CUBIC_HZ_SHIFT 10 // time unit 1/1024 s
// 2^40 / 410: (W_max - cwnd) * this = K^3 in (1/1024 s)^3
#define CUBIC_CUBE_FACTOR ((1ULL << (10 + 3 * CUBIC_HZ_SHIFT)) / CUBIC_C_SCALED)
/* Reno-friendly increase with the same beta: 3 (1 - beta) / (1 + beta)
 * segments per window, as segments per ACKed segment * 8:
 * 8 * (1024 + 717) / 3 / (1024 - 717). */
#define CUBIC_BETA_FRIENDLY ((8 * (CUBIC_BETA_SCALE + CUBIC_BETA) / 3) / \
			     (CUBIC_BETA_SCALE - CUBIC_BETA))
#define CUBIC_MAX_OFFS (1ULL << 18) // (t - K) beyond 256 s: no overflow

// HyStart++ (RFC 9406 4.3)
#define HSPP_MIN_RTT_THRESH_US 4000
#define HSPP_MAX_RTT_THRESH_US 16000
#define HSPP_MIN_RTT_DIVISOR 8
#define HSPP_N_RTT_SAMPLE 8
#define HSPP_CSS_GROWTH_DIVISOR 4
#define HSPP_CSS_ROUNDS 5
#define HSPP_LOW_WINDOW 16 // below this many segments the RTT is too noisy

struct cubic {
	uint32_t cnt; // ACKed segments per +1 of cwnd
	uint32_t last_max_cwnd; // W_max
	uint32_t last_cwnd; // cwnd at the last update
	uint32_t last_time; // ms of the last update
	uint32_t origin_point; // W(K)
	uint32_t K; // 1/1024 s
	uint32_t delay_min; // us
	uint32_t epoch_start; // ms, 0: no epoch
	uint32_t ack_cnt; // ACKed segments (TCP-friendly estimate)
	uint32_t tcp_cwnd; // the Reno-friendly window
	// HyStart++
	uint8_t hs_done; // slow start ended (HyStart++ off from now on)
	uint8_t in_css;
	uint8_t css_rounds;
	uint8_t _pad;
	uint32_t window_end; // the current round ends when this is ACKed
	uint32_t last_round_min_rtt; // us, ~0: none
	uint32_t cur_round_min_rtt;
	uint32_t rtt_sample_cnt;
	uint32_t css_baseline_min_rtt;
	uint32_t css_cnt; // CSS: ACKed segments toward +1
};
BUILD_BUG_ON(sizeof(struct cubic) > sizeof(((tcp_conn_t *)0)->ca_priv));

static inline struct cubic *cubic_of(tcp_conn_t *c)
{
	return (struct cubic *)c->ca_priv;
}

static uint32_t cubic_now_ms(void)
{
	uint32_t t = (uint32_t)(tcp_now_us() / 1000u);

	return t ? t : 1; // 0 means "no epoch"
}

/* floor(cbrt(a)): Newton's method from a power of two above the root;
 * the iterates fall monotonically to the floor of the root. */
uint32_t tcp_cubic_root(uint64_t a)
{
	uint64_t x, y;
	int bits;

	if (a == 0)
		return 0;
	bits = 64 - __builtin_clzll(a);
	x = 1ULL << ((bits + 2) / 3);
	for (;;) {
		y = (2 * x + a / (x * x)) / 3;
		if (y >= x)
			break;
		x = y;
	}
	return (uint32_t)x;
}

static void cubic_reset(struct cubic *ca)
{
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->last_cwnd = 0;
	ca->last_time = 0;
	ca->origin_point = 0;
	ca->K = 0;
	ca->delay_min = 0;
	ca->epoch_start = 0;
	ca->ack_cnt = 0;
	ca->tcp_cwnd = 0;
}

static void hystart_reset(tcp_conn_t *c)
{
	struct cubic *ca = cubic_of(c);

	ca->window_end = c->snd_nxt;
	ca->last_round_min_rtt = ~0u;
	ca->cur_round_min_rtt = ~0u;
	ca->rtt_sample_cnt = 0;
	ca->in_css = 0;
	ca->css_rounds = 0;
	ca->css_baseline_min_rtt = ~0u;
	ca->css_cnt = 0;
}

static void cubic_init(tcp_conn_t *c)
{
	struct cubic *ca = cubic_of(c);

	cubic_reset(ca);
	hystart_reset(c);
	ca->hs_done = c->ssthresh != TCP_INFINITE_SSTHRESH;
}

/* The window CUBIC aims for now; sets ca->cnt, the ACKed segments per
 * +1 of cwnd that get there in about an RTT. */
static void cubic_update(tcp_conn_t *c, uint32_t cwnd, uint32_t acked)
{
	struct cubic *ca = cubic_of(c);
	uint32_t now = cubic_now_ms();
	uint64_t t, offs, delta, target;

	ca->ack_cnt += acked;
	if (ca->last_cwnd == cwnd && (int32_t)(now - ca->last_time) <= 1000 / 32)
		return;
	// at most one recalculation per millisecond
	if (ca->epoch_start && now == ca->last_time)
		goto friendly;
	ca->last_cwnd = cwnd;
	ca->last_time = now;
	if (ca->epoch_start == 0) {
		ca->epoch_start = now;
		ca->ack_cnt = acked;
		ca->tcp_cwnd = cwnd;
		if (ca->last_max_cwnd <= cwnd) {
			ca->K = 0;
			ca->origin_point = cwnd;
		} else {
			// K = cbrt((W_max - cwnd) / C), in 1/1024 s
			ca->K = tcp_cubic_root(CUBIC_CUBE_FACTOR *
					       (uint64_t)(ca->last_max_cwnd - cwnd));
			ca->origin_point = ca->last_max_cwnd;
		}
	}
	/* t: time since the epoch began plus the minimum delay (the window
	 * the ACKs report is an RTT old), in 1/1024 s. */
	t = (uint32_t)(now - ca->epoch_start) + ca->delay_min / 1000u;
	t = (t << CUBIC_HZ_SHIFT) / 1000u;
	offs = t < ca->K ? ca->K - t : t - ca->K;
	if (offs > CUBIC_MAX_OFFS)
		offs = CUBIC_MAX_OFFS;
	// C * (t - K)^3, scaled back to segments
	delta = (CUBIC_C_SCALED * offs * offs * offs) >> (10 + 3 * CUBIC_HZ_SHIFT);
	if (t < ca->K)
		target = ca->origin_point > delta ? ca->origin_point - delta : 1;
	else
		target = (uint64_t)ca->origin_point + delta;
	if (target > cwnd)
		ca->cnt = (uint32_t)(cwnd / (target - cwnd));
	else
		ca->cnt = 100 * cwnd; // a tiny increase near the plateau
	/* No W_max yet (the first epoch after slow start ended without a
	 * loss, HyStart++): grow by at least 5 % per RTT. */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;
friendly:
	/* The TCP-friendly region: never slower than Reno with the same
	 * beta would be. */
	{
		uint32_t d = (cwnd * CUBIC_BETA_FRIENDLY) >> 3;

		if (d == 0)
			d = 1;
		if (ca->ack_cnt > d) {
			uint32_t n = (ca->ack_cnt - 1) / d;

			ca->ack_cnt -= n * d;
			ca->tcp_cwnd += n;
		}
		if (ca->tcp_cwnd > cwnd) {
			uint32_t max_cnt = cwnd / (ca->tcp_cwnd - cwnd);

			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}
	// At most +1 per 2 ACKed segments: 1.5 times per RTT.
	if (ca->cnt < 2)
		ca->cnt = 2;
}

static void cubic_cong_avoid(tcp_conn_t *c, uint32_t ack, uint32_t acked)
{
	struct cubic *ca = cubic_of(c);

	(void)ack;
	if (!tcp_is_cwnd_limited(c))
		return;
	if (tcp_in_slow_start(c)) {
		if (ca->in_css) {
			// Conservative Slow Start: a quarter of the growth
			ca->css_cnt += acked;
			c->cwnd = tcp_min32(c->cwnd + ca->css_cnt /
						      HSPP_CSS_GROWTH_DIVISOR,
					    TCP_CWND_CLAMP);
			ca->css_cnt %= HSPP_CSS_GROWTH_DIVISOR;
			if (c->cwnd > c->ssthresh)
				c->cwnd = c->ssthresh;
			return;
		}
		acked = tcp_slow_start(c, acked);
		if (!acked)
			return;
	}
	cubic_update(c, c->cwnd, acked);
	tcp_cong_avoid_ai(c, ca->cnt, acked);
}

static uint32_t cubic_ssthresh(tcp_conn_t *c)
{
	struct cubic *ca = cubic_of(c);

	ca->epoch_start = 0; // the end of the epoch
	ca->hs_done = 1;
	ca->in_css = 0;
	// W_max, with fast convergence
	if (c->cwnd < ca->last_max_cwnd)
		ca->last_max_cwnd = (uint32_t)(((uint64_t)c->cwnd *
						(CUBIC_BETA_SCALE + CUBIC_BETA)) /
					       (2 * CUBIC_BETA_SCALE));
	else
		ca->last_max_cwnd = c->cwnd;
	return tcp_max32((uint32_t)(((uint64_t)c->cwnd * CUBIC_BETA) /
				    CUBIC_BETA_SCALE),
			 2);
}

static void cubic_set_state(tcp_conn_t *c, uint8_t st)
{
	if (st == TCP_CA_LOSS) {
		struct cubic *ca = cubic_of(c);

		cubic_reset(ca);
		hystart_reset(c);
		ca->hs_done = 1;
	}
}

static void cubic_cwnd_event(tcp_conn_t *c, int ev)
{
	if (ev == CA_EVENT_TX_START) {
		struct cubic *ca = cubic_of(c);
		uint32_t now = cubic_now_ms();
		uint32_t idle = c->lsndtime ?
					(uint32_t)tcp_ticks_to_ms(tcp_now() -
								  c->lsndtime) :
					0;

		/* Idle for a while (application-limited): the epoch moves
		 * by the idle time, so the curve resumes where it was. */
		if (ca->epoch_start && idle > 0) {
			ca->epoch_start += idle;
			if ((int32_t)(ca->epoch_start - now) > 0)
				ca->epoch_start = now;
		}
	}
}

/* HyStart++: one RTT sample in the first slow start. */
static void hystart_update(tcp_conn_t *c, uint32_t delay)
{
	struct cubic *ca = cubic_of(c);

	if (after(c->snd_una, ca->window_end)) {
		// a round ended
		if (ca->in_css) {
			if (ca->css_rounds >= HSPP_CSS_ROUNDS) {
				// CSS is over: congestion avoidance
				c->ssthresh = c->cwnd;
				ca->in_css = 0;
				ca->hs_done = 1;
				return;
			}
			ca->css_rounds++;
		}
		ca->last_round_min_rtt = ca->cur_round_min_rtt;
		ca->cur_round_min_rtt = ~0u;
		ca->rtt_sample_cnt = 0;
		ca->window_end = c->snd_nxt;
	}
	if (delay < ca->cur_round_min_rtt)
		ca->cur_round_min_rtt = delay;
	ca->rtt_sample_cnt++;
	if (ca->rtt_sample_cnt < HSPP_N_RTT_SAMPLE ||
	    ca->cur_round_min_rtt == ~0u)
		return;
	if (!ca->in_css) {
		uint32_t thresh;

		if (ca->last_round_min_rtt == ~0u)
			return;
		thresh = ca->last_round_min_rtt / HSPP_MIN_RTT_DIVISOR;
		if (thresh < HSPP_MIN_RTT_THRESH_US)
			thresh = HSPP_MIN_RTT_THRESH_US;
		if (thresh > HSPP_MAX_RTT_THRESH_US)
			thresh = HSPP_MAX_RTT_THRESH_US;
		if ((uint64_t)ca->cur_round_min_rtt >=
		    (uint64_t)ca->last_round_min_rtt + thresh) {
			// the delay rises: a queue builds -- CSS
			ca->css_baseline_min_rtt = ca->cur_round_min_rtt;
			ca->in_css = 1;
			ca->css_rounds = 1; // this partial round counts
			ca->css_cnt = 0;
			TCPX_INC(TCPX_HYSTARTDELAYDETECT);
			TCPX_ADD(TCPX_HYSTARTDELAYCWND, c->cwnd);
		}
	} else if (ca->cur_round_min_rtt < ca->css_baseline_min_rtt) {
		// the rise was spurious: back to slow start
		ca->css_baseline_min_rtt = ~0u;
		ca->in_css = 0;
		TCPX_INC(TCPX_HYSTARTCSSRESUME);
	}
}

static void cubic_pkts_acked(tcp_conn_t *c, uint32_t acked, int32_t rtt_us)
{
	struct cubic *ca = cubic_of(c);
	uint32_t delay;

	(void)acked;
	if (rtt_us < 0)
		return;
	// No delay samples right after a reduction (the queue drains).
	if (ca->epoch_start &&
	    (int32_t)(cubic_now_ms() - ca->epoch_start) < 1000)
		return;
	delay = rtt_us ? (uint32_t)rtt_us : 1;
	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;
	if (!ca->hs_done && c->ssthresh == TCP_INFINITE_SSTHRESH &&
	    tcp_in_slow_start(c) && c->cwnd >= HSPP_LOW_WINDOW)
		hystart_update(c, delay);
}

static uint32_t cubic_undo_cwnd(tcp_conn_t *c)
{
	return tcp_max32(c->cwnd, c->prior_cwnd);
}

/* Diagnostics: W_max, K (1/1024 s) and the CSS state of a connection
 * that runs CUBIC. */
void tcp_cubic_get_state(tcp_conn_t *c, uint32_t *wmax, uint32_t *K,
			 uint32_t *in_css)
{
	struct cubic *ca = cubic_of(c);

	*wmax = ca->last_max_cwnd;
	*K = ca->K;
	*in_css = ca->in_css;
}

const struct tcp_congestion_ops tcp_cubic = {
	.name = "cubic",
	.init = cubic_init,
	.ssthresh = cubic_ssthresh,
	.cong_avoid = cubic_cong_avoid,
	.set_state = cubic_set_state,
	.cwnd_event = cubic_cwnd_event,
	.pkts_acked = cubic_pkts_acked,
	.undo_cwnd = cubic_undo_cwnd,
};
