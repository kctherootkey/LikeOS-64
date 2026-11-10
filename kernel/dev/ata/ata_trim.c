// LikeOS ATA layer - DATA SET MANAGEMENT (TRIM)
//
// A solid state drive (and a thin-provisioned virtual disk) can be told
// which sectors no longer hold data the host cares about.  ATA does this
// with DATA SET MANAGEMENT (06h) and its TRIM bit: the command carries a
// data-out payload of 512-byte blocks, each holding 64 little-endian range
// entries of 8 bytes -- the starting LBA in bits 47:0 and the number of
// sectors (1..65535) in bits 63:48.  Unused entries are zero (a length of
// zero means "no range").  How many payload blocks one command may carry
// the drive says in IDENTIFY word 105.
//
// This file owns three things:
//
//   * the PURE encoder ata_trim_encode(): turns [lba, lba+count) into as
//     many range entries as fit into the payload, splitting the range into
//     pieces of at most 65535 sectors, zero-filling the tail, and reports
//     how many sectors it covered so the caller loops for the rest.  The
//     boxed host tests call it directly;
//   * ata_trim_build_tf(): the command for a prepared payload -- the plain
//     DMA form of DATA SET MANAGEMENT, or the queued form (SEND FPDMA
//     QUEUED, subcommand DATA SET MANAGEMENT, AUXILIARY bit 0 = TRIM) when
//     the device supports it, NCQ is in use and the model is not one known
//     to corrupt data or hang with queued TRIM;
//   * ata_trim(): a synchronous TRIM of an arbitrary range for in-kernel
//     callers (it loops payload by payload through ata_qc_exec, it never
//     goes back through the block layer -- the block layer's discard path
//     is built from the two pure functions above);
//
// plus the policy helpers the disk registration uses for the block layer
// limits: the payload size (ata_trim_payload_blocks), whether queued TRIM
// may be used (ata_trim_queued_ok) and whether a trimmed sector reads back
// as zeroes (ata_trim_zeroes_data -- DRAT and RZAT in IDENTIFY word 69,
// honoured only for models known to tell the truth, because a number of
// drives announce "read zeroes after TRIM" and do not do it).
//
// Context: the pure functions run anywhere (including the host tests);
// ata_trim() may sleep (before the scheduler exists it polls).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

#define TRIM_PFX "[ata] "

/* The largest payload this driver puts into one DATA SET MANAGEMENT
 * command, in 512-byte blocks.  IDENTIFY word 105 may announce more (some
 * drives claim 0xFFFF); eight blocks are 512 ranges of 65535 sectors, i.e.
 * about 16 GiB of 512-byte sectors per command -- large discards are split
 * into several commands, which keeps every single command well inside its
 * timeout and avoids drives that announce big payloads but choke on them. */
#define ATA_TRIM_MAX_PAYLOAD_BLOCKS 8u

/* Sectors one range entry can describe (its length field is 16 bits; zero
 * would mean "unused"). */
#define ATA_TRIM_SECTORS_PER_RANGE ((uint64_t)ATA_MAX_TRIM_RANGE)

/* Bytes of one payload block and entries per block (both pinned by
 * ata_layout_checks()). */
#define ATA_TRIM_BLOCK_BYTES ((uint32_t)ATA_SECT_SIZE)
#define ATA_TRIM_ENTRIES_PER_BLOCK ((uint32_t)ATA_TRIM_RANGES_PER_BLOCK)

/* SEND FPDMA QUEUED: the subcommand lives in COUNT bits 12:8 */
#define ATA_SUBCMD_SEND_FPDMA_DSM 0x00
/* SEND FPDMA QUEUED, subcommand DSM: AUXILIARY bit 0 selects TRIM */
#define ATA_SEND_FPDMA_AUX_TRIM 0x00000001u

/* ==========================================================================
 * Model lists
 * ========================================================================== */

/* A tiny glob match: '*' matches any run of characters (also none), '?'
 * exactly one; everything else literally.  Recursive only on '*', with the
 * pattern length bounding the depth.  Pure. */
