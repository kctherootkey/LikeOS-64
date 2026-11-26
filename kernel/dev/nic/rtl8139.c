// LikeOS Realtek RTL8139 NIC Driver
//
// The RTL8139 is a venerable 10/100 Mbit/s PCI Ethernet controller. It is
// supported by QEMU (`-device rtl8139`) and most physical RTL8139C /
// RTL8139C+ cards.  It uses I/O-port (PMIO) registers (BAR0) and a single
// linear ring buffer for RX with four 2 KiB transmit slots.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/rtl8139.h>
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
// Supported PCI device IDs.  All entries expose the same
// RTL8139-compatible register layout (RTL8139C/C+/D, plus numerous
// OEM rebrands).
// ============================================================================
typedef struct {
	uint16_t vid, did;
	const char *name;
} rtl8139_id_t;

static const rtl8139_id_t rtl8139_pci_ids[] = {
	{ 0x10EC, 0x8139, "Realtek RTL8139" }, // QEMU -device rtl8139
	{ 0x10EC, 0x8138, "Realtek RTL8139B mPCI" },
	{ 0x1113, 0x1211, "SMC EZ Card 1211TX" },
	{ 0x1500, 0x1360, "Delta 8139" },
	{ 0x4033, 0x1360, "Addtron 8139" },
	{ 0x1186, 0x1300, "D-Link DFE-538TX" },
	{ 0x1186, 0x1340, "D-Link DFE-690TXD" },
	{ 0x13D1, 0xAB06, "AbocOM FE2500" },
	{ 0x1259, 0xA117, "AT-2550FX" },
	{ 0x1259, 0xA11E, "AT-2550FT" },
	{ 0x14EA, 0xAB06, "Planex FNW-3603-TX" },
	{ 0x14EA, 0xAB07, "Planex FNW-3800-TX" },
	{ 0x11DB, 0x1234, "Sega Dreamcast BBA" },
	{ 0x1432, 0x9130, "Edimax RTL8139" },
	{ 0x02AC, 0x1012, "SpeedStream" },
	{ 0x018A, 0x0106, "LevelOne FPC-0106TX" },
	{ 0x126C, 0x1211, "Nortel BayStack 21" },
	{ 0x1743, 0x8139, "Peppercon ROL/F-100" },
	{ 0x021B, 0x8139, "Hawking PN672TX" },
	{ 0x16EC, 0xAB06, "USRobotics USR997902" },
	{ 0, 0, NULL },
};

static const rtl8139_id_t *rtl8139_lookup(uint16_t vid, uint16_t did)
{
	for (const rtl8139_id_t *e = rtl8139_pci_ids; e->name; e++) {
		if (e->vid == vid && e->did == did)
			return e;
	}
	return NULL;
}

// Global single-NIC state
static rtl8139_dev_t g_rtl8139;
int g_rtl8139_initialized = 0;
int g_rtl8139_legacy_irq = -1;

// ============================================================================
// I/O helpers (RTL8139 uses PMIO BAR0)
// ============================================================================
static inline void rtl_write8(rtl8139_dev_t *dev, uint16_t reg, uint8_t v)
{
	outb(dev->io_base + reg, v);
}
static inline void rtl_write16(rtl8139_dev_t *dev, uint16_t reg, uint16_t v)
{
	outw(dev->io_base + reg, v);
}
static inline void rtl_write32(rtl8139_dev_t *dev, uint16_t reg, uint32_t v)
{
	outl(dev->io_base + reg, v);
}
static inline uint8_t rtl_read8(rtl8139_dev_t *dev, uint16_t reg)
{
	return inb(dev->io_base + reg);
}
static inline uint16_t rtl_read16(rtl8139_dev_t *dev, uint16_t reg)
{
	return inw(dev->io_base + reg);
}
static inline uint32_t rtl_read32(rtl8139_dev_t *dev, uint16_t reg)
{
	return inl(dev->io_base + reg);
}

