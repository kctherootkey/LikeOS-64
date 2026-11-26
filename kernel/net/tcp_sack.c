// LikeOS TCP: the SACK scoreboard (RFC 2018, RFC 2883, RFC 6675)
//
// The peer's SACK blocks tag the segments of the retransmit queue they
// cover: TCPCB_SACKED_ACKED, counted in sacked_out.  A tagged segment is
// no longer in flight, and is not resent; one that was marked lost or
// retransmitted leaves those counts.  The blocks are checked first (inside
// snd_una..snd_nxt, not reversed, at most four), sorted, and compared with
// the blocks of the previous ACK (recv_sack_cache): only what they did not
// already cover is walked, from whichever end of the queue is closer.  A
// segment of several MSS (sent before the path MTU shrank) that a block
// covers in part is split at an MSS boundary so the covered part can be
// tagged.
//
// D-SACK (RFC 2883): a first block below the cumulative ACK, or inside the
// second block, reports data that arrived twice.  It is counted against
// the retransmissions of the current recovery (undo_retrans): when every
// retransmission proved unnecessary the recovery is undone
// (tcp_recovery.c), and RACK widens its reordering window.
//
// Reordering: a never-retransmitted segment SACKed below the highest
// SACKed one arrived out of order; the distance raises `reordering' and
// counts reord_seen.
//
// A peer without SACK gets the RFC 6582 emulation: every duplicate ACK
// counts as one SACKed segment in sacked_out (tcp_add_reno_sack).
//
// Under conn->lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>

uint32_t tcp_skb_pcount(const tcp_conn_t *c, const sk_buff_t *skb)
{
	uint32_t mss = c->mss_cache ? c->mss_cache : TCP_MSS_DEFAULT;

	return skb->len > mss ? (skb->len + mss - 1) / mss : 1;
}

uint32_t tcp_highest_sack_seq(const tcp_conn_t *c)
{
	if (!c->sacked_out)
		return c->snd_una;
	return c->fack_seq;
}

void tcp_count_delivered(tcp_conn_t *c, uint32_t delivered, int ece_ack)
{
	c->delivered += delivered;
	if (ece_ack) {
		c->delivered_ce += delivered;
		TCPX_ADD(TCPX_DELIVEREDCE, delivered);
	}
}

/* `low_seq' (never retransmitted) was delivered below the highest SACKed
 * sequence: the path reorders by at least that distance. */
void tcp_check_sack_reordering(tcp_conn_t *c, uint32_t low_seq, int ts)
{
	uint32_t mss = c->mss_cache, fack = tcp_highest_sack_seq(c), metric;

	if (!before(low_seq, fack))
		return;
	metric = fack - low_seq;
	if (mss && metric > c->reordering * mss)
		c->reordering = tcp_min32((metric + mss - 1) / mss,
					  TCP_MAX_REORDERING);
	c->reord_seen++;
	TCPX_INC(ts ? TCPX_TSREORDER : TCPX_SACKREORDER);
}

// ---------------------------------------------------------------------------
// Non-SACK peers: duplicate ACKs as SACKs (RFC 6582).
// ---------------------------------------------------------------------------
/* sacked_out + lost_out never exceed packets_out; returns 1 when
 * sacked_out had to be cut (more duplicate ACKs than holes: reordering). */
static int tcp_limit_reno_sacked(tcp_conn_t *c)
{
	uint32_t holes = tcp_max32(c->lost_out, 1);

	holes = tcp_min32(holes, c->packets_out);
	if (c->sacked_out + holes > c->packets_out) {
		c->sacked_out = c->packets_out - holes;
		return 1;
	}
	return 0;
}

static void tcp_check_reno_reordering(tcp_conn_t *c, uint32_t addend)
{
	if (!tcp_limit_reno_sacked(c))
		return;
	c->reordering = tcp_min32(c->packets_out + addend, TCP_MAX_REORDERING);
	c->reord_seen++;
	TCPX_INC(TCPX_RENOREORDER);
}

