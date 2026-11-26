// LikeOS AMD PCnet32 NIC Driver
//
// Supports the AMD PCnet-PCI family (PCI 0x1022:0x2000):
//   - Am79C970A   "PCnet-PCI II"
//   - Am79C973    "PCnet-FAST III"
//   - QEMU `-device pcnet`
//
// Programs the chip in 32-bit DWord I/O mode with SWSTYLE=2 ("PCnet-PCI II
// 32-bit"), which works on every member of the family.  The chip has only
// legacy INTx — no MSI/MSI-X.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/pcnet32.h>
#include <kernel/dev/nic/nic_util.h>
#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/skb.h>
#include <kernel/hal/pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/interrupt.h>
#include <kernel/mm/slab.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/ioapic.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// Supported PCI device IDs for the AMD PCnet-PCI family.
// ============================================================================
typedef struct {
	uint16_t vid, did;
	const char *name;
} pcnet_id_t;

static const pcnet_id_t pcnet_pci_ids[] = {
	{ 0x1022, 0x2000, "AMD PCnet-PCI / Am79C97x" }, // QEMU -device pcnet
	{ 0x1022, 0x2001, "AMD PCnet-Home" },
	{ 0x1023, 0x2000, "Trident PCnet (rebrand)" },
	{ 0, 0, NULL },
};

static const pcnet_id_t *pcnet_lookup(uint16_t vid, uint16_t did)
{
	for (const pcnet_id_t *e = pcnet_pci_ids; e->name; e++) {
		if (e->vid == vid && e->did == did)
			return e;
	}
	return NULL;
}

static pcnet_dev_t g_pcnet;
int g_pcnet_initialized = 0;
int g_pcnet_legacy_irq = -1;

// ============================================================================
// I/O helpers
// ============================================================================
static inline void pcnet_w32(pcnet_dev_t *dev, uint16_t off, uint32_t v)
{
	outl(dev->io_base + off, v);
}
static inline uint32_t pcnet_r32(pcnet_dev_t *dev, uint16_t off)
{
	return inl(dev->io_base + off);
}

/* CSR/BCR access (DWord mode): RAP then the data port, as one step under
 * reg_lock -- the interrupt handler, the transmit hook, the poll and the
 * watchdog all use the index register. */
static uint32_t pcnet_read_csr(pcnet_dev_t *dev, uint32_t idx)
{
	uint64_t f;
	uint32_t v;

	spin_lock_irqsave(&dev->reg_lock, &f);
	pcnet_w32(dev, PCNET_RAP, idx);
	v = pcnet_r32(dev, PCNET_RDP);
	spin_unlock_irqrestore(&dev->reg_lock, f);
	return v;
}
static void pcnet_write_csr(pcnet_dev_t *dev, uint32_t idx, uint32_t v)
{
	uint64_t f;

	spin_lock_irqsave(&dev->reg_lock, &f);
	pcnet_w32(dev, PCNET_RAP, idx);
	pcnet_w32(dev, PCNET_RDP, v);
	spin_unlock_irqrestore(&dev->reg_lock, f);
}
static uint32_t pcnet_read_bcr(pcnet_dev_t *dev, uint32_t idx)
{
	uint64_t f;
	uint32_t v;

	spin_lock_irqsave(&dev->reg_lock, &f);
	pcnet_w32(dev, PCNET_RAP, idx);
	v = pcnet_r32(dev, PCNET_BDP);
	spin_unlock_irqrestore(&dev->reg_lock, f);
	return v;
}
static void pcnet_write_bcr(pcnet_dev_t *dev, uint32_t idx, uint32_t v)
{
	uint64_t f;

	spin_lock_irqsave(&dev->reg_lock, &f);
	pcnet_w32(dev, PCNET_RAP, idx);
	pcnet_w32(dev, PCNET_BDP, v);
	spin_unlock_irqrestore(&dev->reg_lock, f);
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void pcnet_delay_us(uint32_t us)
{
	uint32_t t0 = timer_pmtimer_read_raw();
	if (t0 == 0) {
		for (volatile uint32_t i = 0; i < us * 4; i++)
			__asm__ volatile("pause");
		return;
	}
	while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < us)
		__asm__ volatile("pause");
}

