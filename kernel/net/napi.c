// LikeOS — receive side of the device core: NAPI, the backlog, GRO
//
// Every received frame is processed on NET_RX_CPU by the NET_RX softirq,
// in the order its device handed it over: napi instances are polled from
// one poll list that only that CPU walks, and drivers that are not
// converted to NAPI yet feed one shared backlog napi through
// net_rx_packet() / netif_rx().  See kernel/net/netdevice.h.
//
// GRO (net_gro_enabled, on by default): a napi holds up to
// NAPI_GRO_MAX_HELD TCP segments, one per flow, and appends to a held
// segment the payload of every following in-order segment of its flow
// whose headers match (same ACK, window, TTL/TOS, DF and identical TCP
// options), up to 64 KB.  Anything else of the flow -- a gap, a flag
// beyond ACK/PSH, a pure ACK, a short segment -- delivers the held segment
// first, so a flow's data reaches TCP in order.  Held segments are
// delivered at the end of every poll and when the napi completes.
// While a run is held it is built in a jumbo buffer; when it is delivered
// it moves into the smallest buffer that holds it (skb_copy_fit), so the
// socket it is queued on is charged about what it carries, not 64 KB.
//
// Lock order: poll_lock and the backlog queue lock are leaves.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/ip.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/softirq.h>
#include <kernel/net/skb.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

int net_gro_enabled = 1;

static spinlock_t poll_lock = SPINLOCK_INIT("napi_poll");
static struct napi_struct *poll_head, *poll_tail;

static struct napi_struct backlog_napi;
static skb_queue_t backlog_q;
static volatile int napi_ready;

static volatile uint64_t st_rx_action_runs;
static volatile uint64_t st_time_squeeze;
static volatile uint64_t st_backlog_drops;
static volatile uint64_t st_backlog_frames;
static volatile uint64_t st_gro_merged;
static volatile uint64_t st_gro_flushed;
static volatile uint64_t st_gro_held_max;

#define NAPI_LISTED 0x100u // on the poll list (poll_lock)

static inline uint32_t rx_cpu_id(void)
{
	return read_gs_base_msr() ? this_cpu_id() : 0;
}

/* Put `n' on the poll list (it is SCHED and not listed) and raise NET_RX
 * on the receive CPU. */
static void __napi_schedule(struct napi_struct *n)
{
	uint64_t f;
	int add = 0;

	spin_lock_irqsave(&poll_lock, &f);
	if (!(n->state & NAPI_LISTED)) {
		__atomic_fetch_or(&n->state, NAPI_LISTED, __ATOMIC_ACQ_REL);
		n->poll_next = NULL;
		if (poll_tail)
			poll_tail->poll_next = n;
		else
			poll_head = n;
		poll_tail = n;
		add = 1;
	}
	spin_unlock_irqrestore(&poll_lock, f);
	if (add)
		softirq_raise_on(NET_RX_CPU, SOFTIRQ_NET_RX);
}

static int napi_schedule_prep(struct napi_struct *n)
{
	uint32_t v, nv;

	do {
		v = __atomic_load_n(&n->state, __ATOMIC_ACQUIRE);
		if (v & NAPI_STATE_DISABLE)
			return 0;
		nv = v | NAPI_STATE_SCHED;
		if (v & NAPI_STATE_SCHED)
			nv |= NAPI_STATE_MISSED;
	} while (!__atomic_compare_exchange_n(&n->state, &v, nv, 0,
					      __ATOMIC_ACQ_REL,
					      __ATOMIC_ACQUIRE));
	return !(v & NAPI_STATE_SCHED);
}

int napi_schedule(struct napi_struct *n)
{
	if (!napi_schedule_prep(n))
		return 0;
	__napi_schedule(n);
	return 1;
}

int napi_complete_done(struct napi_struct *n, int work_done)
{
	uint32_t v, nv;

	(void)work_done;
	napi_gro_flush(n);
	do {
		v = __atomic_load_n(&n->state, __ATOMIC_ACQUIRE);
		WARN_ON_ONCE(!(v & NAPI_STATE_SCHED));
		if (v & NAPI_STATE_MISSED)
			nv = v & ~NAPI_STATE_MISSED; // stays scheduled
		else
			nv = v & ~NAPI_STATE_SCHED;
	} while (!__atomic_compare_exchange_n(&n->state, &v, nv, 0,
					      __ATOMIC_ACQ_REL,
					      __ATOMIC_ACQUIRE));
	if (v & NAPI_STATE_MISSED) {
		__napi_schedule(n);
		return 0;
	}
	return 1;
}

