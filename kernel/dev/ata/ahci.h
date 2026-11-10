// LikeOS AHCI (Serial ATA Advanced Host Controller Interface) driver
//
// The host controller side of SATA: the register map of an AHCI 1.3.1 HBA
// with every field, the structures the HBA reads and writes in memory
// (command list, command tables, PRDT, received-FIS area), and the
// driver's objects -- struct ahci_hba (one controller), struct ahci_port
// (one SATA port), struct ahci_link (the port's link, or one fan-out link
// behind a port multiplier) and struct ahci_cmd (one command slot).
//
// The driver is split along the hardware (file, owner agent):
//
//   ahci_pci.c    A1  PCI detection (class 01/06/01 + known IDs, quirks),
//                     BAR5 mapping, bus mastering, MSI-X / MSI / INTx (_PRT
//                     routing with bridge swizzle) / polling fallback, PCI
//                     state save and restore
//   ahci_hba.c    A1  capabilities, BIOS/OS handoff, HBA reset, global
//                     init, port enumeration, staggered spin-up, the kahci
//                     worker thread, controller recovery, shutdown,
//                     suspend/resume, ahci_init()/ahci_late_init()
//   ahci_irq.c    A1  the interrupt handler and its polled twin: global
//                     IS, per-port PxIS, completion and error dispatch
//   ahci_dma.c    A1  DMA memory (command lists, FIS areas, command
//                     tables), sg -> PRDT translation, 32-bit HBA bounce
//                     buffers, barriers, the IOMMU translation hook
//   ahci_port.c   A2  port start/stop (ST/FRE/CR/FR), COMRESET, SRST,
//                     link up, speed negotiation, signature classification,
//                     hot-plug, link power management (HIPM/DIPM/ALPM/
//                     APST/DevSleep), suspend/resume of a port
//   ahci_pmp.c    A2  port multipliers (GSCR/PSCR access, fan-out link
//                     reset and enumeration, notification), FIS-based
//                     switching
//   ahci_cmd.c    A3  slot allocation, command header/FIS/PRDT setup,
//                     submission (CI / SACT), completion, NCQ tags,
//                     per-command timeouts, the ATA transport
//   ahci_eh.c     A3  error handling: freeze/thaw, NCQ error recovery via
//                     log 10h, the recovery ladder (abort -> CLO -> SRST
//                     -> COMRESET -> HBA reset), hot-plug and timeout
//                     processing in the kahci thread
//   ahci_debug.c  A1  register dumps, error counters, the command trace
//   (+A3 hooks)       ring, /sys/class/ata_port|ata_link|ata_device, the
//                     debug dump files, fault injection
//
// ---- Return convention -----------------------------------------------------------
// Every function declared here returns 0 (or a documented non-negative
// value such as a slot number or a count) on success and a NEGATIVE ERRNO
// (kernel/ke/syscall.h) on failure -- -ETIMEDOUT for a register that did
// not reach its state in time, -ENODEV for no device / link down, -EIO for
// a device or controller error, -ENOMEM, -EBUSY (no free slot, EH busy),
// -EINVAL, -EOPNOTSUPP (capability missing).  Functions returning void
// cannot fail.  The ST_* codes are not used anywhere in the ATA/AHCI
// driver.
//
// ---- Context rules -----------------------------------------------------------
//   "any context"     callable with interrupts off and from the interrupt
//                     handler / an hrtimer callback; never sleeps.
//   "may sleep"       process context (the kahci thread, a task in a
//                     syscall); starts with might_sleep().  Before the
//                     scheduler runs (ahci_init() at boot) the same paths
//                     run by polling: delays busy-wait (ahci_msleep falls
//                     back to ahci_mdelay) and waits poll the port.
//   "eh_mutex held"   the caller holds port->eh_mutex.
//   "_locked"         the caller holds the named spinlock
//                     (lockdep_assert_held() inside).
//
// ---- Locking (mandatory order, outermost first) --------------------------------
//   port->eh_mutex        (struct ata_mutex, sleeping): port reset, error
//                         recovery, hot-plug, PMP enumeration, LPM changes,
//                         port suspend/resume, device probe/revalidate
//     -> g_disk_registry_mutex (block layer, sleeping): taken inside the
//                         eh_mutex, because probe and hot-plug register
//                         and remove disks (disk_add/disk_del) under it;
//                         never take an eh_mutex with the registry held
//       -> port->lock     (spinlock, irqsave): every Px register write
//                         that changes state, the slot bitmap, CI/SACT
//                         submission, per-slot state, NCQ tag table,
//                         eh_pending
//         -> hba->lock    (spinlock, irqsave): GHC, IS, BOHC, CCC, EM,
//                         CAP-dependent global state, the port array
// Several ports' eh_mutexes are taken only by HBA recovery, in ascending
// port index, from the kahci thread.  An ata_completion's lock, the trace
// ring lock and the block queue lock are leaves.
//
// The interrupt handler: read IS under hba->lock, DROP it, then per set
// bit take port->lock and, in that one hold, read + acknowledge PxIS,
// collect finished slots (active & ~PxCI, ncq & ~PxSACT) -- on an error
// event too, before the error is looked at -- release the slots and
// complete the collected commands (ata_qc_complete: result, an internal
// waiter woken, a block request handed to the block softirq -- nothing
// there issues a command or ends a block request), DROP port->lock;
// finally acknowledge IS under hba->lock.  An error event clears SError,
// aborts the commands still in flight for error handling (freezing the
// port only for AHCI_PxIS_FREEZE events) and hands the port to the kahci
// thread; the handler never resets anything itself.
//
// Never sleep with a spinlock held; might_sleep() in every sleeping path,
// lockdep_assert_held() in every _locked helper.
//
// ---- Execution model -------------------------------------------------------------
// Boot (interrupts off, no scheduler): ahci_init() from kernel/ke/init.c
// before storage_fs_init() detects every HBA, resets and enables it,
// brings each port's link up, classifies and IDENTIFYs the devices,
// configures them and registers their disks with partitions -- all in
// POLLED mode (hba->irq_mode == AHCI_IRQ_POLL, completions by polling).
//
// After sti: ahci_late_init() switches each HBA to interrupts (MSI-X ->
// MSI -> INTx through _PRT -> stays polled with a 1 ms poll timer) and
// starts one "kahci<N>" kernel thread per HBA for error handling,
// timeouts, hot-plug, LPM and the polling fallback.
//
// Request path: blk_submit -> blk_queue (dispatch in the submitting task)
// -> ata_blk_submit (ATA layer builds the qc) -> ahci_qc_issue (slot/tag,
// header, FIS, PRDT, CI/SACT) -> interrupt (acknowledge, collect the
// finished slots, ata_qc_complete -> blk_complete_request) -> block
// softirq on the submitting CPU (ata_blk_complete -> blk_request_complete
// -> end_io / waiter) -> kblockd restarts the queue when dispatch had
// stopped for want of a slot.  Timeouts: the block queue's timeout work
// -> ata_blk_timeout -> ahci_xport_qc_timed_out -> error handling.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_ATA_AHCI_H
#define KERNEL_DEV_ATA_AHCI_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/dev/ata/ata.h>
#include <kernel/dev/block/blkdev.h>

struct ahci_hba;
struct ahci_port;
struct ahci_link;
struct ahci_cmd;

/* ==========================================================================
 * Limits
 * ========================================================================== */

#define AHCI_MAX_HBAS 8
#define AHCI_MAX_PORTS 32 /* CAP.NP is 5 bits */
#define AHCI_MAX_SLOTS 32 /* CAP.NCS is 5 bits */
/* PRDT entries per command table: 248 makes a table exactly one 4 KiB
 * page (128-byte header + 248 x 16).  With 4 KiB pages that are not
 * physically contiguous this still carries 992 KiB per command. */
#define AHCI_MAX_PRD 248
#define AHCI_CMD_TABLE_SIZE (AHCI_CMD_TABLE_HDR_SIZE + AHCI_MAX_PRD * 16)
#define AHCI_CMD_TABLE_HDR_SIZE 0x80
/* The largest transfer the driver offers the block layer per command:
 * (AHCI_MAX_PRD - 1) worst-case 4 KiB pages (one entry spare for an
 * unaligned start), in 512-byte sectors. */
#define AHCI_MAX_SECTORS (((AHCI_MAX_PRD - 1) * 4096) / 512)
#define AHCI_PRD_MAX_BYTES (4u * 1024 * 1024) /* DBC: 22 bits, 4 MiB */
#define AHCI_PRD_ALIGN 2 /* DBA bit 0 reserved; DBC odd */
#define AHCI_CMD_LIST_SIZE (AHCI_MAX_SLOTS * 32) /* 1 KiB */
#define AHCI_CMD_LIST_ALIGN 1024
#define AHCI_RX_FIS_SIZE 256
#define AHCI_RX_FIS_ALIGN 256
#define AHCI_RX_FIS_FBS_SIZE 4096 /* 16 x 256, one per PMP port */
#define AHCI_RX_FIS_FBS_ALIGN 4096
#define AHCI_CMD_TABLE_ALIGN 128
#define AHCI_PMP_MAX_LINKS ATA_PMP_MAX_PORTS /* 15 fan-out links */
#define AHCI_KAHCI_STACK_SIZE 16384
#define AHCI_TRACE_ENTRIES 256 /* per port */

/* ==========================================================================
 * Timing (milliseconds unless noted; AHCI 1.3.1 and SATA requirements)
 * ========================================================================== */

#define AHCI_HR_TIMEOUT_MS 1000 /* GHC.HR clears within 1 s */
#define AHCI_BOHC_BOS_WAIT_MS 25 /* BIOS sets BB within 25 ms */
#define AHCI_BOHC_BB_WAIT_MS 2000 /* BIOS busy: wait at least 2 s */
#define AHCI_ENGINE_STOP_MS 500 /* PxCMD.CR clears within 500 ms */
#define AHCI_FIS_RX_STOP_MS 500 /* PxCMD.FR clears within 500 ms */
#define AHCI_CLO_TIMEOUT_MS 500 /* PxCMD.CLO self-clears */
#define AHCI_COMRESET_ASSERT_MS 1 /* DET=1 held >= 1 ms */
#define AHCI_LINK_UP_MS 1000 /* DET=3 after COMRESET */
#define AHCI_DEV_READY_MS 10000 /* BSY/DRQ clear after link up */
#define AHCI_DEV_READY_SLOW_MS 31000 /* spinning drives: up to 31 s */
/* Error handling's reset attempts: the deadline of each (most drives
 * spin up within 10 s, nearly all within 20 s, outliers get 35 s more,
 * and a short last chance), the pause before another reset in the same
 * run, and how long a port multiplier gets to answer a COMRESET before
 * the follow-up SRST. */
#define AHCI_EH_RESET_TIMEOUTS { 10000, 10000, 35000, 5000 }
#define AHCI_EH_RESET_COOL_DOWN_MS 5000
#define AHCI_PMP_SRST_WAIT_MS 10000
/* Recovery of a port multiplier: tries of the multiplier itself before it
 * is given up, tries of each fan-out link (per EH run) before the link is
 * disabled, and the quick reset of a disabled link (so that its PhyRdy
 * change notification keeps working). */
#define AHCI_EH_PMP_TRIES 5
#define AHCI_EH_PMP_LINK_TRIES 3
/* Revalidation tries of a device in one EH run before it is disabled. */
#define AHCI_EH_DEV_TRIES 3
#define AHCI_TMOUT_INTERNAL_QUICK_MS 5000
#define AHCI_SRST_ASSERT_US 100 /* between SRST set and clear FIS */
#define AHCI_SRST_TIMEOUT_MS 5000 /* SRST FIS (polled, C bit) */
#define AHCI_SPINUP_STAGGER_MS 100 /* CAP.SSS: between port SUD */
#define AHCI_POLL_INTERVAL_US 1000 /* polled-mode kahci period */
#define AHCI_LPM_SETTLE_MS 10
#define AHCI_EH_MAX_TRIES 3 /* per ladder rung */

/* ==========================================================================
 * Generic host control registers (offsets from ABAR, BAR5)
 * ========================================================================== */

#define AHCI_CAP 0x00 /* host capabilities */
#define AHCI_GHC 0x04 /* global host control */
#define AHCI_IS 0x08 /* interrupt status (RWC, one bit per port) */
#define AHCI_PI 0x0C /* ports implemented */
#define AHCI_VS 0x10 /* version */
#define AHCI_CCC_CTL 0x14 /* command completion coalescing control */
#define AHCI_CCC_PORTS 0x18 /* command completion coalescing ports */
#define AHCI_EM_LOC 0x1C /* enclosure management location */
#define AHCI_EM_CTL 0x20 /* enclosure management control */
#define AHCI_CAP2 0x24 /* host capabilities extended */
#define AHCI_BOHC 0x28 /* BIOS/OS handoff control and status */
#define AHCI_VENDOR 0xA0 /* 0xA0..0xFF vendor specific */
#define AHCI_PORT_BASE 0x100 /* port 0 registers */
#define AHCI_PORT_SIZE 0x80 /* per port */
#define AHCI_ABAR_MIN_SIZE (AHCI_PORT_BASE + AHCI_MAX_PORTS * AHCI_PORT_SIZE)

/* CAP -- host capabilities (RO) */
#define AHCI_CAP_S64A (1u << 31) /* 64-bit addressing */
#define AHCI_CAP_SNCQ (1u << 30) /* native command queuing */
#define AHCI_CAP_SSNTF (1u << 29) /* SNotification register */
#define AHCI_CAP_SMPS (1u << 28) /* mechanical presence switch */
#define AHCI_CAP_SSS (1u << 27) /* staggered spin-up */
#define AHCI_CAP_SALP (1u << 26) /* aggressive link power management */
#define AHCI_CAP_SAL (1u << 25) /* activity LED */
#define AHCI_CAP_SCLO (1u << 24) /* command list override */
#define AHCI_CAP_ISS_SHIFT 20 /* interface speed support */
#define AHCI_CAP_ISS_MASK (0xFu << AHCI_CAP_ISS_SHIFT)
#define AHCI_CAP_SNZO (1u << 19) /* reserved (was: non-zero DMA offsets) */
#define AHCI_CAP_SAM (1u << 18) /* AHCI mode only (no legacy) */
#define AHCI_CAP_SPM (1u << 17) /* port multiplier */
#define AHCI_CAP_FBSS (1u << 16) /* FIS-based switching */
#define AHCI_CAP_PMD (1u << 15) /* PIO multiple DRQ block */
#define AHCI_CAP_SSC (1u << 14) /* slumber state capable */
#define AHCI_CAP_PSC (1u << 13) /* partial state capable */
#define AHCI_CAP_NCS_SHIFT 8 /* number of command slots - 1 */
#define AHCI_CAP_NCS_MASK (0x1Fu << AHCI_CAP_NCS_SHIFT)
#define AHCI_CAP_CCCS (1u << 7) /* command completion coalescing */
#define AHCI_CAP_EMS (1u << 6) /* enclosure management */
#define AHCI_CAP_SXS (1u << 5) /* external SATA */
#define AHCI_CAP_NP_MASK 0x1Fu /* number of ports - 1 */
/* ISS values */
#define AHCI_ISS_GEN1 1 /* 1.5 Gb/s */
#define AHCI_ISS_GEN2 2 /* 3 Gb/s */
#define AHCI_ISS_GEN3 3 /* 6 Gb/s */

/* GHC -- global host control */
#define AHCI_GHC_AE (1u << 31) /* AHCI enable */
#define AHCI_GHC_MRSM (1u << 2) /* MSI revert to single message (RO) */
#define AHCI_GHC_IE (1u << 1) /* interrupt enable */
#define AHCI_GHC_HR (1u << 0) /* HBA reset (self-clearing) */

