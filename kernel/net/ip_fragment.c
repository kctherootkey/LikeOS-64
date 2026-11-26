// LikeOS — IPv4 fragmentation and reassembly
//
// Reassembly (ip_defrag): fragments of one datagram collect in a queue
// (inet_frag.c) sorted by offset.  The rules:
//   * the last fragment fixes the length; a later fragment beyond it, or a
//     second "last" one disagreeing with it, invalidates the datagram;
//   * every fragment but the last carries a multiple of 8 bytes (excess is
//     cut off), and an empty one invalidates the datagram;
//   * a fragment that only repeats bytes already held (a duplicate, also a
//     smaller piece of a stretch already held) is dropped and changes
//     nothing; one that overlaps held data partially invalidates the whole
//     datagram (RFC 5722 for the reasoning) -- the teardrop family of
//     attacks relies on overlaps;
//   * the datagram may not exceed 65535 bytes, a queue may hold at most
//     IPFRAG_MAX_FRAGS fragments;
//   * a datagram not complete IPFRAG_TIME_MS after its first fragment is
//     dropped, with an ICMP time exceeded (reassembly) to the sender when
//     fragment 0 had arrived and the datagram was addressed to us;
//   * every queue's memory counts against 4 MB: above it the oldest queues
//     are dropped until less than 3 MB are held.
// The complete datagram is copied into one new buffer (its first
// fragment's header, total length and fragment field fixed up) and
// continues through ip_local_deliver.
//
// Fragmentation (ip_do_fragment): a datagram larger than the MTU is split
// into pieces of (MTU - header) rounded down to 8 bytes; every piece
// carries the header, the first one with all options, the others only
// those marked "copied".
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/inet_frag.h>
#include <kernel/net/stats.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

#define IPFRAG_TIME_MS 30000u
#define IPFRAG_HIGH_THRESH (4u * 1024u * 1024u)
#define IPFRAG_LOW_THRESH (3u * 1024u * 1024u)
#define IPFRAG_POOL 1024
#define IPFRAG_MAX_FRAGS 1024

/* Where a queued fragment's payload lies in the datagram (skb->cb, after
 * the IPCB, which the first fragment keeps for the reassembled whole). */
struct ipfrag_cb {
	uint32_t offset;
	uint32_t end;
};
#define FRAG_CB(skb) ((struct ipfrag_cb *)((skb)->cb + IPCB_TAIL_OFF))
BUILD_BUG_ON(IPCB_TAIL_OFF + sizeof(struct ipfrag_cb) > SKB_CB_SIZE);

static struct inet_frags ip4_frags;
static struct inet_frag_queue ip4_frag_pool[IPFRAG_POOL];
static uint64_t ip4_frag_timeouts;

static void ip_expire(struct inet_frag_queue *q);

void ipfrag_init(void)
{
	inet_frags_init(&ip4_frags, ip4_frag_pool, IPFRAG_POOL, IPFRAG_TIME_MS,
			IPFRAG_HIGH_THRESH, IPFRAG_LOW_THRESH, ip_expire);
}

void ipfrag_get_stats(struct ipfrag_stats *st)
{
	st->queues = ip4_frags.nqueues;
	st->mem = inet_frag_mem(&ip4_frags);
	st->high_thresh = ip4_frags.high_thresh;
	st->low_thresh = ip4_frags.low_thresh;
	st->evicted = __atomic_load_n(&ip4_frags.evicted, __ATOMIC_RELAXED);
	st->timeouts = __atomic_load_n(&ip4_frag_timeouts, __ATOMIC_RELAXED);
}

void ipfrag_set_thresh(uint64_t high, uint64_t low)
{
	ip4_frags.high_thresh = high ? high : IPFRAG_HIGH_THRESH;
	ip4_frags.low_thresh = low ? low : IPFRAG_LOW_THRESH;
}

