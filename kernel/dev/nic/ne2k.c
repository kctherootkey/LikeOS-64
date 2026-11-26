// LikeOS NE2000-compatible PCI NIC Driver (Realtek RTL8029AS)
//
// The RTL8029(AS) is the PCI bus member of the NE2000 family of 10 Mbit
// Ethernet controllers.  It uses an internal 16 KiB SRAM accessed via a
// "remote DMA" register file, and a single 32-port I/O window (BAR0).
// Emulated by QEMU as `-device ne2k_pci`.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/ne2k.h>
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
// Supported PCI device IDs.  All NE2000-PCI clones present the same
// RTL8029-style register layout.
// ============================================================================
typedef struct {
	uint16_t vid, did;
	const char *name;
} ne2k_id_t;

static const ne2k_id_t ne2k_pci_ids[] = {
	{ 0x10EC, 0x8029, "Realtek RTL8029" }, // QEMU -device ne2k_pci
	{ 0x1050, 0x0940, "Winbond 89C940" },
	{ 0x11F6, 0x1401, "Compex RL2000" },
	{ 0x8E2E, 0x3000, "KTI ET32P2" },
	{ 0x4A14, 0x5000, "NetVin NV5000SC" },
	{ 0x1106, 0x0926, "Via 86C926" },
	{ 0x10BD, 0x0E34, "SureCom NE34" },
	{ 0x1050, 0x5A5A, "Holtek HT80232" },
	{ 0x12C3, 0x0058, "Quic 5DPC" },
	{ 0x12C3, 0x5598, "Quic 5DPC v2" },
	{ 0x8C4A, 0x1980, "Winbond W89C940" },
	{ 0, 0, NULL },
};

static const ne2k_id_t *ne2k_lookup(uint16_t vid, uint16_t did)
{
	for (const ne2k_id_t *e = ne2k_pci_ids; e->name; e++) {
		if (e->vid == vid && e->did == did)
			return e;
	}
	return NULL;
}

// Single-NIC state (mirrors rtl8139 / pcnet32)
static ne2k_dev_t g_ne2k;
int g_ne2k_initialized = 0;
int g_ne2k_legacy_irq = -1;

