// LikeOS ATA layer - ATAPI devices and optical drives
//
// An ATAPI device (a CD/DVD/BD drive on a SATA port) speaks SCSI -- the
// MMC command set -- tunnelled through one ATA command, PACKET (A0h): the
// host sends PACKET, the device asks for the 12- or 16-byte command
// descriptor block (the "packet", IDENTIFY PACKET DEVICE word 0 bits 1:0
// say which size), and the data phase follows either by DMA (FEATURE bit
// 0 set; with DMADIR -- FEATURE bit 2 -- giving the direction for drives
// behind bridges that need it, IDENTIFY word 62 bit 15) or by PIO in
// pieces no larger than the byte count limit the host put into LBA mid/
// high.  With AHCI the controller runs both phases itself: the driver puts
// the CDB into the command table's ACMD area, sets the A bit in the command
// header, and gets one completion.
//
// Errors are reported the SCSI way: the command ends with ERR set, the
// high nibble of the error register is the SENSE KEY, and the full sense
// data (additional sense code + qualifier) is fetched with REQUEST SENSE --
// which must be the very next command, or the drive forgets it.
//
// This file turns such a drive into the read-only block device srN
// (2048-byte blocks, removable, no partition scan) through the block
// layer's genhd interface:
//
//   * probe/registration (atapi_dev_register): CDB size, DMA capability,
//     waiting for the unit to become ready (TEST UNIT READY + REQUEST
//     SENSE, unit attentions after power-on cleared), INQUIRY, GET
//     CONFIGURATION (current profile, feature list: removable-medium
//     mechanism, CD/DVD/BD read and write features), MODE SENSE(10) of the
//     capabilities page 2Ah as the fallback, READ CAPACITY;
//   * the block I/O path (atapi_blk_submit): READ(10)/READ(12) straight
//     from the request's scatter/gather list, asynchronous, completed from
//     the transport's completion (any context) -- with a small ring of
//     preallocated commands so the submit path never allocates;
//   * pass-through: internal and SG_IO packet commands travel through the
//     disk's request queue as BLK_OP_DRV_IN/OUT requests, so they are
//     ordered with ordinary reads, and a CHECK CONDITION stops the queue
//     until REQUEST SENSE has collected the sense data;
//   * media handling: GET EVENT STATUS NOTIFICATION (polled, media class)
//     or TEST UNIT READY, UNIT ATTENTION "medium may have changed", READ
//     CAPACITY of the new disc, disk_media_changed()/disk_set_capacity(),
//     tray eject/load with START STOP UNIT, door locking with PREVENT
//     ALLOW MEDIUM REMOVAL (locked while the node is open, the way a
//     production system keeps a mounted disc in the drive).
//
// Media events: every drive has a delayed work on kblockd (the drive's
// events work) that asks it for media events with GET EVENT STATUS
// NOTIFICATION -- through the disk's request queue, like every other
// packet command -- every dev->media_poll_ms (2000; a drive that notifies
// asynchronously through SATA AN is not polled, its notification kicks
// the work).  A change is looked at with TEST UNIT READY and acted on:
// READ CAPACITY of the new disc, disk_set_capacity()/disk_media_changed().
// Error handling has no part in it.  A caller that holds the disk
// registry mutex (not recursive) leaves the change pending
// (dev->media_changed) and kicks the work, which delivers it.  While the
// disk suspends or resumes (its queue then only dispatches
// power-management requests) the packet commands of suspend/resume
// travel as such, the change notice through the registration worker; so
// does a change found by a caller holding the port's eh_mutex.
//
// Context: atapi_blk_submit() and the completion callbacks run in any
// context and never sleep or allocate; everything else may sleep (and
// polls before the scheduler runs, at boot).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/dev/ata/ata_dev_internal.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

#define ATAPI_PFX "[ata] "

#define ATAPI_BLOCK_SIZE 2048u
#define ATAPI_MEDIA_POLL_MS 2000u
/* How long registration waits for a drive that reports "becoming ready"
 * (spinning up a disc that was in it at power-on). */
#define ATAPI_READY_WAIT_MS 8000u
#define ATAPI_READY_STEP_MS 100u
/* Commands of the block I/O ring.  The queue depth of an optical drive is
 * one; the extra entries cover the instant in which a finished command's
 * completion has not quite returned while the next request is already
 * being dispatched from inside it. */
#define ATAPI_IO_QCS 3
/* PIO byte count limit (LBA mid/high): even, below 64 KiB */
#define ATAPI_MAX_BYTE_COUNT 0xFFFEu
#define ATAPI_INQUIRY_LEN 96u
#define ATAPI_GET_CONFIG_LEN 2048u
#define ATAPI_MODE_2A_LEN 64u
#define ATAPI_SENSE_REQ_LEN 18u

#define ATAPI_DRIVE_MAGIC 0x49505441u /* "ATPI" */

/* MMC opcodes this file builds (scsi.h has the common ones) */
#define MMC_READ_CD 0xBE
#define MMC_READ_CD_MSF 0xB9
#define MMC_READ_DISC_INFO 0x51
#define MMC_READ_TRACK_INFO 0x52

/* GET CONFIGURATION feature codes */
#define MMC_FEAT_PROFILE_LIST 0x0000
#define MMC_FEAT_CORE 0x0001
#define MMC_FEAT_MORPHING 0x0002
#define MMC_FEAT_REMOVABLE 0x0003
#define MMC_FEAT_RANDOM_READ 0x0010
#define MMC_FEAT_MULTI_READ 0x001D
#define MMC_FEAT_CD_READ 0x001E
#define MMC_FEAT_DVD_READ 0x001F
#define MMC_FEAT_RANDOM_WRITE 0x0020
#define MMC_FEAT_INC_STREAM_WRITE 0x0021
#define MMC_FEAT_CD_TAO 0x002D
#define MMC_FEAT_CD_MASTERING 0x002E
#define MMC_FEAT_DVD_R_WRITE 0x002F
#define MMC_FEAT_BD_READ 0x0040
#define MMC_FEAT_BD_WRITE 0x0041
#define MMC_FEAT_POWER_MGMT 0x0100
#define MMC_FEAT_RT_STREAMING 0x0107

/* Media event codes (GET EVENT STATUS NOTIFICATION, media class) */
#define MMC_EVT_NO_CHANGE 0x0
#define MMC_EVT_EJECT_REQUEST 0x1
#define MMC_EVT_NEW_MEDIA 0x2
#define MMC_EVT_MEDIA_REMOVAL 0x3
#define MMC_EVT_MEDIA_CHANGED 0x4
#define MMC_EVT_BG_FORMAT_DONE 0x5
#define MMC_EVT_BG_FORMAT_RESTART 0x6
#define MMC_GESN_CLASS_MEDIA 0x10 /* class request bit for media */
#define MMC_GESN_NOTIF_MEDIA 4 /* notification class number */

/* Capability bits we keep about the drive */
#define ATAPI_CAP_CD_R (1u << 0)
#define ATAPI_CAP_CD_RW (1u << 1)
#define ATAPI_CAP_DVD (1u << 2)
#define ATAPI_CAP_DVD_R (1u << 3)
#define ATAPI_CAP_DVD_RAM (1u << 4)
#define ATAPI_CAP_BD (1u << 5)
#define ATAPI_CAP_BD_R (1u << 6)
#define ATAPI_CAP_CDDA (1u << 7)
#define ATAPI_CAP_XA (1u << 8)
#define ATAPI_CAP_WRITE_CD (1u << 9)
#define ATAPI_CAP_WRITE_DVD (1u << 10)
#define ATAPI_CAP_MULTI_READ (1u << 11)

struct atapi_drive {
	uint32_t magic;
	struct ata_device *dev;
	struct gendisk *disk;
	char name[DISK_NAME_LEN]; /* "sr0" once allocated */

	/* block I/O ring; `lock' is a leaf (irqsave) */
	spinlock_t lock;
	struct ata_queued_cmd io_qc[ATAPI_IO_QCS];
	uint8_t io_busy[ATAPI_IO_QCS];

	/* internal / pass-through packet commands, one at a time */
	struct ata_mutex cmd_mutex;
	volatile int sense_hold; /* the queue is stopped for REQUEST SENSE */
	uint32_t probe_qcflags; /* EH flag while registering */

	/* INQUIRY */
	char vendor[9], product[17], revision[5];
	uint8_t inq_type, inq_rmb, inq_version;

	/* capabilities */
	uint16_t current_profile;
	uint32_t caps; /* ATAPI_CAP_* */
	uint8_t has_gc; /* GET CONFIGURATION works */
	uint8_t has_gesn; /* polled GET EVENT STATUS works */
	uint8_t lockable, ejectable, mech_type; /* loading mechanism */
	uint8_t pad0;
	uint16_t max_speed_kbs, cur_speed_kbs;

	/* media */
	uint64_t capacity; /* 2048-byte blocks */
	uint32_t block_size; /* what READ CAPACITY reported */
	uint8_t last_key, last_asc, last_ascq;
	uint8_t door_locked_open; /* we locked it because a node is open */
	int open_count;
	int dying;
	/* The task running the disk's suspend or resume hook (NULL
	 * otherwise): its packet commands must pass the suspended queue. */
	task_t *volatile pm_task;

	/* media events (atapi_check_events): the delayed work that polls
	 * the drive, and the agreement of its events with TEST UNIT READY */
	struct blk_work events_work;
	uint8_t get_event_changed, tur_changed, ignore_get_event;
	uint8_t pad1;
	uint32_t tur_mismatch;

	/* statistics */
	uint64_t n_reads, n_read_errors, n_requeues, n_media_changes;
	uint64_t n_pt_cmds, n_pt_errors;
};

/* The drives, so a struct ata_device leads to its drive object. */
static struct atapi_drive *g_atapi_drives[DISK_MAX_SR];
static spinlock_t g_atapi_lock = SPINLOCK_INIT("atapi_drives");

static int atapi_blk_submit(struct gendisk *disk, struct blk_request *req);
static int atapi_can_notify(struct ata_device *dev);
static void atapi_defer_notify(struct ata_device *dev);

/* ==========================================================================
 * Small helpers
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

static inline uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* Copy a space-padded SCSI string and trim it. */
static void atapi_copy_str(char *dst, const uint8_t *src, int len)
{
	int end = len;

	for (int i = 0; i < len; i++)
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
	while (end > 0 && dst[end - 1] == ' ')
		end--;
	dst[end] = '\0';
}

static const char *atapi_dev_label(const struct atapi_drive *drv,
				   const struct ata_device *dev)
{
	if (drv && drv->name[0])
		return drv->name;
	return dev ? dev->name : "atapi";
}

static struct atapi_drive *atapi_drive_of(const struct ata_device *dev)
{
	struct atapi_drive *drv = NULL;
	uint64_t fl;

	if (!dev)
		return NULL;
	spin_lock_irqsave(&g_atapi_lock, &fl);
	for (int i = 0; i < DISK_MAX_SR; i++)
		if (g_atapi_drives[i] && g_atapi_drives[i]->dev == dev) {
			drv = g_atapi_drives[i];
			break;
		}
	spin_unlock_irqrestore(&g_atapi_lock, fl);
	VM_BUG_ON(drv && drv->magic != ATAPI_DRIVE_MAGIC);
	return drv;
}

static int atapi_drive_table_add(struct atapi_drive *drv)
{
	uint64_t fl;
	int ret = -ENOSPC;

	spin_lock_irqsave(&g_atapi_lock, &fl);
	for (int i = 0; i < DISK_MAX_SR; i++)
		if (!g_atapi_drives[i]) {
			g_atapi_drives[i] = drv;
			ret = 0;
			break;
		}
	spin_unlock_irqrestore(&g_atapi_lock, fl);
	return ret;
}

static void atapi_drive_table_del(struct atapi_drive *drv)
{
	uint64_t fl;

	spin_lock_irqsave(&g_atapi_lock, &fl);
	for (int i = 0; i < DISK_MAX_SR; i++)
		if (g_atapi_drives[i] == drv)
			g_atapi_drives[i] = NULL;
	spin_unlock_irqrestore(&g_atapi_lock, fl);
}

/* Sense data (fixed 70h/71h or descriptor 72h/73h) -> key/asc/ascq. */
static void atapi_sense_decode(const uint8_t *s, uint32_t len, uint8_t *key,
			       uint8_t *asc, uint8_t *ascq)
{
	uint8_t rc;

	*key = *asc = *ascq = 0;
	if (!s || len < 1)
		return;
	rc = s[0] & 0x7F;
	if ((rc == 0x72 || rc == 0x73) && len >= 4) {
		*key = s[1] & 0x0F;
		*asc = s[2];
		*ascq = s[3];
	} else if ((rc == 0x70 || rc == 0x71) && len >= 3) {
		*key = s[2] & 0x0F;
		if (len >= 13)
			*asc = s[12];
		if (len >= 14)
			*ascq = s[13];
	}
}