// ============================================================================
// PM Timer microsecond delay (mirrors e1000)
// ============================================================================
static void rtl_delay_us(uint32_t us)
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
// Read MAC from IDR0..IDR5
// ============================================================================
static void rtl_read_mac(rtl8139_dev_t *dev)
{
	for (int i = 0; i < 6; i++)
		dev->mac_addr[i] = rtl_read8(dev, RTL_IDR0 + i);
}

// ============================================================================
// Soft reset
// ============================================================================
static int rtl_reset(rtl8139_dev_t *dev)
{
	// Power on the device through CONFIG1 (LWAKE=0, LWPTN=0).  Required
	// on a cold-booted real card; harmless on QEMU.
	rtl_write8(dev, RTL_CONFIG1, 0x00);

	// Issue software reset
	rtl_write8(dev, RTL_CR, RTL_CR_RST);

	// Wait for RST bit to auto-clear (max ~10 ms)
	for (int i = 0; i < 1000; i++) {
		if (!(rtl_read8(dev, RTL_CR) & RTL_CR_RST))
			return 0;
		rtl_delay_us(10);
	}
	kprintf("RTL8139: reset timed out\n");
	return -1;
}

// ============================================================================
// RX init — allocate the linear ring buffer, point RBSTART at it
// ============================================================================
static int rtl_init_rx(rtl8139_dev_t *dev)
{
	uint32_t pages = (RTL8139_RX_BUF_TOTAL + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys) {
		kprintf("RTL8139: failed to allocate RX buffer\n");
		return -1;
	}

	// RTL8139 only DMAs to addresses below 4 GiB.  Our identity-mapped
	// physical allocator should normally satisfy this on small systems,
	// but warn loudly if we got a high address — the hardware will then
	// silently corrupt memory.
	if (phys >> 32) {
		kprintf("RTL8139: ERROR: RX buffer phys 0x%lx is above 4GB\n",
			phys);
		return -1;
	}

	dev->rx_buf = (uint8_t *)phys_to_virt(phys);
	dev->rx_buf_phys = phys;
	dev->rx_offset = 0;

	// Zero the buffer (rep stos — was a byte loop).
	mm_memset(dev->rx_buf, 0, RTL8139_RX_BUF_TOTAL);

	// Point hardware at it
	rtl_write32(dev, RTL_RBSTART, (uint32_t)phys);
	return 0;
}

// ============================================================================
// TX init — allocate four 2 KiB slot buffers
// ============================================================================
static int rtl_init_tx(rtl8139_dev_t *dev)
{
	for (int i = 0; i < RTL8139_NUM_TX_DESC; i++) {
		uint64_t phys = mm_allocate_physical_page();
		if (!phys) {
			kprintf("RTL8139: failed to allocate TX buffer %d\n",
				i);
			return -1;
		}
		if (phys >> 32) {
			kprintf("RTL8139: ERROR: TX buffer phys 0x%lx above 4GB\n",
				phys);
			return -1;
		}
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(phys);
		dev->tx_bufs_phys[i] = phys;
	}
	return 0;
}

// Interrupt sources the driver handles, and those it keeps while the
// NAPI poll runs (receive and transmit completion are the poll's).
#define RTL_INTR_MASK                                                  \
	(RTL_INT_SERR | RTL_INT_TIMEOUT | RTL_INT_PUN | RTL_INT_RXOVW | \
	 RTL_INT_FOVW | RTL_INT_TER | RTL_INT_TOK | RTL_INT_RER |      \
	 RTL_INT_ROK)
#define RTL_INTR_NAPI                                                  \
	(RTL_INT_RXOVW | RTL_INT_FOVW | RTL_INT_TER | RTL_INT_TOK |    \
	 RTL_INT_RER | RTL_INT_ROK)
// Early TX threshold to start with: 256 bytes (8 x 32)
#define RTL_TX_FLAG_START (8u << RTL_TSD_ERTXTH_SHIFT)

/* The receive filter: broadcast and our address always, multicast through
 * the 64-bit hash in MAR0..7, everything with IFF_PROMISC.  CPU 0
 * (watchdog / bring-up). */