// ============================================================================
// I/O helpers
// ============================================================================
static inline void ne_w8(ne2k_dev_t *dev, uint16_t reg, uint8_t v)
{
	outb(dev->io_base + reg, v);
}
static inline uint8_t ne_r8(ne2k_dev_t *dev, uint16_t reg)
{
	return inb(dev->io_base + reg);
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void ne_delay_us(uint32_t us)
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
// Remote DMA helpers — read/write internal SRAM via the data port.
// The chip runs in word mode (DCR.WTS): every transfer moves 16-bit words,
// so the byte count is rounded up to even -- a read stores the extra byte
// (the buffer has room for it), a write sends one byte of padding (the
// transmit length is programmed separately).  Byte mode (8-bit) is only
// used for the address PROM before the chip is configured.
// Caller holds the chip lock.
// ============================================================================
static void ne_wait_rdc(ne2k_dev_t *dev)
{
	for (int i = 0; i < 1000; i++) {
		if (ne_r8(dev, NE_P0_ISR) & NE_INT_RDC)
			break;
		ne_delay_us(10);
	}
	ne_w8(dev, NE_P0_ISR, NE_INT_RDC);
}

static void ne_dma_setup(ne2k_dev_t *dev, uint16_t addr, uint16_t count,
			 uint8_t cmd)
{
	ne_w8(dev, NE_P0_ISR, NE_INT_RDC);
	ne_w8(dev, NE_P0_RBCR0, count & 0xFF);
	ne_w8(dev, NE_P0_RBCR1, (count >> 8) & 0xFF);
	ne_w8(dev, NE_P0_RSAR0, addr & 0xFF);
	ne_w8(dev, NE_P0_RSAR1, (addr >> 8) & 0xFF);
	ne_w8(dev, NE_CR, cmd | NE_CR_STA);
}

/* Byte-mode read (the PROM, before word mode is set up). */
static void ne_dma_read8(ne2k_dev_t *dev, uint16_t src, uint8_t *dst,
			 uint16_t len)
{
	ne_dma_setup(dev, src, len, NE_CR_RD_RD);
	for (uint16_t i = 0; i < len; i++)
		dst[i] = inb(dev->io_base + NE_DATA);
	ne_wait_rdc(dev);
}

/* Word-mode read of `len' bytes; `dst' has room for len rounded up to
 * even. */
static void ne_dma_read(ne2k_dev_t *dev, uint16_t src, uint8_t *dst,
			uint16_t len)
{
	uint16_t count = (uint16_t)((len + 1u) & ~1u);

	ne_dma_setup(dev, src, count, NE_CR_RD_RD);
	for (uint16_t i = 0; i < count; i += 2) {
		uint16_t w = inw(dev->io_base + NE_DATA);

		dst[i] = (uint8_t)w;
		dst[i + 1] = (uint8_t)(w >> 8);
	}
	ne_wait_rdc(dev);
}

/* Word-mode write of `len' bytes from `src', which holds len rounded up to
 * even. */
static void ne_dma_write(ne2k_dev_t *dev, uint16_t dst, const uint8_t *src,
			 uint16_t len)
{
	uint16_t count = (uint16_t)((len + 1u) & ~1u);

	ne_dma_setup(dev, dst, count, NE_CR_RD_WR);
	for (uint16_t i = 0; i < count; i += 2)
		outw(dev->io_base + NE_DATA,
		     (uint16_t)(src[i] | ((uint16_t)src[i + 1] << 8)));
	ne_wait_rdc(dev);
}

// ============================================================================
// Reset chip
// ============================================================================
static int ne_reset(ne2k_dev_t *dev)
{
	// Read-then-write the reset port (the original NE2000 trick).
	uint8_t r = ne_r8(dev, NE_RESET);
	ne_w8(dev, NE_RESET, r);

	// Wait for ISR.RST to set, max ~10 ms.
	for (int i = 0; i < 1000; i++) {
		if (ne_r8(dev, NE_P0_ISR) & NE_INT_RST) {
			ne_w8(dev, NE_P0_ISR, 0xFF); // ack everything
			return 0;
		}
		ne_delay_us(10);
	}
	kprintf("NE2K: reset timeout (ISR=0x%x)\n", ne_r8(dev, NE_P0_ISR));
	return -1;
}

// ============================================================================
// Read MAC from PROM (first 6 logical bytes of internal address ROM)
//
// The NE2000's PROM is wired to a 16-bit data bus and **every logical
// byte appears twice** in the I/O space (real silicon and QEMU both do
// this — see ne2000_reset() in QEMU which explicitly duplicates the
// PROM contents into pairs).  So to recover the 6 MAC bytes we read
// 12 bytes and keep every other one.
// ============================================================================
static void ne_read_mac(ne2k_dev_t *dev)
{
	// After reset the chip is in 8-bit DMA mode (DCR.WTS=0); the PROM is
	// mapped at remote DMA address 0x0000.
	uint8_t prom[32] = { 0 };

	// Make sure we're on Page 0, abort any in-flight DMA.
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
	ne_w8(dev, NE_P0_DCR, NE_DCR_LS | NE_DCR_FT_8B); // 8-bit, normal, FT=8
	ne_w8(dev, NE_P0_RBCR0, 0);
	ne_w8(dev, NE_P0_RBCR1, 0);
	ne_w8(dev, NE_P0_RCR, NE_RCR_MON); // monitor mode
	ne_w8(dev, NE_P0_TCR, NE_TCR_LB_INTERNAL); // internal loopback
	ne_w8(dev, NE_P0_IMR, 0); // mask all
	ne_w8(dev, NE_P0_ISR, 0xFF); // clear all

	ne_dma_read8(dev, 0x0000, prom, sizeof(prom));

	// De-duplicate: take bytes 0, 2, 4, 6, 8, 10
	for (int i = 0; i < 6; i++)
		dev->mac_addr[i] = prom[i * 2];
}

// Interrupt sources: all handled; the NAPI poll owns PRX/RXE while it runs
#define NE_IMR_ALL                                                   \
	(NE_INT_PRX | NE_INT_PTX | NE_INT_RXE | NE_INT_TXE | NE_INT_OVW | \
	 NE_INT_CNT)
#define NE_IMR_NAPI (NE_INT_PRX | NE_INT_RXE)

/* The receive filter: page 1 MAR0..7 (a 64-bit hash of the multicast
 * addresses, all ones for every multicast), RCR accept bits.  The chip
 * keeps running; the receiver accepts broadcast only meanwhile.  Caller
 * holds the chip lock. */
static void ne_set_filter_locked(ne2k_dev_t *dev, const uint8_t mar[8],
				 uint8_t rcr)
{
	ne_w8(dev, NE_P0_RCR, NE_RCR_AB);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STA);
	for (int m = 0; m < 8; m++)
		ne_w8(dev, NE_P1_MAR0 + m, mar[m]);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
	ne_w8(dev, NE_P0_RCR, rcr);
}

