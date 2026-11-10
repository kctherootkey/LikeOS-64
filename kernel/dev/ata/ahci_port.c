// LikeOS AHCI driver - port engines, link reset, hot-plug, link power management
//
// Everything that happens to ONE SATA port of an AHCI host controller
// below the level of individual commands:
//
//   * the port object itself (allocation, the per-port locks, DMA areas,
//     links, the timeout timer) and its teardown;
//   * the two DMA engines of a port -- the command list engine (PxCMD.ST,
//     reported by PxCMD.CR) and the FIS receive engine (PxCMD.FRE, reported
//     by PxCMD.FR) -- started and stopped in the order the AHCI 1.3.1
//     specification (section 10.1.2 and 10.3) requires, with its 500 ms
//     limits, plus the command list override (PxCMD.CLO);
//   * port initialisation: programming PxCLB/PxFB with the command list and
//     received-FIS area, clearing PxSERR/PxIS, powering the device on
//     (PxCMD.POD on cold-presence ports), spinning it up (PxCMD.SUD on
//     staggered spin-up controllers), the PxIE mask;
//   * the SATA PHY: SControl (DET/SPD/IPM), SStatus decode (DET/SPD/IPM),
//     COMRESET (DET=1 for at least 1 ms, then DET=0 and wait for DET=3),
//     link debouncing, speed limits and the speed step-down after
//     repeated link errors, taking the PHY offline;
//   * device readiness and classification: BSY/DRQ in PxTFD with the
//     ATA timeouts (31 s for a spinning drive), the signature in PxSIG or
//     in the D2H FIS of a software reset, the follow-up software reset to
//     port multiplier port 15 that tells a port multiplier from a disk;
//   * software reset (two H2D FISes with SRST set and cleared), hard reset
//     (COMRESET + ready + classify) and the ABORT rung of error handling;
//   * probing what is attached and handing it to the ATA layer, hot-plug
//     (PxIS.PCS/PRCS/CPDS/DMPS, cold presence, mechanical switch) with
//     removal and insertion handling;
//   * link power management: HIPM (PxCMD.ALPE/ASP), the SControl IPM
//     permissions, automatic partial-to-slumber (PxCMD.APSTE), DevSleep
//     (PxDEVSLP), DIPM through SET FEATURES (SATA feature 03h);
//   * port suspend, resume and shutdown, SCR access.
//
// Port multipliers and FIS-based switching live in ahci_pmp.c, commands in
// ahci_cmd.c, the error handling ladder in ahci_eh.c.
//
// ---- Concurrency -----------------------------------------------------------------
// Every port is independent: it has its own spinlock (port->lock), its own
// sleeping mutex (port->eh_mutex) and its own state.  Nothing in this file
// takes a lock that spans ports; the only HBA-wide resource touched is the
// global IS register (one bit per port, under hba->lock, nested inside
// port->lock as the lock order allows).  Several ports therefore reset,
// probe and hot-plug in parallel when several kahci contexts or the boot
// path work on them.
//
// The functions documented "eh_mutex held / boot" are called by the kahci
// thread (error handling, hot-plug, LPM) with the port's eh_mutex held, or
// at boot by ahci_init() before the scheduler exists, interrupts off.  In
// the second case every wait busy-waits (ahci_msleep falls back to the
// calibrated delay loop) -- the same code, polled.
//
// Register writes that change a port's state (PxCMD, PxSCTL, PxIE, PxFBS,
// PxDEVSLP) happen under port->lock so that they never interleave with
// the issue path's PxCI/PxSACT writes or the interrupt handler's PxIS
// acknowledgement.  Waiting for a register never happens under a spinlock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */

/* Set to 1 for a line per register step (engine start/stop, every SControl
 * write, every debounce sample).  The normal log only has what a user
 * wants to see: link up/down, speed, resets, hot-plug, errors. */
#define AHCI_PORT_DEBUG 0

