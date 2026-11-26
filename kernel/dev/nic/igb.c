// LikeOS Intel 82576 Gigabit Ethernet Controller (igb) NIC Driver
//
// Drives the 82575 / 82576 / I350 / I210 / I211 family.  QEMU exposes
// `-device igb` as a 82576 (vendor 0x8086, device 0x10C9).  The 8257x
// classic register layout (CTRL/STATUS/RCTL/TCTL/RAL/RAH/IMS/ICR) is
// preserved on the 82576; the per-queue rings live at 0xC000/0xE000
// instead of 0x2800/0x3800 and need their RXDCTL/TXDCTL.ENABLE bit
// asserted.  Interrupts arrive by MSI when the device offers it (a
// vector from the interrupt allocator), else by the legacy INTx line.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/igb.h>
#include <kernel/dev/nic/nic_util.h>
#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/skb.h>
#include <kernel/ke/irq.h>
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
// Supported PCI device IDs
// ============================================================================
typedef struct {
	uint16_t did;
	const char *name;
} igb_id_t;

static const igb_id_t igb_pci_ids[] = {
	// 82575
	{ IGB_DEV_82575EB_COPPER, "Intel 82575EB Copper" },
	{ IGB_DEV_82575EB_FIBER, "Intel 82575EB Fiber" },
	{ IGB_DEV_82575GB_QUAD, "Intel 82575GB Quad Copper" },
	// 82576
	{ IGB_DEV_82576,
	  "Intel 82576 Gigabit Ethernet Controller" }, // QEMU -device igb
	{ IGB_DEV_82576_FIBER, "Intel 82576 Fiber" },
	{ IGB_DEV_82576_SERDES, "Intel 82576 SerDes" },
	{ IGB_DEV_82576_QUAD, "Intel 82576 Quad Copper" },
	{ IGB_DEV_82576_NS, "Intel 82576 NS" },
	{ IGB_DEV_82576_NS_SERDES, "Intel 82576 NS SerDes" },
	{ IGB_DEV_82576_SERDES_QD, "Intel 82576 Backplane SerDes" },
	{ IGB_DEV_82576_QUAD_ET2, "Intel 82576 Quad Copper ET2" },
	// I350
	{ IGB_DEV_I350_COPPER, "Intel I350 Copper" },
	{ IGB_DEV_I350_FIBER, "Intel I350 Fiber" },
	{ IGB_DEV_I350_SERDES, "Intel I350 SerDes" },
	{ IGB_DEV_I350_SGMII, "Intel I350 SGMII" },
	// I210/I211
	{ IGB_DEV_I210_COPPER, "Intel I210 Copper" },
	{ IGB_DEV_I210_FIBER, "Intel I210 Fiber" },
	{ IGB_DEV_I210_SERDES, "Intel I210 SerDes" },
	{ IGB_DEV_I210_SGMII, "Intel I210 SGMII" },
	{ IGB_DEV_I211_COPPER, "Intel I211 Copper" },
	{ 0, NULL },
};

static const igb_id_t *igb_lookup(uint16_t did)
{
	for (const igb_id_t *e = igb_pci_ids; e->name; e++) {
		if (e->did == did)
			return e;
	}
	return NULL;
}

