// LikeOS ICMP (Internet Control Message Protocol, RFC 792, RFC 1122)
//
// Receiving:
//   * echo request -> echo reply (to the requester, from the address it
//     was sent to, the request's TOS); not for a request sent to a
//     broadcast or multicast address;
//   * timestamp request -> timestamp reply (milliseconds since midnight
//     UT); the same broadcast rule;
//   * destination unreachable, time exceeded, parameter problem: the error
//     is about a datagram we sent; the quoted header is checked and the
//     error goes to the err_handler of the quoted protocol (TCP, UDP) and
//     to the raw sockets of that protocol.  "Fragmentation needed" first
//     lowers the path MTU of the destination (a path-MTU exception, at
//     least 552 bytes, never raised by a report);
//   * redirects and source quench are ignored (a host keeps its routes;
//     source quench is deprecated, RFC 6633), as are address mask and
//     information requests;
//   * echo replies and errors quoting an echo also go to the kernel ping's
//     reply queue (icmp_recv_reply).
// Only echo, echo reply, timestamp, timestamp reply, address mask and
// address mask reply may come as broadcast or multicast; the rest is
// dropped then (an error to a broadcast address is bogus).
//
// Sending errors (icmp_send): never about an ICMP error, a datagram sent
// to a broadcast or multicast address, a fragment other than the first,
// or a datagram whose source is not a single host.  Rate limits: errors
// (destination unreachable, source quench, time exceeded, parameter
// problem) draw on a global token bucket of 1000 messages a second
// (burst 50) and on one per destination (one a second, burst 6; not for
// our own addresses).  Replies to echo and timestamp requests are not
// limited.  An error quotes the offending datagram up to 576 bytes in all.
//
// Lock order: icmp_rl_lock and icmp_reply_lock are leaves.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/net/fib.h>
#include <kernel/io/console.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/sched.h>
#include <kernel/dev/rand/random.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

#ifndef ENONET
#define ENONET 64 // Machine is not on the network
#endif

#define ICMP_REPLY_QUEUE_SIZE 16
#define ICMP_ERR_MAX_LEN 576 // a whole error message, IP header included
#define ICMP_MSGS_PER_SEC 1000
#define ICMP_MSGS_BURST 50
#define ICMP_RATELIMIT_MS 1000 // per destination
#define ICMP_XRLIM_BURST 6
#define ICMP_RATEMASK                                                    \
	((1u << ICMP_DEST_UNREACH) | (1u << ICMP_SOURCE_QUENCH) |        \
	 (1u << ICMP_TIME_EXCEEDED) | (1u << ICMP_PARAMETERPROB))
#define ICMP_PEERS 256

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline int ipv4_is_multicast(uint32_t a)
{
	return (a & 0xF0000000u) == 0xE0000000u;
}

// Which types are errors (an error is never answered with an error).
static int icmp_type_is_error(uint8_t type)
{
	switch (type) {
	case ICMP_DEST_UNREACH:
	case ICMP_SOURCE_QUENCH:
	case ICMP_REDIRECT:
	case ICMP_TIME_EXCEEDED:
	case ICMP_PARAMETERPROB:
		return 1;
	default:
		return type > NR_ICMP_TYPES; // unknown: assume an error
	}
}

// ============================================================================
// Errors -> errno (RFC 1122 4.2.3.9; "fatal" = a hard error)
// ============================================================================

