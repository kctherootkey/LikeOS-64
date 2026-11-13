// LikeOS - queue limits: validation, derived values and live updates
//
// A driver describes its device with struct blk_limits.  blk_validate_limits
// fills what it left open with the conventional defaults, derives the values
// the block layer itself uses (the usable atomic write units from the
// hardware ones, the zone append limit, the effective write cache), and
// refuses impossible combinations, as the established blk-settings rules do.
// queue_limits_start_update / queue_limits_commit_update change the limits
// of a live queue: the driver copies the current ones, changes them, and the
// commit validates them and swaps them in with the queue frozen (nothing in
// flight was built against the old ones).
//
// Units: every *_sectors field of the limits counts LOGICAL blocks (this
// block layer's rule: max_sectors, max_hw_sectors, max_discard_sectors,
// max_write_zeroes_sectors, chunk_sectors, max_[hw_]zone_append_sectors),
// except the derived atomic_write_max_sectors / atomic_write_boundary_sectors
// (512-byte sectors, as blk_validate_atomic_write takes them).  Zone
// reports (struct blk_zone) and the zone ioctls count 512-byte sectors;
// rq->append_sector counts logical blocks, like rq->sector.  Byte fields
// are bytes.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define BLK_DEF_MAX_SEGMENT_SIZE 65536u
#define BLK_DEF_SEG_BOUNDARY 0xFFFFFFFFULL
#define BLK_SAFE_MAX_SECTORS_512 255u /* the established safe default */
#define BLK_PAGE_SIZE 4096u

static inline int blk_is_pow2_64(uint64_t v)
{
	return v && !(v & (v - 1));
}

static inline uint32_t blk_rounddown_pow2(uint32_t v)
{
	uint32_t r = 1;

	if (!v)
		return 0;
	while (r <= v / 2)
		r <<= 1;
	return r;
}

/* The usable atomic write values from the hardware ones (the established
 * blk_validate_atomic_write_limits / blk_atomic_writes_update_limits). */
static void blk_validate_atomic_write_limits(struct blk_limits *lim)
{
	uint32_t lbs = lim->logical_block_size;
	uint64_t max_hw_bytes = (uint64_t)lim->max_hw_sectors * lbs;
	uint32_t hw_max_sectors = lim->atomic_write_hw_max >> 9;
	uint32_t boundary_sectors, unit_limit;
	uint64_t seg_bytes;

	if (!(lim->features & BLK_FEAT_ATOMIC_WRITES))
		goto unsupported;
	if (!lim->atomic_write_hw_max || lim->atomic_write_hw_max == 0xFFFFFFFFu)
		goto unsupported;
	if (WARN_ON_ONCE(!blk_is_pow2_64(lim->atomic_write_hw_unit_min)))
		goto unsupported;
	if (WARN_ON_ONCE(!blk_is_pow2_64(lim->atomic_write_hw_unit_max)))
		goto unsupported;
	if (WARN_ON_ONCE(lim->atomic_write_hw_unit_min >
			 lim->atomic_write_hw_unit_max))
		goto unsupported;
	if (WARN_ON_ONCE(lim->atomic_write_hw_unit_max >
			 lim->atomic_write_hw_max))
		goto unsupported;
	if (WARN_ON_ONCE(lim->chunk_sectors &&
			 hw_max_sectors > lim->chunk_sectors * (lbs / 512)))
		goto unsupported;
	boundary_sectors = lim->atomic_write_hw_boundary >> 9;
	if (boundary_sectors) {
		uint32_t a, b;

		if (WARN_ON_ONCE(lim->atomic_write_hw_max >
				 lim->atomic_write_hw_boundary))
			goto unsupported;
		/* Chunks and atomic boundaries must nest. */
		if (lim->chunk_sectors) {
			uint32_t c512 = lim->chunk_sectors * (lbs / 512);

			a = c512 > boundary_sectors ? c512 : boundary_sectors;
			b = c512 > boundary_sectors ? boundary_sectors : c512;
			if (WARN_ON_ONCE(a % b))
				goto unsupported;
		}
		if (!blk_is_pow2_64(boundary_sectors))
			goto unsupported;
	}
	/* A unit must fit one request: the size and segment limits. */
	seg_bytes = (uint64_t)lim->max_segments * BLK_PAGE_SIZE;
	unit_limit = (uint32_t)(max_hw_bytes < seg_bytes ? max_hw_bytes :
							   seg_bytes);
	if (max_hw_bytes > 0xFFFFFFFFULL && seg_bytes > 0xFFFFFFFFULL)
		unit_limit = 0x80000000u;
	unit_limit = blk_rounddown_pow2(unit_limit);
	lim->atomic_write_max_sectors =
		hw_max_sectors < (uint32_t)(max_hw_bytes >> 9) ?
			hw_max_sectors :
			(uint32_t)(max_hw_bytes >> 9);
	lim->atomic_write_unit_min = lim->atomic_write_hw_unit_min < unit_limit ?
					     lim->atomic_write_hw_unit_min :
					     unit_limit;
	lim->atomic_write_unit_max = lim->atomic_write_hw_unit_max < unit_limit ?
					     lim->atomic_write_hw_unit_max :
					     unit_limit;
	lim->atomic_write_boundary_sectors = boundary_sectors;
	return;

unsupported:
	lim->atomic_write_max_sectors = 0;
	lim->atomic_write_boundary_sectors = 0;
	lim->atomic_write_unit_min = 0;
	lim->atomic_write_unit_max = 0;
}

