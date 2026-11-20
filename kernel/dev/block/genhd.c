// LikeOS - disks: the disk registry, names, device numbers, the life of a
// gendisk from disk_alloc() to its last reference, capacity and media
// changes, and the lookups the storage bootstrap and the ioctls use.
//
// ---- Names and numbers -----------------------------------------------------------
// Disks are named sda, sdb ... sdz, sdaa ... in the order they are
// allocated, the lowest free index first (a disk unplugged and plugged in
// again gets its old name back while nothing took it meanwhile).  Each sd
// disk owns 16 minors: 16*i for the disk, 16*i+1 .. 16*i+15 for its first
// fifteen partitions; disks 0..15 sit on major 8, disks 16..127 on majors
// 65..71.  Partitions numbered 16 and above get a minor of the extended
// major (BLK_EXT_MAJOR) in order of appearance.  Optical drives are sr0,
// sr1 ... on major 11 with one minor each.  These are the numbers st_rdev
// reports, /sys/dev/block lists and /proc/partitions prints.
//
// diskseq: a counter that never repeats within a boot, bumped for every
// disk added and for every media or partition table change of a disk, so
// that a program holding "diskseq 7" can tell it still talks to the same
// medium (BLKGETDISKSEQ, /sys/block/<disk>/diskseq, by-diskseq links).
//
// ---- Lifetime --------------------------------------------------------------------------
// A gendisk is reference counted (refcount_t in struct gendisk): the
// registry holds one from disk_alloc() until disk_del(); every reference
// to its whole-disk block device holds one (open handles, mounts, requests
// in flight); every partition holds one for as long as it exists.  When
// the last goes, the driver's free_disk runs and the memory goes with the
// queue.  A last reference dropped where sleeping is impossible (a request
// completing in an interrupt handler) is handed to a deferred list that
// the next registry operation -- or any submitter that may sleep -- runs.
//
// ---- Locking --------------------------------------------------------------------------
// g_disk_registry_mutex (a blk_mutex, outermost lock of the storage stack)
// covers the index tables, the extended minor bitmap, each disk's
// partition table and the open counts and holders of every device.  It
// is held across partition scans (which read the disk) and across queue
// teardown; it is NOT held across a driver's suspend/resume/shutdown.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/fs/devevent.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/ida.h>
#include <kernel/uapi/bug.h>

struct blk_mutex g_disk_registry_mutex = BLK_MUTEX_INIT("disk_registry");

/* Index tables: the disk that owns sd index i / sr index i, including one
 * that is allocated but not yet added.  Registry mutex. */
static struct gendisk *g_sd_disks[DISK_MAX_DISKS];
static struct gendisk *g_sr_disks[DISK_MAX_SR];
/* Minors of the extended major (BLK_EXT_MAJOR): every disk of a named
 * kind and its partitions, and sd partitions numbered 16 and above, take
 * the lowest free one.  Its own lock (any context that may allocate). */
static DEFINE_IDA(g_blk_ext_ida);
/* Disks of named kinds (BLK_DISK_NVME), in index order, and their
 * indexes (the sysfs walk keys).  Registry mutex. */
static struct gendisk *g_named_disks;
static DEFINE_IDA(g_named_index_ida);
/* The disk sequence counter. */
static uint64_t g_diskseq;
/* Disks currently added (sd + sr). */
static int g_disks_up;

/* Deferred releases: disks and partitions whose last reference went in a
 * context that cannot sleep. */
static spinlock_t g_defer_lock = SPINLOCK_INIT("genhd_defer");
static struct gendisk *g_defer_disks;
#define GENHD_DEFER_PARTS 64
static struct blk_partition *g_defer_parts[GENHD_DEFER_PARTS];
static int g_defer_nparts;
/* Driver releases parked on a block softirq section, handed over at its
 * end (blk_cpl_end). */
static struct blk_release *g_defer_items;

/* kblockd's reaping of the deferred lists: queued whenever something is
 * deferred from a context that cannot release it.  Initialised on first
 * use (any context): 0 not yet, 1 being initialised, 2 ready. */
static struct blk_work g_genhd_reap_work;
static volatile int g_genhd_reap_state;

/* The innermost block softirq completion section of each CPU. */
static struct blk_cpl_ctx *g_softirq_cpl[MAX_CPUS];

/* ==========================================================================
 * Names and device numbers (pure)
 * ========================================================================== */

int disk_name_for_index(int index, char *out, size_t cap)
{
	char tmp[DISK_NAME_LEN];
	int n = 0;

	if (!out || cap == 0 || index < 0)
		return -EINVAL;
	/* Bijective base 26: a..z, aa..zz, aaa..  (index 0 = "a"). */
	do {
		if (n >= (int)sizeof(tmp) - 1)
			return -ENAMETOOLONG;
		tmp[n++] = (char)('a' + index % 26);
		index = index / 26 - 1;
	} while (index >= 0);
	if ((size_t)n + 3 > cap)
		return -ENAMETOOLONG;
	out[0] = 's';
	out[1] = 'd';
	for (int i = 0; i < n; i++)
		out[2 + i] = tmp[n - 1 - i];
	out[2 + n] = 0;
	return 0;
}

int disk_part_name(const char *disk_name, int partno, char *out, size_t cap)
{
	size_t l;
	int need_p;
	int w;

	if (!disk_name || !out || cap == 0 || partno <= 0)
		return -EINVAL;
	l = kstrlen(disk_name);
	if (!l)
		return -EINVAL;
	/* "sda" + 1 = "sda1"; a name ending in a digit takes a 'p' so the
	 * two numbers do not run together ("nvme0n1p1"). */
	need_p = disk_name[l - 1] >= '0' && disk_name[l - 1] <= '9';
	w = ksnprintf(out, cap, need_p ? "%sp%d" : "%s%d", disk_name, partno);
	if (w < 0 || (size_t)w >= cap)
		return -ENAMETOOLONG;
	return 0;
}

int disk_devt_for_index(int index, int partno, uint32_t *major,
			uint32_t *minor)
{
	if (index < 0 || index >= DISK_MAX_DISKS || partno < 0)
		return -EINVAL;
	if (partno >= DISK_MINORS)
		return -ERANGE;
	if (major)
		*major = index < 16 ? BLK_SD_MAJOR :
				      BLK_SD_MAJOR_EXT_FIRST +
					      (uint32_t)(index - 16) / 16;
	if (minor)
		*minor = 16u * (uint32_t)(index % 16) + (uint32_t)partno;
	return 0;
}

/* ==========================================================================
 * Registry walks
 * ========================================================================== */

static int genhd_disk_listed(const struct gendisk *d)
{
	return d && (d->flags & GENHD_FL_UP) && !(d->flags & GENHD_FL_DEAD);
}

/* The first listed named disk at or after `d' in the named list. */
static struct gendisk *genhd_named_from(struct gendisk *d)
{
	for (; d; d = d->named_next)
		if (genhd_disk_listed(d))
			return d;
	return NULL;
}

struct gendisk *genhd_first_locked(void)
{
	for (int i = 0; i < DISK_MAX_DISKS; i++)
		if (genhd_disk_listed(g_sd_disks[i]))
			return g_sd_disks[i];
	for (int i = 0; i < DISK_MAX_SR; i++)
		if (genhd_disk_listed(g_sr_disks[i]))
			return g_sr_disks[i];
	return genhd_named_from(g_named_disks);
}

struct gendisk *genhd_next_locked(struct gendisk *disk)
{
	int i;

