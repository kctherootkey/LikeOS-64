// LikeOS-64 - IP Routing Table
//
// Static routing table with longest-prefix-match lookup.
// Populated by DHCP and ioctl (SIOCADDRT/SIOCDELRT).

#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/syscall.h"

// Route flags
#define RTF_UP      0x0001  // Route is usable
#define RTF_GATEWAY 0x0002  // Destination is a gateway
#define RTF_HOST    0x0004  // Host route (not network)
#define RTF_REJECT  0x0200  // Reject route

#define MAX_ROUTES  32

typedef struct {
    uint32_t dst_net;       // Destination network (host byte order)
    uint32_t netmask;       // Netmask (host byte order)
    uint32_t gateway;       // Gateway IP (host byte order), 0 if connected
    net_device_t* dev;      // Output device
    uint32_t metric;        // Route metric (lower = preferred)
    uint16_t flags;         // RTF_* flags
    int active;             // Entry in use
} rt_entry_t;

static rt_entry_t route_table[MAX_ROUTES];
static spinlock_t route_lock = SPINLOCK_INIT("route");

// Count set bits in netmask (prefix length)
static int mask_len(uint32_t mask) {
    int bits = 0;
    while (mask & 0x80000000) {
        bits++;
        mask <<= 1;
    }
    return bits;
}

void route_init(void) {
    for (int i = 0; i < MAX_ROUTES; i++)
        route_table[i].active = 0;
}

int route_add(uint32_t dst_net, uint32_t netmask, uint32_t gateway,
              net_device_t* dev, uint32_t metric, uint16_t flags) {
    uint64_t fl;
    spin_lock_irqsave(&route_lock, &fl);

    // Check for duplicate
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (route_table[i].active &&
            route_table[i].dst_net == dst_net &&
            route_table[i].netmask == netmask) {
            // Update existing route
            route_table[i].gateway = gateway;
            route_table[i].dev = dev;
            route_table[i].metric = metric;
            route_table[i].flags = flags;
            spin_unlock_irqrestore(&route_lock, fl);
            return 0;
        }
    }

    // Find free slot
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!route_table[i].active) {
            route_table[i].dst_net = dst_net;
            route_table[i].netmask = netmask;
            route_table[i].gateway = gateway;
            route_table[i].dev = dev;
            route_table[i].metric = metric;
            route_table[i].flags = flags;
            route_table[i].active = 1;
            spin_unlock_irqrestore(&route_lock, fl);
            return 0;
        }
    }

    spin_unlock_irqrestore(&route_lock, fl);
    return -ENOSPC;
}

int route_del(uint32_t dst_net, uint32_t netmask, uint32_t gateway) {
    uint64_t fl;
    spin_lock_irqsave(&route_lock, &fl);

    for (int i = 0; i < MAX_ROUTES; i++) {
        if (route_table[i].active &&
            route_table[i].dst_net == dst_net &&
            route_table[i].netmask == netmask &&
            (gateway == 0 || route_table[i].gateway == gateway)) {
            route_table[i].active = 0;
            spin_unlock_irqrestore(&route_lock, fl);
            return 0;
        }
    }

    spin_unlock_irqrestore(&route_lock, fl);
    return -ESRCH;
}

// Longest-prefix-match route lookup
// Returns: device to use, and sets *next_hop_out to the next-hop IP
net_device_t* route_lookup(uint32_t dst_ip, uint32_t* next_hop_out) {
    uint64_t fl;
    spin_lock_irqsave(&route_lock, &fl);

    int best = -1;
    int best_prefix = -1;
    uint32_t best_metric = 0xFFFFFFFF;

    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!route_table[i].active) continue;
        if (!(route_table[i].flags & RTF_UP)) continue;
        if (route_table[i].flags & RTF_REJECT) continue;

        if ((dst_ip & route_table[i].netmask) == route_table[i].dst_net) {
            int prefix = mask_len(route_table[i].netmask);
            if (prefix > best_prefix ||
                (prefix == best_prefix && route_table[i].metric < best_metric)) {
                best = i;
                best_prefix = prefix;
                best_metric = route_table[i].metric;
            }
        }
    }

    if (best < 0) {
        spin_unlock_irqrestore(&route_lock, fl);
        if (next_hop_out) *next_hop_out = dst_ip;
        return NULL;
    }

    net_device_t* dev = route_table[best].dev;
    if (next_hop_out) {
        if (route_table[best].flags & RTF_GATEWAY)
            *next_hop_out = route_table[best].gateway;
        else
            *next_hop_out = dst_ip;
    }

    spin_unlock_irqrestore(&route_lock, fl);
    return dev;
}

// Get route table entries (for /proc/net/route style display or ioctl)
int route_get_table(rt_entry_t* entries, int max_entries) {
    uint64_t fl;
    spin_lock_irqsave(&route_lock, &fl);

    int count = 0;
    for (int i = 0; i < MAX_ROUTES && count < max_entries; i++) {
        if (route_table[i].active) {
            entries[count++] = route_table[i];
        }
    }

    spin_unlock_irqrestore(&route_lock, fl);
    return count;
}

// Get route table for userspace
int net_get_route_table(net_route_info_t* entries, int max_entries) {
    uint64_t fl;
    spin_lock_irqsave(&route_lock, &fl);

    int count = 0;
    for (int i = 0; i < MAX_ROUTES && count < max_entries; i++) {
        if (!route_table[i].active) continue;
        entries[count].dst_net = route_table[i].dst_net;
        entries[count].netmask = route_table[i].netmask;
        entries[count].gateway = route_table[i].gateway;
        entries[count].flags = route_table[i].flags;
        entries[count].metric = (uint16_t)route_table[i].metric;
        if (route_table[i].dev && route_table[i].dev->name) {
            const char* n = route_table[i].dev->name;
            int j = 0;
            while (n[j] && j < 15) { entries[count].dev_name[j] = n[j]; j++; }
            entries[count].dev_name[j] = '\0';
        } else {
            entries[count].dev_name[0] = '*';
            entries[count].dev_name[1] = '\0';
        }
        count++;
    }

    spin_unlock_irqrestore(&route_lock, fl);
    return count;
}