static void ne_compute_filter(ne2k_dev_t *dev, uint8_t mar[8], uint8_t *rcr)
{
	uint8_t mc[NETDEV_MC_MAX][6];
	int all = 0, n = 0;
	uint32_t flags = 0;

	if (dev->net_dev.core_ready) {
		n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
		flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);
	}
	for (int m = 0; m < 8; m++)
		mar[m] = 0;
	*rcr = NE_RCR_AB;
	if (all || (flags & (IFF_ALLMULTI | IFF_PROMISC))) {
		for (int m = 0; m < 8; m++)
			mar[m] = 0xFF;
		*rcr |= NE_RCR_AM;
	} else if (n > 0) {
		for (int i = 0; i < n; i++) {
			uint32_t bit = nic_mc_hash64_be(mc[i]);

			mar[bit >> 3] |= (uint8_t)(1u << (bit & 7));
		}
		*rcr |= NE_RCR_AM;
	}
	if (flags & IFF_PROMISC)
		*rcr |= NE_RCR_PRO;
}

/* Program the reset chip (NS DP8390 sequence): stopped, word-mode DMA,
 * receiver in monitor mode and transmitter in loopback while the ring
 * pointers, the address and the filter go in, then started with the
 * interrupts unmasked.  Both transmit buffers and the ring start empty.
 * Caller holds the chip lock (or the device is not live yet). */
static void ne_hw_init(ne2k_dev_t *dev)
{
	uint8_t mar[8], rcr;

	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
	// word transfers, normal operation, FIFO threshold 8 bytes
	ne_w8(dev, NE_P0_DCR, NE_DCR_WTS | NE_DCR_LS | NE_DCR_FT_8B);
	ne_w8(dev, NE_P0_RBCR0, 0);
	ne_w8(dev, NE_P0_RBCR1, 0);
	ne_w8(dev, NE_P0_RCR, NE_RCR_MON);
	ne_w8(dev, NE_P0_TCR, NE_TCR_LB_INTERNAL);

	// Ring pointers — standard 8390 convention:
	//   BNRY = PSTART, CURR = PSTART + 1.  The ring-empty test in
	//   the chip is `BNRY + 1 == CURR`, so this leaves the ring
	//   empty as expected on entry.
	ne_w8(dev, NE_P0_TPSR, NE_TX_PAGE_START);
	ne_w8(dev, NE_P0_PSTART, NE_RX_PAGE_START);
	ne_w8(dev, NE_P0_PSTOP, NE_RX_PAGE_STOP);
	ne_w8(dev, NE_P0_BNRY, NE_RX_PAGE_START);

	// Clear interrupts and mask
	ne_w8(dev, NE_P0_ISR, 0xFF);
	ne_w8(dev, NE_P0_IMR, 0);

	// Page 1 — program MAC, CURR
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STP);
	for (int m = 0; m < 6; m++)
		ne_w8(dev, NE_P1_PAR0 + m, dev->mac_addr[m]);
	ne_w8(dev, NE_P1_CURR, NE_RX_PAGE_START + 1);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
	dev->next_pkt = NE_RX_PAGE_START + 1;
	dev->tx_len[0] = dev->tx_len[1] = 0;
	dev->tx_active = -1;
	dev->tx_next = 0;
	dev->tx_stamp = 0;

	// Back to page 0, start chip, filter, interrupts
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
	ne_w8(dev, NE_P0_TCR, NE_TCR_LB_NORMAL);
	ne_compute_filter(dev, mar, &rcr);
	ne_set_filter_locked(dev, mar, rcr);
	ne_w8(dev, NE_P0_ISR, 0xFF);
	dev->imr = NE_IMR_ALL;
	ne_w8(dev, NE_P0_IMR, dev->imr);
}

// ============================================================================
// Transmit (net_device_t xmit hook): two buffers, one sent while the next
// is loaded
// ============================================================================