	if (!disk)
		return NULL;
	if (disk->kind == BLK_DISK_NVME)
		return genhd_named_from(disk->named_next);
	if (disk->kind == BLK_DISK_SD) {
		for (i = disk->index + 1; i < DISK_MAX_DISKS; i++)
			if (genhd_disk_listed(g_sd_disks[i]))
				return g_sd_disks[i];
		i = 0;
	} else {
		i = disk->index + 1;
	}
	for (; i < DISK_MAX_SR; i++)
		if (genhd_disk_listed(g_sr_disks[i]))
			return g_sr_disks[i];
	return genhd_named_from(g_named_disks);
}

int genhd_disk_alive_locked(const struct gendisk *disk)
{
	if (!disk)
		return 0;
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d))
		if (d == disk)
			return 1;
	return 0;
}

int genhd_disk_count(void)
{
	return __atomic_load_n(&g_disks_up, __ATOMIC_RELAXED);
}

int disk_for_each(int (*fn)(struct gendisk *disk, void *arg), void *arg)
{
	int rc = 0;

	if (!fn)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d && !rc;
	     d = genhd_next_locked(d))
		rc = fn(d, arg);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

block_device_t *genhd_find_name_locked(const char *name)
{
	if (!name || !*name)
		return NULL;
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		if (kstrcmp(d->disk_name, name) == 0)
			return &d->bdev;
		for (int i = 1; i <= DISK_MAX_PARTS; i++) {
			struct blk_partition *p = d->part[i];
			if (p && kstrcmp(p->dev_name, name) == 0)
				return &p->bdev;
		}
	}
	return NULL;
}

block_device_t *genhd_find_devt_locked(uint32_t major, uint32_t minor)
{
	for (struct gendisk *d = genhd_first_locked(); d;
	     d = genhd_next_locked(d)) {
		if (d->bdev.major == major && d->bdev.minor == minor)
			return &d->bdev;
		for (int i = 1; i <= DISK_MAX_PARTS; i++) {
			struct blk_partition *p = d->part[i];
			if (p && p->bdev.major == major &&
			    p->bdev.minor == minor)
				return &p->bdev;
		}
	}
	return NULL;
}

/* ==========================================================================
 * Extended minors and the sequence counter
 * ========================================================================== */

int blk_ext_devt_alloc(uint32_t *minor)
{
	int m;

	if (!minor)
		return -EINVAL;
	m = ida_alloc_range(&g_blk_ext_ida, 0, KDEV_MINOR_MAX);
	if (m < 0)
		return m == -ENOMEM ? -ENOMEM : -ENOSPC;
	*minor = (uint32_t)m;
	return 0;
}

void blk_ext_devt_free(uint32_t minor)
{
	if (WARN_ON(minor > KDEV_MINOR_MAX))
		return;
	WARN_ON(!ida_is_allocated(&g_blk_ext_ida, minor));
	ida_free(&g_blk_ext_ida, minor);
}

/* The partition code's names for the same allocator (sd partitions >= 16
 * and every partition of a named disk). */
int genhd_ext_minor_alloc_locked(uint32_t *minor)
{
	return blk_ext_devt_alloc(minor);
}

void genhd_ext_minor_free_locked(uint32_t minor)
{
	blk_ext_devt_free(minor);
}

uint64_t genhd_next_diskseq(void)
{
	return __atomic_add_fetch(&g_diskseq, 1, __ATOMIC_SEQ_CST);
}

/* ==========================================================================
 * Text helpers
 * ========================================================================== */

/* "500 GB/466 GiB": the decimal size the drive is sold as, and the
 * binary one tools print. */
static void genhd_format_size(uint64_t bytes, char *out, size_t cap)
{
	static const char *const dec[] = { "B", "kB", "MB", "GB", "TB", "PB" };
	static const char *const bin[] = { "B", "KiB", "MiB", "GiB", "TiB",
					   "PiB" };
	uint64_t d = bytes, b = bytes;
	uint64_t drem = 0, brem = 0;
	int di = 0, bi = 0;

	while (d >= 1000 && di < 5) {
		drem = d % 1000;
		d /= 1000;
		di++;
	}
	while (b >= 1024 && bi < 5) {
		brem = b % 1024;
		b /= 1024;
		bi++;
	}
	ksnprintf(out, cap, "%llu.%llu %s/%llu.%llu %s", (unsigned long long)d,
		  (unsigned long long)(drem / 100), dec[di],
		  (unsigned long long)b, (unsigned long long)(brem * 10 / 1024),
		  bin[bi]);
}

/* Copy an identity string for a by-id name: trimmed, runs of white space
 * become one '_', characters outside the portable set become '_'. */
static size_t genhd_id_append(char *out, size_t pos, size_t cap,
			      const char *s)
{
	size_t start, end;
	int space = 0;

	if (!s)
		return pos;
	start = 0;
	while (s[start] == ' ' || s[start] == '\t')
		start++;
	end = kstrlen(s);
	while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
			       s[end - 1] == '\n'))
		end--;
	for (size_t i = start; i < end && pos + 1 < cap; i++) {
		char c = s[i];
		int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
			 (c >= 'A' && c <= 'Z') || c == '#' || c == '+' ||
			 c == '-' || c == '.' || c == ':' || c == '=' ||
			 c == '@' || c == '_';
		if (c == ' ' || c == '\t') {
			if (space)
				continue;
			space = 1;
			out[pos++] = '_';
			continue;
		}
		space = 0;
		out[pos++] = ok ? c : '_';
	}
	out[pos] = 0;
	return pos;
}

static size_t genhd_str_append(char *out, size_t pos, size_t cap,
			       const char *s)
{
	while (s && *s && pos + 1 < cap)
		out[pos++] = *s++;
	if (cap)
		out[pos < cap ? pos : cap - 1] = 0;
	return pos;
}

int blk_bdev_disk_id(const block_device_t *bdev, char *out, size_t cap)
{
	const struct gendisk *d;
	size_t pos = 0;
	char num[16];

	if (!bdev || !out || cap < 8)
		return -EINVAL;
	if (!blk_bdev_is_extended(bdev) || !bdev->disk)
		return -ENODEV;
	d = bdev->disk;
	if (!d->model[0] && !d->serial[0])
		return -ENOENT;
	out[0] = 0;
	if (kstrcmp(d->bus, "usb") == 0) {
		/* usb-<vendor>_<model>_<serial>-0:<lun> */
		pos = genhd_str_append(out, pos, cap, "usb-");
		pos = genhd_id_append(out, pos, cap, d->vendor);
		pos = genhd_str_append(out, pos, cap, "_");
		pos = genhd_id_append(out, pos, cap, d->model);
		if (d->serial[0]) {
			pos = genhd_str_append(out, pos, cap, "_");
			pos = genhd_id_append(out, pos, cap, d->serial);
		}
		ksnprintf(num, sizeof(num), "-0:%d", d->lun > 0 ? d->lun : 0);
		pos = genhd_str_append(out, pos, cap, num);
	} else {
		/* ata-<model>_<serial> (and <bus>-... for anything else) */
		pos = genhd_str_append(out, pos, cap,
				       d->bus[0] ? d->bus : "ata");
		pos = genhd_str_append(out, pos, cap, "-");
		pos = genhd_id_append(out, pos, cap, d->model);
		if (d->serial[0]) {
			pos = genhd_str_append(out, pos, cap, "_");
			pos = genhd_id_append(out, pos, cap, d->serial);
		}
	}
	if (bdev->part) {
		ksnprintf(num, sizeof(num), "-part%d", bdev->part->partno);
		pos = genhd_str_append(out, pos, cap, num);
	}
	if (pos + 1 >= cap)
		return -ENAMETOOLONG;
	return 0;
}