void tcp_add_reno_sack(tcp_conn_t *c, int num_dupack, int ece_ack)
{
	uint32_t prior;

	if (num_dupack <= 0)
		return;
	prior = c->sacked_out;
	c->sacked_out += (uint32_t)num_dupack;
	tcp_check_reno_reordering(c, 0);
	if (c->sacked_out > prior)
		tcp_count_delivered(c, c->sacked_out - prior, ece_ack);
}

void tcp_remove_reno_sacks(tcp_conn_t *c, uint32_t acked, int ece_ack)
{
	if (acked > 0) {
		// One ACK filled a hole; the rest ate duplicate ACKs.
		tcp_count_delivered(c, acked > c->sacked_out + 1 ?
					       acked - c->sacked_out :
					       1,
				    ece_ack);
		if (acked - 1 >= c->sacked_out)
			c->sacked_out = 0;
		else
			c->sacked_out -= acked - 1;
	}
	tcp_check_reno_reordering(c, acked);
}

void tcp_reset_reno_sack(tcp_conn_t *c)
{
	c->sacked_out = 0;
}

// ---------------------------------------------------------------------------
// D-SACK.
// ---------------------------------------------------------------------------
/* A D-SACK of [start, end): how many segments arrived twice; 0 when it
 * is dubious (larger than any window, or more duplicates than we ever
 * retransmitted). */
static uint32_t tcp_dsack_seen(tcp_conn_t *c, uint32_t start, uint32_t end,
			       struct tcp_sacktag_state *st)
{
	uint32_t len, dup_segs = 1;

	if (!before(start, end))
		return 0;
	len = end - start;
	if (len > c->max_window)
		return 0;
	if (c->mss_cache && len > c->mss_cache)
		dup_segs = (len + c->mss_cache - 1) / c->mss_cache;
	else if (c->tlp_high_seq && c->tlp_high_seq == end)
		st->flag |= FLAG_DSACK_TLP;
	c->dsack_dups += dup_segs;
	// Only what we retransmitted can arrive twice.
	if (c->dsack_dups > c->total_retrans)
		return 0;
	/* RACK widens its reordering window in rounds that saw D-SACKs
	 * (a spurious recovery); not for the probe's own duplicate. */
	if (!(st->flag & FLAG_DSACK_TLP))
		c->rack.dsack_seen = 1;
	st->flag |= FLAG_DSACKING_ACK;
	st->sack_delivered += dup_segs; // a spurious retransmission arrived
	return dup_segs;
}

static int tcp_check_dsack(tcp_conn_t *c, const struct tcp_sack_block *sp,
			   int num, uint32_t ack_seq, uint32_t prior_snd_una,
			   struct tcp_sacktag_state *st)
{
	uint32_t s0 = sp[0].start_seq, e0 = sp[0].end_seq, dup;

	if (before(s0, ack_seq)) {
		TCPX_INC(TCPX_DSACKRECV);
	} else if (num > 1) {
		if (after(e0, sp[1].end_seq) || before(s0, sp[1].start_seq))
			return 0;
		TCPX_INC(TCPX_DSACKOFORECV);
	} else {
		return 0;
	}
	dup = tcp_dsack_seen(c, s0, e0, st);
	if (!dup) {
		TCPX_INC(TCPX_DSACKIGNOREDDUBIOUS);
		return 0;
	}
	/* A D-SACK for data already ACKed (so no longer on the queue):
	 * counted here against the recovery's retransmissions. */
	if (c->undo_marker && c->undo_retrans > 0 && !after(e0, prior_snd_una) &&
	    after(e0, c->undo_marker)) {
		int32_t left = c->undo_retrans - (int32_t)dup;

		c->undo_retrans = left > 0 ? left : 0;
	}
	return 1;
}

/* RFC 2018 / RFC 2883 block validation: inside snd_una..snd_nxt, not
 * reversed; a D-SACK may lie below snd_una, but not below the start of
 * the recovery it can undo (nor wrap around). */
