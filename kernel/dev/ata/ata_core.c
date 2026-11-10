// LikeOS ATA layer - device lifecycle, IDENTIFY, queued commands, ata_mutex
//
// This file is the heart of the ATA device layer.  It owns:
//
//   * struct ata_mutex, the sleeping mutex the AHCI driver uses for its
//     per-port error-handling serialisation (port->eh_mutex), built the
//     same way as the USB mass storage driver's per-device I/O lock and
//     usable before the scheduler exists (boot probing is polled);
//   * the queued-command helpers: allocation, reference counting,
//     completion (ata_qc_complete, which every transport calls exactly
//     once per command), the synchronous execution path (ata_qc_exec and
//     ata_exec_internal) with the transport's busy/error-handling answers
//     retried and a last-resort recovery for a command the transport lost;
//   * error decoding (status/error registers -> AC_ERR_* -> -errno) and the
//     established failed-command report;
//   * IDENTIFY (PACKET) DEVICE: issuing it, classifying the device when the
//     signature was not conclusive, and parsing the page into struct
//     ata_device (ata_id_parse -- pure, the host tests run it on crafted
//     pages);
//   * the device lifecycle: allocation, probe, revalidation after resets
//     (with the identity check that takes a swapped drive offline),
//     detach;
//   * the device quirk table;
//   * the registration worker: at run time a disk is added to (or removed
//     from) the block layer by a worker thread, not by the prober.  The
//     lock order allows disk_add() under the port's eh_mutex, but
//     disk_add() also scans the partition table with real I/O on that
//     very port, and the first read error that needs error handling
//     would wait for the eh_mutex (and the EH thread) the prober is
//     holding -- a hot-plugged flaky disk would wedge its port.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata_dev_internal.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ==========================================================================
 * Small utilities
 * ========================================================================== */

volatile int g_ata_system_up;

uint64_t ata_self_id(void)
{
	task_t *cur = sched_current();

	return cur ? (uint64_t)(int64_t)cur->id : 0;
}

/* Once anything runs with the scheduler and interrupts live, hrtimers and
 * kernel threads are available for good. */
void ata_note_system_up(void)
{
	if (unlikely(!g_ata_system_up) && ata_can_sleep())
		g_ata_system_up = 1;
}

void ata_dev_printk(const struct ata_device *dev, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	__builtin_va_start(ap, fmt);
	kvsnprintf(line, sizeof(line), fmt, ap);
	__builtin_va_end(ap);
	kprintf("[ata] %s: %s", dev ? dev->name : "ata?", line);
}

/* Sleep (or busy-wait before the scheduler) for about `ms' milliseconds. */
static void ata_delay_ms(uint32_t ms)
{
	if (ata_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0)
			; /* a signal cut it short: sleep the rest */
	} else {
		while (ms--)
			lapic_delay_us(1000);
	}
}

/* ==========================================================================
 * struct ata_mutex
 *
 * A flag and an owner under a small spinlock.  A contended locker that may
 * sleep publishes TASK_BLOCKED with the flag's address as its wait channel
 * while still holding the spinlock, so the release (which clears the flag
 * under the same spinlock and then wakes the channel) can never fall
 * between the locker's check and its sleep.  Before the scheduler exists,
 * or with interrupts off, a free mutex is taken at once and a held one is
 * spun on -- boot is single-threaded, so then only another processor can
 * hold it.
 * ========================================================================== */

void ata_mutex_init(struct ata_mutex *m, const char *name)
{
	BUG_ON(!m);
	m->locked = 0;
	m->owner = (uint64_t)-1;
	m->name = name ? name : "ata_mutex";
	spinlock_init(&m->wait_lock, m->name);
	m->contended = 0;
}

void ata_mutex_lock(struct ata_mutex *m)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? (uint64_t)(int64_t)cur->id : 0;
	int can_sleep = ata_can_sleep();
	int counted = 0;
	uint64_t spins = 0;
	int spin_warned = 0;

	BUG_ON(!m);
	if (can_sleep)
		might_sleep();

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&m->wait_lock, &fl);
		if (!m->locked) {
			m->locked = 1;
			m->owner = me;
			spin_unlock_irqrestore(&m->wait_lock, fl);
			task_sleep_lock_taken();
			return;
		}
		/* Not recursive: the owner locking again would sleep forever
		 * waiting for itself. */
		if (cur && m->owner == me) {
			spin_unlock_irqrestore(&m->wait_lock, fl);
			kprintf("[ata] mutex %s: recursive lock by task %llu\n",
				m->name, (unsigned long long)me);
			BUG();
		}
		if (!counted) {
			m->contended++;
			counted = 1;
		}
		if (can_sleep && cur) {
			cur->wait_channel = (void *)&m->locked;
			task_set_blocked(cur);
			spin_unlock_irqrestore(&m->wait_lock, fl);
			sched_schedule();
			cur->wait_channel = NULL;
			continue;
		}
		spin_unlock_irqrestore(&m->wait_lock, fl);
		/* Boot / interrupts off: only another processor can hold it.
		 * Say so once if that takes suspiciously long (a recursion at
		 * boot would otherwise be a silent hang). */
		__asm__ volatile("pause" ::: "memory");
		if (++spins == 50000000ULL && !spin_warned) {
			spin_warned = 1;
			kprintf("[ata] mutex %s: spinning on a held mutex "
				"(owner %llu) with no scheduler -- recursion?\n",
				m->name, (unsigned long long)m->owner);
		}
	}
}

int ata_mutex_trylock(struct ata_mutex *m)
{
	uint64_t fl;
	int taken = 0;

	BUG_ON(!m);
	spin_lock_irqsave(&m->wait_lock, &fl);
	if (!m->locked) {
		m->locked = 1;
		m->owner = ata_self_id();
		taken = 1;
	}
	spin_unlock_irqrestore(&m->wait_lock, fl);
	if (taken)
		task_sleep_lock_taken();
	return taken;
}

void ata_mutex_unlock(struct ata_mutex *m)
{
	uint64_t fl;
	uint64_t me = ata_self_id();

	BUG_ON(!m);
	spin_lock_irqsave(&m->wait_lock, &fl);
	if (unlikely(!m->locked)) {
		spin_unlock_irqrestore(&m->wait_lock, fl);
		WARN(1, "[ata] mutex %s: unlock of a free mutex", m->name);
		return;
	}
	if (unlikely(m->owner != me)) {
		uint64_t owner = m->owner;

		spin_unlock_irqrestore(&m->wait_lock, fl);
		WARN(1, "[ata] mutex %s: unlock by task %llu, owner is %llu",
		     m->name, (unsigned long long)me,
		     (unsigned long long)owner);
		return;
	}
	m->locked = 0;
	m->owner = (uint64_t)-1;
	spin_unlock_irqrestore(&m->wait_lock, fl);
	task_sleep_lock_dropped();
	/* Nobody can be parked on it before the scheduler exists. */
	if (sched_current())
		sched_wake_channel((void *)&m->locked);
}

int ata_mutex_held(const struct ata_mutex *m)
{
	if (!m)
		return 0;
	return m->locked && m->owner == ata_self_id();
}

int ata_mutex_release_if_owner(struct ata_mutex *m, uint64_t task_id)
{
	uint64_t fl;
	int released = 0;

	if (!m)
		return 0;
	spin_lock_irqsave(&m->wait_lock, &fl);
	if (m->locked && m->owner == task_id) {
		m->locked = 0;
		m->owner = (uint64_t)-1;
		released = 1;
	}
	spin_unlock_irqrestore(&m->wait_lock, fl);
	if (released) {
		kprintf("[ata] mutex %s: released for dead task %llu\n",
			m->name, (unsigned long long)task_id);
		if (sched_current())
			sched_wake_channel((void *)&m->locked);
	}
	return released;
}

/* ==========================================================================
 * Error-handling context
 *
 * Device management commands must be serialised with the port's error
 * handling.  Those issued while the caller holds the port's eh_mutex (the
 * probe and revalidation paths, which run inside error handling or the
 * boot probe, and callers inside ata_dev_lock()) must carry ATA_QCFLAG_EH:
 * a frozen port only accepts EH commands, and the error handling that
 * would thaw it cannot run while the caller holds its mutex.
 * ========================================================================== */

void ata_eh_ctx_enter(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);

	if (!p)
		return;
	if (p->eh_depth == 0)
		p->eh_owner = ata_self_id();
	p->eh_depth++;
}

void ata_eh_ctx_exit(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);

	if (!p)
		return;
	if (WARN_ON(p->eh_depth == 0))
		return;
	if (--p->eh_depth == 0)
		p->eh_owner = (uint64_t)-1;
}

int ata_dev_eh_held(struct ata_device *dev)
{
	struct ata_dev_priv *p;

	if (!dev)
		return 0;
	p = ata_dev_priv(dev);
	if (p && p->eh_depth && p->eh_owner == ata_self_id())
		return 1;
	/* On AHCI the mutex itself answers, whoever took it (the port probe,
	 * the EH thread, a transport lock() call). */
	if (dev->xport == &ahci_ata_transport && dev->xport_priv) {
		struct ahci_link *link = ahci_dev_link(dev);

		if (link && link->port)
			return ata_mutex_held(&link->port->eh_mutex);
	}
	return 0;
}

void ata_dev_lock(struct ata_device *dev)
{
	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	if (dev->xport && dev->xport->lock)
		dev->xport->lock(dev);
	ata_eh_ctx_enter(dev);
}

void ata_dev_unlock(struct ata_device *dev)
{
	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	ata_eh_ctx_exit(dev);
	if (dev->xport && dev->xport->unlock)
		dev->xport->unlock(dev);
}

int ata_mgmt_enter(struct ata_device *dev)
{
	if (ata_dev_eh_held(dev))
		return 0;
	if (!dev->xport || !dev->xport->lock)
		return 0;
	ata_dev_lock(dev);
	return 1;
}

void ata_mgmt_exit(struct ata_device *dev, int took)
{
	if (took)
		ata_dev_unlock(dev);
}

/* ==========================================================================
 * Device quirks
 *
 * Patterns use '*' (any run) and '?' (one character).  Entries are the
 * well-documented firmware problems: queued TRIM that corrupts data on
 * several SSD families, and the families whose "read zeroes after TRIM"
 * claim is known to hold (only those advertise discard_zeroes_data and get
 * WRITE ZEROES by TRIM).  Everything else trusts the IDENTIFY data.
 * ========================================================================== */

struct ata_quirk_entry {
	const char *model;
	const char *firmware; /* NULL: any revision */
	uint32_t quirks;
};

static const struct ata_quirk_entry ata_quirk_table[] = {
	/* Queued TRIM corrupts data on these; their non-queued TRIM is fine
	 * and verified to read back zeroes. */
	{ "Crucial_CT*M500*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Crucial_CT*M550*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Crucial_CT*MX100*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Micron_M500_*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Micron_M510_*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Micron_M550_*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Samsung SSD 840*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Samsung SSD 850*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Samsung SSD 860*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Samsung SSD 870*", NULL,
	  ATA_QUIRK_NO_NCQ_TRIM | ATA_QUIRK_ZERO_AFTER_TRIM },
	/* Families whose deterministic zeroes after TRIM are trustworthy. */
	{ "INTEL*SSDSC2*", NULL, ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "SSD*INTEL*", NULL, ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "Samsung*SSD*", NULL, ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "SAMSUNG*SSD*", NULL, ATA_QUIRK_ZERO_AFTER_TRIM },
	{ "SAMSUNG*MZ7*", NULL, ATA_QUIRK_ZERO_AFTER_TRIM },
	/* Early SATA drives whose NCQ implementation drops commands. */
	{ "Maxtor 7V300F0", "VA111630", ATA_QUIRK_NO_NCQ },
	{ "Hitachi HTS541080G9SA00", "MB4OC60D", ATA_QUIRK_NO_NCQ },
	{ NULL, NULL, 0 },
};

/* Glob match with '*' and '?' (no character classes: a '[' in a pattern
 * matches itself).  Pure, iterative (no recursion depth on long models). */
static int ata_glob_match(const char *pat, const char *s)
{
	const char *star = NULL, *mark = NULL;

	while (*s) {
		if (*pat == '*') {
			star = pat++;
			mark = s;
		} else if (*pat == '?' || *pat == *s) {
			pat++;
			s++;
		} else if (star) {
			pat = star + 1;
			s = ++mark;
		} else {
			return 0;
		}
	}
	while (*pat == '*')
		pat++;
	return *pat == '\0';
}

uint32_t ata_quirks_lookup(const char *model, const char *firmware)
{
	uint32_t q = 0;

	if (!model)
		return 0;
	for (const struct ata_quirk_entry *e = ata_quirk_table; e->model;
	     e++) {
		if (!ata_glob_match(e->model, model))
			continue;
		if (e->firmware &&
		    (!firmware || !ata_glob_match(e->firmware, firmware)))
			continue;
		q |= e->quirks;
	}
	return q;
}

/* ==========================================================================
 * IDENTIFY strings
 * ========================================================================== */

void ata_id_c_string(const uint16_t *id, char *out, int word_ofs, int len)
{
	int i;

	if (!out)
		return;
	if (!id || len <= 0) {
		if (len >= 0)
			out[0] = '\0';
		return;
	}
	/* ATA strings put the first character in the HIGH byte of each
	 * word (the "byte-swapped" string of every IDENTIFY dump). */
	for (i = 0; i + 1 < len; i += 2) {
		uint16_t w = id[word_ofs + i / 2];

		out[i] = (char)(w >> 8);
		out[i + 1] = (char)(w & 0xFF);
	}
	if (i < len) /* odd length: the last high byte only */
		out[i] = (char)(id[word_ofs + i / 2] >> 8);
	out[len] = '\0';
	/* Trim trailing padding (spaces and NULs). */
	for (i = len - 1; i >= 0 && (out[i] == ' ' || out[i] == '\0'); i--)
		out[i] = '\0';
}

/* Strip leading blanks in place (serial numbers are often right-justified)
 * and turn control characters into '_' so the strings are safe in names,
 * logs and /dev/disk/by-id. */
static void ata_str_tidy(char *s)
{
	char *p = s, *d = s;

	while (*p == ' ')
		p++;
	while (*p) {
		char c = *p++;

		if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F)
			c = '_';
		*d++ = c;
	}
	*d = '\0';
}

/* ==========================================================================
 * IDENTIFY parsing (pure)
 * ========================================================================== */

static int ata_is_pow2(uint32_t v)
{
	return v && !(v & (v - 1));
}

static int ata_word_usable(uint16_t w)
{
	return w != 0x0000 && w != 0xFFFF;
}

/* Words 82/85 are valid when not 0000h/FFFFh; 83, 84 and 87 (and 119,
 * 120, 106, 209) carry the 01b signature in bits 15:14. */
