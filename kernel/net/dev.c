// LikeOS — network device core: registry, transmit queues, link state,
// watchdog, multicast lists, statistics
//
// See kernel/net/netdevice.h for the transmit contract and the lock order.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/softirq.h>
#include <kernel/net/skb.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// Registry
// ============================================================================

static net_device_t *net_devices[NET_MAX_DEVICES];
static volatile int net_device_num;
static spinlock_t net_registry_lock = SPINLOCK_INIT("net_registry");
static int next_ifindex = 2; // 1 is the loopback device

static struct netdev_notifier *notifiers;

static inline uint32_t dev_cpu_id(void)
{
	return read_gs_base_msr() ? this_cpu_id() : 0;
}

void register_netdevice_notifier(struct netdev_notifier *nb)
{
	uint64_t f;

	spin_lock_irqsave(&net_registry_lock, &f);
	nb->next = notifiers;
	notifiers = nb;
	spin_unlock_irqrestore(&net_registry_lock, f);
}

/* Call every notifier.  The list only grows (boot time), so it is walked
 * without the lock: no notifier runs under one. */
void call_netdevice_notifiers(unsigned long event, net_device_t *dev)
{
	for (struct netdev_notifier *nb =
		     __atomic_load_n(&notifiers, __ATOMIC_ACQUIRE);
	     nb; nb = nb->next)
		nb->fn(nb, event, dev);
}

static void dev_tx_retry(net_timer_t *t);

void dev_init_core(net_device_t *dev)
{
	if (dev->core_ready)
		return;
	skb_queue_init(&dev->txq.q, "dev_txq");
	dev->txq.state = 0;
	dev->txq.trans_start = timer_ticks();
	dev->txq.busy_since = 0;
	dev->txq.busy_retries = 0;
	dev->txq.sched_next = NULL;
	net_timer_setup(&dev->txq.retry, dev_tx_retry);
	spinlock_init(&dev->mc.lock, "dev_mc");
	dev->mc.count = 0;
	dev->mc.overflow = 0;
	if (!dev->tx_queue_len)
		dev->tx_queue_len = NETDEV_TX_QUEUE_LEN;
	if (!dev->watchdog_timeo_ms)
		dev->watchdog_timeo_ms = NETDEV_WATCHDOG_MS;
	/* Administratively up with carrier: the drivers have no way yet to
	 * report a carrier of their own (they will call netif_carrier_*). */
	__atomic_fetch_or(&dev->flags, IFF_UP | IFF_RUNNING, __ATOMIC_ACQ_REL);
	__atomic_store_n(&dev->core_ready, 1, __ATOMIC_RELEASE);
}

int net_register(net_device_t *dev)
{
	uint64_t flags;

	BUG_ON(dev == NULL);
	if (!(dev->priv_flags & IFF_NO_QUEUE))
		dev->flags |= IFF_BROADCAST | IFF_MULTICAST;
	dev_init_core(dev);
	spin_lock_irqsave(&net_registry_lock, &flags);
	if (net_device_num >= NET_MAX_DEVICES) {
		WARN_ON_ONCE(
			net_device_num >=
			NET_MAX_DEVICES); /* NET_MAX_DEVICES exhausted: too many NICs registered */
		spin_unlock_irqrestore(&net_registry_lock, flags);
		return -1;
	}
	if (!dev->ifindex)
		dev->ifindex = next_ifindex++;
	net_devices[net_device_num] = dev;
	__atomic_store_n(&net_device_num, net_device_num + 1, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&net_registry_lock, flags);
	call_netdevice_notifiers(NETDEV_REGISTER, dev);
	call_netdevice_notifiers(NETDEV_UP, dev);
	return 0;
}

net_device_t *net_get_device(int index)
{
	if (index < 0 ||
	    index >= __atomic_load_n(&net_device_num, __ATOMIC_ACQUIRE))
		return NULL;
	return net_devices[index];
}

net_device_t *net_get_default_device(void)
{
	return __atomic_load_n(&net_device_num, __ATOMIC_ACQUIRE) > 0 ?
		       net_devices[0] :
		       NULL;
}

int net_device_count(void)
{
	return __atomic_load_n(&net_device_num, __ATOMIC_ACQUIRE);
}

