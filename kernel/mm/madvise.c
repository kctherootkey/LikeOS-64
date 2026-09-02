// LikeOS-64 -- madvise.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h> /* copy_to_user for the mincore vector */

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
	case MADV_FREE:
		/* FREE permits the kernel to drop the pages any time before
		 * they are written again; dropping them now is the simplest
		 * conforming choice and the one allocators (which use it for
		 * a cheap "I am done with this") expect to cost nothing. */
		mm_dontneed_range(cur, addr, length);
		return 0;
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
	case MADV_WILLNEED:
	case MADV_REMOVE:
	case MADV_DONTFORK:
	case MADV_DOFORK:
	case MADV_MERGEABLE:
	case MADV_UNMERGEABLE:
	case MADV_HUGEPAGE:
	case MADV_NOHUGEPAGE:
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

/*
 * mincore(2): which pages of a range are resident.
 *
 * On this kernel the question is real -- anonymous memory, brk, BSS and
 * private file mappings are demand-paged, so a mapped page that has never
 * been touched has no frame behind it -- and the answer is one page-table
 * walk per page: present translation means resident.  JavaScriptCore's heap
 * statistics are the first caller; the diagnostics in /usr/local/bin can use
 * the same view.
 *
 * The walk runs under the address-space READ lock, the same one a page fault
 * holds, so a concurrent munmap cannot free a page-table page mid-walk.  The
 * result is staged through a kernel buffer and copied out with NO lock held:
 * the destination vector is user memory, whose own demand fault must be free
 * to take the lock itself.  Chunked so the stage buffer stays small.
 *
 * Deviation, stated rather than hidden: the conventional call fails with
 * ENOMEM when part of the range is not mapped at all.  Distinguishing
 * "mapped but never touched" from "not mapped" would mean consulting the
 * region table as well as the page tables, and every caller found so far
 * only asks about its own live mappings; an unmapped page simply reports
 * non-resident here.
 */
static int64_t sys_mincore_walk(task_t *mm, uint64_t addr, uint64_t pages,
				uint64_t vec)
{
	uint8_t kbuf[256];
	uint64_t done = 0;

	while (done < pages) {
		uint64_t n = pages - done;
		if (n > sizeof(kbuf))
			n = sizeof(kbuf);

		mm_read_lock(&mm->mmap_lock);
		for (uint64_t i = 0; i < n; i++) {
			uint64_t va = addr + ((done + i) << 12);
			kbuf[i] = mm_virt_to_phys_in(mm->pml4, va) ? 1 : 0;
		}
		mm_read_unlock(&mm->mmap_lock);

		if (copy_to_user((void *)(vec + done), kbuf, (size_t)n) != 0)
			return -EFAULT;
		done += n;
	}
	return 0;
}

int64_t sys_mincore(uint64_t addr, uint64_t length, uint64_t vec)
{
	task_t *cur = sched_current();
	task_t *mm = cur ? task_mm_owner(cur) : NULL;

	if (!mm)
		return -EFAULT;
	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (addr >= 0x0000800000000000ULL || addr + length < addr)
		return -ENOMEM;
	if (length == 0)
		return 0;

	uint64_t pages = (length + PAGE_SIZE - 1) >> 12;
	if (!validate_user_ptr(vec, pages))
		return -EFAULT;

	return sys_mincore_walk(mm, addr, pages, vec);
}
