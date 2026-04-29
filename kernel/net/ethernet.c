// LikeOS-64 Ethernet Frame Layer
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/skb.h"

// Build and send an Ethernet frame.  The TX buffer is a per-call sk_buff
// from the size-classed pool, so no global TX spinlock is held across the
// underlying NIC submit -- callers' interrupts stay enabled long enough
// for TLB-shootdown IPIs and other priority work.
int eth_send(net_device_t* dev, const uint8_t dst_mac[ETH_ALEN],
             uint16_t ethertype, const uint8_t* payload, uint16_t len) {
    if (!dev || !dev->send) return -1;
    if (len > dev->mtu) return -1;

    uint16_t frame_len = ETH_HLEN + len;

    sk_buff_t* skb = skb_alloc(frame_len);
    if (!skb) return -1;
    skb->dev = dev;

    uint8_t* p = skb_append(skb, frame_len);
    eth_header_t* hdr = (eth_header_t*)p;
    for (int i = 0; i < ETH_ALEN; i++) {
        hdr->dst[i] = dst_mac[i];
        hdr->src[i] = dev->mac_addr[i];
    }
    hdr->ethertype = net_htons(ethertype);
    for (uint16_t i = 0; i < len; i++)
        p[ETH_HLEN + i] = payload[i];

    int ret = dev->send(dev, skb->data, skb->len);
    skb_put(skb);
    return ret;
}

// Process received Ethernet frame
void eth_rx(net_device_t* dev, const uint8_t* frame, uint16_t len) {
    if (len < ETH_HLEN) return;

    const eth_header_t* hdr = (const eth_header_t*)frame;
    uint16_t ethertype = net_ntohs(hdr->ethertype);
    const uint8_t* payload = frame + ETH_HLEN;
    uint16_t payload_len = len - ETH_HLEN;

    switch (ethertype) {
    case ETH_P_ARP:
        arp_rx(dev, payload, payload_len);
        break;
    case ETH_P_IP:
        // Snoop the source IP/MAC binding from the IPv4 header so any reply
        // we send (echo reply, RST, ICMP error) hits the ARP cache and does
        // not have to busy-wait on arp_resolve from softirq context.
        if (payload_len >= sizeof(ipv4_header_t)) {
            const ipv4_header_t* ip = (const ipv4_header_t*)payload;
            uint32_t src_ip = net_ntohl(ip->src_addr);
            // Don't snoop multicast / broadcast / zero source addresses.
            if (src_ip != 0 && (src_ip & 0xF0000000U) != 0xE0000000U &&
                src_ip != 0xFFFFFFFFU) {
                arp_add_entry(src_ip, hdr->src);
            }
        }
        ipv4_rx(dev, payload, payload_len);
        break;
    default:
        // Unknown ethertype, drop
        break;
    }
}
