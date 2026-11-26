// LikeOS TCP: loss detection and recovery
//
// The loss state machine of a sender (conn->ca_state):
//   Open      nothing suspicious;
//   Disorder  SACKs or duplicate ACKs arrived, nothing is presumed lost yet;
//   CWR       the window is being reduced for an ECN echo (or a loss the
//             tail loss probe repaired), nothing to retransmit;
//   Recovery  fast recovery: segments presumed lost are resent, the window
//             falls to ssthresh by Proportional Rate Reduction (RFC 6937);
//   Loss      after a retransmission timeout: everything outstanding not
//             sent recently is presumed lost and resent in slow start.
//
// Loss detection is RACK (RFC 8985): a segment is lost when a segment sent
// at least a reordering window later was delivered.  The window is a
// quarter of the minimum RTT, zero while no reordering was ever seen and
// the classic three-duplicate rule would fire anyway, and grows by a
// quarter min_rtt per round that saw D-SACKs (spurious recoveries).
// Losses whose window has not run out yet get the REO timer.  A peer
// without SACK gets NewReno (RFC 6582): the head is lost after three
// duplicate ACKs, and every partial ACK of the recovery resends the next.
//
// The tail loss probe (TLP, RFC 8985 7): when the last segments of a
// flight may have been lost, nothing would trigger a fast recovery; after
// two smoothed RTTs (plus the delayed-ACK time when only one segment is
// out) new data, or else the last segment, goes again to elicit the ACK
// or SACK that lets RACK repair the losses without waiting for the RTO.
//
// Undo: a reduction turns out spurious when every retransmission of the
// episode is reported twice by D-SACK (RFC 3708), or when the echoed
// timestamp of the ACK predates the first retransmission (Eifel, RFC
// 3522), or -- after a timeout -- when F-RTO (RFC 5682, SACK-enhanced)
// sees data never retransmitted acknowledged.  The window and ssthresh go
// back to what they were.
//
// Everything runs under conn->lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>

// ===========================================================================
// The scoreboard's lost marks.
// ===========================================================================
void tcp_clear_retrans_hints(tcp_conn_t *c)
{
	c->retransmit_skb_hint = NULL;
}

/* The lost walk resumes at the earliest lost segment. */
static void tcp_verify_retransmit_hint(tcp_conn_t *c, sk_buff_t *skb)
{
	if (!c->retransmit_skb_hint ||
	    before(TCP_SKB_CB(skb)->seq,
		   TCP_SKB_CB(c->retransmit_skb_hint)->seq))
		c->retransmit_skb_hint = skb;
}

void tcp_mark_skb_lost(tcp_conn_t *c, sk_buff_t *skb)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

	if (cb->sacked & TCPCB_SACKED_ACKED)
		return;
	tcp_verify_retransmit_hint(c, skb);
	if (cb->sacked & TCPCB_LOST) {
		if (cb->sacked & TCPCB_SACKED_RETRANS) {
			// a retransmission lost again: resend it
			cb->sacked &= (uint8_t)~TCPCB_SACKED_RETRANS;
			if (c->retrans_out)
				c->retrans_out--;
			c->lost++;
			TCPX_INC(TCPX_LOSTRETRANSMIT);
		}
	} else {
		c->lost_out++;
		cb->sacked |= TCPCB_LOST;
		c->lost++;
	}
}

int tcp_any_retrans_done(const tcp_conn_t *c)
{
	sk_buff_t *head = skb_peek(&c->rtx_queue);

	if (c->retrans_out)
		return 1;
	return head && (TCP_SKB_CB(head)->sacked & TCPCB_EVER_RETRANS);
}

static void tcp_enter_recovery(tcp_conn_t *c, int ece_ack);

// ===========================================================================
// The time-ordered list of the segments in flight.
//
// RACK (RFC 8985) asks of each segment sent before the most recently
// delivered one whether it is overdue.  Walking the retransmit queue for
// that costs every SACKed segment on every ACK: a receiver that SACKs all
// but the head would make each ACK cost the whole flight.  So the segments
// that can still be lost are kept on a list of their own in the order they
// were last sent: a transmission (new or again) moves a segment to its
// tail, a SACK or a RACK loss takes it off, and the walk stops at the first
// segment sent after the latest delivery.  Under conn->lock.
// ===========================================================================
static inline int tcp_tsorted_on(const tcp_conn_t *c, const sk_buff_t *skb)
{
	return skb->ts_prev != NULL || c->tsorted_head == skb;
}

void tcp_tsorted_del(tcp_conn_t *c, sk_buff_t *skb)
{
	if (!tcp_tsorted_on(c, skb))
		return;
	if (skb->ts_prev)
		skb->ts_prev->ts_next = skb->ts_next;
	else
		c->tsorted_head = skb->ts_next;
	if (skb->ts_next)
		skb->ts_next->ts_prev = skb->ts_prev;
	else
		c->tsorted_tail = skb->ts_prev;
	skb->ts_next = skb->ts_prev = NULL;
}

void tcp_tsorted_move_tail(tcp_conn_t *c, sk_buff_t *skb)
{
	tcp_tsorted_del(c, skb);
	skb->ts_prev = c->tsorted_tail;
	skb->ts_next = NULL;
	if (c->tsorted_tail)
		c->tsorted_tail->ts_next = skb;
	else
		c->tsorted_head = skb;
	c->tsorted_tail = skb;
}

