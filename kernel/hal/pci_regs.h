// LikeOS - PCI and PCI Express configuration space registers
//
// The standard configuration space layout shared by every PCI function:
// the type 0 (device) and type 1 (PCI-to-PCI bridge) headers, the
// classic capability list (power management, MSI, MSI-X, PCI Express,
// SATA, Advanced Features, ...) and the PCI Express extended capability
// list (AER, device serial number, ACS, ARI, ATS, PRI, PASID, SR-IOV,
// LTR, L1 PM substates, resizable BAR, DPC, PTM, secondary PCIe, data
// link feature, physical layer 16/32/64 GT/s, NPEM, vendor-specific).
// Further: the class codes and vendor IDs the drivers match on, and a
// few decode helpers.
//
// Offsets of a classic capability are relative to the capability header
// found by pci_find_capability(); offsets of an extended capability are
// relative to the capability header found by pci_find_ext_capability()
// (an offset >= 0x100 in the 4 KiB extended configuration space).
//
// The macro names are the conventional, well known ones, so a driver
// can be written against the names every PCI datasheet and driver uses.
// A handful of names that have no conventional spelling are LikeOS
// names; their comments say so.
//
// This file is NOT included by kernel/hal/pci.h.  Older drivers carry
// local definitions (PCI_CAP_MSI, PCI_CMD_INTX_DISABLE, PCI_PMCSR_*, ...)
// and keep compiling unchanged; a driver that wants the full register
// map includes <kernel/hal/pci_regs.h> explicitly.  None of the names
// that kernel/hal/pci.h defines are defined here.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_HAL_PCI_REGS_H
#define KERNEL_HAL_PCI_REGS_H

#include <kernel/uapi/types.h>

/* ==========================================================================
 * Configuration space sizes
 * ========================================================================== */

#define PCI_CFG_SPACE_SIZE 256 /* conventional PCI, PCI-X mode 1 */
#define PCI_CFG_SPACE_EXP_SIZE 4096 /* PCI Express, PCI-X mode 2 */
#define PCI_STD_HEADER_SIZEOF 64 /* standardized header 0x00..0x3f */
#define PCI_STD_NUM_BARS 6 /* BARs in a type 0 header */

/* ==========================================================================
 * Common header (all header types)
 * ========================================================================== */

#define PCI_VENDOR_ID 0x00 /* 16 bits; 0xffff = no function */
#define PCI_DEVICE_ID 0x02 /* 16 bits */

#define PCI_COMMAND 0x04 /* 16 bits */
#define PCI_COMMAND_IO 0x1 /* decode I/O space */
#define PCI_COMMAND_MEMORY 0x2 /* decode memory space */
#define PCI_COMMAND_MASTER 0x4 /* bus master (DMA) enable */
#define PCI_COMMAND_SPECIAL 0x8 /* respond to special cycles */
#define PCI_COMMAND_INVALIDATE 0x10 /* memory write and invalidate */
#define PCI_COMMAND_VGA_PALETTE 0x20 /* VGA palette snooping */
#define PCI_COMMAND_PARITY 0x40 /* parity error response */
#define PCI_COMMAND_WAIT 0x80 /* address/data stepping (obsolete) */
#define PCI_COMMAND_SERR 0x100 /* SERR# driver enable */
#define PCI_COMMAND_FAST_BACK 0x200 /* fast back-to-back writes */
#define PCI_COMMAND_INTX_DISABLE 0x400 /* INTx assertion disable */

#define PCI_STATUS 0x06 /* 16 bits; error bits are RW1C */
#define PCI_STATUS_IMM_READY 0x01 /* immediate readiness */
#define PCI_STATUS_INTERRUPT 0x08 /* INTx pending */
#define PCI_STATUS_CAP_LIST 0x10 /* capability list at 0x34 valid */
#define PCI_STATUS_66MHZ 0x20 /* 66 MHz capable */
#define PCI_STATUS_UDF 0x40 /* user definable features (obsolete) */
#define PCI_STATUS_FAST_BACK 0x80 /* fast back-to-back capable */
#define PCI_STATUS_PARITY 0x100 /* master data parity error */
#define PCI_STATUS_DEVSEL_MASK 0x600 /* DEVSEL# timing */
#define PCI_STATUS_DEVSEL_FAST 0x000
#define PCI_STATUS_DEVSEL_MEDIUM 0x200
#define PCI_STATUS_DEVSEL_SLOW 0x400
#define PCI_STATUS_SIG_TARGET_ABORT 0x800 /* signalled target abort */
#define PCI_STATUS_REC_TARGET_ABORT 0x1000 /* received target abort */
#define PCI_STATUS_REC_MASTER_ABORT 0x2000 /* received master abort */
#define PCI_STATUS_SIG_SYSTEM_ERROR 0x4000 /* signalled SERR# */
#define PCI_STATUS_DETECTED_PARITY 0x8000 /* detected parity error */

#define PCI_CLASS_REVISION 0x08 /* 32 bits: class << 8 | revision */
#define PCI_REVISION_ID 0x08 /* 8 bits */
#define PCI_CLASS_PROG 0x09 /* 8 bits: programming interface */
#define PCI_CLASS_DEVICE 0x0a /* 16 bits: base class << 8 | subclass */

#define PCI_CACHE_LINE_SIZE 0x0c /* 8 bits, in dwords */
#define PCI_LATENCY_TIMER 0x0d /* 8 bits */
#define PCI_HEADER_TYPE 0x0e /* 8 bits */
#define PCI_HEADER_TYPE_MASK 0x7f /* layout of 0x10..0x3f */
#define PCI_HEADER_TYPE_NORMAL 0 /* type 0: device */
#define PCI_HEADER_TYPE_BRIDGE 1 /* type 1: PCI-to-PCI bridge */
#define PCI_HEADER_TYPE_CARDBUS 2 /* type 2: CardBus bridge */
#define PCI_HEADER_TYPE_MFD 0x80 /* multi-function device */

#define PCI_BIST 0x0f /* 8 bits: built-in self test */
#define PCI_BIST_CODE_MASK 0x0f /* completion code, 0 = passed */
#define PCI_BIST_START 0x40 /* write 1 to start; clears when done */
#define PCI_BIST_CAPABLE 0x80 /* BIST supported */

/* ==========================================================================
 * Base address registers (type 0: 0-5, type 1: 0-1)
 *
 * Sizing: write all ones with decoding off, read back, the clear low
 * bits of the address field give the size.
 * ========================================================================== */

#define PCI_BASE_ADDRESS_0 0x10 /* 32 bits */
#define PCI_BASE_ADDRESS_1 0x14 /* 32 bits (types 0 and 1) */
#define PCI_BASE_ADDRESS_2 0x18 /* 32 bits (type 0 only) */
#define PCI_BASE_ADDRESS_3 0x1c /* 32 bits */
#define PCI_BASE_ADDRESS_4 0x20 /* 32 bits */
#define PCI_BASE_ADDRESS_5 0x24 /* 32 bits */
#define PCI_BASE_ADDRESS_SPACE 0x01 /* 0 = memory, 1 = I/O */
#define PCI_BASE_ADDRESS_SPACE_IO 0x01
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0x00
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06 /* memory BAR width */
#define PCI_BASE_ADDRESS_MEM_TYPE_32 0x00 /* anywhere below 4 GiB */
#define PCI_BASE_ADDRESS_MEM_TYPE_1M 0x02 /* below 1 MiB (obsolete) */
#define PCI_BASE_ADDRESS_MEM_TYPE_64 0x04 /* 64 bits, uses the next BAR */
#define PCI_BASE_ADDRESS_MEM_PREFETCH 0x08 /* prefetchable */
#define PCI_BASE_ADDRESS_MEM_MASK (~0x0fUL) /* memory address bits */
#define PCI_BASE_ADDRESS_IO_MASK (~0x03UL) /* I/O address bits */

/* ==========================================================================
 * Type 0 header (devices)
 * ========================================================================== */

#define PCI_CARDBUS_CIS 0x28 /* CardBus CIS pointer */
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c /* 16 bits */
#define PCI_SUBSYSTEM_ID 0x2e /* 16 bits */
#define PCI_ROM_ADDRESS 0x30 /* expansion ROM: bits 31..11 address */
#define PCI_ROM_ADDRESS_ENABLE 0x01 /* ROM decode enable */
#define PCI_ROM_ADDRESS_MASK (~0x7ffU)

#define PCI_CAPABILITY_LIST 0x34 /* 8 bits: first capability offset */

#define PCI_INTERRUPT_LINE 0x3c /* 8 bits: routing, written by firmware */
#define PCI_INTERRUPT_PIN 0x3d /* 8 bits: 0 none, 1-4 = INTA#-INTD# */
#define PCI_MIN_GNT 0x3e /* 8 bits */
#define PCI_MAX_LAT 0x3f /* 8 bits */

/* ==========================================================================
 * Type 1 header (PCI-to-PCI bridges, root and switch ports)
 * ========================================================================== */

#define PCI_PRIMARY_BUS 0x18 /* bus the bridge sits on */
#define PCI_SECONDARY_BUS 0x19 /* bus directly behind the bridge */
#define PCI_SUBORDINATE_BUS 0x1a /* highest bus behind the bridge */
#define PCI_SEC_LATENCY_TIMER 0x1b /* secondary latency timer */
/* The four bytes at 0x18 as one dword */
#define PCI_PRIMARY_BUS_MASK 0x000000ff
#define PCI_SECONDARY_BUS_MASK 0x0000ff00
#define PCI_SUBORDINATE_BUS_MASK 0x00ff0000
#define PCI_SEC_LATENCY_TIMER_MASK 0xff000000

#define PCI_IO_BASE 0x1c /* I/O window forwarded downstream */
#define PCI_IO_LIMIT 0x1d
#define PCI_IO_RANGE_TYPE_MASK 0x0fUL /* I/O decode width */
#define PCI_IO_RANGE_TYPE_16 0x00
#define PCI_IO_RANGE_TYPE_32 0x01
#define PCI_IO_RANGE_MASK (~0x0fUL) /* 4 KiB granular window */
#define PCI_IO_1K_RANGE_MASK (~0x03UL) /* 1 KiB granular (some Intel) */
#define PCI_SEC_STATUS 0x1e /* secondary status, same bits as STATUS */
#define PCI_MEMORY_BASE 0x20 /* non-prefetchable window */
#define PCI_MEMORY_LIMIT 0x22
#define PCI_MEMORY_RANGE_TYPE_MASK 0x0fUL
#define PCI_MEMORY_RANGE_MASK (~0x0fUL)
#define PCI_PREF_MEMORY_BASE 0x24 /* prefetchable window */
#define PCI_PREF_MEMORY_LIMIT 0x26
#define PCI_PREF_RANGE_TYPE_MASK 0x0fUL
#define PCI_PREF_RANGE_TYPE_32 0x00
#define PCI_PREF_RANGE_TYPE_64 0x01 /* upper halves at 0x28/0x2c */
#define PCI_PREF_RANGE_MASK (~0x0fUL)
#define PCI_PREF_BASE_UPPER32 0x28 /* prefetchable base bits 63..32 */
#define PCI_PREF_LIMIT_UPPER32 0x2c /* prefetchable limit bits 63..32 */
#define PCI_IO_BASE_UPPER16 0x30 /* I/O base bits 31..16 */
#define PCI_IO_LIMIT_UPPER16 0x32 /* I/O limit bits 31..16 */
#define PCI_ROM_ADDRESS1 0x38 /* expansion ROM of a type 1 header */

