// LikeOS AHCI driver - port multipliers and FIS-based switching
//
// A SATA port multiplier (Serial ATA Port Multiplier specification 1.2)
// fans one host port out to up to 15 device ports.  The host talks to it
// through the PM port field of every FIS: 0..14 address the fan-out ports,
// 15 the multiplier's own control port.  The control port has registers:
//
//   GSCR  general status and control (product id, revision, number of
//         fan-out ports, error summary, features supported/enabled)
//   PSCR  per fan-out port status and control: the same SStatus /
//         SError / SControl / SActive / SNotification a host port has
//
// both read and written with two ATA commands sent to port 15: READ PORT
// MULTIPLIER (E4h) and WRITE PORT MULTIPLIER (E8h) -- the register number
// in the feature field, the fan-out port in the device field, the 32-bit
// value in count / LBA low / LBA mid / LBA high.
//
// On top of that this file implements
//   * attaching a multiplier: PxCMD.PMA, the GSCR identity, notification
//     and error-summary enables, then a hard reset of every fan-out link
//     through its PSCR SControl and a software reset with the link's PM
//     port number to read the device's signature, and the device probe;
//   * detaching it and revalidating it after a reset or resume;
//   * asynchronous notification (PxSNTF) and the GSCR error summary: which
//     fan-out link changed, its SError, hot-plug on a fan-out link,
//     link-level error handling;
//   * FIS-based switching (AHCI 1.3.1, section 9.3): with PxFBS.EN the
//     HBA keeps commands outstanding to several devices behind the
//     multiplier at once and gives every device its own 256-byte area in
//     a 4 KiB received-FIS block; the issue path names the device in
//     PxFBS.DEV; an error on one device is cleared with PxFBS.DEC without
//     stopping the others (section 9.3.6).
//
// Without FIS-based switching the HBA does command-based switching: only
// one device behind the multiplier may have commands outstanding at a time
// (the issue path keeps that rule), and a command to the control port
// needs the whole port idle.  Every register access here therefore runs
// with the port quiesced (ahci_port_quiesce) unless the caller did that.
//
// Every function runs with the port's eh_mutex held (the kahci thread) or
// at boot, polled.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */

#define AHCI_PMP_DEBUG 0

/* "[ahci] ata1.15: ..." for the multiplier, "[ahci] ata1.03: ..." for a
 * fan-out link */