static int trim_glob_match(const char *pat, const char *str)
{
	while (*pat) {
		if (*pat == '*') {
			while (*pat == '*')
				pat++;
			if (!*pat)
				return 1;
			for (; *str; str++)
				if (trim_glob_match(pat, str))
					return 1;
			return 0;
		}
		if (!*str)
			return 0;
		if (*pat != '?' && *pat != *str)
			return 0;
		pat++;
		str++;
	}
	return *str == '\0';
}

/* Models whose firmware mishandles queued TRIM (SEND FPDMA QUEUED with
 * DATA SET MANAGEMENT): data corruption, or a command that never
 * completes.  The plain, non-queued DATA SET MANAGEMENT works on all of
 * them and is used instead. */
static const char *const trim_no_queued_models[] = {
	"Micron_M500_*",
	"Crucial_CT*M500*",
	"Micron_M510_*",
	"Micron_M550_*",
	"Crucial_CT*M550*",
	"Crucial_CT*MX100*",
	"Samsung SSD 840*",
	"Samsung SSD 850*",
	"Samsung SSD 860*",
	"Samsung SSD 870*",
	"Samsung Portable SSD*",
	"SAMSUNG*MZ7*",
	"FCCT*M500*",
	"*SanDisk*SD8SB8U*",
	NULL,
};

/* Models whose "deterministic read zeroes after TRIM" (IDENTIFY word 69
 * bits 14 and 5) is known to be honest.  Many drives set both bits and
 * still return stale data or non-zero patterns for trimmed sectors, so
 * discard_zeroes_data is only claimed for these. */
static const char *const trim_rzat_trusted_models[] = {
	"Crucial_CT*",
	"Micron_*",
	"INTEL*SSD*",
	"SSDSC2*",
	"SSDSA2*",
	"Samsung SSD 8*",
	"SAMSUNG*MZ7*",
	"SanDisk SD7*",
	"SanDisk SDSSDH*",
	"SanDisk SDSSDX*",
	"TOSHIBA THNSN*",
	"KINGSTON SA400*",
	NULL,
};

static int trim_model_in(const char *const *list, const char *model)
{
	if (!model || !model[0])
		return 0;
	for (int i = 0; list[i]; i++)
		if (trim_glob_match(list[i], model))
			return 1;
	return 0;
}

/* ==========================================================================
 * Capability helpers (pure)
 * ========================================================================== */

/* The device supports TRIM at all.  The ATA core sets ATA_DFLAG_TRIM from
 * IDENTIFY word 169 bit 0 (and may clear it for a quirk); that decision is
 * final here. */
static int trim_supported(const struct ata_device *dev)
{
	return dev && (dev->flags & ATA_DFLAG_TRIM) &&
	       !(dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED));
}

uint32_t ata_trim_payload_blocks(const struct ata_device *dev)
{
	uint32_t blocks;

	if (!trim_supported(dev))
		return 0;

	/* The core parsed word 105 into dsm_max_blocks; fall back to the raw
	 * word for a device the core has not configured yet.  Zero means the
	 * drive does not say, and one block is what every TRIM-capable
	 * drive accepts. */
	blocks = dev->dsm_max_blocks;
	if (blocks == 0)
		blocks = dev->id[ATA_ID_MAX_DSM_BLOCKS];
	if (blocks == 0 || blocks == 0xFFFF)
		blocks = (blocks == 0xFFFF) ? ATA_TRIM_MAX_PAYLOAD_BLOCKS : 1;
	if (blocks > ATA_TRIM_MAX_PAYLOAD_BLOCKS)
		blocks = ATA_TRIM_MAX_PAYLOAD_BLOCKS;

	/* The payload itself is a transfer: it must fit into one command of
	 * the host (host_max_sectors is in 512-byte units). */
	if (dev->host_max_sectors && blocks > dev->host_max_sectors)
		blocks = dev->host_max_sectors;
	return blocks ? blocks : 1;
}

