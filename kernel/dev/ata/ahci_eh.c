// LikeOS AHCI driver - error handling, NCQ error recovery, port recovery
//
// Errors reach this file from three directions:
//
//   - the interrupt path (ahci_port_irq) calls ahci_eh_irq_error_locked
//     with port->lock held when PxIS carries an error event, after it
//     completed the commands that finished before the error: SError is
//     read and cleared, the commands still in flight are aborted for EH
//     (AHCI_CMD_EH), nothing new is admitted, the port is frozen (PxIE
//     masked) for the AHCI_PxIS_FREEZE events only, and the kahci thread
//     is kicked; the SError analysis and the task file status of a
//     device error are taken by the EH run;
//   - a command's timeout (the block queue's timeout work, an internal
//     command's bounded wait) reaches the transport's qc_timed_out
//     (ahci_cmd.c), which marks the command and asks for AHCI_EH_TIMEOUT
//     the same way;
//   - the ATA layer asks for resets (transport reset op, ATA PASS-THROUGH
//     software reset), revalidation and LPM changes.
//
// The work happens in ahci_eh_run(), called by the HBA's kahci thread with
// nothing held; it takes the port's eh_mutex and loops over "rounds" until
// no action is pending.  An error round follows the AHCI 1.3.1
// specification, section 6.2.2:
//
//   1. complete what the HBA did finish (PxCI / PxSACT already clear) and
//      the interrupt did not abort;
//   2. stop the command engine (PxCMD.ST = 0, wait for CR = 0), which
//      clears PxCI and PxSACT -- from here on software owns every slot;
//   3. collect the commands that were in flight;
//   4. clear PxSERR and PxIS;
//   5. if PxTFD still shows BSY or DRQ the device needs a reset: command
//      list override (CAP.SCLO) and a software reset, or a COMRESET;
//   6. restart the engine;
//   7. for NCQ errors read the NCQ Command Error log (READ LOG EXT page
//      10h) BEFORE any reset clears it: it names the failed tag, every
//      other outstanding queued command was aborted by the device and is
//      an innocent victim (SATA 3.x section 13.6.3.2);
//   8. decide per command: retry (innocent victims always, failed ones by
//      policy and counter) or fail with a decoded error.
//
// The recovery ladder escalates from the mildest action that can work:
// abort (stop/clear/restart) -> command list override -> software reset
// (SRST: an H2D Register FIS with SRST set and the header's R and C bits,
// then one with SRST clear, section 10.4.1) with a DEVICE RESET first for
// ATAPI devices -> COMRESET (ahci_port.c) -> HBA reset (ahci_hba.c) ->
// give up and disable the port.  The starting rung rises with every
// consecutive error run that saw no clean completion in between.
//
// Commands EH itself issues while it runs (the NCQ log read, a
// revalidation's IDENTIFY, a PMP register access) have nobody else to
// recover them: the EH thread is blocked on them.  So while EH runs, an
// error or timeout of such a command completes it at once with the error
// (its slot stays a zombie until the next engine stop; a freezing event
// or a timeout freezes the port, and EH's next command is refused until
// a reset thaws it); EH sees the result, and the run goes round again.
// This keeps EH free of deadlocks without running hardware recovery in
// interrupt context.
//
// At boot (interrupts off, no scheduler, no kahci yet) the same code runs
// inline from the polled wait of the command that failed
// (ahci_eh_poll_run via the transport's poll); the controller reset rung
// is not available there.
//
// Locking: eh_mutex -> port->lock -> hba->lock.  Block queues are stopped
// and started with no driver lock held (their lock is a leaf that must
// never nest inside a driver lock).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define AHCI_PFX "[ahci] "

/* COMRESET attempts of one recovery before the devices are given up
 * (the last one at a lower link speed), with their deadlines. */
static const uint32_t ahci_eh_reset_timeouts[] = AHCI_EH_RESET_TIMEOUTS;
#define AHCI_EH_RESET_TRIES \
	((int)(sizeof(ahci_eh_reset_timeouts) / sizeof(ahci_eh_reset_timeouts[0])))
/* Rounds of one EH run before the remaining work is left for the next. */
#define AHCI_EH_MAX_ROUNDS 8
/* Link errors on a link before its speed is lowered by one generation. */
#define AHCI_EH_SPEED_DOWN_ERRORS 3
/* NCQ errors without a clean completion before NCQ is turned off. */
#define AHCI_EH_NCQ_OFF_ERRORS 3
/* How long ahci_eh_reset_dev waits for the worker's EH run. */
#define AHCI_EH_RESET_WAIT_MS 120000

/* One command EH took from the hardware. */
struct ahci_eh_qc {
	struct ata_queued_cmd *qc;
	uint8_t slot;
	uint8_t ncq;
	uint8_t blamed; /* this command caused (or suffered) the error */
	uint8_t timedout;
	uint8_t faulted; /* carried an injected fault */
	uint8_t ncq_suspect; /* queued, error not yet attributed */
	uint8_t pad[2];
	uint32_t err_mask; /* AC_ERR_* found by EH */
	int err; /* -errno to complete with */
	const char *why; /* decoded reason, for the log */
};

/* The state of one error round. */
struct ahci_eh_ctx {
	uint32_t actions;
	uint32_t irq_stat, serror, tfd; /* latched by the interrupt path */
	uint32_t failed_hint; /* slots active when the error hit */
	uint32_t tfd_now; /* PxTFD after the engine stopped */
	int fbs_dev; /* PxFBS.DWE when SDE was set, else -1 */
	struct ahci_eh_qc q[AHCI_MAX_SLOTS];
	uint32_t collected; /* valid entries of q[] */
	enum ahci_eh_level reached;
	int link_gone; /* the device is not there any more */
	int need_hba_reset;
	int did_reset; /* SRST or COMRESET ran: revalidate */
	int srst_done; /* a software reset ran (result below) */
	int srst_result;
	uint32_t srst_sig;
	int inline_ctx; /* not the kahci thread (boot / reset op) */
	int bus_errors; /* link-level error seen in this round */
	int quiet; /* only a device error of commands that asked for quiet */
	int frozen; /* the port was frozen when the round started */
	int pmp_recovered; /* the multiplier and its devices were recovered */
	uint32_t reset_timeout_ms; /* deadline of this COMRESET attempt */
	int revalidate; /* the failed commands ask for a revalidation */
};

/* Commands EH re-issues or completes at the end of a run. */
struct ahci_eh_lists {
	struct ata_queued_cmd *retry_head, *retry_tail;
	struct ata_queued_cmd *done_head, *done_tail;
};

static uint32_t ahci_eh_run_locked(struct ahci_port *port, int inline_ctx);

/* ==========================================================================
 * Names and small helpers
 * ========================================================================== */

static const char *ahci_eh_level_name(enum ahci_eh_level l)
{
	switch (l) {
	case AHCI_EH_LVL_NONE:
		return "none";
	case AHCI_EH_LVL_RETRY:
		return "retry";
	case AHCI_EH_LVL_ABORT:
		return "abort";
	case AHCI_EH_LVL_CLO:
		return "command list override";
	case AHCI_EH_LVL_SRST:
		return "software reset";
	case AHCI_EH_LVL_COMRESET:
		return "COMRESET";
	case AHCI_EH_LVL_HBA_RESET:
		return "controller reset";
	case AHCI_EH_LVL_GIVE_UP:
		return "give up";
	default:
		return "?";
	}
}

static int ahci_eh_actions_str(uint32_t a, char *buf, int cap)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{ AHCI_EH_TIMEOUT, "TIMEOUT" },
		{ AHCI_EH_DEV_ERROR, "DEV_ERROR" },
		{ AHCI_EH_NCQ_ERROR, "NCQ_ERROR" },
		{ AHCI_EH_HOST_ERROR, "HOST_ERROR" },
		{ AHCI_EH_IF_FATAL, "IF_FATAL" },
		{ AHCI_EH_IF_NONFATAL, "IF_NONFATAL" },
		{ AHCI_EH_OVERFLOW, "OVERFLOW" },
		{ AHCI_EH_UNKNOWN_FIS, "UNKNOWN_FIS" },
		{ AHCI_EH_IPMS, "IPMS" },
		{ AHCI_EH_HOTPLUG, "HOTPLUG" },
		{ AHCI_EH_SNTF, "SNTF" },
		{ AHCI_EH_RESET_SOFT, "RESET_SOFT" },
		{ AHCI_EH_RESET_HARD, "RESET_HARD" },
		{ AHCI_EH_REVALIDATE, "REVALIDATE" },
		{ AHCI_EH_LPM, "LPM" },
		{ AHCI_EH_HBA_RESET, "HBA_RESET" },
		{ AHCI_EH_DETACH, "DETACH" },
		{ AHCI_EH_SUSPEND, "SUSPEND" },
		{ AHCI_EH_RESUME, "RESUME" },
		{ AHCI_EH_FAULT, "FAULT" },
		{ AHCI_EH_REISSUE, "REISSUE" },
		{ AHCI_EH_PORT, "PORT" },
	};
	int len = 0;

	if (cap <= 0)
		return 0;
	buf[0] = 0;
	len += ksnprintf(buf + len, (size_t)(cap - len), "{");
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(a & names[i].bit) || len >= cap - 1)
			continue;
		len += ksnprintf(buf + len, (size_t)(cap - len), " %s",
				 names[i].name);
	}
	if (len < cap - 1)
		len += ksnprintf(buf + len, (size_t)(cap - len), " }");
	if (len >= cap)
		len = cap - 1;
	return len;
}

static inline uint32_t ahci_eh_bit(uint32_t s)
{
	return 1u << (s & 31);
}

static inline int ahci_eh_dev_usable(const struct ata_device *dev)
{
	return dev && dev->magic == ATA_DEVICE_MAGIC &&
	       !(dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED));
}

/* Every link of the port that may carry a device: the host link, or the
 * fan-out links behind a port multiplier. */
static int ahci_eh_links(struct ahci_port *port, struct ahci_link **out,
			 int cap)
{
	int n = 0;

	if (port->pmp_attached) {
		uint32_t nr = port->nr_pmp_links;

		if (nr > AHCI_PMP_MAX_LINKS)
			nr = AHCI_PMP_MAX_LINKS;
		for (uint32_t i = 0; i < nr && n < cap; i++)
			out[n++] = &port->pmp_links[i];
	} else if (cap > 0) {
		out[n++] = &port->host_link;
	}
	return n;
}

static void ahci_eh_for_each_dev(struct ahci_port *port,
				 void (*fn)(struct ahci_port *port,
					    struct ahci_link *link,
					    struct ata_device *dev, void *arg),
				 void *arg)
{
	struct ahci_link *links[AHCI_PMP_MAX_LINKS + 1];
	int n = ahci_eh_links(port, links, AHCI_PMP_MAX_LINKS + 1);

	for (int i = 0; i < n; i++) {
		struct ata_device *dev = links[i]->dev;

		if (dev && dev->magic == ATA_DEVICE_MAGIC)
			fn(port, links[i], dev, arg);
	}
}

static int ahci_eh_port_has_device(struct ahci_port *port)
{
	struct ahci_link *links[AHCI_PMP_MAX_LINKS + 1];
	int n = ahci_eh_links(port, links, AHCI_PMP_MAX_LINKS + 1);

	if (port->pmp_attached)
		return 1;
	for (int i = 0; i < n; i++)
		if (ahci_eh_dev_usable(links[i]->dev))
			return 1;
	return 0;
}

static int ahci_eh_host_link_online(struct ahci_port *port)
{
	uint32_t ssts = ahci_port_readl(port, AHCI_PxSSTS);

	return AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_PHY;
}

/* ==========================================================================
 * Block queues of the port's disks (no driver lock held)
 * ========================================================================== */

static void ahci_eh_queue_stop_one(struct ahci_port *port,
				   struct ahci_link *link,
				   struct ata_device *dev, void *arg)
{
	(void)port;
	(void)link;
	(void)arg;
	/* A detached device's disk is on its way out (disk_del on the
	 * registration worker, which may free it): not ours to touch. */
	if (dev->flags & ATA_DFLAG_DETACHED)
		return;
	if (dev->disk && dev->disk->queue)
		blk_queue_stop(dev->disk->queue);
}

static void ahci_eh_queue_start_one(struct ahci_port *port,
				    struct ahci_link *link,
				    struct ata_device *dev, void *arg)
{
	(void)port;
	(void)link;
	(void)arg;
	/* Its queue stays stopped: disk_del() fails what waits there. */
	if (dev->flags & ATA_DFLAG_DETACHED)
		return;
	if (dev->disk && dev->disk->queue) {
		blk_queue_start_async(dev->disk->queue);
	}
}

static void ahci_eh_stop_queues(struct ahci_port *port)
{
	ahci_eh_for_each_dev(port, ahci_eh_queue_stop_one, NULL);
}

static void ahci_eh_start_queues(struct ahci_port *port)
{
	ahci_eh_for_each_dev(port, ahci_eh_queue_start_one, NULL);
}

/* ==========================================================================
 * Error decoding: status/error -> errno, AC_ERR_* and a reason
 * ========================================================================== */

struct ahci_tf_diag {
	int err;
	uint32_t err_mask;
	const char *why;
};

/* The meaning of a final status/error pair (ACS-4 section 6.2/6.3; the
 * ATAPI error register carries the sense key in its high nibble). */
static void ahci_eh_decode_tf(uint8_t status, uint8_t error, int atapi,
			      int write, struct ahci_tf_diag *d)
{
	d->err = 0;
	d->err_mask = 0;
	d->why = "no error";

	if (status & ATA_BUSY) {
		d->err = -EIO;
		d->err_mask = AC_ERR_HSM;
		d->why = "device still busy (BSY)";
		return;
	}
	if (status & ATA_DF) {
		d->err = -EIO;
		d->err_mask = AC_ERR_DEV;
		d->why = "device fault (DF)";
		return;
	}
	if (!(status & ATA_ERR)) {
		if (status & ATA_DRQ) {
			d->err = -EIO;
			d->err_mask = AC_ERR_HSM;
			d->why = "data request (DRQ) left pending";
		}
		return;
	}

	d->err = -EIO;
	if (atapi) {
		uint8_t key = (uint8_t)(error >> ATAPI_ERR_SENSE_KEY_SHIFT);

		switch (key) {
		case 0x0:
			d->err_mask = AC_ERR_DEV;
			d->why = (error & ATA_ABORTED) ? "packet command aborted"
						       : "check condition, no sense";
			break;
		case 0x1:
			d->err_mask = AC_ERR_DEV;
			d->why = "recovered error";
			break;
		case 0x2:
			d->err_mask = AC_ERR_DEV;
			d->why = "not ready (medium absent or becoming ready)";
			break;
		case 0x3:
			d->err_mask = AC_ERR_DEV | AC_ERR_MEDIA;
			d->why = "medium error";
			break;
		case 0x4:
			d->err_mask = AC_ERR_DEV;
			d->why = "hardware error";
			break;
		case 0x5:
			d->err_mask = AC_ERR_DEV | AC_ERR_INVALID;
			d->why = "illegal request";
			break;
		case 0x6:
			d->err_mask = AC_ERR_DEV;
			d->why = "unit attention (medium changed or reset)";
			break;
		case 0x7:
			d->err = -EACCES;
			d->err_mask = AC_ERR_DEV;
			d->why = "data protect";
			break;
		case 0xB:
			d->err_mask = AC_ERR_ATA_BUS;
			d->why = "aborted command (transport)";
			break;
		default:
			d->err_mask = AC_ERR_DEV;
			d->why = "check condition";
			break;
		}
		return;
	}

	if (error & ATA_ICRC) {
		d->err_mask = AC_ERR_ATA_BUS;
		d->why = "interface CRC error (ICRC)";
	} else if (error & ATA_UNC) {
		d->err_mask = AC_ERR_DEV | AC_ERR_MEDIA;
		d->why = write ? "unrecoverable write error (UNC)"
			       : "unrecovered read error (UNC)";
	} else if (error & ATA_IDNF) {
		d->err_mask = AC_ERR_DEV | AC_ERR_INVALID;
		d->why = "ID not found: address out of range (IDNF)";
	} else if (error & ATA_MC) {
		d->err_mask = AC_ERR_DEV;
		d->why = "media changed (MC)";
	} else if (error & ATA_MCR) {
		d->err_mask = AC_ERR_DEV;
		d->why = "media change request (MCR)";
	} else if (error & ATA_TRK0NF) {
		d->err = -ENOMEDIUM;
		d->err_mask = AC_ERR_DEV;
		d->why = "no media (NM)";
	} else if (error & ATA_AMNF) {
		d->err_mask = AC_ERR_DEV | AC_ERR_MEDIA;
		d->why = "address mark not found (AMNF)";
	} else if (error & ATA_ABORTED) {
		d->err_mask = AC_ERR_DEV;
		d->why = "command aborted by the device (ABRT)";
	} else {
		d->err_mask = AC_ERR_DEV | AC_ERR_OTHER;
		d->why = "error with no cause reported";
	}
}

