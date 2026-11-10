// LikeOS AHCI driver - command slots, submission, completion, timeouts
//
// This file is the command engine of an AHCI port: everything between an
// ATA queued command (struct ata_queued_cmd, built by the ATA layer) and
// the HBA's command list.  It owns
//
//   - the data side of a command: the PRDT that ahci_dma_map() builds
//     (ahci_dma.c) from the block layer's scatter/gather list is checked
//     against the even-address / even-length / 4 MiB rules of the AHCI
//     1.3.1 specification (section 4.2.3.3) and gets its I bit;
//   - slot allocation: the command list has CAP.NCS + 1 slots, an NCQ tag
//     IS the slot number, and the highest slot is reserved for commands
//     error handling issues (ATA_QCFLAG_EH) so recovery can always run;
//   - command construction: the Register Host to Device FIS from the task
//     file (C bit, PMP field, the NCQ tag in the count register), the
//     ATAPI packet, the command header (CFL, A, W, P, R, B, C, PMP, PRDTL,
//     CTBA) -- section 4.2.2 and 4.2.3;
//   - submission: PxSACT for first-party DMA queued commands, then PxCI,
//     after a store fence that hands the header, table and PRDT to the HBA
//     (section 5.3.2 and 5.3.12).  A non-queued command never runs next
//     to queued ones (SATA forbids mixing): it is refused with -EBUSY
//     while queued commands are outstanding, and the block queue holds it
//     at its head -- dispatching nothing behind it -- until a completion
//     lets it try again, so the queued commands drain and it cannot
//     starve;
//   - completion: the slots the HBA finished are found by the difference
//     between what the driver issued and what PxCI / PxSACT still show,
//     the result task file is taken from the received-FIS area (D2H
//     Register FIS, PIO Setup ending status, Set Device Bits), the slots
//     are released, and the commands are completed -- by the interrupt
//     handler with the port lock held (ata_qc_complete only wakes an
//     internal waiter or hands a block request to the block softirq), by
//     the pollers and error handling after dropping it;
//   - timeouts: there is no timer here.  The block queue's timeout work
//     (one coarse timer per queue) and an internal command's bounded wait
//     call the transport's qc_timed_out, which marks the command and hands
//     it to the error handler (ahci_eh.c) on the kahci thread;
//   - the polled raw-FIS path the reset sequences use (ahci_exec_raw_fis),
//     the engine "kick" after an aborted command, port drain and the
//     "fail everything" path used by unplug and controller recovery;
//   - the struct ata_transport of AHCI devices.
//
// Ownership of a slot.  The HBA owns a slot from the PxCI (and PxSACT)
// write until the bit clears again -- by completion or by clearing
// PxCMD.ST.  Software may reuse a slot, its command table and the data
// buffers only after that.  When software has to give up on a command
// while the HBA might still hold it (a command of the error handler itself
// timed out or failed while the engine is halted), the queued command is
// completed but the slot stays allocated as a "zombie" until PxCI/PxSACT
// show it free; the next engine stop frees it.
//
// Locking (see ahci.h): port->lock (irqsave) protects every field touched
// here; hba->lock nests inside it.  ata_qc_complete() never issues a
// command and never enters the block layer's request end, so the
// interrupt handler calls it with port->lock held; everybody else
// completes after dropping the lock (an inline completion of a poller
// with interrupts off ends the block request right there).
//
// -O2 rules: register access goes through the volatile accessors of
// ahci.h; memory the HBA writes (received FISes, PRDBC) is read through
// volatile pointers after ahci_rmb(); memory the HBA reads (header, table,
// PRDT) is fully written before ahci_wmb() and the PxCI store.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define AHCI_PFX "[ahci] "

/* The 4 KiB granule of the PRDT layout checks. */
#define AHCI_PRDT_PAGE 4096ULL
/* PRDTL is 16 bits wide. */
#define AHCI_PRDTL_MAX 0xFFFFu
/* Polling granularity of the raw-FIS and drain waits. */
#define AHCI_RAW_POLL_US 10
#define AHCI_RAW_FAST_US 2000 /* poll finely this long, then per ms */

/* The prefetchable (P) bit of the command header lets the HBA fetch the
 * PRDT ahead of the data phase.  It is an optimisation only, it must not
 * be used for queued or ATAPI commands, and some controllers have been
 * seen to mishandle it, so it stays off; the decision point is kept in
 * ahci_cmd_prepare_locked() so it can be enabled for a known-good HBA. */
static const int ahci_use_prefetch = 0;

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

static inline uint32_t ahci_bit(uint32_t slot)
{
	return 1u << (slot & 31);
}

static inline uint32_t ahci_popcount32(uint32_t v)
{
	uint32_t n = 0;

	while (v) {
		v &= v - 1;
		n++;
	}
	return n;
}

/* Lowest set bit of a non-zero mask. */
static inline uint32_t ahci_ffs32(uint32_t v)
{
	return (uint32_t)__builtin_ctz(v);
}

static inline uint32_t ahci_slot_mask(uint32_t nr_slots)
{
	if (nr_slots >= 32)
		return 0xFFFFFFFFu;
	return (1u << nr_slots) - 1u;
}

static inline int ahci_qc_ok(const struct ata_queued_cmd *qc)
{
	return qc && qc->magic == ATA_QC_MAGIC;
}

static inline struct ahci_link *ahci_qc_link(const struct ata_queued_cmd *qc)
{
	if (!qc->dev || qc->dev->magic != ATA_DEVICE_MAGIC)
		return NULL;
	return (struct ahci_link *)qc->dev->xport_priv;
}

static inline int ahci_qc_is_ncq(const struct ata_queued_cmd *qc)
{
	return qc->protocol == ATA_PROT_FPDMA;
}

/* Data direction of a command as the command header's W bit wants it. */
static int ahci_qc_is_write(const struct ata_queued_cmd *qc)
{
	switch (qc->protocol) {
	case ATA_PROT_PIO_OUT:
	case ATA_PROT_DMA_OUT:
		return 1;
	case ATA_PROT_PIO_IN:
	case ATA_PROT_DMA_IN:
		return 0;
	case ATA_PROT_FPDMA:
		return (qc->flags & ATA_QCFLAG_WRITE) ||
		       qc->tf.command == ATA_CMD_FPDMA_WRITE ||
		       (qc->tf.flags & ATA_TFLAG_WRITE) ||
		       qc->dma_dir == ATA_DIR_TO_DEVICE;
	case ATA_PROT_ATAPI_PIO:
	case ATA_PROT_ATAPI_DMA:
		return qc->dma_dir == ATA_DIR_TO_DEVICE;
	default:
		return qc->dma_dir == ATA_DIR_TO_DEVICE ||
		       (qc->flags & ATA_QCFLAG_WRITE);
	}
}

/* Does the protocol move data at all? */
static int ahci_prot_has_data(enum ata_prot p)
{
	switch (p) {
	case ATA_PROT_PIO_IN:
	case ATA_PROT_PIO_OUT:
	case ATA_PROT_DMA_IN:
	case ATA_PROT_DMA_OUT:
	case ATA_PROT_FPDMA:
	case ATA_PROT_ATAPI_PIO:
	case ATA_PROT_ATAPI_DMA:
		return 1;
	default:
		return 0;
	}
}

static const char *ahci_prot_name(enum ata_prot p)
{
	switch (p) {
	case ATA_PROT_NODATA:
		return "nodata";
	case ATA_PROT_PIO_IN:
		return "pio-in";
	case ATA_PROT_PIO_OUT:
		return "pio-out";
	case ATA_PROT_DMA_IN:
		return "dma-in";
	case ATA_PROT_DMA_OUT:
		return "dma-out";
	case ATA_PROT_FPDMA:
		return "ncq";
	case ATA_PROT_ATAPI_PIO:
		return "atapi-pio";
	case ATA_PROT_ATAPI_DMA:
		return "atapi-dma";
	case ATA_PROT_ATAPI_NODATA:
		return "atapi-nodata";
	case ATA_PROT_SRST:
		return "srst";
	default:
		return "unknown";
	}
}

/* The PMP field a command to this link carries: 0 for the host link and
 * for a port without a multiplier, the fan-out port number otherwise. */
static inline uint8_t ahci_link_pmp(const struct ahci_link *link)
{
	if (!link || link->is_host)
		return 0;
	return (uint8_t)(link->pmp & FIS_PMP_MASK);
}

static inline uint64_t ahci_now_ns(void)
{
	return hrtimer_now_ns();
}

int ahci_qc_is_eh_own(const struct ahci_port *port,
		      const struct ata_queued_cmd *qc)
{
	if (!qc)
		return 0;
	/* ATA_QCFLAG_EH is only ever set by a context holding the port's
	 * eh_mutex (ata_qc_exec via ata_dev_eh_held, the EH run's polled
	 * commands, an optical drive's registration, the port's own SATA
	 * feature commands) -- inside an EH run or outside one (a
	 * management command under ata_dev_lock / ata_mgmt_enter).  Either
	 * way no EH run can recover the command for its issuer, who holds
	 * the mutex that run would need: the command must end by itself. */
	(void)port;
	return (qc->flags & ATA_QCFLAG_EH) != 0;
}

/* A device that may still be used through this port. */
static int ahci_dev_usable(const struct ata_device *dev)
{
	return dev && dev->magic == ATA_DEVICE_MAGIC &&
	       !(dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED));
}

/* ==========================================================================
 * Default timeouts
 * ========================================================================== */

uint32_t ahci_qc_default_timeout(const struct ata_queued_cmd *qc)
{
	if (!qc)
		return ATA_TMOUT_INTERNAL_MS;
	if (qc->timeout_ms)
		return qc->timeout_ms;

	switch (qc->tf.command) {
	case ATA_CMD_FLUSH:
	case ATA_CMD_FLUSH_EXT:
		return ATA_TMOUT_FLUSH_MS;
	case ATA_CMD_ID_ATA:
	case ATA_CMD_ID_ATAPI:
		return ATA_TMOUT_IDENTIFY_MS;
	case ATA_CMD_DSM:
	case ATA_CMD_DSM_XL:
	case ATA_CMD_FPDMA_SEND:
		return ATA_TMOUT_TRIM_MS;
	case ATA_CMD_SMART:
		return ATA_TMOUT_SMART_MS;
	case ATA_CMD_EDD:
		return ATA_TMOUT_DIAG_MS;
	case ATA_CMD_STANDBYNOW1:
	case ATA_CMD_STANDBY:
	case ATA_CMD_IDLEIMMEDIATE:
	case ATA_CMD_IDLE:
	case ATA_CMD_SLEEP:
		return ATA_TMOUT_STANDBY_MS;
	case ATA_CMD_SEC_ERASE_UNIT:
		return ATA_TMOUT_SEC_ERASE_MIN_MS;
	case ATA_CMD_PACKET:
		return ATA_TMOUT_ATAPI_MS;
	case ATA_CMD_SET_FEATURES:
		if (qc->tf.feature == SETFEATURES_SPINUP)
			return ATA_TMOUT_SPINUP_MS;
		return ATA_TMOUT_INTERNAL_MS;
	case ATA_CMD_FPDMA_READ:
	case ATA_CMD_FPDMA_WRITE:
	case ATA_CMD_READ_DMA:
	case ATA_CMD_READ_DMA_EXT:
	case ATA_CMD_WRITE_DMA:
	case ATA_CMD_WRITE_DMA_EXT:
	case ATA_CMD_WRITE_DMA_FUA_EXT:
	case ATA_CMD_READ_SECTORS:
	case ATA_CMD_READ_SECTORS_EXT:
	case ATA_CMD_WRITE_SECTORS:
	case ATA_CMD_WRITE_SECTORS_EXT:
	case ATA_CMD_READ_MULTI:
	case ATA_CMD_WRITE_MULTI:
	case ATA_CMD_READ_MULTIPLE_EXT:
	case ATA_CMD_WRITE_MULTIPLE_EXT:
	case ATA_CMD_VERIFY:
	case ATA_CMD_VERIFY_EXT:
	case ATA_CMD_ZERO_EXT:
		return ATA_TMOUT_RW_MS;
	default:
		break;
	}
	if (qc->flags & ATA_QCFLAG_IO)
		return ATA_TMOUT_RW_MS;
	if (ata_prot_is_atapi(qc->protocol))
		return ATA_TMOUT_ATAPI_MS;
	return ATA_TMOUT_INTERNAL_MS;
}

/* ==========================================================================
 * PRDT helpers
 *
 * AHCI 1.3.1 section 4.2.3.3: each Physical Region Descriptor names one
 * physically contiguous data region -- DBA (bit 0 reserved: word aligned)
 * and DBC (byte count minus one in bits 21:0, bit 0 always 1: even
 * lengths), bit 31 asks for a Descriptor Processed interrupt.  A region
 * holds at most 4 MiB, and the upper address dword may be non-zero only
 * when the HBA supports 64-bit addressing (CAP.S64A).  The builder itself,
 * ahci_prdt_build(), lives with the DMA translation and bounce code in
 * ahci_dma.c and is reached through ahci_dma_map(); the command engine
 * only checks the result.  No descriptor carries the I bit: the command's
 * end is signalled by its FIS (D2H Register, PIO Setup, Set Device Bits),
 * and a Descriptor Processed interrupt per command would only double the
 * interrupts.
 * ========================================================================== */

/* Consistency check of a built PRDT (debug builds call it on every
 * command): even addresses, odd DBC, sizes within limits, total. */
static int ahci_prdt_check(const struct ahci_prdt_entry *prdt, uint32_t n,
			   uint32_t nbytes, int dma64)
{
	uint64_t total = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t dbc = prdt[i].dbc & AHCI_PRD_DBC_MASK;

		if (prdt[i].dba & 1)
			return -EINVAL;
		if (!(dbc & 1))
			return -EINVAL;
		if (!dma64 && prdt[i].dbau)
			return -ERANGE;
		if (prdt[i].rsvd)
			return -EINVAL;
		total += (uint64_t)dbc + 1;
	}
	return total == nbytes ? 0 : -EINVAL;
}

/* ==========================================================================
 * Slot / tag allocation
 * ========================================================================== */