static const struct {
	int err;
	int fatal;
} icmp_unreach_err[NR_ICMP_UNREACH + 1] = {
	{ ENETUNREACH, 0 }, // NET_UNREACH
	{ EHOSTUNREACH, 0 }, // HOST_UNREACH
	{ ENOPROTOOPT, 1 }, // PROT_UNREACH
	{ ECONNREFUSED, 1 }, // PORT_UNREACH
	{ EMSGSIZE, 0 }, // FRAG_NEEDED
	{ EOPNOTSUPP, 0 }, // SR_FAILED
	{ ENETUNREACH, 1 }, // NET_UNKNOWN
	{ EHOSTDOWN, 1 }, // HOST_UNKNOWN
	{ ENONET, 1 }, // HOST_ISOLATED
	{ ENETUNREACH, 1 }, // NET_ANO
	{ EHOSTUNREACH, 1 }, // HOST_ANO
	{ ENETUNREACH, 0 }, // NET_UNR_TOS
	{ EHOSTUNREACH, 0 }, // HOST_UNR_TOS
	{ EHOSTUNREACH, 1 }, // PKT_FILTERED
	{ EHOSTUNREACH, 1 }, // PREC_VIOLATION
	{ EHOSTUNREACH, 1 }, // PREC_CUTOFF
};

int icmp_err_convert(uint8_t type, uint8_t code, int *fatal)
{
	*fatal = 0;
	switch (type) {
	case ICMP_DEST_UNREACH:
		if (code <= NR_ICMP_UNREACH) {
			*fatal = icmp_unreach_err[code].fatal;
			return icmp_unreach_err[code].err;
		}
		return EHOSTUNREACH;
	case ICMP_PARAMETERPROB:
		*fatal = 1;
		return EPROTO;
	case ICMP_TIME_EXCEEDED:
	default:
		return EHOSTUNREACH;
	}
}

// ============================================================================
// Rate limits
// ============================================================================

static spinlock_t icmp_rl_lock = SPINLOCK_INIT("icmp_rl");
static uint64_t icmp_global_last_ms;
static uint32_t icmp_global_credit = ICMP_MSGS_BURST;

struct icmp_peer {
	uint32_t addr; // 0: free
	uint32_t tokens; // milliseconds of credit
	uint64_t last_ms;
};
static struct icmp_peer icmp_peers[ICMP_PEERS];
static uint64_t st_rl_global, st_rl_host, st_out, st_out_err;
static uint64_t st_in, st_in_err, st_in_csum;

static uint64_t icmp_now_ms(void)
{
	return timer_get_precise_us() / 1000;
}

/* The global bucket: 1000 messages a second, at most 50 at once. */
static int icmp_global_allow_locked(uint64_t now)
{
	uint64_t delta = now - icmp_global_last_ms;

	if (delta > 1000)
		delta = 1000;
	if (delta) {
		uint64_t c = icmp_global_credit +
			     delta * ICMP_MSGS_PER_SEC / 1000;

		icmp_global_credit = (uint32_t)(c > ICMP_MSGS_BURST ?
							ICMP_MSGS_BURST :
							c);
		icmp_global_last_ms = now;
	}
	return icmp_global_credit > 0;
}

/* The bucket of one destination: a message a second, bursts of six. */
static int icmp_peer_allow_locked(uint32_t daddr, uint64_t now)
{
	uint32_t h = (daddr * 0x9E3779B1u) >> 24; // 256 slots
	struct icmp_peer *p = NULL, *victim = NULL;
	uint64_t tok;

	for (uint32_t i = 0; i < 8; i++) { // probe a few slots
		struct icmp_peer *c = &icmp_peers[(h + i) & (ICMP_PEERS - 1)];

		if (c->addr == daddr) {
			p = c;
			break;
		}
		if (!victim || !c->addr ||
		    (victim->addr && c->last_ms < victim->last_ms))
			victim = c;
	}
	if (!p) {
		p = victim;
		p->addr = daddr;
		p->tokens = ICMP_XRLIM_BURST * ICMP_RATELIMIT_MS;
		p->last_ms = now;
	}
	tok = p->tokens + (now - p->last_ms);
	p->last_ms = now;
	if (tok > ICMP_XRLIM_BURST * ICMP_RATELIMIT_MS)
		tok = ICMP_XRLIM_BURST * ICMP_RATELIMIT_MS;
	if (tok >= ICMP_RATELIMIT_MS) {
		p->tokens = (uint32_t)(tok - ICMP_RATELIMIT_MS);
		return 1;
	}
	p->tokens = (uint32_t)tok;
	return 0;
}

