// LikeOS ATA layer - SET FEATURES, logs, diagnostics, device configuration
//
// Everything that changes how a device behaves rather than what it
// stores:
//
//   * SET FEATURES (EFh) and its subcommands -- volatile write cache
//     (02h/82h), read look-ahead (AAh/55h), transfer mode (03h), advanced
//     power management (05h/85h), acoustic management (42h/C2h), power-up
//     in standby (06h/86h) and the spin-up that follows it (07h), the SATA
//     features (10h/90h: DMA setup auto-activate, device-initiated power
//     management, asynchronous notification, software settings
//     preservation ...), sense data reporting (C3h);
//   * SET MULTIPLE MODE;
//   * READ LOG (DMA) EXT and the general purpose log directory;
//   * EXECUTE DEVICE DIAGNOSTIC and its result codes;
//   * READ NATIVE MAX ADDRESS (EXT) / SET MAX ADDRESS (EXT): the host
//     protected area;
//   * the configuration policy applied after IDENTIFY and after every
//     reset, resume and revalidation (ata_dev_configure).  Settings made
//     explicitly (an ioctl turning the write cache off, an APM level) are
//     remembered in the device's policy and survive resets; everything
//     else gets the defaults: highest transfer mode, write cache and
//     look-ahead on, NCQ at min(device, host) depth, DIPM following the
//     link power policy, APM 254, asynchronous notification for packet
//     devices, sense data reporting, software settings preservation, and
//     the security feature set frozen.
//
// After a successful SET FEATURES the cached IDENTIFY words are updated
// (and their checksum recomputed) so HDIO_GET_IDENTITY and the sysfs view
// show the state the device is in without another IDENTIFY.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata_dev_internal.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* Queued TRIM (SEND FPDMA QUEUED with the DSM subcommand) is opt-in:
 * several SSD firmware families corrupt data with it, and the plain DSM
 * TRIM it replaces works everywhere. */
int ata_policy_queued_trim;

/* ==========================================================================
 * Cached IDENTIFY maintenance
 * ========================================================================== */

/* Recompute word 255 after changing cached words: low byte A5h, high byte
 * makes the 512 bytes sum to zero.  Pages without the signature stay as
 * they are. */
static void ata_id_fix_checksum(uint16_t *id)
{
	uint8_t sum = 0;

	if ((id[ATA_ID_INTEGRITY] & 0xFF) != ATA_ID_INTEGRITY_SIG)
		return;
	for (int i = 0; i < ATA_ID_WORDS - 1; i++)
		sum = (uint8_t)(sum + (id[i] & 0xFF) + (id[i] >> 8));
	sum = (uint8_t)(sum + ATA_ID_INTEGRITY_SIG);
	id[ATA_ID_INTEGRITY] =
		(uint16_t)(((uint8_t)(0x100 - sum) << 8) | ATA_ID_INTEGRITY_SIG);
}

static void ata_id_set_bit(uint16_t *id, int word, uint16_t bit, int on)
{
	if (on)
		id[word] |= bit;
	else
		id[word] &= (uint16_t)~bit;
}

/* Bring the cached state in line with a SET FEATURES that succeeded. */
static void ata_sf_note(struct ata_device *dev, uint8_t subcmd, uint8_t count)
{
	uint16_t *id = dev->id;

	switch (subcmd) {
	case SETFEATURES_WC_ON:
	case SETFEATURES_WC_OFF: {
		int on = subcmd == SETFEATURES_WC_ON;

		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_1, ATA_ID_CS1_WCACHE, on);
		if (on)
			dev->flags |= ATA_DFLAG_WCACHE_ON;
		else
			dev->flags &= ~ATA_DFLAG_WCACHE_ON;
		break;
	}
	case SETFEATURES_RLA_ON:
	case SETFEATURES_RLA_OFF: {
		int on = subcmd == SETFEATURES_RLA_ON;

		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_1, ATA_ID_CS1_RLA, on);
		if (on)
			dev->flags |= ATA_DFLAG_RLA_ON;
		else
			dev->flags &= ~ATA_DFLAG_RLA_ON;
		break;
	}
	case SETFEATURES_XFER:
		dev->xfer_mode = count;
		break;
	case SETFEATURES_APM_ON:
		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_2, ATA_ID_CS2_APM, 1);
		id[ATA_ID_APM_LEVEL] =
			(uint16_t)((id[ATA_ID_APM_LEVEL] & 0xFF00) | count);
		dev->apm_level = count;
		break;
	case SETFEATURES_APM_OFF:
		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_2, ATA_ID_CS2_APM, 0);
		dev->apm_level = 0;
		break;
	case SETFEATURES_AAM_ON:
		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_2, ATA_ID_CS2_AAM, 1);
		id[ATA_ID_AAM] = (uint16_t)((id[ATA_ID_AAM] & 0xFF00) | count);
		dev->aam_level = count;
		break;
	case SETFEATURES_AAM_OFF:
		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_2, ATA_ID_CS2_AAM, 0);
		dev->aam_level = 0;
		break;
	case SETFEATURES_PUIS_ON:
		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_2, ATA_ID_CS2_PUIS, 1);
		dev->flags |= ATA_DFLAG_PUIS;
		break;
	case SETFEATURES_PUIS_OFF:
		ata_id_set_bit(id, ATA_ID_CFS_ENABLE_2, ATA_ID_CS2_PUIS, 0);
		dev->flags &= ~ATA_DFLAG_PUIS;
		break;
	case SETFEATURES_SATA_ENABLE:
	case SETFEATURES_SATA_DISABLE: {
		int on = subcmd == SETFEATURES_SATA_ENABLE;
		uint16_t bit = 0;

		switch (count) {
		case SATA_FEAT_FPDMA_OFFSET: bit = ATA_ID_FEAT_NZ_OFFSET; break;
		case SATA_FEAT_FPDMA_AA: bit = ATA_ID_FEAT_DMA_SETUP_AA; break;
		case SATA_FEAT_DIPM: bit = ATA_ID_FEAT_DIPM; break;
		case SATA_FEAT_FPDMA_IN_ORDER: bit = ATA_ID_FEAT_IN_ORDER; break;
		case SATA_FEAT_AN: bit = ATA_ID_FEAT_AN; break;
		case SATA_FEAT_SSP: bit = ATA_ID_FEAT_SSP; break;
		case SATA_FEAT_DEVSLP: bit = ATA_ID_FEAT_DEVSLP; break;
		case SATA_FEAT_HYBRID: bit = ATA_ID_FEAT_HYBRID; break;
		default: break;
		}
		if (bit)
			ata_id_set_bit(id, ATA_ID_FEATURE_ENABLED, bit, on);
		if (count == SATA_FEAT_DIPM) {
			if (on)
				dev->flags |= ATA_DFLAG_DIPM_ON;
			else
				dev->flags &= ~ATA_DFLAG_DIPM_ON;
		}
		break;
	}
	case SETFEATURES_SENSE_DATA:
		ata_id_set_bit(id, ATA_ID_COMMAND_SET_4, ATA_ID_CS3_SENSE_DATA,
			       count != 0);
		break;
	default:
		break;
	}
	ata_id_fix_checksum(id);
}