// ============================================================================
// Read MAC address from APROM (offset 0..5 in BAR0 I/O space)
//
// In 32-bit DWord I/O mode the chip ignores 8-bit / 16-bit accesses to the
// APROM region and returns 0xFF, so we must read it as two 32-bit DWords
// (offset 0x00 and 0x04) and unpack the bytes ourselves.  Doing it this
// way also works in word/byte mode because the APROM is just a flat 16-byte
// PROM image at the start of BAR0.
// ============================================================================
static void pcnet_read_mac(pcnet_dev_t *dev)
{
	uint32_t lo = inl(dev->io_base + PCNET_APROM + 0);
	uint32_t hi = inl(dev->io_base + PCNET_APROM + 4);
	dev->mac_addr[0] = (uint8_t)(lo);
	dev->mac_addr[1] = (uint8_t)(lo >> 8);
	dev->mac_addr[2] = (uint8_t)(lo >> 16);
	dev->mac_addr[3] = (uint8_t)(lo >> 24);
	dev->mac_addr[4] = (uint8_t)(hi);
	dev->mac_addr[5] = (uint8_t)(hi >> 8);
}

// ============================================================================
// Reset chip and switch into 32-bit DWord I/O mode
// ============================================================================
static int pcnet_reset(pcnet_dev_t *dev)
{
	// Read the 16-bit RESET register (in word mode it sits at offset 0x14;
	// in DWord mode at 0x18 — read both, only the appropriate one will
	// actually trigger the reset depending on the chip's current mode).
	(void)inl(dev->io_base + PCNET_RST); // DWord-mode reset
	(void)inw(dev->io_base + PCNET_RST16); // word-mode reset

	pcnet_delay_us(1000); // 1 ms — chip needs ~1 µs but we're generous

	// Switch to 32-bit DWord mode by performing a 32-bit OUT of zero to
	// RDP (offset 0x10).  After this all CSR/BCR access is via 32-bit
	// DWord ports as defined in pcnet32.h.
	outl(dev->io_base + PCNET_RDP16, 0);

	// Stop the chip (clears CSR0)
	pcnet_write_csr(dev, CSR0, CSR0_STOP);
	pcnet_delay_us(100);

	return 0;
}

// ============================================================================
// Descriptor rings
// ============================================================================

/* Every RX descriptor the chip's, every TX descriptor software's, both
 * positions at the start -- what a (re)initialisation expects. */
static void pcnet_rings_reset(pcnet_dev_t *dev)
{
	for (int i = 0; i < PCNET_NUM_RX_DESC; i++) {
		dev->rx_descs[i].rbadr = (uint32_t)dev->rx_bufs_phys[i];
		dev->rx_descs[i].bcnt = (int16_t)(-PCNET_RX_BUF_SIZE);
		dev->rx_descs[i].mcnt = 0;
		dev->rx_descs[i].reserved = 0;
		__asm__ volatile("" ::: "memory");
		*(volatile uint16_t *)&dev->rx_descs[i].status = RXD_OWN;
	}
	for (int i = 0; i < PCNET_NUM_TX_DESC; i++) {
		*(volatile uint16_t *)&dev->tx_descs[i].status = 0;
		dev->tx_descs[i].tbadr = (uint32_t)dev->tx_bufs_phys[i];
		dev->tx_descs[i].bcnt = 0;
		dev->tx_descs[i].misc = 0;
		dev->tx_descs[i].reserved = 0;
		dev->tx_stamp[i] = 0;
	}
	__asm__ volatile("mfence" ::: "memory");
	dev->rx_cur = 0;
	dev->tx_cur = 0;
	dev->tx_clean = 0;
}

static int pcnet_init_rx(pcnet_dev_t *dev)
{
	uint32_t bytes = sizeof(pcnet_rx_desc_t) * PCNET_NUM_RX_DESC;
	uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys) {
		kprintf("PCnet32: failed to allocate RX ring\n");
		return -1;
	}
	if (phys >> 32) {
		kprintf("PCnet32: ERROR: RX ring phys 0x%lx is above 4GB\n",
			phys);
		return -1;
	}

	dev->rx_descs = (pcnet_rx_desc_t *)phys_to_virt(phys);
	dev->rx_descs_phys = phys;

	for (int i = 0; i < PCNET_NUM_RX_DESC; i++) {
		uint64_t bphys = mm_allocate_physical_page();
		if (!bphys || (bphys >> 32)) {
			kprintf("PCnet32: bad RX buffer alloc %d\n", i);
			return -1;
		}
		dev->rx_bufs[i] = (uint8_t *)phys_to_virt(bphys);
		dev->rx_bufs_phys[i] = bphys;
	}
	return 0;
}