/* Start sending buffer `i'.  Caller holds the chip lock. */
static void ne_tx_start_locked(ne2k_dev_t *dev, int i)
{
	ne_w8(dev, NE_P0_TPSR, (uint8_t)(NE_TX_PAGE_START + i * NE_TX_PAGES));
	ne_w8(dev, NE_P0_TBCR0, dev->tx_len[i] & 0xFF);
	ne_w8(dev, NE_P0_TBCR1, (dev->tx_len[i] >> 8) & 0xFF);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_TXP | NE_CR_STA);
	dev->tx_active = i;
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp = now ? now : 1;
	}
}

/* The active transmission ended (PTX or TXE): free its buffer and start
 * the other if it is loaded.  Caller holds the chip lock. */
static void ne_tx_done_locked(ne2k_dev_t *dev, int error)
{
	int i = dev->tx_active;

	if (i < 0)
		return;
	if (error)
		dev->net_dev.tx_errors++;
	dev->tx_len[i] = 0;
	dev->tx_active = -1;
	dev->tx_stamp = 0;
	if (dev->tx_len[i ^ 1])
		ne_tx_start_locked(dev, i ^ 1);
	if (netif_queue_stopped(&dev->net_dev))
		netif_wake_queue(&dev->net_dev);
}

static int ne2k_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint8_t buf[1520];
	uint64_t f;

	BUG_ON(dev == NULL);
	if (len == 0 || len > 1518) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}
	// Pad short frames to 60 bytes (the chip appends the CRC); one more
	// byte of room for the word-mode transfer.
	uint16_t out_len = (uint16_t)((len < 60) ? 60 : len);
	mm_memcpy(buf, skb->data, len);
	mm_memset(buf + len, 0, (size_t)(out_len - len) + 1);

	spin_lock_irqsave(&dev->tx_lock, &f);
	int i = dev->tx_len[dev->tx_next] ? dev->tx_next ^ 1 : dev->tx_next;
	if (dev->tx_len[i]) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&dev->tx_lock, f);
		return NETDEV_TX_BUSY;
	}
	ne_dma_write(dev, (uint16_t)((NE_TX_PAGE_START + i * NE_TX_PAGES) *
				     NE_PAGE_SIZE),
		     buf, out_len);
	dev->tx_len[i] = out_len;
	dev->tx_next = i ^ 1;
	if (dev->tx_active < 0)
		ne_tx_start_locked(dev, i);
	ndev->tx_packets++;
	ndev->tx_bytes += out_len;
	if (dev->tx_len[0] && dev->tx_len[1])
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

// ============================================================================
// Link status — RTL8029AS reports link via CONFIG1 bit 2 ("LSO"); the
// generic NE2000 has no link bit, so we just claim "up" when the chip is
// running.  QEMU's emulation always reports link up anyway.
// ============================================================================
static int ne2k_link_status(net_device_t *ndev)
{
	(void)ndev;
	return 1;
}

// Quiesce the controller ahead of an ACPI S5 transition.  The DP8390
// core itself has no Wake-on-LAN support, so we only need to mask
// interrupts, stop the chip, and drop PCI bus-master so the root
// complex discards any in-flight DMA after the OS has handed off to
// the firmware.
static void ne2k_shutdown(net_device_t *ndev)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)ndev->driver_data;
	if (!dev || !dev->io_base)
		return;

	// Page 0, abort any in-flight remote DMA, stop the chip.
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
	// IMR = 0 (mask all), ISR = 0xFF (W1C all pending causes).
	ne_w8(dev, NE_P0_IMR, 0x00);
	ne_w8(dev, NE_P0_ISR, 0xFF);

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

/* Throw the ring away and start it empty.  Caller holds the chip lock. */
static void ne_rx_ring_reset_locked(ne2k_dev_t *dev)
{
	ne_w8(dev, NE_P0_BNRY, NE_RX_PAGE_START);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STA);
	ne_w8(dev, NE_P1_CURR, NE_RX_PAGE_START + 1);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
	dev->next_pkt = NE_RX_PAGE_START + 1;
}

/* Take the next frame out of the ring into rx_bounce.  Returns its length,
 * 0 when the ring is empty, -1 when the ring was corrupt (and reset).
 * Caller holds the chip lock. */
