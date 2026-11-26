/* Kernel-side probes for the TCP tests of wave 6 (host/net/t_tcp2.c calls
 * them as n0_kt2_*): the SACK scoreboard on a hand-built retransmit
 * queue, the integer cube root, the CUBIC window over time, and PRR's
 * send counts -- driven through the stack's own functions.  Every check
 * function returns 0 on success, otherwise the line of the failed check
 * with a message in kt_msg.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/tcp.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include "kstubs.h"

extern char kt_msg[256];

#define KT(cond, ...)                                                   \
	do {                                                            \
		if (!(cond)) {                                          \
			ksnprintf(kt_msg, sizeof(kt_msg), __VA_ARGS__); \
			return __LINE__;                                \
		}                                                       \
	} while (0)

/* A connection that exists only for the probe: synchronized, SACK on,
 * MSS 1000, sequence space starting at 1000; never hashed. */
static tcp_conn_t *kt_conn(void)
{
	tcp_conn_t *c = tcp_conn_alloc();

	if (!c)
		return NULL;
	c->state = TCP_STATE_ESTABLISHED;
	c->sack_ok = 1;
	c->mss_cache = 1000;
	c->mss_clamp = 1000;
	c->max_window = 1u << 20;
	c->snd_wnd = 1u << 20;
	c->iss = 999;
	c->snd_una = c->snd_nxt = c->write_seq = 1000;
	return c;
}

static void kt_conn_free(tcp_conn_t *c)
{
	tcp_purge_queues(c);
	c->state = TCP_STATE_CLOSED;
	c->proto_ref = 0;
	tcp_conn_put(c);
	tcp_reap_pending();
}

// A sent segment of `len' bytes at snd_nxt, onto the retransmit queue.
static sk_buff_t *kt_sent(tcp_conn_t *c, uint32_t len, uint8_t sacked)
{
	sk_buff_t *skb = skb_alloc_headroom(TCP_MAX_HEADER, len);
	struct tcp_skb_cb *cb;

	if (!skb)
		return NULL;
	mm_memset(skb_append(skb, len), 0x5A, len);
	cb = TCP_SKB_CB(skb);
	mm_memset(cb, 0, sizeof(*cb));
	cb->seq = c->snd_nxt;
	cb->end_seq = c->snd_nxt + len;
	cb->tcp_flags = TCP_ACK;
	cb->sacked = sacked;
	cb->tx_us = tcp_now_us();
	cb->first_tx_us = cb->tx_us;
	tcp_wmem_charge(c, skb);
	__skb_queue_tail(&c->rtx_queue, skb);
	c->snd_nxt = cb->end_seq;
	c->write_seq = c->snd_nxt;
	c->packets_out++;
	if (sacked & TCPCB_SACKED_RETRANS)
		c->retrans_out++;
	return skb;
}

static int kt_tag(tcp_conn_t *c, uint32_t ack, uint32_t prior_una, int n,
		  const uint32_t *blocks)
{
	tcp_parsed_opts_t opt;
	struct tcp_sacktag_state st;
	struct tcp_rate_sample rs;

	mm_memset(&opt, 0, sizeof(opt));
	mm_memset(&st, 0, sizeof(st));
	mm_memset(&rs, 0, sizeof(rs));
	st.rate = &rs;
	opt.sack_count = (uint8_t)n;
	for (int i = 0; i < n; i++) {
		opt.sack[i].start_seq = blocks[2 * i];
		opt.sack[i].end_seq = blocks[2 * i + 1];
	}
	return tcp_sacktag_write_queue(c, &opt, ack, prior_una, &st);
}

// The tags of the queue as a string: 'S' SACKed, '.' not, per segment.
static void kt_tags(tcp_conn_t *c, char *out, int max)
{
	int n = 0;

	for (sk_buff_t *s = skb_peek(&c->rtx_queue); s && n < max - 1; s = s->next)
		out[n++] = (TCP_SKB_CB(s)->sacked & TCPCB_SACKED_ACKED) ? 'S' : '.';
	out[n] = 0;
}

static int kt_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* The scoreboard: whole blocks, the cache, extension, invalid blocks,
 * D-SACK below the ACK and inside the second block, reordering. */
