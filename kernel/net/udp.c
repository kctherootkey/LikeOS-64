// LikeOS UDP (User Datagram Protocol, RFC 768, RFC 1122 4.1)
//
// Sockets are found through two hash tables: by local port (every socket
// on a port, for binding and for broadcast/multicast fan-out) and by local
// address + port (for unicast lookup).  A unicast datagram goes to the best
// match among the sockets bound to its exact destination address, or, when
// there is none, among those bound to the wildcard address; within a chain
// the score prefers a socket connected to the sender (its address and its
// port) and one bound to the receiving device.  A socket of an
// SO_REUSEPORT group (same port, same address, same owner, all with the
// option) stands for its group: the datagram goes to the member chosen by a
// hash of the 4-tuple, so one flow always reaches the same member.  A
// datagram to a broadcast or multicast address goes to every matching
// socket (a copy each).  No socket: ICMP port unreachable (not for
// broadcast/multicast), counted as NoPorts.
//
// Receiving: header length checked, checksum verified unless it is 0
// ("not computed", RFC 768), the datagram queued whole on the socket while
// its receive buffer (SO_RCVBUF, 212992 by default) is not exceeded.
//
// Sending: a datagram of at most 65507 bytes, routed per datagram (or as
// connected), DF as the socket's IP_MTU_DISCOVER mode says, SO_BROADCAST
// required for a broadcast destination, the checksum always computed (0
// goes out as 0xFFFF).  UDP_CORK and MSG_MORE collect several sends into
// one pending datagram that goes out when the cork is removed or a send
// comes without MSG_MORE.  The pending datagram's buffer is sized to what
// was appended (a full-frame buffer first, the 64 KB one only once the
// data outgrows it) and its memory is charged: a larger buffer is taken
// only while it stays within twice the socket's send buffer (SO_SNDBUF),
// and all pending datagrams together may pin at most UDP_CORK_MEM_MAX;
// beyond either the send fails with ENOBUFS.  A failed append discards
// the whole pending datagram, as a datagram cannot be sent in part.
//
// ICMP errors (udp_err) reach the socket that sent the datagram: a
// connected socket gets the hard ones as its pending error (ECONNREFUSED
// for port unreachable, ...); a socket with IP_RECVERR gets every error on
// its error queue (MSG_ERRQUEUE) and as its pending error.
//
// Lock order: udp_table_lock -> s->lock.  Delivery to a socket happens
// under udp_table_lock (an unhashed socket is never queued to); wakeups
// after it is dropped.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/cred.h>
#include <kernel/dev/rand/random.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/uapi/bug.h>

#define UDP_HTABLE_SIZE 256
#define UDP_PORT_LOW 49152u
#define UDP_PORT_RANGE 16384u
#define UDP_MAX_PAYLOAD (IP_MAX_PACKET - IP_HDR_MIN - 8u) // 65507

#define UDP_LOCK_ADDR 1
#define UDP_LOCK_PORT 2

struct udp_cork_state {
	sk_buff_t *skb; // the payload so far, 8 bytes of room in front
	uint32_t daddr;
	uint16_t dport;
	uint16_t _pad;
	uint32_t charged; // skb->truesize, charged to udp_cork_mem
	uint32_t _pad2;
};

/* Packet memory all pending (corked) datagrams together may hold.  The
 * packet pools are one budget for the whole machine: without a cap, a
 * program with many sockets could keep every large buffer pending and
 * starve reassembly and every other large datagram. */
#define UDP_CORK_MEM_MAX (8u << 20)
static volatile uint64_t udp_cork_mem;

static net_socket_t *udp_port_hash[UDP_HTABLE_SIZE]; // via s->hnext
static net_socket_t *udp_addr_hash[UDP_HTABLE_SIZE]; // via s->h2next
static uint32_t udp_nhashed;
static uint32_t udp_secret;
static spinlock_t udp_table_lock = SPINLOCK_INIT("udp_table");

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline int ipv4_is_multicast(uint32_t a)
{
	return (a & 0xF0000000u) == 0xE0000000u;
}

static inline uint32_t udp_hfn(uint16_t port)
{
	return ((uint32_t)port ^ ((uint32_t)port >> 8)) & (UDP_HTABLE_SIZE - 1);
}

static inline uint32_t udp_h2fn(uint32_t addr, uint16_t port)
{
	uint32_t h = (addr ^ udp_secret) * 0x9E3779B1u;

	h ^= (uint32_t)port * 0x85EBCA77u;
	h ^= h >> 16;
	return h & (UDP_HTABLE_SIZE - 1);
}

static inline uint16_t sk_lport(const net_socket_t *s)
{
	return net_ntohs(s->local_addr.sin_port);
}

static inline uint32_t sk_laddr(const net_socket_t *s)
{
	return net_ntohl(s->local_addr.sin_addr.s_addr);
}

static inline uint32_t sk_raddr(const net_socket_t *s)
{
	return s->connected ? net_ntohl(s->remote_addr.sin_addr.s_addr) : 0;
}

static inline uint16_t sk_rport(const net_socket_t *s)
{
	return s->connected ? net_ntohs(s->remote_addr.sin_port) : 0;
}

