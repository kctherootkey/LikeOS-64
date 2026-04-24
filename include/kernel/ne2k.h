// LikeOS-64 NE2000-compatible PCI NIC Driver
// Supports the Realtek RTL8029(AS) (vendor 0x10EC, device 0x8029) which
// is the PCI-bus member of the venerable NE2000 family.  Emulated by QEMU
// as `-device ne2k_pci` and shipped on countless cheap 90s NICs.

#ifndef _KERNEL_NE2K_H_
#define _KERNEL_NE2K_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI Device IDs
// ============================================================================
#define NE2K_VENDOR_ID          0x10EC
#define NE2K_DEV_RTL8029        0x8029

// ============================================================================
// Register offsets (relative to BAR0, I/O port space — 32 ports total)
// ============================================================================
// Page-independent
#define NE_CR           0x00    // Command Register
#define NE_DATA         0x10    // Remote DMA data port (read/write SRAM)
#define NE_RESET        0x1F    // Reset port (read to trigger reset)

// Page 0 — Read
#define NE_P0_CLDA0     0x01
#define NE_P0_CLDA1     0x02
#define NE_P0_BNRY      0x03    // Boundary pointer
#define NE_P0_TSR       0x04    // Transmit status
#define NE_P0_NCR       0x05
#define NE_P0_FIFO      0x06
#define NE_P0_ISR       0x07    // Interrupt status (W1C)
#define NE_P0_CRDA0     0x08
#define NE_P0_CRDA1     0x09
#define NE_P0_RSR       0x0C    // Receive status

// Page 0 — Write
#define NE_P0_PSTART    0x01    // Page start
#define NE_P0_PSTOP     0x02    // Page stop
#define NE_P0_TPSR      0x04    // Transmit page start
#define NE_P0_TBCR0     0x05    // Transmit byte count lo
#define NE_P0_TBCR1     0x06    // Transmit byte count hi
#define NE_P0_RSAR0     0x08    // Remote start addr lo
#define NE_P0_RSAR1     0x09    // Remote start addr hi
#define NE_P0_RBCR0     0x0A    // Remote byte count lo
#define NE_P0_RBCR1     0x0B    // Remote byte count hi
#define NE_P0_RCR       0x0C    // Receive config
#define NE_P0_TCR       0x0D    // Transmit config
#define NE_P0_DCR       0x0E    // Data config
#define NE_P0_IMR       0x0F    // Interrupt mask

// Page 1
#define NE_P1_PAR0      0x01    // Physical address (MAC) bytes 0..5
#define NE_P1_CURR      0x07    // Current page pointer
#define NE_P1_MAR0      0x08    // Multicast filter bytes 0..7

// ============================================================================
// CR (Command Register) bits
// ============================================================================
#define NE_CR_STP       (1 << 0)    // Stop
#define NE_CR_STA       (1 << 1)    // Start
#define NE_CR_TXP       (1 << 2)    // Transmit packet
#define NE_CR_RD_RD     (1 << 3)    // Remote read   (RD2:0 = 001)
#define NE_CR_RD_WR     (2 << 3)    // Remote write  (RD2:0 = 010)
#define NE_CR_RD_SEND   (3 << 3)    // Send packet   (RD2:0 = 011)
#define NE_CR_RD_ABORT  (4 << 3)    // Abort/complete (RD2:0 = 100)
#define NE_CR_PS0       (1 << 6)    // Page select bit 0
#define NE_CR_PS1       (1 << 7)    // Page select bit 1

// ============================================================================
// ISR / IMR bits (mirror each other)
// ============================================================================
#define NE_INT_PRX      (1 << 0)    // Packet received OK
#define NE_INT_PTX      (1 << 1)    // Packet transmitted OK
#define NE_INT_RXE      (1 << 2)    // Receive error
#define NE_INT_TXE      (1 << 3)    // Transmit error
#define NE_INT_OVW      (1 << 4)    // RX ring overflow
#define NE_INT_CNT      (1 << 5)    // Counter overflow
#define NE_INT_RDC      (1 << 6)    // Remote DMA complete
#define NE_INT_RST      (1 << 7)    // Reset done (read-only)

// ============================================================================
// DCR bits
// ============================================================================
#define NE_DCR_WTS      (1 << 0)    // Word transfer (16-bit)
#define NE_DCR_BOS      (1 << 1)    // Byte order select
#define NE_DCR_LAS      (1 << 2)    // Long DMA address (32-bit)
#define NE_DCR_LS       (1 << 3)    // Loopback select (1 = normal)
#define NE_DCR_AR       (1 << 4)    // Auto-init remote
#define NE_DCR_FT_8B    (2 << 5)    // FIFO threshold = 8 bytes

// ============================================================================
// RCR bits
// ============================================================================
#define NE_RCR_SEP      (1 << 0)    // Save errored packets
#define NE_RCR_AR       (1 << 1)    // Accept runts
#define NE_RCR_AB       (1 << 2)    // Accept broadcast
#define NE_RCR_AM       (1 << 3)    // Accept multicast
#define NE_RCR_PRO      (1 << 4)    // Promiscuous
#define NE_RCR_MON      (1 << 5)    // Monitor mode (RX disabled)

// ============================================================================
// TCR bits
// ============================================================================
#define NE_TCR_LB_NORMAL    0x00
#define NE_TCR_LB_INTERNAL  0x02

// ============================================================================
// SRAM layout (NE2000 has 16 KiB internal RAM at addr 0x4000..0x7FFF,
// addressed in 256-byte pages).  Standard layout:
//   TX buffer:  pages 0x40..0x45  (6 pages = 1536 bytes, one full Ethernet frame)
//   RX ring:    pages 0x46..0x7F  (58 pages)
// ============================================================================
#define NE_TX_PAGE_START    0x40
#define NE_RX_PAGE_START    0x46
#define NE_RX_PAGE_STOP     0x80    // exclusive
#define NE_PAGE_SIZE        256

// ============================================================================
// Per-device state
// ============================================================================
typedef struct {
    const pci_device_t* pci_dev;
    uint16_t io_base;
    uint8_t  mac_addr[6];

    uint8_t  next_pkt;          // Software RX read pointer (page number)
    int      link_up;
    uint8_t  msi_vector;        // Always 0 — chip has no MSI

    net_device_t net_dev;
} ne2k_dev_t;

// ============================================================================
// Public API
// ============================================================================
void ne2k_init(void);
void ne2k_irq_handler(void);

#endif // _KERNEL_NE2K_H_