/* `skb' was split off `pos' (tcp_fragment): it was sent with it. */
void tcp_tsorted_insert_after(tcp_conn_t *c, sk_buff_t *pos, sk_buff_t *skb)
{
	skb->ts_next = skb->ts_prev = NULL;
	if (!tcp_tsorted_on(c, pos))
		return;
	skb->ts_prev = pos;
	skb->ts_next = pos->ts_next;
	if (pos->ts_next)
		pos->ts_next->ts_prev = skb;
	else
		c->tsorted_tail = skb;
	pos->ts_next = skb;
}

/* The queues were emptied (purge, reset): the list goes with them. */
void tcp_tsorted_reset(tcp_conn_t *c)
{
	c->tsorted_head = c->tsorted_tail = NULL;
}

// ===========================================================================
// RACK.
// ===========================================================================
static int tcp_rack_sent_after(uint64_t t1, uint64_t t2, uint32_t seq1,
			       uint32_t seq2)
{
	return t1 > t2 || (t1 == t2 && after(seq1, seq2));
}

/* The reordering window (us). */
static uint32_t tcp_rack_reo_wnd(const tcp_conn_t *c)
{
	uint64_t w;

	if (!c->reord_seen) {
		/* No reordering seen: as aggressive as the three-duplicate
		 * rule while recovering or when that rule fires anyway. */
		if (c->ca_state >= TCP_CA_RECOVERY)
			return 0;
		if (c->sacked_out >= c->reordering)
			return 0;
	}
	if (tcp_min_rtt(c) == ~0u)
		return c->srtt_us;
	w = (uint64_t)(tcp_min_rtt(c) >> 2) * c->rack.reo_wnd_steps;
	return w < c->srtt_us ? (uint32_t)w : c->srtt_us;
}

/* How long `skb' may still be outstanding before it counts as lost (us;
 * <= 0: lost now). */
static int64_t tcp_rack_skb_timeout(const tcp_conn_t *c, const sk_buff_t *skb,
				    uint32_t reo_wnd)
{
	uint64_t now = tcp_now_us(), tx = TCP_SKB_CB(skb)->tx_us;
	int64_t elapsed = now > tx ? (int64_t)(now - tx) : 0;

	return (int64_t)c->rack.rtt_us + reo_wnd - elapsed;
}

/* Mark lost what was sent a reordering window before the most recently
 * delivered segment and is still not delivered.  *reo_timeout: the
 * longest wait left for the others (0: none).
 *
 * The time-ordered list holds exactly the candidates, oldest first: the
 * walk stops at the first one sent after the most recently delivered
 * segment (every one behind it was sent later still), and what it marks
 * lost leaves the list -- until it is sent again.  Each ACK so costs the
 * segments it decides about, not the flight. */
static void tcp_rack_detect_loss(tcp_conn_t *c, uint32_t *reo_timeout)
{
	uint32_t reo_wnd = tcp_rack_reo_wnd(c);
	sk_buff_t *skb, *next;

	*reo_timeout = 0;
	for (skb = c->tsorted_head; skb; skb = next) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		int64_t remaining;

		next = skb->ts_next;
		if (cb->sacked & TCPCB_SACKED_ACKED) {
			tcp_tsorted_del(c, skb); // (SACK takes it off itself)
			continue;
		}
		// marked lost and not resent yet: nothing new to learn
		if ((cb->sacked & TCPCB_LOST) && !(cb->sacked & TCPCB_SACKED_RETRANS))
			continue;
		if (!tcp_rack_sent_after(c->rack.mstamp, cb->tx_us,
					 c->rack.end_seq, cb->end_seq)) {
			/* Sent later than the latest delivery: so was
			 * every one behind it.  Sent at the same instant:
			 * the order is by sequence, the rest of that
			 * instant is looked at too. */
			if (cb->tx_us > c->rack.mstamp)
				break;
			continue;
		}
		remaining = tcp_rack_skb_timeout(c, skb, reo_wnd);
		if (remaining <= 0) {
			tcp_mark_skb_lost(c, skb);
			tcp_tsorted_del(c, skb);
		} else if ((uint64_t)remaining > *reo_timeout) {
			*reo_timeout = remaining > 0x7FFFFFFF ? 0x7FFFFFFFu :
								(uint32_t)remaining;
		}
	}
}

int tcp_rack_mark_lost(tcp_conn_t *c)
{
	uint32_t timeout;

	if (!c->rack.advanced)
		return 0;
	c->rack.advanced = 0; // no new information until it moves again
	tcp_rack_detect_loss(c, &timeout);
	if (timeout) {
		tcp_reset_xmit_timer(c, TCP_TIME_REO_TIMEOUT,
				     tcp_us_to_ticks(timeout) + 1);
		return 1;
	}
	return 0;
}

/* A segment was (S)ACKed: if it was sent later than RACK's most recently
 * delivered segment, it takes that place. */