void icmp_rl_reset(void)
{
	uint64_t f;

	spin_lock_irqsave(&icmp_rl_lock, &f);
	icmp_global_credit = ICMP_MSGS_BURST;
	icmp_global_last_ms = icmp_now_ms();
	mm_memset(icmp_peers, 0, sizeof(icmp_peers));
	spin_unlock_irqrestore(&icmp_rl_lock, f);
}

void icmp_get_stats(struct icmp_rl_stats *st)
{
	st->out_msgs = __atomic_load_n(&st_out, __ATOMIC_RELAXED);
	st->out_errors = __atomic_load_n(&st_out_err, __ATOMIC_RELAXED);
	st->rl_global = __atomic_load_n(&st_rl_global, __ATOMIC_RELAXED);
	st->rl_host = __atomic_load_n(&st_rl_host, __ATOMIC_RELAXED);
	st->in_msgs = __atomic_load_n(&st_in, __ATOMIC_RELAXED);
	st->in_errors = __atomic_load_n(&st_in_err, __ATOMIC_RELAXED);
	st->in_csum_errors = __atomic_load_n(&st_in_csum, __ATOMIC_RELAXED);
}

static void icmp_count_out(uint8_t type, int ok)
{
	if (!ok) {
		NET_STATS_INC(NET_MIB_ICMP_OUTERRORS);
		__atomic_fetch_add(&st_out_err, 1, __ATOMIC_RELAXED);
		return;
	}
	NET_STATS_INC(NET_MIB_ICMP_OUTMSGS);
	__atomic_fetch_add(&st_out, 1, __ATOMIC_RELAXED);
	switch (type) {
	case ICMP_ECHO_REPLY:
		NET_STATS_INC(NET_MIB_ICMP_OUTECHOREPS);
		break;
	case ICMP_TIMESTAMP_REPLY:
		NET_STATS_INC(NET_MIB_ICMP_OUTTIMESTAMPREPS);
		break;
	case ICMP_DEST_UNREACH:
		NET_STATS_INC(NET_MIB_ICMP_OUTDESTUNREACHS);
		break;
	case ICMP_TIME_EXCEEDED:
		NET_STATS_INC(NET_MIB_ICMP_OUTTIMEEXCDS);
		break;
	case ICMP_PARAMETERPROB:
		NET_STATS_INC(NET_MIB_ICMP_OUTPARMPROBS);
		break;
	default:
		break;
	}
}

// ============================================================================
// The kernel ping's reply queue
// ============================================================================

typedef struct {
	uint32_t src_ip;
	uint8_t type;
	uint8_t code;
	uint16_t id;
	uint16_t seq;
	uint8_t ttl;
	uint8_t pad[3];
	uint64_t recv_us;
} icmp_reply_entry_t;

static icmp_reply_entry_t icmp_reply_queue[ICMP_REPLY_QUEUE_SIZE];
static int icmp_reply_head = 0;
static int icmp_reply_tail = 0;
static volatile int icmp_reply_ready = 0;
static spinlock_t icmp_reply_lock = SPINLOCK_INIT("icmp_reply");

// Kernel-side timestamps for RTT (microseconds from timer_get_precise_us)
static volatile uint64_t icmp_send_us = 0;

static void icmp_queue_reply(uint32_t src_ip, uint8_t type, uint8_t code,
			     uint16_t id, uint16_t seq, uint8_t ttl)
{
	uint64_t flags;
	spin_lock_irqsave(&icmp_reply_lock, &flags);

	int next_tail = (icmp_reply_tail + 1) % ICMP_REPLY_QUEUE_SIZE;
	if (next_tail == icmp_reply_head)
		icmp_reply_head = (icmp_reply_head + 1) % ICMP_REPLY_QUEUE_SIZE;

	icmp_reply_entry_t *entry = &icmp_reply_queue[icmp_reply_tail];
	entry->src_ip = src_ip;
	entry->type = type;
	entry->code = code;
	entry->id = id;
	entry->seq = seq;
	entry->ttl = ttl;
	entry->recv_us = timer_get_precise_us();
	icmp_reply_tail = next_tail;
	icmp_reply_ready = (icmp_reply_head != icmp_reply_tail);

	spin_unlock_irqrestore(&icmp_reply_lock, flags);
	sched_wake_channel((void *)&icmp_reply_ready);
}

