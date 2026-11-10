// LikeOS ATA layer - definitions private to the device core
//
// ata.h is the contract every part of the storage stack codes against.
// This header is what the four files of the ATA device core share with
// each other and with nobody else:
//
//   ata_core.c      device lifecycle, IDENTIFY, queued commands, ata_mutex,
//                   the device quirk table, the deferred registration worker
//   ata_rw.c        block requests -> ATA commands, the disk's blk_ops
//   ata_features.c  SET FEATURES, logs, diagnostics, the configuration policy
//   ata_pm.c        power management, suspend/resume/shutdown
//
// Nothing outside these four files includes it.  The structures here are
// the per-device private state the core keeps next to struct ata_device
// (the device is allocated as one block: the public struct first, the
// private state behind it -- see struct ata_dev_full), the request
// context of a block request in flight, and the handful of cross-file
// helpers.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_ATA_ATA_DEV_INTERNAL_H
#define KERNEL_DEV_ATA_ATA_DEV_INTERNAL_H

#include <kernel/dev/ata/ata.h>

/* ==========================================================================
 * Private bits of struct ata_queued_cmd.flags
 *
 * ata.h hands out bits 0..23.  The ATA core keeps bits 24..31 for itself to
 * remember where a command's memory came from, which decides what the last
 * ata_qc_put() does with it.  No transport ever looks at these bits.
 * ========================================================================== */

#define ATA_QCFLAG_ORIGIN_HEAP (1u << 28) /* ata_qc_alloc(): kfree at 0 */
#define ATA_QCFLAG_ORIGIN_POOL (1u << 29) /* device pool: back to pool at 0 */
/* ata_qc_exec() waits on this command: ata_qc_complete() then sets
 * SETTLED as its very last access, after which the waiter may let the
 * memory go (a command on the waiter's stack, or freed right after). */
#define ATA_QCFLAG_WAITER (1u << 26)
#define ATA_QCFLAG_SETTLED (1u << 27)
#define ATA_QCFLAG_CORE_MASK 0xFF000000u

/* Grace on top of a command's own timeout before ata_qc_exec() concludes
 * that the transport lost the command (its own timeout handling should
 * have completed it long before). */
#define ATA_EXEC_GRACE_MS 3000
/* After asking the transport to reset a stuck command away, how long to
 * wait for the command to come back. */
#define ATA_EXEC_RESET_WAIT_MS 10000

/* ==========================================================================
 * IDENTIFY word 2 ("specific configuration", ATA8-ACS and later)
 *
 * A device that powered up in standby (PUIS) may answer IDENTIFY with an
 * incomplete page; word 2 then says whether it also needs SET FEATURES
 * spin-up (07h) before it accepts media access.
 * ========================================================================== */

#define ATA_ID_SPECCFG_SPINUP_INCOMPLETE 0x37C8 /* needs 07h, page partial */
#define ATA_ID_SPECCFG_SPINUP_COMPLETE 0x738C /* needs 07h, page complete */
#define ATA_ID_SPECCFG_INCOMPLETE 0x8C73 /* no 07h needed, page partial */
#define ATA_ID_SPECCFG_COMPLETE 0xC837 /* no 07h needed, page complete */

/* ==========================================================================
 * Device quirks
 *
 * Firmware that claims a feature it does not implement correctly.  The
 * table lives in ata_core.c and is matched against the model (and
 * optionally the firmware revision) with '*' / '?' wildcards.  The lookup
 * is pure, so it is called wherever a decision depends on it instead of
 * storing the result.
 * ========================================================================== */

