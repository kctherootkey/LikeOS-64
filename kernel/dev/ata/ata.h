// LikeOS ATA/ATAPI command layer - protocol definitions and the device model
//
// Everything about talking to an ATA or ATAPI device that does not depend
// on the host controller: the command set (opcodes, SET FEATURES and SMART
// subcommands, log addresses), the Frame Information Structures a SATA
// link carries, the IDENTIFY data layout, the task file, and the driver's
// objects -- struct ata_device (one drive), struct ata_queued_cmd (one
// command in flight) and the two synchronisation primitives the storage
// stack builds on (struct ata_completion, struct ata_mutex).
//
// The layer is split into files by subject (each line: file, owner agent):
//
//   ata_core.c      A4  device lifecycle (alloc, probe, IDENTIFY, revalidate,
//                       detach), queued-command helpers, internal command
//                       execution, error decoding, ata_mutex
//   ata_rw.c        A4  read/write/flush translation of block requests,
//                       transfer chunking (LBA28 256 / LBA48 65536 sectors),
//                       FPDMA (NCQ) vs DMA EXT vs DMA selection, FUA, the
//                       blk_ops of an ATA disk
//   ata_features.c  A4  SET FEATURES (write cache, look-ahead, transfer
//                       mode, SATA features, APM, AAM, PUIS), READ LOG EXT,
//                       EXECUTE DEVICE DIAGNOSTIC, native max / SET MAX,
//                       the post-IDENTIFY configuration policy
//   ata_pm.c        A4  CHECK POWER MODE, STANDBY/IDLE (IMMEDIATE), SLEEP,
//                       wake, device suspend/resume/shutdown
//   ata_smart.c     A5  SMART (data, thresholds, status, logs, self-tests)
//   ata_trim.c      A5  DATA SET MANAGEMENT / TRIM range encoding + issue
//   ata_security.c  A5  ATA security feature set
//   atapi.c         A5  ATAPI devices: PACKET commands, the srN optical disk
//   ata_sat.c       A5  SCSI/ATA translation for SG_IO, ATA PASS-THROUGH,
//                       the HDIO_* and SCSI_IOCTL_* handlers
//
// The host controller is reached only through struct ata_transport (an
// operations table the AHCI driver provides, kernel/dev/ata/ahci.h), so
// these files never touch controller registers and the pure parts
// (IDENTIFY parsing, command selection, TRIM encoding, SAT translation)
// can be compiled on the host against a fake transport for the boxed tests.
//
// ---- Return convention ----------------------------------------------------------
// Every function here returns 0 (or a documented non-negative value) on
// success and a NEGATIVE ERRNO on failure:
//   -EIO        the device reported an error (ERR/DF in status); details in
//               qc->result_tf (status/error) and qc->err_mask
//   -ETIMEDOUT  the command did not complete in time (EH has run)
//   -ENODEV     no device / link down / device detached
//   -EINVAL     a malformed request (bad length, LBA out of range)
//   -EOPNOTSUPP the device does not support the feature
//   -EACCES     ATA security: the device is locked / frozen
//   -ENOMEDIUM  ATAPI: no medium
//   -EBUSY      no command slot, or the device is in error handling
//   -ENOMEM     allocation failure
//   -ECANCELED  the command was aborted by error handling (retryable)
//
// ---- Context rules ---------------------------------------------------------------
//   "any context"  interrupts may be off, may be an interrupt handler.
//   "may sleep"    process context; uses might_sleep().  Before the
//                  scheduler runs (boot probe) the same functions work by
//                  polling -- every wait goes through ata_completion_wait(),
//                  which polls the transport when ata_can_sleep() is false.
//
// ---- Locks (see ahci.h for the full order) ------------------------------------------
//   port eh_mutex (struct ata_mutex) -> registry mutex (block, blk_mutex)
//   -> port->lock -> hba->lock.  The probe and hot-plug paths hold the
//   eh_mutex while a disk is added or removed; the block layer never calls
//   into a driver (submit, ioctl, shutdown, suspend, resume ...) with the
//   registry mutex held, so nothing ever takes an eh_mutex under it.
//   struct ata_completion's own spinlock is a leaf.  The ATA layer itself
//   owns no spinlock; device-level management commands are serialised by
//   the port's eh_mutex where they change device state
//   (ata_dev_lock()/ata_dev_unlock()); ata_set_sata_feature() never takes
//   it (its callers hold it or run inside error handling).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_ATA_ATA_H
#define KERNEL_DEV_ATA_ATA_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/hal/lapic.h> /* lapic_delay_us */
#include <kernel/dev/block/blkdev.h>

struct ata_device;
struct ata_queued_cmd;
struct ata_transport;
struct gendisk;
struct task;

/* ==========================================================================
 * Sizes and limits
 * ========================================================================== */

#define ATA_SECT_SIZE 512 /* the protocol's sector (IDENTIFY, logs) */
#define ATA_ID_WORDS 256 /* IDENTIFY data: 256 16-bit words */
#define ATA_ID_BYTES (ATA_ID_WORDS * 2)
#define ATA_MAX_SECTORS_LBA28 256 /* count 0 means 256 */
#define ATA_MAX_SECTORS_LBA48 65536 /* count 0 means 65536 */
#define ATA_MAX_LBA28 ((1ULL << 28) - 1)
#define ATA_MAX_LBA48 ((1ULL << 48) - 1)
#define ATA_MAX_QUEUE 32 /* NCQ tags 0..31 */
#define ATA_MAX_TRIM_RANGE 0xFFFF /* sectors per DSM range entry */
#define ATA_TRIM_RANGES_PER_BLOCK 64 /* 8-byte entries per 512 bytes */
#define ATA_MAX_DEVICES_PER_LINK 1 /* SATA: one device per link */
#define ATA_PMP_MAX_PORTS 15 /* fan-out ports of a port multiplier */
#define ATA_PMP_CTRL_PORT 15 /* the multiplier's own control port */
#define ATAPI_CDB_LEN_MAX 16
#define ATAPI_SENSE_LEN 96 /* fixed + descriptor sense, generous */
#define ATA_SENSE_LEN 96 /* sense built by the SAT layer */

/* Command timeouts (milliseconds).  The block layer's default request
 * timeout is BLK_DEFAULT_TIMEOUT_MS (30 s) for reads/writes. */
#define ATA_TMOUT_RW_MS 30000 /* READ/WRITE (DMA, FPDMA) */
#define ATA_TMOUT_FLUSH_MS 60000 /* FLUSH CACHE (EXT): may write a lot */
#define ATA_TMOUT_INTERNAL_MS 10000 /* setup commands (SET FEATURES ...) */
#define ATA_TMOUT_IDENTIFY_MS 5000 /* IDENTIFY (PACKET) DEVICE */
#define ATA_TMOUT_SPINUP_MS 30000 /* wake from standby: spin-up */
#define ATA_TMOUT_STANDBY_MS 30000 /* STANDBY IMMEDIATE (head park) */
#define ATA_TMOUT_TRIM_MS 60000 /* DATA SET MANAGEMENT */
#define ATA_TMOUT_SMART_MS 15000 /* SMART READ/RETURN STATUS */
#define ATA_TMOUT_DIAG_MS 31000 /* EXECUTE DEVICE DIAGNOSTIC */
#define ATA_TMOUT_PMP_MS 2000 /* READ/WRITE PORT MULTIPLIER */
#define ATA_TMOUT_ATAPI_MS 30000 /* PACKET commands (sg default) */
#define ATA_TMOUT_ATAPI_TUR_MS 10000 /* TEST UNIT READY / media poll */
/* SECURITY ERASE UNIT: IDENTIFY word 89/90 give the time in 2-minute units
 * (bit 15 set: extended format, bits 14:0); the timeout is that time
 * doubled, at least this: */
#define ATA_TMOUT_SEC_ERASE_MIN_MS (2 * 60 * 60 * 1000)

#define ATA_MAX_RETRIES 5 /* EH retries of a failed I/O command */
#define ATA_POLL_STEP_US 10 /* polled wait granularity */
#define ATA_WAIT_FOREVER 0xFFFFFFFFu /* ata_completion_wait: no timeout */

/* ==========================================================================
 * Command opcodes (ACS-4 names)
 * ========================================================================== */

#define ATA_CMD_NOP 0x00
#define ATA_CMD_CFA_REQ_EXT_ERR 0x03
#define ATA_CMD_DSM 0x06 /* DATA SET MANAGEMENT (TRIM) */
#define ATA_CMD_DSM_XL 0x07
#define ATA_CMD_DEV_RESET 0x08 /* DEVICE RESET (ATAPI) */
#define ATA_CMD_REQ_SENSE_DATA 0x0B /* REQUEST SENSE DATA EXT */
#define ATA_CMD_RESTORE 0x10 /* RECALIBRATE (obsolete) */
#define ATA_CMD_READ_SECTORS 0x20 /* READ SECTOR(S), PIO, LBA28 */
#define ATA_CMD_READ_SECTORS_EXT 0x24 /* PIO, LBA48 */
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_READ_DMA_QUEUED_EXT 0x26 /* TCQ (obsolete) */
#define ATA_CMD_READ_NATIVE_MAX_EXT 0x27
#define ATA_CMD_READ_MULTIPLE_EXT 0x29
#define ATA_CMD_READ_STREAM_DMA_EXT 0x2A
#define ATA_CMD_READ_STREAM_EXT 0x2B
#define ATA_CMD_READ_LOG_EXT 0x2F
#define ATA_CMD_WRITE_SECTORS 0x30 /* WRITE SECTOR(S), PIO, LBA28 */
#define ATA_CMD_WRITE_SECTORS_EXT 0x34
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_WRITE_DMA_QUEUED_EXT 0x36
#define ATA_CMD_SET_MAX_EXT 0x37 /* SET MAX ADDRESS EXT */
#define ATA_CMD_CFA_WRITE_NE 0x38
#define ATA_CMD_WRITE_MULTIPLE_EXT 0x39
#define ATA_CMD_WRITE_STREAM_DMA_EXT 0x3A
#define ATA_CMD_WRITE_STREAM_EXT 0x3B
#define ATA_CMD_WRITE_DMA_FUA_EXT 0x3D
#define ATA_CMD_WRITE_QUEUED_FUA_EXT 0x3E
#define ATA_CMD_WRITE_LOG_EXT 0x3F
#define ATA_CMD_VERIFY 0x40 /* READ VERIFY SECTOR(S) */
#define ATA_CMD_VERIFY_EXT 0x42
#define ATA_CMD_ZERO_EXT 0x44 /* ZERO EXT (ACS-4) */
#define ATA_CMD_WRITE_UNCORR_EXT 0x45
#define ATA_CMD_READ_LOG_DMA_EXT 0x47
#define ATA_CMD_ZAC_MGMT_IN 0x4A
#define ATA_CMD_CONFIG_STREAM 0x51
#define ATA_CMD_WRITE_LOG_DMA_EXT 0x57
#define ATA_CMD_TRUSTED_NONDATA 0x5B
#define ATA_CMD_TRUSTED_RCV 0x5C
#define ATA_CMD_TRUSTED_RCV_DMA 0x5D
#define ATA_CMD_TRUSTED_SND 0x5E
#define ATA_CMD_TRUSTED_SND_DMA 0x5F
#define ATA_CMD_FPDMA_READ 0x60 /* READ FPDMA QUEUED (NCQ) */
#define ATA_CMD_FPDMA_WRITE 0x61 /* WRITE FPDMA QUEUED (NCQ) */
#define ATA_CMD_NCQ_NON_DATA 0x63
#define ATA_CMD_FPDMA_SEND 0x64 /* SEND FPDMA QUEUED (queued TRIM) */
#define ATA_CMD_FPDMA_RECV 0x65 /* RECEIVE FPDMA QUEUED */
#define ATA_CMD_SEEK 0x70
#define ATA_CMD_SET_DATE_TIME_EXT 0x77
#define ATA_CMD_ACCESSIBLE_MAX 0x78 /* ACCESSIBLE MAX ADDRESS CONFIG */
#define ATA_CMD_EDD 0x90 /* EXECUTE DEVICE DIAGNOSTIC */
#define ATA_CMD_INIT_DEV_PARAMS 0x91
#define ATA_CMD_DOWNLOAD_MICRO 0x92
#define ATA_CMD_DOWNLOAD_MICRO_DMA 0x93
#define ATA_CMD_ZAC_MGMT_OUT 0x9F
#define ATA_CMD_PACKET 0xA0
#define ATA_CMD_ID_ATAPI 0xA1 /* IDENTIFY PACKET DEVICE */
#define ATA_CMD_SERVICE 0xA2
#define ATA_CMD_SMART 0xB0
#define ATA_CMD_CONF_OVERLAY 0xB1 /* DEVICE CONFIGURATION OVERLAY */
#define ATA_CMD_SET_SECTOR_CONFIG 0xB2
#define ATA_CMD_SANITIZE_DEVICE 0xB4
#define ATA_CMD_NV_CACHE 0xB6
#define ATA_CMD_CFA_ERASE 0xC0
#define ATA_CMD_READ_MULTI 0xC4
#define ATA_CMD_WRITE_MULTI 0xC5
#define ATA_CMD_SET_MULTI 0xC6 /* SET MULTIPLE MODE */
#define ATA_CMD_READ_DMA_QUEUED 0xC7
#define ATA_CMD_READ_DMA 0xC8 /* READ DMA, LBA28 */
#define ATA_CMD_WRITE_DMA 0xCA /* WRITE DMA, LBA28 */
#define ATA_CMD_WRITE_DMA_QUEUED 0xCC
#define ATA_CMD_CFA_WRITE_MULT_NE 0xCD
#define ATA_CMD_WRITE_MULTI_FUA_EXT 0xCE
#define ATA_CMD_CHK_MED_CRD_TYP 0xD1
#define ATA_CMD_GET_MEDIA_STATUS 0xDA
#define ATA_CMD_MEDIA_LOCK 0xDE
#define ATA_CMD_MEDIA_UNLOCK 0xDF
#define ATA_CMD_STANDBYNOW1 0xE0 /* STANDBY IMMEDIATE */
#define ATA_CMD_IDLEIMMEDIATE 0xE1 /* IDLE IMMEDIATE (+ UNLOAD) */
#define ATA_CMD_STANDBY 0xE2 /* STANDBY with timer */
#define ATA_CMD_IDLE 0xE3 /* IDLE with timer */
#define ATA_CMD_PMP_READ 0xE4 /* READ PORT MULTIPLIER (on port 15) */
#define ATA_CMD_READ_BUFFER 0xE4 /* same opcode on a device */
#define ATA_CMD_CHK_POWER 0xE5 /* CHECK POWER MODE */
#define ATA_CMD_SLEEP 0xE6
#define ATA_CMD_FLUSH 0xE7 /* FLUSH CACHE */
#define ATA_CMD_PMP_WRITE 0xE8 /* WRITE PORT MULTIPLIER (on port 15) */
#define ATA_CMD_WRITE_BUFFER 0xE8 /* same opcode on a device */
#define ATA_CMD_READ_BUFFER_DMA 0xE9
#define ATA_CMD_FLUSH_EXT 0xEA /* FLUSH CACHE EXT */
#define ATA_CMD_WRITE_BUFFER_DMA 0xEB
#define ATA_CMD_ID_ATA 0xEC /* IDENTIFY DEVICE */
#define ATA_CMD_MEDIA_EJECT 0xED
#define ATA_CMD_SET_FEATURES 0xEF
#define ATA_CMD_SEC_SET_PASS 0xF1 /* SECURITY SET PASSWORD */
#define ATA_CMD_SEC_UNLOCK 0xF2
#define ATA_CMD_SEC_ERASE_PREP 0xF3
#define ATA_CMD_SEC_ERASE_UNIT 0xF4
#define ATA_CMD_SEC_FREEZE_LOCK 0xF5
#define ATA_CMD_SEC_DISABLE_PASS 0xF6
#define ATA_CMD_READ_NATIVE_MAX 0xF8
#define ATA_CMD_SET_MAX 0xF9 /* SET MAX ADDRESS (+ security subcmds) */