// ============================================================================
// Sending
// ============================================================================

/* Build an ICMP message (header + body) and send it to `daddr' from
 * `saddr' (0: the route's).  Not under any lock. */
static int icmp_xmit(uint32_t saddr, uint32_t daddr, uint8_t type,
		     uint8_t code, uint32_t rest, const uint8_t *body,
		     uint32_t blen, uint8_t tos, uint8_t ttl,
		     struct ip_route *rt)
{
	struct ip_tx tx;
	sk_buff_t *skb = skb_alloc(8 + blen);
	uint8_t *p;
	uint16_t c;
	int rc;

	if (!skb) {
		icmp_count_out(type, 0);
		return -ENOBUFS;
	}
	p = skb_append(skb, 8 + blen);
	p[0] = type;
	p[1] = code;
	p[2] = p[3] = 0;
	wr32(p + 4, rest);
	if (blen)
		mm_memcpy(p + 8, body, blen);
	c = csum_fold(csum_partial(p, 8 + blen, 0));
	mm_memcpy(p + 2, &c, 2);
	mm_memset(&tx, 0, sizeof(tx));
	tx.saddr = saddr;
	tx.daddr = daddr;
	tx.proto = IP_PROTO_ICMP;
	tx.ttl = ttl;
	tx.tos = tos;
	tx.mc_ttl = 1;
	tx.pmtudisc = IP_PMTUDISC_DONT; // ICMP goes without DF
	rc = ip_send_skb(skb, &tx, rt, NULL);
	icmp_count_out(type, rc >= 0);
	return rc;
}

void icmp_send(sk_buff_t *skb_in, uint8_t type, uint8_t code, uint32_t info)
{
	const uint8_t *iph;
	struct inet_skb_parm *cb;
	struct ip_route rt;
	uint32_t ihl, avail, tot, qlen, saddr, daddr;
	uint64_t now, f;
	int limited = (ICMP_RATEMASK >> type) & 1;
	uint8_t tos;

	if (!skb_in || skb_in->network_header == SKB_HDR_UNSET)
		return;
	iph = skb_network_header(skb_in);
	if (iph < skb_in->head || iph + IP_HDR_MIN > skb_in->tail)
		return;
	ihl = (uint32_t)(iph[0] & 0x0F) * 4;
	avail = (uint32_t)(skb_in->tail - iph);
	if (ihl < IP_HDR_MIN || ihl > avail)
		return;
	cb = IPCB(skb_in);
	// only about a datagram to one of our addresses, from one host
	if (cb->rt_type != RTN_LOCAL || skb_in->pkt_type == PACKET_BROADCAST ||
	    skb_in->pkt_type == PACKET_MULTICAST)
		return;
	if (rd16(iph + 6) & IP_OFFSET)
		return; // only the first fragment is answered
	daddr = rd32(iph + 12); // the offender's source: where it goes
	if (daddr == 0 || ipv4_is_multicast(daddr) ||
	    ip_addr_is_broadcast(daddr) || (daddr & 0xFF000000u) == 0)
		return;
	if (icmp_type_is_error(type) && iph[9] == IP_PROTO_ICMP) {
		// never an error about an error (nor about unknown types)
		if (avail < ihl + 1 || icmp_type_is_error(iph[ihl]))
			return;
	}
	now = icmp_now_ms();
	if (limited) {
		int ok;

		spin_lock_irqsave(&icmp_rl_lock, &f);
		ok = icmp_global_allow_locked(now);
		spin_unlock_irqrestore(&icmp_rl_lock, f);
		if (!ok) {
			NET_STATS_INC(NET_MIB_ICMP_OUTRATELIMITGLOBAL);
			__atomic_fetch_add(&st_rl_global, 1, __ATOMIC_RELAXED);
			return;
		}
	}
	/* From the address the datagram was sent to when that is ours,
	 * otherwise from the route's. */
	saddr = rd32(iph + 16);
	if (!ip_addr_is_local(saddr))
		saddr = 0;
	if (ip_route_out(daddr, saddr, NULL, &rt) < 0)
		return;
	if (limited) {
		int ok = 1;

		spin_lock_irqsave(&icmp_rl_lock, &f);
		if (!rt.loopback)
			ok = icmp_peer_allow_locked(daddr, now);
		if (ok && icmp_global_credit)
			icmp_global_credit--;
		spin_unlock_irqrestore(&icmp_rl_lock, f);
		if (!ok) {
			NET_STATS_INC(NET_MIB_ICMP_OUTRATELIMITHOST);
			__atomic_fetch_add(&st_rl_host, 1, __ATOMIC_RELAXED);
			ip_route_put(&rt);
			return;
		}
	}
	tot = rd16(iph + 2);
	if (tot < ihl)
		tot = ihl;
	qlen = tot < avail ? tot : avail;
	if (qlen > ICMP_ERR_MAX_LEN - IP_HDR_MIN - 8)
		qlen = ICMP_ERR_MAX_LEN - IP_HDR_MIN - 8;
	tos = icmp_type_is_error(type) ? (uint8_t)((iph[1] & 0x1E) | 0xC0) :
					 iph[1];
	icmp_xmit(saddr, daddr, type, code, info, iph, qlen, tos, 0, &rt);
	ip_route_put(&rt);
}

