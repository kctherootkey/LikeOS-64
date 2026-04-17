// LikeOS-64 Ethernet Frame Layer
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"

// Static frame buffer to avoid large stack allocations (kernel stack is only 8KB)
static uint8_t eth_frame_buf[ETH_FRAME_MAX];
static spinlock_t eth_send_lock = SPINLOCK_INIT("eth_tx");

// Build and send an Ethernet frame
int eth_send(net_device_t* dev, const uint8_t dst_mac[ETH_ALEN],
             uint16_t ethertype, const uint8_t* payload, uint16_t len) {
    if (!dev || !dev->send) return -1;
    if (len > dev->mtu) return -1;

    uint16_t frame_len = ETH_HLEN + len;

    uint64_t flags;
    spin_lock_irqsave(&eth_send_lock, &flags);

    // Build Ethernet header
    eth_header_t* hdr = (eth_header_t*)eth_frame_buf;
    for (int i = 0; i < ETH_ALEN; i++) {
        hdr->dst[i] = dst_mac[i];
        hdr->src[i] = dev->mac_addr[i];
    }
    hdr->ethertype = net_htons(ethertype);

    // Copy payload
    for (uint16_t i = 0; i < len; i++)
        eth_frame_buf[ETH_HLEN + i] = payload[i];

    int ret = dev->send(dev, eth_frame_buf, frame_len);
    spin_unlock_irqrestore(&eth_send_lock, flags);
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
        ipv4_rx(dev, payload, payload_len);
        break;
    default:
        // Unknown ethertype, drop
        break;
    }
}
