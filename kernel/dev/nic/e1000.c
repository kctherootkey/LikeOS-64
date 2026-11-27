// LikeOS Intel E1000 NIC Driver
// Supports the e1000-class parts:
//   - 82540EM  (QEMU `-device e1000`, VirtualBox "Intel PRO/1000 MT Desktop")
//   - 82545EM  (VMware default, VirtualBox "Intel PRO/1000 MT Server" and
//               "Intel PRO/1000 T Server")
// Note: The 82574L (PCI 0x10D3) is an e1000e-class part and is handled
// by the separate e1000e driver in this directory, not here.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/e1000.h>
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
// Supported PCI device IDs for the entire 8254x PCI/PCI-X family
// (82540, 82541, 82543, 82544, 82545, 82546, 82547).  8257x PCIe parts
// are handled by the separate e1000e driver.
// ============================================================================
typedef struct {
	uint16_t did;
	const char *name;
} e1000_id_t;

static const e1000_id_t e1000_pci_ids[] = {
	// 82540
	{ 0x100E, "82540EM" }, // QEMU -device e1000, VBox MT Desktop
	{ 0x1015, "82540EM LOM" },
	{ 0x1016, "82540EP LOM" },
	{ 0x1017, "82540EP" },
	{ 0x101E, "82540EP LP" },
	// 82541
	{ 0x1013, "82541EI" },
	{ 0x1018, "82541EI Mobile" },
	{ 0x1076, "82541GI" },
	{ 0x1077, "82541GI Mobile" },
	{ 0x1078, "82541ER" },
	{ 0x1079, "82546GB Quad Copper" },
	{ 0x107C, "82541PI" },
	// 82543
	{ 0x1004, "82543GC Copper" }, // QEMU -device e1000-82543gc
	{ 0x1003, "82543GC Fiber" },
	// 82544
	{ 0x1008, "82544EI Copper" },
	{ 0x1009, "82544EI Fiber" },
	{ 0x100C, "82544GC Copper" }, // QEMU -device e1000-82544gc
	{ 0x100D, "82544GC LOM" },
	// 82545
	{ 0x100F, "82545EM Copper" }, // VMware, VBox MT Server
	{ 0x1011, "82545EM Fiber" },
	{ 0x1026, "82545GM Copper" },
	{ 0x1027, "82545GM Fiber" },
	{ 0x1028, "82545GM SerDes" },
	// 82546
	{ 0x1010, "82546EB Copper" },
	{ 0x1012, "82546EB Fiber" },
	{ 0x101D, "82546EB Quad Copper" },
	{ 0x1079, "82546GB Quad Copper (2)" },
	{ 0x107A, "82546GB Fiber" },
	{ 0x107B, "82546GB SerDes" },
	{ 0x108A, "82546GB Copper" },
	{ 0x1099, "82546GB Quad Copper KSP3" },
	{ 0x10B5, "82546GB Quad Copper" },
	// 82547
	{ 0x1019, "82547EI" },
	{ 0x101A, "82547EI Mobile" },
	{ 0x1075, "82547GI" },
	{ 0, NULL },
};

static const e1000_id_t *e1000_lookup(uint16_t did)
{
	for (const e1000_id_t *e = e1000_pci_ids; e->name; e++) {
		if (e->did == did)
			return e;
	}
	return NULL;
}

// Global e1000 device (single NIC support)
static e1000_dev_t g_e1000;
int g_e1000_initialized = 0;
int g_e1000_legacy_irq = -1; // Legacy IRQ number (-1 = not using legacy)

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t e1000_read(e1000_dev_t *dev, uint32_t reg)
{
	return *(volatile uint32_t *)(dev->mmio_base + reg);
}