/* ---- SET FEATURES subcommands (feature register) ---------------------- */
#define SETFEATURES_WC_ON 0x02 /* enable volatile write cache */
#define SETFEATURES_WC_OFF 0x82
#define SETFEATURES_XFER 0x03 /* set transfer mode (count = mode) */
#define SETFEATURES_APM_ON 0x05 /* enable APM (count = level) */
#define SETFEATURES_APM_OFF 0x85
#define SETFEATURES_PUIS_ON 0x06 /* power-up in standby */
#define SETFEATURES_PUIS_OFF 0x86
#define SETFEATURES_SPINUP 0x07 /* spin up after PUIS */
#define SETFEATURES_SATA_ENABLE 0x10 /* enable SATA feature (count) */
#define SETFEATURES_SATA_DISABLE 0x90
#define SETFEATURES_AAM_ON 0x42 /* acoustic management (count) */
#define SETFEATURES_AAM_OFF 0xC2
#define SETFEATURES_EPC 0x4A /* extended power conditions */
#define SETFEATURES_RLA_OFF 0x55 /* disable read look-ahead */
#define SETFEATURES_RLA_ON 0xAA
#define SETFEATURES_REVERT_DEF_OFF 0x66 /* keep settings over reset */
#define SETFEATURES_REVERT_DEF_ON 0xCC
#define SETFEATURES_SENSE_DATA 0xC3 /* sense data reporting (count 1/0) */
#define SETFEATURES_CDL 0x0D /* command duration limits */

/* SET FEATURES 10h/90h: SATA features (count register) */
#define SATA_FEAT_FPDMA_OFFSET 0x01 /* non-zero buffer offsets */
#define SATA_FEAT_FPDMA_AA 0x02 /* DMA Setup FIS auto-activate */
#define SATA_FEAT_DIPM 0x03 /* device-initiated power management */
#define SATA_FEAT_FPDMA_IN_ORDER 0x04 /* in-order data delivery */
#define SATA_FEAT_AN 0x05 /* asynchronous notification */
#define SATA_FEAT_SSP 0x06 /* software settings preservation */
#define SATA_FEAT_DAPS 0x07 /* device automatic partial to slumber */
#define SATA_FEAT_HW_CTRL 0x08 /* hardware feature control */
#define SATA_FEAT_DEVSLP 0x09 /* device sleep */
#define SATA_FEAT_HYBRID 0x0A /* hybrid information */

/* Transfer modes for SETFEATURES_XFER (count register) */
#define XFER_PIO_SLOW 0x00
#define XFER_PIO_0 0x08
#define XFER_PIO_1 0x09
#define XFER_PIO_2 0x0A
#define XFER_PIO_3 0x0B
#define XFER_PIO_4 0x0C
#define XFER_MW_DMA_0 0x20
#define XFER_MW_DMA_1 0x21
#define XFER_MW_DMA_2 0x22
#define XFER_UDMA_0 0x40
#define XFER_UDMA_1 0x41
#define XFER_UDMA_2 0x42
#define XFER_UDMA_3 0x43
#define XFER_UDMA_4 0x44
#define XFER_UDMA_5 0x45
#define XFER_UDMA_6 0x46
#define XFER_UDMA_7 0x47

/* ---- SMART (command B0h, subcommand in the feature register) ----------- */
#define ATA_SMART_READ_VALUES 0xD0 /* SMART READ DATA (512 bytes) */
#define ATA_SMART_READ_THRESHOLDS 0xD1 /* obsolete but widely answered */
#define ATA_SMART_AUTOSAVE 0xD2 /* count F1h enable / 00h disable */
#define ATA_SMART_SAVE 0xD3
#define ATA_SMART_IMMEDIATE_OFFLINE 0xD4 /* LBA low = test (below) */
#define ATA_SMART_READ_LOG_SECTOR 0xD5 /* SMART READ LOG (LBA low = log) */
#define ATA_SMART_WRITE_LOG_SECTOR 0xD6
#define ATA_SMART_WRITE_THRESHOLDS 0xD7 /* obsolete */
#define ATA_SMART_ENABLE 0xD8
#define ATA_SMART_DISABLE 0xD9
#define ATA_SMART_STATUS 0xDA /* RETURN STATUS */
#define ATA_SMART_AUTO_OFFLINE 0xDB /* count F8h enable / 00h disable */
/* Signature in LBA mid/high that every SMART command carries */
#define ATA_SMART_LBAM_PASS 0x4F
#define ATA_SMART_LBAH_PASS 0xC2
/* RETURN STATUS: threshold exceeded is reported by this signature */
#define ATA_SMART_LBAM_FAIL 0xF4
#define ATA_SMART_LBAH_FAIL 0x2C
/* EXECUTE OFF-LINE IMMEDIATE subcommands (LBA low) */
#define ATA_SMART_TEST_OFFLINE 0x00
#define ATA_SMART_TEST_SHORT 0x01
#define ATA_SMART_TEST_EXTENDED 0x02
#define ATA_SMART_TEST_CONVEYANCE 0x03
#define ATA_SMART_TEST_SELECTIVE 0x04
#define ATA_SMART_TEST_ABORT 0x7F
#define ATA_SMART_TEST_SHORT_CAPTIVE 0x81
#define ATA_SMART_TEST_EXTENDED_CAPTIVE 0x82
#define ATA_SMART_TEST_CONVEYANCE_CAPTIVE 0x83
#define ATA_SMART_TEST_SELECTIVE_CAPTIVE 0x84
/* SMART READ DATA layout (offsets in the 512-byte page) */
#define ATA_SMART_DATA_REVISION 0 /* word */
#define ATA_SMART_DATA_ATTRS 2 /* 30 attributes x 12 bytes */
#define ATA_SMART_NUM_ATTRS 30
#define ATA_SMART_ATTR_SIZE 12
#define ATA_SMART_DATA_OFFLINE_STATUS 362
#define ATA_SMART_DATA_SELFTEST_STATUS 363
#define ATA_SMART_DATA_OFFLINE_TIME 364 /* word, seconds */
#define ATA_SMART_DATA_OFFLINE_CAP 367
#define ATA_SMART_DATA_SMART_CAP 368 /* word */
#define ATA_SMART_DATA_ERRLOG_CAP 370
#define ATA_SMART_DATA_SHORT_TIME 372 /* minutes */
#define ATA_SMART_DATA_EXT_TIME 373 /* minutes, 0xFF: see word 375 */
#define ATA_SMART_DATA_CONV_TIME 374
#define ATA_SMART_DATA_EXT_TIME_WORD 375
#define ATA_SMART_DATA_CHECKSUM 511 /* sum of all 512 bytes == 0 */

/* ---- Log addresses (SMART READ LOG / READ LOG EXT) -------------------- */
#define ATA_LOG_DIRECTORY 0x00 /* general purpose / SMART log directory */
#define ATA_LOG_SUMMARY_ERR 0x01 /* SMART summary error log */
#define ATA_LOG_COMP_ERR 0x02 /* SMART comprehensive error log */
#define ATA_LOG_EXT_COMP_ERR 0x03 /* extended comprehensive error log */
#define ATA_LOG_DEVSTAT 0x04 /* device statistics */
#define ATA_LOG_SELFTEST 0x06 /* SMART self-test log */
#define ATA_LOG_EXT_SELFTEST 0x07 /* extended self-test log */
#define ATA_LOG_POWER_COND 0x08
#define ATA_LOG_SELECTIVE_SELFTEST 0x09
#define ATA_LOG_DEVICE_STAT_NOTIF 0x0A
#define ATA_LOG_PENDING_DEFECTS 0x0C
#define ATA_LOG_NCQ_CMD_ERR 0x10 /* NCQ command error log (EH) */
#define ATA_LOG_SATA_PHY_EVENT 0x11 /* SATA PHY event counters */
#define ATA_LOG_NCQ_QUEUE_MGMT 0x12
#define ATA_LOG_NCQ_SEND_RECV 0x13
#define ATA_LOG_HYBRID_INFO 0x14
#define ATA_LOG_REBUILD_ASSIST 0x15
#define ATA_LOG_IDENTIFY 0x30 /* IDENTIFY DEVICE data log */
#define ATA_LOG_SCT_STATUS 0xE0 /* SCT command/status */
#define ATA_LOG_SCT_DATA 0xE1 /* SCT data transfer */
/* IDENTIFY DEVICE data log (30h) pages */
#define ATA_LOG_ID_PAGE_LIST 0x00
#define ATA_LOG_ID_COPY 0x01
#define ATA_LOG_ID_CAPACITY 0x02
#define ATA_LOG_ID_SUPP_CAPS 0x03
#define ATA_LOG_ID_CURRENT_SETTINGS 0x04
#define ATA_LOG_ID_SATA_SETTINGS 0x08
/* NCQ command error log (10h) page 0 layout */
#define ATA_NCQ_ERR_TAG_BYTE 0 /* bits 4:0 tag, bit 7 NQ (non-queued) */
#define ATA_NCQ_ERR_NQ 0x80
#define ATA_NCQ_ERR_STATUS 2
#define ATA_NCQ_ERR_ERROR 3
#define ATA_NCQ_ERR_LBA0 4 /* LBA 7:0, 15:8, 23:16 at 4,5,6 */
#define ATA_NCQ_ERR_DEVICE 7
#define ATA_NCQ_ERR_LBA3 8 /* LBA 31:24, 39:32, 47:40 at 8,9,10 */
#define ATA_NCQ_ERR_COUNT 12 /* count 7:0, 15:8 at 12,13 */
#define ATA_NCQ_ERR_SENSE_KEY 14 /* NCQ autosense (ACS-4) */
#define ATA_NCQ_ERR_ASC 15
#define ATA_NCQ_ERR_ASCQ 16
#define ATA_NCQ_ERR_CHECKSUM 511

/* ---- DATA SET MANAGEMENT ------------------------------------------------ */
#define ATA_DSM_TRIM 0x01 /* feature register bit 0 */
/* One range entry: LBA in bits 47:0, length (sectors, 1..65535) in bits
 * 63:48; little-endian; unused entries zero. */
#define ATA_DSM_RANGE(lba, len)                                        \
	(((uint64_t)(lba) & 0x0000FFFFFFFFFFFFULL) |                   \
	 ((uint64_t)((len) & 0xFFFF) << 48))

/* ---- SECURITY (the 512-byte data block of F1h/F2h/F4h/F6h) -------------- */
#define ATA_SEC_PW_LEN 32 /* bytes of password, words 1..16 */
#define ATA_SEC_CTRL_MASTER 0x0001 /* word 0 bit 0: 1 master, 0 user */
#define ATA_SEC_CTRL_ENHANCED 0x0002 /* ERASE UNIT: enhanced erase */
#define ATA_SEC_CTRL_LEVEL_MAX 0x0100 /* SET PASSWORD: maximum security */
#define ATA_SEC_MASTER_ID_WORD 17 /* SET PASSWORD master identifier */

/* ==========================================================================
 * Status, error, device and control registers
 * ========================================================================== */

/* Status register */
#define ATA_ERR 0x01 /* an error occurred (see error register) */
#define ATA_SENSE 0x02 /* sense data available (ACS-3+) */
#define ATA_CORR 0x04 /* obsolete: corrected data / alignment error */
#define ATA_DRQ 0x08 /* data request */
#define ATA_DSC 0x10 /* deferred write error / seek complete */
#define ATA_DF 0x20 /* device fault (also stream error) */
#define ATA_DRDY 0x40 /* device ready */
#define ATA_BUSY 0x80 /* BSY */

/* Error register */
#define ATA_AMNF 0x01 /* address mark not found (obsolete) / ILI */
#define ATA_TRK0NF 0x02 /* track 0 not found / end of media */
#define ATA_ABORTED 0x04 /* command aborted */
#define ATA_MCR 0x08 /* media change request (obsolete) */
#define ATA_IDNF 0x10 /* ID not found: address out of range */
#define ATA_MC 0x20 /* media changed (obsolete) */
#define ATA_UNC 0x40 /* uncorrectable data error */
#define ATA_ICRC 0x80 /* interface CRC error */
/* ATAPI: the error register's high nibble is the sense key */
#define ATAPI_ERR_SENSE_KEY_SHIFT 4
#define ATAPI_ERR_EOM 0x02
#define ATAPI_ERR_ILI 0x01

/* Device register */
#define ATA_DEVICE_OBS 0xA0 /* obsolete bits 7 and 5 (set for legacy) */
#define ATA_LBA 0x40 /* LBA addressing */
#define ATA_DEV1 0x10 /* device 1 (parallel ATA only) */
#define ATA_FPDMA_FUA 0x80 /* FPDMA WRITE: device bit 7 = FUA */

/* Device control register */
#define ATA_NIEN 0x02 /* disable INTRQ */
#define ATA_SRST 0x04 /* software reset */
#define ATA_HOB 0x80 /* read back the high-order bytes */

/* ATAPI PACKET feature bits */
#define ATAPI_PKT_DMA 0x01 /* data phase by DMA */
#define ATAPI_DMADIR 0x04 /* DMA direction for bridged devices */
/* ATAPI interrupt reason (count register) */
#define ATAPI_INT_REASON_CD 0x01 /* command/data */
#define ATAPI_INT_REASON_IO 0x02 /* direction: 1 to host */
#define ATAPI_INT_REASON_REL 0x04

/* ==========================================================================
 * Device signatures and classes
 * ========================================================================== */

/* PxSIG / the D2H FIS after reset: count | lbal<<8 | lbam<<16 | lbah<<24 */
#define ATA_SIG_ATA 0x00000101u
#define ATA_SIG_ATAPI 0xEB140101u
#define ATA_SIG_PMP 0x96690101u /* port multiplier */
#define ATA_SIG_SEMB 0xC33C0101u /* SATA enclosure management bridge */
#define ATA_SIG_ZAC 0xABCD0101u /* host-managed zoned device */

enum ata_class {
	ATA_DEV_UNKNOWN = 0, /* not yet classified / unrecognised sig */
	ATA_DEV_NONE, /* no device on the link */
	ATA_DEV_ATA, /* ATA disk */
	ATA_DEV_ATAPI, /* ATAPI (optical) */
	ATA_DEV_PMP, /* port multiplier */
	ATA_DEV_SEMB, /* enclosure management bridge */
	ATA_DEV_ZAC, /* host-managed zoned disk (treated as ATA, ro) */
};

