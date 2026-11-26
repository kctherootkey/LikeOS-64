// LikeOS Intel 8255x ("eepro100") NIC Driver
//
// See kernel/dev/nic/eepro100.h for the supported PCI device IDs and a
// summary of the simplified Command Block / Receive Frame Area model
// programmed here.
//
// The driver follows the same "single device" pattern as e1000.c /
// pcnet32.c: one global eepro100_dev_t, one shared IRQ handler dispatched
// from kernel/ke/interrupt.c, MAC read out of the on-chip 93C46-style
// EEPROM, and DMA descriptors allocated from contiguous low-physical
// pages.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/eepro100.h>
#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/skb.h>
#include <kernel/dev/nic/nic_util.h>
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

static eepro100_dev_t g_eepro100;
int g_eepro100_initialized = 0;
int g_eepro100_legacy_irq = -1;

// ============================================================================
// CSR access — MMIO when available, otherwise BAR1 I/O ports.  Every SCB
// register has the same byte offset in both spaces, so we can route the
// access through a single inline helper.
// ============================================================================
static inline uint8_t e100_r8(eepro100_dev_t *d, uint16_t off)
{
	if (d->use_io)
		return inb(d->io_base + off);
	return *(volatile uint8_t *)(d->mmio_base + off);
}
static inline uint16_t e100_r16(eepro100_dev_t *d, uint16_t off)
{
	if (d->use_io)
		return inw(d->io_base + off);
	return *(volatile uint16_t *)(d->mmio_base + off);
}
static inline uint32_t e100_r32(eepro100_dev_t *d, uint16_t off)
{
	if (d->use_io)
		return inl(d->io_base + off);
	return *(volatile uint32_t *)(d->mmio_base + off);
}
static inline void e100_w8(eepro100_dev_t *d, uint16_t off, uint8_t v)
{
	if (d->use_io) {
		outb(d->io_base + off, v);
		return;
	}
	*(volatile uint8_t *)(d->mmio_base + off) = v;
}
static inline void e100_w16(eepro100_dev_t *d, uint16_t off, uint16_t v)
{
	if (d->use_io) {
		outw(d->io_base + off, v);
		return;
	}
	*(volatile uint16_t *)(d->mmio_base + off) = v;
}
static inline void e100_w32(eepro100_dev_t *d, uint16_t off, uint32_t v)
{
	if (d->use_io) {
		outl(d->io_base + off, v);
		return;
	}
	*(volatile uint32_t *)(d->mmio_base + off) = v;
}

// ============================================================================
// PM-Timer microsecond delay (PIT-based fallback if PM Timer unavailable)
// ============================================================================
static void e100_delay_us(uint32_t us)
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
// Wait for SCBCmd to drain.  The chip clears SCBCmd after it has accepted
// the command; we must block on this before issuing the next CU/RU op.
// ============================================================================
static int e100_wait_scb(eepro100_dev_t *d)
{
	for (int i = 0; i < 10000; i++) {
		if (e100_r8(d, EEPRO100_SCB_CMD) == 0)
			return 0;
		e100_delay_us(10);
	}
	kprintf("eepro100: SCB command timeout (cmd=0x%x)\n",
		e100_r8(d, EEPRO100_SCB_CMD));
	return -1;
}

// ============================================================================
// 93C46-style EEPROM bit-banged read (16-bit word, 6-bit address)
// ============================================================================
#define EEP_CS 0x02
#define EEP_SK 0x01
#define EEP_DI 0x04
#define EEP_DO 0x08

static void e100_eep_pulse(eepro100_dev_t *d, uint16_t v)
{
	e100_w16(d, EEPRO100_SCB_EEPROM, v | EEP_SK);
	e100_delay_us(2);
	e100_w16(d, EEPRO100_SCB_EEPROM, v);
	e100_delay_us(2);
}

static uint16_t e100_eeprom_read(eepro100_dev_t *d, uint8_t addr)
{
	// Drop CS, then raise CS to begin a transaction.
	e100_w16(d, EEPRO100_SCB_EEPROM, 0);
	e100_delay_us(2);
	e100_w16(d, EEPRO100_SCB_EEPROM, EEP_CS);
	e100_delay_us(2);

	// Shift in: start bit (1), opcode READ (10), 6-bit address.
	uint16_t cmd = (1 << 8) | (0x2 << 6) | (addr & 0x3F); // 9 bits total
	for (int i = 8; i >= 0; i--) {
		uint16_t bit = ((cmd >> i) & 1) ? EEP_DI : 0;
		e100_w16(d, EEPRO100_SCB_EEPROM, EEP_CS | bit);
		e100_delay_us(2);
		e100_eep_pulse(d, EEP_CS | bit);
	}

	// Shift out 16 data bits MSB-first.
	uint16_t val = 0;
	for (int i = 0; i < 16; i++) {
		e100_w16(d, EEPRO100_SCB_EEPROM, EEP_CS | EEP_SK);
		e100_delay_us(2);
		uint16_t r = e100_r16(d, EEPRO100_SCB_EEPROM);
		val = (val << 1) | ((r & EEP_DO) ? 1 : 0);
		e100_w16(d, EEPRO100_SCB_EEPROM, EEP_CS);
		e100_delay_us(2);
	}

	// Drop CS to end transaction.
	e100_w16(d, EEPRO100_SCB_EEPROM, 0);
	e100_delay_us(2);
	return val;
}

