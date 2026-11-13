// LikeOS - NVMe on-wire definitions: controller registers, queue entries, admin and I/O commands, identify and log page structures
//
// Everything an NVMe host driver exchanges with a controller, as the NVM
// Express Base Specification 2.x, the NVM Command Set Specification and
// the Zoned Namespace Command Set Specification define it:
//
//   - controller registers (BAR0 offsets and bit fields), doorbells
//   - PRP entries and SGL descriptors
//   - the 64-byte submission queue entry (struct nvme_command) with one
//     view per command family, and every admin and I/O opcode
//   - the 16-byte completion queue entry and every status code
//   - Identify data (controller, namespace, command-set specific, lists)
//   - log pages, feature identifiers and their dword encodings
//   - asynchronous events, reservations, directives, Flexible Data
//     Placement, ANA, zoned namespaces, protection information
//
// This file is a contract: it holds only definitions of the wire format
// (structures, constants, trivial helpers).  Driver policy (queue depths,
// timeouts, the controller page size the driver chooses) lives in the
// driver headers.  NVMe over Fabrics and NVMe-MI are not covered beyond
// opcode constants.
//
// Conventions:
//   - Every multi-byte field is little-endian on the wire and declared
//     __le16/__le32/__le64; read it with le*_to_cpu(), write it with
//     cpu_to_le*() (kernel/ke/byteorder.h).  128-bit counters are uint8_t[16].
//   - Byte offsets in comments are from the start of the structure.
//   - "0's based" means the field holds the value minus one.
//   - Structures are laid out so their natural layout already equals the
//     wire layout; __attribute__((packed)) is used only where the
//     specification places a field at a misaligned offset.  Every size and
//     the key offsets are checked by _nvme_spec_check() at the end.
//   - Command dword bit encodings that are ORed into a whole dword are
//     given already shifted (e.g. NVME_FWACT_ACTV, NVME_FEAT_SEL_SAVED).
//
// Status values: the status field of a completion entry (dword 3 bits 31:16)
// carries the phase tag in bit 0.  The driver stores the status shifted right
// by one (le16_to_cpu(cqe->status) >> 1), and every NVME_SC_* / NVME_SCT_* /
// NVME_STATUS_* constant below is in that shifted representation:
//   bits  7:0  SC   status code            (NVME_SC_MASK)
//   bits 10:8  SCT  status code type       (NVME_SCT_MASK, NVME_SCT())
//   bits 12:11 CRD  command retry delay    (NVME_STATUS_CRD)
//   bit  13    M    more                   (NVME_STATUS_MORE)
//   bit  14    DNR  do not retry           (NVME_STATUS_DNR)
// so NVME_SC_ZONE_FULL (0x1b9) is SCT 1, SC 0xb9.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_NVME_NVME_SPEC_H
#define KERNEL_DEV_NVME_NVME_SPEC_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/byteorder.h>

/* ==========================================================================
 * General constants
 * ========================================================================== */

#define NVME_SQE_SIZE		64	/* submission queue entry, bytes */
#define NVME_CQE_SIZE		16	/* completion queue entry, bytes */

/* Queue entry sizes of the NVM command set (CC.IOSQES/IOCQES, SQES/CQES:
 * log2 of the size in bytes). */
#define NVME_ADM_SQES		6
#define NVME_NVM_IOSQES		6
#define NVME_NVM_IOCQES		4

#define NVME_MAX_QUEUE_ENTRIES	65536	/* I/O queue: CAP.MQES max + 1 */
#define NVME_MAX_ADMIN_ENTRIES	4096	/* admin queue: AQA.ASQS/ACQS max + 1 */
#define NVME_MIN_QUEUE_ENTRIES	2	/* any queue: size 1 is invalid */
#define NVME_MAX_QID		65535	/* queue identifiers 0..65535 */

#define NVME_NSID_NONE		0x00000000	/* command not namespace-bound */
#define NVME_NSID_ALL		0xffffffff	/* broadcast: all namespaces */

/* Controller identifiers (CNTLID). */
#define NVME_CNTLID_MIN		1
#define NVME_CNTLID_MAX		0xffef
#define NVME_CNTLID_DYNAMIC	0xffff

/* Special NSSR / NSSD value, the ASCII string 'NVMe'. */
#define NVME_SUBSYS_RESET	0x4E564D65
#define NVME_SUBSYS_SHUTDOWN	0x4E564D65

/* NVMe Qualified Names: the field is 256 bytes, a name at most 223 bytes. */
#define NVMF_NQN_FIELD_LEN	256
#define NVMF_NQN_SIZE		223
/* The well-known NQN of a discovery subsystem (never matched against an
 * I/O subsystem). */
#define NVME_DISC_SUBSYS_NAME	"nqn.2014-08.org.nvmexpress.discovery"

#define NVME_IDENTIFY_DATA_SIZE	4096	/* every Identify data structure */
#define NVME_LOG_PAGE_DWORD	4	/* NUMD counts dwords */
#define NVME_TELEMETRY_BLOCK_SIZE 512	/* telemetry data area block */

#define NVME_MAX_CHANGED_NAMESPACES 1024
#define NVME_DSM_MAX_RANGES	256
#define NVME_MAX_PSD		32	/* power state descriptors */
#define NVME_MAX_LBAF		64	/* LBA formats in Identify Namespace */
#define NVME_MAX_UUID_ENTRIES	127	/* Identify UUID list */

/* Identifier lengths (namespace identification descriptors). */
#define NVME_NIDT_EUI64_LEN	8
#define NVME_NIDT_NGUID_LEN	16
#define NVME_NIDT_UUID_LEN	16
#define NVME_NIDT_CSI_LEN	1

/* Units of Identify Controller fields:
 *   RTD3R/RTD3E          microseconds
 *   CRDT1..3             100 milliseconds
 *   KAS                  100 milliseconds (keep-alive granularity)
 *   CAP.TO, CRTO fields  500 milliseconds
 *   HMPRE/HMMIN/HMMINDS  4 KiB
 *   EDSTT                minutes
 *   WCTEMP/CCTEMP/MNTMT/MXTMT and every temperature: Kelvin
 *   MTFA                 100 milliseconds
 *   MDTS                 2^MDTS units of CAP.MPSMIN pages, 0 = no limit
 *   TNVMCAP/UNVMCAP      bytes, 128-bit
 */
#define NVME_KAS_UNIT_MS	100
#define NVME_CRDT_UNIT_MS	100
#define NVME_CAP_TO_UNIT_MS	500
#define NVME_HMB_UNIT_SIZE	4096
#define NVME_MTFA_UNIT_MS	100

/* ==========================================================================
 * Controller registers (BAR0)
 * ========================================================================== */

enum {
	NVME_REG_CAP	= 0x0000,	/* Controller Capabilities (64) */
	NVME_REG_VS	= 0x0008,	/* Version */
	NVME_REG_INTMS	= 0x000c,	/* Interrupt Mask Set */
	NVME_REG_INTMC	= 0x0010,	/* Interrupt Mask Clear */
	NVME_REG_CC	= 0x0014,	/* Controller Configuration */
	NVME_REG_CSTS	= 0x001c,	/* Controller Status */
	NVME_REG_NSSR	= 0x0020,	/* NVM Subsystem Reset */
	NVME_REG_AQA	= 0x0024,	/* Admin Queue Attributes */
	NVME_REG_ASQ	= 0x0028,	/* Admin SQ Base Address (64) */
	NVME_REG_ACQ	= 0x0030,	/* Admin CQ Base Address (64) */
	NVME_REG_CMBLOC	= 0x0038,	/* Controller Memory Buffer Location */
	NVME_REG_CMBSZ	= 0x003c,	/* Controller Memory Buffer Size */
	NVME_REG_BPINFO	= 0x0040,	/* Boot Partition Information */
	NVME_REG_BPRSEL	= 0x0044,	/* Boot Partition Read Select */
	NVME_REG_BPMBL	= 0x0048,	/* Boot Partition Memory Buffer
					 * Location (64) */
	NVME_REG_CMBMSC	= 0x0050,	/* CMB Memory Space Control (64) */
	NVME_REG_CMBSTS	= 0x0058,	/* CMB Status */
	NVME_REG_CMBEBS	= 0x005c,	/* CMB Elasticity Buffer Size */
	NVME_REG_CMBSWTP = 0x0060,	/* CMB Sustained Write Throughput */
	NVME_REG_NSSD	= 0x0064,	/* NVM Subsystem Shutdown */
	NVME_REG_CRTO	= 0x0068,	/* Controller Ready Timeouts */
	NVME_REG_PMRCAP	= 0x0e00,	/* Persistent Memory Capabilities */
	NVME_REG_PMRCTL	= 0x0e04,	/* Persistent Memory Region Control */
	NVME_REG_PMRSTS	= 0x0e08,	/* Persistent Memory Region Status */
	NVME_REG_PMREBS	= 0x0e0c,	/* PMR Elasticity Buffer Size */
	NVME_REG_PMRSWTP = 0x0e10,	/* PMR Sustained Write Throughput */
	NVME_REG_PMRMSCL = 0x0e14,	/* PMR Memory Space Control Lower */
	NVME_REG_PMRMSCU = 0x0e18,	/* PMR Memory Space Control Upper */
	NVME_REG_DBS	= 0x1000,	/* SQ 0 Tail Doorbell */
};

/* ---- CAP: Controller Capabilities (64-bit) ------------------------------- */

#define NVME_CAP_MQES(cap)	((cap) & 0xffff)	/* 0's based */
#define NVME_CAP_CQR(cap)	(((cap) >> 16) & 0x1)	/* contiguous queues */
#define NVME_CAP_AMS(cap)	(((cap) >> 17) & 0x3)
#define NVME_CAP_TIMEOUT(cap)	(((cap) >> 24) & 0xff)	/* 500 ms units */
#define NVME_CAP_STRIDE(cap)	(((cap) >> 32) & 0xf)	/* DSTRD */
#define NVME_CAP_NSSRC(cap)	(((cap) >> 36) & 0x1)	/* NSSRS */
#define NVME_CAP_CSS(cap)	(((cap) >> 37) & 0xff)
#define NVME_CAP_BPS(cap)	(((cap) >> 45) & 0x1)	/* boot partitions */
#define NVME_CAP_CPS(cap)	(((cap) >> 46) & 0x3)	/* power scope */
#define NVME_CAP_MPSMIN(cap)	(((cap) >> 48) & 0xf)	/* 2^(12+n) */
#define NVME_CAP_MPSMAX(cap)	(((cap) >> 52) & 0xf)	/* 2^(12+n) */
#define NVME_CAP_PMRS(cap)	(((cap) >> 56) & 0x1)	/* PMR supported */
#define NVME_CAP_CMBS(cap)	(((cap) >> 57) & 0x1)	/* CMB supported */
#define NVME_CAP_NSSS(cap)	(((cap) >> 58) & 0x1)	/* NSSD supported */
#define NVME_CAP_CRMS(cap)	(((cap) >> 59) & 0x3)	/* ready modes */
#define NVME_CAP_NSSES(cap)	(((cap) >> 61) & 0x1)	/* shutdown enh. */

/* CAP.AMS (after NVME_CAP_AMS()) */
enum {
	NVME_CAP_AMS_WRRUMC	= 1 << 0,	/* weighted round robin + urgent */
	NVME_CAP_AMS_VS		= 1 << 1,	/* vendor specific */
};

/* CAP.CSS (after NVME_CAP_CSS()) */
enum {
	NVME_CAP_CSS_NVM	= 1 << 0,	/* NVM command set */
	NVME_CAP_CSS_CSI	= 1 << 6,	/* one or more I/O command sets */
	NVME_CAP_CSS_NOIO	= 1 << 7,	/* no I/O command set (admin) */
};

/* CAP.CPS (after NVME_CAP_CPS()) */
enum {
	NVME_CAP_CPS_NOT_REPORTED = 0,
	NVME_CAP_CPS_CTRL	= 1,
	NVME_CAP_CPS_DOMAIN	= 2,
	NVME_CAP_CPS_SUBSYSTEM	= 3,
};

/* CAP.CRMS, unshifted 64-bit masks */
#define NVME_CAP_CRMS_CRWMS	(1ULL << 59)	/* ready with media */
#define NVME_CAP_CRMS_CRIMS	(1ULL << 60)	/* ready independent of media */

/* ---- VS: Version --------------------------------------------------------- */

#define NVME_VS(major, minor, tertiary) \
	(((major) << 16) | ((minor) << 8) | (tertiary))

#define NVME_MAJOR(ver)		((ver) >> 16)
#define NVME_MINOR(ver)		(((ver) >> 8) & 0xff)
#define NVME_TERTIARY(ver)	((ver) & 0xff)

#define NVME_VS_1_0	NVME_VS(1, 0, 0)
#define NVME_VS_1_1	NVME_VS(1, 1, 0)
#define NVME_VS_1_2	NVME_VS(1, 2, 0)
#define NVME_VS_1_2_1	NVME_VS(1, 2, 1)
#define NVME_VS_1_3	NVME_VS(1, 3, 0)
#define NVME_VS_1_4	NVME_VS(1, 4, 0)
#define NVME_VS_2_0	NVME_VS(2, 0, 0)
#define NVME_VS_2_1	NVME_VS(2, 1, 0)

/* ---- CC: Controller Configuration ---------------------------------------- */

enum {
	/* Enable (EN): bit 0 */
	NVME_CC_ENABLE		= 1 << 0,
	NVME_CC_EN_SHIFT	= 0,

	/* I/O Command Set Selected (CSS): bits 06:04 */
	NVME_CC_CSS_SHIFT	= 4,
	NVME_CC_CSS_MASK	= 7 << NVME_CC_CSS_SHIFT,
	NVME_CC_CSS_NVM		= 0 << NVME_CC_CSS_SHIFT,
	NVME_CC_CSS_CSI		= 6 << NVME_CC_CSS_SHIFT,
	NVME_CC_CSS_ADMIN	= 7 << NVME_CC_CSS_SHIFT, /* admin only */

	/* Memory Page Size (MPS): bits 10:07, page = 2^(12 + MPS) */
	NVME_CC_MPS_SHIFT	= 7,
	NVME_CC_MPS_MASK	= 0xf << NVME_CC_MPS_SHIFT,

	/* Arbitration Mechanism Selected (AMS): bits 13:11 */
	NVME_CC_AMS_SHIFT	= 11,
	NVME_CC_AMS_MASK	= 7 << NVME_CC_AMS_SHIFT,
	NVME_CC_AMS_RR		= 0 << NVME_CC_AMS_SHIFT,
	NVME_CC_AMS_WRRU	= 1 << NVME_CC_AMS_SHIFT,
	NVME_CC_AMS_VS		= 7 << NVME_CC_AMS_SHIFT,

	/* Shutdown Notification (SHN): bits 15:14 */
	NVME_CC_SHN_SHIFT	= 14,
	NVME_CC_SHN_MASK	= 3 << NVME_CC_SHN_SHIFT,
	NVME_CC_SHN_NONE	= 0 << NVME_CC_SHN_SHIFT,
	NVME_CC_SHN_NORMAL	= 1 << NVME_CC_SHN_SHIFT,
	NVME_CC_SHN_ABRUPT	= 2 << NVME_CC_SHN_SHIFT,

	/* I/O Submission Queue Entry Size (IOSQES): bits 19:16 */
	NVME_CC_IOSQES_SHIFT	= 16,
	NVME_CC_IOSQES_MASK	= 0xf << NVME_CC_IOSQES_SHIFT,
	NVME_CC_IOSQES		= NVME_NVM_IOSQES << NVME_CC_IOSQES_SHIFT,

	/* I/O Completion Queue Entry Size (IOCQES): bits 23:20 */
	NVME_CC_IOCQES_SHIFT	= 20,
	NVME_CC_IOCQES_MASK	= 0xf << NVME_CC_IOCQES_SHIFT,
	NVME_CC_IOCQES		= NVME_NVM_IOCQES << NVME_CC_IOCQES_SHIFT,

	/* Controller Ready Independent of Media Enable (CRIME): bit 24 */
	NVME_CC_CRIME		= 1 << 24,
};

/* ---- CSTS: Controller Status --------------------------------------------- */

enum {
	NVME_CSTS_RDY		= 1 << 0,	/* ready */
	NVME_CSTS_CFS		= 1 << 1,	/* controller fatal status */
	NVME_CSTS_SHST_SHIFT	= 2,
	NVME_CSTS_SHST_NORMAL	= 0 << 2,	/* normal operation */
	NVME_CSTS_SHST_OCCUR	= 1 << 2,	/* shutdown in progress */
	NVME_CSTS_SHST_CMPLT	= 2 << 2,	/* shutdown complete */
	NVME_CSTS_SHST_MASK	= 3 << 2,
	NVME_CSTS_NSSRO		= 1 << 4,	/* NVM subsystem reset occurred */
	NVME_CSTS_PP		= 1 << 5,	/* processing paused */
	NVME_CSTS_ST		= 1 << 6,	/* shutdown type: subsystem */
};

/* ---- AQA: Admin Queue Attributes ----------------------------------------- */

#define NVME_AQA_ASQS_MASK	0xfff		/* bits 11:0, 0's based */
#define NVME_AQA_ACQS_SHIFT	16		/* bits 27:16, 0's based */
#define NVME_AQA_ACQS_MASK	(0xfffu << NVME_AQA_ACQS_SHIFT)
#define NVME_AQA(sq_entries, cq_entries) \
	((((uint32_t)(sq_entries) - 1) & NVME_AQA_ASQS_MASK) | \
	 ((((uint32_t)(cq_entries) - 1) & 0xfff) << NVME_AQA_ACQS_SHIFT))

/* ASQ/ACQ: bits 63:12 base address, 4 KiB aligned. */
#define NVME_ASQ_ACQ_ALIGN	4096

/* ---- CMBLOC / CMBSZ: Controller Memory Buffer ---------------------------- */

#define NVME_CMB_BIR(cmbloc)	((cmbloc) & 0x7)
#define NVME_CMB_OFST(cmbloc)	(((cmbloc) >> 12) & 0xfffff) /* SZU units */

enum {
	NVME_CMBLOC_CQMMS	= 1 << 3,	/* queue mixed memory support */
	NVME_CMBLOC_CQPDS	= 1 << 4,	/* queue physically discontig. */
	NVME_CMBLOC_CDPMLS	= 1 << 5,	/* data pointer mixed locations */
	NVME_CMBLOC_CDPCILS	= 1 << 6,	/* data ptr and cmd independent */
	NVME_CMBLOC_CDMMMS	= 1 << 7,	/* data and metadata mixed */
	NVME_CMBLOC_CQDA	= 1 << 8,	/* queue dword alignment */
	NVME_CMBLOC_OFST_SHIFT	= 12,
};

enum {
	NVME_CMBSZ_SQS		= 1 << 0,	/* submission queues */
	NVME_CMBSZ_CQS		= 1 << 1,	/* completion queues */
	NVME_CMBSZ_LISTS	= 1 << 2,	/* PRP/SGL lists */
	NVME_CMBSZ_RDS		= 1 << 3,	/* read data */
	NVME_CMBSZ_WDS		= 1 << 4,	/* write data */

	NVME_CMBSZ_SZ_SHIFT	= 12,
	NVME_CMBSZ_SZ_MASK	= 0xfffff,

	NVME_CMBSZ_SZU_SHIFT	= 8,
	NVME_CMBSZ_SZU_MASK	= 0xf,
};

/* CMBSZ.SZU values: 4 KiB * 16^SZU */
enum {
	NVME_CMBSZ_SZU_4K	= 0,
	NVME_CMBSZ_SZU_64K	= 1,
	NVME_CMBSZ_SZU_1M	= 2,
	NVME_CMBSZ_SZU_16M	= 3,
	NVME_CMBSZ_SZU_256M	= 4,
	NVME_CMBSZ_SZU_4G	= 5,
	NVME_CMBSZ_SZU_64G	= 6,
};

#define NVME_CMBSZ_SZU(cmbsz) \
	(((cmbsz) >> NVME_CMBSZ_SZU_SHIFT) & NVME_CMBSZ_SZU_MASK)
#define NVME_CMBSZ_SZ(cmbsz) \
	(((cmbsz) >> NVME_CMBSZ_SZ_SHIFT) & NVME_CMBSZ_SZ_MASK)

/* Bytes in one CMBSZ.SZU (also PMREBS/CMBEBS/...SZU) unit. */
static inline uint64_t nvme_cmbsz_unit_bytes(uint32_t szu)
{
	return 1ULL << (12 + 4 * (szu & 0xf));
}

/* CMB size in bytes from the raw CMBSZ register. */
static inline uint64_t nvme_cmbsz_bytes(uint32_t cmbsz)
{
	return (uint64_t)NVME_CMBSZ_SZ(cmbsz) *
	       nvme_cmbsz_unit_bytes(NVME_CMBSZ_SZU(cmbsz));
}

/* ---- CMBMSC / CMBSTS / CMBEBS / CMBSWTP ---------------------------------- */

enum {
	NVME_CMBMSC_CRE		= 1 << 0,	/* capabilities registers enabled */
	NVME_CMBMSC_CMSE	= 1 << 1,	/* memory space enable */
};
#define NVME_CMBMSC_CBA_SHIFT	12
#define NVME_CMBMSC_CBA_MASK	(~0xfffULL)	/* bits 63:12 controller base */

enum {
	NVME_CMBSTS_CBAI	= 1 << 0,	/* controller base address invalid */
};

#define NVME_CMBEBS_CMBSZU(v)	((v) & 0xf)
#define NVME_CMBEBS_CMBRBB	(1u << 4)	/* read bypass behavior */
#define NVME_CMBEBS_CMBWBZ(v)	(((v) >> 8) & 0xffffff)

#define NVME_CMBSWTP_CMBSWTU(v)	((v) & 0xf)	/* 0 bytes/s, 1 KiB/s ... */
#define NVME_CMBSWTP_CMBSWTV(v)	(((v) >> 8) & 0xffffff)

/* ---- BPINFO / BPRSEL / BPMBL: Boot Partitions ---------------------------- */

#define NVME_BPINFO_BPSZ(v)	((v) & 0x7fff)		/* 128 KiB units */
#define NVME_BPINFO_BRS(v)	(((v) >> 24) & 0x3)
#define NVME_BPINFO_ABPID(v)	(((v) >> 31) & 0x1)	/* active partition */
#define NVME_BP_UNIT_SIZE	(128 * 1024)

enum {
	NVME_BPINFO_BRS_NONE	= 0,	/* no read operation requested */
	NVME_BPINFO_BRS_READING	= 1,	/* read in progress */
	NVME_BPINFO_BRS_SUCCESS	= 2,	/* read completed successfully */
	NVME_BPINFO_BRS_ERROR	= 3,	/* error completing the read */
};

#define NVME_BPRSEL_BPRSZ_SHIFT	0	/* bits 9:0 read size, 4 KiB units */
#define NVME_BPRSEL_BPRSZ_MASK	0x3ffu
#define NVME_BPRSEL_BPROF_SHIFT	10	/* bits 29:10 offset, 4 KiB units */
#define NVME_BPRSEL_BPROF_MASK	(0xfffffu << NVME_BPRSEL_BPROF_SHIFT)
#define NVME_BPRSEL_BPID	(1u << 31)	/* boot partition 1 */
#define NVME_BPRSEL(bpid, ofst_4k, sz_4k) \
	(((uint32_t)(sz_4k) & NVME_BPRSEL_BPRSZ_MASK) | \
	 (((uint32_t)(ofst_4k) << NVME_BPRSEL_BPROF_SHIFT) & \
	  NVME_BPRSEL_BPROF_MASK) | ((bpid) ? NVME_BPRSEL_BPID : 0))

#define NVME_BPMBL_BMBBA_MASK	(~0xfffULL)	/* 4 KiB aligned buffer */

/* ---- CRTO: Controller Ready Timeouts (500 ms units) ---------------------- */

#define NVME_CRTO_CRIMT(crto)	((crto) >> 16)		/* independent of media */
#define NVME_CRTO_CRWMT(crto)	((crto) & 0xffff)	/* with media */

/* ---- PMR: Persistent Memory Region --------------------------------------- */

#define NVME_PMRCAP_RDS		(1u << 3)	/* read data support */
#define NVME_PMRCAP_WDS		(1u << 4)	/* write data support */
#define NVME_PMRCAP_BIR(v)	(((v) >> 5) & 0x7)
#define NVME_PMRCAP_PMRTU(v)	(((v) >> 8) & 0x3)	/* timeout units */
#define NVME_PMRCAP_PMRWBM(v)	(((v) >> 10) & 0xf)	/* write barrier */
#define NVME_PMRCAP_PMRTO(v)	(((v) >> 16) & 0xff)	/* timeout */
#define NVME_PMRCAP_CMSS	(1u << 24)	/* memory space control */

enum {
	NVME_PMRCAP_PMRTU_500MS	= 0,
	NVME_PMRCAP_PMRTU_MIN	= 1,	/* minutes */
};

#define NVME_PMRCTL_EN		(1u << 0)

#define NVME_PMRSTS_ERR(v)	((v) & 0xff)
#define NVME_PMRSTS_NRDY	(1u << 8)	/* not ready */
#define NVME_PMRSTS_HSTS(v)	(((v) >> 9) & 0x7)	/* health status */
#define NVME_PMRSTS_CBAI	(1u << 12)	/* base address invalid */

enum {
	NVME_PMRSTS_HSTS_NORMAL		= 0,
	NVME_PMRSTS_HSTS_RESTORE_ERR	= 1,
	NVME_PMRSTS_HSTS_READ_ONLY	= 2,
	NVME_PMRSTS_HSTS_UNRELIABLE	= 3,
};

#define NVME_PMREBS_PMRSZU(v)	((v) & 0xf)
#define NVME_PMREBS_RBB		(1u << 4)	/* read bypass behavior */
#define NVME_PMREBS_PMRWBZ(v)	(((v) >> 8) & 0xffffff)

#define NVME_PMRSWTP_PMRSWTU(v)	((v) & 0xf)
#define NVME_PMRSWTP_PMRSWTV(v)	(((v) >> 8) & 0xffffff)

#define NVME_PMRMSCL_CMSE	(1u << 1)	/* memory space enable */
#define NVME_PMRMSCL_CBA_MASK	0xfffff000u	/* base bits 31:12 */
/* PMRMSCU holds base address bits 63:32. */

/* ---- Doorbells ------------------------------------------------------------ */
/* SQ y Tail Doorbell at 0x1000 + (2y) * (4 << CAP.DSTRD),
 * CQ y Head Doorbell at 0x1000 + (2y + 1) * (4 << CAP.DSTRD).
 * Both are 32-bit registers; bits 15:0 hold the tail / head index. */

#define NVME_DB_STRIDE_BYTES(dstrd)	(4u << (dstrd))
#define NVME_SQ_TAIL_DB(qid, dstrd) \
	(NVME_REG_DBS + (2u * (qid)) * NVME_DB_STRIDE_BYTES(dstrd))
#define NVME_CQ_HEAD_DB(qid, dstrd) \
	(NVME_REG_DBS + (2u * (qid) + 1u) * NVME_DB_STRIDE_BYTES(dstrd))

/* Doorbell index in units of 32-bit words from NVME_REG_DBS, given the
 * stride in words (1 << CAP.DSTRD), as a driver indexing a u32 array. */
#define NVME_SQ_DB_IDX(qid, db_stride)	(2u * (qid) * (db_stride))
#define NVME_CQ_DB_IDX(qid, db_stride)	((2u * (qid) + 1u) * (db_stride))

/* ==========================================================================
 * Data pointers: PRP entries and SGL descriptors
 * ========================================================================== */

