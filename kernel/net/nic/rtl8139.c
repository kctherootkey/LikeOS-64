// LikeOS-64 Realtek RTL8139 NIC Driver
//
// The RTL8139 is a venerable 10/100 Mbit/s PCI Ethernet controller. It is
// supported by QEMU (`-device rtl8139`) and most physical RTL8139C /
// RTL8139C+ cards.  It uses I/O-port (PMIO) registers (BAR0) and a single
// linear ring buffer for RX with four 2 KiB transmit slots.

#include "../../../include/kernel/rtl8139.h"
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

// Global single-NIC state
static rtl8139_dev_t g_rtl8139;
int g_rtl8139_initialized = 0;
int g_rtl8139_legacy_irq = -1;

// ============================================================================
// I/O helpers (RTL8139 uses PMIO BAR0)
// ============================================================================
static inline void rtl_write8(rtl8139_dev_t* dev, uint16_t reg, uint8_t v) {
    outb(dev->io_base + reg, v);
}
static inline void rtl_write16(rtl8139_dev_t* dev, uint16_t reg, uint16_t v) {
    outw(dev->io_base + reg, v);
}
static inline void rtl_write32(rtl8139_dev_t* dev, uint16_t reg, uint32_t v) {
    outl(dev->io_base + reg, v);
}
static inline uint8_t rtl_read8(rtl8139_dev_t* dev, uint16_t reg) {
    return inb(dev->io_base + reg);
}
static inline uint16_t rtl_read16(rtl8139_dev_t* dev, uint16_t reg) {
    return inw(dev->io_base + reg);
}
static inline uint32_t rtl_read32(rtl8139_dev_t* dev, uint16_t reg) {
    return inl(dev->io_base + reg);
}