// Send ICMP echo request with configurable TTL (the kernel ping)
int icmp_send_echo(net_device_t *dev, uint32_t dst_ip, uint16_t id,
		   uint16_t seq, const uint8_t *payload, uint16_t payload_len,
		   uint8_t ttl)
{
	struct ip_route rt;
	int rc;

	if (!dev)
		return -1;
	if ((uint32_t)payload_len + 8 > NET_MTU_DEFAULT)
		return -1;
	if (ip_route_out(dst_ip, 0, dev, &rt) < 0)
		return -1;
	// Record send timestamp before transmitting
	icmp_send_us = timer_get_precise_us();
	rc = icmp_xmit(0, dst_ip, ICMP_ECHO_REQUEST, 0,
		       ((uint32_t)id << 16) | seq, payload, payload_len, 0,
		       ttl ? ttl : IP_DEFAULT_TTL, &rt);
	ip_route_put(&rt);
	return rc < 0 ? -1 : 0;
}

// Receive ICMP reply (blocking with timeout, filtered by ID and seq)
int icmp_recv_reply(uint32_t *src_ip, uint16_t expected_id, uint8_t *type_out,
		    uint8_t *code_out, uint16_t *seq_out,
		    uint64_t timeout_ticks, uint64_t *rtt_us_out,
		    uint16_t expected_seq, uint8_t *ttl_out)
{
	uint64_t start = timer_ticks();
	task_t *cur = sched_current();
	for (;;) {
		while (!icmp_reply_ready) {
			if (timer_ticks() - start > timeout_ticks)
				return -1;
			// Block the task until woken by icmp_rcv or timeout
			cur->wait_channel = (void *)&icmp_reply_ready;
			uint64_t remaining =
				timeout_ticks - (timer_ticks() - start);
			if (remaining > timeout_ticks)
				remaining = 0; // underflow guard
			cur->wakeup_tick = timer_ticks() + remaining;
			task_set_blocked(cur);
			sched_schedule();
			cur->wait_channel = NULL;
			cur->wakeup_tick = 0;
		}
		uint64_t flags;
		spin_lock_irqsave(&icmp_reply_lock, &flags);

		int found = -1;
		for (int idx = icmp_reply_head; idx != icmp_reply_tail;
		     idx = (idx + 1) % ICMP_REPLY_QUEUE_SIZE) {
			icmp_reply_entry_t *entry = &icmp_reply_queue[idx];
			/* Match by (id, seq): an echo reply carries the
			 * values of the request; an error quoting an echo
			 * request carries those of the quoted request (see
			 * icmp_note_reply); others have 0/0. */
			if (entry->id != expected_id ||
			    entry->seq != expected_seq)
				continue;
			found = idx;
			break;
		}

		if (found < 0) {
			/* Stale entries stay queued; wait for a new one
			 * instead of spinning over them. */
			icmp_reply_ready = 0;
			spin_unlock_irqrestore(&icmp_reply_lock, flags);
			continue;
		}

		icmp_reply_entry_t entry = icmp_reply_queue[found];
		while (found != icmp_reply_head) {
			int prev = (found - 1 + ICMP_REPLY_QUEUE_SIZE) %
				   ICMP_REPLY_QUEUE_SIZE;
			icmp_reply_queue[found] = icmp_reply_queue[prev];
			found = prev;
		}
		icmp_reply_head = (icmp_reply_head + 1) % ICMP_REPLY_QUEUE_SIZE;
		icmp_reply_ready = (icmp_reply_head != icmp_reply_tail);
		spin_unlock_irqrestore(&icmp_reply_lock, flags);

		if (src_ip)
			*src_ip = entry.src_ip;
		if (type_out)
			*type_out = entry.type;
		if (code_out)
			*code_out = entry.code;
		if (seq_out)
			*seq_out = entry.seq;
		if (ttl_out)
			*ttl_out = entry.ttl;
		if (rtt_us_out) {
			uint64_t t0 = icmp_send_us;
			uint64_t t1 = entry.recv_us;
			*rtt_us_out = (t1 > t0) ? (t1 - t0) : 0;
		}
		return 0;
	}
}