/* PRP (Physical Region Page) entry: a 64-bit physical address, dword
 * aligned (bits 1:0 zero).  PRP1 may carry an offset into a memory page;
 * every later entry must be page aligned.  If a transfer spans exactly two
 * pages PRP2 is the second page; if more, PRP2 points to a PRP list: a
 * page of PRP entries whose last entry points to the next list page when
 * more entries follow.  The memory page size is CC.MPS. */
#define NVME_PRP_ENTRY_SIZE	8
#define NVME_PRP_ALIGN_MASK	0x3ULL

/* SGL descriptor subtype - lower 4 bits of the identifier byte. */
enum {
	NVME_SGL_FMT_ADDRESS		= 0x00,	/* address */
	NVME_SGL_FMT_OFFSET		= 0x01,	/* offset (in capsule) */
	NVME_SGL_FMT_TRANSPORT_A	= 0x0A,	/* transport specific */
	NVME_SGL_FMT_INVALIDATE		= 0x0f,	/* remote invalidation */
};

/* SGL descriptor type - upper 4 bits of the identifier byte. */
enum {
	NVME_SGL_FMT_DATA_DESC		= 0x00,	/* data block */
	NVME_SGL_FMT_BIT_BUCKET_DESC	= 0x01,	/* bit bucket */
	NVME_SGL_FMT_SEG_DESC		= 0x02,	/* segment */
	NVME_SGL_FMT_LAST_SEG_DESC	= 0x03,	/* last segment */
	NVME_KEY_SGL_FMT_DATA_DESC	= 0x04,	/* keyed data block */
	NVME_TRANSPORT_SGL_DATA_DESC	= 0x05,	/* transport data block */
	NVME_SGL_FMT_VENDOR_DESC	= 0x0f,	/* vendor specific */
};

/* Identifier byte from a type and a subtype. */
#define NVME_SGL_ID(type, subtype) \
	((uint8_t)((((type) & 0xf) << 4) | ((subtype) & 0xf)))
#define NVME_SGL_TYPE(id)	(((id) >> 4) & 0xf)
#define NVME_SGL_SUBTYPE(id)	((id) & 0xf)

#define NVME_SGL_DESC_SIZE	16

/* Data block / bit bucket / segment / last segment descriptor. */
struct nvme_sgl_desc {
	__le64	addr;		/* 0: address (or offset) */
	__le32	length;		/* 8: length in bytes */
	uint8_t	rsvd[3];	/* 12 */
	uint8_t	type;		/* 15: SGL identifier */
};

/* Keyed data block descriptor. */
struct nvme_keyed_sgl_desc {
	__le64	addr;		/* 0 */
	uint8_t	length[3];	/* 8: 24-bit length */
	uint8_t	key[4];		/* 11: 32-bit key */
	uint8_t	type;		/* 15: SGL identifier */
};

union nvme_data_ptr {
	struct {
		__le64	prp1;
		__le64	prp2;
	};
	struct nvme_sgl_desc	sgl;
	struct nvme_keyed_sgl_desc ksgl;
};

/* ==========================================================================
 * Opcodes
 * ========================================================================== */

/* Admin command set. */
enum nvme_admin_opcode {
	nvme_admin_delete_sq		= 0x00,
	nvme_admin_create_sq		= 0x01,
	nvme_admin_get_log_page		= 0x02,
	nvme_admin_delete_cq		= 0x04,
	nvme_admin_create_cq		= 0x05,
	nvme_admin_identify		= 0x06,
	nvme_admin_abort_cmd		= 0x08,
	nvme_admin_set_features		= 0x09,
	nvme_admin_get_features		= 0x0a,
	nvme_admin_async_event		= 0x0c,
	nvme_admin_ns_mgmt		= 0x0d,
	nvme_admin_activate_fw		= 0x10,	/* Firmware Commit */
	nvme_admin_download_fw		= 0x11,	/* Firmware Image Download */
	nvme_admin_dev_self_test	= 0x14,
	nvme_admin_ns_attach		= 0x15,
	nvme_admin_keep_alive		= 0x18,
	nvme_admin_directive_send	= 0x19,
	nvme_admin_directive_recv	= 0x1a,
	nvme_admin_virtual_mgmt		= 0x1c,
	nvme_admin_nvme_mi_send		= 0x1d,
	nvme_admin_nvme_mi_recv		= 0x1e,
	nvme_admin_capacity_mgmt	= 0x20,
	nvme_admin_lockdown		= 0x24,
	nvme_admin_dbbuf		= 0x7C,	/* Doorbell Buffer Config */
	nvme_admin_fabrics		= 0x7F,	/* Fabrics commands (unused) */
	nvme_admin_format_nvm		= 0x80,
	nvme_admin_security_send	= 0x81,
	nvme_admin_security_recv	= 0x82,
	nvme_admin_sanitize_nvm		= 0x84,
	nvme_admin_get_lba_status	= 0x86,
	nvme_admin_vendor_start		= 0xC0,
};

/* NVMe over Fabrics command opcode (both queues). */
enum nvmf_fabrics_opcode {
	nvme_fabrics_command		= 0x7f,
};

/* I/O commands: NVM command set and Zoned Namespace command set. */
enum nvme_opcode {
	nvme_cmd_flush		= 0x00,
	nvme_cmd_write		= 0x01,
	nvme_cmd_read		= 0x02,
	nvme_cmd_write_uncor	= 0x04,
	nvme_cmd_compare	= 0x05,
	nvme_cmd_write_zeroes	= 0x08,
	nvme_cmd_dsm		= 0x09,
	nvme_cmd_verify		= 0x0c,
	nvme_cmd_resv_register	= 0x0d,
	nvme_cmd_resv_report	= 0x0e,
	nvme_cmd_resv_acquire	= 0x11,
	nvme_cmd_io_mgmt_recv	= 0x12,
	nvme_cmd_resv_release	= 0x15,
	nvme_cmd_cancel		= 0x18,	/* NVMe 2.1 */
	nvme_cmd_copy		= 0x19,
	nvme_cmd_io_mgmt_send	= 0x1d,
	nvme_cmd_zone_mgmt_send	= 0x79,
	nvme_cmd_zone_mgmt_recv	= 0x7a,
	nvme_cmd_zone_append	= 0x7d,
	nvme_cmd_vendor_start	= 0x80,
};

/* Data transfer direction is encoded in opcode bits 1:0:
 * 00b none, 01b host to controller, 10b controller to host, 11b both. */
#define NVME_OPCODE_XFER(op)		((op) & 0x3)
enum {
	NVME_OPCODE_XFER_NONE		= 0,
	NVME_OPCODE_XFER_H2C		= 1,
	NVME_OPCODE_XFER_C2H		= 2,
	NVME_OPCODE_XFER_BIDI		= 3,
};

/* ==========================================================================
 * Submission queue entry: command dword 0 and the common layout
 * ========================================================================== */

/*
 * The flags byte (command dword 0 bits 15:8):
 *   bits 1:0  FUSE: fused operation
 *   bits 7:6  PSDT: PRP or SGL for data transfer
 *     00b PRPs; 01b SGLs, MPTR is a single contiguous metadata buffer
 *     (byte aligned); 10b SGLs, MPTR points to an SGL segment holding
 *     exactly one SGL descriptor (qword aligned).
 */
enum {
	NVME_CMD_FUSE_FIRST	= (1 << 0),
	NVME_CMD_FUSE_SECOND	= (1 << 1),

	NVME_CMD_SGL_METABUF	= (1 << 6),
	NVME_CMD_SGL_METASEG	= (1 << 7),
	NVME_CMD_SGL_ALL	= NVME_CMD_SGL_METABUF | NVME_CMD_SGL_METASEG,
};
#define NVME_CMD_FUSE_MASK	0x03
#define NVME_CMD_PSDT_MASK	0xc0

struct nvme_common_command {
	uint8_t			opcode;		/* 0 */
	uint8_t			flags;		/* 1: FUSE, PSDT */
	uint16_t		command_id;	/* 2: CID, opaque to device */
	__le32			nsid;		/* 4 */
	__le32			cdw2[2];	/* 8 */
	__le64			metadata;	/* 16: MPTR */
	union nvme_data_ptr	dptr;		/* 24: DPTR */
	union {
		struct {
			__le32	cdw10;		/* 40 */
			__le32	cdw11;		/* 44 */
			__le32	cdw12;		/* 48 */
			__le32	cdw13;		/* 52 */
			__le32	cdw14;		/* 56 */
			__le32	cdw15;		/* 60 */
		};
		struct {
			__le32	cdw10;
			__le32	cdw11;
			__le32	cdw12;
			__le32	cdw13;
			__le32	cdw14;
			__le32	cdw15;
		} cdws;
	};
};

/* ==========================================================================
 * NVM I/O: Read, Write, Compare, Verify, Write Uncorrectable, Zone Append
 * ========================================================================== */

/*
 * Read (0x02), Write (0x01), Compare (0x05), Verify (0x0c), Write
 * Uncorrectable (0x04) and Zone Append (0x7d) share this layout.
 *   cdw2/cdw3  storage tag and the upper bits of the expected initial
 *              reference tag for the 32b/64b guard PI formats
 *   cdw10-11   SLBA (Zone Append: ZSLBA)
 *   cdw12      bits 15:0 NLB (0's based) | bits 31:16 control
 *   cdw13      bits 7:0 DSM | bits 31:16 DSPEC (directive specific)
 *   cdw14      ILBRT / EILBRT low 32 bits (initial reference tag)
 *   cdw15      bits 15:0 LBAT | bits 31:16 LBATM (application tag/mask)
 */
struct nvme_rw_command {
	uint8_t			opcode;		/* 0 */
	uint8_t			flags;		/* 1 */
	uint16_t		command_id;	/* 2 */
	__le32			nsid;		/* 4 */
	__le32			cdw2;		/* 8 */
	__le32			cdw3;		/* 12 */
	__le64			metadata;	/* 16 */
	union nvme_data_ptr	dptr;		/* 24 */
	__le64			slba;		/* 40 */
	__le16			length;		/* 48: NLB, 0's based */
	__le16			control;	/* 50: cdw12 bits 31:16 */
	__le32			dsmgmt;		/* 52: cdw13 */
	__le32			reftag;		/* 56: cdw14 */
	__le16			lbat;		/* 60 */
	__le16			lbatm;		/* 62 */
};

/* rw.control (cdw12 bits 31:16) and rw.dsmgmt (cdw13). */
enum {
	NVME_RW_LR			= 1 << 15,	/* limited retry */
	NVME_RW_FUA			= 1 << 14,	/* force unit access */
	NVME_RW_PRINFO_PRACT		= 1 << 13,	/* PI action */
	NVME_RW_PRINFO_PRCHK_GUARD	= 1 << 12,	/* check guard */
	NVME_RW_PRINFO_PRCHK_APP	= 1 << 11,	/* check app tag */
	NVME_RW_PRINFO_PRCHK_REF	= 1 << 10,	/* check ref tag */
	NVME_RW_APPEND_PIREMAP		= 1 << 9,	/* zone append */
	NVME_RW_STC			= 1 << 8,	/* storage tag check */
	NVME_RW_DTYPE_STREAMS		= 1 << 4,	/* directive type 1 */
	NVME_RW_DTYPE_DPLCMT		= 2 << 4,	/* directive type 2 */
	NVME_RW_DSM_FREQ_UNSPEC		= 0,
	NVME_RW_DSM_FREQ_TYPICAL	= 1,
	NVME_RW_DSM_FREQ_RARE		= 2,
	NVME_RW_DSM_FREQ_READS		= 3,
	NVME_RW_DSM_FREQ_WRITES		= 4,
	NVME_RW_DSM_FREQ_RW		= 5,
	NVME_RW_DSM_FREQ_ONCE		= 6,
	NVME_RW_DSM_FREQ_PREFETCH	= 7,
	NVME_RW_DSM_FREQ_TEMP		= 8,
	NVME_RW_DSM_LATENCY_NONE	= 0 << 4,
	NVME_RW_DSM_LATENCY_IDLE	= 1 << 4,
	NVME_RW_DSM_LATENCY_NORM	= 2 << 4,
	NVME_RW_DSM_LATENCY_LOW		= 3 << 4,
	NVME_RW_DSM_SEQ_REQ		= 1 << 6,
	NVME_RW_DSM_COMPRESSED		= 1 << 7,
	NVME_WZ_DEAC			= 1 << 9,	/* write zeroes */
	NVME_WZ_NSZ			= 1 << 7,	/* write zeroes, 2.1 */
};
#define NVME_RW_PRINFO_MASK	(0xf << 10)
#define NVME_RW_DTYPE_SHIFT	4
#define NVME_RW_DTYPE_MASK	(0xf << 4)
#define NVME_RW_DSM_FREQ_MASK	0xf
#define NVME_RW_DSM_LATENCY_MASK (3 << 4)
#define NVME_RW_DSPEC_SHIFT	16		/* in dsmgmt (cdw13) */
#define NVME_RW_DSPEC_MASK	0xffff0000u

/* ==========================================================================
 * NVM I/O: Dataset Management, Write Zeroes, Copy, Cancel
 * ========================================================================== */

struct nvme_dsm_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24: range list */
	__le32			nr;		/* 40: ranges, 0's based (7:0) */
	__le32			attributes;	/* 44: NVME_DSMGMT_* */
	uint32_t		rsvd12[4];
};

enum {
	NVME_DSMGMT_IDR		= 1 << 0,	/* integral dataset for read */
	NVME_DSMGMT_IDW		= 1 << 1,	/* integral dataset for write */
	NVME_DSMGMT_AD		= 1 << 2,	/* attribute deallocate */
};

/* One Dataset Management range (16 bytes). */
struct nvme_dsm_range {
	__le32			cattr;		/* 0: context attributes */
	__le32			nlb;		/* 4: length in logical blocks */
	__le64			slba;		/* 8: starting LBA */
};

/* Context attributes (cattr) of a range. */
enum {
	NVME_DSM_CATTR_AF_MASK	= 0xf,		/* access frequency */
	NVME_DSM_CATTR_AL_SHIFT	= 4,		/* access latency 5:4 */
	NVME_DSM_CATTR_SR	= 1 << 6,	/* sequential read range */
	NVME_DSM_CATTR_SW	= 1 << 7,	/* sequential write range */
	NVME_DSM_CATTR_WP	= 1 << 8,	/* write prepare */
	NVME_DSM_CATTR_CAS_SHIFT = 24,		/* command access size 31:24 */
};

/* Write Zeroes (0x08): rw layout; control bit 9 DEAC (deallocate). */
struct nvme_write_zeroes_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2;
	__le64			metadata;
	union nvme_data_ptr	dptr;
	__le64			slba;		/* 40 */
	__le16			length;		/* 48: NLB, 0's based */
	__le16			control;	/* 50: LR FUA PRINFO DEAC STC */
	__le32			dsmgmt;		/* 52 */
	__le32			reftag;		/* 56 */
	__le16			lbat;		/* 60 */
	__le16			lbatm;		/* 62 */
};

/*
 * Copy (0x19).
 *   cdw10-11  SDLBA: destination starting LBA
 *   cdw12     bits 7:0 NR (ranges, 0's based) | 11:8 DESFMT |
 *             15:12 PRINFOR | 23:20 DTYPE | 24 STCW | 29:26 PRINFOW |
 *             30 FUA | 31 LR
 *   cdw13     bits 31:16 DSPEC
 *   cdw14     LBTL / ILBRT for the destination
 *   cdw15     LBAT | LBATM
 * control (cdw12 bits 31:16) uses the NVME_RW_* bit positions
 * (LR, FUA, PRINFO = PRINFOW, STC = STCW, DTYPE).
 */
struct nvme_copy_command {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	__le32			cdw2;
	__le32			cdw3;
	__le64			metadata;
	union nvme_data_ptr	dptr;		/* 24: source range list */
	__le64			sdlba;		/* 40 */
	uint8_t			nr_range;	/* 48: 0's based */
	uint8_t			desfmt;		/* 49: 3:0 DESFMT 7:4 PRINFOR */
	__le16			control;	/* 50 */
	__le16			rsvd13;		/* 52 */
	__le16			dspec;		/* 54 */
	__le32			reftag;		/* 56 */
	__le16			lbat;		/* 60 */
	__le16			lbatm;		/* 62 */
};

#define NVME_COPY_DESFMT_MASK		0x0f
#define NVME_COPY_PRINFOR_SHIFT		4
#define NVME_COPY_PRINFOR_MASK		0xf0
/* PRINFOR bit values (before shifting): */
enum {
	NVME_COPY_PRINFO_PRCHK_REF	= 1 << 0,
	NVME_COPY_PRINFO_PRCHK_APP	= 1 << 1,
	NVME_COPY_PRINFO_PRCHK_GUARD	= 1 << 2,
	NVME_COPY_PRINFO_PRACT		= 1 << 3,
};

/* Copy descriptor formats (DESFMT), see Identify Controller OCFS. */
enum {
	NVME_COPY_FMT_0		= 0,	/* 32 bytes, 16b guard PI */
	NVME_COPY_FMT_1		= 1,	/* 40 bytes, 32b/64b guard PI */
	NVME_COPY_FMT_2		= 2,	/* 32 bytes, cross namespace */
	NVME_COPY_FMT_3		= 3,	/* 40 bytes, cross namespace */
};

/* Source range entry, descriptor format 0 (32 bytes). */
struct nvme_copy_range {
	uint8_t			rsvd0[8];
	__le64			slba;		/* 8 */
	__le16			nlb;		/* 16: 0's based */
	uint8_t			rsvd18[6];
	__le32			eilbrt;		/* 24: expected ref tag */
	__le16			elbat;		/* 28: expected app tag */
	__le16			elbatm;		/* 30: app tag mask */
};

/* Source range entry, descriptor format 1 (40 bytes). */
struct nvme_copy_range_f1 {
	uint8_t			rsvd0[8];
	__le64			slba;		/* 8 */
	__le16			nlb;		/* 16: 0's based */
	uint8_t			rsvd18[8];
	uint8_t			elbt[10];	/* 26: expected storage+ref tag */
	__le16			elbat;		/* 36 */
	__le16			elbatm;		/* 38 */
};

/* Source range entry, descriptor format 2 (32 bytes). */
struct nvme_copy_range_f2 {
	__le32			snsid;		/* 0: source namespace */
	uint8_t			rsvd4[4];
	__le64			slba;		/* 8 */
	__le16			nlb;		/* 16: 0's based */
	uint8_t			rsvd18[4];
	__le16			sopt;		/* 22: source options */
	__le32			eilbrt;		/* 24 */
	__le16			elbat;		/* 28 */
	__le16			elbatm;		/* 30 */
};

/* Source range entry, descriptor format 3 (40 bytes). */
struct nvme_copy_range_f3 {
	__le32			snsid;		/* 0 */
	uint8_t			rsvd4[4];
	__le64			slba;		/* 8 */
	__le16			nlb;		/* 16: 0's based */
	uint8_t			rsvd18[4];
	__le16			sopt;		/* 22 */
	uint8_t			rsvd24[2];
	uint8_t			elbt[10];	/* 26 */
	__le16			elbat;		/* 36 */
	__le16			elbatm;		/* 38 */
};

/*
 * Cancel (I/O 0x18, NVMe 2.1): abort one or all outstanding commands of
 * this submission queue.  cdw10: bits 15:0 SQID, 31:16 CID;
 * cdw11: bits 1:0 action code.
 */
struct nvme_cancel_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint32_t		rsvd2[8];
	__le16			sqid;		/* 40 */
	uint16_t		cid;		/* 42 */
	__le32			cdw11;		/* 44: ACODE */
	uint32_t		rsvd12[4];
};

enum {
	NVME_CANCEL_ACODE_SINGLE	= 0,	/* the command CID */
	NVME_CANCEL_ACODE_MULTIPLE	= 1,	/* all commands for NSID */
};

/* Result (dword 0) of Cancel: bits 15:0 commands aborted (multiple),
 * dword 1 bits 15:0 commands deferred. */

/* ==========================================================================
 * Zoned Namespace: Zone Management Send / Receive
 * ========================================================================== */

/* Zone Send Action (ZSA, cdw13 bits 7:0). */
enum nvme_zone_mgmt_action {
	NVME_ZONE_CLOSE		= 0x1,
	NVME_ZONE_FINISH	= 0x2,
	NVME_ZONE_OPEN		= 0x3,
	NVME_ZONE_RESET		= 0x4,
	NVME_ZONE_OFFLINE	= 0x5,
	NVME_ZONE_SET_DESC_EXT	= 0x10,	/* set zone descriptor extension */
	NVME_ZONE_FLUSH		= 0x11,	/* flush explicit ZRWA range */
};

struct nvme_zone_mgmt_send_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	__le32			cdw2[2];
	__le64			metadata;
	union nvme_data_ptr	dptr;		/* 24: descriptor extension */
	__le64			slba;		/* 40: zone start LBA */
	__le32			cdw12;		/* 48 */
	uint8_t			zsa;		/* 52: NVME_ZONE_* */
	uint8_t			select_all;	/* 53: NVME_ZMS_* */
	uint8_t			rsvd13[2];
	__le32			cdw14[2];
};

/* zms.select_all byte (cdw13 bits 15:8). */
enum {
	NVME_ZMS_SELECT_ALL	= 1 << 0,	/* all zones, SLBA ignored */
	NVME_ZMS_ZSASO		= 1 << 1,	/* action specific: open ZRWAA */
};

struct nvme_zone_mgmt_recv_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	__le64			rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24 */
	__le64			slba;		/* 40 */
	__le32			numd;		/* 48: dwords, 0's based */
	uint8_t			zra;		/* 52: receive action */
	uint8_t			zrasf;		/* 53: action specific field */
	uint8_t			pr;		/* 54: bit 0 partial report */
	uint8_t			rsvd13;
	__le32			cdw14[2];
};

/* Zone Receive Action (ZRA) and Zone Receive Action Specific (ZRASF). */
enum {
	NVME_ZRA_ZONE_REPORT		= 0,
	NVME_ZRA_EXT_ZONE_REPORT	= 1,	/* with descriptor extensions */
	NVME_ZRASF_ZONE_REPORT_ALL	= 0,
	NVME_ZRASF_ZONE_STATE_EMPTY	= 0x01,
	NVME_ZRASF_ZONE_STATE_IMP_OPEN	= 0x02,
	NVME_ZRASF_ZONE_STATE_EXP_OPEN	= 0x03,
	NVME_ZRASF_ZONE_STATE_CLOSED	= 0x04,
	NVME_ZRASF_ZONE_STATE_FULL	= 0x05,
	NVME_ZRASF_ZONE_STATE_READONLY	= 0x06,
	NVME_ZRASF_ZONE_STATE_OFFLINE	= 0x07,
	NVME_ZRASF_ZONE_STATE_ZRWA	= 0x09,	/* zones with a ZRWA */
	NVME_REPORT_ZONE_PARTIAL	= 1,
};

/* ==========================================================================
 * I/O Management Send / Receive (Flexible Data Placement)
 * ========================================================================== */

struct nvme_io_mgmt_recv_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	__le64			rsvd2[2];
	union nvme_data_ptr	dptr;
	uint8_t			mo;		/* 40: management operation */
	uint8_t			rsvd11;
	__le16			mos;		/* 42: operation specific */
	__le32			numd;		/* 44: dwords, 0's based */
	__le32			cdw12[4];
};

struct nvme_io_mgmt_send_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	__le64			rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24: placement id list */
	uint8_t			mo;		/* 40 */
	uint8_t			rsvd11;
	__le16			mos;		/* 42: RUH update: ids, 0's based */
	__le32			cdw11[5];
};

enum {
	NVME_IO_MGMT_RECV_MO_RUHS	= 1,	/* reclaim unit handle status */
	NVME_IO_MGMT_SEND_MO_RUHU	= 1,	/* reclaim unit handle update */
};

/* Reclaim Unit Handle Status descriptor (32 bytes). */
struct nvme_fdp_ruh_status_desc {
	__le16			pid;		/* 0: placement identifier */
	__le16			ruhid;		/* 2: reclaim unit handle id */
	__le32			earutr;		/* 4: est. active RU time left, s */
	__le64			ruamw;		/* 8: RU available media writes */
	uint8_t			reserved[16];
};

/* Reclaim Unit Handle Status (header 16 bytes). */
struct nvme_fdp_ruh_status {
	uint8_t			rsvd0[14];
	__le16			nruhsd;		/* 14: descriptors */
	struct nvme_fdp_ruh_status_desc ruhsd[];
};

/* Placement identifier in DSPEC with directive type data placement:
 * bits 15:0 = (reclaim group << (16 - RGIF)) | placement handle. */

/* ==========================================================================
 * Admin: Identify, Get/Set Features, Get Log Page
 * ========================================================================== */

/*
 * Identify (0x06).
 *   cdw10  bits 7:0 CNS | bits 31:16 CNTID
 *   cdw11  bits 15:0 CNS specific identifier | bits 31:24 CSI
 *   cdw14  bits 6:0 UUID index
 */
struct nvme_identify {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24 */
	uint8_t			cns;		/* 40 */
	uint8_t			rsvd3;
	__le16			ctrlid;		/* 42: CNTID */
	__le16			cnssid;		/* 44: CNS specific id */
	uint8_t			rsvd11;
	uint8_t			csi;		/* 47 */
	uint32_t		rsvd12[2];
	uint8_t			uuid;		/* 56: UUID index (6:0) */
	uint8_t			rsvd14[3];
	uint32_t		rsvd15;
};

/*
 * Get Features (0x0a) / Set Features (0x09).
 *   cdw10  bits 7:0 FID | Get: bits 10:8 SEL | Set: bit 31 SV (save)
 *   cdw11..15 feature specific; cdw14 bits 6:0 UUID index
 */
struct nvme_features {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;
	__le32			fid;		/* 40: cdw10 */
	__le32			dword11;	/* 44 */
	__le32			dword12;	/* 48 */
	__le32			dword13;	/* 52 */
	__le32			dword14;	/* 56 */
	__le32			dword15;	/* 60 */
};

/* features.fid (cdw10) high bits. */
#define NVME_FEAT_SV		(1u << 31)	/* Set Features: save */
#define NVME_FEAT_SEL_SHIFT	8
#define NVME_FEAT_SEL_MASK	(0x7u << 8)
enum {
	NVME_FEAT_SEL_CURRENT	= 0 << 8,
	NVME_FEAT_SEL_DEFAULT	= 1 << 8,
	NVME_FEAT_SEL_SAVED	= 2 << 8,
	NVME_FEAT_SEL_SUPPORTED	= 3 << 8,	/* capabilities */
};

/* Get Features SEL = supported capabilities: result dword 0 bits. */
enum {
	NVME_FEAT_CAP_SAVE	= 1 << 0,	/* saveable */
	NVME_FEAT_CAP_NS	= 1 << 1,	/* namespace specific */
	NVME_FEAT_CAP_CHANGE	= 1 << 2,	/* changeable */
};

/*
 * Get Log Page (0x02).
 *   cdw10  bits 7:0 LID | 14:8 LSP | 15 RAE | 31:16 NUMDL
 *   cdw11  bits 15:0 NUMDU | 31:16 LSI (log specific identifier)
 *   cdw12/13  LPOL/LPOU: log page offset (bytes, or an index if OT)
 *   cdw14  bits 6:0 UUID index | 23 OT | 31:24 CSI
 * NUMD = NUMDU:NUMDL is the dword count, 0's based.
 */
struct nvme_get_log_page_command {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24 */
	uint8_t			lid;		/* 40 */
	uint8_t			lsp;		/* 41: 6:0 LSP, 7 RAE */
	__le16			numdl;		/* 42 */
	__le16			numdu;		/* 44 */
	__le16			lsi;		/* 46 */
	union {
		struct {
			__le32	lpol;		/* 48 */
			__le32	lpou;		/* 52 */
		};
		__le64		lpo;
	};
	uint8_t			uuid;		/* 56: UUID index (6:0) */
	uint8_t			rsvd14;
	uint8_t			ot;		/* 58: bit 7 offset type */
	uint8_t			csi;		/* 59 */
	uint32_t		rsvd15;
};

