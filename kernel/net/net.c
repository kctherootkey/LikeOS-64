// LikeOS Network Subsystem - Device Registry and Initialization
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/dev/nic/e1000.h>
#include <kernel/dev/nic/e1000e.h>
#include <kernel/dev/nic/rtl8139.h>
#include <kernel/dev/nic/pcnet32.h>
#include <kernel/dev/nic/ne2k.h>
#include <kernel/dev/nic/vmxnet3.h>
#include <kernel/dev/nic/eepro100.h>
#include <kernel/dev/nic/igb.h>
#include <kernel/dev/nic/tulip.h>
#include <kernel/io/console.h>
#include <kernel/mm/slab.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/rand/random.h>
#include <kernel/net/ratelimit.h>
#include <kernel/ke/timer.h>
#include <kernel/net/skb.h>
#include <kernel/net/softirq.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/net_timer.h>
#include <kernel/ke/percpu.h>
#include <kernel/uapi/bug.h>

// Broadcast MAC address
const uint8_t eth_broadcast_addr[ETH_ALEN] = { 0xFF, 0xFF, 0xFF,
					       0xFF, 0xFF, 0xFF };

// Hostname (settable from userspace)
static char g_hostname[64] = "r00tbox";

void net_set_hostname(const char *name)
{
	int i = 0;
	while (name[i] && i < 63) {
		g_hostname[i] = name[i];
		i++;
	}
	g_hostname[i] = '\0';
}

const char *net_get_hostname(void)
{
	return g_hostname;
}

/* The device registry, the transmit queues, link state and the watchdog
 * are in dev.c; the receive side (NAPI, the backlog) in napi.c. */

static inline uint32_t net_safe_cpu_id(void);

// Called from timer IRQ
void net_timer_tick(void)
{
	// Hand the per-connection TCP timer sweep to softirq context instead of
	// running it inline here on every CPU with interrupts disabled.  Only
	// the lowest online CPU raises it, so the sweep runs once per tick (its
	// per-connection deadlines are absolute, so a single sweeper suffices).
	if (net_safe_cpu_id() != 0)
		return;
	softirq_raise(SOFTIRQ_TCP_TIMER);
	net_timer_tick_hook(); // the network timer wheel (neighbours, ...)
	/* The DHCP lease timer and the NIC link watchdogs run in the
	 * housekeeping softirq, ten times a second.  dhcp_tick() used to be
	 * called right here, from the hard timer interrupt of EVERY CPU:
	 * it builds a packet and transmits it -- skb allocation, ARP, the
	 * NIC's transmit lock -- with interrupts off, and the unlocked
	 * once-a-second check let two CPUs renew at the same time. */
	static uint64_t hk_last;
	uint64_t now = timer_ticks();
	uint64_t period = net_ms_to_ticks(100);
	if (now - hk_last >= period) {
		hk_last = now;
		softirq_raise(SOFTIRQ_NET_HOUSEKEEPING);
	}
}

void net_housekeeping_kick(void)
{
	softirq_raise_on(0, SOFTIRQ_NET_HOUSEKEEPING); // CPU 0, as below
}

/* SOFTIRQ_NET_HOUSEKEEPING, on CPU 0 only (see net_timer_tick and
 * net_housekeeping_kick), so it never runs concurrently with itself, nor
 * with the receive softirq that feeds dhcp_rx() on the same CPU. */
static void net_housekeeping_softirq(void)
{
	static uint64_t dhcp_last;
	uint64_t now = timer_ticks();
	// The calibrated rate, so dhcp_tick() runs at ~1 Hz wall time.
	uint64_t hz = timer_get_frequency();

	if (hz == 0)
		hz = 100;
	if (now - dhcp_last >= hz) {
		dhcp_last = now;
		dhcp_tick();
	}
	linkwatch_run(); // carrier changes -> notifiers
	dev_watchdog_run(); // the drivers' link supervision, TX timeouts
	/* Packet memory: top the dynamic reserve up while the static pools
	 * are short, give memory back once they are not. */
	if (skb_reserve_maintain())
		net_housekeeping_kick();
}

// Called by NIC driver on packet receive (HARD IRQ context).
//
// The bare minimum happens here: an skb from the packet pools (never the
// slab allocator: interrupts are off), a copy of the frame, and the
// backlog napi (napi.c), which the NET_RX softirq on NET_RX_CPU drains --
// so every frame is processed by one CPU in arrival order, with
// interrupts enabled, never re-entering protocol locks held by process
// context.
//
// Loopback traffic takes the same backlog (loopback_xmit): one FIFO for
// everything, so the segments of a sender that migrates between CPUs
// across a preemption point (data from one CPU, the FIN from close() on
// another) still arrive in the order they were sent.

// Before percpu_init(), GS base is 0; this_cpu_id() would fault.  In that
// (BSP-only) window we are always CPU 0.
static inline uint32_t net_safe_cpu_id(void)
{
	return read_gs_base_msr() ? this_cpu_id() : 0;
}

void net_rx_packet(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	BUG_ON(dev == NULL);
	BUG_ON(data == NULL);
	dev->rx_packets++;
	dev->rx_bytes += len;
	if (len == 0)
		return;
	WARN_RATELIMIT(len > 9000, "net_rx_packet: oversized frame len=%u",
		       len);
	sk_buff_t *skb = skb_alloc(len);
	if (!skb) {
		dev->rx_errors++;
		return;
	}
	skb->dev = dev;
	uint8_t *p = skb_append(skb, len);
	/* rep-movs-backed bulk copy -- was a byte-at-a-time loop running in
	 * hard-IRQ context per packet. */
	mm_memcpy(p, data, len);
	if (netif_rx(skb) != NET_RX_SUCCESS)
		dev->rx_dropped++;
}