/* The class a link's signature means.  Pure. */
static inline enum ata_class ata_class_from_sig(uint32_t sig)
{
	switch (sig) {
	case ATA_SIG_ATA:
		return ATA_DEV_ATA;
	case ATA_SIG_ATAPI:
		return ATA_DEV_ATAPI;
	case ATA_SIG_PMP:
		return ATA_DEV_PMP;
	case ATA_SIG_SEMB:
		return ATA_DEV_SEMB;
	case ATA_SIG_ZAC:
		return ATA_DEV_ZAC;
	case 0xFFFFFFFFu:
	case 0x00000000u:
		return ATA_DEV_NONE;
	default:
		/* Some devices report only the high half reliably. */
		if ((sig >> 16) == (ATA_SIG_ATAPI >> 16))
			return ATA_DEV_ATAPI;
		if ((sig >> 16) == (ATA_SIG_PMP >> 16))
			return ATA_DEV_PMP;
		if ((sig >> 16) == 0)
			return ATA_DEV_ATA;
		return ATA_DEV_UNKNOWN;
	}
}

static inline const char *ata_class_name(enum ata_class c)
{
	switch (c) {
	case ATA_DEV_NONE:
		return "none";
	case ATA_DEV_ATA:
		return "ATA";
	case ATA_DEV_ATAPI:
		return "ATAPI";
	case ATA_DEV_PMP:
		return "PMP";
	case ATA_DEV_SEMB:
		return "SEMB";
	case ATA_DEV_ZAC:
		return "ZAC";
	default:
		return "unknown";
	}
}

/* ==========================================================================
 * Frame Information Structures (Serial ATA revision 3.x, all little-endian)
 * ========================================================================== */

#define FIS_TYPE_REG_H2D 0x27 /* register, host to device */
#define FIS_TYPE_REG_D2H 0x34 /* register, device to host */
#define FIS_TYPE_DMA_ACTIVATE 0x39
#define FIS_TYPE_DMA_SETUP 0x41 /* first-party DMA setup (NCQ) */
#define FIS_TYPE_DATA 0x46
#define FIS_TYPE_BIST 0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_SDB 0xA1 /* set device bits */

/* Lengths in bytes (a command FIS length in the AHCI command header is
 * given in dwords: FIS_H2D_LEN / 4 = 5). */
#define FIS_H2D_LEN 20
#define FIS_D2H_LEN 20
#define FIS_DMA_ACTIVATE_LEN 4
#define FIS_DMA_SETUP_LEN 28
#define FIS_BIST_LEN 12
#define FIS_PIO_SETUP_LEN 20
#define FIS_SDB_LEN 8
#define FIS_DATA_HDR_LEN 4

/* Byte 1 of most FISes: bits 3:0 port multiplier port, high bits per type */
#define FIS_PMP_MASK 0x0F
#define FIS_H2D_C 0x80 /* H2D: 1 = command register update */
#define FIS_I 0x40 /* D2H/PIO/SDB/DMA setup: interrupt bit */
#define FIS_D 0x20 /* PIO/DMA setup: direction, 1 = device to host */
#define FIS_DMA_SETUP_A 0x80 /* DMA setup: auto-activate */
#define FIS_SDB_N 0x80 /* SDB: notification (async notify) */

struct fis_reg_h2d {
	uint8_t fis_type; /* FIS_TYPE_REG_H2D */
	uint8_t pm_c; /* bits 3:0 PM port, bit 7 C */
	uint8_t command;
	uint8_t feature_lo; /* features 7:0 */
	uint8_t lba0, lba1, lba2; /* LBA 7:0, 15:8, 23:16 */
	uint8_t device;
	uint8_t lba3, lba4, lba5; /* LBA 31:24, 39:32, 47:40 */
	uint8_t feature_hi; /* features 15:8 */
	uint8_t count_lo, count_hi; /* count 7:0, 15:8 (NCQ: tag in 7:3) */
	uint8_t icc; /* isochronous command completion */
	uint8_t control; /* device control */
	uint8_t aux[4]; /* auxiliary 7:0 .. 31:24 (ACS-3) */
} __attribute__((packed));

struct fis_reg_d2h {
	uint8_t fis_type; /* FIS_TYPE_REG_D2H */
	uint8_t pm_i; /* bits 3:0 PM port, bit 6 I */
	uint8_t status;
	uint8_t error;
	uint8_t lba0, lba1, lba2;
	uint8_t device;
	uint8_t lba3, lba4, lba5;
	uint8_t rsvd0;
	uint8_t count_lo, count_hi;
	uint8_t rsvd1[2];
	uint8_t rsvd2[4];
} __attribute__((packed));

struct fis_dma_activate {
	uint8_t fis_type; /* FIS_TYPE_DMA_ACTIVATE */
	uint8_t pm; /* bits 3:0 PM port */
	uint8_t rsvd[2];
} __attribute__((packed));

struct fis_dma_setup {
	uint8_t fis_type; /* FIS_TYPE_DMA_SETUP */
	uint8_t flags; /* 3:0 PM port, 5 D, 6 I, 7 A */
	uint8_t rsvd0[2];
	uint64_t dma_buffer_id; /* NCQ: bits 4:0 = tag */
	uint32_t rsvd1;
	uint32_t dma_buffer_offset; /* byte offset, bits 1:0 zero */
	uint32_t transfer_count; /* bytes, bit 0 zero */
	uint32_t rsvd2;
} __attribute__((packed));

struct fis_bist {
	uint8_t fis_type; /* FIS_TYPE_BIST */
	uint8_t pm; /* bits 3:0 PM port */
	uint8_t pattern; /* pattern definition bits (T A S L F P R V) */
	uint8_t rsvd;
	uint32_t data[2];
} __attribute__((packed));

struct fis_pio_setup {
	uint8_t fis_type; /* FIS_TYPE_PIO_SETUP */
	uint8_t flags; /* 3:0 PM port, 5 D, 6 I */
	uint8_t status;
	uint8_t error;
	uint8_t lba0, lba1, lba2;
	uint8_t device;
	uint8_t lba3, lba4, lba5;
	uint8_t rsvd0;
	uint8_t count_lo, count_hi;
	uint8_t rsvd1;
	uint8_t e_status; /* ending status after the data phase */
	uint16_t transfer_count; /* bytes in this data FIS */
	uint8_t rsvd2[2];
} __attribute__((packed));

struct fis_set_device_bits {
	uint8_t fis_type; /* FIS_TYPE_SDB */
	uint8_t flags; /* 3:0 PM port, 6 I, 7 N */
	uint8_t status; /* bits 2:0 status-lo, 6:4 status-hi */
	uint8_t error;
	uint32_t protocol; /* NCQ: SActive bits of completed tags */
} __attribute__((packed));

struct fis_data {
	uint8_t fis_type; /* FIS_TYPE_DATA */
	uint8_t pm; /* bits 3:0 PM port */
	uint8_t rsvd[2];
	uint32_t data[]; /* 1..2048 dwords */
} __attribute__((packed));

/* ==========================================================================
 * IDENTIFY (PACKET) DEVICE word map
 * ========================================================================== */

#define ATA_ID_CONFIG 0 /* general configuration */
#define ATA_ID_CYLS 1 /* default cylinders (obsolete) */
#define ATA_ID_SPECIFIC_CFG 2
#define ATA_ID_HEADS 3 /* default heads (obsolete) */
#define ATA_ID_SECTORS 6 /* default sectors/track (obsolete) */
#define ATA_ID_SERNO 10 /* 10..19: serial number, 20 chars */
#define ATA_ID_SERNO_LEN 20
#define ATA_ID_FW_REV 23 /* 23..26: firmware revision, 8 chars */
#define ATA_ID_FW_REV_LEN 8
#define ATA_ID_PROD 27 /* 27..46: model number, 40 chars */
#define ATA_ID_PROD_LEN 40
#define ATA_ID_MAX_MULTSECT 47
#define ATA_ID_TRUSTED 48
#define ATA_ID_CAPABILITY 49
#define ATA_ID_CAPABILITY_2 50
#define ATA_ID_OLD_PIO_MODES 51
#define ATA_ID_OLD_DMA_MODES 52
#define ATA_ID_FIELD_VALID 53
#define ATA_ID_CUR_CYLS 54
#define ATA_ID_CUR_HEADS 55
#define ATA_ID_CUR_SECTORS 56
#define ATA_ID_CUR_CAPACITY 57 /* 57..58 */
#define ATA_ID_MULTSECT 59
#define ATA_ID_LBA_CAPACITY 60 /* 60..61: LBA28 user sectors */
#define ATA_ID_SWDMA_MODES 62
#define ATA_ID_MWDMA_MODES 63
#define ATA_ID_PIO_MODES 64
#define ATA_ID_EIDE_DMA_MIN 65
#define ATA_ID_EIDE_DMA_TIME 66
#define ATA_ID_EIDE_PIO 67
#define ATA_ID_EIDE_PIO_IORDY 68
#define ATA_ID_ADDITIONAL_SUPP 69
#define ATA_ID_QUEUE_DEPTH 75
#define ATA_ID_SATA_CAPABILITY 76
#define ATA_ID_SATA_CAPABILITY_2 77
#define ATA_ID_FEATURE_SUPP 78 /* SATA features supported */
#define ATA_ID_FEATURE_ENABLED 79 /* SATA features enabled */
#define ATA_ID_MAJOR_VER 80
#define ATA_ID_MINOR_VER 81
#define ATA_ID_COMMAND_SET_1 82
#define ATA_ID_COMMAND_SET_2 83
#define ATA_ID_CFSSE 84 /* command set/feature supported ext. */
#define ATA_ID_CFS_ENABLE_1 85
#define ATA_ID_CFS_ENABLE_2 86
#define ATA_ID_CSF_DEFAULT 87
#define ATA_ID_UDMA_MODES 88
#define ATA_ID_ERASE_TIME 89 /* normal security erase time */
#define ATA_ID_ENH_ERASE_TIME 90 /* enhanced security erase time */
#define ATA_ID_APM_LEVEL 91 /* current APM level (bits 7:0) */
#define ATA_ID_MASTER_PW_REV 92
#define ATA_ID_HW_CONFIG 93
#define ATA_ID_AAM 94 /* bits 7:0 current, 15:8 vendor recommended */
#define ATA_ID_LBA_CAPACITY_2 100 /* 100..103: LBA48 user sectors */
#define ATA_ID_MAX_DSM_BLOCKS 105 /* DSM: max 512-byte range blocks */
#define ATA_ID_SECTOR_SIZE 106 /* physical/logical sector size */
#define ATA_ID_WWN 108 /* 108..111: world wide name */
#define ATA_ID_LOGICAL_SECTOR_SIZE 117 /* 117..118: words per logical */
#define ATA_ID_COMMAND_SET_3 119
#define ATA_ID_COMMAND_SET_4 120
#define ATA_ID_LAST_LUN 126
#define ATA_ID_DLF 128 /* security status */
#define ATA_ID_CSFO 129
#define ATA_ID_CFA_POWER 160
#define ATA_ID_CFA_KEY_MGMT 162
#define ATA_ID_CFA_MODES 163
#define ATA_ID_FORM_FACTOR 168 /* bits 3:0 nominal form factor */
#define ATA_ID_DATA_SET_MGMT 169 /* bit 0: TRIM */
#define ATA_ID_ROT_SPEED 217 /* nominal media rotation rate */
#define ATA_ID_SCT_CMD_XPORT 206
#define ATA_ID_SECTOR_ALIGNMENT 209 /* logical sector offset */
#define ATA_ID_TRANSPORT_MAJOR 222
#define ATA_ID_EXT_CAPACITY 230 /* 230..233: extended user sectors */
#define ATA_ID_INTEGRITY 255 /* 7:0 = A5h signature, 15:8 checksum */

/* Word 0 (ATA): bit 15 clear = ATA device; bit 7 removable media;
 * bit 2 response incomplete.  ATAPI: bits 15:14 = 10b, 12:8 command packet
 * set (05h = CD/DVD), 7 removable, 6:5 DRQ timing, 1:0 packet size
 * (00 12 bytes, 01 16 bytes). */
