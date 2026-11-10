// LikeOS AHCI driver - PCI detection, BAR5 mapping, interrupt setup, PCI state
//
// The PCI side of an AHCI host bus adapter.  An AHCI HBA is an ordinary PCI
// function: class 01h (mass storage), subclass 06h (Serial ATA), programming
// interface 01h (AHCI 1.0), with its register file -- the "ABAR" -- in the
// memory BAR at index 5.  Everything the rest of the driver needs from the
// PCI layer lives here:
//
//   * DETECTION: the class code identifies every standard AHCI function, and
//     a table of known controllers adds the ones that present themselves
//     differently (Intel functions strapped to "RAID" mode keep the AHCI
//     register interface under subclass 04h) and attaches the per-chipset
//     quirk flags (broken MSI, 32-bit-only DMA, phantom ports, interface
//     errors that must be ignored ...).  A table entry can also refuse a
//     match outright (Intel VMD presents a RAID class code but is not an
//     AHCI controller at all).
//
//   * CONFIGURATION: the function is brought to the D0 power state, memory
//     decoding and bus mastering are enabled, legacy INTx is masked until
//     an interrupt mode is chosen, and the Intel port-enable bits in the
//     PCS configuration register are set for every implemented port (older
//     Intel chipsets leave a port's PHY powered down until its bit is set,
//     and the port then never sees a device).
//
//   * MAPPING: ABAR is decoded with pci_bar_decode() (64-bit BARs and the
//     real window size included) and mapped UNCACHED -- every AHCI register
//     access has side effects, and a write-back or write-combining mapping
//     would merge, reorder or elide them.
//
//   * INTERRUPTS: after the scheduler runs, ahci_pci_setup_irq() picks the
//     best mode the function and its quirks allow -- MSI-X entry 0, then
//     single-message MSI, then legacy INTx routed through the ACPI _PRT of
//     the root bridge (the pin swizzled through every PCI-PCI bridge on the
//     way up, as the PCI-to-PCI bridge specification defines), and when
//     even that fails, the 1 ms polling timer.  INTx is level-triggered and
//     active-low; the line may be shared, so the handler answers "not mine"
//     when the HBA has nothing pending.
//
//   * STATE: the configuration header and the MSI/MSI-X programming are
//     saved after the probe and restored around controller recovery and
//     suspend/resume, so a function that lost its configuration (a D3hot
//     transition without No_Soft_Reset, a PCI error) comes back exactly as
//     it was.
//
// Context: ahci_pci_match/enable run at boot (interrupts off, no scheduler);
// setup_irq runs from ahci_late_init() after sti; save/restore are "any
// context" (configuration space accesses do not sleep).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/irq.h>
#include <kernel/hal/acpi.h>
#include <kernel/fs/sysfs.h>

/* ==========================================================================
 * PCI configuration space layout used here
 * ========================================================================== */

#define AHCI_PCI_CFG_VENDOR 0x00
#define AHCI_PCI_CFG_COMMAND 0x04
#define AHCI_PCI_CFG_STATUS 0x06
#define AHCI_PCI_CFG_REVISION 0x08
#define AHCI_PCI_CFG_CACHE_LINE 0x0C
#define AHCI_PCI_CFG_LATENCY 0x0D
#define AHCI_PCI_CFG_SUBSYS_VENDOR 0x2C
#define AHCI_PCI_CFG_SUBSYS_ID 0x2E
#define AHCI_PCI_CFG_ROM 0x30
#define AHCI_PCI_CFG_INT_LINE 0x3C

#define AHCI_PCI_CMD_IO (1u << 0)
#define AHCI_PCI_CMD_MEM (1u << 1)
#define AHCI_PCI_CMD_MASTER (1u << 2)
#define AHCI_PCI_CMD_PARITY (1u << 6)
#define AHCI_PCI_CMD_SERR (1u << 8)

/* Status register error bits (RW1C) */
#define AHCI_PCI_STS_MDPE (1u << 8) /* master data parity error */
#define AHCI_PCI_STS_STA (1u << 11) /* signalled target abort */
#define AHCI_PCI_STS_RTA (1u << 12) /* received target abort */
#define AHCI_PCI_STS_RMA (1u << 13) /* received master abort */
#define AHCI_PCI_STS_SSE (1u << 14) /* signalled system error */
#define AHCI_PCI_STS_DPE (1u << 15) /* detected parity error */
#define AHCI_PCI_STS_ERRORS                                              \
	(AHCI_PCI_STS_MDPE | AHCI_PCI_STS_STA | AHCI_PCI_STS_RTA |       \
	 AHCI_PCI_STS_RMA | AHCI_PCI_STS_SSE | AHCI_PCI_STS_DPE)

/* Power management capability (PCI Bus Power Management Interface) */
#define AHCI_PCI_CAP_PM 0x01
#define AHCI_PCI_PM_CTRL 4 /* PMCSR, 16 bits */
#define AHCI_PCI_PM_STATE_MASK 0x0003u
#define AHCI_PCI_PM_NO_SOFT_RESET 0x0008u
/* D3hot -> D0 needs 10 ms before the function may be touched again */
#define AHCI_PCI_PM_D3_DELAY_MS 10

/* MSI capability */
#define AHCI_MSI_CTRL 2 /* 16 bits */
#define AHCI_MSI_ADDR_LO 4
#define AHCI_MSI_ADDR_HI 8 /* 64-bit capable functions only */
#define AHCI_MSI_DATA_32 8
#define AHCI_MSI_DATA_64 12
#define AHCI_MSI_MASK_32 12 /* per-vector masking */
#define AHCI_MSI_MASK_64 16
#define AHCI_MSI_CTRL_ENABLE (1u << 0)
#define AHCI_MSI_CTRL_MME_MASK (7u << 4)
#define AHCI_MSI_CTRL_64BIT (1u << 7)
#define AHCI_MSI_CTRL_PVM (1u << 8)

/* MSI-X capability */
#define AHCI_MSIX_CTRL 2 /* 16 bits */
#define AHCI_MSIX_TABLE 4 /* BIR + offset */
#define AHCI_MSIX_CTRL_ENABLE (1u << 15)
#define AHCI_MSIX_CTRL_FMASK (1u << 14)
#define AHCI_MSIX_CTRL_SIZE_MASK 0x07FFu

/* Per-HBA MSI-X table entry 0, mapped when MSI-X is in use, so that its
 * programming can be saved and restored like the rest of the state. */
static volatile uint32_t *g_ahci_msix_entry[AHCI_MAX_HBAS];

/* ==========================================================================
 * The known-controller table
 *
 * The class code finds every standard AHCI function on its own; an entry
 * here is needed only to name a controller, to attach quirks, to accept a
 * function whose class code is not 01/06/01, or to refuse one.  Matching is
 * first hit wins, so specific entries go before wildcards.
 * ========================================================================== */

#define AHCI_CLASS_AHCI 0x010601u
#define AHCI_CLASS_RAID 0x010400u

/* Intel desktop/mobile chipsets that need the PCS port-enable fix. */
#define AHCI_Q_INTEL_OLD (AHCI_Q_INTEL_PCS)
/* AMD SB600: the first AMD AHCI southbridge, with the most problems. */
#define AHCI_Q_SB600                                                     \
	(AHCI_Q_IGN_SERR_INTERNAL | AHCI_Q_NO_MSI | AHCI_Q_32BIT_ONLY |  \
	 AHCI_Q_AMD_32BIT | AHCI_Q_NO_PMP)
/* AMD SB7x0/SB8x0/SB9x0 */
#define AHCI_Q_SB700 (AHCI_Q_IGN_SERR_INTERNAL | AHCI_Q_SRST_NO_CLO)
/* Marvell 88SE61xx: AHCI with a PATA port behind it */
#define AHCI_Q_MV61XX                                                    \
	(AHCI_Q_NO_NCQ | AHCI_Q_NO_MSI | AHCI_Q_MV_PATA | AHCI_Q_NO_PMP | \
	 AHCI_Q_SRST_RETRY)