// ============================================================================
// Receiving
// ============================================================================

/* An echo reply, or an error quoting one of our echo requests: for the
 * kernel ping. */
static void icmp_note_reply(sk_buff_t *skb, uint32_t from)
{
	const uint8_t *p = skb->data;
	uint32_t len = skb->len;
	uint16_t id = 0, seq = 0;

	if (p[0] == ICMP_ECHO_REPLY) {
		id = rd16(p + 4);
		seq = rd16(p + 6);
	} else if (len >= 8 + IP_HDR_MIN) {
		const uint8_t *in = p + 8;
		uint32_t ihl = (uint32_t)(in[0] & 0x0F) * 4;

		if (ihl >= IP_HDR_MIN && len >= 8 + ihl + 8 &&
		    in[9] == IP_PROTO_ICMP) {
			id = rd16(in + ihl + 4);
			seq = rd16(in + ihl + 6);
		}
	}
	icmp_queue_reply(from, p[0], p[1], id, seq, IPCB(skb)->ttl);
}

/* Hand a validated error to the quoted protocol and its raw sockets. */
static void icmp_err_dispatch(struct icmp_err *e)
{
	const struct net_protocol *prot;
	int fatal = 0;

	e->err = icmp_err_convert(e->type, e->code, &fatal);
	e->fatal = (uint8_t)fatal;
	raw_icmp_error(e);
	prot = inet_get_protocol(e->proto);
	if (prot && prot->err_handler)
		prot->err_handler(e);
}

uint32_t ip_update_pmtu(uint32_t daddr, uint32_t mtu, int *locked)
{
	uint32_t before = fib_exception_get_pmtu(daddr), after;

	*locked = mtu < FIB_MIN_PMTU;
	if (fib_exception_set_pmtu(daddr, mtu) < 0)
		return 0;
	after = fib_exception_get_pmtu(daddr);
	if (after && after != before)
		NET_STATS_INC(NET_MIB_ICMP_PMTUUPDATES);
	return after;
}