#define ATA_QUIRK_NO_NCQ (1u << 0) /* NCQ broken: queue depth 1 */
#define ATA_QUIRK_NO_NCQ_TRIM (1u << 1) /* queued TRIM corrupts data */
#define ATA_QUIRK_NO_TRIM (1u << 2) /* TRIM broken altogether */
#define ATA_QUIRK_ZERO_AFTER_TRIM (1u << 3) /* RZAT verified trustworthy */
#define ATA_QUIRK_MAX_SEC_128 (1u << 4) /* at most 128 sectors/command */
#define ATA_QUIRK_MAX_SEC_1024 (1u << 5) /* at most 1024 sectors/command */
#define ATA_QUIRK_NO_FUA (1u << 6) /* FUA writes broken */
#define ATA_QUIRK_NO_DIPM (1u << 7) /* DIPM makes the link drop */
#define ATA_QUIRK_NO_LOG_DMA (1u << 8) /* READ LOG DMA EXT broken */
#define ATA_QUIRK_BROKEN_HPA (1u << 9) /* READ NATIVE MAX misbehaves */
#define ATA_QUIRK_NO_FPDMA_AA (1u << 10) /* DMA setup auto-activate broken */
#define ATA_QUIRK_NO_SETXFER (1u << 11) /* SET FEATURES XFER hangs it */
#define ATA_QUIRK_NO_APM (1u << 12) /* APM requests misbehave */

uint32_t ata_quirks_lookup(const char *model, const char *firmware);
/* The quirks of a device (its parsed model/firmware).  Pure. */
static inline uint32_t ata_dev_quirks(const struct ata_device *dev)
{
	return ata_quirks_lookup(dev->model, dev->firmware);
}

/* ==========================================================================
 * Block request context (ata_rw.c)
 *
 * One block request in flight is one struct ata_io.  A request may need
 * several ATA commands -- a flush before the data (PREFLUSH), the data in
 * chunks of at most max_sectors / the PRD table's reach, a flush after it
 * (FUA emulation), a FLUSH CACHE re-issued after a write error per the ATA
 * rules, a TRIM spanning several DSM payloads -- and those run one after
 * the other.  The completion of one (block softirq, ata_blk_complete)
 * gives the request back to the head of its queue (blk_queue_continue)
 * with the context attached, and kblockd dispatches it again: ->submit
 * finds the context and issues the next step.  When a step cannot be
 * issued (no slot, a non-queued command next to queued ones, port in
 * error handling, device asleep) ->submit answers -EBUSY and the queue
 * retries after the next completion -- dispatching nothing behind it
 * meanwhile -- or, with nothing in flight, shortly after.
 * ========================================================================== */

#define ATA_IO_POOL 64 /* contexts per device (>= 2 x NCQ depth) */
#define ATA_QC_POOL 64 /* queued commands per device for block I/O */
#define ATA_IO_CHUNK_SG 32 /* sg elements of one chunk (split request) */
#define ATA_DSM_BUFS 4 /* TRIM payload buffers per device */
#define ATA_DSM_BUF_BLOCKS_MAX 8 /* 512-byte blocks per payload, at most */
#define ATA_ZERO_BUF_BYTES 65536 /* zeroes for WRITE ZEROES by writes */
#define ATA_FLUSH_MAX_TRIES 32 /* FLUSH CACHE re-issues after errors */
#define ATA_PRD_MAX_BYTES (4u * 1024 * 1024) /* one PRD entry, AHCI */
/* -ECANCELED from the transport means "requeue" (HBA recovery, re-issues
 * that could not run).  A request is re-issued that often before the
 * driver concludes recovery is looping and fails it with -EIO. */
#define ATA_IO_REQUEUE_MAX 1000
/* Internal synchronous commands re-issued after -ECANCELED. */
#define ATA_EXEC_REQUEUE_MAX 8

/* What a context still has to do, in order (bit mask, lowest first). */
#define ATA_IO_STEP_DATA (1u << 1) /* READ / WRITE chunks */
#define ATA_IO_STEP_ZERO (1u << 2) /* WRITE ZEROES as writes */
#define ATA_IO_STEP_TRIM (1u << 3) /* DSM TRIM payloads */
#define ATA_IO_STEP_FLUSH (1u << 4) /* the request is a FLUSH */
#define ATA_IO_STEP_DRV (1u << 6) /* pass-through command */

