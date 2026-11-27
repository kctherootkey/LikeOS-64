// LikeOS Intel e1000e (PCIe gigabit) NIC Driver
//
// Targets (verified working on real hardware and/or hypervisor):
//   - Intel 82574L Gigabit Network Connection                 (PCI 0x10D3)
//       Used by QEMU `-device e1000e`.  Original target of this driver.
//   - Intel 82583V Gigabit Network Connection                 (PCI 0x150C)
//   - Intel I217-LM / I217-V                                  (PCH LPT LOM)
//   - Intel I218-LM / I218-V / I218-LM2 / I218-V2 / I218-LM3  (PCH LPT-LP/WPT LOM)
//   - Intel I219-LM / I219-V family, generations 1..29        (PCH SPT through MTP/LNP/ARL/PTP/NVL)
//       Verified on Lenovo ThinkPad P50 (PCH SPT-H, dev 0x156F).
//
// Note: The VirtualBox NIC choices "Intel PRO/1000 MT Desktop" (82540EM)
// and "Intel PRO/1000 MT Server" (82545EM) are e1000-class parts and are
// handled by the separate e1000 driver, not this one.
//
// ---------------------------------------------------------------------------
// Implementation status
// ---------------------------------------------------------------------------
// The 82574L register layout is largely backwards-compatible with the classic
// e1000 (82540EM/82545EM): we reuse the legacy RX/TX descriptor formats and
// the same MMIO offsets defined in <kernel/dev/nic/e1000.h>.  The extra PCIe / MSI-X
// facilities that distinguish e1000e proper from e1000 are NOT used here —
// the legacy programming model is sufficient for our purposes (UEFI desktop
// systems, hypervisor emulation, business-laptop LOMs) and matches the
// interrupt/DMA-handling style of the existing e1000 driver.
//
// What works:
//   * Probe / BAR0 MMIO mapping (uncached) for all supported parts.
//   * Full ICH8/9/10 + PCH (LPT/SPT/CNP/ICP/CMP/TGP/ADP/RPL/MTP/LNP/ARL/PTP/NVL)
//     bring-up:
//       - PHY ULP exit (CV_SMB_CTRL / I218_ULP_CONFIG1 / FEXTNVM7 handshake)
//       - LANPHYPC toggle and LAN_INIT_DONE wait on cold boot
//       - PCIe master-disable drain + CTRL.RST sequenced reset
//       - CTRL_EXT (DRV_LOAD / PHY_PDEN / bit22) re-programmed post-reset
//       - PBA reprogrammed for SPT family
//       - HV_OEM_BITS LPLU / GBE_DIS cleared (BIOS hands the PHY off with
//         LPLU=1 on Lenovo P50; without this TX hangs on link-up)
//       - K1 disabled, KMRN HD_CTRL programmed for negotiated speed
//       - K1 workaround for LPT-LP / SPT / CNP+ at 100 Mb FD
//       - EEE disabled at MAC and PHY (EEER / IPCNFG / EMI 0x8001)
//       - I82577 PHY config (CRS_ON_TX, downshift=3)
//       - SPT TX-hang workaround: IOSFPC bit 16, FEXTNVM11.DISABLE_MULR_FIX,
//         TARC0 CB_MULTIQ_2_REQ
//       - TCTL.MULR + RRTHRESH set on PCH-LAN (the actual TX-stall gate;
//         without MULR the TX engine fetches one descriptor and stalls)
//       - KABGTXD.BGSQLBIAS, MAC DPG exit, ME H2ME ENFORCE_SETTINGS
//   * Legacy RX/TX descriptor rings, single-queue, contiguous DMA;
//     packet buffers write-back mapped where the DMA is snooped (8257x,
//     SPT and later), descriptor rings uncached.
//   * NAPI receive (budgeted ring drain, descriptor error checks), TX
//     reclaim in the poll with queue stop/wake, adaptive interrupt
//     moderation (ITR), RX checksum offload, multicast table, hardware
//     statistics, transmit hang detection.
//   * Per-link-up configuration for the speed negotiated each time (KMRN
//     timing, K1, and on SPT+ TIPG, PLL clock gate, pointer gap, LTR,
//     flow control resolved from the autonegotiation).
//   * Legacy INTx via ACPI _PRT GSI lookup, or MSI when available.
//   * Link state tracking via LSC interrupt + STATUS.LU polling.
//   * MAC address read from RAL0/RAH0 (UEFI-programmed) with EEPROM fallback.
//
// What is intentionally NOT implemented:
//   * MSI-X, multi-queue RX/TX, RSS, VMDq, SR-IOV.
//   * TX checksum offload, TSO/LSO, header-split.
//   * VLAN tagging (CTRL.VME left at hardware default).
//   * Wake-on-LAN / D3-cold (CTRL.ADVD3WUC left at hardware default).
//   * Energy-Efficient Ethernet (explicitly disabled — saves debugging pain).
//   * NVM / EEPROM writes, firmware update path.
//
// Diagnostics:
//   By default the driver prints only the e1000-style essentials (probe,
//   MAC, IRQ/MSI selection, link state, init-success, allocation errors).
//   Set E1000E_DBG to 1 (just below the includes) to enable the full
//   verbose register-dump / state-trace firehose used during bring-up.
// ---------------------------------------------------------------------------
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/e1000e.h>
#include <kernel/dev/nic/e1000.h>
#include <kernel/dev/nic/nic_util.h>
#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/skb.h>
#include <kernel/hal/pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/interrupt.h>
#include <kernel/mm/slab.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/ioapic.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// Debug / diagnostic logging.
//
// Set E1000E_DBG to 1 to enable verbose driver diagnostics (register dumps,
// PHY/MAC state transitions, TX/RX path tracing).  When 0 (the default)
// every e1000e_dbg() call collapses to a no-op and is fully eliminated by
// the compiler, leaving zero overhead and a silent driver.
//
// All informational/diagnostic output in this file goes through e1000e_dbg();
// only true error-recovery surface should ever call kprintf directly
// (currently nothing does).
// ============================================================================
#define E1000E_DBG 0

#if E1000E_DBG
static void e1000e_dbg(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	kvprintf(fmt, ap);
	__builtin_va_end(ap);
}
#else
// When debug is off, e1000e_dbg() collapses to (void)0 via a macro so the
// variadic arguments are NEVER evaluated at the call site.  This is critical
// because most diagnostic calls feed live MMIO reads (e1000e_read(...)) as
// arguments — leaving those reads in the binary would defeat the purpose of
// disabling debug.  The do-while idiom keeps the call statement-safe in
// every context (if/else without braces, etc.).
#define e1000e_dbg(...) \
	do {            \
	} while (0)
#endif

// ============================================================================
// Supported PCI device IDs.  Compiled from publicly documented
// hw.h (E1000_DEV_ID_*).  Covers the
// entire 8257x family plus 80003ES2LAN, ICH8/9/10, and the PCH (LPT, SPT,
// LBG, CNP, ICP, CMP, TGP, ADP, RPL, MTP, LNP, ARL, PTP, NVL) I217/I218/I219
// LOM parts that ship in virtually every Intel-based business laptop and
// desktop board from 2012 onwards (including the Lenovo ThinkPad P50's
// I219-LM at 0x156F).  Labels follow the hw.h part name and PCH generation.
// ============================================================================
typedef struct {
	uint16_t did;
	const char *name;
} e1000e_id_t;

static const e1000e_id_t e1000e_pci_ids[] = {
	// ---- 82571 family ----
	{ 0x105E, "82571EB Copper (dual)" },
	{ 0x105F, "82571EB Fiber (dual)" },
	{ 0x1060, "82571EB SerDes (dual)" },
	{ 0x10A4, "82571EB Quad Copper" },
	{ 0x10A5, "82571EB Quad Fiber" },
	{ 0x10BC, "82571EB Quad Copper LP" },
	{ 0x10D9, "82571EB SerDes Dual" },
	{ 0x10DA, "82571EB SerDes Quad" },
	{ 0x10D5, "82571PT Quad Copper" },
	// ---- 82572 family ----
	{ 0x107D, "82572EI Copper" },
	{ 0x107E, "82572EI Fiber" },
	{ 0x107F, "82572EI SerDes" },
	{ 0x10B9, "82572EI" },
	// ---- 82573 family ----
	{ 0x108B, "82573E" },
	{ 0x108C, "82573E IAMT" },
	{ 0x109A, "82573L" },
	// ---- 82574 / 82583 ----
	{ 0x10D3, "82574L" }, // QEMU -device e1000e
	{ 0x10F6, "82574LA" },
	{ 0x150C, "82583V" },
	// ---- 80003ES2LAN ----
	{ 0x1096, "80003ES2LAN Copper (DPT)" },
	{ 0x1098, "80003ES2LAN SerDes (DPT)" },
	{ 0x10BA, "80003ES2LAN Copper (SPT)" },
	{ 0x10BB, "80003ES2LAN SerDes (SPT)" },
	// ---- ICH8 ----
	{ 0x1049, "ICH8 IGP M Amt" },
	{ 0x104A, "ICH8 IGP Amt" },
	{ 0x104B, "ICH8 IGP C" },
	{ 0x104C, "ICH8 IFE" },
	{ 0x104D, "ICH8 IGP M" },
	{ 0x10C4, "ICH8 IFE GT" },
	{ 0x10C5, "ICH8 IFE G" },
	{ 0x1501, "ICH8 82567V-3" },
	// ---- ICH9 ----
	{ 0x10BD, "ICH9 IGP Amt" },
	{ 0x10BF, "ICH9 IGP M" },
	{ 0x10CB, "ICH9 IGP M V" },
	{ 0x10E5, "ICH9 BM" },
	{ 0x294C, "ICH9 IGP C" },
	{ 0x10F5, "ICH9 IGP M Amt" },
	{ 0x10C0, "ICH9 IFE" },
	{ 0x10C2, "ICH9 IFE G" },
	{ 0x10C3, "ICH9 IFE GT" },
	// ---- ICH10 ----
	{ 0x10CC, "ICH10 R BM LM" },
	{ 0x10CD, "ICH10 R BM LF" },
	{ 0x10CE, "ICH10 R BM V" },
	{ 0x10DE, "ICH10 D BM LM" },
	{ 0x10DF, "ICH10 D BM LF" },
	{ 0x1525, "ICH10 D BM V" },
	// ---- PCH (Ibex Peak) ----
	{ 0x10EA, "PCH M HV LM" },
	{ 0x10EB, "PCH M HV LC" },
	{ 0x10EF, "PCH D HV DM" },
	{ 0x10F0, "PCH D HV DC" },
	// ---- PCH2 (Cougar Point) ----
	{ 0x1502, "PCH2 LV LM" },
	{ 0x1503, "PCH2 LV V" },
	// ---- LPT / I217 (Lynx Point, Haswell PCH) ----
	{ 0x153A, "I217-LM (LPT)" },
	{ 0x153B, "I217-V (LPT)" },
	// ---- LPT-LP / WPT / I218 (Lynx Point LP, Wildcat Point) ----
	{ 0x155A, "I218-LM (LPT-LP)" },
	{ 0x1559, "I218-V (LPT-LP)" },
	{ 0x15A0, "I218-LM2 (LPT)" },
	{ 0x15A1, "I218-V2 (LPT)" },
	{ 0x15A2, "I218-LM3 (WPT)" },
	{ 0x15A3, "I218-V3 (WPT)" },
	// ---- SPT / LBG / I219 (Sunrise Point, Lewisburg) ----  ← ThinkPad P50
	{ 0x156F, "I219-LM (SPT)" },
	{ 0x1570, "I219-V (SPT)" },
	{ 0x15B7, "I219-LM2 (SPT-H)" },
	{ 0x15B8, "I219-V2 (SPT-H)" },
	{ 0x15B9, "I219-LM3 (LBG)" },
	{ 0x15D7, "I219-LM4 (SPT)" },
	{ 0x15D8, "I219-V4 (SPT)" },
	{ 0x15E3, "I219-LM5 (SPT)" },
	{ 0x15D6, "I219-V5 (SPT)" },
	// ---- CNP / I219 (Cannon Point) ----
	{ 0x15BD, "I219-LM6 (CNP)" },
	{ 0x15BE, "I219-V6 (CNP)" },
	{ 0x15BB, "I219-LM7 (CNP)" },
	{ 0x15BC, "I219-V7 (CNP)" },
	// ---- ICP / I219 (Ice Point) ----
	{ 0x15DF, "I219-LM8 (ICP)" },
	{ 0x15E0, "I219-V8 (ICP)" },
	{ 0x15E1, "I219-LM9 (ICP)" },
	{ 0x15E2, "I219-V9 (ICP)" },
	// ---- CMP / I219 (Comet Point) ----
	{ 0x0D4E, "I219-LM10 (CMP)" },
	{ 0x0D4F, "I219-V10 (CMP)" },
	{ 0x0D4C, "I219-LM11 (CMP)" },
	{ 0x0D4D, "I219-V11 (CMP)" },
	{ 0x0D53, "I219-LM12 (CMP)" },
	{ 0x0D55, "I219-V12 (CMP)" },
	// ---- TGP / I219 (Tiger Point) ----
	{ 0x15FB, "I219-LM13 (TGP)" },
	{ 0x15FC, "I219-V13 (TGP)" },
	{ 0x15F9, "I219-LM14 (TGP)" },
	{ 0x15FA, "I219-V14 (TGP)" },
	{ 0x15F4, "I219-LM15 (TGP)" },
	{ 0x15F5, "I219-V15 (TGP)" },
	// ---- ADP / RPL / I219 (Alder Point, Raptor Point) ----
	{ 0x1A1E, "I219-LM16 (ADP)" },
	{ 0x1A1F, "I219-V16 (ADP)" },
	{ 0x1A1C, "I219-LM17 (ADP)" },
	{ 0x1A1D, "I219-V17 (ADP)" },
	{ 0x550C, "I219-LM19 (ADP)" },
	{ 0x550D, "I219-V19 (ADP)" },
	{ 0x0DC7, "I219-LM22 (RPL)" },
	{ 0x0DC8, "I219-V22 (RPL)" },
	{ 0x0DC5, "I219-LM23 (RPL)" },
	{ 0x0DC6, "I219-V23 (RPL)" },
	// ---- MTP / LNP / ARL / PTP / NVL / I219 ----
	{ 0x550A, "I219-LM18 (MTP)" },
	{ 0x550B, "I219-V18 (MTP)" },
	{ 0x550E, "I219-LM20 (LNP)" },
	{ 0x550F, "I219-V20 (LNP)" },
	{ 0x5510, "I219-LM21 (LNP)" },
	{ 0x5511, "I219-V21 (LNP)" },
	{ 0x57A0, "I219-LM24 (ARL)" },
	{ 0x57A1, "I219-V24 (ARL)" },
	{ 0x57B3, "I219-LM25 (PTP)" },
	{ 0x57B4, "I219-V25 (PTP)" },
	{ 0x57B5, "I219-LM26 (PTP)" },
	{ 0x57B6, "I219-V26 (PTP)" },
	{ 0x57B7, "I219-LM27 (PTP)" },
	{ 0x57B8, "I219-V27 (PTP)" },
	{ 0x57B9, "I219-LM29 (NVL)" },
	{ 0x57BA, "I219-V29 (NVL)" },
	{ 0, NULL },
};

static const e1000e_id_t *e1000e_lookup(uint16_t did)
{
	for (const e1000e_id_t *e = e1000e_pci_ids; e->name; e++) {
		if (e->did == did)
			return e;
	}
	return NULL;
}

// Returns 1 if `did` is an I217/I218/I219 PCH-LAN part that requires the
// PCH-LAN / I219-specific bring-up quirks below.  These parts integrate the
// MAC into the PCH and the PHY into the CPU package, so the OS must
// negotiate access with the Management Engine (ME) firmware via the
// SW/FW semaphore and clear several PCH power-saving features (K1 idle,
// EEE, ULP) before TX/RX will actually move bytes on the wire.
static int e1000e_is_pch_lan(uint16_t did)
{
	switch (did) {
	// ICH8/9/10 PCH parts
	case 0x1049:
	case 0x104A:
	case 0x104B:
	case 0x104C:
	case 0x104D:
	case 0x10C4:
	case 0x10C5:
	case 0x1501:
	case 0x10BD:
	case 0x10BF:
	case 0x10CB:
	case 0x10CC:
	case 0x10CD:
	case 0x10CE:
	case 0x10E5:
	case 0x294C:
	case 0x10F5:
	case 0x10C0:
	case 0x10C2:
	case 0x10C3:
	case 0x10DF:
	case 0x10DE:
	case 0x1525:
	case 0x10F0:
	// PCH / PCH2
	case 0x10EA:
	case 0x10EB:
	case 0x10EF:
	case 0x1502:
	case 0x1503:
	// I217 / I218 / I219 (LPT, LPT-LP, SPT, CNP, ICP, CMP, TGP, ADP, RPL, MTP,
	// LNP, ARL, PTP, NVL)
	case 0x153A:
	case 0x153B:
	case 0x155A:
	case 0x1559:
	case 0x15A0:
	case 0x15A1:
	case 0x15A2:
	case 0x15A3:
	case 0x156F:
	case 0x1570:
	case 0x15B7:
	case 0x15B8:
	case 0x15B9:
	case 0x15BB:
	case 0x15BC:
	case 0x15BD:
	case 0x15BE:
	case 0x15DF:
	case 0x15E0:
	case 0x15E1:
	case 0x15E2:
	case 0x15D6:
	case 0x15D7:
	case 0x15D8:
	case 0x15E3:
	case 0x0D4E:
	case 0x0D4F:
	case 0x0D4C:
	case 0x0D4D:
	case 0x0D53:
	case 0x0D55:
	case 0x15F9:
	case 0x15FA:
	case 0x15FB:
	case 0x15FC:
	case 0x15F4:
	case 0x15F5:
	case 0x1A1C:
	case 0x1A1D:
	case 0x1A1E:
	case 0x1A1F:
	case 0x550A:
	case 0x550B:
	case 0x550C:
	case 0x550D:
	case 0x0DC5:
	case 0x0DC6:
	case 0x0DC7:
	case 0x0DC8:
	case 0x550E:
	case 0x550F:
	case 0x5510:
	case 0x5511:
	case 0x57A0:
	case 0x57A1:
	case 0x57B3:
	case 0x57B4:
	case 0x57B5:
	case 0x57B6:
	case 0x57B7:
	case 0x57B8:
	case 0x57B9:
	case 0x57BA:
		return 1;
	default:
		return 0;
	}
}

// MAC generation of an I219 PCH-LAN part, ordered so that later PCH
// generations compare greater.  Several I219 IDs are typed by their MAC
// generation rather than by the PCH they were first paired with: the
// 0x15D6/0x15D7/0x15D8/0x15E3 and CMP 0x0D53/0x0D55 parts are SPT MACs,
// ICP and CMP 0x0D4C-0x0D4F are CNP MACs, RPL parts are ADP MACs, and
// LNP/ARL/PTP/NVL parts are MTP MACs.  Everything that is not an I219
// (8257x, 80003ES2LAN, ICH8-10, PCH, PCH2, LPT) reports E1000E_MAC_PRE_SPT.
enum e1000e_mac_gen {
	E1000E_MAC_PRE_SPT = 0,
	E1000E_MAC_PCH_SPT,
	E1000E_MAC_PCH_CNP,
	E1000E_MAC_PCH_TGP,
	E1000E_MAC_PCH_ADP,
	E1000E_MAC_PCH_MTP,
};

static enum e1000e_mac_gen e1000e_mac_gen(uint16_t did)
{
	switch (did) {
	case 0x156F:
	case 0x1570:
	case 0x15B7:
	case 0x15B8:
	case 0x15B9:
	case 0x15D7:
	case 0x15D8:
	case 0x15E3:
	case 0x15D6:
	case 0x0D53:
	case 0x0D55:
		return E1000E_MAC_PCH_SPT;
	case 0x15BD:
	case 0x15BE:
	case 0x15BB:
	case 0x15BC:
	case 0x15DF:
	case 0x15E0:
	case 0x15E1:
	case 0x15E2:
	case 0x0D4E:
	case 0x0D4F:
	case 0x0D4C:
	case 0x0D4D:
		return E1000E_MAC_PCH_CNP;
	case 0x15FB:
	case 0x15FC:
	case 0x15F9:
	case 0x15FA:
	case 0x15F4:
	case 0x15F5:
		return E1000E_MAC_PCH_TGP;
	case 0x0DC5:
	case 0x0DC6:
	case 0x1A1E:
	case 0x1A1F:
	case 0x1A1C:
	case 0x1A1D:
	case 0x0DC7:
	case 0x0DC8:
	case 0x550C:
	case 0x550D:
		return E1000E_MAC_PCH_ADP;
	case 0x550A:
	case 0x550B:
	case 0x550E:
	case 0x550F:
	case 0x5510:
	case 0x5511:
	case 0x57A0:
	case 0x57A1:
	case 0x57B3:
	case 0x57B4:
	case 0x57B5:
	case 0x57B6:
	case 0x57B7:
	case 0x57B8:
	case 0x57B9:
	case 0x57BA:
		return E1000E_MAC_PCH_MTP;
	default:
		return E1000E_MAC_PRE_SPT;
	}
}

// The parts that get the post-link 10/100 full-duplex K1 / REQ_PLL_CLK
// sequence in e1000e_init().  This is the set it was brought up and
// verified on (ThinkPad P50 SPT I219); it is deliberately an explicit ID
// list and not a MAC-generation range.
static int e1000e_has_10_100_k1_pll_quirk(uint16_t did)
{
	switch (did) {
	case 0x156F:
	case 0x1570:
	case 0x15B7:
	case 0x15B8:
	case 0x15B9:
		return 1;
	default:
		return 0;
	}
}

// Single-NIC global.
static e1000e_dev_t g_e1000e;
int g_e1000e_initialized = 0;
int g_e1000e_legacy_irq = -1;

// ============================================================================
// MMIO helpers
// ============================================================================
static inline uint32_t e1000e_read(e1000e_dev_t *dev, uint32_t reg)
{
	return *(volatile uint32_t *)(dev->mmio_base + reg);
}