static int pcnet_init_tx(pcnet_dev_t *dev)
{
	uint32_t bytes = sizeof(pcnet_tx_desc_t) * PCNET_NUM_TX_DESC;
	uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys) {
		kprintf("PCnet32: failed to allocate TX ring\n");
		return -1;
	}
	if (phys >> 32) {
		kprintf("PCnet32: ERROR: TX ring phys 0x%lx is above 4GB\n",
			phys);
		return -1;
	}

	dev->tx_descs = (pcnet_tx_desc_t *)phys_to_virt(phys);
	dev->tx_descs_phys = phys;

	for (int i = 0; i < PCNET_NUM_TX_DESC; i++) {
		uint64_t bphys = mm_allocate_physical_page();
		if (!bphys || (bphys >> 32)) {
			kprintf("PCnet32: bad TX buffer alloc %d\n", i);
			return -1;
		}
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(bphys);
		dev->tx_bufs_phys[i] = bphys;
	}
	pcnet_rings_reset(dev);
	return 0;
}

// ============================================================================
// Init block, logical address filter, (re)initialisation
// ============================================================================

/* The receive filter into the init block: our address, PROM for
 * promiscuous mode, the logical address filter (a 64-bit hash of the
 * multicast addresses; all ones receives every multicast frame). */
static void pcnet_fill_filter(pcnet_dev_t *dev)
{
	pcnet_init_block_t *ib = dev->init_block;
	uint8_t mc[NETDEV_MC_MAX][6];
	uint16_t lf[4] = { 0, 0, 0, 0 };
	int all = 0, n = 0;
	uint32_t flags = 0;

	if (dev->net_dev.core_ready) {
		n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
		flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);
	}
	if (all || (flags & (IFF_ALLMULTI | IFF_PROMISC))) {
		lf[0] = lf[1] = lf[2] = lf[3] = 0xFFFF;
	} else {
		for (int i = 0; i < n; i++) {
			uint32_t bit = nic_mc_hash64_le(mc[i]);

			lf[bit >> 4] |= (uint16_t)(1u << (bit & 15));
		}
	}
	ib->mode = (flags & IFF_PROMISC) ? PCNET_INIT_MODE_PROM : 0;
	for (int i = 0; i < 6; i++)
		ib->padr[i] = dev->mac_addr[i];
	for (int i = 0; i < 4; i++) {
		ib->laddrf[2 * i] = (uint8_t)lf[i];
		ib->laddrf[2 * i + 1] = (uint8_t)(lf[i] >> 8);
	}
}

/* Hand the init block to the chip (CSR0.INIT) and wait for IDON. */
static int pcnet_load_init_block(pcnet_dev_t *dev)
{
	uint64_t phys = dev->init_block_phys;

	__asm__ volatile("mfence" ::: "memory");
	// Tell the chip where the init block lives (CSR1 = lo, CSR2 = hi 16 bits)
	pcnet_write_csr(dev, CSR1, (uint32_t)(phys & 0xFFFF));
	pcnet_write_csr(dev, CSR2, (uint32_t)((phys >> 16) & 0xFFFF));

	// Enable AUTOPAD and auto-strip RX FCS
	pcnet_write_csr(dev, CSR4,
			pcnet_read_csr(dev, CSR4) | CSR4_APAD_XMT |
				CSR4_ASTRP_RCV);

	// Kick off init.  Hardware DMAs the init block, populates internal
	// state and then sets CSR0.IDON.
	pcnet_write_csr(dev, CSR0, CSR0_INIT);

	// Wait for IDON, max ~100 ms
	for (int i = 0; i < 10000; i++) {
		uint32_t csr0 = pcnet_read_csr(dev, CSR0);
		if (csr0 & CSR0_IDON) {
			// Acknowledge IDON by writing 1 to it
			pcnet_write_csr(dev, CSR0, CSR0_IDON);
			return 0;
		}
		pcnet_delay_us(10);
	}
	kprintf("PCnet32: init block timeout (CSR0=0x%x)\n",
		pcnet_read_csr(dev, CSR0));
	return -1;
}

