// LikeOS ATA layer - SMART (Self-Monitoring, Analysis and Reporting
// Technology), the device's error logs and its self-tests
//
// Every SMART command is opcode B0h with the subcommand in the FEATURE
// register and the signature 4Fh/C2h in LBA mid/high (a drive aborts a
// SMART command without it):
//
//   D0h READ DATA          512 bytes: 30 attributes of 12 bytes, the
//                          off-line/self-test status, capabilities and
//                          the recommended self-test durations
//   D1h READ THRESHOLDS    512 bytes: the threshold of each attribute
//   D2h AUTOSAVE           COUNT F1h enable / 00h disable
//   D4h EXECUTE OFF-LINE IMMEDIATE   LBA low: 00h off-line collection,
//                          01h short, 02h extended, 03h conveyance, 04h
//                          selective self-test (81h-84h: the same in
//                          captive mode -- the command completes when the
//                          test does), 7Fh abort the running test
//   D5h READ LOG           LBA low: log address, COUNT: sectors
//   D8h ENABLE OPERATIONS / D9h DISABLE OPERATIONS
//   DAh RETURN STATUS      no data; LBA mid/high come back 4Fh/C2h when
//                          all is well and F4h/2Ch when an attribute has
//                          crossed its threshold ("drive failure
//                          predicted")
//   DBh AUTO OFF-LINE      COUNT F8h enable / 00h disable
//
// The logs a drive keeps about itself, read through SMART READ LOG (the
// 28-bit "SMART logs") or READ LOG EXT / READ LOG DMA EXT (the 48-bit
// "general purpose logs", GPL):
//
//   00h  log directory (both kinds: pages per log address)
//   01h  SMART summary error log    (the last five errors, 28-bit)
//   02h  SMART comprehensive error log
//   03h  extended comprehensive error log (GPL, 48-bit, 4 per page)
//   04h  device statistics (GPL: power-on hours, sectors read/written,
//        temperatures, CRC errors, SSD endurance ...)
//   06h  SMART self-test log (21 entries)
//   07h  extended self-test log (GPL, 19 entries per page)
//   10h  NCQ command error log -- read by the error handler after an NCQ
//        failure; ata_ncq_err_decode() here is the decoder it shares.
//
// What this file provides: the command wrappers declared in ata.h, the
// decoders for every page above, the attribute table with the
// conventional attribute names and raw-value interpretations, the probe
// summary ata_smart_report() ("ata1.00: SMART: health PASSED, 34 C, 1234
// power-on hours, 0 reallocated, 0 pending") and ata_smart_dump_logs()
// for the full picture in the kernel log.  The decoders are pure.
//
// SMART commands are ordinary non-queued commands: they go through
// ata_exec_internal() and wait (or poll at boot).  Nothing here takes a
// lock of the storage stack; the attribute flags of the device (SMART_ON)
// are updated with atomic operations.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

#define SMART_PFX "[ata] "

/* ==========================================================================
 * Layouts (offsets into the 512-byte pages)
 * ========================================================================== */

/* SMART READ DATA attribute entry (12 bytes) */
#define SMART_ATTR_ID 0
#define SMART_ATTR_FLAGS 1 /* word */
#define SMART_ATTR_VALUE 3 /* normalised, 1..253 */
#define SMART_ATTR_WORST 4
#define SMART_ATTR_RAW 5 /* 6 bytes, vendor format */
/* Attribute flag bits */
#define SMART_AF_PREFAIL 0x0001 /* pre-failure (else old-age) */
#define SMART_AF_ONLINE 0x0002 /* updated during normal operation */
#define SMART_AF_PERFORMANCE 0x0004
#define SMART_AF_ERROR_RATE 0x0008
#define SMART_AF_EVENT_COUNT 0x0010
#define SMART_AF_SELF_PRESERVING 0x0020
/* SMART READ THRESHOLDS entry (12 bytes) */
#define SMART_THR_ID 0
#define SMART_THR_VALUE 1

/* Off-line data collection capability (byte 367) */
#define SMART_OFFCAP_EXEC_OFFLINE 0x01
#define SMART_OFFCAP_AUTO_OFFLINE 0x02
#define SMART_OFFCAP_ABORT_RESTART 0x04
#define SMART_OFFCAP_READ_SCAN 0x08
#define SMART_OFFCAP_SELF_TEST 0x10
#define SMART_OFFCAP_CONVEYANCE 0x20
#define SMART_OFFCAP_SELECTIVE 0x40
/* SMART capability word (368) */
#define SMART_CAP_SAVES_BEFORE_PM 0x0001
#define SMART_CAP_AUTOSAVE 0x0002

/* SMART summary / comprehensive error log (01h/02h), 28-bit */
#define SMART_ERRLOG_VERSION 0
#define SMART_ERRLOG_INDEX 1 /* 1..5, 0 = no entries */
#define SMART_ERRLOG_DATA 2 /* 5 x 90 bytes */
#define SMART_ERRLOG_ENTRY_SIZE 90
#define SMART_ERRLOG_ENTRIES 5
#define SMART_ERRLOG_COUNT 452 /* word: device error count */
#define SMART_ERRLOG_CMD_SIZE 12 /* command data structure */
#define SMART_ERRLOG_CMDS 5
#define SMART_ERRLOG_ERR_OFS 60 /* error data structure (30 bytes) */

/* Extended comprehensive error log (03h), 48-bit */
#define SMART_XERRLOG_VERSION 0
#define SMART_XERRLOG_INDEX 2 /* word */
#define SMART_XERRLOG_DATA 4 /* 4 x 124 bytes */
#define SMART_XERRLOG_ENTRY_SIZE 124
#define SMART_XERRLOG_ENTRIES 4 /* per page */
#define SMART_XERRLOG_COUNT 500 /* word: device error count */
#define SMART_XERRLOG_CMD_SIZE 18
#define SMART_XERRLOG_CMDS 5
#define SMART_XERRLOG_ERR_OFS 90 /* error data structure (34 bytes) */

/* SMART self-test log (06h) */
#define SMART_STLOG_REVISION 0 /* word */
#define SMART_STLOG_DATA 2 /* 21 x 24 bytes */
#define SMART_STLOG_ENTRY_SIZE 24
#define SMART_STLOG_ENTRIES 21
#define SMART_STLOG_INDEX 508 /* 1..21, 0 = none */
/* Extended self-test log (07h) */
#define SMART_XSTLOG_INDEX 2 /* word */
#define SMART_XSTLOG_DATA 4 /* 19 x 26 bytes */
#define SMART_XSTLOG_ENTRY_SIZE 26
#define SMART_XSTLOG_ENTRIES 19

/* Device statistics (04h) */
#define DEVSTAT_PAGE_LIST 0x00
#define DEVSTAT_GENERAL 0x01
#define DEVSTAT_FREE_FALL 0x02
#define DEVSTAT_ROTATING 0x03
#define DEVSTAT_GENERAL_ERRORS 0x04
#define DEVSTAT_TEMPERATURE 0x05
#define DEVSTAT_TRANSPORT 0x06
#define DEVSTAT_SSD 0x07
#define DEVSTAT_MAX_PAGE 0x07
/* Flags in bits 63:56 of a statistic */
#define DEVSTAT_SUPPORTED (1ULL << 63)
#define DEVSTAT_VALID (1ULL << 62)
#define DEVSTAT_NORMALIZED (1ULL << 61)
#define DEVSTAT_MONITORED (1ULL << 60) /* monitored condition met */

/* How many entries the logs printers show at most */
#define SMART_PRINT_ERRORS 5
#define SMART_PRINT_SELFTESTS 5

/* ==========================================================================
 * Little-endian accessors (pure; pages are byte arrays)
 * ========================================================================== */

