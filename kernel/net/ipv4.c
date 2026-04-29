// LikeOS-64 IPv4 Layer
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/random.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/skb.h"

// TX path uses per-fragment sk_buff allocations from the size-classed pool;
// no global TX spinlock is held across the lower-layer send.  See
// include/kernel/skb.h for the deadlock + TLB-IPI rationale.

// ============================================================================
// RFC 1191 Path-MTU Discovery cache (LRU)
// ============================================================================
static pmtu_entry_t pmtu_cache[PMTU_CACHE_SIZE];
static spinlock_t pmtu_lock = SPINLOCK_INIT("pmtu");

void pmtu_set(uint32_t dst_ip, uint16_t mtu) {
    if (mtu < 68) mtu = 68;     // RFC 791 minimum
    uint64_t f; spin_lock_irqsave(&pmtu_lock, &f);
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < PMTU_CACHE_SIZE; i++) {
        if (pmtu_cache[i].dst_ip == dst_ip) {
            pmtu_cache[i].mtu = mtu;
            pmtu_cache[i].last_use = timer_ticks();
            spin_unlock_irqrestore(&pmtu_lock, f);
            return;
        }
        if (pmtu_cache[i].dst_ip == 0 && free_slot < 0) free_slot = i;
        if (pmtu_cache[i].last_use < pmtu_cache[oldest].last_use) oldest = i;
    }
    int slot = (free_slot >= 0) ? free_slot : oldest;
    pmtu_cache[slot].dst_ip = dst_ip;
    pmtu_cache[slot].mtu = mtu;
    pmtu_cache[slot].last_use = timer_ticks();
    spin_unlock_irqrestore(&pmtu_lock, f);
}

uint16_t pmtu_get(uint32_t dst_ip) {
    uint64_t f; spin_lock_irqsave(&pmtu_lock, &f);
    for (int i = 0; i < PMTU_CACHE_SIZE; i++) {
        if (pmtu_cache[i].dst_ip == dst_ip) {
            pmtu_cache[i].last_use = timer_ticks();
            uint16_t m = pmtu_cache[i].mtu;
            spin_unlock_irqrestore(&pmtu_lock, f);
            return m;
        }
    }
    spin_unlock_irqrestore(&pmtu_lock, f);
    return 0;
}

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
                                  uint16_t payload_len, uint8_t ttl, uint8_t tos) {
    switch (protocol) {
    case IP_PROTO_ICMP:
        icmp_rx(dev, src_ip, payload, payload_len, ttl);
        break;
    case IP_PROTO_UDP:
        udp_rx(dev, src_ip, dst_ip, payload, payload_len, ttl, tos);
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
                          slot->data, slot->total_len, ttl, ip->tos);
    slot->active = 0;
    return 1;
}

static int ipv4_send_common(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                            const uint8_t* payload, uint16_t len, uint8_t ttl,
                            uint8_t tos) {
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
    int dst_mac_known = 0;
    int is_mcast = (dst_ip >= 0xE0000000U && dst_ip < 0xF0000000U);
    if (out_dev != net_get_loopback()) {
        if (dst_ip == 0xFFFFFFFF) {
            for (int i = 0; i < ETH_ALEN; i++) dst_mac[i] = 0xFF;
            dst_mac_known = 1;
        } else if (is_mcast) {
            // RFC 1112: multicast MAC = 01:00:5e + low 23 bits of group.
            dst_mac[0] = 0x01; dst_mac[1] = 0x00; dst_mac[2] = 0x5E;
            dst_mac[3] = (uint8_t)((dst_ip >> 16) & 0x7F);
            dst_mac[4] = (uint8_t)((dst_ip >> 8) & 0xFF);
            dst_mac[5] = (uint8_t)(dst_ip & 0xFF);
            dst_mac_known = 1;
        } else if (arp_resolve(out_dev, next_hop, dst_mac) == 0) {
            dst_mac_known = 1;
        }
        // ARP miss: arp_resolve already fired arp_request().  We will queue
        // each per-fragment skb via arp_queue_pending() below; the queued
        // skb is transmitted from arp_drain_pending() once the reply lands.
        // No upper-layer lock is held across this path, so no busy-wait.
    }

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

        sk_buff_t* skb = skb_alloc(total_len);
        if (!skb) return -1;
        skb->dev = out_dev;

        uint8_t* buf = skb_append(skb, total_len);
        ipv4_header_t* ip = (ipv4_header_t*)buf;
        ip->version_ihl = 0x45;
        ip->tos = tos;
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
            buf[sizeof(ipv4_header_t) + i] = payload[offset + i];

        int ret;
        if (out_dev == net_get_loopback()) {
            ret = out_dev->send(out_dev, skb->data, skb->len);
            skb_put(skb);
        } else if (dst_mac_known) {
            ret = eth_send(out_dev, dst_mac, ETH_P_IP, skb->data, skb->len);
            skb_put(skb);
        } else {
            // ARP not yet resolved -- transfer skb ownership to ARP pending
            // queue.  arp_drain_pending() will transmit it once the reply
            // arrives.  Treat as success so upper layers (TCP/UDP/ICMP) do
            // not retry immediately.
            if (arp_queue_pending(out_dev, next_hop, skb) < 0) {
                // Pool full: drop and surface error so caller can retry.
                skb_put(skb);
                return -1;
            }
            ret = 0;
        }
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
    return ipv4_send_common(dev, dst_ip, protocol, payload, len, 64, 0);
}