#define pmp_info(port, pmp, fmt, ...)                                  \
	kprintf("[ahci] %s.%02d: " fmt "\n", (port)->name, (int)(pmp), \
		##__VA_ARGS__)
#define pmp_warn(port, pmp, fmt, ...)                                    \
	kprintf("[ahci] %s.%02d: WARNING: " fmt "\n", (port)->name,      \
		(int)(pmp), ##__VA_ARGS__)
#if AHCI_PMP_DEBUG
#define pmp_dbg(port, pmp, fmt, ...) pmp_info(port, pmp, fmt, ##__VA_ARGS__)
#else
#define pmp_dbg(port, pmp, fmt, ...)                     \
	do {                                             \
		if (0)                                   \
			kprintf(fmt, ##__VA_ARGS__);     \
		(void)(port);                            \
		(void)(pmp);                             \
	} while (0)
#endif

/* ==========================================================================
 * Tuning
 * ========================================================================== */

/* READ/WRITE PORT MULTIPLIER attempts before giving up on a register. */
#define AHCI_PMP_TRIES 2
/* How long a fan-out link event waits for the other devices' commands to
 * finish before it aborts and requeues them. */
#define AHCI_PMP_QUIESCE_MS 2000
/* Silicon Image 3726/3826 private register with the early R_OK bit. */
#define AHCI_PMP_GSCR_SII_POL 129
/* A reset attempt's release of DET on a fan-out link: checked up to this
 * many times, each after this delay. */
#define AHCI_PMP_LINK_RESUME_TRIES 5
#define AHCI_PMP_LINK_RESUME_DELAY_MS 200
/* A device behind a multiplier sometimes needs a moment between its link
 * coming up and answering the SRST. */
#define AHCI_PMP_SRST_RETRY_DELAY_MS 100

/* Bits of the PMP's error-summary/notification masks that name fan-out
 * ports, and the bit of PxSNTF that is the multiplier itself. */
#define AHCI_PMP_CTRL_BIT (1u << ATA_PMP_CTRL_PORT)

/* ==========================================================================
 * Multiplier quirks
 * ========================================================================== */

#define PMPQ_NO_SNTF (1u << 0) /* notification unreliable: poll GSCR */
#define PMPQ_NO_LPM (1u << 1) /* no link power management on fan-outs */
#define PMPQ_SLOW_RESET (1u << 2) /* hold SControl.DET = 1 for 10 ms */
#define PMPQ_LAST_PORT_CONFIG (1u << 3) /* highest port is its config dev */
#define PMPQ_NO_FBS (1u << 4) /* FIS-based switching unreliable */

struct ahci_pmp_quirk {
	uint16_t vendor, device;
	uint8_t max_ports; /* 0 = as reported */
	uint32_t flags;
	const char *name;
};

/* Known multipliers.  The Silicon Image 3726 and 4726 report their
 * internal configuration device as the last fan-out port; it is no disk
 * and hangs when it is reset like one. */
static const struct ahci_pmp_quirk ahci_pmp_quirks[] = {
	{ 0x1095, 0x3726, 5,
	  PMPQ_LAST_PORT_CONFIG | PMPQ_NO_LPM | PMPQ_SLOW_RESET,
	  "Silicon Image 3726" },
	{ 0x1095, 0x4726, 5,
	  PMPQ_LAST_PORT_CONFIG | PMPQ_NO_LPM | PMPQ_SLOW_RESET,
	  "Silicon Image 4726" },
	{ 0x1095, 0x4723, 0, PMPQ_NO_LPM | PMPQ_SLOW_RESET,
	  "Silicon Image 4723" },
	{ 0x197b, 0x2352, 0, PMPQ_NO_LPM, "JMicron JMB352" },
	{ 0x197b, 0x0325, 0, 0, "JMicron JMB39x" },
	{ 0x11ab, 0x4140, 0, PMPQ_NO_LPM, "Marvell 88SM4140" },
	{ 0, 0, 0, 0, NULL },
};

static const struct ahci_pmp_quirk *ahci_pmp_find_quirk(uint32_t prod_id)
{
	uint16_t vendor = (uint16_t)(prod_id & 0xFFFF);
	uint16_t device = (uint16_t)(prod_id >> 16);

	for (const struct ahci_pmp_quirk *q = ahci_pmp_quirks; q->vendor; q++)
		if (q->vendor == vendor && q->device == device)
			return q;
	return NULL;
}

static uint32_t ahci_pmp_quirk_flags(const struct ahci_port *port)
{
	const struct ahci_pmp_quirk *q =
		ahci_pmp_find_quirk(port->pmp_gscr_prod_id);

	return q ? q->flags : 0;
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

#define ahci_might_sleep()                   \
	do {                                 \
		if (sched_current() != NULL) \
			might_sleep();       \
	} while (0)

static inline void ahci_pmp_assert_eh_held(const struct ahci_port *port)
{
	if (ata_can_sleep())
		WARN_ON_ONCE(!ata_mutex_held(&port->eh_mutex));
}

/* The spec revision a GSCR[1] value claims. */
static const char *ahci_pmp_spec_rev(uint32_t rev)
{
	if (rev & (1u << 3))
		return "1.2";
	if (rev & (1u << 2))
		return "1.1";
	if (rev & (1u << 1))
		return "1.0";
	return "?";
}

/* Mask of the fan-out ports in use. */
static uint32_t ahci_pmp_link_mask(const struct ahci_port *port)
{
	if (port->nr_pmp_links == 0)
		return 0;
	if (port->nr_pmp_links >= 32)
		return 0xFFFFFFFFu;
	return (1u << port->nr_pmp_links) - 1;
}

/* Prepare the D2H area of the control port so the answer is known new. */
static void ahci_pmp_clear_ctrl_d2h(struct ahci_port *port)
{
	volatile struct fis_reg_d2h *d2h =
		ahci_port_rx_d2h(port, ATA_PMP_CTRL_PORT);

	d2h->fis_type = 0;
	d2h->status = ATA_BUSY;
	d2h->error = 0;
	ahci_mb();
}

/* The engine must run for a command to port 15; after a reset it may not. */
static int ahci_pmp_ensure_engine(struct ahci_port *port)
{
	uint32_t cmd = ahci_port_readl(port, AHCI_PxCMD);
	int rc;

	if (cmd & AHCI_PxCMD_ST)
		return 0;
	if (!(cmd & AHCI_PxCMD_FR)) {
		rc = ahci_port_start_fis_rx(port);
		if (rc && rc != -ETIMEDOUT)
			return rc;
	}
	rc = ahci_port_start_engine(port);
	if (rc == -EBUSY && ahci_port_clo(port) == 0)
		rc = ahci_port_start_engine(port);
	return rc;
}

/* Stop the port's normal traffic for multiplier work.  Innocent commands
 * get AHCI_PMP_QUIESCE_MS to finish; what is still outstanding then (the
 * commands of a vanished device, typically) is aborted with the engine
 * stopped and handed back for requeueing (-ECANCELED). */
static int ahci_pmp_quiesce(struct ahci_port *port, uint32_t *token)
{
	int rc;

	rc = ahci_port_quiesce(port, AHCI_PMP_QUIESCE_MS, token);
	if (rc == 0)
		return 0;
	/* Only commands of normal operation are aborted here; while error
	 * handling owns the port its commands are left alone (-EBUSY).  An
	 * EH run calling us (eh_running, our caller holds the eh_mutex) is
	 * single-threaded: what is outstanding then is ordinary traffic
	 * from before the run, which the port in state EH no longer
	 * replenishes. */
	if (!(*token & AHCI_QUIESCE_STATE) && !port->eh_running)
		return rc;
	pmp_info(port, ATA_PMP_CTRL_PORT,
		 "aborting outstanding commands (CI %08x) for requeue",
		 ahci_port_readl(port, AHCI_PxCI));
	(void)ahci_port_stop_engine_abort(port);
	ahci_port_fail_all(port, -ECANCELED);
	return ahci_pmp_ensure_engine(port);
}

/* ==========================================================================
 * READ / WRITE PORT MULTIPLIER
 * ========================================================================== */

/* Issue one register FIS to the control port and collect the D2H answer
 * (result registers in *res).  The caller has the port idle. */
static int ahci_pmp_exec_fis(struct ahci_port *port,
			     const struct fis_reg_h2d *fis,
			     struct fis_reg_d2h *res)
{
	volatile struct fis_reg_d2h *d2h;
	uint32_t tfd;
	int rc;

	rc = ahci_pmp_ensure_engine(port);
	if (rc)
		return rc;
	ahci_pmp_clear_ctrl_d2h(port);
	if (port->fbs_enabled) {
		uint64_t fl;

		/* PxFBS.DEV must name the control port before CI is set. */
		spin_lock_irqsave(&port->lock, &fl);
		ahci_fbs_select_dev_locked(port, ATA_PMP_CTRL_PORT);
		spin_unlock_irqrestore(&port->lock, fl);
	}
	rc = ahci_exec_raw_fis(port, ATA_PMP_CTRL_PORT, fis, 0,
			       ATA_TMOUT_PMP_MS);

	ahci_rmb();
	d2h = ahci_port_rx_d2h(port, ATA_PMP_CTRL_PORT);
	res->fis_type = d2h->fis_type;
	res->pm_i = d2h->pm_i;
	res->status = d2h->status;
	res->error = d2h->error;
	res->lba0 = d2h->lba0;
	res->lba1 = d2h->lba1;
	res->lba2 = d2h->lba2;
	res->device = d2h->device;
	res->lba3 = d2h->lba3;
	res->lba4 = d2h->lba4;
	res->lba5 = d2h->lba5;
	res->count_lo = d2h->count_lo;
	res->count_hi = d2h->count_hi;

	if (rc) {
		tfd = ahci_port_readl(port, AHCI_PxTFD);
		pmp_dbg(port, ATA_PMP_CTRL_PORT,
			"%s failed (%d), TFD %08x, D2H %02x/%02x",
			fis->command == ATA_CMD_PMP_READ ? "READ PM"
							 : "WRITE PM",
			rc, tfd, res->status, res->error);
		if (rc == -ETIMEDOUT)
			return rc;
		return (tfd & AHCI_PxTFD_STS_ERR) ? -EIO : rc;
	}
	if (res->fis_type != FIS_TYPE_REG_D2H) {
		/* With FBS the answer must be in the control port's area;
		 * without it the shared area.  None arrived: the multiplier
		 * completed the command without the register FIS. */
		pmp_dbg(port, ATA_PMP_CTRL_PORT, "no D2H answer (type %02x)",
			res->fis_type);
		return -EIO;
	}
	if (res->status & (ATA_ERR | ATA_DF)) {
		pmp_dbg(port, ATA_PMP_CTRL_PORT,
			"register access rejected: status %02x error %02x",
			res->status, res->error);
		return -EIO;
	}
	return 0;
}

static void ahci_pmp_build_fis(struct fis_reg_h2d *fis, uint8_t command,
			       int pmp_port, int reg, uint32_t val)
{
	kmemset(fis, 0, sizeof(*fis));
	fis->fis_type = FIS_TYPE_REG_H2D;
	fis->pm_c = (uint8_t)(ATA_PMP_CTRL_PORT | FIS_H2D_C);
	fis->command = command;
	fis->feature_lo = (uint8_t)reg;
	fis->feature_hi = (uint8_t)(reg >> 8);
	fis->device = (uint8_t)(pmp_port & 0x0F);
	if (command == ATA_CMD_PMP_WRITE) {
		fis->count_lo = (uint8_t)val;
		fis->lba0 = (uint8_t)(val >> 8);
		fis->lba1 = (uint8_t)(val >> 16);
		fis->lba2 = (uint8_t)(val >> 24);
	}
}

static int ahci_pmp_access(struct ahci_port *port, uint8_t command,
			   int pmp_port, int reg, uint32_t *val)
{
	struct fis_reg_h2d fis;
	struct fis_reg_d2h res;
	uint32_t token = 0;
	int rc = -EIO;

	BUG_ON(!port || !val);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	if (pmp_port < 0 || pmp_port > ATA_PMP_CTRL_PORT || reg < 0 ||
	    reg > 0xFFFF)
		return -EINVAL;
	if (AHCI_SSTS_DET(ahci_port_readl(port, AHCI_PxSSTS)) !=
	    AHCI_SSTS_DET_PHY)
		return -ENODEV;

	rc = ahci_pmp_quiesce(port, &token);
	if (rc) {
		ahci_port_unquiesce(port, token);
		return rc;
	}
	ahci_pmp_build_fis(&fis, command, pmp_port, reg, *val);
	for (int t = 0; t < AHCI_PMP_TRIES; t++) {
		kmemset(&res, 0, sizeof(res));
		rc = ahci_pmp_exec_fis(port, &fis, &res);
		if (rc == 0)
			break;
		if (rc == -ENODEV)
			break;
		/* A timed-out command leaves the slot set: kick the engine
		 * before the next attempt. */
		(void)ahci_port_engine_restart(port);
	}
	ahci_port_unquiesce(port, token);
	if (rc) {
		WARN_RATELIMIT(1, "%s: %s of PMP port %d register %d failed "
				  "(%d)",
			       port->name,
			       command == ATA_CMD_PMP_READ ? "read" : "write",
			       pmp_port, reg, rc);
		return rc;
	}
	if (command == ATA_CMD_PMP_READ)
		*val = (uint32_t)res.count_lo | ((uint32_t)res.lba0 << 8) |
		       ((uint32_t)res.lba1 << 16) | ((uint32_t)res.lba2 << 24);
	return 0;
}

int ahci_pmp_read(struct ahci_port *port, int pmp_port, int reg,
		  uint32_t *val)
{
	uint32_t v = 0;
	int rc;

	if (!val)
		return -EINVAL;
	rc = ahci_pmp_access(port, ATA_CMD_PMP_READ, pmp_port, reg, &v);
	if (rc == 0) {
		*val = v;
		pmp_dbg(port, pmp_port, "read reg %d = %08x", reg, v);
	}
	return rc;
}

int ahci_pmp_write(struct ahci_port *port, int pmp_port, int reg,
		   uint32_t val)
{
	uint32_t v = val;
	int rc;

	rc = ahci_pmp_access(port, ATA_CMD_PMP_WRITE, pmp_port, reg, &v);
	if (rc == 0)
		pmp_dbg(port, pmp_port, "write reg %d = %08x", reg, val);
	return rc;
}

/* ==========================================================================
 * PSCR access of a fan-out link
 * ========================================================================== */

static int ahci_pmp_link_ok(const struct ahci_link *link)
{
	const struct ahci_port *port;

	if (!link || link->is_host || !link->port)
		return 0;
	port = link->port;
	return port->pmp_attached && link->pmp >= 0 &&
	       (uint32_t)link->pmp < port->nr_pmp_links;
}

int ahci_pmp_scr_read(struct ahci_link *link, int scr, uint32_t *val)
{
	uint32_t v = 0;
	int rc;

	if (!ahci_pmp_link_ok(link) || !val)
		return -EINVAL;
	if (scr < AHCI_SCR_STATUS || scr > AHCI_SCR_NOTIFY)
		return -EINVAL;
	rc = ahci_pmp_read(link->port, link->pmp, scr, &v);
	if (rc)
		return rc;
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

int ahci_pmp_scr_write(struct ahci_link *link, int scr, uint32_t val)
{
	int rc;

	if (!ahci_pmp_link_ok(link))
		return -EINVAL;
	if (scr == AHCI_SCR_STATUS)
		return -EINVAL; /* read-only */
	if (scr < AHCI_SCR_STATUS || scr > AHCI_SCR_NOTIFY)
		return -EINVAL;
	rc = ahci_pmp_write(link->port, link->pmp, scr, val);
	if (rc == 0 && scr == AHCI_SCR_CONTROL)
		link->scontrol = val;
	return rc;
}

/* ==========================================================================
 * PxCMD.PMA
 * ========================================================================== */

/* PMA may only change with the command engine stopped. */
static int ahci_pmp_set_pma(struct ahci_port *port, int on)
{
	uint32_t cmd = ahci_port_readl(port, AHCI_PxCMD);
	int was_running = (cmd & AHCI_PxCMD_ST) != 0;
	uint64_t fl;
	int rc;

	if (!!(cmd & AHCI_PxCMD_PMA) == !!on)
		return 0;
	if (was_running) {
		rc = ahci_port_stop_engine(port);
		if (rc)
			return rc;
	}
	spin_lock_irqsave(&port->lock, &fl);
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	cmd &= ~(AHCI_PxCMD_ICC_MASK | AHCI_PxCMD_CLO | AHCI_PxCMD_PMA);
	if (on)
		cmd |= AHCI_PxCMD_PMA;
	ahci_port_writel(port, AHCI_PxCMD, cmd);
	ahci_port_flush(port);
	spin_unlock_irqrestore(&port->lock, fl);
	if (was_running || on)
		return ahci_pmp_ensure_engine(port);
	return 0;
}

/* ==========================================================================
 * FIS-based switching (AHCI 1.3.1, 9.3)
 * ========================================================================== */

void ahci_fbs_select_dev_locked(struct ahci_port *port, int pmp)
{
	uint32_t v;

	VM_BUG_ON(!port);
	lockdep_assert_held(&port->lock);
	if (!port->fbs_enabled || port->fbs_last_dev == pmp)
		return;
	if (WARN_ON_ONCE(pmp < 0 || pmp > ATA_PMP_CTRL_PORT))
		return;
	v = ahci_port_readl(port, AHCI_PxFBS);
	v &= ~(AHCI_PxFBS_DEV_MASK | AHCI_PxFBS_DEC);
	v |= ((uint32_t)pmp << AHCI_PxFBS_DEV_SHIFT) & AHCI_PxFBS_DEV_MASK;
	ahci_port_writel(port, AHCI_PxFBS, v);
	port->fbs_last_dev = pmp;
}

int ahci_fbs_enable(struct ahci_port *port)
{
	uint32_t fbs, cmd, token = 0;
	int was_running, rc;
	uint64_t fl;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	if (!port->fbs_capable)
		return -EOPNOTSUPP;
	if (!port->pmp_attached)
		return -EINVAL; /* FBS without a multiplier is meaningless */
	if (port->fbs_enabled)
		return 0;
	if (ahci_pmp_quirk_flags(port) & PMPQ_NO_FBS) {
		pmp_info(port, ATA_PMP_CTRL_PORT,
			 "FIS-based switching not used with this multiplier");
		return -EOPNOTSUPP;
	}
	/* The per-device areas need the 4 KiB, 4 KiB aligned FIS block. */
	if (port->rx_fis_area.size < AHCI_RX_FIS_FBS_SIZE ||
	    (port->rx_fis_area.bus & (AHCI_RX_FIS_FBS_ALIGN - 1))) {
		WARN_RATELIMIT(1, "%s: FBS-capable port without a 4 KiB FIS "
				  "area (%llu bytes at %llx)",
			       port->name,
			       (unsigned long long)port->rx_fis_area.size,
			       (unsigned long long)port->rx_fis_area.bus);
		return -EINVAL;
	}

	/* 9.3.3: PxFBS.EN changes only with ST = 0. */
	rc = ahci_port_quiesce(port, AHCI_PMP_QUIESCE_MS, &token);
	if (rc)
		goto out_unquiesce;
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	was_running = (cmd & AHCI_PxCMD_ST) != 0;
	rc = ahci_port_stop_engine(port);
	if (rc)
		goto out_unquiesce;

	kmemset(port->rx_fis, 0, AHCI_RX_FIS_FBS_SIZE);
	ahci_mb();
	spin_lock_irqsave(&port->lock, &fl);
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	fbs &= ~(AHCI_PxFBS_DEV_MASK | AHCI_PxFBS_DEC);
	ahci_port_writel(port, AHCI_PxFBS, fbs | AHCI_PxFBS_EN);
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	if (fbs & AHCI_PxFBS_EN) {
		port->fbs_enabled = 1;
		port->fbs_last_dev = 0;
	}
	spin_unlock_irqrestore(&port->lock, fl);

	if (!(fbs & AHCI_PxFBS_EN)) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "the HBA refused to enable FIS-based switching "
			 "(PxFBS %08x)",
			 fbs);
		rc = -EIO;
	} else {
		pmp_info(port, ATA_PMP_CTRL_PORT,
			 "FIS-based switching enabled (ADO %u, PxFBS %08x)",
			 (fbs & AHCI_PxFBS_ADO_MASK) >> AHCI_PxFBS_ADO_SHIFT,
			 fbs);
		rc = 0;
	}
	if (was_running || port->pmp_attached) {
		int rc2 = ahci_pmp_ensure_engine(port);

		if (rc2 && !rc)
			rc = rc2;
	}
out_unquiesce:
	ahci_port_unquiesce(port, token);
	return rc;
}

int ahci_fbs_disable(struct ahci_port *port)
{
	uint32_t fbs, cmd, token = 0;
	int was_running, rc;
	uint64_t fl;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	if (!port->fbs_enabled && !(fbs & AHCI_PxFBS_EN))
		return 0;

	rc = ahci_port_quiesce(port, AHCI_PMP_QUIESCE_MS, &token);
	if (rc) {
		/* Disabling happens on detach as well: go on regardless,
		 * whatever is left was failed by the caller. */
		pmp_dbg(port, ATA_PMP_CTRL_PORT,
			"FBS disable with commands outstanding");
	}
	cmd = ahci_port_readl(port, AHCI_PxCMD);
	was_running = (cmd & AHCI_PxCMD_ST) != 0;
	rc = ahci_port_stop_engine(port);
	if (rc) {
		ahci_port_unquiesce(port, token);
		return rc;
	}
	spin_lock_irqsave(&port->lock, &fl);
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	ahci_port_writel(port, AHCI_PxFBS,
			 fbs & ~(AHCI_PxFBS_EN | AHCI_PxFBS_DEC |
				 AHCI_PxFBS_DEV_MASK));
	fbs = ahci_port_readl(port, AHCI_PxFBS);
	port->fbs_enabled = 0;
	port->fbs_last_dev = 0;
	spin_unlock_irqrestore(&port->lock, fl);
	if (fbs & AHCI_PxFBS_EN)
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "PxFBS.EN did not clear (PxFBS %08x)", fbs);
	else
		pmp_dbg(port, ATA_PMP_CTRL_PORT,
			"FIS-based switching disabled");
	/* The single FIS area is the first 256 bytes again: clear it. */
	kmemset(port->rx_fis, 0, AHCI_RX_FIS_SIZE);
	ahci_mb();
	rc = 0;
	if (was_running && port->pmp_attached)
		rc = ahci_pmp_ensure_engine(port);
	ahci_port_unquiesce(port, token);
	return rc;
}

/* ==========================================================================
 * Fan-out link reset
 * ========================================================================== */

/* One reset of a fan-out link.  `timing' NULL: the polled boot probe's
 * own (delay-counted) link wait; otherwise one attempt of error
 * handling's reset, its every wait bounded by `deadline_ns': COMRESET
 * through PSCR SControl, the release of DET checked (each time after
 * 200 ms), the link debounced with `timing' (-EPIPE: it does not
 * settle), SError cleared, then the classifying SRST. */
static int ahci_pmp_link_reset_common(struct ahci_link *link,
				      enum ata_class *class,
				      uint64_t deadline_ns,
				      const uint32_t *timing, int classify)
{
	struct ahci_port *port;
	uint32_t sctl, v, ssts = 0, serr = 0, sig = AHCI_PxSIG_INVALID;
	uint32_t hold_ms, token = 0, ipm;
	enum ata_class cls = ATA_DEV_NONE;
	int rc, tries;

	if (!ahci_pmp_link_ok(link)) {
		if (class)
			*class = ATA_DEV_NONE;
		return -ENODEV;
	}
	port = link->port;
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);

	link->state = AHCI_LINK_RESETTING;
	link->n_resets++;
	link->last_reset_ns = hrtimer_now_ns();
	ahci_link_check_speed_down(link);

	/* The SRST that classifies the device stops the command engine:
	 * nothing else may be outstanding on the port. */
	rc = ahci_pmp_quiesce(port, &token);
	if (rc)
		goto out;

	/* 1. COMRESET on the fan-out port through its PSCR SControl. */
	rc = ahci_pmp_read(port, link->pmp, SATA_PMP_PSCR_CONTROL, &sctl);
	if (rc)
		goto out;
	ipm = (link->lpm_policy == AHCI_LPM_KEEP_FIRMWARE)
		      ? (sctl & AHCI_SCTL_IPM_MASK)
		      : (link->lpm_policy == AHCI_LPM_MAX_PERFORMANCE ||
			 (ahci_pmp_quirk_flags(port) & PMPQ_NO_LPM))
				? AHCI_SCTL_IPM_NO_PS
				: (sctl & AHCI_SCTL_IPM_MASK);
	v = AHCI_SCTL_DET_COMRESET | ipm;
	if (link->speed_limit)
		v |= ((uint32_t)link->speed_limit << AHCI_SCTL_SPD_SHIFT) &
		     AHCI_SCTL_SPD_MASK;
	rc = ahci_pmp_write(port, link->pmp, SATA_PMP_PSCR_CONTROL, v);
	if (rc)
		goto out;
	hold_ms = (ahci_pmp_quirk_flags(port) & PMPQ_SLOW_RESET)
			  ? 10
			  : AHCI_COMRESET_ASSERT_MS + 1;
	ahci_msleep(hold_ms);
	v &= ~AHCI_SCTL_DET_MASK;
	if (timing) {
		/* 2. Release DET, checked: some multipliers drop an
		 * SControl write.  Each check after 200 ms (a PHY may react
		 * badly to SStatus being read at once), then the link
		 * debounced against the attempt's deadline. */
		uint32_t cur = 0;
		int left = AHCI_PMP_LINK_RESUME_TRIES;

		do {
			rc = ahci_pmp_write(port, link->pmp,
					    SATA_PMP_PSCR_CONTROL, v);
			if (rc)
				goto out;
			ahci_msleep(AHCI_PMP_LINK_RESUME_DELAY_MS);
			rc = ahci_pmp_read(port, link->pmp,
					   SATA_PMP_PSCR_CONTROL, &cur);
			if (rc)
				goto out;
		} while ((cur & AHCI_SCTL_DET_MASK) != AHCI_SCTL_DET_NONE &&
			 --left);
		if ((cur & AHCI_SCTL_DET_MASK) != AHCI_SCTL_DET_NONE)
			pmp_warn(port, link->pmp,
				 "failed to resume link (SControl %X)", cur);
		link->scontrol = v;
		rc = ahci_link_debounce_deadline(link, timing, deadline_ns,
						 &ssts);
		if (rc == -EPIPE) {
			link->state = AHCI_LINK_OFFLINE;
			pmp_warn(port, link->pmp,
				 "link does not settle (SStatus %X)", ssts);
			cls = ATA_DEV_UNKNOWN;
			goto out;
		}
		if (rc)
			goto out;
	} else {
		rc = ahci_pmp_write(port, link->pmp, SATA_PMP_PSCR_CONTROL, v);
		if (rc)
			goto out;
		link->scontrol = v;

		/* 2. Wait for the fan-out link. */
		rc = ahci_link_debounce_deadline(
			link, ahci_deb_timing(link->eh_hotplugged),
			AHCI_NO_DEADLINE, &ssts);
		if (rc && rc != -EPIPE)
			goto out;
	}
	/* 3. The reset latched X/N in its SError (and GSCR_ERROR): clear. */
	if (ahci_pmp_read(port, link->pmp, SATA_PMP_PSCR_ERROR, &serr) == 0 &&
	    serr)
		(void)ahci_pmp_write(port, link->pmp, SATA_PMP_PSCR_ERROR,
				     serr);
	link->sstatus = ssts;
	link->serror = 0;
	link->speed = (uint8_t)AHCI_SSTS_SPD(ssts);

	if (AHCI_SSTS_DET(ssts) != AHCI_SSTS_DET_PHY) {
		link->state = AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_OFFLINE
				      ? AHCI_LINK_DISABLED
				      : AHCI_LINK_OFFLINE;
		if (AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_PRESENT)
			ahci_link_note_error(link);
		pmp_dbg(port, link->pmp,
			"SATA link down (SStatus %X SControl %X)", ssts, v);
		rc = -ENODEV;
		goto out;
	}
	link->state = AHCI_LINK_ONLINE;
	pmp_info(port, link->pmp, "SATA link up %s (SStatus %X SControl %X)",
		 ahci_speed_str(AHCI_SSTS_SPD(ssts)), ssts, v);
	if (!classify) {
		/* The link reset alone (a disabled link's quick reset). */
		cls = ATA_DEV_UNKNOWN;
		rc = 0;
		goto out;
	}

	/* 4. Classify: SRST with the link's PM port; its D2H FIS carries the
	 * signature.  Devices behind a multiplier do not update PxSIG. */
	if (timing) {
		/* Within a reset attempt: once, bounded by the attempt's
		 * deadline (a failure fails the attempt; the next one gets
		 * its own). */
		rc = ahci_port_softreset_deadline(port, link->pmp, &sig,
						  deadline_ns);
	} else {
		tries = 2;
		for (int t = 0; t < tries; t++) {
			rc = ahci_port_softreset(port, link->pmp, &sig);
			if (rc == 0 && sig != AHCI_PxSIG_INVALID)
				break;
			if (t + 1 < tries)
				ahci_msleep(AHCI_PMP_SRST_RETRY_DELAY_MS);
		}
	}
	if (rc == 0 && sig == AHCI_PxSIG_INVALID)
		rc = -EIO;
	if (rc) {
		pmp_warn(port, link->pmp,
			 "link up but the device does not answer SRST (%d)",
			 rc);
		cls = ATA_DEV_UNKNOWN;
		goto out;
	}
	link->sig = sig;
	cls = ata_class_from_sig(sig);
	if (cls == ATA_DEV_NONE)
		cls = ATA_DEV_UNKNOWN;
	pmp_dbg(port, link->pmp, "signature %08x -> %s", sig,
		ata_class_name(cls));
	rc = 0;
out:
	if (rc == -ENODEV)
		cls = ATA_DEV_NONE;
	link->class = cls;
	if (class)
		*class = cls;
	(void)ahci_pmp_ensure_engine(port);
	ahci_port_unquiesce(port, token);
	return rc;
}

/* Error handling's reset of a fan-out link: the attempts of the host
 * link's reset -- each with its deadline (AHCI_EH_RESET_TIMEOUTS), the
 * next one once the previous deadline passed, a lower link speed after a
 * link that would not settle and for the last attempt -- with the
 * hot-plug debounce timing while the link's hot-plug event is handled. */
static int ahci_pmp_link_reset_attempts(struct ahci_link *link,
					enum ata_class *class)
{
	static const uint32_t timeouts[] = AHCI_EH_RESET_TIMEOUTS;
	const int tries = (int)(sizeof(timeouts) / sizeof(timeouts[0]));
	const uint32_t *timing = ahci_deb_timing(link->eh_hotplugged);
	int rc = -EIO;

	for (int try = 0; try < tries; try++) {
		uint64_t start = hrtimer_now_ns();
		uint64_t deadline = start + (uint64_t)timeouts[try] * 1000000ULL;
		uint64_t now;

		rc = ahci_pmp_link_reset_common(link, class, deadline, timing,
						1);
		if (rc == 0 || rc == -ENODEV || try + 1 >= tries)
			break;
		if ((rc == -EPIPE || try + 2 == tries) &&
		    link->speed_limit != AHCI_SSTS_SPD_GEN1) {
			uint8_t cur = link->speed_limit ? link->speed_limit
							: link->speed;

			if (cur > AHCI_SSTS_SPD_GEN1) {
				link->speed_limit = (uint8_t)(cur - 1);
				link->n_speed_downs++;
				pmp_info(link->port, link->pmp,
					 "limiting SATA link speed to %s",
					 ahci_speed_str(link->speed_limit));
			}
		}
		now = hrtimer_now_ns();
		pmp_warn(link->port, link->pmp,
			 "reset failed (%d), retrying in %llu secs", rc,
			 (unsigned long long)(now < deadline
				? (deadline - now + 999999999ULL) / 1000000000ULL
				: 0));
		if (now < deadline)
			ahci_msleep((uint32_t)((deadline - now + 999999ULL) /
					       1000000ULL));
	}
	return rc;
}

int ahci_pmp_link_reset(struct ahci_link *link, enum ata_class *class)
{
	/* Once the clock runs the reset is error handling's (attempts with
	 * deadlines); the polled boot probe counts its waits in delays. */
	if (link && link->port && link->port->hba &&
	    link->port->hba->kahci_running)
		return ahci_pmp_link_reset_attempts(link, class);
	return ahci_pmp_link_reset_common(link, class, 0, NULL, 1);
}

void ahci_pmp_eh_begin(struct ahci_port *port)
{
	for (uint32_t l = 0; l < AHCI_PMP_MAX_LINKS; l++)
		port->pmp_link_tries[l] = AHCI_EH_PMP_LINK_TRIES;
}

/* A fan-out link failed its recovery: 1 when it has tries left (the
 * caller tries again), 0 when it is out of them -- the link is disabled,
 * and its device with it. */
static int ahci_pmp_handle_link_fail(struct ahci_port *port,
				     struct ahci_link *link)
{
	uint8_t *tries;

	if (link->pmp < 0 || link->pmp >= AHCI_PMP_MAX_LINKS)
		return 0;
	tries = &port->pmp_link_tries[link->pmp];
	if (*tries && --*tries)
		return 1;
	if (!link->eh_link_disabled) {
		pmp_warn(port, link->pmp,
			 "failed to recover link after %d tries, disabling",
			 AHCI_EH_PMP_LINK_TRIES);
		link->eh_link_disabled = 1;
	}
	if (link->dev && link->dev->magic == ATA_DEVICE_MAGIC &&
	    !(link->dev->flags & ATA_DFLAG_DISABLED)) {
		kprintf("[ahci] %s: device disabled by error handling\n",
			link->dev->name);
		link->dev->flags |= ATA_DFLAG_DISABLED;
	}
	return 0;
}

int ahci_pmp_link_recover(struct ahci_link *link, enum ata_class *class)
{
	struct ahci_port *port;
	int rc;

	if (!link || !link->port)
		return -EINVAL;
	port = link->port;
	if (link->eh_link_disabled) {
		if (class)
			*class = ATA_DEV_NONE;
		return -ENODEV;
	}
	for (;;) {
		rc = ahci_pmp_link_reset(link, class);
		if (rc == 0 || rc == -ENODEV)
			return rc;
		if (!ahci_pmp_handle_link_fail(port, link))
			return rc;
		pmp_info(port, link->pmp, "link recovery failed (%d), trying again",
			 rc);
	}
}

int ahci_pmp_handle_disabled_links(struct ahci_port *port)
{
	for (uint32_t i = 0; i < port->nr_pmp_links && i < AHCI_PMP_MAX_LINKS;
	     i++) {
		struct ahci_link *link = &port->pmp_links[i];
		int rc;

		if (!link->eh_link_disabled)
			continue;
		/* Some multipliers need a link reset for SError.N to work. */
		(void)ahci_pmp_link_reset_common(
			link, NULL,
			hrtimer_now_ns() +
				(uint64_t)AHCI_TMOUT_INTERNAL_QUICK_MS * 1000000ULL,
			ahci_deb_timing(0), 0);
		/* unconditionally clear SError.N */
		rc = ahci_pmp_scr_write(link, AHCI_SCR_ERROR, AHCI_SERR_DIAG_N);
		if (rc) {
			pmp_warn(port, link->pmp,
				 "failed to clear SError.N (%d)", rc);
			return rc;
		}
	}
	return 0;
}

int ahci_pmp_recover(struct ahci_port *port, int reset)
{
	int tries = AHCI_EH_PMP_TRIES, reval_failed = 0, rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	for (;;) {
		if (reset) {
			enum ata_class cls = ATA_DEV_NONE;

			rc = port->hba->kahci_running
				     ? ahci_port_reset_attempts(port, &cls,
							       port->eh_hotplugged)
				     : ahci_port_hardreset(port, &cls);
			if (rc || cls != ATA_DEV_PMP) {
				pmp_warn(port, ATA_PMP_CTRL_PORT,
					 "failed to reset the multiplier (%d), giving up",
					 rc);
				break;
			}
		}
		rc = ahci_pmp_revalidate(port);
		if (rc == 0)
			return 0;
		tries--;
		if (rc == -ENODEV && tries > 2)
			tries = 2; /* gone or different: two more chances */
		if (!tries) {
			pmp_warn(port, ATA_PMP_CTRL_PORT,
				 "failed to recover the multiplier after %d tries, giving up",
				 AHCI_EH_PMP_TRIES);
			break;
		}
		/* consecutive revalidation failures: a lower link speed */
		if (reval_failed)
			(void)ahci_port_down_spd_limit(port);
		else
			reval_failed = 1;
		reset = 1;
	}
	(void)ahci_port_stop_engine_abort(port);
	ahci_port_fail_all(port, -ENODEV);
	ahci_pmp_detach(port);
	return -ENODEV;
}

/* ==========================================================================
 * Fan-out link probe
 * ========================================================================== */

/* Reset one fan-out link and attach what is there. */
static int ahci_pmp_probe_link(struct ahci_port *port, struct ahci_link *link)
{
	enum ata_class cls = ATA_DEV_NONE;
	int rc;

	rc = ahci_pmp_link_recover(link, &cls);
	if (rc == -ENODEV || cls == ATA_DEV_NONE)
		return 0; /* empty (or disabled) fan-out port */
	if (rc)
		return rc;
	switch (cls) {
	case ATA_DEV_PMP:
		pmp_warn(port, link->pmp,
			 "a port multiplier behind a port multiplier is not "
			 "supported");
		return -EOPNOTSUPP;
	case ATA_DEV_SEMB:
		pmp_info(port, link->pmp,
			 "enclosure management bridge: not used");
		return 0;
	case ATA_DEV_UNKNOWN:
		pmp_info(port, link->pmp,
			 "unknown signature %08x, assuming ATA", link->sig);
		cls = ATA_DEV_ATA;
		break;
	default:
		break;
	}
	return ahci_link_attach(link, cls);
}

/* Read the GSCR identity and enable what the driver uses.  Fills the
 * port's pmp_gscr_* fields and nr_pmp_links. */
static int ahci_pmp_configure(struct ahci_port *port, int quiet)
{
	const struct ahci_pmp_quirk *q;
	uint32_t prod = 0, rev = 0, info = 0, feat = 0, feat_en = 0;
	uint32_t nr;
	int rc;

	rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_PROD_ID,
			   &prod);
	if (rc)
		return rc;
	rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_REV, &rev);
	if (rc)
		return rc;
	rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_PORT_INFO,
			   &info);
	if (rc)
		return rc;
	if (ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_FEAT,
			  &feat) != 0)
		feat = 0;
	if (ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_FEAT_EN,
			  &feat_en) != 0)
		feat_en = 0;

	nr = info & 0x0F;
	q = ahci_pmp_find_quirk(prod);
	if (q && q->max_ports && nr > q->max_ports)
		nr = q->max_ports;
	if (nr == 0 || nr > AHCI_PMP_MAX_LINKS) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "invalid number of fan-out ports (GSCR 2 = %08x)",
			 info);
		return -EINVAL;
	}

	port->pmp_gscr_prod_id = prod;
	port->pmp_gscr_rev = rev;
	port->pmp_gscr_feat = feat;
	port->pmp_gscr_feat_en = feat_en;
	port->nr_pmp_links = nr;

	/* Asynchronous notification: the multiplier can notify (the HBA
	 * always takes a notification -- PxSNTF or the received FIS).  It
	 * is enabled by recovery once the fan-out links are reset and
	 * probed. */
	port->pmp_an = (feat & SATA_PMP_FEAT_NOTIFY) != 0;

	/* Monitor PhyRdy changes on the fan-out ports: SError.N of a port
	 * shows in the error summary (GSCR 32). */
	rc = ahci_pmp_write(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_ERROR_EN,
			    AHCI_SERR_DIAG_N);
	if (rc) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "failed to write GSCR_ERROR_EN (%d)", rc);
		return -EIO;
	}

	/* Silicon Image 3726/3826: no early R_OK -- with several ports busy
	 * it rarely drops a deferred R_OK meant for the host, and every
	 * drive behind it then times out. */
	if ((prod & 0xFFFF) == 0x1095 &&
	    ((prod >> 16) == 0x3726 || (prod >> 16) == 0x3826)) {
		uint32_t reg = 0;

		rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT,
				   AHCI_PMP_GSCR_SII_POL, &reg);
		if (rc == 0)
			rc = ahci_pmp_write(port, ATA_PMP_CTRL_PORT,
					    AHCI_PMP_GSCR_SII_POL, reg & ~0x1u);
		if (rc) {
			pmp_warn(port, ATA_PMP_CTRL_PORT,
				 "failed to clear the Sil3x26 early R_OK (%d)",
				 rc);
			return -EIO;
		}
	}

	if (!quiet) {
		pmp_info(port, ATA_PMP_CTRL_PORT,
			 "Port Multiplier %s, 0x%04x:0x%04x r%u, %u ports, "
			 "feat 0x%x/0x%x%s%s",
			 ahci_pmp_spec_rev(rev), prod & 0xFFFF, prod >> 16,
			 (rev >> 8) & 0xFF, nr, feat_en, feat,
			 q ? ", " : "", q ? q->name : "");
		if (!port->pmp_an)
			pmp_info(port, ATA_PMP_CTRL_PORT,
				 "asynchronous notification not supported, "
				 "hot-plug will not work on fan-out ports (use "
				 "warm-plug)");
	}
	return 0;
}

