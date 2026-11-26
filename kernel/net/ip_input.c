// LikeOS — IPv4 input: header checks, the input route, local delivery
//
// ip_rcv() takes a received datagram (skb->data at the IP header) and
// checks the header the way a host must: version 4, header length of at
// least 20 bytes and inside the frame, header checksum, header length <=
// total length <= frame length (the frame's padding is cut off).  The
// input route then decides what the datagram is to this host:
//   * one of our addresses (any interface's: the weak host model) --
//     local delivery;
//   * the limited broadcast, or the subnet broadcast of the interface it
//     came in on -- broadcast delivery;
//   * a multicast group the interface has joined (224.0.0.1 always) --
//     multicast delivery;
//   * anything else is dropped: this host does not forward.
// Martians are dropped on the way: a multicast or broadcast source, a
// source in 0.0.0.0/8 (except towards the limited broadcast, which is
// where an unconfigured host's DHCP traffic goes), 127.0.0.0/8 as source
// or destination on a real interface, and one of our own addresses as the
// source of a datagram from the outside.
// Options are checked next (ip_options.c): a malformed option is answered
// with an ICMP parameter problem, a source-routed datagram is dropped.
// ip_local_deliver() reassembles fragments, gives raw sockets their copies
// and calls the protocol's handler; a protocol nobody handles is answered
// with ICMP protocol unreachable.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/net/netdevice.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

static const struct net_protocol *inet_protos[256];

void inet_add_protocol(const struct net_protocol *p, uint8_t proto)
{
	__atomic_store_n(&inet_protos[proto], p, __ATOMIC_RELEASE);
}

const struct net_protocol *inet_get_protocol(uint8_t proto)
{
	return __atomic_load_n(&inet_protos[proto], __ATOMIC_ACQUIRE);
}

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline int ipv4_is_multicast(uint32_t a)
{
	return (a & 0xF0000000u) == 0xE0000000u;
}

static inline int ipv4_is_loopback(uint32_t a)
{
	return (a & 0xFF000000u) == 0x7F000000u;
}

static inline int ipv4_is_zeronet(uint32_t a)
{
	return (a & 0xFF000000u) == 0;
}

// ---- address helpers ----

int ip_addr_is_local(uint32_t addr)
{
	int n = net_device_count();

	if (ipv4_is_loopback(addr))
		return 1;
	for (int i = 0; i < n; i++) {
		net_device_t *d = net_get_device(i);

		if (d && d->ip_addr && d->ip_addr == addr)
			return 1;
	}
	return 0;
}

net_device_t *ip_dev_by_addr(uint32_t addr)
{
	int n = net_device_count();

	if (ipv4_is_loopback(addr))
		return net_get_loopback();
	for (int i = 0; i < n; i++) {
		net_device_t *d = net_get_device(i);

		if (d && d->ip_addr && d->ip_addr == addr)
			return d;
	}
	return NULL;
}

net_device_t *ip_dev_by_index(int ifindex)
{
	net_device_t *lo = net_get_loopback();
	int n = net_device_count();

	if (lo && lo->ifindex == ifindex)
		return lo;
	for (int i = 0; i < n; i++) {
		net_device_t *d = net_get_device(i);

		if (d && d->ifindex == ifindex)
			return d;
	}
	return NULL;
}

static int dev_subnet_bcast(const net_device_t *d, uint32_t addr)
{
	return d->ip_addr && d->netmask && d->netmask != 0xFFFFFFFFu &&
	       addr == (d->ip_addr | ~d->netmask);
}

int ip_addr_is_broadcast(uint32_t addr)
{
	int n = net_device_count();

	if (addr == 0xFFFFFFFFu)
		return 1;
	for (int i = 0; i < n; i++) {
		net_device_t *d = net_get_device(i);

		if (d && dev_subnet_bcast(d, addr))
			return 1;
	}
	return 0;
}

int ip_mc_is_member(net_device_t *dev, uint32_t group)
{
	if (!dev || !ipv4_is_multicast(group))
		return 0;
	if (group == 0xE0000001u) // all-hosts: every host is a member
		return 1;
	return igmp_is_member(dev, group);
}

// ---- the input route ----

/* A unicast datagram for a device that has no address yet: 1 when it is
 * a reply to the DHCP client -- sent to the device's own link address, a
 * UDP datagram (its first fragment, whole header present) to the client
 * port. */
static int ip_dhcp_reply_unconfigured(const sk_buff_t *skb)
{
	const uint8_t *iph = skb->data;
	uint32_t ihl = (uint32_t)(iph[0] & 0x0F) * 4;

	if (skb->pkt_type != PACKET_HOST || iph[9] != IP_PROTO_UDP)
		return 0;
	if (rd16(iph + 6) & IP_OFFSET)
		return 0; // a later fragment: no UDP header to look at
	if (rd16(iph + 2) < ihl + 8 || skb->len < ihl + 8)
		return 0;
	return rd16(iph + ihl + 2) == DHCP_CLIENT_PORT;
}