// Called from acpi_poweroff() before writing SLP_TYP|SLP_EN.  Each NIC
// driver may register a shutdown() callback that masks its interrupts,
// stops bus-master DMA, and clears any Wake-on-LAN enables.  Skipping
// this leaves PME# / WUC asserted on PCH-LAN integrated NICs (notably
// the I219 LOM on Lenovo business laptops), which causes the chipset
// to refuse the S5 transition: screen goes black and fans stop, but
// the power rail stays hot until the user presses the power button.
void net_quiesce_for_poweroff(void)
{
	// No locking -- by the time we get here the acpi_poweroff() caller
	// is about to mask interrupts, and this has to run even if the
	// registry lock is held by some stuck path.
	for (int i = 0; i < net_device_num; i++) {
		net_device_t *d = net_devices[i];
		if (d && d->shutdown) {
			d->shutdown(d);
		}
	}
}

// ============================================================================
// Transmit
// ============================================================================

/* Devices with frames to send, per CPU, linked through txq.sched_next.
 * Only the owning CPU touches its list, with interrupts disabled. */
static net_device_t *tx_list[MAX_CPUS];

static volatile uint64_t st_tx_deferred; // frames queued for NET_TX
static volatile uint64_t st_tx_direct_runs;

static inline uint32_t txq_state(const net_device_t *dev)
{
	return __atomic_load_n(&dev->txq.state, __ATOMIC_ACQUIRE);
}

void netif_schedule(net_device_t *dev)
{
	uint64_t f;
	uint32_t cpu;

	if (__atomic_fetch_or(&dev->txq.state, TXQ_SCHED, __ATOMIC_ACQ_REL) &
	    TXQ_SCHED)
		return;
	f = local_irq_save();
	cpu = dev_cpu_id();
	dev->txq.sched_next = tx_list[cpu];
	tx_list[cpu] = dev;
	local_irq_restore(f);
	softirq_raise(SOFTIRQ_NET_TX);
}

void netif_stop_queue(net_device_t *dev)
{
	__atomic_fetch_or(&dev->txq.state, TXQ_XOFF, __ATOMIC_ACQ_REL);
}

void netif_wake_queue(net_device_t *dev)
{
	if (__atomic_fetch_and(&dev->txq.state, ~TXQ_XOFF, __ATOMIC_ACQ_REL) &
	    TXQ_XOFF)
		netif_schedule(dev);
}

int netif_queue_stopped(const net_device_t *dev)
{
	return (txq_state(dev) & TXQ_XOFF) != 0;
}

/* Hand one frame to the driver.  NETDEV_TX_OK: the driver took it (and the
 * skb is gone); NETDEV_TX_BUSY: refused, the skb is still the caller's. */
static int dev_hard_xmit(net_device_t *dev, sk_buff_t *skb)
{
	if (dev->xmit) {
		int rc = dev->xmit(skb, dev);

		return rc == NETDEV_TX_BUSY ? NETDEV_TX_BUSY : NETDEV_TX_OK;
	}
	if (!dev->send || skb->len == 0 || skb->len > 0xFFFFu) {
		dev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}
	if (dev->send(dev, skb->data, (uint16_t)skb->len) < 0)
		return NETDEV_TX_BUSY;
	skb_put(skb);
	return NETDEV_TX_OK;
}

static void txq_drop_all(net_device_t *dev)
{
	sk_buff_t *skb;
	uint32_t n = 0;

	skb = skb_queue_take_all(&dev->txq.q, &n);
	dev->tx_dropped += n;
	while (skb) {
		sk_buff_t *next = skb->next;

		skb->next = skb->prev = NULL;
		skb_put(skb);
		skb = next;
	}
}

/* Retry budget of one busy episode before the retries move to the tick. */
#define TX_BUSY_FAST_RETRIES 64

/* Hand queued frames to the driver, in order, as the only CPU doing so for
 * this device.  Returns with the queue empty, stopped, refused (retry
 * scheduled) or the quota used up (NET_TX scheduled).  One quota per call:
 * a sender whose frames keep being joined by other CPUs' hands the rest to
 * the NET_TX softirq instead of transmitting for everybody indefinitely. */
