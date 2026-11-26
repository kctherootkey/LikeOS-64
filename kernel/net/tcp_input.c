// LikeOS TCP: input
//
// Every segment for a full socket comes here with the socket's lock held
// (tcp_rcv_state_process).  An ESTABLISHED connection first tries the
// header-prediction fast path: the expected next segment with nothing
// unusual about it is either a pure ACK of new data or in-order data, and
// goes straight to the ACK processing or the receive queue.  Everything
// else takes the full path of RFC 9293 3.10.7: PAWS and the sequence
// check, RST and SYN checks (RFC 5961), the ACK (window update, the
// retransmit queue, RTT, congestion window, loss recovery), urgent data,
// then the text: in order onto the receive queue, out of order into the
// out-of-order tree, with the SACK blocks to report, and the FIN.
//
// ACKs are sent at once (quick-ACK mode at the start and after anything
// unusual, every second full-sized segment, out-of-order data, a filled
// hole) or delayed by the estimated ACK timeout.  The receive buffer is
// sized by what the application reads per round trip.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

// ===========================================================================
// RTT and RTO (RFC 6298), microseconds.
// ===========================================================================
void tcp_set_rto(tcp_conn_t *c)
{
	/* RTO = SRTT + max(4 * RTTVAR, RTO_MIN): the floor applies to the
	 * variance term, not to the sum -- on a steady path the variance
	 * decays to nothing, and a floor on the sum would put the RTO a hair
	 * above the RTT itself. */
	uint64_t var = 4ULL * c->rttvar_us, rto;

	if (var < TCP_RTO_MIN_US)
		var = TCP_RTO_MIN_US;
	rto = (uint64_t)c->srtt_us + var;
	if (rto > TCP_RTO_MAX_US)
		rto = TCP_RTO_MAX_US;
	c->rto_us = (uint32_t)rto;
}

void tcp_rtt_estimator(tcp_conn_t *c, uint32_t r_us)
{
	if (r_us == 0)
		r_us = 1;
	if (r_us > TCP_RTO_MAX_US)
		r_us = TCP_RTO_MAX_US;
	if (c->srtt_us == 0) {
		c->srtt_us = r_us;
		c->rttvar_us = r_us / 2;
	} else {
		uint32_t diff = c->srtt_us > r_us ? c->srtt_us - r_us :
						     r_us - c->srtt_us;

		c->rttvar_us = (c->rttvar_us * 3 + diff) / 4;
		c->srtt_us = (c->srtt_us * 7 + r_us) / 8;
		if (!c->srtt_us)
			c->srtt_us = 1;
	}
	tcp_set_rto(c);
	c->rto_backoff = 0; // RFC 6298 5.7: a valid sample ends the backoff
}

/* A sample from a timestamp echo alone (the handshake): the TSecr is the
 * TSval of the very segment the peer answered. */
void tcp_rtt_sample_ts(tcp_conn_t *c, const tcp_parsed_opts_t *opt)
{
	uint32_t delta;

	if (!opt->ts_present || opt->tsecr == 0)
		return;
	delta = tcp_ts_now(c->ts_offset) - opt->tsecr;
	if ((int32_t)delta < 0 || delta > TCP_RTO_MAX_US / 1000u)
		return;
	tcp_rtt_estimator(c, delta ? delta * 1000u : 1000u);
	tcp_update_rtt_min(c, delta ? delta * 1000u : 1000u);
}

// ===========================================================================
// ACK processing.
// ===========================================================================
/* RFC 9293 3.10.7.4 / the SND.WL1, SND.WL2 rule: a segment updates the
 * window when it acknowledges new data, is newer, or is the same segment
 * offering more -- or closing the window altogether: a receiver that runs
 * out of buffer may shut the window with a pure window update (same
 * sequence, same ACK), and the sender must stop sending into it. */
static int tcp_may_update_window(const tcp_conn_t *c, uint32_t ack,
				 uint32_t seq, uint32_t nwin)
{
	return after(ack, c->snd_una) || after(seq, c->snd_wl1) ||
	       (seq == c->snd_wl1 && (nwin > c->snd_wnd || !nwin));
}

/* The window of the segment (the SND.WL1 rule), and snd_una.  Returns
 * FLAG_WIN_UPDATE when the window was taken from it. */
static int tcp_ack_update_window(tcp_conn_t *c, const sk_buff_t *skb,
				 uint32_t ack, int flag)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t nwin = cb->window;
	int rflag = 0;

	if (!(flag & FLAG_SYNACK) && c->ws_enabled)
		nwin <<= c->snd_wscale; // a SYN-ACK's window is unscaled
	if (tcp_may_update_window(c, ack, cb->seq, nwin)) {
		rflag |= FLAG_WIN_UPDATE;
		c->snd_wl1 = cb->seq;
		if (c->snd_wnd != nwin) {
			c->snd_wnd = nwin;
			if (nwin > c->max_window) {
				c->max_window = nwin;
				tcp_sync_mss(c, c->pmtu_cookie);
			}
		}
	}
	if (after(ack, c->snd_una)) {
		c->bytes_acked += ack - c->snd_una;
		c->snd_una = ack;
	}
	return rflag;
}

/* The RTT of this ACK: from the send time of what it newly acknowledged
 * (Karn: never from a retransmitted segment), else of what it SACKed,
 * else from its timestamp echo when it acknowledges new data and the echo
 * is plausible.  Returns 1 when the estimator took a sample. */
static int tcp_ack_update_rtt(tcp_conn_t *c, int flag, int64_t seq_rtt_us,
			      int64_t sack_rtt_us, int64_t ca_rtt_us,
			      uint64_t first_tx, const tcp_parsed_opts_t *opt,
			      struct tcp_rate_sample *rs)
{
	if (seq_rtt_us < 0)
		seq_rtt_us = sack_rtt_us;
	if (seq_rtt_us < 0 && c->ts_enabled && opt && opt->ts_present &&
	    opt->tsecr && (flag & FLAG_ACKED)) {
		/* The echo is believed only when it is plausible: not from
		 * the future and no older than the first transmission of
		 * what this ACK acknowledges. */
		uint32_t delta = tcp_ts_now(c->ts_offset) - opt->tsecr;
		uint64_t now_us = tcp_now_us();

		if ((int32_t)delta >= 0 && first_tx && now_us >= first_tx &&
		    (uint64_t)delta * 1000u <=
			    (now_us - first_tx) + timer_us_per_tick() + 1000u)
			seq_rtt_us = ca_rtt_us = delta ? (int64_t)delta * 1000 : 1000;
	}
	rs->rtt_us = ca_rtt_us;
	if (seq_rtt_us < 0)
		return 0;
	if (seq_rtt_us > TCP_RTO_MAX_US)
		seq_rtt_us = TCP_RTO_MAX_US;
	if (ca_rtt_us >= 0 &&
	    !((flag & FLAG_ACK_MAYBE_DELAYED) &&
	      (uint64_t)ca_rtt_us > tcp_min_rtt(c)))
		tcp_update_rtt_min(c, (uint32_t)tcp_min32(
					      (uint32_t)(ca_rtt_us > TCP_RTO_MAX_US ?
								  TCP_RTO_MAX_US :
								  ca_rtt_us),
					      TCP_RTO_MAX_US));
	tcp_rtt_estimator(c, (uint32_t)seq_rtt_us);
	return 1;
}

/* Was this retransmitted segment's retransmission unnecessary -- does the
 * ACK echo a timestamp from before it? */
static int tcp_skb_spurious_retrans(const tcp_conn_t *c, const sk_buff_t *skb)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint64_t now = tcp_now_us();
	uint32_t ts_tx;

	if (!(cb->sacked & TCPCB_RETRANS) || !c->ts_enabled || !c->rcv_tsecr)
		return 0;
	ts_tx = tcp_ts_now(c->ts_offset) -
		(uint32_t)(now > cb->tx_us ? (now - cb->tx_us) / 1000u : 0);
	return before(c->rcv_tsecr, ts_tx);
}

/* Remove what the cumulative ACK covers from the retransmit queue:
 * counters, RACK, the delivery rate, the RTT.  Returns FLAG_* bits. */
static int tcp_clean_rtx_queue(tcp_conn_t *c, const tcp_parsed_opts_t *opt,
			       uint32_t prior_fack, uint32_t prior_snd_una,
			       struct tcp_sacktag_state *st, int ece_ack)
{
	uint64_t now = tcp_now_us(), first_ackt = 0, last_ackt = 0, first_tx = 0;
	uint32_t prior_sacked = c->sacked_out, reord = c->snd_nxt;
	uint32_t pkts_acked = 0;
	int64_t seq_rtt_us = -1, sack_rtt_us = -1, ca_rtt_us = -1;
	int flag = 0, fully_acked = 1, rtt_update;
	sk_buff_t *skb;

	while ((skb = skb_peek(&c->rtx_queue)) != NULL) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		uint8_t sacked = cb->sacked;

		if (after(cb->end_seq, c->snd_una)) {
			/* Partly acknowledged: drop the acknowledged bytes
			 * from its head; it stays one segment. */
			if (after(c->snd_una, cb->seq) && skb->len &&
			    !(cb->tcp_flags & TCP_SYN))
				tcp_trim_head(c, skb, c->snd_una - cb->seq);
			fully_acked = 0;
			break;
		}
		if (!first_tx)
			first_tx = cb->first_tx_us;
		if (sacked & TCPCB_RETRANS) {
			if ((sacked & TCPCB_SACKED_RETRANS) && c->retrans_out)
				c->retrans_out--;
			flag |= FLAG_RETRANS_DATA_ACKED;
		} else if (!(sacked & TCPCB_SACKED_ACKED)) {
			last_ackt = cb->tx_us;
			if (!first_ackt)
				first_ackt = last_ackt;
			if (before(cb->seq, reord))
				reord = cb->seq;
			if (!after(cb->end_seq, c->high_seq))
				flag |= FLAG_ORIG_SACK_ACKED;
		}
		if (sacked & TCPCB_SACKED_ACKED) {
			if (c->sacked_out)
				c->sacked_out--;
		} else if (tcp_is_sack(c)) {
			tcp_count_delivered(c, 1, ece_ack);
			if (!tcp_skb_spurious_retrans(c, skb))
				tcp_rack_advance(c, sacked, cb->end_seq, cb->tx_us);
		}
		if ((sacked & TCPCB_LOST) && c->lost_out)
			c->lost_out--;
		if (c->packets_out)
			c->packets_out--;
		pkts_acked++;
		tcp_rate_skb_delivered(c, skb, st->rate);
		if (!(cb->tcp_flags & TCP_SYN)) {
			flag |= FLAG_DATA_ACKED;
		} else {
			flag |= FLAG_SYN_ACKED;
			c->retrans_stamp = 0;
			c->retrans_ts = 0;
		}
		if (skb == c->retransmit_skb_hint)
			c->retransmit_skb_hint = NULL;
		tcp_tsorted_del(c, skb);
		__skb_unlink(skb, &c->rtx_queue);
		tcp_wmem_uncharge(c, skb);
		skb_put(skb);
	}
	if (c->snd_urg_pending && !before(c->snd_una, c->snd_up))
		c->snd_urg_pending = 0;
	/* The ACK stopped at a segment the peer had SACKed: it took the
	 * SACK back (reneging). */
	if (skb && (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED))
		flag |= FLAG_SACK_RENEGING;
	if (first_ackt && !(flag & FLAG_RETRANS_DATA_ACKED)) {
		seq_rtt_us = now > first_ackt ? (int64_t)(now - first_ackt) : 0;
		ca_rtt_us = now > last_ackt ? (int64_t)(now - last_ackt) : 0;
		/* One small segment ACKed alone, nothing unusual: the peer
		 * probably delayed that ACK -- no minimum-RTT sample. */
		if (pkts_acked == 1 && fully_acked && !prior_sacked &&
		    c->snd_una - prior_snd_una < c->mss_cache &&
		    st->rate->prior_delivered + 1 == c->delivered &&
		    !(flag & (FLAG_CA_ALERT | FLAG_SYN_ACKED)))
			flag |= FLAG_ACK_MAYBE_DELAYED;
	}
	if (st->first_sackt) {
		sack_rtt_us = now > st->first_sackt ? (int64_t)(now - st->first_sackt) : 0;
		ca_rtt_us = now > st->last_sackt ? (int64_t)(now - st->last_sackt) : 0;
	}
	rtt_update = tcp_ack_update_rtt(c, flag, seq_rtt_us, sack_rtt_us,
					ca_rtt_us, first_tx, opt, st->rate);
	if (flag & FLAG_ACKED) {
		flag |= FLAG_SET_XMIT_TIMER; // the TLP or RTO timer
		if (tcp_is_reno(c)) {
			tcp_remove_reno_sacks(c, pkts_acked, ece_ack);
			/* Without SACK a cumulatively ACKed retransmission
			 * cannot tell whether the original arrived. */
			if (flag & FLAG_RETRANS_DATA_ACKED)
				flag &= ~FLAG_ORIG_SACK_ACKED;
		} else if (before(reord, prior_fack)) {
			// a hole of never-resent data filled: reordering
			tcp_check_sack_reordering(c, reord, 0);
		}
	} else if (skb && rtt_update && sack_rtt_us >= 0 &&
		   (uint64_t)sack_rtt_us > now - TCP_SKB_CB(skb)->tx_us) {
		/* The SACK RTT is older than the head's transmission: the
		 * head was resent since (a TLP) -- re-arm from now. */
		flag |= FLAG_SET_XMIT_TIMER;
	}
	if (c->ca_ops->pkts_acked && pkts_acked) {
		int64_t r = st->rate->rtt_us;

		c->ca_ops->pkts_acked(c, pkts_acked,
				      r < 0 ? -1 :
				      r > TCP_RTO_MAX_US ? (int32_t)TCP_RTO_MAX_US :
							   (int32_t)r);
	}
	return flag;
}

