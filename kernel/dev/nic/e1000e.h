// LikeOS Intel e1000e (PCI Express) NIC Driver
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
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_E1000E_H_
#define _KERNEL_E1000E_H_

#include <kernel/uapi/types.h>
#include <kernel/hal/pci.h>
#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/ke/sched.h>
#include <kernel/dev/nic/e1000.h> // Reuse register definitions and descriptor structs

// ============================================================================
// PCI Device IDs handled by this driver
// ============================================================================
#define E1000E_VENDOR_ID 0x8086
#define E1000E_DEV_82574L 0x10D3 // QEMU `-device e1000e`
#define E1000E_DEV_82583V 0x150C // Intel 82583V Gigabit Network Connection

// ============================================================================
// Ring sizes (kept identical to e1000 to share descriptor layout)
// ============================================================================
#define E1000E_NUM_RX_DESC E1000_NUM_RX_DESC
#define E1000E_NUM_TX_DESC E1000_NUM_TX_DESC
#define E1000E_RX_BUF_SIZE E1000_RX_BUF_SIZE

// ============================================================================
// Device State
// ============================================================================
typedef struct {
	volatile uint8_t *mmio_base;
	uint64_t mmio_phys;
	uint32_t mmio_size;

	// RX ring (legacy descriptor format).  rx_tail is the last
	// descriptor handed to the hardware (RDT); the next one to clean
	// follows it.  Only the NAPI poll (NET_RX_CPU) touches the ring.
	e1000_rx_desc_legacy_t *rx_descs;
	uint64_t rx_descs_phys;
	uint8_t *rx_bufs[E1000E_NUM_RX_DESC];
	uint64_t rx_bufs_phys[E1000E_NUM_RX_DESC];
	uint16_t rx_tail;
	int rx_discarding; // inside a frame larger than one buffer

	// TX ring (legacy descriptor format): filled at tx_tail by the
	// transmit hook, reclaimed at tx_clean by the poll and the watchdog,
	// both under tx_lock.  tx_stamp: tick a descriptor was queued (0 =
	// free), for the hang check.
	e1000_tx_desc_legacy_t *tx_descs;
	uint64_t tx_descs_phys;
	uint8_t *tx_bufs[E1000E_NUM_TX_DESC];
	uint64_t tx_bufs_phys[E1000E_NUM_TX_DESC];
	uint64_t tx_stamp[E1000E_NUM_TX_DESC];
	uint16_t tx_tail;
	uint16_t tx_clean;
	spinlock_t tx_lock; // the TX ring, tx_tail/tx_clean, TDT/TDH

	// Device info
	uint8_t mac_addr[ETH_ALEN];
	const pci_device_t *pci_dev;
	uint8_t msi_vector;
	uint8_t buffers_cached; // packet buffers left write-back mapped
	uint16_t mta_regs; // 128 (8257x) or 32 (ICH/PCH)
	int link_up;
	uint32_t link_speed; // Mb/s, 0 while down
	int link_fd;
	int fc_mode; // NIC_FC_* in force
	// Hot-plug support: set by IRQ when LSC fires; consumed by the
	// watchdog (net housekeeping softirq, CPU 0), which does the edge
	// work that is not safe from interrupt context.
	volatile int link_change_pending;
	volatile int rx_mode_pending; // set_rx_mode: apply in the watchdog
	uint16_t device_id;
	/* e1000e_link_state_poll() runs from the link watchdog (CPU 0) and
	 * from init; one at a time, the other skips. */
	volatile int link_busy;
	/* CPU-side owner of the PHY: see e1000e_acquire_swflag(). */
	volatile int phy_busy;
	int swflag_held; // the hardware semaphore is ours (under phy_busy)
	/* LSC interrupt storm guard: LSC interrupts counted per one-second
	 * window; past the limit LSC is masked, and the watchdog unmasks it
	 * again a second later (it used to stay masked for good). */
	uint32_t lsc_count;
	uint64_t lsc_window_tick;
	uint64_t lsc_masked_tick; // 0 = LSC not masked

	// NAPI and interrupt moderation (the poll's state)
	struct napi_struct napi;
	int itr_rx_class, itr_tx_class; // NIC_ITR_*
	uint32_t itr_rate; // interrupts/s programmed
	uint32_t itr_rx_packets, itr_rx_bytes; // since the last ITR update
	uint32_t itr_tx_packets, itr_tx_bytes; // under tx_lock
	uint32_t itr_tx_acc_packets, itr_tx_acc_bytes; // the poll's

	// Statistics the hardware counts (accumulated by the watchdog)
	uint64_t stats_tick;
	uint64_t st_crc, st_align, st_rxerr, st_missed, st_colls;
	uint64_t st_latecol, st_ecol, st_tncrs, st_rlen, st_nobuf;
	uint64_t st_csum_err, st_tx_hangs, st_rx_frag;

	// Network device for registration
	net_device_t net_dev;
} e1000e_dev_t;

// ============================================================================
// Driver API
// ============================================================================
void e1000e_init(void);
int e1000e_irq_handler(void);

#endif // _KERNEL_E1000E_H_
