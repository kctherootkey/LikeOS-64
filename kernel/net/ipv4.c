// LikeOS IPv4 Layer -- glue
//
// The IPv4 layer proper lives in ip_input.c (receive, input route, local
// delivery), ip_options.c, ip_fragment.c and inet_frag.c (fragmentation
// and reassembly) and ip_output.c (routing a datagram out, DF, fragments).
// This file initialises it and keeps the byte-buffer senders the rest of
// the kernel uses (TCP's segments, IGMP, the kernel ping, raw sockets)
// on top of ip_send_skb(), plus the identification generator.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/net/netdevice.h>
#include <kernel/dev/rand/random.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

void ip_input_init(void);

/* Identification (RFC 791, RFC 6864).  Every datagram that may be
 * fragmented needs one that does not repeat while fragments of an earlier
 * datagram between the same two hosts can still be in flight.  It comes
 * from a counter per destination -- one of IP_IDENTS buckets chosen by a
 * keyed hash of (source, destination, protocol) -- that starts at a random
 * value and counts up: the identification of consecutive datagrams to one
 * host increments, as passive fingerprinting expects of the common
 * stacks, while a third party cannot read one host's counter from another
 * host's traffic.  Datagrams with DF from an unconnected socket carry 0
 * (they can never be fragmented); a connected socket counts its own (see
 * ip_send_skb). */
#define IP_IDENTS 2048
static uint32_t ip_idents[IP_IDENTS];
static uint32_t ip_idents_key[4];

void ipv4_init(void)
{
	for (int i = 0; i < IP_IDENTS; i++)
		ip_idents[i] = random_u32();
	for (int i = 0; i < 4; i++)
		ip_idents_key[i] = random_u32();
	ipfrag_init();
	raw_init();
	icmp_init();
	ip_output_init();
	ip_input_init();
}

uint16_t ip_select_ident(uint32_t saddr, uint32_t daddr, uint8_t proto,
			 uint32_t segs)
{
	uint32_t h = daddr ^ ip_idents_key[0];

	h = (h ^ (h >> 16)) * 0x7FEB352Du;
	h ^= saddr + ip_idents_key[1];
	h = (h ^ (h >> 15)) * 0x846CA68Bu;
	h ^= (uint32_t)proto * 0x9E3779B1u + ip_idents_key[2];
	h ^= h >> 16;
	return (uint16_t)__atomic_fetch_add(&ip_idents[h & (IP_IDENTS - 1)],
					    segs ? segs : 1,
					    __ATOMIC_RELAXED);
}