#define ATA_ID_CONFIG_NOT_ATA 0x8000
#define ATA_ID_CONFIG_REMOVABLE 0x0080
#define ATA_ID_CONFIG_INCOMPLETE 0x0004
#define ATA_ID_CONFIG_ATAPI_MASK 0xC000
#define ATA_ID_CONFIG_ATAPI 0x8000
#define ATA_ID_CONFIG_PKT_SET(w) (((w) >> 8) & 0x1F)
#define ATA_ID_CONFIG_PKT_CDROM 0x05
#define ATA_ID_CONFIG_CDB16 0x0001
#define ATA_ID_CONFIG_CFA 0x848A /* CompactFlash signature */
/* Word 49 */
#define ATA_ID_CAP_DMA (1u << 8)
#define ATA_ID_CAP_LBA (1u << 9)
#define ATA_ID_CAP_IORDY_DIS (1u << 10)
#define ATA_ID_CAP_IORDY (1u << 11)
#define ATA_ID_CAP_STBY_TIMER (1u << 13) /* standard timer values */
/* Word 53 */
#define ATA_ID_FV_64_70 (1u << 1) /* words 64..70 valid */
#define ATA_ID_FV_88 (1u << 2) /* word 88 valid */
/* Word 59 */
#define ATA_ID_MULT_VALID (1u << 8)
#define ATA_ID_SANITIZE_SUPP (1u << 12)
#define ATA_ID_SANITIZE_CRYPTO (1u << 13)
#define ATA_ID_SANITIZE_OVERWRITE (1u << 14)
#define ATA_ID_SANITIZE_BLOCK (1u << 15)
/* Word 69 */
#define ATA_ID_ADD_NONVOL_WC (1u << 2) /* all write cache non-volatile */
#define ATA_ID_ADD_EXT_CAPACITY (1u << 3) /* words 230..233 valid */
#define ATA_ID_ADD_ENCRYPT_ALL (1u << 4) /* device encrypts all data */
#define ATA_ID_ADD_RZAT (1u << 5) /* trimmed sectors read zero */
#define ATA_ID_ADD_DMA_DL_MICRO (1u << 8)
#define ATA_ID_ADD_DRAT (1u << 14) /* deterministic read after TRIM */
/* Word 76: SATA capabilities */
#define ATA_ID_SATA_GEN1 (1u << 1) /* 1.5 Gb/s */
#define ATA_ID_SATA_GEN2 (1u << 2) /* 3.0 Gb/s */
#define ATA_ID_SATA_GEN3 (1u << 3) /* 6.0 Gb/s */
#define ATA_ID_SATA_NCQ (1u << 8)
#define ATA_ID_SATA_HIPM (1u << 9) /* host-initiated PM requests */
#define ATA_ID_SATA_PHY_EVENT (1u << 10)
#define ATA_ID_SATA_NCQ_UNLOAD (1u << 11)
#define ATA_ID_SATA_NCQ_PRIO (1u << 12)
#define ATA_ID_SATA_HAPS (1u << 13) /* host auto partial to slumber */
#define ATA_ID_SATA_DAPS (1u << 14) /* device auto partial to slumber */
#define ATA_ID_SATA_LOG_DMA (1u << 15) /* READ LOG DMA EXT equivalent */
/* Word 77: SATA additional capabilities */
#define ATA_ID_SATA2_CUR_SPEED(w) (((w) >> 1) & 0x7)
#define ATA_ID_SATA2_NCQ_STREAM (1u << 4)
#define ATA_ID_SATA2_NCQ_QMGMT (1u << 5) /* NCQ NON-DATA */
#define ATA_ID_SATA2_NCQ_SEND_RECV (1u << 6)
#define ATA_ID_SATA2_DEVSLP_RPS (1u << 7)
/* Words 78/79: SATA features supported / enabled */
#define ATA_ID_FEAT_NZ_OFFSET (1u << 1)
#define ATA_ID_FEAT_DMA_SETUP_AA (1u << 2)
#define ATA_ID_FEAT_DIPM (1u << 3)
#define ATA_ID_FEAT_IN_ORDER (1u << 4)
#define ATA_ID_FEAT_AN (1u << 5) /* asynchronous notification */
#define ATA_ID_FEAT_SSP (1u << 6)
#define ATA_ID_FEAT_NCQ_AUTOSENSE (1u << 7)
#define ATA_ID_FEAT_DEVSLP (1u << 8)
#define ATA_ID_FEAT_HYBRID (1u << 9)
/* Word 80: major version (bit n = ATA/ATAPI-n / ACS) */
#define ATA_ID_MAJOR_ATA8_ACS (1u << 8)
#define ATA_ID_MAJOR_ACS2 (1u << 9)
#define ATA_ID_MAJOR_ACS3 (1u << 10)
#define ATA_ID_MAJOR_ACS4 (1u << 11)
#define ATA_ID_MAJOR_ACS5 (1u << 12)
/* Word 82 (supported) / 85 (enabled) */
#define ATA_ID_CS1_SMART (1u << 0)
#define ATA_ID_CS1_SECURITY (1u << 1)
#define ATA_ID_CS1_PM (1u << 3) /* power management (mandatory) */
#define ATA_ID_CS1_PACKET (1u << 4)
#define ATA_ID_CS1_WCACHE (1u << 5) /* volatile write cache */
#define ATA_ID_CS1_RLA (1u << 6) /* read look-ahead */
#define ATA_ID_CS1_DEV_RESET (1u << 9)
#define ATA_ID_CS1_WBUFFER (1u << 12)
#define ATA_ID_CS1_RBUFFER (1u << 13)
#define ATA_ID_CS1_NOP (1u << 14)
/* Word 83 (supported) / 86 (enabled) */
#define ATA_ID_CS2_DL_MICRO (1u << 0)
#define ATA_ID_CS2_APM (1u << 3)
#define ATA_ID_CS2_PUIS (1u << 5)
#define ATA_ID_CS2_SPINUP_REQ (1u << 6) /* SET FEATURES spin-up needed */
#define ATA_ID_CS2_SET_MAX_SEC (1u << 8)
#define ATA_ID_CS2_AAM (1u << 9)
#define ATA_ID_CS2_LBA48 (1u << 10)
#define ATA_ID_CS2_FLUSH (1u << 12)
#define ATA_ID_CS2_FLUSH_EXT (1u << 13)
#define ATA_ID_CS2_EXT_VALID (1u << 15) /* word 86: words 119/120 valid */
/* Word 84 (supported) / 87 (enabled) */
#define ATA_ID_CSE_SMART_ERRLOG (1u << 0)
#define ATA_ID_CSE_SMART_SELFTEST (1u << 1)
#define ATA_ID_CSE_GPL (1u << 5) /* general purpose logging */
#define ATA_ID_CSE_FUA (1u << 6) /* WRITE DMA FUA EXT */
#define ATA_ID_CSE_WWN (1u << 8)
#define ATA_ID_CSE_UNLOAD (1u << 13) /* IDLE IMMEDIATE with UNLOAD */
/* Words 83/84/87 bits 15:14 = 01b when the word is valid */
#define ATA_ID_WORD_VALID(w) (((w) & 0xC000) == 0x4000)
/* Word 106: physical/logical sector size */
#define ATA_ID_SS_MULTI_LOGICAL (1u << 13) /* > 1 logical per physical */
#define ATA_ID_SS_LONG_LOGICAL (1u << 12) /* words 117/118 valid */
#define ATA_ID_SS_LOG2_PER_PHYS(w) ((w) & 0xF)
/* Word 119 (supported) / 120 (enabled) */
#define ATA_ID_CS3_WRV (1u << 1) /* write-read-verify */
#define ATA_ID_CS3_WRITE_UNC (1u << 2)
#define ATA_ID_CS3_LOG_DMA (1u << 3) /* READ/WRITE LOG DMA EXT */
#define ATA_ID_CS3_DL_MICRO_3 (1u << 4)
#define ATA_ID_CS3_FREEFALL (1u << 5)
#define ATA_ID_CS3_SENSE_DATA (1u << 6) /* sense data reporting */
#define ATA_ID_CS3_EPC (1u << 7)
#define ATA_ID_CS3_AMAX (1u << 8) /* accessible max address config */
#define ATA_ID_CS3_DSN (1u << 9)
/* Word 128: security status */
#define ATA_ID_SEC_SUPPORTED (1u << 0)
#define ATA_ID_SEC_ENABLED (1u << 1)
#define ATA_ID_SEC_LOCKED (1u << 2)
#define ATA_ID_SEC_FROZEN (1u << 3)
#define ATA_ID_SEC_COUNT_EXPIRED (1u << 4)
#define ATA_ID_SEC_ENH_ERASE (1u << 5)
#define ATA_ID_SEC_LEVEL_MAX (1u << 8) /* master password capability */
/* Word 169 */
#define ATA_ID_DSM_TRIM (1u << 0)
/* Word 209 (bits 15:14 = 01b valid) */
#define ATA_ID_ALIGN_OFFSET(w) ((w) & 0x3FFF)
/* Word 217 */
#define ATA_ID_ROT_NONE 0x0001 /* non-rotating (solid state) */
/* Word 255 */
#define ATA_ID_INTEGRITY_SIG 0xA5

/* Little helpers over an IDENTIFY page (pure). */
static inline uint32_t ata_id_u32(const uint16_t *id, int w)
{
	return (uint32_t)id[w] | ((uint32_t)id[w + 1] << 16);
}
static inline uint64_t ata_id_u64(const uint16_t *id, int w)
{
	return (uint64_t)id[w] | ((uint64_t)id[w + 1] << 16) |
	       ((uint64_t)id[w + 2] << 32) | ((uint64_t)id[w + 3] << 48);
}
static inline int ata_id_is_ata(const uint16_t *id)
{
	return (id[ATA_ID_CONFIG] & ATA_ID_CONFIG_NOT_ATA) == 0;
}
static inline int ata_id_is_atapi(const uint16_t *id)
{
	return (id[ATA_ID_CONFIG] & ATA_ID_CONFIG_ATAPI_MASK) ==
	       ATA_ID_CONFIG_ATAPI;
}
static inline int ata_id_has_lba48(const uint16_t *id)
{
	return ATA_ID_WORD_VALID(id[ATA_ID_COMMAND_SET_2]) &&
	       (id[ATA_ID_COMMAND_SET_2] & ATA_ID_CS2_LBA48) &&
	       ata_id_u64(id, ATA_ID_LBA_CAPACITY_2) != 0;
}
static inline int ata_id_has_ncq(const uint16_t *id)
{
	return id[ATA_ID_SATA_CAPABILITY] != 0xFFFF &&
	       (id[ATA_ID_SATA_CAPABILITY] & ATA_ID_SATA_NCQ);
}
static inline int ata_id_queue_depth(const uint16_t *id)
{
	return (id[ATA_ID_QUEUE_DEPTH] & 0x1F) + 1;
}
static inline int ata_id_rotational(const uint16_t *id)
{
	return id[ATA_ID_ROT_SPEED] != ATA_ID_ROT_NONE;
}
/* The checksum word: valid when the low byte is A5h and all 512 bytes
 * sum to zero; a page without the signature is accepted (older
 * devices). */
static inline int ata_id_checksum_ok(const uint16_t *id)
{
	uint8_t sum = 0;

	if ((id[ATA_ID_INTEGRITY] & 0xFF) != ATA_ID_INTEGRITY_SIG)
		return 1;
	for (int i = 0; i < ATA_ID_WORDS; i++)
		sum = (uint8_t)(sum + (id[i] & 0xFF) + (id[i] >> 8));
	return sum == 0;
}

/* ==========================================================================
 * SATA port multiplier registers (read/written with READ/WRITE PORT
 * MULTIPLIER on control port 15; device field = port, feature = register)
 * ========================================================================== */

/* General status and control registers (GSCR) */
#define SATA_PMP_GSCR_PROD_ID 0 /* 31:16 device id, 15:0 vendor id */
#define SATA_PMP_GSCR_REV 1 /* bits: 1 = 1.0, 2 = 1.1, 3 = 1.2 */
#define SATA_PMP_GSCR_PORT_INFO 2 /* 3:0 number of fan-out ports */
#define SATA_PMP_GSCR_ERROR 32 /* per-port error summary bits */
#define SATA_PMP_GSCR_ERROR_EN 33
#define SATA_PMP_GSCR_FEAT 64 /* features supported */
#define SATA_PMP_GSCR_FEAT_EN 96 /* features enabled */
#define SATA_PMP_GSCR_DWORDS 128
#define SATA_PMP_FEAT_BIST (1u << 0)
#define SATA_PMP_FEAT_PMREQ (1u << 1) /* PM requests to the PMP */
#define SATA_PMP_FEAT_DYNSSC (1u << 2)
#define SATA_PMP_FEAT_NOTIFY (1u << 3) /* asynchronous notification */
/* Port status and control registers (PSCR) of each fan-out port */
#define SATA_PMP_PSCR_STATUS 0 /* SStatus */
#define SATA_PMP_PSCR_ERROR 1 /* SError */
#define SATA_PMP_PSCR_CONTROL 2 /* SControl */
#define SATA_PMP_PSCR_ACTIVE 3 /* SActive */
#define SATA_PMP_PSCR_NOTIFY 4 /* SNotification */

/* ==========================================================================
 * Task file
 * ========================================================================== */

/* struct ata_taskfile.flags */
#define ATA_TFLAG_LBA48 (1u << 0) /* use the HOB (high-order) bytes */
#define ATA_TFLAG_ISADDR (1u << 1) /* the address registers are valid */
#define ATA_TFLAG_DEVICE (1u << 2) /* the device register is valid */
#define ATA_TFLAG_WRITE (1u << 3) /* data goes to the device */
#define ATA_TFLAG_LBA (1u << 4) /* LBA addressing */
#define ATA_TFLAG_FUA (1u << 5) /* forced unit access */
#define ATA_TFLAG_POLLING (1u << 6) /* no interrupt expected */

/* The ATA register set as one command sees it.  For FPDMA commands the
 * fields keep their FIS meaning: the sector count goes in feature/
 * hob_feature, the tag in nsect bits 7:3 (filled by the transport, which
 * owns tags), FUA in device bit 7. */
struct ata_taskfile {
	uint32_t flags; /* ATA_TFLAG_* */
	uint8_t command;
	uint8_t feature, hob_feature;
	uint8_t nsect, hob_nsect;
	uint8_t lbal, lbam, lbah; /* LBA 7:0, 15:8, 23:16 */
	uint8_t hob_lbal, hob_lbam, hob_lbah; /* LBA 31:24, 39:32, 47:40 */
	uint8_t device;
	uint8_t ctl; /* device control */
	uint8_t icc;
	uint32_t auxiliary;
	/* result (D2H FIS): */
	uint8_t status;
	uint8_t error;
};

/* Set the LBA (and the LBA flag) of a task file.  Pure. */
static inline void ata_tf_set_lba(struct ata_taskfile *tf, uint64_t lba,
				  int lba48)
{
	tf->flags |= ATA_TFLAG_ISADDR | ATA_TFLAG_LBA | ATA_TFLAG_DEVICE;
	tf->lbal = (uint8_t)lba;
	tf->lbam = (uint8_t)(lba >> 8);
	tf->lbah = (uint8_t)(lba >> 16);
	if (lba48) {
		tf->flags |= ATA_TFLAG_LBA48;
		tf->hob_lbal = (uint8_t)(lba >> 24);
		tf->hob_lbam = (uint8_t)(lba >> 32);
		tf->hob_lbah = (uint8_t)(lba >> 40);
		tf->device = ATA_LBA;
	} else {
		/* LBA28: bits 27:24 live in the device register */
		tf->device = (uint8_t)(ATA_LBA | ((lba >> 24) & 0x0F));
	}
}

/* The LBA a (result) task file carries.  Pure. */
static inline uint64_t ata_tf_read_lba(const struct ata_taskfile *tf)
{
	uint64_t lba = (uint64_t)tf->lbal | ((uint64_t)tf->lbam << 8) |
		       ((uint64_t)tf->lbah << 16);

	if (tf->flags & ATA_TFLAG_LBA48)
		lba |= ((uint64_t)tf->hob_lbal << 24) |
		       ((uint64_t)tf->hob_lbam << 32) |
		       ((uint64_t)tf->hob_lbah << 40);
	else
		lba |= (uint64_t)(tf->device & 0x0F) << 24;
	return lba;
}

/* Task file -> H2D register FIS (command bit set).  `pmp' is the port
 * multiplier port (0 without one).  Pure. */
static inline void ata_tf_to_fis(const struct ata_taskfile *tf, uint8_t pmp,
				 int is_cmd, struct fis_reg_h2d *fis)
{
	fis->fis_type = FIS_TYPE_REG_H2D;
	fis->pm_c = (uint8_t)((pmp & FIS_PMP_MASK) | (is_cmd ? FIS_H2D_C : 0));
	fis->command = tf->command;
	fis->feature_lo = tf->feature;
	fis->lba0 = tf->lbal;
	fis->lba1 = tf->lbam;
	fis->lba2 = tf->lbah;
	fis->device = tf->device;
	fis->lba3 = tf->hob_lbal;
	fis->lba4 = tf->hob_lbam;
	fis->lba5 = tf->hob_lbah;
	fis->feature_hi = tf->hob_feature;
	fis->count_lo = tf->nsect;
	fis->count_hi = tf->hob_nsect;
	fis->icc = tf->icc;
	fis->control = tf->ctl;
	fis->aux[0] = (uint8_t)tf->auxiliary;
	fis->aux[1] = (uint8_t)(tf->auxiliary >> 8);
	fis->aux[2] = (uint8_t)(tf->auxiliary >> 16);
	fis->aux[3] = (uint8_t)(tf->auxiliary >> 24);
}

/* D2H register FIS -> result task file (status, error and registers).
 * Pure. */
static inline void ata_tf_from_fis(const struct fis_reg_d2h *fis,
				   struct ata_taskfile *tf)
{
	tf->status = fis->status;
	tf->error = fis->error;
	tf->lbal = fis->lba0;
	tf->lbam = fis->lba1;
	tf->lbah = fis->lba2;
	tf->device = fis->device;
	tf->hob_lbal = fis->lba3;
	tf->hob_lbam = fis->lba4;
	tf->hob_lbah = fis->lba5;
	tf->nsect = fis->count_lo;
	tf->hob_nsect = fis->count_hi;
}

