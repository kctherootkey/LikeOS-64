// LikeOS-64 AMD PCnet32 NIC Driver
//
// Supports the AMD PCnet-PCI family (vendor 0x1022, device 0x2000):
//   - Am79C970A   "PCnet-PCI II"
//   - Am79C973    "PCnet-FAST III"
//   - QEMU `-device pcnet` (emulates Am79C970A)
//
// All three share the same PCI ID and the same software-style 2 (SWSTYLE=2)
// 32-bit DWord I/O programming model used here.

#ifndef _KERNEL_PCNET32_H_
#define _KERNEL_PCNET32_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI IDs
// ============================================================================
#define PCNET_VENDOR_ID         0x1022   // AMD
#define PCNET_DEV_LANCE         0x2000   // Am79C970A / Am79C973 / QEMU pcnet

// ============================================================================
// I/O register offsets relative to BAR0 (DWord/32-bit mode)
// ============================================================================
#define PCNET_APROM         0x00    // 16 bytes — first 6 = MAC
#define PCNET_RDP           0x10    // Register Data Port (32-bit)
#define PCNET_RAP           0x14    // Register Address Port (32-bit)
#define PCNET_RST           0x18    // Reset register (32-bit)
#define PCNET_BDP           0x1C    // BCR Data Port (32-bit)

// 16-bit word-mode aliases (used only for the initial reset)
#define PCNET_RDP16         0x10
#define PCNET_RAP16         0x12
#define PCNET_RST16         0x14
#define PCNET_BDP16         0x16

// ============================================================================
// CSR (Control/Status Register) indices
// ============================================================================
#define CSR0                0       // Status / control
#define CSR1                1       // IADR low (init block addr lo)
#define CSR2                2       // IADR high
#define CSR3                3       // Interrupt mask & deferral
#define CSR4                4       // Test & feature
#define CSR5                5       // Extended control/interrupt
#define CSR15               15      // Mode
#define CSR58               58      // Software style (alias of BCR20)

// CSR0 bits
#define CSR0_INIT           (1 << 0)    // Start initialisation
#define CSR0_STRT           (1 << 1)    // Start
#define CSR0_STOP           (1 << 2)    // Stop
#define CSR0_TDMD           (1 << 3)    // Transmit demand
#define CSR0_TXON           (1 << 4)
#define CSR0_RXON           (1 << 5)
#define CSR0_INEA           (1 << 6)    // Interrupt enable
#define CSR0_INTR           (1 << 7)    // Interrupt asserted (RO)
#define CSR0_IDON           (1 << 8)    // Init done
#define CSR0_TINT           (1 << 9)    // TX interrupt
#define CSR0_RINT           (1 << 10)   // RX interrupt
#define CSR0_MERR           (1 << 11)   // Memory error
#define CSR0_MISS           (1 << 12)   // Missed frame
#define CSR0_CERR           (1 << 13)   // Collision error
#define CSR0_BABL           (1 << 14)   // Babble (TX too long)
#define CSR0_ERR            (1 << 15)   // BABL|CERR|MISS|MERR (RO)

// CSR3 (interrupt-mask) bits  — set bit = MASKED (i.e. interrupt disabled)
#define CSR3_BABLM          (1 << 14)
#define CSR3_MISSM          (1 << 12)
#define CSR3_MERRM          (1 << 11)
#define CSR3_RINTM          (1 << 10)
#define CSR3_TINTM          (1 << 9)
#define CSR3_IDONM          (1 << 8)

// CSR4 features
#define CSR4_DMAPLUS        (1 << 14)   // Disable DMA Plus
#define CSR4_APAD_XMT       (1 << 11)   // Auto-pad TX short frames
#define CSR4_ASTRP_RCV      (1 << 10)   // Auto-strip RX FCS

// CSR15 (Mode) bits
#define CSR15_PROM          (1 << 15)   // Promiscuous mode
#define CSR15_DRCVBC        (1 << 14)   // Disable receive broadcast
#define CSR15_DTX           (1 << 1)    // Disable transmitter
#define CSR15_DRX           (1 << 0)    // Disable receiver

// ============================================================================
// BCR (Bus Configuration Register) indices
// ============================================================================
#define BCR2                2       // Misc config
#define BCR9                9       // Full-duplex control
#define BCR18               18      // Burst & bus control
#define BCR20               20      // Software style

