// LikeOS-64 -- mmap, munmap and brk.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/ke/pipe.h>
#include <kernel/fs/devfs.h>
#include <kernel/mm/shm.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/fs/file.h>

/* The mmap region table lives in the mm layer: mm_find_mmap_region(),
 * mm_alloc_mmap_region() and mm_unmap_range_and_regions() (kernel/mm/memory.c,
 * declared in kernel/mm/memory.h). */

static int64_t sys_brk_locked(uint64_t new_brk)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	/* Threads share the leader's heap — all brk bookkeeping (and the
	 * fault handler's validity check) goes through the group leader. */
	cur = task_mm_owner(cur);

	// If new_brk is 0, return current break
	if (new_brk == 0) {
		return (int64_t)cur->brk;
	}

	// Validate new break is reasonable
	if (new_brk < cur->brk_start) {
		return (int64_t)cur->brk; // Can't shrink below start
	}

	// Don't let heap grow into stack area
	if (new_brk >= cur->user_stack_top - (2 * 1024 * 1024)) {
		return (int64_t)cur->brk; // Would collide with stack
	}

	/* Growing the heap: demand-paged — no pages are allocated here.  The
	 * page-fault handler zero-fills anything in [brk_start, brk) on first
	 * touch, so growing the break is just bookkeeping.  Shrinking keeps
	 * the pages mapped (as before). */
	cur->brk = new_brk;
	return (int64_t)new_brk;
}

int64_t sys_brk(uint64_t new_brk)
{
	RUN_WRITE_LOCKED(sys_brk_locked(new_brk));
}