#define NVME_LOG_LSP_MASK	0x7f
#define NVME_LOG_RAE		(1 << 7)	/* in lsp: retain async event */
#define NVME_LOG_OT		(1 << 7)	/* in ot: offset is an index */

/* ==========================================================================
 * Admin: queue management, abort, firmware, format, AER, keep alive
 * ========================================================================== */

/*
 * Create I/O Completion Queue (0x05).
 *   cdw10  bits 15:0 QID | 31:16 QSIZE (0's based)
 *   cdw11  bit 0 PC | bit 1 IEN | bits 31:16 IV (interrupt vector)
 */
struct nvme_create_cq {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	uint32_t		rsvd1[5];
	__le64			prp1;		/* 24 */
	uint64_t		rsvd8;
	__le16			cqid;		/* 40 */
	__le16			qsize;		/* 42 */
	__le16			cq_flags;	/* 44 */
	__le16			irq_vector;	/* 46 */
	uint32_t		rsvd12[4];
};

/*
 * Create I/O Submission Queue (0x01).
 *   cdw10  bits 15:0 QID | 31:16 QSIZE (0's based)
 *   cdw11  bit 0 PC | bits 2:1 QPRIO | bits 31:16 CQID
 *   cdw12  bits 15:0 NVMSETID
 */
struct nvme_create_sq {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	uint32_t		rsvd1[5];
	__le64			prp1;		/* 24 */
	uint64_t		rsvd8;
	__le16			sqid;		/* 40 */
	__le16			qsize;		/* 42 */
	__le16			sq_flags;	/* 44 */
	__le16			cqid;		/* 46 */
	__le16			nvmsetid;	/* 48 */
	uint16_t		rsvd12;
	uint32_t		rsvd13[3];
};

/* cq_flags / sq_flags. */
enum {
	NVME_QUEUE_PHYS_CONTIG	= (1 << 0),
	NVME_CQ_IRQ_ENABLED	= (1 << 1),
	NVME_SQ_PRIO_URGENT	= (0 << 1),
	NVME_SQ_PRIO_HIGH	= (1 << 1),
	NVME_SQ_PRIO_MEDIUM	= (2 << 1),
	NVME_SQ_PRIO_LOW	= (3 << 1),
};
#define NVME_SQ_PRIO_MASK	(3 << 1)

/* Delete I/O Submission (0x00) / Completion (0x04) Queue. */
struct nvme_delete_queue {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	uint32_t		rsvd1[9];
	__le16			qid;		/* 40 */
	uint16_t		rsvd10;
	uint32_t		rsvd11[5];
};

/*
 * Abort (0x08): cdw10 bits 15:0 SQID, 31:16 CID.
 * Result dword 0 bit 0: 0 = the command was aborted, 1 = not aborted.
 */
struct nvme_abort_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	uint32_t		rsvd1[9];
	__le16			sqid;		/* 40 */
	uint16_t		cid;		/* 42 */
	uint32_t		rsvd11[5];
};
#define NVME_ABORT_NOT_ABORTED	(1u << 0)	/* in result dword 0 */

/*
 * Firmware Image Download (0x11).
 *   cdw10  NUMD, dwords, 0's based
 *   cdw11  OFST, offset in dwords; both must respect FWUG granularity
 */
struct nvme_download_firmware {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	uint32_t		rsvd1[5];
	union nvme_data_ptr	dptr;		/* 24 */
	__le32			numd;		/* 40 */
	__le32			offset;		/* 44 */
	uint32_t		rsvd12[4];
};

/*
 * Firmware Commit (0x10, nvme_admin_activate_fw): common.cdw10
 *   bits 2:0 FS (firmware slot, 0 = controller chooses)
 *   bits 5:3 CA (commit action)
 *   bit 31   BPID (boot partition for CA 6/7)
 */
#define NVME_FWACT_SLOT_MASK	0x7
enum {
	NVME_FWACT_REPL		= (0 << 3),	/* replace in slot */
	NVME_FWACT_REPL_ACTV	= (1 << 3),	/* replace, activate at reset */
	NVME_FWACT_ACTV		= (2 << 3),	/* activate at next reset */
	NVME_FWACT_REPL_ACTV_IMM = (3 << 3),	/* replace, activate now */
	NVME_FWACT_REPL_BP	= (6 << 3),	/* replace boot partition */
	NVME_FWACT_ACTV_BP	= (7 << 3),	/* mark boot partition active */
	NVME_FWACT_MASK		= (7 << 3),
};
#define NVME_FWACT_BPID		(1u << 31)

/* Firmware Commit result dword 0 bit 0: multiple update detected (MUD). */
#define NVME_FWACT_RESULT_MUD	(1u << 0)

/*
 * Format NVM (0x80): cdw10
 *   bits 3:0 LBAFL | 4 MSET | 7:5 PI | 8 PIL | 11:9 SES | 13:12 LBAFU
 */
struct nvme_format_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[4];
	__le32			cdw10;		/* 40 */
	uint32_t		rsvd11[5];
};

enum {
	NVME_FORMAT_LBAFL_MASK	= 0xf,
	NVME_FORMAT_MSET	= 1 << 4,	/* metadata as extended LBA */
	NVME_FORMAT_PI_SHIFT	= 5,
	NVME_FORMAT_PI_MASK	= 7 << 5,
	NVME_FORMAT_PIL		= 1 << 8,	/* PI in first bytes of md */
	NVME_FORMAT_SES_SHIFT	= 9,
	NVME_FORMAT_SES_MASK	= 7 << 9,
	NVME_FORMAT_SES_NONE	= 0 << 9,	/* no secure erase */
	NVME_FORMAT_SES_USER	= 1 << 9,	/* user data erase */
	NVME_FORMAT_SES_CRYPTO	= 2 << 9,	/* cryptographic erase */
	NVME_FORMAT_LBAFU_SHIFT	= 12,
	NVME_FORMAT_LBAFU_MASK	= 3 << 12,
};

/* Format NVM cdw10 for LBA format index lbaf (0..63). */
#define NVME_FORMAT_LBAF(lbaf) \
	(((uint32_t)(lbaf) & NVME_FORMAT_LBAFL_MASK) | \
	 ((((uint32_t)(lbaf) >> 4) & 0x3) << NVME_FORMAT_LBAFU_SHIFT))

/*
 * Asynchronous Event Request (0x0c): no parameters; completes when an
 * event occurs.  Keep Alive (0x18): no parameters.
 */

/* ==========================================================================
 * Admin: namespace management and attachment, self-test, virtualization,
 * capacity management, lockdown, sanitize, security, LBA status
 * ========================================================================== */

/* Namespace Management (0x0d): common.cdw10 bits 3:0 SEL;
 * cdw11 bits 31:24 CSI (create).  Create returns the NSID in dword 0. */
enum {
	NVME_NS_MGMT_SEL_CREATE	= 0x0,
	NVME_NS_MGMT_SEL_DELETE	= 0x1,
};
#define NVME_NS_MGMT_CSI_SHIFT	24

/* Namespace Attachment (0x15): common.cdw10 bits 3:0 SEL; data is a
 * controller list (struct nvme_ctrl_list). */
enum {
	NVME_NS_ATTACH_SEL_ATTACH = 0x0,
	NVME_NS_ATTACH_SEL_DETACH = 0x1,
};

/* Device Self-test (0x14): common.cdw10 bits 3:0 STC. */
enum {
	NVME_ST_CODE_SHORT	= 0x1,
	NVME_ST_CODE_EXTENDED	= 0x2,
	NVME_ST_CODE_VS		= 0xe,
	NVME_ST_CODE_ABORT	= 0xf,
};

/*
 * Virtualization Management (0x1c): common.cdw10
 *   bits 3:0 ACT | 10:8 RT | 31:16 CNTLID; cdw11 bits 15:0 NR
 */
enum {
	NVME_VIRT_MGMT_ACT_PRIM_CTRL_FLEX_ALLOC	= 0x1,
	NVME_VIRT_MGMT_ACT_OFFLINE_SEC_CTRL	= 0x7,
	NVME_VIRT_MGMT_ACT_ASSIGN_SEC_CTRL	= 0x8,
	NVME_VIRT_MGMT_ACT_ONLINE_SEC_CTRL	= 0x9,
};
enum {
	NVME_VIRT_MGMT_RT_VQ_RESOURCE	= 0x0,	/* queue resources */
	NVME_VIRT_MGMT_RT_VI_RESOURCE	= 0x1,	/* interrupt resources */
};
#define NVME_VIRT_MGMT_ACT_MASK		0xf
#define NVME_VIRT_MGMT_RT_SHIFT		8
#define NVME_VIRT_MGMT_CNTLID_SHIFT	16

/*
 * Capacity Management (0x20): common.cdw10 bits 3:0 OPER,
 * 31:16 ELID (element identifier); cdw11 CAPL, cdw12 CAPU (capacity in
 * units of the capacity unit of the configuration, 64-bit).
 */
enum {
	NVME_CAP_MGMT_OPER_SELECT_CONFIG	= 0x0,
	NVME_CAP_MGMT_OPER_CREATE_EG		= 0x1,
	NVME_CAP_MGMT_OPER_DELETE_EG		= 0x2,
	NVME_CAP_MGMT_OPER_CREATE_NVMSET	= 0x3,
	NVME_CAP_MGMT_OPER_DELETE_NVMSET	= 0x4,
};
#define NVME_CAP_MGMT_ELID_SHIFT	16

/*
 * Lockdown (0x24): common.cdw10
 *   bits 3:0 SCP (scope) | 4 PRHBT (prohibit) | 6:5 IFC (interface) |
 *   15:8 OFI (opcode or feature identifier); cdw14 bits 6:0 UUID index
 */
enum {
	NVME_LOCKDOWN_SCP_ADMIN_OPCODE	= 0x0,
	NVME_LOCKDOWN_SCP_SET_FEATURE	= 0x2,
	NVME_LOCKDOWN_SCP_MI_OPCODE	= 0x3,
	NVME_LOCKDOWN_SCP_PCIE_OPCODE	= 0x4,
	NVME_LOCKDOWN_PRHBT		= 1 << 4,
	NVME_LOCKDOWN_IFC_SHIFT		= 5,
	NVME_LOCKDOWN_IFC_ADMIN_SQ	= 0 << 5,
	NVME_LOCKDOWN_IFC_ADMIN_MI	= 1 << 5,
	NVME_LOCKDOWN_IFC_MI		= 2 << 5,
	NVME_LOCKDOWN_OFI_SHIFT		= 8,
};

/*
 * Sanitize (0x84).
 *   cdw10  bits 2:0 SANACT | 3 AUSE | 7:4 OWPASS | 8 OIPBP | 9 NODAS |
 *          10 EMVS (enter media verification state, 2.1)
 *   cdw11  OVRPAT: overwrite pattern
 */
struct nvme_sanitize_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint32_t		rsvd2[8];
	__le32			cdw10;		/* 40 */
	__le32			ovrpat;		/* 44 */
	uint32_t		rsvd12[4];
};

enum {
	NVME_SANACT_EXIT_FAILURE	= 0x1,	/* exit failure mode */
	NVME_SANACT_BLOCK_ERASE		= 0x2,
	NVME_SANACT_OVERWRITE		= 0x3,
	NVME_SANACT_CRYPTO_ERASE	= 0x4,
	NVME_SANACT_EXIT_MEDIA_VERIF	= 0x5,	/* 2.1 */
	NVME_SANACT_MASK		= 0x7,
	NVME_SANITIZE_AUSE		= 1 << 3, /* allow unrestricted exit */
	NVME_SANITIZE_OWPASS_SHIFT	= 4,	/* overwrite passes 7:4 */
	NVME_SANITIZE_OWPASS_MASK	= 0xf << 4,
	NVME_SANITIZE_OIPBP		= 1 << 8, /* invert pattern between */
	NVME_SANITIZE_NODAS		= 1 << 9, /* no-deallocate after */
	NVME_SANITIZE_EMVS		= 1 << 10, /* 2.1 */
};

/*
 * Security Send (0x81) / Security Receive (0x82).
 *   cdw10  bits 7:0 NSSF | 15:8 SPSP0 | 23:16 SPSP1 | 31:24 SECP
 *   cdw11  TL (send transfer length) / AL (receive allocation length)
 */
struct nvme_security_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24 */
	uint8_t			nssf;		/* 40: NVMe security specific */
	uint8_t			spsp0;		/* 41: SP specific, low byte */
	uint8_t			spsp1;		/* 42: SP specific, high byte */
	uint8_t			secp;		/* 43: security protocol */
	union {
		__le32		tl;		/* 44: send */
		__le32		al;		/* 44: receive */
	};
	uint32_t		rsvd12[4];
};

/*
 * Get LBA Status (0x86).
 *   cdw10-11 SLBA | cdw12 MNDW (max dwords, 0's based) |
 *   cdw13 bits 15:0 RL (range length) | 31:24 ATYPE
 */
struct nvme_get_lba_status_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24 */
	__le64			slba;		/* 40 */
	__le32			mndw;		/* 48 */
	__le16			rl;		/* 52 */
	uint8_t			rsvd13;
	uint8_t			atype;		/* 55 */
	uint32_t		rsvd14[2];
};

enum {
	NVME_LBA_STATUS_ATYPE_ALLOCATED		= 0x02,
	NVME_LBA_STATUS_ATYPE_SCAN_UNTRACKED	= 0x10,	/* potentially */
	NVME_LBA_STATUS_ATYPE_TRACKED		= 0x11,	/* unrecoverable */
};

/* Get LBA Status data: header (8 bytes) + descriptors (16 bytes each). */
struct nvme_lba_status_desc {
	__le64			dslba;		/* 0: descriptor starting LBA */
	__le32			nlb;		/* 8: number of logical blocks */
	uint8_t			rsvd12;
	uint8_t			status;		/* 13 */
	uint8_t			rsvd14[2];
};

struct nvme_lba_status {
	__le32			nlsd;		/* 0: descriptors returned */
	uint8_t			cmpc;		/* 4: completion condition */
	uint8_t			rsvd5[3];
	struct nvme_lba_status_desc descs[];
};

enum {
	NVME_LBA_STATUS_CMPC_NO_INFO	= 0x0,
	NVME_LBA_STATUS_CMPC_INCOMPLETE	= 0x1,	/* MNDW reached */
	NVME_LBA_STATUS_CMPC_COMPLETE	= 0x2,	/* RL fully processed */
};

/* ==========================================================================
 * Admin: directives, doorbell buffer config
 * ========================================================================== */

/*
 * Directive Send (0x19) / Directive Receive (0x1a).
 *   cdw10  NUMD (dwords, 0's based)
 *   cdw11  bits 7:0 DOPER | 15:8 DTYPE | 31:16 DSPEC
 *   cdw12  (send, identify / enable directive) bit 0 ENDIR, 15:8 TDTYPE
 *          (send, streams / release identifier) -; (receive, streams /
 *          allocate resources) bits 15:0 NSR
 */
struct nvme_directive_cmd {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	__le32			nsid;
	uint64_t		rsvd2[2];
	union nvme_data_ptr	dptr;		/* 24 */
	__le32			numd;		/* 40 */
	uint8_t			doper;		/* 44 */
	uint8_t			dtype;		/* 45 */
	__le16			dspec;		/* 46 */
	uint8_t			endir;		/* 48 */
	uint8_t			tdtype;		/* 49 */
	uint16_t		rsvd15;

	uint32_t		rsvd16[3];
};

/*
 * Doorbell Buffer Config (0x7c): PRP1 = shadow doorbell buffer, PRP2 =
 * EventIdx buffer; both one memory page, page aligned.  Each holds one
 * dword per doorbell, laid out with the doorbell stride.
 */
struct nvme_dbbuf {
	uint8_t			opcode;
	uint8_t			flags;
	uint16_t		command_id;
	uint32_t		rsvd1[5];
	__le64			prp1;		/* 24: shadow doorbells */
	__le64			prp2;		/* 32: event indexes */
	uint32_t		rsvd12[6];
};

/* ==========================================================================
 * Reservations (I/O commands)
 * ========================================================================== */

/*
 * Reservation Register (0x0d): cdw10 bits 2:0 RREGA | 3 IEKEY |
 *   31:30 CPTPL; data: struct nvmet_pr_register_data.
 * Reservation Acquire (0x11): cdw10 bits 2:0 RACQA | 3 IEKEY |
 *   15:8 RTYPE; data: struct nvmet_pr_acquire_data.
 * Reservation Release (0x15): cdw10 bits 2:0 RRELA | 3 IEKEY |
 *   15:8 RTYPE; data: struct nvmet_pr_release_data.
 * Reservation Report (0x0e): cdw10 NUMD (0's based) | cdw11 bit 0 EDS;
 *   data: struct nvme_reservation_status(_ext).
 * All are sent through common.cdw10/cdw11.
 */
enum nvme_pr_type {
	NVME_PR_WRITE_EXCLUSIVE			= 1,
	NVME_PR_EXCLUSIVE_ACCESS		= 2,
	NVME_PR_WRITE_EXCLUSIVE_REG_ONLY	= 3,
	NVME_PR_EXCLUSIVE_ACCESS_REG_ONLY	= 4,
	NVME_PR_WRITE_EXCLUSIVE_ALL_REGS	= 5,
	NVME_PR_EXCLUSIVE_ACCESS_ALL_REGS	= 6,
};
#define NVME_PR_RTYPE_SHIFT	8	/* RTYPE in cdw10 bits 15:8 */

enum nvme_eds {
	NVME_EXTENDED_DATA_STRUCT	= 0x1,	/* report cdw11 bit 0 */
};

/* Reservation capabilities (Identify Namespace RESCAP). */
enum nvme_pr_capabilities {
	NVME_PR_SUPPORT_PTPL				= 1,
	NVME_PR_SUPPORT_WRITE_EXCLUSIVE			= 1 << 1,
	NVME_PR_SUPPORT_EXCLUSIVE_ACCESS		= 1 << 2,
	NVME_PR_SUPPORT_WRITE_EXCLUSIVE_REG_ONLY	= 1 << 3,
	NVME_PR_SUPPORT_EXCLUSIVE_ACCESS_REG_ONLY	= 1 << 4,
	NVME_PR_SUPPORT_WRITE_EXCLUSIVE_ALL_REGS	= 1 << 5,
	NVME_PR_SUPPORT_EXCLUSIVE_ACCESS_ALL_REGS	= 1 << 6,
	NVME_PR_SUPPORT_IEKEY_VER_1_3_DEF		= 1 << 7,
};

enum nvme_pr_register_action {
	NVME_PR_REGISTER_ACT_REG		= 0,
	NVME_PR_REGISTER_ACT_UNREG		= 1,
	NVME_PR_REGISTER_ACT_REPLACE		= 1 << 1,
};

enum nvme_pr_acquire_action {
	NVME_PR_ACQUIRE_ACT_ACQUIRE		= 0,
	NVME_PR_ACQUIRE_ACT_PREEMPT		= 1,
	NVME_PR_ACQUIRE_ACT_PREEMPT_AND_ABORT	= 1 << 1,
};

enum nvme_pr_release_action {
	NVME_PR_RELEASE_ACT_RELEASE		= 0,
	NVME_PR_RELEASE_ACT_CLEAR		= 1,
};

/* Change Persist Through Power Loss state (register cdw10 bits 31:30). */
enum nvme_pr_change_ptpl {
	NVME_PR_CPTPL_NO_CHANGE			= 0,
	NVME_PR_CPTPL_RESV			= 1u << 30,
	NVME_PR_CPTPL_CLEARED			= 2u << 30,
	NVME_PR_CPTPL_PERSIST			= 3u << 30,
};

#define NVME_PR_IGNORE_KEY (1 << 3)	/* IEKEY, cdw10 bit 3 */

/* Reservation command data. */
struct nvmet_pr_register_data {
	__le64	crkey;		/* current reservation key */
	__le64	nrkey;		/* new reservation key */
};

struct nvmet_pr_acquire_data {
	__le64	crkey;
	__le64	prkey;		/* preempt reservation key */
};

struct nvmet_pr_release_data {
	__le64	crkey;
};

/* Reservation Report: registered controller data structure (24 bytes). */
struct nvme_registered_ctrl {
	__le16	cntlid;		/* 0 */
	uint8_t	rcsts;		/* 2: bit 0 holds reservation */
	uint8_t	rsvd3[5];
	__le64	hostid;		/* 8 */
	__le64	rkey;		/* 16 */
};

/* Reservation Report: header (24 bytes). */
struct nvme_reservation_status {
	__le32	gen;		/* 0: generation */
	uint8_t	rtype;		/* 4: reservation type */
	uint8_t	regctl[2];	/* 5: number of registered controllers */
	uint8_t	resv5[2];
	uint8_t	ptpls;		/* 9: persist through power loss state */
	uint8_t	resv10[14];
	struct nvme_registered_ctrl regctl_ds[];
};

/* Extended registered controller data structure (64 bytes). */
struct nvme_registered_ctrl_ext {
	__le16	cntlid;		/* 0 */
	uint8_t	rcsts;		/* 2 */
	uint8_t	rsvd3[5];
	__le64	rkey;		/* 8 */
	uint8_t	hostid[16];	/* 16: 128-bit host identifier */
	uint8_t	rsvd32[32];
};

/* Extended Reservation Report header (64 bytes). */
struct nvme_reservation_status_ext {
	__le32	gen;
	uint8_t	rtype;
	uint8_t	regctl[2];
	uint8_t	resv5[2];
	uint8_t	ptpls;
	uint8_t	resv10[14];
	uint8_t	rsvd24[40];
	struct nvme_registered_ctrl_ext regctl_eds[];
};

#define NVME_RCSTS_HOLDS_RESV	(1 << 0)

/* ==========================================================================
 * The submission queue entry
 * ========================================================================== */

struct nvme_command {
	union {
		struct nvme_common_command common;
		struct nvme_rw_command rw;
		struct nvme_identify identify;
		struct nvme_features features;
		struct nvme_create_cq create_cq;
		struct nvme_create_sq create_sq;
		struct nvme_delete_queue delete_queue;
		struct nvme_download_firmware dlfw;
		struct nvme_download_firmware download_fw;
		struct nvme_format_cmd format;
		struct nvme_dsm_cmd dsm;
		struct nvme_write_zeroes_cmd write_zeroes;
		struct nvme_zone_mgmt_send_cmd zms;
		struct nvme_zone_mgmt_recv_cmd zmr;
		struct nvme_abort_cmd abort;
		struct nvme_get_log_page_command get_log_page;
		struct nvme_dbbuf dbbuf;
		struct nvme_directive_cmd directive;
		struct nvme_io_mgmt_recv_cmd imr;
		struct nvme_io_mgmt_send_cmd ims;
		struct nvme_copy_command copy;
		struct nvme_cancel_cmd cancel;
		struct nvme_sanitize_cmd sanitize;
		struct nvme_security_cmd security;
		struct nvme_get_lba_status_cmd get_lba_status;
	};
};

static inline bool nvme_is_fabrics(const struct nvme_command *cmd)
{
	return cmd->common.opcode == nvme_fabrics_command;
}

/* Opcode bit 0: data flows from host to controller. */
static inline bool nvme_is_write(const struct nvme_command *cmd)
{
	return cmd->common.opcode & 1;
}

/* ==========================================================================
 * Completion queue entry
 * ========================================================================== */

struct nvme_completion {
	/* Dwords 0-1: command specific result. */
	union nvme_result {
		__le16	u16;
		__le32	u32;
		__le64	u64;
	} result;
	__le16	sq_head;	/* 8: SQ head pointer, entries consumed */
	__le16	sq_id;		/* 10: submission queue that issued it */
	uint16_t command_id;	/* 12: CID of the completed command */
	__le16	status;		/* 14: bit 0 phase tag, 15:1 status */
};

/* Raw status field (le16_to_cpu(cqe->status)). */
#define NVME_CQE_PHASE		0x0001		/* phase tag */
#define NVME_CQE_STATUS(raw)	((uint16_t)((raw) >> 1))

static inline uint16_t nvme_cqe_status_word(const struct nvme_completion *c)
{
	return le16_to_cpu(c->status);
}

static inline bool nvme_cqe_phase(const struct nvme_completion *c)
{
	return (le16_to_cpu(c->status) & NVME_CQE_PHASE) != 0;
}

/* ==========================================================================
 * Status codes (shifted representation, see the header comment)
 * ========================================================================== */

enum {
	/*
	 * Generic Command Status (SCT 0):
	 */
	NVME_SCT_GENERIC		= 0x0,
	NVME_SC_SUCCESS			= 0x0,
	NVME_SC_INVALID_OPCODE		= 0x1,
	NVME_SC_INVALID_FIELD		= 0x2,
	NVME_SC_CMDID_CONFLICT		= 0x3,
	NVME_SC_DATA_XFER_ERROR		= 0x4,
	NVME_SC_POWER_LOSS		= 0x5,	/* aborted: power loss notif. */
	NVME_SC_INTERNAL		= 0x6,
	NVME_SC_ABORT_REQ		= 0x7,	/* aborted: Abort command */
	NVME_SC_ABORT_QUEUE		= 0x8,	/* aborted: SQ deletion */
	NVME_SC_FUSED_FAIL		= 0x9,	/* aborted: failed fused */
	NVME_SC_FUSED_MISSING		= 0xa,	/* aborted: missing fused */
	NVME_SC_INVALID_NS		= 0xb,	/* invalid ns or format */
	NVME_SC_CMD_SEQ_ERROR		= 0xc,
	NVME_SC_SGL_INVALID_LAST	= 0xd,	/* invalid SGL segment desc */
	NVME_SC_SGL_INVALID_COUNT	= 0xe,	/* invalid number of SGL desc */
	NVME_SC_SGL_INVALID_DATA	= 0xf,	/* data SGL length invalid */
	NVME_SC_SGL_INVALID_METADATA	= 0x10,	/* metadata SGL length */
	NVME_SC_SGL_INVALID_TYPE	= 0x11,	/* SGL descriptor type */
	NVME_SC_CMB_INVALID_USE		= 0x12,
	NVME_SC_PRP_INVALID_OFFSET	= 0x13,
	NVME_SC_ATOMIC_WU_EXCEEDED	= 0x14,
	NVME_SC_OP_DENIED		= 0x15,
	NVME_SC_SGL_INVALID_OFFSET	= 0x16,
	NVME_SC_RESERVED		= 0x17,
	NVME_SC_HOST_ID_INCONSIST	= 0x18,
	NVME_SC_KA_TIMEOUT_EXPIRED	= 0x19,
	NVME_SC_KA_TIMEOUT_INVALID	= 0x1A,
	NVME_SC_ABORTED_PREEMPT_ABORT	= 0x1B,
	NVME_SC_SANITIZE_FAILED		= 0x1C,
	NVME_SC_SANITIZE_IN_PROGRESS	= 0x1D,
	NVME_SC_SGL_INVALID_GRANULARITY	= 0x1E,
	NVME_SC_CMD_NOT_SUP_CMB_QUEUE	= 0x1F,
	NVME_SC_NS_WRITE_PROTECTED	= 0x20,
	NVME_SC_CMD_INTERRUPTED		= 0x21,
	NVME_SC_TRANSIENT_TR_ERR	= 0x22,
	NVME_SC_PROHIBITED_BY_LOCKDOWN	= 0x23,
	NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY = 0x24,
	NVME_SC_INVALID_KEY_TAG		= 0x25,	/* 2.1 */
	NVME_SC_HDISNS_NOT_ENABLED	= 0x26,	/* 2.1: dispersed ns */
	NVME_SC_HOST_ID_NOT_INIT	= 0x27,	/* 2.1 */
	NVME_SC_INCORRECT_KEY		= 0x28,	/* 2.1 */
	NVME_SC_FDP_DISABLED		= 0x29,
	NVME_SC_INVALID_PLACEMENT_HANDLE = 0x2A,

