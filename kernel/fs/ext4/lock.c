// LikeOS - ext4 filesystem driver: the fine-grained locking engine (driver rwsem,
// I/O mutex, metadata, per-inode and block-group locks)
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * Fine-grained locking engine.
 *
 * Replaces the single reentrant I/O mutex (which serialised EVERY
 * filesystem operation — including multi-millisecond USB data transfers —
 * against every other one) with a conventional lock hierarchy:
 *
 *   per-inode rwsem  →  metadata rwsem  →  per-block-group lock
 *        →  cache spinlocks (mbc / parsed-inode / bpool / pagecache)
 *        →  block-device mutex
 *
 *   - st->meta (metadata/transaction rwsem): SHARED for read-only metadata
 *     access (path resolution, extent mapping, stat, readdir, xattr
 *     reads); EXCLUSIVE for every mutator.  The exclusive mode is exactly
 *     the old global mutex: the journal transaction machinery (st->txn,
 *     st->ckpt, write-back buffer, group-descriptor dirty spans, superblock
 *     counters) is entirely serialised by it and needs no further change.
 *     Exclusive acquire/release keeps the txn begin/commit hooks.
 *   - st->ilock[] (per-inode rwsems, hashed by inode number): file-content
 *     fences.  Readers hold SHARED across data-page reads (which run with
 *     NO metadata lock at all — see pagecache lock_map); write/truncate/
 *     unlink/rename-overwrite/O_TRUNC hold EXCLUSIVE so a file's data
 *     blocks can never be freed or rewritten under an in-flight reader.
 *     A hash collision merely over-serialises two unrelated files.
 *   - st->bglock[] (per-block-group locks, hashed): serialise allocation
 *     bitmap read-modify-write per group.  Today every allocator also
 *     holds st->meta exclusive (journaled bitmap writes), so these add
 *     ordering structure rather than parallelism; they become live when
 *     allocation moves off the exclusive path.
 *   - Superblock/GDT counter updates: under st->meta exclusive (this is the
 *     filesystem-wide "superblock lock" level of the hierarchy).
 *
 * Fairness: writers set w_wait, and new readers defer to queued writers
 * so a stream of readers cannot starve mutators.  The one exception is a
 * task that already holds a shared fs lock (task->fs_rdepth > 0): its
 * nested shared acquisition (page-in during a read) must not queue
 * behind a writer that is itself waiting for the first shared hold to
 * drain — that would deadlock.  Recursion under one's own EXCLUSIVE hold
 * (write path faulting into a mapped file, nested VFS calls) is granted
 * as a depth increment, preserving the old reentrant-mutex semantics.
 * =================================================================== */

/* Park the current task on `sem`'s wait channel.  Same discipline as the
 * other sleeping fs/device mutexes: blind-block under the spinlock, then
 * schedule; the wake side (sched_wake_channel) claims BLOCKED→READY
 * atomically, and sched_schedule handles the woken-before-scheduled race. */
static void ext4_rwsem_park(ext4_rwsem_t *sem, uint64_t *flags)
{
	task_t *cur = sched_current();
	if (cur) {
		cur->wait_channel = (void *)sem;
		task_set_blocked(cur);
	}
	spin_unlock_irqrestore(&sem->lock, *flags);
	sched_schedule();
}

/* Record one more shared hold of `cur' (sem->lock held). */
static void ext4_rhold_add(ext4_rwsem_t *sem, task_t *cur)
{
	int free_slot = -1;

	if (!cur || !sem->rh) {
		sem->untracked++;
		return;
	}
	for (unsigned i = 0; i < sem->rh_hi; i++) {
		if (sem->rh[i].count && sem->rh[i].task == (uint64_t)cur->id) {
			sem->rh[i].count++;
			return;
		}
		if (!sem->rh[i].count && free_slot < 0)
			free_slot = (int)i;
	}
	if (free_slot < 0) {
		if (sem->rh_hi >= sem->rh_n) {
			sem->untracked++;
			return;
		}
		free_slot = (int)sem->rh_hi++;
	}
	sem->rh[free_slot].task = (uint64_t)cur->id;
	sem->rh[free_slot].count = 1;
}

