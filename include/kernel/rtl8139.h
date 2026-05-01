// LikeOS-64 Realtek RTL8139 NIC Driver
// Supports the RTL8139 (vendor 0x10EC, device 0x8139) PCI 10/100 Ethernet
// controller as emulated by QEMU `-device rtl8139` and most real
// RTL8139/RTL8139C/RTL8139C+ silicon.

#ifndef _KERNEL_RTL8139_H_
#define _KERNEL_RTL8139_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI Device IDs
// ============================================================================
#define RTL8139_VENDOR_ID       0x10EC
#define RTL8139_DEV_RTL8139     0x8139

// ============================================================================
// Register offsets (relative to BAR0 — I/O port space)
// ============================================================================
#define RTL_IDR0        0x00    // MAC address (6 bytes IDR0..IDR5)
#define RTL_MAR0        0x08    // Multicast filter (8 bytes)
#define RTL_TSD0        0x10    // TX Status of descriptor 0..3 (4 x dword)
#define RTL_TSAD0       0x20    // TX Start Address of descriptor 0..3
#define RTL_RBSTART     0x30    // RX Buffer start (dword)
#define RTL_ERBCR       0x34    // Early RX Byte Count
#define RTL_ERSR        0x36    // Early RX Status
#define RTL_CR          0x37    // Command Register (byte)
#define RTL_CAPR        0x38    // Current Address of Packet Read (word)
#define RTL_CBR         0x3A    // Current Buffer Address (word)
#define RTL_IMR         0x3C    // Interrupt Mask Register (word)
#define RTL_ISR         0x3E    // Interrupt Status Register (word)
#define RTL_TCR         0x40    // TX Configuration Register (dword)
#define RTL_RCR         0x44    // RX Configuration Register (dword)
#define RTL_TCTR        0x48    // Timer Counter
#define RTL_MPC         0x4C    // Missed Packet Counter
#define RTL_9346CR      0x50    // 93C46 EEPROM Command Register (byte)
#define RTL_CONFIG0     0x51
#define RTL_CONFIG1     0x52
#define RTL_TIMERINT    0x54
#define RTL_MSR         0x58    // Media Status Register (byte)
#define RTL_CONFIG3     0x59
#define RTL_CONFIG4     0x5A
#define RTL_BMCR        0x62    // Basic Mode Control Register (word)
#define RTL_BMSR        0x64    // Basic Mode Status Register (word)

// ============================================================================
// CR (Command) bits (offset 0x37, 8-bit)
// ============================================================================
#define RTL_CR_BUFE     (1 << 0)    // RX buffer empty (read-only)
#define RTL_CR_TE       (1 << 2)    // Transmitter enable
#define RTL_CR_RE       (1 << 3)    // Receiver enable
#define RTL_CR_RST      (1 << 4)    // Software reset (auto-clears)

// ============================================================================
// 9346CR (EEPROM control) bits (offset 0x50, 8-bit)
// ============================================================================
#define RTL_9346CR_NORMAL   0x00    // Normal mode (registers locked)
#define RTL_9346CR_CFGWE    0xC0    // Config registers write enable

// ============================================================================
// IMR / ISR bits (offsets 0x3C / 0x3E, 16-bit)
// ============================================================================
#define RTL_INT_ROK         (1 << 0)    // Receive OK
#define RTL_INT_RER         (1 << 1)    // Receive Error
#define RTL_INT_TOK         (1 << 2)    // Transmit OK
#define RTL_INT_TER         (1 << 3)    // Transmit Error
#define RTL_INT_RXOVW       (1 << 4)    // RX Buffer Overflow
#define RTL_INT_PUN         (1 << 5)    // Packet Underrun / Link Change
#define RTL_INT_FOVW        (1 << 6)    // RX FIFO Overflow
#define RTL_INT_LENCHG      (1 << 13)   // Cable length changed
#define RTL_INT_TIMEOUT     (1 << 14)
#define RTL_INT_SERR        (1 << 15)   // System Error