static void ata_id_decode_security(struct ata_device *dev, const uint16_t *id)
{
	struct ata_security *s = &dev->security;
	uint16_t w = id[ATA_ID_DLF];
	uint16_t t;

	kmemset(s, 0, sizeof(*s));
	if (!(ata_word_usable(id[ATA_ID_COMMAND_SET_1]) &&
	      (id[ATA_ID_COMMAND_SET_1] & ATA_ID_CS1_SECURITY)))
		return;
	if (w == 0xFFFF)
		return;
	s->word128 = w;
	s->supported = !!(w & ATA_ID_SEC_SUPPORTED);
	if (!s->supported)
		return;
	s->enabled = !!(w & ATA_ID_SEC_ENABLED);
	s->locked = !!(w & ATA_ID_SEC_LOCKED);
	s->frozen = !!(w & ATA_ID_SEC_FROZEN);
	s->count_expired = !!(w & ATA_ID_SEC_COUNT_EXPIRED);
	s->enhanced_erase = !!(w & ATA_ID_SEC_ENH_ERASE);
	s->level_max = !!(w & ATA_ID_SEC_LEVEL_MAX);
	/* Erase time estimates (words 89/90): bit 15 set = extended format,
	 * bits 14:0 in 2-minute units; otherwise bits 7:0 in 2-minute units
	 * (FFh: more than 508 minutes).  0: not reported. */
	t = id[ATA_ID_ERASE_TIME];
	if (t != 0xFFFF)
		s->erase_time_min = (t & 0x8000) ? (uint32_t)(t & 0x7FFF) * 2
						 : (uint32_t)(t & 0xFF) * 2;
	t = id[ATA_ID_ENH_ERASE_TIME];
	if (t != 0xFFFF)
		s->enh_erase_time_min = (t & 0x8000) ?
						(uint32_t)(t & 0x7FFF) * 2 :
						(uint32_t)(t & 0xFF) * 2;
	t = id[ATA_ID_MASTER_PW_REV];
	s->master_pw_rev = ata_word_usable(t) ? t : 0;
}

static void ata_id_decode_modes(struct ata_device *dev, const uint16_t *id)
{
	uint16_t w53 = id[ATA_ID_FIELD_VALID];

	/* PIO 0..2 are implied by any ATA device; 3 and 4 from word 64. */
	dev->pio_mask = 0x07;
	if (w53 & ATA_ID_FV_64_70)
		dev->pio_mask |= (uint8_t)((id[ATA_ID_PIO_MODES] & 0x03) << 3);
	dev->mwdma_mask = (uint8_t)(id[ATA_ID_MWDMA_MODES] & 0x07);
	dev->udma_mask = (w53 & ATA_ID_FV_88) ?
				 (uint8_t)(id[ATA_ID_UDMA_MODES] & 0x7F) :
				 0;
	/* READ/WRITE MULTIPLE: word 47 bits 7:0 the maximum, word 59 bit 8
	 * says bits 7:0 hold the current setting. */
	if (id[ATA_ID_MULTSECT] & ATA_ID_MULT_VALID)
		dev->multi_count = (uint8_t)(id[ATA_ID_MULTSECT] & 0xFF);
	else
		dev->multi_count = 0;
}

static void ata_id_decode_sata(struct ata_device *dev, const uint16_t *id,
			       uint64_t *flags)
{
	uint16_t w76 = id[ATA_ID_SATA_CAPABILITY];
	uint16_t w77 = id[ATA_ID_SATA_CAPABILITY_2];
	uint16_t w78 = id[ATA_ID_FEATURE_SUPP];
	uint16_t w79 = id[ATA_ID_FEATURE_ENABLED];

	dev->sata_speed_max = 0;
	if (!ata_word_usable(w76))
		return; /* parallel ATA device (behind a bridge) */
	if (w76 & ATA_ID_SATA_GEN3)
		dev->sata_speed_max = 3;
	else if (w76 & ATA_ID_SATA_GEN2)
		dev->sata_speed_max = 2;
	else if (w76 & ATA_ID_SATA_GEN1)
		dev->sata_speed_max = 1;
	if (w76 & ATA_ID_SATA_NCQ)
		*flags |= ATA_DFLAG_NCQ;
	if (w76 & ATA_ID_SATA_HIPM)
		*flags |= ATA_DFLAG_HIPM;
	if (ata_word_usable(w77) && (w77 & ATA_ID_SATA2_NCQ_SEND_RECV))
		*flags |= ATA_DFLAG_NCQ_SEND_RECV;
	if (!ata_word_usable(w78))
		return;
	if (w78 & ATA_ID_FEAT_DIPM) {
		*flags |= ATA_DFLAG_DIPM;
		if (ata_word_usable(w79) && (w79 & ATA_ID_FEAT_DIPM))
			*flags |= ATA_DFLAG_DIPM_ON;
	}
	/* ATA_DFLAG_AN is not a capability: it says the notification is
	 * enabled, which configuration sets once the device accepted it. */
	if (w78 & ATA_ID_FEAT_DEVSLP)
		*flags |= ATA_DFLAG_DEVSLP;
	if (w78 & ATA_ID_FEAT_NCQ_AUTOSENSE)
		*flags |= ATA_DFLAG_NCQ_AUTOSENSE;
}

/* The capability bits ata_id_parse() owns: everything below the state
 * bits (48..63), which belong to the device's life, not to its page. */
#define ATA_DFLAG_CAP_MASK ((1ULL << 48) - 1)

int ata_id_parse_why(struct ata_device *dev, const uint16_t *id,
		     const char **why)
{
	const char *dummy;
	uint64_t flags = 0;
	uint16_t w0, w49, w69, w82, w83, w84, w85, w86, w87, w106, w119, w209;
	int is_cfa, atapi, lba48 = 0, all_zero = 1, all_ones = 1;
	uint32_t quirks;
	uint32_t lss = ATA_SECT_SIZE, pss;
	uint8_t log2pp = 0;

	if (!why)
		why = &dummy;
	*why = NULL;
	if (!dev || !id) {
		*why = "no IDENTIFY page";
		return -EINVAL;
	}
	for (int i = 0; i < ATA_ID_WORDS; i++) {
		if (id[i] != 0x0000)
			all_zero = 0;
		if (id[i] != 0xFFFF)
			all_ones = 0;
	}
	if (all_zero || all_ones) {
		*why = all_zero ? "IDENTIFY page is all zeroes" :
				  "IDENTIFY page is all ones (no device)";
		return -EINVAL;
	}
	if (!ata_id_checksum_ok(id)) {
		*why = "IDENTIFY checksum (word 255) does not add up";
		return -EINVAL;
	}

	w0 = id[ATA_ID_CONFIG];
	is_cfa = (w0 == ATA_ID_CONFIG_CFA);
	atapi = !is_cfa && ata_id_is_atapi(id);
	if (!is_cfa && !atapi && (w0 & ATA_ID_CONFIG_NOT_ATA)) {
		*why = "word 0 describes neither an ATA nor an ATAPI device";
		return -EINVAL;
	}

	if (id != dev->id)
		kmemcpy(dev->id, id, ATA_ID_BYTES);

	/* ---- identity ------------------------------------------------- */
	ata_id_c_string(id, dev->model, ATA_ID_PROD, ATA_ID_PROD_LEN);
	ata_id_c_string(id, dev->serial, ATA_ID_SERNO, ATA_ID_SERNO_LEN);
	ata_id_c_string(id, dev->firmware, ATA_ID_FW_REV, ATA_ID_FW_REV_LEN);
	ata_str_tidy(dev->model);
	ata_str_tidy(dev->serial);
	ata_str_tidy(dev->firmware);
	dev->major_version = ata_word_usable(id[ATA_ID_MAJOR_VER]) ?
				     id[ATA_ID_MAJOR_VER] :
				     0;
	dev->minor_version = ata_word_usable(id[ATA_ID_MINOR_VER]) ?
				     id[ATA_ID_MINOR_VER] :
				     0;
	dev->form_factor = (uint16_t)(id[ATA_ID_FORM_FACTOR] & 0x000F);
	{
		uint16_t rot = id[ATA_ID_ROT_SPEED];

		if (rot == ATA_ID_ROT_NONE)
			dev->rotation_rate = 1;
		else if (rot >= 0x0401 && rot <= 0xFFFE)
			dev->rotation_rate = rot;
		else
			dev->rotation_rate = 0;
	}
	quirks = ata_quirks_lookup(dev->model, dev->firmware);

	if (w0 & ATA_ID_CONFIG_REMOVABLE)
		flags |= ATA_DFLAG_REMOVABLE;

	w49 = id[ATA_ID_CAPABILITY];
	w69 = id[ATA_ID_ADDITIONAL_SUPP];
	w82 = id[ATA_ID_COMMAND_SET_1];
	w83 = id[ATA_ID_COMMAND_SET_2];
	w84 = id[ATA_ID_CFSSE];
	w85 = id[ATA_ID_CFS_ENABLE_1];
	w86 = id[ATA_ID_CFS_ENABLE_2];
	w87 = id[ATA_ID_CSF_DEFAULT];
	w106 = id[ATA_ID_SECTOR_SIZE];
	w119 = id[ATA_ID_COMMAND_SET_3];
	w209 = id[ATA_ID_SECTOR_ALIGNMENT];
	if (!ata_word_usable(w82))
		w82 = 0;
	if (!ata_word_usable(w85))
		w85 = 0;
	if (!ATA_ID_WORD_VALID(w83))
		w83 = 0;
	if (!ATA_ID_WORD_VALID(w84))
		w84 = 0;
	if (!ATA_ID_WORD_VALID(w87))
		w87 = 0;
	if (w86 == 0xFFFF)
		w86 = 0;
	if (!ATA_ID_WORD_VALID(w119))
		w119 = 0;
	if (w69 == 0xFFFF)
		w69 = 0;

	ata_id_decode_modes(dev, id);
	ata_id_decode_sata(dev, id, &flags);
	ata_id_decode_security(dev, id);

	/* ---- feature words common to ATA and ATAPI ----------------------- */
	if (w82 & ATA_ID_CS1_SMART) {
		flags |= ATA_DFLAG_SMART;
		if (w85 & ATA_ID_CS1_SMART)
			flags |= ATA_DFLAG_SMART_ON;
	}
	if (w82 & ATA_ID_CS1_SECURITY)
		flags |= ATA_DFLAG_SECURITY;
	if (w82 & ATA_ID_CS1_WCACHE) {
		flags |= ATA_DFLAG_WCACHE;
		if (w85 & ATA_ID_CS1_WCACHE)
			flags |= ATA_DFLAG_WCACHE_ON;
	}
	if (w82 & ATA_ID_CS1_RLA) {
		flags |= ATA_DFLAG_RLA;
		if (w85 & ATA_ID_CS1_RLA)
			flags |= ATA_DFLAG_RLA_ON;
	}
	if (w83 & ATA_ID_CS2_APM) {
		flags |= ATA_DFLAG_APM;
		dev->apm_level = (w86 & ATA_ID_CS2_APM) ?
					 (uint8_t)(id[ATA_ID_APM_LEVEL] & 0xFF) :
					 0;
	} else {
		dev->apm_level = 0;
	}
	if (w83 & ATA_ID_CS2_AAM) {
		flags |= ATA_DFLAG_AAM;
		dev->aam_level = (w86 & ATA_ID_CS2_AAM) ?
					 (uint8_t)(id[ATA_ID_AAM] & 0xFF) :
					 0;
	} else {
		dev->aam_level = 0;
	}
	if ((w83 & ATA_ID_CS2_PUIS) && (w86 & ATA_ID_CS2_PUIS))
		flags |= ATA_DFLAG_PUIS;
	if (w83 & ATA_ID_CS2_SPINUP_REQ)
		flags |= ATA_DFLAG_SPINUP_REQ;
	if ((w83 & ATA_ID_CS2_DL_MICRO) || (w69 & ATA_ID_ADD_DMA_DL_MICRO))
		flags |= ATA_DFLAG_DL_MICRO;
	if (w84 & ATA_ID_CSE_GPL)
		flags |= ATA_DFLAG_GPL;
	if (w84 & ATA_ID_CSE_UNLOAD)
		flags |= ATA_DFLAG_UNLOAD;
	if (w119 & ATA_ID_CS3_LOG_DMA)
		flags |= ATA_DFLAG_LOG_DMA;
	if (w119 & ATA_ID_CS3_SENSE_DATA)
		flags |= ATA_DFLAG_SENSE_DATA;
	if (id[ATA_ID_MULTSECT] & ATA_ID_SANITIZE_SUPP)
		flags |= ATA_DFLAG_SANITIZE;
	if (w69 & ATA_ID_ADD_NONVOL_WC)
		flags |= ATA_DFLAG_NONVOL_WC;
	if (dev->class == ATA_DEV_ZAC)
		flags |= ATA_DFLAG_ZAC;

	/* World wide name (words 108..111, most significant word first). */
	dev->wwn = 0;
	if ((w84 & ATA_ID_CSE_WWN) || (w87 & ATA_ID_CSE_WWN)) {
		uint64_t wwn = ((uint64_t)id[ATA_ID_WWN] << 48) |
			       ((uint64_t)id[ATA_ID_WWN + 1] << 32) |
			       ((uint64_t)id[ATA_ID_WWN + 2] << 16) |
			       (uint64_t)id[ATA_ID_WWN + 3];

		if (wwn != 0 && wwn != ~0ULL) {
			dev->wwn = wwn;
			flags |= ATA_DFLAG_WWN;
		}
	}

	/* ---- ATAPI: a packet device ------------------------------------- */
	if (atapi) {
		dev->cdb_len = ((w0 & 0x0003) == ATA_ID_CONFIG_CDB16) ? 16 : 12;
		if (dev->cdb_len == 16)
			flags |= ATA_DFLAG_CDB16;
		dev->atapi_type = (uint8_t)ATA_ID_CONFIG_PKT_SET(w0);
		if (w49 & ATA_ID_CAP_DMA)
			flags |= ATA_DFLAG_ATAPI_DMA;
		/* Word 62 bit 15: the device needs DMADIR (bridged PATA). */
		if (id[ATA_ID_SWDMA_MODES] & 0x8000)
			flags |= ATA_DFLAG_ATAPI_DMADIR;
		/* Capacity and block size come from READ CAPACITY. */
		dev->n_sectors = 0;
		dev->n_native_sectors = 0;
		dev->logical_sector_size =
			dev->atapi_type == ATA_ID_CONFIG_PKT_CDROM ? 2048 : 512;
		dev->physical_sector_size = dev->logical_sector_size;
		dev->alignment_offset = 0;
		dev->log2_per_phys = 0;
		dev->cylinders = dev->heads = dev->sectors = 0;
		dev->max_sectors = dev->host_max_sectors ?
					   (dev->host_max_sectors * ATA_SECT_SIZE) /
						   dev->logical_sector_size :
					   ATA_MAX_SECTORS_LBA28;
		if (!dev->max_sectors)
			dev->max_sectors = 1;
		dev->ncq_depth = dev->ncq_depth_dev = 0;
		dev->dsm_max_blocks = 0;
		dev->trim_max_sectors = 0;
		dev->flags = (dev->flags & ~ATA_DFLAG_CAP_MASK) | flags;
		return 0;
	}

	/* ---- ATA: addressing and capacity -------------------------------- */
	if (!(w49 & ATA_ID_CAP_LBA)) {
		*why = "device has no LBA addressing (CHS-only devices are "
		       "not supported)";
		return -EINVAL;
	}
	flags |= ATA_DFLAG_LBA;

	/* 48-bit addressing: supported in word 83 and not disabled (word 86
	 * reflects the enabled state; a device configuration overlay can
	 * switch it off).  A bridge that leaves word 86 zero is trusted on
	 * word 83 alone. */
	if (ata_id_has_lba48(id) &&
	    (w86 == 0 || (w86 & ATA_ID_CS2_LBA48)))
		lba48 = 1;
	{
		uint64_t lba28 = ata_id_u32(id, ATA_ID_LBA_CAPACITY);
		uint64_t cap;

		if (lba28 > ATA_MAX_LBA28)
			lba28 = ATA_MAX_LBA28;
		if (lba48) {
			cap = ata_id_u64(id, ATA_ID_LBA_CAPACITY_2);
			/* ACS-3: words 230..233 when word 69 bit 3 says so */
			if (w69 & ATA_ID_ADD_EXT_CAPACITY) {
				uint64_t ext = ata_id_u64(id,
							  ATA_ID_EXT_CAPACITY);

				if (ext && ext <= ATA_MAX_LBA48 + 1)
					cap = ext;
			}
			if (cap > ATA_MAX_LBA48 + 1)
				cap = ATA_MAX_LBA48 + 1;
			if (cap < lba28)
				cap = lba28;
			flags |= ATA_DFLAG_LBA48;
		} else {
			cap = lba28;
		}
		if (cap == 0) {
			*why = "device reports zero capacity";
			return -EINVAL;
		}
		dev->n_sectors = cap;
	}
	if (dev->n_native_sectors && dev->n_native_sectors > dev->n_sectors)
		flags |= ATA_DFLAG_HPA;

	/* Obsolete default geometry (HDIO_GETGEO), current one if valid. */
	dev->cylinders = id[ATA_ID_CYLS];
	dev->heads = id[ATA_ID_HEADS];
	dev->sectors = id[ATA_ID_SECTORS];
	if ((id[ATA_ID_FIELD_VALID] & 0x0001) && id[ATA_ID_CUR_CYLS] &&
	    id[ATA_ID_CUR_HEADS] && id[ATA_ID_CUR_SECTORS]) {
		dev->cylinders = id[ATA_ID_CUR_CYLS];
		dev->heads = id[ATA_ID_CUR_HEADS];
		dev->sectors = id[ATA_ID_CUR_SECTORS];
	}

	/* ---- sector geometry: 512n, 512e, 4Kn ----------------------------
	 * Word 106 (signature 01b in 15:14): bit 12 = words 117..118 give the
	 * logical sector size in WORDS; bit 13 = several logical sectors per
	 * physical sector, 2^(bits 3:0) of them.  Word 209 (signature 01b):
	 * bits 13:0 = which logical sector of the first physical sector LBA 0
	 * is, so the first physical boundary is that many short of a full
	 * physical sector from LBA 0. */
	if (ATA_ID_WORD_VALID(w106)) {
		if (w106 & ATA_ID_SS_LONG_LOGICAL) {
			uint32_t words =
				ata_id_u32(id, ATA_ID_LOGICAL_SECTOR_SIZE);

			if (words > 0x8000) {
				*why = "logical sector size (words 117-118) "
				       "out of range";
				return -EINVAL;
			}
			lss = words * 2;
		}
		if (w106 & ATA_ID_SS_MULTI_LOGICAL)
			log2pp = (uint8_t)ATA_ID_SS_LOG2_PER_PHYS(w106);
	}
	if (!ata_is_pow2(lss) || lss < 512 || lss > 65536) {
		*why = "logical sector size is not a power of two in "
		       "512..65536";
		return -EINVAL;
	}
	pss = lss << log2pp;
	if (log2pp > 15 || pss > (1u << 20)) {
		/* nonsense ratio: treat the sectors as unaligned-agnostic */
		log2pp = 0;
		pss = lss;
	}
	dev->logical_sector_size = lss;
	dev->physical_sector_size = pss;
	dev->log2_per_phys = log2pp;
	dev->alignment_offset = 0;
	if (log2pp && ATA_ID_WORD_VALID(w209)) {
		uint32_t per = 1u << log2pp;
		uint32_t k = ATA_ID_ALIGN_OFFSET(w209);

		if (k < per)
			dev->alignment_offset = ((per - k) % per) * lss;
	}

	/* ---- NCQ ---------------------------------------------------------- */
	dev->ncq_depth_dev = 0;
	dev->ncq_depth = 0;
	if ((flags & ATA_DFLAG_NCQ) && !(quirks & ATA_QUIRK_NO_NCQ) && lba48) {
		uint32_t ddepth = (uint32_t)ata_id_queue_depth(id);
		uint32_t hdepth = dev->host_ncq ? dev->host_queue_depth : 0;

		dev->ncq_depth_dev = ddepth;
		if (hdepth > ATA_MAX_QUEUE)
			hdepth = ATA_MAX_QUEUE;
		if (ddepth >= 2 && hdepth >= 2) {
			dev->ncq_depth = ddepth < hdepth ? ddepth : hdepth;
			flags |= ATA_DFLAG_NCQ_ON;
		}
	} else if ((flags & ATA_DFLAG_NCQ) && (quirks & ATA_QUIRK_NO_NCQ)) {
		flags &= ~ATA_DFLAG_NCQ;
	} else if (!lba48) {
		flags &= ~ATA_DFLAG_NCQ; /* FPDMA carries 48-bit LBAs */
	}

	/* ---- cache flushing and FUA --------------------------------------- */
	if (w83 & ATA_ID_CS2_FLUSH)
		flags |= ATA_DFLAG_FLUSH;
	if (lba48 && (w83 & ATA_ID_CS2_FLUSH_EXT))
		flags |= ATA_DFLAG_FLUSH_EXT;
	/* FLUSH CACHE is mandatory for any device with a volatile write
	 * cache (ATA/ATAPI-5 on); some never set the bit. */
	if ((flags & ATA_DFLAG_WCACHE) && !(flags & ATA_DFLAG_FLUSH))
		flags |= ATA_DFLAG_FLUSH;
	if (lba48 && (w84 & ATA_ID_CSE_FUA) && !(quirks & ATA_QUIRK_NO_FUA))
		flags |= ATA_DFLAG_FUA;

	/* ---- TRIM (DATA SET MANAGEMENT) ------------------------------------ */
	dev->dsm_max_blocks = 0;
	dev->trim_max_sectors = 0;
	if (lba48 && (id[ATA_ID_DATA_SET_MGMT] & ATA_ID_DSM_TRIM) &&
	    !(quirks & ATA_QUIRK_NO_TRIM)) {
		uint16_t blocks = id[ATA_ID_MAX_DSM_BLOCKS];

		flags |= ATA_DFLAG_TRIM;
		if (w69 & ATA_ID_ADD_DRAT)
			flags |= ATA_DFLAG_TRIM_DRAT;
		if (w69 & ATA_ID_ADD_RZAT)
			flags |= ATA_DFLAG_TRIM_RZAT;
		/* Word 105: 512-byte payload blocks one DSM accepts; 0 means
		 * "not reported" -- one block is always allowed. */
		if (blocks == 0 || blocks == 0xFFFF)
			blocks = 1;
		dev->dsm_max_blocks = blocks;
		dev->trim_max_sectors = (uint64_t)blocks *
					ATA_TRIM_RANGES_PER_BLOCK *
					ATA_MAX_TRIM_RANGE;
	}

	/* ---- per-command transfer limit ------------------------------------
	 * ATA counts: LBA28 1..256, LBA48 1..65536 (0 means the maximum);
	 * 65535 keeps every count non-zero.  The host's limit is in 512-byte
	 * units; ours is in logical sectors. */
	{
		uint32_t max = lba48 ? 65535 : ATA_MAX_SECTORS_LBA28;
		uint32_t per_ls = lss / ATA_SECT_SIZE;

		if (quirks & ATA_QUIRK_MAX_SEC_128)
			max = max < 128 ? max : 128;
		if (quirks & ATA_QUIRK_MAX_SEC_1024)
			max = max < 1024 ? max : 1024;
		if (dev->host_max_sectors) {
			uint32_t host = dev->host_max_sectors / (per_ls ? per_ls : 1);

			if (host == 0)
				host = 1;
			if (host < max)
				max = host;
		}
		dev->max_sectors = max;
	}

	dev->flags = (dev->flags & ~ATA_DFLAG_CAP_MASK) | flags;
	return 0;
}

