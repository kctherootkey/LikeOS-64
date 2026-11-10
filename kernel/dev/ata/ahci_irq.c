// LikeOS AHCI driver - interrupt handler and its polled twin
//
// An AHCI HBA signals one interrupt for everything: the global interrupt
// status register IS has one bit per port, and each port's PxIS says what
// happened there -- a command finished (DHRS, PSS, DSS, SDBS, DPS), the
// device reported an error (TFES), the host bus or the link failed (HBFS,
// HBDS, IFS, UFS, IPMS), or the link changed (PCS, PRCS).  The AHCI 1.3.1
// specification (section 10.7) gives the order of service: read IS, then
// for each port with its bit set read and clear PxIS, then clear IS.  The
// locking rules of this driver fix where each step runs:
//
//   1. read IS (no lock: reading changes nothing);
//   2. per port whose bit was set and which the HBA implements, in ONE
//      hold of port->lock: read PxIS, clear it, then
//        - an error event (AHCI_PxIS_ERROR): the commands that finished
//          before the error are collected and completed first -- their
//          Set Device Bits FISes arrived before the error was signalled
//          -- and then the error is handed over (ahci_eh_irq_error_locked:
//          SError read and cleared, the commands still in flight aborted
//          for the kahci thread's error handling, the port frozen for the
//          AHCI_PxIS_FREEZE events only);
//        - otherwise a Set Device Bits FIS is checked for an asynchronous
//          notification (PxSNTF read and cleared where the HBA has it),
//          and the finished commands are collected (one completion
//          register read: PxSACT while queued commands run, PxCI
//          otherwise; the result task files; the slots retired) and
//          completed (ata_qc_complete);
//      then drop the lock;
//   3. under hba->lock: acknowledge IS.
//
// Completing a command here is the ATA-level end only: the result is
// decoded, an internal command's waiter is woken, and a command that
// carries a block request hands the request to the block softirq of the
// CPU that submitted it (blk_complete_request) -- which runs on the tail
// of this interrupt, or of an inter-processor interrupt on that CPU.  The
// block layer's end of the request (accounting, the submitter's wake-up)
// runs there, and the next command is issued by a task (the submitter,
// or kblockd restarting the queue), never from this handler: on a
// hypervisor every doorbell write is an exit the host may hold up, and
// held up here it would stall the interrupt path of the processor that
// takes this controller's interrupts and every processor spinning on the
// port lock behind it.
//
// Bits that are read-only in PxIS (PCS, PRCS, UFS) reflect bits of PxSERR
// and stay set until those are cleared.  They are error events: the error
// path clears SError right away (an HBA whose SError is left set may stop
// working), keeps the value for error handling, and freezes the port, so
// no further interrupt arrives for them until error handling has run.
// With link power management active a PhyRdy change means nothing: PRCS
// is dropped and SERR.N cleared on every interrupt of the port.
//
// IS is acknowledged after the ports, in every interrupt mode: it is a
// level latch of the ports' PxIS, and cleared before them it could drop the
// bit of a port whose event arrived in between, after which an MSI
// controller sends no further message.  Cleared last, a bit whose port got
// a new event in the meantime latches again and the next message comes
// (or a level-triggered INTx line stays asserted for it).  Bits of ports
// the HBA does not implement are acknowledged in IS and otherwise left
// alone (their PxIE is zero; their PxIS cannot raise anything).
//
// In interrupt mode the handler is the only steady-state reader of the
// status registers: per interrupt IS, each flagged port's PxIS (and PxSNTF
// for a Set Device Bits FIS where the HBA has it), one completion register
// per port (PxSACT while queued commands run, PxCI otherwise), the IS
// acknowledgment.  Every register access of a virtual HBA is an exit the
// device model may hold up while its host is busy, with interrupts off and
// the port's lock held.
//
// The same per-port service runs without interrupts: at boot (polled
// probe, completions driven through the transport's poll hook), from the
// 1 ms poll timer when no interrupt could be set up (a hard interrupt
// context too: it does exactly what the handler does), and from waiters
// and the kahci thread (they complete after dropping the lock).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/io/console.h>