#define port_info(port, fmt, ...) \
	kprintf("[ahci] %s: " fmt "\n", (port)->name, ##__VA_ARGS__)
#define port_warn(port, fmt, ...) \
	kprintf("[ahci] %s: WARNING: " fmt "\n", (port)->name, ##__VA_ARGS__)
#if AHCI_PORT_DEBUG
#define port_dbg(port, fmt, ...) \
	kprintf("[ahci] %s: " fmt "\n", (port)->name, ##__VA_ARGS__)
#else
#define port_dbg(port, fmt, ...)                 \
	do {                                     \
		if (0)                           \
			kprintf(fmt, ##__VA_ARGS__); \
		(void)(port);                    \
	} while (0)
#endif

/* ==========================================================================
 * Local tuning (values the specification leaves to the host)
 * ========================================================================== */

/* A reset attempt's release of DET: checked up to this many times, each
 * after this delay. */
#define AHCI_LINK_RESUME_TRIES 5
#define AHCI_LINK_RESUME_DELAY_MS 200
/* Waiting for an ICC (interface communication control) request to be
 * taken by the HBA and for the link to reach the requested state. */
#define AHCI_ICC_TIMEOUT_MS AHCI_LPM_SETTLE_MS
/* Busy-wait granularity of the first millisecond of every register wait;
 * afterwards the waits step in milliseconds (sleeping when allowed). */
#define AHCI_FAST_POLL_US 10
/* On a host that supports port multipliers: how long hard reset waits for
 * the device to drop BSY before it sends the follow-up SRST to port 15
 * (a multiplier may hold BSY until it gets one).  Devices that are not
 * multipliers report their signature within milliseconds of the link
 * coming up unless they spin up, which the long wait afterwards covers. */
#define AHCI_PMP_READY_PEEK_MS 1000
/* Drain timeout when the port has to be quiesced (LPM, DevSleep, FBS). */
#define AHCI_QUIESCE_TIMEOUT_MS 30000
/* Shutdown: how long ahci_port_shutdown() tries to get the eh_mutex
 * before it proceeds without it (a wedged error handler must not stop a
 * power-off). */
#define AHCI_SHUTDOWN_LOCK_MS 5000
/* The default link power management policy of a new port.  Link power
 * management saves little on a desktop and has a long history of broken
 * device/host combinations, so a port starts with every interface power
 * state transition disabled; the policy can be changed per port through
 * sysfs (link_power_management_policy) or the transport's set_lpm op. */
#define AHCI_LPM_DEFAULT_POLICY AHCI_LPM_MAX_PERFORMANCE

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

/* "may sleep" paths start with this: before the scheduler exists (the boot
 * probe) every wait busy-waits and nothing sleeps, so the check applies
 * only once there is a task -- then interrupts must be on. */
#define ahci_might_sleep()                      \
	do {                                    \
		if (sched_current() != NULL)    \
			might_sleep();          \
	} while (0)

/* The functions documented "eh_mutex held" check it when a task runs them
 * (at boot there is no owner to compare with). */
static inline void ahci_assert_eh_held(const struct ahci_port *port)
{
	if (ata_can_sleep())
		WARN_ON_ONCE(!ata_mutex_held(&port->eh_mutex));
}

static inline uint64_t ahci_now_ns(void)
{
	return hrtimer_now_ns();
}

/* Read-modify-write of PxCMD under port->lock.  Two fields need care when
 * a value read back is written again: ICC (a non-zero value is a REQUEST
 * the HBA has not taken yet -- writing it back would repeat it) and CLO
 * (write-one-to-set, self-clearing).  Both are always written as zero
 * unless the caller sets them explicitly. */
static uint32_t ahci_port_cmd_rmw_locked(struct ahci_port *port,
					 uint32_t clear, uint32_t set)
{
	uint32_t v;

	lockdep_assert_held(&port->lock);
	v = ahci_port_readl(port, AHCI_PxCMD);
	v &= ~(clear | AHCI_PxCMD_ICC_MASK | AHCI_PxCMD_CLO);
	v |= set;
	ahci_port_writel(port, AHCI_PxCMD, v);
	ahci_port_flush(port);
	return v;
}

static uint32_t ahci_port_cmd_rmw(struct ahci_port *port, uint32_t clear,
				  uint32_t set)
{
	uint64_t fl;
	uint32_t v;

	spin_lock_irqsave(&port->lock, &fl);
	v = ahci_port_cmd_rmw_locked(port, clear, set);
	spin_unlock_irqrestore(&port->lock, fl);
	return v;
}

/* Wait for (port register & mask) == val.  The first millisecond is polled
 * every 10 us -- most transitions (CR, FR, CLO) finish within
 * microseconds -- and the rest in 1 ms steps, which sleep when the caller
 * may sleep and busy-wait otherwise.  0 or -ETIMEDOUT, last value in
 * *last. */
static int ahci_port_wait_bits(struct ahci_port *port, uint32_t off,
			       uint32_t mask, uint32_t val, uint32_t timeout_ms,
			       uint32_t *last)
{
	int rc;

	rc = ahci_wait_reg(port->mmio, off, mask, val, 1, AHCI_FAST_POLL_US,
			   last);
	if (rc == 0 || timeout_ms <= 1)
		return rc;
	return ahci_wait_reg(port->mmio, off, mask, val, timeout_ms - 1, 1000,
			     last);
}

static const char *ahci_det_name(uint32_t det)
{
	switch (det) {
	case AHCI_SSTS_DET_NONE:
		return "no device";
	case AHCI_SSTS_DET_PRESENT:
		return "device present, no PHY communication";
	case AHCI_SSTS_DET_PHY:
		return "device present, PHY established";
	case AHCI_SSTS_DET_OFFLINE:
		return "PHY offline";
	default:
		return "reserved";
	}
}

static const char *ahci_ipm_name(uint32_t ipm)
{
	switch (ipm) {
	case AHCI_SSTS_IPM_NONE:
		return "none";
	case AHCI_SSTS_IPM_ACTIVE:
		return "active";
	case AHCI_SSTS_IPM_PARTIAL:
		return "partial";
	case AHCI_SSTS_IPM_SLUMBER:
		return "slumber";
	case AHCI_SSTS_IPM_DEVSLEEP:
		return "devsleep";
	default:
		return "reserved";
	}
}

/* The fastest generation the HBA supports (CAP.ISS), as an SStatus.SPD /
 * SControl.SPD value.  An HBA that reports a reserved value is treated as
 * Gen3 capable: the PHY negotiates down by itself. */
static uint8_t ahci_port_hw_speed_max(const struct ahci_port *port)
{
	uint32_t iss = port->hba->caps.iss;

	if (iss == 0)
		iss = (port->hba->cap & AHCI_CAP_ISS_MASK) >> AHCI_CAP_ISS_SHIFT;
	if (iss >= AHCI_ISS_GEN1 && iss <= AHCI_ISS_GEN3)
		return (uint8_t)iss;
	return AHCI_SSTS_SPD_GEN3;
}

/* Port can carry a port multiplier: CAP.SPM and no quirk against it. */
static int ahci_port_pmp_capable(const struct ahci_port *port)
{
	return port->hba->caps.spm && !(port->hba->quirks & AHCI_Q_NO_PMP);
}

/* Clear the D2H register FIS area(s) before a reset, so that a signature
 * found afterwards is known to be new: the FIS type byte is zeroed and the
 * status reads BSY until the device writes its FIS.  `pmp' < 0 clears
 * every area (all 16 with FBS).  The HBA owns the area while FRE is set,
 * but it writes it only when a FIS arrives, which a stopped engine with
 * the device being reset does not produce before we issue the reset. */
static void ahci_port_clear_rx_d2h(struct ahci_port *port, int pmp)
{
	int first = 0, last = 0;

	if (!port->rx_fis)
		return;
	if (port->fbs_enabled) {
		first = pmp < 0 ? 0 : pmp;
		last = pmp < 0 ? 15 : pmp;
	}
	for (int i = first; i <= last; i++) {
		volatile struct fis_reg_d2h *d2h = ahci_port_rx_d2h(port, i);

		d2h->fis_type = 0;
		d2h->status = ATA_BUSY;
		d2h->error = 0;
		d2h->count_lo = 0xFF;
		d2h->lba0 = 0xFF;
		d2h->lba1 = 0xFF;
		d2h->lba2 = 0xFF;
	}
	ahci_mb();
}

/* The signature (count | lbal << 8 | lbam << 16 | lbah << 24) of the D2H
 * FIS in the area of `pmp', or AHCI_PxSIG_INVALID when no D2H register FIS
 * arrived there since ahci_port_clear_rx_d2h(). */
static uint32_t ahci_port_rx_sig(struct ahci_port *port, int pmp,
				 uint8_t *status)
{
	volatile struct fis_reg_d2h *d2h;
	uint32_t sig;

	ahci_rmb();
	d2h = ahci_port_rx_d2h(port, pmp);
	if (status)
		*status = d2h->status;
	if (d2h->fis_type != FIS_TYPE_REG_D2H)
		return AHCI_PxSIG_INVALID;
	sig = (uint32_t)d2h->count_lo | ((uint32_t)d2h->lba0 << 8) |
	      ((uint32_t)d2h->lba1 << 16) | ((uint32_t)d2h->lba2 << 24);
	return sig;
}

/* Acknowledge everything latched in PxSERR and PxIS and the port's bit in
 * the global IS.  PxIS.PCS and PxIS.PRCS are read-only reflections of
 * PxSERR.DIAG.X / .N and go away with them. */
static void ahci_port_clear_status_locked(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	uint64_t hfl;
	uint32_t serr, is;

	lockdep_assert_held(&port->lock);
	serr = ahci_port_readl(port, AHCI_PxSERR);
	if (serr)
		ahci_port_writel(port, AHCI_PxSERR, serr);
	is = ahci_port_readl(port, AHCI_PxIS);
	if (is)
		ahci_port_writel(port, AHCI_PxIS, is);
	spin_lock_irqsave(&hba->lock, &hfl);
	ahci_hba_writel(hba, AHCI_IS, 1u << port->index);
	spin_unlock_irqrestore(&hba->lock, hfl);
	ahci_port_flush(port);
}

static void ahci_port_clear_status(struct ahci_port *port)
{
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_clear_status_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
}

/* Decode a PxCMD / PxSERR value for the log through the debug helpers. */
static const char *ahci_port_cmd_text(uint32_t cmd, char *buf, int cap)
{
	buf[0] = '\0';
	ahci_pxcmd_str(cmd, buf, cap);
	return buf;
}

static const char *ahci_port_serr_text(uint32_t serr, char *buf, int cap)
{
	buf[0] = '\0';
	ahci_serror_str(serr, buf, cap);
	return buf;
}

static void ahci_port_trace_reset(struct ahci_port *port, int rc,
				  uint32_t level)
{
	if (port->trace)
		ahci_trace(port, AHCI_TRACE_RESET, NULL, rc, level);
}

/* ==========================================================================
 * LPM policy names
 * ========================================================================== */

const char *ahci_lpm_policy_name(enum ahci_lpm_policy p)
{
	switch (p) {
	case AHCI_LPM_KEEP_FIRMWARE:
		return "keep_firmware_settings";
	case AHCI_LPM_MAX_PERFORMANCE:
		return "max_performance";
	case AHCI_LPM_MEDIUM_POWER:
		return "medium_power";
	case AHCI_LPM_MED_POWER_WITH_DIPM:
		return "med_power_with_dipm";
	case AHCI_LPM_MIN_POWER:
		return "min_power";
	default:
		return "unknown";
	}
}

int ahci_lpm_policy_parse(const char *s)
{
	static const enum ahci_lpm_policy all[] = {
		AHCI_LPM_KEEP_FIRMWARE,	      AHCI_LPM_MAX_PERFORMANCE,
		AHCI_LPM_MEDIUM_POWER,	      AHCI_LPM_MED_POWER_WITH_DIPM,
		AHCI_LPM_MIN_POWER,
	};
	size_t len;

	if (!s)
		return -EINVAL;
	/* sysfs writes usually end in a newline */
	len = kstrlen(s);
	while (len && (s[len - 1] == '\n' || s[len - 1] == ' ' ||
		       s[len - 1] == '\t'))
		len--;
	if (!len)
		return -EINVAL;
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		const char *n = ahci_lpm_policy_name(all[i]);

		if (kstrlen(n) == len && kstrncmp(n, s, len) == 0)
			return (int)all[i];
	}
	return -EINVAL;
}

/* ==========================================================================
 * Port object
 * ========================================================================== */

static void ahci_link_init(struct ahci_port *port, struct ahci_link *link,
			   int is_host, int pmp)
{
	kmemset(link, 0, sizeof(*link));
	link->port = port;
	link->is_host = is_host;
	link->pmp = is_host ? 0 : pmp;
	link->state = AHCI_LINK_OFFLINE;
	link->class = ATA_DEV_UNKNOWN;
	link->sig = AHCI_PxSIG_INVALID;
	link->lpm_policy = AHCI_LPM_DEFAULT_POLICY;
	if (is_host)
		ksnprintf(link->name, sizeof(link->name), "link%d",
			  port->port_no);
	else
		ksnprintf(link->name, sizeof(link->name), "link%d.%d",
			  port->port_no, pmp);
}

/* The system-wide ATA port number of a new port.  When the HBA reserved a
 * range for its CAP.NP ports (hba->first_port_no), the port gets its
 * index in that range, so the numbers follow the hardware layout;
 * otherwise the next free number is handed out. */
static int ahci_port_number(struct ahci_hba *hba, int index)
{
	uint64_t fl;
	int no;

	if (hba->first_port_no > 0)
		return hba->first_port_no + index;
	spin_lock_irqsave(&g_ahci_list_lock, &fl);
	if (g_ahci_next_port_no < 1)
		g_ahci_next_port_no = 1;
	no = g_ahci_next_port_no++;
	spin_unlock_irqrestore(&g_ahci_list_lock, fl);
	return no;
}

int ahci_port_alloc(struct ahci_hba *hba, int index)
{
	struct ahci_port *port;
	uint32_t cmd, sctl;
	uint64_t fl;
	int rc;

	BUG_ON(!hba);
	if (index < 0 || index >= AHCI_MAX_PORTS) {
		kprintf("[ahci] %s: port index %d out of range\n", hba->name,
			index);
		return -EINVAL;
	}
	if (WARN_ON(hba->ports[index] != NULL))
		return -EEXIST;
	if (hba->mmio_size &&
	    AHCI_PORT_BASE + (uint64_t)(index + 1) * AHCI_PORT_SIZE >
		    hba->mmio_size) {
		kprintf("[ahci] %s: port %d lies outside ABAR (%llu bytes)\n",
			hba->name, index, (unsigned long long)hba->mmio_size);
		return -EINVAL;
	}

	port = kcalloc(1, sizeof(*port));
	if (!port)
		return -ENOMEM;

	port->hba = hba;
	port->index = index;
	port->port_no = ahci_port_number(hba, index);
	ksnprintf(port->name, sizeof(port->name), "ata%d", port->port_no);
	port->mmio = hba->mmio + AHCI_PORT_BASE + index * AHCI_PORT_SIZE;
	port->state = AHCI_PORT_UNUSED;
	port->flags = AHCI_PORT_F_IMPLEMENTED;

	spinlock_init(&port->lock, port->name);
	ata_mutex_init(&port->eh_mutex, port->name);
	wq_head_init(&port->eh_wait, port->name);
	blk_work_init(&port->deferred_work, ahci_cmd_deferred_work_fn, port);

	/* Capabilities of this port (PxCMD read-only bits). */
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if ((cmd & AHCI_PxCMD_ESP) && !(hba->quirks & AHCI_Q_NO_SXS))
		port->flags |= AHCI_PORT_F_EXTERNAL;
	if (cmd & AHCI_PxCMD_HPCP)
		port->flags |= AHCI_PORT_F_HOTPLUG;
	if (cmd & AHCI_PxCMD_CPD)
		port->flags |= AHCI_PORT_F_CPD;
	if ((cmd & AHCI_PxCMD_MPSP) && hba->caps.smps)
		port->flags |= AHCI_PORT_F_MPS;
	if (cmd & AHCI_PxCMD_ATAPI)
		port->flags |= AHCI_PORT_F_ATAPI;
	port->hotplug_capable =
		!(hba->quirks & AHCI_Q_NO_HOTPLUG) &&
		(port->flags & (AHCI_PORT_F_HOTPLUG | AHCI_PORT_F_EXTERNAL |
				AHCI_PORT_F_CPD | AHCI_PORT_F_MPS)) != 0;

	/* FIS-based switching needs the HBA capability, the port capability
	 * and port multiplier support; a quirk can force it either way.  The
	 * received-FIS area is sized from this (4 KiB instead of 256 B). */
	port->fbs_capable =
		ahci_port_pmp_capable(port) &&
		!(hba->quirks & AHCI_Q_NO_FBS) &&
		(((hba->caps.fbss) && (cmd & AHCI_PxCMD_FBSCP)) ||
		 (hba->quirks & AHCI_Q_YES_FBS));
	port->fbs_enabled = 0;
	port->fbs_last_dev = 0;

	port->devslp_capable = 0;
	if (hba->caps.sds && !(hba->quirks & AHCI_Q_NO_DEVSLP)) {
		uint32_t ds = ahci_port_readl(port, AHCI_PxDEVSLP);

		port->devslp_capable = (ds & AHCI_DEVSLP_DSP) != 0;
		port->saved_devslp = ds;
	}

	/* Command slots and NCQ tags.  NCQ uses every slot the HBA has (32
	 * on most).  No slot is reserved: error handling's own commands run
	 * on slot 0 (internal_slot) while EH owns the port -- the commands
	 * in flight are aborted and collected then, and the engine stopped,
	 * so every slot is free -- and a management command outside EH
	 * waits for an idle port like any non-queued command. */
	port->nr_slots = hba->caps.n_slots;
	if (port->nr_slots == 0)
		port->nr_slots = ((hba->cap & AHCI_CAP_NCS_MASK) >>
				  AHCI_CAP_NCS_SHIFT) + 1;
	if (port->nr_slots > AHCI_MAX_SLOTS)
		port->nr_slots = AHCI_MAX_SLOTS;
	port->internal_slot = 0;
	if (hba->caps.sncq && !(hba->quirks & AHCI_Q_NO_NCQ))
		port->ncq_depth = port->nr_slots;
	else
		port->ncq_depth = 0;
	for (uint32_t i = 0; i < AHCI_MAX_SLOTS; i++) {
		port->cmds[i].slot = i;
		port->cmds[i].state = AHCI_CMD_FREE;
		port->cmds[i].qc = NULL;
	}

	/* What the firmware left in SControl: the IPM permissions are kept
	 * under the keep_firmware_settings policy. */
	sctl = ahci_port_readl(port, AHCI_PxSCTL);
	port->saved_sctl = sctl;
	port->saved_cmd = cmd;
	port->lpm_policy = AHCI_LPM_DEFAULT_POLICY;

	ahci_link_init(port, &port->host_link, 1, 0);
	port->host_link.scontrol = sctl;
	port->host_link.sstatus = ahci_port_readl(port, AHCI_PxSSTS);
	for (int i = 0; i < AHCI_PMP_MAX_LINKS; i++)
		ahci_link_init(port, &port->pmp_links[i], 0, i);
	port->pmp_attached = 0;
	port->nr_pmp_links = 0;

	port->polled = hba->irq_mode == AHCI_IRQ_POLL;
	port->irq_mask = 0;

	/* Command list, received-FIS area, command tables. */
	rc = ahci_port_dma_alloc(port);
	if (rc) {
		kprintf("[ahci] %s: port %d: DMA allocation failed (%d)\n",
			hba->name, index, rc);
		kfree(port);
		return rc;
	}
	port->trace = ahci_trace_alloc(); /* NULL is fine: no trace */

	spin_lock_irqsave(&hba->lock, &fl);
	hba->ports[index] = port;
	spin_unlock_irqrestore(&hba->lock, fl);

	{
		char cbuf[128];

		port_dbg(port,
			 "port %d at %p: CMD %08x [%s] SCTL %08x SSTS %08x "
			 "slots %u ncq %u%s%s%s",
			 index, (void *)port->mmio, cmd,
			 ahci_port_cmd_text(cmd, cbuf, sizeof(cbuf)), sctl,
			 port->host_link.sstatus, port->nr_slots,
			 port->ncq_depth, port->hotplug_capable ? " hotplug" : "",
			 port->fbs_capable ? " fbs" : "",
			 port->devslp_capable ? " devslp" : "");
		(void)cbuf;
	}
	return 0;
}

void ahci_port_free(struct ahci_port *port)
{
	struct ahci_hba *hba;
	uint64_t fl;

	if (!port)
		return;
	hba = port->hba;
	ahci_might_sleep();

	/* Devices must be gone (detached by the caller); anything left is a
	 * bug in the teardown order -- detach it rather than leak it. */
	if (WARN_ON(port->host_link.dev != NULL))
		ahci_link_detach(&port->host_link);
	for (int i = 0; i < AHCI_PMP_MAX_LINKS; i++)
		if (WARN_ON(port->pmp_links[i].dev != NULL))
			ahci_link_detach(&port->pmp_links[i]);
	BUG_ON(port->active != 0 && port->state == AHCI_PORT_RUNNING);

	/* The HBA must not touch the memory we are about to free. */
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_irq_disable_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
	(void)ahci_port_stop_engine(port);
	(void)ahci_port_stop_fis_rx(port);
	/* The deferred command's issue on kblockd must not find the port
	 * freed: take it off the list, or wait for the run under way. */
	if (blk_work_cancel_sync(&port->deferred_work, 1000)) {
		WARN(1, "[ahci] %s: deferred command work does not finish; leaking the port",
		     port->name);
		return;
	}
	/* Completions parked for a lock-free context are done before the
	 * memory goes. */
	(void)ahci_cmd_run_deferred_completions(port);

	ahci_port_dma_free(port);
	if (port->trace) {
		kfree(port->trace);
		port->trace = NULL;
	}

	if (hba) {
		spin_lock_irqsave(&hba->lock, &fl);
		if (hba->ports[port->index] == port)
			hba->ports[port->index] = NULL;
		spin_unlock_irqrestore(&hba->lock, fl);
	}
	kfree(port);
}

/* ==========================================================================
 * Command list and FIS receive engines (AHCI 1.3.1, 10.1.2 and 10.3)
 * ========================================================================== */

int ahci_port_stop_engine(struct ahci_port *port)
{
	uint32_t cmd, last = 0;
	int rc;

	BUG_ON(!port);
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	/* Already idle: neither started nor running. */
	if (!(cmd & (AHCI_PxCMD_ST | AHCI_PxCMD_CR)))
		return 0;

	/* Clearing ST makes the HBA finish the FIS it is transmitting, then
	 * clear PxCI and PxSACT and finally CR.  The specification gives it
	 * 500 ms. */
	if (cmd & AHCI_PxCMD_ST)
		ahci_port_cmd_rmw(port, AHCI_PxCMD_ST, 0);
	rc = ahci_port_wait_bits(port, AHCI_PxCMD, AHCI_PxCMD_CR, 0,
				 AHCI_ENGINE_STOP_MS, &last);
	if (rc) {
		char cbuf[128];

		WARN_RATELIMIT(1,
			       "%s: command list engine did not stop in %u ms "
			       "(PxCMD %08x [%s], CI %08x)",
			       port->name, AHCI_ENGINE_STOP_MS, last,
			       ahci_port_cmd_text(last, cbuf, sizeof(cbuf)),
			       ahci_port_readl(port, AHCI_PxCI));
		return -ETIMEDOUT;
	}
	port_dbg(port, "engine stopped (PxCMD %08x)", last);
	return 0;
}

int ahci_port_stop_engine_abort(struct ahci_port *port)
{
	uint64_t fl;

	BUG_ON(!port);
	/* Clearing ST makes the HBA clear PxCI and PxSACT of every command
	 * it holds.  A completion pass (interrupt, poll timer, a polled
	 * waiter) after that would read the cleared bits as "finished" and
	 * complete reads that never filled their buffers and writes that
	 * never reached the medium as successes.  Claiming the commands for
	 * error handling and clearing ST under the same lock leaves no
	 * window: the completion path skips claimed slots, and nothing can
	 * be issued between the claim and the stop.  The caller fails or
	 * requeues the claimed commands afterwards. */
	spin_lock_irqsave(&port->lock, &fl);
	(void)ahci_cmd_claim_active_locked(port);
	if (ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_ST)
		(void)ahci_port_cmd_rmw_locked(port, AHCI_PxCMD_ST, 0);
	spin_unlock_irqrestore(&port->lock, fl);
	/* ST is clear now: this only waits for CR (and reports a hang). */
	return ahci_port_stop_engine(port);
}

int ahci_port_start_engine(struct ahci_port *port)
{
	uint32_t cmd, tfd, ssts;

	BUG_ON(!port);
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if (cmd & AHCI_PxCMD_ST)
		return 0;

	/* 10.3.1: ST may only be set while CR is clear, with the FIS receive
	 * engine running, and while the device is not BSY/DRQ (unless CLO
	 * was just used, which clears those bits in PxTFD). */
	if (WARN_ON(cmd & AHCI_PxCMD_CR))
		return -EBUSY;
	if (!(cmd & AHCI_PxCMD_FRE) || !(cmd & AHCI_PxCMD_FR)) {
		port_warn(port, "start of the command engine without FIS "
				"receive (PxCMD %08x)",
			  cmd);
		return -EINVAL;
	}
	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if (tfd & (AHCI_PxTFD_STS_BSY | AHCI_PxTFD_STS_DRQ)) {
		port_dbg(port, "engine start refused: TFD %08x busy", tfd);
		return -EBUSY;
	}
	ssts = ahci_port_readl(port, AHCI_PxSSTS);
	if (AHCI_SSTS_DET(ssts) != AHCI_SSTS_DET_PHY)
		port_dbg(port, "engine start without a link (SSTS %08x)",
			 ssts);

	ahci_port_cmd_rmw(port, 0, AHCI_PxCMD_ST);
	if (port->hba->quirks & AHCI_Q_DELAY_ENGINE)
		ahci_mdelay(1);
	port_dbg(port, "engine started (PxCMD %08x)",
		 ahci_port_readl(port, AHCI_PxCMD));
	return 0;
}

int ahci_port_stop_fis_rx(struct ahci_port *port)
{
	uint32_t cmd, last = 0;
	int rc;

	BUG_ON(!port);
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if (!(cmd & (AHCI_PxCMD_FRE | AHCI_PxCMD_FR)))
		return 0;
	/* FRE may only be cleared once the command engine is idle. */
	if (cmd & (AHCI_PxCMD_ST | AHCI_PxCMD_CR)) {
		port_warn(port, "FIS receive stop with the engine running "
				"(PxCMD %08x)",
			  cmd);
		return -EBUSY;
	}
	if (cmd & AHCI_PxCMD_FRE)
		ahci_port_cmd_rmw(port, AHCI_PxCMD_FRE, 0);
	rc = ahci_port_wait_bits(port, AHCI_PxCMD, AHCI_PxCMD_FR, 0,
				 AHCI_FIS_RX_STOP_MS, &last);
	if (rc) {
		WARN_RATELIMIT(1,
			       "%s: FIS receive engine did not stop in %u ms "
			       "(PxCMD %08x)",
			       port->name, AHCI_FIS_RX_STOP_MS, last);
		return -ETIMEDOUT;
	}
	return 0;
}

int ahci_port_start_fis_rx(struct ahci_port *port)
{
	uint32_t cmd, last = 0;
	int rc;

	BUG_ON(!port);
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if ((cmd & AHCI_PxCMD_FRE) && (cmd & AHCI_PxCMD_FR))
		return 0;
	/* PxFB must point at our area before FRE goes on. */
	if (WARN_ON(ahci_port_readl(port, AHCI_PxFB) == 0 &&
		    ahci_port_readl(port, AHCI_PxFBU) == 0))
		return -EINVAL;
	if (!(cmd & AHCI_PxCMD_FRE))
		ahci_port_cmd_rmw(port, 0, AHCI_PxCMD_FRE);
	rc = ahci_port_wait_bits(port, AHCI_PxCMD, AHCI_PxCMD_FR,
				 AHCI_PxCMD_FR, AHCI_FIS_RX_STOP_MS, &last);
	if (rc) {
		/* Not fatal: FR follows FRE on every HBA seen so far, a slow
		 * one is reported and the caller carries on. */
		WARN_RATELIMIT(1,
			       "%s: FIS receive did not start in %u ms "
			       "(PxCMD %08x)",
			       port->name, AHCI_FIS_RX_STOP_MS, last);
		return -ETIMEDOUT;
	}
	return 0;
}

int ahci_port_clo(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	uint32_t cmd, last = 0, tfd;
	int rc;

	if (!hba->caps.sclo || (hba->quirks & AHCI_Q_NO_CLO))
		return -EOPNOTSUPP;
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	/* 3.3.7: CLO only with ST clear. */
	if (WARN_ON(cmd & AHCI_PxCMD_ST))
		return -EBUSY;

	port->stats.clo++;
	ahci_port_cmd_rmw(port, 0, AHCI_PxCMD_CLO);
	rc = ahci_port_wait_bits(port, AHCI_PxCMD, AHCI_PxCMD_CLO, 0,
				 AHCI_CLO_TIMEOUT_MS, &last);
	if (rc) {
		WARN_RATELIMIT(1, "%s: command list override did not clear "
				  "(PxCMD %08x)",
			       port->name, last);
		return -ETIMEDOUT;
	}
	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if (tfd & (AHCI_PxTFD_STS_BSY | AHCI_PxTFD_STS_DRQ)) {
		/* The HBA cleared CLO but not the bits it is meant to
		 * override; seen on HBAs whose CLO is broken. */
		WARN_RATELIMIT(1, "%s: CLO left TFD %08x busy", port->name,
			       tfd);
		return -ETIMEDOUT;
	}
	port_dbg(port, "command list override done (TFD %08x)", tfd);
	return 0;
}

int ahci_port_engine_restart(struct ahci_port *port)
{
	uint32_t tfd;
	uint64_t fl;
	int rc;

	BUG_ON(!port);
	ahci_assert_eh_held(port);
	port->stats.aborts++;

	rc = ahci_port_stop_engine(port);
	if (rc)
		return rc; /* the caller escalates to a reset */

	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_clear_status_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);

	if (!(ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_FR)) {
		rc = ahci_port_start_fis_rx(port);
		if (rc)
			return rc;
	}
	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if (tfd & (AHCI_PxTFD_STS_BSY | AHCI_PxTFD_STS_DRQ)) {
		rc = ahci_port_clo(port);
		if (rc == -EOPNOTSUPP)
			return -EBUSY;
		if (rc)
			return rc;
	}
	/* Nothing to run on an empty port. */
	if (AHCI_SSTS_DET(ahci_port_readl(port, AHCI_PxSSTS)) !=
	    AHCI_SSTS_DET_PHY)
		return 0;
	return ahci_port_start_engine(port);
}

/* ==========================================================================
 * SCR access
 * ========================================================================== */

static uint32_t ahci_scr_offset(int scr)
{
	switch (scr) {
	case AHCI_SCR_STATUS:
		return AHCI_PxSSTS;
	case AHCI_SCR_ERROR:
		return AHCI_PxSERR;
	case AHCI_SCR_CONTROL:
		return AHCI_PxSCTL;
	case AHCI_SCR_ACTIVE:
		return AHCI_PxSACT;
	case AHCI_SCR_NOTIFY:
		return AHCI_PxSNTF;
	default:
		return 0;
	}
}

int ahci_link_scr_read(struct ahci_link *link, int scr, uint32_t *val)
{
	struct ahci_port *port;
	uint32_t off, v;

	if (!link || !val)
		return -EINVAL;
	port = link->port;
	if (!link->is_host)
		return ahci_pmp_scr_read(link, scr, val);
	off = ahci_scr_offset(scr);
	if (!off)
		return -EINVAL;
	if (scr == AHCI_SCR_NOTIFY &&
	    (!port->hba->caps.ssntf || (port->hba->quirks & AHCI_Q_NO_SNTF)))
		return -EOPNOTSUPP;
	v = ahci_port_readl(port, off);
	*val = v;
	switch (scr) {
	case AHCI_SCR_STATUS:
		link->sstatus = v;
		break;
	case AHCI_SCR_ERROR:
		link->serror = v;
		break;
	case AHCI_SCR_CONTROL:
		link->scontrol = v;
		break;
	default:
		break;
	}
	return 0;
}

int ahci_link_scr_write(struct ahci_link *link, int scr, uint32_t val)
{
	struct ahci_port *port;
	uint64_t fl;
	uint32_t off;

	if (!link)
		return -EINVAL;
	port = link->port;
	if (!link->is_host)
		return ahci_pmp_scr_write(link, scr, val);
	off = ahci_scr_offset(scr);
	if (!off)
		return -EINVAL;
	switch (scr) {
	case AHCI_SCR_STATUS:
	case AHCI_SCR_ACTIVE:
		/* SStatus is read-only; SActive is the issue path's (writes
		 * there set NCQ tags). */
		return -EINVAL;
	case AHCI_SCR_NOTIFY:
		if (!port->hba->caps.ssntf ||
		    (port->hba->quirks & AHCI_Q_NO_SNTF))
			return -EOPNOTSUPP;
		break;
	default:
		break;
	}
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, off, val);
	ahci_port_flush(port);
	spin_unlock_irqrestore(&port->lock, fl);
	if (scr == AHCI_SCR_CONTROL)
		link->scontrol = val;
	return 0;
}

void ahci_link_refresh(struct ahci_link *link)
{
	struct ahci_link_status st;
	uint32_t ssts = 0, sctl = 0, serr = 0;

	if (!link)
		return;
	if (ahci_link_scr_read(link, AHCI_SCR_STATUS, &ssts) != 0)
		return;
	(void)ahci_link_scr_read(link, AHCI_SCR_CONTROL, &sctl);
	(void)ahci_link_scr_read(link, AHCI_SCR_ERROR, &serr);
	ahci_decode_ssts(ssts, &st);
	link->speed = st.spd;
	if (link->state != AHCI_LINK_RESETTING) {
		if (st.det == AHCI_SSTS_DET_OFFLINE ||
		    (sctl & AHCI_SCTL_DET_MASK) == AHCI_SCTL_DET_DISABLE)
			link->state = AHCI_LINK_DISABLED;
		else
			link->state = st.online ? AHCI_LINK_ONLINE
						: AHCI_LINK_OFFLINE;
	}
}

/* ==========================================================================
 * SControl: speed limit and IPM permissions
 * ========================================================================== */

/* The SControl.IPM value ("transitions NOT allowed") for a policy on the
 * host link, limited by what the HBA can do: no partial without CAP.PSC,
 * no slumber without CAP.SSC, no DevSleep without PxDEVSLP.DSP.  Under
 * keep_firmware_settings the firmware's bits stay. */
static uint32_t ahci_policy_sctl_ipm(const struct ahci_port *port,
				     const struct ahci_link *link,
				     enum ahci_lpm_policy pol)
{
	const struct ahci_hba *hba = port->hba;
	uint32_t ipm;

	if (hba->quirks & AHCI_Q_NO_LPM)
		pol = AHCI_LPM_MAX_PERFORMANCE;

	switch (pol) {
	case AHCI_LPM_KEEP_FIRMWARE:
		return port->saved_sctl & AHCI_SCTL_IPM_MASK;
	case AHCI_LPM_MEDIUM_POWER:
		/* partial only */
		ipm = AHCI_SCTL_IPM_NO_SD;
		break;
	case AHCI_LPM_MED_POWER_WITH_DIPM:
		/* partial and slumber */
		ipm = AHCI_SCTL_IPM_NO_DEVSLP;
		break;
	case AHCI_LPM_MIN_POWER:
		ipm = AHCI_SCTL_IPM_ANY;
		break;
	case AHCI_LPM_MAX_PERFORMANCE:
	default:
		return AHCI_SCTL_IPM_NONE;
	}
	if (link->is_host) {
		if (!hba->caps.psc)
			ipm |= AHCI_SCTL_IPM_NO_PARTIAL;
		if (!hba->caps.ssc)
			ipm |= AHCI_SCTL_IPM_NO_SLUMBER;
		if (!port->devslp_capable)
			ipm |= AHCI_SCTL_IPM_NO_DEVSLP;
	} else {
		/* A fan-out link: older multipliers know only the two-bit
		 * field of SATA 2.6 (partial / slumber). */
		ipm &= AHCI_SCTL_IPM_NO_PS;
	}
	return ipm;
}

/* The SControl.SPD value for a link: its limit, never above the HBA. */
static uint32_t ahci_link_sctl_spd(const struct ahci_link *link)
{
	uint8_t hw = ahci_port_hw_speed_max(link->port);
	uint8_t lim = link->speed_limit;

	if (lim == 0)
		return 0; /* no restriction */
	if (lim > hw)
		lim = hw;
	return (uint32_t)lim << AHCI_SCTL_SPD_SHIFT;
}

/* Build an SControl value from `old': DET as given, SPD from the link's
 * limit, IPM from the policy, PMP and SPM fields zero. */
static uint32_t ahci_link_sctl_value(const struct ahci_link *link,
				     uint32_t old, uint32_t det)
{
	const struct ahci_port *port = link->port;
	enum ahci_lpm_policy pol =
		link->is_host ? port->lpm_policy : link->lpm_policy;

	/* Every field is recomputed: the PMP and SPM fields of the host
	 * link's SControl are unused by AHCI and must read zero. */
	(void)old;
	return (det & AHCI_SCTL_DET_MASK) | ahci_link_sctl_spd(link) |
	       ahci_policy_sctl_ipm(port, link, pol);
}

/* Bring the PHY online without a reset (DET = 0 from DET = 4). */
int ahci_port_phy_online(struct ahci_port *port)
{
	uint32_t sctl;
	uint64_t fl;

	BUG_ON(!port);
	sctl = ahci_port_readl(port, AHCI_PxSCTL);
	if ((sctl & AHCI_SCTL_DET_MASK) == AHCI_SCTL_DET_NONE)
		return 0;
	spin_lock_irqsave(&port->lock, &fl);
	sctl = ahci_link_sctl_value(&port->host_link, sctl, AHCI_SCTL_DET_NONE);
	ahci_port_writel(port, AHCI_PxSCTL, sctl);
	(void)ahci_port_readl(port, AHCI_PxSCTL);
	spin_unlock_irqrestore(&port->lock, fl);
	port->host_link.scontrol = sctl;
	if (port->host_link.state == AHCI_LINK_DISABLED)
		port->host_link.state = AHCI_LINK_OFFLINE;
	port_dbg(port, "PHY online (SControl %X)", sctl);
	return 0;
}

int ahci_port_phy_offline(struct ahci_port *port)
{
	uint32_t sctl, last = 0;
	uint64_t fl;
	int rc;

	BUG_ON(!port);
	ahci_assert_eh_held(port);
	/* SCTL.DET may only change with the engine stopped. */
	(void)ahci_port_stop_engine(port);
	spin_lock_irqsave(&port->lock, &fl);
	sctl = ahci_port_readl(port, AHCI_PxSCTL);
	sctl = (sctl & ~AHCI_SCTL_DET_MASK) | AHCI_SCTL_DET_DISABLE;
	ahci_port_writel(port, AHCI_PxSCTL, sctl);
	(void)ahci_port_readl(port, AHCI_PxSCTL);
	spin_unlock_irqrestore(&port->lock, fl);
	port->host_link.scontrol = sctl;
	port->host_link.state = AHCI_LINK_DISABLED;

	rc = ahci_port_wait_bits(port, AHCI_PxSSTS, AHCI_SSTS_DET_MASK,
				 AHCI_SSTS_DET_OFFLINE, AHCI_LINK_UP_MS, &last);
	if (rc)
		port_dbg(port, "PHY did not report offline (SStatus %X)",
			 last);
	port_info(port, "SATA PHY taken offline (SControl %X)", sctl);
	return 0;
}

/* ==========================================================================
 * Link debounce, speed step-down
 * ========================================================================== */

/* The next lower speed for a link, from the speed it runs at (or its
 * current limit, or the HBA maximum).  -ERANGE at Gen1. */
static int ahci_link_lower_speed(const struct ahci_link *link, uint8_t *out)
{
	uint8_t cur = (uint8_t)AHCI_SSTS_SPD(link->sstatus);

	if (cur == AHCI_SSTS_SPD_NONE || cur > AHCI_SSTS_SPD_GEN3)
		cur = link->speed;
	if (cur == AHCI_SSTS_SPD_NONE || cur > AHCI_SSTS_SPD_GEN3)
		cur = link->speed_limit;
	if (cur == AHCI_SSTS_SPD_NONE)
		cur = ahci_port_hw_speed_max(link->port);
	if (link->speed_limit && link->speed_limit < cur)
		cur = link->speed_limit;
	if (cur <= AHCI_SSTS_SPD_GEN1)
		return -ERANGE;
	*out = (uint8_t)(cur - 1);
	return 0;
}

/* Before a reset: when the link collected AHCI_SPEED_DOWN_ERRS link errors
 * inside the window, lower its speed limit one step so this reset already
 * negotiates the slower speed.  A link that is at 1.5 Gbps just restarts
 * counting. */
void ahci_link_check_speed_down(struct ahci_link *link)
{
	struct ahci_port *port = link->port;
	uint64_t now = ahci_now_ns();
	uint8_t lower;

	if (link->spd_err_count < AHCI_SPEED_DOWN_ERRS)
		return;
	if (link->spd_err_window_ns &&
	    now - link->spd_err_window_ns >
		    (uint64_t)AHCI_SPEED_DOWN_WINDOW_MS * 1000000ULL) {
		/* stale errors: start a new window */
		link->spd_err_count = 0;
		link->spd_err_window_ns = now;
		return;
	}
	link->spd_err_count = 0;
	link->spd_err_window_ns = now;
	if (ahci_link_lower_speed(link, &lower) != 0) {
		port_warn(port, "%s: repeated link errors at the lowest speed",
			  link->name);
		return;
	}
	link->speed_limit = lower;
	link->n_speed_downs++;
	port_info(port, "%s: limiting SATA link speed to %s after %u link "
			"errors",
		  link->name, ahci_speed_str(lower), AHCI_SPEED_DOWN_ERRS);
}

/* Count one link error towards the speed step-down. */
void ahci_link_note_error(struct ahci_link *link)
{
	uint64_t now = ahci_now_ns();

	__atomic_fetch_add(&link->n_link_errors, 1, __ATOMIC_RELAXED);
	if (link->spd_err_window_ns == 0 ||
	    now - link->spd_err_window_ns >
		    (uint64_t)AHCI_SPEED_DOWN_WINDOW_MS * 1000000ULL) {
		link->spd_err_window_ns = now;
		link->spd_err_count = 0;
	}
	__atomic_fetch_add(&link->spd_err_count, 1, __ATOMIC_RELAXED);
}

int ahci_link_set_speed_limit(struct ahci_link *link, uint8_t spd)
{
	struct ahci_port *port;
	uint8_t hw;

	if (!link)
		return -EINVAL;
	port = link->port;
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	if (spd > AHCI_SSTS_SPD_GEN3)
		return -EINVAL;
	hw = ahci_port_hw_speed_max(port);
	if (spd > hw)
		spd = 0; /* above what the HBA can do = no limit */
	if (spd && (link->speed_limit == 0 || spd < link->speed_limit))
		link->n_speed_downs++;
	link->speed_limit = spd;
	if (spd)
		port_info(port, "%s: SATA link speed limited to %s",
			  link->name, ahci_speed_str(spd));
	else
		port_info(port, "%s: SATA link speed limit removed",
			  link->name);

	/* SControl.SPD takes effect at the next COMRESET: error handling's
	 * own, with the port frozen across it (its PHY events are not
	 * hot-plug events) and the devices revalidated after it. */
	ahci_eh_request(port, AHCI_EH_RESET_HARD);
	return 0;
}

/* ==========================================================================
 * COMRESET
 * ========================================================================== */

/* Log the link state the way users expect it:
 *   ata1: SATA link up 6.0 Gbps (SStatus 133 SControl 300)
 *   ata1: SATA link down (SStatus 0 SControl 300) */
static void ahci_link_log_state(struct ahci_port *port,
				struct ahci_link *link, uint32_t ssts,
				uint32_t sctl)
{
	const char *who = link->is_host ? port->name : link->name;

	if (AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_PHY)
		kprintf("[ahci] %s: SATA link up %s (SStatus %X SControl %X)\n",
			who, ahci_speed_str(AHCI_SSTS_SPD(ssts)), ssts, sctl);
	else if (AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_NONE)
		kprintf("[ahci] %s: SATA link down (SStatus %X SControl %X)\n",
			who, ssts, sctl);
	else
		kprintf("[ahci] %s: SATA link down: %s (SStatus %X SControl "
			"%X)\n",
			who, ahci_det_name(AHCI_SSTS_DET(ssts)), ssts, sctl);
}

/* Link debouncing of a reset attempt: SStatus.DET sampled every
 * timing[0] ms has to keep its value for timing[1] ms; DET = 1 (device
 * seen, PHY not up) is waited out until the deadline -- the attempt's, or
 * timing[2] ms from now if that comes first.  -EPIPE: DET still changing
 * at the deadline (the link speed should be lowered).  `deadline_ns'
 * AHCI_NO_DEADLINE: only timing[2] bounds it.  The sleeps are counted as
 * well as the clock read: a clock that does not advance (no timer source
 * yet) still ends every wait.  In two steps, so several links can be
 * debounced in one loop: the first sample, then one per interval. */
struct ahci_deb {
	const uint32_t *timing;
	uint64_t deadline_ns, last_ns;
	uint32_t last, waited_ms, stable_ms;
	uint32_t sstatus; /* the last SStatus read (0: none yet) */
};

static int ahci_link_deb_begin(struct ahci_link *link, struct ahci_deb *d,
			       const uint32_t *timing, uint64_t deadline_ns)
{
	uint64_t t;
	uint32_t cur;
	int rc;

	d->timing = timing;
	d->waited_ms = 0;
	d->stable_ms = 0;
	d->sstatus = 0;
	t = hrtimer_now_ns() + (uint64_t)timing[2] * 1000000ULL;
	d->deadline_ns = t < deadline_ns ? t : deadline_ns;
	rc = ahci_link_scr_read(link, AHCI_SCR_STATUS, &cur);
	if (rc)
		return rc;
	d->sstatus = cur;
	d->last = AHCI_SSTS_DET(cur);
	d->last_ns = hrtimer_now_ns();
	return 0;
}

/* One sample, timing[0] ms after the previous one: 1 to go on, 0 settled,
 * -EPIPE or a read error. */
static int ahci_link_deb_step(struct ahci_link *link, struct ahci_deb *d)
{
	const uint32_t *timing = d->timing;
	uint64_t now;
	uint32_t cur;
	int rc, past;

	d->waited_ms += timing[0];
	rc = ahci_link_scr_read(link, AHCI_SCR_STATUS, &cur);
	if (rc)
		return rc;
	d->sstatus = cur;
	now = hrtimer_now_ns();
	past = now > d->deadline_ns || d->waited_ms > timing[2];
	if (AHCI_SSTS_DET(cur) == d->last) {
		d->stable_ms += timing[0];
		if (d->last == AHCI_SSTS_DET_PRESENT && !past)
			return 1;
		if (now > d->last_ns + (uint64_t)timing[1] * 1000000ULL ||
		    d->stable_ms > timing[1])
			return 0;
		return 1;
	}
	/* unstable: start over */
	d->last = AHCI_SSTS_DET(cur);
	d->last_ns = now;
	d->stable_ms = 0;
	return past ? -EPIPE : 1;
}

int ahci_link_debounce_deadline(struct ahci_link *link,
				const uint32_t *timing, uint64_t deadline_ns,
				uint32_t *sstatus)
{
	struct ahci_deb d;
	int rc;

	rc = ahci_link_deb_begin(link, &d, timing, deadline_ns);
	if (rc)
		return rc;
	if (sstatus)
		*sstatus = d.sstatus;
	do {
		ahci_msleep(timing[0]);
		rc = ahci_link_deb_step(link, &d);
		if (sstatus)
			*sstatus = d.sstatus;
	} while (rc == 1);
	return rc;
}

/* Debounce timings { interval, duration, timeout } (ms) of a reset
 * attempt: after a hot-plug event the connector needs longer to settle. */
static const uint32_t ahci_deb_timing_normal[3] = { 5, 100, 2000 };
static const uint32_t ahci_deb_timing_hotplug[3] = { 25, 500, 2000 };

const uint32_t *ahci_deb_timing(int hotplug)
{
	return hotplug ? ahci_deb_timing_hotplug : ahci_deb_timing_normal;
}

/* COMRESET, first part: the link marked resetting, the engine stopped,
 * FIS receive on, then DET = 1 (after taking the PHY offline for a new
 * speed limit when `timing' is set).  Returns SControl as it was; *sctl:
 * the value asserted.  The caller holds DET for the assertion time. */
static uint32_t ahci_port_comreset_assert(struct ahci_port *port,
					  const uint32_t *timing,
					  uint32_t *sctl_out)
{
	struct ahci_link *link;
	uint32_t sctl_old, sctl;
	uint64_t fl;
	int rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	link = &port->host_link;

	port->stats.comreset++;
	link->n_resets++;
	link->last_reset_ns = ahci_now_ns();
	link->state = AHCI_LINK_RESETTING;
	ahci_link_check_speed_down(link);

	/* 10.4.2: the command engine must be stopped before SCTL.DET is set
	 * to 1.  A failure to stop is reported but the reset goes on -- it
	 * is the remedy for exactly that situation. */
	rc = ahci_port_stop_engine(port);
	if (rc)
		port_warn(port, "COMRESET with the engine still running");
	/* FIS receive stays on: the device's signature FIS must land. */
	if (!(ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_FR))
		(void)ahci_port_start_fis_rx(port);
	ahci_port_clear_rx_d2h(port, -1);

	sctl_old = ahci_port_readl(port, AHCI_PxSCTL);
	if (timing) {
		/* A new speed limit: the PHY goes offline (DET = 4) while
		 * the SPD field changes, then the COMRESET. */
		sctl = ahci_link_sctl_value(link, sctl_old,
					    AHCI_SCTL_DET_DISABLE);
		if ((sctl & AHCI_SCTL_SPD_MASK) !=
		    (sctl_old & AHCI_SCTL_SPD_MASK)) {
			spin_lock_irqsave(&port->lock, &fl);
			ahci_port_writel(port, AHCI_PxSCTL, sctl);
			(void)ahci_port_readl(port, AHCI_PxSCTL);
			spin_unlock_irqrestore(&port->lock, fl);
		}
	}
	/* Assert COMRESET: DET = 1 for at least 1 ms. */
	sctl = ahci_link_sctl_value(link, sctl_old, AHCI_SCTL_DET_COMRESET);
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, AHCI_PxSCTL, sctl);
	(void)ahci_port_readl(port, AHCI_PxSCTL); /* post the write */
	spin_unlock_irqrestore(&port->lock, fl);
	*sctl_out = sctl;
	return sctl_old;
}

