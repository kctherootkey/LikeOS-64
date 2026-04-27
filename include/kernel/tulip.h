// LikeOS-64 DEC 21x4x ("Tulip") NIC Driver
//
// Drives the DEC/Intel 21040 / 21041 / 21140 / 21142 / 21143 ("Tulip")
// family of 10/100 Mbit/s PCI Ethernet controllers.  QEMU exposes
// `-device tulip` as a 21143 (vendor 0x1011, device 0x0019).
//
// Programming model: 16 32-bit Control / Status Registers (CSR0..CSR15),
// each 8 bytes apart in BAR0.  We use BAR0 as MMIO if the chip exposes
// it that way (QEMU does), else fall back to PMIO via BAR0 I/O port.
// Single TX and RX descriptor ring, "ring" mode (not chained).

#ifndef _KERNEL_TULIP_H_
#define _KERNEL_TULIP_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"

#define TULIP_VENDOR_DEC          0x1011
#define TULIP_DEV_DC21040         0x0002
#define TULIP_DEV_DC21041         0x0014
#define TULIP_DEV_DC21140         0x0009
#define TULIP_DEV_DC21142_43      0x0019  // QEMU `-device tulip`

// CSR offsets (each CSR occupies 8 bytes; only the low 4 bytes are used)
#define TULIP_CSR0   0x00   // Bus mode
#define TULIP_CSR1   0x08   // TX poll demand
#define TULIP_CSR2   0x10   // RX poll demand
#define TULIP_CSR3   0x18   // RX descriptor list base
#define TULIP_CSR4   0x20   // TX descriptor list base
#define TULIP_CSR5   0x28   // Status
#define TULIP_CSR6   0x30   // Operating mode
#define TULIP_CSR7   0x38   // Interrupt enable
#define TULIP_CSR8   0x40   // Missed frames
#define TULIP_CSR9   0x48   // SROM / MII serial
#define TULIP_CSR10  0x50   // Diagnostic
#define TULIP_CSR11  0x58   // General-purpose timer
#define TULIP_CSR12  0x60   // SIA status (link)
#define TULIP_CSR13  0x68   // SIA connectivity
#define TULIP_CSR14  0x70   // SIA TX/RX
#define TULIP_CSR15  0x78   // SIA general

// CSR0 bits
#define TULIP_CSR0_SWR        (1u << 0)   // Software reset

// CSR5 (Status) bits
#define TULIP_CSR5_TI         (1u << 0)   // TX interrupt
#define TULIP_CSR5_RI         (1u << 6)   // RX interrupt
#define TULIP_CSR5_RU         (1u << 7)   // RX unavailable
#define TULIP_CSR5_LNF        (1u << 12)  // Link fail (21041/43)
#define TULIP_CSR5_LNP        (1u << 4)   // Link pass / change (varies)
#define TULIP_CSR5_NIS        (1u << 16)  // Normal interrupt summary
#define TULIP_CSR5_AIS        (1u << 15)  // Abnormal interrupt summary
#define TULIP_CSR5_TS_SHIFT   20          // TX state
#define TULIP_CSR5_RS_SHIFT   17          // RX state

// CSR6 (Operating Mode) bits
#define TULIP_CSR6_HP         (1u << 0)   // Hash/perfect filtering
#define TULIP_CSR6_SR         (1u << 1)   // Start RX
#define TULIP_CSR6_PR         (1u << 6)   // Promiscuous
#define TULIP_CSR6_PB         (1u << 3)   // Pass bad frames
#define TULIP_CSR6_FD         (1u << 9)   // Full duplex
#define TULIP_CSR6_TR_SHIFT   14          // TX threshold
#define TULIP_CSR6_ST         (1u << 13)  // Start TX
#define TULIP_CSR6_PS         (1u << 18)  // Port select (MII vs SIA)
#define TULIP_CSR6_SF         (1u << 21)  // Store and forward
#define TULIP_CSR6_TTM        (1u << 22)  // Transmit threshold mode

// CSR7 (Interrupt Enable) bits — same layout as CSR5
#define TULIP_CSR7_TIE        (1u << 0)
#define TULIP_CSR7_RIE        (1u << 6)
#define TULIP_CSR7_RUE        (1u << 7)
#define TULIP_CSR7_NIE        (1u << 16)
#define TULIP_CSR7_AIE        (1u << 15)
#define TULIP_CSR7_LFE        (1u << 12)  // Link fail interrupt enable
#define TULIP_CSR7_LPE        (1u << 4)   // Link pass interrupt enable

// CSR12 (SIA Status) bits — link sense
#define TULIP_CSR12_LS10      (1u << 1)   // Link status 10Base-T (active low)
#define TULIP_CSR12_LS100     (1u << 2)   // Link status 100Base-TX

// Descriptor flags
#define TULIP_RDES0_OWN       (1u << 31)
#define TULIP_RDES0_FS        (1u << 9)
#define TULIP_RDES0_LS        (1u << 8)
#define TULIP_RDES0_ES        (1u << 15)  // Error summary
#define TULIP_RDES0_FL_MASK   0x3FFF0000  // Frame length [29:16]
#define TULIP_RDES0_FL_SHIFT  16

#define TULIP_RDES1_RCH       (1u << 24)  // Second address chained
#define TULIP_RDES1_RER       (1u << 25)  // Receive end of ring

#define TULIP_TDES0_OWN       (1u << 31)
#define TULIP_TDES0_ES        (1u << 15)

#define TULIP_TDES1_FS        (1u << 29)  // First segment
#define TULIP_TDES1_LS        (1u << 30)  // Last segment
#define TULIP_TDES1_IC        (1u << 31)  // Interrupt on completion
#define TULIP_TDES1_TER       (1u << 25)  // Transmit end of ring
#define TULIP_TDES1_TCH       (1u << 24)  // Second address chained
#define TULIP_TDES1_SET       (1u << 27)  // Setup packet
#define TULIP_TDES1_AC        (1u << 26)  // Add CRC disable

typedef struct __attribute__((packed,aligned(16))) {
    uint32_t status;     // RDES0
    uint32_t ctrl;       // RDES1
    uint32_t buf1;       // RDES2 (physical)
    uint32_t buf2;       // RDES3 (next desc / second buffer)
} tulip_desc_t;

#define TULIP_NUM_RX_DESC   32
#define TULIP_NUM_TX_DESC   32
#define TULIP_RX_BUF_SIZE   2048
#define TULIP_TX_BUF_SIZE   2048

typedef struct {
    // BAR0 access — try MMIO first; if BAR is I/O space, fall back to PMIO
    int            use_mmio;
    volatile uint8_t* mmio_base;
    uint64_t       mmio_phys;
    uint16_t       io_base;

    tulip_desc_t*  rx_descs;
    uint64_t       rx_descs_phys;
    uint8_t*       rx_bufs[TULIP_NUM_RX_DESC];
    uint64_t       rx_bufs_phys[TULIP_NUM_RX_DESC];
    uint16_t       rx_cur;

    tulip_desc_t*  tx_descs;
    uint64_t       tx_descs_phys;
    uint8_t*       tx_bufs[TULIP_NUM_TX_DESC];
    uint64_t       tx_bufs_phys[TULIP_NUM_TX_DESC];
    uint16_t       tx_cur;

    uint8_t        mac_addr[ETH_ALEN];
    const pci_device_t* pci_dev;
    int            link_up;
    uint16_t       device_id;

    net_device_t   net_dev;
} tulip_dev_t;

void tulip_init(void);
void tulip_irq_handler(void);

#endif // _KERNEL_TULIP_H_