/* Fixed-format sense from the error register when REQUEST SENSE cannot be
 * had: only the key is known. */
static uint32_t atapi_sense_from_error(uint8_t error, uint8_t *sense,
				       uint32_t cap)
{
	uint8_t s[18];

	kmemset(s, 0, sizeof(s));
	s[0] = 0x70;
	s[2] = (uint8_t)(error >> ATAPI_ERR_SENSE_KEY_SHIFT);
	if (error & ATAPI_ERR_ILI)
		s[2] |= 0x20;
	if (error & ATAPI_ERR_EOM)
		s[2] |= 0x40;
	s[7] = 10;
	if (cap > sizeof(s))
		cap = sizeof(s);
	kmemcpy(sense, s, cap);
	return cap;
}

static const char *atapi_key_name(uint8_t key)
{
	static const char *const names[16] = {
		"NO SENSE",	   "RECOVERED ERROR", "NOT READY",
		"MEDIUM ERROR",	   "HARDWARE ERROR",  "ILLEGAL REQUEST",
		"UNIT ATTENTION",  "DATA PROTECT",    "BLANK CHECK",
		"VENDOR SPECIFIC", "COPY ABORTED",    "ABORTED COMMAND",
		"EQUAL",	   "VOLUME OVERFLOW", "MISCOMPARE",
		"COMPLETED",
	};
	return names[key & 0x0F];
}

static const char *mmc_profile_name(uint16_t p)
{
	switch (p) {
	case 0x0000:
		return "none";
	case 0x0001:
		return "non-removable disk";
	case 0x0002:
		return "removable disk";
	case 0x0003:
		return "MO erasable";
	case 0x0004:
		return "MO write once";
	case 0x0005:
		return "AS-MO";
	case 0x0008:
		return "CD-ROM";
	case 0x0009:
		return "CD-R";
	case 0x000A:
		return "CD-RW";
	case 0x0010:
		return "DVD-ROM";
	case 0x0011:
		return "DVD-R";
	case 0x0012:
		return "DVD-RAM";
	case 0x0013:
		return "DVD-RW (restricted overwrite)";
	case 0x0014:
		return "DVD-RW (sequential)";
	case 0x0015:
		return "DVD-R DL (sequential)";
	case 0x0016:
		return "DVD-R DL (layer jump)";
	case 0x0017:
		return "DVD-RW DL";
	case 0x0018:
		return "DVD-Download";
	case 0x001A:
		return "DVD+RW";
	case 0x001B:
		return "DVD+R";
	case 0x002A:
		return "DVD+RW DL";
	case 0x002B:
		return "DVD+R DL";
	case 0x0040:
		return "BD-ROM";
	case 0x0041:
		return "BD-R (SRM)";
	case 0x0042:
		return "BD-R (RRM)";
	case 0x0043:
		return "BD-RE";
	case 0x0050:
		return "HD DVD-ROM";
	case 0x0051:
		return "HD DVD-R";
	case 0x0052:
		return "HD DVD-RAM";
	case 0xFFFF:
		return "non-standard";
	default:
		return "unknown";
	}
}

/* ==========================================================================
 * Packet construction
 * ========================================================================== */

/* DMA only for the bulk data commands with a length the controllers
 * handle safely; everything small and odd goes by PIO.  Many drives
 * mishandle DMA for short informational commands. */
static int atapi_want_dma(const struct ata_device *dev, const uint8_t *cdb,
			  uint32_t nbytes)
{
	if (!(dev->flags & ATA_DFLAG_ATAPI_DMA) || nbytes == 0)
		return 0;
	if (nbytes & 15)
		return 0;
	switch (cdb[0]) {
	case READ_10:
	case READ_12:
	case MMC_READ_CD:
	case MMC_READ_CD_MSF:
	case WRITE_10:
	case WRITE_12:
	case WRITE_VERIFY:
		return 1;
	default:
		return 0;
	}
}

/* Fill a command for one packet.  The data (sg or buffer) and nbytes are
 * already attached. */
static void atapi_prep_packet(const struct ata_device *dev,
			      struct ata_queued_cmd *qc, const uint8_t *cdb,
			      int cdb_len, enum ata_dma_dir dir)
{
	struct ata_taskfile *tf = &qc->tf;
	uint32_t nbytes = qc->nbytes;
	int pkt = dev->cdb_len == 16 ? 16 : 12;

	kmemset(qc->cdb, 0, sizeof(qc->cdb));
	kmemcpy(qc->cdb, cdb, (size_t)cdb_len);
	qc->cdb_len = (uint8_t)pkt;

	kmemset(tf, 0, sizeof(*tf));
	tf->command = ATA_CMD_PACKET;
	tf->flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;
	tf->device = 0;
	qc->dma_dir = nbytes ? dir : ATA_DIR_NONE;
	if (dir == ATA_DIR_TO_DEVICE && nbytes) {
		tf->flags |= ATA_TFLAG_WRITE;
		qc->flags |= ATA_QCFLAG_WRITE;
	}

	if (nbytes == 0) {
		qc->protocol = ATA_PROT_ATAPI_NODATA;
	} else if (atapi_want_dma(dev, cdb, nbytes)) {
		qc->protocol = ATA_PROT_ATAPI_DMA;
		tf->feature = ATAPI_PKT_DMA;
		/* DMADIR: 1 = device to host, for drives that need it */
		if ((dev->flags & ATA_DFLAG_ATAPI_DMADIR) &&
		    dir == ATA_DIR_FROM_DEVICE) {
			tf->feature |= ATAPI_DMADIR;
			qc->flags |= ATA_QCFLAG_DMADIR;
		}
	} else {
		uint32_t limit = nbytes > ATAPI_MAX_BYTE_COUNT
					 ? ATAPI_MAX_BYTE_COUNT
					 : nbytes;

		/* The byte count limit must be even unless it is the whole
		 * (odd) transfer. */
		if ((limit & 1) && limit != nbytes)
			limit--;
		qc->protocol = ATA_PROT_ATAPI_PIO;
		tf->lbam = (uint8_t)(limit & 0xFF);
		tf->lbah = (uint8_t)(limit >> 8);
	}
}

/* ==========================================================================
 * Command execution (process context)
 * ========================================================================== */

/* The end_io of a pass-through packet that travelled through the queue
 * (interrupt handler, port lock held): hand the request to the block
 * softirq, where atapi_drv_complete finishes it. */
static void atapi_drv_end_io(struct ata_queued_cmd *qc)
{
	struct blk_request *req = qc->req;

	VM_BUG_ON(!req || !qc->private_data);
	blk_complete_request(req, qc->result, qc->resid);
}

/* Its block softirq half.  On CHECK CONDITION the queue is stopped BEFORE
 * the request completes, so no read sneaks in before REQUEST SENSE. */
static void atapi_drv_complete(struct blk_request *req,
			       struct ata_queued_cmd *qc)
{
	struct atapi_drive *drv = (struct atapi_drive *)qc->private_data;

	VM_BUG_ON(!drv);
	if (qc->result && (qc->result_tf.status & ATA_ERR) && drv->disk &&
	    drv->disk->queue) {
		drv->sense_hold = 1;
		blk_queue_stop(drv->disk->queue);
	}
	blk_request_complete(req, qc->result, qc->resid);
}

/* Is the disk's queue usable for pass-through? */
static int atapi_queue_usable(const struct atapi_drive *drv)
{
	return drv && drv->disk && drv->disk->queue &&
	       (drv->disk->flags & GENHD_FL_UP) &&
	       !(drv->disk->flags & GENHD_FL_DEAD) && !drv->dying;
}

/* Run a prepared packet command to completion.  Through the request queue
 * once the disk is live (ordered with reads), directly before. */
static int atapi_issue_sync(struct atapi_drive *drv, struct ata_queued_cmd *qc)
{
	struct blk_request *req;
	int ret;

	if (!atapi_queue_usable(drv))
		return ata_qc_exec(qc);
	/* A caller under the port's eh_mutex (a registration, a
	 * revalidation, an ata_dev_lock() section) goes to the drive
	 * directly: error handling holds the queue stopped while it runs,
	 * and the eh_mutex holder is the one who would have to restart it.
	 * There is no ordinary read to be ordered with meanwhile. */
	if (ata_dev_eh_held(drv->dev))
		return ata_qc_exec(qc);

	/* BLK_OP_DRV_IN for every packet: the block layer refuses "write"
	 * operations on a read-only device, and an optical drive is one --
	 * but TEST UNIT READY, START STOP UNIT or a MODE SELECT are not
	 * writes to the medium.  The data direction the controller uses is
	 * the command's own (qc->dma_dir). */
	req = blk_request_alloc(&drv->disk->bdev, BLK_OP_DRV_IN, 0, 0);
	if (!req)
		return -ENOMEM;
	req->drv_cmd = qc;
	req->flags |= BLK_REQ_HEAD | BLK_REQ_NO_ACCOUNT | BLK_REQ_QUIET;
	/* The disk's own suspend / resume hook: a suspended queue only lets
	 * power-management requests through. */
	if (drv->pm_task && drv->pm_task == sched_current())
		req->flags |= BLK_REQ_PM;
	req->timeout_ms = qc->timeout_ms;
	if (qc->nbytes && qc->sg == &qc->sg_one) {
		ret = blk_request_add_buf(req, qc->sg_one.virt, qc->nbytes);
		if (ret) {
			blk_request_free(req);
			return ret;
		}
	}
	ret = blk_execute(req);
	/* The command's own result is authoritative once it ran; a request
	 * the queue refused never reached the drive. */
	if (qc->flags & ATA_QCFLAG_COMPLETE)
		ret = qc->result;
	blk_request_free(req);
	return ret;
}

/* REQUEST SENSE straight to the drive (the queue is stopped or not in use
 * yet).  Returns bytes of sense or -errno. */
static int atapi_fetch_sense(struct atapi_drive *drv, struct ata_device *dev,
			     uint8_t *sense, uint32_t cap)
{
	struct ata_queued_cmd *qc;
	uint8_t cdb[12];
	uint8_t *buf;
	uint32_t len = ATAPI_SENSE_REQ_LEN;
	int ret;

	buf = kalloc(ATAPI_SENSE_LEN);
	if (!buf)
		return -ENOMEM;
	kmemset(buf, 0, ATAPI_SENSE_LEN);
	qc = ata_qc_alloc(dev);
	if (!qc) {
		kfree(buf);
		return -ENOMEM;
	}
	kmemset(cdb, 0, sizeof(cdb));
	cdb[0] = REQUEST_SENSE;
	cdb[4] = (uint8_t)len;
	ata_qc_set_buf(qc, buf, len, ATA_DIR_FROM_DEVICE);
	qc->flags |= ATA_QCFLAG_INTERNAL | ATA_QCFLAG_QUIET |
		     ATA_QCFLAG_RESULT_TF | (drv ? drv->probe_qcflags : 0);
	qc->timeout_ms = ATA_TMOUT_ATAPI_TUR_MS;
	atapi_prep_packet(dev, qc, cdb, 6, ATA_DIR_FROM_DEVICE);
	ret = ata_qc_exec(qc);
	if (ret == 0) {
		uint32_t got = len - (qc->resid <= len ? qc->resid : len);

		if (got > cap)
			got = cap;
		kmemcpy(sense, buf, got);
		ret = (int)got;
	}
	if (unlikely(qc->flags & ATA_QCFLAG_HW_OWNED)) {
		/* The controller may still write the sense into it: keep
		 * the buffer rather than hand back memory under DMA. */
		WARN_RATELIMIT(1, "[atapi] %s: keeping the sense buffer of a "
				  "command the controller still owns",
			       dev->name);
		buf = NULL;
	}
	ata_qc_free(qc);
	kfree(buf);
	return ret;
}