/* What the datagram is to us: RTN_LOCAL, RTN_BROADCAST or RTN_MULTICAST;
 * 0 when it is to be dropped (counted). */
static uint8_t ip_route_input(sk_buff_t *skb, uint32_t saddr, uint32_t daddr)
{
	net_device_t *dev = skb->dev;

	if (dev == net_get_loopback() || skb->pkt_type == PACKET_LOOPBACK) {
		// our own datagram, routed back to us
		if (ipv4_is_multicast(daddr))
			return RTN_MULTICAST;
		if (ip_addr_is_broadcast(daddr))
			return RTN_BROADCAST;
		return RTN_LOCAL;
	}
	if (ipv4_is_multicast(saddr) || saddr == 0xFFFFFFFFu)
		goto martian;
	if (daddr == 0xFFFFFFFFu || (saddr == 0 && daddr == 0)) {
		if (saddr && (ip_addr_is_local(saddr) ||
			      ip_addr_is_broadcast(saddr)))
			goto martian;
		return RTN_BROADCAST;
	}
	// a zero source only towards the limited broadcast (above)
	if (ipv4_is_zeronet(saddr) || ipv4_is_zeronet(daddr))
		goto martian;
	if (ipv4_is_loopback(daddr) || ipv4_is_loopback(saddr))
		goto martian;
	// our own address or a broadcast address as the source
	if (ip_addr_is_local(saddr) || ip_addr_is_broadcast(saddr))
		goto martian;
	if (ipv4_is_multicast(daddr)) {
		if (ip_mc_is_member(dev, daddr))
			return RTN_MULTICAST;
		return 0; // a group nobody here joined
	}
	if (dev_subnet_bcast(dev, daddr))
		return RTN_BROADCAST;
	if (ip_addr_is_local(daddr))
		return RTN_LOCAL;
	/* An interface without an address yet takes the DHCP client's
	 * replies sent to its own link address (the offer and the ACK are
	 * addressed to the address being offered) -- nothing else: it has
	 * no address that anything else could be for. */
	if (!dev->ip_addr && ip_dhcp_reply_unconfigured(skb))
		return RTN_LOCAL;
	NET_STATS_INC(NET_MIB_IP_INADDRERRORS); // not ours, no forwarding
	return 0;
martian:
	NET_STATS_INC(NET_MIB_IP_INMARTIANS);
	return 0;
}

// ---- receive ----

int ip_rcv(sk_buff_t *skb)
{
	struct inet_skb_parm *cb;
	const uint8_t *iph;
	uint32_t ihl, tot;
	uint8_t type;

	BUG_ON(skb->dev == NULL);
	if (skb->pkt_type == PACKET_OTHERHOST)
		goto drop;
	NET_STATS_INC(NET_MIB_IP_INRECEIVES);
	if (skb->len < IP_HDR_MIN)
		goto hdr_error;
	iph = skb->data;
	ihl = (uint32_t)(iph[0] & 0x0F) * 4;
	if ((iph[0] >> 4) != 4 || ihl < IP_HDR_MIN || ihl > skb->len)
		goto hdr_error;
	if (ipv4_checksum(iph, (uint16_t)ihl) != 0) {
		NET_STATS_INC(NET_MIB_IP_INCSUMERRORS);
		goto hdr_error;
	}
	tot = rd16(iph + 2);
	if (tot > skb->len) {
		NET_STATS_INC(NET_MIB_IP_INTRUNCATEDPKTS);
		goto drop;
	}
	if (tot < ihl)
		goto hdr_error;
	if (skb->len > tot) {
		skb_trim(skb, tot); // link-layer padding
		if (skb->ip_summed == CHECKSUM_COMPLETE)
			skb->ip_summed = CHECKSUM_NONE;
	}
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, (int)ihl);
	skb->protocol = ETH_P_IP;

	cb = IPCB(skb);
	mm_memset(cb, 0, sizeof(*cb));
	cb->iif = skb->dev->ifindex;
	cb->saddr = rd32(iph + 12);
	cb->daddr = rd32(iph + 16);
	cb->tos = iph[1];
	cb->ttl = iph[8];
	cb->proto = iph[9];
	cb->ihl = (uint16_t)ihl;
	cb->tot_len = (uint16_t)tot;
	if (skb->pkt_type == PACKET_LOOPBACK)
		cb->flags |= IPSKB_LOOPED;
	skb->src_ip = cb->saddr;
	skb->dst_ip = cb->daddr;
	skb->ip_proto = cb->proto;
	skb->rx_ttl = cb->ttl;

	type = ip_route_input(skb, cb->saddr, cb->daddr);
	if (!type)
		goto drop;
	cb->rt_type = type;
	if (type == RTN_BROADCAST)
		NET_STATS_INC(NET_MIB_IP_INBCASTPKTS);
	else if (type == RTN_MULTICAST)
		NET_STATS_INC(NET_MIB_IP_INMCASTPKTS);

	if (ihl > IP_HDR_MIN) {
		uint32_t bad = 0;

		if (ip_options_compile(iph, &cb->opt, &bad) < 0) {
			icmp_send(skb, ICMP_PARAMETERPROB, 0, bad << 24);
			goto hdr_error;
		}
		if (cb->opt.srr) {
			// source routing is not accepted
			NET_STATS_INC(NET_MIB_IP_INSRCROUTED);
			goto drop;
		}
	}
	ip_local_deliver(skb);
	return NET_RX_SUCCESS;

