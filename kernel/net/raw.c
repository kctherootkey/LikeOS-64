// LikeOS — raw IPv4 sockets (SOCK_RAW)
//
// A raw socket of protocol P receives a copy of every datagram of protocol
// P delivered to this host (the whole datagram, IP header included), after
// reassembly and before the protocol itself sees it; it may filter by a
// bound local address and a connected peer.  IPPROTO_RAW sockets only
// send (with IP_HDRINCL implied) and receive nothing, as everywhere.
// Sockets are found through a table indexed by protocol, so delivery does
// not look at any socket of another protocol.
//
// ICMP errors about datagrams of protocol P reach P's raw sockets that are
// connected or asked for them (IP_RECVERR).
//
// Lock order: raw_lock -> s->lock.  Wakeups happen after raw_lock is
// dropped.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/uapi/bug.h>

static net_socket_t *raw_table[256];
static spinlock_t raw_lock = SPINLOCK_INIT("raw_table");

void raw_init(void)
{
	uint64_t f;

	spin_lock_irqsave(&raw_lock, &f);
	mm_memset(raw_table, 0, sizeof(raw_table));
	spin_unlock_irqrestore(&raw_lock, f);
}

void raw_hash(net_socket_t *s)
{
	uint8_t proto = (uint8_t)s->protocol;
	net_socket_t **pp;
	uint64_t f;

	spin_lock_irqsave(&raw_lock, &f);
	s->hnext = NULL;
	for (pp = &raw_table[proto]; *pp; pp = &(*pp)->hnext)
		;
	*pp = s; // in creation order
	s->hashed = 1;
	spin_unlock_irqrestore(&raw_lock, f);
}

void raw_unhash(net_socket_t *s)
{
	uint8_t proto = (uint8_t)s->protocol;
	uint64_t f;

	spin_lock_irqsave(&raw_lock, &f);
	for (net_socket_t **pp = &raw_table[proto]; *pp; pp = &(*pp)->hnext) {
		if (*pp == s) {
			*pp = s->hnext;
			break;
		}
	}
	s->hnext = NULL;
	spin_unlock_irqrestore(&raw_lock, f);
}

static int raw_match(const net_socket_t *s, uint32_t laddr, uint32_t raddr,
		     int ifindex)
{
	uint32_t bound = net_ntohl(s->local_addr.sin_addr.s_addr);

	if (!s->active || s->closed)
		return 0;
	if (bound && bound != laddr)
		return 0;
	if (s->connected &&
	    net_ntohl(s->remote_addr.sin_addr.s_addr) != raddr)
		return 0;
	if (s->bound_ifindex && s->bound_ifindex != ifindex)
		return 0;
	return 1;
}

int raw_local_deliver(sk_buff_t *skb, uint8_t proto)
{
	struct inet_skb_parm *cb = IPCB(skb);
	uint64_t woken[NET_MAX_SOCKETS / 64];
	int delivered = 0, any = 0;
	uint64_t f;

	if (proto == IPPROTO_RAW || !__atomic_load_n(&raw_table[proto],
						    __ATOMIC_ACQUIRE))
		return 0;
	mm_memset(woken, 0, sizeof(woken));
	spin_lock_irqsave(&raw_lock, &f);
	for (net_socket_t *s = raw_table[proto]; s; s = s->hnext) {
		sk_buff_t *c;
		struct dgram_cb *d;
		int idx;

		if (!raw_match(s, cb->daddr, cb->saddr, cb->iif))
			continue;
		c = skb_clone(skb);
		if (!c) {
			NET_STATS_INC(NET_MIB_RAW_DROPS);
			continue;
		}
		c->truesize = skb->truesize; // it pins the whole buffer
		// the whole datagram, from the IP header
		skb_push(c, cb->ihl);
		d = DGRAM_CB(c); // the clone's own cb; skb's IPCB stays
		d->saddr = cb->saddr;
		d->sport = 0;
		d->daddr = cb->daddr;
		d->ttl = cb->ttl;
		d->tos = cb->tos;
		d->ifindex = cb->iif;
		d->_pad = 0;
		if (dgram_enqueue(s, c) < 0) {
			NET_STATS_INC(NET_MIB_RAW_DROPS);
			skb_put(c);
			continue;
		}
		delivered = 1;
		idx = sock_slot(s);
		if (idx >= 0) {
			woken[idx / 64] |= 1ULL << (idx % 64);
			any = 1;
		}
	}
	spin_unlock_irqrestore(&raw_lock, f);
	if (any)
		sock_wake_slots(woken);
	return delivered;
}