int ata_id_parse(struct ata_device *dev, const uint16_t *id)
{
	return ata_id_parse_why(dev, id, NULL);
}

/* ==========================================================================
 * Names and decoding (pure)
 * ========================================================================== */

const char *ata_cmd_name(uint8_t command)
{
	switch (command) {
	case ATA_CMD_NOP: return "NOP";
	case ATA_CMD_CFA_REQ_EXT_ERR: return "CFA REQUEST EXTENDED ERROR";
	case ATA_CMD_DSM: return "DATA SET MANAGEMENT";
	case ATA_CMD_DSM_XL: return "DATA SET MANAGEMENT XL";
	case ATA_CMD_DEV_RESET: return "DEVICE RESET";
	case ATA_CMD_REQ_SENSE_DATA: return "REQUEST SENSE DATA EXT";
	case ATA_CMD_RESTORE: return "RECALIBRATE";
	case ATA_CMD_READ_SECTORS: return "READ SECTOR(S)";
	case ATA_CMD_READ_SECTORS_EXT: return "READ SECTOR(S) EXT";
	case ATA_CMD_READ_DMA_EXT: return "READ DMA EXT";
	case ATA_CMD_READ_DMA_QUEUED_EXT: return "READ DMA QUEUED EXT";
	case ATA_CMD_READ_NATIVE_MAX_EXT: return "READ NATIVE MAX ADDRESS EXT";
	case ATA_CMD_READ_MULTIPLE_EXT: return "READ MULTIPLE EXT";
	case ATA_CMD_READ_STREAM_DMA_EXT: return "READ STREAM DMA EXT";
	case ATA_CMD_READ_STREAM_EXT: return "READ STREAM EXT";
	case ATA_CMD_READ_LOG_EXT: return "READ LOG EXT";
	case ATA_CMD_WRITE_SECTORS: return "WRITE SECTOR(S)";
	case ATA_CMD_WRITE_SECTORS_EXT: return "WRITE SECTOR(S) EXT";
	case ATA_CMD_WRITE_DMA_EXT: return "WRITE DMA EXT";
	case ATA_CMD_WRITE_DMA_QUEUED_EXT: return "WRITE DMA QUEUED EXT";
	case ATA_CMD_SET_MAX_EXT: return "SET MAX ADDRESS EXT";
	case ATA_CMD_CFA_WRITE_NE: return "CFA WRITE SECTORS WITHOUT ERASE";
	case ATA_CMD_WRITE_MULTIPLE_EXT: return "WRITE MULTIPLE EXT";
	case ATA_CMD_WRITE_STREAM_DMA_EXT: return "WRITE STREAM DMA EXT";
	case ATA_CMD_WRITE_STREAM_EXT: return "WRITE STREAM EXT";
	case ATA_CMD_WRITE_DMA_FUA_EXT: return "WRITE DMA FUA EXT";
	case ATA_CMD_WRITE_QUEUED_FUA_EXT: return "WRITE DMA QUEUED FUA EXT";
	case ATA_CMD_WRITE_LOG_EXT: return "WRITE LOG EXT";
	case ATA_CMD_VERIFY: return "READ VERIFY SECTOR(S)";
	case ATA_CMD_VERIFY_EXT: return "READ VERIFY SECTOR(S) EXT";
	case ATA_CMD_ZERO_EXT: return "ZERO EXT";
	case ATA_CMD_WRITE_UNCORR_EXT: return "WRITE UNCORRECTABLE EXT";
	case ATA_CMD_READ_LOG_DMA_EXT: return "READ LOG DMA EXT";
	case ATA_CMD_ZAC_MGMT_IN: return "ZAC MANAGEMENT IN";
	case ATA_CMD_CONFIG_STREAM: return "CONFIGURE STREAM";
	case ATA_CMD_WRITE_LOG_DMA_EXT: return "WRITE LOG DMA EXT";
	case ATA_CMD_TRUSTED_NONDATA: return "TRUSTED NON-DATA";
	case ATA_CMD_TRUSTED_RCV: return "TRUSTED RECEIVE";
	case ATA_CMD_TRUSTED_RCV_DMA: return "TRUSTED RECEIVE DMA";
	case ATA_CMD_TRUSTED_SND: return "TRUSTED SEND";
	case ATA_CMD_TRUSTED_SND_DMA: return "TRUSTED SEND DMA";
	case ATA_CMD_FPDMA_READ: return "READ FPDMA QUEUED";
	case ATA_CMD_FPDMA_WRITE: return "WRITE FPDMA QUEUED";
	case ATA_CMD_NCQ_NON_DATA: return "NCQ NON-DATA";
	case ATA_CMD_FPDMA_SEND: return "SEND FPDMA QUEUED";
	case ATA_CMD_FPDMA_RECV: return "RECEIVE FPDMA QUEUED";
	case ATA_CMD_SEEK: return "SEEK";
	case ATA_CMD_SET_DATE_TIME_EXT: return "SET DATE & TIME EXT";
	case ATA_CMD_ACCESSIBLE_MAX: return "ACCESSIBLE MAX ADDRESS CONFIGURATION";
	case ATA_CMD_EDD: return "EXECUTE DEVICE DIAGNOSTIC";
	case ATA_CMD_INIT_DEV_PARAMS: return "INITIALIZE DEVICE PARAMETERS";
	case ATA_CMD_DOWNLOAD_MICRO: return "DOWNLOAD MICROCODE";
	case ATA_CMD_DOWNLOAD_MICRO_DMA: return "DOWNLOAD MICROCODE DMA";
	case ATA_CMD_ZAC_MGMT_OUT: return "ZAC MANAGEMENT OUT";
	case ATA_CMD_PACKET: return "PACKET";
	case ATA_CMD_ID_ATAPI: return "IDENTIFY PACKET DEVICE";
	case ATA_CMD_SERVICE: return "SERVICE";
	case ATA_CMD_SMART: return "SMART";
	case ATA_CMD_CONF_OVERLAY: return "DEVICE CONFIGURATION OVERLAY";
	case ATA_CMD_SET_SECTOR_CONFIG: return "SET SECTOR CONFIGURATION EXT";
	case ATA_CMD_SANITIZE_DEVICE: return "SANITIZE DEVICE";
	case ATA_CMD_NV_CACHE: return "NV CACHE";
	case ATA_CMD_CFA_ERASE: return "CFA ERASE SECTORS";
	case ATA_CMD_READ_MULTI: return "READ MULTIPLE";
	case ATA_CMD_WRITE_MULTI: return "WRITE MULTIPLE";
	case ATA_CMD_SET_MULTI: return "SET MULTIPLE MODE";
	case ATA_CMD_READ_DMA_QUEUED: return "READ DMA QUEUED";
	case ATA_CMD_READ_DMA: return "READ DMA";
	case ATA_CMD_WRITE_DMA: return "WRITE DMA";
	case ATA_CMD_WRITE_DMA_QUEUED: return "WRITE DMA QUEUED";
	case ATA_CMD_CFA_WRITE_MULT_NE: return "CFA WRITE MULTIPLE WITHOUT ERASE";
	case ATA_CMD_WRITE_MULTI_FUA_EXT: return "WRITE MULTIPLE FUA EXT";
	case ATA_CMD_CHK_MED_CRD_TYP: return "CHECK MEDIA CARD TYPE";
	case ATA_CMD_GET_MEDIA_STATUS: return "GET MEDIA STATUS";
	case ATA_CMD_MEDIA_LOCK: return "DOOR LOCK";
	case ATA_CMD_MEDIA_UNLOCK: return "DOOR UNLOCK";
	case ATA_CMD_STANDBYNOW1: return "STANDBY IMMEDIATE";
	case ATA_CMD_IDLEIMMEDIATE: return "IDLE IMMEDIATE";
	case ATA_CMD_STANDBY: return "STANDBY";
	case ATA_CMD_IDLE: return "IDLE";
	case ATA_CMD_READ_BUFFER: return "READ BUFFER / READ PORT MULTIPLIER";
	case ATA_CMD_CHK_POWER: return "CHECK POWER MODE";
	case ATA_CMD_SLEEP: return "SLEEP";
	case ATA_CMD_FLUSH: return "FLUSH CACHE";
	case ATA_CMD_WRITE_BUFFER: return "WRITE BUFFER / WRITE PORT MULTIPLIER";
	case ATA_CMD_READ_BUFFER_DMA: return "READ BUFFER DMA";
	case ATA_CMD_FLUSH_EXT: return "FLUSH CACHE EXT";
	case ATA_CMD_WRITE_BUFFER_DMA: return "WRITE BUFFER DMA";
	case ATA_CMD_ID_ATA: return "IDENTIFY DEVICE";
	case ATA_CMD_MEDIA_EJECT: return "MEDIA EJECT";
	case ATA_CMD_SET_FEATURES: return "SET FEATURES";
	case ATA_CMD_SEC_SET_PASS: return "SECURITY SET PASSWORD";
	case ATA_CMD_SEC_UNLOCK: return "SECURITY UNLOCK";
	case ATA_CMD_SEC_ERASE_PREP: return "SECURITY ERASE PREPARE";
	case ATA_CMD_SEC_ERASE_UNIT: return "SECURITY ERASE UNIT";
	case ATA_CMD_SEC_FREEZE_LOCK: return "SECURITY FREEZE LOCK";
	case ATA_CMD_SEC_DISABLE_PASS: return "SECURITY DISABLE PASSWORD";
	case ATA_CMD_READ_NATIVE_MAX: return "READ NATIVE MAX ADDRESS";
	case ATA_CMD_SET_MAX: return "SET MAX ADDRESS";
	default: return "unknown command";
	}
}

