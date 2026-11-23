/* Unit test of the ext4 driver's lock engine (kernel/fs/ext4/): shared
 * holds are attributed to their task, and release_locks_for_task gives a
 * dead task's shared holds back as well as its exclusive ones -- of the
 * metadata lock, the per-inode locks and the xattr scratch lock.
 *
 * Built by host/test-ext4.sh against the objects of the filesystem harness
 * (host/test-fs.sh: stubs, caches, the kernel's formatter), with the files
 * of the driver included here so its static functions can be called.  The harness runs
 * one task; switching its id (test_task_set_id) plays the part of several.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "kernel/fs/ext4/super.c"
#include "kernel/fs/ext4/csum.c"
#include "kernel/fs/ext4/lock.c"
#include "kernel/fs/ext4/buffer.c"
#include "kernel/fs/ext4/balloc.c"
#include "kernel/fs/ext4/ialloc.c"
#include "kernel/fs/ext4/mballoc.c"
#include "kernel/fs/ext4/inode.c"
#include "kernel/fs/ext4/extents.c"
#include "kernel/fs/ext4/indirect.c"
#include "kernel/fs/ext4/namei.c"
#include "kernel/fs/ext4/hash.c"
#include "kernel/fs/ext4/dir.c"
#include "kernel/fs/ext4/file.c"
#include "kernel/fs/ext4/symlink.c"
#include "kernel/fs/ext4/xattr.c"
#include "kernel/fs/ext4/ops.c"
#include "kernel/fs/ext4/journal.c"

void test_task_init(uint32_t uid, uint32_t gid);
void test_task_set_id(uint64_t id);
void pagecache_init(void);
extern long test_warnings;

/* The VFS walk is not part of this test. */
int vfs_link_hop(vfs_mount_t *m, const char *path, unsigned long cs,
		 unsigned long ce, const char *target)
{
	(void)m;
	(void)path;
	(void)cs;
	(void)ce;
	(void)target;
	return 0;
}

static int failures;

#define CHECK(cond)                                                       \
	do {                                                              \
		if (!(cond)) {                                            \
			h_printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				 #cond);                                  \
			failures++;                                       \
		}                                                         \
	} while (0)

/* Can a writer get `sem' right now?  Takes and drops it if so. */
static int can_write(ext4_rwsem_t *sem)
{
	uint64_t flags;
	int ok;

	spin_lock_irqsave(&sem->lock, &flags);
	ok = !sem->writer && sem->readers == 0;
	spin_unlock_irqrestore(&sem->lock, flags);
	if (ok) {
		ext4_rwsem_write_lock(sem);
		ext4_rwsem_write_unlock(sem);
	}
	return ok;
}

int main(void)
{
	ext4_fs_t *fs = (ext4_fs_t *)kcalloc(1, sizeof(*fs));
	task_t *t;

	test_task_init(0, 0);
	t = sched_current();
	CHECK(ext4_state_alloc(fs) == ST_OK);
	ext4_rwsem_t *meta = &fs->st->meta;
	ext4_rwsem_t *il = ext4_ilock_of(fs, 12);

	/* Task 5 takes the metadata lock shared twice (nested) and an inode
	 * lock shared, task 6 the metadata lock once. */
	test_task_set_id(5);
	ext4_meta_rlock(fs);
	ext4_meta_rlock(fs);
	ext4_ilock_shared(fs, 12);
	test_task_set_id(6);
	ext4_meta_rlock(fs);
	CHECK(meta->readers == 3);
	CHECK(il->readers == 1);
	CHECK(!can_write(meta));

	/* Task 5 dies inside the driver: all its holds come back. */
	CHECK(ext4_io_release_if_owner(fs, 5) == 1);
	CHECK(meta->readers == 1);
	CHECK(il->readers == 0);
	CHECK(can_write(il));
	CHECK(!can_write(meta)); /* task 6 still holds it */
	CHECK(ext4_io_release_if_owner(fs, 99) == 0); /* holds nothing */

	/* Task 6 lets go normally. */
	test_task_set_id(6);
	ext4_meta_runlock(fs);
	CHECK(meta->readers == 0);
	CHECK(meta->rh_hi == 0);

	/* An exclusive holder with a running transaction dies: the lock and
	 * the transaction both go. */
	test_task_set_id(7);
	ext4_io_lock(fs);
	CHECK(ext4_meta_held_excl(fs));
	ext4_ilock_excl(fs, 40);
	fs->st->txn.active = 1;
	fs->st->txn.n = 0;
	CHECK(ext4_io_release_if_owner(fs, 7) == 1);
	CHECK(!meta->writer);
	CHECK(!fs->st->txn.active);
	CHECK(can_write(ext4_ilock_of(fs, 40)));

	/* The xattr scratch lock, held exclusive by a task that dies. */
	test_task_set_id(8);
	ext4_rwsem_write_lock(&s_xa_scratch);
	CHECK(ext4_io_release_if_owner(fs, 8) == 1);
	CHECK(!s_xa_scratch.writer);

	/* More readers than the table records: the excess is counted
	 * untracked and still released by its holders. */
	for (uint64_t id = 100; id < 100 + EXT4_META_TRACK + 6; id++) {
		test_task_set_id(id);
		ext4_meta_rlock(fs);
	}
	CHECK(meta->readers == EXT4_META_TRACK + 6);
	CHECK(meta->untracked == 6);
	CHECK(ext4_io_release_if_owner(fs, 100) == 1); /* a tracked one */
	CHECK(meta->readers == EXT4_META_TRACK + 5);
	for (uint64_t id = 101; id < 100 + EXT4_META_TRACK + 6; id++) {
		test_task_set_id(id);
		ext4_meta_runlock(fs);
	}
	CHECK(meta->readers == 0);
	CHECK(meta->untracked == 0);
	CHECK(meta->rh_hi == 0);

	/* Shared under one's own exclusive hold is recursion, not a
	 * recorded shared hold. */
	test_task_set_id(9);
	ext4_io_lock(fs);
	ext4_meta_rlock(fs);
	CHECK(meta->readers == 0 && meta->wdepth == 2);
	ext4_meta_runlock(fs);
	ext4_io_unlock(fs);
	CHECK(!meta->writer);

	/* The task's own counters came back to zero for the live paths. */
	t->fs_rdepth = 0;
	t->fs_held = 0;
	test_task_set_id(1);
	CHECK(can_write(meta));
	CHECK(test_warnings == 0);

	ext4_fs_release(fs);
	kfree(fs);
	h_printf("test-ext4-rwsem: %s (%d failure(s))\n",
		 failures ? "FAILED" : "ok", failures);
	return failures ? 1 : 0;
}