// Send IPv4 packet with custom TTL
int ipv4_send_ttl(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                  const uint8_t* payload, uint16_t len, uint8_t ttl) {
    return ipv4_send_common(dev, dst_ip, protocol, payload, len, ttl, 0);
}

// Send IPv4 packet with custom TTL and TOS (for SOCK_RAW / IP_TTL / IP_TOS).
int ipv4_send_full(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                   const uint8_t* payload, uint16_t len,
                   uint8_t ttl, uint8_t tos) {
    return ipv4_send_common(dev, dst_ip, protocol, payload, len, ttl, tos);
}

// Send raw IPv4 frame (caller-supplied IP header). Used for SOCK_RAW + IP_HDRINCL.
int ipv4_send_raw(net_device_t* dev, uint32_t dst_ip,
                  const uint8_t* full_ip_packet, uint16_t total_len) {
    if (!dev || !full_ip_packet || total_len < sizeof(ipv4_header_t)) return -1;
    uint32_t next_hop = dst_ip;
    net_device_t* out_dev = route_lookup(dst_ip, &next_hop);
    if (!out_dev) out_dev = dev;
    if (!out_dev) return -1;

    if (out_dev == net_get_loopback())
        return out_dev->send(out_dev, full_ip_packet, total_len);

    uint8_t dst_mac[ETH_ALEN];
    int is_mcast = (dst_ip >= 0xE0000000U && dst_ip < 0xF0000000U);
    if (is_mcast) {
        dst_mac[0] = 0x01; dst_mac[1] = 0x00; dst_mac[2] = 0x5e;
        dst_mac[3] = (uint8_t)((dst_ip >> 16) & 0x7F);
        dst_mac[4] = (uint8_t)((dst_ip >> 8) & 0xFF);
        dst_mac[5] = (uint8_t)(dst_ip & 0xFF);
    } else if (dst_ip == 0xFFFFFFFFU) {
        for (int i = 0; i < ETH_ALEN; i++) dst_mac[i] = 0xFF;
    } else {
        if (arp_resolve(out_dev, next_hop, dst_mac) < 0) {
            // Defer raw frame via ARP pending queue; copy into an skb so we
            // can hand off ownership.
            sk_buff_t* skb = skb_alloc(total_len);
            if (!skb) return -1;
            skb->dev = out_dev;
            uint8_t* buf = skb_append(skb, total_len);
            for (uint16_t i = 0; i < total_len; i++) buf[i] = full_ip_packet[i];
            if (arp_queue_pending(out_dev, next_hop, skb) < 0) {
                skb_put(skb);
                return -1;
            }
            return 0;
        }
    }

    return eth_send(out_dev, dst_mac, ETH_P_IP, full_ip_packet, total_len);
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

    // Accept packets addressed to us, broadcast, loopback, multicast, or if we have no IP yet (DHCP)
    // On loopback device, accept all packets (we explicitly routed them there)
    int is_mcast = (dst_ip >= 0xE0000000U && dst_ip < 0xF0000000U);
    if (dev != net_get_loopback() &&
        dst_ip != dev->ip_addr && dst_ip != 0xFFFFFFFF &&
        dst_ip != (dev->ip_addr | ~dev->netmask) &&
        (dst_ip & 0xFF000000) != 0x7F000000 &&
        !is_mcast &&
        dev->ip_addr != 0) {
        return;
    }

    const uint8_t* payload = data + ihl;
    uint16_t payload_len = total_len - ihl;

    if ((frag & IPV4_FLAG_MF) || (frag & IPV4_FRAG_OFFSET_MASK)) {
        ipv4_reassemble_fragment(dev, ip, src_ip, dst_ip, payload, payload_len, ip->ttl);
        return;
    }

    // SOCK_RAW dispatch: hand a copy of the full IP packet to any matching
    // raw sockets, in addition to normal protocol demux.
    raw_socket_deliver(src_ip, dst_ip, ip->protocol, data, total_len);

    // IGMP demux (protocol 2)
    if (ip->protocol == 2) {
        igmp_rx(dev, src_ip, payload, payload_len);
        return;
    }

    ipv4_dispatch_payload(dev, src_ip, dst_ip, ip->protocol, payload, payload_len, ip->ttl, ip->tos);
}