/* Zoned limits (the established blk_validate_zoned_limits). */
static int blk_validate_zoned_limits(struct blk_limits *lim)
{

	if (!(lim->features & BLK_FEAT_ZONED)) {
		if (WARN_ON_ONCE(lim->max_open_zones) ||
		    WARN_ON_ONCE(lim->max_active_zones) ||
		    WARN_ON_ONCE(lim->max_hw_zone_append_sectors))
			return -EINVAL;
		lim->zone_write_granularity = 0;
		lim->max_zone_append_sectors = 0;
		return 0;
	}
	if (WARN_ON_ONCE(!lim->chunk_sectors) ||
	    !blk_is_pow2_64(lim->chunk_sectors))
		return -EINVAL;
	/* An open zone limit above the active one means nothing. */
	if (lim->max_active_zones && lim->max_open_zones > lim->max_active_zones)
		lim->max_open_zones = lim->max_active_zones;
	if (!lim->zone_write_granularity ||
	    lim->zone_write_granularity < lim->logical_block_size)
		lim->zone_write_granularity = lim->logical_block_size;
	/* Native zone append: no larger than a zone or a request. */
	lim->max_zone_append_sectors = lim->max_hw_zone_append_sectors;
	if (lim->max_zone_append_sectors > lim->chunk_sectors)
		lim->max_zone_append_sectors = lim->chunk_sectors;
	if (lim->max_zone_append_sectors > lim->max_hw_sectors)
		lim->max_zone_append_sectors = lim->max_hw_sectors;
	return 0;
}