void tcp_rack_advance(tcp_conn_t *c, uint8_t sacked, uint32_t end_seq,
		      uint64_t xmit_us)
{
	uint64_t now = tcp_now_us();
	uint32_t rtt_us = now > xmit_us ? (uint32_t)tcp_min32(
						  (uint32_t)(now - xmit_us),
						  TCP_RTO_MAX_US) :
					  0;

	/* A resent segment acknowledged faster than the minimum RTT: the
	 * ACK is for the original, whose time is unknown. */
	if (rtt_us < tcp_min_rtt(c) && (sacked & TCPCB_RETRANS))
		return;
	c->rack.advanced = 1;
	c->rack.rtt_us = rtt_us;
	if (tcp_rack_sent_after(xmit_us, c->rack.mstamp, end_seq,
				c->rack.end_seq)) {
		c->rack.mstamp = xmit_us;
		c->rack.end_seq = end_seq;
	}
}

/* The REO timer: the reordering window of the waiting segments ran out. */
void tcp_rack_reo_timeout(tcp_conn_t *c)
{
	uint32_t timeout, prior_inflight = tcp_packets_in_flight(c);
	uint32_t lost = c->lost;

	tcp_rack_detect_loss(c, &timeout);
	if (prior_inflight != tcp_packets_in_flight(c)) {
		if (c->ca_state != TCP_CA_RECOVERY) {
			tcp_enter_recovery(c, 0);
			tcp_cwnd_reduction(c, 1, (int)(c->lost - lost), 0);
		}
		tcp_xmit_retransmit_queue(c);
	}
	/* Segments still within their window are looked at again by the
	 * next ACK; the RTO (from the head's transmission) stays the
	 * backstop. */
	if (c->pending_timer != TCP_TIME_RETRANS)
		tcp_rearm_rto(c);
}

/* Adapt the reordering window: one step more per round with D-SACKs
 * (kept for TCP_RACK_RECOVERY_THRESH recoveries), back to one step
 * after that. */
void tcp_rack_update_reo_wnd(tcp_conn_t *c, const struct tcp_rate_sample *rs)
{
	if (!rs->prior_delivered)
		return;
	// A D-SACK within an RTT of the last adaptation is not new.
	if (before(rs->prior_delivered, c->rack.last_delivered))
		c->rack.dsack_seen = 0;
	if (c->rack.dsack_seen) {
		if (c->rack.reo_wnd_steps < 0xFF)
			c->rack.reo_wnd_steps++;
		c->rack.dsack_seen = 0;
		c->rack.last_delivered = c->delivered;
		c->rack.reo_wnd_persist = TCP_RACK_RECOVERY_THRESH;
	} else if (!c->rack.reo_wnd_persist) {
		c->rack.reo_wnd_steps = 1;
	}
}

/* NewReno (RFC 6582) for a peer without SACK: the head is lost after
 * `reordering' duplicate ACKs, and after every partial ACK in Recovery. */
static void tcp_newreno_mark_lost(tcp_conn_t *c, int snd_una_advanced)
{
	if ((c->ca_state < TCP_CA_RECOVERY && c->sacked_out >= c->reordering) ||
	    (c->ca_state == TCP_CA_RECOVERY && snd_una_advanced)) {
		sk_buff_t *skb = skb_peek(&c->rtx_queue);
		uint32_t mss = c->mss_cache ? c->mss_cache : TCP_MSS_DEFAULT;

		if (!skb || (TCP_SKB_CB(skb)->sacked & TCPCB_LOST))
			return;
		if (skb->len > mss && !(TCP_SKB_CB(skb)->tcp_flags & TCP_SYN))
			tcp_fragment(c, &c->rtx_queue, skb, mss);
		tcp_mark_skb_lost(c, skb);
	}
}

// ===========================================================================
// Window reduction: PRR (RFC 6937).
// ===========================================================================
void tcp_init_cwnd_reduction(tcp_conn_t *c)
{
	c->high_seq = c->snd_nxt;
	c->tlp_high_seq = 0;
	c->cwnd_cnt = 0;
	c->prior_cwnd = c->cwnd;
	c->prr_delivered = 0;
	c->prr_out = 0;
	c->ssthresh = c->ca_ops->ssthresh(c);
	if (c->ecn_flags & TCP_ECN_OK)
		c->ecn_flags |= TCP_ECN_QUEUE_CWR;
}

/* PRR: send in proportion to what is delivered so the window reaches
 * ssthresh by the end of the recovery -- proportionally while the flight
 * is above ssthresh, in slow start (SSRB: at most what was delivered plus
 * one per ACK that advanced snd_una without new losses) below it. */
void tcp_cwnd_reduction(tcp_conn_t *c, int newly_acked_sacked, int newly_lost,
			int flag)
{
	int64_t sndcnt, delta;

	if (newly_acked_sacked <= 0 || !c->prior_cwnd)
		return;
	delta = (int64_t)c->ssthresh - (int64_t)tcp_packets_in_flight(c);
	c->prr_delivered += (uint32_t)newly_acked_sacked;
	if (delta < 0) {
		uint64_t dividend = (uint64_t)c->ssthresh * c->prr_delivered +
				    c->prior_cwnd - 1;

		sndcnt = (int64_t)(dividend / c->prior_cwnd) - (int64_t)c->prr_out;
	} else {
		sndcnt = (int64_t)c->prr_delivered - (int64_t)c->prr_out;
		if (sndcnt < newly_acked_sacked)
			sndcnt = newly_acked_sacked;
		if ((flag & FLAG_SND_UNA_ADVANCED) && !newly_lost)
			sndcnt++;
		if (sndcnt > delta)
			sndcnt = delta;
	}
	// The first ACK of a recovery always lets the fast retransmit go.
	if (sndcnt < (c->prr_out ? 0 : 1))
		sndcnt = c->prr_out ? 0 : 1;
	if (sndcnt < 0)
		sndcnt = 0;
	c->cwnd = tcp_packets_in_flight(c) + (uint32_t)sndcnt;
	if (c->cwnd == 0)
		c->cwnd = 1;
}