static void rtl_set_rx_filter(rtl8139_dev_t *dev)
{
	uint32_t mar[2] = { 0, 0 };
	uint32_t mode = RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AM;
	uint32_t flags = 0;
	uint8_t mc[NETDEV_MC_MAX][6];
	int all = 0, n = 0;

	if (dev->net_dev.core_ready) {
		n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
		flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);
	}
	if (flags & IFF_PROMISC) {
		mode |= RTL_RCR_AAP;
		mar[0] = mar[1] = 0xFFFFFFFFu;
	} else if (all || (flags & IFF_ALLMULTI)) {
		mar[0] = mar[1] = 0xFFFFFFFFu;
	} else {
		for (int i = 0; i < n; i++) {
			uint32_t bit = nic_mc_hash64_be(mc[i]);

			mar[bit >> 5] |= 1u << (bit & 31);
		}
	}
	dev->rx_config = RTL_RCR_WRAP | RTL_RCR_RBLEN_32K |
			 RTL_RCR_MXDMA_UNLIM | mode;
	rtl_write32(dev, RTL_RCR, dev->rx_config);
	rtl_write32(dev, RTL_MAR0, mar[0]);
	rtl_write32(dev, RTL_MAR0 + 4, mar[1]);
}

/* Program the reset chip: ring addresses, receive and transmit config,
 * the filter, the read pointer, the interrupt mask, RX/TX on.  Both rings
 * start empty.  Bring-up and the TX-timeout restart (watchdog, tx_lock
 * held by the caller there). */
