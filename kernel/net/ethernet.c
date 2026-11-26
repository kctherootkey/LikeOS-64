// LikeOS Ethernet Frame Layer
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/ip.h>
#include <kernel/net/netdevice.h>
#include <kernel/io/console.h>
#include <kernel/net/skb.h>
#include <kernel/uapi/bug.h>
#include <kernel/mm/memory.h>

// Build an Ethernet frame around `payload' and queue it on the device
// (dev_queue_xmit: the driver is never entered under a protocol lock).
int eth_send(net_device_t *dev, const uint8_t dst_mac[ETH_ALEN],
	     uint16_t ethertype, const uint8_t *payload, uint16_t len)
{
	BUG_ON(dev == NULL);
	BUILD_BUG_ON(ETH_HLEN != 14);
	if (!dev || (!dev->send && !dev->xmit))
		return -1;
	if (len > dev->mtu)
		return -1;
	WARN_ON(len == 0); /* eth_send: zero-length payload */

	sk_buff_t *skb = skb_alloc(len);
	if (!skb)
		return -1;
	if (len)
		mm_memcpy(skb_append(skb, len), payload, len);
	skb_reset_network_header(skb);
	eth_header_t *hdr = (eth_header_t *)skb_push(skb, ETH_HLEN);
	mm_memcpy(hdr->dst, dst_mac, ETH_ALEN);
	mm_memcpy(hdr->src, dev->mac_addr, ETH_ALEN);
	hdr->ethertype = net_htons(ethertype);
	skb_reset_mac_header(skb);
	skb->protocol = ethertype;
	skb->dev = dev;
	return dev_queue_xmit(skb) == NET_XMIT_SUCCESS ? 0 : -1;
}

// How a frame was addressed: to us, to everyone, to a group, to another
// station (a NIC in promiscuous mode, a shared medium).
static uint8_t eth_pkt_type(const net_device_t *dev, const uint8_t *dst)
{
	if (dst[0] & 0x01) {
		for (int i = 0; i < ETH_ALEN; i++)
			if (dst[i] != 0xFF)
				return PACKET_MULTICAST;
		return PACKET_BROADCAST;
	}
	for (int i = 0; i < ETH_ALEN; i++)
		if (dst[i] != dev->mac_addr[i])
			return PACKET_OTHERHOST;
	return PACKET_HOST;
}

/* A received frame (skb->data at the Ethernet header): classified by
 * its destination address, the header pulled, and handed to ARP or IPv4.
 * Consumes the skb. */
int eth_rcv(sk_buff_t *skb)
{
	net_device_t *dev = skb->dev;
	const eth_header_t *hdr;
	uint16_t ethertype;

	BUG_ON(dev == NULL);
	if (skb->len < ETH_HLEN) {
		skb_put(skb);
		return NET_RX_DROP;
	}
	skb_reset_mac_header(skb);
	hdr = (const eth_header_t *)skb->data;
	ethertype = net_ntohs(hdr->ethertype);
	skb->pkt_type = eth_pkt_type(dev, hdr->dst);

	/* A group address as the source: no station sends that (IEEE 802.3
	 * 3.2.3); bad input from the wire, dropped and counted. */
	if (hdr->src[0] & 0x01) {
		dev->rx_dropped++;
		dev->rx_bad_src++;
		skb_put(skb);
		return NET_RX_DROP;
	}

	/* A frame for another station is not ours to process (the address
	 * filter let it through: promiscuous mode). */
	if (skb->pkt_type == PACKET_OTHERHOST) {
		skb_put(skb);
		return NET_RX_DROP;
	}
	if (skb->pkt_type == PACKET_MULTICAST)
		dev->multicast++;
	skb_pull(skb, ETH_HLEN);
	skb->protocol = ethertype;

	switch (ethertype) {
	case ETH_P_ARP:
		arp_rcv(dev, skb->data,
			(uint16_t)(skb->len > 0xFFFF ? 0xFFFF : skb->len),
			skb->pkt_type);
		skb_put(skb);
		return NET_RX_SUCCESS;
	case ETH_P_IP:
		/* No address learning here: the neighbour table learns from
		 * address resolution only (arp.c). */
		return ip_rcv(skb);
	default:
		skb_put(skb); // unknown ethertype
		return NET_RX_DROP;
	}
}

// A received frame from a byte buffer.
void eth_rx(net_device_t *dev, const uint8_t *frame, uint16_t len)
{
	sk_buff_t *skb;

	BUG_ON(dev == NULL);
	BUG_ON(frame == NULL);
	if (len < ETH_HLEN)
		return;
	skb = skb_alloc(len);
	if (!skb)
		return;
	mm_memcpy(skb_append(skb, len), frame, len);
	skb->dev = dev;
	eth_rcv(skb);
}