/* A zero-window probe was answered with an open window: the probes stop
 * and the data goes.  An answer that leaves the window closed resets only
 * probes_out (in tcp_ack): the backoff and the episode's start
 * (probes_tstamp), which bound an orphan's and a TCP_USER_TIMEOUT
 * socket's patience, run on. */
static void tcp_ack_probe(tcp_conn_t *c)
{
	sk_buff_t *head = tcp_send_head(c);

	if (!head)
		return;
	if (!after(TCP_SKB_CB(head)->seq + tcp_min32(head->len, tcp_current_mss(c)),
		   c->snd_una + c->snd_wnd)) {
		c->rto_backoff = 0;
		c->probes_out = 0;
		c->probes_tstamp = 0;
		if (c->pending_timer == TCP_TIME_PROBE0)
			tcp_clear_xmit_timer(c);
	} else {
		tcp_check_probe_timer(c);
	}
}

/* Only a few ACKs need the loss machinery: a duplicate, one with news
 * from SACK, a D-SACK or an ECN echo, and any while not Open. */
static int tcp_ack_is_dubious(const tcp_conn_t *c, int flag)
{
	return !(flag & FLAG_NOT_DUP) || (flag & FLAG_CA_ALERT) ||
	       c->ca_state != TCP_CA_OPEN;
}

static uint32_t tcp_newly_delivered(tcp_conn_t *c, uint32_t prior_delivered,
				    int flag)
{
	uint32_t delivered = c->delivered - prior_delivered;

	(void)flag;
	return delivered;
}

/* The ACK-side ECN echo: ECE on a segment of an ECN connection. */
static int tcp_ecn_rcv_ecn_echo(const tcp_conn_t *c, const sk_buff_t *skb)
{
	return (c->ecn_flags & TCP_ECN_OK) &&
	       (TCP_SKB_CB(skb)->tcp_flags & (TCP_ECE | TCP_SYN)) == TCP_ECE;
}

static void tcp_replace_ts_recent(tcp_conn_t *c, uint32_t seq,
				  const tcp_parsed_opts_t *opt);

/* Process the ACK field of an accepted segment.  Returns > 0 when the ACK
 * is acceptable, 0 for an old ACK (its field is ignored, its data still
 * welcome), < 0 when the segment must be dropped.  With
 * FLAG_UPDATE_TS_RECENT the segment's TSval becomes ts_recent -- only once
 * the ACK proved acceptable (RFC 7323 5.3 / RFC 5961: a segment that is
 * dropped for its ACK must not move what PAWS judges the next ones by). */
static int tcp_ack(tcp_conn_t *c, sk_buff_t *skb, const tcp_parsed_opts_t *opt,
		   int flag)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	struct tcp_sacktag_state st;
	struct tcp_rate_sample rs;
	uint32_t ack = cb->ack_seq, prior_snd_una = c->snd_una;
	uint32_t max_win = tcp_max32(c->max_window, 65535u);
	uint32_t prior_packets = c->packets_out, prior_fack;
	uint32_t delivered = c->delivered, lost = c->lost;
	int num_dupack = 0, rexmit = REXMIT_NONE;
	int sack = c->sack_ok && opt->sack_count;

	mm_memset(&st, 0, sizeof(st));
	mm_memset(&rs, 0, sizeof(rs));
	rs.rtt_us = -1;
	rs.interval_us = -1;
	st.rate = &rs;

	if (before(ack, prior_snd_una)) {
		/* RFC 5961 5.2: an ACK older than anything the window could
		 * explain is an attack; a merely old one is a reordered
		 * segment whose data is still good. */
		if (before(ack, prior_snd_una - max_win)) {
			if (!(flag & FLAG_NO_CHALLENGE_ACK))
				tcp_send_challenge_ack(c);
			return -1;
		}
		goto old_ack;
	}
	if (after(ack, c->snd_nxt)) {
		/* Acknowledges data never sent (RFC 9293 3.10.7.4): an ACK
		 * (rate limited); a caller that answers itself says so. */
		if (!(flag & FLAG_NO_CHALLENGE_ACK))
			tcp_send_challenge_ack(c);
		return -1;
	}
	if (after(ack, prior_snd_una)) {
		flag |= FLAG_SND_UNA_ADVANCED;
		c->retransmits = 0;
	}
	if (flag & FLAG_UPDATE_TS_RECENT)
		tcp_replace_ts_recent(c, cb->seq, opt);
	prior_fack = tcp_is_sack(c) ? tcp_highest_sack_seq(c) : c->snd_una;
	rs.prior_in_flight = tcp_packets_in_flight(c);
	if (cb->seq != cb->end_seq)
		flag |= FLAG_DATA;
	flag |= tcp_ack_update_window(c, skb, ack, flag);
	if (sack)
		flag |= tcp_sacktag_write_queue(c, opt, ack, prior_snd_una, &st);
	if (tcp_ecn_rcv_ecn_echo(c, skb))
		flag |= FLAG_ECE;
	if (st.sack_delivered)
		tcp_count_delivered(c, st.sack_delivered, flag & FLAG_ECE);
	c->probes_out = 0;
	c->last_rx_tick = tcp_now();
	if (!prior_packets)
		goto no_queue;

	flag |= tcp_clean_rtx_queue(c, opt, prior_fack, prior_snd_una, &st,
				    flag & FLAG_ECE);
	tcp_rack_update_reo_wnd(c, &rs);
	if (c->tlp_high_seq)
		tcp_process_tlp_ack(c, ack, flag);
	if (tcp_ack_is_dubious(c, flag)) {
		if (!(flag & (FLAG_SND_UNA_ADVANCED | FLAG_NOT_DUP |
			      FLAG_DSACKING_ACK))) {
			num_dupack = 1;
			// pure ACKs merged by GRO: each was one duplicate
			if (!(flag & FLAG_DATA) && cb->segs > 1)
				num_dupack = cb->segs;
		}
		tcp_fastretrans_alert(c, prior_snd_una, num_dupack, &flag, &rexmit);
	}
	if (flag & FLAG_SET_XMIT_TIMER)
		tcp_set_xmit_timer(c);
	if ((flag & FLAG_FORWARD_PROGRESS) || !(flag & FLAG_NOT_DUP))
		ip_confirm_daddr(c->remote_ip); // the next hop is reachable
	delivered = tcp_newly_delivered(c, delivered, flag);
	lost = c->lost - lost;
	tcp_rate_gen(c, delivered, lost, c->is_sack_reneg, &rs);
	tcp_cong_control(c, ack, delivered, flag, &rs);
	tcp_xmit_recovery(c, rexmit);
	if (!c->packets_out && tcp_send_head(c))
		tcp_check_probe_timer(c);
	if (!tcp_any_retrans_done(c))
		c->retrans_stamp = 0; // the give-up clocks stop
	if (flag & FLAG_ACKED)
		tcp_sndbuf_expand(c);
	if (flag & FLAG_SND_UNA_ADVANCED)
		tcp_update_tx_ready(c);
	return 1;

no_queue:
	if (flag & FLAG_DSACKING_ACK)
		tcp_fastretrans_alert(c, prior_snd_una, num_dupack, &flag, &rexmit);
	tcp_ack_probe(c);
	if (c->tlp_high_seq)
		tcp_process_tlp_ack(c, ack, flag);
	if (flag & FLAG_SND_UNA_ADVANCED)
		tcp_update_tx_ready(c);
	return 1;

old_ack:
	/* An old ACK may still carry SACK news (a reordered ACK): tag, and
	 * let recovery use it. */
	if (sack) {
		flag |= tcp_sacktag_write_queue(c, opt, ack, prior_snd_una, &st);
		if (st.sack_delivered)
			tcp_count_delivered(c, st.sack_delivered, 0);
		tcp_fastretrans_alert(c, prior_snd_una, num_dupack, &flag, &rexmit);
		tcp_xmit_recovery(c, rexmit);
	}
	return 0;
}

// ===========================================================================
// Receiving data.
// ===========================================================================
static void tcp_incr_quickack(tcp_conn_t *c, uint32_t max_quickacks)
{
	uint32_t q = c->rcv_mss ? c->rcv_wnd / (2 * c->rcv_mss) : 2;

	if (q == 0)
		q = 2;
	if (q > max_quickacks)
		q = max_quickacks;
	if (q > c->ack_quick)
		c->ack_quick = (uint8_t)q;
}

void tcp_enter_quickack_mode(tcp_conn_t *c, uint32_t max_quickacks)
{
	tcp_incr_quickack(c, max_quickacks);
	c->ack_pingpong = 0;
	c->ack_ato = (uint32_t)tcp_ms(TCP_ATO_MIN_MS);
}

static int tcp_in_quickack_mode(const tcp_conn_t *c)
{
	return c->ack_quick && !c->ack_pingpong;
}

static void tcp_schedule_ack(tcp_conn_t *c)
{
	c->ack_pending |= TCP_ACK_SCHED;
}

