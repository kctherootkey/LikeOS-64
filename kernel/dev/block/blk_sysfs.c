// LikeOS - the block layer in /sys and /proc
//
//   /sys/block/<disk>/            one directory per disk (sda, sr0)
//       dev size ro removable range ext_range capability stat inflight
//       alignment_offset discard_alignment diskseq uevent hidden events
//       events_async events_poll_msecs
//       queue/   logical_block_size physical_block_size hw_sector_size
//                minimum_io_size optimal_io_size max_sectors_kb (rw)
//                max_hw_sectors_kb max_segments max_segment_size
//                rotational (rw) scheduler (rw) nr_requests (rw)
//                discard_granularity discard_max_bytes discard_max_hw_bytes
//                discard_zeroes_data write_zeroes_max_bytes write_cache (rw)
//                fua nomerges (rw) rq_affinity (rw) read_ahead_kb (rw) zoned dax iostats
//                add_random io_poll chunk_sectors dma_alignment
//       device/  model vendor rev serial wwid state queue_depth timeout
//                type device_blocked iorequest_cnt iodone_cnt ioerr_cnt
//       power/   state (rw: "suspend"/"off", "resume"/"on")
//       holders/ slaves/ (empty: no stacked devices exist)
//       subsystem -> ../../class/block
//       <part>/  dev partition start size ro stat inflight alignment_offset
//                discard_alignment uevent holders/ subsystem
//   /sys/class/block/<dev>  -> ../../block/<disk>[/<part>]
//   /sys/dev/block/M:m      -> ../../block/<disk>[/<part>]
//   /sys/power/disk_state   "suspend" / "resume": every disk at once
//   /proc/partitions, /proc/diskstats
//
// None of this is a static tree: the directories are dynamic pseudofs
// directories whose listings and lookups come from the disk registry at the
// moment they are asked, so a disk plugged in or removed appears and
// disappears without anything to add or tear down here.  A node made by a
// lookup remembers its disk by pointer and checks it against the live
// registry (under the registry mutex) every time it is read, so a node of
// a disk removed in between reads as "no such device".
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/fs/sysfs.h>
#include <kernel/uapi/dirent.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ==========================================================================
 * Attribute tables
 * ========================================================================== */

/* Directory kinds (dir node arg2 bits 24..31) */
enum {
	DK_BLOCK_TOP = 1,
	DK_DISK,
	DK_QUEUE,
	DK_DEVICE,
	DK_POWER,
	DK_PART,
	DK_EMPTY,
};

/* Attribute ids (file node arg2 bits 0..15) */
enum {
	A_DEV = 1,
	A_SIZE,
	A_RO,
	A_REMOVABLE,
	A_RANGE,
	A_EXT_RANGE,
	A_CAPABILITY,
	A_STAT,
	A_INFLIGHT,
	A_ALIGN,
	A_DISCARD_ALIGN,
	A_DISKSEQ,
	A_UEVENT,
	A_HIDDEN,
	A_EVENTS,
	A_EVENTS_ASYNC,
	A_EVENTS_POLL,
	A_PARTITION,
	A_START,
	Q_LBS,
	Q_PBS,
	Q_HW_SECTOR,
	Q_MIN_IO,
	Q_OPT_IO,
	Q_MAX_SECTORS_KB,
	Q_MAX_HW_SECTORS_KB,
	Q_MAX_SEGMENTS,
	Q_MAX_SEGMENT_SIZE,
	Q_ROTATIONAL,
	Q_SCHEDULER,
	Q_NR_REQUESTS,
	Q_DISCARD_GRAN,
	Q_DISCARD_MAX,
	Q_DISCARD_MAX_HW,
	Q_DISCARD_ZEROES,
	Q_WRITE_ZEROES_MAX,
	Q_WRITE_CACHE,
	Q_FUA,
	Q_NOMERGES,
	Q_RQ_AFFINITY,
	Q_READ_AHEAD,
	Q_ZONED,
	Q_DAX,
	Q_IOSTATS,
	Q_ADD_RANDOM,
	Q_IO_POLL,
	Q_CHUNK,
	Q_DMA_ALIGN,
	D_MODEL,
	D_VENDOR,
	D_REV,
	D_SERIAL,
	D_WWID,
	D_STATE,
	D_QUEUE_DEPTH,
	D_TIMEOUT,
	D_TYPE,
	D_BLOCKED,
	D_IOREQ,
	D_IODONE,
	D_IOERR,
	P_STATE,
};

enum { T_FILE = 1, T_DIR, T_LINK };

struct blk_attr {
	const char *name;
	uint16_t id; /* attribute id or directory kind */
	uint8_t type; /* T_* */
	uint8_t writable;
	const char *link; /* T_LINK target */
};

static const struct blk_attr g_disk_attrs[] = {
	{ "dev", A_DEV, T_FILE, 0, NULL },
	{ "size", A_SIZE, T_FILE, 0, NULL },
	{ "ro", A_RO, T_FILE, 0, NULL },
	{ "removable", A_REMOVABLE, T_FILE, 0, NULL },
	{ "range", A_RANGE, T_FILE, 0, NULL },
	{ "ext_range", A_EXT_RANGE, T_FILE, 0, NULL },
	{ "capability", A_CAPABILITY, T_FILE, 0, NULL },
	{ "stat", A_STAT, T_FILE, 0, NULL },
	{ "inflight", A_INFLIGHT, T_FILE, 0, NULL },
	{ "alignment_offset", A_ALIGN, T_FILE, 0, NULL },
	{ "discard_alignment", A_DISCARD_ALIGN, T_FILE, 0, NULL },
	{ "diskseq", A_DISKSEQ, T_FILE, 0, NULL },
	{ "uevent", A_UEVENT, T_FILE, 0, NULL },
	{ "hidden", A_HIDDEN, T_FILE, 0, NULL },
	{ "events", A_EVENTS, T_FILE, 0, NULL },
	{ "events_async", A_EVENTS_ASYNC, T_FILE, 0, NULL },
	{ "events_poll_msecs", A_EVENTS_POLL, T_FILE, 0, NULL },
	{ "queue", DK_QUEUE, T_DIR, 0, NULL },
	{ "device", DK_DEVICE, T_DIR, 0, NULL },
	{ "power", DK_POWER, T_DIR, 0, NULL },
	{ "holders", DK_EMPTY, T_DIR, 0, NULL },
	{ "slaves", DK_EMPTY, T_DIR, 0, NULL },
	{ "subsystem", 0, T_LINK, 0, "../../class/block" },
};

