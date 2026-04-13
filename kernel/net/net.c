// LikeOS-64 Network Subsystem - Device Registry and Initialization
#include "../../include/kernel/net.h"
#include "../../include/kernel/e1000.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/random.h"

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

// Loopback device
static net_device_t lo_device;
static int lo_initialized = 0;

static int loopback_send(net_device_t* dev, const uint8_t* data, uint16_t len) {
    // Feed packets back into the IP layer
    // The data is a raw IPv4 packet (no Ethernet framing)
    dev->tx_packets++;
    dev->tx_bytes += len;
    dev->rx_packets++;
    dev->rx_bytes += len;
    ipv4_rx(dev, data, len);
    return 0;
}

static int loopback_link_status(net_device_t* dev) {
    (void)dev;
    return 1; // Always up
}

net_device_t* net_get_loopback(void) {
    return lo_initialized ? &lo_device : NULL;
}

void net_init(void) {
    kprintf("NET: Initializing network subsystem\n");

    // Initialize CSPRNG first (needed by TCP, DHCP, etc.)
    random_init();

    // Initialize loopback device
    mm_memset(&lo_device, 0, sizeof(lo_device));
    lo_device.name = "lo";
    mm_memset(lo_device.mac_addr, 0, ETH_ALEN);
    lo_device.mtu = 65535;
    lo_device.ip_addr = 0x7F000001;  // 127.0.0.1 (host byte order)
    lo_device.netmask = 0xFF000000;  // 255.0.0.0
    lo_device.gateway = 0;
    lo_device.dns_server = 0;
    lo_device.send = loopback_send;
    lo_device.link_status = loopback_link_status;
    lo_device.lock = (spinlock_t)SPINLOCK_INIT("lo");
    lo_initialized = 1;

    arp_init();
    udp_init();
    tcp_init();
    socket_init();

    // Initialize routing table
    route_init();

    // Initialize DNS resolver
    dns_init();

    // Add loopback route: 127.0.0.0/8 -> lo
    route_add(0x7F000000, 0xFF000000, 0, &lo_device, 0, RTF_UP);

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