static void qdisc_run(net_device_t *dev)
{
	struct netdev_queue *txq = &dev->txq;
	int quota = NETDEV_TX_WEIGHT;

	for (;;) {
		int busy = 0;
		uint64_t now = timer_ticks();

		if (__atomic_fetch_or(&txq->state, TXQ_RUNNING,
				      __ATOMIC_ACQ_REL) &
		    TXQ_RUNNING)
			return; // another CPU runs it and will see our frames
		while (quota > 0 && !(txq_state(dev) & TXQ_XOFF)) {
			sk_buff_t *skb;
			int rc;

			if (!(__atomic_load_n(&dev->flags, __ATOMIC_ACQUIRE) &
			      IFF_UP)) {
				txq_drop_all(dev); // taken down meanwhile
				break;
			}
			skb = skb_dequeue(&txq->q);
			if (!skb)
				break;
			rc = dev_hard_xmit(dev, skb);
			quota--;
			if (rc == NETDEV_TX_OK) {
				txq->trans_start = now;
				txq->busy_since = 0;
				txq->busy_retries = 0;
				__atomic_fetch_and(&txq->state, ~TXQ_STUCK,
						   __ATOMIC_ACQ_REL);
				continue;
			}
			if (txq_state(dev) & TXQ_STUCK) {
				// one attempt per frame while stuck
				dev->tx_dropped++;
				skb_put(skb);
				continue;
			}
			skb_queue_head(&txq->q, skb);
			txq->requeues++;
			if (!txq->busy_since) {
				txq->busy_since = now ? now : 1;
			} else if (now - txq->busy_since >=
				   net_ms_to_ticks(NETDEV_TX_STUCK_MS)) {
				__atomic_fetch_or(&txq->state, TXQ_STUCK,
						  __ATOMIC_ACQ_REL);
				txq_drop_all(dev);
				txq->busy_since = 0;
				txq->busy_retries = 0;
				break;
			}
			busy = 1;
			break;
		}
		__atomic_fetch_and(&txq->state, ~TXQ_RUNNING, __ATOMIC_ACQ_REL);
		if (busy) {
			if (txq->busy_retries < TX_BUSY_FAST_RETRIES) {
				txq->busy_retries++;
				netif_schedule(dev);
			} else {
				net_timer_mod(&txq->retry, timer_ticks() + 1);
			}
			return;
		}
		if (skb_queue_empty(&txq->q) || (txq_state(dev) & TXQ_XOFF))
			return;
		if (quota <= 0) {
			netif_schedule(dev);
			return;
		}
		/* Frames were queued after our last dequeue found the queue
		 * empty and before RUNNING was cleared: their senders saw it
		 * running, so they are ours. */
	}
}

static void dev_tx_retry(net_timer_t *t)
{
	net_device_t *dev = (net_device_t *)((char *)t -
					     __builtin_offsetof(net_device_t,
								txq.retry));

	if (!skb_queue_empty(&dev->txq.q))
		netif_schedule(dev);
}