/* Forget one shared hold of `cur' (sem->lock held). */
static void ext4_rhold_del(ext4_rwsem_t *sem, task_t *cur)
{
	if (cur && sem->rh) {
		for (unsigned i = 0; i < sem->rh_hi; i++) {
			if (!sem->rh[i].count ||
			    sem->rh[i].task != (uint64_t)cur->id)
				continue;
			sem->rh[i].count--;
			while (sem->rh_hi && !sem->rh[sem->rh_hi - 1].count)
				sem->rh_hi--;
			return;
		}
	}
	if (!WARN_ON(sem->untracked <= 0))
		sem->untracked--;
}

/* A task that died inside the driver: give back its exclusive hold and
 * every shared hold recorded for it.  Returns 1 if it held anything; then
 * *had_excl says whether that included the exclusive hold. */
int ext4_rwsem_release_owner(ext4_rwsem_t *sem, uint64_t task_id,
				    int *had_excl)
{
	uint64_t flags;
	int released = 0;

	if (had_excl)
		*had_excl = 0;
	spin_lock_irqsave(&sem->lock, &flags);
	for (unsigned i = 0; i < sem->rh_hi; i++) {
		if (!sem->rh[i].count || sem->rh[i].task != task_id)
			continue;
		WARN_ON(sem->readers < sem->rh[i].count);
		sem->readers -= sem->rh[i].count;
		if (sem->readers < 0)
			sem->readers = 0;
		sem->rh[i].count = 0;
		released = 1;
	}
	while (sem->rh_hi && !sem->rh[sem->rh_hi - 1].count)
		sem->rh_hi--;
	if (sem->writer && sem->owner == task_id) {
		sem->writer = 0;
		sem->owner = (uint64_t)-1;
		sem->wdepth = 0;
		if (sem->r_wait > 0)
			sem->reader_turn = 1;
		if (had_excl)
			*had_excl = 1;
		released = 1;
	}
	spin_unlock_irqrestore(&sem->lock, flags);
	if (released)
		sched_wake_channel((void *)sem);
	return released;
}

static void ext4_rwsem_read_lock(ext4_rwsem_t *sem)
{
	might_sleep();
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	int queued = 0;
	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == my_id && cur) {
			/* Nested acquisition under our own exclusive hold:
			 * treat as exclusive recursion. */
			sem->wdepth++;
			cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		/* Defer to queued writers -- unless it is this cohort's turn,
		 * or deferring would deadlock a nested shared acquisition. */
		int defer_to_writers = sem->w_wait && !sem->reader_turn &&
				       !(cur && cur->fs_rdepth);
		if (!sem->writer && !defer_to_writers) {
			sem->readers++;
			ext4_rhold_add(sem, cur);
			if (cur) {
				cur->fs_rdepth++;
				cur->fs_held++;
			}
			if (queued) {
				sem->r_wait--;
				/* Cohort drained: the phase is over and the
				 * queued writers go next. */
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
		ext4_rwsem_park(sem, &flags);
	}
}

static void ext4_rwsem_read_unlock(ext4_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	uint64_t flags;
	spin_lock_irqsave(&sem->lock, &flags);
	if (sem->writer && sem->owner == my_id && cur) {
		/* matching nested-under-exclusive acquisition */
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
		ext4_rhold_del(sem, cur);
	}
	if (cur && cur->fs_rdepth > 0)
		cur->fs_rdepth--;
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	int wake = (sem->readers == 0);
	spin_unlock_irqrestore(&sem->lock, flags);
	if (wake)
		sched_wake_channel((void *)sem);
}

void ext4_rwsem_write_lock(ext4_rwsem_t *sem)
{
	might_sleep();
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	int queued = 0;
	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == my_id) {
			sem->wdepth++; /* reentrant exclusive */
			if (queued)
				sem->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		/* A cohort of readers was promised this phase; grabbing the
		 * sem in the gap before they wake would re-starve them (the
		 * wake is not instantaneous, and a writer that slips in here
		 * puts the whole stream ahead of them again). */
		int reader_phase = sem->reader_turn && sem->r_wait > 0;
		if (!sem->writer && sem->readers == 0 && !reader_phase) {
			sem->writer = 1;
			sem->owner = my_id;
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
		ext4_rwsem_park(sem, &flags);
	}
}

void ext4_rwsem_write_unlock(ext4_rwsem_t *sem)
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
	/* Waiting readers get the next turn, ahead of any queued writer --
	 * see reader_turn in the struct. */
	if (sem->r_wait > 0)
		sem->reader_turn = 1;
	spin_unlock_irqrestore(&sem->lock, flags);
	sched_wake_channel((void *)sem);
}


