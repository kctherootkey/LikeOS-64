// LikeOS-64 ARP (Address Resolution Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/sched.h"
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

// Blocking ARP request/reply for arping
static volatile uint32_t arp_reply_ip = 0;
static volatile uint8_t arp_reply_mac[ETH_ALEN];
static volatile int arp_reply_ready = 0;

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

    // Wake anyone waiting for an ARP reply
    if (opcode == ARP_OP_REPLY) {
        arp_reply_ip = sender_ip;
        for (int i = 0; i < ETH_ALEN; i++)
            arp_reply_mac[i] = arp->sender_mac[i];
        arp_reply_ready = 1;
        sched_wake_channel((void*)&arp_reply_ready);
    }
}

// Get ARP table entries for userspace
int net_get_arp_table(net_arp_info_t* entries, int max_entries) {
    uint64_t flags;
    spin_lock_irqsave(&arp_lock, &flags);
    int count = 0;
    for (int i = 0; i < ARP_TABLE_SIZE && count < max_entries; i++) {
        if (arp_table[i].valid) {
            entries[count].ip = arp_table[i].ip;
            for (int m = 0; m < ETH_ALEN; m++)
                entries[count].mac[m] = arp_table[i].mac[m];
            entries[count].valid = 1;
            entries[count].pad = 0;
            count++;
        }
    }
    spin_unlock_irqrestore(&arp_lock, flags);
    return count;
}

// Blocking ARP request/reply for arping
int arp_send_request(net_device_t* dev, uint32_t target_ip) {
    arp_reply_ready = 0;
    arp_reply_ip = 0;
    arp_request(dev, target_ip);
    return 0;
}

int arp_recv_reply(uint32_t target_ip, uint8_t mac_out[6], uint64_t timeout_ticks) {
    uint64_t start = timer_ticks();
    task_t* cur = sched_current();
    while (!arp_reply_ready || arp_reply_ip != target_ip) {
        if (timer_ticks() - start > timeout_ticks) return -1;

        cur->state = TASK_BLOCKED;
        cur->wait_channel = (void*)&arp_reply_ready;
        cur->wakeup_tick = start + timeout_ticks;
        sched_schedule();
        cur->wait_channel = NULL;
        cur->wakeup_tick = 0;
    }
    for (int i = 0; i < ETH_ALEN; i++)
        mac_out[i] = arp_reply_mac[i];
    arp_reply_ready = 0;
    return 0;
}