static int ne_rx_one_locked(ne2k_dev_t *dev)
{
	// Read CURR from page 1
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STA);
	uint8_t curr = ne_r8(dev, NE_P1_CURR);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);

	if (curr == dev->next_pkt)
		return 0; // ring empty

	// The 4-byte header at <next_pkt>:0: receive status, next page,
	// length (LE, including the header)
	uint8_t hdr[4];
	ne_dma_read(dev, (uint16_t)(dev->next_pkt * NE_PAGE_SIZE), hdr, 4);

	uint8_t status = hdr[0];
	uint8_t next_pkt = hdr[1];
	uint16_t length = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);

	// Sanity-check: a valid packet has status bit 0 (PRX) set, the
	// next-page lies in the RX ring, and the length is plausible.
	if (!(status & NE_RSR_PRX) || next_pkt < NE_RX_PAGE_START ||
	    next_pkt >= NE_RX_PAGE_STOP || length < 4 + 14 || length > 1522) {
		dev->net_dev.rx_errors++;
		ne_rx_ring_reset_locked(dev);
		return -1;
	}

	uint16_t payload_len = length - 4; // strip the in-RAM 4-byte header
	if (payload_len > 1518)
		payload_len = 1518;

	// Packet may wrap the ring — split into two reads if so.
	uint16_t addr = (uint16_t)(dev->next_pkt * NE_PAGE_SIZE + 4);
	uint16_t end = NE_RX_PAGE_STOP * NE_PAGE_SIZE;
	if (addr + payload_len <= end) {
		ne_dma_read(dev, addr, dev->rx_bounce, payload_len);
	} else {
		uint16_t first = end - addr; // even: both ends are
		ne_dma_read(dev, addr, dev->rx_bounce, first);
		ne_dma_read(dev, NE_RX_PAGE_START * NE_PAGE_SIZE,
			    dev->rx_bounce + first, payload_len - first);
	}

	// Advance: BNRY = next_pkt - 1 (with wrap to PSTOP-1 at PSTART)
	uint8_t new_bnry = (next_pkt == NE_RX_PAGE_START) ?
				   (NE_RX_PAGE_STOP - 1) :
				   (next_pkt - 1);
	ne_w8(dev, NE_P0_BNRY, new_bnry);
	dev->next_pkt = next_pkt;
	return payload_len;
}

static int ne2k_poll(struct napi_struct *n, int budget)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)n->dev->driver_data;
	net_device_t *nd = &dev->net_dev;
	uint64_t f;
	int work = 0;

	while (work < budget) {
		int len;

		spin_lock_irqsave(&dev->tx_lock, &f);
		len = ne_rx_one_locked(dev);
		spin_unlock_irqrestore(&dev->tx_lock, f);
		if (len == 0)
			break;
		work++;
		if (len < 0)
			continue;
		sk_buff_t *skb = skb_alloc((uint32_t)len);

		if (!skb) {
			nd->rx_dropped++;
			continue;
		}
		skb->dev = nd;
		mm_memcpy(skb_append(skb, (uint32_t)len), dev->rx_bounce,
			  (size_t)len);
		nd->rx_packets++;
		nd->rx_bytes += (uint32_t)len;
		if (dev->rx_bounce[0] & 1)
			nd->multicast++;
		napi_gro_receive(&dev->napi, skb);
	}
	if (work < budget && napi_complete_done(n, work)) {
		spin_lock_irqsave(&dev->tx_lock, &f);
		dev->imr = NE_IMR_ALL;
		ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
		ne_w8(dev, NE_P0_IMR, dev->imr);
		spin_unlock_irqrestore(&dev->tx_lock, f);
	}
	return work;
}

/* Receive ring overflow (the DP8390 procedure): stop, let the receive
 * finish, clear the remote byte count, note whether a transmission was cut
 * short, restart in loopback, empty the ring, back to normal and resend
 * what was cut short.  The ring's frames are dropped.  Caller holds the
 * chip lock. */
static void ne_rx_overrun_locked(ne2k_dev_t *dev)
{
	int was_txing = (ne_r8(dev, NE_CR) & NE_CR_TXP) != 0;

	dev->st_overruns++;
	dev->net_dev.rx_dropped++;
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
	ne_delay_us(2000);
	ne_w8(dev, NE_P0_RBCR0, 0);
	ne_w8(dev, NE_P0_RBCR1, 0);
	int resend = was_txing &&
		     !(ne_r8(dev, NE_P0_ISR) & (NE_INT_PTX | NE_INT_TXE));
	ne_w8(dev, NE_P0_TCR, NE_TCR_LB_INTERNAL);
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
	ne_rx_ring_reset_locked(dev);
	ne_w8(dev, NE_P0_ISR, NE_INT_OVW);
	ne_w8(dev, NE_P0_TCR, NE_TCR_LB_NORMAL);
	if (resend)
		ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_TXP | NE_CR_STA);
}

