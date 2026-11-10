// LikeOS ATA layer - device power management
//
// The ATA power states and how a device moves between them:
//
//   ACTIVE / IDLE   the device answers media access at once (IDLE: some
//                   electronics off, heads possibly unloaded)
//   STANDBY         media stopped; the next media access spins it up
//                   (seconds for a rotating disk), or SET FEATURES 07h
//                   when it powered up in standby and requires that
//   SLEEP           the interface itself is down: only a reset (software
//                   reset or COMRESET, which error handling performs)
//                   brings the device back
//
// Commands: CHECK POWER MODE (E5h, the answer in COUNT), IDLE / STANDBY
// with a timer (E3h/E2h, COUNT = the standby timer encoding below), IDLE
// IMMEDIATE (E1h, optionally with the UNLOAD feature that parks the heads)
// and STANDBY IMMEDIATE (E0h), SLEEP (E6h).  APM levels are set through
// SET FEATURES (ata_features.c): 01h..7Fh allow spin-down, 80h..FEh do
// not, FEh is the highest performance.
//
// On top of that the device-level transitions the block layer asks for:
//   suspend   flush the write cache, STANDBY IMMEDIATE;
//   resume    wake (reset out of SLEEP), re-IDENTIFY, check it is still the
//             same drive (a swapped drive is taken offline and probed
//             afresh), re-apply the configuration;
//   shutdown  flush; on power-off/halt also STANDBY IMMEDIATE so a
//             rotating disk parks its heads in an orderly way and an SSD
//             records a clean power loss.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata_dev_internal.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* IDLE IMMEDIATE with UNLOAD FEATURE: FEATURE 44h, LBA 554E4Ch ("UNL");
 * the device answers with C4h in LBA low when the heads were unloaded. */
#define ATA_UNLOAD_FEATURE 0x44
#define ATA_UNLOAD_LBAL 0x4C
#define ATA_UNLOAD_LBAM 0x4E
#define ATA_UNLOAD_LBAH 0x55
#define ATA_UNLOAD_DONE 0xC4

/* How long a shutdown waits for error handling to let go of the device
 * before it flushes regardless. */
#define ATA_SHUTDOWN_LOCK_WAIT_MS 5000

/* ==========================================================================
 * Names
 * ========================================================================== */

const char *ata_power_state_name(enum ata_power_state st)
{
	switch (st) {
	case ATA_PWR_ACTIVE: return "active";
	case ATA_PWR_IDLE: return "idle";
	case ATA_PWR_STANDBY: return "standby";
	case ATA_PWR_SLEEP: return "sleeping";
	default: return "unknown";
	}
}

const char *ata_chk_power_name(uint8_t mode)
{
	switch (mode) {
	case ATA_CHK_PWR_STANDBY: return "standby";
	case ATA_CHK_PWR_STANDBY_Y: return "standby (standby_y)";
	case 0x40: return "active, NV cache spun down";
	case 0x41: return "active, NV cache spun up";
	case ATA_CHK_PWR_IDLE: return "idle";
	case ATA_CHK_PWR_IDLE_A: return "idle (idle_a)";
	case ATA_CHK_PWR_IDLE_B: return "idle (idle_b)";
	case ATA_CHK_PWR_IDLE_C: return "idle (idle_c)";
	case ATA_CHK_PWR_ACTIVE: return "active/idle";
	default: return "reserved";
	}
}

/* What an APM level means (for logs and the sysfs view). */
static const char *ata_apm_level_name(uint8_t level)
{
	if (level == 0)
		return "disabled";
	if (level == 1)
		return "minimum power, spin-down";
	if (level < 0x80)
		return "power saving with spin-down";
	if (level < 0xFE)
		return "power saving without spin-down";
	if (level == 0xFE)
		return "maximum performance";
	return "reserved";
}

/* ==========================================================================
 * The standby timer encoding (pure)
 *
 *   0          timer disabled
 *   1..240     n x 5 seconds (5 s .. 20 min)
 *   241..251   (n - 240) x 30 minutes (30 min .. 5.5 h)
 *   252        21 minutes
 *   253        vendor-defined (8 .. 12 h)
 *   254        reserved
 *   255        21 minutes 15 seconds
 *
 * Seconds are rounded UP to the next representable period, so a device is
 * never told to stop sooner than asked; beyond 5.5 hours the longest
 * defined period is used.
 * ========================================================================== */