/* Append " NAME" to buf at *pos if it fits. */
static void ata_str_add(char *buf, int cap, int *pos, const char *s)
{
	while (*s && *pos < cap - 1)
		buf[(*pos)++] = *s++;
	buf[*pos] = '\0';
}

int ata_status_str(uint8_t status, char *buf, int cap)
{
	static const struct {
		uint8_t bit;
		const char *name;
	} bits[] = {
		{ ATA_BUSY, " BSY" }, { ATA_DRDY, " DRDY" },
		{ ATA_DF, " DF" },    { ATA_DSC, " DSC" },
		{ ATA_DRQ, " DRQ" },  { ATA_CORR, " CORR" },
		{ ATA_SENSE, " SENSE" }, { ATA_ERR, " ERR" },
	};
	int pos = 0;

	if (!buf || cap <= 0)
		return 0;
	buf[0] = '\0';
	ata_str_add(buf, cap, &pos, "{");
	for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
		if (status & bits[i].bit)
			ata_str_add(buf, cap, &pos, bits[i].name);
	ata_str_add(buf, cap, &pos, " }");
	return pos;
}

int ata_error_str(uint8_t error, char *buf, int cap)
{
	static const struct {
		uint8_t bit;
		const char *name;
	} bits[] = {
		{ ATA_ICRC, " ICRC" },   { ATA_UNC, " UNC" },
		{ ATA_MC, " MC" },       { ATA_IDNF, " IDNF" },
		{ ATA_MCR, " MCR" },     { ATA_ABORTED, " ABRT" },
		{ ATA_TRK0NF, " NM" },   { ATA_AMNF, " AMNF" },
	};
	int pos = 0;

	if (!buf || cap <= 0)
		return 0;
	buf[0] = '\0';
	ata_str_add(buf, cap, &pos, "{");
	for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
		if (error & bits[i].bit)
			ata_str_add(buf, cap, &pos, bits[i].name);
	ata_str_add(buf, cap, &pos, " }");
	return pos;
}

const char *ata_xfer_mode_name(uint8_t mode)
{
	switch (mode) {
	case XFER_PIO_SLOW: return "PIO (slow)";
	case XFER_PIO_0: return "PIO0";
	case XFER_PIO_1: return "PIO1";
	case XFER_PIO_2: return "PIO2";
	case XFER_PIO_3: return "PIO3";
	case XFER_PIO_4: return "PIO4";
	case XFER_MW_DMA_0: return "MWDMA0";
	case XFER_MW_DMA_1: return "MWDMA1";
	case XFER_MW_DMA_2: return "MWDMA2";
	case XFER_UDMA_0: return "UDMA/16";
	case XFER_UDMA_1: return "UDMA/25";
	case XFER_UDMA_2: return "UDMA/33";
	case XFER_UDMA_3: return "UDMA/44";
	case XFER_UDMA_4: return "UDMA/66";
	case XFER_UDMA_5: return "UDMA/100";
	case XFER_UDMA_6: return "UDMA/133";
	case XFER_UDMA_7: return "UDMA7";
	default: return "<unknown mode>";
	}
}

/* "ATA-8", "ACS-3" ... from word 80 (highest bit set). */
static const char *ata_version_name(uint16_t major, char *buf, int cap)
{
	int hi = -1;

	if (!major) {
		ksnprintf(buf, (size_t)cap, "ATA");
		return buf;
	}
	for (int b = 14; b >= 1; b--) {
		if (major & (1u << b)) {
			hi = b;
			break;
		}
	}
	if (hi < 0)
		ksnprintf(buf, (size_t)cap, "ATA");
	else if (hi <= 7)
		ksnprintf(buf, (size_t)cap, "ATA-%d", hi);
	else if (hi == 8)
		ksnprintf(buf, (size_t)cap, "ATA8-ACS");
	else
		ksnprintf(buf, (size_t)cap, "ACS-%d", hi - 7);
	return buf;
}

static const char *ata_err_mask_name(uint32_t m)
{
	if (m & AC_ERR_NODEV_HINT)
		return "device gone?";
	if (m & AC_ERR_TIMEOUT)
		return "timeout";
	if (m & AC_ERR_HOST_BUS)
		return "host bus error";
	if (m & AC_ERR_ATA_BUS)
		return "ATA bus error";
	if (m & AC_ERR_HSM)
		return "HSM violation";
	if (m & AC_ERR_MEDIA)
		return "media error";
	if (m & AC_ERR_NCQ)
		return "NCQ error";
	if (m & AC_ERR_ABORTED)
		return "aborted by error handling";
	if (m & AC_ERR_INVALID)
		return "invalid argument";
	if (m & AC_ERR_SYSTEM)
		return "system error";
	if (m & AC_ERR_DEV)
		return "device error";
	if (m & AC_ERR_OTHER)
		return "unknown error";
	return "no error";
}

/* ==========================================================================
 * Error decoding
 * ========================================================================== */

int ata_err_mask_to_errno(uint32_t err_mask, uint8_t status, uint8_t error)
{
	(void)status;
	if (!err_mask)
		return 0;
	if (err_mask & AC_ERR_NODEV_HINT)
		return -ENODEV;
	if (err_mask & AC_ERR_TIMEOUT)
		return -ETIMEDOUT;
	if (err_mask & AC_ERR_ABORTED)
		return -ECANCELED;
	/* Invalid argument from the HOST side (nothing reached the device
	 * or the device was never at fault). */
	if ((err_mask & AC_ERR_INVALID) &&
	    !(err_mask & (AC_ERR_DEV | AC_ERR_MEDIA)))
		return -EINVAL;
	if (err_mask & AC_ERR_SYSTEM)
		return (error == 0) ? -ENOMEM : -EIO;
	return -EIO;
}

int ata_qc_decode_error(struct ata_queued_cmd *qc)
{
	uint8_t st, er;
	uint32_t mask;
	int rc;

	if (!qc)
		return -EINVAL;
	st = qc->result_tf.status;
	er = qc->result_tf.error;
	mask = qc->err_mask;

	if (st & (ATA_ERR | ATA_DF)) {
		if (st & ATA_DF)
			mask |= AC_ERR_DEV;
		if (st & ATA_ERR) {
			if (ata_prot_is_atapi(qc->protocol)) {
				/* the error register carries the sense key;
				 * the packet layer reads the sense data */
				mask |= AC_ERR_DEV;
			} else {
				if (er & ATA_ICRC)
					mask |= AC_ERR_ATA_BUS;
				if (er & (ATA_UNC | ATA_AMNF))
					mask |= AC_ERR_MEDIA;
				if (er & ATA_IDNF)
					mask |= AC_ERR_INVALID | AC_ERR_DEV;
				if ((er & ATA_ABORTED) || !er ||
				    !(mask & (AC_ERR_ATA_BUS | AC_ERR_MEDIA |
					      AC_ERR_INVALID)))
					mask |= AC_ERR_DEV;
			}
		}
	} else if (st & ATA_BUSY) {
		/* A command that ended with BSY still up broke the protocol. */
		mask |= AC_ERR_HSM;
	} else if ((st & ATA_DRQ) && !ata_prot_is_atapi(qc->protocol) &&
		   qc->protocol != ATA_PROT_PIO_IN &&
		   qc->protocol != ATA_PROT_PIO_OUT) {
		mask |= AC_ERR_HSM;
	}
	qc->err_mask = mask;
	rc = ata_err_mask_to_errno(mask, st, er);
	/* A locked security state aborts every media access. */
	if (rc == -EIO && qc->dev && qc->dev->security.locked &&
	    (er & ATA_ABORTED) && !(mask & (AC_ERR_MEDIA | AC_ERR_ATA_BUS)))
		rc = -EACCES;
	return rc;
}

static void ata_tf_line(const struct ata_taskfile *tf, char *buf, int cap)
{
	ksnprintf(buf, (size_t)cap,
		  "%02x/%02x:%02x:%02x:%02x:%02x/%02x:%02x:%02x:%02x:%02x/%02x",
		  tf->command, tf->feature, tf->nsect, tf->lbal, tf->lbam,
		  tf->lbah, tf->hob_feature, tf->hob_nsect, tf->hob_lbal,
		  tf->hob_lbam, tf->hob_lbah, tf->device);
}

static const char *ata_prot_name(enum ata_prot p)
{
	switch (p) {
	case ATA_PROT_NODATA: return "nodata";
	case ATA_PROT_PIO_IN: return "pio in";
	case ATA_PROT_PIO_OUT: return "pio out";
	case ATA_PROT_DMA_IN: return "dma in";
	case ATA_PROT_DMA_OUT: return "dma out";
	case ATA_PROT_FPDMA: return "ncq dma";
	case ATA_PROT_ATAPI_PIO: return "atapi pio";
	case ATA_PROT_ATAPI_DMA: return "atapi dma";
	case ATA_PROT_ATAPI_NODATA: return "atapi nodata";
	case ATA_PROT_SRST: return "srst";
	default: return "unknown";
	}
}

void ata_dev_report_error(struct ata_device *dev,
			  const struct ata_queued_cmd *qc)
{
	struct ata_dev_priv *p;
	char cmd[64], res[64], sts[64], err[64];
	uint32_t n;

	if (!dev || !qc)
		return;
	p = ata_dev_priv(dev);
	/* Per device: the first 20 failures in full, then one in 100 so a
	 * dying disk does not drown the console. */
	if (p) {
		n = __atomic_add_fetch(&p->reports, 1, __ATOMIC_RELAXED);
		if (n > 20 && (n % 100) != 0) {
			__atomic_add_fetch(&p->reports_suppressed, 1,
					   __ATOMIC_RELAXED);
			return;
		}
	}
	ata_tf_line(&qc->tf, cmd, sizeof(cmd));
	ata_tf_line(&qc->result_tf, res, sizeof(res));
	ata_status_str(qc->result_tf.status, sts, sizeof(sts));
	ata_error_str(qc->result_tf.error, err, sizeof(err));
	ata_dev_printk(dev, "failed command: %s\n", ata_cmd_name(qc->tf.command));
	ata_dev_printk(dev, "cmd %s tag %d %s %u bytes, lba %llu count %u\n",
		       cmd, qc->tag, ata_prot_name(qc->protocol), qc->nbytes,
		       (unsigned long long)qc->lba, qc->n_sect);
	ata_dev_printk(dev, "res %s Emask 0x%x (%s) -> %d\n", res,
		       qc->err_mask, ata_err_mask_name(qc->err_mask),
		       qc->result);
	ata_dev_printk(dev, "status: %s error: %s\n", sts, err);
	if (p && p->reports_suppressed)
		ata_dev_printk(dev, "(%u earlier failure reports suppressed)\n",
			       p->reports_suppressed);
}

/* ==========================================================================
 * Queued commands
 * ========================================================================== */

static int ata_qc_poll_hook(void *arg)
{
	struct ata_device *dev = arg;

	if (!dev || !dev->xport || !dev->xport->poll)
		return 0;
	return dev->xport->poll(dev);
}

void ata_qc_init(struct ata_queued_cmd *qc, struct ata_device *dev)
{
	BUG_ON(!qc);
	kmemset(qc, 0, sizeof(*qc));
	qc->magic = ATA_QC_MAGIC;
	qc->dev = dev;
	qc->protocol = ATA_PROT_UNKNOWN;
	qc->dma_dir = ATA_DIR_NONE;
	qc->tag = -1;
	qc->max_retries = ATA_MAX_RETRIES;
	refcount_set(&qc->refcount, 1);
	ata_completion_init(&qc->done, "ata_qc");
	ata_completion_set_poll(&qc->done, ata_qc_poll_hook, dev);
}

struct ata_queued_cmd *ata_qc_alloc(struct ata_device *dev)
{
	struct ata_queued_cmd *qc;

	if (!dev)
		return NULL;
	qc = kalloc(sizeof(*qc));
	if (!qc)
		return NULL;
	ata_qc_init(qc, dev);
	qc->flags |= ATA_QCFLAG_ORIGIN_HEAP;
	return qc;
}

void ata_qc_get(struct ata_queued_cmd *qc)
{
	BUG_ON(!qc || qc->magic != ATA_QC_MAGIC);
	refcount_inc(&qc->refcount);
}

void ata_qc_put(struct ata_queued_cmd *qc)
{
	uint32_t origin;

	BUG_ON(!qc);
	BUG_ON(qc->magic != ATA_QC_MAGIC);
	origin = qc->flags & (ATA_QCFLAG_ORIGIN_HEAP | ATA_QCFLAG_ORIGIN_POOL);
	if (!refcount_dec_and_test(&qc->refcount))
		return;
	/* Freeing a command the transport still owns would hand its slot's
	 * memory to someone else while the HBA writes into it. */
	BUG_ON(qc->flags & ATA_QCFLAG_ACTIVE);
	if (origin & ATA_QCFLAG_ORIGIN_HEAP) {
		qc->magic = 0xDEADC0DEu;
		kfree(qc);
	} else if (origin & ATA_QCFLAG_ORIGIN_POOL) {
		ata_rw_qc_pool_release(qc);
	}
	/* embedded / stack commands: the owner's memory, nothing to do */
}

void ata_qc_free(struct ata_queued_cmd *qc)
{
	if (!qc)
		return;
	ata_qc_put(qc);
}

void ata_qc_set_buf(struct ata_queued_cmd *qc, void *buf, uint32_t len,
		    enum ata_dma_dir dir)
{
	BUG_ON(!qc);
	if (!buf || !len) {
		qc->sg = NULL;
		qc->n_elem = 0;
		qc->nbytes = 0;
		qc->dma_dir = ATA_DIR_NONE;
		qc->flags &= ~ATA_QCFLAG_WRITE;
		return;
	}
	qc->sg_one.virt = buf;
	qc->sg_one.phys = 0;
	qc->sg_one.len = len;
	qc->sg_one.flags = 0;
	qc->sg = &qc->sg_one;
	qc->n_elem = 1;
	qc->nbytes = len;
	qc->dma_dir = dir;
	if (dir == ATA_DIR_TO_DEVICE)
		qc->flags |= ATA_QCFLAG_WRITE;
	else
		qc->flags &= ~ATA_QCFLAG_WRITE;
}