int ata_trim_queued_ok(const struct ata_device *dev)
{
	if (!trim_supported(dev))
		return 0;
	/* The device announced queued TRIM (the core reads the NCQ SEND/
	 * RECEIVE log 13h and sets ATA_DFLAG_NCQ_TRIM), and NCQ is really in
	 * use on this link. */
	if (!(dev->flags & ATA_DFLAG_NCQ_TRIM))
		return 0;
	if (!(dev->flags & ATA_DFLAG_NCQ_ON) || dev->ncq_depth == 0)
		return 0;
	if (!(dev->flags & ATA_DFLAG_NCQ_SEND_RECV) &&
	    !(dev->id[ATA_ID_SATA_CAPABILITY_2] != 0xFFFF &&
	      (dev->id[ATA_ID_SATA_CAPABILITY_2] & ATA_ID_SATA2_NCQ_SEND_RECV)))
		return 0;
	/* Queued TRIM is only safe when deterministic: a drive that may
	 * return either old data or zeroes after a queued TRIM has, in the
	 * field, also been the kind that loses ordering against writes. */
	if (!(dev->flags & ATA_DFLAG_TRIM_DRAT) &&
	    !(dev->id[ATA_ID_ADDITIONAL_SUPP] & ATA_ID_ADD_DRAT))
		return 0;
	if (trim_model_in(trim_no_queued_models, dev->model))
		return 0;
	return 1;
}

int ata_trim_zeroes_data(const struct ata_device *dev)
{
	uint16_t w69;
	int drat, rzat;

	if (!trim_supported(dev))
		return 0;
	w69 = dev->id[ATA_ID_ADDITIONAL_SUPP];
	if (w69 == 0xFFFF)
		w69 = 0;
	drat = (dev->flags & ATA_DFLAG_TRIM_DRAT) || (w69 & ATA_ID_ADD_DRAT);
	rzat = (dev->flags & ATA_DFLAG_TRIM_RZAT) || (w69 & ATA_ID_ADD_RZAT);
	if (!drat || !rzat)
		return 0;

	/* Families known to implement it as announced. */
	if (trim_model_in(trim_rzat_trusted_models, dev->model))
		return 1;
	/* Any other solid state drive only when it claims ACS-3
	 * or later conformance (word 80 bit 10+) was certified against the
	 * stricter ACS-3 definition of RZAT. */
	if (trim_glob_match("*SSD*", dev->model) &&
	    dev->major_version != 0xFFFF &&
	    (dev->major_version &
	     (ATA_ID_MAJOR_ACS3 | ATA_ID_MAJOR_ACS4 | ATA_ID_MAJOR_ACS5)))
		return 1;
	return 0;
}

/* ==========================================================================
 * The range encoder (pure)
 * ========================================================================== */

/* Store one 64-bit value little-endian, byte by byte, so the encoder is
 * independent of the host's byte order and of the buffer's alignment. */
static void trim_put_le64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

uint64_t ata_trim_encode(void *buf, uint32_t blocks, uint64_t lba,
			 uint64_t count)
{
	uint8_t *p = (uint8_t *)buf;
	uint64_t entries_max, entry = 0, covered = 0;

	if (!buf || blocks == 0)
		return 0;

	entries_max = (uint64_t)blocks * ATA_TRIM_ENTRIES_PER_BLOCK;

	/* Every entry of the payload is written: the used ones with a range,
	 * the rest with zero (a zero length marks an unused entry, and a
	 * payload must not carry stale ranges from a previous use of the
	 * buffer). */
	while (entry < entries_max && covered < count) {
		uint64_t start = lba + covered;
		uint64_t len = count - covered;

		/* A range entry addresses 48 bits.  The part of the request
		 * beyond the 48-bit space cannot be described at all: stop
		 * there and report what was covered (the caller sees the
		 * shortfall and fails the rest as out of range). */
		if (start > ATA_MAX_LBA48)
			break;
		if (len > ATA_TRIM_SECTORS_PER_RANGE)
			len = ATA_TRIM_SECTORS_PER_RANGE;
		if (start + len - 1 > ATA_MAX_LBA48)
			len = ATA_MAX_LBA48 - start + 1;

		trim_put_le64(p + entry * 8, ATA_DSM_RANGE(start, len));
		entry++;
		covered += len;
	}
	for (; entry < entries_max; entry++)
		trim_put_le64(p + entry * 8, 0);
	return covered;
}

/* ==========================================================================
 * Command construction (pure)
 * ========================================================================== */

