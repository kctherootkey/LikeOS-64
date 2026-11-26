// LikeOS DEC 21x4x ("Tulip") NIC Driver
//
// Drives the DEC 21040 / 21041 / 21140 / 21142 / 21143 PCI Ethernet
// controllers — the QEMU `-device tulip` exposes a 21143 (vendor 0x1011,
// device 0x0019).  Programming model: 16 8-byte CSRs in BAR0, single TX
// and RX descriptor ring in "ring" mode (RER/TER on the last entry to
// loop back to the head).  Legacy INTx only — no MSI/MSI-X on this
// part family.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/tulip.h>
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
// Supported PCI device IDs
// ============================================================================
typedef struct {
	uint16_t vid, did;
	const char *name;
} tulip_id_t;

static const tulip_id_t tulip_pci_ids[] = {
	{ TULIP_VENDOR_DEC, TULIP_DEV_DC21040, "DEC DC21040 Tulip" },
	{ TULIP_VENDOR_DEC, TULIP_DEV_DC21041, "DEC DC21041 Tulip" },
	{ TULIP_VENDOR_DEC, TULIP_DEV_DC21140, "DEC DC21140 Fast Tulip" },
	{ TULIP_VENDOR_DEC, TULIP_DEV_DC21142_43,
	  "DEC DC21143 Tulip" }, // QEMU -device tulip
	{ 0, 0, NULL },
};

static const tulip_id_t *tulip_lookup(uint16_t vid, uint16_t did)
{
	for (const tulip_id_t *e = tulip_pci_ids; e->name; e++)
		if (e->vid == vid && e->did == did)
			return e;
	return NULL;
}

static tulip_dev_t g_tulip;
int g_tulip_initialized = 0;
int g_tulip_legacy_irq = -1;