void udp_init(void)
{
	uint64_t f;
	uint32_t secret = random_u32();

	spin_lock_irqsave(&udp_table_lock, &f);
	mm_memset(udp_port_hash, 0, sizeof(udp_port_hash));
	mm_memset(udp_addr_hash, 0, sizeof(udp_addr_hash));
	udp_nhashed = 0;
	udp_secret = secret;
	spin_unlock_irqrestore(&udp_table_lock, f);
}

void udp_get_table_stats(struct udp_table_stats *st)
{
	uint64_t f;

	spin_lock_irqsave(&udp_table_lock, &f);
	st->sockets = udp_nhashed;
	st->max_chain = 0;
	for (int i = 0; i < UDP_HTABLE_SIZE; i++) {
		uint32_t n = 0;

		for (net_socket_t *s = udp_port_hash[i]; s; s = s->hnext)
			n++;
		if (n > st->max_chain)
			st->max_chain = n;
	}
	spin_unlock_irqrestore(&udp_table_lock, f);
}

// ---- the tables (udp_table_lock held) ----

static void udp_link2_locked(net_socket_t *s)
{
	net_socket_t **pp = &udp_addr_hash[udp_h2fn(sk_laddr(s), sk_lport(s))];

	while (*pp)
		pp = &(*pp)->h2next;
	s->h2next = NULL;
	*pp = s; // tail: a reuseport group keeps its join order
}

static void udp_unlink2_locked(net_socket_t *s)
{
	net_socket_t **pp = &udp_addr_hash[udp_h2fn(sk_laddr(s), sk_lport(s))];

	for (; *pp; pp = &(*pp)->h2next) {
		if (*pp == s) {
			*pp = s->h2next;
			break;
		}
	}
	s->h2next = NULL;
}

static void udp_hash_locked(net_socket_t *s)
{
	net_socket_t **pp = &udp_port_hash[udp_hfn(sk_lport(s))];

	while (*pp)
		pp = &(*pp)->hnext;
	s->hnext = NULL;
	*pp = s;
	udp_link2_locked(s);
	s->hashed = 1;
	udp_nhashed++;
}

static void udp_unhash_locked(net_socket_t *s)
{
	net_socket_t **pp = &udp_port_hash[udp_hfn(sk_lport(s))];
	int found = 0;

	for (; *pp; pp = &(*pp)->hnext) {
		if (*pp == s) {
			*pp = s->hnext;
			found = 1;
			break;
		}
	}
	s->hnext = NULL;
	if (found) {
		udp_unlink2_locked(s);
		udp_nhashed--;
	}
}

/* Move s to the chain of a new local address (connect fills it in). */
static void udp_rehash_locked(net_socket_t *s, uint32_t addr)
{
	udp_unlink2_locked(s);
	s->local_addr.sin_addr.s_addr = net_htonl(addr);
	udp_link2_locked(s);
}

/* Would s, bound to addr:port, clash with a socket already there? */
static int udp_port_conflict_locked(const net_socket_t *s, uint16_t port,
				    uint32_t addr)
{
	uint32_t euid = s->bind_euid;

	for (net_socket_t *o = udp_port_hash[udp_hfn(port)]; o; o = o->hnext) {
		uint32_t oaddr;

		if (o == s || sk_lport(o) != port)
			continue;
		if (o->reuse_addr && s->reuse_addr)
			continue;
		if (o->bound_ifindex && s->bound_ifindex &&
		    o->bound_ifindex != s->bound_ifindex)
			continue;
		oaddr = sk_laddr(o);
		if (oaddr && addr && oaddr != addr)
			continue; // different addresses never clash
		if (o->reuse_port && s->reuse_port && o->bind_euid == euid)
			continue; // the same owner's reuseport group
		return 1;
	}
	return 0;
}

/* Bind s to addr:port (port 0: an ephemeral one).  Table lock held.
 * `start' is where the ephemeral search begins (drawn by the caller
 * before taking the lock).  0 or -errno. */
static int udp_bind_locked(net_socket_t *s, uint32_t addr, uint16_t port,
			   uint32_t start)
{
	if (s->bound)
		return -EINVAL;
	if (port) {
		if (udp_port_conflict_locked(s, port, addr))
			return -EADDRINUSE;
	} else {
		/* Every candidate from a random start; the table holds at
		 * most NET_MAX_SOCKETS sockets, so a free port turns up
		 * within that many steps unless the range is full. */
		for (uint32_t i = 0; i < UDP_PORT_RANGE; i++) {
			uint16_t cand = (uint16_t)(UDP_PORT_LOW +
						   (start + i) % UDP_PORT_RANGE);

			if (!udp_port_conflict_locked(s, cand, addr)) {
				port = cand;
				break;
			}
		}
		if (!port)
			return -EADDRINUSE;
	}
	s->local_addr.sin_family = AF_INET;
	s->local_addr.sin_port = net_htons(port);
	s->local_addr.sin_addr.s_addr = net_htonl(addr);
	// sending from a group or broadcast address uses the device's
	s->udp_saddr =
		(ipv4_is_multicast(addr) || ip_addr_is_broadcast(addr)) ? 0 :
									   addr;
	udp_hash_locked(s);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	s->bound = 1;
	return 0;
}

