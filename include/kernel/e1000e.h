// LikeOS-64 Intel e1000e (PCI Express) NIC Driver
//
// Targets the gigabit PCI-Express family of Intel server/desktop NICs that
// share the 82574L register layout:
//   - 82574L  (QEMU's `-device e1000e`)
//   - 82583V  (Intel 82583V Gigabit Network Connection)
//
// Note: The VirtualBox NIC choices "Intel PRO/1000 MT Desktop" (82540EM),
// "Intel PRO/1000 MT Server" (82545EM) and "Intel PRO/1000 T Server"
// (82543GC) are all e1000-class parts and are handled by the separate
// e1000 driver, NOT this driver.
//
// The legacy descriptor format is used for both RX and TX (the same format
// the older e1000 driver uses); this keeps the driver simple while still
// being functionally correct for the supported hardware.

#ifndef _KERNEL_E1000E_H_
#define _KERNEL_E1000E_H_

#include "types.h"
#include "pci.h"
#include "net.h"
#include "sched.h"
#include "e1000.h"   // Reuse register definitions and descriptor structs

// ============================================================================
// PCI Device IDs handled by this driver
// ============================================================================
#define E1000E_VENDOR_ID        0x8086
#define E1000E_DEV_82574L       0x10D3   // QEMU `-device e1000e`
#define E1000E_DEV_82583V       0x150C   // Intel 82583V Gigabit Network Connection

// ============================================================================
// Ring sizes (kept identical to e1000 to share descriptor layout)
// ============================================================================
#define E1000E_NUM_RX_DESC      E1000_NUM_RX_DESC
#define E1000E_NUM_TX_DESC      E1000_NUM_TX_DESC
#define E1000E_RX_BUF_SIZE      E1000_RX_BUF_SIZE

// ============================================================================
// Device State
// ============================================================================
typedef struct {
    volatile uint8_t* mmio_base;
    uint64_t mmio_phys;
    uint32_t mmio_size;

    // RX ring (legacy descriptor format)
    e1000_rx_desc_legacy_t* rx_descs;
    uint64_t rx_descs_phys;
    uint8_t* rx_bufs[E1000E_NUM_RX_DESC];
    uint64_t rx_bufs_phys[E1000E_NUM_RX_DESC];
    uint16_t rx_tail;

    // TX ring (legacy descriptor format)
    e1000_tx_desc_legacy_t* tx_descs;
    uint64_t tx_descs_phys;
    uint8_t* tx_bufs[E1000E_NUM_TX_DESC];
    uint64_t tx_bufs_phys[E1000E_NUM_TX_DESC];
    uint16_t tx_tail;

    // Device info
    uint8_t mac_addr[ETH_ALEN];
    const pci_device_t* pci_dev;
    uint8_t msi_vector;
    int link_up;
    // Hot-plug support: set by IRQ when LSC fires; consumed in
    // process context (e1000e_link_status / e1000e_send) to perform
    // the heavy edge handling (re-clear OEM bits, etc.) that is not
    // safe from interrupt context.
    volatile int link_change_pending;
    uint16_t device_id;

    // Network device for registration
    net_device_t net_dev;
} e1000e_dev_t;

// ============================================================================
// Driver API
// ============================================================================
void e1000e_init(void);
void e1000e_irq_handler(void);

#endif // _KERNEL_E1000E_H_