/* ==========================================================================
 * SET FEATURES
 * ========================================================================== */

static int ata_set_features_raw(struct ata_device *dev, uint8_t subcmd,
				uint8_t count, uint32_t timeout_ms)
{
	struct ata_taskfile tf;
	int rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_SET_FEATURES;
	tf.feature = subcmd;
	tf.nsect = count;
	tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_ISADDR;
	tf.device = 0;
	rc = ata_exec_nodata(dev, &tf, timeout_ms, ATA_QCFLAG_QUIET);
	if (rc == -EIO && ata_tf_is_abort_only(&tf))
		return -EOPNOTSUPP;
	if (rc) {
		ata_dev_printk(dev, "SET FEATURES %02xh (count %02xh) failed "
				    "(%d, status %02x error %02x)\n",
			       subcmd, count, rc, tf.status, tf.error);
		return rc;
	}
	ata_sf_note(dev, subcmd, count);
	return 0;
}

int ata_set_features(struct ata_device *dev, uint8_t subcmd, uint8_t count)
{
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_set_features_raw(dev, subcmd, count,
				  subcmd == SETFEATURES_SPINUP ?
					  ATA_TMOUT_SPINUP_MS :
					  ATA_TMOUT_INTERNAL_MS);
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ---- write cache ---------------------------------------------------- */

static int ata_apply_write_cache(struct ata_device *dev, int on)
{
	int rc;

	if (!(dev->flags & ATA_DFLAG_WCACHE))
		return -EOPNOTSUPP;
	/* Empty the cache before switching it off, so nothing that was
	 * acknowledged is only in a cache we stop flushing. */
	if (!on && (dev->flags & ATA_DFLAG_WCACHE_ON)) {
		rc = ata_flush_cache(dev);
		if (rc)
			ata_dev_printk(dev, "flush before disabling the write "
					    "cache failed (%d)\n",
				       rc);
	}
	rc = ata_set_features_raw(dev, on ? SETFEATURES_WC_ON :
					    SETFEATURES_WC_OFF,
				  0, ATA_TMOUT_FLUSH_MS);
	if (rc == 0)
		ata_rw_update_disk_limits(dev);
	return rc;
}

int ata_set_write_cache(struct ata_device *dev, int on)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_apply_write_cache(dev, !!on);
	if (rc == 0) {
		if (p)
			p->policy.write_cache = (int8_t)!!on;
		ata_dev_printk(dev, "write cache %s\n", on ? "enabled" :
							      "disabled");
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ---- read look-ahead ------------------------------------------------- */

static int ata_apply_read_lookahead(struct ata_device *dev, int on)
{
	if (!(dev->flags & ATA_DFLAG_RLA))
		return -EOPNOTSUPP;
	return ata_set_features_raw(dev, on ? SETFEATURES_RLA_ON :
					      SETFEATURES_RLA_OFF,
				    0, ATA_TMOUT_INTERNAL_MS);
}

int ata_set_read_lookahead(struct ata_device *dev, int on)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_apply_read_lookahead(dev, !!on);
	if (rc == 0 && p)
		p->policy.read_lookahead = (int8_t)!!on;
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ---- transfer mode ---------------------------------------------------- */

/* Is `mode' one the device says it supports? */
static int ata_xfer_mode_supported(const struct ata_device *dev, uint8_t mode)
{
	if (mode >= XFER_UDMA_0 && mode <= XFER_UDMA_7)
		return !!(dev->udma_mask & (1u << (mode - XFER_UDMA_0)));
	if (mode >= XFER_MW_DMA_0 && mode <= XFER_MW_DMA_2)
		return !!(dev->mwdma_mask & (1u << (mode - XFER_MW_DMA_0)));
	if (mode >= XFER_PIO_0 && mode <= XFER_PIO_4)
		return !!(dev->pio_mask & (1u << (mode - XFER_PIO_0)));
	return mode == XFER_PIO_SLOW;
}

/* The fastest mode the device supports. */
static uint8_t ata_best_xfer_mode(const struct ata_device *dev)
{
	if (dev->udma_mask)
		return (uint8_t)(XFER_UDMA_0 + 31 -
				 __builtin_clz((unsigned)dev->udma_mask));
	if (dev->mwdma_mask)
		return (uint8_t)(XFER_MW_DMA_0 + 31 -
				 __builtin_clz((unsigned)dev->mwdma_mask));
	if (dev->pio_mask)
		return (uint8_t)(XFER_PIO_0 + 31 -
				 __builtin_clz((unsigned)dev->pio_mask));
	return XFER_PIO_0;
}

int ata_set_xfer_mode(struct ata_device *dev, uint8_t mode)
{
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (!ata_xfer_mode_supported(dev, mode))
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_set_features_raw(dev, SETFEATURES_XFER, mode,
				  ATA_TMOUT_INTERNAL_MS);
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ---- SATA features ----------------------------------------------------- */

/* Does word 76/78 say the device has SATA feature `feature'? */
static int ata_sata_feature_supported(const struct ata_device *dev,
				      uint8_t feature)
{
	uint16_t w76 = dev->id[ATA_ID_SATA_CAPABILITY];
	uint16_t w78 = dev->id[ATA_ID_FEATURE_SUPP];

	if (w76 == 0 || w76 == 0xFFFF || w78 == 0 || w78 == 0xFFFF)
		return 0;
	switch (feature) {
	case SATA_FEAT_FPDMA_OFFSET: return !!(w78 & ATA_ID_FEAT_NZ_OFFSET);
	case SATA_FEAT_FPDMA_AA: return !!(w78 & ATA_ID_FEAT_DMA_SETUP_AA);
	case SATA_FEAT_DIPM: return !!(w78 & ATA_ID_FEAT_DIPM);
	case SATA_FEAT_FPDMA_IN_ORDER: return !!(w78 & ATA_ID_FEAT_IN_ORDER);
	case SATA_FEAT_AN: return !!(w78 & ATA_ID_FEAT_AN);
	case SATA_FEAT_SSP: return !!(w78 & ATA_ID_FEAT_SSP);
	case SATA_FEAT_DAPS: return !!(w76 & ATA_ID_SATA_DAPS);
	case SATA_FEAT_DEVSLP: return !!(w78 & ATA_ID_FEAT_DEVSLP);
	case SATA_FEAT_HYBRID: return !!(w78 & ATA_ID_FEAT_HYBRID);
	case SATA_FEAT_HW_CTRL: return 1; /* reported in its own log */
	default: return 0;
	}
}

static int ata_sata_feature_enabled(const struct ata_device *dev,
				    uint8_t feature)
{
	uint16_t w79 = dev->id[ATA_ID_FEATURE_ENABLED];

	if (w79 == 0xFFFF)
		return 0;
	switch (feature) {
	case SATA_FEAT_FPDMA_OFFSET: return !!(w79 & ATA_ID_FEAT_NZ_OFFSET);
	case SATA_FEAT_FPDMA_AA: return !!(w79 & ATA_ID_FEAT_DMA_SETUP_AA);
	case SATA_FEAT_DIPM: return !!(w79 & ATA_ID_FEAT_DIPM);
	case SATA_FEAT_FPDMA_IN_ORDER: return !!(w79 & ATA_ID_FEAT_IN_ORDER);
	case SATA_FEAT_AN: return !!(w79 & ATA_ID_FEAT_AN);
	case SATA_FEAT_SSP: return !!(w79 & ATA_ID_FEAT_SSP);
	case SATA_FEAT_DEVSLP: return !!(w79 & ATA_ID_FEAT_DEVSLP);
	case SATA_FEAT_HYBRID: return !!(w79 & ATA_ID_FEAT_HYBRID);
	default: return 0;
	}
}

static const char *ata_sata_feature_name(uint8_t feature)
{
	switch (feature) {
	case SATA_FEAT_FPDMA_OFFSET: return "non-zero buffer offsets";
	case SATA_FEAT_FPDMA_AA: return "DMA setup auto-activate";
	case SATA_FEAT_DIPM: return "device-initiated power management";
	case SATA_FEAT_FPDMA_IN_ORDER: return "in-order data delivery";
	case SATA_FEAT_AN: return "asynchronous notification";
	case SATA_FEAT_SSP: return "software settings preservation";
	case SATA_FEAT_DAPS: return "automatic partial to slumber";
	case SATA_FEAT_HW_CTRL: return "hardware feature control";
	case SATA_FEAT_DEVSLP: return "device sleep";
	case SATA_FEAT_HYBRID: return "hybrid information";
	default: return "unknown SATA feature";
	}
}

static int ata_apply_sata_feature(struct ata_device *dev, uint8_t feature,
				  int on)
{
	int rc;

	if (!ata_sata_feature_supported(dev, feature))
		return -EOPNOTSUPP;
	rc = ata_set_features_raw(dev, on ? SETFEATURES_SATA_ENABLE :
					    SETFEATURES_SATA_DISABLE,
				  feature, ATA_TMOUT_INTERNAL_MS);
	if (rc && rc != -EOPNOTSUPP)
		ata_dev_printk(dev, "could not %s %s (%d)\n",
			       on ? "enable" : "disable",
			       ata_sata_feature_name(feature), rc);
	return rc;
}

int ata_set_sata_feature(struct ata_device *dev, uint8_t feature, int on)
{

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (feature == SATA_FEAT_DIPM && on &&
	    (ata_dev_quirks(dev) & ATA_QUIRK_NO_DIPM))
		return -EOPNOTSUPP;
	if (ata_can_sleep())
		might_sleep();
	/* No EH serialisation taken here: the callers (the port's link
	 * power management, the configuration policy) hold the eh_mutex or
	 * run inside error handling already. */
	return ata_apply_sata_feature(dev, feature, !!on);
}

/* ---- APM / AAM / PUIS / spin-up ------------------------------------------ */

static int ata_apply_apm(struct ata_device *dev, uint8_t level)
{
	if (!(dev->flags & ATA_DFLAG_APM))
		return -EOPNOTSUPP;
	if (level == 0xFF)
		return -EINVAL; /* reserved */
	if (level == 0)
		return ata_set_features_raw(dev, SETFEATURES_APM_OFF, 0,
					    ATA_TMOUT_INTERNAL_MS);
	return ata_set_features_raw(dev, SETFEATURES_APM_ON, level,
				    ATA_TMOUT_INTERNAL_MS);
}

int ata_set_apm(struct ata_device *dev, uint8_t level)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_dev_quirks(dev) & ATA_QUIRK_NO_APM)
		return -EOPNOTSUPP;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_apply_apm(dev, level);
	if (rc == 0) {
		if (p)
			p->policy.apm = level;
		if (level)
			ata_dev_printk(dev, "APM level %u (%s)\n", level,
				       level >= 128 ? "no spin-down" :
						      "spin-down allowed");
		else
			ata_dev_printk(dev, "APM disabled\n");
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

static int ata_apply_aam(struct ata_device *dev, uint8_t level)
{
	if (!(dev->flags & ATA_DFLAG_AAM))
		return -EOPNOTSUPP;
	if (level == 0)
		return ata_set_features_raw(dev, SETFEATURES_AAM_OFF, 0,
					    ATA_TMOUT_INTERNAL_MS);
	/* 80h (quietest) .. FEh (fastest); 01h..7Fh are vendor/reserved */
	if (level < 0x80 || level == 0xFF)
		return -EINVAL;
	return ata_set_features_raw(dev, SETFEATURES_AAM_ON, level,
				    ATA_TMOUT_INTERNAL_MS);
}

int ata_set_aam(struct ata_device *dev, uint8_t level)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_apply_aam(dev, level);
	if (rc == 0 && p)
		p->policy.aam = level;
	ata_mgmt_exit(dev, took);
	return rc;
}

static int ata_apply_puis(struct ata_device *dev, int on)
{
	if (!(dev->id[ATA_ID_COMMAND_SET_2] & ATA_ID_CS2_PUIS) ||
	    !ATA_ID_WORD_VALID(dev->id[ATA_ID_COMMAND_SET_2]))
		return -EOPNOTSUPP;
	return ata_set_features_raw(dev, on ? SETFEATURES_PUIS_ON :
					      SETFEATURES_PUIS_OFF,
				    0, ATA_TMOUT_INTERNAL_MS);
}

int ata_set_puis(struct ata_device *dev, int on)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_apply_puis(dev, !!on);
	if (rc == 0) {
		if (p)
			p->policy.puis = (int8_t)!!on;
		ata_dev_printk(dev, "power-up in standby %s%s\n",
			       on ? "enabled" : "disabled",
			       (on && (dev->flags & ATA_DFLAG_SPINUP_REQ)) ?
				       " (spin-up by SET FEATURES)" :
				       "");
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

int ata_spinup(struct ata_device *dev)
{
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_set_features_raw(dev, SETFEATURES_SPINUP, 0,
				  ATA_TMOUT_SPINUP_MS);
	if (rc == 0) {
		dev->power_state = ATA_PWR_ACTIVE;
		ata_dev_printk(dev, "spun up\n");
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ---- SET MULTIPLE MODE ------------------------------------------------- */

int ata_set_multiple(struct ata_device *dev, uint8_t count)
{
	struct ata_taskfile tf;
	uint8_t max;
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->class == ATA_DEV_ATAPI)
		return -EOPNOTSUPP;
	max = (uint8_t)(dev->id[ATA_ID_MAX_MULTSECT] & 0xFF);
	if (!max)
		return -EOPNOTSUPP;
	/* 0 disables multiple mode (ACS-2+); otherwise a power of two that
	 * does not exceed the maximum. */
	if (count && ((count & (count - 1)) || count > max))
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_SET_MULTI;
	tf.nsect = count;
	tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_ISADDR;
	rc = ata_exec_nodata(dev, &tf, 0, ATA_QCFLAG_QUIET);
	if (rc == -EIO && ata_tf_is_abort_only(&tf))
		rc = -EOPNOTSUPP;
	if (rc == 0) {
		dev->multi_count = count;
		dev->id[ATA_ID_MULTSECT] =
			(uint16_t)((dev->id[ATA_ID_MULTSECT] & 0xFE00) |
				   (count ? ATA_ID_MULT_VALID : 0) | count);
		ata_id_fix_checksum(dev->id);
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ---- sense data reporting ------------------------------------------------- */

static int ata_apply_sense_reporting(struct ata_device *dev, int on)
{
	if (!(dev->flags & ATA_DFLAG_SENSE_DATA))
		return -EOPNOTSUPP;
	return ata_set_features_raw(dev, SETFEATURES_SENSE_DATA, on ? 1 : 0,
				    ATA_TMOUT_INTERNAL_MS);
}

int ata_set_sense_reporting(struct ata_device *dev, int on)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_apply_sense_reporting(dev, !!on);
	if (rc == 0 && p)
		p->policy.sense_reporting = (int8_t)!!on;
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ==========================================================================
 * Logs
 * ========================================================================== */

int ata_read_log_ext(struct ata_device *dev, uint8_t log, uint16_t page,
		     void *buf, uint16_t count, uint32_t qcflags)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct ata_taskfile tf;
	int dma, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !buf || !count)
		return -EINVAL;
	if (!(dev->flags & ATA_DFLAG_GPL))
		return -EOPNOTSUPP;
	if (ata_can_sleep())
		might_sleep();
	dma = (dev->flags & ATA_DFLAG_LOG_DMA) &&
	      !(ata_dev_quirks(dev) & ATA_QUIRK_NO_LOG_DMA) &&
	      !(p && p->log_dma_broken) &&
	      (dev->id[ATA_ID_CAPABILITY] & ATA_ID_CAP_DMA);
again:
	kmemset(&tf, 0, sizeof(tf));
	tf.command = dma ? ATA_CMD_READ_LOG_DMA_EXT : ATA_CMD_READ_LOG_EXT;
	tf.flags = ATA_TFLAG_LBA48 | ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE |
		   ATA_TFLAG_LBA;
	tf.nsect = (uint8_t)(count & 0xFF);
	tf.hob_nsect = (uint8_t)(count >> 8);
	tf.lbal = log;
	tf.lbam = (uint8_t)(page & 0xFF);
	tf.hob_lbam = (uint8_t)(page >> 8);
	tf.device = ATA_LBA;
	rc = ata_exec_internal(dev, &tf, NULL,
			       dma ? ATA_PROT_DMA_IN : ATA_PROT_PIO_IN, buf,
			       (uint32_t)count * ATA_SECT_SIZE,
			       ATA_DIR_FROM_DEVICE, ATA_TMOUT_INTERNAL_MS,
			       qcflags);
	if (rc && dma && rc != -ENODEV && rc != -ENOMEM) {
		/* Some firmware advertises READ LOG DMA EXT and botches it:
		 * PIO from now on. */
		if (p)
			p->log_dma_broken = 1;
		ata_dev_printk(dev, "READ LOG DMA EXT of log %02xh failed (%d), "
				    "using READ LOG EXT\n",
			       log, rc);
		dma = 0;
		goto again;
	}
	if (rc == -EIO && ata_tf_is_abort_only(&tf))
		rc = -EOPNOTSUPP;
	return rc;
}

int ata_read_log_directory(struct ata_device *dev, uint16_t *pages256)
{
	uint8_t *buf;
	int rc;

	if (!dev || !pages256)
		return -EINVAL;
	buf = kalloc(ATA_SECT_SIZE);
	if (!buf)
		return -ENOMEM;
	rc = ata_read_log_ext(dev, ATA_LOG_DIRECTORY, 0, buf, 1,
			      ATA_QCFLAG_QUIET);
	if (rc == 0) {
		for (int i = 0; i < 256; i++)
			pages256[i] = (uint16_t)(buf[2 * i] |
						 ((uint16_t)buf[2 * i + 1] << 8));
		/* word 0: general purpose logging version, 0001h */
		if (pages256[0] != 0x0001)
			ata_dev_printk(dev, "log directory version %04x "
					    "(expected 0001)\n",
				       pages256[0]);
	}
	kfree(buf);
	return rc;
}

/* NCQ SEND AND RECEIVE log (13h): dword 0 bit 0 = DATA SET MANAGEMENT is
 * available as SEND FPDMA QUEUED, dword 1 bit 0 = its TRIM subcommand. */
int ata_dev_probe_ncq_send_recv(struct ata_device *dev)
{
	uint16_t *dir = NULL;
	uint8_t *log = NULL;
	int rc = 0;

	dev->flags &= ~ATA_DFLAG_NCQ_TRIM;
	if (!(dev->flags & ATA_DFLAG_NCQ_SEND_RECV) ||
	    !(dev->flags & ATA_DFLAG_NCQ_ON) || !(dev->flags & ATA_DFLAG_GPL) ||
	    !(dev->flags & ATA_DFLAG_TRIM))
		return 0;
	dir = kalloc(256 * sizeof(uint16_t));
	log = kalloc(ATA_SECT_SIZE);
	if (!dir || !log) {
		rc = -ENOMEM;
		goto out;
	}
	rc = ata_read_log_directory(dev, dir);
	if (rc)
		goto out;
	if (dir[ATA_LOG_NCQ_SEND_RECV] == 0)
		goto out; /* log not there */
	rc = ata_read_log_ext(dev, ATA_LOG_NCQ_SEND_RECV, 0, log, 1,
			      ATA_QCFLAG_QUIET);
	if (rc)
		goto out;
	if ((log[0] & 0x01) && (log[4] & 0x01)) {
		if (ata_dev_quirks(dev) & ATA_QUIRK_NO_NCQ_TRIM)
			ata_dev_printk(dev, "queued TRIM disabled: known to "
					    "corrupt data on this model\n");
		else if (ata_policy_queued_trim)
			dev->flags |= ATA_DFLAG_NCQ_TRIM;
		else
			ata_dev_printk(dev, "queued TRIM available, plain TRIM "
					    "used (queued TRIM is opt-in)\n");
	}
out:
	if (dir)
		kfree(dir);
	if (log)
		kfree(log);
	return rc;
}

/* ==========================================================================
 * EXECUTE DEVICE DIAGNOSTIC
 * ========================================================================== */

static const char *ata_diag_code_name(uint8_t code)
{
	switch (code & 0x7F) {
	case 0x01: return "passed";
	case 0x00:
	case 0x02: return "formatter device error";
	case 0x03: return "sector buffer error";
	case 0x04: return "ECC circuitry error";
	case 0x05: return "controlling microprocessor error";
	default: return "device-specific failure";
	}
}

int ata_execute_diagnostic(struct ata_device *dev, uint8_t *code)
{
	struct ata_taskfile tf;
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !code)
		return -EINVAL;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	/* Nothing must be cached when the device resets itself. */
	if (dev->class != ATA_DEV_ATAPI && (dev->flags & ATA_DFLAG_WCACHE_ON))
		(void)ata_flush_cache(dev);

	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_EDD;
	tf.flags = ATA_TFLAG_DEVICE;
	rc = ata_exec_nodata(dev, &tf, ATA_TMOUT_DIAG_MS, ATA_QCFLAG_QUIET);
	/* The diagnostic code is in the ERROR register whether or not the
	 * device flagged the result as an error. */
	*code = tf.error;
	if (rc == 0 || rc == -EIO) {
		uint8_t c = tf.error;
		enum ata_class sig_class = ata_class_from_sig(
			0x00000101u | ((uint32_t)tf.lbam << 16) |
			((uint32_t)tf.lbah << 24));

		if (c == 0x01 || c == 0x81) {
			ata_dev_printk(dev, "device diagnostic passed%s\n",
				       c == 0x81 ? " (device 1 failed)" : "");
		} else {
			ata_dev_printk(dev, "device diagnostic FAILED: code "
					    "%02xh, %s%s\n",
				       c, ata_diag_code_name(c),
				       (c & 0x80) ? " (device 1 failed too)" :
						    "");
		}
		if ((tf.lbam || tf.lbah) && sig_class != dev->class &&
		    !(sig_class == ATA_DEV_ATA && dev->class == ATA_DEV_ZAC))
			ata_dev_printk(dev, "signature after diagnostic says "
					    "%s, device is %s\n",
				       ata_class_name(sig_class),
				       ata_class_name(dev->class));
		rc = 0;
		/* The diagnostic leaves the device in its post-reset state:
		 * everything not preserved must be configured again. */
		(void)ata_dev_configure(dev);
	} else {
		ata_dev_printk(dev, "EXECUTE DEVICE DIAGNOSTIC failed (%d)\n",
			       rc);
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ==========================================================================
 * Host protected area
 * ========================================================================== */

static int ata_hpa_supported(const struct ata_device *dev)
{
	uint16_t w83 = dev->id[ATA_ID_COMMAND_SET_2];

	return ATA_ID_WORD_VALID(w83) && (w83 & ATA_ID_CS2_SET_MAX_SEC);
}

static int ata_read_native_max_raw(struct ata_device *dev, uint64_t *sectors)
{
	struct ata_taskfile tf;
	int ext = !!(dev->flags & ATA_DFLAG_LBA48);
	uint64_t lba;
	int rc;

	kmemset(&tf, 0, sizeof(tf));
	tf.command = ext ? ATA_CMD_READ_NATIVE_MAX_EXT :
			   ATA_CMD_READ_NATIVE_MAX;
	tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_LBA | (ext ? ATA_TFLAG_LBA48 :
								0);
	tf.device = ATA_LBA;
	rc = ata_exec_nodata(dev, &tf, 0, ATA_QCFLAG_QUIET);
	if (rc == -EIO && ata_tf_is_abort_only(&tf))
		return -EOPNOTSUPP;
	if (rc)
		return rc;
	lba = ata_tf_read_lba(&tf);
	*sectors = lba + 1;
	return 0;
}

int ata_read_native_max(struct ata_device *dev, uint64_t *sectors)
{
	uint64_t n = 0;
	int took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !sectors)
		return -EINVAL;
	if (dev->class == ATA_DEV_ATAPI || !ata_hpa_supported(dev))
		return -EOPNOTSUPP;
	if (ata_dev_quirks(dev) & ATA_QUIRK_BROKEN_HPA)
		return -EOPNOTSUPP;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_read_native_max_raw(dev, &n);
	if (rc == 0) {
		/* A 28-bit answer on a 48-bit disk tops out at 2^28; that is
		 * no evidence of a protected area. */
		if (!(dev->flags & ATA_DFLAG_LBA48) && n > ATA_MAX_LBA28 + 1)
			n = ATA_MAX_LBA28 + 1;
		dev->n_native_sectors = n;
		if (n > dev->n_sectors)
			dev->flags |= ATA_DFLAG_HPA;
		else
			dev->flags &= ~ATA_DFLAG_HPA;
		*sectors = n;
	}
	ata_mgmt_exit(dev, took);
	return rc;
}

int ata_set_max_address(struct ata_device *dev, uint64_t sectors,
			int persist)
{
	struct ata_taskfile tf;
	uint64_t native = 0;
	int ext, took, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (dev->class == ATA_DEV_ATAPI || !ata_hpa_supported(dev))
		return -EOPNOTSUPP;
	if (dev->security.locked)
		return -EACCES;
	if (ata_can_sleep())
		might_sleep();
	took = ata_mgmt_enter(dev);
	ext = !!(dev->flags & ATA_DFLAG_LBA48);

	/* The command set requires READ NATIVE MAX ADDRESS immediately
	 * before SET MAX ADDRESS. */
	rc = ata_read_native_max_raw(dev, &native);
	if (rc)
		goto out;
	if (sectors == 0 || sectors > native ||
	    (!ext && sectors > ATA_MAX_LBA28 + 1)) {
		rc = -EINVAL;
		goto out;
	}
	kmemset(&tf, 0, sizeof(tf));
	tf.command = ext ? ATA_CMD_SET_MAX_EXT : ATA_CMD_SET_MAX;
	ata_tf_set_lba(&tf, sectors - 1, ext);
	/* COUNT bit 0 (VV, value volatile): 1 keeps the new maximum over
	 * power cycles and resets. */
	tf.nsect = persist ? 0x01 : 0x00;
	tf.feature = 0; /* SET MAX ADDRESS proper, not a security subcmd */
	rc = ata_exec_nodata(dev, &tf, 0, ATA_QCFLAG_QUIET);
	if (rc == -EIO && ata_tf_is_abort_only(&tf)) {
		ata_dev_printk(dev, "SET MAX ADDRESS refused (already set "
				    "since power-on, or locked)\n");
		rc = -EIO;
		goto out;
	}
	if (rc)
		goto out;
	dev->n_native_sectors = native;
	ata_dev_printk(dev, "user capacity set to %llu of %llu sectors (%s)\n",
		       (unsigned long long)sectors,
		       (unsigned long long)native,
		       persist ? "persistent" : "until power-off");
	/* Pick up the new capacity the way the device reports it now. */
	rc = ata_dev_revalidate(dev);
out:
	ata_mgmt_exit(dev, took);
	return rc;
}

/* ==========================================================================
 * The configuration policy
 * ========================================================================== */

static int ata_cfg_fatal(int rc)
{
	return rc == -ENODEV;
}

/* NCQ depth from the device, the host, the quirks and what error handling
 * decided earlier. */
static void ata_cfg_ncq(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	uint32_t depth = 0;

	dev->flags &= ~ATA_DFLAG_NCQ_ON;
	if ((dev->flags & ATA_DFLAG_NCQ) && (dev->flags & ATA_DFLAG_LBA48) &&
	    dev->host_ncq && !(ata_dev_quirks(dev) & ATA_QUIRK_NO_NCQ)) {
		depth = dev->ncq_depth_dev;
		if (depth > dev->host_queue_depth)
			depth = dev->host_queue_depth;
		if (depth > ATA_MAX_QUEUE)
			depth = ATA_MAX_QUEUE;
		if (p && p->ncq_depth_limit && depth > p->ncq_depth_limit)
			depth = p->ncq_depth_limit;
		if (p && p->ncq_forced_off)
			depth = 0;
	}
	if (depth >= 2) {
		dev->ncq_depth = depth;
		dev->flags |= ATA_DFLAG_NCQ_ON;
	} else {
		dev->ncq_depth = 0;
	}
}

int ata_dev_apply_policy(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct ata_dev_policy pol;
	uint32_t quirks = ata_dev_quirks(dev);
	int rc;

	if (p)
		pol = p->policy;
	else {
		kmemset(&pol, 0xFF, sizeof(pol)); /* all "default" (-1) */
		pol.ncq_prio = 0;
	}

	/* volatile write cache: on unless asked otherwise */
	if (dev->flags & ATA_DFLAG_WCACHE) {
		int want = pol.write_cache < 0 ? 1 : pol.write_cache;

		if (!!(dev->flags & ATA_DFLAG_WCACHE_ON) != want ||
		    pol.write_cache >= 0) {
			rc = ata_apply_write_cache(dev, want);
			if (ata_cfg_fatal(rc))
				return rc;
			if (rc)
				ata_dev_printk(dev, "could not turn the write "
						    "cache %s (%d)\n",
					       want ? "on" : "off", rc);
		}
	}
	/* read look-ahead: on unless asked otherwise */
	if (dev->flags & ATA_DFLAG_RLA) {
		int want = pol.read_lookahead < 0 ? 1 : pol.read_lookahead;

		if (!!(dev->flags & ATA_DFLAG_RLA_ON) != want ||
		    pol.read_lookahead >= 0) {
			rc = ata_apply_read_lookahead(dev, want);
			if (ata_cfg_fatal(rc))
				return rc;
		}
	}
	/* APM: 254 (highest performance short of no APM, no spin-down) by
	 * default; an explicit level or "off" when asked */
	if ((dev->flags & ATA_DFLAG_APM) && !(quirks & ATA_QUIRK_NO_APM)) {
		int want = pol.apm < 0 ? 254 : pol.apm;

		if (dev->apm_level != want || pol.apm >= 0) {
			rc = ata_apply_apm(dev, (uint8_t)want);
			if (ata_cfg_fatal(rc))
				return rc;
		}
	}
	/* AAM, PUIS: the firmware's setting unless asked */
	if (pol.aam >= 0 && (dev->flags & ATA_DFLAG_AAM)) {
		rc = ata_apply_aam(dev, (uint8_t)pol.aam);
		if (ata_cfg_fatal(rc))
			return rc;
	}
	if (pol.puis >= 0) {
		rc = ata_apply_puis(dev, pol.puis);
		if (ata_cfg_fatal(rc))
			return rc;
	}
	/* sense data reporting (also turns NCQ autosense on) */
	if ((dev->flags & ATA_DFLAG_SENSE_DATA) && pol.sense_reporting != 0) {
		rc = ata_apply_sense_reporting(dev, 1);
		if (ata_cfg_fatal(rc))
			return rc;
	}
	/* a standby timer set earlier */
	if (pol.standby_timer >= 0) {
		struct ata_taskfile tf;

		kmemset(&tf, 0, sizeof(tf));
		tf.command = ATA_CMD_IDLE;
		tf.nsect = (uint8_t)pol.standby_timer;
		tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_ISADDR;
		rc = ata_exec_nodata(dev, &tf, ATA_TMOUT_STANDBY_MS,
				     ATA_QCFLAG_QUIET);
		if (ata_cfg_fatal(rc))
			return rc;
		if (rc == 0)
			dev->standby_timer = (uint8_t)pol.standby_timer;
	}
	return 0;
}

/* Enable SATA asynchronous notification on ATAPI devices.  Off by
 * default: many drives notify unreliably, and the media events of a
 * drive are polled then. */
int ata_atapi_an = 0;

int ata_dev_configure(struct ata_device *dev)
{
	struct ata_dev_priv *p;
	struct ata_link_info li;
	uint32_t quirks;
	uint8_t mode;
	int took, rc;

	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	took = ata_mgmt_enter(dev);
	p = ata_dev_priv(dev);
	quirks = ata_dev_quirks(dev);
	kmemset(&li, 0, sizeof(li));
	li.lpm_policy = AHCI_LPM_KEEP_FIRMWARE;
	if (dev->xport->link_info)
		dev->xport->link_info(dev, &li);

	/* ---- transfer mode ------------------------------------------------
	 * Meaningless on the wire for SATA, but some devices (and bridges)
	 * want it before they run DMA commands; an abort is harmless. */
	mode = ata_best_xfer_mode(dev);
	if (!(quirks & ATA_QUIRK_NO_SETXFER)) {
		rc = ata_set_features_raw(dev, SETFEATURES_XFER, mode,
					  ATA_TMOUT_INTERNAL_MS);
		if (ata_cfg_fatal(rc))
			goto out;
		if (rc)
			dev->xfer_mode = mode; /* SATA: in effect anyway */
	} else {
		dev->xfer_mode = mode;
	}

	if (dev->class == ATA_DEV_ATAPI) {
		/* Asynchronous notification (media events without polling):
		 * only where asked for (ata_atapi_an, off by default), the
		 * device supports it, and -- behind a port multiplier -- the
		 * host can tell an ATAPI notification from the multiplier's
		 * own (SNotification).  The device's media are polled
		 * unless the notification was enabled here. */
		dev->flags &= ~ATA_DFLAG_AN;
		if (ata_atapi_an && ata_sata_feature_supported(dev, SATA_FEAT_AN) &&
		    (!li.pmp_attached || li.sntf)) {
			rc = ata_apply_sata_feature(dev, SATA_FEAT_AN, 1);
			if (ata_cfg_fatal(rc))
				goto out;
			if (rc == 0)
				dev->flags |= ATA_DFLAG_AN;
			else
				ata_dev_printk(dev, "failed to enable ATAPI AN (%d)\n",
					       rc);
		}
		if (dev->power_state == ATA_PWR_UNKNOWN)
			dev->power_state = ATA_PWR_ACTIVE;
		ata_dev_printk(dev, "configured for %s%s\n",
			       ata_xfer_mode_name(dev->xfer_mode),
			       (dev->flags & ATA_DFLAG_AN) ?
				       ", ATAPI AN" : "");
		rc = 0;
		goto out;
	}

	/* ---- READ/WRITE MULTIPLE size (PIO multi-sector commands) --------- */
	{
		uint8_t max = (uint8_t)(dev->id[ATA_ID_MAX_MULTSECT] & 0xFF);

		if (max && !(max & (max - 1)) && dev->multi_count != max) {
			rc = ata_set_multiple(dev, max);
			if (ata_cfg_fatal(rc))
				goto out;
		}
	}

	/* ---- host protected area ------------------------------------------ */
	if (ata_hpa_supported(dev) && !(quirks & ATA_QUIRK_BROKEN_HPA) &&
	    !dev->security.locked) {
		uint64_t native = 0;

		rc = ata_read_native_max_raw(dev, &native);
		if (ata_cfg_fatal(rc))
			goto out;
		if (rc == 0) {
			if (!(dev->flags & ATA_DFLAG_LBA48) &&
			    native > ATA_MAX_LBA28 + 1)
				native = ATA_MAX_LBA28 + 1;
			dev->n_native_sectors = native;
			if (native > dev->n_sectors) {
				dev->flags |= ATA_DFLAG_HPA;
				ata_dev_printk(dev, "host protected area: %llu "
						    "of %llu native sectors "
						    "visible (left as it is)\n",
					       (unsigned long long)dev->n_sectors,
					       (unsigned long long)native);
			} else {
				dev->flags &= ~ATA_DFLAG_HPA;
			}
		}
	}

	/* ---- write cache, look-ahead, APM, AAM, PUIS, sense, timer -------- */
	rc = ata_dev_apply_policy(dev);
	if (ata_cfg_fatal(rc))
		goto out;

	/* ---- NCQ ----------------------------------------------------------- */
	ata_cfg_ncq(dev);
	if (dev->flags & ATA_DFLAG_NCQ_ON) {
		/* DMA setup FIS auto-activate saves a FIS per write. */
		if (ata_sata_feature_supported(dev, SATA_FEAT_FPDMA_AA) &&
		    !ata_sata_feature_enabled(dev, SATA_FEAT_FPDMA_AA) &&
		    !(quirks & ATA_QUIRK_NO_FPDMA_AA)) {
			rc = ata_apply_sata_feature(dev, SATA_FEAT_FPDMA_AA, 1);
			if (ata_cfg_fatal(rc))
				goto out;
		}
		rc = ata_dev_probe_ncq_send_recv(dev);
		if (ata_cfg_fatal(rc))
			goto out;
	} else {
		dev->flags &= ~ATA_DFLAG_NCQ_TRIM;
	}

	/* ---- device-initiated power management per the link policy -------- */
	if (ata_sata_feature_supported(dev, SATA_FEAT_DIPM)) {
		int want = -1;

		switch (li.lpm_policy) {
		case AHCI_LPM_MED_POWER_WITH_DIPM:
		case AHCI_LPM_MIN_POWER:
			want = !(quirks & ATA_QUIRK_NO_DIPM);
			break;
		case AHCI_LPM_MAX_PERFORMANCE:
		case AHCI_LPM_MEDIUM_POWER:
			want = 0;
			break;
		default: /* keep the firmware's choice */
			break;
		}
		if (want >= 0 &&
		    want != !!(dev->flags & ATA_DFLAG_DIPM_ON)) {
			rc = ata_apply_sata_feature(dev, SATA_FEAT_DIPM, want);
			if (ata_cfg_fatal(rc))
				goto out;
		}
	}

	/* ---- software settings preservation: keep all of the above over a
	 * COMRESET (the default on most devices; make sure) ----------------- */
	if (ata_sata_feature_supported(dev, SATA_FEAT_SSP) &&
	    !ata_sata_feature_enabled(dev, SATA_FEAT_SSP)) {
		rc = ata_apply_sata_feature(dev, SATA_FEAT_SSP, 1);
		if (ata_cfg_fatal(rc))
			goto out;
	}

	/* ---- security: left as the firmware left it.  A production system
	 * does not freeze the security feature set itself: a secure erase
	 * by an installer or by hdparm must stay possible on a drive the
	 * BIOS did not freeze (and after a suspend/resume cycle on one it
	 * did).  Only an explicit policy (freeze_security > 0) freezes. -- */
	if (dev->security.supported && !dev->security.locked &&
	    !dev->security.frozen && p && p->policy.freeze_security > 0) {
		rc = ata_security_freeze(dev);
		if (ata_cfg_fatal(rc))
			goto out;
		if (rc == 0) {
			dev->flags |= ATA_DFLAG_FROZEN_SEC;
			dev->security.frozen = 1;
		}
	}

	if (dev->power_state == ATA_PWR_UNKNOWN)
		dev->power_state = ATA_PWR_ACTIVE;
	ata_dev_printk(dev, "configured for %s, write cache %s, look-ahead "
			    "%s, %s%s, APM %s\n",
		       ata_xfer_mode_name(dev->xfer_mode),
		       (dev->flags & ATA_DFLAG_WCACHE_ON) ? "on" :
		       (dev->flags & ATA_DFLAG_WCACHE)    ? "off" :
							    "none",
		       (dev->flags & ATA_DFLAG_RLA_ON) ? "on" :
		       (dev->flags & ATA_DFLAG_RLA)    ? "off" :
							 "none",
		       (dev->flags & ATA_DFLAG_NCQ_ON) ? "NCQ" : "no NCQ",
		       (dev->flags & ATA_DFLAG_DIPM_ON) ? ", DIPM" : "",
		       (dev->flags & ATA_DFLAG_APM) ?
			       (dev->apm_level ? "on" : "off") :
			       "n/a");
	if (dev->flags & ATA_DFLAG_NCQ_ON)
		ata_dev_printk(dev, "NCQ depth %u (device %u, host %u)%s\n",
			       dev->ncq_depth, dev->ncq_depth_dev,
			       dev->host_queue_depth,
			       (dev->flags & ATA_DFLAG_NCQ_TRIM) ?
				       ", queued TRIM" :
				       "");
	rc = 0;
out:
	ata_mgmt_exit(dev, took);
	return rc;
}