void tcp_end_cwnd_reduction(tcp_conn_t *c)
{
	// cwnd goes to ssthresh at the end of CWR or Recovery (unless undone)
	if (c->ssthresh < TCP_INFINITE_SSTHRESH &&
	    (c->ca_state == TCP_CA_CWR || c->undo_marker)) {
		c->cwnd = tcp_max32(c->ssthresh, 1);
		c->snd_cwnd_stamp = tcp_now();
	}
	tcp_ca_event(c, CA_EVENT_COMPLETE_CWR);
}

/* CWR: an ECN echo (congestion without loss); no undo. */
void tcp_enter_cwr(tcp_conn_t *c)
{
	c->prior_ssthresh = 0;
	if (c->ca_state < TCP_CA_CWR) {
		c->undo_marker = 0;
		tcp_init_cwnd_reduction(c);
		tcp_set_ca_state(c, TCP_CA_CWR);
	}
}

static void tcp_try_keep_open(tcp_conn_t *c)
{
	uint8_t st = TCP_CA_OPEN;

	if (c->sacked_out + c->lost_out || tcp_any_retrans_done(c))
		st = TCP_CA_DISORDER;
	if (c->ca_state != st) {
		tcp_set_ca_state(c, st);
		c->high_seq = c->snd_nxt;
	}
}

static void tcp_try_to_open(tcp_conn_t *c, int flag)
{
	if (!tcp_any_retrans_done(c)) {
		c->retrans_stamp = 0;
		c->retrans_ts = 0;
	}
	if (flag & FLAG_ECE)
		tcp_enter_cwr(c);
	if (c->ca_state != TCP_CA_CWR)
		tcp_try_keep_open(c);
}

// ===========================================================================
// Undo.
// ===========================================================================
static void tcp_init_undo(tcp_conn_t *c)
{
	c->undo_marker = c->snd_una;
	// retransmissions still in flight may be D-SACKed later
	c->undo_retrans = (int32_t)c->retrans_out;
	if (c->tlp_high_seq && c->tlp_retrans)
		c->undo_retrans++;
	if (!c->undo_retrans)
		c->undo_retrans = -1; // 0 means "every retransmission D-SACKed"
}

/* Eifel (RFC 3522): the ACK echoes a timestamp from before the first
 * retransmission -- it answers the original, which was only delayed.  (An
 * echo of the retransmission's own TSval answers the retransmission.) */
static int tcp_packet_delayed(const tcp_conn_t *c)
{
	return c->ts_enabled && c->retrans_ts && c->rcv_tsecr &&
	       before(c->rcv_tsecr, c->retrans_ts);
}

static int tcp_may_undo(const tcp_conn_t *c)
{
	return c->undo_marker && (!c->undo_retrans || tcp_packet_delayed(c));
}

static void tcp_undo_cwnd_reduction(tcp_conn_t *c, int unmark_loss)
{
	if (unmark_loss) {
		for (sk_buff_t *skb = skb_peek(&c->rtx_queue); skb; skb = skb->next)
			TCP_SKB_CB(skb)->sacked &= (uint8_t)~TCPCB_LOST;
		c->lost_out = 0;
		tcp_clear_retrans_hints(c);
	}
	if (c->prior_ssthresh) {
		c->cwnd = c->ca_ops->undo_cwnd(c);
		if (c->prior_ssthresh > c->ssthresh) {
			c->ssthresh = c->prior_ssthresh;
			c->ecn_flags &= (uint8_t)~TCP_ECN_QUEUE_CWR;
		}
	}
	c->snd_cwnd_stamp = tcp_now();
	c->undo_marker = 0;
	c->rack.advanced = 1; // look at the losses again
}

/* A non-SACK sender holds its state until something above high_seq is
 * ACKed (RFC 6582: no false fast retransmits). */
static int tcp_is_non_sack_preventing_reopen(tcp_conn_t *c)
{
	if (c->snd_una == c->high_seq && tcp_is_reno(c)) {
		if (!tcp_any_retrans_done(c)) {
			c->retrans_stamp = 0;
			c->retrans_ts = 0;
		}
		return 1;
	}
	return 0;
}

// The recovery ended: undo it if it was spurious, then Open.
static int tcp_try_undo_recovery(tcp_conn_t *c)
{
	if (tcp_may_undo(c)) {
		tcp_undo_cwnd_reduction(c, 0);
		TCPX_INC(c->ca_state == TCP_CA_LOSS ? TCPX_LOSSUNDO :
						      TCPX_FULLUNDO);
	} else if (c->rack.reo_wnd_persist) {
		c->rack.reo_wnd_persist--;
	}
	if (tcp_is_non_sack_preventing_reopen(c))
		return 1;
	tcp_set_ca_state(c, TCP_CA_OPEN);
	c->is_sack_reneg = 0;
	return 0;
}