static const struct blk_attr g_queue_attrs[] = {
	{ "logical_block_size", Q_LBS, T_FILE, 0, NULL },
	{ "physical_block_size", Q_PBS, T_FILE, 0, NULL },
	{ "hw_sector_size", Q_HW_SECTOR, T_FILE, 0, NULL },
	{ "minimum_io_size", Q_MIN_IO, T_FILE, 0, NULL },
	{ "optimal_io_size", Q_OPT_IO, T_FILE, 0, NULL },
	{ "max_sectors_kb", Q_MAX_SECTORS_KB, T_FILE, 1, NULL },
	{ "max_hw_sectors_kb", Q_MAX_HW_SECTORS_KB, T_FILE, 0, NULL },
	{ "max_segments", Q_MAX_SEGMENTS, T_FILE, 0, NULL },
	{ "max_segment_size", Q_MAX_SEGMENT_SIZE, T_FILE, 0, NULL },
	{ "rotational", Q_ROTATIONAL, T_FILE, 1, NULL },
	{ "scheduler", Q_SCHEDULER, T_FILE, 1, NULL },
	{ "nr_requests", Q_NR_REQUESTS, T_FILE, 1, NULL },
	{ "discard_granularity", Q_DISCARD_GRAN, T_FILE, 0, NULL },
	{ "discard_max_bytes", Q_DISCARD_MAX, T_FILE, 0, NULL },
	{ "discard_max_hw_bytes", Q_DISCARD_MAX_HW, T_FILE, 0, NULL },
	{ "discard_zeroes_data", Q_DISCARD_ZEROES, T_FILE, 0, NULL },
	{ "write_zeroes_max_bytes", Q_WRITE_ZEROES_MAX, T_FILE, 0, NULL },
	{ "write_cache", Q_WRITE_CACHE, T_FILE, 1, NULL },
	{ "fua", Q_FUA, T_FILE, 0, NULL },
	{ "nomerges", Q_NOMERGES, T_FILE, 1, NULL },
	{ "rq_affinity", Q_RQ_AFFINITY, T_FILE, 1, NULL },
	{ "read_ahead_kb", Q_READ_AHEAD, T_FILE, 1, NULL },
	{ "zoned", Q_ZONED, T_FILE, 0, NULL },
	{ "dax", Q_DAX, T_FILE, 0, NULL },
	{ "iostats", Q_IOSTATS, T_FILE, 0, NULL },
	{ "add_random", Q_ADD_RANDOM, T_FILE, 0, NULL },
	{ "io_poll", Q_IO_POLL, T_FILE, 0, NULL },
	{ "chunk_sectors", Q_CHUNK, T_FILE, 0, NULL },
	{ "dma_alignment", Q_DMA_ALIGN, T_FILE, 0, NULL },
};

static const struct blk_attr g_device_attrs[] = {
	{ "model", D_MODEL, T_FILE, 0, NULL },
	{ "vendor", D_VENDOR, T_FILE, 0, NULL },
	{ "rev", D_REV, T_FILE, 0, NULL },
	{ "serial", D_SERIAL, T_FILE, 0, NULL },
	{ "wwid", D_WWID, T_FILE, 0, NULL },
	{ "state", D_STATE, T_FILE, 0, NULL },
	{ "queue_depth", D_QUEUE_DEPTH, T_FILE, 0, NULL },
	{ "timeout", D_TIMEOUT, T_FILE, 0, NULL },
	{ "type", D_TYPE, T_FILE, 0, NULL },
	{ "device_blocked", D_BLOCKED, T_FILE, 0, NULL },
	{ "iorequest_cnt", D_IOREQ, T_FILE, 0, NULL },
	{ "iodone_cnt", D_IODONE, T_FILE, 0, NULL },
	{ "ioerr_cnt", D_IOERR, T_FILE, 0, NULL },
};

static const struct blk_attr g_power_attrs[] = {
	{ "state", P_STATE, T_FILE, 1, NULL },
};

static const struct blk_attr g_part_attrs[] = {
	{ "dev", A_DEV, T_FILE, 0, NULL },
	{ "partition", A_PARTITION, T_FILE, 0, NULL },
	{ "start", A_START, T_FILE, 0, NULL },
	{ "size", A_SIZE, T_FILE, 0, NULL },
	{ "ro", A_RO, T_FILE, 0, NULL },
	{ "stat", A_STAT, T_FILE, 0, NULL },
	{ "inflight", A_INFLIGHT, T_FILE, 0, NULL },
	{ "alignment_offset", A_ALIGN, T_FILE, 0, NULL },
	{ "discard_alignment", A_DISCARD_ALIGN, T_FILE, 0, NULL },
	{ "uevent", A_UEVENT, T_FILE, 0, NULL },
	{ "holders", DK_EMPTY, T_DIR, 0, NULL },
	{ "subsystem", 0, T_LINK, 0, "../../../class/block" },
};

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct blk_attr *blk_table(int kind, int *n)
{
	switch (kind) {
	case DK_DISK:
		*n = NELEM(g_disk_attrs);
		return g_disk_attrs;
	case DK_QUEUE:
		*n = NELEM(g_queue_attrs);
		return g_queue_attrs;
	case DK_DEVICE:
		*n = NELEM(g_device_attrs);
		return g_device_attrs;
	case DK_POWER:
		*n = NELEM(g_power_attrs);
		return g_power_attrs;
	case DK_PART:
		*n = NELEM(g_part_attrs);
		return g_part_attrs;
	default:
		*n = 0;
		return NULL;
	}
}