// ============================================================================
// CSR access — supports both MMIO (BAR0 as memory) and PMIO (BAR0 as I/O).
// ============================================================================
static inline uint32_t tulip_csr_read(tulip_dev_t *dev, uint32_t off)
{
	if (dev->use_mmio) {
		return *(volatile uint32_t *)(dev->mmio_base + off);
	} else {
		return inl(dev->io_base + off);
	}
}
static inline void tulip_csr_write(tulip_dev_t *dev, uint32_t off, uint32_t v)
{
	if (dev->use_mmio) {
		*(volatile uint32_t *)(dev->mmio_base + off) = v;
		__asm__ volatile("mfence" ::: "memory");
	} else {
		outl(dev->io_base + off, v);
	}
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void tulip_delay_us(uint32_t us)
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
// SROM (serial EEPROM behind CSR9, a 93C46 or 93C66)
// ============================================================================
#define TULIP_SROM_READ_CMD 6 // start bit + read opcode
#define TULIP_CSR9_SR (1u << 11)
#define TULIP_CSR9_RD (1u << 14)
#define TULIP_CSR9_CS (1u << 0)
#define TULIP_CSR9_SCLK (1u << 1)
#define TULIP_CSR9_SDI (1u << 2)
#define TULIP_CSR9_SDO (1u << 3)
#define TULIP_SROM_BYTES 128

/* One SROM read transaction for word `loc' with `addr_bits' address bits.
 * Returns every bit seen on SDO: the word in the low 16 bits, above it
 * what the part drove while the command and address went out (a 93C46
 * answers its dummy 0 two bits early when sent 8 address bits -- that is
 * how the address width is told apart). */
static uint32_t tulip_srom_xfer(tulip_dev_t *dev, uint32_t loc, int addr_bits)
{
	const uint32_t enb = TULIP_CSR9_SR | TULIP_CSR9_RD | TULIP_CSR9_CS;
	uint32_t cmd = loc | ((uint32_t)TULIP_SROM_READ_CMD << addr_bits);
	uint32_t ret = 0;

	tulip_csr_write(dev, TULIP_CSR9, enb & ~TULIP_CSR9_CS);
	tulip_csr_write(dev, TULIP_CSR9, enb);
	for (int i = 4 + addr_bits; i >= 0; i--) {
		uint32_t d = ((cmd >> i) & 1) ? TULIP_CSR9_SDI : 0;

		tulip_csr_write(dev, TULIP_CSR9, enb | d);
		tulip_delay_us(2);
		tulip_csr_write(dev, TULIP_CSR9, enb | d | TULIP_CSR9_SCLK);
		tulip_delay_us(2);
		ret = (ret << 1) |
		      ((tulip_csr_read(dev, TULIP_CSR9) & TULIP_CSR9_SDO) ? 1 :
									    0);
	}
	tulip_csr_write(dev, TULIP_CSR9, enb);
	tulip_delay_us(2);
	for (int i = 0; i < 16; i++) {
		tulip_csr_write(dev, TULIP_CSR9, enb | TULIP_CSR9_SCLK);
		tulip_delay_us(2);
		ret = (ret << 1) |
		      ((tulip_csr_read(dev, TULIP_CSR9) & TULIP_CSR9_SDO) ? 1 :
									    0);
		tulip_csr_write(dev, TULIP_CSR9, enb);
		tulip_delay_us(2);
	}
	tulip_csr_write(dev, TULIP_CSR9, enb & ~TULIP_CSR9_CS);
	return ret;
}

/* The 21140 media leaf of a DEC-format SROM (format version 1..4): the
 * general-purpose port direction, and from the default media block (else
 * the first non-MII one) the port value and the CSR6 port bits it wants.
 * Leaves gpr_valid 0 when the SROM has no usable leaf. */
static void tulip_parse_media(tulip_dev_t *dev, const uint8_t *ee)
{
	uint32_t leaf, nblocks, p;
	int chosen = 0;

	if (dev->device_id != TULIP_DEV_DC21140)
		return;
	if (ee[18] < 1 || ee[18] > 4 || ee[19] < 1)
		return; // not a DEC-format SROM
	leaf = (uint32_t)ee[27] | ((uint32_t)ee[28] << 8);
	if (leaf < 30 || leaf + 4 > TULIP_SROM_BYTES)
		return;
	dev->gpr_dir = ee[leaf + 2];
	nblocks = ee[leaf + 3];
	p = leaf + 4;
	for (uint32_t b = 0; b < nblocks && p < TULIP_SROM_BYTES; b++) {
		uint32_t len, gpr_off;
		uint16_t cmd;

		if (ee[p] & 0x80) { // extended block: length, type, data
			len = (ee[p] & 0x7F) + 1u;
			if (p + len > TULIP_SROM_BYTES || len < 6 ||
			    ee[p + 1] != 0) {
				p += len;
				continue; // MII and other block types
			}
			gpr_off = p + 3;
		} else { // compact block: media, GPR, command (16 bits)
			len = 4;
			if (p + len > TULIP_SROM_BYTES)
				break;
			gpr_off = p + 1;
		}
		cmd = (uint16_t)(ee[gpr_off + 1] |
				 ((uint16_t)ee[gpr_off + 2] << 8));
		if (!chosen || (cmd & 0x4000)) { // bit 14: default media
			dev->gpr_data = ee[gpr_off];
			dev->csr6_media = 0x02000000u | ((cmd & 0x71u) << 18);
			chosen = 1;
			if (cmd & 0x4000)
				break;
		}
		p += len;
	}
	dev->gpr_valid = chosen;
}

/* The station address from the SROM: the address width probed, the first
 * 128 bytes read; boards following the DEC SROM format keep the address at
 * byte 20, older ones at byte 0 (recognisable by their first 8 bytes
 * repeating at byte 16).  Only an SROM with no address at all leaves the
 * fallback. */
static void tulip_read_mac(tulip_dev_t *dev)
{
	uint8_t ee[TULIP_SROM_BYTES];
	int addr_bits =
		(tulip_srom_xfer(dev, 0xFF, 8) & 0x40000u) ? 8 : 6;
	int sa = 0;
	uint32_t sum = 0;

	for (uint32_t w = 0; w < TULIP_SROM_BYTES / 2; w++) {
		uint16_t v = (uint16_t)tulip_srom_xfer(dev, w, addr_bits);

		ee[2 * w] = (uint8_t)v;
		ee[2 * w + 1] = (uint8_t)(v >> 8);
	}
	tulip_csr_write(dev, TULIP_CSR9, 0); // leave SROM mode
	for (int i = 0; i < 8; i++)
		if (ee[i] != ee[16 + i])
			sa = 20;
	if (ee[0] == 0xFF && ee[1] == 0xFF && ee[2] == 0)
		sa = 2; // multiport boards: the address follows a marker
	for (int i = 0; i < 6; i++) {
		dev->mac_addr[i] = ee[sa + i];
		sum += ee[sa + i];
	}
	if (sum == 0 || sum == 6 * 0xFF || (dev->mac_addr[0] & 0x01)) {
		kprintf("Tulip: SROM holds no station address, using fallback\n");
		dev->mac_addr[0] = 0x52;
		dev->mac_addr[1] = 0x54;
		dev->mac_addr[2] = 0x00;
		dev->mac_addr[3] = 0x12;
		dev->mac_addr[4] = 0x34;
		dev->mac_addr[5] = 0x56;
	}
	tulip_parse_media(dev, ee);
}

// ============================================================================
// Software reset (CSR0.SWR — self-clearing)
// ============================================================================
static int tulip_reset(tulip_dev_t *dev)
{
	tulip_csr_write(dev, TULIP_CSR0, TULIP_CSR0_SWR);
	// Datasheet: SWR self-clears within ~50 PCI cycles.  Wait up to 1 ms.
	for (int i = 0; i < 1000; i++) {
		if (!(tulip_csr_read(dev, TULIP_CSR0) & TULIP_CSR0_SWR))
			break;
		tulip_delay_us(1);
	}
	if (tulip_csr_read(dev, TULIP_CSR0) & TULIP_CSR0_SWR) {
		kprintf("Tulip: software reset did not clear\n");
		return -1;
	}

	// Sensible bus mode: 32 longword burst, no automatic polling.
	// BAR=10b (programmable burst length), PBL=32 (bits 13:8 = 0x20),
	// CAL=8 longword cache alignment (bits 15:14 = 0b10).
	tulip_csr_write(dev, TULIP_CSR0, (1u << 14) | (32u << 8));
	return 0;
}

// ============================================================================
// Rings
// ============================================================================

/* Every RX descriptor the chip's, every TX descriptor ours, positions at
 * the start; the ring bases into CSR3/CSR4. */
static void tulip_rings_reset(tulip_dev_t *dev)
{
	for (int i = 0; i < TULIP_NUM_RX_DESC; i++) {
		// RDES1.BS1 is 11 bits (bits 0..10) — max 2047, so 2048 would
		// wrap to 0 and the chip would silently drop every received
		// frame.  Use 1536 (next multiple of 32 above max ethernet
		// frame 1518).
		dev->rx_descs[i].ctrl = 1536 & 0x7FF;
		if (i == TULIP_NUM_RX_DESC - 1)
			dev->rx_descs[i].ctrl |= TULIP_RDES1_RER;
		dev->rx_descs[i].buf1 = (uint32_t)dev->rx_bufs_phys[i];
		dev->rx_descs[i].buf2 = 0;
		__asm__ volatile("" ::: "memory");
		*(volatile uint32_t *)&dev->rx_descs[i].status = TULIP_RDES0_OWN;
	}
	for (int i = 0; i < TULIP_NUM_TX_DESC; i++) {
		*(volatile uint32_t *)&dev->tx_descs[i].status = 0;
		dev->tx_descs[i].ctrl =
			(i == TULIP_NUM_TX_DESC - 1) ? TULIP_TDES1_TER : 0;
		dev->tx_descs[i].buf1 = (uint32_t)dev->tx_bufs_phys[i];
		dev->tx_descs[i].buf2 = 0;
		dev->tx_stamp[i] = 0;
	}
	__asm__ volatile("mfence" ::: "memory");
	dev->rx_cur = 0;
	dev->tx_cur = 0;
	dev->tx_dirty = 0;
	tulip_csr_write(dev, TULIP_CSR3, (uint32_t)dev->rx_descs_phys);
	tulip_csr_write(dev, TULIP_CSR4, (uint32_t)dev->tx_descs_phys);
}

static int tulip_init_rx(tulip_dev_t *dev)
{
	uint32_t bytes = sizeof(tulip_desc_t) * TULIP_NUM_RX_DESC;
	uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys) {
		kprintf("Tulip: failed to allocate RX ring\n");
		return -1;
	}
	if (phys >> 32) {
		kprintf("Tulip: RX ring phys 0x%lx above 4GB\n", phys);
		return -1;
	}
	dev->rx_descs = (tulip_desc_t *)phys_to_virt(phys);
	dev->rx_descs_phys = phys;
	mm_memset(dev->rx_descs, 0, bytes);

	for (int i = 0; i < TULIP_NUM_RX_DESC; i++) {
		uint64_t bp = mm_allocate_physical_page();
		if (!bp || (bp >> 32)) {
			kprintf("Tulip: bad RX buffer alloc %d\n", i);
			return -1;
		}
		dev->rx_bufs[i] = (uint8_t *)phys_to_virt(bp);
		dev->rx_bufs_phys[i] = bp;
	}
	return 0;
}