static void ata_qc_account_error(struct ata_device *dev,
				 struct ata_queued_cmd *qc)
{
	__atomic_add_fetch(&dev->n_errors, 1, __ATOMIC_RELAXED);
	if (qc->err_mask & AC_ERR_TIMEOUT)
		__atomic_add_fetch(&dev->n_timeouts, 1, __ATOMIC_RELAXED);
	if (qc->err_mask & AC_ERR_MEDIA)
		__atomic_add_fetch(&dev->n_media_errors, 1, __ATOMIC_RELAXED);
	if (qc->err_mask & AC_ERR_ATA_BUS)
		__atomic_add_fetch(&dev->n_bus_errors, 1, __ATOMIC_RELAXED);
	if (qc->retries)
		__atomic_add_fetch(&dev->n_retries, qc->retries,
				   __ATOMIC_RELAXED);
	dev->last_error_ns = g_ata_system_up ? hrtimer_now_ns() : 0;
	dev->last_error_tf = qc->result_tf;
	dev->last_error_cmd = qc->tf.command;
}

void ata_qc_complete(struct ata_queued_cmd *qc)
{
	void (*end_io)(struct ata_queued_cmd *qc);
	struct ata_device *dev;
	int waiter;

	BUG_ON(!qc);
	BUG_ON(qc->magic != ATA_QC_MAGIC);
	dev = qc->dev;
	BUG_ON(!dev);

	if (unlikely(qc->flags & ATA_QCFLAG_COMPLETE)) {
		WARN_RATELIMIT(1, "[ata] %s: %s completed twice", dev->name,
			       ata_cmd_name(qc->tf.command));
		return;
	}
	qc->flags &= ~ATA_QCFLAG_ACTIVE;
	qc->flags |= ATA_QCFLAG_COMPLETE;
	waiter = !!(qc->flags & ATA_QCFLAG_WAITER);

	/* What went wrong.  The transport may have decided already (timeout,
	 * device gone, cancelled by EH): keep its verdict and make sure the
	 * error mask agrees.  A command the transport completed without an
	 * error mask succeeded: the HBA signals every device error (TFES)
	 * and error handling owns those commands, so the result registers
	 * are not second-guessed -- a queued command's status comes from
	 * the last Set Device Bits FIS, which may be the one that reported
	 * another command's error.  An error mask without a verdict is
	 * decoded with the registers.  A command error handling ended for a
	 * retry is no failure of its own: its request goes back to the
	 * queue. */
	if (qc->flags & ATA_QCFLAG_EH_RETRY) {
		/* reported by error handling already */
	} else if (qc->result == 0) {
		if (unlikely(qc->err_mask))
			qc->result = ata_qc_decode_error(qc);
	} else {
		switch (qc->result) {
		case -ETIMEDOUT:
			qc->err_mask |= AC_ERR_TIMEOUT;
			qc->flags |= ATA_QCFLAG_TIMEDOUT;
			break;
		case -ENODEV:
			qc->err_mask |= AC_ERR_NODEV_HINT;
			break;
		case -ECANCELED:
			qc->err_mask |= AC_ERR_ABORTED;
			qc->flags |= ATA_QCFLAG_ABORTED;
			break;
		case -EINVAL:
			qc->err_mask |= AC_ERR_INVALID;
			break;
		default:
			if (!qc->err_mask)
				qc->err_mask = AC_ERR_OTHER;
			break;
		}
	}
	if (unlikely(qc->result) && !(qc->flags & ATA_QCFLAG_EH_RETRY)) {
		ata_qc_account_error(dev, qc);
		if (!(qc->flags & ATA_QCFLAG_QUIET) &&
		    qc->result != -ECANCELED && qc->result != -ENODEV)
			ata_dev_report_error(dev, qc);
	}

	ata_completion_complete(&qc->done);

	/* A command carrying a block request: the transport's reference goes
	 * first (the owner -- the request's context, a preparer, the drive --
	 * keeps its own, so the memory stays), then end_io hands the request
	 * to the block softirq.  That is the last touch of the command here:
	 * the softirq may run on another processor at once and give the
	 * command back to its pool.  Nothing of the block layer's request
	 * end, and no new command, comes from this context. */
	end_io = qc->end_io;
	if (end_io) {
		ata_qc_put(qc);
		end_io(qc);
		return;
	}

	/* A waiter (ata_qc_exec), woken above.  The transport's reference:
	 * the waiter still holds its own, so the memory survives this put;
	 * SETTLED then tells the waiter that nothing here touches the
	 * command any more. */
	ata_qc_put(qc);
	if (waiter)
		__atomic_or_fetch(&qc->flags, ATA_QCFLAG_SETTLED,
				  __ATOMIC_RELEASE);
}

/* The protocol's default timeout. */
static uint32_t ata_qc_default_timeout(const struct ata_queued_cmd *qc)
{
	switch (qc->tf.command) {
	case ATA_CMD_ID_ATA:
	case ATA_CMD_ID_ATAPI:
		return ATA_TMOUT_IDENTIFY_MS;
	case ATA_CMD_FLUSH:
	case ATA_CMD_FLUSH_EXT:
		return ATA_TMOUT_FLUSH_MS;
	case ATA_CMD_STANDBYNOW1:
	case ATA_CMD_STANDBY:
	case ATA_CMD_IDLEIMMEDIATE:
	case ATA_CMD_IDLE:
		return ATA_TMOUT_STANDBY_MS;
	case ATA_CMD_EDD:
		return ATA_TMOUT_DIAG_MS;
	case ATA_CMD_DSM:
		return ATA_TMOUT_TRIM_MS;
	case ATA_CMD_SMART:
		return ATA_TMOUT_SMART_MS;
	default:
		break;
	}
	switch (qc->protocol) {
	case ATA_PROT_DMA_IN:
	case ATA_PROT_DMA_OUT:
	case ATA_PROT_FPDMA:
		return ATA_TMOUT_RW_MS;
	case ATA_PROT_ATAPI_PIO:
	case ATA_PROT_ATAPI_DMA:
	case ATA_PROT_ATAPI_NODATA:
		return ATA_TMOUT_ATAPI_MS;
	default:
		return ATA_TMOUT_INTERNAL_MS;
	}
}

/* The command did not come back long after its timeout.  The transport
 * completes every command it accepted, timed-out ones included (its
 * timeout handling and error handling end them), so this only waits
 * longer -- it must never give the command up while the transport may
 * still own it: the memory (often the caller's stack) and the data
 * buffer belong to the HBA until ata_qc_complete() ran.  The transport is
 * reminded once that this command is overdue (any context; latched for
 * its error handler), and the wait says so every 30 seconds. */
static void ata_qc_exec_overdue(struct ata_queued_cmd *qc)
{
	struct ata_device *dev = qc->dev;
	const struct ata_transport *xp = dev->xport;
	uint32_t waited_s = 0;

	ata_dev_printk(dev, "%s (cmd %02x) still outstanding %u ms after its "
			    "%u ms timeout, waiting for the transport\n",
		       ata_cmd_name(qc->tf.command), qc->tf.command,
		       ATA_EXEC_GRACE_MS, qc->timeout_ms);
	WARN_RATELIMIT(1, "[ata] %s: overdue %s", dev->name,
		       ata_cmd_name(qc->tf.command));
	if (xp->schedule_eh)
		xp->schedule_eh(dev, AHCI_EH_TIMEOUT);
	while (ata_completion_wait(&qc->done, 30000) != 0) {
		waited_s += 30;
		ata_dev_printk(dev, "%s still not completed by the transport "
				    "after %u more seconds\n",
			       ata_cmd_name(qc->tf.command), waited_s);
	}
}

/* `qc' came back failed while the controller may still own its slot
 * (ATA_QCFLAG_HW_OWNED: a timed-out command on a running engine): the
 * issuer is about to reuse or free its buffer.  The eh_mutex holder --
 * the only issuer a running EH cannot recover for (ahci_qc_is_eh_own) --
 * stops the engine, which reaps the slot; the buffer is ours again then
 * and the flag goes.  Anyone else must not stop the engine under the
 * port's error handling: the flag stays, and buffers that can be kept
 * (ata_exec_internal's bounce) are kept. */
static void ata_qc_reclaim_hw(struct ata_queued_cmd *qc)
{
	struct ata_device *dev = qc->dev;
	struct ahci_link *link;
	struct ahci_port *port;
	uint64_t fl;
	int owned;

	if (dev->xport != &ahci_ata_transport || !dev->xport_priv) {
		__atomic_add_fetch(&dev->hw_owned_seq, 1, __ATOMIC_RELEASE);
		return;
	}
	link = ahci_dev_link(dev);
	port = link ? link->port : NULL;
	if (!port) {
		__atomic_add_fetch(&dev->hw_owned_seq, 1, __ATOMIC_RELEASE);
		return;
	}
	if (!ata_dev_eh_held(dev)) {
		WARN_RATELIMIT(1, "[ata] %s: %s failed while the controller "
				  "still owns its slot",
			       dev->name, ata_cmd_name(qc->tf.command));
		__atomic_add_fetch(&dev->hw_owned_seq, 1, __ATOMIC_RELEASE);
		return;
	}
	(void)ahci_cmd_kick_engine(port);
	spin_lock_irqsave(&port->lock, &fl);
	owned = qc->tag >= 0 && qc->tag < AHCI_MAX_SLOTS &&
		(port->zombie_slots & (1u << (uint32_t)qc->tag));
	spin_unlock_irqrestore(&port->lock, fl);
	if (owned) {
		WARN_RATELIMIT(1, "[ata] %s: the controller does not let go "
				  "of the slot of %s",
			       dev->name, ata_cmd_name(qc->tf.command));
		__atomic_add_fetch(&dev->hw_owned_seq, 1, __ATOMIC_RELEASE);
		return;
	}
	qc->flags &= ~ATA_QCFLAG_HW_OWNED;
}

/* Run a command of a caller outside error handling on a disk that is up:
 * through the disk's request queue, as a pass-through request.  Ordered
 * with the filesystem's I/O there -- a non-queued command waits at the
 * head of the queue while the queued ones drain, which the transport's
 * -EBUSY alone could not guarantee -- and its timeout is the queue's.
 * Returns 1 with *rc set when it went that way, 0 when the caller must
 * use ata_qc_exec() (boot, error handling, no live disk). */
static int ata_exec_queued(struct ata_device *dev, struct ata_queued_cmd *qc,
			   int *rc)
{
	struct gendisk *d = dev->disk;
	struct blk_request *req;
	int ret;

	if (!ata_can_sleep() || (qc->flags & ATA_QCFLAG_EH) ||
	    ata_dev_eh_held(dev))
		return 0;
	if (!d || d->magic != GENDISK_MAGIC || d->ops != &ata_disk_blk_ops ||
	    !d->queue || !(d->flags & GENHD_FL_UP) ||
	    (d->flags & GENHD_FL_DEAD))
		return 0;
	if (!qc->timeout_ms)
		qc->timeout_ms = ata_qc_default_timeout(qc);
	/* Data to the device is a "write" for the block layer (refused on a
	 * read-only disk); reads and non-data commands are not. */
	req = blk_request_alloc(&d->bdev,
				qc->dma_dir == ATA_DIR_TO_DEVICE ?
					BLK_OP_DRV_OUT :
					BLK_OP_DRV_IN,
				0, 0);
	if (!req) {
		*rc = -ENOMEM;
		return 1;
	}
	req->drv_cmd = qc;
	req->flags |= BLK_REQ_HEAD | BLK_REQ_NO_ACCOUNT | BLK_REQ_QUIET;
	req->timeout_ms = qc->timeout_ms;
	if (qc->nbytes && qc->sg == &qc->sg_one && qc->sg_one.virt) {
		ret = blk_request_add_buf(req, qc->sg_one.virt, qc->nbytes);
		if (ret) {
			blk_request_free(req);
			*rc = ret;
			return 1;
		}
	}
	ret = blk_execute(req);
	blk_request_free(req);
	*rc = (qc->flags & ATA_QCFLAG_COMPLETE) ? qc->result : ret;
	return 1;
}

int ata_qc_exec(struct ata_queued_cmd *qc)
{
	struct ata_device *dev;
	const struct ata_transport *xp;
	int sleep_ok = ata_can_sleep();
	uint64_t waited_us = 0, limit_us;
	uint32_t tmo;
	int rc;

	BUG_ON(!qc);
	BUG_ON(qc->magic != ATA_QC_MAGIC);
	dev = qc->dev;
	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	BUG_ON(qc->flags & ATA_QCFLAG_ACTIVE); /* issued twice */
	if (sleep_ok) {
		might_sleep();
		ata_note_system_up();
	}
	xp = dev->xport;
	if (unlikely(!xp || !xp->qc_issue)) {
		qc->result = -ENODEV;
		return -ENODEV;
	}
	if (unlikely(dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED))) {
		qc->result = -ENODEV;
		qc->err_mask = AC_ERR_NODEV_HINT;
		return -ENODEV;
	}
	if (ata_exec_queued(dev, qc, &rc))
		return rc;

	tmo = qc->timeout_ms ? qc->timeout_ms : ata_qc_default_timeout(qc);
	qc->timeout_ms = tmo;
	if (!(qc->flags & ATA_QCFLAG_EH) && ata_dev_eh_held(dev))
		qc->flags |= ATA_QCFLAG_EH;
	if (!sleep_ok)
		qc->flags |= ATA_QCFLAG_POLLED;
	qc->flags &= ~(ATA_QCFLAG_COMPLETE | ATA_QCFLAG_FAILED |
		       ATA_QCFLAG_TIMEDOUT | ATA_QCFLAG_ABORTED |
		       ATA_QCFLAG_SENSE_VALID | ATA_QCFLAG_SETTLED);
	qc->flags |= ATA_QCFLAG_WAITER;
	qc->result = 0;
	qc->err_mask = 0;
	qc->resid = 0;
	qc->tag = -1;
	kmemset(&qc->result_tf, 0, sizeof(qc->result_tf));
	if (ata_completion_done(&qc->done))
		ata_completion_reinit(&qc->done);
	if (!qc->done.poll)
		ata_completion_set_poll(&qc->done, ata_qc_poll_hook, dev);

	/* No slot right now, or the port is in error handling: try again
	 * until the command's own timeout has passed. */
	limit_us = (uint64_t)tmo * 1000ULL;
	for (;;) {
		rc = xp->qc_issue(qc);
		if (likely(rc == 0))
			break;
		if (rc != -EBUSY && rc != -EAGAIN) {
			qc->flags &= ~ATA_QCFLAG_WAITER;
			qc->result = rc;
			if (rc == -ENODEV)
				qc->err_mask |= AC_ERR_NODEV_HINT;
			else if (rc == -EINVAL)
				qc->err_mask |= AC_ERR_INVALID;
			else
				qc->err_mask |= AC_ERR_OTHER;
			return rc;
		}
		if (waited_us >= limit_us) {
			qc->flags &= ~ATA_QCFLAG_WAITER;
			qc->result = -EBUSY;
			ata_dev_printk(dev, "%s: transport busy for %u ms, "
					    "giving up\n",
				       ata_cmd_name(qc->tf.command), tmo);
			return -EBUSY;
		}
		if (sleep_ok) {
			ata_delay_ms(1);
		} else {
			if (xp->poll)
				xp->poll(dev);
			lapic_delay_us(1000);
		}
		waited_us += 1000;
	}

	/* The command's timeout is this wait's: when it runs out, the
	 * transport hands the command to its error handling (or, for the
	 * holder of the port's error-handling mutex, fails it at once), which
	 * completes it with -ETIMEDOUT.  The grace only decides when to start
	 * saying that it is overdue. */
	rc = ata_completion_wait(&qc->done, tmo);
	if (unlikely(rc)) {
		if (xp->qc_timed_out)
			(void)xp->qc_timed_out(dev, qc, NULL);
		rc = ata_completion_wait(&qc->done, ATA_EXEC_GRACE_MS);
		if (unlikely(rc))
			ata_qc_exec_overdue(qc);
	}

	/* Wait for ata_qc_complete() to let go of the command: it is a
	 * handful of instructions away once `done' is set, but the memory
	 * is not ours before it has. */
	for (uint64_t i = 0;; i++) {
		if (__atomic_load_n(&qc->flags, __ATOMIC_ACQUIRE) &
		    ATA_QCFLAG_SETTLED)
			break;
		__asm__ volatile("pause" ::: "memory");
		/* The completer may be a preemptible thread (error handling,
		 * a poller): give it the processor rather than spin against
		 * it. */
		if ((i & 1023) == 1023) {
			if (ata_can_sleep())
				sched_yield_in_kernel();
			else
				lapic_delay_us(10);
		}
		if (i == 10000000ULL)
			WARN(1, "[ata] %s: completion of %s does not settle",
			     dev->name, ata_cmd_name(qc->tf.command));
	}
	VM_WARN_ON(qc->flags & ATA_QCFLAG_ACTIVE);
	qc->flags &= ~ATA_QCFLAG_WAITER;
	if (unlikely(qc->flags & ATA_QCFLAG_HW_OWNED))
		ata_qc_reclaim_hw(qc);
	return qc->result;
}