#define BCR20_SSIZE32       (1 << 8)    // 32-bit software interface
#define BCR20_SWSTYLE_2     2           // PCnet-PCI II 32-bit style

// ============================================================================
// Init Block (SWSTYLE=2, 32-bit, 28 bytes — DMA-coherent, 4-byte aligned)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t mode;              // 0x00 — copied to CSR15 by chip
    uint8_t  rlen;              // 0x02 — upper 4 bits = log2(rx ring size)
    uint8_t  tlen;              // 0x03 — upper 4 bits = log2(tx ring size)
    uint8_t  padr[6];           // 0x04 — MAC address
    uint16_t reserved;          // 0x0A
    uint8_t  laddrf[8];         // 0x0C — logical address filter (multicast)
    uint32_t rdra;              // 0x14 — RX descriptor ring physical addr
    uint32_t tdra;              // 0x18 — TX descriptor ring physical addr
} pcnet_init_block_t;

// ============================================================================
// RX descriptor (16 bytes, SWSTYLE=2)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint32_t rbadr;             // 0x00 — RX buffer phys addr
    int16_t  bcnt;              // 0x04 — two's-complement of buffer byte count
    uint16_t status;            // 0x06 — bit 15 = OWN
    uint32_t mcnt;              // 0x08 — message byte count (lower 12 bits)
    uint32_t reserved;          // 0x0C
} pcnet_rx_desc_t;

// ============================================================================
// TX descriptor (16 bytes, SWSTYLE=2)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint32_t tbadr;             // 0x00 — TX buffer phys addr
    int16_t  bcnt;              // 0x04 — two's-complement of byte count
    uint16_t status;            // 0x06 — bit 15 = OWN, 8 = STP, 9 = ENP
    uint32_t misc;              // 0x08 — TX error info
    uint32_t reserved;          // 0x0C
} pcnet_tx_desc_t;

// RX status bits
#define RXD_OWN             (1 << 15)
#define RXD_ERR             (1 << 14)
#define RXD_FRAM            (1 << 13)   // Framing error
#define RXD_OFLO            (1 << 12)   // Overflow
#define RXD_CRC             (1 << 11)
#define RXD_BUFF            (1 << 10)   // Buffer error
#define RXD_STP             (1 << 9)    // Start of packet
#define RXD_ENP             (1 << 8)    // End of packet

// TX status bits
#define TXD_OWN             (1 << 15)
#define TXD_ERR             (1 << 14)
#define TXD_ADD_FCS         (1 << 13)   // (Read-only after submit)
#define TXD_MORE            (1 << 12)
#define TXD_ONE             (1 << 11)
#define TXD_DEF             (1 << 10)
#define TXD_STP             (1 << 9)    // Start of packet
#define TXD_ENP             (1 << 8)    // End of packet

// ============================================================================
// Driver constants
// ============================================================================
#define PCNET_NUM_RX_DESC   16          // log2 = 4 → rlen = 4 << 4 = 0x40
#define PCNET_NUM_TX_DESC   16
#define PCNET_RX_BUF_SIZE   2048
#define PCNET_TX_BUF_SIZE   2048

#define PCNET_RX_LEN_LOG2   4
#define PCNET_TX_LEN_LOG2   4

// ============================================================================
// Per-device state
// ============================================================================
typedef struct {
    const pci_device_t* pci_dev;
    uint16_t io_base;
    uint8_t  mac_addr[6];

    // DMA-coherent init block
    pcnet_init_block_t* init_block;
    uint64_t init_block_phys;

    // RX ring
    pcnet_rx_desc_t* rx_descs;
    uint64_t rx_descs_phys;
    uint8_t* rx_bufs[PCNET_NUM_RX_DESC];
    uint64_t rx_bufs_phys[PCNET_NUM_RX_DESC];
    uint16_t rx_cur;

    // TX ring
    pcnet_tx_desc_t* tx_descs;
    uint64_t tx_descs_phys;
    uint8_t* tx_bufs[PCNET_NUM_TX_DESC];
    uint64_t tx_bufs_phys[PCNET_NUM_TX_DESC];
    uint16_t tx_cur;
    spinlock_t tx_lock;     // Serializes pcnet_send() across CPUs

    int link_up;

    net_device_t net_dev;
} pcnet_dev_t;

void pcnet32_init(void);
void pcnet32_irq_handler(void);

#endif // _KERNEL_PCNET32_H_
