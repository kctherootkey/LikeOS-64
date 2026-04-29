// LikeOS-64 UDP (User Datagram Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/skb.h"

// UDP pseudo-header for checksum
typedef struct __attribute__((packed)) {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t udp_length;
} udp_pseudo_header_t;

void udp_init(void) {
    // Nothing to initialize
}

// Compute UDP checksum (RFC 768): pseudo-header + UDP header + payload.
// Returns the wire value: 0xFFFF substituted for 0 since 0 means "no checksum".
static uint16_t udp_compute_checksum(uint32_t src_ip, uint32_t dst_ip,
                                     const uint8_t* udp_msg, uint16_t udp_len) {
    udp_pseudo_header_t ph;
    ph.src_addr = net_htonl(src_ip);
    ph.dst_addr = net_htonl(dst_ip);
    ph.zero = 0;
    ph.protocol = IP_PROTO_UDP;
    ph.udp_length = net_htons(udp_len);

    // One's-complement sum over pseudo-header || udp_msg.
    uint32_t sum = 0;
    const uint16_t* p = (const uint16_t*)&ph;
    for (size_t i = 0; i < sizeof(ph) / 2; i++) sum += p[i];
    p = (const uint16_t*)udp_msg;
    uint16_t whole = (uint16_t)(udp_len & ~1U);
    for (uint16_t i = 0; i < whole / 2; i++) sum += p[i];
    if (udp_len & 1) {
        uint16_t tail = (uint16_t)udp_msg[udp_len - 1];
        sum += tail; // low byte; high byte zero (per RFC 1071)
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t cs = (uint16_t)~sum;
    return cs ? cs : 0xFFFF;
}

// Send UDP datagram.  Per-call sk_buff allocation; no shared TX spinlock.
int udp_send(net_device_t* dev, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             const uint8_t* data, uint16_t len) {
    if (!dev) return -1;

    uint32_t udp_len = sizeof(udp_header_t) + (uint32_t)len;
    if (udp_len > 65535U - sizeof(ipv4_header_t)) return -1;

    sk_buff_t* skb = skb_alloc((uint32_t)udp_len);
    if (!skb) return -1;
    skb->dev = dev;

    uint8_t* buf = skb_append(skb, (uint32_t)udp_len);
    udp_header_t* udp = (udp_header_t*)buf;
    udp->src_port = net_htons(src_port);
    udp->dst_port = net_htons(dst_port);
    udp->length = net_htons((uint16_t)udp_len);
    udp->checksum = 0;

    for (uint16_t i = 0; i < len; i++)
        buf[sizeof(udp_header_t) + i] = data[i];

    // Compute checksum (RFC 768).  Source IP is the outgoing iface IP;
    // for loopback / unset, fall back to dst_ip so the pseudo-header is
    // consistent with what the receiver will recompute.
    uint32_t src_ip = dev->ip_addr ? dev->ip_addr : dst_ip;
    udp->checksum = udp_compute_checksum(src_ip, dst_ip, buf,
                                         (uint16_t)udp_len);

    int ret = ipv4_send(dev, dst_ip, IP_PROTO_UDP, skb->data, skb->len);
    skb_put(skb);
    return ret;
}

// Process received UDP datagram
void udp_rx(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
            const uint8_t* data, uint16_t len, uint8_t ttl, uint8_t tos) {
    if (len < sizeof(udp_header_t)) return;

    const udp_header_t* udp = (const udp_header_t*)data;
    uint16_t src_port = net_ntohs(udp->src_port);
    uint16_t dst_port = net_ntohs(udp->dst_port);
    uint16_t udp_len = net_ntohs(udp->length);

    if (udp_len > len || udp_len < sizeof(udp_header_t)) return;

    // RFC 768 checksum verify (a value of 0 means "sender disabled checksum").
    if (udp->checksum != 0) {
        uint16_t saved = udp->checksum;
        ((udp_header_t*)data)->checksum = 0;
        uint16_t expected = 0;
        // Recompute over the wire-form datagram, restore.
        // We must use a temporary buffer because `data` may be const-mapped;
        // here we already cast away const above.
        uint32_t sum = 0;
        udp_pseudo_header_t ph;
        ph.src_addr = net_htonl(src_ip);
        ph.dst_addr = net_htonl(dst_ip);
        ph.zero = 0;
        ph.protocol = IP_PROTO_UDP;
        ph.udp_length = net_htons(udp_len);
        const uint16_t* p = (const uint16_t*)&ph;
        for (size_t i = 0; i < sizeof(ph)/2; i++) sum += p[i];
        p = (const uint16_t*)data;
        uint16_t whole = (uint16_t)(udp_len & ~1U);
        for (uint16_t i = 0; i < whole/2; i++) sum += p[i];
        if (udp_len & 1) sum += (uint16_t)data[udp_len - 1];
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        expected = (uint16_t)~sum;
        if (expected == 0) expected = 0xFFFF;
        ((udp_header_t*)data)->checksum = saved;
        if (expected != saved) {
            // Bad checksum — silently drop per RFC 768.
            return;
        }
    }

    const uint8_t* payload = data + sizeof(udp_header_t);
    uint16_t payload_len = udp_len - sizeof(udp_header_t);

    // Check for DHCP client port
    if (dst_port == DHCP_CLIENT_PORT) {
        dhcp_rx(dev, payload, payload_len);
        return;
    }

    // Check for DNS client port
    if (dst_port == DNS_CLIENT_PORT && src_port == 53) {
        dns_rx(payload, payload_len);
        return;
    }

    // Deliver to socket layer
    extern net_socket_t* sock_find_udp(uint16_t port, uint32_t dst_ip);
    if (!sock_find_udp(dst_port, dst_ip)) {
        uint8_t quoted[sizeof(ipv4_header_t) + sizeof(udp_header_t) + 8];
        ipv4_header_t* ip = (ipv4_header_t*)quoted;
        ip->version_ihl = 0x45;
        ip->tos = 0;
        ip->total_length = net_htons((uint16_t)(sizeof(ipv4_header_t) + udp_len));
        ip->identification = 0;
        ip->flags_fragment = 0;
        ip->ttl = 64;
        ip->protocol = IP_PROTO_UDP;
        ip->checksum = 0;
        ip->src_addr = net_htonl(src_ip);
        ip->dst_addr = net_htonl(dst_ip);
        ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

        uint16_t copy_len = udp_len;
        if (copy_len > sizeof(quoted) - sizeof(ipv4_header_t))
            copy_len = (uint16_t)(sizeof(quoted) - sizeof(ipv4_header_t));
        for (uint16_t i = 0; i < copy_len; i++)
            quoted[sizeof(ipv4_header_t) + i] = data[i];

        icmp_send_error_packet(dev, src_ip, ICMP_DEST_UNREACH,
                               ICMP_PORT_UNREACH, quoted,
                               (uint16_t)(sizeof(ipv4_header_t) + copy_len), 0);
        return;
    }

    udp_deliver_to_socket(src_ip, src_port, dst_ip, dst_port, payload, payload_len, ttl, tos, dev);
}

// Deliver UDP data to a bound socket
void udp_deliver_to_socket(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           const uint8_t* data, uint16_t len,
                           uint8_t ttl, uint8_t tos, net_device_t* dev) {
    // This is called from the socket layer's perspective
    // Find socket bound to dst_port and enqueue data
    extern net_socket_t* sock_find_udp(uint16_t port, uint32_t dst_ip);
    net_socket_t* sk = sock_find_udp(dst_port, dst_ip);
    if (!sk) return;

    uint64_t flags;
    spin_lock_irqsave(&sk->lock, &flags);

    int next = (sk->udp_rx_tail + 1) % 16;
    if (next == sk->udp_rx_head) {
        // Queue full, drop packet
        spin_unlock_irqrestore(&sk->lock, flags);
        return;
    }

    // Copy data
    uint16_t copy_len = len;
    if (copy_len > NET_RX_BUF_SIZE) copy_len = NET_RX_BUF_SIZE;
    for (uint16_t i = 0; i < copy_len; i++)
        sk->udp_rx_queue[sk->udp_rx_tail].data[i] = data[i];
    sk->udp_rx_queue[sk->udp_rx_tail].len = copy_len;
    sk->udp_rx_queue[sk->udp_rx_tail].from.sin_family = AF_INET;
    sk->udp_rx_queue[sk->udp_rx_tail].from.sin_port = net_htons(src_port);
    sk->udp_rx_queue[sk->udp_rx_tail].from.sin_addr.s_addr = net_htonl(src_ip);
    sk->udp_rx_queue[sk->udp_rx_tail].dst_ip = dst_ip;
    sk->udp_rx_queue[sk->udp_rx_tail].ttl = ttl;
    sk->udp_rx_queue[sk->udp_rx_tail].tos = tos;
    sk->udp_rx_queue[sk->udp_rx_tail].ifindex = 0;
    if (dev) {
        for (int idx = 0; idx < 16; idx++) {
            if (net_get_device(idx) == dev) {
                sk->udp_rx_queue[sk->udp_rx_tail].ifindex = (uint32_t)idx;
                break;
            }
        }
    }

    sk->udp_rx_tail = next;
    sk->udp_rx_ready = 1;

    spin_unlock_irqrestore(&sk->lock, flags);
}