static void rtl_hw_start(rtl8139_dev_t *dev)
{
	rtl_write32(dev, RTL_RBSTART, (uint32_t)dev->rx_buf_phys);
	for (int i = 0; i < RTL8139_NUM_TX_DESC; i++) {
		rtl_write32(dev, RTL_TSAD0 + i * 4,
			    (uint32_t)dev->tx_bufs_phys[i]);
		dev->tx_stamp[i] = 0;
	}
	dev->tx_cur = dev->tx_dirty = 0;
	dev->tx_flag = RTL_TX_FLAG_START;
	dev->rx_offset = 0;
	dev->rx_early_tick = 0;

	// RX and TX on before their configuration (the chip ignores it
	// otherwise), as the datasheet's sequence has it
	rtl_write8(dev, RTL_CR, RTL_CR_TE | RTL_CR_RE);
	// 32K ring with WRAP (a frame past the end goes on into the pad --
	// the chip ignores WRAP at 64K, see rtl8139.h), unlimited DMA burst
	rtl_set_rx_filter(dev);
	// TX: max DMA burst 1024 B, normal IFG
	rtl_write32(dev, RTL_TCR, RTL_TCR_MXDMA_1024 | RTL_TCR_IFG_NORMAL);
	rtl_write32(dev, RTL_MPC, 0); // missed packet counter
	rtl_write16(dev, RTL_CAPR, 0xFFF0); // -16: nothing read yet
	rtl_write16(dev, RTL_ISR, 0xFFFF);
	rtl_write16(dev, RTL_IMR, RTL_INTR_MASK);
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================

/* Complete the frames the chip is done with (TOK, underrun, abort), oldest
 * first.  An underrun raises the early TX threshold by 64 bytes; an abort
 * is cleared in TCR.  Caller holds tx_lock. */
static void rtl_tx_clean_locked(rtl8139_dev_t *dev)
{
	uint32_t dirty = dev->tx_dirty;

	while (dirty != dev->tx_cur) {
		uint32_t entry = dirty % RTL8139_NUM_TX_DESC;
		uint32_t tsd = rtl_read32(dev, RTL_TSD0 + entry * 4);

		if (!(tsd & (RTL_TSD_TOK | RTL_TSD_TUN | RTL_TSD_TABT)))
			break; // still being sent
		if (tsd & (RTL_TSD_OWC | RTL_TSD_TABT)) {
			dev->net_dev.tx_errors++;
			if (tsd & RTL_TSD_TABT) {
				dev->st_tx_aborted++;
				rtl_write32(dev, RTL_TCR,
					    RTL_TCR_MXDMA_1024 |
						    RTL_TCR_IFG_NORMAL |
						    RTL_TCR_CLRABT);
				rtl_write16(dev, RTL_ISR, RTL_INT_TER);
			}
			if (tsd & RTL_TSD_CRS)
				dev->st_tx_carrier++;
			if (tsd & RTL_TSD_OWC)
				dev->st_tx_window++;
		} else {
			if ((tsd & RTL_TSD_TUN) && dev->tx_flag < 0x00300000u) {
				dev->tx_flag += 2u << RTL_TSD_ERTXTH_SHIFT;
				dev->st_tx_fifo++;
			}
			dev->st_colls += (tsd >> RTL_TSD_NCC_SHIFT) & 15;
		}
		dev->tx_stamp[entry] = 0;
		dirty++;
	}
	if (dirty != dev->tx_dirty) {
		dev->tx_dirty = dirty;
		if (netif_queue_stopped(&dev->net_dev))
			netif_wake_queue(&dev->net_dev);
	}
}

static int rtl8139_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;

	BUG_ON(dev == NULL);
	if (len == 0 || len > 1792) { // TSD size field: at most 1792 bytes
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&dev->tx_lock, &f);
	if (dev->tx_cur - dev->tx_dirty >= RTL8139_NUM_TX_DESC) {
		rtl_tx_clean_locked(dev);
		if (dev->tx_cur - dev->tx_dirty >= RTL8139_NUM_TX_DESC) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}

	uint32_t entry = dev->tx_cur % RTL8139_NUM_TX_DESC;

	mm_memcpy(dev->tx_bufs[entry], skb->data, len);
	/* Hardware does not pad short frames: zero-pad to the 60-byte
	 * (sans-FCS) minimum. */
	if (len < 60)
		mm_memset(dev->tx_bufs[entry] + len, 0, 60 - len);
	uint32_t tx_len = (len < 60) ? 60 : len;
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp[entry] = now ? now : 1;
	}

	// The buffer is visible to the device's DMA before the start.
	__asm__ volatile("mfence" ::: "memory");

	// Writing the size (OWN clear) with the early TX threshold starts
	// the transmission.
	rtl_write32(dev, RTL_TSD0 + entry * 4,
		    dev->tx_flag | (tx_len & RTL_TSD_SIZE_MASK));
	dev->tx_cur++;

	ndev->tx_packets++;
	ndev->tx_bytes += tx_len;
	if (dev->tx_cur - dev->tx_dirty >= RTL8139_NUM_TX_DESC)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

// ============================================================================
// Link, receive filter, statistics, watchdog
// ============================================================================
static int rtl_msr_link(rtl8139_dev_t *dev)
{
	// MSR.LINKB is INVERSE: 1 means link DOWN.
	return (rtl_read8(dev, RTL_MSR) & RTL_MSR_LINKB) ? 0 : 1;
}

/* The carrier follows MSR once it has reported a link at all. */
static void rtl_link_check(rtl8139_dev_t *dev)
{
	int now_up = rtl_msr_link(dev);

	if (now_up)
		dev->link_seen = 1;
	if (now_up == dev->link_up)
		return;
	dev->link_up = now_up;
	kprintf("RTL8139: Link %s\n", now_up ? "UP" : "DOWN");
	if (now_up) {
		netif_carrier_on(&dev->net_dev);
	} else {
		dhcp_invalidate(&dev->net_dev);
		if (dev->link_seen)
			netif_carrier_off(&dev->net_dev);
	}
}

static int rtl8139_link_status(net_device_t *ndev)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;
	int up = rtl_msr_link(dev);

	if (up != dev->link_up)
		net_housekeeping_kick();
	return up;
}

static void rtl8139_set_rx_mode(net_device_t *ndev)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

