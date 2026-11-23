/* LikeOS - NTFS filesystem driver
 *
 * Helpers (strings, allocation, times), locking, error handling.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#define NTFS_INTERNAL 1 /* the driver-internal section of ntfs.h */
#include <kernel/fs/ntfs/ntfs.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/fs/ntfs/ntfs_int.h>

/* ===========================================================================
 * S2  Helpers
 * =========================================================================== */

unsigned long ntfs_strlen(const char *s)
{
	unsigned long n = 0;

	while (s && s[n])
		n++;
	return n;
}

int ntfs_streq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int ntfs_strneq(const char *a, const char *b, unsigned long n)
{
	for (unsigned long i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
		if (!a[i])
			return 1;
	}
	return 1;
}

/* Allocations: a zeroed one, and a free that tolerates NULL. */
void *ntfs_zalloc(unsigned long n)
{
	void *p = kalloc(n ? n : 1);

	if (p)
		mm_memset(p, 0, n ? n : 1);
	return p;
}

void ntfs_free(void *p)
{
	if (p)
		kfree(p);
}

/* memmove (the kernel has memcpy only). */
void ntfs_memmove(void *dst, const void *src, unsigned long n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *sp = (const uint8_t *)src;

	if (d == sp || !n)
		return;
	if (d < sp || d >= sp + n) {
		for (unsigned long i = 0; i < n; i++)
			d[i] = sp[i];
	} else {
		for (unsigned long i = n; i > 0; i--)
			d[i - 1] = sp[i - 1];
	}
}

