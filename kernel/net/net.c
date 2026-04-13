// LikeOS-64 Network Subsystem - Device Registry and Initialization
#include "../../include/kernel/net.h"
#include "../../include/kernel/e1000.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/memory.h"

// Broadcast MAC address
const uint8_t eth_broadcast_addr[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Network device registry
static net_device_t* net_devices[NET_MAX_DEVICES];
static int net_device_num = 0;
static spinlock_t net_registry_lock = SPINLOCK_INIT("net_registry");

int net_register(net_device_t* dev) {
    uint64_t flags;
    spin_lock_irqsave(&net_registry_lock, &flags);
    if (net_device_num >= NET_MAX_DEVICES) {
        spin_unlock_irqrestore(&net_registry_lock, flags);
        return -1;
    }
    net_devices[net_device_num++] = dev;
    spin_unlock_irqrestore(&net_registry_lock, flags);
    return 0;
}

net_device_t* net_get_device(int index) {
    if (index < 0 || index >= net_device_num) return NULL;
    return net_devices[index];
}

net_device_t* net_get_default_device(void) {
    return net_device_num > 0 ? net_devices[0] : NULL;
}

int net_device_count(void) {
    return net_device_num;
}

// Called from timer IRQ
void net_timer_tick(void) {
    tcp_timer_tick();
}

// Called by NIC driver on packet receive
void net_rx_packet(net_device_t* dev, const uint8_t* data, uint16_t len) {
    dev->rx_packets++;
    dev->rx_bytes += len;
    eth_rx(dev, data, len);
}

void net_init(void) {
    kprintf("NET: Initializing network subsystem\n");

    arp_init();
    udp_init();
    tcp_init();
    socket_init();

    // Probe for NIC hardware
    e1000_init();

    // If we have a NIC, start DHCP
    net_device_t* dev = net_get_default_device();
    if (dev) {
        kprintf("NET: %s: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->name,
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        dhcp_init();
        dhcp_discover(dev);
    } else {
        kprintf("NET: No network devices found\n");
    }
}
