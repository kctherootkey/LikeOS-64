/* LikeOS - exFAT filesystem driver
 *
 * Byte helpers, checksums, locks, device I/O, error policy, timestamps.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/exfat/exfat.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/mount.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/exfat/exfat_int.h>

/* ===========================================================================
 * 2. Byte helpers and checksums
 * =========================================================================== */

unsigned exfat_ilog2(unsigned long v)
{
	unsigned s = 0;

	while (v > 1) {
		v >>= 1;
		s++;
	}
	return s;
}

unsigned long exfat_strlen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

int exfat_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* Boot checksum (specification 3.4): a rotating 32-bit sum over the first
 * eleven sectors of a boot region, skipping VolumeFlags and PercentInUse of
 * the boot sector, which may change without the checksum changing. */
uint32_t exfat_boot_checksum(const uint8_t *region, unsigned long ss)
{
	uint32_t sum = 0;
	unsigned long n = ss * EXFAT_BOOT_CHECKSUM_SECTOR;

	for (unsigned long i = 0; i < n; i++) {
		if (i == EXFAT_BS_FLAGS || i == EXFAT_BS_FLAGS + 1 ||
		    i == EXFAT_BS_PERCENT)
			continue;
		sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) +
		      (uint32_t)region[i];
	}
	return sum;
}

/* Entry-set checksum (specification 6.3.3): a rotating 16-bit sum over all
 * entries of the set, skipping the SetChecksum field itself. */
uint16_t exfat_set_checksum(const uint8_t *set, unsigned entries)
{
	uint16_t sum = 0;
	unsigned n = entries * EXFAT_DENTRY_SIZE;

	for (unsigned i = 0; i < n; i++) {
		if (i == EXFAT_DE_SET_CHECKSUM || i == EXFAT_DE_SET_CHECKSUM + 1)
			continue;
		sum = (uint16_t)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) +
				 (uint16_t)set[i]);
	}
	return sum;
}

/* NameHash (specification 7.6.4): a rotating 16-bit sum over the bytes of
 * the UPCASED name, low byte first. */
uint16_t exfat_name_hash(const uint16_t *up, unsigned len)
{
	uint16_t h = 0;

	for (unsigned i = 0; i < len; i++) {
		uint16_t c = up[i];

		h = (uint16_t)(((h & 1) ? 0x8000u : 0) + (h >> 1) + (c & 0xFF));
		h = (uint16_t)(((h & 1) ? 0x8000u : 0) + (h >> 1) + (c >> 8));
	}
	return h;
}

/* Upcase table checksum (specification 7.2.2): rotating 32-bit sum over
 * the table's bytes as stored. */
uint32_t exfat_table_checksum(uint32_t sum, const uint8_t *p,
			      unsigned long n)
{
	for (unsigned long i = 0; i < n; i++)
		sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) +
		      (uint32_t)p[i];
	return sum;
}

/* ===========================================================================
 * 3. The lock engine
 *
 * The hierarchy (exfat.h, section 4):
 *
 *   per-stream rwsem (hashed by handle)  ->  fs->meta  ->  fs->alloc
 *       ->  fs->st_lock / fs->mc_lock / cache spinlocks  ->  device
 *
 * fs->meta is SHARED for everything that only reads metadata -- path walks,
 * stat, readdir, the page cache's block mapping (lock_map) -- and EXCLUSIVE
 * for every mutation, the commit and page-cache writeback (lock_io).  Its
 * exclusive owner may take it again, exclusive or shared (writeback nested
 * in write() through pagecache_balance_dirty, metadata pages read under a
 * mutation).  The per-stream locks fence file data: readers hold them
 * SHARED across their page reads, which run with only the page cache's own
 * short mapping holds; write, truncate, unlink and rename-over hold them
 * EXCLUSIVE, so data blocks are never freed or rewritten under a reader.
 *
 * Fairness: a waiting writer holds back new readers (so a stream of readers
 * cannot starve mutations) -- except a task that already holds some
 * filesystem lock shared, whose nested shared request must not queue behind
 * a writer that waits for that very hold.  When an exclusive hold ends with
 * readers waiting, that cohort goes first, then the queued writers: neither
 * side can be starved by a stream of the other.
 * =========================================================================== */