int ata_exec_internal(struct ata_device *dev, struct ata_taskfile *tf,
		      const uint8_t *cdb, enum ata_prot prot, void *buf,
		      uint32_t len, enum ata_dma_dir dir, uint32_t timeout_ms,
		      uint32_t qcflags)
{
	struct ata_queued_cmd *qc;
	void *bounce = NULL;
	int rc;

	if (!dev || !tf)
		return -EINVAL;
	if (dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	if (len && !buf)
		return -EINVAL;
	if (!len)
		dir = ATA_DIR_NONE;
	if (len && dir == ATA_DIR_NONE)
		return -EINVAL;

	qc = ata_qc_alloc(dev);
	if (!qc)
		return -ENOMEM;
	/* The data goes through a buffer of our own: should the command be
	 * lost by the transport, the caller's memory (often its stack) is
	 * never the one an HBA might still write into. */
	if (len) {
		bounce = kalloc(len);
		if (!bounce) {
			ata_qc_free(qc);
			return -ENOMEM;
		}
		if (dir == ATA_DIR_TO_DEVICE)
			kmemcpy(bounce, buf, len);
		else
			kmemset(bounce, 0, len);
	}

	qc->tf = *tf;
	qc->tf.status = 0;
	qc->tf.error = 0;
	qc->protocol = prot;
	qc->flags |= ATA_QCFLAG_INTERNAL | ATA_QCFLAG_RESULT_TF |
		     (qcflags & ~ATA_QCFLAG_CORE_MASK);
	if (cdb) {
		uint8_t n = dev->cdb_len ? dev->cdb_len : 12;

		if (n > ATAPI_CDB_LEN_MAX)
			n = ATAPI_CDB_LEN_MAX;
		kmemcpy(qc->cdb, cdb, n);
		qc->cdb_len = n;
	}
	ata_qc_set_buf(qc, bounce, len, dir);
	qc->timeout_ms = timeout_ms;
	qc->lba = ata_tf_read_lba(tf);
	qc->n_sect = len / ATA_SECT_SIZE;
	if (qcflags & ATA_QCFLAG_RETRY)
		qc->max_retries = ATA_MAX_RETRIES;
	else
		qc->max_retries = 0;

	/* -ECANCELED means "requeue": recovery cancelled the command (or a
	 * re-issue could not run); it goes out again. */
	for (int tries = 0;; tries++) {
		rc = ata_qc_exec(qc);
		if (rc != -ECANCELED || tries >= ATA_EXEC_REQUEUE_MAX)
			break;
		ata_delay_ms(10);
	}
	if (rc == -ECANCELED)
		rc = -EIO;

	if (unlikely(!(qc->flags & ATA_QCFLAG_COMPLETE))) {
		/* Not accepted at all leaves it incomplete too (rc says
		 * why); an accepted command always completes before
		 * ata_qc_exec returns.  Never let go of memory the transport
		 * might still own: only an accepted-but-incomplete command
		 * is kept (that cannot happen by the transport contract). */
		if (rc == -EBUSY || rc == -ENODEV || rc == -EINVAL ||
		    !(qc->flags & ATA_QCFLAG_ACTIVE)) {
			if (bounce)
				kfree(bounce);
			ata_qc_free(qc);
			return rc ? rc : -EIO;
		}
		ata_dev_printk(dev, "leaking a lost %s and its %u-byte "
				    "buffer\n",
			       ata_cmd_name(tf->command), len);
		return rc ? rc : -ETIMEDOUT;
	}

	/* Hand the result registers back. */
	tf->status = qc->result_tf.status;
	tf->error = qc->result_tf.error;
	tf->nsect = qc->result_tf.nsect;
	tf->hob_nsect = qc->result_tf.hob_nsect;
	tf->lbal = qc->result_tf.lbal;
	tf->lbam = qc->result_tf.lbam;
	tf->lbah = qc->result_tf.lbah;
	tf->hob_lbal = qc->result_tf.hob_lbal;
	tf->hob_lbam = qc->result_tf.hob_lbam;
	tf->hob_lbah = qc->result_tf.hob_lbah;
	tf->device = qc->result_tf.device;

	if (rc == 0 && dir == ATA_DIR_FROM_DEVICE && len) {
		uint32_t got = len - (qc->resid < len ? qc->resid : len);

		kmemcpy(buf, bounce, got);
		if (got < len)
			kmemset((uint8_t *)buf + got, 0, len - got);
	}
	if (bounce && (qc->flags & ATA_QCFLAG_HW_OWNED)) {
		/* Failed while the controller still owned its slot (a timed
		 * out command on a running engine): a slow device may yet
		 * write into the bounce buffer.  Never hand that memory back
		 * -- a few hundred bytes lost beat a corrupted heap. */
		WARN_RATELIMIT(1, "[ata] %s: keeping the %u-byte buffer of %s: "
				  "the controller may still write into it",
			       dev->name, len, ata_cmd_name(tf->command));
		bounce = NULL;
	}
	if (bounce)
		kfree(bounce);
	ata_qc_free(qc);
	return rc;
}

int ata_exec_nodata(struct ata_device *dev, struct ata_taskfile *tf,
		    uint32_t timeout_ms, uint32_t qcflags)
{
	return ata_exec_internal(dev, tf, NULL, ATA_PROT_NODATA, NULL, 0,
				 ATA_DIR_NONE,
				 timeout_ms ? timeout_ms : ATA_TMOUT_INTERNAL_MS,
				 qcflags);
}

/* ==========================================================================
 * The registration worker
 *
 * At run time a disk is added and removed by this thread, never by the
 * prober: disk_add() reads the partition table through the very port
 * whose error handling the prober (which holds the port's eh_mutex, often
 * on the EH thread itself) would be blocking, so a read error during the
 * scan could never be recovered.  At boot there is no scheduler and no
 * error handling thread yet; registration is synchronous then, so the
 * root disk exists when the storage bootstrap looks for it.
 * ========================================================================== */

static spinlock_t g_ata_work_lock = SPINLOCK_INIT("ata_work");
static struct ata_work_item *g_ata_work_head, *g_ata_work_tail;
static struct wait_queue_head g_ata_work_wq;
static volatile int g_ata_work_wq_ready;
static task_t *g_ata_worker;
static volatile int g_ata_worker_state; /* 0 none, 1 starting, 2 running */
static uint8_t g_ata_worker_stack[16384] __attribute__((aligned(16)));

static void ata_dev_free_now(struct ata_device *dev);

/* Drop a disk reference the worker holds that may be the last one.  The
 * last put frees the disk and with it the driver's per-disk state (the
 * optical drive object, dev->disk, the request queue), which the port's
 * error handling reads under the eh_mutex (media checks, stopping and
 * starting the disk's queue): taken under the same mutex, so it never
 * happens in the middle of an EH run.  Allowed: the eh_mutex comes before
 * the disk registry mutex the release takes, and error handling never
 * waits for this worker. */
static void ata_work_disk_put_locked_dev(struct ata_device *dev,
					 struct gendisk *disk)
{
	ata_dev_lock(dev);
	disk_put(disk);
	ata_dev_unlock(dev);
}

static void ata_work_run_item(struct ata_work_item *w)
{
	struct ata_device *dev = w->dev;
	struct ata_dev_priv *p = ata_dev_priv(dev);
	uint64_t fl;
	int rc;

	switch (w->kind) {
	case ATA_WORK_DISK_ADD: {
		int drop = 0;

		spin_lock_irqsave(&g_ata_work_lock, &fl);
		if (!p || p->disk_state != ATA_DISK_ADD_PENDING ||
		    (dev->flags & ATA_DFLAG_DETACHED)) {
			drop = 1;
			if (p && p->disk_state == ATA_DISK_ADD_PENDING)
				p->disk_state = ATA_DISK_NONE;
		} else {
			p->disk_state = ATA_DISK_ADDED;
		}
		if (p)
			p->pending_disk = NULL;
		spin_unlock_irqrestore(&g_ata_work_lock, fl);
		if (drop) {
			ata_dev_printk(dev, "device went away before its disk "
					    "was added\n");
			/* never added: frees it */
			ata_work_disk_put_locked_dev(dev, w->disk);
			break;
		}
		rc = disk_add(w->disk);
		if (rc) {
			ata_dev_printk(dev, "disk_add failed (%d)\n", rc);
			spin_lock_irqsave(&g_ata_work_lock, &fl);
			p->disk_state = ATA_DISK_NONE;
			spin_unlock_irqrestore(&g_ata_work_lock, fl);
			/* dev->flags is written by error handling under the
			 * eh_mutex (DETACHED, DISABLED): never a concurrent
			 * read-modify-write, and the final put below frees
			 * what error handling looks at. */
			ata_dev_lock(dev);
			dev->flags &= ~ATA_DFLAG_REGISTERED;
			disk_put(w->disk);
			ata_dev_unlock(dev);
		} else {
			ata_dev_printk(dev, "registered as %s\n",
				       w->disk->disk_name);
		}
		break;
	}
	case ATA_WORK_DISK_DEL:
		disk_del(w->disk);
		spin_lock_irqsave(&g_ata_work_lock, &fl);
		if (p && p->disk_state == ATA_DISK_DEL_PENDING)
			p->disk_state = ATA_DISK_NONE;
		spin_unlock_irqrestore(&g_ata_work_lock, fl);
		/* the item's reference, normally the last */
		ata_work_disk_put_locked_dev(dev, w->disk);
		break;
	case ATA_WORK_SET_CAPACITY:
		if (p && p->disk_state == ATA_DISK_ADDED &&
		    !(dev->flags & ATA_DFLAG_DETACHED)) {
			rc = disk_set_capacity(w->disk, w->arg);
			if (rc)
				ata_dev_printk(dev, "capacity update to %llu "
						    "failed (%d)\n",
					       (unsigned long long)w->arg, rc);
		}
		ata_work_disk_put_locked_dev(dev, w->disk);
		break;
	case ATA_WORK_MEDIA_CHANGED:
		/* The state at the time this runs, not when it was queued: a
		 * disc inserted and taken out again before the worker got
		 * here must not be announced as present.  Only for a live
		 * disk (not one whose add failed or is being deleted).  The
		 * I/O runs without the eh_mutex; only the put takes it. */
		if (p && dev->disk == w->disk &&
		    p->disk_state == ATA_DISK_ADDED &&
		    !(dev->flags & ATA_DFLAG_DETACHED))
			disk_media_changed(w->disk, dev->media_present ? 1 : 0);
		ata_work_disk_put_locked_dev(dev, w->disk);
		break;
	case ATA_WORK_FREE_DEV:
		ata_dev_free_now(dev);
		return; /* the item lived inside the device */
	}
	if (w->kind != ATA_WORK_FREE_DEV)
		ata_dev_put(dev);
	if (!w->embedded)
		kfree(w);
}

static void ata_worker_main(void *arg)
{
	task_t *cur = sched_current();

	(void)arg;
	g_ata_worker_state = 2;
	for (;;) {
		struct ata_work_item *w;
		struct wait_queue_entry we;
		uint64_t fl;

		spin_lock_irqsave(&g_ata_work_lock, &fl);
		w = g_ata_work_head;
		if (w) {
			g_ata_work_head = w->next;
			if (!g_ata_work_head)
				g_ata_work_tail = NULL;
			w->next = NULL;
			spin_unlock_irqrestore(&g_ata_work_lock, fl);
			ata_work_run_item(w);
			continue;
		}
		/* Nothing to do: go to sleep while still holding the list
		 * lock.  A queuer links its item under the same lock and
		 * wakes the queue afterwards, so it either sees us blocked
		 * on the queue or we saw its item above. */
		wq_entry_init(&we, cur);
		wq_add(&g_ata_work_wq, &we);
		cur->wait_channel = &g_ata_work_wq;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&g_ata_work_lock, fl);
		sched_schedule();
		cur->wait_channel = NULL;
		wq_remove(&g_ata_work_wq, &we);
	}
}

/* Start the worker on first use (process context only). */
static int ata_worker_ensure(void)
{
	if (g_ata_worker_state)
		return 0;
	if (!sched_current())
		return -EAGAIN;
	if (__sync_val_compare_and_swap(&g_ata_worker_state, 0, 1) != 0)
		return 0;
	if (!g_ata_work_wq_ready) {
		wq_head_init(&g_ata_work_wq, "ata_work");
		g_ata_work_wq_ready = 1;
	}
	g_ata_worker = sched_add_task(ata_worker_main, NULL, g_ata_worker_stack,
				      sizeof(g_ata_worker_stack));
	if (!g_ata_worker) {
		g_ata_worker_state = 0;
		kprintf("[ata] could not start the registration worker\n");
		return -ENOMEM;
	}
	return 0;
}

static void ata_work_queue(struct ata_work_item *w)
{
	uint64_t fl;

	w->next = NULL;
	spin_lock_irqsave(&g_ata_work_lock, &fl);
	if (g_ata_work_tail)
		g_ata_work_tail->next = w;
	else
		g_ata_work_head = w;
	g_ata_work_tail = w;
	spin_unlock_irqrestore(&g_ata_work_lock, fl);
	if (g_ata_work_wq_ready)
		wq_wake_all(&g_ata_work_wq);
}

/* Is the asynchronous path available (scheduler + worker)? */
static int ata_work_async_ok(void)
{
	if (!sched_current())
		return 0;
	return ata_worker_ensure() == 0;
}

static struct ata_work_item *ata_work_new(enum ata_work_kind kind,
					  struct ata_device *dev,
					  struct gendisk *disk, uint64_t arg)
{
	struct ata_work_item *w = kalloc(sizeof(*w));

	if (!w)
		return NULL;
	kmemset(w, 0, sizeof(*w));
	w->kind = kind;
	w->dev = dev;
	w->disk = disk;
	w->arg = arg;
	ata_dev_get(dev);
	return w;
}