/* At most 10 failure reports per port in 5 seconds; the count of the
 * suppressed ones is printed with the next report that gets through. */
static int ahci_eh_report_ratelimit(struct ahci_port *port)
{
	static uint64_t win_ns[AHCI_MAX_PORTS];
	static uint32_t in_win[AHCI_MAX_PORTS], missed[AHCI_MAX_PORTS];
	uint64_t now = hrtimer_now_ns();
	uint32_t i = (uint32_t)port->index % AHCI_MAX_PORTS;

	if (now - win_ns[i] > 5000000000ULL) {
		if (missed[i])
			kprintf(AHCI_PFX "%s: %u failure reports suppressed\n",
				port->name, missed[i]);
		win_ns[i] = now;
		in_win[i] = 0;
		missed[i] = 0;
	}
	if (in_win[i] >= 10) {
		missed[i]++;
		return 0;
	}
	in_win[i]++;
	return 1;
}

/* Log one failed command: the command, the registers, the decode. */
static void ahci_eh_report_failed(struct ahci_port *port,
				  const struct ahci_eh_qc *e)
{
	const struct ata_queued_cmd *qc = e->qc;
	const char *dname = (qc->dev && qc->dev->magic == ATA_DEVICE_MAGIC)
				    ? qc->dev->name
				    : port->name;
	char sbuf[64], ebuf[64];

	/* An injected fault failed as it was told to: one line, and the
	 * command is quiet from here on (the ATA and block layers do not
	 * report it again) -- a fault-injection run must stay readable. */
	if (e->faulted) {
		((struct ata_queued_cmd *)qc)->flags |= ATA_QCFLAG_QUIET;
		kprintf(AHCI_PFX "%s: injected fault: %s tag %u lba %llu -> %d (%s)\n",
			dname, ata_cmd_name(qc->tf.command), e->slot,
			(unsigned long long)qc->lba, e->err,
			e->why ? e->why : "?");
		return;
	}
	if (qc->flags & ATA_QCFLAG_QUIET)
		return;
	/* A device error is not a kernel bug: an error line, at most a
	 * few per second.  The decoded command block follows once, from
	 * the completion (ata_dev_report_error). */
	if (!ahci_eh_report_ratelimit(port))
		return;
	ata_status_str(qc->result_tf.status, sbuf, sizeof(sbuf));
	ata_error_str(qc->result_tf.error, ebuf, sizeof(ebuf));
	kprintf(AHCI_PFX "%s: failed command: %s (0x%02x) %s tag %u lba %llu count %u: status 0x%02x %s error 0x%02x %s: %s -> %d (err_mask 0x%x, %u retries)\n",
		dname, ata_cmd_name(qc->tf.command), qc->tf.command,
		e->ncq ? "ncq" : "non-ncq", e->slot,
		(unsigned long long)qc->lba, qc->n_sect,
		qc->result_tf.status, sbuf, qc->result_tf.error, ebuf,
		e->why ? e->why : "?", e->err, e->err_mask, qc->retries);
}

/* ==========================================================================
 * Freeze, thaw, scheduling
 * ========================================================================== */

void ahci_eh_freeze_locked(struct ahci_port *port)
{
	BUG_ON(!port);
	lockdep_assert_held(&port->lock);

	/* EH state without an EH run is ahci_cmd_kick_engine's stop/restart
	 * window: freeze it too, or the kick's restore would put the port
	 * back to RUNNING with its interrupts masked. */
	if (port->state == AHCI_PORT_RUNNING || port->state == AHCI_PORT_IDLE ||
	    (port->state == AHCI_PORT_EH && !port->eh_running))
		port->state = AHCI_PORT_FROZEN;
	/* PxIE cleared, without reading anything back: the freeze runs in
	 * the interrupt handler, and an interrupt the HBA had already sent
	 * finds the port frozen (its completions are left alone).  Error
	 * handling's own commands are refused until the port is unmasked
	 * again. */
	ahci_port_writel(port, AHCI_PxIE, 0);
	port->irq_enabled = 0;
	port->eh_frozen = 1;
}

/* Clear the error and event status a frozen port collected and unmask
 * its interrupts again; the port state is the caller's business. */
static void ahci_eh_unmask_locked(struct ahci_port *port)
{
	struct ahci_hba *hba;
	uint32_t v, mask;
	uint64_t hfl;

	lockdep_assert_held(&port->lock);
	hba = port->hba;

	/* Never masked, on a port served by its interrupt (an EH run for a
	 * link power management change, a revalidation ...): the
	 * handler kept acknowledging PxIS and IS all along and PxIE holds the
	 * mask already.  Nothing was collected to clear, and clearing PxIS/IS
	 * behind the handler's back could only take an event it has yet to
	 * serve.  A polled port (boot, no interrupt) is cleared as always. */
	if (port->irq_enabled && !port->polled)
		return;

	/* The interrupt status collected while frozen goes (PxIS, then the
	 * port's IS bit); SError is error handling's (cleared by the error
	 * interrupt and by the resets), not the thaw's. */
	v = ahci_port_readl(port, AHCI_PxIS);
	ahci_port_writel(port, AHCI_PxIS, v);
	spin_lock_irqsave(&hba->lock, &hfl);
	ahci_hba_writel(hba, AHCI_IS, ahci_eh_bit((uint32_t)port->index));
	spin_unlock_irqrestore(&hba->lock, hfl);

	mask = port->irq_mask ? port->irq_mask : AHCI_PxIE_DEFAULT;
	ahci_port_irq_enable_locked(port, mask);
}

void ahci_eh_thaw_locked(struct ahci_port *port)
{
	BUG_ON(!port);
	lockdep_assert_held(&port->lock);

	ahci_eh_unmask_locked(port);
	if (port->state == AHCI_PORT_FROZEN || port->state == AHCI_PORT_EH)
		port->state = ahci_eh_port_has_device(port) ? AHCI_PORT_RUNNING
							    : AHCI_PORT_IDLE;
}

/* Error handling is due (no freeze): the actions pend, a deferred
 * non-queued command goes back to its queue (its issuer requeues it) so
 * that EH can run, the kahci thread is woken -- the run itself starts
 * once the commands in flight have drained (ahci_eh_run_locked). */
static void ahci_eh_set_pending_locked(struct ahci_port *port,
				       uint32_t actions)
{
	lockdep_assert_held(&port->lock);
	port->eh_pending |= actions;
	ahci_cmd_requeue_deferred_locked(port, -ECANCELED);
	ahci_kahci_kick(port->hba, AHCI_WORK_EH);
}

void ahci_eh_schedule_locked(struct ahci_port *port, uint32_t actions)
{
	BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	if (!actions)
		return;
	if ((actions & AHCI_EH_FREEZING) && !port->eh_running)
		ahci_eh_freeze_locked(port);
	ahci_eh_set_pending_locked(port, actions);
}

void ahci_eh_request(struct ahci_port *port, uint32_t actions)
{
	uint64_t fl;

	if (!port || !actions)
		return;
	spin_lock_irqsave(&port->lock, &fl);
	ahci_eh_set_pending_locked(port, actions);
	spin_unlock_irqrestore(&port->lock, fl);
	ahci_cmd_run_deferred_completions(port);
}

void ahci_eh_schedule(struct ahci_port *port, uint32_t actions)
{
	uint64_t fl;

	if (!port || !actions)
		return;
	spin_lock_irqsave(&port->lock, &fl);
	ahci_eh_schedule_locked(port, actions);
	spin_unlock_irqrestore(&port->lock, fl);
	ahci_cmd_run_deferred_completions(port);
}

/* PxIS error bits -> EH actions (section 6.1.4 / 10.6).  A device error
 * with queued commands outstanding names no command: the NCQ Command Error
 * log does.  An incorrect port multiplier field is an error only with a
 * multiplier attached (without one the event still freezes the port and
 * aborts what is in flight).  An interface fatal error confined to one
 * device behind the multiplier (`fbs_dev' >= 0) is that device's error,
 * not the link's. */
static uint32_t ahci_eh_irq_actions(struct ahci_port *port, uint32_t is,
				    int fbs_dev)
{
	uint32_t a = 0;
	uint32_t dev_err = port->ncq_active ? AHCI_EH_NCQ_ERROR
					    : AHCI_EH_DEV_ERROR;

	if (is & AHCI_PxIS_TFES)
		a |= dev_err;
	if (is & AHCI_PxIS_UFS)
		a |= AHCI_EH_UNKNOWN_FIS;
	if ((is & AHCI_PxIS_IPMS) && port->pmp_attached)
		a |= AHCI_EH_IPMS;
	if (is & (AHCI_PxIS_HBFS | AHCI_PxIS_HBDS))
		a |= AHCI_EH_HOST_ERROR;
	if (is & AHCI_PxIS_IFS)
		a |= fbs_dev >= 0 ? dev_err : AHCI_EH_IF_FATAL;
	if (is & (AHCI_PxIS_PCS | AHCI_PxIS_PRCS)) {
		port->hotplug_ns = hrtimer_now_ns();
		if (!(port->hba->quirks & AHCI_Q_NO_HOTPLUG))
			a |= AHCI_EH_HOTPLUG;
		else
			WARN_RATELIMIT(1, AHCI_PFX "%s: hot-plug event ignored (quirk), PxIS 0x%08x",
				       port->name, is);
	}
	return a;
}

/* Abort the commands of one PMP link (its device's error was cleared,
 * the other devices go on): the eh_mutex holder's are completed with the
 * error, the others become error handling's.  Returns those.  port->lock
 * held. */
static uint32_t ahci_eh_abort_link_locked(struct ahci_port *port, int pmp,
					  uint32_t err_mask)
{
	uint32_t act = port->active, own = 0, others = 0;

	lockdep_assert_held(&port->lock);
	while (act) {
		uint32_t s = (uint32_t)__builtin_ctz(act);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *qc = cmd->qc;

		act &= ~ahci_eh_bit(s);
		if (!qc || cmd->pmp != (uint8_t)pmp)
			continue;
		if (ahci_qc_is_eh_own(port, qc)) {
			own |= ahci_eh_bit(s);
			continue;
		}
		others |= ahci_eh_bit(s);
		if (cmd->state == AHCI_CMD_TIMEDOUT)
			qc->flags |= ATA_QCFLAG_TIMEDOUT;
		cmd->state = AHCI_CMD_EH;
	}
	if (own)
		(void)ahci_cmd_eh_fail_inflight_locked(port, own, -EIO,
						       err_mask);
	return others;
}

/* Clear the error state of the one device behind the multiplier that
 * failed (PxFBS.DEC, AHCI 1.3.1 section 9.3.6): the HBA drops that
 * device's commands and goes on with the others.  The time DEC takes is
 * not specified: a few short re-reads.  port->lock held (the interrupt
 * handler). */
static void ahci_eh_fbs_dec_locked(struct ahci_port *port)
{
	uint32_t fbs;
	int retries = 3;

	lockdep_assert_held(&port->lock);
	WARN_ON_ONCE(!port->fbs_enabled);
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	ahci_port_writel(port, AHCI_PxFBS, fbs | AHCI_PxFBS_DEC);
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	while ((fbs & AHCI_PxFBS_DEC) && retries--) {
		ahci_udelay(1);
		fbs = ahci_port_readl(port, AHCI_PxFBS);
	}
	if (fbs & AHCI_PxFBS_DEC)
		WARN_RATELIMIT(1, AHCI_PFX "%s: failed to clear device error",
			       port->name);
}

/* A plain device error -- TFES and nothing else, no SError: the device
 * answered a command with ERR (an ATAPI CHECK CONDITION, an ABRT) and the
 * link and the HBA are fine. */
static int ahci_eh_dev_error_only(uint32_t irq_stat, uint32_t serr,
				  uint32_t actions)
{
	return (irq_stat & AHCI_PxIS_ERROR) == AHCI_PxIS_TFES && !serr &&
	       actions == AHCI_EH_DEV_ERROR;
}

/* Every command in `slots' asked not to be reported when it fails
 * (ATA_QCFLAG_QUIET: probes and media checks, where an error is an answer
 * -- an empty optical drive says NOT READY to its TEST UNIT READY). */
static int ahci_eh_slots_quiet_locked(struct ahci_port *port, uint32_t slots)
{
	lockdep_assert_held(&port->lock);
	if (!slots)
		return 0;
	while (slots) {
		uint32_t s = (uint32_t)__builtin_ctz(slots);
		struct ata_queued_cmd *qc = port->cmds[s].qc;

		slots &= slots - 1;
		if (!qc || !(qc->flags & ATA_QCFLAG_QUIET))
			return 0;
	}
	return 1;
}

/* The error event of an interrupt (or of a polled wait standing in for
 * it).  The commands that finished before the error were completed by
 * the caller already; everything still in flight now is aborted for error
 * handling -- whichever of them failed is for EH to find out (the NCQ
 * Command Error log names the failed queued command, the others are
 * retried as they are).  The commands of the eh_mutex holder (error
 * handling's own) are completed at once with the error.  Only the
 * AHCI_PxIS_FREEZE events mask the port's interrupts; after a device error
 * or a host bus data error the port keeps them.  With FIS-based switching
 * a device error of one device behind the multiplier aborts that device's
 * commands only, and its error state is cleared right here (DEC).
 * port->lock held. */