/* Recovery runs with the multiplier's notification off: PHY event
 * notifications would disturb the resets. */
static int ahci_pmp_notify_off(struct ahci_port *port)
{
	int rc;

	if (!(port->pmp_gscr_feat_en & SATA_PMP_FEAT_NOTIFY))
		return 0;
	port->pmp_gscr_feat_en &= ~SATA_PMP_FEAT_NOTIFY;
	rc = ahci_pmp_write(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_FEAT_EN,
			    port->pmp_gscr_feat_en);
	if (rc)
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "failed to disable NOTIFY (%d)", rc);
	return rc ? -EIO : 0;
}

/* The end of recovery: SNotification cleared; notification on again
 * where the multiplier can notify -- unless a fan-out link uses link
 * power management, with which hot-plug does not work anyway. */
static int ahci_pmp_notify_on(struct ahci_port *port)
{
	struct ahci_hba *hba = port->hba;
	int rc;

	if (hba->caps.ssntf && !(hba->quirks & AHCI_Q_NO_SNTF)) {
		uint64_t fl;
		uint32_t sntf;

		spin_lock_irqsave(&port->lock, &fl);
		sntf = ahci_port_readl(port, AHCI_PxSNTF);
		ahci_port_writel(port, AHCI_PxSNTF, sntf);
		spin_unlock_irqrestore(&port->lock, fl);
	}
	for (uint32_t i = 0; i < port->nr_pmp_links && i < AHCI_PMP_MAX_LINKS;
	     i++)
		if (port->pmp_links[i].lpm_policy > AHCI_LPM_MAX_PERFORMANCE)
			return 0;
	if (!port->pmp_an)
		return 0;
	port->pmp_gscr_feat_en |= SATA_PMP_FEAT_NOTIFY;
	rc = ahci_pmp_write(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_FEAT_EN,
			    port->pmp_gscr_feat_en);
	if (rc) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "failed to write PMP_FEAT_EN (%d)", rc);
		return -EIO;
	}
	return 0;
}