static int tcp_is_sackblock_valid(const tcp_conn_t *c, int is_dsack,
				  uint32_t start, uint32_t end)
{
	if (after(end, c->snd_nxt) || !before(start, end))
		return 0;
	if (!before(start, c->snd_nxt))
		return 0;
	if (after(start, c->snd_una))
		return 1;
	if (!is_dsack || !c->undo_marker)
		return 0;
	if (after(end, c->snd_una))
		return 0;
	if (!before(start, c->undo_marker))
		return 1;
	if (!after(end, c->undo_marker))
		return 0;
	return !before(start, end - c->max_window);
}

// ---------------------------------------------------------------------------
// Tagging.
// ---------------------------------------------------------------------------
/* Tag one segment (its bits are `sacked'); returns the new bits. */
static uint8_t tcp_sacktag_one(tcp_conn_t *c, struct tcp_sacktag_state *st,
			       uint8_t sacked, uint32_t start, uint32_t end,
			       int dup_sack, uint64_t xmit_us)
{
	// A D-SACK of a retransmitted segment: that retransmission was not
	// needed.
	if (dup_sack && (sacked & TCPCB_RETRANS)) {
		if (c->undo_marker && c->undo_retrans > 0 &&
		    after(end, c->undo_marker))
			c->undo_retrans--;
		if ((sacked & TCPCB_SACKED_ACKED) && before(start, st->reord))
			st->reord = start;
	}
	if (!after(end, c->snd_una))
		return sacked; // ACKed meanwhile: about to go
	if (!(sacked & TCPCB_SACKED_ACKED)) {
		tcp_rack_advance(c, sacked, end, xmit_us);
		if (sacked & TCPCB_SACKED_RETRANS) {
			/* Not marked lost: the retransmission is still believed
			 * in flight, its bit stays. */
			if (sacked & TCPCB_LOST) {
				sacked &= (uint8_t)~(TCPCB_LOST | TCPCB_SACKED_RETRANS);
				if (c->lost_out)
					c->lost_out--;
				if (c->retrans_out)
					c->retrans_out--;
			}
		} else {
			if (!(sacked & TCPCB_RETRANS)) {
				/* Never resent, below the highest SACK: it was
				 * reordered. */
				if (before(start, tcp_highest_sack_seq(c)) &&
				    before(start, st->reord))
					st->reord = start;
				if (!after(end, c->high_seq))
					st->flag |= FLAG_ORIG_SACK_ACKED;
				if (!st->first_sackt)
					st->first_sackt = xmit_us;
				st->last_sackt = xmit_us;
			}
			if (sacked & TCPCB_LOST) {
				sacked &= (uint8_t)~TCPCB_LOST;
				if (c->lost_out)
					c->lost_out--;
			}
		}
		sacked |= TCPCB_SACKED_ACKED;
		st->flag |= FLAG_DATA_SACKED;
		c->sacked_out++;
		st->sack_delivered++;
		if (c->sacked_out == 1 || after(end, c->fack_seq))
			c->fack_seq = end;
	}
	if (dup_sack && (sacked & TCPCB_SACKED_RETRANS)) {
		sacked &= (uint8_t)~TCPCB_SACKED_RETRANS;
		if (c->retrans_out)
			c->retrans_out--;
	}
	return sacked;
}

/* Does [start, end) cover `skb'?  A segment of several MSS covered in
 * part is split at an MSS boundary first (its covered part is then `skb'
 * itself, or the new segment behind it).  1 covered, 0 not, < 0 when it
 * could not be split. */