void netif_napi_add(net_device_t *dev, struct napi_struct *n,
		    int (*poll)(struct napi_struct *, int), int weight)
{
	BUG_ON(poll == NULL);
	mm_memset(n, 0, sizeof(*n));
	n->dev = dev;
	n->poll = poll;
	// a device polled by NAPI hands its frames to GRO
	if (dev)
		dev->features |= NETIF_F_GRO;
	n->weight = weight > 0 ? weight : NAPI_POLL_WEIGHT;
	// disabled until napi_enable(), like a device that is not up yet
	n->state = NAPI_STATE_SCHED | NAPI_STATE_DISABLE;
}

void napi_enable(struct napi_struct *n)
{
	__atomic_fetch_and(&n->state,
			   ~(NAPI_STATE_SCHED | NAPI_STATE_MISSED |
			     NAPI_STATE_DISABLE),
			   __ATOMIC_ACQ_REL);
}

void napi_disable(struct napi_struct *n)
{
	uint32_t v;

	might_sleep();
	__atomic_fetch_or(&n->state, NAPI_STATE_DISABLE, __ATOMIC_ACQ_REL);
	/* Own SCHED ourselves once the poll that holds it gives it up:
	 * then nobody can schedule the napi until napi_enable(). */
	for (;;) {
		v = __atomic_load_n(&n->state, __ATOMIC_ACQUIRE);
		if (!(v & NAPI_STATE_SCHED) &&
		    __atomic_compare_exchange_n(&n->state, &v,
						v | NAPI_STATE_SCHED, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			break;
		sched_yield_in_kernel();
	}
	napi_gro_flush(n);
}

// ============================================================================
// Delivery
// ============================================================================

int netif_receive_skb(sk_buff_t *skb)
{
	net_device_t *dev;

	if (!skb)
		return NET_RX_DROP;
	dev = skb->dev;
	BUG_ON(dev == NULL);
	/* An administratively down device receives nothing. */
	if (dev->core_ready &&
	    !(__atomic_load_n(&dev->flags, __ATOMIC_ACQUIRE) & IFF_UP)) {
		dev->rx_dropped++;
		skb_put(skb);
		return NET_RX_DROP;
	}
	/* Loopback frames, and our own multicast and broadcast datagrams
	 * looped back to us, are bare IPv4 datagrams; everything else
	 * starts with its Ethernet header.  The skb is handed on (the
	 * protocols consume it). */
	if (dev == net_get_loopback() || skb->pkt_type == PACKET_LOOPBACK) {
		skb->protocol = ETH_P_IP;
		skb->pkt_type = PACKET_LOOPBACK;
		ip_rcv(skb);
	} else {
		eth_rcv(skb);
	}
	return NET_RX_SUCCESS;
}

/* The backlog napi's poll: frames queued by netif_rx(), in order. */
static int process_backlog(struct napi_struct *n, int quota)
{
	int work = 0;

	while (work < quota) {
		sk_buff_t *skb;
		uint64_t f;

		spin_lock_irqsave(&backlog_q.lock, &f);
		skb = __skb_dequeue(&backlog_q);
		if (!skb) {
			/* Idle again, decided under the queue lock: an
			 * enqueue either came before (and we got it) or
			 * sees SCHED clear and schedules us anew. */
			__atomic_fetch_and(&n->state,
					   ~(NAPI_STATE_SCHED |
					     NAPI_STATE_MISSED),
					   __ATOMIC_ACQ_REL);
			spin_unlock_irqrestore(&backlog_q.lock, f);
			break;
		}
		spin_unlock_irqrestore(&backlog_q.lock, f);
		// GRO when the device asks for it (napi_gro_receive decides)
		napi_gro_receive(n, skb);
		work++;
	}
	return work;
}

int netif_rx(sk_buff_t *skb)
{
	net_device_t *dev = skb->dev;
	int sched = 0;
	uint64_t f;

	BUG_ON(dev == NULL);
	if (!napi_ready) {
		skb_put(skb);
		return NET_RX_DROP;
	}
	spin_lock_irqsave(&backlog_q.lock, &f);
	/* The backlog bound protects the machine from a flood off the wire.
	 * Loopback traffic is generated here at the pace TCP allows and is
	 * bounded by the packet memory instead: dropping it only turns into
	 * retransmissions on both ends. */
	if (backlog_q.len >= NETDEV_MAX_BACKLOG && dev != net_get_loopback()) {
		spin_unlock_irqrestore(&backlog_q.lock, f);
		dev->rx_backlog_drops++;
		st_backlog_drops++;
		skb_put(skb);
		return NET_RX_DROP;
	}
	__skb_queue_tail(&backlog_q, skb);
	if (!(__atomic_fetch_or(&backlog_napi.state, NAPI_STATE_SCHED,
				__ATOMIC_ACQ_REL) &
	      NAPI_STATE_SCHED))
		sched = 1;
	spin_unlock_irqrestore(&backlog_q.lock, f);
	st_backlog_frames++;
	if (sched)
		__napi_schedule(&backlog_napi);
	return NET_RX_SUCCESS;
}

uint32_t net_backlog_len(void)
{
	return skb_queue_len(&backlog_q);
}

/* The NET_RX softirq: poll the scheduled napis, NAPI_POLL_WEIGHT frames
 * each, until the list is empty, NETDEV_BUDGET frames were processed or
 * NETDEV_BUDGET_USECS passed.  What is left goes back to the head of the
 * list and the softirq is raised again (the softirq loop or ksoftirqd
 * comes back for it). */
static void net_rx_action(void)
{
	struct napi_struct *list, *repoll = NULL, *repoll_tail = NULL;
	uint64_t start = timer_get_precise_ns();
	int budget = NETDEV_BUDGET;
	uint64_t f;

	if (rx_cpu_id() != NET_RX_CPU) {
		/* Raised on the wrong CPU: every poll runs on NET_RX_CPU. */
		if (__atomic_load_n(&poll_head, __ATOMIC_ACQUIRE))
			softirq_raise_on(NET_RX_CPU, SOFTIRQ_NET_RX);
		return;
	}
	st_rx_action_runs++;
	spin_lock_irqsave(&poll_lock, &f);
	list = poll_head;
	poll_head = poll_tail = NULL;
	for (struct napi_struct *n = list; n; n = n->poll_next)
		__atomic_fetch_and(&n->state, ~NAPI_LISTED, __ATOMIC_ACQ_REL);
	spin_unlock_irqrestore(&poll_lock, f);

	while (list) {
		struct napi_struct *n = list;
		int work = 0;

		list = n->poll_next;
		n->poll_next = NULL;
		if (__atomic_load_n(&n->state, __ATOMIC_ACQUIRE) &
		    NAPI_STATE_SCHED)
			work = n->poll(n, n->weight);
		WARN_ON_ONCE(work > n->weight);
		n->polls++;
		n->frames += (uint64_t)work;
		budget -= work;
		napi_gro_flush(n);
		/* Listed again already: its poll completed and was
		 * rescheduled (NAPI_STATE_MISSED) -- against the contract
		 * for a poll that used its whole weight, but then it is on
		 * the list and must not be linked twice. */
		if (work >= n->weight &&
		    (__atomic_load_n(&n->state, __ATOMIC_ACQUIRE) &
		     (NAPI_STATE_SCHED | NAPI_LISTED)) == NAPI_STATE_SCHED) {
			/* Used its whole weight: still scheduled, polled
			 * again after the others. */
			if (repoll_tail)
				repoll_tail->poll_next = n;
			else
				repoll = n;
			repoll_tail = n;
		}
		if (budget <= 0 ||
		    timer_get_precise_ns() - start >=
			    (uint64_t)NETDEV_BUDGET_USECS * 1000) {
			if (list || repoll)
				st_time_squeeze++;
			break;
		}
	}

	/* Back onto the poll list: the ones not polled in this run first,
	 * then whatever was scheduled meanwhile, then the repolls -- a napi
	 * that used its whole weight goes behind the others.  None of ours
	 * can have been listed meanwhile: they are all still SCHED, so
	 * napi_schedule() leaves them alone, and the repolls were checked
	 * above. */
	if (repoll_tail)
		repoll_tail->poll_next = NULL;
	if (!list && !repoll)
		return;
	spin_lock_irqsave(&poll_lock, &f);
	for (struct napi_struct *n = list; n; n = n->poll_next)
		__atomic_fetch_or(&n->state, NAPI_LISTED, __ATOMIC_ACQ_REL);
	for (struct napi_struct *n = repoll; n; n = n->poll_next)
		__atomic_fetch_or(&n->state, NAPI_LISTED, __ATOMIC_ACQ_REL);
	{
		struct napi_struct *head = list, *tail = NULL;

		for (struct napi_struct *n = list; n; n = n->poll_next)
			tail = n;
		// + the newly scheduled
		if (poll_head) {
			if (tail)
				tail->poll_next = poll_head;
			else
				head = poll_head;
			tail = poll_tail;
		}
		// + the repolls
		if (repoll) {
			if (tail)
				tail->poll_next = repoll;
			else
				head = repoll;
			tail = repoll_tail;
		}
		poll_head = head;
		poll_tail = tail;
	}
	spin_unlock_irqrestore(&poll_lock, f);
	softirq_raise_on(NET_RX_CPU, SOFTIRQ_NET_RX);
}

void napi_get_stats(struct napi_stats *out)
{
	mm_memset(out, 0, sizeof(*out));
	out->rx_action_runs = st_rx_action_runs;
	out->time_squeeze = st_time_squeeze;
	out->backlog_drops = st_backlog_drops;
	out->backlog_frames = st_backlog_frames;
	out->gro_merged = st_gro_merged;
	out->gro_flushed = st_gro_flushed;
	out->gro_held_max = st_gro_held_max;
	out->backlog_len = skb_queue_len(&backlog_q);
}

void napi_init_subsys(void)
{
	skb_queue_init(&backlog_q, "net_backlog");
	netif_napi_add(NULL, &backlog_napi, process_backlog, NAPI_POLL_WEIGHT);
	napi_enable(&backlog_napi);
	softirq_register(SOFTIRQ_NET_RX, net_rx_action);
	__atomic_store_n(&napi_ready, 1, __ATOMIC_RELEASE);
}

// ============================================================================
// GRO
// ============================================================================

/* What GRO keeps about a held segment, in its control block. */
struct gro_cb {
	uint32_t saddr, daddr; // host order
	uint16_t sport, dport; // network order, as on the wire
	uint32_t next_seq; // the sequence number that would continue it
	uint16_t mss; // payload of its first segment
	uint16_t count; // segments merged into it
	uint16_t hlen; // Ethernet + IP + TCP header bytes
	uint16_t big; // moved into a buffer sized for a merged segment
};

#define GRO_CB(skb) ((struct gro_cb *)(skb)->cb)
BUILD_BUG_ON(sizeof(struct gro_cb) > SKB_CB_SIZE);

/* The parsed headers of a candidate frame. */
struct gro_hdr {
	const uint8_t *ip, *tcp;
	uint32_t saddr, daddr, seq;
	uint16_t sport, dport;
	uint16_t ip_total, tcp_hlen, payload;
	uint8_t flags;
};

#define TCPF_ALL_BUT_PSH_ACK (TCP_FIN | TCP_SYN | TCP_RST | TCP_URG | 0xC0)

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* 1 when `skb' is an IPv4 TCP segment GRO can look at (valid header and
 * transport checksums, no IP options, not a fragment); fills `h'. */
static int gro_parse(sk_buff_t *skb, struct gro_hdr *h)
{
	const uint8_t *d = skb->data;
	uint32_t len = skb->len;
	uint32_t sum;

	if (len < ETH_HLEN + 40 || rd16(d + 12) != ETH_P_IP)
		return 0;
	h->ip = d + ETH_HLEN;
	if (h->ip[0] != 0x45)
		return 0; // not IPv4 or IP options present
	h->ip_total = rd16(h->ip + 2);
	if (h->ip_total < 40 || (uint32_t)h->ip_total + ETH_HLEN > len)
		return 0;
	if (rd16(h->ip + 6) & 0x3FFF)
		return 0; // a fragment
	if (h->ip[9] != IP_PROTO_TCP)
		return 0;
	if (ipv4_checksum(h->ip, 20) != 0)
		return 0;
	h->tcp = h->ip + 20;
	h->tcp_hlen = (uint16_t)((h->tcp[12] >> 4) * 4);
	if (h->tcp_hlen < 20 || h->tcp_hlen > h->ip_total - 20)
		return 0;
	h->saddr = rd32(h->ip + 12);
	h->daddr = rd32(h->ip + 16);
	h->sport = rd16(h->tcp);
	h->dport = rd16(h->tcp + 2);
	h->seq = rd32(h->tcp + 4);
	h->flags = h->tcp[13];
	h->payload = (uint16_t)(h->ip_total - 20 - h->tcp_hlen);
	sum = csum_tcpudp_nofold(h->saddr, h->daddr,
				 (uint16_t)(h->ip_total - 20), IP_PROTO_TCP,
				 csum_partial(h->tcp, h->ip_total - 20u, 0));
	if (csum_fold(sum) != 0)
		return 0; // corrupt: TCP will see (and drop) it
	// Ethernet padding beyond the datagram
	if ((uint32_t)h->ip_total + ETH_HLEN < len)
		skb_trim(skb, (uint32_t)h->ip_total + ETH_HLEN);
	return 1;
}

static int gro_same_flow(const sk_buff_t *held, const struct gro_hdr *h)
{
	const struct gro_cb *c = GRO_CB(held);

	return c->saddr == h->saddr && c->daddr == h->daddr &&
	       c->sport == h->sport && c->dport == h->dport;
}

/* Every header field that must match for `h' to continue `held'. */
static int gro_can_merge(const sk_buff_t *held, const struct gro_hdr *h)
{
	const struct gro_cb *c = GRO_CB(held);
	const uint8_t *hip = held->data + ETH_HLEN;
	const uint8_t *htcp = hip + 20;
	uint16_t htcp_hlen = (uint16_t)((htcp[12] >> 4) * 4);

	if (h->seq != c->next_seq)
		return 0;
	if (h->payload == 0 || h->payload > c->mss)
		return 0;
	if (hip[1] != h->ip[1] || hip[8] != h->ip[8]) // TOS, TTL
		return 0;
	if ((rd16(hip + 6) ^ rd16(h->ip + 6)) & 0x4000) // DF
		return 0;
	if (rd32(htcp + 8) != rd32(h->tcp + 8)) // ACK number
		return 0;
	if (rd16(htcp + 14) != rd16(h->tcp + 14)) // window
		return 0;
	if (htcp_hlen != h->tcp_hlen)
		return 0;
	for (uint16_t i = 20; i < htcp_hlen; i++) // options, TS values too
		if (htcp[i] != h->tcp[i])
			return 0;
	if (held->len + h->payload > (uint32_t)ETH_HLEN + 65535)
		return 0;
	return 1;
}

/* Deliver a held segment, its headers updated for the merged payload. */
static void gro_finish(sk_buff_t *skb)
{
	struct gro_cb *c = GRO_CB(skb);

	if (c->big) {
		/* Out of the jumbo buffer it was built in, into one sized
		 * for it (when there is one: else it goes as it is). */
		sk_buff_t *fit = skb_copy_fit(skb);

		if (fit) {
			skb_put(skb);
			skb = fit;
			c = GRO_CB(skb);
			c->big = 0;
		}
	}
	if (c->count > 1) {
		uint8_t *ip = skb->data + ETH_HLEN;
		uint8_t *tcp = ip + 20;
		uint16_t total = (uint16_t)(skb->len - ETH_HLEN);
		uint32_t sum;
		uint16_t check;

		ip[2] = (uint8_t)(total >> 8);
		ip[3] = (uint8_t)total;
		ip[10] = ip[11] = 0;
		check = ipv4_checksum(ip, 20);
		mm_memcpy(ip + 10, &check, 2);
		tcp[16] = tcp[17] = 0;
		sum = csum_tcpudp_nofold(c->saddr, c->daddr,
					 (uint16_t)(total - 20), IP_PROTO_TCP,
					 csum_partial(tcp, total - 20u, 0));
		check = csum_fold(sum);
		mm_memcpy(tcp + 16, &check, 2);
		skb->shinfo->gso_segs = c->count;
		skb->shinfo->gso_size = c->mss;
		st_gro_flushed += c->count;
	}
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	netif_receive_skb(skb);
}

static void gro_remove(struct napi_struct *n, uint32_t i)
{
	for (uint32_t j = i + 1; j < n->gro_count; j++)
		n->gro_held[j - 1] = n->gro_held[j];
	n->gro_count--;
	n->gro_held[n->gro_count] = NULL;
}

void napi_gro_flush(struct napi_struct *n)
{
	while (n->gro_count) {
		sk_buff_t *skb = n->gro_held[0];

		gro_remove(n, 0);
		gro_finish(skb);
	}
}

/* Append the payload of the segment `h' describes to the held segment at
 * index i.  Returns 0
 * when there was no memory for a larger buffer (nothing changed). */
static int gro_merge(struct napi_struct *n, uint32_t i,
		     const struct gro_hdr *h)
{
	sk_buff_t *held = n->gro_held[i];
	struct gro_cb *c = GRO_CB(held);

	/* The first merge moves the held segment into a buffer for a 64 KB
	 * segment; a received frame's own buffer is not appended to (its
	 * tailroom belongs to nobody's accounting). */
	if (!c->big || skb_tailroom(held) < h->payload) {
		sk_buff_t *big = skb_alloc_headroom(64, ETH_HLEN + 65535);

		if (!big)
			return 0;
		mm_memcpy(skb_append(big, held->len), held->data, held->len);
		big->dev = held->dev;
		big->protocol = held->protocol;
		big->tstamp_ns = held->tstamp_ns;
		mm_memcpy(big->cb, held->cb, SKB_CB_SIZE);
		n->gro_held[i] = big;
		skb_put(held);
		held = big;
		c = GRO_CB(held);
		c->big = 1;
	}
	mm_memcpy(skb_append(held, h->payload),
		  h->tcp + h->tcp_hlen, h->payload);
	// PSH of any merged segment is carried by the result
	held->data[ETH_HLEN + 20 + 13] |= (uint8_t)(h->flags & TCP_PSH);
	c->next_seq = h->seq + h->payload;
	c->count++;
	st_gro_merged++;
	return 1;
}

void napi_gro_receive(struct napi_struct *n, sk_buff_t *skb)
{
	struct gro_hdr h;
	uint32_t i;

	if (!net_gro_enabled || !skb->dev ||
	    !(skb->dev->features & NETIF_F_GRO)) {
		netif_receive_skb(skb);
		return;
	}
	if (!gro_parse(skb, &h)) {
		/* Not a segment GRO understands -- maybe one of a held flow
		 * (IP options, a bad checksum): what is held goes first, so
		 * nothing of a flow overtakes it. */
		napi_gro_flush(n);
		netif_receive_skb(skb);
		return;
	}
	for (i = 0; i < n->gro_count; i++)
		if (n->gro_held[i]->dev == skb->dev &&
		    gro_same_flow(n->gro_held[i], &h))
			break;
	if ((h.flags & TCPF_ALL_BUT_PSH_ACK) || !(h.flags & TCP_ACK) ||
	    h.payload == 0) {
		// Not mergeable: deliver what is held of the flow first.
		if (i < n->gro_count) {
			sk_buff_t *held = n->gro_held[i];

			gro_remove(n, i);
			gro_finish(held);
		}
		netif_receive_skb(skb);
		return;
	}
	if (i < n->gro_count) {
		sk_buff_t *held = n->gro_held[i];

		if (gro_can_merge(held, &h) && gro_merge(n, i, &h)) {
			held = n->gro_held[i];
			skb_put(skb);
			/* A short or pushed segment ends the run, and so
			 * does a full one. */
			if ((h.flags & TCP_PSH) || h.payload < GRO_CB(held)->mss ||
			    held->len + GRO_CB(held)->mss >
				    (uint32_t)ETH_HLEN + 65535) {
				gro_remove(n, i);
				gro_finish(held);
			}
			return;
		}
		gro_remove(n, i);
		gro_finish(held);
	}
	if (h.flags & TCP_PSH) {
		netif_receive_skb(skb); // nothing would follow it
		return;
	}
	if (n->gro_count == NAPI_GRO_MAX_HELD) {
		sk_buff_t *old = n->gro_held[0];

		gro_remove(n, 0);
		gro_finish(old);
	}
	{
		struct gro_cb *c = GRO_CB(skb);

		mm_memset(c, 0, sizeof(*c));
		c->saddr = h.saddr;
		c->daddr = h.daddr;
		c->sport = h.sport;
		c->dport = h.dport;
		c->next_seq = h.seq + h.payload;
		c->mss = h.payload;
		c->count = 1;
		c->hlen = (uint16_t)(ETH_HLEN + 20 + h.tcp_hlen);
	}
	n->gro_held[n->gro_count++] = skb;
	if (n->gro_count > st_gro_held_max)
		st_gro_held_max = n->gro_count;
}