static int ahci_pmp_handle_link_fail(struct ahci_port *port,
				     struct ahci_link *link);
static int ahci_pmp_recover_finish(struct ahci_port *port);

/* After recovery: the links whose PHY status changed meanwhile (error
 * summary).  Each consumes one of its tries and is recovered again; out
 * of tries it is left.  Returns the links to recover again, or -errno
 * through *err when the summary cannot be read. */
static uint32_t ahci_pmp_recheck(struct ahci_port *port, int *err)
{
	uint32_t gerr = 0, again = 0;

	*err = 0;
	if (ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_ERROR,
			  &gerr) != 0) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "failed to read PMP_GSCR_ERROR");
		*err = -EIO;
		return 0;
	}
	gerr &= ahci_pmp_link_mask(port);
	for (uint32_t i = 0; i < port->nr_pmp_links; i++) {
		struct ahci_link *link = &port->pmp_links[i];

		if (!(gerr & (1u << i)))
			continue;
		if (ahci_pmp_handle_link_fail(port, link)) {
			again |= 1u << i;
		} else {
			pmp_warn(port, (int)i,
				 "PHY status changed but maxed out on retries, giving up");
			pmp_warn(port, (int)i,
				 "manually issue a rescan to resume this link");
		}
	}
	if (again)
		pmp_info(port, ATA_PMP_CTRL_PORT,
			 "SError.N set for some ports, repeating recovery");
	return again;
}

