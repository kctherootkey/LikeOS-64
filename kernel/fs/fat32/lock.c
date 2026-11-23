/* LikeOS - FAT filesystem driver: the lock engine (private recursive rwsem,
 * lock wrappers)
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

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
 * in write() through pagecache_balance_dirty, directory pages read under a
 * mutation).  The per-stream locks fence file data: readers hold them SHARED
 * across their page reads, which run with only the page cache's own short
 * mapping holds; write, truncate, unlink and rename-over hold them
 * EXCLUSIVE, so data blocks are never freed or rewritten under a reader.
 *
 * Fairness: a waiting writer holds back new readers (so a stream of readers
 * cannot starve mutations) -- except a task that already holds some
 * filesystem lock shared, whose nested shared request must not queue behind
 * a writer that waits for that very hold.  When an exclusive hold ends with
 * readers waiting, that cohort goes first, then the queued writers: neither
 * side can be starved by a stream of the other.
 * =========================================================================== */

void fat_rwsem_init(fat_rwsem_t *s, const char *name)
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
	for (unsigned i = 0; i < FAT_RWSEM_TRACK; i++) {
		s->rh[i].task = 0;
		s->rh[i].count = 0;
	}
	s->name = name;
	spinlock_init(&s->lock, name);
}

/* Record one more shared hold of `cur' (the rwsem's spinlock held). */
static void fat_rhold_add(fat_rwsem_t *s, task_t *cur)
{
	int free_slot = -1;

	if (!cur) {
		s->untracked++;
		return;
	}
	for (int i = 0; i < FAT_RWSEM_TRACK; i++) {
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
static void fat_rhold_del(fat_rwsem_t *s, task_t *cur)
{
	if (cur)
		for (int i = 0; i < FAT_RWSEM_TRACK; i++)
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
static void fat_rwsem_park(fat_rwsem_t *s, uint64_t *flags)
{
	task_t *cur = sched_current();

	if (cur) {
		cur->wait_channel = (void *)s;
		task_set_blocked(cur);
	}
	spin_unlock_irqrestore(&s->lock, *flags);
	sched_schedule();
}

void fat_rwsem_read_lock(fat_rwsem_t *s)
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
			fat_rhold_add(s, cur);
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
		fat_rwsem_park(s, &flags);
	}
}

void fat_rwsem_read_unlock(fat_rwsem_t *s)
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
		fat_rhold_del(s, cur);
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

void fat_rwsem_write_lock(fat_rwsem_t *s)
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
		fat_rwsem_park(s, &flags);
	}
}

void fat_rwsem_write_unlock(fat_rwsem_t *s)
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
int fat_rwsem_held_excl(fat_rwsem_t *s)
{
	return s->writer && s->owner == fat_task_id();
}

/* A task that died inside the driver: give back its exclusive hold AND
 * every shared hold recorded for it.  (User memory is never touched with a
 * filesystem lock held -- reads copy out of held pages after unlocking,
 * writes copy into a kernel buffer before locking, readdir assembles its
 * records in a kernel buffer -- so a fault in kernel mode cannot strand a
 * hold in the first place; this is the second line.) */
int fat_rwsem_release_owner(fat_rwsem_t *s, uint64_t task_id)
{
	uint64_t flags;
	int r = 0;

	spin_lock_irqsave(&s->lock, &flags);
	for (int i = 0; i < FAT_RWSEM_TRACK; i++) {
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

static inline fat_rwsem_t *fat_slock_of(fat_fs_t *fs, uint32_t handle)
{
	return &fs->slock[handle % FAT_SLOCKS];
}

void fat_slock_shared(fat_fs_t *fs, uint32_t h)
{
	fat_rwsem_read_lock(fat_slock_of(fs, h));
}

void fat_sunlock_shared(fat_fs_t *fs, uint32_t h)
{
	fat_rwsem_read_unlock(fat_slock_of(fs, h));
}

void fat_slock_excl(fat_fs_t *fs, uint32_t h)
{
	fat_rwsem_write_lock(fat_slock_of(fs, h));
}

void fat_sunlock_excl(fat_fs_t *fs, uint32_t h)
{
	fat_rwsem_write_unlock(fat_slock_of(fs, h));
}