static void rtl8139_get_stats64(net_device_t *ndev,
				struct net_device_stats64 *s)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;

	s->tx_aborted_errors = dev->st_tx_aborted;
	s->tx_carrier_errors = dev->st_tx_carrier;
	s->tx_fifo_errors = dev->st_tx_fifo;
	s->collisions = dev->st_colls;
	s->rx_crc_errors = dev->st_rx_crc;
	s->rx_frame_errors = dev->st_rx_frame;
	s->rx_length_errors = dev->st_rx_length;
	s->rx_fifo_errors = dev->st_rx_fifo;
	s->rx_missed_errors = dev->st_missed;
}

static void rtl8139_tx_timeout(net_device_t *ndev)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
}

/* Restart after a transmit timeout: the chip is reset (its descriptor
 * order starts again at 0) and programmed anew; queued frames are
 * dropped.  Watchdog context (CPU 0): the poll is not running. */
static void rtl_restart(rtl8139_dev_t *dev)
{
	uint64_t f;

	spin_lock_irqsave(&dev->tx_lock, &f);
	rtl_write16(dev, RTL_IMR, 0);
	dev->net_dev.tx_errors += dev->tx_cur - dev->tx_dirty;
	rtl_write8(dev, RTL_CR, RTL_CR_RST);
	for (int i = 0; i < 1000; i++) {
		if (!(rtl_read8(dev, RTL_CR) & RTL_CR_RST))
			break;
		rtl_delay_us(10);
	}
	rtl_hw_start(dev);
	if (netif_queue_stopped(&dev->net_dev))
		netif_wake_queue(&dev->net_dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	napi_schedule(&dev->napi);
}

// Network housekeeping softirq, CPU 0, ~10 Hz and when kicked.
static void rtl8139_watchdog(net_device_t *ndev)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;
	uint64_t f, now = timer_ticks();
	int hang = 0;

	if (!dev || !g_rtl8139_initialized)
		return;
	rtl_link_check(dev);
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL))
		rtl_set_rx_filter(dev);
	spin_lock_irqsave(&dev->tx_lock, &f);
	rtl_tx_clean_locked(dev);
	if (dev->tx_cur != dev->tx_dirty) {
		uint64_t stamp =
			dev->tx_stamp[dev->tx_dirty % RTL8139_NUM_TX_DESC];

		if (stamp && now - stamp > net_ms_to_ticks(5000))
			hang = 1;
	}
	spin_unlock_irqrestore(&dev->tx_lock, f);
	dev->st_missed += rtl_read32(dev, RTL_MPC);
	rtl_write32(dev, RTL_MPC, 0);
	if (hang) {
		kprintf("RTL8139: transmit timed out, restarting the chip\n");
		__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
	}
	if (__atomic_exchange_n(&dev->reset_pending, 0, __ATOMIC_ACQ_REL))
		rtl_restart(dev);
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the
// RTL8139 datasheet, leaving CR.RE/CR.TE asserted, IMR populated, or
// CONFIG1.PMEn / CONFIG3.LWAKE / CONFIG5 wake bits set can hold PME#
// active and prevent a clean platform power-down.  Mask interrupts,
// clear all wake bits (CONFIG3/CONFIG5 are write-protected unless
// 9346CR is in EEPROM-config mode), halt RX/TX, and drop bus-master.
static void rtl8139_shutdown(net_device_t *ndev)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)ndev->driver_data;
	if (!dev || !dev->io_base)
		return;

	// Mask all interrupts and ack any pending ISR bits.
	rtl_write16(dev, RTL_IMR, 0x0000);
	rtl_write16(dev, RTL_ISR, 0xFFFF);

	// Unlock CONFIG registers (9346CR @ 0x50: 0xC0 = config-write mode).
	rtl_write8(dev, 0x50, 0xC0);
	rtl_write8(dev, RTL_CONFIG1, 0x00); // PMEn=0, LWACT=0
	rtl_write8(dev, RTL_CONFIG3, 0x00); // Magic/LinkUp/etc wake = 0
	rtl_write8(dev, RTL_CONFIG4, 0x00); // LWPTN/LWPME = 0
	rtl_write8(dev, 0x50, 0x00); // Re-lock CONFIG

	// Halt RX and TX engines.
	rtl_write8(dev, RTL_CR, 0x00);

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

