// LikeOS-64 Intel E1000 NIC Driver
// Supports the e1000-class parts:
//   - 82540EM  (QEMU `-device e1000`, VirtualBox "Intel PRO/1000 MT Desktop")
//   - 82545EM  (VMware default, VirtualBox "Intel PRO/1000 MT Server" and
//               "Intel PRO/1000 T Server")
// Note: The 82574L (PCI 0x10D3) is an e1000e-class part and is handled
// by the separate e1000e driver in this directory, not here.
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

// ============================================================================
// Supported PCI device IDs for the entire 8254x PCI/PCI-X family
// (82540, 82541, 82543, 82544, 82545, 82546, 82547).  8257x PCIe parts
// are handled by the separate e1000e driver.
// ============================================================================
typedef struct { uint16_t did; const char* name; } e1000_id_t;

static const e1000_id_t e1000_pci_ids[] = {
    // 82540
    { 0x100E, "82540EM"          },   // QEMU -device e1000, VBox MT Desktop
    { 0x1015, "82540EM LOM"      },
    { 0x1016, "82540EP LOM"      },
    { 0x1017, "82540EP"          },
    { 0x101E, "82540EP LP"       },
    // 82541
    { 0x1013, "82541EI"          },
    { 0x1018, "82541EI Mobile"   },
    { 0x1076, "82541GI"          },
    { 0x1077, "82541GI Mobile"   },
    { 0x1078, "82541ER"          },
    { 0x1079, "82546GB Quad Copper" },
    { 0x107C, "82541PI"          },
    // 82543
    { 0x1004, "82543GC Copper"   },   // QEMU -device e1000-82543gc
    { 0x1003, "82543GC Fiber"    },
    // 82544
    { 0x1008, "82544EI Copper"   },
    { 0x1009, "82544EI Fiber"    },
    { 0x100C, "82544GC Copper"   },   // QEMU -device e1000-82544gc
    { 0x100D, "82544GC LOM"      },
    // 82545
    { 0x100F, "82545EM Copper"   },   // VMware, VBox MT Server
    { 0x1011, "82545EM Fiber"    },
    { 0x1026, "82545GM Copper"   },
    { 0x1027, "82545GM Fiber"    },
    { 0x1028, "82545GM SerDes"   },
    // 82546
    { 0x1010, "82546EB Copper"   },
    { 0x1012, "82546EB Fiber"    },
    { 0x101D, "82546EB Quad Copper" },
    { 0x1079, "82546GB Quad Copper (2)" },
    { 0x107A, "82546GB Fiber"    },
    { 0x107B, "82546GB SerDes"   },
    { 0x108A, "82546GB Copper"   },
    { 0x1099, "82546GB Quad Copper KSP3" },
    { 0x10B5, "82546GB Quad Copper" },
    // 82547
    { 0x1019, "82547EI"          },
    { 0x101A, "82547EI Mobile"   },
    { 0x1075, "82547GI"          },
    { 0,      NULL               },
};

static const e1000_id_t* e1000_lookup(uint16_t did) {
    for (const e1000_id_t* e = e1000_pci_ids; e->name; e++) {
        if (e->did == did) return e;
    }
    return NULL;
}

// Global e1000 device (single NIC support)
static e1000_dev_t g_e1000;
int g_e1000_initialized = 0;
int g_e1000_legacy_irq = -1;  // Legacy IRQ number (-1 = not using legacy)

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t e1000_read(e1000_dev_t* dev, uint32_t reg) {
    return *(volatile uint32_t*)(dev->mmio_base + reg);
}