int ahci_pmp_attach(struct ahci_port *port)
{
	struct ahci_hba *hba;
	uint64_t fl;
	int rc, found = 0;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	hba = port->hba;
	if (!hba->caps.spm || (hba->quirks & AHCI_Q_NO_PMP))
		return -EOPNOTSUPP;
	if (port->pmp_attached) {
		WARN_ON_ONCE(1);
		return ahci_pmp_revalidate(port);
	}

	/* 1. Tell the HBA (PMA) -- it then checks PM port fields of the
	 * FISes it receives. */
	rc = ahci_pmp_set_pma(port, 1);
	if (rc) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "could not set PxCMD.PMA (%d)", rc);
		return rc;
	}
	/* The PMP register helpers check pmp_attached for PSCR access. */
	spin_lock_irqsave(&port->lock, &fl);
	port->pmp_attached = 1;
	port->nr_pmp_links = 0;
	port->host_link.class = ATA_DEV_PMP;
	spin_unlock_irqrestore(&port->lock, fl);
	/* A new multiplier online: its links get their recovery tries. */
	ahci_pmp_eh_begin(port);

	/* 2. Identity, features. */
	rc = ahci_pmp_configure(port, 0);
	if (rc) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "GSCR access failed (%d), multiplier not used", rc);
		spin_lock_irqsave(&port->lock, &fl);
		port->pmp_attached = 0;
		port->nr_pmp_links = 0;
		spin_unlock_irqrestore(&port->lock, fl);
		(void)ahci_pmp_set_pma(port, 0);
		return rc;
	}

	/* 3. FIS-based switching where the HBA offers it. */
	if (port->fbs_capable)
		(void)ahci_fbs_enable(port);

	/* PHY event notification would disturb the link resets: off while
	 * the fan-out links are probed. */
	rc = ahci_pmp_notify_off(port);
	if (rc) {
		ahci_pmp_detach(port);
		return rc;
	}

	/* 4. Every fan-out link: reset, classify, probe. */
	for (uint32_t i = 0; i < port->nr_pmp_links; i++) {
		struct ahci_link *link = &port->pmp_links[i];

		link->dev = NULL;
		link->class = ATA_DEV_NONE;
		link->state = AHCI_LINK_OFFLINE;
		link->eh_pending = 0;
		link->lpm_policy = port->lpm_policy;
		if (ahci_pmp_quirk_flags(port) & PMPQ_NO_LPM)
			link->lpm_policy = AHCI_LPM_MAX_PERFORMANCE;
		if (!link->sysfs_added) {
			if (ahci_sysfs_add_link(link) == 0)
				link->sysfs_added = 1;
		}
		rc = ahci_pmp_probe_link(port, link);
		if (rc)
			pmp_warn(port, (int)i, "probe failed (%d)", rc);
		else if (link->dev)
			found++;
	}

	/* 5. Notification on again (SNotification cleared), and the links
	 * whose PHY status changed meanwhile recovered again. */
	rc = ahci_pmp_recover_finish(port);
	if (rc) {
		ahci_pmp_detach(port);
		return rc;
	}
	pmp_info(port, ATA_PMP_CTRL_PORT, "%d device%s behind the multiplier%s",
		 found, found == 1 ? "" : "s",
		 port->fbs_enabled ? " (FBS)" : " (command-based switching)");
	return 0;
}