static igb_dev_t g_igb;
int g_igb_initialized = 0;
int g_igb_legacy_irq = -1;

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t igb_read(igb_dev_t *dev, uint32_t reg)
{
	return *(volatile uint32_t *)(dev->mmio_base + reg);
}
static inline void igb_write(igb_dev_t *dev, uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(dev->mmio_base + reg) = val;
	__asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void igb_delay_us(uint32_t us)
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
// Read MAC from RAL0 / RAH0 (the 82576 stores the factory MAC there at
// reset time; EEPROM read is much messier on 82576+)
// ============================================================================
static void igb_read_mac(igb_dev_t *dev)
{
	uint32_t ral = igb_read(dev, IGB_RAL0);
	uint32_t rah = igb_read(dev, IGB_RAH0);
	dev->mac_addr[0] = ral & 0xFF;
	dev->mac_addr[1] = (ral >> 8) & 0xFF;
	dev->mac_addr[2] = (ral >> 16) & 0xFF;
	dev->mac_addr[3] = (ral >> 24) & 0xFF;
	dev->mac_addr[4] = rah & 0xFF;
	dev->mac_addr[5] = (rah >> 8) & 0xFF;
}

// ============================================================================
// Soft init (do not assert CTRL.RST — leave UEFI's PHY negotiation alone)
// ============================================================================
static void igb_reset(igb_dev_t *dev)
{
	igb_write(dev, IGB_IMC, 0xFFFFFFFFu);
	(void)igb_read(dev, IGB_ICR);

	// Disable RX/TX for clean ring setup
	igb_write(dev, IGB_RCTL, 0);
	igb_write(dev, IGB_TCTL, 0);

	igb_delay_us(10000);

	igb_write(dev, IGB_IMC, 0xFFFFFFFFu);
	(void)igb_read(dev, IGB_ICR);
}

// ============================================================================
// Interrupt moderation (EITR of vector 0 in single-vector mode)
// ============================================================================
static void igb_write_itr(igb_dev_t *dev, uint32_t rate)
{
	uint32_t v = nic_itr_reg(rate) & IGB_EITR_INTERVAL_MASK;

	if (dev->device_id == IGB_DEV_82575EB_COPPER ||
	    dev->device_id == IGB_DEV_82575EB_FIBER ||
	    dev->device_id == IGB_DEV_82575GB_QUAD)
		v |= v << 16; // 82575: the counter takes the interval too
	else
		v |= IGB_EITR_CNT_IGNR;
	igb_write(dev, IGB_EITR0, v);
}

// ============================================================================
// RX ring (queue 0)
// ============================================================================
static int igb_init_rx(igb_dev_t *dev)
{
	uint32_t ring_bytes = sizeof(igb_rx_desc_t) * IGB_NUM_RX_DESC;
	uint32_t ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t phys = mm_allocate_contiguous_pages(ring_pages);
	if (!phys) {
		kprintf("IGB: Failed to allocate RX ring\n");
		return -1;
	}
	dev->rx_descs = (igb_rx_desc_t *)phys_to_virt(phys);
	dev->rx_descs_phys = phys;
	WARN_ON(phys & 0x7F); /* IGB RX ring must be 128-byte aligned */

	for (uint32_t i = 0; i < ring_bytes / 8; i++)
		((uint64_t *)dev->rx_descs)[i] = 0;

	for (int i = 0; i < IGB_NUM_RX_DESC; i++) {
		uint64_t bp = mm_allocate_physical_page();
		if (!bp) {
			kprintf("IGB: Failed to allocate RX buffer %d\n", i);
			return -1;
		}
		dev->rx_bufs[i] = (uint8_t *)phys_to_virt(bp);
		dev->rx_bufs_phys[i] = bp;
		// Advanced descriptor read format: pkt_addr / hdr_addr.
		dev->rx_descs[i].read.pkt_addr = bp;
		dev->rx_descs[i].read.hdr_addr = 0;
	}

	// Force single-queue mode (no RSS), so all incoming packets land in
	// queue 0 — the only ring we set up.
	igb_write(dev, IGB_MRQC, 0);

	igb_write(dev, IGB_RDBAL0, (uint32_t)(phys & 0xFFFFFFFF));
	igb_write(dev, IGB_RDBAH0, (uint32_t)(phys >> 32));
	igb_write(dev, IGB_RDLEN0, ring_bytes);
	igb_write(dev, IGB_RDH0, 0);
	igb_write(dev, IGB_RDT0, IGB_NUM_RX_DESC - 1);

	// Configure per-queue RX: 2 KiB packet buffer, advanced one-buffer
	// descriptor format.  RCTL.BSIZE is ignored on the 82576 in advanced
	// mode — SRRCTL is the source of truth for the queue's buffer size.
	igb_write(dev, IGB_SRRCTL0,
		  (2u << IGB_SRRCTL_BSIZEPACKET_SHIFT) |
			  IGB_SRRCTL_DESCTYPE_ADV1BUF);

	// IP and TCP/UDP receive checksum verification, reported per frame
	igb_write(dev, IGB_RXCSUM,
		  igb_read(dev, IGB_RXCSUM) | IGB_RXCSUM_IPOFL |
			  IGB_RXCSUM_TUOFL);

	// Interrupt moderation from 20000/s; the poll adapts it
	dev->itr_rate = NIC_ITR_RATE_LOW;
	dev->itr_rx_class = dev->itr_tx_class = NIC_ITR_LOW;
	igb_write_itr(dev, dev->itr_rate);

	// Enable queue 0
	uint32_t rxdctl = igb_read(dev, IGB_RXDCTL0);
	rxdctl |= IGB_QUEUE_ENABLE;
	igb_write(dev, IGB_RXDCTL0, rxdctl);

	// Wait for queue enable to stick (datasheet: poll bit 25)
	for (int i = 0; i < 1000; i++) {
		if (igb_read(dev, IGB_RXDCTL0) & IGB_QUEUE_ENABLE)
			break;
		igb_delay_us(100);
	}

	igb_write(dev, IGB_RCTL,
		  IGB_RCTL_EN | IGB_RCTL_BAM | IGB_RCTL_BSIZE_2048 |
			  IGB_RCTL_SECRC);

	dev->rx_tail = IGB_NUM_RX_DESC - 1;
	dev->rx_discarding = 0;
	return 0;
}

// ============================================================================
// TX ring (queue 0)
// ============================================================================
static int igb_init_tx(igb_dev_t *dev)
{
	uint32_t ring_bytes = sizeof(igb_adv_tx_desc_t) * IGB_NUM_TX_DESC;
	uint32_t ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	BUILD_BUG_ON(sizeof(igb_adv_tx_desc_t) != 16);
	uint64_t phys = mm_allocate_contiguous_pages(ring_pages);
	if (!phys) {
		kprintf("IGB: Failed to allocate TX ring\n");
		return -1;
	}
	dev->tx_descs = (igb_adv_tx_desc_t *)phys_to_virt(phys);
	dev->tx_descs_phys = phys;

	for (uint32_t i = 0; i < ring_bytes / 8; i++)
		((uint64_t *)dev->tx_descs)[i] = 0;

	for (int i = 0; i < IGB_NUM_TX_DESC; i++) {
		uint64_t bp = mm_allocate_physical_page();
		if (!bp) {
			kprintf("IGB: Failed to allocate TX buffer %d\n", i);
			return -1;
		}
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(bp);
		dev->tx_bufs_phys[i] = bp;
		dev->tx_stamp[i] = 0;
		dev->tx_len[i] = 0;
	}

	igb_write(dev, IGB_TDBAL0, (uint32_t)(phys & 0xFFFFFFFF));
	igb_write(dev, IGB_TDBAH0, (uint32_t)(phys >> 32));
	igb_write(dev, IGB_TDLEN0, ring_bytes);
	igb_write(dev, IGB_TDH0, 0);
	igb_write(dev, IGB_TDT0, 0);

	igb_write(dev, IGB_TIPG, 0x0060200A); // matches 8254x default

	// Enable TX queue
	uint32_t txdctl = igb_read(dev, IGB_TXDCTL0);
	txdctl |= IGB_QUEUE_ENABLE;
	igb_write(dev, IGB_TXDCTL0, txdctl);
	for (int i = 0; i < 1000; i++) {
		if (igb_read(dev, IGB_TXDCTL0) & IGB_QUEUE_ENABLE)
			break;
		igb_delay_us(100);
	}

	igb_write(dev, IGB_TCTL,
		  IGB_TCTL_EN | IGB_TCTL_PSP | (15u << IGB_TCTL_CT_SHIFT) |
			  (64u << IGB_TCTL_COLD_SHIFT) | IGB_TCTL_RTLC);

	dev->tx_tail = 0;
	dev->tx_clean = 0;
	return 0;
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================
#define IGB_TX_WAKE_THRESH 32
#define IGB_IMS_NAPI                                       \
	(IGB_ICR_RXT0 | IGB_ICR_TXDW | IGB_ICR_RXDMT0 | \
	 IGB_ICR_RXO)
#define IGB_IMS_ALL (IGB_IMS_NAPI | IGB_ICR_LSC)

static uint32_t igb_tx_unused(const igb_dev_t *dev)
{
	return nic_ring_unused(dev->tx_tail, dev->tx_clean, IGB_NUM_TX_DESC);
}

// Caller holds tx_lock.
static uint32_t igb_tx_clean_locked(igb_dev_t *dev)
{
	uint32_t n = 0;

	while (dev->tx_clean != dev->tx_tail) {
		volatile igb_adv_tx_desc_t *d = &dev->tx_descs[dev->tx_clean];

		if (!(d->olinfo_status & IGB_ADVTXD_STAT_DD))
			break;
		dev->itr_tx_packets++;
		dev->itr_tx_bytes += dev->tx_len[dev->tx_clean];
		d->cmd_type_len = 0;
		d->olinfo_status = 0;
		dev->tx_stamp[dev->tx_clean] = 0;
		dev->tx_clean = (uint16_t)((dev->tx_clean + 1) % IGB_NUM_TX_DESC);
		n++;
	}
	return n;
}

static void igb_tx_maybe_wake_locked(igb_dev_t *dev)
{
	if (netif_queue_stopped(&dev->net_dev) &&
	    igb_tx_unused(dev) >= IGB_TX_WAKE_THRESH)
		netif_wake_queue(&dev->net_dev);
}

static int igb_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;
	uint16_t i;

	BUG_ON(dev == NULL);
	if (len == 0 || len > IGB_RX_BUF_SIZE) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&dev->tx_lock, &f);
	if (igb_tx_unused(dev) == 0) {
		igb_tx_clean_locked(dev);
		if (igb_tx_unused(dev) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}
	i = dev->tx_tail;
	volatile igb_adv_tx_desc_t *d = &dev->tx_descs[i];

	mm_memcpy(dev->tx_bufs[i], skb->data, len);
	d->buffer_addr = dev->tx_bufs_phys[i];
	d->olinfo_status = len << IGB_ADVTXD_PAYLEN_SHIFT;
	d->cmd_type_len = IGB_ADVTXD_DTYP_DATA | IGB_ADVTXD_DCMD_DEXT |
			  IGB_ADVTXD_DCMD_IFCS | IGB_ADVTXD_DCMD_EOP |
			  IGB_ADVTXD_DCMD_RS | len;
	dev->tx_len[i] = (uint16_t)len;
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp[i] = now ? now : 1;
	}
	__asm__ volatile("mfence" ::: "memory");
	dev->tx_tail = (uint16_t)((i + 1) % IGB_NUM_TX_DESC);
	igb_write(dev, IGB_TDT0, dev->tx_tail);

	ndev->tx_packets++;
	ndev->tx_bytes += len;
	if (igb_tx_unused(dev) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

/* Drop what the transmit ring holds and restart it empty, with the queue
 * disabled meanwhile (link lost with frames pending, or a hang). */
static void igb_tx_ring_reset(igb_dev_t *dev)
{
	uint64_t f;
	uint32_t pending;

	spin_lock_irqsave(&dev->tx_lock, &f);
	igb_tx_clean_locked(dev);
	pending = (uint32_t)((dev->tx_tail + IGB_NUM_TX_DESC - dev->tx_clean) %
			     IGB_NUM_TX_DESC);
	if (!pending) {
		spin_unlock_irqrestore(&dev->tx_lock, f);
		return;
	}
	uint32_t txdctl = igb_read(dev, IGB_TXDCTL0);
	igb_write(dev, IGB_TXDCTL0, txdctl & ~IGB_QUEUE_ENABLE);
	for (int i = 0; i < 100; i++) {
		if (!(igb_read(dev, IGB_TXDCTL0) & IGB_QUEUE_ENABLE))
			break;
		igb_delay_us(10);
	}
	for (int i = 0; i < IGB_NUM_TX_DESC; i++) {
		dev->tx_descs[i].cmd_type_len = 0;
		dev->tx_descs[i].olinfo_status = 0;
		dev->tx_stamp[i] = 0;
	}
	__asm__ volatile("mfence" ::: "memory");
	dev->tx_tail = 0;
	dev->tx_clean = 0;
	igb_write(dev, IGB_TDH0, 0);
	igb_write(dev, IGB_TDT0, 0);
	igb_write(dev, IGB_TXDCTL0, txdctl | IGB_QUEUE_ENABLE);
	dev->net_dev.tx_errors += pending;
	igb_tx_maybe_wake_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
}

// ============================================================================
// PHY power (copper parts)
// ============================================================================
static int igb_mdic(igb_dev_t *dev, uint32_t cmd, uint16_t *val)
{
	igb_write(dev, IGB_MDIC, cmd);
	for (int i = 0; i < 200; i++) {
		uint32_t v;

		igb_delay_us(50);
		v = igb_read(dev, IGB_MDIC);
		if (v & IGB_MDIC_READY) {
			if (v & IGB_MDIC_ERROR)
				return -1;
			if (val)
				*val = (uint16_t)v;
			return 0;
		}
	}
	return -1;
}

/* A PHY left powered down (by firmware, or a previous owner) never brings
 * the link up: clear BMCR power-down -- only when it reads back set, so a
 * PHY that is up (and every emulation) is left alone. */
static void igb_power_up_phy(igb_dev_t *dev)
{
	uint16_t bmcr = 0;
	uint32_t rd = (0u << 16) | ((uint32_t)IGB_PHY_ADDR << 21) |
		      IGB_MDIC_OP_READ;

	if (igb_mdic(dev, rd, &bmcr) != 0 || bmcr == 0xFFFF ||
	    !(bmcr & IGB_MII_BMCR_PDOWN))
		return;
	(void)igb_mdic(dev,
		       (uint32_t)(bmcr & ~IGB_MII_BMCR_PDOWN) | (0u << 16) |
			       ((uint32_t)IGB_PHY_ADDR << 21) |
			       IGB_MDIC_OP_WRITE,
		       NULL);
	kprintf("IGB: PHY was powered down, powered up\n");
}

// ============================================================================
// Link, receive filter, statistics, watchdog
// ============================================================================
static uint32_t igb_status_speed(uint32_t status)
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

static void igb_link_state_poll(igb_dev_t *dev)
{
	uint32_t status = igb_read(dev, IGB_STATUS);
	int now_up = (status & IGB_STATUS_LU) ? 1 : 0;
	int was_up = dev->link_up;
	int pending = __atomic_exchange_n(&dev->link_change_pending, 0,
					  __ATOMIC_ACQ_REL);

	if (now_up == was_up && !pending)
		return;
	if (!now_up) {
		if (was_up) {
			dev->link_up = 0;
			dev->link_speed = 0;
			kprintf("IGB: Link DOWN\n");
			netif_carrier_off(&dev->net_dev);
			dhcp_invalidate(&dev->net_dev);
		}
		igb_tx_ring_reset(dev);
		return;
	}
	dev->link_speed = igb_status_speed(status);
	if (!was_up) {
		dev->link_up = 1;
		kprintf("IGB: Link UP (%u Mb/s, %s duplex)\n", dev->link_speed,
			(status & IGB_STATUS_FD) ? "full" : "half");
	}
	netif_carrier_on(&dev->net_dev);
}

static int igb_link_status(net_device_t *ndev)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;
	int up = (igb_read(dev, IGB_STATUS) & IGB_STATUS_LU) ? 1 : 0;

	if (up != dev->link_up) {
		__atomic_store_n(&dev->link_change_pending, 1,
				 __ATOMIC_RELEASE);
		net_housekeeping_kick();
	}
	return up;
}

static void igb_apply_rx_mode(igb_dev_t *dev)
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
		igb_write(dev, IGB_MTA + r * 4, mta[r]);
	uint32_t rctl = igb_read(dev, IGB_RCTL) & ~(IGB_RCTL_UPE | IGB_RCTL_MPE);
	if (flags & IFF_PROMISC)
		rctl |= IGB_RCTL_UPE | IGB_RCTL_MPE;
	else if (all || (flags & IFF_ALLMULTI))
		rctl |= IGB_RCTL_MPE;
	igb_write(dev, IGB_RCTL, rctl);
}