static inline int blk_node_kind(const struct pfs_node *n)
{
	return (int)((n->arg2 >> 24) & 0xFF);
}

static inline int blk_node_partno(const struct pfs_node *n)
{
	return (int)((n->arg2 >> 16) & 0xFF);
}

static inline int blk_node_attr(const struct pfs_node *n)
{
	return (int)(n->arg2 & 0xFFFF);
}

/* The device a node describes, if its disk is still live.  Registry
 * mutex held. */
static block_device_t *blk_node_bdev_locked(const struct pfs_node *n)
{
	struct gendisk *d = (struct gendisk *)n->arg;
	int partno = blk_node_partno(n);

	if (!d || !genhd_disk_alive_locked(d))
		return NULL;
	if (!partno)
		return &d->bdev;
	if (partno > DISK_MAX_PARTS || !d->part[partno])
		return NULL;
	return &d->part[partno]->bdev;
}

/* ==========================================================================
 * Shows
 * ========================================================================== */

static long blk_print_stat(char *buf, long cap, const struct blk_disk_stats *s)
{
	return pfs_printf(
		buf, cap, 0,
		"%8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu\n",
		(unsigned long long)s->ios[BLK_STAT_READ],
		(unsigned long long)s->merges[BLK_STAT_READ],
		(unsigned long long)s->sectors[BLK_STAT_READ],
		(unsigned long long)s->ticks_ms[BLK_STAT_READ],
		(unsigned long long)s->ios[BLK_STAT_WRITE],
		(unsigned long long)s->merges[BLK_STAT_WRITE],
		(unsigned long long)s->sectors[BLK_STAT_WRITE],
		(unsigned long long)s->ticks_ms[BLK_STAT_WRITE],
		(unsigned long long)(s->in_flight[0] + s->in_flight[1]),
		(unsigned long long)s->io_ticks_ms,
		(unsigned long long)s->time_in_queue_ms,
		(unsigned long long)s->ios[BLK_STAT_DISCARD],
		(unsigned long long)s->merges[BLK_STAT_DISCARD],
		(unsigned long long)s->sectors[BLK_STAT_DISCARD],
		(unsigned long long)s->ticks_ms[BLK_STAT_DISCARD],
		(unsigned long long)s->ios[BLK_STAT_FLUSH],
		(unsigned long long)s->ticks_ms[BLK_STAT_FLUSH]);
}

long blk_sysfs_uevent_locked(block_device_t *bdev, char *buf, long cap)
{
	long p = 0;

	p = pfs_printf(buf, cap, p, "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\nDEVTYPE=%s\n",
		       bdev->major, bdev->minor, bdev->name,
		       bdev->part ? "partition" : "disk");
	if (bdev->part) {
		p = pfs_printf(buf, cap, p, "PARTN=%d\n", bdev->part->partno);
		if (bdev->part->name[0])
			p = pfs_printf(buf, cap, p, "PARTNAME=%s\n",
				       bdev->part->name);
	}
	p = pfs_printf(buf, cap, p, "DISKSEQ=%llu\n",
		       (unsigned long long)bdev->disk->diskseq);
	return p;
}

/* A string attribute with the trailing blanks of an IDENTIFY field gone. */
static long blk_print_trim(char *buf, long cap, const char *s)
{
	long n = (long)kstrlen(s);

	while (n > 0 && s[n - 1] == ' ')
		n--;
	{
		long p = 0;
		for (long i = 0; i < n; i++, p++)
			if (p < cap)
				buf[p] = s[i];
		if (p < cap)
			buf[p] = '\n';
		return p + 1;
	}
}