/* Bits of PxIS that mean "look at the command slots" (an interrupt with
 * one of them that finds nothing finished is counted as spurious). */
#define AHCI_IRQ_COMPLETION_BITS                                        \
	(AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS | AHCI_PxIS_SDBS | \
	 AHCI_PxIS_DPS)

/* Count the error bits of one PxIS value into the port statistics. */
static void ahci_irq_count_errors_locked(struct ahci_port *port,
					 uint32_t irq_stat)
{
	struct ahci_port_stats *st = &port->stats;

	lockdep_assert_held(&port->lock);
	if (!(irq_stat & AHCI_PxIS_ERROR))
		return;
	st->errors++;
	if (irq_stat & AHCI_PxIS_TFES)
		st->tfes++;
	if (irq_stat & AHCI_PxIS_HBFS)
		st->hbfs++;
	if (irq_stat & AHCI_PxIS_HBDS)
		st->hbds++;
	if (irq_stat & AHCI_PxIS_IFS)
		st->ifs++;
	if (irq_stat & AHCI_PxIS_INFS)
		st->infs++;
	if (irq_stat & AHCI_PxIS_OFS)
		st->ofs++;
	if (irq_stat & AHCI_PxIS_IPMS)
		st->ipms++;
	if (irq_stat & AHCI_PxIS_UFS)
		st->ufs++;
}

/* With a link power management policy below max_performance the link
 * leaves and re-enters PhyRdy on its own, and a PhyRdy change says
 * nothing about the device: it is not an event then.  Nor is the first
 * one within AHCI_TMOUT_SPURIOUS_PHY_MS of a policy change, which the
 * change itself may cause.  port->lock held. */
static int ahci_irq_ignore_phy_events(struct ahci_port *port)
{
	struct ahci_link *link = &port->host_link;

	if (link->lpm_policy > AHCI_LPM_MAX_PERFORMANCE)
		return 1;
	if (link->lpm_changed) {
		if (hrtimer_now_ns() <
		    link->last_lpm_change_ns +
			    (uint64_t)AHCI_TMOUT_SPURIOUS_PHY_MS * 1000000ULL)
			return 1;
		/* The window is over for good (no clock read any more). */
		link->lpm_changed = 0;
	}
	return 0;
}

/* An asynchronous notification of `dev' (an ATAPI drive that has the
 * feature enabled): its medium may have changed.  The drive's media
 * events work runs now (kblockd); error handling has no part in it. */
static void ahci_irq_media_notify_locked(struct ahci_port *port,
					 struct ata_device *dev)
{
	lockdep_assert_held(&port->lock);
	if (!dev || dev->magic != ATA_DEVICE_MAGIC ||
	    dev->class != ATA_DEV_ATAPI || !(dev->flags & ATA_DFLAG_AN))
		return;
	atapi_media_event(dev);
}

/* A device sent a Set Device Bits FIS with the N bit.  `have_sntf': the
 * HBA records the notifying PM ports in PxSNTF, which is read and cleared
 * (RW1C) here.  Without a port multiplier only a directly attached ATAPI
 * drive notifies (its media); behind one the multiplier's own control
 * port (bit 15) reports PHY changes of its fan-out links, and those are
 * error handling's -- as is every notification when PxSNTF cannot say
 * who sent it. */