// SYS_MMAP - map memory
static int64_t sys_mmap_locked(uint64_t addr, uint64_t length, uint64_t prot,
			       uint64_t flags, uint64_t fd, uint64_t offset)
{
	/* One exit, so the backing file's hold is released however this
	 * answers.  Anonymous mappings leave it NULL and release nothing. */
	vfs_file_t *backing = NULL;
	int64_t ret;

	task_t *cur = sched_current();
	if (!cur) {
		ret = -ENOMEM;
		goto out;
	}
	/* fd lookup below stays on the calling task; the region table,
	 * mmap_base and pml4 belong to the thread-group leader. */
	task_t *caller = cur;
	cur = task_mm_owner(cur);

	// Security: Validate length - must be non-zero and reasonable
	if (length == 0) {
		ret = -EINVAL;
		goto out;
	}

	// Security: Prevent integer overflow when aligning length
	if (length > 0x7FFFFFFFFFFFFFF0ULL) {
		return -ENOMEM; // Would overflow during PAGE_ALIGN
	}

	// Round up length to page size
	length = PAGE_ALIGN(length);

	// Security: Prevent excessive allocation (max 2GB per mmap call)
	if (length > (2ULL * 1024 * 1024 * 1024)) {
		ret = -ENOMEM;
		goto out;
	}

	// Find a free mmap region slot
	mmap_region_t *region = mm_alloc_mmap_region(cur);
	if (!region) {
		/* Loud on purpose: a process that runs out of region slots
		 * fails every subsequent mmap, which downstream looks like a
		 * random allocation crash rather than a table limit -- a
		 * dlopen() failing here is reported by the loader as "cannot
		 * find", which sends the reader looking for a missing file.
		 *
		 * The breakdown says WHY the table is full, which the bare
		 * count does not: file-backed entries are libraries and their
		 * segments (four per shared object, so a large dependency graph
		 * alone accounts for hundreds), while anonymous ones are heap,
		 * thread stacks and large allocations.  Whichever dominates is
		 * where to look. */
		int n_file = 0, n_anon = 0, n_lazy = 0;
		uint64_t anon_bytes = 0;

		for (uint32_t i = 0; i < cur->mmap_capacity; i++) {
			mmap_region_t *r = &cur->mmap_regions[i];

			if (!r->in_use)
				continue;
			if (r->file) {
				n_file++;
			} else {
				n_anon++;
				anon_bytes += r->length;
			}
			if (r->lazy)
				n_lazy++;
		}
		WARN_RATELIMIT(
			1,
			"mmap: pid %d out of mmap regions (max %d): %d file-backed, %d anonymous (%llu KB), %d lazy",
			cur->id, TASK_MAX_MMAP, n_file, n_anon,
			(unsigned long long)(anon_bytes / 1024), n_lazy);
		ret = -ENOMEM;
		goto out;
	}

	// Determine virtual address
	uint64_t vaddr;
	if (flags & MAP_FIXED) {
		if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
			return -EINVAL; // Invalid fixed address
		}
		// Security: Reject mappings below 64KB to prevent NULL deref exploits
		if (addr < 0x10000) {
			ret = -EINVAL;
			goto out;
		}
		vaddr = addr;

		/* MAP_FIXED replaces whatever is already here, so tear the old
		 * mapping down properly: free its pages AND release its region
		 * records.  Doing this for every MAP_FIXED path (rather than
		 * only the lazy one, which is all that used to unmap anything)
		 * is what keeps the region table from filling up -- rtld maps
		 * every DSO segment this way.  It also stops a stale record
		 * from shadowing the new mapping in mm_find_mmap_region(), which
		 * would report the old file and protection for these pages. */
		mm_unmap_range_and_regions(cur, vaddr, length);
	} else {
		// Allocate from mmap area (grows down from below stack)
		// Move base down first, then return the new base as the start of the mapped region
		cur->mmap_base -= length;
		if (cur->mmap_base < cur->brk + (4 * 1024 * 1024)) {
			// Too close to heap
			cur->mmap_base += length; // Rollback
			ret = -ENOMEM;
			goto out;
		}
		// Security: Reject mappings below 64KB to prevent NULL deref exploits
		if (cur->mmap_base < 0x10000) {
			cur->mmap_base += length; // Rollback
			ret = -ENOMEM;
			goto out;
		}
		vaddr = cur->mmap_base;
	}

	// Calculate page flags
	uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
	if (prot & PROT_WRITE) {
		page_flags |= PAGE_WRITABLE;
	}
	if (!(prot & PROT_EXEC)) {
		page_flags |= PAGE_NO_EXECUTE;
	}

	bool is_anonymous = (flags & MAP_ANONYMOUS) || (int64_t)fd == -1;

	/* Resolve and validate the backing file up front (also needed for the
	 * lazy path).  Only real VFS files can back a mapping — socket/pipe/
	 * epoll fd markers and stdio placeholders cannot. */
	if (!is_anonymous) {
		/* Held for the rest of the call.  The region records built
		 * below take their own reference, but between this lookup and
		 * that point the descriptor can be closed by a sibling thread
		 * -- and then the reference is taken on a file that has already
		 * been released. */
		backing = fdget(caller, (int)fd);
		if (!backing) {
			ret = -EBADF;
			goto out;
		}
		if (fd_is_special(backing)) {
			ret = -ENODEV;
			goto out;
		}
	}

	/* Device mapping: /dev/fb0 maps the framebuffer BAR itself.  Pages
	 * are mapped eagerly with PAGE_DEVICE PTEs (never freed back to the
	 * physical allocator, shared across fork) and write-combining
	 * caching (PWT selects PAT entry 1, programmed WC at boot). */
	/* Shared memory: map the OBJECT's own physical pages.  This is the one
	 * mapping in the system that is shared between processes with no fork
	 * relationship — the generic MAP_SHARED path below allocates fresh
	 * pages per process, which is fine for anonymous memory but would give
	 * every opener of a /dev/shm object its own private copy. */
	if (backing) {
		shm_object_t *sobj = devfs_shm_object(backing);
		if (sobj) {
			if (!(flags & MAP_SHARED)) {
				/* A private mapping of shared memory is legal
				 * but pointless here, and implementing it means
				 * copy-on-write over borrowed pages.  Say so
				 * rather than silently sharing. */
				ret = -EOPNOTSUPP;
				goto out;
			}
			if ((offset & (PAGE_SIZE - 1)) != 0) {
				ret = -EINVAL;
				goto out;
			}
			/* Bound the mapping by the object's PAGE span, not its
			 * byte length.  `length` reaches this point already
			 * rounded up to whole pages, but ftruncate() records
			 * the exact byte size the caller set -- so comparing
			 * the two refused every object whose size was not a
			 * page multiple.  shm_set_size() allocates
			 * ceil(size / PAGE_SIZE) pages, so the partial last
			 * page is real, and POSIX maps it (zero tail included).
			 * WebKit sizes its IPC message bodies to the byte,
			 * which is how an https response (> 4 KB encoded, so
			 * out-of-line in shared memory) could never be mapped
			 * by its own sender, while http responses stayed under
			 * the inline limit and never touched this path. */
			uint64_t obj_span = (sobj->size + PAGE_SIZE - 1) &
					    ~(uint64_t)(PAGE_SIZE - 1);
			if (offset + length > obj_span)
				return -EINVAL; /* past the object's pages */

			/* PAGE_DEVICE marks these as pages this address space
			 * does not own: teardown must not hand them back to the
			 * physical allocator, and fork shares rather than copies
			 * them.  They belong to the shm object and are released
			 * only when it is destroyed.  Without this bit every
			 * munmap frees memory the object still owns — the pages
			 * get reused underneath it and are freed a second time
			 * later. */
			uint64_t shm_flags = page_flags | PAGE_DEVICE;

			for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
				uint64_t phys = shm_page_phys(
					sobj, (offset + off) / PAGE_SIZE);
				if (!phys || !mm_map_page_in_address_space(
						     cur->pml4, vaddr + off,
						     phys, shm_flags)) {
					for (uint64_t cl = 0; cl < off;
					     cl += PAGE_SIZE)
						mm_unmap_page_in_address_space(
							cur->pml4, vaddr + cl);
					if (!(flags & MAP_FIXED))
						cur->mmap_base += length;
					ret = -ENOMEM;
					goto out;
				}
			}
			/* Pin the object for the life of the mapping: the
			 * region holds a vfs reference, and unmapping releases
			 * it through the normal file teardown. */
			vfs_incref(backing);
			region->start = vaddr;
			region->length = length;
			region->prot = prot;
			region->flags = flags | MAP_SHARED;
			region->fd = (int)fd;
			region->offset = offset;
			region->lazy = false;
			region->file = backing;
			/* Marked as a device mapping so fork() shares the pages
			 * instead of copying them, and teardown never hands
			 * them back to the physical allocator — they belong to
			 * the shm object, not to this address space. */
			region->device = true;
			region->device_phys =
				shm_page_phys(sobj, offset / PAGE_SIZE);
			region->in_use = true;
			mm_merge_region_neighbours(cur, region);
			ret = (int64_t)vaddr;
			goto out;
		}
	}

	if (backing && devfs_is_fb0(backing)) {
		uint64_t dev_phys = fbdev_mmap_phys(offset, length);
		uint64_t dev_flags =
			page_flags | PAGE_DEVICE | PAGE_WRITE_THROUGH;

		if (!dev_phys) {
			if (!(flags & MAP_FIXED))
				cur->mmap_base += length; // Rollback
			ret = -ENODEV;
			goto out;
		}
		for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
			if (!mm_map_page_in_address_space(
				    cur->pml4, vaddr + off, dev_phys + off,
				    dev_flags)) {
				for (uint64_t cl = 0; cl < off; cl += PAGE_SIZE)
					mm_unmap_page_in_address_space(
						cur->pml4, vaddr + cl);
				if (!(flags & MAP_FIXED))
					cur->mmap_base += length; // Rollback
				ret = -ENOMEM;
				goto out;
			}
		}
		vfs_incref(backing);
		region->start = vaddr;
		region->length = length;
		region->prot = prot;
		/* Force shared semantics: fork must share the device pages,
		 * never COW them. */
		region->flags = flags | MAP_SHARED;
		region->fd = (int)fd;
		region->offset = offset;
		region->lazy = false;
		region->file = backing;
		region->device = true;
		region->device_phys = dev_phys;
		region->in_use = true;
		mm_merge_region_neighbours(cur, region);
		ret = (int64_t)vaddr;
		goto out;
	}

	/* Demand paging: PRIVATE mappings (anonymous or file-backed) are not
	 * populated here at all — the page-fault handler materialises pages
	 * on first touch (zero-fill / file page-in).  Only MAP_SHARED stays
	 * eager: fork() must find real pages to share.
	 *
	 * MAP_FIXED over an existing mapping must not leave stale pages in
	 * place (a lazy region would otherwise never fault there and expose
	 * the old contents).  That teardown now happens for every MAP_FIXED
	 * path where vaddr is settled, above, which also stops the eager path
	 * from silently overwriting live PTEs and leaking the pages. */
	/* PROT_NONE takes the lazy path even when MAP_SHARED is asked for: the
	 * mapping has no accessible contents, so there is nothing for the eager
	 * path to share, and the fault handler above already refuses PROT_NONE
	 * regions.  Mapping it eagerly would hand out PAGE_PRESENT|PAGE_USER
	 * pages that are freely readable — i.e. PROT_NONE would not protect. */
	bool prot_none = !(prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
	if (!(flags & MAP_SHARED) || prot_none) {
		/* No unmap here: MAP_FIXED already tore down the old mapping,
		 * pages and region records both, where vaddr was settled. */
		if (backing)
			vfs_incref(backing);
		region->start = vaddr;
		region->length = length;
		region->prot = prot;
		region->flags = flags;
		region->fd = is_anonymous ? -1 : (int)fd;
		region->offset = offset;
		region->lazy = true;
		region->file = backing;
		region->in_use = true;
		mm_merge_region_neighbours(cur, region);
		ret = (int64_t)vaddr;
		goto out;
	}

	// Map pages (eager, MAP_SHARED only)
	uint64_t pages_mapped = 0;

	for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
		uint64_t phys = mm_allocate_physical_page();
		if (!phys) {
			// Unmap already-mapped pages on failure
			for (uint64_t cleanup = 0; cleanup < off;
			     cleanup += PAGE_SIZE) {
				mm_unmap_page_in_address_space(cur->pml4,
							       vaddr + cleanup);
			}
			if (!(flags & MAP_FIXED)) {
				cur->mmap_base += length; // Rollback
			}
			ret = -ENOMEM;
			goto out;
		}

		/* No memset in production: mm_allocate_physical_page already
		 * zeroed the page (double-zeroing every anonymous page slowed
		 * each process start — rtld/malloc mmap hundreds of pages).
		 * DEBUG builds poison on alloc, so zero explicitly there:
		 * anon mmap pages must read as zero in userspace. */
#if DEBUG
		mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
#endif

		// For file-backed mappings, read content from file
		if (backing) {
			// Seek to the correct position and read into direct-mapped address
			long file_off = (long)(offset + off);
			if (vfs_seek(backing, file_off, SEEK_SET) >= 0) {
				vfs_read(backing, phys_to_virt(phys),
					 PAGE_SIZE);
			}
		}

		if (!mm_map_page_in_address_space(cur->pml4, vaddr + off, phys,
						  page_flags)) {
			mm_free_physical_page(phys);
			// Unmap already-mapped pages on failure
			for (uint64_t cleanup = 0; cleanup < off;
			     cleanup += PAGE_SIZE) {
				mm_unmap_page_in_address_space(cur->pml4,
							       vaddr + cleanup);
			}
			if (!(flags & MAP_FIXED)) {
				cur->mmap_base += length; // Rollback
			}
			ret = -ENOMEM;
			goto out;
		}
		pages_mapped++;
	}

	// Record the mapping
	region->start = vaddr;
	region->length = length;
	region->prot = prot;
	region->flags = flags;
	region->fd = is_anonymous ? -1 : (int)fd;
	region->offset = offset;
	region->lazy = false;
	region->file = NULL;
	region->in_use = true;
	mm_merge_region_neighbours(cur, region);

	ret = (int64_t)vaddr;
	goto out;

out:
	if (backing)
		fdput(backing);
	return ret;
}

int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
		 uint64_t flags, uint64_t fd, uint64_t offset)
{
	RUN_WRITE_LOCKED(
		sys_mmap_locked(addr, length, prot, flags, fd, offset));
}

// SYS_MUNMAP - unmap memory
static int64_t sys_munmap_locked(uint64_t addr, uint64_t length)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	cur = task_mm_owner(cur); // regions/pml4 live on the group leader

	if (addr == 0 || length == 0) {
		return -EINVAL;
	}
	if (addr & (PAGE_SIZE - 1)) {
		return -EINVAL;
	}

	length = PAGE_ALIGN(length);

	/* Handles the common case of a single munmap call covering multiple
	 * contiguous MAP_FIXED regions (e.g. the full span of a DSO). */
	return mm_unmap_range_and_regions(cur, addr, length) ? 0 : -EINVAL;
}

int64_t sys_munmap(uint64_t addr, uint64_t length)
{
	RUN_WRITE_LOCKED(sys_munmap_locked(addr, length));
}
