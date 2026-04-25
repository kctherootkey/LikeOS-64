// LikeOS-64 Intel 8255x ("eepro100") NIC Driver
//
// Supports the entire Intel 82557 / 82558 / 82559 / 82550 / 82551 / 82562 /
// 82801 family (vendor 0x8086).  In QEMU these correspond to the
// `-device i82550 / i82551 / i82557a / i82557b / i82557c / i82558a /
// i82558b / i82559a / i82559b / i82559c / i82559er / i82562 / i82801`
// models, all served by hw/net/eepro100.c with three distinct PCI device
// IDs:
//
//   0x1229 — i82557A/B/C, i82558A/B, i82559A/B/C
//   0x1209 — i82550, i82551, i82559ER, i82562
//   0x2449 — i82801
//
// All variants present the same Simplified Command Block / Receive Frame
// Area programming model used by Linux's `e100` driver, so a single
// driver covers every PCI ID above.

#ifndef _KERNEL_EEPRO100_H_
#define _KERNEL_EEPRO100_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

// ============================================================================
// PCI IDs
// ============================================================================
#define EEPRO100_VENDOR_ID      0x8086
#define EEPRO100_DEV_82557      0x1229      // 82557A/B/C, 82558A/B, 82559A/B/C
#define EEPRO100_DEV_82551IT    0x1209      // 82550, 82551, 82559ER, 82562
#define EEPRO100_DEV_82801      0x2449      // i82801 (ICH)

// ============================================================================
// SCB (System Control Block) register offsets — first 32 bytes of CSR space
// (BAR0 MMIO or BAR1 I/O).  Identical layout for every 8255x variant.
// ============================================================================
#define EEPRO100_SCB_STATUS     0x00    // 8-bit RO  — CU/RU state byte (CUS bits 7:6, RUS bits 5:2)
#define EEPRO100_SCB_ACK        0x01    // 8-bit RW  — STAT/ACK byte (CX/FR/CNA/RNR/MDI/SWI); read to see, write same bits to clear
#define EEPRO100_SCB_CMD        0x02    // 8-bit RW  — RU + CU command
#define EEPRO100_SCB_INTMASK    0x03    // 8-bit RW  — interrupt mask byte
#define EEPRO100_SCB_POINTER    0x04    // 32-bit    — generic DMA pointer
#define EEPRO100_SCB_PORT       0x08    // 32-bit    — software reset / selftest
#define EEPRO100_SCB_EEPROM     0x0E    // 16-bit    — bit-banged 93C46 EEPROM
#define EEPRO100_SCB_MDI        0x10    // 32-bit    — MII PHY access

// ============================================================================
// SCBCmd values (bit fields in the 8-bit SCBCmd register)
// ============================================================================
// Upper nibble — Command Unit (CU) commands
#define EEPRO100_CU_NOP         0x00
#define EEPRO100_CU_START       0x10
#define EEPRO100_CU_RESUME      0x20
#define EEPRO100_CU_LOAD_BASE   0x60
// Lower nibble — Receive Unit (RU) commands
#define EEPRO100_RU_NOP         0x00
#define EEPRO100_RU_START       0x01
#define EEPRO100_RU_RESUME      0x02
#define EEPRO100_RU_ABORT       0x04
#define EEPRO100_RU_LOAD_BASE   0x06
#define EEPRO100_RU_RESUMENR    0x07

// ============================================================================
// SCB status bits (high byte returned from SCB_STATUS)
// ============================================================================
#define EEPRO100_STAT_CX        0x80    // CU executed command with I bit
#define EEPRO100_STAT_FR        0x40    // RU finished a frame
#define EEPRO100_STAT_CNA       0x20    // CU went idle / suspended
#define EEPRO100_STAT_RNR       0x10    // RU went into NoResources state
#define EEPRO100_STAT_MDI       0x08    // MDI op done
#define EEPRO100_STAT_SWI       0x04    // Software-generated INT
#define EEPRO100_STAT_FCP       0x01    // Flow control pause