int dev_queue_xmit(sk_buff_t *skb)
{
	net_device_t *dev;
	struct netdev_queue *txq;
	uint64_t f;

	if (!skb)
		return NET_XMIT_DROP;
	dev = skb->dev;
	BUG_ON(dev == NULL);
	if (!dev->core_ready) {
		/* A device the core was never told about: send in place,
		 * as before the core existed. */
		int rc = dev->send ? dev->send(dev, skb->data,
					       (uint16_t)skb->len) :
				     -1;
		skb_put(skb);
		return rc < 0 ? NET_XMIT_DROP : NET_XMIT_SUCCESS;
	}
	if ((dev->flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) {
		dev->tx_dropped++;
		skb_put(skb);
		return NET_XMIT_DROP;
	}
	if (dev->priv_flags & IFF_NO_QUEUE) {
		if (dev_hard_xmit(dev, skb) == NETDEV_TX_BUSY) {
			dev->tx_dropped++;
			skb_put(skb);
			return NET_XMIT_DROP;
		}
		return NET_XMIT_SUCCESS;
	}
	txq = &dev->txq;
	spin_lock_irqsave(&txq->q.lock, &f);
	if (txq->q.len >= dev->tx_queue_len) {
		spin_unlock_irqrestore(&txq->q.lock, f);
		dev->tx_dropped++;
		skb_put(skb);
		return NET_XMIT_DROP;
	}
	__skb_queue_tail(&txq->q, skb);
	spin_unlock_irqrestore(&txq->q.lock, f);

	if (!irqs_disabled()) {
		/* No interrupts-off lock is held (they all disable
		 * interrupts): hand the frames over now. */
		st_tx_direct_runs++;
		qdisc_run(dev);
	} else {
		st_tx_deferred++;
		netif_schedule(dev);
	}
	return NET_XMIT_SUCCESS;
}

/* The NET_TX softirq: run every device on this CPU's transmit list. */
static void net_tx_action(void)
{
	net_device_t *list;
	uint64_t f;

	f = local_irq_save();
	list = tx_list[dev_cpu_id()];
	tx_list[dev_cpu_id()] = NULL;
	local_irq_restore(f);
	while (list) {
		net_device_t *dev = list;

		list = dev->txq.sched_next;
		dev->txq.sched_next = NULL;
		__atomic_fetch_and(&dev->txq.state, ~TXQ_SCHED,
				   __ATOMIC_ACQ_REL);
		qdisc_run(dev);
	}
}

void net_tx_flush_local(void)
{
	if (irqs_disabled())
		return;
	if (!__atomic_load_n(&tx_list[dev_cpu_id()], __ATOMIC_ACQUIRE))
		return;
	net_tx_action();
}

void dev_core_init_subsys(void)
{
	softirq_register(SOFTIRQ_NET_TX, net_tx_action);
}

// ============================================================================
// Link state
// ============================================================================

static volatile int linkwatch_pending;

static void linkwatch_fire(net_device_t *dev)
{
	__atomic_store_n(&dev->link_events, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&linkwatch_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

void netif_carrier_on(net_device_t *dev)
{
	if (!(__atomic_fetch_or(&dev->flags, IFF_RUNNING, __ATOMIC_ACQ_REL) &
	      IFF_RUNNING)) {
		linkwatch_fire(dev);
		if (!skb_queue_empty(&dev->txq.q))
			netif_schedule(dev);
	}
}

void netif_carrier_off(net_device_t *dev)
{
	if (__atomic_fetch_and(&dev->flags, ~IFF_RUNNING, __ATOMIC_ACQ_REL) &
	    IFF_RUNNING) {
		/* Without a carrier nothing queued will go out: drop it, as
		 * the queue would be dropped on its way to the driver. */
		txq_drop_all(dev);
		linkwatch_fire(dev);
	}
}

int netif_carrier_ok(const net_device_t *dev)
{
	return (__atomic_load_n(&dev->flags, __ATOMIC_ACQUIRE) &
		IFF_RUNNING) != 0;
}

int dev_change_flags(net_device_t *dev, uint32_t flags)
{
	uint32_t old;

	if (!dev || !dev->core_ready)
		return -1;
	if (flags & IFF_UP) {
		old = __atomic_fetch_or(&dev->flags, IFF_UP, __ATOMIC_ACQ_REL);
		if (!(old & IFF_UP)) {
			call_netdevice_notifiers(NETDEV_UP, dev);
			if (!skb_queue_empty(&dev->txq.q))
				netif_schedule(dev);
		}
		return 0;
	}
	old = __atomic_fetch_and(&dev->flags, ~IFF_UP, __ATOMIC_ACQ_REL);
	if (old & IFF_UP) {
		/* Down: nothing more is sent or received; what waits to be
		 * sent is dropped, and the neighbours and routes learned
		 * through the device are forgotten (the notifiers). */
		txq_drop_all(dev);
		call_netdevice_notifiers(NETDEV_DOWN, dev);
	}
	return 0;
}

void linkwatch_run(void)
{
	if (!__atomic_exchange_n(&linkwatch_pending, 0, __ATOMIC_ACQ_REL))
		return;
	for (int i = 0; i < net_device_count(); i++) {
		net_device_t *dev = net_get_device(i);

		if (dev && __atomic_exchange_n(&dev->link_events, 0,
					       __ATOMIC_ACQ_REL))
			call_netdevice_notifiers(NETDEV_CHANGE, dev);
	}
}

// ============================================================================
// Watchdog
// ============================================================================

void dev_watchdog_run(void)
{
	uint64_t now = timer_ticks();

	for (int i = 0; i < net_device_count(); i++) {
		net_device_t *d = net_get_device(i);
		struct netdev_queue *txq;

		if (!d)
			continue;
		/* The driver's link supervision first, as it always ran. */
		if (d->watchdog)
			d->watchdog(d);
		if (!d->core_ready || !netif_carrier_ok(d))
			continue;
		txq = &d->txq;
		if (skb_queue_empty(&txq->q))
			continue;
		if (!(txq_state(d) & TXQ_XOFF) && !txq->busy_since)
			continue;
		if (now - txq->trans_start <
		    net_ms_to_ticks(d->watchdog_timeo_ms))
			continue;
		txq->timeouts++;
		txq->trans_start = now;
		if (d->tx_timeout)
			d->tx_timeout(d);
		netif_schedule(d);
	}
}

// ============================================================================
// Multicast list
// ============================================================================

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < 6; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

void dev_set_rx_mode(net_device_t *dev)
{
	if (dev && dev->set_rx_mode)
		dev->set_rx_mode(dev);
}

int __dev_mc_add(net_device_t *dev, const uint8_t addr[6])
{
	int changed = 0, found = 0;
	uint64_t f;

	if (!dev || !dev->core_ready)
		return -1;
	spin_lock_irqsave(&dev->mc.lock, &f);
	for (uint32_t i = 0; i < dev->mc.count; i++) {
		if (mac_eq(dev->mc.a[i].addr, addr)) {
			dev->mc.a[i].users++;
			found = 1;
			break;
		}
	}
	if (!found) {
		if (dev->mc.count < NETDEV_MC_MAX) {
			struct netdev_hw_addr *a = &dev->mc.a[dev->mc.count++];

			for (int i = 0; i < 6; i++)
				a->addr[i] = addr[i];
			a->users = 1;
		} else {
			dev->mc.overflow++;
		}
		changed = 1;
	}
	spin_unlock_irqrestore(&dev->mc.lock, f);
	return changed;
}

int __dev_mc_del(net_device_t *dev, const uint8_t addr[6])
{
	int changed = 0, found = 0;
	uint64_t f;

	if (!dev || !dev->core_ready)
		return -1;
	spin_lock_irqsave(&dev->mc.lock, &f);
	for (uint32_t i = 0; i < dev->mc.count; i++) {
		if (!mac_eq(dev->mc.a[i].addr, addr))
			continue;
		found = 1;
		if (--dev->mc.a[i].users == 0) {
			dev->mc.a[i] = dev->mc.a[dev->mc.count - 1];
			dev->mc.count--;
			changed = 1;
		}
		break;
	}
	if (!found && dev->mc.overflow) {
		dev->mc.overflow--;
		found = 1;
		changed = !dev->mc.overflow;
	}
	spin_unlock_irqrestore(&dev->mc.lock, f);
	return found ? changed : -1;
}

int dev_mc_add(net_device_t *dev, const uint8_t addr[6])
{
	int rc = __dev_mc_add(dev, addr);

	if (rc > 0)
		dev_set_rx_mode(dev);
	return rc < 0 ? -1 : 0;
}

int dev_mc_del(net_device_t *dev, const uint8_t addr[6])
{
	int rc = __dev_mc_del(dev, addr);

	if (rc > 0)
		dev_set_rx_mode(dev);
	return rc < 0 ? -1 : 0;
}

int dev_mc_snapshot(net_device_t *dev, uint8_t (*out)[6], int max, int *all)
{
	int n = 0;
	uint64_t f;

	spin_lock_irqsave(&dev->mc.lock, &f);
	for (uint32_t i = 0; i < dev->mc.count && n < max; i++, n++)
		for (int j = 0; j < 6; j++)
			out[n][j] = dev->mc.a[i].addr[j];
	if (all)
		*all = dev->mc.overflow != 0 || (int)dev->mc.count > max;
	spin_unlock_irqrestore(&dev->mc.lock, f);
	return n;
}

// ============================================================================
// Statistics
// ============================================================================

void dev_get_stats64(net_device_t *dev, struct net_device_stats64 *s)
{
	mm_memset(s, 0, sizeof(*s));
	s->rx_packets = dev->rx_packets;
	s->tx_packets = dev->tx_packets;
	s->rx_bytes = dev->rx_bytes;
	s->tx_bytes = dev->tx_bytes;
	s->rx_errors = dev->rx_errors;
	s->tx_errors = dev->tx_errors;
	s->rx_dropped = dev->rx_dropped + dev->rx_backlog_drops;
	s->tx_dropped = dev->tx_dropped;
	s->multicast = dev->multicast;
	s->tx_timeouts = dev->txq.timeouts;
	s->tx_requeues = dev->txq.requeues;
	s->rx_backlog_drops = dev->rx_backlog_drops;
	if (dev->get_stats64)
		dev->get_stats64(dev, s);
}
