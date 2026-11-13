// LikeOS - zoned block devices: zone reports, zone management, write
// pointer tracking and zone write plugging, zone append, BLK*ZONE*
//
// A zoned disk (BLK_FEAT_ZONED, NVMe ZNS) is divided into zones of
// limits.chunk_sectors logical blocks of the disk (a power of two; the
// last zone may be smaller).  Zone reports and the BLK*ZONE* ioctls use
// 512-byte sectors (UAPI).  Sequential-write-required zones must be written at their
// write pointer, in order; conventional zones take any write.
//
// ---- Zone write plugging ----------------------------------------------------
// The device sees at most one write per sequential zone at a time: a write
// to a zone that already has one in flight waits on that zone's write plug
// and is submitted, by kblockd, when the one before it completed.  The
// block layer tracks each zone's write pointer itself (wp_offset, from the
// last zone report, advanced at submission): a write that does not start
// at it -- or that would cross the zone's end, or goes to a full zone --
// fails with -EIO before it reaches the device.  A failed write leaves the
// write pointer unknown: the plug is marked NEED_WP_UPDATE, kblockd reads
// the zone back and only then lets the next write go.  Zone reset and
// finish move the tracked pointer at submission (to 0, to the zone's
// capacity); a reset of every zone resets every pointer.
//
// ---- Zone append -------------------------------------------------------------
// A device with native zone append (max_zone_append_sectors) writes the
// data at the zone's write pointer itself and reports where
// (rq->append_sector, logical blocks of the disk); the block layer only
// moves its tracked pointer on success.  Without native support the
// append is emulated: it is plugged like a write, turned into a write at
// the tracked pointer, and its append_sector is that position.
//
// Locking: q->zone_wplugs_lock (spinlock, irqsave, a leaf) covers the
// plug table pointer and every plug in it.  Reports and management
// commands are I/O and are made without any lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* Zones a disk may have before the plug table is refused (a table entry
 * per zone). */
#define BLK_ZONE_MAX_ZONES (1u << 22)
#define BLK_ZONE_WP_INVALID (~0ULL)

/* struct blk_zone_wplug.flags */
#define BLK_ZONE_WPLUG_PLUGGED (1u << 0) /* a write is at the device */
#define BLK_ZONE_WPLUG_NEED_WP_UPDATE (1u << 1) /* re-read the pointer */
#define BLK_ZONE_WPLUG_CONV (1u << 2) /* conventional zone */
#define BLK_ZONE_WPLUG_UNPLUG (1u << 3) /* on the unplug list */

struct blk_zone_wplug {
	uint32_t flags;
	uint32_t zone_no;
	uint64_t wp_offset; /* 512-byte sectors from the zone start */
	uint8_t cond;
	struct blk_request *head, *tail; /* held writes, through rq_next */
	struct blk_zone_wplug *unplug_next;
};

struct blk_zone_plugs {
	struct blk_queue *q;
	struct gendisk *disk;
	uint32_t nr_zones;
	uint32_t shift;
	uint64_t zone_sectors; /* 512-byte sectors */
	uint64_t zone_capacity; /* writable sectors of a sequential zone */
	uint64_t capacity; /* the disk, 512-byte sectors */
	struct blk_zone_wplug *wplugs;
	uint64_t *conv_bitmap;
	struct blk_zone_wplug *unplug_head; /* kblockd's to-do */
	struct blk_work work;
};

/* ==========================================================================
 * Geometry
 * ========================================================================== */

int blk_queue_is_zoned(const struct blk_queue *q)
{
	return q && (q->limits.features & BLK_FEAT_ZONED) &&
	       q->limits.chunk_sectors;
}

static uint32_t blk_disk_per_lba(const struct gendisk *disk);

/* 512-byte sectors per zone (the zone ioctls' unit; the limit itself
 * counts logical blocks). */
uint64_t bdev_zone_sectors(const struct gendisk *disk)
{
	if (!disk || !blk_queue_is_zoned(disk->queue))
		return 0;
	return (uint64_t)disk->queue->limits.chunk_sectors *
	       blk_disk_per_lba(disk);
}

static uint32_t blk_zone_shift(uint64_t zs)
{
	uint32_t s = 0;

	while ((1ULL << s) < zs)
		s++;
	return s;
}

unsigned int disk_zone_no(const struct gendisk *disk, uint64_t sector)
{
	uint64_t zs = bdev_zone_sectors(disk);

	if (!zs)
		return 0;
	return (unsigned int)(sector >> blk_zone_shift(zs));
}

static uint64_t blk_disk_capacity512(const struct gendisk *disk)
{
	uint32_t lbs = disk->queue ? disk->queue->limits.logical_block_size :
				     disk->limits.logical_block_size;

	return disk->capacity * (lbs >= 512 ? lbs / 512 : 1);
}