int ata_work_disk_add(struct ata_device *dev, struct gendisk *disk)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct ata_work_item *w;
	uint64_t fl;
	int rc;

	if (!p || !disk)
		return -EINVAL;
	if (ata_work_async_ok()) {
		w = ata_work_new(ATA_WORK_DISK_ADD, dev, disk, 0);
		if (w) {
			spin_lock_irqsave(&g_ata_work_lock, &fl);
			p->disk_state = ATA_DISK_ADD_PENDING;
			p->pending_disk = disk;
			spin_unlock_irqrestore(&g_ata_work_lock, fl);
			ata_work_queue(w);
			ata_dev_printk(dev, "disk registration queued\n");
			return 0;
		}
		ata_dev_printk(dev, "no memory for deferred registration, "
				    "registering synchronously\n");
	}
	/* Boot (or no worker): register right here. */
	spin_lock_irqsave(&g_ata_work_lock, &fl);
	p->disk_state = ATA_DISK_ADDED;
	spin_unlock_irqrestore(&g_ata_work_lock, fl);
	rc = disk_add(disk);
	if (rc) {
		spin_lock_irqsave(&g_ata_work_lock, &fl);
		p->disk_state = ATA_DISK_NONE;
		spin_unlock_irqrestore(&g_ata_work_lock, fl);
		return rc;
	}
	ata_dev_printk(dev, "registered as %s\n", disk->disk_name);
	return 0;
}

void ata_work_disk_del(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct gendisk *disk;
	struct ata_work_item *w = NULL;
	uint64_t fl;
	enum ata_disk_state st;

	if (!p)
		return;
	disk = dev->disk;
	spin_lock_irqsave(&g_ata_work_lock, &fl);
	st = p->disk_state;
	if (st == ATA_DISK_ADD_PENDING) {
		/* the queued ADD sees this and drops the disk */
		p->disk_state = ATA_DISK_NONE;
	} else if (st == ATA_DISK_ADDED) {
		p->disk_state = ATA_DISK_DEL_PENDING;
	}
	spin_unlock_irqrestore(&g_ata_work_lock, fl);
	if (st != ATA_DISK_ADDED || !disk)
		return;

	if (ata_work_async_ok())
		w = ata_work_new(ATA_WORK_DISK_DEL, dev, disk, 0);
	if (w) {
		disk_get(disk);
		ata_work_queue(w);
		return;
	}
	disk_del(disk);
	spin_lock_irqsave(&g_ata_work_lock, &fl);
	if (p->disk_state == ATA_DISK_DEL_PENDING)
		p->disk_state = ATA_DISK_NONE;
	spin_unlock_irqrestore(&g_ata_work_lock, fl);
}

void ata_work_set_capacity(struct ata_device *dev, uint64_t sectors)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct gendisk *disk = dev->disk;
	struct ata_work_item *w = NULL;

	if (!p || !disk || p->disk_state != ATA_DISK_ADDED)
		return;
	if (ata_work_async_ok())
		w = ata_work_new(ATA_WORK_SET_CAPACITY, dev, disk, sectors);
	if (w) {
		disk_get(disk);
		ata_work_queue(w);
		return;
	}
	if (disk_set_capacity(disk, sectors))
		ata_dev_printk(dev, "capacity update failed\n");
}

int ata_work_media_changed(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct gendisk *disk;
	struct ata_work_item *w;

	if (!p)
		return -EINVAL;
	/* Only a live disk gets the news (optical drives register through
	 * ata_work_disk_add() as well, so disk_state tells).  One whose add
	 * is still queued may be added with what it knew before the change:
	 * the caller keeps the change pending (-EAGAIN) and delivers it at
	 * its next look. */
	disk = dev->disk;
	if (!disk || (dev->flags & ATA_DFLAG_DETACHED))
		return 0;
	if (p->disk_state == ATA_DISK_ADD_PENDING)
		return -EAGAIN;
	if (p->disk_state != ATA_DISK_ADDED)
		return 0;
	if (!ata_work_async_ok())
		return -EAGAIN;
	w = ata_work_new(ATA_WORK_MEDIA_CHANGED, dev, disk, 0);
	if (!w)
		return -ENOMEM;
	disk_get(disk);
	ata_work_queue(w);
	return 0;
}

/* ==========================================================================
 * Device objects
 * ========================================================================== */

struct ata_device *ata_dev_alloc(const struct ata_transport *xport,
				 void *xport_priv, int port_no, int devno)
{
	struct ata_dev_full *full;
	struct ata_device *dev;
	struct ata_dev_priv *p;

	if (!xport) {
		kprintf("[ata] ata_dev_alloc without a transport\n");
		return NULL;
	}
	full = kalloc(sizeof(*full));
	if (!full)
		return NULL;
	kmemset(full, 0, sizeof(*full));
	dev = &full->dev;
	p = &full->priv;

	dev->magic = ATA_DEVICE_MAGIC;
	ksnprintf(dev->name, sizeof(dev->name), "ata%d.%02d", port_no, devno);
	dev->port_no = port_no;
	dev->devno = devno;
	dev->class = ATA_DEV_UNKNOWN;
	dev->flags = 0;
	refcount_set(&dev->refcount, 1);
	dev->xport = xport;
	dev->xport_priv = xport_priv;
	/* Conservative defaults until the transport fills its limits. */
	dev->host_max_segments = 128;
	dev->host_max_sectors = ATA_MAX_SECTORS_LBA28;
	dev->host_queue_depth = 0;
	dev->host_dma_alignment = 1;
	dev->host_ncq = 0;
	dev->host_fua = 1;
	dev->logical_sector_size = ATA_SECT_SIZE;
	dev->physical_sector_size = ATA_SECT_SIZE;
	dev->max_sectors = ATA_MAX_SECTORS_LBA28;
	dev->power_state = ATA_PWR_UNKNOWN;
	dev->cdb_len = 12;

	p->magic = ATA_DEV_PRIV_MAGIC;
	p->eh_owner = (uint64_t)-1;
	p->eh_depth = 0;
	p->policy.write_cache = -1;
	p->policy.read_lookahead = -1;
	p->policy.apm = -1;
	p->policy.aam = -1;
	p->policy.puis = -1;
	p->policy.ncq_prio = 0;
	p->policy.sense_reporting = -1;
	p->policy.freeze_security = -1;
	p->policy.standby_timer = -1;
	p->disk_state = ATA_DISK_NONE;
	p->free_item.embedded = 1;
	return dev;
}

void ata_dev_get(struct ata_device *dev)
{
	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	refcount_inc(&dev->refcount);
}

static void ata_dev_free_now(struct ata_device *dev)
{
	struct ata_dev_full *full;

	WARN_ON(dev->disk != NULL);
	/* The sysfs device directory reads this object: it goes first. */
	if (dev->xport == &ahci_ata_transport)
		ahci_sysfs_remove_device(dev);
	ata_rw_dev_fini(dev);
	full = (struct ata_dev_full *)((char *)dev -
				       __builtin_offsetof(struct ata_dev_full,
							  dev));
	dev->magic = 0;
	full->priv.magic = 0;
	kfree(full);
}

static void ata_dev_cpl_free(struct blk_release *r)
{
	struct ata_dev_full *full = (struct ata_dev_full *)
		((char *)r - __builtin_offsetof(struct ata_dev_full,
						priv.cpl_release));

	ata_dev_free_now(&full->dev);
}

void ata_dev_put(struct ata_device *dev)
{
	struct ata_dev_priv *p;

	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	if (!refcount_dec_and_test(&dev->refcount))
		return;
	p = ata_dev_priv(dev);
	/* Inside a completion section (a pool command's final put in
	 * ata_qc_complete): freed by the section's end, in this task, once
	 * the completion is through with the command and its pool. */
	if (p) {
		p->cpl_release.fn = ata_dev_cpl_free;
		if (blk_cpl_park(&p->cpl_release))
			return;
	}
	/* The last reference can go from an interrupt (a pool command's
	 * final put): the big frees are left to the worker then. */
	if (!ata_can_sleep() && p && g_ata_worker_state == 2) {
		p->free_queued = 1;
		p->free_item.kind = ATA_WORK_FREE_DEV;
		p->free_item.dev = dev;
		p->free_item.embedded = 1;
		ata_work_queue(&p->free_item);
		return;
	}
	ata_dev_free_now(dev);
}

/* ==========================================================================
 * IDENTIFY and classification
 * ========================================================================== */

/* One IDENTIFY (PACKET) DEVICE.  The page lands in `id' in CPU order; on
 * failure *res holds the result registers (classification looks at the
 * signature an aborted IDENTIFY leaves behind). */
static int ata_read_id_once(struct ata_device *dev, enum ata_class class,
			    uint16_t *id, struct ata_taskfile *res)
{
	struct ata_taskfile tf;
	uint8_t *raw;
	int rc;

	raw = kalloc(ATA_ID_BYTES);
	if (!raw)
		return -ENOMEM;
	kmemset(&tf, 0, sizeof(tf));
	tf.command = (class == ATA_DEV_ATAPI) ? ATA_CMD_ID_ATAPI :
						ATA_CMD_ID_ATA;
	tf.flags = ATA_TFLAG_DEVICE;
	tf.device = 0;
	rc = ata_exec_internal(dev, &tf, NULL, ATA_PROT_PIO_IN, raw,
			       ATA_ID_BYTES, ATA_DIR_FROM_DEVICE,
			       ATA_TMOUT_IDENTIFY_MS, ATA_QCFLAG_QUIET);
	if (res)
		*res = tf;
	if (rc == 0) {
		/* The page is little-endian words. */
		for (int i = 0; i < ATA_ID_WORDS; i++)
			id[i] = (uint16_t)(raw[2 * i] |
					   ((uint16_t)raw[2 * i + 1] << 8));
	}
	kfree(raw);
	return rc;
}

/* IDENTIFY with retries of transient failures (timeouts, bus errors, an
 * empty or corrupt page).  A clean abort is returned at once -- it is the
 * device's answer -- with its registers in *res. */
static int ata_read_id_retry(struct ata_device *dev, enum ata_class class,
			     uint16_t *id, struct ata_taskfile *res)
{
	int rc = -EIO;

	for (int attempt = 0; attempt < 3; attempt++) {
		rc = ata_read_id_once(dev, class, id, res);
		if (rc == 0) {
			int zero = 1, ones = 1;

			for (int i = 0; i < ATA_ID_WORDS; i++) {
				if (id[i])
					zero = 0;
				if (id[i] != 0xFFFF)
					ones = 0;
			}
			if (zero || ones) {
				ata_dev_printk(dev, "IDENTIFY returned an "
						    "empty page (attempt %d)\n",
					       attempt + 1);
				rc = -EIO;
				continue;
			}
			if (!ata_id_checksum_ok(id)) {
				ata_dev_printk(dev, "IDENTIFY checksum "
						    "mismatch (attempt %d)\n",
					       attempt + 1);
				rc = -EIO;
				continue;
			}
			return 0;
		}
		/* A clean abort is an answer, not a transient failure. */
		if (rc == -EIO && ata_tf_is_abort_only(res))
			return rc;
		if (rc == -ENODEV || rc == -ENOMEM)
			return rc;
		ata_dev_printk(dev, "%s failed (%d), attempt %d\n",
			       class == ATA_DEV_ATAPI ? "IDENTIFY PACKET DEVICE" :
							"IDENTIFY DEVICE",
			       rc, attempt + 1);
	}
	return rc;
}

int ata_dev_read_id(struct ata_device *dev, enum ata_class class,
		    uint16_t *id)
{
	struct ata_taskfile res;

	if (!dev || !id)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	kmemset(&res, 0, sizeof(res));
	return ata_read_id_retry(dev, class, id, &res);
}

/* What the registers an aborted IDENTIFY leaves behind say the device is
 * (the signature in LBA mid/high). */
static enum ata_class ata_class_from_abort(const struct ata_taskfile *tf)
{
	uint32_t sig = 0x00000101u | ((uint32_t)tf->lbam << 16) |
		       ((uint32_t)tf->lbah << 24);

	if (tf->lbam == 0 && tf->lbah == 0)
		return ATA_DEV_UNKNOWN;
	return ata_class_from_sig(sig);
}

/* Read the IDENTIFY page, settling the class on the way:
 *   - an unrecognised signature tries IDENTIFY DEVICE first, then PACKET;
 *   - an IDENTIFY DEVICE aborted with the packet signature in LBA mid/high
 *     (14h/EBh) is an ATAPI device, a PMP or SEMB signature there is not
 *     ours at all;
 *   - a page whose word 0 contradicts the command that fetched it is
 *     re-fetched with the other command. */
static int ata_dev_identify_classify(struct ata_device *dev,
				     enum ata_class *class, uint16_t *id)
{
	enum ata_class try = *class;
	int tried_ata = 0, tried_atapi = 0;
	struct ata_taskfile res;
	int rc = -EIO;

	if (try == ATA_DEV_UNKNOWN)
		try = ATA_DEV_ATA;
	for (int round = 0; round < 4; round++) {
		enum ata_class cmd_class =
			try == ATA_DEV_ATAPI ? ATA_DEV_ATAPI : ATA_DEV_ATA;

		if (cmd_class == ATA_DEV_ATAPI)
			tried_atapi = 1;
		else
			tried_ata = 1;
		kmemset(&res, 0, sizeof(res));
		rc = ata_read_id_retry(dev, cmd_class, id, &res);
		if (rc == 0) {
			if (cmd_class == ATA_DEV_ATA && ata_id_is_atapi(id) &&
			    id[ATA_ID_CONFIG] != ATA_ID_CONFIG_CFA) {
				if (tried_atapi)
					return -EIO;
				ata_dev_printk(dev, "IDENTIFY DEVICE returned a "
						    "packet device page, "
						    "reclassifying as ATAPI\n");
				try = ATA_DEV_ATAPI;
				continue;
			}
			if (cmd_class == ATA_DEV_ATAPI && !ata_id_is_atapi(id)) {
				if (tried_ata)
					return -EIO;
				ata_dev_printk(dev, "IDENTIFY PACKET DEVICE "
						    "returned an ATA page, "
						    "reclassifying as ATA\n");
				try = ATA_DEV_ATA;
				continue;
			}
			if (*class == ATA_DEV_ZAC && cmd_class == ATA_DEV_ATA)
				try = ATA_DEV_ZAC;
			*class = try;
			return 0;
		}
		/* Retries exhausted on something other than a clean abort:
		 * the device (or the link) is not usable. */
		if (rc != -EIO || !ata_tf_is_abort_only(&res))
			return rc;
		/* Aborted: what does the device say it is? */
		switch (ata_class_from_abort(&res)) {
		case ATA_DEV_ATAPI:
			if (tried_atapi)
				return -EIO;
			ata_dev_printk(dev, "IDENTIFY DEVICE aborted with the "
					    "packet signature: ATAPI device\n");
			try = ATA_DEV_ATAPI;
			continue;
		case ATA_DEV_PMP:
			ata_dev_printk(dev, "port multiplier signature on a "
					    "device link, ignored\n");
			*class = ATA_DEV_PMP;
			return -EOPNOTSUPP;
		case ATA_DEV_SEMB:
			ata_dev_printk(dev, "enclosure management bridge "
					    "(SEMB), not supported\n");
			*class = ATA_DEV_SEMB;
			return -EOPNOTSUPP;
		default:
			break;
		}
		if (cmd_class == ATA_DEV_ATA && !tried_atapi) {
			try = ATA_DEV_ATAPI;
			continue;
		}
		if (cmd_class == ATA_DEV_ATAPI && !tried_ata) {
			try = ATA_DEV_ATA;
			continue;
		}
		break;
	}
	ata_dev_printk(dev, "device answers neither IDENTIFY DEVICE nor "
			    "IDENTIFY PACKET DEVICE\n");
	return rc ? rc : -EIO;
}