struct ata_io {
	uint32_t magic; /* ATA_IO_MAGIC while in use */
	uint32_t index; /* slot in the device's pool */
	struct ata_device *dev;
	struct blk_request *req;
	uint32_t steps; /* ATA_IO_STEP_* still to run */
	uint32_t cur_step; /* the step of the command in flight */
	int write; /* data direction of DATA/ZERO */
	int fua_native; /* FUA carried by the write itself */
	uint32_t lss; /* logical sector size, bytes */

	/* position */
	uint64_t lba; /* next absolute sector to process */
	uint64_t remaining; /* sectors still to process */
	uint32_t sg_idx; /* data cursor: element ... */
	uint32_t sg_off; /* ... and byte offset in it */
	uint32_t chunk_sectors; /* sectors of the command in flight */
	uint32_t chunk_bytes;
	uint32_t chunk_next_idx; /* cursor after the chunk in flight */
	uint32_t chunk_next_off;
	struct blk_sg_entry chunk_sg[ATA_IO_CHUNK_SG];

	/* TRIM */
	void *dsm_buf; /* payload buffer (from the device's set) */
	int dsm_slot; /* its index, -1 none */
	uint32_t dsm_blocks; /* payload size in 512-byte blocks */

	/* results */
	int status; /* first error (0 so far) */
	uint32_t retries; /* EH-cancelled chunks re-issued */
	uint32_t flush_tries; /* FLUSH CACHE re-issues */
	uint64_t flush_bad_lba; /* last LBA a FLUSH reported, ~0 none */
	uint64_t bytes_done;

	/* pass-through: the command's reference count when we first issued
	 * it (the preparer's own references); a re-issue after -ECANCELED
	 * waits until the previous completion has dropped the transport's */
	int drv_base_refs;

	/* the command that just completed: set by its end_io in the
	 * interrupt handler, consumed by ata_blk_complete in the softirq */
	struct ata_queued_cmd *done_qc;
	/* error handling's retry count of the step in flight, carried to
	 * the command that re-issues it (ATA_QCFLAG_EH_RETRY) */
	uint32_t qc_retries;
};

#define ATA_IO_MAGIC 0x4F494154u /* "TAIO" */

/* ==========================================================================
 * Per-device private state
 * ========================================================================== */

/* Disk registration state (g_ata_work_lock) */
enum ata_disk_state {
	ATA_DISK_NONE = 0, /* no gendisk */
	ATA_DISK_ADD_PENDING, /* gendisk allocated, disk_add() queued */
	ATA_DISK_ADDED, /* live */
	ATA_DISK_DEL_PENDING, /* disk_del() queued */
};

/* One job for the registration worker (ata_core.c). */
enum ata_work_kind {
	ATA_WORK_DISK_ADD = 1, /* disk_add(disk) */
	ATA_WORK_DISK_DEL, /* disk_del(disk) */
	ATA_WORK_SET_CAPACITY, /* disk_set_capacity(disk, arg) */
	ATA_WORK_FREE_DEV, /* the last ata_dev_put came from atomic ctx */
	ATA_WORK_MEDIA_CHANGED, /* disk_media_changed(disk, medium now) */
};
struct ata_work_item {
	struct ata_work_item *next;
	enum ata_work_kind kind;
	int embedded; /* part of the device: not kfree'd */
	struct ata_device *dev;
	struct gendisk *disk;
	uint64_t arg;
};

/* Persistent configuration policy.  -1 = "the default policy", other
 * values were set explicitly (ioctl, sysfs) and are re-applied after
 * every reset, resume and revalidation. */
struct ata_dev_policy {
	int8_t write_cache; /* -1 default (on), 0 off, 1 on */
	int8_t read_lookahead; /* -1 default (on), 0 off, 1 on */
	int16_t apm; /* -1 default (254), 0 disable, 1..254 level */
	int16_t aam; /* -1 firmware, 0 disable, 0x80..0xFE level */
	int8_t puis; /* -1 firmware, 0 off, 1 on */
	int8_t ncq_prio; /* 0 off (default), 1 high-prio for sync+meta */
	int8_t sense_reporting; /* -1 default (on when supported) */
	int8_t freeze_security; /* -1 default (leave), 0 leave, 1 freeze */
	int16_t standby_timer; /* -1 none, else the encoded count */
	int16_t pad;
};