static inline void e1000e_write(e1000e_dev_t *dev, uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(dev->mmio_base + reg) = val;
	__asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// I217/I218/I219 (PCH-LAN) register set and bring-up quirks
// ============================================================================
// Registers exclusive to the PCH-LAN / I219 family.  Offsets and bit
// definitions taken from Intel's I218/I219 datasheets and EHL/CNL EDS.
#define E1000_CTRL_EXT 0x00018 // Extended Device Control
#define E1000_FEXTNVM 0x00028
#define E1000_FEXTNVM3 0x0003C
#define E1000_FEXTNVM4 0x00024
#define E1000_FEXTNVM5 0x00014
#define E1000_FEXTNVM6 0x00010
#define E1000_FEXTNVM7 0x000E4
#define E1000_FEXTNVM8 0x05BB0
#define E1000_FEXTNVM9 0x05BB4 // SPT+ Future Extended NVM 9
#define E1000_FEXTNVM11 0x05BBC // SPT/CNP/TGP+ Future Extended NVM 11
#define E1000_PBA 0x01000
#define E1000_PBS 0x01008
#define E1000_PBECCSTS 0x0100C
#define E1000_KUMCTRLSTA 0x00034
#define E1000_FWSM 0x05B54
#define E1000_GCR 0x05B00 // PCIe control
#define E1000_EXTCNF_CTRL 0x00F00
#define E1000_PHY_CTRL 0x00F10
#define E1000_DPGFR 0x00FAC
#define E1000_IOSFPC 0x00F28 // SPT/KBL Tx corruption / hang workaround
#define E1000_IPCNFG 0x00E38 // EEE control
#define E1000_EEER 0x00E30 // Energy-Efficient Ethernet
#define E1000_PCH_LPT_PRA 0x05B58
#define E1000_TARC0 0x03840
#define E1000_TARC1 0x03940
#define E1000_TXDCTL1 0x03928
#define E1000_KABGTXD 0x03004 // AFE Band Gap Transmit Ref Data
#define E1000_KABGTXD_BGSQLBIAS 0x00050000u
#define E1000_TIDV 0x03820
#define E1000_TADV 0x0382C
#define E1000_TDFH 0x03410
#define E1000_TDFT 0x03418
#define E1000_TDFHS 0x03420
#define E1000_TDFTS 0x03428
#define E1000_FCRTL 0x02160 // Flow Control Receive Threshold Low
#define E1000_FCRTH 0x02168 // Flow Control Receive Threshold High
#define E1000_FCTTV 0x00170 // Flow Control Transmit Timer Value
#define E1000_FCAL 0x00028 // Flow Control Address Low
#define E1000_FCAH 0x0002C // Flow Control Address High
#define E1000_FCT 0x00030 // Flow Control Type
#define E1000_MANC 0x05820 // Management Control
#define E1000_H2ME 0x05B50 // Host-to-ME
// NOTE: bit positions match the I218/I219 datasheet exactly.  Earlier we
// had these off by one (ULP at bit 10, ENFORCE_SETTINGS at bit 11), which
// caused the "exit ULP" handshake to actually REQUEST ULP (writing 0x800 ==
// ULP-indication bit), gating the TX descriptor-fetch DMA on real I219-LM
// hardware (TDH never advances, GPTC stays 0, no errors).
#define E1000_H2ME_ULP 0x00000800u // Host requests ULP (bit 11)
#define E1000_H2ME_ENFORCE_SETTINGS \
	0x00001000u // Tell ME: host owns config (bit 12)

// GCR bits controlling PCIe no-snoop attributes for descriptor and buffer DMA.
// On PCH-LAN we bring the device up in normal snooped mode by clearing
// these bits rather than relying on reset state inherited from firmware.
#define E1000_GCR_RXD_NO_SNOOP 0x00000001u
#define E1000_GCR_RXDSCW_NO_SNOOP 0x00000002u
#define E1000_GCR_RXDSCR_NO_SNOOP 0x00000004u
#define E1000_GCR_TXD_NO_SNOOP 0x00000008u
#define E1000_GCR_TXDSCW_NO_SNOOP 0x00000010u
#define E1000_GCR_TXDSCR_NO_SNOOP 0x00000020u
#define E1000_GCR_NO_SNOOP_ALL                                \
	(E1000_GCR_RXD_NO_SNOOP | E1000_GCR_RXDSCW_NO_SNOOP | \
	 E1000_GCR_RXDSCR_NO_SNOOP | E1000_GCR_TXD_NO_SNOOP | \
	 E1000_GCR_TXDSCW_NO_SNOOP | E1000_GCR_TXDSCR_NO_SNOOP)

// CTRL_EXT bits
#define E1000_CTRL_EXT_LSECCK (1u << 5)
#define E1000_CTRL_EXT_DRV_LOAD (1u << 28)
#define E1000_CTRL_EXT_FORCE_SMBUS (1u << 11)
#define E1000_CTRL_EXT_DPG_EN (1u << 3)
#define E1000_CTRL_EXT_DMA_DYN_CLK_EN (1u << 19)
#define E1000_CTRL_EXT_LPCD (1u << 24)
#define E1000_CTRL_EXT_RO_DIS (1u << 17)

// STATUS bits (PCH)
#define E1000_STATUS_LAN_INIT_DONE (1u << 9)
#define E1000_STATUS_PHYRA (1u << 10)

#define E1000_ICH8_LAN_INIT_TIMEOUT 1500

// MDIC register (MAC-mediated MDIO PHY access)
#define E1000_MDIC 0x00020
#define E1000_MDIC_DATA_MASK 0x0000FFFFu
#define E1000_MDIC_REG_SHIFT 16
#define E1000_MDIC_PHY_SHIFT 21
#define E1000_MDIC_OP_WRITE (1u << 26)
#define E1000_MDIC_OP_READ (2u << 26)
#define E1000_MDIC_READY (1u << 28)
#define E1000_MDIC_INT_EN (1u << 29)
#define E1000_MDIC_ERROR (1u << 30)

#define E1000E_MAX_PHY_REG_ADDRESS 0x1Fu
#define E1000E_IGP_PAGE_SHIFT 5
#define E1000E_IGP_PHY_PAGE_SELECT 0x1Fu
#define E1000E_PHY_REG(page, reg)            \
	(((page) << E1000E_IGP_PAGE_SHIFT) | \
	 ((reg) & E1000E_MAX_PHY_REG_ADDRESS))

// PHY MMD register addresses on PCH-LAN.  On I219 the integrated PHY
// sits at MDIO address 2 (verified empirically on real hardware).
#define E1000E_PHY_ADDR 2
#define PHY_CONTROL_REG 0 // MII BMCR
/* The I219 spreads its registers over two MDIO addresses (datasheet §9.3,
 * table 9-1): the general, PAGED registers at address 1 -- every page from
 * 768 up, page 768 being that address's page 0 (OEM bits, SMBus address,
 * ...) -- and the PHY-specific ones at address 2, page 0.  The threshold
 * was 778, which sent pages 768..777 -- the OEM bits, the SMBus control at
 * 769, the LPI registers at 772 -- to address 2, where those register
 * numbers are the interrupt status, PHY Control and PHY Configuration
 * registers.  See e1000e_phy_paged_access(). */
#define E1000E_HV_INTC_FC_PAGE_START 768

// Paged HV PHY registers used by the PCH-LAN SMBus / ULP handoff.
#define E1000_CV_SMB_CTRL E1000E_PHY_REG(769, 23)
#define E1000_CV_SMB_CTRL_FORCE_SMBUS 0x0001
#define E1000_I218_ULP_CONFIG1 E1000E_PHY_REG(779, 16)
#define E1000_I218_ULP_CONFIG1_START 0x0001
#define E1000_I218_ULP_CONFIG1_IND 0x0004
#define E1000_I218_ULP_CONFIG1_STICKY_ULP 0x0010
#define E1000_I218_ULP_CONFIG1_INBAND_EXIT 0x0020
#define E1000_I218_ULP_CONFIG1_WOL_HOST 0x0040
#define E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS 0x0100
#define E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC 0x0400
#define E1000_I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST 0x0800
#define E1000_I218_ULP_CONFIG1_DISABLE_SMB_PERST 0x1000

// Intel I82577/I82578/I82579/I217/I218/I219 integrated-PHY copper setup
// register.  Page 0 register 22.  These bits configure the PHY's copper
// front-end before autonegotiation runs.  Per the I82577 datasheet, both
// bits must be set unconditionally before autoneg restart.
//
//   I82577_CFG_ASSERT_CRS_ON_TX (bit 15)
//     Force the PHY to assert internal carrier-sense whenever the MAC
//     is transmitting.  Without this the MAC's TX state machine on
//     PCH-LAN parts can lose CRS during a frame and abort the
//     transmission, presenting as TXQE with TDH stuck and GPTC=0 — our
//     exact symptom.
//
//   I82577_CFG_ENABLE_DOWNSHIFT (bits 11:10 = 0b11 = 3 retries)
//     Configure auto-downshift to 3 retries.  The NVM default on OEM
//     ThinkPad parts is 0 retries (= immediate downshift after a single
//     failed 100Mb training pulse), which causes the PHY to resolve at
//     10 Mb/s on its very first link attempt even when the partner
//     advertises 100FD with autoneg ACK — the *exact* symptom we see
//     in dmesg (LPAR=0xC1E1 with 100FD+Ack but BMCR resolves to 10FD).
#define I82577_CFG_REG 22
#define I82577_CFG_ASSERT_CRS_ON_TX 0x8000
#define I82577_CFG_ENABLE_DOWNSHIFT 0x0C00

// I82577 PHY Status Register 2 (page 0 reg 26): ground-truth resolved
// speed/duplex from the PHY itself.  Trusting BMCR after autoneg is
// unsafe because IEEE 802.3 leaves BMCR.SPEED bits undefined when
// AUTONEG_EN=1; only this register is authoritative.
#define I82577_PHY_STATUS_2 26
#define I82577_PHY_STATUS2_REV_POLARITY 0x0400
#define I82577_PHY_STATUS2_MDIX 0x0800
#define I82577_PHY_STATUS2_SPEED_MASK 0x0300
#define I82577_PHY_STATUS2_SPEED_1000MBPS 0x0200
#define I82577_PHY_STATUS2_SPEED_100MBPS 0x0100

// EMI (Extended MDI) interface for paged HV PHY registers used by the
// EEE block on PCH-LAN (I217/I218/I219).  EEE settings live in the
// "EMI" address space accessed via two paged PHY registers:
//   * I82579_EMI_ADDR — write the EMI register address here
//   * I82579_EMI_DATA — read/write the EMI data through this window
// The real EMI registers we need for EEE-disable are:
//   * I217_EEE_ADVERTISEMENT (0x040E) — local EEE ability  (must be 0)
//   * I217_EEE_LP_ABILITY    (0x040F) — link-partner EEE   (read-only)
//   * I217_EEE_PCS_STATUS    (0x06E8 on I218, 0x06E9 on I217) — PCS state
// The EMI window is PHY address 2, page 0, registers 16 (address) and 17
// (data) -- I219 datasheet §9.5.12.1.  It used to be addressed as page 776
// registers 22/23, which on address 2 are PHY Configuration and PHY
// Control: the "EEE disable" wrote the EMI address into the first and 0
// into the second, clearing LNK_EN and dropping the link.  On the I217
// and later, the EEE advertisement is EMI 0x8001 (IEEE MMD 7.60) and the
// partner's 0x8002 (7.61); 0x040E/0x040F were the 82579's.
#define I82579_EMI_ADDR E1000E_PHY_REG(0, 16)
#define I82579_EMI_DATA E1000E_PHY_REG(0, 17)
#define I217_EEE_ADVERTISEMENT 0x8001
#define I217_EEE_LP_ABILITY 0x8002

// HV LPI Control register (paged): bit 7 enables 100Mb LPI auto-shutoff,
// bit 16 (across the 32-bit MAC mirror) enables 1000Mb LPI.  On the
// PHY side it lives at page 776 reg 16 and we just clear it entirely.
#define I218_LPI_CTRL E1000E_PHY_REG(776, 18)

// RX statistics counters (read-to-clear)
#define E1000_CRCERRS 0x04000
#define E1000_RXERRC 0x0400C
#define E1000_MPC 0x04010
#define E1000_GPRC 0x04074
#define E1000_RNBC 0x040A0
#define E1000_TPR 0x040D0
#define E1000_GPTC 0x04080
#define E1000_TPT 0x040D4

// EXTCNF_CTRL bits
#define E1000_EXTCNF_CTRL_SWFLAG (1u << 5)
#define E1000_EXTCNF_CTRL_OEM_WRITE_ENABLE (1u << 3)

// FEXTNVM6: K1 power-save controls (SPT/CNP/ICP)
#define E1000_FEXTNVM6_K1_OFF_ENABLE (1u << 24)
#define E1000_FEXTNVM6_REQ_PLL_CLK (1u << 8)
#define E1000_FEXTNVM6_ENABLE_K1_ENTRY_CONDITION (1u << 1)

// FEXTNVM7
#define E1000_FEXTNVM7_SIDE_CLK_UNGATE (1u << 2)
#define E1000_FEXTNVM7_DISABLE_PB_READ (1u << 18)
#define E1000_FEXTNVM7_DISABLE_SMB_PERST (1u << 5)
#define E1000_FEXTNVM7_DISABLE_TSYNC_CLK (1u << 31)
#define E1000_FEXTNVM7_ENABLE_TSYNC_INTR (1u << 0)

#define E1000_PBECCSTS_ECC_ENABLE 0x00010000u
#define E1000_CTRL_MEHE 0x00080000u

// CTRL bits used for the LANPHYPC handoff on SPT/CNP/TGP+ (I218/I219).
// On these PCH-LAN parts the integrated PHY's clock and power gate is
// controlled by the Management Engine by default.  The driver MUST take
// ownership by toggling LANPHYPC_OVERRIDE / LANPHYPC_VALUE — otherwise
// the PHY analog frontend stays gated even though MDIO appears to work
// (the link LED can stay lit because the ME has its own link), and our
// BMCR autoneg restart never actually drives FLPs onto the wire.
//   E1000_CTRL_LANPHYPC_OVERRIDE = 0x00010000  (bit 16, SW takes control)
//   E1000_CTRL_LANPHYPC_VALUE    = 0x00020000  (bit 17, value while OVRD)
#define E1000_CTRL_LANPHYPC_OVERRIDE 0x00010000u
#define E1000_CTRL_LANPHYPC_VALUE 0x00020000u

// FEXTNVM9 / FEXTNVM11 quirks (CMP / TGP)
#define E1000_FEXTNVM9_IOSFSB_CLKGATE_DIS (1u << 11)
#define E1000_FEXTNVM9_IOSFSB_CLKREQ_DIS (1u << 12)
#define E1000_FEXTNVM11_DISABLE_MULR_FIX 0x00002000u
#define E1000_TARC0_CB_MULTIQ_3_REQ 0x30000000u
#define E1000_TARC0_CB_MULTIQ_2_REQ 0x20000000u
#define E1000_TXDCTL_PTHRESH 0x0000003Fu
#define E1000_TXDCTL_HTHRESH 0x00003F00u
#define E1000_TXDCTL_WTHRESH 0x003F0000u
#define E1000_TXDCTL_GRAN 0x01000000u
#define E1000_TXDCTL_COUNT_DESC 0x00400000u

// PHY_CTRL bits
#define E1000_PHY_CTRL_GBE_DISABLE (1u << 6)
#define E1000_PHY_CTRL_NOND0A_LPLU (1u << 3)
#define E1000_PHY_CTRL_D0A_LPLU (1u << 2)

// Per-link-up configuration of the SPT and later PCH-LAN MACs/PHYs.
#define E1000_PCIEANACFG 0x00F18 // PCIe analog config (K1-off mirror)
#define E1000_LTRV 0x000F8 // Latency Tolerance Reporting value
#define E1000_LTRV_REQ_SHIFT 15
#define E1000_LTRV_NOSNOOP_SHIFT 16
#define E1000_LTRV_SEND (1u << 30)
#define E1000_PCI_LTR_CAP_LPT 0xA8 // platform max snoop/no-snoop latency
#define E1000_FCRTV_PCH 0x05F40 // flow control refresh timer
#define E1000_FCRTL_XONE 0x80000000u // send XON below the low mark
#define E1000_CTRL_SPD_100 (1u << 8)
#define E1000_CTRL_SPD_1000 (1u << 9)
#define E1000_CTRL_RFCE (1u << 27) // honour received pause frames
#define E1000_CTRL_TFCE (1u << 28) // send pause frames
#define E1000_TIPG_IPGT_MASK 0x3FFu
#define E1000_RFCTL_NFSW_DIS (1u << 6)
#define E1000_RFCTL_NFSR_DIS (1u << 7)
#define I217_RX_CONFIG 0xB20C // EMI: analog RX latency
#define I217_PLL_CLOCK_GATE_REG E1000E_PHY_REG(772, 28)
#define I217_PLL_CLOCK_GATE_MASK 0x07FFu
#define HV_PM_CTRL E1000E_PHY_REG(770, 17)
#define HV_PM_CTRL_K1_CLK_REQ 0x0200u
#define I219_PTR_GAP_REG E1000E_PHY_REG(776, 20) // MAC/PHY pointer gap
#define BM_PORT_CTRL_PAUSE E1000E_PHY_REG(769, 27) // PHY-side pause time
// Flow control on SPT and later with the standard MTU and PBA 26 KB.
#define E1000E_PCH_FC_HIGH_WATER 0x05C20u
#define E1000E_PCH_FC_LOW_WATER 0x05048u
#define E1000E_FC_PAUSE_TIME 0xFFFFu
#define E1000E_MAX_FRAME 1518u // 1500 + Ethernet header + FCS

// PMA timer-based microsecond delay forward decl
static void e1000e_delay_us(uint32_t us);

static void __attribute__((unused)) e1000e_dump_pci_dma_state(e1000e_dev_t *dev,
							      uint32_t tx_seq)
{
#if E1000E_DBG
	if (!dev || !dev->pci_dev) {
		return;
	}

	const pci_device_t *pdev = dev->pci_dev;
	uint32_t cmdsts =
		pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
	uint16_t cmd = (uint16_t)(cmdsts & 0xFFFFu);
	uint16_t sts = (uint16_t)(cmdsts >> 16);
	uint8_t pcie_cap = pci_find_capability(pdev, 0x10);

	e1000e_dbg(
		"E1000E: tx#%u PCI CMD=%04x STS=%04x BUSM=%u MEM=%u MABRT=%u TABRT_R=%u TABRT_S=%u DPERR=%u SERR=%u\n",
		tx_seq, cmd, sts, (cmd >> 2) & 1, (cmd >> 1) & 1,
		(sts >> 13) & 1, (sts >> 12) & 1, (sts >> 11) & 1,
		(sts >> 15) & 1, (sts >> 14) & 1);

	if (pcie_cap) {
		uint32_t devctlsts =
			pci_cfg_read32(pdev->bus, pdev->device, pdev->function,
				       pcie_cap + 0x08);
		uint32_t lnksts =
			pci_cfg_read32(pdev->bus, pdev->device, pdev->function,
				       pcie_cap + 0x10);
		uint16_t devsta = (uint16_t)(devctlsts >> 16);
		uint16_t linksta = (uint16_t)(lnksts >> 16);

		e1000e_dbg(
			"E1000E: tx#%u PCIe DEVSTA=%04x CORR=%u NFAT=%u FAT=%u UR=%u AUX=%u TPEND=%u LNKSTA=%04x speed=%u width=x%u\n",
			tx_seq, devsta, (devsta >> 0) & 1, (devsta >> 1) & 1,
			(devsta >> 2) & 1, (devsta >> 3) & 1, (devsta >> 4) & 1,
			(devsta >> 5) & 1, linksta, linksta & 0xF,
			(linksta >> 4) & 0x3F);
	} else {
		// The I219 is an LPC-attached integrated MAC and does not expose a
		// PCIe Express capability (Cap ID 0x10) in standard config space.
		// Nothing to do here.
		e1000e_dbg(
			"E1000E: tx#%u no PCIe Express cap (expected on I219 LPC-integrated MAC)\n",
			tx_seq);
	}
#else
	(void)dev;
	(void)tx_seq;
#endif
}

/* The software/firmware semaphore for the PHY (EXTCNF_CTRL.SWFLAG), as
 * every PCH-LAN driver takes it: wait up to 50 ms for the flag to be FREE
 * -- the Management Engine holds it while it uses the PHY itself -- then
 * request it and wait up to a second for the hardware to grant it.  This
 * used to clear a flag it found set, i.e. take the PHY away from the ME in
 * the middle of whatever the ME was doing with it, and to give up after
 * 100 ms of asking.
 *
 * It also takes dev->phy_busy, the lock that keeps the CPUs off the MDIC
 * register at the same time: the link watchdog, the transmit path's link
 * poll, initialisation and shutdown all talk to the PHY.  Every PHY access
 * after initialisation goes through here.  phy_busy is a plain spin flag,
 * not an interrupts-off lock -- the holders busy-wait for milliseconds on
 * the MDIO bus -- and it is never taken from hard-IRQ context. */
#define E1000E_SWFLAG_FREE_MS 50
#define E1000E_SWFLAG_GRANT_MS 1000

static int e1000e_acquire_swflag(e1000e_dev_t *dev)
{
	uint32_t ext;
	int i;

	while (__atomic_exchange_n(&dev->phy_busy, 1, __ATOMIC_ACQUIRE))
		__asm__ volatile("pause" ::: "memory");

	for (i = 0; i < E1000E_SWFLAG_FREE_MS; i++) {
		ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
		if (!(ext & E1000_EXTCNF_CTRL_SWFLAG))
			break;
		e1000e_delay_us(1000);
	}
	if (i == E1000E_SWFLAG_FREE_MS) {
		e1000e_dbg("E1000E: SWFLAG held by firmware (EXTCNF=%08x)\n",
			   ext);
		__atomic_store_n(&dev->phy_busy, 0, __ATOMIC_RELEASE);
		return -1;
	}

	e1000e_write(dev, E1000_EXTCNF_CTRL, ext | E1000_EXTCNF_CTRL_SWFLAG);
	for (i = 0; i < E1000E_SWFLAG_GRANT_MS; i++) {
		ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
		if (ext & E1000_EXTCNF_CTRL_SWFLAG) {
			dev->swflag_held = 1;
			return 0;
		}
		e1000e_delay_us(1000);
	}
	// Not granted: withdraw the request.
	e1000e_write(dev, E1000_EXTCNF_CTRL, ext & ~E1000_EXTCNF_CTRL_SWFLAG);
	e1000e_dbg("E1000E: SWFLAG not granted (FWSM=%08x)\n",
		   e1000e_read(dev, E1000_FWSM));
	__atomic_store_n(&dev->phy_busy, 0, __ATOMIC_RELEASE);
	return -1;
}

/* Release what e1000e_acquire_swflag() took.  A no-op when it failed: the
 * flag then belongs to the firmware and must not be cleared. */
static void e1000e_release_swflag(e1000e_dev_t *dev)
{
	if (!dev->swflag_held)
		return;
	uint32_t ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
	e1000e_write(dev, E1000_EXTCNF_CTRL, ext & ~E1000_EXTCNF_CTRL_SWFLAG);
	dev->swflag_held = 0;
	__atomic_store_n(&dev->phy_busy, 0, __ATOMIC_RELEASE);
}

/* MDIC transactions are retried on MTP-class MACs and later (up to two
 * retries, 10 ms apart), whose PHY interface is known to fail the odd
 * transaction; a transaction there also only counts when the MAC echoes
 * the register number that was asked for. */
static int e1000e_mdic_retries(const e1000e_dev_t *dev)
{
	return e1000e_mac_gen(dev->device_id) >= E1000E_MAC_PCH_MTP ? 2 : 0;
}

/* One MDIC transaction: `mdic' is the command word.  Returns the final
 * MDIC value with READY set, or 0 on failure. */
static uint32_t e1000e_mdic_xfer(e1000e_dev_t *dev, uint32_t mdic, uint8_t reg)
{
	int retries = e1000e_mdic_retries(dev);

	for (int attempt = 0; attempt <= retries; attempt++) {
		uint32_t v = 0;
		int ok = 0;

		e1000e_write(dev, E1000_MDIC, mdic);
		// Spec: completes within ~64 us on a 25 MHz MDIO; allow up to 5 ms.
		for (int i = 0; i < 100; i++) {
			e1000e_delay_us(50);
			v = e1000e_read(dev, E1000_MDIC);
			if (v & E1000_MDIC_READY) {
				ok = 1;
				break;
			}
		}
		if (ok && (v & E1000_MDIC_ERROR))
			ok = 0;
		if (ok && retries &&
		    ((v >> E1000_MDIC_REG_SHIFT) & E1000E_MAX_PHY_REG_ADDRESS) !=
			    reg)
			ok = 0;
		if (ok)
			return v;
		if (attempt < retries)
			e1000e_delay_us(10000);
	}
	return 0;
}

// Minimal MDIO-via-MAC PHY register read.  Used on PCH-LAN parts to
// acknowledge the PHY after reset: until software performs at least one
// MDIO transaction, the PHY keeps STATUS.PHYRA asserted and gates its
// data interface to the MAC, so neither RX nor TX frames flow even
// though L1 link is up.
//
// Returns 0 on success and stores the 16-bit PHY register value in
// *val; returns -1 on timeout or MDIC.ERROR.
static int e1000e_phy_read_at(e1000e_dev_t *dev, uint8_t phy_addr, uint8_t reg,
			      uint16_t *val)
{
	uint32_t mdic = ((uint32_t)reg << E1000_MDIC_REG_SHIFT) |
			((uint32_t)phy_addr << E1000_MDIC_PHY_SHIFT) |
			E1000_MDIC_OP_READ;
	uint32_t v = e1000e_mdic_xfer(dev, mdic, reg);

	if (!v)
		return -1;
	if (val)
		*val = (uint16_t)(v & E1000_MDIC_DATA_MASK);
	return 0;
}

static int e1000e_phy_write_at(e1000e_dev_t *dev, uint8_t phy_addr, uint8_t reg,
			       uint16_t val)
{
	uint32_t mdic = ((uint32_t)val & E1000_MDIC_DATA_MASK) |
			((uint32_t)reg << E1000_MDIC_REG_SHIFT) |
			((uint32_t)phy_addr << E1000_MDIC_PHY_SHIFT) |
			E1000_MDIC_OP_WRITE;

	return e1000e_mdic_xfer(dev, mdic, reg) ? 0 : -1;
}

static uint8_t e1000e_phy_addr_for_hv_page(uint16_t page)
{
	return (page >= E1000E_HV_INTC_FC_PAGE_START) ? 1 : E1000E_PHY_ADDR;
}

/* A paged PHY register, E1000E_PHY_REG(page, reg), as the I219 datasheet
 * lays them out (§9.3): pages 768 and up live at MDIO address 1, page 768
 * being that address's page 0; page 0 is address 2.  The page is selected
 * by writing page * 32 to register 31 before touching a register above
 * 15 (0-15 are the IEEE registers, identical on every page).  Pages 1..767
 * are the 82577/82578 debug window, which nothing here uses; asking for one
 * is a bug.  Caller holds the PHY (e1000e_acquire_swflag). */
static int e1000e_phy_paged_access(e1000e_dev_t *dev, uint32_t offset,
				   uint16_t *val, int write)
{
	uint16_t page = (uint16_t)(offset >> E1000E_IGP_PAGE_SHIFT);
	uint8_t reg = (uint8_t)(offset & E1000E_MAX_PHY_REG_ADDRESS);
	uint8_t phy_addr = e1000e_phy_addr_for_hv_page(page);

	if (WARN_ON_ONCE(page > 0 && page < E1000E_HV_INTC_FC_PAGE_START))
		return -1;
	if (page == E1000E_HV_INTC_FC_PAGE_START)
		page = 0;
	if (reg > 15 &&
	    e1000e_phy_write_at(dev, phy_addr, E1000E_IGP_PHY_PAGE_SELECT,
				(uint16_t)(page << E1000E_IGP_PAGE_SHIFT)) != 0)
		return -1;
	if (write)
		return e1000e_phy_write_at(dev, phy_addr, reg, *val);
	return e1000e_phy_read_at(dev, phy_addr, reg, val);
}

static int e1000e_phy_read_paged_locked(e1000e_dev_t *dev, uint32_t offset,
					uint16_t *val)
{
	uint16_t tmp = 0;
	int rc = e1000e_phy_paged_access(dev, offset, &tmp, 0);

	if (rc == 0 && val)
		*val = tmp;
	return rc;
}

static int e1000e_phy_write_paged_locked(e1000e_dev_t *dev, uint32_t offset,
					 uint16_t val)
{
	return e1000e_phy_paged_access(dev, offset, &val, 1);
}

static void e1000e_force_exit_phy_ulp(e1000e_dev_t *dev)
{
	if (e1000e_acquire_swflag(dev) != 0) {
		e1000e_dbg("E1000E: ULP exit: could not acquire SWFLAG\n");
		return;
	}

	uint32_t ctrl_ext_restore = e1000e_read(dev, E1000_CTRL_EXT);
	uint16_t smb_before = 0, smb_after = 0;
	uint16_t ulp_before = 0, ulp_after = 0;
	int smb_rc = e1000e_phy_read_paged_locked(dev, E1000_CV_SMB_CTRL,
						  &smb_before);

	if (smb_rc != 0) {
		uint32_t ctrl_ext =
			ctrl_ext_restore | E1000_CTRL_EXT_FORCE_SMBUS;
		e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);
		e1000e_delay_us(50000);
		smb_rc = e1000e_phy_read_paged_locked(dev, E1000_CV_SMB_CTRL,
						      &smb_before);
	}

	if (smb_rc == 0) {
		uint16_t smb_ctrl =
			(uint16_t)(smb_before & ~E1000_CV_SMB_CTRL_FORCE_SMBUS);
		if (e1000e_phy_write_paged_locked(dev, E1000_CV_SMB_CTRL,
						  smb_ctrl) == 0) {
			(void)e1000e_phy_read_paged_locked(
				dev, E1000_CV_SMB_CTRL, &smb_after);
		}
	}

	if (e1000e_phy_read_paged_locked(dev, E1000_I218_ULP_CONFIG1,
					 &ulp_before) == 0) {
		uint16_t ulp_cfg = ulp_before;
		ulp_cfg &= ~(E1000_I218_ULP_CONFIG1_IND |
			     E1000_I218_ULP_CONFIG1_STICKY_ULP |
			     E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS |
			     E1000_I218_ULP_CONFIG1_WOL_HOST |
			     E1000_I218_ULP_CONFIG1_INBAND_EXIT |
			     E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC |
			     E1000_I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST |
			     E1000_I218_ULP_CONFIG1_DISABLE_SMB_PERST);

		if (e1000e_phy_write_paged_locked(dev, E1000_I218_ULP_CONFIG1,
						  ulp_cfg) == 0) {
			(void)e1000e_phy_write_paged_locked(
				dev, E1000_I218_ULP_CONFIG1,
				(uint16_t)(ulp_cfg |
					   E1000_I218_ULP_CONFIG1_START));
			(void)e1000e_phy_read_paged_locked(
				dev, E1000_I218_ULP_CONFIG1, &ulp_after);
		}
	}

	e1000e_write(dev, E1000_CTRL_EXT,
		     ctrl_ext_restore & ~E1000_CTRL_EXT_FORCE_SMBUS);

#if E1000E_DBG
	uint32_t fextnvm7_before = e1000e_read(dev, E1000_FEXTNVM7);
	e1000e_write(dev, E1000_FEXTNVM7,
		     fextnvm7_before & ~E1000_FEXTNVM7_DISABLE_SMB_PERST);
	uint32_t fextnvm7_after = e1000e_read(dev, E1000_FEXTNVM7);
#else
	{
		uint32_t fextnvm7_tmp = e1000e_read(dev, E1000_FEXTNVM7);
		e1000e_write(dev, E1000_FEXTNVM7,
			     fextnvm7_tmp & ~E1000_FEXTNVM7_DISABLE_SMB_PERST);
	}
#endif

	e1000e_release_swflag(dev);

	e1000e_dbg(
		"E1000E: PHY SMB/ULP exit CV_SMB_CTRL=%04x->%04x ULP_CFG1=%04x->%04x FEXTNVM7=%08x->%08x\n",
		smb_before, smb_after, ulp_before, ulp_after, fextnvm7_before,
		fextnvm7_after);
}

