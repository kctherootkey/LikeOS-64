// LikeOS-64 Intel 8255x ("eepro100") NIC Driver
//
// See include/kernel/eepro100.h for the supported PCI device IDs and a
// summary of the simplified Command Block / Receive Frame Area model
// programmed here.
//
// The driver follows the same "single device" pattern as e1000.c /
// pcnet32.c: one global eepro100_dev_t, one shared IRQ handler dispatched
// from kernel/ke/interrupt.c, MAC read out of the on-chip 93C46-style
// EEPROM, and DMA descriptors allocated from contiguous low-physical
// pages.

#include "../../../include/kernel/eepro100.h"
#include "../../../include/kernel/net.h"
#include "../../../include/kernel/pci.h"
#include "../../../include/kernel/memory.h"
#include "../../../include/kernel/console.h"
#include "../../../include/kernel/interrupt.h"
#include "../../../include/kernel/slab.h"
#include "../../../include/kernel/lapic.h"
#include "../../../include/kernel/ioapic.h"
#include "../../../include/kernel/acpi.h"
#include "../../../include/kernel/timer.h"

static eepro100_dev_t g_eepro100;
int g_eepro100_initialized = 0;
int g_eepro100_legacy_irq  = -1;

// ============================================================================
// CSR access — MMIO when available, otherwise BAR1 I/O ports.  Every SCB
// register has the same byte offset in both spaces, so we can route the
// access through a single inline helper.
// ============================================================================
static inline uint8_t e100_r8(eepro100_dev_t* d, uint16_t off) {
    if (d->use_io) return inb(d->io_base + off);
    return *(volatile uint8_t*)(d->mmio_base + off);
}
static inline uint16_t e100_r16(eepro100_dev_t* d, uint16_t off) {
    if (d->use_io) return inw(d->io_base + off);
    return *(volatile uint16_t*)(d->mmio_base + off);
}
static inline uint32_t e100_r32(eepro100_dev_t* d, uint16_t off) {
    if (d->use_io) return inl(d->io_base + off);
    return *(volatile uint32_t*)(d->mmio_base + off);
}
static inline void e100_w8(eepro100_dev_t* d, uint16_t off, uint8_t v) {
    if (d->use_io) { outb(d->io_base + off, v); return; }
    *(volatile uint8_t*)(d->mmio_base + off) = v;
}
static inline void e100_w16(eepro100_dev_t* d, uint16_t off, uint16_t v) {
    if (d->use_io) { outw(d->io_base + off, v); return; }
    *(volatile uint16_t*)(d->mmio_base + off) = v;
}
static inline void e100_w32(eepro100_dev_t* d, uint16_t off, uint32_t v) {
    if (d->use_io) { outl(d->io_base + off, v); return; }
    *(volatile uint32_t*)(d->mmio_base + off) = v;
}