static void tcp_initialize_rcv_mss(tcp_conn_t *c)
{
	uint32_t hint = tcp_min32(c->advmss, c->mss_cache);

	hint = tcp_min32(hint, c->rcv_wnd / 2);
	hint = tcp_min32(hint, TCP_MSS_DEFAULT);
	hint = tcp_max32(hint, TCP_MIN_MSS);
	c->rcv_mss = hint;
}

static void tcp_set_window_clamp(tcp_conn_t *c, uint32_t val)
{
	uint32_t old = c->window_clamp;

	if (val < TCP_MIN_MSS * 2u)
		val = TCP_MIN_MSS * 2u;
	if (val == old)
		return;
	c->window_clamp = val;
	if (val < old) {
		c->rcv_ssthresh = tcp_min32(c->rcv_ssthresh, val);
	} else {
		uint32_t n = tcp_min32(c->rcv_wnd, val);

		c->rcv_ssthresh = tcp_max32(n, c->rcv_ssthresh);
	}
}

/* Estimate the peer's segment size from what arrives (the ACK-every-
 * second-segment rule needs it), and how much of a received buffer is
 * payload (the window offered for a given buffer depends on it). */
static void tcp_measure_rcv_mss(tcp_conn_t *c, const sk_buff_t *skb)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t lss = c->last_seg_size;
	uint32_t len = skb->len;

	if (cb->segs > 1)
		len = skb->len / cb->segs; // merged by GRO: one segment's
	c->last_seg_size = 0;
	if (len >= c->rcv_mss) {
		if (len != c->rcv_mss && skb->truesize) {
			uint64_t val = ((uint64_t)skb->len << 8) / skb->truesize;
			uint8_t old = c->scaling_ratio;

			if (val > 255)
				val = 255;
			c->scaling_ratio = val ? (uint8_t)val : 1;
			if (old != c->scaling_ratio) {
				tcp_set_window_clamp(c, tcp_win_from_space(
								c, c->rcvbuf));
				if (c->window_clamp < c->rcvq_space.space)
					c->rcvq_space.space = c->window_clamp;
			}
		}
		c->rcv_mss = tcp_min32(len, c->advmss);
	} else {
		len += c->tcp_header_len;
		if (len >= TCP_MSS_DEFAULT + sizeof(tcp_header_t) ||
		    (len >= TCP_MIN_MSS + sizeof(tcp_header_t) &&
		     !(cb->tcp_flags & (TCP_PSH | TCP_URG | TCP_FIN | TCP_SYN |
					TCP_RST)))) {
			len -= c->tcp_header_len;
			c->last_seg_size = len;
			if (len == lss) {
				c->rcv_mss = len;
				return;
			}
		}
		if (c->ack_pending & TCP_ACK_PUSHED)
			c->ack_pending |= TCP_ACK_PUSHED2;
		c->ack_pending |= TCP_ACK_PUSHED;
	}
}

// Receiver-side RTT, for the receive-buffer tuning.
static void tcp_rcv_rtt_update(tcp_conn_t *c, uint32_t sample, int win_dep)
{
	uint32_t new_sample = c->rcv_rtt_est.rtt_us;
	int64_t m = sample;

	if (new_sample != 0) {
		if (!win_dep) {
			m -= (new_sample >> 3);
			new_sample = (uint32_t)((int64_t)new_sample + m);
		} else {
			m <<= 3;
			if (m < (int64_t)new_sample)
				new_sample = (uint32_t)m;
		}
	} else {
		new_sample = (uint32_t)(m << 3);
	}
	c->rcv_rtt_est.rtt_us = new_sample ? new_sample : 8;
}

static void tcp_rcv_rtt_measure(tcp_conn_t *c)
{
	uint64_t now = tcp_now_us();

	if (c->rcv_rtt_est.time_us == 0)
		goto new_measure;
	if (before(c->rcv_nxt, c->rcv_rtt_est.seq))
		return;
	if (now > c->rcv_rtt_est.time_us)
		tcp_rcv_rtt_update(c, (uint32_t)tcp_min32(
					      (uint32_t)(now - c->rcv_rtt_est.time_us),
					      TCP_RTO_MAX_US),
				   1);
new_measure:
	c->rcv_rtt_est.seq = c->rcv_nxt + c->rcv_wnd;
	c->rcv_rtt_est.time_us = now;
}

static void tcp_rcv_rtt_measure_ts(tcp_conn_t *c, const sk_buff_t *skb)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (!c->ts_enabled || !c->rcv_tsecr ||
	    c->rcv_tsecr == c->rcv_rtt_last_tsecr)
		return;
	c->rcv_rtt_last_tsecr = c->rcv_tsecr;
	if (cb->end_seq - cb->seq >= c->rcv_mss) {
		int32_t delta = (int32_t)(tcp_ts_now(c->ts_offset) - c->rcv_tsecr);

		if (delta > 0 && (uint32_t)delta < TCP_RTO_MAX_US / 1000u)
			tcp_rcv_rtt_update(c, (uint32_t)delta * 1000u, 0);
	}
}

/* DRS: once per receiver RTT, size the receive buffer for twice what the
 * application read in that RTT (plus room for the sender to grow), so the
 * window never limits a reader that keeps up.  Only a number changes --
 * no buffer is allocated. */
void tcp_rcv_space_adjust(tcp_conn_t *c)
{
	uint64_t now = tcp_now_us(), time;
	uint32_t copied, inq;

	if (!c->rcvq_space.time_us)
		goto new_measure;
	time = now > c->rcvq_space.time_us ? now - c->rcvq_space.time_us : 0;
	if (c->rcv_rtt_est.rtt_us == 0 || time < (c->rcv_rtt_est.rtt_us >> 3))
		return;
	copied = c->copied_seq - c->rcvq_space.seq;
	inq = c->rcv_nxt - c->copied_seq;
	copied = copied > inq ? copied - inq : 0;
	if (copied <= c->rcvq_space.space)
		goto new_measure;
	if (!(c->userlocks & TCP_RCVBUF_LOCK) && !tcp_under_memory_pressure()) {
		uint64_t rcvwin = ((uint64_t)copied << 1) + 16ULL * c->advmss;
		uint64_t grow = c->rcvq_space.space ?
					rcvwin * (copied - c->rcvq_space.space) /
						c->rcvq_space.space :
					0;
		uint64_t rcvbuf;

		rcvwin += grow << 1;
		if (rcvwin > 0x7FFFFFFFull)
			rcvwin = 0x7FFFFFFFull;
		rcvbuf = (uint64_t)tcp_space_from_win(c, (uint32_t)rcvwin);
		if (rcvbuf > TCP_RMEM_MAX)
			rcvbuf = TCP_RMEM_MAX;
		if (rcvbuf > (uint64_t)c->rcvbuf) {
			c->rcvbuf = (int)rcvbuf;
			c->window_clamp = tcp_win_from_space(c, c->rcvbuf);
			TCPX_INC(TCPX_RCVBUFGROW);
		}
	}
	c->rcvq_space.space = copied;
new_measure:
	c->rcvq_space.seq = c->copied_seq;
	c->rcvq_space.time_us = now;
}

/* rcv_ssthresh grows with the data that arrives while memory allows:
 * the window opens up to window_clamp as the connection proves it needs
 * it. */
static void tcp_grow_window(tcp_conn_t *c, const sk_buff_t *skb)
{
	int32_t room = (int32_t)tcp_min32(c->window_clamp,
					   (uint32_t)tcp_max32(0, tcp_space(c))) -
		       (int32_t)c->rcv_ssthresh;
	int32_t incr;

	if (room <= 0 || tcp_under_memory_pressure())
		return;
	if (tcp_win_from_space(c, (int)skb->truesize) <= skb->len) {
		incr = 2 * (int32_t)c->advmss;
	} else {
		int32_t truesize = (int32_t)tcp_win_from_space(c, (int)skb->truesize) >> 1;
		int32_t window = (int32_t)tcp_win_from_space(c, TCP_RMEM_MAX) >> 1;

		incr = 0;
		while ((int32_t)c->rcv_ssthresh <= window) {
			if (truesize <= (int32_t)skb->len) {
				incr = 2 * (int32_t)c->rcv_mss;
				break;
			}
			truesize >>= 1;
			window >>= 1;
		}
	}
	if (incr) {
		incr = incr > 2 * (int32_t)skb->len ? incr : 2 * (int32_t)skb->len;
		c->rcv_ssthresh += (uint32_t)(incr < room ? incr : room);
		c->ack_quick |= 1;
	}
}

/* ECN, the data receiver (RFC 3168 6.1.3): a CE mark is echoed with ECE
 * on every ACK until the sender's CWR; the first one is ACKed at once (the
 * sender may have a tiny window).  A segment without ECT after ECT ones
 * is probably a retransmission: ACK it quickly too. */
static void tcp_ecn_check_ce(tcp_conn_t *c, const sk_buff_t *skb)
{
	if (!(c->ecn_flags & TCP_ECN_OK))
		return;
	switch (TCP_SKB_CB(skb)->ip_dsfield & INET_ECN_MASK) {
	case INET_ECN_NOT_ECT:
		if (c->ecn_flags & TCP_ECN_SEEN)
			tcp_enter_quickack_mode(c, 2);
		break;
	case INET_ECN_CE:
		c->received_ce++;
		if (!(c->ecn_flags & TCP_ECN_DEMAND_CWR)) {
			tcp_enter_quickack_mode(c, 2);
			c->ecn_flags |= TCP_ECN_DEMAND_CWR;
		}
		c->ecn_flags |= TCP_ECN_SEEN;
		break;
	default:
		c->ecn_flags |= TCP_ECN_SEEN;
		break;
	}
}

/* The sender reduced its window (CWR): stop echoing, and ACK at once. */
static void tcp_ecn_accept_cwr(tcp_conn_t *c, const sk_buff_t *skb)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (cb->tcp_flags & TCP_CWR) {
		c->ecn_flags &= (uint8_t)~TCP_ECN_DEMAND_CWR;
		if (cb->seq != cb->end_seq)
			c->ack_pending |= TCP_ACK_NOW;
	}
}

static void tcp_event_data_recv(tcp_conn_t *c, const sk_buff_t *skb)
{
	uint64_t now = tcp_now();

	tcp_schedule_ack(c);
	tcp_measure_rcv_mss(c, skb);
	tcp_rcv_rtt_measure(c);
	if (!c->ack_ato) {
		// the first data: start the delayed-ACK machinery quick
		tcp_incr_quickack(c, TCP_MAX_QUICKACKS);
		c->ack_ato = (uint32_t)tcp_ms(TCP_ATO_MIN_MS);
	} else {
		uint64_t m = now - c->ack_lrcvtime;
		uint64_t atomin = tcp_ms(TCP_ATO_MIN_MS);
		uint64_t rto = tcp_rto_ticks(c);

		if (m <= atomin / 2) {
			c->ack_ato = (uint32_t)((c->ack_ato >> 1) + atomin / 2);
		} else if (m < c->ack_ato) {
			c->ack_ato = (uint32_t)((c->ack_ato >> 1) + m);
			if (c->ack_ato > rto)
				c->ack_ato = (uint32_t)rto;
		} else if (m > rto) {
			/* A long gap: the sender restarts its window and
			 * wants quick ACKs. */
			tcp_incr_quickack(c, TCP_MAX_QUICKACKS);
		}
	}
	c->ack_lrcvtime = now;
	tcp_ecn_check_ce(c, skb);
	if (skb->len >= 128)
		tcp_grow_window(c, skb);
}