static int pcnet_init_block(pcnet_dev_t *dev)
{
	uint64_t phys = mm_allocate_contiguous_pages(1);
	if (!phys || (phys >> 32)) {
		kprintf("PCnet32: failed to allocate init block\n");
		return -1;
	}

	dev->init_block = (pcnet_init_block_t *)phys_to_virt(phys);
	dev->init_block_phys = phys;

	pcnet_init_block_t *ib = dev->init_block;
	for (uint32_t i = 0; i < sizeof(*ib) / 4; i++)
		((uint32_t *)ib)[i] = 0;

	ib->rlen = (uint8_t)(PCNET_RX_LEN_LOG2 << 4);
	ib->tlen = (uint8_t)(PCNET_TX_LEN_LOG2 << 4);
	ib->rdra = (uint32_t)dev->rx_descs_phys;
	ib->tdra = (uint32_t)dev->tx_descs_phys;
	pcnet_fill_filter(dev);
	return pcnet_load_init_block(dev);
}

/* Stop the chip, put both rings back to their start and initialise it
 * again with the current receive filter: after a transmit FIFO underflow
 * (which turns the transmitter off), a transmit hang, or a filter change
 * (the filter is only read at initialisation).  Frames in the rings are
 * dropped.  Watchdog context (CPU 0): the poll is not running; tx_lock
 * keeps the transmit hook out. */
static void pcnet_restart(pcnet_dev_t *dev)
{
	uint64_t f;
	uint32_t dropped;

	dev->st_restarts++;
	spin_lock_irqsave(&dev->tx_lock, &f);
	pcnet_write_csr(dev, CSR0, CSR0_STOP);
	pcnet_delay_us(10);
	dropped = (uint32_t)((dev->tx_cur + PCNET_NUM_TX_DESC - dev->tx_clean) %
			     PCNET_NUM_TX_DESC);
	pcnet_rings_reset(dev);
	pcnet_fill_filter(dev);
	(void)pcnet_load_init_block(dev);
	pcnet_write_csr(dev, CSR3, 0);
	pcnet_write_csr(dev, CSR0, CSR0_STRT | CSR0_INEA);
	dev->net_dev.tx_errors += dropped;
	if (netif_queue_stopped(&dev->net_dev))
		netif_wake_queue(&dev->net_dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	napi_schedule(&dev->napi);
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================
#define PCNET_TX_WAKE_THRESH 8

static uint32_t pcnet_tx_unused(const pcnet_dev_t *dev)
{
	return nic_ring_unused(dev->tx_cur, dev->tx_clean, PCNET_NUM_TX_DESC);
}

/* Reclaim descriptors the chip handed back (OWN clear), counting the
 * errors it reported; an underflow (the transmitter turned itself off)
 * asks the watchdog for a restart.  Caller holds tx_lock. */
static void pcnet_tx_clean_locked(pcnet_dev_t *dev)
{
	while (dev->tx_clean != dev->tx_cur) {
		volatile pcnet_tx_desc_t *d = &dev->tx_descs[dev->tx_clean];
		uint16_t st = d->status;

		if (st & TXD_OWN)
			break;
		if (st & TXD_ERR) {
			uint32_t m = d->misc;

			dev->net_dev.tx_errors++;
			if (m & TXM_RTRY)
				dev->st_tx_aborted++;
			if (m & TXM_LCAR)
				dev->st_tx_carrier++;
			if (m & TXM_LCOL)
				dev->st_tx_window++;
			if (m & TXM_UFLO) {
				dev->st_tx_fifo++;
				__atomic_store_n(&dev->restart_pending, 1,
						 __ATOMIC_RELEASE);
				net_housekeeping_kick();
			}
		}
		d->status = 0;
		d->misc = 0;
		dev->tx_stamp[dev->tx_clean] = 0;
		dev->tx_clean =
			(uint16_t)((dev->tx_clean + 1) % PCNET_NUM_TX_DESC);
	}
	if (netif_queue_stopped(&dev->net_dev) &&
	    pcnet_tx_unused(dev) >= PCNET_TX_WAKE_THRESH)
		netif_wake_queue(&dev->net_dev);
}

static int pcnet_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;

	BUG_ON(dev == NULL);
	if (len == 0 || len > PCNET_TX_BUF_SIZE) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&dev->tx_lock, &f);
	if (pcnet_tx_unused(dev) == 0) {
		pcnet_tx_clean_locked(dev);
		if (pcnet_tx_unused(dev) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}

	uint16_t slot = dev->tx_cur;
	volatile pcnet_tx_desc_t *desc = &dev->tx_descs[slot];

	// Pad short frames to 60 bytes (chip's auto-pad covers this too, but
	// doing it in software is cheap and works regardless of CSR4 settings).
	uint32_t tx_len = (len < 60) ? 60 : len;
	mm_memcpy(dev->tx_bufs[slot], skb->data, len);
	if (tx_len > len)
		mm_memset(dev->tx_bufs[slot] + len, 0, tx_len - len);

	desc->tbadr = (uint32_t)dev->tx_bufs_phys[slot];
	desc->bcnt = (int16_t)(-(int32_t)tx_len); // two's complement length
	desc->misc = 0;
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp[slot] = now ? now : 1;
	}

	__asm__ volatile("mfence" ::: "memory");

	// Hand to hardware: STP+ENP (single-buffer packet) and OWN
	desc->status = TXD_OWN | TXD_STP | TXD_ENP;

	__asm__ volatile("mfence" ::: "memory");

	/* Demand transmission (CSR0.TDMD) with INEA kept.  CSR0's status
	 * bits are write-one-to-clear: writing only TDMD|INEA leaves them
	 * for the interrupt handler. */
	pcnet_write_csr(dev, CSR0, CSR0_INEA | CSR0_TDMD);

	dev->tx_cur = (uint16_t)((slot + 1) % PCNET_NUM_TX_DESC);
	ndev->tx_packets++;
	ndev->tx_bytes += tx_len;
	if (pcnet_tx_unused(dev) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

// ============================================================================
// Link status
// ============================================================================
/* Link state from BCR4, the LED0 control/status register.  Bit 6 is LNKSTE
 * -- the ENABLE that makes LED0 show link status -- not the status: it is
 * set in the register's default value 0x00C0, so testing it reported "up"
 * whatever the wire said.  The status is LEDOUT (bit 15), which the chip
 * sets while the selected condition (link) holds, so the register reads
 * back exactly 0x00C0 when the link is down and something else when it is
 * up -- the test every driver for this family uses. */
static int pcnet_link_up(pcnet_dev_t *dev)
{
	// Registers are 16 bits; the upper half of a DWord-mode read is
	// undefined.
	return (pcnet_read_bcr(dev, 4) & 0xFFFF) != 0xC0;
}

/* Link supervision (watchdog).  The carrier follows the indicator only
 * once it has shown "up": an emulation whose LED register never reports
 * a link keeps transmitting as it always did. */
static void pcnet_link_check(pcnet_dev_t *dev)
{
	int now_up = pcnet_link_up(dev);

	if (now_up)
		dev->link_seen = 1;
	if (now_up == dev->link_up)
		return;
	dev->link_up = now_up;
	kprintf("PCnet32: Link %s\n", now_up ? "UP" : "DOWN");
	if (now_up) {
		netif_carrier_on(&dev->net_dev);
	} else {
		dhcp_invalidate(&dev->net_dev);
		if (dev->link_seen)
			netif_carrier_off(&dev->net_dev);
	}
}

static int pcnet_link_status(net_device_t *ndev)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;

	if (pcnet_link_up(dev) != dev->link_up)
		net_housekeeping_kick();
	return pcnet_link_up(dev);
}

static void pcnet_set_rx_mode(net_device_t *ndev)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

static void pcnet_get_stats64(net_device_t *ndev, struct net_device_stats64 *s)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;

	s->rx_missed_errors = dev->st_missed;
	s->rx_crc_errors = dev->st_rx_crc;
	s->rx_frame_errors = dev->st_rx_frame;
	s->rx_fifo_errors = dev->st_rx_fifo;
	s->tx_aborted_errors = dev->st_tx_aborted;
	s->tx_carrier_errors = dev->st_tx_carrier;
	s->tx_fifo_errors = dev->st_tx_fifo;
	s->rx_errors += dev->st_missed;
}

