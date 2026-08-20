// LikeOS-64 -- directory reading.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>

int64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (count == 0)
		return 0;
	if (!validate_user_ptr(dirp, count))
		return -EFAULT;
	vfs_file_t *file = fdget(cur, (int)fd);
	long ret;

	if (!file)
		return -EBADF;
	/* Sockets and epoll instances were missing from this check, so
	 * getdents64() on one reached vfs_readdir with a marker. */
	if (fd_is_special(file)) {
		fdput(file);
		return -ENOTDIR;
	}
	ret = vfs_readdir(file, (void *)dirp, (long)count);
	fdput(file);
	if (ret == ST_UNSUPPORTED)
		return -ENOTDIR;
	return ret;
}

int64_t sys_getdents(uint64_t fd, uint64_t dirp, uint64_t count)
{
	return sys_getdents64(fd, dirp, count);
}