static uint32_t blk_disk_per_lba(const struct gendisk *disk)
{
	uint32_t lbs = disk->queue ? disk->queue->limits.logical_block_size :
				     disk->limits.logical_block_size;

	return lbs >= 512 ? lbs / 512 : 1;
}

/* The write pointer offset a zone report implies (the established
 * rule: full, conventional, offline and read-only zones have none). */
static uint64_t blk_zone_wp_offset(const struct blk_zone *zone)
{
	switch (zone->cond) {
	case BLK_ZONE_COND_IMP_OPEN:
	case BLK_ZONE_COND_EXP_OPEN:
	case BLK_ZONE_COND_CLOSED:
	case BLK_ZONE_COND_ACTIVE:
		return zone->wp - zone->start;
	case BLK_ZONE_COND_EMPTY:
		return 0;
	default:
		return BLK_ZONE_WP_INVALID;
	}
}

/* ==========================================================================
 * Reports
 * ========================================================================== */

struct blk_report_zones_args {
	struct gendisk *disk;
	report_zones_cb user_cb;
	void *user_data;
	int cached; /* report the tracked write pointers */
};

int disk_report_zone(struct gendisk *disk, struct blk_zone *zone,
		     unsigned int idx, void *cb_data)
{
	struct blk_report_zones_args *args =
		(struct blk_report_zones_args *)cb_data;
	struct blk_queue *q;

	if (!zone || !args)
		return -EINVAL;
	if (!disk)
		disk = args->disk;
	q = disk ? disk->queue : NULL;
	/* A zone the block layer tracks reports its tracked pointer when the
	 * caller asked for the cached view. */
	if (args->cached && q && zone->type == BLK_ZONE_TYPE_SEQWRITE_REQ) {
		struct blk_zone_plugs *p;
		uint64_t fl;

		/* The table and its geometry only under the lock: a
		 * revalidation may swap and free it at any time. */
		spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
		p = (struct blk_zone_plugs *)q->zone_wplugs;
		if (p) {
			uint32_t zno = (uint32_t)(zone->start >> p->shift);

			if (zno < p->nr_zones &&
			    p->wplugs[zno].wp_offset != BLK_ZONE_WP_INVALID &&
			    !(p->wplugs[zno].flags &
			      BLK_ZONE_WPLUG_NEED_WP_UPDATE))
				zone->wp = zone->start +
					   p->wplugs[zno].wp_offset;
		}
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	}
	if (!args->user_cb)
		return 0;
	return args->user_cb(zone, idx, args->user_data);
}

/* What the driver calls for each zone: through disk_report_zone. */
static int blk_zone_report_wrapper(struct blk_zone *zone, unsigned int idx,
				   void *data)
{
	struct blk_report_zones_args *args =
		(struct blk_report_zones_args *)data;

	return disk_report_zone(args->disk, zone, idx, args);
}

static int blk_report_zones_args(struct gendisk *disk, uint64_t sector,
				 unsigned int nr_zones, report_zones_cb cb,
				 void *data, int cached)
{
	struct blk_report_zones_args args;

	if (!disk || !disk->queue || !blk_queue_is_zoned(disk->queue) ||
	    !disk->ops || !disk->ops->report_zones)
		return -EOPNOTSUPP;
	blk_might_sleep();
	if (!nr_zones || sector >= blk_disk_capacity512(disk))
		return 0;
	args.disk = disk;
	args.user_cb = cb;
	args.user_data = data;
	args.cached = cached;
	return disk->ops->report_zones(disk, sector, nr_zones,
				       blk_zone_report_wrapper, &args);
}

int blkdev_report_zones(struct gendisk *disk, uint64_t sector,
			unsigned int nr_zones, report_zones_cb cb, void *data)
{
	return blk_report_zones_args(disk, sector, nr_zones, cb, data, 0);
}

/* One zone, as the device reports it now. */
static int blk_zone_get_one_cb(struct blk_zone *zone, unsigned int idx,
			       void *data)
{
	(void)idx;
	*(struct blk_zone *)data = *zone;
	return 0;
}

static int blk_zone_report_one(struct gendisk *disk, uint64_t sector,
			       struct blk_zone *out)
{
	int n = blkdev_report_zones(disk, sector, 1, blk_zone_get_one_cb, out);

	if (n < 0)
		return n;
	return n == 1 ? 0 : -EIO;
}

/* ==========================================================================
 * Plug helpers (q->zone_wplugs_lock held)
 * ========================================================================== */

static inline int blk_zone_is_conv(const struct blk_zone_plugs *p, uint32_t z)
{
	return p->conv_bitmap &&
	       (p->conv_bitmap[z / 64] & (1ULL << (z % 64))) != 0;
}