void exfat_rwsem_init(exfat_rwsem_t *s, const char *name)
{
	s->readers = 0;
	s->writer = 0;
	s->owner = (uint64_t)-1;
	s->wdepth = 0;
	s->w_wait = 0;
	s->r_wait = 0;
	s->reader_turn = 0;
	s->locked = 0;
	s->untracked = 0;
	for (unsigned i = 0; i < EXFAT_RWSEM_TRACK; i++) {
		s->rh[i].task = 0;
		s->rh[i].count = 0;
	}
	s->name = name;
	spinlock_init(&s->lock, name);
}

/* Record one more shared hold of `cur' (the rwsem's spinlock held). */
static void exfat_rhold_add(exfat_rwsem_t *s, task_t *cur)
{
	int free_slot = -1;

	if (!cur) {
		s->untracked++;
		return;
	}
	for (int i = 0; i < EXFAT_RWSEM_TRACK; i++) {
		if (s->rh[i].count && s->rh[i].task == (uint64_t)cur->id) {
			s->rh[i].count++;
			return;
		}
		if (!s->rh[i].count && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		s->untracked++;
		return;
	}
	s->rh[free_slot].task = (uint64_t)cur->id;
	s->rh[free_slot].count = 1;
}

/* Forget one shared hold of `cur' (the rwsem's spinlock held). */
static void exfat_rhold_del(exfat_rwsem_t *s, task_t *cur)
{
	if (cur)
		for (int i = 0; i < EXFAT_RWSEM_TRACK; i++)
			if (s->rh[i].count &&
			    s->rh[i].task == (uint64_t)cur->id) {
				s->rh[i].count--;
				return;
			}
	if (!WARN_ON(s->untracked <= 0))
		s->untracked--;
}

/* Sleep on the semaphore's channel; the spinlock is dropped on the way.
 * The wake side (sched_wake_channel) claims BLOCKED -> READY atomically and
 * the scheduler handles a wake that comes before the schedule. */
static void exfat_rwsem_park(exfat_rwsem_t *s, uint64_t *flags)
{
	task_t *cur = sched_current();

	if (cur) {
		cur->wait_channel = (void *)s;
		task_set_blocked(cur);
	}
	spin_unlock_irqrestore(&s->lock, *flags);
	sched_schedule();
}

void exfat_rwsem_read_lock(exfat_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	int queued = 0;

	might_sleep();
	for (;;) {
		uint64_t flags;

		spin_lock_irqsave(&s->lock, &flags);
		if (s->writer && s->owner == me && cur) {
			/* Shared under our own exclusive hold: recursion. */
			s->wdepth++;
			cur->fs_held++;
			spin_unlock_irqrestore(&s->lock, flags);
			return;
		}
		int defer = s->w_wait && !s->reader_turn &&
			    !(cur && cur->fs_rdepth);
		if (!s->writer && !defer) {
			s->readers++;
			s->locked++;
			exfat_rhold_add(s, cur);
			if (cur) {
				cur->fs_rdepth++;
				cur->fs_held++;
			}
			if (queued) {
				s->r_wait--;
				if (s->r_wait == 0)
					s->reader_turn = 0;
			}
			spin_unlock_irqrestore(&s->lock, flags);
			return;
		}
		if (!queued) {
			s->r_wait++;
			queued = 1;
		}
		exfat_rwsem_park(s, &flags);
	}
}

void exfat_rwsem_read_unlock(exfat_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	uint64_t flags;
	int wake;

	spin_lock_irqsave(&s->lock, &flags);
	if (s->writer && s->owner == me && cur) {
		/* The matching release of a shared-under-exclusive hold. */
		WARN_ON(s->wdepth <= 1);
		if (s->wdepth > 1)
			s->wdepth--;
		if (cur->fs_held > 0)
			cur->fs_held--;
		spin_unlock_irqrestore(&s->lock, flags);
		return;
	}
	WARN_ON(s->readers <= 0);
	if (s->readers > 0) {
		s->readers--;
		s->locked--;
		exfat_rhold_del(s, cur);
	}
	if (cur && cur->fs_rdepth > 0)
		cur->fs_rdepth--;
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	wake = (s->readers == 0);
	spin_unlock_irqrestore(&s->lock, flags);
	if (wake)
		sched_wake_channel((void *)s);
}

void exfat_rwsem_write_lock(exfat_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	int queued = 0;

	might_sleep();
	for (;;) {
		uint64_t flags;

		spin_lock_irqsave(&s->lock, &flags);
		if (s->writer && s->owner == me) {
			s->wdepth++;
			if (queued)
				s->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&s->lock, flags);
			return;
		}
		int reader_phase = s->reader_turn && s->r_wait > 0;
		if (!s->writer && s->readers == 0 && !reader_phase) {
			s->writer = 1;
			s->owner = me;
			s->wdepth = 1;
			s->locked++;
			if (queued)
				s->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&s->lock, flags);
			return;
		}
		if (!queued) {
			s->w_wait++;
			queued = 1;
		}
		exfat_rwsem_park(s, &flags);
	}
}