// ---- SACK blocks we send (RFC 2018, RFC 2883) ----
static int tcp_sack_extend(struct tcp_sack_block *sp, uint32_t seq,
			   uint32_t end_seq)
{
	if (!after(seq, sp->end_seq) && !after(sp->start_seq, end_seq)) {
		if (before(seq, sp->start_seq))
			sp->start_seq = seq;
		if (after(end_seq, sp->end_seq))
			sp->end_seq = end_seq;
		return 1;
	}
	return 0;
}

void tcp_dsack_set(tcp_conn_t *c, uint32_t seq, uint32_t end_seq)
{
	if (!c->sack_ok)
		return;
	if (!before(seq, c->rcv_nxt))
		TCPX_INC(TCPX_DSACKOFOSENT);
	c->dsack = 1;
	c->duplicate_sack[0].start_seq = seq;
	c->duplicate_sack[0].end_seq = end_seq;
}

static void tcp_dsack_extend(tcp_conn_t *c, uint32_t seq, uint32_t end_seq)
{
	if (!c->dsack)
		tcp_dsack_set(c, seq, end_seq);
	else
		tcp_sack_extend(&c->duplicate_sack[0], seq, end_seq);
}

static void tcp_sack_maybe_coalesce(tcp_conn_t *c)
{
	struct tcp_sack_block *sp = &c->selective_acks[0];

	for (uint32_t i = 1; i < c->num_sacks;) {
		if (tcp_sack_extend(sp, c->selective_acks[i].start_seq,
				    c->selective_acks[i].end_seq)) {
			c->num_sacks--;
			for (uint32_t j = i; j < c->num_sacks; j++)
				c->selective_acks[j] = c->selective_acks[j + 1];
			continue;
		}
		i++;
	}
}

/* An out-of-order range arrived: the block it joins moves to the front
 * (the most recently changed block is reported first), or it becomes a
 * new front block (the oldest falls off when all are taken). */
static void tcp_sack_new_ofo_skb(tcp_conn_t *c, uint32_t seq, uint32_t end_seq)
{
	struct tcp_sack_block *sp = c->selective_acks;
	uint32_t n = c->num_sacks, i;

	for (i = 0; i < n; i++) {
		if (tcp_sack_extend(&sp[i], seq, end_seq)) {
			for (; i > 0; i--) {
				struct tcp_sack_block t = sp[i];

				sp[i] = sp[i - 1];
				sp[i - 1] = t;
			}
			if (c->num_sacks > 1)
				tcp_sack_maybe_coalesce(c);
			return;
		}
	}
	if (n >= TCP_MAX_SACK_BLOCKS)
		n = TCP_MAX_SACK_BLOCKS - 1;
	for (i = n; i > 0; i--)
		sp[i] = sp[i - 1];
	sp[0].start_seq = seq;
	sp[0].end_seq = end_seq;
	c->num_sacks = (uint8_t)(n + 1);
}

// rcv_nxt moved: blocks it now covers are gone.
static void tcp_sack_remove(tcp_conn_t *c)
{
	uint32_t n = c->num_sacks;

	if (rb_empty_root(&c->ooo_queue)) {
		c->num_sacks = 0;
		return;
	}
	for (uint32_t i = 0; i < n;) {
		if (!before(c->rcv_nxt, c->selective_acks[i].start_seq)) {
			for (uint32_t j = i + 1; j < n; j++)
				c->selective_acks[j - 1] = c->selective_acks[j];
			n--;
			continue;
		}
		i++;
	}
	c->num_sacks = (uint8_t)n;
}

// ---- the queues ----
static inline sk_buff_t *rb_to_skb(struct rb_node *n)
{
	return n ? rb_entry(n, sk_buff_t, rbnode) : NULL;
}

static void tcp_drop_skb(sk_buff_t *skb)
{
	skb_put(skb);
}

/* Append `from' to `to' when it continues it and fits into its tailroom
 * (small segments of an interactive flow end up in one buffer) -- only a
 * buffer TCP allocated itself (a collapsed one), never the buffer a
 * frame arrived in.  The caller frees `from' on success. */
static int tcp_try_coalesce(tcp_conn_t *c, sk_buff_t *to, sk_buff_t *from)
{
	struct tcp_skb_cb *tcb = TCP_SKB_CB(to), *fcb = TCP_SKB_CB(from);

	(void)c;
	if (tcb->end_seq != fcb->seq || (tcb->tcp_flags & TCP_FIN))
		return 0;
	if (!(tcb->rxflags & TCPCB_RX_OWN_DATA) || skb_cloned(to) ||
	    from->len > skb_tailroom(to))
		return 0;
	if (from->len)
		mm_memcpy(skb_append(to, from->len), from->data, from->len);
	tcb->end_seq = fcb->end_seq;
	tcb->tcp_flags |= fcb->tcp_flags;
	tcb->ack_seq = fcb->ack_seq;
	return 1;
}

/* In-order data onto the receive queue.  `skb' is not charged yet; it
 * starts at rcv_nxt. */
static void tcp_queue_rcv(tcp_conn_t *c, sk_buff_t *skb)
{
	sk_buff_t *tail = skb_peek_tail(&c->rcv_queue);
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t len = skb->len;

	c->bytes_received += cb->end_seq - c->rcv_nxt;
	c->rcv_nxt = cb->end_seq;
	if (tail && tcp_try_coalesce(c, tail, skb)) {
		skb_put(skb);
	} else if (len) {
		tcp_rmem_charge(c, skb);
		__skb_queue_tail(&c->rcv_queue, skb);
	} else {
		skb_put(skb); // a FIN alone
	}
	if (len) {
		c->rcvq_bytes += len;
		tcp_wake_reader(c);
	}
}

static int tcp_prune_queue(tcp_conn_t *c, int in_order);

/* Room for `skb' in the receive buffer, pruning to make some.  in_order:
 * the data is at rcv_nxt (it would be delivered at once). */
static int tcp_try_rmem_schedule(tcp_conn_t *c, const sk_buff_t *skb,
				 int in_order)
{
	(void)skb;
	if (c->rmem_alloc > (uint32_t)c->rcvbuf || tcp_mem_over_limit())
		return tcp_prune_queue(c, in_order) < 0 ? -1 : 0;
	return 0;
}

void tcp_ofo_purge(tcp_conn_t *c)
{
	struct rb_node *n = rb_first(&c->ooo_queue);

	while (n) {
		sk_buff_t *skb = rb_to_skb(n);

		n = rb_next(n);
		rb_erase(&skb->rbnode, &c->ooo_queue);
		skb->next = skb->prev = NULL;
		tcp_rmem_uncharge(c, skb);
		skb_put(skb);
	}
	c->ooo_last_skb = NULL;
}

/* Under memory pressure the out-of-order data goes first, from the
 * highest sequence down (what is furthest from being deliverable).  The
 * SACK blocks then no longer describe what we hold: they are reset.
 * Returns 1 when something was freed. */
void tcp_prune_ofo_queue(tcp_conn_t *c)
{
	struct rb_node *n = rb_last(&c->ooo_queue);
	uint32_t goal = (uint32_t)c->rcvbuf >> 3;
	int freed = 0;

	while (n) {
		sk_buff_t *skb = rb_to_skb(n);
		struct rb_node *prev = rb_prev(n);

		rb_erase(&skb->rbnode, &c->ooo_queue);
		skb->next = skb->prev = NULL;
		goal = goal > skb->truesize ? goal - skb->truesize : 0;
		tcp_rmem_uncharge(c, skb);
		skb_put(skb);
		freed = 1;
		if (!goal && c->rmem_alloc <= (uint32_t)c->rcvbuf &&
		    !tcp_under_memory_pressure() && !tcp_mem_over_limit())
			break;
		n = prev;
	}
	if (freed) {
		TCPX_INC(TCPX_OFOPRUNED);
		c->num_sacks = 0;
		c->pred_flags = 0;
		c->ooo_last_skb = rb_to_skb(rb_last(&c->ooo_queue));
	}
}

/* The global reclaim (tcp.c: TCP memory over its hard limit) takes all of
 * a connection's out-of-order data: it is the only memory TCP can take
 * back without losing anything the peer will not resend -- the SACK
 * blocks that announced it are withdrawn (reneging, RFC 2018 8).  Under
 * conn->lock.  Returns the bytes (truesize) freed. */
uint32_t tcp_ofo_reclaim(tcp_conn_t *c)
{
	uint32_t before = c->rmem_alloc;

	if (rb_empty_root(&c->ooo_queue))
		return 0;
	tcp_ofo_purge(c);
	c->num_sacks = 0;
	c->pred_flags = 0;
	TCPX_INC(TCPX_OFOPRUNED);
	return before - c->rmem_alloc;
}

/* Compact the receive queue: from its first "bloated" skb on (a buffer
 * much larger than the data it holds -- tiny segments each in a frame
 * buffer), the data is copied into fresh buffers sized for it, which
 * replace those skbs.  The skbs before that point stay as they are, and so
 * do buffers a collapse made (TCPCB_RX_OWN_DATA: as compact as the
 * allocator makes them) -- so repeated prunes of a queue that is already
 * compact copy nothing again.  The data ends with the tail skb's last byte
 * (a FIN occupies no byte; a FIN that came alone is not on the queue).
 * For the receive queue only (the reader's position is copied_seq). */
static void tcp_collapse_rcv(tcp_conn_t *c)
{
	sk_buff_t *tail = skb_peek_tail(&c->rcv_queue), *first = NULL, *skb;
	sk_buff_t *out_head = NULL, *out_tail = NULL;
	uint32_t seq, end, total = 0;
	uint8_t fin;

	if (!tail)
		return;
	for (skb = skb_peek(&c->rcv_queue); skb; skb = skb->next) {
		const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

		if (!(cb->rxflags & TCPCB_RX_OWN_DATA) &&
		    !(cb->tcp_flags & TCP_FIN) &&
		    tcp_win_from_space(c, (int)skb->truesize) > skb->len) {
			first = skb;
			break;
		}
	}
	if (!first)
		return;
	end = TCP_SKB_CB(tail)->seq + tail->len;
	seq = first == skb_peek(&c->rcv_queue) ? c->copied_seq :
						 TCP_SKB_CB(first)->seq;
	fin = TCP_SKB_CB(tail)->tcp_flags & TCP_FIN;
	if (!before(seq, end))
		return;
	skb = first;
	while (before(seq, end)) {
		uint32_t want = end - seq;
		uint32_t cap = SKB_MAX_ALLOC;
		sk_buff_t *n;
		struct tcp_skb_cb *ncb;

		if (want > cap)
			want = cap;
		/* A buffer sized for the data (a fitted class, from its
		 * reserve: interrupts are off here); without one, the
		 * pools: a small slot, or a jumbo one for a large run. */
		n = skb_alloc_fit(0, want);
		if (!n) {
			if (want < 16384 && want > SKB_SMALL_DATA - SKB_HEADROOM)
				want = SKB_SMALL_DATA - SKB_HEADROOM;
			n = skb_alloc_headroom(0, want);
		}
		if (!n)
			break;
		ncb = TCP_SKB_CB(n);
		mm_memset(ncb, 0, sizeof(*ncb));
		ncb->rxflags = TCPCB_RX_OWN_DATA;
		ncb->seq = seq;
		while (want && skb) {
			struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
			uint32_t off = seq - cb->seq;
			uint32_t avail = skb->len > off ? skb->len - off : 0;
			uint32_t k = avail < want ? avail : want;

			if (k) {
				mm_memcpy(skb_append(n, k), skb->data + off, k);
				seq += k;
				want -= k;
			}
			if (seq - cb->seq >= skb->len)
				skb = skb->next;
		}
		ncb->end_seq = seq;
		n->next = NULL;
		if (out_tail)
			out_tail->next = n;
		else
			out_head = n;
		out_tail = n;
		total++;
		if (!skb)
			break;
	}
	if (!out_head)
		return;
	if (before(seq, end)) {
		/* Ran out of buffers half way: keep the originals. */
		while (out_head) {
			sk_buff_t *n = out_head;

			out_head = n->next;
			n->next = NULL;
			skb_put(n);
		}
		return;
	}
	// The originals from `first' to the tail go; the copies take over.
	while ((skb = skb_peek_tail(&c->rcv_queue)) != NULL) {
		int last = skb == first;

		__skb_unlink(skb, &c->rcv_queue);
		tcp_rmem_uncharge(c, skb);
		skb_put(skb);
		if (last)
			break;
	}
	while (out_head) {
		sk_buff_t *n = out_head;

		out_head = n->next;
		n->next = NULL;
		if (!out_head && fin) {
			TCP_SKB_CB(n)->tcp_flags |= TCP_FIN;
			TCP_SKB_CB(n)->end_seq++;
		}
		tcp_rmem_charge(c, n);
		__skb_queue_tail(&c->rcv_queue, n);
	}
	TCPX_ADD(TCPX_RCVCOLLAPSED, total);
}