void ahci_pmp_detach(struct ahci_port *port)
{
	uint64_t fl;

	if (!port)
		return;
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	if (!port->pmp_attached)
		return;

	/* Nothing may still run on the port while devices go away. */
	if (port->active || port->deferred_qc) {
		(void)ahci_port_stop_engine_abort(port);
		ahci_port_fail_all(port, -ENODEV);
	}
	for (uint32_t i = 0; i < AHCI_PMP_MAX_LINKS; i++) {
		struct ahci_link *link = &port->pmp_links[i];

		if (link->dev)
			ahci_link_detach(link);
		link->class = ATA_DEV_NONE;
		link->state = AHCI_LINK_OFFLINE;
		link->eh_link_disabled = 0;
	}
	if (port->fbs_enabled)
		(void)ahci_fbs_disable(port);

	(void)ahci_port_stop_engine(port);
	spin_lock_irqsave(&port->lock, &fl);
	ahci_port_writel(port, AHCI_PxCMD,
			 ahci_port_readl(port, AHCI_PxCMD) &
				 ~(AHCI_PxCMD_PMA | AHCI_PxCMD_ICC_MASK |
				   AHCI_PxCMD_CLO));
	port->pmp_attached = 0;
	port->nr_pmp_links = 0;
	if (port->host_link.class == ATA_DEV_PMP)
		port->host_link.class = ATA_DEV_NONE;
	spin_unlock_irqrestore(&port->lock, fl);
	pmp_info(port, ATA_PMP_CTRL_PORT, "port multiplier detached");
}