	/* Generic, NVM command set (and Key Value) specific: */
	NVME_SC_LBA_RANGE		= 0x80,
	NVME_SC_CAP_EXCEEDED		= 0x81,
	NVME_SC_NS_NOT_READY		= 0x82,
	NVME_SC_RESERVATION_CONFLICT	= 0x83,
	NVME_SC_FORMAT_IN_PROGRESS	= 0x84,
	NVME_SC_INVALID_VALUE_SIZE	= 0x85,
	NVME_SC_INVALID_KEY_SIZE	= 0x86,
	NVME_SC_KV_KEY_NOT_EXISTS	= 0x87,
	NVME_SC_UNRECOVERED_ERROR	= 0x88,
	NVME_SC_KEY_EXISTS		= 0x89,

	/*
	 * Command Specific Status (SCT 1):
	 */
	NVME_SCT_COMMAND_SPECIFIC	= 0x100,
	NVME_SC_CQ_INVALID		= 0x100,
	NVME_SC_QID_INVALID		= 0x101,
	NVME_SC_QUEUE_SIZE		= 0x102,
	NVME_SC_ABORT_LIMIT		= 0x103,
	NVME_SC_ABORT_MISSING		= 0x104,	/* obsolete */
	NVME_SC_ASYNC_LIMIT		= 0x105,
	NVME_SC_FIRMWARE_SLOT		= 0x106,
	NVME_SC_FIRMWARE_IMAGE		= 0x107,
	NVME_SC_INVALID_VECTOR		= 0x108,
	NVME_SC_INVALID_LOG_PAGE	= 0x109,
	NVME_SC_INVALID_FORMAT		= 0x10a,
	NVME_SC_FW_NEEDS_CONV_RESET	= 0x10b,
	NVME_SC_INVALID_QUEUE		= 0x10c,	/* queue deletion */
	NVME_SC_FEATURE_NOT_SAVEABLE	= 0x10d,
	NVME_SC_FEATURE_NOT_CHANGEABLE	= 0x10e,
	NVME_SC_FEATURE_NOT_PER_NS	= 0x10f,
	NVME_SC_FW_NEEDS_SUBSYS_RESET	= 0x110,
	NVME_SC_FW_NEEDS_RESET		= 0x111,
	NVME_SC_FW_NEEDS_MAX_TIME	= 0x112,
	NVME_SC_FW_ACTIVATE_PROHIBITED	= 0x113,
	NVME_SC_OVERLAPPING_RANGE	= 0x114,
	NVME_SC_NS_INSUFFICIENT_CAP	= 0x115,
	NVME_SC_NS_ID_UNAVAILABLE	= 0x116,
	NVME_SC_NS_ALREADY_ATTACHED	= 0x118,
	NVME_SC_NS_IS_PRIVATE		= 0x119,
	NVME_SC_NS_NOT_ATTACHED		= 0x11a,
	NVME_SC_THIN_PROV_NOT_SUPP	= 0x11b,
	NVME_SC_CTRL_LIST_INVALID	= 0x11c,
	NVME_SC_SELF_TEST_IN_PROGRESS	= 0x11d,
	NVME_SC_BP_WRITE_PROHIBITED	= 0x11e,
	NVME_SC_CTRL_ID_INVALID		= 0x11f,
	NVME_SC_SEC_CTRL_STATE_INVALID	= 0x120,
	NVME_SC_CTRL_RES_NUM_INVALID	= 0x121,
	NVME_SC_RES_ID_INVALID		= 0x122,
	NVME_SC_PMR_SAN_PROHIBITED	= 0x123,
	NVME_SC_ANA_GROUP_ID_INVALID	= 0x124,
	NVME_SC_ANA_ATTACH_FAILED	= 0x125,
	NVME_SC_INSUFFICIENT_CAP	= 0x126,
	NVME_SC_NS_ATTACH_LIMIT		= 0x127,
	NVME_SC_PROHIBIT_CMD_EXEC_NOT_SUPP = 0x128,
	NVME_SC_IOCS_NOT_SUPPORTED	= 0x129,
	NVME_SC_IOCS_NOT_ENABLED	= 0x12a,
	NVME_SC_IOCS_COMBINATION_REJECTED = 0x12b,
	NVME_SC_INVALID_IO_CMD_SET	= 0x12c,
	NVME_SC_ID_UNAVAILABLE		= 0x12d,

	/*
	 * Command Specific Status, NVM command set:
	 */
	NVME_SC_BAD_ATTRIBUTES		= 0x180,	/* conflicting attrs */
	NVME_SC_INVALID_PI		= 0x181,
	NVME_SC_READ_ONLY		= 0x182,	/* write to RO range */
	NVME_SC_CMD_SIZE_LIM_EXCEEDED	= 0x183,
	NVME_SC_INCOMPATIBLE_NS		= 0x184,	/* ns or format */
	NVME_SC_FAST_COPY_NOT_POSSIBLE	= 0x185,
	NVME_SC_OVERLAPPING_IO_RANGE	= 0x186,

	/*
	 * Command Specific Status, Zoned Namespace command set:
	 */
	NVME_SC_ZONE_BOUNDARY_ERROR	= 0x1b8,
	NVME_SC_ZONE_FULL		= 0x1b9,
	NVME_SC_ZONE_READ_ONLY		= 0x1ba,
	NVME_SC_ZONE_OFFLINE		= 0x1bb,
	NVME_SC_ZONE_INVALID_WRITE	= 0x1bc,
	NVME_SC_ZONE_TOO_MANY_ACTIVE	= 0x1bd,
	NVME_SC_ZONE_TOO_MANY_OPEN	= 0x1be,
	NVME_SC_ZONE_INVALID_TRANSITION	= 0x1bf,

	/*
	 * Media and Data Integrity Errors (SCT 2):
	 */
	NVME_SCT_MEDIA_ERROR		= 0x200,
	NVME_SC_WRITE_FAULT		= 0x280,
	NVME_SC_READ_ERROR		= 0x281,	/* unrecovered read */
	NVME_SC_GUARD_CHECK		= 0x282,	/* end-to-end guard */
	NVME_SC_APPTAG_CHECK		= 0x283,	/* end-to-end app tag */
	NVME_SC_REFTAG_CHECK		= 0x284,	/* end-to-end ref tag */
	NVME_SC_COMPARE_FAILED		= 0x285,
	NVME_SC_ACCESS_DENIED		= 0x286,
	NVME_SC_UNWRITTEN_BLOCK		= 0x287,	/* deallocated */
	NVME_SC_STORAGE_TAG_CHECK	= 0x288,	/* end-to-end storage */

	/*
	 * Path-related Errors (SCT 3):
	 */
	NVME_SCT_PATH			= 0x300,
	NVME_SC_INTERNAL_PATH_ERROR	= 0x300,
	NVME_SC_ANA_PERSISTENT_LOSS	= 0x301,
	NVME_SC_ANA_INACCESSIBLE	= 0x302,
	NVME_SC_ANA_TRANSITION		= 0x303,
	NVME_SC_CTRL_PATH_ERROR		= 0x360,
	NVME_SC_HOST_PATH_ERROR		= 0x370,
	NVME_SC_HOST_ABORTED_CMD	= 0x371,

	/*
	 * Vendor Specific (SCT 7):
	 */
	NVME_SCT_VENDOR			= 0x700,

	NVME_SC_MASK			= 0x00ff, /* Status Code */
	NVME_SCT_MASK			= 0x0700, /* Status Code Type */
	NVME_SCT_SC_MASK		= NVME_SCT_MASK | NVME_SC_MASK,

	NVME_STATUS_CRD			= 0x1800, /* Command Retry Delayed */
	NVME_STATUS_MORE		= 0x2000,
	NVME_STATUS_DNR			= 0x4000, /* Do Not Retry */
};

/* Fields of a shifted status value. */
#define NVME_SCT(status)	((status) >> 8 & 7)
#define NVME_SC(status)		((status) & NVME_SC_MASK)
#define NVME_STATUS_CRD_SHIFT	11
#define NVME_CRD(status)	(((status) & NVME_STATUS_CRD) >> \
				 NVME_STATUS_CRD_SHIFT)

/* Status code type values as returned by NVME_SCT(). */
enum {
	NVME_SCT_TYPE_GENERIC		= 0x0,
	NVME_SCT_TYPE_CMD_SPECIFIC	= 0x1,
	NVME_SCT_TYPE_MEDIA		= 0x2,
	NVME_SCT_TYPE_PATH		= 0x3,
	NVME_SCT_TYPE_VENDOR		= 0x7,
};

/* CRD selects Identify Controller CRDT1..3 (0 = retry at once). */
enum {
	NVME_CRD_NONE	= 0,
	NVME_CRD_CRDT1	= 1,
	NVME_CRD_CRDT2	= 2,
	NVME_CRD_CRDT3	= 3,
};

/* ==========================================================================
 * Identify: CNS and CSI values
 * ========================================================================== */

/* Controller or Namespace Structure (CNS, Identify cdw10 bits 7:0). */
enum {
	NVME_ID_CNS_NS			= 0x00,	/* namespace (NSID) */
	NVME_ID_CNS_CTRL		= 0x01,	/* controller */
	NVME_ID_CNS_NS_ACTIVE_LIST	= 0x02,	/* active NSIDs > NSID */
	NVME_ID_CNS_NS_DESC_LIST	= 0x03,	/* ns identification desc */
	NVME_ID_CNS_NVMSET_LIST		= 0x04,	/* NVM set list */
	NVME_ID_CNS_CS_NS		= 0x05,	/* I/O cmd set specific ns */
	NVME_ID_CNS_CS_CTRL		= 0x06,	/* I/O cmd set specific ctrl */
	NVME_ID_CNS_NS_ACTIVE_LIST_CS	= 0x07,	/* active NSIDs of a CSI */
	NVME_ID_CNS_NS_CS_INDEP		= 0x08,	/* cmd set independent ns */
	NVME_ID_CNS_NS_FMT_IDX		= 0x09,	/* ns data for a format idx */
	NVME_ID_CNS_CS_NS_FMT_IDX	= 0x0a,	/* cs ns data for format idx */
	NVME_ID_CNS_NS_PRESENT_LIST	= 0x10,	/* allocated NSIDs */
	NVME_ID_CNS_NS_PRESENT		= 0x11,	/* allocated namespace */
	NVME_ID_CNS_CTRL_NS_LIST	= 0x12,	/* ctrls attached to NSID */
	NVME_ID_CNS_CTRL_LIST		= 0x13,	/* ctrls in the subsystem */
	NVME_ID_CNS_PRIMARY_CTRL_CAP	= 0x14,	/* primary ctrl capabilities */
	NVME_ID_CNS_SCNDRY_CTRL_LIST	= 0x15,	/* secondary ctrl list */
	NVME_ID_CNS_NS_GRANULARITY	= 0x16,	/* namespace granularity */
	NVME_ID_CNS_UUID_LIST		= 0x17,	/* UUID list */
	NVME_ID_CNS_DOMAIN_LIST		= 0x18,	/* domain list */
	NVME_ID_CNS_ENDGRP_LIST		= 0x19,	/* endurance group list */
	NVME_ID_CNS_NS_PRESENT_LIST_CS	= 0x1a,	/* allocated NSIDs of a CSI */
	NVME_ID_CNS_CS_NS_PRESENT	= 0x1b,	/* cs data, allocated ns */
	NVME_ID_CNS_CMD_SET		= 0x1c,	/* I/O command set vectors */
};

/* Command Set Identifier (CSI). */
enum {
	NVME_CSI_NVM			= 0,
	NVME_CSI_KV			= 1,	/* key value */
	NVME_CSI_ZNS			= 2,	/* zoned namespace */
	NVME_CSI_SLM			= 3,	/* subsystem local memory, 2.1 */
	NVME_CSI_CP			= 4,	/* computational programs, 2.1 */
};

/* ==========================================================================
 * Identify: power state descriptor and LBA format
 * ========================================================================== */

/* Power State Descriptor (32 bytes). */
struct nvme_id_power_state {
	__le16			max_power;	/* 0: MP, centiwatts (MXPS) */
	uint8_t			rsvd2;
	uint8_t			flags;		/* 3: NVME_PS_FLAGS_* */
	__le32			entry_lat;	/* 4: ENLAT, microseconds */
	__le32			exit_lat;	/* 8: EXLAT, microseconds */
	uint8_t			read_tput;	/* 12: RRT (4:0) */
	uint8_t			read_lat;	/* 13: RRL (4:0) */
	uint8_t			write_tput;	/* 14: RWT (4:0) */
	uint8_t			write_lat;	/* 15: RWL (4:0) */
	__le16			idle_power;	/* 16: IDLP */
	uint8_t			idle_scale;	/* 18: IPS (7:6) */
	uint8_t			rsvd19;
	__le16			active_power;	/* 20: ACTP */
	uint8_t			active_work_scale; /* 22: APW 2:0, APS 7:6 */
	uint8_t			rsvd23[9];
};

enum {
	NVME_PS_FLAGS_MAX_POWER_SCALE	= 1 << 0,	/* MXPS: 0.0001 W */
	NVME_PS_FLAGS_NON_OP_STATE	= 1 << 1,	/* NOPS */
};

/* idle_scale / active_work_scale bits 7:6: power scale. */
enum {
	NVME_PS_PS_NOT_REPORTED	= 0,
	NVME_PS_PS_0_0001_W	= 1,
	NVME_PS_PS_0_01_W	= 2,
};
#define NVME_PS_SCALE(v)	(((v) >> 6) & 0x3)
#define NVME_PS_APW(v)		((v) & 0x7)	/* active power workload */

/* LBA Format data structure (4 bytes). */
struct nvme_lbaf {
	__le16			ms;		/* 0: metadata bytes per LBA */
	uint8_t			ds;		/* 2: LBA data size, 2^ds */
	uint8_t			rp;		/* 3: relative performance 1:0 */
};

enum {
	NVME_LBAF_RP_BEST	= 0,
	NVME_LBAF_RP_BETTER	= 1,
	NVME_LBAF_RP_GOOD	= 2,
	NVME_LBAF_RP_DEGRADED	= 3,
};
#define NVME_LBAF_RP_MASK	0x3

/* ==========================================================================
 * Identify Controller (CNS 0x01)
 * ========================================================================== */

struct nvme_id_ctrl {
	__le16			vid;		/* 0: PCI vendor id */
	__le16			ssvid;		/* 2: PCI subsystem vendor id */
	char			sn[20];		/* 4: serial number, ASCII */
	char			mn[40];		/* 24: model number, ASCII */
	char			fr[8];		/* 64: firmware revision */
	uint8_t			rab;		/* 72: arbitration burst 2^n */
	uint8_t			ieee[3];	/* 73: IEEE OUI */
	uint8_t			cmic;		/* 76: multi-path I/O caps */
	uint8_t			mdts;		/* 77: max data transfer size */
	__le16			cntlid;		/* 78: controller id */
	__le32			ver;		/* 80: version (VS format) */
	__le32			rtd3r;		/* 84: RTD3 resume, us */
	__le32			rtd3e;		/* 88: RTD3 entry, us */
	__le32			oaes;		/* 92: optional async events */
	__le32			ctratt;		/* 96: controller attributes */
	__le16			rrls;		/* 100: read recovery levels */
	uint8_t			rsvd102[9];
	uint8_t			cntrltype;	/* 111: controller type */
	uint8_t			fguid[16];	/* 112: FRU GUID */
	__le16			crdt1;		/* 128: retry delay 1, 100 ms */
	__le16			crdt2;		/* 130 */
	__le16			crdt3;		/* 132 */
	uint8_t			rsvd134[119];
	uint8_t			nvmsr;		/* 253: NVM subsystem report */
	uint8_t			vwci;		/* 254: VPD write cycle info */
	uint8_t			mec;		/* 255: mgmt endpoint caps */
	__le16			oacs;		/* 256: optional admin cmds */
	uint8_t			acl;		/* 258: abort limit, 0's based */
	uint8_t			aerl;		/* 259: AER limit, 0's based */
	uint8_t			frmw;		/* 260: firmware updates */
	uint8_t			lpa;		/* 261: log page attributes */
	uint8_t			elpe;		/* 262: error log entries, 0's */
	uint8_t			npss;		/* 263: power states, 0's based */
	uint8_t			avscc;		/* 264: admin vendor cmd config */
	uint8_t			apsta;		/* 265: autonomous PS transition */
	__le16			wctemp;		/* 266: warning temp, K */
	__le16			cctemp;		/* 268: critical temp, K */
	__le16			mtfa;		/* 270: max fw activation, 100ms */
	__le32			hmpre;		/* 272: HMB preferred, 4 KiB */
	__le32			hmmin;		/* 276: HMB minimum, 4 KiB */
	uint8_t			tnvmcap[16];	/* 280: total capacity, bytes */
	uint8_t			unvmcap[16];	/* 296: unallocated, bytes */
	__le32			rpmbs;		/* 312: replay protected mem */
	__le16			edstt;		/* 316: ext self-test, minutes */
	uint8_t			dsto;		/* 318: self-test options */
	uint8_t			fwug;		/* 319: fw update gran, 4 KiB */
	__le16			kas;		/* 320: keep alive, 100 ms */
	__le16			hctma;		/* 322: host ctrl thermal mgmt */
	__le16			mntmt;		/* 324: min thermal mgmt temp, K */
	__le16			mxtmt;		/* 326: max thermal mgmt temp, K */
	__le32			sanicap;	/* 328: sanitize capabilities */
	__le32			hmminds;	/* 332: HMB min desc size, 4 KiB */
	__le16			hmmaxd;		/* 336: HMB max descriptors */
	__le16			nvmsetidmax;	/* 338: max NVM set id */
	__le16			endgidmax;	/* 340: max endurance group id */
	uint8_t			anatt;		/* 342: ANA transition time, s */
	uint8_t			anacap;		/* 343: ANA capabilities */
	__le32			anagrpmax;	/* 344: max ANA group id */
	__le32			nanagrpid;	/* 348: number of ANA group ids */
	__le32			pels;		/* 352: persistent event log size,
						 * 64 KiB units */
	__le16			domainid;	/* 356: domain identifier */
	uint8_t			rsvd358[10];
	uint8_t			megcap[16];	/* 368: max endurance grp cap */
	uint8_t			tmpthha;	/* 384: temp threshold
						 * hysteresis attrs (2.1) */
	uint8_t			rsvd385;
	__le16			cqt;		/* 386: cmd quiesce time, ms */
	uint8_t			rsvd388[124];
	uint8_t			sqes;		/* 512: 3:0 required 7:4 max */
	uint8_t			cqes;		/* 513: 3:0 required 7:4 max */
	__le16			maxcmd;		/* 514: max outstanding (fabrics) */
	__le32			nn;		/* 516: number of namespaces */
	__le16			oncs;		/* 520: optional NVM commands */
	__le16			fuses;		/* 522: fused operations */
	uint8_t			fna;		/* 524: format NVM attributes */
	uint8_t			vwc;		/* 525: volatile write cache */
	__le16			awun;		/* 526: atomic write normal, 0's */
	__le16			awupf;		/* 528: atomic write power fail */
	uint8_t			nvscc;		/* 530: I/O vendor cmd config */
	uint8_t			nwpc;		/* 531: ns write protection */
	__le16			acwu;		/* 532: atomic compare & write */
	__le16			ocfs;		/* 534: copy formats supported */
	__le32			sgls;		/* 536: SGL support */
	__le32			mnan;		/* 540: max allowed namespaces */
	uint8_t			maxdna[16];	/* 544: max domain ns attach */
	__le32			maxcna;		/* 560: max I/O ctrl ns attach */
	uint8_t			rsvd564[204];
	char			subnqn[256];	/* 768: NVM subsystem NQN */
	uint8_t			rsvd1024[768];
	__le32			ioccsz;		/* 1792: fabrics: SQE size, 16 B */
	__le32			iorcsz;		/* 1796: fabrics: CQE size, 16 B */
	__le16			icdoff;		/* 1800: in capsule data offset */
	uint8_t			ctrattr;	/* 1802: fabrics ctrl attrs */
	uint8_t			msdbd;		/* 1803: max SGL data block desc */
	__le16			ofcs;		/* 1804: optional fabric cmds */
	uint8_t			dctype;		/* 1806: discovery ctrl type */
	uint8_t			rsvd1807[241];
	struct nvme_id_power_state psd[32];	/* 2048 */
	uint8_t			vs[1024];	/* 3072: vendor specific */
};

/* Controller type (CNTRLTYPE). */
enum nvme_ctrl_type {
	NVME_CTRL_IO	= 1,		/* I/O controller */
	NVME_CTRL_DISC	= 2,		/* Discovery controller */
	NVME_CTRL_ADMIN	= 3,		/* Administrative controller */
};

enum nvme_dctype {
	NVME_DCTYPE_NOT_REPORTED	= 0,
	NVME_DCTYPE_DDC			= 1, /* Direct Discovery Controller */
	NVME_DCTYPE_CDC			= 2, /* Central Discovery Controller */
};

/* Controller attributes (CTRATT). */
enum nvme_ctrl_attr {
	NVME_CTRL_ATTR_HID_128_BIT	= (1 << 0),
	NVME_CTRL_ATTR_NOPSPM		= (1 << 1),
	NVME_CTRL_ATTR_NVM_SETS		= (1 << 2),
	NVME_CTRL_ATTR_RRL		= (1 << 3),
	NVME_CTRL_ATTR_ENDURANCE_GROUPS	= (1 << 4),
	NVME_CTRL_ATTR_PLM		= (1 << 5),
	NVME_CTRL_ATTR_TBKAS		= (1 << 6),
	NVME_CTRL_ATTR_NG		= (1 << 7),
	NVME_CTRL_ATTR_SQA		= (1 << 8),
	NVME_CTRL_ATTR_UUID_LIST	= (1 << 9),
	NVME_CTRL_ATTR_MDS		= (1 << 10),
	NVME_CTRL_ATTR_FCM		= (1 << 11),
	NVME_CTRL_ATTR_VCM		= (1 << 12),
	NVME_CTRL_ATTR_DEG		= (1 << 13),
	NVME_CTRL_ATTR_DNVMS		= (1 << 14),
	NVME_CTRL_ATTR_ELBAS		= (1 << 15),
	NVME_CTRL_ATTR_RHII		= (1 << 18),
	NVME_CTRL_ATTR_FDPS		= (1 << 19),
};

enum {
	/* CMIC: multi-path I/O and namespace sharing */
	NVME_CTRL_CMIC_MULTI_PORT		= 1 << 0,
	NVME_CTRL_CMIC_MULTI_CTRL		= 1 << 1,
	NVME_CTRL_CMIC_SRIOV			= 1 << 2,
	NVME_CTRL_CMIC_ANA			= 1 << 3,
	/* ONCS: optional NVM command support */
	NVME_CTRL_ONCS_COMPARE			= 1 << 0,
	NVME_CTRL_ONCS_WRITE_UNCORRECTABLE	= 1 << 1,
	NVME_CTRL_ONCS_DSM			= 1 << 2,
	NVME_CTRL_ONCS_WRITE_ZEROES		= 1 << 3,
	NVME_CTRL_ONCS_SAVE_SELECT		= 1 << 4,
	NVME_CTRL_ONCS_RESERVATIONS		= 1 << 5,
	NVME_CTRL_ONCS_TIMESTAMP		= 1 << 6,
	NVME_CTRL_ONCS_VERIFY			= 1 << 7,
	NVME_CTRL_ONCS_COPY			= 1 << 8,
	/* VWC: volatile write cache */
	NVME_CTRL_VWC_PRESENT			= 1 << 0,
	NVME_CTRL_VWC_FLUSH_MASK		= 3 << 1,
	NVME_CTRL_VWC_FLUSH_NOT_INDICATED	= 0 << 1,
	NVME_CTRL_VWC_FLUSH_NO_BROADCAST	= 2 << 1,
	NVME_CTRL_VWC_FLUSH_BROADCAST		= 3 << 1,
	/* OACS: optional admin command support */
	NVME_CTRL_OACS_SEC_SUPP			= 1 << 0,
	NVME_CTRL_OACS_FORMAT			= 1 << 1,
	NVME_CTRL_OACS_FW			= 1 << 2,
	NVME_CTRL_OACS_NS_MNGT_SUPP		= 1 << 3,
	NVME_CTRL_OACS_SELF_TEST		= 1 << 4,
	NVME_CTRL_OACS_DIRECTIVES		= 1 << 5,
	NVME_CTRL_OACS_NVME_MI			= 1 << 6,
	NVME_CTRL_OACS_VIRT_MGMT		= 1 << 7,
	NVME_CTRL_OACS_DBBUF_SUPP		= 1 << 8,
	NVME_CTRL_OACS_LBA_STATUS		= 1 << 9,
	NVME_CTRL_OACS_LOCKDOWN			= 1 << 10,
	/* LPA: log page attributes */
	NVME_CTRL_LPA_SMART_PER_NS		= 1 << 0,
	NVME_CTRL_LPA_CMD_EFFECTS_LOG		= 1 << 1,
	NVME_CTRL_LPA_EXTENDED_DATA		= 1 << 2,
	NVME_CTRL_LPA_TELEMETRY			= 1 << 3,
	NVME_CTRL_LPA_PERSISTENT_EVENT		= 1 << 4,
	NVME_CTRL_LPA_SUPPORTED_LOGS		= 1 << 5,
	NVME_CTRL_LPA_TELEMETRY_DA4		= 1 << 6,
	/* CTRATT (same bits as enum nvme_ctrl_attr) */
	NVME_CTRL_CTRATT_128_ID			= 1 << 0,
	NVME_CTRL_CTRATT_NON_OP_PSP		= 1 << 1,
	NVME_CTRL_CTRATT_NVM_SETS		= 1 << 2,
	NVME_CTRL_CTRATT_READ_RECV_LVLS		= 1 << 3,
	NVME_CTRL_CTRATT_ENDURANCE_GROUPS	= 1 << 4,
	NVME_CTRL_CTRATT_PREDICTABLE_LAT	= 1 << 5,
	NVME_CTRL_CTRATT_NAMESPACE_GRANULARITY	= 1 << 7,
	NVME_CTRL_CTRATT_UUID_LIST		= 1 << 9,
	/* SGLS: SGL support */
	NVME_CTRL_SGLS_BYTE_ALIGNED		= 1,
	NVME_CTRL_SGLS_DWORD_ALIGNED		= 2,
	NVME_CTRL_SGLS_SUPP_MASK		= 3,
	NVME_CTRL_SGLS_KSDBDS			= 1 << 2,
	NVME_CTRL_SGLS_BIT_BUCKET		= 1 << 16,
	NVME_CTRL_SGLS_MD_CONTIG		= 1 << 17,
	NVME_CTRL_SGLS_LONG_LEN			= 1 << 18,
	NVME_CTRL_SGLS_MSDS			= 1 << 19,
	NVME_CTRL_SGLS_SAOS			= 1 << 20,
	NVME_CTRL_SGLS_TSDBDS			= 1 << 21,
};

/* FRMW: firmware updates. */
enum {
	NVME_CTRL_FRMW_1ST_RO		= 1 << 0,	/* slot 1 read only */
	NVME_CTRL_FRMW_NR_SLOTS_SHIFT	= 1,		/* bits 3:1 */
	NVME_CTRL_FRMW_NR_SLOTS_MASK	= 7 << 1,
	NVME_CTRL_FRMW_FW_ACT_NO_RESET	= 1 << 4,	/* activate w/o reset */
	NVME_CTRL_FRMW_MP_UP_DETECTION	= 1 << 5,	/* multiple update */
};

