// LikeOS — IPv4 output
//
// ip_route_out() turns a destination into a struct ip_route: the device,
// the next hop, the source address, the route type and the path MTU (the
// device's, a route metric, or a path-MTU exception).  ip_send_skb() puts
// the IP header in front of a transport payload and sends it:
//
//   * TTL: the multicast TTL for a group, otherwise the sender's IP_TTL or
//     the route's hop limit (64);
//   * DF by the sender's path-MTU discovery mode: WANT sets it when the
//     datagram fits the path MTU (and the path MTU is not at its locked
//     floor), DO and PROBE always (a datagram too big is refused with
//     EMSGSIZE), DONT and OMIT never, INTERFACE never but bounded by the
//     device MTU;
//   * identification: a connected socket's own counter, 0 for an
//     unconnected datagram with DF (it can never be fragmented), the
//     per-destination counter otherwise (ipv4.c);
//   * a datagram larger than the MTU without DF is fragmented;
//   * multicast is looped back to this host when the sender wants it and
//     the group is joined here; broadcast always is;
//   * every frame goes to the loopback device or through the neighbour
//     table of the egress device (ARP).
//
// Nothing here holds a lock across the device: dev_queue_xmit() defers to
// the transmit softirq when it is entered with interrupts off.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/net/netdevice.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

static inline int ipv4_is_multicast(uint32_t a)
{
	return (a & 0xF0000000u) == 0xE0000000u;
}

void ip_route_put(struct ip_route *rt)
{
	if (rt && rt->dst) {
		dst_release(rt->dst);
		rt->dst = NULL;
	}
}

/* Fill `rt' from a held output entry (its hold moves into rt). */
static void ip_route_from_dst(struct ip_route *rt, struct dst_entry *dst,
			      uint32_t daddr)
{
	net_device_t *lo = net_get_loopback();

	rt->dst = dst;
	rt->dev = dst->dev;
	rt->nexthop = rt_nexthop(dst, daddr);
	rt->type = dst->type;
	rt->mtu_locked = (dst->flags & DST_MTU_LOCKED) ? 1 : 0;
	if (dst->type == RTN_LOCAL) {
		// one of our interface addresses
		rt->loopback = 1;
		rt->saddr = dst->prefsrc ? dst->prefsrc : daddr;
	} else if (dst->dev == lo) {
		rt->loopback = 1; // 127.0.0.0/8
		rt->saddr = daddr;
	} else {
		rt->saddr = dst->prefsrc ? dst->prefsrc :
			    dst->dev ? dst->dev->ip_addr :
				       0;
	}
	if (rt->loopback && lo)
		rt->dev = lo;
	if (dst->type == RTN_BROADCAST)
		rt->nexthop = daddr;
	if (rt->loopback) {
		rt->dev_mtu = rt->mtu = lo ? lo->mtu : IP_MAX_PACKET;
	} else {
		rt->dev_mtu = rt->dev ? rt->dev->mtu : NET_MTU_DEFAULT;
		rt->mtu = dst_mtu(dst);
	}
}

int ip_route_out(uint32_t daddr, uint32_t saddr, net_device_t *oif,
		 struct ip_route *rt)
{
	net_device_t *lo = net_get_loopback();
	struct dst_entry *dst = NULL;
	int rc;

	mm_memset(rt, 0, sizeof(*rt));
	if (ipv4_is_multicast(daddr)) {
		// a group: out of the requested device or the route's
		net_device_t *dev = oif;
		uint32_t nh;

		if (!dev)
			dev = route_lookup(daddr, &nh);
		if (!dev)
			dev = net_get_default_device();
		if (!dev)
			return -ENETUNREACH;
		rt->dev = dev;
		rt->nexthop = daddr;
		rt->type = RTN_MULTICAST;
		rt->loopback = dev == lo;
		rt->saddr = saddr ? saddr : dev->ip_addr;
		rt->mtu = rt->dev_mtu = dev->mtu;
		return 0;
	}
	rc = ip_route_output(daddr, saddr, oif, &dst);
	if (rc == 0) {
		ip_route_from_dst(rt, dst, daddr);
	} else {
		/* No route.  A sender that named its device (the DHCP
		 * client before the lease installed any route) still goes
		 * out of it, towards the gateway when off-link. */
		net_device_t *dev = oif;

		if (!dev || (rc != -ENETUNREACH && rc != -ENOBUFS))
			return rc;
		rt->dev = dev;
		rt->nexthop = daddr;
		if (dev->gateway &&
		    (daddr & dev->netmask) != (dev->ip_addr & dev->netmask))
			rt->nexthop = dev->gateway;
		rt->loopback = dev == lo;
		rt->saddr = rt->loopback ? daddr : dev->ip_addr;
		rt->type = daddr == 0xFFFFFFFFu ? RTN_BROADCAST : RTN_UNICAST;
		if (rt->type == RTN_BROADCAST)
			rt->nexthop = daddr;
		rt->mtu = rt->dev_mtu = dev->mtu;
	}
	if (saddr)
		rt->saddr = saddr;
	if (!rt->mtu)
		rt->mtu = NET_MTU_DEFAULT;
	if (!rt->dev_mtu)
		rt->dev_mtu = NET_MTU_DEFAULT;
	return 0;
}