// Every retransmission was D-SACKed: undo.
static int tcp_try_undo_dsack(tcp_conn_t *c)
{
	if (c->undo_marker && !c->undo_retrans) {
		if (c->rack.reo_wnd_persist < TCP_RACK_RECOVERY_THRESH)
			c->rack.reo_wnd_persist++;
		tcp_undo_cwnd_reduction(c, 0);
		TCPX_INC(TCPX_DSACKUNDO);
		return 1;
	}
	return 0;
}

// Loss state: undo after a partial ACK, or for F-RTO.
static int tcp_try_undo_loss(tcp_conn_t *c, int frto_undo)
{
	if (frto_undo || tcp_may_undo(c)) {
		tcp_undo_cwnd_reduction(c, 1);
		TCPX_INC(TCPX_LOSSUNDO);
		if (frto_undo)
			TCPX_INC(TCPX_SPURIOUSRTOS);
		c->retransmits = 0;
		c->rto_backoff = 0;
		if (tcp_is_non_sack_preventing_reopen(c))
			return 1;
		if (frto_undo || tcp_is_sack(c)) {
			tcp_set_ca_state(c, TCP_CA_OPEN);
			c->is_sack_reneg = 0;
		}
		return 1;
	}
	return 0;
}

/* Recovery: a partial ACK whose echo predates the retransmissions -- the
 * hole was filled by the delayed original (reordering). */
static int tcp_try_undo_partial(tcp_conn_t *c, uint32_t prior_snd_una)
{
	if (c->undo_marker && tcp_packet_delayed(c)) {
		tcp_check_sack_reordering(c, prior_snd_una, 1);
		/* Retransmissions still out: keep clocking out new data but
		 * mark and resend no more. */
		if (c->retrans_out)
			return 1;
		if (!tcp_any_retrans_done(c)) {
			c->retrans_stamp = 0;
			c->retrans_ts = 0;
		}
		tcp_undo_cwnd_reduction(c, 1);
		TCPX_INC(TCPX_PARTIALRECOVERY);
		tcp_try_keep_open(c);
	}
	/* Otherwise a partial ACK: NewReno marks the next hole lost
	 * (tcp_newreno_mark_lost), RACK finds the losses by time. */
	return 0;
}

// ===========================================================================
// The state machine.
// ===========================================================================
static void tcp_enter_recovery(tcp_conn_t *c, int ece_ack)
{
	c->retrans_ts = 0; // the clock starts with this fast retransmit
	TCPX_INC(tcp_is_reno(c) ? TCPX_RENORECOVERY : TCPX_SACKRECOVERY);
	c->prior_ssthresh = 0;
	tcp_init_undo(c);
	if (!tcp_in_cwnd_reduction(c)) {
		if (!ece_ack)
			c->prior_ssthresh = tcp_current_ssthresh(c);
		tcp_init_cwnd_reduction(c);
	}
	tcp_set_ca_state(c, TCP_CA_RECOVERY);
}

/* SACK reneging: the ACK stopped at a segment the peer had SACKed.  The
 * RTO (shortened) sorts it out: it clears the SACK tags. */
static int tcp_check_sack_reneging(tcp_conn_t *c, int *ack_flag)
{
	if ((*ack_flag & FLAG_SACK_RENEGING) &&
	    (*ack_flag & FLAG_SND_UNA_ADVANCED)) {
		uint64_t delay = tcp_us_to_ticks(tcp_max32(c->srtt_us >> 1, 10000));

		tcp_reset_xmit_timer(c, TCP_TIME_RETRANS, delay);
		*ack_flag &= ~FLAG_SET_XMIT_TIMER;
		return 1;
	}
	return 0;
}

static void tcp_identify_packet_loss(tcp_conn_t *c, int *ack_flag)
{
	if (tcp_rtx_queue_empty(c))
		return;
	if (tcp_is_reno(c)) {
		tcp_newreno_mark_lost(c, *ack_flag & FLAG_SND_UNA_ADVANCED);
	} else {
		uint32_t prior_retrans = c->retrans_out;

		if (tcp_rack_mark_lost(c))
			*ack_flag &= ~FLAG_SET_XMIT_TIMER;
		if (prior_retrans > c->retrans_out)
			*ack_flag |= FLAG_LOST_RETRANS;
	}
}

static int tcp_time_to_recover(const tcp_conn_t *c)
{
	return c->lost_out != 0;
}

/* An ACK in the Loss state: Open again when the lost data is recovered
 * or the timeout proved spurious; otherwise more is resent. */