int atapi_exec_cdb_resid(struct ata_device *dev, const uint8_t *cdb,
			 int cdb_len, void *buf, uint32_t len,
			 enum ata_dma_dir dir, uint8_t *sense,
			 uint32_t *sense_len, uint8_t *scsi_status,
			 uint32_t timeout_ms, uint32_t *resid)
{
	struct atapi_drive *drv;
	struct ata_queued_cmd *qc;
	uint8_t local_sense[ATAPI_SENSE_LEN];
	uint32_t slen = 0;
	uint8_t key = 0, asc = 0, ascq = 0;
	int ret;

	if (!dev || !cdb || cdb_len < 6 || cdb_len > ATAPI_CDB_LEN_MAX)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if ((len && !buf) || (len && dir == ATA_DIR_NONE) ||
	    (!len && dir != ATA_DIR_NONE && buf))
		return -EINVAL;
	if (len & 1) {
		/* The controller moves data in 16-bit units: run the command
		 * on a bounce buffer one byte longer and copy the caller's
		 * bytes.  (The drive transfers at most the allocation length
		 * the CDB names, so the extra byte stays unused.) */
		uint8_t *bounce = kalloc(len + 1);
		uint32_t r = len + 1;
		uint32_t hw_snap = ata_dev_hw_owned_snap(dev);

		if (!bounce)
			return -ENOMEM;
		kmemset(bounce, 0, len + 1);
		if (dir == ATA_DIR_TO_DEVICE)
			kmemcpy(bounce, buf, len);
		ret = atapi_exec_cdb_resid(dev, cdb, cdb_len, bounce, len + 1,
					   dir, sense, sense_len, scsi_status,
					   timeout_ms, &r);
		if (dir == ATA_DIR_FROM_DEVICE)
			kmemcpy(buf, bounce, len);
		if (resid)
			*resid = r ? r - 1 : 0;
		if (unlikely(ata_dev_hw_owned_since(dev, hw_snap))) {
			/* the command failed while the controller still
			 * owned its slot: the bounce may yet be written */
			WARN_RATELIMIT(1, "[atapi] %s: keeping a %u-byte "
					  "bounce buffer the controller may "
					  "still write into",
				       dev->name, len + 1);
			bounce = NULL;
		}
		kfree(bounce);
		return ret;
	}
	if (scsi_status)
		*scsi_status = SAM_STAT_GOOD;
	if (sense_len) {
		slen = *sense_len;
		*sense_len = 0;
	}
	if (resid)
		*resid = len;
	if (dev->class != ATA_DEV_ATAPI)
		return -ENODEV;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (cdb_len > (dev->cdb_len == 16 ? 16 : 12))
		return -EINVAL; /* the drive takes no packets that long */
	if (ata_can_sleep())
		might_sleep();

	drv = atapi_drive_of(dev);
	if (drv)
		ata_mutex_lock(&drv->cmd_mutex);

	qc = ata_qc_alloc(dev);
	if (!qc) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	if (len)
		ata_qc_set_buf(qc, buf, len, dir);
	qc->flags |= ATA_QCFLAG_INTERNAL | ATA_QCFLAG_RESULT_TF |
		     ATA_QCFLAG_QUIET | (drv ? drv->probe_qcflags : 0);
	qc->timeout_ms = timeout_ms ? timeout_ms : ATA_TMOUT_ATAPI_MS;
	qc->max_retries = 0; /* the caller decides about retries */
	atapi_prep_packet(dev, qc, cdb, cdb_len, len ? dir : ATA_DIR_NONE);
	if (drv) {
		qc->private_data = drv;
		drv->n_pt_cmds++;
	}

	ret = atapi_issue_sync(drv, qc);
	if (resid) {
		uint32_t r = qc->resid;

		*resid = (ret == 0 || (qc->flags & ATA_QCFLAG_COMPLETE))
				 ? (r <= len ? r : len)
				 : len;
	}

	if (ret == -EIO && (qc->result_tf.status & ATA_ERR)) {
		/* CHECK CONDITION: the sense key is in the error register;
		 * the rest comes from REQUEST SENSE, now, before anything
		 * else reaches the drive. */
		int got;

		if (scsi_status)
			*scsi_status = SAM_STAT_CHECK_CONDITION;
		got = atapi_fetch_sense(drv, dev, local_sense,
					sizeof(local_sense));
		if (got <= 0) {
			got = (int)atapi_sense_from_error(qc->result_tf.error,
							  local_sense,
							  sizeof(local_sense));
		}
		atapi_sense_decode(local_sense, (uint32_t)got, &key, &asc,
				   &ascq);
		if (key == 0 && asc == 0)
			key = qc->result_tf.error >> ATAPI_ERR_SENSE_KEY_SHIFT;
		if (sense && sense_len) {
			uint32_t n = (uint32_t)got < slen ? (uint32_t)got : slen;

			kmemcpy(sense, local_sense, n);
			*sense_len = n;
		}
		if (drv) {
			drv->last_key = key;
			drv->last_asc = asc;
			drv->last_ascq = ascq;
			drv->n_pt_errors++;
		}
		/* Interpret what matters to the caller's errno. */
		if (key == NOT_READY && asc == SAT_ASC_MEDIUM_NOT_PRESENT) {
			dev->media_present = 0;
			ret = -ENOMEDIUM;
		} else if (key == UNIT_ATTENTION) {
			if (asc == SAT_ASC_MEDIUM_CHANGED ||
			    asc == SAT_ASC_POWER_ON_RESET)
				dev->media_changed = 1;
		}
	}

	if (drv && drv->sense_hold) {
		drv->sense_hold = 0;
		if (drv->disk && drv->disk->queue)
			blk_queue_start(drv->disk->queue);
	}
	ata_qc_free(qc);
out_unlock:
	if (drv)
		ata_mutex_unlock(&drv->cmd_mutex);
	return ret;
}

int atapi_exec_cdb(struct ata_device *dev, const uint8_t *cdb, int cdb_len,
		   void *buf, uint32_t len, enum ata_dma_dir dir,
		   uint8_t *sense, uint32_t *sense_len, uint8_t *scsi_status,
		   uint32_t timeout_ms)
{
	return atapi_exec_cdb_resid(dev, cdb, cdb_len, buf, len, dir, sense,
				    sense_len, scsi_status, timeout_ms, NULL);
}

/* One command with the sense decoded, for the helpers below.  *key etc.
 * are set on CHECK CONDITION (0 otherwise). */
static int atapi_cmd(struct ata_device *dev, const uint8_t *cdb, int cdb_len,
		     void *buf, uint32_t len, enum ata_dma_dir dir,
		     uint32_t timeout_ms, uint8_t *key, uint8_t *asc,
		     uint8_t *ascq, uint32_t *resid)
{
	uint8_t sense[ATAPI_SENSE_LEN];
	uint32_t slen = sizeof(sense);
	uint8_t status = 0;
	int ret;

	ret = atapi_exec_cdb_resid(dev, cdb, cdb_len, buf, len, dir, sense,
				   &slen, &status, timeout_ms, resid);
	if (key)
		*key = 0;
	if (asc)
		*asc = 0;
	if (ascq)
		*ascq = 0;
	if (status == SAM_STAT_CHECK_CONDITION) {
		uint8_t k, a, q;

		atapi_sense_decode(sense, slen, &k, &a, &q);
		if (key)
			*key = k;
		if (asc)
			*asc = a;
		if (ascq)
			*ascq = q;
	}
	return ret;
}

/* ==========================================================================
 * The standard commands
 * ========================================================================== */

int atapi_request_sense(struct ata_device *dev, uint8_t *sense, uint32_t *len)
{
	struct atapi_drive *drv;
	int ret;

	if (!dev || !sense || !len || *len == 0)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	drv = atapi_drive_of(dev);
	if (drv)
		ata_mutex_lock(&drv->cmd_mutex);
	ret = atapi_fetch_sense(drv, dev, sense, *len);
	if (drv)
		ata_mutex_unlock(&drv->cmd_mutex);
	if (ret < 0) {
		*len = 0;
		return ret;
	}
	*len = (uint32_t)ret;
	return 0;
}

int atapi_test_unit_ready(struct ata_device *dev)
{
	uint8_t cdb[12] = { TEST_UNIT_READY };
	uint8_t key, asc, ascq;
	int ret;

	ret = atapi_cmd(dev, cdb, 6, NULL, 0, ATA_DIR_NONE,
			ATA_TMOUT_ATAPI_TUR_MS, &key, &asc, &ascq, NULL);
	if (ret == 0) {
		dev->media_present = 1;
		return 0;
	}
	if (ret == -ENOMEDIUM)
		return -ENOMEDIUM;
	if (ret == -EIO) {
		if (key == UNIT_ATTENTION)
			return -EAGAIN; /* reported once; try again */
		if (key == NOT_READY) {
			if (asc == SAT_ASC_MEDIUM_NOT_PRESENT) {
				dev->media_present = 0;
				return -ENOMEDIUM;
			}
			/* 04h/01h becoming ready, 04h/xx in progress */
			if (asc == SAT_ASC_LOGICAL_UNIT_NOT_READY)
				return -EAGAIN;
			return -EAGAIN;
		}
	}
	return ret;
}

int atapi_read_capacity(struct ata_device *dev, uint64_t *blocks,
			uint32_t *block_size)
{
	uint8_t cdb[12] = { READ_CAPACITY };
	uint8_t *buf;
	uint32_t last, bs;
	int ret;

	if (!dev || !blocks || !block_size)
		return -EINVAL;
	buf = kalloc(8);
	if (!buf)
		return -ENOMEM;
	kmemset(buf, 0, 8);
	ret = atapi_cmd(dev, cdb, 10, buf, 8, ATA_DIR_FROM_DEVICE,
			ATA_TMOUT_ATAPI_MS, NULL, NULL, NULL, NULL);
	if (ret == 0) {
		last = get_be32(buf);
		bs = get_be32(buf + 4);
		*block_size = bs;
		/* Blank or unreadable discs report 0 (or a bogus all-ones). */
		if (last == 0xFFFFFFFFu)
			*blocks = 0;
		else
			*blocks = (uint64_t)last + 1;
	}
	kfree(buf);
	return ret;
}

int atapi_lock_door(struct ata_device *dev, int lock)
{
	uint8_t cdb[12] = { ALLOW_MEDIUM_REMOVAL };
	int ret;

	if (!dev)
		return -EINVAL;
	cdb[4] = lock ? 1 : 0; /* PREVENT bit */
	ret = atapi_cmd(dev, cdb, 6, NULL, 0, ATA_DIR_NONE,
			ATA_TMOUT_ATAPI_TUR_MS, NULL, NULL, NULL, NULL);
	if (ret == 0)
		dev->door_locked = (uint8_t)(lock ? 1 : 0);
	else if (ret != -ENODEV)
		WARN_RATELIMIT(1, ATAPI_PFX "%s: %s door failed (%d)",
			       dev->name, lock ? "locking" : "unlocking", ret);
	return ret;
}

int atapi_eject(struct ata_device *dev, int load)
{
	struct atapi_drive *drv;
	uint8_t cdb[12] = { START_STOP };
	int ret;

	if (!dev)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	drv = atapi_drive_of(dev);

	if (!load && dev->door_locked) {
		/* A disc held by an open node stays in: the way a mounted
		 * disc refuses to eject.  Only an explicit unlock (or the
		 * last close) frees it. */
		if (drv && drv->open_count > 1) {
			kprintf(ATAPI_PFX "%s: eject refused: the drive is in "
					  "use\n",
				atapi_dev_label(drv, dev));
			return -EBUSY;
		}
		atapi_lock_door(dev, 0);
		if (drv)
			drv->door_locked_open = 0;
	}
	/* LoEj = 1; Start = 1 loads, 0 ejects.  Not IMMED: wait for the tray */
	cdb[4] = (uint8_t)(0x02 | (load ? 0x01 : 0x00));
	ret = atapi_cmd(dev, cdb, 6, NULL, 0, ATA_DIR_NONE, ATA_TMOUT_ATAPI_MS,
			NULL, NULL, NULL, NULL);
	if (ret == 0) {
		kprintf(ATAPI_PFX "%s: tray %s\n", atapi_dev_label(drv, dev),
			load ? "loaded" : "ejected");
		if (!load) {
			dev->media_present = 0;
			dev->media_changed = 1;
		}
		/* Let the media logic notice the change (new capacity or
		 * none) right away rather than at the next poll. */
		atapi_media_event(dev);
		if (drv && drv->disk && !load) {
			drv->capacity = 0;
			if (atapi_can_notify(dev)) {
				dev->media_changed = 0;
				disk_set_capacity(drv->disk, 0);
				disk_media_changed(drv->disk, 0);
			} else {
				atapi_defer_notify(dev);
			}
		}
	}
	return ret;
}

/* ==========================================================================
 * Capability discovery
 * ========================================================================== */