void exfat_rwsem_write_unlock(exfat_rwsem_t *s)
{
	task_t *cur = sched_current();
	uint64_t flags;

	spin_lock_irqsave(&s->lock, &flags);
	WARN_ON(!s->writer || s->wdepth <= 0);
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	if (s->wdepth > 1) {
		s->wdepth--;
		spin_unlock_irqrestore(&s->lock, flags);
		return;
	}
	s->writer = 0;
	s->owner = (uint64_t)-1;
	s->wdepth = 0;
	if (s->locked > 0)
		s->locked--;
	if (s->r_wait > 0)
		s->reader_turn = 1;
	spin_unlock_irqrestore(&s->lock, flags);
	sched_wake_channel((void *)s);
}

/* Does the current task hold `s' exclusive? */
int exfat_rwsem_held_excl(exfat_rwsem_t *s)
{
	return s->writer && s->owner == exfat_task_id();
}

/* A task that died inside the driver: give back its exclusive hold AND
 * every shared hold recorded for it.  (User memory is never touched with a
 * filesystem lock held -- reads copy out of held pages after unlocking,
 * writes copy into a kernel buffer before locking, readdir assembles its
 * records in a kernel buffer -- so a fault in kernel mode cannot strand a
 * hold in the first place; this is the second line.) */
int exfat_rwsem_release_owner(exfat_rwsem_t *s, uint64_t task_id)
{
	uint64_t flags;
	int r = 0;

	spin_lock_irqsave(&s->lock, &flags);
	for (int i = 0; i < EXFAT_RWSEM_TRACK; i++) {
		if (!s->rh[i].count || s->rh[i].task != task_id)
			continue;
		WARN_ON(s->readers < s->rh[i].count);
		s->readers -= s->rh[i].count;
		s->locked -= s->rh[i].count;
		if (s->readers < 0)
			s->readers = 0;
		if (s->locked < 0)
			s->locked = 0;
		s->rh[i].count = 0;
		r = 1;
	}
	if (s->writer && s->owner == task_id) {
		s->writer = 0;
		s->owner = (uint64_t)-1;
		s->wdepth = 0;
		if (s->locked > 0)
			s->locked--;
		r = 1;
	}
	if (r && s->r_wait > 0)
		s->reader_turn = 1;
	spin_unlock_irqrestore(&s->lock, flags);
	if (r)
		sched_wake_channel((void *)s);
	return r;
}

/* ---- the instance's locks -------------------------------------------------- */

static inline exfat_rwsem_t *exfat_slock_of(exfat_fs_t *fs, uint32_t handle)
{
	return &fs->slock[handle % EXFAT_SLOCKS];
}

void exfat_slock_shared(exfat_fs_t *fs, uint32_t h)
{
	exfat_rwsem_read_lock(exfat_slock_of(fs, h));
}

void exfat_sunlock_shared(exfat_fs_t *fs, uint32_t h)
{
	exfat_rwsem_read_unlock(exfat_slock_of(fs, h));
}

void exfat_slock_excl(exfat_fs_t *fs, uint32_t h)
{
	exfat_rwsem_write_lock(exfat_slock_of(fs, h));
}