uint8_t ata_standby_timer_encode(uint32_t seconds)
{
	if (seconds == 0)
		return 0;
	if (seconds <= 240u * 5u)
		return (uint8_t)((seconds + 4) / 5);
	if (seconds <= 21u * 60u)
		return 252;
	if (seconds <= 21u * 60u + 15u)
		return 255;
	if (seconds <= 11u * 30u * 60u)
		return (uint8_t)(240 + (seconds + 1799) / 1800);
	return 251;
}

/* The decoded length of a timer value, seconds (0 off, ~0 vendor). */
static uint32_t ata_standby_timer_seconds(uint8_t v)
{
	if (v == 0)
		return 0;
	if (v <= 240)
		return (uint32_t)v * 5;
	if (v <= 251)
		return (uint32_t)(v - 240) * 1800;
	if (v == 252)
		return 21 * 60;
	if (v == 255)
		return 21 * 60 + 15;
	return 0xFFFFFFFFu;
}

/* ==========================================================================
 * Commands
 * ========================================================================== */

static int ata_pm_cmd(struct ata_device *dev, uint8_t command, uint8_t count,
		      uint32_t timeout_ms, struct ata_taskfile *out)
{
	struct ata_taskfile tf;
	int rc;

	kmemset(&tf, 0, sizeof(tf));
	tf.command = command;
	tf.nsect = count;
	tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_ISADDR;
	rc = ata_exec_nodata(dev, &tf, timeout_ms, ATA_QCFLAG_QUIET);
	if (out)
		*out = tf;
	if (rc == -EIO && ata_tf_is_abort_only(&tf))
		return -EOPNOTSUPP;
	if (rc)
		ata_dev_printk(dev, "%s failed (%d, status %02x error %02x)\n",
			       ata_cmd_name(command), rc, tf.status, tf.error);
	return rc;
}

static int ata_pm_usable(struct ata_device *dev)
{
	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	return 0;
}

int ata_check_power_mode(struct ata_device *dev, uint8_t *mode)
{
	struct ata_taskfile tf;
	int rc;

	rc = ata_pm_usable(dev);
	if (rc)
		return rc;
	if (!mode)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	/* A sleeping device does not answer anything; asking would only
	 * time out.  Its media is stopped: report standby. */
	if (dev->flags & ATA_DFLAG_SLEEPING) {
		*mode = ATA_CHK_PWR_STANDBY;
		return 0;
	}
	rc = ata_pm_cmd(dev, ATA_CMD_CHK_POWER, 0, 0, &tf);
	if (rc)
		return rc;
	*mode = tf.nsect;
	switch (tf.nsect) {
	case ATA_CHK_PWR_STANDBY:
	case ATA_CHK_PWR_STANDBY_Y:
	case 0x40:
		dev->power_state = ATA_PWR_STANDBY;
		break;
	case ATA_CHK_PWR_IDLE:
	case ATA_CHK_PWR_IDLE_A:
	case ATA_CHK_PWR_IDLE_B:
	case ATA_CHK_PWR_IDLE_C:
		dev->power_state = ATA_PWR_IDLE;
		break;
	case ATA_CHK_PWR_ACTIVE:
	case 0x41:
		dev->power_state = ATA_PWR_ACTIVE;
		break;
	default:
		ata_dev_printk(dev, "CHECK POWER MODE: reserved value %02xh\n",
			       tf.nsect);
		break;
	}
	return 0;
}

/* STANDBY IMMEDIATE without the locking / flushing wrapper. */
static int ata_standby_immediate_raw(struct ata_device *dev)
{
	int rc = ata_pm_cmd(dev, ATA_CMD_STANDBYNOW1, 0, ATA_TMOUT_STANDBY_MS,
			    NULL);

	if (rc == 0)
		dev->power_state = ATA_PWR_STANDBY;
	return rc;
}

int ata_standby_immediate(struct ata_device *dev)
{
	int took, rc;

	rc = ata_pm_usable(dev);
	if (rc)
		return rc;
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_SLEEPING)
		return 0; /* deeper than standby already */
	took = ata_mgmt_enter(dev);
	/* Never stop the media with data only in the cache. */
	if (dev->class != ATA_DEV_ATAPI && (dev->flags & ATA_DFLAG_WCACHE_ON)) {
		rc = ata_flush_cache(dev);
		if (rc && rc != -EOPNOTSUPP)
			ata_dev_printk(dev, "flush before standby failed (%d)\n",
				       rc);
	}
	rc = ata_standby_immediate_raw(dev);
	ata_mgmt_exit(dev, took);
	return rc;
}