uint32_t ip_route_mtu(const struct ip_route *rt, uint8_t pmtudisc)
{
	switch (pmtudisc) {
	case IP_PMTUDISC_PROBE:
	case IP_PMTUDISC_INTERFACE:
	case IP_PMTUDISC_OMIT:
		return rt->dev_mtu; // the path MTU is not used
	default:
		return rt->mtu;
	}
}

int ip_finish_output(struct ip_route *rt, sk_buff_t *skb)
{
	net_device_t *lo = net_get_loopback();
	net_device_t *dev = rt->loopback && lo ? lo : rt->dev;

	if (!dev) {
		skb_put(skb);
		NET_STATS_INC(NET_MIB_IP_OUTDISCARDS);
		return -ENETUNREACH;
	}
	skb->dev = dev;
	skb->protocol = ETH_P_IP;
	skb_reset_network_header(skb);
	if (dev == lo) {
		if (dev_queue_xmit(skb) == NET_XMIT_SUCCESS)
			return 0;
	} else if (ip_neigh_output(dev, rt->nexthop, skb) == 0) {
		return 0;
	}
	NET_STATS_INC(NET_MIB_IP_OUTDISCARDS);
	return -ENOBUFS;
}

static int ip_fragment_out(void *ctx, sk_buff_t *frag)
{
	return ip_finish_output((struct ip_route *)ctx, frag);
}

/* A copy of our own multicast or broadcast datagram for this host's
 * listeners: it re-enters the receive path as looped back (no link
 * header; ip_rcv trusts it). */
static void ip_loop_copy(sk_buff_t *skb, net_device_t *dev)
{
	sk_buff_t *n = skb_clone(skb);

	if (!n)
		return;
	n->dev = dev;
	n->pkt_type = PACKET_LOOPBACK;
	n->protocol = ETH_P_IP;
	n->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_network_header(n);
	netif_rx(n);
}