static int __attribute__((unused)) e1000e_phy_read(e1000e_dev_t *dev,
						   uint8_t reg, uint16_t *val)
{
	return e1000e_phy_read_at(dev, E1000E_PHY_ADDR, reg, val);
}

// MII register addresses we touch directly.
#define MII_BMCR 0x00
#define MII_BMSR 0x01
#define MII_ANAR 0x04
#define MII_GBCR 0x09 // 1000BASE-T Control

#define MII_BMCR_RESET 0x8000
#define MII_BMCR_LOOPBACK 0x4000
#define MII_BMCR_SPEED_LSB 0x2000 // 100
#define MII_BMCR_AUTONEG_EN 0x1000
#define MII_BMCR_PWR_DOWN 0x0800
#define MII_BMCR_ISOLATE 0x0400
#define MII_BMCR_AUTONEG_RESTART 0x0200
#define MII_BMCR_DUPLEX_FD 0x0100
#define MII_BMCR_SPEED_MSB 0x0040

#define MII_BMSR_LSTATUS 0x0004
#define MII_BMSR_AN_COMPLETE 0x0020

// IEEE 802.3 ANAR fields: selector=00001 (802.3 CSMA/CD), advertise
// 10HD|10FD|100HD|100FD, and pause symmetric+asymmetric so the link
// partner knows we accept its pause frames (IEEE 802.3 defaults).
#define MII_ANAR_FULL_RANGE 0x05E1 // sel + 10/100 HD/FD + asym/sym pause

// 1000BASE-T Control: advertise 1000FD only (most modern partners
// reject 1000HD outright).
#define MII_GBCR_ADV_1000FD 0x0200
#define MII_GBCR_ADV_1000HD 0x0100

// Intel HV (Hidden-Vendor) PHY OEM control register.  On I217/I218/I219
// this is at PHY page 0, register 25 — i.e. plain MDIO register 0x19,
// no page select needed.  The NVM default for OEM-branded laptops
// (including ThinkPad P50) leaves LPLU set, which forces the PHY to
// auto-negotiate only up to 10 Mb/s when the host has not explicitly
// taken ownership.  We must therefore unconditionally clear LPLU and
// GBE_DIS and set RESTART_AN; without this we get exactly our symptom:
// BMSR.AN_COMPLETE=1, LSTATUS=1, but BMCR result bits show 10 Mb/s and
// MAC STATUS.SPEED reads 00.
// HV_OEM_BITS is PHY ADDRESS 1, page 0, register 25 (I219 datasheet
// §9.5.8.2), written PHY_REG(768, 25): page 768 is address 1's page 0.
// Raw register 0x19 on address 2 -- what this was first read as -- is the
// Interrupt Status register, which reads 0 and ignores writes:
//     E1000E: HV_OEM_BITS 0 -> 0 (... RESTART_AN set)
// The later PHY_REG(769, 25), with the page threshold at 778, still went
// to address 2 register 25, so the bits were never actually cleared.
#define HV_OEM_BITS E1000E_PHY_REG(768, 25)
#define HV_OEM_BITS_LPLU 0x0004 // Low-Power Link-Up enable
#define HV_OEM_BITS_A1KDIS 0x0008 // 1Gb idle disable
#define HV_OEM_BITS_GBE_DIS 0x0040 // Gigabit ethernet disable
#define HV_OEM_BITS_RESTART_AN 0x0400 // Restart auto-negotiation

// Clear the MAC-side mirror of the PHY power-control bits.  PHY_CTRL is
// the MAC's view of the LPLU / GBE_DIS / SMBus signals it forwards to
// the integrated PHY; if D0A_LPLU stays set after reset the PHY will
// honor it and refuse to negotiate above 10 Mb/s even when we clear the
// PHY-side HV_OEM_BITS register.
static void e1000e_clear_mac_phy_ctrl_lplu(e1000e_dev_t *dev)
{
	uint32_t before = e1000e_read(dev, E1000_PHY_CTRL);
	uint32_t after = before & ~(E1000_PHY_CTRL_GBE_DISABLE |
				    E1000_PHY_CTRL_NOND0A_LPLU |
				    E1000_PHY_CTRL_D0A_LPLU);
	e1000e_write(dev, E1000_PHY_CTRL, after);
	e1000e_dbg("E1000E: MAC PHY_CTRL %08x -> %08x (LPLU/GBE_DIS cleared)\n",
		   before, e1000e_read(dev, E1000_PHY_CTRL));
}

// Clear the PHY-side LPLU and GBE_DIS bits and request an autoneg
// restart.  Required on every OEM-branded PCH-LAN part where the NVM
// ships LPLU=1 by default.
//
// CRITICAL: HV_OEM_BITS is paged (page 769 reg 25) AND its write requires
// the SW_FLAG semaphore + EXTCNF_CTRL.OEM_WRITE_ENABLE set before the
// write so the MAC routes the MDIO transaction to the OEM page rather
// than dropping it (per I218/I219 datasheet).
// `restart_an`: when nonzero, also set HV_OEM_BITS_RESTART_AN to force the
// PHY to re-run autoneg.  When zero (BIOS-already-up path), we leave the
// existing link intact and only clear the LPLU/GBE_DIS bits in place --
// this lifts the MII data-path throttle without dropping the live link.
//
// Empirically required on Lenovo P50 / I219-LM: the BIOS leaves the PHY
// in HV_OEM_BITS=0x544 (LPLU=1, GBE_DIS=1, RESTART_AN=1) post-handoff.
// LPLU=1 makes the PHY accept link pulses but stall the data path on
// MII -- the MAC fetches the descriptor (TDFH advances) but never pulls
// the buffer payload, so TDH stays 0 and GPTC stays 0.  Symptom is
// indistinguishable from a TX-arbiter gate.
static int e1000e_oem_bits_clear_lplu_ex(e1000e_dev_t *dev, int restart_an)
{
	if (e1000e_acquire_swflag(dev) != 0) {
		e1000e_dbg("E1000E: HV_OEM_BITS: SWFLAG acquire failed\n");
		return -1;
	}

	// Set OEM_WRITE_ENABLE so the upcoming write to page 769 reg 25
	// is honored by the MAC's PHY-write filter.
	uint32_t ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
	e1000e_write(dev, E1000_EXTCNF_CTRL,
		     ext | E1000_EXTCNF_CTRL_OEM_WRITE_ENABLE);

	uint16_t before = 0xFFFF, after = 0xFFFF;
	int rc = -1;
	if (e1000e_phy_read_paged_locked(dev, HV_OEM_BITS, &before) == 0) {
		uint16_t v = before;
		v &= ~(HV_OEM_BITS_LPLU | HV_OEM_BITS_GBE_DIS);
		if (restart_an)
			v |= HV_OEM_BITS_RESTART_AN;
		else
			v &= ~HV_OEM_BITS_RESTART_AN;
		if (e1000e_phy_write_paged_locked(dev, HV_OEM_BITS, v) == 0) {
			// Spec: wait at least 1 ms after writing HV_OEM_BITS before
			// next PHY register access so the PHY internal state machine
			// restarts cleanly.
			e1000e_delay_us(2000);
			(void)e1000e_phy_read_paged_locked(dev, HV_OEM_BITS,
							   &after);
			rc = 0;
		} else {
			e1000e_dbg("E1000E: HV_OEM_BITS write failed\n");
		}
	} else {
		e1000e_dbg("E1000E: HV_OEM_BITS read failed\n");
	}

	// Clear OEM_WRITE_ENABLE and release SW_FLAG.
	ext = e1000e_read(dev, E1000_EXTCNF_CTRL);
	e1000e_write(dev, E1000_EXTCNF_CTRL,
		     ext & ~E1000_EXTCNF_CTRL_OEM_WRITE_ENABLE);
	e1000e_release_swflag(dev);

	e1000e_dbg(
		"E1000E: HV_OEM_BITS(768.25) %04x -> %04x (LPLU/GBE_DIS cleared, RESTART_AN=%d)\n",
		before, after, restart_an ? 1 : 0);
	return rc;
}

// Backwards-compatible wrapper: original callers always wanted the
// autoneg-restart variant.
static int e1000e_oem_bits_clear_lplu(e1000e_dev_t *dev)
{
	return e1000e_oem_bits_clear_lplu_ex(dev, 1);
}

// Disable Energy-Efficient Ethernet (EEE / 802.3az LPI) before autoneg
// restart.  On OEM-branded NVMs (ThinkPad P50 included) the PHY's EEE
// advertisement is enabled by default and is exchanged via the autoneg
// Next-Page mechanism (LPAR.NP=1 in the dmesg confirms partner accepted
// our NP).  When EEE NextPage exchange fails or is incompatible with
// the link partner the PHY's HCD selection FSM falls back to the
// lowest common ability — exactly 10 Mb/s FD — instead of the IEEE
// 802.3 mandated highest common denominator.  Writing 0 to the PHY's
// EEE_ADVERTISEMENT register before autoneg avoids this misnegotiation.
//
// We do the same here, plus zero the MAC-side EEER/IPCNFG so the MAC
// doesn't re-enable EEE LPI on the internal MII bus after link.
static void e1000e_disable_eee(e1000e_dev_t *dev)
{
	// 1. MAC-side disable: clear EEER (LPI enables, FRC_AN) and IPCNFG.
#if E1000E_DBG
	uint32_t eeer_before = e1000e_read(dev, E1000_EEER);
	uint32_t ipc_before = e1000e_read(dev, E1000_IPCNFG);
#endif
	e1000e_write(dev, E1000_EEER, 0);
	e1000e_write(dev, E1000_IPCNFG, 0);
	e1000e_dbg(
		"E1000E: MAC EEE disabled EEER=%08x->%08x IPCNFG=%08x->%08x\n",
		eeer_before, e1000e_read(dev, E1000_EEER), ipc_before,
		e1000e_read(dev, E1000_IPCNFG));

	// 2. PHY-side disable: clear the EEE advertisement (EMI 0x8001,
	//    IEEE MMD 7.60): write the EMI register address into
	//    I82579_EMI_ADDR, then the data through I82579_EMI_DATA.  The EMI
	//    window is PHY address 2, page 0, registers 16/17 -- holding the
	//    PHY semaphore, as every other PHY access does.
	uint16_t lp = 0xFFFF, adv_after = 0xFFFF;
	if (e1000e_acquire_swflag(dev) != 0) {
		e1000e_dbg("E1000E: EEE: PHY semaphore not granted, skipped\n");
		return;
	}
	if (e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
					  I217_EEE_ADVERTISEMENT) == 0 &&
	    e1000e_phy_write_paged_locked(dev, I82579_EMI_DATA, 0x0000) == 0) {
		// Read back to confirm
		if (e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
						  I217_EEE_ADVERTISEMENT) ==
		    0) {
			(void)e1000e_phy_read_paged_locked(dev, I82579_EMI_DATA,
							   &adv_after);
		}
	}
	if (e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
					  I217_EEE_LP_ABILITY) == 0) {
		(void)e1000e_phy_read_paged_locked(dev, I82579_EMI_DATA, &lp);
	}
	e1000e_release_swflag(dev);
	e1000e_dbg(
		"E1000E: PHY EEE adv cleared (EMI[8001]=%04x) LP_ability(EMI[8002])=%04x\n",
		adv_after, lp);
	(void)lp;
	(void)adv_after;
	// NOTE: I218_LPI_CTRL (page 776 reg 18) is not touched.  With the old
	// page threshold it resolved to PHY address 2 register 18, PHY
	// Control 2, and clearing THAT is what broke autoneg ("LPAR=0,
	// AN_COMPLETE=0") when it was tried; the real LPI control register
	// is page 772 register 20 at address 1, and EEE is already off
	// through the advertisement above.
}

// PHY-front-end copper config that MUST run before autoneg restart on
// every I82577/82578/82579/I217/I218/I219 PHY.  Without it, the PHY's
// NVM defaults leave CRS_ON_TX off and the downshift retry counter at
// 0, which on this exact board (ThinkPad P50, I219-LM PCH-SPT-H) causes
// the PHY to resolve at 10 Mb/s even when both ends advertise 100FD
// with autoneg ACK.
//
// Bits to set in I82577_CFG_REG (page 0 reg 22):
//   * ASSERT_CRS_ON_TX   - keep CRS asserted during MAC transmits so
//                          the MAC TX FSM doesn't abort mid-frame.
//   * ENABLE_DOWNSHIFT=3 - allow 3 training retries at the higher
//                          speed before downshifting; the NVM default
//                          of 0 retries is what's pinning us to 10Mb.
static int e1000e_copper_link_setup_82577(e1000e_dev_t *dev)
{
	uint16_t before = 0, after = 0;
	if (e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_CFG_REG, &before) !=
	    0) {
		e1000e_dbg("E1000E: I82577_CFG_REG read failed\n");
		return -1;
	}
	uint16_t v = before;
	v &= ~I82577_CFG_ENABLE_DOWNSHIFT; // clear retry-count field first
	v |= I82577_CFG_ASSERT_CRS_ON_TX;
	v |= I82577_CFG_ENABLE_DOWNSHIFT; // = 3 retries (bits 11:10 = 11b)
	if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, I82577_CFG_REG, v) != 0) {
		e1000e_dbg("E1000E: I82577_CFG_REG write failed\n");
		return -1;
	}
	e1000e_delay_us(1000);
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_CFG_REG, &after);
	e1000e_dbg(
		"E1000E: I82577_CFG_REG %04x -> %04x (CRS_ON_TX | DOWNSHIFT=3)\n",
		before, after);
	if (after != v) {
		e1000e_dbg(
			"E1000E: WARNING I82577_CFG_REG write did not stick (wanted %04x got %04x)\n",
			v, after);
	}
	return 0;
}

// Restart copper auto-negotiation on the integrated PHY and block until
// it completes (or timeout).  This is the missing step that left the I219
// PHY without FLPs on the wire after our PHY_RST: BMSR.AN_COMPLETE = 0,
// BMSR.LSTATUS = 0, while MAC STATUS.LU = 1 reflected only the cached
// SMBus idle link.  Without a real copper link the MAC PCS never grants
// TX, so descriptors stay in the on-chip FIFO and GPTC stays at 0.
//
// Sequence is the IEEE 802.3 / Intel PCH-LAN canonical one:
//   1. Run the I82577 copper-link setup (PHY copper front-end config).
//   2. Program advertised abilities (ANAR, GBCR).
//   3. Set BMCR = AUTONEG_EN | AUTONEG_RESTART.
//   4. Poll BMSR.AN_COMPLETE for up to 5 s.
//   5. Read I82577_PHY_STATUS_2 for ground-truth resolved speed.
static int e1000e_phy_restart_autoneg(e1000e_dev_t *dev)
{
	uint16_t bmsr_before = 0;
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMSR, &bmsr_before);
#if E1000E_DBG
	uint16_t bmcr_before = 0, anar_before = 0, gbcr_before = 0;
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMCR, &bmcr_before);
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_ANAR, &anar_before);
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_GBCR, &gbcr_before);
#endif

	// CRITICAL on real hardware: if the BIOS already brought up link
	// before we got here (BMSR.LSTATUS=1 AND BMSR.AN_COMPLETE=1), do
	// NOT restart autoneg.  Restarting forces the PHY to drop the
	// existing link, re-train from scratch, and on this ThinkPad P50
	// / I219-LM the partner switch's autoneg state machine takes
	// longer than our 5 s poll to re-converge — link goes down and
	// never comes back up during driver init.  Adopt the BIOS-
	// negotiated link as-is; it's already valid.
	if ((bmsr_before & MII_BMSR_LSTATUS) &&
	    (bmsr_before & MII_BMSR_AN_COMPLETE)) {
		e1000e_dbg(
			"E1000E: PHY link already UP (BMCR=%04x BMSR=%04x ANAR=%04x GBCR=%04x) - "
			"adopting BIOS-negotiated link, skipping autoneg restart\n",
			bmcr_before, bmsr_before, anar_before, gbcr_before);

		// Still clear the MAC-side LPLU mirror so the MAC won't gate
		// its own internal MII transmitter, but leave the PHY alone.
		e1000e_clear_mac_phy_ctrl_lplu(dev);

#if E1000E_DBG
		// Read PSS2 ground truth speed for the diagnostic line.
		uint16_t pss2 = 0;
		(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR,
					 I82577_PHY_STATUS_2, &pss2);
		const char *phy_speed_str = "10";
		switch (pss2 & I82577_PHY_STATUS2_SPEED_MASK) {
		case I82577_PHY_STATUS2_SPEED_1000MBPS:
			phy_speed_str = "1000";
			break;
		case I82577_PHY_STATUS2_SPEED_100MBPS:
			phy_speed_str = "100";
			break;
		default:
			phy_speed_str = "10";
			break;
		}
		e1000e_dbg(
			"E1000E: PHY ground-truth I82577_PHY_STATUS_2=%04x speed=%sMb/s MDIX=%u RevPol=%u\n",
			pss2, phy_speed_str,
			(pss2 & I82577_PHY_STATUS2_MDIX) ? 1 : 0,
			(pss2 & I82577_PHY_STATUS2_REV_POLARITY) ? 1 : 0);
#endif

		// CRITICAL on Lenovo P50 / I219-LM: BIOS leaves the PHY's OEM
		// bits at 0x544 (LPLU=1, GBE_DIS=1, RESTART_AN=1) post-handoff.
		// LPLU=1 with link UP makes the PHY accept link pulses but
		// throttles the MII data path -- the MAC fetches our descriptor
		// (TDFH advances by 16 bytes) but the buffer payload is never
		// pulled from host memory and the descriptor is never written
		// back, so TDH stays 0 and GPTC stays 0.  Indistinguishable
		// from a TX-arbiter gate without inspecting HV_OEM_BITS directly.
		//
		// Clear LPLU/GBE_DIS in place WITHOUT setting RESTART_AN, so we
		// lift the data-path throttle without dropping the live link.
		(void)e1000e_oem_bits_clear_lplu_ex(dev, 0);
		return 0;
	}

	// Clear the MAC-side LPLU / GBE_DIS mirror first.  If we don't, the
	// PHY will pick up the LPLU signal again on the next autoneg cycle
	// and resolve at 10 Mb/s no matter what we write into HV_OEM_BITS.
	e1000e_clear_mac_phy_ctrl_lplu(dev);

	// Then clear the PHY-side OEM bits so the PHY itself permits >10Mb
	// resolutions.  This both clears LPLU/GBE_DIS and triggers an
	// internal autoneg restart (RESTART_AN bit).
	(void)e1000e_oem_bits_clear_lplu(dev);

	// Disable EEE BEFORE programming the I82577 copper front-end and
	// BEFORE writing autoneg advertisements.  EEE NextPage exchange on
	// OEM-branded PCH-LAN NVMs causes the PHY's HCD selection to fall
	// back to 10Mb when the partner doesn't speak matching EEE NPs.
	e1000e_disable_eee(dev);

	// CRITICAL: program the I82577 PHY copper front-end (CRS_ON_TX +
	// ENABLE_DOWNSHIFT=3 retries) BEFORE writing ANAR/GBCR/BMCR.  Without
	// this the PHY uses NVM-default downshift count (0 retries) and
	// resolves at 10 Mb/s on the first 100Mb training failure.
	(void)e1000e_copper_link_setup_82577(dev);

	if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, MII_ANAR,
				MII_ANAR_FULL_RANGE) != 0) {
		e1000e_dbg("E1000E: PHY ANAR write failed\n");
		return -1;
	}
	if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, MII_GBCR,
				MII_GBCR_ADV_1000FD) != 0) {
		e1000e_dbg("E1000E: PHY GBCR write failed\n");
		return -1;
	}
	if (e1000e_phy_write_at(dev, E1000E_PHY_ADDR, MII_BMCR,
				MII_BMCR_AUTONEG_EN |
					MII_BMCR_AUTONEG_RESTART) != 0) {
		e1000e_dbg("E1000E: PHY BMCR autoneg-restart write failed\n");
		return -1;
	}

	uint16_t bmsr = 0;