static long blk_attr_show(struct pfs_node *n, char *buf, long cap)
{
	block_device_t *b;
	struct gendisk *d;
	struct blk_queue *q;
	const struct blk_limits *l;
	long r = 0;

	blk_mutex_lock(&g_disk_registry_mutex);
	b = blk_node_bdev_locked(n);
	if (!b) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return -ENODEV;
	}
	d = b->disk;
	q = d->queue;
	l = &q->limits;
	switch (blk_node_attr(n)) {
	case A_DEV:
		r = pfs_printf(buf, cap, 0, "%u:%u\n", b->major, b->minor);
		break;
	case A_SIZE:
		r = pfs_printf(buf, cap, 0, "%llu\n",
			       (unsigned long long)((uint64_t)b->total_sectors *
						    b->sector_size / 512));
		break;
	case A_RO:
		r = pfs_printf(buf, cap, 0, "%d\n", blk_bdev_read_only(b));
		break;
	case A_REMOVABLE:
		r = pfs_printf(buf, cap, 0, "%d\n",
			       (d->flags & GENHD_FL_REMOVABLE) ? 1 : 0);
		break;
	case A_RANGE:
		r = pfs_printf(buf, cap, 0, "%u\n", d->minors);
		break;
	case A_EXT_RANGE:
		r = pfs_printf(buf, cap, 0, "%u\n",
			       d->kind == BLK_DISK_SD ? 256u : 1u);
		break;
	case A_CAPABILITY:
		r = pfs_printf(buf, cap, 0, "%x\n",
			       ((d->flags & GENHD_FL_REMOVABLE) ? 0x1 : 0) |
				       ((d->flags & GENHD_FL_NO_PART_SCAN) ?
						0x200 :
						0) |
				       ((d->flags & GENHD_FL_HIDDEN) ? 0x400 : 0));
		break;
	case A_STAT: {
		struct blk_disk_stats s;
		blk_queue_stats_snapshot(q, b->part ? &b->part->stats : &d->stats,
					 &s);
		r = blk_print_stat(buf, cap, &s);
		break;
	}
	case A_INFLIGHT: {
		uint64_t f[2];
		blk_queue_inflight(q, b->part ? &b->part->stats : &d->stats, f);
		r = pfs_printf(buf, cap, 0, "%8llu %8llu\n",
			       (unsigned long long)f[0],
			       (unsigned long long)f[1]);
		break;
	}
	case A_ALIGN:
		r = pfs_printf(buf, cap, 0, "%u\n", b->alignment_offset);
		break;
	case A_DISCARD_ALIGN:
		r = pfs_printf(buf, cap, 0, "%u\n", l->discard_alignment);
		break;
	case A_DISKSEQ:
		r = pfs_printf(buf, cap, 0, "%llu\n",
			       (unsigned long long)d->diskseq);
		break;
	case A_UEVENT:
		r = blk_sysfs_uevent_locked(b, buf, cap);
		break;
	case A_HIDDEN:
		r = pfs_printf(buf, cap, 0, "%d\n",
			       (d->flags & GENHD_FL_HIDDEN) ? 1 : 0);
		break;
	case A_EVENTS:
		r = pfs_printf(buf, cap, 0, "%s\n",
			       (d->flags & GENHD_FL_REMOVABLE) ? "media_change" :
								 "");
		break;
	case A_EVENTS_ASYNC:
		r = pfs_printf(buf, cap, 0, "\n");
		break;
	case A_EVENTS_POLL:
		r = pfs_printf(buf, cap, 0, "%d\n",
			       (d->flags & GENHD_FL_REMOVABLE) ? 2000 : -1);
		break;
	case A_PARTITION:
		r = pfs_printf(buf, cap, 0, "%d\n", b->part ? b->part->partno : 0);
		break;
	case A_START:
		r = pfs_printf(buf, cap, 0, "%llu\n",
			       (unsigned long long)(b->start_lba * b->sector_size /
						    512));
		break;
	case Q_LBS:
	case Q_HW_SECTOR:
		r = pfs_printf(buf, cap, 0, "%u\n", l->logical_block_size);
		break;
	case Q_PBS:
		r = pfs_printf(buf, cap, 0, "%u\n", l->physical_block_size);
		break;
	case Q_MIN_IO:
		r = pfs_printf(buf, cap, 0, "%u\n", l->io_min);
		break;
	case Q_OPT_IO:
		r = pfs_printf(buf, cap, 0, "%u\n", l->io_opt);
		break;
	case Q_MAX_SECTORS_KB:
		r = pfs_printf(buf, cap, 0, "%u\n",
			       (uint32_t)((uint64_t)l->max_sectors *
					  l->logical_block_size / 1024));
		break;
	case Q_MAX_HW_SECTORS_KB:
		r = pfs_printf(buf, cap, 0, "%u\n",
			       (uint32_t)((uint64_t)l->max_hw_sectors *
					  l->logical_block_size / 1024));
		break;
	case Q_MAX_SEGMENTS:
		r = pfs_printf(buf, cap, 0, "%u\n", l->max_segments);
		break;
	case Q_MAX_SEGMENT_SIZE:
		r = pfs_printf(buf, cap, 0, "%u\n", l->max_segment_size);
		break;
	case Q_ROTATIONAL:
		r = pfs_printf(buf, cap, 0, "%u\n", l->rotational ? 1 : 0);
		break;
	case Q_SCHEDULER:
		r = blk_queue_sched_show(q, buf, cap);
		break;
	case Q_NR_REQUESTS:
		r = pfs_printf(buf, cap, 0, "%u\n", q->nr_requests);
		break;
	case Q_DISCARD_GRAN:
		r = pfs_printf(buf, cap, 0, "%u\n", l->discard_granularity);
		break;
	case Q_DISCARD_MAX:
	case Q_DISCARD_MAX_HW:
		r = pfs_printf(buf, cap, 0, "%llu\n",
			       (unsigned long long)(l->max_discard_sectors *
						    l->logical_block_size));
		break;
	case Q_DISCARD_ZEROES:
		r = pfs_printf(buf, cap, 0, "0\n");
		break;
	case Q_WRITE_ZEROES_MAX:
		r = pfs_printf(buf, cap, 0, "%llu\n",
			       (unsigned long long)(l->max_write_zeroes_sectors *
						    l->logical_block_size));
		break;
	case Q_WRITE_CACHE:
		r = pfs_printf(buf, cap, 0, "%s\n",
			       l->write_cache ? "write back" : "write through");
		break;
	case Q_FUA:
		r = pfs_printf(buf, cap, 0, "%u\n", l->fua ? 1 : 0);
		break;
	case Q_NOMERGES:
		r = pfs_printf(buf, cap, 0, "%d\n",
			       (q->flags & BLK_QUEUE_FL_NOMERGES) ? 2 : 0);
		break;
	case Q_RQ_AFFINITY:
		/* 0: complete where the interrupt came; 1: in the
		 * submitter's cache domain; 2: on the submitting CPU. */
		r = pfs_printf(buf, cap, 0, "%u\n",
			       (unsigned)__atomic_load_n(&q->rq_affinity,
							 __ATOMIC_RELAXED));
		break;
	case Q_READ_AHEAD:
		r = pfs_printf(buf, cap, 0, "%u\n", q->read_ahead_kb);
		break;
	case Q_ZONED:
		r = pfs_printf(buf, cap, 0, "none\n");
		break;
	case Q_DAX:
	case Q_ADD_RANDOM:
	case Q_IO_POLL:
	case Q_CHUNK:
		r = pfs_printf(buf, cap, 0, "0\n");
		break;
	case Q_IOSTATS:
		r = pfs_printf(buf, cap, 0, "1\n");
		break;
	case Q_DMA_ALIGN:
		r = pfs_printf(buf, cap, 0, "%u\n", l->dma_alignment);
		break;
	case D_MODEL:
		r = blk_print_trim(buf, cap, d->model);
		break;
	case D_VENDOR:
		r = blk_print_trim(buf, cap, d->vendor);
		break;
	case D_REV:
		r = blk_print_trim(buf, cap, d->firmware);
		break;
	case D_SERIAL:
		r = blk_print_trim(buf, cap, d->serial);
		break;
	case D_WWID:
		/* the SCSI identifier tools match on: the WWN when there is
		 * one, else the vendor-model-serial triple */
		if (d->wwn[0])
			r = pfs_printf(buf, cap, 0, "naa.%s\n",
				       (d->wwn[0] == '0' &&
					(d->wwn[1] == 'x' || d->wwn[1] == 'X')) ?
					       d->wwn + 2 :
					       d->wwn);
		else
			r = pfs_printf(buf, cap, 0, "t10.%s %s %s\n", d->vendor,
				       d->model, d->serial);
		break;
	case D_STATE:
		r = pfs_printf(buf, cap, 0, "%s\n",
			       (d->flags & GENHD_FL_DEAD) ? "offline" :
			       (d->flags & GENHD_FL_SUSPENDED) ? "quiesce" :
								  "running");
		break;
	case D_QUEUE_DEPTH:
		r = pfs_printf(buf, cap, 0, "%u\n", q->depth);
		break;
	case D_TIMEOUT:
		r = pfs_printf(buf, cap, 0, "%u\n", BLK_DEFAULT_TIMEOUT_MS / 1000);
		break;
	case D_TYPE:
		r = pfs_printf(buf, cap, 0, "%d\n", d->scsi_type);
		break;
	case D_BLOCKED:
		r = pfs_printf(buf, cap, 0, "%d\n",
			       (q->flags & BLK_QUEUE_FL_STOPPED) ? 1 : 0);
		break;
	case D_IOREQ:
		r = pfs_printf(buf, cap, 0, "0x%llx\n",
			       (unsigned long long)q->dispatched);
		break;
	case D_IODONE:
		r = pfs_printf(buf, cap, 0, "0x%llx\n",
			       (unsigned long long)q->completed);
		break;
	case D_IOERR:
		r = pfs_printf(buf, cap, 0, "0x%llx\n",
			       (unsigned long long)q->errors);
		break;
	case P_STATE:
		r = pfs_printf(buf, cap, 0, "%s\n",
			       (d->flags & GENHD_FL_SUSPENDED) ? "suspended" :
								  "running");
		break;
	default:
		r = -EINVAL;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return r;
}