/* The timer of an incomplete datagram fired (softirq; q held). */
static void ip_expire(struct inet_frag_queue *q)
{
	sk_buff_t *head = NULL;
	uint64_t fl;

	spin_lock_irqsave(&q->lock, &fl);
	if (q->flags & INET_FRAG_COMPLETE) {
		spin_unlock_irqrestore(&q->lock, fl);
		return;
	}
	inet_frag_kill(q);
	NET_STATS_INC(NET_MIB_IP_REASMTIMEOUT);
	NET_STATS_INC(NET_MIB_IP_REASMFAILS);
	__atomic_fetch_add(&ip4_frag_timeouts, 1, __ATOMIC_RELAXED);
	/* Only with fragment 0 is there a header (and the first bytes of the
	 * payload) to quote, and only a datagram for us is ours to report. */
	if ((q->flags & INET_FRAG_FIRST_IN) && q->fragments &&
	    FRAG_CB(q->fragments)->offset == 0 &&
	    IPCB(q->fragments)->rt_type == RTN_LOCAL)
		head = skb_get(q->fragments);
	spin_unlock_irqrestore(&q->lock, fl);
	if (head) {
		icmp_send(head, ICMP_TIME_EXCEEDED, ICMP_EXC_FRAGTIME, 0);
		skb_put(head);
	}
}

#define FRAG_OK 0
#define FRAG_DUP 1
#define FRAG_OVERLAP 2

/* Insert [offset, end) into the sorted fragment list (q->lock held). */
static int frag_insert(struct inet_frag_queue *q, sk_buff_t *skb,
		       uint32_t offset, uint32_t end)
{
	sk_buff_t *prev = NULL, *cur;
	uint32_t covered;

	FRAG_CB(skb)->offset = offset;
	FRAG_CB(skb)->end = end;
	skb->next = skb->prev = NULL;
	// the common case: in order, after everything held
	if (!q->last || FRAG_CB(q->last)->end <= offset) {
		if (q->last)
			q->last->next = skb;
		else
			q->fragments = skb;
		q->last = skb;
		return FRAG_OK;
	}
	for (cur = q->fragments; cur; prev = cur, cur = cur->next) {
		if (FRAG_CB(cur)->end <= offset)
			continue;
		if (FRAG_CB(cur)->offset >= end)
			break; // goes in front of cur, no overlap
		/* Overlaps cur.  Duplicate when held fragments cover
		 * [offset, end) without a gap from here on. */
		covered = offset;
		for (sk_buff_t *c = cur; c && FRAG_CB(c)->offset <= covered;
		     c = c->next) {
			if (FRAG_CB(c)->end > covered)
				covered = FRAG_CB(c)->end;
			if (covered >= end)
				return FRAG_DUP;
		}
		return FRAG_OVERLAP;
	}
	skb->next = cur;
	if (prev)
		prev->next = skb;
	else
		q->fragments = skb;
	if (!cur)
		q->last = skb;
	return FRAG_OK;
}

/* What a completed queue hands to ip_frag_build(): its fragments (taken
 * out of the queue, ours now) and the datagram's size. */
struct ipfrag_done {
	sk_buff_t *list;
	uint32_t len; // payload bytes
	uint16_t max_size;
};

static void ipfrag_free_list(sk_buff_t *f)
{
	while (f) {
		sk_buff_t *next = f->next;

		f->next = f->prev = NULL;
		skb_put(f);
		f = next;
	}
}

/* Build the whole datagram from a completed queue's fragments, with no
 * lock held (a 64 KB buffer may have to come from the slab).  Frees the
 * fragments; NULL on failure. */
static sk_buff_t *ip_frag_build(struct ipfrag_done *d)
{
	sk_buff_t *head = d->list, *nskb = NULL;
	uint32_t ihl, total;
	uint8_t *p;

	BUG_ON(!head || FRAG_CB(head)->offset != 0);
	ihl = IPCB(head)->ihl;
	total = ihl + d->len;
	if (total > IP_MAX_PACKET)
		goto fail;
	nskb = skb_alloc(total);
	if (!nskb)
		goto fail;
	p = skb_append(nskb, total);
	mm_memcpy(p, skb_network_header(head), ihl);
	for (sk_buff_t *f = head; f; f = f->next) {
		uint32_t off = FRAG_CB(f)->offset;
		uint32_t n = FRAG_CB(f)->end - off;

		BUG_ON(off + n > d->len || n > f->len);
		mm_memcpy(p + ihl + off, f->data, n);
	}
	p[2] = (uint8_t)(total >> 8);
	p[3] = (uint8_t)total;
	p[6] = p[7] = 0; // no longer a fragment
	p[10] = p[11] = 0;
	{
		uint16_t c = ipv4_checksum(p, (uint16_t)ihl);

		mm_memcpy(p + 10, &c, 2);
	}
	nskb->dev = head->dev;
	nskb->pkt_type = head->pkt_type;
	nskb->protocol = head->protocol;
	nskb->tstamp_ns = head->tstamp_ns;
	nskb->ip_summed = CHECKSUM_NONE;
	skb_reset_network_header(nskb);
	skb_set_transport_header(nskb, (int)ihl);
	mm_memcpy(nskb->cb, head->cb, sizeof(struct inet_skb_parm));
	IPCB(nskb)->flags |= IPSKB_REASSEMBLED;
	IPCB(nskb)->frag_max_size = d->max_size;
	IPCB(nskb)->tot_len = (uint16_t)total;
	nskb->src_ip = head->src_ip;
	nskb->dst_ip = head->dst_ip;
	nskb->ip_proto = head->ip_proto;
	nskb->rx_ttl = head->rx_ttl;
	NET_STATS_INC(NET_MIB_IP_REASMOKS);
	ipfrag_free_list(head);
	return nskb;
fail:
	NET_STATS_INC(NET_MIB_IP_REASMFAILS);
	ipfrag_free_list(head);
	return NULL;
}