void ahci_eh_irq_error_locked(struct ahci_port *port, uint32_t irq_stat)
{
	struct ahci_hba *hba;
	uint32_t serr, actions, err_mask, others;
	int fbs_dev = -1;
	char ibuf[96], sbuf[128];

	BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	hba = port->hba;

	/* Which device failed, with FIS-based switching. */
	if (port->fbs_enabled) {
		uint32_t fbs = ahci_port_readl(port, AHCI_PxFBS);
		int pmp = (int)((fbs & AHCI_PxFBS_DWE_MASK) >>
				AHCI_PxFBS_DWE_SHIFT);

		if ((fbs & AHCI_PxFBS_SDE) && pmp < (int)port->nr_pmp_links)
			fbs_dev = pmp;
	}

	/* SError is cleared here: an HBA whose SError stays set may stop
	 * working.  The value is kept for error handling, which analyses
	 * it. */
	serr = ahci_port_readl(port, AHCI_PxSERR);
	ahci_port_writel(port, AHCI_PxSERR, serr);

	/* Some controllers raise IFS for device errors: ignored there. */
	if (hba->quirks & AHCI_Q_IGN_IRQ_IF_ERR)
		irq_stat &= ~AHCI_PxIS_IFS;
	/* ... and report an internal error with them. */
	if ((irq_stat & AHCI_PxIS_TFES) &&
	    (hba->quirks & AHCI_Q_IGN_SERR_INTERNAL))
		serr &= ~AHCI_SERR_ERR_E;

	actions = ahci_eh_irq_actions(port, irq_stat, fbs_dev);
	/* An error event always gets the port looked at, even when nothing
	 * in it names a specific recovery. */
	if (!actions)
		actions = AHCI_EH_PORT;

	port->eh_irq_stat |= irq_stat;
	port->eh_serror |= serr;

	ahci_irq_stat_str(irq_stat, ibuf, sizeof(ibuf));
	ahci_serror_str(serr, sbuf, sizeof(sbuf));
	/* Whether this error is an answer of a quiet command (an empty
	 * optical drive's NOT READY) or worth a report: decided now, while
	 * the failed commands are still attached to their slots, and
	 * latched for the EH run's report. */
	if (!(ahci_eh_dev_error_only(irq_stat, serr, actions) &&
	      ahci_eh_slots_quiet_locked(port, port->active)))
		port->eh_noisy = 1;
	WARN_RATELIMIT((actions & ~(AHCI_EH_HOTPLUG | AHCI_EH_SNTF)) != 0 &&
			       port->eh_noisy,
		       AHCI_PFX "%s: exception: irq_stat 0x%08x %s SErr 0x%08x %s active 0x%08x sact 0x%08x%s",
		       port->name, irq_stat, ibuf, serr, sbuf, port->active,
		       port->ncq_active, port->eh_running ? " (during EH)" : "");
	ahci_trace(port, AHCI_TRACE_ERROR, NULL, -EIO, irq_stat);

	err_mask = AC_ERR_DEV;
	if (actions & AHCI_EH_HOST_ERROR)
		err_mask = AC_ERR_HOST_BUS;
	else if (actions & (AHCI_EH_IF_FATAL | AHCI_EH_HOTPLUG))
		err_mask = AC_ERR_ATA_BUS;
	else if (actions & (AHCI_EH_UNKNOWN_FIS | AHCI_EH_IPMS))
		err_mask = AC_ERR_HSM;

	/* Hand over: freeze for the events after which the port's state is
	 * unknown -- during an EH run too -- and abort what is in flight
	 * (the whole port; with a single device error behind the
	 * multiplier, that device's link, whose error state is cleared). */
	if (irq_stat & AHCI_PxIS_FREEZE) {
		ahci_eh_freeze_locked(port);
		others = ahci_cmd_eh_abort_locked(port, -EIO, err_mask);
	} else if (fbs_dev >= 0) {
		others = ahci_eh_abort_link_locked(port, fbs_dev, err_mask);
		ahci_eh_fbs_dec_locked(port);
		port->eh_fbs_link = (uint8_t)(fbs_dev + 1);
	} else {
		others = ahci_cmd_eh_abort_locked(port, -EIO, err_mask);
	}
	port->eh_failed_slots |= others;

	/* Error handling is due, also when the error hit one of its own
	 * commands during a run: the run goes round once more. */
	ahci_eh_set_pending_locked(port, actions);
}

void ahci_eh_inject_error_locked(struct ahci_port *port, uint32_t slots,
				 uint32_t irq_stat, uint32_t serr)
{
	uint32_t actions;

	BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	/* The event the interrupt would have raised, for error handling's
	 * report and choice of recovery: its actions and status latched.
	 * Only the injected command failed (it never reached the HBA); the
	 * device is still executing the others for real -- they are neither
	 * aborted nor frozen out, but complete normally while nothing new
	 * is admitted, and the run starts once they have drained. */
	actions = ahci_eh_irq_actions(port, irq_stat, -1) | AHCI_EH_FAULT;
	port->eh_irq_stat |= irq_stat;
	port->eh_serror |= serr;
	for (uint32_t b = slots & port->active; b; b &= b - 1) {
		uint32_t s = (uint32_t)__builtin_ctz(b);

		port->cmds[s].state = AHCI_CMD_EH;
	}
	port->eh_failed_slots |= slots;
	ahci_eh_set_pending_locked(port, actions);
}

/* ==========================================================================
 * Polled command execution for EH's own commands
 * ========================================================================== */

/* Clear what the error left in SError / PxIS / the HBA's IS bit. */
static void ahci_eh_clear_errors(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	uint32_t v;
	uint64_t fl, hfl;

	spin_lock_irqsave(&port->lock, &fl);
	v = ahci_port_readl(port, AHCI_PxSERR);
	if (v)
		ahci_port_writel(port, AHCI_PxSERR, v);
	v = ahci_port_readl(port, AHCI_PxIS);
	if (v)
		ahci_port_writel(port, AHCI_PxIS, v);
	spin_lock_irqsave(&hba->lock, &hfl);
	ahci_hba_writel(hba, AHCI_IS, ahci_eh_bit((uint32_t)port->index));
	spin_unlock_irqrestore(&hba->lock, hfl);
	spin_unlock_irqrestore(&port->lock, fl);
}

/* Run one command for EH and drive it to completion by polling, without
 * relying on interrupts or on the kahci thread (which is us).  `tf' gets
 * the result registers.  Returns 0 or -errno. */
static int ahci_eh_exec_polled(struct ahci_port *port, struct ata_device *dev,
			       struct ata_taskfile *tf, enum ata_prot prot,
			       void *buf, uint32_t len, enum ata_dma_dir dir,
			       uint32_t timeout_ms)
{
	struct ata_queued_cmd *qc;
	uint64_t waited_us = 0, limit_us;
	int r;

	if (!ahci_eh_dev_usable(dev))
		return -ENODEV;
	qc = ata_qc_alloc(dev);
	if (!qc)
		return -ENOMEM;
	qc->tf = *tf;
	qc->protocol = prot;
	qc->flags |= ATA_QCFLAG_EH | ATA_QCFLAG_QUIET | ATA_QCFLAG_RESULT_TF |
		     ATA_QCFLAG_INTERNAL;
	qc->timeout_ms = timeout_ms ? timeout_ms : ATA_TMOUT_INTERNAL_MS;
	qc->dma_dir = dir;
	if (buf && len)
		ata_qc_set_buf(qc, buf, len, dir);
	limit_us = (uint64_t)qc->timeout_ms * 1000ULL;

	r = ahci_qc_issue(qc);
	if (r) {
		ata_qc_free(qc);
		return r;
	}

	while (!ata_completion_done(&qc->done)) {
		uint64_t fl;
		uint32_t is;

		(void)ahci_port_complete(port);
		(void)ahci_cmd_run_deferred_completions(port);
		if (ata_completion_done(&qc->done))
			break;

		/* Errors, also with the port's interrupt masked. */
		spin_lock_irqsave(&port->lock, &fl);
		is = ahci_port_readl(port, AHCI_PxIS);
		if (is & AHCI_PxIS_ERROR) {
			ahci_port_writel(port, AHCI_PxIS, is & AHCI_PxIS_ERROR);
			ahci_eh_irq_error_locked(port, is & AHCI_PxIS_ERROR);
		}
		if (waited_us >= limit_us && qc->xport_cmd) {
			/* The command's timeout is this wait's, counted from
			 * the delays (the clock may not run yet at boot): the
			 * port is frozen, the command ended here with
			 * -ETIMEDOUT (its slot left to the engine stop below),
			 * anything else in flight aborted, and error handling
			 * looks at the port again. */
			struct ahci_cmd *cmd = (struct ahci_cmd *)qc->xport_cmd;

			ahci_eh_freeze_locked(port);
			(void)ahci_cmd_eh_fail_inflight_locked(
				port, ahci_eh_bit(cmd->slot), -ETIMEDOUT,
				AC_ERR_TIMEOUT);
			(void)ahci_cmd_eh_abort_locked(port, -EIO,
						       AC_ERR_OTHER);
			ahci_eh_set_pending_locked(port, AHCI_EH_TIMEOUT);
		}
		spin_unlock_irqrestore(&port->lock, fl);
		(void)ahci_cmd_run_deferred_completions(port);
		if (ata_completion_done(&qc->done))
			break;

		if (waited_us < 2000) {
			ahci_udelay(20);
			waited_us += 20;
		} else {
			ahci_msleep(1);
			waited_us += 1000;
		}
	}

	*tf = qc->result_tf;
	r = qc->result;
	if (!r && (qc->result_tf.status & (ATA_ERR | ATA_DF)))
		r = -EIO;
	/* A failed command may leave the HBA halted holding the slot (and
	 * possibly still pointing at `buf'): stop the engine before the
	 * caller frees the buffer. */
	if (port->zombie_slots)
		(void)ahci_cmd_kick_engine(port);
	ata_qc_free(qc);
	return r;
}

/* ==========================================================================
 * NCQ error analysis: READ LOG EXT, log 10h
 * ========================================================================== */

static int ahci_eh_read_ncq_log(struct ahci_port *port, struct ata_device *dev,
				uint8_t *page)
{
	struct ata_taskfile tf;
	uint8_t sum = 0;
	int r = -EIO, use_dma = !!(dev->flags & ATA_DFLAG_LOG_DMA);

	for (int attempt = 0; attempt < 2; attempt++) {
		kmemset(&tf, 0, sizeof(tf));
		tf.flags = ATA_TFLAG_LBA48 | ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
		tf.command = use_dma ? ATA_CMD_READ_LOG_DMA_EXT
				     : ATA_CMD_READ_LOG_EXT;
		tf.lbal = ATA_LOG_NCQ_CMD_ERR; /* log address */
		tf.lbam = 0; /* page 0, low byte */
		tf.hob_lbam = 0; /* page 0, high byte */
		tf.nsect = 1; /* one 512-byte page */
		tf.hob_nsect = 0;
		tf.device = ATA_LBA;
		kmemset(page, 0, ATA_SECT_SIZE);
		r = ahci_eh_exec_polled(port, dev, &tf,
					use_dma ? ATA_PROT_DMA_IN
						: ATA_PROT_PIO_IN,
					page, ATA_SECT_SIZE,
					ATA_DIR_FROM_DEVICE,
					ATA_TMOUT_INTERNAL_MS);
		if (r == 0 || !use_dma)
			break;
		/* Some devices advertise READ LOG DMA EXT and refuse it in
		 * this state: fall back to the PIO form. */
		kprintf(AHCI_PFX "%s: READ LOG DMA EXT failed (%d), retrying with PIO\n",
			dev->name, r);
		use_dma = 0;
	}
	if (r)
		return r;
	for (int i = 0; i < ATA_SECT_SIZE; i++)
		sum = (uint8_t)(sum + page[i]);
	if (sum != 0) {
		kprintf(AHCI_PFX "%s: NCQ error log has a bad checksum (0x%02x)\n",
			dev->name, sum);
		return -EIO;
	}
	return 0;
}

/* Fill a command's result from the NCQ error log page. */
static void ahci_eh_tf_from_ncq_log(const uint8_t *p, struct ata_taskfile *tf)
{
	tf->flags = ATA_TFLAG_LBA48 | ATA_TFLAG_LBA;
	tf->status = p[ATA_NCQ_ERR_STATUS];
	tf->error = p[ATA_NCQ_ERR_ERROR];
	tf->lbal = p[ATA_NCQ_ERR_LBA0];
	tf->lbam = p[ATA_NCQ_ERR_LBA0 + 1];
	tf->lbah = p[ATA_NCQ_ERR_LBA0 + 2];
	tf->device = p[ATA_NCQ_ERR_DEVICE];
	tf->hob_lbal = p[ATA_NCQ_ERR_LBA3];
	tf->hob_lbam = p[ATA_NCQ_ERR_LBA3 + 1];
	tf->hob_lbah = p[ATA_NCQ_ERR_LBA3 + 2];
	tf->nsect = p[ATA_NCQ_ERR_COUNT];
	tf->hob_nsect = p[ATA_NCQ_ERR_COUNT + 1];
}

/* The device whose queued commands were outstanding. */
static struct ata_device *ahci_eh_ncq_device(struct ahci_port *port,
					     const struct ahci_eh_ctx *c)
{
	if (c) {
		if (c->fbs_dev >= 0 && port->pmp_attached &&
		    c->fbs_dev < (int)port->nr_pmp_links &&
		    c->fbs_dev < AHCI_PMP_MAX_LINKS)
			return port->pmp_links[c->fbs_dev].dev;
		for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++)
			if ((c->collected & ahci_eh_bit(s)) && c->q[s].ncq_suspect)
				return c->q[s].qc->dev;
	}
	if (!port->pmp_attached)
		return port->host_link.dev;
	for (uint32_t i = 0; i < port->nr_pmp_links && i < AHCI_PMP_MAX_LINKS;
	     i++)
		if (port->pmp_links[i].dev &&
		    (port->pmp_links[i].dev->flags & ATA_DFLAG_NCQ_ON))
			return port->pmp_links[i].dev;
	return NULL;
}

/* Read the log and return the failed tag (-ENOENT when the log says the
 * failing command was not a queued one).  *page receives the log. */
static int ahci_eh_ncq_log_tag(struct ahci_port *port, struct ata_device *dev,
			       uint8_t *page)
{
	int r;

	if (!ahci_eh_dev_usable(dev))
		return -ENODEV;
	r = ahci_eh_read_ncq_log(port, dev, page);
	if (r)
		return r;
	if (page[ATA_NCQ_ERR_TAG_BYTE] & ATA_NCQ_ERR_NQ)
		return -ENOENT;
	return page[ATA_NCQ_ERR_TAG_BYTE] & 0x1F;
}

int ahci_eh_ncq_analyse(struct ahci_port *port)
{
	struct ata_device *dev;
	uint8_t *page;
	int r;

	BUG_ON(!port);
	if (ata_can_sleep())
		might_sleep();
	dev = ahci_eh_ncq_device(port, NULL);
	if (!dev)
		return -ENODEV;
	page = (uint8_t *)kcalloc(1, ATA_SECT_SIZE);
	if (!page)
		return -ENOMEM;
	r = ahci_eh_ncq_log_tag(port, dev, page);
	kfree(page);
	return r;
}

/* Attribute an NCQ error: the tag the log names failed, every other
 * outstanding queued command is an innocent victim. */
static int ahci_eh_ncq_autopsy(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	struct ata_device *dev = ahci_eh_ncq_device(port, c);
	uint8_t *page;
	int tag, found = 0;

	page = (uint8_t *)kcalloc(1, ATA_SECT_SIZE);
	if (!page)
		tag = -ENOMEM;
	else
		tag = ahci_eh_ncq_log_tag(port, dev, page);

	if (tag >= 0) {
		for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
			struct ahci_eh_qc *e = &c->q[s];
			struct ahci_tf_diag d;

			if (!(c->collected & ahci_eh_bit(s)) || !e->ncq_suspect)
				continue;
			e->ncq_suspect = 0;
			if ((int)s != tag)
				continue; /* aborted by the device: innocent */
			found = 1;
			ahci_eh_tf_from_ncq_log(page, &e->qc->result_tf);
			ahci_eh_decode_tf(e->qc->result_tf.status,
					  e->qc->result_tf.error, 0,
					  ahci_qc_default_timeout(e->qc) ==
						  ATA_TMOUT_RW_MS &&
						  e->qc->tf.command ==
							  ATA_CMD_FPDMA_WRITE,
					  &d);
			if (!d.err) {
				/* The log names the tag but no error bit:
				 * still the culprit. */
				d.err = -EIO;
				d.err_mask = AC_ERR_DEV;
				d.why = "NCQ error without status";
			}
			e->blamed = 1;
			e->err = d.err;
			e->err_mask = d.err_mask | AC_ERR_NCQ;
			e->why = d.why;
			/* NCQ autosense (ACS-4): sense key/ASC/ASCQ */
			if ((dev->flags & ATA_DFLAG_NCQ_AUTOSENSE) &&
			    page[ATA_NCQ_ERR_SENSE_KEY]) {
				struct ata_queued_cmd *qc = e->qc;

				kmemset(qc->sense, 0, sizeof(qc->sense));
				qc->sense[0] = 0x70; /* fixed, current */
				qc->sense[2] = page[ATA_NCQ_ERR_SENSE_KEY] & 0x0F;
				qc->sense[7] = 10;
				qc->sense[12] = page[ATA_NCQ_ERR_ASC];
				qc->sense[13] = page[ATA_NCQ_ERR_ASCQ];
				qc->sense_len = 18;
				qc->flags |= ATA_QCFLAG_SENSE_VALID;
			}
		}
		if (!found)
			kprintf(AHCI_PFX "%s: NCQ error log names tag %d, which was not outstanding\n",
				port->name, tag);
		else
			kprintf(AHCI_PFX "%s: NCQ error log: tag %d failed (status 0x%02x error 0x%02x)\n",
				port->name, tag, page[ATA_NCQ_ERR_STATUS],
				page[ATA_NCQ_ERR_ERROR]);
	} else if (tag == -ENOENT) {
		kprintf(AHCI_PFX "%s: NCQ error log: the failing command was not queued\n",
			port->name);
	} else {
		kprintf(AHCI_PFX "%s: NCQ error log unavailable (%d)\n",
			port->name, tag);
		port->ncq_log_failures++;
	}

	/* Without a culprit every suspect is blamed: retried (counted), so
	 * a command that keeps failing eventually fails for good. */
	if (!found) {
		for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
			struct ahci_eh_qc *e = &c->q[s];

			if (!(c->collected & ahci_eh_bit(s)))
				continue;
			if (!e->ncq_suspect && tag >= 0)
				continue;
			if (!e->ncq)
				continue;
			e->ncq_suspect = 0;
			e->blamed = 1;
			e->err = -EIO;
			e->err_mask = AC_ERR_DEV | AC_ERR_NCQ | AC_ERR_OTHER;
			e->why = tag == -ENOENT
					 ? "NCQ error on a non-queued command"
					 : "NCQ error, failed tag unknown";
		}
	}
	if (page)
		kfree(page);
	return tag >= 0 || tag == -ENOENT ? 0 : tag;
}

