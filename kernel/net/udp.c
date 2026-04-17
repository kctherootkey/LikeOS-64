// LikeOS-64 UDP (User Datagram Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/syscall.h"

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

// Static send buffer to avoid large stack allocations
static uint8_t udp_send_buf[NET_MTU_DEFAULT];
static spinlock_t udp_send_lock = SPINLOCK_INIT("udp_tx");

// Send UDP datagram
int udp_send(net_device_t* dev, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             const uint8_t* data, uint16_t len) {
    if (!dev) return -1;

    uint16_t udp_len = sizeof(udp_header_t) + len;
    if (udp_len > NET_MTU_DEFAULT) return -1;

    uint64_t flags;
    spin_lock_irqsave(&udp_send_lock, &flags);

    udp_header_t* udp = (udp_header_t*)udp_send_buf;
    udp->src_port = net_htons(src_port);
    udp->dst_port = net_htons(dst_port);
    udp->length = net_htons(udp_len);
    udp->checksum = 0;  // UDP checksum optional in IPv4

    // Copy payload
    for (uint16_t i = 0; i < len; i++)
        udp_send_buf[sizeof(udp_header_t) + i] = data[i];

    int ret = ipv4_send(dev, dst_ip, IP_PROTO_UDP, udp_send_buf, udp_len);
    spin_unlock_irqrestore(&udp_send_lock, flags);
    return ret;
}

// Process received UDP datagram
void udp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    if (len < sizeof(udp_header_t)) return;

    const udp_header_t* udp = (const udp_header_t*)data;
    uint16_t src_port = net_ntohs(udp->src_port);
    uint16_t dst_port = net_ntohs(udp->dst_port);
    uint16_t udp_len = net_ntohs(udp->length);

    if (udp_len > len || udp_len < sizeof(udp_header_t)) return;

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
    udp_deliver_to_socket(src_ip, src_port, dst_port, payload, payload_len);
}

// Deliver UDP data to a bound socket
void udp_deliver_to_socket(uint32_t src_ip, uint16_t src_port,
                           uint16_t dst_port,
                           const uint8_t* data, uint16_t len) {
    // This is called from the socket layer's perspective
    // Find socket bound to dst_port and enqueue data
    extern net_socket_t* sock_find_udp(uint16_t port);
    net_socket_t* sk = sock_find_udp(dst_port);
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

    sk->udp_rx_tail = next;
    sk->udp_rx_ready = 1;

    spin_unlock_irqrestore(&sk->lock, flags);
}