/* ==========================================================================
 * Deferred releases
 * ========================================================================== */

static void disk_release(struct gendisk *disk);

static void genhd_reap_work_fn(void *arg)
{
	(void)arg;
	blk_reap_deferred_requests();
	genhd_reap_deferred();
}

void genhd_kick_reap(void)
{
	int st;

	/* Before kblockd runs the item would run inline, in whatever context
	 * deferred the release: leave the lists to the next submission's
	 * reap (blk_submit) instead. */
	if (!blk_kblockd_running())
		return;
	st = __atomic_load_n(&g_genhd_reap_state, __ATOMIC_ACQUIRE);
	if (st != 2) {
		int zero = 0;

		if (__atomic_compare_exchange_n(&g_genhd_reap_state, &zero, 1, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE)) {
			blk_work_init(&g_genhd_reap_work, genhd_reap_work_fn,
				      NULL);
			__atomic_store_n(&g_genhd_reap_state, 2,
					 __ATOMIC_RELEASE);
		}
		/* Another CPU initialises it right now: a handful of
		 * instructions. */
		while (__atomic_load_n(&g_genhd_reap_state, __ATOMIC_ACQUIRE) !=
		       2)
			__asm__ volatile("pause" ::: "memory");
	}
	(void)kblockd_schedule_work(&g_genhd_reap_work);
}

/* The global deferred lists, reaped by genhd_reap_deferred(). */
static void genhd_defer_disk(struct gendisk *disk)
{
	uint64_t fl;

	spin_lock_irqsave(&g_defer_lock, &fl);
	disk->next = g_defer_disks;
	g_defer_disks = disk;
	spin_unlock_irqrestore(&g_defer_lock, fl);
	genhd_kick_reap();
}

static void genhd_defer_item(struct blk_release *r)
{
	uint64_t fl;

	spin_lock_irqsave(&g_defer_lock, &fl);
	r->next = g_defer_items;
	g_defer_items = r;
	spin_unlock_irqrestore(&g_defer_lock, fl);
	genhd_kick_reap();
}

static void genhd_defer_partition_global(struct blk_partition *p);

void genhd_defer_partition_release(struct blk_partition *p)
{
	struct blk_cpl_ctx *c = blk_cpl_current();

	/* Inside a completion section: its end releases it. */
	if (c && c->nparts < BLK_CPL_PARTS) {
		c->parts[c->nparts++] = p;
		return;
	}
	genhd_defer_partition_global(p);
}

static void genhd_defer_partition_global(struct blk_partition *p)
{
	uint64_t fl;

	spin_lock_irqsave(&g_defer_lock, &fl);
	if (g_defer_nparts < GENHD_DEFER_PARTS) {
		g_defer_parts[g_defer_nparts++] = p;
		spin_unlock_irqrestore(&g_defer_lock, fl);
		genhd_kick_reap();
		return;
	}
	spin_unlock_irqrestore(&g_defer_lock, fl);
	/* The list is full: leaking a partition record is better than
	 * freeing in interrupt context.  Say so. */
	WARN_RATELIMIT(1, "deferred partition release list full; %s leaked",
		       p->dev_name);
}

void genhd_reap_deferred(void)
{
	struct gendisk *disks;
	struct blk_release *items;
	struct blk_partition *parts[GENHD_DEFER_PARTS];
	int np;
	uint64_t fl;

	if (!__atomic_load_n(&g_defer_disks, __ATOMIC_ACQUIRE) &&
	    !__atomic_load_n(&g_defer_nparts, __ATOMIC_ACQUIRE) &&
	    !__atomic_load_n(&g_defer_items, __ATOMIC_ACQUIRE)) {
		blkdev_nodes_reap();
		return;
	}
	spin_lock_irqsave(&g_defer_lock, &fl);
	disks = g_defer_disks;
	g_defer_disks = NULL;
	items = g_defer_items;
	g_defer_items = NULL;
	np = g_defer_nparts;
	for (int i = 0; i < np; i++)
		parts[i] = g_defer_parts[i];
	g_defer_nparts = 0;
	spin_unlock_irqrestore(&g_defer_lock, fl);

	/* Partitions first: each still holds a disk reference. */
	for (int i = 0; i < np; i++)
		blk_partition_release(parts[i]);
	while (disks) {
		struct gendisk *d = disks;
		disks = d->next;
		d->next = NULL;
		disk_release(d);
	}
	/* Driver items last, as at a section's end. */
	while (items) {
		struct blk_release *r = items;

		items = r->next;
		r->next = NULL;
		r->fn(r);
	}
	blkdev_nodes_reap();
}

/* ==========================================================================
 * Reference counting and release
 * ========================================================================== */

void disk_get(struct gendisk *disk)
{
	if (!disk)
		return;
	VM_BUG_ON(disk->magic != GENDISK_MAGIC);
	refcount_inc(&disk->refcount);
}

/* Take a named disk off the named list and give its index back.
 * Registry mutex held. */
static void genhd_named_unlink_locked(struct gendisk *disk)
{
	struct gendisk **pp;

	for (pp = &g_named_disks; *pp && *pp != disk; pp = &(*pp)->named_next)
		;
	if (*pp) {
		*pp = disk->named_next;
		disk->named_next = NULL;
		if (disk->index >= 0)
			ida_free(&g_named_index_ida, (unsigned int)disk->index);
		disk->index = -1;
	}
}

static void disk_release(struct gendisk *disk)
{
	int held;

	BUG_ON(disk == NULL);
	if (WARN_ON(disk->magic != GENDISK_MAGIC))
		return;
	blk_might_sleep();
	/* An allocated disk that never reached disk_add() still owns its
	 * index; one that was deleted released it there.  The last reference
	 * may be dropped by a caller that holds the registry already (a
	 * partition removed during a rescan). */
	held = blk_mutex_held(&g_disk_registry_mutex);
	if (!held)
		blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->kind == BLK_DISK_SD) {
		if (disk->index >= 0 && disk->index < DISK_MAX_DISKS &&
		    g_sd_disks[disk->index] == disk)
			g_sd_disks[disk->index] = NULL;
	} else if (disk->kind == BLK_DISK_SR) {
		if (disk->index >= 0 && disk->index < DISK_MAX_SR &&
		    g_sr_disks[disk->index] == disk)
			g_sr_disks[disk->index] = NULL;
	} else {
		genhd_named_unlink_locked(disk);
	}
	if (!held)
		blk_mutex_unlock(&g_disk_registry_mutex);
	if (disk->ops && disk->ops->disk_release)
		disk->ops->disk_release(disk);
	if (disk->ops && disk->ops->free_disk)
		disk->ops->free_disk(disk);
	if (disk->queue) {
		/* The disk's reference; a blk_get_queue() holder keeps the
		 * memory a little longer. */
		blk_put_queue(disk->queue);
		disk->queue = NULL;
	}
	if (disk->ext_minor_dyn) {
		blk_ext_devt_free(disk->first_minor);
		disk->ext_minor_dyn = 0;
	}
	disk->magic = 0;
	kfree(disk);
}

void disk_put(struct gendisk *disk)
{
	if (!disk)
		return;
	VM_BUG_ON(disk->magic != GENDISK_MAGIC);
	if (refcount_dec_and_test(&disk->refcount)) {
		blk_might_sleep();
		disk_release(disk);
	}
}

