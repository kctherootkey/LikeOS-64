// LikeOS ATA layer - ATA security feature set
//
// The ATA security feature set lets a drive be locked with a password: a
// locked drive refuses every media access until SECURITY UNLOCK with the
// right password, and SECURITY ERASE UNIT destroys all user data (the
// "secure erase" of an SSD or disk).  The state lives in IDENTIFY word 128:
//
//   bit 0  supported            bit 4  count expired (5 failed unlocks;
//   bit 1  enabled (a user              only a power cycle or hardware
//          password is set)             reset re-arms the counter)
//   bit 2  locked               bit 5  enhanced erase supported
//   bit 3  frozen (SECURITY     bit 8  security level: 0 high, 1 maximum
//          FREEZE LOCK ran;
//          no state change
//          until power cycle)
//
// words 89/90 give the normal/enhanced erase time estimates (2-minute
// units; bit 15 selects the extended 15-bit format) and word 92 the master
// password revision code.
//
// The commands:
//
//   F1h SECURITY SET PASSWORD     PIO data-out, 512-byte block
//   F2h SECURITY UNLOCK           PIO data-out, 512-byte block
//   F3h SECURITY ERASE PREPARE    no data; MUST immediately precede F4h
//   F4h SECURITY ERASE UNIT       PIO data-out, 512-byte block; runs for
//                                 the time words 89/90 announce
//   F5h SECURITY FREEZE LOCK      no data
//   F6h SECURITY DISABLE PASSWORD PIO data-out, 512-byte block
//
// The 512-byte block: word 0 control (bit 0 identifier 0 user / 1 master;
// for SET PASSWORD bit 8 the level 0 high / 1 maximum; for ERASE UNIT bit 1
// enhanced erase), words 1..16 the 32-byte password (bytes exactly as the
// caller gave them -- this is the order the established tools use, so a
// password set elsewhere unlocks here), word 17 the master password
// identifier (SET PASSWORD with the master bit, 0000h/FFFFh mean "do not
// change"), words 18..255 zero.
//
// What the state machine allows is checked before a command goes out, so
// the caller gets a precise error (-EACCES "frozen", "locked", "count
// expired") instead of a bare ABORT from the drive; the drive's answer is
// still authoritative -- after every state-changing command the IDENTIFY
// data is read again and dev->security is refreshed from it.
//
// Passwords never appear in the kernel log, and every buffer that held one
// is cleared before it is freed.  Security operations of all drives are
// serialised by one sleeping mutex: they are rare, some run for hours, and
// ERASE PREPARE/ERASE UNIT must not be separated by another security
// command.  ERASE additionally stops and drains the disk's request queue
// so nothing else reaches the drive between PREPARE and UNIT, and refuses
// while any partition of the disk is mounted.
//
// Context: ata_security_parse() and ata_security_state_str() are pure;
// everything else may sleep (and polls before the scheduler runs).
// Permission checks (root only) happen in the ioctl layer (ata_sat.c).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

#define SEC_PFX "[ata] "

/* Word 0 of the password block */
#define SEC_BLOCK_WORDS (ATA_SECT_SIZE / 2)

/* Erase time values that mean "longer than the field can say" */
#define SEC_ERASE_TIME_MAX_SHORT 0xFF /* > 508 minutes */
#define SEC_ERASE_TIME_MAX_EXT 0x7FFF /* > 65532 minutes */
/* Timeout when the drive gives no estimate: a full day.  An erase that
 * outlives this is broken; one that ends earlier is fine. */
#define SEC_ERASE_UNKNOWN_TMOUT_MS (24ULL * 60 * 60 * 1000)
/* Longest timeout this driver ever arms for an erase (the timeout field
 * is 32-bit milliseconds: ~49 days; a week is plenty). */
#define SEC_ERASE_MAX_TMOUT_MS (7ULL * 24 * 60 * 60 * 1000)
/* Margin on top of twice the estimate */
#define SEC_ERASE_MARGIN_MS (30ULL * 60 * 1000)

/* Serialises the security operations of every drive. */
static struct ata_mutex g_ata_sec_mutex = ATA_MUTEX_INIT("ata_security");

/* Which operation a precondition check is for */
enum sec_op {
	SEC_OP_SET_PASSWORD,
	SEC_OP_UNLOCK,
	SEC_OP_ERASE,
	SEC_OP_FREEZE,
	SEC_OP_DISABLE,
};