static void ahci_irq_async_notification_locked(struct ahci_port *port,
					       int have_sntf)
{
	uint32_t sntf = 0;

	lockdep_assert_held(&port->lock);
	if (have_sntf) {
		sntf = ahci_port_readl(port, AHCI_PxSNTF);
		ahci_port_writel(port, AHCI_PxSNTF, sntf);
	}
	if (!port->pmp_attached) {
		ahci_irq_media_notify_locked(port, port->host_link.dev);
		return;
	}
	if (!have_sntf) {
		ahci_eh_schedule_locked(port, AHCI_EH_SNTF);
		return;
	}
	for (uint32_t l = 0;
	     l < port->nr_pmp_links && l < AHCI_PMP_MAX_LINKS; l++) {
		struct ahci_link *link = &port->pmp_links[l];

		if (link->pmp >= 0 && link->pmp < 32 &&
		    (sntf & (1u << link->pmp)))
			ahci_irq_media_notify_locked(port, link->dev);
	}
	if (sntf & (1u << ATA_PMP_CTRL_PORT))
		ahci_eh_schedule_locked(port, AHCI_EH_SNTF);
}

/* A Set Device Bits FIS arrived.  Where the HBA has PxSNTF it says
 * whether that was a notification.  Without it the received-FIS area is
 * looked at: the FIS that raised this interrupt is the last one stored
 * there (with FIS-based switching there is one area per device and no
 * HBA of that kind lacks PxSNTF). */
static void ahci_irq_sdb_locked(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	struct ahci_rx_fis *rx;

	lockdep_assert_held(&port->lock);
	if (hba->caps.ssntf && !(hba->quirks & AHCI_Q_NO_SNTF)) {
		ahci_irq_async_notification_locked(port, 1);
		return;
	}
	if (port->fbs_enabled) {
		WARN_ON_ONCE(1);
		return;
	}
	rx = ahci_rx_fis_of(port, 0);
	if (!rx)
		return;
	ahci_dma_sync_rx_for_cpu(port);
	if (((const volatile struct ahci_rx_fis *)rx)->sdbfis.flags & FIS_SDB_N)
		ahci_irq_async_notification_locked(port, 0);
}

/* The locked part of a port's service: read + acknowledge PxIS, then
 * either the error path (finished commands collected first) or the
 * notification check and the collection.  The collected commands --
 * those parked earlier for a lock-free context first -- are appended to
 * *headp, *tailp and counted in *done; the caller completes them.
 * Returns the PxIS value read. */
static uint32_t ahci_port_irq_locked(struct ahci_port *port,
				     struct ata_queued_cmd **headp,
				     struct ata_queued_cmd **tailp, int *done)
{
	uint32_t status;
	int collect;

	lockdep_assert_held(&port->lock);

	/* A port that stopped answering reads all ones: taken as it reads
	 * (every error bit set), which freezes the port and resets it. */
	status = ahci_port_readl(port, AHCI_PxIS);
	/* A frozen port's commands belong to error handling; an unused port
	 * has none. */
	collect = port->state != AHCI_PORT_FROZEN &&
		  port->state != AHCI_PORT_UNUSED;
	if (status == 0)
		goto complete;

	/* Acknowledge first (RW1C): an event arriving after this point sets
	 * its bit again and is seen by the next interrupt or poll. */
	ahci_port_writel(port, AHCI_PxIS, status);
	port->stats.irqs++;
	if (port->trace)
		ahci_trace(port, AHCI_TRACE_IRQ, NULL, 0, status);
	ahci_irq_count_errors_locked(port, status);

	/* A link reset makes the multiplier field of the FISes meaningless
	 * for a moment. */
	if (unlikely(port->host_link.state == AHCI_LINK_RESETTING))
		status &= ~AHCI_PxIS_IPMS;
	if (unlikely(ahci_irq_ignore_phy_events(port))) {
		status &= ~AHCI_PxIS_PRCS;
		ahci_port_writel(port, AHCI_PxSERR, AHCI_SERR_DIAG_N);
	}

	if (unlikely(status & AHCI_PxIS_ERROR)) {
		/* Set Device Bits FISes that reported completions may have
		 * arrived before the error: those commands are done and are
		 * completed as such, then the error takes what is left. */
		if (collect)
			*done += ahci_port_irq_collect_error_locked(port, headp,
								    tailp);
		ahci_eh_irq_error_locked(port, status);
		if (port->trace)
			ahci_trace(port, AHCI_TRACE_ERROR, NULL, -EIO, status);
		return status;
	}

