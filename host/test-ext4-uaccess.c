/* Lock check of the ext4 driver's user-memory accesses (kernel/fs/ext4/):
 * user memory must never be touched while the task holds a filesystem lock
 * (a fault on a user address ends the task in place, and whatever it held
 * would stay held).  Every access is bracketed by smap_disable(); the
 * driver is compiled by host/test-ext4.sh with smap_disable renamed to the
 * function below, which stops the run (BUG) if the task holds any
 * filesystem lock at that moment (task->fs_held), and the filesystem
 * harness then drives reads, writes and directory listings through it.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>

void ext4_chk_smap_disable(void)
{
	task_t *cur = sched_current();

	BUG_ON(cur && cur->fs_held);
	smap_disable();
}
