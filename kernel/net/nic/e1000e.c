// LikeOS-64 Intel e1000e (PCIe gigabit) NIC Driver
//
// Targets:
//   - Intel 82574L Gigabit Network Connection (PCI 0x10D3) — QEMU `-device e1000e`
//   - Intel 82583V Gigabit Network Connection (PCI 0x150C)
//
// Note: The VirtualBox NIC choices "Intel PRO/1000 MT Desktop" (82540EM)
// and "Intel PRO/1000 MT Server" (82545EM) are e1000-class parts and are
// handled by the separate e1000 driver, not this one.
//
// The 82574L register layout is largely backwards-compatible with the
// classic e1000 (82540EM/82545EM): we reuse the legacy RX/TX descriptor
// formats and the same MMIO offsets defined in <kernel/e1000.h>.  The
// extra PCIe / MSI-X facilities that distinguish e1000e proper from
// e1000 are NOT used here — for our purposes (UEFI desktop systems,
// hypervisor emulation) the legacy programming model is sufficient and
// matches the interrupt/DMA-handling style of the existing e1000 driver.
#include "../../../include/kernel/e1000e.h"
#include "../../../include/kernel/e1000.h"
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

// Single-NIC global (mirrors the e1000 driver layout).
static e1000e_dev_t g_e1000e;
int g_e1000e_initialized = 0;
int g_e1000e_legacy_irq = -1;

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t e1000e_read(e1000e_dev_t* dev, uint32_t reg) {
    return *(volatile uint32_t*)(dev->mmio_base + reg);
}