/* ==========================================================================
 * Stores
 * ========================================================================== */

static int blk_parse_u64(const char *buf, long len, uint64_t *out)
{
	uint64_t v = 0;
	long i = 0;

	while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
		i++;
	if (i >= len || buf[i] < '0' || buf[i] > '9')
		return -EINVAL;
	for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
		uint64_t nv = v * 10 + (uint64_t)(buf[i] - '0');
		if (nv < v)
			return -EINVAL;
		v = nv;
	}
	while (i < len && (buf[i] == '\n' || buf[i] == ' '))
		i++;
	if (i != len)
		return -EINVAL;
	*out = v;
	return 0;
}

/* `buf' (len bytes, maybe with a newline) equals `word'. */
static int blk_word_is(const char *buf, long len, const char *word)
{
	long wl = (long)kstrlen(word);

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		len--;
	if (len != wl)
		return 0;
	for (long i = 0; i < len; i++)
		if (buf[i] != word[i])
			return 0;
	return 1;
}

static long blk_power_store_disk(struct gendisk *d, const char *buf, long len)
{
	int rc;

	if (blk_word_is(buf, len, "suspend") || blk_word_is(buf, len, "off")) {
		blk_mutex_lock(&g_blk_pm_mutex);
		rc = blk_disk_suspend(d);
		blk_mutex_unlock(&g_blk_pm_mutex);
	} else if (blk_word_is(buf, len, "resume") ||
		   blk_word_is(buf, len, "on")) {
		blk_mutex_lock(&g_blk_pm_mutex);
		rc = blk_disk_resume(d);
		blk_mutex_unlock(&g_blk_pm_mutex);
	} else {
		return -EINVAL;
	}
	return rc ? rc : len;
}