/* Lost in the ring (a bad header): receiver off, read position back to the
 * start of the ring, filter and read pointer reprogrammed, receiver on.
 * Frames in the ring are dropped.  Poll context (CPU 0). */
static void rtl_rx_reset(rtl8139_dev_t *dev)
{
	dev->st_rx_resets++;
	rtl_write8(dev, RTL_CR, RTL_CR_TE); // receiver off
	rtl_write32(dev, RTL_RBSTART, (uint32_t)dev->rx_buf_phys);
	dev->rx_offset = 0;
	dev->rx_early_tick = 0;
	rtl_write8(dev, RTL_CR, RTL_CR_TE | RTL_CR_RE);
	rtl_write32(dev, RTL_RCR, dev->rx_config);
	rtl_write16(dev, RTL_CAPR, 0xFFF0);
}

static int rtl_rx_clean(rtl8139_dev_t *dev, int budget)
{
	net_device_t *nd = &dev->net_dev;
	int work = 0;

	while (work < budget && !(rtl_read8(dev, RTL_CR) & RTL_CR_BUFE)) {
		// The chip DMAs the header and frame into rx_buf behind the
		// compiler's back, and inb carries no memory clobber: keep the
		// reads below after the BUFE check that says they are there.
		__asm__ volatile("" ::: "memory");
		// Each packet in the ring is laid out as:
		//   [0..1]  status (16-bit, little-endian)
		//   [2..3]  length INCLUDING 4-byte CRC (16-bit)
		//   [4..]   packet data (length-4 bytes)
		volatile uint8_t *p = dev->rx_buf + dev->rx_offset;
		uint16_t status = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
		uint16_t length = (uint16_t)p[2] | ((uint16_t)p[3] << 8);

		/* The chip is still copying this frame (early receive):
		 * its length reads 0xFFF0 until it is complete.  Not ready
		 * -- unless it stays so for two ticks, then it is garbage. */
		if (length == NIC_RTL_RX_EARLY_LEN) {
			uint64_t now = timer_ticks();

			if (!dev->rx_early_tick) {
				dev->rx_early_tick = now ? now : 1;
				break;
			}
			if (now - dev->rx_early_tick <= 2)
				break;
			length = 0; // stuck: treat as lost in the ring
		}
		dev->rx_early_tick = 0;

		if (!(status & RTL_RXS_ROK) || length < 8 ||
		    length > RTL_MAX_RX_SIZE) {
			nd->rx_errors++;
			if (status & RTL_RXS_CRC)
				dev->st_rx_crc++;
			if (status & RTL_RXS_FAE)
				dev->st_rx_frame++;
			if (status & (RTL_RXS_LONG | RTL_RXS_RUNT))
				dev->st_rx_length++;
			rtl_rx_reset(dev);
			work++;
			break;
		}

		// Deliver to upper layers (length includes 4-byte CRC; strip it)
		uint32_t payload = length - 4u;
		sk_buff_t *skb = skb_alloc(payload);

		if (skb) {
			skb->dev = nd;
			mm_memcpy(skb_append(skb, payload), (const void *)(p + 4),
				  payload);
			nd->rx_packets++;
			nd->rx_bytes += payload;
			if (p[4] & 1)
				nd->multicast++;
			napi_gro_receive(&dev->napi, skb);
		} else {
			nd->rx_dropped++;
		}

		// Advance offset, 4-byte aligned, accounting for 4-byte header.
		// The ring wraps at RTL8139_RX_RING_LEN (a frame that ran past
		// it was written on into the padding, see rtl8139.h).  The read
		// pointer goes to the chip at once: it decides BUFE (and frees
		// ring space) from it.
		dev->rx_offset = (uint16_t)nic_rtl8139_rx_next(
			dev->rx_offset, length, RTL8139_RX_RING_LEN);
		rtl_write16(dev, RTL_CAPR,
			    nic_rtl8139_capr(dev->rx_offset));
		work++;
	}
	return work;
}