/* "at least 1 ms": one more than asked, so that a delay that is counted
 * from a tick in progress still holds DET long enough. */
#define AHCI_COMRESET_HOLD_MS (AHCI_COMRESET_ASSERT_MS + 1)

/* The boot probe's release: DET = 0 with the same speed and IPM fields,
 * written once.  Returns the value written. */
static uint32_t ahci_port_comreset_release(struct ahci_port *port,
					   uint32_t sctl_old, uint32_t sctl)
{
	struct ahci_link *link = &port->host_link;
	uint64_t fl;

	sctl = ahci_link_sctl_value(link, sctl, AHCI_SCTL_DET_NONE);
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, AHCI_PxSCTL, sctl);
	(void)ahci_port_readl(port, AHCI_PxSCTL);
	spin_unlock_irqrestore(&port->lock, fl);
	link->scontrol = sctl;
	port_dbg(port, "COMRESET (SControl %X -> %X)", sctl_old, sctl);
	return sctl;
}

static int ahci_port_comreset_finish(struct ahci_port *port, uint32_t sctl,
				     uint32_t ssts);

/* COMRESET.  `timing' NULL: the boot probe's own link wait (the
 * debounce timings of a reset attempt, no deadline but their own);
 * otherwise one reset attempt of error handling, bounded by
 * `deadline_ns', with its debounce timing. */
