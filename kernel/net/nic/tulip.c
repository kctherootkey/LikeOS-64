// LikeOS-64 DEC 21x4x ("Tulip") NIC Driver
//
// Drives the DEC 21040 / 21041 / 21140 / 21142 / 21143 PCI Ethernet
// controllers — the QEMU `-device tulip` exposes a 21143 (vendor 0x1011,
// device 0x0019).  Programming model: 16 8-byte CSRs in BAR0, single TX
// and RX descriptor ring in "ring" mode (RER/TER on the last entry to
// loop back to the head).  Legacy INTx only — no MSI/MSI-X on this
// part family.

#include "../../../include/kernel/tulip.h"
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
// Supported PCI device IDs
// ============================================================================
typedef struct { uint16_t vid, did; const char* name; } tulip_id_t;

static const tulip_id_t tulip_pci_ids[] = {
    { TULIP_VENDOR_DEC, TULIP_DEV_DC21040,    "DEC DC21040 Tulip"        },
    { TULIP_VENDOR_DEC, TULIP_DEV_DC21041,    "DEC DC21041 Tulip"        },
    { TULIP_VENDOR_DEC, TULIP_DEV_DC21140,    "DEC DC21140 Fast Tulip"   },
    { TULIP_VENDOR_DEC, TULIP_DEV_DC21142_43, "DEC DC21143 Tulip" },  // QEMU -device tulip
    { 0, 0, NULL },
};

static const tulip_id_t* tulip_lookup(uint16_t vid, uint16_t did) {
    for (const tulip_id_t* e = tulip_pci_ids; e->name; e++)
        if (e->vid == vid && e->did == did) return e;
    return NULL;
}

static tulip_dev_t g_tulip;
int g_tulip_initialized = 0;
int g_tulip_legacy_irq = -1;