/* AVSCC / NVSCC bit 0: vendor commands use the standard format. */
#define NVME_CTRL_AVSCC_AVS	(1 << 0)
#define NVME_CTRL_NVSCC_NVS	(1 << 0)

/* APSTA bit 0: autonomous power state transitions supported. */
#define NVME_CTRL_APSTA_APST	(1 << 0)

/* HCTMA bit 0: host controlled thermal management supported. */
#define NVME_CTRL_HCTMA_HCTM	(1 << 0)

/* DSTO: device self-test options. */
#define NVME_CTRL_DSTO_ONE_DST	(1 << 0)	/* one test per subsystem */

/* FUSES bit 0: compare and write. */
#define NVME_CTRL_FUSES_COMPARE_AND_WRITE (1 << 0)

/* FNA: format NVM attributes. */
enum {
	NVME_CTRL_FNA_FMT_ALL_NAMESPACES	= 1 << 0,
	NVME_CTRL_FNA_SEC_ALL_NAMESPACES	= 1 << 1,
	NVME_CTRL_FNA_CRYPTO_ERASE		= 1 << 2,
	NVME_CTRL_FNA_NSID_FFFFFFFF		= 1 << 3, /* broadcast unsupp. */
};

/* NWPC: namespace write protection capabilities. */
enum {
	NVME_CTRL_NWPC_WRITE_PROTECT		= 1 << 0,
	NVME_CTRL_NWPC_WRITE_PROTECT_POWER_CYCLE = 1 << 1,
	NVME_CTRL_NWPC_WRITE_PROTECT_PERMANENT	= 1 << 2,
};

/* OCFS: optional copy formats supported. */
enum {
	NVME_CTRL_OCFS_FMT_0	= 1 << 0,
	NVME_CTRL_OCFS_FMT_1	= 1 << 1,
	NVME_CTRL_OCFS_FMT_2	= 1 << 2,
	NVME_CTRL_OCFS_FMT_3	= 1 << 3,
};

/* ANACAP: ANA capabilities. */
enum {
	NVME_CTRL_ANACAP_OPT		= 1 << 0,
	NVME_CTRL_ANACAP_NON_OPT	= 1 << 1,
	NVME_CTRL_ANACAP_INACCESSIBLE	= 1 << 2,
	NVME_CTRL_ANACAP_PERSISTENT_LOSS = 1 << 3,
	NVME_CTRL_ANACAP_CHANGE		= 1 << 4,
	NVME_CTRL_ANACAP_GRPID_NO_CHG	= 1 << 6,
	NVME_CTRL_ANACAP_GRPID_MGMT	= 1 << 7,
};

/* SANICAP: sanitize capabilities. */
#define NVME_CTRL_SANICAP_CES		(1u << 0)	/* crypto erase */
#define NVME_CTRL_SANICAP_BES		(1u << 1)	/* block erase */
#define NVME_CTRL_SANICAP_OWS		(1u << 2)	/* overwrite */
#define NVME_CTRL_SANICAP_VERS		(1u << 3)	/* media verif., 2.1 */
#define NVME_CTRL_SANICAP_NDI		(1u << 29)	/* no-dealloc inhib. */
#define NVME_CTRL_SANICAP_NODMMAS_SHIFT	30		/* bits 31:30 */
#define NVME_CTRL_SANICAP_NODMMAS_MASK	(3u << 30)

/* RPMBS: replay protected memory block support. */
#define NVME_CTRL_RPMBS_NR_UNITS(v)	((v) & 0x7)
#define NVME_CTRL_RPMBS_AUTH_METHOD(v)	(((v) >> 3) & 0x7)
#define NVME_CTRL_RPMBS_TOTAL_SIZE(v)	(((v) >> 16) & 0xff) /* 128 KiB, 0's */
#define NVME_CTRL_RPMBS_ACCESS_SIZE(v)	(((v) >> 24) & 0xff) /* 512 B, 0's */

/* SQES / CQES nibbles. */
#define NVME_CTRL_SQES_MIN(v)	((v) & 0xf)
#define NVME_CTRL_SQES_MAX(v)	(((v) >> 4) & 0xf)
#define NVME_CTRL_CQES_MIN(v)	((v) & 0xf)
#define NVME_CTRL_CQES_MAX(v)	(((v) >> 4) & 0xf)

/* ==========================================================================
 * Identify Namespace (CNS 0x00, NVM command set)
 * ========================================================================== */

struct nvme_id_ns {
	__le64			nsze;		/* 0: size in logical blocks */
	__le64			ncap;		/* 8: capacity in blocks */
	__le64			nuse;		/* 16: utilization in blocks */
	uint8_t			nsfeat;		/* 24: features */
	uint8_t			nlbaf;		/* 25: LBA formats, 0's based */
	uint8_t			flbas;		/* 26: formatted LBA size */
	uint8_t			mc;		/* 27: metadata capabilities */
	uint8_t			dpc;		/* 28: data protection caps */
	uint8_t			dps;		/* 29: data protection settings */
	uint8_t			nmic;		/* 30: multi-path / sharing */
	uint8_t			rescap;		/* 31: reservation capabilities */
	uint8_t			fpi;		/* 32: format progress indicator */
	uint8_t			dlfeat;		/* 33: deallocate features */
	__le16			nawun;		/* 34: atomic write normal, 0's */
	__le16			nawupf;		/* 36: atomic write power fail */
	__le16			nacwu;		/* 38: atomic compare & write */
	__le16			nabsn;		/* 40: atomic boundary size */
	__le16			nabo;		/* 42: atomic boundary offset */
	__le16			nabspf;		/* 44: boundary size power fail */
	__le16			noiob;		/* 46: optimal I/O boundary */
	uint8_t			nvmcap[16];	/* 48: NVM capacity, bytes */
	__le16			npwg;		/* 64: pref write gran, 0's */
	__le16			npwa;		/* 66: pref write align, 0's */
	__le16			npdg;		/* 68: pref dealloc gran, 0's */
	__le16			npda;		/* 70: pref dealloc align, 0's */
	__le16			nows;		/* 72: optimal write size, 0's */
	__le16			mssrl;		/* 74: max single source range */
	__le32			mcl;		/* 76: max copy length */
	uint8_t			msrc;		/* 80: max source ranges, 0's */
	uint8_t			kpios;		/* 81: key per I/O status, 2.1 */
	uint8_t			nulbaf;		/* 82: unique-capability LBAFs */
	uint8_t			rsvd83;
	__le32			kpiodaag;	/* 84: key per I/O data access
						 * alignment/granularity, 2.1 */
	uint8_t			rsvd88[4];
	__le32			anagrpid;	/* 92: ANA group identifier */
	uint8_t			rsvd96[3];
	uint8_t			nsattr;		/* 99: namespace attributes */
	__le16			nvmsetid;	/* 100: NVM set identifier */
	__le16			endgid;		/* 102: endurance group id */
	uint8_t			nguid[16];	/* 104: namespace GUID */
	uint8_t			eui64[8];	/* 120: IEEE EUI-64 */
	struct nvme_lbaf	lbaf[64];	/* 128: LBA formats 0..63 */
	uint8_t			vs[3712];	/* 384: vendor specific */
};

/* I/O Command Set Independent Identify Namespace (CNS 0x08). */
struct nvme_id_ns_cs_indep {
	uint8_t			nsfeat;		/* 0 */
	uint8_t			nmic;		/* 1 */
	uint8_t			rescap;		/* 2 */
	uint8_t			fpi;		/* 3 */
	__le32			anagrpid;	/* 4 */
	uint8_t			nsattr;		/* 8 */
	uint8_t			rsvd9;
	__le16			nvmsetid;	/* 10 */
	__le16			endgid;		/* 12 */
	uint8_t			nstat;		/* 14: namespace status */
	uint8_t			rsvd15[4081];
};

enum {
	/* NSFEAT */
	NVME_NS_FEAT_THIN	= 1 << 0,	/* thin provisioning */
	NVME_NS_FEAT_ATOMICS	= 1 << 1,	/* NAWUN/NAWUPF/NACWU valid */
	NVME_NS_FEAT_DAE	= 1 << 2,	/* deallocated/unwritten err */
	NVME_NS_FEAT_UIDREUSE	= 1 << 3,	/* NGUID/EUI64 never reused */
	NVME_NS_FEAT_IO_OPT	= 1 << 4,	/* NPWG..NOWS valid */
	/* NSATTR */
	NVME_NS_ATTR_RO		= 1 << 0,	/* write protected */
	/* FLBAS */
	NVME_NS_FLBAS_LBA_MASK	= 0xf,
	NVME_NS_FLBAS_LBA_UMASK	= 0x60,
	NVME_NS_FLBAS_LBA_SHIFT	= 1,
	NVME_NS_FLBAS_META_EXT	= 0x10,		/* metadata in extended LBA */
	/* NMIC */
	NVME_NS_NMIC_SHARED	= 1 << 0,
	NVME_NS_NMIC_DISPERSED	= 1 << 1,	/* 2.1 */
	/* cs_indep NSFEAT */
	NVME_NS_ROTATIONAL	= 1 << 4,
	NVME_NS_VWC_NOT_PRESENT = 1 << 5,
	/* DPC: protection capabilities */
	NVME_NS_DPC_PI_LAST	= 1 << 4,
	NVME_NS_DPC_PI_FIRST	= 1 << 3,
	NVME_NS_DPC_PI_TYPE3	= 1 << 2,
	NVME_NS_DPC_PI_TYPE2	= 1 << 1,
	NVME_NS_DPC_PI_TYPE1	= 1 << 0,
	/* DPS: protection settings */
	NVME_NS_DPS_PI_FIRST	= 1 << 3,
	NVME_NS_DPS_PI_MASK	= 0x7,
	NVME_NS_DPS_PI_TYPE1	= 1,
	NVME_NS_DPS_PI_TYPE2	= 2,
	NVME_NS_DPS_PI_TYPE3	= 3,
};

/* NSTAT (cs_indep). */
enum {
	NVME_NSTAT_NRDY		= 1 << 0,	/* NRDY: set = namespace ready */
};

/* DLFEAT: deallocate logical block features. */
enum {
	NVME_NS_DLFEAT_READ_MASK	= 0x7,
	NVME_NS_DLFEAT_READ_NOT_REPORTED = 0x0,
	NVME_NS_DLFEAT_READ_ZEROES	= 0x1,	/* reads return 0x00 */
	NVME_NS_DLFEAT_READ_ONES	= 0x2,	/* reads return 0xff */
	NVME_NS_DLFEAT_WRITE_ZEROES	= 1 << 3, /* DEAC supported */
	NVME_NS_DLFEAT_GUARD_CRC	= 1 << 4, /* guard = CRC of data */
};

/* FPI: format progress indicator. */
#define NVME_NS_FPI_SUPPORTED		(1 << 7)
#define NVME_NS_FPI_REMAINING(v)	((v) & 0x7f)	/* percent left */

/* Identify Namespace Metadata Capabilities (MC): */
enum {
	NVME_MC_EXTENDED_LBA	= (1 << 0),
	NVME_MC_METADATA_PTR	= (1 << 1),
};

/* LBA format index from FLBAS (bits 3:0 low, 6:5 high). */
static inline uint8_t nvme_lbaf_index(uint8_t flbas)
{
	return (flbas & NVME_NS_FLBAS_LBA_MASK) |
		((flbas & NVME_NS_FLBAS_LBA_UMASK) >> NVME_NS_FLBAS_LBA_SHIFT);
}

/*
 * Namespace Management create: host software specified fields.  The
 * controller ignores every other byte.  (4096 bytes)
 */
struct nvme_ns_mgmt_host_sw_specified {
	__le64			nsze;		/* 0 */
	__le64			ncap;		/* 8 */
	uint8_t			rsvd16[10];
	uint8_t			flbas;		/* 26 */
	uint8_t			rsvd27[2];
	uint8_t			dps;		/* 29 */
	uint8_t			nmic;		/* 30 */
	uint8_t			rsvd31[61];
	__le32			anagrpid;	/* 92 */
	uint8_t			rsvd96[4];
	__le16			nvmsetid;	/* 100 */
	__le16			endgid;		/* 102 */
	uint8_t			rsvd104[280];
	__le64			lbstm;		/* 384: storage tag mask */
	__le16			nphndls;	/* 392: placement handles */
	uint8_t			rsvd394[118];
	__le16			phndl[128];	/* 512: placement handle list */
	uint8_t			rsvd768[3328];
};

/* ==========================================================================
 * Identify: NVM command set specific (CSI 0x00)
 * ========================================================================== */

/* I/O Command Set specific Identify Namespace, NVM (CNS 0x05, CSI 0). */
struct nvme_id_ns_nvm {
	__le64	lbstm;		/* 0: logical block storage tag mask */
	uint8_t	pic;		/* 8: protection information caps */
	uint8_t	pifa;		/* 9: PI format attribute, 2.1 */
	uint8_t	rsvd10[2];
	__le32	elbaf[64];	/* 12: extended LBA formats */
	__le32	npdgl;		/* 268: pref dealloc gran, large, 2.1 */
	__le32	npdal;		/* 272: pref dealloc align, large, 2.1 */
	uint8_t	rsvd276[3820];
};

/* ELBAF fields. */
enum {
	NVME_ID_NS_NVM_STS_MASK		= 0x7f,	/* storage tag size */
	NVME_ID_NS_NVM_GUARD_SHIFT	= 7,	/* PI format 8:7 */
	NVME_ID_NS_NVM_GUARD_MASK	= 0x3,
	NVME_ID_NS_NVM_QPIF_SHIFT	= 9,	/* qualified PI format 12:9 */
	NVME_ID_NS_NVM_QPIF_MASK	= 0xf,
	NVME_ID_NS_NVM_QPIFS		= 1 << 3, /* in pic: QPIF supported */
};

/* PIC: protection information capabilities. */
enum {
	NVME_NVM_PIC_16BPISTS	= 1 << 0,	/* 16b guard storage tag */
	NVME_NVM_PIC_16BPISTM	= 1 << 1,	/* 16b guard storage tag mask */
	NVME_NVM_PIC_STCRS	= 1 << 2,	/* storage tag check read */
};

/* PIFA bit 0 (2.1): storage tag check read support. */
#define NVME_NVM_PIFA_STCRS	(1 << 0)

/* Protection information format (ELBAF bits 8:7). */
enum {
	NVME_NVM_NS_16B_GUARD	= 0,
	NVME_NVM_NS_32B_GUARD	= 1,
	NVME_NVM_NS_64B_GUARD	= 2,
	NVME_NVM_NS_QTYPE_GUARD	= 3,	/* qualified: see QPIF */
};

static inline uint8_t nvme_elbaf_sts(uint32_t elbaf)
{
	return elbaf & NVME_ID_NS_NVM_STS_MASK;
}

static inline uint8_t nvme_elbaf_guard_type(uint32_t elbaf)
{
	return (elbaf >> NVME_ID_NS_NVM_GUARD_SHIFT) &
		NVME_ID_NS_NVM_GUARD_MASK;
}

static inline uint8_t nvme_elbaf_qualified_guard_type(uint32_t elbaf)
{
	return (elbaf >> NVME_ID_NS_NVM_QPIF_SHIFT) & NVME_ID_NS_NVM_QPIF_MASK;
}

/* I/O Command Set specific Identify Controller, NVM (CNS 0x06, CSI 0).
 * The size limits are 2^n units of CAP.MPSMIN, 0 = no limit. */
struct nvme_id_ctrl_nvm {
	uint8_t	vsl;		/* 0: verify size limit */
	uint8_t	wzsl;		/* 1: write zeroes size limit */
	uint8_t	wusl;		/* 2: write uncorrectable size limit */
	uint8_t	dmrl;		/* 3: DSM ranges limit */
	__le32	dmrsl;		/* 4: DSM range size limit, blocks */
	__le64	dmsl;		/* 8: DSM size limit, blocks */
	uint8_t	kpiocap;	/* 16: key per I/O caps, 2.1 */
	uint8_t	wzdsl;		/* 17: write zeroes w/ DEAC limit, 2.1 */
	__le16	aocs;		/* 18: admin optional cmd support, 2.1 */
	__le32	ver;		/* 20: NVM command set version, 2.1 */
	uint8_t	lbamqf;		/* 24: LBA migration queue format, 2.1 */
	uint8_t	rsvd25[4071];
};

/* ==========================================================================
 * Identify: Zoned Namespace command set specific (CSI 0x02)
 * ========================================================================== */

/* Zone format extension of an LBA format (16 bytes). */
struct nvme_zns_lbafe {
	__le64			zsze;		/* 0: zone size, blocks */
	uint8_t			zdes;		/* 8: descriptor ext, 64 B units */
	uint8_t			rsvd9[7];
};

/* I/O Command Set specific Identify Namespace, ZNS (CNS 0x05, CSI 2). */
struct nvme_id_ns_zns {
	__le16			zoc;		/* 0: zone operation chars */
	__le16			ozcs;		/* 2: optional zoned cmd support */
	__le32			mar;		/* 4: max active resources, 0's */
	__le32			mor;		/* 8: max open resources, 0's */
	__le32			rrl;		/* 12: reset recommended limit, s */
	__le32			frl;		/* 16: finish recommended limit */
	__le32			rrl1;		/* 20 */
	__le32			rrl2;		/* 24 */
	__le32			rrl3;		/* 28 */
	__le32			frl1;		/* 32 */
	__le32			frl2;		/* 36 */
	__le32			frl3;		/* 40 */
	__le32			numzrwa;	/* 44: ZRWA resources, 0's */
	__le16			zrwafg;		/* 48: ZRWA flush gran, blocks */
	__le16			zrwasz;		/* 50: ZRWA size, blocks */
	uint8_t			zrwacap;	/* 52: ZRWA capability */
	uint8_t			rsvd53[2763];
	struct nvme_zns_lbafe	lbafe[64];	/* 2816 */
	uint8_t			vs[256];	/* 3840 */
};

enum {
	NVME_ZNS_ZOC_VZCAP	= 1 << 0,	/* variable zone capacity */
	NVME_ZNS_ZOC_ZAE	= 1 << 1,	/* zone active excursions */
	NVME_ZNS_OZCS_RAZB	= 1 << 0,	/* read across zone boundary */
	NVME_ZNS_OZCS_ZRWASUP	= 1 << 1,	/* ZRWA supported */
	NVME_ZNS_ZRWACAP_EXPFLUSHSUP = 1 << 0,	/* explicit ZRWA flush */
};

/* I/O Command Set specific Identify Controller, ZNS (CNS 0x06, CSI 2). */
struct nvme_id_ctrl_zns {
	uint8_t	zasl;		/* 0: zone append size limit, 2^n MPSMIN */
	uint8_t	rsvd1[4095];
};

/* ==========================================================================
 * Identify: namespace identification descriptors and lists
 * ========================================================================== */

/* Namespace Identification Descriptor header (CNS 0x03); NIDL bytes of
 * identifier follow.  The list is 4096 bytes, ended by NIDL 0. */
struct nvme_ns_id_desc {
	uint8_t nidt;		/* 0: identifier type */
	uint8_t nidl;		/* 1: identifier length */
	__le16 reserved;
};

enum {
	NVME_NIDT_EUI64		= 0x01,
	NVME_NIDT_NGUID		= 0x02,
	NVME_NIDT_UUID		= 0x03,
	NVME_NIDT_CSI		= 0x04,
};

/* Namespace list (CNS 0x02, 0x07, 0x10, 0x1a): ascending NSIDs, 0 ends. */
struct nvme_ns_list {
	__le32			ns[1024];
};

/* Controller list (CNS 0x12, 0x13; Namespace Attachment data). */
struct nvme_ctrl_list {
	__le16			num;		/* 0: number of identifiers */
	__le16			identifier[2047];
};

/* Primary Controller Capabilities (CNS 0x14). */
struct nvme_primary_ctrl_cap {
	__le16			cntlid;		/* 0 */
	__le16			portid;		/* 2 */
	uint8_t			crt;		/* 4: resource types (VQ, VI) */
	uint8_t			rsvd5[27];
	__le32			vqfrt;		/* 32: VQ flexible total */
	__le32			vqrfa;		/* 36: VQ flexible assigned */
	__le16			vqrfap;		/* 40: VQ flexible to primary */
	__le16			vqprt;		/* 42: VQ private total */
	__le16			vqfrsm;		/* 44: VQ flexible max per sec */
	__le16			vqgran;		/* 46: VQ preferred granularity */
	uint8_t			rsvd48[16];
	__le32			vifrt;		/* 64: VI flexible total */
	__le32			virfa;		/* 68 */
	__le16			virfap;		/* 72 */
	__le16			viprt;		/* 74 */
	__le16			vifrsm;		/* 76 */
	__le16			vigran;		/* 78 */
	uint8_t			rsvd80[4016];
};

/* Secondary Controller entry (32 bytes) and list (CNS 0x15). */
struct nvme_secondary_ctrl {
	__le16			scid;		/* 0: secondary controller id */
	__le16			pcid;		/* 2: primary controller id */
	uint8_t			scs;		/* 4: bit 0 online */
	uint8_t			rsvd5[3];
	__le16			vfn;		/* 8: virtual function number */
	__le16			nvq;		/* 10: VQ flexible assigned */
	__le16			nvi;		/* 12: VI flexible assigned */
	uint8_t			rsvd14[18];
};

struct nvme_secondary_ctrl_list {
	uint8_t			num;		/* 0: entries */
	uint8_t			rsvd[31];
	struct nvme_secondary_ctrl sc_entry[127];
};

/* Namespace Granularity descriptor (16 bytes) and list (CNS 0x16). */
struct nvme_id_ns_granularity_desc {
	__le64			nszegran;	/* 0: size granularity, bytes */
	__le64			ncapgran;	/* 8: capacity granularity */
};

struct nvme_id_ns_granularity_list {
	__le32			attributes;	/* 0: bit 0 one descriptor */
	uint8_t			num_descriptors; /* 4: 0's based */
	uint8_t			rsvd5[27];
	struct nvme_id_ns_granularity_desc entry[16];	/* 32 */
	uint8_t			rsvd288[3808];
};

/* UUID list entry (32 bytes) and list (CNS 0x17). */
struct nvme_id_uuid_list_entry {
	uint8_t			header;		/* 0: bits 1:0 association */
	uint8_t			rsvd1[15];
	uint8_t			uuid[16];	/* 16 */
};

struct nvme_id_uuid_list {
	uint8_t			rsvd0[32];
	struct nvme_id_uuid_list_entry entry[NVME_MAX_UUID_ENTRIES];
};

enum {
	NVME_ID_UUID_HDR_ASSOCIATION_MASK	= 0x3,
	NVME_ID_UUID_ASSOCIATION_NONE		= 0,
	NVME_ID_UUID_ASSOCIATION_VENDOR		= 1,	/* IEEE vendor id */
	NVME_ID_UUID_ASSOCIATION_SUBSYSTEM	= 2,	/* subsystem vendor */
};

/* NVM Set Attributes entry (128 bytes) and list (CNS 0x04). */
struct nvme_nvmset_attr {
	__le16			nvmsetid;	/* 0 */
	__le16			endgid;		/* 2 */
	uint8_t			rsvd4[4];
	__le32			rr4kt;		/* 8: random 4 KiB read typ, 100ns */
	__le32			ows;		/* 12: optimal write size, bytes */
	uint8_t			tnvmsetcap[16];	/* 16: total capacity */
	uint8_t			unvmsetcap[16];	/* 32: unallocated capacity */
	uint8_t			rsvd48[80];
};

struct nvme_id_nvmset_list {
	uint8_t			nid;		/* 0: identifiers in list */
	uint8_t			rsvd1[127];
	struct nvme_nvmset_attr	ent[31];	/* 128 */
};

/* Domain Attributes entry (128 bytes) and list (CNS 0x18). */
struct nvme_id_domain_attr {
	__le16			dom_id;		/* 0 */
	uint8_t			rsvd2[14];
	uint8_t			dom_cap[16];	/* 16: total capacity */
	uint8_t			unalloc_dom_cap[16];	/* 32 */
	uint8_t			max_egrp_dom_cap[16];	/* 48 */
	uint8_t			rsvd64[64];
};

struct nvme_id_domain_list {
	uint8_t			num;		/* 0 */
	uint8_t			rsvd[127];
	struct nvme_id_domain_attr domain_attr[31];
};

/* Endurance Group list (CNS 0x19). */
struct nvme_id_endurance_group_list {
	__le16			num;
	__le16			identifier[2047];
};

/* I/O Command Set data structure (CNS 0x1c): 512 command set vectors,
 * each a bitmap of CSI values that may be enabled together. */
struct nvme_id_iocs {
	__le64			iocsc[512];
};

#define NVME_IOCS_VECTOR_NVM	(1ULL << NVME_CSI_NVM)
#define NVME_IOCS_VECTOR_KV	(1ULL << NVME_CSI_KV)
#define NVME_IOCS_VECTOR_ZNS	(1ULL << NVME_CSI_ZNS)

/* ==========================================================================
 * Log pages: identifiers
 * ========================================================================== */

enum {
	NVME_LOG_SUPPORTED	= 0x00,	/* supported log pages */
	NVME_LOG_ERROR		= 0x01,	/* error information */
	NVME_LOG_SMART		= 0x02,	/* SMART / health information */
	NVME_LOG_FW_SLOT	= 0x03,	/* firmware slot information */
	NVME_LOG_CHANGED_NS	= 0x04,	/* changed namespace list */
	NVME_LOG_CMD_EFFECTS	= 0x05,	/* commands supported and effects */
	NVME_LOG_DEVICE_SELF_TEST = 0x06,
	NVME_LOG_TELEMETRY_HOST = 0x07,	/* telemetry host-initiated */
	NVME_LOG_TELEMETRY_CTRL = 0x08,	/* telemetry controller-initiated */
	NVME_LOG_ENDURANCE_GROUP = 0x09,
	NVME_LOG_PLM		= 0x0a,	/* predictable latency per NVM set */
	NVME_LOG_PLM_AGGREGATE	= 0x0b,	/* predictable latency aggregate */
	NVME_LOG_ANA		= 0x0c,	/* asymmetric namespace access */
	NVME_LOG_PERSISTENT_EVENT = 0x0d,
	NVME_LOG_LBA_STATUS	= 0x0e,	/* LBA status information */
	NVME_LOG_ENDGRP_EVENT_AGG = 0x0f, /* endurance group event aggregate */
	NVME_LOG_MEDIA_UNIT_STATUS = 0x10,
	NVME_LOG_SUPPORTED_CAP_CONFIG = 0x11, /* capacity config list */
	NVME_LOG_FEATURES	= 0x12,	/* feature ids supported and effects */
	NVME_LOG_MI_CMD_EFFECTS	= 0x13,	/* NVMe-MI commands supported */
	NVME_LOG_CMD_FEAT_LOCKDOWN = 0x14, /* command and feature lockdown */
	NVME_LOG_BOOT_PARTITION	= 0x15,
	NVME_LOG_RMI		= 0x16,	/* rotational media information */
	NVME_LOG_DISPERSED_NS	= 0x17,	/* dispersed ns participating, 2.1 */
	NVME_LOG_MGMT_ADDR_LIST	= 0x18,	/* management address list, 2.1 */
	NVME_LOG_PHY_RX_EOM	= 0x19,	/* receiver eye opening, 2.1 */
	NVME_LOG_REACH_GROUPS	= 0x1a,	/* reachability groups, 2.1 */
	NVME_LOG_REACH_ASSOC	= 0x1b,	/* reachability associations, 2.1 */
	NVME_LOG_CHANGED_ALLOC_NS = 0x1c, /* changed allocated ns list, 2.1 */
	NVME_LOG_FDP_CONFIGS	= 0x20,	/* FDP configurations */
	NVME_LOG_RUHU		= 0x21,	/* reclaim unit handle usage */
	NVME_LOG_FDP_STATS	= 0x22,	/* FDP statistics */
	NVME_LOG_FDP_EVENTS	= 0x23,	/* FDP events */
	NVME_LOG_DISC		= 0x70,	/* discovery (fabrics) */
	NVME_LOG_RESERVATION	= 0x80,	/* reservation notification */
	NVME_LOG_SANITIZE	= 0x81,	/* sanitize status */
	NVME_LOG_ZNS_CHANGED_ZONES = 0xbf, /* changed zone list (ZNS) */
	NVME_LOG_VENDOR_START	= 0xc0,
	NVME_LOG_VENDOR_END	= 0xff,
};