/* ==========================================================================
 * The recovery ladder
 * ========================================================================== */

static int ahci_eh_can_clo(const struct ahci_port *port)
{
	const struct ahci_hba *hba = port->hba;

	return hba->caps.sclo && !(hba->quirks & AHCI_Q_NO_CLO);
}

/* Wait for BSY and DRQ to clear in PxTFD (polled, sleeping in 1 ms steps
 * when allowed). */
static int ahci_eh_wait_ready(struct ahci_port *port, uint32_t timeout_ms)
{
	uint32_t last = 0;
	int r;

	r = ahci_wait_reg(port->mmio, AHCI_PxTFD, ATA_BUSY | ATA_DRQ, 0,
			  timeout_ms, 1000, &last);
	if (r)
		kprintf(AHCI_PFX "%s: device not ready after %u ms (PxTFD 0x%08x)\n",
			port->name, timeout_ms, last);
	return r;
}

/* Rung ABORT: stop the engine, drop zombies, clear errors, restart.
 * -EBUSY when the device still asserts BSY/DRQ (needs a reset). */
static int ahci_eh_abort(struct ahci_port *port)
{
	uint32_t tfd;
	uint64_t fl;
	int r;

	port->stats.aborts++;
	r = ahci_port_stop_engine(port);
	if (r) {
		kprintf(AHCI_PFX "%s: engine stop failed (%d), PxCMD 0x%08x\n",
			port->name, r, ahci_port_readl(port, AHCI_PxCMD));
		return r;
	}
	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_cmd_reap_zombies_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
	ahci_eh_clear_errors(port);

	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if (tfd & (ATA_BUSY | ATA_DRQ))
		return -EBUSY;
	return ahci_port_start_engine(port);
}

/* Rung CLO: as ABORT, overriding a stuck BSY/DRQ.  CLO only makes the
 * command list usable again -- it does not make the device ready -- so a
 * stuck device always continues with the software reset. */
static int ahci_eh_clo(struct ahci_port *port)
{
	uint32_t tfd;
	uint64_t fl;
	int r, was_busy;

	r = ahci_port_stop_engine(port);
	if (r)
		return r;
	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_cmd_reap_zombies_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
	ahci_eh_clear_errors(port);

	tfd = ahci_port_readl(port, AHCI_PxTFD);
	was_busy = !!(tfd & (ATA_BUSY | ATA_DRQ));
	if (was_busy) {
		if (!ahci_eh_can_clo(port))
			return -EOPNOTSUPP;
		r = ahci_port_clo(port);
		port->stats.clo++;
		if (r) {
			kprintf(AHCI_PFX "%s: command list override failed (%d)\n",
				port->name, r);
			return r;
		}
	}
	r = ahci_port_start_engine(port);
	if (r)
		return r;
	if (was_busy) {
		kprintf(AHCI_PFX "%s: BSY/DRQ was stuck (PxTFD 0x%08x): software reset follows\n",
			port->name, tfd);
		return -EAGAIN;
	}
	return 0;
}

/* The signature a reset produced: the D2H Register FIS of the link, else
 * PxSIG (host link). */
static uint32_t ahci_eh_read_sig(struct ahci_port *port, int pmp)
{
	struct ahci_rx_fis *rx = ahci_rx_fis_of(port, pmp);
	struct fis_reg_d2h d2h;

	ahci_rmb();
	if (rx) {
		const volatile uint8_t *s =
			(const volatile uint8_t *)&rx->rfis;
		uint8_t *d = (uint8_t *)&d2h;

		for (size_t i = 0; i < sizeof(d2h); i++)
			d[i] = s[i];
		if (d2h.fis_type == FIS_TYPE_REG_D2H)
			return (uint32_t)d2h.count_lo |
			       ((uint32_t)d2h.lba0 << 8) |
			       ((uint32_t)d2h.lba1 << 16) |
			       ((uint32_t)d2h.lba2 << 24);
	}
	return ahci_port_readl(port, AHCI_PxSIG);
}

/* Prepare the command list for a reset FIS: engine stopped, BSY/DRQ
 * overridden where needed, errors cleared, engine started. */
static int ahci_eh_prepare_reset_fis(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	uint32_t tfd;
	uint64_t fl;
	int r;

	r = ahci_port_stop_engine(port);
	if (r)
		return r;
	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_cmd_reap_zombies_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if ((tfd & (ATA_BUSY | ATA_DRQ)) && !(hba->quirks & AHCI_Q_SRST_NO_CLO)) {
		if (!ahci_eh_can_clo(port))
			return -EOPNOTSUPP; /* only COMRESET helps */
		r = ahci_port_clo(port);
		port->stats.clo++;
		if (r)
			return r;
	}
	ahci_eh_clear_errors(port);
	return ahci_port_start_engine(port);
}

/* Software reset of one link (AHCI 1.3.1 section 10.4.1): two Register
 * H2D FISes on the command list -- the first with Control.SRST set and
 * the header's R (reset) and C (clear busy upon R_OK) bits, the second
 * with SRST clear -- then the device's signature D2H FIS. */
static int ahci_eh_softreset_link(struct ahci_port *port, int pmp,
				  uint32_t *sig_out)
{
	struct ahci_hba *hba = port->hba;
	struct fis_reg_h2d fis;
	int tries = (hba->quirks & AHCI_Q_SRST_RETRY) ? 2 : 1;
	int r = -EIO;

	for (int t = 0; t < tries; t++) {
		r = ahci_eh_prepare_reset_fis(port);
		if (r)
			continue;

		kmemset(&fis, 0, sizeof(fis));
		fis.fis_type = FIS_TYPE_REG_H2D;
		fis.pm_c = (uint8_t)(pmp & FIS_PMP_MASK); /* C = 0: control */
		fis.control = ATA_SRST;
		r = ahci_exec_raw_fis(port, pmp, &fis,
				      AHCI_CMDH_R | AHCI_CMDH_C,
				      AHCI_SRST_TIMEOUT_MS);
		if (r) {
			kprintf(AHCI_PFX "%s: SRST assert (pmp %d) failed: %d\n",
				port->name, pmp, r);
			continue;
		}
		/* SRST must stay asserted at least 5 us */
		ahci_udelay(AHCI_SRST_ASSERT_US);

		fis.control = 0;
		r = ahci_exec_raw_fis(port, pmp, &fis, 0, AHCI_SRST_TIMEOUT_MS);
		if (r) {
			kprintf(AHCI_PFX "%s: SRST release (pmp %d) failed: %d\n",
				port->name, pmp, r);
			continue;
		}
		r = ahci_eh_wait_ready(port, AHCI_DEV_READY_MS);
		if (r)
			continue;
		if (sig_out)
			*sig_out = ahci_eh_read_sig(port, pmp);
		return 0;
	}
	return r;
}

/* DEVICE RESET (08h) for an ATAPI device: resets the packet device's
 * state machine without touching the link. */
static int ahci_eh_device_reset(struct ahci_port *port, struct ahci_link *link,
				uint32_t *sig_out)
{
	struct fis_reg_h2d fis;
	int pmp = link->is_host ? 0 : link->pmp;
	int r;

	r = ahci_eh_prepare_reset_fis(port);
	if (r)
		return r;
	kmemset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_REG_H2D;
	fis.pm_c = (uint8_t)((pmp & FIS_PMP_MASK) | FIS_H2D_C);
	fis.command = ATA_CMD_DEV_RESET;
	r = ahci_exec_raw_fis(port, pmp, &fis, 0, AHCI_SRST_TIMEOUT_MS);
	if (r)
		return r;
	r = ahci_eh_wait_ready(port, AHCI_DEV_READY_MS);
	if (r)
		return r;
	if (sig_out)
		*sig_out = ahci_eh_read_sig(port, pmp);
	return 0;
}

/* Rung SRST: DEVICE RESET for ATAPI devices, a software reset of every
 * link otherwise (port multiplier: its control port first, then each
 * fan-out link that has a device). */
static int ahci_eh_srst(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	struct ahci_link *links[AHCI_PMP_MAX_LINKS + 1];
	uint32_t sig = 0;
	int n, r = 0, rr;

	port->stats.srst++;
	if (!port->pmp_attached) {
		struct ahci_link *link = &port->host_link;

		if (link->class == ATA_DEV_ATAPI) {
			rr = ahci_eh_device_reset(port, link, &sig);
			if (rr == 0) {
				kprintf(AHCI_PFX "%s: ATAPI DEVICE RESET done (sig 0x%08x)\n",
					port->name, sig);
				goto done_host;
			}
			kprintf(AHCI_PFX "%s: ATAPI DEVICE RESET failed (%d), software reset\n",
				port->name, rr);
		}
		r = ahci_eh_softreset_link(port, 0, &sig);
		if (r)
			return r;
done_host:
		link->n_resets++;
		link->last_reset_ns = hrtimer_now_ns();
		link->sig = sig;
		if (link->dev && link->dev->magic == ATA_DEVICE_MAGIC)
			link->dev->n_resets++;
		if (ata_class_from_sig(sig) == ATA_DEV_NONE) {
			kprintf(AHCI_PFX "%s: no device answered the reset (sig 0x%08x)\n",
				port->name, sig);
			if (c)
				c->link_gone = 1;
			return -ENODEV;
		}
		if (link->class != ATA_DEV_UNKNOWN &&
		    ata_class_from_sig(sig) != link->class)
			kprintf(AHCI_PFX "%s: device class changed over reset: %s -> %s\n",
				port->name, ata_class_name(link->class),
				ata_class_name(ata_class_from_sig(sig)));
		if (c) {
			c->srst_done = 1;
			c->srst_result = 0;
			c->srst_sig = sig;
		}
		return 0;
	}

	/* Port multiplier: the control port, then the devices behind it */
	r = ahci_eh_softreset_link(port, ATA_PMP_CTRL_PORT, &sig);
	if (r) {
		kprintf(AHCI_PFX "%s: port multiplier software reset failed (%d)\n",
			port->name, r);
		return r;
	}
	if (c) {
		c->srst_done = 1;
		c->srst_result = 0;
		c->srst_sig = sig;
	}
	n = ahci_eh_links(port, links, AHCI_PMP_MAX_LINKS + 1);
	for (int i = 0; i < n; i++) {
		struct ahci_link *link = links[i];
		uint32_t lsig = 0;

		if (!link->dev)
			continue;
		rr = ahci_eh_softreset_link(port, link->pmp, &lsig);
		link->n_resets++;
		link->last_reset_ns = hrtimer_now_ns();
		if (rr) {
			kprintf(AHCI_PFX "%s: software reset of %s failed (%d)\n",
				port->name, link->name, rr);
			r = rr;
			continue;
		}
		link->sig = lsig;
	}
	return r;
}

/* Rung COMRESET: hard reset of the port's link through SControl, wait
 * ready, classify; a port multiplier's fan-out links are reset through
 * their PSCR afterwards. */
static int ahci_eh_comreset(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	enum ata_class cls = ATA_DEV_UNKNOWN;
	struct ahci_link *link = &port->host_link;
	int r;

	/* stats.comreset and link->n_resets are counted by the COMRESET
	 * itself (ahci_port_comreset) */
	(void)ahci_port_stop_engine(port);
	/* The attempt's deadline needs the clock (and the worker that runs
	 * on it); the boot probe's polled run counts its waits instead. */
	if (c && c->reset_timeout_ms && port->hba->kahci_running)
		r = ahci_port_hardreset_deadline(
			port, &cls, c->reset_timeout_ms,
			(c->actions & AHCI_EH_HOTPLUG) != 0);
	else
		r = ahci_port_hardreset(port, &cls);
	if (r == -ENODEV || (r == 0 && cls == ATA_DEV_NONE)) {
		kprintf(AHCI_PFX "%s: link down after COMRESET\n", port->name);
		if (c)
			c->link_gone = 1;
		return -ENODEV;
	}
	if (r) {
		kprintf(AHCI_PFX "%s: COMRESET failed (%d), SStatus 0x%x\n",
			port->name, r, ahci_port_readl(port, AHCI_PxSSTS));
		return r;
	}
	if (!(ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_ST)) {
		r = ahci_port_start_engine(port);
		if (r) {
			kprintf(AHCI_PFX "%s: engine start after COMRESET failed (%d)\n",
				port->name, r);
			return r;
		}
	}
	ahci_eh_clear_errors(port);
	if (link->dev && link->dev->magic == ATA_DEVICE_MAGIC)
		link->dev->n_resets++;
	if (link->class != ATA_DEV_UNKNOWN && cls != ATA_DEV_UNKNOWN &&
	    cls != link->class)
		kprintf(AHCI_PFX "%s: device class changed over COMRESET: %s -> %s\n",
			port->name, ata_class_name(link->class),
			ata_class_name(cls));

	if (port->pmp_attached && cls == ATA_DEV_PMP) {
		/* The multiplier is recovered as a whole: revalidated (with
		 * its own tries and resets), its disabled links given their
		 * quick reset, every other fan-out link recovered with its
		 * tries, the devices revalidated or attached.  Given up, the
		 * multiplier is detached. */
		int rr = ahci_pmp_recover(port, 0);

		if (rr)
			kprintf(AHCI_PFX "%s: port multiplier recovery failed (%d)\n",
				port->name, rr);
		if (c)
			c->pmp_recovered = 1;
	}
	if (c) {
		c->srst_done = 1; /* a hard reset satisfies a soft request */
		c->srst_result = 0;
		c->srst_sig = ahci_port_readl(port, AHCI_PxSIG);
	}
	return 0;
}

static void ahci_eh_disable_dev(struct ahci_port *port, struct ahci_link *link,
				struct ata_device *dev, void *arg)
{
	(void)port;
	(void)link;
	(void)arg;
	if (!(dev->flags & ATA_DFLAG_DISABLED))
		kprintf(AHCI_PFX "%s: device disabled by error handling\n",
			dev->name);
	dev->flags |= ATA_DFLAG_DISABLED;
}

/* Rung GIVE_UP: the port is beyond recovery. */
static void ahci_eh_give_up(struct ahci_port *port)
{
	uint64_t fl;

	kprintf(AHCI_PFX "%s: recovery failed, disabling the device(s)\n",
		port->name);
	(void)ahci_port_stop_engine_abort(port);
	ahci_port_fail_all(port, -EIO);
	ahci_eh_for_each_dev(port, ahci_eh_disable_dev, NULL);
	/* The port itself stays usable (thawed at the end of the run), so
	 * that the next PHY event -- a device exchanged -- is seen. */
	spin_lock_irqsave(&port->lock, &fl);
	if (port->state == AHCI_PORT_FROZEN)
		port->state = AHCI_PORT_EH;
	spin_unlock_irqrestore(&port->lock, fl);
}