static int rtl8139_poll(struct napi_struct *n, int budget)
{
	rtl8139_dev_t *dev = (rtl8139_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	spin_lock_irqsave(&dev->tx_lock, &f);
	rtl_tx_clean_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	work = rtl_rx_clean(dev, budget);
	if (dev->rx_early_tick)
		return budget; // a frame is being copied: look again soon
	if (work < budget && napi_complete_done(n, work))
		rtl_write16(dev, RTL_IMR, RTL_INTR_MASK);
	return work;
}

// ============================================================================
// Top-half IRQ handler (called from kernel/ke/interrupt.c dispatcher)
// ============================================================================
/* Returns 1 when the interrupt was this chip's, 0 otherwise (shared line);
 * the dispatcher acknowledges the local APIC.  Receive and transmit work
 * goes to the NAPI poll with those sources masked until it is done. */
int rtl8139_irq_handler(void)
{
	if (!g_rtl8139_initialized)
		return 0;

	// Feed entropy from NIC IRQ timing
	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	rtl8139_dev_t *dev = &g_rtl8139;
	BUG_ON(dev == NULL);
	uint16_t isr = rtl_read16(dev, RTL_ISR);
	int handled;
	if (isr == 0 || isr == 0xFFFF)
		return 0;
	// ours only for a cause it has enabled (nic_rtl8139_irq_ours)
	handled = nic_rtl8139_irq_ours(isr, rtl_read16(dev, RTL_IMR));

	// ACK by writing 1s to the bits we handled (write-1-to-clear).
	rtl_write16(dev, RTL_ISR, isr);

	if (isr & RTL_INT_PUN) // packet underrun / link change
		net_housekeeping_kick();
	if (isr & (RTL_INT_RXOVW | RTL_INT_FOVW)) {
		/* The ring or the FIFO overflowed: frames were lost; the
		 * poll empties the ring and the chip goes on. */
		dev->net_dev.rx_errors++;
		dev->st_rx_fifo++;
	}
	if (isr & RTL_INT_SERR)
		dev->net_dev.rx_errors++;
	/* Receive/transmit work also when the interrupt was another
	 * device's: the cause is acknowledged above, and a poll about to
	 * re-enable its interrupts would not hear of it otherwise. */
	if (isr & RTL_INTR_NAPI) {
		rtl_write16(dev, RTL_IMR, RTL_INTR_MASK & ~RTL_INTR_NAPI);
		napi_schedule(&dev->napi);
	}
	return handled;
}

// ============================================================================
// Resolve I/O BAR (RTL8139's BAR0 is PMIO)
// ============================================================================
static int rtl_resolve_iobase(rtl8139_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];
	if (!(bar0 & 1)) {
		// The chip also exposes MMIO via BAR1; fall back to it if BAR0 is
		// not an I/O BAR (shouldn't normally happen).
		kprintf("RTL8139: BAR0 is not I/O space (bar0=0x%x)\n", bar0);
		return -1;
	}
	dev->io_base = (uint16_t)(bar0 & 0xFFFC);
	return 0;
}

