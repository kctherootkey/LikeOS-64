// LikeOS-64 AMD PCnet32 NIC Driver
//
// Supports the AMD PCnet-PCI family (PCI 0x1022:0x2000):
//   - Am79C970A   "PCnet-PCI II"
//   - Am79C973    "PCnet-FAST III"
//   - QEMU `-device pcnet`
//
// Programs the chip in 32-bit DWord I/O mode with SWSTYLE=2 ("PCnet-PCI II
// 32-bit"), which works on every member of the family.  The chip has only
// legacy INTx — no MSI/MSI-X.

#include "../../../include/kernel/pcnet32.h"
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
// Supported PCI device IDs for the AMD PCnet-PCI family.
// ============================================================================
typedef struct { uint16_t vid, did; const char* name; } pcnet_id_t;

static const pcnet_id_t pcnet_pci_ids[] = {
    { 0x1022, 0x2000, "AMD PCnet-PCI / Am79C97x" },  // QEMU -device pcnet
    { 0x1022, 0x2001, "AMD PCnet-Home"           },
    { 0x1023, 0x2000, "Trident PCnet (rebrand)"  },
    { 0,      0,      NULL                        },
};

static const pcnet_id_t* pcnet_lookup(uint16_t vid, uint16_t did) {
    for (const pcnet_id_t* e = pcnet_pci_ids; e->name; e++) {
        if (e->vid == vid && e->did == did) return e;
    }
    return NULL;
}

static pcnet_dev_t g_pcnet;
int g_pcnet_initialized = 0;
int g_pcnet_legacy_irq = -1;

// ============================================================================
// I/O helpers
// ============================================================================
static inline void pcnet_w32(pcnet_dev_t* dev, uint16_t off, uint32_t v) {
    outl(dev->io_base + off, v);
}
static inline uint32_t pcnet_r32(pcnet_dev_t* dev, uint16_t off) {
    return inl(dev->io_base + off);
}

// CSR access (DWord mode)
static inline uint32_t pcnet_read_csr(pcnet_dev_t* dev, uint32_t idx) {
    pcnet_w32(dev, PCNET_RAP, idx);
    return pcnet_r32(dev, PCNET_RDP);
}
static inline void pcnet_write_csr(pcnet_dev_t* dev, uint32_t idx, uint32_t v) {
    pcnet_w32(dev, PCNET_RAP, idx);
    pcnet_w32(dev, PCNET_RDP, v);
}