static int tulip_init_tx(tulip_dev_t *dev)
{
	uint32_t bytes = sizeof(tulip_desc_t) * TULIP_NUM_TX_DESC;
	uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys) {
		kprintf("Tulip: failed to allocate TX ring\n");
		return -1;
	}
	if (phys >> 32) {
		kprintf("Tulip: TX ring phys 0x%lx above 4GB\n", phys);
		return -1;
	}
	dev->tx_descs = (tulip_desc_t *)phys_to_virt(phys);
	dev->tx_descs_phys = phys;
	mm_memset(dev->tx_descs, 0, bytes);

	for (int i = 0; i < TULIP_NUM_TX_DESC; i++) {
		uint64_t bp = mm_allocate_physical_page();
		if (!bp || (bp >> 32)) {
			kprintf("Tulip: bad TX buffer alloc %d\n", i);
			return -1;
		}
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(bp);
		dev->tx_bufs_phys[i] = bp;
	}
	return 0;
}

// ============================================================================
// Transmit (net_device_t xmit hook) and setup frames
// ============================================================================
#define TULIP_TX_WAKE_THRESH 8

static uint32_t tulip_tx_unused(const tulip_dev_t *dev)
{
	return nic_ring_unused(dev->tx_cur, dev->tx_dirty, TULIP_NUM_TX_DESC);
}