void ext4_rwsem_init(ext4_rwsem_t *sem, const char *name,
			    ext4_rhold_t *rh, unsigned rh_n)
{
	sem->readers = 0;
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	sem->w_wait = 0;
	sem->r_wait = 0;
	sem->reader_turn = 0;
	sem->rh = rh;
	sem->rh_n = rh ? rh_n : 0;
	sem->rh_hi = 0;
	sem->untracked = 0;
	for (unsigned i = 0; i < sem->rh_n; i++) {
		rh[i].task = 0;
		rh[i].count = 0;
	}
	spinlock_init(&sem->lock, name);
}


/* Debug mirrors read by the scheduler's Ctrl+D dump (legacy names).  They
 * describe the root filesystem's metadata lock. */
volatile int ext4_io_locked = 0;
volatile int ext4_io_depth = 0;
volatile uint64_t ext4_io_owner = (uint64_t)-1;

/* Metadata lock, EXCLUSIVE (mutators).  Semantically identical to the old
 * global mutex including the transaction hooks: the outermost acquire
 * begins a journal transaction, the matching outermost release commits it. */
void ext4_io_lock(ext4_fs_t *fs)
{
	ext4_rwsem_write_lock(&fs->st->meta);
	if (fs->st->meta.wdepth == 1) {
		ext4_txn_begin(fs);
		if (fs->st->is_root) {
			ext4_io_locked = 1;
			ext4_io_owner = fs->st->meta.owner;
		}
	}
	if (fs->st->is_root)
		ext4_io_depth = fs->st->meta.wdepth;
}

void ext4_io_unlock(ext4_fs_t *fs)
{
	/* On the outermost release, commit the operation's transaction while
	 * the lock is still held.  The flush sleeps on disk I/O — fine, we
	 * still own the exclusive hold. */
	if (fs->st->meta.wdepth == 1 && fs->st->txn.active)
		ext4_txn_flush(fs);
	if (fs->st->is_root) {
		if (fs->st->meta.wdepth == 1) {
			ext4_io_locked = 0;
			ext4_io_owner = (uint64_t)-1;
			ext4_io_depth = 0;
		} else {
			ext4_io_depth = fs->st->meta.wdepth - 1;
		}
	}
	ext4_rwsem_write_unlock(&fs->st->meta);
}

/* Metadata lock, SHARED (read-only ops: resolve/stat/readdir/extent
 * mapping/xattr reads).  Concurrent with other shared holders. */
void ext4_meta_rlock(ext4_fs_t *fs)
{
	ext4_rwsem_read_lock(&fs->st->meta);
}
void ext4_meta_runlock(ext4_fs_t *fs)
{
	ext4_rwsem_read_unlock(&fs->st->meta);
}

/* Does the current task hold the metadata lock EXCLUSIVE?  Used by shared
 * paths that would otherwise perform an opportunistic mutator action
 * (e.g. open's deferred GDT flush). */
int ext4_meta_held_excl(ext4_fs_t *fs)
{
	task_t *cur = sched_current();
	return fs->st->meta.writer && cur &&
	       fs->st->meta.owner == (uint64_t)cur->id;
}

/* Per-inode locks. */
static inline ext4_rwsem_t *ext4_ilock_of(ext4_fs_t *fs, unsigned long ino)
{
	return &fs->st->ilock[ino % EXT4_ILOCKS];
}
void ext4_ilock_shared(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_read_lock(ext4_ilock_of(fs, ino));
}
void ext4_iunlock_shared(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_read_unlock(ext4_ilock_of(fs, ino));
}
void ext4_ilock_excl(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_write_lock(ext4_ilock_of(fs, ino));
}
void ext4_iunlock_excl(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_write_unlock(ext4_ilock_of(fs, ino));
}

/* Per-block-group allocation locks (nest inside st->meta exclusive). */
void ext4_bg_lock(ext4_fs_t *fs, unsigned g)
{
	ext4_rwsem_write_lock(&fs->st->bglock[g % EXT4_BGLOCKS]);
}
void ext4_bg_unlock(ext4_fs_t *fs, unsigned g)
{
	ext4_rwsem_write_unlock(&fs->st->bglock[g % EXT4_BGLOCKS]);
}