/* An error about a datagram of ours: check the quote, hand it to the
 * quoted protocol (which updates the path MTU for "fragmentation needed"
 * when the datagram is one of its senders'). */
static void icmp_unreach(sk_buff_t *skb)
{
	const uint8_t *p = skb->data;
	const uint8_t *in = p + 8;
	struct icmp_err e;
	uint32_t ihl;

	if (skb->len < 8 + IP_HDR_MIN)
		goto bad;
	ihl = (uint32_t)(in[0] & 0x0F) * 4;
	if ((in[0] >> 4) != 4 || ihl < IP_HDR_MIN || skb->len < 8 + ihl + 8)
		goto bad;
	mm_memset(&e, 0, sizeof(e));
	e.type = p[0];
	e.code = p[1];
	e.proto = in[9];
	e.saddr = rd32(in + 12);
	e.daddr = rd32(in + 16);
	e.from = IPCB(skb)->saddr;
	e.iph = in;
	e.ihl = ihl;
	e.l4 = in + ihl;
	e.l4len = skb->len - 8 - ihl;
	e.skb = skb;
	switch (e.type) {
	case ICMP_DEST_UNREACH:
		if (e.code > NR_ICMP_UNREACH)
			return;
		/* "Fragmentation needed": the next-hop MTU (0 from routers
		 * older than RFC 1191: the floor then).  The path MTU is
		 * lowered by the protocol that finds the datagram's sender
		 * -- a report about nobody's datagram changes nothing. */
		if (e.code == ICMP_FRAG_NEEDED)
			e.info = rd16(p + 6);
		break;
	case ICMP_TIME_EXCEEDED:
		break;
	case ICMP_PARAMETERPROB:
		e.info = (uint32_t)p[4]; // the pointer
		break;
	default:
		return;
	}
	icmp_err_dispatch(&e);
	return;
bad:
	NET_STATS_INC(NET_MIB_ICMP_INERRORS);
	__atomic_fetch_add(&st_in_err, 1, __ATOMIC_RELAXED);
}

/* Reply to an echo or timestamp request: to its source, from the address
 * it was sent to, with its TOS. */
static void icmp_reply(sk_buff_t *skb, uint8_t type, const uint8_t *body,
		       uint32_t blen, uint32_t rest)
{
	struct inet_skb_parm *cb = IPCB(skb);
	struct ip_route rt;
	uint32_t saddr = ip_addr_is_local(cb->daddr) ? cb->daddr : 0;

	if (ip_route_out(cb->saddr, saddr, NULL, &rt) < 0)
		return;
	icmp_xmit(saddr, cb->saddr, type, 0, rest, body, blen, cb->tos, 0,
		  &rt);
	ip_route_put(&rt);
}

static uint32_t icmp_ms_since_midnight(void)
{
	uint64_t ms = timer_get_realtime_ns() / 1000000ULL;

	return (uint32_t)(ms % 86400000ULL);
}