static inline void e1000e_write(e1000e_dev_t* dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(dev->mmio_base + reg) = val;
    __asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// EEPROM Read
// ============================================================================
static uint16_t e1000e_read_eeprom(e1000e_dev_t* dev, uint8_t addr) {
    e1000e_write(dev, E1000_EERD,
                 (uint32_t)addr << E1000_EERD_ADDR_SHIFT | E1000_EERD_START);
    uint32_t val;
    for (int i = 0; i < 10000; i++) {
        val = e1000e_read(dev, E1000_EERD);
        if (val & E1000_EERD_DONE) {
            return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
        }
    }
    return 0;
}

// ============================================================================
// Read MAC Address
// ============================================================================
static void e1000e_read_mac(e1000e_dev_t* dev) {
    uint16_t w0 = e1000e_read_eeprom(dev, 0);
    uint16_t w1 = e1000e_read_eeprom(dev, 1);
    uint16_t w2 = e1000e_read_eeprom(dev, 2);

    if (w0 != 0 || w1 != 0 || w2 != 0) {
        dev->mac_addr[0] = w0 & 0xFF;
        dev->mac_addr[1] = (w0 >> 8) & 0xFF;
        dev->mac_addr[2] = w1 & 0xFF;
        dev->mac_addr[3] = (w1 >> 8) & 0xFF;
        dev->mac_addr[4] = w2 & 0xFF;
        dev->mac_addr[5] = (w2 >> 8) & 0xFF;
    } else {
        // Fallback: read from RAL/RAH (preprogrammed by firmware)
        uint32_t ral = e1000e_read(dev, E1000_RAL);
        uint32_t rah = e1000e_read(dev, E1000_RAH);
        dev->mac_addr[0] = ral & 0xFF;
        dev->mac_addr[1] = (ral >> 8) & 0xFF;
        dev->mac_addr[2] = (ral >> 16) & 0xFF;
        dev->mac_addr[3] = (ral >> 24) & 0xFF;
        dev->mac_addr[4] = rah & 0xFF;
        dev->mac_addr[5] = (rah >> 8) & 0xFF;
    }
}

// ============================================================================
// PM Timer-based microsecond delay
// ============================================================================
static void e1000e_delay_us(uint32_t us) {
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
// Soft Reset
// ============================================================================
static void e1000e_reset(e1000e_dev_t* dev) {
    // Mask + clear all interrupts
    e1000e_write(dev, E1000_IMC, 0xFFFFFFFF);
    (void)e1000e_read(dev, E1000_ICR);

    // Soft reset of the data path only — leave PHY/link untouched, the UEFI
    // firmware already negotiated.  A full CTRL.RST on e1000e parts can take many
    // seconds for link-up under hypervisors and breaks RX in some
    // some hypervisor builds, exactly as on the older e1000.
    e1000e_write(dev, E1000_RCTL, 0);
    e1000e_write(dev, E1000_TCTL, 0);

    e1000e_delay_us(10000);

    e1000e_write(dev, E1000_IMC, 0xFFFFFFFF);
    (void)e1000e_read(dev, E1000_ICR);
}

// ============================================================================
// RX Ring Initialization
// ============================================================================
static int e1000e_init_rx(e1000e_dev_t* dev) {
    uint32_t rx_ring_size = sizeof(e1000_rx_desc_legacy_t) * E1000E_NUM_RX_DESC;
    uint32_t rx_ring_pages = (rx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t rx_phys = mm_allocate_contiguous_pages(rx_ring_pages);
    if (!rx_phys) {
        kprintf("E1000E: Failed to allocate RX descriptor ring\n");
        return -1;
    }

    dev->rx_descs = (e1000_rx_desc_legacy_t*)phys_to_virt(rx_phys);
    dev->rx_descs_phys = rx_phys;

    for (uint32_t i = 0; i < rx_ring_size / 8; i++)
        ((uint64_t*)dev->rx_descs)[i] = 0;

    for (int i = 0; i < E1000E_NUM_RX_DESC; i++) {
        uint64_t buf_phys = mm_allocate_physical_page();
        if (!buf_phys) {
            kprintf("E1000E: Failed to allocate RX buffer %d\n", i);
            return -1;
        }
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(buf_phys);
        dev->rx_bufs_phys[i] = buf_phys;

        dev->rx_descs[i].buffer_addr = buf_phys;
        dev->rx_descs[i].status = 0;
    }

    e1000e_write(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000e_write(dev, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000e_write(dev, E1000_RDLEN, rx_ring_size);
    e1000e_write(dev, E1000_RDH, 0);
    e1000e_write(dev, E1000_RDT, E1000E_NUM_RX_DESC - 1);

    uint32_t rctl = E1000_RCTL_EN
                  | E1000_RCTL_BAM
                  | E1000_RCTL_BSIZE_2048
                  | E1000_RCTL_SECRC;
    e1000e_write(dev, E1000_RCTL, rctl);

    dev->rx_tail = E1000E_NUM_RX_DESC - 1;
    return 0;
}

// ============================================================================
// TX Ring Initialization
// ============================================================================
static int e1000e_init_tx(e1000e_dev_t* dev) {
    uint32_t tx_ring_size = sizeof(e1000_tx_desc_legacy_t) * E1000E_NUM_TX_DESC;
    uint32_t tx_ring_pages = (tx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t tx_phys = mm_allocate_contiguous_pages(tx_ring_pages);
    if (!tx_phys) {
        kprintf("E1000E: Failed to allocate TX descriptor ring\n");
        return -1;
    }

    dev->tx_descs = (e1000_tx_desc_legacy_t*)phys_to_virt(tx_phys);
    dev->tx_descs_phys = tx_phys;

    for (uint32_t i = 0; i < tx_ring_size / 8; i++)
        ((uint64_t*)dev->tx_descs)[i] = 0;

    for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
        uint64_t buf_phys = mm_allocate_physical_page();
        if (!buf_phys) {
            kprintf("E1000E: Failed to allocate TX buffer %d\n", i);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(buf_phys);
        dev->tx_bufs_phys[i] = buf_phys;
    }

    e1000e_write(dev, E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000e_write(dev, E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000e_write(dev, E1000_TDLEN, tx_ring_size);
    e1000e_write(dev, E1000_TDH, 0);
    e1000e_write(dev, E1000_TDT, 0);

    e1000e_write(dev, E1000_TIPG,
                 E1000_TIPG_IPGT | E1000_TIPG_IPGR1 | E1000_TIPG_IPGR2);

    uint32_t tctl = E1000_TCTL_EN
                  | E1000_TCTL_PSP
                  | (15 << E1000_TCTL_CT_SHIFT)
                  | (64 << E1000_TCTL_COLD_SHIFT)
                  | E1000_TCTL_RTLC;
    e1000e_write(dev, E1000_TCTL, tctl);

    dev->tx_tail = 0;
    return 0;
}

// ============================================================================
// Send (net_device_t callback)
// ============================================================================
static int e1000e_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    e1000e_dev_t* dev = (e1000e_dev_t*)ndev->driver_data;

    if (len > E1000E_RX_BUF_SIZE) return -1;

    uint16_t tail = dev->tx_tail;
    volatile e1000_tx_desc_legacy_t* desc = &dev->tx_descs[tail];
    if (tail != 0 || desc->cmd != 0) {
        if (!(desc->status & E1000_TXD_STAT_DD) && desc->cmd != 0) {
            ndev->tx_errors++;
            return -1;
        }
    }

    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[tail][i] = data[i];

    desc->buffer_addr = dev->tx_bufs_phys[tail];
    desc->length = len;
    desc->cso = 0;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    __asm__ volatile("mfence" ::: "memory");

    dev->tx_tail = (tail + 1) % E1000E_NUM_TX_DESC;
    e1000e_write(dev, E1000_TDT, dev->tx_tail);

    ndev->tx_packets++;
    ndev->tx_bytes += len;
    return 0;
}

// ============================================================================
// Link Status (net_device_t callback)
// ============================================================================
static int e1000e_link_status(net_device_t* ndev) {
    e1000e_dev_t* dev = (e1000e_dev_t*)ndev->driver_data;
    uint32_t status = e1000e_read(dev, E1000_STATUS);
    return (status & E1000_STATUS_LU) ? 1 : 0;
}

// ============================================================================
// IRQ Handler
// ============================================================================
void e1000e_irq_handler(void) {
    if (!g_e1000e_initialized) {
        lapic_eoi();
        return;
    }

    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    e1000e_dev_t* dev = &g_e1000e;
    uint32_t icr = e1000e_read(dev, E1000_ICR);

    if (icr == 0) {
        lapic_eoi();
        return;
    }

    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000e_read(dev, E1000_STATUS);
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        kprintf("E1000E: Link %s\n", dev->link_up ? "UP" : "DOWN");
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
        uint16_t tail = (dev->rx_tail + 1) % E1000E_NUM_RX_DESC;

        while (1) {
            volatile e1000_rx_desc_legacy_t* desc = &dev->rx_descs[tail];
            if (!(desc->status & E1000_RXD_STAT_DD))
                break;

            uint16_t len = desc->length;
            if ((desc->status & E1000_RXD_STAT_EOP) && len > 0 && len <= E1000E_RX_BUF_SIZE) {
                net_rx_packet(&dev->net_dev, dev->rx_bufs[tail], len);
            } else {
                dev->net_dev.rx_errors++;
            }

            desc->buffer_addr = dev->rx_bufs_phys[tail];
            desc->status = 0;

            dev->rx_tail = tail;
            e1000e_write(dev, E1000_RDT, tail);

            tail = (tail + 1) % E1000E_NUM_RX_DESC;
        }
    }

    // TXDW is observed via DD bit in send(); nothing else to do here.

    lapic_eoi();
}

// ============================================================================
// Map MMIO BAR
// ============================================================================
static int e1000e_map_bar(e1000e_dev_t* dev) {
    uint32_t bar0 = dev->pci_dev->bar[0];

    if (bar0 & 1) {
        kprintf("E1000E: BAR0 is I/O port, not MMIO\n");
        return -1;
    }

    uint64_t phys = bar0 & 0xFFFFFFF0ULL;
    if ((bar0 & 0x06) == 0x04) {
        phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
    }

    dev->mmio_phys = phys;
    dev->mmio_size = 128 * 1024;

    uint64_t virt_base = (uint64_t)phys_to_virt(phys);
    uint32_t num_pages = (dev->mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt_base + i * PAGE_SIZE;
        if (!mm_is_page_mapped(page_virt)) {
            mm_map_page(page_virt, page_phys,
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
        }
    }

    dev->mmio_base = (volatile uint8_t*)virt_base;
    return 0;
}

// ============================================================================
// Driver Initialization
// ============================================================================
void e1000e_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != E1000E_VENDOR_ID) continue;

        uint16_t did = devs[i].device_id;
        if (did != E1000E_DEV_82574L && did != E1000E_DEV_82583V)
            continue;

        const char* name = "e1000e";
        if (did == E1000E_DEV_82574L) name = "82574L";
        else if (did == E1000E_DEV_82583V) name = "82583V";

        kprintf("E1000E: Found %s (PCI %02x:%02x.%x)\n",
                name, devs[i].bus, devs[i].device, devs[i].function);

        e1000e_dev_t* dev = &g_e1000e;
        dev->pci_dev = &devs[i];
        dev->device_id = did;

        pci_enable_busmaster_mem(dev->pci_dev);

        if (e1000e_map_bar(dev) < 0) {
            kprintf("E1000E: Failed to map BAR0\n");
            return;
        }

        e1000e_reset(dev);

        e1000e_read_mac(dev);
        kprintf("E1000E: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        // Set link up — preserve UEFI speed/duplex, only assert SLU.
        uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
        ctrl |= E1000_CTRL_SLU;
        ctrl &= ~E1000_CTRL_PHY_RST;
        e1000e_write(dev, E1000_CTRL, ctrl);

        // Clear multicast table
        for (int m = 0; m < 128; m++)
            e1000e_write(dev, E1000_MTA + m * 4, 0);

        uint32_t ral = (uint32_t)dev->mac_addr[0]
                     | ((uint32_t)dev->mac_addr[1] << 8)
                     | ((uint32_t)dev->mac_addr[2] << 16)
                     | ((uint32_t)dev->mac_addr[3] << 24);
        uint32_t rah = (uint32_t)dev->mac_addr[4]
                     | ((uint32_t)dev->mac_addr[5] << 8)
                     | (1u << 31);
        e1000e_write(dev, E1000_RAL, ral);
        e1000e_write(dev, E1000_RAH, rah);

        if (e1000e_init_rx(dev) < 0) return;
        if (e1000e_init_tx(dev) < 0) return;

        // ------------------------------------------------------------------
        // MSI first; legacy INTx with full ACPI _PRT lookup as fallback.
        // Same logic as the older e1000 driver — see comments there for
        // the rationale (notably: PCI interrupt_line is NOT the IOAPIC GSI
        // in APIC mode; the BIOS-assigned PIC IRQ value must NOT be
        // trusted on ICH9-class systems).
        // ------------------------------------------------------------------
        dev->msi_vector = E1000E_MSI_VECTOR;
        int msi_ret = pci_enable_msi(dev->pci_dev, dev->msi_vector);
        if (msi_ret < 0) {
            dev->msi_vector = 0;

            uint8_t irq = 0xFF;
            uint32_t gsi = 0;
            uint8_t pin = dev->pci_dev->interrupt_pin;
            if (pin >= 1 && pin <= 4) {
                uint8_t acpi_pin = pin - 1;
                uint8_t lookup_dev = dev->pci_dev->device;
                uint8_t lookup_pin = acpi_pin;

                if (dev->pci_dev->bus != 0) {
                    const pci_device_t *bridge = pci_find_bridge_for_bus(dev->pci_dev->bus);
                    if (bridge) {
                        lookup_pin = (acpi_pin + dev->pci_dev->device) % 4;
                        lookup_dev = bridge->device;
                    }
                }

                if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
                                        lookup_dev, lookup_pin,
                                        &gsi) == 0 && gsi <= 23) {
                    irq = (uint8_t)gsi;
                    kprintf("E1000E: ACPI _PRT resolved INT%c -> GSI %u\n",
                            'A' + acpi_pin, gsi);
                }
            }

            if (irq == 0xFF) {
                irq = dev->pci_dev->interrupt_line;
                if (irq != 0xFF && irq <= 23) {
                    kprintf("E1000E: ACPI _PRT lookup failed, falling back to "
                            "PCI interrupt_line = %d\n", irq);
                } else {
                    kprintf("E1000E: WARNING: no valid IRQ (line=%d, _PRT failed)\n",
                            dev->pci_dev->interrupt_line);
                    kprintf("E1000E: Interrupts will not work\n");
                }
            }

            kprintf("E1000E: Using legacy IRQ %d\n", irq);
            g_e1000e_legacy_irq = irq;

            // Some firmware leaves PCI Command
            // bit 10 (INTx Disable) set, preventing the device from ever
            // raising its interrupt pin.  Clear it for the legacy path.
            {
                uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                              dev->pci_dev->device,
                                              dev->pci_dev->function, 0x04);
                if (cmd & PCI_CMD_INTX_DISABLE) {
                    cmd &= ~PCI_CMD_INTX_DISABLE;
                    pci_cfg_write32(dev->pci_dev->bus,
                                    dev->pci_dev->device,
                                    dev->pci_dev->function, 0x04, cmd);
                    kprintf("E1000E: cleared PCI Command INTx Disable bit\n");
                }
            }

            if (irq <= 23) {
                uint8_t vector = 32 + irq;
                ioapic_configure_legacy_irq(irq, vector,
                                            IOAPIC_POLARITY_LOW,
                                            IOAPIC_TRIGGER_LEVEL);
            }
        } else {
            kprintf("E1000E: MSI enabled (vector %d)\n", dev->msi_vector);
        }

        // Mark the driver initialised before unmasking IMS — the IRQ
        // dispatcher only routes to e1000e_irq_handler() when this flag
        // is set; otherwise the vector may collide with the I2C LPSS
        // dispatch range and lock up the system in a level-triggered
        // IRQ storm (same problem we hit with the e1000 driver).
        dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("e1000e");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;
        g_e1000e_initialized = 1;

        e1000e_write(dev, E1000_IMS,
                     E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_LSC |
                     E1000_ICR_RXDMT0 | E1000_ICR_RXO);

        // Wait up to 5 s for link-up (some hypervisor links can take a while).
        uint32_t status = e1000e_read(dev, E1000_STATUS);
        if (!(status & E1000_STATUS_LU)) {
            uint32_t t0 = timer_pmtimer_read_raw();
            while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < 5000000) {
                e1000e_delay_us(50000);
                status = e1000e_read(dev, E1000_STATUS);
                if (status & E1000_STATUS_LU) break;
            }
        }
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        kprintf("E1000E: Link %s\n", dev->link_up ? "UP" : "DOWN");

        // net_device_t setup
        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = e1000e_send;
        dev->net_dev.link_status = e1000e_link_status;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);

        kprintf("E1000E: Driver initialized successfully\n");
        return;  // Only handle first matching device
    }
}
