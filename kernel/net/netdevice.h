// LikeOS — network device core: transmit queue, NAPI, link state, watchdog
//
// struct net_device itself is in kernel/net/net.h (drivers embed it); this
// header defines the core's parts of it and the core API.
//
// Transmit contract
// -----------------
// dev_queue_xmit(skb) takes a complete layer-2 frame (skb->dev set) and
// always consumes the skb.  The frame goes onto the device's transmit
// queue (bounded by dev->tx_queue_len; beyond it the frame is dropped and
// counted in tx_dropped) and the queue is run -- frames handed to the
// driver in order, by one CPU at a time:
//   * right away in the caller's context when interrupts are enabled.
//     Every lock of the network stack is taken with interrupts disabled,
//     so such a caller holds none of them and the driver is never entered
//     under a protocol lock;
//   * otherwise (a protocol lock is held, or hard-IRQ context) the device
//     is put on this CPU's transmit list and the NET_TX softirq raised.
//     A protocol that drops its last interrupts-off lock calls
//     net_tx_flush_local() to send at once what it queued meanwhile; the
//     TCP and socket lock-release helpers do.
// A driver that cannot take a frame (its ring is full) answers
// NETDEV_TX_BUSY (an skb driver, dev->xmit) or -1 (a legacy driver,
// dev->send): the frame goes back to the head of the queue and the queue
// is retried at the next transmit, from the NET_TX softirq and at the next
// tick.  A device that has refused every frame for NETDEV_TX_STUCK_MS is
// treated as unable to send: its queue is dropped and it gets one attempt
// per frame from then on, until it accepts one again.  Devices flagged
// IFF_NO_QUEUE (loopback) are called directly, in any context.
//
// Receive (NAPI)
// --------------
// A driver calls napi_schedule() from its interrupt; the NET_RX softirq --
// always on CPU NET_RX_CPU, so every received frame is processed in order
// by one CPU -- calls the napi's poll(budget) which delivers up to `budget'
// frames with napi_gro_receive()/netif_receive_skb() and calls
// napi_complete_done() when it did less than its budget.  net_rx_action()
// polls with a weight of NAPI_POLL_WEIGHT per napi, a total budget of
// netdev_budget frames and netdev_budget_usecs per run, and raises itself
// again when work is left.  Drivers that are not converted yet call
// net_rx_packet() from their interrupt; the frame goes to the backlog,
// which is itself a napi (bounded by netdev_max_backlog).
//
// Lock order (all taken with interrupts disabled, all leaves except where
// noted): dev registry lock; txq.q.lock; the per-CPU transmit list (IRQs
// off only); napi poll-list lock; backlog queue lock; mc list lock (taken
// under igmp_lock by __dev_mc_add/__dev_mc_del).  None is held across a
// driver call or a protocol call.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_NETDEVICE_H
#define LIKEOS_NETDEVICE_H

#include <kernel/uapi/types.h>
#include <kernel/net/skb.h>
#include <kernel/net/net_timer.h>

struct net_device;
typedef struct net_device net_device_t;

// ---- device flags: dev->flags holds the ifreq IFF_* values (net.h);
// IFF_UP is administrative, IFF_RUNNING is the carrier ----

// ---- private flags (dev->priv_flags) ----
#define IFF_NO_QUEUE 0x1 // no transmit queue: xmit called directly
#define IFF_LIVE_ADDR_CHANGE 0x2

// ---- features (dev->features) ----
#define NETIF_F_SG (1ULL << 0)
#define NETIF_F_IP_CSUM (1ULL << 1) // TX checksum offload (IPv4 TCP/UDP)
#define NETIF_F_RXCSUM (1ULL << 2) // RX checksum offload
#define NETIF_F_TSO (1ULL << 3)
#define NETIF_F_GRO (1ULL << 4) // merge received TCP segments
#define NETIF_F_LLTX (1ULL << 5) // driver does its own TX locking
#define NETIF_F_HW_VLAN (1ULL << 6)
#define NETIF_F_LOOPBACK (1ULL << 7)

// ---- transmit return codes ----
#define NETDEV_TX_OK 0x00
#define NETDEV_TX_BUSY 0x10
#define NET_XMIT_SUCCESS 0
#define NET_XMIT_DROP (-1)

#define NETDEV_TX_QUEUE_LEN 1000 // default dev->tx_queue_len
#define NETDEV_WATCHDOG_MS 5000 // default dev->watchdog_timeo
#define NETDEV_TX_WEIGHT 64 // frames per queue run
#define NETDEV_TX_STUCK_MS 100

// ---- receive budgets ----
#define NAPI_POLL_WEIGHT 64
#define NETDEV_BUDGET 300
#define NETDEV_BUDGET_USECS 2000
#define NETDEV_MAX_BACKLOG 1000
#define NET_RX_CPU 0 // the CPU every frame is processed on