static int ahci_port_comreset_common(struct ahci_port *port,
				     const uint32_t *timing,
				     uint64_t deadline_ns)
{
	struct ahci_link *link = &port->host_link;
	uint32_t sctl_old, sctl, ssts = 0;
	uint64_t fl;
	int rc;

	sctl_old = ahci_port_comreset_assert(port, timing, &sctl);
	ahci_msleep(AHCI_COMRESET_HOLD_MS);

	if (timing) {
		/* Release: DET = 0 with the same speed and IPM fields. */
		sctl = ahci_link_sctl_value(link, sctl, AHCI_SCTL_DET_NONE);
		/* A reset attempt: the release is checked (some controllers
		 * ignore an SControl write), each time after 200 ms -- some
		 * PHYs react badly to SStatus being read right away -- then
		 * the link is debounced against the attempt's deadline. */
		int tries = AHCI_LINK_RESUME_TRIES;
		uint32_t cur;

		do {
			spin_lock_irqsave(&port->lock, &fl);
			ahci_port_writel(port, AHCI_PxSCTL, sctl);
			spin_unlock_irqrestore(&port->lock, fl);
			ahci_msleep(AHCI_LINK_RESUME_DELAY_MS);
			cur = ahci_port_readl(port, AHCI_PxSCTL);
		} while ((cur & AHCI_SCTL_DET_MASK) != AHCI_SCTL_DET_NONE &&
			 --tries);
		if ((cur & AHCI_SCTL_DET_MASK) != AHCI_SCTL_DET_NONE)
			port_warn(port, "failed to resume link (SControl %X)",
				  cur);
		link->scontrol = sctl;
		port_dbg(port, "COMRESET (SControl %X -> %X)", sctl_old, sctl);
		rc = ahci_link_debounce_deadline(link, timing, deadline_ns,
						 &ssts);
		if (rc == -EPIPE) {
			link->sstatus = ssts;
			link->state = AHCI_LINK_OFFLINE;
			port_warn(port, "link does not settle (SStatus %X)",
				  ssts);
			ahci_port_trace_reset(port, rc, AHCI_EH_LVL_COMRESET);
			return rc;
		}
	} else {
		sctl = ahci_port_comreset_release(port, sctl_old, sctl);
		/* Wait for the link: DET settles to 3 (up) or 0 (empty). */
		rc = ahci_link_debounce_deadline(
			link, ahci_deb_timing(port->eh_hotplugged),
			AHCI_NO_DEADLINE, &ssts);
		(void)rc; /* the DET value decides, settled or not */
	}
	return ahci_port_comreset_finish(port, sctl, ssts);
}

/* COMRESET, last part: the events of the reset cleared, the link state
 * taken from the SStatus the debounce ended with. */
static int ahci_port_comreset_finish(struct ahci_port *port, uint32_t sctl,
				     uint32_t ssts)
{
	struct ahci_link *link = &port->host_link;
	uint32_t serr;
	uint64_t fl;
	int rc;

	/* The reset set SError.X/N (and with them PxIS.PCS/PRCS): clear
	 * them, a COMRESET is not a hot-plug event. */
	spin_lock_irqsave(&port->lock, &fl);
	serr = ahci_port_readl(port, AHCI_PxSERR);
	ahci_port_writel(port, AHCI_PxSERR, serr);
	ahci_port_writel(port, AHCI_PxIS,
			 AHCI_PxIS_PCS | AHCI_PxIS_PRCS | AHCI_PxIS_CPDS |
				 AHCI_PxIS_IFS | AHCI_PxIS_INFS);
	spin_unlock_irqrestore(&port->lock, fl);

	link->sstatus = ssts;
	link->serror = 0;
	link->speed = (uint8_t)AHCI_SSTS_SPD(ssts);
	ahci_link_log_state(port, link, ssts, sctl);

	switch (AHCI_SSTS_DET(ssts)) {
	case AHCI_SSTS_DET_PHY:
		link->state = AHCI_LINK_ONLINE;
		rc = 0;
		break;
	case AHCI_SSTS_DET_PRESENT:
		/* A device answers COMINIT but the PHY never trains: a bad
		 * cable or a speed the device cannot do. */
		link->state = AHCI_LINK_OFFLINE;
		ahci_link_note_error(link);
		rc = -ETIMEDOUT;
		break;
	case AHCI_SSTS_DET_OFFLINE:
		link->state = AHCI_LINK_DISABLED;
		rc = -ENODEV;
		break;
	default:
		link->state = AHCI_LINK_OFFLINE;
		rc = -ENODEV;
		break;
	}
	ahci_port_trace_reset(port, rc, AHCI_EH_LVL_COMRESET);
	return rc;
}

int ahci_port_comreset(struct ahci_port *port)
{
	return ahci_port_comreset_common(port, NULL, 0);
}

/* ==========================================================================
 * Device readiness and classification
 * ========================================================================== */

int ahci_port_wait_ready(struct ahci_port *port, uint32_t timeout_ms)
{
	uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;
	uint64_t next_link_check_us = 10000;
	uint32_t tfd = 0;

	BUG_ON(!port);
	for (;;) {
		tfd = ahci_port_readl(port, AHCI_PxTFD);
		if (!(tfd & (AHCI_PxTFD_STS_BSY | AHCI_PxTFD_STS_DRQ)))
			return 0;
		if (waited_us >= next_link_check_us) {
			uint32_t det = AHCI_SSTS_DET(
				ahci_port_readl(port, AHCI_PxSSTS));

			next_link_check_us = waited_us + 10000;
			/* The link went away while we waited: nothing will
			 * become ready. */
			if (det != AHCI_SSTS_DET_PHY &&
			    det != AHCI_SSTS_DET_PRESENT) {
				port_dbg(port, "link lost while waiting for "
					       "ready (TFD %08x)",
					 tfd);
				return -ENODEV;
			}
		}
		if (waited_us >= limit_us)
			break;
		if (waited_us < 1000) {
			ahci_udelay(AHCI_FAST_POLL_US);
			waited_us += AHCI_FAST_POLL_US;
		} else {
			ahci_msleep(1);
			waited_us += 1000;
		}
	}
	port_dbg(port, "device not ready after %u ms (TFD %08x)", timeout_ms,
		 tfd);
	return -ETIMEDOUT;
}

/* Wait for the D2H FIS of `pmp' to arrive with BSY and DRQ clear (used
 * with FBS, where PxTFD is not the device's status). */
static int ahci_port_wait_rx_ready(struct ahci_port *port, int pmp,
				   uint32_t timeout_ms)
{
	uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;

	for (;;) {
		volatile struct fis_reg_d2h *d2h;
		uint8_t type, status;

		ahci_rmb();
		d2h = ahci_port_rx_d2h(port, pmp);
		type = d2h->fis_type;
		status = d2h->status;
		if (type == FIS_TYPE_REG_D2H &&
		    !(status & (ATA_BUSY | ATA_DRQ)))
			return 0;
		if (waited_us >= limit_us)
			return -ETIMEDOUT;
		if (waited_us < 1000) {
			ahci_udelay(AHCI_FAST_POLL_US);
			waited_us += AHCI_FAST_POLL_US;
		} else {
			ahci_msleep(1);
			waited_us += 1000;
		}
	}
}

enum ata_class ahci_port_classify(struct ahci_port *port)
{
	struct ahci_link *link;
	enum ata_class cls;
	uint32_t sig, ssts;

	BUG_ON(!port);
	link = &port->host_link;
	ssts = ahci_port_readl(port, AHCI_PxSSTS);
	if (AHCI_SSTS_DET(ssts) != AHCI_SSTS_DET_PHY) {
		link->class = ATA_DEV_NONE;
		return ATA_DEV_NONE;
	}
	sig = ahci_port_readl(port, AHCI_PxSIG);
	link->sig = sig;
	if (sig == AHCI_PxSIG_INVALID) {
		/* The link is up but no signature FIS arrived. */
		cls = ATA_DEV_UNKNOWN;
	} else {
		cls = ata_class_from_sig(sig);
	}
	port_dbg(port, "signature %08x -> %s", sig, ata_class_name(cls));
	link->class = cls;
	return cls;
}

/* PxCMD.ATAPI (and the drive LED for ATAPI commands) follow the class. */
static void ahci_port_set_atapi(struct ahci_port *port, int atapi)
{
	uint32_t cmd = ahci_port_readl(port, AHCI_PxCMD);
	uint32_t want = atapi ? (AHCI_PxCMD_ATAPI |
				 (port->hba->caps.sal ? AHCI_PxCMD_DLAE : 0))
			      : 0;

	if ((cmd & (AHCI_PxCMD_ATAPI | AHCI_PxCMD_DLAE)) == want)
		return;
	ahci_port_cmd_rmw(port, AHCI_PxCMD_ATAPI | AHCI_PxCMD_DLAE, want);
	if (atapi)
		port->flags |= AHCI_PORT_F_ATAPI;
	else
		port->flags &= ~AHCI_PORT_F_ATAPI;
}

/* ==========================================================================
 * Software reset (AHCI 1.3.1, 10.4.1)
 * ========================================================================== */

/* Get the engine into a state where a reset FIS can be issued: stopped,
 * CLO if the device holds BSY/DRQ, started again. */
static int ahci_port_kick_engine(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	uint32_t tfd;
	int rc;

	rc = ahci_port_stop_engine(port);
	if (rc)
		return rc;
	if (!(ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_FR)) {
		rc = ahci_port_start_fis_rx(port);
		if (rc)
			return rc;
	}
	tfd = ahci_port_readl(port, AHCI_PxTFD);
	if (tfd & (AHCI_PxTFD_STS_BSY | AHCI_PxTFD_STS_DRQ)) {
		if (hba->quirks & AHCI_Q_SRST_NO_CLO)
			return -EBUSY;
		rc = ahci_port_clo(port);
		if (rc == -EOPNOTSUPP)
			return -EBUSY; /* only a COMRESET helps now */
		if (rc)
			return rc;
	}
	return ahci_port_start_engine(port);
}

/* The two FISes of the SRST sequence.  0 or the raw-FIS error. */
static int ahci_port_srst_fises(struct ahci_port *port, int pmp,
				uint32_t timeout_ms)
{
	struct fis_reg_h2d fis;
	int rc;

	/* 1: SRST set.  R (reset) and C (clear BSY after R_OK): the HBA
	 * clears the slot as soon as the FIS is acknowledged -- the device
	 * answers nothing while SRST is asserted. */
	kmemset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_REG_H2D;
	fis.pm_c = (uint8_t)(pmp & FIS_PMP_MASK); /* control update, C = 0 */
	fis.control = ATA_SRST;
	rc = ahci_exec_raw_fis(port, pmp, &fis, AHCI_CMDH_R | AHCI_CMDH_C,
			       timeout_ms);
	if (rc) {
		port_dbg(port, "SRST assert to pmp %d failed (%d)", pmp, rc);
		return rc;
	}

	/* The device needs SRST asserted for at least 5 us. */
	ahci_udelay(AHCI_SRST_ASSERT_US);

	/* 2: SRST clear, no special header bits: the slot completes when
	 * the device sends its D2H register FIS with the signature. */
	kmemset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_REG_H2D;
	fis.pm_c = (uint8_t)(pmp & FIS_PMP_MASK);
	fis.control = 0;
	rc = ahci_exec_raw_fis(port, pmp, &fis, 0, timeout_ms);
	if (rc)
		port_dbg(port, "SRST release to pmp %d failed (%d)", pmp, rc);
	return rc;
}

/* Milliseconds left until `deadline_ns' (at least 1); `dflt' without a
 * deadline. */
static uint32_t ahci_ms_left(uint64_t deadline_ns, uint32_t dflt)
{
	uint64_t now;

	if (!deadline_ns)
		return dflt;
	now = hrtimer_now_ns();
	if (now >= deadline_ns)
		return 1;
	return (uint32_t)((deadline_ns - now + 999999ULL) / 1000000ULL);
}

/* SRST to `pmp'.  `deadline_ns' (0: none): a reset attempt's deadline,
 * which bounds both FISes and the wait for the device to become ready --
 * an attempt of error handling's reset passes it through. */
static int ahci_port_softreset_common(struct ahci_port *port, int pmp,
				      uint32_t *sig, uint64_t deadline_ns)
{
	struct ahci_hba *hba;
	uint32_t s = AHCI_PxSIG_INVALID;
	uint8_t status = 0;
	int rc, tries;

	BUG_ON(!port);
	hba = port->hba;
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	if (pmp < 0 || pmp > ATA_PMP_CTRL_PORT)
		return -EINVAL;
	if (AHCI_SSTS_DET(ahci_port_readl(port, AHCI_PxSSTS)) !=
	    AHCI_SSTS_DET_PHY)
		return -ENODEV;

	port->stats.srst++;
	tries = (hba->quirks & AHCI_Q_SRST_RETRY) ? 2 : 1;
	for (int t = 0; t < tries; t++) {
		rc = ahci_port_kick_engine(port);
		if (rc) {
			port_dbg(port, "SRST: engine kick failed (%d)", rc);
			goto out;
		}
		ahci_port_clear_rx_d2h(port, pmp);
		if (port->fbs_enabled) {
			uint64_t fl;

			/* With FBS, PxFBS.DEV names the PM port of the next
			 * command before its CI bit is set. */
			spin_lock_irqsave(&port->lock, &fl);
			ahci_fbs_select_dev_locked(port, pmp);
			spin_unlock_irqrestore(&port->lock, fl);
		}
		rc = ahci_port_srst_fises(
			port, pmp, ahci_ms_left(deadline_ns, AHCI_SRST_TIMEOUT_MS));
		if (rc == 0)
			break;
		if (t + 1 < tries) {
			port_info(port, "SRST failed (%d), retrying", rc);
			ahci_msleep(10);
		}
	}
	if (rc)
		goto out;

	/* Ready: PxTFD carries the status of the last D2H FIS; with FBS the
	 * per-device area must be watched instead. */
	if (port->fbs_enabled)
		rc = ahci_port_wait_rx_ready(
			port, pmp, ahci_ms_left(deadline_ns, AHCI_DEV_READY_MS));
	else
		rc = ahci_port_wait_ready(
			port, ahci_ms_left(deadline_ns, AHCI_DEV_READY_MS));
	if (rc) {
		port_dbg(port, "SRST: device %d not ready (TFD %08x)", pmp,
			 ahci_port_readl(port, AHCI_PxTFD));
		goto out;
	}

	s = ahci_port_rx_sig(port, pmp, &status);
	if (s == AHCI_PxSIG_INVALID && !port->pmp_attached) {
		/* Without a received FIS (some HBAs do not post the
		 * signature FIS into the area): PxSIG. */
		s = ahci_port_readl(port, AHCI_PxSIG);
	}
	port_dbg(port, "SRST pmp %d: signature %08x status %02x", pmp, s,
		 status);
	rc = 0;
out:
	if (sig)
		*sig = s;
	ahci_port_trace_reset(port, rc, AHCI_EH_LVL_SRST);
	if (rc && rc != -ENODEV)
		WARN_RATELIMIT(1, "%s: software reset of pmp %d failed (%d)",
			       port->name, pmp, rc);
	return rc;
}

