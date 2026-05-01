// LikeOS-64 Intel 82576 Gigabit Ethernet Controller (igb) NIC Driver
//
// Supports the 82575 / 82576 / I350 / I210 / I211 family of PCIe gigabit
// controllers.  The QEMU emulation target is `-device igb` (vendor 0x8086,
// device 0x10C9 = 82576 copper).  Register layout is largely a superset
// of the 8254x ("e1000") class, so the driver mirrors the e1000 driver's
// structure but adds a few 82576-specific touches:
//   - 128 KiB MMIO BAR0 (versus 128 KiB for 8254x — same size, different
//     internal layout for the per-queue rings)
//   - Per-queue RX/TX descriptor control registers at queue-0 offsets
//   - PCI Express capability for INTx-disable handling

#ifndef _KERNEL_IGB_H_
#define _KERNEL_IGB_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI Device IDs
// ============================================================================
#define IGB_VENDOR_ID           0x8086

// 82575
#define IGB_DEV_82575EB_COPPER  0x10A7
#define IGB_DEV_82575EB_FIBER   0x10A9
#define IGB_DEV_82575GB_QUAD    0x10D6
// 82576
#define IGB_DEV_82576           0x10C9   // QEMU `-device igb` (copper)
#define IGB_DEV_82576_FIBER     0x10E6
#define IGB_DEV_82576_SERDES    0x10E7
#define IGB_DEV_82576_QUAD      0x10E8
#define IGB_DEV_82576_NS        0x150A
#define IGB_DEV_82576_NS_SERDES 0x1518
#define IGB_DEV_82576_SERDES_QD 0x150D
#define IGB_DEV_82576_QUAD_ET2  0x1526
// I350
#define IGB_DEV_I350_COPPER     0x1521
#define IGB_DEV_I350_FIBER      0x1522
#define IGB_DEV_I350_SERDES     0x1523
#define IGB_DEV_I350_SGMII      0x1524
// I210/I211
#define IGB_DEV_I210_COPPER     0x1533
#define IGB_DEV_I210_FIBER      0x1536
#define IGB_DEV_I210_SERDES     0x1537
#define IGB_DEV_I210_SGMII      0x1538
#define IGB_DEV_I211_COPPER     0x1539

// ============================================================================
// MMIO Register Offsets (subset compatible across all family members)
// ============================================================================
#define IGB_CTRL            0x00000  // Device Control
#define IGB_STATUS          0x00008  // Device Status
#define IGB_CTRL_EXT        0x00018  // Extended Device Control
#define IGB_MDIC            0x00020  // MDI Control
#define IGB_EERD            0x00014  // EEPROM Read
#define IGB_ICR             0x000C0  // Interrupt Cause Read
#define IGB_ICS             0x000C8  // Interrupt Cause Set
#define IGB_IMS             0x000D0  // Interrupt Mask Set
#define IGB_IMC             0x000D8  // Interrupt Mask Clear
#define IGB_RCTL            0x00100  // Receive Control
#define IGB_TCTL            0x00400  // Transmit Control
#define IGB_TIPG            0x00410  // Transmit Inter-Packet Gap

// Per-queue RX (queue 0).  All per-queue blocks are 0x40 apart.
#define IGB_RDBAL0          0x0C000
#define IGB_RDBAH0          0x0C004
#define IGB_RDLEN0          0x0C008
#define IGB_SRRCTL0         0x0C00C  // Split & Replication RX Control
#define IGB_RDH0            0x0C010
#define IGB_RXCTL0          0x0C014
#define IGB_RDT0            0x0C018
#define IGB_RXDCTL0         0x0C028

// SRRCTL fields
#define IGB_SRRCTL_BSIZEPACKET_SHIFT  0    // packet buffer size, 1 KB units
#define IGB_SRRCTL_DESCTYPE_SHIFT     25   // 0 = legacy (NOT supported on 82576)
                                           // 1 = advanced one-buffer
#define IGB_SRRCTL_DESCTYPE_ADV1BUF   (1u << IGB_SRRCTL_DESCTYPE_SHIFT)
#define IGB_SRRCTL_DROP_EN            (1u << 31)

// MRQC (Multiple RX Queues Command).  We force single-queue, RSS off.
#define IGB_MRQC            0x05818

// Per-queue TX (queue 0)
#define IGB_TDBAL0          0x0E000
#define IGB_TDBAH0          0x0E004
#define IGB_TDLEN0          0x0E008
#define IGB_TDH0            0x0E010
#define IGB_TDT0            0x0E018
#define IGB_TXDCTL0         0x0E028

#define IGB_RAL0            0x05400
#define IGB_RAH0            0x05404
#define IGB_MTA             0x05200  // Multicast Table Array (128 entries)