int kt2_sacktag(void)
{
	tcp_conn_t *c = kt_conn();
	char tags[64];
	int flag;
	uint64_t dsack0 = tcp_xmib_read("TCPDSACKRecv");
	uint64_t ofo0 = tcp_xmib_read("TCPDSACKOfoRecv");
	uint64_t discard0 = tcp_xmib_read("TCPSackDiscard");

	KT(c, "no connection");
	for (int i = 0; i < 10; i++)
		KT(kt_sent(c, 1000, 0), "no skb"); // 1000 .. 11000
	{
		static const uint32_t b[] = { 3000, 5000, 7000, 8000 };

		flag = kt_tag(c, 1000, 1000, 2, b);
	}
	kt_tags(c, tags, sizeof(tags));
	KT((flag & FLAG_DATA_SACKED) && c->sacked_out == 3 &&
		   kt_streq(tags, "..SS..S..."),
	   "two blocks: flag %x sacked %u tags %s", flag, c->sacked_out, tags);
	KT(tcp_highest_sack_seq(c) == 8000, "fack %u", tcp_highest_sack_seq(c));
	// the same blocks again: nothing new (the cache)
	{
		static const uint32_t b[] = { 7000, 8000, 3000, 5000 };

		flag = kt_tag(c, 1000, 1000, 2, b);
	}
	KT(!(flag & FLAG_DATA_SACKED) && c->sacked_out == 3,
	   "repeat: flag %x sacked %u", flag, c->sacked_out);
	// one block grows: only its new part is tagged
	{
		static const uint32_t b[] = { 3000, 6000, 7000, 8000 };

		flag = kt_tag(c, 1000, 1000, 2, b);
	}
	kt_tags(c, tags, sizeof(tags));
	KT((flag & FLAG_DATA_SACKED) && c->sacked_out == 4 &&
		   kt_streq(tags, "..SSS.S..."),
	   "grown: flag %x sacked %u tags %s", flag, c->sacked_out, tags);
	/* invalid: beyond snd_nxt, reversed, below snd_una without being a
	 * D-SACK we could use */
	{
		static const uint32_t b[] = { 11000, 12000, 9500, 9000 };

		flag = kt_tag(c, 1000, 1000, 2, b);
	}
	KT(!(flag & FLAG_DATA_SACKED) && c->sacked_out == 4,
	   "invalid blocks tagged: flag %x sacked %u", flag, c->sacked_out);
	KT(tcp_xmib_read("TCPSackDiscard") >= discard0 + 2,
	   "invalid blocks not counted");
	// reordering: a hole below the highest SACK filled by its original
	{
		static const uint32_t b[] = { 2000, 3000 };
		uint32_t seen = c->reord_seen;

		flag = kt_tag(c, 1000, 1000, 1, b);
		KT((flag & FLAG_DATA_SACKED) && c->reord_seen == seen + 1 &&
			   c->reordering == 6,
		   "reordering: seen %u -> %u, degree %u", seen, c->reord_seen,
		   c->reordering);
	}
	/* D-SACK below the cumulative ACK: the ACK covered the first
	 * segment (resent once) and reports it twice; the first block is
	 * the D-SACK, the scoreboard counts it against the undo. */
	{
		static const uint32_t b[] = { 1000, 2000, 2000, 6000 };
		sk_buff_t *head = skb_peek(&c->rtx_queue);

		TCP_SKB_CB(head)->sacked |= TCPCB_SACKED_RETRANS |
					    TCPCB_EVER_RETRANS;
		c->retrans_out = 1;
		c->total_retrans = 1;
		c->undo_marker = 1000;
		c->undo_retrans = 2;
		c->snd_una = 2000; // tcp_ack moved it before tagging
		flag = kt_tag(c, 2000, 1000, 2, b);
		KT((flag & FLAG_DSACKING_ACK) && c->undo_retrans == 1,
		   "D-SACK below the ACK: flag %x undo_retrans %d", flag,
		   c->undo_retrans);
		KT(tcp_xmib_read("TCPDSACKRecv") == dsack0 + 1, "not counted");
		KT(c->rack.dsack_seen, "RACK did not see it");
	}
	/* D-SACK inside the second block (data above the ACK arrived
	 * twice): the first block lies within the second. */
	{
		static const uint32_t b[] = { 7000, 8000, 7000, 9000 };
		sk_buff_t *s = skb_peek(&c->rtx_queue);

		while (s && TCP_SKB_CB(s)->seq != 7000)
			s = s->next;
		KT(s, "no segment at 7000");
		TCP_SKB_CB(s)->sacked |= TCPCB_EVER_RETRANS;
		c->total_retrans = 2;
		flag = kt_tag(c, 2000, 2000, 2, b);
		kt_tags(c, tags, sizeof(tags));
		KT((flag & FLAG_DSACKING_ACK) && (flag & FLAG_DATA_SACKED) &&
			   tcp_xmib_read("TCPDSACKOfoRecv") == ofo0 + 1,
		   "D-SACK in the second block: flag %x", flag);
		KT(kt_streq(tags, ".SSSS.SS.."), "tags %s", tags);
	}
	kt_conn_free(c);
	return 0;
}