void exfat_sunlock_excl(exfat_fs_t *fs, uint32_t h)
{
	exfat_rwsem_write_unlock(exfat_slock_of(fs, h));
}

/* ===========================================================================
 * 4. Device I/O
 *
 * Reads go to the real device, bounded by the device and chunked.  Writes
 * and discards have exactly ONE way to the device each: exfat_dev_write()
 * and exfat_dev_discard().  They refuse unless the mount is armed
 * read-write and not latched read-only, and unless the sectors lie inside
 * the region the caller's kind of write may touch.  The page cache never
 * sees the real device: it gets fs->guard, whose write is the chokepoint.
 * =========================================================================== */

int exfat_dev_read(exfat_fs_t *fs, uint64_t lba, uint64_t count,
		   void *buf)
{
	block_device_t *b = fs->bdev;
	uint64_t end, max_chunk;
	uint8_t *p = (uint8_t *)buf;

	might_sleep();
	if (!b || !b->read || !buf)
		return ST_IO;
	if (count == 0)
		return ST_OK;
	if (!exfat_add_ok(lba, count, &end) || end > b->total_sectors) {
		WARN_RATELIMIT(1,
			       "exfat: read of sectors %llu+%llu beyond the device (%lu sectors)",
			       (unsigned long long)lba,
			       (unsigned long long)count, b->total_sectors);
		return ST_IO;
	}
	max_chunk = EXFAT_MAX_IO_BYTES >> fs->dev_shift;
	if (max_chunk == 0)
		max_chunk = 1;
	while (count) {
		uint64_t n = count < max_chunk ? count : max_chunk;
		int r = b->read(b, (unsigned long)lba, (unsigned long)n, p);

		if (r != ST_OK)
			return r < 0 ? r : ST_IO;
		fs->st_dev_reads++;
		lba += n;
		count -= n;
		p += n << fs->dev_shift;
	}
	return ST_OK;
}

/* Sectors of the main boot sector on the device (at least one). */
uint64_t exfat_boot_dev_sectors(exfat_fs_t *fs)
{
	uint64_t n = (1ULL << fs->fs_shift) >> fs->dev_shift;

	return n ? n : 1;
}

/* THE write chokepoint.  Every byte this driver ever stores goes through
 * here -- its own metadata writes and, through the guard device, the page
 * cache's writeback of file and directory data. */
int exfat_dev_write(exfat_fs_t *fs, uint64_t lba, uint64_t count,
		    const void *buf, int kind)
{
	block_device_t *b = fs->bdev;
	uint64_t end, lo, hi, max_chunk;
	const uint8_t *p = (const uint8_t *)buf;

	might_sleep();
	if (!__atomic_load_n(&fs->armed, __ATOMIC_ACQUIRE) && fs->may_write &&
	    !fs->ro && !fs->ro_latched && kind != EXFAT_W_BOOT &&
	    exfat_rwsem_held_excl(&fs->meta)) {
		/* The first real write of a mutation: VolumeDirty goes to the
		 * device (and is synced) before anything else does. */
		int ar = exfat_arm_now(fs);

		if (ar != ST_OK) {
			fs->io_errors++;
			return ar;
		}
	}
	if (!__atomic_load_n(&fs->armed, __ATOMIC_ACQUIRE) || fs->ro ||
	    fs->ro_latched) {
		fs->io_errors++;
		/* After errors=remount-ro the refusal is the policy working;
		 * anything else is a write nobody was entitled to make. */
		if (fs->ro_latched) {
			if (fs->refused_logged < 5) {
				fs->refused_logged++;
				kprintf("exfat: %s: write of sector %llu refused: file system is read-only after errors%s\n",
					b ? b->name : "?",
					(unsigned long long)lba,
					fs->refused_logged == 5 ?
						" (further refusals not logged)" :
						"");
			}
		}
		else
			WARN_RATELIMIT(1,
				       "exfat: write of sectors %llu+%llu refused: mount not armed read-write",
				       (unsigned long long)lba,
				       (unsigned long long)count);
		return ST_ROFS;
	}
	if (!b || !b->write || !buf) {
		fs->io_errors++;
		return ST_IO;
	}
	if (count == 0)
		return ST_OK;
	switch (kind) {
	case EXFAT_W_BOOT:
		lo = 0;
		hi = exfat_boot_dev_sectors(fs);
		break;
	case EXFAT_W_META:
		lo = fs->fat_lba;
		hi = fs->vol_dev_sectors;
		break;
	case EXFAT_W_HEAP:
		lo = fs->heap_lba;
		hi = fs->vol_dev_sectors;
		break;
	default:
		WARN_ON_ONCE(1);
		return ST_INVALID;
	}
	if (hi > b->total_sectors)
		hi = b->total_sectors;
	if (!exfat_add_ok(lba, count, &end) || lba < lo || end > hi) {
		WARN_RATELIMIT(1,
			       "exfat: write of sectors %llu+%llu outside the allowed range %llu..%llu (kind %d) refused",
			       (unsigned long long)lba,
			       (unsigned long long)count,
			       (unsigned long long)lo, (unsigned long long)hi,
			       kind);
		return ST_INVALID;
	}
	max_chunk = EXFAT_MAX_IO_BYTES >> fs->dev_shift;
	if (max_chunk == 0)
		max_chunk = 1;
	while (count) {
		uint64_t n = count < max_chunk ? count : max_chunk;
		int r = b->write(b, (unsigned long)lba, (unsigned long)n, p);

		if (r != ST_OK) {
			fs->io_errors++;
			return r < 0 ? r : ST_IO;
		}
		fs->st_dev_writes++;
		fs->barrier_needed = 1;
		lba += n;
		count -= n;
		p += n << fs->dev_shift;
	}
	return ST_OK;
}