/* Supported Log Pages (LID 0x00): one dword per LID. */
struct nvme_supported_log {
	__le32	lids[256];
};

enum {
	NVME_LIDS_LSUPP	= 1 << 0,	/* LID supported */
	NVME_LIDS_IOS	= 1 << 1,	/* index offset supported */
};
#define NVME_LIDS_LIDSP_SHIFT	16	/* bits 31:16 LID specific */

/* ==========================================================================
 * Log page 0x01: Error Information (64 bytes per entry)
 * ========================================================================== */

struct nvme_error_slot {
	__le64		error_count;		/* 0: unique, incrementing */
	__le16		sqid;			/* 8 */
	__le16		cmdid;			/* 10 */
	__le16		status_field;		/* 12: 15:1 status, 0 phase */
	__le16		param_error_location;	/* 14: 7:0 byte, 10:8 bit */
	__le64		lba;			/* 16: first failing LBA */
	__le32		nsid;			/* 24 */
	uint8_t		vs;			/* 28: vendor specific log page */
	uint8_t		trtype;			/* 29: transport type */
	uint8_t		csi;			/* 30: command set indicator */
	uint8_t		opcode;			/* 31: opcode (2.0) */
	__le64		cs;			/* 32: command specific info */
	__le16		trtype_spec_info;	/* 40 */
	uint8_t		resv2[21];
	uint8_t		log_page_version;	/* 63 */
};

/* ==========================================================================
 * Log page 0x02: SMART / Health Information (512 bytes)
 * ========================================================================== */

struct nvme_smart_log {
	uint8_t			critical_warning;	/* 0 */
	uint8_t			temperature[2];		/* 1: composite, K */
	uint8_t			avail_spare;		/* 3: percent */
	uint8_t			spare_thresh;		/* 4: percent */
	uint8_t			percent_used;		/* 5: may exceed 100 */
	uint8_t			endu_grp_crit_warn_sumry; /* 6 */
	uint8_t			rsvd7[25];
	uint8_t			data_units_read[16];	/* 32: 1000 x 512 B */
	uint8_t			data_units_written[16];	/* 48 */
	uint8_t			host_reads[16];		/* 64: commands */
	uint8_t			host_writes[16];	/* 80 */
	uint8_t			ctrl_busy_time[16];	/* 96: minutes */
	uint8_t			power_cycles[16];	/* 112 */
	uint8_t			power_on_hours[16];	/* 128 */
	uint8_t			unsafe_shutdowns[16];	/* 144 */
	uint8_t			media_errors[16];	/* 160 */
	uint8_t			num_err_log_entries[16]; /* 176 */
	__le32			warning_temp_time;	/* 192: minutes */
	__le32			critical_comp_time;	/* 196: minutes */
	__le16			temp_sensor[8];		/* 200: K, 0 = none */
	__le32			thm_temp1_trans_count;	/* 216 */
	__le32			thm_temp2_trans_count;	/* 220 */
	__le32			thm_temp1_total_time;	/* 224: seconds */
	__le32			thm_temp2_total_time;	/* 228: seconds */
	uint8_t			rsvd232[280];
};

enum {
	NVME_SMART_CRIT_SPARE		= 1 << 0,
	NVME_SMART_CRIT_TEMPERATURE	= 1 << 1,
	NVME_SMART_CRIT_RELIABILITY	= 1 << 2,
	NVME_SMART_CRIT_MEDIA		= 1 << 3,	/* read only */
	NVME_SMART_CRIT_VOLATILE_MEMORY	= 1 << 4,	/* backup failed */
	NVME_SMART_CRIT_PMR_RO		= 1 << 5,	/* PMR read only */
};

/* Endurance group critical warning summary bits. */
enum {
	NVME_SMART_EGCW_SPARE		= 1 << 0,
	NVME_SMART_EGCW_RELIABILITY	= 1 << 2,
	NVME_SMART_EGCW_RO		= 1 << 3,
};

/* ==========================================================================
 * Log page 0x03: Firmware Slot Information (512 bytes)
 * ========================================================================== */

struct nvme_fw_slot_info_log {
	uint8_t			afi;		/* 0: active firmware info */
	uint8_t			rsvd1[7];
	__le64			frs[7];		/* 8: revision of slot 1..7 */
	uint8_t			rsvd64[448];
};

#define NVME_FW_AFI_ACTIVE(afi)		((afi) & 0x7)	/* running slot */
#define NVME_FW_AFI_NEXT(afi)		(((afi) >> 4) & 0x7) /* after reset */

/* ==========================================================================
 * Log page 0x04: Changed Namespace List (4096 bytes)
 * ========================================================================== */

/* Up to 1024 NSIDs; first entry 0xffffffff = more than 1024 changed. */
struct nvme_changed_ns_list_log {
	__le32			log[NVME_MAX_CHANGED_NAMESPACES];
};

/* ==========================================================================
 * Log page 0x05: Commands Supported and Effects (4096 bytes)
 * ========================================================================== */

struct nvme_effects_log {
	__le32 acs[256];	/* admin commands, by opcode */
	__le32 iocs[256];	/* I/O commands, by opcode */
	uint8_t resv[2048];
};

enum {
	NVME_CMD_EFFECTS_CSUPP		= 1 << 0,	/* supported */
	NVME_CMD_EFFECTS_LBCC		= 1 << 1,	/* LB content change */
	NVME_CMD_EFFECTS_NCC		= 1 << 2,	/* ns capability change */
	NVME_CMD_EFFECTS_NIC		= 1 << 3,	/* ns inventory change */
	NVME_CMD_EFFECTS_CCC		= 1 << 4,	/* ctrl capability chg */
	NVME_CMD_EFFECTS_CSER_MASK	= 0x3 << 14,	/* GENMASK(15, 14) */
	NVME_CMD_EFFECTS_CSE_MASK	= 0x7 << 16,	/* GENMASK(18, 16) */
	NVME_CMD_EFFECTS_UUID_SEL	= 1 << 19,
};
#define NVME_CMD_EFFECTS_SCOPE_MASK	0xfff00000u	/* GENMASK(31, 20) */

/* CSE values (after >> 16). */
enum {
	NVME_CMD_EFFECTS_CSE_NONE	= 0,	/* no restriction */
	NVME_CMD_EFFECTS_CSE_SAME_NS	= 1,	/* one per namespace */
	NVME_CMD_EFFECTS_CSE_ALL	= 2,	/* one at a time, any ns */
};

/* Command scope (CSP, bits 31:20). */
#define NVME_CMD_EFFECTS_CSP_NS		(1u << 20)
#define NVME_CMD_EFFECTS_CSP_CTRL	(1u << 21)
#define NVME_CMD_EFFECTS_CSP_NVM_SET	(1u << 22)
#define NVME_CMD_EFFECTS_CSP_ENDGRP	(1u << 23)
#define NVME_CMD_EFFECTS_CSP_DOMAIN	(1u << 24)
#define NVME_CMD_EFFECTS_CSP_NSS	(1u << 25)

/* ==========================================================================
 * Log page 0x06: Device Self-test (564 bytes)
 * ========================================================================== */

#define NVME_LOG_ST_MAX_RESULTS	20

/* Self-test result entry (28 bytes; POH is at a misaligned offset). */
struct nvme_st_result {
	uint8_t			dsts;		/* 0: 7:4 code, 3:0 result */
	uint8_t			seg;		/* 1: failing segment number */
	uint8_t			vdi;		/* 2: valid diagnostic info */
	uint8_t			rsvd3;
	__le64			poh;		/* 4: power on hours */
	__le32			nsid;		/* 12 */
	__le64			flba;		/* 16: failing LBA */
	uint8_t			sct;		/* 24: status code type */
	uint8_t			sc;		/* 25: status code */
	uint8_t			vs[2];		/* 26 */
} __attribute__((packed));

struct nvme_self_test_log {
	uint8_t			current_operation; /* 0: 3:0 running STC */
	uint8_t			completion;	/* 1: 6:0 percent done */
	uint8_t			rsvd2[2];
	struct nvme_st_result	result[NVME_LOG_ST_MAX_RESULTS]; /* 4 */
} __attribute__((packed));

/* Result (dsts bits 3:0). */
enum {
	NVME_ST_RESULT_NO_ERR		= 0x0,
	NVME_ST_RESULT_ABORTED		= 0x1,	/* by self-test command */
	NVME_ST_RESULT_CLR		= 0x2,	/* by controller reset */
	NVME_ST_RESULT_NS_REMOVED	= 0x3,
	NVME_ST_RESULT_ABORTED_FORMAT	= 0x4,
	NVME_ST_RESULT_FATAL_ERR	= 0x5,
	NVME_ST_RESULT_UNKNOWN_SEG_FAIL	= 0x6,
	NVME_ST_RESULT_KNOWN_SEG_FAIL	= 0x7,
	NVME_ST_RESULT_ABORTED_UNKNOWN	= 0x8,
	NVME_ST_RESULT_ABORTED_SANITIZE	= 0x9,
	NVME_ST_RESULT_NOT_USED		= 0xf,	/* entry unused */
	NVME_ST_RESULT_MASK		= 0xf,
};
#define NVME_ST_CODE_SHIFT	4	/* dsts bits 7:4: STC of the test */

/* Valid diagnostic information (vdi). */
enum {
	NVME_ST_VALID_DIAG_INFO_NSID	= 1 << 0,
	NVME_ST_VALID_DIAG_INFO_FLBA	= 1 << 1,
	NVME_ST_VALID_DIAG_INFO_SCT	= 1 << 2,
	NVME_ST_VALID_DIAG_INFO_SC	= 1 << 3,
};

/* ==========================================================================
 * Log pages 0x07/0x08: Telemetry (header 512 bytes + data blocks)
 * ========================================================================== */

/* LSP of telemetry host-initiated: create new host-initiated data. */
#define NVME_LOG_TELEM_LSP_CREATE	(1 << 0)

struct nvme_telemetry_log {
	uint8_t			lpi;		/* 0: log identifier */
	uint8_t			rsvd1[4];
	uint8_t			ieee[3];	/* 5: IEEE OUI */
	__le16			dalb1;		/* 8: data area 1 last block */
	__le16			dalb2;		/* 10 */
	__le16			dalb3;		/* 12 */
	uint8_t			rsvd14[2];
	__le32			dalb4;		/* 16: needs ETDAS */
	uint8_t			rsvd20[361];
	uint8_t			hostdgn;	/* 381: host data generation */
	uint8_t			ctrlavail;	/* 382: ctrl-initiated avail */
	uint8_t			ctrldgn;	/* 383: ctrl data generation */
	uint8_t			rsnident[128];	/* 384: reason identifier */
	uint8_t			data_area[];	/* 512: 512-byte blocks */
};

/* ==========================================================================
 * Log page 0x09: Endurance Group Information (512 bytes)
 * ========================================================================== */

struct nvme_endurance_group_log {
	uint8_t	egcw;		/* 0: critical warning */
	uint8_t	egfeat;		/* 1: endurance group features */
	uint8_t	rsvd2;
	uint8_t	avsp;		/* 3: available spare, percent */
	uint8_t	avspt;		/* 4: spare threshold */
	uint8_t	pused;		/* 5: percentage used */
	__le16	did;		/* 6: domain identifier */
	uint8_t	rsvd8[24];
	uint8_t	ee[16];		/* 32: endurance estimate */
	uint8_t	dur[16];	/* 48: data units read */
	uint8_t	duw[16];	/* 64: data units written */
	uint8_t	muw[16];	/* 80: media units written */
	uint8_t	hrc[16];	/* 96: host read commands */
	uint8_t	hwc[16];	/* 112: host write commands */
	uint8_t	mdie[16];	/* 128: media and integrity errors */
	uint8_t	neile[16];	/* 144: error information entries */
	uint8_t	tegcap[16];	/* 160: total capacity */
	uint8_t	uegcap[16];	/* 176: unallocated capacity */
	uint8_t	rsvd192[320];
};

/* ==========================================================================
 * Log pages 0x0a/0x0b: Predictable Latency
 * ========================================================================== */

/* Predictable Latency Per NVM Set (LSI = NVM set id, 512 bytes). */
struct nvme_nvmset_predictable_lat_log {
	uint8_t			status;		/* 0: 2:0 window */
	uint8_t			rsvd1;
	__le16			event_type;	/* 2 */
	uint8_t			rsvd4[28];
	__le64			dtwin_rt;	/* 32: DTWIN reads typical */
	__le64			dtwin_wt;	/* 40: DTWIN writes typical */
	__le64			dtwin_tmax;	/* 48: DTWIN time max, ms */
	__le64			ndwin_tmin_hi;	/* 56: NDWIN time min high */
	__le64			ndwin_tmin_lo;	/* 64: NDWIN time min low */
	uint8_t			rsvd72[56];
	__le64			dtwin_re;	/* 128: DTWIN reads estimate */
	__le64			dtwin_we;	/* 136: DTWIN writes estimate */
	__le64			dtwin_te;	/* 144: DTWIN time estimate */
	uint8_t			rsvd152[360];
};

enum {
	NVME_PLM_STATUS_DISABLED	= 0x0,
	NVME_PLM_STATUS_DTWIN		= 0x1,	/* deterministic window */
	NVME_PLM_STATUS_NDWIN		= 0x2,	/* non-deterministic */
};

enum {
	NVME_PLM_EVENT_DTWIN_READS_WARN	= 1 << 0,
	NVME_PLM_EVENT_DTWIN_WRITES_WARN = 1 << 1,
	NVME_PLM_EVENT_DTWIN_TIME_WARN	= 1 << 2,
	NVME_PLM_EVENT_DTWIN_EXCEEDED	= 1 << 14,
	NVME_PLM_EVENT_DTWIN_EXCURSION	= 1 << 15,
};

/* Aggregate logs (0x0b, 0x0f): entry count + 16-bit identifiers. */
struct nvme_aggregate_event_log {
	__le64			num_entries;
	__le16			entries[];
};

/* ==========================================================================
 * Log page 0x0c: Asymmetric Namespace Access
 * ========================================================================== */

enum nvme_ana_state {
	NVME_ANA_OPTIMIZED		= 0x01,
	NVME_ANA_NONOPTIMIZED		= 0x02,
	NVME_ANA_INACCESSIBLE		= 0x03,
	NVME_ANA_PERSISTENT_LOSS	= 0x04,
	NVME_ANA_CHANGE			= 0x0f,
};
#define NVME_ANA_STATE_MASK	0x0f

/* ANA group descriptor (32 bytes + NNSIDS * 4). */
struct nvme_ana_group_desc {
	__le32	grpid;		/* 0: ANA group id */
	__le32	nnsids;		/* 4: number of NSIDs */
	__le64	chgcnt;		/* 8: change count */
	uint8_t	state;		/* 16: 3:0 ANA state */
	uint8_t	rsvd17[15];
	__le32	nsids[];	/* 32 */
};

/* flag for the log specific field of the ANA log: return groups only */
#define NVME_ANA_LOG_RGO	(1 << 0)

/* ANA log header (16 bytes). */
struct nvme_ana_rsp_hdr {
	__le64	chgcnt;		/* 0: change count */
	__le16	ngrps;		/* 8: group descriptors */
	__le16	rsvd10[3];
};

/* ==========================================================================
 * Log page 0x0d: Persistent Event Log
 * ========================================================================== */

/* LSP: management action. */
enum {
	NVME_PEVENT_LOG_READ		= 0x0,
	NVME_PEVENT_LOG_EST_CTX_AND_READ = 0x1,	/* establish context */
	NVME_PEVENT_LOG_RELEASE_CTX	= 0x2,
};

/* Header (512 bytes, several misaligned fields). */
struct nvme_persistent_event_log {
	uint8_t			lid;		/* 0 */
	uint8_t			rsvd1[3];
	__le32			tnev;		/* 4: total number of events */
	__le64			tll;		/* 8: total log length, bytes */
	uint8_t			rv;		/* 16: log revision */
	uint8_t			rsvd17;
	__le16			lhl;		/* 18: log header length */
	__le64			ts;		/* 20: timestamp */
	uint8_t			poh[16];	/* 28: power on hours */
	__le64			pcc;		/* 44: power cycle count */
	__le16			vid;		/* 52 */
	__le16			ssvid;		/* 54 */
	char			sn[20];		/* 56 */
	char			mn[40];		/* 76 */
	char			subnqn[256];	/* 116 */
	__le16			gen_number;	/* 372: generation number */
	__le32			rci;		/* 374: reporting context info */
	uint8_t			rsvd378[102];
	uint8_t			seb[32];	/* 480: supported events bitmap */
} __attribute__((packed));

/* Event header (24 bytes); vendor info and event data follow. */
struct nvme_persistent_event_entry {
	uint8_t			etype;		/* 0: event type */
	uint8_t			etype_rev;	/* 1 */
	uint8_t			ehl;		/* 2: header length - 3 */
	uint8_t			ehai;		/* 3: additional info */
	__le16			cntlid;		/* 4 */
	__le64			ets;		/* 6: event timestamp */
	__le16			pelpid;		/* 14: port identifier */
	uint8_t			rsvd16[4];
	__le16			vsil;		/* 20: vendor info length */
	__le16			el;		/* 22: event length */
} __attribute__((packed));

enum nvme_persistent_event_types {
	NVME_PEL_SMART_HEALTH_EVENT	= 0x01,
	NVME_PEL_FW_COMMIT_EVENT	= 0x02,
	NVME_PEL_TIMESTAMP_EVENT	= 0x03,
	NVME_PEL_POWER_ON_RESET_EVENT	= 0x04,
	NVME_PEL_NSS_HW_ERROR_EVENT	= 0x05,
	NVME_PEL_CHANGE_NS_EVENT	= 0x06,
	NVME_PEL_FORMAT_START_EVENT	= 0x07,
	NVME_PEL_FORMAT_COMPLETION_EVENT = 0x08,
	NVME_PEL_SANITIZE_START_EVENT	= 0x09,
	NVME_PEL_SANITIZE_COMPLETION_EVENT = 0x0a,
	NVME_PEL_SET_FEATURE_EVENT	= 0x0b,
	NVME_PEL_TELEMETRY_CRT		= 0x0c,	/* telemetry log created */
	NVME_PEL_THERMAL_EXCURSION_EVENT = 0x0d,
	NVME_PEL_VENDOR_SPECIFIC_EVENT	= 0xde,
	NVME_PEL_TCG_DEFINED_EVENT	= 0xdf,
};

/* ==========================================================================
 * Log page 0x0e: LBA Status Information
 * ========================================================================== */

/* LBA range descriptor (16 bytes). */
struct nvme_lba_rd {
	__le64			rslba;		/* 0: range starting LBA */
	__le32			rnlb;		/* 8: range number of blocks */
	uint8_t			rsvd12[4];
};

/* Per-namespace element header (16 bytes) + NLRD range descriptors. */
struct nvme_lbas_ns_element {
	__le32			neid;		/* 0: namespace element id */
	__le32			nlrd;		/* 4: range descriptors */
	uint8_t			ratype;		/* 8: recommended action type */
	uint8_t			rsvd9[7];
	struct nvme_lba_rd	lba_rd[];
};

/* Log header (16 bytes). */
struct nvme_lba_status_log {
	__le32			lslplen;	/* 0: log page length */
	__le32			nlslne;		/* 4: namespace elements */
	__le32			estulb;		/* 8: est. unrecoverable LBs */
	uint8_t			rsvd12[2];
	__le16			lsgc;		/* 14: generation counter */
	/* struct nvme_lbas_ns_element entries follow */
};

/* ==========================================================================
 * Log page 0x10: Media Unit Status
 * ========================================================================== */

/* Media unit status descriptor, fixed part (14 bytes); MUCS channel
 * identifiers (16-bit) and vendor data follow at CIO. */
struct nvme_media_unit_stat_desc {
	__le16			muid;		/* 0: media unit id */
	__le16			domainid;	/* 2 */
	__le16			endgid;		/* 4 */
	__le16			nvmsetid;	/* 6 */
	__le16			cap_adj_fctr;	/* 8: capacity adjust factor */
	uint8_t			avl_spare;	/* 10 */
	uint8_t			percent_used;	/* 11 */
	uint8_t			mucs;		/* 12: channel identifiers */
	uint8_t			cio;		/* 13: channel id offset */
};

/* Log header (16 bytes). */
struct nvme_media_unit_stat_log {
	__le16			nmu;		/* 0: number of media units */
	__le16			cchans;		/* 2: channel count */
	__le16			sel_config;	/* 4: selected configuration */
	uint8_t			rsvd6[10];
	/* descriptors follow */
};

/* ==========================================================================
 * Log page 0x12: Feature Identifiers Supported and Effects (1024 bytes)
 * ========================================================================== */

struct nvme_supported_features_log {
	__le32	fis[256];
};

enum {
	NVME_FIS_FSUPP	= 1 << 0,	/* supported */
	NVME_FIS_UDCC	= 1 << 1,	/* user data content change */
	NVME_FIS_NCC	= 1 << 2,	/* namespace capability change */
	NVME_FIS_NIC	= 1 << 3,	/* namespace inventory change */
	NVME_FIS_CCC	= 1 << 4,	/* controller capability change */
	NVME_FIS_UUID_SEL = 1 << 19,
	NVME_FIS_NSCPE	= 1 << 20,	/* namespace scope */
	NVME_FIS_CSCPE	= 1 << 21,	/* controller scope */
	NVME_FIS_NSETSCPE = 1 << 22,	/* NVM set scope */
	NVME_FIS_EGSCPE	= 1 << 23,	/* endurance group scope */
	NVME_FIS_DSCPE	= 1 << 24,	/* domain scope */
	NVME_FIS_NSSCPE	= 1 << 25,	/* NVM subsystem scope */
};

/* ==========================================================================
 * Log page 0x14: Command and Feature Lockdown (512 bytes)
 * ========================================================================== */

struct nvme_lockdown_log {
	uint8_t			cfila;		/* 0: 3:0 scope, 6:5 interface */
	uint8_t			rsvd1[2];
	uint8_t			lsize;		/* 3: list size */
	uint8_t			plist[508];	/* 4: prohibited opcodes/FIDs */
};

/* LSP: content selection. */
enum {
	NVME_LOCKDOWN_LOG_SUPPORTED	= 0x0,	/* lockdown supported list */
	NVME_LOCKDOWN_LOG_PROHIBITED	= 0x1,	/* prohibited list */
};

/* ==========================================================================
 * Log page 0x15: Boot Partition (header 16 bytes + partition data)
 * ========================================================================== */

/* LSP bit 0: boot partition identifier. */
struct nvme_boot_partition {
	uint8_t			lid;		/* 0 */
	uint8_t			rsvd1[3];
	__le32			bpinfo;		/* 4: BPINFO register format */
	uint8_t			rsvd8[8];
	uint8_t			boot_partition_data[];
};

/* ==========================================================================
 * Log page 0x16: Rotational Media Information (512 bytes)
 * ========================================================================== */

struct nvme_rotational_media_log {
	__le16	endgid;		/* 0 */
	__le16	numa;		/* 2: number of actuators */
	__le16	nrs;		/* 4: nominal rotational speed, rpm */
	uint8_t	rsvd6[2];
	__le32	spinc;		/* 8: spinup count */
	__le32	fspinc;		/* 12: failed spinup count */
	__le32	ldc;		/* 16: load count */
	__le32	fldc;		/* 20: failed load count */
	uint8_t	rsvd24[488];
};

/* ==========================================================================
 * Log pages 0x20-0x23: Flexible Data Placement (LSI = endurance group)
 * ========================================================================== */

/* Reclaim unit handle descriptor (4 bytes). */
struct nvme_fdp_ruh_desc {
	uint8_t			ruht;		/* 0: reclaim unit handle type */
	uint8_t			reserved[3];
};

enum {
	NVME_FDP_RUHT_INITIALLY_ISOLATED	= 0x1,
	NVME_FDP_RUHT_PERSISTENTLY_ISOLATED	= 0x2,
};

/* FDP configuration descriptor (64 bytes + NRUH * 4 + vendor data). */
struct nvme_fdp_config_desc {
	__le16			dsze;		/* 0: descriptor size, bytes */
	uint8_t			fdpa;		/* 2: FDP attributes */
	uint8_t			vss;		/* 3: vendor specific, 8 B units */
	__le32			nrg;		/* 4: number of reclaim groups */
	__le16			nruh;		/* 8: reclaim unit handles */
	__le16			maxpids;	/* 10: max placement ids, 0's */
	__le32			nns;		/* 12: max namespaces, 0's */
	__le64			runs;		/* 16: reclaim unit size, bytes */
	__le32			erutl;		/* 24: est. RU time limit, s */
	uint8_t			rsvd28[36];
	struct nvme_fdp_ruh_desc ruhs[];	/* 64 */
};

enum {
	NVME_FDP_FDPA_RGIF_MASK	= 0xf,		/* reclaim group id format */
	NVME_FDP_FDPA_FDPVWC	= 1 << 6,	/* volatile write cache */
	NVME_FDP_FDPA_VALID	= 1 << 7,	/* configuration valid */
};

/* FDP configurations log header (16 bytes); descriptors follow. */
struct nvme_fdp_config_log {
	__le16			numfdpc;	/* 0: configurations, 0's */
	uint8_t			ver;		/* 2: version */
	uint8_t			rsvd3;
	__le32			sze;		/* 4: log size, bytes */
	uint8_t			rsvd8[8];
	/* struct nvme_fdp_config_desc entries follow */
};

/* Reclaim unit handle usage descriptor (8 bytes) and log (0x21). */
struct nvme_fdp_ruhu_desc {
	uint8_t			ruha;		/* 0: RUH attributes */
	uint8_t			rsvd1[7];
};

struct nvme_fdp_ruhu_log {
	__le16			nruh;		/* 0 */
	uint8_t			rsvd2[6];
	struct nvme_fdp_ruhu_desc ruhus[];
};

enum {
	NVME_FDP_RUHA_UNUSED	= 0x0,
	NVME_FDP_RUHA_HOST	= 0x1,	/* host specified */
	NVME_FDP_RUHA_CTRL	= 0x2,	/* controller specified */
};

/* FDP statistics (0x22, 64 bytes). */
struct nvme_fdp_stats_log {
	uint8_t			hbmw[16];	/* 0: host bytes with metadata */
	uint8_t			mbmw[16];	/* 16: media bytes written */
	uint8_t			mbe[16];	/* 32: media bytes erased */
	uint8_t			rsvd48[16];
};

/* Timestamp (8 bytes): feature 0x0e data, FDP event field. */
struct nvme_timestamp {
	uint8_t			timestamp[6];	/* 0: ms since epoch, LE */
	uint8_t			attr;		/* 6: 0 synch, 3:1 origin */
	uint8_t			rsvd;
};

/* FDP event descriptor (64 bytes). */
struct nvme_fdp_event {
	uint8_t			type;		/* 0: event type */
	uint8_t			flags;		/* 1: NVME_FDP_EVENT_F_* */
	__le16			pid;		/* 2: placement identifier */
	struct nvme_timestamp	ts;		/* 4 */
	__le32			nsid;		/* 12 */
	uint8_t			type_specific[16]; /* 16 */
	__le16			rgid;		/* 32: reclaim group */
	uint8_t			ruhid;		/* 34: reclaim unit handle */
	uint8_t			rsvd35[5];
	uint8_t			vs[24];		/* 40 */
};