static int atapi_inquiry(struct atapi_drive *drv, struct ata_device *dev)
{
	uint8_t cdb[12] = { INQUIRY };
	uint8_t *buf;
	uint32_t resid = 0;
	int ret;

	buf = kalloc(ATAPI_INQUIRY_LEN);
	if (!buf)
		return -ENOMEM;
	kmemset(buf, 0, ATAPI_INQUIRY_LEN);
	cdb[4] = ATAPI_INQUIRY_LEN;
	ret = atapi_cmd(dev, cdb, 6, buf, ATAPI_INQUIRY_LEN,
			ATA_DIR_FROM_DEVICE, ATA_TMOUT_ATAPI_MS, NULL, NULL,
			NULL, &resid);
	if (ret == 0 && ATAPI_INQUIRY_LEN - resid < 36) {
		/* A short answer still has the type; the strings may be
		 * missing.  Fill what is there. */
		kmemset(buf + (ATAPI_INQUIRY_LEN - resid), 0, resid);
	}
	if (ret == 0) {
		drv->inq_type = buf[0] & 0x1F;
		drv->inq_rmb = (buf[1] & 0x80) ? 1 : 0;
		drv->inq_version = buf[2];
		atapi_copy_str(drv->vendor, buf + 8, 8);
		atapi_copy_str(drv->product, buf + 16, 16);
		atapi_copy_str(drv->revision, buf + 32, 4);
		if (!drv->vendor[0])
			ksnprintf(drv->vendor, sizeof(drv->vendor), "ATAPI");
		if (!drv->product[0])
			ksnprintf(drv->product, sizeof(drv->product), "%.16s",
				  dev->model);
		dev->atapi_type = drv->inq_type;
	}
	kfree(buf);
	return ret;
}

/* GET CONFIGURATION: current profile and the features we report. */
static void atapi_get_configuration(struct atapi_drive *drv,
				    struct ata_device *dev)
{
	uint8_t cdb[12] = { GET_CONFIGURATION };
	uint8_t *buf;
	uint32_t resid = 0, got, total;
	int ret;

	buf = kalloc(ATAPI_GET_CONFIG_LEN);
	if (!buf)
		return;
	kmemset(buf, 0, ATAPI_GET_CONFIG_LEN);
	cdb[1] = 0x00; /* RT 00b: all features */
	put_be16(cdb + 2, 0); /* starting feature */
	put_be16(cdb + 7, ATAPI_GET_CONFIG_LEN);
	ret = atapi_cmd(dev, cdb, 10, buf, ATAPI_GET_CONFIG_LEN,
			ATA_DIR_FROM_DEVICE, ATA_TMOUT_ATAPI_MS, NULL, NULL,
			NULL, &resid);
	if (ret != 0) {
		kfree(buf);
		return;
	}
	got = ATAPI_GET_CONFIG_LEN - resid;
	if (got < 8) {
		kfree(buf);
		return;
	}
	drv->has_gc = 1;
	total = get_be32(buf) + 4; /* data length excludes itself */
	if (total > got)
		total = got;
	drv->current_profile = get_be16(buf + 6);

	for (uint32_t off = 8; off + 4 <= total;) {
		uint16_t code = get_be16(buf + off);
		uint8_t alen = buf[off + 3];
		const uint8_t *d = buf + off + 4;

		if (off + 4 + alen > total)
			break;
		switch (code) {
		case MMC_FEAT_PROFILE_LIST:
			for (uint32_t k = 0; k + 4 <= alen; k += 4) {
				uint16_t p = get_be16(d + k);

				if (p == 0x0009)
					drv->caps |= ATAPI_CAP_CD_R;
				else if (p == 0x000A)
					drv->caps |= ATAPI_CAP_CD_RW;
				else if (p == 0x0010)
					drv->caps |= ATAPI_CAP_DVD;
				else if (p == 0x0011 || p == 0x0014 ||
					 p == 0x001A || p == 0x001B ||
					 p == 0x002B)
					drv->caps |= ATAPI_CAP_DVD_R |
						     ATAPI_CAP_DVD;
				else if (p == 0x0012)
					drv->caps |= ATAPI_CAP_DVD_RAM |
						     ATAPI_CAP_DVD;
				else if (p == 0x0040)
					drv->caps |= ATAPI_CAP_BD;
				else if (p >= 0x0041 && p <= 0x0043)
					drv->caps |= ATAPI_CAP_BD_R |
						     ATAPI_CAP_BD;
			}
			break;
		case MMC_FEAT_REMOVABLE:
			if (alen >= 1) {
				drv->mech_type = (uint8_t)(d[0] >> 5);
				drv->ejectable = (d[0] & 0x08) ? 1 : 0;
				drv->lockable = (d[0] & 0x01) ? 1 : 0;
			}
			break;
		case MMC_FEAT_MULTI_READ:
			drv->caps |= ATAPI_CAP_MULTI_READ;
			break;
		case MMC_FEAT_CD_READ:
			if (alen >= 1 && (d[0] & 0x01))
				drv->caps |= ATAPI_CAP_CDDA;
			break;
		case MMC_FEAT_DVD_READ:
			drv->caps |= ATAPI_CAP_DVD;
			break;
		case MMC_FEAT_CD_TAO:
		case MMC_FEAT_CD_MASTERING:
			drv->caps |= ATAPI_CAP_WRITE_CD;
			break;
		case MMC_FEAT_DVD_R_WRITE:
			drv->caps |= ATAPI_CAP_WRITE_DVD;
			break;
		case MMC_FEAT_BD_READ:
			drv->caps |= ATAPI_CAP_BD;
			break;
		case MMC_FEAT_BD_WRITE:
			drv->caps |= ATAPI_CAP_BD_R;
			break;
		default:
			break;
		}
		off += 4 + alen;
	}
	kfree(buf);
}

/* MODE SENSE(10) of the capabilities and mechanical status page (2Ah):
 * speeds, lock/eject capability, the loading mechanism, XA/CD-DA. */
static void atapi_mode_sense_caps(struct atapi_drive *drv,
				  struct ata_device *dev)
{
	uint8_t cdb[12] = { MODE_SENSE_10 };
	uint8_t *buf, *pg;
	uint32_t resid = 0, got, bdlen;
	int ret;

	buf = kalloc(ATAPI_MODE_2A_LEN);
	if (!buf)
		return;
	kmemset(buf, 0, ATAPI_MODE_2A_LEN);
	cdb[1] = 0x08; /* DBD */
	cdb[2] = 0x2A; /* current values, page 2Ah */
	put_be16(cdb + 7, ATAPI_MODE_2A_LEN);
	ret = atapi_cmd(dev, cdb, 10, buf, ATAPI_MODE_2A_LEN,
			ATA_DIR_FROM_DEVICE, ATA_TMOUT_ATAPI_MS, NULL, NULL,
			NULL, &resid);
	if (ret) {
		kfree(buf);
		return;
	}
	got = ATAPI_MODE_2A_LEN - resid;
	bdlen = get_be16(buf + 6);
	if (got < 8 + bdlen + 16) {
		kfree(buf);
		return;
	}
	pg = buf + 8 + bdlen;
	if ((pg[0] & 0x3F) != 0x2A) {
		kfree(buf);
		return;
	}
	if (pg[2] & 0x01)
		drv->caps |= ATAPI_CAP_CD_R;
	if (pg[2] & 0x02)
		drv->caps |= ATAPI_CAP_CD_RW;
	if (pg[2] & 0x08)
		drv->caps |= ATAPI_CAP_DVD;
	if (pg[2] & 0x10)
		drv->caps |= ATAPI_CAP_DVD_R;
	if (pg[2] & 0x20)
		drv->caps |= ATAPI_CAP_DVD_RAM;
	if (pg[3] & 0x03)
		drv->caps |= ATAPI_CAP_WRITE_CD;
	if (pg[3] & 0x30)
		drv->caps |= ATAPI_CAP_WRITE_DVD;
	if (pg[4] & 0x30) /* mode 2 form 1/2 */
		drv->caps |= ATAPI_CAP_XA;
	if (pg[5] & 0x01)
		drv->caps |= ATAPI_CAP_CDDA;
	if (!drv->has_gc) {
		drv->lockable = (pg[6] & 0x01) ? 1 : 0;
		drv->ejectable = (pg[6] & 0x08) ? 1 : 0;
		drv->mech_type = (uint8_t)(pg[6] >> 5);
	}
	drv->max_speed_kbs = get_be16(pg + 8);
	if (pg[1] + 2 >= 16)
		drv->cur_speed_kbs = get_be16(pg + 14);
	kfree(buf);
}

/* Probe whether polled GET EVENT STATUS NOTIFICATION works. */
static int atapi_gesn(struct ata_device *dev, uint8_t *event, int *present,
		      int *tray_open)
{
	uint8_t cdb[12] = { GET_EVENT_STATUS_NOTIFICATION };
	uint8_t *buf;
	uint32_t resid = 0;
	int ret;

	buf = kalloc(8);
	if (!buf)
		return -ENOMEM;
	kmemset(buf, 0, 8);
	cdb[1] = 0x01; /* polled */
	cdb[4] = MMC_GESN_CLASS_MEDIA;
	put_be16(cdb + 7, 8);
	ret = atapi_cmd(dev, cdb, 10, buf, 8, ATA_DIR_FROM_DEVICE,
			ATA_TMOUT_ATAPI_TUR_MS, NULL, NULL, NULL, &resid);
	if (ret == 0) {
		/* NEA (no event available) or the wrong class: not usable */
		if ((buf[2] & 0x80) || (buf[2] & 0x07) != MMC_GESN_NOTIF_MEDIA ||
		    8 - resid < 8) {
			ret = -EOPNOTSUPP;
		} else {
			*event = buf[4] & 0x0F;
			*present = (buf[5] & 0x02) ? 1 : 0;
			*tray_open = (buf[5] & 0x01) ? 1 : 0;
		}
	}
	kfree(buf);
	return ret;
}

/* Wait (bounded) until the unit answers TEST UNIT READY with something
 * final: ready, or no medium. */
static int atapi_wait_ready(struct ata_device *dev, uint32_t max_ms)
{
	uint32_t waited = 0;
	int ret, ua = 0;

	for (;;) {
		ret = atapi_test_unit_ready(dev);
		if (ret == 0 || ret == -ENOMEDIUM)
			return ret;
		if (ret != -EAGAIN)
			return ret;
		/* Unit attentions (power on, reset, medium change) are
		 * reported once each; a few in a row are normal and need no
		 * pause.  "Becoming ready" does. */
		if (++ua <= 4)
			continue;
		if (waited >= max_ms)
			return -ETIMEDOUT;
		ahci_msleep(ATAPI_READY_STEP_MS);
		waited += ATAPI_READY_STEP_MS;
	}
}

/* READ CAPACITY into the drive object (2048-byte blocks). */
static int atapi_update_capacity(struct atapi_drive *drv,
				 struct ata_device *dev)
{
	uint64_t blocks = 0;
	uint32_t bs = 0;
	int ret;

	ret = atapi_read_capacity(dev, &blocks, &bs);
	if (ret)
		return ret;
	drv->block_size = bs;
	if (bs != ATAPI_BLOCK_SIZE) {
		/* Audio and mixed-mode discs report 2352/2340; some drives 0
		 * or 512.  The block device works in 2048-byte blocks, the
		 * data blocks of every disc format. */
		if (bs && bs != 2352 && bs != 2340 && bs != 2336)
			kprintf(ATAPI_PFX "%s: unusual block size %u, using "
					  "%u\n",
				atapi_dev_label(drv, dev), bs,
				ATAPI_BLOCK_SIZE);
		if (bs && (bs % 512) == 0 && bs < ATAPI_BLOCK_SIZE)
			blocks = blocks * bs / ATAPI_BLOCK_SIZE;
	}
	drv->capacity = blocks;
	return 0;
}