#if E1000E_DBG
	int ms_waited = 0;
#endif
	for (int i = 0; i < 5000; i++) {
		e1000e_delay_us(1000);
#if E1000E_DBG
		ms_waited = i + 1;
#endif
		if (e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMSR, &bmsr) !=
		    0) {
			continue;
		}
		if (bmsr & MII_BMSR_AN_COMPLETE)
			break;
	}

#if E1000E_DBG
	// BMSR.LSTATUS is latched-low; re-read so we see the live state.
	uint16_t bmsr2 = 0;
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMSR, &bmsr2);

	// Diagnostic: read the link-partner advertisement (MII reg 5 LPAR)
	// and the 1000BASE-T status (MII reg 10 GBSR).  If LPAR shows that
	// the partner only advertised 10 Mb/s, our resolved 10 Mb/s outcome
	// is correct and the partner-side cable/switch is the limit.  If
	// LPAR shows 100/1000 capability but BMCR result still reads 10 Mb/s,
	// the PHY itself is misbehaving.
	uint16_t lpar = 0, gbsr = 0;
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, 5, &lpar);
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, 10, &gbsr);
	e1000e_dbg(
		"E1000E: PHY LPAR=%04x (10HD=%u 10FD=%u 100HD=%u 100FD=%u T4=%u Pause=%u AsymPause=%u RemFault=%u Ack=%u)\n",
		lpar, (lpar >> 5) & 1, (lpar >> 6) & 1, (lpar >> 7) & 1,
		(lpar >> 8) & 1, (lpar >> 9) & 1, (lpar >> 10) & 1,
		(lpar >> 11) & 1, (lpar >> 13) & 1, (lpar >> 14) & 1);
	e1000e_dbg(
		"E1000E: PHY GBSR=%04x (LP_1000HD=%u LP_1000FD=%u LRS=%u RRS=%u IDLE_ERR=%u)\n",
		gbsr, (gbsr >> 10) & 1, (gbsr >> 11) & 1, (gbsr >> 13) & 1,
		(gbsr >> 14) & 1, gbsr & 0xFF);

	uint16_t anar_after = 0, gbcr_after = 0, bmcr_after = 0;
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_BMCR, &bmcr_after);
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_ANAR, &anar_after);
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_GBCR, &gbcr_after);

	e1000e_dbg(
		"E1000E: PHY autoneg restart ANAR %04x->%04x GBCR %04x->%04x BMCR %04x->%04x\n",
		anar_before, anar_after, gbcr_before, gbcr_after, bmcr_before,
		bmcr_after);
	e1000e_dbg(
		"E1000E: PHY autoneg %s after %d ms (BMSR %04x -> %04x, LSTATUS=%u AN_COMPLETE=%u)\n",
		(bmsr & MII_BMSR_AN_COMPLETE) ? "complete" : "TIMEOUT",
		ms_waited, bmsr_before, bmsr2,
		(bmsr2 & MII_BMSR_LSTATUS) ? 1 : 0,
		(bmsr2 & MII_BMSR_AN_COMPLETE) ? 1 : 0);

	// Ground truth: read I82577_PHY_STATUS_2 (page 0 reg 26).  IEEE 802.3
	// leaves BMCR.SPEED bits undefined when AUTONEG_EN=1, so this is the
	// only authoritative source of the PHY's resolved speed.  If MAC
	// STATUS.SPEED disagrees with this register the fault is in the
	// MAC<->PHY internal interface, not in autoneg.
	uint16_t pss2 = 0;
	(void)e1000e_phy_read_at(dev, E1000E_PHY_ADDR, I82577_PHY_STATUS_2,
				 &pss2);
	const char *phy_speed_str = "10";
	switch (pss2 & I82577_PHY_STATUS2_SPEED_MASK) {
	case I82577_PHY_STATUS2_SPEED_1000MBPS:
		phy_speed_str = "1000";
		break;
	case I82577_PHY_STATUS2_SPEED_100MBPS:
		phy_speed_str = "100";
		break;
	default:
		phy_speed_str = "10";
		break;
	}
	e1000e_dbg(
		"E1000E: PHY ground-truth I82577_PHY_STATUS_2=%04x speed=%sMb/s MDIX=%u RevPol=%u\n",
		pss2, phy_speed_str, (pss2 & I82577_PHY_STATUS2_MDIX) ? 1 : 0,
		(pss2 & I82577_PHY_STATUS2_REV_POLARITY) ? 1 : 0);
#endif // E1000E_DBG

	return (bmsr & MII_BMSR_AN_COMPLETE) ? 0 : -1;
}

// LANPHYPC takeover sequence for SPT/CNP/TGP+ I218/I219.  On these parts,
// the PHY's clock and power gate is owned by the Management Engine until the OS driver explicitly
// takes ownership via CTRL.LANPHYPC_OVERRIDE.  When OVERRIDE=1 the
// LANPHYPC output pin is driven by VALUE; when VALUE=0 the pin is LOW
// which the PHY interprets as "power ON".  (VALUE=1 powers the PHY
// DOWN — getting that wrong manifests as "link LED was on at boot, goes
// off during driver init and stays off".)
static void __attribute__((unused))
e1000e_toggle_lanphypc_pch_lpt(e1000e_dev_t *dev)
{
#if E1000E_DBG
	uint32_t before = e1000e_read(dev, E1000_CTRL);
	uint32_t mac_reg = before;
#else
	uint32_t mac_reg = e1000e_read(dev, E1000_CTRL);
#endif

	// Step 1: drop both OVERRIDE and VALUE so we don't glitch the gate
	//         to "off" while flipping OVERRIDE.
	mac_reg &= ~E1000_CTRL_LANPHYPC_OVERRIDE;
	mac_reg &= ~E1000_CTRL_LANPHYPC_VALUE;
	e1000e_write(dev, E1000_CTRL, mac_reg);
	(void)e1000e_read(dev, E1000_CTRL); // posting flush
	e1000e_delay_us(20);

	// Step 2: assert OVERRIDE alone (VALUE stays 0 = drive PHY power ON).
	mac_reg |= E1000_CTRL_LANPHYPC_OVERRIDE;
	e1000e_write(dev, E1000_CTRL, mac_reg);
	(void)e1000e_read(dev, E1000_CTRL); // posting flush
	e1000e_delay_us(50000); // 50 ms delay

	// Step 3: drop OVERRIDE — deliberately skipped.  On SPT-H the PHY clock
	//         domain is happiest with OVERRIDE left asserted after the
	//         50 ms wait so the ME does not perpetually contend for the
	//         gate input.

#if E1000E_DBG
	uint32_t after = e1000e_read(dev, E1000_CTRL);
	e1000e_dbg(
		"E1000E: LANPHYPC toggled CTRL %08x -> %08x (OVERRIDE=1 VALUE=0 = PHY power ON)\n",
		before, after);
#endif
}

static void e1000e_wait_lan_init_done(e1000e_dev_t *dev)
{
	uint32_t status = 0;
	uint32_t loops_left = E1000_ICH8_LAN_INIT_TIMEOUT;

	do {
		status = e1000e_read(dev, E1000_STATUS);
		if (status & E1000_STATUS_LAN_INIT_DONE) {
			break;
		}
		e1000e_delay_us(150);
	} while (--loops_left);

	if (!(status & E1000_STATUS_LAN_INIT_DONE)) {
		e1000e_dbg("E1000E: LAN_INIT_DONE timeout (STATUS=%08x)\n",
			   status);
		return;
	}

	e1000e_dbg("E1000E: LAN_INIT_DONE after %u us (STATUS=%08x)\n",
		   (E1000_ICH8_LAN_INIT_TIMEOUT - loops_left) * 150, status);

	e1000e_write(dev, E1000_STATUS, status & ~E1000_STATUS_LAN_INIT_DONE);
	e1000e_dbg("E1000E: LAN_INIT_DONE clear -> STATUS=%08x\n",
		   e1000e_read(dev, E1000_STATUS));
}

// Apply the I217/I218/I219 (PCH-LAN) bring-up workarounds.
//
// Two things are required on these parts before RX/TX will flow:
//
//   1. CTRL_EXT.DRV_LOAD must be set so the Management Engine knows
//      an OS driver is now in charge of the NIC.  Without this the ME
//      considers itself the sole owner and silently filters frames.
//
//   2. Software MUST perform at least one MDIO transaction against the
//      integrated PHY to acknowledge the post-reset PHY state and
//      clear STATUS.PHYRA.  Until PHYRA is acknowledged the PHY gates
//      its data interface to the MAC and *no frames flow in either
//      direction* even though L1 link is up.  We do this under the ME
//      SW-flag so we don't race with ME firmware over MDIO ownership.
static void e1000e_pch_lan_init(e1000e_dev_t *dev)
{
	e1000e_dbg("E1000E: applying PCH-LAN (I217/I218/I219) workarounds\n");

	// Configure CTRL_EXT correctly for an OS-driver scenario:
	//   * DRV_LOAD (bit 28)  : tell ME we now own the NIC
	//   * PHY_PDEN  (bit 20) : MUST be 0, otherwise the PHY is allowed
	//                          to enter low-power and the MAC sees no
	//                          link clock — RX silently dies.  NVM
	//                          default is 1b on PCH-LAN parts.
	//   * PBA_SUPPORT (bit 31): MUST be 0 in INTx/MSI mode (1b is the
	//                           MSI-X-only IMS->PBA-clear behaviour).
	//   * ASDCHK     (bit 12): self-clearing one-shot, force to 0 so a
	//                          stale value from BIOS doesn't re-fire
	//                          a speed-detection sequence on top of us.
	//   * RO_DIS     (bit 17): leave whatever NVM/BIOS programmed.
	//   * BIT(22)            : MUST be 1 per the I218/I219 datasheet.
	//                          On discrete 82574 this would be a LINK_MODE
	//                          bit, but on PCH-LAN parts the integrated PHY
	//                          makes LINK_MODE meaningless and BIT(22) is
	//                          re-purposed as a TX-arbitration mandatory bit
	//                          — without it the descriptor engine never
	//                          fetches even though TCTL.EN is honored.
	//   * PHY_PDEN  (bit 20) : SET on PCH-LAN per the datasheet. Only enables PHY
	//                          power-down when MAC is in D3, so harmless at
	//                          D0; required by spec for the PCH-LAN family.
	uint32_t ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
	e1000e_dbg("E1000E: CTRL_EXT=%08x (before)\n", ctrl_ext);
	ctrl_ext |= E1000_CTRL_EXT_DRV_LOAD;
	ctrl_ext &= ~((1u << 31) // PBA_SUPPORT (MSI-X-only)
		      | (1u << 12) // ASDCHK (one-shot, force 0)
		      | E1000_CTRL_EXT_FORCE_SMBUS |
		      (1u << 23)); // LINK_MODE bit 1 (clear; bit 22 set below)
	ctrl_ext |= (1u << 22) // datasheet-mandated TX-arbitration BIT(22)
		    | (1u << 20); // datasheet-mandated PHY_PDEN
	e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);
	e1000e_dbg(
		"E1000E: CTRL_EXT=%08x (after DRV_LOAD/PHY_PDEN/BIT22/!PBA)\n",
		e1000e_read(dev, E1000_CTRL_EXT));

	e1000e_delay_us(2000);

	// ------------------------------------------------------------------
	// Hard MAC reset (CTRL.RST) AFTER asserting DRV_LOAD.
	//
	// On SPT-H / I219-LM the ME firmware leaves the MAC in a half-owned
	// state at boot: registers accept writes, link comes up, but the
	// TX descriptor-fetch DMA stays gated until the MAC is fully reset
	// *with the OS announcing ownership first*.  FWSM.FW_VALID=1 plus
	// FEXTNVM7 bit 31 set is the giveaway — that's "FW controls PHY
	// power management", and it disappears only after a CTRL.RST that
	// is performed under the SW-FLAG with DRV_LOAD already asserted.
	//
	// Per spec 4.6: CTRL.RST resets the MAC core but PRESERVES PCIe
	// config, CTRL_EXT, and the NVM-shadowed PHY config.  However it
	// DOES clear CTRL_EXT.DRV_LOAD on most PCH-LAN parts, so we have
	// to re-assert it after the reset completes.  We must also wait
	// for the bit to self-clear (typically <1 us, but spec allows up
	// to ~10 ms on PCH-LAN due to PHY re-link).
	{
		// ------------------------------------------------------------
		//  Full-chip reset — PCH-LAN reset sequence per Intel datasheet.
		//
		//  ROOT CAUSE of the I219-LM TX stall on the Lenovo P50:
		//  ME firmware on PCH-SPT-H is actively driving manageability
		//  TLPs through the LAN function's PCIe master at boot
		//  (FWSM.FW_VALID=1, MANC.EN_MNG2HOST=1, ICR.MNG fires
		//  spontaneously).  Per the datasheet:
		//
		//    "Prevent the PCI-E bus from sticking if there is no TLP
		//     connection on the last TLP read/write transaction when
		//     MAC is reset."
		//
		//  If CTRL.RST is asserted while an ME-driven TLP is in flight,
		//  the MAC's outbound DMA path gets wedged.  Symptom is exactly
		//  what we observe: TCTL.EN sticks, TDT tracks the doorbell,
		//  but TDH never advances, GPTC=0, TPT=0, and there are no
		//  PCI errors — because no TLP is ever issued from the wedged
		//  master.
		//
		//  Fix:
		//    1. Set CTRL.GIO_MASTER_DISABLE (bit 2) and poll
		//       STATUS.GIO_MASTER_EN (bit 19) to clear, draining all
		//       in-flight master TLPs.
		//    2. Mask interrupts, clear ICR.
		//    3. RCTL=0, TCTL=PSP (NOT 0 — PSP is the canonical PCH-LAN
		//       value during the quiesce window).
		//    4. Wait 10 ms for any pending transactions to retire.
		//    5. Issue full-chip reset: CTRL |= (PHY_RST | RST) in
		//       the SAME mmio write, under SWFLAG.  PHY_RST in the
		//       same write is what re-syncs the MAC<->PHY KMRN bus;
		//       without it the KMRN interface stays in whatever
		//       UEFI/ME left it and the MAC's TX scheduler can stay
		//       gated even though MDIO is responsive.
		//    6. Wait 20 ms (PHY reset path takes longer than MAC).
		// ------------------------------------------------------------

		// Step 1: Drain PCIe master.
		{
			uint32_t ctrl_drain = e1000e_read(dev, E1000_CTRL);
			e1000e_write(
				dev, E1000_CTRL,
				ctrl_drain |
					(1u << 2) /* GIO_MASTER_DISABLE */);
			(void)e1000e_read(dev, E1000_STATUS);

			int drain_us = 0;
			uint32_t st_drain = 0;
			for (drain_us = 0; drain_us < 800000; drain_us += 100) {
				st_drain = e1000e_read(dev, E1000_STATUS);
				if (!(st_drain &
				      (1u << 19) /* GIO_MASTER_EN */))
					break;
				e1000e_delay_us(100);
			}
			e1000e_dbg(
				"E1000E: PCIe master drain: %s after %d us (STATUS=%08x)\n",
				(st_drain & (1u << 19)) ? "TIMEOUT" : "ok",
				drain_us, st_drain);
		}

		// Step 2: mask interrupts, ack any pending.
		e1000e_write(dev, E1000_IMC, 0xFFFFFFFFu);
		(void)e1000e_read(dev, 0x000C0); // ICR

		// Step 3+4: quiesce TX/RX (TCTL=PSP per spec), wait 10 ms.
		e1000e_write(dev, E1000_RCTL, 0);
		e1000e_write(dev, E1000_TCTL, E1000_TCTL_PSP);
		(void)e1000e_read(dev, E1000_STATUS); // posting flush
		e1000e_delay_us(10000);

		// Step 5: full-chip reset under SWFLAG.
		if (e1000e_acquire_swflag(dev) != 0) {
			e1000e_dbg(
				"E1000E: CTRL.RST: could not acquire SWFLAG, proceeding anyway\n");
		}
		uint32_t ctrl_pre = e1000e_read(dev, E1000_CTRL);
		e1000e_dbg("E1000E: CTRL.RST: asserting RST only (CTRL=%08x)\n",
			   ctrl_pre);
		// PCH-LAN reset: write CTRL with E1000_CTRL_RST asserted.
		// We deliberately do NOT combine CTRL.PHY_RST with CTRL.RST on
		// PCH-LAN — doing so tears down the BIOS-negotiated PHY link in
		// the same cycle and leaves the PHY in a state from which only
		// the full PHY hardware-reset sequence (with HV PHY workarounds,
		// LCD config from NVM, MSE thresholds, etc.) can recover.  The
		// BIOS already brought up link before we got here; preserving
		// that link across our chip reset means the PHY just keeps
		// working.
		e1000e_write(dev, E1000_CTRL, ctrl_pre | E1000_CTRL_RST);

		// Step 6: per the datasheet, msleep(20) — do NOT touch the chip
		// during this window; the spec says "cannot issue a flush
		// here because it hangs the hardware".
		e1000e_delay_us(20000);

		// Poll for RST self-clear, up to 20 ms more.
		int spun = 0;
		uint32_t ctrl_post = 0;
		for (spun = 0; spun < 2000; spun++) {
			ctrl_post = e1000e_read(dev, E1000_CTRL);
			if (!(ctrl_post & E1000_CTRL_RST))
				break;
			e1000e_delay_us(10);
		}
		e1000e_dbg(
			"E1000E: CTRL.RST: cleared after %d us (CTRL=%08x)\n",
			spun * 10, ctrl_post);

		// Reset clears interrupt mask state — re-mask everything for now.
		e1000e_write(dev, E1000_IMC, 0xFFFFFFFFu);
		(void)e1000e_read(dev, 0x000C0); // ICR read-to-clear
		e1000e_release_swflag(dev);

		// Re-assert DRV_LOAD (CTRL.RST cleared it on PCH-LAN) and
		// re-program CTRL_EXT per the I218/I219 datasheet:
		//   - DRV_LOAD (bit 28) : OS now owns the NIC
		//   - BIT(22)           : mandatory PCH-LAN TX-arbitration bit
		//   - PHY_PDEN (bit 20) : mandatory PCH-LAN+ bit
		//   - RO_DIS  (bit 17)  : disable PCIe relaxed-ordering for
		//                          descriptor coherency
		//   - clear PBA_SUPPORT (bit 31), ASDCHK (bit 12), FORCE_SMBUS
		//     (bit 11), and LINK_MODE bit 1 (bit 23).
		ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
		ctrl_ext &= ~((1u << 31) | (1u << 12) |
			      E1000_CTRL_EXT_FORCE_SMBUS | (1u << 23));
		ctrl_ext |=
			E1000_CTRL_EXT_DRV_LOAD | E1000_CTRL_EXT_RO_DIS |
			(1u << 22) // datasheet-mandated TX-arbitration BIT(22)
			| (1u << 20); // datasheet-mandated PHY_PDEN
		e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);
		e1000e_dbg(
			"E1000E: CTRL.RST: CTRL_EXT re-set to %08x (DRV_LOAD/PHY_PDEN/BIT22)\n",
			e1000e_read(dev, E1000_CTRL_EXT));

		// NOTE: do NOT call e1000e_toggle_lanphypc_pch_lpt here on this
		// ThinkPad P50 / I219-LM: empirically, before the toggle the PHY
		// is responsive (ID 0x154:0x00a6 readable at MDIO addr 2), and
		// after asserting CTRL.LANPHYPC_OVERRIDE the PHY goes silent
		// (every MDIO read returns 0xFFFF).  The toggle is needed
		// only when the ME left the PHY power-gated; the SPT-H BIOS on
		// this board does not.

		// PCH-LAN post-reset wait: after a full reset, wait until the
		// hardware finishes the basic LAN/PHY config sequence before
		// touching more PHY-facing state.
		e1000e_wait_lan_init_done(dev);

		// PCH-SPT family init reprograms PBA=26 after reset.  If the
		// reset/firmware default leaves the packet buffer split wrong,
		// the MAC can look alive while the TX datapath never actually
		// starts moving descriptors.
#if E1000E_DBG
		uint32_t pba_before = e1000e_read(dev, E1000_PBA);