/* Hand descriptor tx_cur (buffer filled, `ctrl' = TDES1 without the ring
 * end bit) to the chip and kick the transmit process.  Caller holds
 * tx_lock. */
static void tulip_tx_post_locked(tulip_dev_t *dev, uint32_t ctrl)
{
	uint16_t slot = dev->tx_cur;
	volatile tulip_desc_t *desc = &dev->tx_descs[slot];

	if (slot == TULIP_NUM_TX_DESC - 1)
		ctrl |= TULIP_TDES1_TER;
	desc->ctrl = ctrl;
	desc->buf1 = (uint32_t)dev->tx_bufs_phys[slot];
	desc->buf2 = 0;
	__asm__ volatile("mfence" ::: "memory");
	desc->status = TULIP_TDES0_OWN; // hand off to hardware
	__asm__ volatile("mfence" ::: "memory");
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp[slot] = now ? now : 1;
	}
	dev->tx_cur = (uint16_t)((slot + 1) % TULIP_NUM_TX_DESC);
	tulip_csr_write(dev, TULIP_CSR1, 0xFFFFFFFFu); // transmit poll demand
}

/* Reclaim descriptors the chip handed back, counting the errors it
 * reported.  Caller holds tx_lock. */
static void tulip_tx_clean_locked(tulip_dev_t *dev)
{
	while (dev->tx_dirty != dev->tx_cur) {
		volatile tulip_desc_t *d = &dev->tx_descs[dev->tx_dirty];
		uint32_t st = d->status;

		if (st & TULIP_TDES0_OWN)
			break;
		if (!(d->ctrl & TULIP_TDES1_SET)) {
			if (st & TULIP_TDES0_ES) {
				dev->net_dev.tx_errors++;
				if (st & TULIP_TDES0_UF)
					dev->st_tx_fifo++;
				if (st & (TULIP_TDES0_NC | TULIP_TDES0_LO))
					dev->st_tx_carrier++;
				if (st & TULIP_TDES0_LC)
					dev->st_tx_window++;
				if (st & TULIP_TDES0_EC)
					dev->st_tx_aborted++;
			}
			dev->st_colls += (st >> TULIP_TDES0_CC_SHIFT) & 0xF;
		}
		dev->tx_stamp[dev->tx_dirty] = 0;
		dev->tx_dirty = (uint16_t)((dev->tx_dirty + 1) % TULIP_NUM_TX_DESC);
	}
	if (netif_queue_stopped(&dev->net_dev) &&
	    tulip_tx_unused(dev) >= TULIP_TX_WAKE_THRESH)
		netif_wake_queue(&dev->net_dev);
}

/* The receive filter: a setup frame (perfect filtering of up to 14
 * multicast addresses plus broadcast and ours, the 512-bit hash beyond
 * that) through the transmit ring, and CSR6's pass-all-multicast and
 * promiscuous bits.  Caller holds tx_lock.  Returns -1 when the ring has
 * no room (try again later). */
static int tulip_set_filter_locked(tulip_dev_t *dev)
{
	uint8_t mc[NETDEV_MC_MAX][6];
	int all = 0, n = 0;
	uint32_t flags = 0, ctrl;

	if (tulip_tx_unused(dev) == 0) {
		tulip_tx_clean_locked(dev);
		if (tulip_tx_unused(dev) == 0)
			return -1;
	}
	if (dev->net_dev.core_ready) {
		n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
		flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);
	}
	uint32_t *frame = (uint32_t *)dev->tx_bufs[dev->tx_cur];

	if (n <= NIC_TULIP_PERFECT_MAX) {
		nic_tulip_setup_perfect(frame, dev->mac_addr,
					(const uint8_t(*)[6])mc, n);
		ctrl = TULIP_TDES1_SET | NIC_TULIP_SETUP_LEN;
	} else {
		nic_tulip_setup_hash(frame, dev->mac_addr,
				     (const uint8_t(*)[6])mc, n);
		ctrl = TULIP_TDES1_SET | TULIP_TDES1_FT0 | NIC_TULIP_SETUP_LEN;
	}
	tulip_tx_post_locked(dev, ctrl);

	dev->csr6 &= ~(TULIP_CSR6_PR | TULIP_CSR6_PM);
	if (flags & IFF_PROMISC)
		dev->csr6 |= TULIP_CSR6_PR | TULIP_CSR6_PM;
	else if (all || (flags & IFF_ALLMULTI))
		dev->csr6 |= TULIP_CSR6_PM;
	tulip_csr_write(dev, TULIP_CSR6, dev->csr6);
	return 0;
}