int icmp_rcv(sk_buff_t *skb)
{
	struct inet_skb_parm *cb = IPCB(skb);
	const uint8_t *p;
	uint8_t type;

	NET_STATS_INC(NET_MIB_ICMP_INMSGS);
	__atomic_fetch_add(&st_in, 1, __ATOMIC_RELAXED);
	if (skb->len < 8)
		goto error;
	// always verified: receive offload vouches for TCP and UDP only
	if (csum_fold(skb_checksum(skb, 0, skb->len, 0)) != 0) {
		NET_STATS_INC(NET_MIB_ICMP_INCSUMERRORS);
		__atomic_fetch_add(&st_in_csum, 1, __ATOMIC_RELAXED);
		goto error;
	}
	p = skb->data;
	type = p[0];
	if (type > NR_ICMP_TYPES)
		goto error;
	if (cb->rt_type == RTN_BROADCAST || cb->rt_type == RTN_MULTICAST) {
		/* Only queries and their replies make sense to a group;
		 * echo and timestamp requests to one are not answered
		 * (smurf amplification). */
		if (type == ICMP_ECHO_REQUEST || type == ICMP_TIMESTAMP)
			goto drop;
		if (type != ICMP_ECHO_REPLY && type != ICMP_TIMESTAMP_REPLY &&
		    type != ICMP_ADDRMASK && type != ICMP_ADDRMASK_REPLY)
			goto error;
	}
	switch (type) {
	case ICMP_ECHO_REQUEST:
		NET_STATS_INC(NET_MIB_ICMP_INECHOS);
		if (p[1] == 0)
			icmp_reply(skb, ICMP_ECHO_REPLY, p + 8, skb->len - 8,
				   rd32(p + 4));
		break;
	case ICMP_TIMESTAMP:
		NET_STATS_INC(NET_MIB_ICMP_INTIMESTAMPS);
		if (skb->len < 8 + 12)
			goto error;
		{
			uint8_t ts[12];
			uint32_t now = icmp_ms_since_midnight();

			mm_memcpy(ts, p + 8, 4); // originate, as sent
			wr32(ts + 4, now); // receive
			wr32(ts + 8, now); // transmit
			icmp_reply(skb, ICMP_TIMESTAMP_REPLY, ts, sizeof(ts),
				   rd32(p + 4));
		}
		break;
	case ICMP_ECHO_REPLY:
		icmp_note_reply(skb, cb->saddr);
		break;
	case ICMP_DEST_UNREACH:
		NET_STATS_INC(NET_MIB_ICMP_INDESTUNREACHS);
		icmp_unreach(skb);
		icmp_note_reply(skb, cb->saddr);
		break;
	case ICMP_TIME_EXCEEDED:
		NET_STATS_INC(NET_MIB_ICMP_INTIMEEXCDS);
		icmp_unreach(skb);
		icmp_note_reply(skb, cb->saddr);
		break;
	case ICMP_PARAMETERPROB:
		NET_STATS_INC(NET_MIB_ICMP_INPARMPROBS);
		icmp_unreach(skb);
		break;
	case ICMP_REDIRECT:
		/* A host keeps its routes: an unsolicited redirect would
		 * let anybody on the path move our traffic. */
		NET_STATS_INC(NET_MIB_ICMP_INREDIRECTS);
		break;
	case ICMP_SOURCE_QUENCH:
		NET_STATS_INC(NET_MIB_ICMP_INSRCQUENCHS); // RFC 6633
		break;
	default:
		break; // address mask, information, replies: nothing to do
	}
drop:
	skb_put(skb);
	return 0;
error:
	NET_STATS_INC(NET_MIB_ICMP_INERRORS);
	__atomic_fetch_add(&st_in_err, 1, __ATOMIC_RELAXED);
	skb_put(skb);
	return 0;
}

/* A datagram we sent could not be delivered and nothing on the wire says
 * so (address resolution failed): the error goes to its sender as if a
 * router had answered with `type'/`code'. */
void icmp_dispatch_error(uint8_t icmp_type, uint8_t icmp_code,
			 const uint8_t *inner_ip, uint16_t inner_len,
			 uint16_t mtu_hint)
{
	struct icmp_err e;
	uint32_t ihl;

	if (!inner_ip || inner_len < IP_HDR_MIN + 8)
		return;
	ihl = (uint32_t)(inner_ip[0] & 0x0F) * 4;
	if (ihl < IP_HDR_MIN || inner_len < ihl + 8)
		return;
	mm_memset(&e, 0, sizeof(e));
	e.type = icmp_type;
	e.code = icmp_code;
	e.proto = inner_ip[9];
	e.saddr = rd32(inner_ip + 12);
	e.daddr = rd32(inner_ip + 16);
	e.from = e.saddr;
	e.iph = inner_ip;
	e.ihl = ihl;
	e.l4 = inner_ip + ihl;
	e.l4len = (uint32_t)inner_len - ihl;
	e.info = mtu_hint;
	icmp_err_dispatch(&e);
}

void icmp_init(void)
{
	icmp_rl_reset();
}