static void atapi_log_drive(struct atapi_drive *drv, struct ata_device *dev)
{
	char caps[160];
	int len = 0;
	const char *mech;

	switch (drv->mech_type) {
	case 0:
		mech = "caddy";
		break;
	case 1:
		mech = "tray";
		break;
	case 2:
		mech = "pop-up";
		break;
	case 4:
		mech = "changer";
		break;
	case 5:
		mech = "cartridge changer";
		break;
	default:
		mech = "unknown loader";
		break;
	}
	caps[0] = '\0';
	if (drv->caps & (ATAPI_CAP_WRITE_CD | ATAPI_CAP_WRITE_DVD))
		len += ksnprintf(caps + len, sizeof(caps) - len, " writer");
	if (drv->caps & ATAPI_CAP_BD)
		len += ksnprintf(caps + len, sizeof(caps) - len, " bd%s",
				 (drv->caps & ATAPI_CAP_BD_R) ? "-r/re" : "");
	if (drv->caps & ATAPI_CAP_DVD)
		len += ksnprintf(caps + len, sizeof(caps) - len, " dvd%s",
				 (drv->caps & ATAPI_CAP_DVD_RAM) ? "-ram" : "");
	if (drv->caps & (ATAPI_CAP_CD_R | ATAPI_CAP_CD_RW))
		len += ksnprintf(caps + len, sizeof(caps) - len, " cd/rw");
	if (drv->caps & ATAPI_CAP_XA)
		len += ksnprintf(caps + len, sizeof(caps) - len, " xa/form2");
	if (drv->caps & ATAPI_CAP_CDDA)
		len += ksnprintf(caps + len, sizeof(caps) - len, " cdda");
	if (drv->caps & ATAPI_CAP_MULTI_READ)
		len += ksnprintf(caps + len, sizeof(caps) - len, " multiread");
	(void)len;

	kprintf(ATAPI_PFX "%s: %s %s %s, MMC drive (type %u, version %u): "
			  "%ux%s %s%s%s%s\n",
		atapi_dev_label(drv, dev), drv->vendor, drv->product,
		drv->revision, drv->inq_type, drv->inq_version,
		drv->max_speed_kbs ? drv->max_speed_kbs / 176 : 0,
		caps, mech, drv->lockable ? ", lockable" : "",
		drv->ejectable ? ", eject" : "",
		(dev->flags & ATA_DFLAG_ATAPI_DMA) ? ", DMA" : ", PIO only");
	kprintf(ATAPI_PFX "%s: %u-byte packets%s%s, current profile %04xh "
			  "(%s), media %s\n",
		atapi_dev_label(drv, dev), dev->cdb_len,
		(dev->flags & ATA_DFLAG_ATAPI_DMADIR) ? ", DMADIR" : "",
		(dev->flags & ATA_DFLAG_AN) ? ", async notification"
					    : ", polled media",
		drv->current_profile, mmc_profile_name(drv->current_profile),
		dev->media_present ? "present" : "absent");
	if (dev->media_present)
		kprintf(ATAPI_PFX "%s: %llu %u-byte blocks (%llu MB)\n",
			atapi_dev_label(drv, dev),
			(unsigned long long)drv->capacity, ATAPI_BLOCK_SIZE,
			(unsigned long long)(drv->capacity * ATAPI_BLOCK_SIZE /
					     1000000ULL));
}

/* ==========================================================================
 * Media changes
 * ========================================================================== */

/* May this context tell the block layer about a media change?  That ends
 * in disk_media_changed()/disk_set_capacity(), which take the disk
 * registry mutex -- not possible while the caller holds it already (an
 * open() under the registry mutex; the mutex is not recursive).  Holding
 * the port's eh_mutex is fine: it comes BEFORE the registry mutex in the
 * lock order (eh_mutex -> registry mutex -> port->lock -> hba->lock).
 * When notifying is not possible the change is left pending
 * (media_changed) and the worker is asked to look again at once. */
static int atapi_can_notify(struct ata_device *dev)
{
	(void)dev;
	return !blk_mutex_held(&g_disk_registry_mutex);
}

/* May this context read the new medium through the disk's request queue
 * (disk_media_changed: partition table, filesystem identity)?  Not under
 * the port's eh_mutex: error handling holds the queue stopped and the
 * holder is the one who would restart it.  Not from the disk's own
 * suspend or resume hook: the suspended queue holds ordinary requests
 * back until the hook has returned. */
static int atapi_can_read_medium(struct atapi_drive *drv,
				 struct ata_device *dev)
{
	if (ata_dev_eh_held(dev))
		return 0;
	if (drv->pm_task && drv->pm_task == sched_current())
		return 0;
	return 1;
}

/* Tell the block layer about a new medium state.  Where that must not
 * read through the queue, the registration worker does it (with the
 * state current when it runs).  Without a worker (boot) the change stays
 * pending in media_changed for the next check. */
static void atapi_notify_media(struct atapi_drive *drv, struct ata_device *dev,
			       int present)
{
	int r;

	if (atapi_can_read_medium(drv, dev)) {
		disk_media_changed(drv->disk, present);
		return;
	}
	r = ata_work_media_changed(dev);
	if (r) {
		kprintf(ATAPI_PFX "%s: media change notice deferred (%d)\n",
			atapi_dev_label(drv, dev), r);
		dev->media_changed = 1;
	}
}

/* ==========================================================================
 * Media events: the drive's events work
 *
 * Every optical drive has one piece of delayed work on kblockd
 * (drv->events_work).  It asks the drive for events -- GET EVENT STATUS
 * NOTIFICATION, polled, media class -- through the disk's request queue
 * like any other packet command, and re-arms itself every
 * dev->media_poll_ms (a drive with asynchronous notification is not
 * polled: its notification kicks the work).  Nothing of it runs in error
 * handling.  A medium change found there is acted on at once: the medium
 * is looked at with TEST UNIT READY, its capacity read, and the block
 * layer told (disk_media_changed) -- in this system the kernel is the
 * only consumer of the event.
 * ========================================================================== */

#define ATAPI_EV_MEDIA_CHANGE 0x1u
#define ATAPI_EV_EJECT_REQUEST 0x2u
/* GET EVENT STATUS NOTIFICATION and TEST UNIT READY disagreeing this many
 * times in a row: the drive's events are not believed any more. */
#define ATAPI_TUR_MISMATCH_MAX 8
/* TEST UNIT READY asked again after a unit attention, at most. */
#define ATAPI_TUR_RETRIES 3

/* Queue the drive's events work after `delay_ms' (0: now) -- never for a
 * drive going away: `dying' is set and tested under g_atapi_lock.  Any
 * context (the interrupt handler kicks it on a notification). */
static void atapi_events_queue_locked(struct atapi_drive *drv,
				      uint32_t delay_ms)
{
	int cpu;

	if (!drv || drv->dying || !blk_softirq_running())
		return;
	cpu = blk_this_cpu();
	(void)kblockd_mod_delayed_work_on(cpu < 0 ? 0u : (uint32_t)cpu,
					  &drv->events_work,
					  (uint64_t)delay_ms * 1000000ULL);
}

static void atapi_events_queue(struct atapi_drive *drv, uint32_t delay_ms)
{
	uint64_t fl;

	spin_lock_irqsave(&g_atapi_lock, &fl);
	atapi_events_queue_locked(drv, delay_ms);
	spin_unlock_irqrestore(&g_atapi_lock, fl);
}

/* The disk reference an events run holds is dropped by a separate piece
 * of work: when it is the last one, the drive -- the events work's own
 * memory -- is freed, which the run itself must not do. */
struct atapi_disk_put {
	struct atapi_disk_put *next;
	struct gendisk *disk;
};
static struct atapi_disk_put *g_atapi_puts;
static struct blk_work g_atapi_put_work;
static int g_atapi_put_work_ready;

static void atapi_disk_put_workfn(void *arg)
{
	struct atapi_disk_put *list;
	uint64_t fl;

	(void)arg;
	spin_lock_irqsave(&g_atapi_lock, &fl);
	list = g_atapi_puts;
	g_atapi_puts = NULL;
	spin_unlock_irqrestore(&g_atapi_lock, fl);
	while (list) {
		struct atapi_disk_put *next = list->next;

		disk_put(list->disk);
		kfree(list);
		list = next;
	}
}

static void atapi_disk_put_later(struct atapi_disk_put *p)
{
	uint64_t fl;

	spin_lock_irqsave(&g_atapi_lock, &fl);
	p->next = g_atapi_puts;
	g_atapi_puts = p;
	spin_unlock_irqrestore(&g_atapi_lock, fl);
	(void)kblockd_schedule_work(&g_atapi_put_work);
}

void atapi_media_event(struct ata_device *dev)
{
	uint64_t fl;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC ||
	    dev->class != ATA_DEV_ATAPI)
		return;
	/* The drive looked up and its work queued under one hold of the
	 * table lock: a drive being freed leaves the table first. */
	spin_lock_irqsave(&g_atapi_lock, &fl);
	for (int i = 0; i < DISK_MAX_SR; i++)
		if (g_atapi_drives[i] && g_atapi_drives[i]->dev == dev) {
			atapi_events_queue_locked(g_atapi_drives[i], 0);
			break;
		}
	spin_unlock_irqrestore(&g_atapi_lock, fl);
}

/* The medium changed but this context may not tell the block layer (it
 * holds the disk registry mutex): the change stays pending in
 * media_changed, and the events work, which may, runs now. */
static void atapi_defer_notify(struct ata_device *dev)
{
	dev->media_changed = 1;
	atapi_media_event(dev);
}

/* GET EVENT STATUS NOTIFICATION, polled, media class: the events the
 * drive has.  A unit attention instead of an answer is a media change. */
static unsigned int atapi_get_events(struct ata_device *dev)
{
	uint8_t cdb[12] = { GET_EVENT_STATUS_NOTIFICATION };
	uint8_t *buf;
	uint8_t key = 0;
	uint32_t resid = 0;
	unsigned int events = 0;
	int ret;

	buf = kalloc(8);
	if (!buf)
		return 0;
	kmemset(buf, 0, 8);
	cdb[1] = 0x01; /* polled */
	cdb[4] = MMC_GESN_CLASS_MEDIA;
	put_be16(cdb + 7, 8);
	ret = atapi_cmd(dev, cdb, 10, buf, 8, ATA_DIR_FROM_DEVICE,
			ATA_TMOUT_ATAPI_TUR_MS, &key, NULL, NULL, &resid);
	if (ret && key == UNIT_ATTENTION) {
		events = ATAPI_EV_MEDIA_CHANGE;
	} else if (ret == 0 && 8 - resid >= 8 && get_be16(buf) >= 4 &&
		   !(buf[2] & 0x80) &&
		   (buf[2] & 0x07) == MMC_GESN_NOTIF_MEDIA) {
		uint8_t code = buf[4] & 0x0F;

		if (code == MMC_EVT_EJECT_REQUEST)
			events = ATAPI_EV_EJECT_REQUEST;
		else if (code == MMC_EVT_NEW_MEDIA ||
			 code == MMC_EVT_MEDIA_REMOVAL)
			events = ATAPI_EV_MEDIA_CHANGE;
	}
	kfree(buf);
	return events;
}

/* The drive's events.  `clearing': the caller wants to know about a
 * medium change now (an open, a resume, a media-changed query) -- the
 * medium is then looked at with TEST UNIT READY; a poll trusts the
 * drive's events alone (some drives hang when asked TEST UNIT READY
 * again and again).  Changes noticed elsewhere (a unit attention of a
 * read, an eject) are pending in dev->media_changed and are reported
 * here. */
static unsigned int atapi_check_events(struct atapi_drive *drv,
				       struct ata_device *dev, int clearing)
{
	uint8_t key = 0, asc = 0, ascq = 0;
	uint8_t cdb[12] = { TEST_UNIT_READY };
	unsigned int events = 0;
	int last_present, ret;

	if (drv->has_gesn)
		events = atapi_get_events(dev);
	drv->get_event_changed |= (events & ATAPI_EV_MEDIA_CHANGE) != 0;

	/* A drive whose events and TEST UNIT READY kept disagreeing (or
	 * that has no usable events at all) is judged by TEST UNIT READY
	 * alone. */
	if (drv->ignore_get_event || !drv->has_gesn) {
		events &= ~ATAPI_EV_MEDIA_CHANGE;
		goto do_tur;
	}
	if (dev->media_changed) {
		events |= ATAPI_EV_MEDIA_CHANGE;
		dev->media_changed = 0;
		drv->tur_changed = 1;
	}
	if (!clearing)
		return events;
do_tur:
	last_present = dev->media_present;
	/* A unit attention is reported once each: it marks the change and
	 * TEST UNIT READY is asked again (a few times). */
	for (int tries = 0;; tries++) {
		ret = atapi_cmd(dev, cdb, 6, NULL, 0, ATA_DIR_NONE,
				ATA_TMOUT_ATAPI_TUR_MS, &key, &asc, &ascq,
				NULL);
		if (!ret || key != UNIT_ATTENTION)
			break;
		dev->media_changed = 1;
		if (tries >= ATAPI_TUR_RETRIES)
			break;
	}
	/* Present when it is ready, or when the sense says anything but
	 * "medium not present". */
	dev->media_present =
		(uint8_t)(ret == 0 ||
			  (key != 0 && asc != SAT_ASC_MEDIUM_NOT_PRESENT));
	if (last_present != dev->media_present)
		dev->media_changed = 1;
	if (dev->media_changed) {
		events |= ATAPI_EV_MEDIA_CHANGE;
		dev->media_changed = 0;
		drv->tur_changed = 1;
	}
	if (drv->ignore_get_event || !drv->has_gesn)
		return events;

	/* Do the drive's events report changes TEST UNIT READY never
	 * sees? */
	if (!drv->tur_changed) {
		if (drv->get_event_changed) {
			if (drv->tur_mismatch++ > ATAPI_TUR_MISMATCH_MAX) {
				kprintf(ATAPI_PFX "%s: GET EVENT STATUS and TEST UNIT READY disagree continuously, the events are not used any more\n",
					atapi_dev_label(drv, dev));
				drv->ignore_get_event = 1;
			}
		} else {
			drv->tur_mismatch = 0;
		}
	}
	drv->tur_changed = 0;
	drv->get_event_changed = 0;
	return events;
}