#define PCI_BRIDGE_CONTROL 0x3e /* 16 bits */
#define PCI_BRIDGE_CTL_PARITY 0x01 /* parity error response, secondary */
#define PCI_BRIDGE_CTL_SERR 0x02 /* forward SERR# upstream */
#define PCI_BRIDGE_CTL_ISA 0x04 /* ISA alias filtering */
#define PCI_BRIDGE_CTL_VGA 0x08 /* forward legacy VGA ranges */
#define PCI_BRIDGE_CTL_MASTER_ABORT 0x20 /* report master aborts */
#define PCI_BRIDGE_CTL_BUS_RESET 0x40 /* secondary bus reset (hot reset) */
#define PCI_BRIDGE_CTL_FAST_BACK 0x80 /* fast back-to-back, secondary */

/* ==========================================================================
 * Capability list (classic, in the first 256 bytes)
 * ========================================================================== */

#define PCI_CAP_ID_MASK 0x00ff /* ID byte of the 16-bit header */
#define PCI_CAP_LIST_NEXT_MASK 0xff00 /* next byte of the 16-bit header */

#define PCI_CAP_LIST_ID 0 /* capability ID */
#define PCI_CAP_ID_PM 0x01 /* power management */
#define PCI_CAP_ID_AGP 0x02 /* accelerated graphics port */
#define PCI_CAP_ID_VPD 0x03 /* vital product data */
#define PCI_CAP_ID_SLOTID 0x04 /* slot identification */
#define PCI_CAP_ID_MSI 0x05 /* message signalled interrupts */
#define PCI_CAP_ID_CHSWP 0x06 /* CompactPCI hot swap */
#define PCI_CAP_ID_PCIX 0x07 /* PCI-X */
#define PCI_CAP_ID_HT 0x08 /* HyperTransport */
#define PCI_CAP_ID_VNDR 0x09 /* vendor specific */
#define PCI_CAP_ID_DBG 0x0A /* debug port */
#define PCI_CAP_ID_CCRC 0x0B /* CompactPCI central resource control */
#define PCI_CAP_ID_SHPC 0x0C /* standard hot-plug controller */
#define PCI_CAP_ID_SSVID 0x0D /* bridge subsystem vendor/device ID */
#define PCI_CAP_ID_AGP3 0x0E /* AGP target PCI-PCI bridge */
#define PCI_CAP_ID_SECDEV 0x0F /* secure device */
#define PCI_CAP_ID_EXP 0x10 /* PCI Express */
#define PCI_CAP_ID_MSIX 0x11 /* MSI-X */
#define PCI_CAP_ID_SATA 0x12 /* SATA data/index configuration */
#define PCI_CAP_ID_AF 0x13 /* advanced features (FLR) */
#define PCI_CAP_ID_EA 0x14 /* enhanced allocation */
#define PCI_CAP_ID_MAX PCI_CAP_ID_EA
#define PCI_CAP_LIST_NEXT 1 /* offset of the next capability, 0 = end */
#define PCI_CAP_FLAGS 2 /* 16 bits, capability specific */
#define PCI_CAP_SIZEOF 4

/* ==========================================================================
 * Power management capability (PCI_CAP_ID_PM)
 * ========================================================================== */

#define PCI_PM_PMC 2 /* 16 bits: PM capabilities */
#define PCI_PM_CAP_VER_MASK 0x0007 /* PM spec version */
#define PCI_PM_CAP_PME_CLOCK 0x0008 /* PCI clock needed for PME# */
#define PCI_PM_CAP_RESERVED 0x0010
#define PCI_PM_CAP_DSI 0x0020 /* device specific init needed */
#define PCI_PM_CAP_AUX_POWER 0x01C0 /* aux current requirement */
#define PCI_PM_CAP_D1 0x0200 /* D1 supported */
#define PCI_PM_CAP_D2 0x0400 /* D2 supported */
#define PCI_PM_CAP_PME 0x0800 /* PME# supported (from D0) */
#define PCI_PM_CAP_PME_MASK 0xF800 /* states PME# can be raised from */
#define PCI_PM_CAP_PME_D0 0x0800
#define PCI_PM_CAP_PME_D1 0x1000
#define PCI_PM_CAP_PME_D2 0x2000
#define PCI_PM_CAP_PME_D3hot 0x4000
#define PCI_PM_CAP_PME_D3cold 0x8000
#define PCI_PM_CAP_PME_SHIFT 11 /* first bit of the PME mask */
#define PCI_PM_CTRL 4 /* 16 bits: PM control/status (PMCSR) */
#define PCI_PM_CTRL_STATE_MASK 0x0003 /* power state, PCI_D0..PCI_D3hot */
#define PCI_PM_CTRL_NO_SOFT_RESET 0x0008 /* state kept across D3hot->D0 */
#define PCI_PM_CTRL_PME_ENABLE 0x0100 /* PME# generation enable */
#define PCI_PM_CTRL_DATA_SEL_MASK 0x1e00 /* data register select */
#define PCI_PM_CTRL_DATA_SCALE_MASK 0x6000 /* data register scale */
#define PCI_PM_CTRL_PME_STATUS 0x8000 /* PME# asserted (RW1C) */
#define PCI_PM_PPB_EXTENSIONS 6 /* 8 bits: bridge support extensions */
#define PCI_PM_PPB_B2_B3 0x40 /* secondary clock stopped in D3hot */
#define PCI_PM_BPCC_ENABLE 0x80 /* bus power/clock control enable */
#define PCI_PM_DATA_REGISTER 7 /* 8 bits: data selected by DATA_SEL */
#define PCI_PM_SIZEOF 8

/* Power states (the PMCSR state field values; D3cold is the state with
 * main power removed, never written to PMCSR) */
#define PCI_D0 0
#define PCI_D1 1
#define PCI_D2 2
#define PCI_D3hot 3
#define PCI_D3cold 4
#define PCI_UNKNOWN 5
#define PCI_POWER_ERROR (-1)

/* Mandatory delays after a power state change */
#define PCI_PM_D2_DELAY 200 /* us, after a transition to/from D2 */
#define PCI_PM_D3HOT_WAIT 10 /* ms, after D3hot -> D0 */
#define PCI_PM_D3COLD_WAIT 100 /* ms, after D3cold -> D0 (power-up) */

/* ==========================================================================
 * Vital product data (PCI_CAP_ID_VPD)
 * ========================================================================== */

#define PCI_VPD_ADDR 2 /* 16 bits: address, F flag */
#define PCI_VPD_ADDR_MASK 0x7fff
#define PCI_VPD_ADDR_F 0x8000 /* write 0 to read, flips on completion */
#define PCI_VPD_DATA 4 /* 32 bits */
#define PCI_CAP_VPD_SIZEOF 8

/* ==========================================================================
 * MSI capability (PCI_CAP_ID_MSI)
 *
 * The layout after the message address depends on PCI_MSI_FLAGS_64BIT:
 * the _32 offsets apply without it, the _64 offsets with it.
 * ========================================================================== */

#define PCI_MSI_FLAGS 0x02 /* 16 bits: message control */
#define PCI_MSI_FLAGS_ENABLE 0x0001 /* MSI enable */
#define PCI_MSI_FLAGS_QMASK 0x000e /* log2 of vectors requested (RO) */
#define PCI_MSI_FLAGS_QSIZE 0x0070 /* log2 of vectors granted */
#define PCI_MSI_FLAGS_64BIT 0x0080 /* 64-bit message address */
#define PCI_MSI_FLAGS_MASKBIT 0x0100 /* per-vector mask/pending present */
#define PCI_MSI_RFU 3 /* rest of the capability flags */
#define PCI_MSI_ADDRESS_LO 0x04 /* message address bits 31..0 */
#define PCI_MSI_ADDRESS_HI 0x08 /* message address bits 63..32 (64BIT) */
#define PCI_MSI_DATA_32 0x08 /* 16 bits: message data, 32-bit layout */
#define PCI_MSI_MASK_32 0x0c /* mask bits, 32-bit layout */
#define PCI_MSI_PENDING_32 0x10 /* pending bits, 32-bit layout */
#define PCI_MSI_DATA_64 0x0c /* 16 bits: message data, 64-bit layout */
#define PCI_MSI_MASK_64 0x10 /* mask bits, 64-bit layout */
#define PCI_MSI_PENDING_64 0x14 /* pending bits, 64-bit layout */

/* ==========================================================================
 * MSI-X capability (PCI_CAP_ID_MSIX)
 *
 * The vector table and the pending bit array live in memory behind a
 * BAR: BIR selects the BAR, the remaining bits are the byte offset.
 * ========================================================================== */

#define PCI_MSIX_FLAGS 2 /* 16 bits: message control */
#define PCI_MSIX_FLAGS_QSIZE 0x07FF /* table size - 1 */
#define PCI_MSIX_FLAGS_MASKALL 0x4000 /* function mask: all vectors */
#define PCI_MSIX_FLAGS_ENABLE 0x8000 /* MSI-X enable */
#define PCI_MSIX_TABLE 4 /* 32 bits: table BIR/offset */
#define PCI_MSIX_TABLE_BIR 0x00000007 /* BAR holding the table */
#define PCI_MSIX_TABLE_OFFSET 0xfffffff8 /* table offset in that BAR */
#define PCI_MSIX_PBA 8 /* 32 bits: pending bit array BIR/offset */
#define PCI_MSIX_PBA_BIR 0x00000007 /* BAR holding the PBA */
#define PCI_MSIX_PBA_OFFSET 0xfffffff8 /* PBA offset in that BAR */
#define PCI_MSIX_FLAGS_BIRMASK PCI_MSIX_PBA_BIR /* old name */
#define PCI_CAP_MSIX_SIZEOF 12

/* One MSI-X table entry (memory, not configuration space) */
#define PCI_MSIX_ENTRY_SIZE 16
#define PCI_MSIX_ENTRY_LOWER_ADDR 0x0 /* message address bits 31..0 */
#define PCI_MSIX_ENTRY_UPPER_ADDR 0x4 /* message address bits 63..32 */
#define PCI_MSIX_ENTRY_DATA 0x8 /* message data */
#define PCI_MSIX_ENTRY_VECTOR_CTRL 0xc /* vector control */
#define PCI_MSIX_ENTRY_CTRL_MASKBIT 0x00000001 /* vector masked */
#define PCI_MSIX_ENTRY_CTRL_ST 0xffff0000 /* TPH steering tag */

/* ==========================================================================
 * Advanced features capability (PCI_CAP_ID_AF; FLR on conventional PCI)
 * ========================================================================== */

#define PCI_AF_LENGTH 2
#define PCI_AF_CAP 3
#define PCI_AF_CAP_TP 0x01 /* transactions pending bit supported */
#define PCI_AF_CAP_FLR 0x02 /* function level reset supported */
#define PCI_AF_CTRL 4
#define PCI_AF_CTRL_FLR 0x01 /* write 1: start function level reset */
#define PCI_AF_STATUS 5
#define PCI_AF_STATUS_TP 0x01 /* transactions pending */
#define PCI_CAP_AF_SIZEOF 6

/* ==========================================================================
 * SATA capability (PCI_CAP_ID_SATA; AHCI index/data pair location)
 * ========================================================================== */

#define PCI_SATA_REGS 4 /* where the index/data pair lives */
#define PCI_SATA_REGS_MASK 0xF /* BAR location (4 = BAR0 .. 9 = BAR5) */
#define PCI_SATA_REGS_INLINE 0xF /* in configuration space */
#define PCI_SATA_SIZEOF_SHORT 8
#define PCI_SATA_SIZEOF_LONG 16