int udp_bind(net_socket_t *s, const struct sockaddr_in *a)
{
	uint32_t addr = net_ntohl(a->sin_addr.s_addr);
	uint16_t port = net_ntohs(a->sin_port);
	uint32_t start = random_u32();
	uint64_t f;
	int rc;

	if (a->sin_family != AF_INET && a->sin_family != 0)
		return -EAFNOSUPPORT;
	if (addr && !ip_addr_is_local(addr) && !ipv4_is_multicast(addr) &&
	    !ip_addr_is_broadcast(addr))
		return -EADDRNOTAVAIL;
	if (port && port < 1024 && !capable())
		return -EACCES;
	spin_lock_irqsave(&udp_table_lock, &f);
	s->bind_euid = current_euid();
	rc = udp_bind_locked(s, addr, port, start);
	if (rc == 0)
		s->udp_userlocks = (uint8_t)((addr ? UDP_LOCK_ADDR : 0) |
					     (port ? UDP_LOCK_PORT : 0));
	spin_unlock_irqrestore(&udp_table_lock, f);
	return rc;
}

/* Bind to the wildcard address and an ephemeral port when not bound. */
static int udp_autobind(net_socket_t *s)
{
	uint32_t start;
	uint64_t f;
	int rc = 0;

	if (__atomic_load_n(&s->bound, __ATOMIC_ACQUIRE))
		return 0;
	start = random_u32();
	spin_lock_irqsave(&udp_table_lock, &f);
	if (!s->bound) {
		s->bind_euid = current_euid();
		rc = udp_bind_locked(s, 0, 0, start);
	}
	spin_unlock_irqrestore(&udp_table_lock, f);
	return rc;
}

void udp_unhash(net_socket_t *s)
{
	uint64_t f;

	spin_lock_irqsave(&udp_table_lock, &f);
	udp_unhash_locked(s);
	spin_unlock_irqrestore(&udp_table_lock, f);
}

// ---- lookup (udp_table_lock held) ----

static int udp_score(const net_socket_t *s, uint32_t saddr, uint16_t sport,
		     uint32_t key_addr, uint16_t dport, int ifindex)
{
	int score = 2;

	if (!s->active || s->closed || s->type != SOCK_DGRAM ||
	    sk_lport(s) != dport || sk_laddr(s) != key_addr)
		return -1;
	if (s->connected) {
		if (sk_raddr(s) != saddr)
			return -1;
		score += 4;
		if (sk_rport(s)) {
			if (sk_rport(s) != sport)
				return -1;
			score += 4;
		}
	}
	if (s->bound_ifindex) {
		if (ifindex && s->bound_ifindex != ifindex)
			return -1;
		score += 4;
	}
	return score;
}

static uint32_t udp_flow_hash(uint32_t saddr, uint16_t sport, uint32_t daddr,
			      uint16_t dport)
{
	uint32_t h = saddr ^ udp_secret;

	h = (h ^ (h >> 16)) * 0x7FEB352Du;
	h ^= daddr;
	h = (h ^ (h >> 15)) * 0x846CA68Bu;
	h ^= ((uint32_t)sport << 16) | dport;
	h = (h ^ (h >> 16)) * 0x7FEB352Du;
	return h ^ (h >> 15);
}

/* The member of s's reuseport group the flow goes to. */
static net_socket_t *udp_reuseport_select(net_socket_t *s, uint32_t hash)
{
	uint32_t n = 0, pick;
	net_socket_t *o;

	for (o = s; o; o = o->h2next)
		if (o->reuse_port && !o->connected && o->active &&
		    !o->closed && sk_lport(o) == sk_lport(s) &&
		    sk_laddr(o) == sk_laddr(s) && o->bind_euid == s->bind_euid &&
		    o->bound_ifindex == s->bound_ifindex)
			n++;
	if (n <= 1)
		return s;
	pick = hash % n;
	for (o = s; o; o = o->h2next)
		if (o->reuse_port && !o->connected && o->active &&
		    !o->closed && sk_lport(o) == sk_lport(s) &&
		    sk_laddr(o) == sk_laddr(s) && o->bind_euid == s->bind_euid &&
		    o->bound_ifindex == s->bound_ifindex && pick-- == 0)
			return o;
	return s;
}

static net_socket_t *udp_lookup2_locked(uint32_t saddr, uint16_t sport,
					uint32_t daddr, uint16_t dport,
					uint32_t key_addr, int ifindex)
{
	net_socket_t *result = NULL;
	int badness = 0;

	for (net_socket_t *s = udp_addr_hash[udp_h2fn(key_addr, dport)]; s;
	     s = s->h2next) {
		int score = udp_score(s, saddr, sport, key_addr, dport, ifindex);

		if (score > badness) {
			badness = score;
			result = s;
			if (s->reuse_port && !s->connected)
				return udp_reuseport_select(
					s, udp_flow_hash(saddr, sport, daddr,
							 dport));
		}
	}
	return result;
}