/* Act on a medium change: the capacity of the new medium (or none), the
 * block layer told.  Where this context may not tell it, the change is
 * kept pending and the events work delivers it. */
static void atapi_media_changed(struct atapi_drive *drv,
				struct ata_device *dev, int was_present)
{
	int present = dev->media_present;

	if (!drv->disk)
		return;
	if (!atapi_can_notify(dev)) {
		atapi_defer_notify(dev);
		return;
	}
	drv->n_media_changes++;
	if (present) {
		if (atapi_update_capacity(drv, dev) == 0) {
			disk_set_capacity(drv->disk, drv->capacity);
			kprintf(ATAPI_PFX "%s: media changed: %llu %u-byte blocks\n",
				atapi_dev_label(drv, dev),
				(unsigned long long)drv->capacity,
				ATAPI_BLOCK_SIZE);
		} else {
			drv->capacity = 0;
			disk_set_capacity(drv->disk, 0);
		}
		atapi_notify_media(drv, dev, 1);
	} else {
		drv->capacity = 0;
		disk_set_capacity(drv->disk, 0);
		atapi_notify_media(drv, dev, 0);
		if (was_present)
			kprintf(ATAPI_PFX "%s: media removed\n",
				atapi_dev_label(drv, dev));
	}
}

/* One check with the medium looked at (`clearing'), or a poll; a change
 * is acted on.  Returns 1 when the medium changed, 0 otherwise, -errno
 * when the drive is not usable. */
static int atapi_check_media_common(struct ata_device *dev, int clearing)
{
	struct atapi_drive *drv;
	unsigned int events;
	int was_present;

	if (!dev)
		return -EINVAL;
	if (dev->class != ATA_DEV_ATAPI)
		return -EINVAL;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (ata_can_sleep())
		might_sleep();
	drv = atapi_drive_of(dev);
	if (!drv || drv->dying)
		return -ENODEV;

	was_present = dev->media_present;
	events = atapi_check_events(drv, dev, clearing);
	/* A change found by a poll: the medium is looked at now, as an open
	 * would. */
	if ((events & ATAPI_EV_MEDIA_CHANGE) && !clearing)
		events |= atapi_check_events(drv, dev, 1);
	if (events & ATAPI_EV_EJECT_REQUEST)
		/* The eject button with the door locked by an open node: the
		 * disc stays (the user closes first). */
		kprintf(ATAPI_PFX "%s: eject button pressed%s\n",
			atapi_dev_label(drv, dev),
			dev->door_locked ? " (door locked: in use)" : "");
	if (!(events & ATAPI_EV_MEDIA_CHANGE))
		return 0;
	atapi_media_changed(drv, dev, was_present);
	return 1;
}

int atapi_check_media(struct ata_device *dev)
{
	return atapi_check_media_common(dev, 1);
}

/* The drive's events work (kblockd): a poll, then the next one.  A run
 * holds a reference to the disk -- taken under g_atapi_lock while the
 * drive is not going away -- so the drive (which the disk owns) outlives
 * it; a removal never waits for a run (its commands may wait for the
 * removal itself), the last reference frees the drive. */
static void atapi_events_workfn(void *arg)
{
	struct atapi_drive *drv = (struct atapi_drive *)arg;
	struct atapi_disk_put *ref;
	struct ata_device *dev;
	uint64_t fl;

	ref = (struct atapi_disk_put *)kalloc(sizeof(*ref));
	spin_lock_irqsave(&g_atapi_lock, &fl);
	dev = drv->dev;
	if (!ref || drv->dying || !drv->disk || !dev ||
	    (dev->flags & ATA_DFLAG_DETACHED)) {
		spin_unlock_irqrestore(&g_atapi_lock, fl);
		if (ref) {
			kfree(ref);
		} else if (!drv->dying) {
			/* no memory for this run: the next poll tries */
			atapi_events_queue(drv, ATAPI_MEDIA_POLL_MS);
		}
		return;
	}
	ref->disk = drv->disk;
	disk_get(ref->disk);
	spin_unlock_irqrestore(&g_atapi_lock, fl);

	(void)atapi_check_media_common(dev, 0);
	if (dev->media_poll_ms)
		atapi_events_queue(drv, dev->media_poll_ms);
	/* The last touch of the drive: from here on it may go. */
	atapi_disk_put_later(ref);
}

void atapi_events_start(void)
{
	uint64_t fl;

	spin_lock_irqsave(&g_atapi_lock, &fl);
	for (int i = 0; i < DISK_MAX_SR; i++) {
		struct atapi_drive *drv = g_atapi_drives[i];

		if (drv && drv->dev && drv->dev->media_poll_ms)
			atapi_events_queue_locked(drv,
						  drv->dev->media_poll_ms);
	}
	spin_unlock_irqrestore(&g_atapi_lock, fl);
}

/* ==========================================================================
 * Block I/O (any context)
 * ========================================================================== */

/* Take a free command of the ring: not marked busy, and its previous
 * completion fully returned (the transport dropped its reference).  The
 * second condition can lag the first by the few instructions at the end
 * of ata_qc_complete(); spin briefly for it. */
static int atapi_io_get(struct atapi_drive *drv)
{
	for (int spin = 0; spin < 1000; spin++) {
		uint64_t fl;
		int idx = -1;

		spin_lock_irqsave(&drv->lock, &fl);
		for (int i = 0; i < ATAPI_IO_QCS; i++) {
			if (drv->io_busy[i])
				continue;
			if (drv->io_qc[i].magic == ATA_QC_MAGIC &&
			    refcount_read(&drv->io_qc[i].refcount) > 1)
				continue;
			drv->io_busy[i] = 1;
			idx = i;
			break;
		}
		spin_unlock_irqrestore(&drv->lock, fl);
		if (idx >= 0)
			return idx;
		lapic_delay_us(1);
	}
	return -1;
}

static void atapi_io_put(struct atapi_drive *drv, struct ata_queued_cmd *qc)
{
	uint64_t fl;
	int idx = (int)(qc - drv->io_qc);

	BUG_ON(idx < 0 || idx >= ATAPI_IO_QCS);
	spin_lock_irqsave(&drv->lock, &fl);
	WARN_ON(!drv->io_busy[idx]);
	drv->io_busy[idx] = 0;
	spin_unlock_irqrestore(&drv->lock, fl);
}

/* The end_io of a block read (interrupt handler, port lock held): hand
 * the request to the block softirq, where atapi_io_complete finishes
 * it. */
static void atapi_io_end_io(struct ata_queued_cmd *qc)
{
	struct blk_request *req = qc->req;

	VM_BUG_ON(!req || !qc->private_data);
	blk_complete_request(req, qc->result, qc->resid);
}

/* Completion of a block read (block softirq, no transport lock held). */
static void atapi_io_complete(struct blk_request *req,
			      struct ata_queued_cmd *qc)
{
	struct atapi_drive *drv = (struct atapi_drive *)qc->private_data;
	struct ata_device *dev;
	int status = qc->result;
	uint32_t resid = qc->resid;

	BUG_ON(!drv || drv->magic != ATAPI_DRIVE_MAGIC || qc->req != req);
	dev = drv->dev;

	if (status) {
		uint8_t key = qc->result_tf.error >> ATAPI_ERR_SENSE_KEY_SHIFT;

		drv->n_read_errors++;
		drv->last_key = key;
		drv->last_asc = 0;
		drv->last_ascq = 0;
		if ((qc->result_tf.status & ATA_ERR) && key == UNIT_ATTENTION) {
			/* The medium may have changed under the read: the
			 * change is marked for the drive's events (reported
			 * by the next check), and the read is refused -- it
			 * may have been meant for the previous disc. */
			dev->media_changed = 1;
			status = -EIO;
		} else if ((qc->result_tf.status & ATA_ERR) &&
			   key == NOT_READY) {
			status = dev->media_present ? -EIO : -ENOMEDIUM;
		}
		if (!(req->flags & (BLK_REQ_QUIET | BLK_REQ_RAHEAD)))
			WARN_RATELIMIT(1,
				       ATAPI_PFX "%s: read of %u blocks at %llu "
						 "failed: %s (status 0x%02x "
						 "error 0x%02x, %d)",
				       atapi_dev_label(drv, dev), req->count,
				       (unsigned long long)req->sector,
				       atapi_key_name(key),
				       qc->result_tf.status,
				       qc->result_tf.error, status);
		resid = req->count * ATAPI_BLOCK_SIZE;
	}
	req->driver_data = NULL;
	atapi_io_put(drv, qc);
	blk_request_complete(req, status, resid);
}

static int atapi_submit_read(struct atapi_drive *drv, struct ata_device *dev,
			     struct blk_request *req)
{
	struct ata_queued_cmd *qc;
	uint64_t nbytes = (uint64_t)req->count * ATAPI_BLOCK_SIZE;
	uint64_t sgbytes = 0;
	uint8_t cdb[16];
	int cdb_len, idx, ret;

	if (req->count == 0 || !req->sg || req->nsg == 0)
		return -EINVAL;
	if (!dev->media_present)
		return -ENOMEDIUM;
	if (req->sector >= drv->capacity ||
	    req->count > drv->capacity - req->sector)
		return -EINVAL;
	for (uint32_t i = 0; i < req->nsg; i++)
		sgbytes += req->sg[i].len;
	if (unlikely(sgbytes != nbytes)) {
		WARN_RATELIMIT(1,
			       ATAPI_PFX "%s: read of %u blocks with %llu sg "
					 "bytes",
			       atapi_dev_label(drv, dev), req->count,
			       (unsigned long long)sgbytes);
		return -EINVAL;
	}
	if (nbytes > 0xFFFFFFFFull)
		return -EINVAL;

	kmemset(cdb, 0, sizeof(cdb));
	if (req->sector <= 0xFFFFFFFFull && req->count <= 0xFFFF) {
		cdb[0] = READ_10;
		put_be32(cdb + 2, (uint32_t)req->sector);
		put_be16(cdb + 7, (uint16_t)req->count);
		cdb_len = 10;
	} else if (req->sector <= 0xFFFFFFFFull) {
		cdb[0] = READ_12;
		put_be32(cdb + 2, (uint32_t)req->sector);
		put_be32(cdb + 6, req->count);
		cdb_len = 12;
	} else {
		return -EINVAL; /* no optical medium is that large */
	}

	idx = atapi_io_get(drv);
	if (idx < 0) {
		WARN_RATELIMIT(1, ATAPI_PFX "%s: no free I/O command",
			       atapi_dev_label(drv, dev));
		return -EBUSY;
	}
	qc = &drv->io_qc[idx];
	ata_qc_init(qc, dev);
	qc->sg = req->sg;
	qc->n_elem = req->nsg;
	qc->nbytes = (uint32_t)nbytes;
	qc->flags |= ATA_QCFLAG_IO | ATA_QCFLAG_RESULT_TF |
		     ((req->flags & BLK_REQ_FAILFAST) ? 0 : ATA_QCFLAG_RETRY);
	if (req->flags & BLK_REQ_POLLED)
		qc->flags |= ATA_QCFLAG_POLLED;
	if (req->flags & BLK_REQ_QUIET)
		qc->flags |= ATA_QCFLAG_QUIET;
	qc->max_retries = (req->flags & BLK_REQ_FAILFAST) ? 0 : ATA_MAX_RETRIES;
	/* Retries error handling asked for (the request came back through
	 * the queue) count on. */
	qc->retries = req->retries;
	qc->timeout_ms = req->timeout_ms ? req->timeout_ms : ATA_TMOUT_ATAPI_MS;
	qc->lba = req->sector;
	qc->n_sect = req->count;
	qc->req = req;
	qc->end_io = atapi_io_end_io;
	qc->private_data = drv;
	atapi_prep_packet(dev, qc, cdb, cdb_len, ATA_DIR_FROM_DEVICE);
	req->driver_data = qc;

	ret = dev->xport->qc_issue(qc);
	if (ret == 0) {
		drv->n_reads++;
		return 0;
	}
	req->driver_data = NULL;
	atapi_io_put(drv, qc);
	if (ret == -EAGAIN)
		return -EBUSY; /* port in error handling: requeue */
	return ret;
}

/* A prepared pass-through command (atapi_issue_sync) from the queue. */
static int atapi_submit_drv(struct atapi_drive *drv, struct ata_device *dev,
			    struct blk_request *req)
{
	struct ata_queued_cmd *qc = (struct ata_queued_cmd *)req->drv_cmd;
	int ret;