/* Partial blocks: a segment of several MSS (sent before the path MTU
 * shrank) covered in part is split at MSS boundaries; a one-MSS segment
 * covered in part is not tagged. */
int kt2_sacktag_split(void)
{
	tcp_conn_t *c = kt_conn();
	char tags[64];
	int flag;

	KT(c, "no connection");
	KT(kt_sent(c, 3000, 0) && kt_sent(c, 1000, 0), "no skb"); // 1000..5000
	KT(c->packets_out == 2, "packets_out %u", c->packets_out);
	{
		static const uint32_t b[] = { 2000, 3500 };

		flag = kt_tag(c, 1000, 1000, 1, b);
	}
	kt_tags(c, tags, sizeof(tags));
	KT((flag & FLAG_DATA_SACKED) && c->sacked_out == 1 &&
		   kt_streq(tags, ".S.."),
	   "split: flag %x sacked %u tags %s", flag, c->sacked_out, tags);
	KT(c->packets_out == 4, "packets_out %u after the split",
	   c->packets_out);
	{
		uint32_t want[] = { 1000, 2000, 2000, 3000, 3000, 4000, 4000, 5000 };
		int i = 0;

		for (sk_buff_t *s = skb_peek(&c->rtx_queue); s; s = s->next, i++) {
			KT(i < 4 && TCP_SKB_CB(s)->seq == want[2 * i] &&
				   TCP_SKB_CB(s)->end_seq == want[2 * i + 1] &&
				   s->len == want[2 * i + 1] - want[2 * i],
			   "piece %d: %u..%u len %u", i, TCP_SKB_CB(s)->seq,
			   TCP_SKB_CB(s)->end_seq, s->len);
		}
	}
	// half of a one-MSS segment: not tagged
	{
		static const uint32_t b[] = { 4000, 4500 };

		flag = kt_tag(c, 1000, 1000, 1, b);
	}
	KT(!(flag & FLAG_DATA_SACKED) && c->sacked_out == 1 && c->packets_out == 4,
	   "half segment: flag %x sacked %u packets %u", flag, c->sacked_out,
	   c->packets_out);
	kt_conn_free(c);
	return 0;
}

// floor(cbrt(a)) for small, large and random values.
int kt2_cubic_root(void)
{
	uint64_t x = 0x9E3779B97F4A7C15ULL;

	KT(tcp_cubic_root(0) == 0 && tcp_cubic_root(1) == 1 &&
		   tcp_cubic_root(7) == 1 && tcp_cubic_root(8) == 2 &&
		   tcp_cubic_root(26) == 2 && tcp_cubic_root(27) == 3,
	   "small values");
	KT(tcp_cubic_root(~0ULL) == 2642245, "2^64-1: %u",
	   tcp_cubic_root(~0ULL));
	for (int i = 0; i < 20000; i++) {
		uint64_t a, r;

		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		a = x >> (i % 60);
		r = tcp_cubic_root(a);
		KT(r * r * r <= a && (r + 1) * (r + 1) * (r + 1) > a,
		   "cbrt(%llu) = %llu", (unsigned long long)a,
		   (unsigned long long)r);
	}
	return 0;
}

// ---- CUBIC over time (the host side moves the clock) ----
static tcp_conn_t *kt_cc;

/* A CUBIC connection whose window `wmax' was just cut by a loss: the
 * new ssthresh and cwnd. */