/* Does the receive buffer have room again?  Above the global hard limit
 * a connection still gets its minimum (TCP_RMEM_MIN, the first of
 * tcp_rmem) for in-order data: whatever everybody else holds, every
 * connection can make progress. */
static int tcp_rmem_ok(const tcp_conn_t *c, int in_order)
{
	if (c->rmem_alloc > (uint32_t)c->rcvbuf)
		return 0;
	if (!tcp_mem_over_limit())
		return 1;
	return in_order && c->rmem_alloc < TCP_RMEM_MIN;
}

/* The receive buffer is over its size, or TCP over its global limit: grow
 * the buffer if allowed, compact the receive queue, and drop out-of-order
 * data (this connection's own first; the global reclaim takes everybody
 * else's) before in-order data is refused.  Returns -1 when the incoming
 * data must be dropped. */
static int tcp_prune_queue(tcp_conn_t *c, int in_order)
{
	TCPX_INC(TCPX_PRUNECALLED);
	if (c->rmem_alloc >= (uint32_t)c->rcvbuf) {
		// tcp_clamp_window
		if (c->rcvbuf < TCP_RMEM_MAX && !(c->userlocks & TCP_RCVBUF_LOCK) &&
		    !tcp_under_memory_pressure() &&
		    tcp_mem_allocated() < TCP_MEM_LOW)
			c->rcvbuf = (int)tcp_min32(c->rmem_alloc, TCP_RMEM_MAX);
		if (c->rmem_alloc > (uint32_t)c->rcvbuf)
			c->rcv_ssthresh = tcp_min32(c->window_clamp,
						    2u * c->advmss);
	} else if (tcp_under_memory_pressure()) {
		c->rcv_ssthresh = tcp_min32(c->rcv_ssthresh, 4u * c->advmss);
	}
	if (tcp_rmem_ok(c, in_order))
		return 0;
	tcp_collapse_rcv(c);
	if (tcp_rmem_ok(c, in_order))
		return 0;
	tcp_prune_ofo_queue(c);
	if (tcp_rmem_ok(c, in_order))
		return 0;
	if (tcp_mem_over_limit())
		tcp_mem_reclaim_kick(); // the others' out-of-order data
	TCPX_INC(TCPX_RCVPRUNED);
	c->pred_flags = 0;
	return -1;
}

/* Merge `from' into `to' when it continues it (out-of-order tree). */
static int tcp_ooo_try_coalesce(tcp_conn_t *c, sk_buff_t *to, sk_buff_t *from)
{
	if (!to)
		return 0;
	return tcp_try_coalesce(c, to, from);
}

static void tcp_fin(tcp_conn_t *c);

/* Move what became in order from the out-of-order tree to the receive
 * queue. */
static void tcp_ofo_queue(tcp_conn_t *c)
{
	uint32_t dsack_high = c->rcv_nxt;
	struct rb_node *p = rb_first(&c->ooo_queue);

	while (p) {
		sk_buff_t *skb = rb_to_skb(p);
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		int fin;

		if (after(cb->seq, c->rcv_nxt))
			break;
		if (before(cb->seq, dsack_high)) {
			uint32_t dsack = dsack_high;

			if (before(cb->end_seq, dsack_high))
				dsack_high = cb->end_seq;
			tcp_dsack_extend(c, cb->seq, dsack);
		}
		p = rb_next(p);
		rb_erase(&skb->rbnode, &c->ooo_queue);
		skb->next = skb->prev = NULL;
		tcp_rmem_uncharge(c, skb);
		if (!after(cb->end_seq, c->rcv_nxt)) {
			tcp_drop_skb(skb);
			continue;
		}
		if (before(cb->seq, c->rcv_nxt)) {
			uint32_t trim = c->rcv_nxt - cb->seq;

			skb_pull(skb, trim < skb->len ? trim : skb->len);
			cb->seq = c->rcv_nxt;
		}
		fin = cb->tcp_flags & TCP_FIN;
		tcp_queue_rcv(c, skb);
		if (fin) {
			tcp_fin(c);
			break;
		}
	}
	c->ooo_last_skb = rb_to_skb(rb_last(&c->ooo_queue));
}

/* Out-of-order data into the tree: coalesced with the run it continues,
 * duplicates and overlaps trimmed (and reported as D-SACK), the SACK
 * blocks updated.  The ACK goes at once (it carries the news). */
static void tcp_data_queue_ofo(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	struct rb_node **p, *parent = NULL;
	uint32_t seq = cb->seq, end_seq = cb->end_seq;
	sk_buff_t *skb1;

	tcp_ecn_check_ce(c, skb);
	if (tcp_try_rmem_schedule(c, skb, 0) < 0) {
		TCPX_INC(TCPX_OFODROP);
		NET_STATS_INC(NET_MIB_TCP_OOOQUEUEFULL);
		tcp_schedule_ack(c);
		c->ack_pending |= TCP_ACK_NOW;
		tcp_drop_skb(skb);
		return;
	}
	c->pred_flags = 0;
	tcp_schedule_ack(c);
	c->ack_pending |= TCP_ACK_NOW;
	TCPX_INC(TCPX_OFOQUEUE);
	c->rcv_ooopack += cb->segs ? cb->segs : 1;

	p = &c->ooo_queue.rb_node;
	if (rb_empty_root(&c->ooo_queue)) {
		if (c->sack_ok) {
			c->num_sacks = 1;
			c->selective_acks[0].start_seq = seq;
			c->selective_acks[0].end_seq = end_seq;
		}
		rb_link_node(&skb->rbnode, NULL, p);
		rb_insert_color(&skb->rbnode, &c->ooo_queue);
		c->ooo_last_skb = skb;
		goto end;
	}
	// Usually it continues the last run.
	if (tcp_ooo_try_coalesce(c, c->ooo_last_skb, skb)) {
		tcp_drop_skb(skb);
		skb = NULL;
		goto add_sack;
	}
	if (!before(seq, TCP_SKB_CB(c->ooo_last_skb)->end_seq)) {
		parent = &c->ooo_last_skb->rbnode;
		p = &parent->rb_right;
		goto insert;
	}
	while (*p) {
		parent = *p;
		skb1 = rb_to_skb(parent);
		if (before(seq, TCP_SKB_CB(skb1)->seq)) {
			p = &parent->rb_left;
			continue;
		}
		if (before(seq, TCP_SKB_CB(skb1)->end_seq)) {
			if (!after(end_seq, TCP_SKB_CB(skb1)->end_seq)) {
				// all of it is here already
				TCPX_INC(TCPX_OFOMERGE);
				tcp_dsack_set(c, seq, end_seq);
				tcp_drop_skb(skb);
				skb = NULL;
				goto add_sack;
			}
			if (after(seq, TCP_SKB_CB(skb1)->seq)) {
				tcp_dsack_set(c, seq, TCP_SKB_CB(skb1)->end_seq);
			} else {
				// same start, longer: it replaces skb1
				rb_replace_node(&skb1->rbnode, &skb->rbnode,
						&c->ooo_queue);
				tcp_dsack_extend(c, TCP_SKB_CB(skb1)->seq,
						 TCP_SKB_CB(skb1)->end_seq);
				TCPX_INC(TCPX_OFOMERGE);
				skb1->next = skb1->prev = NULL;
				tcp_rmem_uncharge(c, skb1);
				tcp_drop_skb(skb1);
				goto merge_right;
			}
		} else if (tcp_ooo_try_coalesce(c, skb1, skb)) {
			tcp_drop_skb(skb);
			skb = NULL;
			goto add_sack;
		}
		p = &parent->rb_right;
	}
insert:
	rb_link_node(&skb->rbnode, parent, p);
	rb_insert_color(&skb->rbnode, &c->ooo_queue);
merge_right:
	// what the new one covers on its right goes
	skb1 = NULL;
	while ((skb1 = rb_to_skb(rb_next(&skb->rbnode))) != NULL) {
		if (!after(end_seq, TCP_SKB_CB(skb1)->seq))
			break;
		if (before(end_seq, TCP_SKB_CB(skb1)->end_seq)) {
			tcp_dsack_extend(c, TCP_SKB_CB(skb1)->seq, end_seq);
			break;
		}
		rb_erase(&skb1->rbnode, &c->ooo_queue);
		skb1->next = skb1->prev = NULL;
		tcp_dsack_extend(c, TCP_SKB_CB(skb1)->seq,
				 TCP_SKB_CB(skb1)->end_seq);
		TCPX_INC(TCPX_OFOMERGE);
		tcp_rmem_uncharge(c, skb1);
		tcp_drop_skb(skb1);
	}
	if (!skb1)
		c->ooo_last_skb = skb;
add_sack:
	if (c->sack_ok)
		tcp_sack_new_ofo_skb(c, seq, end_seq);
end:
	if (skb) {
		tcp_rmem_charge(c, skb);
		if (c->sack_ok)
			tcp_grow_window(c, skb);
	}
}

/* The text of an accepted segment (RFC 9293 3.10.7.4, seventh).  `skb'
 * has its data at the payload.  Consumes it. */