static int ahci_eh_recover_ctx(struct ahci_port *port,
			       enum ahci_eh_level level, struct ahci_eh_ctx *c)
{
	switch (level) {
	case AHCI_EH_LVL_NONE:
	case AHCI_EH_LVL_RETRY:
		return 0;
	case AHCI_EH_LVL_ABORT:
		return ahci_eh_abort(port);
	case AHCI_EH_LVL_CLO:
		return ahci_eh_clo(port);
	case AHCI_EH_LVL_SRST:
		return ahci_eh_srst(port, c);
	case AHCI_EH_LVL_COMRESET:
		return ahci_eh_comreset(port, c);
	case AHCI_EH_LVL_HBA_RESET:
		/* Needs every port's eh_mutex: requested from kahci, which
		 * runs ahci_hba_recover() with no eh_mutex held. */
		{
			uint64_t fl;

			spin_lock_irqsave(&port->lock, &fl);
			port->eh_pending |= AHCI_EH_HBA_RESET;
			spin_unlock_irqrestore(&port->lock, fl);
			ahci_kahci_kick(port->hba, AHCI_WORK_HBA_RESET);
		}
		return -EAGAIN;
	case AHCI_EH_LVL_GIVE_UP:
		ahci_eh_give_up(port);
		return -EIO;
	default:
		return -EINVAL;
	}
}

int ahci_eh_recover(struct ahci_port *port, enum ahci_eh_level level)
{
	BUG_ON(!port);
	if (ata_can_sleep())
		might_sleep();
	return ahci_eh_recover_ctx(port, level, NULL);
}

/* Climb from `from' until a rung succeeds.  Returns 0 recovered, -EAGAIN
 * when the controller reset is needed (c->need_hba_reset), -ENODEV when
 * the device is gone, other -errno after giving up. */

/* One rung, accounted. */
static int ahci_eh_rung(struct ahci_port *port, struct ahci_eh_ctx *c,
			enum ahci_eh_level lvl)
{
	c->reached = lvl;
	port->eh_level = lvl;
	port->eh_ladder_hist[lvl]++;
	ahci_trace(port, AHCI_TRACE_RESET, NULL, 0, (uint32_t)lvl);
	return ahci_eh_recover_ctx(port, lvl, c);
}

/* Recovery from `from' on: the engine stopped and restarted (abort), or
 * a software reset where one was asked for; failing that, or when a reset
 * is due, COMRESET -- tried AHCI_EH_RESET_TRIES times, the last time at a
 * lower link speed; when that fails too the devices are given up
 * (disabled), the port stays as it is for the next PHY event.  The
 * controller is never reset from here: that is asked for explicitly
 * (AHCI_EH_HBA_RESET) and run by kahci. */
static int ahci_eh_climb(struct ahci_port *port, struct ahci_eh_ctx *c,
			 enum ahci_eh_level from)
{
	int r = -EIO;

	if (from == AHCI_EH_LVL_HBA_RESET) {
		/* The controller reset needs every port's eh_mutex: it is
		 * kahci's job (ahci_hba_recover), requested by
		 * AHCI_EH_HBA_RESET.  Without kahci (boot) there is none. */
		if (port->hba->kahci_running) {
			c->need_hba_reset = 1;
			c->reached = AHCI_EH_LVL_HBA_RESET;
			port->eh_level = AHCI_EH_LVL_HBA_RESET;
			port->eh_ladder_hist[AHCI_EH_LVL_HBA_RESET]++;
			return -EAGAIN;
		}
		kprintf(AHCI_PFX "%s: no controller reset before the worker runs\n",
			port->name);
		from = AHCI_EH_LVL_COMRESET;
	}
	if (from == AHCI_EH_LVL_GIVE_UP)
		goto give_up;

	if (from <= AHCI_EH_LVL_ABORT || from == AHCI_EH_LVL_SRST) {
		enum ahci_eh_level lvl = from == AHCI_EH_LVL_SRST
						 ? AHCI_EH_LVL_SRST
						 : AHCI_EH_LVL_ABORT;

		r = ahci_eh_rung(port, c, lvl);
		if (lvl == AHCI_EH_LVL_SRST) {
			port->eh_last_reset_ns = hrtimer_now_ns();
			port->eh_run_did_reset = 1;
		}
		if (r == 0) {
			if (lvl == AHCI_EH_LVL_SRST)
				c->did_reset = 1;
			return 0;
		}
		if (r == -ENODEV) {
			c->link_gone = 1;
			return r;
		}
		kprintf(AHCI_PFX "%s: %s failed (%d), resetting the link\n",
			port->name, ahci_eh_level_name(lvl), r);
	}

	/* Resets within one run are AHCI_EH_RESET_COOL_DOWN_MS apart. */
	if (port->eh_run_did_reset) {
		uint64_t until = port->eh_last_reset_ns +
				 (uint64_t)AHCI_EH_RESET_COOL_DOWN_MS * 1000000ULL;
		uint64_t now = hrtimer_now_ns();

		if (now < until)
			ahci_msleep((uint32_t)((until - now + 999999ULL) /
					       1000000ULL));
	}
	for (int try = 0; try < AHCI_EH_RESET_TRIES; try++) {
		uint64_t deadline, fl;

		/* Each attempt has its own deadline: the device must be
		 * ready by then.  The port is frozen across the reset (its
		 * PHY events are the reset's own) and thawed once it
		 * succeeded -- or after the last attempt, so the next PHY
		 * event is seen. */
		spin_lock_irqsave(&port->lock, &fl);
		ahci_eh_freeze_locked(port);
		spin_unlock_irqrestore(&port->lock, fl);
		port->eh_last_reset_ns = hrtimer_now_ns();
		port->eh_run_did_reset = 1;
		deadline = port->eh_last_reset_ns +
			   (uint64_t)ahci_eh_reset_timeouts[try] * 1000000ULL;
		c->reset_timeout_ms = ahci_eh_reset_timeouts[try];
		r = ahci_eh_rung(port, c, AHCI_EH_LVL_COMRESET);
		c->reset_timeout_ms = 0;
		if (r == 0 || r == -ENODEV || try + 1 >= AHCI_EH_RESET_TRIES) {
			spin_lock_irqsave(&port->lock, &fl);
			ahci_eh_callee_unmask_locked(port);
			spin_unlock_irqrestore(&port->lock, fl);
		}
		if (r == 0) {
			c->did_reset = 1;
			return 0;
		}
		if (r == -ENODEV) {
			c->link_gone = 1;
			return r;
		}
		/* The link would not settle: a lower speed next time; the
		 * last attempt runs at a lower speed in any case. */
		if (r == -EPIPE || try + 2 == AHCI_EH_RESET_TRIES)
			(void)ahci_port_down_spd_limit(port);
		if (try + 1 < AHCI_EH_RESET_TRIES) {
			/* The next attempt only once this one's deadline
			 * has passed. */
			uint64_t now = hrtimer_now_ns();
			uint32_t secs = 0;

			if (now < deadline)
				secs = (uint32_t)((deadline - now + 999999999ULL) /
						  1000000000ULL);
			kprintf(AHCI_PFX "%s: COMRESET failed (%d), retrying in %u secs\n",
				port->name, r, secs);
			if (now < deadline)
				ahci_msleep((uint32_t)((deadline - now + 999999ULL) /
						       1000000ULL));
		} else {
			kprintf(AHCI_PFX "%s: COMRESET failed (%d)\n",
				port->name, r);
		}
	}
	kprintf(AHCI_PFX "%s: reset failed, giving up\n", port->name);
give_up:
	(void)ahci_eh_rung(port, c, AHCI_EH_LVL_GIVE_UP);
	return r ? r : -EIO;
}

/* ==========================================================================
 * Calls into the ATA layer and the port code from EH
 *
 * Revalidation, hot-plug probing and LPM changes issue ordinary
 * commands and wait for them the ordinary way (interrupt or poll).  For
 * the time of such a call the port runs with interrupts on; EH stays the
 * owner (eh_running), so a failure of those commands completes them at
 * once instead of waiting for an EH run that cannot happen.
 * ========================================================================== */

void ahci_eh_callee_unmask_locked(struct ahci_port *port)
{
	BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	WARN_ON_ONCE(!port->eh_running);
	/* Interrupts on, so the callee's commands complete the ordinary
	 * way -- but the port stays in error handling (AHCI_PORT_EH): a
	 * RUNNING port would admit ordinary commands, issue the deferred
	 * list and let the ATA layer's parked continuations out, all of
	 * which would then fall under the rules for EH's own commands
	 * (failed at once on any error, aborted by the engine kicks).  EH's
	 * own context is admitted in state EH anyway, and the interrupt path
	 * keeps completing commands there. */
	if (port->state == AHCI_PORT_EH || port->state == AHCI_PORT_FROZEN) {
		ahci_eh_unmask_locked(port);
		port->state = AHCI_PORT_EH;
	}
}

static void ahci_eh_callee_begin(struct ahci_port *port)
{
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	ahci_eh_callee_unmask_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
}

static void ahci_eh_callee_end(struct ahci_port *port)
{
	uint64_t fl;

	ahci_cmd_run_deferred_completions(port);
	spin_lock_irqsave(&port->lock, &fl);
	if (port->state == AHCI_PORT_RUNNING || port->state == AHCI_PORT_IDLE)
		port->state = AHCI_PORT_EH;
	spin_unlock_irqrestore(&port->lock, fl);
	/* A callee's command may have failed and halted the engine. */
	if (port->zombie_slots)
		(void)ahci_cmd_kick_engine(port);
}

struct ahci_eh_reval_arg {
	struct ahci_eh_ctx *c;
	int failures;
};

/* Lower a link's speed limit by one generation (effective at its next
 * COMRESET): the host link's through the port, a fan-out link's own. */
static void ahci_eh_link_down_spd(struct ahci_port *port,
				  struct ahci_link *link)
{
	uint8_t cur;

	if (link->is_host) {
		(void)ahci_port_down_spd_limit(port);
		return;
	}
	cur = link->speed_limit ? link->speed_limit
				: (uint8_t)AHCI_SSTS_SPD(link->sstatus);
	if (cur <= AHCI_SSTS_SPD_GEN1 || cur > AHCI_SSTS_SPD_GEN3)
		return;
	link->speed_limit = (uint8_t)(cur - 1);
	link->n_speed_downs++;
}

static void ahci_eh_revalidate_one(struct ahci_port *port,
				   struct ahci_link *link,
				   struct ata_device *dev, void *argp)
{
	struct ahci_eh_reval_arg *arg = (struct ahci_eh_reval_arg *)argp;
	int r;

	if (!ahci_eh_dev_usable(dev) || !(dev->flags & ATA_DFLAG_CONFIGURED))
		return;
	dev->n_revalidations++;
	ahci_eh_callee_begin(port);
	r = ata_dev_revalidate(dev);
	ahci_eh_callee_end(port);
	if (r == 0)
		return;
	if (r == -ENODEV) {
		kprintf(AHCI_PFX "%s: gone after reset, detaching\n", dev->name);
		/* Off the link as well, with the link's reference dropped:
		 * the disk is deleted (and freed) by the registration worker,
		 * and a device left on the link would have later EH runs
		 * reach the freed disk through it. */
		ahci_link_detach(link);
		return;
	}
	kprintf(AHCI_PFX "%s: revalidation failed (%d)\n", dev->name, r);
	/* Another reset and revalidation while the device has tries left
	 * in this run (the last one at a lower link speed); out of them,
	 * it is disabled. */
	if (link->eh_dev_tries)
		link->eh_dev_tries--;
	if (link->eh_dev_tries == 1)
		ahci_eh_link_down_spd(port, link);
	if (!link->eh_dev_tries)
		ahci_eh_disable_dev(port, link, dev, NULL);
	else
		arg->failures++;
}

/* After a reset: re-IDENTIFY and reconfigure every configured device.  A
 * failure that leaves the device tries asks for a hard reset and another
 * revalidation in the next round. */
static void ahci_eh_revalidate_all(struct ahci_port *port,
				   struct ahci_eh_ctx *c)
{
	struct ahci_eh_reval_arg arg = { .c = c, .failures = 0 };
	uint64_t fl;

	ahci_eh_for_each_dev(port, ahci_eh_revalidate_one, &arg);
	if (!arg.failures)
		return;
	spin_lock_irqsave(&port->lock, &fl);
	port->eh_pending |= AHCI_EH_RESET_HARD;
	spin_unlock_irqrestore(&port->lock, fl);
}

/* ==========================================================================
 * Policies: link speed and NCQ after repeated errors
 * ========================================================================== */

static void ahci_eh_speed_down(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	struct ahci_link *link = &port->host_link;
	uint64_t fl;

	if (!c->bus_errors || port->pmp_attached)
		return;
	link->n_link_errors++;
	if (link->n_link_errors < AHCI_EH_SPEED_DOWN_ERRORS)
		return;
	kprintf(AHCI_PFX "%s: %u link errors\n", port->name,
		link->n_link_errors);
	link->n_link_errors = 0;
	/* Only the limit is lowered here: the COMRESET that applies it is
	 * error handling's own, in the next round -- the port frozen across
	 * it (its PHY events are not hot-plug events), the device
	 * revalidated after it. */
	if (ahci_port_down_spd_limit(port))
		return; /* nothing slower to try */
	spin_lock_irqsave(&port->lock, &fl);
	port->eh_pending |= AHCI_EH_RESET_HARD;
	spin_unlock_irqrestore(&port->lock, fl);
}

static void ahci_eh_ncq_off_one(struct ahci_port *port,
				struct ahci_link *link,
				struct ata_device *dev, void *arg)
{
	(void)port;
	(void)link;
	(void)arg;
	if (!(dev->flags & ATA_DFLAG_NCQ_ON))
		return;
	dev->flags &= ~ATA_DFLAG_NCQ_ON;
	dev->ncq_depth = 0;
	if (dev->disk && dev->disk->queue)
		blk_queue_set_depth(dev->disk->queue, 1);
	kprintf(AHCI_PFX "%s: NCQ disabled after repeated NCQ errors\n",
		dev->name);
}

static void ahci_eh_ncq_policy(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	int hsm = 0;

	if (!(c->actions & AHCI_EH_NCQ_ERROR))
		return;
	for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
		const struct ahci_eh_qc *e = &c->q[s];

		if (!(c->collected & ahci_eh_bit(s)) || !e->blamed || !e->ncq)
			continue;
		/* media errors are the disk's problem, not NCQ's */
		if (e->err_mask & (AC_ERR_HSM | AC_ERR_TIMEOUT | AC_ERR_OTHER |
				   AC_ERR_ATA_BUS))
			hsm = 1;
	}
	if (!hsm)
		return;
	port->stats.ncq_errors++;
	port->ncq_errors_since_ok++;
	if (port->ncq_errors_since_ok < AHCI_EH_NCQ_OFF_ERRORS)
		return;
	port->flags |= AHCI_PORT_F_NO_NCQ;
	port->ncq_errors_since_ok = 0;
	ahci_eh_for_each_dev(port, ahci_eh_ncq_off_one, NULL);
}

/* ==========================================================================
 * The error round
 * ========================================================================== */

/* Retry policy of a failed (blamed) command. */
static int ahci_eh_should_retry(const struct ata_queued_cmd *qc,
				uint32_t err_mask)
{
	uint32_t max;

	if (qc->flags & ATA_QCFLAG_EH)
		return 0; /* EH's own commands report to EH */
	if (!(qc->flags & ATA_QCFLAG_RETRY))
		return 0;
	if (qc->req && (qc->req->flags & BLK_REQ_FAILFAST))
		return 0;
	max = qc->max_retries ? qc->max_retries : ATA_MAX_RETRIES;
	if (qc->retries >= max)
		return 0;
	if (err_mask & AC_ERR_INVALID)
		return 0; /* the request itself is wrong */
	if (err_mask & AC_ERR_MEDIA)
		return qc->retries < 1; /* one more read of a weak sector */
	if ((err_mask & AC_ERR_DEV) &&
	    !(err_mask & (AC_ERR_ATA_BUS | AC_ERR_NCQ | AC_ERR_TIMEOUT |
			  AC_ERR_HSM)))
		return 0; /* the device refused it (ABRT) */
	return 1;
}