int ahci_port_softreset(struct ahci_port *port, int pmp, uint32_t *sig)
{
	return ahci_port_softreset_common(port, pmp, sig, 0);
}

int ahci_port_softreset_deadline(struct ahci_port *port, int pmp,
				 uint32_t *sig, uint64_t deadline_ns)
{
	return ahci_port_softreset_common(port, pmp, sig, deadline_ns);
}

/* ==========================================================================
 * Hard reset: COMRESET + ready + classify
 * ========================================================================== */

/* The COMRESET with the device's readiness waited for.  `deadline_ns'
 * (0: none) bounds every wait of this attempt: a device not ready by
 * then fails the attempt, and the caller's next attempt gets its own,
 * longer, deadline.  Without one the attempt waits for a spinning-up
 * drive and tries a waking SRST itself (boot probe, resume). */
static int ahci_port_hardreset_rest(struct ahci_port *port,
				    enum ata_class *class,
				    uint64_t deadline_ns, int rc);

static int ahci_port_hardreset_common(struct ahci_port *port,
				      enum ata_class *class,
				      uint64_t deadline_ns,
				      const uint32_t *timing)
{
	int rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	rc = deadline_ns ? ahci_port_comreset_common(port, timing, deadline_ns)
			 : ahci_port_comreset(port);
	return ahci_port_hardreset_rest(port, class, deadline_ns, rc);
}

/* The hard reset after its COMRESET (whose result is `rc'): the device's
 * readiness waited for, the link classified, the engine started. */
static int ahci_port_hardreset_rest(struct ahci_port *port,
				    enum ata_class *class,
				    uint64_t deadline_ns, int rc)
{
	struct ahci_link *link = &port->host_link;
	enum ata_class cls = ATA_DEV_NONE;
	uint32_t tfd, sig;
	int pmp_capable = ahci_port_pmp_capable(port);

	if (rc) {
		cls = rc == -ENODEV ? ATA_DEV_NONE : ATA_DEV_UNKNOWN;
		link->class = cls;
		if (class)
			*class = cls;
		return rc;
	}

	/* The device sends its signature FIS once it finished its power-on
	 * or reset diagnostics; until then PxTFD reads 7Fh/80h.
	 *
	 * On an HBA that supports port multipliers the wait is short: a
	 * multiplier answers the COMRESET only from its control port, and
	 * some keep BSY set until they get an SRST there, so after a brief
	 * look the follow-up SRST to port 15 classifies the link (a device
	 * that is not a multiplier ignores the PM port field and answers
	 * with its own signature).  Only when that fails too does the long
	 * spin-up wait follow. */
	if (deadline_ns) {
		/* The attempt's deadline; with port multiplier support only
		 * AHCI_PMP_SRST_WAIT_MS of it before the follow-up SRST (some
		 * multipliers send no D2H FIS after the COMRESET when their
		 * first port is empty). */
		uint64_t now = hrtimer_now_ns();
		uint32_t left = deadline_ns > now
					? (uint32_t)((deadline_ns - now) / 1000000ULL)
					: 0;

		if (pmp_capable && left > AHCI_PMP_SRST_WAIT_MS)
			left = AHCI_PMP_SRST_WAIT_MS;
		rc = ahci_port_wait_ready(port, left ? left : 1);
	} else {
		rc = ahci_port_wait_ready(port, pmp_capable
							? AHCI_PMP_READY_PEEK_MS
							: AHCI_DEV_READY_MS);
	}
	if (rc == -ENODEV)
		goto link_gone;
	if (rc == 0)
		cls = ahci_port_classify(port);

	if (pmp_capable &&
	    (rc != 0 || cls == ATA_DEV_UNKNOWN || cls == ATA_DEV_NONE)) {
		int rc2 = ahci_port_softreset_common(port, ATA_PMP_CTRL_PORT,
						     &sig, deadline_ns);

		if (rc2 == 0 && sig != AHCI_PxSIG_INVALID) {
			cls = ata_class_from_sig(sig);
			link->sig = sig;
			rc = 0;
			port_dbg(port, "follow-up SRST: %08x -> %s", sig,
				 ata_class_name(cls));
		} else if (rc2 == -ENODEV) {
			goto link_gone;
		} else if (rc == 0) {
			rc = rc2 ? rc2 : -EIO;
		}
	}
	if (rc == -ETIMEDOUT && !deadline_ns) {
		tfd = ahci_port_readl(port, AHCI_PxTFD);
		if ((tfd & AHCI_PxTFD_STS_BSY) &&
		    !(tfd & AHCI_PxTFD_STS_DRQ)) {
			/* BSY alone: a rotating drive still spinning up.  ATA
			 * gives it 31 s from power-on. */
			port_info(port, "device busy (TFD %08x), waiting for "
					"spin-up",
				  tfd);
			rc = ahci_port_wait_ready(
				port, AHCI_DEV_READY_SLOW_MS -
					      (pmp_capable ? AHCI_PMP_READY_PEEK_MS
							   : AHCI_DEV_READY_MS));
			if (rc == -ENODEV)
				goto link_gone;
			if (rc == 0)
				cls = ahci_port_classify(port);
		}
	}
	if (rc == -ETIMEDOUT && !deadline_ns) {
		/* Still not ready: an SRST has woken devices that missed
		 * their power-on reset. */
		int rc2 = ahci_port_softreset(
			port, pmp_capable ? ATA_PMP_CTRL_PORT : 0, &sig);

		if (rc2 == 0 && sig != AHCI_PxSIG_INVALID) {
			cls = ata_class_from_sig(sig);
			link->sig = sig;
			rc = 0;
		}
	}
	if (rc) {
		tfd = ahci_port_readl(port, AHCI_PxTFD);
		WARN_RATELIMIT(1, "%s: link up but device not ready (TFD "
				  "%08x, %d)",
			       port->name, tfd, rc);
		link->class = ATA_DEV_UNKNOWN;
		if (class)
			*class = ATA_DEV_UNKNOWN;
		return rc;
	}
	if (cls == ATA_DEV_NONE) {
		/* Link up, ready, but a 0 / all-ones signature: something is
		 * there that did not identify itself. */
		cls = ATA_DEV_UNKNOWN;
	}
	link->class = cls;

	ahci_port_set_atapi(port, cls == ATA_DEV_ATAPI);

	/* Leave the port able to execute commands. */
	if (!(ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_ST)) {
		rc = ahci_port_start_engine(port);
		if (rc == -EBUSY) {
			if (ahci_port_clo(port) == 0)
				rc = ahci_port_start_engine(port);
		}
		if (rc) {
			port_warn(port, "engine start after reset failed (%d)",
				  rc);
			if (class)
				*class = cls;
			return rc;
		}
	}
	if (class)
		*class = cls;
	return 0;

link_gone:
	link->class = ATA_DEV_NONE;
	if (class)
		*class = ATA_DEV_NONE;
	return -ENODEV;
}

int ahci_port_hardreset(struct ahci_port *port, enum ata_class *class)
{
	return ahci_port_hardreset_common(port, class, 0, NULL);
}

int ahci_port_hardreset_deadline(struct ahci_port *port,
				 enum ata_class *class, uint32_t timeout_ms,
				 int hotplug)
{
	return ahci_port_hardreset_common(
		port, class,
		hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ULL,
		hotplug ? ahci_deb_timing_hotplug : ahci_deb_timing_normal);
}

static const uint32_t ahci_port_reset_timeouts[] = AHCI_EH_RESET_TIMEOUTS;

int ahci_port_reset_attempts(struct ahci_port *port, enum ata_class *class,
			     int hotplug)
{
	const int tries = (int)(sizeof(ahci_port_reset_timeouts) /
				sizeof(ahci_port_reset_timeouts[0]));
	int rc = -EIO;

	BUG_ON(!port);
	ahci_might_sleep();
	/* Resets within one run of error handling are
	 * AHCI_EH_RESET_COOL_DOWN_MS apart. */
	if (port->eh_run_did_reset) {
		uint64_t until = port->eh_last_reset_ns +
				 (uint64_t)AHCI_EH_RESET_COOL_DOWN_MS * 1000000ULL;
		uint64_t now = hrtimer_now_ns();

		if (now < until)
			ahci_msleep((uint32_t)((until - now + 999999ULL) /
					       1000000ULL));
	}
	for (int try = 0; try < tries; try++) {
		uint64_t deadline, fl;

		/* Within a run of error handling the port is frozen across
		 * each attempt -- the PHY events the reset itself causes are
		 * not hot-plug events -- and thawed once it succeeded, found
		 * the link empty or was the last: the thaw clears what the
		 * reset latched, and the next real PHY event is seen. */
		if (port->eh_running) {
			spin_lock_irqsave(&port->lock, &fl);
			ahci_eh_freeze_locked(port);
			spin_unlock_irqrestore(&port->lock, fl);
		}
		port->eh_last_reset_ns = hrtimer_now_ns();
		port->eh_run_did_reset = 1;
		deadline = port->eh_last_reset_ns +
			   (uint64_t)ahci_port_reset_timeouts[try] * 1000000ULL;
		rc = ahci_port_hardreset_deadline(port, class,
						  ahci_port_reset_timeouts[try],
						  hotplug);
		if (port->eh_running &&
		    (rc == 0 || rc == -ENODEV || try + 1 >= tries)) {
			spin_lock_irqsave(&port->lock, &fl);
			ahci_eh_callee_unmask_locked(port);
			spin_unlock_irqrestore(&port->lock, fl);
		}
		if (rc == 0 || rc == -ENODEV || try + 1 >= tries)
			break;
		/* The link would not settle: a lower speed next time; and
		 * the last attempt at a lower speed in any case. */
		if (rc == -EPIPE || try + 2 == tries)
			(void)ahci_port_down_spd_limit(port);
		{
			uint64_t now = hrtimer_now_ns();

			port_warn(port, "reset failed (%d), retrying in %llu secs",
				  rc,
				  (unsigned long long)(now < deadline
					? (deadline - now + 999999999ULL) /
						  1000000000ULL
					: 0));
			if (now < deadline)
				ahci_msleep((uint32_t)((deadline - now + 999999ULL) /
						       1000000ULL));
		}
	}
	return rc;
}

int ahci_port_down_spd_limit(struct ahci_port *port)
{
	struct ahci_link *link = &port->host_link;
	uint8_t cur;

	cur = link->speed_limit ? link->speed_limit
				: (uint8_t)AHCI_SSTS_SPD(
					  ahci_port_readl(port, AHCI_PxSSTS));
	if (cur <= AHCI_SSTS_SPD_GEN1)
		return -EAGAIN;
	/* Only the limit: SControl.SPD takes it at the next COMRESET,
	 * which is the caller's next reset attempt. */
	link->speed_limit = (uint8_t)(cur - 1);
	link->n_speed_downs++;
	port_info(port, "limiting SATA link speed to %s",
		  ahci_speed_str(link->speed_limit));
	return 0;
}

/* ==========================================================================
 * Port initialisation (AHCI 1.3.1, 10.1.2) with spin-up and power-on
 * ========================================================================== */

/* The PxIE value of this port: the default set (cold presence and the
 * mechanical switch are not among it -- a device change shows as a
 * connect or PhyRdy change); without hot-plug no connect/PhyRdy
 * interrupts (link loss is then found by the command errors it causes);
 * with link power management active no PhyRdy change interrupt either
 * (power state transitions raise it).  A controller that raises IFS for
 * device errors keeps it enabled: the error path ignores IFS there. */
static uint32_t ahci_port_irq_mask_for(const struct ahci_port *port)
{
	uint32_t mask = AHCI_PxIE_DEFAULT;

	if (port->hba->quirks & AHCI_Q_NO_HOTPLUG)
		mask &= ~(AHCI_PxIS_PCS | AHCI_PxIS_PRCS);
	if (port->lpm_policy != AHCI_LPM_MAX_PERFORMANCE &&
	    port->lpm_policy != AHCI_LPM_KEEP_FIRMWARE)
		mask &= ~AHCI_PxIS_PRCS;
	return mask;
}

/* Get both engines idle; when they refuse, a PHY reset (DET 1 -> 0)
 * usually frees them (10.1.2: "a port reset"). */
static int ahci_port_force_idle(struct ahci_port *port)
{
	uint32_t sctl;
	uint64_t fl;
	int rc;

	rc = ahci_port_stop_engine(port);
	if (rc == 0)
		rc = ahci_port_stop_fis_rx(port);
	if (rc == 0)
		return 0;

	port_warn(port, "port does not go idle (PxCMD %08x), resetting the "
			"PHY",
		  ahci_port_readl(port, AHCI_PxCMD));
	spin_lock_irqsave(&port->lock, &fl);
	sctl = ahci_port_readl(port, AHCI_PxSCTL);
	ahci_port_writel(port, AHCI_PxSCTL,
			 (sctl & ~AHCI_SCTL_DET_MASK) | AHCI_SCTL_DET_COMRESET);
	(void)ahci_port_readl(port, AHCI_PxSCTL);
	spin_unlock_irqrestore(&port->lock, fl);
	ahci_msleep(AHCI_COMRESET_ASSERT_MS + 1);
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, AHCI_PxSCTL, sctl & ~AHCI_SCTL_DET_MASK);
	(void)ahci_port_readl(port, AHCI_PxSCTL);
	spin_unlock_irqrestore(&port->lock, fl);

	rc = ahci_port_stop_engine(port);
	if (rc == 0)
		rc = ahci_port_stop_fis_rx(port);
	if (rc)
		port_warn(port, "port still not idle (PxCMD %08x): the "
				"controller needs a reset",
			  ahci_port_readl(port, AHCI_PxCMD));
	return rc;
}

int ahci_port_init(struct ahci_port *port)
{
	struct ahci_hba *hba;
	uint64_t clb, fb, fl;
	uint32_t cmd, sctl, v;
	int rc;

	BUG_ON(!port || !port->hba);
	hba = port->hba;
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	BUG_ON(!port->cmd_list || !port->rx_fis);

	/* 1. Idle: the firmware may have left the port running on its own
	 * command list. */
	rc = ahci_port_force_idle(port);
	if (rc)
		return rc;

	/* 2. FBS and PMA are re-established by the multiplier code after a
	 * probe; DevSleep by the LPM policy.  All three need ST = 0, which
	 * holds now. */
	if (hba->caps.fbss || (hba->quirks & AHCI_Q_YES_FBS)) {
		v = ahci_port_readl(port, AHCI_PxFBS);
		if (v & AHCI_PxFBS_EN) {
			spin_lock_irqsave(&port->lock, &fl);
			ahci_port_writel(port, AHCI_PxFBS,
					 v & ~(AHCI_PxFBS_EN | AHCI_PxFBS_DEC));
			spin_unlock_irqrestore(&port->lock, fl);
		}
	}
	port->fbs_enabled = 0;
	port->fbs_last_dev = 0;
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if ((cmd & AHCI_PxCMD_PMA) && !port->pmp_attached)
		ahci_port_cmd_rmw(port, AHCI_PxCMD_PMA, 0);
	/* Aggressive LPM and DevSleep are re-enabled by the policy once a
	 * device is known; until then the link stays active.  Under
	 * keep_firmware_settings the firmware's choice stays. */
	if (port->lpm_policy != AHCI_LPM_KEEP_FIRMWARE) {
		if (port->devslp_capable) {
			v = ahci_port_readl(port, AHCI_PxDEVSLP);
			if (v & AHCI_DEVSLP_ADSE) {
				spin_lock_irqsave(&port->lock, &fl);
				ahci_port_writel(port, AHCI_PxDEVSLP,
						 v & ~AHCI_DEVSLP_ADSE);
				spin_unlock_irqrestore(&port->lock, fl);
			}
		}
		ahci_port_cmd_rmw(port,
				  AHCI_PxCMD_ALPE | AHCI_PxCMD_ASP |
					  AHCI_PxCMD_APSTE,
				  0);
	}

	/* 3. Command list and received-FIS area. */
	clb = port->cmd_list_area.bus;
	fb = port->rx_fis_area.bus;
	if (WARN_ON(clb & (AHCI_CMD_LIST_ALIGN - 1)) ||
	    WARN_ON(fb & (AHCI_RX_FIS_ALIGN - 1)) ||
	    WARN_ON(port->fbs_capable && port->rx_fis_area.size >=
						 AHCI_RX_FIS_FBS_SIZE &&
		    (fb & (AHCI_RX_FIS_FBS_ALIGN - 1))))
		return -EINVAL;
	if (!hba->dma64 && ((clb >> 32) || (fb >> 32))) {
		port_warn(port, "DMA areas above 4 GiB on a 32-bit HBA "
				"(CLB %llx FB %llx)",
			  (unsigned long long)clb, (unsigned long long)fb);
		return -EINVAL;
	}
	kmemset(port->rx_fis, 0, port->rx_fis_area.size);
	ahci_mb();
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, AHCI_PxCLB, (uint32_t)clb);
	ahci_port_writel(port, AHCI_PxFB, (uint32_t)fb);
	if (hba->caps.s64a) {
		ahci_port_writel(port, AHCI_PxCLBU, (uint32_t)(clb >> 32));
		ahci_port_writel(port, AHCI_PxFBU, (uint32_t)(fb >> 32));
	}
	ahci_port_flush(port);
	spin_unlock_irqrestore(&port->lock, fl);
	if (ahci_port_readl(port, AHCI_PxCLB) != (uint32_t)clb ||
	    ahci_port_readl(port, AHCI_PxFB) != (uint32_t)fb) {
		port_warn(port, "CLB/FB did not take (CLB %08x FB %08x, want "
				"%08x %08x)",
			  ahci_port_readl(port, AHCI_PxCLB),
			  ahci_port_readl(port, AHCI_PxFB), (uint32_t)clb,
			  (uint32_t)fb);
		return -EIO;
	}

	/* 4. Whatever the firmware or a previous life left latched. */
	ahci_port_clear_status(port);

	/* 5. PHY online with our speed and IPM fields (DET = 0: a firmware
	 * that took the PHY offline gets it back; SPD applies at the next
	 * COMRESET). */
	sctl = ahci_port_readl(port, AHCI_PxSCTL);
	v = ahci_link_sctl_value(&port->host_link, sctl, AHCI_SCTL_DET_NONE);
	if (v != sctl) {
		spin_lock_irqsave(&port->lock, &fl);
		ahci_port_writel(port, AHCI_PxSCTL, v);
		(void)ahci_port_readl(port, AHCI_PxSCTL);
		spin_unlock_irqrestore(&port->lock, fl);
		port_dbg(port, "SControl %X -> %X", sctl, v);
	}
	port->host_link.scontrol = v;
	if (port->host_link.state == AHCI_LINK_DISABLED)
		port->host_link.state = AHCI_LINK_OFFLINE;

	/* 6. Power and spin-up (10.10.1): a cold-presence port gets its
	 * device power switched on first (POD), then a staggered spin-up
	 * HBA is told to spin the device up (SUD) -- the HBA then starts
	 * the OOB sequence.  The caller spaces SUD between ports.  Without
	 * CAP.SSS, SUD is read-only 1 and the write is harmless. */
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if ((port->flags & AHCI_PORT_F_CPD) && !(cmd & AHCI_PxCMD_POD)) {
		ahci_port_cmd_rmw(port, 0, AHCI_PxCMD_POD);
		port_dbg(port, "device power on (POD)");
	}
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if (!(cmd & AHCI_PxCMD_SUD)) {
		ahci_port_cmd_rmw(port, 0, AHCI_PxCMD_SUD);
		if (hba->caps.sss && !(hba->quirks & AHCI_Q_NO_SSS))
			port_dbg(port, "staggered spin-up (SUD)");
	}
	port->flags |= AHCI_PORT_F_SPUN_UP;

	/* 7. FIS receive on (needed for the signature of the probe). */
	rc = ahci_port_start_fis_rx(port);
	if (rc && rc != -ETIMEDOUT)
		return rc;

	/* 8. Wake the link if the firmware left it in a low power state.
	 * ICC is only written while it reads 0 (no request pending). */
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	if (!(cmd & AHCI_PxCMD_ICC_MASK)) {
		uint32_t ssts = ahci_port_readl(port, AHCI_PxSSTS);

		if (AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_PHY &&
		    AHCI_SSTS_IPM(ssts) != AHCI_SSTS_IPM_ACTIVE)
			ahci_port_cmd_rmw(port, 0, AHCI_PxCMD_ICC_ACTIVE);
	}

	/* 9. Interrupt sources of the port (GHC.IE is the HBA's). */
	spin_lock_irqsave(&port->lock, &fl);
	port->irq_mask = ahci_port_irq_mask_for(port);
	ahci_port_irq_enable_locked(port, port->irq_mask);
	port->polled = hba->irq_mode == AHCI_IRQ_POLL;
	if (port->state != AHCI_PORT_SUSPENDED)
		port->state = AHCI_PORT_IDLE;
	spin_unlock_irqrestore(&port->lock, fl);

	{
		char cbuf[128];

		cmd = ahci_port_readl(port, AHCI_PxCMD);
		port_dbg(port, "initialised: CMD %08x [%s] SSTS %X SCTL %X IE "
			       "%08x",
			 cmd, ahci_port_cmd_text(cmd, cbuf, sizeof(cbuf)),
			 ahci_port_readl(port, AHCI_PxSSTS),
			 ahci_port_readl(port, AHCI_PxSCTL), port->irq_mask);
		(void)cbuf;
	}
	return 0;
}