static int tulip_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;

	BUG_ON(dev == NULL);
	if (len == 0 || len > 1536) { // TDES1.BS1: 11 bits
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&dev->tx_lock, &f);
	if (tulip_tx_unused(dev) == 0) {
		tulip_tx_clean_locked(dev);
		if (tulip_tx_unused(dev) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}
	uint32_t tx_len = (len < 60) ? 60 : len;
	uint8_t *buf = dev->tx_bufs[dev->tx_cur];

	mm_memcpy(buf, skb->data, len);
	if (tx_len > len)
		mm_memset(buf + len, 0, tx_len - len);
	tulip_tx_post_locked(dev, TULIP_TDES1_FS | TULIP_TDES1_LS |
					  TULIP_TDES1_IC | (tx_len & 0x7FF));
	ndev->tx_packets++;
	ndev->tx_bytes += tx_len;
	if (tulip_tx_unused(dev) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

/* Interrupt sources: all handled; the poll owns RX/TX completion and
 * receive-unavailable while it runs. */
#define TULIP_CSR7_ALL                                                   \
	(TULIP_CSR7_RIE | TULIP_CSR7_TIE | TULIP_CSR7_RUE | TULIP_CSR7_LFE | \
	 TULIP_CSR7_LPE | TULIP_CSR7_NIE | TULIP_CSR7_AIE | TULIP_CSR7_FBE)
#define TULIP_CSR7_NAPI (TULIP_CSR7_RIE | TULIP_CSR7_TIE | TULIP_CSR7_RUE)

/* Program the reset chip: rings, the 21140 general-purpose port, the
 * filter (setup frame), receiver and transmitter on, interrupts.  Bring-up
 * and the timeout restart (tx_lock held there). */
static void tulip_hw_start(tulip_dev_t *dev)
{
	tulip_rings_reset(dev);
	if (dev->gpr_valid) {
		// direction first (bit 8 makes the write a direction write)
		tulip_csr_write(dev, TULIP_CSR12, 0x100u | dev->gpr_dir);
		tulip_csr_write(dev, TULIP_CSR12, dev->gpr_data);
	}
	/* Full duplex, store-and-forward, the SROM's port bits on the
	 * 21140.  Receiver and transmitter start; the setup frame queued
	 * first is what the transmit process takes first, so the address
	 * filter is programmed before anything is sent -- no promiscuous
	 * mode needed for the broadcast DHCP replies. */
	dev->csr6 = TULIP_CSR6_SR | TULIP_CSR6_ST | TULIP_CSR6_FD |
		    TULIP_CSR6_SF | dev->csr6_media;
	(void)tulip_set_filter_locked(dev);
	tulip_csr_write(dev, TULIP_CSR5, 0xFFFFFFFFu); // stale status
	tulip_csr_write(dev, TULIP_CSR7, TULIP_CSR7_ALL);
}

// ============================================================================
// Link, statistics, watchdog
// ============================================================================
static int tulip_csr12_link(tulip_dev_t *dev)
{
	/* Only the 21041 and 21142/21143 have link status bits in CSR12
	 * (SIA status).  On the 21140 CSR12 is the general-purpose port --
	 * it reads back what tulip_hw_start wrote there -- and the 21040's
	 * SIA has another layout: no link indication, the link counts as
	 * up. */
	if (dev->device_id != TULIP_DEV_DC21142_43 &&
	    dev->device_id != TULIP_DEV_DC21041)
		return 1;
	uint32_t sia = tulip_csr_read(dev, TULIP_CSR12);
	// CSR12.LS10 / LS100 are active-low link-fail bits on the 21143:
	// link is up if either of those is 0 in the appropriate medium.
	return (!(sia & TULIP_CSR12_LS10) || !(sia & TULIP_CSR12_LS100)) ? 1 :
									    0;
}

/* The carrier follows CSR12 once it has reported a link at all. */
static void tulip_link_check(tulip_dev_t *dev)
{
	int now_up = tulip_csr12_link(dev);

	if (now_up)
		dev->link_seen = 1;
	if (now_up == dev->link_up)
		return;
	dev->link_up = now_up;
	kprintf("Tulip: Link %s\n", now_up ? "UP" : "DOWN");
	if (now_up) {
		netif_carrier_on(&dev->net_dev);
	} else {
		dhcp_invalidate(&dev->net_dev);
		if (dev->link_seen)
			netif_carrier_off(&dev->net_dev);
	}
}

static int tulip_link_status(net_device_t *ndev)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;
	int up = tulip_csr12_link(dev);

	if (up != dev->link_up)
		net_housekeeping_kick();
	return up;
}

static void tulip_set_rx_mode(net_device_t *ndev)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

static void tulip_get_stats64(net_device_t *ndev, struct net_device_stats64 *s)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;

	s->rx_crc_errors = dev->st_rx_crc;
	s->rx_length_errors = dev->st_rx_long;
	s->rx_missed_errors = dev->st_missed;
	s->tx_fifo_errors = dev->st_tx_fifo;
	s->tx_carrier_errors = dev->st_tx_carrier;
	s->tx_aborted_errors = dev->st_tx_aborted;
	s->collisions = dev->st_colls;
}

