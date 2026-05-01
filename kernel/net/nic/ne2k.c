// LikeOS-64 NE2000-compatible PCI NIC Driver (Realtek RTL8029AS)
//
// The RTL8029(AS) is the PCI bus member of the NE2000 family of 10 Mbit
// Ethernet controllers.  It uses an internal 16 KiB SRAM accessed via a
// "remote DMA" register file, and a single 32-port I/O window (BAR0).
// Emulated by QEMU as `-device ne2k_pci`.

#include "../../../include/kernel/ne2k.h"
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

// ============================================================================
// Supported PCI device IDs.  All NE2000-PCI clones present the same
// RTL8029-style register layout.
// ============================================================================
typedef struct { uint16_t vid, did; const char* name; } ne2k_id_t;

static const ne2k_id_t ne2k_pci_ids[] = {
    { 0x10EC, 0x8029, "Realtek RTL8029"     },  // QEMU -device ne2k_pci
    { 0x1050, 0x0940, "Winbond 89C940"      },
    { 0x11F6, 0x1401, "Compex RL2000"       },
    { 0x8E2E, 0x3000, "KTI ET32P2"          },
    { 0x4A14, 0x5000, "NetVin NV5000SC"     },
    { 0x1106, 0x0926, "Via 86C926"          },
    { 0x10BD, 0x0E34, "SureCom NE34"        },
    { 0x1050, 0x5A5A, "Holtek HT80232"      },
    { 0x12C3, 0x0058, "Quic 5DPC"           },
    { 0x12C3, 0x5598, "Quic 5DPC v2"        },
    { 0x8C4A, 0x1980, "Winbond W89C940"     },
    { 0,      0,      NULL                   },
};