/* ==========================================================================
 * Bridge subsystem vendor ID capability (PCI_CAP_ID_SSVID)
 * ========================================================================== */

#define PCI_SSVID_VENDOR_ID 4
#define PCI_SSVID_DEVICE_ID 6

/* ==========================================================================
 * PCI Express capability (PCI_CAP_ID_EXP)
 * ========================================================================== */

#define PCI_EXP_FLAGS 0x02 /* 16 bits: PCIe capabilities */
#define PCI_EXP_FLAGS_VERS 0x000f /* capability version (1 or 2) */
#define PCI_EXP_FLAGS_TYPE 0x00f0 /* device/port type, PCI_EXP_TYPE_* << 4 */
#define PCI_EXP_TYPE_ENDPOINT 0x0 /* PCIe endpoint */
#define PCI_EXP_TYPE_LEG_END 0x1 /* legacy endpoint */
#define PCI_EXP_TYPE_ROOT_PORT 0x4 /* root port */
#define PCI_EXP_TYPE_UPSTREAM 0x5 /* switch upstream port */
#define PCI_EXP_TYPE_DOWNSTREAM 0x6 /* switch downstream port */
#define PCI_EXP_TYPE_PCI_BRIDGE 0x7 /* PCIe to PCI/PCI-X bridge */
#define PCI_EXP_TYPE_PCIE_BRIDGE 0x8 /* PCI/PCI-X to PCIe bridge */
#define PCI_EXP_TYPE_RC_END 0x9 /* root complex integrated endpoint */
#define PCI_EXP_TYPE_RC_EC 0xa /* root complex event collector */
#define PCI_EXP_FLAGS_SLOT 0x0100 /* port is connected to a slot */
#define PCI_EXP_FLAGS_IRQ 0x3e00 /* MSI/MSI-X vector for PCIe events */
#define PCI_EXP_FLAGS_FLIT 0x8000 /* flit mode supported */

#define PCI_EXP_DEVCAP 0x04 /* 32 bits: device capabilities */
#define PCI_EXP_DEVCAP_PAYLOAD 0x00000007 /* max payload supported, 128 << n */
#define PCI_EXP_DEVCAP_PHANTOM 0x00000018 /* phantom functions */
#define PCI_EXP_DEVCAP_EXT_TAG 0x00000020 /* 8-bit tags supported */
#define PCI_EXP_DEVCAP_L0S 0x000001c0 /* endpoint L0s acceptable latency */
#define PCI_EXP_DEVCAP_L1 0x00000e00 /* endpoint L1 acceptable latency */
#define PCI_EXP_DEVCAP_ATN_BUT 0x00001000 /* attention button (v1 only) */
#define PCI_EXP_DEVCAP_ATN_IND 0x00002000 /* attention indicator (v1 only) */
#define PCI_EXP_DEVCAP_PWR_IND 0x00004000 /* power indicator (v1 only) */
#define PCI_EXP_DEVCAP_RBER 0x00008000 /* role-based error reporting */
#define PCI_EXP_DEVCAP_PWR_VAL 0x03fc0000 /* captured slot power limit */
#define PCI_EXP_DEVCAP_PWR_SCL 0x0c000000 /* captured slot power scale */
#define PCI_EXP_DEVCAP_FLR 0x10000000 /* function level reset supported */
#define PCI_EXP_DEVCAP_TEE 0x40000000 /* TEE I/O (TDISP) supported */

#define PCI_EXP_DEVCTL 0x08 /* 16 bits: device control */
#define PCI_EXP_DEVCTL_CERE 0x0001 /* correctable error reporting */
#define PCI_EXP_DEVCTL_NFERE 0x0002 /* non-fatal error reporting */
#define PCI_EXP_DEVCTL_FERE 0x0004 /* fatal error reporting */
#define PCI_EXP_DEVCTL_URRE 0x0008 /* unsupported request reporting */
#define PCI_EXP_DEVCTL_RELAX_EN 0x0010 /* relaxed ordering enable */
#define PCI_EXP_DEVCTL_PAYLOAD 0x00e0 /* max payload size */
#define PCI_EXP_DEVCTL_PAYLOAD_128B 0x0000
#define PCI_EXP_DEVCTL_PAYLOAD_256B 0x0020
#define PCI_EXP_DEVCTL_PAYLOAD_512B 0x0040
#define PCI_EXP_DEVCTL_PAYLOAD_1024B 0x0060
#define PCI_EXP_DEVCTL_PAYLOAD_2048B 0x0080
#define PCI_EXP_DEVCTL_PAYLOAD_4096B 0x00a0
#define PCI_EXP_DEVCTL_EXT_TAG 0x0100 /* 8-bit tag enable */
#define PCI_EXP_DEVCTL_PHANTOM 0x0200 /* phantom functions enable */
#define PCI_EXP_DEVCTL_AUX_PME 0x0400 /* aux power PM enable */
#define PCI_EXP_DEVCTL_NOSNOOP_EN 0x0800 /* no snoop enable */
#define PCI_EXP_DEVCTL_READRQ 0x7000 /* max read request size */
#define PCI_EXP_DEVCTL_READRQ_128B 0x0000
#define PCI_EXP_DEVCTL_READRQ_256B 0x1000
#define PCI_EXP_DEVCTL_READRQ_512B 0x2000
#define PCI_EXP_DEVCTL_READRQ_1024B 0x3000
#define PCI_EXP_DEVCTL_READRQ_2048B 0x4000
#define PCI_EXP_DEVCTL_READRQ_4096B 0x5000
#define PCI_EXP_DEVCTL_BCR_FLR 0x8000 /* initiate FLR / bridge config retry */

#define PCI_EXP_DEVSTA 0x0a /* 16 bits: device status (RW1C) */
#define PCI_EXP_DEVSTA_CED 0x0001 /* correctable error detected */
#define PCI_EXP_DEVSTA_NFED 0x0002 /* non-fatal error detected */
#define PCI_EXP_DEVSTA_FED 0x0004 /* fatal error detected */
#define PCI_EXP_DEVSTA_URD 0x0008 /* unsupported request detected */
#define PCI_EXP_DEVSTA_AUXPD 0x0010 /* aux power detected */
#define PCI_EXP_DEVSTA_TRPND 0x0020 /* non-posted requests outstanding */
#define PCI_CAP_EXP_RC_ENDPOINT_SIZEOF_V1 12 /* v1 without link registers */

#define PCI_EXP_LNKCAP 0x0c /* 32 bits: link capabilities */
#define PCI_EXP_LNKCAP_SLS 0x0000000f /* max link speed (LNKCAP2 vector bit) */
#define PCI_EXP_LNKCAP_SLS_2_5GB 0x00000001 /* 2.5 GT/s */
#define PCI_EXP_LNKCAP_SLS_5_0GB 0x00000002 /* 5.0 GT/s */
#define PCI_EXP_LNKCAP_SLS_8_0GB 0x00000003 /* 8.0 GT/s */
#define PCI_EXP_LNKCAP_SLS_16_0GB 0x00000004 /* 16.0 GT/s */
#define PCI_EXP_LNKCAP_SLS_32_0GB 0x00000005 /* 32.0 GT/s */
#define PCI_EXP_LNKCAP_SLS_64_0GB 0x00000006 /* 64.0 GT/s */
#define PCI_EXP_LNKCAP_MLW 0x000003f0 /* maximum link width */
#define PCI_EXP_LNKCAP_ASPMS 0x00000c00 /* ASPM support */
#define PCI_EXP_LNKCAP_ASPM_L0S 0x00000400 /* ASPM L0s supported */
#define PCI_EXP_LNKCAP_ASPM_L1 0x00000800 /* ASPM L1 supported */
#define PCI_EXP_LNKCAP_L0SEL 0x00007000 /* L0s exit latency */
#define PCI_EXP_LNKCAP_L1EL 0x00038000 /* L1 exit latency */
#define PCI_EXP_LNKCAP_CLKPM 0x00040000 /* clock power management (CLKREQ#) */
#define PCI_EXP_LNKCAP_SDERC 0x00080000 /* surprise down reporting */
#define PCI_EXP_LNKCAP_DLLLARC 0x00100000 /* DL_Active reporting capable */
#define PCI_EXP_LNKCAP_LBNC 0x00200000 /* bandwidth notification capable */
#define PCI_EXP_LNKCAP_PN 0xff000000 /* port number */

#define PCI_EXP_LNKCTL 0x10 /* 16 bits: link control */
#define PCI_EXP_LNKCTL_ASPMC 0x0003 /* ASPM control */
#define PCI_EXP_LNKCTL_ASPM_L0S 0x0001 /* L0s enable */
#define PCI_EXP_LNKCTL_ASPM_L1 0x0002 /* L1 enable */
#define PCI_EXP_LNKCTL_RCB 0x0008 /* read completion boundary 128 B */
#define PCI_EXP_LNKCTL_LD 0x0010 /* link disable (downstream ports) */
#define PCI_EXP_LNKCTL_RL 0x0020 /* retrain link (downstream ports) */
#define PCI_EXP_LNKCTL_CCC 0x0040 /* common clock configuration */
#define PCI_EXP_LNKCTL_ES 0x0080 /* extended synch */
#define PCI_EXP_LNKCTL_CLKREQ_EN 0x0100 /* clock power management enable */
#define PCI_EXP_LNKCTL_HAWD 0x0200 /* hardware autonomous width disable */
#define PCI_EXP_LNKCTL_LBMIE 0x0400 /* bandwidth management irq enable */
#define PCI_EXP_LNKCTL_LABIE 0x0800 /* autonomous bandwidth irq enable */

#define PCI_EXP_LNKSTA 0x12 /* 16 bits: link status */
#define PCI_EXP_LNKSTA_CLS 0x000f /* current link speed */
#define PCI_EXP_LNKSTA_CLS_2_5GB 0x0001 /* 2.5 GT/s */
#define PCI_EXP_LNKSTA_CLS_5_0GB 0x0002 /* 5.0 GT/s */
#define PCI_EXP_LNKSTA_CLS_8_0GB 0x0003 /* 8.0 GT/s */
#define PCI_EXP_LNKSTA_CLS_16_0GB 0x0004 /* 16.0 GT/s */
#define PCI_EXP_LNKSTA_CLS_32_0GB 0x0005 /* 32.0 GT/s */
#define PCI_EXP_LNKSTA_CLS_64_0GB 0x0006 /* 64.0 GT/s */
#define PCI_EXP_LNKSTA_NLW 0x03f0 /* negotiated link width */
#define PCI_EXP_LNKSTA_NLW_X1 0x0010
#define PCI_EXP_LNKSTA_NLW_X2 0x0020
#define PCI_EXP_LNKSTA_NLW_X4 0x0040
#define PCI_EXP_LNKSTA_NLW_X8 0x0080
#define PCI_EXP_LNKSTA_NLW_SHIFT 4
#define PCI_EXP_LNKSTA_LT 0x0800 /* link training in progress */
#define PCI_EXP_LNKSTA_SLC 0x1000 /* slot uses the platform-provided clock */
#define PCI_EXP_LNKSTA_DLLLA 0x2000 /* data link layer link active */
#define PCI_EXP_LNKSTA_LBMS 0x4000 /* bandwidth management status (RW1C) */
#define PCI_EXP_LNKSTA_LABS 0x8000 /* autonomous bandwidth status (RW1C) */
#define PCI_CAP_EXP_ENDPOINT_SIZEOF_V1 20 /* v1 with link registers */