static void tulip_tx_timeout(net_device_t *ndev)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
}

// Quiesce before ACPI S5: software reset (clears CSR6.ST/SR), mask CSR7,
// drop bus master.
static void tulip_shutdown(net_device_t *ndev)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;
	if (!dev)
		return;
	if (!dev->use_mmio && !dev->io_base)
		return;
	if (dev->use_mmio && !dev->mmio_base)
		return;

	// Mask all interrupts
	tulip_csr_write(dev, TULIP_CSR7, 0);

	// Halt RX/TX by clearing CSR6.SR and CSR6.ST
	uint32_t csr6 = tulip_csr_read(dev, TULIP_CSR6);
	csr6 &= ~(TULIP_CSR6_SR | TULIP_CSR6_ST);
	tulip_csr_write(dev, TULIP_CSR6, csr6);

	// Software reset wipes the chip into a known-quiet state
	tulip_csr_write(dev, TULIP_CSR0, TULIP_CSR0_SWR);

	if (dev->pci_dev) {
		const pci_device_t *pdev = dev->pci_dev;
		uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device,
					      pdev->function, 0x04);
		cmd &= ~0x0004u;
		pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04,
				cmd);
	}
}

// Network housekeeping softirq, CPU 0, ~10 Hz and when kicked.
static void tulip_watchdog(net_device_t *ndev)
{
	tulip_dev_t *dev = (tulip_dev_t *)ndev->driver_data;
	uint64_t f, now = timer_ticks();
	int hang = 0;

	if (!dev || !g_tulip_initialized)
		return;
	tulip_link_check(dev);
	// CSR8: missed frames (bits 15:0), cleared on read
	dev->st_missed += tulip_csr_read(dev, TULIP_CSR8) & 0xFFFF;
	spin_lock_irqsave(&dev->tx_lock, &f);
	tulip_tx_clean_locked(dev);
	if (__atomic_load_n(&dev->rx_mode_pending, __ATOMIC_ACQUIRE) &&
	    tulip_set_filter_locked(dev) == 0)
		__atomic_store_n(&dev->rx_mode_pending, 0, __ATOMIC_RELEASE);
	if (dev->tx_dirty != dev->tx_cur && dev->tx_stamp[dev->tx_dirty] &&
	    now - dev->tx_stamp[dev->tx_dirty] > net_ms_to_ticks(5000))
		hang = 1;
	if (hang || __atomic_exchange_n(&dev->reset_pending, 0,
					__ATOMIC_ACQ_REL)) {
		/* Transmit timeout or fatal bus error: reset the chip and
		 * program it anew; what the rings held is dropped. */
		kprintf("Tulip: transmit timed out or bus error, resetting the chip\n");
		dev->net_dev.tx_errors +=
			(uint32_t)((dev->tx_cur + TULIP_NUM_TX_DESC -
				    dev->tx_dirty) %
				   TULIP_NUM_TX_DESC);
		tulip_csr_write(dev, TULIP_CSR7, 0);
		if (tulip_reset(dev) == 0)
			tulip_hw_start(dev);
		if (netif_queue_stopped(&dev->net_dev))
			netif_wake_queue(&dev->net_dev);
		spin_unlock_irqrestore(&dev->tx_lock, f);
		napi_schedule(&dev->napi);
		return;
	}
	spin_unlock_irqrestore(&dev->tx_lock, f);
}