void disk_put_any(struct gendisk *disk)
{
	struct blk_cpl_ctx *c;

	if (!disk)
		return;
	VM_BUG_ON(disk->magic != GENDISK_MAGIC);
	if (!refcount_dec_and_test(&disk->refcount))
		return;
	if (blk_can_release_now()) {
		disk_release(disk);
		return;
	}
	/* Inside a completion section: the completion's caller may still
	 * use the driver's objects free_disk would free; the section's end
	 * releases the disk, in this task, once the completion returned. */
	c = blk_cpl_current();
	if (c) {
		disk->next = c->disks;
		c->disks = disk;
		return;
	}
	genhd_defer_disk(disk);
}

/* ==========================================================================
 * Completion sections
 * ========================================================================== */

/* Is the caller the block softirq (not a hard interrupt nested in it)?
 * Its sections belong to the CPU, not to the task it interrupted. */
static int blk_cpl_in_softirq(void)
{
	return sched_current() != NULL && !blk_in_hardirq() &&
	       blk_in_softirq();
}

struct blk_cpl_ctx *blk_cpl_current(void)
{
	task_t *cur;

	if (blk_cpl_in_softirq())
		return g_softirq_cpl[this_cpu_id()];
	if (!blk_can_sleep())
		return NULL;
	cur = sched_current();
	return (struct blk_cpl_ctx *)cur->blk_cpl_ctx;
}

void blk_cpl_begin(struct blk_cpl_ctx *c)
{
	task_t *cur;

	c->rqs = NULL;
	c->disks = NULL;
	c->items = NULL;
	c->nparts = 0;
	c->outer = NULL;
	c->softirq = 0;
	c->collect_done = 0;
	c->done_head = c->done_tail = NULL;
	/* In the block softirq the section is the CPU's: its end may not
	 * release (the softirq never sleeps), it hands what was parked to
	 * kblockd once the completion is through with it. */
	if (blk_cpl_in_softirq()) {
		uint32_t cpu = this_cpu_id();

		c->active = 1;
		c->softirq = 1;
		c->outer = g_softirq_cpl[cpu];
		g_softirq_cpl[cpu] = c;
		return;
	}
	/* Otherwise only where the end may release (sleep): an interrupt
	 * handler's section stays inert and leaves the interrupted task's
	 * alone. */
	c->active = blk_can_sleep();
	if (!c->active)
		return;
	cur = sched_current();
	c->outer = (struct blk_cpl_ctx *)cur->blk_cpl_ctx;
	cur->blk_cpl_ctx = c;
}

void blk_cpl_collect_done(struct blk_cpl_ctx *c)
{
	if (!c->active || c->softirq) {
		WARN_ON_ONCE(1); /* only a task's section collects */
		return;
	}
	c->collect_done = 1;
}

struct blk_cpl_ctx *blk_cpl_collector(void)
{
	struct blk_cpl_ctx *c = blk_cpl_current();

	if (!c || c->softirq)
		return NULL;
	for (; c; c = c->outer)
		if (c->collect_done)
			return c;
	return NULL;
}

void blk_cpl_flush_done(struct blk_cpl_ctx *c)
{
	might_sleep();
	while (c->done_head) {
		struct blk_softirq_entry *e = c->done_head;

		c->done_head = e->next;
		if (!c->done_head)
			c->done_tail = NULL;
		e->next = NULL;
		e->fn(e);
	}
}

int blk_cpl_park(struct blk_release *r)
{
	struct blk_cpl_ctx *c = blk_cpl_current();

	if (!c)
		return 0;
	r->next = c->items;
	c->items = r;
	return 1;
}

/* Hand everything parked on `c' to `o' (an outer section). */
static void blk_cpl_splice(struct blk_cpl_ctx *o, struct blk_cpl_ctx *c)
{
	if (c->done_head) {
		if (o->done_tail)
			o->done_tail->next = c->done_head;
		else
			o->done_head = c->done_head;
		o->done_tail = c->done_tail;
		c->done_head = c->done_tail = NULL;
	}
	while (c->rqs) {
		struct blk_request *r = c->rqs;

		c->rqs = r->disp_next;
		r->disp_next = o->rqs;
		o->rqs = r;
	}
	while (c->disks) {
		struct gendisk *d = c->disks;

		c->disks = d->next;
		d->next = o->disks;
		o->disks = d;
	}
	while (c->items) {
		struct blk_release *r = c->items;

		c->items = r->next;
		r->next = o->items;
		o->items = r;
	}
	for (int i = 0; i < c->nparts; i++) {
		if (o->nparts < BLK_CPL_PARTS)
			o->parts[o->nparts++] = c->parts[i];
		else
			genhd_defer_partition_global(c->parts[i]);
	}
	c->nparts = 0;
}

/* The end of an outermost block softirq section: everything parked goes
 * to the deferred lists, which kblockd reaps (each deferral kicks it). */
static void blk_cpl_hand_to_kblockd(struct blk_cpl_ctx *c)
{
	while (c->rqs) {
		struct blk_request *r = c->rqs;

		c->rqs = r->disp_next;
		blk_request_defer_destroy(r);
	}
	for (int i = 0; i < c->nparts; i++)
		genhd_defer_partition_global(c->parts[i]);
	c->nparts = 0;
	while (c->disks) {
		struct gendisk *d = c->disks;

		c->disks = d->next;
		genhd_defer_disk(d);
	}
	while (c->items) {
		struct blk_release *r = c->items;

		c->items = r->next;
		genhd_defer_item(r);
	}
	/* Each deferral above kicked the reap; nothing deferred, nothing
	 * to wake. */
}

void blk_cpl_end(struct blk_cpl_ctx *c)
{
	task_t *cur;

	if (!c->active)
		return;
	if (c->softirq) {
		uint32_t cpu = this_cpu_id();

		WARN_ON(g_softirq_cpl[cpu] != c); /* sections must nest */
		g_softirq_cpl[cpu] = c->outer;
		c->active = 0;
		if (c->outer) {
			blk_cpl_splice(c->outer, c);
			return;
		}
		blk_cpl_hand_to_kblockd(c);
		return;
	}
	cur = sched_current();
	WARN_ON(cur->blk_cpl_ctx != c); /* sections must nest */
	/* Completions collected and not flushed yet: here, while the
	 * section still parks what they release. */
	if (c->collect_done)
		blk_cpl_flush_done(c);
	cur->blk_cpl_ctx = c->outer;
	c->active = 0;
	if (c->outer) {
		blk_cpl_splice(c->outer, c);
		return;
	}
	if (WARN_ON(!blk_can_sleep())) {
		/* Left with interrupts off: what can wait goes to the
		 * deferred lists; driver items run (they only free). */
		while (c->rqs) {
			struct blk_request *r = c->rqs;

			c->rqs = r->disp_next;
			blk_request_defer_destroy(r);
		}
		for (int i = 0; i < c->nparts; i++)
			genhd_defer_partition_global(c->parts[i]);
		while (c->disks) {
			struct gendisk *d = c->disks;

			c->disks = d->next;
			genhd_defer_disk(d);
		}
	} else {
		blk_request_destroy_list(c->rqs);
		c->rqs = NULL;
		/* Partitions first: each still holds a disk reference. */
		for (int i = 0; i < c->nparts; i++)
			blk_partition_release(c->parts[i]);
		while (c->disks) {
			struct gendisk *d = c->disks;

			c->disks = d->next;
			d->next = NULL;
			disk_release(d);
		}
	}
	c->nparts = 0;
	while (c->items) {
		struct blk_release *r = c->items;

		c->items = r->next;
		r->next = NULL;
		r->fn(r);
	}
}