// ============================================================================
// PM Timer microsecond delay (mirrors e1000)
// ============================================================================
static void rtl_delay_us(uint32_t us) {
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
static void rtl_read_mac(rtl8139_dev_t* dev) {
    for (int i = 0; i < 6; i++)
        dev->mac_addr[i] = rtl_read8(dev, RTL_IDR0 + i);
}

// ============================================================================
// Soft reset
// ============================================================================
static int rtl_reset(rtl8139_dev_t* dev) {
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
static int rtl_init_rx(rtl8139_dev_t* dev) {
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
        kprintf("RTL8139: ERROR: RX buffer phys 0x%lx is above 4GB\n", phys);
        return -1;
    }

    dev->rx_buf = (uint8_t*)phys_to_virt(phys);
    dev->rx_buf_phys = phys;
    dev->rx_offset = 0;

    // Zero the buffer
    for (uint32_t i = 0; i < RTL8139_RX_BUF_TOTAL; i++)
        dev->rx_buf[i] = 0;

    // Point hardware at it
    rtl_write32(dev, RTL_RBSTART, (uint32_t)phys);
    return 0;
}

// ============================================================================
// TX init — allocate four 2 KiB slot buffers
// ============================================================================
static int rtl_init_tx(rtl8139_dev_t* dev) {
    for (int i = 0; i < RTL8139_NUM_TX_DESC; i++) {
        uint64_t phys = mm_allocate_physical_page();
        if (!phys) {
            kprintf("RTL8139: failed to allocate TX buffer %d\n", i);
            return -1;
        }
        if (phys >> 32) {
            kprintf("RTL8139: ERROR: TX buffer phys 0x%lx above 4GB\n", phys);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(phys);
        dev->tx_bufs_phys[i] = phys;
        rtl_write32(dev, RTL_TSAD0 + i * 4, (uint32_t)phys);
    }
    dev->tx_cur = 0;
    return 0;
}

// ============================================================================
// Send a single Ethernet frame (net_device_t callback)
// ============================================================================
static int rtl8139_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    rtl8139_dev_t* dev = (rtl8139_dev_t*)ndev->driver_data;

    if (len > RTL8139_TX_BUF_SIZE) return -1;
    if (len < 60) {
        // Hardware does not pad short frames in software descriptor mode.
        // We zero-pad to the 60-byte (sans-FCS) minimum here.
        for (uint16_t i = len; i < 60; i++)
            dev->tx_bufs[dev->tx_cur][i] = 0;
        // 'len' is what we actually copied; we still tell the HW to send 60.
    }

    // Wait for descriptor to be free (OWN=1 means previous DMA done).  On
    // first use TSD reads back as 0 so we treat that as "free".
    uint8_t slot = dev->tx_cur;
    uint32_t tsd_off = RTL_TSD0 + slot * 4;
    uint32_t tsd = rtl_read32(dev, tsd_off);
    if (tsd != 0 && !(tsd & RTL_TSD_OWN)) {
        // Descriptor still busy — drop
        ndev->tx_errors++;
        return -1;
    }

    // Copy payload into the slot buffer
    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[slot][i] = data[i];

    uint16_t tx_len = (len < 60) ? 60 : len;

    // Ensure prior writes are visible to the device DMA before kicking it.
    __asm__ volatile("mfence" ::: "memory");

    // Writing the size to TSD with OWN=0 starts the transmission.
    // We leave Early TX Threshold at 0 (= use default).
    rtl_write32(dev, tsd_off, (uint32_t)tx_len & RTL_TSD_SIZE_MASK);

    dev->tx_cur = (slot + 1) % RTL8139_NUM_TX_DESC;
    ndev->tx_packets++;
    ndev->tx_bytes += tx_len;
    return 0;
}

// ============================================================================
// Link status callback
// ============================================================================
static int rtl8139_link_status(net_device_t* ndev) {
    rtl8139_dev_t* dev = (rtl8139_dev_t*)ndev->driver_data;
    // MSR.LINKB is INVERSE: 1 means link DOWN.
    return (rtl_read8(dev, RTL_MSR) & RTL_MSR_LINKB) ? 0 : 1;
}

// ============================================================================
// Drain the RX ring buffer
// ============================================================================
static void rtl_drain_rx(rtl8139_dev_t* dev) {
    while (!(rtl_read8(dev, RTL_CR) & RTL_CR_BUFE)) {
        // Each packet in the ring is laid out as:
        //   [0..1]  status (16-bit, little-endian)
        //   [2..3]  length INCLUDING 4-byte CRC (16-bit)
        //   [4..]   packet data (length-4 bytes)
        uint8_t* p = dev->rx_buf + dev->rx_offset;
        uint16_t status = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        uint16_t length = (uint16_t)p[2] | ((uint16_t)p[3] << 8);

        // Bit 0 of status is ROK; if not set or length is bogus, reset RX.
        if (!(status & 0x01) || length < 4 || length > 1518) {
            dev->net_dev.rx_errors++;
            // Reinitialise RX path on a corrupt header
            rtl_write8(dev, RTL_CR, RTL_CR_TE);  // disable RX only
            rtl_write32(dev, RTL_RBSTART, (uint32_t)dev->rx_buf_phys);
            dev->rx_offset = 0;
            rtl_write8(dev, RTL_CR, RTL_CR_TE | RTL_CR_RE);
            return;
        }

        // Deliver to upper layers (length includes 4-byte CRC; strip it)
        if (length >= 4) {
            uint16_t payload = length - 4;
            net_rx_packet(&dev->net_dev, p + 4, payload);
        }

        // Advance offset, 4-byte aligned, accounting for 4-byte header
        dev->rx_offset = (dev->rx_offset + length + 4 + 3) & ~3;
        if (dev->rx_offset >= RTL8139_RX_BUF_SIZE)
            dev->rx_offset -= RTL8139_RX_BUF_SIZE;

        // CAPR is offset by 16 bytes from the buffer start (hardware quirk).
        rtl_write16(dev, RTL_CAPR, dev->rx_offset - 16);
    }
}

// ============================================================================
// Top-half IRQ handler (called from kernel/ke/interrupt.c dispatcher)
// ============================================================================
void rtl8139_irq_handler(void) {
    if (!g_rtl8139_initialized) {
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

    rtl8139_dev_t* dev = &g_rtl8139;
    uint16_t isr = rtl_read16(dev, RTL_ISR);
    if (isr == 0) {
        lapic_eoi();
        return;
    }

    // ACK by writing 1s to the bits we handled (write-1-to-clear).
    rtl_write16(dev, RTL_ISR, isr);

    if (isr & (RTL_INT_PUN | RTL_INT_LENCHG)) {
        dev->link_up = (rtl_read8(dev, RTL_MSR) & RTL_MSR_LINKB) ? 0 : 1;
        kprintf("RTL8139: Link %s\n", dev->link_up ? "UP" : "DOWN");
    }

    if (isr & RTL_INT_ROK) {
        rtl_drain_rx(dev);
    }
    if (isr & RTL_INT_RER) {
        dev->net_dev.rx_errors++;
    }
    if (isr & (RTL_INT_RXOVW | RTL_INT_FOVW)) {
        // Overflow — reset RX pointer
        dev->net_dev.rx_dropped++;
        rtl_write8(dev, RTL_CR, RTL_CR_TE);
        rtl_write32(dev, RTL_RBSTART, (uint32_t)dev->rx_buf_phys);
        dev->rx_offset = 0;
        rtl_write8(dev, RTL_CR, RTL_CR_TE | RTL_CR_RE);
    }
    // TOK / TER are handled implicitly by send() polling OWN bit; nothing
    // to do here.

    lapic_eoi();
}

// ============================================================================
// Resolve I/O BAR (RTL8139's BAR0 is PMIO)
// ============================================================================
static int rtl_resolve_iobase(rtl8139_dev_t* dev) {
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
void rtl8139_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != RTL8139_VENDOR_ID) continue;
        if (devs[i].device_id != RTL8139_DEV_RTL8139) continue;

        kprintf("RTL8139: Found RTL8139 (PCI %02x:%02x.%x)\n",
                devs[i].bus, devs[i].device, devs[i].function);

        rtl8139_dev_t* dev = &g_rtl8139;
        dev->pci_dev = &devs[i];

        pci_enable_busmaster_mem(dev->pci_dev);

        // pci_enable_busmaster_mem() only sets MEM (bit 1) + Bus Master (bit 2).
        // RTL8139's BAR0 is I/O space, so we must also set I/O Space Enable
        // (bit 0) — without it, inb/inw/inl from io_base return 0xFF.
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

        if (rtl_resolve_iobase(dev) < 0) return;
        kprintf("RTL8139: I/O base 0x%x\n", dev->io_base);

        if (rtl_reset(dev) < 0) return;

        rtl_read_mac(dev);
        kprintf("RTL8139: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        if (rtl_init_rx(dev) < 0) return;
        if (rtl_init_tx(dev) < 0) return;

        // Configure RX: accept broadcast+physical-match, 8K buffer, WRAP set
        // so we can read past the buffer end into our pad area.
        rtl_write32(dev, RTL_RCR,
                    RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_AM |
                    RTL_RCR_WRAP | RTL_RCR_RBLEN_8K |
                    RTL_RCR_MXDMA_UNLIM);

        // Configure TX: max DMA burst 1024 B, normal IFG.
        rtl_write32(dev, RTL_TCR, RTL_TCR_MXDMA_1024 | RTL_TCR_IFG_NORMAL);

        // RTL8139 has no MSI capability in the real silicon nor in QEMU's
        // emulation, but we still try — pci_enable_msi() will fail
        // gracefully and we'll fall through to the legacy INTx path that
        // mirrors the e1000 driver.
        dev->msi_vector = 0;
        int msi_ret = -1;  // pci_enable_msi(dev->pci_dev, RTL8139_MSI_VECTOR);
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
                    const pci_device_t* bridge = pci_find_bridge_for_bus(dev->pci_dev->bus);
                    if (bridge) {
                        lookup_pin = (acpi_pin + dev->pci_dev->device) % 4;
                        lookup_dev = bridge->device;
                    }
                }
                if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
                                        lookup_dev, lookup_pin,
                                        &gsi) == 0 && gsi <= 23) {
                    irq = (uint8_t)gsi;
                    kprintf("RTL8139: ACPI _PRT resolved INT%c -> GSI %u\n",
                            'A' + acpi_pin, gsi);
                }
            }
            if (irq == 0xFF) {
                irq = dev->pci_dev->interrupt_line;
                if (irq != 0xFF && irq <= 23) {
                    kprintf("RTL8139: ACPI _PRT lookup failed, falling back "
                            "to PCI interrupt_line = %d\n", irq);
                } else {
                    kprintf("RTL8139: WARNING: no valid IRQ\n");
                }
            }
            kprintf("RTL8139: Using legacy IRQ %d\n", irq);
            g_rtl8139_legacy_irq = irq;

            // Clear PCI Command INTx Disable (some firmwares set it).
            uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                          dev->pci_dev->device,
                                          dev->pci_dev->function, 0x04);
            if (cmd & PCI_CMD_INTX_DISABLE) {
                cmd &= ~PCI_CMD_INTX_DISABLE;
                pci_cfg_write32(dev->pci_dev->bus,
                                dev->pci_dev->device,
                                dev->pci_dev->function, 0x04, cmd);
                kprintf("RTL8139: cleared PCI Command INTx Disable bit\n");
            }

            if (irq <= 23) {
                uint8_t vector = 32 + irq;
                ioapic_configure_legacy_irq(irq, vector,
                                            IOAPIC_POLARITY_LOW,
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
        g_rtl8139_initialized = 1;

        // Enable RX + TX
        rtl_write8(dev, RTL_CR, RTL_CR_TE | RTL_CR_RE);

        // Reset RX read pointer
        rtl_write16(dev, RTL_CAPR, 0xFFF0);  // -16
        dev->rx_offset = 0;

        // Clear and then enable interrupts we care about
        rtl_write16(dev, RTL_ISR, 0xFFFF);
        rtl_write16(dev, RTL_IMR,
                    RTL_INT_ROK | RTL_INT_RER |
                    RTL_INT_TOK | RTL_INT_TER |
                    RTL_INT_RXOVW | RTL_INT_FOVW |
                    RTL_INT_PUN | RTL_INT_LENCHG);

        // Initial link status (give the link a moment to come up)
        uint8_t msr = rtl_read8(dev, RTL_MSR);
        if (msr & RTL_MSR_LINKB) {
            uint32_t t0 = timer_pmtimer_read_raw();
            while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < 5000000) {
                rtl_delay_us(50000);
                msr = rtl_read8(dev, RTL_MSR);
                if (!(msr & RTL_MSR_LINKB)) break;
            }
        }
        dev->link_up = (msr & RTL_MSR_LINKB) ? 0 : 1;
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
        dev->net_dev.send = rtl8139_send;
        dev->net_dev.link_status = rtl8139_link_status;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);
        kprintf("RTL8139: Driver initialized successfully\n");
        return;  // first device only
    }
}