// ============================================================================
// SCB PORT operations
// ============================================================================
#define EEPRO100_PORT_SOFT_RESET 0
#define EEPRO100_PORT_SELFTEST   1
#define EEPRO100_PORT_SEL_RESET  2

// ============================================================================
// Command Block (TX / config / IA-setup) — 16-byte header, payload follows.
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t status;        // bit 15 = C (complete), bit 13 = OK
    uint16_t command;       // EL/S/I + cmd type
    uint32_t link;          // physical address of next CB
    uint32_t tbd_addr;      // 0xFFFFFFFF for simplified mode (data inline)
    uint16_t tcb_bytes;     // payload length (for CmdTx)
    uint8_t  tx_threshold;  // typical = 0xE0
    uint8_t  tbd_count;     // 0 in simplified mode
    // Payload follows immediately (CmdTx data, CmdConfigure bytes,
    // CmdIASetup MAC, etc.).
} eepro100_cb_t;

// CB command bits
#define EEPRO100_CMD_EL         0x8000  // End of CBL — CU suspends after this
#define EEPRO100_CMD_S          0x4000  // Suspend after this CB
#define EEPRO100_CMD_I          0x2000  // Generate CX interrupt on completion
#define EEPRO100_CMD_NOP        0x0000
#define EEPRO100_CMD_IASETUP    0x0001
#define EEPRO100_CMD_CONFIGURE  0x0002
#define EEPRO100_CMD_MULTICAST  0x0003
#define EEPRO100_CMD_TX         0x0004

// CB status bits
#define EEPRO100_STATUS_C       0x8000
#define EEPRO100_STATUS_OK      0x2000

// ============================================================================
// Receive Frame Descriptor (RFD) — 16-byte header, buffer follows inline
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t status;        // bit 15 = C, bit 13 = OK, bit 0..7 = RX flags
    uint16_t command;       // EL / S
    uint32_t link;          // physical address of next RFD
    uint32_t rx_buf_addr;   // 0 — buffer follows the header inline
    uint16_t count;         // bytes received (low 14 bits) | F + EOF bits
    uint16_t size;          // buffer capacity
    // Buffer follows immediately.
} eepro100_rfd_t;

// ============================================================================
// Driver constants
// ============================================================================
#define EEPRO100_NUM_RX_DESC    16
#define EEPRO100_NUM_TX_DESC    16          // Round-robin CB ring
#define EEPRO100_RX_BUF_SIZE    1518        // Max Ethernet frame
#define EEPRO100_TX_BUF_SIZE    1600        // Generous for any CmdTx frame

// Each TX CB block: 16-byte header + max payload, rounded to 8 bytes
#define EEPRO100_TX_BLOCK_SIZE  (16 + EEPRO100_TX_BUF_SIZE + 8)

// Each RX RFD block: 16-byte header + buffer
#define EEPRO100_RX_BLOCK_SIZE  (16 + EEPRO100_RX_BUF_SIZE + 8)

// ============================================================================
// Per-device state
// ============================================================================
typedef struct {
    const pci_device_t* pci_dev;
    uint16_t device_id;
    const char* model_name;

    // CSR access — either MMIO (preferred) or I/O port fallback
    volatile uint8_t* mmio_base;        // != NULL when using MMIO
    uint64_t          mmio_phys;
    uint16_t          io_base;          // != 0 when using I/O ports
    int               use_io;

    uint8_t  mac_addr[6];

    // TX command-block ring (single contiguous DMA region)
    uint8_t* tx_blocks;                 // virt
    uint64_t tx_blocks_phys;            // phys
    uint16_t tx_prod;                   // next slot to hand to HW
    uint16_t tx_cons;                   // next slot HW will complete

    // RX RFD ring (single contiguous DMA region)
    uint8_t* rx_blocks;
    uint64_t rx_blocks_phys;
    uint16_t rx_cur;

    int link_up;
    int cu_started;                     // first CU_START done, use CU_RESUME

    net_device_t net_dev;
} eepro100_dev_t;

void eepro100_init(void);
void eepro100_irq_handler(void);

#endif // _KERNEL_EEPRO100_H_