static inline uint16_t sm_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t sm_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t sm_le48(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 5; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static inline uint64_t sm_le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/* ==========================================================================
 * The checksum (pure)
 * ========================================================================== */

int ata_smart_checksum_ok(const void *buf512)
{
	const uint8_t *p = (const uint8_t *)buf512;
	uint8_t sum = 0;

	if (!p)
		return 0;
	for (int i = 0; i < ATA_SECT_SIZE; i++)
		sum = (uint8_t)(sum + p[i]);
	return sum == 0;
}

/* ==========================================================================
 * Attribute names and raw interpretation
 * ========================================================================== */

enum smart_raw_kind {
	SMART_RAW_48 = 0, /* the 48-bit raw value as a number */
	SMART_RAW_HOURS, /* low 32 bits: hours */
	SMART_RAW_TEMP, /* byte 0 Celsius, bytes 2/4 min/max when sane */
	SMART_RAW_16, /* low 16 bits (vendors pack other data above) */
	SMART_RAW_24, /* low 24 bits */
};

struct smart_attr_desc {
	uint8_t id;
	uint8_t kind; /* enum smart_raw_kind */
	const char *name;
};

/* The conventional names of the attributes the industry agrees on.  The
 * numbering is not standardised; these are the meanings the vast majority
 * of drives use, which is what every SMART tool shows as well. */
static const struct smart_attr_desc smart_attr_table[] = {
	{ 1, SMART_RAW_48, "Raw_Read_Error_Rate" },
	{ 2, SMART_RAW_48, "Throughput_Performance" },
	{ 3, SMART_RAW_16, "Spin_Up_Time" },
	{ 4, SMART_RAW_48, "Start_Stop_Count" },
	{ 5, SMART_RAW_48, "Reallocated_Sector_Ct" },
	{ 6, SMART_RAW_48, "Read_Channel_Margin" },
	{ 7, SMART_RAW_48, "Seek_Error_Rate" },
	{ 8, SMART_RAW_48, "Seek_Time_Performance" },
	{ 9, SMART_RAW_HOURS, "Power_On_Hours" },
	{ 10, SMART_RAW_48, "Spin_Retry_Count" },
	{ 11, SMART_RAW_48, "Calibration_Retry_Count" },
	{ 12, SMART_RAW_48, "Power_Cycle_Count" },
	{ 13, SMART_RAW_48, "Read_Soft_Error_Rate" },
	{ 22, SMART_RAW_48, "Helium_Level" },
	{ 170, SMART_RAW_48, "Available_Reservd_Space" },
	{ 171, SMART_RAW_48, "Program_Fail_Count" },
	{ 172, SMART_RAW_48, "Erase_Fail_Count" },
	{ 173, SMART_RAW_48, "Wear_Leveling_Count" },
	{ 174, SMART_RAW_48, "Unexpect_Power_Loss_Ct" },
	{ 175, SMART_RAW_48, "Program_Fail_Count_Chip" },
	{ 176, SMART_RAW_48, "Erase_Fail_Count_Chip" },
	{ 177, SMART_RAW_48, "Wear_Leveling_Count" },
	{ 178, SMART_RAW_48, "Used_Rsvd_Blk_Cnt_Chip" },
	{ 179, SMART_RAW_48, "Used_Rsvd_Blk_Cnt_Tot" },
	{ 180, SMART_RAW_48, "Unused_Rsvd_Blk_Cnt_Tot" },
	{ 181, SMART_RAW_48, "Program_Fail_Cnt_Total" },
	{ 182, SMART_RAW_48, "Erase_Fail_Count_Total" },
	{ 183, SMART_RAW_48, "Runtime_Bad_Block" },
	{ 184, SMART_RAW_48, "End-to-End_Error" },
	{ 187, SMART_RAW_48, "Reported_Uncorrect" },
	{ 188, SMART_RAW_48, "Command_Timeout" },
	{ 189, SMART_RAW_48, "High_Fly_Writes" },
	{ 190, SMART_RAW_TEMP, "Airflow_Temperature_Cel" },
	{ 191, SMART_RAW_48, "G-Sense_Error_Rate" },
	{ 192, SMART_RAW_48, "Power-Off_Retract_Count" },
	{ 193, SMART_RAW_48, "Load_Cycle_Count" },
	{ 194, SMART_RAW_TEMP, "Temperature_Celsius" },
	{ 195, SMART_RAW_48, "Hardware_ECC_Recovered" },
	{ 196, SMART_RAW_48, "Reallocated_Event_Count" },
	{ 197, SMART_RAW_48, "Current_Pending_Sector" },
	{ 198, SMART_RAW_48, "Offline_Uncorrectable" },
	{ 199, SMART_RAW_48, "UDMA_CRC_Error_Count" },
	{ 200, SMART_RAW_48, "Multi_Zone_Error_Rate" },
	{ 201, SMART_RAW_48, "Soft_Read_Error_Rate" },
	{ 202, SMART_RAW_48, "Data_Address_Mark_Errs" },
	{ 203, SMART_RAW_48, "Run_Out_Cancel" },
	{ 204, SMART_RAW_48, "Soft_ECC_Correction" },
	{ 205, SMART_RAW_48, "Thermal_Asperity_Rate" },
	{ 206, SMART_RAW_48, "Flying_Height" },
	{ 207, SMART_RAW_48, "Spin_High_Current" },
	{ 208, SMART_RAW_48, "Spin_Buzz" },
	{ 209, SMART_RAW_48, "Offline_Seek_Performnce" },
	{ 220, SMART_RAW_48, "Disk_Shift" },
	{ 221, SMART_RAW_48, "G-Sense_Error_Rate" },
	{ 222, SMART_RAW_48, "Loaded_Hours" },
	{ 223, SMART_RAW_48, "Load_Retry_Count" },
	{ 224, SMART_RAW_48, "Load_Friction" },
	{ 225, SMART_RAW_48, "Load_Cycle_Count" },
	{ 226, SMART_RAW_48, "Load-in_Time" },
	{ 227, SMART_RAW_48, "Torq-amp_Count" },
	{ 228, SMART_RAW_48, "Power-off_Retract_Count" },
	{ 230, SMART_RAW_48, "Head_Amplitude" },
	{ 231, SMART_RAW_48, "SSD_Life_Left" },
	{ 232, SMART_RAW_48, "Available_Reservd_Space" },
	{ 233, SMART_RAW_48, "Media_Wearout_Indicator" },
	{ 234, SMART_RAW_48, "Thermal_Throttle" },
	{ 235, SMART_RAW_48, "Good_Block_Count" },
	{ 240, SMART_RAW_HOURS, "Head_Flying_Hours" },
	{ 241, SMART_RAW_48, "Total_LBAs_Written" },
	{ 242, SMART_RAW_48, "Total_LBAs_Read" },
	{ 249, SMART_RAW_48, "NAND_Writes_1GiB" },
	{ 250, SMART_RAW_48, "Read_Error_Retry_Rate" },
	{ 254, SMART_RAW_48, "Free_Fall_Sensor" },
};

static const struct smart_attr_desc *smart_attr_lookup(uint8_t id)
{
	for (size_t i = 0;
	     i < sizeof(smart_attr_table) / sizeof(smart_attr_table[0]); i++)
		if (smart_attr_table[i].id == id)
			return &smart_attr_table[i];
	return NULL;
}

/* One attribute, decoded from READ DATA + READ THRESHOLDS. */
struct smart_attr {
	uint8_t id;
	uint16_t flags;
	uint8_t value, worst, threshold;
	uint8_t raw_bytes[6];
	uint64_t raw;
	uint8_t failing_now; /* value <= threshold (threshold != 0) */
	uint8_t failed_past; /* worst <= threshold */
	uint8_t has_threshold;
};

/* Decode the attribute table.  `thr' may be NULL (no thresholds).
 * Returns the number of attributes (entries with a non-zero id).  Pure. */
static int smart_decode_attrs(const uint8_t *data, const uint8_t *thr,
			      struct smart_attr *out, int max)
{
	int n = 0;

	for (int i = 0; i < ATA_SMART_NUM_ATTRS && n < max; i++) {
		const uint8_t *a = data + ATA_SMART_DATA_ATTRS +
				   i * ATA_SMART_ATTR_SIZE;
		struct smart_attr *o = &out[n];

		if (a[SMART_ATTR_ID] == 0)
			continue;
		kmemset(o, 0, sizeof(*o));
		o->id = a[SMART_ATTR_ID];
		o->flags = sm_le16(a + SMART_ATTR_FLAGS);
		o->value = a[SMART_ATTR_VALUE];
		o->worst = a[SMART_ATTR_WORST];
		kmemcpy(o->raw_bytes, a + SMART_ATTR_RAW, 6);
		o->raw = sm_le48(a + SMART_ATTR_RAW);

		/* The threshold page lists the same ids, normally in the same
		 * slot -- but match by id, not by position. */
		if (thr) {
			const uint8_t *t = thr + ATA_SMART_DATA_ATTRS +
					   i * ATA_SMART_ATTR_SIZE;

			if (t[SMART_THR_ID] != o->id) {
				t = NULL;
				for (int j = 0; j < ATA_SMART_NUM_ATTRS; j++) {
					const uint8_t *c =
						thr + ATA_SMART_DATA_ATTRS +
						j * ATA_SMART_ATTR_SIZE;
					if (c[SMART_THR_ID] == o->id) {
						t = c;
						break;
					}
				}
			}
			if (t) {
				o->threshold = t[SMART_THR_VALUE];
				o->has_threshold = 1;
			}
		}
		/* Threshold 0 means "never fails"; FEh/FFh are "always
		 * passing" markers some vendors use; normalised values of
		 * 0, FEh and FFh are not real values. */
		if (o->has_threshold && o->threshold != 0 &&
		    o->threshold < 0xFE && o->value >= 1 && o->value <= 0xFD) {
			o->failing_now = o->value <= o->threshold;
			o->failed_past = o->worst <= o->threshold;
		}
		n++;
	}
	return n;
}

/* Interpret the raw value the conventional way.  `minmax' receives the
 * min/max temperatures when the attribute carries them (else zeroes). */
static uint64_t smart_attr_raw_value(const struct smart_attr *a, int *tmin,
				     int *tmax)
{
	const struct smart_attr_desc *d = smart_attr_lookup(a->id);
	int kind = d ? d->kind : SMART_RAW_48;

	if (tmin)
		*tmin = 0;
	if (tmax)
		*tmax = 0;
	switch (kind) {
	case SMART_RAW_HOURS:
		return a->raw & 0xFFFFFFFFull;
	case SMART_RAW_16:
		return a->raw & 0xFFFFull;
	case SMART_RAW_24:
		return a->raw & 0xFFFFFFull;
	case SMART_RAW_TEMP: {
		int cur = a->raw_bytes[0];
		int lo = a->raw_bytes[2], hi = a->raw_bytes[4];

		/* Bytes 2 and 4 are min/max on most drives; accept them only
		 * when they bracket the current value plausibly. */
		if (lo && hi && lo <= cur && cur <= hi && hi < 128) {
			if (tmin)
				*tmin = lo;
			if (tmax)
				*tmax = hi;
		}
		return (uint64_t)cur;
	}
	default:
		return a->raw;
	}
}

static void smart_flags_str(uint16_t f, char *buf, size_t cap)
{
	/* The established compact form: P prefailure, O online, S speed/
	 * performance, R error rate, C event count, K self-preserving. */
	ksnprintf(buf, cap, "%c%c%c%c%c%c",
		  (f & SMART_AF_PREFAIL) ? 'P' : '-',
		  (f & SMART_AF_ONLINE) ? 'O' : '-',
		  (f & SMART_AF_PERFORMANCE) ? 'S' : '-',
		  (f & SMART_AF_ERROR_RATE) ? 'R' : '-',
		  (f & SMART_AF_EVENT_COUNT) ? 'C' : '-',
		  (f & SMART_AF_SELF_PRESERVING) ? 'K' : '-');
}

/* ==========================================================================
 * Status bytes (pure)
 * ========================================================================== */

static const char *smart_offline_status_str(uint8_t b)
{
	switch (b & 0x7F) {
	case 0x00:
		return "never started";
	case 0x02:
		return "completed without error";
	case 0x03:
		return "in progress";
	case 0x04:
		return "suspended by an interrupting command";
	case 0x05:
		return "aborted by an interrupting command";
	case 0x06:
		return "aborted by the device with a fatal error";
	default:
		return (b & 0x7F) >= 0x40 ? "vendor specific" : "reserved";
	}
}

static const char *smart_selftest_status_str(uint8_t b)
{
	switch (b >> 4) {
	case 0x0:
		return "completed without error";
	case 0x1:
		return "aborted by the host";
	case 0x2:
		return "interrupted by a reset";
	case 0x3:
		return "fatal error, unable to complete";
	case 0x4:
		return "completed, unknown element failed";
	case 0x5:
		return "completed, electrical element failed";
	case 0x6:
		return "completed, servo/seek element failed";
	case 0x7:
		return "completed, read element failed";
	case 0x8:
		return "completed, handling damage suspected";
	case 0xF:
		return "in progress";
	default:
		return "reserved status";
	}
}

static const char *smart_test_name(uint8_t test)
{
	switch (test) {
	case ATA_SMART_TEST_OFFLINE:
		return "off-line data collection";
	case ATA_SMART_TEST_SHORT:
		return "short self-test (off-line)";
	case ATA_SMART_TEST_EXTENDED:
		return "extended self-test (off-line)";
	case ATA_SMART_TEST_CONVEYANCE:
		return "conveyance self-test (off-line)";
	case ATA_SMART_TEST_SELECTIVE:
		return "selective self-test (off-line)";
	case ATA_SMART_TEST_ABORT:
		return "abort self-test";
	case ATA_SMART_TEST_SHORT_CAPTIVE:
		return "short self-test (captive)";
	case ATA_SMART_TEST_EXTENDED_CAPTIVE:
		return "extended self-test (captive)";
	case ATA_SMART_TEST_CONVEYANCE_CAPTIVE:
		return "conveyance self-test (captive)";
	case ATA_SMART_TEST_SELECTIVE_CAPTIVE:
		return "selective self-test (captive)";
	default:
		return (test >= 0x40 && test <= 0x7E) ||
				       (test >= 0x90)
			       ? "vendor specific test"
			       : "reserved test";
	}
}

/* The device state an error log entry records (state byte bits 3:0) */
static const char *smart_err_state_str(uint8_t s)
{
	switch (s & 0x0F) {
	case 0x0:
		return "unknown";
	case 0x1:
		return "sleep";
	case 0x2:
		return "standby";
	case 0x3:
		return "active or idle";
	case 0x4:
		return "executing SMART off-line or self-test";
	default:
		return (s & 0x0F) >= 0xB ? "vendor specific" : "reserved";
	}
}

/* ==========================================================================
 * Command wrappers
 * ========================================================================== */

/* SMART support: the core sets ATA_DFLAG_SMART from word 82 bit 0. */
static int smart_check(struct ata_device *dev)
{
	if (!dev)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (dev->class != ATA_DEV_ATA && dev->class != ATA_DEV_ZAC)
		return -EOPNOTSUPP;
	if (!(dev->flags & ATA_DFLAG_SMART))
		return -EOPNOTSUPP;
	return 0;
}

/* One SMART command.  `tf_out' (may be NULL) receives the result
 * registers.  Data: PIO in for `buf' with `sectors' sectors, or none. */
static int smart_cmd(struct ata_device *dev, uint8_t feature, uint8_t lbal,
		     uint8_t count, void *buf, uint32_t sectors,
		     uint32_t timeout_ms, struct ata_taskfile *tf_out,
		     uint32_t qcflags)
{
	struct ata_taskfile tf;
	int ret;

	kmemset(&tf, 0, sizeof(tf));
	tf.command = ATA_CMD_SMART;
	tf.feature = feature;
	tf.nsect = count;
	tf.lbal = lbal;
	tf.lbam = ATA_SMART_LBAM_PASS;
	tf.lbah = ATA_SMART_LBAH_PASS;
	tf.device = 0;
	tf.flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE;

	if (buf && sectors)
		ret = ata_exec_internal(dev, &tf, NULL, ATA_PROT_PIO_IN, buf,
					sectors * ATA_SECT_SIZE,
					ATA_DIR_FROM_DEVICE,
					timeout_ms ? timeout_ms
						   : ATA_TMOUT_SMART_MS,
					qcflags | ATA_QCFLAG_RESULT_TF);
	else
		ret = ata_exec_internal(dev, &tf, NULL, ATA_PROT_NODATA, NULL,
					0, ATA_DIR_NONE,
					timeout_ms ? timeout_ms
						   : ATA_TMOUT_SMART_MS,
					qcflags | ATA_QCFLAG_RESULT_TF);
	if (tf_out)
		*tf_out = tf;
	if (ret == -EIO && !(qcflags & ATA_QCFLAG_QUIET)) {
		char st[48], er[48];

		ata_status_str(tf.status, st, sizeof(st));
		ata_error_str(tf.error, er, sizeof(er));
		WARN_RATELIMIT(1,
			       SMART_PFX "%s: SMART %02Xh failed: status 0x%02x "
					 "%s error 0x%02x %s",
			       dev->name, feature, tf.status, st, tf.error, er);
	}
	return ret;
}

int ata_smart_enable(struct ata_device *dev, int on)
{
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (ata_can_sleep())
		might_sleep();
	ret = smart_cmd(dev, on ? ATA_SMART_ENABLE : ATA_SMART_DISABLE, 0, 0,
			NULL, 0, 0, NULL, 0);
	if (ret == 0) {
		if (on)
			__atomic_or_fetch(&dev->flags, ATA_DFLAG_SMART_ON,
					  __ATOMIC_SEQ_CST);
		else
			__atomic_and_fetch(&dev->flags, ~ATA_DFLAG_SMART_ON,
					   __ATOMIC_SEQ_CST);
		kprintf(SMART_PFX "%s: SMART %s\n", dev->name,
			on ? "enabled" : "disabled");
	}
	return ret;
}

int ata_smart_read_data(struct ata_device *dev, void *buf512)
{
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (!buf512)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	ret = smart_cmd(dev, ATA_SMART_READ_VALUES, 0, 1, buf512, 1, 0, NULL,
			0);
	if (ret == 0 && !ata_smart_checksum_ok(buf512))
		WARN_RATELIMIT(1,
			       SMART_PFX "%s: SMART data checksum mismatch "
					 "(byte 511 0x%02x)",
			       dev->name, ((uint8_t *)buf512)[511]);
	return ret;
}

int ata_smart_read_thresholds(struct ata_device *dev, void *buf512)
{
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (!buf512)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	ret = smart_cmd(dev, ATA_SMART_READ_THRESHOLDS, 0, 1, buf512, 1, 0,
			NULL, ATA_QCFLAG_QUIET);
	if (ret == 0 && !ata_smart_checksum_ok(buf512))
		WARN_RATELIMIT(1,
			       SMART_PFX "%s: SMART threshold checksum "
					 "mismatch",
			       dev->name);
	return ret;
}

int ata_smart_return_status(struct ata_device *dev, int *failing)
{
	struct ata_taskfile tf;
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (!failing)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	*failing = 0;
	ret = smart_cmd(dev, ATA_SMART_STATUS, 0, 0, NULL, 0, 0, &tf, 0);
	if (ret)
		return ret;

	if (tf.lbam == ATA_SMART_LBAM_PASS && tf.lbah == ATA_SMART_LBAH_PASS)
		return 0;
	if (tf.lbam == ATA_SMART_LBAM_FAIL && tf.lbah == ATA_SMART_LBAH_FAIL) {
		*failing = 1;
		kprintf(SMART_PFX "%s: SMART RETURN STATUS: threshold "
				  "exceeded -- DRIVE FAILURE PREDICTED, back up "
				  "the data\n",
			dev->name);
		return 0;
	}
	/* Neither signature: the registers did not come back (a bridge that
	 * does not return them) -- the status is unknown, not "good". */
	WARN_RATELIMIT(1,
		       SMART_PFX "%s: SMART RETURN STATUS: unrecognised "
				 "signature %02x/%02x",
		       dev->name, tf.lbam, tf.lbah);
	return -EIO;
}

int ata_smart_read_log(struct ata_device *dev, uint8_t log, void *buf,
		       uint8_t count)
{
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (!buf || count == 0)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	/* The error logs need the error logging feature set, the self-test
	 * logs the self-test feature set (word 84 bits 0/1 when valid). */
	if (ATA_ID_WORD_VALID(dev->id[ATA_ID_CFSSE])) {
		uint16_t w84 = dev->id[ATA_ID_CFSSE];

		if ((log == ATA_LOG_SUMMARY_ERR || log == ATA_LOG_COMP_ERR) &&
		    !(w84 & ATA_ID_CSE_SMART_ERRLOG))
			return -EOPNOTSUPP;
		if ((log == ATA_LOG_SELFTEST ||
		     log == ATA_LOG_SELECTIVE_SELFTEST) &&
		    !(w84 & ATA_ID_CSE_SMART_SELFTEST))
			return -EOPNOTSUPP;
	}
	return smart_cmd(dev, ATA_SMART_READ_LOG_SECTOR, log, count, buf,
			 count, 0, NULL, ATA_QCFLAG_QUIET);
}

/* The duration of a self-test in minutes from SMART READ DATA (0 when the
 * drive does not say). */
static uint32_t smart_test_minutes(const uint8_t *data, uint8_t test)
{
	switch (test & 0x7F) {
	case ATA_SMART_TEST_SHORT:
		return data[ATA_SMART_DATA_SHORT_TIME];
	case ATA_SMART_TEST_EXTENDED: {
		uint32_t m = data[ATA_SMART_DATA_EXT_TIME];

		if (m == 0xFF)
			m = sm_le16(data + ATA_SMART_DATA_EXT_TIME_WORD);
		return m;
	}
	case ATA_SMART_TEST_CONVEYANCE:
		return data[ATA_SMART_DATA_CONV_TIME];
	case ATA_SMART_TEST_SELECTIVE: {
		/* a selective test may span the whole disk: extended time */
		uint32_t m = data[ATA_SMART_DATA_EXT_TIME];

		if (m == 0xFF)
			m = sm_le16(data + ATA_SMART_DATA_EXT_TIME_WORD);
		return m;
	}
	default:
		return 0;
	}
}

int ata_smart_execute_offline(struct ata_device *dev, uint8_t test)
{
	uint8_t *data = NULL;
	uint32_t timeout_ms = ATA_TMOUT_SMART_MS;
	uint32_t minutes = 0;
	int captive;
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (ata_can_sleep())
		might_sleep();

	switch (test) {
	case ATA_SMART_TEST_OFFLINE:
	case ATA_SMART_TEST_SHORT:
	case ATA_SMART_TEST_EXTENDED:
	case ATA_SMART_TEST_CONVEYANCE:
	case ATA_SMART_TEST_SELECTIVE:
	case ATA_SMART_TEST_ABORT:
	case ATA_SMART_TEST_SHORT_CAPTIVE:
	case ATA_SMART_TEST_EXTENDED_CAPTIVE:
	case ATA_SMART_TEST_CONVEYANCE_CAPTIVE:
	case ATA_SMART_TEST_SELECTIVE_CAPTIVE:
		break;
	default:
		/* Vendor-specific codes (40h-7Eh, C0h-FFh) are passed on;
		 * reserved ones are refused. */
		if (!((test >= 0x40 && test <= 0x7E) || test >= 0xC0))
			return -EINVAL;
		break;
	}
	captive = (test & 0x80) && test != ATA_SMART_TEST_ABORT;

	/* Check the capability bits and size the captive timeout from the
	 * drive's own estimate. */
	data = kalloc(ATA_SECT_SIZE);
	if (!data)
		return -ENOMEM;
	if (ata_smart_read_data(dev, data) == 0) {
		uint8_t cap = data[ATA_SMART_DATA_OFFLINE_CAP];
		uint8_t base = test & 0x7F;

		if (base == ATA_SMART_TEST_OFFLINE &&
		    !(cap & SMART_OFFCAP_EXEC_OFFLINE))
			ret = -EOPNOTSUPP;
		else if ((base == ATA_SMART_TEST_SHORT ||
			  base == ATA_SMART_TEST_EXTENDED) &&
			 !(cap & SMART_OFFCAP_SELF_TEST))
			ret = -EOPNOTSUPP;
		else if (base == ATA_SMART_TEST_CONVEYANCE &&
			 !(cap & SMART_OFFCAP_CONVEYANCE))
			ret = -EOPNOTSUPP;
		else if (base == ATA_SMART_TEST_SELECTIVE &&
			 !(cap & SMART_OFFCAP_SELECTIVE))
			ret = -EOPNOTSUPP;
		else if (test == ATA_SMART_TEST_ABORT &&
			 !(cap & SMART_OFFCAP_ABORT_RESTART) &&
			 !(cap & SMART_OFFCAP_SELF_TEST))
			ret = -EOPNOTSUPP;
		minutes = smart_test_minutes(data, test);
	}
	kfree(data);
	if (ret) {
		kprintf(SMART_PFX "%s: %s not supported by the drive\n",
			dev->name, smart_test_name(test));
		return ret;
	}

	if (captive) {
		/* The command completes when the test does: twice the
		 * estimate plus five minutes, at least an hour when the
		 * drive gives no estimate. */
		uint64_t ms = minutes ? ((uint64_t)minutes * 2 + 5) * 60000ULL
				      : 60ULL * 60000ULL;

		if (ms > 0xFFFFFFF0ull)
			ms = 0xFFFFFFF0ull;
		timeout_ms = (uint32_t)ms;
	}

	kprintf(SMART_PFX "%s: starting %s%s\n", dev->name,
		smart_test_name(test),
		minutes ? "" : " (no duration estimate)");
	ret = smart_cmd(dev, ATA_SMART_IMMEDIATE_OFFLINE, test, 0, NULL, 0,
			timeout_ms, NULL, 0);
	if (ret == 0 && minutes && !captive && test != ATA_SMART_TEST_ABORT)
		kprintf(SMART_PFX "%s: %s running, about %u minutes\n",
			dev->name, smart_test_name(test), minutes);
	else if (ret == 0 && captive)
		kprintf(SMART_PFX "%s: %s finished\n", dev->name,
			smart_test_name(test));
	return ret;
}

int ata_smart_autosave(struct ata_device *dev, int on)
{
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (ata_can_sleep())
		might_sleep();
	return smart_cmd(dev, ATA_SMART_AUTOSAVE, 0, on ? 0xF1 : 0x00, NULL, 0,
			 0, NULL, 0);
}

int ata_smart_auto_offline(struct ata_device *dev, int on)
{
	int ret = smart_check(dev);

	if (ret)
		return ret;
	if (ata_can_sleep())
		might_sleep();
	return smart_cmd(dev, ATA_SMART_AUTO_OFFLINE, 0, on ? 0xF8 : 0x00,
			 NULL, 0, 0, NULL, 0);
}

/* ==========================================================================
 * NCQ command error log (10h) -- shared with the error handler
 * ========================================================================== */

int ata_ncq_err_decode(const void *buf512, struct ata_ncq_err_info *out)
{
	const uint8_t *p = (const uint8_t *)buf512;
	int any = 0;

	if (!p || !out)
		return -EINVAL;
	kmemset(out, 0, sizeof(*out));

	for (int i = 0; i < ATA_SECT_SIZE - 1; i++)
		if (p[i]) {
			any = 1;
			break;
		}
	out->checksum_ok = (uint8_t)ata_smart_checksum_ok(p);
	if (!any) {
		out->tag = -1;
		return -ENOENT;
	}

	out->nq = (p[ATA_NCQ_ERR_TAG_BYTE] & ATA_NCQ_ERR_NQ) ? 1 : 0;
	out->tag = out->nq ? -1 : (p[ATA_NCQ_ERR_TAG_BYTE] & 0x1F);
	out->status = p[ATA_NCQ_ERR_STATUS];
	out->error = p[ATA_NCQ_ERR_ERROR];
	out->device = p[ATA_NCQ_ERR_DEVICE];
	out->lba = (uint64_t)p[ATA_NCQ_ERR_LBA0] |
		   ((uint64_t)p[ATA_NCQ_ERR_LBA0 + 1] << 8) |
		   ((uint64_t)p[ATA_NCQ_ERR_LBA0 + 2] << 16) |
		   ((uint64_t)p[ATA_NCQ_ERR_LBA3] << 24) |
		   ((uint64_t)p[ATA_NCQ_ERR_LBA3 + 1] << 32) |
		   ((uint64_t)p[ATA_NCQ_ERR_LBA3 + 2] << 40);
	out->count = sm_le16(p + ATA_NCQ_ERR_COUNT);
	out->sense_key = p[ATA_NCQ_ERR_SENSE_KEY] & 0x0F;
	out->asc = p[ATA_NCQ_ERR_ASC];
	out->ascq = p[ATA_NCQ_ERR_ASCQ];
	out->sense_valid =
		(uint8_t)(out->sense_key || out->asc || out->ascq ? 1 : 0);
	return 0;
}

/* ==========================================================================
 * Printers of the pages
 * ========================================================================== */

/* SMART READ DATA + thresholds: the attribute table. */
static void smart_print_attrs(struct ata_device *dev, const uint8_t *data,
			      const uint8_t *thr)
{
	struct smart_attr *attrs;
	int n;

	attrs = kcalloc(ATA_SMART_NUM_ATTRS, sizeof(*attrs));
	if (!attrs)
		return;
	n = smart_decode_attrs(data, thr, attrs, ATA_SMART_NUM_ATTRS);
	kprintf(SMART_PFX "%s: SMART attributes (revision %u, %d entries):\n",
		dev->name, sm_le16(data + ATA_SMART_DATA_REVISION), n);
	kprintf(SMART_PFX "%s:   ID# NAME                     FLAGS  VAL "
			  "WORST THRESH  WHEN_FAILED  RAW\n",
		dev->name);
	for (int i = 0; i < n; i++) {
		const struct smart_attr *a = &attrs[i];
		const struct smart_attr_desc *d = smart_attr_lookup(a->id);
		char fl[8], name[32];
		int tmin, tmax;
		uint64_t rv = smart_attr_raw_value(a, &tmin, &tmax);

		smart_flags_str(a->flags, fl, sizeof(fl));
		if (d)
			ksnprintf(name, sizeof(name), "%s", d->name);
		else
			ksnprintf(name, sizeof(name), "Unknown_Attribute");
		if (tmax)
			kprintf(SMART_PFX "%s:   %3u %-24s %s %3u %5u %6u  %-11s  "
					  "%llu (min/max %d/%d)\n",
				dev->name, a->id, name, fl, a->value, a->worst,
				a->threshold,
				a->failing_now	   ? "FAILING_NOW"
				: a->failed_past ? "In_the_past"
						   : "-",
				(unsigned long long)rv, tmin, tmax);
		else
			kprintf(SMART_PFX "%s:   %3u %-24s %s %3u %5u %6u  %-11s  "
					  "%llu\n",
				dev->name, a->id, name, fl, a->value, a->worst,
				a->threshold,
				a->failing_now	   ? "FAILING_NOW"
				: a->failed_past ? "In_the_past"
						   : "-",
				(unsigned long long)rv);
	}
	kfree(attrs);
}

static void smart_print_capabilities(struct ata_device *dev,
				     const uint8_t *data)
{
	uint8_t off_status = data[ATA_SMART_DATA_OFFLINE_STATUS];
	uint8_t st_status = data[ATA_SMART_DATA_SELFTEST_STATUS];
	uint8_t cap = data[ATA_SMART_DATA_OFFLINE_CAP];
	uint16_t scap = sm_le16(data + ATA_SMART_DATA_SMART_CAP);
	uint32_t ext = data[ATA_SMART_DATA_EXT_TIME];

	if (ext == 0xFF)
		ext = sm_le16(data + ATA_SMART_DATA_EXT_TIME_WORD);
	kprintf(SMART_PFX "%s: off-line data collection: %s (auto off-line "
			  "%s), %u s\n",
		dev->name, smart_offline_status_str(off_status),
		(off_status & 0x80) ? "enabled" : "disabled",
		sm_le16(data + ATA_SMART_DATA_OFFLINE_TIME));
	if ((st_status >> 4) == 0xF)
		kprintf(SMART_PFX "%s: self-test: in progress, %u%% remaining\n",
			dev->name, (st_status & 0x0F) * 10);
	else
		kprintf(SMART_PFX "%s: self-test: last %s\n", dev->name,
			smart_selftest_status_str(st_status));
	kprintf(SMART_PFX "%s: capabilities:%s%s%s%s%s%s%s%s%s%s\n", dev->name,
		(cap & SMART_OFFCAP_EXEC_OFFLINE) ? " offline-immediate" : "",
		(cap & SMART_OFFCAP_AUTO_OFFLINE) ? " auto-offline" : "",
		(cap & SMART_OFFCAP_ABORT_RESTART) ? " abort/restart" : "",
		(cap & SMART_OFFCAP_READ_SCAN) ? " read-scan" : "",
		(cap & SMART_OFFCAP_SELF_TEST) ? " self-test" : "",
		(cap & SMART_OFFCAP_CONVEYANCE) ? " conveyance" : "",
		(cap & SMART_OFFCAP_SELECTIVE) ? " selective" : "",
		(scap & SMART_CAP_SAVES_BEFORE_PM) ? " saves-before-pm" : "",
		(scap & SMART_CAP_AUTOSAVE) ? " autosave" : "",
		(data[ATA_SMART_DATA_ERRLOG_CAP] & 1) ? " error-logging" : "");
	if (cap & SMART_OFFCAP_SELF_TEST)
		kprintf(SMART_PFX "%s: recommended polling: short %u min, "
				  "extended %u min%s\n",
			dev->name, data[ATA_SMART_DATA_SHORT_TIME], ext,
			(cap & SMART_OFFCAP_CONVEYANCE) ? "" : "");
	if (cap & SMART_OFFCAP_CONVEYANCE)
		kprintf(SMART_PFX "%s: recommended polling: conveyance %u min\n",
			dev->name, data[ATA_SMART_DATA_CONV_TIME]);
}

/* A 28-bit error log entry (90 bytes) of logs 01h/02h. */
static void smart_print_err_entry28(struct ata_device *dev, const uint8_t *e,
				    unsigned int number)
{
	const uint8_t *er = e + SMART_ERRLOG_ERR_OFS;
	uint32_t lba = (uint32_t)er[3] | ((uint32_t)er[4] << 8) |
		       ((uint32_t)er[5] << 16) | ((uint32_t)(er[6] & 0x0F) << 24);
	char es[48];

	ata_error_str(er[1], es, sizeof(es));
	kprintf(SMART_PFX "%s: error %u at %u power-on hours, state: %s\n",
		dev->name, number, sm_le16(er + 28),
		smart_err_state_str(er[27]));
	kprintf(SMART_PFX "%s:   ER 0x%02x %s ST 0x%02x SC 0x%02x LBA %u "
			  "DEV 0x%02x\n",
		dev->name, er[1], es, er[7], er[2], lba, er[6]);
	/* The five commands that led to it, oldest first. */
	for (int c = 0; c < SMART_ERRLOG_CMDS; c++) {
		const uint8_t *cd = e + c * SMART_ERRLOG_CMD_SIZE;
		uint32_t clba;

		if (cd[7] == 0 && cd[1] == 0 && cd[2] == 0 && sm_le32(cd + 8) == 0)
			continue;
		clba = (uint32_t)cd[3] | ((uint32_t)cd[4] << 8) |
		       ((uint32_t)cd[5] << 16) | ((uint32_t)(cd[6] & 0x0F) << 24);
		kprintf(SMART_PFX "%s:   cmd %02x FR %02x SC %02x LBA %u DEV "
				  "%02x DC %02x at %u ms: %s\n",
			dev->name, cd[7], cd[1], cd[2], clba, cd[6], cd[0],
			sm_le32(cd + 8), ata_cmd_name(cd[7]));
	}
}

/* SMART summary error log (01h). */
static void smart_print_summary_errlog(struct ata_device *dev,
				       const uint8_t *log)
{
	uint8_t index = log[SMART_ERRLOG_INDEX];
	uint16_t count = sm_le16(log + SMART_ERRLOG_COUNT);

	if (!ata_smart_checksum_ok(log))
		kprintf(SMART_PFX "%s: summary error log checksum mismatch\n",
			dev->name);
	if (count == 0 || index == 0) {
		kprintf(SMART_PFX "%s: SMART error log: no errors logged "
				  "(version %u)\n",
			dev->name, log[SMART_ERRLOG_VERSION]);
		return;
	}
	if (index > SMART_ERRLOG_ENTRIES) {
		kprintf(SMART_PFX "%s: SMART error log: bad index %u\n",
			dev->name, index);
		return;
	}
	kprintf(SMART_PFX "%s: SMART error log: %u errors in the drive's "
			  "lifetime, the last %u:\n",
		dev->name, count,
		count < SMART_ERRLOG_ENTRIES ? count : SMART_ERRLOG_ENTRIES);
	/* index points to the most recent entry; walk backwards */
	for (int i = 0; i < SMART_ERRLOG_ENTRIES && i < (int)count &&
			i < SMART_PRINT_ERRORS;
	     i++) {
		int slot = ((int)index - 1 - i + SMART_ERRLOG_ENTRIES) %
			   SMART_ERRLOG_ENTRIES;
		const uint8_t *e = log + SMART_ERRLOG_DATA +
				   slot * SMART_ERRLOG_ENTRY_SIZE;
		int empty = 1;

		for (int b = 0; b < SMART_ERRLOG_ENTRY_SIZE; b++)
			if (e[b]) {
				empty = 0;
				break;
			}
		if (empty)
			continue;
		smart_print_err_entry28(dev, e, (unsigned int)count - i);
	}
}

/* An extended (48-bit) error log entry (124 bytes) of log 03h. */
static void smart_print_err_entry48(struct ata_device *dev, const uint8_t *e,
				    unsigned int number)
{
	const uint8_t *er = e + SMART_XERRLOG_ERR_OFS;
	uint64_t lba = sm_le48(er + 4);
	char es[48];

	ata_error_str(er[1], es, sizeof(es));
	kprintf(SMART_PFX "%s: error %u at %u power-on hours, state: %s\n",
		dev->name, number, sm_le16(er + 32),
		smart_err_state_str(er[31]));
	kprintf(SMART_PFX "%s:   ER 0x%02x %s ST 0x%02x SC 0x%04x LBA %llu "
			  "DEV 0x%02x\n",
		dev->name, er[1], es, er[11], sm_le16(er + 2),
		(unsigned long long)lba, er[10]);
	for (int c = 0; c < SMART_XERRLOG_CMDS; c++) {
		const uint8_t *cd = e + c * SMART_XERRLOG_CMD_SIZE;

		if (cd[12] == 0 && sm_le48(cd + 5) == 0 && cd[3] == 0)
			continue;
		kprintf(SMART_PFX "%s:   cmd %02x FR %04x SC %04x LBA %llu DEV "
				  "%02x DC %02x at %u ms: %s\n",
			dev->name, cd[12], (uint16_t)(cd[1] | (cd[2] << 8)),
			(uint16_t)(cd[3] | (cd[4] << 8)),
			(unsigned long long)sm_le48(cd + 5), cd[11], cd[0],
			sm_le32(cd + 14), ata_cmd_name(cd[12]));
	}
}

/* The SMART self-test log (06h). */
static void smart_print_selftest_log(struct ata_device *dev,
				     const uint8_t *log)
{
	uint8_t index = log[SMART_STLOG_INDEX];

	if (!ata_smart_checksum_ok(log))
		kprintf(SMART_PFX "%s: self-test log checksum mismatch\n",
			dev->name);
	if (index == 0 || index > SMART_STLOG_ENTRIES) {
		kprintf(SMART_PFX "%s: SMART self-test log: no self-tests "
				  "logged\n",
			dev->name);
		return;
	}
	kprintf(SMART_PFX "%s: SMART self-test log (revision %u), most recent "
			  "first:\n",
		dev->name, sm_le16(log + SMART_STLOG_REVISION));
	for (int i = 0; i < SMART_STLOG_ENTRIES && i < SMART_PRINT_SELFTESTS;
	     i++) {
		int slot = ((int)index - 1 - i + SMART_STLOG_ENTRIES) %
			   SMART_STLOG_ENTRIES;
		const uint8_t *d = log + SMART_STLOG_DATA +
				   slot * SMART_STLOG_ENTRY_SIZE;
		uint32_t flba = sm_le32(d + 5);

		if (d[0] == 0 && d[1] == 0 && sm_le16(d + 2) == 0)
			continue;
		if ((d[1] >> 4) == 0xF)
			kprintf(SMART_PFX "%s:   #%d %-30s %s (%u%% left), %u "
					  "h\n",
				dev->name, i + 1, smart_test_name(d[0]),
				smart_selftest_status_str(d[1]),
				(d[1] & 0x0F) * 10, sm_le16(d + 2));
		else if ((d[1] >> 4) >= 3 && (d[1] >> 4) <= 8 &&
			 flba != 0xFFFFFFFFu)
			kprintf(SMART_PFX "%s:   #%d %-30s %s, %u h, first error "
					  "at LBA %u\n",
				dev->name, i + 1, smart_test_name(d[0]),
				smart_selftest_status_str(d[1]),
				sm_le16(d + 2), flba);
		else
			kprintf(SMART_PFX "%s:   #%d %-30s %s, %u h\n",
				dev->name, i + 1, smart_test_name(d[0]),
				smart_selftest_status_str(d[1]),
				sm_le16(d + 2));
	}
}

/* The extended self-test log (07h), page 0. */
static void smart_print_ext_selftest_log(struct ata_device *dev,
					 const uint8_t *log, uint16_t pages)
{
	uint16_t index = sm_le16(log + SMART_XSTLOG_INDEX);

	if (index == 0) {
		kprintf(SMART_PFX "%s: extended self-test log: no self-tests "
				  "logged\n",
			dev->name);
		return;
	}
	kprintf(SMART_PFX "%s: extended self-test log (%u pages), most recent "
			  "first:\n",
		dev->name, pages);
	/* Only page 0 was read: show the entries it holds, walking back from
	 * the index when the index lies on it. */
	for (int i = 0; i < SMART_PRINT_SELFTESTS; i++) {
		int slot = (int)index - 1 - i;
		const uint8_t *d;
		uint64_t flba;

		if (slot < 0 || slot >= SMART_XSTLOG_ENTRIES)
			break;
		d = log + SMART_XSTLOG_DATA + slot * SMART_XSTLOG_ENTRY_SIZE;
		if (d[0] == 0 && d[1] == 0)
			continue;
		flba = sm_le48(d + 5);
		if ((d[1] >> 4) >= 3 && (d[1] >> 4) <= 8 &&
		    flba != 0xFFFFFFFFFFFFull)
			kprintf(SMART_PFX "%s:   #%d %-30s %s, %u h, first error "
					  "at LBA %llu\n",
				dev->name, i + 1, smart_test_name(d[0]),
				smart_selftest_status_str(d[1]),
				sm_le16(d + 2), (unsigned long long)flba);
		else
			kprintf(SMART_PFX "%s:   #%d %-30s %s, %u h\n",
				dev->name, i + 1, smart_test_name(d[0]),
				smart_selftest_status_str(d[1]),
				sm_le16(d + 2));
	}
}

/* ---- Device statistics (04h) ------------------------------------------ */

enum devstat_unit {
	DS_COUNT = 0,
	DS_HOURS,
	DS_MINUTES,
	DS_CELSIUS, /* signed 8-bit */
	DS_PERCENT,
	DS_MS,
};

struct devstat_desc {
	uint8_t page;
	uint16_t offset;
	uint8_t unit;
	const char *name;
};

static const struct devstat_desc devstat_table[] = {
	{ DEVSTAT_GENERAL, 0x08, DS_COUNT, "Lifetime Power-On Resets" },
	{ DEVSTAT_GENERAL, 0x10, DS_HOURS, "Power-on Hours" },
	{ DEVSTAT_GENERAL, 0x18, DS_COUNT, "Logical Sectors Written" },
	{ DEVSTAT_GENERAL, 0x20, DS_COUNT, "Number of Write Commands" },
	{ DEVSTAT_GENERAL, 0x28, DS_COUNT, "Logical Sectors Read" },
	{ DEVSTAT_GENERAL, 0x30, DS_COUNT, "Number of Read Commands" },
	{ DEVSTAT_GENERAL, 0x38, DS_MS, "Date and Time TimeStamp" },
	{ DEVSTAT_GENERAL, 0x40, DS_COUNT, "Pending Error Count" },
	{ DEVSTAT_GENERAL, 0x48, DS_PERCENT, "Workload Utilization" },
	{ DEVSTAT_GENERAL, 0x50, DS_COUNT, "Utilization Usage Rate" },
	{ DEVSTAT_GENERAL, 0x58, DS_COUNT, "Resource Availability" },
	{ DEVSTAT_GENERAL, 0x60, DS_PERCENT, "Random Write Resources Used" },
	{ DEVSTAT_FREE_FALL, 0x08, DS_COUNT, "Number of Free-Fall Events" },
	{ DEVSTAT_FREE_FALL, 0x10, DS_COUNT, "Overlimit Shock Events" },
	{ DEVSTAT_ROTATING, 0x08, DS_HOURS, "Spindle Motor Power-on Hours" },
	{ DEVSTAT_ROTATING, 0x10, DS_HOURS, "Head Flying Hours" },
	{ DEVSTAT_ROTATING, 0x18, DS_COUNT, "Head Load Events" },
	{ DEVSTAT_ROTATING, 0x20, DS_COUNT, "Reallocated Logical Sectors" },
	{ DEVSTAT_ROTATING, 0x28, DS_COUNT, "Read Recovery Attempts" },
	{ DEVSTAT_ROTATING, 0x30, DS_COUNT, "Mechanical Start Failures" },
	{ DEVSTAT_ROTATING, 0x38, DS_COUNT, "Reallocation Candidate Sectors" },
	{ DEVSTAT_ROTATING, 0x40, DS_COUNT, "High Priority Unload Events" },
	{ DEVSTAT_GENERAL_ERRORS, 0x08, DS_COUNT,
	  "Reported Uncorrectable Errors" },
	{ DEVSTAT_GENERAL_ERRORS, 0x10, DS_COUNT,
	  "Resets Between Command Acceptance and Completion" },
	{ DEVSTAT_GENERAL_ERRORS, 0x18, DS_COUNT,
	  "Physical Element Status Changed" },
	{ DEVSTAT_TEMPERATURE, 0x08, DS_CELSIUS, "Current Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x10, DS_CELSIUS,
	  "Average Short Term Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x18, DS_CELSIUS,
	  "Average Long Term Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x20, DS_CELSIUS, "Highest Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x28, DS_CELSIUS, "Lowest Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x30, DS_CELSIUS,
	  "Highest Average Short Term Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x38, DS_CELSIUS,
	  "Lowest Average Short Term Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x40, DS_CELSIUS,
	  "Highest Average Long Term Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x48, DS_CELSIUS,
	  "Lowest Average Long Term Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x50, DS_MINUTES, "Time in Over-Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x58, DS_CELSIUS,
	  "Specified Maximum Operating Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x60, DS_MINUTES,
	  "Time in Under-Temperature" },
	{ DEVSTAT_TEMPERATURE, 0x68, DS_CELSIUS,
	  "Specified Minimum Operating Temperature" },
	{ DEVSTAT_TRANSPORT, 0x08, DS_COUNT, "Number of Hardware Resets" },
	{ DEVSTAT_TRANSPORT, 0x10, DS_COUNT, "Number of ASR Events" },
	{ DEVSTAT_TRANSPORT, 0x18, DS_COUNT, "Number of Interface CRC Errors" },
	{ DEVSTAT_SSD, 0x08, DS_PERCENT, "Percentage Used Endurance Indicator" },
};

static const char *devstat_page_name(uint8_t page)
{
	switch (page) {
	case DEVSTAT_PAGE_LIST:
		return "List of supported pages";
	case DEVSTAT_GENERAL:
		return "General Statistics";
	case DEVSTAT_FREE_FALL:
		return "Free-Fall Statistics";
	case DEVSTAT_ROTATING:
		return "Rotating Media Statistics";
	case DEVSTAT_GENERAL_ERRORS:
		return "General Errors Statistics";
	case DEVSTAT_TEMPERATURE:
		return "Temperature Statistics";
	case DEVSTAT_TRANSPORT:
		return "Transport Statistics";
	case DEVSTAT_SSD:
		return "Solid State Device Statistics";
	default:
		return "Vendor/reserved page";
	}
}

/* One statistic: 1 printed (supported and valid), 0 skipped.  Pure apart
 * from the print. */
static int devstat_print_one(struct ata_device *dev, const uint8_t *page,
			     const struct devstat_desc *d)
{
	uint64_t q = sm_le64(page + d->offset);
	uint64_t v;

	if (!(q & DEVSTAT_SUPPORTED) || !(q & DEVSTAT_VALID))
		return 0;
	v = q & 0x0000FFFFFFFFFFFFull;
	switch (d->unit) {
	case DS_CELSIUS: {
		int t = (int8_t)(uint8_t)(q & 0xFF);

		if ((uint8_t)(q & 0xFF) == 0x80) /* "no value" */
			return 0;
		kprintf(SMART_PFX "%s:   %-48s %d C%s\n", dev->name, d->name, t,
			(q & DEVSTAT_MONITORED) ? " (condition met)" : "");
		return 1;
	}
	case DS_PERCENT:
		kprintf(SMART_PFX "%s:   %-48s %u%%%s\n", dev->name, d->name,
			(unsigned int)(q & 0xFF),
			(q & DEVSTAT_NORMALIZED) ? " (normalized)" : "");
		return 1;
	case DS_HOURS:
		kprintf(SMART_PFX "%s:   %-48s %llu h\n", dev->name, d->name,
			(unsigned long long)(v & 0xFFFFFFFFull));
		return 1;
	case DS_MINUTES:
		kprintf(SMART_PFX "%s:   %-48s %llu min\n", dev->name, d->name,
			(unsigned long long)(v & 0xFFFFFFFFull));
		return 1;
	case DS_MS:
		kprintf(SMART_PFX "%s:   %-48s %llu ms\n", dev->name, d->name,
			(unsigned long long)v);
		return 1;
	default:
		kprintf(SMART_PFX "%s:   %-48s %llu%s\n", dev->name, d->name,
			(unsigned long long)v,
			(q & DEVSTAT_MONITORED) ? " (condition met)" : "");
		return 1;
	}
}

/* Read and print the device statistics log.  Returns the current
 * temperature (Celsius) through *temp when found (else leaves it). */
static void smart_devstat(struct ata_device *dev, uint8_t *buf, int verbose,
			  int *temp, uint64_t *poh, int *endurance)
{
	uint8_t pages[16];
	int npages, ret;

	ret = ata_read_log_ext(dev, ATA_LOG_DEVSTAT, DEVSTAT_PAGE_LIST, buf, 1,
			       0);
	if (ret)
		return;
	/* Page 0: header qword, then byte 8 = number of entries, bytes 9.. =
	 * the supported page numbers. */
	npages = buf[8];
	if (npages > (int)sizeof(pages))
		npages = sizeof(pages);
	for (int i = 0; i < npages; i++)
		pages[i] = buf[9 + i];
	if (verbose)
		kprintf(SMART_PFX "%s: device statistics: %d pages\n",
			dev->name, npages);

	for (int i = 0; i < npages; i++) {
		uint8_t pg = pages[i];
		uint16_t rev;

		if (pg == DEVSTAT_PAGE_LIST || pg > DEVSTAT_MAX_PAGE)
			continue;
		if (ata_read_log_ext(dev, ATA_LOG_DEVSTAT, pg, buf, 1, 0))
			continue;
		rev = sm_le16(buf);
		/* The header names the page; a mismatch is a broken log. */
		if (buf[2] != pg) {
			WARN_RATELIMIT(1,
				       SMART_PFX "%s: device statistics page "
						 "%u reports number %u",
				       dev->name, pg, buf[2]);
			continue;
		}
		if (verbose)
			kprintf(SMART_PFX "%s:  page %02xh %s (revision %u)\n",
				dev->name, pg, devstat_page_name(pg), rev);
		for (size_t k = 0;
		     k < sizeof(devstat_table) / sizeof(devstat_table[0]); k++) {
			const struct devstat_desc *d = &devstat_table[k];
			uint64_t q;

			if (d->page != pg)
				continue;
			q = sm_le64(buf + d->offset);
			if ((q & DEVSTAT_SUPPORTED) && (q & DEVSTAT_VALID)) {
				if (pg == DEVSTAT_TEMPERATURE &&
				    d->offset == 0x08 && temp &&
				    (uint8_t)(q & 0xFF) != 0x80)
					*temp = (int8_t)(uint8_t)(q & 0xFF);
				if (pg == DEVSTAT_GENERAL && d->offset == 0x10 &&
				    poh)
					*poh = q & 0xFFFFFFFFull;
				if (pg == DEVSTAT_SSD && d->offset == 0x08 &&
				    endurance)
					*endurance = (int)(q & 0xFF);
			}
			if (verbose)
				devstat_print_one(dev, buf, d);
		}
	}
}

/* ==========================================================================
 * The probe summary
 * ========================================================================== */

/* Find one attribute by id in a decoded table. */
static const struct smart_attr *smart_find(const struct smart_attr *a, int n,
					   uint8_t id)
{
	for (int i = 0; i < n; i++)
		if (a[i].id == id)
			return &a[i];
	return NULL;
}

void ata_smart_report(struct ata_device *dev)
{
	uint8_t *data = NULL, *thr = NULL, *log = NULL;
	struct smart_attr *attrs = NULL;
	const struct smart_attr *a;
	int n = 0, failing = 0, health_known = 0;
	int temp = -1000, endurance = -1;
	uint64_t poh = (uint64_t)-1;
	char line[192];
	int len;

	if (smart_check(dev))
		return;
	if (ata_can_sleep())
		might_sleep();

	if (!(dev->flags & ATA_DFLAG_SMART_ON)) {
		/* SMART is supported but switched off.  Reading it would be
		 * aborted; say so instead of turning it on behind the
		 * administrator's back. */
		kprintf(SMART_PFX "%s: SMART supported, disabled\n", dev->name);
		return;
	}

	data = kalloc(ATA_SECT_SIZE);
	thr = kalloc(ATA_SECT_SIZE);
	log = kalloc(ATA_SECT_SIZE);
	attrs = kcalloc(ATA_SMART_NUM_ATTRS, sizeof(*attrs));
	if (!data || !thr || !log || !attrs)
		goto out;

	if (ata_smart_return_status(dev, &failing) == 0)
		health_known = 1;

	if (ata_smart_read_data(dev, data) == 0) {
		int have_thr = ata_smart_read_thresholds(dev, thr) == 0;

		n = smart_decode_attrs(data, have_thr ? thr : NULL, attrs,
				       ATA_SMART_NUM_ATTRS);
	}

	/* Temperature and power-on hours: attributes first, the device
	 * statistics log refines them when present. */
	a = smart_find(attrs, n, 194);
	if (!a)
		a = smart_find(attrs, n, 190);
	if (a) {
		int cur = (int)smart_attr_raw_value(a, NULL, NULL);

		if (cur > 0 && cur < 128)
			temp = cur;
	}
	a = smart_find(attrs, n, 9);
	if (a)
		poh = smart_attr_raw_value(a, NULL, NULL);
	if (dev->flags & ATA_DFLAG_GPL)
		smart_devstat(dev, log, 0, &temp, &poh, &endurance);

	len = ksnprintf(line, sizeof(line), "health %s",
			!health_known ? "unknown"
			: failing     ? "FAILING"
				      : "PASSED");
	if (temp > -100)
		len += ksnprintf(line + len, sizeof(line) - len, ", %d C", temp);
	if (poh != (uint64_t)-1)
		len += ksnprintf(line + len, sizeof(line) - len,
				 ", %llu power-on hours",
				 (unsigned long long)poh);
	a = smart_find(attrs, n, 5);
	if (a)
		len += ksnprintf(line + len, sizeof(line) - len,
				 ", %llu reallocated",
				 (unsigned long long)a->raw);
	a = smart_find(attrs, n, 197);
	if (a)
		len += ksnprintf(line + len, sizeof(line) - len,
				 ", %llu pending",
				 (unsigned long long)a->raw);
	a = smart_find(attrs, n, 198);
	if (a && a->raw)
		len += ksnprintf(line + len, sizeof(line) - len,
				 ", %llu offline-uncorrectable",
				 (unsigned long long)a->raw);
	a = smart_find(attrs, n, 199);
	if (a && a->raw)
		len += ksnprintf(line + len, sizeof(line) - len,
				 ", %llu CRC errors",
				 (unsigned long long)a->raw);
	if (endurance >= 0)
		len += ksnprintf(line + len, sizeof(line) - len,
				 ", %d%% endurance used", endurance);
	(void)len;
	kprintf(SMART_PFX "%s: SMART: %s\n", dev->name, line);

	/* Attributes past their threshold are worth a line each. */
	for (int i = 0; i < n; i++) {
		const struct smart_attr_desc *d;

		if (!attrs[i].failing_now && !attrs[i].failed_past)
			continue;
		d = smart_attr_lookup(attrs[i].id);
		kprintf(SMART_PFX "%s: SMART attribute %u %s %s (value %u, "
				  "worst %u, threshold %u)\n",
			dev->name, attrs[i].id, d ? d->name : "Unknown",
			attrs[i].failing_now ? "FAILING NOW"
					     : "failed in the past",
			attrs[i].value, attrs[i].worst, attrs[i].threshold);
	}

	/* The error count from the summary error log, when logging works. */
	if (ata_smart_read_log(dev, ATA_LOG_SUMMARY_ERR, log, 1) == 0) {
		uint16_t errs = sm_le16(log + SMART_ERRLOG_COUNT);

		if (errs)
			kprintf(SMART_PFX "%s: SMART error log: %u errors "
					  "recorded in the drive's lifetime\n",
				dev->name, errs);
	}
	/* And the outcome of the last self-test, if one failed. */
	if (n && (data[ATA_SMART_DATA_SELFTEST_STATUS] >> 4) >= 3 &&
	    (data[ATA_SMART_DATA_SELFTEST_STATUS] >> 4) <= 8)
		kprintf(SMART_PFX "%s: last SMART self-test: %s\n", dev->name,
			smart_selftest_status_str(
				data[ATA_SMART_DATA_SELFTEST_STATUS]));
out:
	kfree(attrs);
	kfree(log);
	kfree(thr);
	kfree(data);
}

/* ==========================================================================
 * The full dump
 * ========================================================================== */

/* GPL directory: pages of each log address.  Returns 0 and fills pages[]
 * (256 entries) or -errno. */
static int smart_gpl_directory(struct ata_device *dev, uint8_t *buf,
			       uint16_t *pages)
{
	int ret;

	if (!(dev->flags & ATA_DFLAG_GPL))
		return -EOPNOTSUPP;
	ret = ata_read_log_ext(dev, ATA_LOG_DIRECTORY, 0, buf, 1, 0);
	if (ret)
		return ret;
	for (int i = 0; i < 256; i++)
		pages[i] = sm_le16(buf + 2 * i);
	/* word 0 is the GPL version (0001h), not a page count */
	if (pages[0] != 0x0001)
		WARN_RATELIMIT(1,
			       SMART_PFX "%s: GPL directory version %u",
			       dev->name, pages[0]);
	return 0;
}

static void smart_dump_gpl(struct ata_device *dev, uint8_t *buf)
{
	uint16_t *pages;
	char line[160];
	int len = 0, shown = 0;

	pages = kcalloc(256, sizeof(uint16_t));
	if (!pages)
		return;
	if (smart_gpl_directory(dev, buf, pages)) {
		kfree(pages);
		return;
	}

	for (int log = 1; log < 256; log++) {
		if (!pages[log])
			continue;
		len += ksnprintf(line + len, sizeof(line) - len, " %02xh:%u",
				 log, pages[log]);
		if (len > 120) {
			kprintf(SMART_PFX "%s: GPL logs%s\n", dev->name, line);
			len = 0;
			line[0] = '\0';
		}
		shown++;
	}
	if (len)
		kprintf(SMART_PFX "%s: GPL logs%s\n", dev->name, line);
	if (!shown)
		kprintf(SMART_PFX "%s: GPL directory lists no logs\n",
			dev->name);

	/* Extended comprehensive error log (03h) */
	if (pages[ATA_LOG_EXT_COMP_ERR] &&
	    ata_read_log_ext(dev, ATA_LOG_EXT_COMP_ERR, 0, buf, 1, 0) == 0) {
		uint16_t index = sm_le16(buf + SMART_XERRLOG_INDEX);
		uint16_t count = sm_le16(buf + SMART_XERRLOG_COUNT);
		int pages_err = pages[ATA_LOG_EXT_COMP_ERR];

		if (count == 0 || index == 0) {
			kprintf(SMART_PFX "%s: extended error log: no errors "
					  "logged\n",
				dev->name);
		} else {
			kprintf(SMART_PFX "%s: extended error log (%d pages): "
					  "%u errors, most recent first:\n",
				dev->name, pages_err, count);
			/* Walk back from the index through the pages it
			 * spans; each page read on demand. */
			int cur_page = 0;

			for (int i = 0; i < SMART_PRINT_ERRORS && i < count;
			     i++) {
				int total = pages_err * SMART_XERRLOG_ENTRIES;
				int slot = ((int)index - 1 - i) % total;
				int page, ent;

				if (slot < 0)
					slot += total;
				page = slot / SMART_XERRLOG_ENTRIES;
				ent = slot % SMART_XERRLOG_ENTRIES;
				if (page != cur_page) {
					if (ata_read_log_ext(
						    dev, ATA_LOG_EXT_COMP_ERR,
						    (uint16_t)page, buf, 1, 0))
						break;
					cur_page = page;
				}
				smart_print_err_entry48(
					dev,
					buf + SMART_XERRLOG_DATA +
						ent * SMART_XERRLOG_ENTRY_SIZE,
					(unsigned int)count - i);
			}
		}
	}

	/* Extended self-test log (07h) */
	if (pages[ATA_LOG_EXT_SELFTEST] &&
	    ata_read_log_ext(dev, ATA_LOG_EXT_SELFTEST, 0, buf, 1, 0) == 0)
		smart_print_ext_selftest_log(dev, buf,
					     pages[ATA_LOG_EXT_SELFTEST]);

	/* Device statistics (04h) */
	if (pages[ATA_LOG_DEVSTAT])
		smart_devstat(dev, buf, 1, NULL, NULL, NULL);

	/* NCQ command error log (10h): what the last NCQ failure was */
	if (pages[ATA_LOG_NCQ_CMD_ERR] &&
	    ata_read_log_ext(dev, ATA_LOG_NCQ_CMD_ERR, 0, buf, 1, 0) == 0) {
		struct ata_ncq_err_info ni;

		if (ata_ncq_err_decode(buf, &ni) == 0)
			kprintf(SMART_PFX "%s: NCQ error log: %s tag %d status "
					  "0x%02x error 0x%02x LBA %llu count "
					  "%u%s\n",
				dev->name, ni.nq ? "non-queued," : "",
				ni.tag, ni.status, ni.error,
				(unsigned long long)ni.lba, ni.count,
				ni.checksum_ok ? "" : " (bad checksum)");
		else
			kprintf(SMART_PFX "%s: NCQ error log: empty\n",
				dev->name);
	}
	kfree(pages);
}

void ata_smart_dump_logs(struct ata_device *dev)
{
	uint8_t *data = NULL, *thr = NULL, *log = NULL;
	int failing = 0;

	if (smart_check(dev)) {
		if (dev)
			kprintf(SMART_PFX "%s: SMART not available\n",
				dev->name);
		return;
	}
	if (ata_can_sleep())
		might_sleep();

	data = kalloc(ATA_SECT_SIZE);
	thr = kalloc(ATA_SECT_SIZE);
	log = kalloc(ATA_SECT_SIZE);
	if (!data || !thr || !log)
		goto out;

	kprintf(SMART_PFX "%s: ---- SMART report: %s, serial %s, firmware %s "
			  "----\n",
		dev->name, dev->model, dev->serial, dev->firmware);
	kprintf(SMART_PFX "%s: SMART %s\n", dev->name,
		(dev->flags & ATA_DFLAG_SMART_ON) ? "enabled" : "disabled");
	if (!(dev->flags & ATA_DFLAG_SMART_ON))
		goto gpl;

	if (ata_smart_return_status(dev, &failing) == 0)
		kprintf(SMART_PFX "%s: overall health self-assessment: %s\n",
			dev->name, failing ? "FAILED" : "PASSED");

	if (ata_smart_read_data(dev, data) == 0) {
		int have_thr = ata_smart_read_thresholds(dev, thr) == 0;

		smart_print_capabilities(dev, data);
		smart_print_attrs(dev, data, have_thr ? thr : NULL);
	}

	/* SMART log directory (only meaningful with GPL or when the drive
	 * implements multi-sector SMART logs) */
	if (ata_smart_read_log(dev, ATA_LOG_DIRECTORY, log, 1) == 0)
		kprintf(SMART_PFX "%s: SMART log directory version %u; "
				  "summary %u, comprehensive %u, self-test %u, "
				  "selective %u pages\n",
			dev->name, sm_le16(log), sm_le16(log + 2 * 1),
			sm_le16(log + 2 * 2), sm_le16(log + 2 * 6),
			sm_le16(log + 2 * 9));

	if (ata_smart_read_log(dev, ATA_LOG_SUMMARY_ERR, log, 1) == 0)
		smart_print_summary_errlog(dev, log);
	if (ata_smart_read_log(dev, ATA_LOG_SELFTEST, log, 1) == 0)
		smart_print_selftest_log(dev, log);

gpl:
	smart_dump_gpl(dev, log);
	kprintf(SMART_PFX "%s: ---- end of SMART report ----\n", dev->name);
out:
	kfree(log);
	kfree(thr);
	kfree(data);
}