/* Choose a free slot.  Pure: `busy' is the allocated bitmap.
 *   internal: the internal slot (0) -- error handling's own commands,
 *             issued while EH owns the port and every slot is free.
 *   ncq:      tags 0 .. ncq_depth-1, all of the HBA's slots, next-fit
 *             from `hint' so the tags rotate through the queue.
 *   else:     the lowest free slot.
 * No slot is reserved.  Returns the slot or -EBUSY. */
static int ahci_slot_pick(uint32_t busy, uint32_t nr_slots,
			  uint32_t internal_slot, uint32_t ncq_depth, int ncq,
			  int internal, uint32_t hint)
{
	uint32_t all, usable, free;

	if (nr_slots == 0 || nr_slots > AHCI_MAX_SLOTS)
		return -EBUSY;
	all = ahci_slot_mask(nr_slots);

	if (internal) {
		if (internal_slot < nr_slots && !(busy & ahci_bit(internal_slot)))
			return (int)internal_slot;
		return -EBUSY;
	}

	usable = all;

	if (ncq) {
		uint32_t depth = ncq_depth;

		if (depth > nr_slots)
			depth = nr_slots;
		if (depth == 0)
			return -EBUSY;
		usable &= ahci_slot_mask(depth);
		free = usable & ~busy;
		if (!free)
			return -EBUSY;
		for (uint32_t k = 0; k < depth; k++) {
			uint32_t s = (hint + k) % depth;

			if (free & ahci_bit(s))
				return (int)s;
		}
		return -EBUSY; /* unreachable: free was non-zero */
	}

	free = usable & ~busy;
	if (!free)
		return -EBUSY;
	return (int)ahci_ffs32(free);
}

/* The NCQ tag range of a queued command: the port's tag space, further
 * limited by the queue depth the issuing device reported in IDENTIFY word
 * 75 -- an FPDMA QUEUED tag must be lower than the device's depth, and the
 * rotating tag hint would otherwise walk every tag the HBA has even with
 * only one command in flight.  Behind a port multiplier every device gets
 * its own limit out of the shared tag space. */
static uint32_t ahci_qc_ncq_depth(const struct ahci_port *port,
				  const struct ata_queued_cmd *qc)
{
	uint32_t depth = port->ncq_depth;
	uint32_t dev_depth = 0;

	if (qc && qc->dev && qc->dev->magic == ATA_DEVICE_MAGIC) {
		dev_depth = qc->dev->ncq_depth;
		if (!dev_depth)
			dev_depth = qc->dev->ncq_depth_dev;
	}
	if (dev_depth && dev_depth < depth)
		depth = dev_depth;
	return depth;
}

/* ahci_slot_alloc_locked with an explicit NCQ tag range (`ncq_depth',
 * at most port->ncq_depth). */
static int ahci_slot_alloc_depth_locked(struct ahci_port *port, int ncq,
					int internal, uint32_t ncq_depth)
{
	struct ahci_cmd *cmd;
	int slot;

	BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	if (ncq_depth > port->ncq_depth || !ncq_depth)
		ncq_depth = port->ncq_depth;

	slot = ahci_slot_pick(port->slot_map, port->nr_slots,
			      port->internal_slot, ncq_depth, ncq, internal,
			      ncq_depth ? port->tag_hint % ncq_depth : 0);
	if (slot < 0)
		return -EBUSY;

	cmd = &port->cmds[slot];
	/* The bitmap and the per-slot state must agree: a slot the bitmap
	 * calls free but whose command is still alive is a double use. */
	BUG_ON(port->slot_map & ahci_bit((uint32_t)slot));
	BUG_ON(cmd->state != AHCI_CMD_FREE);
	BUG_ON(cmd->qc != NULL);
	BUG_ON(port->active & ahci_bit((uint32_t)slot));
	if (WARN_ON_ONCE(cmd->slot != (uint32_t)slot))
		cmd->slot = (uint32_t)slot;

	port->slot_map |= ahci_bit((uint32_t)slot);
	cmd->state = AHCI_CMD_ALLOCATED;
	cmd->is_ncq = ncq ? 1 : 0;
	cmd->internal = internal ? 1 : 0;
	cmd->pmp = 0;
	cmd->n_prd = 0;
	cmd->nbytes = 0;
	cmd->start_ns = 0;
	cmd->timeout_ms = 0;
	cmd->generation++;
	if (ncq && ncq_depth)
		port->tag_hint = ((uint32_t)slot + 1) % ncq_depth;
	return slot;
}

int ahci_slot_alloc_locked(struct ahci_port *port, int ncq, int internal)
{
	BUG_ON(!port);
	return ahci_slot_alloc_depth_locked(port, ncq, internal,
					    port->ncq_depth);
}

void ahci_slot_free_locked(struct ahci_port *port, uint32_t slot)
{
	struct ahci_cmd *cmd;
	uint32_t bit;

	BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	BUG_ON(slot >= port->nr_slots || slot >= AHCI_MAX_SLOTS);

	bit = ahci_bit(slot);
	cmd = &port->cmds[slot];
	BUG_ON(!(port->slot_map & bit)); /* double free */
	BUG_ON(port->active & bit); /* freeing a command the HBA owns */
	BUG_ON(port->ncq_active & bit);
	WARN_ON(cmd->bounce.area.size != 0); /* bounce buffer not unmapped */

	cmd->state = AHCI_CMD_FREE;
	cmd->qc = NULL;
	cmd->is_ncq = 0;
	cmd->internal = 0;
	cmd->pmp = 0;
	cmd->n_prd = 0;
	cmd->nbytes = 0;
	cmd->timeout_ms = 0;
	port->slot_map &= ~bit;
	port->zombie_slots &= ~bit;
	port->fault_slots &= ~bit;
}

/* ==========================================================================
 * Received FIS area and result task files
 * ========================================================================== */

struct ahci_rx_fis *ahci_rx_fis_of(struct ahci_port *port, int pmp)
{
	if (!port || !port->rx_fis)
		return NULL;
	if (port->fbs_enabled && pmp >= 0 && pmp < 16)
		return &((struct ahci_rx_fis_fbs *)port->rx_fis)->dev[pmp];
	return (struct ahci_rx_fis *)port->rx_fis;
}

/* Copy bytes the HBA wrote.  Every byte is a volatile load so the
 * compiler can neither cache nor merge them across the caller's fence. */
static void ahci_fis_copy(void *dst, const volatile void *src, size_t n)
{
	const volatile uint8_t *s = (const volatile uint8_t *)src;
	uint8_t *d = (uint8_t *)dst;

	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
}

/* Forget the response FISes of the previous command (non-queued commands
 * only: there is exactly one of them in flight then), so that after
 * completion whichever FIS carries a valid type byte is this command's. */
static void ahci_rx_fis_forget(struct ahci_port *port, int pmp)
{
	struct ahci_rx_fis *rx = ahci_rx_fis_of(port, pmp);

	if (!rx)
		return;
	((volatile struct ahci_rx_fis *)rx)->rfis.fis_type = 0;
	((volatile struct ahci_rx_fis *)rx)->psfis.fis_type = 0;
}

/* The result taskfile from the received-FIS area.  `check_tfd': PxTFD is
 * read as well and its error bits win over a FIS that shows none (error
 * handling).  Without it -- a command the HBA finished without error --
 * the FIS is the result and PxTFD is read only when the device sent none,
 * so a clean completion costs no register access beyond PxCI/PxSACT. */
static void ahci_cmd_result_tf(struct ahci_port *port, int pmp,
			       enum ata_prot prot, int ncq,
			       struct ata_taskfile *tf, int check_tfd)
{
	struct ahci_rx_fis *rx = ahci_rx_fis_of(port, pmp);
	uint32_t tfd = check_tfd ? ahci_port_readl(port, AHCI_PxTFD) : 0;
	uint8_t tfd_status, tfd_error;
	int have = 0;

	/* the PxCI/PxSACT read that saw completion comes first */
	ahci_dma_sync_rx_for_cpu(port);

	if (rx && ncq) {
		struct fis_set_device_bits sdb;

		ahci_fis_copy(&sdb, &rx->sdbfis, sizeof(sdb));
		if (sdb.fis_type == FIS_TYPE_SDB) {
			/* status-hi in 6:4, status-lo in 2:0; BSY and DRQ are
			 * never carried and are zero by definition */
			tf->status = (uint8_t)(sdb.status & 0x77);
			tf->error = sdb.error;
			have = 1;
		}
	} else if (rx) {
		struct fis_reg_d2h d2h;
		struct fis_pio_setup ps;

		ahci_fis_copy(&d2h, &rx->rfis, sizeof(d2h));
		ahci_fis_copy(&ps, &rx->psfis, sizeof(ps));
		if (d2h.fis_type == FIS_TYPE_REG_D2H) {
			ata_tf_from_fis(&d2h, tf);
			have = 1;
		} else if (ps.fis_type == FIS_TYPE_PIO_SETUP &&
			   (prot == ATA_PROT_PIO_IN || prot == ATA_PROT_PIO_OUT ||
			    prot == ATA_PROT_ATAPI_PIO)) {
			/* A PIO data-in command ends with the last PIO Setup
			 * FIS: its E_Status is the final status. */
			tf->status = ps.e_status;
			tf->error = ps.error;
			tf->lbal = ps.lba0;
			tf->lbam = ps.lba1;
			tf->lbah = ps.lba2;
			tf->device = ps.device;
			tf->hob_lbal = ps.lba3;
			tf->hob_lbam = ps.lba4;
			tf->hob_lbah = ps.lba5;
			tf->nsect = ps.count_lo;
			tf->hob_nsect = ps.count_hi;
			have = 1;
		}
	}

	if (have && !check_tfd)
		return;
	if (!check_tfd)
		tfd = ahci_port_readl(port, AHCI_PxTFD);
	tfd_status = (uint8_t)(tfd & AHCI_PxTFD_STS_MASK);
	tfd_error =
		(uint8_t)((tfd & AHCI_PxTFD_ERR_MASK) >> AHCI_PxTFD_ERR_SHIFT);

	/* PxTFD is the HBA's shadow of the device status: authoritative for
	 * the error bits, and the only source when no FIS was kept. */
	if (!have) {
		tf->status = tfd_status;
		tf->error = tfd_error;
	} else if ((tfd_status & (ATA_ERR | ATA_DF)) &&
		   !(tf->status & (ATA_ERR | ATA_DF))) {
		tf->status = tfd_status;
		tf->error = tfd_error;
	}
}

void ahci_cmd_read_result_tf(struct ahci_port *port, int pmp,
			     enum ata_prot prot, int ncq,
			     struct ata_taskfile *tf)
{
	ahci_cmd_result_tf(port, pmp, prot, ncq, tf, 1);
}

/* ==========================================================================
 * Command construction
 * ========================================================================== */

/* Build the Register H2D FIS of qc for slot `slot' (AHCI 1.3.1 section
 * 4.2.3.1, SATA 3.x section 10.5.5). */
static void ahci_cmd_build_fis(const struct ata_queued_cmd *qc, uint32_t slot,
			       uint8_t pmp, struct fis_reg_h2d *fis)
{
	ata_tf_to_fis(&qc->tf, pmp, 1, fis);

	if (ahci_qc_is_ncq(qc)) {
		/* First-party DMA queued commands carry the tag in count
		 * bits 7:3; bits 2:0 are reserved (PRIO lives in the high
		 * count byte, which the ATA layer filled). */
		fis->count_lo = (uint8_t)((qc->tf.nsect & 0x07) | (slot << 3));
		if (WARN_ON_ONCE(!(fis->device & ATA_LBA)))
			fis->device |= ATA_LBA; /* FPDMA requires bit 6 */
	}
	/* The SRST bit belongs to the reset sequences only. */
	if (WARN_ON_ONCE(fis->control & ATA_SRST))
		fis->control &= (uint8_t)~ATA_SRST;

	if (ata_prot_is_atapi(qc->protocol)) {
		if (WARN_ON_ONCE(fis->command != ATA_CMD_PACKET))
			fis->command = ATA_CMD_PACKET;
		if (qc->protocol == ATA_PROT_ATAPI_DMA) {
			fis->feature_lo |= ATAPI_PKT_DMA;
			if ((qc->flags & ATA_QCFLAG_DMADIR) &&
			    !ahci_qc_is_write(qc))
				fis->feature_lo |= ATAPI_DMADIR;
		} else {
			fis->feature_lo &=
				(uint8_t)~(ATAPI_PKT_DMA | ATAPI_DMADIR);
		}
		/* PIO: the byte count limit per DRQ block lives in LBA
		 * mid/high.  Zero there makes some drives transfer nothing;
		 * use the transfer size (even, at most 0xFFFE). */
		if (qc->protocol == ATA_PROT_ATAPI_PIO && fis->lba1 == 0 &&
		    fis->lba2 == 0) {
			uint32_t lim = qc->nbytes;

			if (lim == 0 || lim > 0xFFFE)
				lim = 0xFFFE;
			lim &= ~1u;
			fis->lba1 = (uint8_t)lim;
			fis->lba2 = (uint8_t)(lim >> 8);
		}
	}
}

int ahci_cmd_prepare_locked(struct ahci_port *port, struct ahci_cmd *cmd,
			    struct ata_queued_cmd *qc)
{
	struct ahci_link *link;
	struct ahci_cmd_table *t;
	struct ahci_cmd_header *h;
	struct fis_reg_h2d fis;
	int atapi, write, has_data;
	uint32_t opts, n_prd = 0;
	uint8_t pmp;

	BUG_ON(!port || !cmd || !qc);
	lockdep_assert_held(&port->lock);
	BUG_ON(cmd->state != AHCI_CMD_ALLOCATED);

	link = ahci_qc_link(qc);
	if (!link)
		return -ENODEV;
	t = cmd->table;
	h = cmd->hdr;
	if (WARN_ON_ONCE(!t || !h))
		return -ENODEV; /* the port's DMA areas are missing */

	atapi = ata_prot_is_atapi(qc->protocol);
	write = ahci_qc_is_write(qc);
	has_data = qc->nbytes && qc->n_elem && qc->sg;
	pmp = ahci_link_pmp(link);