// ============================================================================
// PORT software reset.  After this the chip is in the "post-reset" state:
// CU and RU are both idle, statistics zeroed, MII registers default.
// ============================================================================
static void e100_port_reset(eepro100_dev_t *d)
{
	// PORT is a 32-bit register: low 2 bits select the operation, rest is
	// a DMA address used by the selftest path (we don't use it).
	e100_w32(d, EEPRO100_SCB_PORT, EEPRO100_PORT_SOFT_RESET);
	e100_delay_us(20); // spec says 10us; be generous
	e100_w32(d, EEPRO100_SCB_PORT, EEPRO100_PORT_SEL_RESET);
	e100_delay_us(20);
}

// ============================================================================
// Helpers for the per-CB / per-RFD blocks inside the ring DMA region
// ============================================================================
static inline eepro100_cb_t *e100_tx_cb(eepro100_dev_t *d, uint16_t i)
{
	return (eepro100_cb_t *)(d->tx_blocks +
				 (uint32_t)i * EEPRO100_TX_BLOCK_SIZE);
}
static inline uint64_t e100_tx_cb_phys(eepro100_dev_t *d, uint16_t i)
{
	return d->tx_blocks_phys + (uint32_t)i * EEPRO100_TX_BLOCK_SIZE;
}
static inline uint8_t *e100_tx_payload(eepro100_dev_t *d, uint16_t i)
{
	return (uint8_t *)e100_tx_cb(d, i) + sizeof(eepro100_cb_t);
}

static inline eepro100_rfd_t *e100_rx_rfd(eepro100_dev_t *d, uint16_t i)
{
	return (eepro100_rfd_t *)(d->rx_blocks +
				  (uint32_t)i * EEPRO100_RX_BLOCK_SIZE);
}
static inline uint64_t e100_rx_rfd_phys(eepro100_dev_t *d, uint16_t i)
{
	return d->rx_blocks_phys + (uint32_t)i * EEPRO100_RX_BLOCK_SIZE;
}
static inline uint8_t *e100_rx_payload(eepro100_dev_t *d, uint16_t i)
{
	return (uint8_t *)e100_rx_rfd(d, i) + sizeof(eepro100_rfd_t);
}

// ============================================================================
// Allocate TX CB ring + RX RFD ring (each in one contiguous block of
// low-physical memory so that 32-bit `link` fields can hold the addresses).
// ============================================================================

/* Both rings in their initial state: every CB a NOP linked into a circle,
 * every RFD armed for the chip (the last one with EL, so the RU stops
 * there rather than overrunning frames not taken yet). */
static void e100_rings_reset(eepro100_dev_t *d)
{
	for (uint16_t i = 0; i < EEPRO100_NUM_TX_DESC; i++) {
		eepro100_cb_t *cb = e100_tx_cb(d, i);
		cb->status = 0;
		cb->command = EEPRO100_CMD_NOP;
		cb->link = (uint32_t)e100_tx_cb_phys(
			d, (i + 1) % EEPRO100_NUM_TX_DESC);
		cb->tbd_addr = 0xFFFFFFFFu;
		cb->tcb_bytes = 0;
		cb->tx_threshold = 0xE0;
		cb->tbd_count = 0;
		d->tx_stamp[i] = 0;
	}
	d->tx_prod = 0;
	d->tx_cons = 0;
	d->cu_started = 0;

	for (uint16_t i = 0; i < EEPRO100_NUM_RX_DESC; i++) {
		eepro100_rfd_t *rfd = e100_rx_rfd(d, i);
		rfd->status = 0;
		rfd->command =
			(i == EEPRO100_NUM_RX_DESC - 1) ? EEPRO100_CMD_EL : 0;
		rfd->link = (uint32_t)e100_rx_rfd_phys(
			d, (i + 1) % EEPRO100_NUM_RX_DESC);
		rfd->rx_buf_addr = 0; // 0 = buffer immediately after header
		rfd->count = 0;
		rfd->size = EEPRO100_RX_BUF_SIZE;
	}
	d->rx_cur = 0;
	__asm__ volatile("mfence" ::: "memory");
}