static void pcnet_tx_timeout(net_device_t *ndev)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->restart_pending, 1, __ATOMIC_RELEASE);
}

// Network housekeeping softirq, CPU 0, ~10 Hz and when kicked.
static void pcnet_watchdog(net_device_t *ndev)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;
	uint64_t f, now = timer_ticks();
	int hang = 0;

	if (!dev || !g_pcnet_initialized)
		return;
	pcnet_link_check(dev);
	spin_lock_irqsave(&dev->tx_lock, &f);
	pcnet_tx_clean_locked(dev);
	if (dev->tx_clean != dev->tx_cur && dev->tx_stamp[dev->tx_clean] &&
	    now - dev->tx_stamp[dev->tx_clean] > net_ms_to_ticks(5000))
		hang = 1;
	spin_unlock_irqrestore(&dev->tx_lock, f);
	if (hang) {
		kprintf("PCnet32: transmit timed out, restarting the chip\n");
		__atomic_store_n(&dev->restart_pending, 1, __ATOMIC_RELEASE);
	}
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL))
		__atomic_store_n(&dev->restart_pending, 1, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&dev->restart_pending, 0, __ATOMIC_ACQ_REL))
		pcnet_restart(dev);
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the Am79C97x
// datasheet, asserting CSR0.STOP halts both the RX and TX engines, masks
// all interrupt sources, and aborts any pending bus-master DMA at the
// next quiescent point.  Then drop the PCI Command.BME bit so any TLP
// that escaped the in-controller stop is dropped at the root complex.
static void pcnet_shutdown(net_device_t *ndev)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)ndev->driver_data;
	if (!dev || !dev->io_base)
		return;

	pcnet_write_csr(dev, CSR3, 0xFFFF);
	pcnet_write_csr(dev, CSR0, CSR0_STOP);

	if (dev->pci_dev) {
		const pci_device_t *pdev = dev->pci_dev;
		uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device,
					      pdev->function, 0x04);
		cmd &= ~0x0004u; // clear Bus Master Enable
		pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04,
				cmd);
	}
}

