// LikeOS TCP: delivery-rate sampling
//
// Every transmitted segment records how much the connection had delivered
// (cumulatively ACKed or SACKed) by then and when; when the segment is
// delivered, the difference is a sample of the delivery rate: segments
// over the longer of the send phase (between the first and the last
// transmission of the window the sample spans) and the ACK phase (between
// the deliveries).  A sample taken while the sender had nothing to send
// is marked application-limited -- it says the path carries at least that
// much, not that it carries no more.  The best recent sample feeds
// TCP_INFO's delivery rate.
//
// Times are microseconds; a segment keeps the low 32 bits of its stamps
// (intervals stay far below the 71 minutes they wrap at).  Under
// conn->lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>

#define TCPCB_TX_RATE_VALID 0x02 // tx.delivered* hold a sample start

static int64_t tcp_stamp32_delta(uint32_t t1, uint32_t t0)
{
	int32_t d = (int32_t)(t1 - t0);

	return d > 0 ? d : 0;
}

/* A transmission (also a retransmission) of `skb', its tx_us already set:
 * remember the delivery state the sample will start from.  With nothing
 * in flight the interval starts now. */
void tcp_rate_skb_sent(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (!c->packets_out) {
		c->first_tx_us = cb->tx_us;
		c->delivered_us = cb->tx_us;
	}
	cb->tx.first_tx_us32 = (uint32_t)c->first_tx_us;
	cb->tx.delivered_us32 = (uint32_t)c->delivered_us;
	cb->tx.delivered = c->delivered;
	cb->txflags |= TCPCB_TX_RATE_VALID;
	if (c->app_limited)
		cb->txflags |= TCPCB_TX_APP_LIMITED;
	else
		cb->txflags &= (uint8_t)~TCPCB_TX_APP_LIMITED;
}

/* `skb' was ACKed or SACKed: the most recently sent of the delivered
 * segments starts the sample. */
void tcp_rate_skb_delivered(tcp_conn_t *c, sk_buff_t *skb,
			    struct tcp_rate_sample *rs)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (!rs || !(cb->txflags & TCPCB_TX_RATE_VALID))
		return;
	if (!rs->prior_delivered ||
	    cb->tx_us > c->first_tx_us ||
	    (cb->tx_us == c->first_tx_us && after(cb->end_seq, rs->last_end_seq))) {
		rs->prior_delivered = cb->tx.delivered;
		rs->prior_mstamp = cb->tx.delivered_us32 ?
					   cb->tx.delivered_us32 :
					   1; // 0 means "no sample"
		rs->is_app_limited = (cb->txflags & TCPCB_TX_APP_LIMITED) != 0;
		rs->is_retrans = (cb->sacked & TCPCB_RETRANS) != 0;
		rs->last_end_seq = cb->end_seq;
		c->first_tx_us = cb->tx_us;
		rs->interval_us = tcp_stamp32_delta((uint32_t)cb->tx_us,
						    cb->tx.first_tx_us32);
	}
	/* A SACKed segment is cumulatively ACKed later: it must not start
	 * a second sample then. */
	if (cb->sacked & TCPCB_SACKED_ACKED)
		cb->txflags &= (uint8_t)~TCPCB_TX_RATE_VALID;
}

/* The sample of one ACK: `delivered' segments newly delivered, `lost'
 * newly marked lost. */
void tcp_rate_gen(tcp_conn_t *c, uint32_t delivered, uint32_t lost,
		  int is_sack_reneg, struct tcp_rate_sample *rs)
{
	uint64_t now = tcp_now_us();
	int64_t snd_us, ack_us;

	// The application-limited bubble is gone once it was delivered.
	if (c->app_limited && after(c->delivered, c->app_limited))
		c->app_limited = 0;
	if (delivered)
		c->delivered_us = now;
	rs->acked_sacked = delivered;
	rs->losses = (int32_t)lost;
	if (!rs->prior_mstamp || is_sack_reneg) {
		rs->delivered = -1;
		rs->interval_us = -1;
		return;
	}
	rs->delivered = (int32_t)(c->delivered - rs->prior_delivered);
	snd_us = rs->interval_us;
	ack_us = tcp_stamp32_delta((uint32_t)now, (uint32_t)rs->prior_mstamp);
	rs->interval_us = snd_us > ack_us ? snd_us : ack_us;
	rs->snd_interval_us = (uint32_t)snd_us;
	rs->rcv_interval_us = (uint32_t)ack_us;
	/* Shorter than the minimum RTT: an artifact (a spurious
	 * retransmission's delivery), not a rate. */
	if (rs->interval_us < (int64_t)tcp_min_rtt(c) || rs->delivered <= 0) {
		rs->interval_us = -1;
		return;
	}
	// Keep the last unlimited sample, or the best limited one.
	if (!rs->is_app_limited || !c->rate_interval_us ||
	    (uint64_t)rs->delivered * c->rate_interval_us >=
		    (uint64_t)c->rate_delivered * (uint64_t)rs->interval_us) {
		c->rate_delivered = (uint32_t)rs->delivered;
		c->rate_interval_us = (uint64_t)rs->interval_us;
		c->rate_app_limited = rs->is_app_limited;
	}
}

/* The application is about to queue data: if nothing is waiting to be
 * sent and nothing limits us but the application, the samples from here
 * until this flight is delivered are application-limited. */
void tcp_rate_check_app_limited(tcp_conn_t *c)
{
	if (c->write_seq - c->snd_nxt < c->mss_cache &&
	    __atomic_load_n(&c->wmem_tx, __ATOMIC_ACQUIRE) == 0 &&
	    tcp_packets_in_flight(c) < c->cwnd && c->lost_out <= c->retrans_out) {
		uint32_t v = c->delivered + tcp_packets_in_flight(c);

		c->app_limited = v ? v : 1;
	}
}

uint64_t tcp_delivery_rate(const tcp_conn_t *c)
{
	uint64_t bytes = (uint64_t)c->rate_delivered * c->mss_cache;

	if (!c->rate_interval_us)
		return 0;
	if (bytes < (1ULL << 40)) // no overflow in the product
		return bytes * 1000000ULL / c->rate_interval_us;
	return bytes / c->rate_interval_us * 1000000ULL;
}