static void igb_set_rx_mode(net_device_t *ndev)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

// The 82576 keeps the 8254x statistics registers at the same offsets.
static void igb_update_stats(igb_dev_t *dev)
{
	dev->st_crc += igb_read(dev, 0x4000);
	dev->st_align += igb_read(dev, 0x4004);
	dev->st_rxerr += igb_read(dev, 0x400C);
	dev->st_missed += igb_read(dev, 0x4010);
	dev->st_ecol += igb_read(dev, 0x4018);
	dev->st_latecol += igb_read(dev, 0x4020);
	dev->st_colls += igb_read(dev, 0x4028);
	dev->st_tncrs += igb_read(dev, 0x4034);
	dev->st_rlen += igb_read(dev, 0x4040) + igb_read(dev, 0x40A4) +
			igb_read(dev, 0x40AC);
	dev->st_nobuf += igb_read(dev, 0x40A0);
}

static void igb_get_stats64(net_device_t *ndev, struct net_device_stats64 *s)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;

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

static void igb_check_tx_hang(igb_dev_t *dev)
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
		    !(igb_read(dev, IGB_STATUS) & IGB_STATUS_TXOFF))
			hang = 1;
	}
	spin_unlock_irqrestore(&dev->tx_lock, f);
	if (!hang)
		return;
	dev->st_tx_hangs++;
	if (reported < 5) {
		reported++;
		kprintf("IGB: transmit unit hang, restarting the transmit ring\n");
	}
	igb_tx_ring_reset(dev);
}