// ============================================================================
// Receive (NAPI poll)
// ============================================================================
static int pcnet_rx_clean(pcnet_dev_t *dev, int budget)
{
	net_device_t *nd = &dev->net_dev;
	int work = 0;

	while (work < budget) {
		volatile pcnet_rx_desc_t *desc = &dev->rx_descs[dev->rx_cur];
		uint16_t st = desc->status;

		if (st & RXD_OWN)
			break; // Hardware still owns this descriptor

		if (st & RXD_ERR) {
			nd->rx_errors++;
			if (st & RXD_CRC)
				dev->st_rx_crc++;
			if (st & RXD_FRAM)
				dev->st_rx_frame++;
			if (st & (RXD_OFLO | RXD_BUFF))
				dev->st_rx_fifo++;
		} else if ((st & (RXD_STP | RXD_ENP)) ==
			   (RXD_STP | RXD_ENP)) {
			// mcnt excludes the FCS (ASTRP_RCV is set)
			uint32_t len = desc->mcnt & 0xFFF;

			if (len > 0 && len <= PCNET_RX_BUF_SIZE) {
				sk_buff_t *skb = skb_alloc(len);

				if (skb) {
					const uint8_t *b =
						dev->rx_bufs[dev->rx_cur];

					skb->dev = nd;
					mm_memcpy(skb_append(skb, len), b,
						  len);
					nd->rx_packets++;
					nd->rx_bytes += len;
					if (nic_is_multicast(b))
						nd->multicast++;
					napi_gro_receive(&dev->napi, skb);
				} else {
					nd->rx_dropped++;
				}
			} else {
				nd->rx_errors++;
			}
		} else {
			// a frame spread over several buffers: never this big
			nd->rx_errors++;
		}

		// Hand the descriptor back to the chip
		desc->bcnt = (int16_t)(-PCNET_RX_BUF_SIZE);
		desc->mcnt = 0;
		__asm__ volatile("mfence" ::: "memory");
		desc->status = RXD_OWN;

		dev->rx_cur = (uint16_t)((dev->rx_cur + 1) % PCNET_NUM_RX_DESC);
		work++;
	}
	return work;
}