/* THE discard chokepoint: cluster-heap sectors only, only when armed, only
 * with the `discard' option.  The caller has made sure the cleared bitmap
 * bits are on the device first (section 17). */
int exfat_dev_discard(exfat_fs_t *fs, uint64_t lba, uint64_t count)
{
	block_device_t *b = fs->bdev;
	uint64_t end;
	int r;

	might_sleep();
	if (!fs->opts.discard)
		return ST_OK;
	if (!__atomic_load_n(&fs->armed, __ATOMIC_ACQUIRE) || fs->ro ||
	    fs->ro_latched) {
		if (!fs->ro_latched)
			WARN_RATELIMIT(1,
				       "exfat: discard of sectors %llu+%llu refused: mount not armed",
				       (unsigned long long)lba,
				       (unsigned long long)count);
		return ST_ROFS;
	}
	if (!b || count == 0)
		return ST_OK;
	if (!exfat_add_ok(lba, count, &end) || lba < fs->heap_lba ||
	    end > fs->vol_dev_sectors || end > b->total_sectors) {
		WARN_RATELIMIT(1,
			       "exfat: discard of sectors %llu+%llu outside the cluster heap refused",
			       (unsigned long long)lba,
			       (unsigned long long)count);
		return ST_INVALID;
	}
	if (!blk_bdev_is_extended(b) || !b->discard_max_sectors)
		return ST_OK; /* the device has no discard: nothing to do */
	r = blk_discard_sync(b, lba, count, 0);
	if (r == 0) {
		fs->st_discards++;
		return ST_OK;
	}
	/* A refused discard loses nothing: the clusters are free either
	 * way.  Quietly stop asking a device that does not support it. */
	return ST_OK;
}

int exfat_dev_sync(exfat_fs_t *fs)
{
	block_device_t *b = fs->bdev;

	might_sleep();
	if (!b || !b->sync) {
		fs->barrier_needed = 0;
		return ST_OK;
	}
	if (b->sync(b) != ST_OK)
		return ST_IO;
	fs->barrier_needed = 0;
	return ST_OK;
}

/* ---- the guard device the page cache writes through ---------------------- */

static exfat_fs_t *exfat_fs_of_guard(block_device_t *dev)
{
	return (exfat_fs_t *)((char *)dev - __builtin_offsetof(exfat_fs_t, guard));
}

static int exfat_guard_read(block_device_t *dev, unsigned long lba,
			    unsigned long count, void *buf)
{
	return exfat_dev_read(exfat_fs_of_guard(dev), lba, count, buf);
}

