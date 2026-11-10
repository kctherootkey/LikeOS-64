// LikeOS ATA layer - SCSI/ATA translation, SG_IO, HDIO and SCSI ioctls
//
// Userspace storage tools speak SCSI.  smartctl, hdparm, sdparm, sg_inq,
// lsblk/udev identity probes and the partitioning tools send command
// descriptor blocks through ioctl(SG_IO) and expect a SCSI device to
// answer them -- on a SATA disk the kernel translates.  This file is that
// translator, following the SCSI/ATA Translation standard (SAT):
//
//   INQUIRY               standard data ("ATA     " + model + firmware) and
//                         the vital product data pages 00h (list), 80h
//                         (serial), 83h (identification: T10 vendor id,
//                         NAA from the WWN, the serial), 89h (ATA
//                         information: signature + the whole IDENTIFY
//                         page -- what smartctl and udev read), B0h (block
//                         limits), B1h (block characteristics: rotation
//                         rate, form factor), B2h (logical block
//                         provisioning)
//   READ CAPACITY(10/16)  incl. the logical-per-physical exponent, lowest
//                         aligned LBA and LBPME/LBPRZ
//   TEST UNIT READY, REQUEST SENSE (REQUEST SENSE DATA EXT where the drive
//                         reports sense data)
//   READ/WRITE(6/10/12/16), VERIFY(10/12/16) incl. BYTCHK compare
//   SYNCHRONIZE CACHE(10/16)  -> FLUSH CACHE (EXT)
//   UNMAP, WRITE SAME(10/16)  -> DATA SET MANAGEMENT TRIM or writes
//   START STOP UNIT       -> STANDBY IMMEDIATE / IDLE IMMEDIATE / wake
//   MODE SENSE(6/10)      read/write error recovery, caching, control,
//                         informational exceptions; MODE SELECT(6/10)
//                         caching (WCE, DRA) and control (D_SENSE)
//   LOG SENSE             supported pages, temperature, self-test results,
//                         informational exceptions
//   SEND DIAGNOSTIC       -> SMART self-tests / EXECUTE DEVICE DIAGNOSTIC
//   REPORT LUNS, REPORT SUPPORTED OPERATION CODES, GET LBA STATUS
//   ATA PASS-THROUGH(12/16)  every protocol (non-data, PIO in/out, DMA,
//                         UDMA, FPDMA, the resets, device diagnostic,
//                         return response information) with CK_COND, T_DIR,
//                         BYT_BLOK, T_TYPE and T_LENGTH, the result
//                         registers returned in an ATA status return
//                         descriptor (descriptor sense) -- what smartctl
//                         and hdparm are built on.
//
// and everything else answers CHECK CONDITION, ILLEGAL REQUEST, INVALID
// COMMAND OPERATION CODE.  The pure builders (INQUIRY data, VPD pages,
// READ CAPACITY data, sense data, the PASS-THROUGH decoder) are exported
// for the boxed host tests.
//
// On an ATAPI drive (srN) the CDB is not translated: it goes to the drive
// as a PACKET command (atapi_exec_cdb_resid).
//
// The ioctls of an ATA disk / optical drive node (blk_ops.ioctl, tried
// before the generic block ioctls):
//
//   SG_IO                 struct sg_io_hdr: CDB, data (one buffer or an
//                         iovec list), sense buffer, timeout; status, host
//                         and driver status, residual and duration back
//   HDIO_GET_IDENTITY, HDIO_GETGEO, HDIO_GET_32BIT/MULTCOUNT/DMA/WCACHE/
//   ACOUSTIC/KEEPSETTINGS/NOWERR/UNMASKINTR/BUSSTATE/ADDRESS/QDMA/NICE,
//   HDIO_SET_WCACHE/ACOUSTIC/MULTCOUNT/KEEPSETTINGS/32BIT/DMA/...,
//   HDIO_DRIVE_CMD, HDIO_DRIVE_TASK, HDIO_DRIVE_TASKFILE, HDIO_DRIVE_RESET
//   SCSI_IOCTL_TEST_UNIT_READY/START_UNIT/STOP_UNIT/SYNC/DOORLOCK/
//   DOORUNLOCK/PROBE_HOST/GET_PCI, and the optical drive requests eject,
//   close tray, lock door, drive status and media changed.
//
// Permissions follow the established semantics: SG_IO needs root except
// for a list of harmless read-only commands on a node opened for reading;
// HDIO_DRIVE_* and every HDIO_SET_* need root; the GET requests do not.
// User memory is only touched through copy_from_user()/copy_to_user().
//
// Pass-through commands to a registered disk travel through its request
// queue (BLK_OP_DRV_IN/OUT carrying the prepared struct ata_queued_cmd),
// so they are ordered with the filesystem's I/O and never race the NCQ
// queue; before the disk exists they go straight to ata_qc_exec().
//
// Context: the builders are pure; everything that executes may sleep.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/cred.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

#define SAT_PFX "[ata] "

/* The largest data transfer one SG_IO / HDIO command may carry.  Larger
 * requests fail with ENOMEM, as with the established reserved-buffer
 * limit -- tools split their transfers. */
#define SAT_SG_MAX_XFER (4u * 1024 * 1024)
#define SAT_SG_MAX_IOVEC 256u
/* UNMAP parameter list we accept (descriptors x 16 + 8) */
#define SAT_UNMAP_MAX_PARAM 65536u
#define SAT_UNMAP_MAX_DESC ((SAT_UNMAP_MAX_PARAM - 8) / 16)
/* Sectors per block-layer call of the read/write translation */
#define SAT_RW_CHUNK 8192u
/* WRITE SAME without UNMAP: the pattern buffer */
#define SAT_WSAME_BUF (256u * 1024)
/* The SAT layer identification in VPD page 89h */
#define SAT_VENDOR_ID "LikeOS  "
#define SAT_PRODUCT_ID "SATA translator "
#define SAT_PRODUCT_REV "1.00"

/* INQUIRY version descriptors (SPC-4 table of standards) */
#define SAT_VD_SAM5 0x00A0
#define SAT_VD_SPC4 0x0460
#define SAT_VD_SBC3 0x04C0
#define SAT_VD_SAT3 0x1EE0
#define SAT_VD_ZBC 0x0620

/* The standard INQUIRY data length this translator returns */
#define SAT_INQ_STD_LEN 96u
/* VPD 89h total length */
#define SAT_VPD89_LEN 572u

/* Optical drive requests (the established CD-ROM ioctl numbers; the
 * userspace header is not part of this system's UAPI yet, so the values
 * live here). */
#define SAT_CDROMEJECT 0x5309
#define SAT_CDROMCLOSETRAY 0x5319
#define SAT_CDROM_LOCKDOOR 0x5329
#define SAT_CDROM_DRIVE_STATUS 0x5326
#define SAT_CDROM_MEDIA_CHANGED 0x5325
#define SAT_CDROM_GET_CAPABILITY 0x5331
#define SAT_CDS_NO_INFO 0
#define SAT_CDS_NO_DISC 1
#define SAT_CDS_TRAY_OPEN 2
#define SAT_CDS_DRIVE_NOT_READY 3
#define SAT_CDS_DISC_OK 4
#define SAT_CDC_CLOSE_TRAY 0x1
#define SAT_CDC_OPEN_TRAY 0x2
#define SAT_CDC_LOCK 0x4
#define SAT_CDC_MEDIA_CHANGED 0x80
#define SAT_CDC_GENERIC_PACKET 0x80000
#define SAT_CDC_DRIVE_STATUS 0x800

/* MMC opcodes a non-root user may send to an optical drive */
#define MMC_READ_FORMAT_CAPACITIES 0x23
#define MMC_READ_SUB_CHANNEL 0x42
#define MMC_READ_TRACK_INFO 0x52
#define MMC_READ_BUFFER_CAPACITY 0x5C
#define MMC_REPORT_KEY 0xA4
#define MMC_READ_DVD_STRUCTURE 0xAD
#define MMC_READ_CD_MSF 0xB9

/* Sense data formats */
#define SENSE_FIXED_CURRENT 0x70
#define SENSE_DESC_CURRENT 0x72
#define SENSE_FIXED_LEN 18u
#define SENSE_DESC_HDR_LEN 8u
#define SENSE_DESC_INFO 0x00
#define SENSE_DESC_SKS 0x02
#define SENSE_DESC_ATA 0x09

/* ==========================================================================
 * Byte order helpers
 * ========================================================================== */

static inline void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void put_be64(uint8_t *p, uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4, (uint32_t)v);
}

