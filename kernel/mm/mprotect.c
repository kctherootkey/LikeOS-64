// LikeOS-64 -- mprotect.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/ke/smp.h>
#include <kernel/fs/icache.h>


static int64_t sys_mprotect_locked(uint64_t addr, uint64_t len, uint64_t prot)
{
	task_t *cur = sched_current();
	if (!cur) {
		return -ESRCH;
	}

	// Validate alignment
	if (addr & (PAGE_SIZE - 1)) {
		return -EINVAL;
	}

	// Round up length to page boundary
	uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

	// Build page flags
	uint64_t flags = PAGE_PRESENT | PAGE_USER;
	if (prot & 0x2) { // PROT_WRITE
		flags |= PAGE_WRITABLE;
	}
	if (!(prot & 0x4)) { // !PROT_EXEC
		flags |= PAGE_NO_EXECUTE;
	}

	// Update page table entries
	uint64_t *pml4 = cur->pml4;
	if (!pml4) {
		return -EFAULT;
	}

	/* Keep the region records describing the protection the pages actually
	 * have -- including when the range covers only PART of a region, which
	 * has to SPLIT it.
	 *
	 * This used to honour a full-region cover only, leaving a partial
	 * mprotect to change the page tables while the record kept the old
	 * protection for the whole span.  Two things follow, and the second is
	 * expensive:
	 *
	 *   - a lazy page in the changed part faults in with the RECORD's
	 *     protection, undoing the mprotect;
	 *   - the record still looks like its neighbours, so mappings that are
	 *     no longer alike merge with each other.  A thread stack is exactly
	 *     this shape -- one mmap of guard+stack, then mprotect(PROT_NONE)
	 *     over the guard alone -- so every stack stayed one RW record and
	 *     each new one coalesced onto the last.  The region count then
	 *     stays flat while the mapped total climbs by a stack per thread,
	 *     which reads as a leak and hides the guard page from the records
	 *     entirely.
	 *
	 * Split into up to three: the part before the range keeps the old
	 * protection, the covered part takes the new one, and any tail keeps
	 * the old.  Out of slots, the region is left whole with its protection
	 * unchanged -- the page tables below are still updated, which is the
	 * same partial state as before, but it is now the rare failure case
	 * rather than the normal path. */
	{
		task_t *mm = task_mm_owner(cur);
		uint64_t end = addr + pages * PAGE_SIZE;

		for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
			mmap_region_t *r = &mm->mmap_regions[i];
			uint64_t r_end;

			if (!r->in_use)
				continue;
			r_end = r->start + r->length;
			if (end <= r->start || addr >= r_end)
				continue; /* no overlap */

			if (addr <= r->start && end >= r_end) {
				r->prot = prot; /* whole region */
				continue;
			}

			/* Carve the tail off first, so `r' can then be trimmed
			 * to the head and the middle handled by the next loop
			 * iteration or by this one's own adjustment. */
			if (end < r_end) {
				size_t ridx = (size_t)(r - mm->mmap_regions);
				mmap_region_t *tail =
					mm_alloc_mmap_region(mm);

				/* Claiming a slot can grow -- and move -- the
				 * table, so the pointer is rebuilt. */
				r = &mm->mmap_regions[ridx];
				if (!tail)
					continue;
				*tail = *r;
				tail->start = end;
				tail->length = r_end - end;
				tail->offset = r->offset + (end - r->start);
				if (tail->file)
					vfs_incref(tail->file);
				tail->in_use = true;
				r->length = end - r->start;
				r_end = end;
			}

			if (addr > r->start) {
				size_t ridx = (size_t)(r - mm->mmap_regions);
				mmap_region_t *mid =
					mm_alloc_mmap_region(mm);

				r = &mm->mmap_regions[ridx];
				if (!mid)
					continue;
				*mid = *r;
				mid->start = addr;
				mid->length = r_end - addr;
				mid->offset = r->offset + (addr - r->start);
				mid->prot = prot;
				if (mid->file)
					vfs_incref(mid->file);
				mid->in_use = true;
				r->length = addr - r->start;
			} else {
				r->prot = prot;
			}
		}
	}

	for (uint64_t i = 0; i < pages; i++) {
		uint64_t vaddr = addr + i * PAGE_SIZE;
		uint64_t page_flags = flags;

		// Get current PTE
		uint64_t phys = mm_get_physical_address(vaddr);
		if (phys == 0) {
			// Page not mapped
			continue;
		}

		/* Device MMIO PTEs (/dev/fb0): the marker and caching bits
		 * must survive protection changes — losing PAGE_DEVICE would
		 * make a later unmap free BAR memory into the allocator. */
		{
			uint64_t *pte =
				mm_get_page_table_from_pml4(pml4, vaddr, false);
			if (pte && (*pte & PAGE_DEVICE))
				page_flags |= PAGE_DEVICE |
					      (*pte & (PAGE_WRITE_THROUGH |
						       PAGE_CACHE_DISABLE));
		}

		// Remap with new protection
		mm_map_page_in_address_space(pml4, vaddr, phys, flags);
	}

	// Flush TLB for modified pages on local CPU
	// Use virt_to_phys() for the PML4 pointer itself (not mm_get_physical_address)
	__asm__ volatile("mov %0, %%cr3" : : "r"(virt_to_phys(pml4)));

	// TLB shootdown: threads sharing this address space (CLONE_VM) may be running
	// on other CPUs with stale TLB entries. Broadcast invalidation to all CPUs.
	smp_tlb_shootdown_sync();

	return 0;
}


int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
	RUN_WRITE_LOCKED(sys_mprotect_locked(addr, len, prot));
}