	/* Command FIS */
	ahci_cmd_build_fis(qc, cmd->slot, pmp, &fis);
	kmemset(t->cfis, 0, sizeof(t->cfis));
	kmemcpy(t->cfis, &fis, sizeof(fis));

	/* ATAPI command (the packet), zero padded to 16 bytes */
	kmemset(t->acmd, 0, sizeof(t->acmd));
	if (atapi) {
		uint32_t len = qc->cdb_len;

		if (len == 0 || len > ATAPI_CDB_LEN_MAX)
			return -EINVAL;
		kmemcpy(t->acmd, qc->cdb, len);
	}

	/* Data: the PRDT (and a bounce buffer where the HBA cannot reach
	 * the memory) */
	cmd->n_prd = 0;
	cmd->nbytes = 0;
	if (has_data) {
		int r = ahci_dma_map(port, cmd, qc->sg, qc->n_elem, write);

		if (r < 0)
			return r;
		n_prd = cmd->n_prd;
		BUG_ON(n_prd > AHCI_MAX_PRD);
		if (cmd->nbytes != qc->nbytes)
			WARN_RATELIMIT(1,
				       AHCI_PFX "%s: slot %u maps %u bytes, command wants %u",
				       port->name, cmd->slot, cmd->nbytes,
				       qc->nbytes);
		VM_WARN_ON(ahci_prdt_check(t->prdt, n_prd, cmd->nbytes,
					   port->hba->dma64) != 0);
	}

	/* Command header (section 4.2.2) */
	opts = (FIS_H2D_LEN / 4) & AHCI_CMDH_CFL_MASK;
	opts |= n_prd << AHCI_CMDH_PRDTL_SHIFT;
	opts |= ((uint32_t)pmp << AHCI_CMDH_PMP_SHIFT) & AHCI_CMDH_PMP_MASK;
	if (write && has_data)
		opts |= AHCI_CMDH_W;
	if (atapi)
		opts |= AHCI_CMDH_A;
	if (ahci_use_prefetch && has_data && !atapi && !ahci_qc_is_ncq(qc))
		opts |= AHCI_CMDH_P;
	h->opts = opts;
	h->prdbc = 0;
	if (!port->hba->dma64 && (cmd->table_bus >> 32)) {
		WARN_ON_ONCE(1); /* table placed above 4 GiB on a 32-bit HBA */
		if (has_data)
			ahci_dma_unmap(port, cmd, write);
		cmd->n_prd = 0;
		return -EINVAL;
	}
	h->ctba = (uint32_t)cmd->table_bus;
	h->ctbau = (uint32_t)(cmd->table_bus >> 32);
	h->rsvd[0] = h->rsvd[1] = h->rsvd[2] = h->rsvd[3] = 0;

	cmd->pmp = pmp;
	cmd->qc = qc;
	if (!ahci_qc_is_ncq(qc))
		ahci_rx_fis_forget(port, pmp);
	return 0;
}

/* ==========================================================================
 * Deferred completions and zombie slots
 * ========================================================================== */

void ahci_cmd_defer_complete_locked(struct ahci_port *port,
				    struct ata_queued_cmd *qc)
{
	lockdep_assert_held(&port->lock);
	BUG_ON(!qc);
	qc->xport_next = NULL;
	if (port->cpl_tail)
		port->cpl_tail->xport_next = qc;
	else
		port->cpl_head = qc;
	port->cpl_tail = qc;
}

/* Complete a chain linked through xport_next.  No transport lock held (the
 * pollers, error handling, the fail paths).  One completion section
 * around the whole chain: a release the first command's completion causes
 * waits until the walk -- which still holds the next commands -- is over;
 * and a block request ended inline (a poller with interrupts off) or
 * collected by error handling stays inside it. */
int ahci_complete_chain(struct ata_queued_cmd *qc)
{
	struct blk_cpl_ctx cpl;
	int n = 0;

	if (!qc)
		return 0;
	blk_cpl_begin(&cpl);
	while (qc) {
		struct ata_queued_cmd *next = qc->xport_next;

		qc->xport_next = NULL;
		ata_qc_complete(qc);
		qc = next;
		n++;
	}
	blk_cpl_end(&cpl);
	return n;
}

int ahci_cmd_run_deferred_completions(struct ahci_port *port)
{
	struct ata_queued_cmd *head;
	uint64_t fl;

	if (!port || !port->cpl_head)
		return 0;
	spin_lock_irqsave(&port->lock, &fl);
	head = port->cpl_head;
	port->cpl_head = port->cpl_tail = NULL;
	spin_unlock_irqrestore(&port->lock, fl);
	return ahci_complete_chain(head);
}

struct ata_queued_cmd *ahci_cmd_zombify_locked(struct ahci_port *port,
					       struct ahci_cmd *cmd)
{
	struct ata_queued_cmd *qc;
	uint32_t bit;

	lockdep_assert_held(&port->lock);
	BUG_ON(!cmd || cmd->slot >= port->nr_slots);
	bit = ahci_bit(cmd->slot);
	BUG_ON(!(port->slot_map & bit));

	qc = cmd->qc;
	cmd->qc = NULL;
	cmd->state = AHCI_CMD_EH;
	port->active &= ~bit;
	port->ncq_active &= ~bit;
	port->fault_slots &= ~bit;
	port->zombie_slots |= bit;
	if (qc)
		qc->xport_cmd = NULL;
	return qc;
}

static uint32_t ahci_errno_to_mask(int err);

/* The zombie slot of `cmd' is free: the command held for it (if any) is
 * completed with -ECANCELED -- requeued by its issuer.  port->lock held. */
static void ahci_cmd_hand_back_held_locked(struct ahci_port *port,
					   struct ahci_cmd *cmd)
{
	struct ata_queued_cmd *qc = cmd->held_qc;

	lockdep_assert_held(&port->lock);
	if (!qc)
		return;
	cmd->held_qc = NULL;
	port->held_slots &= ~ahci_bit(cmd->slot);
	qc->flags &= ~ATA_QCFLAG_HW_OWNED;
	qc->flags |= ATA_QCFLAG_ABORTED;
	qc->result = -ECANCELED;
	qc->err_mask |= ahci_errno_to_mask(-ECANCELED);
	ahci_cmd_defer_complete_locked(port, qc);
}

uint32_t ahci_cmd_reap_zombies_locked(struct ahci_port *port)
{
	uint32_t ci, sact, pxcmd, free;

	lockdep_assert_held(&port->lock);
	if (!port->zombie_slots)
		return 0;

	pxcmd = ahci_port_readl(port, AHCI_PxCMD);
	if (port->hba->dma_stopped ||
	    !(pxcmd & (AHCI_PxCMD_ST | AHCI_PxCMD_CR))) {
		/* Clearing ST cleared PxCI and PxSACT (or the controller can
		 * do no DMA at all any more): nothing is owned. */
		ci = 0;
		sact = 0;
	} else {
		ci = ahci_port_readl(port, AHCI_PxCI);
		sact = ahci_port_readl(port, AHCI_PxSACT);
		if (ci == 0xFFFFFFFFu && sact == 0xFFFFFFFFu)
			return port->zombie_slots; /* controller gone */
	}

	free = port->zombie_slots & ~(ci | sact);
	/* A slot held for a command the device may still run stays until
	 * the controller reset (or a controller that can do no DMA): clear
	 * registers after an engine stop say nothing about the device, and
	 * its tag must not go to another command meanwhile. */
	if (!port->hba->dma_stopped)
		free &= ~port->held_slots;
	while (free) {
		uint32_t s = ahci_ffs32(free);
		struct ahci_cmd *cmd = &port->cmds[s];

		free &= ~ahci_bit(s);
		port->zombie_slots &= ~ahci_bit(s);
		/* The qc is long completed (or held, and its data is not
		 * wanted any more): pass "write" so a read bounce is freed
		 * without copying back. */
		if (cmd->n_prd || cmd->bounce.area.size)
			ahci_dma_unmap(port, cmd, 1);
		cmd->n_prd = 0;
		/* The slot is confirmed free: a command held for it goes
		 * back for requeueing, its buffer the issuer's again. */
		ahci_cmd_hand_back_held_locked(port, cmd);
		ahci_slot_free_locked(port, s);
	}
	return port->zombie_slots;
}

void ahci_port_dma_done(struct ahci_port *port)
{
	uint32_t z;
	uint64_t fl;

	BUG_ON(!port);
	spin_lock_irqsave(&port->lock, &fl);
	z = port->zombie_slots;
	while (z) {
		uint32_t s = ahci_ffs32(z);
		struct ahci_cmd *cmd = &port->cmds[s];

		z &= ~ahci_bit(s);
		port->zombie_slots &= ~ahci_bit(s);
		if (cmd->n_prd || cmd->bounce.area.size)
			ahci_dma_unmap(port, cmd, 1);
		cmd->n_prd = 0;
		ahci_cmd_hand_back_held_locked(port, cmd);
		ahci_slot_free_locked(port, s);
	}
	spin_unlock_irqrestore(&port->lock, fl);
	(void)ahci_cmd_run_deferred_completions(port);
}

struct ata_queued_cmd *ahci_cmd_retire_locked(struct ahci_port *port,
					      struct ahci_cmd *cmd)
{
	struct ata_queued_cmd *qc;
	int write;

	lockdep_assert_held(&port->lock);
	BUG_ON(!cmd);
	BUG_ON(port->active & ahci_bit(cmd->slot));

	qc = cmd->qc;
	if (qc)
		write = ahci_qc_is_write(qc);
	else
		write = cmd->hdr ? !!(cmd->hdr->opts & AHCI_CMDH_W) : 1;
	if (cmd->n_prd || cmd->bounce.area.size)
		ahci_dma_unmap(port, cmd, write);
	cmd->n_prd = 0;
	if (qc)
		qc->xport_cmd = NULL;
	cmd->qc = NULL;
	ahci_slot_free_locked(port, cmd->slot);
	return qc;
}

/* err -> the AC_ERR_* class a transport-level failure means */
static uint32_t ahci_errno_to_mask(int err)
{
	switch (err) {
	case 0:
		return 0;
	case -ENODEV:
		return AC_ERR_NODEV_HINT;
	case -ECANCELED:
	case -EAGAIN:
		return AC_ERR_ABORTED;
	case -ETIMEDOUT:
		return AC_ERR_TIMEOUT;
	case -ENOMEM:
	case -EFAULT:
	case -ERANGE:
		return AC_ERR_SYSTEM;
	case -EINVAL:
	case -E2BIG:
		return AC_ERR_INVALID;
	default:
		return AC_ERR_OTHER;
	}
}

int ahci_cmd_eh_fail_inflight_locked(struct ahci_port *port, uint32_t mask,
				     int err, uint32_t err_mask)
{
	uint32_t todo;
	int n = 0;

	lockdep_assert_held(&port->lock);
	todo = mask & port->active;
	while (todo) {
		uint32_t s = ahci_ffs32(todo);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *qc;

		todo &= ~ahci_bit(s);
		if (!cmd->qc)
			continue; /* a raw FIS: its poller handles it */
		qc = cmd->qc;
		/* The result registers of a failed command from the
		 * received-FIS area: the Set Device Bits FIS of a queued
		 * one, the D2H Register FIS otherwise.  No register is
		 * read. */
		qc->result_tf.flags =
			qc->tf.flags & (ATA_TFLAG_LBA48 | ATA_TFLAG_LBA);
		ahci_dma_sync_rx_for_cpu(port);
		{
			struct ahci_rx_fis *rx = ahci_rx_fis_of(port, cmd->pmp);

			if (rx && cmd->is_ncq) {
				const volatile struct ahci_rx_fis *v = rx;

				qc->result_tf.status = v->sdbfis.status;
				qc->result_tf.error = v->sdbfis.error;
			} else if (rx) {
				struct fis_reg_d2h f;

				ahci_fis_copy(&f, &rx->rfis, sizeof(f));
				ata_tf_from_fis(&f, &qc->result_tf);
			}
		}
		qc->err_mask |= err_mask | ahci_errno_to_mask(err);
		qc->result = err;
		if (err == -ETIMEDOUT)
			qc->flags |= ATA_QCFLAG_TIMEDOUT;
		ahci_trace(port, err == -ETIMEDOUT ? AHCI_TRACE_TIMEOUT
						    : AHCI_TRACE_ERROR,
			   cmd, err, 0);
		/* Whether the HBA still holds the slot is not asked: it stays
		 * allocated until the next engine stop (the issuer's next
		 * command, the end of its wait or of the EH callee kicks the
		 * engine), and the command's buffer counts as the HBA's
		 * until then. */
		ahci_cmd_zombify_locked(port, cmd);
		qc->flags |= ATA_QCFLAG_HW_OWNED;
		ahci_cmd_defer_complete_locked(port, qc);
		n++;
	}
	return n;
}

uint32_t ahci_cmd_eh_abort_locked(struct ahci_port *port, int own_err,
				  uint32_t own_err_mask)
{
	uint32_t act, own = 0, others = 0;

	lockdep_assert_held(&port->lock);
	act = port->active;
	while (act) {
		uint32_t s = ahci_ffs32(act);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *qc = cmd->qc;

		act &= ~ahci_bit(s);
		if (!qc)
			continue; /* a raw FIS: its poller sees the error */
		if (ahci_qc_is_eh_own(port, qc)) {
			own |= ahci_bit(s);
			continue;
		}
		others |= ahci_bit(s);
		if (cmd->state == AHCI_CMD_TIMEDOUT)
			qc->flags |= ATA_QCFLAG_TIMEDOUT;
		cmd->state = AHCI_CMD_EH;
	}
	if (own)
		(void)ahci_cmd_eh_fail_inflight_locked(port, own, own_err,
						       own_err_mask);
	return others;
}

int ahci_eh_drained_locked(const struct ahci_port *port)
{
	uint32_t act;

	lockdep_assert_held(&port->lock);
	/* A slot carrying an injected fault never reached the HBA: it can
	 * only fail. */
	act = port->active & ~port->fault_slots;
	while (act) {
		uint32_t s = ahci_ffs32(act);
		const struct ahci_cmd *cmd = &port->cmds[s];

		act &= ~ahci_bit(s);
		if (cmd->qc && cmd->state == AHCI_CMD_ISSUED)
			return 0;
	}
	return 1;
}