/* VS -- version: major in 31:16, minor in 15:0 (BCD-ish) */
#define AHCI_VS_0_95 0x00000905u
#define AHCI_VS_1_0 0x00010000u
#define AHCI_VS_1_1 0x00010100u
#define AHCI_VS_1_2 0x00010200u
#define AHCI_VS_1_3 0x00010300u
#define AHCI_VS_1_3_1 0x00010301u
#define AHCI_VS_MAJOR(v) ((v) >> 16)
#define AHCI_VS_MINOR(v) ((v)&0xFFFFu)

/* CCC_CTL -- command completion coalescing control */
#define AHCI_CCC_TV_SHIFT 16 /* timeout value, ms */
#define AHCI_CCC_TV_MASK (0xFFFFu << AHCI_CCC_TV_SHIFT)
#define AHCI_CCC_CC_SHIFT 8 /* command completions */
#define AHCI_CCC_CC_MASK (0xFFu << AHCI_CCC_CC_SHIFT)
#define AHCI_CCC_INT_SHIFT 3 /* interrupt number (RO) */
#define AHCI_CCC_INT_MASK (0x1Fu << AHCI_CCC_INT_SHIFT)
#define AHCI_CCC_EN (1u << 0)

/* EM_LOC -- enclosure management message buffer location */
#define AHCI_EM_LOC_OFST_SHIFT 16 /* dword offset from ABAR */
#define AHCI_EM_LOC_SZ_MASK 0xFFFFu /* size in dwords */

/* EM_CTL -- enclosure management control */
#define AHCI_EM_CTL_ATTR_PM (1u << 27) /* port multiplier support */
#define AHCI_EM_CTL_ATTR_ALHD (1u << 26) /* activity LED hw driven */
#define AHCI_EM_CTL_ATTR_XMT (1u << 25) /* transmit only */
#define AHCI_EM_CTL_ATTR_SMB (1u << 24) /* single message buffer */
#define AHCI_EM_CTL_SUPP_SGPIO (1u << 19)
#define AHCI_EM_CTL_SUPP_SES2 (1u << 18)
#define AHCI_EM_CTL_SUPP_SAFTE (1u << 17)
#define AHCI_EM_CTL_SUPP_LED (1u << 16)
#define AHCI_EM_CTL_RST (1u << 9) /* reset */
#define AHCI_EM_CTL_TM (1u << 8) /* transmit message */
#define AHCI_EM_CTL_MR (1u << 0) /* message received (RWC) */

/* CAP2 -- host capabilities extended (RO) */
#define AHCI_CAP2_DESO (1u << 5) /* DevSleep entrance from slumber only */
#define AHCI_CAP2_SADM (1u << 4) /* aggressive device sleep management */
#define AHCI_CAP2_SDS (1u << 3) /* device sleep */
#define AHCI_CAP2_APST (1u << 2) /* automatic partial to slumber */
#define AHCI_CAP2_NVMP (1u << 1) /* NVMHCI present */
#define AHCI_CAP2_BOH (1u << 0) /* BIOS/OS handoff */

/* BOHC -- BIOS/OS handoff control and status */
#define AHCI_BOHC_BB (1u << 4) /* BIOS busy */
#define AHCI_BOHC_OOC (1u << 3) /* OS ownership change (RWC) */
#define AHCI_BOHC_SOOE (1u << 2) /* SMI on OS ownership change enable */
#define AHCI_BOHC_OOS (1u << 1) /* OS owned semaphore */
#define AHCI_BOHC_BOS (1u << 0) /* BIOS owned semaphore */

/* ==========================================================================
 * Port registers (offsets from the port's base: ABAR + 0x100 + n * 0x80)
 * ========================================================================== */

#define AHCI_PxCLB 0x00 /* command list base, 1 KiB aligned */
#define AHCI_PxCLBU 0x04 /* command list base, upper 32 bits */
#define AHCI_PxFB 0x08 /* FIS base, 256 B aligned (4 KiB with FBS) */
#define AHCI_PxFBU 0x0C /* FIS base, upper 32 bits */
#define AHCI_PxIS 0x10 /* interrupt status (RWC) */
#define AHCI_PxIE 0x14 /* interrupt enable */
#define AHCI_PxCMD 0x18 /* command and status */
#define AHCI_PxTFD 0x20 /* task file data (RO) */
#define AHCI_PxSIG 0x24 /* signature (RO) */
#define AHCI_PxSSTS 0x28 /* SATA status (SCR0: SStatus) */
#define AHCI_PxSCTL 0x2C /* SATA control (SCR2: SControl) */
#define AHCI_PxSERR 0x30 /* SATA error (SCR1: SError, RWC) */
#define AHCI_PxSACT 0x34 /* SATA active (SCR3: SActive) */
#define AHCI_PxCI 0x38 /* command issue */
#define AHCI_PxSNTF 0x3C /* SATA notification (SCR4) */
#define AHCI_PxFBS 0x40 /* FIS-based switching control */
#define AHCI_PxDEVSLP 0x44 /* device sleep */
#define AHCI_PxVS 0x70 /* 0x70..0x7F vendor specific */

/* PxIS / PxIE -- interrupt status / enable (same bit positions) */
#define AHCI_PxIS_CPDS (1u << 31) /* cold port detect */
#define AHCI_PxIS_TFES (1u << 30) /* task file error */
#define AHCI_PxIS_HBFS (1u << 29) /* host bus fatal error */
#define AHCI_PxIS_HBDS (1u << 28) /* host bus data error */
#define AHCI_PxIS_IFS (1u << 27) /* interface fatal error */
#define AHCI_PxIS_INFS (1u << 26) /* interface non-fatal error */
#define AHCI_PxIS_OFS (1u << 24) /* overflow */
#define AHCI_PxIS_IPMS (1u << 23) /* incorrect port multiplier */
#define AHCI_PxIS_PRCS (1u << 22) /* PhyRdy change (RO; clear SERR.N) */
#define AHCI_PxIS_DMPS (1u << 7) /* device mechanical presence */
#define AHCI_PxIS_PCS (1u << 6) /* port connect change (RO; SERR.X) */
#define AHCI_PxIS_DPS (1u << 5) /* descriptor processed (PRD I bit) */
#define AHCI_PxIS_UFS (1u << 4) /* unknown FIS (RO; SERR.DIAG.F) */
#define AHCI_PxIS_SDBS (1u << 3) /* set device bits FIS */
#define AHCI_PxIS_DSS (1u << 2) /* DMA setup FIS */
#define AHCI_PxIS_PSS (1u << 1) /* PIO setup FIS */
#define AHCI_PxIS_DHRS (1u << 0) /* device to host register FIS */
/* Groups the driver works with */
#define AHCI_PxIS_ERROR_FATAL                                             \
	(AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)
/* The events after which the port is frozen (PxIE masked, nothing new
 * admitted) until error handling has run: the host bus or the link
 * failed, the device or the link changed, the HBA got a FIS it cannot
 * place.  The port's state is unknown after any of them. */
#define AHCI_PxIS_FREEZE                                                 \
	(AHCI_PxIS_HBFS | AHCI_PxIS_IFS | AHCI_PxIS_PCS | AHCI_PxIS_PRCS | \
	 AHCI_PxIS_UFS | AHCI_PxIS_IPMS)
/* The events that hand the port to error handling: the freezing ones,
 * plus a device error (TFES) and a host bus data error (HBDS), after
 * which the commands still in flight are aborted for error handling but
 * the port keeps its interrupts.  The interface non-fatal error (INFS),
 * the overflow (OFS) and the cold presence / mechanical presence events
 * are none of them: they are not enabled, and seen in PxIS next to an
 * enabled event they are acknowledged and otherwise ignored. */
#define AHCI_PxIS_ERROR                                                  \
	(AHCI_PxIS_FREEZE | AHCI_PxIS_TFES | AHCI_PxIS_HBDS)
#define AHCI_PxIS_HOTPLUG                                             \
	(AHCI_PxIS_CPDS | AHCI_PxIS_PRCS | AHCI_PxIS_PCS |            \
	 AHCI_PxIS_DMPS)
#define AHCI_PxIS_COMPLETE                                              \
	(AHCI_PxIS_SDBS | AHCI_PxIS_DSS | AHCI_PxIS_PSS | AHCI_PxIS_DHRS | \
	 AHCI_PxIS_DPS)
/* The PxIE value of a running port: every error event and every FIS
 * that can end a command.  DPS is part of it, but no PRD entry ever
 * carries the I bit, so it never fires.  Link power management takes
 * PRCS out while a policy other than max_performance is active. */
#define AHCI_PxIE_DEFAULT                                               \
	(AHCI_PxIS_ERROR | AHCI_PxIS_DPS | AHCI_PxIS_SDBS | AHCI_PxIS_DSS | \
	 AHCI_PxIS_PSS | AHCI_PxIS_DHRS)

/* PxCMD -- command and status */
#define AHCI_PxCMD_ICC_SHIFT 28 /* interface communication control */
#define AHCI_PxCMD_ICC_MASK (0xFu << AHCI_PxCMD_ICC_SHIFT)
#define AHCI_PxCMD_ICC_IDLE (0x0u << AHCI_PxCMD_ICC_SHIFT) /* no-op */
#define AHCI_PxCMD_ICC_ACTIVE (0x1u << AHCI_PxCMD_ICC_SHIFT)
#define AHCI_PxCMD_ICC_PARTIAL (0x2u << AHCI_PxCMD_ICC_SHIFT)
#define AHCI_PxCMD_ICC_SLUMBER (0x6u << AHCI_PxCMD_ICC_SHIFT)
#define AHCI_PxCMD_ICC_DEVSLEEP (0x8u << AHCI_PxCMD_ICC_SHIFT)
#define AHCI_PxCMD_ASP (1u << 27) /* aggressive slumber/partial */
#define AHCI_PxCMD_ALPE (1u << 26) /* aggressive LPM enable */
#define AHCI_PxCMD_DLAE (1u << 25) /* drive LED on ATAPI enable */
#define AHCI_PxCMD_ATAPI (1u << 24) /* device is ATAPI */
#define AHCI_PxCMD_APSTE (1u << 23) /* auto partial to slumber enable */
#define AHCI_PxCMD_FBSCP (1u << 22) /* FIS-based switching capable (RO) */
#define AHCI_PxCMD_ESP (1u << 21) /* external SATA port (RO) */
#define AHCI_PxCMD_CPD (1u << 20) /* cold presence detection (RO) */
#define AHCI_PxCMD_MPSP (1u << 19) /* mechanical presence switch (RO) */
#define AHCI_PxCMD_HPCP (1u << 18) /* hot plug capable port (RO) */
#define AHCI_PxCMD_PMA (1u << 17) /* port multiplier attached */
#define AHCI_PxCMD_CPS (1u << 16) /* cold presence state (RO) */
#define AHCI_PxCMD_CR (1u << 15) /* command list running (RO) */
#define AHCI_PxCMD_FR (1u << 14) /* FIS receive running (RO) */
#define AHCI_PxCMD_MPSS (1u << 13) /* mechanical presence switch state */
#define AHCI_PxCMD_CCS_SHIFT 8 /* current command slot (RO) */
#define AHCI_PxCMD_CCS_MASK (0x1Fu << AHCI_PxCMD_CCS_SHIFT)
#define AHCI_PxCMD_FRE (1u << 4) /* FIS receive enable */
#define AHCI_PxCMD_CLO (1u << 3) /* command list override */
#define AHCI_PxCMD_POD (1u << 2) /* power on device (CPD ports) */
#define AHCI_PxCMD_SUD (1u << 1) /* spin-up device (SSS) */
#define AHCI_PxCMD_ST (1u << 0) /* start */

/* PxTFD -- task file data */
#define AHCI_PxTFD_ERR_SHIFT 8
#define AHCI_PxTFD_ERR_MASK (0xFFu << AHCI_PxTFD_ERR_SHIFT)
#define AHCI_PxTFD_STS_MASK 0xFFu
#define AHCI_PxTFD_STS_BSY ATA_BUSY
#define AHCI_PxTFD_STS_DRQ ATA_DRQ
#define AHCI_PxTFD_STS_ERR ATA_ERR

/* PxSIG -- signature: count 7:0, LBA low 15:8, mid 23:16, high 31:24 */
#define AHCI_PxSIG_INVALID 0xFFFFFFFFu

/* PxSSTS -- SStatus */
#define AHCI_SSTS_IPM_SHIFT 8 /* interface power management */
#define AHCI_SSTS_IPM_MASK (0xFu << AHCI_SSTS_IPM_SHIFT)
#define AHCI_SSTS_IPM_NONE 0 /* not present / no communication */
#define AHCI_SSTS_IPM_ACTIVE 1
#define AHCI_SSTS_IPM_PARTIAL 2
#define AHCI_SSTS_IPM_SLUMBER 6
#define AHCI_SSTS_IPM_DEVSLEEP 8
#define AHCI_SSTS_SPD_SHIFT 4 /* current interface speed */
#define AHCI_SSTS_SPD_MASK (0xFu << AHCI_SSTS_SPD_SHIFT)
#define AHCI_SSTS_SPD_NONE 0
#define AHCI_SSTS_SPD_GEN1 1 /* 1.5 Gb/s */
#define AHCI_SSTS_SPD_GEN2 2 /* 3 Gb/s */
#define AHCI_SSTS_SPD_GEN3 3 /* 6 Gb/s */
#define AHCI_SSTS_DET_MASK 0xFu /* device detection */
#define AHCI_SSTS_DET_NONE 0 /* no device, no PHY communication */
#define AHCI_SSTS_DET_PRESENT 1 /* device present, no PHY comm */
#define AHCI_SSTS_DET_PHY 3 /* device present, PHY established */
#define AHCI_SSTS_DET_OFFLINE 4 /* PHY offline (disabled / BIST) */
#define AHCI_SSTS_IPM(v) (((v)&AHCI_SSTS_IPM_MASK) >> AHCI_SSTS_IPM_SHIFT)
#define AHCI_SSTS_SPD(v) (((v)&AHCI_SSTS_SPD_MASK) >> AHCI_SSTS_SPD_SHIFT)
#define AHCI_SSTS_DET(v) ((v)&AHCI_SSTS_DET_MASK)

/* PxSCTL -- SControl */
#define AHCI_SCTL_PMP_SHIFT 16 /* port multiplier port (unused) */
#define AHCI_SCTL_PMP_MASK (0xFu << AHCI_SCTL_PMP_SHIFT)
#define AHCI_SCTL_SPM_SHIFT 12 /* select power management (unused) */
#define AHCI_SCTL_SPM_MASK (0xFu << AHCI_SCTL_SPM_SHIFT)
#define AHCI_SCTL_IPM_SHIFT 8 /* transitions NOT allowed */
#define AHCI_SCTL_IPM_MASK (0xFu << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_ANY (0x0u << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_NO_PARTIAL (0x1u << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_NO_SLUMBER (0x2u << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_NO_PS (0x3u << AHCI_SCTL_IPM_SHIFT) /* neither */
#define AHCI_SCTL_IPM_NO_DEVSLP (0x4u << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_NO_PD (0x5u << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_NO_SD (0x6u << AHCI_SCTL_IPM_SHIFT)
#define AHCI_SCTL_IPM_NONE (0x7u << AHCI_SCTL_IPM_SHIFT) /* all off */
#define AHCI_SCTL_SPD_SHIFT 4 /* speed allowed (0 no limit) */
#define AHCI_SCTL_SPD_MASK (0xFu << AHCI_SCTL_SPD_SHIFT)
#define AHCI_SCTL_DET_MASK 0xFu /* device detection initialization */
#define AHCI_SCTL_DET_NONE 0x0 /* no action */
#define AHCI_SCTL_DET_COMRESET 0x1 /* perform interface init (COMRESET) */
#define AHCI_SCTL_DET_DISABLE 0x4 /* disable SATA, PHY offline */