static int exfat_guard_write(block_device_t *dev, unsigned long lba,
			     unsigned long count, const void *buf)
{
	/* File and directory pages: the cluster heap, nothing else. */
	return exfat_dev_write(exfat_fs_of_guard(dev), lba, count, buf,
			       EXFAT_W_HEAP);
}

static int exfat_guard_sync(block_device_t *dev)
{
	return exfat_dev_sync(exfat_fs_of_guard(dev));
}

void exfat_guard_init(exfat_fs_t *fs)
{
	mm_memset(&fs->guard, 0, sizeof(fs->guard));
	fs->guard.name = fs->bdev->name;
	fs->guard.sector_size = fs->bdev->sector_size;
	fs->guard.total_sectors = fs->bdev->total_sectors;
	fs->guard.read = exfat_guard_read;
	fs->guard.write = exfat_guard_write;
	fs->guard.sync = exfat_guard_sync;
	/* magic stays 0: a legacy device as far as anybody can tell, so the
	 * extended members (all zero) are never consulted. */
}

/* Zero `count' device sectors from `lba' (directory clusters before they
 * become reachable).  Through the chokepoint, in EXFAT_ZERO_CHUNK pieces. */
int exfat_dev_zero(exfat_fs_t *fs, uint64_t lba, uint64_t count)
{
	uint64_t per = EXFAT_ZERO_CHUNK >> fs->dev_shift;
	uint8_t *z;
	int r = ST_OK;

	might_sleep();
	if (count == 0)
		return ST_OK;
	if (per == 0)
		per = 1;
	z = (uint8_t *)kcalloc(1, (size_t)(exfat_min64(count, per)
					   << fs->dev_shift));
	if (!z)
		return ST_NOMEM;
	while (count && r == ST_OK) {
		uint64_t n = count < per ? count : per;

		r = exfat_dev_write(fs, lba, n, z, EXFAT_W_HEAP);
		lba += n;
		count -= n;
	}
	kfree(z);
	if (r == ST_OK)
		fs->barrier_needed = 1;
	return r;
}

/* ===========================================================================
 * 5. Error policy
 *
 * Corruption found while mounted: the operation that met it fails with
 * ST_IO, and the file system reacts as errors= says -- remount-ro (the
 * default) latches the instance read-only, continue only logs, panic stops
 * the machine.  Nothing is written to record the error: a volume armed
 * for writing keeps VolumeDirty set (the clean marking refuses once errors
 * were seen), which is exactly the "check me" mark exFAT has; one that was
 * only read stays untouched.
 * =========================================================================== */

const char *exfat_name_of(exfat_fs_t *fs)
{
	const char *src = fs->mnt ? fs->mnt->source : NULL;

	/* The mount's source is a pointer the mount table fills in; it may
	 * not be there yet (or any more). */
	if (src && src[0])
		return src;
	return (fs->bdev && fs->bdev->name) ? fs->bdev->name : "?";
}

void exfat_fs_error(exfat_fs_t *fs, const char *what, uint32_t where)
{
	if (!fs)
		return;
	fs->errors_seen++;
	kprintf("exfat: %s: ERROR: %s (at %u)\n", exfat_name_of(fs), what,
		where);
	switch (fs->opts.errors) {
	case EXFAT_ERRORS_PANIC:
		panic("exfat: %s: errors=panic: %s\n", exfat_name_of(fs),
		      what);
		break;
	case EXFAT_ERRORS_CONTINUE:
		break;
	default:
		if (!fs->ro_latched) {
			fs->ro_latched = 1;
			if (!fs->ro)
				kprintf("exfat: %s: remounting read-only\n",
					exfat_name_of(fs));
		}
		break;
	}
}