/* ==========================================================================
 * Allocation and registration
 * ========================================================================== */

static void genhd_default_limits(struct blk_limits *l)
{
	mm_memset(l, 0, sizeof(*l));
	l->logical_block_size = 512;
	l->physical_block_size = 512;
	l->io_min = 512;
	l->max_hw_sectors = 256;
	l->max_sectors = 256;
	l->max_segments = 128;
	l->max_segment_size = 65536;
	l->seg_boundary_mask = 0xFFFFFFFFULL;
	l->dma_alignment = 3;
	l->queue_depth = 1;
	l->rotational = 1;
}

struct gendisk *disk_alloc(enum blk_disk_kind kind, const struct blk_ops *ops,
			   void *private_data)
{
	struct gendisk *disk;
	int idx = -1;

	if (kind != BLK_DISK_SD && kind != BLK_DISK_SR)
		return NULL;
	if (!ops || !ops->submit) {
		kprintf(BLK_LOG_PREFIX "disk_alloc: driver has no submit operation\n");
		return NULL;
	}
	blk_might_sleep();
	genhd_reap_deferred();
	disk = (struct gendisk *)kcalloc(1, sizeof(*disk));
	if (!disk)
		return NULL;
	disk->magic = GENDISK_MAGIC;
	disk->kind = kind;
	disk->ops = ops;
	disk->private_data = private_data;
	refcount_set(&disk->refcount, 1); /* the registry's */
	genhd_default_limits(&disk->limits);
	disk->host_no = -1;
	disk->scsi_type = kind == BLK_DISK_SR ? TYPE_ROM : TYPE_DISK;
	ksnprintf(disk->vendor, sizeof(disk->vendor), "ATA     ");
	ksnprintf(disk->bus, sizeof(disk->bus), "ata");
	disk->bdev.registry_index = -1;

	blk_mutex_lock(&g_disk_registry_mutex);
	if (kind == BLK_DISK_SD) {
		for (int i = 0; i < DISK_MAX_DISKS; i++) {
			if (!g_sd_disks[i]) {
				idx = i;
				g_sd_disks[i] = disk;
				break;
			}
		}
	} else {
		for (int i = 0; i < DISK_MAX_SR; i++) {
			if (!g_sr_disks[i]) {
				idx = i;
				g_sr_disks[i] = disk;
				break;
			}
		}
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	if (idx < 0) {
		kprintf(BLK_LOG_PREFIX "no free %s name: too many disks\n",
			kind == BLK_DISK_SD ? "sd" : "sr");
		kfree(disk);
		return NULL;
	}
	disk->index = idx;
	if (kind == BLK_DISK_SD) {
		disk_name_for_index(idx, disk->disk_name,
				    sizeof(disk->disk_name));
		disk_devt_for_index(idx, 0, &disk->major, &disk->first_minor);
		disk->minors = DISK_MINORS;
	} else {
		ksnprintf(disk->disk_name, sizeof(disk->disk_name), "sr%d", idx);
		disk->major = BLK_SR_MAJOR;
		disk->first_minor = (uint32_t)idx;
		disk->minors = 1;
		disk->flags |= GENHD_FL_REMOVABLE | GENHD_FL_NO_PART_SCAN;
	}
	return disk;
}

/* Is `name' taken by any disk (allocated or added) or partition?
 * Registry mutex held. */
static int genhd_name_taken_locked(const char *name)
{
	for (int i = 0; i < DISK_MAX_DISKS; i++)
		if (g_sd_disks[i] && kstrcmp(g_sd_disks[i]->disk_name, name) == 0)
			return 1;
	for (int i = 0; i < DISK_MAX_SR; i++)
		if (g_sr_disks[i] && kstrcmp(g_sr_disks[i]->disk_name, name) == 0)
			return 1;
	for (struct gendisk *d = g_named_disks; d; d = d->named_next)
		if (kstrcmp(d->disk_name, name) == 0)
			return 1;
	return genhd_find_name_locked(name) != NULL;
}

struct gendisk *disk_alloc_named(enum blk_disk_kind kind,
				 const struct blk_ops *ops, void *private_data,
				 const char *name)
{
	struct gendisk *disk, **pp;
	uint32_t minor;
	size_t len;
	int idx;

	if (kind != BLK_DISK_NVME || !ops || !name)
		return NULL;
	len = kstrlen(name);
	if (!len || len >= DISK_NAME_LEN) {
		kprintf(BLK_LOG_PREFIX "disk_alloc_named: bad name \"%s\"\n", name);
		return NULL;
	}
	blk_might_sleep();
	genhd_reap_deferred();
	disk = (struct gendisk *)kcalloc(1, sizeof(*disk));
	if (!disk)
		return NULL;
	if (blk_ext_devt_alloc(&minor)) {
		kprintf(BLK_LOG_PREFIX "%s: no device number left\n", name);
		kfree(disk);
		return NULL;
	}
	disk->magic = GENDISK_MAGIC;
	disk->kind = kind;
	disk->ops = ops;
	disk->private_data = private_data;
	refcount_set(&disk->refcount, 1); /* the registry's */
	genhd_default_limits(&disk->limits);
	/* A named disk is solid state until its driver says otherwise. */
	disk->limits.rotational = 0;
	disk->host_no = -1;
	disk->scsi_type = TYPE_DISK;
	ksnprintf(disk->vendor, sizeof(disk->vendor), "NVMe    ");
	ksnprintf(disk->bus, sizeof(disk->bus), "nvme");
	disk->bdev.registry_index = -1;
	kstrncpy(disk->disk_name, name, sizeof(disk->disk_name) - 1);
	disk->disk_name[sizeof(disk->disk_name) - 1] = 0;
	disk->major = BLK_EXT_MAJOR;
	disk->first_minor = minor;
	disk->minors = 1;
	disk->ext_minor_dyn = 1;

	blk_mutex_lock(&g_disk_registry_mutex);
	if (genhd_name_taken_locked(disk->disk_name)) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		kprintf(BLK_LOG_PREFIX "%s: name in use\n", disk->disk_name);
		blk_ext_devt_free(minor);
		kfree(disk);
		return NULL;
	}
	idx = ida_alloc(&g_named_index_ida);
	if (idx < 0) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		blk_ext_devt_free(minor);
		kfree(disk);
		return NULL;
	}
	disk->index = idx;
	/* In index order: the sysfs walks key on it. */
	for (pp = &g_named_disks; *pp && (*pp)->index < idx;
	     pp = &(*pp)->named_next)
		;
	disk->named_next = *pp;
	*pp = disk;
	blk_mutex_unlock(&g_disk_registry_mutex);
	return disk;
}