#define PCI_EXP_SLTCAP 0x14 /* 32 bits: slot capabilities */
#define PCI_EXP_SLTCAP_ABP 0x00000001 /* attention button present */
#define PCI_EXP_SLTCAP_PCP 0x00000002 /* power controller present */
#define PCI_EXP_SLTCAP_MRLSP 0x00000004 /* MRL sensor present */
#define PCI_EXP_SLTCAP_AIP 0x00000008 /* attention indicator present */
#define PCI_EXP_SLTCAP_PIP 0x00000010 /* power indicator present */
#define PCI_EXP_SLTCAP_HPS 0x00000020 /* hot-plug surprise */
#define PCI_EXP_SLTCAP_HPC 0x00000040 /* hot-plug capable */
#define PCI_EXP_SLTCAP_SPLV 0x00007f80 /* slot power limit value */
#define PCI_EXP_SLTCAP_SPLS 0x00018000 /* slot power limit scale */
#define PCI_EXP_SLTCAP_EIP 0x00020000 /* electromechanical interlock */
#define PCI_EXP_SLTCAP_NCCS 0x00040000 /* no command completed support */
#define PCI_EXP_SLTCAP_PSN 0xfff80000 /* physical slot number */

#define PCI_EXP_SLTCTL 0x18 /* 16 bits: slot control */
#define PCI_EXP_SLTCTL_ABPE 0x0001 /* attention button pressed enable */
#define PCI_EXP_SLTCTL_PFDE 0x0002 /* power fault detected enable */
#define PCI_EXP_SLTCTL_MRLSCE 0x0004 /* MRL sensor changed enable */
#define PCI_EXP_SLTCTL_PDCE 0x0008 /* presence detect changed enable */
#define PCI_EXP_SLTCTL_CCIE 0x0010 /* command completed irq enable */
#define PCI_EXP_SLTCTL_HPIE 0x0020 /* hot-plug interrupt enable */
#define PCI_EXP_SLTCTL_AIC 0x00c0 /* attention indicator control */
#define PCI_EXP_SLTCTL_ATTN_IND_SHIFT 6
#define PCI_EXP_SLTCTL_ATTN_IND_ON 0x0040
#define PCI_EXP_SLTCTL_ATTN_IND_BLINK 0x0080
#define PCI_EXP_SLTCTL_ATTN_IND_OFF 0x00c0
#define PCI_EXP_SLTCTL_PIC 0x0300 /* power indicator control */
#define PCI_EXP_SLTCTL_PWR_IND_ON 0x0100
#define PCI_EXP_SLTCTL_PWR_IND_BLINK 0x0200
#define PCI_EXP_SLTCTL_PWR_IND_OFF 0x0300
#define PCI_EXP_SLTCTL_PCC 0x0400 /* power controller control */
#define PCI_EXP_SLTCTL_PWR_ON 0x0000 /* slot power on */
#define PCI_EXP_SLTCTL_PWR_OFF 0x0400 /* slot power off */
#define PCI_EXP_SLTCTL_EIC 0x0800 /* electromechanical interlock control */
#define PCI_EXP_SLTCTL_DLLSCE 0x1000 /* DLL state changed enable */
#define PCI_EXP_SLTCTL_ASPL_DISABLE 0x2000 /* auto slot power limit disable */
#define PCI_EXP_SLTCTL_IBPD_DISABLE 0x4000 /* in-band presence detect off */

#define PCI_EXP_SLTSTA 0x1a /* 16 bits: slot status */
#define PCI_EXP_SLTSTA_ABP 0x0001 /* attention button pressed */
#define PCI_EXP_SLTSTA_PFD 0x0002 /* power fault detected */
#define PCI_EXP_SLTSTA_MRLSC 0x0004 /* MRL sensor changed */
#define PCI_EXP_SLTSTA_PDC 0x0008 /* presence detect changed */
#define PCI_EXP_SLTSTA_CC 0x0010 /* command completed */
#define PCI_EXP_SLTSTA_MRLSS 0x0020 /* MRL sensor state (1 = open) */
#define PCI_EXP_SLTSTA_PDS 0x0040 /* presence detect state */
#define PCI_EXP_SLTSTA_EIS 0x0080 /* electromechanical interlock state */
#define PCI_EXP_SLTSTA_DLLSC 0x0100 /* DLL state changed */

#define PCI_EXP_RTCTL 0x1c /* 16 bits: root control */
#define PCI_EXP_RTCTL_SECEE 0x0001 /* system error on correctable */
#define PCI_EXP_RTCTL_SENFEE 0x0002 /* system error on non-fatal */
#define PCI_EXP_RTCTL_SEFEE 0x0004 /* system error on fatal */
#define PCI_EXP_RTCTL_PMEIE 0x0008 /* PME interrupt enable */
#define PCI_EXP_RTCTL_RRS_SVE 0x0010 /* config RRS software visibility */
#define PCI_EXP_RTCTL_CRSSVE PCI_EXP_RTCTL_RRS_SVE /* old name */
#define PCI_EXP_RTCAP 0x1e /* 16 bits: root capabilities */
#define PCI_EXP_RTCAP_RRS_SV 0x0001 /* RRS software visibility capable */
#define PCI_EXP_RTCAP_CRSVIS PCI_EXP_RTCAP_RRS_SV /* old name */
#define PCI_EXP_RTSTA 0x20 /* 32 bits: root status */
#define PCI_EXP_RTSTA_PME_RQ_ID 0x0000ffff /* PME requester ID */
#define PCI_EXP_RTSTA_PME 0x00010000 /* PME status (RW1C) */
#define PCI_EXP_RTSTA_PENDING 0x00020000 /* further PMEs pending */

/* The registers from here on exist only with capability version 2
 * (PCI_EXP_FLAGS_VERS >= 2); on a version 1 capability they are absent
 * and must read as zero / be ignored. */

#define PCI_EXP_DEVCAP2 0x24 /* 32 bits: device capabilities 2 */
#define PCI_EXP_DEVCAP2_COMP_TMOUT_DIS 0x00000010 /* timeout disable supported */
#define PCI_EXP_DEVCAP2_ARI 0x00000020 /* ARI forwarding supported */
#define PCI_EXP_DEVCAP2_ATOMIC_ROUTE 0x00000040 /* AtomicOp routing */
#define PCI_EXP_DEVCAP2_ATOMIC_COMP32 0x00000080 /* 32-bit AtomicOp completer */
#define PCI_EXP_DEVCAP2_ATOMIC_COMP64 0x00000100 /* 64-bit AtomicOp completer */
#define PCI_EXP_DEVCAP2_ATOMIC_COMP128 0x00000200 /* 128-bit CAS completer */
#define PCI_EXP_DEVCAP2_LTR 0x00000800 /* latency tolerance reporting */
#define PCI_EXP_DEVCAP2_TPH_COMP_MASK 0x00003000 /* TPH completer support */
#define PCI_EXP_DEVCAP2_TPH_COMP_NONE 0x0
#define PCI_EXP_DEVCAP2_TPH_COMP_TPH_ONLY 0x1
#define PCI_EXP_DEVCAP2_TPH_COMP_EXT_TPH 0x3
#define PCI_EXP_DEVCAP2_OBFF_MASK 0x000c0000 /* OBFF mechanisms */
#define PCI_EXP_DEVCAP2_OBFF_MSG 0x00040000 /* OBFF by message */
#define PCI_EXP_DEVCAP2_OBFF_WAKE 0x00080000 /* OBFF by WAKE# */
#define PCI_EXP_DEVCAP2_EE_PREFIX 0x00200000 /* end-end TLP prefix supported */
#define PCI_EXP_DEVCAP2_EE_PREFIX_MAX 0x00c00000 /* max end-end TLP prefixes */
/* Completion timeout ranges supported (bits 3:0); LikeOS names */
#define PCI_EXP_DEVCAP2_COMP_TMOUT_RANGES 0x0000000f /* 0 = not reported */
#define PCI_EXP_DEVCAP2_COMP_TMOUT_RANGE_A 0x00000001 /* 50 us - 10 ms */
#define PCI_EXP_DEVCAP2_COMP_TMOUT_RANGE_B 0x00000002 /* 10 ms - 250 ms */
#define PCI_EXP_DEVCAP2_COMP_TMOUT_RANGE_C 0x00000004 /* 250 ms - 4 s */
#define PCI_EXP_DEVCAP2_COMP_TMOUT_RANGE_D 0x00000008 /* 4 s - 64 s */

#define PCI_EXP_DEVCTL2 0x28 /* 16 bits: device control 2 */
#define PCI_EXP_DEVCTL2_COMP_TIMEOUT 0x000f /* completion timeout value */
/* Completion timeout values; LikeOS names, named after the lower bound */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_DEFAULT 0x0000 /* 50 us - 50 ms */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_50US 0x0001 /* 50 us - 100 us (A) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_1MS 0x0002 /* 1 ms - 10 ms (A) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_16MS 0x0005 /* 16 ms - 55 ms (B) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_65MS 0x0006 /* 65 ms - 210 ms (B) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_260MS 0x0009 /* 260 ms - 900 ms (C) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_1S 0x000a /* 1 s - 3.5 s (C) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_4S 0x000d /* 4 s - 13 s (D) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_17S 0x000e /* 17 s - 64 s (D) */
#define PCI_EXP_DEVCTL2_COMP_TMOUT_DIS 0x0010 /* completion timeout off */
#define PCI_EXP_DEVCTL2_ARI 0x0020 /* ARI forwarding enable */
#define PCI_EXP_DEVCTL2_ATOMIC_REQ 0x0040 /* AtomicOp requester enable */
#define PCI_EXP_DEVCTL2_ATOMIC_EGRESS_BLOCK 0x0080 /* block AtomicOp egress */
#define PCI_EXP_DEVCTL2_IDO_REQ_EN 0x0100 /* ID-based ordering, requests */
#define PCI_EXP_DEVCTL2_IDO_CMP_EN 0x0200 /* ID-based ordering, completions */
#define PCI_EXP_DEVCTL2_LTR_EN 0x0400 /* LTR mechanism enable */
#define PCI_EXP_DEVCTL2_OBFF_MSGA_EN 0x2000 /* OBFF message variation A */
#define PCI_EXP_DEVCTL2_OBFF_MSGB_EN 0x4000 /* OBFF message variation B */
#define PCI_EXP_DEVCTL2_OBFF_WAKE_EN 0x6000 /* OBFF by WAKE# */
#define PCI_EXP_DEVSTA2 0x2a /* 16 bits: device status 2 */
#define PCI_CAP_EXP_RC_ENDPOINT_SIZEOF_V2 0x2c /* v2 without link registers */

#define PCI_EXP_LNKCAP2 0x2c /* 32 bits: link capabilities 2 */
#define PCI_EXP_LNKCAP2_SLS 0x000000fe /* supported link speeds vector */
#define PCI_EXP_LNKCAP2_SLS_2_5GB 0x00000002 /* 2.5 GT/s */
#define PCI_EXP_LNKCAP2_SLS_5_0GB 0x00000004 /* 5.0 GT/s */
#define PCI_EXP_LNKCAP2_SLS_8_0GB 0x00000008 /* 8.0 GT/s */
#define PCI_EXP_LNKCAP2_SLS_16_0GB 0x00000010 /* 16.0 GT/s */
#define PCI_EXP_LNKCAP2_SLS_32_0GB 0x00000020 /* 32.0 GT/s */
#define PCI_EXP_LNKCAP2_SLS_64_0GB 0x00000040 /* 64.0 GT/s */
#define PCI_EXP_LNKCAP2_CROSSLINK 0x00000100 /* crosslink supported */