static void igb_tx_timeout(net_device_t *ndev)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;

	dev->st_tx_hangs++;
	igb_tx_ring_reset(dev);
}

static void igb_watchdog(net_device_t *ndev)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;
	uint64_t f;

	if (!dev || !g_igb_initialized)
		return;
	igb_link_state_poll(dev);
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL))
		igb_apply_rx_mode(dev);
	spin_lock_irqsave(&dev->tx_lock, &f);
	igb_tx_clean_locked(dev);
	igb_tx_maybe_wake_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	igb_check_tx_hang(dev);
	if (timer_ticks() - dev->stats_tick >= net_ms_to_ticks(2000)) {
		dev->stats_tick = timer_ticks();
		igb_update_stats(dev);
	}
}

// Quiesce the controller before ACPI S5: mask all interrupts (legacy +
// extended), halt the RX/TX queues, clear Wake-on-LAN, deassert
// CTRL_EXT.DRV_LOAD so the management firmware reclaims ownership, and
// finally drop PCI bus-master so any in-flight DMA is dropped by the
// root complex.
static void igb_shutdown(net_device_t *ndev)
{
	igb_dev_t *dev = (igb_dev_t *)ndev->driver_data;
	if (!dev || !dev->mmio_base)
		return;

	igb_write(dev, IGB_IMC, 0xFFFFFFFFu);
	igb_write(dev, IGB_EIMC, 0xFFFFFFFFu);
	(void)igb_read(dev, IGB_ICR);

	uint32_t rxdctl = igb_read(dev, IGB_RXDCTL0);
	igb_write(dev, IGB_RXDCTL0, rxdctl & ~IGB_QUEUE_ENABLE);
	uint32_t txdctl = igb_read(dev, IGB_TXDCTL0);
	igb_write(dev, IGB_TXDCTL0, txdctl & ~IGB_QUEUE_ENABLE);

	uint32_t rctl = igb_read(dev, IGB_RCTL);
	igb_write(dev, IGB_RCTL, rctl & ~IGB_RCTL_EN);
	uint32_t tctl = igb_read(dev, IGB_TCTL);
	igb_write(dev, IGB_TCTL, tctl & ~IGB_TCTL_EN);

	igb_write(dev, IGB_WUFC, 0);
	igb_write(dev, IGB_WUC, 0);

	uint32_t ctrl_ext = igb_read(dev, IGB_CTRL_EXT);
	igb_write(dev, IGB_CTRL_EXT, ctrl_ext & ~IGB_CTRL_EXT_DRV_LOAD);

	if (dev->pci_dev) {
		const pci_device_t *pdev = dev->pci_dev;
		uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device,
					      pdev->function, 0x04);
		cmd &= ~0x0004u;
		pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04,
				cmd);
	}
}