	if (!qc || qc->magic != ATA_QC_MAGIC || qc->dev != dev) {
		WARN_RATELIMIT(1, ATAPI_PFX "%s: bad pass-through request",
			       atapi_dev_label(drv, dev));
		return -EINVAL;
	}
	/* A retry error handling asked for issues the command again: the
	 * previous completion's marks go (its retry count stays). */
	if (qc->flags & ATA_QCFLAG_COMPLETE) {
		qc->flags &= ~(ATA_QCFLAG_COMPLETE | ATA_QCFLAG_FAILED |
			       ATA_QCFLAG_TIMEDOUT | ATA_QCFLAG_ABORTED |
			       ATA_QCFLAG_EH_RETRY);
		qc->result = 0;
		qc->err_mask = 0;
		qc->resid = 0;
		qc->tag = -1;
		kmemset(&qc->result_tf, 0, sizeof(qc->result_tf));
		if (ata_completion_done(&qc->done))
			ata_completion_reinit(&qc->done);
	}
	qc->req = req;
	qc->end_io = atapi_drv_end_io;
	qc->private_data = drv;
	req->driver_data = qc;
	ret = dev->xport->qc_issue(qc);
	if (ret == 0)
		return 0;
	req->driver_data = NULL;
	if (ret == -EAGAIN)
		return -EBUSY;
	return ret;
}

static int atapi_blk_submit(struct gendisk *disk, struct blk_request *req)
{
	struct ata_device *dev;
	struct atapi_drive *drv;

	VM_BUG_ON(!disk || disk->magic != GENDISK_MAGIC);
	VM_BUG_ON(!req || req->magic != BLK_REQUEST_MAGIC);
	dev = (struct ata_device *)disk->private_data;
	if (unlikely(!dev || dev->magic != ATA_DEVICE_MAGIC))
		return -ENODEV;
	if (unlikely(dev->flags & ATA_DFLAG_DETACHED))
		return -ENODEV;
	drv = atapi_drive_of(dev);
	if (unlikely(!drv || drv->dying))
		return -ENODEV;

	switch (req->op) {
	case BLK_OP_READ:
		return atapi_submit_read(drv, dev, req);
	case BLK_OP_DRV_IN:
	case BLK_OP_DRV_OUT:
		return atapi_submit_drv(drv, dev, req);
	case BLK_OP_FLUSH:
		/* Nothing is cached for writing on a read-only device. */
		blk_request_complete(req, 0, 0);
		return 0;
	case BLK_OP_WRITE:
	case BLK_OP_DISCARD:
	case BLK_OP_SECURE_ERASE:
	case BLK_OP_WRITE_ZEROES:
		return -EROFS;
	default:
		return -EOPNOTSUPP;
	}
}

/* Requeues of one request after recovery cancelled it before it fails. */
#define ATAPI_REQUEUE_MAX 1000

/* The block softirq's half of a completion (ops->complete). */
static void atapi_blk_complete(struct blk_request *req)
{
	struct ata_queued_cmd *qc = (struct ata_queued_cmd *)req->driver_data;

	BUG_ON(!qc || qc->magic != ATA_QC_MAGIC || qc->req != req);
	if ((qc->flags & ATA_QCFLAG_EH_RETRY) ||
	    (qc->result == -ECANCELED && req->retries < ATAPI_REQUEUE_MAX)) {
		/* Error handling ended the command for a retry, or recovery
		 * cancelled it (requeue): the request goes back to the queue
		 * and is dispatched again (a read gets a fresh command, a
		 * pass-through its own once more). */
		if (req->op != BLK_OP_DRV_IN && req->op != BLK_OP_DRV_OUT)
			atapi_io_put((struct atapi_drive *)qc->private_data, qc);
		blk_queue_requeue(req->queue, req);
		return;
	}
	if (req->op == BLK_OP_DRV_IN || req->op == BLK_OP_DRV_OUT)
		atapi_drv_complete(req, qc);
	else
		atapi_io_complete(req, qc);
}

/* The queue's timeout work found the request expired (ops->timeout). */
static enum blk_eh_timer_return atapi_blk_timeout(struct gendisk *disk,
						  struct blk_request *req,
						  void *driver_data)
{
	struct ata_device *dev = (struct ata_device *)disk->private_data;

	(void)driver_data;
	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !dev->xport ||
	    !dev->xport->qc_timed_out)
		return BLK_EH_RESET_TIMER;
	(void)dev->xport->qc_timed_out(dev, NULL, req);
	return BLK_EH_DONE;
}

static int atapi_blk_poll(struct gendisk *disk)
{
	struct ata_device *dev = (struct ata_device *)disk->private_data;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !dev->xport ||
	    !dev->xport->poll)
		return 0;
	return dev->xport->poll(dev);
}

/* ==========================================================================
 * The rest of the block operations (process context)
 * ========================================================================== */

static struct atapi_drive *atapi_disk_drive(struct gendisk *disk)
{
	struct ata_device *dev;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return NULL;
	dev = (struct ata_device *)disk->private_data;
	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return NULL;
	return atapi_drive_of(dev);
}

static int atapi_blk_open(block_device_t *bdev, int flags)
{
	struct atapi_drive *drv;
	struct ata_device *dev;
	int ret;

	drv = atapi_disk_drive(bdev ? bdev->disk : NULL);
	if (!drv)
		return -ENODEV;
	dev = drv->dev;
	if (ata_can_sleep())
		might_sleep();
	if ((flags & 3) == O_WRONLY || (flags & 3) == O_RDWR)
		return -EROFS;

	/* Look at the medium now: a disc inserted since the last poll must
	 * be visible to this open. */
	ret = atapi_check_media(dev);
	if (ret < 0 && ret != -ENOMEDIUM)
		kprintf(ATAPI_PFX "%s: media check on open failed (%d)\n",
			atapi_dev_label(drv, dev), ret);
	if (!dev->media_present && !(flags & O_NONBLOCK))
		return -ENOMEDIUM;

	ata_mutex_lock(&drv->cmd_mutex);
	drv->open_count++;
	ata_mutex_unlock(&drv->cmd_mutex);
	/* First opener with a disc: keep it in the drive while in use. */
	if (drv->open_count == 1 && dev->media_present && drv->lockable &&
	    !dev->door_locked) {
		if (atapi_lock_door(dev, 1) == 0)
			drv->door_locked_open = 1;
	}
	return 0;
}

static void atapi_blk_release(block_device_t *bdev)
{
	struct atapi_drive *drv;
	struct ata_device *dev;
	int last;

	drv = atapi_disk_drive(bdev ? bdev->disk : NULL);
	if (!drv)
		return;
	dev = drv->dev;
	ata_mutex_lock(&drv->cmd_mutex);
	WARN_ON(drv->open_count <= 0);
	if (drv->open_count > 0)
		drv->open_count--;
	last = drv->open_count == 0;
	ata_mutex_unlock(&drv->cmd_mutex);
	if (last && drv->door_locked_open && !(dev->flags & ATA_DFLAG_DETACHED)) {
		atapi_lock_door(dev, 0);
		drv->door_locked_open = 0;
	}
}

static int atapi_blk_check_events(struct gendisk *disk)
{
	struct atapi_drive *drv = atapi_disk_drive(disk);

	if (!drv)
		return -ENODEV;
	return atapi_check_media(drv->dev);
}

static int atapi_blk_revalidate(struct gendisk *disk)
{
	struct atapi_drive *drv = atapi_disk_drive(disk);
	int ret;

	if (!drv)
		return -ENODEV;
	if (!drv->dev->media_present) {
		drv->capacity = 0;
		return disk_set_capacity(disk, 0);
	}
	ret = atapi_update_capacity(drv, drv->dev);
	if (ret)
		return ret;
	return disk_set_capacity(disk, drv->capacity);
}

static int atapi_blk_shutdown(struct gendisk *disk,
			      enum blk_shutdown_mode mode)
{
	struct atapi_drive *drv = atapi_disk_drive(disk);

	(void)mode;
	if (!drv)
		return 0;
	/* Leave the tray usable after the system is gone. */
	if (drv->dev->door_locked && !(drv->dev->flags & ATA_DFLAG_DETACHED))
		atapi_lock_door(drv->dev, 0);
	drv->door_locked_open = 0;
	return 0;
}

static int atapi_blk_suspend(struct gendisk *disk)
{
	struct atapi_drive *drv = atapi_disk_drive(disk);
	uint8_t cdb[12] = { START_STOP };
	int ret;

	if (!drv)
		return -ENODEV;
	/* The queue is suspended already: our command passes as a
	 * power-management request (atapi_issue_sync). */
	drv->pm_task = sched_current();
	/* Stop the spindle (Start = 0, LoEj = 0); the disc stays in. */
	cdb[4] = 0x00;
	ret = atapi_cmd(drv->dev, cdb, 6, NULL, 0, ATA_DIR_NONE,
			ATA_TMOUT_ATAPI_MS, NULL, NULL, NULL, NULL);
	drv->pm_task = NULL;
	return ret == -ENODEV ? -ENODEV : 0;
}

static int atapi_blk_resume(struct gendisk *disk)
{
	struct atapi_drive *drv = atapi_disk_drive(disk);
	uint8_t cdb[12] = { START_STOP };

	if (!drv)
		return -ENODEV;
	/* The queue stays suspended until we return: our packet commands
	 * pass as power-management requests, and a medium change found
	 * here is announced by the registration worker, which reads the
	 * new disc once the queue runs again (atapi_notify_media). */
	drv->pm_task = sched_current();
	if (drv->dev->media_present) {
		cdb[4] = 0x01; /* Start */
		atapi_cmd(drv->dev, cdb, 6, NULL, 0, ATA_DIR_NONE,
			  ATA_TMOUT_ATAPI_MS, NULL, NULL, NULL, NULL);
	}
	/* The disc may have been exchanged while suspended. */
	drv->dev->media_changed = 1;
	atapi_check_media(drv->dev);
	if (drv->door_locked_open)
		atapi_lock_door(drv->dev, 1);
	drv->pm_task = NULL;
	return 0;
}

static void atapi_drive_free(struct atapi_drive *drv)
{
	/* Every block command must have come home: the queue was destroyed
	 * (it waits for in-flight requests) before the last disk reference
	 * went away.  A completion may still be on its last steps after the
	 * request ended (waking the waiter, dropping the transport's
	 * reference) -- in another task, which can be preempted for any
	 * length of time: where we may sleep, wait for it as long as it
	 * takes, yielding; otherwise (no scheduler, interrupts off) bounded,
	 * as there is nobody to yield to. */
	for (int i = 0; i < ATAPI_IO_QCS; i++) {
		int can_sleep = ata_can_sleep();
		uint64_t spin = 0;

		while (drv->io_qc[i].magic == ATA_QC_MAGIC &&
		       refcount_read(&drv->io_qc[i].refcount) > 1) {
			if (!can_sleep && spin >= 1000)
				break;
			if (can_sleep) {
				sched_yield_in_kernel();
				if (spin == 100000)
					WARN(1, "[atapi] %s: command %d still referenced at release",
					     drv->dev ? drv->dev->name : "?", i);
			} else {
				lapic_delay_us(10);
			}
			spin++;
		}
		WARN_ON(drv->io_busy[i]);
	}
	/* The events work: never queued again (`dying', and off the table
	 * the notification looks it up in), nothing pending any more, and a
	 * run that dropped the last reference has returned (bounded: it ends
	 * right after handing its disk reference over). */
	{
		uint64_t fl;

		spin_lock_irqsave(&g_atapi_lock, &fl);
		drv->dying = 1;
		spin_unlock_irqrestore(&g_atapi_lock, fl);
	}
	atapi_drive_table_del(drv);
	(void)blk_work_cancel_sync(&drv->events_work, 1000);
	drv->magic = 0;
	kfree(drv);
}

static void atapi_blk_free_disk(struct gendisk *disk)
{
	struct ata_device *dev = (struct ata_device *)disk->private_data;
	struct atapi_drive *drv = atapi_disk_drive(disk);

	if (drv) {
		drv->disk = NULL;
		atapi_drive_free(drv);
	}
	disk->private_data = NULL;
	if (dev && dev->magic == ATA_DEVICE_MAGIC) {
		/* A disk the worker failed to add goes here without an
		 * unregister: the device must not keep pointing at it. */
		if (dev->disk == disk)
			dev->disk = NULL;
		ata_dev_put(dev);
	}
}