// ============================================================================
// CSR access — supports both MMIO (BAR0 as memory) and PMIO (BAR0 as I/O).
// ============================================================================
static inline uint32_t tulip_csr_read(tulip_dev_t* dev, uint32_t off) {
    if (dev->use_mmio) {
        return *(volatile uint32_t*)(dev->mmio_base + off);
    } else {
        return inl(dev->io_base + off);
    }
}
static inline void tulip_csr_write(tulip_dev_t* dev, uint32_t off, uint32_t v) {
    if (dev->use_mmio) {
        *(volatile uint32_t*)(dev->mmio_base + off) = v;
        __asm__ volatile("mfence" ::: "memory");
    } else {
        outl(dev->io_base + off, v);
    }
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void tulip_delay_us(uint32_t us) {
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
// Read MAC from SROM (CSR9 serial EEPROM interface)
//
// The DC21040/41 expose the MAC in a 6-byte boot ROM at SROM offset 0;
// QEMU populates it the same way for the 21143.  The classical SROM
// access protocol is a bit-banged 93C46/93C66 via CSR9 — implementing
// the full state machine is overkill, but on QEMU we can rely on the
// chip having already latched the address into its receive address
// filter (look at the first received packet's destination MAC, which the
// chip uses for perfect filtering after a setup frame).
//
// Simpler approach: read SROM via CSR9.  Falls back to a synthesized
// MAC (52:54:00:12:34:56 — QEMU's default) if SROM access fails.
// ============================================================================
#define TULIP_SROM_READ_CMD     6
#define TULIP_CSR9_SR           (1u << 11)
#define TULIP_CSR9_RD           (1u << 14)
#define TULIP_CSR9_REG          (1u << 10)  // SROM access enable
#define TULIP_CSR9_CS           (1u << 0)
#define TULIP_CSR9_SCLK         (1u << 1)
#define TULIP_CSR9_SDI          (1u << 2)
#define TULIP_CSR9_SDO          (1u << 3)

static uint16_t tulip_srom_read(tulip_dev_t* dev, uint8_t addr) {
    // Enable SROM access mode (SR | RD only — do NOT set REG (bit 10),
    // that bit is the MII serial-register select and putting it on at
    // the same time as SR confuses the SROM state machine and SDO
    // returns nonsense).
    tulip_csr_write(dev, TULIP_CSR9, TULIP_CSR9_SR | TULIP_CSR9_RD);
    tulip_delay_us(10);

    uint32_t base = TULIP_CSR9_SR | TULIP_CSR9_RD;

    // Drive CS high
    tulip_csr_write(dev, TULIP_CSR9, base | TULIP_CSR9_CS);
    tulip_delay_us(10);

    // Build command: 3 cmd bits (110 = read) + 6 addr bits (MSB first)
    uint16_t cmd = (TULIP_SROM_READ_CMD << 6) | (addr & 0x3F);

    // Shift out 9 bits (3 cmd + 6 addr)
    for (int i = 8; i >= 0; i--) {
        uint32_t bit = (cmd >> i) & 1;
        uint32_t v = base | TULIP_CSR9_CS | (bit ? TULIP_CSR9_SDI : 0);
        tulip_csr_write(dev, TULIP_CSR9, v);
        tulip_delay_us(2);
        tulip_csr_write(dev, TULIP_CSR9, v | TULIP_CSR9_SCLK);
        tulip_delay_us(2);
        tulip_csr_write(dev, TULIP_CSR9, v);
        tulip_delay_us(2);
    }

    // Shift in 16 data bits (sample SDO while SCLK is high)
    uint16_t data = 0;
    for (int i = 0; i < 16; i++) {
        tulip_csr_write(dev, TULIP_CSR9, base | TULIP_CSR9_CS | TULIP_CSR9_SCLK);
        tulip_delay_us(2);
        uint32_t s = tulip_csr_read(dev, TULIP_CSR9);
        data = (data << 1) | ((s & TULIP_CSR9_SDO) ? 1 : 0);
        tulip_csr_write(dev, TULIP_CSR9, base | TULIP_CSR9_CS);
        tulip_delay_us(2);
    }

    // Drop CS, exit SROM mode
    tulip_csr_write(dev, TULIP_CSR9, base);
    tulip_csr_write(dev, TULIP_CSR9, 0);

    return data;
}

static void tulip_read_mac(tulip_dev_t* dev) {
    // 21143 SROM places the 6-byte MAC at SROM offset 20 (decimal) as
    // three 16-bit words.  21040/21041 use offset 0.  Try the standard
    // 21143 location (word 10/11/12) first, then fall back to offset 0.
    uint16_t w0 = tulip_srom_read(dev, 10);
    uint16_t w1 = tulip_srom_read(dev, 11);
    uint16_t w2 = tulip_srom_read(dev, 12);
    if ((w0 == 0xFFFF && w1 == 0xFFFF && w2 == 0xFFFF) ||
        (w0 == 0x0000 && w1 == 0x0000 && w2 == 0x0000)) {
        w0 = tulip_srom_read(dev, 0);
        w1 = tulip_srom_read(dev, 1);
        w2 = tulip_srom_read(dev, 2);
    }
    // Sanity-check: a valid unicast MAC has bit 0 of the first byte == 0.
    // If the SROM is unreadable (e.g. QEMU's emulation does not actually
    // populate one), synthesize a locally-administered MAC so we have a
    // stable identity — the chip itself does not enforce source-MAC
    // matching, and we'll be running with the perfect filter bypassed
    // (CSR6.PR set) anyway.
    uint8_t b0 = w0 & 0xFF;
    int valid = !((w0 == 0xFFFF && w1 == 0xFFFF && w2 == 0xFFFF) ||
                  (w0 == 0x0000 && w1 == 0x0000 && w2 == 0x0000) ||
                  (b0 & 0x01));
    if (!valid) {
        kprintf("Tulip: SROM MAC unreadable (got %04x %04x %04x), "
                "using fallback\n", w0, w1, w2);
        dev->mac_addr[0] = 0x52;
        dev->mac_addr[1] = 0x54;
        dev->mac_addr[2] = 0x00;
        dev->mac_addr[3] = 0x12;
        dev->mac_addr[4] = 0x34;
        dev->mac_addr[5] = 0x56;
        return;
    }
    dev->mac_addr[0] = w0 & 0xFF;
    dev->mac_addr[1] = (w0 >> 8) & 0xFF;
    dev->mac_addr[2] = w1 & 0xFF;
    dev->mac_addr[3] = (w1 >> 8) & 0xFF;
    dev->mac_addr[4] = w2 & 0xFF;
    dev->mac_addr[5] = (w2 >> 8) & 0xFF;
}

// ============================================================================
// Software reset (CSR0.SWR — self-clearing)
// ============================================================================
static int tulip_reset(tulip_dev_t* dev) {
    tulip_csr_write(dev, TULIP_CSR0, TULIP_CSR0_SWR);
    // Datasheet: SWR self-clears within ~50 PCI cycles.  Wait up to 1 ms.
    for (int i = 0; i < 1000; i++) {
        if (!(tulip_csr_read(dev, TULIP_CSR0) & TULIP_CSR0_SWR)) break;
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
// RX ring init
// ============================================================================
static int tulip_init_rx(tulip_dev_t* dev) {
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
    dev->rx_descs = (tulip_desc_t*)phys_to_virt(phys);
    dev->rx_descs_phys = phys;

    for (uint32_t i = 0; i < bytes / 8; i++)
        ((uint64_t*)dev->rx_descs)[i] = 0;

    for (int i = 0; i < TULIP_NUM_RX_DESC; i++) {
        uint64_t bp = mm_allocate_physical_page();
        if (!bp || (bp >> 32)) {
            kprintf("Tulip: bad RX buffer alloc %d\n", i);
            return -1;
        }
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(bp);
        dev->rx_bufs_phys[i] = bp;

        dev->rx_descs[i].status = TULIP_RDES0_OWN;            // hardware owns
        // RDES1.BS1 is 11 bits (bits 0..10) — max 2047, so 2048 would
        // wrap to 0 and the chip would silently drop every received
        // frame.  Use 1536 (next multiple of 32 above max ethernet
        // frame 1518) which is what real Tulip drivers use.
        dev->rx_descs[i].ctrl   = 1536 & 0x7FF;
        if (i == TULIP_NUM_RX_DESC - 1)
            dev->rx_descs[i].ctrl |= TULIP_RDES1_RER;
        dev->rx_descs[i].buf1 = (uint32_t)bp;
        dev->rx_descs[i].buf2 = 0;
    }
    dev->rx_cur = 0;
    tulip_csr_write(dev, TULIP_CSR3, (uint32_t)phys);
    return 0;
}

// ============================================================================
// TX ring init
// ============================================================================
static int tulip_init_tx(tulip_dev_t* dev) {
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
    dev->tx_descs = (tulip_desc_t*)phys_to_virt(phys);
    dev->tx_descs_phys = phys;

    for (uint32_t i = 0; i < bytes / 8; i++)
        ((uint64_t*)dev->tx_descs)[i] = 0;

    for (int i = 0; i < TULIP_NUM_TX_DESC; i++) {
        uint64_t bp = mm_allocate_physical_page();
        if (!bp || (bp >> 32)) {
            kprintf("Tulip: bad TX buffer alloc %d\n", i);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(bp);
        dev->tx_bufs_phys[i] = bp;

        dev->tx_descs[i].status = 0;     // software owns
        dev->tx_descs[i].ctrl   = 0;
        if (i == TULIP_NUM_TX_DESC - 1)
            dev->tx_descs[i].ctrl |= TULIP_TDES1_TER;
        dev->tx_descs[i].buf1 = (uint32_t)bp;
        dev->tx_descs[i].buf2 = 0;
    }
    dev->tx_cur = 0;
    tulip_csr_write(dev, TULIP_CSR4, (uint32_t)phys);
    return 0;
}

// ============================================================================
// send / link_status / shutdown
// ============================================================================
static int tulip_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    tulip_dev_t* dev = (tulip_dev_t*)ndev->driver_data;
    if (len > TULIP_TX_BUF_SIZE) return -1;

    uint16_t slot = dev->tx_cur;
    volatile tulip_desc_t* desc = &dev->tx_descs[slot];

    if (desc->status & TULIP_TDES0_OWN) {
        ndev->tx_errors++;
        return -1;
    }

    uint16_t tx_len = (len < 60) ? 60 : len;
    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[slot][i] = data[i];
    for (uint16_t i = len; i < tx_len; i++)
        dev->tx_bufs[slot][i] = 0;

    uint32_t ctrl = TULIP_TDES1_FS | TULIP_TDES1_LS | TULIP_TDES1_IC;
    // TDES1.BS1 is 11 bits — anything >= 2048 would wrap.  Our TX
    // buffer is one page and we cap len at TULIP_TX_BUF_SIZE above,
    // and ethernet frames are <= 1518, so this never happens in
    // practice — but mask for safety.
    ctrl |= (uint32_t)(tx_len & 0x7FF);      // BS1 (size of buffer 1)
    if (slot == TULIP_NUM_TX_DESC - 1)
        ctrl |= TULIP_TDES1_TER;
    desc->ctrl = ctrl;
    desc->buf1 = (uint32_t)dev->tx_bufs_phys[slot];
    desc->buf2 = 0;

    __asm__ volatile("mfence" ::: "memory");
    desc->status = TULIP_TDES0_OWN;          // hand off to hardware
    __asm__ volatile("mfence" ::: "memory");

    // Kick TX poll
    tulip_csr_write(dev, TULIP_CSR1, 0xFFFFFFFFu);

    dev->tx_cur = (slot + 1) % TULIP_NUM_TX_DESC;
    ndev->tx_packets++;
    ndev->tx_bytes += len;
    return 0;
}

static int tulip_link_status(net_device_t* ndev) {
    tulip_dev_t* dev = (tulip_dev_t*)ndev->driver_data;
    uint32_t sia = tulip_csr_read(dev, TULIP_CSR12);
    // CSR12.LS10 / LS100 are active-low link-fail bits on the 21143:
    // link is up if either of those is 0 in the appropriate medium.
    // We treat "either bit clear" as link-up — close enough for the
    // generic case, and exactly what QEMU emulates.
    int now_up = (!(sia & TULIP_CSR12_LS10) || !(sia & TULIP_CSR12_LS100)) ? 1 : 0;
    int was_up = dev->link_up;
    if (now_up != was_up) {
        dev->link_up = now_up;
        kprintf("Tulip: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) dhcp_invalidate(ndev);
    }
    return now_up;
}

// Quiesce before ACPI S5: software reset (clears CSR6.ST/SR), mask CSR7,
// drop bus master.
static void tulip_shutdown(net_device_t* ndev) {
    tulip_dev_t* dev = (tulip_dev_t*)ndev->driver_data;
    if (!dev) return;
    if (!dev->use_mmio && !dev->io_base) return;
    if (dev->use_mmio && !dev->mmio_base) return;

    // Mask all interrupts
    tulip_csr_write(dev, TULIP_CSR7, 0);

    // Halt RX/TX by clearing CSR6.SR and CSR6.ST
    uint32_t csr6 = tulip_csr_read(dev, TULIP_CSR6);
    csr6 &= ~(TULIP_CSR6_SR | TULIP_CSR6_ST);
    tulip_csr_write(dev, TULIP_CSR6, csr6);

    // Software reset wipes the chip into a known-quiet state
    tulip_csr_write(dev, TULIP_CSR0, TULIP_CSR0_SWR);

    if (dev->pci_dev) {
        const pci_device_t* pdev = dev->pci_dev;
        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0004u;
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// IRQ handler
// ============================================================================
void tulip_irq_handler(void) {
    if (!g_tulip_initialized) {
        lapic_eoi();
        return;
    }

    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    tulip_dev_t* dev = &g_tulip;
    uint32_t sr = tulip_csr_read(dev, TULIP_CSR5);
    if (sr == 0) {
        lapic_eoi();
        return;
    }
    // Acknowledge by writing 1s back
    tulip_csr_write(dev, TULIP_CSR5, sr);

    if (sr & (TULIP_CSR5_LNF | TULIP_CSR5_LNP)) {
        uint32_t sia = tulip_csr_read(dev, TULIP_CSR12);
        int now_up = (!(sia & TULIP_CSR12_LS10) || !(sia & TULIP_CSR12_LS100)) ? 1 : 0;
        int was_up = dev->link_up;
        dev->link_up = now_up;
        if (now_up != was_up) {
            kprintf("Tulip: Link %s\n", now_up ? "UP" : "DOWN");
            if (!now_up) dhcp_invalidate(&dev->net_dev);
        }
    }

    if (sr & TULIP_CSR5_RI) {
        // Process all completed RX descriptors
        while (1) {
            uint16_t slot = dev->rx_cur;
            volatile tulip_desc_t* desc = &dev->rx_descs[slot];
            if (desc->status & TULIP_RDES0_OWN) break;

            uint32_t st = desc->status;
            if (!(st & TULIP_RDES0_ES) && (st & TULIP_RDES0_FS) && (st & TULIP_RDES0_LS)) {
                uint16_t len = (uint16_t)((st & TULIP_RDES0_FL_MASK) >> TULIP_RDES0_FL_SHIFT);
                // Hardware reports FCS-included length; strip 4 bytes
                if (len > 4) len -= 4;
                if (len > 0 && len <= TULIP_RX_BUF_SIZE) {
                    net_rx_packet(&dev->net_dev, dev->rx_bufs[slot], len);
                } else {
                    dev->net_dev.rx_errors++;
                }
            } else {
                dev->net_dev.rx_errors++;
            }

            // Hand back to hardware
            desc->status = TULIP_RDES0_OWN;
            dev->rx_cur = (slot + 1) % TULIP_NUM_RX_DESC;
        }
        // Kick RX poll in case the chip suspended
        tulip_csr_write(dev, TULIP_CSR2, 0xFFFFFFFFu);
    }

    lapic_eoi();
}

// ============================================================================
// BAR resolution: prefer MMIO (BAR0 even), fall back to PMIO (BAR0 odd).
// ============================================================================
static int tulip_resolve_bar(tulip_dev_t* dev) {
    uint32_t bar0 = dev->pci_dev->bar[0];
    if (bar0 & 1) {
        // I/O port BAR
        dev->use_mmio = 0;
        dev->io_base = (uint16_t)(bar0 & 0xFFFC);
        return 0;
    }
    // Memory-mapped BAR
    dev->use_mmio = 1;
    uint64_t phys = bar0 & 0xFFFFFFF0ULL;
    if ((bar0 & 0x06) == 0x04)
        phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;

    dev->mmio_phys = phys;
    uint32_t size = 256;  // 16 CSRs * 8 bytes ample, but map a full page
    uint64_t virt = (uint64_t)phys_to_virt(phys);
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t pp = (phys & ~((uint64_t)PAGE_SIZE - 1)) + i * PAGE_SIZE;
        uint64_t pv = (virt & ~((uint64_t)PAGE_SIZE - 1)) + i * PAGE_SIZE;
        if (!mm_is_page_mapped(pv)) {
            mm_map_page(pv, pp,
                        PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
        }
    }
    dev->mmio_base = (volatile uint8_t*)virt;
    return 0;
}

// ============================================================================
// Driver entry point
// ============================================================================
void tulip_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        const tulip_id_t* match = tulip_lookup(devs[i].vendor_id, devs[i].device_id);
        if (!match) continue;

        kprintf("Tulip: Found %s (PCI %02x:%02x.%x)\n",
                match->name, devs[i].bus, devs[i].device, devs[i].function);

        tulip_dev_t* dev = &g_tulip;
        dev->pci_dev = &devs[i];
        dev->device_id = devs[i].device_id;

        pci_enable_busmaster_mem(dev->pci_dev);

        if (tulip_resolve_bar(dev) < 0) return;

        // If we landed on PMIO BAR0, also enable I/O Space in PCI Command
        if (!dev->use_mmio) {
            uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                          dev->pci_dev->device,
                                          dev->pci_dev->function, 0x04);
            if (!(cmd & 0x1)) {
                pci_cfg_write32(dev->pci_dev->bus,
                                dev->pci_dev->device,
                                dev->pci_dev->function, 0x04, cmd | 0x1);
            }
            kprintf("Tulip: I/O base 0x%x\n", dev->io_base);
        } else {
            kprintf("Tulip: MMIO base 0x%lx\n", dev->mmio_phys);
        }

        if (tulip_reset(dev) < 0) return;

        tulip_read_mac(dev);
        kprintf("Tulip: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        if (tulip_init_rx(dev) < 0) return;
        if (tulip_init_tx(dev) < 0) return;

        // Resolve legacy IRQ via ACPI _PRT (preferred) or PCI line
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
                kprintf("Tulip: ACPI _PRT resolved INT%c -> GSI %u\n",
                        'A' + acpi_pin, gsi);
            }
        }
        if (irq == 0xFF) {
            irq = dev->pci_dev->interrupt_line;
            if (irq != 0xFF && irq <= 23) {
                kprintf("Tulip: ACPI _PRT lookup failed, falling back to "
                        "PCI interrupt_line = %d\n", irq);
            } else {
                kprintf("Tulip: WARNING: no valid IRQ\n");
            }
        }
        kprintf("Tulip: Using legacy IRQ %d\n", irq);
        g_tulip_legacy_irq = irq;

        {
            uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                          dev->pci_dev->device,
                                          dev->pci_dev->function, 0x04);
            if (cmd & PCI_CMD_INTX_DISABLE) {
                cmd &= ~PCI_CMD_INTX_DISABLE;
                pci_cfg_write32(dev->pci_dev->bus,
                                dev->pci_dev->device,
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
        g_tulip_initialized = 1;

        // Start RX + TX (full duplex, store-and-forward).  Set CSR6.PR
        // (promiscuous) so the chip's perfect/hash address filter is
        // bypassed — we have not (yet) sent a Setup Frame to program
        // the filter, and without one the 21143 drops every incoming
        // packet (including broadcast DHCP OFFERs).  In promiscuous mode
        // the chip delivers all frames to the host and the kernel's
        // ethernet layer does the per-MAC matching itself.
        uint32_t csr6 = TULIP_CSR6_SR | TULIP_CSR6_ST | TULIP_CSR6_FD |
                        TULIP_CSR6_SF | TULIP_CSR6_PR;
        tulip_csr_write(dev, TULIP_CSR6, csr6);

        // Unmask interrupts: RX, TX, link change, normal/abnormal summaries
        tulip_csr_write(dev, TULIP_CSR7,
                        TULIP_CSR7_RIE | TULIP_CSR7_TIE | TULIP_CSR7_RUE |
                        TULIP_CSR7_LFE | TULIP_CSR7_LPE |
                        TULIP_CSR7_NIE | TULIP_CSR7_AIE);

        // Initial link probe
        uint32_t sia = tulip_csr_read(dev, TULIP_CSR12);
        dev->link_up = (!(sia & TULIP_CSR12_LS10) || !(sia & TULIP_CSR12_LS100)) ? 1 : 0;
        kprintf("Tulip: Link %s\n", dev->link_up ? "UP" : "DOWN");

        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = tulip_send;
        dev->net_dev.link_status = tulip_link_status;
        dev->net_dev.shutdown = tulip_shutdown;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);

        kprintf("Tulip: Driver initialized successfully\n");
        return;
    }
}