/* PxSERR -- SError (RWC).  DIAG in 31:16, ERR in 15:0. */
#define AHCI_SERR_DIAG_X (1u << 26) /* exchanged (COMINIT seen) */
#define AHCI_SERR_DIAG_F (1u << 25) /* unknown FIS type */
#define AHCI_SERR_DIAG_T (1u << 24) /* transport state transition error */
#define AHCI_SERR_DIAG_S (1u << 23) /* link sequence error */
#define AHCI_SERR_DIAG_H (1u << 22) /* handshake error */
#define AHCI_SERR_DIAG_C (1u << 21) /* CRC error */
#define AHCI_SERR_DIAG_D (1u << 20) /* disparity error */
#define AHCI_SERR_DIAG_B (1u << 19) /* 10B to 8B decode error */
#define AHCI_SERR_DIAG_W (1u << 18) /* comm wake */
#define AHCI_SERR_DIAG_I (1u << 17) /* PHY internal error */
#define AHCI_SERR_DIAG_N (1u << 16) /* PhyRdy change */
#define AHCI_SERR_ERR_E (1u << 11) /* internal error */
#define AHCI_SERR_ERR_P (1u << 10) /* protocol error */
#define AHCI_SERR_ERR_C (1u << 9) /* persistent comm / data integrity */
#define AHCI_SERR_ERR_T (1u << 8) /* transient data integrity error */
#define AHCI_SERR_ERR_M (1u << 1) /* recovered communications error */
#define AHCI_SERR_ERR_I (1u << 0) /* recovered data integrity error */
#define AHCI_SERR_ALL 0xFFFFFFFFu /* write to clear everything */
#define AHCI_SERR_HOTPLUG (AHCI_SERR_DIAG_X | AHCI_SERR_DIAG_N)

/* PxSNTF -- SNotification */
#define AHCI_SNTF_PMN_MASK 0xFFFFu /* PM notify, one bit per PMP port */

/* PxFBS -- FIS-based switching control */
#define AHCI_PxFBS_DWE_SHIFT 16 /* device with error (RO) */
#define AHCI_PxFBS_DWE_MASK (0xFu << AHCI_PxFBS_DWE_SHIFT)
#define AHCI_PxFBS_ADO_SHIFT 12 /* active device optimization (RO) */
#define AHCI_PxFBS_ADO_MASK (0xFu << AHCI_PxFBS_ADO_SHIFT)
#define AHCI_PxFBS_DEV_SHIFT 8 /* device to issue / clear */
#define AHCI_PxFBS_DEV_MASK (0xFu << AHCI_PxFBS_DEV_SHIFT)
#define AHCI_PxFBS_SDE (1u << 2) /* single device error (RO) */
#define AHCI_PxFBS_DEC (1u << 1) /* device error clear (self-clearing) */
#define AHCI_PxFBS_EN (1u << 0) /* enable */

/* PxDEVSLP -- device sleep */
#define AHCI_DEVSLP_DM_SHIFT 25 /* DITO multiplier (RO) */
#define AHCI_DEVSLP_DM_MASK (0xFu << AHCI_DEVSLP_DM_SHIFT)
#define AHCI_DEVSLP_DITO_SHIFT 15 /* device sleep idle timeout, ms */
#define AHCI_DEVSLP_DITO_MASK (0x3FFu << AHCI_DEVSLP_DITO_SHIFT)
#define AHCI_DEVSLP_MDAT_SHIFT 10 /* minimum assertion time, ms */
#define AHCI_DEVSLP_MDAT_MASK (0x1Fu << AHCI_DEVSLP_MDAT_SHIFT)
#define AHCI_DEVSLP_DETO_SHIFT 2 /* exit timeout, ms */
#define AHCI_DEVSLP_DETO_MASK (0xFFu << AHCI_DEVSLP_DETO_SHIFT)
#define AHCI_DEVSLP_DSP (1u << 1) /* device sleep present (RO) */
#define AHCI_DEVSLP_ADSE (1u << 0) /* aggressive device sleep enable */
/* defaults when the device's IDENTIFY DEVSLP timing log gives none */
#define AHCI_DEVSLP_DEF_DETO 20
#define AHCI_DEVSLP_DEF_MDAT 10
#define AHCI_DEVSLP_DEF_DITO 1000

/* ==========================================================================
 * In-memory structures the HBA reads and writes (all little-endian)
 * ========================================================================== */

/* Command header (command list entry), 32 bytes.  DW0 bits: */
#define AHCI_CMDH_PRDTL_SHIFT 16 /* PRDT length (entries) */
#define AHCI_CMDH_PMP_SHIFT 12 /* port multiplier port */
#define AHCI_CMDH_PMP_MASK (0xFu << AHCI_CMDH_PMP_SHIFT)
#define AHCI_CMDH_C (1u << 10) /* clear busy upon R_OK */
#define AHCI_CMDH_B (1u << 9) /* BIST */
#define AHCI_CMDH_R (1u << 8) /* reset (SRST sequence) */
#define AHCI_CMDH_P (1u << 7) /* prefetchable */
#define AHCI_CMDH_W (1u << 6) /* write: data to the device */
#define AHCI_CMDH_A (1u << 5) /* ATAPI (ACMD valid) */
#define AHCI_CMDH_CFL_MASK 0x1Fu /* command FIS length, dwords, 2..16 */

struct ahci_cmd_header {
	uint32_t opts; /* PRDTL, PMP, C, B, R, P, W, A, CFL */
	volatile uint32_t prdbc; /* PRD byte count transferred (HBA) */
	uint32_t ctba; /* command table base, 128-byte aligned */
	uint32_t ctbau; /* upper 32 bits */
	uint32_t rsvd[4];
} __attribute__((packed));

/* Physical region descriptor, 16 bytes */
#define AHCI_PRD_I (1u << 31) /* interrupt on completion (DPS) */
#define AHCI_PRD_DBC_MASK 0x3FFFFFu /* byte count - 1, bit 0 always 1 */

struct ahci_prdt_entry {
	uint32_t dba; /* data base address, bit 0 reserved (even) */
	uint32_t dbau; /* upper 32 bits */
	uint32_t rsvd;
	uint32_t dbc; /* bit 31 I, 21:0 byte count - 1 */
} __attribute__((packed));

/* Command table: command FIS, ATAPI command, PRDT */
#define AHCI_CT_CFIS_OFF 0x00
#define AHCI_CT_CFIS_LEN 64
#define AHCI_CT_ACMD_OFF 0x40
#define AHCI_CT_ACMD_LEN 16
#define AHCI_CT_RSVD_OFF 0x50
#define AHCI_CT_RSVD_LEN 48
#define AHCI_CT_PRDT_OFF 0x80

struct ahci_cmd_table {
	uint8_t cfis[AHCI_CT_CFIS_LEN]; /* command FIS (H2D, 20 bytes used) */
	uint8_t acmd[AHCI_CT_ACMD_LEN]; /* ATAPI command (12 or 16 bytes) */
	uint8_t rsvd[AHCI_CT_RSVD_LEN];
	struct ahci_prdt_entry prdt[AHCI_MAX_PRD];
} __attribute__((packed));

/* Received FIS structure, 256 bytes (one per PMP port with FBS) */
#define AHCI_RXFIS_DSFIS 0x00 /* DMA setup FIS, 28 bytes */
#define AHCI_RXFIS_PSFIS 0x20 /* PIO setup FIS, 20 bytes */
#define AHCI_RXFIS_RFIS 0x40 /* D2H register FIS, 20 bytes */
#define AHCI_RXFIS_SDBFIS 0x58 /* set device bits FIS, 8 bytes */
#define AHCI_RXFIS_UFIS 0x60 /* unknown FIS, up to 64 bytes */
#define AHCI_RXFIS_RSVD 0xA0 /* to 0xFF */

struct ahci_rx_fis {
	struct fis_dma_setup dsfis; /* 0x00 */
	uint8_t pad0[4]; /* 0x1C */
	struct fis_pio_setup psfis; /* 0x20 */
	uint8_t pad1[12]; /* 0x34 */
	struct fis_reg_d2h rfis; /* 0x40 */
	uint8_t pad2[4]; /* 0x54 */
	struct fis_set_device_bits sdbfis; /* 0x58 */
	uint8_t ufis[64]; /* 0x60 */
	uint8_t rsvd[96]; /* 0xA0 */
} __attribute__((packed));

/* The FBS variant: one received-FIS structure per device behind a PMP */
struct ahci_rx_fis_fbs {
	struct ahci_rx_fis dev[16];
} __attribute__((packed));

/* ==========================================================================
 * Quirks (struct ahci_pci_id.quirks, hba->quirks)
 * ========================================================================== */

#define AHCI_Q_NO_MSI (1u << 0) /* MSI broken: INTx */
#define AHCI_Q_NO_MSIX (1u << 1)
#define AHCI_Q_NO_NCQ (1u << 2) /* NCQ broken */
#define AHCI_Q_NO_PMP (1u << 3) /* port multipliers broken */
#define AHCI_Q_NO_FBS (1u << 4)
#define AHCI_Q_YES_FBS (1u << 5) /* FBS works though CAP says not */
#define AHCI_Q_32BIT_ONLY (1u << 6) /* DMA below 4 GiB despite S64A */
#define AHCI_Q_FORCE_PI (1u << 7) /* PI unreliable: derive from NP */
#define AHCI_Q_NO_HOTPLUG (1u << 8)
#define AHCI_Q_NO_LPM (1u << 9) /* ALPM/HIPM broken */
#define AHCI_Q_NO_DEVSLP (1u << 10)
#define AHCI_Q_NO_SNTF (1u << 11)
#define AHCI_Q_NO_CLO (1u << 12) /* CLO broken: use COMRESET */
#define AHCI_Q_SRST_RETRY (1u << 13) /* SRST needs a retry (Marvell) */
#define AHCI_Q_SRST_NO_CLO (1u << 14)
#define AHCI_Q_MV_PATA (1u << 15) /* Marvell: hide the PATA port */
#define AHCI_Q_IGN_SERR_INTERNAL (1u << 16) /* ignore SERR.E (JMicron) */
#define AHCI_Q_IGN_IRQ_IF_ERR (1u << 17) /* ignore IFS (JMicron) */
#define AHCI_Q_INTEL_PCS (1u << 18) /* set port enables in PCI cfg PCS */
#define AHCI_Q_NO_SXS (1u << 19)
#define AHCI_Q_SINGLE_IRQ (1u << 20) /* one vector for all ports */
#define AHCI_Q_DELAY_ENGINE (1u << 21) /* extra delay after ST */
#define AHCI_Q_NO_SSS (1u << 22) /* ignore staggered spin-up */
#define AHCI_Q_VM (1u << 23) /* emulated: shorter link debounce */
#define AHCI_Q_ASMEDIA_PORTS (1u << 24) /* ASMedia: phantom ports in PI */
#define AHCI_Q_BROKEN_SUSPEND (1u << 25)
#define AHCI_Q_NO_ALPM_ON_HOTPLUG (1u << 26)
#define AHCI_Q_AMD_32BIT (1u << 27) /* SB6xx: 32-bit DMA only */
#define AHCI_Q_RESET_BEFORE_AE (1u << 28) /* HR needs AE set first */
#define AHCI_Q_NOT_AHCI (1u << 31) /* table entry that refuses a match */

/* Intel: port control and status register in PCI config space */
#define AHCI_INTEL_PCI_PCS 0x92 /* 16-bit on older, 32-bit on PCH */
#define AHCI_INTEL_PCI_MAP 0x90

#define AHCI_PCI_ANY 0xFFFFu
#define AHCI_PCI_CLASS_STORAGE 0x01
#define AHCI_PCI_SUBCLASS_SATA 0x06
#define AHCI_PCI_SUBCLASS_RAID 0x04
#define AHCI_PCI_PROGIF_AHCI 0x01
#define AHCI_PCI_ABAR 5 /* BAR index of ABAR */

/* A known controller.  Matched by vendor/device (AHCI_PCI_ANY wildcard),
 * optionally restricted to a class code (0 = any). */
struct ahci_pci_id {
	uint16_t vendor, device;
	uint32_t class_code; /* class<<16 | subclass<<8 | prog-if, 0 any */
	uint32_t quirks; /* AHCI_Q_* */
	const char *name; /* "Intel Sunrise Point-H AHCI" */
};

/* ==========================================================================
 * Driver state enums
 * ========================================================================== */

enum ahci_irq_mode {
	AHCI_IRQ_POLL = 0, /* no interrupt: kahci / blk_wait poll */
	AHCI_IRQ_INTX, /* legacy INTx through the I/O APIC (_PRT) */
	AHCI_IRQ_MSI, /* single-message MSI */
	AHCI_IRQ_MSIX, /* MSI-X, entry 0 */
};

enum ahci_hba_state {
	AHCI_HBA_PROBING = 0, /* ahci_init() in progress */
	AHCI_HBA_POLLED, /* running, polled (boot) */
	AHCI_HBA_RUNNING, /* running, interrupts and kahci */
	AHCI_HBA_RESETTING, /* controller recovery in progress */
	AHCI_HBA_SUSPENDED,
	AHCI_HBA_DEAD, /* recovery failed: all ports failed */
};

enum ahci_port_state {
	AHCI_PORT_UNUSED = 0, /* not implemented / not initialised */
	AHCI_PORT_IDLE, /* initialised, no device */
	AHCI_PORT_RUNNING, /* engine started, device usable */
	AHCI_PORT_FROZEN, /* error seen: no new commands until EH */
	AHCI_PORT_EH, /* EH running */
	AHCI_PORT_DISABLED, /* EH gave up / PHY offline */
	AHCI_PORT_SUSPENDED,
};

enum ahci_link_state {
	AHCI_LINK_OFFLINE = 0, /* DET != 3 */
	AHCI_LINK_ONLINE, /* PHY established */
	AHCI_LINK_RESETTING,
	AHCI_LINK_DISABLED, /* SCTL.DET = 4 */
};

/* Link power management policies (sysfs link_power_management_policy:
 * "max_performance", "medium_power", "med_power_with_dipm", "min_power",
 * "keep_firmware_settings") */
enum ahci_lpm_policy {
	AHCI_LPM_KEEP_FIRMWARE = 0, /* touch nothing */
	AHCI_LPM_MAX_PERFORMANCE, /* IPM transitions disabled */
	AHCI_LPM_MEDIUM_POWER, /* HIPM to partial */
	AHCI_LPM_MED_POWER_WITH_DIPM, /* HIPM slumber + DIPM */
	AHCI_LPM_MIN_POWER, /* + APST, DevSleep where present */
};

/* Error handling actions (port->eh_pending, link->eh_pending), set by the
 * interrupt handler, the transport (timeouts included) and sysfs; consumed
 * by ahci_eh_run() in the kahci thread. */