uint32_t ahci_cmd_claim_active_locked(struct ahci_port *port)
{
	uint32_t act, claimed = 0, ci = 0;
	int ci_read = 0;

	lockdep_assert_held(&port->lock);
	act = port->active;
	while (act) {
		uint32_t s = ahci_ffs32(act);
		struct ahci_cmd *cmd = &port->cmds[s];

		act &= ~ahci_bit(s);
		if (!cmd->qc) {
			/* A raw FIS: its poller (ahci_exec_raw_fis) watches
			 * PxCI itself and would take the clear of the coming
			 * stop for its completion.  Still pending in PxCI ->
			 * marked AHCI_CMD_EH, which the poller reports as an
			 * abort; a bit already clear finished for real. */
			if (!ci_read) {
				ci = ahci_port_readl(port, AHCI_PxCI);
				ci_read = 1;
			}
			if ((ci & ahci_bit(s)) && cmd->state == AHCI_CMD_ISSUED) {
				cmd->state = AHCI_CMD_EH;
				claimed |= ahci_bit(s);
			}
			continue;
		}
		if (cmd->state == AHCI_CMD_TIMEDOUT)
			cmd->qc->flags |= ATA_QCFLAG_TIMEDOUT;
		/* ALLOCATED never reaches `active'; COMPLETING has left it
		 * already.  Anything else here is the HBA's or EH's. */
		VM_WARN_ON(cmd->state != AHCI_CMD_ISSUED &&
			   cmd->state != AHCI_CMD_TIMEDOUT &&
			   cmd->state != AHCI_CMD_EH);
		cmd->state = AHCI_CMD_EH;
		claimed |= ahci_bit(s);
	}
	return claimed;
}

/* ==========================================================================
 * Timeouts
 *
 * The command engine keeps no timer.  A command's timeout is watched by
 * whoever issued it: the block queue's timeout work for block requests
 * (one coarse timer per queue, kblockd) and the bounded wait of an
 * internal command's waiter (ata_qc_exec).  Either calls the transport's
 * qc_timed_out (ahci_xport_qc_timed_out), which finds the command under
 * port->lock and hands it to the error handler here.
 * ========================================================================== */

/* `cmd' (ISSUED) passed its timeout.  It is marked so the EH run
 * classifies it as timed out, and EH is scheduled -- without freezing:
 * the other commands in flight finish (or time out) first, the port
 * admits nothing new meanwhile, and the EH run freezes the port when it
 * starts (ahci_eh_run_locked).  A command issued under the port's
 * eh_mutex (EH's own callees: IDENTIFY of a revalidation, a PMP
 * register access ...; a management command under ata_dev_lock) has as its issuer the
 * mutex holder, blocked on it, and no EH run could recover it for them:
 * the port is frozen at once and everything in flight aborted -- the
 * holder's command completed now with -ETIMEDOUT (parked on cpl_head for
 * the caller to complete after dropping the lock, its slot left to the
 * next engine stop), the others handed to EH.  port->lock held. */
static void ahci_cmd_timed_out_locked(struct ahci_port *port,
				      struct ahci_cmd *cmd)
{
	struct ata_queued_cmd *qc = cmd->qc;
	uint32_t s = cmd->slot;

	lockdep_assert_held(&port->lock);
	VM_BUG_ON(cmd->state != AHCI_CMD_ISSUED);
	cmd->state = AHCI_CMD_TIMEDOUT;
	port->stats.timeouts++;
	if (qc) {
		qc->flags |= ATA_QCFLAG_TIMEDOUT;
		if (qc->dev && qc->dev->magic == ATA_DEVICE_MAGIC)
			qc->dev->n_timeouts++;
		WARN_RATELIMIT(1,
			       AHCI_PFX "%s: slot %u %s (0x%02x, %s) timed out after %u ms%s",
			       port->name, s, ata_cmd_name(qc->tf.command),
			       qc->tf.command, ahci_prot_name(qc->protocol),
			       cmd->timeout_ms,
			       port->eh_running ? " (during EH)" : "");
	}
	ahci_trace(port, AHCI_TRACE_TIMEOUT, cmd, -ETIMEDOUT, 0);
	if (ahci_qc_is_eh_own(port, qc)) {
		ahci_eh_freeze_locked(port);
		(void)ahci_cmd_eh_fail_inflight_locked(port, ahci_bit(s),
						       -ETIMEDOUT,
						       AC_ERR_TIMEOUT);
		(void)ahci_cmd_eh_abort_locked(port, -EIO, AC_ERR_OTHER);
		ahci_eh_schedule_locked(port, AHCI_EH_TIMEOUT);
		return;
	}
	/* Also while EH runs: the run may be past its last round already,
	 * and the scheduling makes sure another one looks at the timed-out
	 * command (scheduling during EH neither freezes nor re-enters). */
	ahci_eh_schedule_locked(port, AHCI_EH_TIMEOUT);
}

/* Does the command `c' (in flight here, port->lock held) carry the
 * expired request `req'?  The block layer names an expired request by
 * its address, picked by its timeout scan; by the time the transport
 * looks, that request may have completed and its memory -- a request on
 * a submitter's stack, a queue's own flush request -- been dispatched
 * again, now carried by `c'.  The queue's tag table tells the two apart:
 * the scan marks the tag's use it found expired (BLK_TAG_TIMED_OUT)
 * before asking the driver, and every new dispatch starts a new use of
 * a tag in flight.  `c' holding the request keeps the request and its
 * tag use alive while they are read. */
static int ahci_cmd_req_expired(const struct ata_queued_cmd *c,
				const struct blk_request *req)
{
	const struct blk_queue *q;
	int tag;

	if (c->req != req)
		return 0;
	q = req->queue;
	tag = __atomic_load_n(&req->bl.tag, __ATOMIC_RELAXED);
	if (!q || tag < 0 || tag >= BLK_QUEUE_MAX_TAGS)
		return 0;
	if (__atomic_load_n(&q->tags[tag].rq, __ATOMIC_ACQUIRE) != req)
		return 0;
	return (__atomic_load_n(&q->tags[tag].word, __ATOMIC_ACQUIRE) & 3u) ==
	       BLK_TAG_TIMED_OUT;
}

/* Find the command `qc' -- or, with qc NULL, the command carrying the
 * expired block request `req' -- or with both NULL every command of
 * `dev', and time it out if it is still at the hardware.  Returns the
 * number of commands found in flight. */
static int ahci_cmd_timeout_match(struct ahci_port *port,
				  struct ata_device *dev,
				  const struct ata_queued_cmd *qc,
				  const struct blk_request *req)
{
	uint64_t fl;
	uint32_t act;
	int found = 0;

	spin_lock_irqsave(&port->lock, &fl);
	/* The deferred command timed out waiting for its turn: it ends with
	 * -ETIMEDOUT, and error handling is scheduled without freezing the
	 * port -- nothing is in error at the hardware, but the port takes
	 * nothing new until EH has looked, or the command's issuer would
	 * keep it busy. */
	if (port->deferred_qc) {
		struct ata_queued_cmd *c = port->deferred_qc;

		if (qc ? c == qc :
		    req ? ahci_cmd_req_expired(c, req) : c->dev == dev) {
			found++;
			WARN_RATELIMIT(1,
				       AHCI_PFX "%s: %s (0x%02x) timed out waiting for the queued commands",
				       port->name, ata_cmd_name(c->tf.command),
				       c->tf.command);
			ahci_cmd_requeue_deferred_locked(port, -ETIMEDOUT);
			ahci_eh_schedule_locked(port, AHCI_EH_PORT);
			if (qc || req) {
				spin_unlock_irqrestore(&port->lock, fl);
				ahci_cmd_run_deferred_completions(port);
				return found;
			}
		}
	}
	act = port->active;
	while (act) {
		uint32_t s = ahci_ffs32(act);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *c = cmd->qc;

		act &= ~ahci_bit(s);
		if (!c)
			continue; /* a raw FIS: its poller times it */
		if (qc ? c != qc :
		    req ? !ahci_cmd_req_expired(c, req) : c->dev != dev)
			continue;
		found++;
		if (cmd->state == AHCI_CMD_ISSUED)
			ahci_cmd_timed_out_locked(port, cmd);
		if (qc || req)
			break;
	}
	spin_unlock_irqrestore(&port->lock, fl);
	/* A command of the mutex holder completed above. */
	ahci_cmd_run_deferred_completions(port);
	return found;
}

/* ==========================================================================
 * Submission
 * ========================================================================== */

/* Argument checks that need no lock. */
static int ahci_qc_validate(const struct ahci_port *port,
			    const struct ata_queued_cmd *qc)
{
	int ncq = ahci_qc_is_ncq(qc);

	switch (qc->protocol) {
	case ATA_PROT_NODATA:
	case ATA_PROT_PIO_IN:
	case ATA_PROT_PIO_OUT:
	case ATA_PROT_DMA_IN:
	case ATA_PROT_DMA_OUT:
	case ATA_PROT_FPDMA:
	case ATA_PROT_ATAPI_PIO:
	case ATA_PROT_ATAPI_DMA:
	case ATA_PROT_ATAPI_NODATA:
	case ATA_PROT_SRST:
		break;
	default:
		return -EINVAL;
	}

	if (ahci_prot_has_data(qc->protocol)) {
		int non_data_ncq = ncq && (qc->tf.command == ATA_CMD_NCQ_NON_DATA);

		if (!non_data_ncq &&
		    (qc->nbytes == 0 || qc->n_elem == 0 || !qc->sg))
			return -EINVAL;
	} else if (qc->protocol != ATA_PROT_SRST && qc->nbytes) {
		return -EINVAL; /* data on a no-data protocol */
	}
	if (qc->nbytes & (AHCI_PRD_ALIGN - 1))
		return -EINVAL; /* the PRDT can only describe even counts */
	if (qc->n_elem > 0x10000u)
		return -EINVAL;
	if (ata_prot_is_atapi(qc->protocol) &&
	    (qc->cdb_len == 0 || qc->cdb_len > ATAPI_CDB_LEN_MAX))
		return -EINVAL;
	if (ncq) {
		if (qc->flags & ATA_QCFLAG_EH)
			return -EINVAL; /* EH never queues */
		if (!port->hba->caps.sncq || port->ncq_depth == 0 ||
		    (port->flags & AHCI_PORT_F_NO_NCQ) ||
		    (port->hba->quirks & AHCI_Q_NO_NCQ))
			return -EINVAL;
	}
	return 0;
}

/* Another PMP link than `pmp' has commands in flight (command-based
 * switching allows one device at a time). */
static int ahci_cmd_pmp_conflict_locked(struct ahci_port *port, uint8_t pmp)
{
	uint32_t act;

	lockdep_assert_held(&port->lock);
	if (!port->pmp_attached || port->fbs_enabled)
		return 0;
	act = port->active;
	while (act) {
		uint32_t s = ahci_ffs32(act);

		act &= ~ahci_bit(s);
		if (port->cmds[s].pmp != pmp)
			return 1;
	}
	return 0;
}

/* Simulate the fault the debug knobs asked for.  The command is in the
 * software bookkeeping as issued, but PxCI is never written, so the HBA
 * knows nothing of it.  A timeout is left to the command's deadline; the
 * other faults fail this command alone, with the status the hardware
 * would have reported latched for error handling.  The device knows
 * nothing of the fault and goes on with the other commands: they are not
 * declared aborted (their buffers stay theirs until they complete). */
static void ahci_cmd_inject_fault_locked(struct ahci_port *port,
					 struct ahci_cmd *cmd, int kind)
{
	uint32_t bit = ahci_bit(cmd->slot);
	uint8_t err = port->fault.error ? port->fault.error : ATA_ABORTED;

	lockdep_assert_held(&port->lock);
	port->fault_slots |= bit;

	switch (kind) {
	case AHCI_FAULT_TIMEOUT:
		/* Nothing else: the command's timeout expires (the block
		 * queue's timeout work, an internal waiter's wait). */
		kprintf(AHCI_PFX "%s: fault injection: slot %u will time out\n",
			port->name, cmd->slot);
		return;
	case AHCI_FAULT_DEV_ERROR:
		/* the task file the device would have left */
		port->eh_tfd = ((uint32_t)err << AHCI_PxTFD_ERR_SHIFT) |
			       ATA_DRDY | ATA_ERR;
		ahci_eh_inject_error_locked(port, bit, AHCI_PxIS_TFES, 0);
		return;
	case AHCI_FAULT_HOST_ERROR:
		ahci_eh_inject_error_locked(port, bit, AHCI_PxIS_HBDS, 0);
		return;
	case AHCI_FAULT_LINK_DOWN:
		ahci_eh_inject_error_locked(port, bit,
					    AHCI_PxIS_PRCS | AHCI_PxIS_IFS,
					    AHCI_SERR_DIAG_N);
		return;
	default:
		WARN_ON_ONCE(1);
		port->fault_slots &= ~bit;
		return;
	}
}

/* Hand a prepared command to the HBA (section 5.3.12 / 5.3.2): the
 * command's memory is complete, the store fence orders it before the
 * register writes, SACT before CI for queued commands. */
static void ahci_cmd_hw_issue_locked(struct ahci_port *port,
				     struct ahci_cmd *cmd, int fault)
{
	uint32_t bit = ahci_bit(cmd->slot);

	lockdep_assert_held(&port->lock);
	VM_BUG_ON(port->active & bit);

	cmd->state = AHCI_CMD_ISSUED;
	port->active |= bit;
	if (cmd->is_ncq)
		port->ncq_active |= bit;

	if (unlikely(fault != AHCI_FAULT_NONE)) {
		ahci_cmd_inject_fault_locked(port, cmd, fault);
		return;
	}

	/* write-back where needed + store fence: the HBA owns it now */
	ahci_dma_sync_cmd_for_device(port, cmd);
	if (cmd->is_ncq)
		ahci_port_writel(port, AHCI_PxSACT, bit);
	/* FIS-based switching: PxFBS.DEV names the device the command
	 * goes to (AHCI 1.3.1 section 9.3.6), before the PxCI write. */
	if (port->fbs_enabled)
		ahci_fbs_select_dev_locked(port, cmd->pmp);
	ahci_port_writel(port, AHCI_PxCI, bit);
}

