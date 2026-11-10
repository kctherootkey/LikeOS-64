// LikeOS ATA layer - read/write/flush translation and the ATA disk operations
//
// A block request (struct blk_request) becomes one or more ATA commands
// here.  The rules of the ATA command set that decide which command:
//
//   * NCQ (READ/WRITE FPDMA QUEUED, 60h/61h) whenever the device and the
//     host both queue: the sector count travels in the FEATURE registers,
//     the tag in COUNT bits 7:3 (the transport owns tags), FUA in DEVICE
//     bit 7 and the priority in COUNT bits 15:14;
//   * otherwise READ/WRITE DMA (C8h/CAh, 28-bit: LBA 0..2^28-1, 1..256
//     sectors with 0 meaning 256) when the range fits, READ/WRITE DMA EXT
//     (25h/35h, 48-bit, 1..65536 sectors with 0 meaning 65536) when it
//     does not;
//   * WRITE DMA FUA EXT (3Dh) for a forced-unit-access write without NCQ;
//     a device without FUA gets the write followed by a cache flush;
//   * PIO READ/WRITE SECTOR(S) (EXT) for the rare device without DMA.
//
// Requests larger than one command can carry -- the device's max_sectors
// or the reach of the host's PRD table for the request's memory -- are
// split into chunks that run one after the other.  FLUSH CACHE (EXT) is
// re-issued after a write error per the ATA rules: the device reports the
// first LBA it could not write, drops that data from its cache, and a
// further FLUSH continues with the rest.  DISCARD becomes DSM TRIM
// payloads (encoded by ata_trim.c), WRITE ZEROES becomes either TRIM (on
// devices whose zero-after-TRIM is trustworthy) or writes from a zero
// buffer.  Pass-through commands (SG_IO, HDIO_*) arrive prepared and are
// issued in order with the rest.
//
// The life of a request here: ->submit (ata_blk_submit) runs in the task
// that dispatches -- the submitter, or kblockd -- and issues the first
// command.  The command's end_io (ata_io_qc_done) runs in the interrupt
// handler and only hands the request to the block softirq.  There
// ->complete (ata_blk_complete) reads the result: the request ends, or it
// goes back to the head of its queue with this context attached
// (blk_queue_continue) and kblockd's dispatch issues its next command
// through ->submit.  Nothing here sleeps, and the memory comes from
// per-device pools.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ata_dev_internal.h>
#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ==========================================================================
 * Pure command building (host-testable)
 * ========================================================================== */

int ata_rw_check_range(const struct ata_device *dev, uint64_t lba,
		       uint64_t count)
{
	if (!dev || count == 0)
		return -EINVAL;
	if (lba >= dev->n_sectors)
		return -EINVAL;
	if (count > dev->n_sectors - lba) /* no overflow: lba < n_sectors */
		return -EINVAL;
	return 0;
}

int ata_rw_build_tf_ex(const struct ata_device *dev, struct ata_queued_cmd *qc,
		       uint64_t lba, uint32_t count, int write, int fua,
		       int allow_ncq, int prio)
{
	struct ata_taskfile *tf;
	uint32_t maxs;
	int has48, use_ncq, use_dma;
	int lba28_strict, lba28_possible;
	int fua_ok;

	if (!dev || !qc)
		return -EINVAL;
	if (!(dev->flags & ATA_DFLAG_LBA))
		return -EINVAL; /* CHS addressing is not supported */
	has48 = !!(dev->flags & ATA_DFLAG_LBA48);
	maxs = has48 ? ATA_MAX_SECTORS_LBA48 : ATA_MAX_SECTORS_LBA28;
	if (dev->max_sectors && dev->max_sectors < maxs)
		maxs = dev->max_sectors;
	if (count == 0 || count > maxs)
		return -EINVAL;
	if (ata_rw_check_range(dev, lba, count))
		return -EINVAL;

	/* A 28-bit command reaches LBA 0x0FFFFFFF; the last sector of that
	 * range is kept for the 48-bit commands where a device has them
	 * (some firmware mishandles a 28-bit command touching it). */
	lba28_strict = (lba + count < (1ULL << 28)) &&
		       count <= ATA_MAX_SECTORS_LBA28;
	lba28_possible = (lba + count <= (1ULL << 28)) &&
			 count <= ATA_MAX_SECTORS_LBA28;
	if (!has48 && !lba28_possible)
		return -EINVAL;

	use_dma = !!(dev->id[ATA_ID_CAPABILITY] & ATA_ID_CAP_DMA);
	use_ncq = allow_ncq && use_dma && has48 &&
		  (dev->flags & ATA_DFLAG_NCQ_ON) && dev->ncq_depth >= 1;
	fua_ok = (dev->flags & ATA_DFLAG_FUA) && dev->host_fua;

	/* FUA on a read only means something to a queued read (read the
	 * media, not the cache); a plain DMA read ignores it. */
	if (fua && !write && !use_ncq)
		fua = 0;
	if (fua && !fua_ok)
		return -EOPNOTSUPP;
	if (fua && !use_ncq && (!use_dma || !has48))
		return -EOPNOTSUPP; /* WRITE DMA FUA EXT is a 48-bit DMA command */

	tf = &qc->tf;
	kmemset(tf, 0, sizeof(*tf));
	qc->flags &= ~ATA_QCFLAG_WRITE;
	if (write)
		qc->flags |= ATA_QCFLAG_WRITE;

	if (use_ncq) {
		tf->command = write ? ATA_CMD_FPDMA_WRITE : ATA_CMD_FPDMA_READ;
		ata_tf_set_lba(tf, lba, 1);
		/* count 65536 encodes as 0 */
		tf->feature = (uint8_t)(count & 0xFF);
		tf->hob_feature = (uint8_t)((count >> 8) & 0xFF);
		tf->nsect = 0; /* tag << 3, filled by the transport */
		tf->hob_nsect = (uint8_t)((prio & 0x3) << 6);
		tf->device = ATA_LBA | (fua ? ATA_FPDMA_FUA : 0);
		qc->protocol = ATA_PROT_FPDMA;
	} else if (use_dma) {
		int lba48;

		if (fua) {
			tf->command = ATA_CMD_WRITE_DMA_FUA_EXT;
			lba48 = 1;
		} else if (lba28_strict || (!has48 && lba28_possible)) {
			tf->command = write ? ATA_CMD_WRITE_DMA :
					      ATA_CMD_READ_DMA;
			lba48 = 0;
		} else {
			tf->command = write ? ATA_CMD_WRITE_DMA_EXT :
					      ATA_CMD_READ_DMA_EXT;
			lba48 = 1;
		}
		ata_tf_set_lba(tf, lba, lba48);
		tf->nsect = (uint8_t)(count & 0xFF);
		if (lba48)
			tf->hob_nsect = (uint8_t)((count >> 8) & 0xFF);
		qc->protocol = write ? ATA_PROT_DMA_OUT : ATA_PROT_DMA_IN;
	} else {
		int lba48 = !(lba28_strict || (!has48 && lba28_possible));

		if (fua)
			return -EOPNOTSUPP;
		if (lba48)
			tf->command = write ? ATA_CMD_WRITE_SECTORS_EXT :
					      ATA_CMD_READ_SECTORS_EXT;
		else
			tf->command = write ? ATA_CMD_WRITE_SECTORS :
					      ATA_CMD_READ_SECTORS;
		ata_tf_set_lba(tf, lba, lba48);
		tf->nsect = (uint8_t)(count & 0xFF);
		if (lba48)
			tf->hob_nsect = (uint8_t)((count >> 8) & 0xFF);
		qc->protocol = write ? ATA_PROT_PIO_OUT : ATA_PROT_PIO_IN;
	}
	if (write)
		tf->flags |= ATA_TFLAG_WRITE;
	if (fua)
		tf->flags |= ATA_TFLAG_FUA;
	qc->dma_dir = write ? ATA_DIR_TO_DEVICE : ATA_DIR_FROM_DEVICE;
	qc->lba = lba;
	qc->n_sect = count;
	if (!qc->timeout_ms)
		qc->timeout_ms = ATA_TMOUT_RW_MS;
	return 0;
}