#define AHCI_EH_TIMEOUT (1u << 0) /* a command passed its deadline */
#define AHCI_EH_DEV_ERROR (1u << 1) /* TFES: device reported an error */
#define AHCI_EH_NCQ_ERROR (1u << 2) /* error with NCQ outstanding */
#define AHCI_EH_HOST_ERROR (1u << 3) /* HBFS / HBDS */
#define AHCI_EH_IF_FATAL (1u << 4) /* IFS */
#define AHCI_EH_IF_NONFATAL (1u << 5) /* INFS */
#define AHCI_EH_OVERFLOW (1u << 6) /* OFS */
#define AHCI_EH_UNKNOWN_FIS (1u << 7) /* UFS */
#define AHCI_EH_IPMS (1u << 8) /* incorrect port multiplier */
#define AHCI_EH_HOTPLUG (1u << 9) /* PCS / PRCS / CPDS / DMPS */
#define AHCI_EH_SNTF (1u << 10) /* PMP / ATAPI asynchronous notify */
#define AHCI_EH_RESET_SOFT (1u << 11) /* requested SRST */
#define AHCI_EH_RESET_HARD (1u << 12) /* requested COMRESET */
#define AHCI_EH_REVALIDATE (1u << 13)
#define AHCI_EH_LPM (1u << 14) /* apply port->lpm_policy */
#define AHCI_EH_HBA_RESET (1u << 15) /* escalate to the controller */
#define AHCI_EH_DETACH (1u << 16) /* drop the device(s) */
#define AHCI_EH_SUSPEND (1u << 17)
#define AHCI_EH_RESUME (1u << 18)
/* bit 19: unused (ATAPI media events are the drive's own work) */
#define AHCI_EH_FAULT (1u << 20) /* injected fault (debug) */
#define AHCI_EH_REISSUE (1u << 21) /* EH holds commands to re-issue */
/* Error handling without a failed command of its own: a deferred
 * non-queued command timed out waiting for its turn, an error interrupt
 * named nothing EH could act on, or a device error answered one of EH's
 * own commands during a run.  The run looks at the port once; nothing is
 * frozen for it. */
#define AHCI_EH_PORT (1u << 22)
/* A PhyRdy change shortly after a link power management policy change
 * is spurious (ms). */
#define AHCI_TMOUT_SPURIOUS_PHY_MS 10000u
#define AHCI_EH_ERRORS                                                  \
	(AHCI_EH_TIMEOUT | AHCI_EH_DEV_ERROR | AHCI_EH_NCQ_ERROR |      \
	 AHCI_EH_HOST_ERROR | AHCI_EH_IF_FATAL | AHCI_EH_IF_NONFATAL |  \
	 AHCI_EH_OVERFLOW | AHCI_EH_UNKNOWN_FIS | AHCI_EH_IPMS |        \
	 AHCI_EH_FAULT)
/* Actions scheduled through ahci_eh_schedule_locked() that freeze the
 * port (PxIE masked) at once.  The interrupt handler decides the freeze
 * of an error event by the event itself (AHCI_PxIS_FREEZE): a device
 * error (TFES) and a host bus data error (HBDS) abort the commands in
 * flight without masking the port's interrupts.  A command timeout does
 * not freeze when it is noticed: the other commands in flight finish
 * first, and the EH run freezes the port when it starts.  While any
 * action is pending the port admits nothing but error handling's own
 * commands, and the EH run starts once every command still in flight
 * has failed (ahci_eh_drained_locked). */
#define AHCI_EH_FREEZING                                                 \
	(AHCI_EH_HOST_ERROR | AHCI_EH_IF_FATAL | AHCI_EH_OVERFLOW |      \
	 AHCI_EH_UNKNOWN_FIS | AHCI_EH_IPMS | AHCI_EH_RESET_SOFT |       \
	 AHCI_EH_RESET_HARD | AHCI_EH_HBA_RESET | AHCI_EH_DETACH)

/* The recovery ladder (ahci_eh_recover), mildest first */
enum ahci_eh_level {
	AHCI_EH_LVL_NONE = 0,
	AHCI_EH_LVL_RETRY, /* re-issue the failed commands */
	AHCI_EH_LVL_ABORT, /* stop engine, clear CI, restart */
	AHCI_EH_LVL_CLO, /* + command list override if BSY/DRQ stuck */
	AHCI_EH_LVL_SRST, /* software reset */
	AHCI_EH_LVL_COMRESET, /* hard link reset */
	AHCI_EH_LVL_HBA_RESET, /* controller reset + full re-init */
	AHCI_EH_LVL_GIVE_UP, /* disable the port/device */
};

/* kahci work bits (hba->kahci_work), set from any context with
 * ahci_kahci_kick() */
#define AHCI_WORK_EH (1u << 0) /* some port has eh_pending */
#define AHCI_WORK_POLL (1u << 1) /* polled mode: run ahci_hba_poll */
#define AHCI_WORK_HBA_RESET (1u << 3)
#define AHCI_WORK_LPM (1u << 5)
#define AHCI_WORK_GONE (1u << 6) /* the function was found removed */
#define AHCI_WORK_STOP (1u << 31) /* thread exits (shutdown) */

/* Per-slot command state */
enum ahci_cmd_state {
	AHCI_CMD_FREE = 0,
	AHCI_CMD_ALLOCATED, /* slot taken, being built */
	AHCI_CMD_ISSUED, /* CI (and SACT) set */
	AHCI_CMD_COMPLETING, /* collected by the IRQ, being completed */
	AHCI_CMD_TIMEDOUT, /* deadline passed, waiting for EH */
	AHCI_CMD_EH, /* owned by error handling */
};

/* ==========================================================================
 * Driver structures
 * ========================================================================== */

/* A physically contiguous DMA area. */
struct ahci_dma_area {
	void *virt; /* kernel virtual address (direct map) */
	uint64_t phys; /* CPU physical address */
	uint64_t bus; /* what the HBA is given (IOMMU hook) */
	size_t size; /* bytes */
	uint32_t pages; /* 4 KiB pages allocated */
	uint32_t flags; /* AHCI_DMA_F_* */
};
#define AHCI_DMA_F_BELOW_4G (1u << 0) /* placed below 4 GiB */
#define AHCI_DMA_F_UNCACHED (1u << 1) /* mapped UC (non-coherent quirk) */

/* Bounce state of one command on a 32-bit HBA whose buffer is above 4 GiB
 * (or not 2-byte aligned): the data goes through `area'. */
struct ahci_bounce {
	struct ahci_dma_area area; /* size 0 = not bounced */
	const struct blk_sg_entry *sg; /* the original list, copied back */
	uint32_t nsg;
	uint32_t nbytes;
};

/* One command slot of a port. */
struct ahci_cmd {
	uint32_t slot; /* 0 .. nr_slots-1; NCQ tag == slot */
	volatile enum ahci_cmd_state state; /* port->lock */
	struct ata_queued_cmd *qc; /* NULL when free */
	struct ahci_cmd_header *hdr; /* &port->cmd_list[slot] */
	struct ahci_cmd_table *table; /* this slot's command table */
	uint64_t table_bus; /* its bus address */
	uint8_t is_ncq;
	uint8_t pmp; /* port multiplier port */
	uint8_t internal; /* EH / reset command */
	uint8_t pad;
	uint32_t n_prd; /* PRDT entries used */
	uint32_t nbytes; /* bytes mapped */
	uint64_t start_ns; /* issue time (trace latency) */
	uint32_t timeout_ms; /* the command's timeout, for reports */
	uint32_t generation; /* bumped at every allocation: catches a
			      * completion for a slot reused since */
	struct ahci_bounce bounce;
	/* A failed command (not error handling's own) whose slot the HBA
	 * may still be doing DMA for: held here, its slot a zombie, and
	 * completed (-ECANCELED, requeued) only once the slot is confirmed
	 * free -- its buffer is not given back before.  port->lock. */
	struct ata_queued_cmd *held_qc;
};

/* Link status decoded from SStatus. */
struct ahci_link_status {
	uint8_t det; /* AHCI_SSTS_DET_* */
	uint8_t spd; /* AHCI_SSTS_SPD_* */
	uint8_t ipm; /* AHCI_SSTS_IPM_* */
	uint8_t online; /* det == PHY established */
};

/* A SATA link: the port's own (host link), or one fan-out link of a port
 * multiplier.  A device hangs off a link. */
struct ahci_link {
	struct ahci_port *port;
	int pmp; /* 0 for the host link / PMP fan-out number */
	int is_host; /* the port's own link */
	enum ahci_link_state state;
	char name[16]; /* "link1", "link1.3" */
	/* cached SCRs (host link: PxSSTS/PxSCTL/PxSERR; PMP link: PSCR) */
	uint32_t sstatus, scontrol, serror;
	uint8_t speed; /* current, AHCI_SSTS_SPD_* */
	uint8_t speed_limit; /* 0 none, else limit (EH lowers it) */
	uint8_t pad[2];
	enum ata_class class;
	uint32_t sig; /* signature found at reset */
	struct ata_device *dev; /* NULL when no device */
	volatile uint32_t eh_pending; /* AHCI_EH_* for this link only */
	enum ahci_lpm_policy lpm_policy;
	/* A hot-plug event of this link is being handled: its resets
	 * debounce with the hot-plug timing.  eh_mutex held. */
	uint8_t eh_hotplugged;
	/* A fan-out link that failed recovery AHCI_EH_PMP_LINK_TRIES times:
	 * not reset nor probed any more until it reports a PhyRdy change
	 * (SError.N).  eh_mutex held. */
	uint8_t eh_link_disabled;
	/* Revalidation tries left of the link's device in this EH run
	 * (AHCI_EH_DEV_TRIES at its start).  eh_mutex held. */
	uint8_t eh_dev_tries;
	/* The last change of the link power management policy (applied by
	 * EH): a PhyRdy change shortly after it is not an event.  Written
	 * under port->lock. */
	uint8_t lpm_changed;
	uint64_t last_lpm_change_ns;
	/* counters */
	uint32_t n_resets;
	uint32_t n_link_errors;
	uint32_t n_speed_downs;
	uint64_t last_reset_ns;
	/* speed step-down bookkeeping (ahci_port.c): link errors counted
	 * since spd_err_window_ns; AHCI_SPEED_DOWN_ERRS of them inside
	 * AHCI_SPEED_DOWN_WINDOW_MS lower the speed at the next reset */
	uint32_t spd_err_count;
	uint32_t sysfs_added; /* ahci_sysfs_add_link() done (PMP links) */
	uint64_t spd_err_window_ns;
};
#define AHCI_SPEED_DOWN_ERRS 3
#define AHCI_SPEED_DOWN_WINDOW_MS (10u * 60u * 1000u)

/* Per-port statistics (diagnostics, sysfs). */
struct ahci_port_stats {
	uint64_t irqs; /* PxIS != 0 handled */
	uint64_t cmds_issued;
	uint64_t cmds_completed;
	uint64_t ncq_issued;
	uint64_t bytes_read, bytes_written;
	uint32_t max_ncq_depth_seen;
	uint32_t errors; /* any AHCI_PxIS_ERROR */
	uint32_t tfes, hbfs, hbds, ifs, infs, ofs, ipms, ufs;
	uint32_t timeouts;
	uint32_t ncq_errors;
	uint32_t retries;
	uint32_t aborts;
	uint32_t clo;
	uint32_t srst, comreset, hba_resets;
	uint32_t eh_runs;
	uint32_t hotplug_add, hotplug_remove;
	uint32_t serr_crc, serr_disparity, serr_decode, serr_handshake;
	uint32_t serr_phyrdy, serr_comwake, serr_internal, serr_protocol;
	uint32_t bounces; /* commands that needed a bounce buffer */
	uint32_t held_cmds; /* failed commands held for a busy slot */
	uint32_t deferred; /* commands refused: cannot run next to others */
	uint32_t spurious; /* completion IRQ with nothing finished */
};

/* One entry of the command trace ring */
#define AHCI_TRACE_ISSUE 1
#define AHCI_TRACE_COMPLETE 2
#define AHCI_TRACE_ERROR 3
#define AHCI_TRACE_TIMEOUT 4
#define AHCI_TRACE_ABORT 5
#define AHCI_TRACE_RESET 6
#define AHCI_TRACE_IRQ 7
#define AHCI_TRACE_EH 8
struct ahci_trace_entry {
	uint64_t ts_ns;
	uint64_t lba;
	uint32_t count; /* sectors */
	uint32_t latency_us; /* COMPLETE/ERROR: since issue */
	uint32_t aux; /* IRQ: PxIS; EH: actions; RESET: level */
	int16_t result; /* -errno */
	uint8_t event; /* AHCI_TRACE_* */
	uint8_t command; /* ATA opcode */
	uint8_t slot;
	uint8_t pmp;
	uint8_t status; /* ATA status */
	uint8_t error; /* ATA error */
};
struct ahci_trace_ring {
	spinlock_t lock; /* leaf */
	uint32_t head; /* next write */
	uint32_t pad;
	uint64_t total; /* entries ever written */
	struct ahci_trace_entry e[AHCI_TRACE_ENTRIES];
};

/* Fault injection knobs (debug sysfs: fault_inject).  Checked by the issue
 * path through ahci_fault_check(). */
#define AHCI_FAULT_NONE 0
#define AHCI_FAULT_TIMEOUT 1 /* do not set CI: let the timeout expire */
#define AHCI_FAULT_DEV_ERROR 2 /* complete with ERR + error byte */
#define AHCI_FAULT_HOST_ERROR 3 /* simulate HBDS */
#define AHCI_FAULT_LINK_DOWN 4 /* simulate PhyRdy loss */
struct ahci_fault_inject {
	volatile uint32_t remaining; /* commands still to fail, 0 off */
	volatile uint32_t every_n; /* fail every n-th command, 0 off */
	uint32_t kind; /* AHCI_FAULT_* */
	uint8_t error; /* ATA error byte for DEV_ERROR */
	uint8_t only_ncq; /* 1: only NCQ commands */
	uint8_t only_write; /* 1: only writes */
	uint8_t pad;
	uint32_t counter; /* commands seen */
	uint32_t injected; /* faults injected */
};

/* One SATA port. */
struct ahci_port {
	struct ahci_hba *hba;
	int index; /* port number on the HBA (bit in PI) */
	int port_no; /* system-wide ATA port number, 1-based */
	char name[16]; /* "ata1" */
	volatile uint8_t *mmio; /* hba->mmio + 0x100 + index * 0x80 */
	enum ahci_port_state state; /* port->lock */
	uint32_t flags; /* AHCI_PORT_F_* */

	/* locks */
	spinlock_t lock; /* see the locking rules */
	struct ata_mutex eh_mutex;

	/* DMA areas */
	struct ahci_dma_area cmd_list_area; /* 1 KiB command list */
	struct ahci_dma_area rx_fis_area; /* 256 B (4 KiB with FBS) */
	struct ahci_dma_area cmd_table_area; /* nr_slots x 4 KiB tables */
	struct ahci_cmd_header *cmd_list; /* == cmd_list_area.virt */
	void *rx_fis; /* struct ahci_rx_fis or _fbs */

	/* command slots (port->lock) */
	uint32_t nr_slots; /* CAP.NCS + 1 */
	uint32_t slot_map; /* allocated slots */
	uint32_t active; /* issued through CI and not completed */
	uint32_t ncq_active; /* of those, issued through SACT */
	uint32_t internal_slot; /* slot of EH's own commands: 0 (not reserved) */
	uint32_t ncq_depth; /* tags handed out at most (device/host) */
	struct ahci_cmd cmds[AHCI_MAX_SLOTS];
	/* The one non-queued command (carrying a block request) accepted
	 * while queued commands still run: issued by deferred_work (kblockd)
	 * once the queue drained; while it waits every other command is
	 * refused (-EBUSY), so it cannot starve.  port->lock. */
	struct ata_queued_cmd *deferred_qc;
	struct blk_work deferred_work;