static inline void e1000_write(e1000_dev_t* dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(dev->mmio_base + reg) = val;
    __asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// EEPROM Read
// ============================================================================
static uint16_t e1000_read_eeprom(e1000_dev_t* dev, uint8_t addr) {
    e1000_write(dev, E1000_EERD, (uint32_t)addr << E1000_EERD_ADDR_SHIFT | E1000_EERD_START);
    uint32_t val;
    for (int i = 0; i < 10000; i++) {
        val = e1000_read(dev, E1000_EERD);
        if (val & E1000_EERD_DONE) {
            return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
        }
    }
    return 0;
}

// ============================================================================
// Read MAC Address
// ============================================================================
static void e1000_read_mac(e1000_dev_t* dev) {
    // Try EEPROM first
    uint16_t w0 = e1000_read_eeprom(dev, 0);
    uint16_t w1 = e1000_read_eeprom(dev, 1);
    uint16_t w2 = e1000_read_eeprom(dev, 2);

    if (w0 != 0 || w1 != 0 || w2 != 0) {
        dev->mac_addr[0] = w0 & 0xFF;
        dev->mac_addr[1] = (w0 >> 8) & 0xFF;
        dev->mac_addr[2] = w1 & 0xFF;
        dev->mac_addr[3] = (w1 >> 8) & 0xFF;
        dev->mac_addr[4] = w2 & 0xFF;
        dev->mac_addr[5] = (w2 >> 8) & 0xFF;
    } else {
        // Fallback: read from RAL/RAH
        uint32_t ral = e1000_read(dev, E1000_RAL);
        uint32_t rah = e1000_read(dev, E1000_RAH);
        dev->mac_addr[0] = ral & 0xFF;
        dev->mac_addr[1] = (ral >> 8) & 0xFF;
        dev->mac_addr[2] = (ral >> 16) & 0xFF;
        dev->mac_addr[3] = (ral >> 24) & 0xFF;
        dev->mac_addr[4] = rah & 0xFF;
        dev->mac_addr[5] = (rah >> 8) & 0xFF;
    }
}

// ============================================================================
// PM Timer-based microsecond delay (available early — timer_init_pmtimer()
// runs before net_init()).  Falls back to a pause loop if PM Timer is absent.
// ============================================================================
static void e1000_delay_us(uint32_t us) {
    uint32_t t0 = timer_pmtimer_read_raw();
    if (t0 == 0) {
        // PM Timer unavailable — rough fallback
        for (volatile uint32_t i = 0; i < us * 4; i++)
            __asm__ volatile("pause");
        return;
    }
    while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < us)
        __asm__ volatile("pause");
}

// ============================================================================
// Device Reset
// ============================================================================
static void e1000_reset(e1000_dev_t* dev) {
    // Disable interrupts
    e1000_write(dev, E1000_IMC, 0xFFFFFFFF);

    // Read and clear pending interrupts
    (void)e1000_read(dev, E1000_ICR);

    // Soft reset: disable RX and TX without a full CTRL.RST.
    // CTRL.RST causes VirtualBox's E1000 emulation to break the RX
    // path — TX works but no packets are ever received.  The UEFI
    // firmware already performed the hardware reset and PHY
    // negotiation, so a soft init is sufficient.
    e1000_write(dev, E1000_RCTL, 0);  // Disable receiver
    e1000_write(dev, E1000_TCTL, 0);  // Disable transmitter

    // Wait for in-flight DMA to drain (10 ms via PM Timer)
    e1000_delay_us(10000);

    // Disable interrupts again (in case RX/TX raised something)
    e1000_write(dev, E1000_IMC, 0xFFFFFFFF);
    (void)e1000_read(dev, E1000_ICR);
}