/* Allocate, build and issue `qc' now.  `take_ref': the qc does not yet
 * hold the transport reference.  port->lock held.  Returns the
 * ahci_qc_issue codes. */
static int ahci_cmd_issue_now_locked(struct ahci_port *port,
				     struct ata_queued_cmd *qc, int take_ref)
{
	int ncq = ahci_qc_is_ncq(qc);
	int eh = !!(qc->flags & ATA_QCFLAG_EH);
	struct ahci_cmd *cmd;
	uint64_t now;
	uint32_t timeout_ms;
	int slot, r, fault = AHCI_FAULT_NONE;

	lockdep_assert_held(&port->lock);

	if (port->zombie_slots)
		(void)ahci_cmd_reap_zombies_locked(port);

	/* Error handling's own commands go to slot 0 (the port is EH's
	 * then, nothing else runs); a zombie of an earlier one still there
	 * leaves them the lowest free slot. */
	slot = ahci_slot_alloc_depth_locked(port, ncq, eh && !ncq ? 1 : 0,
					    ahci_qc_ncq_depth(port, qc));
	if (slot < 0 && eh && !ncq)
		slot = ahci_slot_alloc_locked(port, 0, 0);
	if (slot < 0)
		return -EBUSY;
	cmd = &port->cmds[slot];

	r = ahci_cmd_prepare_locked(port, cmd, qc);
	if (r < 0) {
		cmd->qc = NULL;
		ahci_slot_free_locked(port, (uint32_t)slot);
		if (r == -ENOMEM)
			return -EBUSY; /* bounce memory: retry later */
		if (r == -ENODEV)
			return -ENODEV;
		WARN_RATELIMIT(1, AHCI_PFX "%s: cannot build %s (0x%02x): %d",
			       port->name, ata_cmd_name(qc->tf.command),
			       qc->tf.command, r);
		return -EINVAL;
	}

	if (!eh)
		fault = ahci_fault_check_locked(port, cmd);

	/* For the reports only: the command's timeout is watched by its
	 * issuer (the block queue's timeout work, an internal waiter). */
	now = ahci_now_ns();
	timeout_ms = ahci_qc_default_timeout(qc);
	cmd->timeout_ms = timeout_ms;
	cmd->start_ns = now;
	qc->issue_ns = now;
	qc->tag = slot;
	qc->xport_cmd = cmd;
	qc->xport_next = NULL;
	qc->flags &= ~ATA_QCFLAG_TIMEDOUT;
	if (take_ref) {
		ata_qc_get(qc);
		qc->flags |= ATA_QCFLAG_ACTIVE;
	}

	ahci_cmd_hw_issue_locked(port, cmd, fault);

	/* statistics */
	port->stats.cmds_issued++;
	if (ncq) {
		uint32_t depth = ahci_popcount32(port->ncq_active);

		port->stats.ncq_issued++;
		if (depth > port->stats.max_ncq_depth_seen)
			port->stats.max_ncq_depth_seen = depth;
		if (depth <= AHCI_MAX_SLOTS)
			port->ncq_depth_hist[depth]++;
		port->ncq_depth_sum += depth;
		port->ncq_depth_samples++;
	}
	if (cmd->bounce.area.size)
		port->stats.bounces++;
	ahci_trace(port, AHCI_TRACE_ISSUE, cmd, 0, 0);
	return 0;
}

/* Admission under port->lock: refuse or issue. */
static int ahci_qc_admit_locked(struct ahci_port *port,
				struct ahci_link *link,
				struct ata_queued_cmd *qc, int reissue)
{
	int ncq = ahci_qc_is_ncq(qc);
	int eh = !!(qc->flags & ATA_QCFLAG_EH);
	int take_ref = !reissue;
	uint8_t pmp = ahci_link_pmp(link);
	int eh_ctx;

	lockdep_assert_held(&port->lock);

	/* Error handling's work is what it issues itself, flagged
	 * ATA_QCFLAG_EH (the ATA layer flags every command of the eh_mutex
	 * holder).  Block I/O is never EH work, whichever context
	 * dispatches it -- an interrupt-off poller, another CPU's dispatch
	 * batch, the EH thread itself when it restarts the queues. */
	eh_ctx = eh;

	/* Port state */
	switch (port->state) {
	case AHCI_PORT_UNUSED:
	case AHCI_PORT_DISABLED:
		return -ENODEV;
	case AHCI_PORT_IDLE:
		/* Probing runs before the port is called RUNNING: what
		 * matters is a started engine. */
		if (!(ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_ST))
			return -ENODEV;
		break;
	case AHCI_PORT_FROZEN:
	case AHCI_PORT_EH:
		if (!eh_ctx)
			return -EAGAIN;
		/* Error handling may have left the engine stopped (a restart
		 * that failed): a command written to PxCI now would never
		 * run, and its cleared bit would read as finished.  Its
		 * commands are rare enough for the register read. */
		if (unlikely(!(ahci_port_readl(port, AHCI_PxCMD) &
			       AHCI_PxCMD_ST)))
			return -EAGAIN;
		break;
	case AHCI_PORT_SUSPENDED:
		if (!eh_ctx)
			return -EAGAIN;
		break;
	case AHCI_PORT_RUNNING:
	default:
		break;
	}
	/* Error handling scheduled: nothing but its own commands until it
	 * ran -- the commands in flight drain (or fail), then the run
	 * starts. */
	if (!eh_ctx && port->eh_pending)
		return -EAGAIN;
	/* A frozen port takes no command of error handling's own context
	 * either: nothing would signal its end.  It fails at once; recovery
	 * (a reset) thaws the port. */
	if (eh_ctx && port->eh_frozen)
		return -EIO;
	/* A non-queued command waits for the queue to drain: nothing else
	 * is admitted meanwhile (the issuer requeues), or it could starve. */
	if (!eh && port->deferred_qc) {
		port->stats.deferred++;
		return -EBUSY;
	}

	/* Link state */
	if (link->state == AHCI_LINK_DISABLED)
		return -ENODEV;
	if (link->state == AHCI_LINK_OFFLINE && !eh_ctx) {
		if (!link->is_host)
			return -ENODEV;
		if (AHCI_SSTS_DET(ahci_port_readl(port, AHCI_PxSSTS)) !=
		    AHCI_SSTS_DET_PHY)
			return -ENODEV;
	}

	if (ncq) {
		if (port->active & ~port->ncq_active)
			return -EBUSY; /* a non-queued command runs */
		/* Full when every tag this device may use is taken (behind
		 * a port multiplier other devices' tags do not count). */
		if (ahci_popcount32(port->ncq_active &
				    ahci_slot_mask(ahci_qc_ncq_depth(port, qc))) >=
		    ahci_qc_ncq_depth(port, qc)) {
			port->ncq_full_busy++;
			return -EBUSY;
		}
		if (ahci_cmd_pmp_conflict_locked(port, pmp)) {
			port->pmp_switch_busy++;
			return -EBUSY;
		}
		return ahci_cmd_issue_now_locked(port, qc, take_ref);
	}

	if (eh) {
		/* EH commands run on an idle port: inside a run EH aborted
		 * and collected the rest; outside one (the eh_mutex holder's
		 * management command) the command waits, as every non-queued
		 * command does, until what runs has finished. */
		if (port->active) {
			WARN_RATELIMIT(port->eh_running && port->ncq_active,
				       AHCI_PFX "%s: EH command 0x%02x with NCQ 0x%08x in flight",
				       port->name, qc->tf.command,
				       port->ncq_active);
			return -EBUSY;
		}
		return ahci_cmd_issue_now_locked(port, qc, take_ref);
	}

	/* Non-queued: one at a time, never next to queued ones.  One that
	 * carries a block request is accepted and held until what runs has
	 * finished (deferred_qc; the completion that empties the port hands
	 * its issue to kblockd), and nothing else is admitted meanwhile; any
	 * other is refused and its issuer tries again. */
	if (port->ncq_active || (port->active & ~port->ncq_active) ||
	    ahci_cmd_pmp_conflict_locked(port, pmp)) {
		port->stats.deferred++;
		if (reissue || !qc->end_io || !qc->req)
			return -EBUSY;
		qc->xport_next = NULL;
		qc->xport_cmd = NULL;
		qc->tag = -1;
		ata_qc_get(qc);
		qc->flags |= ATA_QCFLAG_ACTIVE;
		port->deferred_qc = qc;
		return 0;
	}
	return ahci_cmd_issue_now_locked(port, qc, take_ref);
}

/* ATA_PROT_SRST: a software reset the ATA layer asked for (ATA
 * PASS-THROUGH with the SRST protocol).  It is a job for the error
 * handler; the qc completes when EH has run the reset. */
static int ahci_qc_issue_srst(struct ahci_port *port, struct ahci_link *link,
			      struct ata_queued_cmd *qc, int reissue)
{
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	if (port->state == AHCI_PORT_UNUSED ||
	    port->state == AHCI_PORT_DISABLED) {
		spin_unlock_irqrestore(&port->lock, fl);
		return -ENODEV;
	}
	qc->xport_next = NULL;
	qc->xport_cmd = NULL;
	qc->tag = -1;
	if (port->srst_tail)
		port->srst_tail->xport_next = qc;
	else
		port->srst_head = qc;
	port->srst_tail = qc;
	if (!reissue) {
		ata_qc_get(qc);
		qc->flags |= ATA_QCFLAG_ACTIVE;
	}
	link->eh_pending |= AHCI_EH_RESET_SOFT;
	ahci_eh_schedule_locked(port, AHCI_EH_RESET_SOFT);
	spin_unlock_irqrestore(&port->lock, fl);
	return 0;
}