// ============================================================================
// RCR (RX Config) bits (offset 0x44, 32-bit)
// ============================================================================
#define RTL_RCR_AAP         (1 << 0)    // Accept All Packets (promiscuous)
#define RTL_RCR_APM         (1 << 1)    // Accept Physical Match (our MAC)
#define RTL_RCR_AM          (1 << 2)    // Accept Multicast
#define RTL_RCR_AB          (1 << 3)    // Accept Broadcast
#define RTL_RCR_AR          (1 << 4)    // Accept Runt
#define RTL_RCR_AER         (1 << 5)    // Accept Error
#define RTL_RCR_WRAP        (1 << 7)    // Wrap bit (use linear ring + 1500)
#define RTL_RCR_RBLEN_8K    (0 << 11)   // 8K + 16 bytes
#define RTL_RCR_RBLEN_16K   (1 << 11)   // 16K + 16 bytes
#define RTL_RCR_RBLEN_32K   (2 << 11)   // 32K + 16 bytes
#define RTL_RCR_RBLEN_64K   (3 << 11)   // 64K + 16 bytes
#define RTL_RCR_MXDMA_1024  (6 << 8)    // Max DMA burst = 1024 bytes
#define RTL_RCR_MXDMA_UNLIM (7 << 8)    // Max DMA burst = unlimited

// ============================================================================
// TCR (TX Config) bits (offset 0x40, 32-bit)
// ============================================================================
#define RTL_TCR_MXDMA_1024  (6 << 8)    // Max DMA burst = 1024 bytes
#define RTL_TCR_IFG_NORMAL  (3 << 24)   // Normal IFG
#define RTL_TCR_LBK_OFF     (0 << 17)

// ============================================================================
// TSD (TX Status of descriptor N) bits (offsets 0x10/14/18/1C, 32-bit)
// ============================================================================
#define RTL_TSD_OWN         (1 << 13)   // Set when DMA is complete
#define RTL_TSD_TUN         (1 << 14)   // TX FIFO Underrun
#define RTL_TSD_TOK         (1 << 15)   // Transmit OK
#define RTL_TSD_SIZE_MASK   0x1FFF      // Bits 0-12 hold size

// ============================================================================
// MSR (Media Status) bits (offset 0x58, 8-bit)
// ============================================================================
#define RTL_MSR_LINKB       (1 << 2)    // Inverse Link status (1 = link DOWN)
#define RTL_MSR_SPEED_10    (1 << 3)    // 1 = 10Mb, 0 = 100Mb

// ============================================================================
// Driver constants
// ============================================================================
// Use 8K + 16 RX buffer (smallest, fits in 4 pages)
#define RTL8139_RX_BUF_SIZE     (8192 + 16)
#define RTL8139_RX_BUF_PAD      1500   // Extra slack for WRAP-mode reads
#define RTL8139_RX_BUF_TOTAL    (RTL8139_RX_BUF_SIZE + RTL8139_RX_BUF_PAD)
#define RTL8139_NUM_TX_DESC     4
#define RTL8139_TX_BUF_SIZE     2048   // One page per TX buffer

// ============================================================================
// Per-device state
// ============================================================================
typedef struct {
    const pci_device_t* pci_dev;
    uint16_t io_base;       // I/O port base from BAR0
    uint8_t  mac_addr[6];

    // RX ring (single contiguous DMA buffer)
    uint8_t* rx_buf;
    uint64_t rx_buf_phys;
    uint16_t rx_offset;     // Software read pointer (matches CAPR + 16)

    // TX descriptors
    uint8_t* tx_bufs[RTL8139_NUM_TX_DESC];
    uint64_t tx_bufs_phys[RTL8139_NUM_TX_DESC];
    uint8_t  tx_cur;        // Next descriptor to use (0..3)
    spinlock_t tx_lock;     // Serializes rtl8139_send() across CPUs

    int link_up;
    uint8_t msi_vector;     // 0 if MSI not in use (RTL8139 has no MSI)

    net_device_t net_dev;
} rtl8139_dev_t;

// ============================================================================
// Public API
// ============================================================================
void rtl8139_init(void);
void rtl8139_irq_handler(void);

#endif // _KERNEL_RTL8139_H_