	if (status & AHCI_PxIS_SDBS)
		ahci_irq_sdb_locked(port);

complete:
	if (collect)
		*done += ahci_port_irq_collect_locked(port, headp, tailp);
	return status;
}

/* Commands parked on cpl_head for a lock-free context (the error path
 * ended the error handler's own commands, a deferred command was handed
 * back): appended to the chain, counted. */
static int ahci_irq_take_parked_locked(struct ahci_port *port,
				       struct ata_queued_cmd **headp,
				       struct ata_queued_cmd **tailp)
{
	int n = 0;

	lockdep_assert_held(&port->lock);
	if (!port->cpl_head)
		return 0;
	for (struct ata_queued_cmd *qc = port->cpl_head; qc;
	     qc = qc->xport_next)
		n++;
	if (*tailp)
		(*tailp)->xport_next = port->cpl_head;
	else
		*headp = port->cpl_head;
	*tailp = port->cpl_tail;
	port->cpl_head = port->cpl_tail = NULL;
	return n;
}

/* The handler's service of one port (also the polled-mode timer's, which
 * stands in for it): acknowledge, collect and complete in one hold of
 * port->lock.  Returns the number of commands completed. */
static int ahci_port_irq_service(struct ahci_port *port)
{
	struct ata_queued_cmd *head = NULL, *tail = NULL;
	uint64_t fl;
	uint32_t status;
	int done = 0;

	spin_lock_irqsave(&port->lock, &fl);
	status = ahci_port_irq_locked(port, &head, &tail, &done);
	done += ahci_irq_take_parked_locked(port, &head, &tail);
	if (done == 0 && (status & AHCI_IRQ_COMPLETION_BITS) &&
	    !(status & AHCI_PxIS_ERROR)) {
		/* A completion interrupt with nothing finished: a poller (or
		 * another CPU) got there first. */
		port->stats.spurious++;
	}
	ahci_complete_chain_locked(port, head);
	spin_unlock_irqrestore(&port->lock, fl);
	return done;
}

int ahci_port_irq(struct ahci_port *port)
{
	BUG_ON(port == NULL);
	VM_BUG_ON(port->hba == NULL);
	return ahci_port_irq_service(port);
}

/* One port's polled service.  `from_timer': the polled-mode timer, which
 * stands in for the interrupt and so does what the handler does.
 * Otherwise (a waiter polling, kahci) the same is done under the lock and
 * the commands are completed after dropping it. */
static int ahci_port_poll_common(struct ahci_port *port, int from_timer)
{
	struct ata_queued_cmd *head = NULL, *tail = NULL;
	uint64_t fl;
	int done = 0;

	BUG_ON(port == NULL);
	if (unlikely(port->state == AHCI_PORT_UNUSED || !port->mmio))
		return 0;
	if (from_timer)
		return ahci_port_irq_service(port);

	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_port_irq_locked(port, &head, &tail, &done);
	done += ahci_irq_take_parked_locked(port, &head, &tail);
	spin_unlock_irqrestore(&port->lock, fl);
	(void)ahci_complete_chain(head);
	return done;
}

int ahci_port_poll(struct ahci_port *port)
{
	return ahci_port_poll_common(port, 0);
}

/* The global status, read: which ports have an interrupt pending.  No
 * lock: the read changes nothing, and the acknowledgment below is what
 * the lock orders against a controller reset. */
static uint32_t ahci_hba_read_is(struct ahci_hba *hba)
{
	return ahci_hba_readl(hba, AHCI_IS);
}

/* ... and acknowledged, AFTER the ports' own PxIS (the order of the AHCI
 * interrupt handling sequence: PxIS first, then IS).  IS is a level latch
 * of the ports' status: cleared first, a port event that lands between
 * reading IS and clearing PxIS can leave the port's IS bit set without
 * the HBA ever sending another MSI for it -- the controller then raises
 * no interrupt at all until something else clears IS (seen as a CD-ROM
 * command "timing out" with the drive long finished while the disk next
 * to it stalled).  Cleared last, a bit whose port has a new event latches
 * again and the HBA sends the next message for it.  The whole value read
 * is written back, bits of unimplemented ports included, so a stray one
 * cannot keep the line asserted. */