static inline uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get_be24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t get_be64(const uint8_t *p)
{
	return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static inline uint32_t sat_min32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

/* Copy `n' bytes of a response into the caller's buffer, bounded by its
 * length: returns how many were copied. */
static uint32_t sat_copy_out(uint8_t *dst, uint32_t cap, const uint8_t *src,
			     uint32_t n)
{
	uint32_t m = sat_min32(cap, n);

	if (m)
		kmemcpy(dst, src, m);
	return m;
}

/* ==========================================================================
 * IDENTIFY strings in their raw form (pure)
 * ========================================================================== */

/* ATA strings: two characters per word, the first in the high byte.  This
 * copies `len' characters starting at word `word' without trimming --
 * VPD/INQUIRY fields are fixed-width and space padded. */
static void sat_id_raw_string(const uint16_t *id, int word, int len,
			      uint8_t *out)
{
	for (int i = 0; i < len / 2; i++) {
		uint16_t w = id[word + i];
		uint8_t c0 = (uint8_t)(w >> 8), c1 = (uint8_t)w;

		out[2 * i] = (c0 >= 0x20 && c0 < 0x7F) ? c0 : ' ';
		out[2 * i + 1] = (c1 >= 0x20 && c1 < 0x7F) ? c1 : ' ';
	}
}

/* A fixed-width ASCII field from a C string, space padded. */
static void sat_pad_string(uint8_t *out, int width, const char *s)
{
	int i = 0;

	for (; s && s[i] && i < width; i++)
		out[i] = (uint8_t)s[i];
	for (; i < width; i++)
		out[i] = ' ';
}

/* The WWN from IDENTIFY words 108..111 (word 108 most significant), 0
 * when not reported. */
static uint64_t sat_wwn(const struct ata_device *dev)
{
	const uint16_t *id = dev->id;
	uint64_t w;

	if (!ATA_ID_WORD_VALID(id[ATA_ID_CSF_DEFAULT]) ||
	    !(id[ATA_ID_CSF_DEFAULT] & ATA_ID_CSE_WWN))
		return dev->wwn;
	w = ((uint64_t)id[ATA_ID_WWN] << 48) |
	    ((uint64_t)id[ATA_ID_WWN + 1] << 32) |
	    ((uint64_t)id[ATA_ID_WWN + 2] << 16) | id[ATA_ID_WWN + 3];
	return w ? w : dev->wwn;
}

static uint32_t sat_lbs(const struct ata_device *dev)
{
	uint32_t s = dev->logical_sector_size;

	return (s >= 512 && (s & (s - 1)) == 0) ? s : ATA_SECT_SIZE;
}

/* Logical sectors per physical, as the exponent READ CAPACITY(16) wants */
static uint8_t sat_log2_per_phys(const struct ata_device *dev)
{
	uint8_t e = dev->log2_per_phys;

	if (e == 0) {
		uint16_t w = dev->id[ATA_ID_SECTOR_SIZE];

		if ((w & 0xC000) == 0x4000 && (w & ATA_ID_SS_MULTI_LOGICAL))
			e = (uint8_t)ATA_ID_SS_LOG2_PER_PHYS(w);
	}
	return e & 0x0F;
}

/* The lowest aligned LBA: IDENTIFY word 209 gives the offset of LBA 0
 * inside the first physical sector. */
static uint16_t sat_lowest_aligned(const struct ata_device *dev)
{
	uint16_t w = dev->id[ATA_ID_SECTOR_ALIGNMENT];
	uint32_t per = 1u << sat_log2_per_phys(dev);
	uint32_t ofs;

	if ((w & 0xC000) != 0x4000 || per <= 1)
		return 0;
	ofs = ATA_ID_ALIGN_OFFSET(w) % per;
	return (uint16_t)((per - ofs) % per);
}

static int sat_is_ro(const struct ata_device *dev)
{
	return dev->disk && dev->disk->bdev.ro;
}

/* ==========================================================================
 * Sense data (pure)
 * ========================================================================== */

struct sat_sense_info {
	uint8_t key, asc, ascq;
	uint8_t has_info;
	uint64_t info; /* information field: a failing LBA */
	uint8_t has_sks;
	uint8_t sks[3]; /* sense key specific bytes */
	const struct ata_taskfile *ata_tf; /* ATA status return */
	int extend;
};

void ata_sat_ata_status_desc(const struct ata_taskfile *tf, int extend,
			     uint8_t desc[14])
{
	kmemset(desc, 0, 14);
	desc[0] = SENSE_DESC_ATA;
	desc[1] = 0x0C;
	if (!tf)
		return;
	desc[2] = extend ? 0x01 : 0x00;
	desc[3] = tf->error;
	desc[4] = extend ? tf->hob_nsect : 0;
	desc[5] = tf->nsect;
	desc[6] = extend ? tf->hob_lbal : 0;
	desc[7] = tf->lbal;
	desc[8] = extend ? tf->hob_lbam : 0;
	desc[9] = tf->lbam;
	desc[10] = extend ? tf->hob_lbah : 0;
	desc[11] = tf->lbah;
	desc[12] = tf->device;
	desc[13] = tf->status;
}

/* Build fixed or descriptor sense.  Returns the bytes written (bounded by
 * cap). */
static uint32_t sat_sense_build(uint8_t *out, uint32_t cap, int descriptor,
				const struct sat_sense_info *si)
{
	uint8_t s[ATA_SENSE_LEN];
	uint32_t len;

	kmemset(s, 0, sizeof(s));
	if (descriptor) {
		uint32_t p = SENSE_DESC_HDR_LEN;

		s[0] = SENSE_DESC_CURRENT;
		s[1] = si->key & 0x0F;
		s[2] = si->asc;
		s[3] = si->ascq;
		if (si->has_info) {
			s[p + 0] = SENSE_DESC_INFO;
			s[p + 1] = 0x0A;
			s[p + 2] = 0x80; /* VALID */
			put_be64(s + p + 4, si->info);
			p += 12;
		}
		if (si->has_sks) {
			s[p + 0] = SENSE_DESC_SKS;
			s[p + 1] = 0x06;
			s[p + 4] = si->sks[0];
			s[p + 5] = si->sks[1];
			s[p + 6] = si->sks[2];
			p += 8;
		}
		if (si->ata_tf) {
			ata_sat_ata_status_desc(si->ata_tf, si->extend, s + p);
			p += 14;
		}
		s[7] = (uint8_t)(p - SENSE_DESC_HDR_LEN);
		len = p;
	} else {
		s[0] = SENSE_FIXED_CURRENT;
		s[2] = si->key & 0x0F;
		s[7] = (uint8_t)(SENSE_FIXED_LEN - 8);
		s[12] = si->asc;
		s[13] = si->ascq;
		if (si->ata_tf) {
			/* ATA PASS-THROUGH in fixed format: INFORMATION =
			 * error, status, device, count(7:0); COMMAND-SPECIFIC
			 * INFORMATION = extend/upper-nonzero flags, LBA 23:0 */
			const struct ata_taskfile *tf = si->ata_tf;

			s[3] = tf->error;
			s[4] = tf->status;
			s[5] = tf->device;
			s[6] = tf->nsect;
			s[8] = (uint8_t)((si->extend ? 0x80 : 0) |
					 (si->extend && tf->hob_nsect ? 0x40 : 0) |
					 (si->extend && (tf->hob_lbal ||
							 tf->hob_lbam ||
							 tf->hob_lbah)
						  ? 0x20
						  : 0));
			s[9] = tf->lbal;
			s[10] = tf->lbam;
			s[11] = tf->lbah;
		} else if (si->has_info) {
			s[0] |= 0x80; /* VALID */
			put_be32(s + 3, si->info > 0xFFFFFFFFull
						? 0xFFFFFFFFu
						: (uint32_t)si->info);
		}
		if (si->has_sks) {
			s[15] = si->sks[0];
			s[16] = si->sks[1];
			s[17] = si->sks[2];
		}
		len = SENSE_FIXED_LEN;
	}
	return sat_copy_out(out, cap, s, len);
}

uint32_t ata_sat_build_sense(uint8_t *sense, uint32_t cap, int descriptor,
			     uint8_t key, uint8_t asc, uint8_t ascq,
			     const struct ata_taskfile *tf)
{
	struct sat_sense_info si;

	if (!sense || cap == 0)
		return 0;
	kmemset(&si, 0, sizeof(si));
	si.key = key;
	si.asc = asc;
	si.ascq = ascq;
	si.ata_tf = tf;
	si.extend = tf ? ((tf->flags & ATA_TFLAG_LBA48) ? 1 : 0) : 0;
	return sat_sense_build(sense, cap, descriptor, &si);
}

void ata_sat_error_to_sense(uint8_t status, uint8_t error, uint8_t *key,
			    uint8_t *asc, uint8_t *ascq)
{
	uint8_t k = ABORTED_COMMAND, a = 0x00, q = 0x00;

	if (status & ATA_BUSY) {
		/* BSY still set: the command never finished properly */
		k = ABORTED_COMMAND;
		a = 0x00;
		q = 0x00;
	} else if (status & ATA_DF) {
		k = HARDWARE_ERROR;
		a = SAT_ASC_INTERNAL_TARGET_FAILURE;
		q = 0x00;
	} else if (!(status & ATA_ERR)) {
		k = NO_SENSE;
	} else if ((error & ATA_ICRC) && (error & ATA_ABORTED)) {
		/* interface CRC error: retryable, not a media problem */
		k = ABORTED_COMMAND;
		a = SAT_ASC_IU_CRC_ERROR;
		q = 0x03;
	} else if (error & ATA_UNC) {
		k = MEDIUM_ERROR;
		a = SAT_ASC_UNRECOVERED_READ;
		q = 0x04; /* unrecovered read error - auto reallocate failed */
	} else if (error & ATA_IDNF) {
		k = MEDIUM_ERROR;
		a = SAT_ASC_ID_NOT_FOUND;
		q = 0x01; /* record not found */
	} else if (error & ATA_AMNF) {
		k = MEDIUM_ERROR;
		a = 0x13; /* address mark not found for data field */
		q = 0x00;
	} else if (error & ATA_TRK0NF) {
		k = HARDWARE_ERROR;
		a = 0x06; /* no reference position found */
		q = 0x00;
	} else if (error & ATA_MC) {
		k = UNIT_ATTENTION;
		a = SAT_ASC_MEDIUM_CHANGED;
		q = 0x00;
	} else if (error & ATA_MCR) {
		k = UNIT_ATTENTION;
		a = 0x5A; /* operator medium removal request */
		q = 0x01;
	} else if (error & ATA_ICRC) {
		k = ABORTED_COMMAND;
		a = SAT_ASC_IU_CRC_ERROR;
		q = 0x03;
	} else if (error & ATA_ABORTED) {
		k = ABORTED_COMMAND;
		a = 0x00;
		q = 0x00;
	}
	if (key)
		*key = k;
	if (asc)
		*asc = a;
	if (ascq)
		*ascq = q;
}

/* ==========================================================================
 * Per-device translator state
 * ========================================================================== */

#define SAT_MAX_STATES 64

struct sat_state {
	const struct ata_device *dev;
	char serial[ATA_ID_SERNO_LEN + 1]; /* guards against a reused address */
	uint8_t d_sense; /* control mode page D_SENSE */
	uint8_t has_last_pt;
	uint8_t last_pt_extend;
	struct ata_taskfile last_pt_tf; /* RETURN RESPONSE INFORMATION */
};

static struct sat_state g_sat_states[SAT_MAX_STATES];
static spinlock_t g_sat_lock = SPINLOCK_INIT("ata_sat");

/* Copy-out of the state (under the lock) and write-back helpers keep the
 * state consistent without holding a spinlock across commands. */
static int sat_state_index(const struct ata_device *dev, int create)
{
	int free_idx = -1;

	lockdep_assert_held(&g_sat_lock);
	for (int i = 0; i < SAT_MAX_STATES; i++) {
		struct sat_state *s = &g_sat_states[i];

		if (s->dev == dev) {
			if (kstrcmp(s->serial, dev->serial) == 0)
				return i;
			/* same address, another device: start afresh */
			kmemset(s, 0, sizeof(*s));
			s->dev = dev;
			kstrncpy(s->serial, dev->serial, ATA_ID_SERNO_LEN);
			return i;
		}
		if (!s->dev && free_idx < 0)
			free_idx = i;
	}
	if (!create)
		return -1;
	if (free_idx < 0) {
		/* full: recycle slot 0's neighbour round-robin */
		static int victim;

		free_idx = victim;
		victim = (victim + 1) % SAT_MAX_STATES;
	}
	kmemset(&g_sat_states[free_idx], 0, sizeof(g_sat_states[0]));
	g_sat_states[free_idx].dev = dev;
	kstrncpy(g_sat_states[free_idx].serial, dev->serial, ATA_ID_SERNO_LEN);
	return free_idx;
}

static int sat_get_d_sense(const struct ata_device *dev)
{
	uint64_t fl;
	int i, d = 0;

	spin_lock_irqsave(&g_sat_lock, &fl);
	i = sat_state_index(dev, 0);
	if (i >= 0)
		d = g_sat_states[i].d_sense;
	spin_unlock_irqrestore(&g_sat_lock, fl);
	return d;
}

static void sat_set_d_sense(const struct ata_device *dev, int on)
{
	uint64_t fl;
	int i;

	spin_lock_irqsave(&g_sat_lock, &fl);
	i = sat_state_index(dev, 1);
	if (i >= 0)
		g_sat_states[i].d_sense = (uint8_t)(on ? 1 : 0);
	spin_unlock_irqrestore(&g_sat_lock, fl);
}

static void sat_set_last_pt(const struct ata_device *dev,
			    const struct ata_taskfile *tf, int extend)
{
	uint64_t fl;
	int i;

	spin_lock_irqsave(&g_sat_lock, &fl);
	i = sat_state_index(dev, 1);
	if (i >= 0) {
		g_sat_states[i].last_pt_tf = *tf;
		g_sat_states[i].last_pt_extend = (uint8_t)extend;
		g_sat_states[i].has_last_pt = 1;
	}
	spin_unlock_irqrestore(&g_sat_lock, fl);
}

static int sat_get_last_pt(const struct ata_device *dev,
			   struct ata_taskfile *tf, int *extend)
{
	uint64_t fl;
	int i, ok = 0;

	spin_lock_irqsave(&g_sat_lock, &fl);
	i = sat_state_index(dev, 0);
	if (i >= 0 && g_sat_states[i].has_last_pt) {
		*tf = g_sat_states[i].last_pt_tf;
		*extend = g_sat_states[i].last_pt_extend;
		ok = 1;
	}
	spin_unlock_irqrestore(&g_sat_lock, fl);
	return ok;
}

/* ==========================================================================
 * Result helpers
 * ========================================================================== */

/* One command being translated */
struct sat_cmd {
	struct ata_device *dev;
	const uint8_t *cdb;
	int cdb_len;
	uint8_t *buf;
	uint32_t len;
	enum ata_dma_dir dir;
	uint32_t timeout_ms;
	struct ata_sat_result *res;
	int d_sense;
};

static void sat_good(struct sat_cmd *c, uint32_t transferred)
{
	c->res->status = SAM_STAT_GOOD;
	c->res->sense_len = 0;
	c->res->resid = c->len > transferred ? c->len - transferred : 0;
}

static void sat_check_info(struct sat_cmd *c, const struct sat_sense_info *si)
{
	c->res->status = SAM_STAT_CHECK_CONDITION;
	c->res->sense_len = sat_sense_build(c->res->sense, ATA_SENSE_LEN,
					    c->d_sense, si);
	c->res->driver_status = SG_ERR_DRIVER_SENSE;
}

static void sat_check(struct sat_cmd *c, uint8_t key, uint8_t asc,
		      uint8_t ascq)
{
	struct sat_sense_info si;

	kmemset(&si, 0, sizeof(si));
	si.key = key;
	si.asc = asc;
	si.ascq = ascq;
	sat_check_info(c, &si);
	c->res->resid = c->len;
}

/* ILLEGAL REQUEST with a field pointer: `in_cdb' 1 for a CDB field (ASC
 * 24h), 0 for a parameter list field (ASC 26h); bit < 0: no bit pointer. */
static void sat_invalid(struct sat_cmd *c, int in_cdb, int byte, int bit)
{
	struct sat_sense_info si;

	kmemset(&si, 0, sizeof(si));
	si.key = ILLEGAL_REQUEST;
	si.asc = in_cdb ? SAT_ASC_INVALID_FIELD_CDB : SAT_ASC_INVALID_FIELD_PARAM;
	si.ascq = 0;
	if (byte >= 0) {
		si.has_sks = 1;
		si.sks[0] = (uint8_t)(0x80 | (in_cdb ? 0x40 : 0) |
				      (bit >= 0 ? (0x08 | (bit & 7)) : 0));
		si.sks[1] = (uint8_t)(byte >> 8);
		si.sks[2] = (uint8_t)byte;
	}
	sat_check_info(c, &si);
	c->res->resid = c->len;
}

static void sat_invalid_field(struct sat_cmd *c, int byte, int bit)
{
	sat_invalid(c, 1, byte, bit);
}

static void sat_invalid_param(struct sat_cmd *c, int byte, int bit)
{
	sat_invalid(c, 0, byte, bit);
}

static void sat_invalid_opcode(struct sat_cmd *c)
{
	struct sat_sense_info si;

	kmemset(&si, 0, sizeof(si));
	si.key = ILLEGAL_REQUEST;
	si.asc = SAT_ASC_INVALID_OPCODE;
	si.has_sks = 1;
	si.sks[0] = 0xC0; /* SKSV, C/D: byte 0 */
	sat_check_info(c, &si);
	c->res->resid = c->len;
}

static void sat_lba_out_of_range(struct sat_cmd *c)
{
	sat_check(c, ILLEGAL_REQUEST, SAT_ASC_LBA_OUT_OF_RANGE, 0x00);
}

/* A transport failure (-errno) -> host/driver status without sense. */
static void sat_host_error(struct sat_cmd *c, int err)
{
	struct ata_sat_result *r = c->res;

	r->status = SAM_STAT_GOOD;
	r->resid = c->len;
	switch (err) {
	case -ETIMEDOUT:
		r->host_status = SG_ERR_DID_TIME_OUT;
		r->driver_status = SG_ERR_DRIVER_TIMEOUT;
		break;
	case -ENODEV:
	case -ENXIO:
		r->host_status = SG_ERR_DID_NO_CONNECT;
		break;
	case -EBUSY:
	case -EAGAIN:
		r->host_status = SG_ERR_DID_BUS_BUSY;
		r->driver_status = SG_ERR_DRIVER_BUSY;
		break;
	case -ECANCELED:
		r->host_status = SG_ERR_DID_RESET;
		r->driver_status = SG_ERR_DRIVER_SOFT;
		break;
	case -ENOMEM:
		r->host_status = SG_ERR_DID_ERROR;
		r->driver_status = SG_ERR_DRIVER_BUSY;
		break;
	case -ENOMEDIUM:
		/* not a host failure: report it the SCSI way */
		sat_check(c, NOT_READY, SAT_ASC_MEDIUM_NOT_PRESENT, 0x00);
		return;
	case -EROFS:
		sat_check(c, DATA_PROTECT, SAT_ASC_WRITE_PROTECTED, 0x00);
		return;
	case -EACCES:
		/* a security-locked drive refuses media access */
		sat_check(c, DATA_PROTECT, 0x74, 0x71);
		return;
	case -EINVAL:
		sat_check(c, ILLEGAL_REQUEST, SAT_ASC_INVALID_FIELD_CDB, 0x00);
		return;
	case -EOPNOTSUPP:
		sat_invalid_opcode(c);
		return;
	default:
		r->host_status = SG_ERR_DID_ERROR;
		r->driver_status = SG_ERR_DRIVER_HARD;
		break;
	}
}

/* REQUEST SENSE DATA EXT: the drive's own sense for the last error, when
 * the drive reports sense data.  0 with key/asc/ascq, -errno otherwise. */
static int sat_request_sense_data_ext(struct ata_device *dev, uint8_t *key,
				      uint8_t *asc, uint8_t *ascq)
{
	struct ata_taskfile tf;
	int ret;

	if (!(dev->flags & ATA_DFLAG_SENSE_DATA))
		return -EOPNOTSUPP;
	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_REQ_SENSE_DATA;
	tf.device = ATA_LBA;
	tf.flags = ATA_TFLAG_LBA48 | ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	ret = ata_exec_internal(dev, &tf, NULL, ATA_PROT_NODATA, NULL, 0,
				ATA_DIR_NONE, ATA_TMOUT_INTERNAL_MS,
				ATA_QCFLAG_RESULT_TF | ATA_QCFLAG_QUIET);
	if (ret)
		return ret;
	/* SENSE KEY in LBA 19:16, ASC in 15:8, ASCQ in 7:0 */
	*key = tf.lbah & 0x0F;
	*asc = tf.lbam;
	*ascq = tf.lbal;
	return 0;
}

/* A failed ATA command -> CHECK CONDITION with the best sense available.
 * `lba' is reported as the information field when `has_lba'. */
static void sat_ata_error(struct sat_cmd *c, const struct ata_taskfile *rtf,
			  int write, int has_lba, uint64_t lba)
{
	struct sat_sense_info si;

	kmemset(&si, 0, sizeof(si));
	if ((rtf->status & ATA_SENSE) &&
	    sat_request_sense_data_ext(c->dev, &si.key, &si.asc, &si.ascq) ==
		    0 &&
	    si.key) {
		/* the drive's own sense */
	} else {
		ata_sat_error_to_sense(rtf->status, rtf->error, &si.key,
				       &si.asc, &si.ascq);
		if (write && si.key == MEDIUM_ERROR) {
			si.asc = SAT_ASC_WRITE_ERROR;
			si.ascq = 0x02; /* write error - auto reallocation failed */
		}
	}
	if (has_lba) {
		si.has_info = 1;
		si.info = lba;
	}
	sat_check_info(c, &si);
	c->res->resid = c->len;
}

/* ==========================================================================
 * Command issue
 * ========================================================================== */

/* Is the disk's queue usable for pass-through? */
static int sat_queue_usable(const struct ata_device *dev)
{
	const struct gendisk *d = dev->disk;

	return d && d->magic == GENDISK_MAGIC && d->queue &&
	       (d->flags & GENHD_FL_UP) && !(d->flags & GENHD_FL_DEAD);
}

/* Run a prepared command: through the request queue when the disk is up
 * (ordered with the filesystem's I/O), else directly. */
static int sat_issue_qc(struct ata_device *dev, struct ata_queued_cmd *qc)
{
	struct blk_request *req;
	int ret;

	if (!sat_queue_usable(dev))
		return ata_qc_exec(qc);

	/* Data to the device is a "write" for the block layer (refused on a
	 * read-only disk); reads and non-data commands are not. */
	req = blk_request_alloc(&dev->disk->bdev,
				qc->dma_dir == ATA_DIR_TO_DEVICE
					? BLK_OP_DRV_OUT
					: BLK_OP_DRV_IN,
				0, 0);
	if (!req)
		return -ENOMEM;
	req->drv_cmd = qc;
	req->flags |= BLK_REQ_HEAD | BLK_REQ_NO_ACCOUNT | BLK_REQ_QUIET;
	req->timeout_ms = qc->timeout_ms;
	if (qc->nbytes && qc->sg == &qc->sg_one && qc->sg_one.virt) {
		ret = blk_request_add_buf(req, qc->sg_one.virt, qc->nbytes);
		if (ret) {
			blk_request_free(req);
			return ret;
		}
	}
	ret = blk_execute(req);
	blk_request_free(req);
	if (qc->flags & ATA_QCFLAG_COMPLETE)
		return qc->result;
	if (ret == -EOPNOTSUPP || ret == -EINVAL) {
		/* The disk's submit path does not take pass-through
		 * requests: run it directly. */
		WARN_ON_ONCE(1);
		return ata_qc_exec(qc);
	}
	return ret;
}

/* Execute one ATA command from a task file with data; `rtf' receives the
 * result registers (also on error).  Returns 0 / -errno (-EIO with the
 * error in rtf). */
static int sat_exec_tf(struct ata_device *dev, const struct ata_taskfile *tf,
		       enum ata_prot prot, void *buf, uint32_t len,
		       enum ata_dma_dir dir, uint32_t timeout_ms,
		       struct ata_taskfile *rtf, uint32_t *resid,
		       uint32_t extra_flags)
{
	struct ata_queued_cmd *qc;
	int ret;

	qc = ata_qc_alloc(dev);
	if (!qc)
		return -ENOMEM;
	if (len)
		ata_qc_set_buf(qc, buf, len, dir);
	qc->tf = *tf;
	qc->protocol = prot;
	qc->dma_dir = len ? dir : ATA_DIR_NONE;
	if (dir == ATA_DIR_TO_DEVICE && len)
		qc->flags |= ATA_QCFLAG_WRITE;
	qc->flags |= ATA_QCFLAG_PASSTHRU | ATA_QCFLAG_RESULT_TF |
		     ATA_QCFLAG_QUIET | extra_flags;
	qc->timeout_ms = timeout_ms ? timeout_ms : ATA_TMOUT_INTERNAL_MS;
	qc->max_retries = 0; /* pass-through: the caller decides */
	qc->lba = ata_tf_read_lba(tf);
	qc->n_sect = len / ATA_SECT_SIZE;

	ret = sat_issue_qc(dev, qc);
	if (rtf) {
		*rtf = qc->result_tf;
		rtf->flags = tf->flags;
	}
	if (resid)
		*resid = (qc->flags & ATA_QCFLAG_COMPLETE)
				 ? (qc->resid <= len ? qc->resid : len)
				 : len;
	ata_qc_free(qc);
	return ret;
}

/* ==========================================================================
 * Response builders (pure)
 * ========================================================================== */

int ata_sat_inquiry_std(const struct ata_device *dev, uint8_t *buf,
			uint32_t len)
{
	uint8_t r[SAT_INQ_STD_LEN];
	uint8_t fw[8];

	if (!dev || !buf)
		return -EINVAL;
	kmemset(r, 0, sizeof(r));
	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = (dev->flags & ATA_DFLAG_REMOVABLE) ? 0x80 : 0x00;
	r[2] = 0x06; /* SPC-4 */
	r[3] = 0x02 | 0x10; /* response data format 2, HiSup */
	r[4] = (uint8_t)(SAT_INQ_STD_LEN - 5);
	r[7] = (dev->flags & ATA_DFLAG_NCQ_ON) ? 0x02 : 0x00; /* CmdQue */
	sat_pad_string(r + 8, 8, "ATA");
	sat_id_raw_string(dev->id, ATA_ID_PROD, 16, r + 16);
	/* product revision: the last four characters of the 8-character
	 * firmware revision, or the first four when those are blank */
	sat_id_raw_string(dev->id, ATA_ID_FW_REV, 8, fw);
	if (fw[4] == ' ' && fw[5] == ' ' && fw[6] == ' ' && fw[7] == ' ')
		kmemcpy(r + 32, fw, 4);
	else
		kmemcpy(r + 32, fw + 4, 4);
	/* version descriptors */
	put_be16(r + 58, SAT_VD_SAM5);
	put_be16(r + 60, SAT_VD_SAT3);
	put_be16(r + 62, SAT_VD_SPC4);
	put_be16(r + 64, (dev->class == ATA_DEV_ZAC) ? SAT_VD_ZBC : SAT_VD_SBC3);
	return (int)sat_copy_out(buf, len, r, sizeof(r));
}

/* The VPD pages (pure).  Each builder writes the full page into `r'
 * (sized for the largest) and returns its length. */
static uint32_t sat_vpd_00(const struct ata_device *dev, uint8_t *r)
{
	static const uint8_t pages[] = {
		SAT_VPD_SUPPORTED,   SAT_VPD_SERIAL,	  SAT_VPD_DEVICE_ID,
		SAT_VPD_ATA_INFO,    SAT_VPD_BLOCK_LIMITS, SAT_VPD_BLOCK_CHARS,
		SAT_VPD_LBP,
	};

	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_SUPPORTED;
	put_be16(r + 2, sizeof(pages));
	kmemcpy(r + 4, pages, sizeof(pages));
	return 4 + sizeof(pages);
}

static uint32_t sat_vpd_80(const struct ata_device *dev, uint8_t *r)
{
	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_SERIAL;
	put_be16(r + 2, ATA_ID_SERNO_LEN);
	sat_id_raw_string(dev->id, ATA_ID_SERNO, ATA_ID_SERNO_LEN, r + 4);
	return 4 + ATA_ID_SERNO_LEN;
}

static uint32_t sat_vpd_83(const struct ata_device *dev, uint8_t *r)
{
	uint32_t p = 4;
	uint64_t wwn = sat_wwn(dev);

	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_DEVICE_ID;

	/* 1. vendor specific (type 0): the serial number, ASCII */
	r[p + 0] = 0x02; /* code set ASCII */
	r[p + 1] = 0x00; /* association LU, type vendor specific */
	r[p + 2] = 0;
	r[p + 3] = ATA_ID_SERNO_LEN;
	sat_id_raw_string(dev->id, ATA_ID_SERNO, ATA_ID_SERNO_LEN, r + p + 4);
	p += 4 + ATA_ID_SERNO_LEN;

	/* 2. T10 vendor id (type 1): "ATA     " + model + serial, the
	 * identifier SAT defines for ATA devices */
	r[p + 0] = 0x02;
	r[p + 1] = 0x01;
	r[p + 2] = 0;
	r[p + 3] = 8 + ATA_ID_PROD_LEN + ATA_ID_SERNO_LEN;
	sat_pad_string(r + p + 4, 8, "ATA");
	sat_id_raw_string(dev->id, ATA_ID_PROD, ATA_ID_PROD_LEN, r + p + 12);
	sat_id_raw_string(dev->id, ATA_ID_SERNO, ATA_ID_SERNO_LEN,
			  r + p + 12 + ATA_ID_PROD_LEN);
	p += 4 + 8 + ATA_ID_PROD_LEN + ATA_ID_SERNO_LEN;

	/* 3. NAA (type 3) from the world wide name */
	if (wwn) {
		r[p + 0] = 0x01; /* binary */
		r[p + 1] = 0x03; /* NAA */
		r[p + 2] = 0;
		r[p + 3] = 8;
		put_be64(r + p + 4, wwn);
		p += 12;
	}
	put_be16(r + 2, (uint16_t)(p - 4));
	return p;
}

static uint32_t sat_vpd_89(const struct ata_device *dev, uint8_t *r)
{
	int atapi = dev->class == ATA_DEV_ATAPI;

	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_ATA_INFO;
	put_be16(r + 2, SAT_VPD89_LEN - 4);
	sat_pad_string(r + 8, 8, SAT_VENDOR_ID);
	sat_pad_string(r + 16, 16, SAT_PRODUCT_ID);
	sat_pad_string(r + 32, 4, SAT_PRODUCT_REV);
	/* The device signature as a D2H register FIS, as after a reset. */
	r[36] = FIS_TYPE_REG_D2H;
	r[37] = 0x00;
	r[38] = ATA_DRDY | ATA_DSC; /* status 50h */
	r[39] = 0x01; /* error: diagnostic passed */
	r[40] = 0x01; /* LBA low */
	r[41] = atapi ? 0x14 : 0x00; /* LBA mid */
	r[42] = atapi ? 0xEB : 0x00; /* LBA high */
	r[43] = 0x00; /* device */
	r[48] = 0x01; /* count */
	r[56] = atapi ? ATA_CMD_ID_ATAPI : ATA_CMD_ID_ATA;
	/* the IDENTIFY page, little-endian words as the device sent it */
	for (int i = 0; i < ATA_ID_WORDS; i++) {
		r[60 + 2 * i] = (uint8_t)dev->id[i];
		r[60 + 2 * i + 1] = (uint8_t)(dev->id[i] >> 8);
	}
	return SAT_VPD89_LEN;
}

static uint32_t sat_vpd_b0(const struct ata_device *dev, uint8_t *r)
{
	uint32_t per = 1u << sat_log2_per_phys(dev);
	uint32_t max_xfer = dev->max_sectors;

	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_BLOCK_LIMITS;
	put_be16(r + 2, 0x3C);
	r[4] = 0x01; /* WSNZ: WRITE SAME with 0 blocks is not allowed */
	r[5] = 0; /* COMPARE AND WRITE: not supported */
	put_be16(r + 6, (uint16_t)per); /* optimal transfer granularity */
	if (max_xfer == 0)
		max_xfer = (dev->flags & ATA_DFLAG_LBA48) ? 65535 : 256;
	put_be32(r + 8, max_xfer);
	put_be32(r + 12, 0); /* optimal transfer length: not reported */
	put_be32(r + 16, 0); /* maximum prefetch */
	if (dev->flags & ATA_DFLAG_TRIM) {
		uint32_t blocks = ata_trim_payload_blocks(dev);
		uint64_t max_unmap = (uint64_t)blocks *
				     ATA_TRIM_RANGES_PER_BLOCK *
				     ATA_MAX_TRIM_RANGE;

		if (max_unmap > 0xFFFFFFFFull)
			max_unmap = 0xFFFFFFFFull;
		put_be32(r + 20, (uint32_t)max_unmap);
		put_be32(r + 24, SAT_UNMAP_MAX_DESC);
		put_be32(r + 28, per); /* optimal unmap granularity */
		put_be32(r + 32, 0x80000000u | sat_lowest_aligned(dev));
	}
	/* maximum WRITE SAME length: what one command may cover */
	put_be64(r + 36, (dev->flags & ATA_DFLAG_LBA48) ? 0x3FFFC0ull : 0);
	return 64;
}

static uint32_t sat_vpd_b1(const struct ata_device *dev, uint8_t *r)
{
	uint16_t rot = dev->id[ATA_ID_ROT_SPEED];
	uint16_t ff = dev->id[ATA_ID_FORM_FACTOR];

	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_BLOCK_CHARS;
	put_be16(r + 2, 0x3C);
	/* 0001h non-rotating, 0401h..FFFEh rpm, 0 not reported */
	if (rot == 0xFFFF || (rot > 1 && rot < 0x0401))
		rot = 0;
	put_be16(r + 4, rot);
	r[6] = 0; /* product type: not indicated */
	r[7] = (ff == 0xFFFF) ? 0 : (uint8_t)(ff & 0x0F);
	r[8] = 0; /* ZONED / FUAB / VBULS */
	return 64;
}

static uint32_t sat_vpd_b2(const struct ata_device *dev, uint8_t *r)
{
	r[0] = (dev->class == ATA_DEV_ZAC) ? TYPE_ZBC : TYPE_DISK;
	r[1] = SAT_VPD_LBP;
	put_be16(r + 2, 4);
	r[4] = 0; /* threshold exponent */
	if (dev->flags & ATA_DFLAG_TRIM) {
		r[5] = 0x80 | 0x40; /* LBPU (UNMAP), LBPWS (WRITE SAME 16) */
		if (ata_trim_zeroes_data(dev))
			r[5] |= 0x04; /* LBPRZ = 001b: unmapped reads zero */
		r[6] = 0x02; /* provisioning type: thin */
	}
	return 8;
}

int ata_sat_inquiry_vpd(const struct ata_device *dev, uint8_t page,
			uint8_t *buf, uint32_t len)
{
	uint8_t *r;
	uint32_t n;
	int ret;

	if (!dev || !buf)
		return -EINVAL;
	r = kcalloc(1, SAT_VPD89_LEN);
	if (!r)
		return -ENOMEM;
	switch (page) {
	case SAT_VPD_SUPPORTED:
		n = sat_vpd_00(dev, r);
		break;
	case SAT_VPD_SERIAL:
		n = sat_vpd_80(dev, r);
		break;
	case SAT_VPD_DEVICE_ID:
		n = sat_vpd_83(dev, r);
		break;
	case SAT_VPD_ATA_INFO:
		n = sat_vpd_89(dev, r);
		break;
	case SAT_VPD_BLOCK_LIMITS:
		n = sat_vpd_b0(dev, r);
		break;
	case SAT_VPD_BLOCK_CHARS:
		n = sat_vpd_b1(dev, r);
		break;
	case SAT_VPD_LBP:
		n = sat_vpd_b2(dev, r);
		break;
	default:
		kfree(r);
		return -ENOENT;
	}
	ret = (int)sat_copy_out(buf, len, r, n);
	kfree(r);
	return ret;
}

int ata_sat_read_capacity10(const struct ata_device *dev, uint8_t *buf,
			    uint32_t len)
{
	uint8_t r[8];
	uint64_t last;

	if (!dev || !buf)
		return -EINVAL;
	last = dev->n_sectors ? dev->n_sectors - 1 : 0;
	put_be32(r, last > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)last);
	put_be32(r + 4, sat_lbs(dev));
	return (int)sat_copy_out(buf, len, r, sizeof(r));
}