/* ==========================================================================
 * Quiescing a port
 * ========================================================================== */

static struct ahci_link *ahci_port_link_by_bit(struct ahci_port *port,
					       int bit)
{
	if (bit == 0)
		return &port->host_link;
	if (bit >= 1 && bit <= AHCI_PMP_MAX_LINKS)
		return &port->pmp_links[bit - 1];
	return NULL;
}

static struct blk_queue *ahci_link_queue(const struct ahci_link *link)
{
	struct ata_device *dev = link->dev;

	if (!dev || !dev->disk)
		return NULL;
	return dev->disk->queue;
}

/* Wait until the HBA has nothing outstanding: PxCI and PxSACT clear (a
 * stopped engine clears both).  Commands the HBA finished but the
 * completion path has not collected yet do not count -- the HBA is done
 * with them. */
static int ahci_port_wait_hw_idle(struct ahci_port *port, uint32_t timeout_ms)
{
	uint64_t waited_us = 0, limit_us = (uint64_t)timeout_ms * 1000ULL;

	for (;;) {
		uint32_t cmd = ahci_port_readl(port, AHCI_PxCMD);

		if (!(cmd & (AHCI_PxCMD_ST | AHCI_PxCMD_CR)))
			return 0;
		if ((ahci_port_readl(port, AHCI_PxCI) |
		     ahci_port_readl(port, AHCI_PxSACT)) == 0)
			return 0;
		if (waited_us >= limit_us)
			return -ETIMEDOUT;
		if (waited_us < 1000) {
			ahci_udelay(AHCI_FAST_POLL_US);
			waited_us += AHCI_FAST_POLL_US;
		} else {
			ahci_msleep(1);
			waited_us += 1000;
		}
	}
}

int ahci_port_quiesce(struct ahci_port *port, uint32_t timeout_ms,
		      uint32_t *token)
{
	enum ahci_port_state st;
	uint32_t mask = 0;
	uint64_t fl;
	int rc = 0;

	BUG_ON(!port || !token);
	ahci_might_sleep();
	ahci_assert_eh_held(port);

	/* No new block requests reach the driver ... */
	for (int bit = 0; bit <= AHCI_PMP_MAX_LINKS; bit++) {
		struct ahci_link *link = ahci_port_link_by_bit(port, bit);
		struct blk_queue *q;

		if (!link || (bit > 0 && (uint32_t)bit > port->nr_pmp_links))
			continue;
		q = ahci_link_queue(link);
		if (!q)
			continue;
		if (__atomic_load_n(&q->flags, __ATOMIC_ACQUIRE) &
		    BLK_QUEUE_FL_STOPPED)
			continue; /* stopped by someone else: theirs */
		blk_queue_stop(q);
		mask |= 1u << bit;
	}
	/* ... and no other non-EH command is accepted by the issue path. */
	spin_lock_irqsave(&port->lock, &fl);
	st = port->state;
	if (st == AHCI_PORT_RUNNING) {
		port->state = AHCI_PORT_EH;
		mask |= AHCI_QUIESCE_STATE;
	}
	spin_unlock_irqrestore(&port->lock, fl);
	*token = mask;
	if (timeout_ms == 0)
		return 0;

	if (st == AHCI_PORT_RUNNING) {
		/* Normal operation: let the outstanding commands finish and
		 * be completed. */
		rc = ahci_port_drain(port, timeout_ms);
		if (rc == 0)
			rc = ahci_port_wait_hw_idle(port, AHCI_ENGINE_STOP_MS);
		if (rc)
			port_warn(port, "commands still active after %u ms (CI "
					"%08x SACT %08x)",
				  timeout_ms, ahci_port_readl(port, AHCI_PxCI),
				  ahci_port_readl(port, AHCI_PxSACT));
		return rc;
	}
	/* Error handling owns the port (FROZEN / EH) or nothing runs: the
	 * commands it holds are its business and must not be waited for in
	 * software or aborted here; only the hardware has to be idle. */
	rc = ahci_port_wait_hw_idle(port, timeout_ms);
	if (rc) {
		port_dbg(port, "hardware busy in state %d (CI %08x SACT %08x)",
			 (int)st, ahci_port_readl(port, AHCI_PxCI),
			 ahci_port_readl(port, AHCI_PxSACT));
		return -EBUSY;
	}
	return 0;
}

void ahci_port_unquiesce(struct ahci_port *port, uint32_t token)
{
	uint64_t fl;

	BUG_ON(!port);
	if (token & AHCI_QUIESCE_STATE) {
		spin_lock_irqsave(&port->lock, &fl);
		/* Only undo our own change: an error that froze the port in
		 * the meantime keeps it frozen for the error handler. */
		if (port->state == AHCI_PORT_EH)
			port->state = AHCI_PORT_RUNNING;
		spin_unlock_irqrestore(&port->lock, fl);
	}
	for (int bit = 0; bit <= AHCI_PMP_MAX_LINKS; bit++) {
		struct ahci_link *link;
		struct blk_queue *q;

		if (!(token & (1u << bit)))
			continue;
		link = ahci_port_link_by_bit(port, bit);
		if (!link)
			continue;
		q = ahci_link_queue(link);
		if (q)
			blk_queue_start(q);
	}
}

/* ==========================================================================
 * Attaching and detaching devices
 * ========================================================================== */

static void ahci_link_set_host_limits(struct ahci_link *link,
				      struct ata_device *dev)
{
	struct ahci_port *port = link->port;
	int ncq = port->ncq_depth > 0 && !(port->flags & AHCI_PORT_F_NO_NCQ);

	dev->host_max_segments = AHCI_MAX_PRD;
	dev->host_max_sectors = AHCI_MAX_SECTORS;
	dev->host_dma_alignment = AHCI_PRD_ALIGN - 1;
	dev->host_ncq = ncq;
	dev->host_queue_depth = ncq ? port->ncq_depth : 0;
	dev->host_fua = 1;
}

/* Make sure the issue path accepts the probe's commands: after a reset in
 * error handling the port may still be frozen (interrupts masked), and
 * at boot it is IDLE.  Inside an EH run the probe's commands are EH's
 * own: interrupts go on, but the port stays in error handling until the
 * run ends (ordinary commands wait for that). */
static void ahci_port_make_runnable(struct ahci_port *port)
{
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	switch (port->state) {
	case AHCI_PORT_FROZEN:
	case AHCI_PORT_EH:
		if (port->eh_running)
			ahci_eh_callee_unmask_locked(port);
		else
			ahci_eh_thaw_locked(port);
		break;
	case AHCI_PORT_UNUSED:
	case AHCI_PORT_IDLE:
	case AHCI_PORT_DISABLED:
		if (port->eh_running) {
			/* emptied earlier in this run (a hot removal) */
			port->state = AHCI_PORT_EH;
			ahci_eh_callee_unmask_locked(port);
		} else {
			port->state = AHCI_PORT_RUNNING;
		}
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&port->lock, fl);
}

int ahci_link_attach(struct ahci_link *link, enum ata_class class)
{
	struct ahci_port *port;
	struct ata_device *dev;
	uint64_t fl;
	int rc;

	BUG_ON(!link || !link->port);
	port = link->port;
	ahci_might_sleep();
	ahci_assert_eh_held(port);

	switch (class) {
	case ATA_DEV_ATA:
	case ATA_DEV_ATAPI:
	case ATA_DEV_ZAC:
		break;
	default:
		port_warn(port, "%s: cannot attach a device of class %s",
			  link->name, ata_class_name(class));
		return -EINVAL;
	}
	if (WARN_ON(link->dev != NULL))
		return -EEXIST;

	dev = ata_dev_alloc(&ahci_ata_transport, link, port->port_no,
			    link->is_host ? 0 : link->pmp);
	if (!dev) {
		port_warn(port, "%s: out of memory for the device",
			  link->name);
		return -ENOMEM;
	}
	ahci_link_set_host_limits(link, dev);

	spin_lock_irqsave(&port->lock, &fl);
	link->dev = dev;
	link->class = class;
	spin_unlock_irqrestore(&port->lock, fl);

	ahci_port_make_runnable(port);

	rc = ata_dev_probe(dev, class);
	if (rc) {
		port_warn(port, "%s: device probe failed (%d)", link->name,
			  rc);
		if (dev->flags & ATA_DFLAG_REGISTERED)
			ata_dev_detach(dev);
		spin_lock_irqsave(&port->lock, &fl);
		link->dev = NULL;
		spin_unlock_irqrestore(&port->lock, fl);
		ahci_sysfs_remove_device(dev);
		ata_dev_put(dev);
		return rc;
	}

	/* The link's power policy now that the device's abilities are
	 * known (DIPM, DevSleep, HIPM support). */
	if ((link->is_host ? port->lpm_policy : link->lpm_policy) !=
	    AHCI_LPM_KEEP_FIRMWARE)
		(void)ahci_port_set_lpm(port, link,
					link->is_host ? port->lpm_policy
						      : link->lpm_policy);
	return 0;
}

void ahci_link_detach(struct ahci_link *link)
{
	struct ahci_port *port;
	struct ata_device *dev;
	uint64_t fl;

	if (!link || !link->dev)
		return;
	port = link->port;
	ahci_might_sleep();

	dev = link->dev;
	ata_dev_detach(dev);
	spin_lock_irqsave(&port->lock, &fl);
	link->dev = NULL;
	link->class = ATA_DEV_NONE;
	spin_unlock_irqrestore(&port->lock, fl);
	/* The device's sysfs entries go before its last reference. */
	ahci_sysfs_remove_device(dev);
	ata_dev_put(dev);
}

/* ==========================================================================
 * Probe
 * ========================================================================== */

/* A port multiplier on an HBA that cannot address one: multipliers pass
 * commands without a PM port field to their port 0, so the device there
 * is usable on its own. */
static int ahci_port_probe_pmp_fallback(struct ahci_port *port)
{
	enum ata_class cls;
	uint32_t sig = AHCI_PxSIG_INVALID;
	int rc;

	port_info(port, "port multiplier found but the HBA does not support "
			"them: using its port 0 only");
	rc = ahci_port_softreset(port, 0, &sig);
	if (rc)
		return rc;
	cls = ata_class_from_sig(sig);
	if (cls == ATA_DEV_PMP || cls == ATA_DEV_NONE ||
	    cls == ATA_DEV_SEMB) {
		port_info(port, "nothing usable behind the port multiplier "
				"(signature %08x)",
			  sig);
		return 0;
	}
	if (cls == ATA_DEV_UNKNOWN)
		cls = ATA_DEV_ATA;
	return ahci_link_attach(&port->host_link, cls);
}

static int ahci_port_probe_finish(struct ahci_port *port, int rc,
				  enum ata_class cls);

int ahci_port_probe(struct ahci_port *port)
{
	enum ata_class cls = ATA_DEV_NONE;
	int rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_assert_eh_held(port);

	if (port->pmp_attached) {
		/* Probing again a port that has a multiplier: recover it and
		 * its links (with their tries; after a hot-plug event its
		 * host link is reset first); a multiplier that cannot be
		 * recovered is detached and the port probed from scratch
		 * below. */
		rc = ahci_pmp_recover(port, port->eh_hotplugged);
		if (rc == 0)
			return 0;
	}

	/* Once the clock runs (after the polled boot probe) the reset is
	 * error handling's: its attempts with their deadlines and debounce
	 * timings.  The boot probe counts its waits in delays. */
	if (port->hba->kahci_running)
		rc = ahci_port_reset_attempts(port, &cls, port->eh_hotplugged);
	else
		rc = ahci_port_hardreset(port, &cls);
	return ahci_port_probe_finish(port, rc, cls);
}

/* The boot probe of several ports at once (polled, before kahci): the
 * COMRESET asserted on every port, held, released on every port, and
 * all links debounced in one loop -- each with its own timing, samples
 * and counted time, exactly as one port alone -- then each COMRESET
 * finished.  Only the waiting overlaps.  rcs[i]: what ports[i]'s
 * COMRESET returned, for ahci_port_probe_after_comreset().  Every
 * port's eh_mutex held. */
void ahci_ports_comreset_together(struct ahci_port *const *ports, int n,
				  int *rcs)
{
	struct ahci_deb deb[AHCI_MAX_PORTS];
	uint32_t sctl_old[AHCI_MAX_PORTS], sctl[AHCI_MAX_PORTS];
	uint32_t due[AHCI_MAX_PORTS], elapsed = 0;
	uint8_t active[AHCI_MAX_PORTS];

	BUG_ON(n < 0 || n > AHCI_MAX_PORTS);
	if (n == 0)
		return;
	for (int i = 0; i < n; i++)
		sctl_old[i] = ahci_port_comreset_assert(ports[i], NULL,
							&sctl[i]);
	ahci_msleep(AHCI_COMRESET_HOLD_MS);
	for (int i = 0; i < n; i++)
		sctl[i] = ahci_port_comreset_release(ports[i], sctl_old[i],
						     sctl[i]);
	/* Wait for the links: DET settles to 3 (up) or 0 (empty).  A link
	 * whose first SStatus read fails is finished with SStatus 0, as
	 * alone. */
	for (int i = 0; i < n; i++) {
		const uint32_t *timing =
			ahci_deb_timing(ports[i]->eh_hotplugged);

		active[i] = ahci_link_deb_begin(&ports[i]->host_link, &deb[i],
						timing, AHCI_NO_DEADLINE) == 0;
		due[i] = timing[0];
	}
	for (;;) {
		uint32_t next = 0;
		int any = 0;

		for (int i = 0; i < n; i++)
			if (active[i] && (!any || due[i] < next)) {
				next = due[i];
				any = 1;
			}
		if (!any)
			break;
		ahci_msleep(next - elapsed);
		elapsed = next;
		for (int i = 0; i < n; i++) {
			if (!active[i] || due[i] != elapsed)
				continue;
			/* the DET value decides, settled or not */
			if (ahci_link_deb_step(&ports[i]->host_link,
					       &deb[i]) == 1)
				due[i] += deb[i].timing[0];
			else
				active[i] = 0;
		}
	}
	for (int i = 0; i < n; i++)
		rcs[i] = ahci_port_comreset_finish(ports[i], sctl[i],
						   deb[i].sstatus);
}

int ahci_port_probe_after_comreset(struct ahci_port *port, int rc)
{
	enum ata_class cls = ATA_DEV_NONE;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	rc = ahci_port_hardreset_rest(port, &cls, 0, rc);
	return ahci_port_probe_finish(port, rc, cls);
}