// ============================================================================
// Receive (NAPI poll)
// ============================================================================
static void igb_rx_deliver(igb_dev_t *dev, const uint8_t *buf, uint32_t len,
			   uint32_t staterr)
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
	cs = nic_e1000_rx_csum((uint8_t)staterr, (uint8_t)(staterr >> 24), 1,
			       1);
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

static int igb_rx_clean(igb_dev_t *dev, int budget)
{
	uint16_t ntc = (uint16_t)((dev->rx_tail + 1) % IGB_NUM_RX_DESC);
	uint16_t last = dev->rx_tail;
	int work = 0, refill = 0;

	while (work < budget) {
		volatile igb_rx_desc_t *d = &dev->rx_descs[ntc];
		uint32_t st = d->wb.status_error;

		if (!(st & IGB_RXD_STAT_DD))
			break;
		uint16_t len = d->wb.length;

		switch (nic_e1000_rx_verdict((uint8_t)st, (uint8_t)(st >> 24),
					     len, IGB_RX_BUF_SIZE,
					     &dev->rx_discarding)) {
		case NIC_RXV_DELIVER:
			igb_rx_deliver(dev, dev->rx_bufs[ntc], len, st);
			break;
		case NIC_RXV_ERROR:
			dev->net_dev.rx_errors++;
			break;
		default:
			dev->st_rx_frag++;
			break;
		}
		// Hand the descriptor back: rewrite the read-format fields,
		// which clears the DD/EOP status bits the chip wrote there.
		d->read.pkt_addr = dev->rx_bufs_phys[ntc];
		d->read.hdr_addr = 0;
		last = ntc;
		ntc = (uint16_t)((ntc + 1) % IGB_NUM_RX_DESC);
		work++;
		if (++refill >= 32) {
			__asm__ volatile("mfence" ::: "memory");
			dev->rx_tail = last;
			igb_write(dev, IGB_RDT0, last);
			refill = 0;
		}
	}
	if (refill) {
		__asm__ volatile("mfence" ::: "memory");
		dev->rx_tail = last;
		igb_write(dev, IGB_RDT0, last);
	}
	return work;
}