static int e100_alloc_rings(eepro100_dev_t *d)
{
	uint32_t tx_bytes =
		(uint32_t)EEPRO100_NUM_TX_DESC * EEPRO100_TX_BLOCK_SIZE;
	uint32_t rx_bytes =
		(uint32_t)EEPRO100_NUM_RX_DESC * EEPRO100_RX_BLOCK_SIZE;

	uint32_t tx_pages = (tx_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	uint32_t rx_pages = (rx_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t tx_phys = mm_allocate_contiguous_pages(tx_pages);
	uint64_t rx_phys = mm_allocate_contiguous_pages(rx_pages);
	if (!tx_phys || !rx_phys || (tx_phys >> 32) || (rx_phys >> 32)) {
		kprintf("eepro100: failed to allocate rings (or above 4GB)\n");
		return -1;
	}

	d->tx_blocks = (uint8_t *)phys_to_virt(tx_phys);
	d->tx_blocks_phys = tx_phys;
	d->rx_blocks = (uint8_t *)phys_to_virt(rx_phys);
	d->rx_blocks_phys = rx_phys;

	// Zero everything (descriptor fields and buffer area).
	mm_memset(d->tx_blocks, 0, tx_bytes);
	mm_memset(d->rx_blocks, 0, rx_bytes);
	e100_rings_reset(d);
	return 0;
}

// ============================================================================
// Issue a single one-shot CB (CmdConfigure / CmdIASetup) and wait for it
// to complete, with the CU idle (bring-up and reset only).  We borrow
// tx_blocks slot 0 for this — it is re-armed as a NOP afterwards, before
// normal TX traffic begins, and the CU is left idle (EL): the first frame
// starts it with CU_START.
// ============================================================================
static int e100_issue_setup_cb(eepro100_dev_t *d, uint16_t cmd_type,
			       const uint8_t *payload, uint32_t payload_len)
{
	eepro100_cb_t *cb = e100_tx_cb(d, 0);

	// For CmdConfigure / CmdIASetup the chip reads the payload starting
	// at CB+8 (overlapping tbd_addr / tcb_bytes / tx_threshold / tbd_count),
	// *not* at CB+16 like CmdTx simplified-mode data does.  See QEMU's
	// hw/net/eepro100.c:action_command() — pci_dma_read(cb_address + 8, ...).
	uint8_t *dst = (uint8_t *)cb + 8;
	for (uint32_t i = 0; i < payload_len; i++)
		dst[i] = payload[i];

	cb->status = 0;
	cb->command = EEPRO100_CMD_EL | cmd_type;
	cb->link = (uint32_t)e100_tx_cb_phys(d, 0); // self-loop (EL stops CU)
	__asm__ volatile("mfence" ::: "memory");

	// Hand to the CU
	if (e100_wait_scb(d) < 0)
		return -1;
	e100_w32(d, EEPRO100_SCB_POINTER, (uint32_t)e100_tx_cb_phys(d, 0));
	e100_w8(d, EEPRO100_SCB_CMD, EEPRO100_CU_START);

	// Poll for CB completion (status.C = 1)
	for (int i = 0; i < 100000; i++) {
		if (((volatile eepro100_cb_t *)cb)->status &
		    EEPRO100_STATUS_C) {
			// Acknowledge any CU bits the chip set
			uint8_t st = e100_r8(d, EEPRO100_SCB_ACK);
			if (st)
				e100_w8(d, EEPRO100_SCB_ACK, st);
			// Restore slot 0 to a NOP so normal TX can reuse it later
			cb->status = 0;
			cb->command = EEPRO100_CMD_NOP;
			cb->tbd_addr = 0xFFFFFFFFu;
			cb->tcb_bytes = 0;
			cb->tx_threshold = 0xE0;
			cb->tbd_count = 0;
			cb->link = (uint32_t)e100_tx_cb_phys(d, 1);
			return 0;
		}
		e100_delay_us(20);
	}
	kprintf("eepro100: setup CB timeout (cmd=0x%x status=0x%x)\n", cmd_type,
		cb->status);
	return -1;
}

// ============================================================================
// Configure RU base = CU base = 0 (we use absolute physical addresses for
// every CB.link / RFD.link).
// ============================================================================
static int e100_load_bases(eepro100_dev_t *d)
{
	if (e100_wait_scb(d) < 0)
		return -1;
	e100_w32(d, EEPRO100_SCB_POINTER, 0);
	e100_w8(d, EEPRO100_SCB_CMD, EEPRO100_CU_LOAD_BASE);
	if (e100_wait_scb(d) < 0)
		return -1;
	e100_w32(d, EEPRO100_SCB_POINTER, 0);
	e100_w8(d, EEPRO100_SCB_CMD, EEPRO100_RU_LOAD_BASE);
	if (e100_wait_scb(d) < 0)
		return -1;
	return 0;
}

// ============================================================================
// 22-byte CmdConfigure payload.  The values below are the well-known
// "safe" defaults for the 8255x; only a handful of bits matter to QEMU's
// eepro100 emulation:
//
//   byte[0]   = 22       (configuration byte count)
//   byte[7]   bit0 = 0   (don't discard short frames — DHCP/ARP need them)
//   byte[8]   bit7 = 0   (CSMA enabled)
//   byte[15]  bit0 = 0   (not promiscuous)
//   byte[18]  bit2 = 0   (don't transfer received CRC)
//   byte[18]  bit3 = 0   (drop frames larger than 1518 bytes)
//   byte[20]  bit6 = 0   (no Multiple-IA mode)
//   byte[21]  bit3 = 1   (Multicast All — no hash filter programmed)
// ============================================================================
static const uint8_t e100_default_config[22] = {
	/* 0  */ 0x16,
	/* 1  */ 0x08,
	/* 2  */ 0x00,
	/* 3  */ 0x00,
	/* 4  */ 0x00,
	/* 5  */ 0x00,
	/* 6  */ 0x32,
	/* 7  */ 0x00,
	/* 8  */ 0x01,
	/* 9  */ 0x00,
	/* 10 */ 0x2E,
	/* 11 */ 0x00,
	/* 12 */ 0x60,
	/* 13 */ 0x00,
	/* 14 */ 0x00,
	/* 15 */ 0xC8,
	/* 16 */ 0x00,
	/* 17 */ 0x40,
	/* 18 */ 0xF2,
	/* 19 */ 0x80,
	/* 20 */ 0x3F,
	/* 21 */ 0x08,
};

/* Bring the reset chip up: bases, Configure, IA Setup, then the receive
 * unit on the RFD ring.  The CU stays idle until the first frame. */
static int e100_hw_setup(eepro100_dev_t *d)
{
	if (e100_load_bases(d) < 0) {
		kprintf("eepro100: load-base sequence failed\n");
		return -1;
	}
	if (e100_issue_setup_cb(d, EEPRO100_CMD_CONFIGURE, e100_default_config,
				sizeof(e100_default_config)) < 0) {
		kprintf("eepro100: CmdConfigure failed\n");
		return -1;
	}
	if (e100_issue_setup_cb(d, EEPRO100_CMD_IASETUP, d->mac_addr, 6) < 0) {
		kprintf("eepro100: CmdIASetup failed\n");
		return -1;
	}
	return 0;
}

static void e100_ru_start(eepro100_dev_t *d)
{
	if (e100_wait_scb(d) == 0) {
		e100_w32(d, EEPRO100_SCB_POINTER,
			 (uint32_t)e100_rx_rfd_phys(d, d->rx_cur));
		e100_w8(d, EEPRO100_SCB_CMD, EEPRO100_RU_START);
	}
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================
#define EEPRO100_TX_WAKE_THRESH 4

static uint32_t e100_tx_unused(const eepro100_dev_t *d)
{
	return nic_ring_unused(d->tx_prod, d->tx_cons, EEPRO100_NUM_TX_DESC);
}

/* Reclaim completed CBs (status.C), oldest first.  Caller holds tx_lock. */
static void e100_tx_clean_locked(eepro100_dev_t *d)
{
	while (d->tx_cons != d->tx_prod) {
		volatile eepro100_cb_t *cb = e100_tx_cb(d, d->tx_cons);
		uint16_t st = cb->status;

		if (!(st & EEPRO100_STATUS_C))
			break;
		if (!(st & EEPRO100_STATUS_OK)) {
			d->net_dev.tx_errors++;
			d->st_tx_bad++;
		}
		d->tx_stamp[d->tx_cons] = 0;
		d->tx_cons = (uint16_t)((d->tx_cons + 1) % EEPRO100_NUM_TX_DESC);
	}
	if (netif_queue_stopped(&d->net_dev) &&
	    e100_tx_unused(d) >= EEPRO100_TX_WAKE_THRESH)
		netif_wake_queue(&d->net_dev);
}

/* Queue the CB at tx_prod (filled by the caller except command and link)
 * and get the CU to it: the new CB carries S (the CU suspends after it)
 * and I (CX interrupt on completion); the S of the CB before it is
 * cleared, so a CU still working there runs on into the new one, and a
 * CU already suspended there is resumed.  The very first CB after the
 * setup commands (CU idle) goes with CU_START.  Caller holds tx_lock (the
 * SCB command register is shared with the receive unit's commands). */
static int e100_queue_cb_locked(eepro100_dev_t *d, uint16_t type)
{
	uint16_t slot = d->tx_prod;
	volatile eepro100_cb_t *cb = e100_tx_cb(d, slot);
	volatile eepro100_cb_t *prev = e100_tx_cb(
		d,
		(uint16_t)((slot + EEPRO100_NUM_TX_DESC - 1) %
			   EEPRO100_NUM_TX_DESC));

	cb->status = 0;
	cb->link = (uint32_t)e100_tx_cb_phys(
		d, (uint16_t)((slot + 1) % EEPRO100_NUM_TX_DESC));
	cb->command = (uint16_t)(type | EEPRO100_CMD_S | EEPRO100_CMD_I);
	__asm__ volatile("mfence" ::: "memory");
	if (d->cu_started)
		prev->command &= (uint16_t)~EEPRO100_CMD_S;
	__asm__ volatile("mfence" ::: "memory");

	if (e100_wait_scb(d) < 0)
		return -1;
	if (!d->cu_started) {
		e100_w32(d, EEPRO100_SCB_POINTER,
			 (uint32_t)e100_tx_cb_phys(d, slot));
		e100_w8(d, EEPRO100_SCB_CMD, EEPRO100_CU_START);
		d->cu_started = 1;
	} else {
		e100_w8(d, EEPRO100_SCB_CMD, EEPRO100_CU_RESUME);
	}
	{
		uint64_t now = timer_ticks();

		d->tx_stamp[slot] = now ? now : 1;
	}
	d->tx_prod = (uint16_t)((slot + 1) % EEPRO100_NUM_TX_DESC);
	return 0;
}

static int eepro100_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	eepro100_dev_t *d = (eepro100_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;

	BUG_ON(d == NULL);
	if (len == 0 || len > EEPRO100_TX_BUF_SIZE) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}
	uint32_t out_len = (len < 60) ? 60 : len;

	spin_lock_irqsave(&d->tx_lock, &f);
	if (e100_tx_unused(d) == 0) {
		e100_tx_clean_locked(d);
		if (e100_tx_unused(d) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&d->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}

	uint16_t slot = d->tx_prod;
	volatile eepro100_cb_t *cb = e100_tx_cb(d, slot);
	uint8_t *buf = e100_tx_payload(d, slot);

	mm_memcpy(buf, skb->data, len);
	if (out_len > len)
		mm_memset(buf + len, 0, out_len - len);
	// Simplified mode: tbd_addr = 0xFFFFFFFF, the data follows the CB
	cb->tbd_addr = 0xFFFFFFFFu;
	cb->tcb_bytes = (uint16_t)out_len;
	cb->tx_threshold = 0xE0;
	cb->tbd_count = 0;
	if (e100_queue_cb_locked(d, EEPRO100_CMD_TX) < 0) {
		ndev->tx_errors++;
		__atomic_store_n(&d->reset_pending, 1, __ATOMIC_RELEASE);
	} else {
		ndev->tx_packets++;
		ndev->tx_bytes += out_len;
	}
	if (e100_tx_unused(d) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&d->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

// ============================================================================
// Link status — the PHY's BMSR via the MDI window.  For QEMU this always
// reports link-up while a netdev is attached.
// ============================================================================
static int e100_mdi_link(eepro100_dev_t *d)
{
	int up = -1;

	// BMSR link status is latched low: the second read is the state now
	for (int pass = 0; pass < 2; pass++) {
		// MDI Read | PHY 1 | reg 1 (BMSR)
		e100_w32(d, EEPRO100_SCB_MDI,
			 (2u << 26) | (1u << 21) | (1u << 16));
		up = -1;
		for (int i = 0; i < 1000; i++) {
			uint32_t r = e100_r32(d, EEPRO100_SCB_MDI);
			if (r & (1u << 28)) { // R bit (operation ready)
				up = (r & (1u << 2)) ? 1 : 0;
				break;
			}
			e100_delay_us(10);
		}
	}
	return up;
}

/* Link supervision (watchdog): the carrier follows the PHY once it has
 * reported a link at all. */
static void e100_link_check(eepro100_dev_t *d)
{
	int now_up = e100_mdi_link(d);

	if (now_up < 0 || now_up == d->link_up)
		return;
	if (now_up)
		d->link_seen = 1;
	d->link_up = now_up;
	kprintf("eepro100: Link %s\n", now_up ? "UP" : "DOWN");
	if (now_up) {
		netif_carrier_on(&d->net_dev);
	} else {
		dhcp_invalidate(&d->net_dev);
		if (d->link_seen)
			netif_carrier_off(&d->net_dev);
	}
}

static int eepro100_link_status(net_device_t *ndev)
{
	eepro100_dev_t *d = (eepro100_dev_t *)ndev->driver_data;

	// the watchdog owns the MDI window; this asks it to look now
	d->link_poll_tick = 0;
	net_housekeeping_kick();
	return d->link_up;
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the 8255x
// datasheet, masking the SCB interrupt byte and issuing a PORT software
// reset returns the chip to the post-reset state with RU/CU idle, all
// causes acked, and DMA halted.  Then drop PCI Command.BME so the root
// complex discards anything still in flight.
static void eepro100_shutdown(net_device_t *ndev)
{
	eepro100_dev_t *d = (eepro100_dev_t *)ndev->driver_data;
	if (!d)
		return;
	if (!d->mmio_base && !d->io_base)
		return;

	// Mask all interrupt sources (SCB INTMASK bit 0 = M = mask all).
	e100_w8(d, EEPRO100_SCB_INTMASK, 0x01);

	// PORT software reset — idles RU and CU, halts DMA, clears causes.
	e100_w32(d, EEPRO100_SCB_PORT, EEPRO100_PORT_SOFT_RESET);

	if (d->pci_dev) {
		const pci_device_t *pdev = d->pci_dev;
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
static volatile int e100_rnr_pending; // the RU ran out of RFDs

static int e100_rx_clean(eepro100_dev_t *d, int budget)
{
	net_device_t *nd = &d->net_dev;
	int work = 0;

	while (work < budget) {
		eepro100_rfd_t *rfd = e100_rx_rfd(d, d->rx_cur);
		// The chip writes the RFD behind the compiler's back: read
		// status.C through volatile, and read count only after it.
		uint16_t st = ((volatile eepro100_rfd_t *)rfd)->status;

		if (!(st & EEPRO100_STATUS_C))
			break;
		__asm__ volatile("" ::: "memory");

		uint16_t len = ((volatile eepro100_rfd_t *)rfd)->count &
			       EEPRO100_RFD_COUNT_MASK;
		if (!(st & EEPRO100_STATUS_OK)) {
			// the chip flagged the frame (CRC, alignment, ...)
			nd->rx_errors++;
			d->st_rx_bad++;
		} else if (len == 0 || len > EEPRO100_RX_BUF_SIZE) {
			nd->rx_errors++;
			d->st_rx_long++;
		} else {
			sk_buff_t *skb = skb_alloc(len);

			if (skb) {
				const uint8_t *b = e100_rx_payload(d, d->rx_cur);

				skb->dev = nd;
				mm_memcpy(skb_append(skb, len), b, len);
				nd->rx_packets++;
				nd->rx_bytes += len;
				if (b[0] & 1)
					nd->multicast++;
				napi_gro_receive(&d->napi, skb);
			} else {
				nd->rx_dropped++;
			}
		}

		// Re-arm this RFD: clear status/count, place EL on the newly-
		// free descriptor so the chip stops walking past us if we fall
		// behind, and clear EL on the previous EL-holder.
		uint16_t prev = (d->rx_cur + EEPRO100_NUM_RX_DESC - 1) %
				EEPRO100_NUM_RX_DESC;
		rfd->status = 0;
		rfd->count = 0;
		rfd->command = EEPRO100_CMD_EL;
		__asm__ volatile("mfence" ::: "memory");
		e100_rx_rfd(d, prev)->command = 0;
		__asm__ volatile("mfence" ::: "memory");

		d->rx_cur = (d->rx_cur + 1) % EEPRO100_NUM_RX_DESC;
		work++;
	}
	return work;
}

static int eepro100_poll(struct napi_struct *n, int budget)
{
	eepro100_dev_t *d = (eepro100_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	spin_lock_irqsave(&d->tx_lock, &f);
	e100_tx_clean_locked(d);
	spin_unlock_irqrestore(&d->tx_lock, f);
	work = e100_rx_clean(d, budget);
	if (__atomic_exchange_n(&e100_rnr_pending, 0, __ATOMIC_ACQ_REL)) {
		/* The RU ran out of RFDs and stopped: the ones just freed
		 * are armed again -- restart it at the first of them. */
		spin_lock_irqsave(&d->tx_lock, &f);
		e100_ru_start(d);
		spin_unlock_irqrestore(&d->tx_lock, f);
	}
	if (work < budget && napi_complete_done(n, work))
		e100_w8(d, EEPRO100_SCB_INTMASK, 0x00);
	return work;
}

// ============================================================================
// Top-half IRQ handler — dispatched from kernel/ke/interrupt.c
// ============================================================================
/* Returns 1 when the interrupt was this device's, 0 otherwise (shared
 * legacy line); the dispatcher acknowledges the local APIC.  Everything
 * goes to the NAPI poll, with the device's interrupts masked until it is
 * done. */
int eepro100_irq_handler(void)
{
	if (!g_eepro100_initialized)
		return 0;

	// Entropy from IRQ timing
	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	eepro100_dev_t *d = &g_eepro100;
	BUG_ON(d == NULL);

	// The STAT/ACK bits (CX/FR/CNA/RNR/MDI/SWI) live in the SCBAck byte
	// (offset 1), NOT the SCBStatus byte (offset 0, which holds CUS/RUS
	// state).
	uint8_t st = e100_r8(d, EEPRO100_SCB_ACK);
	if (!st || st == 0xFF)
		return 0; // not ours (shared legacy IRQ) or already acked

	// Acknowledge every status bit we observed, in one shot.  QEMU's
	// eepro100_acknowledge() ANDs scb_stat with ~SCBAck, so writing the
	// bits back clears them and deasserts INTx.
	e100_w8(d, EEPRO100_SCB_ACK, st);

	if (st & EEPRO100_STAT_RNR) {
		d->st_rnr++;
		d->net_dev.rx_dropped++;
		__atomic_store_n(&e100_rnr_pending, 1, __ATOMIC_RELEASE);
	}
	e100_w8(d, EEPRO100_SCB_INTMASK, EEPRO100_INT_MASK_ALL);
	napi_schedule(&d->napi);
	return 1;
}

// ============================================================================
// Statistics, watchdog
// ============================================================================
static void eepro100_get_stats64(net_device_t *ndev,
				 struct net_device_stats64 *s)
{
	eepro100_dev_t *d = (eepro100_dev_t *)ndev->driver_data;

	s->rx_crc_errors = d->st_rx_bad;
	s->rx_length_errors = d->st_rx_long;
	s->rx_over_errors = d->st_rnr;
}

static void eepro100_tx_timeout(net_device_t *ndev)
{
	eepro100_dev_t *d = (eepro100_dev_t *)ndev->driver_data;

	__atomic_store_n(&d->reset_pending, 1, __ATOMIC_RELEASE);
}

/* Reset and bring the chip up again (a command never completed): the
 * frames in both rings are dropped.  Watchdog context (CPU 0): the poll
 * is not running; tx_lock keeps the transmit hook out. */
static void e100_restart(eepro100_dev_t *d)
{
	uint64_t f;

	spin_lock_irqsave(&d->tx_lock, &f);
	e100_w8(d, EEPRO100_SCB_INTMASK, EEPRO100_INT_MASK_ALL);
	d->net_dev.tx_errors += (uint32_t)((d->tx_prod + EEPRO100_NUM_TX_DESC -
					    d->tx_cons) %
					   EEPRO100_NUM_TX_DESC);
	e100_port_reset(d);
	e100_rings_reset(d);
	if (e100_hw_setup(d) == 0)
		e100_ru_start(d);
	e100_w8(d, EEPRO100_SCB_ACK, 0xFF);
	e100_w8(d, EEPRO100_SCB_INTMASK, 0x00);
	if (netif_queue_stopped(&d->net_dev))
		netif_wake_queue(&d->net_dev);
	spin_unlock_irqrestore(&d->tx_lock, f);
	napi_schedule(&d->napi);
}

// Network housekeeping softirq, CPU 0, ~10 Hz and when kicked.
static void eepro100_watchdog(net_device_t *ndev)
{
	eepro100_dev_t *d = (eepro100_dev_t *)ndev->driver_data;
	uint64_t f, now = timer_ticks();
	int hang = 0;

	if (!d || !g_eepro100_initialized)
		return;
	if (!d->link_poll_tick ||
	    now - d->link_poll_tick >= net_ms_to_ticks(1000)) {
		d->link_poll_tick = now ? now : 1;
		e100_link_check(d);
	}
	spin_lock_irqsave(&d->tx_lock, &f);
	e100_tx_clean_locked(d);
	if (d->tx_cons != d->tx_prod && d->tx_stamp[d->tx_cons] &&
	    now - d->tx_stamp[d->tx_cons] > net_ms_to_ticks(5000))
		hang = 1;
	spin_unlock_irqrestore(&d->tx_lock, f);
	if (hang) {
		kprintf("eepro100: transmit timed out, resetting the chip\n");
		__atomic_store_n(&d->reset_pending, 1, __ATOMIC_RELEASE);
	}
	if (__atomic_exchange_n(&d->reset_pending, 0, __ATOMIC_ACQ_REL))
		e100_restart(d);
}

// ============================================================================
// Map BAR0 (MMIO) or fall back to BAR1 (I/O)
// ============================================================================
static int e100_map_csr(eepro100_dev_t *d)
{
	uint32_t bar0 = d->pci_dev->bar[0];
	if (!(bar0 & 1)) {
		// MMIO (memory-mapped CSR window — 4KB), strictly uncached
		uint64_t phys = bar0 & 0xFFFFFFF0ULL;
		if ((bar0 & 0x06) == 0x04) {
			phys |= ((uint64_t)d->pci_dev->bar[1]) << 32;
		}
		d->mmio_phys = phys;
		uint64_t virt = mm_map_device_mmio(phys, 1);
		if (!virt) {
			kprintf("eepro100: cannot map the CSR BAR\n");
			return -1;
		}
		d->mmio_base = (volatile uint8_t *)virt;
		d->use_io = 0;
		return 0;
	}

	// BAR0 was an I/O port BAR — the spec puts the same SCB layout in BAR1
	// (I/O), so we fall back to that.
	uint32_t bar1 = d->pci_dev->bar[1];
	if (bar1 & 1) {
		d->io_base = (uint16_t)(bar1 & 0xFFFC);
	} else {
		d->io_base = (uint16_t)(bar0 & 0xFFFC);
	}
	d->use_io = 1;
	return 0;
}

// ============================================================================
// Supported PCI device IDs for the Intel 8255x family.  All variants
// are covered by 3 base IDs (0x1229 = 82557/8/9, 0x1209 =
// 82550/1/9ER/82562, 0x2449 = 82801) plus a long tail of OEM/embedded
// IDs used in various server boards, blades and chipset LOMs.
// ============================================================================
typedef struct {
	uint16_t did;
	const char *name;
} eepro100_id_t;

static const eepro100_id_t eepro100_pci_ids[] = {
	{ 0x1029, "i82559 InBusiness" },
	{ 0x1030, "i82559" },
	{ 0x1031, "PRO/100 VE (ICH3)" },
	{ 0x1032, "PRO/100 VE (ICH3) #2" },
	{ 0x1033, "PRO/100 VM (ICH3)" },
	{ 0x1034, "PRO/100 VM (ICH3) #2" },
	{ 0x1035, "82801CAM (ICH3)" },
	{ 0x1036, "82801CAM (ICH3) #2" },
	{ 0x1037, "82801CAM (ICH3) #3" },
	{ 0x1038, "PRO/100 VM (ICH3-M)" },
	{ 0x1039, "PRO/100 VE (ICH4)" },
	{ 0x103A, "PRO/100 VE (ICH4)" },
	{ 0x103B, "PRO/100 VM (ICH4)" },
	{ 0x103C, "PRO/100 VM (ICH4)" },
	{ 0x103D, "PRO/100 VE (ICH4)" },
	{ 0x103E, "PRO/100 VM (ICH4)" },
	{ 0x1050, "PRO/100 VM (ICH5)" },
	{ 0x1051, "PRO/100 VE (ICH5)" },
	{ 0x1052, "PRO/100 VE (ICH5)" },
	{ 0x1053, "PRO/100 VM (ICH5)" },
	{ 0x1054, "PRO/100 VM (ICH5)" },
	{ 0x1055, "PRO/100 VM (ICH5)" },
	{ 0x1056, "PRO/100 VE (ICH5)" },
	{ 0x1057, "PRO/100 VE (ICH5)" },
	{ 0x1059, "82551QM" },
	{ 0x1064, "PRO/100 VE (ICH6)" },
	{ 0x1065, "PRO/100 VE (ICH6)" },
	{ 0x1066, "PRO/100 VE (ICH6)" },
	{ 0x1067, "PRO/100 VE (ICH6)" },
	{ 0x1068, "PRO/100 VE (ICH6)" },
	{ 0x1069, "PRO/100 VE (ICH6)" },
	{ 0x106A, "PRO/100 VE (ICH6)" },
	{ 0x106B, "PRO/100 VE (ICH6)" },
	{ 0x1091, "PRO/100 VE (ICH7)" },
	{ 0x1092, "PRO/100 VE (ICH7)" },
	{ 0x1093, "PRO/100 VE (ICH7)" },
	{ 0x1094, "PRO/100 VE (ICH7)" },
	{ 0x1095, "PRO/100 VE (ICH7)" },
	{ 0x1209, "i82550/82551/82559ER/82562" },
	{ 0x1229, "i82557/82558/82559" },
	{ 0x2449, "i82801 (ICH)" },
	{ 0x2459, "i82801DB (ICH4)" },
	{ 0x245D, "i82801EB (ICH5)" },
	{ 0x27DC, "i82801G (ICH7)" },
	{ 0, NULL },
};

static const eepro100_id_t *eepro100_lookup(uint16_t did)
{
	for (const eepro100_id_t *e = eepro100_pci_ids; e->name; e++) {
		if (e->did == did)
			return e;
	}
	return NULL;
}

// ============================================================================
// Driver entry point
// ============================================================================
void eepro100_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		if (devs[i].vendor_id != EEPRO100_VENDOR_ID)
			continue;
		uint16_t did = devs[i].device_id;
		const eepro100_id_t *match = eepro100_lookup(did);
		if (!match)
			continue;

		const char *name = match->name;
		kprintf("eepro100: Found %s (PCI %02x:%02x.%x)\n", name,
			devs[i].bus, devs[i].device, devs[i].function);

		eepro100_dev_t *d = &g_eepro100;
		d->pci_dev = &devs[i];
		d->device_id = did;
		d->model_name = name;
		d->use_io = 0;
		d->cu_started = 0;
		d->tx_lock = (spinlock_t)SPINLOCK_INIT("eepro100_tx");

		// Enable PCI memory + bus-master.  pci_enable_busmaster_mem()
		// doesn't touch I/O Space Enable; if we end up using BAR1 we
		// also need that bit set.
		pci_enable_busmaster_mem(d->pci_dev);
		{
			uint32_t cmd = pci_cfg_read32(
				d->pci_dev->bus, d->pci_dev->device,
				d->pci_dev->function, 0x04);
			if (!(cmd & 0x1)) {
				pci_cfg_write32(
					d->pci_dev->bus, d->pci_dev->device,
					d->pci_dev->function, 0x04, cmd | 0x1);
			}
		}

		if (e100_map_csr(d) < 0)
			return;

		// Reset the chip into a known state.
		e100_port_reset(d);

		// Read MAC from the on-chip 93C46 EEPROM (3 16-bit words).
		uint16_t w0 = e100_eeprom_read(d, 0);
		uint16_t w1 = e100_eeprom_read(d, 1);
		uint16_t w2 = e100_eeprom_read(d, 2);
		d->mac_addr[0] = w0 & 0xFF;
		d->mac_addr[1] = (w0 >> 8) & 0xFF;
		d->mac_addr[2] = w1 & 0xFF;
		d->mac_addr[3] = (w1 >> 8) & 0xFF;
		d->mac_addr[4] = w2 & 0xFF;
		d->mac_addr[5] = (w2 >> 8) & 0xFF;
		kprintf("eepro100: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			d->mac_addr[0], d->mac_addr[1], d->mac_addr[2],
			d->mac_addr[3], d->mac_addr[4], d->mac_addr[5]);

		// Allocate descriptor rings + buffer area.
		if (e100_alloc_rings(d) < 0)
			return;

		// CU/RU bases (absolute addresses everywhere), Configure,
		// Individual Address Setup.
		if (e100_hw_setup(d) < 0)
			return;

		// Resolve the legacy IRQ via ACPI _PRT (with the bridge-swizzle
		// dance from e1000.c / pcnet32.c) and program the IOAPIC.  No
		// MSI on the 8255x family.
		uint8_t irq = 0xFF;
		uint32_t gsi = 0;
		uint8_t pin = d->pci_dev->interrupt_pin;
		if (pin >= 1 && pin <= 4) {
			uint8_t acpi_pin = pin - 1;
			uint8_t lookup_dev = d->pci_dev->device;
			uint8_t lookup_pin = acpi_pin;
			if (d->pci_dev->bus != 0) {
				const pci_device_t *bridge =
					pci_find_bridge_for_bus(
						d->pci_dev->bus);
				if (bridge) {
					lookup_pin = (acpi_pin +
						      d->pci_dev->device) %
						     4;
					lookup_dev = bridge->device;
				}
			}
			if (acpi_pci_lookup_irq("\\\\_SB_.PCI0", lookup_dev,
						lookup_pin, &gsi) == 0 &&
			    gsi <= 23) {
				irq = (uint8_t)gsi;
				kprintf("eepro100: ACPI _PRT resolved INT%c -> GSI %u\n",
					'A' + acpi_pin, gsi);
			}
		}
		if (irq == 0xFF) {
			irq = d->pci_dev->interrupt_line;
			if (irq != 0xFF && irq <= 23) {
				kprintf("eepro100: ACPI _PRT lookup failed, falling back "
					"to PCI interrupt_line = %d\n",
					irq);
			} else {
				kprintf("eepro100: WARNING: no valid IRQ\n");
			}
		}
		kprintf("eepro100: Using legacy IRQ %d\n", irq);
		g_eepro100_legacy_irq = irq;

		// Clear PCI Command bit 10 (INTx Disable) if firmware left it set.
		{
			uint32_t cmd = pci_cfg_read32(
				d->pci_dev->bus, d->pci_dev->device,
				d->pci_dev->function, 0x04);
			if (cmd & PCI_CMD_INTX_DISABLE) {
				cmd &= ~PCI_CMD_INTX_DISABLE;
				pci_cfg_write32(
					d->pci_dev->bus, d->pci_dev->device,
					d->pci_dev->function, 0x04, cmd);
			}
		}

		if (irq <= 23) {
			uint8_t vector = 32 + irq;
			ioapic_configure_legacy_irq(irq, vector,
						    IOAPIC_POLARITY_LOW,
						    IOAPIC_TRIGGER_LEVEL);
		}

		// Initialise net_device fields BEFORE enabling chip-side
		// interrupts (same rationale as e1000.c — see comment there).
		d->net_dev.lock = (spinlock_t)SPINLOCK_INIT("eepro100");
		d->net_dev.rx_packets = 0;
		d->net_dev.tx_packets = 0;
		d->net_dev.rx_bytes = 0;
		d->net_dev.tx_bytes = 0;
		d->net_dev.rx_errors = 0;
		d->net_dev.tx_errors = 0;
		d->net_dev.rx_dropped = 0;
		d->net_dev.driver_data = d;
		// the poll exists before the handler can be called
		netif_napi_add(&d->net_dev, &d->napi, eepro100_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&d->napi);
		g_eepro100_initialized = 1;

		// Initial link check
		{
			int up = e100_mdi_link(d);

			d->link_up = up > 0;
			d->link_seen = d->link_up;
		}
		kprintf("eepro100: Link %s\n", d->link_up ? "UP" : "DOWN");

		// Wire up net_device_t and register
		d->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			d->net_dev.mac_addr[m] = d->mac_addr[m];
		d->net_dev.mtu = NET_MTU_DEFAULT;
		d->net_dev.ip_addr = 0;
		d->net_dev.netmask = 0;
		d->net_dev.gateway = 0;
		d->net_dev.dns_server = 0;
		d->net_dev.send = NULL;
		d->net_dev.xmit = eepro100_xmit;
		d->net_dev.link_status = eepro100_link_status;
		d->net_dev.shutdown = eepro100_shutdown;
		d->net_dev.watchdog = eepro100_watchdog;
		d->net_dev.tx_timeout = eepro100_tx_timeout;
		d->net_dev.get_stats64 = eepro100_get_stats64;
		d->link_poll_tick = timer_ticks();

		net_register(&d->net_dev);

		// Hand the RX RFD ring to the chip and unmask SCB interrupts
		// (bit 0 = Mask-All; the per-event bits are clear too).
		{
			uint64_t f;

			spin_lock_irqsave(&d->tx_lock, &f);
			e100_ru_start(d);
			spin_unlock_irqrestore(&d->tx_lock, f);
		}
		e100_w8(d, EEPRO100_SCB_INTMASK, 0x00);
		kprintf("eepro100: Driver initialized successfully\n");
		return;
	}
}