static void tcp_process_loss(tcp_conn_t *c, int flag, int num_dupack,
			     int *rexmit)
{
	int recovered = !before(c->snd_una, c->high_seq);

	if ((flag & FLAG_SND_UNA_ADVANCED) && tcp_try_undo_loss(c, 0))
		return;
	if (c->frto) {
		/* F-RTO (RFC 5682 3.1, SACK-enhanced).  Step 3b: data never
		 * retransmitted was (S)ACKed -- the timeout was spurious. */
		if ((flag & FLAG_ORIG_SACK_ACKED) && tcp_try_undo_loss(c, 1))
			return;
		if (after(c->snd_nxt, c->high_seq)) {
			if ((flag & FLAG_DATA_SACKED) || num_dupack)
				c->frto = 0; // step 3a: the loss was real
		} else if ((flag & FLAG_SND_UNA_ADVANCED) && !recovered) {
			c->high_seq = c->snd_nxt;
			/* Step 2b: send new data (after the window update of
			 * this ACK); without any, conventional recovery. */
			if (!tcp_write_queue_empty(c) &&
			    after(c->snd_una + c->snd_wnd, c->snd_nxt)) {
				*rexmit = REXMIT_NEW;
				return;
			}
			c->frto = 0;
		}
	}
	if (recovered) {
		// step 2a and the first part of 3a
		tcp_try_undo_recovery(c);
		return;
	}
	if (tcp_is_reno(c)) {
		/* A duplicate ACK after the new data of step 2b: it was
		 * delivered -- in flight falls, the clock goes on. */
		if (after(c->snd_nxt, c->high_seq) && num_dupack)
			tcp_add_reno_sack(c, num_dupack, flag & FLAG_ECE);
		else if (flag & FLAG_SND_UNA_ADVANCED)
			tcp_reset_reno_sack(c);
	}
	*rexmit = REXMIT_LOST;
}

/* Called for every "dubious" ACK (a duplicate, one with SACK or ECE news,
 * or any ACK while not Open).  Decides the state and what to resend. */
void tcp_fastretrans_alert(tcp_conn_t *c, uint32_t prior_snd_una,
			   int num_dupack, int *ack_flag, int *rexmit)
{
	int flag = *ack_flag, ece_ack = flag & FLAG_ECE;

	if (!c->packets_out && c->sacked_out)
		c->sacked_out = 0;
	// ECN: the reduction is real -- no undo.
	if (ece_ack)
		c->prior_ssthresh = 0;
	if (tcp_check_sack_reneging(c, ack_flag))
		return;
	// The end of an episode: high_seq acknowledged.
	if (c->ca_state == TCP_CA_OPEN) {
		c->retrans_stamp = 0;
		c->retrans_ts = 0;
	} else if (!before(c->snd_una, c->high_seq)) {
		switch (c->ca_state) {
		case TCP_CA_CWR:
			/* Held until something above high_seq is ACKed: our
			 * CWR has reached the peer then. */
			if (c->snd_una != c->high_seq) {
				tcp_end_cwnd_reduction(c);
				tcp_set_ca_state(c, TCP_CA_OPEN);
			}
			break;
		case TCP_CA_RECOVERY:
			if (tcp_is_reno(c))
				tcp_reset_reno_sack(c);
			if (tcp_try_undo_recovery(c))
				return;
			tcp_end_cwnd_reduction(c);
			break;
		default:
			break;
		}
	}
	switch (c->ca_state) {
	case TCP_CA_RECOVERY:
		if (!(flag & FLAG_SND_UNA_ADVANCED)) {
			if (tcp_is_reno(c))
				tcp_add_reno_sack(c, num_dupack, ece_ack);
		} else if (tcp_try_undo_partial(c, prior_snd_una)) {
			return;
		}
		if (tcp_try_undo_dsack(c))
			tcp_try_to_open(c, flag);
		tcp_identify_packet_loss(c, ack_flag);
		if (c->ca_state != TCP_CA_RECOVERY) {
			if (!tcp_time_to_recover(c))
				return;
			/* Undone, but losses are evident (reordering, then
			 * a real loss): a new recovery. */
			tcp_enter_recovery(c, ece_ack);
		}
		break;
	case TCP_CA_LOSS:
		tcp_process_loss(c, flag, num_dupack, rexmit);
		if (c->ca_state != TCP_CA_LOSS && c->rto_stamp) {
			c->total_rto_time += (uint32_t)tcp_ticks_to_ms(tcp_now() -
								      c->rto_stamp);
			c->rto_stamp = 0;
		}
		tcp_identify_packet_loss(c, ack_flag);
		if (!(c->ca_state == TCP_CA_OPEN ||
		      (*ack_flag & FLAG_LOST_RETRANS)))
			return;
		// undone, or retransmissions were lost:
		/* fall through */
	default:
		if (tcp_is_reno(c)) {
			if (flag & FLAG_SND_UNA_ADVANCED)
				tcp_reset_reno_sack(c);
			tcp_add_reno_sack(c, num_dupack, ece_ack);
		}
		if (c->ca_state <= TCP_CA_DISORDER)
			tcp_try_undo_dsack(c);
		tcp_identify_packet_loss(c, ack_flag);
		if (!tcp_time_to_recover(c)) {
			tcp_try_to_open(c, flag);
			return;
		}
		tcp_enter_recovery(c, ece_ack);
	}
	*rexmit = REXMIT_LOST;
}

// ===========================================================================
// Retransmission timeout.
// ===========================================================================
/* Everything outstanding is presumed lost -- except what was sent too
 * recently to tell; SACKs the peer reneged on are forgotten. */
static void tcp_timeout_mark_lost(tcp_conn_t *c)
{
	sk_buff_t *head = skb_peek(&c->rtx_queue), *skb;
	int is_reneg = head && (TCP_SKB_CB(head)->sacked & TCPCB_SACKED_ACKED);

	if (is_reneg) {
		TCPX_INC(TCPX_SACKRENEGING);
		c->sacked_out = 0;
		c->is_sack_reneg = 1;
	} else if (tcp_is_reno(c)) {
		tcp_reset_reno_sack(c);
	}
	for (skb = head; skb; skb = skb->next) {
		if (is_reneg)
			TCP_SKB_CB(skb)->sacked &= (uint8_t)~TCPCB_SACKED_ACKED;
		else if (skb != head && tcp_rack_skb_timeout(c, skb, 0) > 0)
			continue; // sent too recently to call it lost
		tcp_mark_skb_lost(c, skb);
	}
	tcp_clear_retrans_hints(c);
}

