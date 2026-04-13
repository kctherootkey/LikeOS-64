// LikeOS-64 Intel E1000 NIC Driver
// Supports QEMU (82540EM), VMware (82545EM), VirtualBox (82574L)
#include "../../include/kernel/e1000.h"
#include "../../include/kernel/net.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/lapic.h"

// Global e1000 device (single NIC support)
static e1000_dev_t g_e1000;
static int g_e1000_initialized = 0;

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
// Device Reset
// ============================================================================
static void e1000_reset(e1000_dev_t* dev) {
    // Disable interrupts
    e1000_write(dev, E1000_IMC, 0xFFFFFFFF);

    // Reset device
    uint32_t ctrl = e1000_read(dev, E1000_CTRL);
    ctrl |= E1000_CTRL_RST;
    e1000_write(dev, E1000_CTRL, ctrl);

    // Wait for reset to complete
    for (volatile int i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }

    // Disable interrupts again after reset
    e1000_write(dev, E1000_IMC, 0xFFFFFFFF);

    // Read and clear pending interrupts
    (void)e1000_read(dev, E1000_ICR);
}

// ============================================================================
// Initialize RX Ring with Extended Descriptors
// ============================================================================
static int e1000_init_rx(e1000_dev_t* dev) {
    // Allocate descriptor ring
    uint32_t rx_ring_size = sizeof(e1000_rx_desc_ext_t) * E1000_NUM_RX_DESC;
    uint32_t rx_ring_pages = (rx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t rx_phys = mm_allocate_contiguous_pages(rx_ring_pages);
    if (!rx_phys) {
        kprintf("E1000: Failed to allocate RX descriptor ring\n");
        return -1;
    }

    dev->rx_descs = (e1000_rx_desc_ext_t*)phys_to_virt(rx_phys);
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

        // Set up extended RX descriptor (read format)
        dev->rx_descs[i].read.buffer_addr = buf_phys;
        dev->rx_descs[i].read.reserved = 0;
    }

    // Program RX ring registers
    e1000_write(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000_write(dev, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000_write(dev, E1000_RDLEN, rx_ring_size);
    e1000_write(dev, E1000_RDH, 0);
    e1000_write(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);

    // Enable extended RX descriptors if supported (82574L)
    if (dev->device_id == E1000_DEV_82574L) {
        uint32_t rfctl = e1000_read(dev, E1000_RFCTL);
        rfctl |= (1 << 15);  // EXSTEN - Extended Status Enable
        e1000_write(dev, E1000_RFCTL, rfctl);
    }

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
    uint32_t tx_ring_size = sizeof(e1000_tx_desc_ext_t) * E1000_NUM_TX_DESC;
    uint32_t tx_ring_pages = (tx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t tx_phys = mm_allocate_contiguous_pages(tx_ring_pages);
    if (!tx_phys) {
        kprintf("E1000: Failed to allocate TX descriptor ring\n");
        return -1;
    }

    dev->tx_descs = (e1000_tx_desc_ext_t*)phys_to_virt(tx_phys);
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

    uint16_t tail = dev->tx_tail;

    // Check if descriptor is available (DD bit set means hardware is done with it)
    volatile e1000_tx_desc_ext_data_t* desc = &dev->tx_descs[tail].data;
    if (tail != 0 || dev->tx_descs[tail].data.olinfo_status != 0) {
        // For non-first descriptor, check DD bit
        if (!(desc->olinfo_status & E1000_TXD_EXT_STAT_DD) &&
            desc->cmd_type_len != 0) {
            // Descriptor still in use
            ndev->tx_errors++;
            return -1;
        }
    }

    // Copy data to TX buffer
    for (uint16_t i = 0; i < len; i++)
        dev->tx_bufs[tail][i] = data[i];

    // Set up extended TX descriptor (data format)
    desc->buffer_addr = dev->tx_bufs_phys[tail];
    desc->cmd_type_len = (uint32_t)len
                       | E1000_TXD_EXT_CMD_EOP
                       | E1000_TXD_EXT_CMD_IFCS
                       | E1000_TXD_EXT_CMD_RS
                       | E1000_TXD_EXT_CMD_DEXT
                       | E1000_TXD_EXT_DTYP_DATA;
    desc->olinfo_status = ((uint32_t)len << E1000_TXD_EXT_PAYLEN_SHIFT);

    // Memory barrier before advancing tail
    __asm__ volatile("mfence" ::: "memory");

    // Advance tail
    dev->tx_tail = (tail + 1) % E1000_NUM_TX_DESC;
    e1000_write(dev, E1000_TDT, dev->tx_tail);

    ndev->tx_packets++;
    ndev->tx_bytes += len;
    return 0;
}

// ============================================================================
// E1000 Link Status (net_device_t callback)
// ============================================================================
static int e1000_link_status(net_device_t* ndev) {
    e1000_dev_t* dev = (e1000_dev_t*)ndev->driver_data;
    uint32_t status = e1000_read(dev, E1000_STATUS);
    return (status & E1000_STATUS_LU) ? 1 : 0;
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

    if (icr & E1000_ICR_LSC) {
        // Link status change
        uint32_t status = e1000_read(dev, E1000_STATUS);
        dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
        kprintf("E1000: Link %s\n", dev->link_up ? "UP" : "DOWN");
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
        // Process received packets
        uint16_t tail = (dev->rx_tail + 1) % E1000_NUM_RX_DESC;

        while (1) {
            volatile e1000_rx_desc_ext_wb_t* desc = &dev->rx_descs[tail].wb;
            if (!(desc->status_error & E1000_RXD_EXT_STAT_DD))
                break;

            uint16_t len = desc->length;
            if ((desc->status_error & E1000_RXD_EXT_STAT_EOP) && len > 0 && len <= E1000_RX_BUF_SIZE) {
                net_rx_packet(&dev->net_dev, dev->rx_bufs[tail], len);
            } else {
                dev->net_dev.rx_errors++;
            }

            // Reset descriptor for reuse (read format)
            dev->rx_descs[tail].read.buffer_addr = dev->rx_bufs_phys[tail];
            dev->rx_descs[tail].read.reserved = 0;

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
        if (did != E1000_DEV_82540EM && did != E1000_DEV_82545EM &&
            did != E1000_DEV_82574L && did != E1000_DEV_I217_LM)
            continue;

        const char* name = "E1000";
        if (did == E1000_DEV_82540EM) name = "82540EM";
        else if (did == E1000_DEV_82545EM) name = "82545EM";
        else if (did == E1000_DEV_82574L) name = "82574L";
        else if (did == E1000_DEV_I217_LM) name = "I217-LM";

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

        // Set link up
        uint32_t ctrl = e1000_read(dev, E1000_CTRL);
        ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
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
            kprintf("E1000: MSI not available, using legacy IRQ %d\n",
                    dev->pci_dev->interrupt_line);
            dev->msi_vector = 0;
        } else {
            kprintf("E1000: MSI enabled (vector %d)\n", dev->msi_vector);
        }

        // Enable interrupts: RXT0, TXDW, LSC
        e1000_write(dev, E1000_IMS,
                    E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_LSC |
                    E1000_ICR_RXDMT0 | E1000_ICR_RXO);

        // Check link status
        uint32_t status = e1000_read(dev, E1000_STATUS);
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
        dev->net_dev.driver_data = dev;
        dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("e1000");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;

        g_e1000_initialized = 1;

        // Register with network subsystem
        net_register(&dev->net_dev);

        kprintf("E1000: Driver initialized successfully\n");
        return;  // Only handle first device
    }
}