int disk_set_name(struct gendisk *disk, const char *name)
{
	size_t len;
	int rc = 0;

	if (!disk || disk->magic != GENDISK_MAGIC || !name ||
	    disk->kind != BLK_DISK_NVME)
		return -EINVAL;
	len = kstrlen(name);
	if (!len || len >= DISK_NAME_LEN)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->flags & GENHD_FL_UP) {
		rc = -EBUSY;
	} else if (kstrcmp(disk->disk_name, name) != 0) {
		/* The disk's own (old) name is on the list too: it cannot
		 * collide with the new one, which differs from it. */
		if (genhd_name_taken_locked(name)) {
			rc = -EEXIST;
		} else {
			kstrncpy(disk->disk_name, name,
				 sizeof(disk->disk_name) - 1);
			disk->disk_name[sizeof(disk->disk_name) - 1] = 0;
		}
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

int set_capacity_and_notify(struct gendisk *disk, uint64_t sectors)
{
	uint64_t old;
	int rc;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	old = disk->capacity;
	rc = disk_set_capacity_locked(disk, sectors);
	blk_mutex_unlock(&g_disk_registry_mutex);
	/* The change event of the established interface (RESIZE=1): the new
	 * diskseq tells userland, and the log says it -- only for a live disk
	 * whose size really changed, never for the first size. */
	if (!rc && old != sectors && (disk->flags & GENHD_FL_UP) && old &&
	    sectors)
		kprintf(BLK_LOG_PREFIX "%s: detected capacity change from %llu to %llu\n",
			disk->disk_name, (unsigned long long)old,
			(unsigned long long)sectors);
	return rc;
}

/* Fill the whole-disk block device from the disk and its queue. */
static void genhd_setup_bdev(struct gendisk *disk)
{
	block_device_t *b = &disk->bdev;
	const struct blk_limits *l = &disk->queue->limits;

	b->name = disk->disk_name;
	b->sector_size = l->logical_block_size;
	b->total_sectors = (unsigned long)disk->capacity;
	b->read = blk_legacy_read;
	b->write = blk_legacy_write;
	b->sync = blk_legacy_sync;
	b->release_locks_for_task = NULL;
	/* Non-NULL: the storage bootstrap skips devices without it. */
	b->driver_data = disk->private_data ? disk->private_data : disk;
	b->magic = BLOCK_DEVICE_MAGIC;
	b->flags = BDEV_F_WHOLE_DISK |
		   (disk->kind == BLK_DISK_SR ? BDEV_F_CDROM : 0);
	b->ops = disk->ops;
	b->disk = disk;
	b->part = NULL;
	b->parent = NULL;
	b->queue = disk->queue;
	b->start_lba = 0;
	b->major = disk->major;
	b->minor = disk->first_minor;
	b->physical_sector_size = l->physical_block_size;
	b->alignment_offset = l->alignment_offset;
	b->io_min = l->io_min;
	b->io_opt = l->io_opt;
	b->max_sectors = l->max_sectors;
	b->queue_depth = l->queue_depth;
	b->discard_granularity = l->discard_granularity;
	b->discard_max_sectors = l->max_discard_sectors;
	b->write_zeroes_max_sectors = l->max_write_zeroes_sectors;
	b->ro = (disk->flags & GENHD_FL_RO) ? 1 : 0;
	b->removable = (disk->flags & GENHD_FL_REMOVABLE) ? 1 : 0;
	b->rotational = l->rotational;
	b->flush_supported = l->write_cache;
	b->fua_supported = l->fua;
	b->discard_zeroes_data = l->discard_zeroes_data;
	b->ready = !(disk->flags & GENHD_FL_REMOVABLE) ||
		   (disk->flags & GENHD_FL_MEDIA_PRESENT);
	refcount_set(&b->refcount, 1); /* the registry's */
	b->registry_index = -1;
	b->open_count = 0;
	b->holder = NULL;
	b->diskseq = disk->diskseq;
}

static void genhd_log_disk(struct gendisk *disk)
{
	const struct blk_limits *l = &disk->queue->limits;
	char size[48];

	if (disk->kind == BLK_DISK_SR) {
		kprintf(BLK_LOG_PREFIX "%s: optical drive \"%s\" %s, %s\n",
			disk->disk_name, disk->model,
			disk->firmware[0] ? disk->firmware : "",
			(disk->flags & GENHD_FL_MEDIA_PRESENT) ?
				"medium present" :
				"no medium");
		if (disk->flags & GENHD_FL_MEDIA_PRESENT) {
			genhd_format_size(disk->capacity *
						  l->logical_block_size,
					  size, sizeof(size));
			kprintf(BLK_LOG_PREFIX "%s: %llu %u-byte blocks (%s)\n",
				disk->disk_name,
				(unsigned long long)disk->capacity,
				l->logical_block_size, size);
		}
		return;
	}
	genhd_format_size(disk->capacity * l->logical_block_size, size,
			  sizeof(size));
	kprintf(BLK_LOG_PREFIX "%s: %llu %u-byte logical blocks (%s)\n",
		disk->disk_name, (unsigned long long)disk->capacity,
		l->logical_block_size, size);
	if (l->physical_block_size != l->logical_block_size)
		kprintf(BLK_LOG_PREFIX "%s: %u-byte physical blocks, alignment offset %u\n",
			disk->disk_name, l->physical_block_size,
			l->alignment_offset);
	kprintf(BLK_LOG_PREFIX "%s: \"%s\" serial \"%s\" firmware \"%s\"%s%s\n",
		disk->disk_name, disk->model, disk->serial, disk->firmware,
		disk->wwn[0] ? " wwn " : "", disk->wwn);
	kprintf(BLK_LOG_PREFIX "%s: write cache: %s, FUA: %s, %s, queue depth %u, max %u sectors/request%s\n",
		disk->disk_name, l->write_cache ? "enabled" : "disabled",
		l->fua ? "supported" : "not supported",
		l->rotational ? "rotational" : "solid state", l->queue_depth,
		l->max_sectors,
		l->max_discard_sectors ? ", discard supported" : "");
	if (disk->flags & GENHD_FL_RO)
		kprintf(BLK_LOG_PREFIX "%s: write protected\n",
			disk->disk_name);
}

void genhd_refresh_identity(block_device_t *bdev)
{
	struct blk_fs_identity id;
	int rc;

	if (!bdev || !blk_bdev_is_extended(bdev))
		return;
	blk_might_sleep();
	/* The probe reads the device: never under the registry mutex (the
	 * driver may hold its error-handling mutex around us and need the
	 * registry from its error handler). */
	mm_memset(&id, 0, sizeof(id));
	if (bdev->ready && bdev->total_sectors &&
	    !(bdev->flags & BDEV_F_DEAD)) {
		rc = blk_probe_fs_identity(bdev, &id);
		if (rc && rc != -ENOENT)
			kprintf(BLK_LOG_PREFIX "%s: filesystem probe failed (%d)\n",
				bdev->name, rc);
	}
	blk_mutex_lock(&g_disk_registry_mutex);
	if (!(bdev->flags & BDEV_F_DEAD) && bdev->disk &&
	    !(bdev->disk->flags & GENHD_FL_DEAD)) {
		if (bdev->part)
			bdev->part->fs_id = id;
		else
			bdev->disk->fs_id = id;
		if (id.valid)
			kprintf(BLK_LOG_PREFIX "%s: %s filesystem%s%s%s%s\n",
				bdev->name, id.type, id.uuid[0] ? " UUID " : "",
				id.uuid, id.label[0] ? " LABEL " : "", id.label);
		if (!(bdev->disk->flags & GENHD_FL_HIDDEN) &&
		    (bdev->flags & BDEV_F_NODE))
			(void)blkdev_links_update_locked(bdev);
	}
	blk_mutex_unlock(&g_disk_registry_mutex);
}

void blk_devevent_locked(block_device_t *bdev, uint32_t type)
{
	struct gendisk *disk = bdev ? bdev->disk : NULL;
	uint32_t flags = 0;
	uint64_t id, parent = 0;

	if (!disk || (disk->flags & GENHD_FL_HIDDEN))
		return;
	/* Only a device that is (still) there is announced or changed: an
	 * ADD that lost the race with a deletion stays unpublished, so no
	 * ADD ever follows the device's REMOVE. */
	if (type != DEVICE_EVENT_REMOVE &&
	    ((bdev->flags & BDEV_F_DEAD) ||
	     (disk->flags & (GENHD_FL_DEAD | GENHD_FL_UP)) != GENHD_FL_UP))
		return;
	if (disk->flags & GENHD_FL_REMOVABLE)
		flags |= DEVICE_EVENT_F_REMOVABLE;
	if (disk->flags & GENHD_FL_HOTPLUGGED)
		flags |= DEVICE_EVENT_F_HOTPLUGGED;
	if (disk->bus[0] == 'u' && disk->bus[1] == 's' && disk->bus[2] == 'b' &&
	    disk->bus[3] == '\0')
		flags |= DEVICE_EVENT_F_USB;
	id = ((uint64_t)bdev->major << 32) | bdev->minor;
	if (bdev->part)
		parent = ((uint64_t)disk->bdev.major << 32) | disk->bdev.minor;
	devevent_publish(type,
			 bdev->part ? DEVICE_CLASS_BLOCK_PARTITION :
				      DEVICE_CLASS_BLOCK_DISK,
			 id, parent, flags, 0, 0, bdev->major, bdev->minor,
			 bdev->name);
}

void blk_devevent(block_device_t *bdev, uint32_t type)
{
	if (!bdev)
		return;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	blk_devevent_locked(bdev, type);
	blk_mutex_unlock(&g_disk_registry_mutex);
}

int disk_add(struct gendisk *disk)
{
	int rc;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	blk_might_sleep();
	if (disk->flags & (GENHD_FL_UP | GENHD_FL_DEAD))
		return -EBUSY;
	/* An NVMe disk may be empty: a namespace in a format the host
	 * cannot do block I/O on stays registered with capacity 0 (its
	 * pass-through node still works) and grows on a revalidation.  The
	 * partition and identity probes below skip an empty disk. */
	if (!disk->capacity && !(disk->flags & GENHD_FL_REMOVABLE) &&
	    disk->kind != BLK_DISK_NVME) {
		kprintf(BLK_LOG_PREFIX "%s: refusing a disk of capacity 0\n",
			disk->disk_name);
		return -EINVAL;
	}
	if (disk->kind == BLK_DISK_SR)
		disk->flags |= GENHD_FL_NO_PART_SCAN;

	if (disk->tag_set) {
		/* A multi-queue disk: its queue exists since
		 * blk_mq_alloc_disk(), with the limits the driver gave there
		 * or committed since (queue_limits_commit_update). */
		if (!disk->queue)
			return -EINVAL;
	} else {
		disk->queue = blk_queue_create(disk, &disk->limits);
		if (!disk->queue)
			return -ENOMEM;
	}
	/* The driver's limits after normalisation are what everybody sees. */
	disk->limits = disk->queue->limits;
	disk->diskseq = genhd_next_diskseq();
	genhd_setup_bdev(disk);

	blk_mutex_lock(&g_disk_registry_mutex);
	genhd_log_disk(disk);
	if (block_register(&disk->bdev) != ST_OK) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		kprintf(BLK_LOG_PREFIX "%s: could not be registered\n",
			disk->disk_name);
		if (!disk->tag_set)
			blk_queue_destroy(disk->queue);
		return -ENOSPC;
	}
	disk->flags |= GENHD_FL_UP;
	disk->add_ns = hrtimer_now_ns();
	if (sched_current())
		disk->flags |= GENHD_FL_HOTPLUGGED;
	__atomic_add_fetch(&g_disks_up, 1, __ATOMIC_RELAXED);

	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		rc = blk_sysfs_add_disk_locked(disk);
		if (rc)
			kprintf(BLK_LOG_PREFIX "%s: sysfs entries failed (%d)\n",
				disk->disk_name, rc);
		rc = blkdev_node_add_locked(&disk->bdev);
		if (rc)
			kprintf(BLK_LOG_PREFIX "%s: /dev node failed (%d)\n",
				disk->disk_name, rc);
	}
	/* A reference for the I/O below: a hot removal racing with the scan
	 * must not free the disk under it. */
	disk_get(disk);
	blk_mutex_unlock(&g_disk_registry_mutex);

	/* Partitions, unless the disk is not partitionable or empty.  The
	 * table is read without the registry mutex. */
	if (!(disk->flags & GENHD_FL_NO_PART_SCAN) && disk->capacity &&
	    disk->bdev.ready) {
		rc = blk_partition_scan(disk);
		if (rc)
			kprintf(BLK_LOG_PREFIX "%s: partition table could not be read (%d)\n",
				disk->disk_name, rc);
	}
	/* The whole disk's own identity (a disk formatted without a table,
	 * an ISO image) and its /dev/disk links. */
	genhd_refresh_identity(&disk->bdev);
	blk_devevent(&disk->bdev, DEVICE_EVENT_ADD);
	disk_put(disk);
	return 0;
}

