// LikeOS-64 IPv4 Layer
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/random.h"
#include "../../include/kernel/timer.h"

// Static send buffer to avoid large stack allocations (kernel stack is only 8KB)
static uint8_t ipv4_send_buf[NET_MTU_DEFAULT + sizeof(ipv4_header_t)];
static spinlock_t ipv4_send_lock = SPINLOCK_INIT("ipv4_tx");

#define IPV4_FLAG_MF            0x2000
#define IPV4_FLAG_DF            0x4000
#define IPV4_FRAG_OFFSET_MASK   0x1FFF
#define IPV4_REASSEMBLY_SLOTS   4
#define IPV4_REASSEMBLY_TIMEOUT 300

typedef struct {
    int active;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t identification;
    uint8_t protocol;
    uint8_t have_last;
    uint16_t total_len;
    uint64_t expire_tick;
    uint8_t data[65535];
    uint8_t bitmap[(65535 + 7) / 8];
} ipv4_reassembly_slot_t;

static ipv4_reassembly_slot_t ipv4_reassembly[IPV4_REASSEMBLY_SLOTS];

static void ipv4_dispatch_payload(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                                  uint8_t protocol, const uint8_t* payload,
                                  uint16_t payload_len, uint8_t ttl) {
    switch (protocol) {
    case IP_PROTO_ICMP:
        icmp_rx(dev, src_ip, payload, payload_len, ttl);
        break;
    case IP_PROTO_UDP:
        udp_rx(dev, src_ip, dst_ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        tcp_rx(dev, src_ip, dst_ip, payload, payload_len);
        break;
    default:
        break;
    }
}

static ipv4_reassembly_slot_t* ipv4_get_reassembly_slot(uint32_t src_ip, uint32_t dst_ip,
                                                        uint16_t identification,
                                                        uint8_t protocol) {
    ipv4_reassembly_slot_t* free_slot = NULL;
    uint64_t now = timer_ticks();

    for (int i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        ipv4_reassembly_slot_t* slot = &ipv4_reassembly[i];
        if (slot->active && now >= slot->expire_tick)
            slot->active = 0;
        if (!slot->active) {
            if (!free_slot) free_slot = slot;
            continue;
        }
        if (slot->src_ip == src_ip && slot->dst_ip == dst_ip &&
            slot->identification == identification && slot->protocol == protocol) {
            return slot;
        }
    }

    if (!free_slot) free_slot = &ipv4_reassembly[0];
    free_slot->active = 1;
    free_slot->src_ip = src_ip;
    free_slot->dst_ip = dst_ip;
    free_slot->identification = identification;
    free_slot->protocol = protocol;
    free_slot->have_last = 0;
    free_slot->total_len = 0;
    free_slot->expire_tick = now + IPV4_REASSEMBLY_TIMEOUT;
    for (uint32_t i = 0; i < sizeof(free_slot->bitmap); i++)
        free_slot->bitmap[i] = 0;
    return free_slot;
}

static int ipv4_reassembly_complete(const ipv4_reassembly_slot_t* slot) {
    if (!slot->have_last) return 0;
    uint16_t blocks = (uint16_t)((slot->total_len + 7) / 8);
    for (uint16_t block = 0; block < blocks; block++) {
        if (!(slot->bitmap[block] & 1U))
            return 0;
    }
    return 1;
}

static void ipv4_mark_fragment(ipv4_reassembly_slot_t* slot, uint16_t offset, uint16_t len) {
    uint16_t start = (uint16_t)(offset / 8);
    uint16_t end = (uint16_t)((offset + len + 7) / 8);
    for (uint16_t block = start; block < end; block++)
        slot->bitmap[block] |= 1U;
}

static int ipv4_reassemble_fragment(net_device_t* dev, const ipv4_header_t* ip,
                                    uint32_t src_ip, uint32_t dst_ip,
                                    const uint8_t* payload, uint16_t payload_len,
                                    uint8_t ttl) {
    uint16_t frag = net_ntohs(ip->flags_fragment);
    uint16_t offset = (uint16_t)((frag & IPV4_FRAG_OFFSET_MASK) * 8);
    uint16_t identification = net_ntohs(ip->identification);
    ipv4_reassembly_slot_t* slot = ipv4_get_reassembly_slot(src_ip, dst_ip,
                                                            identification, ip->protocol);

    if ((uint32_t)offset + payload_len > sizeof(slot->data)) {
        slot->active = 0;
        return -1;
    }

    for (uint16_t i = 0; i < payload_len; i++)
        slot->data[offset + i] = payload[i];
    ipv4_mark_fragment(slot, offset, payload_len);

    if (!(frag & IPV4_FLAG_MF)) {
        slot->have_last = 1;
        slot->total_len = (uint16_t)(offset + payload_len);
    }
    slot->expire_tick = timer_ticks() + IPV4_REASSEMBLY_TIMEOUT;

    if (!ipv4_reassembly_complete(slot))
        return 0;

    ipv4_dispatch_payload(dev, src_ip, dst_ip, ip->protocol,
                          slot->data, slot->total_len, ttl);
    slot->active = 0;
    return 1;
}

static int ipv4_send_common(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
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

    uint32_t lo_src_override = 0;
    if (out_dev != net_get_loopback() && out_dev->ip_addr != 0 &&
        dst_ip == out_dev->ip_addr) {
        lo_src_override = out_dev->ip_addr;
        net_device_t* lo = net_get_loopback();
        if (lo) out_dev = lo;
    }

    uint8_t dst_mac[ETH_ALEN];
    if (out_dev != net_get_loopback()) {
        if (dst_ip == 0xFFFFFFFF) {
            for (int i = 0; i < ETH_ALEN; i++) dst_mac[i] = 0xFF;
        } else if (arp_resolve(out_dev, next_hop, dst_mac) < 0) {
            uint64_t arp_deadline = timer_ticks() + 300;
            while ((uint64_t)timer_ticks() < arp_deadline) {
                __asm__ volatile("sti; hlt");
                if (arp_cache_lookup(next_hop, dst_mac) == 0)
                    goto arp_done_common;
            }
            return -1;
        }
    }
arp_done_common:

    uint16_t mtu = out_dev == net_get_loopback() ? NET_MTU_DEFAULT : out_dev->mtu;
    if (mtu < sizeof(ipv4_header_t) + 8) return -1;

    uint16_t max_payload = (uint16_t)(mtu - sizeof(ipv4_header_t));
    uint16_t fragment_payload = (uint16_t)(max_payload & ~7U);
    if (fragment_payload == 0 || len <= max_payload)
        fragment_payload = max_payload;

    uint16_t identification = (uint16_t)random_u32();
    uint32_t src_ip = out_dev == net_get_loopback() ?
        (lo_src_override ? lo_src_override : dst_ip) : out_dev->ip_addr;

    uint16_t offset = 0;
    while (offset < len) {
        uint16_t chunk = (uint16_t)(len - offset);
        uint16_t frag_flags = 0;
        if (chunk > max_payload) chunk = fragment_payload;
        if (offset + chunk < len) frag_flags |= IPV4_FLAG_MF;
        frag_flags |= (uint16_t)((offset / 8) & IPV4_FRAG_OFFSET_MASK);

        uint16_t total_len = (uint16_t)(sizeof(ipv4_header_t) + chunk);
        if (total_len > sizeof(ipv4_send_buf)) return -1;

        uint64_t iflags;
        spin_lock_irqsave(&ipv4_send_lock, &iflags);

        ipv4_header_t* ip = (ipv4_header_t*)ipv4_send_buf;
        ip->version_ihl = 0x45;
        ip->tos = 0;
        ip->total_length = net_htons(total_len);
        ip->identification = net_htons(identification);
        ip->flags_fragment = net_htons(frag_flags);
        ip->ttl = ttl;
        ip->protocol = protocol;
        ip->checksum = 0;
        ip->src_addr = net_htonl(src_ip);
        ip->dst_addr = net_htonl(dst_ip);
        ip->checksum = ipv4_checksum(ip, sizeof(ipv4_header_t));

        for (uint16_t i = 0; i < chunk; i++)
            ipv4_send_buf[sizeof(ipv4_header_t) + i] = payload[offset + i];

        int ret;
        if (out_dev == net_get_loopback())
            ret = out_dev->send(out_dev, ipv4_send_buf, total_len);
        else
            ret = eth_send(out_dev, dst_mac, ETH_P_IP, ipv4_send_buf, total_len);

        spin_unlock_irqrestore(&ipv4_send_lock, iflags);
        if (ret < 0) return ret;
        offset = (uint16_t)(offset + chunk);
    }

    return 0;
}

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
    return ipv4_send_common(dev, dst_ip, protocol, payload, len, 64);
}

// Send IPv4 packet with custom TTL
int ipv4_send_ttl(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                  const uint8_t* payload, uint16_t len, uint8_t ttl) {
    return ipv4_send_common(dev, dst_ip, protocol, payload, len, ttl);
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
    uint16_t frag = net_ntohs(ip->flags_fragment);

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

    if ((frag & IPV4_FLAG_MF) || (frag & IPV4_FRAG_OFFSET_MASK)) {
        ipv4_reassemble_fragment(dev, ip, src_ip, dst_ip, payload, payload_len, ip->ttl);
        return;
    }

    ipv4_dispatch_payload(dev, src_ip, dst_ip, ip->protocol, payload, payload_len, ip->ttl);
}