const struct blk_ops atapi_blk_ops = {
	.name = "atapi",
	.submit = atapi_blk_submit,
	.complete = atapi_blk_complete,
	.timeout = atapi_blk_timeout,
	.poll = atapi_blk_poll,
	.ioctl = ata_sat_blk_ioctl,
	.ioctl_mode = ata_sat_blk_ioctl_mode,
	.open = atapi_blk_open,
	.release = atapi_blk_release,
	.check_events = atapi_blk_check_events,
	.revalidate = atapi_blk_revalidate,
	.shutdown = atapi_blk_shutdown,
	.suspend = atapi_blk_suspend,
	.resume = atapi_blk_resume,
	.free_disk = atapi_blk_free_disk,
};

/* ==========================================================================
 * Registration
 * ========================================================================== */

/* IDENTIFY PACKET DEVICE -> packet size, DMA, DMADIR. */
static int atapi_parse_identify(struct ata_device *dev)
{
	const uint16_t *id = dev->id;
	uint16_t cfg = id[ATA_ID_CONFIG];

	if (!ata_id_is_atapi(id)) {
		kprintf(ATAPI_PFX "%s: IDENTIFY PACKET word 0 0x%04x is not an "
				  "ATAPI device\n",
			dev->name, cfg);
		return -EINVAL;
	}
	if (cfg & ATA_ID_CONFIG_INCOMPLETE)
		kprintf(ATAPI_PFX "%s: IDENTIFY PACKET response incomplete\n",
			dev->name);

	switch (cfg & 0x3) {
	case 0:
		dev->cdb_len = 12;
		break;
	case 1:
		dev->cdb_len = 16;
		dev->flags |= ATA_DFLAG_CDB16;
		break;
	default:
		kprintf(ATAPI_PFX "%s: reserved packet size code %u, using 12 "
				  "bytes\n",
			dev->name, cfg & 0x3);
		dev->cdb_len = 12;
		break;
	}
	if (cfg & ATA_ID_CONFIG_REMOVABLE)
		dev->flags |= ATA_DFLAG_REMOVABLE;

	/* DMA: word 49 bit 8.  Word 62 bit 15: the drive needs DMADIR. */
	if (id[ATA_ID_CAPABILITY] & ATA_ID_CAP_DMA) {
		dev->flags |= ATA_DFLAG_ATAPI_DMA;
		if (id[ATA_ID_SWDMA_MODES] != 0xFFFF &&
		    (id[ATA_ID_SWDMA_MODES] & 0x8000))
			dev->flags |= ATA_DFLAG_ATAPI_DMADIR;
	}
	return 0;
}

int atapi_dev_register(struct ata_device *dev)
{
	struct atapi_drive *drv;
	struct gendisk *disk;
	uint8_t pkt_set;
	uint32_t max_blocks;
	int ret;

	if (!dev)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (dev->class != ATA_DEV_ATAPI)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	if (atapi_drive_of(dev)) {
		WARN_ON_ONCE(1); /* registered twice */
		return -EEXIST;
	}

	ret = atapi_parse_identify(dev);
	if (ret)
		return ret;
	pkt_set = (uint8_t)ATA_ID_CONFIG_PKT_SET(dev->id[ATA_ID_CONFIG]);

	drv = kcalloc(1, sizeof(*drv));
	if (!drv)
		return -ENOMEM;
	drv->magic = ATAPI_DRIVE_MAGIC;
	drv->dev = dev;
	spinlock_init(&drv->lock, "atapi_drive");
	ata_mutex_init(&drv->cmd_mutex, "atapi_cmd");
	blk_work_init(&drv->events_work, atapi_events_workfn, drv);
	{
		uint64_t fl;

		spin_lock_irqsave(&g_atapi_lock, &fl);
		if (!g_atapi_put_work_ready) {
			blk_work_init(&g_atapi_put_work, atapi_disk_put_workfn,
				      NULL);
			g_atapi_put_work_ready = 1;
		}
		spin_unlock_irqrestore(&g_atapi_lock, fl);
	}
	/* Registration runs with the port's eh_mutex held (probe, hot-plug):
	 * our commands are error-handling-context commands until the disk
	 * is up. */
	drv->probe_qcflags = ATA_QCFLAG_EH;
	ret = atapi_drive_table_add(drv);
	if (ret) {
		kprintf(ATAPI_PFX "%s: too many optical drives\n", dev->name);
		kfree(drv);
		return ret;
	}

	/* Wait for the unit; clears the power-on unit attention. */
	ret = atapi_wait_ready(dev, ATAPI_READY_WAIT_MS);
	if (ret == -ENOMEDIUM || ret == -ETIMEDOUT) {
		dev->media_present = 0;
	} else if (ret == 0) {
		dev->media_present = 1;
	} else {
		kprintf(ATAPI_PFX "%s: TEST UNIT READY failed (%d); continuing\n",
			dev->name, ret);
		dev->media_present = 0;
	}

	ret = atapi_inquiry(drv, dev);
	if (ret) {
		kprintf(ATAPI_PFX "%s: INQUIRY failed (%d)\n", dev->name, ret);
		goto fail;
	}
	if (drv->inq_type != TYPE_ROM && drv->inq_type != TYPE_WORM) {
		kprintf(ATAPI_PFX "%s: ATAPI peripheral type %u (packet set "
				  "%u, %s %s) is not an optical drive; not "
				  "supported\n",
			dev->name, drv->inq_type, pkt_set, drv->vendor,
			drv->product);
		ret = -EOPNOTSUPP;
		goto fail;
	}
	if (pkt_set != ATA_ID_CONFIG_PKT_CDROM)
		kprintf(ATAPI_PFX "%s: packet set %u reported for a CD/DVD "
				  "device\n",
			dev->name, pkt_set);

	atapi_get_configuration(drv, dev);
	atapi_mode_sense_caps(drv, dev);
	if (!drv->has_gc && !drv->lockable && !drv->ejectable) {
		/* Neither source answered: assume the common tray drive. */
		drv->lockable = 1;
		drv->ejectable = 1;
		drv->mech_type = 1;
	}
	{
		uint8_t ev;
		int pr, to;

		drv->has_gesn = atapi_gesn(dev, &ev, &pr, &to) == 0;
	}
	if (dev->media_present && atapi_update_capacity(drv, dev) != 0) {
		/* A disc the drive cannot read (blank, damaged): treat as
		 * absent for the block device. */
		drv->capacity = 0;
	}

	disk = disk_alloc(BLK_DISK_SR, &atapi_blk_ops, dev);
	if (!disk) {
		ret = -ENOMEM;
		goto fail;
	}
	ksnprintf(drv->name, sizeof(drv->name), "%s", disk->disk_name);

	/* limits (counts in 2048-byte blocks) */
	max_blocks = dev->host_max_sectors
			     ? dev->host_max_sectors / (ATAPI_BLOCK_SIZE / 512)
			     : 64;
	if (max_blocks == 0)
		max_blocks = 1;
	if (max_blocks > 0xFFFF)
		max_blocks = 0xFFFF;
	kmemset(&disk->limits, 0, sizeof(disk->limits));
	disk->limits.logical_block_size = ATAPI_BLOCK_SIZE;
	disk->limits.physical_block_size = ATAPI_BLOCK_SIZE;
	disk->limits.io_min = ATAPI_BLOCK_SIZE;
	disk->limits.io_opt = 0;
	disk->limits.alignment_offset = 0;
	disk->limits.max_hw_sectors = max_blocks;
	disk->limits.max_sectors = max_blocks;
	disk->limits.max_segments =
		dev->host_max_segments ? dev->host_max_segments : AHCI_MAX_PRD;
	disk->limits.max_segment_size = AHCI_PRD_MAX_BYTES;
	disk->limits.seg_boundary_mask = 0xFFFFFFFFull;
	disk->limits.dma_alignment =
		dev->host_dma_alignment ? dev->host_dma_alignment : 1;
	disk->limits.queue_depth = 1;
	disk->limits.rotational = 1;
	disk->limits.write_cache = 0;
	disk->limits.fua = 0;
	disk->limits.discard_zeroes_data = 0;
	disk->limits.secure_erase = 0;
	disk->capacity = dev->media_present ? drv->capacity : 0;

	disk->flags |= GENHD_FL_REMOVABLE | GENHD_FL_NO_PART_SCAN | GENHD_FL_RO;
	if (dev->media_present && drv->capacity)
		disk->flags |= GENHD_FL_MEDIA_PRESENT;
	ksnprintf(disk->vendor, sizeof(disk->vendor), "%-8.8s", drv->vendor);
	ksnprintf(disk->model, sizeof(disk->model), "%s",
		  dev->model[0] ? dev->model : drv->product);
	ksnprintf(disk->serial, sizeof(disk->serial), "%s", dev->serial);
	ksnprintf(disk->firmware, sizeof(disk->firmware), "%s",
		  dev->firmware[0] ? dev->firmware : drv->revision);
	disk->wwn[0] = '\0';
	ksnprintf(disk->bus, sizeof(disk->bus), "ata");
	disk->host_no = dev->port_no > 0 ? dev->port_no - 1 : 0;
	disk->channel = 0;
	disk->target = dev->devno;
	disk->lun = 0;
	disk->scsi_type = TYPE_ROM;
	disk->cylinders = 0;
	disk->heads = 0;
	disk->sectors_per_track = 0;
	{
		/* the controller, for /sys links and by-path */
		struct ahci_link *link = (struct ahci_link *)dev->xport_priv;

		if (link && link->port && link->port->hba)
			disk->pci = link->port->hba->pci;
	}

	ata_dev_get(dev); /* the disk's reference, dropped in free_disk */
	drv->disk = disk;
	dev->disk = disk;
	/* A drive that notifies by itself is not polled; the others every
	 * ATAPI_MEDIA_POLL_MS.  At boot the polling starts with the storage
	 * stack's threads (atapi_events_start). */
	dev->media_poll_ms = (dev->flags & ATA_DFLAG_AN) ? 0
							  : ATAPI_MEDIA_POLL_MS;
	if (dev->media_poll_ms)
		atapi_events_queue(drv, dev->media_poll_ms);

	/* At run time the disk is added by the registration worker, as an
	 * ATA disk is: disk_add() reads the medium (filesystem identity)
	 * through the disk's queue, and we run with the port's eh_mutex
	 * held -- often on the error handling thread itself, which a read
	 * that has to wait for the port would then wait on.  At boot (no
	 * scheduler, no worker) it is added right here, so the drive exists
	 * when the storage bootstrap looks. */
	dev->flags |= ATA_DFLAG_REGISTERED;
	ret = ata_work_disk_add(dev, disk);
	if (ret) {
		kprintf(ATAPI_PFX "%s: adding %s failed (%d)\n", dev->name,
			drv->name, ret);
		dev->flags &= ~ATA_DFLAG_REGISTERED;
		dev->disk = NULL;
		drv->disk = NULL;
		/* disk_put frees through free_disk, which drops the device
		 * reference and the drive. */
		disk_put(disk);
		return ret;
	}
	drv->probe_qcflags = 0;
	atapi_log_drive(drv, dev);
	return 0;

fail:
	atapi_drive_table_del(drv);
	drv->magic = 0;
	kfree(drv);
	return ret;
}

void atapi_dev_unregister(struct ata_device *dev)
{
	struct atapi_drive *drv;
	struct gendisk *disk;

	if (!dev)
		return;
	if (ata_can_sleep())
		might_sleep();
	drv = atapi_drive_of(dev);
	if (!drv)
		return;
	{
		uint64_t fl;

		/* No events run starts on a drive going away, and none is
		 * queued again (both test `dying' under the lock). */
		spin_lock_irqsave(&g_atapi_lock, &fl);
		drv->dying = 1;
		spin_unlock_irqrestore(&g_atapi_lock, fl);
	}
	disk = drv->disk;
	dev->flags &= ~ATA_DFLAG_REGISTERED;
	dev->media_poll_ms = 0;
	/* The pending events run is taken back; one in progress is not
	 * waited for (its commands may wait for this removal): it holds the
	 * disk, and the drive goes with the last reference. */
	(void)blk_work_cancel_sync(&drv->events_work, 0);
	if (disk) {
		kprintf(ATAPI_PFX "%s: removing %s (%llu reads, %llu errors, "
				  "%llu media changes)\n",
			dev->name, drv->name, (unsigned long long)drv->n_reads,
			(unsigned long long)drv->n_read_errors,
			(unsigned long long)drv->n_media_changes);
		/* Through the registration worker, like the add: an add
		 * still pending is dropped there (never added), a live disk
		 * is deleted there -- disk_del fails what is queued, waits
		 * for what is in flight and drops the registry reference;
		 * the drive object goes with the last disk reference
		 * (free_disk).  Synchronous without a worker (boot). */
		ata_work_disk_del(dev);
		dev->disk = NULL;
	} else {
		atapi_drive_free(drv);
	}
}