int ip_send_skb(sk_buff_t *skb, const struct ip_tx *tx, struct ip_route *rt,
		uint32_t *mtu_out)
{
	struct ip_route local;
	uint32_t total, mtu, saddr;
	uint8_t ttl, *iph;
	uint16_t frag = 0, id;
	int df, rc, own_rt = 0;

	if (!rt) {
		rc = ip_route_out(tx->daddr, tx->saddr, tx->oif, &local);
		if (rc < 0) {
			NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
			skb_put(skb);
			return rc;
		}
		rt = &local;
		own_rt = 1;
	}
	NET_STATS_INC(NET_MIB_IP_OUTREQUESTS);
	total = skb->len + IP_HDR_MIN;
	mtu = ip_route_mtu(rt, tx->pmtudisc);
	if (mtu_out)
		*mtu_out = mtu;
	if (total > IP_MAX_PACKET) {
		rc = -EMSGSIZE;
		goto drop;
	}
	switch (tx->pmtudisc) {
	case IP_PMTUDISC_DONT:
	case IP_PMTUDISC_OMIT:
		df = 0;
		break;
	case IP_PMTUDISC_INTERFACE:
		df = 0;
		if (total > mtu) {
			rc = -EMSGSIZE;
			goto drop;
		}
		break;
	case IP_PMTUDISC_DO:
	case IP_PMTUDISC_PROBE:
		df = 1;
		if (total > mtu) {
			rc = -EMSGSIZE;
			goto drop;
		}
		break;
	default: // WANT
		df = total <= mtu && !rt->mtu_locked;
		break;
	}
	if ((tx->flags & IPTX_NO_FRAG) && total > mtu) {
		rc = -EMSGSIZE;
		goto drop;
	}
	if (rt->type == RTN_MULTICAST)
		ttl = tx->mc_ttl;
	else if (tx->ttl)
		ttl = tx->ttl;
	else
		ttl = (uint8_t)(rt->dst ? dst_hoplimit(rt->dst) :
					  IP_DEFAULT_TTL);
	saddr = tx->saddr ? tx->saddr : rt->saddr;
	if (df)
		frag |= IP_DF;
	if (tx->idp)
		id = __atomic_fetch_add(tx->idp, 1, __ATOMIC_RELAXED);
	else if (df && !(tx->flags & IPTX_ID_PER_DEST))
		id = 0; // can never be fragmented: no identification needed
	else
		id = ip_select_ident(saddr, tx->daddr, tx->proto, 1);

	/* The header goes into the headroom: it must be ours alone (a clone
	 * of the same data still in flight would have its header
	 * rewritten). */
	if (skb_headroom(skb) < IP_HDR_MIN + ETH_HLEN || skb_cloned(skb)) {
		skb = skb_cow_head(skb, IP_HDR_MIN + ETH_HLEN);
		if (!skb) {
			rc = -ENOBUFS;
			NET_STATS_INC(NET_MIB_IP_OUTDISCARDS);
			goto out;
		}
	}
	iph = skb_push(skb, IP_HDR_MIN);
	iph[0] = 0x45;
	iph[1] = tx->tos;
	iph[2] = (uint8_t)(total >> 8);
	iph[3] = (uint8_t)total;
	iph[4] = (uint8_t)(id >> 8);
	iph[5] = (uint8_t)id;
	iph[6] = (uint8_t)(frag >> 8);
	iph[7] = (uint8_t)frag;
	iph[8] = ttl;
	iph[9] = tx->proto;
	iph[10] = iph[11] = 0;
	iph[12] = (uint8_t)(saddr >> 24);
	iph[13] = (uint8_t)(saddr >> 16);
	iph[14] = (uint8_t)(saddr >> 8);
	iph[15] = (uint8_t)saddr;
	iph[16] = (uint8_t)(tx->daddr >> 24);
	iph[17] = (uint8_t)(tx->daddr >> 16);
	iph[18] = (uint8_t)(tx->daddr >> 8);
	iph[19] = (uint8_t)tx->daddr;
	{
		uint16_t c = ipv4_checksum(iph, IP_HDR_MIN);

		mm_memcpy(iph + 10, &c, 2);
	}
	skb->protocol = ETH_P_IP;
	skb_reset_network_header(skb);

	if (!rt->loopback && rt->dev) {
		if (rt->type == RTN_MULTICAST && tx->mc_loop &&
		    ip_mc_is_member(rt->dev, tx->daddr))
			ip_loop_copy(skb, rt->dev);
		else if (rt->type == RTN_BROADCAST)
			ip_loop_copy(skb, rt->dev);
		if (rt->type == RTN_MULTICAST && ttl == 0) {
			// TTL 0: for this host only
			skb_put(skb);
			rc = 0;
			goto out;
		}
	}
	if (total > mtu && !rt->loopback)
		rc = ip_do_fragment(skb, mtu, ip_fragment_out, rt);
	else
		rc = ip_finish_output(rt, skb);
	goto out;
drop:
	NET_STATS_INC(NET_MIB_IP_OUTDISCARDS);
	skb_put(skb);
out:
	if (own_rt)
		ip_route_put(rt);
	return rc;
}

int ip_queue_xmit(sk_buff_t *skb, uint32_t saddr, uint32_t daddr,
		  uint8_t proto, uint8_t ttl, uint8_t tos,
		  struct dst_cache *cache, uint16_t *idp)
{
	struct ip_route rt;
	struct ip_tx tx;
	struct dst_entry *dst = NULL;
	int rc;

	mm_memset(&tx, 0, sizeof(tx));
	tx.saddr = saddr;
	tx.daddr = daddr;
	tx.proto = proto;
	tx.ttl = ttl;
	tx.tos = tos;
	tx.pmtudisc = IP_PMTUDISC_WANT;
	tx.flags = IPTX_ID_PER_DEST;
	tx.idp = idp;
	if (cache) {
		dst = dst_cache_get(cache);
		if (dst && cache->daddr != daddr) {
			dst_release(dst);
			dst = NULL;
		}
	}
	if (dst) {
		mm_memset(&rt, 0, sizeof(rt));
		ip_route_from_dst(&rt, dst, daddr);
		if (saddr)
			rt.saddr = saddr;
	} else {
		rc = ip_route_out(daddr, saddr, NULL, &rt);
		if (rc < 0) {
			NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
			skb_put(skb);
			return rc;
		}
		if (cache && rt.dst)
			dst_cache_set(cache, rt.dst, daddr);
	}
	rc = ip_send_skb(skb, &tx, &rt, NULL);
	ip_route_put(&rt);
	return rc;
}

void ip_output_init(void)
{
}