/* ==========================================================================
 * Synchronisation: completion and sleeping mutex
 * ========================================================================== */

/* True when the caller may block on a completion or mutex.  False before
 * the scheduler runs, with interrupts off, in a hard interrupt handler and
 * in the block softirq -- then every wait polls (blk_can_sleep). */
static inline int ata_can_sleep(void)
{
	return blk_can_sleep();
}

/* A one-shot "it is done" signal between a completer (interrupt handler,
 * hrtimer, the kahci thread, the polling loop) and one or more waiters.
 *
 *   ata_completion_init()      before first use (any context)
 *   ata_completion_reinit()    reuse for another round (no waiters!)
 *   ata_completion_complete()  IRQ-SAFE: marks done under the completion's
 *                              own spinlock, then wakes every waiter
 *   ata_completion_done()      non-blocking test
 *   ata_completion_wait()      waits with a timeout (ms, or
 *                              ATA_WAIT_FOREVER): sleeps when
 *                              ata_can_sleep(), otherwise polls -- calling
 *                              `poll(poll_arg)' between checks so a polled
 *                              controller makes progress -- in steps of
 *                              ATA_POLL_STEP_US.  Returns 0 (done) or
 *                              -ETIMEDOUT.  Uninterruptible: a pending
 *                              signal neither ends nor shortens it.
 *
 * The sleep follows the scheduler's publish-then-check rule: the waiter
 * checks `done' and publishes TASK_BLOCKED under `lock'; complete() sets
 * `done' under the same lock before waking, so a completion can never
 * fall between the check and the block.  A timed wait arms the tick
 * deadline (wakeup_tick) as the backstop and an hrtimer for precision. */
struct ata_completion {
	spinlock_t lock; /* leaf lock, irqsave */
	volatile int done;
	struct wait_queue_head wq;
	/* polled-mode progress hook (the transport's poll), may be NULL */
	int (*poll)(void *arg);
	void *poll_arg;
};

static inline void ata_completion_init(struct ata_completion *c,
				       const char *name)
{
	spinlock_init(&c->lock, name);
	c->done = 0;
	wq_head_init(&c->wq, name);
	c->poll = NULL;
	c->poll_arg = NULL;
}

static inline void ata_completion_reinit(struct ata_completion *c)
{
	uint64_t fl;

	spin_lock_irqsave(&c->lock, &fl);
	WARN_ON(c->wq.first != NULL); /* reinit with waiters on it */
	c->done = 0;
	spin_unlock_irqrestore(&c->lock, fl);
}

static inline void ata_completion_set_poll(struct ata_completion *c,
					   int (*poll)(void *arg), void *arg)
{
	c->poll = poll;
	c->poll_arg = arg;
}

static inline void ata_completion_complete(struct ata_completion *c)
{
	uint64_t fl;

	spin_lock_irqsave(&c->lock, &fl);
	__atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
	spin_unlock_irqrestore(&c->lock, fl);
	wq_wake_all(&c->wq);
}

static inline int ata_completion_done(const struct ata_completion *c)
{
	return __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);
}

static inline int ata_completion_wait(struct ata_completion *c,
				      uint32_t timeout_ms)
{
	if (!ata_can_sleep()) {
		/* Boot / interrupts off: poll.  The elapsed time is counted
		 * from the delays themselves, which works before any clock
		 * has been calibrated; it can only run long, never short. */
		uint64_t waited_us = 0;
		uint64_t limit_us = (uint64_t)timeout_ms * 1000ULL;

		for (;;) {
			if (ata_completion_done(c))
				return 0;
			if (c->poll)
				c->poll(c->poll_arg);
			if (ata_completion_done(c))
				return 0;
			if (timeout_ms != ATA_WAIT_FOREVER &&
			    waited_us >= limit_us)
				return -ETIMEDOUT;
			lapic_delay_us(ATA_POLL_STEP_US);
			waited_us += ATA_POLL_STEP_US;
		}
	}

	might_sleep();
	task_t *cur = sched_current();
	uint64_t deadline_ns = 0;

	if (timeout_ms != ATA_WAIT_FOREVER)
		deadline_ns =
			hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		struct wait_queue_entry we;
		uint64_t fl, now_ns;

		spin_lock_irqsave(&c->lock, &fl);
		if (c->done) {
			spin_unlock_irqrestore(&c->lock, fl);
			return 0;
		}
		now_ns = hrtimer_now_ns();
		if (deadline_ns && now_ns >= deadline_ns) {
			spin_unlock_irqrestore(&c->lock, fl);
			return -ETIMEDOUT;
		}
		wq_entry_init(&we, cur);
		wq_add(&c->wq, &we);
		cur->wait_channel = c;
		cur->wakeup_tick =
			deadline_ns ? timer_ticks() +
					      timer_ns_to_ticks(deadline_ns -
								now_ns) +
					      1
				    : 0;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&c->lock, fl);
		if (deadline_ns)
			hrtimer_wait_arm(cur, deadline_ns);
		sched_schedule();
		if (deadline_ns)
			hrtimer_wait_disarm(cur);
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		wq_remove(&c->wq, &we);
	}
}

/* The driver-local sleeping mutex (port eh_mutex, and any other sleeping
 * serialisation in the ATA/AHCI driver).  Built like the USB mass storage
 * driver's msd_io_lock(): a flag and an owner task id under a small
 * spinlock; a contended locker publishes TASK_BLOCKED with the flag's
 * address as its wait channel and is woken by sched_wake_channel() on
 * release.  Implemented in ata_core.c (A4).
 *
 * Semantics:
 *   - not recursive: re-locking from the owner is a BUG_ON;
 *   - lock() may sleep.  Before the scheduler exists or with interrupts
 *     off, it takes a free mutex at once and spins (pause) on a held one --
 *     only another CPU can hold it then, and boot is single-threaded;
 *   - unlock() only by the owner (WARN and ignore otherwise);
 *   - counted as a sleeping lock (task_sleep_lock_taken/dropped);
 *   - ata_mutex_held(): the CALLING task holds it (assertion helper);
 *   - ata_mutex_release_if_owner(): orphan recovery for a killed task. */
struct ata_mutex {
	volatile int locked; /* 0 free, 1 held */
	volatile uint64_t owner; /* holder's task id, (uint64_t)-1 none */
	const char *name;
	spinlock_t wait_lock;
	uint32_t contended; /* diagnostics */
};

#define ATA_MUTEX_INIT(n)                                          \
	{                                                          \
		.locked = 0, .owner = (uint64_t)-1, .name = (n),   \
		.wait_lock = SPINLOCK_INIT(n), .contended = 0      \
	}

void ata_mutex_init(struct ata_mutex *m, const char *name); /* any ctx */
void ata_mutex_lock(struct ata_mutex *m); /* may sleep */
int ata_mutex_trylock(struct ata_mutex *m); /* 1 taken, 0 busy; any ctx */
void ata_mutex_unlock(struct ata_mutex *m);
int ata_mutex_held(const struct ata_mutex *m);
int ata_mutex_release_if_owner(struct ata_mutex *m, uint64_t task_id);

/* ==========================================================================
 * Error masks (struct ata_queued_cmd.err_mask)
 * ========================================================================== */

#define AC_ERR_DEV (1u << 0) /* the device reported an error */
#define AC_ERR_HSM (1u << 1) /* host state machine / protocol violation */
#define AC_ERR_TIMEOUT (1u << 2) /* timed out */
#define AC_ERR_MEDIA (1u << 3) /* media error (UNC, IDNF ...) */
#define AC_ERR_ATA_BUS (1u << 4) /* SATA link error (CRC, PHY) */
#define AC_ERR_HOST_BUS (1u << 5) /* host bus (PCI) fatal/data error */
#define AC_ERR_SYSTEM (1u << 6) /* system error: memory, DMA mapping */
#define AC_ERR_INVALID (1u << 7) /* invalid argument to the command */
#define AC_ERR_OTHER (1u << 8) /* unknown */
#define AC_ERR_NODEV_HINT (1u << 9) /* the device may have gone */
#define AC_ERR_NCQ (1u << 10) /* NCQ error (needs log 10h) */
#define AC_ERR_ABORTED (1u << 11) /* aborted by EH (innocent victim) */

/* ==========================================================================
 * The device
 * ========================================================================== */

/* struct ata_device.flags (capabilities found and state) */
#define ATA_DFLAG_LBA (1ULL << 0)
#define ATA_DFLAG_LBA48 (1ULL << 1)
#define ATA_DFLAG_NCQ (1ULL << 2) /* device supports NCQ */
#define ATA_DFLAG_NCQ_ON (1ULL << 3) /* NCQ in use (host + policy) */
#define ATA_DFLAG_FUA (1ULL << 4)
#define ATA_DFLAG_FLUSH (1ULL << 5) /* FLUSH CACHE */
#define ATA_DFLAG_FLUSH_EXT (1ULL << 6)
#define ATA_DFLAG_WCACHE (1ULL << 7) /* volatile write cache supported */
#define ATA_DFLAG_WCACHE_ON (1ULL << 8)
#define ATA_DFLAG_RLA (1ULL << 9) /* read look-ahead supported */
#define ATA_DFLAG_RLA_ON (1ULL << 10)
#define ATA_DFLAG_TRIM (1ULL << 11)
#define ATA_DFLAG_TRIM_DRAT (1ULL << 12) /* deterministic after TRIM */
#define ATA_DFLAG_TRIM_RZAT (1ULL << 13) /* zeroes after TRIM */
#define ATA_DFLAG_NCQ_TRIM (1ULL << 14) /* queued TRIM (SEND FPDMA) */
#define ATA_DFLAG_SMART (1ULL << 15)
#define ATA_DFLAG_SMART_ON (1ULL << 16)
#define ATA_DFLAG_SECURITY (1ULL << 17)
#define ATA_DFLAG_APM (1ULL << 18)
#define ATA_DFLAG_AAM (1ULL << 19)
#define ATA_DFLAG_PUIS (1ULL << 20) /* power-up in standby enabled */
#define ATA_DFLAG_SPINUP_REQ (1ULL << 21) /* needs SET FEATURES spin-up */
#define ATA_DFLAG_HIPM (1ULL << 22)
#define ATA_DFLAG_DIPM (1ULL << 23)
#define ATA_DFLAG_DIPM_ON (1ULL << 24)
#define ATA_DFLAG_DEVSLP (1ULL << 25)
#define ATA_DFLAG_AN (1ULL << 26) /* asynchronous notification enabled */
#define ATA_DFLAG_GPL (1ULL << 27) /* READ LOG EXT */
#define ATA_DFLAG_LOG_DMA (1ULL << 28) /* READ LOG DMA EXT */
#define ATA_DFLAG_SENSE_DATA (1ULL << 29) /* sense data reporting */
#define ATA_DFLAG_NCQ_AUTOSENSE (1ULL << 30)
#define ATA_DFLAG_NCQ_SEND_RECV (1ULL << 31)
#define ATA_DFLAG_WWN (1ULL << 32)
#define ATA_DFLAG_REMOVABLE (1ULL << 33)
#define ATA_DFLAG_ZAC (1ULL << 34)
#define ATA_DFLAG_HPA (1ULL << 35) /* host protected area present */
#define ATA_DFLAG_UNLOAD (1ULL << 36)
#define ATA_DFLAG_NONVOL_WC (1ULL << 37) /* write cache non-volatile */
#define ATA_DFLAG_SANITIZE (1ULL << 38)
#define ATA_DFLAG_DL_MICRO (1ULL << 39)
#define ATA_DFLAG_ATAPI_DMA (1ULL << 40) /* ATAPI data phase by DMA */
#define ATA_DFLAG_ATAPI_DMADIR (1ULL << 41)
#define ATA_DFLAG_CDB16 (1ULL << 42) /* 16-byte packets */
/* state */
#define ATA_DFLAG_CONFIGURED (1ULL << 48) /* ata_dev_configure() done */
#define ATA_DFLAG_REGISTERED (1ULL << 49) /* its disk is added */
#define ATA_DFLAG_DETACHED (1ULL << 50) /* unplugged: fail everything */
#define ATA_DFLAG_SUSPENDED (1ULL << 51)
#define ATA_DFLAG_SLEEPING (1ULL << 52) /* in SLEEP: reset to wake */
#define ATA_DFLAG_FROZEN_SEC (1ULL << 53) /* we froze security */
#define ATA_DFLAG_NEEDS_REVALIDATE (1ULL << 54)
#define ATA_DFLAG_DISABLED (1ULL << 55) /* EH gave up on it */

/* Power state (CHECK POWER MODE count register / our view) */
enum ata_power_state {
	ATA_PWR_UNKNOWN = 0,
	ATA_PWR_ACTIVE, /* active or idle (FFh / 80h ..) */
	ATA_PWR_IDLE,
	ATA_PWR_STANDBY, /* 00h */
	ATA_PWR_SLEEP, /* SLEEP issued: only a reset wakes it */
};
/* CHECK POWER MODE result values (count register) */
#define ATA_CHK_PWR_STANDBY 0x00
#define ATA_CHK_PWR_STANDBY_Y 0x01 /* EPC standby_y */
#define ATA_CHK_PWR_IDLE 0x80
#define ATA_CHK_PWR_IDLE_A 0x81
#define ATA_CHK_PWR_IDLE_B 0x82
#define ATA_CHK_PWR_IDLE_C 0x83
#define ATA_CHK_PWR_ACTIVE 0xFF

/* Security state decoded from IDENTIFY word 128 */
struct ata_security {
	uint16_t word128; /* raw */
	uint8_t supported, enabled, locked, frozen, count_expired;
	uint8_t enhanced_erase; /* enhanced erase supported */
	uint8_t level_max; /* 1 = maximum, 0 = high */
	uint8_t pad;
	uint32_t erase_time_min; /* normal erase estimate, minutes (0 ?) */
	uint32_t enh_erase_time_min; /* enhanced erase estimate, minutes */
	uint16_t master_pw_rev;
	uint16_t pad2;
};

/* Link facts the transport reports (sysfs, HDIO, diagnostics) */
struct ata_link_info {
	uint32_t sstatus, scontrol, serror; /* raw SCRs */
	uint8_t speed; /* 0 none, 1 1.5, 2 3.0, 3 6.0 Gb/s */
	uint8_t speed_limit; /* 0 none, else max allowed */
	uint8_t ipm; /* 1 active, 2 partial, 6 slumber, 8 devsleep */
	uint8_t pmp; /* port multiplier port (0 none) */
	uint8_t pmp_attached; /* a port multiplier sits on the port */
	uint8_t sntf; /* the host reports notifiers per PM port (SNotification) */
	int port_no; /* ATA port number ("ata1" = 1) */
	int lpm_policy; /* enum ahci_lpm_policy value */
};

/* The transport: how the ATA layer reaches a device's host controller.
 * The AHCI driver provides one instance (ahci_cmd.c: ahci_ata_transport);
 * the host tests provide a fake. */