int ata_sat_read_capacity16(const struct ata_device *dev, uint8_t *buf,
			    uint32_t len)
{
	uint8_t r[32];
	uint16_t lowest;

	if (!dev || !buf)
		return -EINVAL;
	kmemset(r, 0, sizeof(r));
	put_be64(r, dev->n_sectors ? dev->n_sectors - 1 : 0);
	put_be32(r + 8, sat_lbs(dev));
	r[12] = 0; /* P_TYPE / PROT_EN: no protection information */
	r[13] = sat_log2_per_phys(dev); /* P_I_EXPONENT 0 */
	lowest = sat_lowest_aligned(dev);
	r[14] = (uint8_t)((lowest >> 8) & 0x3F);
	r[15] = (uint8_t)lowest;
	if (dev->flags & ATA_DFLAG_TRIM) {
		r[14] |= 0x80; /* LBPME */
		if (ata_trim_zeroes_data(dev))
			r[14] |= 0x40; /* LBPRZ */
	}
	return (int)sat_copy_out(buf, len, r, sizeof(r));
}

/* ==========================================================================
 * ATA PASS-THROUGH decoding (pure)
 * ========================================================================== */

int ata_sat_pt_decode(const uint8_t *cdb, int cdb_len, uint32_t buf_len,
		      uint32_t lbs, struct ata_sat_pt *pt, int *bad_byte,
		      int *bad_bit)
{
	struct ata_taskfile *tf;
	uint32_t units = 0, bsz;
	int bb = -1, bbit = -1;

	if (!cdb || !pt)
		return -EINVAL;
	kmemset(pt, 0, sizeof(*pt));
	tf = &pt->tf;
	if (bad_byte)
		*bad_byte = -1;
	if (bad_bit)
		*bad_bit = -1;

	if (cdb[0] == ATA_16 && cdb_len >= 16)
		pt->cdb_len = 16;
	else if (cdb[0] == ATA_12 && cdb_len >= 12)
		pt->cdb_len = 12;
	else {
		bb = 0;
		goto invalid;
	}

	pt->protocol = (uint8_t)((cdb[1] >> 1) & 0x0F);
	pt->multiple_count = (uint8_t)(cdb[1] >> 5);
	pt->extend = (pt->cdb_len == 16) ? (cdb[1] & 0x01) : 0;
	pt->off_line = (uint8_t)(cdb[2] >> 6);
	pt->ck_cond = (cdb[2] & SAT_CK_COND) ? 1 : 0;
	pt->t_type = (cdb[2] & 0x10) ? 1 : 0;
	pt->t_dir = (cdb[2] & SAT_T_DIR) ? 1 : 0;
	pt->byt_blok = (cdb[2] & SAT_BYT_BLOK) ? 1 : 0;
	pt->t_length = cdb[2] & SAT_T_LEN_MASK;

	if (pt->cdb_len == 16) {
		tf->feature = cdb[4];
		tf->nsect = cdb[6];
		tf->lbal = cdb[8];
		tf->lbam = cdb[10];
		tf->lbah = cdb[12];
		tf->device = cdb[13];
		tf->command = cdb[14];
		if (pt->extend) {
			tf->hob_feature = cdb[3];
			tf->hob_nsect = cdb[5];
			tf->hob_lbal = cdb[7];
			tf->hob_lbam = cdb[9];
			tf->hob_lbah = cdb[11];
			tf->flags |= ATA_TFLAG_LBA48;
		}
	} else {
		tf->feature = cdb[3];
		tf->nsect = cdb[4];
		tf->lbal = cdb[5];
		tf->lbam = cdb[6];
		tf->lbah = cdb[7];
		tf->device = cdb[8];
		tf->command = cdb[9];
	}
	tf->flags |= ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	if (tf->device & ATA_LBA)
		tf->flags |= ATA_TFLAG_LBA;
	/* The PMP/DEV bit of the device register is the translator's (one
	 * device per link): clear DEV so a stray bit cannot address a
	 * non-existent device 1. */
	tf->device &= (uint8_t)~ATA_DEV1;

	/* Transfer length */
	bsz = pt->t_type ? (lbs ? lbs : ATA_SECT_SIZE) : ATA_SECT_SIZE;
	pt->block_size = bsz;
	switch (pt->t_length) {
	case 0:
		units = 0;
		break;
	case 1:
		units = tf->feature |
			(pt->extend ? ((uint32_t)tf->hob_feature << 8) : 0);
		break;
	case 2:
		units = tf->nsect |
			(pt->extend ? ((uint32_t)tf->hob_nsect << 8) : 0);
		break;
	case 3:
		units = 0; /* the buffer's length (TPSIU) */
		break;
	}
	if (pt->t_length == 3) {
		pt->xfer_len = buf_len;
	} else if (pt->t_length == 0) {
		pt->xfer_len = 0;
	} else {
		/* a zero count means 256 (28-bit) / 65536 (48-bit) blocks
		 * for the commands that use the field as a sector count */
		if (units == 0 && pt->byt_blok)
			units = pt->extend ? 65536 : 256;
		pt->xfer_len = pt->byt_blok ? units * bsz : units;
	}

	/* Protocol -> ATA protocol and direction */
	switch (pt->protocol) {
	case SAT_PROT_HARD_RESET:
	case SAT_PROT_SRST:
	case SAT_PROT_DEV_RESET:
	case SAT_PROT_DEV_DIAG:
	case SAT_PROT_RET_RESP:
		pt->prot = ATA_PROT_UNKNOWN;
		pt->dir = ATA_DIR_NONE;
		pt->xfer_len = 0;
		return 0;
	case SAT_PROT_NONDATA:
		/* T_LENGTH is meaningless without a data phase; tools are
		 * not consistent about clearing it, so it is ignored. */
		pt->prot = ATA_PROT_NODATA;
		pt->dir = ATA_DIR_NONE;
		pt->xfer_len = 0;
		return 0;
	case SAT_PROT_PIO_IN:
		pt->prot = ATA_PROT_PIO_IN;
		pt->dir = ATA_DIR_FROM_DEVICE;
		if (!pt->t_dir) {
			bb = 2;
			bbit = 3;
			goto invalid;
		}
		break;
	case SAT_PROT_PIO_OUT:
		pt->prot = ATA_PROT_PIO_OUT;
		pt->dir = ATA_DIR_TO_DEVICE;
		if (pt->t_dir) {
			bb = 2;
			bbit = 3;
			goto invalid;
		}
		break;
	case SAT_PROT_DMA:
	case SAT_PROT_UDMA_IN:
	case SAT_PROT_UDMA_OUT:
		if (pt->protocol == SAT_PROT_UDMA_IN && !pt->t_dir) {
			bb = 2;
			bbit = 3;
			goto invalid;
		}
		if (pt->protocol == SAT_PROT_UDMA_OUT && pt->t_dir) {
			bb = 2;
			bbit = 3;
			goto invalid;
		}
		pt->dir = pt->t_dir ? ATA_DIR_FROM_DEVICE : ATA_DIR_TO_DEVICE;
		pt->prot = pt->t_dir ? ATA_PROT_DMA_IN : ATA_PROT_DMA_OUT;
		break;
	case SAT_PROT_FPDMA:
		pt->dir = pt->t_dir ? ATA_DIR_FROM_DEVICE : ATA_DIR_TO_DEVICE;
		pt->prot = ATA_PROT_FPDMA;
		break;
	case SAT_PROT_DMA_QUEUED:
	default:
		/* legacy TCQ and reserved values */
		bb = 1;
		bbit = 4;
		goto unsupported;
	}

	if (pt->t_length == 0 || pt->xfer_len == 0) {
		/* a data protocol must move data */
		bb = 2;
		bbit = 1;
		goto invalid;
	}
	if (pt->xfer_len > buf_len) {
		/* the CDB asks for more than the buffer holds */
		bb = 2;
		bbit = 1;
		goto invalid;
	}
	if (pt->xfer_len & 1) {
		bb = 2;
		bbit = 1;
		goto invalid;
	}
	if (pt->dir == ATA_DIR_TO_DEVICE)
		tf->flags |= ATA_TFLAG_WRITE;
	return 0;

unsupported:
	if (bad_byte)
		*bad_byte = bb;
	if (bad_bit)
		*bad_bit = bbit;
	return -EOPNOTSUPP;
invalid:
	if (bad_byte)
		*bad_byte = bb;
	if (bad_bit)
		*bad_bit = bbit;
	return -EINVAL;
}

/* ==========================================================================
 * Mode pages (pure)
 * ========================================================================== */

#define MPAGE_RW_RECOVERY 0x01
#define MPAGE_CACHING 0x08
#define MPAGE_CONTROL 0x0A
#define MPAGE_IEC 0x1C
#define MPAGE_ALL 0x3F

#define MPC_CURRENT 0
#define MPC_CHANGEABLE 1
#define MPC_DEFAULT 2
#define MPC_SAVED 3

static uint32_t sat_mpage_rw_recovery(const struct ata_device *dev, int pc,
				      uint8_t *p)
{
	(void)dev;
	kmemset(p, 0, 12);
	p[0] = MPAGE_RW_RECOVERY;
	p[1] = 0x0A;
	if (pc != MPC_CHANGEABLE) {
		p[2] = 0x80 | 0x40; /* AWRE ARRE: the drive reallocates */
		p[3] = 0; /* read retry count */
		p[8] = 0; /* write retry count */
	}
	return 12;
}

static uint32_t sat_mpage_caching(const struct ata_device *dev, int pc,
				  uint8_t *p)
{
	kmemset(p, 0, 20);
	p[0] = MPAGE_CACHING;
	p[1] = 0x12;
	if (pc == MPC_CHANGEABLE) {
		if (dev->flags & ATA_DFLAG_WCACHE)
			p[2] |= 0x04; /* WCE */
		if (dev->flags & ATA_DFLAG_RLA)
			p[12] |= 0x20; /* DRA */
		return 20;
	}
	if (pc == MPC_DEFAULT) {
		/* the drive's defaults: cache on, look-ahead on */
		if (dev->flags & ATA_DFLAG_WCACHE)
			p[2] |= 0x04;
		return 20;
	}
	if (dev->flags & ATA_DFLAG_WCACHE_ON)
		p[2] |= 0x04;
	if ((dev->flags & ATA_DFLAG_RLA) && !(dev->flags & ATA_DFLAG_RLA_ON))
		p[12] |= 0x20;
	return 20;
}

static uint32_t sat_mpage_control(const struct ata_device *dev, int pc,
				  uint8_t *p)
{
	kmemset(p, 0, 12);
	p[0] = MPAGE_CONTROL;
	p[1] = 0x0A;
	if (pc == MPC_CHANGEABLE) {
		p[2] = 0x04; /* D_SENSE */
		return 12;
	}
	p[2] = 0x02; /* GLTSD: log parameters are not saved */
	if (pc == MPC_CURRENT && sat_get_d_sense(dev))
		p[2] |= 0x04;
	p[3] = 0x00; /* QAM 0: restricted reordering */
	put_be16(p + 8, 0xFFFF); /* busy timeout: unlimited */
	put_be16(p + 10, 0); /* extended self-test time: see LOG SENSE */
	return 12;
}

static uint32_t sat_mpage_iec(const struct ata_device *dev, int pc,
			      uint8_t *p)
{
	kmemset(p, 0, 12);
	p[0] = MPAGE_IEC;
	p[1] = 0x0A;
	if (pc == MPC_CHANGEABLE)
		return 12;
	/* DEXCPT when SMART is off; MRIE 6: report on request (the
	 * informational exceptions log page) */
	if (!(dev->flags & ATA_DFLAG_SMART_ON))
		p[2] |= 0x08;
	p[3] = 0x06;
	return 12;
}

/* Append the requested page(s); returns bytes, 0 for an unsupported page */
static uint32_t sat_mode_pages(const struct ata_device *dev, uint8_t page,
			       int pc, uint8_t *p)
{
	uint32_t n = 0;

	switch (page) {
	case MPAGE_RW_RECOVERY:
		return sat_mpage_rw_recovery(dev, pc, p);
	case MPAGE_CACHING:
		return sat_mpage_caching(dev, pc, p);
	case MPAGE_CONTROL:
		return sat_mpage_control(dev, pc, p);
	case MPAGE_IEC:
		return sat_mpage_iec(dev, pc, p);
	case MPAGE_ALL:
		n += sat_mpage_rw_recovery(dev, pc, p + n);
		n += sat_mpage_caching(dev, pc, p + n);
		n += sat_mpage_control(dev, pc, p + n);
		n += sat_mpage_iec(dev, pc, p + n);
		return n;
	default:
		return 0;
	}
}

/* ==========================================================================
 * Device state kept in step with pass-through commands
 * ========================================================================== */

/* The block layer's view of the write cache follows the drive's (a FLUSH
 * that the queue skips because it believes there is no cache would lose
 * data). */
static void sat_sync_write_cache(struct ata_device *dev)
{
	struct gendisk *disk = dev->disk;
	uint8_t on = (dev->flags & ATA_DFLAG_WCACHE_ON) ? 1 : 0;

	if (!sat_queue_usable(dev))
		return;
	disk->limits.write_cache = on;
	if (disk->queue)
		disk->queue->limits.write_cache = on;
	blk_mutex_lock(&g_disk_registry_mutex);
	disk->bdev.flush_supported = on;
	for (int i = 1; i <= disk->nr_parts && i <= DISK_MAX_PARTS; i++)
		if (disk->part[i])
			disk->part[i]->bdev.flush_supported = on;
	blk_mutex_unlock(&g_disk_registry_mutex);
}

static void sat_request_revalidate(struct ata_device *dev)
{
	__atomic_or_fetch(&dev->flags, ATA_DFLAG_NEEDS_REVALIDATE,
			  __ATOMIC_SEQ_CST);
	if (dev->xport && dev->xport->schedule_eh)
		dev->xport->schedule_eh(dev, AHCI_EH_REVALIDATE);
}

/* After a successful pass-through command: update what the driver knows
 * about the device so it does not act on stale state. */