int ata_idle_immediate(struct ata_device *dev, int unload_heads)
{
	struct ata_taskfile tf;
	int took, rc;

	rc = ata_pm_usable(dev);
	if (rc)
		return rc;
	if (unload_heads && !(dev->flags & ATA_DFLAG_UNLOAD))
		return -EOPNOTSUPP;
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_SLEEPING)
		return -EAGAIN; /* needs a reset first: ata_dev_wake */
	took = ata_mgmt_enter(dev);
	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_IDLEIMMEDIATE;
	tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_ISADDR;
	if (unload_heads) {
		tf.feature = ATA_UNLOAD_FEATURE;
		tf.lbal = ATA_UNLOAD_LBAL;
		tf.lbam = ATA_UNLOAD_LBAM;
		tf.lbah = ATA_UNLOAD_LBAH;
	}
	rc = ata_exec_nodata(dev, &tf, ATA_TMOUT_SPINUP_MS, ATA_QCFLAG_QUIET);
	if (rc == -EIO && ata_tf_is_abort_only(&tf))
		rc = -EOPNOTSUPP;
	if (rc == 0) {
		dev->power_state = ATA_PWR_IDLE;
		if (unload_heads && tf.lbal != ATA_UNLOAD_DONE)
			ata_dev_printk(dev, "heads not reported unloaded (LBA "
					    "low %02xh)\n",
				       tf.lbal);
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

/* STANDBY / IDLE with the timer (E2h / E3h). */
static int ata_timer_cmd(struct ata_device *dev, uint8_t command,
			 uint32_t seconds)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	uint8_t v = ata_standby_timer_encode(seconds);
	int took, rc;

	rc = ata_pm_usable(dev);
	if (rc)
		return rc;
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_SLEEPING)
		return -EAGAIN;
	took = ata_mgmt_enter(dev);
	if (command == ATA_CMD_STANDBY && dev->class != ATA_DEV_ATAPI &&
	    (dev->flags & ATA_DFLAG_WCACHE_ON))
		(void)ata_flush_cache(dev);
	rc = ata_pm_cmd(dev, command, v,
			command == ATA_CMD_STANDBY ? ATA_TMOUT_STANDBY_MS :
						     ATA_TMOUT_SPINUP_MS,
			NULL);
	if (rc == 0) {
		dev->standby_timer = v;
		if (p)
			p->policy.standby_timer = v;
		dev->power_state = command == ATA_CMD_STANDBY ? ATA_PWR_STANDBY :
								ATA_PWR_IDLE;
		if (v)
			ata_dev_printk(dev, "standby timer %u (%u s)%s\n", v,
				       ata_standby_timer_seconds(v),
				       (dev->flags & ATA_DFLAG_APM) &&
						       dev->apm_level >= 0x80 ?
					       ", but the APM level forbids "
					       "spin-down" :
					       "");
		else
			ata_dev_printk(dev, "standby timer off\n");
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

int ata_standby(struct ata_device *dev, uint32_t seconds)
{
	return ata_timer_cmd(dev, ATA_CMD_STANDBY, seconds);
}

int ata_idle(struct ata_device *dev, uint32_t seconds)
{
	return ata_timer_cmd(dev, ATA_CMD_IDLE, seconds);
}

int ata_sleep(struct ata_device *dev)
{
	int took, rc;

	rc = ata_pm_usable(dev);
	if (rc)
		return rc;
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_SLEEPING)
		return 0;
	/* Let what is queued finish first (outside the EH serialisation, so
	 * error handling stays possible while we wait). */
	if (dev->disk && dev->disk->queue && ata_can_sleep() &&
	    !ata_dev_eh_held(dev))
		(void)blk_queue_drain(dev->disk->queue, ATA_TMOUT_FLUSH_MS);
	took = ata_mgmt_enter(dev);
	if (dev->class != ATA_DEV_ATAPI && (dev->flags & ATA_DFLAG_WCACHE_ON)) {
		rc = ata_flush_cache(dev);
		if (rc && rc != -EOPNOTSUPP) {
			ata_dev_printk(dev, "not sleeping: flush failed (%d)\n",
				       rc);
			goto out;
		}
	}
	rc = ata_pm_cmd(dev, ATA_CMD_SLEEP, 0, ATA_TMOUT_STANDBY_MS, NULL);
	if (rc == 0) {
		dev->flags |= ATA_DFLAG_SLEEPING;
		dev->power_state = ATA_PWR_SLEEP;
		ata_dev_printk(dev, "sleeping (a reset wakes it)\n");
	}
out:
	ata_mgmt_exit(dev, took);
	return rc;
}