// BCR access (DWord mode)
static inline uint32_t pcnet_read_bcr(pcnet_dev_t* dev, uint32_t idx) {
    pcnet_w32(dev, PCNET_RAP, idx);
    return pcnet_r32(dev, PCNET_BDP);
}
static inline void pcnet_write_bcr(pcnet_dev_t* dev, uint32_t idx, uint32_t v) {
    pcnet_w32(dev, PCNET_RAP, idx);
    pcnet_w32(dev, PCNET_BDP, v);
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void pcnet_delay_us(uint32_t us) {
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
static void pcnet_read_mac(pcnet_dev_t* dev) {
    uint32_t lo = inl(dev->io_base + PCNET_APROM + 0);
    uint32_t hi = inl(dev->io_base + PCNET_APROM + 4);
    dev->mac_addr[0] = (uint8_t)(lo      );
    dev->mac_addr[1] = (uint8_t)(lo >>  8);
    dev->mac_addr[2] = (uint8_t)(lo >> 16);
    dev->mac_addr[3] = (uint8_t)(lo >> 24);
    dev->mac_addr[4] = (uint8_t)(hi      );
    dev->mac_addr[5] = (uint8_t)(hi >>  8);
}

// ============================================================================
// Reset chip and switch into 32-bit DWord I/O mode
// ============================================================================
static int pcnet_reset(pcnet_dev_t* dev) {
    // Read the 16-bit RESET register (in word mode it sits at offset 0x14;
    // in DWord mode at 0x18 — read both, only the appropriate one will
    // actually trigger the reset depending on the chip's current mode).
    (void)inl(dev->io_base + PCNET_RST);   // DWord-mode reset
    (void)inw(dev->io_base + PCNET_RST16); // word-mode reset

    pcnet_delay_us(1000);  // 1 ms — chip needs ~1 µs but we're generous

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
// Initialise RX descriptor ring
// ============================================================================
static int pcnet_init_rx(pcnet_dev_t* dev) {
    uint32_t bytes = sizeof(pcnet_rx_desc_t) * PCNET_NUM_RX_DESC;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys = mm_allocate_contiguous_pages(pages);
    if (!phys) {
        kprintf("PCnet32: failed to allocate RX ring\n");
        return -1;
    }
    if (phys >> 32) {
        kprintf("PCnet32: ERROR: RX ring phys 0x%lx is above 4GB\n", phys);
        return -1;
    }

    dev->rx_descs = (pcnet_rx_desc_t*)phys_to_virt(phys);
    dev->rx_descs_phys = phys;

    // Zero ring
    for (uint32_t i = 0; i < bytes / 8; i++)
        ((uint64_t*)dev->rx_descs)[i] = 0;

    for (int i = 0; i < PCNET_NUM_RX_DESC; i++) {
        uint64_t bphys = mm_allocate_physical_page();
        if (!bphys || (bphys >> 32)) {
            kprintf("PCnet32: bad RX buffer alloc %d\n", i);
            return -1;
        }
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(bphys);
        dev->rx_bufs_phys[i] = bphys;

        dev->rx_descs[i].rbadr = (uint32_t)bphys;
        dev->rx_descs[i].bcnt = (int16_t)(-PCNET_RX_BUF_SIZE);  // ones-complement length
        dev->rx_descs[i].status = RXD_OWN;
        dev->rx_descs[i].mcnt = 0;
        dev->rx_descs[i].reserved = 0;
    }
    dev->rx_cur = 0;
    return 0;
}

// ============================================================================
// Initialise TX descriptor ring (descriptors start owned by software)
// ============================================================================
static int pcnet_init_tx(pcnet_dev_t* dev) {
    uint32_t bytes = sizeof(pcnet_tx_desc_t) * PCNET_NUM_TX_DESC;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys = mm_allocate_contiguous_pages(pages);
    if (!phys) {
        kprintf("PCnet32: failed to allocate TX ring\n");
        return -1;
    }
    if (phys >> 32) {
        kprintf("PCnet32: ERROR: TX ring phys 0x%lx is above 4GB\n", phys);
        return -1;
    }

    dev->tx_descs = (pcnet_tx_desc_t*)phys_to_virt(phys);
    dev->tx_descs_phys = phys;

    for (uint32_t i = 0; i < bytes / 8; i++)
        ((uint64_t*)dev->tx_descs)[i] = 0;

    for (int i = 0; i < PCNET_NUM_TX_DESC; i++) {
        uint64_t bphys = mm_allocate_physical_page();
        if (!bphys || (bphys >> 32)) {
            kprintf("PCnet32: bad TX buffer alloc %d\n", i);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(bphys);
        dev->tx_bufs_phys[i] = bphys;

        dev->tx_descs[i].tbadr = (uint32_t)bphys;
        dev->tx_descs[i].bcnt = 0;
        dev->tx_descs[i].status = 0;          // Software owns
        dev->tx_descs[i].misc = 0;
        dev->tx_descs[i].reserved = 0;
    }
    dev->tx_cur = 0;
    return 0;
}

// ============================================================================
// Build init block and fire CSR0.INIT
// ============================================================================
static int pcnet_init_block(pcnet_dev_t* dev) {
    uint64_t phys = mm_allocate_contiguous_pages(1);
    if (!phys || (phys >> 32)) {
        kprintf("PCnet32: failed to allocate init block\n");
        return -1;
    }

    dev->init_block = (pcnet_init_block_t*)phys_to_virt(phys);
    dev->init_block_phys = phys;

    pcnet_init_block_t* ib = dev->init_block;
    for (uint32_t i = 0; i < sizeof(*ib) / 4; i++)
        ((uint32_t*)ib)[i] = 0;

    ib->mode = 0;  // Normal operation; not promiscuous
    ib->rlen = (uint8_t)(PCNET_RX_LEN_LOG2 << 4);
    ib->tlen = (uint8_t)(PCNET_TX_LEN_LOG2 << 4);
    for (int i = 0; i < 6; i++)
        ib->padr[i] = dev->mac_addr[i];
    for (int i = 0; i < 8; i++)
        ib->laddrf[i] = 0;          // No multicast
    ib->rdra = (uint32_t)dev->rx_descs_phys;
    ib->tdra = (uint32_t)dev->tx_descs_phys;

    __asm__ volatile("mfence" ::: "memory");

    // Tell the chip where the init block lives (CSR1 = lo, CSR2 = hi 16 bits)
    pcnet_write_csr(dev, CSR1, (uint32_t)(phys & 0xFFFF));
    pcnet_write_csr(dev, CSR2, (uint32_t)((phys >> 16) & 0xFFFF));

    // Enable AUTOPAD and auto-strip RX FCS
    pcnet_write_csr(dev, CSR4,
                    pcnet_read_csr(dev, CSR4) | CSR4_APAD_XMT | CSR4_ASTRP_RCV);

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
    kprintf("PCnet32: init block timeout (CSR0=0x%x)\n", pcnet_read_csr(dev, CSR0));
    return -1;
}

// ============================================================================
// Send a single Ethernet frame
// ============================================================================
static int pcnet_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    pcnet_dev_t* dev = (pcnet_dev_t*)ndev->driver_data;

    if (len > PCNET_TX_BUF_SIZE) return -1;

    uint16_t slot = dev->tx_cur;
    volatile pcnet_tx_desc_t* desc = &dev->tx_descs[slot];

    if (desc->status & TXD_OWN) {
        // Hardware still owns it — TX ring is full
        ndev->tx_errors++;
        return -1;
    }

    // Pad short frames to 60 bytes (chip's auto-pad covers this too, but
    // doing it in software is cheap and works regardless of CSR4 settings).
    uint16_t tx_len = (len < 60) ? 60 : len;
    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[slot][i] = data[i];
    for (uint16_t i = len; i < tx_len; i++)
        dev->tx_bufs[slot][i] = 0;

    desc->tbadr = (uint32_t)dev->tx_bufs_phys[slot];
    desc->bcnt = (int16_t)(-(int32_t)tx_len);   // ones-complement of length
    desc->misc = 0;

    __asm__ volatile("mfence" ::: "memory");

    // Hand to hardware: STP+ENP (single-buffer packet) and OWN
    desc->status = TXD_OWN | TXD_STP | TXD_ENP;

    __asm__ volatile("mfence" ::: "memory");

    // Demand transmission (CSR0.TDMD).  Re-assert INEA so we don't lose
    // interrupts.  Use a read-modify-write so we don't accidentally clear
    // existing W1C status bits.
    uint32_t csr0 = pcnet_read_csr(dev, CSR0);
    csr0 = (csr0 & ~(CSR0_RINT | CSR0_TINT | CSR0_IDON | CSR0_BABL |
                     CSR0_CERR | CSR0_MISS | CSR0_MERR)) |
           CSR0_INEA | CSR0_TDMD;
    pcnet_write_csr(dev, CSR0, csr0);

    dev->tx_cur = (slot + 1) % PCNET_NUM_TX_DESC;
    ndev->tx_packets++;
    ndev->tx_bytes += tx_len;
    return 0;
}

// ============================================================================
// Link status callback — read BCR9 / BCR4 link-up bit
// ============================================================================
// Polled-only: PCnet32's interrupt set has no link-change cause we wire,
// so hot-plug detection happens here.  Edge logic mirrors the IRQ-driven
// NICs: print UP/DOWN once, and on a DOWN edge invalidate the DHCP lease
// so the next dhclient does a full DISCOVER against the new network.
static int pcnet_link_status(net_device_t* ndev) {
    pcnet_dev_t* dev = (pcnet_dev_t*)ndev->driver_data;
    // BCR4 bit 6 = LNKST (link status).  On QEMU's pcnet emulation this
    // is permanently asserted whenever a netdev is plugged in.
    uint32_t bcr4 = pcnet_read_bcr(dev, 4);
    int now_up = (bcr4 & (1 << 6)) ? 1 : 0;
    int was_up = dev->link_up;
    if (now_up != was_up) {
        dev->link_up = now_up;
        kprintf("PCnet32: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) dhcp_invalidate(ndev);
    }
    return now_up;
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the Am79C97x
// datasheet, asserting CSR0.STOP halts both the RX and TX engines, masks
// all interrupt sources, and aborts any pending bus-master DMA at the
// next quiescent point.  Then drop the PCI Command.BME bit so any TLP
// that escaped the in-controller stop is dropped at the root complex.
static void pcnet_shutdown(net_device_t* ndev) {
    pcnet_dev_t* dev = (pcnet_dev_t*)ndev->driver_data;
    if (!dev || !dev->io_base) return;

    pcnet_write_csr(dev, CSR3, 0xFFFF);
    pcnet_write_csr(dev, CSR0, CSR0_STOP);

    if (dev->pci_dev) {
        const pci_device_t* pdev = dev->pci_dev;
        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0004u; // clear Bus Master Enable
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// Drain RX descriptors that are now owned by software
// ============================================================================
static void pcnet_drain_rx(pcnet_dev_t* dev) {
    while (1) {
        volatile pcnet_rx_desc_t* desc = &dev->rx_descs[dev->rx_cur];
        if (desc->status & RXD_OWN)
            break;  // Hardware still owns this descriptor — nothing left

        if (desc->status & RXD_ERR) {
            dev->net_dev.rx_errors++;
        } else if ((desc->status & (RXD_STP | RXD_ENP)) == (RXD_STP | RXD_ENP)) {
            // Good single-buffer packet
            uint16_t len = desc->mcnt & 0xFFF;
            // mcnt includes the 4-byte FCS unless ASTRP_RCV is set; we
            // enabled ASTRP_RCV so len is the payload length.
            if (len > 0 && len <= PCNET_RX_BUF_SIZE) {
                net_rx_packet(&dev->net_dev, dev->rx_bufs[dev->rx_cur], len);
            } else {
                dev->net_dev.rx_errors++;
            }
        }

        // Hand the descriptor back to the chip
        desc->bcnt = (int16_t)(-PCNET_RX_BUF_SIZE);
        desc->mcnt = 0;
        __asm__ volatile("mfence" ::: "memory");
        desc->status = RXD_OWN;

        dev->rx_cur = (dev->rx_cur + 1) % PCNET_NUM_RX_DESC;
    }
}

// ============================================================================
// IRQ top half (called from kernel/ke/interrupt.c dispatcher)
// ============================================================================
void pcnet32_irq_handler(void) {
    if (!g_pcnet_initialized) {
        lapic_eoi();
        return;
    }

    // Entropy
    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    pcnet_dev_t* dev = &g_pcnet;
    uint32_t csr0 = pcnet_read_csr(dev, CSR0);

    if (!(csr0 & CSR0_INTR)) {
        lapic_eoi();
        return;
    }

    // Acknowledge: write back the W1C bits we observed (IDON, RINT, TINT,
    // BABL, CERR, MISS, MERR) AND keep INEA asserted.  Do NOT set INIT,
    // STRT, STOP, TDMD here.
    uint32_t ack = csr0 & (CSR0_RINT | CSR0_TINT | CSR0_IDON |
                           CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR);
    pcnet_write_csr(dev, CSR0, CSR0_INEA | ack);

    if (csr0 & CSR0_RINT)
        pcnet_drain_rx(dev);

    if (csr0 & CSR0_MISS)
        dev->net_dev.rx_dropped++;
    if (csr0 & CSR0_MERR)
        kprintf("PCnet32: memory error\n");
    if (csr0 & CSR0_BABL)
        dev->net_dev.tx_errors++;

    lapic_eoi();
}

// ============================================================================
// Resolve I/O BAR (BAR0)
// ============================================================================
static int pcnet_resolve_iobase(pcnet_dev_t* dev) {
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
void pcnet32_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        const pcnet_id_t* match = pcnet_lookup(devs[i].vendor_id, devs[i].device_id);
        if (!match) continue;

        kprintf("PCnet32: Found %s (PCI %02x:%02x.%x)\n",
                match->name, devs[i].bus, devs[i].device, devs[i].function);

        pcnet_dev_t* dev = &g_pcnet;
        dev->pci_dev = &devs[i];

        pci_enable_busmaster_mem(dev->pci_dev);

        // pci_enable_busmaster_mem() only sets MEM (bit 1) + Bus Master (bit 2).
        // PCnet's BAR0 is I/O space, so we must also set I/O Space Enable
        // (bit 0) — without it, inb/inw/inl from io_base return 0xFF and
        // the MAC reads as ff:ff:ff:ff:ff:ff.
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

        if (pcnet_resolve_iobase(dev) < 0) return;
        kprintf("PCnet32: I/O base 0x%x\n", dev->io_base);

        if (pcnet_reset(dev) < 0) return;

        // Identify chip via CSR88 (CHIPID lower 16 bits) — informational only
        pcnet_write_csr(dev, 88 - 0, 0);  // ensure RAP wraps
        uint32_t chipid_lo = pcnet_read_csr(dev, 88);
        uint32_t chipid_hi = pcnet_read_csr(dev, 89);
        uint32_t part_id = ((chipid_hi & 0xFFFF) << 16) | (chipid_lo & 0xFFFF);
        const char* chip = "Unknown PCnet";
        switch ((part_id >> 12) & 0xFFFF) {
            case 0x2420: chip = "Am79C970A (PCnet-PCI II)"; break;
            case 0x2621: chip = "Am79C971 (PCnet-FAST)"; break;
            case 0x2625: chip = "Am79C973 (PCnet-FAST III)"; break;
            case 0x2627: chip = "Am79C975"; break;
            default:     chip = "PCnet (generic)"; break;
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
        if (pcnet_init_rx(dev) < 0) return;
        if (pcnet_init_tx(dev) < 0) return;

        // Build init block & program it into the chip
        if (pcnet_init_block(dev) < 0) return;

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
                kprintf("PCnet32: ACPI _PRT resolved INT%c -> GSI %u\n",
                        'A' + acpi_pin, gsi);
            }
        }
        if (irq == 0xFF) {
            irq = dev->pci_dev->interrupt_line;
            if (irq != 0xFF && irq <= 23) {
                kprintf("PCnet32: ACPI _PRT lookup failed, falling back to "
                        "PCI interrupt_line = %d\n", irq);
            } else {
                kprintf("PCnet32: WARNING: no valid IRQ\n");
            }
        }
        kprintf("PCnet32: Using legacy IRQ %d\n", irq);
        g_pcnet_legacy_irq = irq;

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
        g_pcnet_initialized = 1;

        // Unmask all interrupts (clear CSR3 mask bits)
        pcnet_write_csr(dev, CSR3, 0);

        // Start the chip and enable interrupts
        pcnet_write_csr(dev, CSR0, CSR0_STRT | CSR0_INEA);

        // Initial link check (BCR4 bit 6 = LNKST)
        dev->link_up = (pcnet_read_bcr(dev, 4) & (1 << 6)) ? 1 : 0;
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
        dev->net_dev.send = pcnet_send;
        dev->net_dev.link_status = pcnet_link_status;
        dev->net_dev.shutdown = pcnet_shutdown;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);
        kprintf("PCnet32: Driver initialized successfully\n");
        return;
    }
}