static inline void e1000_write(e1000_dev_t *dev, uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(dev->mmio_base + reg) = val;
	__asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// EEPROM Read
// ============================================================================
/* The EERD layout of the 8254x: the word address in bits 15:8 and DONE in
 * bit 4 -- except on the 82541 and 82547, whose EERD has the address in
 * bits 15:2 and DONE in bit 1.  Returns 0 and the word, -1 when the read
 * never completed (the caller then takes the address the hardware loaded
 * into RAL0/RAH0). */
static int e1000_is_82541_82547(uint16_t did)
{
	switch (did) {
	case 0x1013: // 82541EI
	case 0x1018: // 82541EI Mobile
	case 0x1076: // 82541GI
	case 0x1077: // 82541GI Mobile
	case 0x1078: // 82541ER
	case 0x107C: // 82541PI
	case 0x1019: // 82547EI
	case 0x101A: // 82547EI Mobile
	case 0x1075: // 82547GI
		return 1;
	default:
		return 0;
	}
}

static void e1000_delay_us(uint32_t us);

static int e1000_read_eeprom(e1000_dev_t *dev, uint8_t addr, uint16_t *out)
{
	uint32_t shift = E1000_EERD_ADDR_SHIFT, done = E1000_EERD_DONE;

	if (e1000_is_82541_82547(dev->device_id)) {
		shift = 2;
		done = 1u << 1;
	}
	e1000_write(dev, E1000_EERD, ((uint32_t)addr << shift) | E1000_EERD_START);
	for (int i = 0; i < 2000; i++) {
		uint32_t val = e1000_read(dev, E1000_EERD);

		if (val & done) {
			*out = (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
			return 0;
		}
		e1000_delay_us(5);
	}
	return -1;
}

// ============================================================================
// Read MAC Address
// ============================================================================
static void e1000_read_mac(e1000_dev_t *dev)
{
	uint16_t w0 = 0, w1 = 0, w2 = 0;
	int nvm_ok = e1000_read_eeprom(dev, 0, &w0) == 0 &&
		     e1000_read_eeprom(dev, 1, &w1) == 0 &&
		     e1000_read_eeprom(dev, 2, &w2) == 0;

	if (nvm_ok && (w0 | w1 | w2) != 0 &&
	    !(w0 == 0xFFFF && w1 == 0xFFFF && w2 == 0xFFFF) &&
	    !(w0 & 1)) { // a station address is never a group address
		dev->mac_addr[0] = w0 & 0xFF;
		dev->mac_addr[1] = (w0 >> 8) & 0xFF;
		dev->mac_addr[2] = w1 & 0xFF;
		dev->mac_addr[3] = (w1 >> 8) & 0xFF;
		dev->mac_addr[4] = w2 & 0xFF;
		dev->mac_addr[5] = (w2 >> 8) & 0xFF;
	} else {
		// RAL/RAH, loaded from the EEPROM by the hardware at reset
		uint32_t ral = e1000_read(dev, E1000_RAL);
		uint32_t rah = e1000_read(dev, E1000_RAH);
		dev->mac_addr[0] = ral & 0xFF;
		dev->mac_addr[1] = (ral >> 8) & 0xFF;
		dev->mac_addr[2] = (ral >> 16) & 0xFF;
		dev->mac_addr[3] = (ral >> 24) & 0xFF;
		dev->mac_addr[4] = rah & 0xFF;
		dev->mac_addr[5] = (rah >> 8) & 0xFF;
	}
}

// ============================================================================
// PM Timer-based microsecond delay (available early — timer_init_pmtimer()
// runs before net_init()).  Falls back to a pause loop if PM Timer is absent.
// ============================================================================
static void e1000_delay_us(uint32_t us)
{
	uint32_t t0 = timer_pmtimer_read_raw();
	if (t0 == 0) {
		// PM Timer unavailable — rough fallback
		for (volatile uint32_t i = 0; i < us * 4; i++)
			__asm__ volatile("pause");
		return;
	}
	while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < us)
		__asm__ volatile("pause");
}

// ============================================================================
// Device Reset
// ============================================================================
static void e1000_reset(e1000_dev_t *dev)
{
	// Disable interrupts
	e1000_write(dev, E1000_IMC, 0xFFFFFFFF);

	// Read and clear pending interrupts
	(void)e1000_read(dev, E1000_ICR);

	// Soft reset: disable RX and TX without a full CTRL.RST.
	// CTRL.RST causes VirtualBox's E1000 emulation to break the RX
	// path — TX works but no packets are ever received.  The UEFI
	// firmware already performed the hardware reset and PHY
	// negotiation, so a soft init is sufficient.
	e1000_write(dev, E1000_RCTL, 0); // Disable receiver
	e1000_write(dev, E1000_TCTL, 0); // Disable transmitter

	// Wait for in-flight DMA to drain (10 ms via PM Timer)
	e1000_delay_us(10000);

	// Disable interrupts again (in case RX/TX raised something)
	e1000_write(dev, E1000_IMC, 0xFFFFFFFF);
	(void)e1000_read(dev, E1000_ICR);
}

// ============================================================================
// Initialize RX Ring
// ============================================================================
static int e1000_init_rx(e1000_dev_t *dev)
{
	// Allocate descriptor ring (legacy RX descriptors)
	uint32_t rx_ring_size =
		sizeof(e1000_rx_desc_legacy_t) * E1000_NUM_RX_DESC;
	uint32_t rx_ring_pages = (rx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t rx_phys = mm_allocate_contiguous_pages(rx_ring_pages);
	if (!rx_phys) {
		kprintf("E1000: Failed to allocate RX descriptor ring\n");
		return -1;
	}

	dev->rx_descs = (e1000_rx_desc_legacy_t *)phys_to_virt(rx_phys);
	dev->rx_descs_phys = rx_phys;
	WARN_ON(rx_phys & 0xF); /* RX descriptor ring must be 16-byte aligned */
	for (uint32_t i = 0; i < rx_ring_size / 8; i++)
		((uint64_t *)dev->rx_descs)[i] = 0;

	// Allocate RX packet buffers
	for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
		uint64_t buf_phys = mm_allocate_physical_page();
		if (!buf_phys) {
			kprintf("E1000: Failed to allocate RX buffer %d\n", i);
			return -1;
		}
		dev->rx_bufs[i] = (uint8_t *)phys_to_virt(buf_phys);
		dev->rx_bufs_phys[i] = buf_phys;

		// Set up legacy RX descriptor
		dev->rx_descs[i].buffer_addr = buf_phys;
		dev->rx_descs[i].status = 0;
	}

	// Program RX ring registers
	e1000_write(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
	e1000_write(dev, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
	e1000_write(dev, E1000_RDLEN, rx_ring_size);
	e1000_write(dev, E1000_RDH, 0);
	e1000_write(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);

	/* TCP/UDP receive checksum offload: the MAC reports in each
	 * descriptor whether it verified the checksum (e1000_rx_deliver). */
	e1000_write(dev, E1000_RXCSUM,
		    e1000_read(dev, E1000_RXCSUM) | E1000_RXCSUM_TUOFL);

	/* Interrupt moderation: no per-frame receive delay, the rate capped
	 * by ITR from 20000/s; the poll adapts it (e1000_update_itr). */
	e1000_write(dev, E1000_RDTR, 0);
	e1000_write(dev, E1000_RADV, 8);
	dev->itr_rate = NIC_ITR_RATE_LOW;
	dev->itr_rx_class = dev->itr_tx_class = NIC_ITR_LOW;
	e1000_write(dev, E1000_ITR, nic_itr_reg(dev->itr_rate));

	// Configure receive control
	uint32_t rctl = E1000_RCTL_EN // Enable receiver
			| E1000_RCTL_BAM // Accept broadcast
			| E1000_RCTL_BSIZE_2048 // 2048 byte buffers
			| E1000_RCTL_SECRC; // Strip CRC

	e1000_write(dev, E1000_RCTL, rctl);

	dev->rx_tail = E1000_NUM_RX_DESC - 1;
	dev->rx_discarding = 0;
	return 0;
}

// ============================================================================
// Initialize TX Ring
// ============================================================================
/* Fiber and SerDes parts take a transmit IPG of 9, copper 8. */
static int e1000_is_fiber(uint16_t did)
{
	switch (did) {
	case 0x1003:
	case 0x1009:
	case 0x1011:
	case 0x1012:
	case 0x1027:
	case 0x1028:
	case 0x107A:
	case 0x107B:
		return 1;
	default:
		return 0;
	}
}

static int e1000_init_tx(e1000_dev_t *dev)
{
	uint32_t tx_ring_size =
		sizeof(e1000_tx_desc_legacy_t) * E1000_NUM_TX_DESC;
	uint32_t tx_ring_pages = (tx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t tx_phys = mm_allocate_contiguous_pages(tx_ring_pages);
	if (!tx_phys) {
		kprintf("E1000: Failed to allocate TX descriptor ring\n");
		return -1;
	}

	dev->tx_descs = (e1000_tx_desc_legacy_t *)phys_to_virt(tx_phys);
	dev->tx_descs_phys = tx_phys;
	WARN_ON(tx_phys & 0xF); /* TX descriptor ring must be 16-byte aligned */
	for (uint32_t i = 0; i < tx_ring_size / 8; i++)
		((uint64_t *)dev->tx_descs)[i] = 0;

	// Allocate TX packet buffers
	for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
		uint64_t buf_phys = mm_allocate_physical_page();
		if (!buf_phys) {
			kprintf("E1000: Failed to allocate TX buffer %d\n", i);
			return -1;
		}
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(buf_phys);
		dev->tx_bufs_phys[i] = buf_phys;
		dev->tx_stamp[i] = 0;
	}

	// Program TX ring registers
	e1000_write(dev, E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
	e1000_write(dev, E1000_TDBAH, (uint32_t)(tx_phys >> 32));
	e1000_write(dev, E1000_TDLEN, tx_ring_size);
	e1000_write(dev, E1000_TDH, 0);
	e1000_write(dev, E1000_TDT, 0);

	/* Transmit inter-packet gap of the 82543 and later: IPGT 8 on copper
	 * (9 on fiber), IPGR1 8, IPGR2 6. */
	e1000_write(dev, E1000_TIPG,
		    (e1000_is_fiber(dev->device_id) ? 9u : 8u) | (8u << 10) |
			    (6u << 20));

	/* Transmit interrupt delay: descriptors carry IDE (e1000_xmit), TXDW
	 * waits TIDV for more completions, TADV bounds the wait. */
	e1000_write(dev, E1000_TIDV_REG, 8);
	e1000_write(dev, E1000_TADV_REG, 32);

	// Configure transmit control
	uint32_t tctl =
		E1000_TCTL_EN // Enable transmitter
		| E1000_TCTL_PSP // Pad short packets
		| (15 << E1000_TCTL_CT_SHIFT) // Collision threshold
		| (64
		   << E1000_TCTL_COLD_SHIFT) // Collision distance (full duplex)
		| E1000_TCTL_RTLC; // Retransmit on late collision

	e1000_write(dev, E1000_TCTL, tctl);

	dev->tx_tail = 0;
	dev->tx_clean = 0;
	return 0;
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================
#define E1000_TX_WAKE_THRESH 32
#define E1000_IMS_NAPI                                        \
	(E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_RXDMT0 | \
	 E1000_ICR_RXO)
#define E1000_IMS_ALL (E1000_IMS_NAPI | E1000_ICR_LSC)

static uint32_t e1000_tx_unused(const e1000_dev_t *dev)
{
	return nic_ring_unused(dev->tx_tail, dev->tx_clean, E1000_NUM_TX_DESC);
}

/* Reclaim written-back descriptors, oldest first.  Caller holds tx_lock. */
static uint32_t e1000_tx_clean_locked(e1000_dev_t *dev)
{
	uint32_t n = 0;

	while (dev->tx_clean != dev->tx_tail) {
		volatile e1000_tx_desc_legacy_t *d =
			&dev->tx_descs[dev->tx_clean];
		uint8_t st = d->status;

		if (!(st & E1000_TXD_STAT_DD))
			break;
		if (st & (E1000_TXD_STAT_EC | E1000_TXD_STAT_LC))
			dev->net_dev.tx_errors++;
		dev->itr_tx_packets++;
		dev->itr_tx_bytes += d->length;
		d->cmd = 0;
		d->status = 0;
		dev->tx_stamp[dev->tx_clean] = 0;
		dev->tx_clean =
			(uint16_t)((dev->tx_clean + 1) % E1000_NUM_TX_DESC);
		n++;
	}
	return n;
}

static void e1000_tx_maybe_wake_locked(e1000_dev_t *dev)
{
	if (netif_queue_stopped(&dev->net_dev) &&
	    e1000_tx_unused(dev) >= E1000_TX_WAKE_THRESH)
		netif_wake_queue(&dev->net_dev);
}

/* One frame to the hardware (the core calls this one CPU at a time; tx_lock
 * orders it against the poll's reclaim and the watchdog's ring reset). */
static int e1000_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;
	uint16_t i;

	BUG_ON(dev == NULL);
	BUILD_BUG_ON(sizeof(e1000_rx_desc_legacy_t) != 16);
	BUILD_BUG_ON(sizeof(e1000_tx_desc_legacy_t) != 16);
	if (len == 0 || len > E1000_RX_BUF_SIZE) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&dev->tx_lock, &f);
	if (e1000_tx_unused(dev) == 0) {
		e1000_tx_clean_locked(dev);
		if (e1000_tx_unused(dev) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}

	i = dev->tx_tail;
	volatile e1000_tx_desc_legacy_t *desc = &dev->tx_descs[i];

	mm_memcpy(dev->tx_bufs[i], skb->data, len);
	desc->buffer_addr = dev->tx_bufs_phys[i];
	desc->length = (uint16_t)len;
	desc->cso = 0;
	desc->status = 0;
	desc->css = 0;
	desc->special = 0;
	desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS |
		    E1000_TXD_CMD_IDE;
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp[i] = now ? now : 1;
	}

	// Descriptor and buffer before the doorbell
	__asm__ volatile("mfence" ::: "memory");

	dev->tx_tail = (uint16_t)((i + 1) % E1000_NUM_TX_DESC);
	e1000_write(dev, E1000_TDT, dev->tx_tail);

	ndev->tx_packets++;
	ndev->tx_bytes += len;
	if (e1000_tx_unused(dev) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

/* Throw away what the transmit ring holds and restart it empty (link lost
 * with frames pending, or a transmit hang).  Only TCTL.EN and the ring
 * registers are touched -- never CTRL.RST, which breaks reception on the
 * VirtualBox e1000. */
static void e1000_tx_ring_reset(e1000_dev_t *dev)
{
	uint64_t f;
	uint32_t pending;

	spin_lock_irqsave(&dev->tx_lock, &f);
	e1000_tx_clean_locked(dev);
	pending = (uint32_t)((dev->tx_tail + E1000_NUM_TX_DESC -
			      dev->tx_clean) %
			     E1000_NUM_TX_DESC);
	if (!pending) {
		spin_unlock_irqrestore(&dev->tx_lock, f);
		return;
	}
	uint32_t tctl = e1000_read(dev, E1000_TCTL);
	e1000_write(dev, E1000_TCTL, tctl & ~E1000_TCTL_EN);
	(void)e1000_read(dev, E1000_STATUS);
	e1000_delay_us(100);
	for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
		volatile e1000_tx_desc_legacy_t *d = &dev->tx_descs[i];

		d->cmd = 0;
		d->status = 0;
		d->length = 0;
		dev->tx_stamp[i] = 0;
	}
	__asm__ volatile("mfence" ::: "memory");
	dev->tx_tail = 0;
	dev->tx_clean = 0;
	e1000_write(dev, E1000_TDH, 0);
	e1000_write(dev, E1000_TDT, 0);
	e1000_write(dev, E1000_TCTL, tctl);
	dev->net_dev.tx_errors += pending;
	e1000_tx_maybe_wake_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
}

// ============================================================================
// Link state, receive filter, statistics, watchdog
// ============================================================================
static uint32_t e1000_status_speed(uint32_t status)
{
	switch ((status >> 6) & 0x3) {
	case 0:
		return 10;
	case 1:
		return 100;
	default:
		return 1000;
	}
}

/* Act on a link change: carrier, the DHCP lease, the transmit ring.  The
 * LSC interrupt only flags it; this runs in the watchdog (CPU 0). */
static void e1000_link_state_poll(e1000_dev_t *dev)
{
	uint32_t status = e1000_read(dev, E1000_STATUS);
	int now_up = (status & E1000_STATUS_LU) ? 1 : 0;
	int was_up = dev->link_up;
	int pending = __atomic_exchange_n(&dev->link_change_pending, 0,
					  __ATOMIC_ACQ_REL);

	if (now_up == was_up && !pending)
		return;
	if (!now_up) {
		if (was_up) {
			dev->link_up = 0;
			dev->link_speed = 0;
			kprintf("E1000: Link DOWN\n");
			netif_carrier_off(&dev->net_dev);
			// The network may be another one when the link
			// returns (cable moved, bridged<->NAT): full DISCOVER.
			dhcp_invalidate(&dev->net_dev);
		}
		e1000_tx_ring_reset(dev);
		return;
	}
	dev->link_speed = e1000_status_speed(status);
	if (!was_up) {
		dev->link_up = 1;
		kprintf("E1000: Link UP (%u Mb/s, %s duplex)\n",
			dev->link_speed,
			(status & E1000_STATUS_FD) ? "full" : "half");
	}
	netif_carrier_on(&dev->net_dev);
}

static void e1000_apply_rx_mode(e1000_dev_t *dev)
{
	uint8_t mc[NETDEV_MC_MAX][6];
	uint32_t mta[128];
	int all = 0;
	int n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
	uint32_t flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);

	for (int r = 0; r < 128; r++)
		mta[r] = 0;
	for (int i = 0; i < n; i++) {
		uint32_t h = nic_mta_hash(mc[i], 128);

		mta[h >> 5] |= 1u << (h & 31);
	}
	for (int r = 0; r < 128; r++)
		e1000_write(dev, E1000_MTA + r * 4, mta[r]);

	uint32_t rctl = e1000_read(dev, E1000_RCTL) &
			~(E1000_RCTL_UPE | E1000_RCTL_MPE);
	if (flags & IFF_PROMISC)
		rctl |= E1000_RCTL_UPE | E1000_RCTL_MPE;
	else if (all || (flags & IFF_ALLMULTI))
		rctl |= E1000_RCTL_MPE;
	e1000_write(dev, E1000_RCTL, rctl);
}

static void e1000_set_rx_mode(net_device_t *ndev)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

static void e1000_update_stats(e1000_dev_t *dev)
{
	dev->st_crc += e1000_read(dev, E1000_ST_CRCERRS);
	dev->st_align += e1000_read(dev, E1000_ST_ALGNERRC);
	dev->st_rxerr += e1000_read(dev, E1000_ST_RXERRC);
	dev->st_missed += e1000_read(dev, E1000_ST_MPC);
	dev->st_colls += e1000_read(dev, E1000_ST_COLC);
	dev->st_latecol += e1000_read(dev, E1000_ST_LATECOL);
	dev->st_ecol += e1000_read(dev, E1000_ST_ECOL);
	dev->st_tncrs += e1000_read(dev, E1000_ST_TNCRS);
	dev->st_rlen += e1000_read(dev, E1000_ST_RLEC) +
			e1000_read(dev, E1000_ST_RUC) +
			e1000_read(dev, E1000_ST_ROC);
	dev->st_nobuf += e1000_read(dev, E1000_ST_RNBC);
}

static void e1000_get_stats64(net_device_t *ndev, struct net_device_stats64 *s)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;

	s->rx_crc_errors = dev->st_crc;
	s->rx_frame_errors = dev->st_align;
	s->rx_missed_errors = dev->st_missed;
	s->rx_length_errors = dev->st_rlen + dev->st_rx_frag;
	s->rx_fifo_errors = dev->st_nobuf;
	s->collisions = dev->st_colls;
	s->tx_aborted_errors = dev->st_ecol;
	s->tx_carrier_errors = dev->st_tncrs;
	s->rx_errors += dev->st_crc + dev->st_align + dev->st_rxerr +
			dev->st_rlen;
	s->tx_errors += dev->st_ecol + dev->st_latecol;
}

/* The oldest transmit descriptor not done for longer than the link speed
 * allows (link up, transmitter not paused): restart the transmit ring. */
static void e1000_check_tx_hang(e1000_dev_t *dev)
{
	static uint32_t reported;
	uint64_t now = timer_ticks(), f;
	uint32_t limit_ms;
	int hang = 0;

	if (!dev->link_up)
		return;
	limit_ms = dev->link_speed == 1000 ? 2000 :
		   dev->link_speed == 100  ? 10000 :
					     16000;
	spin_lock_irqsave(&dev->tx_lock, &f);
	if (dev->tx_clean != dev->tx_tail) {
		uint64_t stamp = dev->tx_stamp[dev->tx_clean];

		if (stamp && now - stamp > net_ms_to_ticks(limit_ms) &&
		    !(e1000_read(dev, E1000_STATUS) & E1000_STATUS_TXOFF))
			hang = 1;
	}
	spin_unlock_irqrestore(&dev->tx_lock, f);
	if (!hang)
		return;
	dev->st_tx_hangs++;
	if (reported < 5) {
		reported++;
		kprintf("E1000: transmit unit hang (TDH=%u TDT=%u), restarting the transmit ring\n",
			e1000_read(dev, E1000_TDH), e1000_read(dev, E1000_TDT));
	}
	e1000_tx_ring_reset(dev);
}

static void e1000_tx_timeout(net_device_t *ndev)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;

	dev->st_tx_hangs++;
	e1000_tx_ring_reset(dev);
}

// Network housekeeping softirq, CPU 0, ~10 Hz and after an LSC interrupt.
static void e1000_watchdog(net_device_t *ndev)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;
	uint64_t f;

	if (!dev || !g_e1000_initialized)
		return;
	e1000_link_state_poll(dev);
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL))
		e1000_apply_rx_mode(dev);
	spin_lock_irqsave(&dev->tx_lock, &f);
	e1000_tx_clean_locked(dev);
	e1000_tx_maybe_wake_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	e1000_check_tx_hang(dev);
	if (timer_ticks() - dev->stats_tick >= net_ms_to_ticks(2000)) {
		dev->stats_tick = timer_ticks();
		e1000_update_stats(dev);
	}
}