#define PCI_EXP_LNKCTL2 0x30 /* 16 bits: link control 2 */
#define PCI_EXP_LNKCTL2_TLS 0x000f /* target link speed */
#define PCI_EXP_LNKCTL2_TLS_2_5GT 0x0001 /* 2.5 GT/s */
#define PCI_EXP_LNKCTL2_TLS_5_0GT 0x0002 /* 5.0 GT/s */
#define PCI_EXP_LNKCTL2_TLS_8_0GT 0x0003 /* 8.0 GT/s */
#define PCI_EXP_LNKCTL2_TLS_16_0GT 0x0004 /* 16.0 GT/s */
#define PCI_EXP_LNKCTL2_TLS_32_0GT 0x0005 /* 32.0 GT/s */
#define PCI_EXP_LNKCTL2_TLS_64_0GT 0x0006 /* 64.0 GT/s */
#define PCI_EXP_LNKCTL2_ENTER_COMP 0x0010 /* enter compliance */
#define PCI_EXP_LNKCTL2_HASD 0x0020 /* hardware autonomous speed disable */
#define PCI_EXP_LNKCTL2_TX_MARGIN 0x0380 /* transmit margin */
#define PCI_EXP_LNKSTA2 0x32 /* 16 bits: link status 2 */
#define PCI_EXP_LNKSTA2_FLIT 0x0400 /* link is in flit mode */
#define PCI_CAP_EXP_ENDPOINT_SIZEOF_V2 0x34 /* v2 with link registers */

#define PCI_EXP_SLTCAP2 0x34 /* 32 bits: slot capabilities 2 */
#define PCI_EXP_SLTCAP2_IBPD 0x00000001 /* in-band PD disable supported */
#define PCI_EXP_SLTCTL2 0x38 /* 16 bits: slot control 2 */
#define PCI_EXP_SLTSTA2 0x3a /* 16 bits: slot status 2 */

/* ==========================================================================
 * Extended capability list (PCI Express, configuration space >= 0x100)
 *
 * Every extended capability starts with a 32-bit header:
 *   bits 15..0  capability ID (PCI_EXT_CAP_ID_*)
 *   bits 19..16 capability version
 *   bits 31..20 offset of the next capability (0 = end of list)
 * ========================================================================== */

#define PCI_EXT_CAP_ID(header) ((header) & 0x0000ffff)
#define PCI_EXT_CAP_VER(header) (((header) >> 16) & 0xf)
#define PCI_EXT_CAP_NEXT(header) (((header) >> 20) & 0xffc)

#define PCI_EXT_CAP_ID_ERR 0x01 /* advanced error reporting */
#define PCI_EXT_CAP_ID_VC 0x02 /* virtual channel */
#define PCI_EXT_CAP_ID_DSN 0x03 /* device serial number */
#define PCI_EXT_CAP_ID_PWR 0x04 /* power budgeting */
#define PCI_EXT_CAP_ID_RCLD 0x05 /* root complex link declaration */
#define PCI_EXT_CAP_ID_RCILC 0x06 /* root complex internal link control */
#define PCI_EXT_CAP_ID_RCEC 0x07 /* root complex event collector */
#define PCI_EXT_CAP_ID_MFVC 0x08 /* multi-function virtual channel */
#define PCI_EXT_CAP_ID_VC9 0x09 /* virtual channel (MFVC present) */
#define PCI_EXT_CAP_ID_RCRB 0x0A /* root complex register block */
#define PCI_EXT_CAP_ID_VNDR 0x0B /* vendor specific (VSEC) */
#define PCI_EXT_CAP_ID_CAC 0x0C /* config access correlation (obsolete) */
#define PCI_EXT_CAP_ID_ACS 0x0D /* access control services */
#define PCI_EXT_CAP_ID_ARI 0x0E /* alternative routing-ID */
#define PCI_EXT_CAP_ID_ATS 0x0F /* address translation services */
#define PCI_EXT_CAP_ID_SRIOV 0x10 /* single root I/O virtualization */
#define PCI_EXT_CAP_ID_MRIOV 0x11 /* multi root I/O virtualization */
#define PCI_EXT_CAP_ID_MCAST 0x12 /* multicast */
#define PCI_EXT_CAP_ID_PRI 0x13 /* page request interface */
#define PCI_EXT_CAP_ID_AMD_XXX 0x14 /* reserved for AMD */
#define PCI_EXT_CAP_ID_REBAR 0x15 /* resizable BAR */
#define PCI_EXT_CAP_ID_DPA 0x16 /* dynamic power allocation */
#define PCI_EXT_CAP_ID_TPH 0x17 /* TPH requester */
#define PCI_EXT_CAP_ID_LTR 0x18 /* latency tolerance reporting */
#define PCI_EXT_CAP_ID_SECPCI 0x19 /* secondary PCIe (8.0 GT/s) */
#define PCI_EXT_CAP_ID_PMUX 0x1A /* protocol multiplexing */
#define PCI_EXT_CAP_ID_PASID 0x1B /* process address space ID */
#define PCI_EXT_CAP_ID_DPC 0x1D /* downstream port containment */
#define PCI_EXT_CAP_ID_L1SS 0x1E /* L1 PM substates */
#define PCI_EXT_CAP_ID_PTM 0x1F /* precision time measurement */
#define PCI_EXT_CAP_ID_DVSEC 0x23 /* designated vendor specific */
#define PCI_EXT_CAP_ID_VF_REBAR 0x24 /* VF resizable BAR */
#define PCI_EXT_CAP_ID_DLF 0x25 /* data link feature */
#define PCI_EXT_CAP_ID_PL_16GT 0x26 /* physical layer 16.0 GT/s */
#define PCI_EXT_CAP_ID_NPEM 0x29 /* native PCIe enclosure management */
#define PCI_EXT_CAP_ID_PL_32GT 0x2A /* physical layer 32.0 GT/s */
#define PCI_EXT_CAP_ID_DOE 0x2E /* data object exchange */
#define PCI_EXT_CAP_ID_DEV3 0x2F /* device 3 */
#define PCI_EXT_CAP_ID_IDE 0x30 /* integrity and data encryption */
#define PCI_EXT_CAP_ID_PL_64GT 0x31 /* physical layer 64.0 GT/s */
#define PCI_EXT_CAP_ID_MAX PCI_EXT_CAP_ID_PL_64GT

/* ==========================================================================
 * Advanced error reporting (PCI_EXT_CAP_ID_ERR)
 * ========================================================================== */

#define PCI_ERR_UNCOR_STATUS 0x04 /* uncorrectable error status (RW1C) */
#define PCI_ERR_UNC_UND 0x00000001 /* undefined (training error) */
#define PCI_ERR_UNC_DLP 0x00000010 /* data link protocol error */
#define PCI_ERR_UNC_SURPDN 0x00000020 /* surprise down */
#define PCI_ERR_UNC_POISON_TLP 0x00001000 /* poisoned TLP received */
#define PCI_ERR_UNC_FCP 0x00002000 /* flow control protocol error */
#define PCI_ERR_UNC_COMP_TIME 0x00004000 /* completion timeout */
#define PCI_ERR_UNC_COMP_ABORT 0x00008000 /* completer abort */
#define PCI_ERR_UNC_UNX_COMP 0x00010000 /* unexpected completion */
#define PCI_ERR_UNC_RX_OVER 0x00020000 /* receiver overflow */
#define PCI_ERR_UNC_MALF_TLP 0x00040000 /* malformed TLP */
#define PCI_ERR_UNC_ECRC 0x00080000 /* ECRC error */
#define PCI_ERR_UNC_UNSUP 0x00100000 /* unsupported request */
#define PCI_ERR_UNC_ACSV 0x00200000 /* ACS violation */
#define PCI_ERR_UNC_INTN 0x00400000 /* uncorrectable internal error */
#define PCI_ERR_UNC_MCBTLP 0x00800000 /* multicast blocked TLP */
#define PCI_ERR_UNC_ATOMEG 0x01000000 /* AtomicOp egress blocked */
#define PCI_ERR_UNC_TLPPRE 0x02000000 /* TLP prefix blocked */
#define PCI_ERR_UNC_POISON_BLK 0x04000000 /* poisoned TLP egress blocked */
#define PCI_ERR_UNC_DMWR_BLK 0x08000000 /* DMWr request egress blocked */
#define PCI_ERR_UNC_IDE_CHECK 0x10000000 /* IDE check failed */
#define PCI_ERR_UNC_MISR_IDE 0x20000000 /* misrouted IDE TLP */
#define PCI_ERR_UNC_PCRC_CHECK 0x40000000 /* PCRC check failed */
#define PCI_ERR_UNC_XLAT_BLK 0x80000000 /* TLP translation egress blocked */
#define PCI_ERR_UNCOR_MASK 0x08 /* uncorrectable error mask, same bits */
#define PCI_ERR_UNCOR_SEVER 0x0c /* 1 = fatal, 0 = non-fatal, same bits */
#define PCI_ERR_COR_STATUS 0x10 /* correctable error status (RW1C) */
#define PCI_ERR_COR_RCVR 0x00000001 /* receiver error */
#define PCI_ERR_COR_BAD_TLP 0x00000040 /* bad TLP */
#define PCI_ERR_COR_BAD_DLLP 0x00000080 /* bad DLLP */
#define PCI_ERR_COR_REP_ROLL 0x00000100 /* REPLAY_NUM rollover */
#define PCI_ERR_COR_REP_TIMER 0x00001000 /* replay timer timeout */
#define PCI_ERR_COR_ADV_NFAT 0x00002000 /* advisory non-fatal */
#define PCI_ERR_COR_INTERNAL 0x00004000 /* corrected internal error */
#define PCI_ERR_COR_LOG_OVER 0x00008000 /* header log overflow */
#define PCI_ERR_COR_MASK 0x14 /* correctable error mask, same bits */
#define PCI_ERR_CAP 0x18 /* capabilities and control */
#define PCI_ERR_CAP_FEP(x) ((x) & 0x1f) /* first error pointer (bit no.) */
#define PCI_ERR_CAP_ECRC_GENC 0x00000020 /* ECRC generation capable */
#define PCI_ERR_CAP_ECRC_GENE 0x00000040 /* ECRC generation enable */
#define PCI_ERR_CAP_ECRC_CHKC 0x00000080 /* ECRC check capable */
#define PCI_ERR_CAP_ECRC_CHKE 0x00000100 /* ECRC check enable */
#define PCI_ERR_CAP_PREFIX_LOG_PRESENT 0x00000800 /* TLP prefix log valid */
#define PCI_ERR_CAP_COMP_TIME_LOG 0x00001000 /* timeout header log capable */
#define PCI_ERR_CAP_TLP_LOG_FLIT 0x00040000 /* TLP logged in flit mode */
#define PCI_ERR_CAP_TLP_LOG_SIZE 0x00f80000 /* logged TLP size (flit mode) */
#define PCI_ERR_HEADER_LOG 0x1c /* header of the failing TLP, 16 bytes */
#define PCI_ERR_ROOT_COMMAND 0x2c /* root error command */
#define PCI_ERR_ROOT_CMD_COR_EN 0x00000001 /* irq on correctable */
#define PCI_ERR_ROOT_CMD_NONFATAL_EN 0x00000002 /* irq on non-fatal */
#define PCI_ERR_ROOT_CMD_FATAL_EN 0x00000004 /* irq on fatal */
#define PCI_ERR_ROOT_STATUS 0x30 /* root error status (RW1C) */
#define PCI_ERR_ROOT_COR_RCV 0x00000001 /* ERR_COR received */
#define PCI_ERR_ROOT_MULTI_COR_RCV 0x00000002 /* ERR_COR while one logged */
#define PCI_ERR_ROOT_UNCOR_RCV 0x00000004 /* ERR_FATAL/NONFATAL received */
#define PCI_ERR_ROOT_MULTI_UNCOR_RCV 0x00000008 /* further FATAL/NONFATAL */
#define PCI_ERR_ROOT_FIRST_FATAL 0x00000010 /* first uncorrectable was fatal */
#define PCI_ERR_ROOT_NONFATAL_RCV 0x00000020 /* ERR_NONFATAL received */
#define PCI_ERR_ROOT_FATAL_RCV 0x00000040 /* ERR_FATAL received */
#define PCI_ERR_ROOT_AER_IRQ 0xf8000000 /* AER interrupt message number */
#define PCI_ERR_ROOT_ERR_SRC 0x34 /* source IDs: COR [15:0], UNCOR [31:16] */
#define PCI_ERR_PREFIX_LOG 0x38 /* TLP prefix log, up to 16 bytes */