/* The socket a datagram from saddr:sport to daddr:dport belongs to: one
 * bound to daddr first, the wildcard ones after. */
static net_socket_t *udp_lookup_locked(uint32_t saddr, uint16_t sport,
				       uint32_t daddr, uint16_t dport,
				       int ifindex)
{
	net_socket_t *s = udp_lookup2_locked(saddr, sport, daddr, dport, daddr,
					     ifindex);

	if (s || !daddr)
		return s;
	return udp_lookup2_locked(saddr, sport, daddr, dport, 0, ifindex);
}

static int udp_mcast_match(const net_socket_t *s, uint32_t saddr,
			   uint16_t sport, uint32_t daddr, uint16_t dport,
			   int ifindex)
{
	uint32_t laddr;

	if (!s->active || s->closed || s->type != SOCK_DGRAM ||
	    sk_lport(s) != dport)
		return 0;
	if (s->connected && (sk_raddr(s) != saddr ||
			     (sk_rport(s) && sk_rport(s) != sport)))
		return 0;
	laddr = sk_laddr(s);
	if (laddr && laddr != daddr)
		return 0;
	if (s->bound_ifindex && s->bound_ifindex != ifindex)
		return 0;
	if (ipv4_is_multicast(daddr) && !s->mc_all) {
		// only the groups this socket joined itself
		int joined = 0;

		for (int i = 0; i < NET_MAX_MCAST_GROUPS; i++)
			if (s->mcast_groups[i] == daddr)
				joined = 1;
		if (!joined)
			return 0;
	}
	return 1;
}

// ---- receive ----

/* The socket's view of a datagram, written over the IPCB (read into
 * `ip' before) once the datagram is about to be queued. */
static void udp_fill_cb(sk_buff_t *skb, const struct inet_skb_parm *ip,
			uint16_t sport)
{
	struct dgram_cb d;

	d.saddr = ip->saddr;
	d.sport = sport;
	d.ttl = ip->ttl;
	d.tos = ip->tos;
	d.daddr = ip->daddr;
	d.ifindex = ip->iif;
	d._pad = 0;
	mm_memcpy(DGRAM_CB(skb), &d, sizeof(d));
}

/* Broadcast or multicast: a copy for every matching socket. */
static void udp_mcast_deliver(sk_buff_t *skb, uint16_t sport, uint16_t dport)
{
	struct inet_skb_parm *cb = IPCB(skb);
	uint64_t woken[NET_MAX_SOCKETS / 64];
	int any = 0, matched = 0;
	uint64_t f;

	mm_memset(woken, 0, sizeof(woken));
	spin_lock_irqsave(&udp_table_lock, &f);
	for (net_socket_t *s = udp_port_hash[udp_hfn(dport)]; s;
	     s = s->hnext) {
		sk_buff_t *c;
		int idx;

		if (!udp_mcast_match(s, cb->saddr, sport, cb->daddr, dport,
				     cb->iif))
			continue;
		matched = 1;
		c = skb_clone(skb);
		if (!c) {
			NET_STATS_INC(NET_MIB_UDP_RCVBUFERRORS);
			NET_STATS_INC(NET_MIB_UDP_INERRORS);
			continue;
		}
		c->truesize = skb->truesize; // it pins the whole buffer
		udp_fill_cb(c, cb, sport);
		if (dgram_enqueue(s, c) < 0) {
			NET_STATS_INC(NET_MIB_UDP_RCVBUFERRORS);
			NET_STATS_INC(NET_MIB_UDP_INERRORS);
			skb_put(c);
			continue;
		}
		NET_STATS_INC(NET_MIB_UDP_INDATAGRAMS);
		idx = sock_slot(s);
		if (idx >= 0) {
			woken[idx / 64] |= 1ULL << (idx % 64);
			any = 1;
		}
	}
	spin_unlock_irqrestore(&udp_table_lock, f);
	if (any)
		sock_wake_slots(woken);
	if (!matched)
		NET_STATS_INC(NET_MIB_UDP_IGNOREDMULTI);
	skb_put(skb);
}