#endif
		e1000e_write(dev, E1000_PBA, 26);
		e1000e_dbg("E1000E: PBA %08x -> %08x\n", pba_before,
			   e1000e_read(dev, E1000_PBA));

		e1000e_delay_us(10000); // PHY re-link settling
	}
	// ------------------------------------------------------------------

	// Acknowledge the PHY via MDIO.  Hold the SW flag while we do it so
	// the ME doesn't race us on the MDIC bus.  Probe a few PHY addresses
	// because integrated PCH-LAN PHYs do not consistently sit at addr 1
	// (I219 frequently uses addr 2; some parts auto-select).  We read
	// the hardwired PHY-ID registers (MII regs 2 and 3) — a non-zero,
	// non-0xFFFF value identifies a real PHY at that address.
	if (e1000e_acquire_swflag(dev) == 0) {
		int found = -1;
		for (uint8_t addr = 0; addr <= 3; addr++) {
			uint16_t id1 = 0, id2 = 0;
			int rc1 = e1000e_phy_read_at(dev, addr, 2, &id1);
			int rc2 = e1000e_phy_read_at(dev, addr, 3, &id2);
			e1000e_dbg(
				"E1000E: PHY probe addr=%u rc=%d/%d ID=%04x:%04x\n",
				addr, rc1, rc2, id1, id2);
			if (rc1 == 0 && rc2 == 0 &&
			    (id1 != 0x0000 && id1 != 0xFFFF)) {
				found = addr;
			}
		}
		if (found >= 0) {
			uint16_t bmcr = 0, bmsr = 0;
			e1000e_phy_read_at(dev, (uint8_t)found, 0, &bmcr);
			e1000e_phy_read_at(dev, (uint8_t)found, 1, &bmsr);
			e1000e_dbg(
				"E1000E: PHY at addr %d BMCR=%04x BMSR=%04x\n",
				found, bmcr, bmsr);
		} else {
			e1000e_dbg(
				"E1000E: no PHY responded on MDIO addrs 0..3\n");
		}
		e1000e_release_swflag(dev);
	} else {
		e1000e_dbg(
			"E1000E: could not acquire ME SW flag — skipping PHY probe\n");
	}

	// Clear STATUS.PHYRA.  Per 82574 datasheet 10.2.2.2: "This bit is
	// read/write.  Hardware sets this bit following the assertion of
	// PHY reset.  The bit is cleared on writing 0b to it."  So we
	// clear it by writing the rest of STATUS unchanged but with bit 10
	// set to 0 — NOT by W1C.  STATUS is otherwise read-only so writing
	// the whole register is safe.
	uint32_t st = e1000e_read(dev, E1000_STATUS);
	if (st & E1000_STATUS_PHYRA) {
		e1000e_write(dev, E1000_STATUS, st & ~E1000_STATUS_PHYRA);
	}

	// PCH-LAN TX-arbiter workaround.  TARC0 / TARC1 are the per-queue
	// TX-arbitration registers; on PCH-LAN parts (I217 / I218 / I219)
	// a few bits in these registers come out of reset cleared even
	// though the silicon REQUIRES them set in order to issue a TX
	// descriptor-fetch on the PCIe bus.  The symptom — and we hit it
	// exactly — is: TCTL.EN = 1, TXDCTL.ENABLE = 1, TDH != TDT, the
	// descriptor in host memory is well-formed (cmd has EOP|IFCS|RS,
	// length nonzero, status = 0), STATUS.GIO_MASTER_EN = 1, MSI
	// enabled — and yet TDH never advances and GPTC / TPT stay at
	// zero.  These magic bits are required by the I218/I219 datasheet's
	// hardware-init bit programming.
	//
	// TARC0 bits:
	//   23  — undocumented "TX queue arbitration count enable"
	//   24  — same family
	//   26  — same family (required on I219)
	//   27  — same family (required on I219)
	// TARC1 bits:
	//   24  set   — match TARC0
	//   28  set only while TCTL.MULR is clear (it is set: see below)
	{
		uint32_t tarc0 = e1000e_read(dev, E1000_TARC0);
#if E1000E_DBG
		uint32_t tarc0_before = tarc0;
#endif
		tarc0 |= (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27);
		e1000e_write(dev, E1000_TARC0, tarc0);

		// TARC1: per the I218/I219 datasheet, set bits 24, 26, 30, and
		// bit 28 only while TCTL.MULR is clear.  e1000e_init_tx() sets
		// MULR on every PCH-LAN part, so bit 28 is cleared here.
		uint32_t tarc1 = e1000e_read(dev, E1000_TARC1);
#if E1000E_DBG
		uint32_t tarc1_before = tarc1;
#endif
		tarc1 |= (1u << 24) | (1u << 26) | (1u << 30);
		tarc1 &= ~(1u << 28);
		e1000e_write(dev, E1000_TARC1, tarc1);

		// Descriptor data corruption during NFS v2 UDP traffic: the
		// NFS filtering of the receive filter is switched off.
		e1000e_write(dev, E1000_RFCTL,
			     e1000e_read(dev, E1000_RFCTL) |
				     E1000_RFCTL_NFSW_DIS |
				     E1000_RFCTL_NFSR_DIS);

		e1000e_dbg("E1000E: TARC0 %08x -> %08x, TARC1 %08x -> %08x\n",
			   tarc0_before, e1000e_read(dev, E1000_TARC0),
			   tarc1_before, e1000e_read(dev, E1000_TARC1));
	}

	// PCH-LAN init also normalizes the PCIe DMA snoop policy through GCR.
	// Leaving the reset-time no-snoop state in place can make descriptor/
	// buffer DMA depend on platform firmware cache policy instead of the
	// host's normal coherent path.  For PCH-LAN the safe baseline is the
	// snooped mode: clear all no-snoop request bits.
	{
		uint32_t gcr_before = e1000e_read(dev, E1000_GCR);
		uint32_t gcr = gcr_before & ~E1000_GCR_NO_SNOOP_ALL;
		e1000e_write(dev, E1000_GCR, gcr);
		(void)e1000e_read(dev, E1000_GCR);
		e1000e_dbg("E1000E: GCR %08x -> %08x (PCIe DMA snoop policy)\n",
			   gcr_before, e1000e_read(dev, E1000_GCR));
	}

	// ULP exit + "host owns config" handshake with the ME.
	//
	// On SPT-H / I219-LM the ME may have parked the LAN controller in
	// ULP (Ultra-Low-Power) before handing it to the OS.  In ULP the
	// MAC accepts register writes (TCTL.EN, TDT, etc. all stick) but
	// the TX descriptor-fetch DMA is gated off — exactly the symptom
	// we see: TDH stays 0, GPTC=0, descriptor.sta=0.  We exit ULP
	// by clearing H2ME.ULP and asserting H2ME.ENFORCE_SETTINGS so the
	// ME knows the host driver has taken over and must not re-arm ULP.
	{
		uint32_t h2me_before = e1000e_read(dev, E1000_H2ME);
		uint32_t h2me = h2me_before;
		h2me &= ~E1000_H2ME_ULP;
		h2me |= E1000_H2ME_ENFORCE_SETTINGS;
		e1000e_write(dev, E1000_H2ME, h2me);
		e1000e_delay_us(10000);
		e1000e_dbg(
			"E1000E: H2ME %08x -> %08x (ULP cleared, ENFORCE_SETTINGS set)\n",
			h2me_before, e1000e_read(dev, E1000_H2ME));
	}

	// PCH ULP exit is not just the H2ME handshake.  The PHY-side SMBus
	// latch and I218 ULP state must also be cleared, otherwise the MAC can
	// accept descriptor doorbells while the MAC<->PHY path still sits in a
	// reset-to-SMBus / ULP state and TX fetch never starts.
	e1000e_force_exit_phy_ulp(dev);

	// The remaining SPT/LPT init path has two MAC-side pieces required by
	// the datasheet that we did not previously carry over: enable packet-
	// buffer ECC / memory error handling, and explicitly exit the S0ix-
	// style dynamic power-gating state.  On the failing P50 the key clue
	// is FEXTNVM7 staying at 0x80000000, which is the exact "time-sync
	// clock disabled / still power-gated" pattern that must be cleared on
	// resume before allowing normal DMA again.
	{
		uint32_t pbeccsts_before = e1000e_read(dev, E1000_PBECCSTS);
		uint32_t pbeccsts = pbeccsts_before | E1000_PBECCSTS_ECC_ENABLE;
		e1000e_write(dev, E1000_PBECCSTS, pbeccsts);

		uint32_t ctrl_before = e1000e_read(dev, E1000_CTRL);
		uint32_t ctrl = ctrl_before | E1000_CTRL_MEHE;
		e1000e_write(dev, E1000_CTRL, ctrl);

		uint32_t dpgfr_before = e1000e_read(dev, E1000_DPGFR);
		uint32_t dpgfr = dpgfr_before & ~(1u << 2);
		e1000e_write(dev, E1000_DPGFR, dpgfr);

		uint32_t ctrl_ext_before = e1000e_read(dev, E1000_CTRL_EXT);
		uint32_t ctrl_ext =
			ctrl_ext_before & ~(E1000_CTRL_EXT_DPG_EN |
					    E1000_CTRL_EXT_DMA_DYN_CLK_EN);
		ctrl_ext |= E1000_CTRL_EXT_RO_DIS;
		e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);

		uint32_t fextnvm7_before = e1000e_read(dev, E1000_FEXTNVM7);
		uint32_t fextnvm7 = fextnvm7_before;
		fextnvm7 &= ~E1000_FEXTNVM7_DISABLE_TSYNC_CLK;
		fextnvm7 |= E1000_FEXTNVM7_ENABLE_TSYNC_INTR;
		// Side-clock ungate: every SPT-or-later MAC.
		if (e1000e_mac_gen(dev->pci_dev->device_id) >=
		    E1000E_MAC_PCH_SPT) {
			fextnvm7 |= E1000_FEXTNVM7_SIDE_CLK_UNGATE;
		}
		e1000e_write(dev, E1000_FEXTNVM7, fextnvm7);

		e1000e_dbg(
			"E1000E: MAC DPG exit PBECCSTS=%08x->%08x CTRL=%08x->%08x DPGFR=%08x->%08x CTRL_EXT=%08x->%08x FEXTNVM7=%08x->%08x\n",
			pbeccsts_before, e1000e_read(dev, E1000_PBECCSTS),
			ctrl_before, e1000e_read(dev, E1000_CTRL), dpgfr_before,
			e1000e_read(dev, E1000_DPGFR), ctrl_ext_before,
			e1000e_read(dev, E1000_CTRL_EXT), fextnvm7_before,
			e1000e_read(dev, E1000_FEXTNVM7));
	}

	// The PCH-LAN reset path unconditionally sets KABGTXD.BGSQLBIAS after
	// the global reset.  Without this the AFE bandgap reference for the
	// Tx path can stay in a metastable bias state on PCH-LAN parts, which
	// silently gates the per-queue Tx DMA fetch even though every ring/
	// control register reads back as configured.  This is one of the two
	// writes that are unconditional in the datasheet's reset path; we
	// were missing it.
	{
		uint32_t kabgtxd_before = e1000e_read(dev, E1000_KABGTXD);
		uint32_t kabgtxd = kabgtxd_before | E1000_KABGTXD_BGSQLBIAS;
		e1000e_write(dev, E1000_KABGTXD, kabgtxd);
		e1000e_dbg("E1000E: KABGTXD %08x -> %08x (BGSQLBIAS set)\n",
			   kabgtxd_before, e1000e_read(dev, E1000_KABGTXD));
	}

	// One-shot diagnostic dump of registers that commonly gate TX on
	// PCH-LAN parts when the driver looks otherwise correct.
	e1000e_dbg(
		"E1000E: MANC=%08x FWSM=%08x FEXTNVM=%08x FEXTNVM6=%08x FEXTNVM7=%08x\n",
		e1000e_read(dev, E1000_MANC), e1000e_read(dev, E1000_FWSM),
		e1000e_read(dev, E1000_FEXTNVM),
		e1000e_read(dev, E1000_FEXTNVM6),
		e1000e_read(dev, E1000_FEXTNVM7));
}

// ============================================================================
// KMRN (Kumeran) MAC↔PHY interconnect access
// ============================================================================
//
// On PCH-LAN parts the MAC and the in-package PHY are not connected by
// a real GMII bus — instead they speak a serial side-band protocol
// called "Kumeran" over an internal interface.  KMRNCTRLSTA (MAC reg
// 0x00034) is the doorbell used by software to read/write Kumeran
// configuration registers that live on the PHY side of the link.
//
// Layout of KMRNCTRLSTA:
//   bits 15:0   data (read after REN completes; write payload when REN=0)
//   bits 22:16  REGADDR — Kumeran register offset
//   bit  21     REN     — Read ENable; set to start a read transaction
//
// (Often called read_kmrn_reg / write_kmrn_reg in vendor sample code.)
#define E1000_KMRNCTRLSTA 0x00034
#define E1000_KMRNCTRLSTA_OFFSET_SHIFT 16
#define E1000_KMRNCTRLSTA_OFFSET_MASK 0x001F0000u
#define E1000_KMRNCTRLSTA_REN 0x00200000u

// Kumeran register offsets we care about.
#define E1000_KMRN_K1_CONFIG_OFFSET 0x07
#define E1000_KMRN_K1_ENABLE 0x0002 // bit 1 of the 16-bit data
#define E1000_KMRN_HD_CTRL_OFFSET 0x10 // Half-Duplex / link timing
#define E1000_KMRN_HD_CTRL_10_100_DEFAULT 0x0004
#define E1000_KMRN_HD_CTRL_1000_DEFAULT 0x0000
#define E1000_KMRN_INBAND_PARAM_OFFSET 0x09 // Inband control parameters

static int e1000e_kmrn_read(e1000e_dev_t *dev, uint8_t kmrn_off, uint16_t *val)
{
	uint32_t cmd = (((uint32_t)kmrn_off << E1000_KMRNCTRLSTA_OFFSET_SHIFT) &
			E1000_KMRNCTRLSTA_OFFSET_MASK) |
		       E1000_KMRNCTRLSTA_REN;
	e1000e_write(dev, E1000_KMRNCTRLSTA, cmd);
	e1000e_delay_us(2);
	uint32_t r = e1000e_read(dev, E1000_KMRNCTRLSTA);
	if (val)
		*val = (uint16_t)(r & 0xFFFF);
	return 0;
}

static int e1000e_kmrn_write(e1000e_dev_t *dev, uint8_t kmrn_off, uint16_t val)
{
	uint32_t cmd = (((uint32_t)kmrn_off << E1000_KMRNCTRLSTA_OFFSET_SHIFT) &
			E1000_KMRNCTRLSTA_OFFSET_MASK) |
		       (uint32_t)val;
	e1000e_write(dev, E1000_KMRNCTRLSTA, cmd);
	e1000e_delay_us(2);
	return 0;
}

// Force the Kumeran K1 power-save state OFF.
//
// K1 is a low-power state that the Kumeran link enters between bursts.
// On many I217/I218/I219 steppings — and *consistently* on SPT-H I219-LM
// (dev 0x15B7) at 100 Mbps full-duplex — a hardware bug in the K1
// state machine causes EVERY frame received from the wire to be dropped
// inside the MAC↔PHY interconnect.  STATUS reports link UP, the PHY
// reports a clean autoneg, but TPR (Total Packets Received, which
// counts every frame at the MAC *before* address filtering) stays at
// zero.  Disabling K1 makes the interconnect always-on at the cost of
// a few mW and immediately fixes the RX path.  This is exactly the
// fix per the I218/I219 datasheet: configure K1 with k1_enable=false.
static void e1000e_disable_k1(e1000e_dev_t *dev)
{
	uint16_t k1 = 0;
	e1000e_kmrn_read(dev, E1000_KMRN_K1_CONFIG_OFFSET, &k1);
#if E1000E_DBG
	uint16_t before = k1;
#endif
	k1 &= ~E1000_KMRN_K1_ENABLE;
	e1000e_kmrn_write(dev, E1000_KMRN_K1_CONFIG_OFFSET, k1);

// Per Intel: after touching K1 we must briefly toggle CTRL_EXT.SPD_BYPS
// around a CTRL.SPEED rewrite so the MAC re-syncs its internal clock
// tree to the new K1 setting.  SPD_BYPS = CTRL_EXT bit 15.
#define E1000_CTRL_EXT_SPD_BYPS (1u << 15)
	uint32_t ext = e1000e_read(dev, E1000_CTRL_EXT);
	e1000e_write(dev, E1000_CTRL_EXT, ext | E1000_CTRL_EXT_SPD_BYPS);
	(void)e1000e_read(dev, E1000_STATUS); // posted-write flush
	e1000e_delay_us(20);

	uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
	e1000e_write(dev, E1000_CTRL, ctrl); // rewrite triggers the resync
	(void)e1000e_read(dev, E1000_STATUS);
	e1000e_delay_us(20);

	e1000e_write(dev, E1000_CTRL_EXT, ext & ~E1000_CTRL_EXT_SPD_BYPS);

#if E1000E_DBG
	uint16_t after = 0;
	e1000e_kmrn_read(dev, E1000_KMRN_K1_CONFIG_OFFSET, &after);
#endif
	e1000e_dbg("E1000E: K1 disabled (KMRN[0x07]: %04x -> %04x)\n", before,
		   after);
}

// Configure the MAC↔PHY Kumeran interconnect for 10/100-Mbps timing.
//
// On PCH-LAN parts the MAC and PHY talk over an internal serial link
// ("Kumeran") whose RX clock domain timing differs between 10/100 and
// 1000 Mbps.  Out of reset the HD_CTRL register at KMRN offset 0x10
// holds whatever default the silicon picked — which for many
// SPT/CNP/ICP I219 steppings is the 1000-Mbps value (0x0000) even when
// the PHY auto-negotiates down to 100 Mbps.  In that mismatched state
// the MAC's RX deserializer rejects every nibble it sees from the PHY,
// so STATUS.LU is asserted, the PHY shows clean RX activity, but TPR
// stays at 0 and not a single frame ever reaches the descriptor ring.
//
// We fix this by configuring KMRN HD_CTRL for the resolved 10/100 timing.
// The HD_CTRL bits 2:0 carry the inter-packet-gap timing; 0x4 is the
// documented "10/100" default, 0x0 is the "1000" default.  We additionally
// keep TIPG.IPGT at the canonical 10/100 value (it is harmless at full-
// duplex but required at half-duplex which the spec says we must still
// support).
// TIPG is programmed once globally in e1000e_init_tx() with the canonical
// PCH-LAN value (IPGT=8, IPGR1=8, IPGR2=6 = 0x00602008).  The KMRN helpers
// only touch the KMRN HD_CTRL register's per-speed inter-packet-gap field.
// A previous version of this file forced TIPG.IPGT to 0xFF here on the
// (incorrect) belief that it was the canonical 10/100 value; in fact
// IPGT=0xFF parks the MAC's per-packet IPG timer for so long that the TX
// scheduler never dispatches a frame even though the descriptor is in
// the on-chip cache — hardware reports ICR.TXQE while TDH stays at 0
// and TDFH never advances past TDFHS, exactly the symptom we observed
// on real I219-LM hardware.
static void e1000e_configure_kmrn_for_10_100(e1000e_dev_t *dev)
{
	uint16_t hd = 0;
	e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &hd);
#if E1000E_DBG
	uint16_t before = hd;
#endif
	hd &= ~0x0007u; // clear inter-packet-gap field
	hd |= E1000_KMRN_HD_CTRL_10_100_DEFAULT;
	e1000e_kmrn_write(dev, E1000_KMRN_HD_CTRL_OFFSET, hd);

#if E1000E_DBG
	uint16_t after = 0;
	e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &after);
#endif
	e1000e_dbg(
		"E1000E: KMRN HD_CTRL for 10/100 (KMRN[0x10]: %04x -> %04x)\n",
		before, after);
}

static void e1000e_configure_kmrn_for_1000(e1000e_dev_t *dev)
{
	uint16_t hd = 0;
	e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &hd);
#if E1000E_DBG
	uint16_t before = hd;
#endif
	hd &= ~0x0007u;
	hd |= E1000_KMRN_HD_CTRL_1000_DEFAULT;
	e1000e_kmrn_write(dev, E1000_KMRN_HD_CTRL_OFFSET, hd);

#if E1000E_DBG
	uint16_t after = 0;
	e1000e_kmrn_read(dev, E1000_KMRN_HD_CTRL_OFFSET, &after);
#endif
	e1000e_dbg("E1000E: KMRN HD_CTRL for 1000 (KMRN[0x10]: %04x -> %04x)\n",
		   before, after);
}

// ============================================================================
// EEPROM Read
// ============================================================================
/* EERD on the 8257x / 80003 / 82574 / 82583 parts.  Their layout is not the
 * 8254x one in e1000.h: the word address starts at bit 2 (not 8) and DONE
 * is bit 1 (not 4); data is bits 31:16 in both.  With the 8254x layout the
 * read asked for the wrong word and never saw DONE.
 *
 * Never on ICH8-10 or PCH parts: they have no EERD at all -- offset 0x14
 * there is FEXTNVM5, a PHY/power configuration register, which every MAC
 * read used to overwrite three times.  Their NVM is the platform flash,
 * and the MAC address it holds is already loaded into RAL0/RAH0.
 *
 * Returns 0 and the word, or -1 when the read did not complete. */
#define E1000E_EERD_START 0x00000001u
#define E1000E_EERD_DONE 0x00000002u
#define E1000E_EERD_ADDR_SHIFT 2
#define E1000E_EERD_DATA_SHIFT 16

static int e1000e_read_eeprom(e1000e_dev_t *dev, uint8_t addr, uint16_t *out)
{
	if (WARN_ON_ONCE(e1000e_is_pch_lan(dev->device_id)))
		return -1;
	e1000e_write(dev, E1000_EERD,
		     ((uint32_t)addr << E1000E_EERD_ADDR_SHIFT) |
			     E1000E_EERD_START);
	for (int i = 0; i < 100000; i++) {
		uint32_t val = e1000e_read(dev, E1000_EERD);
		if (val & E1000E_EERD_DONE) {
			*out = (uint16_t)(val >> E1000E_EERD_DATA_SHIFT);
			return 0;
		}
		e1000e_delay_us(5);
	}
	return -1;
}

// ============================================================================
// Read MAC Address
// ============================================================================
static void e1000e_read_mac(e1000e_dev_t *dev)
{
	uint16_t w0 = 0, w1 = 0, w2 = 0;
	int nvm_ok = !e1000e_is_pch_lan(dev->device_id) &&
		     e1000e_read_eeprom(dev, 0, &w0) == 0 &&
		     e1000e_read_eeprom(dev, 1, &w1) == 0 &&
		     e1000e_read_eeprom(dev, 2, &w2) == 0;

	if (nvm_ok && (w0 != 0 || w1 != 0 || w2 != 0) &&
	    !(w0 == 0xFFFF && w1 == 0xFFFF && w2 == 0xFFFF)) {
		dev->mac_addr[0] = w0 & 0xFF;
		dev->mac_addr[1] = (w0 >> 8) & 0xFF;
		dev->mac_addr[2] = w1 & 0xFF;
		dev->mac_addr[3] = (w1 >> 8) & 0xFF;
		dev->mac_addr[4] = w2 & 0xFF;
		dev->mac_addr[5] = (w2 >> 8) & 0xFF;
	} else {
		// RAL/RAH, loaded from the NVM by the hardware (firmware on PCH)
		uint32_t ral = e1000e_read(dev, E1000_RAL);
		uint32_t rah = e1000e_read(dev, E1000_RAH);
		dev->mac_addr[0] = ral & 0xFF;
		dev->mac_addr[1] = (ral >> 8) & 0xFF;
		dev->mac_addr[2] = (ral >> 16) & 0xFF;
		dev->mac_addr[3] = (ral >> 24) & 0xFF;
		dev->mac_addr[4] = rah & 0xFF;
		dev->mac_addr[5] = (rah >> 8) & 0xFF;
	}
}

// ============================================================================
// PM Timer-based microsecond delay
// ============================================================================
static void e1000e_delay_us(uint32_t us)
{
	uint32_t t0 = timer_pmtimer_read_raw();
	if (t0 == 0) {
		for (volatile uint32_t i = 0; i < us * 4; i++)
			__asm__ volatile("pause");
		return;
	}
	while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < us)
		__asm__ volatile("pause");
}

/* E1000E_CACHED_DMA_BUFFERS: leave the RX/TX packet buffers write-back
 * mapped where the device's DMA is cache-coherent -- PCIe snooped, which
 * every 8257x/82583 does and the SPT and later PCH-LAN MACs do with GCR's
 * no-snoop requests cleared (e1000e_pch_lan_init).  The copy out of an
 * uncached receive buffer costs an uncached read per word on every frame.
 * Older ICH/PCH LAN parts (ICH8..LPT) keep uncached buffers.  The
 * descriptor rings are uncached in every case: the CPU and the device
 * update them word by word.  0 restores uncached buffers everywhere. */
#define E1000E_CACHED_DMA_BUFFERS 1

static int e1000e_buffers_cacheable(uint16_t did)
{
	if (!E1000E_CACHED_DMA_BUFFERS)
		return 0;
	if (!e1000e_is_pch_lan(did))
		return 1;
	return e1000e_mac_gen(did) >= E1000E_MAC_PCH_SPT;
}

static void e1000e_make_dma_pages_uncached(uint64_t phys_base,
					   uint32_t page_count, const char *tag)
{
	(void)tag;
	if (!phys_base || page_count == 0) {
		return;
	}

	uint64_t phys_last = phys_base + ((uint64_t)page_count - 1) * PAGE_SIZE;
	if (!is_phys_in_direct_map(phys_last)) {
		e1000e_dbg(
			"E1000E: DMA region %s outside direct map (%llx..%llx)\n",
			tag, (unsigned long long)phys_base,
			(unsigned long long)phys_last);
		return;
	}

	uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_WRITE_THROUGH |
			 PAGE_CACHE_DISABLE | PAGE_GLOBAL | PAGE_NO_EXECUTE;
	uint64_t virt_base = (uint64_t)phys_to_virt(phys_base);

	for (uint32_t page = 0; page < page_count; page++) {
		uint64_t page_phys = phys_base + (uint64_t)page * PAGE_SIZE;
		uint64_t page_virt = virt_base + (uint64_t)page * PAGE_SIZE;
		mm_map_page(page_virt, page_phys, flags);
	}

	if (smp_is_enabled()) {
		smp_tlb_shootdown_sync();
	}

	/*e1000e_dbg("E1000E: DMA region %s remapped UC (%llx..%llx)\n",
            tag,
            (unsigned long long)phys_base,
            (unsigned long long)phys_last);*/
}

// ============================================================================
// Soft Reset
// ============================================================================
static void e1000e_reset(e1000e_dev_t *dev)
{
	// Mask + clear all interrupts
	e1000e_write(dev, E1000_IMC, 0xFFFFFFFF);
	(void)e1000e_read(dev, E1000_ICR);

	// Soft reset of the data path only — leave PHY/link untouched, the UEFI
	// firmware already negotiated.  A full CTRL.RST on e1000e parts can take many
	// seconds for link-up under hypervisors and breaks RX in some
	// some hypervisor builds, exactly as on the older e1000.
	e1000e_write(dev, E1000_RCTL, 0);
	e1000e_write(dev, E1000_TCTL, 0);

	e1000e_delay_us(10000);

	e1000e_write(dev, E1000_IMC, 0xFFFFFFFF);
	(void)e1000e_read(dev, E1000_ICR);
}