/* ==========================================================================
 * Device serial number (PCI_EXT_CAP_ID_DSN)
 *
 * The 64-bit serial (an EUI-64) as two dwords.  The offsets have no
 * conventional names; PCI_DSN_LO / PCI_DSN_HI are LikeOS names.
 * ========================================================================== */

#define PCI_DSN_LO 0x04 /* serial number bits 31..0 */
#define PCI_DSN_HI 0x08 /* serial number bits 63..32 */
#define PCI_EXT_CAP_DSN_SIZEOF 12

/* ==========================================================================
 * Vendor-specific extended capability (PCI_EXT_CAP_ID_VNDR)
 * ========================================================================== */

#define PCI_VNDR_HEADER 4 /* 32 bits: VSEC header */
#define PCI_VNDR_HEADER_ID(x) ((x) & 0xffff) /* vendor defined ID */
#define PCI_VNDR_HEADER_REV(x) (((x) >> 16) & 0xf) /* revision */
#define PCI_VNDR_HEADER_LEN(x) (((x) >> 20) & 0xfff) /* length in bytes */

/* ==========================================================================
 * Designated vendor-specific extended capability (PCI_EXT_CAP_ID_DVSEC)
 * ========================================================================== */

#define PCI_DVSEC_HEADER1 0x4 /* vendor ID, revision, length */
#define PCI_DVSEC_HEADER1_VID(x) ((x) & 0xffff)
#define PCI_DVSEC_HEADER1_REV(x) (((x) >> 16) & 0xf)
#define PCI_DVSEC_HEADER1_LEN(x) (((x) >> 20) & 0xfff)
#define PCI_DVSEC_HEADER2 0x8 /* DVSEC ID */
#define PCI_DVSEC_HEADER2_ID(x) ((x) & 0xffff)

/* ==========================================================================
 * Alternative routing-ID interpretation (PCI_EXT_CAP_ID_ARI)
 * ========================================================================== */

#define PCI_ARI_CAP 0x04 /* 16 bits */
#define PCI_ARI_CAP_MFVC 0x0001 /* MFVC function groups capable */
#define PCI_ARI_CAP_ACS 0x0002 /* ACS function groups capable */
#define PCI_ARI_CAP_NFN(x) (((x) >> 8) & 0xff) /* next function number */
#define PCI_ARI_CTRL 0x06 /* 16 bits */
#define PCI_ARI_CTRL_MFVC 0x0001 /* MFVC function groups enable */
#define PCI_ARI_CTRL_ACS 0x0002 /* ACS function groups enable */
#define PCI_ARI_CTRL_FG(x) (((x) >> 4) & 7) /* function group */
#define PCI_EXT_CAP_ARI_SIZEOF 8

/* ==========================================================================
 * Access control services (PCI_EXT_CAP_ID_ACS)
 *
 * The capability bits at PCI_ACS_CAP and the enable bits at
 * PCI_ACS_CTRL share one layout.
 * ========================================================================== */

#define PCI_ACS_CAP 0x04 /* 16 bits */
#define PCI_ACS_SV 0x0001 /* source validation */
#define PCI_ACS_TB 0x0002 /* translation blocking */
#define PCI_ACS_RR 0x0004 /* P2P request redirect */
#define PCI_ACS_CR 0x0008 /* P2P completion redirect */
#define PCI_ACS_UF 0x0010 /* upstream forwarding */
#define PCI_ACS_EC 0x0020 /* P2P egress control */
#define PCI_ACS_DT 0x0040 /* direct translated P2P */
#define PCI_ACS_EGRESS_BITS 0x05 /* 8 bits: egress control vector size */
#define PCI_ACS_CTRL 0x06 /* 16 bits */
#define PCI_ACS_EGRESS_CTL_V 0x08 /* egress control vector */

/* ==========================================================================
 * Address translation services (PCI_EXT_CAP_ID_ATS)
 * ========================================================================== */

#define PCI_ATS_CAP 0x04 /* 16 bits */
#define PCI_ATS_CAP_QDEP(x) ((x) & 0x1f) /* invalidate queue depth, 0 = 32 */
#define PCI_ATS_MAX_QDEP 32
#define PCI_ATS_CAP_PAGE_ALIGNED 0x0020 /* page aligned requests */
#define PCI_ATS_CTRL 0x06 /* 16 bits */
#define PCI_ATS_CTRL_ENABLE 0x8000 /* ATS enable */
#define PCI_ATS_CTRL_STU(x) ((x) & 0x1f) /* smallest translation unit */
#define PCI_ATS_MIN_STU 12 /* STU 0 = 4 KiB */
#define PCI_EXT_CAP_ATS_SIZEOF 8

/* ==========================================================================
 * Page request interface (PCI_EXT_CAP_ID_PRI)
 * ========================================================================== */

#define PCI_PRI_CTRL 0x04 /* 16 bits */
#define PCI_PRI_CTRL_ENABLE 0x0001
#define PCI_PRI_CTRL_RESET 0x0002
#define PCI_PRI_STATUS 0x06 /* 16 bits */
#define PCI_PRI_STATUS_RF 0x0001 /* response failure */
#define PCI_PRI_STATUS_UPRGI 0x0002 /* unexpected page request group index */
#define PCI_PRI_STATUS_STOPPED 0x0100 /* PRI stopped */
#define PCI_PRI_STATUS_PASID 0x8000 /* PRG response needs a PASID */
#define PCI_PRI_MAX_REQ 0x08 /* 32 bits: outstanding requests capacity */
#define PCI_PRI_ALLOC_REQ 0x0c /* 32 bits: outstanding requests allowed */
#define PCI_EXT_CAP_PRI_SIZEOF 16

/* ==========================================================================
 * Process address space ID (PCI_EXT_CAP_ID_PASID)
 * ========================================================================== */

#define PCI_PASID_CAP 0x04 /* 16 bits */
#define PCI_PASID_CAP_EXEC 0x0002 /* execute permission supported */
#define PCI_PASID_CAP_PRIV 0x0004 /* privileged mode supported */
#define PCI_PASID_CAP_WIDTH 0x1f00 /* max PASID width */
#define PCI_PASID_CTRL 0x06 /* 16 bits */
#define PCI_PASID_CTRL_ENABLE 0x0001
#define PCI_PASID_CTRL_EXEC 0x0002
#define PCI_PASID_CTRL_PRIV 0x0004
#define PCI_EXT_CAP_PASID_SIZEOF 8

/* ==========================================================================
 * Single root I/O virtualization (PCI_EXT_CAP_ID_SRIOV)
 * ========================================================================== */

#define PCI_SRIOV_CAP 0x04 /* 32 bits: SR-IOV capabilities */
#define PCI_SRIOV_CAP_VFM 0x00000001 /* VF migration capable */
#define PCI_SRIOV_CAP_INTR(x) ((x) >> 21) /* migration irq message number */
#define PCI_SRIOV_CTRL 0x08 /* 16 bits: SR-IOV control */
#define PCI_SRIOV_CTRL_VFE 0x0001 /* VF enable */
#define PCI_SRIOV_CTRL_VFM 0x0002 /* VF migration enable */
#define PCI_SRIOV_CTRL_INTR 0x0004 /* VF migration irq enable */
#define PCI_SRIOV_CTRL_MSE 0x0008 /* VF memory space enable */
#define PCI_SRIOV_CTRL_ARI 0x0010 /* ARI capable hierarchy */
#define PCI_SRIOV_STATUS 0x0a /* 16 bits */
#define PCI_SRIOV_STATUS_VFM 0x0001 /* VF migration status */
#define PCI_SRIOV_INITIAL_VF 0x0c /* 16 bits: initial VFs */
#define PCI_SRIOV_TOTAL_VF 0x0e /* 16 bits: total VFs */
#define PCI_SRIOV_NUM_VF 0x10 /* 16 bits: number of VFs enabled */
#define PCI_SRIOV_FUNC_LINK 0x12 /* 8 bits: function dependency link */
#define PCI_SRIOV_VF_OFFSET 0x14 /* 16 bits: routing ID of the first VF */
#define PCI_SRIOV_VF_STRIDE 0x16 /* 16 bits: routing ID stride */
#define PCI_SRIOV_VF_DID 0x1a /* 16 bits: VF device ID */
#define PCI_SRIOV_SUP_PGSIZE 0x1c /* 32 bits: supported page sizes */
#define PCI_SRIOV_SYS_PGSIZE 0x20 /* 32 bits: system page size */
#define PCI_SRIOV_BAR 0x24 /* VF BAR0, five more follow */
#define PCI_SRIOV_NUM_BARS 6
#define PCI_SRIOV_VFM 0x3c /* VF migration state array offset */
#define PCI_SRIOV_VFM_BIR(x) ((x) & 7)
#define PCI_SRIOV_VFM_OFFSET(x) ((x) & ~7)
#define PCI_SRIOV_VFM_UA 0x0 /* inactive.unavailable */
#define PCI_SRIOV_VFM_MI 0x1 /* dormant.migrate-in */
#define PCI_SRIOV_VFM_MO 0x2 /* active.migrate-out */
#define PCI_SRIOV_VFM_AV 0x3 /* active.available */
#define PCI_EXT_CAP_SRIOV_SIZEOF 0x40

/* ==========================================================================
 * Latency tolerance reporting (PCI_EXT_CAP_ID_LTR)
 *
 * latency = value * 32^scale ns
 * ========================================================================== */

#define PCI_LTR_MAX_SNOOP_LAT 0x4 /* 16 bits */
#define PCI_LTR_MAX_NOSNOOP_LAT 0x6 /* 16 bits */
#define PCI_LTR_VALUE_MASK 0x000003ff
#define PCI_LTR_SCALE_MASK 0x00001c00
#define PCI_LTR_SCALE_SHIFT 10
#define PCI_LTR_NOSNOOP_VALUE 0x03ff0000 /* no-snoop value, dword view */
#define PCI_LTR_NOSNOOP_SCALE 0x1c000000 /* no-snoop scale, dword view */
/* Scale encodings; LikeOS names */
#define PCI_LTR_SCALE_1NS 0
#define PCI_LTR_SCALE_32NS 1
#define PCI_LTR_SCALE_1024NS 2
#define PCI_LTR_SCALE_32768NS 3
#define PCI_LTR_SCALE_1048576NS 4
#define PCI_LTR_SCALE_33554432NS 5
#define PCI_EXT_CAP_LTR_SIZEOF 8

/* ==========================================================================
 * L1 PM substates (PCI_EXT_CAP_ID_L1SS)
 * ========================================================================== */