#define ATA_RESET_SOFT 1 /* SRST (software reset) */
#define ATA_RESET_HARD 2 /* COMRESET of the link */
#define ATA_RESET_PORT 3 /* the whole port, PMP and all links */
struct ata_transport {
	const char *name; /* "ahci" */
	/* Queue one command.  0: accepted -- ata_qc_complete() will be called
	 * exactly once, from any context.  -EBUSY: no slot/tag now, or the
	 * command cannot run next to what runs (a non-queued command while
	 * queued ones are outstanding, a queued one while a non-queued one
	 * runs, another device's command behind a port multiplier without
	 * FIS-based switching): the caller retries after a completion --
	 * the block path gives the request back to the head of its queue,
	 * which dispatches nothing behind it meanwhile.  -ENODEV: link or
	 * device gone.  -EAGAIN: the port is in error handling and the
	 * command is not an EH command (ATA_QCFLAG_EH); re-submit after EH
	 * (the block path requeues).  -EINVAL: malformed (sg too long, odd
	 * length).  Any context; never sleeps. */
	int (*qc_issue)(struct ata_queued_cmd *qc);
	/* Drive completions without interrupts.  Returns the number of
	 * commands completed.  Any context. */
	int (*poll)(struct ata_device *dev);
	/* Reset the device's link/port (ATA_RESET_*) through error handling
	 * and re-validate the device.  May sleep (takes the port eh_mutex). */
	int (*reset)(struct ata_device *dev, int kind);
	/* Ask for asynchronous error handling of the device's port with these
	 * AHCI_EH_* actions.  Any context. */
	void (*schedule_eh)(struct ata_device *dev, uint32_t actions);
	/* Apply a link power management policy (enum ahci_lpm_policy).  May
	 * sleep. */
	int (*set_lpm)(struct ata_device *dev, int policy);
	/* Current link facts.  Any context. */
	void (*link_info)(struct ata_device *dev, struct ata_link_info *out);
	/* Serialise device-state changes against error handling: takes /
	 * drops the port's eh_mutex.  May sleep. */
	void (*lock)(struct ata_device *dev);
	void (*unlock)(struct ata_device *dev);
	/* A command passed its timeout: the waiter's own bounded wait ran out
	 * (`qc', ata_qc_exec) or the block queue's timeout work found the
	 * request expired (`req', compared with what the commands carry --
	 * never dereferenced: it may have completed meanwhile).  Under the
	 * transport's lock: if the command is still at the hardware it is
	 * marked timed out and handed to error handling (which completes it
	 * with -ETIMEDOUT); a command issued by the holder of the port's
	 * error-handling mutex is completed at once (no error handling could
	 * run for it).  1 when a command was found, 0 when it has completed
	 * already.  Process context (it may complete commands). */
	int (*qc_timed_out)(struct ata_device *dev, struct ata_queued_cmd *qc,
			    const struct blk_request *req);
};

struct ata_device {
	uint32_t magic; /* ATA_DEVICE_MAGIC */
	char name[16]; /* "ata1.00", "ata2.03" (PMP link 3) */
	int port_no; /* ATA port number, 1-based */
	int devno; /* PMP link number, 0 without a PMP */
	enum ata_class class;
	uint64_t flags; /* ATA_DFLAG_* */
	refcount_t refcount;

	/* the transport and its per-link context (struct ahci_link *) */
	const struct ata_transport *xport;
	void *xport_priv;
	/* transport limits, set by the transport before ata_dev_probe() */
	uint32_t host_max_segments; /* PRDT entries per command */
	uint32_t host_max_sectors; /* per command (512-byte units) */
	uint32_t host_queue_depth; /* NCQ tags the host can give (0 none) */
	uint32_t host_dma_alignment; /* mask (AHCI: 1, word aligned) */
	int host_ncq; /* host side NCQ support (CAP.SNCQ) */
	int host_fua; /* allow FUA (policy) */

	/* identity */
	uint16_t id[ATA_ID_WORDS]; /* IDENTIFY (PACKET) DEVICE, CPU order */
	char model[ATA_ID_PROD_LEN + 1];
	char serial[ATA_ID_SERNO_LEN + 1];
	char firmware[ATA_ID_FW_REV_LEN + 1];
	uint64_t wwn; /* 0 when not reported */
	uint16_t major_version, minor_version;
	uint16_t form_factor;
	uint16_t rotation_rate; /* 1 = SSD, else rpm, 0 unknown */

	/* geometry and capacity */
	uint64_t n_sectors; /* user-addressable logical sectors */
	uint64_t n_native_sectors; /* native max (HPA), 0 unknown */
	uint32_t logical_sector_size; /* bytes: 512 or 4096 */
	uint32_t physical_sector_size; /* bytes */
	uint32_t alignment_offset; /* bytes (word 209) */
	uint8_t log2_per_phys; /* logical sectors per physical, log2 */
	uint16_t cylinders, heads, sectors; /* default CHS (obsolete) */
	uint32_t max_sectors; /* per command: LBA28 256, LBA48 65535
			       * (capped by host_max_sectors) */
	uint8_t multi_count; /* READ/WRITE MULTIPLE size */

	/* transfer modes */
	uint8_t pio_mask, mwdma_mask, udma_mask;
	uint8_t xfer_mode; /* XFER_* in use */
	uint8_t sata_speed_max; /* from word 76: 1..3 */

	/* NCQ */
	uint32_t ncq_depth; /* in use: min(device, host), 0 = off */
	uint32_t ncq_depth_dev; /* device maximum */

	/* TRIM */
	uint32_t dsm_max_blocks; /* 512-byte DSM blocks per command */
	uint64_t trim_max_sectors; /* per command (ranges x 65535) */

	/* power */
	enum ata_power_state power_state;
	uint8_t apm_level; /* 01h..FEh, 0 when off */
	uint8_t aam_level;
	uint8_t standby_timer; /* encoded STANDBY timer, 0 off */

	/* security */
	struct ata_security security;

	/* ATAPI */
	uint8_t cdb_len; /* 12 or 16 */
	uint8_t atapi_type; /* peripheral type (TYPE_ROM) */
	uint8_t media_present;
	uint8_t media_changed; /* unit attention seen */
	uint8_t door_locked;
	uint8_t pad_atapi[3];
	uint32_t media_poll_ms; /* events poll interval, 0 = no polling */

	/* block layer */
	struct gendisk *disk;

	/* error accounting */
	uint32_t n_errors; /* commands that failed */
	uint32_t n_timeouts;
	/* Bumped (atomically) whenever a command of this device came back
	 * from ata_qc_exec() still flagged ATA_QCFLAG_HW_OWNED: the
	 * controller may yet write into its buffer.  Paths that free an
	 * internal buffer after a call that hides the qc snapshot it before
	 * and keep the buffer when it moved (ata_dev_hw_owned_since). */
	uint32_t hw_owned_seq;
	uint32_t n_retries;
	uint32_t n_media_errors;
	uint32_t n_bus_errors;
	uint32_t n_resets;
	uint32_t n_revalidations;
	uint64_t last_error_ns;
	struct ata_taskfile last_error_tf; /* diagnostics */
	uint8_t last_error_cmd;
};

#define ATA_DEVICE_MAGIC 0x56454441u /* "ADEV" */

/* ==========================================================================
 * Queued commands
 * ========================================================================== */

/* Protocol of a command: how the data moves */
enum ata_prot {
	ATA_PROT_UNKNOWN = 0,
	ATA_PROT_NODATA, /* no data phase */
	ATA_PROT_PIO_IN, /* PIO data-in (IDENTIFY, SMART READ ...) */
	ATA_PROT_PIO_OUT, /* PIO data-out (SECURITY, DSM via PIO) */
	ATA_PROT_DMA_IN, /* DMA read */
	ATA_PROT_DMA_OUT, /* DMA write */
	ATA_PROT_FPDMA, /* NCQ (direction from ATA_QCFLAG_WRITE) */
	ATA_PROT_ATAPI_PIO, /* PACKET, PIO data phase */
	ATA_PROT_ATAPI_DMA, /* PACKET, DMA data phase */
	ATA_PROT_ATAPI_NODATA, /* PACKET, no data */
	/* Reset sequences the transport runs itself (SRST FIS pair) */
	ATA_PROT_SRST,
};

static inline int ata_prot_is_atapi(enum ata_prot p)
{
	return p == ATA_PROT_ATAPI_PIO || p == ATA_PROT_ATAPI_DMA ||
	       p == ATA_PROT_ATAPI_NODATA;
}
static inline int ata_prot_is_dma(enum ata_prot p)
{
	return p == ATA_PROT_DMA_IN || p == ATA_PROT_DMA_OUT ||
	       p == ATA_PROT_FPDMA || p == ATA_PROT_ATAPI_DMA;
}

/* Data direction of a command */
enum ata_dma_dir {
	ATA_DIR_NONE = 0,
	ATA_DIR_TO_DEVICE = 1,
	ATA_DIR_FROM_DEVICE = 2,
};

/* struct ata_queued_cmd.flags */
#define ATA_QCFLAG_WRITE (1u << 0) /* data to the device */
#define ATA_QCFLAG_IO (1u << 1) /* carries block layer I/O (req) */
#define ATA_QCFLAG_INTERNAL (1u << 2) /* issued by the driver itself */
#define ATA_QCFLAG_EH (1u << 3) /* error handling: may issue while the
				  * port is frozen / in EH */
#define ATA_QCFLAG_RESULT_TF (1u << 4) /* copy the D2H FIS to result_tf */
#define ATA_QCFLAG_QUIET (1u << 5) /* no error log on failure */
#define ATA_QCFLAG_RETRY (1u << 6) /* EH may retry it */
#define ATA_QCFLAG_FAILED (1u << 7) /* EH owns it */
#define ATA_QCFLAG_SENSE_VALID (1u << 8) /* sense[] filled */
#define ATA_QCFLAG_POLLED (1u << 9) /* force polled completion */
#define ATA_QCFLAG_TIMEDOUT (1u << 10)
#define ATA_QCFLAG_ABORTED (1u << 11)
#define ATA_QCFLAG_ACTIVE (1u << 12) /* owned by the transport */
#define ATA_QCFLAG_COMPLETE (1u << 13) /* ata_qc_complete() ran */
#define ATA_QCFLAG_DMADIR (1u << 14) /* ATAPI DMADIR feature bit */
#define ATA_QCFLAG_PASSTHRU (1u << 15) /* from SG_IO / HDIO (user) */
/* [T] The command was completed (failed) while the controller may still
 * own its slot and so its data buffer: the issuer must neither free nor
 * reuse that buffer.  Set by the transport; ata_qc_exec() clears it when
 * it got the slot back (an engine stop by the eh_mutex holder), a
 * re-issue does not. */
#define ATA_QCFLAG_HW_OWNED (1u << 16)
/* [T] Error handling ended the command for a retry (an innocent victim of
 * another command's error, or a retry its policy allows): the command
 * carries a block request, and the request goes back to its queue to be
 * dispatched again -- with this command's retry count -- instead of the
 * transport re-issuing it.  ata_qc_complete() leaves the result alone. */
#define ATA_QCFLAG_EH_RETRY (1u << 17)

/* One ATA (or ATAPI) command from preparation to completion.
 *
 * Lifetime: allocated by its user (ata_qc_alloc(), or embedded/stack for
 * internal commands with ata_qc_init()), refcounted: one reference for
 * the owner and one taken by the transport while ACTIVE.  The transport
 * fills the fields marked [T]; the ATA layer the rest.
 *
 * Completion (ata_qc_complete(), called by the transport exactly once, in
 * any context, with NO transport spinlock held): sets result/err_mask,
 * calls end_io (IRQ-safe callbacks only), then completes `done'. */
struct ata_queued_cmd {
	uint32_t magic; /* ATA_QC_MAGIC */
	struct ata_device *dev;
	enum ata_prot protocol;
	uint32_t flags; /* ATA_QCFLAG_* */
	enum ata_dma_dir dma_dir;

	struct ata_taskfile tf; /* the command */
	struct ata_taskfile result_tf; /* [T] status/error/registers */

	uint8_t cdb[ATAPI_CDB_LEN_MAX]; /* ATAPI packet */
	uint8_t cdb_len;

	/* data: a scatter/gather list in the block layer's format (the same
	 * memory as the blk_request's, or one built for an internal buffer
	 * via ata_qc_set_buf()) */
	struct blk_sg_entry *sg;
	uint32_t n_elem;
	uint32_t nbytes; /* total bytes */
	struct blk_sg_entry sg_one; /* for single-buffer commands */

	uint32_t timeout_ms; /* 0 = per-protocol default */
	uint32_t retries; /* attempts so far */
	uint32_t max_retries; /* allowed by EH (ATA_MAX_RETRIES) */

	/* result */
	int result; /* 0 or -errno */
	uint32_t err_mask; /* AC_ERR_* */
	uint32_t resid; /* [T] bytes not transferred */
	uint8_t sense[ATA_SENSE_LEN]; /* ATAPI sense / SAT sense */
	uint32_t sense_len;

	/* completion */
	struct ata_completion done;
	/* Called from ata_qc_complete() -- in the interrupt handler, with the
	 * transport lock held -- after the transport reference was dropped
	 * (the owner's keeps the command): records what the block softirq
	 * needs and calls blk_complete_request().  May be NULL. */
	void (*end_io)(struct ata_queued_cmd *qc);
	void *private_data; /* end_io's context */
	struct blk_request *req; /* the block request it carries, if IO */
	uint64_t lba; /* for logs/traces: first LBA */
	uint32_t n_sect; /* for logs/traces */

	/* [T] the transport's while ACTIVE */
	int tag; /* NCQ tag / slot, -1 when not issued */
	uint64_t issue_ns;
	void *xport_cmd; /* struct ahci_cmd * */
	struct ata_queued_cmd *xport_next; /* transport's deferred list */

	refcount_t refcount;
};

#define ATA_QC_MAGIC 0x43514154u /* "TAQC" */

/* ==========================================================================
 * ata_core.c (A4)
 * ========================================================================== */

/* Allocate a device for a link.  `devno' is the PMP link (0 none),
 * `port_no' the 1-based ATA port number for names.  The transport fills
 * host_* limits afterwards.  NULL on -ENOMEM.  Any context that may
 * allocate. */
struct ata_device *ata_dev_alloc(const struct ata_transport *xport,
				 void *xport_priv, int port_no, int devno);
void ata_dev_get(struct ata_device *dev);
void ata_dev_put(struct ata_device *dev); /* frees at zero; may sleep */

/* Bring a classified device up: IDENTIFY (or IDENTIFY PACKET), check it,
 * parse it (ata_id_parse), spin it up if needed (PUIS), configure it
 * (ata_dev_configure: transfer mode, write cache, look-ahead, NCQ, DIPM,
 * APM, security freeze), and register its disk (sdX) or optical drive
 * (srN, via atapi_dev_register()).  Logs the device line ("ata1.00: ATA-11:
 * Samsung SSD ..., 976773168 sectors, multi 16: LBA48 NCQ (depth 32)").
 * Caller holds the port eh_mutex.  May sleep (polls at boot). */
int ata_dev_probe(struct ata_device *dev, enum ata_class class);
/* After a reset: re-IDENTIFY, verify it is the same device (model,
 * serial, capacity -- a different one is detached and re-probed),
 * re-apply the configuration.  eh_mutex held.  May sleep. */