/* Add one fragment to q (q->lock held).  Consumes skb.  Returns 1 when
 * this completed the datagram: the queue is killed and *done holds its
 * fragments. */
static int ip_frag_queue(struct inet_frag_queue *q, sk_buff_t *skb,
			 struct ipfrag_done *done)
{
	struct inet_skb_parm *cb = IPCB(skb);
	const uint8_t *iph = skb->data;
	uint16_t frag = (uint16_t)((iph[6] << 8) | iph[7]);
	uint32_t offset = (uint32_t)(frag & IP_OFFSET) << 3;
	uint32_t ihl = cb->ihl;
	uint32_t end;
	int rc;

	if (q->flags & INET_FRAG_COMPLETE) {
		NET_STATS_INC(NET_MIB_IP_REASMFAILS);
		skb_put(skb);
		return 0;
	}
	BUG_ON(skb->len != cb->tot_len || ihl > skb->len);
	end = offset + (skb->len - ihl);
	if (!(frag & IP_MF)) {
		// the last fragment: the length is known now
		if (end < q->len ||
		    ((q->flags & INET_FRAG_LAST_IN) && end != q->len))
			goto discard_qp;
		q->flags |= INET_FRAG_LAST_IN;
		q->len = end;
	} else {
		if (end & 7) {
			end &= ~7u; // not the last: whole 8-byte blocks
			skb->ip_summed = CHECKSUM_NONE;
		}
		if (end > q->len) {
			if (q->flags & INET_FRAG_LAST_IN)
				goto discard_qp; // beyond the end
			q->len = end;
		}
	}
	if (end <= offset)
		goto discard_qp;
	if (end > IP_MAX_PACKET)
		goto discard_qp; // can never fit a datagram
	skb_pull(skb, ihl);
	skb_trim(skb, end - offset);
	skb_set_transport_header(skb, 0);

	rc = frag_insert(q, skb, offset, end);
	if (rc == FRAG_DUP) {
		NET_STATS_INC(NET_MIB_IP_REASMDUPS);
		skb_put(skb);
		return 0;
	}
	if (rc == FRAG_OVERLAP) {
		NET_STATS_INC(NET_MIB_IP_REASMOVERLAPS);
		goto discard_qp;
	}
	q->nfrags++;
	q->meat += end - offset;
	inet_frag_mem_add(q, skb->truesize);
	if (offset == 0)
		q->flags |= INET_FRAG_FIRST_IN;
	if (cb->tot_len > q->max_size)
		q->max_size = cb->tot_len;
	if (q->nfrags > IPFRAG_MAX_FRAGS) {
		inet_frag_kill(q);
		NET_STATS_INC(NET_MIB_IP_REASMFAILS);
		return 0;
	}
	inet_frag_touch(q);
	if ((q->flags & (INET_FRAG_FIRST_IN | INET_FRAG_LAST_IN)) ==
		    (INET_FRAG_FIRST_IN | INET_FRAG_LAST_IN) &&
	    q->meat == q->len) {
		inet_frag_kill(q);
		done->len = q->len;
		done->max_size = q->max_size;
		done->list = inet_frag_steal(q);
		return 1;
	}
	return 0;

discard_qp:
	inet_frag_kill(q);
	NET_STATS_INC(NET_MIB_IP_REASMFAILS);
	skb_put(skb);
	return 0;
}