static void tcp_data_queue(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (cb->seq == cb->end_seq) {
		tcp_drop_skb(skb);
		return;
	}
	tcp_ecn_accept_cwr(c, skb);
	if (cb->seq == c->rcv_nxt) {
		int fin;

		if (tcp_receive_window(c) == 0 && skb->len) {
			// RFC 9293: a zero window takes no data, only ACKs
			TCPX_INC(TCPX_ZEROWINDOWDROP);
			goto out_of_window;
		}
queue_and_out:
		/* In-order data onto an empty receive queue is always taken
		 * (the reader can then always make progress, whatever the
		 * buffer and the global limit say); otherwise only with room
		 * for it. */
		if (skb->len && !skb_queue_empty(&c->rcv_queue) &&
		    tcp_try_rmem_schedule(c, skb, 1) < 0) {
			TCPX_INC(TCPX_RCVQDROP);
			goto out_of_window;
		}
		fin = cb->tcp_flags & TCP_FIN;
		if (skb->len)
			tcp_event_data_recv(c, skb);
		tcp_queue_rcv(c, skb); // consumes skb
		if (fin)
			tcp_fin(c);
		if (!rb_empty_root(&c->ooo_queue)) {
			tcp_ofo_queue(c);
			/* RFC 5681 4.2: a filled hole is ACKed at once. */
			if (rb_empty_root(&c->ooo_queue))
				c->ack_pending |= TCP_ACK_NOW;
		}
		if (c->num_sacks)
			tcp_sack_remove(c);
		return;
	}
	if (!after(cb->end_seq, c->rcv_nxt)) {
		// a retransmission of what we have: D-SACK, ACK at once
		tcp_dsack_set(c, cb->seq, cb->end_seq);
out_of_window:
		tcp_enter_quickack_mode(c, TCP_MAX_QUICKACKS);
		tcp_schedule_ack(c);
		tcp_drop_skb(skb);
		return;
	}
	// beyond the window (a window probe, say)
	if (!before(cb->seq, c->rcv_nxt + tcp_receive_window(c)))
		goto out_of_window;
	if (before(cb->seq, c->rcv_nxt)) {
		/* Partly old: report the old part, keep the new. */
		uint32_t trim = c->rcv_nxt - cb->seq;

		tcp_dsack_set(c, cb->seq, c->rcv_nxt);
		if (!tcp_receive_window(c)) {
			TCPX_INC(TCPX_ZEROWINDOWDROP);
			goto out_of_window;
		}
		skb_pull(skb, trim < skb->len ? trim : skb->len);
		cb->seq = c->rcv_nxt;
		goto queue_and_out;
	}
	tcp_data_queue_ofo(c, skb);
}

/* The peer's FIN, in order (RFC 9293 3.10.7.4, eighth). */
static void tcp_fin(tcp_conn_t *c)
{
	tcp_schedule_ack(c);
	switch (c->state) {
	case TCP_STATE_SYN_RECEIVED:
	case TCP_STATE_ESTABLISHED:
		tcp_set_state(c, TCP_STATE_CLOSE_WAIT);
		/* Our close() usually follows: its FIN carries the ACK. */
		c->ack_pingpong = 1;
		break;
	case TCP_STATE_CLOSE_WAIT:
	case TCP_STATE_CLOSING:
	case TCP_STATE_LAST_ACK:
		break;
	case TCP_STATE_FIN_WAIT_1:
		// simultaneous close
		tcp_send_ack(c);
		tcp_set_state(c, TCP_STATE_CLOSING);
		break;
	case TCP_STATE_FIN_WAIT_2:
		tcp_send_ack(c);
		tcp_time_wait(c, TCP_STATE_TIME_WAIT,
			      tcp_ms(TCP_TIMEWAIT_LEN_MS));
		break;
	default:
		break;
	}
	/* Anything out of order beyond the FIN is meaningless. */
	tcp_ofo_purge(c);
	c->num_sacks = 0;
	// EOF for the reader.
	tcp_poll_notify_key(c, POLLIN | POLLRDNORM | POLLRDHUP);
	sched_wake_channel((void *)&c->rx_ready);
}

// ===========================================================================
// Urgent data (RFC 9293 3.8.5, RFC 6093).
//
// The urgent pointer points just past the urgent byte (as every stack
// sends it).  A new pointer is taken from any acceptable segment; the byte
// itself only once it is received in order (it is then on the receive
// queue): what an out-of-order or beyond-the-window segment claims is never
// believed.  Without SO_OOBINLINE the byte is not part of the stream: a
// read stops at it and skips it, recv(MSG_OOB) returns it; SIOCATMARK says
// whether the next byte read is the mark.  Only the latest mark counts.
// ===========================================================================
/* The urgent byte of the previous mark is the next byte to read and a new
 * mark replaces it: it leaves the stream now (it was out of band). */
static void tcp_urg_skip_byte(tcp_conn_t *c)
{
	sk_buff_t *head = skb_peek(&c->rcv_queue);

	c->copied_seq++;
	if (c->rcvq_bytes)
		c->rcvq_bytes--;
	if (head && !before(c->copied_seq, TCP_SKB_CB(head)->seq + head->len)) {
		__skb_unlink(head, &c->rcv_queue);
		tcp_rmem_uncharge(c, head);
		skb_put(head);
	}
	if (!c->rcvq_bytes)
		c->rx_ready = 0;
}

/* A segment with URG (validated, before its data is queued). */
static void tcp_check_urg(tcp_conn_t *c, const sk_buff_t *skb)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t ptr = cb->urg_ptr;

	if (ptr)
		ptr--; // the urgent byte itself
	ptr += cb->seq;
	if (after(c->copied_seq, ptr)) // read already
		return;
	if (before(ptr, c->rcv_nxt)) // received already: not replayed
		return;
	if (c->urg_state != TCP_URG_NONE && !after(ptr, c->rcv_up))
		return; // a duplicate, or older than the mark we have
	if (c->urg_state != TCP_URG_NONE && c->rcv_up == c->copied_seq &&
	    !c->urg_inline && c->copied_seq != c->rcv_nxt)
		tcp_urg_skip_byte(c);
	c->urg_state = TCP_URG_NOTYET;
	c->rcv_up = ptr;
	c->pred_flags = 0;
}

/* After the data was queued: is the urgent byte now received in order?
 * It is then on the receive queue (nothing read it yet: the reader needs
 * the lock this runs under). */
static void tcp_urg_capture(tcp_conn_t *c)
{
	sk_buff_t *skb;

	if (c->urg_state != TCP_URG_NOTYET || !after(c->rcv_nxt, c->rcv_up))
		return;
	for (skb = skb_peek_tail(&c->rcv_queue); skb; skb = skb->prev) {
		const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

		if (!before(c->rcv_up, cb->seq) &&
		    before(c->rcv_up, cb->seq + skb->len)) {
			c->urgent_byte = skb->data[c->rcv_up - cb->seq];
			c->urg_state = TCP_URG_VALID;
			tcp_poll_notify_key(c, POLLPRI | POLLRDBAND);
			sched_wake_channel((void *)&c->rx_ready);
			return;
		}
		if (before(cb->seq, c->rcv_up))
			break;
	}
	c->urg_state = TCP_URG_NONE; // gone (read or reset): no mark
}

// ===========================================================================
// ACK scheduling.
// ===========================================================================
static void __tcp_ack_snd_check(tcp_conn_t *c, int ofo_possible)
{
	if (((c->rcv_nxt - c->rcv_wup) > c->rcv_mss &&
	     __tcp_select_window(c) >= c->rcv_wnd) ||
	    tcp_in_quickack_mode(c) || (c->ack_pending & TCP_ACK_NOW)) {
		if (tcp_in_quickack_mode(c))
			TCPX_INC(TCPX_QUICKACKS);
		tcp_send_ack(c);
		return;
	}
	if (!ofo_possible || rb_empty_root(&c->ooo_queue)) {
		tcp_send_delayed_ack(c);
		return;
	}
	tcp_send_ack(c);
}

static void tcp_ack_snd_check(tcp_conn_t *c)
{
	if (!(c->ack_pending & TCP_ACK_SCHED))
		return;
	__tcp_ack_snd_check(c, 1);
}

/* Window update on read (the reader freed buffer space): an owed ACK goes
 * now when more than one segment waits for it or a pushed segment was
 * read out completely; otherwise an ACK only for a window that at least
 * doubled, while data may still come. */
void tcp_cleanup_rbuf(tcp_conn_t *c, int copied)
{
	int time_to_ack = 0;

	if (c->ack_pending & TCP_ACK_SCHED) {
		if (c->rcv_nxt - c->rcv_wup > c->rcv_mss ||
		    (copied > 0 &&
		     ((c->ack_pending & TCP_ACK_PUSHED2) ||
		      ((c->ack_pending & TCP_ACK_PUSHED) && !c->ack_pingpong)) &&
		     !c->rmem_alloc))
			time_to_ack = 1;
	}
	if (copied > 0 && !time_to_ack &&
	    (c->state == TCP_STATE_ESTABLISHED ||
	     c->state == TCP_STATE_FIN_WAIT_1 ||
	     c->state == TCP_STATE_FIN_WAIT_2)) {
		uint32_t now = tcp_receive_window(c);

		if (2 * now <= c->window_clamp) {
			uint32_t nw = __tcp_select_window(c);

			if (nw && nw >= 2 * now)
				time_to_ack = 1;
		}
	}
	if (time_to_ack)
		tcp_send_ack(c);
}

// ===========================================================================
// Validation.
// ===========================================================================
static int tcp_paws_check(const tcp_conn_t *c, uint32_t tsval, int win)
{
	if ((int32_t)(c->ts_recent - tsval) <= win)
		return 1;
	if (tcp_now() - c->ts_recent_stamp >= timer_s_to_ticks(TCP_PAWS_24DAYS_S))
		return 1;
	if (!c->ts_recent)
		return 1;
	return 0;
}

/* An old-looking pure ACK may be a reordered duplicate ACK, not an old
 * segment; PAWS lets it through. */
static int tcp_disordered_ack(const tcp_conn_t *c, const sk_buff_t *skb,
			      uint32_t tsval)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t nwin = (uint32_t)cb->window << (c->ws_enabled ? c->snd_wscale : 0);

	return (cb->tcp_flags & TCP_ACK) && cb->seq == cb->end_seq &&
	       cb->seq == c->rcv_nxt && cb->ack_seq == c->snd_una &&
	       !tcp_may_update_window(c, cb->ack_seq, cb->seq, nwin) &&
	       (int32_t)(c->ts_recent - tsval) <=
		       (int32_t)(c->rto_us / 1000u + 1u);
}

static int tcp_paws_discard(const tcp_conn_t *c, const sk_buff_t *skb,
			    uint32_t tsval)
{
	return !tcp_paws_check(c, tsval, 1) && !tcp_disordered_ack(c, skb, tsval);
}

/* After a FIN: a reset at rcv_nxt - 1 is still the peer's. */
static int tcp_reset_check(const tcp_conn_t *c, const sk_buff_t *skb)
{
	return TCP_SKB_CB(skb)->seq == c->rcv_nxt - 1 &&
	       (c->state == TCP_STATE_CLOSE_WAIT ||
		c->state == TCP_STATE_LAST_ACK ||
		c->state == TCP_STATE_CLOSING);
}

static int tcp_sequence(const tcp_conn_t *c, uint32_t seq, uint32_t end_seq)
{
	return !before(end_seq, c->rcv_wup) &&
	       !after(seq, c->rcv_nxt + tcp_receive_window(c));
}

static void tcp_reset_rcv(tcp_conn_t *c)
{
	int err;

	switch (c->state) {
	case TCP_STATE_SYN_SENT:
		err = ECONNREFUSED;
		NET_STATS_INC(NET_MIB_TCP_ATTEMPTFAILS);
		break;
	case TCP_STATE_CLOSE_WAIT:
		err = EPIPE;
		break;
	case TCP_STATE_CLOSED:
		return;
	default:
		err = ECONNRESET;
		break;
	}
	if (c->state == TCP_STATE_ESTABLISHED ||
	    c->state == TCP_STATE_CLOSE_WAIT)
		NET_STATS_INC(NET_MIB_TCP_ESTABRESETS);
	if (c->packets_out || c->rcvq_bytes)
		NET_STATS_INC(NET_MIB_TCP_RSTDATALOSS);
	tcp_reset(c, err);
}

/* PAWS, the sequence check, RST and SYN (RFC 9293 3.10.7.4 first to
 * fourth, RFC 5961, RFC 7323 5.3).  Returns 1 to go on; otherwise the
 * segment was consumed. */