// ============================================================================
// Receive (NAPI poll)
// ============================================================================
static int tulip_rx_clean(tulip_dev_t *dev, int budget)
{
	net_device_t *nd = &dev->net_dev;
	int work = 0;

	while (work < budget) {
		uint16_t slot = dev->rx_cur;
		volatile tulip_desc_t *desc = &dev->rx_descs[slot];
		uint32_t st = desc->status;

		if (st & TULIP_RDES0_OWN)
			break;
		if (!(st & TULIP_RDES0_ES) && (st & TULIP_RDES0_FS) &&
		    (st & TULIP_RDES0_LS)) {
			uint32_t len = (st & TULIP_RDES0_FL_MASK) >>
				       TULIP_RDES0_FL_SHIFT;
			// Hardware reports FCS-included length; strip 4 bytes
			if (len > 4)
				len -= 4;
			if (len >= 14 && len <= 1536) {
				sk_buff_t *skb = skb_alloc(len);

				if (skb) {
					const uint8_t *b = dev->rx_bufs[slot];

					skb->dev = nd;
					mm_memcpy(skb_append(skb, len), b, len);
					nd->rx_packets++;
					nd->rx_bytes += len;
					if (b[0] & 1)
						nd->multicast++;
					napi_gro_receive(&dev->napi, skb);
				} else {
					nd->rx_dropped++;
				}
			} else {
				nd->rx_errors++;
				dev->st_rx_long++;
			}
		} else {
			nd->rx_errors++;
			if (st & TULIP_RDES0_CE)
				dev->st_rx_crc++;
			if (st & TULIP_RDES0_TL)
				dev->st_rx_long++;
		}
		// Hand back to hardware
		desc->status = TULIP_RDES0_OWN;
		dev->rx_cur = (uint16_t)((slot + 1) % TULIP_NUM_RX_DESC);
		work++;
	}
	return work;
}

static int tulip_poll(struct napi_struct *n, int budget)
{
	tulip_dev_t *dev = (tulip_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	spin_lock_irqsave(&dev->tx_lock, &f);
	tulip_tx_clean_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	work = tulip_rx_clean(dev, budget);
	/* The receiver suspends when it finds no descriptor of its own
	 * (RU); with descriptors handed back, a poll demand restarts it. */
	if (work || __atomic_exchange_n(&dev->ru_seen, 0, __ATOMIC_ACQ_REL))
		tulip_csr_write(dev, TULIP_CSR2, 0xFFFFFFFFu);
	if (work < budget && napi_complete_done(n, work))
		tulip_csr_write(dev, TULIP_CSR7, TULIP_CSR7_ALL);
	return work;
}

// ============================================================================
// IRQ handler
// ============================================================================
/* Returns 1 when the interrupt was this chip's, 0 otherwise (shared legacy
 * line); the dispatcher acknowledges the local APIC.  Receive/transmit
 * work goes to the NAPI poll with those sources masked in CSR7 until it
 * is done; link changes and errors to the watchdog. */
int tulip_irq_handler(void)
{
	if (!g_tulip_initialized)
		return 0;

	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	tulip_dev_t *dev = &g_tulip;
	BUG_ON(dev == NULL);
	uint32_t sr = tulip_csr_read(dev, TULIP_CSR5);
	/* Ours when an enabled source is pending: the chip sums those into
	 * the normal/abnormal summary bits. */
	if (!(sr & (TULIP_CSR5_NIS | TULIP_CSR5_AIS)) || sr == 0xFFFFFFFFu)
		return 0;
	// Acknowledge the interrupt bits by writing 1s back
	tulip_csr_write(dev, TULIP_CSR5, sr & TULIP_CSR5_INT_MASK);

	if (sr & (TULIP_CSR5_LNF | TULIP_CSR5_LNP))
		net_housekeeping_kick();
	if (sr & TULIP_CSR5_RU)
		__atomic_store_n(&dev->ru_seen, 1, __ATOMIC_RELEASE);
	if (sr & TULIP_CSR5_FBE) {
		__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
		net_housekeeping_kick();
	}
	if (sr & (TULIP_CSR5_RI | TULIP_CSR5_RU | TULIP_CSR5_TI |
		  TULIP_CSR5_UNF | TULIP_CSR5_TPS)) {
		tulip_csr_write(dev, TULIP_CSR7,
				TULIP_CSR7_ALL & ~TULIP_CSR7_NAPI);
		napi_schedule(&dev->napi);
	}
	return 1;
}

// ============================================================================
// BAR resolution: prefer MMIO (BAR0 even), fall back to PMIO (BAR0 odd).
// ============================================================================
static int tulip_resolve_bar(tulip_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];
	if (bar0 & 1) {
		// I/O port BAR
		dev->use_mmio = 0;
		dev->io_base = (uint16_t)(bar0 & 0xFFFC);
		return 0;
	}
	// Memory-mapped BAR, strictly uncached
	dev->use_mmio = 1;
	uint64_t phys = bar0 & 0xFFFFFFF0ULL;
	if ((bar0 & 0x06) == 0x04)
		phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;

	dev->mmio_phys = phys;
	uint64_t page = phys & ~((uint64_t)PAGE_SIZE - 1);
	uint64_t virt = mm_map_device_mmio(page, 1);
	if (!virt) {
		kprintf("Tulip: cannot map the CSR BAR\n");
		return -1;
	}
	dev->mmio_base = (volatile uint8_t *)(virt + (phys - page));
	return 0;
}