static int ahci_qc_issue_common(struct ata_queued_cmd *qc, int reissue)
{
	struct ahci_link *link;
	struct ahci_port *port;
	uint64_t fl;
	int r;

	if (unlikely(!ahci_qc_ok(qc))) {
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	link = ahci_qc_link(qc);
	if (unlikely(!link))
		return -ENODEV;
	port = link->port;
	BUG_ON(!port);
	BUG_ON(!port->hba);
	if (!ahci_dev_usable(qc->dev))
		return -ENODEV;
	if (port->hba->state == AHCI_HBA_DEAD)
		return -ENODEV;
	if (reissue)
		VM_BUG_ON(!(qc->flags & ATA_QCFLAG_ACTIVE));
	else
		VM_WARN_ON(qc->flags & ATA_QCFLAG_ACTIVE);

	r = ahci_qc_validate(port, qc);
	if (r < 0) {
		WARN_RATELIMIT(1,
			       AHCI_PFX "%s: rejected %s (0x%02x) prot %s nbytes %u nsg %u: %d",
			       port->name, ata_cmd_name(qc->tf.command),
			       qc->tf.command, ahci_prot_name(qc->protocol),
			       qc->nbytes, qc->n_elem, r);
		return r;
	}
	if (qc->protocol == ATA_PROT_SRST)
		return ahci_qc_issue_srst(port, link, qc, reissue);

	/* Error handling's own command -- inside a run or from the eh_mutex
	 * holder outside one (a management command under ata_dev_lock) --
	 * frees a halted engine before it goes out (an earlier command of
	 * theirs failed and left a zombie slot).  Nobody else may: the kick
	 * stops the engine under everybody's commands. */
	if (port->zombie_slots && (qc->flags & ATA_QCFLAG_EH) &&
	    ata_mutex_held(&port->eh_mutex)) {
		uint32_t left;

		spin_lock_irqsave(&port->lock, &fl);
		left = ahci_cmd_reap_zombies_locked(port);
		spin_unlock_irqrestore(&port->lock, fl);
		if (left)
			(void)ahci_cmd_kick_engine(port);
	}

	spin_lock_irqsave(&port->lock, &fl);
	r = ahci_qc_admit_locked(port, link, qc, reissue);
	spin_unlock_irqrestore(&port->lock, fl);
	/* An injected fault is the error event an interrupt would have
	 * raised: the commands its abort ended (error handling's own, which
	 * end at once) are completed here, as the interrupt handler
	 * completes them after its error path. */
	if (unlikely(port->cpl_head))
		(void)ahci_cmd_run_deferred_completions(port);
	return r;
}

int ahci_qc_issue(struct ata_queued_cmd *qc)
{
	return ahci_qc_issue_common(qc, 0);
}

int ahci_qc_reissue(struct ata_queued_cmd *qc)
{
	if (!ahci_qc_ok(qc))
		return -EINVAL;
	qc->result = 0;
	qc->err_mask = 0;
	qc->resid = 0;
	qc->flags &= ~(ATA_QCFLAG_TIMEDOUT | ATA_QCFLAG_FAILED |
		       ATA_QCFLAG_ABORTED);
	kmemset(&qc->result_tf, 0, sizeof(qc->result_tf));
	return ahci_qc_issue_common(qc, 1);
}

/* ==========================================================================
 * Completion
 * ========================================================================== */

/* The result task file of a non-queued command the HBA finished without
 * error, from the received-FIS area alone (the caller synchronised it).
 * A PIO data-in command ends with its last PIO Setup FIS -- the device
 * sends no D2H Register FIS after the data -- whose registers and
 * E_Status are the result; error handling's own commands are read from
 * the D2H Register FIS like everything else. */
static void ahci_qc_fill_rtf(struct ahci_port *port, const struct ahci_cmd *cmd,
			     struct ata_queued_cmd *qc)
{
	struct ahci_rx_fis *rx = ahci_rx_fis_of(port, cmd->pmp);
	struct fis_reg_d2h f;

	if (!rx)
		return;
	if (qc->protocol == ATA_PROT_PIO_IN && !(qc->flags & ATA_QCFLAG_EH)) {
		/* Same layout as the D2H Register FIS up to the count; the
		 * ending status sits in byte 15. */
		ahci_fis_copy(&f, &rx->psfis, sizeof(f));
		ata_tf_from_fis(&f, &qc->result_tf);
		qc->result_tf.status =
			((const volatile struct ahci_rx_fis *)rx)->psfis.e_status;
		return;
	}
	ahci_fis_copy(&f, &rx->rfis, sizeof(f));
	ata_tf_from_fis(&f, &qc->result_tf);
}

/* Fill the result of a command the HBA finished without error.  A queued
 * command gets the status and error of the Set Device Bits FIS
 * (`sdb_status', `sdb_error', read once by the caller for all queued
 * commands of the collection; with FIS-based switching each device has
 * its own area, read here).  A non-queued command gets its result task
 * file only when its issuer asked for it (ATA_QCFLAG_RESULT_TF) or it is
 * the driver's own (ATA_QCFLAG_INTERNAL): nobody else looks at the
 * registers of a command that succeeded. */
static void ahci_cmd_finish_locked(struct ahci_port *port,
				   struct ahci_cmd *cmd,
				   struct ata_queued_cmd *qc, uint8_t sdb_status,
				   uint8_t sdb_error)
{
	uint32_t done_bytes = cmd->nbytes;
	int write = ahci_qc_is_write(qc);

	lockdep_assert_held(&port->lock);

	if (cmd->is_ncq) {
		qc->result_tf.flags =
			qc->tf.flags & (ATA_TFLAG_LBA48 | ATA_TFLAG_LBA);
		if (port->fbs_enabled) {
			struct ahci_rx_fis *rx = ahci_rx_fis_of(port, cmd->pmp);

			if (rx) {
				const volatile struct ahci_rx_fis *v = rx;

				sdb_status = v->sdbfis.status;
				sdb_error = v->sdbfis.error;
			}
		}
		qc->result_tf.status = sdb_status;
		qc->result_tf.error = sdb_error;
	} else if (qc->flags & (ATA_QCFLAG_RESULT_TF | ATA_QCFLAG_INTERNAL)) {
		qc->result_tf.flags =
			qc->tf.flags & (ATA_TFLAG_LBA48 | ATA_TFLAG_LBA);
		ahci_qc_fill_rtf(port, cmd, qc);
	}

	/* PRDBC: bytes the HBA moved.  Valid for non-queued commands; it is
	 * what an ATAPI short transfer is measured by.  ATA DMA commands
	 * either move everything or fail, and some HBAs do not update the
	 * count for them, so it is only checked there. */
	qc->resid = 0;
	if (!cmd->is_ncq && cmd->hdr && cmd->nbytes) {
		uint32_t prdbc = cmd->hdr->prdbc;

		if (ata_prot_is_atapi(qc->protocol)) {
			if (prdbc <= cmd->nbytes) {
				qc->resid = cmd->nbytes - prdbc;
				done_bytes = prdbc;
			}
		} else if (prdbc && prdbc != cmd->nbytes) {
			WARN_RATELIMIT(1,
				       AHCI_PFX "%s: slot %u %s moved %u of %u bytes",
				       port->name, cmd->slot,
				       ata_cmd_name(qc->tf.command), prdbc,
				       cmd->nbytes);
		}
	}

	port->stats.cmds_completed++;
	if (write)
		port->stats.bytes_written += done_bytes;
	else
		port->stats.bytes_read += done_bytes;
	ahci_trace(port, AHCI_TRACE_COMPLETE, cmd, 0, 0);
}

/* Append the chain head..tail to *headp, *tailp. */
static void ahci_chain_append(struct ata_queued_cmd **headp,
			      struct ata_queued_cmd **tailp,
			      struct ata_queued_cmd *head,
			      struct ata_queued_cmd *tail)
{
	if (!head)
		return;
	if (*tailp)
		(*tailp)->xport_next = head;
	else
		*headp = head;
	*tailp = tail;
}

/* The locked part of the completion path: find what finished, take the
 * result task files, retire the slots.  The finished commands are
 * appended to *headp, *tailp; returns their number.  port->lock held. */
/* Did the non-queued command of `cmd' end with an error status?  The
 * received-FIS area (memory) says so: the D2H Register FIS, or the PIO
 * Setup FIS's ending status of a PIO data-in command.  Some HBA models
 * of hypervisors clear the command's PxCI bit before they signal the
 * task file error (the error FIS and TFES follow the clear), so a
 * command that failed reads as finished in the collection the error
 * interrupt does first.  Asked there only.  port->lock held. */
static int ahci_cmd_fis_says_error_locked(struct ahci_port *port,
					  const struct ahci_cmd *cmd)
{
	const struct ata_queued_cmd *qc = cmd->qc;
	const volatile struct ahci_rx_fis *v;
	struct ahci_rx_fis *rx;
	uint8_t st = 0;

	lockdep_assert_held(&port->lock);
	rx = ahci_rx_fis_of(port, cmd->pmp);
	if (!rx || !qc)
		return 0;
	v = rx;
	ahci_dma_sync_rx_for_cpu(port);
	if (v->rfis.fis_type == FIS_TYPE_REG_D2H)
		st = v->rfis.status;
	else if (qc->protocol == ATA_PROT_PIO_IN &&
		 v->psfis.fis_type == FIS_TYPE_PIO_SETUP)
		st = v->psfis.e_status;
	return (st & (ATA_ERR | ATA_DF)) != 0;
}

/* The HBA shows command slots busy that the driver did not issue (or
 * that finished): a protocol violation.  Unless the link is being reset
 * (whose events can do that), the port is frozen, everything in flight
 * aborted, and a hard reset scheduled.  port->lock held. */
static void ahci_cmd_illegal_transition_locked(struct ahci_port *port,
					       uint32_t hw_busy)
{
	lockdep_assert_held(&port->lock);
	if (port->host_link.state == AHCI_LINK_RESETTING)
		return;
	WARN_RATELIMIT(1, AHCI_PFX "%s: illegal command slot transition (%08x->%08x)",
		       port->name, port->active, hw_busy);
	ahci_eh_freeze_locked(port);
	port->eh_failed_slots |= ahci_cmd_eh_abort_locked(port, -EIO, AC_ERR_HSM);
	ahci_eh_schedule_locked(port, AHCI_EH_RESET_HARD);
}

static int ahci_port_collect_locked(struct ahci_port *port,
				    struct ata_queued_cmd **headp,
				    struct ata_queued_cmd **tailp, int on_error)
{
	struct ata_queued_cmd *head = NULL, *tail = NULL;
	uint32_t ci, sact = 0, done, nonncq, skip = 0, is = 0;
	uint8_t sdb_status = 0, sdb_error = 0;
	int n = 0, is_read = 0;

	lockdep_assert_held(&port->lock);
	if (!port->active && !port->zombie_slots) {
		/* Nothing in flight: PxCI must say so too -- a busy slot
		 * nobody issued is an illegal transition. */
		ci = ahci_port_readl(port, AHCI_PxCI);
		if (unlikely(ci))
			ahci_cmd_illegal_transition_locked(port, ci);
		return 0;
	}

	/* One register says what finished: PxSACT for queued commands,
	 * PxCI for a non-queued one.  PxCI is not read while only queued
	 * commands run (their PxCI bits clear at acceptance and say nothing
	 * about completion). */
	nonncq = port->active & ~port->ncq_active;
	ci = 0;
	if (port->ncq_active || port->zombie_slots)
		sact = ahci_port_readl(port, AHCI_PxSACT);
	if (nonncq || !port->ncq_active)
		ci = ahci_port_readl(port, AHCI_PxCI);
	/* A slot busy at the HBA that nobody issued (or that finished
	 * already): the HBA and the driver disagree -- nothing is completed,
	 * the port goes to error handling.  A port that stopped answering
	 * (all ones) ends here too. */
	if (unlikely(((port->ncq_active ? sact : 0) | (nonncq ? ci : 0)) &
		     ~(port->active | port->zombie_slots))) {
		ahci_cmd_illegal_transition_locked(
			port, (port->ncq_active ? sact : 0) | (nonncq ? ci : 0));
		return 0;
	}
	if (port->zombie_slots)
		(void)ahci_cmd_reap_zombies_locked(port);

	done = (nonncq & ~ci) | (port->ncq_active & ~sact);
	done &= ~port->fault_slots; /* never reached the HBA */
	{
		uint32_t d = done;

		while (d) {
			uint32_t s = ahci_ffs32(d);
			const struct ahci_cmd *cmd = &port->cmds[s];

			d &= ~ahci_bit(s);
			/* raw FISes have their own poller; EH owns its slots */
			if (!cmd->qc || cmd->state == AHCI_CMD_EH ||
			    cmd->state == AHCI_CMD_ALLOCATED) {
				skip |= ahci_bit(s);
				continue;
			}
			if (cmd->is_ncq ||
			    !ahci_cmd_fis_says_error_locked(port, cmd))
				continue;
			/* The error interrupt's collection: a non-queued
			 * command whose PxCI bit cleared but whose FIS
			 * carries the error is the failed one -- it stays in
			 * flight, charged with the device error, for the
			 * error path to hand to EH.  (A failed queued
			 * command keeps its PxSACT bit.) */
			if (on_error) {
				skip |= ahci_bit(s);
				cmd->qc->err_mask |= AC_ERR_DEV;
				port->cmds[s].state = AHCI_CMD_EH;
				continue;
			}
			/* Any other collection that finds such a command
			 * may have come before the error interrupt: with
			 * the task file error latched in PxIS it is left in
			 * flight for that interrupt's error path. */
			if (!is_read) {
				is = ahci_port_readl(port, AHCI_PxIS);
				is_read = 1;
			}
			if (is & AHCI_PxIS_TFES)
				skip |= ahci_bit(s);
		}
		done &= ~skip;
	}

	/* The device-written FIS area is read after the completion register
	 * showed the commands done.  The Set Device Bits FIS reports status
	 * and error for every queued command it completed: read once. */
	if (done) {
		ahci_dma_sync_rx_for_cpu(port);
		if ((done & port->ncq_active) && !port->fbs_enabled) {
			struct ahci_rx_fis *rx = ahci_rx_fis_of(port, 0);

			if (rx) {
				const volatile struct ahci_rx_fis *v = rx;

				sdb_status = v->sdbfis.status;
				sdb_error = v->sdbfis.error;
			}
		}
	}

	while (done) {
		uint32_t s = ahci_ffs32(done);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *qc = cmd->qc;

		done &= ~ahci_bit(s);
		VM_BUG_ON(!qc);
		cmd->state = AHCI_CMD_COMPLETING;
		port->active &= ~ahci_bit(s);
		port->ncq_active &= ~ahci_bit(s);
		ahci_cmd_finish_locked(port, cmd, qc, sdb_status, sdb_error);
		(void)ahci_cmd_retire_locked(port, cmd);
		qc->xport_next = NULL;
		if (tail)
			tail->xport_next = qc;
		else
			head = qc;
		tail = qc;
		n++;
	}
	if (head) {
		/* a clean completion ends an error streak */
		port->eh_consecutive = 0;
		port->ncq_errors_since_ok = 0;
		/* Error handling waits for the commands in flight: the last
		 * of them to finish wakes it. */
		if (unlikely(port->eh_pending) && !port->eh_running &&
		    ahci_eh_drained_locked(port))
			ahci_kahci_kick(port->hba, AHCI_WORK_EH);
	}
	ahci_cmd_schedule_deferred_locked(port);
	ahci_chain_append(headp, tailp, head, tail);
	return n;
}

int ahci_port_complete(struct ahci_port *port)
{
	struct ata_queued_cmd *head = NULL, *tail = NULL;
	uint64_t fl;
	int n = 0;

	if (unlikely(!port))
		return 0;

	/* What finished earlier goes first: the parked completions, then the
	 * slots collected now. */
	n += ahci_cmd_run_deferred_completions(port);

	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_port_collect_locked(port, &head, &tail, 0);
	spin_unlock_irqrestore(&port->lock, fl);

	n += ahci_complete_chain(head);
	/* held commands whose slots the collection's reap freed */
	n += ahci_cmd_run_deferred_completions(port);
	return n;
}

int ahci_port_irq_collect_locked(struct ahci_port *port,
				 struct ata_queued_cmd **headp,
				 struct ata_queued_cmd **tailp)
{
	int n = 0;

	lockdep_assert_held(&port->lock);
	/* Completions parked for a lock-free context go first, as in
	 * ahci_port_complete(). */
	if (port->cpl_head) {
		for (struct ata_queued_cmd *qc = port->cpl_head; qc;
		     qc = qc->xport_next)
			n++;
		ahci_chain_append(headp, tailp, port->cpl_head,
				  port->cpl_tail);
		port->cpl_head = port->cpl_tail = NULL;
	}
	return n + ahci_port_collect_locked(port, headp, tailp, 0);
}

int ahci_port_irq_collect_error_locked(struct ahci_port *port,
				       struct ata_queued_cmd **headp,
				       struct ata_queued_cmd **tailp)
{
	int n = 0;

	lockdep_assert_held(&port->lock);
	if (port->cpl_head) {
		for (struct ata_queued_cmd *qc = port->cpl_head; qc;
		     qc = qc->xport_next)
			n++;
		ahci_chain_append(headp, tailp, port->cpl_head,
				  port->cpl_tail);
		port->cpl_head = port->cpl_tail = NULL;
	}
	return n + ahci_port_collect_locked(port, headp, tailp, 1);
}

/* ==========================================================================
 * The deferred non-queued command
 * ========================================================================== */

/* Is error handling scheduled or running on the port?  Pending actions
 * (every one of them is an EH run that will own the port -- the
 * asynchronous-notification and media checks included), a run in
 * progress, or the port frozen or in its EH state.  A deferred command
 * waits for nothing then: it goes back to its queue.  port->lock held. */
static int ahci_eh_scheduled_locked(const struct ahci_port *port)
{
	lockdep_assert_held(&port->lock);
	return port->eh_pending != 0 || port->eh_running ||
	       port->state == AHCI_PORT_FROZEN || port->state == AHCI_PORT_EH;
}

/* Can the deferred command run now?  Nothing in flight it could not run
 * next to.  port->lock held. */
static int ahci_cmd_deferred_can_run_locked(struct ahci_port *port,
					    struct ata_queued_cmd *qc)
{
	struct ahci_link *link = ahci_qc_link(qc);

	lockdep_assert_held(&port->lock);
	return !port->ncq_active && !(port->active & ~port->ncq_active) &&
	       !(link && ahci_cmd_pmp_conflict_locked(port,
						      ahci_link_pmp(link)));
}

void ahci_cmd_requeue_deferred_locked(struct ahci_port *port, int err)
{
	struct ata_queued_cmd *qc = port->deferred_qc;

	lockdep_assert_held(&port->lock);
	if (!qc)
		return;
	port->deferred_qc = NULL;
	qc->result = err;
	qc->err_mask |= ahci_errno_to_mask(err);
	if (err == -ETIMEDOUT)
		qc->flags |= ATA_QCFLAG_TIMEDOUT;
	ahci_cmd_defer_complete_locked(port, qc);
}

void ahci_cmd_schedule_deferred_locked(struct ahci_port *port)
{
	struct ata_queued_cmd *qc = port->deferred_qc;

	lockdep_assert_held(&port->lock);
	if (!qc)
		return;
	/* Error handling is due: it goes back to its queue (requeue). */
	if (ahci_eh_scheduled_locked(port)) {
		ahci_cmd_requeue_deferred_locked(port, -ECANCELED);
		return;
	}
	/* The last command it waited for finished: kblockd issues it (not
	 * this context, which may be the interrupt handler). */
	if (ahci_cmd_deferred_can_run_locked(port, qc))
		(void)kblockd_schedule_work(&port->deferred_work);
}

void ahci_cmd_deferred_work_fn(void *arg)
{
	struct ahci_port *port = (struct ahci_port *)arg;
	struct ata_queued_cmd *qc;
	uint64_t fl;
	int r;

	spin_lock_irqsave(&port->lock, &fl);
	qc = port->deferred_qc;
	if (qc && !ahci_eh_scheduled_locked(port) &&
	    (port->state == AHCI_PORT_RUNNING ||
	     port->state == AHCI_PORT_IDLE) &&
	    ahci_cmd_deferred_can_run_locked(port, qc)) {
		port->deferred_qc = NULL;
		if (!ahci_dev_usable(qc->dev))
			r = -ENODEV;
		else
			r = ahci_cmd_issue_now_locked(port, qc, 0);
		if (r < 0) {
			/* No slot or no bounce memory with nothing in flight:
			 * back to its queue; anything else is final. */
			if (r == -EBUSY)
				r = -ECANCELED;
			qc->result = r;
			qc->err_mask |= ahci_errno_to_mask(r);
			ahci_cmd_defer_complete_locked(port, qc);
		}
	}
	spin_unlock_irqrestore(&port->lock, fl);
	ahci_cmd_run_deferred_completions(port);
}

/* Complete a collected chain in the interrupt handler (or the polled-mode
 * timer standing in for it), with port->lock held: ata_qc_complete only
 * wakes an internal waiter or hands a block request to the block
 * softirq. */
void ahci_complete_chain_locked(struct ahci_port *port,
				struct ata_queued_cmd *qc)
{
	lockdep_assert_held(&port->lock);
	while (qc) {
		struct ata_queued_cmd *next = qc->xport_next;

		qc->xport_next = NULL;
		ata_qc_complete(qc);
		qc = next;
	}
}

/* ==========================================================================
 * Raw FIS execution (reset sequences, port multiplier registers)
 * ========================================================================== */

int ahci_exec_raw_fis(struct ahci_port *port, int pmp,
		      const struct fis_reg_h2d *fis, uint32_t hdr_flags,
		      uint32_t timeout_ms)
{
	const uint32_t allowed = AHCI_CMDH_C | AHCI_CMDH_B | AHCI_CMDH_R |
				 AHCI_CMDH_P;
	struct fis_reg_h2d f;
	struct ahci_cmd *cmd;
	uint64_t fl, waited_us = 0, limit_us;
	uint32_t bit, ci = 0, is = 0, tfd = 0;
	int slot, r = 0, done = 0;

	BUG_ON(!port);
	if (!fis || pmp < 0 || pmp > 15)
		return -EINVAL;
	if (hdr_flags & ~allowed) {
		WARN_ON_ONCE(1);
		hdr_flags &= allowed;
	}
	if (!timeout_ms)
		timeout_ms = AHCI_SRST_TIMEOUT_MS;
	limit_us = (uint64_t)timeout_ms * 1000ULL;

	spin_lock_irqsave(&port->lock, &fl);
	if (port->zombie_slots)
		(void)ahci_cmd_reap_zombies_locked(port);
	slot = ahci_slot_alloc_locked(port, 0, port->nr_slots > 1 ? 1 : 0);
	if (slot < 0) {
		spin_unlock_irqrestore(&port->lock, fl);
		return -EBUSY;
	}
	cmd = &port->cmds[slot];
	bit = ahci_bit((uint32_t)slot);
	if ((hdr_flags & AHCI_CMDH_R) && port->active)
		WARN_RATELIMIT(1,
			       AHCI_PFX "%s: reset FIS with commands 0x%08x active",
			       port->name, port->active);
	if (WARN_ON_ONCE(!cmd->table || !cmd->hdr)) {
		ahci_slot_free_locked(port, (uint32_t)slot);
		spin_unlock_irqrestore(&port->lock, fl);
		return -ENODEV;
	}

	f = *fis;
	f.fis_type = FIS_TYPE_REG_H2D;
	f.pm_c = (uint8_t)((f.pm_c & FIS_H2D_C) | ((uint8_t)pmp & FIS_PMP_MASK));
	kmemset(cmd->table->cfis, 0, sizeof(cmd->table->cfis));
	kmemcpy(cmd->table->cfis, &f, sizeof(f));
	kmemset(cmd->table->acmd, 0, sizeof(cmd->table->acmd));
	cmd->hdr->opts = ((FIS_H2D_LEN / 4) & AHCI_CMDH_CFL_MASK) |
			 (((uint32_t)pmp << AHCI_CMDH_PMP_SHIFT) &
			  AHCI_CMDH_PMP_MASK) |
			 hdr_flags;
	cmd->hdr->prdbc = 0;
	cmd->hdr->ctba = (uint32_t)cmd->table_bus;
	cmd->hdr->ctbau = (uint32_t)(cmd->table_bus >> 32);
	cmd->qc = NULL;
	cmd->pmp = (uint8_t)pmp;
	cmd->internal = 1;
	cmd->timeout_ms = timeout_ms;
	cmd->start_ns = ahci_now_ns();
	ahci_rx_fis_forget(port, pmp);
	cmd->state = AHCI_CMD_ISSUED;
	port->active |= bit;
	ahci_trace(port, AHCI_TRACE_ISSUE, cmd, 0, hdr_flags);
	if (port->fbs_enabled)
		ahci_fbs_select_dev_locked(port, pmp);
	ahci_dma_sync_cmd_for_device(port, cmd);
	ahci_port_writel(port, AHCI_PxCI, bit);
	spin_unlock_irqrestore(&port->lock, fl);

	for (;;) {
		ci = ahci_port_readl(port, AHCI_PxCI);
		if (!(ci & bit)) {
			done = 1;
			break;
		}
		is = ahci_port_readl(port, AHCI_PxIS);
		if (is & (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS |
			  AHCI_PxIS_IFS)) {
			r = -EIO;
			break;
		}
		/* The interrupt handler may have acknowledged the error
		 * already; a halted command with ERR in the shadow status is
		 * the same answer.  Only after a moment, so the status left
		 * by the previous command is not mistaken for ours. */
		if (waited_us >= 1000) {
			tfd = ahci_port_readl(port, AHCI_PxTFD);
			if ((tfd & ATA_ERR) && !(tfd & ATA_BUSY)) {
				r = -EIO;
				break;
			}
		}
		if (waited_us >= limit_us) {
			r = -ETIMEDOUT;
			break;
		}
		if (waited_us < AHCI_RAW_FAST_US) {
			ahci_udelay(AHCI_RAW_POLL_US);
			waited_us += AHCI_RAW_POLL_US;
		} else {
			ahci_msleep(1);
			waited_us += 1000;
		}
	}

	tfd = ahci_port_readl(port, AHCI_PxTFD);
	spin_lock_irqsave(&port->lock, &fl);
	if (done) {
		/* Claimed by somebody else's engine stop while still pending
		 * (ahci_cmd_claim_active_locked): the clear PxCI bit is that
		 * stop's doing, not a completion. */
		int aborted = cmd->state == AHCI_CMD_EH;

		ahci_rmb();
		port->active &= ~bit;
		cmd->state = AHCI_CMD_COMPLETING;
		(void)ahci_cmd_retire_locked(port, cmd);
		if (aborted)
			r = -ECANCELED;
		/* A command (not a reset FIS) that ended with ERR failed. */
		else if (!(hdr_flags & AHCI_CMDH_R) && (f.pm_c & FIS_H2D_C) &&
			 (tfd & (ATA_ERR | ATA_DF)))
			r = -EIO;
	} else {
		/* The HBA still holds the slot: until the engine stops. */
		(void)ahci_cmd_zombify_locked(port, cmd);
	}
	spin_unlock_irqrestore(&port->lock, fl);

	ahci_trace(port, r ? AHCI_TRACE_ERROR : AHCI_TRACE_COMPLETE, NULL, r,
		   tfd);
	if (!done) {
		kprintf(AHCI_PFX "%s: raw FIS 0x%02x (pmp %d, hdr 0x%x) %s: PxCI 0x%08x PxIS 0x%08x PxTFD 0x%08x\n",
			port->name, f.command, pmp, hdr_flags,
			r == -ETIMEDOUT ? "timed out" : "failed", ci, is, tfd);
		(void)ahci_cmd_kick_engine(port);
	}
	return r;
}

static int ahci_cmd_fail_active_locked(struct ahci_port *port, int err,
				       uint32_t tfd, int hold_all,
				       struct ata_queued_cmd **head,
				       struct ata_queued_cmd **tail);

int ahci_cmd_kick_engine(struct ahci_port *port)
{
	struct ata_queued_cmd *head = NULL, *tail = NULL;
	struct ahci_hba *hba;
	uint32_t tfd;
	uint64_t fl;
	int r, rc = 0, rs, n, was_running = 0;

	BUG_ON(!port);
	hba = port->hba;

	/* The caller holds the eh_mutex, inside an EH run (port in EH) or
	 * outside one (a management command's zombie: port RUNNING).  A
	 * RUNNING port admits ordinary commands without looking at PxCMD.ST;
	 * between the stop and the restart they would be written to PxCI
	 * of a stopped engine and read back as finished.  So the port is in
	 * error handling for the window, and RUNNING again afterwards. */
	spin_lock_irqsave(&port->lock, &fl);
	if (port->active) {
		/* Other commands still run.  Stopping the engine under them
		 * would not stop the device: it may still finish them and
		 * write into their buffers after they were handed back.  The
		 * engine is not touched; error handling is asked for instead
		 * -- nothing new is admitted, the commands drain, and its
		 * run frees the zombie with a reset of the idle port. */
		spin_unlock_irqrestore(&port->lock, fl);
		ahci_eh_request(port, AHCI_EH_RESET_HARD);
		return -EAGAIN;
	}
	if (port->state == AHCI_PORT_RUNNING) {
		port->state = AHCI_PORT_EH;
		was_running = 1;
	}
	spin_unlock_irqrestore(&port->lock, fl);

	/* Nothing else is in flight: the stop only concerns the zombies
	 * this kick is for. */
	r = ahci_port_stop_engine_abort(port);
	if (r)
		kprintf(AHCI_PFX "%s: engine did not stop (%d)\n", port->name,
			r);

	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if (tfd & (ATA_BUSY | ATA_DRQ)) {
		if (hba->caps.sclo && !(hba->quirks & AHCI_Q_NO_CLO)) {
			rc = ahci_port_clo(port);
			port->stats.clo++;
			if (rc)
				kprintf(AHCI_PFX "%s: command list override failed (%d), PxTFD 0x%08x\n",
					port->name, rc, tfd);
		} else {
			rc = -EOPNOTSUPP;
		}
	}

	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_cmd_reap_zombies_locked(port);
	/* Whatever else was in flight did not finish and never will after
	 * the restart (its PxCI bit is gone): back for requeueing. */
	n = ahci_cmd_fail_active_locked(port, -ECANCELED,
					ahci_port_readl(port, AHCI_PxTFD), 0,
					&head, &tail);
	spin_unlock_irqrestore(&port->lock, fl);

	rs = ahci_port_start_engine(port);
	if (rs)
		kprintf(AHCI_PFX "%s: engine did not restart (%d), PxTFD 0x%08x\n",
			port->name, rs, ahci_port_readl(port, AHCI_PxTFD));
	spin_lock_irqsave(&port->lock, &fl);
	if (rs) {
		/* A stopped engine must not be handed to the issue path:
		 * the port stays in error handling, and a reset is due (the
		 * device still says BSY/DRQ and CLO could not help).  The
		 * error run that follows escalates if this repeats. */
		port->eh_pending |= AHCI_EH_RESET_HARD;
		ahci_kahci_kick(hba, AHCI_WORK_EH);
	} else if (was_running && port->state == AHCI_PORT_EH) {
		port->state = AHCI_PORT_RUNNING;
	}
	spin_unlock_irqrestore(&port->lock, fl);
	/* Completed only with the engine running again: their requeue may
	 * come straight back into the issue path. */
	if (n) {
		kprintf(AHCI_PFX "%s: engine kick: %d command(s) in flight handed back for requeue\n",
			port->name, n);
		ahci_complete_chain(head);
	}
	/* held commands whose slots the stop freed */
	(void)ahci_cmd_run_deferred_completions(port);
	if (r)
		return r;
	if (rc)
		return rc;
	return rs;
}

/* ==========================================================================
 * Drain, fail everything, poll
 * ========================================================================== */

int ahci_port_drain(struct ahci_port *port, uint32_t timeout_ms)
{
	uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;

	BUG_ON(!port);
	if (ata_can_sleep())
		might_sleep();

	for (;;) {
		uint64_t fl;
		int busy;

		spin_lock_irqsave(&port->lock, &fl);
		busy = port->active != 0 || port->cpl_head != NULL ||
		       port->deferred_qc != NULL;
		spin_unlock_irqrestore(&port->lock, fl);
		if (!busy)
			return 0;
		if (waited_us >= limit_us)
			return -ETIMEDOUT;
		if (ata_can_sleep() && port->hba->kahci_running &&
		    !port->polled) {
			ahci_msleep(1);
			waited_us += 1000;
		} else {
			(void)ahci_cmd_poll_port(port);
			ahci_udelay(100);
			waited_us += 100;
		}
	}
}

/* Fill in a command failed by software and chain it. */
static void ahci_cmd_fail_one(struct ahci_port *port,
			      struct ata_queued_cmd *qc, int err, uint32_t tfd,
			      struct ata_queued_cmd **head,
			      struct ata_queued_cmd **tail)
{
	(void)port;
	qc->result = err;
	qc->err_mask |= ahci_errno_to_mask(err);
	if (err == -ECANCELED)
		qc->flags |= ATA_QCFLAG_ABORTED;
	if (!qc->result_tf.status) {
		qc->result_tf.status = (uint8_t)(tfd & AHCI_PxTFD_STS_MASK);
		qc->result_tf.error = (uint8_t)((tfd & AHCI_PxTFD_ERR_MASK) >>
						AHCI_PxTFD_ERR_SHIFT);
	}
	qc->xport_next = NULL;
	if (*tail)
		(*tail)->xport_next = qc;
	else
		*head = qc;
	*tail = qc;
}

/* Fail every command in flight (port->active) with `err', chained onto
 * head/tail for completion after the lock is dropped.  A slot the HBA
 * still owns (engine not stopped, its PxCI/PxSACT bit set; the controller
 * not known to have stopped all DMA) stays a zombie: error handling's own
 * command is completed saying so (ATA_QCFLAG_HW_OWNED, its waiter
 * reclaims it), any other is held until the slot is free and a
 * controller reset requested.  Returns how many were chained.
 * port->lock held. */
static int ahci_cmd_fail_active_locked(struct ahci_port *port, int err,
				       uint32_t tfd, int hold_all,
				       struct ata_queued_cmd **head,
				       struct ata_queued_cmd **tail)
{
	uint32_t ci, sact, pxcmd, act;
	int engine_off, n = 0, held = 0;

	lockdep_assert_held(&port->lock);
	if (!port->active)
		return 0;
	pxcmd = ahci_port_readl(port, AHCI_PxCMD);
	engine_off = !(pxcmd & (AHCI_PxCMD_ST | AHCI_PxCMD_CR));
	/* Registers that read all ones (a controller that stopped
	 * answering -- or only seems to) cannot confirm a slot clear: its
	 * buffers stay with the hardware, as with the zombie reaper. */
	ci = engine_off ? 0 : ahci_port_readl(port, AHCI_PxCI);
	sact = engine_off ? 0 : ahci_port_readl(port, AHCI_PxSACT);

	act = port->active;
	while (act) {
		uint32_t s = ahci_ffs32(act);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *qc = cmd->qc;
		int hw_owned;

		act &= ~ahci_bit(s);
		if (!qc)
			continue; /* raw FIS: its poller sees the failure */
		/* `hold_all': a controller reset follows, which is what
		 * ends the device's side of every command that reached it
		 * -- until then each counts as the hardware's. */
		hw_owned = ((hold_all || ((ci | sact) & ahci_bit(s))) &&
			    !(port->fault_slots & ahci_bit(s)) &&
			    !port->hba->dma_stopped);
		if (hw_owned && !(qc->flags & ATA_QCFLAG_EH)) {
			/* The HBA may still write into its buffer: the
			 * command is held, not completed, until the slot is
			 * confirmed free (the reaper requeues it then).  A
			 * controller reset stops every DMA. */
			(void)ahci_cmd_zombify_locked(port, cmd);
			cmd->held_qc = qc;
			qc->xport_cmd = NULL;
			qc->flags |= ATA_QCFLAG_HW_OWNED;
			port->held_slots |= ahci_bit(s);
			held++;
			ahci_trace(port, AHCI_TRACE_ABORT, NULL, err, s);
			continue;
		}
		if (hw_owned) {
			/* Error handling's own command: its waiter reclaims
			 * the slot (ata_qc_reclaim_hw). */
			(void)ahci_cmd_zombify_locked(port, cmd);
			qc->flags |= ATA_QCFLAG_HW_OWNED;
		} else {
			port->active &= ~ahci_bit(s);
			port->ncq_active &= ~ahci_bit(s);
			cmd->state = AHCI_CMD_COMPLETING;
			(void)ahci_cmd_retire_locked(port, cmd);
		}
		ahci_trace(port, AHCI_TRACE_ABORT, NULL, err, s);
		ahci_cmd_fail_one(port, qc, err, tfd, head, tail);
		n++;
	}
	if (held) {
		/* A controller reset stops every DMA (the slots are free
		 * then); unless one is under way already. */
		port->stats.held_cmds += (uint32_t)held;
		if (port->hba->state != AHCI_HBA_RESETTING &&
		    port->hba->state != AHCI_HBA_DEAD) {
			port->eh_pending |= AHCI_EH_HBA_RESET;
			ahci_kahci_kick(port->hba, AHCI_WORK_HBA_RESET);
		}
	}
	return n;
}

static void ahci_port_fail_all_common(struct ahci_port *port, int err,
				      int hold_all)
{
	struct ata_queued_cmd *head = NULL, *tail = NULL;
	uint32_t tfd;
	uint64_t fl;
	int n = 0;

	BUG_ON(!port);
	if (err >= 0)
		err = -EIO;

	spin_lock_irqsave(&port->lock, &fl);
	tfd = ahci_port_readl(port, AHCI_PxTFD);
	n += ahci_cmd_fail_active_locked(port, err, tfd, hold_all, &head,
					 &tail);
	if (port->deferred_qc) {
		struct ata_queued_cmd *qc = port->deferred_qc;

		port->deferred_qc = NULL;
		ahci_cmd_fail_one(port, qc, err, tfd, &head, &tail);
		n++;
	}
	while (port->srst_head) {
		struct ata_queued_cmd *qc = port->srst_head;

		port->srst_head = qc->xport_next;
		ahci_cmd_fail_one(port, qc, err, tfd, &head, &tail);
		n++;
	}
	port->srst_tail = NULL;
	while (port->eh_hold_head) {
		struct ata_queued_cmd *qc = port->eh_hold_head;

		port->eh_hold_head = qc->xport_next;
		ahci_cmd_fail_one(port, qc, err, tfd, &head, &tail);
		n++;
	}
	port->eh_hold_tail = NULL;
	spin_unlock_irqrestore(&port->lock, fl);

	if (n)
		kprintf(AHCI_PFX "%s: failing %d command(s) with %d\n",
			port->name, n, err);
	ahci_complete_chain(head);
	ahci_cmd_run_deferred_completions(port);
}

void ahci_port_fail_all(struct ahci_port *port, int err)
{
	ahci_port_fail_all_common(port, err, 0);
}

void ahci_port_fail_all_for_reset(struct ahci_port *port, int err)
{
	ahci_port_fail_all_common(port, err, 1);
}

int ahci_cmd_poll_port(struct ahci_port *port)
{
	int n;

	if (!port)
		return 0;
	n = ahci_port_poll(port);
	/* A frozen port's finished commands are left alone by the
	 * interrupt path; the polled waiter still needs them.  (Any other
	 * state was collected by the poll above.) */
	if (port->state == AHCI_PORT_FROZEN)
		n += ahci_port_complete(port);
	n += ahci_cmd_run_deferred_completions(port);

	if (!port->hba->kahci_running) {
		/* No worker thread yet (boot): EH runs here.  The timeouts
		 * that schedule it come from the polled waiters themselves
		 * (an internal command's bounded wait, the block layer's
		 * polled wait scanning its queue). */
		if (port->eh_pending && !port->eh_running)
			(void)ahci_eh_poll_run(port);
		n += ahci_cmd_run_deferred_completions(port);
	}
	return n;
}

/* ==========================================================================
 * The ATA transport of AHCI devices
 * ========================================================================== */

static struct ahci_port *ahci_xport_port(struct ata_device *dev)
{
	struct ahci_link *link;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return NULL;
	link = ahci_dev_link(dev);
	return link ? link->port : NULL;
}

static int ahci_xport_poll(struct ata_device *dev)
{
	struct ahci_port *port = ahci_xport_port(dev);

	if (!port)
		return 0;
	return ahci_cmd_poll_port(port);
}

static void ahci_xport_schedule_eh(struct ata_device *dev, uint32_t actions)
{
	struct ahci_port *port = ahci_xport_port(dev);
	struct ahci_link *link;
	uint64_t fl;

	if (!port || !actions)
		return;
	/* The caller gave up waiting for this device's commands: whatever
	 * is still in flight for it counts as timed out now. */
	if (actions & AHCI_EH_TIMEOUT) {
		(void)ahci_cmd_timeout_match(port, dev, NULL, NULL);
		actions &= ~AHCI_EH_TIMEOUT;
	}
	if (!actions)
		return;
	link = ahci_dev_link(dev);
	spin_lock_irqsave(&port->lock, &fl);
	if (link)
		link->eh_pending |= actions;
	ahci_eh_schedule_locked(port, actions);
	spin_unlock_irqrestore(&port->lock, fl);
}

/* A command passed its timeout (ata_transport.qc_timed_out). */
static int ahci_xport_qc_timed_out(struct ata_device *dev,
				   struct ata_queued_cmd *qc,
				   const struct blk_request *req)
{
	struct ahci_port *port = ahci_xport_port(dev);

	if (!port || (!qc && !req))
		return 0;
	return ahci_cmd_timeout_match(port, dev, qc, req) ? 1 : 0;
}

static int ahci_xport_set_lpm(struct ata_device *dev, int policy)
{
	struct ahci_port *port = ahci_xport_port(dev);
	struct ahci_link *link;
	int took = 0, r;

	if (!port)
		return -ENODEV;
	if (policy < AHCI_LPM_KEEP_FIRMWARE || policy > AHCI_LPM_MIN_POWER)
		return -EINVAL;
	if ((port->hba->quirks & AHCI_Q_NO_LPM) &&
	    policy != AHCI_LPM_KEEP_FIRMWARE &&
	    policy != AHCI_LPM_MAX_PERFORMANCE)
		return -EOPNOTSUPP;
	link = ahci_dev_link(dev);

	if (ata_can_sleep())
		might_sleep();
	if (!ata_mutex_held(&port->eh_mutex)) {
		if (ata_can_sleep()) {
			ata_mutex_lock(&port->eh_mutex);
			took = 1;
		} else {
			took = ata_mutex_trylock(&port->eh_mutex);
		}
	}
	r = ahci_port_set_lpm(port, link, (enum ahci_lpm_policy)policy);
	if (r == 0) {
		link->lpm_policy = (enum ahci_lpm_policy)policy;
		if (link->is_host)
			port->lpm_policy = (enum ahci_lpm_policy)policy;
	}
	if (took)
		ata_mutex_unlock(&port->eh_mutex);
	return r;
}

static void ahci_xport_link_info(struct ata_device *dev,
				 struct ata_link_info *out)
{
	struct ahci_port *port = ahci_xport_port(dev);
	struct ahci_link *link;

	if (!out)
		return;
	kmemset(out, 0, sizeof(*out));
	if (!port)
		return;
	link = ahci_dev_link(dev);
	if (link->is_host) {
		/* Host link SCRs are plain registers: read them live. */
		out->sstatus = ahci_port_readl(port, AHCI_PxSSTS);
		out->scontrol = ahci_port_readl(port, AHCI_PxSCTL);
		out->serror = ahci_port_readl(port, AHCI_PxSERR);
	} else {
		/* PMP link SCRs need a command: use the cached copies. */
		out->sstatus = link->sstatus;
		out->scontrol = link->scontrol;
		out->serror = link->serror;
	}
	out->speed = (uint8_t)AHCI_SSTS_SPD(out->sstatus);
	out->ipm = (uint8_t)AHCI_SSTS_IPM(out->sstatus);
	out->speed_limit = link->speed_limit;
	out->pmp = ahci_link_pmp(link);
	out->pmp_attached = port->pmp_attached ? 1 : 0;
	out->sntf = port->hba->caps.ssntf &&
		    !(port->hba->quirks & AHCI_Q_NO_SNTF);
	out->port_no = port->port_no;
	out->lpm_policy = (int)link->lpm_policy;
}

static void ahci_xport_lock(struct ata_device *dev)
{
	struct ahci_port *port = ahci_xport_port(dev);

	if (!port)
		return;
	if (ata_can_sleep())
		might_sleep();
	ata_mutex_lock(&port->eh_mutex);
}

static void ahci_xport_unlock(struct ata_device *dev)
{
	struct ahci_port *port = ahci_xport_port(dev);

	if (!port)
		return;
	ata_mutex_unlock(&port->eh_mutex);
}

const struct ata_transport ahci_ata_transport = {
	.name = "ahci",
	.qc_issue = ahci_qc_issue,
	.poll = ahci_xport_poll,
	.reset = ahci_eh_reset_dev,
	.schedule_eh = ahci_xport_schedule_eh,
	.set_lpm = ahci_xport_set_lpm,
	.link_info = ahci_xport_link_info,
	.lock = ahci_xport_lock,
	.unlock = ahci_xport_unlock,
	.qc_timed_out = ahci_xport_qc_timed_out,
};

/* ==========================================================================
 * Layout checks this file relies on
 * ========================================================================== */

static inline __attribute__((unused)) void ahci_cmd_layout_checks(void)
{
	ahci_layout_checks();
	ata_layout_checks();
	BUILD_BUG_ON(AHCI_MAX_SLOTS != 32);
	BUILD_BUG_ON(ATA_MAX_QUEUE > AHCI_MAX_SLOTS);
	BUILD_BUG_ON(FIS_H2D_LEN / 4 != 5);
	BUILD_BUG_ON(AHCI_PRD_MAX_BYTES % AHCI_PRDT_PAGE);
	BUILD_BUG_ON(AHCI_MAX_PRD > AHCI_PRDTL_MAX);
	BUILD_BUG_ON(AHCI_CMDH_PMP_MASK != (0xFu << AHCI_CMDH_PMP_SHIFT));
	BUILD_BUG_ON(sizeof(((struct ahci_port *)0)->ncq_depth_hist) /
			     sizeof(uint32_t) !=
		     AHCI_MAX_SLOTS + 1);
}