#define PCI_L1SS_CAP 0x04 /* 32 bits: capabilities */
#define PCI_L1SS_CAP_PCIPM_L1_2 0x00000001 /* PCI-PM L1.2 */
#define PCI_L1SS_CAP_PCIPM_L1_1 0x00000002 /* PCI-PM L1.1 */
#define PCI_L1SS_CAP_ASPM_L1_2 0x00000004 /* ASPM L1.2 */
#define PCI_L1SS_CAP_ASPM_L1_1 0x00000008 /* ASPM L1.1 */
#define PCI_L1SS_CAP_L1_PM_SS 0x00000010 /* L1 PM substates supported */
#define PCI_L1SS_CAP_CM_RESTORE_TIME 0x0000ff00 /* common mode restore, us */
#define PCI_L1SS_CAP_P_PWR_ON_SCALE 0x00030000 /* port T_POWER_ON scale */
#define PCI_L1SS_CAP_P_PWR_ON_VALUE 0x00f80000 /* port T_POWER_ON value */
#define PCI_L1SS_CTL1 0x08 /* 32 bits: control 1 */
#define PCI_L1SS_CTL1_PCIPM_L1_2 0x00000001
#define PCI_L1SS_CTL1_PCIPM_L1_1 0x00000002
#define PCI_L1SS_CTL1_ASPM_L1_2 0x00000004
#define PCI_L1SS_CTL1_ASPM_L1_1 0x00000008
#define PCI_L1SS_CTL1_L1_2_MASK 0x00000005 /* both L1.2 enables */
#define PCI_L1SS_CTL1_L1SS_MASK 0x0000000f /* all four enables */
#define PCI_L1SS_CTL1_CM_RESTORE_TIME 0x0000ff00
#define PCI_L1SS_CTL1_LTR_L12_TH_VALUE 0x03ff0000 /* L1.2 LTR threshold */
#define PCI_L1SS_CTL1_LTR_L12_TH_SCALE 0xe0000000 /* threshold scale */
#define PCI_L1SS_CTL2 0x0c /* 32 bits: control 2 */
#define PCI_L1SS_CTL2_T_PWR_ON_SCALE 0x00000003
#define PCI_L1SS_CTL2_T_PWR_ON_VALUE 0x000000f8

/* ==========================================================================
 * Resizable BAR (PCI_EXT_CAP_ID_REBAR, PCI_EXT_CAP_ID_VF_REBAR)
 *
 * One CAP/CTRL pair per resizable BAR, 8 bytes apart.  Size encoding
 * n means 1 MiB << n.
 * ========================================================================== */

#define PCI_REBAR_CAP 4 /* 32 bits: supported sizes */
#define PCI_REBAR_CAP_SIZES 0xFFFFFFF0 /* bit n+4 = 1 MiB << n supported */
#define PCI_REBAR_CTRL 8 /* 32 bits */
#define PCI_REBAR_CTRL_BAR_IDX 0x00000007 /* BAR this pair controls */
#define PCI_REBAR_CTRL_NBAR_MASK 0x000000E0 /* number of resizable BARs */
#define PCI_REBAR_CTRL_NBAR_SHIFT 5
#define PCI_REBAR_CTRL_BAR_SIZE 0x00001F00 /* current size encoding */
#define PCI_REBAR_CTRL_BAR_SHIFT 8

#define PCI_VF_REBAR_CAP PCI_REBAR_CAP
#define PCI_VF_REBAR_CAP_SIZES PCI_REBAR_CAP_SIZES
#define PCI_VF_REBAR_CTRL PCI_REBAR_CTRL
#define PCI_VF_REBAR_CTRL_BAR_IDX PCI_REBAR_CTRL_BAR_IDX
#define PCI_VF_REBAR_CTRL_NBAR_MASK PCI_REBAR_CTRL_NBAR_MASK
#define PCI_VF_REBAR_CTRL_BAR_SIZE PCI_REBAR_CTRL_BAR_SIZE

/* ==========================================================================
 * Downstream port containment (PCI_EXT_CAP_ID_DPC)
 * ========================================================================== */

#define PCI_EXP_DPC_CAP 0x04 /* 16 bits: DPC capability */
#define PCI_EXP_DPC_IRQ 0x001F /* DPC interrupt message number */
#define PCI_EXP_DPC_CAP_RP_EXT 0x0020 /* root port extensions */
#define PCI_EXP_DPC_CAP_POISONED_TLP 0x0040 /* poisoned TLP egress blocking */
#define PCI_EXP_DPC_CAP_SW_TRIGGER 0x0080 /* software triggering */
#define PCI_EXP_DPC_RP_PIO_LOG_SIZE 0x0F00 /* RP PIO log size [3:0] */
#define PCI_EXP_DPC_CAP_DL_ACTIVE 0x1000 /* ERR_COR on DL_Active */
#define PCI_EXP_DPC_RP_PIO_LOG_SIZE4 0x2000 /* RP PIO log size [4] */

#define PCI_EXP_DPC_CTL 0x06 /* 16 bits: DPC control */
#define PCI_EXP_DPC_CTL_EN_FATAL 0x0001 /* trigger on ERR_FATAL */
#define PCI_EXP_DPC_CTL_EN_NONFATAL 0x0002 /* trigger on ERR_NONFATAL too */
#define PCI_EXP_DPC_CTL_INT_EN 0x0008 /* DPC interrupt enable */

#define PCI_EXP_DPC_STATUS 0x08 /* 16 bits: DPC status */
#define PCI_EXP_DPC_STATUS_TRIGGER 0x0001 /* containment triggered (RW1C) */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN 0x0006 /* trigger reason */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_UNCOR 0x0000 /* uncorrectable error */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_NFE 0x0002 /* ERR_NONFATAL received */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE 0x0004 /* ERR_FATAL received */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_IN_EXT 0x0006 /* see extension field */
#define PCI_EXP_DPC_STATUS_INTERRUPT 0x0008 /* DPC interrupt status */
#define PCI_EXP_DPC_RP_BUSY 0x0010 /* root port busy */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_EXT 0x0060 /* trigger reason extension */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO 0x0000 /* RP PIO error */
#define PCI_EXP_DPC_STATUS_TRIGGER_RSN_SW_TRIGGER 0x0020 /* software trigger */
#define PCI_EXP_DPC_RP_PIO_FEP 0x1f00 /* RP PIO first error pointer */

#define PCI_EXP_DPC_SOURCE_ID 0x0A /* 16 bits: requester that triggered */

#define PCI_EXP_DPC_RP_PIO_STATUS 0x0C
#define PCI_EXP_DPC_RP_PIO_MASK 0x10
#define PCI_EXP_DPC_RP_PIO_SEVERITY 0x14
#define PCI_EXP_DPC_RP_PIO_SYSERROR 0x18
#define PCI_EXP_DPC_RP_PIO_EXCEPTION 0x1C
#define PCI_EXP_DPC_RP_PIO_HEADER_LOG 0x20
#define PCI_EXP_DPC_RP_PIO_IMPSPEC_LOG 0x30
#define PCI_EXP_DPC_RP_PIO_TLPPREFIX_LOG 0x34

/* ==========================================================================
 * Precision time measurement (PCI_EXT_CAP_ID_PTM)
 * ========================================================================== */

#define PCI_PTM_CAP 0x04 /* 32 bits */
#define PCI_PTM_CAP_REQ 0x00000001 /* requester capable */
#define PCI_PTM_CAP_RES 0x00000002 /* responder capable */
#define PCI_PTM_CAP_ROOT 0x00000004 /* root capable */
#define PCI_PTM_GRANULARITY_MASK 0x0000FF00 /* local clock granularity, ns */
#define PCI_PTM_CTRL 0x08 /* 32 bits */
#define PCI_PTM_CTRL_ENABLE 0x00000001
#define PCI_PTM_CTRL_ROOT 0x00000002 /* root select */

/* ==========================================================================
 * Secondary PCI Express (PCI_EXT_CAP_ID_SECPCI), data link feature
 * (PCI_EXT_CAP_ID_DLF), physical layer 16/32/64 GT/s
 * ========================================================================== */

#define PCI_SECPCI_LE_CTRL 0x0c /* lane equalization control, per lane */

#define PCI_DLF_CAP 0x04 /* 32 bits */
#define PCI_DLF_EXCHANGE_ENABLE 0x80000000 /* feature exchange enable */

#define PCI_PL_16GT_LE_CTRL 0x20 /* lane equalization control, per lane */
#define PCI_PL_16GT_LE_CTRL_DSP_TX_PRESET_MASK 0x0000000F
#define PCI_PL_16GT_LE_CTRL_USP_TX_PRESET_MASK 0x000000F0
#define PCI_PL_16GT_LE_CTRL_USP_TX_PRESET_SHIFT 4
#define PCI_PL_32GT_LE_CTRL 0x20 /* lane equalization control, per lane */
#define PCI_PL_64GT_LE_CTRL 0x20 /* lane equalization control, per lane */

/* ==========================================================================
 * Native PCIe enclosure management (PCI_EXT_CAP_ID_NPEM)
 *
 * Drive bay indications; the indication and reset bits share one layout
 * in the capability and the control register.
 * ========================================================================== */

#define PCI_NPEM_CAP 0x04 /* 32 bits */
#define PCI_NPEM_CAP_CAPABLE 0x00000001
#define PCI_NPEM_CTRL 0x08 /* 32 bits */
#define PCI_NPEM_CTRL_ENABLE 0x00000001
#define PCI_NPEM_CMD_RESET 0x00000002 /* reset command */
#define PCI_NPEM_IND_OK 0x00000004
#define PCI_NPEM_IND_LOCATE 0x00000008
#define PCI_NPEM_IND_FAIL 0x00000010
#define PCI_NPEM_IND_REBUILD 0x00000020
#define PCI_NPEM_IND_PFA 0x00000040 /* predicted failure analysis */
#define PCI_NPEM_IND_HOTSPARE 0x00000080
#define PCI_NPEM_IND_ICA 0x00000100 /* in critical array */
#define PCI_NPEM_IND_IFA 0x00000200 /* in failed array */
#define PCI_NPEM_IND_IDT 0x00000400 /* device type */
#define PCI_NPEM_IND_DISABLED 0x00000800
#define PCI_NPEM_IND_SPEC_0 0x01000000 /* enclosure specific 0..7 */
#define PCI_NPEM_IND_SPEC_1 0x02000000
#define PCI_NPEM_IND_SPEC_2 0x04000000
#define PCI_NPEM_IND_SPEC_3 0x08000000
#define PCI_NPEM_IND_SPEC_4 0x10000000
#define PCI_NPEM_IND_SPEC_5 0x20000000
#define PCI_NPEM_IND_SPEC_6 0x40000000
#define PCI_NPEM_IND_SPEC_7 0x80000000
#define PCI_NPEM_STATUS 0x0c /* 32 bits */
#define PCI_NPEM_STATUS_CC 0x00000001 /* command completed (RW1C) */

/* ==========================================================================
 * Class codes
 *
 * PCI_BASE_CLASS_* is the base class byte; the four-digit
 * PCI_CLASS_* values are base class << 8 | subclass (PCI_CLASS_DEVICE);
 * the six-digit ones add the programming interface (PCI_CLASS_REVISION
 * >> 8).
 * ========================================================================== */

#define PCI_CLASS_NOT_DEFINED 0x0000
#define PCI_CLASS_NOT_DEFINED_VGA 0x0001

