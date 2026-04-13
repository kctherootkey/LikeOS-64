// LikeOS-64 Intel E1000 NIC Driver
// Supports: 82540EM (QEMU), 82545EM (VMware), 82574L (VirtualBox)

#ifndef _KERNEL_E1000_H_
#define _KERNEL_E1000_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI Device IDs
// ============================================================================
#define E1000_VENDOR_ID         0x8086
#define E1000_DEV_82540EM       0x100E  // QEMU default
#define E1000_DEV_82545EM       0x100F  // VMware
#define E1000_DEV_82574L        0x10D3  // VirtualBox
#define E1000_DEV_I217_LM       0x153A  // Optional

// ============================================================================
// E1000 MMIO Register Offsets
// ============================================================================
#define E1000_CTRL          0x0000  // Device Control
#define E1000_STATUS        0x0008  // Device Status
#define E1000_EERD          0x0014  // EEPROM Read
#define E1000_ICR           0x00C0  // Interrupt Cause Read
#define E1000_ICS           0x00C8  // Interrupt Cause Set
#define E1000_IMS           0x00D0  // Interrupt Mask Set
#define E1000_IMC           0x00D8  // Interrupt Mask Clear
#define E1000_RCTL          0x0100  // Receive Control
#define E1000_TCTL          0x0400  // Transmit Control
#define E1000_RDBAL         0x2800  // RX Descriptor Base Low
#define E1000_RDBAH         0x2804  // RX Descriptor Base High
#define E1000_RDLEN         0x2808  // RX Descriptor Length
#define E1000_RDH           0x2810  // RX Descriptor Head
#define E1000_RDT           0x2818  // RX Descriptor Tail
#define E1000_TDBAL         0x3800  // TX Descriptor Base Low
#define E1000_TDBAH         0x3804  // TX Descriptor Base High
#define E1000_TDLEN         0x3808  // TX Descriptor Length
#define E1000_TDH           0x3810  // TX Descriptor Head
#define E1000_TDT           0x3818  // TX Descriptor Tail
#define E1000_RAL           0x5400  // Receive Address Low
#define E1000_RAH           0x5404  // Receive Address High
#define E1000_MTA           0x5200  // Multicast Table Array (128 entries)
#define E1000_TIPG          0x0410  // Transmit Inter-Packet Gap

// Extended descriptor registers
#define E1000_RXDCTL        0x2828  // RX Descriptor Control
#define E1000_TXDCTL        0x3828  // TX Descriptor Control
#define E1000_RFCTL         0x5008  // Receive Filter Control
#define E1000_RXCSUM        0x5000  // RX Checksum Control

// ============================================================================
// CTRL Register Bits
// ============================================================================
#define E1000_CTRL_FD       (1 << 0)    // Full Duplex
#define E1000_CTRL_ASDE     (1 << 5)    // Auto-Speed Detection Enable
#define E1000_CTRL_SLU      (1 << 6)    // Set Link Up
#define E1000_CTRL_RST      (1 << 26)   // Device Reset
#define E1000_CTRL_VME      (1 << 30)   // VLAN Mode Enable
#define E1000_CTRL_PHY_RST  (1 << 31)   // PHY Reset

// ============================================================================
// STATUS Register Bits
// ============================================================================
#define E1000_STATUS_FD     (1 << 0)    // Full Duplex
#define E1000_STATUS_LU     (1 << 1)    // Link Up
#define E1000_STATUS_SPEED_MASK (3 << 6)
#define E1000_STATUS_SPEED_10   (0 << 6)
#define E1000_STATUS_SPEED_100  (1 << 6)
#define E1000_STATUS_SPEED_1000 (2 << 6)

// ============================================================================
// EERD Register Bits
// ============================================================================
#define E1000_EERD_START    (1 << 0)    // Start Read
#define E1000_EERD_DONE     (1 << 4)    // Read Done
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16

// ============================================================================
// RCTL Register Bits
// ============================================================================
#define E1000_RCTL_EN       (1 << 1)    // Receiver Enable
#define E1000_RCTL_SBP      (1 << 2)    // Store Bad Packets
#define E1000_RCTL_UPE      (1 << 3)    // Unicast Promiscuous Enable
#define E1000_RCTL_MPE      (1 << 4)    // Multicast Promiscuous Enable
#define E1000_RCTL_LBM_NONE (0 << 6)    // No Loopback
#define E1000_RCTL_RDMTS_HALF (0 << 8)  // Free Buffer Threshold is 1/2
#define E1000_RCTL_BAM      (1 << 15)   // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 (0 << 16) // Buffer Size 2048
#define E1000_RCTL_BSIZE_256  (3 << 16) // Buffer Size 256
#define E1000_RCTL_SECRC    (1 << 26)   // Strip Ethernet CRC
#define E1000_RCTL_BSEX     (1 << 25)   // Buffer Size Extension

// ============================================================================
// TCTL Register Bits
// ============================================================================
#define E1000_TCTL_EN       (1 << 1)    // Transmitter Enable
#define E1000_TCTL_PSP      (1 << 3)    // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4           // Collision Threshold
#define E1000_TCTL_COLD_SHIFT 12        // Collision Distance
#define E1000_TCTL_RTLC     (1 << 24)   // Re-transmit on Late Collision