#define NET_RX_SUCCESS 0
#define NET_RX_DROP 1

// ---- notifier events ----
#define NETDEV_UP 0x0001 // administratively up (registration, SIOCSIFFLAGS)
#define NETDEV_DOWN 0x0002 // administratively down (SIOCSIFFLAGS)
#define NETDEV_CHANGE 0x0004 // carrier (IFF_RUNNING) changed
#define NETDEV_REGISTER 0x0005
#define NETDEV_CHANGEMTU 0x0007 // dev->mtu changed (SIOCSIFMTU)
#define NETDEV_CHANGEADDR 0x0008 // the interface's address or netmask
	// changed (SIOCSIFADDR/SIOCSIFNETMASK, a DHCP lease bound, lost or
	// released): what was learned on the old network is stale

struct netdev_notifier {
	struct netdev_notifier *next;
	void (*fn)(struct netdev_notifier *nb, unsigned long event,
		   net_device_t *dev);
};

// Register a notifier (boot time).  Called from process or softirq
// context, interrupts enabled, no lock held.
void register_netdevice_notifier(struct netdev_notifier *nb);
// Raise `event' for `dev' (every notifier, in the caller's context).
// Process or softirq context, no lock held: whoever changes what the event
// is about calls this after the change.
void call_netdevice_notifiers(unsigned long event, net_device_t *dev);

// The device's transmit queue (dev->txq).
#define TXQ_XOFF 0x1u // stopped by the driver
#define TXQ_RUNNING 0x2u // a CPU is handing frames to the driver
#define TXQ_SCHED 0x4u // on a CPU's transmit list
#define TXQ_STUCK 0x8u // refused everything for NETDEV_TX_STUCK_MS

struct netdev_queue {
	skb_queue_t q;
	volatile uint32_t state; // TXQ_*
	uint32_t _pad;
	uint64_t trans_start; // tick of the last frame the driver took
	uint64_t busy_since; // tick the driver began refusing, 0 = no
	net_device_t *sched_next; // per-CPU transmit list linkage
	uint64_t requeues; // frames refused and put back
	uint64_t timeouts; // watchdog: transmit stalled
	uint32_t busy_retries; // immediate retries in this busy episode
	uint32_t _pad2;
	net_timer_t retry; // retry a refusing driver at the next tick
};

// Multicast hardware addresses the driver should receive (dev->mc).
#define NETDEV_MC_MAX 32
struct netdev_hw_addr {
	uint8_t addr[6];
	uint16_t users;
};

struct netdev_mc_list {
	spinlock_t lock;
	uint32_t count;
	uint32_t overflow; // more addresses wanted than fit: receive all
	struct netdev_hw_addr a[NETDEV_MC_MAX];
};

// 64-bit statistics (dev_get_stats64).
struct net_device_stats64 {
	uint64_t rx_packets, tx_packets;
	uint64_t rx_bytes, tx_bytes;
	uint64_t rx_errors, tx_errors;
	uint64_t rx_dropped, tx_dropped;
	uint64_t multicast, collisions;
	uint64_t rx_length_errors, rx_over_errors, rx_crc_errors;
	uint64_t rx_frame_errors, rx_fifo_errors, rx_missed_errors;
	uint64_t tx_aborted_errors, tx_carrier_errors, tx_fifo_errors;
	uint64_t tx_timeouts, tx_requeues;
	uint64_t rx_backlog_drops;
};

// ---- NAPI ----
#define NAPI_STATE_SCHED 0x1u // scheduled or being polled
#define NAPI_STATE_MISSED 0x2u // scheduled again while polled
#define NAPI_STATE_DISABLE 0x4u // being disabled
#define NAPI_GRO_MAX_HELD 8 // flows held for merging per napi

struct napi_struct {
	struct napi_struct *poll_next; // the poll list
	net_device_t *dev;
	int (*poll)(struct napi_struct *n, int budget);
	int weight;
	volatile uint32_t state; // NAPI_STATE_*
	uint32_t gro_count;
	sk_buff_t *gro_held[NAPI_GRO_MAX_HELD]; // oldest first
	uint64_t polls; // poll calls
	uint64_t frames; // frames the polls reported
};

void netif_napi_add(net_device_t *dev, struct napi_struct *n,
		    int (*poll)(struct napi_struct *, int), int weight);