// Loopback device
static net_device_t lo_device;
static int lo_initialized = 0;

/* Loopback transmit: the datagram itself (no link header) goes to the
 * backlog as a received frame -- no copy. */
static int loopback_xmit(sk_buff_t *skb, net_device_t *dev)
{
	uint32_t len = skb->len;

	dev->tx_packets++;
	dev->tx_bytes += len;
	/* The frame has left the sender: it no longer counts against the
	 * sending socket's transmit budget (TCP small queues) while it
	 * waits in the receiver's queues. */
	if (skb->destructor) {
		skb->destructor(skb);
		skb->destructor = NULL;
		skb->sk = NULL;
	}
	skb->pkt_type = PACKET_LOOPBACK;
	skb->dev = dev;
	if (netif_rx(skb) != NET_RX_SUCCESS) {
		dev->rx_dropped++;
		return NETDEV_TX_OK; // consumed (dropped) either way
	}
	dev->rx_packets++;
	dev->rx_bytes += len;
	return NETDEV_TX_OK;
}

/* The byte interface of the loopback device, for callers that hand it a
 * finished datagram without an skb. */
static int loopback_send(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	if (len == 0)
		return 0;
	sk_buff_t *skb = skb_alloc(len);
	if (!skb) {
		dev->rx_errors++;
		return -1;
	}
	mm_memcpy(skb_append(skb, len), data, len);
	skb->dev = dev;
	return loopback_xmit(skb, dev) == NETDEV_TX_OK ? 0 : -1;
}

// Compatibility shim for legacy callers (syscall.c, socket.c), called from
// process context while they wait for an inbound packet.  It must NOT
// process softirqs on the calling task's stack (the whole receive path on
// a user task's kernel stack, with hard IRQs nesting on top, overflowed
// it); it only makes sure NET_RX is raised on NET_RX_CPU when frames are
// waiting, so ksoftirqd gets to them while the caller yields.
void loopback_process_pending(void)
{
	if (net_backlog_len() != 0)
		softirq_raise_on(NET_RX_CPU, SOFTIRQ_NET_RX);
}

/* Read-only diagnostic accessors used by the Ctrl+N TCP/net dump
 * (tcp_dump_table).  No locks, no side effects. */
uint32_t net_rx_queue_len(uint32_t cpu)
{
	return cpu == NET_RX_CPU ? net_backlog_len() : 0;
}

int net_rx_cpu(void)
{
	return NET_RX_CPU;
}

static int loopback_link_status(net_device_t *dev)
{
	(void)dev;
	return 1; // Always up
}

net_device_t *net_get_loopback(void)
{
	return lo_initialized ? &lo_device : NULL;
}

void net_init(void)
{
	kprintf("NET: Initializing network subsystem\n");

	// Initialize CSPRNG first (needed by TCP, DHCP, etc.)
	random_init();

	// The packet pools, the timer wheel, the transmit (NET_TX) and
	// receive (NET_RX, backlog) halves of the device core.  Before any
	// send/receive path runs.
	skb_pool_init();
	net_timer_init_subsys();
	dev_core_init_subsys();
	napi_init_subsys();
	softirq_register(SOFTIRQ_NET_HOUSEKEEPING, net_housekeeping_softirq);
	// The routing table first: device registration notifies it.
	route_init();

	// Initialize loopback device
	mm_memset(&lo_device, 0, sizeof(lo_device));
	lo_device.name = "lo";
	mm_memset(lo_device.mac_addr, 0, ETH_ALEN);
	lo_device.mtu = 65535;
	lo_device.ip_addr = 0x7F000001; // 127.0.0.1 (host byte order)
	lo_device.netmask = 0xFF000000; // 255.0.0.0
	lo_device.gateway = 0;
	lo_device.dns_server = 0;
	lo_device.send = loopback_send;
	lo_device.xmit = loopback_xmit;
	lo_device.link_status = loopback_link_status;
	lo_device.lock = (spinlock_t)SPINLOCK_INIT("lo");
	lo_device.flags = IFF_LOOPBACK;
	lo_device.priv_flags = IFF_NO_QUEUE;
	lo_device.features = NETIF_F_LOOPBACK;
	lo_device.ifindex = 1;
	dev_init_core(&lo_device);
	lo_initialized = 1;

	ratelimit_init();
	arp_init();
	ipv4_init(); // reassembly, raw sockets, ICMP, the protocol table
	udp_init();
	tcp_init();
	socket_init();
	igmp_init();

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
	net_device_t *dev = net_get_default_device();
	if (dev) {
		kprintf("NET: %s: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->name, dev->mac_addr[0], dev->mac_addr[1],
			dev->mac_addr[2], dev->mac_addr[3], dev->mac_addr[4],
			dev->mac_addr[5]);

		dhcp_init();
	} else {
		kprintf("NET: No network devices found\n");
	}
}

void net_start_dhcp(void)
{
	net_device_t *dev = net_get_default_device();
	if (dev && !dhcp_configured()) {
		dhcp_discover(dev);
	}
}