static void ahci_hba_ack_is(struct ahci_hba *hba, uint32_t is)
{
	uint64_t fl;

	if (is == 0)
		return;
	spin_lock_irqsave(&hba->lock, &fl);
	ahci_hba_writel(hba, AHCI_IS, is);
	spin_unlock_irqrestore(&hba->lock, fl);
}

/* Second acknowledgment for level-triggered INTx after a poll (the polled
 * service acknowledges PxIS behind the handler's back): the IS bits of
 * ports whose PxIS is now clear.  A port with a new event keeps its bit. */
static void ahci_hba_reack_is(struct ahci_hba *hba)
{
	uint64_t fl;
	uint32_t is, clear = 0;

	spin_lock_irqsave(&hba->lock, &fl);
	is = ahci_hba_readl(hba, AHCI_IS);
	if (is != 0) {
		for (uint32_t b = is; b; b &= b - 1) {
			int i = __builtin_ctz(b);
			volatile uint8_t *pm = hba->mmio + AHCI_PORT_BASE +
					       (uint32_t)i * AHCI_PORT_SIZE;

			if (ahci_readl(pm, AHCI_PxIS) == 0)
				clear |= 1u << i;
		}
		if (clear)
			ahci_hba_writel(hba, AHCI_IS, clear);
	}
	spin_unlock_irqrestore(&hba->lock, fl);
}

/* After an interrupt that read IS as all ones was served: a function
 * that no longer answers configuration cycles is gone -- the controller
 * is marked dead (later interrupts return at once) and kahci gives its
 * ports up. */
static void ahci_irq_check_gone(struct ahci_hba *hba)
{
	uint64_t fl;
	int newly = 0;

	if (ahci_pci_alive(hba))
		return;
	spin_lock_irqsave(&hba->lock, &fl);
	if (hba->state != AHCI_HBA_DEAD) {
		hba->state = AHCI_HBA_DEAD;
		hba->gone_pending = 1;
		newly = 1;
	}
	/* a function that is gone does no DMA */
	hba->dma_stopped = 1;
	spin_unlock_irqrestore(&hba->lock, fl);
	if (newly) {
		WARN_RATELIMIT(1, "[ahci] %s: controller removed", hba->name);
		hba->stats.host_errors++;
		ahci_kahci_kick(hba, AHCI_WORK_GONE);
	}
}

int ahci_irq_handler(void *arg)
{
	struct ahci_hba *hba = arg;
	uint32_t is, mine;
	int exclusive, all_ones;

	if (unlikely(!hba || !hba->mmio))
		return 0;
	VM_WARN_ON(!irqs_disabled());
	/* An MSI/MSI-X vector belongs to this HBA alone: it is always
	 * claimed (and so acknowledged at the local APIC), even when a poll
	 * got to the events first.  Only a shared INTx line may answer "not
	 * mine" and let the next handler look. */
	exclusive = hba->irq_mode == AHCI_IRQ_MSI ||
		    hba->irq_mode == AHCI_IRQ_MSIX;

	is = ahci_hba_read_is(hba);
	if (is == 0) {
		/* Shared INTx line, or an MSI that raced a poll. */
		hba->stats.spurious_irqs++;
		return exclusive;
	}
	/* All ones: the function may have stopped answering (removed, D3hot,
	 * a PCI error).  A controller asleep, dead or being reset on purpose
	 * is not looked at.  Otherwise its ports are served below -- each
	 * reads all ones and goes through its error path -- but a shared
	 * line is never claimed for it: such a controller raises nothing
	 * itself, and the line's other devices must still be served. */
	all_ones = is == 0xFFFFFFFFu;
	if (unlikely(all_ones) &&
	    (hba->state == AHCI_HBA_SUSPENDED || hba->state == AHCI_HBA_DEAD ||
	     hba->state == AHCI_HBA_RESETTING))
		return exclusive;
	hba->stats.irqs++;

	/* Only the ports the HBA implements are looked at.  One of them
	 * without a port structure (its set-up failed) has its interrupts
	 * disabled; a bit for it is only reported. */
	mine = is & hba->port_map;
	for (uint32_t b = mine; b; b &= b - 1) {
		int i = __builtin_ctz(b);
		struct ahci_port *port = hba->ports[i];

		if (unlikely(!port)) {
			WARN_RATELIMIT(1, "[ahci] %s: interrupt on disabled port %d",
				       hba->name, i);
			continue;
		}
		ahci_port_irq(port);
	}

	/* The ports are acknowledged: now the global status.  Once: a bit
	 * that latches again belongs to a port with a new event, which the
	 * next interrupt serves (on a level-triggered line too). */
	ahci_hba_ack_is(hba, is);
	if (unlikely(all_ones)) {
		ahci_irq_check_gone(hba);
		return exclusive;
	}
	return mine != 0 || exclusive;
}

