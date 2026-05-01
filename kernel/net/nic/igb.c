// LikeOS-64 Intel 82576 Gigabit Ethernet Controller (igb) NIC Driver
//
// Drives the 82575 / 82576 / I350 / I210 / I211 family.  QEMU exposes
// `-device igb` as a 82576 (vendor 0x8086, device 0x10C9).  The 8257x
// classic register layout (CTRL/STATUS/RCTL/TCTL/RAL/RAH/IMS/ICR) is
// preserved on the 82576; the per-queue rings live at 0xC000/0xE000
// instead of 0x2800/0x3800 and need their RXDCTL/TXDCTL.ENABLE bit
// asserted.  We use legacy INTx for IRQ delivery (no MSI vector slot
// is reserved for igb).

#include "../../../include/kernel/igb.h"
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
typedef struct { uint16_t did; const char* name; } igb_id_t;

static const igb_id_t igb_pci_ids[] = {
    // 82575
    { IGB_DEV_82575EB_COPPER, "Intel 82575EB Copper"      },
    { IGB_DEV_82575EB_FIBER,  "Intel 82575EB Fiber"       },
    { IGB_DEV_82575GB_QUAD,   "Intel 82575GB Quad Copper" },
    // 82576
    { IGB_DEV_82576,            "Intel 82576 Gigabit Ethernet Controller" }, // QEMU -device igb
    { IGB_DEV_82576_FIBER,      "Intel 82576 Fiber"          },
    { IGB_DEV_82576_SERDES,     "Intel 82576 SerDes"         },
    { IGB_DEV_82576_QUAD,       "Intel 82576 Quad Copper"    },
    { IGB_DEV_82576_NS,         "Intel 82576 NS"             },
    { IGB_DEV_82576_NS_SERDES,  "Intel 82576 NS SerDes"      },
    { IGB_DEV_82576_SERDES_QD,  "Intel 82576 Backplane SerDes" },
    { IGB_DEV_82576_QUAD_ET2,   "Intel 82576 Quad Copper ET2"  },
    // I350
    { IGB_DEV_I350_COPPER, "Intel I350 Copper" },
    { IGB_DEV_I350_FIBER,  "Intel I350 Fiber"  },
    { IGB_DEV_I350_SERDES, "Intel I350 SerDes" },
    { IGB_DEV_I350_SGMII,  "Intel I350 SGMII"  },
    // I210/I211
    { IGB_DEV_I210_COPPER, "Intel I210 Copper" },
    { IGB_DEV_I210_FIBER,  "Intel I210 Fiber"  },
    { IGB_DEV_I210_SERDES, "Intel I210 SerDes" },
    { IGB_DEV_I210_SGMII,  "Intel I210 SGMII"  },
    { IGB_DEV_I211_COPPER, "Intel I211 Copper" },
    { 0, NULL },
};

static const igb_id_t* igb_lookup(uint16_t did) {
    for (const igb_id_t* e = igb_pci_ids; e->name; e++) {
        if (e->did == did) return e;
    }
    return NULL;
}