// ============================================================================
// RX Ring Initialization
// ============================================================================
static int e1000e_init_rx(e1000e_dev_t *dev)
{
	uint32_t rx_ring_size =
		sizeof(e1000_rx_desc_legacy_t) * E1000E_NUM_RX_DESC;
	uint32_t rx_ring_pages = (rx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t rx_phys = mm_allocate_contiguous_pages(rx_ring_pages);
	if (!rx_phys) {
		kprintf("E1000E: Failed to allocate RX descriptor ring\n");
		return -1;
	}

	dev->rx_descs = (e1000_rx_desc_legacy_t *)phys_to_virt(rx_phys);
	dev->rx_descs_phys = rx_phys;
	e1000e_make_dma_pages_uncached(rx_phys, rx_ring_pages, "rx-desc");

	for (uint32_t i = 0; i < rx_ring_size / 8; i++)
		((uint64_t *)dev->rx_descs)[i] = 0;

	for (int i = 0; i < E1000E_NUM_RX_DESC; i++) {
		uint64_t buf_phys = mm_allocate_physical_page();
		if (!buf_phys) {
			kprintf("E1000E: Failed to allocate RX buffer %d\n", i);
			return -1;
		}
		dev->rx_bufs[i] = (uint8_t *)phys_to_virt(buf_phys);
		dev->rx_bufs_phys[i] = buf_phys;
		if (!dev->buffers_cached)
			e1000e_make_dma_pages_uncached(buf_phys, 1, "rx-buf");

		dev->rx_descs[i].buffer_addr = buf_phys;
		dev->rx_descs[i].status = 0;
	}

	e1000e_write(dev, E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
	e1000e_write(dev, E1000_RDBAH, (uint32_t)(rx_phys >> 32));
	e1000e_write(dev, E1000_RDLEN, rx_ring_size);
	e1000e_write(dev, E1000_RDH, 0);
	e1000e_write(dev, E1000_RDT, E1000E_NUM_RX_DESC - 1);

	// Enable the per-queue RX descriptor fetch engine.  On all
	// post-ICH8 Intel parts (82571/2/4, 82576+, ICH9/10, PCH-LAN) the
	// legacy RCTL.EN bit only enables the MAC's RX framer — the actual
	// descriptor-DMA engine is gated by RXDCTL.ENABLE (bit 25), which
	// resets to 0.  Forgetting this bit is the textbook "link UP, RCTL
	// configured, but RDH never advances and TPR stays 0" failure mode
	// on I218/I219.  Intel datasheet 13.5.2: must be set BEFORE
	// RCTL.EN.  We also bump GRAN (bit 24, 1=descriptors not cache
	// lines) and leave the prefetch/host/writeback thresholds at their
	// hardware defaults which the I218/I219 datasheet considers sane.
	{
		uint32_t rxdctl = e1000e_read(dev, E1000_RXDCTL);
		rxdctl |= (1u << 24) // GRAN: thresholds in descriptors
			  | (1u << 25); // ENABLE
		e1000e_write(dev, E1000_RXDCTL, rxdctl);
	}

	/* Receive checksum offload: the MAC verifies IPv4 header and TCP/UDP
	 * checksums and reports the result in each descriptor (e1000e_rx). */
	e1000e_write(dev, E1000_RXCSUM,
		     e1000e_read(dev, E1000_RXCSUM) | E1000_RXCSUM_IPOFL |
			     E1000_RXCSUM_TUOFL);

	/* Interrupt moderation: no per-frame receive delay (RDTR 0, so the
	 * absolute delay RADV is idle too) and the interrupt rate capped by
	 * ITR, starting at 20000/s; the NAPI poll adapts it to the traffic
	 * (e1000e_update_itr). */
	e1000e_write(dev, E1000_RDTR, 0);
	e1000e_write(dev, E1000_RADV, 8);
	dev->itr_rate = NIC_ITR_RATE_LOW;
	dev->itr_rx_class = dev->itr_tx_class = NIC_ITR_LOW;
	e1000e_write(dev, E1000_ITR, nic_itr_reg(dev->itr_rate));

	uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 |
			E1000_RCTL_SECRC;
	e1000e_write(dev, E1000_RCTL, rctl);

	dev->rx_tail = E1000E_NUM_RX_DESC - 1;
	dev->rx_discarding = 0;
	return 0;
}

// ============================================================================
// TX Ring Initialization
// ============================================================================
static int e1000e_init_tx(e1000e_dev_t *dev)
{
	uint32_t tx_ring_size =
		sizeof(e1000_tx_desc_legacy_t) * E1000E_NUM_TX_DESC;
	uint32_t tx_ring_pages = (tx_ring_size + PAGE_SIZE - 1) / PAGE_SIZE;

	uint64_t tx_phys = mm_allocate_contiguous_pages(tx_ring_pages);
	if (!tx_phys) {
		kprintf("E1000E: Failed to allocate TX descriptor ring\n");
		return -1;
	}

	dev->tx_descs = (e1000_tx_desc_legacy_t *)phys_to_virt(tx_phys);
	dev->tx_descs_phys = tx_phys;
	e1000e_make_dma_pages_uncached(tx_phys, tx_ring_pages, "tx-desc");

	for (uint32_t i = 0; i < tx_ring_size / 8; i++)
		((uint64_t *)dev->tx_descs)[i] = 0;

	for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
		uint64_t buf_phys = mm_allocate_physical_page();
		if (!buf_phys) {
			kprintf("E1000E: Failed to allocate TX buffer %d\n", i);
			return -1;
		}
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(buf_phys);
		dev->tx_bufs_phys[i] = buf_phys;
		dev->tx_stamp[i] = 0;
		if (!dev->buffers_cached)
			e1000e_make_dma_pages_uncached(buf_phys, 1, "tx-buf");
	}

	e1000e_write(dev, E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
	e1000e_write(dev, E1000_TDBAH, (uint32_t)(tx_phys >> 32));
	e1000e_write(dev, E1000_TDLEN, tx_ring_size);
	e1000e_write(dev, E1000_TDH, 0);
	e1000e_write(dev, E1000_TDT, 0);

	// TIPG: canonical PCH-LAN value.
	//   IPGT  = 8   (bits  9:0 ) — inter-packet-gap transmit time
	//   IPGR1 = 8   (bits 19:10) — non-back-to-back IPG part 1
	//   IPGR2 = 6   (bits 29:20) — non-back-to-back IPG part 2
	// Per the I218/I219 datasheet, these exact values apply for PCH-LAN
	// regardless of negotiated speed.  IPGT must NOT be 0xFF — that parks
	// the IPG timer effectively forever, causing TXQE to assert and TDH
	// to never advance even with a valid descriptor cached on-chip.
	e1000e_write(dev, E1000_TIPG, (8u << 0) | (8u << 10) | (6u << 20));
	// TIDV — Transmit Interrupt Delay Value.  Use the canonical PCH-LAN
	// default of 8 (units of 1.024us, ~8.2us).  This batches Tx-completion
	// interrupts a little so we don't take one IRQ per packet under load,
	// matching what working hardware ground-truth shows on PCH-LAN.  Pure
	// latency tuning — does not gate TX.
	e1000e_write(dev, E1000_TIDV, 8);
	// TADV bounds the delay of a TXDW behind a stream of frames that
	// keep restarting TIDV (descriptors carry IDE: e1000e_xmit).
	e1000e_write(dev, E1000_TADV, 32);

	// TXDCTL — use the canonical PCH-LAN values from the datasheet:
	//
	//   txdctl = (txdctl & ~TXDCTL_WTHRESH) | TXDCTL_FULL_TX_DESC_WB
	//   txdctl = (txdctl & ~TXDCTL_PTHRESH) | TXDCTL_MAX_TX_DESC_PREFETCH
	// with TXDCTL_FULL_TX_DESC_WB     = 0x01010000 (GRAN=1, WTHRESH=1)
	//      TXDCTL_MAX_TX_DESC_PREFETCH = 0x0100001F (GRAN=1, PTHRESH=31)
	// and the hardware-init bit programming additionally requires BIT(22).
	//
	// Final value: 0x0141001F.
	//
	// Earlier we ORed in BIT(25) believing it was a per-queue ENABLE bit
	// mirroring i350-family RXDCTL/TXDCTL.  PCH-LAN has no per-queue ENABLE;
	// TX is enabled exclusively by TCTL.EN.  Bit 25 on PCH-LAN belongs to
	// the LWTHRESH (low writeback-threshold) field, and writing 1 there
	// configures a Tx writeback policy that the datasheet explicitly does
	// not recommend.  This brings us back to the canonical PCH-LAN init
	// values byte-for-byte.
	{
		uint32_t txdctl = e1000e_read(dev, E1000_TXDCTL);
		txdctl &= ~(E1000_TXDCTL_PTHRESH | E1000_TXDCTL_WTHRESH);
		txdctl |=
			0x01010000u // FULL_TX_DESC_WB    (GRAN=1, WTHRESH=1)
			|
			0x0100001Fu // MAX_TX_DESC_PREFETCH (GRAN=1, PTHRESH=31)
			| (1u
			   << 22); // datasheet hardware-init: TXDCTL |= BIT(22)
		e1000e_write(dev, E1000_TXDCTL, txdctl);

		// Mirror the same TXDCTL programming into queue 1 so the shared
		// TX arbitration block comes up in a known-good state.
		uint32_t txdctl1 = e1000e_read(dev, E1000_TXDCTL1);
		txdctl1 &= ~(E1000_TXDCTL_PTHRESH | E1000_TXDCTL_WTHRESH);
		txdctl1 |= 0x01010000u | 0x0100001Fu | (1u << 22);
		e1000e_write(dev, E1000_TXDCTL1, txdctl1);

		e1000e_dbg(
			"E1000E: TXDCTL0=%08x TXDCTL1=%08x (PCH-LAN canonical)\n",
			e1000e_read(dev, E1000_TXDCTL),
			e1000e_read(dev, E1000_TXDCTL1));
	}

	uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
			(15 << E1000_TCTL_CT_SHIFT) | E1000_TCTL_RTLC;
	e1000e_write(dev, E1000_TCTL, tctl);

	// Generic collision-distance config: use the default 63-slot COLD value
	// and force the write out before continuing.
	tctl = e1000e_read(dev, E1000_TCTL);
	tctl &= ~E1000_TCTL_COLD;
	tctl |= (63u << E1000_TCTL_COLD_SHIFT);
	e1000e_write(dev, E1000_TCTL, tctl);
	(void)e1000e_read(dev, E1000_TCTL);

	// CRITICAL on PCH-LAN (I217/I218/I219): enable MULR (Multiple Request,
	// bit 28) on TCTL.  Without it the TX engine issues a single descriptor
	// read request and waits forever for a completion-gating event that
	// never arrives on PCH-SPT, exactly matching the symptom we see on the
	// ThinkPad P50 / I219-LM: TDFH advances by 16 bytes (one descriptor's
	// worth fetched), then TDH/GPTC/TPT stay at 0 with no errors.
	//
	// Working register dumps from healthy hardware on the same board show
	// TCTL=0x3103F0FA — bit 28 (MULR) AND bit 29 (RRTHRESH low bit) set.
	// RRTHRESH is the hardware-default outstanding-read-request threshold.
	// We set both explicitly to match the known-good configuration byte-for-
	// byte.  We already disabled the buggy MULR-fix in FEXTNVM11 above, so
	// MULR is safe.
	if (e1000e_is_pch_lan(dev->device_id)) {
		tctl = e1000e_read(dev, E1000_TCTL);
#if E1000E_DBG
		uint32_t before = tctl;
#endif
		tctl |= (1u << 28) // MULR — Multiple Request enable
			| (1u << 29); // RRTHRESH[0] — match HW default
		e1000e_write(dev, E1000_TCTL, tctl);
		(void)e1000e_read(dev, E1000_TCTL);
#if E1000E_DBG
		e1000e_dbg("E1000E: TCTL %08x -> %08x (MULR + RRTHRESH set)\n",
			   before, e1000e_read(dev, E1000_TCTL));
#endif
	}

	// FEXTNVM11.DISABLE_MULR_FIX applies to every SPT-or-later MAC; the
	// IOSFPC data-corruption and TARC0 3->2 outstanding-request Tx-hang
	// errata apply to SPT MACs only.  Register write order is unchanged.
	enum e1000e_mac_gen mac_gen = e1000e_mac_gen(dev->device_id);
	if (mac_gen >= E1000E_MAC_PCH_SPT) {
		uint32_t iosfpc = e1000e_read(dev, E1000_IOSFPC);
		uint32_t fextnvm11 = e1000e_read(dev, E1000_FEXTNVM11);
		uint32_t tarc0 = e1000e_read(dev, E1000_TARC0);

		if (mac_gen == E1000E_MAC_PCH_SPT) {
			iosfpc |= (1u << 16);
			e1000e_write(dev, E1000_IOSFPC, iosfpc);
		}

		fextnvm11 |= E1000_FEXTNVM11_DISABLE_MULR_FIX;
		e1000e_write(dev, E1000_FEXTNVM11, fextnvm11);

		if (mac_gen == E1000E_MAC_PCH_SPT) {
			tarc0 &= ~E1000_TARC0_CB_MULTIQ_3_REQ;
			tarc0 |= E1000_TARC0_CB_MULTIQ_2_REQ;
			e1000e_write(dev, E1000_TARC0, tarc0);
		}

		e1000e_dbg(
			"E1000E: SPT+ tx-hang WA IOSFPC=%08x FEXTNVM11=%08x TARC0=%08x\n",
			e1000e_read(dev, E1000_IOSFPC),
			e1000e_read(dev, E1000_FEXTNVM11),
			e1000e_read(dev, E1000_TARC0));
	}

	dev->tx_tail = 0;
	dev->tx_clean = 0;
	return 0;
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================
/* Wake a stopped transmit queue once this many descriptors are free. */
#define E1000E_TX_WAKE_THRESH 32
#define MII_LPA 0x05 // link partner ability

/* Interrupt causes the NAPI poll owns (masked while it is scheduled), and
 * every cause the driver uses. */
#define E1000E_IMS_NAPI                                       \
	(E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_RXDMT0 | \
	 E1000_ICR_RXO)
#define E1000E_IMS_ALL (E1000E_IMS_NAPI | E1000_ICR_LSC)

static uint32_t e1000e_tx_unused(const e1000e_dev_t *dev)
{
	return nic_ring_unused(dev->tx_tail, dev->tx_clean, E1000E_NUM_TX_DESC);
}

/* Reclaim the descriptors the hardware has written back (DD), oldest first.
 * The packet bytes were copied into the ring's own buffers at transmit, so
 * nothing else is released.  Caller holds tx_lock. */
static uint32_t e1000e_tx_clean_locked(e1000e_dev_t *dev)
{
	uint32_t n = 0;

	while (dev->tx_clean != dev->tx_tail) {
		volatile e1000_tx_desc_legacy_t *d =
			&dev->tx_descs[dev->tx_clean];
		uint8_t st = d->status;

		if (!(st & E1000_TXD_STAT_DD))
			break;
		if (st & (E1000_TXD_STAT_EC | E1000_TXD_STAT_LC))
			dev->net_dev.tx_errors++;
		dev->itr_tx_packets++;
		dev->itr_tx_bytes += d->length;
		d->cmd = 0;
		d->status = 0;
		dev->tx_stamp[dev->tx_clean] = 0;
		dev->tx_clean = (uint16_t)((dev->tx_clean + 1) %
					   E1000E_NUM_TX_DESC);
		n++;
	}
	return n;
}

/* Wake the queue if the transmit hook stopped it and room is back.  Caller
 * holds tx_lock, so a stop cannot slip in between the check and the wake. */
static void e1000e_tx_maybe_wake_locked(e1000e_dev_t *dev)
{
	if (netif_queue_stopped(&dev->net_dev) &&
	    e1000e_tx_unused(dev) >= E1000E_TX_WAKE_THRESH)
		netif_wake_queue(&dev->net_dev);
}

/* Hand one frame to the hardware.  Called by the device core one CPU at a
 * time (qdisc_run); tx_lock orders it against the poll's reclaim and the
 * watchdog's ring reset on CPU 0.  A full ring stops the queue and answers
 * NETDEV_TX_BUSY: the core keeps the frame and the reclaim wakes the queue
 * again. */
static int e1000e_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;
	uint16_t i;

	BUG_ON(dev == NULL);
	BUILD_BUG_ON(sizeof(e1000_rx_desc_legacy_t) != 16);
	BUILD_BUG_ON(sizeof(e1000_tx_desc_legacy_t) != 16);
	if (len == 0 || len > E1000E_RX_BUF_SIZE) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&dev->tx_lock, &f);
	if (e1000e_tx_unused(dev) == 0) {
		e1000e_tx_clean_locked(dev);
		if (e1000e_tx_unused(dev) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}

	i = dev->tx_tail;
	volatile e1000_tx_desc_legacy_t *desc = &dev->tx_descs[i];

	mm_memcpy(dev->tx_bufs[i], skb->data, len);
	desc->buffer_addr = dev->tx_bufs_phys[i];
	desc->length = (uint16_t)len;
	desc->cso = 0;
	desc->status = 0;
	desc->css = 0;
	desc->special = 0;
	desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS |
		    E1000_TXD_CMD_IDE;
	{
		uint64_t now = timer_ticks();

		dev->tx_stamp[i] = now ? now : 1;
	}

	// The buffer and descriptor before the doorbell.
	__asm__ volatile("mfence" ::: "memory");

	dev->tx_tail = (uint16_t)((i + 1) % E1000E_NUM_TX_DESC);
	e1000e_write(dev, E1000_TDT, dev->tx_tail);

	ndev->tx_packets++;
	ndev->tx_bytes += len;
	if (e1000e_tx_unused(dev) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

// ============================================================================
// Link Status (net_device_t callback)
// ============================================================================
//
// Cable hot-plug support.  When the cable is unplugged, the PHY drops
// link and the MAC's LSC interrupt fires; STATUS.LU goes to 0.  When
// the cable is replugged, the PHY runs a fresh autoneg cycle and on
// PCH-LAN parts the post-autoneg state may re-apply LPLU / GBE_DIS
// (the OEM NVM defaults on Lenovo ThinkPad NVMs are LPLU=1, GBE_DIS=1)
// — even though we cleared those bits at init time.  LPLU=1 with link
// UP throttles the MII data path: the MAC fetches the TX descriptor
// but the buffer payload is never pulled across the MAC<->PHY interface,
// so TX appears to "succeed" while no frame ever leaves the wire.
//
// We must therefore re-clear the LPLU / GBE_DIS bits on every observed
// link-down -> link-up edge, and redo the configuration that depends on
// the speed the link came up at.  This is too heavy to do from the LSC
// IRQ handler (the PHY writes need the SW_FLAG semaphore, paged PHY
// access, and ms-scale delays), so the IRQ only sets a pending flag and
// kicks the link watchdog (network housekeeping softirq, CPU 0).

/* After the link went down, or when the transmit unit hangs, transmit
 * descriptors the MAC had not yet sent stay owned by it: the ring fills
 * and nothing more goes out.  Throw them away and restart the ring empty,
 * touching only the transmit queue (TCTL.EN off, head and tail to 0, TCTL
 * restored), nothing of the PHY or the rest of the MAC. */
static void e1000e_tx_ring_reset(e1000e_dev_t *dev)
{
	uint64_t txflags;
	uint32_t pending;

	spin_lock_irqsave(&dev->tx_lock, &txflags);
	e1000e_tx_clean_locked(dev);
	pending = (uint32_t)((dev->tx_tail + E1000E_NUM_TX_DESC -
			      dev->tx_clean) %
			     E1000E_NUM_TX_DESC);
	if (!pending) {
		spin_unlock_irqrestore(&dev->tx_lock, txflags);
		return;
	}
	uint32_t tctl = e1000e_read(dev, E1000_TCTL);
	e1000e_write(dev, E1000_TCTL, tctl & ~E1000_TCTL_EN);
	(void)e1000e_read(dev, E1000_STATUS); // posting flush
	e1000e_delay_us(100);
	for (int i = 0; i < E1000E_NUM_TX_DESC; i++) {
		volatile e1000_tx_desc_legacy_t *d = &dev->tx_descs[i];

		d->cmd = 0;
		d->status = 0;
		d->length = 0;
		dev->tx_stamp[i] = 0;
	}
	__asm__ volatile("mfence" ::: "memory");
	dev->tx_tail = 0;
	dev->tx_clean = 0;
	e1000e_write(dev, E1000_TDH, 0);
	e1000e_write(dev, E1000_TDT, 0);
	e1000e_write(dev, E1000_TCTL, tctl);
	dev->net_dev.tx_errors += pending;
	e1000e_tx_maybe_wake_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, txflags);
}

/* A K1 change takes effect only after the MAC re-syncs its clocks to the
 * Kumeran interface: speed forced (FRCSPD, speed bits clear) with the
 * speed bypass set for a moment, then CTRL and CTRL_EXT restored.  Caller
 * holds the PHY semaphore when it got it (the Kumeran register is shared
 * with the firmware). */
static void e1000e_configure_k1_locked(e1000e_dev_t *dev, int enable)
{
	uint16_t k1 = 0;

	e1000e_kmrn_read(dev, E1000_KMRN_K1_CONFIG_OFFSET, &k1);
	if (enable)
		k1 |= E1000_KMRN_K1_ENABLE;
	else
		k1 &= ~E1000_KMRN_K1_ENABLE;
	e1000e_kmrn_write(dev, E1000_KMRN_K1_CONFIG_OFFSET, k1);
	e1000e_delay_us(20);

	uint32_t ext = e1000e_read(dev, E1000_CTRL_EXT);
	uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
	uint32_t forced = (ctrl & ~(E1000_CTRL_SPD_1000 | E1000_CTRL_SPD_100)) |
			  E1000_CTRL_FRCSPD;

	e1000e_write(dev, E1000_CTRL, forced);
	e1000e_write(dev, E1000_CTRL_EXT, ext | E1000_CTRL_EXT_SPD_BYPS);
	(void)e1000e_read(dev, E1000_STATUS);
	e1000e_delay_us(20);
	e1000e_write(dev, E1000_CTRL, ctrl);
	e1000e_write(dev, E1000_CTRL_EXT, ext);
	(void)e1000e_read(dev, E1000_STATUS);
	e1000e_delay_us(20);
}

static uint32_t e1000e_status_speed(uint32_t status)
{
	switch ((status >> 6) & 0x3) {
	case 0:
		return 10;
	case 1:
		return 100;
	default:
		return 1000;
	}
}

/* The Kumeran timing for the speed the link came up at, and on the parts
 * this driver was brought up on (ThinkPad P50, SPT I219) K1 per speed at
 * full duplex: on at 1000 Mb/s, off with the PLL clock request held at
 * 10/100.  Run at every link-up -- the speed can differ each time. */
static void e1000e_pch_link_timing(e1000e_dev_t *dev, uint32_t status)
{
	uint32_t speed = e1000e_status_speed(status);
	int held = e1000e_acquire_swflag(dev) == 0;

	if (speed == 1000)
		e1000e_configure_kmrn_for_1000(dev);
	else
		e1000e_configure_kmrn_for_10_100(dev);

	if (e1000e_has_10_100_k1_pll_quirk(dev->device_id) &&
	    (status & E1000_STATUS_LU) && (status & E1000_STATUS_FD)) {
		if (speed == 1000) {
			e1000e_configure_k1_locked(dev, 1);
		} else {
			e1000e_configure_k1_locked(dev, 0);
			e1000e_write(dev, E1000_FEXTNVM6,
				     e1000e_read(dev, E1000_FEXTNVM6) |
					     E1000_FEXTNVM6_REQ_PLL_CLK);
		}
	}
	if (held)
		e1000e_release_swflag(dev);
}

/* Latency Tolerance Reporting: with link, the time the receive packet
 * buffer can absorb at this speed, capped to the platform's maximum; with
 * no link, no requirement.  SPT and later. */
static void e1000e_set_ltr(e1000e_dev_t *dev, int link, uint32_t speed)
{
	uint32_t reg = E1000_LTRV_SEND;

	if (link) {
		const pci_device_t *p = dev->pci_dev;
		uint32_t cap = pci_cfg_read32(p->bus, p->device, p->function,
					      E1000_PCI_LTR_CAP_LPT);
		uint16_t max_snoop = (uint16_t)(cap & 0xFFFF);
		uint16_t max_nosnoop = (uint16_t)(cap >> 16);
		uint16_t pmax = max_snoop > max_nosnoop ? max_snoop :
							  max_nosnoop;
		uint16_t enc = nic_ltr_encode(e1000e_read(dev, E1000_PBA) &
						      0xFFFF,
					      E1000E_MAX_FRAME, speed, pmax);

		if (enc == 0xFFFF)
			return; // not encodable: leave what is programmed
		reg |= (1u << (E1000_LTRV_REQ_SHIFT +
			       E1000_LTRV_NOSNOOP_SHIFT)) |
		       (1u << E1000_LTRV_REQ_SHIFT) | enc |
		       ((uint32_t)enc << E1000_LTRV_NOSNOOP_SHIFT);
	}
	e1000e_write(dev, E1000_LTRV, reg);
}

/* What the SPT and later MACs need at every link-up, for the speed and
 * duplex the link came up at:
 *   - TIPG.IPGT 0xC at 10/100 full duplex, 8 otherwise (the half-duplex
 *     10 Mb/s 0xFF value is never used: IPGT 0xFF parks the transmitter)
 *   - the analog receive latency (EMI I217_RX_CONFIG)
 *   - the PLL clock gate count: 0x3E8 at 10/100, 0xFA at 1000
 *   - K1 clock request at 1000 Mb/s
 *   - the MAC/PHY pointer gap: at least 0x18 at 1000, 0xC023 at 10/100
 *   - LTR, the SPT K1-off mirror (PCIEANACFG -> FEXTNVM6)
 *   - flow control resolved from the autonegotiation (RFCE/TFCE).
 * Pre-SPT PCH parts and the 8257x keep their configuration. */
static void e1000e_spt_link_up(e1000e_dev_t *dev, uint32_t status)
{
	enum e1000e_mac_gen gen = e1000e_mac_gen(dev->device_id);
	uint32_t speed = e1000e_status_speed(status);
	int fd = (status & E1000_STATUS_FD) != 0;
	uint16_t anar = 0, lpa = 0;
	int fc_known = 0;

	if (gen < E1000E_MAC_PCH_SPT)
		return;

	uint32_t tipg = e1000e_read(dev, E1000_TIPG) & ~E1000_TIPG_IPGT_MASK;
	tipg |= (fd && speed != 1000) ? 0xCu : 0x8u;
	e1000e_write(dev, E1000_TIPG, tipg);

	if (e1000e_acquire_swflag(dev) == 0) {
		uint16_t v;

		(void)(e1000e_phy_write_paged_locked(dev, I82579_EMI_ADDR,
						     I217_RX_CONFIG) == 0 &&
		       e1000e_phy_write_paged_locked(dev, I82579_EMI_DATA,
						     (!fd && speed == 10) ? 0 :
									   1));
		if (e1000e_phy_read_paged_locked(dev, I217_PLL_CLOCK_GATE_REG,
						 &v) == 0) {
			v &= (uint16_t)~I217_PLL_CLOCK_GATE_MASK;
			v |= speed == 1000 ? 0xFA : 0x3E8;
			(void)e1000e_phy_write_paged_locked(
				dev, I217_PLL_CLOCK_GATE_REG, v);
		}
		if (speed == 1000 &&
		    e1000e_phy_read_paged_locked(dev, HV_PM_CTRL, &v) == 0)
			(void)e1000e_phy_write_paged_locked(
				dev, HV_PM_CTRL,
				(uint16_t)(v | HV_PM_CTRL_K1_CLK_REQ));
		if (speed == 1000) {
			if (e1000e_phy_read_paged_locked(dev, I219_PTR_GAP_REG,
							 &v) == 0 &&
			    ((v >> 2) & 0x3FF) < 0x18) {
				v &= (uint16_t)~(0x3FFu << 2);
				v |= 0x18u << 2;
				(void)e1000e_phy_write_paged_locked(
					dev, I219_PTR_GAP_REG, v);
			}
		} else {
			(void)e1000e_phy_write_paged_locked(
				dev, I219_PTR_GAP_REG, 0xC023);
		}
		fc_known = e1000e_phy_read_at(dev, E1000E_PHY_ADDR, MII_ANAR,
					      &anar) == 0 &&
			   e1000e_phy_read_at(dev, E1000E_PHY_ADDR,
					      MII_LPA, &lpa) == 0;
		e1000e_release_swflag(dev);
	}

	e1000e_set_ltr(dev, 1, speed);

	if (gen == E1000E_MAC_PCH_SPT) {
		uint32_t fx6 = e1000e_read(dev, E1000_FEXTNVM6);

		if (e1000e_read(dev, E1000_PCIEANACFG) &
		    E1000_FEXTNVM6_K1_OFF_ENABLE)
			fx6 |= E1000_FEXTNVM6_K1_OFF_ENABLE;
		else
			fx6 &= ~E1000_FEXTNVM6_K1_OFF_ENABLE;
		e1000e_write(dev, E1000_FEXTNVM6, fx6);
	}

	/* Flow control: what both ends advertised decides whether pause
	 * frames are honoured (RFCE) and sent (TFCE). */
	dev->fc_mode = fc_known ? nic_fc_resolve(anar, lpa, fd) : NIC_FC_NONE;
	uint32_t ctrl = e1000e_read(dev, E1000_CTRL) &
			~(E1000_CTRL_RFCE | E1000_CTRL_TFCE);
	if (dev->fc_mode & NIC_FC_RX)
		ctrl |= E1000_CTRL_RFCE;
	if (dev->fc_mode & NIC_FC_TX)
		ctrl |= E1000_CTRL_TFCE;
	e1000e_write(dev, E1000_CTRL, ctrl);
}

/* The one place link state changes are acted on.  The LSC interrupt only
 * flags that something changed (link_change_pending) and kicks the
 * watchdog.  Called from the watchdog (CPU 0) and from init, never from
 * hard-IRQ context; concurrent callers skip. */
static void e1000e_link_state_poll(e1000e_dev_t *dev)
{
	if (__atomic_exchange_n(&dev->link_busy, 1, __ATOMIC_ACQUIRE))
		return;

	uint32_t status = e1000e_read(dev, E1000_STATUS);
	int now_up = (status & E1000_STATUS_LU) ? 1 : 0;
	int was_up = dev->link_up;

	int pending = __atomic_exchange_n(&dev->link_change_pending, 0,
					  __ATOMIC_ACQ_REL);

	/* The LSC storm guard masked LSC a while ago: unmask it again once
	 * a second has passed. */
	if (dev->lsc_masked_tick &&
	    timer_ticks() - dev->lsc_masked_tick >= net_ms_to_ticks(1000)) {
		dev->lsc_masked_tick = 0;
		e1000e_write(dev, E1000_IMS, E1000_ICR_LSC);
	}

	// An edge: a changed STATUS.LU (also covers a lost or masked LSC), or
	// an LSC the interrupt saw -- a down/up bounce shorter than a poll.
	if (now_up == was_up && !pending)
		goto out;

	if (!now_up) {
		if (was_up) {
			dev->link_up = 0;
			dev->link_speed = 0;
			kprintf("E1000E: Link DOWN\n");
			netif_carrier_off(&dev->net_dev);
			/* Invalidate any DHCP lease: the network we get back
			 * may be a different one (cable moved, hypervisor
			 * switched bridged<->NAT), and a unicast renewal aimed
			 * at the old server would go nowhere. */
			dhcp_invalidate(&dev->net_dev);
		}
		// drop what the transmit ring can no longer send
		e1000e_tx_ring_reset(dev);
		if (e1000e_mac_gen(dev->device_id) >= E1000E_MAC_PCH_SPT)
			e1000e_set_ltr(dev, 0, 0);
		goto out;
	}

	/* Up, or down and up again since the last look.  The PHY just ran
	 * a fresh autoneg: re-clear the LPLU / GBE_DIS gates (without an
	 * autoneg restart -- the link is up) and redo what depends on the
	 * speed it came up at. */
	if (e1000e_is_pch_lan(dev->device_id)) {
		e1000e_clear_mac_phy_ctrl_lplu(dev);
		(void)e1000e_oem_bits_clear_lplu_ex(dev, 0);
		status = e1000e_read(dev, E1000_STATUS);
		if (status & E1000_STATUS_LU) {
			e1000e_pch_link_timing(dev, status);
			e1000e_spt_link_up(dev, status);
		}
	}
	dev->link_speed = e1000e_status_speed(status);
	dev->link_fd = (status & E1000_STATUS_FD) != 0;
	if (!was_up) {
		dev->link_up = 1;
		kprintf("E1000E: Link UP (%u Mb/s, %s duplex)\n",
			dev->link_speed, dev->link_fd ? "full" : "half");
	}
	netif_carrier_on(&dev->net_dev);
out:
	__atomic_store_n(&dev->link_busy, 0, __ATOMIC_RELEASE);
}

// ============================================================================
// Receive filter, statistics, watchdog
// ============================================================================

/* Program the multicast table and the promiscuous bits from the device's
 * multicast list and flags.  Watchdog context (CPU 0). */
static void e1000e_apply_rx_mode(e1000e_dev_t *dev)
{
	uint8_t mc[NETDEV_MC_MAX][6];
	uint32_t mta[128];
	int all = 0;
	int n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
	uint32_t flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);

	for (uint32_t r = 0; r < dev->mta_regs; r++)
		mta[r] = 0;
	for (int i = 0; i < n; i++) {
		uint32_t h = nic_mta_hash(mc[i], dev->mta_regs);

		mta[h >> 5] |= 1u << (h & 31);
	}
	for (uint32_t r = 0; r < dev->mta_regs; r++)
		e1000e_write(dev, E1000_MTA + r * 4, mta[r]);

	uint32_t rctl = e1000e_read(dev, E1000_RCTL) &
			~(E1000_RCTL_UPE | E1000_RCTL_MPE);
	if (flags & IFF_PROMISC)
		rctl |= E1000_RCTL_UPE | E1000_RCTL_MPE;
	else if (all || (flags & IFF_ALLMULTI))
		rctl |= E1000_RCTL_MPE;
	e1000e_write(dev, E1000_RCTL, rctl);
}

static void e1000e_set_rx_mode(net_device_t *ndev)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

/* Fold the hardware's clear-on-read error counters into ours. */
static void e1000e_update_stats(e1000e_dev_t *dev)
{
	dev->st_crc += e1000e_read(dev, E1000_ST_CRCERRS);
	dev->st_align += e1000e_read(dev, E1000_ST_ALGNERRC);
	dev->st_rxerr += e1000e_read(dev, E1000_ST_RXERRC);
	dev->st_missed += e1000e_read(dev, E1000_ST_MPC);
	dev->st_colls += e1000e_read(dev, E1000_ST_COLC);
	dev->st_latecol += e1000e_read(dev, E1000_ST_LATECOL);
	dev->st_ecol += e1000e_read(dev, E1000_ST_ECOL);
	dev->st_tncrs += e1000e_read(dev, E1000_ST_TNCRS);
	dev->st_rlen += e1000e_read(dev, E1000_ST_RLEC) +
			e1000e_read(dev, E1000_ST_RUC) +
			e1000e_read(dev, E1000_ST_ROC);
	dev->st_nobuf += e1000e_read(dev, E1000_ST_RNBC);
}

static void e1000e_get_stats64(net_device_t *ndev, struct net_device_stats64 *s)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;

	s->rx_crc_errors = dev->st_crc;
	s->rx_frame_errors = dev->st_align;
	s->rx_missed_errors = dev->st_missed;
	s->rx_length_errors = dev->st_rlen + dev->st_rx_frag;
	s->rx_fifo_errors = dev->st_nobuf;
	s->collisions = dev->st_colls;
	s->tx_aborted_errors = dev->st_ecol;
	s->tx_carrier_errors = dev->st_tncrs;
	s->rx_errors += dev->st_crc + dev->st_align + dev->st_rxerr +
			dev->st_rlen;
	s->tx_errors += dev->st_ecol + dev->st_latecol;
}

/* Transmit hang check: the oldest descriptor not done for longer than the
 * link speed allows, the link up and the transmitter not paused by flow
 * control.  The transmit ring is then restarted. */
static void e1000e_check_tx_hang(e1000e_dev_t *dev)
{
	static uint32_t reported;
	uint64_t now = timer_ticks(), f;
	uint32_t limit_ms;
	int hang = 0;

	if (!dev->link_up)
		return;
	limit_ms = dev->link_speed == 1000 ? 2000 :
		   dev->link_speed == 100  ? 10000 :
					     16000;
	spin_lock_irqsave(&dev->tx_lock, &f);
	if (dev->tx_clean != dev->tx_tail) {
		uint64_t stamp = dev->tx_stamp[dev->tx_clean];

		if (stamp && now - stamp > net_ms_to_ticks(limit_ms) &&
		    !(e1000e_read(dev, E1000_STATUS) & E1000_STATUS_TXOFF))
			hang = 1;
	}
	spin_unlock_irqrestore(&dev->tx_lock, f);
	if (!hang)
		return;
	dev->st_tx_hangs++;
	if (reported < 5) {
		reported++;
		kprintf("E1000E: transmit unit hang (TDH=%u TDT=%u), restarting the transmit ring\n",
			e1000e_read(dev, E1000_TDH),
			e1000e_read(dev, E1000_TDT));
	}
	e1000e_tx_ring_reset(dev);
}

static void e1000e_tx_timeout(net_device_t *ndev)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;

	dev->st_tx_hangs++;
	e1000e_tx_ring_reset(dev);
}