static void sat_post_command(struct ata_device *dev,
			     const struct ata_taskfile *tf)
{
	int wc_changed = 0;

	switch (tf->command) {
	case ATA_CMD_SET_FEATURES:
		ata_dev_lock(dev);
		switch (tf->feature) {
		case SETFEATURES_WC_ON:
			dev->flags |= ATA_DFLAG_WCACHE_ON;
			wc_changed = 1;
			break;
		case SETFEATURES_WC_OFF:
			dev->flags &= ~ATA_DFLAG_WCACHE_ON;
			wc_changed = 1;
			break;
		case SETFEATURES_RLA_ON:
			dev->flags |= ATA_DFLAG_RLA_ON;
			break;
		case SETFEATURES_RLA_OFF:
			dev->flags &= ~ATA_DFLAG_RLA_ON;
			break;
		case SETFEATURES_APM_ON:
			dev->apm_level = tf->nsect;
			break;
		case SETFEATURES_APM_OFF:
			dev->apm_level = 0;
			break;
		case SETFEATURES_AAM_ON:
			dev->aam_level = tf->nsect;
			break;
		case SETFEATURES_AAM_OFF:
			dev->aam_level = 0;
			break;
		case SETFEATURES_PUIS_ON:
			dev->flags |= ATA_DFLAG_PUIS;
			break;
		case SETFEATURES_PUIS_OFF:
			dev->flags &= ~ATA_DFLAG_PUIS;
			break;
		case SETFEATURES_SATA_ENABLE:
			if (tf->nsect == SATA_FEAT_DIPM)
				dev->flags |= ATA_DFLAG_DIPM_ON;
			break;
		case SETFEATURES_SATA_DISABLE:
			if (tf->nsect == SATA_FEAT_DIPM)
				dev->flags &= ~ATA_DFLAG_DIPM_ON;
			break;
		default:
			break;
		}
		ata_dev_unlock(dev);
		if (wc_changed) {
			kprintf(SAT_PFX "%s: write cache %s by pass-through\n",
				dev->name,
				(dev->flags & ATA_DFLAG_WCACHE_ON) ? "enabled"
								   : "disabled");
			sat_sync_write_cache(dev);
		}
		break;
	case ATA_CMD_SMART:
		if (tf->feature == ATA_SMART_ENABLE)
			__atomic_or_fetch(&dev->flags, ATA_DFLAG_SMART_ON,
					  __ATOMIC_SEQ_CST);
		else if (tf->feature == ATA_SMART_DISABLE)
			__atomic_and_fetch(&dev->flags, ~ATA_DFLAG_SMART_ON,
					   __ATOMIC_SEQ_CST);
		break;
	case ATA_CMD_SET_MULTI:
		ata_dev_lock(dev);
		dev->multi_count = tf->nsect;
		ata_dev_unlock(dev);
		break;
	case ATA_CMD_STANDBYNOW1:
	case ATA_CMD_STANDBY:
		dev->power_state = ATA_PWR_STANDBY;
		break;
	case ATA_CMD_IDLEIMMEDIATE:
	case ATA_CMD_IDLE:
		dev->power_state = ATA_PWR_IDLE;
		break;
	case ATA_CMD_SLEEP:
		/* Only a reset wakes the drive now: the core must know, or
		 * the next request hangs until it times out. */
		dev->power_state = ATA_PWR_SLEEP;
		__atomic_or_fetch(&dev->flags, ATA_DFLAG_SLEEPING,
				  __ATOMIC_SEQ_CST);
		kprintf(SAT_PFX "%s: put to SLEEP by pass-through; the next "
				"access resets the link\n",
			dev->name);
		break;
	case ATA_CMD_SEC_SET_PASS:
	case ATA_CMD_SEC_UNLOCK:
	case ATA_CMD_SEC_ERASE_UNIT:
	case ATA_CMD_SEC_FREEZE_LOCK:
	case ATA_CMD_SEC_DISABLE_PASS:
	case ATA_CMD_SET_MAX:
	case ATA_CMD_SET_MAX_EXT:
	case ATA_CMD_CONF_OVERLAY:
	case ATA_CMD_SANITIZE_DEVICE:
	case ATA_CMD_ACCESSIBLE_MAX:
	case ATA_CMD_SET_SECTOR_CONFIG:
	case ATA_CMD_DOWNLOAD_MICRO:
	case ATA_CMD_DOWNLOAD_MICRO_DMA:
		/* capacity, security state, identity or firmware may have
		 * changed: let error handling re-read and re-check the device */
		sat_request_revalidate(dev);
		break;
	default:
		break;
	}
}

/* Commands the translator refuses to pass through because they would pull
 * the device out from under the driver. */
static int sat_pt_refused(const struct ata_device *dev,
			  const struct ata_taskfile *tf, int *byte)
{
	(void)dev;
	/* The transfer mode is the host controller's business: a different
	 * mode than the one configured breaks the link's data transfers. */
	if (tf->command == ATA_CMD_SET_FEATURES &&
	    tf->feature == SETFEATURES_XFER) {
		*byte = 4;
		return 1;
	}
	return 0;
}

/* ==========================================================================
 * The individual translations
 * ========================================================================== */

static void sat_inquiry(struct sat_cmd *c)
{
	const uint8_t *cdb = c->cdb;
	uint32_t alloc = get_be16(cdb + 3);
	uint32_t room = sat_min32(alloc, c->len);
	int n;

	if (cdb[1] & 0x02) { /* CMDDT (obsolete) */
		sat_invalid_field(c, 1, 1);
		return;
	}
	if (!(cdb[1] & 0x01) && cdb[2]) {
		sat_invalid_field(c, 2, -1);
		return;
	}
	if (room == 0 || !c->buf) {
		sat_good(c, 0);
		return;
	}
	if (cdb[1] & 0x01) {
		n = ata_sat_inquiry_vpd(c->dev, cdb[2], c->buf, room);
		if (n == -ENOENT) {
			sat_invalid_field(c, 2, -1);
			return;
		}
	} else {
		n = ata_sat_inquiry_std(c->dev, c->buf, room);
	}
	if (n < 0) {
		sat_host_error(c, n);
		return;
	}
	sat_good(c, (uint32_t)n);
}

static void sat_read_capacity10(struct sat_cmd *c)
{
	int n;

	/* PMI clear: the LBA field must be zero */
	if (!(c->cdb[8] & 0x01) && get_be32(c->cdb + 2)) {
		sat_invalid_field(c, 2, -1);
		return;
	}
	if (!c->buf || c->len == 0) {
		sat_good(c, 0);
		return;
	}
	n = ata_sat_read_capacity10(c->dev, c->buf, c->len);
	if (n < 0) {
		sat_host_error(c, n);
		return;
	}
	sat_good(c, (uint32_t)n);
}

static void sat_get_lba_status(struct sat_cmd *c)
{
	uint64_t lba = get_be64(c->cdb + 2);
	uint32_t alloc = get_be32(c->cdb + 10);
	uint8_t r[24];
	uint64_t left;

	if (lba >= c->dev->n_sectors) {
		sat_lba_out_of_range(c);
		return;
	}
	if (alloc < 24) {
		/* room for the header only, or less */
		kmemset(r, 0, sizeof(r));
		put_be32(r, 20);
		sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), r,
					 8));
		return;
	}
	kmemset(r, 0, sizeof(r));
	put_be32(r, 20); /* parameter data length */
	put_be64(r + 8, lba);
	left = c->dev->n_sectors - lba;
	put_be32(r + 16, left > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)left);
	/* provisioning status 0: mapped (or unknown) -- ATA has no way to
	 * ask which sectors are deallocated */
	r[20] = 0;
	sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), r,
				 sizeof(r)));
}

static void sat_service_action_in16(struct sat_cmd *c)
{
	uint8_t sa = c->cdb[1] & 0x1F;
	uint32_t alloc = get_be32(c->cdb + 10);
	int n;

	switch (sa) {
	case SAI_READ_CAPACITY_16:
		if (!c->buf || alloc == 0 || c->len == 0) {
			sat_good(c, 0);
			return;
		}
		n = ata_sat_read_capacity16(c->dev, c->buf,
					    sat_min32(alloc, c->len));
		if (n < 0) {
			sat_host_error(c, n);
			return;
		}
		sat_good(c, (uint32_t)n);
		return;
	case SAI_GET_LBA_STATUS:
		if (!(c->dev->flags & ATA_DFLAG_TRIM)) {
			sat_invalid_field(c, 1, 4);
			return;
		}
		sat_get_lba_status(c);
		return;
	default:
		sat_invalid_field(c, 1, 4);
		return;
	}
}

static void sat_request_sense(struct sat_cmd *c)
{
	uint8_t s[ATA_SENSE_LEN];
	int desc = c->cdb[1] & 0x01;
	uint32_t alloc = c->cdb[4];
	uint32_t n;
	struct sat_sense_info si;

	{
		kmemset(&si, 0, sizeof(si));
		if (sat_request_sense_data_ext(c->dev, &si.key, &si.asc,
					       &si.ascq) != 0) {
			uint8_t mode = 0xFF;

			/* No sense available: report the power condition
			 * the way SAT does (standby -> 5Eh/42h... is only
			 * meaningful with a timer; NO SENSE otherwise). */
			si.key = NO_SENSE;
			if (c->dev->power_state == ATA_PWR_STANDBY &&
			    ata_check_power_mode(c->dev, &mode) == 0 &&
			    mode == ATA_CHK_PWR_STANDBY) {
				si.asc = 0x5E; /* low power condition on */
				si.ascq = 0x43; /* standby condition by command */
			}
		}
		n = sat_sense_build(s, sizeof(s), desc, &si);
	}
	sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), s, n));
}

static void sat_test_unit_ready(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;

	if (dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED)) {
		sat_check(c, NOT_READY, SAT_ASC_LOGICAL_UNIT_NOT_READY, 0x00);
		return;
	}
	if (dev->security.locked) {
		/* logical unit access not authorized */
		sat_check(c, DATA_PROTECT, 0x74, 0x71);
		return;
	}
	if (dev->flags & ATA_DFLAG_NEEDS_REVALIDATE) {
		/* becoming ready: error handling revalidates it */
		sat_check(c, NOT_READY, SAT_ASC_LOGICAL_UNIT_NOT_READY, 0x01);
		return;
	}
	sat_good(c, 0);
}

/* ---- READ / WRITE --------------------------------------------------------- */

struct sat_rw {
	uint64_t lba;
	uint32_t count;
	int write, fua;
	int len_byte; /* CDB byte of the transfer length (for sense) */
};

static int sat_rw_decode(const uint8_t *cdb, struct sat_rw *rw)
{
	kmemset(rw, 0, sizeof(*rw));
	switch (cdb[0]) {
	case WRITE_6:
		rw->write = 1;
		/* fall through */
	case READ_6:
		rw->lba = ((uint64_t)(cdb[1] & 0x1F) << 16) | get_be16(cdb + 2);
		rw->count = cdb[4] ? cdb[4] : 256;
		rw->len_byte = 4;
		return 0;
	case WRITE_10:
		rw->write = 1;
		/* fall through */
	case READ_10:
		rw->lba = get_be32(cdb + 2);
		rw->count = get_be16(cdb + 7);
		rw->fua = (cdb[1] & 0x08) ? 1 : 0;
		rw->len_byte = 7;
		return (cdb[1] & 0xE0) ? 1 : 0; /* RD/WRPROTECT */
	case WRITE_12:
		rw->write = 1;
		/* fall through */
	case READ_12:
		rw->lba = get_be32(cdb + 2);
		rw->count = get_be32(cdb + 6);
		rw->fua = (cdb[1] & 0x08) ? 1 : 0;
		rw->len_byte = 6;
		return (cdb[1] & 0xE0) ? 1 : 0;
	case WRITE_16:
		rw->write = 1;
		/* fall through */
	case READ_16:
		rw->lba = get_be64(cdb + 2);
		rw->count = get_be32(cdb + 10);
		rw->fua = (cdb[1] & 0x08) ? 1 : 0;
		rw->len_byte = 10;
		return (cdb[1] & 0xE0) ? 1 : 0;
	default:
		return -1;
	}
}

/* One chunk of sector I/O, through the queue when the disk is live. */
static int sat_rw_chunk(struct ata_device *dev, uint64_t lba, uint32_t count,
			uint8_t *buf, int write, int fua)
{
	int ret;

	if (sat_queue_usable(dev)) {
		block_device_t *bdev = &dev->disk->bdev;

		if (write)
			return blk_write_sync(bdev, lba, count, buf,
					      fua ? BLK_REQ_FUA : 0);
		return blk_read_sync(bdev, lba, count, buf);
	}
	if (!write)
		return ata_read_sectors(dev, lba, count, buf);
	ret = ata_write_sectors(dev, lba, count, buf);
	if (ret == 0 && fua && (dev->flags & ATA_DFLAG_WCACHE_ON))
		ret = ata_flush_cache(dev);
	return ret;
}

static void sat_rw_error(struct sat_cmd *c, int ret, int write, uint64_t lba,
			 uint32_t done_bytes)
{
	struct sat_sense_info si;

	if (ret != -EIO) {
		if (ret == -EINVAL || ret == -ERANGE)
			sat_lba_out_of_range(c);
		else
			sat_host_error(c, ret);
		c->res->resid = c->len - done_bytes;
		return;
	}
	kmemset(&si, 0, sizeof(si));
	si.key = MEDIUM_ERROR;
	si.asc = write ? SAT_ASC_WRITE_ERROR : SAT_ASC_UNRECOVERED_READ;
	si.ascq = write ? 0x02 : 0x04;
	si.has_info = 1;
	si.info = lba;
	sat_check_info(c, &si);
	c->res->resid = c->len - done_bytes;
}

static void sat_read_write(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	struct sat_rw rw;
	uint32_t lbs = sat_lbs(dev);
	uint64_t need;
	uint32_t done = 0;
	int ret;

	ret = sat_rw_decode(c->cdb, &rw);
	if (ret < 0) {
		sat_invalid_opcode(c);
		return;
	}
	if (ret > 0) {
		sat_invalid_field(c, 1, 7); /* no protection information */
		return;
	}
	if (rw.count == 0) {
		sat_good(c, 0);
		return;
	}
	if (rw.lba >= dev->n_sectors || rw.count > dev->n_sectors - rw.lba) {
		sat_lba_out_of_range(c);
		return;
	}
	need = (uint64_t)rw.count * lbs;
	if (need > c->len || !c->buf) {
		sat_invalid_field(c, rw.len_byte, -1);
		return;
	}
	if ((rw.write && c->dir != ATA_DIR_TO_DEVICE) ||
	    (!rw.write && c->dir != ATA_DIR_FROM_DEVICE)) {
		sat_invalid_field(c, 0, -1);
		return;
	}
	if (rw.write && sat_is_ro(dev)) {
		sat_check(c, DATA_PROTECT, SAT_ASC_WRITE_PROTECTED, 0x00);
		return;
	}
	if (dev->security.locked) {
		sat_check(c, DATA_PROTECT, 0x74, 0x71);
		return;
	}

	while (done < rw.count) {
		uint32_t chunk = rw.count - done;

		if (chunk > SAT_RW_CHUNK)
			chunk = SAT_RW_CHUNK;
		ret = sat_rw_chunk(dev, rw.lba + done, chunk,
				   c->buf + (uint64_t)done * lbs, rw.write,
				   rw.fua);
		if (ret) {
			sat_rw_error(c, ret, rw.write, rw.lba + done,
				     done * lbs);
			return;
		}
		done += chunk;
	}
	sat_good(c, (uint32_t)need);
}

/* ---- VERIFY ------------------------------------------------------------------ */

static void sat_verify(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	const uint8_t *cdb = c->cdb;
	uint32_t lbs = sat_lbs(dev);
	int bytchk = (cdb[1] >> 1) & 0x03;
	uint64_t lba;
	uint32_t count, done = 0;
	int lba48 = (dev->flags & ATA_DFLAG_LBA48) ? 1 : 0;
	int ret;

	switch (cdb[0]) {
	case VERIFY:
		lba = get_be32(cdb + 2);
		count = get_be16(cdb + 7);
		break;
	case VERIFY_12:
		lba = get_be32(cdb + 2);
		count = get_be32(cdb + 6);
		break;
	default:
		lba = get_be64(cdb + 2);
		count = get_be32(cdb + 10);
		break;
	}
	if (cdb[1] & 0xE0) {
		sat_invalid_field(c, 1, 7);
		return;
	}
	if (bytchk == 2) {
		sat_invalid_field(c, 1, 2);
		return;
	}
	if (count == 0) {
		sat_good(c, 0);
		return;
	}
	if (lba >= dev->n_sectors || count > dev->n_sectors - lba) {
		sat_lba_out_of_range(c);
		return;
	}

	if (bytchk == 0) {
		/* READ VERIFY SECTORS (EXT): the drive reads and checks the
		 * media without transferring anything */
		uint32_t max = lba48 ? 65536 : 256;

		while (done < count) {
			struct ata_taskfile tf, rtf;
			uint32_t chunk = count - done;

			if (chunk > max)
				chunk = max;
			kmemset(&tf, 0, sizeof(tf));
			if (lba48) {
				tf.command = ATA_CMD_VERIFY_EXT;
				tf.hob_nsect = (uint8_t)((chunk >> 8) & 0xFF);
			} else {
				tf.command = ATA_CMD_VERIFY;
			}
			tf.nsect = (uint8_t)(chunk & 0xFF);
			ata_tf_set_lba(&tf, lba + done, lba48);
			ret = sat_exec_tf(dev, &tf, ATA_PROT_NODATA, NULL, 0,
					  ATA_DIR_NONE, c->timeout_ms, &rtf,
					  NULL, 0);
			if (ret == -EIO) {
				rtf.flags = tf.flags;
				sat_ata_error(c, &rtf, 0, 1,
					      ata_tf_read_lba(&rtf));
				return;
			}
			if (ret) {
				sat_host_error(c, ret);
				return;
			}
			done += chunk;
		}
		sat_good(c, 0);
		return;
	}

	/* BYTCHK 01b: compare count blocks of data-out with the media;
	 * BYTCHK 11b: compare one block of data-out with every block. */
	{
		uint64_t need = bytchk == 1 ? (uint64_t)count * lbs : lbs;
		uint32_t step = 256;
		uint8_t *tmp;

		if (need > c->len || !c->buf || c->dir != ATA_DIR_TO_DEVICE) {
			sat_invalid_field(c, bytchk == 1 ? 7 : 1, -1);
			return;
		}
		tmp = kalloc((size_t)step * lbs);
		if (!tmp) {
			sat_host_error(c, -ENOMEM);
			return;
		}
		while (done < count) {
			uint32_t chunk = count - done;

			if (chunk > step)
				chunk = step;
			ret = sat_rw_chunk(dev, lba + done, chunk, tmp, 0, 0);
			if (ret) {
				kfree(tmp);
				sat_rw_error(c, ret, 0, lba + done, 0);
				c->res->resid = c->len;
				return;
			}
			for (uint32_t b = 0; b < chunk; b++) {
				const uint8_t *ref =
					bytchk == 1
						? c->buf + ((uint64_t)done + b) *
								   lbs
						: c->buf;

				if (kmemcmp(tmp + (uint64_t)b * lbs, ref, lbs)) {
					struct sat_sense_info si;

					kfree(tmp);
					kmemset(&si, 0, sizeof(si));
					si.key = MISCOMPARE;
					si.asc = 0x1D; /* miscompare during
							* verify operation */
					si.has_info = 1;
					si.info = lba + done + b;
					sat_check_info(c, &si);
					return;
				}
			}
			done += chunk;
		}
		kfree(tmp);
		sat_good(c, (uint32_t)need);
	}
}

/* ---- SYNCHRONIZE CACHE ---------------------------------------------------- */

static void sat_sync_cache(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	int ret;

	if (!(dev->flags & ATA_DFLAG_WCACHE_ON)) {
		/* nothing volatile to write back */
		sat_good(c, 0);
		return;
	}
	if (sat_queue_usable(dev))
		ret = blk_flush_sync(&dev->disk->bdev);
	else
		ret = ata_flush_cache(dev);
	if (ret == -EIO) {
		sat_check(c, MEDIUM_ERROR, SAT_ASC_WRITE_ERROR, 0x00);
		return;
	}
	if (ret) {
		sat_host_error(c, ret);
		return;
	}
	sat_good(c, 0);
}

/* ---- UNMAP / WRITE SAME ------------------------------------------------------ */

static int sat_discard(struct ata_device *dev, uint64_t lba, uint64_t count)
{
	int ret;

	if (sat_queue_usable(dev)) {
		ret = blk_discard_sync(&dev->disk->bdev, lba, count, 0);
		if (ret != -EOPNOTSUPP)
			return ret;
	}
	return ata_trim(dev, lba, count);
}

