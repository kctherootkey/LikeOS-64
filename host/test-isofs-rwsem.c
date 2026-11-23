/* Unit test of the iso9660 driver's metadata lock (kernel/fs/isofs.c):
 * shared holds are attributed to their task, and release_locks_for_task
 * gives a dead task's shared holds back as well as an exclusive one.
 *
 * Built by host/test-isofs.sh against the objects of the filesystem harness
 * (host/test-fs.sh: stubs, caches, the kernel's formatter), with the driver
 * included here so its static functions can be called.  The harness runs
 * one task; switching its id (test_task_set_id) plays the part of several.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "kernel/fs/isofs.c"

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

int main(void)
{
	isofs_rwsem_t *s = (isofs_rwsem_t *)kcalloc(1, sizeof(*s));
	task_t *t;

	test_task_init(0, 0);
	t = sched_current();
	isofs_rwsem_init(s, "isofs_test");

	/* Task 5 takes it shared twice (nested), task 6 once. */
	test_task_set_id(5);
	isofs_rwsem_down_read(s);
	isofs_rwsem_down_read(s);
	test_task_set_id(6);
	isofs_rwsem_down_read(s);
	CHECK(s->readers == 3);

	/* A writer cannot get in. */
	test_task_set_id(7);
	CHECK(!isofs_rwsem_try_write(s));

	/* Task 5 dies inside the driver: both its holds come back. */
	CHECK(isofs_rwsem_release_owner(s, 5) == 1);
	CHECK(s->readers == 1);
	CHECK(!isofs_rwsem_try_write(s)); /* task 6 still holds it */
	CHECK(isofs_rwsem_release_owner(s, 99) == 0); /* holds nothing */

	/* Task 6 lets go normally. */
	test_task_set_id(6);
	isofs_rwsem_up_read(s);
	CHECK(s->readers == 0);

	/* Now the writer gets it; its death releases it too. */
	test_task_set_id(7);
	CHECK(isofs_rwsem_try_write(s));
	CHECK(isofs_rwsem_held_excl(s));
	CHECK(isofs_rwsem_release_owner(s, 7) == 1);
	CHECK(!s->writer);

	/* More readers than the table records: the excess is counted
	 * untracked and still released by its holders. */
	for (uint64_t id = 100; id < 100 + ISOFS_RWSEM_TRACK + 6; id++) {
		test_task_set_id(id);
		isofs_rwsem_down_read(s);
	}
	CHECK(s->readers == ISOFS_RWSEM_TRACK + 6);
	CHECK(s->untracked == 6);
	CHECK(isofs_rwsem_release_owner(s, 100) == 1); /* a tracked one */
	CHECK(s->readers == ISOFS_RWSEM_TRACK + 5);
	for (uint64_t id = 101; id < 100 + ISOFS_RWSEM_TRACK + 6; id++) {
		test_task_set_id(id);
		isofs_rwsem_up_read(s);
	}
	CHECK(s->readers == 0);
	CHECK(s->untracked == 0);

	/* The task's own counters came back to zero for the live paths. */
	t->fs_rdepth = 0;
	t->fs_held = 0;
	test_task_set_id(1);
	CHECK(isofs_rwsem_try_write(s));
	isofs_rwsem_up_write(s);
	CHECK(test_warnings == 0);

	kfree(s);
	h_printf("test-isofs-rwsem: %s (%d failure(s))\n",
		 failures ? "FAILED" : "ok", failures);
	return failures ? 1 : 0;
}
