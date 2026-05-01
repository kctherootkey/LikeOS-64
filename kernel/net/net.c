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
#include "../../include/kernel/timer.h"
#include "../../include/kernel/skb.h"
#include "../../include/kernel/softirq.h"
#include "../../include/kernel/percpu.h"

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
    static uint64_t dhcp_last = 0;
    uint64_t now = timer_ticks();
    // Timer frequency is TSC-calibrated at boot and is NOT necessarily
    // 100 Hz (e.g. on VMware it lands elsewhere).  Use the real rate so
    // dhcp_tick() runs at ~1 Hz wall-time regardless of platform.
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    if (now - dhcp_last >= hz) {
        dhcp_last = now;
        dhcp_tick();
    }
}

// Called by NIC driver on packet receive (HARD IRQ context).
//
// We do the bare minimum here -- allocate an skb from the size-classed
// pool, copy the frame, enqueue it on this CPU's RX queue, and raise the
// SOFTIRQ_NET_RX bit.  All actual parsing (eth_rx -> ip_rx -> ...) runs
// later in softirq context with interrupts ENABLED, so we never block
// TLB-shootdown IPIs nor risk re-entering protocol locks held by user
// context paths.

static skb_queue_t rx_queue[MAX_CPUS];
static int         rx_queue_inited = 0;

// All protocol-stack RX is funneled through CPU 0's rx_queue, regardless of
// which CPU received the frame from hardware or which CPU originated a
// loopback transmit.  This preserves FIFO ordering across senders that
// migrate between CPUs (e.g. tcp_send_data() emitting multiple segments
// across a preemption point would otherwise enqueue them on different
// per-CPU queues, letting the protocol stack reassemble them out of order
// and corrupt TCP byte streams).  CPU 0's ksoftirqd then sequentially
// drives ipv4_rx / tcp_rx for the entire stack.
// Hardware NIC RX is funnelled through rx_queue[NET_RX_CPU] (a single
// CPU drains the queue, so per-conn segment ordering is preserved
// without per-conn locks held across handler invocations).
//
// Loopback packets, however, are enqueued onto the *sending* CPU's own
// rx_queue and drained by that CPU's ksoftirqd.  Two reasons:
//
//   1. Self-throttling: the loopback sender's CPU does the receive
//      work, so a runaway sender on one CPU cannot DoS the others.
//
//   2. Wake reliability under hypervisors: pinning all loopback RX to
//      ksoftirqd/NET_RX_CPU means a process-context caller on a
//      different CPU must IPI CPU NET_RX_CPU and wait for *its* idle
//      thread to be preempted out of HLT.  Under VMware the IPI
//      latency was observed to stretch into multiple seconds when
//      NET_RX_CPU was deeply idle, hanging accept() / connect() /
//      recv() wait-loops in test_libc.  Draining on the local CPU
//      (where the caller is about to sched_yield_in_kernel() anyway)
//      makes wake-up immediate.
//
// Critically, EACH rx_queue is read by exactly ONE ksoftirqd (its own
// CPU's), so this still preserves per-queue ordering — no two CPUs
// race over the same rx_queue and a TCP connection's segments stay in
// the order they were enqueued by their sender.
#define NET_RX_CPU 0

// Before percpu_init(), GS base is 0; this_cpu_id() would fault.  In that
// (BSP-only) window we are always CPU 0.
static inline uint32_t net_safe_cpu_id(void) {
    return read_gs_base_msr() ? this_cpu_id() : 0;
}

static inline int skb_is_loopback(sk_buff_t* skb) {
    return skb && skb->dev && skb->dev == net_get_loopback();
}

static void net_rx_softirq(void) {
    // Drain THIS CPU's queue only.  See the rx-queue commentary above.
    skb_queue_t* q = &rx_queue[net_safe_cpu_id()];
    sk_buff_t* skb;
    while ((skb = skb_queue_head(q)) != NULL) {
        if (skb_is_loopback(skb)) {
            // Loopback packets are raw IPv4 datagrams (no Ethernet header).
            ipv4_rx(skb->dev, skb->data, skb->len);
        } else {
            eth_rx(skb->dev, skb->data, skb->len);
        }
        skb_put(skb);
    }
}