static void sat_unmap(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	const uint8_t *p = c->buf;
	uint32_t plen = get_be16(c->cdb + 7);
	uint32_t bdlen, n;
	int ret;

	if (!(dev->flags & ATA_DFLAG_TRIM)) {
		sat_invalid_opcode(c);
		return;
	}
	if (c->cdb[1] & 0x01) { /* ANCHOR */
		sat_invalid_field(c, 1, 0);
		return;
	}
	if (plen == 0) {
		sat_good(c, 0);
		return;
	}
	if (plen < 8 || plen > SAT_UNMAP_MAX_PARAM) {
		sat_invalid_field(c, 7, -1);
		return;
	}
	if (!p || c->len < plen || c->dir != ATA_DIR_TO_DEVICE) {
		sat_invalid_field(c, 7, -1);
		return;
	}
	bdlen = get_be16(p + 2);
	if (get_be16(p) + 2u < 8u + bdlen || bdlen > plen - 8 ||
	    (bdlen % 16) != 0) {
		sat_invalid_param(c, 2, -1);
		return;
	}
	if (sat_is_ro(dev)) {
		sat_check(c, DATA_PROTECT, SAT_ASC_WRITE_PROTECTED, 0x00);
		return;
	}
	n = bdlen / 16;
	/* validate every descriptor before trimming any */
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *d = p + 8 + i * 16;
		uint64_t lba = get_be64(d);
		uint32_t cnt = get_be32(d + 8);

		if (cnt == 0)
			continue;
		if (lba >= dev->n_sectors || cnt > dev->n_sectors - lba) {
			sat_lba_out_of_range(c);
			return;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *d = p + 8 + i * 16;
		uint64_t lba = get_be64(d);
		uint32_t cnt = get_be32(d + 8);

		if (cnt == 0)
			continue;
		ret = sat_discard(dev, lba, cnt);
		if (ret == -EIO) {
			sat_check(c, MEDIUM_ERROR, SAT_ASC_WRITE_ERROR, 0x00);
			return;
		}
		if (ret) {
			sat_host_error(c, ret);
			return;
		}
	}
	sat_good(c, plen);
}

static void sat_write_same(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	const uint8_t *cdb = c->cdb;
	uint32_t lbs = sat_lbs(dev);
	int is16 = cdb[0] == WRITE_SAME_16;
	int unmap = (cdb[1] & 0x08) ? 1 : 0;
	int anchor = (cdb[1] & 0x10) ? 1 : 0;
	int ndob = is16 && (cdb[1] & 0x01);
	uint64_t lba = is16 ? get_be64(cdb + 2) : get_be32(cdb + 2);
	uint32_t count = is16 ? get_be32(cdb + 10) : get_be16(cdb + 7);
	int zero = 1;
	uint8_t *pat;
	uint32_t per, done = 0;
	int ret;

	if (anchor) {
		sat_invalid_field(c, 1, 4);
		return;
	}
	if (cdb[1] & 0xE0) {
		sat_invalid_field(c, 1, 7);
		return;
	}
	if (count == 0) {
		sat_invalid_field(c, is16 ? 10 : 7, -1); /* WSNZ */
		return;
	}
	if (lba >= dev->n_sectors || count > dev->n_sectors - lba) {
		sat_lba_out_of_range(c);
		return;
	}
	if (!ndob) {
		if (!c->buf || c->len < lbs || c->dir != ATA_DIR_TO_DEVICE) {
			sat_invalid_field(c, 0, -1);
			return;
		}
		for (uint32_t i = 0; i < lbs; i++)
			if (c->buf[i]) {
				zero = 0;
				break;
			}
	}
	if (sat_is_ro(dev)) {
		sat_check(c, DATA_PROTECT, SAT_ASC_WRITE_PROTECTED, 0x00);
		return;
	}

	/* UNMAP with zero data on a drive whose trimmed sectors read back
	 * zero: deallocate.  Otherwise the blocks are written. */
	if (unmap && zero && (dev->flags & ATA_DFLAG_TRIM) &&
	    ata_trim_zeroes_data(dev)) {
		ret = sat_discard(dev, lba, count);
		if (ret == 0) {
			sat_good(c, ndob ? 0 : lbs);
			return;
		}
		if (ret != -EOPNOTSUPP) {
			sat_rw_error(c, ret, 1, lba, 0);
			return;
		}
	}
	if (zero && sat_queue_usable(dev)) {
		ret = blk_zeroout_sync(&dev->disk->bdev, lba, count,
				       unmap ? 0 : BLK_REQ_NOUNMAP);
		if (ret == 0) {
			sat_good(c, ndob ? 0 : lbs);
			return;
		}
		if (ret != -EOPNOTSUPP) {
			sat_rw_error(c, ret, 1, lba, 0);
			return;
		}
	}

	/* write the pattern block by block, many per command */
	per = SAT_WSAME_BUF / lbs;
	if (per == 0)
		per = 1;
	pat = kalloc((size_t)per * lbs);
	if (!pat) {
		sat_host_error(c, -ENOMEM);
		return;
	}
	for (uint32_t b = 0; b < per; b++) {
		if (zero)
			kmemset(pat + (uint64_t)b * lbs, 0, lbs);
		else
			kmemcpy(pat + (uint64_t)b * lbs, c->buf, lbs);
	}
	while (done < count) {
		uint32_t chunk = count - done;

		if (chunk > per)
			chunk = per;
		ret = sat_rw_chunk(dev, lba + done, chunk, pat, 1, 0);
		if (ret) {
			kfree(pat);
			sat_rw_error(c, ret, 1, lba + done, 0);
			return;
		}
		done += chunk;
	}
	kfree(pat);
	sat_good(c, ndob ? 0 : lbs);
}

/* ---- START STOP UNIT ---------------------------------------------------------- */

static void sat_start_stop(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	uint8_t pc = c->cdb[4] >> 4;
	int loej = (c->cdb[4] & 0x02) ? 1 : 0;
	int start = (c->cdb[4] & 0x01) ? 1 : 0;
	int ret = 0;

	switch (pc) {
	case 0x0: /* START_VALID: the START and LOEJ bits */
		if (start) {
			ret = ata_dev_wake(dev);
		} else {
			if (dev->flags & ATA_DFLAG_WCACHE_ON)
				ret = ata_flush_cache(dev);
			if (ret == 0)
				ret = ata_standby_immediate(dev);
		}
		if (ret == 0 && loej && (dev->flags & ATA_DFLAG_REMOVABLE) &&
		    !start) {
			struct ata_taskfile tf, rtf;

			kmemset(&tf, 0, sizeof(tf));
			tf.command = ATA_CMD_MEDIA_EJECT;
			tf.flags = ATA_TFLAG_DEVICE;
			ret = sat_exec_tf(dev, &tf, ATA_PROT_NODATA, NULL, 0,
					  ATA_DIR_NONE, c->timeout_ms, &rtf,
					  NULL, 0);
		}
		break;
	case 0x1: /* ACTIVE */
		ret = ata_dev_wake(dev);
		break;
	case 0x2: /* IDLE */
	case 0xA: /* FORCE_IDLE_0 */
		ret = ata_idle_immediate(dev, 0);
		break;
	case 0x3: /* STANDBY */
	case 0xB: /* FORCE_STANDBY_0 */
		if (dev->flags & ATA_DFLAG_WCACHE_ON)
			ret = ata_flush_cache(dev);
		if (ret == 0)
			ret = ata_standby_immediate(dev);
		break;
	case 0x7: /* LU_CONTROL: return control to the device's timers */
		ret = 0;
		break;
	default:
		sat_invalid_field(c, 4, 7);
		return;
	}
	if (ret == -EIO) {
		sat_check(c, ABORTED_COMMAND, 0x00, 0x00);
		return;
	}
	if (ret) {
		sat_host_error(c, ret);
		return;
	}
	sat_good(c, 0);
}

/* ---- MODE SENSE / MODE SELECT ------------------------------------------------ */

static void sat_mode_sense(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	const uint8_t *cdb = c->cdb;
	int is10 = cdb[0] == MODE_SENSE_10;
	int dbd = (cdb[1] & 0x08) ? 1 : 0;
	int llbaa = is10 && (cdb[1] & 0x10);
	int pc = cdb[2] >> 6;
	uint8_t page = cdb[2] & 0x3F;
	uint8_t subpage = cdb[3];
	uint32_t alloc = is10 ? get_be16(cdb + 7) : cdb[4];
	uint8_t r[256];
	uint32_t hdr = is10 ? 8 : 4;
	uint32_t bdl = 0, plen, total;
	uint8_t devspec = 0;

	if (pc == MPC_SAVED) {
		sat_check(c, ILLEGAL_REQUEST, 0x39, 0x00); /* saving params
							    * not supported */
		return;
	}
	if (subpage != 0 && !(page == MPAGE_ALL && subpage == 0xFF)) {
		sat_invalid_field(c, 3, -1);
		return;
	}
	kmemset(r, 0, sizeof(r));
	if (!dbd)
		bdl = llbaa ? 16 : 8;
	plen = sat_mode_pages(dev, page, pc, r + hdr + bdl);
	if (plen == 0) {
		sat_invalid_field(c, 2, 5);
		return;
	}
	if (sat_is_ro(dev))
		devspec |= 0x80; /* WP */
	if (dev->flags & ATA_DFLAG_FUA)
		devspec |= 0x10; /* DPOFUA */

	if (bdl == 8) {
		uint8_t *bd = r + hdr;
		uint64_t n = dev->n_sectors;

		put_be32(bd, n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n);
		bd[4] = 0; /* density code */
		bd[5] = (uint8_t)(sat_lbs(dev) >> 16);
		bd[6] = (uint8_t)(sat_lbs(dev) >> 8);
		bd[7] = (uint8_t)sat_lbs(dev);
	} else if (bdl == 16) {
		uint8_t *bd = r + hdr;

		put_be64(bd, dev->n_sectors);
		put_be32(bd + 12, sat_lbs(dev));
	}
	total = hdr + bdl + plen;
	if (is10) {
		put_be16(r, (uint16_t)(total - 2));
		r[2] = 0; /* medium type */
		r[3] = devspec;
		r[4] = llbaa ? 0x01 : 0x00; /* LONGLBA */
		put_be16(r + 6, (uint16_t)bdl);
	} else {
		r[0] = (uint8_t)(total - 1);
		r[1] = 0;
		r[2] = devspec;
		r[3] = (uint8_t)bdl;
	}
	sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), r, total));
}

static void sat_mode_select(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	const uint8_t *cdb = c->cdb;
	int is10 = cdb[0] == MODE_SELECT_10;
	uint32_t plen = is10 ? get_be16(cdb + 7) : cdb[4];
	uint32_t hdr = is10 ? 8 : 4;
	const uint8_t *p = c->buf;
	uint32_t bdl, off;
	int ret;

	if (!(cdb[1] & 0x10)) { /* PF */
		sat_invalid_field(c, 1, 4);
		return;
	}
	if (cdb[1] & 0x01) { /* SP: nothing can be saved */
		sat_invalid_field(c, 1, 0);
		return;
	}
	if (plen == 0) {
		sat_good(c, 0);
		return;
	}
	if (!p || c->len < plen || c->dir != ATA_DIR_TO_DEVICE ||
	    plen < hdr) {
		sat_invalid_field(c, is10 ? 7 : 4, -1);
		return;
	}
	bdl = is10 ? get_be16(p + 6) : p[3];
	if (hdr + bdl > plen) {
		sat_invalid_param(c, is10 ? 6 : 3, -1);
		return;
	}
	if (bdl >= 8) {
		/* the block descriptor may only restate the block size */
		uint32_t bl = (bdl >= 16 && is10 && (p[4] & 0x01))
				      ? get_be32(p + hdr + 12)
				      : get_be24(p + hdr + 5);

		if (bl != 0 && bl != sat_lbs(dev)) {
			sat_invalid_param(c, (int)(hdr + 5), -1);
			return;
		}
	}

	off = hdr + bdl;
	while (off + 2 <= plen) {
		uint8_t pg = p[off] & 0x3F;
		uint32_t pl = p[off + 1];
		uint8_t cur[32], chg[32];
		uint32_t n;

		if (p[off] & 0x40) { /* SPF: no subpages here */
			sat_invalid_param(c, (int)off, 6);
			return;
		}
		if (off + 2 + pl > plen) {
			sat_invalid_param(c, (int)off + 1, -1);
			return;
		}
		n = 0;
		if (pg == MPAGE_CACHING || pg == MPAGE_CONTROL) {
			n = sat_mode_pages(dev, pg, MPC_CURRENT, cur);
			sat_mode_pages(dev, pg, MPC_CHANGEABLE, chg);
		}
		if (n == 0) {
			sat_invalid_param(c, (int)off, 5);
			return;
		}
		if (pl != n - 2) {
			sat_invalid_param(c, (int)off + 1, -1);
			return;
		}
		/* every bit outside the changeable mask must be unchanged */
		for (uint32_t i = 2; i < n; i++) {
			if ((p[off + i] ^ cur[i]) & (uint8_t)~chg[i]) {
				sat_invalid_param(c, (int)(off + i), -1);
				return;
			}
		}
		if (pg == MPAGE_CACHING) {
			int wce = (p[off + 2] & 0x04) ? 1 : 0;
			int dra = (p[off + 12] & 0x20) ? 1 : 0;
			int cur_wce = (cur[2] & 0x04) ? 1 : 0;
			int cur_dra = (cur[12] & 0x20) ? 1 : 0;

			if (wce != cur_wce) {
				ret = ata_set_write_cache(dev, wce);
				if (ret) {
					sat_host_error(c, ret == -EIO
								  ? -EINVAL
								  : ret);
					return;
				}
				sat_sync_write_cache(dev);
			}
			if (dra != cur_dra) {
				ret = ata_set_read_lookahead(dev, !dra);
				if (ret) {
					sat_host_error(c, ret == -EIO
								  ? -EINVAL
								  : ret);
					return;
				}
			}
		} else if (pg == MPAGE_CONTROL) {
			sat_set_d_sense(dev, (p[off + 2] & 0x04) ? 1 : 0);
		}
		off += 2 + pl;
	}
	sat_good(c, plen);
}

/* ---- LOG SENSE ---------------------------------------------------------------- */

#define LPAGE_SUPPORTED 0x00
#define LPAGE_TEMPERATURE 0x0D
#define LPAGE_SELFTEST 0x10
#define LPAGE_IE 0x2F

/* Temperature from SMART attribute 194 (or 190); 0xFF when unknown. */
static uint8_t sat_temperature(struct ata_device *dev)
{
	uint8_t *d;
	uint8_t t = 0xFF;

	if (!(dev->flags & ATA_DFLAG_SMART_ON))
		return t;
	d = kalloc(ATA_SECT_SIZE);
	if (!d)
		return t;
	if (ata_smart_read_data(dev, d) == 0) {
		for (int pass = 0; pass < 2 && t == 0xFF; pass++) {
			uint8_t want = pass == 0 ? 194 : 190;

			for (int i = 0; i < ATA_SMART_NUM_ATTRS; i++) {
				const uint8_t *a = d + ATA_SMART_DATA_ATTRS +
						   i * ATA_SMART_ATTR_SIZE;

				if (a[0] == want && a[5] > 0 && a[5] < 128) {
					t = a[5];
					break;
				}
			}
		}
	}
	kfree(d);
	return t;
}

/* SPC self-test results parameter from a SMART self-test log entry. */
static void sat_selftest_param(uint8_t *p, uint16_t code, const uint8_t *e)
{
	uint8_t test = e[0], st = e[1] >> 4;
	uint8_t stc, res;
	uint32_t flba = (uint32_t)e[5] | ((uint32_t)e[6] << 8) |
			((uint32_t)e[7] << 16) | ((uint32_t)e[8] << 24);

	switch (test) {
	case ATA_SMART_TEST_SHORT:
		stc = 1;
		break;
	case ATA_SMART_TEST_EXTENDED:
		stc = 2;
		break;
	case ATA_SMART_TEST_SHORT_CAPTIVE:
		stc = 5;
		break;
	case ATA_SMART_TEST_EXTENDED_CAPTIVE:
		stc = 6;
		break;
	default:
		stc = 0;
		break;
	}
	switch (st) {
	case 0x0:
		res = 0x0;
		break;
	case 0x1:
		res = 0x1;
		break;
	case 0x2:
		res = 0x2;
		break;
	case 0x3:
		res = 0x3;
		break;
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
	case 0x8:
		/* failed segments: ATA's element codes 4..7 map one to
		 * one, "handling damage" (8) has no SPC code of its own */
		res = (uint8_t)(st > 0x7 ? 0x7 : st);
		break;
	case 0xF:
		res = 0xF;
		break;
	default:
		res = 0x3;
		break;
	}
	put_be16(p, code);
	p[2] = 0x03; /* control: binary, always saved */
	p[3] = 0x10;
	p[4] = (uint8_t)((stc << 5) | res);
	p[5] = (uint8_t)code; /* self-test number */
	p[6] = e[3];
	p[7] = e[2]; /* power-on hours (big-endian here) */
	if (res >= 0x4 && res <= 0x7 && flba != 0xFFFFFFFFu) {
		put_be64(p + 8, flba);
		p[16] = MEDIUM_ERROR;
		p[17] = 0x40; /* diagnostic failure on component */
		p[18] = (uint8_t)(0x80 + res);
	} else {
		put_be64(p + 8, 0xFFFFFFFFFFFFFFFFull);
	}
}

static void sat_log_sense(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	const uint8_t *cdb = c->cdb;
	uint8_t page = cdb[2] & 0x3F;
	uint8_t subpage = cdb[3];
	uint16_t ptr = get_be16(cdb + 5);
	uint32_t alloc = get_be16(cdb + 7);
	uint8_t *r;
	uint32_t n = 4;

	if (cdb[1] & 0x02) { /* PPC */
		sat_invalid_field(c, 1, 1);
		return;
	}
	if (cdb[1] & 0x01) { /* SP */
		sat_invalid_field(c, 1, 0);
		return;
	}
	r = kcalloc(1, 1024);
	if (!r) {
		sat_host_error(c, -ENOMEM);
		return;
	}
	r[0] = page;
	r[1] = subpage;

	if (page == LPAGE_SUPPORTED && subpage == 0) {
		r[4] = LPAGE_SUPPORTED;
		r[5] = LPAGE_TEMPERATURE;
		r[6] = LPAGE_SELFTEST;
		r[7] = LPAGE_IE;
		n = 8;
	} else if (page == LPAGE_SUPPORTED && subpage == 0xFF) {
		static const uint8_t list[] = { 0x00, 0x00, 0x00, 0xFF,
						0x0D, 0x00, 0x10, 0x00,
						0x2F, 0x00 };

		r[0] |= 0x40; /* SPF */
		kmemcpy(r + 4, list, sizeof(list));
		n = 4 + sizeof(list);
	} else if (subpage != 0) {
		kfree(r);
		sat_invalid_field(c, 3, -1);
		return;
	} else if (page == LPAGE_TEMPERATURE) {
		uint8_t t = sat_temperature(dev);

		if (ptr <= 0) {
			put_be16(r + n, 0x0000);
			r[n + 2] = 0x03;
			r[n + 3] = 2;
			r[n + 5] = t;
			n += 6;
		}
		if (ptr <= 1) {
			put_be16(r + n, 0x0001);
			r[n + 2] = 0x03;
			r[n + 3] = 2;
			r[n + 5] = 0xFF; /* reference temperature: unknown */
			n += 6;
		}
	} else if (page == LPAGE_SELFTEST) {
		uint8_t *log = kalloc(ATA_SECT_SIZE);
		int have = log && ata_smart_read_log(dev, ATA_LOG_SELFTEST,
						    log, 1) == 0;
		uint8_t index = have ? log[508] : 0;

		for (int i = 0; i < 20; i++) {
			uint16_t code = (uint16_t)(i + 1);
			uint8_t *p = r + n;

			if (code < ptr)
				continue;
			if (have && index && index <= 21 && i < 21) {
				int slot = ((int)index - 1 - i + 21) % 21;
				const uint8_t *e = log + 2 + slot * 24;

				if (e[0] || e[1] || e[2] || e[3]) {
					sat_selftest_param(p, code, e);
					n += 20;
					continue;
				}
			}
			/* an empty entry */
			kmemset(p, 0, 20);
			put_be16(p, code);
			p[2] = 0x03;
			p[3] = 0x10;
			n += 20;
		}
		kfree(log);
	} else if (page == LPAGE_IE) {
		int failing = 0;
		uint8_t t = sat_temperature(dev);

		if (ptr == 0) {
			if ((dev->flags & ATA_DFLAG_SMART_ON) &&
			    ata_smart_return_status(dev, &failing) == 0 &&
			    failing) {
				r[n + 4] = 0x5D; /* failure prediction */
				r[n + 5] = 0x10; /* hardware impending failure,
						  * general hard drive failure */
			}
			put_be16(r + n, 0x0000);
			r[n + 2] = 0x03;
			r[n + 3] = 3;
			r[n + 6] = t;
			n += 7;
		}
	} else {
		kfree(r);
		sat_invalid_field(c, 2, 5);
		return;
	}
	put_be16(r + 2, (uint16_t)(n - 4));
	sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), r, n));
	kfree(r);
}

