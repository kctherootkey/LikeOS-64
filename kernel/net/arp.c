// LikeOS-64 ARP (Address Resolution Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/timer.h"

// ============================================================================
// ARP Table
// ============================================================================
#define ARP_TABLE_SIZE      64
#define ARP_ENTRY_TIMEOUT   (300 * 100)  // 300 seconds in ticks (100Hz)

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint64_t timestamp;
    int      valid;
} arp_entry_t;

static arp_entry_t arp_table[ARP_TABLE_SIZE];
static spinlock_t arp_lock = SPINLOCK_INIT("arp");

void arp_init(void) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        arp_table[i].valid = 0;
    }
}

void arp_add_entry(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    uint64_t flags;
    spin_lock_irqsave(&arp_lock, &flags);

    // Check for existing entry
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            for (int m = 0; m < ETH_ALEN; m++)
                arp_table[i].mac[m] = mac[m];
            arp_table[i].timestamp = timer_ticks();
            spin_unlock_irqrestore(&arp_lock, flags);
            return;
        }
    }

    // Find free entry
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip = ip;
            for (int m = 0; m < ETH_ALEN; m++)
                arp_table[i].mac[m] = mac[m];
            arp_table[i].timestamp = timer_ticks();
            arp_table[i].valid = 1;
            spin_unlock_irqrestore(&arp_lock, flags);
            return;
        }
    }

    // Table full - evict oldest
    int oldest = 0;
    uint64_t oldest_time = arp_table[0].timestamp;
    for (int i = 1; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].timestamp < oldest_time) {
            oldest_time = arp_table[i].timestamp;
            oldest = i;
        }
    }
    arp_table[oldest].ip = ip;
    for (int m = 0; m < ETH_ALEN; m++)
        arp_table[oldest].mac[m] = mac[m];
    arp_table[oldest].timestamp = timer_ticks();
    arp_table[oldest].valid = 1;

    spin_unlock_irqrestore(&arp_lock, flags);
}

// Lookup MAC for an IP. Returns 0 on success, -1 if not cached.
int arp_resolve(net_device_t* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    // Broadcast address
    if (ip == 0xFFFFFFFF || ip == (dev->ip_addr | ~dev->netmask)) {
        for (int i = 0; i < ETH_ALEN; i++) mac_out[i] = 0xFF;
        return 0;
    }

    uint64_t flags;
    spin_lock_irqsave(&arp_lock, &flags);

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            for (int m = 0; m < ETH_ALEN; m++)
                mac_out[m] = arp_table[i].mac[m];
            spin_unlock_irqrestore(&arp_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&arp_lock, flags);

    // Not in cache - send ARP request
    arp_request(dev, ip);
    return -1;
}

// Send ARP request
void arp_request(net_device_t* dev, uint32_t target_ip) {
    uint8_t pkt[sizeof(arp_header_t)];
    arp_header_t* arp = (arp_header_t*)pkt;

    arp->hw_type = net_htons(ARP_HW_ETHER);
    arp->proto_type = net_htons(ETH_P_IP);
    arp->hw_len = ETH_ALEN;
    arp->proto_len = 4;
    arp->opcode = net_htons(ARP_OP_REQUEST);

    for (int i = 0; i < ETH_ALEN; i++) {
        arp->sender_mac[i] = dev->mac_addr[i];
        arp->target_mac[i] = 0x00;
    }
    arp->sender_ip = net_htonl(dev->ip_addr);
    arp->target_ip = net_htonl(target_ip);

    eth_send(dev, eth_broadcast_addr, ETH_P_ARP, pkt, sizeof(arp_header_t));
}

// Process received ARP packet
void arp_rx(net_device_t* dev, const uint8_t* data, uint16_t len) {
    if (len < sizeof(arp_header_t)) return;

    const arp_header_t* arp = (const arp_header_t*)data;

    if (net_ntohs(arp->hw_type) != ARP_HW_ETHER) return;
    if (net_ntohs(arp->proto_type) != ETH_P_IP) return;
    if (arp->hw_len != ETH_ALEN || arp->proto_len != 4) return;

    uint32_t sender_ip = net_ntohl(arp->sender_ip);
    uint32_t target_ip = net_ntohl(arp->target_ip);

    // Update ARP cache with sender info
    arp_add_entry(sender_ip, arp->sender_mac);

    uint16_t opcode = net_ntohs(arp->opcode);

    if (opcode == ARP_OP_REQUEST && target_ip == dev->ip_addr && dev->ip_addr != 0) {
        // Send ARP reply
        uint8_t reply[sizeof(arp_header_t)];
        arp_header_t* r = (arp_header_t*)reply;

        r->hw_type = net_htons(ARP_HW_ETHER);
        r->proto_type = net_htons(ETH_P_IP);
        r->hw_len = ETH_ALEN;
        r->proto_len = 4;
        r->opcode = net_htons(ARP_OP_REPLY);

        for (int i = 0; i < ETH_ALEN; i++) {
            r->sender_mac[i] = dev->mac_addr[i];
            r->target_mac[i] = arp->sender_mac[i];
        }
        r->sender_ip = net_htonl(dev->ip_addr);
        r->target_ip = arp->sender_ip;

        eth_send(dev, arp->sender_mac, ETH_P_ARP, reply, sizeof(arp_header_t));
    }
}