static int tcp_validate_incoming(tcp_conn_t *c, sk_buff_t *skb,
				 const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint8_t flg = cb->tcp_flags;

	if (c->ts_enabled && opt->ts_present &&
	    tcp_paws_discard(c, skb, opt->tsval)) {
		if (!(flg & TCP_RST)) {
			if (flg & TCP_SYN)
				goto syn_challenge;
			NET_STATS_INC(NET_MIB_TCP_PAWSDROP);
			TCPX_INC(TCPX_PAWSESTAB);
			if (!tcp_oow_rate_limited(c, skb, TCPX_ACKSKIPPEDPAWS))
				tcp_send_dupack(c, skb);
			goto discard;
		}
		// a reset is accepted even when it fails PAWS
	}
	if (!tcp_sequence(c, cb->seq, cb->end_seq)) {
		if (!(flg & TCP_RST)) {
			if (flg & TCP_SYN)
				goto syn_challenge;
			NET_STATS_INC(NET_MIB_TCP_OOWSEQDROP);
			if (!tcp_oow_rate_limited(c, skb, TCPX_ACKSKIPPEDSEQ))
				tcp_send_dupack(c, skb);
		} else if (tcp_reset_check(c, skb)) {
			goto reset;
		}
		goto discard;
	}
	if (flg & TCP_RST) {
		/* RFC 5961 3.2: only a reset exactly at rcv_nxt (or at the
		 * right edge of the last SACK block) is honoured; another
		 * one in the window gets a challenge ACK. */
		if (cb->seq == c->rcv_nxt || tcp_reset_check(c, skb))
			goto reset;
		if (c->sack_ok && c->num_sacks) {
			uint32_t max_sack = c->selective_acks[0].end_seq;

			for (uint32_t i = 1; i < c->num_sacks; i++)
				if (after(c->selective_acks[i].end_seq, max_sack))
					max_sack = c->selective_acks[i].end_seq;
			if (cb->seq == max_sack)
				goto reset;
		}
		tcp_send_challenge_ack(c);
		goto discard;
	}
	if (flg & TCP_SYN) {
		/* Our own SYN-ACK of a simultaneous open, looped back. */
		if (c->state == TCP_STATE_SYN_RECEIVED && c->owner_socket &&
		    (flg & TCP_ACK) && cb->seq + 1 == cb->end_seq &&
		    cb->seq + 1 == c->rcv_nxt && cb->ack_seq == c->snd_nxt)
			return 1;
syn_challenge:
		TCPX_INC(TCPX_SYNCHALLENGE);
		tcp_send_challenge_ack(c);
		goto discard;
	}
	return 1;
discard:
	tcp_drop_skb(skb);
	return 0;
reset:
	tcp_reset_rcv(c);
	tcp_drop_skb(skb);
	return 0;
}

/* RFC 7323 4.3: remember the TSval to echo -- from a segment that starts
 * at or before Last.ACK.sent and is not older than the one held. */
static void tcp_replace_ts_recent(tcp_conn_t *c, uint32_t seq,
				  const tcp_parsed_opts_t *opt)
{
	if (opt->ts_present && !after(seq, c->rcv_wup) &&
	    tcp_paws_check(c, opt->tsval, 0)) {
		c->ts_recent = opt->tsval;
		c->ts_recent_stamp = tcp_now();
	}
}

// ===========================================================================
// The handshake (active side) and the transfer state.
// ===========================================================================
void tcp_init_transfer(tcp_conn_t *c)
{
	uint64_t now = tcp_now();

	tcp_init_cwnd(c);
	c->ca_state = TCP_CA_OPEN;
	if (!c->ca_ops)
		c->ca_ops = tcp_ca_default();
	tcp_ca_init(c);
	c->high_seq = c->snd_nxt;
	c->cwnd_usage_seq = c->snd_nxt;
	c->rack.reo_wnd_steps = 1;
	tcp_update_pacing_rate(c);
	c->copied_seq = c->rcv_nxt;
	tcp_initialize_rcv_mss(c);
	tcp_init_buffer_space(c);
	c->lsndtime = now;
	c->ack_lrcvtime = now;
	c->last_rx_tick = now;
	c->retransmits = 0;
	c->rto_backoff = 0;
	c->retrans_stamp = 0;
	if (c->keepalive)
		tcp_reset_keepalive_timer(c, c->keepidle_ticks);
	// header prediction on
	c->pred_flags = 1;
}

static void tcp_fast_path_check(tcp_conn_t *c)
{
	c->pred_flags = rb_empty_root(&c->ooo_queue) && c->rcv_wnd &&
			c->rmem_alloc < (uint32_t)c->rcvbuf && !c->snd_urg_pending &&
			c->urg_state == TCP_URG_NONE;
}

static void tcp_finish_connect(tcp_conn_t *c)
{
	tcp_set_state(c, TCP_STATE_ESTABLISHED);
	tcp_init_transfer(c);
	c->connect_done = 1;
	/* Wake a non-blocking connect()er parked in poll(POLLOUT): the
	 * success path has to notify as surely as the failure path. */
	tcp_poll_notify_key(c, POLLOUT | POLLWRNORM);
	sched_wake_channel((void *)&c->rx_ready);
}

/* SYN_SENT (RFC 9293 3.10.7.3).  Returns 1 when a reset must answer the
 * segment (the caller sends it); the skb is consumed either way except
 * then. */
static int tcp_rcv_synsent_state_process(tcp_conn_t *c, sk_buff_t *skb,
					 const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint8_t flg = cb->tcp_flags;

	if (flg & TCP_ACK) {
		/* The only acceptable ACK is the one for our SYN; another
		 * belongs to some other incarnation of this tuple and gets
		 * a reset (unless it is one) -- a RST carrying it must not
		 * fail the connect(). */
		if (!after(cb->ack_seq, c->iss) || after(cb->ack_seq, c->snd_nxt))
			return 1;
		if (opt->ts_present && opt->tsecr && c->syn_tsval &&
		    !between(opt->tsecr, c->syn_tsval,
			     tcp_ts_now(c->ts_offset))) {
			TCPX_INC(TCPX_PAWSACTIVE);
			tcp_drop_skb(skb);
			return 0;
		}
		if (flg & TCP_RST) {
			tcp_reset_rcv(c);
			tcp_drop_skb(skb);
			return 0;
		}
		if (!(flg & TCP_SYN)) {
			tcp_drop_skb(skb);
			return 0;
		}
		// Our SYN is acknowledged: negotiate and establish.
		c->irs = cb->seq;
		/* ECN (RFC 3168 6.1.1): only when we asked, and the answer is
		 * ECE alone. */
		if ((c->ecn_flags & TCP_ECN_OK) &&
		    (flg & (TCP_ECE | TCP_CWR)) != TCP_ECE)
			c->ecn_flags = 0;
		c->snd_wl1 = cb->seq;
		c->ts_enabled = opt->ts_present;
		c->sack_ok = opt->sack_perm;
		c->ws_enabled = opt->wscale >= 0;
		if (c->ws_enabled) {
			c->snd_wscale = (uint8_t)opt->wscale;
		} else {
			c->snd_wscale = 0;
			c->rcv_wscale = 0;
			c->window_clamp = tcp_min32(c->window_clamp, 65535u);
		}
		c->peer_mss = opt->mss ? opt->mss : TCP_MSS_DEFAULT;
		c->mss_clamp = c->user_mss && c->user_mss < c->peer_mss ?
				       c->user_mss :
				       c->peer_mss;
		if (c->ts_enabled) {
			c->tcp_header_len = sizeof(tcp_header_t) + 12;
			c->ts_recent = opt->tsval;
			c->ts_recent_stamp = tcp_now();
		} else {
			c->tcp_header_len = sizeof(tcp_header_t);
		}
		c->rcv_tsecr = opt->ts_present ? opt->tsecr : 0;
		tcp_ack(c, skb, opt, FLAG_SLOWPATH | FLAG_SYNACK);
		c->rcv_nxt = cb->seq + 1;
		c->rcv_wup = cb->seq + 1;
		c->snd_wnd = cb->window; // a SYN's window is never scaled
		c->max_window = c->snd_wnd;
		/* The TSecr times our SYN, resent or not: the first RTT
		 * sample (the queue cleaning took the time-based one when
		 * the SYN went once). */
		if (c->ts_enabled && !c->srtt_us)
			tcp_rtt_sample_ts(c, opt);
		tcp_sync_mss(c, c->pmtu_cookie);
		c->copied_seq = c->rcv_nxt;
		tcp_finish_connect(c);
		if (c->ack_pingpong) {
			tcp_schedule_ack(c);
			tcp_enter_quickack_mode(c, TCP_MAX_QUICKACKS);
			c->ack_timeout = tcp_now() + tcp_ms(TCP_DELACK_MAX_MS);
			c->ack_pending |= TCP_ACK_TIMER;
			tcp_reset_delack_timer(c, c->ack_timeout);
		} else {
			tcp_send_ack(c);
		}
		tcp_drop_skb(skb);
		return 0;
	}
	if (flg & TCP_RST) {
		// a reset without ACK cannot prove it saw our SYN
		tcp_drop_skb(skb);
		return 0;
	}
	if (c->ts_recent_stamp && opt->ts_present &&
	    tcp_paws_reject_recent(c->ts_recent, c->ts_recent_stamp,
				   opt->tsval, 0)) {
		tcp_drop_skb(skb);
		return 0;
	}
	if (flg & TCP_SYN) {
		/* Crossed SYNs: a simultaneous open (RFC 9293 3.5, figure
		 * 8), or a socket connecting to itself. */
		tcp_set_state(c, TCP_STATE_SYN_RECEIVED);
		c->irs = cb->seq;
		if ((c->ecn_flags & TCP_ECN_OK) &&
		    (flg & (TCP_ECE | TCP_CWR)) != (TCP_ECE | TCP_CWR))
			c->ecn_flags = 0;
		c->ts_enabled = opt->ts_present;
		if (c->ts_enabled) {
			c->ts_recent = opt->tsval;
			c->ts_recent_stamp = tcp_now();
			c->tcp_header_len = sizeof(tcp_header_t) + 12;
		} else {
			c->tcp_header_len = sizeof(tcp_header_t);
		}
		c->sack_ok = opt->sack_perm;
		c->ws_enabled = opt->wscale >= 0;
		if (c->ws_enabled) {
			c->snd_wscale = (uint8_t)opt->wscale;
		} else {
			c->snd_wscale = 0;
			c->rcv_wscale = 0;
			c->window_clamp = tcp_min32(c->window_clamp, 65535u);
		}
		c->peer_mss = opt->mss ? opt->mss : TCP_MSS_DEFAULT;
		c->mss_clamp = c->user_mss && c->user_mss < c->peer_mss ?
				       c->user_mss :
				       c->peer_mss;
		c->rcv_nxt = cb->seq + 1;
		c->copied_seq = c->rcv_nxt;
		c->rcv_wup = c->rcv_nxt;
		c->snd_wnd = cb->window;
		c->snd_wl1 = cb->seq;
		c->max_window = c->snd_wnd;
		tcp_sync_mss(c, c->pmtu_cookie);
		tcp_initialize_rcv_mss(c);
		tcp_send_synack(c);
		tcp_reset_xmit_timer(c, TCP_TIME_RETRANS, tcp_syn_rto_ticks(0));
		c->retransmits = 0;
		tcp_drop_skb(skb);
		return 0;
	}
	tcp_drop_skb(skb);
	return 0;
}