/* ---- SEND DIAGNOSTIC ------------------------------------------------------------ */

static void sat_send_diagnostic(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	uint8_t code = c->cdb[1] >> 5;
	int selftest = (c->cdb[1] & 0x04) ? 1 : 0;
	uint16_t plen = get_be16(c->cdb + 3);
	uint8_t test;
	int ret;

	if (plen) {
		sat_invalid_field(c, 3, -1);
		return;
	}
	if (selftest) {
		if (code) {
			sat_invalid_field(c, 1, 7);
			return;
		}
		if (dev->flags & ATA_DFLAG_SMART_ON) {
			ret = ata_smart_execute_offline(
				dev, ATA_SMART_TEST_SHORT_CAPTIVE);
		} else {
			uint8_t diag = 0;

			ret = ata_execute_diagnostic(dev, &diag);
			if (ret == 0 && diag != 0x01) {
				sat_check(c, HARDWARE_ERROR, 0x40, 0x80 | 1);
				return;
			}
		}
		if (ret == -EIO) {
			sat_check(c, HARDWARE_ERROR, 0x40, 0x81);
			return;
		}
		if (ret) {
			sat_host_error(c, ret);
			return;
		}
		sat_good(c, 0);
		return;
	}
	switch (code) {
	case 0:
		/* neither a self-test nor a code: nothing to do */
		sat_good(c, 0);
		return;
	case 1:
		test = ATA_SMART_TEST_SHORT;
		break;
	case 2:
		test = ATA_SMART_TEST_EXTENDED;
		break;
	case 4:
		test = ATA_SMART_TEST_ABORT;
		break;
	case 5:
		test = ATA_SMART_TEST_SHORT_CAPTIVE;
		break;
	case 6:
		test = ATA_SMART_TEST_EXTENDED_CAPTIVE;
		break;
	default:
		sat_invalid_field(c, 1, 7);
		return;
	}
	if (!(dev->flags & ATA_DFLAG_SMART_ON)) {
		sat_invalid_field(c, 1, 7);
		return;
	}
	ret = ata_smart_execute_offline(dev, test);
	if (ret == -EOPNOTSUPP) {
		sat_invalid_field(c, 1, 7);
		return;
	}
	if (ret == -EIO) {
		sat_check(c, HARDWARE_ERROR, 0x40, 0x81);
		return;
	}
	if (ret) {
		sat_host_error(c, ret);
		return;
	}
	sat_good(c, 0);
}

/* ---- REPORT LUNS / REPORT SUPPORTED OPERATION CODES ------------------------- */

static void sat_report_luns(struct sat_cmd *c)
{
	uint32_t alloc = get_be32(c->cdb + 6);
	uint8_t r[16];

	if (alloc < 16) {
		sat_invalid_field(c, 6, -1);
		return;
	}
	if (c->cdb[2] > 0x02 && c->cdb[2] != 0x10 && c->cdb[2] != 0x11 &&
	    c->cdb[2] != 0x12) {
		sat_invalid_field(c, 2, -1);
		return;
	}
	kmemset(r, 0, sizeof(r));
	put_be32(r, 8); /* one LUN */
	/* LUN 0 */
	sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), r, 16));
}

struct sat_opdesc {
	uint8_t op;
	uint8_t has_sa;
	uint16_t sa;
	uint8_t cdb_len;
	uint8_t usage[16];
};

static const struct sat_opdesc sat_ops[] = {
	{ TEST_UNIT_READY, 0, 0, 6, { 0x00, 0, 0, 0, 0, 0 } },
	{ REQUEST_SENSE, 0, 0, 6, { 0x03, 0x01, 0, 0, 0xFF, 0 } },
	{ READ_6, 0, 0, 6, { 0x08, 0x1F, 0xFF, 0xFF, 0xFF, 0 } },
	{ WRITE_6, 0, 0, 6, { 0x0A, 0x1F, 0xFF, 0xFF, 0xFF, 0 } },
	{ INQUIRY, 0, 0, 6, { 0x12, 0x01, 0xFF, 0xFF, 0xFF, 0 } },
	{ MODE_SELECT, 0, 0, 6, { 0x15, 0x11, 0, 0, 0xFF, 0 } },
	{ MODE_SENSE, 0, 0, 6, { 0x1A, 0x08, 0xFF, 0xFF, 0xFF, 0 } },
	{ START_STOP, 0, 0, 6, { 0x1B, 0x01, 0, 0x0F, 0xF3, 0 } },
	{ SEND_DIAGNOSTIC, 0, 0, 6, { 0x1D, 0xE4, 0, 0xFF, 0xFF, 0 } },
	{ ALLOW_MEDIUM_REMOVAL, 0, 0, 6, { 0x1E, 0, 0, 0, 0x03, 0 } },
	{ READ_CAPACITY, 0, 0, 10,
	  { 0x25, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0x01, 0 } },
	{ READ_10, 0, 0, 10,
	  { 0x28, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0 } },
	{ WRITE_10, 0, 0, 10,
	  { 0x2A, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0 } },
	{ VERIFY, 0, 0, 10,
	  { 0x2F, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0 } },
	{ SYNCHRONIZE_CACHE, 0, 0, 10,
	  { 0x35, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0 } },
	{ WRITE_SAME, 0, 0, 10,
	  { 0x41, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0 } },
	{ UNMAP, 0, 0, 10, { 0x42, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0 } },
	{ LOG_SENSE, 0, 0, 10,
	  { 0x4D, 0, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0 } },
	{ MODE_SELECT_10, 0, 0, 10,
	  { 0x55, 0x11, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0 } },
	{ MODE_SENSE_10, 0, 0, 10,
	  { 0x5A, 0x18, 0xFF, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0 } },
	{ ATA_16, 0, 0, 16,
	  { 0x85, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0xFF, 0 } },
	{ READ_16, 0, 0, 16,
	  { 0x88, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0, 0 } },
	{ WRITE_16, 0, 0, 16,
	  { 0x8A, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0, 0 } },
	{ VERIFY_16, 0, 0, 16,
	  { 0x8F, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0, 0 } },
	{ SYNCHRONIZE_CACHE_16, 0, 0, 16,
	  { 0x91, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0, 0 } },
	{ WRITE_SAME_16, 0, 0, 16,
	  { 0x93, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0, 0 } },
	{ SERVICE_ACTION_IN_16, 1, SAI_READ_CAPACITY_16, 16,
	  { 0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0,
	    0 } },
	{ SERVICE_ACTION_IN_16, 1, SAI_GET_LBA_STATUS, 16,
	  { 0x9E, 0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0, 0 } },
	{ REPORT_LUNS, 0, 0, 12,
	  { 0xA0, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0 } },
	{ ATA_12, 0, 0, 12,
	  { 0xA1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
	    0 } },
	{ MAINTENANCE_IN, 1, MI_REPORT_SUPPORTED_OPERATION_CODES, 12,
	  { 0xA3, 0x0C, 0x87, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
	    0 } },
	{ READ_12, 0, 0, 12,
	  { 0xA8, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
	    0 } },
	{ WRITE_12, 0, 0, 12,
	  { 0xAA, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
	    0 } },
	{ VERIFY_12, 0, 0, 12,
	  { 0xAF, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
	    0 } },
};

#define SAT_NOPS ((int)(sizeof(sat_ops) / sizeof(sat_ops[0])))

static void sat_report_opcodes(struct sat_cmd *c)
{
	const uint8_t *cdb = c->cdb;
	uint8_t opts = cdb[2] & 0x07;
	uint8_t req_op = cdb[3];
	uint16_t req_sa = get_be16(cdb + 4);
	uint32_t alloc = get_be32(cdb + 6);
	uint8_t *r;
	uint32_t n = 0;

	r = kcalloc(1, 4 + SAT_NOPS * 8 + 32);
	if (!r) {
		sat_host_error(c, -ENOMEM);
		return;
	}
	if (opts == 0) {
		n = 4;
		for (int i = 0; i < SAT_NOPS; i++) {
			uint8_t *d = r + n;

			d[0] = sat_ops[i].op;
			put_be16(d + 2, sat_ops[i].has_sa ? sat_ops[i].sa : 0);
			d[5] = sat_ops[i].has_sa ? 0x01 : 0x00; /* SERVACTV */
			put_be16(d + 6, sat_ops[i].cdb_len);
			n += 8;
		}
		put_be32(r, n - 4);
	} else if (opts <= 3) {
		const struct sat_opdesc *o = NULL;
		int op_has_sa = 0;

		for (int i = 0; i < SAT_NOPS; i++) {
			if (sat_ops[i].op != req_op)
				continue;
			if (sat_ops[i].has_sa)
				op_has_sa = 1;
			if (sat_ops[i].has_sa && opts != 1 &&
			    sat_ops[i].sa != req_sa)
				continue;
			o = &sat_ops[i];
			break;
		}
		/* option 1 asks about an opcode without service actions;
		 * option 2 about one with: the wrong one is an error */
		if ((opts == 1 && op_has_sa) || (opts == 2 && o && !o->has_sa)) {
			kfree(r);
			sat_invalid_field(c, 2, 2);
			return;
		}
		if (o && !(o->has_sa && opts == 1)) {
			r[1] = 0x03; /* supported, per the standard */
			put_be16(r + 2, o->cdb_len);
			kmemcpy(r + 4, o->usage, o->cdb_len);
			n = 4 + o->cdb_len;
		} else {
			r[1] = 0x01; /* not supported */
			n = 4;
		}
	} else {
		kfree(r);
		sat_invalid_field(c, 2, 2);
		return;
	}
	sat_good(c, sat_copy_out(c->buf, sat_min32(alloc, c->len), r, n));
	kfree(r);
}

static void sat_maintenance_in(struct sat_cmd *c)
{
	if ((c->cdb[1] & 0x1F) == MI_REPORT_SUPPORTED_OPERATION_CODES)
		sat_report_opcodes(c);
	else
		sat_invalid_field(c, 1, 4);
}

static void sat_allow_medium_removal(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	uint8_t prevent = c->cdb[4] & 0x03;
	struct ata_taskfile tf, rtf;
	int ret;

	if (!(dev->flags & ATA_DFLAG_REMOVABLE)) {
		/* fixed media: nothing to lock */
		sat_good(c, 0);
		return;
	}
	if (prevent > 1) {
		sat_invalid_field(c, 4, 1);
		return;
	}
	kmemset(&tf, 0, sizeof(tf));
	tf.command = prevent ? ATA_CMD_MEDIA_LOCK : ATA_CMD_MEDIA_UNLOCK;
	tf.flags = ATA_TFLAG_DEVICE;
	ret = sat_exec_tf(dev, &tf, ATA_PROT_NODATA, NULL, 0, ATA_DIR_NONE,
			  c->timeout_ms, &rtf, NULL, 0);
	if (ret == -EIO && (rtf.error & ATA_ABORTED)) {
		/* the drive has no lock: that is fine */
		sat_good(c, 0);
		return;
	}
	if (ret == -EIO) {
		sat_ata_error(c, &rtf, 0, 0, 0);
		return;
	}
	if (ret) {
		sat_host_error(c, ret);
		return;
	}
	dev->door_locked = prevent;
	sat_good(c, 0);
}

/* ---- ATA PASS-THROUGH ---------------------------------------------------------- */

/* The result of a pass-through command as SCSI status + sense. */
static void sat_pt_complete(struct sat_cmd *c, const struct ata_sat_pt *pt,
			    const struct ata_taskfile *rtf, int failed)
{
	struct sat_sense_info si;

	sat_set_last_pt(c->dev, rtf, pt->extend);
	if (!failed && !pt->ck_cond)
		return; /* GOOD, set by the caller */

	kmemset(&si, 0, sizeof(si));
	if (failed) {
		ata_sat_error_to_sense(rtf->status, rtf->error, &si.key,
				       &si.asc, &si.ascq);
		if (si.key == NO_SENSE)
			si.key = ABORTED_COMMAND;
	} else {
		/* CK_COND: "ATA pass through information available" */
		si.key = RECOVERED_ERROR;
		si.asc = SAT_ASC_NO_ADDITIONAL;
		si.ascq = SAT_ASCQ_ATA_PT_INFO;
	}
	si.ata_tf = rtf;
	si.extend = pt->extend;
	c->res->status = SAM_STAT_CHECK_CONDITION;
	/* The ATA status return descriptor needs descriptor format; fixed
	 * format only when the application turned D_SENSE off explicitly
	 * and asks for nothing else -- tools all parse the descriptor. */
	c->res->sense_len = sat_sense_build(c->res->sense, ATA_SENSE_LEN, 1,
					    &si);
	c->res->driver_status = SG_ERR_DRIVER_SENSE;
}

/* The signature a reset leaves in the registers. */
static void sat_reset_signature(const struct ata_device *dev,
				struct ata_taskfile *rtf)
{
	kmemset(rtf, 0, sizeof(*rtf));
	rtf->status = ATA_DRDY | ATA_DSC;
	rtf->error = 0x01;
	rtf->nsect = 0x01;
	rtf->lbal = 0x01;
	if (dev->class == ATA_DEV_ATAPI) {
		rtf->lbam = 0x14;
		rtf->lbah = 0xEB;
		rtf->status = 0x00;
	}
}