hdr_error:
	NET_STATS_INC(NET_MIB_IP_INHDRERRORS);
drop:
	skb_put(skb);
	return NET_RX_DROP;
}

static void ip_local_deliver_finish(sk_buff_t *skb)
{
	struct inet_skb_parm *cb = IPCB(skb);
	const struct net_protocol *p;
	uint8_t proto = cb->proto;
	int raw;

	BUG_ON(skb->len < cb->ihl);
	skb_pull(skb, cb->ihl);
	skb_reset_transport_header(skb);
	raw = raw_local_deliver(skb, proto);
	p = inet_get_protocol(proto);
	if (p && p->handler) {
		NET_STATS_INC(NET_MIB_IP_INDELIVERS);
		p->handler(skb);
		return;
	}
	if (raw) {
		NET_STATS_INC(NET_MIB_IP_INDELIVERS);
	} else {
		NET_STATS_INC(NET_MIB_IP_INUNKNOWNPROTOS);
		icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PROTO_UNREACH, 0);
	}
	skb_put(skb);
}

void ip_local_deliver(sk_buff_t *skb)
{
	const uint8_t *iph = skb->data;

	if (rd16(iph + 6) & (IP_MF | IP_OFFSET)) {
		skb = ip_defrag(skb);
		if (!skb)
			return;
	}
	ip_local_deliver_finish(skb);
}

// ---- the protocols this file adapts: TCP and IGMP ----

/* The segment itself goes to TCP, no copy: tcp_v4_rcv() takes the skb
 * with its data at the TCP header and IPCB filled in, checks the
 * checksum (unless the device or GRO did) and the broadcast/multicast
 * rule, and consumes it. */
static int ip_tcp_rcv(sk_buff_t *skb)
{
	return tcp_v4_rcv(skb);
}

static void ip_tcp_err(const struct icmp_err *e)
{
	uint16_t sport = rd16(e->l4), dport = rd16(e->l4 + 2);
	uint32_t seq = rd32(e->l4 + 4);

	if (e->type == ICMP_DEST_UNREACH && e->code == ICMP_FRAG_NEEDED) {
		/* Only about a connection that exists, and only for a
		 * sequence number it has outstanding: a forged report must
		 * not shrink the path (TCP checks, then updates the
		 * route). */
		tcp_v4_mtu_report(e->saddr, sport, e->daddr, dport, seq, e->info);
		return;
	}
	tcp_icmp_error(e->saddr, sport, e->daddr, dport, seq, e->err);
}

static int ip_igmp_rcv(sk_buff_t *skb)
{
	if (skb->len <= 0xFFFFu)
		igmp_rx(skb->dev, IPCB(skb)->saddr, skb->data,
			(uint16_t)skb->len);
	skb_put(skb);
	return 0;
}

static const struct net_protocol tcp_protocol = {
	.handler = ip_tcp_rcv,
	.err_handler = ip_tcp_err,
};

static const struct net_protocol igmp_protocol = {
	.handler = ip_igmp_rcv,
	.err_handler = NULL,
};

static const struct net_protocol udp_protocol = {
	.handler = udp_rcv,
	.err_handler = udp_err,
};

static const struct net_protocol icmp_protocol = {
	.handler = icmp_rcv,
	.err_handler = NULL,
};

void ip_input_init(void)
{
	inet_add_protocol(&icmp_protocol, IP_PROTO_ICMP);
	inet_add_protocol(&igmp_protocol, IPPROTO_IGMP);
	inet_add_protocol(&tcp_protocol, IP_PROTO_TCP);
	inet_add_protocol(&udp_protocol, IP_PROTO_UDP);
}