// net_device_t watchdog: ~10 times a second and right after an LSC
// interrupt (see net_housekeeping_softirq()), on CPU 0.
static void e1000e_watchdog(net_device_t *ndev)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;
	uint64_t f;

	if (!dev || !g_e1000e_initialized)
		return;
	e1000e_link_state_poll(dev);
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL))
		e1000e_apply_rx_mode(dev);
	/* A completion whose interrupt was lost still frees its descriptor
	 * and wakes a stopped queue. */
	spin_lock_irqsave(&dev->tx_lock, &f);
	e1000e_tx_clean_locked(dev);
	e1000e_tx_maybe_wake_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	e1000e_check_tx_hang(dev);
	if (timer_ticks() - dev->stats_tick >= net_ms_to_ticks(2000)) {
		dev->stats_tick = timer_ticks();
		e1000e_update_stats(dev);
	}
}

static int e1000e_link_status(net_device_t *ndev)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;
	// STATUS.LU is the ground truth; a change is handed to the watchdog.
	int up = (e1000e_read(dev, E1000_STATUS) & E1000_STATUS_LU) ? 1 : 0;

	if (up != dev->link_up) {
		__atomic_store_n(&dev->link_change_pending, 1,
				 __ATOMIC_RELEASE);
		net_housekeeping_kick();
	}
	return up;
}

// ============================================================================
// Receive (NAPI poll)
// ============================================================================

/* One good frame: copied out of the ring buffer into an skb, marked with
 * what the MAC found of its checksum, and passed up. */
static void e1000e_rx_deliver(e1000e_dev_t *dev, const uint8_t *buf,
			      uint32_t len, uint8_t status, uint8_t errors)
{
	net_device_t *nd = &dev->net_dev;
	sk_buff_t *skb = skb_alloc(len);
	int cs;

	if (!skb) {
		nd->rx_dropped++;
		return;
	}
	skb->dev = nd;
	mm_memcpy(skb_append(skb, len), buf, len);
	cs = nic_e1000_rx_csum(status, errors, 1, 1);
	if (cs == NIC_CSUM_OK && (nd->features & NETIF_F_RXCSUM))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	else if (cs == NIC_CSUM_BAD)
		dev->st_csum_err++;
	nd->rx_packets++;
	nd->rx_bytes += len;
	if (nic_is_multicast(buf))
		nd->multicast++;
	dev->itr_rx_packets++;
	dev->itr_rx_bytes += len;
	napi_gro_receive(&dev->napi, skb);
}

/* Clean up to `budget' completed receive descriptors, returning them to
 * the hardware in batches.  Only the poll calls this. */
static int e1000e_rx_clean(e1000e_dev_t *dev, int budget)
{
	uint16_t ntc = (uint16_t)((dev->rx_tail + 1) % E1000E_NUM_RX_DESC);
	uint16_t last = dev->rx_tail;
	int work = 0, refill = 0;

	while (work < budget) {
		volatile e1000_rx_desc_legacy_t *d = &dev->rx_descs[ntc];
		uint8_t status = d->status;

		if (!(status & E1000_RXD_STAT_DD))
			break;
		// Descriptors are uncached device memory: the fields below are
		// read after DD, in program order.
		uint8_t errors = d->errors;
		uint16_t len = d->length;

		switch (nic_e1000_rx_verdict(status, errors, len,
					     E1000E_RX_BUF_SIZE,
					     &dev->rx_discarding)) {
		case NIC_RXV_DELIVER:
			e1000e_rx_deliver(dev, dev->rx_bufs[ntc], len, status,
					  errors);
			break;
		case NIC_RXV_ERROR:
			dev->net_dev.rx_errors++;
			break;
		default:
			dev->st_rx_frag++;
			break;
		}

		d->status = 0;
		d->errors = 0;
		last = ntc;
		ntc = (uint16_t)((ntc + 1) % E1000E_NUM_RX_DESC);
		work++;
		if (++refill >= 32) {
			dev->rx_tail = last;
			e1000e_write(dev, E1000_RDT, last);
			refill = 0;
		}
	}
	if (refill) {
		dev->rx_tail = last;
		e1000e_write(dev, E1000_RDT, last);
	}
	return work;
}

/* Adaptive interrupt moderation from what the last interrupt period moved:
 * 4000/s below gigabit, 4000..20000/s at gigabit. */
static void e1000e_update_itr(e1000e_dev_t *dev, uint32_t txp, uint32_t txb)
{
	uint32_t target;

	if (dev->link_speed != 1000) {
		target = NIC_ITR_RATE_BULK;
	} else {
		int cls;

		dev->itr_rx_class = nic_itr_classify(dev->itr_rx_class,
						     dev->itr_rx_packets,
						     dev->itr_rx_bytes);
		dev->itr_tx_class =
			nic_itr_classify(dev->itr_tx_class, txp, txb);
		cls = dev->itr_rx_class > dev->itr_tx_class ?
			      dev->itr_rx_class :
			      dev->itr_tx_class;
		target = nic_itr_class_rate(cls, 1);
	}
	dev->itr_rx_packets = 0;
	dev->itr_rx_bytes = 0;
	if (target != dev->itr_rate) {
		dev->itr_rate = nic_itr_next_rate(dev->itr_rate, target);
		e1000e_write(dev, E1000_ITR, nic_itr_reg(dev->itr_rate));
	}
}

static int e1000e_poll(struct napi_struct *n, int budget)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)n->dev->driver_data;
	uint32_t txp, txb;
	uint64_t f;
	int work;

	spin_lock_irqsave(&dev->tx_lock, &f);
	e1000e_tx_clean_locked(dev);
	e1000e_tx_maybe_wake_locked(dev);
	// completions counted under tx_lock move to the poll's own total
	dev->itr_tx_acc_packets += dev->itr_tx_packets;
	dev->itr_tx_acc_bytes += dev->itr_tx_bytes;
	dev->itr_tx_packets = 0;
	dev->itr_tx_bytes = 0;
	spin_unlock_irqrestore(&dev->tx_lock, f);

	work = e1000e_rx_clean(dev, budget);
	if (work < budget) {
		txp = dev->itr_tx_acc_packets;
		txb = dev->itr_tx_acc_bytes;
		dev->itr_tx_acc_packets = 0;
		dev->itr_tx_acc_bytes = 0;
		e1000e_update_itr(dev, txp, txb);
		if (napi_complete_done(n, work))
			e1000e_write(dev, E1000_IMS, E1000E_IMS_NAPI);
	}
	return work;
}

// Quiesce the NIC ahead of an ACPI S5 transition.  PCH-LAN integrated
// I218 / I219 controllers will hold PME# asserted (and the platform
// will refuse to fully power down — screen black, fans off, but rail
// still hot) if any of the following are still live when SLP_EN is
// written:
//   * IMS — any unmasked interrupt cause
//   * RCTL.EN / TCTL.EN — receiver / transmitter still enabled
//   * WUC.APME / WUFC — Wake-on-LAN advertised to the platform
//   * PCI Command.BME — bus-master DMA still active
//   * CTRL_EXT.DRV_LOAD — OS-ownership flag asserted to the ME
// Per the I218/I219 datasheet, all of these must be cleared before the
// chipset will release the platform to G2/S5.
static void e1000e_shutdown(net_device_t *ndev)
{
	e1000e_dev_t *dev = (e1000e_dev_t *)ndev->driver_data;
	if (!dev || !dev->mmio_base)
		return;

	// Mask every interrupt cause and ack any pending ones.
	e1000e_write(dev, E1000_IMC, 0xFFFFFFFFu);
	(void)e1000e_read(dev, E1000_ICR);

	// Disable the receiver and transmitter so no further DMA descriptors
	// are fetched after this point.
	uint32_t rctl = e1000e_read(dev, E1000_RCTL);
	e1000e_write(dev, E1000_RCTL, rctl & ~E1000_RCTL_EN);
	uint32_t tctl = e1000e_read(dev, E1000_TCTL);
	e1000e_write(dev, E1000_TCTL, tctl & ~E1000_TCTL_EN);

	// Clear Wake-on-LAN: WUC.APME (offset 0x05800, bit 0) advertises
	// PME# capability to the platform; WUFC (0x05808) selects which
	// packet patterns assert the wake.  Zeroing both is mandatory for
	// a clean S5 on integrated PCH-LAN.
	e1000e_write(dev, 0x05808u /* WUFC */, 0);
	e1000e_write(dev, 0x05800u /* WUC  */, 0);

	// Tell the management engine we are no longer the owning driver.
	// Per the PCH-LAN spec, with DRV_LOAD deasserted the firmware will
	// run its own platform shutdown path and not wait for further OS
	// intervention before allowing the S5 transition.
	if (dev->pci_dev && e1000e_is_pch_lan(dev->pci_dev->device_id)) {
		// Drain the MAC master before D3 so no DMA/TLP remains outstanding
		// when ownership is returned to the firmware side.
		uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
		e1000e_write(dev, E1000_CTRL, ctrl | (1u << 2));
		for (int waited_us = 0; waited_us < 100000; waited_us += 100) {
			if (!(e1000e_read(dev, E1000_STATUS) & (1u << 19))) {
				break;
			}
			e1000e_delay_us(100);
		}

		// Reverse the PCH-LAN power-gating exit that init applies in D0.
		// Leaving these clocks and MAC-side gates forced on into the S5
		// handoff can keep the integrated LAN function warm after the rest
		// of the platform has already started to power down.
		uint32_t pbeccsts = e1000e_read(dev, E1000_PBECCSTS);
		e1000e_write(dev, E1000_PBECCSTS,
			     pbeccsts & ~E1000_PBECCSTS_ECC_ENABLE);

		ctrl = e1000e_read(dev, E1000_CTRL);
		e1000e_write(dev, E1000_CTRL, ctrl & ~E1000_CTRL_MEHE);

		uint32_t dpgfr = e1000e_read(dev, E1000_DPGFR);
		e1000e_write(dev, E1000_DPGFR, dpgfr | (1u << 2));

		uint32_t pch_ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
		pch_ctrl_ext |=
			E1000_CTRL_EXT_DPG_EN | E1000_CTRL_EXT_DMA_DYN_CLK_EN;
		e1000e_write(dev, E1000_CTRL_EXT, pch_ctrl_ext);

		uint32_t fextnvm7 = e1000e_read(dev, E1000_FEXTNVM7);
		fextnvm7 |= E1000_FEXTNVM7_DISABLE_TSYNC_CLK;
		fextnvm7 &= ~(E1000_FEXTNVM7_ENABLE_TSYNC_INTR |
			      E1000_FEXTNVM7_SIDE_CLK_UNGATE);
		e1000e_write(dev, E1000_FEXTNVM7, fextnvm7);

		// Undo the bring-up ownership handshake so the firmware/ME can
		// park the integrated PHY in its shutdown state instead of keeping
		// the link side powered on our behalf.
		uint32_t h2me = e1000e_read(dev, E1000_H2ME);
		h2me &= ~E1000_H2ME_ENFORCE_SETTINGS;
		h2me |= E1000_H2ME_ULP;
		e1000e_write(dev, E1000_H2ME, h2me);

		if (e1000e_acquire_swflag(dev) == 0) {
			uint16_t ulp_cfg = 0;
			if (e1000e_phy_read_paged_locked(dev,
							 E1000_I218_ULP_CONFIG1,
							 &ulp_cfg) == 0) {
				ulp_cfg &=
					~(E1000_I218_ULP_CONFIG1_WOL_HOST |
					  E1000_I218_ULP_CONFIG1_INBAND_EXIT);
				ulp_cfg |=
					E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS |
					E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC;
				(void)e1000e_phy_write_paged_locked(
					dev, E1000_I218_ULP_CONFIG1, ulp_cfg);
				(void)e1000e_phy_write_paged_locked(
					dev, E1000_I218_ULP_CONFIG1,
					(uint16_t)(ulp_cfg |
						   E1000_I218_ULP_CONFIG1_START));
			}
			e1000e_release_swflag(dev);
		}
	}

	uint32_t ctrl_ext = e1000e_read(dev, E1000_CTRL_EXT);
	ctrl_ext &= ~E1000_CTRL_EXT_DRV_LOAD;
	e1000e_write(dev, E1000_CTRL_EXT, ctrl_ext);

	// Drop PCI bus-mastering so any remaining in-flight TLPs are
	// discarded by the root complex rather than landing in memory
	// after the OS has handed off to the firmware.
	if (dev->pci_dev) {
		const pci_device_t *pdev = dev->pci_dev;

		uint8_t pm_cap = pci_find_capability(pdev, 0x01);
		if (pm_cap) {
			uint32_t pmcsr_dw = pci_cfg_read32(
				pdev->bus, pdev->device, pdev->function,
				(uint8_t)(pm_cap + 4));
			uint16_t pmcsr = (uint16_t)(pmcsr_dw & 0xFFFFu);
			pmcsr &= ~((uint16_t)0x0100u); // PME Enable
			pmcsr |= 0x8000u; // clear PME Status if latched
			pmcsr = (uint16_t)((pmcsr & ~0x0003u) |
					   0x0003u); // D3hot
			pmcsr_dw = (pmcsr_dw & 0xFFFF0000u) | pmcsr;
			pci_cfg_write32(pdev->bus, pdev->device, pdev->function,
					(uint8_t)(pm_cap + 4), pmcsr_dw);
		}

		uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device,
					      pdev->function, 0x04);
		cmd &= ~0x0006u; // clear Bus Master Enable and Memory Space Enable
		pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04,
				cmd);
	}
}