	/* links and devices */
	struct ahci_link host_link;
	int pmp_attached; /* a port multiplier is on this port */
	uint32_t nr_pmp_links; /* fan-out ports (GSCR 2) */
	struct ahci_link pmp_links[AHCI_PMP_MAX_LINKS];
	uint32_t pmp_gscr_prod_id, pmp_gscr_rev, pmp_gscr_feat;
	/* The multiplier's features enabled (GSCR 96) as last written, and
	 * whether it can notify (GSCR 64 NOTIFY): notification is turned
	 * off while recovery runs and on again at its end. */
	uint32_t pmp_gscr_feat_en;
	uint8_t pmp_an;

	/* FIS-based switching */
	int fbs_capable; /* CAP.FBSS && PxCMD.FBSCP */
	int fbs_enabled;
	int fbs_last_dev; /* DEV field last programmed */

	/* link power management, hot-plug */
	enum ahci_lpm_policy lpm_policy;
	int hotplug_capable; /* HPCP | ESP | MPSP | CPD */
	uint64_t hotplug_ns; /* last hot-plug event (debounce) */
	int devslp_capable; /* CAP2.SDS && PxDEVSLP.DSP */

	/* error handling (port->lock for the latched values) */
	volatile uint32_t eh_pending; /* AHCI_EH_* */
	uint32_t eh_irq_stat; /* PxIS latched by the handler */
	/* An error latched since the last EH run was more than a quiet
	 * command's device error (see ahci_eh_report_exception). */
	uint8_t eh_noisy;
	uint32_t eh_serror; /* PxSERR latched */
	uint32_t eh_tfd; /* PxTFD latched */
	uint32_t eh_failed_slots; /* slots active when the error hit */
	/* The last reset of error handling, and whether the current run
	 * reset already (a further one waits AHCI_EH_RESET_COOL_DOWN_MS). */
	uint64_t eh_last_reset_ns;
	uint8_t eh_run_did_reset;
	/* Recovery tries left of each fan-out link in this EH run
	 * (AHCI_EH_PMP_LINK_TRIES at its start and when a multiplier comes
	 * online).  eh_mutex held. */
	uint8_t pmp_link_tries[AHCI_PMP_MAX_LINKS];
	/* A hot-plug event is being handled: its resets debounce with the
	 * longer hot-plug timing.  eh_mutex held. */
	uint8_t eh_hotplugged;
	/* Frozen by an error event or a timeout (PxIE masked): commands of
	 * error handling's own context are refused until the port's
	 * interrupts are enabled again (a reset, a callee's unmask, the
	 * thaw).  Under port->lock. */
	uint8_t eh_frozen;
	/* 1 + the PMP link whose single-device error the interrupt handler
	 * cleared (PxFBS.DEC) and aborted, 0 none; taken by the EH run. */
	uint8_t eh_fbs_link;
	enum ahci_eh_level eh_level; /* rung reached by the current run */
	uint32_t eh_consecutive; /* runs without a clean command between */
	struct wait_queue_head eh_wait; /* waiters for EH to finish */
	volatile uint32_t eh_seq; /* bumped when an EH run finishes */

	/* interrupts */
	uint32_t irq_mask; /* PxIE value in use */
	/* PxIE was last written with the port's mask, not 0 (port->lock):
	 * the interrupt handler owns PxIS and IS, nothing to thaw.  Zero --
	 * the initial value -- means masked or not yet started. */
	int irq_enabled;
	int polled; /* completions by polling (boot / no IRQ) */

	/* saved registers (suspend, HBA reset) */
	uint32_t saved_cmd, saved_ie, saved_sctl, saved_fbs, saved_devslp;

	/* diagnostics */
	struct ahci_port_stats stats;
	struct ahci_trace_ring *trace; /* NULL if allocation failed */
	struct ahci_fault_inject fault;
	char sysfs_base[96]; /* "class/ata_port/ata1" relative to /sys */

	/* Command engine and error handling private state (ahci_cmd.c,
	 * ahci_eh.c; port->lock unless noted).  Zero is the correct initial
	 * value of every field, so a zeroed port allocation is enough. */
	uint32_t tag_hint; /* next-fit start of the NCQ tag search */
	uint32_t zombie_slots; /* qc completed by software, slot still
				* owned by the HBA until CI/SACT clear */
	uint32_t fault_slots; /* slots carrying an injected fault */
	uint32_t held_slots; /* zombie slots holding a command (held_qc) */
	volatile int eh_running; /* an EH run is in progress */
	/* commands failed under port->lock, completed by the caller once it
	 * dropped the lock (the interrupt handler: right away, under it):
	 * linked through qc->xport_next */
	struct ata_queued_cmd *cpl_head, *cpl_tail;
	/* ATA_PROT_SRST requests waiting for the EH software reset */
	struct ata_queued_cmd *srst_head, *srst_tail;
	/* commands EH holds across a controller reset (eh_mutex) */
	struct ata_queued_cmd *eh_hold_head, *eh_hold_tail;
	int eh_last_result; /* 0, or -errno of the last EH run's recovery */
	uint32_t ncq_log_failures; /* READ LOG EXT 10h that did not help */
	uint32_t ncq_errors_since_ok; /* NCQ errors without a clean run */
	uint64_t ncq_depth_sum; /* sum of NCQ depths at issue (average) */
	uint64_t ncq_depth_samples;
	uint32_t ncq_depth_hist[AHCI_MAX_SLOTS + 1]; /* depth at issue */
	uint32_t ncq_full_busy; /* NCQ issue refused: queue full */
	uint32_t pmp_switch_busy; /* refused: other PMP link active (CBS) */
	uint32_t eh_ladder_hist[AHCI_EH_LVL_GIVE_UP + 1]; /* rungs climbed */
	uint32_t eh_retried; /* commands re-issued by EH */
	uint32_t eh_failed; /* commands failed by EH */
	uint32_t eh_innocent; /* commands aborted as innocent victims */
};

/* struct ahci_port.flags */
#define AHCI_PORT_F_IMPLEMENTED (1u << 0)
#define AHCI_PORT_F_EXTERNAL (1u << 1) /* eSATA (PxCMD.ESP) */
#define AHCI_PORT_F_HOTPLUG (1u << 2) /* PxCMD.HPCP */
#define AHCI_PORT_F_CPD (1u << 3) /* cold presence detection */
#define AHCI_PORT_F_MPS (1u << 4) /* mechanical presence switch */
#define AHCI_PORT_F_ATAPI (1u << 5) /* PxCMD.ATAPI set */
#define AHCI_PORT_F_SPUN_UP (1u << 6) /* SUD issued */
#define AHCI_PORT_F_NO_NCQ (1u << 7) /* NCQ disabled after errors */

/* Decoded host capabilities */
struct ahci_caps {
	uint32_t n_ports; /* CAP.NP + 1 */
	uint32_t n_slots; /* CAP.NCS + 1 */
	uint32_t iss; /* max speed, AHCI_ISS_* */
	uint8_t s64a, sncq, ssntf, smps, sss, salp, sal, sclo;
	uint8_t sam, spm, fbss, pmd, ssc, psc, cccs, ems, sxs;
	uint8_t boh, nvmp, apst, sds, sadm, deso;
	uint8_t pad[2];
};

/* Per-HBA statistics */
struct ahci_hba_stats {
	uint64_t irqs; /* handler calls that found IS != 0 */
	uint64_t spurious_irqs; /* IS == 0 (shared INTx) */
	uint64_t polls; /* ahci_hba_poll() runs */
	uint32_t hba_resets;
	uint32_t host_errors;
	uint32_t kahci_wakeups;
};

/* One AHCI controller. */
struct ahci_hba {
	struct ahci_hba *next; /* g_ahci_hbas list */
	int index; /* 0.. (kahci<index>) */
	char name[32]; /* "ahci0" */
	const pci_device_t *pci;
	const struct ahci_pci_id *id;
	uint32_t quirks; /* AHCI_Q_* (table + runtime) */
	enum ahci_hba_state state; /* hba->lock */

	/* ABAR */
	uint64_t mmio_phys;
	uint64_t mmio_size;
	volatile uint8_t *mmio;

	spinlock_t lock; /* see the locking rules */

	/* capabilities: raw (as read after reset) and decoded */
	uint32_t cap, cap2, vs, pi;
	uint32_t saved_cap, saved_pi; /* firmware values: restored after HR
				       * (CAP/PI are write-once on some
				       * chipsets and HR clears them) */
	struct ahci_caps caps;
	uint32_t em_loc, em_ctl;
	int dma64; /* 64-bit DMA usable (S64A and no quirk) */

	/* ports */
	uint32_t port_map; /* implemented ports actually used */
	struct ahci_port *ports[AHCI_MAX_PORTS]; /* NULL if absent */
	int first_port_no; /* port_no of the lowest port */

	/* interrupts */
	enum ahci_irq_mode irq_mode;
	int irq_vector; /* MSI/MSI-X/INTx vector, -1 none */
	int irq_gsi; /* INTx GSI, -1 none */
	int irq_active_low, irq_level;
	hrtimer_t poll_timer; /* polled mode: kicks kahci */

	/* the worker thread */
	task_t *kahci;
	uint8_t *kahci_stack;
	struct wait_queue_head kahci_wq;
	volatile uint32_t kahci_work; /* AHCI_WORK_* */
	volatile int kahci_running;
	/* The interrupt handler found the function removed (registers and
	 * configuration space read all ones) and marked the controller
	 * dead: kahci gives its ports up.  hba->lock. */
	int gone_pending;
	/* The controller can do no more DMA: removed (configuration space
	 * reads all ones) or its bus mastering switched off.  Commands whose
	 * slots it still held may be completed then. */
	volatile int dma_stopped;

	/* PCI config state (save/restore around HBA reset and suspend) */
	uint32_t pci_saved[16]; /* config header dwords 0..15 */
	uint8_t msi_cap, msix_cap; /* capability offsets, 0 none */
	uint16_t pci_saved_msi_ctl;
	uint32_t pci_saved_msi[4]; /* address lo/hi, data */
	uint32_t saved_ghc, saved_ccc;

	uint32_t generation; /* bumped by every HBA reset */
	struct ahci_hba_stats stats;
	char sysfs_base[96];
};

/* ==========================================================================
 * Register access, barriers, delays (inline, used by every file)
 * ========================================================================== */

static inline uint32_t ahci_readl(const volatile uint8_t *base, uint32_t off)
{
	return *(const volatile uint32_t *)(base + off);
}
static inline void ahci_writel(volatile uint8_t *base, uint32_t off,
			       uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}
static inline uint32_t ahci_hba_readl(const struct ahci_hba *hba,
				      uint32_t off)
{
	return ahci_readl(hba->mmio, off);
}
static inline void ahci_hba_writel(struct ahci_hba *hba, uint32_t off,
				   uint32_t v)
{
	ahci_writel(hba->mmio, off, v);
}
static inline uint32_t ahci_port_readl(const struct ahci_port *port,
				       uint32_t off)
{
	return ahci_readl(port->mmio, off);
}
static inline void ahci_port_writel(struct ahci_port *port, uint32_t off,
				    uint32_t v)
{
	ahci_writel(port->mmio, off, v);
}
/* Post preceding writes to the port (a read of PxCMD). */
static inline void ahci_port_flush(struct ahci_port *port)
{
	(void)ahci_port_readl(port, AHCI_PxCMD);
}

/* DMA ownership barriers.  The command header, table and PRDT are written
 * with ordinary stores to write-back memory; the HBA reads them after the
 * CI write (uncached MMIO).  ahci_wmb() before handing ownership to the
 * device (the CI/SACT write); ahci_rmb() after seeing the completion
 * (PxCI clear) before reading the received FIS, PRDBC or the data. */
static inline void ahci_wmb(void)
{
	__asm__ volatile("sfence" ::: "memory");
}
static inline void ahci_rmb(void)
{
	__asm__ volatile("lfence" ::: "memory");
}
static inline void ahci_mb(void)
{
	__asm__ volatile("mfence" ::: "memory");
}
/* Write back + invalidate a range (the non-coherent platform path; x86
 * DMA is cache-coherent, so this runs only when a quirk asks). */
static inline void ahci_clflush_range(const void *p, size_t len)
{
	uint64_t a = (uint64_t)p & ~63ULL;
	uint64_t end = (uint64_t)p + len;

	ahci_mb();
	for (; a < end; a += 64)
		__asm__ volatile("clflush (%0)" ::"r"(a) : "memory");
	ahci_mb();
}

/* Delays.  ahci_udelay/mdelay busy-wait (any context, also before the
 * timers are calibrated -- lapic_delay_us falls back to the PIT).
 * ahci_msleep sleeps when it may and busy-waits otherwise. */
static inline void ahci_udelay(uint32_t us)
{
	if (us)
		lapic_delay_us(us);
}
static inline void ahci_mdelay(uint32_t ms)
{
	while (ms) {
		uint32_t chunk = ms > 100 ? 100 : ms;

		lapic_delay_ms(chunk);
		ms -= chunk;
	}
}
static inline void ahci_msleep(uint32_t ms)
{
	if (ata_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0)
			; /* a signal cut it short: sleep the rest */
	} else {
		ahci_mdelay(ms);
	}
}

/* Wait until (readl(base + off) & mask) == val, polling every `step_us'
 * (sleeping between polls when the step is >= 1 ms and sleeping is
 * allowed).  Returns 0, or -ETIMEDOUT with the last value in *last (if
 * non-NULL).  The elapsed time is counted from the delays (works before
 * clock calibration; never shorter than asked).  Any context when
 * step_us < 1000. */
static inline int ahci_wait_reg(const volatile uint8_t *base, uint32_t off,
				uint32_t mask, uint32_t val,
				uint32_t timeout_ms, uint32_t step_us,
				uint32_t *last)
{
	uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;
	uint32_t v;

	if (step_us == 0)
		step_us = 10;
	for (;;) {
		v = ahci_readl(base, off);
		if ((v & mask) == val) {
			if (last)
				*last = v;
			return 0;
		}
		if (waited_us >= limit_us) {
			if (last)
				*last = v;
			return -ETIMEDOUT;
		}
		if (step_us >= 1000)
			ahci_msleep(step_us / 1000);
		else
			ahci_udelay(step_us);
		waited_us += step_us;
	}
}

/* Little helpers */
static inline struct ahci_port *ahci_link_port(const struct ahci_link *link)
{
	return link->port;
}
static inline struct ahci_link *ahci_dev_link(const struct ata_device *dev)
{
	return (struct ahci_link *)dev->xport_priv;
}
static inline struct ahci_port *ahci_dev_port(const struct ata_device *dev)
{
	return ahci_dev_link(dev)->port;
}
static inline const char *ahci_speed_str(uint32_t spd)
{
	switch (spd) {
	case AHCI_SSTS_SPD_GEN1:
		return "1.5 Gbps";
	case AHCI_SSTS_SPD_GEN2:
		return "3.0 Gbps";
	case AHCI_SSTS_SPD_GEN3:
		return "6.0 Gbps";
	default:
		return "<unknown>";
	}
}
static inline void ahci_decode_ssts(uint32_t ssts, struct ahci_link_status *s)
{
	s->det = (uint8_t)AHCI_SSTS_DET(ssts);
	s->spd = (uint8_t)AHCI_SSTS_SPD(ssts);
	s->ipm = (uint8_t)AHCI_SSTS_IPM(ssts);
	s->online = s->det == AHCI_SSTS_DET_PHY;
}

/* ==========================================================================
 * Global state (ahci_hba.c)
 * ========================================================================== */