static const ne2k_id_t* ne2k_lookup(uint16_t vid, uint16_t did) {
    for (const ne2k_id_t* e = ne2k_pci_ids; e->name; e++) {
        if (e->vid == vid && e->did == did) return e;
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
static inline void ne_w8(ne2k_dev_t* dev, uint16_t reg, uint8_t v) {
    outb(dev->io_base + reg, v);
}
static inline uint8_t ne_r8(ne2k_dev_t* dev, uint16_t reg) {
    return inb(dev->io_base + reg);
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void ne_delay_us(uint32_t us) {
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
// Remote DMA helpers — read/write internal SRAM via the data port
// ============================================================================
static void ne_dma_read(ne2k_dev_t* dev, uint16_t src, uint8_t* dst, uint16_t len) {
    // Clear ISR.RDC, set up source address + byte count, then issue the
    // "remote read" command and pull bytes from the data port.
    ne_w8(dev, NE_P0_ISR, NE_INT_RDC);
    ne_w8(dev, NE_P0_RBCR0, len & 0xFF);
    ne_w8(dev, NE_P0_RBCR1, (len >> 8) & 0xFF);
    ne_w8(dev, NE_P0_RSAR0, src & 0xFF);
    ne_w8(dev, NE_P0_RSAR1, (src >> 8) & 0xFF);
    ne_w8(dev, NE_CR, NE_CR_RD_RD | NE_CR_STA);

    for (uint16_t i = 0; i < len; i++)
        dst[i] = inb(dev->io_base + NE_DATA);

    // Wait for DMA done
    for (int i = 0; i < 1000; i++) {
        if (ne_r8(dev, NE_P0_ISR) & NE_INT_RDC) break;
        ne_delay_us(10);
    }
    ne_w8(dev, NE_P0_ISR, NE_INT_RDC);
}

static void ne_dma_write(ne2k_dev_t* dev, uint16_t dst, const uint8_t* src, uint16_t len) {
    ne_w8(dev, NE_P0_ISR, NE_INT_RDC);
    ne_w8(dev, NE_P0_RBCR0, len & 0xFF);
    ne_w8(dev, NE_P0_RBCR1, (len >> 8) & 0xFF);
    ne_w8(dev, NE_P0_RSAR0, dst & 0xFF);
    ne_w8(dev, NE_P0_RSAR1, (dst >> 8) & 0xFF);
    ne_w8(dev, NE_CR, NE_CR_RD_WR | NE_CR_STA);

    for (uint16_t i = 0; i < len; i++)
        outb(dev->io_base + NE_DATA, src[i]);

    for (int i = 0; i < 1000; i++) {
        if (ne_r8(dev, NE_P0_ISR) & NE_INT_RDC) break;
        ne_delay_us(10);
    }
    ne_w8(dev, NE_P0_ISR, NE_INT_RDC);
}

// ============================================================================
// Reset chip
// ============================================================================
static int ne_reset(ne2k_dev_t* dev) {
    // Read-then-write the reset port (the original NE2000 trick).
    uint8_t r = ne_r8(dev, NE_RESET);
    ne_w8(dev, NE_RESET, r);

    // Wait for ISR.RST to set, max ~10 ms.
    for (int i = 0; i < 1000; i++) {
        if (ne_r8(dev, NE_P0_ISR) & NE_INT_RST) {
            ne_w8(dev, NE_P0_ISR, 0xFF);   // ack everything
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
// 12 bytes and keep every other one.  This matches Linux's `ne.c`.
// ============================================================================
static void ne_read_mac(ne2k_dev_t* dev) {
    // After reset the chip is in 8-bit DMA mode (DCR.WTS=0); the PROM is
    // mapped at remote DMA address 0x0000.
    uint8_t prom[32] = {0};

    // Make sure we're on Page 0, abort any in-flight DMA.
    ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
    ne_w8(dev, NE_P0_DCR, NE_DCR_LS | NE_DCR_FT_8B); // 8-bit, normal, FT=8
    ne_w8(dev, NE_P0_RBCR0, 0);
    ne_w8(dev, NE_P0_RBCR1, 0);
    ne_w8(dev, NE_P0_RCR, NE_RCR_MON);               // monitor mode
    ne_w8(dev, NE_P0_TCR, NE_TCR_LB_INTERNAL);       // internal loopback
    ne_w8(dev, NE_P0_IMR, 0);                        // mask all
    ne_w8(dev, NE_P0_ISR, 0xFF);                     // clear all

    ne_dma_read(dev, 0x0000, prom, sizeof(prom));

    // De-duplicate: take bytes 0, 2, 4, 6, 8, 10
    for (int i = 0; i < 6; i++)
        dev->mac_addr[i] = prom[i * 2];
}

// ============================================================================
// Send a single Ethernet frame
// ============================================================================
static int ne2k_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    ne2k_dev_t* dev = (ne2k_dev_t*)ndev->driver_data;

    if (len > 1518) return -1;

    // Pad short frames to 60 bytes (MAC strips FCS — the chip will add 4
    // CRC bytes for us).  Assemble in a small stack buffer.
    uint8_t buf[1518];
    uint16_t out_len = (len < 60) ? 60 : len;
    for (uint16_t i = 0; i < len; i++) buf[i] = data[i];
    for (uint16_t i = len; i < out_len; i++) buf[i] = 0;

    // Serialize across CPUs: chip has a single TX buffer + CR doorbell;
    // two concurrent senders would corrupt the in-flight frame.
    uint64_t txflags;
    spin_lock_irqsave(&dev->tx_lock, &txflags);

    // Wait until any prior transmit has finished (CR.TXP must be 0).
    for (int i = 0; i < 10000; i++) {
        if (!(ne_r8(dev, NE_CR) & NE_CR_TXP)) break;
        ne_delay_us(10);
    }

    // Copy frame into TX buffer (page NE_TX_PAGE_START, address 0x4000)
    ne_dma_write(dev, NE_TX_PAGE_START * NE_PAGE_SIZE, buf, out_len);

    // Program TX
    ne_w8(dev, NE_P0_TPSR, NE_TX_PAGE_START);
    ne_w8(dev, NE_P0_TBCR0, out_len & 0xFF);
    ne_w8(dev, NE_P0_TBCR1, (out_len >> 8) & 0xFF);

    // Kick — STA + TXP, page 0, abort DMA bits cleared
    ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_TXP | NE_CR_STA);

    ndev->tx_packets++;
    ndev->tx_bytes += out_len;
    spin_unlock_irqrestore(&dev->tx_lock, txflags);
    return 0;
}

// ============================================================================
// Link status — RTL8029AS reports link via CONFIG1 bit 2 ("LSO"); the
// generic NE2000 has no link bit, so we just claim "up" when the chip is
// running.  QEMU's emulation always reports link up anyway.
// ============================================================================
static int ne2k_link_status(net_device_t* ndev) {
    (void)ndev;
    return 1;
}

// Quiesce the controller ahead of an ACPI S5 transition.  The DP8390
// core itself has no Wake-on-LAN support, so we only need to mask
// interrupts, stop the chip, and drop PCI bus-master so the root
// complex discards any in-flight DMA after the OS has handed off to
// the firmware.
static void ne2k_shutdown(net_device_t* ndev) {
    ne2k_dev_t* dev = (ne2k_dev_t*)ndev->driver_data;
    if (!dev || !dev->io_base) return;

    // Page 0, abort any in-flight remote DMA, stop the chip.
    ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
    // IMR = 0 (mask all), ISR = 0xFF (W1C all pending causes).
    ne_w8(dev, NE_P0_IMR, 0x00);
    ne_w8(dev, NE_P0_ISR, 0xFF);

    if (dev->pci_dev) {
        const pci_device_t* pdev = dev->pci_dev;
        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0004u; // clear Bus Master Enable
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// Drain RX ring — read packets between BNRY+1 and CURR
// ============================================================================
static void ne_drain_rx(ne2k_dev_t* dev) {
    while (1) {
        // Read CURR from page 1
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STA);
        uint8_t curr = ne_r8(dev, NE_P1_CURR);
        // Switch back to page 0
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);

        if (curr == dev->next_pkt) break;   // ring empty

        // Read 4-byte packet header at <next_pkt>:0
        // header[0] = receive status
        // header[1] = next packet page
        // header[2..3] = length (LE, includes 4-byte header itself)
        uint8_t hdr[4];
        ne_dma_read(dev, dev->next_pkt * NE_PAGE_SIZE, hdr, 4);

        uint8_t  status   = hdr[0];
        uint8_t  next_pkt = hdr[1];
        uint16_t length   = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);

        // Sanity-check: a valid packet has status bit 0 (PRX) set, the
        // next-page lies in the RX ring, and the length is plausible.
        if (!(status & 0x01) ||
            next_pkt < NE_RX_PAGE_START || next_pkt > NE_RX_PAGE_STOP ||
            length < 4 || length > 1522) {
            // Corrupt — reset ring pointers and bail.
            dev->net_dev.rx_errors++;
            ne_w8(dev, NE_P0_BNRY, NE_RX_PAGE_START);
            ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STA);
            ne_w8(dev, NE_P1_CURR, NE_RX_PAGE_START + 1);
            ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
            dev->next_pkt = NE_RX_PAGE_START + 1;
            return;
        }

        uint16_t payload_len = length - 4;   // strip the in-RAM 4-byte header
        if (payload_len > 1518) payload_len = 1518;

        uint8_t pkt[1518];
        // Packet may wrap the ring — split into two reads if so.
        uint16_t addr = dev->next_pkt * NE_PAGE_SIZE + 4;
        uint16_t end  = NE_RX_PAGE_STOP * NE_PAGE_SIZE;
        if (addr + payload_len <= end) {
            ne_dma_read(dev, addr, pkt, payload_len);
        } else {
            uint16_t first = end - addr;
            ne_dma_read(dev, addr, pkt, first);
            ne_dma_read(dev, NE_RX_PAGE_START * NE_PAGE_SIZE,
                        pkt + first, payload_len - first);
        }

        net_rx_packet(&dev->net_dev, pkt, payload_len);

        // Advance: BNRY = next_pkt - 1 (with wrap to PSTOP-1 at PSTART)
        uint8_t new_bnry = (next_pkt == NE_RX_PAGE_START)
                            ? (NE_RX_PAGE_STOP - 1)
                            : (next_pkt - 1);
        ne_w8(dev, NE_P0_BNRY, new_bnry);

        dev->next_pkt = next_pkt;
    }
}

// ============================================================================
// Top-half IRQ handler
// ============================================================================
void ne2k_irq_handler(void) {
    if (!g_ne2k_initialized) {
        lapic_eoi();
        return;
    }

    // Feed entropy from NIC IRQ timing
    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    ne2k_dev_t* dev = &g_ne2k;

    // Force page 0 so we can read ISR
    ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);

    uint8_t isr = ne_r8(dev, NE_P0_ISR);
    if (isr == 0) {
        lapic_eoi();
        return;
    }

    // Ack handled bits (W1C)
    ne_w8(dev, NE_P0_ISR, isr);

    if (isr & NE_INT_PRX)
        ne_drain_rx(dev);
    if (isr & NE_INT_RXE)
        dev->net_dev.rx_errors++;
    if (isr & NE_INT_TXE)
        dev->net_dev.tx_errors++;
    if (isr & NE_INT_OVW) {
        // Ring overflow — reset, then drain whatever is left
        dev->net_dev.rx_dropped++;
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
        ne_w8(dev, NE_P0_RBCR0, 0);
        ne_w8(dev, NE_P0_RBCR1, 0);
        ne_w8(dev, NE_P0_TCR, NE_TCR_LB_INTERNAL);
        ne_w8(dev, NE_P0_BNRY, NE_RX_PAGE_START);
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STP);
        ne_w8(dev, NE_P1_CURR, NE_RX_PAGE_START + 1);
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
        dev->next_pkt = NE_RX_PAGE_START + 1;
        ne_w8(dev, NE_P0_TCR, NE_TCR_LB_NORMAL);
    }
    // PTX, RDC, CNT — nothing to do (TX completion polled in send(),
    // counters auto-clear on read).

    lapic_eoi();
}

// ============================================================================
// Resolve I/O BAR (RTL8029 BAR0 is PMIO, 32 bytes wide)
// ============================================================================
static int ne_resolve_iobase(ne2k_dev_t* dev) {
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
void ne2k_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        const ne2k_id_t* match = ne2k_lookup(devs[i].vendor_id, devs[i].device_id);
        if (!match) continue;

        kprintf("NE2K: Found %s (PCI %02x:%02x.%x)\n",
                match->name, devs[i].bus, devs[i].device, devs[i].function);

        ne2k_dev_t* dev = &g_ne2k;
        dev->pci_dev = &devs[i];

        pci_enable_busmaster_mem(dev->pci_dev);

        // pci_enable_busmaster_mem() only sets MEM (bit 1) + Bus Master
        // (bit 2).  RTL8029 BAR0 is I/O space, so we must also set
        // I/O Space Enable (bit 0) — without it, inb() returns 0xFF.
        {
            uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                          dev->pci_dev->device,
                                          dev->pci_dev->function, 0x04);
            if (!(cmd & 0x1)) {
                pci_cfg_write32(dev->pci_dev->bus,
                                dev->pci_dev->device,
                                dev->pci_dev->function, 0x04, cmd | 0x1);
            }
        }

        if (ne_resolve_iobase(dev) < 0) return;
        kprintf("NE2K: I/O base 0x%x\n", dev->io_base);

        if (ne_reset(dev) < 0) return;

        ne_read_mac(dev);
        kprintf("NE2K: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        // Standard NE2000 init sequence (NS DP8390 datasheet):
        //   1. Stop chip, 8-bit DMA, FT=8
        //   2. Clear remote byte count
        //   3. RX in monitor; TX in internal loopback (defer accept until
        //      after we've programmed the MAC and ring pointers)
        //   4. Page 0: ring pointers (PSTART/PSTOP/BNRY/TPSR)
        //   5. Clear ISR, mask interrupts
        //   6. Page 1: program PAR0..5 from MAC, MAR0..7 = 0xFF, CURR
        //   7. Page 0: STA + abort DMA, RX/TX in normal mode, unmask IRQs
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STP);
        ne_w8(dev, NE_P0_DCR, NE_DCR_LS | NE_DCR_FT_8B);
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

        // Page 1 — program MAC, multicast accept-all, CURR
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_PS0 | NE_CR_STP);
        for (int m = 0; m < 6; m++)
            ne_w8(dev, NE_P1_PAR0 + m, dev->mac_addr[m]);
        for (int m = 0; m < 8; m++)
            ne_w8(dev, NE_P1_MAR0 + m, 0xFF);
        ne_w8(dev, NE_P1_CURR, NE_RX_PAGE_START + 1);

        // Back to page 0, start chip
        ne_w8(dev, NE_CR, NE_CR_RD_ABORT | NE_CR_STA);
        ne_w8(dev, NE_P0_TCR, NE_TCR_LB_NORMAL);
        ne_w8(dev, NE_P0_RCR, NE_RCR_AB | NE_RCR_AM);  // accept BC+MC + own MAC

        dev->next_pkt = NE_RX_PAGE_START + 1;

        // Resolve IRQ via ACPI _PRT first; chip has no MSI capability.
        uint8_t irq = 0xFF;
        uint32_t gsi = 0;
        uint8_t pin = dev->pci_dev->interrupt_pin;
        if (pin >= 1 && pin <= 4) {
            uint8_t acpi_pin = pin - 1;
            uint8_t lookup_dev = dev->pci_dev->device;
            uint8_t lookup_pin = acpi_pin;
            if (dev->pci_dev->bus != 0) {
                const pci_device_t* bridge =
                    pci_find_bridge_for_bus(dev->pci_dev->bus);
                if (bridge) {
                    lookup_pin = (acpi_pin + dev->pci_dev->device) % 4;
                    lookup_dev = bridge->device;
                }
            }
            if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
                                    lookup_dev, lookup_pin, &gsi) == 0
                && gsi <= 23) {
                irq = (uint8_t)gsi;
                kprintf("NE2K: ACPI _PRT resolved INT%c -> GSI %u\n",
                        'A' + acpi_pin, gsi);
            }
        }
        if (irq == 0xFF) {
            irq = dev->pci_dev->interrupt_line;
            if (irq != 0xFF && irq <= 23) {
                kprintf("NE2K: ACPI _PRT lookup failed, falling back to "
                        "PCI interrupt_line = %d\n", irq);
            } else {
                kprintf("NE2K: WARNING: no valid IRQ\n");
            }
        }
        kprintf("NE2K: Using legacy IRQ %d\n", irq);
        g_ne2k_legacy_irq = irq;

        // Clear PCI Command INTx Disable if set
        {
            uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                          dev->pci_dev->device,
                                          dev->pci_dev->function, 0x04);
            if (cmd & PCI_CMD_INTX_DISABLE) {
                cmd &= ~PCI_CMD_INTX_DISABLE;
                pci_cfg_write32(dev->pci_dev->bus,
                                dev->pci_dev->device,
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
        dev->tx_lock = (spinlock_t)SPINLOCK_INIT("ne2k_tx");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;
        dev->link_up = 1;
        g_ne2k_initialized = 1;

        // Enable interrupts of interest
        ne_w8(dev, NE_P0_ISR, 0xFF);
        ne_w8(dev, NE_P0_IMR,
              NE_INT_PRX | NE_INT_PTX | NE_INT_RXE | NE_INT_TXE |
              NE_INT_OVW);

        // Wire up net_device_t and register
        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = ne2k_send;
        dev->net_dev.link_status = ne2k_link_status;
        dev->net_dev.shutdown = ne2k_shutdown;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);
        kprintf("NE2K: Driver initialized successfully\n");
        return;  // first device only
    }
}