/* A retransmission timeout (RFC 6298 5.5, RFC 5681 3.1): ssthresh from
 * the first timeout of an episode, the window to what is in flight plus
 * one, the outstanding data presumed lost; F-RTO decides with the next
 * ACKs whether it was spurious. */
void tcp_enter_loss(tcp_conn_t *c)
{
	int new_recovery = c->ca_state < TCP_CA_RECOVERY;

	tcp_timeout_mark_lost(c);
	if (c->ca_state <= TCP_CA_DISORDER || !after(c->high_seq, c->snd_una) ||
	    (c->ca_state == TCP_CA_LOSS && !c->retransmits)) {
		c->prior_ssthresh = tcp_current_ssthresh(c);
		c->prior_cwnd = c->cwnd;
		c->ssthresh = c->ca_ops->ssthresh(c);
		tcp_ca_event(c, CA_EVENT_LOSS);
		if (c->ca_state <= TCP_CA_DISORDER)
			c->retrans_ts = 0;
		tcp_init_undo(c);
	}
	c->cwnd = tcp_packets_in_flight(c) + 1;
	c->cwnd_cnt = 0;
	c->snd_cwnd_stamp = tcp_now();
	/* A timeout after many duplicate ACKs: the reordering degree was
	 * overestimated. */
	if (c->ca_state <= TCP_CA_DISORDER && c->sacked_out >= TCP_REORDERING &&
	    c->reordering > TCP_REORDERING)
		c->reordering = TCP_REORDERING;
	if (c->ca_state != TCP_CA_LOSS) {
		c->total_rto_recoveries++;
		c->rto_stamp = tcp_now();
	}
	tcp_set_ca_state(c, TCP_CA_LOSS);
	c->high_seq = c->snd_nxt;
	c->tlp_high_seq = 0;
	if (c->ecn_flags & TCP_ECN_OK)
		c->ecn_flags |= TCP_ECN_QUEUE_CWR;
	/* F-RTO step 1: only when no recovery was underway (other than
	 * this timeout's own earlier expiries). */
	c->frto = (uint8_t)((new_recovery || c->retransmits) &&
			    !c->mtup_enabled);
}

// ===========================================================================
// Tail loss probe.
// ===========================================================================
/* The process-TLP-ACK rules (RFC 8985 7.4): the probe's ACK tells whether
 * the tail was lost (then the window is reduced once, CWR). */
void tcp_process_tlp_ack(tcp_conn_t *c, uint32_t ack, int flag)
{
	if (before(ack, c->tlp_high_seq))
		return;
	if (!c->tlp_retrans) {
		c->tlp_high_seq = 0; // the probe was new data: fine
	} else if (flag & FLAG_DSACK_TLP) {
		c->tlp_high_seq = 0; // the original and the probe arrived
	} else if (after(ack, c->tlp_high_seq)) {
		// the probe repaired a loss: reduce once
		tcp_init_cwnd_reduction(c);
		tcp_set_ca_state(c, TCP_CA_CWR);
		tcp_end_cwnd_reduction(c);
		tcp_try_keep_open(c);
		TCPX_INC(TCPX_LOSSPROBERECOVERY);
	} else if (!(flag & (FLAG_SND_UNA_ADVANCED | FLAG_NOT_DUP |
			      FLAG_DATA_SACKED))) {
		c->tlp_high_seq = 0; // a pure duplicate: both arrived
	}
}

/* How far in the future the RTO of the head lies (us; may be <= 0). */
static int64_t tcp_rto_delta_us(const tcp_conn_t *c)
{
	sk_buff_t *head = skb_peek(&c->rtx_queue);
	uint64_t rto = (uint64_t)tcp_rto_ticks(c) * timer_us_per_tick();
	uint64_t now = tcp_now_us();

	if (!head)
		return (int64_t)rto;
	return (int64_t)(TCP_SKB_CB(head)->tx_us + rto) - (int64_t)now;
}

/* Arm the TLP timer instead of the RTO: SACK peers, Open or CWR, data in
 * flight.  Returns 0 when no probe was scheduled. */
int tcp_schedule_loss_probe(tcp_conn_t *c, int advancing_rto)
{
	uint64_t timeout_us, timeout;
	int64_t rto_delta;

	if (!c->packets_out || !tcp_is_sack(c) ||
	    (c->ca_state != TCP_CA_OPEN && c->ca_state != TCP_CA_CWR) ||
	    !tcp_state_synchronized(c->state))
		return 0;
	if (c->srtt_us) {
		timeout_us = 2ULL * c->srtt_us;
		/* One segment out: the peer may hold its ACK for the
		 * delayed-ACK time. */
		if (c->packets_out == 1)
			timeout_us += TCP_RTO_MIN_US;
		else
			timeout_us += 2000; // a little slack
		timeout = tcp_us_to_ticks(timeout_us);
	} else {
		timeout = tcp_ms(TCP_TIMEOUT_INIT_MS);
	}
	// Never later than the RTO would fire.
	if (advancing_rto)
		rto_delta = (int64_t)(tcp_rto_ticks(c) * timer_us_per_tick());
	else
		rto_delta = tcp_rto_delta_us(c);
	if (rto_delta > 0 && (uint64_t)rto_delta < timeout * timer_us_per_tick())
		timeout = tcp_us_to_ticks((uint64_t)rto_delta);
	tcp_reset_xmit_timer(c, TCP_TIME_LOSS_PROBE, timeout);
	return 1;
}