/* FDP events log (0x23): header 64 bytes + events.  LSP bit 0: host
 * events (1) or controller events (0). */
struct nvme_fdp_events_log {
	__le32			n;		/* 0: events */
	uint8_t			rsvd4[60];
	struct nvme_fdp_event	events[];	/* 64 */
};

#define NVME_FDP_EVENTS_LSP_HOST	(1 << 0)

enum {
	NVME_FDP_EVENT_RUNFW	= 0x00,	/* RU not fully written */
	NVME_FDP_EVENT_RUTLE	= 0x01,	/* RU time limit exceeded */
	NVME_FDP_EVENT_RESET	= 0x02,	/* ctrl reset modified RUH */
	NVME_FDP_EVENT_PID	= 0x03,	/* invalid placement identifier */
	NVME_FDP_EVENT_REALLOC	= 0x80,	/* media reallocated */
	NVME_FDP_EVENT_MODIFY	= 0x81,	/* implicitly modified RUH */
};

enum {
	NVME_FDP_EVENT_F_PIV	= 1 << 0,	/* PID valid */
	NVME_FDP_EVENT_F_NSIDV	= 1 << 1,	/* NSID valid */
	NVME_FDP_EVENT_F_LV	= 1 << 2,	/* location valid */
};

/* ==========================================================================
 * Log page 0x80: Reservation Notification (64 bytes)
 * ========================================================================== */

struct nvme_pr_log {
	__le64			count;		/* 0: log page count */
	uint8_t			type;		/* 8: NVME_PR_LOG_* */
	uint8_t			nr_pages;	/* 9: available log pages */
	uint8_t			rsvd1[2];
	__le32			nsid;		/* 12 */
	uint8_t			rsvd2[48];
};

enum {
	NVME_PR_LOG_EMPTY_LOG_PAGE			= 0x00,
	NVME_PR_LOG_REGISTRATION_PREEMPTED		= 0x01,
	NVME_PR_LOG_RESERVATION_RELEASED		= 0x02,
	NVME_PR_LOG_RESERVATOIN_PREEMPTED		= 0x03,
	NVME_PR_LOG_RESERVATION_PREEMPTED		= 0x03,
};

/* Reservation notification mask feature bits (bit number). */
enum {
	NVME_PR_NOTIFY_BIT_REG_PREEMPTED		= 1,
	NVME_PR_NOTIFY_BIT_RESV_RELEASED		= 2,
	NVME_PR_NOTIFY_BIT_RESV_PREEMPTED		= 3,
};

/* ==========================================================================
 * Log page 0x81: Sanitize Status (512 bytes)
 * ========================================================================== */

struct nvme_sanitize_log_page {
	__le16			sprog;		/* 0: progress, n/65536 */
	__le16			sstat;		/* 2: sanitize status */
	__le32			scdw10;		/* 4: cdw10 of the sanitize */
	__le32			eto;		/* 8: est. time overwrite, s */
	__le32			etbe;		/* 12: block erase */
	__le32			etce;		/* 16: crypto erase */
	__le32			etond;		/* 20: overwrite, no-dealloc */
	__le32			etbend;		/* 24: block erase, no-dealloc */
	__le32			etcend;		/* 28: crypto erase, no-dealloc */
	__le32			etpvds;		/* 32: post-verif dealloc, 2.1 */
	uint8_t			ssi;		/* 36: sanitize state info, 2.1 */
	uint8_t			rsvd37[475];
};

enum {
	NVME_SANITIZE_SSTAT_STATUS_MASK		= 0x7,
	NVME_SANITIZE_SSTAT_STATUS_NEVER_SANITIZED = 0x0,
	NVME_SANITIZE_SSTAT_STATUS_COMPLETE_SUCCESS = 0x1,
	NVME_SANITIZE_SSTAT_STATUS_IN_PROGESS	= 0x2,
	NVME_SANITIZE_SSTAT_STATUS_COMPLETED_FAILED = 0x3,
	NVME_SANITIZE_SSTAT_STATUS_ND_COMPLETE_SUCCESS = 0x4,
	NVME_SANITIZE_SSTAT_COMPLETED_PASSES_SHIFT = 3,	/* bits 7:3 */
	NVME_SANITIZE_SSTAT_COMPLETED_PASSES_MASK = 0x1f << 3,
	NVME_SANITIZE_SSTAT_GLOBAL_DATA_ERASED	= 1 << 8,
	NVME_SANITIZE_SSTAT_MVCNCLD		= 1 << 9,	/* 2.1 */
};
#define NVME_SANITIZE_SPROG_DONE	0xffff

/* SSI (2.1): bits 3:0 sanitize state, 7:4 failure state. */
#define NVME_SANITIZE_SSI_SANS(v)	((v) & 0xf)
#define NVME_SANITIZE_SSI_FAILS(v)	(((v) >> 4) & 0xf)

/* ==========================================================================
 * Log page 0xbf: Changed Zone List (ZNS, 4096 bytes)
 * ========================================================================== */

struct nvme_zns_changed_zone_log {
	__le16			nrzid;		/* 0: zone ids; 0xffff overflow */
	uint8_t			rsvd2[6];
	__le64			zid[511];	/* 8: zone start LBAs */
};

/* ==========================================================================
 * Features: identifiers
 * ========================================================================== */

enum {
	NVME_FEAT_ARBITRATION	= 0x01,
	NVME_FEAT_POWER_MGMT	= 0x02,
	NVME_FEAT_LBA_RANGE	= 0x03,
	NVME_FEAT_TEMP_THRESH	= 0x04,
	NVME_FEAT_ERR_RECOVERY	= 0x05,
	NVME_FEAT_VOLATILE_WC	= 0x06,
	NVME_FEAT_NUM_QUEUES	= 0x07,
	NVME_FEAT_IRQ_COALESCE	= 0x08,
	NVME_FEAT_IRQ_CONFIG	= 0x09,
	NVME_FEAT_WRITE_ATOMIC	= 0x0a,
	NVME_FEAT_ASYNC_EVENT	= 0x0b,
	NVME_FEAT_AUTO_PST	= 0x0c,
	NVME_FEAT_HOST_MEM_BUF	= 0x0d,
	NVME_FEAT_TIMESTAMP	= 0x0e,
	NVME_FEAT_KATO		= 0x0f,
	NVME_FEAT_HCTM		= 0x10,
	NVME_FEAT_NOPSC		= 0x11,
	NVME_FEAT_RRL		= 0x12,
	NVME_FEAT_PLM_CONFIG	= 0x13,
	NVME_FEAT_PLM_WINDOW	= 0x14,
	NVME_FEAT_LBA_STS_INTERVAL = 0x15,
	NVME_FEAT_HOST_BEHAVIOR	= 0x16,
	NVME_FEAT_SANITIZE	= 0x17,
	NVME_FEAT_ENDURANCE_EVT_CFG = 0x18,
	NVME_FEAT_IOCS_PROFILE	= 0x19,
	NVME_FEAT_SPINUP_CONTROL = 0x1a,
	NVME_FEAT_FDP		= 0x1d,
	NVME_FEAT_FDP_EVENTS	= 0x1e,
	NVME_FEAT_ENH_CTRL_METADATA = 0x7d,	/* NVMe-MI */
	NVME_FEAT_CTRL_METADATA	= 0x7e,		/* NVMe-MI */
	NVME_FEAT_NS_METADATA	= 0x7f,		/* NVMe-MI */
	NVME_FEAT_SW_PROGRESS	= 0x80,
	NVME_FEAT_HOST_ID	= 0x81,
	NVME_FEAT_RESV_MASK	= 0x82,
	NVME_FEAT_RESV_PERSIST	= 0x83,
	NVME_FEAT_WRITE_PROTECT	= 0x84,
	NVME_FEAT_VENDOR_START	= 0xC0,
	NVME_FEAT_VENDOR_END	= 0xFF,
};

/* ==========================================================================
 * Features: dword encodings
 * ========================================================================== */

/* 0x01 Arbitration (cdw11). */
#define NVME_FEAT_ARB_AB(v)	((v) & 0x7)		/* burst, 2^n; 7 = none */
#define NVME_FEAT_ARB_LPW(v)	(((v) >> 8) & 0xff)	/* low prio weight */
#define NVME_FEAT_ARB_MPW(v)	(((v) >> 16) & 0xff)	/* medium */
#define NVME_FEAT_ARB_HPW(v)	(((v) >> 24) & 0xff)	/* high */
#define NVME_FEAT_ARB(ab, lpw, mpw, hpw) \
	(((uint32_t)(ab) & 0x7) | (((uint32_t)(lpw) & 0xff) << 8) | \
	 (((uint32_t)(mpw) & 0xff) << 16) | (((uint32_t)(hpw) & 0xff) << 24))
#define NVME_FEAT_ARB_AB_NO_LIMIT	0x7

/* 0x02 Power Management (cdw11): bits 4:0 PS, 7:5 WH (workload hint). */
#define NVME_FEAT_PM_PS(v)	((v) & 0x1f)
#define NVME_FEAT_PM_WH(v)	(((v) >> 5) & 0x7)
#define NVME_FEAT_PM(ps, wh) \
	(((uint32_t)(ps) & 0x1f) | (((uint32_t)(wh) & 0x7) << 5))

/* 0x03 LBA Range Type: cdw11 bits 5:0 NUM (0's based); data = up to 64
 * entries of 64 bytes. */
struct nvme_lba_range_type {
	uint8_t			type;		/* 0 */
	uint8_t			attributes;	/* 1 */
	uint8_t			rsvd2[14];
	__le64			slba;		/* 16 */
	__le64			nlb;		/* 24: 0's based */
	uint8_t			guid[16];	/* 32 */
	uint8_t			rsvd48[16];
};

enum {
	NVME_LBART_TYPE_GP	= 0x00,	/* general purpose */
	NVME_LBART_TYPE_FS	= 0x01,
	NVME_LBART_TYPE_RAID	= 0x02,
	NVME_LBART_TYPE_CACHE	= 0x03,
	NVME_LBART_TYPE_SWAP	= 0x04,

	NVME_LBART_ATTRIB_TEMP	= 1 << 0,	/* may be overwritten */
	NVME_LBART_ATTRIB_HIDE	= 1 << 1,	/* hidden from the OS */
};

/* 0x04 Temperature Threshold (cdw11):
 *   bits 15:0 TMPTH (Kelvin) | 19:16 TMPSEL | 21:20 THSEL */
enum {
	NVME_TEMP_THRESH_MASK		= 0xffff,
	NVME_TEMP_THRESH_SELECT_SHIFT	= 16,
	NVME_TEMP_THRESH_SELECT_MASK	= 0xf << 16,
	NVME_TEMP_THRESH_TYPE_OVER	= 0x000000,
	NVME_TEMP_THRESH_TYPE_UNDER	= 0x100000,
	NVME_TEMP_THRESH_TYPE_MASK	= 0x300000,
};
/* TMPSEL: 0 composite, 1..8 temperature sensor n, 0xf all sensors. */
#define NVME_TEMP_THRESH_SEL_COMPOSITE	0x0
#define NVME_TEMP_THRESH_SEL_ALL	0xf

/* 0x05 Error Recovery (cdw11): bits 15:0 TLER (100 ms), bit 16 DULBE. */
#define NVME_FEAT_ERR_TLER_MASK		0xffff
#define NVME_FEAT_ERR_DULBE		(1u << 16)

/* 0x06 Volatile Write Cache (cdw11): bit 0 WCE. */
#define NVME_FEAT_VWC_WCE		(1u << 0)

/* 0x07 Number of Queues (cdw11 and result dword 0):
 *   bits 15:0 NSQR/NSQA, 31:16 NCQR/NCQA, all 0's based. */
#define NVME_FEAT_NUM_QUEUES_VAL(nsq, ncq) \
	((((uint32_t)(nsq) - 1) & 0xffff) | \
	 ((((uint32_t)(ncq) - 1) & 0xffff) << 16))
#define NVME_FEAT_NUM_QUEUES_NSQA(r)	(((r) & 0xffff) + 1)
#define NVME_FEAT_NUM_QUEUES_NCQA(r)	((((r) >> 16) & 0xffff) + 1)

/* 0x08 Interrupt Coalescing (cdw11): bits 7:0 THR (0's based entries),
 * 15:8 TIME (100 us units). */
#define NVME_FEAT_IRQ_COALESCE_VAL(thr, time_100us) \
	(((uint32_t)(thr) & 0xff) | (((uint32_t)(time_100us) & 0xff) << 8))

/* 0x09 Interrupt Vector Configuration (cdw11): bits 15:0 IV, bit 16 CD
 * (coalescing disable). */
#define NVME_FEAT_IRQ_CONFIG_IV_MASK	0xffff
#define NVME_FEAT_IRQ_CONFIG_CD		(1u << 16)

/* 0x0a Write Atomicity Normal (cdw11): bit 0 DN (disable normal). */
#define NVME_FEAT_WRITE_ATOMIC_DN	(1u << 0)

/* 0x0b Asynchronous Event Configuration: see NVME_AEN_CFG_* below. */

/* 0x0c Autonomous Power State Transition: cdw11 bit 0 APSTE; data =
 * 32 entries, one per power state. */
struct nvme_feat_auto_pst {
	__le64 entries[32];
};
#define NVME_FEAT_APST_APSTE		(1u << 0)
/* Entry: bits 7:3 ITPS (idle transition power state), 31:8 ITPT (idle
 * time prior to transition, ms). */
#define NVME_APST_ENTRY_ITPS_SHIFT	3
#define NVME_APST_ENTRY_ITPS_MASK	(0x1fULL << 3)
#define NVME_APST_ENTRY_ITPT_SHIFT	8
#define NVME_APST_ENTRY_ITPT_MASK	(0xffffffULL << 8)
#define NVME_APST_ENTRY(itps, itpt_ms) \
	((((uint64_t)(itps) & 0x1f) << NVME_APST_ENTRY_ITPS_SHIFT) | \
	 (((uint64_t)(itpt_ms) & 0xffffff) << NVME_APST_ENTRY_ITPT_SHIFT))

/*
 * 0x0d Host Memory Buffer.
 *   cdw11  bit 0 EHM (enable) | bit 1 MR (memory return)
 *   cdw12  HSIZE: buffer size in memory pages (CC.MPS)
 *   cdw13  HMDLLA: descriptor list address, low 32 bits (16 B aligned)
 *   cdw14  HMDLUA: descriptor list address, high 32 bits
 *   cdw15  HMDLEC: descriptor list entry count
 */
enum {
	NVME_HOST_MEM_ENABLE	= (1 << 0),
	NVME_HOST_MEM_RETURN	= (1 << 1),
};

/* HMB descriptor list entry (16 bytes). */
struct nvme_host_mem_buf_desc {
	__le64			addr;		/* 0: page aligned (CC.MPS) */
	__le32			size;		/* 8: memory pages */
	uint32_t		rsvd;
};

/* Get Features, HMB attributes data (4096 bytes). */
struct nvme_host_mem_buf_attrs {
	__le32			hsize;		/* 0: memory pages */
	__le32			hmdlal;		/* 4 */
	__le32			hmdlau;		/* 8 */
	__le32			hmdlec;		/* 12 */
	uint8_t			rsvd16[4080];
};

/* 0x0e Timestamp: data = struct nvme_timestamp. */
enum {
	NVME_TIMESTAMP_ATTR_SYNCH	= 1 << 0,	/* may have stopped */
	NVME_TIMESTAMP_ORIGIN_SHIFT	= 1,		/* bits 3:1 */
	NVME_TIMESTAMP_ORIGIN_MASK	= 0x7 << 1,
	NVME_TIMESTAMP_ORIGIN_RESET	= 0 << 1,	/* zeroed by reset */
	NVME_TIMESTAMP_ORIGIN_SET	= 1 << 1,	/* set by host */
};

/* 0x0f Keep Alive Timer (cdw11): KATO in milliseconds (KAS granularity). */

/* 0x10 Host Controlled Thermal Management (cdw11):
 *   bits 15:0 TMT2, 31:16 TMT1 (Kelvin, 0 = disabled). */
#define NVME_FEAT_HCTM_TMT2_MASK	0xffff
#define NVME_FEAT_HCTM_TMT1_SHIFT	16
#define NVME_FEAT_HCTM(tmt1, tmt2) \
	((((uint32_t)(tmt1) & 0xffff) << 16) | ((uint32_t)(tmt2) & 0xffff))

/* 0x11 Non-Operational Power State Config (cdw11): bit 0 NOPPME. */
#define NVME_FEAT_NOPSC_NOPPME		(1u << 0)

/* 0x12 Read Recovery Level (cdw11 bits 15:0 NVMSETID, cdw12 bits 3:0). */
#define NVME_FEAT_RRL_MASK		0xf

/* 0x13 Predictable Latency Mode Config (cdw11 bits 15:0 NVMSETID,
 * cdw12 bit 0 LPE); data 512 bytes. */
struct nvme_plm_config {
	__le16			ee;		/* 0: enable event */
	uint8_t			rsvd2[30];
	__le64			dtwinrt;	/* 32: DTWIN reads threshold */
	__le64			dtwinwt;	/* 40: DTWIN writes threshold */
	__le64			dtwintt;	/* 48: DTWIN time threshold */
	uint8_t			rsvd56[456];
};
#define NVME_FEAT_PLM_LPE		(1u << 0)

/* 0x14 Predictable Latency Mode Window (cdw11 bits 15:0 NVMSETID,
 * cdw12 bits 2:0 WS window select). */
enum {
	NVME_FEAT_PLM_WINDOW_DTWIN	= 0x1,
	NVME_FEAT_PLM_WINDOW_NDWIN	= 0x2,
};

/* 0x15 LBA Status Information Attributes (cdw11):
 *   bits 15:0 LSIRI report interval (100 ms), 31:16 LSIPI poll interval */
#define NVME_FEAT_LBAS_LSIRI_MASK	0xffff
#define NVME_FEAT_LBAS_LSIPI_SHIFT	16

/* 0x16 Host Behavior Support: data 512 bytes. */
struct nvme_feat_host_behavior {
	uint8_t acre;		/* 0: advanced command retry enable */
	uint8_t etdas;		/* 1: extended telemetry data area 4 */
	uint8_t lbafee;		/* 2: LBA format extension enable */
	uint8_t hdisns;		/* 3: host dispersed namespace, 2.1 */
	__le16 cdfe;		/* 4: copy descriptor formats enable, 2.1 */
	uint8_t resv1[506];
};

enum {
	NVME_ENABLE_ACRE	= 1,
	NVME_ENABLE_ETDAS	= 1,
	NVME_ENABLE_LBAFEE	= 1,
	NVME_ENABLE_HDISNS	= 1,
};

/* 0x17 Sanitize Config (cdw11): bit 0 NODRM (no-deallocate response
 * mode). */
#define NVME_FEAT_SANITIZE_NODRM	(1u << 0)

/* 0x18 Endurance Group Event Configuration (cdw11):
 *   bits 15:0 ENDGID, 23:16 EGWARN (critical warning enable) */
#define NVME_FEAT_EG_ENDGID_MASK	0xffff
#define NVME_FEAT_EG_EGWARN_SHIFT	16

/* 0x19 I/O Command Set Profile (cdw11): bits 8:0 IOCSCI, an index into
 * the Identify I/O Command Set data structure (CNS 0x1c). */
#define NVME_FEAT_IOCSP_IOCSCI_MASK	0x1ff

/* 0x1a Spinup Control (cdw11): bit 0 SPINUPC. */
#define NVME_FEAT_SPINUP_CONTROL_EN	(1u << 0)

/* 0x1d Flexible Data Placement (cdw11 bits 15:0 ENDGID;
 * cdw12 bit 0 FDPE, bits 15:8 FDPCIDX configuration index). */
struct nvme_fdp_config {
	uint8_t			flags;
#define FDPCFG_FDPE	(1U << 0)
	uint8_t			fdpcidx;
	__le16			reserved;
};

/* 0x1e FDP Events (cdw11 bits 15:0 PHNDL placement handle, 23:16 NOET
 * number of event types; cdw12 bit 0 FDPEE enable).  Get returns NOET
 * supported event descriptors, Set takes a list of event type bytes. */
struct nvme_fdp_supported_event {
	uint8_t			evt;		/* 0: event type */
	uint8_t			evta;		/* 1: bit 0 enabled */
};
#define NVME_FEAT_FDP_EVENTS_NOET_SHIFT	16
#define NVME_FEAT_FDP_EVENTS_ENABLE	(1u << 0)

/* 0x80 Software Progress Marker (cdw11 bits 7:0 PBSLC). */
/* 0x81 Host Identifier: cdw11 bit 0 EXHID (128-bit); data 8 or 16 B. */
#define NVME_FEAT_HOST_ID_EXHID		(1u << 0)

/* 0x82 Reservation Notification Mask (cdw11). */
enum {
	NVME_FEAT_RESV_MASK_REGPRE	= 1 << 1,	/* reg preempted */
	NVME_FEAT_RESV_MASK_RESREL	= 1 << 2,	/* released */
	NVME_FEAT_RESV_MASK_RESPRE	= 1 << 3,	/* preempted */
};

/* 0x83 Reservation Persistence (cdw11): bit 0 PTPL. */
#define NVME_FEAT_RESV_PERSIST_PTPL	(1u << 0)

/* 0x84 Namespace Write Protection Config (cdw11 bits 2:0 WPS). */
enum {
	NVME_NS_NO_WRITE_PROTECT = 0,
	NVME_NS_WRITE_PROTECT,
	NVME_NS_WRITE_PROTECT_POWER_CYCLE,
	NVME_NS_WRITE_PROTECT_PERMANENT,
};

/* ==========================================================================
 * Asynchronous events
 * ========================================================================== */

/*
 * Asynchronous Event Request completion, result dword 0:
 *   bits 2:0   AET  event type (NVME_AER_*)
 *   bits 15:8  AEI  event information (NVME_AER_NOTICE_*, ...)
 *   bits 23:16 LID  log page to read to clear the event
 */
#define NVME_AER_RESULT_TYPE(r)		((r) & 0x7)
#define NVME_AER_RESULT_INFO(r)		(((r) >> 8) & 0xff)
#define NVME_AER_RESULT_LID(r)		(((r) >> 16) & 0xff)
#define NVME_AER_RESULT(type, info, lid) \
	(((uint32_t)(type) & 0x7) | (((uint32_t)(info) & 0xff) << 8) | \
	 (((uint32_t)(lid) & 0xff) << 16))

enum {
	NVME_AER_ERROR			= 0,
	NVME_AER_SMART			= 1,
	NVME_AER_NOTICE			= 2,
	NVME_AER_IMMEDIATE		= 3,	/* 2.1 */
	NVME_AER_ONESHOT		= 4,	/* 2.1 */
	NVME_AER_CSS			= 6,	/* I/O command set specific */
	NVME_AER_VS			= 7,
};

/* Error status (AET 0) information. */
enum {
	NVME_AER_ERROR_INVALID_DB_REG	= 0x00,	/* write to invalid doorbell */
	NVME_AER_ERROR_INVALID_DB_VAL	= 0x01,	/* invalid doorbell value */
	NVME_AER_ERROR_DIAG_FAILURE	= 0x02,
	NVME_AER_ERROR_PERSIST_INT_ERR	= 0x03,
	NVME_AER_ERROR_TRANSIENT_INT_ERR = 0x04,
	NVME_AER_ERROR_FW_IMAGE_LOAD	= 0x05,
};

/* SMART / health status (AET 1) information. */
enum {
	NVME_AER_SMART_SUBSYSTEM_RELIABILITY	= 0x00,
	NVME_AER_SMART_TEMPERATURE_THRESHOLD	= 0x01,
	NVME_AER_SMART_SPARE_THRESHOLD		= 0x02,
};

/* Notice (AET 2) information. */
enum {
	NVME_AER_NOTICE_NS_CHANGED	= 0x00,	/* log 0x04 */
	NVME_AER_NOTICE_FW_ACT_STARTING = 0x01,	/* log 0x03 */
	NVME_AER_NOTICE_TELEMETRY	= 0x02,	/* log 0x08 */
	NVME_AER_NOTICE_ANA		= 0x03,	/* log 0x0c */
	NVME_AER_NOTICE_PLM_AGGREGATE	= 0x04,	/* log 0x0b */
	NVME_AER_NOTICE_LBA_STATUS	= 0x05,	/* log 0x0e */
	NVME_AER_NOTICE_ENDGRP_AGGREGATE = 0x06, /* log 0x0f */
	NVME_AER_NOTICE_ZONE_DESCR_CHANGED = 0xef, /* ZNS, log 0xbf */
	NVME_AER_NOTICE_DISC_CHANGED	= 0xf0,	/* fabrics discovery */
};

/* Immediate (AET 3, 2.1) information: no log page, nothing to clear;
 * enabled by AEC/OAES bit 15. */
enum {
	NVME_AER_IMMEDIATE_NSS_SHUTDOWN	= 0x00,	/* normal NVM subsys shutdown */
};

/* One-shot (AET 4, 2.1) information: reported once, nothing to clear;
 * enabled by AEC/OAES bit 16. */
enum {
	NVME_AER_ONESHOT_TEMP_HYSTERESIS = 0x00, /* temperature hysteresis
						  * recovery */
};

/* I/O command set specific (AET 6) information. */
enum {
	NVME_AEN_RESV_LOG_PAGE_AVALIABLE	= 0x00,
	NVME_AEN_RESV_LOG_PAGE_AVAILABLE	= 0x00,	/* log 0x80 */
	NVME_AER_CSS_SANITIZE_COMPLETED		= 0x01,	/* log 0x81 */
	NVME_AER_CSS_SANITIZE_DEALLOC_UNEXP	= 0x02,	/* log 0x81 */
};

/*
 * Asynchronous Event Configuration (feature 0x0b, cdw11) and Identify
 * Controller OAES share bit positions 8 and up; bits 7:0 enable SMART
 * critical warnings (NVME_SMART_CRIT_*).
 */
enum {
	NVME_AEN_BIT_NS_ATTR		= 8,
	NVME_AEN_BIT_FW_ACT		= 9,
	NVME_AEN_BIT_TELEMETRY		= 10,
	NVME_AEN_BIT_ANA_CHANGE		= 11,
	NVME_AEN_BIT_PLM_AGGREGATE	= 12,
	NVME_AEN_BIT_LBA_STATUS		= 13,
	NVME_AEN_BIT_ENDGRP_AGGREGATE	= 14,
	NVME_AEN_BIT_NSS_SHUTDOWN	= 15,
	NVME_AEN_BIT_TEMP_HYSTERESIS	= 16,	/* 2.1 */
	NVME_AEN_BIT_ZONE_DESCR_CHANGED	= 27,	/* ZNS */
	NVME_AEN_BIT_DISC_CHANGE	= 31,
};

#define NVME_AEN_CFG_SMART_MASK		0xffu
#define NVME_AEN_CFG_NS_ATTR		(1u << NVME_AEN_BIT_NS_ATTR)
#define NVME_AEN_CFG_FW_ACT		(1u << NVME_AEN_BIT_FW_ACT)
#define NVME_AEN_CFG_TELEMETRY		(1u << NVME_AEN_BIT_TELEMETRY)
#define NVME_AEN_CFG_ANA_CHANGE		(1u << NVME_AEN_BIT_ANA_CHANGE)
#define NVME_AEN_CFG_PLM_AGGREGATE	(1u << NVME_AEN_BIT_PLM_AGGREGATE)
#define NVME_AEN_CFG_LBA_STATUS		(1u << NVME_AEN_BIT_LBA_STATUS)
#define NVME_AEN_CFG_ENDGRP_AGGREGATE	(1u << NVME_AEN_BIT_ENDGRP_AGGREGATE)
#define NVME_AEN_CFG_NSS_SHUTDOWN	(1u << NVME_AEN_BIT_NSS_SHUTDOWN)
#define NVME_AEN_CFG_TEMP_HYSTERESIS	(1u << NVME_AEN_BIT_TEMP_HYSTERESIS)
#define NVME_AEN_CFG_ZONE_DESCR_CHANGED	(1u << NVME_AEN_BIT_ZONE_DESCR_CHANGED)
#define NVME_AEN_CFG_DISC_CHANGE	(1u << NVME_AEN_BIT_DISC_CHANGE)