static int pcnet_poll(struct napi_struct *n, int budget)
{
	pcnet_dev_t *dev = (pcnet_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	spin_lock_irqsave(&dev->tx_lock, &f);
	pcnet_tx_clean_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	work = pcnet_rx_clean(dev, budget);
	if (work < budget && napi_complete_done(n, work)) {
		// unmask; status latched meanwhile interrupts at once
		pcnet_write_csr(dev, CSR3, 0);
		pcnet_write_csr(dev, CSR0, CSR0_INEA);
	}
	return work;
}

// ============================================================================
// IRQ top half (called from kernel/ke/interrupt.c dispatcher)
// ============================================================================
/* Returns 1 when the interrupt was this chip's (CSR0.INTR), 0 otherwise
 * (shared line); the dispatcher acknowledges the local APIC.  Receive and
 * transmit work goes to the NAPI poll with the sources masked in CSR3
 * until it is done. */
int pcnet32_irq_handler(void)
{
	if (!g_pcnet_initialized)
		return 0;

	// Entropy
	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	pcnet_dev_t *dev = &g_pcnet;
	BUG_ON(dev == NULL);
	uint32_t csr0 = pcnet_read_csr(dev, CSR0) & 0xFFFF;

	if (!(csr0 & CSR0_INTR) || csr0 == 0xFFFF)
		return 0;

	// Acknowledge: write back the W1C bits we observed (IDON, RINT, TINT,
	// BABL, CERR, MISS, MERR) AND keep INEA asserted.  Do NOT set INIT,
	// STRT, STOP, TDMD here.
	uint32_t ack = csr0 & (CSR0_RINT | CSR0_TINT | CSR0_IDON | CSR0_BABL |
			       CSR0_CERR | CSR0_MISS | CSR0_MERR);
	pcnet_write_csr(dev, CSR0, CSR0_INEA | ack);

	if (csr0 & CSR0_MISS)
		dev->st_missed++; // ring full: the poll empties it
	if (csr0 & CSR0_MERR)
		dev->st_merr++; // bus master timeout; the chip goes on
	if (csr0 & CSR0_BABL) {
		dev->st_babl++;
		dev->net_dev.tx_errors++;
	}
	/* The poll takes everything else, with the sources masked until it
	 * completes (CSR3). */
	pcnet_write_csr(dev, CSR3, CSR3_NAPI_MASK);
	napi_schedule(&dev->napi);
	return 1;
}

// ============================================================================
// Resolve I/O BAR (BAR0)
// ============================================================================
static int pcnet_resolve_iobase(pcnet_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];
	if (!(bar0 & 1)) {
		kprintf("PCnet32: BAR0 is not I/O space (bar0=0x%x)\n", bar0);
		return -1;
	}
	dev->io_base = (uint16_t)(bar0 & 0xFFFC);
	return 0;
}