static void igb_update_itr(igb_dev_t *dev, uint32_t txp, uint32_t txb)
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
		igb_write_itr(dev, dev->itr_rate);
	}
}

static int igb_poll(struct napi_struct *n, int budget)
{
	igb_dev_t *dev = (igb_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	spin_lock_irqsave(&dev->tx_lock, &f);
	igb_tx_clean_locked(dev);
	igb_tx_maybe_wake_locked(dev);
	dev->itr_tx_acc_packets += dev->itr_tx_packets;
	dev->itr_tx_acc_bytes += dev->itr_tx_bytes;
	dev->itr_tx_packets = 0;
	dev->itr_tx_bytes = 0;
	spin_unlock_irqrestore(&dev->tx_lock, f);

	work = igb_rx_clean(dev, budget);
	if (work < budget) {
		igb_update_itr(dev, dev->itr_tx_acc_packets,
			       dev->itr_tx_acc_bytes);
		dev->itr_tx_acc_packets = 0;
		dev->itr_tx_acc_bytes = 0;
		if (napi_complete_done(n, work))
			igb_write(dev, IGB_IMS, IGB_IMS_NAPI);
	}
	return work;
}

// ============================================================================
// IRQ handler
// ============================================================================
/* Acknowledge the causes, mask receive/transmit and hand them to the NAPI
 * poll; link changes to the watchdog.  Returns 1 when the interrupt was
 * this device's, 0 otherwise (shared legacy line); the dispatcher
 * acknowledges the local APIC. */
int igb_irq_handler(void)
{
	if (!g_igb_initialized)
		return 0;

	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	igb_dev_t *dev = &g_igb;
	BUG_ON(dev == NULL);
	uint32_t icr = igb_read(dev, IGB_ICR);
	int handled;
	WARN_ON_ONCE(icr == 0xFFFFFFFF); /* ICR all-ones: MMIO bus error */
	if (icr == 0xFFFFFFFF)
		return 0;
	if (icr == 0 && !dev->msi_vector)
		return 0;
	/* An MSI is the device's alone; on a shared line only what it
	 * asserted or a cause it enabled (nic_e1000_irq_ours). */
	handled = dev->msi_vector ? 1 :
				    nic_e1000_irq_ours(icr,
						       igb_read(dev, IGB_IMS));

	if (icr & IGB_ICR_LSC) {
		dev->link_change_pending = 1;
		net_housekeeping_kick();
	}
	/* With MSI the message is ours whatever ICR still says: the poll
	 * looks at the rings. */
	if ((icr & (IGB_IMS_NAPI | IGB_ICR_RXQ0 | IGB_ICR_TXQ0)) ||
	    dev->msi_vector) {
		igb_write(dev, IGB_IMC, IGB_IMS_NAPI);
		napi_schedule(&dev->napi);
	}
	return handled;
}

static int igb_msi_handler(void *arg)
{
	(void)arg;
	(void)igb_irq_handler();
	return 1; // an MSI vector is the device's alone
}

// ============================================================================
// MMIO BAR mapping
// ============================================================================
static int igb_map_bar(igb_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];
	if (bar0 & 1) {
		kprintf("IGB: BAR0 is I/O port, not MMIO\n");
		return -1;
	}
	uint64_t phys = bar0 & 0xFFFFFFF0ULL;
	if ((bar0 & 0x06) == 0x04) {
		phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
	}

	dev->mmio_phys = phys;
	dev->mmio_size = 128 * 1024; // 82576 BAR0 is 128 KiB

	// strictly uncached, also inside the write-back direct map
	uint32_t num_pages = (dev->mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t virt_base = mm_map_device_mmio(phys, num_pages);
	if (!virt_base) {
		kprintf("IGB: Failed to map BAR0 MMIO\n");
		return -1;
	}
	dev->mmio_base = (volatile uint8_t *)virt_base;
	return 0;
}

// ============================================================================
// Driver entry point
// ============================================================================
void igb_init(void)
{
	might_sleep();
	BUILD_BUG_ON(sizeof(igb_rx_desc_t) != 16);
	BUILD_BUG_ON(sizeof(igb_tx_desc_t) != 16);
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		if (devs[i].vendor_id != IGB_VENDOR_ID)
			continue;
		const igb_id_t *match = igb_lookup(devs[i].device_id);
		if (!match)
			continue;

		kprintf("IGB: Found %s (PCI %02x:%02x.%x)\n", match->name,
			devs[i].bus, devs[i].device, devs[i].function);

		igb_dev_t *dev = &g_igb;
		dev->pci_dev = &devs[i];
		dev->device_id = devs[i].device_id;

		pci_enable_busmaster_mem(dev->pci_dev);

		if (igb_map_bar(dev) < 0) {
			kprintf("IGB: Failed to map BAR0\n");
			return;
		}

		igb_reset(dev);

		igb_read_mac(dev);
		kprintf("IGB: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		// Set link up; preserve UEFI speed/duplex
		uint32_t ctrl = igb_read(dev, IGB_CTRL);
		ctrl |= IGB_CTRL_SLU;
		ctrl &= ~IGB_CTRL_PHY_RST;
		igb_write(dev, IGB_CTRL, ctrl);

		// A copper PHY left powered down would never link
		igb_power_up_phy(dev);

		// Tell the management firmware that a host driver is now active
		uint32_t ctrl_ext = igb_read(dev, IGB_CTRL_EXT);
		igb_write(dev, IGB_CTRL_EXT, ctrl_ext | IGB_CTRL_EXT_DRV_LOAD);

		// Clear multicast table (128 entries)
		for (int m = 0; m < 128; m++)
			igb_write(dev, IGB_MTA + m * 4, 0);

		// Re-program receive address valid bit
		uint32_t ral = (uint32_t)dev->mac_addr[0] |
			       ((uint32_t)dev->mac_addr[1] << 8) |
			       ((uint32_t)dev->mac_addr[2] << 16) |
			       ((uint32_t)dev->mac_addr[3] << 24);
		uint32_t rah = (uint32_t)dev->mac_addr[4] |
			       ((uint32_t)dev->mac_addr[5] << 8) | (1u << 31);
		igb_write(dev, IGB_RAL0, ral);
		igb_write(dev, IGB_RAH0, rah);

		if (igb_init_rx(dev) < 0)
			return;
		if (igb_init_tx(dev) < 0)
			return;

		/* MSI when the device offers it (the vector is the device's
		 * alone), the legacy line with the ACPI _PRT lookup
		 * otherwise. */
		{
			int vec = 0;

			if (irq_request_msi(dev->pci_dev, 0, igb_msi_handler,
					    dev, "igb", &vec) == 0) {
				dev->msi_vector = vec;
				kprintf("IGB: MSI enabled (vector %d)\n", vec);
			}
		}
		if (!dev->msi_vector) {
			// Resolve legacy IRQ via ACPI _PRT (preferred) or PCI line
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
					kprintf("IGB: ACPI _PRT resolved INT%c -> GSI %u\n",
						'A' + acpi_pin, gsi);
				}
			}
			if (irq == 0xFF) {
				irq = dev->pci_dev->interrupt_line;
				if (irq != 0xFF && irq <= 23) {
					kprintf("IGB: ACPI _PRT lookup failed, falling back to "
						"PCI interrupt_line = %d\n",
						irq);
				} else {
					kprintf("IGB: WARNING: no valid IRQ\n");
				}
			}
			kprintf("IGB: Using legacy IRQ %d\n", irq);
			g_igb_legacy_irq = irq;

			// Clear PCI Command INTx Disable if firmware left it set
			{
				uint32_t cmd = pci_cfg_read32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04);
				if (cmd & PCI_CMD_INTX_DISABLE) {
					cmd &= ~PCI_CMD_INTX_DISABLE;
					pci_cfg_write32(
						dev->pci_dev->bus, dev->pci_dev->device,
						dev->pci_dev->function, 0x04, cmd);
					kprintf("IGB: cleared PCI Command INTx Disable bit\n");
				}
			}

			if (irq <= 23) {
				uint8_t vector = 32 + irq;
				ioapic_configure_legacy_irq(irq, vector,
							    IOAPIC_POLARITY_LOW,
							    IOAPIC_TRIGGER_LEVEL);
			}

		}

		// Initialise net_device fields BEFORE enabling chip-side IRQs
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("igb");
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("igb_tx");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, igb_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_igb_initialized = 1;

		// Wait up to 5 s for the link to come up
		uint32_t status = igb_read(dev, IGB_STATUS);
		if (!(status & IGB_STATUS_LU)) {
			uint32_t t0 = timer_pmtimer_read_raw();
			while (timer_pmtimer_delta_us(
				       t0, timer_pmtimer_read_raw()) <
			       5000000) {
				igb_delay_us(50000);
				status = igb_read(dev, IGB_STATUS);
				if (status & IGB_STATUS_LU)
					break;
			}
		}
		dev->link_up = (status & IGB_STATUS_LU) ? 1 : 0;
		if (dev->link_up) {
			dev->link_speed = igb_status_speed(status);
			kprintf("IGB: Link UP (%u Mb/s, %s duplex)\n",
				dev->link_speed,
				(status & IGB_STATUS_FD) ? "full" : "half");
		} else {
			kprintf("IGB: Link DOWN\n");
		}

		dev->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			dev->net_dev.mac_addr[m] = dev->mac_addr[m];
		dev->net_dev.mtu = NET_MTU_DEFAULT;
		dev->net_dev.ip_addr = 0;
		dev->net_dev.netmask = 0;
		dev->net_dev.gateway = 0;
		dev->net_dev.dns_server = 0;
		dev->net_dev.send = NULL;
		dev->net_dev.xmit = igb_xmit;
		dev->net_dev.link_status = igb_link_status;
		dev->net_dev.shutdown = igb_shutdown;
		dev->net_dev.watchdog = igb_watchdog;
		dev->net_dev.tx_timeout = igb_tx_timeout;
		dev->net_dev.set_rx_mode = igb_set_rx_mode;
		dev->net_dev.get_stats64 = igb_get_stats64;
		dev->net_dev.features |= NETIF_F_RXCSUM;
		dev->stats_tick = timer_ticks();

		net_register(&dev->net_dev);
		if (!dev->link_up)
			netif_carrier_off(&dev->net_dev);
		igb_write(dev, IGB_IMS, IGB_IMS_ALL);

		kprintf("IGB: Driver initialized successfully\n");
		return;
	}
}