// ============================================================================
// PM-Timer microsecond delay (PIT-based fallback if PM Timer unavailable)
// ============================================================================
static void e100_delay_us(uint32_t us) {
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
static int e100_wait_scb(eepro100_dev_t* d) {
    for (int i = 0; i < 10000; i++) {
        if (e100_r8(d, EEPRO100_SCB_CMD) == 0) return 0;
        e100_delay_us(10);
    }
    kprintf("eepro100: SCB command timeout (cmd=0x%x)\n",
            e100_r8(d, EEPRO100_SCB_CMD));
    return -1;
}

// ============================================================================
// 93C46-style EEPROM bit-banged read (16-bit word, 6-bit address)
// ============================================================================
#define EEP_CS  0x02
#define EEP_SK  0x01
#define EEP_DI  0x04
#define EEP_DO  0x08

static void e100_eep_pulse(eepro100_dev_t* d, uint16_t v) {
    e100_w16(d, EEPRO100_SCB_EEPROM, v | EEP_SK);
    e100_delay_us(2);
    e100_w16(d, EEPRO100_SCB_EEPROM, v);
    e100_delay_us(2);
}

static uint16_t e100_eeprom_read(eepro100_dev_t* d, uint8_t addr) {
    // Drop CS, then raise CS to begin a transaction.
    e100_w16(d, EEPRO100_SCB_EEPROM, 0);
    e100_delay_us(2);
    e100_w16(d, EEPRO100_SCB_EEPROM, EEP_CS);
    e100_delay_us(2);

    // Shift in: start bit (1), opcode READ (10), 6-bit address.
    uint16_t cmd = (1 << 8) | (0x2 << 6) | (addr & 0x3F);   // 9 bits total
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
static void e100_port_reset(eepro100_dev_t* d) {
    // PORT is a 32-bit register: low 2 bits select the operation, rest is
    // a DMA address used by the selftest path (we don't use it).
    e100_w32(d, EEPRO100_SCB_PORT, EEPRO100_PORT_SOFT_RESET);
    e100_delay_us(20);          // spec says 10us; be generous
    e100_w32(d, EEPRO100_SCB_PORT, EEPRO100_PORT_SEL_RESET);
    e100_delay_us(20);
}

// ============================================================================
// Helpers for the per-CB / per-RFD blocks inside the ring DMA region
// ============================================================================
static inline eepro100_cb_t* e100_tx_cb(eepro100_dev_t* d, uint16_t i) {
    return (eepro100_cb_t*)(d->tx_blocks + (uint32_t)i * EEPRO100_TX_BLOCK_SIZE);
}
static inline uint64_t e100_tx_cb_phys(eepro100_dev_t* d, uint16_t i) {
    return d->tx_blocks_phys + (uint32_t)i * EEPRO100_TX_BLOCK_SIZE;
}
static inline uint8_t* e100_tx_payload(eepro100_dev_t* d, uint16_t i) {
    return (uint8_t*)e100_tx_cb(d, i) + sizeof(eepro100_cb_t);
}

static inline eepro100_rfd_t* e100_rx_rfd(eepro100_dev_t* d, uint16_t i) {
    return (eepro100_rfd_t*)(d->rx_blocks + (uint32_t)i * EEPRO100_RX_BLOCK_SIZE);
}
static inline uint64_t e100_rx_rfd_phys(eepro100_dev_t* d, uint16_t i) {
    return d->rx_blocks_phys + (uint32_t)i * EEPRO100_RX_BLOCK_SIZE;
}
static inline uint8_t* e100_rx_payload(eepro100_dev_t* d, uint16_t i) {
    return (uint8_t*)e100_rx_rfd(d, i) + sizeof(eepro100_rfd_t);
}

// ============================================================================
// Allocate TX CB ring + RX RFD ring (each in one contiguous block of
// low-physical memory so that 32-bit `link` fields can hold the addresses).
// ============================================================================
static int e100_alloc_rings(eepro100_dev_t* d) {
    uint32_t tx_bytes = (uint32_t)EEPRO100_NUM_TX_DESC * EEPRO100_TX_BLOCK_SIZE;
    uint32_t rx_bytes = (uint32_t)EEPRO100_NUM_RX_DESC * EEPRO100_RX_BLOCK_SIZE;

    uint32_t tx_pages = (tx_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t rx_pages = (rx_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t tx_phys = mm_allocate_contiguous_pages(tx_pages);
    uint64_t rx_phys = mm_allocate_contiguous_pages(rx_pages);
    if (!tx_phys || !rx_phys || (tx_phys >> 32) || (rx_phys >> 32)) {
        kprintf("eepro100: failed to allocate rings (or above 4GB)\n");
        return -1;
    }

    d->tx_blocks      = (uint8_t*)phys_to_virt(tx_phys);
    d->tx_blocks_phys = tx_phys;
    d->rx_blocks      = (uint8_t*)phys_to_virt(rx_phys);
    d->rx_blocks_phys = rx_phys;

    // Zero everything (descriptor fields and buffer area).
    for (uint32_t i = 0; i < tx_bytes; i++) d->tx_blocks[i] = 0;
    for (uint32_t i = 0; i < rx_bytes; i++) d->rx_blocks[i] = 0;

    // Pre-initialise the TX CB ring.  Each CB starts as a NOP that the CU
    // will simply skip; we'll patch slots into CmdTx on demand.  link
    // fields form a circular ring so the CU can keep walking forever.
    for (uint16_t i = 0; i < EEPRO100_NUM_TX_DESC; i++) {
        eepro100_cb_t* cb = e100_tx_cb(d, i);
        cb->status   = 0;
        cb->command  = EEPRO100_CMD_NOP;
        cb->link     = (uint32_t)e100_tx_cb_phys(
                           d, (i + 1) % EEPRO100_NUM_TX_DESC);
        cb->tbd_addr = 0xFFFFFFFFu;
        cb->tcb_bytes = 0;
        cb->tx_threshold = 0xE0;
        cb->tbd_count    = 0;
    }
    d->tx_prod = 0;
    d->tx_cons = 0;

    // Pre-initialise the RX RFD ring.  All RFDs are linked into a circular
    // list and pre-armed for the chip — the LAST one carries the EL bit so
    // the RU stops gracefully if we fail to drain in time.
    for (uint16_t i = 0; i < EEPRO100_NUM_RX_DESC; i++) {
        eepro100_rfd_t* rfd = e100_rx_rfd(d, i);
        rfd->status      = 0;
        rfd->command     = (i == EEPRO100_NUM_RX_DESC - 1)
                              ? EEPRO100_CMD_EL : 0;
        rfd->link        = (uint32_t)e100_rx_rfd_phys(
                                d, (i + 1) % EEPRO100_NUM_RX_DESC);
        rfd->rx_buf_addr = 0;       // 0 = buffer immediately after header
        rfd->count       = 0;
        rfd->size        = EEPRO100_RX_BUF_SIZE;
    }
    d->rx_cur = 0;
    return 0;
}

// ============================================================================
// Issue a single one-shot CB (CmdConfigure / CmdIASetup) and wait for it
// to complete.  We borrow tx_blocks slot 0 for this — it will be re-armed
// as a NOP afterwards, before normal TX traffic begins.
// ============================================================================
static int e100_issue_setup_cb(eepro100_dev_t* d,
                               uint16_t cmd_type,
                               const uint8_t* payload,
                               uint32_t payload_len) {
    eepro100_cb_t* cb = e100_tx_cb(d, 0);

    // For CmdConfigure / CmdIASetup the chip reads the payload starting
    // at CB+8 (overlapping tbd_addr / tcb_bytes / tx_threshold / tbd_count),
    // *not* at CB+16 like CmdTx simplified-mode data does.  See QEMU's
    // hw/net/eepro100.c:action_command() — pci_dma_read(cb_address + 8, ...).
    uint8_t* dst = (uint8_t*)cb + 8;
    for (uint32_t i = 0; i < payload_len; i++) dst[i] = payload[i];

    cb->status   = 0;
    cb->command  = EEPRO100_CMD_EL | cmd_type;
    cb->link     = (uint32_t)e100_tx_cb_phys(d, 0);   // self-loop (EL stops CU)
    __asm__ volatile("mfence" ::: "memory");

    // Hand to the CU
    if (e100_wait_scb(d) < 0) return -1;
    e100_w32(d, EEPRO100_SCB_POINTER, (uint32_t)e100_tx_cb_phys(d, 0));
    e100_w8 (d, EEPRO100_SCB_CMD,     EEPRO100_CU_START);

    // Poll for CB completion (status.C = 1)
    for (int i = 0; i < 100000; i++) {
        if (cb->status & EEPRO100_STATUS_C) {
            // Acknowledge any CU bits the chip set
            uint8_t st = e100_r8(d, EEPRO100_SCB_STATUS);
            if (st) e100_w8(d, EEPRO100_SCB_ACK, st);
            // Restore slot 0 to a NOP so normal TX can reuse it later
            cb->status  = 0;
            cb->command = EEPRO100_CMD_NOP;
            cb->link    = (uint32_t)e100_tx_cb_phys(d, 1);
            return 0;
        }
        e100_delay_us(20);
    }
    kprintf("eepro100: setup CB timeout (cmd=0x%x status=0x%x)\n",
            cmd_type, cb->status);
    return -1;
}

// ============================================================================
// Configure RU base = CU base = 0 (we use absolute physical addresses for
// every CB.link / RFD.link).
// ============================================================================
static int e100_load_bases(eepro100_dev_t* d) {
    if (e100_wait_scb(d) < 0) return -1;
    e100_w32(d, EEPRO100_SCB_POINTER, 0);
    e100_w8 (d, EEPRO100_SCB_CMD,     EEPRO100_CU_LOAD_BASE);
    if (e100_wait_scb(d) < 0) return -1;
    e100_w32(d, EEPRO100_SCB_POINTER, 0);
    e100_w8 (d, EEPRO100_SCB_CMD,     EEPRO100_RU_LOAD_BASE);
    if (e100_wait_scb(d) < 0) return -1;
    return 0;
}

// ============================================================================
// 22-byte CmdConfigure payload.  The values below are the well-known
// "safe" defaults used by Linux's e100 driver; only a handful of bits
// matter to QEMU's eepro100 emulation:
//
//   byte[0]   = 22       (configuration byte count)
//   byte[7]   bit0 = 0   (don't discard short frames — DHCP/ARP need them)
//   byte[8]   bit7 = 0   (CSMA enabled)
//   byte[15]  bit0 = 0   (not promiscuous)
//   byte[18]  bit2 = 0   (don't transfer received CRC)
//   byte[18]  bit3 = 0   (drop frames larger than 1518 bytes)
//   byte[20]  bit6 = 0   (no Multiple-IA mode)
//   byte[21]  bit3 = 1   (Multicast All — we have no real hash filter)
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

// ============================================================================
// Send one Ethernet frame
// ============================================================================
static int eepro100_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    eepro100_dev_t* d = (eepro100_dev_t*)ndev->driver_data;

    if (len > EEPRO100_TX_BUF_SIZE) return -1;
    uint16_t out_len = (len < 60) ? 60 : len;

    uint16_t slot = d->tx_prod;
    eepro100_cb_t* cb = e100_tx_cb(d, slot);

    // If the producer is about to lap the consumer, claim back any
    // descriptors the chip has finished with (status.C set).
    while (1) {
        if (d->tx_cons == slot && (cb->status & EEPRO100_STATUS_C) == 0
            && cb->command != EEPRO100_CMD_NOP) {
            // Ring full — drop frame
            ndev->tx_errors++;
            return -1;
        }
        if (!(e100_tx_cb(d, d->tx_cons)->status & EEPRO100_STATUS_C)
            && e100_tx_cb(d, d->tx_cons)->command != EEPRO100_CMD_NOP)
            break;
        d->tx_cons = (d->tx_cons + 1) % EEPRO100_NUM_TX_DESC;
        if (d->tx_cons == slot) break;
    }

    // Copy payload into the slot's inline buffer
    uint8_t* buf = e100_tx_payload(d, slot);
    for (uint16_t i = 0; i < len; i++) buf[i] = data[i];
    for (uint16_t i = len; i < out_len; i++) buf[i] = 0;

    // Build the CmdTx CB.  Simplified mode: tbd_addr = 0xFFFFFFFF means
    // "data follows the CB header" (which it does — see e100_tx_payload).
    cb->status      = 0;
    cb->tbd_addr    = 0xFFFFFFFFu;
    cb->tcb_bytes   = out_len;
    cb->tx_threshold = 0xE0;
    cb->tbd_count   = 0;
    cb->link        = (uint32_t)e100_tx_cb_phys(
                           d, (slot + 1) % EEPRO100_NUM_TX_DESC);
    // EL = stop CU on this CB, so we don't run off into stale NOPs
    cb->command     = EEPRO100_CMD_EL | EEPRO100_CMD_TX;
    __asm__ volatile("mfence" ::: "memory");

    // Kick the CU.  Because we always set EL on the most-recent CB, the
    // CU is in "Suspended" state after the previous TX completes — issuing
    // CU_START with the new CB's address restarts execution from there.
    if (e100_wait_scb(d) < 0) {
        ndev->tx_errors++;
        return -1;
    }
    e100_w32(d, EEPRO100_SCB_POINTER, (uint32_t)e100_tx_cb_phys(d, slot));
    e100_w8 (d, EEPRO100_SCB_CMD,
             d->cu_started ? EEPRO100_CU_START : EEPRO100_CU_START);
    d->cu_started = 1;

    d->tx_prod = (slot + 1) % EEPRO100_NUM_TX_DESC;
    ndev->tx_packets++;
    ndev->tx_bytes += out_len;
    return 0;
}

// ============================================================================
// Link status callback — ask the PHY's BMSR via the MDI window.  For QEMU
// this always reports link-up while a netdev is attached.
// ============================================================================
// Polled-only: this chip has no link-change interrupt, so all hot-plug
// detection happens here on each call.  Edge logic mirrors the IRQ-
// driven NICs: print UP/DOWN once, and on a DOWN edge invalidate the
// DHCP lease so the next dhclient does a full DISCOVER against the new
// network.
static int eepro100_link_status(net_device_t* ndev) {
    eepro100_dev_t* d = (eepro100_dev_t*)ndev->driver_data;
    // MDI Read | PHY 1 | reg 1 (BMSR)
    uint32_t mdi = (2u << 26) | (1u << 21) | (1u << 16);
    e100_w32(d, EEPRO100_SCB_MDI, mdi);
    int now_up = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t r = e100_r32(d, EEPRO100_SCB_MDI);
        if (r & (1u << 28)) {           // R bit (operation ready)
            now_up = (r & (1u << 2)) ? 1 : 0;   // BMSR.Link Status = bit 2
            break;
        }
        e100_delay_us(10);
    }
    int was_up = d->link_up;
    if (now_up != was_up) {
        d->link_up = now_up;
        kprintf("eepro100: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) dhcp_invalidate(ndev);
    }
    return now_up;
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the 8255x
// datasheet, masking the SCB interrupt byte and issuing a PORT software
// reset returns the chip to the post-reset state with RU/CU idle, all
// causes acked, and DMA halted.  Then drop PCI Command.BME so the root
// complex discards anything still in flight.
static void eepro100_shutdown(net_device_t* ndev) {
    eepro100_dev_t* d = (eepro100_dev_t*)ndev->driver_data;
    if (!d) return;
    if (!d->mmio_base && !d->io_base) return;

    // Mask all interrupt sources (SCB INTMASK bit 0 = M = mask all).
    e100_w8(d, EEPRO100_SCB_INTMASK, 0x01);

    // PORT software reset — idles RU and CU, halts DMA, clears causes.
    e100_w32(d, EEPRO100_SCB_PORT, EEPRO100_PORT_SOFT_RESET);

    if (d->pci_dev) {
        const pci_device_t* pdev = d->pci_dev;
        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0004u; // clear Bus Master Enable
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// Drain RX RFDs that the chip has filled in
// ============================================================================
static void e100_drain_rx(eepro100_dev_t* d) {
    while (1) {
        eepro100_rfd_t* rfd = e100_rx_rfd(d, d->rx_cur);
        if (!(rfd->status & EEPRO100_STATUS_C)) break;

        uint16_t len = rfd->count & 0x3FFF;
        if (len > 0 && len <= EEPRO100_RX_BUF_SIZE) {
            net_rx_packet(&d->net_dev, e100_rx_payload(d, d->rx_cur), len);
        } else {
            d->net_dev.rx_errors++;
        }

        // Re-arm this RFD: clear status/count, place EL on the newly-
        // free descriptor so the chip stops walking past us if we fall
        // behind, and clear EL on the previous EL-holder.
        uint16_t prev = (d->rx_cur + EEPRO100_NUM_RX_DESC - 1)
                        % EEPRO100_NUM_RX_DESC;
        e100_rx_rfd(d, prev)->command = 0;
        rfd->status  = 0;
        rfd->count   = 0;
        rfd->command = EEPRO100_CMD_EL;
        __asm__ volatile("mfence" ::: "memory");

        d->rx_cur = (d->rx_cur + 1) % EEPRO100_NUM_RX_DESC;
    }
}

// ============================================================================
// Top-half IRQ handler — dispatched from kernel/ke/interrupt.c
// ============================================================================
void eepro100_irq_handler(void) {
    if (!g_eepro100_initialized) {
        lapic_eoi();
        return;
    }

    // Entropy from IRQ timing
    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    eepro100_dev_t* d = &g_eepro100;

    // The STAT/ACK bits (CX/FR/CNA/RNR/MDI/SWI) live in the SCBAck byte
    // (offset 1), NOT the SCBStatus byte (offset 0, which holds CUS/RUS
    // state).  Reading offset 0 would always come back without those bits
    // set — we'd ack zero, the chip would keep INTx asserted, and we'd
    // melt under an IRQ storm.
    uint8_t st = e100_r8(d, EEPRO100_SCB_ACK);
    if (!st) {
        // Not ours (shared legacy IRQ) or already acked.
        lapic_eoi();
        return;
    }

    // Acknowledge every status bit we observed, in one shot.  QEMU's
    // eepro100_acknowledge() ANDs scb_stat with ~SCBAck, so writing the
    // bits back clears them and deasserts INTx.
    e100_w8(d, EEPRO100_SCB_ACK, st);

    if (st & EEPRO100_STAT_FR)  e100_drain_rx(d);
    if (st & EEPRO100_STAT_RNR) {
        // RU exhausted its descriptor list — re-arm and resume.
        d->net_dev.rx_dropped++;
        if (e100_wait_scb(d) == 0) {
            e100_w32(d, EEPRO100_SCB_POINTER,
                     (uint32_t)e100_rx_rfd_phys(d, d->rx_cur));
            e100_w8 (d, EEPRO100_SCB_CMD, EEPRO100_RU_START);
        }
    }

    lapic_eoi();
}

// ============================================================================
// Map BAR0 (MMIO) or fall back to BAR1 (I/O)
// ============================================================================
static int e100_map_csr(eepro100_dev_t* d) {
    uint32_t bar0 = d->pci_dev->bar[0];
    if (!(bar0 & 1)) {
        // MMIO (memory-mapped CSR window — 4KB)
        uint64_t phys = bar0 & 0xFFFFFFF0ULL;
        if ((bar0 & 0x06) == 0x04) {
            phys |= ((uint64_t)d->pci_dev->bar[1]) << 32;
        }
        d->mmio_phys = phys;
        uint64_t virt = (uint64_t)phys_to_virt(phys);
        if (!mm_is_page_mapped(virt)) {
            mm_map_page(virt, phys,
                        PAGE_PRESENT | PAGE_WRITABLE
                      | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
        }
        d->mmio_base = (volatile uint8_t*)virt;
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
typedef struct { uint16_t did; const char* name; } eepro100_id_t;

static const eepro100_id_t eepro100_pci_ids[] = {
    { 0x1029, "i82559 InBusiness" },
    { 0x1030, "i82559"            },
    { 0x1031, "PRO/100 VE (ICH3)" },
    { 0x1032, "PRO/100 VE (ICH3) #2" },
    { 0x1033, "PRO/100 VM (ICH3)" },
    { 0x1034, "PRO/100 VM (ICH3) #2" },
    { 0x1035, "82801CAM (ICH3)"   },
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
    { 0x1059, "82551QM"           },
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
    { 0x1229, "i82557/82558/82559"        },
    { 0x2449, "i82801 (ICH)"              },
    { 0x2459, "i82801DB (ICH4)"           },
    { 0x245D, "i82801EB (ICH5)"           },
    { 0x27DC, "i82801G (ICH7)"            },
    { 0,      NULL                         },
};

static const eepro100_id_t* eepro100_lookup(uint16_t did) {
    for (const eepro100_id_t* e = eepro100_pci_ids; e->name; e++) {
        if (e->did == did) return e;
    }
    return NULL;
}

// ============================================================================
// Driver entry point
// ============================================================================
void eepro100_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != EEPRO100_VENDOR_ID) continue;
        uint16_t did = devs[i].device_id;
        const eepro100_id_t* match = eepro100_lookup(did);
        if (!match) continue;

        const char* name = match->name;
        kprintf("eepro100: Found %s (PCI %02x:%02x.%x)\n",
                name, devs[i].bus, devs[i].device, devs[i].function);

        eepro100_dev_t* d = &g_eepro100;
        d->pci_dev    = &devs[i];
        d->device_id  = did;
        d->model_name = name;
        d->use_io     = 0;
        d->cu_started = 0;

        // Enable PCI memory + bus-master.  pci_enable_busmaster_mem()
        // doesn't touch I/O Space Enable; if we end up using BAR1 we
        // also need that bit set.
        pci_enable_busmaster_mem(d->pci_dev);
        {
            uint32_t cmd = pci_cfg_read32(d->pci_dev->bus,
                                          d->pci_dev->device,
                                          d->pci_dev->function, 0x04);
            if (!(cmd & 0x1)) {
                pci_cfg_write32(d->pci_dev->bus, d->pci_dev->device,
                                d->pci_dev->function, 0x04, cmd | 0x1);
            }
        }

        if (e100_map_csr(d) < 0) return;

        // Reset the chip into a known state.
        e100_port_reset(d);

        // Read MAC from the on-chip 93C46 EEPROM (3 16-bit words).
        uint16_t w0 = e100_eeprom_read(d, 0);
        uint16_t w1 = e100_eeprom_read(d, 1);
        uint16_t w2 = e100_eeprom_read(d, 2);
        d->mac_addr[0] =  w0       & 0xFF;
        d->mac_addr[1] = (w0 >> 8) & 0xFF;
        d->mac_addr[2] =  w1       & 0xFF;
        d->mac_addr[3] = (w1 >> 8) & 0xFF;
        d->mac_addr[4] =  w2       & 0xFF;
        d->mac_addr[5] = (w2 >> 8) & 0xFF;
        kprintf("eepro100: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                d->mac_addr[0], d->mac_addr[1], d->mac_addr[2],
                d->mac_addr[3], d->mac_addr[4], d->mac_addr[5]);

        // Allocate descriptor rings + buffer area.
        if (e100_alloc_rings(d) < 0) return;

        // Tell the chip where its CU/RU bases live (= 0; we use absolute
        // physical addresses everywhere).
        if (e100_load_bases(d) < 0) {
            kprintf("eepro100: load-base sequence failed\n");
            return;
        }

        // Send a CmdConfigure, then CmdIASetup.  Both block until the CB
        // reports status.C.
        if (e100_issue_setup_cb(d, EEPRO100_CMD_CONFIGURE,
                                e100_default_config,
                                sizeof(e100_default_config)) < 0) {
            kprintf("eepro100: CmdConfigure failed\n");
            return;
        }
        if (e100_issue_setup_cb(d, EEPRO100_CMD_IASETUP,
                                d->mac_addr, 6) < 0) {
            kprintf("eepro100: CmdIASetup failed\n");
            return;
        }

        // Resolve the legacy IRQ via ACPI _PRT (with the bridge-swizzle
        // dance from e1000.c / pcnet32.c) and program the IOAPIC.  No
        // MSI on the 8255x family.
        uint8_t irq = 0xFF;
        uint32_t gsi = 0;
        uint8_t pin = d->pci_dev->interrupt_pin;
        if (pin >= 1 && pin <= 4) {
            uint8_t acpi_pin   = pin - 1;
            uint8_t lookup_dev = d->pci_dev->device;
            uint8_t lookup_pin = acpi_pin;
            if (d->pci_dev->bus != 0) {
                const pci_device_t* bridge =
                    pci_find_bridge_for_bus(d->pci_dev->bus);
                if (bridge) {
                    lookup_pin = (acpi_pin + d->pci_dev->device) % 4;
                    lookup_dev = bridge->device;
                }
            }
            if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
                                    lookup_dev, lookup_pin, &gsi) == 0
                && gsi <= 23) {
                irq = (uint8_t)gsi;
                kprintf("eepro100: ACPI _PRT resolved INT%c -> GSI %u\n",
                        'A' + acpi_pin, gsi);
            }
        }
        if (irq == 0xFF) {
            irq = d->pci_dev->interrupt_line;
            if (irq != 0xFF && irq <= 23) {
                kprintf("eepro100: ACPI _PRT lookup failed, falling back "
                        "to PCI interrupt_line = %d\n", irq);
            } else {
                kprintf("eepro100: WARNING: no valid IRQ\n");
            }
        }
        kprintf("eepro100: Using legacy IRQ %d\n", irq);
        g_eepro100_legacy_irq = irq;

        // Clear PCI Command bit 10 (INTx Disable) if firmware left it set.
        {
            uint32_t cmd = pci_cfg_read32(d->pci_dev->bus,
                                          d->pci_dev->device,
                                          d->pci_dev->function, 0x04);
            if (cmd & PCI_CMD_INTX_DISABLE) {
                cmd &= ~PCI_CMD_INTX_DISABLE;
                pci_cfg_write32(d->pci_dev->bus, d->pci_dev->device,
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
        d->net_dev.rx_bytes   = 0;
        d->net_dev.tx_bytes   = 0;
        d->net_dev.rx_errors  = 0;
        d->net_dev.tx_errors  = 0;
        d->net_dev.rx_dropped = 0;
        g_eepro100_initialized = 1;

        // Hand the RX RFD ring to the chip and unmask SCB interrupts.
        if (e100_wait_scb(d) == 0) {
            e100_w32(d, EEPRO100_SCB_POINTER,
                     (uint32_t)e100_rx_rfd_phys(d, 0));
            e100_w8 (d, EEPRO100_SCB_CMD, EEPRO100_RU_START);
        }
        // SCBIntmask: bit 0 = Mask-All; clearing it (write 0) unmasks
        // every per-event mask bit.  Per-event bits in the upper nibble
        // ([1..6]) are also clear, so FR/RNR/CX/CNA all fire.
        e100_w8(d, EEPRO100_SCB_INTMASK, 0x00);

        // Initial link check
        d->net_dev.driver_data = d;
        d->link_up = eepro100_link_status(&d->net_dev);
        kprintf("eepro100: Link %s\n", d->link_up ? "UP" : "DOWN");

        // Wire up net_device_t and register
        d->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            d->net_dev.mac_addr[m] = d->mac_addr[m];
        d->net_dev.mtu = NET_MTU_DEFAULT;
        d->net_dev.ip_addr     = 0;
        d->net_dev.netmask     = 0;
        d->net_dev.gateway     = 0;
        d->net_dev.dns_server  = 0;
        d->net_dev.send        = eepro100_send;
        d->net_dev.link_status = eepro100_link_status;
        d->net_dev.shutdown    = eepro100_shutdown;
        d->net_dev.driver_data = d;

        net_register(&d->net_dev);
        kprintf("eepro100: Driver initialized successfully\n");
        return;
    }
}