void raw_icmp_error(const struct icmp_err *e)
{
	uint64_t woken[NET_MAX_SOCKETS / 64];
	int any = 0, pmtu_done = 0;
	int ifindex = e->skb ? IPCB(e->skb)->iif : 0;
	uint64_t f;

	if (!__atomic_load_n(&raw_table[e->proto], __ATOMIC_ACQUIRE))
		return;
	mm_memset(woken, 0, sizeof(woken));
	spin_lock_irqsave(&raw_lock, &f);
	for (net_socket_t *s = raw_table[e->proto]; s; s = s->hnext) {
		int err, harderr = 0, idx;

		if (!raw_match(s, e->saddr, e->daddr, ifindex))
			continue;
		/* A socket that may have sent the quoted datagram (its
		 * protocol, and its bound and connected addresses allow it)
		 * and takes path-MTU reports: the path towards the quoted
		 * destination is narrower.  Once per report. */
		if (e->type == ICMP_DEST_UNREACH &&
		    e->code == ICMP_FRAG_NEEDED && !pmtu_done &&
		    ip_sk_accept_pmtu(s->pmtudisc)) {
			int locked;

			(void)ip_update_pmtu(e->daddr, e->info, &locked);
			pmtu_done = 1;
		}
		if (!s->ip_recverr && !s->connected)
			continue;
		switch (e->type) {
		case ICMP_PARAMETERPROB:
			err = EPROTO;
			harderr = 1;
			break;
		case ICMP_DEST_UNREACH:
			err = EHOSTUNREACH;
			if (e->code > NR_ICMP_UNREACH)
				break;
			if (e->code == ICMP_FRAG_NEEDED) {
				harderr = s->pmtudisc != IP_PMTUDISC_DONT;
				err = EMSGSIZE;
			} else {
				err = e->err;
				harderr = e->fatal;
			}
			break;
		default: // time exceeded
			err = EHOSTUNREACH;
			break;
		}
		if (!s->ip_recverr && !harderr)
			continue;
		ip_queue_error(s, err, SO_EE_ORIGIN_ICMP, e->type, e->code,
			       e->info, e->from, e->daddr, 0, e->iph,
			       e->ihl + e->l4len, 1);
		idx = sock_slot(s);
		if (idx >= 0) {
			woken[idx / 64] |= 1ULL << (idx % 64);
			any = 1;
		}
	}
	spin_unlock_irqrestore(&raw_lock, f);
	if (any)
		sock_wake_slots(woken);
}

int raw_sendmsg(net_socket_t *s, const void *buf, size_t len,
		const struct sockaddr_in *dest)
{
	const struct sockaddr_in *target = dest;
	struct ip_route rt;
	uint32_t daddr;
	uint8_t *kbuf;
	int rc;

	if (!target) {
		if (!s->connected)
			return -EDESTADDRREQ;
		target = &s->remote_addr;
	}
	if (target->sin_family != AF_INET && target->sin_family != 0)
		return -EAFNOSUPPORT;
	if (len > IP_MAX_PACKET)
		return -EMSGSIZE;
	daddr = net_ntohl(target->sin_addr.s_addr);
	rc = ip_route_out(daddr, 0, s->bound_ifindex ?
					    ip_dev_by_index(s->bound_ifindex) :
					    NULL,
			  &rt);
	if (rc < 0) {
		NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
		return rc;
	}
	if (rt.type == RTN_BROADCAST && !s->broadcast) {
		ip_route_put(&rt);
		return -EACCES;
	}
	ip_route_put(&rt);
	// the caller's bytes, copied once (user memory may fault)
	kbuf = (uint8_t *)kalloc(len ? len : 1);
	if (!kbuf)
		return -ENOBUFS;
	smap_disable();
	mm_memcpy(kbuf, buf, len);
	smap_enable();
	if (s->ip_hdrincl || s->protocol == IPPROTO_RAW) {
		rc = ipv4_send_raw(s->bound_ifindex ?
					   ip_dev_by_index(s->bound_ifindex) :
					   NULL,
				   daddr, kbuf, (uint16_t)len);
	} else {
		struct ip_tx tx;
		sk_buff_t *skb = skb_alloc((uint32_t)len);

		if (!skb) {
			kfree(kbuf);
			return -ENOBUFS;
		}
		if (len)
			mm_memcpy(skb_append(skb, (uint32_t)len), kbuf, len);
		mm_memset(&tx, 0, sizeof(tx));
		tx.saddr = s->udp_saddr;
		tx.daddr = daddr;
		tx.oif = s->bound_ifindex ? ip_dev_by_index(s->bound_ifindex) :
					    NULL;
		tx.proto = (uint8_t)s->protocol;
		tx.ttl = s->ip_ttl;
		tx.tos = s->ip_tos;
		tx.pmtudisc = s->pmtudisc;
		tx.mc_ttl = s->mcast_ttl;
		tx.mc_loop = s->mcast_loop;
		rc = ip_send_skb(skb, &tx, NULL, NULL);
	}
	kfree(kbuf);
	return rc < 0 ? rc : (int)len;
}