int ntfs_all_zero(const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

/* ---- Times ------------------------------------------------------------------
 *
 * NTFS time: 100-nanosecond intervals since 1601-01-01 UTC, signed. */
#define NTFS_TIME_OFFSET 116444736000000000LL

void ntfs_time_to_unix(int64_t t, uint64_t *sec, uint64_t *nsec)
{
	int64_t u, s;

	/* Any 64-bit value is a time on disk: clamp instead of wrapping. */
	if (t < (-0x7FFFFFFFFFFFFFFFLL - 1) + NTFS_TIME_OFFSET)
		t = (-0x7FFFFFFFFFFFFFFFLL - 1) + NTFS_TIME_OFFSET;
	u = t - NTFS_TIME_OFFSET;
	s = u / 10000000LL;
	int64_t r = u % 10000000LL;

	if (r < 0) {
		r += 10000000LL;
		s -= 1;
	}
	*sec = (uint64_t)s;
	*nsec = (uint64_t)r * 100ULL;
}

/* The current time as an NTFS time (100 ns units since 1601). */
int64_t ntfs_now(void)
{
	uint64_t s = timer_get_epoch();

	if (s > 900000000000ULL)
		s = 900000000000ULL; /* never wraps the signed 64-bit time */
	return (int64_t)s * 10000000LL + NTFS_TIME_OFFSET;
}

/* Unix seconds + nanoseconds -> NTFS time, clamped. */
int64_t ntfs_unix_to_time(int64_t sec, long nsec)
{
	if (sec < -11644473600LL)
		sec = -11644473600LL;
	if (sec > 900000000000LL)
		sec = 900000000000LL;
	if (nsec < 0 || nsec > 999999999L)
		nsec = 0;
	return sec * 10000000LL + (int64_t)(nsec / 100) + NTFS_TIME_OFFSET;
}

/* ===========================================================================
 * S3  Locking: the private recursive reader/writer semaphore
 *
 * The design (ext4's) is described with ntfs_rwsem_t in ntfs.h; the lock
 * order and the lock instances (metadata, per-file, allocator) are in
 * the next section (S3).
 * =========================================================================== */

/* Park the current task on `sem': blind-block under the spinlock, then
 * schedule; the wake side claims BLOCKED -> READY atomically. */
static void ntfs_rwsem_park(ntfs_rwsem_t *sem, uint64_t *flags)
{
	task_t *cur = sched_current();

	if (cur) {
		cur->wait_channel = (void *)sem;
		task_set_blocked(cur);
	}
	spin_unlock_irqrestore(&sem->lock, *flags);
	sched_schedule();
}

void ntfs_rwsem_init(ntfs_rwsem_t *sem, const char *name)
{
	sem->readers = 0;
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	sem->w_wait = 0;
	sem->r_wait = 0;
	sem->reader_turn = 0;
	sem->untracked = 0;
	for (int i = 0; i < NTFS_RWSEM_TRACK; i++) {
		sem->rh[i].task = 0;
		sem->rh[i].count = 0;
	}
	spinlock_init(&sem->lock, name);
}

/* Record one more shared hold of `cur' (sem->lock held).  A task can die
 * inside the driver only through a kernel-mode fault on a user address;
 * the driver never touches user memory while it holds this lock, and these
 * records let release_locks_for_task() give a dead task's shared holds
 * back even if a path broke that rule. */
static void ntfs_rhold_add(ntfs_rwsem_t *sem, task_t *cur)
{
	int free_slot = -1;

	if (!cur) {
		sem->untracked++;
		return;
	}
	for (int i = 0; i < NTFS_RWSEM_TRACK; i++) {
		if (sem->rh[i].count && sem->rh[i].task == (uint64_t)cur->id) {
			sem->rh[i].count++;
			return;
		}
		if (!sem->rh[i].count && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		sem->untracked++;
		return;
	}
	sem->rh[free_slot].task = (uint64_t)cur->id;
	sem->rh[free_slot].count = 1;
}

/* Forget one shared hold of `cur' (sem->lock held). */
static void ntfs_rhold_del(ntfs_rwsem_t *sem, task_t *cur)
{
	if (cur)
		for (int i = 0; i < NTFS_RWSEM_TRACK; i++)
			if (sem->rh[i].count &&
			    sem->rh[i].task == (uint64_t)cur->id) {
				sem->rh[i].count--;
				return;
			}
	if (!WARN_ON(sem->untracked <= 0))
		sem->untracked--;
}

void ntfs_rwsem_read_lock(ntfs_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	int queued = 0;

	might_sleep();
	for (;;) {
		uint64_t flags;

		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && cur && sem->owner == me) {
			/* Shared under our own exclusive hold: recursion. */
			sem->wdepth++;
			cur->fs_held++;
			if (queued)
				sem->r_wait--;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		int defer = sem->w_wait && !sem->reader_turn &&
			    !(cur && cur->fs_rdepth);
		if (!sem->writer && !defer) {
			sem->readers++;
			ntfs_rhold_add(sem, cur);
			if (cur) {
				cur->fs_rdepth++;
				cur->fs_held++;
			}
			if (queued) {
				sem->r_wait--;
				if (sem->r_wait == 0)
					sem->reader_turn = 0;
			}
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		if (!queued) {
			sem->r_wait++;
			queued = 1;
		}
		ntfs_rwsem_park(sem, &flags);
	}
}

void ntfs_rwsem_read_unlock(ntfs_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	uint64_t flags;
	int wake;

	spin_lock_irqsave(&sem->lock, &flags);
	if (sem->writer && cur && sem->owner == me) {
		WARN_ON(sem->wdepth <= 1);
		sem->wdepth--;
		if (cur->fs_held > 0)
			cur->fs_held--;
		spin_unlock_irqrestore(&sem->lock, flags);
		return;
	}
	WARN_ON(sem->readers <= 0);
	if (sem->readers > 0) {
		sem->readers--;
		ntfs_rhold_del(sem, cur);
	}
	if (cur && cur->fs_rdepth > 0)
		cur->fs_rdepth--;
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	wake = (sem->readers == 0);
	spin_unlock_irqrestore(&sem->lock, flags);
	if (wake)
		sched_wake_channel((void *)sem);
}

void ntfs_rwsem_write_lock(ntfs_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t me = cur ? cur->id : 0;
	int queued = 0;

	might_sleep();
	for (;;) {
		uint64_t flags;

		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == me) {
			sem->wdepth++;
			if (queued)
				sem->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		int reader_phase = sem->reader_turn && sem->r_wait > 0;
		if (!sem->writer && sem->readers == 0 && !reader_phase) {
			sem->writer = 1;
			sem->owner = me;
			sem->wdepth = 1;
			if (queued)
				sem->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		if (!queued) {
			sem->w_wait++;
			queued = 1;
		}
		ntfs_rwsem_park(sem, &flags);
	}
}

void ntfs_rwsem_write_unlock(ntfs_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t flags;

	spin_lock_irqsave(&sem->lock, &flags);
	WARN_ON(!sem->writer || sem->wdepth <= 0);
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	if (sem->wdepth > 1) {
		sem->wdepth--;
		spin_unlock_irqrestore(&sem->lock, flags);
		return;
	}
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	if (sem->r_wait > 0)
		sem->reader_turn = 1;
	spin_unlock_irqrestore(&sem->lock, flags);
	sched_wake_channel((void *)sem);
}

/* Is `sem' held exclusive by the current task? */
int ntfs_rwsem_held_excl(ntfs_rwsem_t *sem)
{
	task_t *cur = sched_current();

	return sem->writer && sem->owner == (uint64_t)(cur ? cur->id : 0);
}

/* Force-release the holds of a dead task: its exclusive hold and every
 * shared hold recorded for it (ntfs_rhold_add()). */
int ntfs_rwsem_release_owner(ntfs_rwsem_t *sem, uint64_t task_id)
{
	uint64_t flags;
	int r = 0;

	spin_lock_irqsave(&sem->lock, &flags);
	for (int i = 0; i < NTFS_RWSEM_TRACK; i++) {
		if (!sem->rh[i].count || sem->rh[i].task != task_id)
			continue;
		WARN_ON(sem->readers < sem->rh[i].count);
		sem->readers -= sem->rh[i].count;
		if (sem->readers < 0)
			sem->readers = 0;
		sem->rh[i].count = 0;
		r = 1;
	}
	if (sem->writer && sem->owner == task_id) {
		sem->writer = 0;
		sem->owner = (uint64_t)-1;
		sem->wdepth = 0;
		if (sem->r_wait > 0)
			sem->reader_turn = 1;
		r = 1;
	}
	spin_unlock_irqrestore(&sem->lock, flags);
	if (r)
		sched_wake_channel((void *)sem);
	return r;
}

/* ===========================================================================
 * S3  Locking
 *
 *   per-file rwsem (fs->flock[mft % NTFS_FLOCKS])
 *     -> metadata rwsem (fs->meta; lock_map = SHARED, lock_io = EXCLUSIVE)
 *       -> allocator mutex (fs->alloc, exclusive only)
 *         -> spinlocks: rc_lock, ic_lock, hdl_lock, sf_lock, then the
 *            page / inode / dentry caches' own
 *           -> block device
 *
 * Readers of file data hold the per-file rwsem SHARED across their page
 * reads (the page cache transfers data with no filesystem lock: the file
 * lock fences the data blocks' lifetime) and the metadata rwsem SHARED
 * while they map.  Everything that changes the volume holds the
 * metadata rwsem EXCLUSIVE; truncate / unlink / write hold the file lock
 * EXCLUSIVE.  No kfree, no I/O and no sleeping under a spinlock.
 *
 * The rwsem itself (ntfs_rwsem_t, ntfs.h) is implemented above.
 * =========================================================================== */

/* --- the lock instances --- */

void ntfs_meta_rlock(ntfs_fs_t *fs)
{
	ntfs_rwsem_read_lock(&fs->meta);
}

void ntfs_meta_runlock(ntfs_fs_t *fs)
{
	ntfs_rwsem_read_unlock(&fs->meta);
}

void ntfs_meta_wlock(ntfs_fs_t *fs)
{
	ntfs_rwsem_write_lock(&fs->meta);
}

void ntfs_meta_wunlock(ntfs_fs_t *fs)
{
	ntfs_rwsem_write_unlock(&fs->meta);
}

int ntfs_meta_held_excl(ntfs_fs_t *fs)
{
	return ntfs_rwsem_held_excl(&fs->meta);
}

static inline ntfs_rwsem_t *ntfs_flock_of(ntfs_fs_t *fs, uint64_t mft_no)
{
	return &fs->flock[mft_no % NTFS_FLOCKS];
}

void ntfs_flock_shared(ntfs_fs_t *fs, uint64_t mft_no)
{
	ntfs_rwsem_read_lock(ntfs_flock_of(fs, mft_no));
}

void ntfs_funlock_shared(ntfs_fs_t *fs, uint64_t mft_no)
{
	ntfs_rwsem_read_unlock(ntfs_flock_of(fs, mft_no));
}

void ntfs_flock_excl(ntfs_fs_t *fs, uint64_t mft_no)
{
	ntfs_rwsem_write_lock(ntfs_flock_of(fs, mft_no));
}

void ntfs_funlock_excl(ntfs_fs_t *fs, uint64_t mft_no)
{
	ntfs_rwsem_write_unlock(ntfs_flock_of(fs, mft_no));
}

/* Allocator mutex: only exclusive, always nested inside the metadata
 * rwsem held exclusive. */
void ntfs_alloc_lock(ntfs_fs_t *fs)
{
	ntfs_assert_meta_excl(fs);
	ntfs_rwsem_write_lock(&fs->alloc);
}

void ntfs_alloc_unlock(ntfs_fs_t *fs)
{
	ntfs_rwsem_write_unlock(&fs->alloc);
}

/* ===========================================================================
 * S23  Write infrastructure: errors, times, write-through, MFT record
 *      writes, $MFTMirr, $LogFile reset, $Volume flags, arming
 *
 * Every change of the volume runs with the metadata lock EXCLUSIVE (the
 * page cache's writeback holds it too, as lock_io()).  Metadata is written
 * THROUGH, synchronously and in a fixed order (allocate before reference,
 * dereference before free -- S24..S31 say where), never left dirty in the
 * page cache:
 *
 *   ntfs_wt_write()      bytes of a non-resident metadata stream ($MFT,
 *                        $Bitmap, $MFT:$BITMAP, an index allocation):
 *                        whole device sectors, the edges read back from
 *                        the device first, then the cached pages of the
 *                        stream are updated in place (never marked dirty:
 *                        writeback has nothing of them to store);
 *   ntfs_mft_write()     one MFT record: re-validated (a record that fails
 *                        the reader's checks is never written), protected
 *                        with its update sequence array (a fresh USN), its
 *                        whole I/O unit written (neighbouring records of a
 *                        4 KB-sector device preserved), the mirror
 *                        copy in $MFTMirr updated for mirrored records,
 *                        the record cache refreshed;
 *   ntfs_logfile_reset() / ntfs_vol_flags() / ntfs_arm(): the volume's
 *                        lifecycle (ntfs.h section 3).
 *
 * Errors.  A failed device write latches the mount read-only
 * (ntfs_dev_write()); corruption found while changing something does the
 * same through ntfs_fs_error().  Both record fs->errors, so the volume
 * keeps its dirty flag at unmount and chkdsk runs.
 * =========================================================================== */

/* ---- write-side rules, and where they live ----
 *
 *  - No attribute data is ever cached dirty apart from a stream's own
 *    pages (EAs, reparse data, lists, indexes are written through); a
 *    conversion writes the data before the record says non-resident
 *    (ntfs_stream_unresident + commit).
 *  - ntfs_add_name(): packed EA size or reparse tag; only ctime changes.
 *  - ntfs_create_in(): failures free exactly what was allocated (never
 *    record 0), a written record is deleted like a file (attribute list,
 *    extension records, $Reparse entry).
 *  - ntfs_clusters_free(): counts only bits it cleared.
 *  - The layout (ntfs_eino_commit) moves attributes between records; no
 *    other stream is ever converted to make room.
 *  - ntfs_fsync(): commit, then device flush, its error.
 *  - ntfs_stream_write(): sizes settle where the data ends; nothing
 *    reaches the disk before the data, so no on-disk rollback can fail.
 *  - S29: the median entry keeps the left child; nodes are re-encoded
 *    whole and checked against the block size.
 *  - S28: the EA pair is rebuilt together.
 *  - S30: gap zeroing through the cache, the straddle page kept, cached
 *    tails zeroed on shrink.
 *  - No separate attribute inodes; every runlist change builds a new array
 *    under the metadata lock and swaps it in, no element pointer survives
 *    a change.
 *  - Direct I/O is synchronous: nothing in flight on fallback.
 *  - ntfs_clusters_alloc(): contiguous requests get one run or fail, a
 *    failed goal restarts the search, every candidate is inside the
 *    volume.
 *  - Attribute lists are built complete, written whole clusters into
 *    fresh clusters before the base record points at them.
 *  - $MFT / $MFT:$BITMAP growth (S25, S27): new clusters through the
 *    accounted allocator, all freed again on failure, the old runlist
 *    untouched until success.
 *  - ntfs_dl_uncovered(): an unmappable fragment is ST_IO.
 *  - S23 / S32: the dirty flag is set at arming, set/clear are
 *    read-modify-write, cleared only by ntfs_settle(clean) after every
 *    commit.
 *  - Truncate runs under the file lock exclusive and the metadata lock;
 *    writeback never waits for a lock we hold.
 *  - Errors propagate unchanged.
 *  - The full-page shortcut is taken only for an aligned whole page.
 *  - Metadata pages are never under writeback: changed in place under the
 *    metadata lock and written through.
 *  - ntfs_mft_write(): whole I/O unit, synchronous, mirror through its
 *    runlist, errors returned; there is no $MFT page writeback.
 *  - ntfs_mft_alloc(): counters, records from 24, validated old record,
 *    numbers below 2^32.
 *  - ntfs_zero_new() errors fail the placement.
 *  - Attributes are edited in the record image they belong to.
 *  - Index roots may live in an extension record (layout).
 *  - Index blocks are protected only after they were built valid.
 *  - Every namespace change holds the metadata lock exclusive.
 *  - ntfs_logfile_reset(): one buffer, exact sector ranges.
 *  - Directories keep link count 1 and cannot be hard linked.
 *  - Truncate commits the new size before anything is dropped.
 *  - ACLs are read per call from their EA, whole (no cache to go stale,
 *    no fixed-size buffer).
 *  - Inherited ACLs are set on the new record before its first commit
 *    (ntfs_acl_inherit()).
 *  - chmod rewrites the access ACL and $LXMOD in one commit.
 *  - ntfs_stream_create() / ntfs_stream_remove() and the user.* xattr view
 *    of streams.
 *  - ntfs_discard_range() / ntfs_trim_fs(): 64-bit sizes, PAGE_SIZE-based
 *    bitmap pages, start aligned up, end down.
 *  - ntfs_set_data_flags(): records rebuilt whole, nothing to change
 *    returns at once, resident empty data converted, non-empty data
 *    refused.
 *  - ntfs_cstream_rewrite(): whole-unit rewrites, one write per unit, data
 *    before the mapping, zero units as holes, 64-bit offsets, source
 *    copied before any unit.
 *  - A clean $LogFile is written before the clean flag
 *    (ntfs_logfile_write_clean()).
 *  - system.* attributes need the file's owner or root (ntfs_owner_ok()).
 * The VFS has no interface for O_TMPFILE, file attribute flag writes
 * (the file-flags EA / fileattr), fallocate or FITRIM, so none is
 * offered. */

/* ---- errors ---- */

/* Latch the mount read-only: nothing is written from now on.  The volume
 * keeps the dirty flag it has on disk (set when the mount armed), so the
 * next system that mounts it runs a check. */
void ntfs_latch(ntfs_fs_t *fs, const char *why)
{
	fs->errors = 1;
	if (fs->latched_ro)
		return;
	fs->latched_ro = 1;
	kprintf("ntfs: %s: %s; the volume is read-only from now on and stays "
		"marked dirty.  Run chkdsk.\n",
		fs->bdev && fs->bdev->name ? fs->bdev->name : "?", why);
	if (fs->opt.errors == NTFS_ERRORS_PANIC)
		panic("ntfs: errors=panic: %s", why);
}

/* Corruption (or an inconsistency the driver cannot repair) found while
 * changing the volume: record it and apply the errors= policy.  Returns
 * ST_IO for the caller to pass on.
 *
 *   errors=remount-ro  (the default here) stop writing: the mount is
 *                      latched read-only;
 *   errors=panic       stop the system;
 *   errors=continue    only the operation that met the problem fails; the
 *                      mount stays read-write.  The error is recorded all
 *                      the same, so the volume keeps its dirty flag at
 *                      unmount and the next check repairs it.
 *
 * A failed device write or flush always latches read-only, whatever the
 * policy (ntfs_dev_write(), ntfs_dev_flush()): what is on the device is no
 * longer known. */
int ntfs_fs_error(ntfs_fs_t *fs, uint64_t no, const char *why)
{
	WARN_RATELIMIT(1, "ntfs: MFT record %llu: %s", (unsigned long long)no,
		       why);
	if (fs->ro)
		return ST_IO;
	if (fs->opt.errors == NTFS_ERRORS_CONTINUE) {
		if (!fs->errors)
			kprintf("ntfs: %s: %s; errors=continue: the volume "
				"stays writable and is left marked dirty.\n",
				fs->bdev && fs->bdev->name ? fs->bdev->name :
							     "?",
				why);
		fs->errors = 1;
		return ST_IO;
	}
	ntfs_latch(fs, why);
	return ST_IO;
}

/* Is file `mref' part of the volume's own metadata, which no operation of
 * a user may change: a system record (below record 16, the root
 * directory excepted) or anything in or below $Extend ($Quota, $ObjId,
 * $Reparse, $UsnJrnl, $RmMetadata ...)?  The parent chain is walked
 * through the first name of each directory (at most 32 levels); a chain
 * that cannot be read counts as protected (fail closed). */
int ntfs_protected(ntfs_fs_t *fs, uint64_t mref)
{
	ntfs_inode_t *ino;
	uint64_t cur = mref;
	int prot = 1;

	if (NTFS_MREF_NO(mref) < NTFS_FILE_FIRST_USER)
		return NTFS_MREF_NO(mref) != NTFS_FILE_ROOT;
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!ino)
		return 1;
	for (int depth = 0; depth < 32; depth++) {
		uint64_t pno;

		if (ntfs_inode_load(fs, cur, ino) != ST_OK)
			break;
		pno = NTFS_MREF_NO(ino->parent);
		if (!ino->parent || pno == NTFS_FILE_ROOT) {
			prot = 0; /* reached the root without $Extend */
			break;
		}
		if (pno == NTFS_FILE_EXTEND || pno < NTFS_FILE_FIRST_USER)
			break; /* below $Extend (or another system file) */
		if (pno == NTFS_MREF_NO(cur))
			break;
		cur = ino->parent;
	}
	kfree(ino);
	return prot;
}

/* A multi-write update could not be finished after its first referencing
 * write reached the device: the structures on disk are inconsistent.
 * Whatever errors= says, nothing more is written (a check repairs it); the
 * operation itself is not undone (undoing would remove things the device
 * already refers to). */
void ntfs_fs_broken(ntfs_fs_t *fs, uint64_t no, const char *why)
{
	WARN_RATELIMIT(1, "ntfs: MFT record %llu: %s", (unsigned long long)no,
		       why);
	ntfs_latch(fs, why);
}

/* An update that failed (no memory, mostly) left something allocated
 * that nothing references -- a record or clusters marked in use: no harm
 * to anything on the volume, but it must not be marked clean, so that the
 * next check reclaims it.  The mount stays writable. */
void ntfs_leaked(ntfs_fs_t *fs)
{
	if (!fs->keep_dirty && fs->armed)
		kprintf("ntfs: %s: a failed update left allocated space "
			"behind; the volume stays marked dirty for a check\n",
			fs->bdev && fs->bdev->name ? fs->bdev->name : "?");
	fs->keep_dirty = 1;
}

/* May the volume be changed now?  ST_OK, or ST_ROFS for a read-only
 * mount (also one latched read-only after an error).  Nothing is armed
 * here: the chokepoint arms right before the first real write. */
int ntfs_write_begin(ntfs_fs_t *fs)
{
	ntfs_assert_meta_excl(fs);
	if (!ntfs_writable(fs))
		return ST_ROFS;
	return ST_OK;
}