// ============================================================================
// IRQ Handler
// ============================================================================
/* Acknowledge the causes, hand receive and transmit completion work to the
 * NAPI poll (their causes stay masked until it is done) and link changes
 * to the watchdog.  Returns 1 when the interrupt was this device's, 0
 * otherwise, so that on a shared legacy line the dispatcher offers it to
 * the next handler.  The dispatcher acknowledges the local APIC. */
int e1000e_irq_handler(void)
{
	if (!g_e1000e_initialized)
		return 0;

	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	e1000e_dev_t *dev = &g_e1000e;
	uint32_t icr = e1000e_read(dev, E1000_ICR);
	int handled;

	if (icr == 0xFFFFFFFFu)
		return 0; // the device is gone from the bus
	if (dev->msi_vector) {
		/* An MSI is ours whatever ICR says: a cause may have been
		 * collected by an earlier read.  The ring is the ground
		 * truth -- let the poll look. */
		handled = 1;
		icr |= E1000_ICR_RXT0;
	} else {
		/* Legacy line: ours when the MAC asserted it, or a cause we
		 * enabled is pending. */
		handled = (icr & E1000_ICR_INT_ASSERTED) ||
			  (icr & e1000e_read(dev, E1000_IMS));
	}

	if (icr & E1000_ICR_LSC) {
		/* Record the change and hand it to the link watchdog
		 * (e1000e_link_state_poll), which logs it and does the
		 * link-down / link-up work outside interrupt context.
		 *
		 * Storm guard: on some PCH-LAN parts the link can bounce
		 * continuously after init.  More than 16 LSC interrupts in a
		 * second mask LSC; the watchdog unmasks it a second later,
		 * and polls STATUS.LU meanwhile. */
		uint64_t now = timer_ticks();
		if (now - dev->lsc_window_tick >= net_ms_to_ticks(1000)) {
			dev->lsc_window_tick = now;
			dev->lsc_count = 0;
		}
		if (++dev->lsc_count > 16 && !dev->lsc_masked_tick) {
			e1000e_write(dev, E1000_IMC, E1000_ICR_LSC);
			dev->lsc_masked_tick = now ? now : 1;
		}
		dev->link_change_pending = 1;
		net_housekeeping_kick();
	}

	/* Receive/transmit work, also when the line turns out not to be
	 * ours: the read above consumed the cause, and a poll that is
	 * about to re-enable its interrupts would otherwise never hear of
	 * the frame behind it. */
	if (icr & E1000E_IMS_NAPI) {
		e1000e_write(dev, E1000_IMC, E1000E_IMS_NAPI);
		napi_schedule(&dev->napi);
	}
	return handled;
}

// ============================================================================
// Map MMIO BAR
// ============================================================================
static int e1000e_map_bar(e1000e_dev_t *dev)
{
	uint32_t bar0 = dev->pci_dev->bar[0];

	if (bar0 & 1) {
		kprintf("E1000E: BAR0 is I/O port, not MMIO\n");
		return -1;
	}

	uint64_t phys = bar0 & 0xFFFFFFF0ULL;
	if ((bar0 & 0x06) == 0x04) {
		phys |= ((uint64_t)dev->pci_dev->bar[1]) << 32;
	}

	dev->mmio_phys = phys;
	dev->mmio_size = 128 * 1024;

	uint32_t num_pages = (dev->mmio_size + PAGE_SIZE - 1) / PAGE_SIZE;

	// Map (or, if the BAR happens to lie inside the kernel direct map,
	// *re*map in place) the device's register window with strict
	// uncacheable attributes.
	//
	// The previous "if (!mm_is_page_mapped) mm_map_page(UC)" pattern
	// was the actual reason this driver worked under QEMU but stalled
	// on every real PCH-LAN system.  On QEMU the I219's BAR is placed
	// above the bootloader's identity/direct map (typically around
	// 0xFEB00000), so the conditional triggered and we got a UC
	// mapping.  On a real Lenovo P50 (and every other modern Intel
	// laptop) the integrated I219-LM's BAR sits below 4 GiB inside
	// the existing kernel direct map, which is mapped *write-back
	// cached*.  MMIO writes through a WB alias get coalesced and
	// posted out of order, so register state visible to the device
	// is not what the driver wrote — TDT may increment but TXDCTL,
	// TCTL or the TX doorbell ordering is silently broken, the
	// descriptor-fetch engine never starts, and the symptom is
	// exactly what we observe: link UP, TDT advancing, TDH stuck at
	// 0, GPTC=0, no PCI errors.  mm_map_device_mmio() handles both
	// cases correctly: it rewrites direct-map PTEs in place to UC
	// when the range is in the direct map, and falls back to
	// mm_map_mmio() otherwise.
	uint64_t virt_base = mm_map_device_mmio(phys, num_pages);
	if (!virt_base) {
		kprintf("E1000E: Failed to map BAR0 MMIO (phys=%llx, %u pages)\n",
			(unsigned long long)phys, num_pages);
		return -1;
	}

	dev->mmio_base = (volatile uint8_t *)virt_base;
	return 0;
}

// ============================================================================
// Driver Initialization
// ============================================================================
void e1000e_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		if (devs[i].vendor_id != E1000E_VENDOR_ID)
			continue;

		uint16_t did = devs[i].device_id;
		const e1000e_id_t *match = e1000e_lookup(did);
		if (!match)
			continue;
		const char *name = match->name;

		kprintf("E1000E: Found %s (PCI %02x:%02x.%x)\n", name,
			devs[i].bus, devs[i].device, devs[i].function);

		e1000e_dev_t *dev = &g_e1000e;
		dev->pci_dev = &devs[i];
		dev->device_id = did;
		dev->buffers_cached = (uint8_t)e1000e_buffers_cacheable(did);
		// STATUS the KMRN/K1 timing was set for during bring-up
		uint32_t pch_timing_status = 0;
		// ICH/PCH LAN MACs have a 32-register multicast table
		dev->mta_regs = e1000e_is_pch_lan(did) ? 32 : 128;

		pci_enable_busmaster_mem(dev->pci_dev);

		if (e1000e_map_bar(dev) < 0) {
			kprintf("E1000E: Failed to map BAR0\n");
			return;
		}

		e1000e_reset(dev);

		// I217/I218/I219 (PCH-LAN) bring-up workarounds — required on
		// every Intel-branded business laptop/desktop with an integrated
		// LOM since Skylake (incl. ThinkPad P50's I219-LM at 0x156F).
		if (e1000e_is_pch_lan(did)) {
			e1000e_pch_lan_init(dev);
		}

		e1000e_read_mac(dev);
		kprintf("E1000E: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
			dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

		// Set link up.  Per 82574 datasheet section 4.6.3.2 (and
		// confirmed for I217/I218/I219), the recommended config when
		// using the integrated PHY's auto-negotiation is:
		//   CTRL.FRCDPLX = 0  (let PHY tell MAC the duplex)
		//   CTRL.FRCSPD  = 0  (let PHY tell MAC the speed)
		//   CTRL.ASDE    = 0  (no separate auto-speed-detect needed —
		//                      datasheet explicitly says ASDE "must be
		//                      set to 0b in the 82574")
		//   CTRL.SLU     = 1  (enable MAC↔PHY communication)
		// The MAC then samples SPEED/FDX from the PHY's internal
		// SPD_IND/FDX signals on the rising edge of LINK and reflects
		// them in STATUS.  If we leave ASDE=1, the MAC additionally
		// tries to time RX_CLK and may latch the wrong frequency
		// when the PHY's link signal toggles, sending the MAC↔PHY
		// KMRN bus out of lock and dropping every frame.
		// Program flow-control registers to canonical IEEE 802.3x values
		// BEFORE asserting SLU.  The PCH-LAN setup_link path requires FCTTV
		// / FCRTL / FCRTH / FCAL / FCAH / FCT programming before transmitter
		// enable.
		// Chip default leaves FCT=0x80800000 / FCAH=0xC0000008 etc., which
		// can wedge the MAC's flow-control state machine.  We do not need
		// RX flow control (FCRTL/FCRTH=0 disables it), but the FCAL/FCAH/FCT
		// multicast-pause-frame-recogniser must hold the spec values, and
		// CTRL.RFCE/TFCE must be cleared so the MAC does not gate TX while
		// waiting for a XON it will never see.
		e1000e_write(dev, E1000_FCAL,
			     0x00C28001u); // 802.3x mc address lo
		e1000e_write(dev, E1000_FCAH,
			     0x00000100u); // 802.3x mc address hi
		e1000e_write(dev, E1000_FCT, 0x00008808u); // 802.3x ethertype
		e1000e_write(dev, E1000_FCTTV, 0x0000FFFFu); // pause-time, full
		if (e1000e_mac_gen(did) >= E1000E_MAC_PCH_SPT) {
			/* SPT and later: full flow control requested.  The
			 * receive thresholds of the 26 KB packet buffer, the
			 * refresh timer and the PHY's pause time are set here;
			 * whether pause frames are honoured or sent is decided
			 * at link-up from the autonegotiation result
			 * (e1000e_spt_link_up), until then both stay off. */
			e1000e_write(dev, E1000_FCRTV_PCH, E1000E_FC_PAUSE_TIME);
			e1000e_write(dev, E1000_FCRTL,
				     E1000E_PCH_FC_LOW_WATER | E1000_FCRTL_XONE);
			e1000e_write(dev, E1000_FCRTH, E1000E_PCH_FC_HIGH_WATER);
			if (e1000e_acquire_swflag(dev) == 0) {
				(void)e1000e_phy_write_paged_locked(
					dev, BM_PORT_CTRL_PAUSE,
					E1000E_FC_PAUSE_TIME);
				e1000e_release_swflag(dev);
			}
		} else {
			e1000e_write(dev, E1000_FCRTL, 0); // RX flow ctrl off
			e1000e_write(dev, E1000_FCRTH, 0);
		}

		uint32_t ctrl = e1000e_read(dev, E1000_CTRL);
		ctrl &= ~(E1000_CTRL_FRCSPD | (1u << 12) /* FRCDPLX */
			  | E1000_CTRL_ASDE | E1000_CTRL_PHY_RST |
			  (1u << 7) // ILOS — spec: must be 0 in normal mode
			  | (1u
			     << 20) // ADVD3WUC — D3 wake; not in use, must be 0
			  | (1u << 27) // RFCE — RX flow control off
			  | (1u << 28) // TFCE — TX flow control off
			  | (1u << 30)); // VME — VLAN tagging off
		ctrl |= E1000_CTRL_SLU;
		e1000e_write(dev, E1000_CTRL, ctrl);
		e1000e_dbg(
			"E1000E: CTRL=%08x (SLU set, FRCSPD/FRCDPLX/ASDE/RFCE/TFCE cleared)\n",
			e1000e_read(dev, E1000_CTRL));

		// Disable Kumeran K1 power-save on PCH-LAN parts.  Without this
		// the MAC↔PHY interconnect drops every received frame on
		// I219-LM at 100 Mbps full-duplex (TPR stays at 0).
		if (e1000e_is_pch_lan(did)) {
			e1000e_disable_k1(dev);

			// Restart copper auto-negotiation on the PHY.  The PHY_RST we
			// asserted in pch_lan_init left the integrated PHY in a state
			// where its 100BASE-T transmitter is up but no FLPs are on the
			// wire and BMSR.AN_COMPLETE = 0; the MAC's STATUS.LU bit then
			// reflects only the cached SMBus link and STATUS.SPEED reads as
			// 10 Mb/s.  Without a real copper link the MAC PCS never grants
			// TX, which is exactly the "TDH=0, GPTC=0, descriptor in FIFO"
			// symptom we observe.
			(void)e1000e_phy_restart_autoneg(dev);

			// Wait for STATUS.LU to assert AND for the speed bits to latch
			// before reading STATUS.SPEED for the KMRN configuration.
			// Reading STATUS immediately after asserting CTRL.SLU returns
			// the previously-latched speed (typically 10 Mb/s post-reset),
			// which then mis-times the MAC<->PHY KMRN bus and silently
			// drops every Tx fetch.  The check_for_link path always gates
			// speed-dependent KMRN reprogramming on a confirmed link
			// resolution.
			//
			// Poll up to ~2.5 s for link-up; that's the worst-case auto-neg
			// time the 802.3 spec allows for 10/100/1000BASE-T.  Then add a
			// 50 ms settle so STATUS.SPEED definitely reflects the resolved
			// speed before we sample it.
			{
				uint32_t status = 0;
				for (int ms = 0; ms < 2500; ms++) {
					status = e1000e_read(dev, E1000_STATUS);
					if (status & E1000_STATUS_LU)
						break;
					e1000e_delay_us(1000);
				}
				e1000e_delay_us(50000);
				uint32_t st_spd =
					e1000e_read(dev, E1000_STATUS);
				uint32_t spd = (st_spd >> 6) & 0x3;
				e1000e_dbg(
					"E1000E: link-resolve STATUS=%08x LU=%u SPEED=%u (%s)\n",
					st_spd,
					(st_spd & E1000_STATUS_LU) ? 1 : 0, spd,
					spd == 0 ? "10Mb/s" :
					spd == 1 ? "100Mb/s" :
					spd == 2 ? "1000Mb/s" :
						   "reserved");
				(void)spd;
				// KMRN timing and the P50 K1 quirk for the speed
				// the link came up at; the link watchdog redoes
				// it at every later link-up.
				e1000e_pch_link_timing(dev, st_spd);
				pch_timing_status = st_spd;
			}
		}

		// Clear STATUS.PHYRA (PHY Reset Asserted).  Per the 82574
		// datasheet (section 10.2.2.2) this bit is RW and is cleared
		// by writing 0b to it (NOT W1C as on most other status bits).
		// The K1-disable sequence above re-asserts PHYRA so we must
		// clear it again here, after the SLU/CTRL writes have settled.
		{
			uint32_t st = e1000e_read(dev, E1000_STATUS);
			if (st & E1000_STATUS_PHYRA) {
				e1000e_write(dev, E1000_STATUS,
					     st & ~E1000_STATUS_PHYRA);
				// Read back to confirm it actually cleared.
				st = e1000e_read(dev, E1000_STATUS);
				if (st & E1000_STATUS_PHYRA) {
					e1000e_dbg(
						"E1000E: WARNING PHYRA still set after clear "
						"(STATUS=%08x) — PHY may still be held in reset\n",
						st);
				}
			}
		}

		// Clear multicast table
		for (int m = 0; m < dev->mta_regs; m++)
			e1000e_write(dev, E1000_MTA + m * 4, 0);

		uint32_t ral = (uint32_t)dev->mac_addr[0] |
			       ((uint32_t)dev->mac_addr[1] << 8) |
			       ((uint32_t)dev->mac_addr[2] << 16) |
			       ((uint32_t)dev->mac_addr[3] << 24);
		uint32_t rah = (uint32_t)dev->mac_addr[4] |
			       ((uint32_t)dev->mac_addr[5] << 8) | (1u << 31);
		e1000e_write(dev, E1000_RAL, ral);
		e1000e_write(dev, E1000_RAH, rah);

		if (e1000e_init_rx(dev) < 0)
			return;
		if (e1000e_init_tx(dev) < 0)
			return;

		// ------------------------------------------------------------------
		// MSI first; legacy INTx with full ACPI _PRT lookup as fallback.
		// Same logic as the older e1000 driver — see comments there for
		// the rationale (notably: PCI interrupt_line is NOT the IOAPIC GSI
		// in APIC mode; the BIOS-assigned PIC IRQ value must NOT be
		// trusted on ICH9-class systems).
		// ------------------------------------------------------------------
		dev->msi_vector = E1000E_MSI_VECTOR;
		int msi_ret = pci_enable_msi(dev->pci_dev, dev->msi_vector);
		if (msi_ret < 0) {
			dev->msi_vector = 0;

			uint8_t irq = 0xFF;
			uint32_t gsi = 0;
			uint8_t pin = dev->pci_dev->interrupt_pin;
			if (pin >= 1 && pin <= 4) {
				uint8_t acpi_pin = pin - 1;
				uint8_t lookup_dev = dev->pci_dev->device;
				uint8_t lookup_pin = acpi_pin;

				if (dev->pci_dev->bus != 0) {
					const pci_device_t *bridge =
						pci_find_bridge_for_bus(
							dev->pci_dev->bus);
					if (bridge) {
						lookup_pin =
							(acpi_pin +
							 dev->pci_dev->device) %
							4;
						lookup_dev = bridge->device;
					}
				}

				if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
							lookup_dev, lookup_pin,
							&gsi) == 0 &&
				    gsi <= 23) {
					irq = (uint8_t)gsi;
					kprintf("E1000E: ACPI _PRT resolved INT%c -> GSI %u\n",
						'A' + acpi_pin, gsi);
				}
			}

			if (irq == 0xFF) {
				irq = dev->pci_dev->interrupt_line;
				if (irq != 0xFF && irq <= 23) {
					kprintf("E1000E: ACPI _PRT lookup failed, falling back to "
						"PCI interrupt_line = %d\n",
						irq);
				} else {
					kprintf("E1000E: WARNING: no valid IRQ (line=%d, _PRT failed)\n",
						dev->pci_dev->interrupt_line);
					kprintf("E1000E: Interrupts will not work\n");
				}
			}

			kprintf("E1000E: Using legacy IRQ %d\n", irq);
			g_e1000e_legacy_irq = irq;

			// Some firmware leaves PCI Command
			// bit 10 (INTx Disable) set, preventing the device from ever
			// raising its interrupt pin.  Clear it for the legacy path.
			{
				uint32_t cmd = pci_cfg_read32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04);
				if (cmd & PCI_CMD_INTX_DISABLE) {
					cmd &= ~PCI_CMD_INTX_DISABLE;
					pci_cfg_write32(dev->pci_dev->bus,
							dev->pci_dev->device,
							dev->pci_dev->function,
							0x04, cmd);
					kprintf("E1000E: cleared PCI Command INTx Disable bit\n");
				}
			}

			// SPT-or-later MAC on the legacy interrupt pin: the
			// IOSF sideband's clock gating and clock requests are
			// switched off, or the pin interrupt can be held back
			// while the sideband sleeps.  Only on this path: with
			// MSI the register keeps the value the firmware left.
			if (e1000e_mac_gen(dev->pci_dev->device_id) >=
			    E1000E_MAC_PCH_SPT) {
				uint32_t fx9 = e1000e_read(dev, E1000_FEXTNVM9);
				e1000e_write(dev, E1000_FEXTNVM9,
					     fx9 |
					     E1000_FEXTNVM9_IOSFSB_CLKGATE_DIS |
					     E1000_FEXTNVM9_IOSFSB_CLKREQ_DIS);
				e1000e_dbg("E1000E: legacy INTx: FEXTNVM9 %08x -> %08x\n",
					   fx9, e1000e_read(dev, E1000_FEXTNVM9));
			}

			if (irq <= 23) {
				uint8_t vector = 32 + irq;
				ioapic_configure_legacy_irq(
					irq, vector, IOAPIC_POLARITY_LOW,
					IOAPIC_TRIGGER_LEVEL);
			}
		} else {
			kprintf("E1000E: MSI enabled (vector %d)\n",
				dev->msi_vector);
		}

		// Mark the driver initialised before unmasking IMS — the IRQ
		// dispatcher only routes to e1000e_irq_handler() when this flag
		// is set; otherwise nothing acknowledges the device and the
		// system locks up in a level-triggered IRQ storm (same problem
		// we hit with the e1000 driver).
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("e1000e");
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("e1000e_tx");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		/* The poll exists before the handler can run: on a shared
		 * legacy line it is called for other devices' interrupts as
		 * soon as it is marked initialised. */
		netif_napi_add(&dev->net_dev, &dev->napi, e1000e_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_e1000e_initialized = 1;

		// Wait up to 2 s for link-up.  IMS is still masked here so we
		// cannot get an LSC storm during the poll.
		uint32_t status = e1000e_read(dev, E1000_STATUS);
		if (!(status & E1000_STATUS_LU)) {
			uint32_t t0 = timer_pmtimer_read_raw();
			while (timer_pmtimer_delta_us(
				       t0, timer_pmtimer_read_raw()) <
			       2000000) {
				e1000e_delay_us(50000);
				status = e1000e_read(dev, E1000_STATUS);
				if (status & E1000_STATUS_LU)
					break;
			}
		}
		dev->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
		if (dev->link_up) {
			dev->link_speed = e1000e_status_speed(status);
			dev->link_fd = (status & E1000_STATUS_FD) != 0;
			// TIPG/PHY/LTR/flow control for this link (SPT+)
			if (e1000e_is_pch_lan(did)) {
				/* The link came up only after the timing was
				 * set (for no link): set it for this one. */
				if (!(pch_timing_status & E1000_STATUS_LU))
					e1000e_pch_link_timing(dev, status);
				e1000e_spt_link_up(dev, status);
			}
			kprintf("E1000E: Link UP (%u Mb/s, %s duplex)\n",
				dev->link_speed,
				dev->link_fd ? "full" : "half");
		} else {
			kprintf("E1000E: Link DOWN\n");
		}

#if E1000E_DBG
		// One-shot diagnostic dump — printed BEFORE we unmask interrupts
		// so it always appears in the log even if an IRQ storm follows.
		{
			uint32_t s = e1000e_read(dev, E1000_STATUS);
			uint32_t cext = e1000e_read(dev, E1000_CTRL_EXT);
			uint32_t rctl = e1000e_read(dev, E1000_RCTL);
			uint32_t rdh = e1000e_read(dev, E1000_RDH);
			uint32_t rdt = e1000e_read(dev, E1000_RDT);
			e1000e_dbg(
				"E1000E: STATUS=%08x CTRL_EXT=%08x RCTL=%08x RDH=%u RDT=%u\n",
				s, cext, rctl, rdh, rdt);
		}
#endif


		// net_device_t setup
		dev->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			dev->net_dev.mac_addr[m] = dev->mac_addr[m];
		dev->net_dev.mtu = NET_MTU_DEFAULT;
		dev->net_dev.ip_addr = 0;
		dev->net_dev.netmask = 0;
		dev->net_dev.gateway = 0;
		dev->net_dev.dns_server = 0;
		dev->net_dev.send = NULL;
		dev->net_dev.xmit = e1000e_xmit;
		dev->net_dev.link_status = e1000e_link_status;
		dev->net_dev.shutdown = e1000e_shutdown;
		dev->net_dev.watchdog = e1000e_watchdog;
		dev->net_dev.tx_timeout = e1000e_tx_timeout;
		dev->net_dev.set_rx_mode = e1000e_set_rx_mode;
		dev->net_dev.get_stats64 = e1000e_get_stats64;
		dev->net_dev.features |= NETIF_F_RXCSUM;
		dev->stats_tick = timer_ticks();

		net_register(&dev->net_dev);
		if (!dev->link_up)
			netif_carrier_off(&dev->net_dev);

		// Unmask RX/TX and LSC, now that the device is registered.
		// LSC is required for prompt cable hot-plug detection.
		// PCH-LAN parts can bounce link rapidly during init; the IRQ
		// handler masks LSC for a second after 16 events within one to
		// prevent CPU starvation, and the link watchdog polls STATUS.LU
		// as well, so the link state stays right while LSC is masked.
		e1000e_write(dev, E1000_IMS, E1000E_IMS_ALL);

		kprintf("E1000E: Driver initialized successfully\n");
		return; // Only handle first matching device
	}
}