// ===========================================================================
// The established fast path and the state machine.
// ===========================================================================
/* Header prediction: the segment is exactly what an ESTABLISHED
 * connection expects next -- only ACK (and PSH) set, the header as long as
 * the options we use, the window unchanged, sequence rcv_nxt, an ACK for
 * something we sent, and a timestamp no older than the last one. */
static int tcp_rcv_established_fast(tcp_conn_t *c, sk_buff_t *skb,
				    const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	const uint8_t *th = skb->data;
	uint32_t doff = (uint32_t)(th[12] >> 4) * 4;
	uint32_t pred_win = c->snd_wnd >> (c->ws_enabled ? c->snd_wscale : 0);

	if (!c->pred_flags)
		return 0;
	if ((cb->tcp_flags & ~TCP_PSH) != TCP_ACK || doff != c->tcp_header_len ||
	    cb->window != pred_win || cb->seq != c->rcv_nxt ||
	    after(cb->ack_seq, c->snd_nxt))
		return 0;
	if (c->ts_enabled) {
		if (!opt->ts_present || opt->sack_count)
			return 0;
		if ((int32_t)(opt->tsval - c->ts_recent) < 0)
			return 0;
	} else if (opt->ts_present) {
		return 0;
	}
	if (cb->seq == cb->end_seq) {
		// a pure ACK
		if (c->ts_enabled && c->rcv_nxt == c->rcv_wup) {
			c->ts_recent = opt->tsval;
			c->ts_recent_stamp = tcp_now();
		}
		TCPX_INC(TCPX_HPACKS);
		c->last_rx_tick = tcp_now();
		c->probes_out = 0;
		/* Every pure ACK goes through tcp_ack: a duplicate is the
		 * loss signal of a peer without SACK. */
		if (cb->ack_seq == c->snd_una)
			TCPX_INC(TCPX_PUREACKS);
		tcp_ack(c, skb, opt, 0);
		c->rcv_rtt_last_tsecr = c->rcv_tsecr;
		tcp_drop_skb(skb);
		tcp_data_snd_check(c);
		return 1;
	}
	// in-order data
	if (tcp_receive_window(c) < cb->end_seq - cb->seq ||
	    c->rmem_alloc + skb->truesize > (uint32_t)c->rcvbuf ||
	    tcp_mem_over_limit())
		return 0;
	if (c->ts_enabled && c->rcv_nxt == c->rcv_wup) {
		c->ts_recent = opt->tsval;
		c->ts_recent_stamp = tcp_now();
	}
	tcp_rcv_rtt_measure_ts(c, skb);
	TCPX_INC(TCPX_HPHITS);
	c->last_rx_tick = tcp_now();
	{
		int new_ack = cb->ack_seq != c->snd_una;

		if (new_ack)
			tcp_ack(c, skb, opt, FLAG_DATA);
		else
			c->snd_wl1 = cb->seq;
		skb_pull(skb, doff);
		tcp_event_data_recv(c, skb);
		tcp_queue_rcv(c, skb); // consumes skb
		if (new_ack)
			tcp_data_snd_check(c);
		if (c->ack_pending & TCP_ACK_SCHED)
			__tcp_ack_snd_check(c, 0);
	}
	return 1;
}

static void tcp_rcv_established(tcp_conn_t *c, sk_buff_t *skb,
				const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t doff = (uint32_t)(skb->data[12] >> 4) * 4;

	if (tcp_rcv_established_fast(c, skb, opt))
		return;
	if (!(cb->tcp_flags & (TCP_ACK | TCP_RST | TCP_SYN))) {
		tcp_drop_skb(skb);
		return;
	}
	if (!tcp_validate_incoming(c, skb, opt))
		return;
	c->last_rx_tick = tcp_now();
	if (tcp_ack(c, skb, opt, FLAG_SLOWPATH | FLAG_UPDATE_TS_RECENT) < 0) {
		tcp_drop_skb(skb);
		return;
	}
	tcp_rcv_rtt_measure_ts(c, skb);
	skb_pull(skb, doff);
	if (cb->tcp_flags & TCP_URG)
		tcp_check_urg(c, skb);
	tcp_data_queue(c, skb);
	if (c->urg_state == TCP_URG_NOTYET)
		tcp_urg_capture(c);
	if (c->state != TCP_STATE_CLOSED) {
		tcp_data_snd_check(c);
		tcp_ack_snd_check(c);
		tcp_fast_path_check(c);
	}
}

void tcp_rcv_state_process(tcp_conn_t *c, sk_buff_t *skb,
			   const tcp_parsed_opts_t *opt)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t doff = (uint32_t)(skb->data[12] >> 4) * 4;
	int acceptable;

	c->rcv_tsecr = opt->ts_present ? opt->tsecr : 0;
	c->segs_in += cb->segs ? cb->segs : 1;
	if (skb->len > doff)
		c->data_segs_in += cb->segs ? cb->segs : 1;
	switch (c->state) {
	case TCP_STATE_CLOSED:
	case TCP_STATE_LISTEN:
		tcp_drop_skb(skb);
		return;
	case TCP_STATE_SYN_SENT:
		if (tcp_rcv_synsent_state_process(c, skb, opt)) {
			tcp_v4_send_reset(skb->dev, c->local_ip, c->remote_ip,
					  skb->data, skb->len);
			tcp_drop_skb(skb);
		}
		return;
	case TCP_STATE_ESTABLISHED:
		tcp_rcv_established(c, skb, opt);
		return;
	default:
		break;
	}
	if (!(cb->tcp_flags & (TCP_ACK | TCP_RST | TCP_SYN))) {
		tcp_drop_skb(skb);
		return;
	}
	if (!tcp_validate_incoming(c, skb, opt))
		return;
	c->last_rx_tick = tcp_now();

	// fifth: the ACK
	if (!(cb->tcp_flags & TCP_ACK)) {
		tcp_drop_skb(skb);
		return;
	}
	/* An unacceptable ACK is answered here, once: a reset in
	 * SYN_RECEIVED, else a (rate-limited) challenge ACK -- tcp_ack()
	 * itself stays silent (FLAG_NO_CHALLENGE_ACK). */
	acceptable = tcp_ack(c, skb, opt,
			     FLAG_SLOWPATH | FLAG_NO_CHALLENGE_ACK |
				     FLAG_UPDATE_TS_RECENT);
	if (acceptable < 0) {
		if (c->state == TCP_STATE_SYN_RECEIVED) {
			tcp_v4_send_reset(skb->dev, c->local_ip, c->remote_ip,
					  skb->data, skb->len);
		} else {
			tcp_send_challenge_ack(c);
		}
		tcp_drop_skb(skb);
		return;
	}
	switch (c->state) {
	case TCP_STATE_SYN_RECEIVED:
		/* The handshake of a simultaneous open completes: finish the
		 * connect() that is waiting for exactly this. */
		if (cb->ack_seq != c->snd_nxt || acceptable == 0) {
			tcp_v4_send_reset(skb->dev, c->local_ip, c->remote_ip,
					  skb->data, skb->len);
			tcp_drop_skb(skb);
			return;
		}
		if (!c->srtt_us && c->ts_enabled)
			tcp_rtt_sample_ts(c, opt);
		c->snd_una = cb->ack_seq;
		c->snd_wnd = (uint32_t)cb->window
			     << (c->ws_enabled ? c->snd_wscale : 0);
		c->snd_wl1 = cb->seq;
		c->max_window = tcp_max32(c->max_window, c->snd_wnd);
		tcp_finish_connect(c);
		break;
	case TCP_STATE_FIN_WAIT_1:
		if (c->snd_una != c->write_seq)
			break;
		tcp_set_state(c, TCP_STATE_FIN_WAIT_2);
		if (!tcp_is_orphan(c)) {
			// our sending side is over: writers learn EPIPE
			tcp_poll_notify_key(c, POLLOUT | POLLWRNORM);
			break;
		}
		if (c->linger2_ms < 0) {
			TCPX_INC(TCPX_ABORTONDATA);
			tcp_done(c);
			tcp_drop_skb(skb);
			return;
		}
		/* An orphan whose peer sends data after our close cannot
		 * deliver it anywhere. */
		if (cb->end_seq != cb->seq &&
		    after(cb->end_seq - ((cb->tcp_flags & TCP_FIN) ? 1 : 0),
			  c->rcv_nxt)) {
			TCPX_INC(TCPX_ABORTONDATA);
			tcp_send_active_reset(c);
			tcp_done(c);
			tcp_drop_skb(skb);
			return;
		}
		{
			uint64_t tmo = tcp_ms(c->linger2_ms ?
						      (uint64_t)c->linger2_ms :
						      TCP_FIN_TIMEOUT_MS);

			if (tmo > tcp_ms(TCP_TIMEWAIT_LEN_MS)) {
				tcp_reset_keepalive_timer(
					c, tmo - tcp_ms(TCP_TIMEWAIT_LEN_MS));
			} else if (cb->tcp_flags & TCP_FIN) {
				/* Its FIN comes with this very segment:
				 * the FIN_WAIT2 hand-over happens there. */
				tcp_reset_keepalive_timer(c, tmo);
			} else {
				tcp_time_wait(c, TCP_STATE_FIN_WAIT_2, tmo);
				tcp_drop_skb(skb);
				return;
			}
		}
		break;
	case TCP_STATE_CLOSING:
		if (c->snd_una == c->write_seq) {
			tcp_time_wait(c, TCP_STATE_TIME_WAIT,
				      tcp_ms(TCP_TIMEWAIT_LEN_MS));
			tcp_drop_skb(skb);
			return;
		}
		break;
	case TCP_STATE_LAST_ACK:
		if (c->snd_una == c->write_seq) {
			tcp_done(c);
			tcp_drop_skb(skb);
			return;
		}
		break;
	default:
		break;
	}

	skb_pull(skb, doff);
	if (cb->tcp_flags & TCP_URG)
		tcp_check_urg(c, skb);
	switch (c->state) {
	case TCP_STATE_CLOSE_WAIT:
	case TCP_STATE_CLOSING:
	case TCP_STATE_LAST_ACK:
		// after the peer's FIN only a retransmission can come
		if (!before(cb->seq, c->rcv_nxt)) {
			tcp_drop_skb(skb);
			skb = NULL;
			break;
		}
		/* fall through */
	case TCP_STATE_FIN_WAIT_1:
	case TCP_STATE_FIN_WAIT_2:
		/* We closed (not just shut down sending): new data has no
		 * reader -- RFC 1122 4.2.2.13 wants a reset. */
		if (skb && tcp_is_orphan(c) && cb->end_seq != cb->seq &&
		    after(cb->end_seq - ((cb->tcp_flags & TCP_FIN) ? 1 : 0),
			  c->rcv_nxt)) {
			TCPX_INC(TCPX_ABORTONDATA);
			tcp_send_active_reset(c);
			tcp_reset(c, ECONNRESET);
			tcp_drop_skb(skb);
			return;
		}
		/* fall through */
	case TCP_STATE_ESTABLISHED:
		if (skb)
			tcp_data_queue(c, skb);
		break;
	default:
		tcp_drop_skb(skb);
		break;
	}
	if (c->urg_state == TCP_URG_NOTYET)
		tcp_urg_capture(c);
	if (c->state != TCP_STATE_CLOSED) {
		tcp_data_snd_check(c);
		tcp_ack_snd_check(c);
	}
}