static inline uint64_t blk_zone_cap(const struct blk_zone_plugs *p, uint32_t z)
{
	uint64_t start = (uint64_t)z << p->shift;
	uint64_t len = p->zone_sectors;

	if (start + len > p->capacity)
		len = p->capacity - start;
	if (p->zone_capacity && p->zone_capacity < len)
		len = p->zone_capacity;
	return len;
}

static void blk_zone_queue_unplug_locked(struct blk_queue *q,
					 struct blk_zone_plugs *p,
					 struct blk_zone_wplug *w)
{
	lockdep_assert_held(&q->zone_wplugs_lock);
	if (w->flags & BLK_ZONE_WPLUG_UNPLUG)
		return;
	w->flags |= BLK_ZONE_WPLUG_UNPLUG;
	w->unplug_next = p->unplug_head;
	p->unplug_head = w;
}

static void blk_zone_kick(struct blk_zone_plugs *p)
{
	(void)kblockd_schedule_work(&p->work);
}

/* Take every held write of a plug (to fail them). */
static struct blk_request *blk_zone_take_plugged_locked(struct blk_queue *q,
							struct blk_zone_wplug *w)
{
	struct blk_request *l = w->head;

	lockdep_assert_held(&q->zone_wplugs_lock);
	w->head = w->tail = NULL;
	for (struct blk_request *r = l; r; r = r->rq_next)
		r->bl.internal &= ~BLK_RQI_ZONE_PLUGGED;
	return l;
}

static void blk_zone_fail_list(struct blk_request *l, int err)
{
	while (l) {
		struct blk_request *r = l;

		l = r->rq_next;
		r->rq_next = NULL;
		blk_request_complete(r, err, (uint32_t)blk_rq_payload_bytes(r));
	}
}

/* Prepare a write (or emulated append) at the plug's pointer: 0 and the
 * pointer advanced (the plug now has a write at the device), or -EIO.
 * q->zone_wplugs_lock held. */
static int blk_zone_wplug_prepare_locked(struct blk_queue *q,
					 struct blk_zone_plugs *p,
					 struct blk_zone_wplug *w,
					 struct blk_request *rq)
{
	uint32_t per = blk_disk_per_lba(p->disk);
	uint64_t zstart = (uint64_t)w->zone_no << p->shift;
	uint64_t sectors = (uint64_t)rq->count * per;
	uint64_t cap = blk_zone_cap(p, w->zone_no);

	lockdep_assert_held(&q->zone_wplugs_lock);
	if (w->wp_offset == BLK_ZONE_WP_INVALID || w->wp_offset >= cap)
		return -EIO; /* full, offline, read-only */
	if (rq->op == BLK_OP_ZONE_APPEND) {
		/* Emulation: a write at the tracked pointer.  rq->lba is
		 * relative to rq->bdev (the whole disk for a zoned disk). */
		uint64_t lba = (zstart + w->wp_offset) / per;

		rq->lba = lba - (rq->sector - rq->lba);
		rq->sector = lba;
		rq->op = BLK_OP_WRITE;
		rq->append_sector = (zstart + w->wp_offset) / per;
		rq->bl.internal |= BLK_RQI_ZONE_APPEND_EMU;
	} else if (blk_rq_pos(rq) != zstart + w->wp_offset) {
		return -EIO; /* not at the write pointer */
	}
	if (w->wp_offset + sectors > cap)
		return -EIO; /* past the zone's end */
	w->wp_offset += sectors;
	w->flags |= BLK_ZONE_WPLUG_PLUGGED;
	rq->bl.internal |= BLK_RQI_ZONE_WPLUG;
	/* It owns its zone's plug: never merged into a neighbour (another
	 * zone's write) whose completion would not release it. */
	rq->flags |= BLK_REQ_NOMERGE;
	return 0;
}

/* ==========================================================================
 * Submission
 * ========================================================================== */

/* Every sequential zone's pointer to `wp' (reset all), held writes
 * failed. */
static void blk_zone_set_all_wp(struct blk_queue *q, struct blk_zone_plugs *p,
				uint64_t wp)
{
	struct blk_request *fail = NULL, **tail = &fail;
	uint64_t fl;

	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	for (uint32_t z = 0; z < p->nr_zones; z++) {
		struct blk_zone_wplug *w = &p->wplugs[z];
		struct blk_request *l;

		if (blk_zone_is_conv(p, z))
			continue;
		w->wp_offset = wp;
		w->flags &= ~BLK_ZONE_WPLUG_NEED_WP_UPDATE;
		l = blk_zone_take_plugged_locked(q, w);
		*tail = l;
		while (*tail)
			tail = &(*tail)->rq_next;
	}
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	blk_zone_fail_list(fail, -EIO);
}