int ata_rw_build_tf(const struct ata_device *dev, struct ata_queued_cmd *qc,
		    uint64_t lba, uint32_t count, int write, int fua)
{
	return ata_rw_build_tf_ex(dev, qc, lba, count, write, fua, 1, 0);
}

/* ==========================================================================
 * Pools (any context)
 * ========================================================================== */

static int ata_bitmap_alloc(volatile uint64_t *map, int n)
{
	uint64_t limit = (n >= 64) ? ~0ULL : ((1ULL << n) - 1);

	for (;;) {
		uint64_t m = __atomic_load_n(map, __ATOMIC_ACQUIRE);
		uint64_t avail = ~m & limit;
		int bit;

		if (!avail)
			return -1;
		bit = __builtin_ctzll(avail);
		if (__atomic_compare_exchange_n(map, &m, m | (1ULL << bit), 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return bit;
	}
}

static void ata_bitmap_free(volatile uint64_t *map, int bit)
{
	uint64_t old = __atomic_fetch_and(map, ~(1ULL << bit),
					  __ATOMIC_RELEASE);

	BUG_ON(!(old & (1ULL << bit))); /* freeing a free slot */
}

int ata_rw_dev_init(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	uint32_t blocks;

	if (!p)
		return -EINVAL;
	if (p->pools_ready)
		return 0;
	p->qc_pool = kalloc(sizeof(struct ata_queued_cmd) * ATA_QC_POOL);
	p->io_pool = kalloc(sizeof(struct ata_io) * ATA_IO_POOL);
	p->zero_buf = kalloc(ATA_ZERO_BUF_BYTES);
	if (!p->qc_pool || !p->io_pool || !p->zero_buf)
		goto nomem;
	kmemset(p->qc_pool, 0, sizeof(struct ata_queued_cmd) * ATA_QC_POOL);
	kmemset(p->io_pool, 0, sizeof(struct ata_io) * ATA_IO_POOL);
	kmemset(p->zero_buf, 0, ATA_ZERO_BUF_BYTES);
	p->qc_map = 0;
	p->io_map = 0;
	p->dsm_map = 0;

	/* TRIM payloads: as many 512-byte blocks per command as the device
	 * accepts (word 105), at most ATA_DSM_BUF_BLOCKS_MAX. */
	blocks = dev->dsm_max_blocks;
	if (blocks > ATA_DSM_BUF_BLOCKS_MAX)
		blocks = ATA_DSM_BUF_BLOCKS_MAX;
	if (!blocks)
		blocks = 1;
	p->dsm_buf_blocks = blocks;
	for (int i = 0; i < ATA_DSM_BUFS; i++) {
		p->dsm_bufs[i] = NULL;
		if (!(dev->flags & ATA_DFLAG_TRIM))
			continue;
		p->dsm_bufs[i] = kalloc(blocks * ATA_SECT_SIZE);
		if (!p->dsm_bufs[i])
			goto nomem;
		kmemset(p->dsm_bufs[i], 0, blocks * ATA_SECT_SIZE);
	}
	p->pools_ready = 1;
	return 0;
nomem:
	ata_rw_dev_fini(dev);
	ata_dev_printk(dev, "no memory for the I/O pools\n");
	return -ENOMEM;
}

void ata_rw_dev_fini(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);

	if (!p)
		return;
	WARN_ON(p->qc_map != 0);
	WARN_ON(p->io_map != 0);
	p->pools_ready = 0;
	if (p->qc_pool)
		kfree(p->qc_pool);
	if (p->io_pool)
		kfree(p->io_pool);
	if (p->zero_buf)
		kfree(p->zero_buf);
	for (int i = 0; i < ATA_DSM_BUFS; i++) {
		if (p->dsm_bufs[i])
			kfree(p->dsm_bufs[i]);
		p->dsm_bufs[i] = NULL;
	}
	p->qc_pool = NULL;
	p->io_pool = NULL;
	p->zero_buf = NULL;
}

/* A pool command, initialised, holding a device reference. */
static struct ata_queued_cmd *ata_rw_qc_get(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct ata_queued_cmd *qc;
	int bit;

	if (unlikely(!p || !p->pools_ready))
		return NULL;
	bit = ata_bitmap_alloc(&p->qc_map, ATA_QC_POOL);
	if (bit < 0)
		return NULL;
	qc = &p->qc_pool[bit];
	ata_qc_init(qc, dev);
	qc->flags |= ATA_QCFLAG_ORIGIN_POOL;
	ata_dev_get(dev);
	return qc;
}

void ata_rw_qc_pool_release(struct ata_queued_cmd *qc)
{
	struct ata_device *dev = qc->dev;
	struct ata_dev_priv *p = ata_dev_priv(dev);
	long idx;

	BUG_ON(!p || !p->qc_pool);
	idx = qc - p->qc_pool;
	BUG_ON(idx < 0 || idx >= ATA_QC_POOL);
	qc->magic = 0;
	qc->end_io = NULL;
	qc->private_data = NULL;
	qc->req = NULL;
	ata_bitmap_free(&p->qc_map, (int)idx);
	ata_dev_put(dev); /* the pool command's reference */
}

static struct ata_io *ata_io_get(struct ata_device *dev)
{
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct ata_io *io;
	int bit;

	if (unlikely(!p || !p->pools_ready))
		return NULL;
	bit = ata_bitmap_alloc(&p->io_map, ATA_IO_POOL);
	if (bit < 0)
		return NULL;
	io = &p->io_pool[bit];
	kmemset(io, 0, sizeof(*io));
	io->magic = ATA_IO_MAGIC;
	io->index = (uint32_t)bit;
	io->dev = dev;
	io->dsm_slot = -1;
	io->flush_bad_lba = ~0ULL;
	io->lss = dev->logical_sector_size ? dev->logical_sector_size :
					     ATA_SECT_SIZE;
	return io;
}

static void ata_io_put(struct ata_io *io)
{
	struct ata_dev_priv *p = ata_dev_priv(io->dev);

	BUG_ON(io->magic != ATA_IO_MAGIC);
	if (io->dsm_slot >= 0) {
		ata_bitmap_free(&p->dsm_map, io->dsm_slot);
		io->dsm_slot = -1;
		io->dsm_buf = NULL;
	}
	io->magic = 0;
	io->req = NULL;
	ata_bitmap_free(&p->io_map, (int)io->index);
}

/* ==========================================================================
 * Running the disk's queue
 * ========================================================================== */

void ata_rw_kick(struct ata_device *dev)
{
	if (!dev || !dev->disk || !dev->disk->queue)
		return;
	blk_queue_run(dev->disk->queue);
}

/* ==========================================================================
 * Building one command of a request
 * ========================================================================== */

/* PRD entries an element range may need: one per 4 KiB page touched for
 * memory known only by its virtual address, one per 4 MiB for a range
 * that is physically contiguous. */
static uint32_t ata_sg_prd_estimate(const struct blk_sg_entry *e, uint32_t off,
				    uint32_t len)
{
	if (!len)
		return 0;
	if (e->phys)
		return (len + ATA_PRD_MAX_BYTES - 1) / ATA_PRD_MAX_BYTES;
	{
		uint64_t start = (uint64_t)(uintptr_t)e->virt + off;

		return (uint32_t)(((start & 4095ULL) + len + 4095ULL) >> 12);
	}
}

static uint32_t ata_dev_prd_limit(const struct ata_device *dev)
{
	uint32_t lim = dev->host_max_segments ? dev->host_max_segments : 128;

	return lim;
}

/* The data of the next chunk: up to `want' sectors from the request's sg
 * list starting at the cursor, as far as the chunk's own sg array and the
 * PRD table reach, cut at a sector boundary.  Returns the sectors in the
 * chunk (0: the list cannot carry even one sector -> -EINVAL to the
 * caller). */
static uint32_t ata_io_build_chunk(struct ata_io *io, uint32_t want,
				   struct blk_sg_entry **sg_out,
				   uint32_t *nsg_out)
{
	struct blk_request *req = io->req;
	uint32_t idx = io->sg_idx, off = io->sg_off;
	uint64_t want_bytes = (uint64_t)want * io->lss;
	uint64_t bytes = 0;
	uint32_t prd = 0, n = 0;
	uint32_t prd_limit = ata_dev_prd_limit(io->dev);
	uint32_t sectors;

	/* The common case: the whole request in one command, its own list
	 * usable as it is. */
	if (idx == 0 && off == 0 && want == io->remaining) {
		uint64_t total = 0;
		uint32_t est = 0;

		for (uint32_t i = 0; i < req->nsg; i++) {
			total += req->sg[i].len;
			est += ata_sg_prd_estimate(&req->sg[i], 0,
						   req->sg[i].len);
		}
		if (total == want_bytes && est <= prd_limit) {
			*sg_out = req->sg;
			*nsg_out = req->nsg;
			io->chunk_next_idx = req->nsg;
			io->chunk_next_off = 0;
			io->chunk_bytes = (uint32_t)want_bytes;
			return want;
		}
	}

	while (bytes < want_bytes && idx < req->nsg && n < ATA_IO_CHUNK_SG) {
		const struct blk_sg_entry *e = &req->sg[idx];
		uint32_t avail = e->len - off;
		uint32_t take = avail;
		uint32_t est;

		if (avail == 0) { /* empty element: skip it */
			idx++;
			off = 0;
			continue;
		}
		if (take > want_bytes - bytes)
			take = (uint32_t)(want_bytes - bytes);
		est = ata_sg_prd_estimate(e, off, take);
		if (prd + est > prd_limit) {
			/* Take what still fits: whole pages for a virtual
			 * range. */
			uint32_t room = prd_limit - prd;
			uint64_t start;
			uint32_t first;

			if (room == 0 || e->phys)
				break;
			start = (uint64_t)(uintptr_t)e->virt + off;
			first = 4096 - (uint32_t)(start & 4095);
			if (room == 1)
				take = first < take ? first : take;
			else if ((uint64_t)first + (uint64_t)(room - 1) * 4096 <
				 take)
				take = first + (room - 1) * 4096;
			est = ata_sg_prd_estimate(e, off, take);
		}
		io->chunk_sg[n].virt = (uint8_t *)e->virt + off;
		io->chunk_sg[n].phys = e->phys ? e->phys + off : 0;
		io->chunk_sg[n].len = take;
		io->chunk_sg[n].flags = 0;
		n++;
		prd += est;
		bytes += take;
		off += take;
		if (off >= e->len) {
			idx++;
			off = 0;
		}
		if (prd >= prd_limit)
			break;
	}

	/* Cut back to a whole number of sectors. */
	sectors = (uint32_t)(bytes / io->lss);
	if (sectors == 0)
		return 0;
	if ((uint64_t)sectors * io->lss != bytes) {
		uint64_t excess = bytes - (uint64_t)sectors * io->lss;

		while (excess && n) {
			struct blk_sg_entry *last = &io->chunk_sg[n - 1];

			if (last->len > excess) {
				last->len -= (uint32_t)excess;
				excess = 0;
			} else {
				excess -= last->len;
				n--;
			}
		}
		/* recompute the cursor from the start */
		bytes = (uint64_t)sectors * io->lss;
		idx = io->sg_idx;
		off = io->sg_off;
		{
			uint64_t left = bytes;

			while (left) {
				uint32_t avail = req->sg[idx].len - off;

				if (avail > left) {
					off += (uint32_t)left;
					left = 0;
				} else {
					left -= avail;
					idx++;
					off = 0;
				}
			}
		}
	}
	*sg_out = io->chunk_sg;
	*nsg_out = n;
	io->chunk_next_idx = idx;
	io->chunk_next_off = off;
	io->chunk_bytes = (uint32_t)bytes;
	return sectors;
}

/* WRITE ZEROES by writes: the chunk's elements all point at the device's
 * zero buffer. */
static uint32_t ata_io_build_zero_chunk(struct ata_io *io, uint32_t want,
					uint32_t *nsg_out)
{
	struct ata_dev_priv *p = ata_dev_priv(io->dev);
	uint64_t want_bytes = (uint64_t)want * io->lss;
	uint64_t bytes = 0;
	uint32_t prd = 0, n = 0, prd_limit = ata_dev_prd_limit(io->dev);
	uint32_t sectors;

	while (bytes < want_bytes && n < ATA_IO_CHUNK_SG) {
		uint32_t take = ATA_ZERO_BUF_BYTES;
		struct blk_sg_entry e = { p->zero_buf, 0, 0, 0 };
		uint32_t est;

		if (take > want_bytes - bytes)
			take = (uint32_t)(want_bytes - bytes);
		est = ata_sg_prd_estimate(&e, 0, take);
		if (prd + est > prd_limit) {
			uint32_t room = prd_limit - prd;
			uint32_t first = 4096 - (uint32_t)((uintptr_t)p->zero_buf &
							    4095);

			if (room == 0)
				break;
			if (room == 1)
				take = first < take ? first : take;
			else if ((uint64_t)first + (uint64_t)(room - 1) * 4096 <
				 take)
				take = first + (room - 1) * 4096;
			est = ata_sg_prd_estimate(&e, 0, take);
		}
		io->chunk_sg[n].virt = p->zero_buf;
		io->chunk_sg[n].phys = 0;
		io->chunk_sg[n].len = take;
		io->chunk_sg[n].flags = 0;
		n++;
		prd += est;
		bytes += take;
		if (prd >= prd_limit)
			break;
	}
	sectors = (uint32_t)(bytes / io->lss);
	if (!sectors)
		return 0;
	bytes = (uint64_t)sectors * io->lss;
	/* trim the tail to the sector multiple */
	{
		uint64_t acc = 0;
		uint32_t i;

		for (i = 0; i < n; i++) {
			if (acc + io->chunk_sg[i].len >= bytes) {
				io->chunk_sg[i].len = (uint32_t)(bytes - acc);
				i++;
				break;
			}
			acc += io->chunk_sg[i].len;
		}
		n = i;
	}
	*nsg_out = n;
	io->chunk_bytes = (uint32_t)bytes;
	return sectors;
}

static void ata_io_fill_flush_tf(const struct ata_device *dev,
				 struct ata_queued_cmd *qc)
{
	kmemset(&qc->tf, 0, sizeof(qc->tf));
	if ((dev->flags & ATA_DFLAG_FLUSH_EXT) &&
	    (dev->flags & ATA_DFLAG_LBA48)) {
		qc->tf.command = ATA_CMD_FLUSH_EXT;
		/* so the failing LBA reads back with its high bytes */
		qc->tf.flags = ATA_TFLAG_LBA48;
	} else {
		qc->tf.command = ATA_CMD_FLUSH;
	}
	qc->tf.flags |= ATA_TFLAG_DEVICE;
	qc->tf.device = ATA_LBA;
	qc->protocol = ATA_PROT_NODATA;
	qc->dma_dir = ATA_DIR_NONE;
	qc->flags |= ATA_QCFLAG_RESULT_TF;
	qc->timeout_ms = ATA_TMOUT_FLUSH_MS;
}

static int ata_io_prio(const struct ata_io *io)
{
	struct ata_device *dev = io->dev;
	struct ata_dev_priv *p = ata_dev_priv(dev);

	if (!p || !p->policy.ncq_prio)
		return 0;
	if (!(dev->id[ATA_ID_SATA_CAPABILITY] & ATA_ID_SATA_NCQ_PRIO))
		return 0;
	if ((io->req->flags & (BLK_REQ_SYNC | BLK_REQ_META)) ==
	    (BLK_REQ_SYNC | BLK_REQ_META))
		return 2; /* high priority */
	return 0;
}

static void ata_io_qc_done(struct ata_queued_cmd *qc);

/* Build the command for the lowest pending step.  0 built, 1 the step is
 * a no-op (dropped), -errno. */
static int ata_io_build(struct ata_io *io, uint32_t step,
			struct ata_queued_cmd *qc)
{
	struct ata_device *dev = io->dev;
	struct blk_request *req = io->req;
	struct ata_dev_priv *p = ata_dev_priv(dev);
	int rc;

	qc->req = req;
	qc->private_data = io;
	qc->end_io = ata_io_qc_done;
	qc->flags |= ATA_QCFLAG_IO;
	if (req->flags & (BLK_REQ_FAILFAST | BLK_REQ_RAHEAD)) {
		qc->max_retries = 0;
	} else {
		qc->flags |= ATA_QCFLAG_RETRY;
		qc->max_retries = ATA_MAX_RETRIES;
	}
	/* A retry error handling asked for counts on. */
	qc->retries = io->qc_retries;
	if (req->flags & BLK_REQ_QUIET)
		qc->flags |= ATA_QCFLAG_QUIET;
	if (req->flags & BLK_REQ_POLLED)
		qc->flags |= ATA_QCFLAG_POLLED;

	switch (step) {
	case ATA_IO_STEP_FLUSH:
		if (!(dev->flags & (ATA_DFLAG_FLUSH | ATA_DFLAG_FLUSH_EXT)) ||
		    !(dev->flags & ATA_DFLAG_WCACHE_ON))
			return 1; /* nothing cached: nothing to flush */
		ata_io_fill_flush_tf(dev, qc);
		if (req->timeout_ms > qc->timeout_ms)
			qc->timeout_ms = req->timeout_ms;
		io->chunk_sectors = 0;
		io->chunk_bytes = 0;
		return 0;

	case ATA_IO_STEP_DATA: {
		struct blk_sg_entry *sg = NULL;
		uint32_t nsg = 0, n;
		uint32_t want = io->remaining > dev->max_sectors ?
					dev->max_sectors :
					(uint32_t)io->remaining;

		n = ata_io_build_chunk(io, want, &sg, &nsg);
		if (n == 0) {
			ata_dev_printk(dev, "request's buffer list cannot "
					    "carry one sector (nsg %u)\n",
				       req->nsg);
			return -EINVAL;
		}
		/* One request is one command: the block layer split it at
		 * this disk's limits (max_sectors, max_segments). */
		if (WARN_RATELIMIT(n != io->remaining,
				   "[ata] %s: request of %llu sectors exceeds one command (%u)",
				   dev->name,
				   (unsigned long long)io->remaining, n))
			return -EIO;
		rc = ata_rw_build_tf_ex(dev, qc, io->lba, n, io->write,
					io->fua_native, 1, ata_io_prio(io));
		if (rc)
			return rc;
		qc->sg = sg;
		qc->n_elem = nsg;
		qc->nbytes = n * io->lss;
		qc->timeout_ms = req->timeout_ms ? req->timeout_ms :
						   ATA_TMOUT_RW_MS;
		io->chunk_sectors = n;
		return 0;
	}

	case ATA_IO_STEP_ZERO: {
		uint32_t nsg = 0, n;
		uint32_t want = io->remaining > dev->max_sectors ?
					dev->max_sectors :
					(uint32_t)io->remaining;

		if (!p || !p->zero_buf)
			return -ENOMEM;
		n = ata_io_build_zero_chunk(io, want, &nsg);
		if (n == 0)
			return -EINVAL;
		if (WARN_RATELIMIT(n != io->remaining,
				   "[ata] %s: zeroing of %llu sectors exceeds one command (%u)",
				   dev->name,
				   (unsigned long long)io->remaining, n))
			return -EIO;
		rc = ata_rw_build_tf_ex(dev, qc, io->lba, n, 1, 0, 1, 0);
		if (rc)
			return rc;
		qc->sg = io->chunk_sg;
		qc->n_elem = nsg;
		qc->nbytes = n * io->lss;
		qc->timeout_ms = req->timeout_ms ? req->timeout_ms :
						   ATA_TMOUT_RW_MS;
		io->chunk_sectors = n;
		return 0;
	}

	case ATA_IO_STEP_TRIM: {
		uint64_t covered;

		if (!io->dsm_buf)
			return -ENOMEM;
		kmemset(io->dsm_buf, 0, io->dsm_blocks * ATA_SECT_SIZE);
		covered = ata_trim_encode(io->dsm_buf, io->dsm_blocks, io->lba,
					  io->remaining);
		if (WARN_RATELIMIT(covered != io->remaining,
				   "[ata] %s: TRIM of %llu sectors exceeds one command (%llu)",
				   dev->name,
				   (unsigned long long)io->remaining,
				   (unsigned long long)covered))
			return -EIO;
		rc = ata_trim_build_tf(dev, qc, io->dsm_blocks);
		if (rc)
			return rc;
		ata_qc_set_buf(qc, io->dsm_buf, io->dsm_blocks * ATA_SECT_SIZE,
			       ATA_DIR_TO_DEVICE);
		if (!qc->timeout_ms || qc->timeout_ms < ATA_TMOUT_TRIM_MS)
			qc->timeout_ms = ATA_TMOUT_TRIM_MS;
		qc->lba = io->lba;
		qc->n_sect = (uint32_t)(covered > 0xFFFFFFFFULL ? 0xFFFFFFFFULL :
								 covered);
		io->chunk_sectors = (uint32_t)(covered > 0xFFFFFFFFULL ?
						       0xFFFFFFFFULL :
						       covered);
		io->chunk_bytes = 0;
		if (p)
			__atomic_add_fetch(&p->n_trims, 1, __ATOMIC_RELAXED);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

/* Prepare the request's prepared pass-through command for issue. */
static int ata_io_prepare_drv(struct ata_io *io, struct ata_queued_cmd **out)
{
	struct blk_request *req = io->req;
	struct ata_queued_cmd *qc = req->drv_cmd;

	if (!qc || qc->magic != ATA_QC_MAGIC || qc->dev != io->dev)
		return -EINVAL;
	if (qc->flags & ATA_QCFLAG_ACTIVE)
		return -EBUSY;
	/* A re-issue (after -ECANCELED) only once the previous completion
	 * has let go of the command entirely. */
	if (io->drv_base_refs &&
	    refcount_read(&qc->refcount) > io->drv_base_refs)
		return -EBUSY;
	if (!io->drv_base_refs)
		io->drv_base_refs = refcount_read(&qc->refcount);
	qc->flags &= ~(ATA_QCFLAG_COMPLETE | ATA_QCFLAG_FAILED |
		       ATA_QCFLAG_TIMEDOUT | ATA_QCFLAG_ABORTED |
		       ATA_QCFLAG_SETTLED | ATA_QCFLAG_WAITER |
		       ATA_QCFLAG_EH_RETRY);
	qc->flags |= ATA_QCFLAG_IO;
	qc->result = 0;
	qc->err_mask = 0;
	qc->resid = 0;
	qc->tag = -1;
	if (ata_completion_done(&qc->done))
		ata_completion_reinit(&qc->done);
	qc->req = req;
	qc->private_data = io;
	qc->end_io = ata_io_qc_done;
	if (req->timeout_ms)
		qc->timeout_ms = req->timeout_ms;
	*out = qc;
	return 0;
}

/* Issue the next command of a request.  0 issued (or nothing left to
 * issue for a no-op step -- see the loop), -EAGAIN not possible right now,
 * other -errno: the request cannot proceed. */
static int ata_io_issue_step(struct ata_io *io)
{
	struct ata_device *dev = io->dev;
	struct ata_dev_priv *p = ata_dev_priv(dev);
	struct ata_queued_cmd *qc;
	uint32_t step;
	int rc;

	for (;;) {
		if (unlikely(dev->flags &
			     (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED)))
			return -ENODEV;
		if (!io->steps)
			return 1; /* done */
		step = io->steps & (~io->steps + 1); /* lowest bit */

		if (unlikely(dev->flags & ATA_DFLAG_SLEEPING)) {
			/* Only a reset wakes a sleeping device: ask error
			 * handling for one; the revalidation that follows
			 * clears SLEEPING and kicks us. */
			if (dev->xport->schedule_eh)
				dev->xport->schedule_eh(dev,
							AHCI_EH_RESET_SOFT);
			return -EAGAIN;
		}

		if (step == ATA_IO_STEP_DRV) {
			rc = ata_io_prepare_drv(io, &qc);
			if (rc == -EBUSY)
				return -EAGAIN;
			if (rc)
				return rc;
			io->cur_step = step;
			rc = dev->xport->qc_issue(qc);
			if (rc == 0)
				return 0;
			qc->end_io = NULL;
			qc->private_data = NULL;
			qc->req = NULL;
			if (rc == -EBUSY || rc == -EAGAIN)
				return -EAGAIN;
			return rc;
		}

		qc = ata_rw_qc_get(dev);
		if (!qc)
			return -EAGAIN;
		rc = ata_io_build(io, step, qc);
		if (rc == 1) {
			/* nothing to do for this step */
			ata_qc_put(qc);
			io->steps &= ~step;
			continue;
		}
		if (rc) {
			ata_qc_put(qc);
			return rc;
		}
		io->cur_step = step;
		rc = dev->xport->qc_issue(qc);
		if (likely(rc == 0)) {
			if (p)
				__atomic_add_fetch(&p->n_commands, 1,
						   __ATOMIC_RELAXED);
			return 0;
		}
		/* never became active: the owner's put returns it */
		ata_qc_put(qc);
		if (rc == -EBUSY || rc == -EAGAIN)
			return -EAGAIN;
		return rc;
	}
}

/* Complete the block request and release the context.  The block softirq
 * (or the dispatching task, for a request that turned out to need no
 * command, or that cannot go on). */
static void ata_io_finish(struct ata_io *io, int status)
{
	struct blk_request *req = io->req;
	uint32_t resid = 0;

	if (blk_op_has_data(req->op) && req->op != BLK_OP_DRV_IN &&
	    req->op != BLK_OP_DRV_OUT) {
		uint64_t total = (uint64_t)req->count * io->lss;

		if (io->bytes_done < total)
			resid = (uint32_t)(total - io->bytes_done);
	}
	if (status && !io->status)
		io->status = status;
	status = io->status;
	req->retries = io->retries;
	req->driver_data = NULL;
	ata_io_put(io);
	blk_request_complete(req, status, resid);
}

/* The request has more to do (a further step, the same step once more):
 * back to the head of its queue with this context attached; kblockd
 * dispatches it, and ata_blk_submit issues the next command. */
static void ata_io_continue_later(struct ata_io *io)
{
	struct ata_dev_priv *p = ata_dev_priv(io->dev);
	struct blk_request *req = io->req;

	BUG_ON(!req->queue);
	if (p)
		__atomic_add_fetch(&p->n_continued, 1, __ATOMIC_RELAXED);
	blk_queue_continue(req->queue, req);
}

/* The end_io of every command a request issues: inside ata_qc_complete,
 * in the interrupt handler with the port lock held.  It only notes which
 * command finished and hands the request to the block softirq; the
 * transport's reference is gone already, the pool command is the
 * context's until ata_blk_complete is done with it. */
static void ata_io_qc_done(struct ata_queued_cmd *qc)
{
	struct ata_io *io = qc->private_data;

	BUG_ON(!io || io->magic != ATA_IO_MAGIC);
	VM_BUG_ON(io->done_qc != NULL);
	io->done_qc = qc;
	blk_complete_request(io->req, qc->result, qc->resid);
}

/* The block softirq's half (ops->complete): the result of the command
 * that finished decides how the request goes on. */
static void ata_blk_complete(struct blk_request *req)
{
	struct ata_io *io = req->driver_data;
	struct ata_queued_cmd *qc;
	struct ata_device *dev;
	struct ata_dev_priv *p;
	uint32_t step;
	int rc, pool;

	BUG_ON(!io || io->magic != ATA_IO_MAGIC || io->req != req);
	qc = io->done_qc;
	BUG_ON(!qc);
	io->done_qc = NULL;
	dev = qc->dev;
	p = ata_dev_priv(dev);
	step = io->cur_step;
	rc = qc->result;
	pool = !!(qc->flags & ATA_QCFLAG_ORIGIN_POOL);
	if (qc->tag >= 0)
		req->driver_tag = (uint32_t)qc->tag;
	/* A failure the transport already reported as expected (an
	 * injected fault) is not reported again by the block layer. */
	if (rc && (qc->flags & ATA_QCFLAG_QUIET) &&
	    !(qc->flags & ATA_QCFLAG_EH_RETRY))
		req->flags |= BLK_REQ_QUIET;

	if (qc->flags & ATA_QCFLAG_EH_RETRY) {
		/* Error handling ended the command for a retry: the same step
		 * goes out again through the queue, its retry count carried
		 * to the command that re-issues it. */
		io->qc_retries = qc->retries;
		if (step == ATA_IO_STEP_DRV) {
			qc->end_io = NULL;
			qc->private_data = NULL;
		} else if (pool) {
			ata_qc_put(qc);
		}
		ata_io_continue_later(io);
		return;
	}
	/* A step that came back for good starts the next one afresh. */
	io->qc_retries = 0;

	if (step == ATA_IO_STEP_DRV && rc == -ECANCELED &&
	    io->retries < ATA_IO_REQUEUE_MAX) {
		/* Requeue: the same prepared command goes out again, once
		 * kblockd dispatches the request anew. */
		io->retries++;
		qc->end_io = NULL;
		qc->private_data = NULL;
		ata_io_continue_later(io);
		return;
	}
	if (step == ATA_IO_STEP_DRV) {
		uint32_t resid = qc->resid;

		/* The preparer keeps its command (and reads its result);
		 * we only drop our hooks. */
		qc->end_io = NULL;
		qc->private_data = NULL;
		io->steps = 0;
		io->status = rc;
		req->retries = io->retries;
		req->driver_data = NULL;
		ata_io_put(io);
		blk_request_complete(req, rc, resid);
		return;
	}

	if (rc == 0) {
		/* The request's one command is done. */
		switch (step) {
		case ATA_IO_STEP_DATA:
		case ATA_IO_STEP_ZERO:
			io->lba += io->chunk_sectors;
			io->remaining -= io->chunk_sectors;
			io->bytes_done += io->chunk_bytes;
			ata_pm_note_media_access(dev);
			break;
		case ATA_IO_STEP_TRIM:
			io->lba += io->chunk_sectors;
			io->remaining -= io->chunk_sectors;
			break;
		default: /* the flush */
			break;
		}
		VM_WARN_ON(io->remaining != 0);
		io->steps &= ~step;
	} else if (rc == -ECANCELED && io->retries < ATA_IO_REQUEUE_MAX) {
		/* Requeue: cancelled by recovery (an innocent victim) or a
		 * re-issue that could not run.  Not a failure, whatever the
		 * request's retry policy: the same step goes out again. */
		io->retries++;
	} else if (step == ATA_IO_STEP_FLUSH &&
		   rc == -EIO && (qc->result_tf.status & ATA_ERR) &&
		   !ata_tf_is_abort_only(&qc->result_tf)) {
		/* FLUSH CACHE hit a write error.  The device reported the
		 * first LBA it could not write and dropped that data; a new
		 * FLUSH carries on with the rest of the cache.  The request
		 * fails in the end (data was lost), but only after the
		 * remaining cache contents made it to the media. */
		struct ata_taskfile r = qc->result_tf;
		uint64_t bad;

		r.flags = qc->tf.flags & ATA_TFLAG_LBA48;
		bad = ata_tf_read_lba(&r);
		io->status = -EIO;
		if (p)
			__atomic_add_fetch(&p->n_flush_retries, 1,
					   __ATOMIC_RELAXED);
		WARN_RATELIMIT(1, "[ata] %s: %s failed at LBA %llu: cached "
				  "data lost, continuing the flush",
			       dev->name, ata_cmd_name(qc->tf.command),
			       (unsigned long long)bad);
		if (io->flush_tries < ATA_FLUSH_MAX_TRIES &&
		    bad != io->flush_bad_lba) {
			io->flush_tries++;
			io->flush_bad_lba = bad;
			/* the step stays: the same FLUSH goes out again */
		} else {
			ata_dev_printk(dev, "flush makes no progress (LBA "
					    "%llu again), giving up\n",
				       (unsigned long long)bad);
			io->steps = 0;
		}
	} else {
		if (rc == -ECANCELED) {
			ata_dev_printk(dev, "request cancelled %u times by "
					    "recovery, failing it\n",
				       io->retries);
			rc = -EIO;
		}
		io->status = rc;
		io->steps = 0;
	}
	/* Drop the owner's reference of the pool command (the transport's
	 * went in ata_qc_complete): it may go back to the pool now. */
	if (pool)
		ata_qc_put(qc);

	if (io->steps == 0) {
		ata_io_finish(io, io->status);
		return;
	}
	ata_io_continue_later(io);
}

/* A request that waits for its next step was failed by its queue (the
 * disk went away): the context goes back to the pool (ops->cleanup_rq). */
static void ata_blk_cleanup_rq(struct blk_request *req)
{
	struct ata_io *io = req->driver_data;

	if (!io)
		return;
	BUG_ON(io->magic != ATA_IO_MAGIC || io->req != req);
	req->driver_data = NULL;
	ata_io_put(io);
}

/* The queue's timeout work found the request expired (ops->timeout): the
 * transport looks for its command under the port lock and hands it to
 * error handling.  Not found: its completion is under way. */
static struct ata_device *ata_disk_dev(struct gendisk *disk);

static enum blk_eh_timer_return ata_blk_timeout(struct gendisk *disk,
						struct blk_request *req,
						void *driver_data)
{
	struct ata_device *dev = ata_disk_dev(disk);

	(void)driver_data;
	if (!dev || !dev->xport || !dev->xport->qc_timed_out)
		return BLK_EH_RESET_TIMER;
	(void)dev->xport->qc_timed_out(dev, NULL, req);
	return BLK_EH_DONE;
}


/* ==========================================================================
 * Block operations
 * ========================================================================== */

static struct ata_device *ata_disk_dev(struct gendisk *disk)
{
	struct ata_device *dev;

	if (unlikely(!disk || disk->magic != GENDISK_MAGIC))
		return NULL;
	dev = disk->private_data;
	if (unlikely(!dev || dev->magic != ATA_DEVICE_MAGIC))
		return NULL;
	return dev;
}

/* May WRITE ZEROES be done by TRIM on this device? */
static int ata_dev_zeroes_by_trim(const struct ata_device *dev)
{
	uint64_t need = ATA_DFLAG_TRIM | ATA_DFLAG_TRIM_DRAT |
			ATA_DFLAG_TRIM_RZAT;

	return (dev->flags & need) == need &&
	       (ata_dev_quirks(dev) & ATA_QUIRK_ZERO_AFTER_TRIM);
}

static int ata_io_take_dsm_buf(struct ata_io *io)
{
	struct ata_dev_priv *p = ata_dev_priv(io->dev);
	int slot;

	if (!p || !p->dsm_bufs[0])
		return -EOPNOTSUPP;
	slot = ata_bitmap_alloc(&p->dsm_map, ATA_DSM_BUFS);
	if (slot < 0)
		return -EBUSY;
	io->dsm_slot = slot;
	io->dsm_buf = p->dsm_bufs[slot];
	io->dsm_blocks = p->dsm_buf_blocks;
	return 0;
}

int ata_blk_submit(struct gendisk *disk, struct blk_request *req)
{
	struct ata_device *dev = ata_disk_dev(disk);
	struct ata_dev_priv *p;
	struct ata_io *io;
	int rc;

	if (unlikely(!dev || !req))
		return -ENODEV;
	VM_BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	p = ata_dev_priv(dev);

	/* The next step of a request already under way: ata_blk_complete
	 * gave it back with its context, and this dispatch issues the step.
	 * Not possible right now: -EBUSY, the context stays with the
	 * request, and the queue tries again after the next completion. */
	if (req->driver_data) {
		io = req->driver_data;
		BUG_ON(io->magic != ATA_IO_MAGIC || io->req != req);
		if (unlikely(dev->flags &
			     (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED))) {
			ata_io_finish(io, -ENODEV);
			return 0;
		}
		rc = ata_io_issue_step(io);
		if (rc == 0)
			return 0;
		if (rc == -EAGAIN)
			return -EBUSY;
		ata_io_finish(io, rc == 1 ? 0 : rc);
		return 0;
	}

	if (unlikely(!p || !p->pools_ready))
		return -ENODEV;
	if (unlikely(dev->flags & (ATA_DFLAG_DETACHED | ATA_DFLAG_DISABLED)))
		return -ENODEV;
	ata_note_system_up();

	io = ata_io_get(dev);
	if (unlikely(!io))
		return -EBUSY; /* every context is in flight */
	io->req = req;
	io->lba = req->sector;
	io->remaining = req->count;
	req->driver_data = io;
	__atomic_add_fetch(&p->n_requests, 1, __ATOMIC_RELAXED);

	switch (req->op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE: {
		uint64_t total = 0;

		if (req->count == 0 || !req->sg || !req->nsg) {
			rc = -EINVAL;
			goto reject;
		}
		if (ata_rw_check_range(dev, req->sector, req->count)) {
			rc = -EINVAL;
			goto reject;
		}
		for (uint32_t i = 0; i < req->nsg; i++)
			total += req->sg[i].len;
		if (total < (uint64_t)req->count * io->lss) {
			WARN_RATELIMIT(1, "[ata] %s: request of %u sectors "
					  "carries only %llu bytes",
				       dev->name, req->count,
				       (unsigned long long)total);
			rc = -EINVAL;
			goto reject;
		}
		io->write = (req->op == BLK_OP_WRITE);
		io->steps = ATA_IO_STEP_DATA;
		/* The flushes a request asks for (PREFLUSH, FUA without
		 * native FUA) are the block layer's flush sequence: what
		 * arrives here is the data command alone. */
		if (req->flags & BLK_REQ_FUA) {
			int native = (dev->flags & ATA_DFLAG_FUA) &&
				     dev->host_fua &&
				     (dev->flags & ATA_DFLAG_LBA48) &&
				     (dev->id[ATA_ID_CAPABILITY] &
				      ATA_ID_CAP_DMA);

			if (io->write && native)
				io->fua_native = 1;
			else if (io->write)
				WARN_ON_ONCE(dev->flags & ATA_DFLAG_WCACHE_ON);
			/* reads: FUA only means "from the media"; native
			 * queued reads carry it, others ignore it */
			if (!io->write && native &&
			    (dev->flags & ATA_DFLAG_NCQ_ON))
				io->fua_native = 1;
		}
		break;
	}
	case BLK_OP_FLUSH:
		io->steps = ATA_IO_STEP_FLUSH;
		io->remaining = 0;
		break;
	case BLK_OP_DISCARD:
		if (!(dev->flags & ATA_DFLAG_TRIM)) {
			rc = -EOPNOTSUPP;
			goto reject;
		}
		if (req->count == 0 ||
		    ata_rw_check_range(dev, req->sector, req->count)) {
			rc = -EINVAL;
			goto reject;
		}
		rc = ata_io_take_dsm_buf(io);
		if (rc)
			goto reject;
		io->steps = ATA_IO_STEP_TRIM;
		io->write = 1;
		break;
	case BLK_OP_SECURE_ERASE:
		/* TRIM does not promise the data is gone. */
		rc = -EOPNOTSUPP;
		goto reject;
	case BLK_OP_WRITE_ZEROES:
		if (req->count == 0 ||
		    ata_rw_check_range(dev, req->sector, req->count)) {
			rc = -EINVAL;
			goto reject;
		}
		io->write = 1;
		if (!(req->flags & BLK_REQ_NOUNMAP) &&
		    ata_dev_zeroes_by_trim(dev) &&
		    ata_io_take_dsm_buf(io) == 0) {
			io->steps = ATA_IO_STEP_TRIM;
		} else {
			io->steps = ATA_IO_STEP_ZERO;
		}
		__atomic_add_fetch(&p->n_zeroes, 1, __ATOMIC_RELAXED);
		break;
	case BLK_OP_DRV_IN:
	case BLK_OP_DRV_OUT:
		if (!req->drv_cmd) {
			rc = -EINVAL;
			goto reject;
		}
		io->steps = ATA_IO_STEP_DRV;
		io->remaining = 0;
		break;
	default:
		rc = -EOPNOTSUPP;
		goto reject;
	}

	rc = ata_io_issue_step(io);
	if (rc == 0)
		return 0;
	if (rc == 1) {
		/* nothing at all to do (a flush without a write cache):
		 * done right away */
		ata_io_finish(io, 0);
		return 0;
	}
	if (rc == -EAGAIN) {
		/* No slot, a command that cannot run next to what runs, the
		 * port in error handling, the device asleep (error handling
		 * wakes it): the queue retries after the next completion,
		 * when error handling restarts it, or shortly when nothing is
		 * in flight. */
		rc = -EBUSY;
	}
reject:
	req->driver_data = NULL;
	ata_io_put(io);
	return rc;
}

int ata_blk_poll(struct gendisk *disk)
{
	struct ata_device *dev = ata_disk_dev(disk);
	int n = 0;

	if (!dev)
		return 0;
	if (dev->xport && dev->xport->poll)
		n = dev->xport->poll(dev);
	return n;
}

/* ==========================================================================
 * Synchronous helpers
 * ========================================================================== */

int ata_flush_cache(struct ata_device *dev)
{
	int sticky = 0;
	uint64_t last_bad = ~0ULL;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (!(dev->flags & (ATA_DFLAG_FLUSH | ATA_DFLAG_FLUSH_EXT)))
		return (dev->flags & ATA_DFLAG_WCACHE_ON) ? -EOPNOTSUPP : 0;

	for (int tries = 0; tries <= ATA_FLUSH_MAX_TRIES; tries++) {
		struct ata_taskfile tf;
		int ext = (dev->flags & ATA_DFLAG_FLUSH_EXT) &&
			  (dev->flags & ATA_DFLAG_LBA48);
		uint64_t bad;
		int rc;

		kmemset(&tf, 0, sizeof(tf));
		tf.command = ext ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH;
		tf.flags = ATA_TFLAG_DEVICE | (ext ? ATA_TFLAG_LBA48 : 0);
		tf.device = ATA_LBA;
		rc = ata_exec_nodata(dev, &tf, ATA_TMOUT_FLUSH_MS, 0);
		if (rc == 0)
			return sticky;
		if (rc != -EIO || !(tf.status & ATA_ERR) ||
		    ata_tf_is_abort_only(&tf))
			return rc;
		/* Write error while flushing: data at `bad' is lost, the
		 * next FLUSH continues with the rest of the cache. */
		tf.flags = ext ? ATA_TFLAG_LBA48 : 0;
		bad = ata_tf_read_lba(&tf);
		ata_dev_printk(dev, "%s: write error at LBA %llu, cached data "
				    "for it is lost; flushing the rest\n",
			       ata_cmd_name(ext ? ATA_CMD_FLUSH_EXT :
						  ATA_CMD_FLUSH),
			       (unsigned long long)bad);
		sticky = -EIO;
		if (bad == last_bad) {
			ata_dev_printk(dev, "flush stuck at LBA %llu\n",
				       (unsigned long long)bad);
			return -EIO;
		}
		last_bad = bad;
	}
	return -EIO;
}

/* A short pause before re-issuing a command recovery cancelled. */
static void ata_rw_sync_backoff(void)
{
	if (ata_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + 10ULL * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0)
			;
	} else {
		lapic_delay_us(10000);
	}
}

static int ata_rw_sync(struct ata_device *dev, uint64_t lba, uint32_t count,
		       void *buf, int write)
{
	uint32_t lss;
	uint8_t *p = buf;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !buf)
		return -EINVAL;
	if (ata_can_sleep())
		might_sleep();
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;
	if (ata_rw_check_range(dev, lba, count))
		return -EINVAL;
	lss = dev->logical_sector_size ? dev->logical_sector_size :
					 ATA_SECT_SIZE;

	while (count) {
		struct ata_queued_cmd *qc;
		uint32_t n = count;
		uint32_t prd_limit = ata_dev_prd_limit(dev);
		uint64_t reach;
		int rc;

		/* within the device's per-command limit, and within what
		 * the PRD table reaches with 4 KiB pages */
		if (n > dev->max_sectors)
			n = dev->max_sectors;
		reach = (uint64_t)(prd_limit > 1 ? prd_limit - 1 : 1) * 4096;
		if ((uint64_t)n * lss > reach)
			n = (uint32_t)(reach / lss);
		if (n == 0)
			n = 1;

		qc = ata_qc_alloc(dev);
		if (!qc)
			return -ENOMEM;
		rc = ata_rw_build_tf_ex(dev, qc, lba, n, write, 0, 0, 0);
		if (rc) {
			ata_qc_free(qc);
			return rc;
		}
		ata_qc_set_buf(qc, p, n * lss,
			       write ? ATA_DIR_TO_DEVICE : ATA_DIR_FROM_DEVICE);
		qc->flags |= ATA_QCFLAG_INTERNAL | ATA_QCFLAG_RETRY;
		/* -ECANCELED: requeue (recovery cancelled it), not fail */
		for (int tries = 0;; tries++) {
			rc = ata_qc_exec(qc);
			if (rc != -ECANCELED || tries >= ATA_EXEC_REQUEUE_MAX)
				break;
			ata_rw_sync_backoff();
		}
		/* ata_qc_exec never returns with the command still owned by
		 * the transport, so the buffer is ours again here. */
		VM_WARN_ON(qc->flags & ATA_QCFLAG_ACTIVE);
		ata_qc_free(qc);
		if (rc)
			return rc;
		ata_pm_note_media_access(dev);
		lba += n;
		count -= n;
		p += (uint64_t)n * lss;
	}
	return 0;
}

int ata_read_sectors(struct ata_device *dev, uint64_t lba, uint32_t count,
		     void *buf)
{
	return ata_rw_sync(dev, lba, count, buf, 0);
}

int ata_write_sectors(struct ata_device *dev, uint64_t lba, uint32_t count,
		      const void *buf)
{
	return ata_rw_sync(dev, lba, count, (void *)buf, 1);
}

/* ==========================================================================
 * Disk registration
 * ========================================================================== */

static void ata_rw_fill_limits(const struct ata_device *dev,
			       struct blk_limits *lim)
{
	uint32_t lss = dev->logical_sector_size ? dev->logical_sector_size :
						  ATA_SECT_SIZE;
	uint32_t pss = dev->physical_sector_size ? dev->physical_sector_size :
						   lss;

	kmemset(lim, 0, sizeof(*lim));
	lim->logical_block_size = lss;
	lim->physical_block_size = pss;
	lim->io_min = pss;
	lim->io_opt = 0;
	lim->alignment_offset = dev->alignment_offset;
	/* in logical sectors, like every count of this disk */
	lim->max_hw_sectors = dev->max_sectors;
	lim->max_sectors = dev->max_sectors;
	lim->max_segments = ata_dev_prd_limit(dev);
	lim->max_segment_size = ATA_PRD_MAX_BYTES;
	lim->seg_boundary_mask = ~0ULL;
	lim->dma_alignment = dev->host_dma_alignment ? dev->host_dma_alignment :
						       1;
	if (dev->flags & ATA_DFLAG_TRIM) {
		uint64_t max = dev->trim_max_sectors;

		uint32_t blocks = dev->dsm_max_blocks;

		lim->discard_granularity = lss;
		lim->discard_alignment = 0;
		/* What one DSM command carries (the payload blocks the device
		 * takes, at most the driver's buffer): the block layer splits
		 * larger discards. */
		if (blocks > ATA_DSM_BUF_BLOCKS_MAX)
			blocks = ATA_DSM_BUF_BLOCKS_MAX;
		if (!blocks)
			blocks = 1;
		max = max ? max * blocks : 0;
		if (max > 0xFFFFFFFFULL)
			max = 0xFFFFFFFFULL;
		lim->max_discard_sectors = max;
		lim->max_discard_segments = 1;
	}
	/* One write from the zero buffer per request. */
	lim->max_write_zeroes_sectors = ATA_ZERO_BUF_BYTES / lss;
	lim->queue_depth = (dev->flags & ATA_DFLAG_NCQ_ON) && dev->ncq_depth ?
				   dev->ncq_depth :
				   1;
	lim->rotational = dev->rotation_rate != 1;
	lim->write_cache = !!(dev->flags & ATA_DFLAG_WCACHE_ON);
	lim->fua = (dev->flags & ATA_DFLAG_FUA) && dev->host_fua &&
		   (dev->flags & ATA_DFLAG_LBA48) &&
		   (dev->id[ATA_ID_CAPABILITY] & ATA_ID_CAP_DMA);
	lim->discard_zeroes_data = ata_dev_zeroes_by_trim(dev);
	lim->secure_erase = 0;
}

static void ata_rw_fill_bdev(const struct ata_device *dev,
			     block_device_t *b, const struct blk_limits *lim)
{
	b->physical_sector_size = lim->physical_block_size;
	b->alignment_offset = lim->alignment_offset;
	b->io_min = lim->io_min;
	b->io_opt = lim->io_opt;
	b->max_sectors = lim->max_sectors;
	b->queue_depth = lim->queue_depth;
	b->discard_granularity = lim->discard_granularity;
	b->discard_max_sectors = lim->max_discard_sectors;
	b->write_zeroes_max_sectors = lim->max_write_zeroes_sectors;
	b->rotational = lim->rotational;
	b->flush_supported = lim->write_cache &&
			     (dev->flags & (ATA_DFLAG_FLUSH |
					    ATA_DFLAG_FLUSH_EXT));
	b->fua_supported = lim->fua;
	b->discard_zeroes_data = lim->discard_zeroes_data;
	b->removable = !!(dev->flags & ATA_DFLAG_REMOVABLE);
}

void ata_rw_update_disk_limits(struct ata_device *dev)
{
	struct gendisk *disk = dev->disk;
	struct blk_limits lim;
	uint32_t old_depth;

	if (!disk || dev->class == ATA_DEV_ATAPI)
		return;
	ata_rw_fill_limits(dev, &lim);
	old_depth = disk->limits.queue_depth;
	disk->limits.write_cache = lim.write_cache;
	disk->limits.fua = lim.fua;
	disk->limits.queue_depth = lim.queue_depth;
	disk->limits.discard_zeroes_data = lim.discard_zeroes_data;
	if (disk->queue) {
		disk->queue->limits.write_cache = lim.write_cache;
		disk->queue->limits.fua = lim.fua;
		disk->queue->limits.discard_zeroes_data =
			lim.discard_zeroes_data;
		if (old_depth != lim.queue_depth)
			blk_queue_set_depth(disk->queue, lim.queue_depth);
	}
	disk->bdev.flush_supported =
		lim.write_cache &&
		(dev->flags & (ATA_DFLAG_FLUSH | ATA_DFLAG_FLUSH_EXT));
	disk->bdev.fua_supported = lim.fua;
	disk->bdev.queue_depth = lim.queue_depth;
}

int ata_disk_register(struct ata_device *dev)
{
	struct gendisk *disk;
	int rc;

	BUG_ON(!dev || dev->magic != ATA_DEVICE_MAGIC);
	if (ata_can_sleep())
		might_sleep();
	if (dev->class == ATA_DEV_ATAPI)
		return -EINVAL;
	if (dev->disk)
		return -EBUSY;
	if (dev->flags & ATA_DFLAG_DETACHED)
		return -ENODEV;

	rc = ata_rw_dev_init(dev);
	if (rc)
		return rc;
	disk = disk_alloc(BLK_DISK_SD, &ata_disk_blk_ops, dev);
	if (!disk) {
		ata_dev_printk(dev, "no disk name/number available\n");
		return -ENOMEM;
	}

	ata_rw_fill_limits(dev, &disk->limits);
	ata_rw_fill_bdev(dev, &disk->bdev, &disk->limits);
	disk->capacity = dev->n_sectors;
	ksnprintf(disk->vendor, sizeof(disk->vendor), "ATA     ");
	kstrncpy(disk->model, dev->model, sizeof(disk->model) - 1);
	disk->model[sizeof(disk->model) - 1] = '\0';
	kstrncpy(disk->serial, dev->serial, sizeof(disk->serial) - 1);
	disk->serial[sizeof(disk->serial) - 1] = '\0';
	kstrncpy(disk->firmware, dev->firmware, sizeof(disk->firmware) - 1);
	disk->firmware[sizeof(disk->firmware) - 1] = '\0';
	if (dev->wwn)
		ksnprintf(disk->wwn, sizeof(disk->wwn), "0x%016llx",
			  (unsigned long long)dev->wwn);
	else
		disk->wwn[0] = '\0';
	ksnprintf(disk->bus, sizeof(disk->bus), "ata");
	if (dev->xport == &ahci_ata_transport && dev->xport_priv) {
		struct ahci_link *link = ahci_dev_link(dev);

		if (link && link->port && link->port->hba)
			disk->pci = link->port->hba->pci;
	}
	disk->host_no = dev->port_no > 0 ? dev->port_no - 1 : 0;
	disk->channel = 0;
	disk->target = dev->devno;
	disk->lun = 0;
	disk->scsi_type = (dev->flags & ATA_DFLAG_ZAC) ? TYPE_ZBC : TYPE_DISK;
	if (dev->heads && dev->sectors) {
		disk->heads = dev->heads;
		disk->sectors_per_track = dev->sectors;
		disk->cylinders = dev->cylinders;
	}
	disk->flags |= GENHD_FL_MEDIA_PRESENT;
	/* Fixed media, configured and answering: ready for I/O (the root
	 * scan only considers ready devices; detach clears it). */
	disk->bdev.ready = 1;
	if (dev->flags & ATA_DFLAG_REMOVABLE)
		disk->flags |= GENHD_FL_REMOVABLE;
	if (dev->flags & ATA_DFLAG_ZAC)
		disk->flags |= GENHD_FL_RO;
	if (g_ata_system_up)
		disk->flags |= GENHD_FL_HOTPLUGGED;

	ata_dev_get(dev); /* the disk's reference, dropped by free_disk */
	dev->disk = disk;
	dev->flags |= ATA_DFLAG_REGISTERED;
	rc = ata_work_disk_add(dev, disk);
	if (rc) {
		ata_dev_printk(dev, "disk_add failed (%d)\n", rc);
		dev->flags &= ~ATA_DFLAG_REGISTERED;
		disk_put(disk); /* never added: free_disk drops our ref */
		return rc;
	}
	return 0;
}

void ata_disk_unregister(struct ata_device *dev)
{
	if (!dev || dev->magic != ATA_DEVICE_MAGIC)
		return;
	if (ata_can_sleep())
		might_sleep();
	ata_work_disk_del(dev);
	dev->flags &= ~ATA_DFLAG_REGISTERED;
}

static int ata_blk_revalidate(struct gendisk *disk)
{
	struct ata_device *dev = ata_disk_dev(disk);
	int took, rc;

	if (!dev)
		return -ENODEV;
	might_sleep();
	took = ata_mgmt_enter(dev);
	rc = ata_dev_revalidate(dev);
	ata_mgmt_exit(dev, took);
	return rc;
}

static int ata_blk_shutdown(struct gendisk *disk, enum blk_shutdown_mode mode)
{
	struct ata_device *dev = ata_disk_dev(disk);

	if (!dev)
		return -ENODEV;
	return ata_dev_shutdown(dev, mode);
}

static int ata_blk_suspend(struct gendisk *disk)
{
	struct ata_device *dev = ata_disk_dev(disk);

	if (!dev)
		return -ENODEV;
	return ata_dev_suspend(dev);
}

static int ata_blk_resume(struct gendisk *disk)
{
	struct ata_device *dev = ata_disk_dev(disk);

	if (!dev)
		return -ENODEV;
	return ata_dev_resume(dev);
}

static void ata_blk_free_disk(struct gendisk *disk)
{
	struct ata_device *dev = ata_disk_dev(disk);

	if (!dev)
		return;
	if (dev->disk == disk)
		dev->disk = NULL;
	disk->private_data = NULL;
	ata_dev_put(dev);
}

const struct blk_ops ata_disk_blk_ops = {
	.name = "ahci",
	.submit = ata_blk_submit,
	.complete = ata_blk_complete,
	.timeout = ata_blk_timeout,
	.cleanup_rq = ata_blk_cleanup_rq,
	.poll = ata_blk_poll,
	.ioctl = ata_sat_blk_ioctl,
	.ioctl_mode = ata_sat_blk_ioctl_mode,
	.open = NULL,
	.release = NULL,
	.check_events = NULL,
	.revalidate = ata_blk_revalidate,
	.shutdown = ata_blk_shutdown,
	.suspend = ata_blk_suspend,
	.resume = ata_blk_resume,
	.free_disk = ata_blk_free_disk,
};