/* ===========================================================================
 * 11. Timestamps
 *
 * A timestamp is a packed local date and time with 2-second resolution
 * (DoubleSeconds 0-4, Minute 5-10, Hour 11-15, Day 16-20, Month 21-24,
 * Year-1980 25-31), a 10 ms increment byte (0..199, also carrying the odd
 * second) and a UTC offset byte.  This driver always writes the offset, so
 * what it writes is unambiguous: UTC with offset 0 by default, local time
 * and its offset with time=local.  Timestamps without a valid offset are
 * read as UTC -- or as local time (time_offset) with time=local.
 * =========================================================================== */

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
static int64_t exfat_days_from_civil(int64_t y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

/* The civil date of a day count since 1970-01-01. */
static void exfat_civil_from_days(int64_t z, int64_t *y, unsigned *m,
				  unsigned *d)
{
	int64_t era;
	unsigned doe, yoe, doy, mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	*y = (int64_t)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y += *m <= 2;
}

/* The offset (minutes east of UTC) this mount writes with. */
static int exfat_write_offset(exfat_fs_t *fs)
{
	return fs->opts.time_local ? fs->opts.time_offset : 0;
}

/* Encode UTC `sec'.`nsec' as a timestamp in the mount's time base. */
void exfat_time_encode(exfat_fs_t *fs, int64_t sec, long nsec,
		       uint32_t *ts, uint8_t *cs10, uint8_t *utc)
{
	int off = exfat_write_offset(fs);
	int64_t local = sec + (int64_t)off * 60, days, y;
	unsigned m, d, rem, hh, mm, ss;
	int steps;

	if (nsec < 0 || nsec >= 1000000000L)
		nsec = 0;
	days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
	rem = (unsigned)(local - days * 86400);
	exfat_civil_from_days(days, &y, &m, &d);
	if (y < 1980) {
		y = 1980;
		m = 1;
		d = 1;
		rem = 0;
		nsec = 0;
	} else if (y > 2107) {
		y = 2107;
		m = 12;
		d = 31;
		rem = 86399;
		nsec = 990000000L;
	}
	hh = rem / 3600;
	mm = (rem / 60) % 60;
	ss = rem % 60;
	*ts = ((uint32_t)(y - 1980) << 25) | ((uint32_t)m << 21) |
	      ((uint32_t)d << 16) | ((uint32_t)hh << 11) | ((uint32_t)mm << 5) |
	      (uint32_t)(ss / 2);
	if (cs10)
		*cs10 = (uint8_t)((ss & 1) * 100 + nsec / 10000000L);
	steps = off / 15;
	*utc = (uint8_t)(EXFAT_UTC_VALID | ((unsigned)steps & 0x7F));
}

/* Decode a timestamp to UTC seconds and nanoseconds. */
void exfat_time_decode(exfat_fs_t *fs, uint32_t ts, uint8_t cs10,
		       uint8_t utc, uint64_t *sec, uint64_t *nsec)
{
	unsigned y = (ts >> 25) + 1980, m = (ts >> 21) & 0xF,
		 d = (ts >> 16) & 0x1F, hh = (ts >> 11) & 0x1F,
		 mm = (ts >> 5) & 0x3F, ss = (ts & 0x1F) * 2;
	int64_t t;
	int off_min;

	if (m < 1 || m > 12)
		m = 1;
	if (d < 1 || d > 31)
		d = 1;
	if (hh > 23)
		hh = 23;
	if (mm > 59)
		mm = 59;
	if (ss > 59)
		ss = 58;
	if (cs10 > 199)
		cs10 = 0;
	ss += cs10 / 100;
	t = exfat_days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
	if (utc & EXFAT_UTC_VALID) {
		int steps = utc & 0x7F;

		if (steps & 0x40)
			steps -= 0x80; /* signed 7-bit */
		off_min = steps * 15;
	} else {
		off_min = fs->opts.time_local ? fs->opts.time_offset : 0;
	}
	t -= (int64_t)off_min * 60;
	if (t < 0)
		t = 0;
	*sec = (uint64_t)t;
	if (nsec)
		*nsec = (uint64_t)(cs10 % 100) * 10000000ULL;
}

/* The current time, encoded. */
void exfat_now(exfat_fs_t *fs, uint32_t *ts, uint8_t *cs10,
	       uint8_t *utc)
{
	uint64_t ns = timer_get_realtime_ns();

	exfat_time_encode(fs, (int64_t)(ns / 1000000000ULL),
			  (long)(ns % 1000000000ULL), ts, cs10, utc);
}