static void ahci_pmp_link_event(struct ahci_port *port,
				struct ahci_link *link);

/* The end of a recovery pass: notification back on (SNotification
 * cleared), then the links whose PHY status changed meanwhile recovered
 * again -- each at the cost of one of its tries -- in another pass with
 * notification off. */
static int ahci_pmp_recover_finish(struct ahci_port *port)
{
	for (;;) {
		uint32_t again;
		int rc;

		rc = ahci_pmp_notify_on(port);
		if (rc)
			return rc;
		again = ahci_pmp_recheck(port, &rc);
		if (rc)
			return rc;
		if (!again)
			return 0;
		rc = ahci_pmp_notify_off(port);
		if (rc)
			return rc;
		for (uint32_t i = 0; i < port->nr_pmp_links; i++)
			if (again & (1u << i))
				ahci_pmp_link_event(port, &port->pmp_links[i]);
	}
}

/* Lower a fan-out link's speed limit by one generation (effective at its
 * next COMRESET). */
static void ahci_pmp_link_down_spd(struct ahci_link *link)
{
	uint8_t cur = link->speed_limit ? link->speed_limit
					: (uint8_t)AHCI_SSTS_SPD(link->sstatus);

	if (cur <= AHCI_SSTS_SPD_GEN1 || cur > AHCI_SSTS_SPD_GEN3)
		return;
	link->speed_limit = (uint8_t)(cur - 1);
	link->n_speed_downs++;
}

int ahci_pmp_revalidate(struct ahci_port *port)
{
	uint32_t prod = 0, old_prod, old_nr;
	int rc;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);
	if (!port->pmp_attached)
		return -ENODEV;

	rc = ahci_pmp_set_pma(port, 1);
	if (rc)
		return rc;
	rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_PROD_ID,
			   &prod);
	if (rc) {
		pmp_info(port, ATA_PMP_CTRL_PORT,
			 "multiplier does not answer (%d)", rc);
		return -ENODEV;
	}
	if (prod != port->pmp_gscr_prod_id) {
		pmp_info(port, ATA_PMP_CTRL_PORT,
			 "a different multiplier (0x%04x:0x%04x, was "
			 "0x%04x:0x%04x)",
			 prod & 0xFFFF, prod >> 16,
			 port->pmp_gscr_prod_id & 0xFFFF,
			 port->pmp_gscr_prod_id >> 16);
		return -ENODEV;
	}
	old_prod = port->pmp_gscr_prod_id;
	old_nr = port->nr_pmp_links;
	rc = ahci_pmp_configure(port, 1);
	if (rc)
		return -ENODEV;
	if (port->nr_pmp_links != old_nr || port->pmp_gscr_prod_id != old_prod)
		return -ENODEV;

	/* After a host link reset or a controller reset FBS is off. */
	if (port->fbs_capable && !port->fbs_enabled)
		(void)ahci_fbs_enable(port);

	/* PHY event notification would disturb the link resets: off. */
	rc = ahci_pmp_notify_off(port);
	if (rc)
		return rc;

	/* Disabled links: a quick reset so their PhyRdy change works; not
	 * recovered otherwise. */
	rc = ahci_pmp_handle_disabled_links(port);
	if (rc)
		return rc;

	for (uint32_t i = 0, dev_tries = AHCI_EH_DEV_TRIES;
	     i < port->nr_pmp_links; i++, dev_tries = AHCI_EH_DEV_TRIES) {
		struct ahci_link *link = &port->pmp_links[i];
		enum ata_class cls = ATA_DEV_NONE;

		if (link->eh_link_disabled)
			continue;
	again:
		rc = ahci_pmp_link_recover(link, &cls);
		if (rc == -ENODEV || cls == ATA_DEV_NONE) {
			if (link->dev) {
				pmp_info(port, (int)i, "device gone");
				port->stats.hotplug_remove++;
				ahci_link_detach(link);
			}
			continue;
		}
		if (rc) {
			pmp_warn(port, (int)i, "link reset failed (%d)", rc);
			continue;
		}
		if (link->dev) {
			rc = ata_dev_revalidate(link->dev);
			if (rc == 0)
				continue;
			if (rc != -ENODEV) {
				/* Not known to be another device: kept, the
				 * link reset and the device revalidated again
				 * while it has tries (the last at a lower link
				 * speed); out of them it is disabled. */
				pmp_info(port, (int)i,
					 "revalidation failed (%d)", rc);
				if (--dev_tries == 1)
					ahci_pmp_link_down_spd(link);
				if (dev_tries)
					goto again;
				pmp_warn(port, (int)i,
					 "device disabled after %d tries",
					 AHCI_EH_DEV_TRIES);
				link->dev->flags |= ATA_DFLAG_DISABLED;
				continue;
			}
			pmp_info(port, (int)i, "device changed or gone (%d)",
				 rc);
			ahci_link_detach(link);
		}
		if (cls == ATA_DEV_UNKNOWN)
			cls = ATA_DEV_ATA;
		if (cls == ATA_DEV_ATA || cls == ATA_DEV_ATAPI ||
		    cls == ATA_DEV_ZAC) {
			rc = ahci_link_attach(link, cls);
			if (rc)
				pmp_warn(port, (int)i, "attach failed (%d)",
					 rc);
		}
	}
	return ahci_pmp_recover_finish(port);
}