/* The TLP timer fired: new data if the window takes some, else the last
 * segment again. */
void tcp_send_loss_probe(tcp_conn_t *c)
{
	uint32_t mss = tcp_current_mss(c);
	sk_buff_t *skb;

	if (c->tlp_high_seq)
		goto rearm; // one probe per episode
	c->tlp_retrans = 0;
	skb = tcp_send_head(c);
	if (skb && !after(TCP_SKB_CB(skb)->seq + tcp_min32(skb->len, mss),
			  c->snd_una + c->snd_wnd)) {
		uint32_t pcount = c->packets_out;

		tcp_write_xmit(c, mss, TCP_NAGLE_OFF, 2);
		if (c->packets_out > pcount)
			goto probe_sent;
		goto rearm;
	}
	skb = skb_peek_tail(&c->rtx_queue);
	if (!skb) {
		c->pending_timer = 0;
		return;
	}
	if (skb_cloned(skb))
		goto rearm; // its last transmission is still in this host
	if (skb->len > mss && !(TCP_SKB_CB(skb)->tcp_flags & TCP_SYN)) {
		// the last MSS of it
		uint32_t keep = skb->len - (skb->len - 1) % mss - 1;

		if (keep && keep < skb->len &&
		    tcp_fragment(c, &c->rtx_queue, skb, keep) < 0)
			goto rearm;
		skb = skb_peek_tail(&c->rtx_queue);
	}
	if (__tcp_retransmit_skb(c, skb))
		goto rearm;
	c->tlp_retrans = 1;
probe_sent:
	c->tlp_high_seq = c->snd_nxt;
	TCPX_INC(TCPX_LOSSPROBES);
	c->pending_timer = 0; // the RTO restarts from now
rearm:
	tcp_rearm_rto(c);
}

// ===========================================================================
// The retransmission timer.
// ===========================================================================
/* The RTO for the oldest outstanding segment -- after a TLP or REO
 * timer, what is left of it since the head was sent. */
void tcp_rearm_rto(tcp_conn_t *c)
{
	uint64_t rto;

	if (!c->packets_out) {
		if (c->pending_timer != TCP_TIME_PROBE0)
			tcp_clear_xmit_timer(c);
		return;
	}
	rto = tcp_rto_ticks(c);
	if (c->pending_timer == TCP_TIME_REO_TIMEOUT ||
	    c->pending_timer == TCP_TIME_LOSS_PROBE) {
		int64_t delta = tcp_rto_delta_us(c);

		rto = tcp_us_to_ticks(delta > 1 ? (uint64_t)delta : 1);
	}
	tcp_reset_xmit_timer(c, TCP_TIME_RETRANS, rto);
}

// A TLP if one may go, else the RTO.
void tcp_set_xmit_timer(tcp_conn_t *c)
{
	if (!tcp_schedule_loss_probe(c, 1))
		tcp_rearm_rto(c);
}

// What the ACK decided to resend.
void tcp_xmit_recovery(tcp_conn_t *c, int rexmit)
{
	if (rexmit == REXMIT_NONE || c->state == TCP_STATE_SYN_SENT)
		return;
	if (rexmit == REXMIT_NEW) {
		tcp_write_xmit(c, tcp_current_mss(c), TCP_NAGLE_OFF, 0);
		if (after(c->snd_nxt, c->high_seq))
			return;
		c->frto = 0;
	}
	tcp_xmit_retransmit_queue(c);
}

/* The path MTU shrank: what is outstanding and too large for it now was
 * dropped on the way.  It is marked lost and resent at once at the new
 * size -- without a congestion signal: the window is not cut (the units
 * changed, not the load), the state goes to Loss only to resend. */
void tcp_simple_retransmit(tcp_conn_t *c)
{
	uint32_t mss = tcp_current_mss(c);

	for (sk_buff_t *skb = skb_peek(&c->rtx_queue); skb; skb = skb->next)
		if (skb->len > mss)
			tcp_mark_skb_lost(c, skb);
	if (!c->lost_out)
		return;
	if (tcp_is_reno(c)) {
		// sacked_out + lost_out within packets_out
		uint32_t holes = tcp_min32(tcp_max32(c->lost_out, 1),
					   c->packets_out);

		if (c->sacked_out + holes > c->packets_out)
			c->sacked_out = c->packets_out - holes;
	}
	if (c->ca_state != TCP_CA_LOSS) {
		c->high_seq = c->snd_nxt;
		c->ssthresh = tcp_current_ssthresh(c);
		c->prior_ssthresh = 0;
		c->undo_marker = 0;
		tcp_set_ca_state(c, TCP_CA_LOSS);
	}
	tcp_xmit_retransmit_queue(c);
}