int udp_rcv(sk_buff_t *skb)
{
	struct inet_skb_parm *cb = IPCB(skb);
	const uint8_t *uh;
	uint16_t ulen, sport, dport;
	net_socket_t *s;
	uint64_t f;
	int rc;

	if (skb->len < 8)
		goto short_packet;
	uh = skb->data;
	ulen = rd16(uh + 4);
	if (ulen < 8 || ulen > skb->len)
		goto short_packet;
	skb_trim(skb, ulen);
	// checksum 0: the sender did not compute one (RFC 768)
	if ((uh[6] || uh[7]) && skb->ip_summed != CHECKSUM_UNNECESSARY) {
		uint32_t sum = csum_tcpudp_nofold(cb->saddr, cb->daddr, ulen,
						  IP_PROTO_UDP,
						  csum_partial(uh, ulen, 0));

		if (csum_fold(sum) != 0) {
			NET_STATS_INC(NET_MIB_UDP_INCSUMERRORS);
			NET_STATS_INC(NET_MIB_UDP_INERRORS);
			skb_put(skb);
			return 0;
		}
	}
	sport = rd16(uh);
	dport = rd16(uh + 2);

	// the kernel's own clients
	if (dport == DHCP_CLIENT_PORT) {
		NET_STATS_INC(NET_MIB_UDP_INDATAGRAMS);
		dhcp_rx(skb->dev, uh + 8, (uint16_t)(ulen - 8));
		skb_put(skb);
		return 0;
	}
	if (dport == DNS_CLIENT_PORT && sport == 53) {
		NET_STATS_INC(NET_MIB_UDP_INDATAGRAMS);
		dns_rx(uh + 8, (uint16_t)(ulen - 8));
		skb_put(skb);
		return 0;
	}

	skb_pull(skb, 8);
	if (cb->rt_type == RTN_BROADCAST || cb->rt_type == RTN_MULTICAST) {
		udp_mcast_deliver(skb, sport, dport);
		return 0;
	}
	spin_lock_irqsave(&udp_table_lock, &f);
	s = udp_lookup_locked(cb->saddr, sport, cb->daddr, dport, cb->iif);
	if (s) {
		struct inet_skb_parm ip = *cb;

		udp_fill_cb(skb, &ip, sport);
		rc = dgram_enqueue(s, skb);
	} else {
		rc = -1;
	}
	spin_unlock_irqrestore(&udp_table_lock, f);
	if (s) {
		if (rc == 0) {
			NET_STATS_INC(NET_MIB_UDP_INDATAGRAMS);
			dgram_wake(s);
			return 0;
		}
		NET_STATS_INC(NET_MIB_UDP_RCVBUFERRORS);
		NET_STATS_INC(NET_MIB_UDP_INERRORS);
		skb_put(skb);
		return 0;
	}
	NET_STATS_INC(NET_MIB_UDP_NOPORTS);
	icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, 0);
	skb_put(skb);
	return 0;

short_packet:
	NET_STATS_INC(NET_MIB_UDP_INERRORS);
	skb_put(skb);
	return 0;
}

void udp_err(const struct icmp_err *e)
{
	uint16_t sport = rd16(e->l4), dport = rd16(e->l4 + 2);
	int ifindex = e->skb ? IPCB(e->skb)->iif : 0;
	int err = e->err, harderr = e->fatal;
	uint32_t info = e->info;
	net_socket_t *s;
	uint64_t f;
	int wake = 0;

	switch (e->type) {
	case ICMP_PARAMETERPROB:
		err = EPROTO;
		harderr = 1;
		break;
	case ICMP_DEST_UNREACH:
		if (e->code == ICMP_FRAG_NEEDED) {
			err = EMSGSIZE;
			harderr = 1; // when the socket discovers the PMTU
		}
		break;
	case ICMP_TIME_EXCEEDED:
		err = EHOSTUNREACH;
		harderr = 0;
		break;
	default:
		return;
	}
	spin_lock_irqsave(&udp_table_lock, &f);
	/* The socket that sent the quoted datagram, found as a datagram from
	 * its destination back to it would be.  A report that matches no
	 * socket changes nothing, not even the path MTU. */
	s = udp_lookup_locked(e->daddr, dport, e->saddr, sport, ifindex);
	if (s && e->type == ICMP_DEST_UNREACH && e->code == ICMP_FRAG_NEEDED) {
		int locked;

		// one of ours: the path towards its destination is narrower
		if (ip_sk_accept_pmtu(s->pmtudisc))
			(void)ip_update_pmtu(e->daddr, info, &locked);
		if (s->pmtudisc == IP_PMTUDISC_DONT)
			s = NULL; // fragments anyway: nothing to report
	}
	if (s && (s->ip_recverr || (harderr && s->connected))) {
		ip_queue_error(s, err, SO_EE_ORIGIN_ICMP, e->type, e->code,
			       info, e->from, e->daddr, dport,
			       e->l4 + 8, e->l4len - 8, 1);
		wake = 1;
	}
	spin_unlock_irqrestore(&udp_table_lock, f);
	if (wake)
		dgram_wake(s);
}

// ---- send ----