/* Power-up in standby: the page may be incomplete and the media not
 * spinning.  Spin it up (SET FEATURES 07h when the device requires it,
 * otherwise any media access -- IDLE IMMEDIATE) and read the page again. */
static int ata_dev_puis_spinup(struct ata_device *dev, uint16_t *id,
			       enum ata_class class)
{
	uint16_t spec = id[ATA_ID_SPECIFIC_CFG];
	int incomplete = !!(id[ATA_ID_CONFIG] & ATA_ID_CONFIG_INCOMPLETE);
	int needs_cmd = (spec == ATA_ID_SPECCFG_SPINUP_INCOMPLETE ||
			 spec == ATA_ID_SPECCFG_SPINUP_COMPLETE);
	int rc;

	if (class == ATA_DEV_ATAPI)
		return 0;
	if (!needs_cmd && !incomplete && spec != ATA_ID_SPECCFG_INCOMPLETE)
		return 0;
	ata_dev_printk(dev, "powered up in standby (word 2 %04x%s), spinning "
			    "up\n",
		       spec, incomplete ? ", page incomplete" : "");
	if (needs_cmd) {
		rc = ata_spinup(dev);
	} else {
		struct ata_taskfile tf;

		kmemset(&tf, 0, sizeof(tf));
		tf.command = ATA_CMD_IDLEIMMEDIATE;
		tf.flags = ATA_TFLAG_DEVICE;
		rc = ata_exec_nodata(dev, &tf, ATA_TMOUT_SPINUP_MS, 0);
	}
	if (rc) {
		ata_dev_printk(dev, "spin-up failed (%d)\n", rc);
		return rc;
	}
	return ata_dev_read_id(dev, class, id);
}

static void ata_dev_print_identity(struct ata_device *dev)
{
	char ver[16];
	const char *geom;
	uint64_t bytes;

	ata_version_name(dev->major_version, ver, sizeof(ver));
	if (dev->class == ATA_DEV_ATAPI) {
		ata_dev_printk(dev, "ATAPI: %s, %s, max %s%s%s\n", dev->model,
			       dev->firmware,
			       dev->udma_mask ?
				       ata_xfer_mode_name(
					       (uint8_t)(XFER_UDMA_0 +
							 (31 - __builtin_clz(
								       dev->udma_mask)))) :
				       "PIO",
			       (dev->flags & ATA_DFLAG_ATAPI_DMA) ? ", DMA" : "",
			       (dev->flags & ATA_DFLAG_CDB16) ? ", CDB16" : "");
		return;
	}
	bytes = dev->n_sectors * dev->logical_sector_size;
	if (dev->logical_sector_size > 512)
		geom = "4Kn";
	else if (dev->physical_sector_size > dev->logical_sector_size)
		geom = "512e";
	else
		geom = "512n";
	ata_dev_printk(dev, "%s: %s, %s, max %s\n", ver, dev->model,
		       dev->firmware,
		       dev->udma_mask ?
			       ata_xfer_mode_name(
				       (uint8_t)(XFER_UDMA_0 +
						 (31 - __builtin_clz(
							       dev->udma_mask)))) :
			       "PIO4");
	ata_dev_printk(dev, "%llu sectors (%llu MB), %s %u/%u bytes, align "
			    "%u, multi %u: %s%s%s\n",
		       (unsigned long long)dev->n_sectors,
		       (unsigned long long)(bytes / 1000000ULL), geom,
		       dev->logical_sector_size, dev->physical_sector_size,
		       dev->alignment_offset, dev->multi_count,
		       (dev->flags & ATA_DFLAG_LBA48) ? "LBA48" : "LBA",
		       (dev->flags & ATA_DFLAG_NCQ) ? " NCQ" : "",
		       (dev->flags & ATA_DFLAG_NCQ_ON) ? "" :
		       (dev->flags & ATA_DFLAG_NCQ) ? " (not used)" : "");
	if (dev->flags & ATA_DFLAG_NCQ)
		ata_dev_printk(dev, "NCQ depth %u/%u%s%s\n", dev->ncq_depth,
			       dev->ncq_depth_dev,
			       (dev->flags & ATA_DFLAG_NCQ_SEND_RECV) ?
				       ", send/recv" :
				       "",
			       (dev->id[ATA_ID_SATA_CAPABILITY] &
				ATA_ID_SATA_NCQ_PRIO) ?
				       ", prio" :
				       "");
	ata_dev_printk(dev, "serial %s, %s, %s%s%s%s%s\n", dev->serial,
		       dev->rotation_rate == 1 ? "solid state" :
		       dev->rotation_rate ? "rotating" :
					    "rotation unknown",
		       (dev->flags & ATA_DFLAG_WCACHE) ? "write cache" :
							 "no write cache",
		       (dev->flags & ATA_DFLAG_FUA) ? ", FUA" : "",
		       (dev->flags & ATA_DFLAG_TRIM) ? ", TRIM" : "",
		       (dev->flags & ATA_DFLAG_TRIM_RZAT) ? " (zeroes)" : "",
		       (dev->flags & ATA_DFLAG_SMART) ? ", SMART" : "");
	if (dev->wwn)
		ata_dev_printk(dev, "world wide name %016llx\n",
			       (unsigned long long)dev->wwn);
	if (dev->rotation_rate > 1)
		ata_dev_printk(dev, "%u rpm, form factor %u\n",
			       dev->rotation_rate, dev->form_factor);
	if (dev->security.supported)
		ata_dev_printk(dev, "security: %s%s%s%s\n",
			       dev->security.enabled ? "enabled" :
						       "not enabled",
			       dev->security.locked ? ", LOCKED" : "",
			       dev->security.frozen ? ", frozen" : "",
			       dev->security.count_expired ? ", expired" : "");
}

/* Register what the device is.  eh_mutex held by the caller. */
static int ata_dev_attach_class(struct ata_device *dev)
{
	int rc;

	if (dev->class == ATA_DEV_ATAPI) {
		rc = ata_dev_configure(dev);
		if (rc == -ENODEV)
			return rc;
		dev->flags |= ATA_DFLAG_CONFIGURED;
		rc = atapi_dev_register(dev);
		if (rc) {
			ata_dev_printk(dev, "ATAPI registration failed (%d)\n",
				       rc);
			return rc;
		}
		dev->flags |= ATA_DFLAG_REGISTERED;
		return 0;
	}

	rc = ata_dev_configure(dev);
	if (rc == -ENODEV)
		return rc;
	if (rc)
		ata_dev_printk(dev, "configuration incomplete (%d), using the "
				    "device as it is\n",
			       rc);
	dev->flags |= ATA_DFLAG_CONFIGURED;
	if (dev->security.locked)
		ata_dev_printk(dev, "the drive is security LOCKED: media "
				    "access fails until it is unlocked\n");
	if ((dev->flags & ATA_DFLAG_SMART) && !dev->security.locked)
		ata_smart_report(dev);
	rc = ata_disk_register(dev);
	if (rc) {
		ata_dev_printk(dev, "disk registration failed (%d)\n", rc);
		return rc;
	}
	return 0;
}

int ata_dev_probe(struct ata_device *dev, enum ata_class class)
{
	uint16_t *id = NULL;
	const char *why = NULL;
	int rc;

	BUG_ON(!dev);
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep()) {
		might_sleep();
		ata_note_system_up();
	}
	ata_eh_ctx_enter(dev);
	dev->class = class;

	switch (class) {
	case ATA_DEV_NONE:
		rc = -ENODEV;
		goto out;
	case ATA_DEV_PMP:
		ata_dev_printk(dev, "port multiplier: enumerated by the "
				    "transport, not a device\n");
		rc = -EOPNOTSUPP;
		goto out;
	case ATA_DEV_SEMB:
		ata_dev_printk(dev, "enclosure management bridge (SEMB) "
				    "ignored\n");
		rc = -EOPNOTSUPP;
		goto out;
	default:
		break;
	}
	if (dev->flags & ATA_DFLAG_DETACHED) {
		rc = -ENODEV;
		goto out;
	}

	id = kalloc(ATA_ID_BYTES);
	if (!id) {
		rc = -ENOMEM;
		goto out;
	}
	rc = ata_dev_identify_classify(dev, &class, id);
	if (rc) {
		if (rc != -EOPNOTSUPP)
			ata_dev_printk(dev, "failed to IDENTIFY (%d), device "
					    "ignored\n",
				       rc);
		dev->class = class;
		goto out;
	}
	dev->class = class;

	rc = ata_dev_puis_spinup(dev, id, class);
	if (rc)
		goto out;

	rc = ata_id_parse_why(dev, id, &why);
	if (rc) {
		ata_dev_printk(dev, "unusable IDENTIFY data: %s\n",
			       why ? why : "?");
		goto out;
	}
	dev->power_state = ATA_PWR_ACTIVE;
	ata_dev_print_identity(dev);
	if (class == ATA_DEV_ZAC)
		ata_dev_printk(dev, "host-managed zoned device: registered "
				    "read-only\n");

	rc = ata_dev_attach_class(dev);
out:
	if (id)
		kfree(id);
	ata_eh_ctx_exit(dev);
	return rc;
}

int ata_dev_revalidate(struct ata_device *dev)
{
	struct ata_device *tmp = NULL;
	struct ata_dev_priv *p;
	uint16_t *id = NULL;
	uint64_t old_sectors;
	uint64_t old_flags;
	uint32_t old_depth;
	const char *why = NULL;
	enum ata_class class;
	int rc;

	BUG_ON(!dev);
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	ata_eh_ctx_enter(dev);
	p = ata_dev_priv(dev);

	if (dev->flags & ATA_DFLAG_DETACHED) {
		rc = -ENODEV;
		goto out;
	}
	if (dev->class != ATA_DEV_ATA && dev->class != ATA_DEV_ATAPI &&
	    dev->class != ATA_DEV_ZAC) {
		rc = -ENODEV;
		goto out;
	}
	__atomic_add_fetch(&dev->n_revalidations, 1, __ATOMIC_RELAXED);
	/* Whatever reset brought us here also ended a SLEEP. */
	dev->flags &= ~ATA_DFLAG_SLEEPING;
	if (dev->power_state == ATA_PWR_SLEEP)
		dev->power_state = ATA_PWR_UNKNOWN;

	id = kalloc(ATA_ID_BYTES);
	tmp = kalloc(sizeof(*tmp));
	if (!id || !tmp) {
		rc = -ENOMEM;
		goto out;
	}
	class = dev->class;
	rc = ata_dev_read_id(dev, class, id);
	if (rc) {
		ata_dev_printk(dev, "revalidation: IDENTIFY failed (%d)\n", rc);
		goto out;
	}
	rc = ata_dev_puis_spinup(dev, id, class);
	if (rc)
		goto out;

	/* Is it the same drive?  Parse into a scratch device first. */
	kmemset(tmp, 0, sizeof(*tmp));
	tmp->magic = ATA_DEVICE_MAGIC;
	tmp->class = dev->class;
	tmp->host_max_segments = dev->host_max_segments;
	tmp->host_max_sectors = dev->host_max_sectors;
	tmp->host_queue_depth = dev->host_queue_depth;
	tmp->host_ncq = dev->host_ncq;
	tmp->host_fua = dev->host_fua;
	tmp->n_native_sectors = dev->n_native_sectors;
	rc = ata_id_parse_why(tmp, id, &why);
	if (rc) {
		ata_dev_printk(dev, "revalidation: unusable IDENTIFY data: %s\n",
			       why ? why : "?");
		goto out;
	}
	if ((ata_id_is_atapi(id) != (dev->class == ATA_DEV_ATAPI)) ||
	    kstrcmp(tmp->model, dev->model) != 0 ||
	    kstrcmp(tmp->serial, dev->serial) != 0) {
		ata_dev_printk(dev, "a different drive answers now (\"%s\" "
				    "serial \"%s\", was \"%s\" serial \"%s\"): "
				    "taking %s offline\n",
			       tmp->model, tmp->serial, dev->model,
			       dev->serial, dev->disk ? dev->disk->disk_name :
							"the device");
		/* Let hot-plug handling probe the newcomer afresh. */
		if (dev->xport->schedule_eh)
			dev->xport->schedule_eh(dev, AHCI_EH_HOTPLUG);
		ata_dev_detach(dev);
		rc = -ENODEV;
		goto out;
	}

	old_sectors = dev->n_sectors;
	old_flags = dev->flags;
	old_depth = dev->ncq_depth;
	rc = ata_id_parse_why(dev, id, &why);
	if (rc)
		goto out;
	/* NCQ that error handling (or a policy) switched off stays off, a
	 * lowered depth stays lowered. */
	if (p && (old_flags & ATA_DFLAG_CONFIGURED)) {
		if (!(old_flags & ATA_DFLAG_NCQ_ON) &&
		    (dev->flags & ATA_DFLAG_NCQ_ON)) {
			p->ncq_forced_off = 1;
		}
		if (old_depth && old_depth < dev->ncq_depth)
			p->ncq_depth_limit = old_depth;
	}
	if (dev->n_sectors != old_sectors && dev->class != ATA_DEV_ATAPI) {
		ata_dev_printk(dev, "capacity changed from %llu to %llu "
				    "sectors\n",
			       (unsigned long long)old_sectors,
			       (unsigned long long)dev->n_sectors);
	}

	rc = ata_dev_configure(dev);
	if (rc == -ENODEV)
		goto out;
	rc = 0;
	dev->flags |= ATA_DFLAG_CONFIGURED;
	dev->flags &= ~ATA_DFLAG_NEEDS_REVALIDATE;
	if (dev->power_state == ATA_PWR_UNKNOWN)
		dev->power_state = ATA_PWR_ACTIVE;
	if (dev->n_sectors != old_sectors && dev->class != ATA_DEV_ATAPI) {
		ata_rw_update_disk_limits(dev);
		ata_work_set_capacity(dev, dev->n_sectors);
	} else {
		ata_rw_update_disk_limits(dev);
	}
	/* Requests given back while the device was asleep or the port in
	 * error handling may go on: run the disk's queue. */
	ata_rw_kick(dev);
out:
	if (id)
		kfree(id);
	if (tmp)
		kfree(tmp);
	ata_eh_ctx_exit(dev);
	return rc;
}

void ata_dev_detach(struct ata_device *dev)
{
	BUG_ON(!dev);
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	ata_eh_ctx_enter(dev);
	if (dev->flags & ATA_DFLAG_DETACHED) {
		ata_eh_ctx_exit(dev);
		return;
	}
	dev->flags |= ATA_DFLAG_DETACHED;
	dev->power_state = ATA_PWR_UNKNOWN;
	/* Not a root candidate (or anything else) any more. */
	if (dev->disk)
		dev->disk->bdev.ready = 0;
	if (dev->xport == &ahci_ata_transport)
		ahci_sysfs_remove_device(dev);
	if (dev->disk)
		ata_dev_printk(dev, "detached (%s)\n", dev->disk->disk_name);
	else
		ata_dev_printk(dev, "detached\n");

	/* Block requests the transport holds are failed by it (it reports
	 * the unplug); the queue's own -- requests waiting for their next
	 * step included -- are failed by disk_del(). */

	if (dev->class == ATA_DEV_ATAPI) {
		if (dev->flags & ATA_DFLAG_REGISTERED)
			atapi_dev_unregister(dev);
	} else {
		ata_disk_unregister(dev);
	}
	dev->flags &= ~ATA_DFLAG_REGISTERED;
	ata_eh_ctx_exit(dev);
}