// ============================================================================
// Top-half IRQ handler
// ============================================================================
/* Returns 1 when the interrupt was this card's, 0 otherwise (shared line);
 * the dispatcher acknowledges the local APIC.
 *
 * The whole handler runs under the chip lock: the DP8390 has one command
 * register, one register page select and one remote-DMA engine, which the
 * transmit hook and the poll drive too.  Transmit completion is handled
 * here (it starts the other buffer); received frames go to the NAPI poll
 * with PRX/RXE masked until it is done. */
int ne2k_irq_handler(void)
{
	if (!g_ne2k_initialized)
		return 0;

	// Feed entropy from NIC IRQ timing
	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	ne2k_dev_t *dev = &g_ne2k;
	uint64_t lflags;
	int sched = 0;

	spin_lock_irqsave(&dev->tx_lock, &lflags);

	// Force page 0 so we can read ISR
	ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);

	uint8_t raw = ne_r8(dev, NE_P0_ISR);
	/* Ours when a source we enabled is pending (RST and RDC never are;
	 * receive sources latched while the poll runs are not either). */
	uint8_t isr = raw & NE_IMR_ALL;
	if (raw == 0xFF || !(isr & dev->imr)) {
		spin_unlock_irqrestore(&dev->tx_lock, lflags);
		return 0;
	}

	// Ack handled bits (W1C)
	ne_w8(dev, NE_P0_ISR, isr);

	if (isr & (NE_INT_PTX | NE_INT_TXE))
		ne_tx_done_locked(dev, (isr & NE_INT_TXE) != 0);
	if (isr & NE_INT_RXE)
		dev->net_dev.rx_errors++;
	if (isr & NE_INT_OVW)
		ne_rx_overrun_locked(dev);
	if (isr & NE_INT_CNT) {
		dev->st_rx_frame += ne_r8(dev, NE_P0_CNTR0);
		dev->st_rx_crc += ne_r8(dev, NE_P0_CNTR1);
		dev->st_missed += ne_r8(dev, NE_P0_CNTR2);
	}
	if ((isr & (NE_INT_PRX | NE_INT_RXE)) && (dev->imr & NE_IMR_NAPI)) {
		dev->imr &= (uint8_t)~NE_IMR_NAPI;
		ne_w8(dev, NE_P0_IMR, dev->imr);
		sched = 1;
	}
	spin_unlock_irqrestore(&dev->tx_lock, lflags);
	if (sched || (isr & (NE_INT_PRX | NE_INT_RXE)))
		napi_schedule(&dev->napi);
	return 1;
}

// ============================================================================
// Receive filter, statistics, watchdog
// ============================================================================
static void ne2k_set_rx_mode(net_device_t *ndev)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

static void ne2k_get_stats64(net_device_t *ndev, struct net_device_stats64 *s)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)ndev->driver_data;

	s->rx_frame_errors = dev->st_rx_frame;
	s->rx_crc_errors = dev->st_rx_crc;
	s->rx_missed_errors = dev->st_missed;
	s->rx_over_errors = dev->st_overruns;
	s->rx_errors += dev->st_rx_frame + dev->st_rx_crc;
}

static void ne2k_tx_timeout(net_device_t *ndev)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
}

// Network housekeeping softirq, CPU 0, ~10 Hz and when kicked.
static void ne2k_watchdog(net_device_t *ndev)
{
	ne2k_dev_t *dev = (ne2k_dev_t *)ndev->driver_data;
	uint64_t f, now = timer_ticks();

	if (!dev || !g_ne2k_initialized)
		return;
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL)) {
		uint8_t mar[8], rcr;

		ne_compute_filter(dev, mar, &rcr);
		spin_lock_irqsave(&dev->tx_lock, &f);
		ne_set_filter_locked(dev, mar, rcr);
		spin_unlock_irqrestore(&dev->tx_lock, f);
	}
	spin_lock_irqsave(&dev->tx_lock, &f);
	if (dev->tx_active >= 0 && dev->tx_stamp &&
	    now - dev->tx_stamp > net_ms_to_ticks(5000))
		__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&dev->reset_pending, 0, __ATOMIC_ACQ_REL)) {
		/* Transmit timeout: reset and reprogram the chip; the
		 * frames in its buffers and ring are dropped. */
		kprintf("NE2K: transmit timed out, resetting the chip\n");
		dev->net_dev.tx_errors += (dev->tx_len[0] ? 1 : 0) +
					  (dev->tx_len[1] ? 1 : 0);
		(void)ne_reset(dev);
		ne_hw_init(dev);
		if (netif_queue_stopped(&dev->net_dev))
			netif_wake_queue(&dev->net_dev);
	}
	spin_unlock_irqrestore(&dev->tx_lock, f);
}