static igb_dev_t g_igb;
int g_igb_initialized = 0;
int g_igb_legacy_irq = -1;

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t igb_read(igb_dev_t* dev, uint32_t reg) {
    return *(volatile uint32_t*)(dev->mmio_base + reg);
}
static inline void igb_write(igb_dev_t* dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(dev->mmio_base + reg) = val;
    __asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void igb_delay_us(uint32_t us) {
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
// Read MAC from RAL0 / RAH0 (the 82576 stores the factory MAC there at
// reset time; EEPROM read is much messier on 82576+)
// ============================================================================
static void igb_read_mac(igb_dev_t* dev) {
    uint32_t ral = igb_read(dev, IGB_RAL0);
    uint32_t rah = igb_read(dev, IGB_RAH0);
    dev->mac_addr[0] = ral & 0xFF;
    dev->mac_addr[1] = (ral >> 8) & 0xFF;
    dev->mac_addr[2] = (ral >> 16) & 0xFF;
    dev->mac_addr[3] = (ral >> 24) & 0xFF;
    dev->mac_addr[4] = rah & 0xFF;
    dev->mac_addr[5] = (rah >> 8) & 0xFF;
}

// ============================================================================
// Soft init (do not assert CTRL.RST — leave UEFI's PHY negotiation alone)
// ============================================================================
static void igb_reset(igb_dev_t* dev) {
    igb_write(dev, IGB_IMC, 0xFFFFFFFFu);
    (void)igb_read(dev, IGB_ICR);

    // Disable RX/TX for clean ring setup
    igb_write(dev, IGB_RCTL, 0);
    igb_write(dev, IGB_TCTL, 0);

    igb_delay_us(10000);

    igb_write(dev, IGB_IMC, 0xFFFFFFFFu);
    (void)igb_read(dev, IGB_ICR);
}

// ============================================================================
// RX ring (queue 0)
// ============================================================================
static int igb_init_rx(igb_dev_t* dev) {
    uint32_t ring_bytes = sizeof(igb_rx_desc_t) * IGB_NUM_RX_DESC;
    uint32_t ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys = mm_allocate_contiguous_pages(ring_pages);
    if (!phys) {
        kprintf("IGB: Failed to allocate RX ring\n");
        return -1;
    }
    dev->rx_descs = (igb_rx_desc_t*)phys_to_virt(phys);
    dev->rx_descs_phys = phys;

    for (uint32_t i = 0; i < ring_bytes / 8; i++)
        ((uint64_t*)dev->rx_descs)[i] = 0;

    for (int i = 0; i < IGB_NUM_RX_DESC; i++) {
        uint64_t bp = mm_allocate_physical_page();
        if (!bp) {
            kprintf("IGB: Failed to allocate RX buffer %d\n", i);
            return -1;
        }
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(bp);
        dev->rx_bufs_phys[i] = bp;
        // Advanced descriptor read format: pkt_addr / hdr_addr.
        dev->rx_descs[i].read.pkt_addr = bp;
        dev->rx_descs[i].read.hdr_addr = 0;
    }

    // Force single-queue mode (no RSS), so all incoming packets land in
    // queue 0 — the only ring we set up.
    igb_write(dev, IGB_MRQC, 0);

    igb_write(dev, IGB_RDBAL0, (uint32_t)(phys & 0xFFFFFFFF));
    igb_write(dev, IGB_RDBAH0, (uint32_t)(phys >> 32));
    igb_write(dev, IGB_RDLEN0, ring_bytes);
    igb_write(dev, IGB_RDH0, 0);
    igb_write(dev, IGB_RDT0, IGB_NUM_RX_DESC - 1);

    // Configure per-queue RX: 2 KiB packet buffer, advanced one-buffer
    // descriptor format.  RCTL.BSIZE is ignored on the 82576 in advanced
    // mode — SRRCTL is the source of truth for the queue's buffer size.
    igb_write(dev, IGB_SRRCTL0,
              (2u << IGB_SRRCTL_BSIZEPACKET_SHIFT) |
              IGB_SRRCTL_DESCTYPE_ADV1BUF);

    // Enable queue 0
    uint32_t rxdctl = igb_read(dev, IGB_RXDCTL0);
    rxdctl |= IGB_QUEUE_ENABLE;
    igb_write(dev, IGB_RXDCTL0, rxdctl);

    // Wait for queue enable to stick (datasheet: poll bit 25)
    for (int i = 0; i < 1000; i++) {
        if (igb_read(dev, IGB_RXDCTL0) & IGB_QUEUE_ENABLE) break;
        igb_delay_us(100);
    }

    igb_write(dev, IGB_RCTL,
              IGB_RCTL_EN | IGB_RCTL_BAM | IGB_RCTL_BSIZE_2048 | IGB_RCTL_SECRC);

    dev->rx_tail = IGB_NUM_RX_DESC - 1;
    return 0;
}

// ============================================================================
// TX ring (queue 0)
// ============================================================================
static int igb_init_tx(igb_dev_t* dev) {
    uint32_t ring_bytes = sizeof(igb_tx_desc_t) * IGB_NUM_TX_DESC;
    uint32_t ring_pages = (ring_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t phys = mm_allocate_contiguous_pages(ring_pages);
    if (!phys) {
        kprintf("IGB: Failed to allocate TX ring\n");
        return -1;
    }
    dev->tx_descs = (igb_tx_desc_t*)phys_to_virt(phys);
    dev->tx_descs_phys = phys;

    for (uint32_t i = 0; i < ring_bytes / 8; i++)
        ((uint64_t*)dev->tx_descs)[i] = 0;

    for (int i = 0; i < IGB_NUM_TX_DESC; i++) {
        uint64_t bp = mm_allocate_physical_page();
        if (!bp) {
            kprintf("IGB: Failed to allocate TX buffer %d\n", i);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(bp);
        dev->tx_bufs_phys[i] = bp;
    }

    igb_write(dev, IGB_TDBAL0, (uint32_t)(phys & 0xFFFFFFFF));
    igb_write(dev, IGB_TDBAH0, (uint32_t)(phys >> 32));
    igb_write(dev, IGB_TDLEN0, ring_bytes);
    igb_write(dev, IGB_TDH0, 0);
    igb_write(dev, IGB_TDT0, 0);

    igb_write(dev, IGB_TIPG, 0x0060200A); // matches 8254x default

    // Enable TX queue
    uint32_t txdctl = igb_read(dev, IGB_TXDCTL0);
    txdctl |= IGB_QUEUE_ENABLE;
    igb_write(dev, IGB_TXDCTL0, txdctl);
    for (int i = 0; i < 1000; i++) {
        if (igb_read(dev, IGB_TXDCTL0) & IGB_QUEUE_ENABLE) break;
        igb_delay_us(100);
    }

    igb_write(dev, IGB_TCTL,
              IGB_TCTL_EN | IGB_TCTL_PSP |
              (15u << IGB_TCTL_CT_SHIFT) |
              (64u << IGB_TCTL_COLD_SHIFT) |
              IGB_TCTL_RTLC);

    dev->tx_tail = 0;
    return 0;
}

// ============================================================================
// send / link_status / shutdown
// ============================================================================
static int igb_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    igb_dev_t* dev = (igb_dev_t*)ndev->driver_data;
    if (len > IGB_RX_BUF_SIZE) return -1;

    // Serialize across CPUs: tx_tail, descriptor slot, TDT0 MMIO.
    uint64_t txflags;
    spin_lock_irqsave(&dev->tx_lock, &txflags);

    uint16_t tail = dev->tx_tail;
    volatile igb_tx_desc_t* desc = &dev->tx_descs[tail];

    if (tail != 0 || desc->cmd != 0) {
        if (!(desc->status & IGB_TXD_STAT_DD) && desc->cmd != 0) {
            ndev->tx_errors++;
            spin_unlock_irqrestore(&dev->tx_lock, txflags);
            return -1;
        }
    }

    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[tail][i] = data[i];

    desc->buffer_addr = dev->tx_bufs_phys[tail];
    desc->length = len;
    desc->cso = 0;
    desc->cmd = IGB_TXD_CMD_EOP | IGB_TXD_CMD_IFCS | IGB_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    __asm__ volatile("mfence" ::: "memory");

    dev->tx_tail = (tail + 1) % IGB_NUM_TX_DESC;
    igb_write(dev, IGB_TDT0, dev->tx_tail);

    ndev->tx_packets++;
    ndev->tx_bytes += len;
    spin_unlock_irqrestore(&dev->tx_lock, txflags);
    return 0;
}

static int igb_link_status(net_device_t* ndev) {
    igb_dev_t* dev = (igb_dev_t*)ndev->driver_data;
    uint32_t status = igb_read(dev, IGB_STATUS);
    int now_up = (status & IGB_STATUS_LU) ? 1 : 0;
    int was_up = dev->link_up;
    if (now_up != was_up) {
        dev->link_up = now_up;
        kprintf("IGB: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) dhcp_invalidate(ndev);
    }
    return now_up;
}

// Quiesce the controller before ACPI S5: mask all interrupts (legacy +
// extended), halt the RX/TX queues, clear Wake-on-LAN, deassert
// CTRL_EXT.DRV_LOAD so the management firmware reclaims ownership, and
// finally drop PCI bus-master so any in-flight DMA is dropped by the
// root complex.
static void igb_shutdown(net_device_t* ndev) {
    igb_dev_t* dev = (igb_dev_t*)ndev->driver_data;
    if (!dev || !dev->mmio_base) return;

    igb_write(dev, IGB_IMC, 0xFFFFFFFFu);
    igb_write(dev, 0x152C /* EIMC */, 0xFFFFFFFFu);
    (void)igb_read(dev, IGB_ICR);

    uint32_t rxdctl = igb_read(dev, IGB_RXDCTL0);
    igb_write(dev, IGB_RXDCTL0, rxdctl & ~IGB_QUEUE_ENABLE);
    uint32_t txdctl = igb_read(dev, IGB_TXDCTL0);
    igb_write(dev, IGB_TXDCTL0, txdctl & ~IGB_QUEUE_ENABLE);

    uint32_t rctl = igb_read(dev, IGB_RCTL);
    igb_write(dev, IGB_RCTL, rctl & ~IGB_RCTL_EN);
    uint32_t tctl = igb_read(dev, IGB_TCTL);
    igb_write(dev, IGB_TCTL, tctl & ~IGB_TCTL_EN);

    igb_write(dev, IGB_WUFC, 0);
    igb_write(dev, IGB_WUC, 0);

    uint32_t ctrl_ext = igb_read(dev, IGB_CTRL_EXT);
    igb_write(dev, IGB_CTRL_EXT, ctrl_ext & ~IGB_CTRL_EXT_DRV_LOAD);

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
void igb_irq_handler(void) {
    if (!g_igb_initialized) {
        lapic_eoi();
        return;
    }

    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    igb_dev_t* dev = &g_igb;
    uint32_t icr = igb_read(dev, IGB_ICR);
    if (icr == 0) {
        lapic_eoi();
        return;
    }

    if (icr & IGB_ICR_LSC) {
        uint32_t status = igb_read(dev, IGB_STATUS);
        int now_up = (status & IGB_STATUS_LU) ? 1 : 0;
        int was_up = dev->link_up;
        dev->link_up = now_up;
        if (now_up != was_up) {
            kprintf("IGB: Link %s\n", now_up ? "UP" : "DOWN");
            if (!now_up) dhcp_invalidate(&dev->net_dev);
        }
    }

    if (icr & (IGB_ICR_RXT0 | IGB_ICR_RXDMT0 | IGB_ICR_RXO | IGB_ICR_RXQ0)) {
        uint16_t tail = (dev->rx_tail + 1) % IGB_NUM_RX_DESC;
        while (1) {
            volatile igb_rx_desc_t* desc = &dev->rx_descs[tail];
            uint32_t st = desc->wb.status_error;
            if (!(st & IGB_RXD_STAT_DD)) break;

            uint16_t len = desc->wb.length;
            if ((st & IGB_RXD_STAT_EOP) && len > 0 && len <= IGB_RX_BUF_SIZE) {
                net_rx_packet(&dev->net_dev, dev->rx_bufs[tail], len);
            } else {
                dev->net_dev.rx_errors++;
            }

            // Hand the descriptor back: rewrite the read-format fields,
            // which clears the DD/EOP status bits the chip wrote there.
            desc->read.pkt_addr = dev->rx_bufs_phys[tail];
            desc->read.hdr_addr = 0;

            dev->rx_tail = tail;
            igb_write(dev, IGB_RDT0, tail);

            tail = (tail + 1) % IGB_NUM_RX_DESC;
        }
    }

    lapic_eoi();
}

// ============================================================================
// MMIO BAR mapping
// ============================================================================
static int igb_map_bar(igb_dev_t* dev) {
    uint32_t bar0 = dev->pci_dev->bar[0];
    if (bar0 & 1) {
        kprintf("IGB: BAR0 is I/O port, not MMIO\n");
        return -1;
    }
    uint64_t phys = bar0 & 0xFFFFFFF0ULL;
    if ((bar0 & 0x06) == 0x04) {
        phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
    }

    dev->mmio_phys = phys;
    dev->mmio_size = 128 * 1024;  // 82576 BAR0 is 128 KiB

    uint64_t virt_base = (uint64_t)phys_to_virt(phys);
    uint32_t num_pages = (dev->mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = 0; i < num_pages; i++) {
        uint64_t pp = phys + i * PAGE_SIZE;
        uint64_t pv = virt_base + i * PAGE_SIZE;
        if (!mm_is_page_mapped(pv)) {
            mm_map_page(pv, pp,
                        PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
        }
    }
    dev->mmio_base = (volatile uint8_t*)virt_base;
    return 0;
}

// ============================================================================
// Driver entry point
// ============================================================================
void igb_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != IGB_VENDOR_ID) continue;
        const igb_id_t* match = igb_lookup(devs[i].device_id);
        if (!match) continue;

        kprintf("IGB: Found %s (PCI %02x:%02x.%x)\n",
                match->name, devs[i].bus, devs[i].device, devs[i].function);

        igb_dev_t* dev = &g_igb;
        dev->pci_dev = &devs[i];
        dev->device_id = devs[i].device_id;

        pci_enable_busmaster_mem(dev->pci_dev);

        if (igb_map_bar(dev) < 0) {
            kprintf("IGB: Failed to map BAR0\n");
            return;
        }

        igb_reset(dev);

        igb_read_mac(dev);
        kprintf("IGB: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        // Set link up; preserve UEFI speed/duplex
        uint32_t ctrl = igb_read(dev, IGB_CTRL);
        ctrl |= IGB_CTRL_SLU;
        ctrl &= ~IGB_CTRL_PHY_RST;
        igb_write(dev, IGB_CTRL, ctrl);

        // Tell the management firmware that a host driver is now active
        uint32_t ctrl_ext = igb_read(dev, IGB_CTRL_EXT);
        igb_write(dev, IGB_CTRL_EXT, ctrl_ext | IGB_CTRL_EXT_DRV_LOAD);

        // Clear multicast table (128 entries)
        for (int m = 0; m < 128; m++)
            igb_write(dev, IGB_MTA + m * 4, 0);

        // Re-program receive address valid bit
        uint32_t ral = (uint32_t)dev->mac_addr[0]
                     | ((uint32_t)dev->mac_addr[1] << 8)
                     | ((uint32_t)dev->mac_addr[2] << 16)
                     | ((uint32_t)dev->mac_addr[3] << 24);
        uint32_t rah = (uint32_t)dev->mac_addr[4]
                     | ((uint32_t)dev->mac_addr[5] << 8)
                     | (1u << 31);
        igb_write(dev, IGB_RAL0, ral);
        igb_write(dev, IGB_RAH0, rah);

        if (igb_init_rx(dev) < 0) return;
        if (igb_init_tx(dev) < 0) return;

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
                kprintf("IGB: ACPI _PRT resolved INT%c -> GSI %u\n",
                        'A' + acpi_pin, gsi);
            }
        }
        if (irq == 0xFF) {
            irq = dev->pci_dev->interrupt_line;
            if (irq != 0xFF && irq <= 23) {
                kprintf("IGB: ACPI _PRT lookup failed, falling back to "
                        "PCI interrupt_line = %d\n", irq);
            } else {
                kprintf("IGB: WARNING: no valid IRQ\n");
            }
        }
        kprintf("IGB: Using legacy IRQ %d\n", irq);
        g_igb_legacy_irq = irq;

        // Clear PCI Command INTx Disable if firmware left it set
        {
            uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                          dev->pci_dev->device,
                                          dev->pci_dev->function, 0x04);
            if (cmd & PCI_CMD_INTX_DISABLE) {
                cmd &= ~PCI_CMD_INTX_DISABLE;
                pci_cfg_write32(dev->pci_dev->bus,
                                dev->pci_dev->device,
                                dev->pci_dev->function, 0x04, cmd);
                kprintf("IGB: cleared PCI Command INTx Disable bit\n");
            }
        }

        if (irq <= 23) {
            uint8_t vector = 32 + irq;
            ioapic_configure_legacy_irq(irq, vector,
                                        IOAPIC_POLARITY_LOW,
                                        IOAPIC_TRIGGER_LEVEL);
        }

        // Initialise net_device fields BEFORE enabling chip-side IRQs
        dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("igb");
        dev->tx_lock = (spinlock_t)SPINLOCK_INIT("igb_tx");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;
        g_igb_initialized = 1;

        igb_write(dev, IGB_IMS,
                  IGB_ICR_RXT0 | IGB_ICR_TXDW | IGB_ICR_LSC |
                  IGB_ICR_RXDMT0 | IGB_ICR_RXO);

        // Wait up to 5 s for the link to come up
        uint32_t status = igb_read(dev, IGB_STATUS);
        if (!(status & IGB_STATUS_LU)) {
            uint32_t t0 = timer_pmtimer_read_raw();
            while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < 5000000) {
                igb_delay_us(50000);
                status = igb_read(dev, IGB_STATUS);
                if (status & IGB_STATUS_LU) break;
            }
        }
        dev->link_up = (status & IGB_STATUS_LU) ? 1 : 0;
        kprintf("IGB: Link %s\n", dev->link_up ? "UP" : "DOWN");

        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = igb_send;
        dev->net_dev.link_status = igb_link_status;
        dev->net_dev.shutdown = igb_shutdown;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);

        kprintf("IGB: Driver initialized successfully\n");
        return;
    }
}