static void ahci_eh_chain(struct ata_queued_cmd **head,
			  struct ata_queued_cmd **tail,
			  struct ata_queued_cmd *qc)
{
	qc->xport_next = NULL;
	if (*tail)
		(*tail)->xport_next = qc;
	else
		*head = qc;
	*tail = qc;
}

/* Collect every command in flight; the engine is stopped. */
static void ahci_eh_collect(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	uint32_t act;
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_cmd_reap_zombies_locked(port);
	c->tfd_now = ahci_port_readl(port, AHCI_PxTFD);
	act = port->active;
	while (act) {
		uint32_t s = (uint32_t)__builtin_ctz(act);
		uint32_t bit = ahci_eh_bit(s);
		struct ahci_cmd *cmd = &port->cmds[s];
		struct ata_queued_cmd *qc = cmd->qc;
		struct ahci_eh_qc *e = &c->q[s];

		act &= ~bit;
		kmemset(e, 0, sizeof(*e));
		e->slot = (uint8_t)s;
		e->ncq = cmd->is_ncq;
		e->timedout = cmd->state == AHCI_CMD_TIMEDOUT ||
			      (qc && (qc->flags & ATA_QCFLAG_TIMEDOUT));
		e->faulted = !!(port->fault_slots & bit);

		/* The result registers of a non-queued command are in the
		 * received-FIS area now; keep them before anything else
		 * overwrites it. */
		if (qc && !cmd->is_ncq) {
			qc->result_tf.flags =
				qc->tf.flags & (ATA_TFLAG_LBA48 | ATA_TFLAG_LBA);
			ahci_cmd_read_result_tf(port, cmd->pmp, qc->protocol, 0,
						&qc->result_tf);
		}
		port->active &= ~bit;
		port->ncq_active &= ~bit;
		cmd->state = AHCI_CMD_EH;
		(void)ahci_cmd_retire_locked(port, cmd);
		if (!qc)
			continue; /* raw FIS: its poller saw the failure */
		e->qc = qc;
		c->collected |= bit;
	}
	spin_unlock_irqrestore(&port->lock, fl);
}

/* Decide who is to blame for what. */
static void ahci_eh_classify(struct ahci_port *port, struct ahci_eh_ctx *c)
{
	uint32_t a = c->actions;

	for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
		struct ahci_eh_qc *e = &c->q[s];
		struct ata_queued_cmd *qc;
		struct ahci_tf_diag d;
		int atapi, write;

		if (!(c->collected & ahci_eh_bit(s)))
			continue;
		qc = e->qc;
		atapi = ata_prot_is_atapi(qc->protocol);
		write = qc->tf.command == ATA_CMD_FPDMA_WRITE ||
			(qc->flags & ATA_QCFLAG_WRITE) ||
			qc->dma_dir == ATA_DIR_TO_DEVICE;

		if (e->faulted) {
			e->blamed = 1;
			if (a & AHCI_EH_HOST_ERROR) {
				e->err = -EIO;
				e->err_mask = AC_ERR_HOST_BUS;
				e->why = "injected host bus error";
			} else if (a & AHCI_EH_IF_FATAL) {
				e->err = -EIO;
				e->err_mask = AC_ERR_ATA_BUS;
				e->why = "injected link loss";
			} else if (a & (AHCI_EH_DEV_ERROR | AHCI_EH_NCQ_ERROR)) {
				qc->result_tf.status =
					(uint8_t)(c->tfd & AHCI_PxTFD_STS_MASK);
				qc->result_tf.error =
					(uint8_t)((c->tfd & AHCI_PxTFD_ERR_MASK) >>
						  AHCI_PxTFD_ERR_SHIFT);
				ahci_eh_decode_tf(qc->result_tf.status,
						  qc->result_tf.error, atapi,
						  write, &d);
				e->err = d.err ? d.err : -EIO;
				e->err_mask = d.err_mask ? d.err_mask : AC_ERR_DEV;
				e->why = d.why;
			} else {
				e->err = -ETIMEDOUT;
				e->err_mask = AC_ERR_TIMEOUT;
				e->why = "injected timeout";
			}
			continue;
		}
		if (e->timedout) {
			e->blamed = 1;
			e->err = -ETIMEDOUT;
			e->err_mask = AC_ERR_TIMEOUT;
			e->why = "command timeout";
			continue;
		}
		if (a & AHCI_EH_HOST_ERROR) {
			e->blamed = 1;
			e->err = -EIO;
			e->err_mask = AC_ERR_HOST_BUS;
			e->why = (c->irq_stat & AHCI_PxIS_HBFS)
					 ? "host bus fatal error (HBFS)"
					 : "host bus data error (HBDS)";
			continue;
		}
		if (a & AHCI_EH_IF_FATAL) {
			e->blamed = 1;
			e->err = -EIO;
			e->err_mask = AC_ERR_ATA_BUS;
			e->why = "interface fatal error (IFS)";
			continue;
		}
		if ((a & AHCI_EH_DEV_ERROR) && !e->ncq) {
			uint8_t st = qc->result_tf.status;
			uint8_t er = qc->result_tf.error;

			/* PxTFD latched at the error is authoritative when
			 * the FIS copy does not show the error. */
			if (!(st & (ATA_ERR | ATA_DF)) &&
			    (c->tfd & (ATA_ERR | ATA_DF))) {
				st = (uint8_t)(c->tfd & AHCI_PxTFD_STS_MASK);
				er = (uint8_t)((c->tfd & AHCI_PxTFD_ERR_MASK) >>
					       AHCI_PxTFD_ERR_SHIFT);
				qc->result_tf.status = st;
				qc->result_tf.error = er;
			}
			ahci_eh_decode_tf(st, er, atapi, write, &d);
			e->blamed = 1;
			e->err = d.err ? d.err : -EIO;
			e->err_mask = d.err_mask ? d.err_mask : AC_ERR_DEV;
			e->why = d.err ? d.why : "task file error";
			if (d.err_mask & AC_ERR_ATA_BUS)
				c->bus_errors = 1;
			if (qc->dev && qc->dev->magic == ATA_DEVICE_MAGIC) {
				if (d.err_mask & AC_ERR_MEDIA)
					qc->dev->n_media_errors++;
				if (d.err_mask & AC_ERR_ATA_BUS)
					qc->dev->n_bus_errors++;
			}
			continue;
		}
		if ((a & AHCI_EH_OVERFLOW) && !e->ncq) {
			e->blamed = 1;
			e->err = -EIO;
			e->err_mask = AC_ERR_HSM;
			e->why = "data overflow: more data than the PRDT describes (OFS)";
			continue;
		}
		if (a & (AHCI_EH_UNKNOWN_FIS | AHCI_EH_IPMS)) {
			e->blamed = 1;
			e->err = -EIO;
			e->err_mask = AC_ERR_HSM;
			e->why = (a & AHCI_EH_IPMS)
					 ? "incorrect port multiplier status (IPMS)"
					 : "unknown FIS received (UFS)";
			continue;
		}
		if ((a & AHCI_EH_NCQ_ERROR) && e->ncq) {
			e->ncq_suspect = 1;
			continue;
		}
		/* innocent: aborted because something else failed */
		e->err = -ECANCELED;
		e->err_mask = AC_ERR_ABORTED;
		e->why = "aborted by error handling";
	}
	if (a & (AHCI_EH_IF_FATAL | AHCI_EH_IF_NONFATAL))
		c->bus_errors = 1;
	if (c->serror & (AHCI_SERR_DIAG_C | AHCI_SERR_DIAG_D |
			 AHCI_SERR_DIAG_B | AHCI_SERR_DIAG_H |
			 AHCI_SERR_ERR_T | AHCI_SERR_ERR_C))
		c->bus_errors = 1;
	(void)port;
}

/* The rung an error starts on. */
static enum ahci_eh_level ahci_eh_base_level(struct ahci_port *port,
					     struct ahci_eh_ctx *c,
					     int engine_stuck)
{
	uint32_t a = c->actions, all_err_mask = 0;
	int lvl = AHCI_EH_LVL_ABORT;
	uint32_t esc;

	for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
		const struct ahci_eh_qc *e = &c->q[s];
		int io;

		if (!(c->collected & ahci_eh_bit(s)) || !e->blamed)
			continue;
		all_err_mask |= e->err_mask;
		/* A failed I/O command, or another command that failed for
		 * more than the device's refusal, has the device looked at
		 * again. */
		io = e->qc && (e->qc->flags & ATA_QCFLAG_IO);
		if ((io && e->err_mask) || (!io && (e->err_mask & ~AC_ERR_DEV)))
			c->revalidate = 1;
	}

	/* The reset decision: a port that was frozen, a protocol violation
	 * or a timeout among the failed commands, BSY/DRQ left set, a host
	 * bus or link failure, an unknown FIS, a port multiplier error, the
	 * SError analysis asking for it, or an engine that does not stop --
	 * the link is reset, by COMRESET (every AHCI port can do one, and it
	 * is preferred).  A software reset only where one was asked for (an
	 * ATA PASS-THROUGH software reset, sysfs).  Otherwise the command
	 * engine is stopped and restarted (abort). */
	if (c->frozen || (all_err_mask & (AC_ERR_HSM | AC_ERR_TIMEOUT)) ||
	    (c->tfd_now & (ATA_BUSY | ATA_DRQ)) ||
	    (a & (AHCI_EH_HOST_ERROR | AHCI_EH_IF_FATAL | AHCI_EH_UNKNOWN_FIS |
		  AHCI_EH_IPMS | AHCI_EH_RESET_HARD)) ||
	    (c->irq_stat & AHCI_PxIS_HBFS) || engine_stuck)
		lvl = AHCI_EH_LVL_COMRESET;
	else if (a & AHCI_EH_RESET_SOFT)
		lvl = AHCI_EH_LVL_SRST;
	if (a & AHCI_EH_HBA_RESET)
		lvl = AHCI_EH_LVL_HBA_RESET;

	/* Escalate across runs that saw no clean completion. */
	esc = port->eh_consecutive / AHCI_EH_MAX_TRIES;
	if (esc) {
		/* Errors that keep coming: the link is reset (the speed
		 * step-down and the NCQ policy deal with the rest). */
		int e = AHCI_EH_LVL_COMRESET;

		if (e > lvl) {
			kprintf(AHCI_PFX "%s: %u error runs without a clean command, starting at %s\n",
				port->name, port->eh_consecutive,
				ahci_eh_level_name((enum ahci_eh_level)(
					e > AHCI_EH_LVL_GIVE_UP
						? AHCI_EH_LVL_GIVE_UP
						: e)));
			lvl = e;
		}
	}
	if (lvl > AHCI_EH_LVL_GIVE_UP)
		lvl = AHCI_EH_LVL_GIVE_UP;
	return (enum ahci_eh_level)lvl;
}

/* Decide the fate of every collected command. */
static void ahci_eh_dispose(struct ahci_port *port, struct ahci_eh_ctx *c,
			    struct ahci_eh_lists *L)
{
	for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
		struct ahci_eh_qc *e = &c->q[s];
		struct ata_queued_cmd *qc;

		if (!(c->collected & ahci_eh_bit(s)))
			continue;
		qc = e->qc;
		c->collected &= ~ahci_eh_bit(s);

		if (c->link_gone || !ahci_eh_dev_usable(qc->dev)) {
			qc->result = -ENODEV;
			qc->err_mask |= AC_ERR_NODEV_HINT;
			port->eh_failed++;
			ahci_eh_chain(&L->done_head, &L->done_tail, qc);
			continue;
		}
		if (c->reached == AHCI_EH_LVL_GIVE_UP ||
		    port->state == AHCI_PORT_DISABLED) {
			qc->result = e->blamed && e->err ? e->err : -EIO;
			qc->err_mask |= e->blamed ? e->err_mask : AC_ERR_OTHER;
			port->eh_failed++;
			if (e->blamed)
				ahci_eh_report_failed(port, e);
			ahci_eh_chain(&L->done_head, &L->done_tail, qc);
			continue;
		}
		if (!e->blamed) {
			/* innocent victim: retried as is, not counted */
			port->eh_innocent++;
			ahci_eh_chain(&L->retry_head, &L->retry_tail, qc);
			continue;
		}
		if (ahci_eh_should_retry(qc, e->err_mask)) {
			qc->retries++;
			port->stats.retries++;
			port->eh_retried++;
			if (qc->dev && qc->dev->magic == ATA_DEVICE_MAGIC)
				qc->dev->n_retries++;
			kprintf(AHCI_PFX "%s: retrying %s (0x%02x) slot %u after %s (attempt %u)\n",
				port->name, ata_cmd_name(qc->tf.command),
				qc->tf.command, e->slot,
				e->why ? e->why : "error", qc->retries);
			ahci_eh_chain(&L->retry_head, &L->retry_tail, qc);
			continue;
		}
		qc->result = e->err ? e->err : -EIO;
		qc->err_mask |= e->err_mask;
		if (e->timedout)
			qc->flags |= ATA_QCFLAG_TIMEDOUT;
		qc->flags |= ATA_QCFLAG_FAILED;
		port->eh_failed++;
		if (qc->dev && qc->dev->magic == ATA_DEVICE_MAGIC) {
			qc->dev->last_error_ns = hrtimer_now_ns();
			qc->dev->last_error_tf = qc->result_tf;
			qc->dev->last_error_cmd = qc->tf.command;
		}
		ahci_eh_report_failed(port, e);
		ahci_eh_chain(&L->done_head, &L->done_tail, qc);
	}
}

/* Complete the ATA_PROT_SRST requests with what the reset produced. */
static void ahci_eh_finish_srst_requests(struct ahci_port *port,
					 struct ahci_eh_ctx *c,
					 struct ahci_eh_lists *L)
{
	struct ata_queued_cmd *qc;
	uint64_t fl;

	if (!c->srst_done && !c->link_gone)
		return;
	spin_lock_irqsave(&port->lock, &fl);
	qc = port->srst_head;
	port->srst_head = port->srst_tail = NULL;
	spin_unlock_irqrestore(&port->lock, fl);

	while (qc) {
		struct ata_queued_cmd *next = qc->xport_next;
		uint32_t tfd = ahci_port_readl(port, AHCI_PxTFD);

		kmemset(&qc->result_tf, 0, sizeof(qc->result_tf));
		qc->result_tf.nsect = (uint8_t)c->srst_sig;
		qc->result_tf.lbal = (uint8_t)(c->srst_sig >> 8);
		qc->result_tf.lbam = (uint8_t)(c->srst_sig >> 16);
		qc->result_tf.lbah = (uint8_t)(c->srst_sig >> 24);
		qc->result_tf.status = (uint8_t)(tfd & AHCI_PxTFD_STS_MASK);
		qc->result_tf.error = (uint8_t)((tfd & AHCI_PxTFD_ERR_MASK) >>
						AHCI_PxTFD_ERR_SHIFT);
		if (c->link_gone) {
			qc->result = -ENODEV;
			qc->err_mask |= AC_ERR_NODEV_HINT;
		} else {
			qc->result = c->srst_result;
			if (c->srst_result)
				qc->err_mask |= AC_ERR_OTHER;
		}
		ahci_eh_chain(&L->done_head, &L->done_tail, qc);
		qc = next;
	}
}

static void ahci_eh_report_exception(struct ahci_port *port,
				     struct ahci_eh_ctx *c)
{
	char abuf[160], ibuf[96], sbuf[128];

	/* A device error of quiet commands only is an answer, not an
	 * exception: recover the engine without a word.  The interrupt
	 * path judged the commands (c->quiet: nothing noisy latched); the
	 * run's actions must be that device error and nothing more. */
	c->quiet = c->quiet &&
		   ahci_eh_dev_error_only(c->irq_stat, c->serror, c->actions);
	if (c->quiet)
		return;
	/* An injected fault was announced when it was injected; its
	 * failed command is reported in one line by report_failed. */
	if (c->actions & AHCI_EH_FAULT)
		return;