struct ata_dev_priv {
	uint32_t magic; /* ATA_DEV_PRIV_MAGIC */

	/* "the calling task holds the port's eh_mutex" for transports
	 * other than AHCI, set by ata_dev_lock() and the eh_mutex-held
	 * entry points (probe/revalidate/detach) */
	volatile uint64_t eh_owner;
	uint32_t eh_depth;

	struct ata_dev_policy policy;

	/* block I/O pools (allocated by ata_rw_dev_init) */
	struct ata_queued_cmd *qc_pool;
	volatile uint64_t qc_map; /* bit set = in use */
	struct ata_io *io_pool;
	volatile uint64_t io_map;
	void *dsm_bufs[ATA_DSM_BUFS];
	uint32_t dsm_buf_blocks;
	volatile uint64_t dsm_map; /* bit set = in use */
	void *zero_buf;
	int pools_ready;

	/* READ LOG DMA EXT failed once: PIO from then on */
	int log_dma_broken;
	/* NCQ was switched off by error handling / policy: keep it off over
	 * revalidations */
	int ncq_forced_off;
	uint32_t ncq_depth_limit; /* lowered by EH, 0 none */

	/* registration */
	enum ata_disk_state disk_state;
	struct gendisk *pending_disk; /* allocated, not yet added */

	/* error reporting rate limit */
	volatile uint32_t reports;
	uint64_t report_window_ns;
	uint32_t reports_suppressed;

	/* statistics (diagnostics) */
	volatile uint64_t n_requests;
	volatile uint64_t n_commands;
	volatile uint64_t n_continued; /* steps dispatched again */
	volatile uint64_t n_flush_retries;
	volatile uint64_t n_trims;
	volatile uint64_t n_zeroes;

	/* the deferred free (ata_dev_put at zero from atomic context) */
	int free_queued;
	struct ata_work_item free_item;
	/* the free parked on a completion section (ata_dev_put at zero
	 * inside ata_qc_complete): run by the section's end */
	struct blk_release cpl_release;
};

#define ATA_DEV_PRIV_MAGIC 0x56495250u /* "PRIV" */

/* The device and its private state, allocated together by ata_dev_alloc(). */
struct ata_dev_full {
	struct ata_device dev;
	struct ata_dev_priv priv;
};

/* The private state of a device made by ata_dev_alloc(); NULL for any
 * other (a host test's device on the stack never reaches code that asks). */
static inline struct ata_dev_priv *ata_dev_priv(struct ata_device *dev)
{
	struct ata_dev_full *full;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return NULL;
	full = (struct ata_dev_full *)((char *)dev -
				       __builtin_offsetof(struct ata_dev_full,
							  dev));
	if (full->priv.magic != ATA_DEV_PRIV_MAGIC)
		return NULL;
	return &full->priv;
}

/* ==========================================================================
 * Cross-file helpers
 * ========================================================================== */

/* ata_core.c */
/* The task id the mutexes and EH markers use for the caller (0 before the
 * scheduler runs). */
uint64_t ata_self_id(void);
/* Set once anything ran with the scheduler and interrupts live: from then
 * on hrtimers and the worker thread exist. */
extern volatile int g_ata_system_up;
void ata_note_system_up(void);
/* Does the calling task hold the device's port eh_mutex (or an
 * ata_dev_lock())? */
int ata_dev_eh_held(struct ata_device *dev);
/* Management commands: take the EH serialisation unless the caller holds
 * it already.  Returns 1 when it took it (pass to ata_mgmt_exit). */
int ata_mgmt_enter(struct ata_device *dev);
void ata_mgmt_exit(struct ata_device *dev, int took);
/* Mark the calling task as running under the eh_mutex (probe/revalidate
 * entry points, ata_dev_lock); nests. */