int kt2_cubic_begin(unsigned wmax, unsigned *cwnd, unsigned *ssthresh)
{
	tcp_conn_t *c = kt_conn();

	KT(c, "no connection");
	kt_cc = c;
	c->ca_ops = &tcp_cubic;
	c->cwnd = wmax;
	tcp_ca_init(c);
	c->ssthresh = c->ca_ops->ssthresh(c);
	c->cwnd = c->ssthresh;
	c->is_cwnd_limited = 1;
	*cwnd = c->cwnd;
	*ssthresh = c->ssthresh;
	return 0;
}

// One window's ACKs, each for one segment; the new cwnd.
unsigned kt2_cubic_round(void)
{
	tcp_conn_t *c = kt_cc;
	uint32_t n = c->cwnd;

	for (uint32_t i = 0; i < n; i++) {
		c->is_cwnd_limited = 1;
		c->ca_ops->cong_avoid(c, 0, 1);
	}
	return c->cwnd;
}

/* Another loss at `cwnd' (below W_max: fast convergence); returns the
 * new W_max, ssthresh in *ssth. */
unsigned kt2_cubic_loss(unsigned cwnd, unsigned *ssth)
{
	tcp_conn_t *c = kt_cc;
	uint32_t wmax, K, css;

	c->cwnd = cwnd;
	c->ssthresh = c->ca_ops->ssthresh(c);
	c->cwnd = c->ssthresh;
	*ssth = c->ssthresh;
	tcp_cubic_get_state(c, &wmax, &K, &css);
	return wmax;
}

void kt2_cubic_state(unsigned *wmax, unsigned *K)
{
	uint32_t w, k, css;

	tcp_cubic_get_state(kt_cc, &w, &k, &css);
	*wmax = w;
	*K = k;
}

void kt2_cubic_end(void)
{
	if (kt_cc)
		kt_conn_free(kt_cc);
	kt_cc = NULL;
}

/* PRR (RFC 6937) on a recovery from a window of 20 with Reno's ssthresh
 * of 10: `nlost' segments lost at the head, every ACK SACKs one more of
 * the others.  sent[i]: what the window let out after ACK i (the
 * retransmissions first).  Returns the final cwnd, -1 on error. */
int kt2_prr(int nlost, int nack, int *sent, unsigned *ssthresh)
{
	tcp_conn_t *c = kt_conn();
	int cwnd;

	if (!c)
		return -1;
	c->ca_ops = &tcp_reno;
	c->cwnd = 20;
	for (int i = 0; i < 20; i++)
		kt_sent(c, 1000, 0);
	c->lost_out = (uint32_t)nlost;
	tcp_init_cwnd_reduction(c);
	tcp_set_ca_state(c, TCP_CA_RECOVERY);
	*ssthresh = c->ssthresh;
	for (int i = 0; i < nack; i++) {
		uint32_t in_flight, n = 0;

		c->sacked_out++;
		tcp_cwnd_reduction(c, 1, 0, 0);
		in_flight = tcp_packets_in_flight(c);
		while (in_flight < c->cwnd) {
			if (c->retrans_out < c->lost_out)
				c->retrans_out++;
			else
				c->packets_out++;
			c->prr_out++;
			in_flight++;
			n++;
		}
		sent[i] = (int)n;
	}
	cwnd = (int)c->cwnd;
	c->packets_out = c->sacked_out = c->lost_out = c->retrans_out = 0;
	kt_conn_free(c);
	return cwnd;
}

/* What the receive queues of a socket's connection hold: o[0] skbs, o[1]
 * of them merged by GRO (more than one segment), o[2] payload bytes,
 * o[3] their memory (rmem_alloc), o[4] the largest truesize of a merged
 * skb, o[5] its payload.  -1 without a connection. */
int kt2_rcvq_mem(int sock, unsigned *o)
{
	net_socket_t *s = sock_get(sock);
	tcp_conn_t *c = s ? s->tcp : NULL;
	uint64_t f;

	mm_memset(o, 0, 6 * sizeof(*o));
	if (!c)
		return -1;
	tcp_lock_acquire(&c->lock, &f);
	for (sk_buff_t *skb = skb_peek(&c->rcv_queue); skb; skb = skb->next) {
		o[0]++;
		if (TCP_SKB_CB(skb)->segs > 1) {
			o[1]++;
			if (skb->truesize > o[4]) {
				o[4] = skb->truesize;
				o[5] = skb->len;
			}
		}
	}
	o[2] = c->rcvq_bytes;
	o[3] = c->rmem_alloc;
	tcp_lock_release(&c->lock, f);
	return 0;
}