int ata_trim_build_tf(const struct ata_device *dev,
		      struct ata_queued_cmd *qc, uint32_t blocks)
{
	struct ata_taskfile *tf;

	if (!dev || !qc)
		return -EINVAL;
	VM_BUG_ON(qc->magic != ATA_QC_MAGIC);
	if (!trim_supported(dev))
		return -EOPNOTSUPP;
	if (blocks == 0 || blocks > 0xFFFF)
		return -EINVAL;
	/* The payload may be attached before (ata_qc_set_buf, the block
	 * request's sg list) or right after this call -- the block layer's
	 * discard path builds the command first.  When it is already there
	 * it must be exactly the announced blocks. */
	if (qc->nbytes != 0 && qc->nbytes != blocks * ATA_TRIM_BLOCK_BYTES) {
		WARN_RATELIMIT(1,
			       TRIM_PFX "%s: TRIM payload %u bytes for %u blocks",
			       dev->name, qc->nbytes, blocks);
		return -EINVAL;
	}

	tf = &qc->tf;
	kmemset(tf, 0, sizeof(*tf));
	qc->flags |= ATA_QCFLAG_WRITE;
	qc->dma_dir = ATA_DIR_TO_DEVICE;
	if (qc->timeout_ms == 0)
		qc->timeout_ms = ATA_TMOUT_TRIM_MS;

	if (ata_trim_queued_ok(dev)) {
		/* SEND FPDMA QUEUED: FEATURE 15:0 = payload blocks, COUNT
		 * 15:8 = subcommand (DATA SET MANAGEMENT), COUNT 7:3 = the
		 * tag (the transport fills it), LBA = 0, DEVICE bit 6 set,
		 * AUXILIARY bit 0 = TRIM. */
		tf->command = ATA_CMD_FPDMA_SEND;
		tf->feature = (uint8_t)(blocks & 0xFF);
		tf->hob_feature = (uint8_t)(blocks >> 8);
		tf->nsect = 0;
		tf->hob_nsect = ATA_SUBCMD_SEND_FPDMA_DSM;
		tf->auxiliary = ATA_SEND_FPDMA_AUX_TRIM;
		tf->device = ATA_LBA;
		tf->flags = ATA_TFLAG_LBA48 | ATA_TFLAG_ISADDR |
			    ATA_TFLAG_DEVICE | ATA_TFLAG_WRITE | ATA_TFLAG_LBA;
		qc->protocol = ATA_PROT_FPDMA;
	} else {
		/* DATA SET MANAGEMENT (DMA): FEATURE bit 0 = TRIM, COUNT =
		 * payload blocks, LBA = 0. */
		tf->command = ATA_CMD_DSM;
		tf->feature = ATA_DSM_TRIM;
		tf->hob_feature = 0;
		tf->nsect = (uint8_t)(blocks & 0xFF);
		tf->hob_nsect = (uint8_t)(blocks >> 8);
		tf->device = ATA_LBA;
		tf->flags = ATA_TFLAG_LBA48 | ATA_TFLAG_ISADDR |
			    ATA_TFLAG_DEVICE | ATA_TFLAG_WRITE | ATA_TFLAG_LBA;
		qc->protocol = ATA_PROT_DMA_OUT;
	}
	return 0;
}

/* ==========================================================================
 * Synchronous TRIM
 * ========================================================================== */

/* Blocks needed for `remaining' sectors, at most `max_blocks'. */
static uint32_t trim_blocks_for(uint64_t remaining, uint32_t max_blocks)
{
	uint64_t ranges = (remaining + ATA_TRIM_SECTORS_PER_RANGE - 1) /
			  ATA_TRIM_SECTORS_PER_RANGE;
	uint64_t blocks = (ranges + ATA_TRIM_ENTRIES_PER_BLOCK - 1) /
			  ATA_TRIM_ENTRIES_PER_BLOCK;

	if (blocks == 0)
		blocks = 1;
	if (blocks > max_blocks)
		blocks = max_blocks;
	return (uint32_t)blocks;
}