void disk_del(struct gendisk *disk)
{
	if (!disk || disk->magic != GENDISK_MAGIC)
		return;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (WARN(disk->flags & GENHD_FL_DEAD, "%s deleted twice",
		 disk->disk_name)) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return;
	}
	if (!(disk->flags & GENHD_FL_UP)) {
		/* Allocated but never added: just give up the index. */
		disk->flags |= GENHD_FL_DEAD;
		blk_mutex_unlock(&g_disk_registry_mutex);
		disk_put(disk);
		return;
	}
	kprintf(BLK_LOG_PREFIX "%s: removing disk\n", disk->disk_name);
	/* New requests fail from here on; mark the partitions too. */
	disk->flags |= GENHD_FL_DEAD;
	disk->bdev.flags |= BDEV_F_DEAD;
	disk->bdev.ready = 0;
	for (int i = 1; i <= DISK_MAX_PARTS; i++) {
		struct blk_partition *p = disk->part[i];
		if (p) {
			p->bdev.flags |= BDEV_F_DEAD;
			p->bdev.ready = 0;
		}
	}
	if (disk->bdev.holder || disk->bdev.open_count ||
	    blk_partition_busy_locked(disk, NULL))
		kprintf(BLK_LOG_PREFIX "%s: removed while in use; its users get -ENODEV\n",
			disk->disk_name);

	(void)blk_partition_remove_all_locked(disk, 1);
	blk_devevent_locked(&disk->bdev, DEVICE_EVENT_REMOVE);
	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		blkdev_node_remove_locked(&disk->bdev);
		blk_sysfs_remove_disk_locked(disk);
	}
	block_unregister(&disk->bdev);
	/* The name may be reused now. */
	if (disk->kind == BLK_DISK_SD) {
		if (g_sd_disks[disk->index] == disk)
			g_sd_disks[disk->index] = NULL;
	} else if (disk->kind == BLK_DISK_SR) {
		if (g_sr_disks[disk->index] == disk)
			g_sr_disks[disk->index] = NULL;
	} else {
		genhd_named_unlink_locked(disk);
	}
	disk->flags &= ~GENHD_FL_UP;
	__atomic_sub_fetch(&g_disks_up, 1, __ATOMIC_RELAXED);
	blk_mutex_unlock(&g_disk_registry_mutex);
	/* Fails what is queued (their end_io runs) and waits for what the
	 * driver holds -- both without the registry mutex.  A concurrent
	 * shutdown or suspend walker holds its own disk reference. */
	blk_queue_destroy(disk->queue);

	/* The registry's reference. */
	(void)refcount_dec_and_test(&disk->bdev.refcount);
	disk_put(disk);
}

/* ==========================================================================
 * Capacity, write protection, media
 * ========================================================================== */