static int ahci_hba_poll_common(struct ahci_hba *hba, int from_timer)
{
	int total = 0;
	uint32_t is;

	if (unlikely(!hba || !hba->mmio))
		return 0;
	/* Not while the controller is dead, asleep or being reset: the
	 * recovery owns every register then. */
	if (hba->state == AHCI_HBA_DEAD || hba->state == AHCI_HBA_SUSPENDED ||
	    hba->state == AHCI_HBA_RESETTING)
		return 0;
	hba->stats.polls++;

	/* The global status is acknowledged like the handler does -- after
	 * the ports -- so an interrupt that is enabled later does not fire
	 * for stale bits. */
	is = ahci_hba_read_is(hba);

	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port || !(hba->port_map & (1u << i)))
			continue;
		total += ahci_port_poll_common(port, from_timer);
	}
	ahci_hba_ack_is(hba, is);
	if (hba->irq_mode == AHCI_IRQ_INTX)
		ahci_hba_reack_is(hba);
	return total;
}

int ahci_hba_poll(struct ahci_hba *hba)
{
	return ahci_hba_poll_common(hba, 0);
}

int ahci_hba_poll_timer(struct ahci_hba *hba)
{
	return ahci_hba_poll_common(hba, 1);
}

void ahci_port_irq_enable_locked(struct ahci_port *port, uint32_t mask)
{
	struct ahci_hba *hba = port->hba;

	lockdep_assert_held(&port->lock);
	/* No hot-plug interrupts where hot-plug is broken or impossible:
	 * a floating PHY on an empty port can toggle PhyRdy forever. */
	if (hba && (hba->quirks & AHCI_Q_NO_HOTPLUG))
		mask &= ~(AHCI_PxIS_PCS | AHCI_PxIS_PRCS | AHCI_PxIS_CPDS |
			  AHCI_PxIS_DMPS);
	/* CPDS only means something on cold-presence-detect ports, DMPS
	 * only with a mechanical presence switch. */
	if (!(port->flags & AHCI_PORT_F_CPD))
		mask &= ~AHCI_PxIS_CPDS;
	if (!(port->flags & AHCI_PORT_F_MPS))
		mask &= ~AHCI_PxIS_DMPS;
	port->irq_mask = mask;
	/* Not read back: the write reaches the port before any later
	 * access to it (MMIO writes are not reordered). */
	ahci_port_writel(port, AHCI_PxIE, mask);
	port->irq_enabled = mask != 0;
	/* Interrupts back on: a freeze is over. */
	port->eh_frozen = 0;
}

void ahci_port_irq_disable_locked(struct ahci_port *port)
{
	lockdep_assert_held(&port->lock);
	ahci_port_writel(port, AHCI_PxIE, 0);
	ahci_port_flush(port);
	port->irq_enabled = 0;
}