static int tcp_match_skb_to_sack(tcp_conn_t *c, sk_buff_t *skb, uint32_t start,
				 uint32_t end)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint32_t mss = c->mss_cache ? c->mss_cache : TCP_MSS_DEFAULT;
	int in_sack = !after(start, cb->seq) && !before(end, cb->end_seq);
	uint32_t pkt_len;

	if (in_sack || tcp_skb_pcount(c, skb) <= 1 || !after(cb->end_seq, start) ||
	    (cb->tcp_flags & TCP_SYN))
		return in_sack;
	in_sack = !after(start, cb->seq);
	if (!in_sack) {
		// the block starts inside: split off the uncovered head
		pkt_len = start - cb->seq;
		if (pkt_len < mss)
			pkt_len = mss;
	} else {
		// the block ends inside: split off the uncovered tail
		pkt_len = end - cb->seq;
		if (pkt_len < mss)
			return -1;
	}
	if (pkt_len > mss) {
		uint32_t n = (pkt_len / mss) * mss;

		if (!in_sack && n < pkt_len)
			n += mss;
		pkt_len = n;
	}
	if (pkt_len >= skb->len && !in_sack)
		return 0;
	if (pkt_len >= skb->len)
		return in_sack;
	if (tcp_fragment(c, &c->rtx_queue, skb, pkt_len) < 0)
		return -1;
	return in_sack;
}

/* Tag the segments of [start, end), from `skb' (the first that may
 * overlap it).  Returns where a later (higher) block may continue. */
static sk_buff_t *tcp_sacktag_walk(tcp_conn_t *c, sk_buff_t *skb,
				   struct tcp_sacktag_state *st, uint32_t start,
				   uint32_t end, int dup_sack)
{
	while (skb) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
		int in;

		if (!before(cb->seq, end))
			break;
		in = tcp_match_skb_to_sack(c, skb, start, end);
		if (in < 0)
			break;
		if (in) {
			uint8_t prior = cb->sacked;

			cb->sacked = tcp_sacktag_one(c, st, cb->sacked, cb->seq,
						     cb->end_seq, dup_sack,
						     cb->tx_us);
			if (!(prior & TCPCB_SACKED_ACKED) &&
			    (cb->sacked & TCPCB_SACKED_ACKED)) {
				tcp_rate_skb_delivered(c, skb, st->rate);
				tcp_tsorted_del(c, skb); // delivered
			}
		}
		skb = skb->next;
	}
	return skb;
}

// ---------------------------------------------------------------------------
// The sequence index of the retransmit queue.
//
// A SACK block can name any part of the flight; finding its first segment
// by walking the queue from the nearer end costs up to half the flight per
// block -- a peer that SACKs blocks in the middle of a large flight on
// every ACK would make each ACK that expensive.  The index records every
// 2^shift-th segment sent (the stride doubles when the index fills, every
// other entry going), so a lookup starts at most about one stride before
// its segment.  Entries below snd_una are stale and dropped: an
// acknowledged segment is the only one that is ever freed (apart from the
// whole queue at once, which resets the index).
// ---------------------------------------------------------------------------
static void tcp_rtx_idx_prune(tcp_conn_t *c)
{
	uint32_t i = 0, n = c->rtx_idx_n;

	while (i < n && before(c->rtx_idx[i].seq, c->snd_una))
		i++;
	if (!i)
		return;
	for (uint32_t j = i; j < n; j++)
		c->rtx_idx[j - i] = c->rtx_idx[j];
	c->rtx_idx_n = (uint8_t)(n - i);
	// a shrunken flight: a finer index again from here on
	if (c->rtx_idx_n < TCP_RTX_IDX_N / 4 && c->rtx_idx_shift)
		c->rtx_idx_shift--;
}

void tcp_rtx_idx_reset(tcp_conn_t *c)
{
	c->rtx_idx_n = 0;
	c->rtx_idx_shift = 0;
	c->rtx_idx_count = 0;
}

void tcp_rtx_idx_add(tcp_conn_t *c, sk_buff_t *skb)
{
	uint32_t n;

	if (++c->rtx_idx_count < (1u << c->rtx_idx_shift))
		return;
	c->rtx_idx_count = 0;
	tcp_rtx_idx_prune(c);
	n = c->rtx_idx_n;
	if (n == TCP_RTX_IDX_N) {
		for (uint32_t i = 0; i < n / 2; i++)
			c->rtx_idx[i] = c->rtx_idx[2 * i];
		n /= 2;
		if (c->rtx_idx_shift < 15)
			c->rtx_idx_shift++;
	}
	c->rtx_idx[n].seq = TCP_SKB_CB(skb)->seq;
	c->rtx_idx[n].skb = skb;
	c->rtx_idx_n = (uint8_t)(n + 1);
}

