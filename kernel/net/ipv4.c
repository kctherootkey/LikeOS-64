// LikeOS-64 IPv4 Layer
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"

static uint16_t ipv4_id_counter = 0;

void ipv4_init(void) {
    ipv4_id_counter = 1;
}

// One's complement checksum
uint16_t ipv4_checksum(const void* data, uint16_t len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t*)ptr;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

// Send IPv4 packet
int ipv4_send(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
              const uint8_t* payload, uint16_t len) {
    if (!dev) return -1;

    uint16_t total_len = sizeof(ipv4_header_t) + len;
    if (total_len > dev->mtu) return -1;

    uint8_t pkt[NET_MTU_DEFAULT + sizeof(ipv4_header_t)];
    ipv4_header_t* ip = (ipv4_header_t*)pkt;

    ip->version_ihl = 0x45;       // IPv4, 5 words (20 bytes)
    ip->tos = 0;
    ip->total_length = net_htons(total_len);
    ip->identification = net_htons(ipv4_id_counter++);
    ip->flags_fragment = net_htons(0x4000);  // Don't Fragment
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_addr = net_htonl(dev->ip_addr);
    ip->dst_addr = net_htonl(dst_ip);

    // Compute header checksum
    ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

    // Copy payload
    for (uint16_t i = 0; i < len; i++)
        pkt[sizeof(ipv4_header_t) + i] = payload[i];

    // Determine next-hop IP for ARP resolution
    uint32_t next_hop = dst_ip;
    if (dev->gateway != 0 && (dst_ip & dev->netmask) != (dev->ip_addr & dev->netmask)) {
        next_hop = dev->gateway;  // Use gateway for off-subnet
    }

    // Resolve MAC address
    uint8_t dst_mac[ETH_ALEN];
    if (dst_ip == 0xFFFFFFFF) {
        // Broadcast
        for (int i = 0; i < ETH_ALEN; i++) dst_mac[i] = 0xFF;
    } else if (arp_resolve(dev, next_hop, dst_mac) < 0) {
        // ARP request sent, packet dropped (caller should retry)
        return -1;
    }

    return eth_send(dev, dst_mac, ETH_P_IP, pkt, total_len);
}

// Process received IPv4 packet
void ipv4_rx(net_device_t* dev, const uint8_t* data, uint16_t len) {
    if (len < sizeof(ipv4_header_t)) return;

    const ipv4_header_t* ip = (const ipv4_header_t*)data;

    // Verify version
    if ((ip->version_ihl >> 4) != 4) return;

    // Get header length
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;

    uint16_t total_len = net_ntohs(ip->total_length);
    if (total_len > len) return;

    // Verify checksum
    if (ipv4_checksum(ip, ihl) != 0) return;

    uint32_t dst_ip = net_ntohl(ip->dst_addr);
    uint32_t src_ip = net_ntohl(ip->src_addr);

    // Accept packets addressed to us, broadcast, or if we have no IP yet (DHCP)
    if (dst_ip != dev->ip_addr && dst_ip != 0xFFFFFFFF &&
        dst_ip != (dev->ip_addr | ~dev->netmask) &&
        dev->ip_addr != 0) {
        return;
    }

    const uint8_t* payload = data + ihl;
    uint16_t payload_len = total_len - ihl;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        icmp_rx(dev, src_ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        udp_rx(dev, src_ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        tcp_rx(dev, src_ip, payload, payload_len);
        break;
    default:
        break;
    }
}