static void udp_tx_lock(net_socket_t *s)
{
	for (;;) {
		int expected = 0;

		if (__atomic_compare_exchange_n(&s->tx_owner, &expected, 1,
						false, __ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return;
		sched_yield_in_kernel();
	}
}

static void udp_tx_unlock(net_socket_t *s)
{
	__atomic_store_n(&s->tx_owner, 0, __ATOMIC_RELEASE);
}

/* The output parameters of socket s towards daddr. */
static void udp_fill_tx(net_socket_t *s, struct ip_tx *tx, uint32_t daddr,
			uint32_t saddr, net_device_t *oif)
{
	mm_memset(tx, 0, sizeof(*tx));
	tx->saddr = saddr;
	tx->daddr = daddr;
	tx->oif = oif;
	tx->proto = IP_PROTO_UDP;
	tx->ttl = s->ip_ttl;
	tx->tos = s->ip_tos;
	tx->pmtudisc = s->pmtudisc;
	tx->mc_ttl = s->mcast_ttl;
	tx->mc_loop = s->mcast_loop;
	/* A connected socket numbers its own datagrams (sent to its
	 * peer); anything else draws from the per-destination counters. */
	if (s->connected && daddr == sk_raddr(s))
		tx->idp = &s->inet_id;
}

static net_device_t *udp_oif(net_socket_t *s, uint32_t daddr)
{
	if (s->bound_ifindex)
		return ip_dev_by_index(s->bound_ifindex);
	if (ipv4_is_multicast(daddr) && s->mcast_if)
		return ip_dev_by_addr(s->mcast_if);
	return NULL;
}

/* Put the UDP header in front of the payload at skb and send it.
 * Consumes skb.  0 or -errno. */
static int udp_send_skb(net_socket_t *s, sk_buff_t *skb, uint16_t sport,
			uint32_t daddr, uint16_t dport, struct ip_route *rt,
			const struct ip_tx *tx)
{
	uint32_t ulen = skb->len + 8, saddr, mtu = 0;
	uint8_t *uh;
	uint16_t c;
	int rc;

	uh = skb_push(skb, 8);
	uh[0] = (uint8_t)(sport >> 8);
	uh[1] = (uint8_t)sport;
	uh[2] = (uint8_t)(dport >> 8);
	uh[3] = (uint8_t)dport;
	uh[4] = (uint8_t)(ulen >> 8);
	uh[5] = (uint8_t)ulen;
	uh[6] = uh[7] = 0;
	saddr = tx->saddr ? tx->saddr : rt->saddr;
	c = csum_fold(csum_tcpudp_nofold(saddr, daddr, (uint16_t)ulen,
					 IP_PROTO_UDP,
					 csum_partial(uh, ulen, 0)));
	if (c == 0)
		c = 0xFFFF; // 0 would mean "no checksum"
	mm_memcpy(uh + 6, &c, 2);
	rc = ip_send_skb(skb, tx, rt, &mtu);
	if (rc == -EMSGSIZE && s && s->ip_recverr)
		ip_queue_error(s, EMSGSIZE, SO_EE_ORIGIN_LOCAL, 0, 0, mtu, 0,
			       daddr, dport, NULL, 0, 0);
	if (rc == 0) {
		NET_STATS_INC(NET_MIB_UDP_OUTDATAGRAMS);
	} else if (rc == -ENOBUFS) {
		NET_STATS_INC(NET_MIB_UDP_SNDBUFERRORS);
		if (!s || !s->ip_recverr)
			rc = 0; // a lost datagram, as the network may lose one
	}
	return rc;
}

/* Charge a pending datagram's buffer of `truesize' bytes that replaces
 * one of `old' bytes (0: the first).  A larger buffer than the first is
 * allowed while it stays within twice the send buffer -- what a datagram
 * may hold beyond its first allocation -- and only while all pending
 * datagrams stay within UDP_CORK_MEM_MAX.  0 or -ENOBUFS. */
static int udp_cork_charge(net_socket_t *s, uint32_t old, uint32_t truesize)
{
	uint64_t total;

	if (truesize <= old)
		return 0;
	if (old && (uint64_t)truesize > 2ULL * (uint32_t)s->sndbuf_size)
		return -ENOBUFS;
	total = __atomic_add_fetch(&udp_cork_mem, truesize - old,
				   __ATOMIC_ACQ_REL);
	if (total > UDP_CORK_MEM_MAX) {
		__atomic_sub_fetch(&udp_cork_mem, truesize - old,
				   __ATOMIC_ACQ_REL);
		return -ENOBUFS;
	}
	return 0;
}

static void udp_cork_uncharge(struct udp_cork_state *ck)
{
	if (ck->charged) {
		__atomic_sub_fetch(&udp_cork_mem, ck->charged,
				   __ATOMIC_ACQ_REL);
		ck->charged = 0;
	}
}

/* Drop the pending datagram (tx owned). */
static void udp_flush_pending(net_socket_t *s)
{
	struct udp_cork_state *ck = s->cork;

	if (!ck || !ck->skb)
		return;
	skb_put(ck->skb);
	ck->skb = NULL;
	udp_cork_uncharge(ck);
}

/* Send what a cork collected (tx owned).  0 or -errno. */
static int udp_push_pending(net_socket_t *s)
{
	struct udp_cork_state *ck = s->cork;
	struct ip_route rt;
	struct ip_tx tx;
	sk_buff_t *skb;
	int rc;

	if (!ck || !ck->skb)
		return 0;
	skb = ck->skb;
	ck->skb = NULL;
	udp_cork_uncharge(ck);
	rc = ip_route_out(ck->daddr, s->udp_saddr, udp_oif(s, ck->daddr), &rt);
	if (rc < 0) {
		NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
		skb_put(skb);
		return rc;
	}
	udp_fill_tx(s, &tx, ck->daddr, s->udp_saddr, udp_oif(s, ck->daddr));
	rc = udp_send_skb(s, skb, sk_lport(s), ck->daddr, ck->dport, &rt, &tx);
	ip_route_put(&rt);
	return rc;
}

static int udp_copy_from(uint8_t *dst, const void *src, size_t len)
{
	if (!len)
		return 0;
	/* User memory (or a kernel bounce buffer of sendmsg): the send
	 * path reads it directly, nothing held. */
	smap_disable();
	mm_memcpy(dst, src, len);
	smap_enable();
	return 0;
}

int udp_sendmsg(net_socket_t *s, const void *buf, size_t len, int flags,
		const struct sockaddr_in *dest)
{
	int corkreq = s->udp_cork || (flags & MSG_MORE);
	struct ip_route rt;
	struct ip_tx tx;
	uint32_t daddr;
	uint16_t dport;
	sk_buff_t *skb;
	int rc;

	might_sleep();
	if (flags & MSG_OOB)
		return -EOPNOTSUPP;
	if (len > UDP_MAX_PAYLOAD)
		return -EMSGSIZE;
	udp_tx_lock(s);
	if (s->cork && s->cork->skb) {
		// a pending datagram: append (its destination stays)
		struct udp_cork_state *ck = s->cork;

		if (ck->skb->len + len > UDP_MAX_PAYLOAD) {
			udp_flush_pending(s);
			rc = -EMSGSIZE;
			goto out;
		}
		if (skb_tailroom(ck->skb) < len) {
			/* Outgrown its buffer: move to one that holds a
			 * whole datagram (the 64 KB class). */
			sk_buff_t *n = skb_copy_expand(
				ck->skb, skb_headroom(ck->skb),
				UDP_MAX_PAYLOAD - ck->skb->len);

			if (!n || udp_cork_charge(s, ck->charged,
						  n->truesize) < 0) {
				skb_put(n);
				udp_flush_pending(s);
				NET_STATS_INC(NET_MIB_UDP_SNDBUFERRORS);
				rc = -ENOBUFS;
				goto out;
			}
			if (n->truesize < ck->charged) // never smaller
				__atomic_sub_fetch(&udp_cork_mem,
						   ck->charged - n->truesize,
						   __ATOMIC_ACQ_REL);
			ck->charged = n->truesize;
			skb_put(ck->skb);
			ck->skb = n;
		}
		udp_copy_from(skb_append(ck->skb, (uint32_t)len), buf, len);
		rc = corkreq ? 0 : udp_push_pending(s);
		if (rc == 0)
			rc = (int)len;
		goto out;
	}
	if (dest) {
		if (dest->sin_family != AF_INET && dest->sin_family != 0) {
			rc = -EAFNOSUPPORT;
			goto out;
		}
		daddr = net_ntohl(dest->sin_addr.s_addr);
		dport = net_ntohs(dest->sin_port);
		if (dport == 0) {
			rc = -EINVAL;
			goto out;
		}
	} else {
		if (!s->connected) {
			rc = -EDESTADDRREQ;
			goto out;
		}
		daddr = sk_raddr(s);
		dport = sk_rport(s);
	}
	rc = ip_route_out(daddr, s->udp_saddr, udp_oif(s, daddr), &rt);
	if (rc < 0) {
		NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
		goto out;
	}
	if (rt.type == RTN_BROADCAST && !s->broadcast) {
		ip_route_put(&rt);
		rc = -EACCES;
		goto out;
	}
	rc = udp_autobind(s);
	if (rc < 0) {
		ip_route_put(&rt);
		goto out;
	}
	/* Sized to this send also when more follows: a pending datagram
	 * starts in the smallest buffer class that holds it, all of whose
	 * room it asks for (appends go there), and grows with what is
	 * appended (udp_cork_charge). */
	skb = skb_alloc(!corkreq ? (uint32_t)len :
			len <= SKB_SMALL_DATA - SKB_HEADROOM ?
				   SKB_SMALL_DATA - SKB_HEADROOM :
				   UDP_MAX_PAYLOAD);
	if (!skb) {
		ip_route_put(&rt);
		NET_STATS_INC(NET_MIB_UDP_SNDBUFERRORS);
		rc = -ENOBUFS;
		goto out;
	}
	udp_copy_from(skb_append(skb, (uint32_t)len), buf, len);
	if (corkreq) {
		if (!s->cork) {
			s->cork = (struct udp_cork_state *)kalloc(
				sizeof(*s->cork));
			if (!s->cork) {
				ip_route_put(&rt);
				skb_put(skb);
				rc = -ENOBUFS;
				goto out;
			}
			mm_memset(s->cork, 0, sizeof(*s->cork));
		}
		if (udp_cork_charge(s, 0, skb->truesize) < 0) {
			ip_route_put(&rt);
			skb_put(skb);
			NET_STATS_INC(NET_MIB_UDP_SNDBUFERRORS);
			rc = -ENOBUFS;
			goto out;
		}
		s->cork->charged = skb->truesize;
		s->cork->skb = skb;
		s->cork->daddr = daddr;
		s->cork->dport = dport;
		ip_route_put(&rt);
		rc = (int)len;
		goto out;
	}
	udp_fill_tx(s, &tx, daddr, s->udp_saddr, udp_oif(s, daddr));
	rc = udp_send_skb(s, skb, sk_lport(s), daddr, dport, &rt, &tx);
	ip_route_put(&rt);
	if (rc == 0)
		rc = (int)len;
out:
	udp_tx_unlock(s);
	return rc;
}

int udp_connect(net_socket_t *s, const struct sockaddr_in *addr)
{
	struct ip_route rt;
	uint32_t daddr, id;
	uint64_t f;
	int rc;

	if (addr->sin_family == 0) {
		// AF_UNSPEC: dissolve the association
		spin_lock_irqsave(&udp_table_lock, &f);
		s->connected = 0;
		mm_memset(&s->remote_addr, 0, sizeof(s->remote_addr));
		s->remote_addr.sin_family = AF_INET;
		if (s->bound && !(s->udp_userlocks & UDP_LOCK_ADDR)) {
			udp_rehash_locked(s, 0);
			s->udp_saddr = 0;
		}
		if (s->bound && !(s->udp_userlocks & UDP_LOCK_PORT)) {
			udp_unhash_locked(s);
			s->hashed = 0;
			s->bound = 0;
			s->local_addr.sin_port = 0;
			s->local_addr.sin_addr.s_addr = 0;
		}
		spin_unlock_irqrestore(&udp_table_lock, f);
		return 0;
	}
	if (addr->sin_family != AF_INET)
		return -EAFNOSUPPORT;
	daddr = net_ntohl(addr->sin_addr.s_addr);
	rc = ip_route_out(daddr, s->udp_saddr, udp_oif(s, daddr), &rt);
	if (rc < 0)
		return rc;
	if (rt.type == RTN_BROADCAST && !s->broadcast) {
		ip_route_put(&rt);
		return -EACCES;
	}
	rc = udp_autobind(s);
	if (rc < 0) {
		ip_route_put(&rt);
		return rc;
	}
	id = random_u32();
	spin_lock_irqsave(&udp_table_lock, &f);
	if (!s->udp_saddr && !ipv4_is_multicast(rt.saddr))
		s->udp_saddr = rt.saddr;
	if (!sk_laddr(s) && s->udp_saddr)
		udp_rehash_locked(s, s->udp_saddr);
	s->remote_addr.sin_family = AF_INET;
	s->remote_addr.sin_addr.s_addr = addr->sin_addr.s_addr;
	s->remote_addr.sin_port = addr->sin_port;
	s->inet_id = (uint16_t)id;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	s->connected = 1;
	spin_unlock_irqrestore(&udp_table_lock, f);
	ip_route_put(&rt);
	return 0;
}

int udp_setsockopt(net_socket_t *s, int optname, int val)
{
	int rc = 0;

	switch (optname) {
	case UDP_CORK:
		udp_tx_lock(s);
		s->udp_cork = val ? 1 : 0;
		if (!val)
			rc = udp_push_pending(s);
		udp_tx_unlock(s);
		return rc < 0 ? rc : 0;
	default:
		return -ENOPROTOOPT;
	}
}

int udp_getsockopt(net_socket_t *s, int optname, int *val)
{
	switch (optname) {
	case UDP_CORK:
		*val = s->udp_cork;
		return 0;
	default:
		return -ENOPROTOOPT;
	}
}

void udp_destroy(net_socket_t *s)
{
	struct udp_cork_state *ck;

	udp_unhash(s);
	udp_tx_lock(s);
	ck = s->cork;
	s->cork = NULL;
	udp_tx_unlock(s);
	if (ck) {
		skb_put(ck->skb);
		udp_cork_uncharge(ck);
		kfree(ck);
	}
}

// ---- the kernel's own datagrams (DHCP, DNS) ----

int udp_send(net_device_t *dev, uint32_t dst_ip, uint16_t src_port,
	     uint16_t dst_port, const uint8_t *data, uint16_t len)
{
	struct ip_route rt;
	struct ip_tx tx;
	sk_buff_t *skb;
	int rc;

	BUG_ON(dev == NULL);
	BUILD_BUG_ON(sizeof(udp_header_t) != 8);
	WARN_ON(src_port == 0);
	WARN_ON(dst_port == 0);
	if (!dev || len > UDP_MAX_PAYLOAD)
		return -1;
	rc = ip_route_out(dst_ip, 0, dev, &rt);
	if (rc < 0) {
		NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
		return -1;
	}
	skb = skb_alloc(len);
	if (!skb) {
		ip_route_put(&rt);
		return -1;
	}
	if (len)
		mm_memcpy(skb_append(skb, len), data, len);
	mm_memset(&tx, 0, sizeof(tx));
	tx.daddr = dst_ip;
	tx.oif = dev;
	tx.proto = IP_PROTO_UDP;
	tx.ttl = IP_DEFAULT_TTL;
	tx.mc_ttl = 1;
	tx.pmtudisc = IP_PMTUDISC_DONT;
	rc = udp_send_skb(NULL, skb, src_port, dst_ip, dst_port, &rt, &tx);
	ip_route_put(&rt);
	return rc < 0 ? -1 : 0;
}