int blk_zone_plug_request(struct blk_queue *q, struct blk_request *rq)
{
	struct blk_zone_plugs *p;
	struct blk_zone_wplug *w;
	struct blk_request *fail = NULL;
	uint64_t sector, fl;
	uint32_t zno;
	int rc = 0;

	if (!blk_queue_is_zoned(q))
		return 0;
	sector = blk_rq_pos(rq);
	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	p = (struct blk_zone_plugs *)q->zone_wplugs;
	if (!p) {
		/* Not revalidated yet: nothing tracked, nothing plugged. */
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		return 0;
	}
	if (rq->op == BLK_OP_ZONE_RESET_ALL) {
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		blk_zone_set_all_wp(q, p, 0);
		return 0;
	}
	zno = (uint32_t)(sector >> p->shift);
	if (zno >= p->nr_zones) {
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		return -EINVAL;
	}
	w = &p->wplugs[zno];
	switch (rq->op) {
	case BLK_OP_ZONE_RESET:
	case BLK_OP_ZONE_FINISH:
		if (blk_zone_is_conv(p, zno)) {
			rc = -EINVAL;
			break;
		}
		/* The established order: the tracked pointer moves at
		 * submission, held writes for the old pointer fail. */
		w->wp_offset = rq->op == BLK_OP_ZONE_RESET ? 0 :
							     blk_zone_cap(p, zno);
		w->flags &= ~BLK_ZONE_WPLUG_NEED_WP_UPDATE;
		fail = blk_zone_take_plugged_locked(q, w);
		break;
	case BLK_OP_ZONE_APPEND:
		if (blk_zone_is_conv(p, zno)) {
			rc = -EINVAL;
			break;
		}
		if (q->limits.max_zone_append_sectors) {
			/* Native: only the pointer is tracked (completion). */
			rq->bl.internal |= BLK_RQI_ZONE_WPLUG;
			rq->flags |= BLK_REQ_NOMERGE;
			break;
		}
		/* emulated: plugged like a write */
		__attribute__((fallthrough));
	case BLK_OP_WRITE:
	case BLK_OP_WRITE_ZEROES:
		if (rq->op != BLK_OP_ZONE_APPEND && blk_zone_is_conv(p, zno))
			break;
		if ((w->flags & (BLK_ZONE_WPLUG_PLUGGED |
				 BLK_ZONE_WPLUG_NEED_WP_UPDATE)) ||
		    w->head) {
			/* Behind the write at the device: held. */
			if (rq->flags & BLK_REQ_NOWAIT) {
				rc = -EAGAIN;
				break;
			}
			rq->rq_next = NULL;
			if (w->tail)
				w->tail->rq_next = rq;
			else
				w->head = rq;
			w->tail = rq;
			rq->bl.internal |= BLK_RQI_ZONE_PLUGGED;
			rq->state = BLK_RQ_QUEUED;
			rc = 1;
			break;
		}
		rc = blk_zone_wplug_prepare_locked(q, p, w, rq);
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	blk_zone_fail_list(fail, -EIO);
	return rc;
}

/* ==========================================================================
 * Completion and unplugging
 * ========================================================================== */

void blk_zone_write_plug_complete(struct blk_request *rq, int status)
{
	struct blk_queue *q = rq->queue;
	struct blk_zone_plugs *p;
	struct blk_zone_wplug *w;
	uint64_t sector, fl;
	uint32_t zno, per;
	int kick = 0;

	rq->bl.internal &= ~BLK_RQI_ZONE_WPLUG;
	if (!q)
		return;
	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	p = (struct blk_zone_plugs *)q->zone_wplugs;
	if (!p) {
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		return;
	}
	per = blk_disk_per_lba(p->disk);
	sector = (rq->bl.internal & BLK_RQI_ZONE_APPEND_EMU) ?
			 rq->append_sector * per :
			 blk_rq_pos(rq);
	zno = (uint32_t)(sector >> p->shift);
	if (zno >= p->nr_zones) {
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		return;
	}
	w = &p->wplugs[zno];
	if (rq->op == BLK_OP_ZONE_APPEND) {
		/* Native append: the device chose the place. */
		if (!status && w->wp_offset != BLK_ZONE_WP_INVALID)
			w->wp_offset += (uint64_t)rq->count * per;
		else if (status)
			w->flags |= BLK_ZONE_WPLUG_NEED_WP_UPDATE;
		if (w->flags & BLK_ZONE_WPLUG_NEED_WP_UPDATE) {
			blk_zone_queue_unplug_locked(q, p, w);
			kick = 1;
		}
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		if (kick)
			blk_zone_kick(p);
		return;
	}
	w->flags &= ~BLK_ZONE_WPLUG_PLUGGED;
	if (status) {
		/* Where the pointer is now only the device knows. */
		w->flags |= BLK_ZONE_WPLUG_NEED_WP_UPDATE;
	}
	if (rq->bl.internal & BLK_RQI_ZONE_APPEND_EMU) {
		/* The submitter asked for a zone append. */
		rq->op = BLK_OP_ZONE_APPEND;
		rq->bl.internal &= ~BLK_RQI_ZONE_APPEND_EMU;
	}
	if (w->head || (w->flags & BLK_ZONE_WPLUG_NEED_WP_UPDATE)) {
		blk_zone_queue_unplug_locked(q, p, w);
		kick = 1;
	}
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	if (kick)
		blk_zone_kick(p);
}

/* kblockd: zones whose next held write may go, or whose pointer must be
 * read back first. */
static void blk_zone_unplug_work_fn(void *arg)
{
	struct blk_zone_plugs *p = (struct blk_zone_plugs *)arg;
	struct blk_queue *q = p->q;
	uint64_t fl;

	for (;;) {
		struct blk_zone_wplug *w;
		struct blk_request *rq = NULL;
		int rc = 0;

		spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
		w = p->unplug_head;
		if (!w) {
			spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
			return;
		}
		p->unplug_head = w->unplug_next;
		w->unplug_next = NULL;
		w->flags &= ~BLK_ZONE_WPLUG_UNPLUG;
		if (w->flags & BLK_ZONE_WPLUG_NEED_WP_UPDATE) {
			uint64_t zstart = (uint64_t)w->zone_no << p->shift;
			struct blk_zone z;
			int err;

			spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
			err = blk_zone_report_one(p->disk, zstart, &z);
			spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
			if (q->zone_wplugs != p) {
				spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
				return;
			}
			w->flags &= ~BLK_ZONE_WPLUG_NEED_WP_UPDATE;
			w->wp_offset = err ? BLK_ZONE_WP_INVALID :
					     blk_zone_wp_offset(&z);
			if (!err)
				w->cond = z.cond;
		}
		if (!(w->flags & BLK_ZONE_WPLUG_PLUGGED) && w->head) {
			rq = w->head;
			w->head = rq->rq_next;
			if (!w->head)
				w->tail = NULL;
			rq->rq_next = NULL;
			rq->bl.internal &= ~BLK_RQI_ZONE_PLUGGED;
			rc = blk_zone_wplug_prepare_locked(q, p, w, rq);
			/* A failed one does not hold the zone: the next may
			 * go at once. */
			if (rc && w->head)
				blk_zone_queue_unplug_locked(q, p, w);
		}
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		if (!rq)
			continue;
		if (rc)
			blk_request_complete(rq, rc,
					     (uint32_t)blk_rq_payload_bytes(rq));
		else
			(void)blk_submit_resume(rq);
	}
}

int blk_zone_cancel_plugged(struct blk_request *rq)
{
	struct blk_queue *q = rq->queue;
	struct blk_zone_plugs *p;
	uint64_t fl;
	int found = 0;

	if (!q)
		return 0;
	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	p = (struct blk_zone_plugs *)q->zone_wplugs;
	if (p && (rq->bl.internal & BLK_RQI_ZONE_PLUGGED)) {
		uint32_t zno = (uint32_t)(blk_rq_pos(rq) >> p->shift);

		if (zno < p->nr_zones) {
			struct blk_zone_wplug *w = &p->wplugs[zno];
			struct blk_request *prev = NULL;

			for (struct blk_request *r = w->head; r;
			     prev = r, r = r->rq_next) {
				if (r != rq)
					continue;
				if (prev)
					prev->rq_next = r->rq_next;
				else
					w->head = r->rq_next;
				if (w->tail == r)
					w->tail = prev;
				r->rq_next = NULL;
				r->bl.internal &= ~BLK_RQI_ZONE_PLUGGED;
				found = 1;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	return found;
}

/* Teardown: every held write onto *tailp (through disp_next). */
struct blk_request **blk_zone_collect_plugged(struct blk_queue *q,
					      struct blk_request **tailp)
{
	struct blk_zone_plugs *p;
	uint64_t fl;

	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	p = (struct blk_zone_plugs *)q->zone_wplugs;
	if (p) {
		for (uint32_t z = 0; z < p->nr_zones; z++) {
			struct blk_request *l =
				blk_zone_take_plugged_locked(q, &p->wplugs[z]);

			while (l) {
				struct blk_request *n = l->rq_next;

				l->rq_next = NULL;
				*tailp = l;
				tailp = &l->disp_next;
				l->disp_next = NULL;
				l = n;
			}
		}
	}
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	return tailp;
}

static void blk_zone_plugs_free(struct blk_zone_plugs *p)
{
	if (!p)
		return;
	if (p->wplugs)
		kfree(p->wplugs);
	if (p->conv_bitmap)
		kfree(p->conv_bitmap);
	kfree(p);
}

void blk_zone_free_plugs(struct blk_queue *q)
{
	struct blk_zone_plugs *p;
	struct blk_request *fail = NULL, **tail = &fail;
	uint64_t fl;

	if (!q)
		return;
	tail = blk_zone_collect_plugged(q, tail);
	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	p = (struct blk_zone_plugs *)q->zone_wplugs;
	q->zone_wplugs = NULL;
	q->conv_zones_bitmap = NULL;
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	while (fail) {
		struct blk_request *r = fail;

		fail = r->disp_next;
		r->disp_next = NULL;
		blk_request_complete(r, -ENODEV,
				     (uint32_t)blk_rq_payload_bytes(r));
	}
	if (!p)
		return;
	if (blk_work_cancel_sync(&p->work, 1000)) {
		WARN(1, BLK_LOG_PREFIX "zone unplug work does not finish; leaking its plugs");
		return;
	}
	blk_zone_plugs_free(p);
}

/* ==========================================================================
 * Revalidation
 * ========================================================================== */

struct blk_revalidate_args {
	struct blk_zone_plugs *p;
	uint64_t sector; /* where the next zone must start */
	uint32_t count;
	int bad;
};

static int blk_revalidate_zone_cb(struct blk_zone *zone, unsigned int idx,
				  void *data)
{
	struct blk_revalidate_args *a = (struct blk_revalidate_args *)data;
	struct blk_zone_plugs *p = a->p;
	const char *name = p->disk->disk_name;
	uint32_t zno;

	(void)idx;
	if (zone->start != a->sector) {
		kprintf(BLK_LOG_PREFIX "%s: zone gap at sector %llu (zone at %llu)\n",
			name, (unsigned long long)a->sector,
			(unsigned long long)zone->start);
		a->bad = 1;
		return -ENODEV;
	}
	/* Every zone has the same size but the last, which may be smaller. */
	if (zone->len == 0 || zone->len > p->zone_sectors ||
	    (zone->len != p->zone_sectors &&
	     zone->start + zone->len < p->capacity)) {
		kprintf(BLK_LOG_PREFIX "%s: invalid zone size %llu at sector %llu\n",
			name, (unsigned long long)zone->len,
			(unsigned long long)zone->start);
		a->bad = 1;
		return -ENODEV;
	}
	zno = (uint32_t)(zone->start >> p->shift);
	if (zno >= p->nr_zones) {
		a->bad = 1;
		return -ENODEV;
	}
	switch (zone->type) {
	case BLK_ZONE_TYPE_CONVENTIONAL:
		p->conv_bitmap[zno / 64] |= 1ULL << (zno % 64);
		p->wplugs[zno].flags |= BLK_ZONE_WPLUG_CONV;
		p->wplugs[zno].wp_offset = BLK_ZONE_WP_INVALID;
		break;
	case BLK_ZONE_TYPE_SEQWRITE_REQ: {
		uint64_t cap = zone->capacity ? zone->capacity : zone->len;

		if (!p->zone_capacity)
			p->zone_capacity = cap;
		else if (cap != p->zone_capacity &&
			 zone->start + zone->len < p->capacity) {
			kprintf(BLK_LOG_PREFIX "%s: zone capacity %llu differs from %llu\n",
				name, (unsigned long long)cap,
				(unsigned long long)p->zone_capacity);
			a->bad = 1;
			return -ENODEV;
		}
		p->wplugs[zno].wp_offset = blk_zone_wp_offset(zone);
		break;
	}
	default:
		kprintf(BLK_LOG_PREFIX "%s: invalid zone type %u at sector %llu\n",
			name, zone->type, (unsigned long long)zone->start);
		a->bad = 1;
		return -ENODEV;
	}
	p->wplugs[zno].cond = zone->cond;
	a->sector = zone->start + zone->len;
	a->count++;
	return 0;
}

int blk_revalidate_disk_zones(struct gendisk *disk)
{
	struct blk_queue *q;
	struct blk_zone_plugs *p, *old;
	struct blk_revalidate_args a;
	uint64_t zs, fl;
	int ret;

	if (!disk || !disk->queue)
		return -EINVAL;
	q = disk->queue;
	might_sleep();
	if (!(q->limits.features & BLK_FEAT_ZONED))
		return -EINVAL;
	if (!disk->ops || !disk->ops->report_zones)
		return -EINVAL;
	zs = (uint64_t)q->limits.chunk_sectors * blk_disk_per_lba(disk);
	if (!zs || (zs & (zs - 1))) {
		kprintf(BLK_LOG_PREFIX "%s: invalid zone size %llu (not a power of two)\n",
			disk->disk_name, (unsigned long long)zs);
		return -ENODEV;
	}
	if (!disk->capacity)
		return -ENODEV;
	p = (struct blk_zone_plugs *)kcalloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;
	p->q = q;
	p->disk = disk;
	p->zone_sectors = zs;
	p->shift = blk_zone_shift(zs);
	p->capacity = blk_disk_capacity512(disk);
	if (((p->capacity + zs - 1) >> p->shift) > BLK_ZONE_MAX_ZONES) {
		kprintf(BLK_LOG_PREFIX "%s: too many zones\n", disk->disk_name);
		kfree(p);
		return -ENODEV;
	}
	p->nr_zones = (uint32_t)((p->capacity + zs - 1) >> p->shift);
	p->wplugs = (struct blk_zone_wplug *)kcalloc(p->nr_zones,
						     sizeof(struct blk_zone_wplug));
	p->conv_bitmap = (uint64_t *)kcalloc((p->nr_zones + 63) / 64,
					     sizeof(uint64_t));
	if (!p->wplugs || !p->conv_bitmap) {
		blk_zone_plugs_free(p);
		return -ENOMEM;
	}
	for (uint32_t z = 0; z < p->nr_zones; z++)
		p->wplugs[z].zone_no = z;
	blk_work_init(&p->work, blk_zone_unplug_work_fn, p);

	/* Read every zone and check the layout (no lock: this is I/O). */
	mm_memset(&a, 0, sizeof(a));
	a.p = p;
	ret = blkdev_report_zones(disk, 0, p->nr_zones, blk_revalidate_zone_cb,
				  &a);
	if (ret >= 0 && !a.bad && a.sector != p->capacity) {
		kprintf(BLK_LOG_PREFIX "%s: zones end at %llu, capacity %llu\n",
			disk->disk_name, (unsigned long long)a.sector,
			(unsigned long long)p->capacity);
		ret = -ENODEV;
	}
	if (ret < 0 || a.bad) {
		kprintf(BLK_LOG_PREFIX "%s: failed to revalidate zones (%d)\n",
			disk->disk_name, ret < 0 ? ret : -ENODEV);
		blk_zone_plugs_free(p);
		return ret < 0 ? ret : -ENODEV;
	}

	/* In use: nothing in flight while the table changes.  Every write
	 * prepared against the old table -- in flight, or held by its zone
	 * -- holds the usage count (it is taken before the plug), so the
	 * freeze waits for all of them and none completes against the new
	 * table. */
	blk_mq_freeze_queue(q);
	spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
	old = (struct blk_zone_plugs *)q->zone_wplugs;
	q->zone_wplugs = p;
	q->conv_zones_bitmap = p->conv_bitmap;
	q->nr_zones = p->nr_zones;
	q->zone_sectors_shift = p->shift;
	q->zone_wplugs_nr = p->nr_zones;
	q->zone_capacity_sectors = p->zone_capacity;
	q->zone_wplugs_work = &p->work;
	disk->nr_zones = p->nr_zones;
	spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
	/* A zoned disk is not partitioned (the established rule). */
	disk_set_flags(disk, GENHD_FL_ZONED_NO_PART);
	blk_mq_unfreeze_queue(q);
	if (old) {
		struct blk_request *fail = NULL, **tail = &fail;

		spin_lock_irqsave(&q->zone_wplugs_lock, &fl);
		for (uint32_t z = 0; z < old->nr_zones; z++) {
			struct blk_request *l =
				blk_zone_take_plugged_locked(q, &old->wplugs[z]);

			*tail = l;
			while (*tail)
				tail = &(*tail)->rq_next;
		}
		spin_unlock_irqrestore(&q->zone_wplugs_lock, fl);
		blk_zone_fail_list(fail, -EIO);
		if (!blk_work_cancel_sync(&old->work, 1000))
			blk_zone_plugs_free(old);
	}
	return 0;
}

/* ==========================================================================
 * Zone management
 * ========================================================================== */

int blkdev_zone_mgmt(struct gendisk *disk, int op, uint64_t sector,
		     uint64_t nr_sectors)
{
	struct blk_queue *q;
	uint64_t zs, cap, end;
	uint32_t per;
	int ret = 0;

	if (!disk || !disk->queue)
		return -EINVAL;
	q = disk->queue;
	might_sleep();
	if (!blk_queue_is_zoned(q))
		return -EOPNOTSUPP;
	if (!blk_op_is_zone_mgmt((enum blk_op)op))
		return -EOPNOTSUPP;
	if (blk_bdev_read_only(&disk->bdev))
		return -EPERM;
	zs = bdev_zone_sectors(disk);
	cap = blk_disk_capacity512(disk);
	per = blk_disk_per_lba(disk);
	end = sector + nr_sectors;
	if (!nr_sectors || end > cap || end < sector)
		return -EINVAL;
	/* Whole zones only (the last zone may be smaller). */
	if (sector & (zs - 1))
		return -EINVAL;
	if ((nr_sectors & (zs - 1)) && end != cap)
		return -EINVAL;
	/* Resetting every zone is one command. */
	if (op == BLK_OP_ZONE_RESET && sector == 0 && nr_sectors == cap)
		op = BLK_OP_ZONE_RESET_ALL;
	while (sector < end) {
		struct blk_request req;

		blk_request_init(&req, &disk->bdev, (enum blk_op)op,
				 op == BLK_OP_ZONE_RESET_ALL ? 0 : sector / per,
				 0);
		req.flags |= BLK_REQ_SYNC;
		ret = blk_execute(&req);
		blk_request_release(&req);
		if (ret || op == BLK_OP_ZONE_RESET_ALL)
			break;
		sector += zs;
		blk_flush_own_done();
	}
	return ret;
}

/* ==========================================================================
 * ioctls
 * ========================================================================== */

struct blk_zone_user_args {
	struct blk_zone *uzones;
	uint32_t room;
	uint32_t filled;
};

static int blk_zone_copy_cb(struct blk_zone *zone, unsigned int idx, void *data)
{
	struct blk_zone_user_args *a = (struct blk_zone_user_args *)data;

	(void)idx;
	if (a->filled >= a->room)
		return -ENOSPC;
	if (copy_to_user(&a->uzones[a->filled], zone, sizeof(*zone)))
		return -EFAULT;
	a->filled++;
	return 0;
}

static int blk_zone_writable(int open_flags)
{
	int acc = open_flags & 3;

	return acc == 1 /* O_WRONLY */ || acc == 2 /* O_RDWR */;
}

long blkdev_zone_ioctl(struct gendisk *disk, unsigned long cmd, void *argp,
		       int open_flags)
{
	struct blk_queue *q;

	switch (cmd) {
	case BLKREPORTZONE:
	case BLKREPORTZONEV2:
	case BLKRESETZONE:
	case BLKOPENZONE:
	case BLKCLOSEZONE:
	case BLKFINISHZONE:
	case BLKGETZONESZ:
	case BLKGETNRZONES:
		break;
	default:
		return -ENOTTY;
	}
	if (!disk || !disk->queue)
		return -ENODEV;
	q = disk->queue;
	if (!argp)
		return -EINVAL;
	switch (cmd) {
	case BLKGETZONESZ: {
		uint32_t v = (uint32_t)bdev_zone_sectors(disk);

		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	}
	case BLKGETNRZONES: {
		uint32_t v = blk_queue_is_zoned(q) ? disk->nr_zones : 0;

		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	}
	case BLKREPORTZONE:
	case BLKREPORTZONEV2: {
		struct blk_zone_report rep;
		struct blk_zone_user_args a;
		int n;

		if (!blk_queue_is_zoned(q))
			return -ENOTTY;
		if (copy_from_user(&rep, argp, sizeof(rep)))
			return -EFAULT;
		if (!rep.nr_zones)
			return -EINVAL;
		a.uzones = (struct blk_zone *)((uint8_t *)argp + sizeof(rep));
		a.room = rep.nr_zones;
		a.filled = 0;
		n = blk_report_zones_args(disk, rep.sector, rep.nr_zones,
					  blk_zone_copy_cb, &a,
					  cmd == BLKREPORTZONEV2 &&
						  (rep.flags &
						   BLK_ZONE_REP_CACHED));
		if (n < 0)
			return n;
		rep.nr_zones = a.filled;
		rep.flags = BLK_ZONE_REP_CAPACITY |
			    (cmd == BLKREPORTZONEV2 ?
				     (rep.flags & BLK_ZONE_REP_CACHED) :
				     0);
		return copy_to_user(argp, &rep, sizeof(rep)) ? -EFAULT : 0;
	}
	default: {
		struct blk_zone_range zrange;
		int op;

		if (!blk_queue_is_zoned(q))
			return -ENOTTY;
		if (!blk_zone_writable(open_flags))
			return -EBADF;
		if (copy_from_user(&zrange, argp, sizeof(zrange)))
			return -EFAULT;
		switch (cmd) {
		case BLKRESETZONE:
			op = BLK_OP_ZONE_RESET;
			break;
		case BLKOPENZONE:
			op = BLK_OP_ZONE_OPEN;
			break;
		case BLKCLOSEZONE:
			op = BLK_OP_ZONE_CLOSE;
			break;
		default:
			op = BLK_OP_ZONE_FINISH;
			break;
		}
		return blkdev_zone_mgmt(disk, op, zrange.sector,
					zrange.nr_sectors);
	}
	}
}