static long blk_attr_store(struct pfs_node *n, const char *buf, long len)
{
	block_device_t *b;
	struct gendisk *d;
	struct blk_queue *q;
	uint64_t v = 0, fl;
	long r = len;
	char word[32];

	blk_mutex_lock(&g_disk_registry_mutex);
	b = blk_node_bdev_locked(n);
	if (!b) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return -ENODEV;
	}
	d = b->disk;
	q = d->queue;
	switch (blk_node_attr(n)) {
	case Q_MAX_SECTORS_KB:
		if (blk_parse_u64(buf, len, &v)) {
			r = -EINVAL;
			break;
		}
		{
			uint64_t s = v * 1024 / q->limits.logical_block_size;
			if (!s || s > q->limits.max_hw_sectors) {
				r = -EINVAL;
				break;
			}
			spin_lock_irqsave(&q->lock, &fl);
			q->limits.max_sectors = (uint32_t)s;
			spin_unlock_irqrestore(&q->lock, fl);
			d->limits.max_sectors = (uint32_t)s;
			d->bdev.max_sectors = (uint32_t)s;
			for (int i = 1; i <= DISK_MAX_PARTS; i++)
				if (d->part[i])
					d->part[i]->bdev.max_sectors = (uint32_t)s;
		}
		break;
	case Q_ROTATIONAL:
		if (blk_parse_u64(buf, len, &v) || v > 1) {
			r = -EINVAL;
			break;
		}
		q->limits.rotational = (uint8_t)v;
		d->bdev.rotational = (uint8_t)v;
		for (int i = 1; i <= DISK_MAX_PARTS; i++)
			if (d->part[i])
				d->part[i]->bdev.rotational = (uint8_t)v;
		break;
	case Q_SCHEDULER: {
		long k = len < (long)sizeof(word) - 1 ? len : (long)sizeof(word) - 1;
		for (long i = 0; i < k; i++)
			word[i] = buf[i];
		word[k] = 0;
		if (blk_queue_set_scheduler(q, word))
			r = -EINVAL;
		break;
	}
	case Q_NR_REQUESTS:
		if (blk_parse_u64(buf, len, &v) || v < 4 || v > 4096) {
			r = -EINVAL;
			break;
		}
		spin_lock_irqsave(&q->lock, &fl);
		q->nr_requests = (uint32_t)v;
		spin_unlock_irqrestore(&q->lock, fl);
		break;
	case Q_WRITE_CACHE: {
		/* The kernel's view of the cache only (the established
		 * meaning); turning the drive's cache itself on or off is
		 * hdparm -W / SG_IO's business. */
		int on;
		if (blk_word_is(buf, len, "write back"))
			on = 1;
		else if (blk_word_is(buf, len, "write through") ||
			 blk_word_is(buf, len, "none"))
			on = 0;
		else {
			r = -EINVAL;
			break;
		}
		q->limits.write_cache = (uint8_t)on;
		d->bdev.flush_supported = (uint8_t)on;
		for (int i = 1; i <= DISK_MAX_PARTS; i++)
			if (d->part[i])
				d->part[i]->bdev.flush_supported = (uint8_t)on;
		break;
	}
	case Q_NOMERGES:
		if (blk_parse_u64(buf, len, &v) || v > 2) {
			r = -EINVAL;
			break;
		}
		spin_lock_irqsave(&q->lock, &fl);
		if (v)
			q->flags |= BLK_QUEUE_FL_NOMERGES;
		else
			q->flags &= ~BLK_QUEUE_FL_NOMERGES;
		spin_unlock_irqrestore(&q->lock, fl);
		break;
	case Q_RQ_AFFINITY:
		if (blk_parse_u64(buf, len, &v) || v > 2) {
			r = -EINVAL;
			break;
		}
		/* Read by blk_complete_request() without a lock. */
		__atomic_store_n(&q->rq_affinity, (uint8_t)v, __ATOMIC_RELAXED);
		break;
	case Q_READ_AHEAD:
		if (blk_parse_u64(buf, len, &v) || v > 65536) {
			r = -EINVAL;
			break;
		}
		q->read_ahead_kb = (uint32_t)v;
		break;
	case P_STATE:
		/* The driver's suspend must not run under the registry
		 * mutex: hold the disk by a reference instead. */
		disk_get(d);
		blk_mutex_unlock(&g_disk_registry_mutex);
		r = blk_power_store_disk(d, buf, len);
		disk_put(d);
		return r;
	default:
		r = -EINVAL;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return r;
}

/* ==========================================================================
 * Dynamic directories
 * ========================================================================== */

static struct pfs_node *blk_make_node(struct pfs_node *dir, const char *name,
				      const struct blk_attr *a,
				      struct gendisk *d, int partno)
{
	struct pfs_node *n;

	if (a->type == T_DIR) {
		n = pfs_node_new(dir->fs, dir, name, PFS_DIR);
		if (!n)
			return NULL;
		/* The parent is itself a lookup's transient node and is freed
		 * as soon as the walk moves on; a listing must not reach back
		 * into it for "..". */
		n->parent = NULL;
		n->arg = d;
		n->arg2 = ((uint64_t)a->id << 24) | ((uint64_t)partno << 16);
		pfs_set_dynamic(n, NULL, NULL);
		return n;
	}
	if (a->type == T_LINK) {
		n = pfs_node_new(dir->fs, dir, name, PFS_LINK);
		if (n)
			ksnprintf(n->link, sizeof(n->link), "%s", a->link);
		return n;
	}
	n = pfs_node_new(dir->fs, dir, name, PFS_FILE);
	if (!n)
		return NULL;
	n->arg = d;
	n->arg2 = ((uint64_t)partno << 16) | a->id;
	n->show = blk_attr_show;
	if (a->writable) {
		n->store = blk_attr_store;
		n->mode = 0644;
	}
	return n;
}

static int blk_dir_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type);
static struct pfs_node *blk_dir_lookup(struct pfs_node *dir, const char *name);

static int blk_attr_dtype(const struct blk_attr *a)
{
	return a->type == T_DIR ? DT_DIR : a->type == T_LINK ? DT_LNK : DT_REG;
}

#define BLK_LIST_PART_KEY 1000u

static int blk_dir_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type)
{
	int kind = blk_node_kind(dir);
	int n = 0;
	const struct blk_attr *t = blk_table(kind, &n);
	unsigned c = *cursor;

	if (c < (unsigned)n) {
		ksnprintf(name, (size_t)cap, "%s", t[c].name);
		*type = blk_attr_dtype(&t[c]);
		*cursor = c + 1;
		return 1;
	}
	if (kind != DK_DISK)
		return 0;
	/* the disk's partitions, keyed by number */
	{
		struct gendisk *d = (struct gendisk *)dir->arg;
		unsigned from = c < BLK_LIST_PART_KEY ? 1 : c - BLK_LIST_PART_KEY;
		int found = 0;

		blk_mutex_lock(&g_disk_registry_mutex);
		if (d && genhd_disk_alive_locked(d)) {
			for (unsigned i = from ? from : 1; i <= DISK_MAX_PARTS;
			     i++) {
				if (d->part[i]) {
					ksnprintf(name, (size_t)cap, "%s",
						  d->part[i]->dev_name);
					*type = DT_DIR;
					*cursor = BLK_LIST_PART_KEY + i + 1;
					found = 1;
					break;
				}
			}
		}
		blk_mutex_unlock(&g_disk_registry_mutex);
		return found;
	}
}