void napi_enable(struct napi_struct *n);
// Wait until a running poll is done and keep it from being scheduled.
// Process context, interrupts enabled.
void napi_disable(struct napi_struct *n);
// From the interrupt handler (any context): poll `n' soon on NET_RX_CPU.
// Returns 1 if this call scheduled it.
int napi_schedule(struct napi_struct *n);
// From poll(), when it did less than its budget.  Returns 1 when the napi
// is now idle (the driver re-enables its interrupt), 0 when it was
// scheduled again meanwhile (the interrupt stays off; poll runs again).
int napi_complete_done(struct napi_struct *n, int work_done);
// Deliver a received frame from poll(): merged with held segments of its
// TCP flow when GRO is on for the device (NETIF_F_GRO and the global
// switch), otherwise right away.
void napi_gro_receive(struct napi_struct *n, sk_buff_t *skb);
void napi_gro_flush(struct napi_struct *n);
// Deliver one frame to the protocols now (softirq on NET_RX_CPU).  Consumes
// the skb.
int netif_receive_skb(sk_buff_t *skb);
// Queue a frame for the protocols from any context (the backlog napi).
// Consumes the skb.  NET_RX_DROP when the backlog is full.
int netif_rx(sk_buff_t *skb);

/* GRO needs this switch (on by default) and NETIF_F_GRO on the device,
 * which netif_napi_add() sets for every device with a NAPI context (its
 * poll hands frames to napi_gro_receive).  TCP takes a merged segment as
 * the segments it holds (cb->segs: the peer's MSS estimate, segment
 * counts, one ACK for all of them).  A merge is delivered in a buffer
 * sized for its data (skb_copy_fit), so the receive queue is charged
 * about what it carries. */
extern int net_gro_enabled;

struct napi_stats {
	uint64_t rx_action_runs;
	uint64_t time_squeeze; // a run ended by budget/time with work left
	uint64_t backlog_drops;
	uint64_t backlog_frames;
	uint64_t gro_merged; // segments merged into held ones
	uint64_t gro_flushed; // merged segments delivered
	uint64_t gro_held_max;
	uint32_t backlog_len;
	uint32_t _pad;
};
void napi_get_stats(struct napi_stats *out);
uint32_t net_backlog_len(void);
void napi_init_subsys(void); // net_init()

// ---- transmit ----
int dev_queue_xmit(sk_buff_t *skb);
void net_tx_flush_local(void); // run this CPU's deferred transmits now
void netif_schedule(net_device_t *dev);
void netif_stop_queue(net_device_t *dev);
void netif_wake_queue(net_device_t *dev); // any context
int netif_queue_stopped(const net_device_t *dev);
void dev_core_init_subsys(void); // net_init()

// ---- link state ----
void netif_carrier_on(net_device_t *dev); // any context
void netif_carrier_off(net_device_t *dev); // any context
int netif_carrier_ok(const net_device_t *dev);
// Administrative state (SIOCSIFFLAGS): IFF_UP in `flags' brings the device
// up (NETDEV_UP), its absence takes it down -- the transmit queue dropped,
// nothing sent or received, NETDEV_DOWN.  Other flags are not changed.
// Process context, no lock held.  0, or -1 for a device without the core.
int dev_change_flags(net_device_t *dev, uint32_t flags);
// Run pending link-change notifications (housekeeping softirq).
void linkwatch_run(void);

// ---- registration and the housekeeping hooks ----
// Prepare the core part of a device that is not in the registry (the
// loopback device).
void dev_init_core(net_device_t *dev);
// Watchdog: transmit timeouts and the driver's link supervision for every
// registered device (housekeeping softirq, ~10 Hz).
void dev_watchdog_run(void);

// ---- multicast list ----
int dev_mc_add(net_device_t *dev, const uint8_t addr[6]);
int dev_mc_del(net_device_t *dev, const uint8_t addr[6]);
/* The list change alone, for a caller that keeps it in step with state of
 * its own under its own lock (IGMP: the membership and the address change
 * together under igmp_lock; mc.lock nests inside).  Returns 1 when the
 * device's filter must be reprogrammed -- dev_set_rx_mode() once that lock
 * is dropped -- 0 when it need not, -1 on error (add: no device core;
 * del: address not on the list).  The drivers' set_rx_mode only flags the
 * change for their watchdog, which programs the list as it then is, so
 * late or repeated calls are harmless. */
int __dev_mc_add(net_device_t *dev, const uint8_t addr[6]);
int __dev_mc_del(net_device_t *dev, const uint8_t addr[6]);
void dev_set_rx_mode(net_device_t *dev);
// Copy the list for the driver's set_rx_mode (returns the count; *all set
// when the list overflowed and every multicast frame should be received).
int dev_mc_snapshot(net_device_t *dev, uint8_t (*out)[6], int max, int *all);

// ---- statistics ----
void dev_get_stats64(net_device_t *dev, struct net_device_stats64 *s);

#endif // LIKEOS_NETDEVICE_H