static void sat_ata_passthru(struct sat_cmd *c)
{
	struct ata_device *dev = c->dev;
	struct ata_sat_pt pt;
	struct ata_taskfile rtf;
	int bad_byte = -1, bad_bit = -1;
	uint32_t resid = 0, xfer;
	uint32_t timeout;
	int ret, refused_byte = -1;

	ret = ata_sat_pt_decode(c->cdb, c->cdb_len, c->buf ? c->len : 0,
				sat_lbs(dev), &pt, &bad_byte, &bad_bit);
	if (ret) {
		sat_invalid_field(c, bad_byte, bad_bit);
		return;
	}
	if (pt.dir != ATA_DIR_NONE && pt.dir != c->dir) {
		/* T_DIR disagrees with the transfer the caller set up */
		sat_invalid_field(c, 2, 3);
		return;
	}
	if (pt.prot == ATA_PROT_FPDMA &&
	    (!(dev->flags & ATA_DFLAG_NCQ_ON) || dev->ncq_depth == 0)) {
		sat_invalid_field(c, 1, 4);
		return;
	}
	if (sat_pt_refused(dev, &pt.tf, &refused_byte)) {
		kprintf(SAT_PFX "%s: pass-through of %s (feature %02xh) "
				"refused\n",
			dev->name, ata_cmd_name(pt.tf.command), pt.tf.feature);
		sat_invalid_field(c, pt.cdb_len == 16 ? refused_byte : 3, -1);
		return;
	}
	timeout = c->timeout_ms ? c->timeout_ms : ATA_TMOUT_INTERNAL_MS;

	switch (pt.protocol) {
	case SAT_PROT_RET_RESP: {
		int extend = 0;

		if (!sat_get_last_pt(dev, &rtf, &extend))
			sat_reset_signature(dev, &rtf);
		pt.ck_cond = 1;
		pt.extend = (uint8_t)extend;
		sat_pt_complete(c, &pt, &rtf, 0);
		c->res->resid = c->len;
		return;
	}
	case SAT_PROT_HARD_RESET:
	case SAT_PROT_SRST:
	case SAT_PROT_DEV_RESET:
		if (!dev->xport || !dev->xport->reset) {
			sat_host_error(c, -EOPNOTSUPP);
			return;
		}
		ret = dev->xport->reset(dev, pt.protocol == SAT_PROT_HARD_RESET
						     ? ATA_RESET_HARD
						     : ATA_RESET_SOFT);
		if (ret) {
			sat_host_error(c, ret);
			return;
		}
		if (pt.off_line)
			ahci_msleep(((1u << (pt.off_line + 1)) - 2) * 1000u);
		sat_reset_signature(dev, &rtf);
		sat_good(c, 0);
		sat_pt_complete(c, &pt, &rtf, 0);
		return;
	case SAT_PROT_DEV_DIAG: {
		uint8_t code = 0;

		ret = ata_execute_diagnostic(dev, &code);
		if (ret && ret != -EIO) {
			sat_host_error(c, ret);
			return;
		}
		sat_reset_signature(dev, &rtf);
		rtf.error = code;
		sat_good(c, 0);
		sat_pt_complete(c, &pt, &rtf, ret == -EIO);
		return;
	}
	default:
		break;
	}

	/* Commands with long run times get the caller's timeout; give the
	 * well-known slow ones at least what the driver itself would. */
	switch (pt.tf.command) {
	case ATA_CMD_SEC_ERASE_UNIT:
		if (timeout < ATA_TMOUT_SEC_ERASE_MIN_MS)
			timeout = ATA_TMOUT_SEC_ERASE_MIN_MS;
		break;
	case ATA_CMD_FLUSH:
	case ATA_CMD_FLUSH_EXT:
		if (timeout < ATA_TMOUT_FLUSH_MS)
			timeout = ATA_TMOUT_FLUSH_MS;
		break;
	case ATA_CMD_STANDBYNOW1:
	case ATA_CMD_STANDBY:
		if (timeout < ATA_TMOUT_STANDBY_MS)
			timeout = ATA_TMOUT_STANDBY_MS;
		break;
	case ATA_CMD_DSM:
		if (timeout < ATA_TMOUT_TRIM_MS)
			timeout = ATA_TMOUT_TRIM_MS;
		break;
	default:
		break;
	}

	xfer = pt.xfer_len;
	ret = sat_exec_tf(dev, &pt.tf, pt.prot, xfer ? c->buf : NULL, xfer,
			  xfer ? pt.dir : ATA_DIR_NONE, timeout, &rtf, &resid,
			  0);
	if (pt.off_line)
		ahci_msleep(((1u << (pt.off_line + 1)) - 2) * 1000u);

	if (ret == 0 || (ret == -EIO && (rtf.status & (ATA_ERR | ATA_DF)))) {
		int failed = ret != 0 ||
			     (rtf.status & (ATA_ERR | ATA_DF)) != 0;

		if (!failed)
			sat_post_command(dev, &pt.tf);
		/* data actually moved: the announced length minus the
		 * controller's residue; the rest of the caller's buffer
		 * was not touched */
		sat_good(c, failed ? 0 : xfer - (resid <= xfer ? resid : xfer));
		sat_pt_complete(c, &pt, &rtf, failed);
		return;
	}
	sat_host_error(c, ret);
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

/* CDB length the opcode's group implies */
static int sat_cdb_group_len(uint8_t op)
{
	switch (op >> 5) {
	case 0:
		return 6;
	case 1:
	case 2:
		return 10;
	case 4:
		return 16;
	case 5:
		return 12;
	default:
		return 6; /* vendor/reserved groups: no rule */
	}
}

int ata_sat_execute(struct ata_device *dev, const uint8_t *cdb, int cdb_len,
		    void *buf, uint32_t len, enum ata_dma_dir dir,
		    uint32_t timeout_ms, struct ata_sat_result *res)
{
	struct sat_cmd c;

	if (!dev || !cdb || !res)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	kmemset(res, 0, sizeof(*res));
	res->resid = len;
	if (cdb_len < 6 || cdb_len > SG_MAX_CDB_SIZE)
		return -EINVAL;
	if (len && !buf)
		return -EINVAL;

	if (dev->class == ATA_DEV_ATAPI) {
		/* The drive speaks SCSI itself. */
		uint8_t st = SAM_STAT_GOOD;
		uint32_t sl = ATA_SENSE_LEN, resid = len;
		int ret;

		ret = atapi_exec_cdb_resid(dev, cdb, cdb_len, buf, len, dir,
					   res->sense, &sl, &st, timeout_ms,
					   &resid);
		res->status = st;
		res->sense_len = sl;
		res->resid = resid;
		if (sl)
			res->driver_status = SG_ERR_DRIVER_SENSE;
		if (ret && st == SAM_STAT_GOOD) {
			c.dev = dev;
			c.cdb = cdb;
			c.cdb_len = cdb_len;
			c.buf = buf;
			c.len = len;
			c.dir = dir;
			c.timeout_ms = timeout_ms;
			c.res = res;
			c.d_sense = 0;
			sat_host_error(&c, ret);
		}
		return 0;
	}

	c.dev = dev;
	c.cdb = cdb;
	c.cdb_len = cdb_len;
	c.buf = (uint8_t *)buf;
	c.len = len;
	c.dir = dir;
	c.timeout_ms = timeout_ms ? timeout_ms : ATA_TMOUT_RW_MS;
	c.res = res;
	c.d_sense = sat_get_d_sense(dev);

	if (dev->flags & ATA_DFLAG_DETACHED) {
		sat_host_error(&c, -ENODEV);
		return 0;
	}
	if (cdb_len < sat_cdb_group_len(cdb[0])) {
		sat_check(&c, ILLEGAL_REQUEST, SAT_ASC_INVALID_FIELD_CDB, 0x00);
		return 0;
	}
	/* NACA / link bits in the CONTROL byte are not supported */
	if (cdb[sat_cdb_group_len(cdb[0]) - 1] & 0x05) {
		sat_invalid_field(&c, sat_cdb_group_len(cdb[0]) - 1, 2);
		return 0;
	}

	switch (cdb[0]) {
	case TEST_UNIT_READY:
		sat_test_unit_ready(&c);
		break;
	case REQUEST_SENSE:
		sat_request_sense(&c);
		break;
	case INQUIRY:
		sat_inquiry(&c);
		break;
	case READ_CAPACITY:
		sat_read_capacity10(&c);
		break;
	case SERVICE_ACTION_IN_16:
		sat_service_action_in16(&c);
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		sat_read_write(&c);
		break;
	case VERIFY:
	case VERIFY_12:
	case VERIFY_16:
		sat_verify(&c);
		break;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		sat_sync_cache(&c);
		break;
	case UNMAP:
		sat_unmap(&c);
		break;
	case WRITE_SAME:
	case WRITE_SAME_16:
		sat_write_same(&c);
		break;
	case START_STOP:
		sat_start_stop(&c);
		break;
	case MODE_SENSE:
	case MODE_SENSE_10:
		sat_mode_sense(&c);
		break;
	case MODE_SELECT:
	case MODE_SELECT_10:
		sat_mode_select(&c);
		break;
	case LOG_SENSE:
		sat_log_sense(&c);
		break;
	case SEND_DIAGNOSTIC:
		sat_send_diagnostic(&c);
		break;
	case REPORT_LUNS:
		sat_report_luns(&c);
		break;
	case MAINTENANCE_IN:
		sat_maintenance_in(&c);
		break;
	case ALLOW_MEDIUM_REMOVAL:
		sat_allow_medium_removal(&c);
		break;
	case ATA_12:
	case ATA_16:
		sat_ata_passthru(&c);
		break;
	default:
		sat_invalid_opcode(&c);
		break;
	}

	/* The sense of a CHECK CONDITION was returned with it (autosense);
	 * a later REQUEST SENSE asks the drive afresh, as SAT requires. */
	return 0;
}

/* ==========================================================================
 * SG_IO
 * ========================================================================== */

static int sat_capable(struct task *cur)
{
	if (cur)
		return cred_is_root(&((task_t *)cur)->cred);
	return capable();
}

/* Commands an unprivileged user may send on a node opened for reading. */
static int sat_cmd_is_safe(const uint8_t *cdb, int atapi)
{
	switch (cdb[0]) {
	case TEST_UNIT_READY:
	case REQUEST_SENSE:
	case INQUIRY:
	case READ_CAPACITY:
	case MODE_SENSE:
	case MODE_SENSE_10:
	case REPORT_LUNS:
	case LOG_SENSE:
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
	case VERIFY:
	case VERIFY_12:
	case VERIFY_16:
		return 1;
	case SERVICE_ACTION_IN_16:
		return (cdb[1] & 0x1F) == SAI_READ_CAPACITY_16 ||
		       (cdb[1] & 0x1F) == SAI_GET_LBA_STATUS;
	case MAINTENANCE_IN:
		return (cdb[1] & 0x1F) == MI_REPORT_SUPPORTED_OPERATION_CODES;
	default:
		break;
	}
	if (!atapi)
		return 0;
	/* the read-only MMC commands of an optical drive */
	switch (cdb[0]) {
	case GET_CONFIGURATION:
	case GET_EVENT_STATUS_NOTIFICATION:
	case READ_TOC:
	case READ_HEADER:
	case READ_DISC_INFORMATION:
	case MMC_READ_TRACK_INFO:
	case MMC_READ_SUB_CHANNEL:
	case MMC_READ_FORMAT_CAPACITIES:
	case MMC_READ_BUFFER_CAPACITY:
	case MMC_READ_DVD_STRUCTURE:
	case MMC_REPORT_KEY:
	case READ_CD:
	case MMC_READ_CD_MSF:
	case MECHANISM_STATUS:
	case GET_PERFORMANCE:
		return 1;
	default:
		return 0;
	}
}

/* The MMC writing commands of an optical drive that have no name in the
 * SCSI header: SET STREAMING, BLANK, CLOSE TRACK/SESSION, RESERVE TRACK,
 * SEND CUE SHEET and SEND OPC INFORMATION. */
#define SAT_MMC_SET_STREAMING 0xB6
#define SAT_MMC_BLANK 0xA1
#define SAT_MMC_CLOSE_TRACK 0x5B
#define SAT_MMC_RESERVE_TRACK 0x53
#define SAT_MMC_SEND_CUE_SHEET 0x5D
#define SAT_MMC_SEND_OPC 0x54

/* Commands an unprivileged user may send in addition on a node opened for
 * writing: writing the medium and its cache is exactly what such an open
 * already allows through write(2), and an optical drive's burning commands
 * belong to the same class.  Nothing here reaches the drive's firmware,
 * security or configuration state -- those stay root-only. */
static int sat_cmd_is_write_safe(const uint8_t *cdb, int atapi)
{
	switch (cdb[0]) {
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
	case WRITE_VERIFY:
	case WRITE_VERIFY_12:
	case WRITE_VERIFY_16:
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
	case ALLOW_MEDIUM_REMOVAL:
	case START_STOP:
		return 1;
	default:
		break;
	}
	if (!atapi)
		return 0;
	switch (cdb[0]) {
	case SET_CD_SPEED:
	case SAT_MMC_SET_STREAMING:
	case SAT_MMC_BLANK:
	case SAT_MMC_CLOSE_TRACK:
	case SAT_MMC_RESERVE_TRACK:
	case SAT_MMC_SEND_CUE_SHEET:
	case SAT_MMC_SEND_OPC:
	case FORMAT_UNIT:
		return 1;
	default:
		return 0;
	}
}

long ata_sat_sg_io(struct ata_device *dev, void *user_hdr, struct task *cur,
		   int open_flags)
{
	struct sg_io_hdr hdr;
	uint8_t cdb[SG_MAX_CDB_SIZE];
	struct sg_iovec *iov = NULL;
	struct ata_sat_result *res = NULL;
	uint8_t *kbuf = NULL;
	uint32_t hw_snap = 0;
	uint32_t len, timeout;
	enum ata_dma_dir dir;
	uint64_t t0;
	long ret = 0;
	int atapi;

	if (!dev || !user_hdr)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	if (copy_from_user(&hdr, user_hdr, sizeof(hdr)))
		return -EFAULT;
	if (hdr.interface_id != 'S')
		return -ENOSYS;
	if (hdr.cmd_len < 6 || hdr.cmd_len > SG_MAX_CDB_SIZE || !hdr.cmdp)
		return -EINVAL;
	kmemset(cdb, 0, sizeof(cdb));
	if (copy_from_user(cdb, hdr.cmdp, hdr.cmd_len))
		return -EFAULT;
	atapi = dev->class == ATA_DEV_ATAPI;

	/* Permissions: root for everything; otherwise the harmless list on
	 * a node opened for reading, plus the medium-writing list when the
	 * node was opened for writing as well. */
	if (!sat_capable(cur)) {
		int acc = open_flags & 3;
		int ok = 0;

		if (acc != O_WRONLY && sat_cmd_is_safe(cdb, atapi))
			ok = 1;
		if (acc != O_RDONLY && sat_cmd_is_write_safe(cdb, atapi))
			ok = 1;
		if (!ok)
			return -EPERM;
	}

	/* Direction and length */
	len = hdr.dxfer_len;
	switch (hdr.dxfer_direction) {
	case SG_DXFER_NONE:
		dir = ATA_DIR_NONE;
		len = 0;
		break;
	case SG_DXFER_TO_DEV:
		dir = ATA_DIR_TO_DEVICE;
		break;
	case SG_DXFER_FROM_DEV:
	case SG_DXFER_TO_FROM_DEV:
		dir = ATA_DIR_FROM_DEVICE;
		break;
	case SG_DXFER_UNKNOWN:
		dir = len ? ATA_DIR_FROM_DEVICE : ATA_DIR_NONE;
		break;
	default:
		return -EINVAL;
	}
	if (len == 0)
		dir = ATA_DIR_NONE;
	if (len > SAT_SG_MAX_XFER)
		return -ENOMEM;
	if (len && !hdr.dxferp)
		return -EINVAL;

	/* The iovec form: dxferp points to iovec_count sg_iovec entries */
	if (len && hdr.iovec_count) {
		uint64_t total = 0;

		if (hdr.iovec_count > SAT_SG_MAX_IOVEC)
			return -EINVAL;
		iov = kcalloc(hdr.iovec_count, sizeof(*iov));
		if (!iov)
			return -ENOMEM;
		if (copy_from_user(iov, hdr.dxferp,
				   (size_t)hdr.iovec_count * sizeof(*iov))) {
			ret = -EFAULT;
			goto out;
		}
		for (uint32_t i = 0; i < hdr.iovec_count; i++)
			total += iov[i].iov_len;
		if (total < len)
			len = (uint32_t)total;
	}

	res = kcalloc(1, sizeof(*res));
	if (!res) {
		ret = -ENOMEM;
		goto out;
	}
	if (len) {
		kbuf = kalloc(len);
		if (!kbuf) {
			ret = -ENOMEM;
			goto out;
		}
		kmemset(kbuf, 0, len);
		hw_snap = ata_dev_hw_owned_snap(dev);
	}

	/* data in */
	if (len && (hdr.dxfer_direction == SG_DXFER_TO_DEV ||
		    hdr.dxfer_direction == SG_DXFER_TO_FROM_DEV) &&
	    !(hdr.flags & SG_FLAG_NO_DXFER)) {
		if (iov) {
			uint32_t off = 0;

			for (uint32_t i = 0; i < hdr.iovec_count && off < len;
			     i++) {
				uint32_t n = (uint32_t)iov[i].iov_len;

				if (n > len - off)
					n = len - off;
				if (n && copy_from_user(kbuf + off,
							iov[i].iov_base, n)) {
					ret = -EFAULT;
					goto out;
				}
				off += n;
			}
		} else if (copy_from_user(kbuf, hdr.dxferp, len)) {
			ret = -EFAULT;
			goto out;
		}
	}

	if (hdr.timeout == 0)
		timeout = SG_DEFAULT_TIMEOUT_MS;
	else if (hdr.timeout == 0xFFFFFFFFu)
		timeout = ATA_WAIT_FOREVER - 1;
	else
		timeout = hdr.timeout;

	t0 = hrtimer_now_ns();
	ret = ata_sat_execute(dev, cdb, hdr.cmd_len, kbuf, len, dir, timeout,
			      res);
	if (ret)
		goto out;

	/* results */
	hdr.status = res->status;
	hdr.masked_status = (uint8_t)((res->status & 0x3E) >> 1);
	hdr.msg_status = 0;
	hdr.host_status = res->host_status;
	hdr.driver_status = res->driver_status;
	hdr.resid = (int)(res->resid <= hdr.dxfer_len ? res->resid
						      : hdr.dxfer_len);
	hdr.duration = (uint32_t)((hrtimer_now_ns() - t0) / 1000000ULL);
	hdr.sb_len_wr = 0;
	if (res->sense_len && hdr.sbp && hdr.mx_sb_len) {
		uint32_t n = sat_min32(res->sense_len, hdr.mx_sb_len);

		if (copy_to_user(hdr.sbp, res->sense, n)) {
			ret = -EFAULT;
			goto out;
		}
		hdr.sb_len_wr = (uint8_t)n;
	}
	hdr.info = (hdr.status || hdr.host_status || hdr.driver_status)
			   ? SG_INFO_CHECK
			   : SG_INFO_OK;
	hdr.info |= SG_INFO_INDIRECT_IO;

	/* data out */
	if (len && dir == ATA_DIR_FROM_DEVICE &&
	    !(hdr.flags & SG_FLAG_NO_DXFER)) {
		uint32_t got = len - (res->resid <= len ? res->resid : len);

		if (iov) {
			uint32_t off = 0;

			for (uint32_t i = 0; i < hdr.iovec_count && off < got;
			     i++) {
				uint32_t n = (uint32_t)iov[i].iov_len;

				if (n > got - off)
					n = got - off;
				if (n && copy_to_user(iov[i].iov_base,
						      kbuf + off, n)) {
					ret = -EFAULT;
					goto out;
				}
				off += n;
			}
		} else if (got && copy_to_user(hdr.dxferp, kbuf, got)) {
			ret = -EFAULT;
			goto out;
		}
	}
	if (copy_to_user(user_hdr, &hdr, sizeof(hdr)))
		ret = -EFAULT;

out:
	if (kbuf && unlikely(ata_dev_hw_owned_since(dev, hw_snap))) {
		/* The command failed while the controller still owned its
		 * slot: the drive may yet move data through the buffer.
		 * Keep it (cleared -- it may have held a password). */
		WARN_RATELIMIT(1, "[ata] %s: keeping a %u-byte SG_IO buffer "
				  "the controller may still use",
			       dev->name, len);
		kmemset(kbuf, 0, len);
		kbuf = NULL;
	}
	if (kbuf) {
		/* the buffer may have held anything (passwords for SECURITY
		 * commands): clear it */
		kmemset(kbuf, 0, len);
		kfree(kbuf);
	}
	kfree(res);
	kfree(iov);
	return ret;
}

/* ==========================================================================
 * HDIO_*
 * ========================================================================== */

static long sat_put_long(void *argp, long v)
{
	if (!argp)
		return -EINVAL;
	return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
}

/* The timeout an HDIO command gets, by opcode. */
static uint32_t sat_hdio_timeout(uint8_t cmd)
{
	switch (cmd) {
	case ATA_CMD_SMART:
		return ATA_TMOUT_SMART_MS;
	case ATA_CMD_STANDBYNOW1:
	case ATA_CMD_STANDBY:
	case ATA_CMD_IDLEIMMEDIATE:
	case ATA_CMD_IDLE:
		return ATA_TMOUT_STANDBY_MS;
	case ATA_CMD_FLUSH:
	case ATA_CMD_FLUSH_EXT:
		return ATA_TMOUT_FLUSH_MS;
	case ATA_CMD_SEC_ERASE_UNIT:
		return ATA_TMOUT_SEC_ERASE_MIN_MS;
	case ATA_CMD_EDD:
		return ATA_TMOUT_DIAG_MS;
	case ATA_CMD_ID_ATA:
	case ATA_CMD_ID_ATAPI:
		return ATA_TMOUT_IDENTIFY_MS;
	default:
		return ATA_TMOUT_INTERNAL_MS;
	}
}

static long sat_hdio_drive_cmd(struct ata_device *dev, void *argp)
{
	uint8_t args[4];
	struct ata_taskfile tf, rtf;
	uint8_t *buf = NULL;
	uint32_t len;
	int byte, ret;

	if (!argp)
		return -EINVAL;
	if (copy_from_user(args, argp, sizeof(args)))
		return -EFAULT;
	len = (uint32_t)args[3] * ATA_SECT_SIZE;

	kmemset(&tf, 0, sizeof(tf));
	tf.command = args[0];
	tf.feature = args[2];
	if (args[0] == ATA_CMD_SMART) {
		tf.nsect = args[3];
		tf.lbal = args[1];
		tf.lbam = ATA_SMART_LBAM_PASS;
		tf.lbah = ATA_SMART_LBAH_PASS;
	} else {
		tf.nsect = args[1];
	}
	tf.flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	if (sat_pt_refused(dev, &tf, &byte))
		return -EINVAL;

	if (len) {
		buf = kalloc(len);
		if (!buf)
			return -ENOMEM;
		kmemset(buf, 0, len);
	}
	ret = sat_exec_tf(dev, &tf, len ? ATA_PROT_PIO_IN : ATA_PROT_NODATA,
			  buf, len, len ? ATA_DIR_FROM_DEVICE : ATA_DIR_NONE,
			  sat_hdio_timeout(tf.command), &rtf, NULL, 0);
	if (ret == 0 || ret == -EIO) {
		args[0] = rtf.status;
		args[1] = rtf.error;
		args[2] = rtf.nsect;
		if (copy_to_user(argp, args, sizeof(args)))
			ret = -EFAULT;
		else if (ret == 0 && len &&
			 copy_to_user((uint8_t *)argp + sizeof(args), buf,
				      len))
			ret = -EFAULT;
		if (ret == 0)
			sat_post_command(dev, &tf);
	}
	if (buf) {
		kmemset(buf, 0, len);
		kfree(buf);
	}
	return ret;
}

static long sat_hdio_drive_task(struct ata_device *dev, void *argp)
{
	uint8_t args[7];
	struct ata_taskfile tf, rtf;
	int byte, ret;

	if (!argp)
		return -EINVAL;
	if (copy_from_user(args, argp, sizeof(args)))
		return -EFAULT;
	kmemset(&tf, 0, sizeof(tf));
	tf.command = args[0];
	tf.feature = args[1];
	tf.nsect = args[2];
	tf.lbal = args[3];
	tf.lbam = args[4];
	tf.lbah = args[5];
	tf.device = (uint8_t)(args[6] & (uint8_t)~ATA_DEV1);
	tf.flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	if (tf.device & ATA_LBA)
		tf.flags |= ATA_TFLAG_LBA;
	if (sat_pt_refused(dev, &tf, &byte))
		return -EINVAL;
	ret = sat_exec_tf(dev, &tf, ATA_PROT_NODATA, NULL, 0, ATA_DIR_NONE,
			  sat_hdio_timeout(tf.command), &rtf, NULL, 0);
	if (ret == 0 || ret == -EIO) {
		args[0] = rtf.status;
		args[1] = rtf.error;
		args[2] = rtf.nsect;
		args[3] = rtf.lbal;
		args[4] = rtf.lbam;
		args[5] = rtf.lbah;
		args[6] = rtf.device;
		if (copy_to_user(argp, args, sizeof(args)))
			ret = -EFAULT;
		if (ret == 0)
			sat_post_command(dev, &tf);
	}
	return ret;
}

static long sat_hdio_drive_taskfile(struct ata_device *dev, void *argp)
{
	ide_task_request_t req;
	struct ata_taskfile tf, rtf;
	enum ata_prot prot;
	enum ata_dma_dir dir = ATA_DIR_NONE;
	uint8_t *buf = NULL;
	uint32_t len = 0;
	uint8_t *uout, *uin;
	int byte, lba48, ret;
	unsigned int of;

	if (!argp)
		return -EINVAL;
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.out_size > SAT_SG_MAX_XFER || req.in_size > SAT_SG_MAX_XFER)
		return -ENOMEM;
	if (req.out_size && req.in_size)
		return -EINVAL; /* one data phase per command */
	uout = (uint8_t *)argp + sizeof(req);
	uin = uout + req.out_size;

	switch (req.data_phase) {
	case TASKFILE_NO_DATA:
		prot = ATA_PROT_NODATA;
		break;
	case TASKFILE_IN:
	case TASKFILE_MULTI_IN:
	case TASKFILE_P_IN:
		prot = ATA_PROT_PIO_IN;
		dir = ATA_DIR_FROM_DEVICE;
		len = (uint32_t)req.in_size;
		break;
	case TASKFILE_OUT:
	case TASKFILE_MULTI_OUT:
	case TASKFILE_P_OUT:
		prot = ATA_PROT_PIO_OUT;
		dir = ATA_DIR_TO_DEVICE;
		len = (uint32_t)req.out_size;
		break;
	case TASKFILE_IN_DMA:
	case TASKFILE_P_IN_DMA:
		prot = ATA_PROT_DMA_IN;
		dir = ATA_DIR_FROM_DEVICE;
		len = (uint32_t)req.in_size;
		break;
	case TASKFILE_OUT_DMA:
	case TASKFILE_P_OUT_DMA:
		prot = ATA_PROT_DMA_OUT;
		dir = ATA_DIR_TO_DEVICE;
		len = (uint32_t)req.out_size;
		break;
	default:
		/* queued (TCQ) phases and the rest do not exist on SATA */
		return -EINVAL;
	}
	if ((prot != ATA_PROT_NODATA && len == 0) || (len & 1))
		return -EINVAL;

	of = req.out_flags.all;
	if (of == 0)
		of = IDE_TASKFILE_STD_OUT_FLAGS; /* nothing said: all valid */
	lba48 = (of & 0xFF00) != 0;
	kmemset(&tf, 0, sizeof(tf));
	tf.command = req.io_ports[IDE_COMMAND_OFFSET];
	if (of & (1u << 1))
		tf.feature = req.io_ports[IDE_FEATURE_OFFSET];
	if (of & (1u << 2))
		tf.lbal = req.io_ports[IDE_SECTOR_OFFSET];
	if (of & (1u << 3))
		tf.nsect = req.io_ports[IDE_NSECTOR_OFFSET];
	if (of & (1u << 4))
		tf.lbam = req.io_ports[IDE_LCYL_OFFSET];
	if (of & (1u << 5))
		tf.lbah = req.io_ports[IDE_HCYL_OFFSET];
	if (of & (1u << 6))
		tf.device = (uint8_t)(req.io_ports[IDE_SELECT_OFFSET] &
				      (uint8_t)~ATA_DEV1);
	if (lba48) {
		tf.flags |= ATA_TFLAG_LBA48;
		if (of & (1u << 9))
			tf.hob_feature = req.hob_ports[IDE_FEATURE_OFFSET];
		if (of & (1u << 10))
			tf.hob_lbal = req.hob_ports[IDE_SECTOR_OFFSET];
		if (of & (1u << 11))
			tf.hob_nsect = req.hob_ports[IDE_NSECTOR_OFFSET];
		if (of & (1u << 12))
			tf.hob_lbam = req.hob_ports[IDE_LCYL_OFFSET];
		if (of & (1u << 13))
			tf.hob_lbah = req.hob_ports[IDE_HCYL_OFFSET];
	}
	tf.flags |= ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	if (tf.device & ATA_LBA)
		tf.flags |= ATA_TFLAG_LBA;
	if (dir == ATA_DIR_TO_DEVICE)
		tf.flags |= ATA_TFLAG_WRITE;
	if (sat_pt_refused(dev, &tf, &byte))
		return -EINVAL;

	if (len) {
		buf = kalloc(len);
		if (!buf)
			return -ENOMEM;
		kmemset(buf, 0, len);
		if (dir == ATA_DIR_TO_DEVICE &&
		    copy_from_user(buf, uout, len)) {
			ret = -EFAULT;
			goto out;
		}
	}
	ret = sat_exec_tf(dev, &tf, prot, buf, len, dir,
			  sat_hdio_timeout(tf.command), &rtf, NULL, 0);
	if (ret == 0 || ret == -EIO) {
		req.io_ports[IDE_ERROR_OFFSET] = rtf.error;
		req.io_ports[IDE_NSECTOR_OFFSET] = rtf.nsect;
		req.io_ports[IDE_SECTOR_OFFSET] = rtf.lbal;
		req.io_ports[IDE_LCYL_OFFSET] = rtf.lbam;
		req.io_ports[IDE_HCYL_OFFSET] = rtf.lbah;
		req.io_ports[IDE_SELECT_OFFSET] = rtf.device;
		req.io_ports[IDE_STATUS_OFFSET] = rtf.status;
		if (lba48) {
			req.hob_ports[IDE_NSECTOR_OFFSET] = rtf.hob_nsect;
			req.hob_ports[IDE_SECTOR_OFFSET] = rtf.hob_lbal;
			req.hob_ports[IDE_LCYL_OFFSET] = rtf.hob_lbam;
			req.hob_ports[IDE_HCYL_OFFSET] = rtf.hob_lbah;
		}
		req.in_flags.all = IDE_TASKFILE_STD_IN_FLAGS |
				   (lba48 ? (IDE_HOB_STD_IN_FLAGS << 8) : 0);
		if (copy_to_user(argp, &req, sizeof(req)))
			ret = -EFAULT;
		else if (ret == 0 && dir == ATA_DIR_FROM_DEVICE &&
			 copy_to_user(uin, buf, len))
			ret = -EFAULT;
		if (ret == 0)
			sat_post_command(dev, &tf);
	}
out:
	if (buf) {
		kmemset(buf, 0, len);
		kfree(buf);
	}
	return ret;
}