static const char *sec_op_name(enum sec_op op)
{
	switch (op) {
	case SEC_OP_SET_PASSWORD:
		return "SECURITY SET PASSWORD";
	case SEC_OP_UNLOCK:
		return "SECURITY UNLOCK";
	case SEC_OP_ERASE:
		return "SECURITY ERASE UNIT";
	case SEC_OP_FREEZE:
		return "SECURITY FREEZE LOCK";
	case SEC_OP_DISABLE:
		return "SECURITY DISABLE PASSWORD";
	}
	return "SECURITY ?";
}

/* ==========================================================================
 * Decoding (pure)
 * ========================================================================== */

/* An erase time word -> minutes (0 = not reported).  The value that means
 * "more than the field holds" is returned as the field's maximum. */
static uint32_t sec_erase_minutes(uint16_t w)
{
	if (w == 0 || w == 0xFFFF)
		return 0;
	if (w & 0x8000) {
		/* extended format: bits 14:0, 2-minute units */
		uint32_t v = w & 0x7FFF;

		return v * 2;
	}
	/* short format: bits 7:0, 2-minute units (bits 14:8 reserved) */
	return (uint32_t)(w & 0xFF) * 2;
}

void ata_security_parse(struct ata_device *dev, const uint16_t *id)
{
	struct ata_security *s;
	uint16_t w128, w82;
	int cs_valid, cs_says;

	if (!dev || !id)
		return;
	s = &dev->security;
	kmemset(s, 0, sizeof(*s));

	w128 = id[ATA_ID_DLF];
	w82 = id[ATA_ID_COMMAND_SET_1];
	s->word128 = w128;

	/* Word 82 says whether the feature set exists at all; word 128 bit 0
	 * says the same thing again.  A word that reads 0000h or FFFFh is not
	 * implemented and carries no information. */
	cs_valid = (w82 != 0x0000 && w82 != 0xFFFF);
	cs_says = cs_valid && (w82 & ATA_ID_CS1_SECURITY);
	if (w128 == 0xFFFF)
		w128 = 0;

	s->supported = (uint8_t)((w128 & ATA_ID_SEC_SUPPORTED) ? 1 : 0);
	if (cs_valid && cs_says != s->supported) {
		/* Some bridges and old drives disagree with themselves.  The
		 * status word is what the state machine reports from, so it
		 * wins -- but say so. */
		kprintf(SEC_PFX "%s: security support inconsistent (word 82 "
				"%s, word 128 %s), using word 128\n",
			dev->name, cs_says ? "yes" : "no",
			s->supported ? "yes" : "no");
	}
	if (!s->supported) {
		dev->flags &= ~ATA_DFLAG_SECURITY;
		return;
	}
	dev->flags |= ATA_DFLAG_SECURITY;

	s->enabled = (uint8_t)((w128 & ATA_ID_SEC_ENABLED) ? 1 : 0);
	s->locked = (uint8_t)((w128 & ATA_ID_SEC_LOCKED) ? 1 : 0);
	s->frozen = (uint8_t)((w128 & ATA_ID_SEC_FROZEN) ? 1 : 0);
	s->count_expired = (uint8_t)((w128 & ATA_ID_SEC_COUNT_EXPIRED) ? 1 : 0);
	s->enhanced_erase = (uint8_t)((w128 & ATA_ID_SEC_ENH_ERASE) ? 1 : 0);
	s->level_max = (uint8_t)((w128 & ATA_ID_SEC_LEVEL_MAX) ? 1 : 0);
	s->erase_time_min = sec_erase_minutes(id[ATA_ID_ERASE_TIME]);
	s->enh_erase_time_min =
		s->enhanced_erase ? sec_erase_minutes(id[ATA_ID_ENH_ERASE_TIME])
				  : 0;
	s->master_pw_rev = id[ATA_ID_MASTER_PW_REV];
	if (s->master_pw_rev == 0xFFFF)
		s->master_pw_rev = 0;

	/* A locked drive with security "not enabled" is a contradiction the
	 * specification does not allow; keep what the drive said but warn. */
	WARN_RATELIMIT(s->locked && !s->enabled,
		       SEC_PFX "%s: locked but security not enabled (word "
			       "128 0x%04x)",
		       dev->name, w128);
}