// Polled by userland (ifconfig etc.): STATUS.LU is the ground truth; a
// change is handed to the watchdog, which does the edge work.
static int e1000_link_status(net_device_t *ndev)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;
	int up = (e1000_read(dev, E1000_STATUS) & E1000_STATUS_LU) ? 1 : 0;

	if (up != dev->link_up) {
		__atomic_store_n(&dev->link_change_pending, 1,
				 __ATOMIC_RELEASE);
		net_housekeeping_kick();
	}
	return up;
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the
// 8254x family datasheet, leaving IMS / RCTL.EN / TCTL.EN / WUC.APME
// asserted at sleep time can cause the chipset to keep PME# alive and
// hold the platform in a half-powered state.  Mask all interrupts,
// halt RX/TX, clear Wake-on-LAN, and drop PCI bus-master so any
// in-flight DMA is dropped by the root complex.
static void e1000_shutdown(net_device_t *ndev)
{
	e1000_dev_t *dev = (e1000_dev_t *)ndev->driver_data;
	if (!dev || !dev->mmio_base)
		return;

	e1000_write(dev, E1000_IMC, 0xFFFFFFFFu);
	(void)e1000_read(dev, E1000_ICR);

	uint32_t rctl = e1000_read(dev, E1000_RCTL);
	e1000_write(dev, E1000_RCTL, rctl & ~E1000_RCTL_EN);
	uint32_t tctl = e1000_read(dev, E1000_TCTL);
	e1000_write(dev, E1000_TCTL, tctl & ~E1000_TCTL_EN);

	// WUC (Wake Up Control) at 0x05800, WUFC (Wake Up Filter Control) at
	// 0x05808 — clearing both deasserts PME# capability.
	e1000_write(dev, 0x05808u /* WUFC */, 0);
	e1000_write(dev, 0x05800u /* WUC  */, 0);

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
static void e1000_rx_deliver(e1000_dev_t *dev, const uint8_t *buf,
			     uint32_t len, uint8_t status, uint8_t errors)
{
	net_device_t *nd = &dev->net_dev;
	sk_buff_t *skb = skb_alloc(len);
	int cs;

	if (!skb) {
		nd->rx_dropped++;
		return;
	}
	skb->dev = nd;
	mm_memcpy(skb_append(skb, len), buf, len);
	// the 8254x folds UDP into TCPCS and reports IP errors apart
	cs = nic_e1000_rx_csum(status, errors, 0, 0);
	if (cs == NIC_CSUM_OK && (nd->features & NETIF_F_RXCSUM))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	else if (cs == NIC_CSUM_BAD)
		dev->st_csum_err++;
	nd->rx_packets++;
	nd->rx_bytes += len;
	if (nic_is_multicast(buf))
		nd->multicast++;
	dev->itr_rx_packets++;
	dev->itr_rx_bytes += len;
	napi_gro_receive(&dev->napi, skb);
}

static int e1000_rx_clean(e1000_dev_t *dev, int budget)
{
	uint16_t ntc = (uint16_t)((dev->rx_tail + 1) % E1000_NUM_RX_DESC);
	uint16_t last = dev->rx_tail;
	int work = 0, refill = 0;

	while (work < budget) {
		volatile e1000_rx_desc_legacy_t *d = &dev->rx_descs[ntc];
		uint8_t status = d->status;

		if (!(status & E1000_RXD_STAT_DD))
			break;
		// the rest of the descriptor only after DD (volatile, in order)
		uint8_t errors = d->errors;
		uint16_t len = d->length;

		switch (nic_e1000_rx_verdict(status, errors, len,
					     E1000_RX_BUF_SIZE,
					     &dev->rx_discarding)) {
		case NIC_RXV_DELIVER:
			e1000_rx_deliver(dev, dev->rx_bufs[ntc], len, status,
					 errors);
			break;
		case NIC_RXV_ERROR:
			dev->net_dev.rx_errors++;
			break;
		default:
			dev->st_rx_frag++;
			break;
		}
		d->status = 0;
		d->errors = 0;
		last = ntc;
		ntc = (uint16_t)((ntc + 1) % E1000_NUM_RX_DESC);
		work++;
		/* Batch the RDT MMIO write (one PCIe round trip) per 32
		 * descriptors rather than one per descriptor. */
		if (++refill >= 32) {
			__asm__ volatile("mfence" ::: "memory");
			dev->rx_tail = last;
			e1000_write(dev, E1000_RDT, last);
			refill = 0;
		}
	}
	if (refill) {
		__asm__ volatile("mfence" ::: "memory");
		dev->rx_tail = last;
		e1000_write(dev, E1000_RDT, last);
	}
	return work;
}

static void e1000_update_itr(e1000_dev_t *dev, uint32_t txp, uint32_t txb)
{
	uint32_t target;

	if (dev->link_speed != 1000) {
		target = NIC_ITR_RATE_BULK;
	} else {
		int cls;

		dev->itr_rx_class = nic_itr_classify(dev->itr_rx_class,
						     dev->itr_rx_packets,
						     dev->itr_rx_bytes);
		dev->itr_tx_class =
			nic_itr_classify(dev->itr_tx_class, txp, txb);
		cls = dev->itr_rx_class > dev->itr_tx_class ?
			      dev->itr_rx_class :
			      dev->itr_tx_class;
		target = nic_itr_class_rate(cls, 1);
	}
	dev->itr_rx_packets = 0;
	dev->itr_rx_bytes = 0;
	if (target != dev->itr_rate) {
		dev->itr_rate = nic_itr_next_rate(dev->itr_rate, target);
		e1000_write(dev, E1000_ITR, nic_itr_reg(dev->itr_rate));
	}
}

static int e1000_poll(struct napi_struct *n, int budget)
{
	e1000_dev_t *dev = (e1000_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	spin_lock_irqsave(&dev->tx_lock, &f);
	e1000_tx_clean_locked(dev);
	e1000_tx_maybe_wake_locked(dev);
	dev->itr_tx_acc_packets += dev->itr_tx_packets;
	dev->itr_tx_acc_bytes += dev->itr_tx_bytes;
	dev->itr_tx_packets = 0;
	dev->itr_tx_bytes = 0;
	spin_unlock_irqrestore(&dev->tx_lock, f);

	work = e1000_rx_clean(dev, budget);
	if (work < budget) {
		e1000_update_itr(dev, dev->itr_tx_acc_packets,
				 dev->itr_tx_acc_bytes);
		dev->itr_tx_acc_packets = 0;
		dev->itr_tx_acc_bytes = 0;
		if (napi_complete_done(n, work))
			e1000_write(dev, E1000_IMS, E1000_IMS_NAPI);
	}
	return work;
}

// ============================================================================
// E1000 IRQ Handler
// ============================================================================
/* Acknowledge the causes; receive and transmit completion go to the NAPI
 * poll (their causes masked until it is done), link changes to the
 * watchdog.  Returns 1 when the interrupt was this device's (a cause it
 * has enabled was pending: nic_e1000_irq_ours), 0 when it was not -- on a
 * shared legacy line the dispatcher then offers it to the next handler.
 * The dispatcher, not the handler, acknowledges the local APIC. */
int e1000_irq_handler(void)
{
	if (!g_e1000_initialized)
		return 0;

	// Feed entropy from NIC interrupt timing
	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	e1000_dev_t *dev = &g_e1000;
	uint32_t icr = e1000_read(dev, E1000_ICR);
	int handled;
	WARN_ON_ONCE(icr == 0xFFFFFFFF); /* ICR all-ones: MMIO bus error */

	if (icr == 0 || icr == 0xFFFFFFFF)
		return 0; // no cause: not ours (or spurious)
	handled = nic_e1000_irq_ours(icr, e1000_read(dev, E1000_IMS));

	if (icr & E1000_ICR_LSC) {
		dev->link_change_pending = 1;
		net_housekeeping_kick();
	}
	/* Receive/transmit work also when the interrupt was another
	 * device's: the read consumed the cause, and a poll about to
	 * re-enable its interrupts would not hear of it otherwise. */
	if (icr & E1000_IMS_NAPI) {
		e1000_write(dev, E1000_IMC, E1000_IMS_NAPI);
		napi_schedule(&dev->napi);
	}
	return handled;
}

// ============================================================================
// E1000 Map MMIO BAR
// ============================================================================
static int e1000_map_bar(e1000_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];

	if (bar0 & 1) {
		kprintf("E1000: BAR0 is I/O port, not MMIO\n");
		return -1;
	}

	// Get physical address (mask off lower bits)
	uint64_t phys = bar0 & 0xFFFFFFF0ULL;

	// Check if 64-bit BAR
	if ((bar0 & 0x06) == 0x04) {
		phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
	}

	dev->mmio_phys = phys;
	dev->mmio_size = 128 * 1024; // E1000 MMIO is typically 128KB

	/* Strictly uncached, also where the BAR lies inside the write-back
	 * direct map (a write-back alias of device registers lets writes
	 * be combined and reordered). */
	uint32_t num_pages = (dev->mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t virt_base = mm_map_device_mmio(phys, num_pages);
	if (!virt_base) {
		kprintf("E1000: Failed to map BAR0 MMIO (phys=%llx)\n",
			(unsigned long long)phys);
		return -1;
	}
	dev->mmio_base = (volatile uint8_t *)virt_base;
	return 0;
}

// ============================================================================
// E1000 Initialization
// ============================================================================
void e1000_init(void)
{
	might_sleep();
	BUILD_BUG_ON(sizeof(e1000_rx_desc_legacy_t) != 16);
	BUILD_BUG_ON(sizeof(e1000_tx_desc_legacy_t) != 16);
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		if (devs[i].vendor_id != E1000_VENDOR_ID)
			continue;

		uint16_t did = devs[i].device_id;
		const e1000_id_t *match = e1000_lookup(did);
		if (!match)
			continue;
		const char *name = match->name;

		kprintf("E1000: Found %s (PCI %02x:%02x.%x)\n", name,
			devs[i].bus, devs[i].device, devs[i].function);

		e1000_dev_t *dev = &g_e1000;
		dev->pci_dev = &devs[i];
		dev->device_id = did;

		// Enable bus mastering and memory space
		pci_enable_busmaster_mem(dev->pci_dev);

		// Map MMIO
		if (e1000_map_bar(dev) < 0) {
			kprintf("E1000: Failed to map BAR0\n");
			return;
		}

		// Reset device
		e1000_reset(dev);

		// Read MAC address
		e1000_read_mac(dev);
		kprintf("E1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		// Set link up — only add SLU; preserve UEFI speed/duplex settings
		// to avoid triggering a PHY re-negotiation that drops the link.
		uint32_t ctrl = e1000_read(dev, E1000_CTRL);
		ctrl |= E1000_CTRL_SLU;
		ctrl &= ~E1000_CTRL_PHY_RST;
		e1000_write(dev, E1000_CTRL, ctrl);

		// Clear multicast table
		for (int m = 0; m < 128; m++) {
			e1000_write(dev, E1000_MTA + m * 4, 0);
		}

		// Set MAC in receive address register
		uint32_t ral = (uint32_t)dev->mac_addr[0] |
			       ((uint32_t)dev->mac_addr[1] << 8) |
			       ((uint32_t)dev->mac_addr[2] << 16) |
			       ((uint32_t)dev->mac_addr[3] << 24);
		uint32_t rah = (uint32_t)dev->mac_addr[4] |
			       ((uint32_t)dev->mac_addr[5] << 8) |
			       (1 << 31); // Address Valid
		e1000_write(dev, E1000_RAL, ral);
		e1000_write(dev, E1000_RAH, rah);

		// Initialize RX and TX rings
		if (e1000_init_rx(dev) < 0)
			return;
		if (e1000_init_tx(dev) < 0)
			return;

		// Enable MSI interrupt
		dev->msi_vector = E1000_MSI_VECTOR;
		int msi_ret = pci_enable_msi(dev->pci_dev, dev->msi_vector);
		if (msi_ret < 0) {
			dev->msi_vector = 0;

			// ALWAYS resolve the IOAPIC GSI from ACPI _PRT first when the
			// IOAPIC is in use.  PCI config-space `interrupt_line` reflects
			// the legacy 8259 PIC IRQ assigned by the BIOS — it is NOT the
			// IOAPIC GSI in APIC mode.  On QEMU/VMware the e1000 happens
			// to land on GSI 11 so the legacy line value matches by
			// coincidence; on VirtualBox with the ICH9 chipset INTA of
			// device 3 is routed via PIRQ to a GSI in the 16-23 range,
			// and trusting `interrupt_line` would program the wrong
			// IOAPIC redirection entry — exactly the symptom we observed
			// (TX works, no RX interrupt ever delivered).
			uint8_t irq = 0xFF;
			uint32_t gsi = 0;
			uint8_t pin = dev->pci_dev->interrupt_pin;
			if (pin >= 1 && pin <= 4) {
				uint8_t acpi_pin =
					pin - 1; // PCI pin 1-4 -> ACPI pin 0-3

				// For devices behind PCI-to-PCI bridges, we must:
				// 1. Find the bridge on bus 0 that owns this secondary bus
				// 2. Apply PCI interrupt pin swizzling
				// 3. Look up the bridge device (not the NIC) in the root _PRT
				uint8_t lookup_dev = dev->pci_dev->device;
				uint8_t lookup_pin = acpi_pin;

				if (dev->pci_dev->bus != 0) {
					const pci_device_t *bridge =
						pci_find_bridge_for_bus(
							dev->pci_dev->bus);
					if (bridge) {
						lookup_pin =
							(acpi_pin +
							 dev->pci_dev->device) %
							4;
						lookup_dev = bridge->device;
					}
				}

				if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
							lookup_dev, lookup_pin,
							&gsi) == 0 &&
				    gsi <= 23) {
					irq = (uint8_t)gsi;
					kprintf("E1000: ACPI _PRT resolved INT%c -> GSI %u\n",
						'A' + acpi_pin, gsi);
				}
			}

			// Fallback: trust PCI config-space interrupt_line (only valid
			// on systems where the BIOS happens to program the same value
			// the IOAPIC uses, e.g. QEMU/VMware/most legacy setups).
			if (irq == 0xFF) {
				irq = dev->pci_dev->interrupt_line;
				if (irq != 0xFF && irq <= 23) {
					kprintf("E1000: ACPI _PRT lookup failed, falling back to "
						"PCI interrupt_line = %d\n",
						irq);
				} else {
					kprintf("E1000: WARNING: no valid IRQ (line=%d, _PRT failed)\n",
						dev->pci_dev->interrupt_line);
					kprintf("E1000: Interrupts will not work\n");
				}
			}

			kprintf("E1000: Using legacy IRQ %d\n", irq);
			g_e1000_legacy_irq = irq;

			// VirtualBox's UEFI firmware leaves PCI Command bit 10 (INTx
			// Disable) SET on the emulated 82540EM, which prevents the
			// device from ever asserting its interrupt pin.  TX still
			// works (it doesn't need the IRQ) but no RX/LSC/etc. interrupt
			// is ever delivered to the IOAPIC.  Explicitly clear bit 10
			// for the legacy IRQ path so the wire actually toggles.
			// (QEMU and VMware leave the bit clear, so this is a no-op
			// there.)
			{
				uint32_t cmd = pci_cfg_read32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04);
				if (cmd & PCI_CMD_INTX_DISABLE) {
					cmd &= ~PCI_CMD_INTX_DISABLE;
					pci_cfg_write32(dev->pci_dev->bus,
							dev->pci_dev->device,
							dev->pci_dev->function,
							0x04, cmd);
					kprintf("E1000: cleared PCI Command INTx Disable bit\n");
				}
			}

			if (irq <= 23) {
				uint8_t vector = 32 + irq;
				ioapic_configure_legacy_irq(
					irq, vector, IOAPIC_POLARITY_LOW,
					IOAPIC_TRIGGER_LEVEL);
			}
		} else {
			kprintf("E1000: MSI enabled (vector %d)\n",
				dev->msi_vector);
		}

		// Mark the driver as initialised BEFORE enabling device interrupts.
		// The IRQ dispatcher (kernel/ke/interrupt.c) only routes the legacy
		// IRQ to e1000_irq_handler() when g_e1000_initialized != 0; if the
		// device fires an interrupt (LSC, etc.) before this flag is set the
		// dispatcher falls through to the other handlers and nothing
		// acknowledges the device, locking up the system in a
		// level-triggered IRQ storm.
		// We populate the minimum state e1000_irq_handler() touches.
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("e1000");
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("e1000_tx");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, e1000_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_e1000_initialized = 1;

		// Check link status.  After a CTRL.RST, VirtualBox's E1000
		// emulation can take several seconds to bring the link back up.
		// Use PM Timer for accurate timing (up to 5 seconds).
		uint32_t status = e1000_read(dev, E1000_STATUS);
		if (!(status & E1000_STATUS_LU)) {
			uint32_t t0 = timer_pmtimer_read_raw();
			while (timer_pmtimer_delta_us(
				       t0, timer_pmtimer_read_raw()) <
			       5000000) {
				e1000_delay_us(50000); // check every 50 ms
				status = e1000_read(dev, E1000_STATUS);
				if (status & E1000_STATUS_LU)
					break;
			}
		}
		dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
		if (dev->link_up) {
			dev->link_speed = e1000_status_speed(status);
			kprintf("E1000: Link UP (%u Mb/s, %s duplex)\n",
				dev->link_speed,
				(status & E1000_STATUS_FD) ? "full" : "half");
		} else {
			kprintf("E1000: Link DOWN\n");
		}

		// Set up net_device_t
		dev->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			dev->net_dev.mac_addr[m] = dev->mac_addr[m];
		dev->net_dev.mtu = NET_MTU_DEFAULT;
		dev->net_dev.ip_addr = 0;
		dev->net_dev.netmask = 0;
		dev->net_dev.gateway = 0;
		dev->net_dev.dns_server = 0;
		dev->net_dev.send = NULL;
		dev->net_dev.xmit = e1000_xmit;
		dev->net_dev.link_status = e1000_link_status;
		dev->net_dev.shutdown = e1000_shutdown;
		dev->net_dev.watchdog = e1000_watchdog;
		dev->net_dev.tx_timeout = e1000_tx_timeout;
		dev->net_dev.set_rx_mode = e1000_set_rx_mode;
		dev->net_dev.get_stats64 = e1000_get_stats64;
		dev->net_dev.features |= NETIF_F_RXCSUM;
		dev->stats_tick = timer_ticks();
		// Note: lock and statistics counters are initialised earlier (before
		// the handler can run) so the IRQ handler always sees a valid
		// net_device_t.

		// Register with network subsystem
		net_register(&dev->net_dev);
		if (!dev->link_up)
			netif_carrier_off(&dev->net_dev);

		// Enable interrupts: receive/transmit (NAPI) and link change
		e1000_write(dev, E1000_IMS, E1000_IMS_ALL);

		kprintf("E1000: Driver initialized successfully\n");
		return; // Only handle first device
	}
}