sk_buff_t *ip_defrag(sk_buff_t *skb)
{
	struct inet_skb_parm *cb = IPCB(skb);
	struct inet_frag_queue *q;
	/* filled by ip_frag_queue() when it completes the datagram; zeroed
	 * so the compiler can see it is never read otherwise */
	struct ipfrag_done done = { 0 };
	struct frag_key key;
	int complete;
	uint64_t fl;

	NET_STATS_INC(NET_MIB_IP_REASMREQDS);
	if (inet_frag_mem(&ip4_frags) > ip4_frags.high_thresh)
		inet_frag_evict(&ip4_frags);
	key.saddr = cb->saddr;
	key.daddr = cb->daddr;
	key.id = (uint16_t)((skb->data[4] << 8) | skb->data[5]);
	key.proto = cb->proto;
	key._pad = 0;
	q = inet_frag_find(&ip4_frags, &key);
	if (!q) {
		NET_STATS_INC(NET_MIB_IP_REASMFAILS);
		skb_put(skb);
		return NULL;
	}
	spin_lock_irqsave(&q->lock, &fl);
	complete = ip_frag_queue(q, skb, &done);
	spin_unlock_irqrestore(&q->lock, fl);
	inet_frag_put(q);
	return complete ? ip_frag_build(&done) : NULL;
}

// ---- output ----

int ip_do_fragment(sk_buff_t *skb, uint32_t mtu,
		   int (*out)(void *ctx, sk_buff_t *frag), void *ctx)
{
	uint8_t hdr[IP_HDR_MAX];
	uint32_t hlen, total, left, ptr, offset, flen;
	uint16_t frag_field;
	int not_last, err = 0, first = 1;

	if (skb->len < IP_HDR_MIN)
		goto fail;
	hlen = (uint32_t)(skb->data[0] & 0x0F) * 4;
	total = (uint32_t)((skb->data[2] << 8) | skb->data[3]);
	if (hlen < IP_HDR_MIN || total != skb->len || total <= hlen)
		goto fail;
	if (mtu < hlen + 8)
		goto fail; // no room for the smallest fragment
	mm_memcpy(hdr, skb->data, hlen);
	frag_field = (uint16_t)((hdr[6] << 8) | hdr[7]);
	not_last = (frag_field & IP_MF) != 0;
	offset = (uint32_t)(frag_field & IP_OFFSET) << 3;
	left = total - hlen;
	ptr = hlen;
	flen = (mtu - hlen) & ~7u;

	while (left > 0) {
		uint32_t len = left > mtu - hlen ? flen : left;
		uint16_t ff = (uint16_t)(offset >> 3);
		sk_buff_t *f;
		uint8_t *p;

		f = skb_alloc(hlen + len);
		if (!f) {
			err = -ENOBUFS;
			break;
		}
		p = skb_append(f, hlen + len);
		mm_memcpy(p, hdr, hlen);
		mm_memcpy(p + hlen, skb->data + ptr, len);
		left -= len;
		if (left > 0 || not_last)
			ff |= IP_MF;
		p[2] = (uint8_t)((hlen + len) >> 8);
		p[3] = (uint8_t)(hlen + len);
		p[6] = (uint8_t)(ff >> 8);
		p[7] = (uint8_t)ff;
		p[10] = p[11] = 0;
		{
			uint16_t c = ipv4_checksum(p, (uint16_t)hlen);

			mm_memcpy(p + 10, &c, 2);
		}
		f->protocol = ETH_P_IP;
		skb_reset_network_header(f);
		if (first) {
			// the others carry only the copied options
			ip_options_fragment(hdr);
			first = 0;
		}
		NET_STATS_INC(NET_MIB_IP_FRAGCREATES);
		err = out(ctx, f);
		if (err)
			break;
		ptr += len;
		offset += len;
	}
	skb_put(skb);
	if (err) {
		NET_STATS_INC(NET_MIB_IP_FRAGFAILS);
		return err;
	}
	NET_STATS_INC(NET_MIB_IP_FRAGOKS);
	return 0;
fail:
	NET_STATS_INC(NET_MIB_IP_FRAGFAILS);
	skb_put(skb);
	return -EMSGSIZE;
}
