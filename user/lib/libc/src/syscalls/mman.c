#include <sys/mman.h>
#include <errno.h>
#include <unistd.h> /* getpagesize() for the msync alignment check */
#include "syscall.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
	long ret =
		syscall6(SYS_MMAP, (long)addr, length, prot, flags, fd, offset);
	if (ret < 0 && ret > -4096) {
		errno = -ret;
		return MAP_FAILED;
	}
	return (void *)ret;
}

int munmap(void *addr, size_t length)
{
	long ret = syscall2(SYS_MUNMAP, (long)addr, length);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
	long ret = syscall3(SYS_MPROTECT, (long)addr, len, prot);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/* mlock/munlock: page-locking stubs — the kernel currently does not
 * restrict page eviction for user mappings, so these succeed trivially. */
int mlock(const void *addr, size_t len)
{
	(void)addr;
	(void)len;
	return 0;
}
int munlock(const void *addr, size_t len)
{
	(void)addr;
	(void)len;
	return 0;
}
int mlockall(int flags)
{
	(void)flags;
	return 0;
}
int munlockall(void)
{
	return 0;
}

/* madvise is purely advisory; accepting and ignoring the hint is a
 * conforming implementation (like the mlock family above). */
int madvise(void *addr, size_t len, int advice)
{
	(void)addr;
	(void)len;
	(void)advice;
	return 0;
}

/* msync: flush a mapping back to its backing store.
 *
 * Nothing in this system needs to copy anything back.  A /dev/fb0 mapping
 * writes straight into video memory, so the pixels are already where they
 * belong; anonymous and shared-memory mappings have no backing store; and
 * file-backed MAP_SHARED is materialised as a private copy that the kernel
 * deliberately does not write back.  So the flush itself is a no-op — but the
 * argument checking is real, because callers use EINVAL to detect a bad
 * address or a nonsensical flag combination. */
int msync(void *addr, size_t length, int flags)
{
	if (((unsigned long)addr & (unsigned long)(getpagesize() - 1)) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) {
		errno = EINVAL;
		return -1;
	}
	/* MS_ASYNC and MS_SYNC are mutually exclusive. */
	if ((flags & MS_ASYNC) && (flags & MS_SYNC)) {
		errno = EINVAL;
		return -1;
	}
	(void)length;
	return 0;
}
