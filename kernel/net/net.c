// LikeOS-64 Network Subsystem - Device Registry and Initialization
#include "../../include/kernel/net.h"
#include "../../include/kernel/e1000.h"
#include "../../include/kernel/e1000e.h"
#include "../../include/kernel/rtl8139.h"
#include "../../include/kernel/pcnet32.h"
#include "../../include/kernel/ne2k.h"
#include "../../include/kernel/vmxnet3.h"
#include "../../include/kernel/eepro100.h"
#include "../../include/kernel/igb.h"
#include "../../include/kernel/tulip.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/random.h"

// Broadcast MAC address
const uint8_t eth_broadcast_addr[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Hostname (settable from userspace)
static char g_hostname[64] = "r00tbox";

void net_set_hostname(const char* name) {
    int i = 0;
    while (name[i] && i < 63) {
        g_hostname[i] = name[i];
        i++;
    }
    g_hostname[i] = '\0';
}

const char* net_get_hostname(void) {
    return g_hostname;
}

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

// Called from acpi_poweroff() before writing SLP_TYP|SLP_EN.  Each NIC
// driver may register a shutdown() callback that masks its interrupts,
// stops bus-master DMA, and clears any Wake-on-LAN enables.  Skipping
// this leaves PME# / WUC asserted on PCH-LAN integrated NICs (notably
// the I219 LOM on Lenovo business laptops), which causes the chipset
// to refuse the S5 transition: screen goes black and fans stop, but
// the power rail stays hot until the user presses the power button.
void net_quiesce_for_poweroff(void) {
    // No locking — by the time we get here, IRQs have been masked by
    // the acpi_poweroff() caller is about to mask them, and we want
    // to run even if the registry lock is held by some stuck path.
    for (int i = 0; i < net_device_num; i++) {
        net_device_t* d = net_devices[i];
        if (d && d->shutdown) {
            d->shutdown(d);
        }
    }
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

// Deferred loopback processing to avoid re-entrant lock deadlocks.
// A small queue is needed because TCP and fragmented IPv4 can emit multiple
// packets before the blocking syscall loop gets a chance to drain them.
#define LOOPBACK_PENDING_QUEUE_SIZE 64

typedef struct {
    uint8_t buf[NET_MTU_DEFAULT + 64];
    uint16_t len;
    net_device_t* dev;
} loopback_pending_pkt_t;

static loopback_pending_pkt_t lo_pending_queue[LOOPBACK_PENDING_QUEUE_SIZE];
static uint8_t lo_pending_head = 0;
static uint8_t lo_pending_tail = 0;
static spinlock_t lo_pending_lock = SPINLOCK_INIT("lo_pending");

static int loopback_send(net_device_t* dev, const uint8_t* data, uint16_t len) {
    // Buffer packet for deferred processing — caller may hold locks
    dev->tx_packets++;
    dev->tx_bytes += len;
    dev->rx_packets++;
    dev->rx_bytes += len;
    if (len <= sizeof(lo_pending_queue[0].buf)) {
        uint64_t flags;
        spin_lock_irqsave(&lo_pending_lock, &flags);

        uint8_t next_tail = (uint8_t)((lo_pending_tail + 1) % LOOPBACK_PENDING_QUEUE_SIZE);
        if (next_tail != lo_pending_head) {
            loopback_pending_pkt_t* pkt = &lo_pending_queue[lo_pending_tail];
            for (uint16_t i = 0; i < len; i++)
                pkt->buf[i] = data[i];
            pkt->len = len;
            pkt->dev = dev;
            lo_pending_tail = next_tail;
        }

        spin_unlock_irqrestore(&lo_pending_lock, flags);
    }
    return 0;
}

// Process deferred loopback packets (call only when no network locks are held)
void loopback_process_pending(void) {
    while (1) {
        loopback_pending_pkt_t pkt;
        int has_packet = 0;
        uint64_t flags;

        spin_lock_irqsave(&lo_pending_lock, &flags);
        if (lo_pending_head != lo_pending_tail) {
            loopback_pending_pkt_t* queued = &lo_pending_queue[lo_pending_head];
            pkt = *queued;
            lo_pending_head = (uint8_t)((lo_pending_head + 1) % LOOPBACK_PENDING_QUEUE_SIZE);
            has_packet = 1;
        }
        spin_unlock_irqrestore(&lo_pending_lock, flags);

        if (!has_packet)
            break;

        ipv4_rx(pkt.dev, pkt.buf, pkt.len);
    }
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
    e1000e_init();
    rtl8139_init();
    pcnet32_init();
    ne2k_init();
    vmxnet3_init();
    eepro100_init();
    igb_init();
    tulip_init();

    // If we have a NIC, initialize DHCP state (discover sent later after sti)
    net_device_t* dev = net_get_default_device();
    if (dev) {
        kprintf("NET: %s: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->name,
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        dhcp_init();
    } else {
        kprintf("NET: No network devices found\n");
    }
}

void net_start_dhcp(void) {
    net_device_t* dev = net_get_default_device();
    if (dev && !dhcp_configured()) {
        dhcp_discover(dev);
    }
}