	ahci_eh_actions_str(c->actions, abuf, sizeof(abuf));
	ahci_irq_stat_str(c->irq_stat, ibuf, sizeof(ibuf));
	ahci_serror_str(c->serror, sbuf, sizeof(sbuf));
	kprintf(AHCI_PFX "%s: EH: actions 0x%x %s irq_stat 0x%08x %s SErr 0x%08x %s tfd 0x%x slots 0x%08x (run %u, streak %u)\n",
		port->name, c->actions, abuf, c->irq_stat, ibuf, c->serror,
		sbuf, c->tfd, c->failed_hint, port->stats.eh_runs,
		port->eh_consecutive);
}

/* Errors, timeouts and reset requests. */
static void ahci_eh_handle_errors(struct ahci_port *port, struct ahci_eh_ctx *c,
				  struct ahci_eh_lists *L)
{
	uint32_t a = c->actions;
	enum ahci_eh_level lvl;
	int engine_stuck = 0, r, ncq_suspects = 0;

	ahci_eh_report_exception(port, c);

	/* 1. what the HBA did finish is fine */
	if (!(a & (AHCI_EH_HOST_ERROR | AHCI_EH_IF_FATAL | AHCI_EH_OVERFLOW)))
		(void)ahci_port_complete(port);

	/* From here on EH owns every command still in flight: nothing may
	 * complete them as successes -- in particular not after a single
	 * device error clear (PxFBS.DEC), which makes the HBA drop the
	 * failed device's CI/SACT bits. */
	{
		uint64_t fl;
		uint32_t act;

		spin_lock_irqsave(&port->lock, &fl);
		act = port->active;
		while (act) {
			uint32_t s = (uint32_t)__builtin_ctz(act);
			struct ahci_cmd *cmd = &port->cmds[s];

			act &= ~ahci_eh_bit(s);
			if (cmd->state == AHCI_CMD_TIMEDOUT && cmd->qc)
				cmd->qc->flags |= ATA_QCFLAG_TIMEDOUT;
			cmd->state = AHCI_CMD_EH;
		}
		spin_unlock_irqrestore(&port->lock, fl);
	}

	/* FIS-based switching: the device whose single-device error the
	 * interrupt handler cleared (c->fbs_dev, latched at the round's
	 * start); a frozen port is recovered as a whole. */

	/* 2. stop the engine: every slot is software's now */
	r = ahci_port_stop_engine(port);
	if (r) {
		engine_stuck = 1;
		kprintf(AHCI_PFX "%s: engine does not stop (%d): hard reset needed\n",
			port->name, r);
	}

	/* 3./4. collect, clear */
	ahci_eh_collect(port, c);
	ahci_eh_clear_errors(port);
	ahci_eh_classify(port, c);
	for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++)
		if ((c->collected & ahci_eh_bit(s)) && c->q[s].ncq_suspect)
			ncq_suspects++;

	/* Gone? (hot-unplug while commands were in flight) */
	if (!port->pmp_attached && !ahci_eh_host_link_online(port)) {
		kprintf(AHCI_PFX "%s: link offline (SStatus 0x%x): device gone\n",
			port->name, ahci_port_readl(port, AHCI_PxSSTS));
		c->link_gone = 1;
		c->actions |= AHCI_EH_HOTPLUG;
		port->eh_pending |= AHCI_EH_HOTPLUG;
		goto dispose;
	}

	/* 5./6. the ladder.  An error run counts towards escalation. */
	if (c->collected || (a & AHCI_EH_ERRORS))
		port->eh_consecutive++;
	lvl = ahci_eh_base_level(port, c, engine_stuck);

	if (lvl <= AHCI_EH_LVL_ABORT) {
		c->reached = AHCI_EH_LVL_ABORT;
		port->eh_level = AHCI_EH_LVL_ABORT;
		port->eh_ladder_hist[AHCI_EH_LVL_ABORT]++;
		r = ahci_eh_abort(port);
		if (r == 0) {
			/* 7. NCQ autopsy before any reset clears the log */
			if (ncq_suspects) {
				r = ahci_eh_ncq_autopsy(port, c);
				if (r == -EIO || r == -ETIMEDOUT) {
					kprintf(AHCI_PFX "%s: device does not answer after the NCQ error, resetting\n",
						port->name);
					r = ahci_eh_climb(port, c,
							  AHCI_EH_LVL_SRST);
				} else {
					r = 0;
				}
			}
		} else {
			r = ahci_eh_climb(port, c, AHCI_EH_LVL_CLO);
		}
	} else {
		if (ncq_suspects) {
			/* The link is reset first and the NCQ Command Error
			 * log is not read (the port was frozen, or the
			 * device needs the reset): no queued command is
			 * known to have failed, every one is retried as it
			 * is. */
			for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
				struct ahci_eh_qc *e = &c->q[s];

				if (!(c->collected & ahci_eh_bit(s)) ||
				    !e->ncq_suspect)
					continue;
				e->ncq_suspect = 0;
				e->err = -ECANCELED;
				e->err_mask = AC_ERR_ABORTED;
				e->why = "aborted by error handling";
			}
		}
		r = ahci_eh_climb(port, c, lvl);
	}
	c->srst_result = r == 0 ? c->srst_result : r;
	port->eh_last_result = r == -EAGAIN ? 0 : r;
	if (r == -EAGAIN && c->need_hba_reset) {
		kprintf(AHCI_PFX "%s: escalating to a controller reset\n",
			port->name);
	} else if (r && r != -ENODEV) {
		kprintf(AHCI_PFX "%s: recovery ended at %s with %d\n",
			port->name, ahci_eh_level_name(c->reached), r);
	} else if (r == 0 && !c->quiet) {
		kprintf(AHCI_PFX "%s: recovered at %s\n", port->name,
			ahci_eh_level_name(c->reached));
	}

	/* Policies: link speed and NCQ */
	if (!c->link_gone && !c->need_hba_reset && r == 0) {
		ahci_eh_speed_down(port, c);
		ahci_eh_ncq_policy(port, c);
	}

	/* Revalidate what the reset touched -- or, without a reset, the
	 * device whose failed commands ask for it.  A hot-plug event's
	 * attach / detach / revalidation follows in this round
	 * (ahci_port_hotplug), once for both. */
	if ((c->did_reset || c->revalidate) && !c->link_gone &&
	    !c->need_hba_reset && port->state != AHCI_PORT_DISABLED &&
	    !(c->actions & AHCI_EH_HOTPLUG) && !c->pmp_recovered)
		ahci_eh_revalidate_all(port, c);

dispose:
	if (c->need_hba_reset) {
		/* Hold everything across the controller reset. */
		for (uint32_t s = 0; s < AHCI_MAX_SLOTS; s++) {
			struct ahci_eh_qc *e = &c->q[s];

			if (!(c->collected & ahci_eh_bit(s)))
				continue;
			c->collected &= ~ahci_eh_bit(s);
			if (e->blamed && !ahci_eh_should_retry(e->qc,
							       e->err_mask)) {
				e->qc->result = e->err ? e->err : -EIO;
				e->qc->err_mask |= e->err_mask;
				ahci_eh_report_failed(port, e);
				ahci_eh_chain(&L->done_head, &L->done_tail,
					      e->qc);
				continue;
			}
			if (e->blamed)
				e->qc->retries++;
			ahci_eh_chain(&L->retry_head, &L->retry_tail, e->qc);
		}
		return;
	}
	ahci_eh_dispose(port, c, L);
	ahci_eh_finish_srst_requests(port, c, L);
}

/* SError / INFS only: nothing failed, just account and clear. */
static void ahci_eh_handle_nonfatal(struct ahci_port *port,
				    struct ahci_eh_ctx *c)
{
	char sbuf[128];

	ahci_serror_str(c->serror, sbuf, sizeof(sbuf));
	kprintf(AHCI_PFX "%s: non-fatal interface error, SErr 0x%08x %s\n",
		port->name, c->serror, sbuf);
	ahci_eh_clear_errors(port);
	/* The port kept running: clearing its status may have taken a
	 * completion interrupt with it.  Collect what finished. */
	(void)ahci_port_complete(port);
	c->bus_errors = 1;
	port->host_link.n_link_errors++;
}

static void ahci_eh_detach_one(struct ahci_port *port, struct ahci_link *link,
			       struct ata_device *dev, void *arg)
{
	(void)port;
	(void)arg;
	if (!(dev->flags & ATA_DFLAG_DETACHED))
		kprintf(AHCI_PFX "%s: detaching\n", dev->name);
	/* Device detached and taken off the link (reference dropped), as
	 * every other removal does: nothing may reach its disk through the
	 * link once the registration worker deleted it. */
	ahci_link_detach(link);
}

static void ahci_eh_lpm(struct ahci_port *port)
{
	struct ahci_link *links[AHCI_PMP_MAX_LINKS + 1];
	int n, r;

	if (port->hba->quirks & AHCI_Q_NO_LPM)
		return;
	ahci_eh_callee_begin(port);
	if (!port->pmp_attached) {
		r = ahci_port_set_lpm(port, &port->host_link, port->lpm_policy);
		if (r && r != -EOPNOTSUPP)
			kprintf(AHCI_PFX "%s: LPM policy %d failed (%d)\n",
				port->name, (int)port->lpm_policy, r);
	} else {
		n = ahci_eh_links(port, links, AHCI_PMP_MAX_LINKS + 1);
		for (int i = 0; i < n; i++) {
			if (!links[i]->dev)
				continue;
			r = ahci_port_set_lpm(port, links[i],
					      links[i]->lpm_policy);
			if (r && r != -EOPNOTSUPP)
				kprintf(AHCI_PFX "%s: LPM policy on %s failed (%d)\n",
					port->name, links[i]->name, r);
		}
	}
	ahci_eh_callee_end(port);
}

/* Does the port hold commands an error interrupt (or a timeout) handed
 * to error handling? */
static int ahci_eh_has_aborted(struct ahci_port *port)
{
	uint64_t fl;
	uint32_t act;
	int found = 0;

	spin_lock_irqsave(&port->lock, &fl);
	act = port->active;
	while (act && !found) {
		uint32_t s = (uint32_t)__builtin_ctz(act);
		const struct ahci_cmd *cmd = &port->cmds[s];

		act &= ~ahci_eh_bit(s);
		found = cmd->qc && (cmd->state == AHCI_CMD_EH ||
				    cmd->state == AHCI_CMD_TIMEDOUT);
	}
	spin_unlock_irqrestore(&port->lock, fl);
	return found;
}

/* One round: everything that was pending when it started. */
static void ahci_eh_round(struct ahci_port *port, struct ahci_eh_ctx *c,
			  struct ahci_eh_lists *L)
{
	uint32_t a = c->actions;
	const uint32_t heavy = (AHCI_EH_ERRORS & ~AHCI_EH_IF_NONFATAL) |
			       AHCI_EH_RESET_SOFT | AHCI_EH_RESET_HARD |
			       AHCI_EH_HBA_RESET;

	if (a & AHCI_EH_DETACH) {
		kprintf(AHCI_PFX "%s: detach requested\n", port->name);
		/* The run started once the commands in flight drained; what
		 * is left failed or timed out, and the device may still run
		 * it: held until a controller reset ends that (which is
		 * asked for), not handed back now. */
		(void)ahci_port_stop_engine_abort(port);
		ahci_port_fail_all_for_reset(port, -ENODEV);
		ahci_eh_for_each_dev(port, ahci_eh_detach_one, NULL);
		c->link_gone = 1;
		ahci_eh_finish_srst_requests(port, c, L);
		return;
	}

	/* Commands an error interrupt aborted (AHCI_CMD_EH) are collected,
	 * classified and retried or failed by the error round, whatever
	 * event aborted them (a hot-plug event or an error that named no
	 * specific recovery too). */
	if ((a & heavy) || ahci_eh_has_aborted(port)) {
		ahci_eh_handle_errors(port, c, L);
		if (c->need_hba_reset)
			return;
		a = c->actions; /* handle_errors may add HOTPLUG */
	} else if (a & AHCI_EH_IF_NONFATAL) {
		ahci_eh_handle_nonfatal(port, c);
		ahci_eh_speed_down(port, c);
	}

	if (a & AHCI_EH_HOTPLUG) {
		int r;

		if (!port->pmp_attached && !ahci_eh_host_link_online(port) &&
		    port->active) {
			(void)ahci_port_stop_engine_abort(port);
			ahci_port_fail_all(port, -ENODEV);
		}
		ahci_eh_callee_begin(port);
		r = ahci_port_hotplug(port, c->did_reset);
		ahci_eh_callee_end(port);
		if (r && r != -ENODEV)
			kprintf(AHCI_PFX "%s: hot-plug handling failed (%d)\n",
				port->name, r);
		if (c->link_gone)
			ahci_eh_finish_srst_requests(port, c, L);
	}

	/* A port multiplier reported PHY changes of its links (or notified
	 * through an HBA without PxSNTF).  The interrupt handler consumed
	 * PxSNTF and scheduled the media check of notifying ATAPI drives
	 * itself. */
	if ((a & AHCI_EH_SNTF) && port->pmp_attached) {
		ahci_eh_callee_begin(port);
		(void)ahci_pmp_handle_notify(port);
		ahci_eh_callee_end(port);
	}
	if (a & AHCI_EH_LPM)
		ahci_eh_lpm(port);
	if ((a & AHCI_EH_REVALIDATE) && !c->did_reset)
		ahci_eh_revalidate_all(port, c);
}

/* ==========================================================================
 * Running EH
 * ========================================================================== */

/* The retry list after the thaw.  A command carrying a block request is
 * completed for a retry (ATA_QCFLAG_EH_RETRY): its request goes back to
 * its queue -- from this run's done queue, after the eh_mutex is released
 * -- and is dispatched again like any other.  A command with a waiter of
 * its own (no block request) is re-issued here; what cannot be issued
 * waits for the next run (EH pending again) or fails. */
static void ahci_eh_reissue(struct ahci_port *port, struct ahci_eh_lists *L)
{
	struct ata_queued_cmd *qc = L->retry_head;
	uint64_t fl;

	L->retry_head = L->retry_tail = NULL;
	while (qc) {
		struct ata_queued_cmd *next = qc->xport_next;
		int r;

		qc->xport_next = NULL;
		if (qc->end_io && qc->req) {
			qc->result = -EAGAIN;
			qc->flags |= ATA_QCFLAG_EH_RETRY;
			ahci_eh_chain(&L->done_head, &L->done_tail, qc);
			qc = next;
			continue;
		}
		r = ahci_qc_reissue(qc);
		if (r == -EAGAIN || r == -EBUSY) {
			spin_lock_irqsave(&port->lock, &fl);
			if (port->eh_hold_tail)
				port->eh_hold_tail->xport_next = qc;
			else
				port->eh_hold_head = qc;
			port->eh_hold_tail = qc;
			/* a later EH run re-issues it */
			port->eh_pending |= AHCI_EH_REISSUE;
			spin_unlock_irqrestore(&port->lock, fl);
			ahci_kahci_kick(port->hba, AHCI_WORK_EH);
		} else if (r) {
			qc->result = r;
			qc->err_mask |= r == -ENODEV ? AC_ERR_NODEV_HINT
						     : AC_ERR_OTHER;
			ahci_eh_chain(&L->done_head, &L->done_tail, qc);
		}
		qc = next;
	}
}

static void ahci_eh_complete_done(struct ahci_eh_lists *L)
{
	struct ata_queued_cmd *qc = L->done_head;
	struct blk_cpl_ctx cpl;

	L->done_head = L->done_tail = NULL;
	/* One completion section around the whole list, as for any other
	 * chain: releases that a completion parks run after the last
	 * command of the list, not while EH still holds the later ones. */
	blk_cpl_begin(&cpl);
	while (qc) {
		struct ata_queued_cmd *next = qc->xport_next;

		qc->xport_next = NULL;
		ata_qc_complete(qc);
		qc = next;
	}
	blk_cpl_end(&cpl);
}