// ============================================================================
// Driver entry point
// ============================================================================
void pcnet32_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		const pcnet_id_t *match =
			pcnet_lookup(devs[i].vendor_id, devs[i].device_id);
		if (!match)
			continue;

		kprintf("PCnet32: Found %s (PCI %02x:%02x.%x)\n", match->name,
			devs[i].bus, devs[i].device, devs[i].function);

		pcnet_dev_t *dev = &g_pcnet;
		dev->pci_dev = &devs[i];
		dev->reg_lock = (spinlock_t)SPINLOCK_INIT("pcnet32_reg");
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("pcnet32_tx");

		pci_enable_busmaster_mem(dev->pci_dev);

		// pci_enable_busmaster_mem() only sets MEM (bit 1) + Bus Master (bit 2).
		// PCnet's BAR0 is I/O space, so we must also set I/O Space Enable
		// (bit 0) — without it, inb/inw/inl from io_base return 0xFF and
		// the MAC reads as ff:ff:ff:ff:ff:ff.
		{
			uint32_t cmd = pci_cfg_read32(
				dev->pci_dev->bus, dev->pci_dev->device,
				dev->pci_dev->function, 0x04);
			if (!(cmd & 0x1)) {
				pci_cfg_write32(dev->pci_dev->bus,
						dev->pci_dev->device,
						dev->pci_dev->function, 0x04,
						cmd | 0x1);
			}
		}

		if (pcnet_resolve_iobase(dev) < 0)
			return;
		kprintf("PCnet32: I/O base 0x%x\n", dev->io_base);

		if (pcnet_reset(dev) < 0)
			return;

		// Identify chip via CSR88 (CHIPID lower 16 bits) — informational only
		pcnet_write_csr(dev, 88 - 0, 0); // ensure RAP wraps
		uint32_t chipid_lo = pcnet_read_csr(dev, 88);
		uint32_t chipid_hi = pcnet_read_csr(dev, 89);
		uint32_t part_id =
			((chipid_hi & 0xFFFF) << 16) | (chipid_lo & 0xFFFF);
		const char *chip = "Unknown PCnet";
		switch ((part_id >> 12) & 0xFFFF) {
		case 0x2420:
			chip = "Am79C970A (PCnet-PCI II)";
			break;
		case 0x2621:
			chip = "Am79C971 (PCnet-FAST)";
			break;
		case 0x2625:
			chip = "Am79C973 (PCnet-FAST III)";
			break;
		case 0x2627:
			chip = "Am79C975";
			break;
		default:
			chip = "PCnet (generic)";
			break;
		}
		kprintf("PCnet32: chip = %s (part_id=0x%x)\n", chip, part_id);

		// Select software style 2 (PCnet-PCI II 32-bit) via BCR20
		pcnet_write_bcr(dev, BCR20, BCR20_SWSTYLE_2 | BCR20_SSIZE32);

		// Read MAC from APROM
		pcnet_read_mac(dev);
		kprintf("PCnet32: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		// Allocate descriptor rings + buffers (RX descriptors start hw-owned)
		if (pcnet_init_rx(dev) < 0)
			return;
		if (pcnet_init_tx(dev) < 0)
			return;

		// Build init block & program it into the chip
		if (pcnet_init_block(dev) < 0)
			return;

		// Resolve IRQ via ACPI _PRT first, then fall back to PCI line.
		// (See e1000.c for the rationale — `interrupt_line` is unreliable
		// in APIC mode.)
		uint8_t irq = 0xFF;
		uint32_t gsi = 0;
		uint8_t pin = dev->pci_dev->interrupt_pin;
		if (pin >= 1 && pin <= 4) {
			uint8_t acpi_pin = pin - 1;
			uint8_t lookup_dev = dev->pci_dev->device;
			uint8_t lookup_pin = acpi_pin;
			if (dev->pci_dev->bus != 0) {
				const pci_device_t *bridge =
					pci_find_bridge_for_bus(
						dev->pci_dev->bus);
				if (bridge) {
					lookup_pin = (acpi_pin +
						      dev->pci_dev->device) %
						     4;
					lookup_dev = bridge->device;
				}
			}
			if (acpi_pci_lookup_irq("\\\\_SB_.PCI0", lookup_dev,
						lookup_pin, &gsi) == 0 &&
			    gsi <= 23) {
				irq = (uint8_t)gsi;
				kprintf("PCnet32: ACPI _PRT resolved INT%c -> GSI %u\n",
					'A' + acpi_pin, gsi);
			}
		}
		if (irq == 0xFF) {
			irq = dev->pci_dev->interrupt_line;
			if (irq != 0xFF && irq <= 23) {
				kprintf("PCnet32: ACPI _PRT lookup failed, falling back to "
					"PCI interrupt_line = %d\n",
					irq);
			} else {
				kprintf("PCnet32: WARNING: no valid IRQ\n");
			}
		}
		kprintf("PCnet32: Using legacy IRQ %d\n", irq);
		g_pcnet_legacy_irq = irq;

		// Clear PCI Command INTx Disable if set
		{
			uint32_t cmd = pci_cfg_read32(
				dev->pci_dev->bus, dev->pci_dev->device,
				dev->pci_dev->function, 0x04);
			if (cmd & PCI_CMD_INTX_DISABLE) {
				cmd &= ~PCI_CMD_INTX_DISABLE;
				pci_cfg_write32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04, cmd);
				kprintf("PCnet32: cleared PCI Command INTx Disable bit\n");
			}
		}

		if (irq <= 23) {
			uint8_t vector = 32 + irq;
			ioapic_configure_legacy_irq(irq, vector,
						    IOAPIC_POLARITY_LOW,
						    IOAPIC_TRIGGER_LEVEL);
		}

		// Init net_device fields BEFORE enabling chip-side interrupts so
		// the dispatcher always sees a valid net_dev (same rationale as
		// the e1000 driver).
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("pcnet32");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, pcnet_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_pcnet_initialized = 1;

		// Initial link check (BCR4 LEDOUT, see pcnet_link_up)
		dev->link_up = pcnet_link_up(dev);
		dev->link_seen = dev->link_up;
		kprintf("PCnet32: Link %s\n", dev->link_up ? "UP" : "DOWN");

		// Wire up net_device_t and register
		dev->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			dev->net_dev.mac_addr[m] = dev->mac_addr[m];
		dev->net_dev.mtu = NET_MTU_DEFAULT;
		dev->net_dev.ip_addr = 0;
		dev->net_dev.netmask = 0;
		dev->net_dev.gateway = 0;
		dev->net_dev.dns_server = 0;
		dev->net_dev.send = NULL;
		dev->net_dev.xmit = pcnet_xmit;
		dev->net_dev.link_status = pcnet_link_status;
		dev->net_dev.shutdown = pcnet_shutdown;
		dev->net_dev.watchdog = pcnet_watchdog;
		dev->net_dev.tx_timeout = pcnet_tx_timeout;
		dev->net_dev.set_rx_mode = pcnet_set_rx_mode;
		dev->net_dev.get_stats64 = pcnet_get_stats64;

		net_register(&dev->net_dev);

		// Unmask all interrupts (clear CSR3 mask bits), start the
		// chip and enable interrupts
		pcnet_write_csr(dev, CSR3, 0);
		pcnet_write_csr(dev, CSR0, CSR0_STRT | CSR0_INEA);
		kprintf("PCnet32: Driver initialized successfully\n");
		return;
	}
}
