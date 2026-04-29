// LikeOS-64 IGMPv2 (RFC 2236) - minimal: send Membership Report on join,
// send Leave Group on drop, respond to General Query with reports for joined
// groups.  Does not maintain timers per group; queries get an immediate reply.

#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"

// IGMPv2 packet
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  max_resp;     // tenths of a second (queries) / 0 (reports)
    uint16_t checksum;
    uint32_t group_addr;   // network byte order
} igmp_packet_t;

#define IGMP_TYPE_QUERY        0x11
#define IGMP_TYPE_V1_REPORT    0x12
#define IGMP_TYPE_V2_REPORT    0x16
#define IGMP_TYPE_LEAVE        0x17

// Per-device joined-group table.
#define IGMP_MAX_GROUPS    16
typedef struct {
    int active;
    net_device_t* dev;
    uint32_t group;        // host byte order
} igmp_membership_t;

static igmp_membership_t igmp_table[IGMP_MAX_GROUPS];
static spinlock_t igmp_lock = SPINLOCK_INIT("igmp");

void igmp_init(void) {
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) igmp_table[i].active = 0;
}

static int igmp_send(net_device_t* dev, uint32_t dst_group, uint8_t type) {
    if (!dev) return -1;
    igmp_packet_t pkt;
    pkt.type = type;
    pkt.max_resp = 0;
    pkt.checksum = 0;
    pkt.group_addr = net_htonl(dst_group);
    pkt.checksum = ipv4_checksum(&pkt, sizeof(pkt));

    // IGMP messages must use TTL=1 (RFC 2236 sec 9).  Destination IP is the
    // group itself for reports, 224.0.0.2 (all-routers) for leaves.
    uint32_t dst_ip = (type == IGMP_TYPE_LEAVE) ? 0xE0000002U : dst_group;
    return ipv4_send_full(dev, dst_ip, /*proto*/ 2, (const uint8_t*)&pkt,
                          sizeof(pkt), 1, 0);
}

int igmp_join(net_device_t* dev, uint32_t group) {
    if (!dev || (group >> 28) != 0xE) return -1;

    uint64_t flags;
    spin_lock_irqsave(&igmp_lock, &flags);

    int slot = -1;
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_table[i].active && igmp_table[i].dev == dev &&
            igmp_table[i].group == group) {
            spin_unlock_irqrestore(&igmp_lock, flags);
            return 0;  // already joined
        }
        if (!igmp_table[i].active && slot < 0) slot = i;
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&igmp_lock, flags);
        return -1;
    }
    igmp_table[slot].active = 1;
    igmp_table[slot].dev = dev;
    igmp_table[slot].group = group;
    spin_unlock_irqrestore(&igmp_lock, flags);

    // Send unsolicited Membership Report (RFC 2236 sec 3).
    if (dev != net_get_loopback())
        igmp_send(dev, group, IGMP_TYPE_V2_REPORT);
    return 0;
}

int igmp_leave(net_device_t* dev, uint32_t group) {
    uint64_t flags;
    spin_lock_irqsave(&igmp_lock, &flags);
    int found = 0;
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_table[i].active && igmp_table[i].dev == dev &&
            igmp_table[i].group == group) {
            igmp_table[i].active = 0;
            found = 1;
        }
    }
    spin_unlock_irqrestore(&igmp_lock, flags);
    if (found && dev != net_get_loopback())
        igmp_send(dev, group, IGMP_TYPE_LEAVE);
    return found ? 0 : -1;
}

void igmp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    (void)src_ip;
    if (len < sizeof(igmp_packet_t)) return;
    if (ipv4_checksum(data, len) != 0) return;

    const igmp_packet_t* pkt = (const igmp_packet_t*)data;
    if (pkt->type != IGMP_TYPE_QUERY) return;

    uint32_t qgroup = net_ntohl(pkt->group_addr);

    // Reply with a Membership Report for each joined group on this device.
    uint64_t flags;
    spin_lock_irqsave(&igmp_lock, &flags);
    uint32_t reports[IGMP_MAX_GROUPS];
    int nrep = 0;
    for (int i = 0; i < IGMP_MAX_GROUPS && nrep < IGMP_MAX_GROUPS; i++) {
        if (!igmp_table[i].active || igmp_table[i].dev != dev) continue;
        if (qgroup == 0 || qgroup == igmp_table[i].group)
            reports[nrep++] = igmp_table[i].group;
    }
    spin_unlock_irqrestore(&igmp_lock, flags);

    for (int i = 0; i < nrep; i++)
        igmp_send(dev, reports[i], IGMP_TYPE_V2_REPORT);
}