/* The EH run (eh_mutex held, or the boot context).  Returns the actions
 * that need the mutex dropped first (controller reset, suspend/resume). */
static uint32_t ahci_eh_run_locked(struct ahci_port *port, int inline_ctx)
{
	struct ahci_eh_lists L = { 0 };
	struct ahci_eh_ctx *c;
	uint32_t post = 0;
	uint64_t fl;
	int rounds;

	BUG_ON(!port);
	wq_head_init_once(&port->eh_wait, "ahci_eh_wait");

	spin_lock_irqsave(&port->lock, &fl);
	if (port->eh_running || port->state == AHCI_PORT_UNUSED) {
		spin_unlock_irqrestore(&port->lock, fl);
		return 0;
	}
	/* Error handling starts once every command in flight has failed
	 * (aborted by an error, timed out) or finished: the others are let
	 * finish first, nothing new being admitted meanwhile.  The last of
	 * them to finish wakes the kahci thread again (or a later timeout
	 * makes it fail). */
	if (!ahci_eh_drained_locked(port)) {
		spin_unlock_irqrestore(&port->lock, fl);
		return 0;
	}
	port->eh_running = 1;
	port->eh_run_did_reset = 0;
	/* Every fan-out link gets its recovery tries for this run, every
	 * device its revalidation tries. */
	ahci_pmp_eh_begin(port);
	port->host_link.eh_dev_tries = AHCI_EH_DEV_TRIES;
	for (uint32_t l = 0; l < AHCI_PMP_MAX_LINKS; l++)
		port->pmp_links[l].eh_dev_tries = AHCI_EH_DEV_TRIES;
	if (port->state == AHCI_PORT_RUNNING ||
	    port->state == AHCI_PORT_FROZEN || port->state == AHCI_PORT_IDLE)
		port->state = AHCI_PORT_EH;
	/* Timed-out commands are error handling's from here on: the port is
	 * frozen so that nothing else touches them. */
	{
		uint32_t act = port->active;

		while (act) {
			uint32_t s = (uint32_t)__builtin_ctz(act);

			act &= ~ahci_eh_bit(s);
			if (port->cmds[s].qc &&
			    port->cmds[s].state == AHCI_CMD_TIMEDOUT) {
				ahci_eh_freeze_locked(port);
				break;
			}
		}
	}
	/* commands held over a controller reset come back now */
	L.retry_head = port->eh_hold_head;
	L.retry_tail = port->eh_hold_tail;
	port->eh_hold_head = port->eh_hold_tail = NULL;
	spin_unlock_irqrestore(&port->lock, fl);

	ahci_eh_stop_queues(port);

	c = (struct ahci_eh_ctx *)kcalloc(1, sizeof(*c));
	if (!c) {
		kprintf(AHCI_PFX "%s: EH: no memory for the error context\n",
			port->name);
	}

	for (rounds = 0; c && rounds < AHCI_EH_MAX_ROUNDS; rounds++) {
		struct ahci_link *links[AHCI_PMP_MAX_LINKS + 1];
		uint32_t actions;
		int n;

		kmemset(c, 0, sizeof(*c));
		spin_lock_irqsave(&port->lock, &fl);
		actions = port->eh_pending;
		port->eh_pending = 0;
		n = ahci_eh_links(port, links, AHCI_PMP_MAX_LINKS + 1);
		for (int i = 0; i < n; i++) {
			actions |= links[i]->eh_pending;
			links[i]->eh_pending = 0;
		}
		actions |= port->host_link.eh_pending;
		port->host_link.eh_pending = 0;
		if (port->srst_head)
			actions |= AHCI_EH_RESET_SOFT;
		c->irq_stat = port->eh_irq_stat;
		c->serror = port->eh_serror;
		c->tfd = port->eh_tfd;
		c->fbs_dev = (int)port->eh_fbs_link - 1;
		c->frozen = port->eh_frozen;
		port->eh_fbs_link = 0;
		/* The error interrupt cleared SError and left its analysis to
		 * this round.  The device status of a task file error is read
		 * here too: the HBA halted on it and PxTFD still shows it. */
		if (c->serror)
			actions |= ahci_serror_classify(port, c->serror);
		if ((c->irq_stat & AHCI_PxIS_TFES) && !c->tfd)
			c->tfd = ahci_port_readl(port, AHCI_PxTFD);
		c->failed_hint = port->eh_failed_slots;
		c->quiet = !port->eh_noisy; /* refined in report_exception */
		port->eh_noisy = 0;
		port->eh_irq_stat = 0;
		port->eh_serror = 0;
		port->eh_tfd = 0;
		port->eh_failed_slots = 0;
		if (port->state == AHCI_PORT_RUNNING ||
		    port->state == AHCI_PORT_FROZEN ||
		    port->state == AHCI_PORT_IDLE)
			port->state = AHCI_PORT_EH;
		spin_unlock_irqrestore(&port->lock, fl);

		post |= actions & (AHCI_EH_SUSPEND | AHCI_EH_RESUME);
		actions &= ~(AHCI_EH_SUSPEND | AHCI_EH_RESUME);
		if (!actions)
			break;
		if (port->state == AHCI_PORT_DISABLED &&
		    !(actions & (AHCI_EH_HOTPLUG | AHCI_EH_DETACH))) {
			/* nothing to recover any more */
			ahci_port_fail_all(port, -EIO);
			continue;
		}

		c->actions = actions;
		c->inline_ctx = inline_ctx;
		ahci_trace(port, AHCI_TRACE_EH, NULL, 0, actions);
		ahci_eh_round(port, c, &L);
		if (c->need_hba_reset) {
			post |= AHCI_EH_HBA_RESET;
			break;
		}
	}
	if (rounds == AHCI_EH_MAX_ROUNDS && port->eh_pending) {
		/* Exceptions kept coming in every round: give up on them
		 * (a controller reset asked for stays; kahci runs it), so
		 * that kahci, which sleeps until woken, does not spin. */
		kprintf(AHCI_PFX "%s: EH: actions 0x%x still pending after %d rounds, giving up\n",
			port->name, port->eh_pending, rounds);
		spin_lock_irqsave(&port->lock, &fl);
		port->eh_pending &= AHCI_EH_HBA_RESET;
		spin_unlock_irqrestore(&port->lock, fl);
	}

	/* finish */
	if (port->zombie_slots && port->state != AHCI_PORT_DISABLED)
		(void)ahci_cmd_kick_engine(port);

	if (post & AHCI_EH_HBA_RESET) {
		/* the commands wait for the controller to come back */
		spin_lock_irqsave(&port->lock, &fl);
		if (L.retry_head) {
			if (port->eh_hold_tail)
				port->eh_hold_tail->xport_next = L.retry_head;
			else
				port->eh_hold_head = L.retry_head;
			port->eh_hold_tail = L.retry_tail;
		}
		L.retry_head = L.retry_tail = NULL;
		/* kahci sees the bit and runs the controller recovery,
		 * which fails the held commands with -ECANCELED (requeue)
		 * and schedules a hard reset + revalidation per port */
		port->eh_pending |= AHCI_EH_HBA_RESET;
		spin_unlock_irqrestore(&port->lock, fl);
		ahci_kahci_kick(port->hba, AHCI_WORK_HBA_RESET);
	} else {
		int stuck = 0;

		spin_lock_irqsave(&port->lock, &fl);
		if (port->state == AHCI_PORT_EH ||
		    port->state == AHCI_PORT_FROZEN) {
			/* Thawing hands the port to the issue path, which
			 * never looks at PxCMD.ST: a port with devices whose
			 * engine did not come back (a failed restart) would
			 * take commands that never run and read their clear
			 * PxCI bits as finished.  It stays in error handling
			 * with a reset requested; the streak count makes the
			 * next runs escalate rather than loop. */
			if (ahci_eh_port_has_device(port) &&
			    !(ahci_port_readl(port, AHCI_PxCMD) &
			      AHCI_PxCMD_ST)) {
				stuck = 1;
				port->eh_consecutive++;
				port->eh_pending |= AHCI_EH_RESET_HARD;
				/* the commands to re-issue wait with it */
				if (L.retry_head) {
					if (port->eh_hold_tail)
						port->eh_hold_tail->xport_next =
							L.retry_head;
					else
						port->eh_hold_head =
							L.retry_head;
					port->eh_hold_tail = L.retry_tail;
				}
				L.retry_head = L.retry_tail = NULL;
			} else {
				ahci_eh_thaw_locked(port);
			}
		}
		spin_unlock_irqrestore(&port->lock, fl);
		if (stuck) {
			kprintf(AHCI_PFX "%s: EH: command engine not running at the end of recovery, resetting again\n",
				port->name);
			ahci_kahci_kick(port->hba, AHCI_WORK_EH);
		} else {
			ahci_eh_reissue(port, &L);
		}
	}
	ahci_eh_complete_done(&L);
	if (c)
		kfree(c);

	spin_lock_irqsave(&port->lock, &fl);
	port->eh_running = 0;
	port->eh_seq++;
	spin_unlock_irqrestore(&port->lock, fl);

	if (!(post & AHCI_EH_HBA_RESET))
		ahci_eh_start_queues(port);
	wq_wake_all(&port->eh_wait);
	ahci_cmd_run_deferred_completions(port);
	return post;
}

void ahci_eh_run(struct ahci_port *port)
{
	struct blk_cpl_ctx doneq;
	uint32_t post;

	if (!port)
		return;
	might_sleep();

	if (!port->eh_pending && !port->host_link.eh_pending &&
	    !port->srst_head && !port->eh_hold_head) {
		wq_wake_all(&port->eh_wait);
		return;
	}
	/* The block requests whose commands this run completes wait on its
	 * done queue and are finished here, in this thread, once the
	 * eh_mutex is released: ended, or given back to their queue for a
	 * retry. */
	blk_cpl_begin(&doneq);
	blk_cpl_collect_done(&doneq);
	ata_mutex_lock(&port->eh_mutex);
	post = ahci_eh_run_locked(port, 0);
	ata_mutex_unlock(&port->eh_mutex);
	blk_cpl_end(&doneq);

	/* Port suspend/resume take the eh_mutex themselves. */
	if (post & AHCI_EH_SUSPEND)
		(void)ahci_port_suspend(port);
	if (post & AHCI_EH_RESUME)
		(void)ahci_port_resume(port);
	/* AHCI_EH_HBA_RESET is left in eh_pending: the kahci loop runs the
	 * controller recovery next (never under an eh_mutex). */
}

int ahci_eh_poll_run(struct ahci_port *port)
{
	uint32_t post;
	int took;

	if (!port || port->eh_running)
		return 0;
	if (!port->eh_pending && !port->srst_head && !port->eh_hold_head)
		return 0;
	took = ata_mutex_trylock(&port->eh_mutex);
	if (!took && ata_can_sleep() && !ata_mutex_held(&port->eh_mutex))
		return 0; /* another task holds the port: it runs EH */
	post = ahci_eh_run_locked(port, 1);
	if (took)
		ata_mutex_unlock(&port->eh_mutex);
	if (post & (AHCI_EH_SUSPEND | AHCI_EH_RESUME)) {
		uint64_t fl;

		/* they take the mutex themselves: leave them to kahci */
		spin_lock_irqsave(&port->lock, &fl);
		port->eh_pending |= post & (AHCI_EH_SUSPEND | AHCI_EH_RESUME);
		spin_unlock_irqrestore(&port->lock, fl);
		ahci_kahci_kick(port->hba, AHCI_WORK_EH);
	}
	return 1;
}

int ahci_eh_wait(struct ahci_port *port, uint32_t timeout_ms)
{
	uint64_t deadline_ns = 0;
	task_t *cur;

	if (!port)
		return -ENODEV;
	wq_head_init_once(&port->eh_wait, "ahci_eh_wait");

	if (!ata_can_sleep()) {
		uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;

		for (;;) {
			if (!port->eh_pending && !port->eh_running)
				return 0;
			if (timeout_ms != ATA_WAIT_FOREVER &&
			    waited_us >= limit_us)
				return -ETIMEDOUT;
			(void)ahci_cmd_poll_port(port);
			ahci_udelay(100);
			waited_us += 100;
		}
	}

	might_sleep();
	cur = sched_current();
	if (timeout_ms != ATA_WAIT_FOREVER)
		deadline_ns = hrtimer_now_ns() +
			      (uint64_t)timeout_ms * 1000000ULL;
	for (;;) {
		struct wait_queue_entry we;
		uint64_t fl, now_ns;

		spin_lock_irqsave(&port->lock, &fl);
		if (!port->eh_pending && !port->eh_running) {
			spin_unlock_irqrestore(&port->lock, fl);
			return 0;
		}
		now_ns = hrtimer_now_ns();
		if (deadline_ns && now_ns >= deadline_ns) {
			spin_unlock_irqrestore(&port->lock, fl);
			return -ETIMEDOUT;
		}
		wq_entry_init(&we, cur);
		wq_add(&port->eh_wait, &we);
		cur->wait_channel = &port->eh_wait;
		cur->wakeup_tick =
			deadline_ns ? timer_ticks() +
					      timer_ns_to_ticks(deadline_ns -
								now_ns) +
					      1
				    : timer_ticks() + timer_ns_to_ticks(
							      100000000ULL) +
					      1; /* 100 ms backstop re-check */
		task_set_blocked(cur);
		spin_unlock_irqrestore(&port->lock, fl);
		if (deadline_ns)
			hrtimer_wait_arm(cur, deadline_ns);
		sched_schedule();
		if (deadline_ns)
			hrtimer_wait_disarm(cur);
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		wq_remove(&port->eh_wait, &we);
	}
}

int ahci_eh_reset_dev(struct ata_device *dev, int kind)
{
	struct ahci_link *link;
	struct ahci_port *port;
	uint32_t actions;
	uint64_t fl;
	int r = 0;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -ENODEV;
	link = ahci_dev_link(dev);
	if (!link || !link->port)
		return -ENODEV;
	port = link->port;

	switch (kind) {
	case ATA_RESET_SOFT:
		actions = AHCI_EH_RESET_SOFT;
		break;
	case ATA_RESET_HARD:
	case ATA_RESET_PORT:
		actions = AHCI_EH_RESET_HARD;
		break;
	default:
		return -EINVAL;
	}
	if (ata_can_sleep())
		might_sleep();

	spin_lock_irqsave(&port->lock, &fl);
	link->eh_pending |= actions;
	ahci_eh_schedule_locked(port, actions);
	spin_unlock_irqrestore(&port->lock, fl);

	if (ata_mutex_held(&port->eh_mutex) || !port->hba->kahci_running ||
	    !ata_can_sleep()) {
		/* The caller holds the port (a revalidation waking a
		 * sleeping disk, boot probing) or there is no worker: run
		 * the reset here. */
		int took = 0;

		if (!ata_mutex_held(&port->eh_mutex))
			took = ata_mutex_trylock(&port->eh_mutex);
		if (took || ata_mutex_held(&port->eh_mutex) ||
		    !ata_can_sleep()) {
			uint32_t post = ahci_eh_run_locked(port, 1);

			if (took)
				ata_mutex_unlock(&port->eh_mutex);
			if (post & (AHCI_EH_SUSPEND | AHCI_EH_RESUME)) {
				spin_lock_irqsave(&port->lock, &fl);
				port->eh_pending |= post & (AHCI_EH_SUSPEND |
							    AHCI_EH_RESUME);
				spin_unlock_irqrestore(&port->lock, fl);
				ahci_kahci_kick(port->hba, AHCI_WORK_EH);
			}
		} else {
			r = ahci_eh_wait(port, AHCI_EH_RESET_WAIT_MS);
		}
	} else {
		r = ahci_eh_wait(port, AHCI_EH_RESET_WAIT_MS);
	}
	if (r)
		return r;
	if (!ahci_eh_dev_usable(dev))
		return -ENODEV;
	if (port->state == AHCI_PORT_DISABLED)
		return -EIO;
	return port->eh_last_result;
}