int ata_dev_wake(struct ata_device *dev)
{
	uint8_t mode = ATA_CHK_PWR_ACTIVE;
	int took, rc;

	rc = ata_pm_usable(dev);
	if (rc)
		return rc;
	if (ata_can_sleep())
		might_sleep();

	if (dev->flags & ATA_DFLAG_SLEEPING) {
		/* Only a reset ends SLEEP.  The transport's reset runs error
		 * handling, which revalidates the device (and that clears
		 * SLEEPING).  The caller must not hold the EH serialisation
		 * itself: then only a request for one is possible. */
		if (ata_dev_eh_held(dev) || !dev->xport->reset) {
			if (dev->xport->schedule_eh)
				dev->xport->schedule_eh(dev, AHCI_EH_RESET_SOFT);
			return -EAGAIN;
		}
		ata_dev_printk(dev, "waking from SLEEP by reset\n");
		rc = dev->xport->reset(dev, ATA_RESET_SOFT);
		if (rc) {
			ata_dev_printk(dev, "reset to wake failed (%d)\n", rc);
			return rc;
		}
		if (dev->flags & ATA_DFLAG_SLEEPING) {
			/* reset done but no revalidation ran: do it here */
			took = ata_mgmt_enter(dev);
			rc = ata_dev_revalidate(dev);
			ata_mgmt_exit(dev, took);
			if (rc)
				return rc;
		}
		ata_rw_kick(dev);
	}

	took = ata_mgmt_enter(dev);
	if (dev->class == ATA_DEV_ATAPI) {
		dev->power_state = ATA_PWR_ACTIVE;
		ata_mgmt_exit(dev, took);
		return 0;
	}
	rc = ata_check_power_mode(dev, &mode);
	if (rc == 0 && (mode == ATA_CHK_PWR_STANDBY ||
			mode == ATA_CHK_PWR_STANDBY_Y || mode == 0x40)) {
		if ((dev->flags & ATA_DFLAG_PUIS) &&
		    (dev->flags & ATA_DFLAG_SPINUP_REQ)) {
			/* powered up in standby and wants the command */
			rc = ata_set_features(dev, SETFEATURES_SPINUP, 0);
		} else {
			rc = ata_pm_cmd(dev, ATA_CMD_IDLEIMMEDIATE, 0,
					ATA_TMOUT_SPINUP_MS, NULL);
		}
		if (rc == 0)
			ata_dev_printk(dev, "spun up from standby\n");
	} else if (rc == -EOPNOTSUPP) {
		rc = 0; /* no power management commands: always active */
	}
	if (rc == 0)
		dev->power_state = ATA_PWR_ACTIVE;
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ==========================================================================
 * Device suspend / resume / shutdown
 * ========================================================================== */

int ata_dev_suspend(struct ata_device *dev)
{
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return 0; /* nothing left to put to sleep */
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_SUSPENDED)
		return 0;
	took = ata_mgmt_enter(dev);

	if (dev->class != ATA_DEV_ATAPI) {
		/* Everything acknowledged must be on the media before the
		 * device loses power; a failing flush stops the suspend. */
		if (dev->flags & ATA_DFLAG_WCACHE_ON) {
			rc = ata_flush_cache(dev);
			if (rc && rc != -EOPNOTSUPP) {
				ata_dev_printk(dev, "suspend aborted: cache "
						    "flush failed (%d)\n",
					       rc);
				goto out;
			}
		}
		if (!(dev->flags & ATA_DFLAG_SLEEPING)) {
			rc = ata_standby_immediate_raw(dev);
			if (rc && rc != -EOPNOTSUPP)
				ata_dev_printk(dev, "STANDBY IMMEDIATE before "
						    "suspend failed (%d), "
						    "suspending anyway\n",
					       rc);
		}
	}
	dev->flags |= ATA_DFLAG_SUSPENDED;
	ata_dev_printk(dev, "suspended (%s)\n",
		       ata_power_state_name(dev->power_state));
	rc = 0;
out:
	ata_mgmt_exit(dev, took);
	return rc;
}