static int blk_validate_integrity_limits(struct blk_limits *lim)
{
	struct blk_integrity *bi = &lim->integrity;
	uint32_t md = bi->metadata_size ? bi->metadata_size : bi->tuple_size;
	uint32_t lexp = 9;

	while ((1u << lexp) < lim->logical_block_size)
		lexp++;
	if (!md) {
		mm_memset(bi, 0, sizeof(*bi));
		return 0;
	}
	if (!bi->metadata_size)
		bi->metadata_size = md;
	if (bi->tuple_size == 0 && md <= 255)
		bi->tuple_size = (uint8_t)md;
	switch (bi->csum_type) {
	case BLK_INTEGRITY_CSUM_NONE:
		if (bi->pi_tuple_size) {
			WARN(1, "pi_tuple_size set without a checksum");
			return -EINVAL;
		}
		break;
	case BLK_INTEGRITY_CSUM_CRC:
	case BLK_INTEGRITY_CSUM_IP:
		if (!bi->pi_tuple_size)
			bi->pi_tuple_size = 8;
		if (bi->pi_tuple_size != 8) {
			WARN(1, "16-bit guard PI needs an 8-byte tuple");
			return -EINVAL;
		}
		break;
	case BLK_INTEGRITY_CSUM_CRC64:
		if (!bi->pi_tuple_size)
			bi->pi_tuple_size = 16;
		if (bi->pi_tuple_size != 16) {
			WARN(1, "64-bit guard PI needs a 16-byte tuple");
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	if (bi->pi_offset + bi->pi_tuple_size > bi->metadata_size) {
		WARN(1, "PI tuple (offset %u, %u bytes) outside %u bytes of metadata",
		     bi->pi_offset, bi->pi_tuple_size, bi->metadata_size);
		return -EINVAL;
	}
	if (!bi->interval_exp)
		bi->interval_exp = (uint8_t)lexp;
	if (bi->interval_exp < 9 || bi->interval_exp > lexp)
		return -EINVAL;
	return 0;
}

int blk_validate_limits(struct blk_limits *lim)
{
	int rc;

	if (!lim)
		return -EINVAL;
	if (!lim->logical_block_size)
		lim->logical_block_size = 512;
	if (blk_validate_block_size(lim->logical_block_size))
		return -EINVAL;
	if (lim->physical_block_size < lim->logical_block_size)
		lim->physical_block_size = lim->logical_block_size;
	if (!blk_is_pow2_64(lim->physical_block_size))
		return -EINVAL;
	if (lim->io_min < lim->physical_block_size)
		lim->io_min = lim->physical_block_size;
	if (lim->io_opt)
		lim->io_opt -= lim->io_opt % lim->physical_block_size;
	if (lim->alignment_offset >= lim->physical_block_size)
		lim->alignment_offset %= lim->physical_block_size;

	/* Request size: the hardware, the device's own ceiling, the soft
	 * limit below them. */
	if (!lim->max_hw_sectors) {
		uint32_t per = lim->logical_block_size / 512;

		lim->max_hw_sectors = BLK_SAFE_MAX_SECTORS_512 / (per ? per : 1);
		if (!lim->max_hw_sectors)
			lim->max_hw_sectors = 1;
	}
	if (lim->max_dev_sectors && lim->max_hw_sectors > lim->max_dev_sectors)
		lim->max_hw_sectors = lim->max_dev_sectors;
	if (!lim->max_sectors || lim->max_sectors > lim->max_hw_sectors)
		lim->max_sectors = lim->max_hw_sectors;

	/* Segments. */
	if (!lim->max_segments)
		lim->max_segments = BLK_DEFAULT_MAX_SEGMENTS;
	if (lim->virt_boundary_mask) {
		/* Gaps are bounded by the boundary instead: one segment may
		 * then be as long as anything (the established rule). */
		if (WARN_ON_ONCE(lim->max_segment_size &&
				 lim->max_segment_size != 0xFFFFFFFFu))
			return -EINVAL;
		lim->max_segment_size = 0xFFFFFFFFu;
	} else if (!lim->max_segment_size) {
		lim->max_segment_size = BLK_DEF_MAX_SEGMENT_SIZE;
	}
	if (!lim->seg_boundary_mask)
		lim->seg_boundary_mask = BLK_DEF_SEG_BOUNDARY;
	if (WARN_ON_ONCE(lim->seg_boundary_mask < BLK_PAGE_SIZE - 1))
		return -EINVAL;
	if (lim->dma_alignment >= BLK_PAGE_SIZE)
		return -EINVAL;

	/* Discard and secure erase. */
	if (lim->max_hw_discard_sectors && !lim->max_discard_sectors)
		lim->max_discard_sectors = lim->max_hw_discard_sectors;
	if (lim->max_hw_discard_sectors &&
	    lim->max_discard_sectors > lim->max_hw_discard_sectors)
		lim->max_discard_sectors = lim->max_hw_discard_sectors;
	if (!lim->discard_granularity && lim->max_discard_sectors)
		lim->discard_granularity = lim->physical_block_size;
	if (lim->max_discard_sectors && !lim->max_discard_segments)
		lim->max_discard_segments = 1;
	if (lim->max_secure_erase_sectors)
		lim->secure_erase = 1;

	/* Features and their legacy flags agree, either way set.  FUA is
	 * meaningless without a write cache. */
	if (lim->write_cache)
		lim->features |= BLK_FEAT_WRITE_CACHE;
	if (lim->fua)
		lim->features |= BLK_FEAT_FUA;
	if (lim->rotational)
		lim->features |= BLK_FEAT_ROTATIONAL;
	if (!(lim->features & BLK_FEAT_WRITE_CACHE))
		lim->features &= ~BLK_FEAT_FUA;
	lim->write_cache = (lim->features & BLK_FEAT_WRITE_CACHE) &&
			   !(lim->lim_flags & BLK_FLAG_WRITE_CACHE_DISABLED);
	lim->fua = (lim->features & BLK_FEAT_FUA) && lim->write_cache;
	lim->rotational = (lim->features & BLK_FEAT_ROTATIONAL) != 0;

	rc = blk_validate_integrity_limits(lim);
	if (rc)
		return rc;
	rc = blk_validate_zoned_limits(lim);
	if (rc)
		return rc;
	blk_validate_atomic_write_limits(lim);
	if (!lim->max_write_streams)
		lim->write_stream_granularity = 0;
	return 0;
}

int blk_validate_atomic_write(const struct gendisk *disk, uint64_t sector,
			      uint32_t bytes)
{
	const struct blk_limits *l;

	if (!disk || !disk->queue)
		return -EINVAL;
	l = &disk->queue->limits;
	if (!(l->features & BLK_FEAT_ATOMIC_WRITES) || !l->atomic_write_unit_max)
		return -EOPNOTSUPP;
	if (!bytes || !blk_is_pow2_64(bytes))
		return -EINVAL;
	if (bytes < l->atomic_write_unit_min || bytes > l->atomic_write_unit_max)
		return -EINVAL;
	/* Naturally aligned to its own size. */
	if (((sector << 9) & ((uint64_t)bytes - 1)) != 0)
		return -EINVAL;
	/* Never across an atomic write boundary of the device. */
	if (l->atomic_write_boundary_sectors) {
		uint64_t b = l->atomic_write_boundary_sectors;
		uint64_t last = sector + (bytes >> 9) - 1;

		if (sector / b != last / b)
			return -EINVAL;
	}
	if ((bytes >> 9) > l->atomic_write_max_sectors)
		return -EINVAL;
	return 0;
}

/* The derived per-device fields of the block devices of a disk (the
 * legacy block_device_t view). Registry mutex held. */
static void blk_limits_to_bdevs_locked(struct gendisk *disk,
				       const struct blk_limits *l)
{
	block_device_t *b = &disk->bdev;

	b->physical_sector_size = l->physical_block_size;
	b->io_min = l->io_min;
	b->io_opt = l->io_opt;
	b->max_sectors = l->max_sectors;
	b->queue_depth = l->queue_depth;
	b->discard_granularity = l->discard_granularity;
	b->discard_max_sectors = l->max_discard_sectors;
	b->write_zeroes_max_sectors = l->max_write_zeroes_sectors;
	b->rotational = l->rotational;
	b->flush_supported = l->write_cache;
	b->fua_supported = l->fua;
	for (int i = 1; i <= DISK_MAX_PARTS; i++) {
		struct blk_partition *p = disk->part[i];

		if (!p)
			continue;
		p->bdev.physical_sector_size = l->physical_block_size;
		p->bdev.io_min = l->io_min;
		p->bdev.io_opt = l->io_opt;
		p->bdev.max_sectors = l->max_sectors;
		p->bdev.queue_depth = l->queue_depth;
		p->bdev.discard_granularity = l->discard_granularity;
		p->bdev.discard_max_sectors = l->max_discard_sectors;
		p->bdev.write_zeroes_max_sectors = l->max_write_zeroes_sectors;
		p->bdev.rotational = l->rotational;
		p->bdev.flush_supported = l->write_cache;
		p->bdev.fua_supported = l->fua;
	}
}

void queue_limits_start_update(struct blk_queue *q, struct blk_limits *lim)
{
	uint64_t fl;

	BUG_ON(q == NULL || lim == NULL);
	spin_lock_irqsave(&q->lock, &fl);
	*lim = q->limits;
	spin_unlock_irqrestore(&q->lock, fl);
}

int queue_limits_commit_update(struct blk_queue *q, const struct blk_limits *lim)
{
	struct blk_limits l;
	struct gendisk *disk;
	uint64_t fl;
	int frozen = 0, rc;

	if (!q || !lim)
		return -EINVAL;
	blk_might_sleep();
	l = *lim;
	rc = blk_validate_limits(&l);
	if (rc)
		return rc;
	disk = q->disk;
	/* A live disk cannot change its logical block size under its
	 * partitions and filesystems. */
	if (disk && (disk_flags(disk) & GENHD_FL_UP) &&
	    l.logical_block_size != q->limits.logical_block_size &&
	    disk->nr_parts) {
		kprintf(BLK_LOG_PREFIX "%s: logical block size change %u -> %u with partitions refused\n",
			disk->disk_name, q->limits.logical_block_size,
			l.logical_block_size);
		return -EBUSY;
	}
	/* Nothing in flight may have been built for the old limits. */
	if (blk_queue_is_mq(q) && blk_can_sleep()) {
		blk_mq_freeze_queue(q);
		frozen = 1;
	}
	spin_lock_irqsave(&q->lock, &fl);
	if (blk_queue_is_mq(q))
		l.queue_depth = q->tag_set ? q->tag_set->queue_depth :
					     l.queue_depth;
	q->limits = l;
	spin_unlock_irqrestore(&q->lock, fl);
	if (disk) {
		blk_mutex_lock(&g_disk_registry_mutex);
		disk->limits = l;
		if (disk_flags(disk) & GENHD_FL_UP) {
			disk->bdev.sector_size = l.logical_block_size;
			blk_limits_to_bdevs_locked(disk, &l);
		}
		blk_mutex_unlock(&g_disk_registry_mutex);
	}
	if (frozen)
		blk_mq_unfreeze_queue(q);
	return 0;
}