/* Append to a bounded buffer; returns the new length (never past cap-1). */
static int sec_append(char *buf, int cap, int len, const char *s)
{
	while (*s && len < cap - 1)
		buf[len++] = *s++;
	if (cap > 0)
		buf[len < cap ? len : cap - 1] = '\0';
	return len;
}

int ata_security_state_str(const struct ata_device *dev, char *buf, int cap)
{
	const struct ata_security *s;
	int len = 0;
	char tmp[64];

	if (!buf || cap <= 0)
		return 0;
	buf[0] = '\0';
	if (!dev)
		return 0;
	s = &dev->security;
	if (!s->supported)
		return sec_append(buf, cap, 0, "not supported");

	len = sec_append(buf, cap, len,
			 s->enabled ? "enabled" : "not enabled");
	len = sec_append(buf, cap, len, s->locked ? ", locked" : ", not locked");
	len = sec_append(buf, cap, len, s->frozen ? ", frozen" : ", not frozen");
	if (s->count_expired)
		len = sec_append(buf, cap, len, ", count expired");
	if (s->enabled)
		len = sec_append(buf, cap, len,
				 s->level_max ? ", level maximum"
					      : ", level high");
	if (s->enhanced_erase)
		len = sec_append(buf, cap, len, ", enhanced erase");
	if (s->erase_time_min) {
		ksnprintf(tmp, sizeof(tmp), ", erase %s%u min",
			  (s->erase_time_min >= 508 &&
			   !(dev->id[ATA_ID_ERASE_TIME] & 0x8000))
				  ? ">"
				  : "",
			  s->erase_time_min);
		len = sec_append(buf, cap, len, tmp);
	}
	if (s->enh_erase_time_min) {
		ksnprintf(tmp, sizeof(tmp), ", enhanced erase %u min",
			  s->enh_erase_time_min);
		len = sec_append(buf, cap, len, tmp);
	}
	if (s->master_pw_rev && s->master_pw_rev != 0xFFFE) {
		ksnprintf(tmp, sizeof(tmp), ", master password rev 0x%04x",
			  s->master_pw_rev);
		len = sec_append(buf, cap, len, tmp);
	}
	return len;
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/* Clear memory that held a password in a way the compiler cannot drop as
 * a dead store before kfree(). */
static void sec_wipe(void *p, size_t n)
{
	volatile uint8_t *v = (volatile uint8_t *)p;

	for (size_t i = 0; i < n; i++)
		v[i] = 0;
	__asm__ volatile("" ::: "memory");
}

/* Build the 512-byte data block of F1h/F2h/F4h/F6h.  `pw' may be NULL (an
 * all-zero password -- what ERASE UNIT with an unset password uses). */
static void sec_build_block(uint16_t *blk, uint16_t control,
			    const uint8_t pw[ATA_SEC_PW_LEN], uint16_t master_id)
{
	uint8_t *bytes = (uint8_t *)blk;

	kmemset(blk, 0, ATA_SECT_SIZE);
	blk[0] = control;
	if (pw)
		kmemcpy(bytes + 2, pw, ATA_SEC_PW_LEN);
	blk[ATA_SEC_MASTER_ID_WORD] = master_id;
}

/* Precondition check against the cached state.  0 to go ahead, else the
 * errno the caller returns. */
static int sec_check_state(struct ata_device *dev, enum sec_op op, int master)
{
	const struct ata_security *s = &dev->security;

	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (dev->class != ATA_DEV_ATA && dev->class != ATA_DEV_ZAC &&
	    dev->class != ATA_DEV_ATAPI)
		return -EOPNOTSUPP;
	if (!s->supported)
		return -EOPNOTSUPP;

	switch (op) {
	case SEC_OP_FREEZE:
		/* FREEZE LOCK on a frozen drive is harmless (and allowed);
		 * on a locked drive it is aborted. */
		if (s->locked)
			goto locked;
		return 0;
	case SEC_OP_SET_PASSWORD:
	case SEC_OP_DISABLE:
		if (s->frozen)
			goto frozen;
		if (s->locked)
			goto locked;
		if (op == SEC_OP_DISABLE && !s->enabled)
			return -EINVAL; /* nothing to disable */
		return 0;
	case SEC_OP_UNLOCK:
		if (s->frozen)
			goto frozen;
		if (s->count_expired)
			goto expired;
		/* UNLOCK of an unlocked drive with security enabled is a
		 * password check the drive answers; allowed. */
		(void)master;
		return 0;
	case SEC_OP_ERASE:
		if (s->frozen)
			goto frozen;
		if (s->count_expired)
			goto expired;
		return 0;
	}
	return -EINVAL;

frozen:
	kprintf(SEC_PFX "%s: %s refused: security is frozen (power cycle the "
			"drive to unfreeze)\n",
		dev->name, sec_op_name(op));
	return -EACCES;
locked:
	kprintf(SEC_PFX "%s: %s refused: the drive is locked (unlock it "
			"first)\n",
		dev->name, sec_op_name(op));
	return -EACCES;
expired:
	kprintf(SEC_PFX "%s: %s refused: unlock attempt counter expired "
			"(power cycle the drive)\n",
		dev->name, sec_op_name(op));
	return -EACCES;
}

/* Issue one security command.  `blk' is the 512-byte data block or NULL
 * for the non-data commands.  On -EIO the result registers are logged
 * (never the data). */
static int sec_exec(struct ata_device *dev, uint8_t command, uint16_t *blk,
		    uint32_t timeout_ms)
{
	struct ata_taskfile tf;
	int ret;

	kmemset(&tf, 0, sizeof(tf));
	tf.command = command;
	tf.device = 0;
	tf.flags = ATA_TFLAG_DEVICE | ATA_TFLAG_ISADDR;
	if (blk) {
		/* One 512-byte block (count is not used by these commands
		 * but set the way the drives that care expect it). */
		tf.flags |= ATA_TFLAG_WRITE;
		tf.nsect = 1;
		ret = ata_exec_internal(dev, &tf, NULL, ATA_PROT_PIO_OUT, blk,
					ATA_SECT_SIZE, ATA_DIR_TO_DEVICE,
					timeout_ms,
					ATA_QCFLAG_RESULT_TF |
						ATA_QCFLAG_QUIET);
	} else {
		ret = ata_exec_internal(dev, &tf, NULL, ATA_PROT_NODATA, NULL,
					0, ATA_DIR_NONE, timeout_ms,
					ATA_QCFLAG_RESULT_TF |
						ATA_QCFLAG_QUIET);
	}

	if (ret == -EIO) {
		char st[48], er[48];

		ata_status_str(tf.status, st, sizeof(st));
		ata_error_str(tf.error, er, sizeof(er));
		kprintf(SEC_PFX "%s: %s failed: status 0x%02x %s error 0x%02x "
				"%s%s\n",
			dev->name, ata_cmd_name(command), tf.status, st,
			tf.error, er,
			(tf.error & ATA_ABORTED) ? " (wrong password or state)"
						 : "");
		/* An aborted security command is the drive saying "no":
		 * the password was wrong or the state does not permit it. */
		if (tf.error & ATA_ABORTED)
			ret = -EACCES;
	} else if (ret) {
		WARN_RATELIMIT(1, SEC_PFX "%s: %s: error %d", dev->name,
			       ata_cmd_name(command), ret);
	}
	return ret;
}

/* Re-read IDENTIFY and refresh the security view (and the copy of the
 * security words in dev->id) under the device lock.  A failure only means
 * the cached state is stale; it is logged and otherwise ignored. */
static void sec_refresh(struct ata_device *dev)
{
	uint16_t *id = kalloc(ATA_ID_BYTES);
	int ret;

	if (!id)
		return;
	ret = ata_dev_read_id(dev, dev->class, id);
	if (ret == 0) {
		ata_dev_lock(dev);
		dev->id[ATA_ID_DLF] = id[ATA_ID_DLF];
		dev->id[ATA_ID_ERASE_TIME] = id[ATA_ID_ERASE_TIME];
		dev->id[ATA_ID_ENH_ERASE_TIME] = id[ATA_ID_ENH_ERASE_TIME];
		dev->id[ATA_ID_MASTER_PW_REV] = id[ATA_ID_MASTER_PW_REV];
		dev->id[ATA_ID_COMMAND_SET_1] = id[ATA_ID_COMMAND_SET_1];
		dev->id[ATA_ID_CFS_ENABLE_1] = id[ATA_ID_CFS_ENABLE_1];
		ata_security_parse(dev, id);
		ata_dev_unlock(dev);
	} else {
		kprintf(SEC_PFX "%s: re-reading IDENTIFY after a security "
				"command failed (%d); state may be stale\n",
			dev->name, ret);
	}
	kfree(id);
}

static void sec_log_state(struct ata_device *dev, const char *what)
{
	char st[160];

	ata_security_state_str(dev, st, sizeof(st));
	kprintf(SEC_PFX "%s: %s: security %s\n", dev->name, what, st);
}

/* After a successful UNLOCK of a locked drive the media became accessible:
 * capacity and partitions may only now be readable.  Ask error handling to
 * revalidate the device (which re-reads IDENTIFY and the capacity). */
static void sec_request_revalidate(struct ata_device *dev)
{
	dev->flags |= ATA_DFLAG_NEEDS_REVALIDATE;
	if (dev->xport && dev->xport->schedule_eh)
		dev->xport->schedule_eh(dev, AHCI_EH_REVALIDATE);
}

/* ==========================================================================
 * The operations
 * ========================================================================== */

int ata_security_set_password(struct ata_device *dev, int master,
			      const uint8_t pw[ATA_SEC_PW_LEN], int level_max,
			      uint16_t master_id)
{
	uint16_t *blk;
	uint16_t control = 0;
	int ret;

	if (!dev || !pw)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	ata_mutex_lock(&g_ata_sec_mutex);
	ret = sec_check_state(dev, SEC_OP_SET_PASSWORD, master);
	if (ret)
		goto out;

	blk = kalloc(ATA_SECT_SIZE);
	if (!blk) {
		ret = -ENOMEM;
		goto out;
	}
	if (master)
		control |= ATA_SEC_CTRL_MASTER;
	if (level_max)
		control |= ATA_SEC_CTRL_LEVEL_MAX;
	/* The master password identifier is only meaningful for a master
	 * password; 0000h and FFFFh leave the drive's value unchanged. */
	sec_build_block(blk, control, pw, master ? master_id : 0);

	ret = sec_exec(dev, ATA_CMD_SEC_SET_PASS, blk, ATA_TMOUT_INTERNAL_MS);
	sec_wipe(blk, ATA_SECT_SIZE);
	kfree(blk);

	if (ret == 0) {
		kprintf(SEC_PFX "%s: %s password set (level %s%s)\n",
			dev->name, master ? "master" : "user",
			level_max ? "maximum" : "high",
			(master && master_id && master_id != 0xFFFF)
				? ", new master revision"
				: "");
		sec_refresh(dev);
		sec_log_state(dev, "after SET PASSWORD");
	}
out:
	ata_mutex_unlock(&g_ata_sec_mutex);
	return ret;
}

int ata_security_unlock(struct ata_device *dev, int master,
			const uint8_t pw[ATA_SEC_PW_LEN])
{
	uint16_t *blk;
	int was_locked;
	int ret;

	if (!dev || !pw)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	ata_mutex_lock(&g_ata_sec_mutex);
	ret = sec_check_state(dev, SEC_OP_UNLOCK, master);
	if (ret)
		goto out;
	was_locked = dev->security.locked;

	blk = kalloc(ATA_SECT_SIZE);
	if (!blk) {
		ret = -ENOMEM;
		goto out;
	}
	sec_build_block(blk, master ? ATA_SEC_CTRL_MASTER : 0, pw, 0);
	ret = sec_exec(dev, ATA_CMD_SEC_UNLOCK, blk, ATA_TMOUT_INTERNAL_MS);
	sec_wipe(blk, ATA_SECT_SIZE);
	kfree(blk);

	/* Refresh in every case: a wrong password decrements the drive's
	 * attempt counter and may set "count expired". */
	sec_refresh(dev);
	if (ret == 0) {
		kprintf(SEC_PFX "%s: unlocked with the %s password\n",
			dev->name, master ? "master" : "user");
		if (was_locked && !dev->security.locked)
			sec_request_revalidate(dev);
	} else if (dev->security.count_expired) {
		kprintf(SEC_PFX "%s: unlock attempt counter expired; further "
				"attempts need a power cycle\n",
			dev->name);
	}
	sec_log_state(dev, "after UNLOCK");
out:
	ata_mutex_unlock(&g_ata_sec_mutex);
	return ret;
}

/* The ERASE UNIT timeout: twice the drive's estimate plus a margin, at
 * least ATA_TMOUT_SEC_ERASE_MIN_MS, a day when the drive says nothing. */
static uint32_t sec_erase_timeout_ms(const struct ata_device *dev,
				     int enhanced)
{
	const struct ata_security *s = &dev->security;
	uint64_t minutes = enhanced ? s->enh_erase_time_min
				    : s->erase_time_min;
	uint16_t raw = dev->id[enhanced ? ATA_ID_ENH_ERASE_TIME
					: ATA_ID_ERASE_TIME];
	uint64_t ms;

	if (minutes == 0) {
		ms = SEC_ERASE_UNKNOWN_TMOUT_MS;
	} else {
		/* "more than the field can say": the estimate is a lower
		 * bound, so allow the unknown-case day on top */
		int open_ended =
			(raw & 0x8000)
				? ((raw & 0x7FFF) == SEC_ERASE_TIME_MAX_EXT)
				: ((raw & 0xFF) == SEC_ERASE_TIME_MAX_SHORT);

		ms = minutes * 60ULL * 1000ULL * 2ULL + SEC_ERASE_MARGIN_MS;
		if (open_ended)
			ms += SEC_ERASE_UNKNOWN_TMOUT_MS;
	}
	if (ms < ATA_TMOUT_SEC_ERASE_MIN_MS)
		ms = ATA_TMOUT_SEC_ERASE_MIN_MS;
	if (ms > SEC_ERASE_MAX_TMOUT_MS)
		ms = SEC_ERASE_MAX_TMOUT_MS;
	return (uint32_t)ms;
}

/* Is any part of the disk mounted (claimed by a filesystem)?  Takes the
 * registry mutex, which is the outermost lock -- the caller holds nothing
 * else of the storage stack. */
static int sec_disk_in_use(struct gendisk *disk)
{
	int busy = 0;

	if (!disk)
		return 0;
	blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->bdev.holder || (disk->bdev.flags & BDEV_F_EXCL))
		busy = 1;
	for (int i = 1; i <= disk->nr_parts && i <= DISK_MAX_PARTS && !busy;
	     i++) {
		struct blk_partition *p = disk->part[i];

		if (p && (p->bdev.holder || (p->bdev.flags & BDEV_F_EXCL)))
			busy = 1;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return busy;
}

int ata_security_erase(struct ata_device *dev, int master,
		       const uint8_t pw[ATA_SEC_PW_LEN], int enhanced)
{
	struct gendisk *disk;
	struct blk_queue *q = NULL;
	uint16_t *blk = NULL;
	uint32_t tmo;
	uint64_t t0;
	int ret;

	if (!dev)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	/* The mount check takes the registry mutex briefly, holding nothing
	 * else (lock order: security mutex -> eh_mutex -> registry mutex; the
	 * check needs none of the others, so it simply runs first).  A mount
	 * racing with an erase the administrator started is the
	 * administrator's race; the drive is erased either way. */
	disk = dev->disk;
	if (sec_disk_in_use(disk)) {
		kprintf(SEC_PFX "%s: SECURITY ERASE refused: a filesystem on "
				"the disk is mounted\n",
			dev->name);
		return -EBUSY;
	}

	ata_mutex_lock(&g_ata_sec_mutex);
	ret = sec_check_state(dev, SEC_OP_ERASE, master);
	if (ret)
		goto out;
	if (enhanced && !dev->security.enhanced_erase) {
		kprintf(SEC_PFX "%s: enhanced erase not supported by the "
				"drive\n",
			dev->name);
		ret = -EOPNOTSUPP;
		goto out;
	}

	blk = kalloc(ATA_SECT_SIZE);
	if (!blk) {
		ret = -ENOMEM;
		goto out;
	}
	sec_build_block(blk,
			(uint16_t)((master ? ATA_SEC_CTRL_MASTER : 0) |
				   (enhanced ? ATA_SEC_CTRL_ENHANCED : 0)),
			pw, 0);
	tmo = sec_erase_timeout_ms(dev, enhanced);

	/* Nothing may reach the drive between ERASE PREPARE and ERASE UNIT:
	 * stop the disk's queue and wait for what is in flight. */
	if (disk && disk->queue) {
		q = disk->queue;
		blk_queue_stop(q);
		ret = blk_queue_drain(q, 30000);
		if (ret) {
			kprintf(SEC_PFX "%s: SECURITY ERASE: request queue did "
					"not drain (%d)\n",
				dev->name, ret);
			goto out_restart;
		}
	}

	kprintf(SEC_PFX "%s: starting %s security erase with the %s password; "
			"timeout %u min -- ALL DATA ON THE DRIVE WILL BE "
			"DESTROYED\n",
		dev->name, enhanced ? "enhanced" : "normal",
		master ? "master" : "user", tmo / 60000);

	ret = sec_exec(dev, ATA_CMD_SEC_ERASE_PREP, NULL,
		       ATA_TMOUT_INTERNAL_MS);
	if (ret)
		goto out_restart;

	t0 = hrtimer_now_ns();
	ret = sec_exec(dev, ATA_CMD_SEC_ERASE_UNIT, blk, tmo);
	if (ret == 0)
		kprintf(SEC_PFX "%s: security erase completed in %llu s\n",
			dev->name,
			(unsigned long long)((hrtimer_now_ns() - t0) /
					     1000000000ULL));
	else
		kprintf(SEC_PFX "%s: security erase FAILED after %llu s (%d)\n",
			dev->name,
			(unsigned long long)((hrtimer_now_ns() - t0) /
					     1000000000ULL),
			ret);

	/* A successful erase also disables the user password; and the
	 * contents are gone -- partitions and capacity must be looked at
	 * again. */
	sec_refresh(dev);
	sec_log_state(dev, "after ERASE UNIT");
	if (ret == 0)
		sec_request_revalidate(dev);

out_restart:
	if (q)
		blk_queue_start(q);
out:
	if (blk) {
		sec_wipe(blk, ATA_SECT_SIZE);
		kfree(blk);
	}
	ata_mutex_unlock(&g_ata_sec_mutex);
	return ret;
}

int ata_security_freeze(struct ata_device *dev)
{
	int ret;

	if (!dev)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	/* FREEZE LOCK is part of the configuration policy the core applies
	 * after every probe and reset -- with the port's eh_mutex HELD.  It
	 * therefore takes neither the security mutex (whose holders take the
	 * eh_mutex inside: the order would invert) nor ata_dev_lock() (the
	 * eh_mutex is not recursive).  The fields it updates are the
	 * configure path's to write under that mutex. */
	ret = sec_check_state(dev, SEC_OP_FREEZE, 0);
	if (ret)
		return ret;
	if (dev->security.frozen) {
		/* Already frozen (by the firmware or by us): done. */
		dev->flags |= ATA_DFLAG_FROZEN_SEC;
		return 0;
	}
	ret = sec_exec(dev, ATA_CMD_SEC_FREEZE_LOCK, NULL,
		       ATA_TMOUT_INTERNAL_MS);
	if (ret == 0) {
		dev->flags |= ATA_DFLAG_FROZEN_SEC;
		/* The frozen bit is volatile state; update the cache without
		 * another IDENTIFY. */
		dev->security.frozen = 1;
		dev->security.word128 |= ATA_ID_SEC_FROZEN;
		dev->id[ATA_ID_DLF] |= ATA_ID_SEC_FROZEN;
		kprintf(SEC_PFX "%s: security frozen (SECURITY FREEZE LOCK)\n",
			dev->name);
	} else if (ret == -EACCES || ret == -EIO) {
		/* Some drives abort FREEZE LOCK when security is not
		 * enabled; that is not an error worth failing the probe for. */
		kprintf(SEC_PFX "%s: SECURITY FREEZE LOCK not accepted (%d)\n",
			dev->name, ret);
	}
	return ret;
}

int ata_security_disable(struct ata_device *dev, int master,
			 const uint8_t pw[ATA_SEC_PW_LEN])
{
	uint16_t *blk;
	int ret;

	if (!dev || !pw)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	ata_mutex_lock(&g_ata_sec_mutex);
	ret = sec_check_state(dev, SEC_OP_DISABLE, master);
	if (ret)
		goto out;
	blk = kalloc(ATA_SECT_SIZE);
	if (!blk) {
		ret = -ENOMEM;
		goto out;
	}
	sec_build_block(blk, master ? ATA_SEC_CTRL_MASTER : 0, pw, 0);
	ret = sec_exec(dev, ATA_CMD_SEC_DISABLE_PASS, blk,
		       ATA_TMOUT_INTERNAL_MS);
	sec_wipe(blk, ATA_SECT_SIZE);
	kfree(blk);
	sec_refresh(dev);
	if (ret == 0)
		kprintf(SEC_PFX "%s: user password disabled (with the %s "
				"password)\n",
			dev->name, master ? "master" : "user");
	sec_log_state(dev, "after DISABLE PASSWORD");
out:
	ata_mutex_unlock(&g_ata_sec_mutex);
	return ret;
}
