// LikeOS-64 -- madvise.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/fs/icache.h>


// SYS_MPROTECT - change memory protection
/*
 * madvise(2).  MADV_DONTNEED releases the pages of a range and leaves the
 * mapping alone, so the next touch reads a fresh zero page; every other advice
 * is a hint this kernel has nothing to do about, and returns success because
 * that is what advisory means.
 *
 * The one that matters is DONTNEED.  Without it, an allocator wanting to give
 * physical memory back has only munmap, which cuts the mapping in two and
 * spends a region record on every trim.
 */
static int64_t sys_madvise_locked(uint64_t addr, uint64_t length,
				  uint64_t advice)
{
	task_t *cur = task_mm_owner(sched_current());

	if (!cur)
		return -EFAULT;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL; /* the address must be page aligned */
	if (length == 0)
		return 0;
	if (addr >= 0x0000800000000000ULL || addr + length < addr)
		return -EINVAL;

	switch (advice) {
	case MADV_DONTNEED:
		mm_dontneed_range(cur, addr, length);
		return 0;
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
	case MADV_WILLNEED:
	case MADV_DONTDUMP:
	case MADV_DODUMP:
		return 0;
	default:
		return -EINVAL;
	}
}


int64_t sys_madvise(uint64_t addr, uint64_t length, uint64_t advice)
{
	RUN_WRITE_LOCKED(sys_madvise_locked(addr, length, advice));
}