/* ==========================================================================
 * Notification and fan-out link events
 * ========================================================================== */

/* One fan-out link reported something (SNotification / GSCR_ERROR): look
 * at its SError and act.  The port is quiesced by the caller. */
static void ahci_pmp_link_event(struct ahci_port *port,
				struct ahci_link *link)
{
	uint32_t serr = 0;
	int rc;

	if (ahci_pmp_scr_read(link, AHCI_SCR_ERROR, &serr) != 0)
		return;
	if (serr)
		(void)ahci_pmp_scr_write(link, AHCI_SCR_ERROR, serr);
	pmp_dbg(port, link->pmp, "event: SError %08x", serr);

	/* A disabled link is not recovered (ahci_pmp_handle_notify enabled
	 * it again when it reported a PhyRdy change). */
	if (link->eh_link_disabled)
		return;

	if (serr & AHCI_SERR_HOTPLUG) {
		/* Hot-plug on the fan-out port: reset the link with the
		 * hot-plug debounce timing; what the reset finds decides --
		 * a new device attached, a vanished one detached, a present
		 * one revalidated (below). */
		link->eh_hotplugged = 1;
		if (!link->dev) {
			rc = ahci_pmp_probe_link(port, link);
			link->eh_hotplugged = 0;
			if (rc == 0 && link->dev) {
				pmp_info(port, link->pmp, "device inserted");
				port->stats.hotplug_add++;
			} else if (rc && rc != -ENODEV) {
				pmp_warn(port, link->pmp,
					 "probe of the new device failed (%d)",
					 rc);
			}
			return;
		}
	} else if (serr & (AHCI_SERR_DIAG_C | AHCI_SERR_DIAG_D |
			   AHCI_SERR_DIAG_B | AHCI_SERR_DIAG_H |
			   AHCI_SERR_ERR_C | AHCI_SERR_ERR_T |
			   AHCI_SERR_ERR_P | AHCI_SERR_DIAG_I |
			   AHCI_SERR_DIAG_S | AHCI_SERR_DIAG_T)) {
		ahci_link_note_error(link);
		/* Transient data errors are retried by the commands' own
		 * error handling; persistent ones need a link reset. */
		if (!(serr & (AHCI_SERR_ERR_C | AHCI_SERR_ERR_P |
			      AHCI_SERR_DIAG_I | AHCI_SERR_DIAG_S |
			      AHCI_SERR_DIAG_T)) &&
		    link->spd_err_count < AHCI_SPEED_DOWN_ERRS)
			return;
		pmp_info(port, link->pmp,
			 "link errors (SError %08x), resetting the link",
			 serr);
	} else {
		return; /* nothing that needs action */
	}

	if (!link->dev) {
		(void)ahci_pmp_probe_link(port, link);
		return;
	}
	{
		enum ata_class cls = ATA_DEV_NONE;

		rc = ahci_pmp_link_recover(link, &cls);
		link->eh_hotplugged = 0;
		if (rc == -ENODEV || cls == ATA_DEV_NONE) {
			pmp_info(port, link->pmp, "device gone after reset");
			port->stats.hotplug_remove++;
			ahci_link_detach(link);
			return;
		}
		if (rc)
			return;
		rc = ata_dev_revalidate(link->dev);
		if (rc == 0)
			return;
		pmp_info(port, link->pmp,
			 "device changed or failed revalidation (%d)", rc);
		ahci_link_detach(link);
		if (cls == ATA_DEV_UNKNOWN)
			cls = ATA_DEV_ATA;
		if (cls == ATA_DEV_ATA || cls == ATA_DEV_ATAPI ||
		    cls == ATA_DEV_ZAC)
			(void)ahci_link_attach(link, cls);
	}
}

/* The multiplier notified (its control port sent a Set Device Bits FIS
 * with the N bit, or the HBA cannot say who did).  PxSNTF was read and
 * cleared by the interrupt handler, which also scheduled the media check
 * of a notifying ATAPI drive: what is left is finding the fan-out links
 * whose PHY changed -- the multiplier's error summary names them. */
int ahci_pmp_handle_notify(struct ahci_port *port)
{
	uint32_t gerr = 0, links, token = 0;
	int rc, handled = 0;

	BUG_ON(!port);
	ahci_might_sleep();
	ahci_pmp_assert_eh_held(port);

	if (!port->pmp_attached)
		return 0;

	rc = ahci_pmp_quiesce(port, &token);
	if (rc) {
		ahci_port_unquiesce(port, token);
		return rc;
	}

	/* The error summary names the fan-out ports with SError bits set. */
	rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT, SATA_PMP_GSCR_ERROR,
			   &gerr);
	if (rc) {
		pmp_warn(port, ATA_PMP_CTRL_PORT,
			 "GSCR error summary unreadable (%d): resetting the "
			 "port",
			 rc);
		ahci_port_unquiesce(port, token);
		ahci_eh_schedule(port, AHCI_EH_RESET_HARD);
		return rc;
	}
	links = gerr & ahci_pmp_link_mask(port);
	/* No link named: look at every link (one PSCR read each). */
	if (!links)
		links = ahci_pmp_link_mask(port);
	pmp_dbg(port, ATA_PMP_CTRL_PORT, "notify: GSCR_ERROR %08x", gerr);

	/* The multiplier itself first: still the same one (a quick look at
	 * its product id)? */
	{
		uint32_t prod = 0;

		rc = ahci_pmp_read(port, ATA_PMP_CTRL_PORT,
				   SATA_PMP_GSCR_PROD_ID, &prod);
		if (rc == 0 && prod != port->pmp_gscr_prod_id) {
			pmp_warn(port, ATA_PMP_CTRL_PORT,
				 "PMP product ID mismatch");
			rc = -ENODEV;
		}
		if (rc)
			goto pmp_fail;
	}

	/* PHY event notification would disturb the link resets: off while
	 * recovery runs. */
	rc = ahci_pmp_notify_off(port);
	if (rc)
		goto pmp_fail;

	/* A disabled link that reports a PhyRdy change (only SError.N
	 * counts there: its X bit stays set for plugging) is enabled again
	 * and handled as a hot-plug event; the links still disabled get
	 * their quick reset, so that their next PhyRdy change is seen. */
	for (uint32_t i = 0; i < port->nr_pmp_links; i++) {
		struct ahci_link *link = &port->pmp_links[i];
		uint32_t serr = 0;

		if (!(links & (1u << i)) || !link->eh_link_disabled)
			continue;
		if (ahci_pmp_scr_read(link, AHCI_SCR_ERROR, &serr) == 0 &&
		    (serr & AHCI_SERR_DIAG_N)) {
			pmp_info(port, (int)i, "PhyRdy changed: enabling the link");
			link->eh_link_disabled = 0;
			/* the device disabled with the link goes; what is
			 * there now is probed afresh */
			if (link->dev &&
			    (link->dev->flags & ATA_DFLAG_DISABLED))
				ahci_link_detach(link);
		}
	}
	rc = ahci_pmp_handle_disabled_links(port);
	if (rc)
		goto pmp_fail;

	for (uint32_t i = 0; i < port->nr_pmp_links; i++) {
		if (!(links & (1u << i)))
			continue;
		ahci_pmp_link_event(port, &port->pmp_links[i]);
		handled++;
	}
	/* notification back on, links that changed meanwhile again */
	rc = ahci_pmp_recover_finish(port);
	if (rc)
		goto pmp_fail;

	(void)ahci_pmp_ensure_engine(port);
	ahci_port_unquiesce(port, token);
	return handled;

pmp_fail:
	/* The multiplier itself failed: it is recovered by a reset (with
	 * its tries). */
	(void)ahci_pmp_ensure_engine(port);
	ahci_port_unquiesce(port, token);
	ahci_eh_schedule(port, AHCI_EH_RESET_HARD);
	return rc;
}