/* Marvell 88SE91xx/92xx: FBS works though CAP.FBSS reads 0 */
#define AHCI_Q_MV91XX (AHCI_Q_YES_FBS)
/* JMicron JMB36x */
#define AHCI_Q_JMB (AHCI_Q_IGN_SERR_INTERNAL | AHCI_Q_IGN_IRQ_IF_ERR)
/* VIA VT8251 */
#define AHCI_Q_VT8251 (AHCI_Q_NO_NCQ | AHCI_Q_NO_PMP | AHCI_Q_NO_MSI)
/* ASMedia ASM116x: PI advertises ports that do not exist */
#define AHCI_Q_ASM116X (AHCI_Q_ASMEDIA_PORTS)

const struct ahci_pci_id ahci_pci_ids[] = {
	/* ---- Not AHCI although the class code says "RAID" ----------------
	 * Intel Volume Management Device: an NVMe remapping bridge. */
	{ 0x8086, 0x201d, 0, AHCI_Q_NOT_AHCI, "Intel VMD (not AHCI)" },
	{ 0x8086, 0x9a0b, 0, AHCI_Q_NOT_AHCI, "Intel VMD (not AHCI)" },
	{ 0x8086, 0x467f, 0, AHCI_Q_NOT_AHCI, "Intel VMD (not AHCI)" },
	{ 0x8086, 0xa77f, 0, AHCI_Q_NOT_AHCI, "Intel VMD (not AHCI)" },

	/* ---- Intel ICH ---------------------------------------------------- */
	{ 0x8086, 0x2652, 0, AHCI_Q_INTEL_OLD, "Intel ICH6R AHCI" },
	{ 0x8086, 0x2653, 0, AHCI_Q_INTEL_OLD, "Intel ICH6-M AHCI" },
	{ 0x8086, 0x27c1, 0, AHCI_Q_INTEL_OLD, "Intel ICH7 AHCI" },
	{ 0x8086, 0x27c5, 0, AHCI_Q_INTEL_OLD, "Intel ICH7-M AHCI" },
	{ 0x8086, 0x27c3, 0, AHCI_Q_INTEL_OLD, "Intel ICH7R RAID" },
	{ 0x8086, 0x2681, 0, AHCI_Q_INTEL_OLD, "Intel ESB2 AHCI" },
	{ 0x8086, 0x2821, 0, AHCI_Q_INTEL_OLD, "Intel ICH8 AHCI" },
	{ 0x8086, 0x2822, 0, AHCI_Q_INTEL_OLD, "Intel ICH8R+ RAID" },
	{ 0x8086, 0x2824, 0, AHCI_Q_INTEL_OLD, "Intel ICH8 AHCI" },
	{ 0x8086, 0x2826, 0, AHCI_Q_INTEL_OLD, "Intel C600 RAID" },
	/* 8086:2829 is also the controller VirtualBox emulates; the
	 * hypervisor check below adds AHCI_Q_VM there. */
	{ 0x8086, 0x2829, 0, AHCI_Q_INTEL_OLD, "Intel ICH8-M AHCI" },
	{ 0x8086, 0x282a, 0, AHCI_Q_INTEL_OLD, "Intel ICH8-M RAID" },
	/* 8086:2922 is the controller QEMU's q35 machine emulates. */
	{ 0x8086, 0x2922, 0, AHCI_Q_INTEL_OLD, "Intel ICH9 AHCI (6 port)" },
	{ 0x8086, 0x2923, 0, AHCI_Q_INTEL_OLD, "Intel ICH9 AHCI (4 port)" },
	{ 0x8086, 0x2924, 0, AHCI_Q_INTEL_OLD, "Intel ICH9 AHCI" },
	{ 0x8086, 0x2925, 0, AHCI_Q_INTEL_OLD, "Intel ICH9 AHCI" },
	{ 0x8086, 0x2927, 0, AHCI_Q_INTEL_OLD, "Intel ICH9 AHCI" },
	{ 0x8086, 0x2929, 0, AHCI_Q_INTEL_OLD, "Intel ICH9-M AHCI" },
	{ 0x8086, 0x292a, 0, AHCI_Q_INTEL_OLD, "Intel ICH9-M AHCI" },
	{ 0x8086, 0x292b, 0, AHCI_Q_INTEL_OLD, "Intel ICH9-M AHCI" },
	{ 0x8086, 0x292c, 0, AHCI_Q_INTEL_OLD, "Intel ICH9-M AHCI" },
	{ 0x8086, 0x292f, 0, AHCI_Q_INTEL_OLD, "Intel ICH9-M AHCI" },
	{ 0x8086, 0x294d, 0, AHCI_Q_INTEL_OLD, "Intel ICH9 AHCI" },
	{ 0x8086, 0x294e, 0, AHCI_Q_INTEL_OLD, "Intel ICH9-M AHCI" },
	{ 0x8086, 0x3a02, 0, AHCI_Q_INTEL_OLD, "Intel ICH10 AHCI" },
	{ 0x8086, 0x3a05, 0, AHCI_Q_INTEL_OLD, "Intel ICH10 RAID" },
	{ 0x8086, 0x3a22, 0, AHCI_Q_INTEL_OLD, "Intel ICH10 AHCI" },
	{ 0x8086, 0x3a25, 0, AHCI_Q_INTEL_OLD, "Intel ICH10 RAID" },

	/* ---- Intel PCH ---------------------------------------------------- */
	{ 0x8086, 0x3b22, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series AHCI" },
	{ 0x8086, 0x3b23, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series AHCI" },
	{ 0x8086, 0x3b24, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series RAID" },
	{ 0x8086, 0x3b25, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series RAID" },
	{ 0x8086, 0x3b29, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series-M AHCI" },
	{ 0x8086, 0x3b2b, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series RAID" },
	{ 0x8086, 0x3b2c, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series-M RAID" },
	{ 0x8086, 0x3b2f, 0, AHCI_Q_INTEL_OLD, "Intel 5 Series AHCI" },
	{ 0x8086, 0x1c02, 0, AHCI_Q_INTEL_OLD, "Intel 6 Series AHCI" },
	{ 0x8086, 0x1c03, 0, AHCI_Q_INTEL_OLD, "Intel 6 Series-M AHCI" },
	{ 0x8086, 0x1c04, 0, AHCI_Q_INTEL_OLD, "Intel 6 Series RAID" },
	{ 0x8086, 0x1c05, 0, AHCI_Q_INTEL_OLD, "Intel 6 Series-M RAID" },
	{ 0x8086, 0x1c06, 0, AHCI_Q_INTEL_OLD, "Intel 6 Series RAID" },
	{ 0x8086, 0x1c07, 0, AHCI_Q_INTEL_OLD, "Intel 6 Series RAID" },
	{ 0x8086, 0x1d02, 0, AHCI_Q_INTEL_OLD, "Intel C600 AHCI" },
	{ 0x8086, 0x1d04, 0, AHCI_Q_INTEL_OLD, "Intel C600 RAID" },
	{ 0x8086, 0x1d06, 0, AHCI_Q_INTEL_OLD, "Intel C600 RAID" },
	{ 0x8086, 0x1e02, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series AHCI" },
	{ 0x8086, 0x1e03, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series-M AHCI" },
	{ 0x8086, 0x1e04, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series RAID" },
	{ 0x8086, 0x1e05, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series RAID" },
	{ 0x8086, 0x1e06, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series RAID" },
	{ 0x8086, 0x1e07, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series-M RAID" },
	{ 0x8086, 0x1e0e, 0, AHCI_Q_INTEL_OLD, "Intel 7 Series RAID" },
	{ 0x8086, 0x8c02, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series AHCI" },
	{ 0x8086, 0x8c03, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-M AHCI" },
	{ 0x8086, 0x8c04, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series RAID" },
	{ 0x8086, 0x8c05, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-M RAID" },
	{ 0x8086, 0x8c06, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series RAID" },
	{ 0x8086, 0x8c07, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-M RAID" },
	{ 0x8086, 0x8c0e, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series RAID" },
	{ 0x8086, 0x8c0f, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-M RAID" },
	{ 0x8086, 0x9c02, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP AHCI" },
	{ 0x8086, 0x9c03, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP AHCI" },
	{ 0x8086, 0x9c04, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP RAID" },
	{ 0x8086, 0x9c05, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP RAID" },
	{ 0x8086, 0x9c06, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP RAID" },
	{ 0x8086, 0x9c07, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP RAID" },
	{ 0x8086, 0x9c0e, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP RAID" },
	{ 0x8086, 0x9c0f, 0, AHCI_Q_INTEL_OLD, "Intel 8 Series-LP RAID" },
	{ 0x8086, 0x9c83, 0, AHCI_Q_INTEL_OLD, "Intel Wildcat Point-LP AHCI" },
	{ 0x8086, 0x9c85, 0, AHCI_Q_INTEL_OLD, "Intel Wildcat Point-LP RAID" },
	{ 0x8086, 0x9c87, 0, AHCI_Q_INTEL_OLD, "Intel Wildcat Point-LP RAID" },
	{ 0x8086, 0x9c8f, 0, AHCI_Q_INTEL_OLD, "Intel Wildcat Point-LP RAID" },
	{ 0x8086, 0x8c82, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series AHCI" },
	{ 0x8086, 0x8c83, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series-M AHCI" },
	{ 0x8086, 0x8c84, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series RAID" },
	{ 0x8086, 0x8c85, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series-M RAID" },
	{ 0x8086, 0x8c86, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series RAID" },
	{ 0x8086, 0x8c87, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series-M RAID" },
	{ 0x8086, 0x8c8e, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series RAID" },
	{ 0x8086, 0x8c8f, 0, AHCI_Q_INTEL_OLD, "Intel 9 Series-M RAID" },
	{ 0x8086, 0x8d02, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg AHCI" },
	{ 0x8086, 0x8d04, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg RAID" },
	{ 0x8086, 0x8d06, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg RAID" },
	{ 0x8086, 0x8d0e, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg RAID" },
	{ 0x8086, 0x8d62, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg sSATA AHCI" },
	{ 0x8086, 0x8d64, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg sSATA RAID" },
	{ 0x8086, 0x8d66, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg sSATA RAID" },
	{ 0x8086, 0x8d6e, 0, AHCI_Q_INTEL_OLD, "Intel Wellsburg sSATA RAID" },
	{ 0x8086, 0x0f22, 0, AHCI_Q_INTEL_OLD, "Intel Bay Trail AHCI" },
	{ 0x8086, 0x0f23, 0, AHCI_Q_INTEL_OLD, "Intel Bay Trail AHCI" },
	{ 0x8086, 0x22a3, 0, AHCI_Q_INTEL_OLD, "Intel Cherry Trail AHCI" },
	{ 0x8086, 0x5ae3, 0, AHCI_Q_INTEL_OLD, "Intel Apollo Lake AHCI" },
	{ 0x8086, 0x31e3, 0, AHCI_Q_INTEL_OLD, "Intel Gemini Lake AHCI" },
	{ 0x8086, 0x9d03, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-LP AHCI" },
	{ 0x8086, 0x9d05, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-LP RAID" },
	{ 0x8086, 0x9d07, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-LP RAID" },
	/* The ThinkPad P50's controller (CM236 chipset). */
	{ 0x8086, 0xa102, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-H AHCI" },
	{ 0x8086, 0xa103, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-H AHCI" },
	{ 0x8086, 0xa105, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-H RAID" },
	{ 0x8086, 0xa106, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-H RAID" },
	{ 0x8086, 0xa107, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-H RAID" },
	{ 0x8086, 0xa10f, 0, AHCI_Q_INTEL_OLD, "Intel Sunrise Point-H RAID" },
	{ 0x8086, 0xa182, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg AHCI" },
	{ 0x8086, 0xa186, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg RAID" },
	{ 0x8086, 0xa1d2, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg RAID" },
	{ 0x8086, 0xa1d6, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg RAID" },
	{ 0x8086, 0xa202, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg sSATA AHCI" },
	{ 0x8086, 0xa206, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg sSATA RAID" },
	{ 0x8086, 0xa252, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg RAID" },
	{ 0x8086, 0xa256, 0, AHCI_Q_INTEL_OLD, "Intel Lewisburg RAID" },
	{ 0x8086, 0xa282, 0, AHCI_Q_INTEL_OLD, "Intel 200 Series AHCI" },
	{ 0x8086, 0xa286, 0, AHCI_Q_INTEL_OLD, "Intel 200 Series RAID" },
	{ 0x8086, 0xa352, 0, AHCI_Q_INTEL_OLD, "Intel Cannon Lake PCH-H AHCI" },
	{ 0x8086, 0xa353, 0, AHCI_Q_INTEL_OLD, "Intel Cannon Lake PCH-H AHCI" },
	{ 0x8086, 0x9dd3, 0, AHCI_Q_INTEL_OLD, "Intel Cannon Point-LP AHCI" },
	{ 0x8086, 0x02d3, 0, AHCI_Q_INTEL_OLD, "Intel Comet Lake PCH-LP AHCI" },
	{ 0x8086, 0x02d7, 0, AHCI_Q_INTEL_OLD, "Intel Comet Lake PCH-LP RAID" },
	{ 0x8086, 0x34d3, 0, 0, "Intel Ice Lake-LP AHCI" },
	{ 0x8086, 0xa0d3, 0, 0, "Intel Tiger Lake-LP AHCI" },
	{ 0x8086, 0x7ae2, 0, 0, "Intel Alder Lake-S AHCI" },
	/* Any other Intel function with the AHCI class code. */
	{ 0x8086, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "Intel AHCI" },

	/* ---- AMD / ATI ---------------------------------------------------- */
	{ 0x1002, 0x4380, 0, AHCI_Q_SB600, "AMD SB600 AHCI" },
	{ 0x1002, 0x4390, 0, AHCI_Q_SB700, "AMD SB7x0/SB8x0/SB9x0 AHCI" },
	{ 0x1002, 0x4391, 0, AHCI_Q_SB700, "AMD SB7x0/SB8x0/SB9x0 AHCI" },
	{ 0x1002, 0x4392, 0, AHCI_Q_SB700, "AMD SB7x0/SB8x0/SB9x0 RAID" },
	{ 0x1002, 0x4393, 0, AHCI_Q_SB700, "AMD SB7x0/SB8x0/SB9x0 RAID" },
	{ 0x1002, 0x4394, 0, AHCI_Q_SB700, "AMD SB7x0/SB8x0/SB9x0 AHCI" },
	{ 0x1002, 0x4395, 0, AHCI_Q_SB700, "AMD SB8x0/SB9x0 AHCI" },
	{ 0x1022, 0x7800, 0, AHCI_Q_SB700, "AMD Hudson-2 AHCI" },
	{ 0x1022, 0x7801, 0, AHCI_Q_SB700, "AMD Hudson-2/FCH AHCI" },
	{ 0x1022, 0x7802, 0, AHCI_Q_SB700, "AMD Hudson-2 RAID" },
	{ 0x1022, 0x7803, 0, AHCI_Q_SB700, "AMD Hudson-2 RAID" },
	{ 0x1022, 0x7804, 0, AHCI_Q_SB700, "AMD Hudson-2 AHCI" },
	{ 0x1022, 0x7900, 0, 0, "AMD FCH AHCI" },
	{ 0x1022, 0x7901, 0, 0, "AMD FCH AHCI" },
	{ 0x1022, 0x7902, 0, 0, "AMD FCH RAID" },
	{ 0x1022, 0x7903, 0, 0, "AMD FCH RAID" },
	{ 0x1022, 0x7904, 0, 0, "AMD FCH AHCI" },
	{ 0x1022, 0x43b5, 0, AHCI_Q_ASM116X, "AMD 300 Series chipset AHCI" },
	{ 0x1022, 0x43b6, 0, AHCI_Q_ASM116X, "AMD 300 Series chipset AHCI" },
	{ 0x1022, 0x43b7, 0, AHCI_Q_ASM116X, "AMD 300 Series chipset AHCI" },
	{ 0x1022, 0x43c8, 0, AHCI_Q_ASM116X, "AMD 400 Series chipset AHCI" },
	{ 0x1022, 0x43eb, 0, AHCI_Q_ASM116X, "AMD 500 Series chipset AHCI" },
	{ 0x1022, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "AMD AHCI" },
	{ 0x1002, AHCI_PCI_ANY, AHCI_CLASS_AHCI, AHCI_Q_IGN_SERR_INTERNAL,
	  "ATI AHCI" },

	/* ---- ASMedia ------------------------------------------------------ */
	{ 0x1b21, 0x0601, 0, 0, "ASMedia ASM106x AHCI" },
	{ 0x1b21, 0x0602, 0, 0, "ASMedia ASM106x AHCI" },
	{ 0x1b21, 0x0611, 0, 0, "ASMedia ASM1061 AHCI" },
	{ 0x1b21, 0x0612, 0, 0, "ASMedia ASM1062 AHCI" },
	{ 0x1b21, 0x0621, 0, 0, "ASMedia ASM1061R RAID" },
	{ 0x1b21, 0x0622, 0, 0, "ASMedia ASM106x RAID" },
	{ 0x1b21, 0x1062, 0, 0, "ASMedia ASM1062 AHCI" },
	{ 0x1b21, 0x1064, 0, AHCI_Q_ASM116X, "ASMedia ASM1064 AHCI" },
	{ 0x1b21, 0x1164, 0, AHCI_Q_ASM116X, "ASMedia ASM1164 AHCI" },
	{ 0x1b21, 0x1165, 0, AHCI_Q_ASM116X, "ASMedia ASM1165 AHCI" },
	{ 0x1b21, 0x1166, 0, AHCI_Q_ASM116X, "ASMedia ASM1166 AHCI" },
	{ 0x1b21, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "ASMedia AHCI" },

	/* ---- Marvell ------------------------------------------------------ */
	{ 0x11ab, 0x6121, 0, AHCI_Q_MV61XX, "Marvell 88SE6121 AHCI" },
	{ 0x11ab, 0x6145, 0, AHCI_Q_MV61XX, "Marvell 88SE6145 AHCI" },
	{ 0x1b4b, 0x9120, 0, AHCI_Q_MV91XX, "Marvell 88SE9120 AHCI" },
	{ 0x1b4b, 0x9123, 0, AHCI_Q_MV91XX, "Marvell 88SE9123 AHCI" },
	{ 0x1b4b, 0x9125, 0, AHCI_Q_MV91XX, "Marvell 88SE9125 AHCI" },
	{ 0x1b4b, 0x9128, 0, AHCI_Q_MV91XX, "Marvell 88SE9128 AHCI" },
	{ 0x1b4b, 0x9130, 0, AHCI_Q_MV91XX, "Marvell 88SE9130 AHCI" },
	{ 0x1b4b, 0x9172, 0, AHCI_Q_MV91XX, "Marvell 88SE9172 AHCI" },
	{ 0x1b4b, 0x917a, 0, AHCI_Q_MV91XX, "Marvell 88SE917A AHCI" },
	{ 0x1b4b, 0x9182, 0, AHCI_Q_MV91XX, "Marvell 88SE9182 AHCI" },
	{ 0x1b4b, 0x9192, 0, AHCI_Q_MV91XX, "Marvell 88SE9192 AHCI" },
	{ 0x1b4b, 0x91a0, 0, AHCI_Q_MV91XX, "Marvell 88SE91A0 AHCI" },
	{ 0x1b4b, 0x91a3, 0, AHCI_Q_MV91XX, "Marvell 88SE91A3 AHCI" },
	{ 0x1b4b, 0x9215, 0, AHCI_Q_MV91XX, "Marvell 88SE9215 AHCI" },
	{ 0x1b4b, 0x9230, 0, AHCI_Q_MV91XX, "Marvell 88SE9230 AHCI" },
	{ 0x1b4b, 0x9235, 0, AHCI_Q_MV91XX, "Marvell 88SE9235 AHCI" },
	{ 0x1b4b, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "Marvell AHCI" },

	/* ---- JMicron ------------------------------------------------------ */
	{ 0x197b, 0x2360, 0, AHCI_Q_JMB, "JMicron JMB360 AHCI" },
	{ 0x197b, 0x2361, 0, AHCI_Q_JMB, "JMicron JMB361 AHCI" },
	{ 0x197b, 0x2362, 0, AHCI_Q_JMB, "JMicron JMB362 AHCI" },
	{ 0x197b, 0x2363, 0, AHCI_Q_JMB, "JMicron JMB363 AHCI" },
	{ 0x197b, 0x2364, 0, AHCI_Q_JMB, "JMicron JMB364 AHCI" },
	{ 0x197b, 0x2365, 0, AHCI_Q_JMB, "JMicron JMB365 AHCI" },
	{ 0x197b, 0x2366, 0, AHCI_Q_JMB, "JMicron JMB366 AHCI" },
	{ 0x197b, 0x2368, 0, AHCI_Q_JMB, "JMicron JMB368 AHCI" },
	{ 0x197b, 0x2392, 0, AHCI_Q_JMB, "JMicron JMB392 AHCI" },
	{ 0x197b, 0x0585, 0, 0, "JMicron JMB585 AHCI" },
	{ 0x197b, AHCI_PCI_ANY, AHCI_CLASS_AHCI, AHCI_Q_JMB, "JMicron AHCI" },

	/* ---- VIA, NVIDIA, SiS --------------------------------------------- */
	{ 0x1106, 0x3349, 0, AHCI_Q_VT8251, "VIA VT8251 AHCI" },
	{ 0x1106, 0x6287, 0, AHCI_Q_VT8251, "VIA VT8251 AHCI" },
	{ 0x1106, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "VIA AHCI" },
	{ 0x10de, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "NVIDIA MCP AHCI" },
	{ 0x1039, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "SiS AHCI" },

	/* ---- Virtual machines --------------------------------------------- */
	{ 0x15ad, 0x07e0, 0, AHCI_Q_VM, "VMware SATA AHCI" },
	{ 0x1af4, AHCI_PCI_ANY, AHCI_CLASS_AHCI, AHCI_Q_VM, "QEMU AHCI" },
	{ 0x80ee, AHCI_PCI_ANY, AHCI_CLASS_AHCI, AHCI_Q_VM,
	  "VirtualBox AHCI" },
	{ 0x1414, AHCI_PCI_ANY, AHCI_CLASS_AHCI, AHCI_Q_VM, "Hyper-V AHCI" },

	{ 0, 0, 0, 0, NULL }
};

/* What an unknown function with the standard class code gets. */
static const struct ahci_pci_id ahci_pci_generic = {
	AHCI_PCI_ANY, AHCI_PCI_ANY, AHCI_CLASS_AHCI, 0, "AHCI"
};

static uint32_t ahci_pci_class_of(const pci_device_t *pci)
{
	return ((uint32_t)pci->class_code << 16) |
	       ((uint32_t)pci->subclass << 8) | (uint32_t)pci->prog_if;
}

const struct ahci_pci_id *ahci_pci_match(const pci_device_t *pci)
{
	uint32_t cls;
	int is_ahci_class;

	if (!pci || pci->vendor_id == 0xFFFF || pci->vendor_id == 0)
		return NULL;
	cls = ahci_pci_class_of(pci);
	is_ahci_class = cls == AHCI_CLASS_AHCI;

	for (const struct ahci_pci_id *e = ahci_pci_ids; e->vendor; e++) {
		if (e->vendor != AHCI_PCI_ANY && e->vendor != pci->vendor_id)
			continue;
		if (e->device != AHCI_PCI_ANY && e->device != pci->device_id)
			continue;
		if (e->class_code && e->class_code != cls)
			continue;
		if (e->quirks & AHCI_Q_NOT_AHCI)
			return NULL;
		/* A device-specific entry accepts any mass-storage function
		 * of that chip (the RAID-mode functions keep the AHCI
		 * register interface).  Anything else must at least be a
		 * mass storage controller. */
		if (pci->class_code != AHCI_PCI_CLASS_STORAGE)
			continue;
		if (!is_ahci_class && pci->subclass != AHCI_PCI_SUBCLASS_SATA &&
		    pci->subclass != AHCI_PCI_SUBCLASS_RAID)
			continue;
		return e;
	}
	if (is_ahci_class)
		return &ahci_pci_generic;
	return NULL;
}

/* ==========================================================================
 * Small configuration helpers
 * ========================================================================== */

static uint16_t ahci_pci_cmd_read(const struct ahci_hba *hba)
{
	return pci_cfg_read16(hba->pci, AHCI_PCI_CFG_COMMAND);
}

static void ahci_pci_cmd_write(const struct ahci_hba *hba, uint16_t v)
{
	pci_cfg_write16(hba->pci, AHCI_PCI_CFG_COMMAND, v);
}

/* Mask or unmask the function's legacy INTx assertion. */
static void ahci_pci_intx(const struct ahci_hba *hba, int enable)
{
	uint16_t cmd = ahci_pci_cmd_read(hba);
	uint16_t want;

	if (cmd == 0xFFFF)
		return;
	want = enable ? (uint16_t)(cmd & ~PCI_CMD_INTX_DISABLE) :
			(uint16_t)(cmd | PCI_CMD_INTX_DISABLE);
	if (want != cmd)
		ahci_pci_cmd_write(hba, want);
}

void ahci_pci_stop_dma(struct ahci_hba *hba)
{
	uint16_t cmd;

	BUG_ON(!hba);
	cmd = ahci_pci_cmd_read(hba);
	if (cmd != 0xFFFF && (cmd & AHCI_PCI_CMD_MASTER)) {
		ahci_pci_cmd_write(hba, (uint16_t)(cmd & ~AHCI_PCI_CMD_MASTER));
		cmd = ahci_pci_cmd_read(hba);
	}
	/* Gone (all ones) or bus mastering off: no more DMA. */
	if (cmd == 0xFFFF || !(cmd & AHCI_PCI_CMD_MASTER))
		hba->dma_stopped = 1;
	else
		kprintf("[ahci] %s: bus mastering does not switch off (cmd 0x%04x)\n",
			hba->name, cmd);
}

/* Is the function still answering configuration cycles?  A surprise
 * removal or a dead link reads all ones. */
int ahci_pci_alive(const struct ahci_hba *hba)
{
	return pci_cfg_read16(hba->pci, AHCI_PCI_CFG_VENDOR) != 0xFFFF;
}

/* Report (and clear) latched PCI error bits in the status register. */
static void ahci_pci_report_status(struct ahci_hba *hba, const char *when)
{
	uint16_t sts = pci_cfg_read16(hba->pci, AHCI_PCI_CFG_STATUS);

	if (sts == 0xFFFF || !(sts & AHCI_PCI_STS_ERRORS))
		return;
	kprintf("[ahci] %s: PCI status 0x%04x at %s:%s%s%s%s%s%s\n",
		hba->name, sts, when,
		(sts & AHCI_PCI_STS_DPE) ? " parity-error" : "",
		(sts & AHCI_PCI_STS_SSE) ? " system-error" : "",
		(sts & AHCI_PCI_STS_RMA) ? " master-abort" : "",
		(sts & AHCI_PCI_STS_RTA) ? " target-abort(rx)" : "",
		(sts & AHCI_PCI_STS_STA) ? " target-abort(tx)" : "",
		(sts & AHCI_PCI_STS_MDPE) ? " master-data-parity" : "");
	/* RW1C: writing the set bits back clears them */
	pci_cfg_write16(hba->pci, AHCI_PCI_CFG_STATUS,
			(uint16_t)(sts & AHCI_PCI_STS_ERRORS));
}

/* CPUID.1:ECX bit 31 -- set by every hypervisor for its guests. */
static int ahci_running_on_hypervisor(void)
{
	uint32_t a = 1, b = 0, c = 0, d = 0;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
	(void)b;
	(void)d;
	return (c >> 31) & 1;
}

/* Power state transitions through the PCI power management capability.
 * `state' 0 = D0 .. 3 = D3hot.  Returns the previous state, or -ENODEV
 * when the function has no power management capability. */
int ahci_pci_set_power(struct ahci_hba *hba, int state)
{
	uint8_t pm = pci_find_capability(hba->pci, AHCI_PCI_CAP_PM);
	uint16_t csr, cur;

	if (!pm)
		return -ENODEV;
	if (state < 0 || state > 3)
		return -EINVAL;
	csr = pci_cfg_read16(hba->pci, (uint8_t)(pm + AHCI_PCI_PM_CTRL));
	if (csr == 0xFFFF)
		return -ENODEV;
	cur = csr & AHCI_PCI_PM_STATE_MASK;
	if ((int)cur == state)
		return (int)cur;
	csr = (uint16_t)((csr & ~AHCI_PCI_PM_STATE_MASK) | (uint16_t)state);
	/* PME status (bit 15) is RW1C: never write it back by accident */
	csr &= (uint16_t)~0x8000u;
	pci_cfg_write16(hba->pci, (uint8_t)(pm + AHCI_PCI_PM_CTRL), csr);
	/* The bus power management specification: 10 ms after a transition
	 * to or from D3hot before the next access to the function. */
	if (cur == 3 || state == 3)
		ahci_mdelay(AHCI_PCI_PM_D3_DELAY_MS);
	else
		ahci_udelay(200);
	kprintf("[ahci] %s: PCI power state D%u -> D%d\n", hba->name,
		(unsigned)cur, state);
	return (int)cur;
}

/* Older Intel chipsets keep a port's PHY switched off until its enable bit
 * in the PCS configuration register is set; the firmware sets the bits
 * only for ports it used.  Set them for every port the HBA implements. */
static void ahci_pci_intel_pcs(struct ahci_hba *hba, uint32_t pi)
{
	uint16_t pcs, want;

	if (hba->pci->vendor_id != 0x8086 || !(hba->quirks & AHCI_Q_INTEL_PCS))
		return;
	pcs = pci_cfg_read16(hba->pci, AHCI_INTEL_PCI_PCS);
	if (pcs == 0xFFFF)
		return;
	want = (uint16_t)(pcs | (uint16_t)(pi & 0xFFu));
	if (want != pcs) {
		pci_cfg_write16(hba->pci, AHCI_INTEL_PCI_PCS, want);
		kprintf("[ahci] %s: Intel PCS port enables 0x%02x -> 0x%02x\n",
			hba->name, pcs & 0xFF, want & 0xFF);
	}
}

void ahci_pci_reset_quirks(struct ahci_hba *hba)
{
	uint32_t pi;

	BUG_ON(hba == NULL);
	if (!hba->mmio)
		return;
	/* The port enables follow the ports the HBA implements, which the
	 * reset has just restored (hba->saved_pi when firmware set it). */
	pi = ahci_hba_readl(hba, AHCI_PI);
	if (pi == 0 || pi == 0xFFFFFFFFu)
		pi = hba->saved_pi;
	ahci_pci_intel_pcs(hba, pi);
	ahci_pci_report_status(hba, "reset");
}

/* Runtime quirks that depend on more than the vendor/device pair. */
static void ahci_pci_runtime_quirks(struct ahci_hba *hba)
{
	uint16_t sv = pci_cfg_read16(hba->pci, AHCI_PCI_CFG_SUBSYS_VENDOR);
	uint8_t rev = pci_cfg_read8(hba->pci, AHCI_PCI_CFG_REVISION);

	/* Emulated controllers: a hypervisor bit, or a subsystem vendor of
	 * a VM product, means links come up instantly and the long debounce
	 * and spin-up waits only cost boot time. */
	if (ahci_running_on_hypervisor() || sv == 0x15ad || sv == 0x80ee ||
	    sv == 0x1af4 || sv == 0x1414) {
		if (!(hba->quirks & AHCI_Q_VM))
			kprintf("[ahci] %s: emulated controller (subsystem vendor 0x%04x)\n",
				hba->name, sv);
		hba->quirks |= AHCI_Q_VM;
	}

	/* AMD SB600 cannot do 64-bit DMA and its MSI does not work; the
	 * table flags the whole family, the revision is logged so a report
	 * says which stepping it was. */
	if (hba->pci->vendor_id == 0x1002 && hba->pci->device_id == 0x4380)
		kprintf("[ahci] %s: AMD SB600 revision 0x%02x: 32-bit DMA, no MSI\n",
			hba->name, rev);

	/* A 32-bit-only quirk always implies the AMD flavour of it too, so
	 * every later check needs to look at one bit only. */
	if (hba->quirks & AHCI_Q_AMD_32BIT)
		hba->quirks |= AHCI_Q_32BIT_ONLY;
}

/* ==========================================================================
 * ahci_pci_enable(): D0, BAR5, bus mastering, quirks
 * ========================================================================== */

int ahci_pci_enable(struct ahci_hba *hba)
{
	struct pci_bar bar;
	uint64_t map_base, map_size, virt;
	size_t pages;
	uint16_t cmd;
	uint32_t pi;
	int rc;

	BUG_ON(hba == NULL);
	BUG_ON(hba->pci == NULL);

	if (!ahci_pci_alive(hba)) {
		kprintf("[ahci] %s: function %02x:%02x.%u does not answer\n",
			hba->name, hba->pci->bus, hba->pci->device,
			hba->pci->function);
		return -ENODEV;
	}

	ahci_pci_runtime_quirks(hba);

	/* A function left in D3hot by the firmware would read all ones. */
	rc = ahci_pci_set_power(hba, 0);
	if (rc > 0)
		kprintf("[ahci] %s: woke the function from D%d\n", hba->name, rc);

	rc = pci_bar_decode(hba->pci, AHCI_PCI_ABAR, &bar);
	if (rc != 0) {
		kprintf("[ahci] %s: no BAR%d (ABAR) -- not usable (%d)\n",
			hba->name, AHCI_PCI_ABAR, rc);
		return -ENODEV;
	}
	if (bar.flags & PCI_BAR_IO) {
		kprintf("[ahci] %s: BAR%d is an I/O BAR, not ABAR\n", hba->name,
			AHCI_PCI_ABAR);
		return -ENODEV;
	}
	if (bar.base == 0) {
		kprintf("[ahci] %s: ABAR unassigned by the firmware\n",
			hba->name);
		return -ENODEV;
	}
	/* The window holds the generic registers and 0x80 bytes per port;
	 * small HBAs decode less than the 32-port maximum (0x1100). */
	if (bar.size == 0 || bar.size > (64ULL << 20)) {
		WARN(1, "[ahci] %s: implausible ABAR size 0x%llx, assuming 0x%x",
		     hba->name, (unsigned long long)bar.size,
		     AHCI_ABAR_MIN_SIZE);
		bar.size = AHCI_ABAR_MIN_SIZE;
	}
	if (bar.size < AHCI_PORT_BASE + AHCI_PORT_SIZE) {
		kprintf("[ahci] %s: ABAR of 0x%llx bytes cannot hold one port\n",
			hba->name, (unsigned long long)bar.size);
		return -ENODEV;
	}

	map_base = bar.base & ~0xFFFULL;
	map_size = (bar.base + bar.size) - map_base;
	pages = (size_t)((map_size + 0xFFFULL) >> 12);
	virt = mm_map_mmio_flags(map_base, pages, MM_MMIO_UC);
	if (!virt) {
		kprintf("[ahci] %s: cannot map ABAR 0x%llx (%lu pages)\n",
			hba->name, (unsigned long long)bar.base,
			(unsigned long)pages);
		return -ENOMEM;
	}
	hba->mmio_phys = bar.base;
	hba->mmio_size = bar.size;
	hba->mmio = (volatile uint8_t *)(virt + (bar.base - map_base));

	/* Memory decoding and bus mastering on; INTx masked until an
	 * interrupt mode is chosen (a shared line must not see us assert it
	 * while nobody would acknowledge it); parity/SERR reporting on. */
	pci_enable_busmaster_mem(hba->pci);
	cmd = ahci_pci_cmd_read(hba);
	cmd |= AHCI_PCI_CMD_MEM | AHCI_PCI_CMD_MASTER | PCI_CMD_INTX_DISABLE;
	ahci_pci_cmd_write(hba, cmd);
	cmd = ahci_pci_cmd_read(hba);
	if (!(cmd & AHCI_PCI_CMD_MEM) || !(cmd & AHCI_PCI_CMD_MASTER)) {
		kprintf("[ahci] %s: memory decode/bus master did not stick (cmd 0x%04x)\n",
			hba->name, cmd);
		return -ENODEV;
	}
	ahci_pci_report_status(hba, "enable");

	/* Interrupt capabilities, recorded for setup_irq and save/restore */
	hba->msi_cap = pci_find_capability(hba->pci, PCI_CAP_MSI);
	hba->msix_cap = pci_find_capability(hba->pci, PCI_CAP_MSIX);
	hba->irq_mode = AHCI_IRQ_POLL;
	hba->irq_vector = -1;
	hba->irq_gsi = -1;

	/* A register file that reads all ones is not there. */
	if (ahci_hba_readl(hba, AHCI_CAP) == 0xFFFFFFFFu &&
	    ahci_hba_readl(hba, AHCI_GHC) == 0xFFFFFFFFu) {
		kprintf("[ahci] %s: ABAR reads all ones\n", hba->name);
		return -ENODEV;
	}

	/* The Intel port enables need the implemented-port mask, which the
	 * register file gives regardless of AHCI mode. */
	pi = ahci_hba_readl(hba, AHCI_PI);
	ahci_pci_intel_pcs(hba, pi);

	sysfs_pci_set_driver(hba->pci, "ahci");

	kprintf("[ahci] %s: [%04x:%04x] %s at %02x:%02x.%u, ABAR 0x%llx size 0x%llx%s%s%s\n",
		hba->name, hba->pci->vendor_id, hba->pci->device_id,
		hba->id && hba->id->name ? hba->id->name : "AHCI",
		hba->pci->bus, hba->pci->device, hba->pci->function,
		(unsigned long long)hba->mmio_phys,
		(unsigned long long)hba->mmio_size,
		(bar.flags & PCI_BAR_MEM64) ? " 64-bit" : "",
		hba->msi_cap ? " MSI" : "", hba->msix_cap ? " MSI-X" : "");
	if (hba->quirks)
		kprintf("[ahci] %s: quirks 0x%08x\n", hba->name, hba->quirks);

	ahci_pci_save_state(hba);
	return 0;
}

/* ==========================================================================
 * INTx routing
 * ========================================================================== */

/* The bridge whose secondary bus is `bus' (class 06/04 PCI-PCI bridges and
 * whatever the PCI layer knows). */
static const pci_device_t *ahci_bridge_above(unsigned char bus)
{
	int n = 0;
	const pci_device_t *devs = pci_get_devices(&n);

	for (int i = 0; devs && i < n; i++) {
		const pci_device_t *b = &devs[i];

		if (b->class_code != 0x06 || b->subclass != 0x04)
			continue;
		if (((pci_cfg_read32_dev(b, 0x18) >> 8) & 0xFF) == bus)
			return b;
	}
	return pci_find_bridge_for_bus(bus);
}

/* The root bridge names firmware commonly uses for the host bridge. */
static const char *const ahci_root_bridges[] = {
	"\\\\_SB_.PCI0",
	"\\\\_SB_.PC00",
	"\\\\_SB_.PCI1",
	NULL,
};

int ahci_pci_intx_gsi(const pci_device_t *pci, int *active_low, int *level)
{
	uint8_t pin;
	uint32_t gsi = 0;

	if (!pci)
		return -EINVAL;
	/* PCI interrupts are level-triggered and active-low by definition;
	 * an ACPI link device could say otherwise, but the routing helper
	 * reports only the GSI. */
	if (active_low)
		*active_low = 1;
	if (level)
		*level = 1;

	pin = pci_cfg_read8(pci, AHCI_PCI_CFG_INT_LINE + 1);
	if (pin == 0xFF)
		return -ENODEV;
	if (pin >= 1 && pin <= 4) {
		uint8_t lpin = (uint8_t)(pin - 1);
		uint8_t ldev = pci->device;
		unsigned char bus = pci->bus;
		int depth = 0;

		/* Swizzle the pin through every bridge up to the root bus:
		 * a device behind a bridge on device D raises the bridge's
		 * pin (pin + D) mod 4. */
		while (bus != 0 && depth < 8) {
			const pci_device_t *br = ahci_bridge_above(bus);

			if (!br)
				break;
			lpin = (uint8_t)((lpin + ldev) % 4);
			ldev = br->device;
			bus = br->bus;
			depth++;
		}
		if (bus == 0) {
			for (int i = 0; ahci_root_bridges[i]; i++) {
				if (acpi_pci_lookup_irq(ahci_root_bridges[i],
							ldev, lpin, &gsi) == 0 &&
				    gsi <= 223)
					return (int)gsi;
			}
		}
	}
	/* No _PRT entry: the firmware's interrupt line (legacy PIC range,
	 * identity mapped to the I/O APIC). */
	if (pci->interrupt_line != 0xFF && pci->interrupt_line != 0 &&
	    pci->interrupt_line <= 23)
		return pci->interrupt_line;
	return -ENOENT;
}

/* ==========================================================================
 * MSI / MSI-X switching
 * ========================================================================== */

static void ahci_pci_msi_disable(struct ahci_hba *hba)
{
	uint16_t ctl;

	if (!hba->msi_cap)
		return;
	ctl = pci_cfg_read16(hba->pci, (uint8_t)(hba->msi_cap + AHCI_MSI_CTRL));
	if (ctl != 0xFFFF && (ctl & AHCI_MSI_CTRL_ENABLE))
		pci_cfg_write16(hba->pci, (uint8_t)(hba->msi_cap + AHCI_MSI_CTRL),
				(uint16_t)(ctl & ~AHCI_MSI_CTRL_ENABLE));
}

static void ahci_pci_msix_disable(struct ahci_hba *hba)
{
	uint16_t ctl;

	if (!hba->msix_cap)
		return;
	ctl = pci_cfg_read16(hba->pci,
			     (uint8_t)(hba->msix_cap + AHCI_MSIX_CTRL));
	if (ctl != 0xFFFF && (ctl & AHCI_MSIX_CTRL_ENABLE))
		pci_cfg_write16(hba->pci,
				(uint8_t)(hba->msix_cap + AHCI_MSIX_CTRL),
				(uint16_t)(ctl & ~AHCI_MSIX_CTRL_ENABLE));
}

/* Map MSI-X table entry 0 (for save/restore of its programming). */
static volatile uint32_t *ahci_pci_msix_entry0(struct ahci_hba *hba)
{
	uint32_t table, bir;
	uint64_t base, ent, page, virt;
	struct pci_bar bar;

	if (!hba->msix_cap)
		return NULL;
	table = pci_cfg_read32_dev(hba->pci,
				   (uint8_t)(hba->msix_cap + AHCI_MSIX_TABLE));
	bir = table & 7u;
	if (bir > 5 || pci_bar_decode(hba->pci, (int)bir, &bar) != 0 ||
	    (bar.flags & PCI_BAR_IO) || !bar.base)
		return NULL;
	base = bar.base;
	ent = base + (table & ~7u);
	page = ent & ~0xFFFULL;
	virt = mm_map_device_mmio(page, 2); /* an entry may straddle a page */
	if (!virt)
		return NULL;
	return (volatile uint32_t *)(virt + (ent - page));
}

static int ahci_pci_try_msix(struct ahci_hba *hba)
{
	int vec = -1, rc;

	if (!hba->msix_cap || (hba->quirks & (AHCI_Q_NO_MSIX | AHCI_Q_NO_MSI)))
		return -EOPNOTSUPP;
	ahci_pci_msi_disable(hba);
	rc = irq_request_msix(hba->pci, 0, 0, ahci_irq_handler, hba,
			      hba->name, &vec);
	if (rc != 0) {
		kprintf("[ahci] %s: MSI-X setup failed (%d)\n", hba->name, rc);
		ahci_pci_msix_disable(hba);
		return rc;
	}
	hba->irq_mode = AHCI_IRQ_MSIX;
	hba->irq_vector = vec;
	if (hba->index >= 0 && hba->index < AHCI_MAX_HBAS)
		g_ahci_msix_entry[hba->index] = ahci_pci_msix_entry0(hba);
	ahci_pci_intx(hba, 0);
	return 0;
}

static int ahci_pci_try_msi(struct ahci_hba *hba)
{
	int vec = -1, rc;
	uint16_t ctl;

	if (!hba->msi_cap || (hba->quirks & AHCI_Q_NO_MSI))
		return -EOPNOTSUPP;
	ahci_pci_msix_disable(hba);
	rc = irq_request_msi(hba->pci, 0, ahci_irq_handler, hba, hba->name,
			     &vec);
	if (rc != 0) {
		kprintf("[ahci] %s: MSI setup failed (%d)\n", hba->name, rc);
		ahci_pci_msi_disable(hba);
		return rc;
	}
	/* One message for the whole HBA: make sure the function was not
	 * left with a multi-message enable (it would then route port n's
	 * events to vector + n, which nobody listens to). */
	ctl = pci_cfg_read16(hba->pci, (uint8_t)(hba->msi_cap + AHCI_MSI_CTRL));
	if (ctl != 0xFFFF && (ctl & AHCI_MSI_CTRL_MME_MASK))
		pci_cfg_write16(hba->pci, (uint8_t)(hba->msi_cap + AHCI_MSI_CTRL),
				(uint16_t)(ctl & ~AHCI_MSI_CTRL_MME_MASK));
	hba->irq_mode = AHCI_IRQ_MSI;
	hba->irq_vector = vec;
	ahci_pci_intx(hba, 0);
	/* GHC.MRSM: the HBA fell back to a single message by itself */
	if (ahci_hba_readl(hba, AHCI_GHC) & AHCI_GHC_MRSM)
		kprintf("[ahci] %s: MSI reverted to single message (MRSM)\n",
			hba->name);
	return 0;
}

static int ahci_pci_try_intx(struct ahci_hba *hba)
{
	int active_low = 1, level = 1, gsi, rc;

	gsi = ahci_pci_intx_gsi(hba->pci, &active_low, &level);
	if (gsi < 0) {
		kprintf("[ahci] %s: no INTx routing (pin %u line %u)\n",
			hba->name, hba->pci->interrupt_pin,
			hba->pci->interrupt_line);
		return gsi;
	}
	ahci_pci_msi_disable(hba);
	ahci_pci_msix_disable(hba);
	rc = irq_request_gsi(gsi, ahci_irq_handler, hba, hba->name, active_low,
			     level);
	if (rc != 0) {
		kprintf("[ahci] %s: GSI %d request failed (%d)\n", hba->name,
			gsi, rc);
		return rc;
	}
	hba->irq_mode = AHCI_IRQ_INTX;
	hba->irq_gsi = gsi;
	hba->irq_vector = 32 + gsi;
	hba->irq_active_low = active_low;
	hba->irq_level = level;
	ahci_pci_intx(hba, 1);
	return 0;
}

int ahci_pci_setup_irq(struct ahci_hba *hba)
{
	BUG_ON(hba == NULL);
	WARN_ON(hba->irq_mode != AHCI_IRQ_POLL);

	if (ahci_pci_try_msix(hba) == 0) {
		kprintf("[ahci] %s: interrupts: MSI-X, vector %d\n", hba->name,
			hba->irq_vector);
	} else if (ahci_pci_try_msi(hba) == 0) {
		kprintf("[ahci] %s: interrupts: MSI, vector %d\n", hba->name,
			hba->irq_vector);
	} else if (ahci_pci_try_intx(hba) == 0) {
		kprintf("[ahci] %s: interrupts: INTx, GSI %d (%s, active-%s)\n",
			hba->name, hba->irq_gsi,
			hba->irq_level ? "level" : "edge",
			hba->irq_active_low ? "low" : "high");
	} else {
		hba->irq_mode = AHCI_IRQ_POLL;
		hba->irq_vector = -1;
		hba->irq_gsi = -1;
		ahci_pci_intx(hba, 0);
		kprintf("[ahci] %s: interrupts: none available -- polling every %u us\n",
			hba->name, AHCI_POLL_INTERVAL_US);
	}
	/* The new interrupt programming is what a restore must bring back. */
	ahci_pci_save_state(hba);
	return 0;
}

void ahci_pci_free_irq(struct ahci_hba *hba)
{
	BUG_ON(hba == NULL);

	switch (hba->irq_mode) {
	case AHCI_IRQ_MSIX:
		ahci_pci_msix_disable(hba);
		if (hba->irq_vector >= 0) {
			irq_free(hba->irq_vector, ahci_irq_handler, hba);
			irq_free_vector(hba->irq_vector);
		}
		if (hba->index >= 0 && hba->index < AHCI_MAX_HBAS)
			g_ahci_msix_entry[hba->index] = NULL;
		break;
	case AHCI_IRQ_MSI:
		ahci_pci_msi_disable(hba);
		if (hba->irq_vector >= 0) {
			irq_free(hba->irq_vector, ahci_irq_handler, hba);
			irq_free_vector(hba->irq_vector);
		}
		break;
	case AHCI_IRQ_INTX:
		ahci_pci_intx(hba, 0);
		if (hba->irq_vector >= 0)
			irq_free(hba->irq_vector, ahci_irq_handler, hba);
		break;
	case AHCI_IRQ_POLL:
	default:
		break;
	}
	hba->irq_mode = AHCI_IRQ_POLL;
	hba->irq_vector = -1;
	hba->irq_gsi = -1;
}

/* ==========================================================================
 * Configuration state save / restore
 * ========================================================================== */

void ahci_pci_save_state(struct ahci_hba *hba)
{
	BUG_ON(hba == NULL);

	for (int i = 0; i < 16; i++)
		hba->pci_saved[i] =
			pci_cfg_read32_dev(hba->pci, (unsigned char)(i * 4));

	hba->pci_saved_msi_ctl = 0;
	for (int i = 0; i < 4; i++)
		hba->pci_saved_msi[i] = 0;

	if (hba->irq_mode == AHCI_IRQ_MSI && hba->msi_cap) {
		uint8_t c = hba->msi_cap;
		uint16_t ctl = pci_cfg_read16(hba->pci, (uint8_t)(c + AHCI_MSI_CTRL));
		int is64 = (ctl & AHCI_MSI_CTRL_64BIT) != 0;

		hba->pci_saved_msi_ctl = ctl;
		hba->pci_saved_msi[0] =
			pci_cfg_read32_dev(hba->pci, (uint8_t)(c + AHCI_MSI_ADDR_LO));
		hba->pci_saved_msi[1] =
			is64 ? pci_cfg_read32_dev(hba->pci,
						  (uint8_t)(c + AHCI_MSI_ADDR_HI)) :
			       0;
		hba->pci_saved_msi[2] = pci_cfg_read16(
			hba->pci,
			(uint8_t)(c + (is64 ? AHCI_MSI_DATA_64 : AHCI_MSI_DATA_32)));
		hba->pci_saved_msi[3] =
			(ctl & AHCI_MSI_CTRL_PVM) ?
				pci_cfg_read32_dev(hba->pci,
						   (uint8_t)(c + (is64 ? AHCI_MSI_MASK_64 :
									 AHCI_MSI_MASK_32))) :
				0;
	} else if (hba->irq_mode == AHCI_IRQ_MSIX && hba->msix_cap) {
		volatile uint32_t *e = NULL;

		hba->pci_saved_msi_ctl = pci_cfg_read16(
			hba->pci, (uint8_t)(hba->msix_cap + AHCI_MSIX_CTRL));
		if (hba->index >= 0 && hba->index < AHCI_MAX_HBAS)
			e = g_ahci_msix_entry[hba->index];
		if (e) {
			for (int i = 0; i < 4; i++)
				hba->pci_saved_msi[i] = e[i];
		}
	}
}

void ahci_pci_restore_state(struct ahci_hba *hba)
{
	uint32_t id;

	BUG_ON(hba == NULL);

	id = pci_cfg_read32_dev(hba->pci, 0x00);
	if (id == 0xFFFFFFFFu) {
		WARN(1, "[ahci] %s: function gone, cannot restore its state",
		     hba->name);
		return;
	}
	if (hba->pci_saved[0] && id != hba->pci_saved[0]) {
		WARN(1, "[ahci] %s: function identity changed 0x%08x -> 0x%08x",
		     hba->name, hba->pci_saved[0], id);
		return;
	}

	/* Decoding off while the BARs are rewritten */
	ahci_pci_cmd_write(hba, (uint16_t)(ahci_pci_cmd_read(hba) &
					   ~(AHCI_PCI_CMD_MEM | AHCI_PCI_CMD_IO |
					     AHCI_PCI_CMD_MASTER)));

	/* BAR0..BAR5 (dwords 4..9), expansion ROM (dword 12) */
	for (int i = 4; i <= 9; i++) {
		if (pci_cfg_read32_dev(hba->pci, (unsigned char)(i * 4)) !=
		    hba->pci_saved[i])
			pci_cfg_write32_dev(hba->pci, (unsigned char)(i * 4),
					    hba->pci_saved[i]);
	}
	if (pci_cfg_read32_dev(hba->pci, AHCI_PCI_CFG_ROM) != hba->pci_saved[12])
		pci_cfg_write32_dev(hba->pci, AHCI_PCI_CFG_ROM, hba->pci_saved[12]);
	/* Cache line size and latency timer (dword 3, bytes 0-1) */
	pci_cfg_write8(hba->pci, AHCI_PCI_CFG_CACHE_LINE,
		       (uint8_t)(hba->pci_saved[3] & 0xFF));
	pci_cfg_write8(hba->pci, AHCI_PCI_CFG_LATENCY,
		       (uint8_t)((hba->pci_saved[3] >> 8) & 0xFF));
	/* Interrupt line (dword 15, byte 0) */
	pci_cfg_write8(hba->pci, AHCI_PCI_CFG_INT_LINE,
		       (uint8_t)(hba->pci_saved[15] & 0xFF));

	/* Interrupt programming */
	if (hba->irq_mode == AHCI_IRQ_MSI && hba->msi_cap) {
		uint8_t c = hba->msi_cap;
		uint16_t ctl = hba->pci_saved_msi_ctl;
		int is64 = (ctl & AHCI_MSI_CTRL_64BIT) != 0;

		pci_cfg_write16(hba->pci, (uint8_t)(c + AHCI_MSI_CTRL),
				(uint16_t)(ctl & ~AHCI_MSI_CTRL_ENABLE));
		pci_cfg_write32_dev(hba->pci, (uint8_t)(c + AHCI_MSI_ADDR_LO),
				    hba->pci_saved_msi[0]);
		if (is64)
			pci_cfg_write32_dev(hba->pci,
					    (uint8_t)(c + AHCI_MSI_ADDR_HI),
					    hba->pci_saved_msi[1]);
		pci_cfg_write16(hba->pci,
				(uint8_t)(c + (is64 ? AHCI_MSI_DATA_64 :
						      AHCI_MSI_DATA_32)),
				(uint16_t)hba->pci_saved_msi[2]);
		if (ctl & AHCI_MSI_CTRL_PVM)
			pci_cfg_write32_dev(hba->pci,
					    (uint8_t)(c + (is64 ? AHCI_MSI_MASK_64 :
								  AHCI_MSI_MASK_32)),
					    hba->pci_saved_msi[3]);
		pci_cfg_write16(hba->pci, (uint8_t)(c + AHCI_MSI_CTRL), ctl);
	} else if (hba->irq_mode == AHCI_IRQ_MSIX && hba->msix_cap) {
		volatile uint32_t *e = NULL;
		uint16_t ctl = hba->pci_saved_msi_ctl;

		if (hba->index >= 0 && hba->index < AHCI_MAX_HBAS)
			e = g_ahci_msix_entry[hba->index];
		/* function mask while the entry is rewritten */
		pci_cfg_write16(hba->pci,
				(uint8_t)(hba->msix_cap + AHCI_MSIX_CTRL),
				(uint16_t)(ctl | AHCI_MSIX_CTRL_FMASK));
		if (e && (hba->pci_saved_msi[0] || hba->pci_saved_msi[2])) {
			e[3] = hba->pci_saved_msi[3] | 1u;
			e[0] = hba->pci_saved_msi[0];
			e[1] = hba->pci_saved_msi[1];
			e[2] = hba->pci_saved_msi[2];
			e[3] = hba->pci_saved_msi[3];
		}
		pci_cfg_write16(hba->pci,
				(uint8_t)(hba->msix_cap + AHCI_MSIX_CTRL), ctl);
	} else {
		ahci_pci_msi_disable(hba);
		ahci_pci_msix_disable(hba);
	}

	/* Command register last: decoding, bus mastering and the INTx
	 * disable bit exactly as saved */
	ahci_pci_cmd_write(hba, (uint16_t)(hba->pci_saved[1] & 0xFFFF));
	ahci_pci_report_status(hba, "restore");
}