int ata_dev_revalidate(struct ata_device *dev);
/* Hot-unplug / give up: mark DETACHED, fail outstanding block requests
 * (-ENODEV), remove the disk.  eh_mutex held.  May sleep. */
void ata_dev_detach(struct ata_device *dev);

/* Issue IDENTIFY (PACKET) DEVICE into `id' (CPU-order words).  May sleep. */
int ata_dev_read_id(struct ata_device *dev, enum ata_class class,
		    uint16_t *id);
/* Parse an IDENTIFY page into dev: strings, capacity (words 60-61,
 * 100-103, 230-233), sector sizes (106, 117-118, 209), features
 * (82-87, 119-120, 76-79, 69, 169, 105), security (128, 89, 90, 92),
 * NCQ depth (75), CHS (1, 3, 6), modes (63, 64, 88), rotation (217).
 * -EINVAL for an unusable page (no LBA, zero capacity, bad checksum,
 * logical sector size not a power of two in 512..65536).  PURE (no I/O,
 * no locks) -- the host tests call it on captured pages. */
int ata_id_parse(struct ata_device *dev, const uint16_t *id);
/* ATA string words -> C string: bytes swapped within each word,
 * trailing spaces/NULs trimmed, NUL terminated (out has len+1 bytes).
 * Pure. */
void ata_id_c_string(const uint16_t *id, char *out, int word_ofs, int len);

/* Queued-command helpers */
struct ata_queued_cmd *ata_qc_alloc(struct ata_device *dev); /* -> NULL */
void ata_qc_free(struct ata_queued_cmd *qc); /* drops the owner's ref */
void ata_qc_get(struct ata_queued_cmd *qc);
void ata_qc_put(struct ata_queued_cmd *qc);
/* Initialise an embedded/stack command: zeroes, magic, dev, refcount 1,
 * tag -1, completion (with the transport's poll hook), defaults. */
void ata_qc_init(struct ata_queued_cmd *qc, struct ata_device *dev);
/* Point the command at one kernel buffer (internal commands). */
void ata_qc_set_buf(struct ata_queued_cmd *qc, void *buf, uint32_t len,
		    enum ata_dma_dir dir);
/* The transport's completion call: the ATA-level end of a command.  Any
 * context -- the transport's interrupt handler calls it with its port
 * lock held.  Decodes result_tf into result/err_mask
 * (ata_qc_decode_error), completes `done' (a waiter of ata_qc_exec wakes),
 * drops the transport reference and calls end_io, which only hands the
 * block request on to the block softirq (blk_complete_request).  Never
 * issues a command and never enters the block layer's request end. */
void ata_qc_complete(struct ata_queued_cmd *qc);
/* Execute a command and wait for its completion.  A caller outside error
 * handling (no eh_mutex, not ATA_QCFLAG_EH) on a disk that is up goes
 * through the disk's request queue as a pass-through request, ordered
 * with the filesystem's I/O (its timeout is the queue's).  Otherwise --
 * error handling, boot, no live disk -- the command is issued through
 * the transport directly (-EBUSY retried until the timeout) and the
 * timeout is this wait's own: when it runs out the transport's
 * qc_timed_out hands the command to error handling, which completes it
 * with -ETIMEDOUT.  Returns qc->result.  May sleep (polls at boot). */
int ata_qc_exec(struct ata_queued_cmd *qc);
/* Build and run an internal non-NCQ command in one call: `tf' the command
 * (result written back into it), `buf'/`len'/`dir' the data (NULL/0/
 * ATA_DIR_NONE for none), `prot' the protocol, timeout in ms (0 =
 * default), `qcflags' extra ATA_QCFLAG_* (EH, QUIET ...).  Returns 0 or
 * -errno; on -EIO the result status/error are in tf.  May sleep. */
int ata_exec_internal(struct ata_device *dev, struct ata_taskfile *tf,
		      const uint8_t *cdb, enum ata_prot prot, void *buf,
		      uint32_t len, enum ata_dma_dir dir, uint32_t timeout_ms,
		      uint32_t qcflags);
/* Status/error -> err_mask and -errno.  Pure. */
int ata_qc_decode_error(struct ata_queued_cmd *qc);
/* The -errno an err_mask means.  Pure. */
int ata_err_mask_to_errno(uint32_t err_mask, uint8_t status, uint8_t error);

/* Serialise device-state changes with EH (transport lock/unlock). */
void ata_dev_lock(struct ata_device *dev);
void ata_dev_unlock(struct ata_device *dev);

/* Diagnostics (pure): opcode -> "READ FPDMA QUEUED"; status/error bits ->
 * "{ DRDY ERR }" / "{ UNC IDNF ABRT }" into buf. */
const char *ata_cmd_name(uint8_t command);
int ata_status_str(uint8_t status, char *buf, int cap);
int ata_error_str(uint8_t error, char *buf, int cap);
/* Log one failed command the established way (rate-limited):
 * "ata1.00: failed command: READ FPDMA QUEUED" + taskfile + decode. */
void ata_dev_report_error(struct ata_device *dev,
			  const struct ata_queued_cmd *qc);

/* ==========================================================================
 * ata_rw.c (A4)
 * ========================================================================== */

/* The blk_ops of an ATA disk (submit/poll/ioctl/shutdown/suspend/resume/
 * free_disk).  .ioctl = ata_sat_blk_ioctl (ata_sat.c). */
extern const struct blk_ops ata_disk_blk_ops;

/* Choose and build the command for a read/write: FPDMA when NCQ is on,
 * else READ/WRITE DMA EXT (LBA48) or DMA (LBA28), WRITE DMA FUA EXT for
 * FUA without NCQ; FPDMA FUA via device bit 7.  `count' 1..max_sectors.
 * Fills qc->tf, protocol, flags, dma_dir.  -EINVAL out of range / too
 * long, -EOPNOTSUPP FUA without support.  PURE (host-testable). */
int ata_rw_build_tf(const struct ata_device *dev, struct ata_queued_cmd *qc,
		    uint64_t lba, uint32_t count, int write, int fua);
/* blk_ops.submit of an ATA disk: translates READ/WRITE (split by
 * max_sectors into chained commands when larger), FLUSH, DISCARD/
 * SECURE_ERASE (ata_trim), WRITE_ZEROES (ZERO EXT / TRIM+RZAT / writes),
 * DRV_IN/DRV_OUT (the prepared qc in req->drv_cmd).  Any context. */
int ata_blk_submit(struct gendisk *disk, struct blk_request *req);
int ata_blk_poll(struct gendisk *disk);
/* FLUSH CACHE EXT (or FLUSH CACHE).  May sleep. */
int ata_flush_cache(struct ata_device *dev);
/* Internal synchronous sector I/O (DMA, non-NCQ), used before the disk is
 * registered and by EH tests.  May sleep. */
int ata_read_sectors(struct ata_device *dev, uint64_t lba, uint32_t count,
		     void *buf);
int ata_write_sectors(struct ata_device *dev, uint64_t lba, uint32_t count,
		      const void *buf);
/* Register the sd disk of an ATA device (limits from the device and the
 * host: logical/physical size, alignment, max sectors, segments, queue
 * depth = ncq_depth or 1, discard limits, write cache, FUA, rotational;
 * identity strings; CHS for HDIO_GETGEO).  eh_mutex held.  May sleep. */
int ata_disk_register(struct ata_device *dev);
void ata_disk_unregister(struct ata_device *dev);

/* ==========================================================================
 * ata_features.c (A4)
 * ========================================================================== */

/* SET FEATURES with a subcommand and count.  May sleep. */
int ata_set_features(struct ata_device *dev, uint8_t subcmd, uint8_t count);
int ata_set_write_cache(struct ata_device *dev, int on);
int ata_set_read_lookahead(struct ata_device *dev, int on);
int ata_set_xfer_mode(struct ata_device *dev, uint8_t mode);
int ata_set_sata_feature(struct ata_device *dev, uint8_t feature, int on);
int ata_set_apm(struct ata_device *dev, uint8_t level); /* 0 = disable */
int ata_set_aam(struct ata_device *dev, uint8_t level); /* 0 = disable */
int ata_set_puis(struct ata_device *dev, int on);
int ata_spinup(struct ata_device *dev); /* SET FEATURES 07h after PUIS */
int ata_set_multiple(struct ata_device *dev, uint8_t count);
int ata_set_sense_reporting(struct ata_device *dev, int on);
/* The configuration policy after IDENTIFY and after every reset: highest
 * UDMA mode, write cache on, look-ahead on, NCQ depth, DIPM per LPM
 * policy, APM 254 (if supported and not set otherwise), AN for ATAPI,
 * sense data reporting, security freeze lock.  May sleep. */
int ata_dev_configure(struct ata_device *dev);
/* READ LOG EXT (or READ LOG DMA EXT when supported and allowed) of
 * `count' 512-byte pages starting at `page' of `log'.  `qcflags' lets EH
 * pass ATA_QCFLAG_EH.  May sleep. */
int ata_read_log_ext(struct ata_device *dev, uint8_t log, uint16_t page,
		     void *buf, uint16_t count, uint32_t qcflags);
/* The general purpose log directory (log 00h): pages per log. */
int ata_read_log_directory(struct ata_device *dev, uint16_t *pages256);
/* EXECUTE DEVICE DIAGNOSTIC: *code = the diagnostic code (01h = passed).
 * May sleep. */
int ata_execute_diagnostic(struct ata_device *dev, uint8_t *code);
/* READ NATIVE MAX ADDRESS (EXT): the native capacity (sectors). */
int ata_read_native_max(struct ata_device *dev, uint64_t *sectors);
/* SET MAX ADDRESS (EXT): set the user capacity (HPA), volatile unless
 * `persist'.  May sleep. */
int ata_set_max_address(struct ata_device *dev, uint64_t sectors,
			int persist);

/* ==========================================================================
 * ata_pm.c (A4)
 * ========================================================================== */

int ata_check_power_mode(struct ata_device *dev, uint8_t *mode);
int ata_standby_immediate(struct ata_device *dev);
int ata_idle_immediate(struct ata_device *dev, int unload_heads);
/* STANDBY / IDLE with a timer in seconds (encoded per the standby timer
 * table: ata_standby_timer_encode). */
int ata_standby(struct ata_device *dev, uint32_t seconds);
int ata_idle(struct ata_device *dev, uint32_t seconds);
int ata_sleep(struct ata_device *dev);
/* Wake a device in STANDBY (any media access) or SLEEP (needs a reset
 * through the transport).  May sleep. */
int ata_dev_wake(struct ata_device *dev);
/* Seconds -> the STANDBY timer count value (0 off, 1..240 x5 s, 241..251
 * x30 min, 252 21 min, 253 vendor, 255 21 min 15 s).  Pure. */
uint8_t ata_standby_timer_encode(uint32_t seconds);
/* Device power transitions for the block layer's ops: suspend = flush +
 * STANDBY IMMEDIATE; resume = wake + revalidate; shutdown = flush (+
 * STANDBY IMMEDIATE on poweroff).  May sleep. */
int ata_dev_suspend(struct ata_device *dev);
int ata_dev_resume(struct ata_device *dev);
int ata_dev_shutdown(struct ata_device *dev, enum blk_shutdown_mode mode);

/* ==========================================================================
 * ata_smart.c (A5)
 * ========================================================================== */

int ata_smart_enable(struct ata_device *dev, int on);
int ata_smart_read_data(struct ata_device *dev, void *buf512);
int ata_smart_read_thresholds(struct ata_device *dev, void *buf512);
/* RETURN STATUS: *failing = 1 when a threshold is exceeded. */
int ata_smart_return_status(struct ata_device *dev, int *failing);
/* SMART READ LOG (sector count `count' of log `log'). */
int ata_smart_read_log(struct ata_device *dev, uint8_t log, void *buf,
		       uint8_t count);
int ata_smart_execute_offline(struct ata_device *dev, uint8_t test);
int ata_smart_autosave(struct ata_device *dev, int on);
int ata_smart_auto_offline(struct ata_device *dev, int on);
/* Checksum of a SMART data/threshold/log page (sum of 512 bytes == 0).
 * Pure. */
int ata_smart_checksum_ok(const void *buf512);
/* Kernel log summary at probe: health, temperature, power-on hours,
 * reallocated/pending sectors when the attributes are present. */
void ata_smart_report(struct ata_device *dev);
/* Print every log the device offers (SMART summary/comprehensive error
 * logs, self-test log, GPL directory, extended comprehensive error log,
 * extended self-test log, device statistics) to the kernel log.  May
 * sleep. */
void ata_smart_dump_logs(struct ata_device *dev);

/* NCQ command error log (10h) page 0, decoded.  Shared with the AHCI
 * error handler (ahci_eh.c reads the page with READ LOG EXT on the
 * internal slot and calls ata_ncq_err_decode()). */
struct ata_ncq_err_info {
	int nq; /* 1: a NON-queued command failed (tag not valid) */
	int tag; /* the failed NCQ tag, -1 when nq */
	uint8_t status, error, device;
	uint8_t checksum_ok; /* the page's byte sum was zero */
	uint64_t lba; /* 48-bit LBA of the failed command */
	uint16_t count;
	uint8_t sense_key, asc, ascq; /* NCQ autosense (0 when none) */
	uint8_t sense_valid; /* key/asc/ascq non-zero */
};
/* Decode page 0 of log 10h.  0 when a failed command is recorded, -ENOENT
 * when the page records nothing (all zero), -EINVAL for NULL arguments.
 * A bad checksum is reported in checksum_ok, not as a failure (several
 * devices never fill it in).  Pure. */
int ata_ncq_err_decode(const void *buf512, struct ata_ncq_err_info *out);

/* ==========================================================================
 * ata_trim.c (A5)
 * ========================================================================== */

/* Encode as many ranges as fit into `buf' (`blocks' x 512 bytes) for
 * [lba, lba+count): entries of at most 65535 sectors, zero-filled tail.
 * Returns the number of sectors covered (<= count), so the caller loops.
 * Pure (host-testable). */
uint64_t ata_trim_encode(void *buf, uint32_t blocks, uint64_t lba,
			 uint64_t count);
/* Build the DSM TRIM command (DMA, or SEND FPDMA QUEUED when queued TRIM
 * is usable) for a prepared payload.  Pure. */
int ata_trim_build_tf(const struct ata_device *dev,
		      struct ata_queued_cmd *qc, uint32_t blocks);
/* Synchronous TRIM of a range (loops over payloads).  May sleep. */
int ata_trim(struct ata_device *dev, uint64_t lba, uint64_t count);
/* 512-byte payload blocks one DSM command of this device carries (IDENTIFY
 * word 105, capped by the driver's limit); 0 without TRIM.  Pure. */
uint32_t ata_trim_payload_blocks(const struct ata_device *dev);
/* Queued TRIM (SEND FPDMA QUEUED) usable: NCQ on, the device announces
 * it, and the model is not known to corrupt data with it.  Pure. */
int ata_trim_queued_ok(const struct ata_device *dev);
/* Trimmed sectors deterministically read back as zeroes (DRAT + RZAT, the
 * model not known to lie about it): discard_zeroes_data.  Pure. */
int ata_trim_zeroes_data(const struct ata_device *dev);

/* ==========================================================================
 * ata_security.c (A5)
 * ========================================================================== */