#define IGB_WUC             0x05800  // Wake Up Control
#define IGB_WUFC            0x05808  // Wake Up Filter Control

// CTRL bits
#define IGB_CTRL_FD         (1u << 0)
#define IGB_CTRL_SLU        (1u << 6)
#define IGB_CTRL_RST        (1u << 26)
#define IGB_CTRL_PHY_RST    (1u << 31)

// CTRL_EXT bits
#define IGB_CTRL_EXT_DRV_LOAD (1u << 28)

// STATUS bits
#define IGB_STATUS_FD       (1u << 0)
#define IGB_STATUS_LU       (1u << 1)

// EERD bits (82576 layout: addr in [15:2], DONE in [1])
#define IGB_EERD_START      (1u << 0)
#define IGB_EERD_DONE       (1u << 1)
#define IGB_EERD_ADDR_SHIFT 2
#define IGB_EERD_DATA_SHIFT 16

// RCTL bits
#define IGB_RCTL_EN         (1u << 1)
#define IGB_RCTL_BAM        (1u << 15)
#define IGB_RCTL_BSIZE_2048 (0u << 16)
#define IGB_RCTL_SECRC      (1u << 26)

// TCTL bits
#define IGB_TCTL_EN         (1u << 1)
#define IGB_TCTL_PSP        (1u << 3)
#define IGB_TCTL_CT_SHIFT   4
#define IGB_TCTL_COLD_SHIFT 12
#define IGB_TCTL_RTLC       (1u << 24)

// RXDCTL / TXDCTL: bit 25 = ENABLE on per-queue control regs
#define IGB_QUEUE_ENABLE    (1u << 25)

// Interrupt cause bits (subset shared with e1000)
#define IGB_ICR_TXDW        (1u << 0)
#define IGB_ICR_LSC         (1u << 2)
#define IGB_ICR_RXDMT0      (1u << 4)
#define IGB_ICR_RXO         (1u << 6)
#define IGB_ICR_RXT0        (1u << 7)
#define IGB_ICR_RXQ0        (1u << 20)
#define IGB_ICR_TXQ0        (1u << 22)

// Advanced RX descriptor (read format on submission, writeback on completion).
// The 82576 supports ONLY the advanced format for receive — the legacy
// 8254x layout silently doesn't work.  Both halves of the union are
// exactly 16 bytes, matching the chip's expected descriptor stride.
typedef union __attribute__((packed)) {
    struct {
        uint64_t pkt_addr;     // bit 0 (A0) = 0; remaining bits = phys addr
        uint64_t hdr_addr;     // 0 when header split is disabled
    } read;
    struct {
        uint32_t info;         // packet type / RSS type / hdr_len / SPH
        uint32_t rss;          // RSS hash
        uint32_t status_error; // status [19:0] | error [31:20]
        uint16_t length;       // packet length (bytes)
        uint16_t vlan;
    } wb;
} igb_rx_desc_t;

// Legacy TX descriptor — still supported on the 82576 for backwards
// compat, identical to the 8254x layout.
typedef struct __attribute__((packed)) {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} igb_tx_desc_t;

#define IGB_RXD_STAT_DD     (1u << 0)
#define IGB_RXD_STAT_EOP    (1u << 1)
#define IGB_TXD_CMD_EOP     (1u << 0)
#define IGB_TXD_CMD_IFCS    (1u << 1)
#define IGB_TXD_CMD_RS      (1u << 3)
#define IGB_TXD_STAT_DD     (1u << 0)

#define IGB_NUM_RX_DESC     256
#define IGB_NUM_TX_DESC     256
#define IGB_RX_BUF_SIZE     2048

typedef struct {
    volatile uint8_t* mmio_base;
    uint64_t mmio_phys;
    uint32_t mmio_size;

    igb_rx_desc_t* rx_descs;
    uint64_t rx_descs_phys;
    uint8_t* rx_bufs[IGB_NUM_RX_DESC];
    uint64_t rx_bufs_phys[IGB_NUM_RX_DESC];
    uint16_t rx_tail;

    igb_tx_desc_t* tx_descs;
    uint64_t tx_descs_phys;
    uint8_t* tx_bufs[IGB_NUM_TX_DESC];
    uint64_t tx_bufs_phys[IGB_NUM_TX_DESC];
    uint16_t tx_tail;
    spinlock_t tx_lock;     // Serializes igb_send() across CPUs

    uint8_t mac_addr[ETH_ALEN];
    const pci_device_t* pci_dev;
    int link_up;
    uint16_t device_id;

    net_device_t net_dev;
} igb_dev_t;

void igb_init(void);
void igb_irq_handler(void);

#endif // _KERNEL_IGB_H_