// ============================================================================
// Driver entry point — probe PCI bus and bring up the first RTL8139 found
// ============================================================================
void rtl8139_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		const rtl8139_id_t *match =
			rtl8139_lookup(devs[i].vendor_id, devs[i].device_id);
		if (!match)
			continue;

		kprintf("RTL8139: Found %s (PCI %02x:%02x.%x)\n", match->name,
			devs[i].bus, devs[i].device, devs[i].function);

		rtl8139_dev_t *dev = &g_rtl8139;
		dev->pci_dev = &devs[i];
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("rtl8139_tx");

		pci_enable_busmaster_mem(dev->pci_dev);

		// pci_enable_busmaster_mem() only sets MEM (bit 1) + Bus Master (bit 2).
		// RTL8139's BAR0 is I/O space, so we must also set I/O Space Enable
		// (bit 0) — without it, inb/inw/inl from io_base return 0xFF.
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

		if (rtl_resolve_iobase(dev) < 0)
			return;
		kprintf("RTL8139: I/O base 0x%x\n", dev->io_base);

		if (rtl_reset(dev) < 0)
			return;

		rtl_read_mac(dev);
		kprintf("RTL8139: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		if (rtl_init_rx(dev) < 0)
			return;
		if (rtl_init_tx(dev) < 0)
			return;

		// RTL8139 has no MSI capability in the real silicon nor in QEMU's
		// emulation, but we still try — pci_enable_msi() will fail
		// gracefully and we'll fall through to the legacy INTx path that
		// mirrors the e1000 driver.
		dev->msi_vector = 0;
		int msi_ret =
			-1; // pci_enable_msi(dev->pci_dev, RTL8139_MSI_VECTOR);
		if (msi_ret < 0) {
			// Resolve legacy IRQ via ACPI _PRT first (see e1000.c for the
			// detailed rationale — `interrupt_line` is unreliable in
			// APIC mode).
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
					kprintf("RTL8139: ACPI _PRT resolved INT%c -> GSI %u\n",
						'A' + acpi_pin, gsi);
				}
			}
			if (irq == 0xFF) {
				irq = dev->pci_dev->interrupt_line;
				if (irq != 0xFF && irq <= 23) {
					kprintf("RTL8139: ACPI _PRT lookup failed, falling back "
						"to PCI interrupt_line = %d\n",
						irq);
				} else {
					kprintf("RTL8139: WARNING: no valid IRQ\n");
				}
			}
			kprintf("RTL8139: Using legacy IRQ %d\n", irq);
			g_rtl8139_legacy_irq = irq;

			// Clear PCI Command INTx Disable (some firmwares set it).
			uint32_t cmd = pci_cfg_read32(
				dev->pci_dev->bus, dev->pci_dev->device,
				dev->pci_dev->function, 0x04);
			if (cmd & PCI_CMD_INTX_DISABLE) {
				cmd &= ~PCI_CMD_INTX_DISABLE;
				pci_cfg_write32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04, cmd);
				kprintf("RTL8139: cleared PCI Command INTx Disable bit\n");
			}

			if (irq <= 23) {
				uint8_t vector = 32 + irq;
				ioapic_configure_legacy_irq(
					irq, vector, IOAPIC_POLARITY_LOW,
					IOAPIC_TRIGGER_LEVEL);
			}
		}

		// Initialise net_device fields BEFORE enabling device IRQs so the
		// dispatcher (which routes the legacy IRQ to us once the
		// initialised flag is set) always sees a valid net_dev.
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("rtl8139");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, rtl8139_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_rtl8139_initialized = 1;

		// Initial link status (give the link a moment to come up)
		uint8_t msr = rtl_read8(dev, RTL_MSR);
		if (msr & RTL_MSR_LINKB) {
			uint32_t t0 = timer_pmtimer_read_raw();
			while (timer_pmtimer_delta_us(
				       t0, timer_pmtimer_read_raw()) <
			       5000000) {
				rtl_delay_us(50000);
				msr = rtl_read8(dev, RTL_MSR);
				if (!(msr & RTL_MSR_LINKB))
					break;
			}
		}
		dev->link_up = (msr & RTL_MSR_LINKB) ? 0 : 1;
		dev->link_seen = dev->link_up;
		kprintf("RTL8139: Link %s\n", dev->link_up ? "UP" : "DOWN");

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
		dev->net_dev.xmit = rtl8139_xmit;
		dev->net_dev.link_status = rtl8139_link_status;
		dev->net_dev.shutdown = rtl8139_shutdown;
		dev->net_dev.watchdog = rtl8139_watchdog;
		dev->net_dev.tx_timeout = rtl8139_tx_timeout;
		dev->net_dev.set_rx_mode = rtl8139_set_rx_mode;
		dev->net_dev.get_stats64 = rtl8139_get_stats64;

		net_register(&dev->net_dev);

		// Rings, filter, RX + TX on, the interrupts it handles
		rtl_hw_start(dev);
		kprintf("RTL8139: Driver initialized successfully\n");
		return; // first device only
	}
}