/* The CHS geometry HDIO_GETGEO reports. */
static void sat_geometry(const struct ata_device *dev, uint64_t start,
			 struct hd_geometry *g)
{
	uint64_t cyl;
	uint32_t heads = 255, spt = 63;

	if (dev->heads && dev->heads <= 255 && dev->sectors &&
	    dev->sectors <= 63 && dev->cylinders) {
		heads = dev->heads;
		spt = dev->sectors;
	}
	cyl = dev->n_sectors / ((uint64_t)heads * spt);
	g->heads = (unsigned char)heads;
	g->sectors = (unsigned char)spt;
	g->cylinders = (unsigned short)(cyl > 0xFFFF ? 0xFFFF : cyl);
	g->start = (unsigned long)start;
}

long ata_hdio_ioctl(struct ata_device *dev, unsigned long cmd, void *argp,
		    struct task *cur)
{
	unsigned long val = (unsigned long)argp;
	int root = sat_capable(cur);
	int ret;

	if (!dev)
		return -ENODEV;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	switch (cmd) {
	/* ---- GET: anyone with the node open ---- */
	case HDIO_GET_IDENTITY:
		if (!argp)
			return -EINVAL;
		/* the 512 bytes as the drive sent them: little-endian words */
		{
			uint8_t id[ATA_ID_BYTES];

			for (int i = 0; i < ATA_ID_WORDS; i++) {
				id[2 * i] = (uint8_t)dev->id[i];
				id[2 * i + 1] = (uint8_t)(dev->id[i] >> 8);
			}
			return copy_to_user(argp, id, sizeof(id)) ? -EFAULT : 0;
		}
	case HDIO_OBSOLETE_IDENTITY:
		return -EINVAL;
	case HDIO_GETGEO: {
		struct hd_geometry g;

		if (!argp)
			return -EINVAL;
		if (dev->class == ATA_DEV_ATAPI)
			return -ENOTTY;
		sat_geometry(dev, 0, &g);
		return copy_to_user(argp, &g, sizeof(g)) ? -EFAULT : 0;
	}
	case HDIO_GET_32BIT:
		return sat_put_long(argp, 0);
	case HDIO_GET_MULTCOUNT:
		return sat_put_long(argp, dev->multi_count);
	case HDIO_GET_DMA:
		return sat_put_long(argp, 1);
	case HDIO_GET_WCACHE:
		return sat_put_long(argp,
				    (dev->flags & ATA_DFLAG_WCACHE_ON) ? 1 : 0);
	case HDIO_GET_ACOUSTIC:
		return sat_put_long(argp, (dev->flags & ATA_DFLAG_AAM)
						  ? dev->aam_level
						  : 0);
	case HDIO_GET_KEEPSETTINGS:
	case HDIO_GET_NOWERR:
	case HDIO_GET_UNMASKINTR:
	case HDIO_GET_NICE:
		return sat_put_long(argp, 0);
	case HDIO_GET_QDMA:
		return sat_put_long(argp, dev->ncq_depth);
	case HDIO_GET_BUSSTATE:
		return sat_put_long(argp, BUSSTATE_ON);
	case HDIO_GET_ADDRESS:
		return sat_put_long(argp, (dev->flags & ATA_DFLAG_LBA48)
						  ? HDIO_ADDRESS_LBA48
					  : (dev->flags & ATA_DFLAG_LBA)
						  ? HDIO_ADDRESS_LBA28
						  : HDIO_ADDRESS_CHS);

	/* ---- the commands: root only ---- */
	case HDIO_DRIVE_CMD:
		if (!root)
			return -EACCES;
		return sat_hdio_drive_cmd(dev, argp);
	case HDIO_DRIVE_TASK:
		if (!root)
			return -EACCES;
		return sat_hdio_drive_task(dev, argp);
	case HDIO_DRIVE_TASKFILE:
		if (!root)
			return -EACCES;
		return sat_hdio_drive_taskfile(dev, argp);
	case HDIO_DRIVE_RESET:
		if (!root)
			return -EACCES;
		if (!dev->xport || !dev->xport->reset)
			return -EOPNOTSUPP;
		kprintf(SAT_PFX "%s: reset requested by HDIO_DRIVE_RESET\n",
			dev->name);
		return dev->xport->reset(dev, ATA_RESET_HARD);

	/* ---- SET: root only ---- */
	case HDIO_SET_WCACHE:
		if (!root)
			return -EACCES;
		if (val > 1)
			return -EINVAL;
		if (!(dev->flags & ATA_DFLAG_WCACHE))
			return -EOPNOTSUPP;
		ret = ata_set_write_cache(dev, (int)val);
		if (ret == 0)
			sat_sync_write_cache(dev);
		return ret;
	case HDIO_SET_ACOUSTIC:
		if (!root)
			return -EACCES;
		if (val > 254)
			return -EINVAL;
		if (!(dev->flags & ATA_DFLAG_AAM))
			return -EOPNOTSUPP;
		return ata_set_aam(dev, (uint8_t)val);
	case HDIO_SET_MULTCOUNT: {
		uint8_t maxm = (uint8_t)(dev->id[ATA_ID_MAX_MULTSECT] & 0xFF);

		if (!root)
			return -EACCES;
		if (val > maxm || (val & (val - 1)))
			return -EINVAL;
		return ata_set_multiple(dev, (uint8_t)val);
	}
	case HDIO_SET_KEEPSETTINGS:
		if (!root)
			return -EACCES;
		if (val > 1)
			return -EINVAL;
		return ata_set_features(dev,
					val ? SETFEATURES_REVERT_DEF_OFF
					    : SETFEATURES_REVERT_DEF_ON,
					0);
	case HDIO_SET_32BIT:
	case HDIO_SET_NOWERR:
	case HDIO_SET_UNMASKINTR:
	case HDIO_SET_NICE:
		if (!root)
			return -EACCES;
		return val == 0 ? 0 : -EINVAL;
	case HDIO_SET_DMA:
		if (!root)
			return -EACCES;
		return val == 1 ? 0 : -EINVAL; /* AHCI is always DMA */
	case HDIO_SET_ADDRESS:
		if (!root)
			return -EACCES;
		/* the addressing mode follows the drive; only the current
		 * one may be "set" */
		return val == ((dev->flags & ATA_DFLAG_LBA48)
				       ? HDIO_ADDRESS_LBA48
				       : HDIO_ADDRESS_LBA28)
			       ? 0
			       : -EINVAL;
	case HDIO_SET_BUSSTATE:
		if (!root)
			return -EACCES;
		return val == BUSSTATE_ON ? 0 : -EINVAL;
	case HDIO_SET_PIO_MODE:
	case HDIO_SET_XFER:
	case HDIO_SCAN_HWIF:
	case HDIO_UNREGISTER_HWIF:
	case HDIO_TRISTATE_HWIF:
	case HDIO_SET_QDMA:
		if (!root)
			return -EACCES;
		/* parallel-IDE controls: meaningless on SATA */
		return -EINVAL;
	default:
		return -ENOTTY;
	}
}

/* ==========================================================================
 * blk_ops.ioctl
 * ========================================================================== */

/* A SCSI command run for an ioctl (TUR, START STOP ...): 0 when GOOD. */
static long sat_simple_cdb(struct ata_device *dev, const uint8_t *cdb,
			   int cdb_len)
{
	struct ata_sat_result *res = kcalloc(1, sizeof(*res));
	long ret;

	if (!res)
		return -ENOMEM;
	ret = ata_sat_execute(dev, cdb, cdb_len, NULL, 0, ATA_DIR_NONE,
			      ATA_TMOUT_ATAPI_MS, res);
	if (ret == 0) {
		if (res->host_status == SG_ERR_DID_TIME_OUT)
			ret = -ETIMEDOUT;
		else if (res->host_status == SG_ERR_DID_NO_CONNECT)
			ret = -ENODEV;
		else if (res->host_status)
			ret = -EIO;
		else if (res->status == SAM_STAT_CHECK_CONDITION) {
			uint8_t key = 0, asc = 0;

			if (res->sense_len >= 14 &&
			    (res->sense[0] & 0x7F) == 0x70) {
				key = res->sense[2] & 0x0F;
				asc = res->sense[12];
			} else if (res->sense_len >= 4) {
				key = res->sense[1] & 0x0F;
				asc = res->sense[2];
			}
			ret = (key == NOT_READY &&
			       asc == SAT_ASC_MEDIUM_NOT_PRESENT)
				      ? -ENOMEDIUM
				      : -EIO;
		} else if (res->status != SAM_STAT_GOOD) {
			ret = -EIO;
		}
	}
	kfree(res);
	return ret;
}

static long sat_cdrom_ioctl(struct ata_device *dev, unsigned long cmd,
			    unsigned long arg)
{
	switch (cmd) {
	case SAT_CDROMEJECT:
		return atapi_eject(dev, 0);
	case SAT_CDROMCLOSETRAY:
		return atapi_eject(dev, 1);
	case SAT_CDROM_LOCKDOOR:
		return atapi_lock_door(dev, arg ? 1 : 0);
	case SAT_CDROM_MEDIA_CHANGED: {
		int was = dev->media_changed;
		int r = atapi_check_media(dev);

		if (r < 0 && r != -ENOMEDIUM)
			return r;
		return (r > 0 || was) ? 1 : 0;
	}
	case SAT_CDROM_DRIVE_STATUS: {
		int r = atapi_test_unit_ready(dev);

		if (r == 0)
			return SAT_CDS_DISC_OK;
		if (r == -ENOMEDIUM)
			return SAT_CDS_NO_DISC;
		if (r == -EAGAIN)
			return SAT_CDS_DRIVE_NOT_READY;
		return SAT_CDS_NO_INFO;
	}
	case SAT_CDROM_GET_CAPABILITY:
		return SAT_CDC_CLOSE_TRAY | SAT_CDC_OPEN_TRAY | SAT_CDC_LOCK |
		       SAT_CDC_MEDIA_CHANGED | SAT_CDC_GENERIC_PACKET |
		       SAT_CDC_DRIVE_STATUS;
	default:
		return -ENOTTY;
	}
}

/* blk_ops.ioctl: the same requests without the open mode of the node --
 * treated as a read-only open, which is what every open of a disk node
 * grants. */
long ata_sat_blk_ioctl(block_device_t *bdev, unsigned long cmd, void *argp,
		       struct task *cur)
{
	return ata_sat_blk_ioctl_mode(bdev, cmd, argp, cur, O_RDONLY);
}

/* blk_ops.ioctl_mode: `open_flags' are the open flags of the node the
 * request came through, so SG_IO lets a caller that opened the disk for
 * writing send the writing commands as well. */
long ata_sat_blk_ioctl_mode(block_device_t *bdev, unsigned long cmd,
			    void *argp, struct task *cur, int open_flags)
{
	struct gendisk *disk;
	struct ata_device *dev;
	int root = sat_capable(cur);
	int atapi;

	if (!bdev || !blk_bdev_is_extended(bdev) || !bdev->disk)
		return -ENOTTY;
	disk = bdev->disk;
	dev = (struct ata_device *)disk->private_data;
	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -ENOTTY;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	atapi = dev->class == ATA_DEV_ATAPI;

	/* Through a partition node the device's own requests (SG_IO, the
	 * SCSI_IOCTL_* and HDIO_* commands) would address the whole disk
	 * with absolute LBAs -- a caller allowed only on /dev/sdb1 could
	 * read or overwrite all of sdb.  So on a partition they are for root
	 * only; everybody else gets the generic block requests (-ENOTTY
	 * hands the call on to them).  The geometry stays answerable. */
	if (bdev->part && !root && cmd != HDIO_GETGEO)
		return -ENOTTY;

	switch (cmd) {
	case SG_IO:
		return ata_sat_sg_io(dev, argp, cur, open_flags);

	case HDIO_GETGEO: {
		struct hd_geometry g;

		if (atapi)
			return -ENOTTY;
		if (!argp)
			return -EINVAL;
		sat_geometry(dev, bdev->start_lba, &g);
		return copy_to_user(argp, &g, sizeof(g)) ? -EFAULT : 0;
	}

	case SG_EMULATED_HOST: {
		/* An ATA device behind the SCSI/ATA translation: the host
		 * is an emulated one (1), as the SG interface reports it. */
		int v = 1;

		if (!argp)
			return -EINVAL;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	}

	case SCSI_IOCTL_SEND_COMMAND:
		return -EINVAL;
	case SCSI_IOCTL_TEST_UNIT_READY: {
		uint8_t cdb[6] = { TEST_UNIT_READY, 0, 0, 0, 0, 0 };

		return sat_simple_cdb(dev, cdb, 6);
	}
	case SCSI_IOCTL_START_UNIT:
	case SCSI_IOCTL_STOP_UNIT: {
		uint8_t cdb[6] = { START_STOP, 0, 0, 0, 0, 0 };

		if (!root)
			return -EACCES;
		cdb[4] = cmd == SCSI_IOCTL_START_UNIT ? 0x01 : 0x00;
		return sat_simple_cdb(dev, cdb, 6);
	}
	case SCSI_IOCTL_SYNC: {
		uint8_t cdb[10] = { SYNCHRONIZE_CACHE };

		if (atapi)
			return 0;
		return sat_simple_cdb(dev, cdb, 10);
	}
	case SCSI_IOCTL_DOORLOCK:
	case SCSI_IOCTL_DOORUNLOCK: {
		uint8_t cdb[6] = { ALLOW_MEDIUM_REMOVAL, 0, 0, 0, 0, 0 };

		cdb[4] = cmd == SCSI_IOCTL_DOORLOCK ? 1 : 0;
		if (atapi)
			return atapi_lock_door(dev, cdb[4]);
		return sat_simple_cdb(dev, cdb, 6);
	}
	case SCSI_IOCTL_PROBE_HOST: {
		static const char host[] = "ahci";
		int ulen;

		if (!argp)
			return -EINVAL;
		if (copy_from_user(&ulen, argp, sizeof(ulen)))
			return -EFAULT;
		if (ulen <= 0)
			return 1;
		if ((size_t)ulen > sizeof(host))
			ulen = (int)sizeof(host);
		if (copy_to_user(argp, host, (size_t)ulen))
			return -EFAULT;
		return 1;
	}
	case SCSI_IOCTL_GET_PCI: {
		char slot[20];

		if (!argp)
			return -EINVAL;
		if (!disk->pci)
			return -ENXIO;
		kmemset(slot, 0, sizeof(slot));
		ksnprintf(slot, sizeof(slot), "0000:%02x:%02x.%x",
			  disk->pci->bus, disk->pci->device,
			  disk->pci->function);
		return copy_to_user(argp, slot, sizeof(slot)) ? -EFAULT : 0;
	}
	default:
		break;
	}

	/* HDIO_* (0x03xx) */
	if ((cmd & 0xFF00) == 0x0300) {
		long r = ata_hdio_ioctl(dev, cmd, argp, cur);

		if (r != -ENOTTY)
			return r;
	}
	/* the optical drive requests (0x53xx) */
	if (atapi && (cmd & 0xFF00) == 0x5300)
		return sat_cdrom_ioctl(dev, cmd, (unsigned long)argp);
	return -ENOTTY;
}