// One's complement checksum
uint16_t ipv4_checksum(const void *data, uint16_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t sum = 0;

	while (len > 1) {
		uint16_t w;

		mm_memcpy(&w, p, 2);
		sum += w;
		p += 2;
		len -= 2;
	}
	if (len == 1)
		sum += *p;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

/* A datagram from a byte buffer: copied into an skb behind room for the
 * headers and sent.  TCP's segments go with DF (as the common stacks send
 * every one: a narrower path answers with fragmentation-needed and
 * tcp_v4_mtu_report() lowers the MSS, RFC 1191) and with the per-destination
 * identification counter; everything else without DF. */
static int ipv4_send_common(net_device_t *dev, uint32_t dst_ip,
			    uint8_t protocol, const uint8_t *payload,
			    uint16_t len, uint8_t ttl, uint8_t tos)
{
	struct ip_tx tx;
	sk_buff_t *skb;

	BUILD_BUG_ON(sizeof(ipv4_header_t) != 20);
	WARN_ON(payload == NULL && len > 0);
	skb = skb_alloc(len);
	if (!skb) {
		NET_STATS_INC(NET_MIB_IP_OUTDISCARDS);
		return -1;
	}
	if (len)
		mm_memcpy(skb_append(skb, len), payload, len);
	mm_memset(&tx, 0, sizeof(tx));
	tx.daddr = dst_ip;
	tx.oif = dev;
	tx.proto = protocol;
	tx.ttl = ttl;
	tx.tos = tos;
	tx.mc_ttl = ttl;
	tx.mc_loop = 0;
	if (protocol == IP_PROTO_TCP) {
		tx.pmtudisc = IP_PMTUDISC_WANT;
		tx.flags = IPTX_ID_PER_DEST;
	} else {
		tx.pmtudisc = IP_PMTUDISC_DONT;
	}
	/* Sent, or queued for address resolution: success for the upper
	 * layers either way.  A drop (no memory, transmit queue full) is
	 * reported so they can retry. */
	return ip_send_skb(skb, &tx, NULL, NULL) < 0 ? -1 : 0;
}

int ipv4_send(net_device_t *dev, uint32_t dst_ip, uint8_t protocol,
	      const uint8_t *payload, uint16_t len)
{
	return ipv4_send_common(dev, dst_ip, protocol, payload, len,
				IP_DEFAULT_TTL, 0);
}

int ipv4_send_ttl(net_device_t *dev, uint32_t dst_ip, uint8_t protocol,
		  const uint8_t *payload, uint16_t len, uint8_t ttl)
{
	return ipv4_send_common(dev, dst_ip, protocol, payload, len, ttl, 0);
}

int ipv4_send_full(net_device_t *dev, uint32_t dst_ip, uint8_t protocol,
		   const uint8_t *payload, uint16_t len, uint8_t ttl,
		   uint8_t tos)
{
	return ipv4_send_common(dev, dst_ip, protocol, payload, len, ttl, tos);
}

/* A datagram whose header the caller built (raw socket, IP_HDRINCL).  The
 * header is completed the way a raw sender may rely on: the source address
 * and the identification when they are 0, and the checksum.  The total
 * length stays as the caller wrote it -- a raw socket is also how a
 * malformed datagram is made on purpose (the receive path's header checks
 * are tested that way).  Never fragmented: larger than the MTU is an
 * error. */
int ipv4_send_raw(net_device_t *dev, uint32_t dst_ip,
		  const uint8_t *full_ip_packet, uint16_t total_len)
{
	struct ip_route rt;
	sk_buff_t *skb;
	uint8_t *iph;
	uint32_t ihl;
	int rc;

	if (!full_ip_packet || total_len < IP_HDR_MIN)
		return -EINVAL;
	ihl = (uint32_t)(full_ip_packet[0] & 0x0F) * 4;
	if ((full_ip_packet[0] >> 4) != 4 || ihl < IP_HDR_MIN ||
	    ihl > total_len)
		return -EINVAL;
	rc = ip_route_out(dst_ip, 0, dev, &rt);
	if (rc < 0) {
		NET_STATS_INC(NET_MIB_IP_OUTNOROUTES);
		return rc;
	}
	if (total_len > ip_route_mtu(&rt, IP_PMTUDISC_DO) && !rt.loopback) {
		ip_route_put(&rt);
		return -EMSGSIZE;
	}
	skb = skb_alloc(total_len);
	if (!skb) {
		ip_route_put(&rt);
		return -ENOBUFS;
	}
	iph = skb_append(skb, total_len);
	mm_memcpy(iph, full_ip_packet, total_len);
	if (!iph[12] && !iph[13] && !iph[14] && !iph[15]) {
		iph[12] = (uint8_t)(rt.saddr >> 24);
		iph[13] = (uint8_t)(rt.saddr >> 16);
		iph[14] = (uint8_t)(rt.saddr >> 8);
		iph[15] = (uint8_t)rt.saddr;
	}
	if (!iph[4] && !iph[5]) {
		uint32_t s = ((uint32_t)iph[12] << 24) | (iph[13] << 16) |
			     (iph[14] << 8) | iph[15];
		uint16_t id = ip_select_ident(s, dst_ip, iph[9], 1);

		iph[4] = (uint8_t)(id >> 8);
		iph[5] = (uint8_t)id;
	}
	iph[10] = iph[11] = 0;
	{
		uint16_t c = ipv4_checksum(iph, (uint16_t)ihl);

		mm_memcpy(iph + 10, &c, 2);
	}
	NET_STATS_INC(NET_MIB_IP_OUTREQUESTS);
	rc = ip_finish_output(&rt, skb);
	ip_route_put(&rt);
	return rc;
}

/* A received datagram from a byte buffer (no link header). */
void ipv4_rx(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	sk_buff_t *skb;

	BUG_ON(dev == NULL);
	if (!data || !len)
		return;
	skb = skb_alloc(len);
	if (!skb)
		return;
	mm_memcpy(skb_append(skb, len), data, len);
	skb->dev = dev;
	ip_rcv(skb);
}