/* What a reset of the host link found, acted on: an empty link detaches
 * what was there, a multiplier is attached, a device present before is
 * revalidated (and replaced when it is not the same any more), a new one
 * attached.  `rc', `cls': the reset's result. */
static int ahci_port_probe_finish(struct ahci_port *port, int rc,
				  enum ata_class cls)
{
	struct ahci_link *link = &port->host_link;
	uint64_t fl;

	if (rc == -ENODEV || cls == ATA_DEV_NONE) {
		/* Empty port: no engine, link down, waiting for hot-plug. */
		if (link->dev) {
			(void)ahci_port_stop_engine_abort(port);
			ahci_port_fail_all(port, -ENODEV);
			ahci_link_detach(link);
		}
		(void)ahci_port_stop_engine(port);
		spin_lock_irqsave(&port->lock, &fl);
		if (port->state != AHCI_PORT_SUSPENDED)
			port->state = AHCI_PORT_IDLE;
		spin_unlock_irqrestore(&port->lock, fl);
		link->class = ATA_DEV_NONE;
		return 0;
	}
	if (rc) {
		port_warn(port, "probe failed: device not ready (%d)", rc);
		return rc;
	}

	switch (cls) {
	case ATA_DEV_PMP:
		if (link->dev) {
			/* A disk replaced by a multiplier. */
			(void)ahci_port_stop_engine_abort(port);
			ahci_port_fail_all(port, -ENODEV);
			ahci_link_detach(link);
			(void)ahci_port_start_engine(port);
		}
		if (ahci_port_pmp_capable(port))
			return ahci_pmp_attach(port);
		return ahci_port_probe_pmp_fallback(port);

	case ATA_DEV_SEMB:
		port_info(port, "SATA enclosure management bridge (signature "
				"%08x): not used",
			  link->sig);
		return 0;

	case ATA_DEV_UNKNOWN:
		port_info(port, "unknown device signature %08x, assuming ATA",
			  link->sig);
		cls = ATA_DEV_ATA;
		break;

	default:
		break;
	}

	if (link->dev) {
		/* A device was there before this probe (error handling or
		 * resume asked for it): same device -> revalidate. */
		rc = ata_dev_revalidate(link->dev);
		if (rc == 0) {
			ahci_port_make_runnable(port);
			return 0;
		}
		if (rc != -ENODEV) {
			/* Not known to be another device (IDENTIFY failed,
			 * e.g. refused while a new PHY event froze the
			 * port): it is kept, with its disk, and error
			 * handling resets and revalidates it again -- until
			 * its tries are used up. */
			port_info(port, "revalidation failed (%d), resetting "
					"again",
				  rc);
			ahci_eh_schedule(port, AHCI_EH_RESET_HARD);
			return rc;
		}
		port_info(port, "device changed or gone (%d), replacing it",
			  rc);
		(void)ahci_port_stop_engine_abort(port);
		ahci_port_fail_all(port, -ENODEV);
		ahci_link_detach(link);
		(void)ahci_port_start_engine(port);
	}
	return ahci_link_attach(link, cls);
}

/* ==========================================================================
 * Hot-plug (AHCI 1.3.1, 7.3 and 10.9)
 * ========================================================================== */

/* The device on the host link is gone: stop the engine (the HBA must not
 * touch the buffers of the commands we are about to fail), fail every
 * outstanding command with -ENODEV, unregister the device. */
static void ahci_port_remove_device(struct ahci_port *port,
				    const char *why)
{
	struct ahci_link *link = &port->host_link;
	uint64_t fl;

	port_info(port, "device removed (%s)", why);
	port->stats.hotplug_remove++;
	(void)ahci_port_stop_engine_abort(port);
	ahci_port_fail_all(port, -ENODEV);
	if (port->pmp_attached)
		ahci_pmp_detach(port);
	ahci_link_detach(link);
	spin_lock_irqsave(&port->lock, &fl);
	if (port->state != AHCI_PORT_SUSPENDED)
		port->state = AHCI_PORT_IDLE;
	spin_unlock_irqrestore(&port->lock, fl);
	link->class = ATA_DEV_NONE;
	link->state = AHCI_LINK_OFFLINE;
}

int ahci_port_hotplug(struct ahci_port *port, int did_reset)
{
	struct ahci_link *link;
	uint32_t token = 0;
	uint64_t fl;
	char sbuf[160];
	int had_dev, rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	link = &port->host_link;

	/* The event's status was taken by the interrupt handler (PxIS
	 * acknowledged, SError cleared and latched): drop the latched
	 * hot-plug bits, the reset below is the event's answer. */
	port_dbg(port, "hot-plug event: SError %08x [%s]", port->eh_serror,
		 ahci_port_serr_text(port->eh_serror, sbuf, sizeof(sbuf)));
	(void)sbuf;
	spin_lock_irqsave(&port->lock, &fl);
	port->eh_irq_stat &= ~AHCI_PxIS_HOTPLUG;
	port->eh_serror &= ~AHCI_SERR_HOTPLUG;
	spin_unlock_irqrestore(&port->lock, fl);
	port->hotplug_ns = ahci_now_ns();
	had_dev = link->dev != NULL || port->pmp_attached;

	/* No debouncing of its own: the link is reset with the hot-plug
	 * debounce timing -- unless this round's recovery reset it already
	 * (with that timing) -- and what the reset found decides: attach,
	 * detach or revalidate. */
	port->eh_hotplugged = 1;
	(void)ahci_port_quiesce(port, 0, &token);
	if (did_reset && !port->pmp_attached)
		rc = ahci_port_probe_finish(port,
					    link->state == AHCI_LINK_ONLINE
						    ? 0
						    : -ENODEV,
					    link->class);
	else
		rc = ahci_port_probe(port);
	ahci_port_unquiesce(port, token);
	port->eh_hotplugged = 0;

	if (!had_dev && (link->dev || port->pmp_attached)) {
		port_info(port, "device inserted (SStatus %X)", link->sstatus);
		port->stats.hotplug_add++;
	} else if (had_dev && !link->dev && !port->pmp_attached) {
		port_info(port, "device removed (SStatus %X)", link->sstatus);
		port->stats.hotplug_remove++;
	}
	return rc;
}

/* ==========================================================================
 * SError classification
 * ========================================================================== */

uint32_t ahci_serror_classify(struct ahci_port *port, uint32_t serr)
{
	struct ahci_port_stats *st;
	struct ahci_link *link;
	uint32_t actions = 0;
	int link_error = 0;

	if (!port || !serr)
		return 0;
	st = &port->stats;
	link = &port->host_link;

	if (port->hba->quirks & AHCI_Q_IGN_SERR_INTERNAL)
		serr &= ~AHCI_SERR_ERR_E;

	/* Diagnostics bits (31:16). */
	if (serr & AHCI_SERR_DIAG_C) {
		__atomic_fetch_add(&st->serr_crc, 1, __ATOMIC_RELAXED);
		link_error = 1;
	}
	if (serr & AHCI_SERR_DIAG_D) {
		__atomic_fetch_add(&st->serr_disparity, 1, __ATOMIC_RELAXED);
		link_error = 1;
	}
	if (serr & AHCI_SERR_DIAG_B) {
		__atomic_fetch_add(&st->serr_decode, 1, __ATOMIC_RELAXED);
		link_error = 1;
	}
	if (serr & AHCI_SERR_DIAG_H) {
		__atomic_fetch_add(&st->serr_handshake, 1, __ATOMIC_RELAXED);
		link_error = 1;
	}
	if (serr & AHCI_SERR_DIAG_N)
		__atomic_fetch_add(&st->serr_phyrdy, 1, __ATOMIC_RELAXED);
	if (serr & AHCI_SERR_DIAG_W)
		__atomic_fetch_add(&st->serr_comwake, 1, __ATOMIC_RELAXED);
	if (serr & (AHCI_SERR_DIAG_I | AHCI_SERR_ERR_E))
		__atomic_fetch_add(&st->serr_internal, 1, __ATOMIC_RELAXED);
	if (serr & AHCI_SERR_ERR_P)
		__atomic_fetch_add(&st->serr_protocol, 1, __ATOMIC_RELAXED);

	/* Hot-plug: device exchanged (COMINIT) or PhyRdy changed. */
	if (serr & AHCI_SERR_HOTPLUG)
		actions |= AHCI_EH_HOTPLUG;
	/* An unknown FIS type arrived. */
	if (serr & AHCI_SERR_DIAG_F)
		actions |= AHCI_EH_UNKNOWN_FIS;
	/* Transport state / link sequence errors, protocol violations, a
	 * PHY internal error, a persistent communication error and an
	 * internal HBA error all leave the link in an unknown state: the
	 * link is reset. */
	if (serr & (AHCI_SERR_DIAG_T | AHCI_SERR_DIAG_S | AHCI_SERR_DIAG_I |
		    AHCI_SERR_ERR_P | AHCI_SERR_ERR_C | AHCI_SERR_ERR_E)) {
		actions |= AHCI_EH_RESET_HARD;
		link_error = 1;
	}
	/* A transient data integrity error: the command is retried. */
	if (serr & AHCI_SERR_ERR_T) {
		actions |= AHCI_EH_IF_NONFATAL;
		link_error = 1;
	}
	/* CRC / disparity / decode / handshake errors that did not already
	 * ask for more: retry; repeated ones escalate (the speed step-down
	 * is applied by the next COMRESET). */
	if (link_error && !(actions & AHCI_EH_RESET_HARD))
		actions |= AHCI_EH_IF_NONFATAL;
	if (link_error) {
		ahci_link_note_error(link);
		if (link->spd_err_count >= AHCI_SPEED_DOWN_ERRS)
			actions |= AHCI_EH_RESET_HARD;
	}
	/* M (recovered communication) and I (recovered data integrity)
	 * need nothing; they are visible in the SError dump. */
	return actions;
}

/* ==========================================================================
 * Link power management
 * ========================================================================== */

/* Request an interface state through PxCMD.ICC and wait for SStatus.IPM
 * to show it (want_ipm 0: do not wait). */
static int ahci_port_set_icc(struct ahci_port *port, uint32_t icc,
			     uint32_t want_ipm)
{
	uint32_t last = 0;
	uint64_t fl;
	int rc;

	/* The previous request must have been taken (ICC reads 0). */
	rc = ahci_port_wait_bits(port, AHCI_PxCMD, AHCI_PxCMD_ICC_MASK, 0,
				 AHCI_ICC_TIMEOUT_MS, &last);
	if (rc) {
		port_dbg(port, "ICC busy (PxCMD %08x)", last);
		return rc;
	}
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_cmd_rmw_locked(port, 0, icc);
	spin_unlock_irqrestore(&port->lock, fl);
	if (!want_ipm)
		return 0;
	rc = ahci_port_wait_bits(port, AHCI_PxSSTS, AHCI_SSTS_IPM_MASK,
				 want_ipm << AHCI_SSTS_IPM_SHIFT,
				 AHCI_ICC_TIMEOUT_MS, &last);
	if (rc)
		port_dbg(port, "interface did not reach %s (SStatus %X)",
			 ahci_ipm_name(want_ipm), last);
	return rc;
}

/* Wake the host link to the active state if it is in a low-power one. */
static void ahci_port_wake_link(struct ahci_port *port)
{
	uint32_t ssts = ahci_port_readl(port, AHCI_PxSSTS);
	uint32_t ipm = AHCI_SSTS_IPM(ssts);

	if (AHCI_SSTS_DET(ssts) != AHCI_SSTS_DET_PHY)
		return;
	if (ipm == AHCI_SSTS_IPM_ACTIVE || ipm == AHCI_SSTS_IPM_NONE)
		return;
	(void)ahci_port_set_icc(port, AHCI_PxCMD_ICC_ACTIVE,
				AHCI_SSTS_IPM_ACTIVE);
}

/* SET FEATURES enable/disable SATA feature on a device.  In normal
 * operation through the ATA layer's helper; while the port is in error
 * handling (only EH commands are accepted) as an EH command; on a frozen
 * port (interrupts masked) not at all -- the device configuration after
 * the coming thaw/revalidate applies the policy then. */
static int ahci_dev_sata_feature(struct ahci_port *port,
				 struct ata_device *dev, uint8_t feature,
				 int on)
{
	struct ata_taskfile tf;
	enum ahci_port_state st;
	uint64_t fl;
	int rc;

	spin_lock_irqsave(&port->lock, &fl);
	st = port->state;
	spin_unlock_irqrestore(&port->lock, fl);

	if (st == AHCI_PORT_FROZEN)
		return -EAGAIN;
	if (st == AHCI_PORT_RUNNING)
		return ata_set_sata_feature(dev, feature, on);

	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_SET_FEATURES;
	tf.feature = on ? SETFEATURES_SATA_ENABLE : SETFEATURES_SATA_DISABLE;
	tf.nsect = feature;
	tf.flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	tf.device = 0;
	rc = ata_exec_internal(dev, &tf, NULL, ATA_PROT_NODATA, NULL, 0,
			       ATA_DIR_NONE, ATA_TMOUT_INTERNAL_MS,
			       ATA_QCFLAG_EH);
	return rc;
}

/* Is DevSleep enabled on the device (IDENTIFY word 79 bit 8)?  Only then
 * does a policy without DevSleep need a SET FEATURES to turn it off. */
static int ahci_dev_devslp_enabled(const struct ata_device *dev)
{
	uint16_t w = dev->id[ATA_ID_FEATURE_ENABLED];

	return w != 0xFFFF && (w & ATA_ID_FEAT_DEVSLP) != 0;
}

/* DIPM on/off for a device (SATA feature 03h). */
static int ahci_dev_set_dipm(struct ahci_port *port, struct ata_device *dev,
			     int on)
{
	int rc;

	if (!dev || !(dev->flags & ATA_DFLAG_DIPM))
		return on ? -EOPNOTSUPP : 0;
	if (!!(dev->flags & ATA_DFLAG_DIPM_ON) == !!on)
		return 0;
	rc = ahci_dev_sata_feature(port, dev, SATA_FEAT_DIPM, on);
	if (rc == 0) {
		if (on)
			dev->flags |= ATA_DFLAG_DIPM_ON;
		else
			dev->flags &= ~ATA_DFLAG_DIPM_ON;
	} else if (rc != -EAGAIN) {
		port_warn(port, "%s: DIPM %s failed (%d)", dev->name,
			  on ? "enable" : "disable", rc);
	}
	return rc;
}

/* Program PxDEVSLP (aggressive device sleep with idle timeout).  PxDEVSLP
 * is written with the command engine stopped, so the port is quiesced
 * around it when it runs. */
static int ahci_port_devslp_program(struct ahci_port *port, int enable)
{
	struct ahci_hba *hba = port->hba;
	uint32_t cur, v, dm, dito, mdat, deto, token = 0;
	int was_running, rc = 0;
	uint64_t fl;

	if (!port->devslp_capable)
		return enable ? -EOPNOTSUPP : 0;
	if (enable && !hba->caps.sadm)
		return -EOPNOTSUPP; /* only software-initiated DevSleep */
	cur = ahci_port_readl(port, AHCI_PxDEVSLP);
	if (!!(cur & AHCI_DEVSLP_ADSE) == !!enable)
		return 0;

	was_running = (ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_ST) != 0;
	if (was_running) {
		rc = ahci_port_quiesce(port, AHCI_QUIESCE_TIMEOUT_MS, &token);
		if (rc) {
			ahci_port_unquiesce(port, token);
			return rc;
		}
		rc = ahci_port_stop_engine(port);
		if (rc) {
			ahci_port_unquiesce(port, token);
			return rc;
		}
	}

	/* DITO is counted in units of (DM + 1) ms. */
	dm = (cur & AHCI_DEVSLP_DM_MASK) >> AHCI_DEVSLP_DM_SHIFT;
	dito = AHCI_DEVSLP_DEF_DITO / (dm + 1);
	if (dito == 0)
		dito = 1;
	if (dito > (AHCI_DEVSLP_DITO_MASK >> AHCI_DEVSLP_DITO_SHIFT))
		dito = AHCI_DEVSLP_DITO_MASK >> AHCI_DEVSLP_DITO_SHIFT;
	mdat = AHCI_DEVSLP_DEF_MDAT;
	if (mdat > (AHCI_DEVSLP_MDAT_MASK >> AHCI_DEVSLP_MDAT_SHIFT))
		mdat = AHCI_DEVSLP_MDAT_MASK >> AHCI_DEVSLP_MDAT_SHIFT;
	deto = AHCI_DEVSLP_DEF_DETO;
	if (deto > (AHCI_DEVSLP_DETO_MASK >> AHCI_DEVSLP_DETO_SHIFT))
		deto = AHCI_DEVSLP_DETO_MASK >> AHCI_DEVSLP_DETO_SHIFT;

	v = cur & ~(AHCI_DEVSLP_DITO_MASK | AHCI_DEVSLP_MDAT_MASK |
		    AHCI_DEVSLP_DETO_MASK | AHCI_DEVSLP_ADSE);
	if (enable)
		v |= (dito << AHCI_DEVSLP_DITO_SHIFT) |
		     (mdat << AHCI_DEVSLP_MDAT_SHIFT) |
		     (deto << AHCI_DEVSLP_DETO_SHIFT) | AHCI_DEVSLP_ADSE;
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, AHCI_PxDEVSLP, v);
	ahci_port_flush(port);
	spin_unlock_irqrestore(&port->lock, fl);
	port->saved_devslp = v;
	port_dbg(port, "PxDEVSLP %08x -> %08x", cur, v);

	if (was_running) {
		rc = ahci_port_start_engine(port);
		ahci_port_unquiesce(port, token);
	}
	return rc;
}

/* A link power management policy was applied to `link': a PhyRdy change
 * in the next AHCI_TMOUT_SPURIOUS_PHY_MS may be the change's own and is
 * ignored by the interrupt handler. */
static void ahci_link_note_lpm_change(struct ahci_port *port,
				      struct ahci_link *link)
{
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	link->last_lpm_change_ns = hrtimer_now_ns();
	link->lpm_changed = 1;
	spin_unlock_irqrestore(&port->lock, fl);
}