// ============================================================================
// Initialize RX Ring with Extended Descriptors
// ============================================================================
static int e1000_init_rx(e1000_dev_t* dev) {
    // Allocate descriptor ring (legacy RX descriptors)
    uint32_t rx_ring_size = sizeof(e1000_rx_desc_legacy_t) * E1000_NUM_RX_DESC;
    uint32_t rx_ring_pages = (rx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t rx_phys = mm_allocate_contiguous_pages(rx_ring_pages);
    if (!rx_phys) {
        kprintf("E1000: Failed to allocate RX descriptor ring\n");
        return -1;
    }

    dev->rx_descs = (e1000_rx_desc_legacy_t*)phys_to_virt(rx_phys);
    dev->rx_descs_phys = rx_phys;

    // Zero descriptors
    for (uint32_t i = 0; i < rx_ring_size / 8; i++)
        ((uint64_t*)dev->rx_descs)[i] = 0;

    // Allocate RX packet buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t buf_phys = mm_allocate_physical_page();
        if (!buf_phys) {
            kprintf("E1000: Failed to allocate RX buffer %d\n", i);
            return -1;
        }
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(buf_phys);
        dev->rx_bufs_phys[i] = buf_phys;

        // Set up legacy RX descriptor
        dev->rx_descs[i].buffer_addr = buf_phys;
        dev->rx_descs[i].status = 0;
    }

    // Program RX ring registers
    e1000_write(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000_write(dev, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000_write(dev, E1000_RDLEN, rx_ring_size);
    e1000_write(dev, E1000_RDH, 0);
    e1000_write(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);

    // Configure receive control
    uint32_t rctl = E1000_RCTL_EN       // Enable receiver
                  | E1000_RCTL_BAM      // Accept broadcast
                  | E1000_RCTL_BSIZE_2048 // 2048 byte buffers
                  | E1000_RCTL_SECRC;   // Strip CRC

    e1000_write(dev, E1000_RCTL, rctl);

    dev->rx_tail = E1000_NUM_RX_DESC - 1;
    return 0;
}

// ============================================================================
// Initialize TX Ring with Extended Descriptors
// ============================================================================
static int e1000_init_tx(e1000_dev_t* dev) {
    uint32_t tx_ring_size = sizeof(e1000_tx_desc_legacy_t) * E1000_NUM_TX_DESC;
    uint32_t tx_ring_pages = (tx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t tx_phys = mm_allocate_contiguous_pages(tx_ring_pages);
    if (!tx_phys) {
        kprintf("E1000: Failed to allocate TX descriptor ring\n");
        return -1;
    }

    dev->tx_descs = (e1000_tx_desc_legacy_t*)phys_to_virt(tx_phys);
    dev->tx_descs_phys = tx_phys;

    // Zero descriptors
    for (uint32_t i = 0; i < tx_ring_size / 8; i++)
        ((uint64_t*)dev->tx_descs)[i] = 0;

    // Allocate TX packet buffers
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t buf_phys = mm_allocate_physical_page();
        if (!buf_phys) {
            kprintf("E1000: Failed to allocate TX buffer %d\n", i);
            return -1;
        }
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(buf_phys);
        dev->tx_bufs_phys[i] = buf_phys;
    }

    // Program TX ring registers
    e1000_write(dev, E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000_write(dev, E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000_write(dev, E1000_TDLEN, tx_ring_size);
    e1000_write(dev, E1000_TDH, 0);
    e1000_write(dev, E1000_TDT, 0);

    // Configure transmit inter-packet gap
    e1000_write(dev, E1000_TIPG, E1000_TIPG_IPGT | E1000_TIPG_IPGR1 | E1000_TIPG_IPGR2);

    // Configure transmit control
    uint32_t tctl = E1000_TCTL_EN       // Enable transmitter
                  | E1000_TCTL_PSP      // Pad short packets
                  | (15 << E1000_TCTL_CT_SHIFT)   // Collision threshold
                  | (64 << E1000_TCTL_COLD_SHIFT)  // Collision distance (full duplex)
                  | E1000_TCTL_RTLC;    // Retransmit on late collision

    e1000_write(dev, E1000_TCTL, tctl);

    dev->tx_tail = 0;
    return 0;
}

// ============================================================================
// E1000 Send Packet (net_device_t callback)
// ============================================================================
static int e1000_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    e1000_dev_t* dev = (e1000_dev_t*)ndev->driver_data;

    if (len > E1000_RX_BUF_SIZE) return -1;

    // Serialize across CPUs: tx_tail, descriptor slot, and TDT MMIO write
    // form one critical section; any two CPUs racing here would otherwise
    // (a) read the same tx_tail, (b) write the same descriptor slot, and
    // (c) lose one packet to the duplicate-tail-bump.  Hold IRQs disabled
    // for the few MMIO cycles so an IRQ-tail TX (e.g. softirq sending a
    // TCP ACK) cannot preempt user-context TX from the same CPU mid-slot.
    uint64_t txflags;
    spin_lock_irqsave(&dev->tx_lock, &txflags);

    uint16_t tail = dev->tx_tail;

    // Check if descriptor is available (DD bit set means hardware is done with it)
    volatile e1000_tx_desc_legacy_t* desc = &dev->tx_descs[tail];
    if (tail != 0 || desc->cmd != 0) {
        if (!(desc->status & E1000_TXD_STAT_DD) && desc->cmd != 0) {
            ndev->tx_errors++;
            spin_unlock_irqrestore(&dev->tx_lock, txflags);
            return -1;
        }
    }

    // Copy data to TX buffer
    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[tail][i] = data[i];

    // Set up legacy TX descriptor
    desc->buffer_addr = dev->tx_bufs_phys[tail];
    desc->length = len;
    desc->cso = 0;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    // Memory barrier before advancing tail
    __asm__ volatile("mfence" ::: "memory");

    // Advance tail
    dev->tx_tail = (tail + 1) % E1000_NUM_TX_DESC;
    e1000_write(dev, E1000_TDT, dev->tx_tail);

    ndev->tx_packets++;
    ndev->tx_bytes += len;
    spin_unlock_irqrestore(&dev->tx_lock, txflags);
    return 0;
}

// ============================================================================
// E1000 Link Status (net_device_t callback)
// ============================================================================
// Polled by userland (ifconfig etc.).  Also acts as a hot-plug fallback
// when the LSC IRQ has not yet fired: detects the edge against the
// cached dev->link_up, prints UP/DOWN, and on a DOWN edge invalidates
// the DHCP lease so the next dhclient invocation does a full DISCOVER
// against the new network instead of a (stale) unicast renewal.
static int e1000_link_status(net_device_t* ndev) {
    e1000_dev_t* dev = (e1000_dev_t*)ndev->driver_data;
    uint32_t status = e1000_read(dev, E1000_STATUS);
    int now_up = (status & E1000_STATUS_LU) ? 1 : 0;
    int was_up = dev->link_up;
    if (now_up != was_up) {
        dev->link_up = now_up;
        kprintf("E1000: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) dhcp_invalidate(ndev);
    }
    return now_up;
}

// Quiesce the controller ahead of an ACPI S5 transition.  Per the
// 8254x family datasheet, leaving IMS / RCTL.EN / TCTL.EN / WUC.APME
// asserted at sleep time can cause the chipset to keep PME# alive and
// hold the platform in a half-powered state.  Mask all interrupts,
// halt RX/TX, clear Wake-on-LAN, and drop PCI bus-master so any
// in-flight DMA is dropped by the root complex.
static void e1000_shutdown(net_device_t* ndev) {
    e1000_dev_t* dev = (e1000_dev_t*)ndev->driver_data;
    if (!dev || !dev->mmio_base) return;

    e1000_write(dev, E1000_IMC, 0xFFFFFFFFu);
    (void)e1000_read(dev, E1000_ICR);

    uint32_t rctl = e1000_read(dev, E1000_RCTL);
    e1000_write(dev, E1000_RCTL, rctl & ~E1000_RCTL_EN);
    uint32_t tctl = e1000_read(dev, E1000_TCTL);
    e1000_write(dev, E1000_TCTL, tctl & ~E1000_TCTL_EN);

    // WUC (Wake Up Control) at 0x05800, WUFC (Wake Up Filter Control) at
    // 0x05808 — clearing both deasserts PME# capability.
    e1000_write(dev, 0x05808u /* WUFC */, 0);
    e1000_write(dev, 0x05800u /* WUC  */, 0);

    if (dev->pci_dev) {
        const pci_device_t* pdev = dev->pci_dev;
        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0004u; // clear Bus Master Enable
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// E1000 IRQ Handler
// ============================================================================
void e1000_irq_handler(void) {
    if (!g_e1000_initialized) {
        lapic_eoi();
        return;
    }

    // Feed entropy from NIC interrupt timing
    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    e1000_dev_t* dev = &g_e1000;
    uint32_t icr = e1000_read(dev, E1000_ICR);

    if (icr == 0) {
        // Spurious - no cause bits set
        lapic_eoi();
        return;
    }

    if (icr & E1000_ICR_LSC) {
        // Link status change
        uint32_t status = e1000_read(dev, E1000_STATUS);
        int now_up = (status & E1000_STATUS_LU) ? 1 : 0;
        int was_up = dev->link_up;
        dev->link_up = now_up;
        if (now_up != was_up) {
            kprintf("E1000: Link %s\n", now_up ? "UP" : "DOWN");
            if (!now_up) {
                // Link dropped: invalidate the DHCP lease so the next
                // dhclient invocation does a full DISCOVER instead of
                // unicast-renewing against the previous network's
                // server (which is unreachable when bridged<->NAT or
                // the cable moved to a different switch).
                dhcp_invalidate(&dev->net_dev);
            }
        }
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
        // Process received packets (legacy descriptors)
        uint16_t tail = (dev->rx_tail + 1) % E1000_NUM_RX_DESC;

        while (1) {
            volatile e1000_rx_desc_legacy_t* desc = &dev->rx_descs[tail];
            if (!(desc->status & E1000_RXD_STAT_DD))
                break;

            uint16_t len = desc->length;
            if ((desc->status & E1000_RXD_STAT_EOP) && len > 0 && len <= E1000_RX_BUF_SIZE) {
                net_rx_packet(&dev->net_dev, dev->rx_bufs[tail], len);
            } else {
                dev->net_dev.rx_errors++;
            }

            // Reset descriptor for reuse
            desc->buffer_addr = dev->rx_bufs_phys[tail];
            desc->status = 0;

            dev->rx_tail = tail;
            e1000_write(dev, E1000_RDT, tail);

            tail = (tail + 1) % E1000_NUM_RX_DESC;
        }
    }

    if (icr & E1000_ICR_TXDW) {
        // TX descriptor written back - descriptors reclaimed via DD bit check in send
    }

    lapic_eoi();
}

// ============================================================================
// E1000 Map MMIO BAR
// ============================================================================
static int e1000_map_bar(e1000_dev_t* dev) {
    uint32_t bar0 = dev->pci_dev->bar[0];

    if (bar0 & 1) {
        kprintf("E1000: BAR0 is I/O port, not MMIO\n");
        return -1;
    }

    // Get physical address (mask off lower bits)
    uint64_t phys = bar0 & 0xFFFFFFF0ULL;

    // Check if 64-bit BAR
    if ((bar0 & 0x06) == 0x04) {
        phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
    }

    dev->mmio_phys = phys;
    dev->mmio_size = 128 * 1024;  // E1000 MMIO is typically 128KB

    // Map MMIO pages
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
// E1000 Initialization
// ============================================================================
void e1000_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != E1000_VENDOR_ID) continue;

        uint16_t did = devs[i].device_id;
        const e1000_id_t* match = e1000_lookup(did);
        if (!match) continue;
        const char* name = match->name;

        kprintf("E1000: Found %s (PCI %02x:%02x.%x)\n",
                name, devs[i].bus, devs[i].device, devs[i].function);

        e1000_dev_t* dev = &g_e1000;
        dev->pci_dev = &devs[i];
        dev->device_id = did;

        // Enable bus mastering and memory space
        pci_enable_busmaster_mem(dev->pci_dev);

        // Map MMIO
        if (e1000_map_bar(dev) < 0) {
            kprintf("E1000: Failed to map BAR0\n");
            return;
        }

        // Reset device
        e1000_reset(dev);

        // Read MAC address
        e1000_read_mac(dev);
        kprintf("E1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        // Set link up — only add SLU; preserve UEFI speed/duplex settings
        // to avoid triggering a PHY re-negotiation that drops the link.
        uint32_t ctrl = e1000_read(dev, E1000_CTRL);
        ctrl |= E1000_CTRL_SLU;
        ctrl &= ~E1000_CTRL_PHY_RST;
        e1000_write(dev, E1000_CTRL, ctrl);

        // Clear multicast table
        for (int m = 0; m < 128; m++) {
            e1000_write(dev, E1000_MTA + m * 4, 0);
        }

        // Set MAC in receive address register
        uint32_t ral = (uint32_t)dev->mac_addr[0]
                     | ((uint32_t)dev->mac_addr[1] << 8)
                     | ((uint32_t)dev->mac_addr[2] << 16)
                     | ((uint32_t)dev->mac_addr[3] << 24);
        uint32_t rah = (uint32_t)dev->mac_addr[4]
                     | ((uint32_t)dev->mac_addr[5] << 8)
                     | (1 << 31);  // Address Valid
        e1000_write(dev, E1000_RAL, ral);
        e1000_write(dev, E1000_RAH, rah);

        // Initialize RX and TX rings
        if (e1000_init_rx(dev) < 0) return;
        if (e1000_init_tx(dev) < 0) return;

        // Enable MSI interrupt
        dev->msi_vector = E1000_MSI_VECTOR;
        int msi_ret = pci_enable_msi(dev->pci_dev, dev->msi_vector);
        if (msi_ret < 0) {
            dev->msi_vector = 0;

            // ALWAYS resolve the IOAPIC GSI from ACPI _PRT first when the
            // IOAPIC is in use.  PCI config-space `interrupt_line` reflects
            // the legacy 8259 PIC IRQ assigned by the BIOS — it is NOT the
            // IOAPIC GSI in APIC mode.  On QEMU/VMware the e1000 happens
            // to land on GSI 11 so the legacy line value matches by
            // coincidence; on VirtualBox with the ICH9 chipset INTA of
            // device 3 is routed via PIRQ to a GSI in the 16-23 range,
            // and trusting `interrupt_line` would program the wrong
            // IOAPIC redirection entry — exactly the symptom we observed
            // (TX works, no RX interrupt ever delivered).
            uint8_t irq = 0xFF;
            uint32_t gsi = 0;
            uint8_t pin = dev->pci_dev->interrupt_pin;
            if (pin >= 1 && pin <= 4) {
                uint8_t acpi_pin = pin - 1;  // PCI pin 1-4 -> ACPI pin 0-3

                // For devices behind PCI-to-PCI bridges, we must:
                // 1. Find the bridge on bus 0 that owns this secondary bus
                // 2. Apply PCI interrupt pin swizzling
                // 3. Look up the bridge device (not the NIC) in the root _PRT
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
                    kprintf("E1000: ACPI _PRT resolved INT%c -> GSI %u\n",
                            'A' + acpi_pin, gsi);
                }
            }

            // Fallback: trust PCI config-space interrupt_line (only valid
            // on systems where the BIOS happens to program the same value
            // the IOAPIC uses, e.g. QEMU/VMware/most legacy setups).
            if (irq == 0xFF) {
                irq = dev->pci_dev->interrupt_line;
                if (irq != 0xFF && irq <= 23) {
                    kprintf("E1000: ACPI _PRT lookup failed, falling back to "
                            "PCI interrupt_line = %d\n", irq);
                } else {
                    kprintf("E1000: WARNING: no valid IRQ (line=%d, _PRT failed)\n",
                            dev->pci_dev->interrupt_line);
                    kprintf("E1000: Interrupts will not work\n");
                }
            }

            kprintf("E1000: Using legacy IRQ %d\n", irq);
            g_e1000_legacy_irq = irq;

            // VirtualBox's UEFI firmware leaves PCI Command bit 10 (INTx
            // Disable) SET on the emulated 82540EM, which prevents the
            // device from ever asserting its interrupt pin.  TX still
            // works (it doesn't need the IRQ) but no RX/LSC/etc. interrupt
            // is ever delivered to the IOAPIC.  Explicitly clear bit 10
            // for the legacy IRQ path so the wire actually toggles.
            // (QEMU and VMware leave the bit clear, so this is a no-op
            // there.)
            {
                uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                              dev->pci_dev->device,
                                              dev->pci_dev->function, 0x04);
                if (cmd & PCI_CMD_INTX_DISABLE) {
                    cmd &= ~PCI_CMD_INTX_DISABLE;
                    pci_cfg_write32(dev->pci_dev->bus,
                                    dev->pci_dev->device,
                                    dev->pci_dev->function, 0x04, cmd);
                    kprintf("E1000: cleared PCI Command INTx Disable bit\n");
                }
            }

            if (irq <= 23) {
                uint8_t vector = 32 + irq;
                ioapic_configure_legacy_irq(irq, vector, IOAPIC_POLARITY_LOW, IOAPIC_TRIGGER_LEVEL);
            }
        } else {
            kprintf("E1000: MSI enabled (vector %d)\n", dev->msi_vector);
        }

        // Mark the driver as initialised BEFORE enabling device interrupts.
        // The IRQ dispatcher (kernel/ke/interrupt.c) only routes the legacy
        // IRQ to e1000_irq_handler() when g_e1000_initialized != 0; if the
        // device fires an interrupt (LSC, etc.) before this flag is set the
        // dispatcher falls through to other handlers (e.g. the I2C LPSS
        // range covers vectors 50-53, which is exactly where GSI 19 lands
        // on VirtualBox ICH9), causing them to access non-existent
        // hardware and lock up the system in a level-triggered IRQ storm.
        // We populate the minimum state e1000_irq_handler() touches.
        dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("e1000");
        dev->tx_lock = (spinlock_t)SPINLOCK_INIT("e1000_tx");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;
        g_e1000_initialized = 1;

        // Enable interrupts: RXT0, TXDW, LSC
        e1000_write(dev, E1000_IMS,
                    E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_LSC |
                    E1000_ICR_RXDMT0 | E1000_ICR_RXO);

        // Check link status.  After a CTRL.RST, VirtualBox's E1000
        // emulation can take several seconds to bring the link back up.
        // Use PM Timer for accurate timing (up to 5 seconds).
        uint32_t status = e1000_read(dev, E1000_STATUS);
        if (!(status & E1000_STATUS_LU)) {
            uint32_t t0 = timer_pmtimer_read_raw();
            while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < 5000000) {
                e1000_delay_us(50000);  // check every 50 ms
                status = e1000_read(dev, E1000_STATUS);
                if (status & E1000_STATUS_LU) break;
            }
        }
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        kprintf("E1000: Link %s\n", dev->link_up ? "UP" : "DOWN");

        // Set up net_device_t
        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = e1000_send;
        dev->net_dev.link_status = e1000_link_status;
        dev->net_dev.shutdown = e1000_shutdown;
        dev->net_dev.driver_data = dev;
        // Note: lock and statistics counters are initialised earlier (before
        // we enable device interrupts) so the IRQ handler always sees a
        // valid net_device_t.

        // Register with network subsystem
        net_register(&dev->net_dev);

        kprintf("E1000: Driver initialized successfully\n");
        return;  // Only handle first device
    }
}
