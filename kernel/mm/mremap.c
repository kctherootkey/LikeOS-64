// LikeOS-64 -- mremap(2): resize or move a mapping without copying pages.
//
// A growing heap or hash table wants more address space at the same place
// if that is free, and somewhere else otherwise -- either way keeping the
// pages it already has rather than copying them.  Growing in place extends
// the region record; moving carries the page-table entries over to the new
// address one by one and unlinks them from the old, so the physical pages
// change address without ever being touched.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/smp.h>

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
#define MREMAP_DONTUNMAP 4

static int range_is_free(task_t *cur, uint64_t start, uint64_t len)
{
	for (uint32_t i = 0; i < cur->mmap_capacity; i++) {
		mmap_region_t *r = &cur->mmap_regions[i];

		if (!r->in_use)
			continue;
		if (start < r->start + r->length && r->start < start + len)
			return 0;
	}
	return 1;
}

/* Carry [old, old+len) to `new': every present PTE is re-created at the
 * new address with the same bits and cleared at the old one.  Pages that
 * were never faulted in stay absent, which the new region's `lazy' flag
 * accounts for. */
static void move_ptes(task_t *cur, uint64_t old, uint64_t new, uint64_t len)
{
	for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
		uint64_t *pte =
			mm_get_page_table_from_pml4(cur->pml4, old + off, false);

		if (!pte || !(*pte & PAGE_PRESENT))
			continue;
		uint64_t entry = *pte;

		mm_map_page_in_address_space(cur->pml4, new + off,
					     entry & PTE_ADDR_MASK,
					     entry & ~PTE_ADDR_MASK);
		*pte = 0;
	}
	/* Both ranges changed on every CPU running this address space. */
	smp_tlb_shootdown_mm_sync(virt_to_phys(cur->pml4));
}

static int64_t sys_mremap_locked(uint64_t old_addr, uint64_t old_size,
				 uint64_t new_size, uint64_t flags,
				 uint64_t new_addr)
{
	task_t *cur = task_mm_owner(sched_current());

	if (!cur)
		return -EFAULT;
	if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
		return -EINVAL;
	if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
		return -EINVAL;
	if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
		return -EINVAL;
	if (old_addr & (PAGE_SIZE - 1))
		return -EINVAL;
	old_size = PAGE_ALIGN(old_size);
	new_size = PAGE_ALIGN(new_size);
	if (new_size == 0)
		return -EINVAL;
	if (old_addr + old_size < old_addr || old_addr + new_size < old_addr)
		return -EINVAL;

	mmap_region_t *r = mm_find_mmap_region(cur, old_addr);

	if (!r || r->start > old_addr)
		return -EFAULT;
	/* The range must lie within one record (a split record is two
	 * mappings as far as this call is concerned). */
	if (old_size && old_addr + old_size > r->start + r->length)
		return -EFAULT;
	if (r->device)
		return -EINVAL; /* device windows do not move */
	if ((flags & MREMAP_DONTUNMAP) && (r->file || !(r->flags & MAP_PRIVATE)))
		return -EINVAL;

	/* Shrinking in place: give back the tail. */
	if (!(flags & MREMAP_FIXED) && new_size <= old_size) {
		if (new_size < old_size)
			mm_unmap_range_and_regions(cur, old_addr + new_size,
						   old_size - new_size);
		return (int64_t)old_addr;
	}

	uint64_t grow = new_size - old_size;

	/* Growing in place: the record ends at old_addr+old_size and the
	 * space beyond it is unclaimed. */
	if (!(flags & MREMAP_FIXED) &&
	    old_addr + old_size == r->start + r->length &&
	    range_is_free(cur, old_addr + old_size, grow) &&
	    old_addr + new_size < 0x0000800000000000ULL) {
		r->length += grow;
		/* The new tail has no pages: it must be demand-faulted. */
		r->lazy = true;
		mm_merge_region_neighbours(cur, r);
		return (int64_t)old_addr;
	}

	if (!(flags & MREMAP_MAYMOVE))
		return -ENOMEM;

	/* Moving.  Pick the destination. */
	uint64_t dest;

	if (flags & MREMAP_FIXED) {
		if (new_addr & (PAGE_SIZE - 1))
			return -EINVAL;
		if (new_addr < 0x10000 || new_addr + new_size < new_addr ||
		    new_addr + new_size > 0x0000800000000000ULL)
			return -EINVAL;
		if (new_addr < old_addr + old_size && old_addr < new_addr + new_size)
			return -EINVAL; /* overlaps the source */
		mm_unmap_range_and_regions(cur, new_addr, new_size);
		dest = new_addr;
	} else {
		cur->mmap_base -= new_size;
		if (cur->mmap_base < cur->brk + (4 * 1024 * 1024) ||
		    cur->mmap_base < 0x10000) {
			cur->mmap_base += new_size;
			return -ENOMEM;
		}
		dest = cur->mmap_base;
	}

	/* A fresh record for the destination, carrying the source's
	 * attributes; claimed only now, after the teardown above, for the
	 * reason sys_mmap gives. */
	mmap_region_t *nr = mm_alloc_mmap_region(cur);

	if (!nr) {
		if (!(flags & MREMAP_FIXED))
			cur->mmap_base += new_size;
		return -ENOMEM;
	}
	/* mm_alloc_mmap_region may have grown the table; re-find the source. */
	r = mm_find_mmap_region(cur, old_addr);
	if (!r) {
		if (!(flags & MREMAP_FIXED))
			cur->mmap_base += new_size;
		return -EFAULT;
	}

	nr->start = dest;
	nr->length = new_size;
	nr->prot = r->prot;
	nr->flags = r->flags;
	nr->fd = r->fd;
	nr->offset = r->offset + (old_addr - r->start);
	nr->lazy = true; /* any part not carried over faults on demand */
	nr->file = r->file;
	nr->device = false;
	nr->device_phys = 0;
	nr->dev_obj = NULL;
	mm_region_ref_hold(nr);
	nr->in_use = true;

	uint64_t carry = old_size < new_size ? old_size : new_size;

	move_ptes(cur, old_addr, dest, carry);

	/* The source range: gone (its PTEs are already cleared, so only the
	 * record is released), or kept as an empty mapping with
	 * MREMAP_DONTUNMAP. */
	if (old_size && !(flags & MREMAP_DONTUNMAP))
		mm_unmap_range_and_regions(cur, old_addr, old_size);
	else if (old_size)
		r->lazy = true;

	return (int64_t)dest;
}

int64_t sys_mremap(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
		   uint64_t flags, uint64_t new_addr)
{
	RUN_WRITE_LOCKED(sys_mremap_locked(old_addr, old_size, new_size, flags,
					   new_addr));
}