// ============================================================================
// Driver entry point
// ============================================================================
void tulip_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		const tulip_id_t *match =
			tulip_lookup(devs[i].vendor_id, devs[i].device_id);
		if (!match)
			continue;

		kprintf("Tulip: Found %s (PCI %02x:%02x.%x)\n", match->name,
			devs[i].bus, devs[i].device, devs[i].function);

		tulip_dev_t *dev = &g_tulip;
		dev->pci_dev = &devs[i];
		dev->device_id = devs[i].device_id;
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("tulip_tx");

		pci_enable_busmaster_mem(dev->pci_dev);

		if (tulip_resolve_bar(dev) < 0)
			return;

		// If we landed on PMIO BAR0, also enable I/O Space in PCI Command
		if (!dev->use_mmio) {
			uint32_t cmd = pci_cfg_read32(
				dev->pci_dev->bus, dev->pci_dev->device,
				dev->pci_dev->function, 0x04);
			if (!(cmd & 0x1)) {
				pci_cfg_write32(dev->pci_dev->bus,
						dev->pci_dev->device,
						dev->pci_dev->function, 0x04,
						cmd | 0x1);
			}
			kprintf("Tulip: I/O base 0x%x\n", dev->io_base);
		} else {
			kprintf("Tulip: MMIO base 0x%lx\n", dev->mmio_phys);
		}

		if (tulip_reset(dev) < 0)
			return;

		tulip_read_mac(dev);
		kprintf("Tulip: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		if (tulip_init_rx(dev) < 0)
			return;
		if (tulip_init_tx(dev) < 0)
			return;

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
				kprintf("Tulip: ACPI _PRT resolved INT%c -> GSI %u\n",
					'A' + acpi_pin, gsi);
			}
		}
		if (irq == 0xFF) {
			irq = dev->pci_dev->interrupt_line;
			if (irq != 0xFF && irq <= 23) {
				kprintf("Tulip: ACPI _PRT lookup failed, falling back to "
					"PCI interrupt_line = %d\n",
					irq);
			} else {
				kprintf("Tulip: WARNING: no valid IRQ\n");
			}
		}
		kprintf("Tulip: Using legacy IRQ %d\n", irq);
		g_tulip_legacy_irq = irq;

		{
			uint32_t cmd = pci_cfg_read32(
				dev->pci_dev->bus, dev->pci_dev->device,
				dev->pci_dev->function, 0x04);
			if (cmd & PCI_CMD_INTX_DISABLE) {
				cmd &= ~PCI_CMD_INTX_DISABLE;
				pci_cfg_write32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04, cmd);
				kprintf("Tulip: cleared PCI Command INTx Disable bit\n");
			}
		}

		if (irq <= 23) {
			uint8_t vector = 32 + irq;
			ioapic_configure_legacy_irq(irq, vector,
						    IOAPIC_POLARITY_LOW,
						    IOAPIC_TRIGGER_LEVEL);
		}

		// Init net_device fields BEFORE enabling chip-side IRQs
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("tulip");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, tulip_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_tulip_initialized = 1;

		// Initial link probe
		dev->link_up = tulip_csr12_link(dev);
		dev->link_seen = dev->link_up;
		kprintf("Tulip: Link %s\n", dev->link_up ? "UP" : "DOWN");

		dev->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			dev->net_dev.mac_addr[m] = dev->mac_addr[m];
		dev->net_dev.mtu = NET_MTU_DEFAULT;
		dev->net_dev.ip_addr = 0;
		dev->net_dev.netmask = 0;
		dev->net_dev.gateway = 0;
		dev->net_dev.dns_server = 0;
		dev->net_dev.send = NULL;
		dev->net_dev.xmit = tulip_xmit;
		dev->net_dev.link_status = tulip_link_status;
		dev->net_dev.shutdown = tulip_shutdown;
		dev->net_dev.watchdog = tulip_watchdog;
		dev->net_dev.tx_timeout = tulip_tx_timeout;
		dev->net_dev.set_rx_mode = tulip_set_rx_mode;
		dev->net_dev.get_stats64 = tulip_get_stats64;

		net_register(&dev->net_dev);

		/* Rings, general-purpose port, setup frame, RX + TX on,
		 * interrupts unmasked. */
		{
			uint64_t f;

			spin_lock_irqsave(&dev->tx_lock, &f);
			tulip_hw_start(dev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
		}
		if (dev->gpr_valid)
			kprintf("Tulip: SROM media: GPR dir %02x data %02x, CSR6 %08x\n",
				dev->gpr_dir, dev->gpr_data, dev->csr6_media);

		kprintf("Tulip: Driver initialized successfully\n");
		return;
	}
}