extern struct ahci_hba *g_ahci_hbas; /* list head (g_ahci_list_lock) */
extern spinlock_t g_ahci_list_lock; /* leaf: list walk only */
extern int g_ahci_nr_hbas;
extern int g_ahci_next_port_no; /* next ATA port number to hand out */

/* ==========================================================================
 * ahci_pci.c (A1)
 * ========================================================================== */

/* The known-controller table (terminated by a zero vendor). */
extern const struct ahci_pci_id ahci_pci_ids[];
/* The table entry for a PCI function, or a generic entry for an unknown
 * class 01/06/01 controller; NULL when it is not an AHCI function (or a
 * NOT_AHCI entry refuses it).  Pure. */
const struct ahci_pci_id *ahci_pci_match(const pci_device_t *pci);
/* Map ABAR (BAR5, uncached), enable memory decoding and bus mastering,
 * apply PCI-level quirks (Intel PCS port enables), record MSI/MSI-X
 * capability offsets.  Boot context.  0, -ENODEV (no BAR5), -ENOMEM. */
int ahci_pci_enable(struct ahci_hba *hba);
/* Does the function still answer configuration cycles (vendor ID not
 * 0xFFFF)?  Any context. */
int ahci_pci_alive(const struct ahci_hba *hba);
/* Switch the function's bus mastering off (a controller given up): no
 * more DMA.  Sets hba->dma_stopped when the command register confirms it
 * (or the function is gone).  May sleep: no. */
void ahci_pci_stop_dma(struct ahci_hba *hba);
/* Choose and set up the interrupt: MSI-X entry 0 -> MSI -> INTx (GSI from
 * _PRT with bridge swizzle, level/active-low) -> AHCI_IRQ_POLL with the
 * 1 ms poll timer.  Registers ahci_irq_handler.  INTx disable bit set for
 * MSI/MSI-X, cleared for INTx.  After sti.  0 (polling is a success). */
int ahci_pci_setup_irq(struct ahci_hba *hba);
void ahci_pci_free_irq(struct ahci_hba *hba);
/* The INTx GSI of the function (routing through _PRT, bridges swizzled).
 * -ENOENT when the firmware gives none. */
int ahci_pci_intx_gsi(const pci_device_t *pci, int *active_low, int *level);
/* Save / restore the config header and the MSI(-X) setup (HBA reset,
 * suspend/resume).  Any context. */
void ahci_pci_save_state(struct ahci_hba *hba);
void ahci_pci_restore_state(struct ahci_hba *hba);
/* PCI power state of the function through its power management capability
 * (0 = D0 .. 3 = D3hot), with the delays the bus power management rules
 * require.  Returns the previous state, -ENODEV without the capability.
 * Boot/process context (a D3hot transition busy-waits 10 ms). */
int ahci_pci_set_power(struct ahci_hba *hba, int state);
/* Re-apply the PCI-level quirks an HBA reset undoes (Intel PCS port
 * enables).  Boot/process context. */
void ahci_pci_reset_quirks(struct ahci_hba *hba);

/* ==========================================================================
 * ahci_hba.c (A1)
 * ========================================================================== */

/* Boot entry points (kernel/ke/init.c).
 *   ahci_init(): before storage_fs_init(), interrupts off, no scheduler.
 *     Finds every AHCI function, initialises it and probes every port in
 *     polled mode; the disks found are registered with their partitions
 *     before it returns.  Never fails (logs and skips broken HBAs).
 *   ahci_late_init(): after sti and the timer calibration, on the boot
 *     thread, before the storage bootstrap mounts the root.  Switches
 *     each HBA to interrupts and starts its kahci thread.  Must not sleep
 *     on the boot thread: long work goes to kahci. */
void ahci_init(void);
void ahci_late_init(void);

/* Allocate and register an HBA object for a matched PCI function.
 * NULL on -ENOMEM. */
struct ahci_hba *ahci_hba_alloc(const pci_device_t *pci,
				const struct ahci_pci_id *id);
/* BIOS/OS handoff (CAP2.BOH): set OOS, wait for BOS to clear (25 ms),
 * then if BB is set at least 2 s more.  0 or -ETIMEDOUT (logged; the
 * driver proceeds -- the firmware is ignored after that). */
int ahci_hba_bios_handoff(struct ahci_hba *hba);
/* Set GHC.AE (written until it sticks, some HBAs need it repeated). */
int ahci_hba_enable_ahci(struct ahci_hba *hba);
/* GHC.HR with AE, wait <= 1 s for HR to clear, AE again, restore the
 * firmware CAP/PI write-once values.  Ports must be stopped.  May sleep
 * (busy-waits at boot).  0, -ETIMEDOUT (HBA dead). */
int ahci_hba_reset(struct ahci_hba *hba);
/* Read CAP/CAP2/VS/PI/EM_* into hba and decode caps (quirks applied:
 * FORCE_PI, 32BIT_ONLY, NO_NCQ ...).  Logs the controller line
 * ("ahci0: AHCI 1.3.1 32 slots 6 ports 6 Gbps 0x3f impl SATA mode ..."). */
void ahci_hba_read_caps(struct ahci_hba *hba);
/* Full controller bring-up: handoff, reset, AE, caps, CCC off, clear IS,
 * allocate and initialise every implemented port, GHC.IE.  0 or -errno. */
int ahci_hba_init(struct ahci_hba *hba);
/* Start every port: staggered spin-up, link up, probe devices (polled at
 * boot).  Returns the number of devices found. */
int ahci_hba_start_ports(struct ahci_hba *hba);
/* Global interrupt enable/disable (GHC.IE) under hba->lock. */
void ahci_hba_irq_enable(struct ahci_hba *hba);
void ahci_hba_irq_disable(struct ahci_hba *hba);
/* Controller recovery (kahci, all ports' eh_mutex taken inside in index
 * order): stop all ports, fail or requeue their commands, PCI state
 * restore, HBA reset, re-init, restart ports, revalidate devices.  0 or
 * -errno (HBA marked DEAD, disks detached). */
int ahci_hba_recover(struct ahci_hba *hba);
/* System shutdown of the controller (after blk_shutdown_all flushed the
 * disks): stop ports, mask interrupts, stop kahci.  May sleep. */
void ahci_hba_shutdown(struct ahci_hba *hba);
/* Suspend / resume of the controller (port contexts saved/restored, PCI
 * state, HBA reset on resume, link up, revalidate).  May sleep. */
int ahci_hba_suspend(struct ahci_hba *hba);
int ahci_hba_resume(struct ahci_hba *hba);
/* ahci_hba_shutdown() for every HBA, highest index first (for the system
 * power-off / reboot path, after blk_shutdown_all()).  May sleep. */
void ahci_shutdown_all(void);
/* kahci: start the thread (after sti), and wake it with work bits from
 * any context. */
int ahci_kahci_start(struct ahci_hba *hba);
void ahci_kahci_kick(struct ahci_hba *hba, uint32_t work);
/* Lookup / iteration */
struct ahci_hba *ahci_hba_get(int index);
int ahci_hba_count(void);

/* ==========================================================================
 * ahci_irq.c (A1)
 * ========================================================================== */

/* The registered handler (irq_handler_t, arg = hba).  Returns 1 when the
 * HBA had a pending interrupt.  See the handler rules at the top. */
int ahci_irq_handler(void *arg);
/* Service one port whose IS bit is set: read + ack PxIS (and SERR bits
 * that hold RO status bits), latch errors/hot-plug into eh_pending
 * (ahci_eh_irq_error / ahci_eh_schedule), collect finished commands
 * unless frozen and complete them (ata_qc_complete) -- all in one hold of
 * port->lock.  Interrupt handler only.  Returns the number of commands
 * completed. */
int ahci_port_irq(struct ahci_port *port);
/* Polled twin of the handler: every port of the HBA, regardless of IS.
 * Any context.  Returns the number of completions. */
int ahci_hba_poll(struct ahci_hba *hba);
/* The same for the polled-mode timer (hard interrupt context): it does
 * what the handler does.  Returns the number of commands completed. */
int ahci_hba_poll_timer(struct ahci_hba *hba);
/* One port (blk_ops.poll / the transport poll / ata_completion poll). */
int ahci_port_poll(struct ahci_port *port);
/* PxIE program/mask (port->lock held). */
void ahci_port_irq_enable_locked(struct ahci_port *port, uint32_t mask);
void ahci_port_irq_disable_locked(struct ahci_port *port);

/* ==========================================================================
 * ahci_dma.c (A1)
 * ========================================================================== */

/* Allocate a zeroed, physically contiguous area of `size' bytes aligned to
 * `align' (power of two, <= 4096 natural; larger via aligned pages),
 * below 4 GiB unless hba->dma64.  Boot/process context. */
int ahci_dma_alloc(struct ahci_hba *hba, size_t size, size_t align,
		   struct ahci_dma_area *out);
void ahci_dma_free(struct ahci_dma_area *a);
/* The command list, the received-FIS area (4 KiB when FBS-capable) and
 * nr_slots command tables of a port; fills port->cmds[].table/table_bus/
 * hdr and the header CTBA fields. */
int ahci_port_dma_alloc(struct ahci_port *port);
void ahci_port_dma_free(struct ahci_port *port);
/* CPU physical -> device bus address (identity until an IOMMU driver
 * exists; the single place to change then).  Pure. */
uint64_t ahci_dma_translate(const struct ahci_hba *hba, uint64_t phys);
/* Build the PRDT for an sg list: translate page by page, merge physically
 * contiguous pieces, split at AHCI_PRD_MAX_BYTES, reject odd lengths/
 * addresses (-EINVAL) and lists that need more than `max_prd' entries
 * (-E2BIG).  `xlate' turns a kernel virtual address into a bus address
 * (NULL: mm_get_physical_address + ahci_dma_translate).  Returns the
 * entry count and the byte total in *nbytes.  PURE apart from the
 * callback (host-testable). */
int ahci_prdt_build(struct ahci_prdt_entry *prdt, uint32_t max_prd,
		    const struct blk_sg_entry *sg, uint32_t nsg,
		    uint64_t (*xlate)(void *ctx, const void *virt), void *ctx,
		    uint32_t *nbytes);
/* Map a command's data into its table: PRDT build, bounce when needed
 * (32-bit HBA and memory above 4 GiB, or misaligned buffers): allocate a
 * bounce area, copy out for writes.  Sets cmd->n_prd/nbytes.  Any context
 * except that a bounce allocation may fail with -ENOMEM in IRQ context
 * (the issue path then returns -EBUSY to retry later). */
int ahci_dma_map(struct ahci_port *port, struct ahci_cmd *cmd,
		 const struct blk_sg_entry *sg, uint32_t nsg, int write);
/* After completion: copy a read bounce back into the sg list, free it. */
void ahci_dma_unmap(struct ahci_port *port, struct ahci_cmd *cmd, int write);
/* Ownership hand-off helpers (any context).  _for_device: the command
 * header + table (FIS, ACMD, PRDT) are complete -- written back from the
 * caches on a non-coherent platform, then ahci_wmb(); call right before
 * the SACT/CI write.  _for_cpu: after a completion was observed and
 * before the received-FIS area is read -- ahci_rmb(), then invalidated on
 * a non-coherent platform.  On x86 (coherent) they are the barriers only.
 * ahci_dma_noncoherent is the debug knob that turns the cache maintenance
 * on (/sys/kernel/debug/ahci/dma_noncoherent). */
void ahci_dma_sync_cmd_for_device(struct ahci_port *port, struct ahci_cmd *cmd);
void ahci_dma_sync_rx_for_cpu(struct ahci_port *port);
extern volatile int ahci_dma_noncoherent;
/* DMA statistics: pages held by AHCI structures; bounces ever / live /
 * failed allocations. */
uint64_t ahci_dma_stat_pages(void);
uint64_t ahci_dma_stat_bounces(uint64_t *live, uint64_t *failed);

/* ==========================================================================
 * ahci_port.c (A2)
 * ========================================================================== */

/* Allocate the port object, its DMA (ahci_port_dma_alloc), lock, eh_mutex,
 * trace ring; links initialised.  Boot context.  -ENOMEM. */
int ahci_port_alloc(struct ahci_hba *hba, int index);
void ahci_port_free(struct ahci_port *port);
/* Initialise a port for use: stop engines, program CLB/FB, clear SERR and
 * IS, spin-up (SUD, staggered by the caller), power on (POD for CPD
 * ports), FRE on, PxIE default, ICC active.  eh_mutex held (or boot).
 * 0 or -errno. */
int ahci_port_init(struct ahci_port *port);
/* Engine control (port->lock not held; may busy-wait up to 500 ms): */
int ahci_port_stop_engine(struct ahci_port *port); /* ST=0, wait CR=0 */
/* Stop the engine of a port whose commands in flight are failed or
 * requeued right afterwards (ahci_port_fail_all and friends): under
 * port->lock every active command is claimed for error handling
 * (ahci_cmd_claim_active_locked) and ST is cleared in the same critical
 * section, so no completion can mistake the PxCI/PxSACT bits the stop
 * clears for finished commands.  Then waits for CR=0 like
 * ahci_port_stop_engine().  0 or -ETIMEDOUT. */
int ahci_port_stop_engine_abort(struct ahci_port *port);
int ahci_port_start_engine(struct ahci_port *port); /* needs FRE, !BSY */
int ahci_port_stop_fis_rx(struct ahci_port *port); /* FRE=0, wait FR=0 */
int ahci_port_start_fis_rx(struct ahci_port *port); /* FRE=1, wait FR=1 */
/* Command list override: clear BSY/DRQ in PxTFD so a reset FIS can be
 * issued (CAP.SCLO).  Engine stopped.  0, -EOPNOTSUPP, -ETIMEDOUT. */
int ahci_port_clo(struct ahci_port *port);
/* COMRESET: SCTL.DET=1 (with the link's speed limit, IPM bits kept) for
 * >= 1 ms, DET=0, debounce (ahci_deb_timing()), clear SERR.
 * 0 (link up), -ENODEV (no device), -ETIMEDOUT.  eh_mutex held / boot. */
int ahci_port_comreset(struct ahci_port *port);
/* Software reset on a link: CLO if needed, H2D FIS with SRST (R and C
 * bits), wait, H2D FIS with SRST clear, wait for the signature D2H FIS;
 * returns the signature in *sig.  `pmp' = 15 resets a port multiplier's
 * control port.  eh_mutex held / boot. */
int ahci_port_softreset(struct ahci_port *port, int pmp, uint32_t *sig);
/* The same within a reset attempt: both FISes and the wait for the device
 * bounded by the attempt's deadline (`deadline_ns'). */
int ahci_port_softreset_deadline(struct ahci_port *port, int pmp,
				 uint32_t *sig, uint64_t deadline_ns);
/* Hard reset + wait ready + classify: COMRESET, wait BSY/DRQ clear
 * (AHCI_DEV_READY_MS, longer for spinning drives), read PxSIG.  Returns
 * the class in *class.  eh_mutex held / boot. */
int ahci_port_hardreset(struct ahci_port *port, enum ata_class *class);
/* The same as one attempt of error handling's reset: every wait bounded
 * by `timeout_ms' from now (the device not ready by then fails it). */
int ahci_port_hardreset_deadline(struct ahci_port *port,
				 enum ata_class *class, uint32_t timeout_ms,
				 int hotplug);
/* The reset of a port outside a run of error handling's ladder (a
 * hot-plugged device's probe, resume): the attempts of error handling's
 * reset, with their deadlines, debounce timings (`hotplug': the longer
 * ones after a hot-plug event), pauses and speed step-down.  0 or
 * -errno; *class as ahci_port_hardreset().  eh_mutex held, may sleep,
 * needs the clock (not the polled boot probe). */