static struct pfs_node *blk_dir_lookup(struct pfs_node *dir, const char *name)
{
	int kind = blk_node_kind(dir);
	int partno = blk_node_partno(dir);
	struct gendisk *d = (struct gendisk *)dir->arg;
	int n = 0;
	const struct blk_attr *t = blk_table(kind, &n);
	struct pfs_node *r = NULL;

	for (int i = 0; i < n; i++)
		if (kstrcmp(t[i].name, name) == 0) {
			r = blk_make_node(dir, name, &t[i], d, partno);
			break;
		}
	if (r && r->type == PFS_DIR)
		pfs_set_dynamic(r, blk_dir_list, blk_dir_lookup);
	if (r || kind != DK_DISK)
		return r;
	/* a partition directory */
	blk_mutex_lock(&g_disk_registry_mutex);
	if (d && genhd_disk_alive_locked(d)) {
		for (int i = 1; i <= DISK_MAX_PARTS; i++) {
			struct blk_partition *p = d->part[i];
			if (p && kstrcmp(p->dev_name, name) == 0) {
				static const struct blk_attr pa = {
					"", DK_PART, T_DIR, 0, NULL
				};
				r = blk_make_node(dir, name, &pa, d, i);
				if (r)
					pfs_set_dynamic(r, blk_dir_list,
							blk_dir_lookup);
				break;
			}
		}
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return r;
}

/* Keys of the flat walks: sd disk i -> i*256, sr drive i -> (128+i)*256,
 * plus the partition number. */
static unsigned blk_disk_key(const struct gendisk *d)
{
	return (unsigned)((d->kind == BLK_DISK_SD ? d->index :
						    DISK_MAX_DISKS + d->index) *
			  256);
}

/* The device with the smallest key >= `from' (whole disks only when
 * `parts' is 0).  Registry mutex held. */
static block_device_t *blk_walk_from_locked(unsigned from, int parts,
					    unsigned *key)
{
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		unsigned base = blk_disk_key(d);

		if (base >= from) {
			*key = base;
			return &d->bdev;
		}
		if (!parts || base + 255 < from)
			continue;
		for (unsigned i = from - base; i <= DISK_MAX_PARTS; i++) {
			if (i && d->part[i]) {
				*key = base + i;
				return &d->part[i]->bdev;
			}
		}
	}
	return NULL;
}

/* "../../block/sda" or "../../block/sda/sda1" */
static void blk_link_target(const block_device_t *b, char *out, size_t cap)
{
	if (b->part)
		ksnprintf(out, cap, "../../block/%s/%s", b->disk->disk_name,
			  b->name);
	else
		ksnprintf(out, cap, "../../block/%s", b->name);
}