/* Decode word 128 (+ 89/90/92) into dev->security.  Pure. */
void ata_security_parse(struct ata_device *dev, const uint16_t *id);
int ata_security_set_password(struct ata_device *dev, int master,
			      const uint8_t pw[ATA_SEC_PW_LEN], int level_max,
			      uint16_t master_id);
int ata_security_unlock(struct ata_device *dev, int master,
			const uint8_t pw[ATA_SEC_PW_LEN]);
/* ERASE PREPARE + ERASE UNIT (timeout from words 89/90). */
int ata_security_erase(struct ata_device *dev, int master,
		       const uint8_t pw[ATA_SEC_PW_LEN], int enhanced);
int ata_security_freeze(struct ata_device *dev);
int ata_security_disable(struct ata_device *dev, int master,
			 const uint8_t pw[ATA_SEC_PW_LEN]);
/* "not supported" / "not enabled, not locked, frozen" ... into buf. */
int ata_security_state_str(const struct ata_device *dev, char *buf, int cap);

/* ==========================================================================
 * atapi.c (A5)
 * ========================================================================== */

/* Configure an ATAPI device after IDENTIFY PACKET (CDB length, DMA
 * capability, AN) and register its srN disk (2048-byte sectors, read-only,
 * removable, no partitions; media polling every 2 s unless AN works).
 * eh_mutex held.  May sleep. */
int atapi_dev_register(struct ata_device *dev);
void atapi_dev_unregister(struct ata_device *dev);
/* Run one packet command: `cdb' (cdb_len bytes, padded to the device's
 * packet size), data buffer and direction, sense collected (REQUEST SENSE
 * on CHECK CONDITION) into sense/sense_len.  *scsi_status receives the
 * SAM status.  0, -EIO with sense, -ENOMEDIUM, -ETIMEDOUT.  May sleep. */
int atapi_exec_cdb(struct ata_device *dev, const uint8_t *cdb, int cdb_len,
		   void *buf, uint32_t len, enum ata_dma_dir dir,
		   uint8_t *sense, uint32_t *sense_len, uint8_t *scsi_status,
		   uint32_t timeout_ms);
/* atapi_exec_cdb() that also reports the residual byte count (SG_IO). */
int atapi_exec_cdb_resid(struct ata_device *dev, const uint8_t *cdb,
			 int cdb_len, void *buf, uint32_t len,
			 enum ata_dma_dir dir, uint8_t *sense,
			 uint32_t *sense_len, uint8_t *scsi_status,
			 uint32_t timeout_ms, uint32_t *resid);
int atapi_test_unit_ready(struct ata_device *dev);
int atapi_request_sense(struct ata_device *dev, uint8_t *sense,
			uint32_t *len);
int atapi_read_capacity(struct ata_device *dev, uint64_t *blocks,
			uint32_t *block_size);
int atapi_eject(struct ata_device *dev, int load);
int atapi_lock_door(struct ata_device *dev, int lock);
/* Look at the medium now (an open, a resume, a media-changed query): GET
 * EVENT STATUS NOTIFICATION and TEST UNIT READY, media_present updated,
 * a change acted on (capacity, disk_media_changed()).  1 changed, 0 not,
 * -errno.  May sleep. */
int atapi_check_media(struct ata_device *dev);
/* A media event of the drive (its asynchronous notification): its events
 * work runs now.  Any context, the interrupt handler included. */
void atapi_media_event(struct ata_device *dev);
/* Start the events work of the drives registered at boot (once the
 * storage stack's threads run). */
void atapi_events_start(void);
/* Enable SATA asynchronous notification on ATAPI devices (0: off, the
 * default -- their media events are polled).  ata_features.c. */
extern int ata_atapi_an;
/* The blk_ops of an optical drive. */
extern const struct blk_ops atapi_blk_ops;

/* ==========================================================================
 * ata_sat.c (A5) -- SCSI/ATA translation and the device ioctls
 * ========================================================================== */

/* SAM status + sense of a translated command */
struct ata_sat_result {
	uint8_t status; /* SAM_STAT_* */
	uint8_t sense[ATA_SENSE_LEN];
	uint32_t sense_len;
	uint32_t resid;
	uint16_t host_status; /* SG_ERR_DID_* */
	uint16_t driver_status; /* SG_ERR_DRIVER_* */
};

/* Execute one SCSI command against an ATA disk on kernel buffers:
 * INQUIRY (standard + VPD 00h, 80h, 83h, 89h, B0h, B1h, B2h), READ
 * CAPACITY (10/16), TEST UNIT READY, REQUEST SENSE, READ/WRITE (6/10/12/
 * 16), VERIFY, SYNCHRONIZE CACHE (10/16), MODE SENSE (6/10: caching,
 * control, R/W error recovery pages), MODE SELECT (caching WCE), START
 * STOP UNIT, UNMAP, WRITE SAME (16, UNMAP bit), REPORT LUNS, REPORT
 * SUPPORTED OPERATION CODES, ATA PASS-THROUGH (12/16) with all protocols
 * and CK_COND/T_DIR/BYT_BLOK/T_LENGTH, LOG SENSE (temperature, self-test).
 * Anything else: CHECK CONDITION, ILLEGAL REQUEST / INVALID COMMAND
 * OPERATION CODE.  Returns 0 when a SCSI status was produced (look at
 * res->status), -errno for a transport failure.  May sleep. */
int ata_sat_execute(struct ata_device *dev, const uint8_t *cdb, int cdb_len,
		    void *buf, uint32_t len, enum ata_dma_dir dir,
		    uint32_t timeout_ms, struct ata_sat_result *res);
/* Fixed (70h) or descriptor (72h) sense with an ATA status return
 * descriptor for PASS-THROUGH CK_COND.  Pure. */
uint32_t ata_sat_build_sense(uint8_t *sense, uint32_t cap, int descriptor,
			     uint8_t key, uint8_t asc, uint8_t ascq,
			     const struct ata_taskfile *tf);
/* The pure response builders the host tests call directly. */
int ata_sat_inquiry_std(const struct ata_device *dev, uint8_t *buf,
			uint32_t len);
int ata_sat_inquiry_vpd(const struct ata_device *dev, uint8_t page,
			uint8_t *buf, uint32_t len);
int ata_sat_read_capacity16(const struct ata_device *dev, uint8_t *buf,
			    uint32_t len);
/* Map a sense/ATA error to the SCSI view (key/asc/ascq).  Pure. */
void ata_sat_error_to_sense(uint8_t status, uint8_t error, uint8_t *key,
			    uint8_t *asc, uint8_t *ascq);
int ata_sat_read_capacity10(const struct ata_device *dev, uint8_t *buf,
			    uint32_t len);

/* A decoded ATA PASS-THROUGH (12)/(16) CDB. */
struct ata_sat_pt {
	int cdb_len; /* 12 or 16 */
	uint8_t protocol; /* SAT_PROT_* */
	uint8_t multiple_count; /* log2 of sectors per DRQ block */
	uint8_t off_line; /* 0..3: wait 2^(n+1)-2 s before status */
	uint8_t extend, ck_cond, t_type, t_dir, byt_blok, t_length;
	struct ata_taskfile tf; /* the ATA command, flags filled */
	enum ata_prot prot; /* ATA protocol (UNKNOWN for resets) */
	enum ata_dma_dir dir;
	uint32_t xfer_len; /* bytes the command moves */
	uint32_t block_size; /* 512 or the logical size (T_TYPE) */
};
/* Decode an ATA PASS-THROUGH CDB against a data buffer of `buf_len'
 * bytes; `lbs' is the logical block size (T_TYPE 1).  0, or -EINVAL with
 * *bad_byte and *bad_bit naming the invalid CDB field (sense key specific
 * data), or -EOPNOTSUPP for a protocol the translator refuses.  Pure. */
int ata_sat_pt_decode(const uint8_t *cdb, int cdb_len, uint32_t buf_len,
		      uint32_t lbs, struct ata_sat_pt *pt, int *bad_byte,
		      int *bad_bit);
/* The ATA status return descriptor (code 09h, 14 bytes) of a result
 * taskfile.  Pure. */
void ata_sat_ata_status_desc(const struct ata_taskfile *tf, int extend,
			     uint8_t desc[14]);

/* SG_IO on an ATA disk or ATAPI drive: copy the sg_io_hdr and buffers in,
 * check permissions (CAP_SYS_RAWIO, or the read-only command list on a
 * node opened for reading), execute (ATAPI: the CDB as a PACKET command;
 * ATA: ata_sat_execute), copy status/sense/resid/duration out.  May
 * sleep. */
long ata_sat_sg_io(struct ata_device *dev, void *user_hdr, struct task *cur,
		   int open_flags);
/* HDIO_* on an ATA disk (HDIO_GET_IDENTITY, DRIVE_CMD, DRIVE_TASK,
 * DRIVE_TASKFILE, GET/SET_WCACHE, GET/SET_ACOUSTIC, GET/SET_MULTCOUNT,
 * GET_32BIT, GET_DMA, DRIVE_RESET ...).  -ENOTTY for unknown requests.
 * May sleep. */
long ata_hdio_ioctl(struct ata_device *dev, unsigned long cmd, void *argp,
		    struct task *cur);
/* blk_ops.ioctl of ATA disks and ATAPI drives: SG_IO, HDIO_*,
 * SCSI_IOCTL_TEST_UNIT_READY/START/STOP/DOORLOCK/DOORUNLOCK, CDROMEJECT-
 * style eject via START STOP UNIT; -ENOTTY for the generic BLK* code.
 * May sleep. */
long ata_sat_blk_ioctl(block_device_t *bdev, unsigned long cmd, void *argp,
		       struct task *cur);
long ata_sat_blk_ioctl_mode(block_device_t *bdev, unsigned long cmd,
			    void *argp, struct task *cur, int open_flags);

/* The HW_OWNED sequence of `dev' (see ata_device.hw_owned_seq): snapshot
 * it before a command whose qc the caller never sees, and ask
 * ata_dev_hw_owned_since() before freeing an internal buffer of that
 * command -- non-zero means keep (leak) the buffer, the controller may
 * still write into it.  Another command of the same device failing that
 * way in between only makes the caller keep a buffer it could have freed. */
static inline uint32_t ata_dev_hw_owned_snap(const struct ata_device *dev)
{
	return __atomic_load_n(&dev->hw_owned_seq, __ATOMIC_ACQUIRE);
}

static inline int ata_dev_hw_owned_since(const struct ata_device *dev,
					 uint32_t snap)
{
	return __atomic_load_n(&dev->hw_owned_seq, __ATOMIC_ACQUIRE) != snap;
}

/* SAT constants the translator uses ------------------------------------- */
/* ATA PASS-THROUGH protocol field (CDB byte 1 bits 4:1) */
#define SAT_PROT_HARD_RESET 0
#define SAT_PROT_SRST 1
#define SAT_PROT_NONDATA 3
#define SAT_PROT_PIO_IN 4
#define SAT_PROT_PIO_OUT 5
#define SAT_PROT_DMA 6
#define SAT_PROT_DMA_QUEUED 7
#define SAT_PROT_DEV_DIAG 8
#define SAT_PROT_DEV_RESET 9
#define SAT_PROT_UDMA_IN 10
#define SAT_PROT_UDMA_OUT 11
#define SAT_PROT_FPDMA 12
#define SAT_PROT_RET_RESP 15 /* return response information */
/* CDB byte 2 */
#define SAT_CK_COND 0x20 /* always return the ATA registers */
#define SAT_T_DIR 0x08 /* 1: from device */
#define SAT_BYT_BLOK 0x04 /* 1: length in blocks */
#define SAT_T_LEN_MASK 0x03 /* 0 none, 1 feature, 2 count, 3 TPSIU */
/* Additional sense codes the translator produces */
#define SAT_ASC_NO_ADDITIONAL 0x00
#define SAT_ASCQ_ATA_PT_INFO 0x1D /* with ASC 00h: ATA PT info available */
#define SAT_ASC_WRITE_ERROR 0x0C
#define SAT_ASC_UNRECOVERED_READ 0x11
#define SAT_ASC_ID_NOT_FOUND 0x14 /* ASCQ 01h record not found */
#define SAT_ASC_INVALID_OPCODE 0x20
#define SAT_ASC_LBA_OUT_OF_RANGE 0x21
#define SAT_ASC_INVALID_FIELD_CDB 0x24
#define SAT_ASC_INVALID_FIELD_PARAM 0x26
#define SAT_ASC_WRITE_PROTECTED 0x27
#define SAT_ASC_MEDIUM_CHANGED 0x28
#define SAT_ASC_POWER_ON_RESET 0x29
#define SAT_ASC_MEDIUM_NOT_PRESENT 0x3A
#define SAT_ASC_INTERNAL_TARGET_FAILURE 0x44
#define SAT_ASC_IU_CRC_ERROR 0x47 /* ASCQ 03h information unit CRC */
#define SAT_ASC_LOGICAL_UNIT_NOT_READY 0x04
#define SAT_ASC_LOGICAL_UNIT_FAILURE 0x3E
/* VPD pages */
#define SAT_VPD_SUPPORTED 0x00
#define SAT_VPD_SERIAL 0x80
#define SAT_VPD_DEVICE_ID 0x83
#define SAT_VPD_ATA_INFO 0x89
#define SAT_VPD_BLOCK_LIMITS 0xB0
#define SAT_VPD_BLOCK_CHARS 0xB1
#define SAT_VPD_LBP 0xB2 /* logical block provisioning */

/* ==========================================================================
 * Compile-time layout checks
 * ========================================================================== */

static inline void ata_layout_checks(void)
{
	BUILD_BUG_ON(sizeof(struct fis_reg_h2d) != FIS_H2D_LEN);
	BUILD_BUG_ON(sizeof(struct fis_reg_d2h) != FIS_D2H_LEN);
	BUILD_BUG_ON(sizeof(struct fis_dma_activate) != FIS_DMA_ACTIVATE_LEN);
	BUILD_BUG_ON(sizeof(struct fis_dma_setup) != FIS_DMA_SETUP_LEN);
	BUILD_BUG_ON(sizeof(struct fis_bist) != FIS_BIST_LEN);
	BUILD_BUG_ON(sizeof(struct fis_pio_setup) != FIS_PIO_SETUP_LEN);
	BUILD_BUG_ON(sizeof(struct fis_set_device_bits) != FIS_SDB_LEN);
	BUILD_BUG_ON(sizeof(struct fis_data) != FIS_DATA_HDR_LEN);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_reg_h2d, count_lo) != 12);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_reg_h2d, control) != 15);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_reg_d2h, count_lo) != 12);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_dma_setup, dma_buffer_id) !=
		     4);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_dma_setup,
					transfer_count) != 20);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_pio_setup,
					transfer_count) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct fis_set_device_bits,
					protocol) != 4);
	BUILD_BUG_ON(ATA_ID_BYTES != 512);
	BUILD_BUG_ON(ATA_DSM_RANGE(1, 2) != ((2ULL << 48) | 1));
	BUILD_BUG_ON(ATA_TRIM_RANGES_PER_BLOCK * 8 != ATA_SECT_SIZE);
	BUILD_BUG_ON(ATA_MAX_QUEUE > 32);
}

#endif // KERNEL_DEV_ATA_ATA_H
