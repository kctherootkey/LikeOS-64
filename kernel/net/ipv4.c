// LikeOS-64 IPv4 Layer
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/random.h"

// Static send buffer to avoid large stack allocations (kernel stack is only 8KB)
static uint8_t ipv4_send_buf[NET_MTU_DEFAULT + sizeof(ipv4_header_t)];
static spinlock_t ipv4_send_lock = SPINLOCK_INIT("ipv4_tx");

void ipv4_init(void) {
    // No more sequential counter - using random IDs
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
    // Use routing table to determine output device and next-hop
    uint32_t next_hop = dst_ip;
    net_device_t* out_dev = route_lookup(dst_ip, &next_hop);
    if (!out_dev) {
        // No route found, fall back to provided device
        out_dev = dev;
        if (!out_dev) return -1;
        // Use gateway if off-subnet
        next_hop = dst_ip;
        if (out_dev->gateway != 0 &&
            (dst_ip & out_dev->netmask) != (out_dev->ip_addr & out_dev->netmask)) {
            next_hop = out_dev->gateway;
        }
    }

    // If destination is our own NIC IP, route through loopback (like Linux)
    uint32_t lo_src_override = 0;
    if (out_dev != net_get_loopback() && out_dev->ip_addr != 0 &&
        dst_ip == out_dev->ip_addr) {
        lo_src_override = out_dev->ip_addr;  // Use NIC IP as source, not loopback's
        net_device_t* lo = net_get_loopback();
        if (lo) out_dev = lo;
    }

    // Loopback device: send raw IPv4 packet (no Ethernet framing)
    if (out_dev == net_get_loopback()) {
        uint16_t total_len = sizeof(ipv4_header_t) + len;
        if (total_len > sizeof(ipv4_send_buf)) return -1;

        uint64_t iflags;
        spin_lock_irqsave(&ipv4_send_lock, &iflags);

        ipv4_header_t* ip = (ipv4_header_t*)ipv4_send_buf;
        ip->version_ihl = 0x45;
        ip->tos = 0;
        ip->total_length = net_htons(total_len);
        ip->identification = net_htons((uint16_t)random_u32());
        ip->flags_fragment = net_htons(0x4000);
        ip->ttl = 64;
        ip->protocol = protocol;
        ip->checksum = 0;
        ip->src_addr = net_htonl(lo_src_override ? lo_src_override : dst_ip);
        ip->dst_addr = net_htonl(dst_ip);
        ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

        for (uint16_t i = 0; i < len; i++)
            ipv4_send_buf[sizeof(ipv4_header_t) + i] = payload[i];

        int ret = out_dev->send(out_dev, ipv4_send_buf, total_len);
        spin_unlock_irqrestore(&ipv4_send_lock, iflags);
        return ret;
    }

    uint16_t total_len = sizeof(ipv4_header_t) + len;
    if (total_len > out_dev->mtu) return -1;

    uint64_t iflags;
    spin_lock_irqsave(&ipv4_send_lock, &iflags);

    ipv4_header_t* ip = (ipv4_header_t*)ipv4_send_buf;

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = net_htons(total_len);
    ip->identification = net_htons((uint16_t)random_u32());
    ip->flags_fragment = net_htons(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_addr = net_htonl(out_dev->ip_addr);
    ip->dst_addr = net_htonl(dst_ip);

    ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

    for (uint16_t i = 0; i < len; i++)
        ipv4_send_buf[sizeof(ipv4_header_t) + i] = payload[i];

    // Determine next-hop IP for ARP resolution (already determined by route_lookup)
    // Resolve MAC address
    uint8_t dst_mac[ETH_ALEN];
    if (dst_ip == 0xFFFFFFFF) {
        // Broadcast
        for (int i = 0; i < ETH_ALEN; i++) dst_mac[i] = 0xFF;
    } else if (arp_resolve(out_dev, next_hop, dst_mac) < 0) {
        // ARP request sent, packet dropped (caller should retry)
        spin_unlock_irqrestore(&ipv4_send_lock, iflags);
        return -1;
    }

    int ret = eth_send(out_dev, dst_mac, ETH_P_IP, ipv4_send_buf, total_len);
    spin_unlock_irqrestore(&ipv4_send_lock, iflags);
    return ret;
}

// Send IPv4 packet with custom TTL
int ipv4_send_ttl(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                  const uint8_t* payload, uint16_t len, uint8_t ttl) {
    uint32_t next_hop = dst_ip;
    net_device_t* out_dev = route_lookup(dst_ip, &next_hop);
    if (!out_dev) {
        out_dev = dev;
        if (!out_dev) return -1;
        next_hop = dst_ip;
        if (out_dev->gateway != 0 &&
            (dst_ip & out_dev->netmask) != (out_dev->ip_addr & out_dev->netmask)) {
            next_hop = out_dev->gateway;
        }
    }

    // If destination is our own NIC IP, route through loopback
    uint32_t lo_src_override2 = 0;
    if (out_dev != net_get_loopback() && out_dev->ip_addr != 0 &&
        dst_ip == out_dev->ip_addr) {
        lo_src_override2 = out_dev->ip_addr;
        net_device_t* lo = net_get_loopback();
        if (lo) out_dev = lo;
    }

    // Loopback device: send raw IPv4 packet (no Ethernet framing)
    if (out_dev == net_get_loopback()) {
        uint16_t total_len = sizeof(ipv4_header_t) + len;
        if (total_len > sizeof(ipv4_send_buf)) return -1;

        uint64_t iflags;
        spin_lock_irqsave(&ipv4_send_lock, &iflags);

        ipv4_header_t* ip = (ipv4_header_t*)ipv4_send_buf;
        ip->version_ihl = 0x45;
        ip->tos = 0;
        ip->total_length = net_htons(total_len);
        ip->identification = net_htons((uint16_t)random_u32());
        ip->flags_fragment = net_htons(0x4000);
        ip->ttl = ttl;
        ip->protocol = protocol;
        ip->checksum = 0;
        ip->src_addr = net_htonl(lo_src_override2 ? lo_src_override2 : dst_ip);
        ip->dst_addr = net_htonl(dst_ip);
        ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

        for (uint16_t i = 0; i < len; i++)
            ipv4_send_buf[sizeof(ipv4_header_t) + i] = payload[i];

        int ret = out_dev->send(out_dev, ipv4_send_buf, total_len);
        spin_unlock_irqrestore(&ipv4_send_lock, iflags);
        return ret;
    }

    uint16_t total_len = sizeof(ipv4_header_t) + len;
    if (total_len > out_dev->mtu) return -1;

    uint64_t iflags;
    spin_lock_irqsave(&ipv4_send_lock, &iflags);

    ipv4_header_t* ip = (ipv4_header_t*)ipv4_send_buf;

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = net_htons(total_len);
    ip->identification = net_htons((uint16_t)random_u32());
    ip->flags_fragment = net_htons(0x4000);
    ip->ttl = ttl;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_addr = net_htonl(out_dev->ip_addr);
    ip->dst_addr = net_htonl(dst_ip);
    ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

    for (uint16_t i = 0; i < len; i++)
        ipv4_send_buf[sizeof(ipv4_header_t) + i] = payload[i];

    uint8_t dst_mac[ETH_ALEN];
    if (dst_ip == 0xFFFFFFFF) {
        for (int i = 0; i < ETH_ALEN; i++) dst_mac[i] = 0xFF;
    } else if (arp_resolve(out_dev, next_hop, dst_mac) < 0) {
        spin_unlock_irqrestore(&ipv4_send_lock, iflags);
        return -1;
    }

    int ret = eth_send(out_dev, dst_mac, ETH_P_IP, ipv4_send_buf, total_len);
    spin_unlock_irqrestore(&ipv4_send_lock, iflags);
    return ret;
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

    // Accept packets addressed to us, broadcast, loopback, or if we have no IP yet (DHCP)
    // On loopback device, accept all packets (we explicitly routed them there)
    if (dev != net_get_loopback() &&
        dst_ip != dev->ip_addr && dst_ip != 0xFFFFFFFF &&
        dst_ip != (dev->ip_addr | ~dev->netmask) &&
        (dst_ip & 0xFF000000) != 0x7F000000 &&
        dev->ip_addr != 0) {
        return;
    }

    const uint8_t* payload = data + ihl;
    uint16_t payload_len = total_len - ihl;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        icmp_rx(dev, src_ip, payload, payload_len, ip->ttl);
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