void ata_eh_ctx_enter(struct ata_device *dev);
void ata_eh_ctx_exit(struct ata_device *dev);
/* ata_id_parse() with the reason for -EINVAL. */
int ata_id_parse_why(struct ata_device *dev, const uint16_t *id,
		     const char **why);
/* Non-data internal command shortcut: tf in, result out.  `timeout_ms' 0
 * = ATA_TMOUT_INTERNAL_MS. */
int ata_exec_nodata(struct ata_device *dev, struct ata_taskfile *tf,
		    uint32_t timeout_ms, uint32_t qcflags);
/* "UDMA/133", "PIO4" ... */
const char *ata_xfer_mode_name(uint8_t mode);
/* The deferred registration worker (runtime) or the synchronous path
 * (boot): add / remove the gendisk of a device, change its capacity. */
int ata_work_disk_add(struct ata_device *dev, struct gendisk *disk);
void ata_work_disk_del(struct ata_device *dev);
void ata_work_set_capacity(struct ata_device *dev, uint64_t sectors);
/* Hand a removable medium's change to the block layer from the worker:
 * disk_media_changed() reads the new medium (partition table, filesystem
 * identity) through the disk's request queue, which error handling holds
 * stopped and a suspended disk holds back -- so a caller in error
 * handling or power management context must not do it itself.  The
 * worker reports the medium state current when it runs.  0 queued (or
 * nothing to tell: no live disk), -EAGAIN (no worker yet: boot; the
 * disk's add still queued) or -ENOMEM; the caller keeps the change
 * pending then. */
int ata_work_media_changed(struct ata_device *dev);
/* Logging helpers with the "[ata] ata1.00: " prefix. */
void ata_dev_printk(const struct ata_device *dev, const char *fmt, ...);
/* Did the command end with only ABRT (device: "not supported / invalid
 * parameter")? */
static inline int ata_tf_is_abort_only(const struct ata_taskfile *tf)
{
	return (tf->status & ATA_ERR) && (tf->error & ATA_ABORTED) &&
	       !(tf->error & (ATA_ICRC | ATA_UNC | ATA_IDNF));
}

/* ata_rw.c */
int ata_rw_dev_init(struct ata_device *dev);
void ata_rw_dev_fini(struct ata_device *dev);
/* Run the disk's request queue: requests given back while the device was
 * asleep or its port in error handling go on.  Process context. */
void ata_rw_kick(struct ata_device *dev);
/* Release a pool command (last ata_qc_put of a POOL command). */
void ata_rw_qc_pool_release(struct ata_queued_cmd *qc);
/* The variant of ata_rw_build_tf() that may refuse NCQ (internal
 * synchronous I/O never queues). */
int ata_rw_build_tf_ex(const struct ata_device *dev, struct ata_queued_cmd *qc,
		       uint64_t lba, uint32_t count, int write, int fua,
		       int allow_ncq, int prio);
/* Validate an LBA range against the device (overflow safe).  Pure. */
int ata_rw_check_range(const struct ata_device *dev, uint64_t lba,
		       uint64_t count);
/* Update the disk's view after write cache / capacity changes. */
void ata_rw_update_disk_limits(struct ata_device *dev);

/* ata_features.c */
/* Apply the configured policy for write cache / look-ahead / APM etc. */
int ata_dev_apply_policy(struct ata_device *dev);
/* Read and interpret the NCQ SEND/RECEIVE log (queued TRIM). */
int ata_dev_probe_ncq_send_recv(struct ata_device *dev);
/* Opt-in for queued TRIM (SEND FPDMA QUEUED); 0 by default. */
extern int ata_policy_queued_trim;

/* ata_pm.c */
/* A successful media access ended any standby. */
static inline void ata_pm_note_media_access(struct ata_device *dev)
{
	if (dev->power_state != ATA_PWR_ACTIVE)
		dev->power_state = ATA_PWR_ACTIVE;
}
const char *ata_power_state_name(enum ata_power_state st);
/* Decode a CHECK POWER MODE count value. */
const char *ata_chk_power_name(uint8_t mode);

#endif // KERNEL_DEV_ATA_ATA_DEV_INTERNAL_H