int ahci_port_reset_attempts(struct ahci_port *port, enum ata_class *class,
			     int hotplug);
/* Lower the host link's speed limit by one generation (effective at its
 * next COMRESET).  0, or -EAGAIN when nothing is slower. */
int ahci_port_down_spd_limit(struct ahci_port *port);
/* Debounce of a reset attempt's link (host or fan-out): DET sampled every
 * timing[0] ms must hold timing[1] ms; DET = 1 is waited out until the
 * deadline (the attempt's, or timing[2] ms from now when earlier;
 * AHCI_NO_DEADLINE: timing[2] alone).  The sleeps are counted too, so a
 * clock that does not advance still ends it.  0, or -EPIPE when DET still
 * changes at the deadline (lower the speed); the last SStatus is in
 * *sstatus either way.  May sleep. */
#define AHCI_NO_DEADLINE (~0ULL)
int ahci_link_debounce_deadline(struct ahci_link *link,
				const uint32_t *timing, uint64_t deadline_ns,
				uint32_t *sstatus);
/* The debounce timings { interval, duration, timeout } (ms): normal, or
 * after a hot-plug event. */
const uint32_t *ahci_deb_timing(int hotplug);
/* Wait until PxTFD has BSY and DRQ clear.  0 or -ETIMEDOUT. */
int ahci_port_wait_ready(struct ahci_port *port, uint32_t timeout_ms);
/* PxSIG -> class (ata_class_from_sig). */
enum ata_class ahci_port_classify(struct ahci_port *port);
/* Bring the port's link up and attach what is there: reset, classify;
 * PMP -> ahci_pmp_attach(); ATA/ATAPI -> ata_dev_alloc + ata_dev_probe on
 * the host link.  Logs "ata1: SATA link up 6.0 Gbps (SStatus 133
 * SControl 300)" or "link down".  eh_mutex held / boot.  0 (also for an
 * empty port) or -errno. */
int ahci_port_probe(struct ahci_port *port);
/* The polled boot probe of several ports (none with a multiplier
 * attached) at once: their COMRESETs asserted, released and debounced
 * together, the waiting overlapped, each port's result as alone.
 * rcs[i]: ports[i]'s COMRESET result for ahci_port_probe_after_comreset(),
 * which goes on with that port's probe as ahci_port_probe() would.
 * Every port's eh_mutex held, boot only. */
void ahci_ports_comreset_together(struct ahci_port *const *ports, int n,
				  int *rcs);
int ahci_port_probe_after_comreset(struct ahci_port *port, int rc);
/* Hot-plug handling (kahci via EH, eh_mutex held): the link reset with
 * the hot-plug debounce timing (`did_reset': this EH round's recovery
 * reset it already), then the device attached, detached or revalidated
 * by what the reset found.  Drops the latched hot-plug bits. */
int ahci_port_hotplug(struct ahci_port *port, int did_reset);
/* Link power management policy for a link (host link: SCTL.IPM, PxCMD
 * ALPE/ASP/APSTE, DevSleep; device: DIPM through ata_set_sata_feature).
 * eh_mutex held.  0, -EOPNOTSUPP. */
int ahci_port_set_lpm(struct ahci_port *port, struct ahci_link *link,
		      enum ahci_lpm_policy policy);
/* Set a link's speed limit (0: none; sysfs) and schedule error
 * handling's hard reset, which applies it with the port frozen.  No reset
 * of its own.  eh_mutex held. */
int ahci_link_set_speed_limit(struct ahci_link *link, uint8_t spd);
/* SError -> statistics and the EH actions it implies (link reset for
 * persistent errors, hot-plug for X/N ...).  Pure apart from counters. */
uint32_t ahci_serror_classify(struct ahci_port *port, uint32_t serr);
/* Port suspend/resume/shutdown (eh_mutex taken inside).  May sleep. */
int ahci_port_suspend(struct ahci_port *port);
int ahci_port_resume(struct ahci_port *port);
void ahci_port_shutdown(struct ahci_port *port);
/* SCR access on the host link (PxSSTS/PxSCTL/PxSERR); a PMP link goes
 * through ahci_pmp_scr_*.  Any context. */
int ahci_link_scr_read(struct ahci_link *link, int scr, uint32_t *val);
int ahci_link_scr_write(struct ahci_link *link, int scr, uint32_t val);
#define AHCI_SCR_STATUS 0
#define AHCI_SCR_ERROR 1
#define AHCI_SCR_CONTROL 2
#define AHCI_SCR_ACTIVE 3
#define AHCI_SCR_NOTIFY 4

/* ---- ahci_port.c helpers shared with ahci_pmp.c and error handling ------- */

/* Count one link error (CRC, disparity, decode, handshake, persistent
 * communication) towards the step-down window; and, before a reset,
 * lower link->speed_limit one generation when the window is full.  Any
 * context / eh_mutex held. */
void ahci_link_note_error(struct ahci_link *link);
void ahci_link_check_speed_down(struct ahci_link *link);
/* Re-read the link's SCRs into its cache (sstatus/scontrol/serror, speed,
 * online state).  Host link: any context; PMP link: eh_mutex held. */
void ahci_link_refresh(struct ahci_link *link);
/* Attach a classified device to a link: ata_dev_alloc, transport limits,
 * ata_dev_probe, LPM policy.  ahci_link_detach: ata_dev_detach + drop the
 * link's reference (the caller has stopped the engine and failed the
 * device's commands first).  eh_mutex held / boot. */
int ahci_link_attach(struct ahci_link *link, enum ata_class class);
void ahci_link_detach(struct ahci_link *link);
/* The ABORT rung: stop the engine (CR clear), clear PxSERR and the error
 * bits of PxIS, CLO when BSY/DRQ are stuck, start the engine again.
 * Outstanding commands are lost (the caller fails or requeues them).
 * eh_mutex held.  0, -ETIMEDOUT, -EBUSY (BSY stuck, no CLO). */
int ahci_port_engine_restart(struct ahci_port *port);
/* Take the PHY offline (SCTL.DET = 4; EH gave up on the port) / back
 * online (DET = 0).  eh_mutex held. */
int ahci_port_phy_offline(struct ahci_port *port);
int ahci_port_phy_online(struct ahci_port *port);
/* Quiesce a port for work that needs the engine stopped or no command
 * outstanding: stop the block queues of its disks; on a RUNNING port
 * refuse new non-EH commands (state EH) and drain (ahci_port_drain, then
 * PxCI|PxSACT == 0); on a port error handling owns (FROZEN/EH) only wait
 * for the hardware to be idle -- its commands are never waited for in
 * software nor aborted (-EBUSY when the hardware stays busy).
 * timeout_ms 0: stop the queues / set the state only, no waiting.
 * `*token' records what was changed; ahci_port_unquiesce(port, token)
 * undoes exactly that.  eh_mutex held.  0, -ETIMEDOUT, -EBUSY (the token
 * is valid either way). */
int ahci_port_quiesce(struct ahci_port *port, uint32_t timeout_ms,
		      uint32_t *token);
void ahci_port_unquiesce(struct ahci_port *port, uint32_t token);
#define AHCI_QUIESCE_STATE (1u << 31) /* token: state was set to EH */
#define AHCI_QUIESCE_ALL 0x7FFFFFFFu /* token: restart every link's queue */
/* LPM policy <-> sysfs text ("max_performance", "medium_power",
 * "med_power_with_dipm", "min_power", "keep_firmware_settings").  parse
 * returns the enum value or -EINVAL.  Pure. */
const char *ahci_lpm_policy_name(enum ahci_lpm_policy p);
int ahci_lpm_policy_parse(const char *s);

/* The D2H register FIS area the HBA fills for PMP port `pmp' (with FBS one
 * area per device, without it the single shared area).  Read only through
 * the returned volatile pointer, after ahci_rmb(). */
static inline volatile struct fis_reg_d2h *
ahci_port_rx_d2h(const struct ahci_port *port, int pmp)
{
	volatile uint8_t *base = (volatile uint8_t *)port->rx_fis;

	if (port->fbs_enabled && pmp >= 0 && pmp < 16)
		base += (uint32_t)pmp * AHCI_RX_FIS_SIZE;
	return (volatile struct fis_reg_d2h *)(base + AHCI_RXFIS_RFIS);
}

/* ==========================================================================
 * ahci_pmp.c (A2)
 * ========================================================================== */

/* READ / WRITE PORT MULTIPLIER of register `reg' of PMP port `pmp_port'
 * (15 = GSCR).  eh_mutex held (or boot).  0, -EIO, -ETIMEDOUT. */
int ahci_pmp_read(struct ahci_port *port, int pmp_port, int reg,
		  uint32_t *val);
int ahci_pmp_write(struct ahci_port *port, int pmp_port, int reg,
		   uint32_t val);
/* PSCR access of a fan-out link. */
int ahci_pmp_scr_read(struct ahci_link *link, int scr, uint32_t *val);
int ahci_pmp_scr_write(struct ahci_link *link, int scr, uint32_t val);
/* A port multiplier was classified on the port: PxCMD.PMA, read GSCR
 * (product, revision, ports, features), enable notification and FBS where
 * possible, then reset + classify + probe every fan-out link.  Logs
 * "ata1.15: Port Multiplier 1.2, 0x197b:0x0325 r0, 5 ports, feat 0x5/0xf".
 * eh_mutex held / boot. */
int ahci_pmp_attach(struct ahci_port *port);
void ahci_pmp_detach(struct ahci_port *port);
/* Hard reset of one fan-out link through PSCR SControl, classify (SRST
 * with the link's PMP number), returns the class.  eh_mutex held. */
int ahci_pmp_link_reset(struct ahci_link *link, enum ata_class *class);
/* A fan-out link's reset within recovery: failed resets consume the
 * link's tries (port->pmp_link_tries); out of tries the link is disabled
 * and its device with it.  Disabled links are not reset (-ENODEV). */
int ahci_pmp_link_recover(struct ahci_link *link, enum ata_class *class);
/* The disabled fan-out links: a quick reset each, SError.N cleared, so
 * their PhyRdy change can be noticed. */
int ahci_pmp_handle_disabled_links(struct ahci_port *port);
/* Every fan-out link gets AHCI_EH_PMP_LINK_TRIES again (an EH run starts,
 * a multiplier came online). */
void ahci_pmp_eh_begin(struct ahci_port *port);
/* Recover the multiplier itself: reset the host link (`reset'), then
 * revalidate it (and its links), up to AHCI_EH_PMP_TRIES times -- a
 * different or vanished multiplier two more times at most -- with a lower
 * link speed after consecutive failures; given up, it is detached
 * (-ENODEV).  eh_mutex held, may sleep. */
int ahci_pmp_recover(struct ahci_port *port, int reset);
/* PxSNTF / GSCR error bits: which links changed; schedules their EH.
 * eh_mutex held. */
int ahci_pmp_handle_notify(struct ahci_port *port);
/* FIS-based switching (CAP.FBSS + PxCMD.FBSCP): enable with the engine
 * stopped (rx FIS area becomes 16 x 256), disable.  A single device error
 * (PxFBS.SDE/DWE) is cleared (DEC) by the interrupt handler.  eh_mutex
 * held. */
int ahci_fbs_enable(struct ahci_port *port);
int ahci_fbs_disable(struct ahci_port *port);
/* Re-check an attached port multiplier after a host link reset or resume:
 * same product (GSCR 0) or -ENODEV (the caller detaches and re-probes),
 * features re-enabled, FBS restored, every fan-out link reset and its
 * device revalidated / attached / detached.  eh_mutex held. */
int ahci_pmp_revalidate(struct ahci_port *port);
/* The issue path's PxFBS.DEV update: with FBS enabled the DEV field must
 * name the PMP port of a command before its CI bit is set (rewritten only
 * when it differs from port->fbs_last_dev).  port->lock held. */
void ahci_fbs_select_dev_locked(struct ahci_port *port, int pmp);

/* ==========================================================================
 * ahci_cmd.c (A3)
 * ========================================================================== */

/* The ATA transport of AHCI links (qc_issue = ahci_qc_issue, poll, reset,
 * schedule_eh, set_lpm, link_info, lock/unlock = eh_mutex). */
extern const struct ata_transport ahci_ata_transport;

/* Issue a queued command (see struct ata_transport.qc_issue for the
 * contract).  Allocates a slot (NCQ: tag == slot), builds header + H2D FIS
 * + ACMD + PRDT (ahci_dma_map), ahci_wmb(), sets SACT (NCQ) then CI.
 * Non-NCQ while NCQ is active (or the reverse) -> -EBUSY.  Takes
 * port->lock.  Any context. */
int ahci_qc_issue(struct ata_queued_cmd *qc);
/* Slot allocation (port->lock held).  NCQ commands get 0..ncq_depth-1,
 * the internal slot is reserved for ATA_QCFLAG_EH commands.  Returns the
 * slot or -EBUSY. */
int ahci_slot_alloc_locked(struct ahci_port *port, int ncq, int internal);
void ahci_slot_free_locked(struct ahci_port *port, uint32_t slot);
/* Fill a slot's command header and table for qc (port->lock held). */
int ahci_cmd_prepare_locked(struct ahci_port *port, struct ahci_cmd *cmd,
			    struct ata_queued_cmd *qc);
/* Completion processing: read PxCI/PxSACT, collect finished slots
 * (active & ~CI, ncq_active & ~SACT), copy result TFs from the received
 * FIS (RESULT_TF or error), unmap DMA, free slots -- all under port->lock
 * -- then DROP it and call ata_qc_complete() for each (the pollers, error
 * handling).  Any context.  Returns the number completed. */
int ahci_port_complete(struct ahci_port *port);
/* The interrupt handler's half of ahci_port_complete(): with port->lock
 * held (the hold that acknowledged PxIS), take the completions parked on
 * cpl_head and collect the finished slots onto *headp, *tailp (the same
 * register reads and retirement).  Returns the number of commands
 * collected. */
int ahci_port_irq_collect_locked(struct ahci_port *port,
				 struct ata_queued_cmd **headp,
				 struct ata_queued_cmd **tailp);
/* The same for the collection an error interrupt does before the error
 * is handed over: a non-queued command whose PxCI bit cleared although
 * its received FIS carries ERR/DF (an HBA model that clears PxCI before
 * signalling the task file error) is not completed but left in flight,
 * charged with AC_ERR_DEV, for error handling.  port->lock held. */
int ahci_port_irq_collect_error_locked(struct ahci_port *port,
				       struct ata_queued_cmd **headp,
				       struct ata_queued_cmd **tailp);
/* Complete a collected chain with port->lock held (the interrupt handler
 * and the polled-mode timer). */
void ahci_complete_chain_locked(struct ahci_port *port,
				struct ata_queued_cmd *qc);
/* Complete a chain of commands linked through xport_next.  No transport
 * lock held.  Returns the number completed. */
int ahci_complete_chain(struct ata_queued_cmd *qc);
/* The deferred non-queued command (port->deferred_qc): after completions,
 * hand its issue to kblockd once nothing runs any more (or give it back
 * for a requeue when error handling is due).  port->lock held. */
void ahci_cmd_schedule_deferred_locked(struct ahci_port *port);
/* Give the deferred command back to its issuer with `err' (-ECANCELED:
 * requeue; -ETIMEDOUT: it timed out waiting), parked on cpl_head.
 * port->lock held. */