/* The indexed segment closest below `seq' (valid), or NULL. */
static sk_buff_t *tcp_rtx_idx_find(tcp_conn_t *c, uint32_t seq)
{
	sk_buff_t *best = NULL;

	tcp_rtx_idx_prune(c);
	for (uint32_t i = 0; i < c->rtx_idx_n; i++) {
		if (after(c->rtx_idx[i].seq, seq))
			break;
		best = c->rtx_idx[i].skb;
	}
	return best;
}

/* The first segment of the retransmit queue that may overlap `seq': the
 * walk starts from whichever is nearest in sequence space -- `hint' (the
 * end of the previous block's walk), the index, the head, or the tail
 * (walking back). */
static sk_buff_t *tcp_sacktag_skip(tcp_conn_t *c, sk_buff_t *hint, uint32_t seq)
{
	sk_buff_t *skb = skb_peek(&c->rtx_queue), *idx;
	uint32_t best = skb ? seq - TCP_SKB_CB(skb)->seq : 0;

	if (!skb)
		return NULL;
	/* Blocks may overlap (a D-SACK inside the next block): a hint
	 * already past `seq' is no use. */
	if (hint && !after(TCP_SKB_CB(hint)->seq, seq) &&
	    seq - TCP_SKB_CB(hint)->seq < best) {
		skb = hint;
		best = seq - TCP_SKB_CB(hint)->seq;
	}
	idx = tcp_rtx_idx_find(c, seq);
	if (idx && seq - TCP_SKB_CB(idx)->seq < best) {
		skb = idx;
		best = seq - TCP_SKB_CB(idx)->seq;
	}
	if (after(seq, c->snd_una) && c->snd_nxt - seq < best) {
		// closer to the tail: walk back to the segment holding seq
		skb = skb_peek_tail(&c->rtx_queue);
		while (skb && skb->prev && after(TCP_SKB_CB(skb)->seq, seq))
			skb = skb->prev;
		return skb;
	}
	while (skb && !after(TCP_SKB_CB(skb)->end_seq, seq))
		skb = skb->next;
	return skb;
}

/* Tag [start, end) except what the cached blocks of the previous ACK
 * covered (already tagged). */
static sk_buff_t *tcp_sacktag_range(tcp_conn_t *c, sk_buff_t *skb,
				    struct tcp_sacktag_state *st, uint32_t start,
				    uint32_t end, int use_cache)
{
	struct tcp_sack_block parts[TCP_MAX_SACK_BLOCKS + 1];
	int nparts = 1;

	parts[0].start_seq = start;
	parts[0].end_seq = end;
	if (use_cache) {
		// subtract every cached block from the parts
		for (int k = 0; k < TCP_MAX_SACK_BLOCKS; k++) {
			const struct tcp_sack_block *cb = &c->recv_sack_cache[k];
			struct tcp_sack_block out[TCP_MAX_SACK_BLOCKS + 1];
			int n = 0;

			if (!before(cb->start_seq, cb->end_seq))
				continue; // empty
			for (int i = 0; i < nparts; i++) {
				struct tcp_sack_block p = parts[i];

				if (!before(cb->start_seq, p.end_seq) ||
				    !after(cb->end_seq, p.start_seq)) {
					if (n < TCP_MAX_SACK_BLOCKS + 1)
						out[n++] = p; // no overlap
					continue;
				}
				if (before(p.start_seq, cb->start_seq) &&
				    n < TCP_MAX_SACK_BLOCKS + 1) {
					out[n].start_seq = p.start_seq;
					out[n].end_seq = cb->start_seq;
					n++;
				}
				if (after(p.end_seq, cb->end_seq) &&
				    n < TCP_MAX_SACK_BLOCKS + 1) {
					out[n].start_seq = cb->end_seq;
					out[n].end_seq = p.end_seq;
					n++;
				}
			}
			for (int i = 0; i < n; i++)
				parts[i] = out[i];
			nparts = n;
			if (!nparts)
				return skb;
		}
	}
	for (int i = 0; i < nparts; i++) {
		skb = tcp_sacktag_skip(c, skb, parts[i].start_seq);
		skb = tcp_sacktag_walk(c, skb, st, parts[i].start_seq,
				       parts[i].end_seq, 0);
	}
	return skb;
}