int ata_dev_resume(struct ata_device *dev)
{
	char model[ATA_ID_PROD_LEN + 1], serial[ATA_ID_SERNO_LEN + 1];
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (ata_can_sleep())
		might_sleep();
	if (!(dev->flags & ATA_DFLAG_SUSPENDED))
		return 0;

	/* Out of SLEEP first (a reset, outside the EH serialisation). */
	if (dev->flags & ATA_DFLAG_SLEEPING) {
		rc = ata_dev_wake(dev);
		if (rc && rc != -EAGAIN)
			ata_dev_printk(dev, "resume: wake failed (%d)\n", rc);
	}

	kstrncpy(model, dev->model, sizeof(model) - 1);
	model[sizeof(model) - 1] = '\0';
	kstrncpy(serial, dev->serial, sizeof(serial) - 1);
	serial[sizeof(serial) - 1] = '\0';

	took = ata_mgmt_enter(dev);
	/* Re-IDENTIFY: still the same drive (model and serial)?  A swapped
	 * drive is taken offline inside and handed to hot-plug handling;
	 * otherwise the configuration (write cache, APM, NCQ, DIPM ...) is
	 * applied again -- whatever the device forgot over the power loss. */
	rc = ata_dev_revalidate(dev);
	if (rc == -ENODEV) {
		ata_dev_printk(dev, "resume: \"%s\" (serial \"%s\") is gone or "
				    "was replaced\n",
			       model, serial);
	} else if (rc) {
		ata_dev_printk(dev, "resume: revalidation failed (%d), will "
				    "retry on the next reset\n",
			       rc);
		dev->flags |= ATA_DFLAG_NEEDS_REVALIDATE;
		rc = 0; /* the device answered before; let I/O decide */
	} else {
		ata_dev_printk(dev, "resumed, %s, APM %s\n",
			       ata_power_state_name(dev->power_state),
			       ata_apm_level_name(dev->apm_level));
	}
	dev->flags &= ~ATA_DFLAG_SUSPENDED;
	ata_mgmt_exit(dev, took);
	ata_rw_kick(dev);
	return rc;
}

/* The EH serialisation for shutdown, but never indefinitely: a port wedged
 * in error handling must not stop the machine from powering off. */
static int ata_shutdown_lock(struct ata_device *dev, int *took)
{
	*took = 0;
	if (ata_dev_eh_held(dev))
		return 0;
	if (dev->xport == &ahci_ata_transport && dev->xport_priv) {
		struct ahci_link *link = ahci_dev_link(dev);

		if (link && link->port) {
			for (uint32_t waited = 0; waited < ATA_SHUTDOWN_LOCK_WAIT_MS;
			     waited += 10) {
				if (ata_mutex_trylock(&link->port->eh_mutex)) {
					ata_eh_ctx_enter(dev);
					*took = 2;
					return 0;
				}
				if (ata_can_sleep()) {
					uint64_t until = hrtimer_now_ns() +
							 10ULL * 1000000ULL;

					while (hrtimer_sleep_until(until, NULL))
						;
				} else {
					lapic_delay_us(10000);
				}
			}
			ata_dev_printk(dev, "error handling busy, shutting the "
					    "device down without it\n");
			return -EBUSY;
		}
	}
	*took = ata_mgmt_enter(dev);
	return 0;
}

static void ata_shutdown_unlock(struct ata_device *dev, int took)
{
	if (took == 2) {
		struct ahci_link *link = ahci_dev_link(dev);

		ata_eh_ctx_exit(dev);
		ata_mutex_unlock(&link->port->eh_mutex);
	} else {
		ata_mgmt_exit(dev, took);
	}
}

int ata_dev_shutdown(struct ata_device *dev, enum blk_shutdown_mode mode)
{
	int took = 0, rc = 0;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED))
		return 0;
	if (ata_can_sleep())
		might_sleep();
	(void)ata_shutdown_lock(dev, &took);

	if (dev->class != ATA_DEV_ATAPI &&
	    !(dev->flags & ATA_DFLAG_SLEEPING)) {
		if (dev->flags & ATA_DFLAG_WCACHE_ON) {
			rc = ata_flush_cache(dev);
			if (rc && rc != -EOPNOTSUPP)
				ata_dev_printk(dev, "shutdown: cache flush "
						    "FAILED (%d), data may be "
						    "lost\n",
					       rc);
			else
				rc = 0;
		}
		if (mode == BLK_SHUTDOWN_POWEROFF || mode == BLK_SHUTDOWN_HALT) {
			int r2 = ata_standby_immediate_raw(dev);

			if (r2 && r2 != -EOPNOTSUPP)
				ata_dev_printk(dev, "shutdown: STANDBY IMMEDIATE "
						    "failed (%d)\n",
					       r2);
		}
	}
	ata_dev_printk(dev, "shut down for %s\n",
		       mode == BLK_SHUTDOWN_REBOOT ? "reboot" :
		       mode == BLK_SHUTDOWN_HALT   ? "halt" :
						     "power-off");
	ata_shutdown_unlock(dev, took);
	return rc;
}