/* LPM on a fan-out link: the multiplier's PSCR SControl IPM bits and DIPM
 * on the device.  Host-initiated requests do not exist per fan-out link. */
static int ahci_pmp_link_set_lpm(struct ahci_port *port,
				 struct ahci_link *link,
				 enum ahci_lpm_policy policy)
{
	uint32_t sctl, v;
	int rc, dipm;

	if (policy == AHCI_LPM_KEEP_FIRMWARE) {
		link->lpm_policy = policy;
		return 0;
	}
	if (!(port->pmp_gscr_feat & SATA_PMP_FEAT_PMREQ) &&
	    policy != AHCI_LPM_MAX_PERFORMANCE) {
		port_info(port, "%s: the port multiplier does not support "
				"power management requests, LPM stays off",
			  link->name);
		policy = AHCI_LPM_MAX_PERFORMANCE;
	}
	dipm = policy == AHCI_LPM_MED_POWER_WITH_DIPM ||
	       policy == AHCI_LPM_MIN_POWER;
	if (!dipm && link->dev)
		(void)ahci_dev_set_dipm(port, link->dev, 0);

	rc = ahci_pmp_scr_read(link, AHCI_SCR_CONTROL, &sctl);
	if (rc)
		return rc;
	v = (sctl & ~(AHCI_SCTL_IPM_MASK | AHCI_SCTL_DET_MASK)) |
	    ahci_policy_sctl_ipm(port, link, policy);
	if (v != sctl) {
		rc = ahci_pmp_scr_write(link, AHCI_SCR_CONTROL, v);
		if (rc)
			return rc;
	}
	if (dipm && link->dev)
		(void)ahci_dev_set_dipm(port, link->dev, 1);
	if (policy != link->lpm_policy || policy != AHCI_LPM_DEFAULT_POLICY)
		port_info(port, "%s: LPM policy %s (SControl %X)", link->name,
			  ahci_lpm_policy_name(policy), v);
	link->lpm_policy = policy;
	ahci_link_note_lpm_change(port, link);
	return 0;
}

int ahci_port_set_lpm(struct ahci_port *port, struct ahci_link *link,
		      enum ahci_lpm_policy policy)
{
	struct ahci_hba *hba;
	struct ata_device *dev;
	uint32_t sctl, v, set = 0, clear, mask;
	int hipm = 0, dipm = 0, apst = 0, devslp = 0, rc = 0, ret = 0;
	enum ahci_lpm_policy old_policy;
	uint64_t fl;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_assert_eh_held(port);
	hba = port->hba;
	if ((int)policy < (int)AHCI_LPM_KEEP_FIRMWARE ||
	    policy > AHCI_LPM_MIN_POWER)
		return -EINVAL;
	if (!link)
		link = &port->host_link;
	if (link->port != port)
		return -EINVAL;
	if (!link->is_host)
		return ahci_pmp_link_set_lpm(port, link, policy);
	old_policy = port->lpm_policy;

	if (policy == AHCI_LPM_KEEP_FIRMWARE) {
		/* Leave everything as it is, just remember it. */
		port->lpm_policy = policy;
		link->lpm_policy = policy;
		return 0;
	}
	if ((hba->quirks & AHCI_Q_NO_LPM) &&
	    policy != AHCI_LPM_MAX_PERFORMANCE) {
		port_info(port, "link power management disabled for this "
				"controller, keeping max_performance");
		policy = AHCI_LPM_MAX_PERFORMANCE;
		ret = -EOPNOTSUPP;
	}
	if (port->pmp_attached && policy != AHCI_LPM_MAX_PERFORMANCE &&
	    !(port->pmp_gscr_feat & SATA_PMP_FEAT_PMREQ)) {
		port_info(port, "port multiplier without power management "
				"support: host link LPM stays off");
		policy = AHCI_LPM_MAX_PERFORMANCE;
		ret = -EOPNOTSUPP;
	}
	dev = link->dev;

	/* What the policy asks for, limited by the hardware. */
	switch (policy) {
	case AHCI_LPM_MEDIUM_POWER:
		hipm = 1;
		break;
	case AHCI_LPM_MED_POWER_WITH_DIPM:
		hipm = 1;
		dipm = 1;
		break;
	case AHCI_LPM_MIN_POWER:
		hipm = 1;
		dipm = 1;
		apst = 1;
		devslp = 1;
		break;
	default:
		break;
	}
	if (hipm && !hba->caps.salp)
		hipm = 0;
	if (hipm && dev && !(dev->flags & ATA_DFLAG_HIPM))
		hipm = 0; /* the device would not acknowledge the requests */
	if (hipm && port->hotplug_capable &&
	    (hba->quirks & AHCI_Q_NO_ALPM_ON_HOTPLUG))
		hipm = 0;
	if (dipm && (!dev || !(dev->flags & ATA_DFLAG_DIPM)))
		dipm = 0;
	if (apst && !(hba->caps.apst && hba->caps.psc && hba->caps.ssc))
		apst = 0;
	if (devslp && !(port->devslp_capable && hba->caps.sadm && dev &&
			(dev->flags & ATA_DFLAG_DEVSLP)))
		devslp = 0;

	/* 1. The link must be active while its permissions change. */
	ahci_port_wake_link(port);

	/* 2. Disable first what the new policy does not want, device side
	 * first (it must stop initiating before the host refuses). */
	if (!dipm && dev)
		(void)ahci_dev_set_dipm(port, dev, 0);
	if (!devslp) {
		rc = ahci_port_devslp_program(port, 0);
		if (rc && rc != -EOPNOTSUPP)
			port_warn(port, "DevSleep disable failed (%d)", rc);
		if (dev && ahci_dev_devslp_enabled(dev) &&
		    port->devslp_capable)
			(void)ahci_dev_sata_feature(port, dev,
						    SATA_FEAT_DEVSLP, 0);
	}

	/* 3. SControl.IPM: which states the link may enter. */
	sctl = ahci_port_readl(port, AHCI_PxSCTL);
	/* ahci_link_sctl_value reads port->lpm_policy: give it the new one */
	port->lpm_policy = policy;
	link->lpm_policy = policy;
	v = ahci_link_sctl_value(link, sctl, AHCI_SCTL_DET_NONE);
	if (v != sctl) {
		spin_lock_irqsave(&port->lock, &fl);
		ahci_port_writel(port, AHCI_PxSCTL, v);
		(void)ahci_port_readl(port, AHCI_PxSCTL);
		spin_unlock_irqrestore(&port->lock, fl);
	}
	link->scontrol = v;

	/* 4. HIPM: aggressive link power management to partial (ASP = 0)
	 * or slumber (ASP = 1, preferred when the HBA can slumber), and
	 * automatic partial-to-slumber. */
	clear = AHCI_PxCMD_ALPE | AHCI_PxCMD_ASP | AHCI_PxCMD_APSTE;
	if (hipm) {
		set |= AHCI_PxCMD_ALPE;
		if (hba->caps.ssc &&
		    (policy != AHCI_LPM_MEDIUM_POWER || !hba->caps.psc))
			set |= AHCI_PxCMD_ASP;
	}
	if (apst)
		set |= AHCI_PxCMD_APSTE;
	ahci_port_cmd_rmw(port, clear, set);

	/* 5. Interrupts: no PhyRdy change interrupt while the link power
	 * states are in use (some HBAs raise it on every transition). */
	spin_lock_irqsave(&port->lock, &fl);
	mask = ahci_port_irq_mask_for(port);
	if (mask != port->irq_mask) {
		port->irq_mask = mask;
		if (port->state != AHCI_PORT_FROZEN)
			ahci_port_irq_enable_locked(port, mask);
	}
	spin_unlock_irqrestore(&port->lock, fl);

	/* 6. Enable on the device side what the policy wants. */
	if (devslp) {
		rc = ahci_dev_sata_feature(port, dev, SATA_FEAT_DEVSLP, 1);
		if (rc == 0)
			rc = ahci_port_devslp_program(port, 1);
		if (rc) {
			if (rc != -EAGAIN)
				port_warn(port, "DevSleep enable failed (%d)",
					  rc);
			devslp = 0;
		}
	}
	if (dipm) {
		rc = ahci_dev_set_dipm(port, dev, 1);
		if (rc)
			dipm = 0;
	}

	if (ret == 0)
		ahci_link_note_lpm_change(port, link);
	if (policy != old_policy || policy != AHCI_LPM_DEFAULT_POLICY || ret)
		port_info(port, "LPM policy %s: SControl %X, HIPM %s%s, DIPM %s, "
				"DevSleep %s",
			  ahci_lpm_policy_name(policy), v,
		  hipm ? ((set & AHCI_PxCMD_ASP) ? "slumber" : "partial")
		       : "off",
			  apst ? " +APST" : "", dipm ? "on" : "off",
			  devslp ? "on" : "off");
	else
		port_dbg(port, "LPM policy %s (SControl %X)",
			 ahci_lpm_policy_name(policy), v);
	return ret;
}

/* ==========================================================================
 * Suspend, resume, shutdown
 * ========================================================================== */

int ahci_port_suspend(struct ahci_port *port)
{
	struct ahci_hba *hba;
	uint32_t token = 0, ssts;
	uint64_t fl;
	int rc;

	BUG_ON(!port);
	hba = port->hba;
	ahci_might_sleep();
	ata_mutex_lock(&port->eh_mutex);

	if (port->state == AHCI_PORT_SUSPENDED) {
		ata_mutex_unlock(&port->eh_mutex);
		return 0;
	}
	if (hba->quirks & AHCI_Q_BROKEN_SUSPEND)
		port_info(port, "controller has a broken suspend: the port "
				"is only quiesced");

	/* 1. Nothing in flight, nothing new coming. */
	rc = ahci_port_quiesce(port, AHCI_QUIESCE_TIMEOUT_MS, &token);
	if (rc) {
		port_warn(port, "suspend refused: commands still active");
		ahci_port_unquiesce(port, token);
		ata_mutex_unlock(&port->eh_mutex);
		return -EBUSY;
	}

	/* 2. Remember the registers a controller reset would lose. */
	port->saved_cmd = ahci_port_readl(port, AHCI_PxCMD);
	port->saved_ie = ahci_port_readl(port, AHCI_PxIE);
	port->saved_sctl = ahci_port_readl(port, AHCI_PxSCTL);
	if (hba->caps.fbss || (hba->quirks & AHCI_Q_YES_FBS))
		port->saved_fbs = ahci_port_readl(port, AHCI_PxFBS);
	if (port->devslp_capable)
		port->saved_devslp = ahci_port_readl(port, AHCI_PxDEVSLP);

	/* 3. Put the link into its lowest state the HBA can request. */
	ssts = ahci_port_readl(port, AHCI_PxSSTS);
	if (!(hba->quirks & (AHCI_Q_NO_LPM | AHCI_Q_BROKEN_SUSPEND)) &&
	    AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_PHY &&
	    (ahci_port_readl(port, AHCI_PxCMD) & AHCI_PxCMD_ST)) {
		if (hba->caps.ssc)
			(void)ahci_port_set_icc(port, AHCI_PxCMD_ICC_SLUMBER,
						AHCI_SSTS_IPM_SLUMBER);
		else if (hba->caps.psc)
			(void)ahci_port_set_icc(port, AHCI_PxCMD_ICC_PARTIAL,
						AHCI_SSTS_IPM_PARTIAL);
	}

	/* 4. Interrupts off, engines off. */
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_irq_disable_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
	if (!(hba->quirks & AHCI_Q_BROKEN_SUSPEND)) {
		(void)ahci_port_stop_engine(port);
		(void)ahci_port_stop_fis_rx(port);
	}

	spin_lock_irqsave(&port->lock, &fl);
	port->state = AHCI_PORT_SUSPENDED;
	spin_unlock_irqrestore(&port->lock, fl);
	/* The block queues stay stopped until resume restarts them. */
	port_info(port, "suspended (SStatus %X)",
		  ahci_port_readl(port, AHCI_PxSSTS));
	ata_mutex_unlock(&port->eh_mutex);
	return 0;
}

/* After resume: bring the host link's device(s) back, or notice they are
 * gone, or that something new is there. */
static int ahci_port_resume_devices(struct ahci_port *port)
{
	struct ahci_link *link = &port->host_link;
	enum ata_class cls = ATA_DEV_NONE;
	int rc;

	if (!link->dev && !port->pmp_attached)
		return ahci_port_probe(port); /* plugged while asleep? */

	rc = port->hba->kahci_running ? ahci_port_reset_attempts(port, &cls, 0)
				      : ahci_port_hardreset(port, &cls);
	if (rc == -ENODEV || cls == ATA_DEV_NONE) {
		ahci_port_remove_device(port, "gone after resume");
		return 0;
	}
	if (rc)
		return rc;
	if (port->pmp_attached) {
		if (cls == ATA_DEV_PMP) {
			rc = ahci_pmp_revalidate(port);
			if (rc == 0)
				return 0;
		}
		ahci_port_remove_device(port, "port multiplier changed");
		return ahci_port_probe(port);
	}
	if (cls != link->class && !(cls == ATA_DEV_UNKNOWN &&
				    link->class == ATA_DEV_ATA)) {
		ahci_port_remove_device(port, "different device after resume");
		return ahci_port_probe(port);
	}
	ahci_port_make_runnable(port);
	rc = ata_dev_revalidate(link->dev);
	if (rc) {
		ahci_port_remove_device(port, "revalidation failed");
		return ahci_port_probe(port);
	}
	if (port->lpm_policy != AHCI_LPM_KEEP_FIRMWARE)
		(void)ahci_port_set_lpm(port, link, port->lpm_policy);
	return 0;
}

int ahci_port_resume(struct ahci_port *port)
{
	uint64_t fl;
	int rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ata_mutex_lock(&port->eh_mutex);

	if (port->state != AHCI_PORT_SUSPENDED) {
		ata_mutex_unlock(&port->eh_mutex);
		return 0;
	}

	/* The controller may have been reset or powered: program the port
	 * again from scratch (CLB/FB, SERR/IS, SControl, spin-up, FRE,
	 * PxIE). */
	spin_lock_irqsave(&port->lock, &fl);
	port->state = AHCI_PORT_IDLE;
	spin_unlock_irqrestore(&port->lock, fl);
	rc = ahci_port_init(port);
	if (rc) {
		port_warn(port, "resume: port initialisation failed (%d)", rc);
		spin_lock_irqsave(&port->lock, &fl);
		port->state = AHCI_PORT_DISABLED;
		spin_unlock_irqrestore(&port->lock, fl);
		if (port->host_link.dev || port->pmp_attached)
			ahci_port_remove_device(port, "port dead after resume");
		ata_mutex_unlock(&port->eh_mutex);
		return rc;
	}

	rc = ahci_port_resume_devices(port);
	if (rc)
		port_warn(port, "resume: devices not restored (%d)", rc);

	/* Restart every queue suspend left stopped. */
	ahci_port_unquiesce(port, AHCI_QUIESCE_ALL);
	port_info(port, "resumed (SStatus %X)",
		  ahci_port_readl(port, AHCI_PxSSTS));
	ata_mutex_unlock(&port->eh_mutex);
	return rc;
}

void ahci_port_shutdown(struct ahci_port *port)
{
	struct ahci_hba *hba;
	int locked = 0;
	uint64_t fl;

	if (!port)
		return;
	hba = port->hba;

	/* A wedged error handler must not stop a power-off: try the mutex
	 * for a while, then go on without it. */
	if (ata_can_sleep()) {
		for (uint32_t ms = 0; ms < AHCI_SHUTDOWN_LOCK_MS; ms += 10) {
			if (ata_mutex_trylock(&port->eh_mutex)) {
				locked = 1;
				break;
			}
			ahci_msleep(10);
		}
		if (!locked)
			port_warn(port, "shutdown without the eh_mutex (error "
					"handling busy)");
	} else if (ata_mutex_trylock(&port->eh_mutex)) {
		locked = 1;
	}

	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_irq_disable_locked(port);
	port->state = AHCI_PORT_DISABLED;
	spin_unlock_irqrestore(&port->lock, fl);
	/* Completions parked for a lock-free context: their waiters are
	 * not left behind. */
	(void)ahci_cmd_run_deferred_completions(port);

	/* The firmware of the next boot expects a quiet port: no DevSleep,
	 * no FBS, no aggressive LPM, engines stopped. */
	if (port->devslp_capable) {
		uint32_t v = ahci_port_readl(port, AHCI_PxDEVSLP);

		if (v & AHCI_DEVSLP_ADSE) {
			(void)ahci_port_stop_engine(port);
			spin_lock_irqsave(&port->lock, &fl);
			ahci_port_writel(port, AHCI_PxDEVSLP,
					 v & ~AHCI_DEVSLP_ADSE);
			spin_unlock_irqrestore(&port->lock, fl);
		}
	}
	(void)ahci_port_stop_engine(port);
	if (port->fbs_enabled ||
	    ((hba->caps.fbss || (hba->quirks & AHCI_Q_YES_FBS)) &&
	     (ahci_port_readl(port, AHCI_PxFBS) & AHCI_PxFBS_EN))) {
		spin_lock_irqsave(&port->lock, &fl);
		ahci_port_writel(port, AHCI_PxFBS,
				 ahci_port_readl(port, AHCI_PxFBS) &
					 ~(AHCI_PxFBS_EN | AHCI_PxFBS_DEC));
		port->fbs_enabled = 0;
		spin_unlock_irqrestore(&port->lock, fl);
	}
	ahci_port_cmd_rmw(port,
			  AHCI_PxCMD_ALPE | AHCI_PxCMD_ASP | AHCI_PxCMD_APSTE,
			  0);
	(void)ahci_port_stop_fis_rx(port);
	ahci_port_clear_status(port);
	port_dbg(port, "shut down (PxCMD %08x)",
		 ahci_port_readl(port, AHCI_PxCMD));

	if (locked)
		ata_mutex_unlock(&port->eh_mutex);
}