int tcp_sacktag_write_queue(tcp_conn_t *c, const tcp_parsed_opts_t *opt,
			    uint32_t ack_seq, uint32_t prior_snd_una,
			    struct tcp_sacktag_state *st)
{
	struct tcp_sack_block sp[TCP_MAX_SACK_BLOCKS];
	int num = opt->sack_count, used = 0, first_sack_index = 0, found_dup;
	int use_cache;
	sk_buff_t *skb = NULL;

	if (num > TCP_MAX_SACK_BLOCKS)
		num = TCP_MAX_SACK_BLOCKS;
	if (num <= 0)
		return 0;
	st->reord = c->snd_nxt;
	if (!c->sacked_out)
		c->fack_seq = c->snd_una;
	found_dup = tcp_check_dsack(c, opt->sack, num, ack_seq, prior_snd_una, st);
	/* Too old to say anything, but a merely reordered ACK still may. */
	if (before(ack_seq, prior_snd_una - c->max_window))
		return 0;
	if (!c->packets_out)
		return st->flag;
	for (int i = 0; i < num; i++) {
		int dup_sack = !i && found_dup;

		sp[used] = opt->sack[i];
		if (!tcp_is_sackblock_valid(c, dup_sack, sp[used].start_seq,
					    sp[used].end_seq)) {
			if (dup_sack) {
				TCPX_INC(c->undo_marker ? TCPX_DSACKIGNOREDOLD :
							  TCPX_DSACKIGNOREDNOUNDO);
			} else if (ack_seq == c->snd_una ||
				   after(sp[used].end_seq, c->snd_una)) {
				// (an old block of a reordered ACK is no news)
				TCPX_INC(TCPX_SACKDISCARD);
			}
			if (i == 0)
				first_sack_index = -1;
			continue;
		}
		if (!after(sp[used].end_seq, prior_snd_una)) {
			if (i == 0)
				first_sack_index = -1;
			continue;
		}
		used++;
	}
	// In sequence order, so the queue is walked once.
	for (int i = used - 1; i > 0; i--) {
		for (int j = 0; j < i; j++) {
			if (after(sp[j].start_seq, sp[j + 1].start_seq)) {
				struct tcp_sack_block t = sp[j];

				sp[j] = sp[j + 1];
				sp[j + 1] = t;
				if (j == first_sack_index)
					first_sack_index = j + 1;
				else if (j + 1 == first_sack_index)
					first_sack_index = j;
			}
		}
	}
	/* The cache is what the previous ACK's blocks tagged; with nothing
	 * SACKed now (a timeout or reneging cleared the tags) it is stale. */
	use_cache = c->sacked_out != 0;
	for (int i = 0; i < used; i++) {
		int dup_sack = found_dup && i == first_sack_index;

		if (dup_sack) {
			skb = tcp_sacktag_skip(c, skb, sp[i].start_seq);
			skb = tcp_sacktag_walk(c, skb, st, sp[i].start_seq,
					       sp[i].end_seq, 1);
		} else {
			skb = tcp_sacktag_range(c, skb, st, sp[i].start_seq,
						sp[i].end_seq, use_cache);
		}
		// The next block is higher: it continues from here.
	}
	for (int i = 0; i < TCP_MAX_SACK_BLOCKS; i++) {
		if (i < used) {
			c->recv_sack_cache[i] = sp[i];
		} else {
			c->recv_sack_cache[i].start_seq = 0;
			c->recv_sack_cache[i].end_seq = 0;
		}
	}
	if (c->ca_state != TCP_CA_LOSS || c->undo_marker)
		tcp_check_sack_reordering(c, st->reord, 0);
	return st->flag;
}