int ata_trim(struct ata_device *dev, uint64_t lba, uint64_t count)
{
	uint32_t max_blocks;
	uint8_t *payload;
	int payload_hw_owned = 0;
	uint64_t done = 0;
	uint32_t commands = 0;
	int ret = 0;

	if (!dev)
		return -EINVAL;
	BUG_ON(dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();

	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (!trim_supported(dev))
		return -EOPNOTSUPP;
	if (count == 0)
		return 0;
	if (lba >= dev->n_sectors || count > dev->n_sectors - lba) {
		WARN_RATELIMIT(1,
			       TRIM_PFX "%s: TRIM %llu+%llu beyond %llu sectors",
			       dev->name, (unsigned long long)lba,
			       (unsigned long long)count,
			       (unsigned long long)dev->n_sectors);
		return -EINVAL;
	}
	if (dev->security.locked)
		return -EACCES;

	max_blocks = ata_trim_payload_blocks(dev);
	if (max_blocks == 0)
		return -EOPNOTSUPP;
	payload = kalloc((size_t)max_blocks * ATA_TRIM_BLOCK_BYTES);
	if (!payload)
		return -ENOMEM;

	while (done < count) {
		struct ata_queued_cmd *qc;
		uint64_t remaining = count - done;
		uint32_t blocks = trim_blocks_for(remaining, max_blocks);
		uint64_t covered;

		covered = ata_trim_encode(payload, blocks, lba + done,
					  remaining);
		if (covered == 0) {
			/* Only possible past the 48-bit address space, which
			 * the range check above already excludes. */
			WARN_ON_ONCE(1);
			ret = -EINVAL;
			break;
		}

		qc = ata_qc_alloc(dev);
		if (!qc) {
			ret = -ENOMEM;
			break;
		}
		ata_qc_set_buf(qc, payload, blocks * ATA_TRIM_BLOCK_BYTES,
			       ATA_DIR_TO_DEVICE);
		qc->flags |= ATA_QCFLAG_INTERNAL | ATA_QCFLAG_RETRY;
		qc->max_retries = ATA_MAX_RETRIES;
		qc->timeout_ms = ATA_TMOUT_TRIM_MS;
		qc->lba = lba + done;
		qc->n_sect = (uint32_t)(covered > 0xFFFFFFFFull ? 0xFFFFFFFFu
								: covered);
		ret = ata_trim_build_tf(dev, qc, blocks);
		if (ret == 0)
			ret = ata_qc_exec(qc);
		if (ret) {
			if (ret == -EIO)
				kprintf(TRIM_PFX
					"%s: TRIM of %llu sectors at %llu "
					"failed: status 0x%02x error 0x%02x "
					"(%s)\n",
					dev->name, (unsigned long long)covered,
					(unsigned long long)(lba + done),
					qc->result_tf.status,
					qc->result_tf.error,
					qc->tf.command == ATA_CMD_FPDMA_SEND
						? "SEND FPDMA QUEUED"
						: "DATA SET MANAGEMENT");
			else
				WARN_RATELIMIT(1,
					       TRIM_PFX "%s: TRIM at %llu: %d",
					       dev->name,
					       (unsigned long long)(lba + done),
					       ret);
		}
		if (unlikely(qc->flags & ATA_QCFLAG_HW_OWNED))
			payload_hw_owned = 1;
		ata_qc_free(qc);
		if (ret)
			break;
		done += covered;
		commands++;
	}

	/* The payload held only LBAs, but clear it anyway before giving the
	 * memory back: a kernel buffer leaves no traces. */
	if (unlikely(payload_hw_owned)) {
		/* A TRIM failed while the controller still owned its slot:
		 * it may yet read the payload -- and a TRIM that reads a
		 * reused buffer would deallocate whatever ranges it finds
		 * there.  Keep the memory, untouched. */
		WARN_RATELIMIT(1, TRIM_PFX "%s: keeping the range payload of "
				  "a command the controller still owns",
			       dev->name);
	} else {
		kmemset(payload, 0,
			(size_t)max_blocks * ATA_TRIM_BLOCK_BYTES);
		kfree(payload);
	}

	if (ret == 0 && commands > 64)
		kprintf(TRIM_PFX "%s: trimmed %llu sectors at %llu in %u "
				 "commands\n",
			dev->name, (unsigned long long)count,
			(unsigned long long)lba, commands);
	return ret;
}