// ============================================================================
// Interrupt Cause Bits (ICR/IMS/IMC)
// ============================================================================
#define E1000_ICR_TXDW      (1 << 0)    // TX Descriptor Written Back
#define E1000_ICR_TXQE      (1 << 1)    // TX Queue Empty
#define E1000_ICR_LSC       (1 << 2)    // Link Status Change
#define E1000_ICR_RXSEQ     (1 << 3)    // RX Sequence Error
#define E1000_ICR_RXDMT0    (1 << 4)    // RX Descriptor Minimum Threshold
#define E1000_ICR_RXO       (1 << 6)    // RX Overrun
#define E1000_ICR_RXT0      (1 << 7)    // RX Timer Interrupt
#define E1000_ICR_INT_ASSERTED (1 << 31)

// ============================================================================
// TIPG default values
// ============================================================================
#define E1000_TIPG_IPGT     10
#define E1000_TIPG_IPGR1    (10 << 10)
#define E1000_TIPG_IPGR2    (10 << 20)

// ============================================================================
// Extended RX Descriptor (Read format)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;       // Address of the receive buffer
    uint64_t reserved;          // Reserved (written by hardware on completion)
} e1000_rx_desc_ext_read_t;

// Extended RX Descriptor (Write-Back format - filled by hardware)
typedef struct __attribute__((packed)) {
    uint32_t mrq;               // Multiple Receive Queues
    union {
        uint32_t rss;           // RSS Hash
        struct {
            uint16_t ip_id;
            uint16_t csum;
        } csum_ip;
    };
    uint32_t status_error;      // Status + Error
    uint16_t length;            // Packet length
    uint16_t vlan;              // VLAN tag
} e1000_rx_desc_ext_wb_t;

// Union of read/write-back formats for RX extended descriptors
typedef union {
    e1000_rx_desc_ext_read_t read;
    e1000_rx_desc_ext_wb_t wb;
} e1000_rx_desc_ext_t;

// Extended RX Status bits (in status_error field)
#define E1000_RXD_EXT_STAT_DD      (1 << 0)    // Descriptor Done
#define E1000_RXD_EXT_STAT_EOP     (1 << 1)    // End of Packet
#define E1000_RXD_EXT_STAT_VP      (1 << 3)    // VLAN Packet

// ============================================================================
// Extended TX Descriptor (Context format)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint32_t vlan_maclen_iplen;
    uint32_t launch_time;       // or fcoef
    uint32_t type_tucmd_mlhl;
    uint32_t mss_l4len_idx;
} e1000_tx_desc_ext_ctx_t;

// Extended TX Descriptor (Data format)
typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;       // Address of transmit buffer
    uint32_t cmd_type_len;      // Command, type, length
    uint32_t olinfo_status;     // Status + offload info
} e1000_tx_desc_ext_data_t;

// Union for TX extended descriptors
typedef union {
    e1000_tx_desc_ext_ctx_t ctx;
    e1000_tx_desc_ext_data_t data;
} e1000_tx_desc_ext_t;

// Extended TX Command bits (in cmd_type_len)
#define E1000_TXD_EXT_CMD_EOP      (1 << 24)   // End of Packet
#define E1000_TXD_EXT_CMD_IFCS     (1 << 25)   // Insert FCS
#define E1000_TXD_EXT_CMD_RS       (1 << 27)   // Report Status
#define E1000_TXD_EXT_CMD_DEXT     (1 << 29)   // Descriptor Extension (must be 1)
#define E1000_TXD_EXT_DTYP_DATA    (1 << 20)   // Data Descriptor Type

// Extended TX Status bits (in olinfo_status)
#define E1000_TXD_EXT_STAT_DD      (1 << 0)    // Descriptor Done
#define E1000_TXD_EXT_PAYLEN_SHIFT 14          // Payload length shift

// ============================================================================
// E1000 Ring Configuration
// ============================================================================
#define E1000_NUM_RX_DESC   256
#define E1000_NUM_TX_DESC   256
#define E1000_RX_BUF_SIZE   2048

// ============================================================================
// E1000 Device State
// ============================================================================
typedef struct {
    volatile uint8_t* mmio_base;    // MMIO-mapped register base
    uint64_t mmio_phys;             // Physical address of BAR0
    uint32_t mmio_size;             // Size of MMIO region

    // RX ring
    e1000_rx_desc_ext_t* rx_descs;  // RX descriptor ring (DMA)
    uint64_t rx_descs_phys;         // Physical address of RX ring
    uint8_t* rx_bufs[E1000_NUM_RX_DESC]; // RX packet buffers
    uint64_t rx_bufs_phys[E1000_NUM_RX_DESC];
    uint16_t rx_tail;               // Software RX tail

    // TX ring
    e1000_tx_desc_ext_t* tx_descs;  // TX descriptor ring (DMA)
    uint64_t tx_descs_phys;         // Physical address of TX ring
    uint8_t* tx_bufs[E1000_NUM_TX_DESC]; // TX packet buffers
    uint64_t tx_bufs_phys[E1000_NUM_TX_DESC];
    uint16_t tx_tail;               // Software TX tail

    // Device info
    uint8_t mac_addr[ETH_ALEN];
    const pci_device_t* pci_dev;
    uint8_t msi_vector;
    int link_up;
    uint16_t device_id;

    // Network device for registration
    net_device_t net_dev;
} e1000_dev_t;

// ============================================================================
// E1000 Driver API
// ============================================================================
void e1000_init(void);
void e1000_irq_handler(void);

#endif // _KERNEL_E1000_H_
