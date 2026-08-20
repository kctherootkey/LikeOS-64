// LikeOS-64 -- sync and fsync.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/file.h>

int64_t sys_fsync(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = fdget(cur, (int)fd);
	int64_t ret = 0;

	if (!file)
		return -EBADF;
	if (!fd_is_special(file)) {
		/* Dispatch to the file's own filesystem; a filesystem with
		 * nothing to flush leaves the op NULL and fsync is a no-op. */
		if (file->ops && file->ops->fsync)
			ret = file->ops->fsync(file);
	}
	fdput(file);
	return ret;
}

int64_t sys_sync(void)
{
	pagecache_sync();
	vfs_sync(); /* flush fs metadata + clean the journal (no-op on FAT32) */
	return 0;
}