void net_rx_packet(net_device_t* dev, const uint8_t* data, uint16_t len) {
    dev->rx_packets++;
    dev->rx_bytes += len;
    if (!rx_queue_inited || len == 0) {
        // Pre-init: drop silently (should not happen after net_init()).
        return;
    }
    sk_buff_t* skb = skb_alloc(len);
    if (!skb) {
        dev->rx_errors++;
        return;
    }
    skb->dev = dev;
    uint8_t* p = skb_append(skb, len);
    for (uint16_t i = 0; i < len; i++) p[i] = data[i];
    // Hardware NIC RX: funnel to NET_RX_CPU's queue/ksoftirqd so a
    // single CPU processes all incoming wire packets in arrival order.
    skb_queue_tail(&rx_queue[NET_RX_CPU], skb);
    softirq_raise_on(NET_RX_CPU, SOFTIRQ_NET_RX);
}

// Loopback device
static net_device_t lo_device;
static int lo_initialized = 0;

// Loopback transmit: enqueue an skb on the LOCAL CPU's RX queue and
// raise SOFTIRQ_NET_RX locally.  See the rx-queue commentary at the
// top of this file for why loopback uses per-CPU queues instead of
// the single NET_RX_CPU queue used for hardware NIC RX.
static int loopback_send(net_device_t* dev, const uint8_t* data, uint16_t len) {
    dev->tx_packets++;
    dev->tx_bytes += len;
    dev->rx_packets++;
    dev->rx_bytes += len;
    if (!rx_queue_inited || len == 0) return 0;
    sk_buff_t* skb = skb_alloc(len);
    if (!skb) {
        dev->rx_errors++;
        return -1;
    }
    skb->dev = dev;
    uint8_t* p = skb_append(skb, len);
    for (uint16_t i = 0; i < len; i++) p[i] = data[i];
    uint32_t my_cpu = net_safe_cpu_id();
    skb_queue_tail(&rx_queue[my_cpu], skb);
    softirq_raise_on(my_cpu, SOFTIRQ_NET_RX);
    return 0;
}

// Compatibility shim for legacy callers (syscall.c, socket.c).  Loopback
// delivery is now driven by the SOFTIRQ_NET_RX handler.
//
// IMPORTANT: this is invoked from process context (typically a user
// task inside sock_accept / sock_connect / sock_recvfrom polling for
// an inbound packet).  Earlier versions of this function called
// softirq_drain() inline, which on a process-context caller (IRQs
// enabled) would synchronously run net_rx_softirq → ipv4_rx → tcp_rx →
// tcp_send_segment → ipv4_send → loopback_send → ...  on the *user
// task's* 16 KiB kernel stack — and then any hard IRQ (timer, e1000,
// xhci completion) would nest on top, blowing the kernel stack under
// real-USB latency (RSP wrapped to small constants like 0x20).
//
// Fix: do NOT process softirqs synchronously on the calling task's
// stack.  Just raise NET_RX on the local CPU; ksoftirqd has its own
// 16 KiB stack and the caller's wait-loop will sched_yield_in_kernel()
// immediately after this returns, giving ksoftirqd a chance to run.
//
// We raise on the LOCAL CPU (not NET_RX_CPU) because loopback packets
// are enqueued on per-CPU queues; the local CPU's ksoftirqd drains
// the local queue.
void loopback_process_pending(void) {
    if (!rx_queue_inited) return;
    uint32_t my_cpu = net_safe_cpu_id();
    if (__atomic_load_n(&rx_queue[my_cpu].len, __ATOMIC_ACQUIRE) != 0) {
        softirq_raise_on(my_cpu, SOFTIRQ_NET_RX);
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

    // Bring up the skb pool and per-CPU RX queues, then register the
    // SOFTIRQ_NET_RX handler.  Must happen before any send/receive path runs.
    skb_pool_init();
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        skb_queue_init(&rx_queue[i], "net_rx");
    }
    rx_queue_inited = 1;
    softirq_register(SOFTIRQ_NET_RX, net_rx_softirq);

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
    igmp_init();

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