/* /sys/block */
static int blk_top_list(struct pfs_node *dir, unsigned *cursor, char *name,
			long cap, int *type)
{
	block_device_t *b;
	unsigned key = 0;
	int found = 0;

	(void)dir;
	blk_mutex_lock(&g_disk_registry_mutex);
	b = blk_walk_from_locked(*cursor, 0, &key);
	if (b) {
		ksnprintf(name, (size_t)cap, "%s", b->name);
		*type = DT_DIR;
		*cursor = key + 1;
		found = 1;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return found;
}

static struct pfs_node *blk_top_lookup(struct pfs_node *dir, const char *name)
{
	struct pfs_node *r = NULL;

	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		if (kstrcmp(d->disk_name, name) == 0 &&
		    !(d->flags & GENHD_FL_HIDDEN)) {
			r = pfs_node_new(dir->fs, dir, name, PFS_DIR);
			if (r) {
				r->arg = d;
				r->arg2 = (uint64_t)DK_DISK << 24;
				pfs_set_dynamic(r, blk_dir_list, blk_dir_lookup);
			}
			break;
		}
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return r;
}

/* /sys/class/block and /sys/dev/block: links (arg2 of the dir: 0 names,
 * 1 major:minor) */
static int blk_flat_list(struct pfs_node *dir, unsigned *cursor, char *name,
			 long cap, int *type)
{
	block_device_t *b;
	unsigned key = 0;
	int found = 0;

	blk_mutex_lock(&g_disk_registry_mutex);
	b = blk_walk_from_locked(*cursor, 1, &key);
	if (b) {
		if (dir->arg2)
			ksnprintf(name, (size_t)cap, "%u:%u", b->major, b->minor);
		else
			ksnprintf(name, (size_t)cap, "%s", b->name);
		*type = DT_LNK;
		*cursor = key + 1;
		found = 1;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return found;
}

static struct pfs_node *blk_flat_lookup(struct pfs_node *dir, const char *name)
{
	block_device_t *b = NULL;
	struct pfs_node *r = NULL;

	blk_mutex_lock(&g_disk_registry_mutex);
	if (dir->arg2) {
		uint32_t ma = 0, mi = 0;
		const char *p = name;
		int ok = *p >= '0' && *p <= '9';

		while (*p >= '0' && *p <= '9')
			ma = ma * 10 + (uint32_t)(*p++ - '0');
		if (ok && *p == ':') {
			p++;
			ok = *p >= '0' && *p <= '9';
			while (*p >= '0' && *p <= '9')
				mi = mi * 10 + (uint32_t)(*p++ - '0');
			if (ok && !*p)
				b = genhd_find_devt_locked(ma, mi);
		}
	} else {
		b = genhd_find_name_locked(name);
	}
	if (b && !(b->disk->flags & GENHD_FL_HIDDEN)) {
		r = pfs_node_new(dir->fs, dir, name, PFS_LINK);
		if (r)
			blk_link_target(b, r->link, sizeof(r->link));
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return r;
}

/* /sys/power/disk_state */
static long blk_power_show(struct pfs_node *n, char *buf, long cap)
{
	int susp = 0, total = 0;

	(void)n;
	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		total++;
		if (d->flags & GENHD_FL_SUSPENDED)
			susp++;
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return pfs_printf(buf, cap, 0, "%s\n",
			  !susp ? "running" :
			  susp == total ? "suspended" :
					  "partially suspended");
}

static long blk_power_store(struct pfs_node *n, const char *buf, long len)
{
	int rc;

	(void)n;
	if (blk_word_is(buf, len, "suspend") || blk_word_is(buf, len, "off"))
		rc = blk_suspend_all();
	else if (blk_word_is(buf, len, "resume") || blk_word_is(buf, len, "on"))
		rc = blk_resume_all();
	else
		return -EINVAL;
	return rc ? rc : len;
}

/* ==========================================================================
 * Registration hooks (the tree is dynamic: these only keep the names)
 * ========================================================================== */

int blk_sysfs_add_disk_locked(struct gendisk *disk)
{
	if (!disk)
		return -EINVAL;
	ksnprintf(disk->sysfs_base, sizeof(disk->sysfs_base), "block/%s",
		  disk->disk_name);
	return 0;
}

void blk_sysfs_remove_disk_locked(struct gendisk *disk)
{
	if (disk)
		disk->sysfs_base[0] = 0;
}

int blk_sysfs_add_partition_locked(struct blk_partition *part)
{
	if (!part)
		return -EINVAL;
	ksnprintf(part->sysfs_base, sizeof(part->sysfs_base), "block/%s/%s",
		  part->disk->disk_name, part->dev_name);
	return 0;
}

void blk_sysfs_remove_partition_locked(struct blk_partition *part)
{
	if (part)
		part->sysfs_base[0] = 0;
}

void blk_sysfs_init(void)
{
	static int done;
	struct pfs_node *d;

	if (done)
		return;
	done = 1;
	blkdev_nodes_init();
	d = sysfs_mkdir("block");
	if (d)
		pfs_set_dynamic(d, blk_top_list, blk_top_lookup);
	d = sysfs_mkdir("class/block");
	if (d) {
		d->arg2 = 0;
		pfs_set_dynamic(d, blk_flat_list, blk_flat_lookup);
	}
	d = sysfs_mkdir("dev/block");
	if (d) {
		d->arg2 = 1;
		pfs_set_dynamic(d, blk_flat_list, blk_flat_lookup);
	}
	if (sysfs_mkdir("power"))
		sysfs_add_attr("power", "disk_state", blk_power_show,
			       blk_power_store, NULL, 0);
}

/* ==========================================================================
 * /proc/partitions and /proc/diskstats
 * ========================================================================== */

long blk_proc_partitions_show(struct pfs_node *n, char *buf, long cap)
{
	long p;

	(void)n;
	p = pfs_printf(buf, cap, 0, "major minor  #blocks  name\n\n");
	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		if (d->flags & GENHD_FL_HIDDEN)
			continue;
		p = pfs_printf(buf, cap, p, "%4u %7u %10llu %s\n", d->bdev.major,
			       d->bdev.minor,
			       (unsigned long long)((uint64_t)d->bdev.total_sectors *
						    d->bdev.sector_size / 1024),
			       d->disk_name);
		for (int i = 1; i <= DISK_MAX_PARTS; i++) {
			struct blk_partition *pt = d->part[i];
			if (!pt)
				continue;
			p = pfs_printf(buf, cap, p, "%4u %7u %10llu %s\n",
				       pt->bdev.major, pt->bdev.minor,
				       (unsigned long long)((uint64_t)pt->bdev
								    .total_sectors *
							    pt->bdev.sector_size /
							    1024),
				       pt->dev_name);
		}
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return p;
}

static long blk_diskstats_line(char *buf, long cap, long p,
			       const block_device_t *b,
			       const struct blk_disk_stats *s)
{
	return pfs_printf(
		buf, cap, p,
		"%4u %7u %s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
		b->major, b->minor, b->name,
		(unsigned long long)s->ios[BLK_STAT_READ],
		(unsigned long long)s->merges[BLK_STAT_READ],
		(unsigned long long)s->sectors[BLK_STAT_READ],
		(unsigned long long)s->ticks_ms[BLK_STAT_READ],
		(unsigned long long)s->ios[BLK_STAT_WRITE],
		(unsigned long long)s->merges[BLK_STAT_WRITE],
		(unsigned long long)s->sectors[BLK_STAT_WRITE],
		(unsigned long long)s->ticks_ms[BLK_STAT_WRITE],
		(unsigned long long)(s->in_flight[0] + s->in_flight[1]),
		(unsigned long long)s->io_ticks_ms,
		(unsigned long long)s->time_in_queue_ms,
		(unsigned long long)s->ios[BLK_STAT_DISCARD],
		(unsigned long long)s->merges[BLK_STAT_DISCARD],
		(unsigned long long)s->sectors[BLK_STAT_DISCARD],
		(unsigned long long)s->ticks_ms[BLK_STAT_DISCARD],
		(unsigned long long)s->ios[BLK_STAT_FLUSH],
		(unsigned long long)s->ticks_ms[BLK_STAT_FLUSH]);
}

long blk_proc_diskstats_show(struct pfs_node *n, char *buf, long cap)
{
	long p = 0;
	struct blk_disk_stats s;

	(void)n;
	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		if (d->flags & GENHD_FL_HIDDEN)
			continue;
		blk_queue_stats_snapshot(d->queue, &d->stats, &s);
		p = blk_diskstats_line(buf, cap, p, &d->bdev, &s);
		for (int i = 1; i <= DISK_MAX_PARTS; i++) {
			struct blk_partition *pt = d->part[i];
			if (!pt)
				continue;
			blk_queue_stats_snapshot(d->queue, &pt->stats, &s);
			p = blk_diskstats_line(buf, cap, p, &pt->bdev, &s);
		}
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return p;
}