void ahci_cmd_requeue_deferred_locked(struct ahci_port *port, int err);
/* deferred_work's function (kblockd). */
void ahci_cmd_deferred_work_fn(void *arg);
/* Execute a raw FIS on the internal slot with the given command header
 * flags (AHCI_CMDH_R | AHCI_CMDH_C for SRST), polled, no completion
 * interrupt expected; the engine must be running.  For reset sequences
 * (ahci_port_softreset).  eh_mutex held / boot.  0, -ETIMEDOUT, -EIO
 * (error status), -ECANCELED (aborted by another engine stop). */
int ahci_exec_raw_fis(struct ahci_port *port, int pmp,
		      const struct fis_reg_h2d *fis, uint32_t hdr_flags,
		      uint32_t timeout_ms);
/* Wait until no command is active (sleeps / polls).  0, -ETIMEDOUT. */
int ahci_port_drain(struct ahci_port *port, uint32_t timeout_ms);
/* Complete every outstanding and held command with `err'
 * (-ENODEV unplug, -EIO give up, -ECANCELED requeue after reset).  Takes
 * port->lock inside, completes outside.  Any context.  The engine should
 * have been stopped with ahci_port_stop_engine_abort() first; a command
 * the HBA still owns (the stop failed) is completed with
 * ATA_QCFLAG_HW_OWNED and its slot left as a zombie. */
void ahci_port_fail_all(struct ahci_port *port, int err);
/* The same before a controller reset: every command that reached the HBA
 * (not error handling's own) is held, not completed -- the device may
 * still run it -- until the reset is done (ahci_port_dma_done). */
void ahci_port_fail_all_for_reset(struct ahci_port *port, int err);
/* The controller can do no DMA for this port any more (its reset is
 * done, or hba->dma_stopped): every zombie slot is free, the commands
 * held for them are handed back (-ECANCELED, requeued). */
void ahci_port_dma_done(struct ahci_port *port);

/* ==========================================================================
 * ahci_eh.c (A3)
 * ========================================================================== */

/* Request error handling (any context): eh_pending |= actions; freezing
 * actions freeze the port; kicks kahci. */
void ahci_eh_schedule(struct ahci_port *port, uint32_t actions);
/* Ask error handling for `actions' without freezing anything (a request
 * from outside, e.g. sysfs): the commands in flight finish first, the run
 * freezes the port across its resets itself.  Any context but hard IRQ. */
void ahci_eh_request(struct ahci_port *port, uint32_t actions);
/* From the interrupt path with port->lock HELD: latch PxIS/PxSERR/PxTFD
 * and the active slots, decide the actions (TFES -> DEV_ERROR or
 * NCQ_ERROR, HBFS/HBDS -> HOST_ERROR, IFS -> IF_FATAL ...), freeze. */
void ahci_eh_irq_error_locked(struct ahci_port *port, uint32_t irq_stat);
/* An injected fault (debug): `slots' failed (they never reached the HBA),
 * `irq_stat' and `serr' stand for what the hardware would have reported
 * and are latched for error handling, marked AHCI_EH_FAULT.  The other
 * commands are left to the device -- not aborted, the port not frozen --
 * and error handling starts once they have drained.  port->lock held. */
void ahci_eh_inject_error_locked(struct ahci_port *port, uint32_t slots,
				 uint32_t irq_stat, uint32_t serr);
/* Freeze: mask PxIE, state FROZEN, stop the block queues of the port's
 * disks; thaw: clear PxIS/SERR, unmask, RUNNING, restart the queues.
 * port->lock held. */
void ahci_eh_freeze_locked(struct ahci_port *port);
void ahci_eh_thaw_locked(struct ahci_port *port);
/* While EH runs (eh_running): let its callees' commands (revalidation,
 * probing, media checks, LPM) complete by interrupt -- clear PxIS/SERR,
 * unmask -- but keep the port in state AHCI_PORT_EH, so that no ordinary
 * command is admitted before EH has finished.  port->lock held. */
void ahci_eh_callee_unmask_locked(struct ahci_port *port);
/* The EH run for one port (kahci; takes eh_mutex): snapshot eh_pending,
 * analyse (NCQ log 10h for NCQ errors, SError, TFD), choose the ladder
 * rung, recover, retry or fail commands (ATA_MAX_RETRIES, FAILFAST),
 * handle hot-plug / SNTF / LPM / media, revalidate devices, thaw.  Wakes
 * port->eh_wait when done. */
void ahci_eh_run(struct ahci_port *port);
/* One rung of the ladder and everything below it as needed.  eh_mutex
 * held.  0 recovered, -errno escalate. */
int ahci_eh_recover(struct ahci_port *port, enum ahci_eh_level level);
/* NCQ error analysis: READ LOG EXT 10h on the internal slot (port must be
 * stopped/restarted around it), find the failed tag, mark the others as
 * innocent for retry.  eh_mutex held.  Returns the failed tag or -errno. */
int ahci_eh_ncq_analyse(struct ahci_port *port);
/* The transport's reset op: schedule + wait for EH with the reset
 * action.  May sleep. */
int ahci_eh_reset_dev(struct ata_device *dev, int kind);
/* Block until the port's EH is idle (eh_pending == 0).  May sleep. */
int ahci_eh_wait(struct ahci_port *port, uint32_t timeout_ms);
/* ahci_eh_schedule() for a caller that already HOLDS port->lock (the
 * interrupt path latching hot-plug or notification events).  Kicks kahci
 * (port->lock -> hba->lock order). */
void ahci_eh_schedule_locked(struct ahci_port *port, uint32_t actions);
/* Non-blocking EH for the polled path (the transport's poll at boot, before
 * kahci runs): runs the pending EH inline when the port's eh_mutex is free
 * or already owned by this context, and never nests.  Any context that may
 * busy-wait (boot, polled waits).  Returns 1 when EH ran. */
int ahci_eh_poll_run(struct ahci_port *port);

/* ---- ahci_cmd.c <-> ahci_eh.c internals (A3; not for other files) ---- */
/* Retire a slot whose command the HBA no longer owns: DMA unmap, detach
 * the qc (returned), free the slot.  port->lock held. */
struct ata_queued_cmd *ahci_cmd_retire_locked(struct ahci_port *port,
					      struct ahci_cmd *cmd);
/* Hand a finished qc to the deferred-completion list (completed later by
 * ahci_cmd_run_deferred_completions without any transport lock). */
void ahci_cmd_defer_complete_locked(struct ahci_port *port,
				    struct ata_queued_cmd *qc);
/* Complete everything on the deferred list.  No transport lock held.  Any
 * context.  Returns the number completed. */
int ahci_cmd_run_deferred_completions(struct ahci_port *port);
/* Free zombie slots whose CI/SACT bits the HBA has cleared.  Returns the
 * zombies still owned by the HBA.  port->lock held. */
uint32_t ahci_cmd_reap_zombies_locked(struct ahci_port *port);
/* Stop the command engine, clear CLO-able BSY/DRQ, restart it (the
 * "kick" after an aborted command).  Commands still in flight are claimed
 * before the stop and handed back with -ECANCELED (requeue): the stop
 * cleared their PxCI/PxSACT bits.  port->lock NOT held; busy-waits up to
 * ~1 s.  0 or -errno. */
int ahci_cmd_kick_engine(struct ahci_port *port);
/* Re-issue a command that already holds the transport reference (EH
 * retry): no fairness hold-back, no new reference.  port->lock NOT held.
 * Returns the ahci_qc_issue codes. */
int ahci_qc_reissue(struct ata_queued_cmd *qc);
/* The port's receive FIS area of a PMP port (FBS: its own 256-byte
 * structure, else the shared one). */
struct ahci_rx_fis *ahci_rx_fis_of(struct ahci_port *port, int pmp);
/* Result task file of the last command from the receive FIS area and
 * PxTFD (D2H register FIS, PIO setup ending status, SDB status). */
void ahci_cmd_read_result_tf(struct ahci_port *port, int pmp,
			     enum ata_prot prot, int ncq,
			     struct ata_taskfile *tf);
/* Default timeout of a command in ms (by opcode and protocol).  Pure. */
uint32_t ahci_qc_default_timeout(const struct ata_queued_cmd *qc);
/* Take a slot's command away from the HBA's ownership bookkeeping while
 * the HBA may still hold it (CI/SACT set): the slot stays allocated as a
 * zombie until the bits clear; the qc is returned.  port->lock held. */
struct ata_queued_cmd *ahci_cmd_zombify_locked(struct ahci_port *port,
					       struct ahci_cmd *cmd);
/* Mark every command in flight as owned by error handling (state
 * AHCI_CMD_EH), so that ahci_port_complete() leaves it alone once an
 * engine stop has cleared its PxCI/PxSACT bit.  A raw FIS still pending in
 * PxCI is marked the same way, which its poller (ahci_exec_raw_fis)
 * reports as -ECANCELED instead of reading the cleared bit as success.
 * Returns the claimed slots.  port->lock held. */
uint32_t ahci_cmd_claim_active_locked(struct ahci_port *port);
/* Was `qc' issued by the holder of the port's eh_mutex (ATA_QCFLAG_EH:
 * an EH run's callees, a management command under ata_dev_lock), or by
 * an EH run without a task (boot), rather than being ordinary traffic?
 * Such a command is completed on a halting error or timeout at once: its
 * issuer holds the mutex an EH run would need.  port->lock held. */
int ahci_qc_is_eh_own(const struct ahci_port *port,
		      const struct ata_queued_cmd *qc);
/* Complete the in-flight commands in `mask' (those the eh_mutex holder
 * issued, ahci_qc_is_eh_own) with `err'/`err_mask', the result registers
 * from the received-FIS area, through the deferred-completion list.  No
 * register is read: each slot stays allocated (a zombie) until the next
 * engine stop, and the command completes with ATA_QCFLAG_HW_OWNED (its
 * buffer is the HBA's until then).  Returns how many.  port->lock held,
 * any context. */
int ahci_cmd_eh_fail_inflight_locked(struct ahci_port *port, uint32_t mask,
				     int err, uint32_t err_mask);
/* Abort everything in flight for error handling: the eh_mutex holder's
 * own commands are completed with `own_err'/`own_err_mask'
 * (ahci_cmd_eh_fail_inflight_locked), every other command becomes error
 * handling's (AHCI_CMD_EH).  Returns the slots handed to EH.
 * port->lock held, any context. */
uint32_t ahci_cmd_eh_abort_locked(struct ahci_port *port, int own_err,
				  uint32_t own_err_mask);
/* Every command in flight on the port has failed (aborted for EH or
 * timed out) or none is in flight: error handling may start.
 * port->lock held. */
int ahci_eh_drained_locked(const struct ahci_port *port);
/* The transport poll of one port: interrupt-less completion, parked
 * completions and inline EH while no kahci thread runs.  Any context. */
int ahci_cmd_poll_port(struct ahci_port *port);

/* ==========================================================================
 * ahci_debug.c (A1, hooks used by A3)
 * ========================================================================== */

/* kprintf dumps of every HBA register / one port / one slot. */
void ahci_dump_hba(const struct ahci_hba *hba);
void ahci_dump_port(const struct ahci_port *port);
void ahci_dump_cmd(const struct ahci_port *port, uint32_t slot);
/* Text decoders (pure): IS bits "TFES IFS ...", SError "{ PHYRdyChg
 * 10B8B Dispar ... }", PxCMD "ST FRE FR CR ...". */
int ahci_irq_stat_str(uint32_t is, char *buf, int cap);
int ahci_serror_str(uint32_t serr, char *buf, int cap);
int ahci_pxcmd_str(uint32_t cmd, char *buf, int cap);
/* Trace ring: allocate per port; record from any context; format for the
 * debug file. */
struct ahci_trace_ring *ahci_trace_alloc(void);
void ahci_trace(struct ahci_port *port, uint8_t event,
		const struct ahci_cmd *cmd, int result, uint32_t aux);
long ahci_trace_format(const struct ahci_port *port, char *buf, long cap);
/* Fault injection: called by ahci_qc_issue (port->lock held) for every
 * command; returns AHCI_FAULT_* to inject. */
int ahci_fault_check_locked(struct ahci_port *port, const struct ahci_cmd *cmd);
/* sysfs:
 *   /sys/class/ata_port/ataN/{port_no,idle_irq,nr_pmp_links,stats,regs,
 *     trace,fault_inject,link_power_management_policy,eh}
 *   /sys/class/ata_link/linkN[.M]/{hw_sata_spd_limit,sata_spd_limit,
 *     sata_spd,sstatus,serror}
 *   /sys/class/ata_device/devN.M[.D]/{class,id,model,serial,firmware,
 *     ncq_depth,errors,power_state,security,trim}
 *   /sys/kernel/debug/ahci/<hba>/{regs,ports/<n>/{regs,trace,fault}}
 * Boot or process context. */
int ahci_sysfs_add_hba(struct ahci_hba *hba);
int ahci_sysfs_add_port(struct ahci_port *port);
int ahci_sysfs_add_link(struct ahci_link *link);
int ahci_sysfs_add_device(struct ata_device *dev);
void ahci_sysfs_remove_device(struct ata_device *dev);

/* ==========================================================================
 * Compile-time layout checks
 * ========================================================================== */

static inline void ahci_layout_checks(void)
{
	BUILD_BUG_ON(sizeof(struct ahci_cmd_header) != 32);
	BUILD_BUG_ON(sizeof(struct ahci_prdt_entry) != 16);
	BUILD_BUG_ON(sizeof(struct ahci_rx_fis) != AHCI_RX_FIS_SIZE);
	BUILD_BUG_ON(sizeof(struct ahci_rx_fis_fbs) != AHCI_RX_FIS_FBS_SIZE);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_rx_fis, dsfis) !=
		     AHCI_RXFIS_DSFIS);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_rx_fis, psfis) !=
		     AHCI_RXFIS_PSFIS);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_rx_fis, rfis) !=
		     AHCI_RXFIS_RFIS);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_rx_fis, sdbfis) !=
		     AHCI_RXFIS_SDBFIS);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_rx_fis, ufis) !=
		     AHCI_RXFIS_UFIS);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_rx_fis, rsvd) !=
		     AHCI_RXFIS_RSVD);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_cmd_table, cfis) !=
		     AHCI_CT_CFIS_OFF);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_cmd_table, acmd) !=
		     AHCI_CT_ACMD_OFF);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_cmd_table, rsvd) !=
		     AHCI_CT_RSVD_OFF);
	BUILD_BUG_ON(__builtin_offsetof(struct ahci_cmd_table, prdt) !=
		     AHCI_CT_PRDT_OFF);
	BUILD_BUG_ON(sizeof(struct ahci_cmd_table) != AHCI_CMD_TABLE_SIZE);
	BUILD_BUG_ON(AHCI_CMD_TABLE_SIZE != 4096);
	BUILD_BUG_ON(AHCI_CMD_TABLE_SIZE % AHCI_CMD_TABLE_ALIGN);
	BUILD_BUG_ON(AHCI_CMD_LIST_SIZE != 1024);
	BUILD_BUG_ON(sizeof(struct fis_reg_h2d) > AHCI_CT_CFIS_LEN);
	BUILD_BUG_ON(ATAPI_CDB_LEN_MAX > AHCI_CT_ACMD_LEN);
	BUILD_BUG_ON(AHCI_MAX_SLOTS > 32);
	BUILD_BUG_ON(AHCI_PMP_MAX_LINKS != 15);
	BUILD_BUG_ON((AHCI_PRD_MAX_BYTES - 1) != AHCI_PRD_DBC_MASK);
	BUILD_BUG_ON(AHCI_PxIS_ERROR & AHCI_PxIS_COMPLETE);
	BUILD_BUG_ON(AHCI_ABAR_MIN_SIZE != 0x1100);
}

#endif // KERNEL_DEV_ATA_AHCI_H