/* ==========================================================================
 * Directives: identify, streams, data placement
 * ========================================================================== */

enum {
	NVME_DIR_IDENTIFY		= 0x00,	/* directive type */
	NVME_DIR_STREAMS		= 0x01,
	NVME_DIR_DATA_PLACEMENT		= 0x02,	/* FDP */
	NVME_DIR_SND_ID_OP_ENABLE	= 0x01,	/* send, identify: enable */
	NVME_DIR_SND_ST_OP_REL_ID	= 0x01,	/* send, streams: rel id */
	NVME_DIR_SND_ST_OP_REL_RSC	= 0x02,	/* send, streams: rel res */
	NVME_DIR_RCV_ID_OP_PARAM	= 0x01,	/* recv, identify: params */
	NVME_DIR_RCV_ST_OP_PARAM	= 0x01,	/* recv, streams: params */
	NVME_DIR_RCV_ST_OP_STATUS	= 0x02,	/* recv, streams: status */
	NVME_DIR_RCV_ST_OP_RESOURCE	= 0x03,	/* recv, streams: allocate */
	NVME_DIR_ENDIR			= 0x01,	/* endir: enable directive */
};

/* Identify directive, return parameters (4096 bytes): bitmaps indexed by
 * directive type. */
struct nvme_id_directives {
	uint8_t			supported[32];	/* 0 */
	uint8_t			enabled[32];	/* 32 */
	uint8_t			rpr[32];	/* 64: persistent across reset */
	uint8_t			rsvd96[4000];
};

/* Streams directive, return parameters (32 bytes). */
struct streams_directive_params {
	__le16	msl;		/* 0: max streams limit */
	__le16	nssa;		/* 2: subsystem streams available */
	__le16	nsso;		/* 4: subsystem streams open */
	uint8_t	rsvd[10];
	__le32	sws;		/* 16: stream write size, blocks */
	__le16	sgs;		/* 20: stream granularity, SWS units */
	__le16	nsa;		/* 22: namespace streams allocated */
	__le16	nso;		/* 24: namespace streams open */
	uint8_t	rsvd2[6];
};

/* Streams directive, get status (header 2 bytes + open stream ids). */
struct nvme_streams_directive_status {
	__le16			osc;		/* open stream count */
	__le16			sid[];
};

/* ==========================================================================
 * Zoned Namespace: zone report
 * ========================================================================== */

/* Zone type (zt bits 3:0). */
enum {
	NVME_ZONE_TYPE_SEQWRITE_REQ	= 0x2,
};
#define NVME_ZONE_TYPE_MASK	0xf

/* Zone state (zs bits 7:4). */
enum {
	NVME_ZONE_STATE_EMPTY		= 0x1,
	NVME_ZONE_STATE_IMP_OPEN	= 0x2,
	NVME_ZONE_STATE_EXP_OPEN	= 0x3,
	NVME_ZONE_STATE_CLOSED		= 0x4,
	NVME_ZONE_STATE_READONLY	= 0xd,
	NVME_ZONE_STATE_FULL		= 0xe,
	NVME_ZONE_STATE_OFFLINE		= 0xf,
};
#define NVME_ZONE_STATE_SHIFT	4
#define NVME_ZONE_STATE(zs)	(((zs) >> NVME_ZONE_STATE_SHIFT) & 0xf)

/* Zone attributes (za). */
enum {
	NVME_ZONE_ATTR_ZFC	= 1 << 0,	/* finished by controller */
	NVME_ZONE_ATTR_FZR	= 1 << 1,	/* finish zone recommended */
	NVME_ZONE_ATTR_RZR	= 1 << 2,	/* reset zone recommended */
	NVME_ZONE_ATTR_ZRWAV	= 1 << 3,	/* ZRWA valid */
	NVME_ZONE_ATTR_ZDEV	= 1 << 7,	/* descriptor extension valid */
};

/* Zone descriptor (64 bytes). */
struct nvme_zone_descriptor {
	uint8_t		zt;		/* 0: zone type */
	uint8_t		zs;		/* 1: zone state (7:4) */
	uint8_t		za;		/* 2: zone attributes */
	uint8_t		zai;		/* 3: zone attributes information */
	uint8_t		rsvd4[4];
	__le64		zcap;		/* 8: zone capacity, blocks */
	__le64		zslba;		/* 16: zone start LBA */
	__le64		wp;		/* 24: write pointer */
	uint8_t		rsvd32[32];
};

/* Report Zones data: header 64 bytes + descriptors.  With the extended
 * report each descriptor is followed by ZDES * 64 bytes of extension. */
struct nvme_zone_report {
	__le64		nr_zones;	/* 0: zones matching (or returned) */
	uint8_t		resv8[56];
	struct nvme_zone_descriptor entries[];
};

/* ==========================================================================
 * Protection information
 * ========================================================================== */

/*
 * A T10 PI-capable namespace can be formatted with different protection
 * schemes; 0 through 3 are defined:
 *   Type 0: no protection information
 *   Type 1: guard and reference tag (= low LBA bits)
 *   Type 2: guard and reference tag (seeded by the command)
 *   Type 3: guard only (reference tag not checked)
 */
enum t10_dif_type {
	T10_PI_TYPE0_PROTECTION = 0x0,
	T10_PI_TYPE1_PROTECTION = 0x1,
	T10_PI_TYPE2_PROTECTION = 0x2,
	T10_PI_TYPE3_PROTECTION = 0x3,
};

/* 16b guard protection information tuple (8 bytes, big-endian). */
struct t10_pi_tuple {
	__be16 guard_tag;	/* CRC-16 T10-DIF of the block data */
	__be16 app_tag;		/* opaque storage */
	__be32 ref_tag;		/* target LBA or indirect LBA */
};

/* Escape values: an app tag of all ones (and, for type 3, a ref tag of all
 * ones) disables checking of that block. */
#define T10_PI_APP_ESCAPE cpu_to_be16(0xffff)
#define T10_PI_REF_ESCAPE cpu_to_be32(0xffffffff)

/* 32b guard protection information (16 bytes, big-endian): CRC-32C guard,
 * 16-bit app tag, 80 bits of storage + reference tag. */
struct nvme_crc32_pi_tuple {
	__be32 guard_tag;
	__be16 app_tag;
	uint8_t storage_ref_tag[10];
};

/* 64b guard protection information (16 bytes, big-endian): CRC-64 NVMe
 * guard, 16-bit app tag, 48 bits of storage + reference tag. */
struct crc64_pi_tuple {
	__be64 guard_tag;
	__be16 app_tag;
	uint8_t ref_tag[6];
};

/* Bits 0-47 of a 64-bit value (64b guard reference tag). */
static inline uint64_t nvme_lower_48_bits(uint64_t n)
{
	return n & ((1ULL << 48) - 1);
}

/* ==========================================================================
 * Temperature helpers (every NVMe temperature is in Kelvin)
 * ========================================================================== */

#define NVME_TEMP_KELVIN_OFFSET	273

static inline int nvme_temp_k_to_c(uint16_t kelvin)
{
	return (int)kelvin - NVME_TEMP_KELVIN_OFFSET;
}

static inline uint16_t nvme_temp_c_to_k(int celsius)
{
	return (uint16_t)(celsius + NVME_TEMP_KELVIN_OFFSET);
}

/* SMART log composite temperature (bytes 1-2, misaligned). */
static inline uint16_t nvme_smart_temp_k(const struct nvme_smart_log *log)
{
	return get_unaligned_le16(log->temperature);
}

/* ==========================================================================
 * Layout checks
 * ========================================================================== */

#define NVME_SPEC_OFF(type, member)	__builtin_offsetof(type, member)
#define NVME_SPEC_CHECK_OFF(type, member, off) \
	BUILD_BUG_ON(NVME_SPEC_OFF(type, member) != (off))

/* Never called at run time for any effect: every check is compile time.
 * The core init calls it so the checks are compiled with the driver. */
static inline void _nvme_spec_check(void)
{
	/* Data pointers and queue entries. */
	BUILD_BUG_ON(sizeof(struct nvme_sgl_desc) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_keyed_sgl_desc) != 16);
	BUILD_BUG_ON(sizeof(union nvme_data_ptr) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_completion) != NVME_CQE_SIZE);
	NVME_SPEC_CHECK_OFF(struct nvme_completion, sq_head, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_completion, sq_id, 10);
	NVME_SPEC_CHECK_OFF(struct nvme_completion, command_id, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_completion, status, 14);

	BUILD_BUG_ON(sizeof(struct nvme_command) != NVME_SQE_SIZE);
	BUILD_BUG_ON(sizeof(struct nvme_common_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_rw_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_identify) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_features) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_create_cq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_create_sq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_delete_queue) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_download_firmware) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_format_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_dsm_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_write_zeroes_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_zone_mgmt_send_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_zone_mgmt_recv_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_abort_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_get_log_page_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_dbbuf) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_directive_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_io_mgmt_recv_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_io_mgmt_send_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_copy_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_cancel_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_sanitize_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_security_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_get_lba_status_cmd) != 64);

	/* Common dword positions. */
	NVME_SPEC_CHECK_OFF(struct nvme_common_command, nsid, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_common_command, metadata, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_common_command, dptr, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_common_command, cdw10, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_common_command, cdw15, 60);
	NVME_SPEC_CHECK_OFF(struct nvme_common_command, cdws, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, slba, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, length, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, control, 50);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, dsmgmt, 52);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, reftag, 56);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, lbat, 60);
	NVME_SPEC_CHECK_OFF(struct nvme_rw_command, lbatm, 62);
	NVME_SPEC_CHECK_OFF(struct nvme_write_zeroes_cmd, control, 50);
	NVME_SPEC_CHECK_OFF(struct nvme_dsm_cmd, nr, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_dsm_cmd, attributes, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_command, sdlba, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_command, nr_range, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_command, control, 50);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_command, dspec, 54);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_command, reftag, 56);
	NVME_SPEC_CHECK_OFF(struct nvme_cancel_cmd, sqid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_cancel_cmd, cid, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_cancel_cmd, cdw11, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_identify, cns, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_identify, ctrlid, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_identify, cnssid, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_identify, csi, 47);
	NVME_SPEC_CHECK_OFF(struct nvme_identify, uuid, 56);
	NVME_SPEC_CHECK_OFF(struct nvme_features, fid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_features, dword11, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_features, dword15, 60);
	NVME_SPEC_CHECK_OFF(struct nvme_create_cq, prp1, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_create_cq, cqid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_create_cq, qsize, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_create_cq, cq_flags, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_create_cq, irq_vector, 46);
	NVME_SPEC_CHECK_OFF(struct nvme_create_sq, prp1, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_create_sq, sqid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_create_sq, sq_flags, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_create_sq, cqid, 46);
	NVME_SPEC_CHECK_OFF(struct nvme_create_sq, nvmsetid, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_delete_queue, qid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_abort_cmd, sqid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_abort_cmd, cid, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_download_firmware, dptr, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_download_firmware, numd, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_download_firmware, offset, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_format_cmd, cdw10, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, lid, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, lsp, 41);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, numdl, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, numdu, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, lsi, 46);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, lpo, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, uuid, 56);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, ot, 58);
	NVME_SPEC_CHECK_OFF(struct nvme_get_log_page_command, csi, 59);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_send_cmd, slba, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_send_cmd, zsa, 52);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_send_cmd, select_all, 53);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_recv_cmd, numd, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_recv_cmd, zra, 52);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_recv_cmd, zrasf, 53);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_mgmt_recv_cmd, pr, 54);
	NVME_SPEC_CHECK_OFF(struct nvme_io_mgmt_recv_cmd, mo, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_io_mgmt_recv_cmd, mos, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_io_mgmt_recv_cmd, numd, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_io_mgmt_send_cmd, mos, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_directive_cmd, numd, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_directive_cmd, doper, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_directive_cmd, dtype, 45);
	NVME_SPEC_CHECK_OFF(struct nvme_directive_cmd, dspec, 46);
	NVME_SPEC_CHECK_OFF(struct nvme_directive_cmd, endir, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_directive_cmd, tdtype, 49);
	NVME_SPEC_CHECK_OFF(struct nvme_dbbuf, prp1, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_dbbuf, prp2, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_sanitize_cmd, cdw10, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_sanitize_cmd, ovrpat, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_security_cmd, nssf, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_security_cmd, secp, 43);
	NVME_SPEC_CHECK_OFF(struct nvme_security_cmd, tl, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_get_lba_status_cmd, slba, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_get_lba_status_cmd, mndw, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_get_lba_status_cmd, rl, 52);
	NVME_SPEC_CHECK_OFF(struct nvme_get_lba_status_cmd, atype, 55);

	/* Command data structures. */
	BUILD_BUG_ON(sizeof(struct nvme_dsm_range) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_copy_range) != 32);
	BUILD_BUG_ON(sizeof(struct nvme_copy_range_f1) != 40);
	BUILD_BUG_ON(sizeof(struct nvme_copy_range_f2) != 32);
	BUILD_BUG_ON(sizeof(struct nvme_copy_range_f3) != 40);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_range, slba, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_range, eilbrt, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_range_f1, elbt, 26);
	NVME_SPEC_CHECK_OFF(struct nvme_copy_range_f3, elbt, 26);
	BUILD_BUG_ON(sizeof(struct nvme_host_mem_buf_desc) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_host_mem_buf_attrs) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_lba_status) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_lba_status_desc) != 16);
	NVME_SPEC_CHECK_OFF(struct nvme_lba_status_desc, status, 13);
	BUILD_BUG_ON(sizeof(struct nvmet_pr_register_data) != 16);
	BUILD_BUG_ON(sizeof(struct nvmet_pr_acquire_data) != 16);
	BUILD_BUG_ON(sizeof(struct nvmet_pr_release_data) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_reservation_status) != 24);
	BUILD_BUG_ON(sizeof(struct nvme_registered_ctrl) != 24);
	BUILD_BUG_ON(sizeof(struct nvme_reservation_status_ext) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_registered_ctrl_ext) != 64);
	NVME_SPEC_CHECK_OFF(struct nvme_reservation_status, ptpls, 9);
	NVME_SPEC_CHECK_OFF(struct nvme_registered_ctrl_ext, hostid, 16);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_ruh_status) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_ruh_status_desc) != 32);
	BUILD_BUG_ON(sizeof(struct nvme_ns_mgmt_host_sw_specified) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_ns_mgmt_host_sw_specified, flbas, 26);
	NVME_SPEC_CHECK_OFF(struct nvme_ns_mgmt_host_sw_specified, anagrpid, 92);
	NVME_SPEC_CHECK_OFF(struct nvme_ns_mgmt_host_sw_specified, lbstm, 384);
	NVME_SPEC_CHECK_OFF(struct nvme_ns_mgmt_host_sw_specified, nphndls, 392);
	NVME_SPEC_CHECK_OFF(struct nvme_ns_mgmt_host_sw_specified, phndl, 512);
	BUILD_BUG_ON(NVME_SGL_DESC_SIZE != sizeof(struct nvme_sgl_desc));
	BUILD_BUG_ON(NVME_PRP_ENTRY_SIZE != sizeof(uint64_t));

	/* Identify: power state, LBA format. */
	BUILD_BUG_ON(sizeof(struct nvme_id_power_state) != 32);
	NVME_SPEC_CHECK_OFF(struct nvme_id_power_state, flags, 3);
	NVME_SPEC_CHECK_OFF(struct nvme_id_power_state, entry_lat, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_id_power_state, exit_lat, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_id_power_state, idle_power, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_id_power_state, active_power, 20);
	BUILD_BUG_ON(sizeof(struct nvme_lbaf) != 4);

	/* Identify Controller. */
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl) != NVME_IDENTIFY_DATA_SIZE);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, vid, 0);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ssvid, 2);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, sn, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mn, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, fr, 64);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, rab, 72);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ieee, 73);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, cmic, 76);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mdts, 77);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, cntlid, 78);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ver, 80);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, rtd3r, 84);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, rtd3e, 88);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, oaes, 92);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ctratt, 96);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, rrls, 100);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, cntrltype, 111);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, fguid, 112);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, crdt1, 128);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, crdt3, 132);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, nvmsr, 253);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mec, 255);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, oacs, 256);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, acl, 258);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, aerl, 259);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, frmw, 260);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, lpa, 261);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, elpe, 262);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, npss, 263);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, avscc, 264);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, apsta, 265);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, wctemp, 266);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, cctemp, 268);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mtfa, 270);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, hmpre, 272);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, hmmin, 276);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, tnvmcap, 280);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, unvmcap, 296);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, rpmbs, 312);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, edstt, 316);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, dsto, 318);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, fwug, 319);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, kas, 320);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, hctma, 322);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mntmt, 324);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mxtmt, 326);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, sanicap, 328);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, hmminds, 332);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, hmmaxd, 336);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, nvmsetidmax, 338);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, endgidmax, 340);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, anatt, 342);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, anacap, 343);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, anagrpmax, 344);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, nanagrpid, 348);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, pels, 352);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, domainid, 356);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, megcap, 368);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, tmpthha, 384);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, cqt, 386);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, sqes, 512);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, cqes, 513);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, maxcmd, 514);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, nn, 516);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, oncs, 520);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, fuses, 522);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, fna, 524);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, vwc, 525);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, awun, 526);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, awupf, 528);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, nvscc, 530);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, nwpc, 531);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, acwu, 532);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ocfs, 534);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, sgls, 536);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, mnan, 540);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, maxdna, 544);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, maxcna, 560);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, subnqn, 768);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ioccsz, 1792);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, iorcsz, 1796);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, icdoff, 1800);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ctrattr, 1802);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, msdbd, 1803);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, ofcs, 1804);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, dctype, 1806);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, psd, 2048);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl, vs, 3072);

	/* Identify Namespace. */
	BUILD_BUG_ON(sizeof(struct nvme_id_ns) != NVME_IDENTIFY_DATA_SIZE);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nsze, 0);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, ncap, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nuse, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nsfeat, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nlbaf, 25);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, flbas, 26);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, mc, 27);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, dpc, 28);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, dps, 29);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nmic, 30);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, rescap, 31);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, fpi, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, dlfeat, 33);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nawun, 34);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nawupf, 36);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nacwu, 38);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nabsn, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nabo, 42);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nabspf, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, noiob, 46);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nvmcap, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, npwg, 64);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, npwa, 66);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, npdg, 68);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, npda, 70);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nows, 72);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, mssrl, 74);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, mcl, 76);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, msrc, 80);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, kpios, 81);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nulbaf, 82);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, kpiodaag, 84);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, anagrpid, 92);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nsattr, 99);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nvmsetid, 100);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, endgid, 102);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, nguid, 104);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, eui64, 120);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, lbaf, 128);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns, vs, 384);

	BUILD_BUG_ON(sizeof(struct nvme_id_ns_cs_indep) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_cs_indep, anagrpid, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_cs_indep, nsattr, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_cs_indep, nvmsetid, 10);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_cs_indep, endgid, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_cs_indep, nstat, 14);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_nvm) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_nvm, pic, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_nvm, pifa, 9);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_nvm, elbaf, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_nvm, npdgl, 268);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_nvm, npdal, 272);
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_nvm) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl_nvm, dmrsl, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl_nvm, dmsl, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl_nvm, kpiocap, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl_nvm, aocs, 18);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl_nvm, ver, 20);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ctrl_nvm, lbamqf, 24);
	BUILD_BUG_ON(sizeof(struct nvme_zns_lbafe) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_zns) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, mar, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, mor, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, rrl, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, frl, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, numzrwa, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, zrwafg, 48);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, zrwacap, 52);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, lbafe, 2816);
	NVME_SPEC_CHECK_OFF(struct nvme_id_ns_zns, vs, 3840);
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_zns) != 4096);

	/* Identify lists. */
	BUILD_BUG_ON(sizeof(struct nvme_ns_id_desc) != 4);
	BUILD_BUG_ON(sizeof(struct nvme_ns_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_ctrl_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_primary_ctrl_cap) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_primary_ctrl_cap, vqfrt, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_primary_ctrl_cap, vifrt, 64);
	BUILD_BUG_ON(sizeof(struct nvme_secondary_ctrl) != 32);
	BUILD_BUG_ON(sizeof(struct nvme_secondary_ctrl_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_granularity_desc) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns_granularity_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_id_uuid_list_entry) != 32);
	BUILD_BUG_ON(sizeof(struct nvme_id_uuid_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_nvmset_attr) != 128);
	BUILD_BUG_ON(sizeof(struct nvme_id_nvmset_list) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_nvmset_attr, tnvmsetcap, 16);
	BUILD_BUG_ON(sizeof(struct nvme_id_domain_attr) != 128);
	BUILD_BUG_ON(sizeof(struct nvme_id_domain_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_id_endurance_group_list) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_id_iocs) != 4096);

	/* Log pages. */
	BUILD_BUG_ON(sizeof(struct nvme_supported_log) != 1024);
	BUILD_BUG_ON(sizeof(struct nvme_error_slot) != 64);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, sqid, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, status_field, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, lba, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, nsid, 24);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, trtype, 29);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, cs, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, trtype_spec_info, 40);
	NVME_SPEC_CHECK_OFF(struct nvme_error_slot, log_page_version, 63);
	BUILD_BUG_ON(sizeof(struct nvme_smart_log) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, temperature, 1);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, endu_grp_crit_warn_sumry, 6);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, data_units_read, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, power_on_hours, 128);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, num_err_log_entries, 176);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, warning_temp_time, 192);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, temp_sensor, 200);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, thm_temp1_trans_count, 216);
	NVME_SPEC_CHECK_OFF(struct nvme_smart_log, thm_temp2_total_time, 228);
	BUILD_BUG_ON(sizeof(struct nvme_fw_slot_info_log) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_fw_slot_info_log, frs, 8);
	BUILD_BUG_ON(sizeof(struct nvme_changed_ns_list_log) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_effects_log) != 4096);
	NVME_SPEC_CHECK_OFF(struct nvme_effects_log, iocs, 1024);
	BUILD_BUG_ON(sizeof(struct nvme_st_result) != 28);
	NVME_SPEC_CHECK_OFF(struct nvme_st_result, poh, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_st_result, nsid, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_st_result, flba, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_st_result, sct, 24);
	BUILD_BUG_ON(sizeof(struct nvme_self_test_log) != 564);
	NVME_SPEC_CHECK_OFF(struct nvme_self_test_log, result, 4);
	BUILD_BUG_ON(sizeof(struct nvme_telemetry_log) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_telemetry_log, dalb1, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_telemetry_log, dalb4, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_telemetry_log, hostdgn, 381);
	NVME_SPEC_CHECK_OFF(struct nvme_telemetry_log, rsnident, 384);
	BUILD_BUG_ON(sizeof(struct nvme_endurance_group_log) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_endurance_group_log, ee, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_endurance_group_log, uegcap, 176);
	BUILD_BUG_ON(sizeof(struct nvme_nvmset_predictable_lat_log) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_nvmset_predictable_lat_log, dtwin_rt, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_nvmset_predictable_lat_log, dtwin_re, 128);
	BUILD_BUG_ON(sizeof(struct nvme_aggregate_event_log) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_ana_rsp_hdr) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_ana_group_desc) != 32);
	NVME_SPEC_CHECK_OFF(struct nvme_ana_group_desc, state, 16);
	BUILD_BUG_ON(sizeof(struct nvme_persistent_event_log) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, tnev, 4);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, ts, 20);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, poh, 28);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, pcc, 44);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, sn, 56);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, subnqn, 116);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, gen_number, 372);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, rci, 374);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_log, seb, 480);
	BUILD_BUG_ON(sizeof(struct nvme_persistent_event_entry) != 24);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_entry, ets, 6);
	NVME_SPEC_CHECK_OFF(struct nvme_persistent_event_entry, el, 22);
	BUILD_BUG_ON(sizeof(struct nvme_lba_rd) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_lbas_ns_element) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_lba_status_log) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_media_unit_stat_desc) != 14);
	BUILD_BUG_ON(sizeof(struct nvme_media_unit_stat_log) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_supported_features_log) != 1024);
	BUILD_BUG_ON(sizeof(struct nvme_lockdown_log) != 512);
	BUILD_BUG_ON(sizeof(struct nvme_boot_partition) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_rotational_media_log) != 512);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_ruh_desc) != 4);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_config_desc) != 64);
	NVME_SPEC_CHECK_OFF(struct nvme_fdp_config_desc, runs, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_fdp_config_desc, erutl, 24);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_config_log) != 16);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_ruhu_desc) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_ruhu_log) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_stats_log) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_timestamp) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_event) != 64);
	NVME_SPEC_CHECK_OFF(struct nvme_fdp_event, nsid, 12);
	NVME_SPEC_CHECK_OFF(struct nvme_fdp_event, rgid, 32);
	NVME_SPEC_CHECK_OFF(struct nvme_fdp_event, vs, 40);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_events_log) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_pr_log) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_sanitize_log_page) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_sanitize_log_page, etcend, 28);
	NVME_SPEC_CHECK_OFF(struct nvme_sanitize_log_page, ssi, 36);
	BUILD_BUG_ON(sizeof(struct nvme_zns_changed_zone_log) != 4096);

	/* Features. */
	BUILD_BUG_ON(sizeof(struct nvme_lba_range_type) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_feat_auto_pst) != 256);
	BUILD_BUG_ON(sizeof(struct nvme_plm_config) != 512);
	BUILD_BUG_ON(sizeof(struct nvme_feat_host_behavior) != 512);
	NVME_SPEC_CHECK_OFF(struct nvme_feat_host_behavior, cdfe, 4);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_config) != 4);
	BUILD_BUG_ON(sizeof(struct nvme_fdp_supported_event) != 2);

	/* Directives. */
	BUILD_BUG_ON(sizeof(struct nvme_id_directives) != 4096);
	BUILD_BUG_ON(sizeof(struct streams_directive_params) != 32);
	NVME_SPEC_CHECK_OFF(struct streams_directive_params, sws, 16);
	NVME_SPEC_CHECK_OFF(struct streams_directive_params, nso, 24);
	BUILD_BUG_ON(sizeof(struct nvme_streams_directive_status) != 2);

	/* Zoned namespaces. */
	BUILD_BUG_ON(sizeof(struct nvme_zone_descriptor) != 64);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_descriptor, zcap, 8);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_descriptor, zslba, 16);
	NVME_SPEC_CHECK_OFF(struct nvme_zone_descriptor, wp, 24);
	BUILD_BUG_ON(sizeof(struct nvme_zone_report) != 64);

	/* Protection information. */
	BUILD_BUG_ON(sizeof(struct t10_pi_tuple) != 8);
	BUILD_BUG_ON(sizeof(struct nvme_crc32_pi_tuple) != 16);
	BUILD_BUG_ON(sizeof(struct crc64_pi_tuple) != 16);
	NVME_SPEC_CHECK_OFF(struct crc64_pi_tuple, app_tag, 8);
	NVME_SPEC_CHECK_OFF(struct crc64_pi_tuple, ref_tag, 10);
}

#undef NVME_SPEC_CHECK_OFF
#undef NVME_SPEC_OFF

#endif /* KERNEL_DEV_NVME_NVME_SPEC_H */