// ============================================================================
// Resolve I/O BAR (RTL8029 BAR0 is PMIO, 32 bytes wide)
// ============================================================================
static int ne_resolve_iobase(ne2k_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];
	if (!(bar0 & 1)) {
		kprintf("NE2K: BAR0 is not I/O space (bar0=0x%x)\n", bar0);
		return -1;
	}
	dev->io_base = (uint16_t)(bar0 & 0xFFFC);
	return 0;
}

// ============================================================================
// Driver entry point
// ============================================================================
void ne2k_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		const ne2k_id_t *match =
			ne2k_lookup(devs[i].vendor_id, devs[i].device_id);
		if (!match)
			continue;

		kprintf("NE2K: Found %s (PCI %02x:%02x.%x)\n", match->name,
			devs[i].bus, devs[i].device, devs[i].function);

		ne2k_dev_t *dev = &g_ne2k;
		dev->pci_dev = &devs[i];
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("ne2k_tx");

		pci_enable_busmaster_mem(dev->pci_dev);

		// pci_enable_busmaster_mem() only sets MEM (bit 1) + Bus Master
		// (bit 2).  RTL8029 BAR0 is I/O space, so we must also set
		// I/O Space Enable (bit 0) — without it, inb() returns 0xFF.
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

		if (ne_resolve_iobase(dev) < 0)
			return;
		kprintf("NE2K: I/O base 0x%x\n", dev->io_base);

		if (ne_reset(dev) < 0)
			return;

		ne_read_mac(dev);
		kprintf("NE2K: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		/* Chip stopped, interrupts masked until the device is
		 * registered (ne_hw_init programs and starts it). */
		ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
		ne_w8(dev, NE_P0_IMR, 0);
		ne_w8(dev, NE_P0_ISR, 0xFF);
		dev->imr = 0;

		// Resolve IRQ via ACPI _PRT first; chip has no MSI capability.
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
				kprintf("NE2K: ACPI _PRT resolved INT%c -> GSI %u\n",
					'A' + acpi_pin, gsi);
			}
		}
		if (irq == 0xFF) {
			irq = dev->pci_dev->interrupt_line;
			if (irq != 0xFF && irq <= 23) {
				kprintf("NE2K: ACPI _PRT lookup failed, falling back to "
					"PCI interrupt_line = %d\n",
					irq);
			} else {
				kprintf("NE2K: WARNING: no valid IRQ\n");
			}
		}
		kprintf("NE2K: Using legacy IRQ %d\n", irq);
		g_ne2k_legacy_irq = irq;

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
				kprintf("NE2K: cleared PCI Command INTx Disable bit\n");
			}
		}

		if (irq <= 23) {
			uint8_t vector = 32 + irq;
			ioapic_configure_legacy_irq(irq, vector,
						    IOAPIC_POLARITY_LOW,
						    IOAPIC_TRIGGER_LEVEL);
		}

		// Initialise net_device fields BEFORE enabling device IRQs.
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("ne2k");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->link_up = 1;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, ne2k_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_ne2k_initialized = 1;

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
		dev->net_dev.xmit = ne2k_xmit;
		dev->net_dev.link_status = ne2k_link_status;
		dev->net_dev.shutdown = ne2k_shutdown;
		dev->net_dev.watchdog = ne2k_watchdog;
		dev->net_dev.tx_timeout = ne2k_tx_timeout;
		dev->net_dev.set_rx_mode = ne2k_set_rx_mode;
		dev->net_dev.get_stats64 = ne2k_get_stats64;

		net_register(&dev->net_dev);

		// Program and start the chip, interrupts on
		{
			uint64_t f;

			spin_lock_irqsave(&dev->tx_lock, &f);
			ne_hw_init(dev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
		}
		kprintf("NE2K: Driver initialized successfully\n");
		return; // first device only
	}
}