#define PCI_BASE_CLASS_STORAGE 0x01
#define PCI_CLASS_STORAGE_SCSI 0x0100
#define PCI_CLASS_STORAGE_IDE 0x0101
#define PCI_CLASS_STORAGE_FLOPPY 0x0102
#define PCI_CLASS_STORAGE_IPI 0x0103
#define PCI_CLASS_STORAGE_RAID 0x0104
#define PCI_CLASS_STORAGE_SATA 0x0106
#define PCI_CLASS_STORAGE_SATA_AHCI 0x010601 /* SATA, AHCI 1.0 interface */
#define PCI_CLASS_STORAGE_SAS 0x0107
#define PCI_CLASS_STORAGE_EXPRESS 0x010802 /* NVM Express */
#define PCI_CLASS_STORAGE_OTHER 0x0180

#define PCI_BASE_CLASS_NETWORK 0x02
#define PCI_CLASS_NETWORK_ETHERNET 0x0200
#define PCI_CLASS_NETWORK_OTHER 0x0280

#define PCI_BASE_CLASS_DISPLAY 0x03
#define PCI_CLASS_DISPLAY_VGA 0x0300
#define PCI_CLASS_DISPLAY_XGA 0x0301
#define PCI_CLASS_DISPLAY_3D 0x0302
#define PCI_CLASS_DISPLAY_OTHER 0x0380

#define PCI_BASE_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_MULTIMEDIA_VIDEO 0x0400
#define PCI_CLASS_MULTIMEDIA_AUDIO 0x0401
#define PCI_CLASS_MULTIMEDIA_PHONE 0x0402
#define PCI_CLASS_MULTIMEDIA_HD_AUDIO 0x0403
#define PCI_CLASS_MULTIMEDIA_OTHER 0x0480

#define PCI_BASE_CLASS_MEMORY 0x05
#define PCI_CLASS_MEMORY_RAM 0x0500
#define PCI_CLASS_MEMORY_FLASH 0x0501
#define PCI_CLASS_MEMORY_CXL 0x0502
#define PCI_CLASS_MEMORY_OTHER 0x0580

#define PCI_BASE_CLASS_BRIDGE 0x06
#define PCI_CLASS_BRIDGE_HOST 0x0600
#define PCI_CLASS_BRIDGE_ISA 0x0601
#define PCI_CLASS_BRIDGE_EISA 0x0602
#define PCI_CLASS_BRIDGE_MC 0x0603
#define PCI_CLASS_BRIDGE_PCI 0x0604
#define PCI_CLASS_BRIDGE_PCI_NORMAL 0x060400
#define PCI_CLASS_BRIDGE_PCI_SUBTRACTIVE 0x060401
#define PCI_CLASS_BRIDGE_PCMCIA 0x0605
#define PCI_CLASS_BRIDGE_NUBUS 0x0606
#define PCI_CLASS_BRIDGE_CARDBUS 0x0607
#define PCI_CLASS_BRIDGE_RACEWAY 0x0608
#define PCI_CLASS_BRIDGE_OTHER 0x0680

#define PCI_BASE_CLASS_COMMUNICATION 0x07
#define PCI_CLASS_COMMUNICATION_SERIAL 0x0700
#define PCI_CLASS_COMMUNICATION_PARALLEL 0x0701
#define PCI_CLASS_COMMUNICATION_MULTISERIAL 0x0702
#define PCI_CLASS_COMMUNICATION_MODEM 0x0703
#define PCI_CLASS_COMMUNICATION_OTHER 0x0780

#define PCI_BASE_CLASS_SYSTEM 0x08
#define PCI_CLASS_SYSTEM_PIC 0x0800
#define PCI_CLASS_SYSTEM_PIC_IOAPIC 0x080010
#define PCI_CLASS_SYSTEM_PIC_IOXAPIC 0x080020
#define PCI_CLASS_SYSTEM_DMA 0x0801
#define PCI_CLASS_SYSTEM_TIMER 0x0802
#define PCI_CLASS_SYSTEM_RTC 0x0803
#define PCI_CLASS_SYSTEM_PCI_HOTPLUG 0x0804
#define PCI_CLASS_SYSTEM_SDHCI 0x0805
#define PCI_CLASS_SYSTEM_RCEC 0x0807
#define PCI_CLASS_SYSTEM_OTHER 0x0880

#define PCI_BASE_CLASS_INPUT 0x09
#define PCI_BASE_CLASS_DOCKING 0x0a
#define PCI_BASE_CLASS_PROCESSOR 0x0b

#define PCI_BASE_CLASS_SERIAL 0x0c
#define PCI_CLASS_SERIAL_FIREWIRE 0x0c00
#define PCI_CLASS_SERIAL_FIREWIRE_OHCI 0x0c0010
#define PCI_CLASS_SERIAL_ACCESS 0x0c01
#define PCI_CLASS_SERIAL_SSA 0x0c02
#define PCI_CLASS_SERIAL_USB 0x0c03
#define PCI_CLASS_SERIAL_USB_UHCI 0x0c0300
#define PCI_CLASS_SERIAL_USB_OHCI 0x0c0310
#define PCI_CLASS_SERIAL_USB_EHCI 0x0c0320
#define PCI_CLASS_SERIAL_USB_XHCI 0x0c0330
#define PCI_CLASS_SERIAL_USB_DEVICE 0x0c03fe
#define PCI_CLASS_SERIAL_FIBER 0x0c04
#define PCI_CLASS_SERIAL_SMBUS 0x0c05
#define PCI_CLASS_SERIAL_IPMI 0x0c07

#define PCI_BASE_CLASS_WIRELESS 0x0d
#define PCI_BASE_CLASS_INTELLIGENT 0x0e
#define PCI_BASE_CLASS_SATELLITE 0x0f
#define PCI_BASE_CLASS_CRYPT 0x10
#define PCI_BASE_CLASS_SIGNAL_PROCESSING 0x11
#define PCI_BASE_CLASS_ACCELERATOR 0x12
#define PCI_CLASS_OTHERS 0xff /* device does not fit any class */

/* ==========================================================================
 * Vendor IDs used by driver quirk tables
 *
 * Names that have a conventional spelling use it; the others
 * (KIOXIA, MICRON, SANDISK, WDC, HGST, SK_HYNIX, PHISON, SMI, SEAGATE,
 * LITEON, LEXAR, KINGSTON, ADATA, NETAC, VIRTUALBOX, MAXIO, INNOGRIT,
 * MEMBLAZE, UMIS, YMTC, CRUCIAL) are LikeOS names.
 * ========================================================================== */

#define PCI_VENDOR_ID_SOLIDIGM 0x025e /* Solidigm */
#define PCI_VENDOR_ID_REALTEK 0x10ec /* Realtek Semiconductor */
#define PCI_VENDOR_ID_AMD 0x1022 /* Advanced Micro Devices */
#define PCI_VENDOR_ID_APPLE 0x106b /* Apple */
#define PCI_VENDOR_ID_TOSHIBA 0x1179 /* Toshiba */
#define PCI_VENDOR_ID_O2 0x1217 /* O2 Micro */
#define PCI_VENDOR_ID_SMI 0x126f /* Silicon Motion */
#define PCI_VENDOR_ID_MICRON 0x1344 /* Micron Technology */
#define PCI_VENDOR_ID_SAMSUNG 0x144d /* Samsung Electronics */
#define PCI_VENDOR_ID_LITEON 0x14a4 /* Lite-On Technology */
#define PCI_VENDOR_ID_VMWARE 0x15ad /* VMware */
#define PCI_VENDOR_ID_MELLANOX 0x15b3 /* Mellanox Technologies */
#define PCI_VENDOR_ID_SANDISK 0x15b7 /* SanDisk */
#define PCI_VENDOR_ID_PHISON 0x1987 /* Phison Electronics */
#define PCI_VENDOR_ID_HUAWEI 0x19e5 /* Huawei Technologies */
#define PCI_VENDOR_ID_REDHAT 0x1b36 /* Red Hat (QEMU devices) */
#define PCI_VENDOR_ID_MARVELL_EXT 0x1b4b /* Marvell Technology Group */
#define PCI_VENDOR_ID_WDC 0x1b96 /* Western Digital */
#define PCI_VENDOR_ID_SEAGATE 0x1bb1 /* Seagate Technology */
#define PCI_VENDOR_ID_HGST 0x1c58 /* HGST */
#define PCI_VENDOR_ID_SK_HYNIX 0x1c5c /* SK hynix */
#define PCI_VENDOR_ID_MEMBLAZE 0x1c5f /* Beijing Memblaze */
#define PCI_VENDOR_ID_ADATA 0x1cc1 /* ADATA Technology */
#define PCI_VENDOR_ID_UMIS 0x1cc4 /* Shenzhen Unionmemory (UMIS) */
#define PCI_VENDOR_ID_AMAZON 0x1d0f /* Amazon.com */
#define PCI_VENDOR_ID_LEXAR 0x1d97 /* Shenzhen Longsys (Lexar) */
#define PCI_VENDOR_ID_INNOGRIT 0x1dbe /* INNOGRIT */
#define PCI_VENDOR_ID_KIOXIA 0x1e0f /* KIOXIA */
#define PCI_VENDOR_ID_YMTC 0x1e49 /* Yangtze Memory Technologies */
#define PCI_VENDOR_ID_MAXIO 0x1e4b /* MAXIO Technology */
#define PCI_VENDOR_ID_NETAC 0x1f40 /* Netac Technology */
#define PCI_VENDOR_ID_KINGSTON 0x2646 /* Kingston Technology */
#define PCI_VENDOR_ID_VIRTUALBOX 0x80ee /* InnoTek (VirtualBox devices) */
#define PCI_VENDOR_ID_INTEL 0x8086 /* Intel */
#define PCI_VENDOR_ID_CRUCIAL 0xc0a9 /* Micron/Crucial Technology */

/* ==========================================================================
 * Decode helpers
 * ========================================================================== */

/* Fields of a 32-bit extended capability header. */
static inline uint32_t pci_ext_cap_id(uint32_t hdr)
{
	return hdr & 0x0000ffff;
}

static inline uint32_t pci_ext_cap_ver(uint32_t hdr)
{
	return (hdr >> 16) & 0xf;
}

/* Offset of the next extended capability; 0 ends the list, and anything
 * below PCI_CFG_SPACE_SIZE is a broken list as well. */
static inline uint32_t pci_ext_cap_next(uint32_t hdr)
{
	return (hdr >> 20) & 0xffc;
}

/* A link speed code (PCI_EXP_LNKSTA_CLS_*, PCI_EXP_LNKCAP_SLS_*,
 * PCI_EXP_LNKCTL2_TLS_*; all share one encoding) as text. */
static inline const char *pcie_lnksta_speed_string(unsigned int lnksta_cls)
{
	switch (lnksta_cls & PCI_EXP_LNKSTA_CLS) {
	case PCI_EXP_LNKSTA_CLS_2_5GB:
		return "2.5 GT/s";
	case PCI_EXP_LNKSTA_CLS_5_0GB:
		return "5.0 GT/s";
	case PCI_EXP_LNKSTA_CLS_8_0GB:
		return "8.0 GT/s";
	case PCI_EXP_LNKSTA_CLS_16_0GB:
		return "16.0 GT/s";
	case PCI_EXP_LNKSTA_CLS_32_0GB:
		return "32.0 GT/s";
	case PCI_EXP_LNKSTA_CLS_64_0GB:
		return "64.0 GT/s";
	default:
		return "Unknown";
	}
}

/* Negotiated link width (lanes) from a PCI_EXP_LNKSTA value. */
static inline unsigned int pcie_lnksta_width(uint16_t lnksta)
{
	return (unsigned int)(lnksta & PCI_EXP_LNKSTA_NLW) >>
	       PCI_EXP_LNKSTA_NLW_SHIFT;
}

#endif /* KERNEL_HAL_PCI_REGS_H */