int disk_set_capacity_locked(struct gendisk *disk, uint64_t sectors)
{
	uint64_t old;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	old = disk->capacity;
	if (old == sectors)
		return 0;
	disk->capacity = sectors;
	disk->bdev.total_sectors = (unsigned long)sectors;
	disk->diskseq = genhd_next_diskseq();
	disk->bdev.diskseq = disk->diskseq;
	for (int i = 1; i <= DISK_MAX_PARTS; i++) {
		struct blk_partition *p = disk->part[i];
		if (!p)
			continue;
		p->bdev.diskseq = disk->diskseq;
		/* A partition that no longer fits is clipped; its contents
		 * past the end are unreachable now. */
		if (p->start_lba >= sectors) {
			p->bdev.total_sectors = 0;
			p->bdev.ready = 0;
		} else if (p->start_lba + p->nr_sectors > sectors) {
			p->bdev.total_sectors =
				(unsigned long)(sectors - p->start_lba);
		}
	}
	if (disk->flags & GENHD_FL_UP)
		kprintf(BLK_LOG_PREFIX "%s: capacity changed from %llu to %llu sectors\n",
			disk->disk_name, (unsigned long long)old,
			(unsigned long long)sectors);
	return 0;
}

int disk_set_capacity(struct gendisk *disk, uint64_t sectors)
{
	int rc;

	if (!disk)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	rc = disk_set_capacity_locked(disk, sectors);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

void disk_set_ro(struct gendisk *disk, int ro)
{
	if (!disk || disk->magic != GENDISK_MAGIC)
		return;
	if (!!(disk->flags & GENHD_FL_RO) == !!ro)
		return;
	if (ro)
		disk->flags |= GENHD_FL_RO;
	else
		disk->flags &= ~GENHD_FL_RO;
	disk->bdev.ro = ro ? 1 : 0;
	if (disk->flags & GENHD_FL_UP)
		kprintf(BLK_LOG_PREFIX "%s: write protect is %s\n",
			disk->disk_name, ro ? "on" : "off");
}

void disk_media_changed(struct gendisk *disk, int present)
{
	if (!disk || disk->magic != GENDISK_MAGIC)
		return;
	blk_might_sleep();
	/* Re-reading the capacity is I/O: done before the registry mutex,
	 * and the driver may call disk_set_capacity() from it. */
	if (present && disk->ops && disk->ops->revalidate) {
		int rc = disk->ops->revalidate(disk);
		if (rc) {
			kprintf(BLK_LOG_PREFIX "%s: medium could not be read (%d)\n",
				disk->disk_name, rc);
			present = 0;
		}
	}
	blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->flags & GENHD_FL_DEAD) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return;
	}
	if (present) {
		disk->flags |= GENHD_FL_MEDIA_PRESENT;
		disk->bdev.ready = 1;
		disk->bdev.total_sectors = (unsigned long)disk->capacity;
	} else {
		disk->flags &= ~GENHD_FL_MEDIA_PRESENT;
		disk->bdev.ready = 0;
	}
	disk->diskseq = genhd_next_diskseq();
	disk->bdev.diskseq = disk->diskseq;
	if (!(disk->flags & GENHD_FL_UP)) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return;
	}
	kprintf(BLK_LOG_PREFIX "%s: medium %s\n", disk->disk_name,
		present ? "inserted" : "removed");
	/* The old medium's partitions go in any case. */
	(void)blk_partition_remove_all_locked(disk, 1);
	disk->pt_type = BLK_PT_NONE;
	disk_get(disk);
	blk_mutex_unlock(&g_disk_registry_mutex);
	/* Reading the new medium: without the registry mutex. */
	if (present && !(disk->flags & GENHD_FL_NO_PART_SCAN))
		(void)blk_partition_scan(disk);
	genhd_refresh_identity(&disk->bdev);
	blk_devevent(&disk->bdev, DEVICE_EVENT_CHANGE);
	disk_put(disk);
}

/* ==========================================================================
 * Lookups
 * ========================================================================== */

block_device_t *blk_lookup_name(const char *name)
{
	block_device_t *b;

	if (!name)
		return NULL;
	blk_might_sleep();
	/* "/dev/sda1" works as well as "sda1". */
	if (kstrncmp(name, "/dev/", 5) == 0)
		name += 5;
	blk_mutex_lock(&g_disk_registry_mutex);
	b = genhd_find_name_locked(name);
	if (b)
		blk_bdev_get(b);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return b;
}

block_device_t *blk_lookup_devt(uint32_t major, uint32_t minor)
{
	block_device_t *b;

	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	b = genhd_find_devt_locked(major, minor);
	if (b)
		blk_bdev_get(b);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return b;
}

static char genhd_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int genhd_strcaseeq(const char *a, const char *b)
{
	while (*a && *b) {
		if (genhd_lower(*a) != genhd_lower(*b))
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

block_device_t *blk_lookup_partuuid(const char *partuuid)
{
	block_device_t *found = NULL;

	if (!partuuid || !*partuuid)
		return NULL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d && !found;
	     d = genhd_next_locked(d)) {
		for (int i = 1; i <= DISK_MAX_PARTS && !found; i++) {
			struct blk_partition *p = d->part[i];
			if (p && p->partuuid[0] &&
			    genhd_strcaseeq(p->partuuid, partuuid))
				found = &p->bdev;
		}
	}
	if (found)
		blk_bdev_get(found);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return found;
}

block_device_t *blk_lookup_parttype(struct gendisk *disk,
				    const struct blk_guid *type, int nth)
{
	block_device_t *found = NULL;

	if (!disk || !type || nth < 0)
		return NULL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (genhd_disk_alive_locked(disk)) {
		for (int i = 1; i <= DISK_MAX_PARTS && !found; i++) {
			struct blk_partition *p = disk->part[i];
			if (p && blk_guid_equal(&p->type_guid, type) &&
			    nth-- == 0)
				found = &p->bdev;
		}
	}
	if (found)
		blk_bdev_get(found);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return found;
}

/* Walk every device's probed identity; `uuid' compares case-insensitively,
 * `label' exactly. */
static block_device_t *genhd_lookup_fs(const char *key, int by_uuid)
{
	block_device_t *found = NULL;

	if (!key || !*key)
		return NULL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	for (struct gendisk *d = genhd_first_locked(); d && !found;
	     d = genhd_next_locked(d)) {
		const struct blk_fs_identity *id;

		/* Partitions first: a table's whole disk rarely holds the
		 * filesystem asked for. */
		for (int i = 1; i <= DISK_MAX_PARTS && !found; i++) {
			struct blk_partition *p = d->part[i];
			if (!p || !p->fs_id.valid)
				continue;
			id = &p->fs_id;
			if (by_uuid ? (id->uuid[0] &&
				       genhd_strcaseeq(id->uuid, key)) :
				      (id->label[0] &&
				       kstrcmp(id->label, key) == 0))
				found = &p->bdev;
		}
		if (found)
			break;
		id = &d->fs_id;
		if (id->valid &&
		    (by_uuid ? (id->uuid[0] && genhd_strcaseeq(id->uuid, key)) :
			       (id->label[0] && kstrcmp(id->label, key) == 0)))
			found = &d->bdev;
	}
	if (found)
		blk_bdev_get(found);
	blk_mutex_unlock(&g_disk_registry_mutex);
	return found;
}

block_device_t *blk_lookup_fs_uuid(const char *uuid)
{
	return genhd_lookup_fs(uuid, 1);
}

block_device_t *blk_lookup_fs_label(const char *label)
{
	return genhd_lookup_fs(label, 0);
}
