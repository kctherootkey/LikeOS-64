// LikeOS-64 Memory Management - Implementation
// Complete Physical Memory Manager, Virtual Memory Manager, and Kernel Heap Allocator

#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/types.h>
#include <kernel/ke/smp.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/sched.h> // For spinlock_t
#include <kernel/mm/rwsem.h> // address-space semaphore
#include <kernel/uapi/bug.h>
#include <kernel/fs/pagecache.h> // pagecache_get_stats for memstat breakdown
#include <kernel/fs/vfs.h> // demand paging: file-backed page-in
#include <kernel/ke/syscall.h> // PROT_* for lazy region protection
#include <kernel/ke/timer.h> // timer_get_precise_us: the slow-fault detector

// Enable SLAB allocator (comment out to use legacy fixed-size heap)
#define USE_SLAB_ALLOCATOR

// ============================================================================
// MEMORY POISONING
// ============================================================================
// Active when compiled with DEBUG=1.  Invalid accesses to freed or
// uninitialized memory become immediately obvious.
//
//   POISON_FREED_PAGE  0xFEEDFACE – physical page returned to free pool
//   POISON_UNINIT_PAGE 0xCCCCCCCC – newly allocated physical page (debug)
//
// Slab-level patterns (0xDEADBEEF freed object, 0xCCCCCCCC new object)
// are defined and applied in slab.c.
#define POISON_FREED_PAGE 0xFEEDFACEU
#define POISON_UNINIT_PAGE 0xCCCCCCCCU

/* Fill *bytes* bytes at *dest* with a repeating 32-bit *pattern*.
 *
 * Debug builds only, and every caller is gated to match.  Poisoning on the
 * release paths used to be unconditional, which cost a write of every byte
 * being freed -- close to a gigabyte for a large process on its way out, all
 * of it inside the physical allocator's global lock with interrupts disabled,
 * and none of it ever read back in a production build. */
#if DEBUG
static void mm_poison_fill(void *dest, uint32_t pattern, size_t bytes)
{
	uint8_t *ptr = (uint8_t *)dest;

	/* Fast path: 8-byte-aligned destination (typical: page / slab callers) */
	if (bytes >= 8 && ((uintptr_t)ptr & 7) == 0) {
		uint64_t pat64 = ((uint64_t)pattern << 32) | pattern;
		uint64_t *p64 = (uint64_t *)ptr;
		size_t words = bytes / 8;
		size_t rem = bytes % 8;

		if (words >= 64) {
			/* rep stosq: CPU's optimised string-store path */
			__asm__ volatile("rep stosq"
					 : "+D"(p64), "+c"(words)
					 : "a"(pat64)
					 : "memory");
		} else {
			/* Unrolled 4×64-bit loop for medium sizes */
			while (words >= 4) {
				p64[0] = pat64;
				p64[1] = pat64;
				p64[2] = pat64;
				p64[3] = pat64;
				p64 += 4;
				words -= 4;
			}
			while (words--)
				*p64++ = pat64;
		}
		ptr = (uint8_t *)p64;
		bytes = rem;
	}

	/* Fallback: 32-bit stores then byte tail for any remainder */
	uint32_t *p32 = (uint32_t *)ptr;
	size_t n32 = bytes >> 2;
	for (size_t i = 0; i < n32; i++)
		p32[i] = pattern;
	uint8_t *t = (uint8_t *)(p32 + n32);
	uint8_t *b = (uint8_t *)&pattern;
	switch (bytes & 3) {
	case 3:
		t[2] = b[2]; /* fall-through */
	case 2:
		t[1] = b[1]; /* fall-through */
	case 1:
		t[0] = b[0];
		break;
	default:
		break;
	}
}
#endif /* DEBUG */

// ============================================================================
// SMP LOCKING
// ============================================================================
// Spinlock for physical memory allocator (bitmap access)
static spinlock_t mm_phys_lock = SPINLOCK_INIT("mm_phys");
// Spinlock for page table pool
static spinlock_t mm_pt_pool_lock = SPINLOCK_INIT("mm_pt_pool");
// Spinlock for kernel page table modifications (mm_map_page / mm_unmap_page).
// On SMP, two CPUs calling mm_get_page_table(create=true) simultaneously can
// race when creating intermediate page table levels (PDPT/PD/PT), causing one
// CPU's newly-allocated level to be silently overwritten by the other.
static spinlock_t mm_kernel_pt_lock = SPINLOCK_INIT("mm_kpt");
// Spinlock for page refcount operations (COW safety on SMP)
static spinlock_t mm_refcount_lock = SPINLOCK_INIT("mm_refcount");

// Kernel PML4 - saved at init time, used when destroying current address space
static uint64_t g_kernel_pml4_phys = 0;

// ============================================================================
// KERNEL STACK GUARD PAGE ALLOCATOR — static state
// ============================================================================
#define KSTACK_MAX_RECYCLED 256

static uint64_t kstack_virt_next = KSTACK_VIRT_BASE;
static uint64_t kstack_recycled[KSTACK_MAX_RECYCLED];
static unsigned int kstack_recycled_count = 0;
static spinlock_t kstack_virt_lock = SPINLOCK_INIT("kstack_virt");

// Forward declaration for page_to_index (used in COW handler before definition)
static inline uint64_t page_to_index(uint64_t phys_addr);

// Magic numbers for heap validation
#define HEAP_MAGIC_ALLOCATED 0xDEADBEEF
#define HEAP_MAGIC_FREE 0xFEEDFACE
#define HEAP_MAGIC_HEADER 0xABCDEF12

// Memory management state
/* ==========================================================================
 * Leak-hunt instrumentation (DEBUG builds only)
 *
 * Records which call site allocated each physical page, and tracks every live
 * address space by its creator, so "memory is disappearing" can be answered
 * with a name instead of a guess.  It found the exit-teardown leak after four
 * wrong guesses had been made without it.
 *
 * Costs, and why it is not on by default: 4 bytes of RAM per physical page for
 * the owner table (~4 MB on a 4 GB machine), a 64 KB registry, and a lock
 * acquisition on every address-space create and destroy.
 *
 * Build with `make DEBUG=1` and read it with `memstat -o`.
 * ========================================================================== */
#ifndef MM_LEAK_INSTRUMENTATION
#define MM_LEAK_INSTRUMENTATION DEBUG
#endif

static struct {
	// Physical memory management
	uint32_t *physical_bitmap; // Bitmap for physical pages
	uint64_t total_pages; // Pages SPANNED by the managed range (incl. holes)
	uint64_t free_pages; // Number of free pages
	/* Pages of real RAM under management, i.e. the span above minus the
	 * holes in it.  Physical memory is not one run: firmware leaves a PCI
	 * hole below 4 GB and continues RAM above it, so the span is far bigger
	 * than the RAM in it and is the wrong thing to report or subtract
	 * from. */
	uint64_t usable_pages;
	/* Who allocated each page: the low 32 bits of the return address of the
	 * mm_allocate_physical_page() caller.  Pure diagnostics -- when memory
	 * drains and every process is gone, the only useful question is which
	 * code path is still holding it, and guessing at that has a poor record.
	 * 4 bytes per page, ~0.1% of RAM. */
#if MM_LEAK_INSTRUMENTATION
	uint32_t *page_owner;
	uint64_t owner_array_size;
#endif
	uint64_t bitmap_size; // Size of bitmap in bytes
	uint64_t memory_start; // Start of manageable memory
	uint64_t memory_end; // End of manageable memory

	// Page reference counting for COW
	uint16_t *page_refcounts; // Reference count per physical page
	uint64_t refcount_array_size; // Size in bytes

	// Virtual memory management
	uint64_t *pml4_table; // Page Map Level 4 table
	uint64_t next_virtual_addr; // Next available virtual address

	// Heap management
	heap_block_t *heap_start; // Start of heap
	heap_block_t *heap_end; // End of heap
	heap_block_t *free_list; // Free blocks list
	uint64_t heap_size; // Total heap size
	uint64_t heap_used; // Used heap memory
	uint32_t allocation_count; // Number of allocations
	uint32_t deallocation_count; // Number of deallocations

	// Statistics
	memory_stats_t stats; // Memory statistics
} mm_state = { 0 };

#if MM_LEAK_INSTRUMENTATION
#define LEAK_INC(v) __sync_fetch_and_add(&(v), 1)
#define LEAK_ADD(v, n) __sync_fetch_and_add(&(v), (unsigned long)(n))
#define PAGE_OWNER_SET(pg, ra)                                   \
	do {                                                     \
		if (mm_state.page_owner)                         \
			mm_state.page_owner[pg] = (uint32_t)(uintptr_t)(ra); \
	} while (0)
#define AS_TRACK_ADD(phys, creator) as_track_add((phys), (creator))
#define AS_TRACK_DEL(phys) as_track_del(phys)
#define AS_TRACK_RETAG(phys, creator) as_track_retag((phys), (creator))
#else
#define LEAK_INC(v) do { } while (0)
#define LEAK_ADD(v, n) do { } while (0)
#define PAGE_OWNER_SET(pg, ra) do { } while (0)
#define AS_TRACK_ADD(phys, creator) do { } while (0)
#define AS_TRACK_DEL(phys) do { } while (0)
#define AS_TRACK_RETAG(phys, creator) do { } while (0)
#endif



// UEFI memory map storage - saved from boot_info for later use
static memory_map_info_t g_uefi_memory_map = { 0 };

// Utility functions (non-static, declared in memory.h)
void mm_memset(void *dest, int val, size_t len)
{
	BUG_ON(dest == NULL);
	uint8_t *ptr = (uint8_t *)dest;
	uint8_t byte_val = (uint8_t)val;

	// Fast path: for larger sizes, use 64-bit stores
	if (len >= 32 && ((uint64_t)ptr & 7) == 0) {
		// Create 64-bit pattern from byte value
		uint64_t pattern = byte_val;
		pattern |= pattern << 8;
		pattern |= pattern << 16;
		pattern |= pattern << 32;

		uint64_t *ptr64 = (uint64_t *)ptr;
		size_t words = len / 8;
		size_t remainder = len % 8;

		// Use rep stosq for very large fills (uses CPU's optimized path)
		if (words >= 64) {
			__asm__ volatile("rep stosq"
					 : "+D"(ptr64), "+c"(words)
					 : "a"(pattern)
					 : "memory");
			ptr = (uint8_t *)ptr64;
			len = remainder;
		} else {
			// Manual unrolled loop for medium sizes
			while (words >= 4) {
				ptr64[0] = pattern;
				ptr64[1] = pattern;
				ptr64[2] = pattern;
				ptr64[3] = pattern;
				ptr64 += 4;
				words -= 4;
			}
			while (words--) {
				*ptr64++ = pattern;
			}
			ptr = (uint8_t *)ptr64;
			len = remainder;
		}
	}

	// Handle remaining bytes
	while (len--) {
		*ptr++ = byte_val;
	}
}

void mm_memcpy(void *dest, const void *src, size_t len)
{
	BUG_ON(dest == NULL || src == NULL);
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;

	// Fast path: use rep movsb for larger aligned copies
	if (len >= 64) {
		__asm__ volatile("rep movsb"
				 : "+D"(d), "+S"(s), "+c"(len)
				 :
				 : "memory");
		return;
	}

	while (len--) {
		*d++ = *s++;
	}
}

// Get current CR3 (page table base)
static uint64_t get_cr3(void)
{
	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	return cr3;
}

// Set CR3 (page table base)
static void set_cr3(uint64_t cr3)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// Flush TLB for specific address (SMP-safe: flushes on all CPUs)
void mm_flush_tlb(uint64_t virtual_addr)
{
	WARN_ON(virtual_addr == 0);
	__asm__ volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
	// SMP note: cross-CPU TLB invalidation is NOT done here.
	// User pages: per-process CR3 means only the local CPU needs invlpg.
	// Kernel SLAB pages: slab_free() calls smp_tlb_shootdown_sync()
	// explicitly after unmapping, before recycling virtual addresses.
	// Doing broadcast IPIs here caused an IPI storm (every COW fault
	// would shootdown all CPUs, flushing their entire TLBs).
}

// Flush all TLB entries (local CPU only)
// Callers: boot-time NX remapping, address-space cloning (per-process)
// — neither requires cross-CPU invalidation.
__no_stack_protector void mm_flush_all_tlb(void)
{
	uint64_t cr3 = get_cr3();
	set_cr3(cr3);
}

// Get dynamic kernel heap start address
uint64_t mm_get_kernel_heap_start(void)
{
	// Align kernel_end to page boundary and add some padding
	uint64_t kernel_end_addr = (uint64_t)kernel_end;
	return PAGE_ALIGN(kernel_end_addr);
}

// PHYSICAL MEMORY MANAGER IMPLEMENTATION

// Forward declarations
static bool is_page_allocated(uint64_t page);
static void set_page_bit(uint64_t page);
static void clear_page_bit(uint64_t page);

// Page table pool - Reserved from start of physical memory range.
// All page tables are allocated from this pool, which is outside the bitmap range.
// Size is calculated dynamically based on RAM: ~1 PT page per 2MB of RAM (worst case)
static uint64_t pt_pool_size = 0; // Calculated at runtime
static uint64_t pt_pool_phys_start = 0;
static uint64_t pt_pool_next = 0;
static uint64_t pt_pool_freelist = 0; // Phys addr of first free recycled page
static int pt_pool_initialized = 0;

// ============================================================================
// EARLY PAGE TABLE WALKING (before full VM is initialized)
// Walk the bootloader's page tables to find physical addresses for virtual addresses
// This does NOT use mm_get_page_table (which needs allocation) - read only!
// ============================================================================

// Get physical address for virtual address by walking page tables (read-only, early boot)
// Returns 0 if the address is not mapped
static uint64_t early_virt_to_phys(uint64_t virtual_addr)
{
	uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
	uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
	uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
	uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;

	// Get PML4 from CR3
	uint64_t pml4_phys = get_cr3() & ~0xFFF;
	BUG_ON(pml4_phys == 0);
	uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

	// Check PML4 entry
	uint64_t pml4e = pml4[pml4_index];
	if (!(pml4e & PAGE_PRESENT)) {
		return 0; // Not mapped
	}

	// Get PDPT
	uint64_t pdpt_phys = pml4e & PTE_ADDR_MASK;
	uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

	// Check PDPT entry
	uint64_t pdpte = pdpt[pdpt_index];
	if (!(pdpte & PAGE_PRESENT)) {
		return 0; // Not mapped
	}

	// Check for 1GB page
	if (pdpte & PAGE_SIZE_FLAG) {
		// 1GB page - physical address is pdpte[51:30] + vaddr[29:0]
		return (pdpte & 0x000FFFFFC0000000ULL) |
		       (virtual_addr & 0x3FFFFFFF);
	}

	// Get PD
	uint64_t pd_phys = pdpte & PTE_ADDR_MASK;
	uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);

	// Check PD entry
	uint64_t pde = pd[pd_index];
	if (!(pde & PAGE_PRESENT)) {
		return 0; // Not mapped
	}

	// Check for 2MB page
	if (pde & PAGE_SIZE_FLAG) {
		// 2MB page - physical address is pde[51:21] + vaddr[20:0]
		return (pde & 0x000FFFFFFFE00000ULL) |
		       (virtual_addr & 0x1FFFFF);
	}

	// Get PT
	uint64_t pt_phys = pde & PTE_ADDR_MASK;
	uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);

	// Check PT entry
	uint64_t pte = pt[pt_index];
	if (!(pte & PAGE_PRESENT)) {
		return 0; // Not mapped
	}

	// 4KB page - physical address is pte[51:12] + vaddr[11:0]
	return (pte & 0x000FFFFFFFFFF000ULL) | (virtual_addr & 0xFFF);
}

// Reserve a physical page in the bitmap (mark as allocated)
// Used during init to mark bootloader-mapped pages as reserved
static void reserve_physical_page(uint64_t phys_addr)
{
	if (phys_addr < mm_state.memory_start ||
	    phys_addr >= mm_state.memory_end) {
		return; // Outside managed range
	}

	uint64_t page = (phys_addr - mm_state.memory_start) / PAGE_SIZE;
	if (page < mm_state.total_pages && !is_page_allocated(page)) {
		set_page_bit(page);
		mm_state.free_pages--;
	}
}

// Walk all kernel virtual addresses and reserve their backing physical pages
// This ensures we never allocate physical pages that are already in use by the kernel
static void reserve_bootloader_mapped_pages(void)
{
	uint64_t kernel_start_virt = KERNEL_OFFSET;
	uint64_t heap_start_virt = mm_get_kernel_heap_start();
	uint64_t heap_end_virt = heap_start_virt + KERNEL_HEAP_SIZE;

	// Also reserve pages for bitmap (refcount array is now allocated separately)
	uint64_t bitmap_end_virt =
		(uint64_t)mm_state.physical_bitmap + mm_state.bitmap_size;

	// Find the highest virtual address we need to scan
	uint64_t scan_end = bitmap_end_virt;
	if (heap_end_virt > scan_end)
		scan_end = heap_end_virt;

	uint64_t pages_reserved = 0;

	kprintf("  Scanning bootloader mappings: 0x%lx - 0x%lx\n",
		kernel_start_virt, scan_end);

	// Walk through all virtual pages in the kernel space
	for (uint64_t virt = kernel_start_virt; virt < scan_end;
	     virt += PAGE_SIZE) {
		uint64_t phys = early_virt_to_phys(virt);
		if (phys != 0) {
			// Check if this physical page is in our managed range
			if (phys >= mm_state.memory_start &&
			    phys < mm_state.memory_end) {
				reserve_physical_page(phys);
				pages_reserved++;
			}
		}
	}

	kprintf("  Reserved %lu bootloader-mapped pages in managed range\n",
		pages_reserved);
}

// ============================================================================
// UEFI MEMORY MAP HANDLING
// Reserve all physical memory regions that are marked as non-usable by UEFI.
// This prevents us from allocating memory that is used by:
//   - UEFI Runtime Services (firmware callbacks)
//   - ACPI tables and NVS data
//   - Memory-mapped I/O regions
//   - Reserved firmware memory
// ============================================================================

static const char *mm_get_efi_memory_type_name(uint32_t type)
{
	switch (type) {
	case EFI_RESERVED_MEMORY_TYPE:
		return "Reserved";
	case EFI_LOADER_CODE:
		return "LoaderCode";
	case EFI_LOADER_DATA:
		return "LoaderData";
	case EFI_BOOT_SERVICES_CODE:
		return "BootServicesCode";
	case EFI_BOOT_SERVICES_DATA:
		return "BootServicesData";
	case EFI_RUNTIME_SERVICES_CODE:
		return "RuntimeServicesCode";
	case EFI_RUNTIME_SERVICES_DATA:
		return "RuntimeServicesData";
	case EFI_CONVENTIONAL_MEMORY:
		return "ConventionalMemory";
	case EFI_UNUSABLE_MEMORY:
		return "UnusableMemory";
	case EFI_ACPI_RECLAIM_MEMORY:
		return "ACPIReclaimMemory";
	case EFI_ACPI_MEMORY_NVS:
		return "ACPIMemoryNVS";
	case EFI_MEMORY_MAPPED_IO:
		return "MemoryMappedIO";
	case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
		return "MMIOPortSpace";
	case EFI_PAL_CODE:
		return "PALCode";
	case EFI_PERSISTENT_MEMORY:
		return "PersistentMemory";
	default:
		return "Unknown";
	}
}

// Mark usable UEFI memory regions as free (inverse of reserve)
// This is used after setting all pages as used, to mark only actual RAM as available
static void mark_usable_uefi_regions(void)
{
	if (g_uefi_memory_map.entry_count == 0) {
		kprintf("  WARNING: No UEFI memory map - marking all managed pages as usable\n");
		// Fallback: mark all as free (old behavior)
		mm_memset(mm_state.physical_bitmap, 0, mm_state.bitmap_size);
		mm_state.free_pages = mm_state.total_pages;
		mm_state.usable_pages = mm_state.total_pages;
		return;
	}

	uint64_t pages_marked_free = 0;

	kprintf("  Marking UEFI usable memory regions as free:\n");

	for (uint32_t i = 0; i < g_uefi_memory_map.entry_count; i++) {
		memory_map_entry_t *entry = &g_uefi_memory_map.entries[i];

		// Only process usable memory types
		if (!mm_is_usable_memory_type(entry->type)) {
			continue;
		}

		// This is a usable region - mark pages as free
		uint64_t region_start = entry->physical_start;
		uint64_t region_end =
			region_start + (entry->number_of_pages * PAGE_SIZE);

		// Only process regions that overlap with our managed memory range
		if (region_end <= mm_state.memory_start ||
		    region_start >= mm_state.memory_end) {
			continue; // Region is outside our managed range
		}

		// Clamp to our managed range
		if (region_start < mm_state.memory_start) {
			region_start = mm_state.memory_start;
		}
		if (region_end > mm_state.memory_end) {
			region_end = mm_state.memory_end;
		}

		uint64_t region_pages = (region_end - region_start) / PAGE_SIZE;

		kprintf("    [%02u] 0x%lx-0x%lx (%s): %lu pages\n", i,
			entry->physical_start,
			entry->physical_start +
				(entry->number_of_pages * PAGE_SIZE),
			mm_get_efi_memory_type_name(entry->type), region_pages);

		// Mark each page in this region as free
		for (uint64_t phys = region_start; phys < region_end;
		     phys += PAGE_SIZE) {
			uint64_t page =
				(phys - mm_state.memory_start) / PAGE_SIZE;
			if (page < mm_state.total_pages &&
			    is_page_allocated(page)) {
				clear_page_bit(page);
				mm_state.free_pages++;
				pages_marked_free++;
			}
		}
	}

	/* Everything just marked free IS the RAM in the managed range; the rest
	 * of the span is holes.  Reservations below take pages back out of
	 * free_pages without changing this, which is what makes "used" mean
	 * "RAM in use" rather than "RAM in use plus the size of the PCI hole". */
	mm_state.usable_pages = pages_marked_free;

	kprintf("  Marked %lu pages as free from UEFI usable regions (%lu MB of real RAM in a %lu MB span)\n",
		pages_marked_free, (pages_marked_free * PAGE_SIZE) / (1024 * 1024),
		((mm_state.memory_end - mm_state.memory_start)) / (1024 * 1024));
}

static void reserve_uefi_memory_regions(void)
{
	if (g_uefi_memory_map.entry_count == 0) {
		kprintf("  WARNING: No UEFI memory map available!\n");
		return;
	}

	uint64_t pages_reserved = 0;
	uint64_t reserved_regions = 0;

	kprintf("  Reserving UEFI non-usable memory regions:\n");

	for (uint32_t i = 0; i < g_uefi_memory_map.entry_count; i++) {
		memory_map_entry_t *entry = &g_uefi_memory_map.entries[i];

		// Skip usable memory types - these are safe to allocate from
		if (mm_is_usable_memory_type(entry->type)) {
			continue;
		}

		// This is a reserved/non-usable region - mark all pages as allocated
		uint64_t region_start = entry->physical_start;
		uint64_t region_end =
			region_start + (entry->number_of_pages * PAGE_SIZE);

		// Only process regions that overlap with our managed memory range
		if (region_end <= mm_state.memory_start ||
		    region_start >= mm_state.memory_end) {
			continue; // Region is outside our managed range
		}

		// Clamp to our managed range
		if (region_start < mm_state.memory_start) {
			region_start = mm_state.memory_start;
		}
		if (region_end > mm_state.memory_end) {
			region_end = mm_state.memory_end;
		}

		uint64_t region_pages = (region_end - region_start) / PAGE_SIZE;

		kprintf("    [%02u] 0x%lx-0x%lx (%s): %lu pages\n", i,
			entry->physical_start,
			entry->physical_start +
				(entry->number_of_pages * PAGE_SIZE),
			mm_get_efi_memory_type_name(entry->type), region_pages);

		// Reserve each page in this region
		for (uint64_t phys = region_start; phys < region_end;
		     phys += PAGE_SIZE) {
			reserve_physical_page(phys);
			pages_reserved++;
		}
		reserved_regions++;
	}

	kprintf("  Reserved %lu pages across %lu UEFI reserved regions\n",
		pages_reserved, reserved_regions);
}

// Initialize memory manager with UEFI memory map from boot_info
// This should be called before mm_initialize_physical_memory if boot_info is available
/* Bytes of physical RAM the bootloader's PML4[272] direct map covers.  Set
 * from boot_info; phys_to_virt() is only valid below this, so the physical
 * allocator must not manage memory past it.  Defaults to the historical 16 GB
 * for a legacy bootloader that does not report the field (direct_map_bytes==0). */
static uint64_t g_direct_map_limit = 16ULL * 1024 * 1024 * 1024;

void mm_initialize_from_boot_info(boot_info_t *boot_info)
{
	if (!boot_info) {
		kprintf("WARNING: No boot_info provided, UEFI memory map not available\n");
		return;
	}

	if (boot_info->direct_map_bytes != 0)
		g_direct_map_limit = boot_info->direct_map_bytes;

	// Copy the memory map to our static storage
	/* Clamp the COUNT to what is actually copied.  It used to record the
	 * firmware's full count while the loop below stopped at
	 * MAX_MEMORY_MAP_ENTRIES, so every consumer walked past the copied
	 * entries into uninitialised storage and treated whatever was there as
	 * real regions -- reserving RAM that exists or freeing RAM that does
	 * not, depending on the garbage. */
	uint32_t n_entries = boot_info->mem_info.entry_count;

	if (n_entries > MAX_MEMORY_MAP_ENTRIES) {
		kprintf("WARNING: UEFI memory map has %u entries, only %u fit - the rest are IGNORED\n",
			n_entries, (uint32_t)MAX_MEMORY_MAP_ENTRIES);
		n_entries = MAX_MEMORY_MAP_ENTRIES;
	}
	g_uefi_memory_map.entry_count = n_entries;
	g_uefi_memory_map.descriptor_size = boot_info->mem_info.descriptor_size;
	g_uefi_memory_map.total_memory = boot_info->mem_info.total_memory;

	for (uint32_t i = 0; i < n_entries; i++) {
		g_uefi_memory_map.entries[i] = boot_info->mem_info.entries[i];
	}

	kprintf("Stored UEFI memory map: %u entries, %lu MB total\n",
		g_uefi_memory_map.entry_count,
		g_uefi_memory_map.total_memory / (1024 * 1024));
}

// Allocation hint - start searching from here for faster subsequent allocations
static uint64_t g_alloc_hint = 0;

// Find first free bit in bitmap
// Uses hint to avoid rescanning already-allocated low pages
/* First free page in the bitmap words [begin_word, end_word), or -1. */
static uint64_t find_free_page_in(uint64_t begin_word, uint64_t end_word)
{
	for (uint64_t i = begin_word; i < end_word; i++) {
		if (mm_state.physical_bitmap[i] == 0xFFFFFFFF)
			continue;
		for (int bit = 0; bit < 32; bit++) {
			if (!(mm_state.physical_bitmap[i] & (1 << bit)))
				return i * 32 + bit;
		}
	}
	return (uint64_t)-1;
}

/* The first page at or above 4 GB, as a page index, or 0 when the managed
 * range is entirely low.  Everything below it is the only memory a device
 * that carries 32-bit addresses can be given. */
static uint64_t dma_boundary_page(void)
{
	if (mm_state.memory_start >= 0x100000000ULL)
		return 0; /* no low memory at all: nothing to protect */
	uint64_t low = (0x100000000ULL - mm_state.memory_start) / PAGE_SIZE;

	return low < mm_state.total_pages ? low : 0;
}

/* Ordinary single-page allocation, taken from HIGH memory first.
 *
 * Anonymous pages, page-table pages, page-cache pages -- none of them care
 * where they are, and there are millions of them.  Contiguous runs for DMA
 * care very much: several NICs here carry 32-bit addresses only, and
 * mm_allocate_contiguous_pages() can serve them only from below 4 GB.  This
 * used to be one rotating scan over the whole range, so ordinary allocations
 * consumed the low region like any other, and on a machine with more than
 * 4 GB the drivers eventually found nothing contiguous left down there --
 * which is what the "served from above 4 GB" warnings were saying, one per
 * buffer, for the rest of the session.
 *
 * Keeping the low region for the allocations that cannot use anything else
 * is what a DMA zone is.  It is a preference, not a reservation: when high
 * memory is gone the low region is used rather than failing, because an
 * allocation that fails is worse than one a device cannot reach.
 */
static uint64_t find_free_page(void)
{
	BUG_ON(mm_state.physical_bitmap == NULL);
	uint64_t num_words = mm_state.bitmap_size / sizeof(uint32_t);
	uint64_t dma_words = (dma_boundary_page() + 31) / 32;
	uint64_t page;

	if (dma_words > num_words)
		dma_words = num_words;

	if (dma_words < num_words) {
		/* High memory, resuming where the last one left off. */
		uint64_t hint = g_alloc_hint / 32;

		if (hint < dma_words || hint >= num_words)
			hint = dma_words;
		page = find_free_page_in(hint, num_words);
		if (page == (uint64_t)-1)
			page = find_free_page_in(dma_words, hint);
		if (page != (uint64_t)-1) {
			g_alloc_hint = page + 1;
			return page;
		}
	}

	/* Low memory: either this machine has none above 4 GB (in which case
	 * the scan above already covered everything and this is the only
	 * pass), or high memory is exhausted and reaching down here is better
	 * than failing. */
	page = find_free_page_in(0, dma_words ? dma_words : num_words);
	if (page != (uint64_t)-1)
		return page; /* deliberately does not move the high hint */

	return (uint64_t)-1; // No free pages
}

// Set bit in bitmap
static void set_page_bit(uint64_t page)
{
	uint64_t index = page / 32;
	uint64_t bit = page % 32;
	if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
		mm_state.physical_bitmap[index] |= (1 << bit);
	}
}

// Clear bit in bitmap
static void clear_page_bit(uint64_t page)
{
	uint64_t index = page / 32;
	uint64_t bit = page % 32;
	if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
		mm_state.physical_bitmap[index] &= ~(1 << bit);
	}
}

// Check if bit is set in bitmap
static bool is_page_allocated(uint64_t page)
{
	uint64_t index = page / 32;
	uint64_t bit = page % 32;
	if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
		return mm_state.physical_bitmap[index] & (1 << bit);
	}
	return true; // Assume allocated if out of range
}

// Initialize physical memory manager
void mm_initialize_physical_memory(uint64_t memory_size)
{
	kprintf("Initializing Physical Memory Manager...\n");

	// =========================================================================
	// KEY INSIGHT: The bootloader uses AllocateAnyPages which allocates physical
	// pages at ARBITRARY locations, not contiguously after kernel_end.
	//
	// We CANNOT assume physical addresses are linear with virtual addresses.
	// Instead, we:
	//   1. Start our managed physical memory well past any bootloader allocations
	//   2. Walk the page tables to find which physical pages are actually in use
	//   3. Mark those pages as reserved in our bitmap
	// =========================================================================

	uint64_t heap_start_virt = mm_get_kernel_heap_start();

	// Calculate PT pool size dynamically based on RAM:
	// - Each PT page can map 512 * 4KB = 2MB of memory
	// - For worst case (all 4KB pages), need memory_size / 2MB page tables
	// - Plus PD pages (memory_size / 1GB) and PDPT pages (small constant)
	// - Add 25% overhead for fragmentation and dynamic allocations
	uint64_t pt_pages_needed =
		memory_size / (2 * 1024 * 1024); // 1 PT per 2MB
	uint64_t pd_pages_needed =
		memory_size / (1024 * 1024 * 1024) + 1; // 1 PD per 1GB
	uint64_t pdpt_pages_needed = 4; // Up to 512GB coverage
	pt_pool_size = pt_pages_needed + pd_pages_needed + pdpt_pages_needed;
	pt_pool_size = (pt_pool_size * 5) / 4; // Add 25% overhead
	if (pt_pool_size < 256)
		pt_pool_size = 256; // Minimum 1MB pool

	// Start at 32MB
	pt_pool_phys_start = 32 * 1024 * 1024;
	uint64_t pt_pool_size_bytes = pt_pool_size * PAGE_SIZE;
	mm_state.memory_start = pt_pool_phys_start + pt_pool_size_bytes;

	kprintf("  PT pool reserved: 0x%lx - 0x%lx (%lu pages, %lu MB)\n",
		pt_pool_phys_start, mm_state.memory_start, pt_pool_size,
		pt_pool_size_bytes / (1024 * 1024));

	// End of managed memory is total RAM, but limited to what the direct map
	// covers: phys_to_virt() only works below g_direct_map_limit, which the
	// bootloader sized to actual RAM (see detect_physmap_size()).  On matching
	// bootloader+kernel this clamp never fires; it remains as a safety net (and
	// for a legacy bootloader that reports no extent, defaulting to 16 GB).
	/* End at the highest usable physical ADDRESS in the firmware map, not
	 * at the total number of bytes of RAM.
	 *
	 * Those are only the same if RAM starts at 0 and runs without a break,
	 * which no real firmware does.  It puts a PCI hole below 4 GB and
	 * continues RAM above it, so using the byte count did BOTH wrong things
	 * at once: the range covered ~1 GB of hole, which is not RAM and so can
	 * never be marked free -- it was counted as "used" for the life of the
	 * boot -- while the real RAM above 4 GB fell outside the range and was
	 * ignored completely.  A 4 GB machine ran on about 3 GB and reported a
	 * gigabyte in use before userspace started. */
	uint64_t highest_usable_end = 0;

	for (uint32_t i = 0; i < g_uefi_memory_map.entry_count; i++) {
		memory_map_entry_t *e = &g_uefi_memory_map.entries[i];
		uint64_t end;

		if (!mm_is_usable_memory_type(e->type))
			continue;
		end = e->physical_start + e->number_of_pages * PAGE_SIZE;
		if (end > highest_usable_end)
			highest_usable_end = end;
	}

	/* With no map to consult there is nothing better than the old guess. */
	mm_state.memory_end = (highest_usable_end > mm_state.memory_start) ?
				      highest_usable_end :
				      memory_size;
	if (mm_state.memory_end > g_direct_map_limit) {
		kprintf("  NOTE: Limiting managed memory to %lu MB (direct map limit)\n",
			g_direct_map_limit / (1024 * 1024));
		kprintf("  System has %lu MB but only %lu MB is usable\n",
			memory_size / (1024 * 1024),
			g_direct_map_limit / (1024 * 1024));
		mm_state.memory_end = g_direct_map_limit;
	}

	// Sanity check
	if (mm_state.memory_end <= mm_state.memory_start) {
		kprintf("ERROR: Not enough RAM! memory_start=0x%lx > memory_end=0x%lx\n",
			mm_state.memory_start, mm_state.memory_end);
		mm_state.memory_end =
			mm_state.memory_start + (64 * 1024 * 1024); // Fallback
	}

	mm_state.total_pages =
		(mm_state.memory_end - mm_state.memory_start) / PAGE_SIZE;
	mm_state.bitmap_size = PAGE_ALIGN((mm_state.total_pages + 7) / 8);

	// Place bitmap in kernel virtual space (after heap).
	// The bootloader maps min_virtual_size (currently 128 MB) of virtual
	// space starting at the kernel base — must cover BSS + heap + bitmap
	// + page refcount array.
	mm_state.physical_bitmap =
		(uint32_t *)(heap_start_virt + KERNEL_HEAP_SIZE);

	kprintf("  Memory range: 0x%lx - 0x%lx (%lu MB)\n",
		mm_state.memory_start, mm_state.memory_end,
		(mm_state.memory_end - mm_state.memory_start) / (1024 * 1024));
	kprintf("  Total pages: %lu\n", mm_state.total_pages);
	kprintf("  Heap: 0x%lx - 0x%lx\n", heap_start_virt,
		heap_start_virt + KERNEL_HEAP_SIZE);
	kprintf("  Bitmap at: %p (size: %lu bytes)\n", mm_state.physical_bitmap,
		mm_state.bitmap_size);

	// CRITICAL FIX: Start with ALL pages marked as USED (allocated)
	// This prevents allocating pages from gaps in physical RAM (like the PCI hole).
	// Then we'll mark only pages in UEFI usable regions as free.
	mm_memset(mm_state.physical_bitmap, 0xFF, mm_state.bitmap_size);
	mm_state.free_pages = 0;

	// Compute refcount array size but DON'T place it yet — we need the allocator
	// running first so we can use properly-tracked physical pages.
	mm_state.refcount_array_size = mm_state.total_pages * sizeof(uint16_t);
	mm_state.page_refcounts = NULL; // Will be allocated below

	kprintf("  Page refcounts: %lu bytes (deferred until allocator ready)\n",
		mm_state.refcount_array_size);

	// =========================================================================
	// CRITICAL: Mark ONLY pages in UEFI usable memory regions as free.
	// This correctly handles gaps in physical RAM (PCI hole, etc.)
	// =========================================================================
	mark_usable_uefi_regions();

	// =========================================================================
	// CRITICAL: Walk the bootloader's page tables and reserve any physical pages
	// that are already mapped. This prevents us from allocating pages that are
	// backing kernel code, heap, bitmap, or refcount array.
	// =========================================================================
	reserve_bootloader_mapped_pages();

	// =========================================================================
	// CRITICAL: Reserve all UEFI non-usable memory regions (Runtime Services,
	// ACPI, MMIO, etc.). This prevents us from allocating memory that the
	// firmware still needs for runtime callbacks.
	// =========================================================================
	reserve_uefi_memory_regions();

	// =========================================================================
	// NOW allocate the refcount array from properly-tracked physical pages.
	// We use the direct map (phys_to_virt) to access them.  The old approach
	// placed the array in the bootloader's extended kernel mapping, but those
	// physical pages were EfiLoaderData and got marked free by
	// mark_usable_uefi_regions.  reserve_bootloader_mapped_pages was SUPPOSED
	// to re-reserve them, but this was unreliable — writes to the refcount
	// array silently went to recycled pages, breaking COW.
	//
	// CRITICAL: The pages MUST be physically contiguous because we access them
	// as a flat array via phys_to_virt(first_phys).  If there are gaps (e.g.
	// reserved pages between free pages), mm_memset and later accesses would
	// hit unrelated physical pages, corrupting whatever they back.
	// =========================================================================
	{
		uint64_t refcount_pages =
			(mm_state.refcount_array_size + PAGE_SIZE - 1) /
			PAGE_SIZE;

		// Find a contiguous run of free pages in the bitmap.
		uint64_t run_start = (uint64_t)-1;
		uint64_t run_len = 0;
		for (uint64_t p = 0; p < mm_state.total_pages; p++) {
			if (!is_page_allocated(p)) {
				if (run_len == 0)
					run_start = p;
				run_len++;
				if (run_len == refcount_pages)
					break;
			} else {
				run_len = 0;
			}
		}

		if (run_len < refcount_pages) {
			kprintf("FATAL: Cannot find %lu contiguous free pages for refcount array!\n",
				refcount_pages);
			mm_state.page_refcounts = NULL;
		} else {
			// Mark all pages in the contiguous run as allocated
			for (uint64_t i = 0; i < refcount_pages; i++) {
				set_page_bit(run_start + i);
				mm_state.free_pages--;
			}
			uint64_t first_phys =
				mm_state.memory_start + run_start * PAGE_SIZE;

			mm_state.page_refcounts =
				(uint16_t *)phys_to_virt(first_phys);
			mm_memset(mm_state.page_refcounts, 0,
				  mm_state.refcount_array_size);

#if MM_LEAK_INSTRUMENTATION
			/* Same trick again for the owner table. */
			mm_state.owner_array_size =
				mm_state.total_pages * sizeof(uint32_t);
			{
				uint64_t owner_pages =
					(mm_state.owner_array_size +
					 PAGE_SIZE - 1) / PAGE_SIZE;
				uint64_t ostart = (uint64_t)-1, olen = 0;

				for (uint64_t q = 0; q < mm_state.total_pages;
				     q++) {
					if (!is_page_allocated(q)) {
						if (olen == 0)
							ostart = q;
						olen++;
						if (olen == owner_pages)
							break;
					} else {
						olen = 0;
					}
				}
				if (olen < owner_pages) {
					kprintf("  NOTE: no room for the page-owner table; allocation attribution is off\n");
					mm_state.page_owner = NULL;
				} else {
					for (uint64_t i = 0; i < owner_pages;
					     i++) {
						set_page_bit(ostart + i);
						mm_state.free_pages--;
					}
					mm_state.page_owner =
						(uint32_t *)phys_to_virt(
							mm_state.memory_start +
							ostart * PAGE_SIZE);
					mm_memset(mm_state.page_owner, 0,
						  mm_state.owner_array_size);
				}
			}
#endif

			kprintf("  Page refcounts at: %p (phys 0x%lx, %lu contiguous pages)\n",
				mm_state.page_refcounts, first_phys,
				refcount_pages);
		}
	}

	kprintf("  Free pages after reservations: %lu\n", mm_state.free_pages);
	kprintf("Physical Memory Manager initialized\n");
}

// Allocate a physical page (SMP-safe)
uint64_t mm_allocate_physical_page(void)
{
	uint64_t flags;
	spin_lock_irqsave(&mm_phys_lock, &flags);

	if (mm_state.free_pages == 0) {
		spin_unlock_irqrestore(&mm_phys_lock, flags);
		return 0; // Out of memory
	}

	uint64_t page = find_free_page();
	if (page == (uint64_t)-1) {
		spin_unlock_irqrestore(&mm_phys_lock, flags);
		return 0; // No free pages found
	}

	set_page_bit(page);
	mm_state.free_pages--;
	PAGE_OWNER_SET(page, __builtin_return_address(0));

	uint64_t phys = mm_state.memory_start + (page * PAGE_SIZE);
	WARN_ON(phys &
		(PAGE_SIZE - 1)); /* allocated page is not page-aligned */
	/* One reference: the one the caller is about to hold.  A page in use
	 * always counts at least one, so zero unambiguously means free. */
	if (mm_state.page_refcounts) {
		mm_state.page_refcounts[page] = 1;
	}

	spin_unlock_irqrestore(&mm_phys_lock, flags);

	/* Zero or poison the page outside the lock; the page bit is already set
     * so only we can access it. */
	if (is_phys_in_direct_map(phys)) {
#if DEBUG
		mm_poison_fill(phys_to_virt(phys), POISON_UNINIT_PAGE,
			       PAGE_SIZE);
#else
		mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
#endif
	}
	return phys;
}

// Free a physical page (SMP-safe)

void mm_free_physical_page(uint64_t physical_address)
{
	WARN_ON(physical_address &
		(PAGE_SIZE - 1)); /* freeing non-page-aligned address */
	VM_BUG_ON(physical_address == 0);
	if (physical_address < mm_state.memory_start ||
	    physical_address >= mm_state.memory_end) {
		return; // Invalid address
	}

	uint64_t flags;
	spin_lock_irqsave(&mm_phys_lock, &flags);

	uint64_t page = (physical_address - mm_state.memory_start) / PAGE_SIZE;
	if (!is_page_allocated(page)) {
		WARN(1, "mm_free_physical_page: double-free of page at 0x%lx",
		     physical_address);
		spin_unlock_irqrestore(&mm_phys_lock, flags);
		return; // Already free
	}

	/* Poison the page before marking it free, in debug builds only.  We hold
     * the lock and the page bit is still set, so no other CPU can grab the
     * page yet.
     *
     * This used to run unconditionally, and it was the single most expensive
     * thing a large process did on its way out.  Tearing down a 900MB address
     * space freed ~230,000 pages, each one a 4KB store loop inside this
     * global lock with interrupts disabled -- close to a gigabyte of stores
     * that nothing ever reads back.  Worse than the raw cost: a CPU waiting
     * for this lock waits with interrupts off (spin_lock_irqsave disables
     * before it spins), so it cannot acknowledge the TLB shootdown IPI that
     * some other CPU is blocked waiting for.  The page freeing starved the
     * acknowledgements, the shootdowns ran into their one-second deadline,
     * and closing a mail client took two minutes.
     *
     * The allocation path above has always been gated this way, and in
     * non-debug builds it zeroes every page it hands out -- so poisoning here
     * meant writing every page twice for no observable effect. */
#if DEBUG
	if (is_phys_in_direct_map(physical_address)) {
		mm_poison_fill(phys_to_virt(physical_address),
			       POISON_FREED_PAGE, PAGE_SIZE);
	}
#endif
	clear_page_bit(page);
	mm_state.free_pages++;

	// Update allocation hint to allow reusing freed pages
	if (page < g_alloc_hint) {
		g_alloc_hint = page;
	}

	/* Releasing a page that others still reference is the bug this whole
	 * rewrite is about, so say so.  One reference is normal and expected:
	 * it is the allocator's own, held by kernel-internal callers (page
	 * tables, slab, DMA buffers) that never share a page and free it
	 * directly rather than through mm_put_page().  Zero is what
	 * mm_put_page() leaves behind when the last reference goes.  Anything
	 * above one means a live mapping is about to be poisoned. */
	if (mm_state.page_refcounts) {
		WARN_RATELIMIT(mm_state.page_refcounts[page] > 1,
			       "mm_free_physical_page: releasing 0x%lx with %u references still held",
			       (unsigned long)physical_address,
			       mm_state.page_refcounts[page]);
		mm_state.page_refcounts[page] = 0;
	}

	spin_unlock_irqrestore(&mm_phys_lock, flags);
}

/* ============================================================================
 * Batched release
 *
 * mm_free_physical_page() takes a global lock with interrupts disabled, so
 * releasing a page costs an acquire/release pair and an interrupt-off window
 * each.  For one page that is noise.  For an address space it is the whole
 * cost: a 900MB process is ~230,000 pages, which is 230,000 of each, back to
 * back, on a lock every other CPU's allocator needs.
 *
 * It is not only slow, it is actively harmful to the rest of the machine.
 * spin_lock_irqsave() disables interrupts BEFORE it spins, so every CPU
 * queued behind this lock is sitting with interrupts off and cannot
 * acknowledge a TLB shootdown -- which is what a third CPU is blocked waiting
 * for.  Long enough and that wait hits its deadline, and the exit that caused
 * it is measured in minutes.
 *
 * Freeing in batches takes the lock a few times per hundred pages instead of
 * once per page, and bounds each interrupt-off window to the arithmetic for
 * one batch -- roughly a microsecond -- with an interrupt window between
 * every one.
 * ========================================================================== */

void mm_free_physical_pages_batch(const uint64_t *phys, unsigned n)
{

	uint64_t flags;
	unsigned i;

	if (n == 0)
		return;
	BUG_ON(phys == NULL);
	BUG_ON(n > MM_FREE_BATCH_MAX);


#if DEBUG
	/* The poison pass below deliberately runs with the lock dropped.
	 *
	 * This used to assert might_sleep(), which was wrong twice over: this
	 * function does not sleep -- the release pass takes mm_phys_lock and
	 * nothing else -- and the assertion fired on a path that is perfectly
	 * legal, the network RX softirq freeing a receive buffer
	 * (net_rx_softirq -> tcp_rx -> tcp_grow_rx_buf -> slab_free ->
	 * mm_free_contiguous_pages).  It reported a sleep that never happens.
	 *
	 * What IS wrong in atomic context is the poison itself.  A caller with
	 * interrupts already off gets them back off from
	 * spin_unlock_irqrestore(), so the pass below becomes a memset of up to
	 * MM_FREE_BATCH_MAX pages -- 256 KB -- with interrupts disabled, on the
	 * hot receive path, once per batch.  Moving it inside the lock would
	 * only make the same window a locked one.
	 *
	 * So skip the poison there and keep the release, which is what the
	 * caller actually asked for.  The cost is that use-after-free on pages
	 * freed from interrupt context is not caught by the poison; the double
	 * -free and refcount checks in the release pass still apply, and they
	 * are the ones that matter. */
	if (!irqs_disabled()) {
		uint64_t poison = 0; /* bit i: phys[i] is ours to poison */

		/* Decide what to poison with the lock held, then poison with it
		 * dropped.  The page bits are deliberately left SET across the
		 * poison pass: while a bit is set no other CPU can be handed
		 * that page, which is what makes it safe to write to it without
		 * holding anything. */
		spin_lock_irqsave(&mm_phys_lock, &flags);
		for (i = 0; i < n; i++) {
			uint64_t page;

			if (phys[i] < mm_state.memory_start ||
			    phys[i] >= mm_state.memory_end)
				continue;
			page = (phys[i] - mm_state.memory_start) / PAGE_SIZE;
			if (is_page_allocated(page))
				poison |= 1ULL << i;
		}
		spin_unlock_irqrestore(&mm_phys_lock, flags);

		for (i = 0; i < n; i++) {
			if (!(poison & (1ULL << i)))
				continue;
			if (is_phys_in_direct_map(phys[i]))
				mm_poison_fill(phys_to_virt(phys[i]),
					       POISON_FREED_PAGE, PAGE_SIZE);
		}
	}
#endif

	/* Release.  This pass is the authoritative one: it re-checks every
	 * entry and is the only place that reports a double free, so the
	 * debug pass above stays silent about what it skips. */
	spin_lock_irqsave(&mm_phys_lock, &flags);
	for (i = 0; i < n; i++) {
		uint64_t pa = phys[i];
		uint64_t page;

		VM_BUG_ON(pa & (PAGE_SIZE - 1));
		if (pa < mm_state.memory_start || pa >= mm_state.memory_end)
			continue;

		page = (pa - mm_state.memory_start) / PAGE_SIZE;
		if (!is_page_allocated(page)) {
			WARN(1, "mm_free_physical_pages_batch: double-free of page at 0x%lx",
			     (unsigned long)pa);
			continue;
		}

		/* Same invariant mm_free_physical_page() documents: one
		 * reference is the allocator's own, anything above one means a
		 * live mapping is about to be released. */
		if (mm_state.page_refcounts) {
			WARN_RATELIMIT(mm_state.page_refcounts[page] > 1,
				       "mm_free_physical_pages_batch: releasing 0x%lx with %u references still held",
				       (unsigned long)pa,
				       mm_state.page_refcounts[page]);
			mm_state.page_refcounts[page] = 0;
		}

		clear_page_bit(page);
		mm_state.free_pages++;
		if (page < g_alloc_hint)
			g_alloc_hint = page;
	}
	spin_unlock_irqrestore(&mm_phys_lock, flags);
}

/* Drop a reference on each of `n` pages, releasing those whose last reference
 * goes, in one batch.  The refcount half is lock-free (a CAS per page, exactly
 * as mm_put_page does it); only the pages that actually reach zero reach the
 * allocator, and they go together. */
void mm_put_pages_batch(const uint64_t *phys, unsigned n)
{
	uint64_t release[MM_FREE_BATCH_MAX];
	unsigned nrelease = 0;
	unsigned i;

	if (n == 0)
		return;
	BUG_ON(phys == NULL);
	BUG_ON(n > MM_FREE_BATCH_MAX);

	for (i = 0; i < n; i++) {
		uint64_t idx = page_to_index(phys[i]);
		uint16_t current;

		if (idx == (uint64_t)-1 || !mm_state.page_refcounts) {
			/* Untracked: the caller's reference is the only one
			 * there can be. */
			release[nrelease++] = phys[i];
			continue;
		}

		current = __atomic_load_n(&mm_state.page_refcounts[idx],
					  __ATOMIC_ACQUIRE);
		for (;;) {
			if (unlikely(current == 0)) {
				WARN(1, "mm_put_pages_batch: reference dropped on free page 0x%lx (double free)",
				     (unsigned long)phys[i]);
				break;
			}
			if (__atomic_compare_exchange_n(
				    &mm_state.page_refcounts[idx], &current,
				    current - 1, false, __ATOMIC_ACQ_REL,
				    __ATOMIC_ACQUIRE))
				break;
			/* CAS failed: `current` was reloaded; retry. */
		}
		if (current == 1)
			release[nrelease++] = phys[i];
	}

	mm_free_physical_pages_batch(release, nrelease);
}

// Get free pages count
uint64_t mm_get_free_pages(void)
{
	return mm_state.free_pages;
}

/* All the RAM the machine has, in pages -- what the memory map reported as
 * usable, not the address span it is scattered over. */
uint64_t mm_get_usable_pages(void)
{
	return mm_state.usable_pages ? mm_state.usable_pages :
				       mm_state.total_pages;
}

// Allocate contiguous physical pages (SMP-safe)
uint64_t mm_allocate_contiguous_pages(size_t page_count)
{
	if (page_count == 0) {
		kprintf("mm_allocate_contiguous_pages: page_count is 0\n");
		return 0;
	}

	uint64_t flags;
	spin_lock_irqsave(&mm_phys_lock, &flags);

	if (mm_state.free_pages < page_count) {
		spin_unlock_irqrestore(&mm_phys_lock, flags);
		kprintf("mm_allocate_contiguous_pages: not enough free pages (%lu free, need %lu)\n",
			(unsigned long)mm_state.free_pages,
			(unsigned long)page_count);
		return 0;
	}
	if (page_count > mm_state.total_pages) {
		spin_unlock_irqrestore(&mm_phys_lock, flags);
		kprintf("mm_allocate_contiguous_pages: page_count %lu > total_pages %lu\n",
			(unsigned long)page_count,
			(unsigned long)mm_state.total_pages);
		return 0;
	}

	// Find contiguous free pages.  Next-fit: resume scanning where the last
	// search left off instead of from page 0 — a first-fit scan re-walks the
	// densely allocated low region on EVERY call, which made each block-sized
	// kalloc() (2 contiguous pages) cost a near-full bitmap scan under this
	// global lock.  Two passes cover the wrap-around.
	static uint64_t next_fit_hint;
	uint64_t limit = mm_state.total_pages - page_count;

	/* Prefer memory a 32-bit device can address.
	 *
	 * Contiguous runs are what drivers program into device descriptors, and
	 * several NICs here (rtl8139, pcnet32, tulip) carry 32-bit addresses
	 * only.  Every caller was written when the managed range stopped below
	 * 4 GB, so all of this memory WAS low; now that the range reaches the
	 * RAM above the PCI hole, keep serving these from below 4 GB.  The
	 * fallback pass covers the whole range so a kalloc() cannot fail while
	 * high memory sits free -- and says so, because that is the point at
	 * which a 32-bit device would be handed an address it cannot reach. */
	uint64_t dma_limit = limit;

	if (mm_state.memory_start < 0x100000000ULL) {
		uint64_t low = (0x100000000ULL - mm_state.memory_start) /
			       PAGE_SIZE;
		if (low > page_count && low - page_count < dma_limit)
			dma_limit = low - page_count;
	}
	if (next_fit_hint > dma_limit)
		next_fit_hint = 0;
	for (int pass = 0; pass < 3; pass++) {
		uint64_t begin = pass ? 0 : next_fit_hint;
		uint64_t end = pass ? next_fit_hint : dma_limit + 1;

		if (pass == 2) {
			/* Nothing below 4 GB left. */
			if (dma_limit == limit)
				break;
			WARN_RATELIMIT(
				1,
				"contiguous allocation of %lu pages served from above 4 GB - a 32-bit DMA device cannot address this",
				(unsigned long)page_count);
			begin = dma_limit + 1;
			end = limit + 1;
		}
		for (uint64_t start_page = begin; start_page < end;
		     start_page++) {
			bool found = true;

			// Check if all pages in range are free
			for (size_t i = 0; i < page_count; i++) {
				if (is_page_allocated(start_page + i)) {
					found = false;
					start_page += i; // skip past the used page
					break;
				}
			}

			if (found) {
				// Allocate all pages in range
				for (size_t i = 0; i < page_count; i++) {
					set_page_bit(start_page + i);
					mm_state.free_pages--;
					PAGE_OWNER_SET(start_page + i,
						       __builtin_return_address(0));
					/* Each frame carries the caller's one
					 * reference, exactly as the single-page
					 * allocator does — the run is released
					 * a frame at a time. */
					if (mm_state.page_refcounts)
						mm_state.page_refcounts
							[start_page + i] = 1;
				}
				next_fit_hint = start_page + page_count;
				uint64_t result = mm_state.memory_start +
						  (start_page * PAGE_SIZE);
				spin_unlock_irqrestore(&mm_phys_lock, flags);
				return result;
			}
		}
	}

	spin_unlock_irqrestore(&mm_phys_lock, flags);
	return 0; // No contiguous block found
}

// Free contiguous physical pages
void mm_free_contiguous_pages(uint64_t physical_address, size_t page_count)
{
	uint64_t batch[MM_FREE_BATCH_MAX];
	unsigned n = 0;

	/* In batches, so a large run costs a few acquisitions of the global
	 * allocator lock rather than one per frame. */
	for (size_t i = 0; i < page_count; i++) {
		if (n == MM_FREE_BATCH_MAX) {
			mm_free_physical_pages_batch(batch, n);
			n = 0;
		}
		batch[n++] = physical_address + (i * PAGE_SIZE);
	}
	mm_free_physical_pages_batch(batch, n);
}

// VIRTUAL MEMORY MANAGER IMPLEMENTATION

// Debug flag for page table operations (non-static so slab.c can use it)
int mm_debug_pt = 0;

// Forward declarations
void *kalloc_dma(size_t size);
uint64_t mm_virt_to_phys_heap(uint64_t virt);

// Convert kernel heap virtual address to physical address
// The kernel heap is at KERNEL_OFFSET + kernel_end, mapped linearly
uint64_t mm_virt_to_phys_heap(uint64_t virt)
{
	// Kernel virtual base: 0xFFFFFFFF80000000
	// The kernel is loaded at physical 0x100000 and mapped to virtual 0xFFFFFFFF80000000
	// So: phys = virt - KERNEL_OFFSET + 0x100000

	if (virt >= KERNEL_OFFSET && virt < 0xFFFFFFFF90000000ULL) {
		return (virt - KERNEL_OFFSET) +
		       0x100000ULL; // KERNEL_START = 0x100000
	}

	// For direct-mapped addresses (PHYS_MAP_BASE + phys), extract physical address
	if (virt >= PHYS_MAP_BASE && virt < (PHYS_MAP_BASE + 0x100000000ULL)) {
		return virt - PHYS_MAP_BASE;
	}

	kprintf("ERROR: mm_virt_to_phys_heap: unknown address %p\n",
		(void *)virt);
	return 0;
}

// Allocate a page for page table structures (SMP-safe)
// Returns: physical address (for use in page table entries)
// Pages come from the reserved PT pool first, then fall back to physical allocator
static uint64_t allocate_pt_page(void)
{
	if (!pt_pool_initialized) {
		kprintf("ERROR: PT pool not initialized!\n");
		return 0;
	}

	uint64_t phys;
	uint64_t flags;

	spin_lock_irqsave(&mm_pt_pool_lock, &flags);

	if (pt_pool_freelist) {
		// Reuse a recycled page from the freelist (preferred)
		phys = pt_pool_freelist;
		uint64_t *page = (uint64_t *)phys_to_virt(phys);
		pt_pool_freelist = page[0];
		spin_unlock_irqrestore(&mm_pt_pool_lock, flags);
	} else if (pt_pool_next < pt_pool_size) {
		// Use reserved pool (outside bitmap range)
		phys = pt_pool_phys_start + (pt_pool_next * PAGE_SIZE);
		pt_pool_next++;
		spin_unlock_irqrestore(&mm_pt_pool_lock, flags);
	} else {
		spin_unlock_irqrestore(&mm_pt_pool_lock, flags);
		// Pool exhausted - fall back to physical memory allocator
		// This is fine for large allocations after initial setup
		phys = mm_allocate_physical_page();
		if (!phys) {
			kprintf("ERROR: Cannot allocate PT page (pool exhausted, phys alloc failed)\n");
			return 0;
		}
	}

	// Access via direct map and zero it
	uint64_t *virt = (uint64_t *)phys_to_virt(phys);
	mm_memset(virt, 0, PAGE_SIZE);

	return phys;
}

// Free a page table page back to the PT pool (or general allocator)
static void free_pt_page(uint64_t phys)
{
	if (!phys)
		return;

	uint64_t pt_pool_end = pt_pool_phys_start + (pt_pool_size * PAGE_SIZE);
	if (phys >= pt_pool_phys_start && phys < pt_pool_end) {
		// PT pool page — recycle to our freelist
		uint64_t flags;
		spin_lock_irqsave(&mm_pt_pool_lock, &flags);
		uint64_t *page = (uint64_t *)phys_to_virt(phys);
		page[0] = pt_pool_freelist;
		pt_pool_freelist = phys;
		spin_unlock_irqrestore(&mm_pt_pool_lock, flags);
	} else {
		// Allocated from general pool — return there
		mm_free_physical_page(phys);
	}
}

/* No processor may still be walking these tables when they go back to the
 * allocator.
 *
 * Page-table pages and ordinary pages come from the SAME allocator here:
 * allocate_pt_page() falls back to mm_allocate_physical_page() as soon as the
 * reserved pool runs out, and the pool holds one address space's worth, so the
 * fallback is reached within the first handful of processes.  A table released
 * while any processor could still be walking it therefore comes straight back
 * as somebody's heap, and from then on the two disagree about what the memory
 * is: the processor reads a program's data as page-table entries, and the
 * program reads page-table entries as its data.  Both have been seen -- an
 * entry of all ones in a live table, and a heap chunk whose size field is all
 * ones.
 *
 * The rule is the reference's, in its order: unlink the page, invalidate, THEN
 * free.  What makes the invalidate sufficient is that remote invalidation here
 * is an inter-processor interrupt.  A processor cannot take one part-way
 * through a walk, so every acknowledgement is that processor saying it is not
 * inside a walk of anything unlinked before the request went out -- and the
 * full reload it does on the way flushes the paging-structure caches too,
 * which hold the upper-level entries and which no address-specific
 * invalidation would touch.  (An architecture whose remote invalidation is not
 * an interrupt has no such promise and must wait out a grace period instead;
 * that is the only reason the reference has a second mechanism for it.)
 *
 * Switching this processor's own CR3 away, which is all that used to happen,
 * says nothing about any other processor.
 *
 * One barrier covers the whole teardown rather than one per batch: an address
 * space reaching here is already unreachable -- no task refers to it -- so
 * after the barrier nothing can begin a walk of any part of it, and the rest
 * of the tree can be dismantled at leisure. */
static void pt_free_barrier(uint64_t pml4_phys)
{
	uint32_t online = percpu_get_online_count();

	for (uint32_t c = 0; c < online && c < 64; c++) {
		percpu_t *p = percpu_get(c);

		if (!p)
			continue;
		if (__atomic_load_n(&p->mmu_active_pml4, __ATOMIC_ACQUIRE) ==
			    pml4_phys ||
		    __atomic_load_n(&p->mmu_incoming_pml4, __ATOMIC_ACQUIRE) ==
			    pml4_phys) {
			/* Still loaded somewhere.  The barrier below cannot
			 * help with this one: that processor will go on
			 * walking these tables for its kernel mappings after
			 * they are freed, because the kernel half lives in the
			 * same tree.  Nothing here can make it leave, so say
			 * so -- a report naming the processor is worth more
			 * than the corruption it turns into. */
			WARN_RATELIMIT(
				1,
				"page tables of address space %llx freed while CPU %u still has it loaded",
				(unsigned long long)pml4_phys, c);
			break;
		}
	}
	smp_tlb_shootdown_sync();
}

// Initialize page table page pool by reserving physical memory
// Must be called BEFORE mm_init_physical_memory to reserve the region
void mm_init_pt_pool(void)
{
	// The pool is already reserved during mm_init_physical_memory
	// This just marks it as ready to use
	if (pt_pool_phys_start == 0) {
		kprintf("ERROR: PT pool physical region not set!\n");
		return;
	}
	kprintf("Initializing page table pool (%lu pages at phys %p)...\n",
		pt_pool_size, (void *)pt_pool_phys_start);
	pt_pool_initialized = 1;
	kprintf("  Page table pool ready\n");
}

// Get page table for virtual address
// NOTE: Uses local variable for PML4 pointer to be SMP-safe. The global
// mm_state.pml4_table is only updated for compatibility but should NOT be
// relied upon in concurrent code paths.
uint64_t *mm_get_page_table(uint64_t virtual_addr, bool create)
{
	uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
	uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
	uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
	uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;

	// Get PML4 via direct map - use LOCAL variable for SMP safety!
	// Multiple CPUs can call this function concurrently, so we must not
	// rely on the global mm_state.pml4_table being stable.
	uint64_t pml4_phys = get_cr3() & ~0xFFF;
	uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

	if (mm_debug_pt) {
		kprintf("PT: vaddr=%p pml4=%lu pdpt=%lu pd=%lu pt=%lu\n",
			(void *)virtual_addr, pml4_index, pdpt_index, pd_index,
			pt_index);
		kprintf("PT: PML4 at %p, entry[%lu]=%p\n", pml4, pml4_index,
			(void *)pml4[pml4_index]);
	}

	uint64_t pdpt_phys = pml4[pml4_index] & PTE_ADDR_MASK;
	uint64_t *pdpt = (pml4[pml4_index] & PAGE_PRESENT) ?
				 (uint64_t *)phys_to_virt(pdpt_phys) :
				 NULL;
	if (!pdpt && create) {
		pdpt_phys = allocate_pt_page();
		if (!pdpt_phys) {
			return NULL;
		}
		// Already zeroed by allocate_pt_page
		pml4[pml4_index] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
		pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
		if (mm_debug_pt)
			kprintf("PT: Created new PDPT at %p\n", pdpt);
	}
	if (!pdpt) {
		return NULL;
	}

	if (mm_debug_pt) {
		kprintf("PT: PDPT at %p, entry[%lu]=%p\n", pdpt, pdpt_index,
			(void *)pdpt[pdpt_index]);
	}

	uint64_t pd_phys = pdpt[pdpt_index] & PTE_ADDR_MASK;
	uint64_t *pd = (pdpt[pdpt_index] & PAGE_PRESENT) ?
			       (uint64_t *)phys_to_virt(pd_phys) :
			       NULL;
	if (!pd && create) {
		pd_phys = allocate_pt_page();
		if (!pd_phys) {
			return NULL;
		}
		// Already zeroed by allocate_pt_page
		pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
		pd = (uint64_t *)phys_to_virt(pd_phys);
		if (mm_debug_pt)
			kprintf("PT: Created new PD at %p\n", pd);
	}
	if (!pd) {
		return NULL;
	}

	if (mm_debug_pt) {
		kprintf("PT: PD at %p, entry[%lu]=%p\n", pd, pd_index,
			(void *)pd[pd_index]);
	}

	// Check for 2MB large page (PAGE_SIZE_FLAG set in PDE)
	if ((pd[pd_index] & PAGE_PRESENT) && (pd[pd_index] & PAGE_SIZE_FLAG)) {
		if (!create) {
			return NULL; // Can't split without create
		}
		// Split 2MB page into 512 4KB pages so individual pages can be remapped
		uint64_t large_phys = pd[pd_index] &
				      0x000FFFFFFFE00000ULL; // 2MB-aligned phys
		// Inherit all flags except PAGE_SIZE_FLAG (bit 7 = PAT in 4KB PTEs, but
		// for 2MB pages bit 7 = PS; original 2MB PAT is bit 12 which we ignore
		// since the bootloader sets WB caching by default)
		uint64_t pte_flags =
			pd[pd_index] &
			0x8000000000000E7FULL; // NX + bits 0-6,9-11
		pte_flags &=
			~PAGE_SIZE_FLAG; // clear bit 7 (PS -> PAT for 4KB, keep 0)
		uint64_t split_pt = allocate_pt_page();
		if (!split_pt) {
			return NULL;
		}
		uint64_t *split_pt_virt = (uint64_t *)phys_to_virt(split_pt);
		for (int k = 0; k < 512; k++) {
			split_pt_virt[k] =
				(large_phys + (uint64_t)k * PAGE_SIZE) |
				pte_flags;
		}
		// Replace 2MB PDE with pointer to new 4KB page table
		pd[pd_index] = split_pt | PAGE_PRESENT | PAGE_WRITABLE;
		if (mm_debug_pt) {
			kprintf("PT: Split 2MB page pd[%lu] phys=0x%lx into 4KB pages, PT at 0x%lx\n",
				pd_index, large_phys, split_pt);
		}
	}

	uint64_t pt_phys = pd[pd_index] & PTE_ADDR_MASK;
	uint64_t *pt = (pd[pd_index] & PAGE_PRESENT) ?
			       (uint64_t *)phys_to_virt(pt_phys) :
			       NULL;
	if (!pt && create) {
		pt_phys = allocate_pt_page();
		if (!pt_phys) {
			return NULL;
		}
		// Already zeroed by allocate_pt_page
		uint64_t new_entry = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
		if (mm_debug_pt) {
			kprintf("PT: Creating PT at %p for pd[%lu]\n",
				(void *)pt_phys, pd_index);
		}
		pd[pd_index] = new_entry;
		if (mm_debug_pt) {
			kprintf("PT: Wrote pd[%lu] = 0x%lx, readback = 0x%lx\n",
				pd_index, new_entry, pd[pd_index]);
		}
		pt = (uint64_t *)phys_to_virt(pt_phys);
		if (mm_debug_pt)
			kprintf("PT: Created new PT at %p\n", pt);
	}

	if (mm_debug_pt && pt) {
		kprintf("PT: PT at %p, returning &pt[%lu] = %p\n", pt, pt_index,
			&pt[pt_index]);
	}

	return pt ? &pt[pt_index] : NULL;
}

// Initialize virtual memory manager
void mm_initialize_virtual_memory(void)
{
	kprintf("Initializing Virtual Memory Manager...\n");

	mm_state.pml4_table = (uint64_t *)(get_cr3() & ~0xFFF);

	// Save kernel PML4 for mm_destroy_address_space to know which PML4 not to free
	g_kernel_pml4_phys = get_cr3() & ~0xFFFULL;

	uint64_t heap_start = mm_get_kernel_heap_start();
	mm_state.next_virtual_addr =
		heap_start + KERNEL_HEAP_SIZE + mm_state.bitmap_size;
	mm_state.next_virtual_addr = PAGE_ALIGN(mm_state.next_virtual_addr);

	kprintf("  Page tables at: %p\n", mm_state.pml4_table);
	kprintf("  Next virtual address: %p\n",
		(void *)mm_state.next_virtual_addr);

	kprintf("Virtual Memory Manager initialized\n");
}

// Remap kernel sections with proper NX permissions
__no_stack_protector void mm_remap_kernel_with_nx(void)
{
	kprintf("Remapping kernel with NX permissions...\n");

	uint64_t text_start = (uint64_t)kernel_text_start;
	uint64_t text_end = (uint64_t)kernel_text_end;
	uint64_t rodata_start = (uint64_t)kernel_rodata_start;
	uint64_t rodata_end = (uint64_t)kernel_rodata_end;
	uint64_t data_start = (uint64_t)kernel_data_start;
	uint64_t data_end = (uint64_t)kernel_data_end;
	uint64_t bss_start = (uint64_t)kernel_bss_start;
	uint64_t bss_end = (uint64_t)kernel_bss_end;

	kprintf("  .text:   %p - %p (R-X)\n", (void *)text_start,
		(void *)text_end);
	kprintf("  .rodata: %p - %p (R--)\n", (void *)rodata_start,
		(void *)rodata_end);
	kprintf("  .data:   %p - %p (RW-)\n", (void *)data_start,
		(void *)data_end);
	kprintf("  .bss:    %p - %p (RW-)\n", (void *)bss_start,
		(void *)bss_end);

	// Remap .text section: Present + Executable (no NX bit)
	for (uint64_t addr = PAGE_ALIGN_DOWN(text_start); addr < text_end;
	     addr += PAGE_SIZE) {
		uint64_t phys = mm_get_physical_address(addr);
		if (phys) {
			mm_set_page_flags(addr, PAGE_PRESENT | PAGE_GLOBAL);
		}
	}

	// Remap .rodata section: Present + Non-Executable (NX bit set)
	for (uint64_t addr = PAGE_ALIGN_DOWN(rodata_start); addr < rodata_end;
	     addr += PAGE_SIZE) {
		uint64_t phys = mm_get_physical_address(addr);
		if (phys) {
			mm_set_page_flags(addr, PAGE_PRESENT | PAGE_GLOBAL |
							PAGE_NO_EXECUTE);
		}
	}

	// Remap .data section: Present + Writable + Non-Executable
	for (uint64_t addr = PAGE_ALIGN_DOWN(data_start); addr < data_end;
	     addr += PAGE_SIZE) {
		uint64_t phys = mm_get_physical_address(addr);
		if (phys) {
			mm_set_page_flags(addr, PAGE_PRESENT | PAGE_WRITABLE |
							PAGE_GLOBAL |
							PAGE_NO_EXECUTE);
		}
	}

	// Remap .bss section: Present + Writable + Non-Executable
	for (uint64_t addr = PAGE_ALIGN_DOWN(bss_start); addr < bss_end;
	     addr += PAGE_SIZE) {
		uint64_t phys = mm_get_physical_address(addr);
		if (phys) {
			mm_set_page_flags(addr, PAGE_PRESENT | PAGE_WRITABLE |
							PAGE_GLOBAL |
							PAGE_NO_EXECUTE);
		}
	}

	// Flush all TLB entries to apply new permissions
	mm_flush_all_tlb();

	kprintf("Kernel remapped with NX permissions\n");
}

// Map virtual page to physical page (SMP-safe)
bool mm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags)
{
	VM_BUG_ON(virtual_addr & (PAGE_SIZE - 1));
	VM_BUG_ON(physical_addr & (PAGE_SIZE - 1));
	uint64_t lock_flags;
	spin_lock_irqsave(&mm_kernel_pt_lock, &lock_flags);

	uint64_t *pte = mm_get_page_table(virtual_addr, true);
	if (!pte) {
		spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);
		return false;
	}

	// Detect remap: if PTE was present with a different physical address,
	// other CPUs may have stale TLB entries pointing to the old page (Case B).
	bool needs_shootdown = false;
	if (*pte & PAGE_PRESENT) {
		uint64_t old_phys = *pte & 0x000FFFFFFFFFF000ULL;
		uint64_t new_phys = physical_addr & ~0xFFFULL;
		if (old_phys != new_phys) {
			needs_shootdown = true;
		}
	}

	uint64_t entry = (physical_addr & ~0xFFF) | flags;
	if (mm_debug_pt) {
		kprintf("MAP: pte=%p writing entry=0x%lx\n", pte, entry);
	}
	*pte = entry;
	if (mm_debug_pt) {
		kprintf("MAP: readback *pte=0x%lx\n", *pte);
	}

	spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);
	mm_flush_tlb(virtual_addr);

	// If we replaced an existing mapping pointing to a different physical page,
	// invalidate stale TLB entries on all other CPUs.
	if (needs_shootdown && sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}

	return true;
}

// Map virtual page without TLB shootdown (for batched operations)
// This is useful when mapping multiple pages in a loop - do a single
// smp_tlb_shootdown_sync() after all mappings are complete.
// Note: For fresh mappings (unmapped -> mapped), no shootdown is needed.
// Shootdown is only needed when remapping existing pages.
bool mm_map_page_no_shootdown(uint64_t virtual_addr, uint64_t physical_addr,
			      uint64_t flags)
{
	uint64_t lock_flags;
	spin_lock_irqsave(&mm_kernel_pt_lock, &lock_flags);

	uint64_t *pte = mm_get_page_table(virtual_addr, true);
	if (!pte) {
		spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);
		return false;
	}

	uint64_t entry = (physical_addr & ~0xFFF) | flags;
	*pte = entry;

	spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);
	mm_flush_tlb(virtual_addr);

	return true;
}

// Map device MMIO region into kernel virtual address space.
// Used for PCI BARs that point above the bootloader's 16GB direct map.
// Returns the virtual base address, or 0 on failure.
uint64_t mm_map_mmio(uint64_t phys_addr, size_t num_pages)
{
	if (num_pages == 0)
		return 0;

	uint64_t phys_base = phys_addr & ~0xFFFULL;

	// Allocate a contiguous kernel virtual range from next_virtual_addr
	uint64_t lock_flags;
	spin_lock_irqsave(&mm_kernel_pt_lock, &lock_flags);
	uint64_t cursor_before = mm_state.next_virtual_addr;
	uint64_t virt_base = mm_state.next_virtual_addr;
	mm_state.next_virtual_addr += num_pages * PAGE_SIZE;
	uint64_t cursor_after = mm_state.next_virtual_addr;
	spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);

	// Map each page as present + writable + write-through + cache-disable (UC)
	// MMIO regions must never be cached.
	uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_WRITE_THROUGH |
			 PAGE_CACHE_DISABLE | PAGE_NO_EXECUTE;

	for (size_t i = 0; i < num_pages; i++) {
		uint64_t va = virt_base + i * PAGE_SIZE;
		uint64_t pa = phys_base + i * PAGE_SIZE;
		if (!mm_map_page(va, pa, flags)) {
			kprintf("mm_map_mmio: failed to map VA 0x%lx -> PA 0x%lx\n",
				va, pa);
			// Unmap any pages we already mapped
			for (size_t j = 0; j < i; j++) {
				mm_unmap_page(virt_base + j * PAGE_SIZE);
			}
			return 0;
		}
	}

	kprintf("mm_map_mmio: 0x%lx->0x%lx pa 0x%lx va 0x%lx np %lu\n",
		cursor_before, cursor_after, phys_base, virt_base,
		(unsigned long)num_pages);
	return virt_base + (phys_addr & 0xFFF); // preserve sub-page offset
}

uint64_t mm_map_device_mmio(uint64_t phys_addr, size_t num_pages)
{
	if (num_pages == 0) {
		return 0;
	}

	uint64_t phys_base = phys_addr & ~0xFFFULL;
	uint64_t phys_last = phys_base + ((uint64_t)num_pages - 1) * PAGE_SIZE;
	uint64_t page_offset = phys_addr & 0xFFFULL;
	uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_WRITE_THROUGH |
			 PAGE_CACHE_DISABLE | PAGE_GLOBAL | PAGE_NO_EXECUTE;

	if (phys_last < phys_base) {
		return 0;
	}

	if (is_phys_in_direct_map(phys_last)) {
		uint64_t virt_base = (uint64_t)phys_to_virt(phys_base);

		for (size_t i = 0; i < num_pages; i++) {
			uint64_t va = virt_base + i * PAGE_SIZE;
			uint64_t pa = phys_base + i * PAGE_SIZE;
			if (!mm_map_page(va, pa, flags)) {
				kprintf("mm_map_device_mmio: failed to remap direct-map VA 0x%lx -> PA 0x%lx\n",
					va, pa);
				return 0;
			}
		}

		if (sched_is_smp()) {
			smp_tlb_shootdown_sync();
		}

		//kprintf("mm_map_device_mmio: remapped direct map pa 0x%lx va 0x%lx np %lu\n",
		//        phys_base, virt_base, (unsigned long)num_pages);
		return virt_base + page_offset;
	}

	return mm_map_mmio(phys_addr, num_pages);
}

// Unmap virtual page without TLB shootdown (for batched operations)
// Caller MUST call smp_tlb_shootdown_sync() after unmapping all pages!
void mm_unmap_page_no_shootdown(uint64_t virtual_addr)
{
	uint64_t lock_flags;
	spin_lock_irqsave(&mm_kernel_pt_lock, &lock_flags);

	uint64_t *pte = mm_get_page_table(virtual_addr, false);
	if (pte && (*pte & PAGE_PRESENT)) {
		*pte = 0;
	}

	spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);

	// Flush local TLB only - caller is responsible for cross-CPU shootdown
	mm_flush_tlb(virtual_addr);
}

// Unmap virtual page (SMP-safe, includes TLB shootdown)
void mm_unmap_page(uint64_t virtual_addr)
{
	mm_unmap_page_no_shootdown(virtual_addr);

	// On SMP, other CPUs may have this page cached - do TLB shootdown
	if (sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}
}

/* ============================================================================
 * Batched unmap: flush first, free second
 *
 * Clearing a page-table entry ends the translation on THIS CPU only.  Any
 * other CPU that has touched the address still holds it in its TLB and will
 * keep reaching the page until something invalidates it there too.  Releasing
 * the page before that happens hands it back to the allocator while it is
 * still reachable -- and since freed pages are poisoned, the old owner starts
 * reading 0xFEEDFACE out of memory it believes is its own, while a new owner
 * may be handed the same page.
 *
 * So an unmap has to be done in that order: clear the entries, invalidate them
 * everywhere, and only then drop the references.  This gather collects the
 * pages of a range so one invalidation covers the whole batch -- tearing down
 * a 2MB thread stack is 512 pages, and a cross-CPU round trip each would cost
 * far more than the unmap itself.
 *
 * The whole-address-space teardowns do not need this: mm_destroy_address_space
 * is called only after its callers have already invalidated everywhere (see
 * sched.c and elf_loader.c, which shoot down immediately before).  It is the
 * per-range unmap -- munmap, and MAP_FIXED replacing an existing mapping --
 * that had no cross-CPU invalidation at all.
 * ========================================================================== */

/* The gather hands its array straight to the batched release, so it must not
 * be the larger of the two. */
BUILD_BUG_ON(MM_TLB_GATHER_BATCH > MM_FREE_BATCH_MAX);

void mm_tlb_gather_init(struct mm_tlb_gather *g, uint64_t *pml4)
{
	BUG_ON(g == NULL);
	g->n = 0;
	g->pml4_phys = pml4 ? virt_to_phys(pml4) : 0;
}

/* Queue a page whose page-table entry has ALREADY been cleared.  Its reference
 * is deliberately still held: that is what keeps the page from being reused
 * before the invalidation below. */
void mm_tlb_gather_page(struct mm_tlb_gather *g, uint64_t phys, uint64_t vaddr)
{
	BUG_ON(g == NULL);
	if (!phys)
		return;
	if (g->n == MM_TLB_GATHER_BATCH)
		mm_tlb_gather_flush(g);
	g->vaddrs[g->n] = vaddr & ~0xFFFULL;
	g->pages[g->n++] = phys;
}

/* Pages whose release had to be DEFERRED because their TLB gather ran with
 * interrupts disabled.
 *
 * The shootdown's ack-wait must be able to service the very IPIs it waits
 * on, so it cannot run with IRQs off -- and a COW fault taken from an
 * IRQs-off context (a signal frame written onto a forked child's
 * copy-on-write stack from the IRQ delivery tail is the everyday case)
 * lands exactly there.  Freeing anyway put pages back in the allocator
 * while other CPUs still held stale translations to them: whoever got the
 * page next was scribbled over by a stale writer -- observed for weeks as
 * unexplainable cross-process heap corruption (poisoned GQueue pointers,
 * malloc chunk-header damage) in whatever process was unlucky.
 *
 * So the pages are parked here instead, and released by the next flush
 * that CAN shoot down -- gathers run on every COW fault, so the queue
 * drains within moments on any live system. */
#define TLB_DEFER_RING 1024
static uint64_t g_tlb_defer_pages[TLB_DEFER_RING];
static int g_tlb_defer_n;
static spinlock_t g_tlb_defer_lock = SPINLOCK_INIT("tlbdefer");

void mm_tlb_deferred_drain(void)
{
	if (!g_tlb_defer_n || irqs_disabled())
		return;
	/* Chunked: the batch buffer must stay small on the kernel stack, and
	 * the take-then-shootdown-then-free order per chunk keeps the
	 * invariant (no page freed before its stale translations die) even
	 * against entries added concurrently. */
	for (;;) {
		uint64_t local[64];
		int n = 0;
		uint64_t f;

		spin_lock_irqsave(&g_tlb_defer_lock, &f);
		while (g_tlb_defer_n > 0 && n < 64)
			local[n++] = g_tlb_defer_pages[--g_tlb_defer_n];
		spin_unlock_irqrestore(&g_tlb_defer_lock, f);
		if (!n)
			return;
		/* Every CPU drops every stale translation, then the pages can
		 * safely re-enter the allocator. */
		smp_tlb_shootdown_sync();
		mm_put_pages_batch(local, (unsigned)n);
	}
}

void mm_tlb_gather_flush(struct mm_tlb_gather *g)
{
	BUG_ON(g == NULL);
	/* Piggyback: any flush that CAN shoot down also drains what an
	 * IRQs-off flush had to park. */
	mm_tlb_deferred_drain();
	if (g->n == 0)
		return;

	/* Invalidate everywhere before a single reference is dropped.  These
	 * are one process's mappings, so only the CPUs running its threads can
	 * be holding them -- usually none by the time a process is being torn
	 * down, in which case this costs nothing at all. */
	if (sched_is_smp()) {
		if (likely(!irqs_disabled())) {
			/* A small batch names its pages, so the CPUs holding
			 * this address space invalidate exactly those and no
			 * CPU loses its whole TLB; a large one degrades to the
			 * whole-space form.  The threshold is where receivers'
			 * single-page invalidations stop being cheaper than
			 * one full reload. */
			if (g->n <= TLB_SHOOTDOWN_PAGE_CEILING)
				smp_tlb_shootdown_pages_sync(g->pml4_phys,
							     g->vaddrs, g->n);
			else
				smp_tlb_shootdown_mm_sync(g->pml4_phys);
		} else {
			/* Cannot shoot down from here: the ack-wait needs to
			 * service the very interrupts it is waiting on.  Park
			 * the pages; the next IRQs-on flush releases them
			 * AFTER a proper shootdown.  See g_tlb_defer_pages. */
			uint64_t f;
			int parked = 0;

			spin_lock_irqsave(&g_tlb_defer_lock, &f);
			while (parked < g->n &&
			       g_tlb_defer_n < TLB_DEFER_RING)
				g_tlb_defer_pages[g_tlb_defer_n++] =
					g->pages[parked++];
			spin_unlock_irqrestore(&g_tlb_defer_lock, f);
			if (parked < g->n) {
				/* Ring full (would take a thousand IRQs-off
				 * COW faults with no IRQs-on gather between
				 * them).  Freeing the remainder keeps the old
				 * corruption window for these pages only;
				 * say so. */
				WARN_RATELIMIT(
					1,
					"TLB defer ring full - releasing %d pages with possible stale translations",
					g->n - parked);
				mm_put_pages_batch(g->pages + parked,
						   (unsigned)(g->n - parked));
			}
			g->n = 0;
			return;
		}
	}

	/* One acquisition of the physical allocator's lock for the whole batch
	 * rather than one per page.  The array is already sized to the batch
	 * maximum, so it goes straight through. */
	mm_put_pages_batch(g->pages, g->n);
	g->n = 0;
}

/*
 * Clear one page-table entry and hand its page to the gather.
 *
 * The reference is NOT dropped here -- mm_tlb_gather_flush() drops it once the
 * translation is gone from every CPU.  Returns nothing; a device mapping or an
 * absent entry simply queues nothing.
 */
static void mm_unmap_page_gathered(uint64_t *pml4, uint64_t virtual_addr,
				   struct mm_tlb_gather *g)
{
	uint64_t *pte = mm_get_page_table_from_pml4(pml4, virtual_addr, false);
	uint64_t phys;

	if (!pte || !(*pte & PAGE_PRESENT))
		return;

	/* Device MMIO is not allocator-owned: clear the entry, queue nothing. */
	if (*pte & PAGE_DEVICE) {
		*pte = 0;
		if (pml4 == mm_get_current_address_space())
			mm_flush_tlb(virtual_addr);
		return;
	}

	phys = *pte & 0x000FFFFFFFFFF000ULL;
	*pte = 0;
	if (pml4 == mm_get_current_address_space())
		mm_flush_tlb(virtual_addr);
	mm_tlb_gather_page(g, phys, virtual_addr);
}

// Unmap virtual page in a specific address space

void mm_unmap_page_in_address_space(uint64_t *pml4, uint64_t virtual_addr)
{
	uint64_t *pte = mm_get_page_table_from_pml4(pml4, virtual_addr, false);
	if (pte && (*pte & PAGE_PRESENT)) {
		/* Device MMIO mapping (framebuffer BAR etc.): the physical
		 * page is not allocator-owned — clear the PTE only.  Freeing
		 * it would corrupt the phys bitmap/refcounts when the BAR
		 * address falls inside the managed range. */
		if (*pte & PAGE_DEVICE) {
			*pte = 0;
			if (pml4 == mm_get_current_address_space()) {
				mm_flush_tlb(virtual_addr);
			}
			return;
		}
		/* Drop this mapping's reference.  The page goes away only when
		 * it was the last one, so a page still mapped elsewhere (a COW
		 * page shared with a forked child, say) survives.  Kernel pages
		 * hold exactly the allocator's own reference, so the same call
		 * releases them outright. */
		uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;

		*pte = 0;
		if (phys)
			mm_put_page(phys);
		// Flush TLB if this is the current address space
		if (pml4 == mm_get_current_address_space()) {
			mm_flush_tlb(virtual_addr);
		}
	}
}

/* ---- the task's mmap region table ---------------------------------------
 *
 * The records describing what a process has mapped.  They sit next to the
 * unmap machinery above because the two must move together: a record and its
 * pages are one thing, and every bug in this area so far has been the two
 * getting out of step.
 */


/* Give a task its region table.  Called once, when the task is created. */
bool mm_regions_init(task_t *task)
{
	size_t bytes;

	if (!task)
		return false;
	bytes = (size_t)MMAP_REGIONS_INITIAL * sizeof(mmap_region_t);
	task->mmap_regions = (mmap_region_t *)kalloc(bytes);
	if (!task->mmap_regions) {
		task->mmap_capacity = 0;
		task->mmap_hwm = 0;
		task->mmap_hint = 0;
		return false;
	}
	mm_memset(task->mmap_regions, 0, bytes);
	task->mmap_capacity = MMAP_REGIONS_INITIAL;
	task->mmap_hwm = 0;
	task->mmap_hint = 0;
	return true;
}

/* Release it.  Every task owns its own allocation, including a thread's empty
 * one, so this is unconditional at teardown. */
/* A region record's references: its backing file, and -- for a mapping of
 * a driver object -- the object itself.  Every record holds its own, so a
 * split takes one more and a merge or release drops one. */
void mm_region_ref_hold(mmap_region_t *r)
{
	if (r->file)
		vfs_incref(r->file);
	if (r->dev_obj && r->dev_get)
		r->dev_get(r->dev_obj);
}

void mm_region_ref_drop(mmap_region_t *r)
{
	if (r->file) {
		vfs_close(r->file);
		r->file = NULL;
	}
	if (r->dev_obj && r->dev_put)
		r->dev_put(r->dev_obj);
	r->dev_obj = NULL;
}

void mm_regions_free(task_t *task)
{
	if (!task || !task->mmap_regions)
		return;

	/* Drop what the records still hold.
	 *
	 * Every other path that retires a region closes its file first --
	 * munmap does, exec does -- but this one, the path EVERY exiting
	 * process takes, only freed the array.  So a process that died with a
	 * file-backed mapping still open (every shared library, and every
	 * shmat) leaked that reference permanently: the vfs_file was never
	 * closed, and for a shm object that reference is exactly what keeps its
	 * pages alive.  Memory stayed used with no process left to own it, and
	 * no amount of waiting gave it back. */
	for (uint32_t i = 0; i < task->mmap_capacity; i++) {
		mmap_region_t *r = &task->mmap_regions[i];

		if (r->in_use)
			mm_region_ref_drop(r);
	}
	kfree(task->mmap_regions);
	task->mmap_regions = NULL;
	task->mmap_capacity = 0;
	/* The bound and the hint describe a table that no longer exists.  exec
	 * calls this and then builds a new one, so leaving them set would have
	 * the new table scanned against the old table's extent. */
	task->mmap_hwm = 0;
	task->mmap_hint = 0;
}

/* Replace a task's table with a copy of `src`'s.
 *
 * fork() builds the child by copying the parent's task_t wholesale, which
 * copies the POINTER -- parent and child would share one table and free it
 * twice.  This gives the child its own, with the parent's contents; the file
 * references those records hold are taken by the caller, which is the only
 * place that knows whether it is cloning an address space or starting an
 * empty one. */
bool mm_regions_clone(task_t *dst, const task_t *src)
{
	size_t bytes;
	uint32_t cap;

	if (!dst || !src)
		return false;
	cap = src->mmap_capacity ? src->mmap_capacity : MMAP_REGIONS_INITIAL;
	bytes = (size_t)cap * sizeof(mmap_region_t);
	dst->mmap_regions = (mmap_region_t *)kalloc(bytes);
	if (!dst->mmap_regions) {
		dst->mmap_capacity = 0;
		dst->mmap_hwm = 0;
		dst->mmap_hint = 0;
		return false;
	}
	mm_memset(dst->mmap_regions, 0, bytes);
	if (src->mmap_regions && src->mmap_capacity)
		mm_memcpy(dst->mmap_regions, src->mmap_regions,
			  (size_t)src->mmap_capacity * sizeof(mmap_region_t));
	dst->mmap_capacity = cap;
	/* The table is copied slot for slot, so the child's bound is the
	 * parent's.  Leaving it at zero would make every one of the child's
	 * inherited mappings invisible to the lookup -- the copy would be
	 * there and nothing would find it. */
	dst->mmap_hwm = src->mmap_hwm > cap ? cap : src->mmap_hwm;
	dst->mmap_hint = 0;
	return true;
}

/* Clone `src`'s region table into `dst` AND take the reference every
 * file-backed region owes its backing file -- as one locked step.
 *
 * The two halves cannot be separated.  munmap() releases those same references
 * while holding this lock for writing, so a copy taken outside it can name a
 * file that is released before the reference is taken; the child then carries
 * a pointer it does not own and destroys somebody else's reference when it
 * execs or exits.  Both fork paths did exactly that -- one with the incref
 * seventy lines further down -- which is why this lives here rather than being
 * written out at each call site.
 *
 * Read, not write: only `src` is read, so concurrent forks need not wait on
 * each other.  Excluding the writers is the whole requirement.
 *
 * Only an in-use slot owns a reference: the table is copied whole, stale
 * pointers in unused slots and all, and referencing those would be as wrong as
 * releasing them.
 */
bool mm_regions_clone_ref(task_t *dst, task_t *src)
{
	bool ok;

	if (!dst || !src)
		return false;

	mm_read_lock(&src->mmap_lock);
	ok = mm_regions_clone(dst, src);
	if (ok) {
		for (uint32_t i = 0; i < dst->mmap_capacity; i++) {
			if (dst->mmap_regions[i].in_use)
				mm_region_ref_hold(&dst->mmap_regions[i]);
		}
	}
	mm_read_unlock(&src->mmap_lock);
	return ok;
}

/* Double the table, up to the ceiling.  Returns false only when the ceiling is
 * reached or memory has run out -- both of which mean the next mmap fails. */
static bool mm_regions_grow(task_t *task)
{
	uint32_t old_cap = task->mmap_capacity;
	uint32_t new_cap;
	mmap_region_t *grown;
	size_t bytes;

	if (old_cap >= TASK_MAX_MMAP)
		return false;
	new_cap = old_cap ? old_cap * 2 : MMAP_REGIONS_INITIAL;
	if (new_cap > TASK_MAX_MMAP)
		new_cap = TASK_MAX_MMAP;

	bytes = (size_t)new_cap * sizeof(mmap_region_t);
	grown = (mmap_region_t *)kalloc(bytes);
	if (!grown)
		return false;
	mm_memset(grown, 0, bytes);
	if (task->mmap_regions && old_cap)
		mm_memcpy(grown, task->mmap_regions,
			  (size_t)old_cap * sizeof(mmap_region_t));

	/* Callers hold pointers into the table only for the duration of a
	 * single mmap, under the address-space write lock, and this runs
	 * before any such pointer is handed out -- so swapping the array is
	 * safe here and nowhere else. */
	kfree(task->mmap_regions);
	task->mmap_regions = grown;
	task->mmap_capacity = new_cap;
	return true;
}

/* Claim a free region slot.  in_use is left false -- the caller sets it once
 * the mapping is fully built, so a failure part-way leaks nothing. */
mmap_region_t *mm_alloc_mmap_region(task_t *task)
{
	if (!task->mmap_regions && !mm_regions_init(task))
		return NULL;

	for (uint32_t i = 0; i < task->mmap_capacity; i++) {
		if (!task->mmap_regions[i].in_use) {
			/* Scrub the recycled slot: stale fields (device,
			 * file, lazy) from a previous mapping must never
			 * leak into a new region. */
			mm_memset(&task->mmap_regions[i], 0,
				  sizeof(mmap_region_t));
			/* Raised at the CLAIM, not when the caller finally
			 * sets in_use: the slot is spoken for from here, and
			 * an over-estimate only costs a longer scan while an
			 * under-estimate would hide a live mapping. */
			if (i + 1 > task->mmap_hwm)
				task->mmap_hwm = i + 1;
			return &task->mmap_regions[i];
		}
	}

	/* Full: grow, then take the first free slot of the enlarged table.  The
	 * scan is repeated rather than assuming where the new space begins --
	 * the growth is a doubling except at the ceiling, where it is clamped.
	 * The table starts small and doubles, so a process pays for the regions
	 * it has rather than for the ceiling. */
	if (mm_regions_grow(task)) {
		for (uint32_t i = 0; i < task->mmap_capacity; i++) {
			if (task->mmap_regions[i].in_use)
				continue;
			mm_memset(&task->mmap_regions[i], 0,
				  sizeof(mmap_region_t));
			if (i + 1 > task->mmap_hwm)
				task->mmap_hwm = i + 1;
			return &task->mmap_regions[i];
		}
	}

	/* Out of region slots.  Every mapping from here on fails with ENOMEM,
	 * and a program does not experience that as "no more mappings" -- it
	 * experiences it as an allocation returning nothing, reported by
	 * whichever library happened to ask, in terms that have nothing to do
	 * with mappings.  Say it plainly here instead, once per boot, so the
	 * next occurrence does not have to be deduced from the symptom. */
	WARN_RATELIMIT(
		1,
		"mmap: %s (pid %d) has used all %d region slots; further mappings will fail",
		task->comm[0] ? task->comm : "?", task->id, TASK_MAX_MMAP);
	return NULL;
}

/* Find the in-use region covering `addr`, or NULL.
 *
 * Two things keep this off the whole table: the slot that answered last time
 * is tried first (faults come in runs inside one mapping), and the scan that
 * follows stops at the high-water mark rather than the capacity.  Both are
 * described where they are declared; both are hints, and a wrong one costs a
 * scan, never a wrong answer -- the region is re-checked against `addr'
 * either way. */
mmap_region_t *mm_find_mmap_region(task_t *task, uint64_t addr)
{
	uint32_t hint = task->mmap_hint;
	uint32_t end = task->mmap_hwm;

	if (end > task->mmap_capacity)
		end = task->mmap_capacity;
	if (hint < end) {
		mmap_region_t *r = &task->mmap_regions[hint];

		if (r->in_use && addr >= r->start &&
		    addr < r->start + r->length)
			return r;
	}
	for (uint32_t i = 0; i < end; i++) {
		mmap_region_t *r = &task->mmap_regions[i];
		if (r->in_use && addr >= r->start &&
		    addr < r->start + r->length) {
			task->mmap_hint = i;
			return r;
		}
	}
	return NULL;
}

/*
 * The region covering *addr if there is one, otherwise the next region above
 * it -- with *addr moved forward to that region's start.  NULL when nothing
 * remains at or above *addr.
 *
 * This is what lets a walk cross a gap in one step.  Advancing a page at a
 * time instead means a full scan of the region table for every 4KB of empty
 * address space, and these tables run close to their limit in the programs
 * that unmap the most: a single call over a large sparse range spent longer
 * looking for regions that were not there than it did on the pages that were.
 */
static mmap_region_t *mm_region_at_or_after(task_t *task, uint64_t *addr)
{
	mmap_region_t *best = mm_find_mmap_region(task, *addr);

	if (best)
		return best;

	for (uint32_t i = 0; i < task->mmap_capacity; i++) {
		mmap_region_t *r = &task->mmap_regions[i];

		if (!r->in_use || r->start < *addr)
			continue;
		/* A record of no length describes nothing, and callers advance
		 * by the region's extent -- returning one would leave them
		 * exactly where they were, forever.  Nothing should produce
		 * one, so say so rather than just stepping over it. */
		if (WARN_ON(r->length == 0))
			continue;
		if (!best || r->start < best->start)
			best = r;
	}
	if (best)
		*addr = best->start;
	return best;
}

/*
 * Tear down every mapping in [addr, addr+length): free the pages AND release
 * or trim the mmap_region_t records that covered them.  Returns 1 if anything
 * was found, 0 if the range held no mapping.
 *
 * Both munmap and MAP_FIXED need exactly this.  MAP_FIXED used to unmap the
 * pages and leave the records behind, so every remap of an existing mapping
 * burned a region slot permanently -- and rtld does MAP_FIXED for every DSO
 * segment.  Claws Mail reached the 512-slot cap with 510 records still marked
 * lazy and 893MB of "anonymous" that no longer existed, after which every
 * mmap() returned -ENOMEM: GTK could not map its GResource bundle, so
 * GtkFileChooserDialog's template failed to build and the attach dialog came
 * up as an empty box with two buttons.
 */
/*
 * Are these two records really one mapping?
 *
 * A mapping is identified by what it describes, not by the call that happened
 * to create it.  Two records that abut and agree on every property are one
 * region, and recording them separately is what makes the table grow without
 * bound: a program that trims and re-grows a heap in cycles spends a slot on
 * every cycle until mmap() starts failing -- far from the code responsible,
 * and usually inside some library that reports nothing useful.
 */
static bool mm_regions_mergeable(const mmap_region_t *a, const mmap_region_t *b)
{
	if (!a->in_use || !b->in_use)
		return false;
	if (a->start + a->length != b->start) /* must abut exactly */
		return false;
	if (a->prot != b->prot || a->flags != b->flags)
		return false;
	if (a->lazy != b->lazy || a->device != b->device)
		return false;
	/* Device mappings carry a physical base as well as a virtual one, and
	 * two that abut in virtual space need not abut in physical space.
	 * Merging those would leave one record claiming a physical run that
	 * was never contiguous. */
	if (a->device && a->device_phys + a->length != b->device_phys)
		return false;
	if (a->file != b->file)
		return false;
	if (a->dev_obj != b->dev_obj)
		return false;
	/* Same file: the offsets have to run on without a break, or the two
	 * halves are showing different parts of it. */
	if (a->file && a->offset + a->length != b->offset)
		return false;
	return true;
}

/*
 * Coalesce a freshly installed region with the neighbours it abuts.
 *
 * mmap() had no merge step at all: only munmap() ran one.  So every mmap()
 * consumed a slot even when the new mapping simply continued an existing one
 * with identical protection, flags and backing -- which is the normal shape of
 * a heap growing, of an allocator taking another arena, and of a program that
 * maps and releases as it works.  A conventional Unix coalesces adjacent
 * mappings as they are made; without that the table only ever grew, and a
 * long-lived process ran out of slots with most of them describing pieces of
 * what should have been a handful of runs.  Browsing directories in a file
 * manager did it in a few dozen steps, after which every further mmap failed
 * and the session wedged.
 *
 * Two single scans are enough and no sort is needed, unlike the whole-table
 * pass below.  Regions never overlap, so at most one record can begin exactly
 * where this one ends and at most one can end exactly where it begins: absorb
 * the successor, then let the predecessor absorb the result.  That also covers
 * a mapping dropped exactly into a hole between two others, which collapses
 * all three into one.
 */
void mm_merge_region_neighbours(task_t *task, mmap_region_t *region)
{
	if (!task || !region || !region->in_use)
		return;

	for (uint32_t i = 0; i < task->mmap_capacity; i++) {
		mmap_region_t *b = &task->mmap_regions[i];

		if (b == region || !mm_regions_mergeable(region, b))
			continue;
		region->length += b->length;
		/* Each record held its own reference; the one that goes away
		 * drops its own. */
		mm_region_ref_drop(b);
		b->in_use = false;
		b->length = 0;
		break;
	}

	for (uint32_t i = 0; i < task->mmap_capacity; i++) {
		mmap_region_t *a = &task->mmap_regions[i];

		if (a == region || !mm_regions_mergeable(a, region))
			continue;
		a->length += region->length;
		mm_region_ref_drop(region);
		region->in_use = false;
		region->length = 0;
		break;
	}
}

/*
 * Coalesce every run of adjacent, identical records into one.
 *
 * Run after anything that changes the shape of the table.
 *
 * Records only ever merge with records they abut, so putting them in address
 * order first turns the whole job into one pass: each record absorbs its
 * successors for as long as they join on, and every record is looked at once.
 *
 * The straightforward version compared every record against every other one,
 * which is the square of the table size on every call -- and the programs that
 * unmap most are exactly the ones whose tables run close to full, so it was
 * always the worst case, roughly a quarter of a million comparisons for each
 * munmap().  A process that trims and regrows its heap does that thousands of
 * times on its way out.
 */
void mm_merge_mmap_regions(task_t *task)
{
	/* Slot indices, not pointers: a quarter of the footprint.
	 *
	 * On the heap, not the stack.  This was `uint16_t order[TASK_MAX_MMAP]'
	 * back when the table was a fixed 512 entries; at the ceiling the table
	 * now allows that is 127 KB of a 16 KB kernel stack, which is not a
	 * near miss but an immediate overflow.  It is sized by what the task
	 * actually has, so the ordinary case allocates a few hundred bytes. */
	uint16_t *order;
	int n = 0;

	if (!task || !task->mmap_regions)
		return;

	BUILD_BUG_ON(TASK_MAX_MMAP > 0xFFFF);

	order = (uint16_t *)kalloc((size_t)task->mmap_capacity *
				   sizeof(uint16_t));
	if (!order)
		return; /* Coalescing is an optimisation; skipping it is safe. */

	for (uint32_t i = 0; i < task->mmap_capacity; i++) {
		if (task->mmap_regions[i].in_use)
			order[n++] = (uint16_t)i;
	}
	if (n < 2) {
		kfree(order);
		return;
	}

	/* Shell sort by start address: short, no recursion, no scratch, and
	 * unbothered by whatever order the slots happen to have been handed
	 * out in -- which is arbitrary, since freed slots are reused. */
	{
		static const int gaps[] = { 127, 57, 23, 10, 4, 1 };

		for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
			int gap = gaps[g];

			for (int i = gap; i < n; i++) {
				uint16_t key = order[i];
				uint64_t key_start =
					task->mmap_regions[key].start;
				int j = i;

				while (j >= gap &&
				       task->mmap_regions[order[j - gap]].start >
					       key_start) {
					order[j] = order[j - gap];
					j -= gap;
				}
				order[j] = key;
			}
		}
	}

	/* One pass in address order.  Each record absorbs the run of records
	 * that join on to it, and the walk resumes past that run, so nothing
	 * is examined twice. */
	{
		int i = 0;

		while (i < n) {
			mmap_region_t *a = &task->mmap_regions[order[i]];
			int j = i + 1;

			while (j < n) {
				mmap_region_t *b = &task->mmap_regions[order[j]];

				if (!mm_regions_mergeable(a, b))
					break;
				a->length += b->length;
				/* Each record held its own reference; the one
				 * that goes away drops its own. */
				mm_region_ref_drop(b);
				b->in_use = false;
				b->length = 0;
				j++;
			}
			i = j;
		}
	}

	kfree(order);
}

/*
 * Count the pages a task actually has resident.
 *
 * Walks the user half of the page tables and counts what is present.  This is
 * the real resident set: what the process would lose if it were killed, as
 * opposed to how much address space it has reserved.
 *
 * The two are very different numbers here, and reporting one as the other is
 * misleading in a specific and expensive way: an allocator that hands physical
 * memory back while keeping its address space -- which is exactly what
 * releasing pages with MADV_DONTNEED does -- looks like a process leaking
 * without bound, because the address space only ever grows.
 */
uint64_t mm_count_resident_pages(uint64_t *pml4)
{
	uint64_t pages = 0;

	if (!pml4)
		return 0;

	/* User half only; entries 256+ are the kernel's and shared by all. */
	for (int i = 0; i < 256; i++) {
		if (!(pml4[i] & PAGE_PRESENT))
			continue;
		uint64_t *pdpt =
			(uint64_t *)phys_to_virt(pml4[i] & PTE_ADDR_MASK);

		for (int j = 0; j < 512; j++) {
			if (!(pdpt[j] & PAGE_PRESENT))
				continue;
			if (pdpt[j] & PAGE_SIZE_FLAG) { /* 1GB */
				pages += 512ULL * 512ULL;
				continue;
			}
			uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] &
								PTE_ADDR_MASK);

			for (int k = 0; k < 512; k++) {
				if (!(pd[k] & PAGE_PRESENT))
					continue;
				if (pd[k] & PAGE_SIZE_FLAG) { /* 2MB */
					pages += 512;
					continue;
				}
				uint64_t *pt = (uint64_t *)phys_to_virt(
					pd[k] & PTE_ADDR_MASK);

				for (int l = 0; l < 512; l++) {
					/* Device mappings are somebody else's
					 * memory (a framebuffer BAR); they are
					 * mapped, not held. */
					if ((pt[l] & PAGE_PRESENT) &&
					    !(pt[l] & PAGE_DEVICE))
						pages++;
				}
			}
		}
	}
	return pages;
}

/*
 * Drop the pages of [addr, addr+length) without touching the mapping.
 *
 * The mapping, its region record and its protections all survive; only the
 * physical pages go.  A later access finds nothing present, the fault handler
 * sees the region is still there, and materialises a fresh zero page -- so the
 * caller gets its memory back and the range keeps reading as zeros.
 *
 * This exists because an allocator that trims a heap wants exactly this and
 * nothing else.  Reaching for munmap instead punches a hole in the middle of a
 * live mapping, which has to be recorded as two records where there was one,
 * and costs a region slot on every trim.  A long-running program that allocates
 * and frees in cycles exhausts the table that way -- the mmap that then fails
 * is nowhere near the code that caused it, and what it breaks (an allocation
 * deep inside a library) rarely reports anything useful.
 *
 * Only whole pages are released: a partial page at either end still holds live
 * data belonging to the caller.
 */
void mm_dontneed_range(task_t *task, uint64_t addr, uint64_t length)
{
	uint64_t start = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t end = (addr + length) & ~(uint64_t)(PAGE_SIZE - 1);
	struct mm_tlb_gather gather;

	mm_assert_write_locked(&task->mmap_lock);
	WARN_ON(irqs_disabled());
	if (end <= start)
		return;

	/* Region by region, not page by page.  The lookup is a scan of the
	 * whole region table, so doing it per page made the cost of a trim
	 * depend on the size of the range squared against the table -- a 64MB
	 * heap released in one call is 16,384 pages, each paying a 512-entry
	 * scan, for a table that describes at most a few hundred mappings. */
	mm_tlb_gather_init(&gather, task->pml4);
	{
		uint64_t va = start;

		while (va < end) {
			mmap_region_t *r = mm_region_at_or_after(task, &va);
			uint64_t region_end, stop;

			if (!r || va >= end)
				break;

			region_end = r->start + r->length;
			stop = (end < region_end) ? end : region_end;

			/* Only inside a mapping this task actually has, and
			 * never a file-backed one: dropping those would lose
			 * writes that were never written back. */
			if (!r->file) {
				for (; va < stop; va += PAGE_SIZE)
					mm_unmap_page_gathered(task->pml4, va,
							       &gather);
			}
			va = stop;
		}
	}
	mm_tlb_gather_flush(&gather);
}

int mm_unmap_range_and_regions(task_t *task, uint64_t addr, uint64_t length)
{
	uint64_t cur_addr = addr;
	uint64_t end_addr = addr + length;
	int freed_any = 0;
	struct mm_tlb_gather gather;

	/* Runs with the address-space semaphore held for writing, from a
	 * syscall -- so the cross-CPU invalidation the gather performs is
	 * legal here.  Asserted rather than assumed: flushing with interrupts
	 * disabled cannot invalidate anywhere but locally. */
	mm_assert_write_locked(&task->mmap_lock);
	WARN_ON(irqs_disabled());
	mm_tlb_gather_init(&gather, task->pml4);

	while (cur_addr < end_addr) {
		/* Crossing a gap costs one step, not one per page: unmapping a
		 * large sparse range used to scan the whole region table for
		 * every 4KB of address space that held nothing. */
		mmap_region_t *region = mm_region_at_or_after(task, &cur_addr);

		if (!region || cur_addr >= end_addr)
			break;

		uint64_t region_end = region->start + region->length;
		uint64_t unmap_end =
			(end_addr < region_end) ? end_addr : region_end;

		/* A watched device mapping: the entries about to be cleared
		 * carry the only record of what the processor wrote through
		 * them, so each written page is handed to the tracker before
		 * the record goes.  Without this, write-then-unmap loses the
		 * write: the pages still hold the data, but nothing is left
		 * to say the device's copy is behind. */
		if (region->device && region->dev_dirty &&
		    region->dev_dirty->page_dirty && region->dev_obj) {
			for (uint64_t va = cur_addr; va < unmap_end;
			     va += PAGE_SIZE) {
				uint64_t *pte = mm_get_page_table_from_pml4(
					task->pml4, va, false);

				if (pte && (*pte & PAGE_PRESENT) &&
				    (*pte & PAGE_DEVICE) &&
				    (*pte & PAGE_DIRTY))
					region->dev_dirty->page_dirty(
						region->dev_obj,
						region->offset +
							(va - region->start));
			}
		}

		/* Clear the entries within [cur_addr, unmap_end).  The pages
		 * are not released yet -- the gather holds them until every
		 * CPU has dropped the translation. */
		for (uint64_t va = cur_addr; va < unmap_end; va += PAGE_SIZE)
			mm_unmap_page_gathered(task->pml4, va, &gather);

		/* Update or free the region record. */
		if (cur_addr == region->start && unmap_end == region_end) {
			region->in_use = false;
			mm_region_ref_drop(region);
			region->lazy = false;
		} else if (cur_addr == region->start) {
			/* Keep file_off = offset + (addr - start) invariant
			 * when the region head is trimmed. */
			region->offset += unmap_end - region->start;
			region->start = unmap_end;
			region->length = region_end - unmap_end;
		} else if (unmap_end == region_end) {
			region->length = cur_addr - region->start;
		} else {
			/* The range falls strictly inside the region, so the
			 * two surviving halves need two records.  malloc trims
			 * the middle out of its mmapped chunks, so this is a
			 * real case, not a corner: leaving one record spanning
			 * the hole would let a fault in the freed middle
			 * silently materialise a fresh anonymous page. */
			/* `region' is a pointer INTO the table, and claiming a
			 * slot can grow the table -- which reallocates it and
			 * frees the old array.  So the index is taken first and
			 * the pointer rebuilt afterwards.
			 *
			 * Without that, every split that happened to be the
			 * allocation which grew the table read and wrote freed
			 * memory: `*tail = *region' copied from the old array
			 * and `region->length' below wrote into it.  The
			 * surviving record kept whatever the freed block
			 * happened to hold, so a process's own map quietly
			 * stopped describing its address space -- no fault, no
			 * warning, just a program working from wrong records.
			 * malloc trims the middle out of its mmapped chunks, so
			 * this path runs constantly. */
			size_t ridx = (size_t)(region - task->mmap_regions);
			mmap_region_t *tail = mm_alloc_mmap_region(task);

			region = &task->mmap_regions[ridx];
			if (tail) {
				*tail = *region;
				tail->start = unmap_end;
				tail->length = region_end - unmap_end;
				tail->offset = region->offset +
					       (unmap_end - region->start);
				/* Each record closes its own reference. */
				mm_region_ref_hold(tail);
				tail->in_use = true;
			}
			/* With no slot for the tail its pages stay mapped but
			 * unrecorded -- still better than a record spanning the
			 * hole, and the table is already exhausted at that
			 * point, which sys_mmap reports loudly. */
			region->length = cur_addr - region->start;
		}

		freed_any = 1;
		cur_addr = region_end;
	}

	/* Invalidate on every CPU, then release everything collected above.
	 * Nothing has been handed back to the allocator before this point. */
	mm_tlb_gather_flush(&gather);
	/* Trimming and splitting above may have left records that abut and
	 * describe the same thing; fold them back together so repeated
	 * map/unmap cycles cannot grow the table without bound. */
	mm_merge_mmap_regions(task);
	return freed_any;
}

// Get physical address for virtual address
uint64_t mm_get_physical_address(uint64_t virtual_addr)
{
	// Fast path: direct-map addresses (phys_to_virt region) are a simple
	// subtraction — no page-table walk needed.  This also avoids breakage
	// when the direct map uses 2MB or 1GB huge pages, which the 4KB-level
	// page-table walker below cannot resolve.
	if (is_direct_map_addr(virtual_addr)) {
		return virtual_addr - PHYS_MAP_BASE;
	}

	uint64_t *pte = mm_get_page_table(virtual_addr, false);
	if (mm_debug_pt) {
		kprintf("GET_PHYS: vaddr=%p pte=%p *pte=0x%lx\n",
			(void *)virtual_addr, pte, pte ? *pte : 0);
	}
	if (pte && (*pte & PAGE_PRESENT)) {
		// Mask out NX bit (63) and other reserved bits, keep only physical address bits 12-51
		return (*pte & 0x000FFFFFFFFFF000ULL) | (virtual_addr & 0xFFF);
	}
	return 0;
}

// Check if page is mapped
bool mm_is_page_mapped(uint64_t virtual_addr)
{
	uint64_t *pte = mm_get_page_table(virtual_addr, false);
	return pte && (*pte & PAGE_PRESENT);
}

// ============================================================================
// KERNEL STACK GUARD PAGE ALLOCATOR
// ============================================================================

// Mark a single 4 KB page not-present.  If it is covered by a 2 MB large
// mapping, mm_get_page_table(create=true) will split it into 4 KB entries
// first so only the requested page is affected.
//
// When the page was previously present and backed by a physical frame
// owned by the kernel (this is the case for BSS-static stacks such as
// interrupt_stack / bsp_ist*_stack / ap_interrupt_stacks / ap_ist*_stacks,
// whose backing pages were mapped by the bootloader and reserved by
// reserve_bootloader_mapped_pages()), the unmapped physical page would
// otherwise leak — there is no other virtual mapping for it once we
// zero the PTE.  We capture the old phys, complete the TLB shootdown
// so no CPU still holds a stale translation, and then return the page
// to the physical allocator.  For never-mapped guard slots (e.g. the
// guard below a stack created by mm_alloc_guarded_kstack, which is
// intentionally allocated as one extra unmapped page in the virtual
// range), the PTE is already zero / non-present and no page is freed.
void mm_mark_guard_page(uint64_t virt_addr)
{
	uint64_t page_va = virt_addr & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t lock_flags;
	uint64_t old_phys = 0;

	spin_lock_irqsave(&mm_kernel_pt_lock, &lock_flags);
	// create=true splits 2 MB pages as needed; we then zero the PTE.
	uint64_t *pte = mm_get_page_table(page_va, true);
	if (pte) {
		/* If it was a live 4 KiB mapping, remember the backing frame so we
         * can return it to the allocator AFTER the TLB shootdown completes.
         * Skip if it's a 2 MiB large page — mm_get_page_table(create=true)
         * is supposed to have split it, but be defensive: only free 4 KiB
         * frames so we never accidentally free 2 MiB of unrelated memory.
         * mm_get_page_table(create=true) returns the PTE, not the PDE, so
         * a successful return here implies a 4 KiB-granular leaf entry. */
		if (*pte & PAGE_PRESENT) {
			old_phys = *pte & PTE_ADDR_MASK;
		}
		*pte = 0;
	}
	spin_unlock_irqrestore(&mm_kernel_pt_lock, lock_flags);

	mm_flush_tlb(page_va);
	if (sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}

	/* The mapping is now gone on every CPU; the physical frame, if any,
     * is no longer reachable from any virtual address — return it to the
     * allocator instead of leaking it.  This is called once per guard
     * page during boot (tss_init, tss_init_ap), so up to ~260 pages on
     * a fully-populated 64-CPU system; without this each cold boot loses
     * ~1 MiB to the BSS-backed stack guard pages. */
	if (old_phys) {
		mm_free_physical_page(old_phys);
	}
}

// Allocate a kernel stack with a guard page immediately below the usable area.
// Returns the base of the usable region, or NULL on failure.
// usable_size must be a non-zero multiple of PAGE_SIZE.
void *mm_alloc_guarded_kstack(size_t usable_size)
{
	if (!usable_size || (usable_size & (PAGE_SIZE - 1))) {
		return NULL;
	}

	size_t total = PAGE_SIZE + usable_size; /* guard page + usable pages */

	// Obtain a virtual slot: prefer recycled slots to avoid exhausting the range.
	uint64_t slot_base;
	uint64_t irq;
	spin_lock_irqsave(&kstack_virt_lock, &irq);
	if (kstack_recycled_count > 0) {
		slot_base = kstack_recycled[--kstack_recycled_count];
	} else {
		if (kstack_virt_next + total > KSTACK_VIRT_LIMIT) {
			spin_unlock_irqrestore(&kstack_virt_lock, irq);
			return NULL;
		}
		slot_base = kstack_virt_next;
		kstack_virt_next += total;
	}
	spin_unlock_irqrestore(&kstack_virt_lock, irq);

	// Guard page at slot_base is intentionally left unmapped.
	// Map usable stack pages immediately above the guard page.
	uint64_t stack_base = slot_base + PAGE_SIZE;
	size_t num_pages = usable_size / PAGE_SIZE;

	for (size_t i = 0; i < num_pages; i++) {
		uint64_t phys = mm_allocate_physical_page();
		if (!phys) {
			// Allocation failed — unmap and free pages already mapped.
			for (size_t j = 0; j < i; j++) {
				uint64_t va = stack_base + j * PAGE_SIZE;
				uint64_t p = mm_get_physical_address(va);
				mm_unmap_page(va);
				if (p)
					mm_free_physical_page(p);
			}
			// Return the slot to the recycle pool.
			spin_lock_irqsave(&kstack_virt_lock, &irq);
			if (kstack_recycled_count < KSTACK_MAX_RECYCLED) {
				kstack_recycled[kstack_recycled_count++] =
					slot_base;
			}
			spin_unlock_irqrestore(&kstack_virt_lock, irq);
			return NULL;
		}
		if (!mm_map_page(stack_base + i * PAGE_SIZE, phys,
				 PAGE_PRESENT | PAGE_WRITABLE |
					 PAGE_NO_EXECUTE)) {
			mm_free_physical_page(phys);
			for (size_t j = 0; j < i; j++) {
				uint64_t va = stack_base + j * PAGE_SIZE;
				uint64_t p = mm_get_physical_address(va);
				mm_unmap_page(va);
				if (p)
					mm_free_physical_page(p);
			}
			spin_lock_irqsave(&kstack_virt_lock, &irq);
			if (kstack_recycled_count < KSTACK_MAX_RECYCLED) {
				kstack_recycled[kstack_recycled_count++] =
					slot_base;
			}
			spin_unlock_irqrestore(&kstack_virt_lock, irq);
			return NULL;
		}
	}

	return (void *)stack_base;
}

// Free a kernel stack returned by mm_alloc_guarded_kstack().
void mm_free_guarded_kstack(void *stack_base, size_t usable_size)
{
	if (!stack_base || !usable_size || (usable_size & (PAGE_SIZE - 1))) {
		return;
	}

	uint64_t stack_va = (uint64_t)stack_base;
	uint64_t slot_base = stack_va - PAGE_SIZE;
	size_t num_pages = usable_size / PAGE_SIZE;

	for (size_t i = 0; i < num_pages; i++) {
		uint64_t va = stack_va + i * PAGE_SIZE;
		uint64_t phys = mm_get_physical_address(va);
		mm_unmap_page_no_shootdown(va);
		if (phys)
			mm_free_physical_page(phys);
	}

	// One shootdown covers all unmapped pages.
	if (sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}

	// Return the slot so it can be reused.
	uint64_t irq;
	spin_lock_irqsave(&kstack_virt_lock, &irq);
	if (kstack_recycled_count < KSTACK_MAX_RECYCLED) {
		kstack_recycled[kstack_recycled_count++] = slot_base;
	}
	spin_unlock_irqrestore(&kstack_virt_lock, irq);
}

// KERNEL HEAP ALLOCATOR IMPLEMENTATION

// Initialize heap
void mm_initialize_heap(void)
{
	kprintf("Initializing Kernel Heap Allocator...\n");

	uint64_t heap_start = mm_get_kernel_heap_start();
	mm_state.heap_start = (heap_block_t *)heap_start;
	mm_state.heap_size = KERNEL_HEAP_SIZE;
	mm_state.heap_end = (heap_block_t *)(heap_start + KERNEL_HEAP_SIZE);
	mm_state.heap_used = 0;
	mm_state.allocation_count = 0;
	mm_state.deallocation_count = 0;

	// Initialize first free block
	mm_state.heap_start->magic = HEAP_MAGIC_FREE;
	mm_state.heap_start->size = KERNEL_HEAP_SIZE - sizeof(heap_block_t);
	mm_state.heap_start->is_free = 1;
	mm_state.heap_start->next = NULL;
	mm_state.heap_start->prev = NULL;

	mm_state.free_list = mm_state.heap_start;

	kprintf("Kernel Heap Allocator initialized\n");
}

// Find suitable free block
static heap_block_t *find_free_block(size_t size)
{
	heap_block_t *current = mm_state.free_list;

	while (current) {
		if (current->is_free && current->size >= size) {
			return current;
		}
		current = current->next;
	}

	return NULL;
}

// Split block if it's too large
static void split_block(heap_block_t *block, size_t size)
{
	if (block->size < size + sizeof(heap_block_t) + 32) {
		return; // Not worth splitting
	}

	heap_block_t *new_block = (heap_block_t *)((uint8_t *)block +
						   sizeof(heap_block_t) + size);
	new_block->magic = HEAP_MAGIC_FREE;
	new_block->size = block->size - size - sizeof(heap_block_t);
	new_block->is_free = 1;
	new_block->next = block->next;
	new_block->prev = block;

	if (block->next) {
		block->next->prev = new_block;
	}
	block->next = new_block;
	block->size = size;
}

// Validate a heap block pointer is within the heap
static int is_valid_heap_block(heap_block_t *block)
{
	if (!block)
		return 1; // NULL is valid (end of list)
	uint64_t addr = (uint64_t)block;
	uint64_t heap_start = (uint64_t)mm_state.heap_start;
	uint64_t heap_end = (uint64_t)mm_state.heap_end;
	if (addr < heap_start || addr >= heap_end)
		return 0;
	if (block->magic != HEAP_MAGIC_ALLOCATED &&
	    block->magic != HEAP_MAGIC_FREE)
		return 0;
	return 1;
}

// Coalesce adjacent free blocks
static void coalesce_blocks(heap_block_t *block)
{
	// Validate current block
	if (!is_valid_heap_block(block)) {
		kprintf("coalesce_blocks: invalid block %p\n", block);
		return;
	}

	// Coalesce with next block
	if (block->next && block->next->is_free) {
		if (!is_valid_heap_block(block->next)) {
			kprintf("coalesce_blocks: invalid block->next %p (from %p)\n",
				block->next, block);
			return;
		}
		block->size += block->next->size + sizeof(heap_block_t);
		if (block->next->next) {
			if (!is_valid_heap_block(block->next->next)) {
				kprintf("coalesce_blocks: invalid block->next->next %p\n",
					block->next->next);
				return;
			}
			block->next->next->prev = block;
		}
		block->next = block->next->next;
	}

	// Coalesce with previous block
	if (block->prev && block->prev->is_free) {
		if (!is_valid_heap_block(block->prev)) {
			kprintf("coalesce_blocks: invalid block->prev %p (from %p)\n",
				block->prev, block);
			return;
		}
		block->prev->size += block->size + sizeof(heap_block_t);
		if (block->next) {
			if (!is_valid_heap_block(block->next)) {
				kprintf("coalesce_blocks: invalid block->next %p (in prev merge)\n",
					block->next);
				return;
			}
			block->next->prev = block->prev;
		}
		block->prev->next = block->next;
	}
}

// Validate heap integrity - returns 0 if OK, -1 if corrupted
int heap_validate(const char *caller)
{
	heap_block_t *cur = mm_state.free_list;
	int count = 0;
	uint64_t heap_end_addr = (uint64_t)mm_state.heap_end;

	while (cur && count < 1000) {
		// Check magic
		if (cur->magic != HEAP_MAGIC_ALLOCATED &&
		    cur->magic != HEAP_MAGIC_FREE) {
			kprintf("HEAP CORRUPT at %s: block %p has bad magic 0x%08x\n",
				caller, cur, cur->magic);
			return -1;
		}
		// Check size is reasonable
		if (cur->size == 0 || cur->size > mm_state.heap_size) {
			kprintf("HEAP CORRUPT at %s: block %p has bad size %lu\n",
				caller, cur, cur->size);
			return -1;
		}
		// Check next pointer is within heap or NULL
		if (cur->next) {
			uint64_t next_addr = (uint64_t)cur->next;
			if (next_addr < (uint64_t)mm_state.heap_start ||
			    next_addr >= heap_end_addr) {
				kprintf("HEAP CORRUPT at %s: block %p has bad next %p\n",
					caller, cur, cur->next);
				return -1;
			}
		}
		cur = cur->next;
		count++;
	}
	return 0;
}

// Allocate memory
void *kalloc(size_t size)
{
#ifdef USE_SLAB_ALLOCATOR
	return slab_alloc(size);
#else
	if (size == 0) {
		return NULL;
	}

	// Align size to 8 bytes
	size = (size + 7) & ~7;

	heap_block_t *block = find_free_block(size);
	if (!block) {
		void *ra = __builtin_return_address(0);
		kprintf("kalloc FAILED for size %lu, heap used=%lu/%lu (caller=%p)\n",
			(unsigned long)size, mm_state.heap_used,
			mm_state.heap_size, ra);
		return NULL; // Out of memory
	}

	// Split block if necessary
	split_block(block, size);

	// Mark block as allocated
	block->magic = HEAP_MAGIC_ALLOCATED;
	block->is_free = 0;

	// Update statistics
	mm_state.heap_used += size + sizeof(heap_block_t);
	mm_state.allocation_count++;

	void *user_ptr = (uint8_t *)block + sizeof(heap_block_t);
#if DEBUG
	mm_poison_fill(user_ptr, POISON_UNINIT_PAGE, size);
#else
	mm_memset(user_ptr, 0, size);
#endif
	return user_ptr;
#endif
}

// Free memory
void kfree(void *ptr)
{
#ifdef USE_SLAB_ALLOCATOR
	slab_free(ptr);
#else
	if (!ptr) {
		return;
	}

	if (!mm_state.heap_start || !mm_state.heap_end) {
		void *ra = __builtin_return_address(0);
		kprintf("ERROR: kfree before heap init (%p, caller=%p)\n", ptr,
			ra);
		return;
	}

	uint8_t *heap_start = (uint8_t *)mm_state.heap_start;
	uint8_t *heap_end = (uint8_t *)mm_state.heap_end;
	if ((uint8_t *)ptr < heap_start + sizeof(heap_block_t) ||
	    (uint8_t *)ptr >= heap_end) {
		void *ra = __builtin_return_address(0);
		kprintf("ERROR: Invalid free() call for non-heap address %p (caller=%p)\n",
			ptr, ra);
		return;
	}

	heap_block_t *block =
		(heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));

	// Validate block
	if (block->magic != HEAP_MAGIC_ALLOCATED || block->is_free) {
		void *ra = __builtin_return_address(0);
		kprintf("ERROR: Invalid free() call for address %p (magic=0x%08x free=%d caller=%p)\n",
			ptr, block->magic, block->is_free, ra);
		return;
	}

	// Mark as free
	block->magic = HEAP_MAGIC_FREE;
	block->is_free = 1;

	/* Poison the freed payload to catch use-after-free (debug builds only,
	 * matching the allocation path above). */
#if DEBUG
	mm_poison_fill((uint8_t *)block + sizeof(heap_block_t), 0xDEADBEEFU,
		       block->size);
#endif

	// Update statistics
	mm_state.heap_used -= block->size + sizeof(heap_block_t);
	mm_state.deallocation_count++;

	// Coalesce with adjacent free blocks
	coalesce_blocks(block);
#endif
}

// Reallocate memory
void *krealloc(void *ptr, size_t new_size)
{
#ifdef USE_SLAB_ALLOCATOR
	return slab_realloc(ptr, new_size);
#else
	if (!ptr) {
		return kalloc(new_size);
	}

	if (new_size == 0) {
		kfree(ptr);
		return NULL;
	}

	heap_block_t *block =
		(heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
	if (block->magic != HEAP_MAGIC_ALLOCATED) {
		return NULL;
	}

	if (block->size >= new_size) {
		return ptr; // Current block is large enough
	}

	// Allocate new block and copy data
	void *new_ptr = kalloc(new_size);
	if (new_ptr) {
		mm_memcpy(new_ptr, ptr,
			  block->size < new_size ? block->size : new_size);
		kfree(ptr);
	}

	return new_ptr;
#endif
}

// Allocate and zero memory
void *kcalloc(size_t count, size_t size)
{
#ifdef USE_SLAB_ALLOCATOR
	return slab_calloc(count, size);
#else
	size_t total_size = count * size;
	void *ptr = kalloc(total_size);
	if (ptr) {
		mm_memset(ptr, 0, total_size);
	}
	return ptr;
#endif
}

// ============================================================================
// DMA-SAFE ALLOCATIONS
// These always use the legacy heap which is in low physical memory (< 4GB)
// Use for device DMA buffers (XHCI, USB, etc.)
// ============================================================================

// DMA-safe allocation (always uses legacy heap for low physical addresses)
void *kalloc_dma(size_t size)
{
	if (size == 0) {
		return NULL;
	}

	// Align size to 8 bytes
	size = (size + 7) & ~7;

	heap_block_t *block = find_free_block(size);
	if (!block) {
		void *ra = __builtin_return_address(0);
		kprintf("kalloc_dma FAILED for size %lu, heap used=%lu/%lu (caller=%p)\n",
			(unsigned long)size, mm_state.heap_used,
			mm_state.heap_size, ra);
		return NULL;
	}

	split_block(block, size);
	block->magic = HEAP_MAGIC_ALLOCATED;
	block->is_free = 0;
	mm_state.heap_used += size + sizeof(heap_block_t);
	mm_state.allocation_count++;

	return (uint8_t *)block + sizeof(heap_block_t);
}

// DMA-safe calloc
void *kcalloc_dma(size_t count, size_t size)
{
	size_t total_size = count * size;
	void *ptr = kalloc_dma(total_size);
	if (ptr) {
		mm_memset(ptr, 0, total_size);
	}
	return ptr;
}

// Free DMA-safe allocation
void kfree_dma(void *ptr)
{
	if (!ptr) {
		return;
	}

	if (!mm_state.heap_start || !mm_state.heap_end) {
		return;
	}

	uint8_t *heap_start = (uint8_t *)mm_state.heap_start;
	uint8_t *heap_end = (uint8_t *)mm_state.heap_end;
	if ((uint8_t *)ptr < heap_start + sizeof(heap_block_t) ||
	    (uint8_t *)ptr >= heap_end) {
		void *ra = __builtin_return_address(0);
		kprintf("ERROR: Invalid kfree_dma for address %p (caller=%p)\n",
			ptr, ra);
		return;
	}

	heap_block_t *block =
		(heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));

	if (block->magic != HEAP_MAGIC_ALLOCATED || block->is_free) {
		void *ra = __builtin_return_address(0);
		kprintf("ERROR: Invalid kfree_dma for address %p (magic=0x%08x free=%d caller=%p)\n",
			ptr, block->magic, block->is_free, ra);
		return;
	}

	block->magic = HEAP_MAGIC_FREE;
	block->is_free = 1;
	mm_state.heap_used -= block->size + sizeof(heap_block_t);
	mm_state.deallocation_count++;
	coalesce_blocks(block);
}

// MEMORY STATISTICS AND DEBUGGING

/* Bytes filesystem drivers hold in reclaimable block/metadata buffers
 * (see mm_buffercache_account in memory.h). */

static volatile int64_t g_buffercache_bytes;

/* How much work the address-space teardown actually does.  If address spaces
 * are destroyed but memory does not come back, the question is whether the
 * walk never saw the pages or saw them and could not release them. */
#if MM_LEAK_INSTRUMENTATION
/* Live address spaces, by creator.
 *
 * Every path that makes one has been read and they all destroy on failure and
 * on exit, yet more are created than destroyed.  Rather than keep guessing at
 * which caller drops one, record the caller of each LIVE address space and let
 * the ones that are still here name themselves. */
#define AS_TRACK_MAX 4096
static struct {
	uint64_t pml4_phys;
	uint32_t creator;
} g_as_track[AS_TRACK_MAX];
static volatile unsigned long g_as_track_live;
static volatile unsigned long g_as_track_overflow;
static spinlock_t g_as_track_lock = SPINLOCK_INIT("as_track");

static void as_track_add(uint64_t pml4_phys, uint32_t creator)
{
	uint64_t f;

	spin_lock_irqsave(&g_as_track_lock, &f);
	for (unsigned i = 0; i < AS_TRACK_MAX; i++) {
		if (g_as_track[i].pml4_phys == 0) {
			g_as_track[i].pml4_phys = pml4_phys;
			g_as_track[i].creator = creator;
			g_as_track_live++;
			spin_unlock_irqrestore(&g_as_track_lock, f);
			return;
		}
	}
	g_as_track_overflow++;
	spin_unlock_irqrestore(&g_as_track_lock, f);
}

/* Re-blame an entry on the caller that really wanted the address space.
 * mm_create_user_address_space() only ever sees mm_clone_address_space() as its
 * caller, which does not say which fork path asked for it. */
static void as_track_retag(uint64_t pml4_phys, uint32_t creator)
{
	uint64_t f;

	spin_lock_irqsave(&g_as_track_lock, &f);
	for (unsigned i = 0; i < AS_TRACK_MAX; i++)
		if (g_as_track[i].pml4_phys == pml4_phys) {
			g_as_track[i].creator = creator;
			break;
		}
	spin_unlock_irqrestore(&g_as_track_lock, f);
}

static void as_track_del(uint64_t pml4_phys)
{
	uint64_t f;

	spin_lock_irqsave(&g_as_track_lock, &f);
	for (unsigned i = 0; i < AS_TRACK_MAX; i++) {
		if (g_as_track[i].pml4_phys == pml4_phys) {
			g_as_track[i].pml4_phys = 0;
			g_as_track[i].creator = 0;
			g_as_track_live--;
			break;
		}
	}
	spin_unlock_irqrestore(&g_as_track_lock, f);
}

/* Which of the three teardown routes actually ran.  Only three places destroy
 * an address space; if fewer run than were created, this says which. */
volatile unsigned long g_as_destroy_exit_self;
volatile unsigned long g_as_exit_self_own;   /* mm == NULL: destroys outright */
volatile unsigned long g_as_exit_self_mm;    /* mm != NULL: only the last one */
volatile unsigned long g_mm_created;
volatile unsigned long g_mm_freed;
#endif /* MM_LEAK_INSTRUMENTATION */
volatile unsigned long g_as_destroy_remove_task;
volatile unsigned long g_as_destroy_exec;

static volatile unsigned long g_create_calls;
#if MM_LEAK_INSTRUMENTATION
static volatile unsigned long g_destroy_calls;
static volatile unsigned long g_destroy_pages_seen;
#endif

void mm_buffercache_account(long delta)
{
	__sync_fetch_and_add(&g_buffercache_bytes, (int64_t)delta);
}

uint64_t mm_buffercache_bytes(void)
{
	int64_t v = g_buffercache_bytes;
	return v > 0 ? (uint64_t)v : 0;
}

// Get memory statistics
#if MM_LEAK_INSTRUMENTATION
/* Report which call sites are holding physical memory.
 *
 * "Used" as a single number cannot distinguish a cache doing its job from a
 * path that allocates and never frees, and when the machine is idle with every
 * process gone the only useful question is which code still owns the pages.
 * Each allocation records its caller; this counts the live pages by caller and
 * prints the worst offenders.
 *
 * Resolve the addresses with:
 *   rm build/kernel.elf && make NO_STRIP=1
 *   addr2line -f -e build/kernel.elf 0xffffffff8xxxxxxx
 */
void mm_dump_page_owners(void)
{
#define OWNER_SLOTS 48
	struct {
		uint32_t site;
		uint64_t pages;
	} top[OWNER_SLOTS];
	unsigned used = 0, i;
	uint64_t untracked = 0, other = 0, total = 0;
	uint64_t flags;

	if (!mm_state.page_owner) {
		kprintf("page owners: not tracked on this boot\n");
		return;
	}

	spin_lock_irqsave(&mm_phys_lock, &flags);
	for (uint64_t pg = 0; pg < mm_state.total_pages; pg++) {
		uint32_t site;

		if (!is_page_allocated(pg))
			continue;
		total++;
		site = mm_state.page_owner[pg];
		if (!site) {
			untracked++; /* reserved at boot, never allocated */
			continue;
		}
		for (i = 0; i < used; i++) {
			if (top[i].site == site) {
				top[i].pages++;
				break;
			}
		}
		if (i == used) {
			if (used < OWNER_SLOTS) {
				top[used].site = site;
				top[used].pages = 1;
				used++;
			} else {
				other++;
			}
		}
	}
	spin_unlock_irqrestore(&mm_phys_lock, flags);

	/* Selection sort: at most OWNER_SLOTS entries, and this runs once. */
	for (i = 0; i < used; i++) {
		unsigned best = i, j;

		for (j = i + 1; j < used; j++)
			if (top[j].pages > top[best].pages)
				best = j;
		if (best != i) {
			uint32_t ts = top[i].site;
			uint64_t tp = top[i].pages;

			top[i] = top[best];
			top[best].site = ts;
			top[best].pages = tp;
		}
	}

	kprintf("\n=== page owners (%lu pages allocated, %lu MB) ===\n", total,
		(total * PAGE_SIZE) / (1024 * 1024));
	for (i = 0; i < used; i++) {
		if (top[i].pages < 16)
			continue; /* noise */
		kprintf("  0xffffffff%08x  %8lu pages  %6lu MB\n", top[i].site,
			top[i].pages,
			(top[i].pages * PAGE_SIZE) / (1024 * 1024));
	}
	kprintf("  boot-reserved/untracked: %lu pages (%lu MB)\n", untracked,
		(untracked * PAGE_SIZE) / (1024 * 1024));

	/* How many references the live pages carry.
	 *
	 * A page still allocated when nothing maps it is either one nobody
	 * released (refcount 1, a free that never ran) or one somebody
	 * referenced twice and released once (refcount >= 2, a reference that
	 * was never given back).  Those are different bugs in different code,
	 * and the count says which without guessing. */
	if (mm_state.page_refcounts) {
		uint64_t rc[5] = { 0, 0, 0, 0, 0 };

		spin_lock_irqsave(&mm_phys_lock, &flags);
		for (uint64_t pg = 0; pg < mm_state.total_pages; pg++) {
			uint16_t c;

			if (!is_page_allocated(pg) || !mm_state.page_owner[pg])
				continue;
			c = mm_state.page_refcounts[pg];
			rc[c < 4 ? c : 4]++;
		}
		spin_unlock_irqrestore(&mm_phys_lock, flags);
		kprintf("  refcounts of tracked live pages: 0:%lu 1:%lu 2:%lu 3:%lu 4+:%lu\n",
			rc[0], rc[1], rc[2], rc[3], rc[4]);
	}
	{
		struct {
			uint32_t site;
			unsigned long n;
		} by[8];
		unsigned used = 0;
		uint64_t f;

		spin_lock_irqsave(&g_as_track_lock, &f);
		for (unsigned i = 0; i < AS_TRACK_MAX; i++) {
			unsigned k;

			if (!g_as_track[i].pml4_phys)
				continue;
			for (k = 0; k < used; k++)
				if (by[k].site == g_as_track[i].creator) {
					by[k].n++;
					break;
				}
			if (k == used && used < 8) {
				by[used].site = g_as_track[i].creator;
				by[used].n = 1;
				used++;
			}
		}
		spin_unlock_irqrestore(&g_as_track_lock, f);
			kprintf("  mm_structs: %lu created, %lu freed (%ld still referenced)\n",
			g_mm_created, g_mm_freed,
			(long)g_mm_created - (long)g_mm_freed);
		kprintf("  exit_mm_self: %lu own-pml4, %lu via mm_struct\n",
			g_as_exit_self_own, g_as_exit_self_mm);
		kprintf("  teardown routes: exit_mm_self=%lu remove_task=%lu exec=%lu\n",
			g_as_destroy_exit_self, g_as_destroy_remove_task,
			g_as_destroy_exec);
	kprintf("  live address spaces: %lu (%lu untracked past the table)\n",
			g_as_track_live, g_as_track_overflow);
		for (unsigned i = 0; i < used; i++)
			kprintf("    created at 0xffffffff%08x: %lu still alive\n",
				by[i].site, by[i].n);
	}
	kprintf("  address spaces: %lu created, %lu destroyed (%ld never torn down), %lu pages released\n",
		g_create_calls, g_destroy_calls,
		(long)g_create_calls - (long)g_destroy_calls,
		g_destroy_pages_seen);
	if (other)
		kprintf("  (%lu more call sites than the table holds)\n", other);
	kprintf("Resolve with: addr2line -f -e build/kernel.elf <addr>  (build NO_STRIP=1)\n");
#undef OWNER_SLOTS
}
#endif /* MM_LEAK_INSTRUMENTATION */

void mm_get_memory_stats(memory_stats_t *stats)
{
	/* Report RAM, not the address span it lives in.  Subtracting free from
	 * the span counted every hole as used and buried the real figure. */
	uint64_t ram_pages = mm_state.usable_pages ? mm_state.usable_pages :
						     mm_state.total_pages;

	stats->total_memory = ram_pages * PAGE_SIZE;
	stats->free_memory = mm_state.free_pages * PAGE_SIZE;
	stats->used_memory = stats->total_memory - stats->free_memory;
	stats->total_pages = ram_pages;
	stats->free_pages = mm_state.free_pages;
	stats->used_pages = stats->total_pages - stats->free_pages;
	stats->heap_allocated = mm_state.heap_used;
	stats->heap_free = mm_state.heap_size - mm_state.heap_used;
	stats->allocations = mm_state.allocation_count;
	stats->deallocations = mm_state.deallocation_count;

	// Ownership breakdown (see memory_stats_t): slab + page cache; the
	// remainder of used_pages is raw page-allocator consumers (user
	// mappings, page tables, kernel stacks, DMA buffers).
	slab_stats_t sstats;
	slab_get_stats(&sstats);
	stats->slab_pages = sstats.total_pages_used;
	stats->slab_large_active =
		sstats.large_allocations - sstats.large_frees;
	pc_stats_t pcs;
	pagecache_get_stats(&pcs);
	stats->pagecache_pages = pcs.total_pages;
}

// Print memory statistics
void mm_print_memory_stats(void)
{
	memory_stats_t stats;
	mm_get_memory_stats(&stats);

	kprintf("\n=== Memory Statistics ===\n");
	kprintf("Physical Memory:\n");
	kprintf("  Total: %d MB (%d pages)\n",
		stats.total_memory / (1024 * 1024), stats.total_pages);
	kprintf("  Used:  %d MB (%d pages)\n",
		stats.used_memory / (1024 * 1024), stats.used_pages);
	kprintf("  Free:  %d MB (%d pages)\n",
		stats.free_memory / (1024 * 1024), stats.free_pages);
	kprintf("========================\n\n");
}

// Validate heap integrity
bool mm_validate_heap(void)
{
	heap_block_t *current = mm_state.heap_start;
	uint32_t block_count = 0;

	while (current && (uint8_t *)current < (uint8_t *)mm_state.heap_end) {
		// Check magic numbers
		if (current->magic != HEAP_MAGIC_ALLOCATED &&
		    current->magic != HEAP_MAGIC_FREE) {
			kprintf("ERROR: Invalid magic in heap block %d at %p\n",
				block_count, current);
			return false;
		}

		// Check bounds
		if ((uint8_t *)current + sizeof(heap_block_t) + current->size >
		    (uint8_t *)mm_state.heap_end) {
			kprintf("ERROR: Heap block %d extends beyond heap end\n",
				block_count);
			return false;
		}

		block_count++;
		current = current->next;

		// Prevent infinite loop
		if (block_count > 1000) {
			kprintf("ERROR: Too many heap blocks, possible corruption\n");
			return false;
		}
	}

	return true;
}

// Print heap statistics
void mm_print_heap_stats(void)
{
	kprintf("\n=== Heap Block Information ===\n");

	heap_block_t *current = mm_state.heap_start;
	uint32_t block_count = 0;
	uint32_t free_blocks = 0;
	uint32_t allocated_blocks = 0;

	while (current && (uint8_t *)current < (uint8_t *)mm_state.heap_end &&
	       block_count < 20) {
		kprintf("Block %d: %p, size=%d, %s\n", block_count, current,
			current->size, current->is_free ? "FREE" : "ALLOCATED");

		if (current->is_free) {
			free_blocks++;
		} else {
			allocated_blocks++;
		}

		block_count++;
		current = current->next;
	}

	kprintf("Total blocks shown: %d (Free: %d, Allocated: %d)\n",
		block_count, free_blocks, allocated_blocks);
	kprintf("==============================\n\n");
}

// ============================================================================
// USER ADDRESS SPACE MANAGEMENT
// ============================================================================

// Get page table entry from a specific PML4, creating intermediate tables if needed
// Note: pml4 is expected to be a virtual address (via phys_to_virt)
uint64_t *mm_get_page_table_from_pml4(uint64_t *pml4, uint64_t virtual_addr,
				      bool create)
{
	if (!pml4) {
		return NULL;
	}

	uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
	uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
	uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
	uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;

	bool is_user_space = (virtual_addr < KERNEL_OFFSET);

	// Get PDPT
	uint64_t *pdpt;
	uint64_t pdpt_phys = pml4[pml4_index] & PTE_ADDR_MASK;
	if (!(pml4[pml4_index] & PAGE_PRESENT)) {
		if (!create)
			return NULL;
		pdpt_phys = allocate_pt_page(); // Use safe PT pool
		if (!pdpt_phys)
			return NULL;
		// Page already zeroed by allocate_pt_page
		// For user space, set PAGE_USER on all levels
		uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
		if (is_user_space) {
			flags |= PAGE_USER;
		}
		pml4[pml4_index] = pdpt_phys | flags;
		pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
	} else {
		// Entry exists - but for user space mapping, we may need to add PAGE_USER
		if (is_user_space && create &&
		    !(pml4[pml4_index] & PAGE_USER)) {
			pml4[pml4_index] |= PAGE_USER;
		}
		pdpt = (uint64_t *)phys_to_virt(pdpt_phys);
	}

	// Get PD
	uint64_t *pd;
	uint64_t pd_phys = pdpt[pdpt_index] & PTE_ADDR_MASK;
	if (!(pdpt[pdpt_index] & PAGE_PRESENT)) {
		if (!create)
			return NULL;
		pd_phys = allocate_pt_page(); // Use safe PT pool
		if (!pd_phys)
			return NULL;
		// Page already zeroed by allocate_pt_page
		uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
		if (is_user_space) {
			flags |= PAGE_USER;
		}
		pdpt[pdpt_index] = pd_phys | flags;
		pd = (uint64_t *)phys_to_virt(pd_phys);
	} else {
		// Entry exists - but for user space mapping, we may need to add PAGE_USER
		if (is_user_space && create &&
		    !(pdpt[pdpt_index] & PAGE_USER)) {
			pdpt[pdpt_index] |= PAGE_USER;
		}
		pd = (uint64_t *)phys_to_virt(pd_phys);
	}

	// Get PT
	uint64_t *pt;
	uint64_t pd_entry = pd[pd_index];

	// Check if this is a 2MB large page (PS bit set)
	if ((pd_entry & PAGE_PRESENT) && (pd_entry & 0x80)) {
		// This is a 2MB page - we need to split it into 4KB pages
		if (!create)
			return NULL;

		// Get the base physical address of the 2MB page (bits 21-51)
		uint64_t large_page_base = pd_entry & 0x000FFFFFFFE00000ULL;
		uint64_t old_flags =
			pd_entry &
			PTE_FLAGS_MASK; // Keep flags including NX bit

		// Allocate a new page table from safe PT pool
		uint64_t pt_phys = allocate_pt_page();
		if (!pt_phys)
			return NULL;

		pt = (uint64_t *)phys_to_virt(pt_phys);

		// Fill the page table with 512 4KB pages covering the same 2MB region
		for (int i = 0; i < 512; i++) {
			uint64_t page_phys = large_page_base + (i * PAGE_SIZE);
			// Keep original flags but remove PS bit and add any user flags if needed
			uint64_t pt_flags = (old_flags & ~0x80) | PAGE_PRESENT;
			if (is_user_space) {
				pt_flags |= PAGE_USER;
			}
			pt[i] = page_phys | pt_flags;
		}

		// Update PD entry to point to the new page table
		uint64_t pd_flags = PAGE_PRESENT | PAGE_WRITABLE;
		if (is_user_space) {
			pd_flags |= PAGE_USER;
		}
		pd[pd_index] = pt_phys | pd_flags;
	} else if (!(pd_entry & PAGE_PRESENT)) {
		if (!create)
			return NULL;
		uint64_t pt_phys = allocate_pt_page(); // Use safe PT pool
		if (!pt_phys)
			return NULL;

		// Page already zeroed by allocate_pt_page
		uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
		if (is_user_space) {
			flags |= PAGE_USER;
		}
		pd[pd_index] = pt_phys | flags;
		pt = (uint64_t *)phys_to_virt(pt_phys);
	} else {
		// Entry exists - but for user space mapping, we may need to add PAGE_USER
		if (is_user_space && create && !(pd[pd_index] & PAGE_USER)) {
			pd[pd_index] |= PAGE_USER;
		}
		uint64_t pt_phys = pd[pd_index] & PTE_ADDR_MASK;
		pt = (uint64_t *)phys_to_virt(pt_phys);
	}

	return &pt[pt_index];
}

// Create a new user address space (PML4)
// No identity mapping - user space is clean.
// Shares:
//   - PML4[272]: Direct map (PHYS_MAP_BASE) for kernel physical memory access
//   - PML4[511]: Kernel higher-half mapping for kernel code/data
uint64_t *mm_create_user_address_space(void)
{
	// Allocate PML4 from safe PT pool
	uint64_t pml4_phys = allocate_pt_page();
	if (!pml4_phys) {
		kprintf("mm_create_user_address_space: Failed to allocate PML4\n");
		return NULL;
	}

	// Access via direct map (already zeroed by allocate_pt_page)
	uint64_t *new_pml4 = (uint64_t *)phys_to_virt(pml4_phys);

	// CRITICAL: Use the saved kernel PML4, NOT current CR3!
	// During fork, CR3 points to the parent's address space. Using CR3 would
	// copy the parent's kernel mappings which might be stale if parent's
	// address space was modified. Always use the original kernel PML4.
	uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(g_kernel_pml4_phys);

	// Copy PML4[272] - Direct map (PHYS_MAP_BASE = 0xFFFF880000000000)
	// This allows kernel code to access physical memory via phys_to_virt()
	if (kernel_pml4[PHYS_MAP_PML4_INDEX] & PAGE_PRESENT) {
		new_pml4[PHYS_MAP_PML4_INDEX] =
			kernel_pml4[PHYS_MAP_PML4_INDEX];
	}

	// Copy PML4[511] - Kernel higher-half mapping (supervisor only)
	// This allows kernel code to run while in user's address space
	// It also provides access to kernel heap, stacks, etc.
	if (kernel_pml4[511] & PAGE_PRESENT) {
		new_pml4[511] = kernel_pml4[511];
	}

	// User space mappings (PML4[0-255]) start empty
	// They will be filled in by mm_map_user_page() when loading ELF, etc.

	/* Counted here, not on entry: a creation that failed is not an address
	 * space anyone has to destroy, and counting it made the
	 * created-vs-destroyed comparison read as a leak. */
	LEAK_INC(g_create_calls);
	AS_TRACK_ADD(pml4_phys, (uint32_t)(uintptr_t)__builtin_return_address(0));
	return new_pml4;
}

/* Queue one leaf frame of a dying address space for release, flushing the
 * batch whenever it fills.  See mm_free_physical_pages_batch(): the point is
 * to take the allocator's global lock a few times per hundred frames instead
 * of once per frame. */
static void destroy_put_page(uint64_t *batch, unsigned *n, uint64_t phys)
{
	if (*n == MM_FREE_BATCH_MAX) {
		mm_put_pages_batch(batch, *n);
		*n = 0;
	}
	batch[(*n)++] = phys;
}

// Destroy an address space and free all user pages
void mm_destroy_address_space(uint64_t *pml4)
{
	uint64_t batch[MM_FREE_BATCH_MAX];
	unsigned nbatch = 0;

	if (!pml4)
		return;

	/* Runs preemptibly by contract: the batched release below drops the
	 * allocator lock between batches precisely so interrupts can be
	 * serviced, and a caller in atomic context would defeat that. */
	might_sleep();

	// pml4 is a virtual address (via phys_to_virt)
	// Calculate the physical address for freeing
	uint64_t pml4_phys = virt_to_phys(pml4);

	// Don't free the kernel PML4 itself (set during mm_initialize_virtual_memory)
	if (pml4_phys == g_kernel_pml4_phys) {
		return;
	}

	// If we're about to destroy the currently active address space,
	// switch to kernel page tables first. This happens when a process
	// exits and mm_struct_put is called before the scheduler switches.
	uint64_t current_cr3 = get_cr3() & ~0xFFFULL;
	if (pml4_phys == current_cr3) {
		/* Tracked like any other switch: this CPU is leaving the
		 * address space it is about to take apart, and must stop being
		 * counted as a holder of it. */
		sched_mmu_track_enter(g_kernel_pml4_phys);
		set_cr3(g_kernel_pml4_phys);
		sched_mmu_track_done(g_kernel_pml4_phys);
	}

	/* Every processor acknowledges an invalidation before one byte of this
	 * tree goes back to the allocator.  See pt_free_barrier(). */
	pt_free_barrier(pml4_phys);

	int pages_freed = 0;
	int pt_freed = 0;

	// Free user-space page tables (entries 0-255, user space only)
	// Skip 256-511 (kernel space) and 272 (direct map)
	// Use 0x000FFFFFFFFFF000ULL mask to extract physical address (bits 12-51)
	for (int i = 0; i < 256; i++) {
		if (pml4[i] & PAGE_PRESENT) {
			uint64_t pdpt_phys = pml4[i] & 0x000FFFFFFFFFF000ULL;
			uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

			for (int j = 0; j < 512; j++) {
				if (pdpt[j] & PAGE_PRESENT) {
					uint64_t pd_phys =
						pdpt[j] & 0x000FFFFFFFFFF000ULL;
					uint64_t *pd = (uint64_t *)phys_to_virt(
						pd_phys);

					for (int k = 0; k < 512; k++) {
						if (pd[k] & PAGE_PRESENT) {
							// Check if it's a 2MB page or a page table
							if (pd[k] &
							    PAGE_SIZE_FLAG) {
								/* Device MMIO 2MB mappings: not allocator-owned. */
								if (pd[k] &
								    PAGE_DEVICE)
									continue;
								// 2MB page - check COW refcount before freeing (mask 21 bits for 2MB alignment).
								// A 2MB mapping covers 512 physical 4K frames; each
								// one must be released individually or 511 of them
								// leak (the bitmap tracks 4K frames).
								uint64_t phys =
									pd[k] &
									0x000FFFFFFFE00000ULL;
								/* A 2MB mapping covers 512
								 * frames and each is counted
								 * on its own, so drop a
								 * reference from every one --
								 * releasing only the first
								 * leaked the other 511. */
								for (int p2 = 0; p2 < 512;
								     p2++)
									destroy_put_page(
										batch,
										&nbatch,
										phys +
											(uint64_t)p2 *
												PAGE_SIZE);
								pages_freed += 512;
							} else {
								uint64_t pt_phys =
									pd[k] &
									0x000FFFFFFFFFF000ULL;
								uint64_t *pt = (uint64_t
											*)
									phys_to_virt(
										pt_phys);

								// Free all physical pages in this PT
								for (int l = 0;
								     l < 512;
								     l++) {
									if (pt[l] &
									    PAGE_PRESENT) {
										/* Device MMIO PTEs: not allocator-owned, skip. */
										if (pt[l] &
										    PAGE_DEVICE)
											continue;
										uint64_t phys =
											pt[l] &
											0x000FFFFFFFFFF000ULL;
										/* One reference per
										 * mapping: this one
										 * goes, and the page
										 * with it if it was
										 * the last. */
										destroy_put_page(
											batch,
											&nbatch,
											phys);
										pages_freed++;
									}
								}

								// Free the PT itself (page tables are not shared)
								free_pt_page(
									pt_phys);
								pt_freed++;
							}
						}
					}

					// Free the PD (page directories are not shared)
					free_pt_page(pd_phys);
					pt_freed++;
				}
			}

			// Free the PDPT (not shared)
			free_pt_page(pdpt_phys);
			pt_freed++;
		}
	}

	/* Release whatever the last batch still holds. */
	mm_put_pages_batch(batch, nbatch);
	nbatch = 0;

	/* Untrack BEFORE the page goes back to the pool.  Freed first, another
	 * CPU can be handed the same physical page for a new address space and
	 * add its own entry for it -- and then this del would remove THAT one,
	 * leaving this dead entry behind for ever.  The table would report a
	 * leak that is only its own bookkeeping. */
	AS_TRACK_DEL(pml4_phys);

	// Free the PML4 itself
	free_pt_page(pml4_phys);
	pt_freed++;

	LEAK_INC(g_destroy_calls);
	LEAK_ADD(g_destroy_pages_seen, pages_freed);

	// Suppress unused variable warnings (used for debugging)
	(void)pages_freed;
	(void)pt_freed;
}

// Switch to a different address space
// pml4 is a virtual address (from phys_to_virt)
void mm_switch_address_space(uint64_t *pml4)
{
	if (pml4) {
		// Convert virtual address back to physical for CR3
		uint64_t pml4_phys = virt_to_phys(pml4);

		/* Every address-space change goes through here, so this is
		 * where this CPU records what it is holding.  A CPU that loads
		 * an address space without saying so would be skipped by an
		 * invalidation for it and keep using translations that have
		 * already been taken away. */
		sched_mmu_track_enter(pml4_phys);
		set_cr3(pml4_phys);
		sched_mmu_track_done(pml4_phys);
	}
}

// Get the current address space (returns virtual address via phys_to_virt)
uint64_t *mm_get_current_address_space(void)
{
	uint64_t pml4_phys = get_cr3() & ~0xFFFULL;
	return (uint64_t *)phys_to_virt(pml4_phys);
}

// Check whether every page covering [vaddr, vaddr+len) is present in the
// current address space (CR3).  Returns true only if all pages are mapped.
/* Translate a user virtual address in a SPECIFIC address space, returning the
 * physical address or 0 if it is not mapped.
 *
 * Everything else that touches user memory works through the current CR3,
 * which is fine while the caller is running in the address space it means.
 * The exit path is not in that position: a thread can be torn down from a CPU
 * whose CR3 belongs to somebody else, and there it needs to reach into the
 * dying task's address space by its page table rather than by the register.
 * The result is a physical address, so the caller writes through the direct
 * map -- a kernel address, with no CR3 or SMAP involvement at all. */
uint64_t mm_virt_to_phys_in(uint64_t *pml4, uint64_t vaddr)
{
	uint64_t pml4i = (vaddr >> 39) & 0x1FF;
	uint64_t pdpti = (vaddr >> 30) & 0x1FF;
	uint64_t pdi = (vaddr >> 21) & 0x1FF;
	uint64_t pti = (vaddr >> 12) & 0x1FF;
	uint64_t pml4e, pdpte, pde, pte;
	uint64_t *pdpt, *pd, *pt;

	if (!pml4)
		return 0;

	pml4e = pml4[pml4i];
	if (!(pml4e & PAGE_PRESENT))
		return 0;

	pdpt = (uint64_t *)phys_to_virt(pml4e & PTE_ADDR_MASK);
	pdpte = pdpt[pdpti];
	if (!(pdpte & PAGE_PRESENT))
		return 0;
	if (pdpte & PAGE_SIZE_FLAG) /* 1 GB page */
		return (pdpte & PTE_ADDR_MASK) + (vaddr & 0x3FFFFFFFULL);

	pd = (uint64_t *)phys_to_virt(pdpte & PTE_ADDR_MASK);
	pde = pd[pdi];
	if (!(pde & PAGE_PRESENT))
		return 0;
	if (pde & PAGE_SIZE_FLAG) /* 2 MB page */
		return (pde & PTE_ADDR_MASK) + (vaddr & 0x1FFFFFULL);

	pt = (uint64_t *)phys_to_virt(pde & PTE_ADDR_MASK);
	pte = pt[pti];
	if (!(pte & PAGE_PRESENT))
		return 0;
	return (pte & PTE_ADDR_MASK) + (vaddr & 0xFFFULL);
}

bool mm_user_addr_mapped(uint64_t vaddr, size_t len)
{
	if (len == 0)
		return true;

	/* Reject ranges that wrap past the top of the 64-bit address space.
     * Without this, a caller passing e.g. rip-16 where rip is small (the
     * crash handler's "Bytes around RIP" dump when a user task jumped to a
     * NULL/low address) yields vaddr = 0xfffffffffffffff0.  Then
     * (vaddr + len - 1) overflows, end_page wraps below start_page, the
     * page-walk loop never runs, and the function wrongly returns true —
     * the caller then dereferences the unmapped wrap address and the kernel
     * itself takes a #PF while reporting a recoverable userspace crash. */
	if (vaddr + len < vaddr)
		return false;

	uint64_t pml4_phys = get_cr3() & ~0xFFFULL;
	uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

	uint64_t start_page = vaddr & ~0xFFFULL;
	uint64_t end_page = (vaddr + len - 1) & ~0xFFFULL;

	for (uint64_t page = start_page; page <= end_page; page += 0x1000) {
		uint64_t pml4i = (page >> 39) & 0x1FF;
		uint64_t pdpti = (page >> 30) & 0x1FF;
		uint64_t pdi = (page >> 21) & 0x1FF;
		uint64_t pti = (page >> 12) & 0x1FF;

		uint64_t pml4e = pml4[pml4i];
		if (!(pml4e & PAGE_PRESENT))
			return false;

		uint64_t *pdpt =
			(uint64_t *)phys_to_virt(pml4e & PTE_ADDR_MASK);
		uint64_t pdpte = pdpt[pdpti];
		if (!(pdpte & PAGE_PRESENT))
			return false;
		if (pdpte & PAGE_SIZE_FLAG)
			continue; /* 1 GB page: present */

		uint64_t *pd = (uint64_t *)phys_to_virt(pdpte & PTE_ADDR_MASK);
		uint64_t pde = pd[pdi];
		if (!(pde & PAGE_PRESENT))
			return false;
		if (pde & PAGE_SIZE_FLAG)
			continue; /* 2 MB page: present */

		uint64_t *pt = (uint64_t *)phys_to_virt(pde & PTE_ADDR_MASK);
		uint64_t pte = pt[pti];
		if (!(pte & PAGE_PRESENT))
			return false;
	}
	return true;
}

// Map a page in a specific address space
bool mm_map_page_in_address_space(uint64_t *pml4, uint64_t virtual_addr,
				  uint64_t physical_addr, uint64_t flags)
{
	uint64_t *pte = mm_get_page_table_from_pml4(pml4, virtual_addr, true);
	if (!pte) {
		return false;
	}

	*pte = (physical_addr & ~0xFFFULL) | flags;

	// Flush TLB if this is the current address space
	if (pml4 == mm_get_current_address_space()) {
		mm_flush_tlb(virtual_addr);
	}

	return true;
}

// Map a user page with appropriate flags
bool mm_map_user_page(uint64_t *pml4, uint64_t virtual_addr,
		      uint64_t physical_addr, uint64_t flags)
{
	// Ensure user bit is set for user-space addresses
	if (virtual_addr < KERNEL_OFFSET) {
		flags |= PAGE_USER;
	}
	return mm_map_page_in_address_space(pml4, virtual_addr, physical_addr,
					    flags);
}

// Map a user stack region
bool mm_map_user_stack(uint64_t *pml4, uint64_t stack_top, size_t stack_size)
{
	if (!pml4 || stack_size == 0) {
		return false;
	}

	size_t pages = (stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t stack_bottom = stack_top - stack_size;

	for (size_t i = 0; i < pages; i++) {
		uint64_t vaddr = stack_bottom + (i * PAGE_SIZE);
		uint64_t phys = mm_allocate_physical_page();

		if (!phys) {
			// Unmap already-mapped pages on failure
			for (size_t j = 0; j < i; j++) {
				mm_unmap_page_in_address_space(
					pml4, stack_bottom + (j * PAGE_SIZE));
			}
			return false;
		}

		/* No memset in production: mm_allocate_physical_page already
		 * zeroed the page (re-zeroing the 2 MB stack cost ~1 ms per
		 * exec).  DEBUG builds poison on alloc, so zero explicitly —
		 * the user stack must not leak poison patterns. */
#if DEBUG
		mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
#endif

		// Map with user, writable, non-executable flags (stack should not be executable)
		uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
				 PAGE_NO_EXECUTE;
		if (!mm_map_page_in_address_space(pml4, vaddr, phys, flags)) {
			mm_free_physical_page(phys);
			// Unmap already-mapped pages on failure
			for (size_t j = 0; j < i; j++) {
				mm_unmap_page_in_address_space(
					pml4, stack_bottom + (j * PAGE_SIZE));
			}
			return false;
		}
	}

	return true;
}

// Get physical address from a specific PML4
uint64_t mm_get_physical_address_from_pml4(uint64_t *pml4,
					   uint64_t virtual_addr)
{
	uint64_t *pte = mm_get_page_table_from_pml4(pml4, virtual_addr, false);
	if (pte && (*pte & PAGE_PRESENT)) {
// Physical address is in bits 12-51 (mask off flags at bits 0-11 and bit 63)
#define PTE_PHYS_MASK_ADDR 0x000FFFFFFFFFF000ULL
		return (*pte & PTE_PHYS_MASK_ADDR) | (virtual_addr & 0xFFF);
	}
	return 0;
}

// Get page flags for a virtual address
uint64_t mm_get_page_flags(uint64_t virtual_addr)
{
	uint64_t *pte = mm_get_page_table(virtual_addr, false);
	if (pte) {
		// Return flags: bits 0-11 (low flags) and bit 63 (NX)
		return (*pte & 0xFFFULL) | (*pte & PAGE_NO_EXECUTE);
	}
	return 0;
}

// Physical address mask: bits 12-51 contain the physical page frame
#define PTE_PHYS_MASK 0x000FFFFFFFFFF000ULL

// Set page flags for a virtual address
bool mm_set_page_flags(uint64_t virtual_addr, uint64_t flags)
{
	uint64_t *pte = mm_get_page_table(virtual_addr, false);
	if (pte && (*pte & PAGE_PRESENT)) {
		uint64_t phys =
			*pte &
			PTE_PHYS_MASK; // Extract physical address (bits 12-51)
		*pte = phys | flags;
		mm_flush_tlb(virtual_addr);
		return true;
	}
	return false;
}

// ============================================================================
// COPY-ON-WRITE SUPPORT
// ============================================================================

// Mark a page as copy-on-write (make it read-only with COW flag)
bool mm_mark_page_cow(uint64_t virtual_addr)
{
	uint64_t *pte = mm_get_page_table(virtual_addr, false);
	if (!pte || !(*pte & PAGE_PRESENT)) {
		return false;
	}

	// Remove writable, add COW marker
	*pte = (*pte & ~PAGE_WRITABLE) | PAGE_COW;
	mm_flush_tlb(virtual_addr);
	return true;
}

/* ============================================================================
 * Taking the address-space semaphore from a fault
 *
 * A fault handler wants the lock for reading, but it cannot always have it:
 * the semaphore sleeps, and a fault taken with interrupts already off is a
 * fault taken while some spinlock is held.  Parking there would deadlock the
 * lock holder rather than protect it.
 *
 * That case is supposed to be impossible.  Every syscall that copies a user
 * buffer while holding an FS or socket lock runs mm_prefault_user_range()
 * first, precisely so the fault happens BEFORE the lock is taken.  So a
 * user-address fault arriving with interrupts disabled means a shield is
 * missing at some entry point, and it is worth saying so loudly rather than
 * silently running the fault unsynchronised.
 *
 * Returns true when the lock was taken and must be released.
 * ========================================================================== */
/* Take the address space for reading, or say that it could not be taken.
 *
 * A false return now means "do not resolve this fault", and both callers obey
 * it.  It used to mean "resolve it anyway, without the lock", which is not
 * something a fault may do.  The region table it walks is an array that
 * mm_regions_grow() replaces and kfree()s under the write lock, so a fault
 * reading it unlocked can be walking freed -- and by then handed out again --
 * memory as though it were region records.  A record whose backing file has
 * been overwritten by whatever took that memory next makes the fault fill an
 * executable page with zeros, and the process dies later executing them,
 * nowhere near the cause.
 *
 * Interrupts being disabled is what used to force the choice, since the
 * semaphore parks and parking with interrupts off does not end well.  But
 * "cannot park" is not "cannot acquire": the lock is almost always free, and
 * one non-parking attempt takes it properly in that case, so the common fault
 * is now correctly locked where before it was not locked at all.  Only when a
 * writer actually holds it -- exactly the window where the table is being
 * replaced and reading it would be wrong -- does this give up.
 *
 * The reference draws the same line: a fault arriving where the handler may
 * not sleep is failed outright rather than resolved without mmap_lock. */
static bool mm_fault_lock(task_t *mm)
{
	if (!mm)
		return false;
	if (unlikely(irqs_disabled()))
		return mm_read_trylock(&mm->mmap_lock);
	mm_read_lock(&mm->mmap_lock);
	return true;
}

static void mm_fault_unlock(task_t *mm, bool locked)
{
	if (locked)
		mm_read_unlock(&mm->mmap_lock);
}

// Handle a COW page fault - allocate new page and copy contents
// This must be SMP-safe: multiple CPUs may handle COW faults simultaneously
//
// The address-space semaphore is taken by the wrapper below, so everything in
// here runs with the address space held stable for reading: no munmap can pull
// the region out from under it, and no fork can turn the mapping into a shared
// one part-way through.
/* Is this physical address one the direct map can actually reach?
 *
 * Asked before any address taken OUT of a page-table entry is followed.  The
 * reference asks the same question with pfn_valid() and answers a failure
 * with print_bad_pte(): the entry is corrupt, so it is named and the fault is
 * refused rather than dereferenced.
 *
 * Refusing matters more here than it looks.  phys_to_virt() of an address
 * past the direct map does not produce an unmapped pointer, it produces a
 * NON-CANONICAL one -- and `rep movs` through one of those is a general
 * protection fault in ring 0, which is the whole machine rather than one
 * process.  A page table with 0xffffffffffffffff in one slot is exactly that
 * shape: the address bits mask to 0x000ffffffffff000, phys_to_virt() lands at
 * 0x000f87fffffff000, and the copy-on-write copy takes the system down with
 * an Oops instead of the faulting process down with a signal.
 *
 * Zero is refused too: no page-table entry naming physical page 0 is one this
 * kernel wrote. */
static bool mm_phys_is_mappable(uint64_t phys)
{
	return phys != 0 && phys < g_direct_map_limit;
}

/* Allocate a page for a fault, reclaiming first if the free list has run dry.
 *
 * The reference's allocator does not report failure until it has tried to
 * make room: its slow path reclaims and retries, and only an allocation that
 * still cannot be satisfied afterwards fails.  Ours reported failure straight
 * away, so a machine whose memory had all ended up in the page cache killed
 * whatever touched a lazy mapping next -- a browser tab, or the terminal it
 * was started from -- while megabytes of clean, droppable file pages sat
 * there unreclaimed.
 *
 * This sits at the fault rather than inside mm_allocate_physical_page()
 * because reclaim takes page-cache locks and the allocator is reached from
 * places that hold them; a fault is process context with nothing of the sort
 * held, which is why the page-in below is allowed to sleep on disk here.
 *
 * Clean pages only.  Writing dirty ones back needs the filesystem's I/O lock
 * for writing, and a fault can already hold it shared -- the same reasoning
 * pagecache_reclaim_if_needed() spells out.  Dropping the clean ones is what
 * relieves the pressure; the dirty ones become reclaimable once the writeback
 * thread has dealt with them. */
static uint64_t mm_alloc_page_for_fault(void)
{
	uint64_t phys = mm_allocate_physical_page();

	if (likely(phys != 0))
		return phys;

	/* A small batch, the size the reference uses for one reclaim round.
	 * Asking for thousands here would not find them any faster -- the
	 * scan is bounded either way -- and this runs on a fault, where the
	 * work is paid for by whoever touched the page. */
	pagecache_shrink(32, 0);
	pagecache_request_writeback();
	return mm_allocate_physical_page();
}

/* ==========================================================================
 * Dirty tracking for device mappings.
 *
 * A driver whose device keeps a second copy of mapped pages needs to know
 * what the processor wrote through the mapping, and the processor already
 * records that: every write leaves the dirty bit in the entry that mapped
 * it, and a write against an entry with the write bit withheld raises a
 * fault that names the page.  The two walks below and the fault branch in
 * the copy-on-write handler turn those records into the driver's, through
 * the mm_dirty_ops callbacks riding on the mapping's region records.
 *
 * Two rules make this correct where a first attempt was not:
 *
 *  - Every entry update is an atomic exchange.  The processor sets the
 *    accessed and dirty bits with locked cycles of its own, from any CPU's
 *    table walk, at any moment; a plain read-modify-write that races one
 *    loses the bit, and a lost dirty bit is a write the device never
 *    hears about -- pixels from the previous frame, sourced from a copy
 *    the client already replaced.
 *
 *  - No walk returns before the translations it changed are gone from
 *    every processor.  The dirty bit is only written into an entry by a
 *    walk that did not already have it cached; a processor still holding
 *    the old translation keeps writing through it, recording nothing, for
 *    as long as the translation lives.  The same holds for the write bit:
 *    a cached writable translation lets writes through long after the
 *    entry was protected.  One ranged invalidation per walk, after the
 *    entries are changed and before the caller trusts the result.
 * ========================================================================== */

/* Atomically update one device entry: succeed only while `need' is fully
 * set, then clear `clear' and set `set'.  Returns false when the entry is
 * not a present device entry or lacks `need' -- including when a competing
 * update got there first, which is an answer, not an error. */
static bool mm_dirty_pte_update(uint64_t *pte, uint64_t need, uint64_t clear,
				uint64_t set)
{
	uint64_t old = __atomic_load_n(pte, __ATOMIC_RELAXED);

	for (;;) {
		if ((old & (PAGE_PRESENT | PAGE_DEVICE)) !=
		    (PAGE_PRESENT | PAGE_DEVICE))
			return false;
		if ((old & need) != need)
			return false;
		if (__atomic_compare_exchange_n(pte, &old,
						(old & ~clear) | set, false,
						__ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			return true;
		/* `old' now holds what beat us; decide again from that. */
	}
}

static int mm_dirty_walk_mappings(struct mm_dirty_walk *w, bool clean)
{
	task_t *cur = sched_current();
	task_t *mm = task_mm_owner(cur);
	uint64_t pml4_phys;

	w->marked = 0;
	w->matched = 0;
	if (!mm || !mm->pml4 || !w->obj || !w->ops)
		return -EINVAL;
	if (irqs_disabled())
		return -EINVAL; /* cannot take the address-space lock here */

	/* The walk covers the CURRENT address space only: its lock can be
	 * taken from here and its tables cannot be torn down while it is
	 * held.  Records other address spaces hold on the same object are
	 * counted by the caller against `matched' (see the tracker), never
	 * walked -- taking another task's address-space lock from inside a
	 * submission is an ordering nothing else in this kernel does. */
	mm_read_lock(&mm->mmap_lock);
	for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
		mmap_region_t *r = &mm->mmap_regions[i];

		if (!r->in_use || !r->device || r->dev_obj != w->obj ||
		    r->dev_dirty != w->ops)
			continue;
		w->matched++;
		if (r->offset < w->file_base)
			continue; /* not a record of this object's pages */

		/* The record's pages within the object, clipped to the
		 * caller's window.  offset = base + delta survives splits,
		 * so this holds for any surviving fragment. */
		uint64_t rp0 = (r->offset - w->file_base) / PAGE_SIZE;
		uint64_t rp1 = rp0 + r->length / PAGE_SIZE;

		if (rp1 > w->npages)
			rp1 = w->npages;
		uint64_t p0 = (w->first > rp0) ? w->first : rp0;
		uint64_t p1 = (w->last < rp1) ? w->last : rp1;

		for (uint64_t p = p0; p < p1; p++) {
			uint64_t va = r->start + (p - rp0) * PAGE_SIZE;
			uint64_t *pte = mm_get_page_table_from_pml4(mm->pml4,
								    va, false);

			if (!pte)
				continue;
			if (clean) {
				if (!mm_dirty_pte_update(pte, PAGE_DIRTY,
							 PAGE_DIRTY, 0))
					continue;
				w->marked++;
				mm_flush_tlb(va);
				/* Recorded before this walk's invalidation
				 * completes, which is fine: the caller reads
				 * the record only after the walk returns. */
				if (w->record)
					w->record(w->arg, p);
			} else {
				if (!mm_dirty_pte_update(pte, PAGE_WRITABLE,
							 PAGE_WRITABLE,
							 PAGE_DIRTY_TRACKED))
					continue;
				w->marked++;
				mm_flush_tlb(va);
			}
		}
	}
	pml4_phys = virt_to_phys(mm->pml4);
	mm_read_unlock(&mm->mmap_lock);

	/* The rule above: nothing is reported until no processor can still
	 * be using a translation this walk changed. */
	if (w->marked)
		smp_tlb_shootdown_mm_sync(pml4_phys);
	return 0;
}

int mm_dirty_wp_mappings(struct mm_dirty_walk *w)
{
	return mm_dirty_walk_mappings(w, false);
}

int mm_dirty_clean_mappings(struct mm_dirty_walk *w)
{
	return mm_dirty_walk_mappings(w, true);
}

/* A write faulted against a tracked device entry (PAGE_DIRTY_TRACKED set,
 * write bit withheld).  Report the page to the tracker, then hand the
 * write bit back: the MAPPING is writable -- the tracker had only borrowed
 * the bit to be told about this moment.  Runs under the address-space
 * read lock the fault wrapper takes.
 *
 * The order is load-bearing: the page is recorded BEFORE the entry becomes
 * writable.  A tracker pass that protects entries and then reads its
 * records must find either the protected entry (the write has not landed)
 * or the record (it has); an entry made writable before the record exists
 * is a window where the write lands and the pass sees neither. */
static bool mm_dirty_mkwrite_locked(uint64_t va, uint64_t *pte)
{
	task_t *mm = task_mm_owner(sched_current());
	mmap_region_t *r = NULL;

	if (!mm || !mm->mmap_regions)
		return false;
	for (uint32_t i = 0; i < mm->mmap_capacity; i++) {
		mmap_region_t *c = &mm->mmap_regions[i];

		if (c->in_use && va >= c->start &&
		    va < c->start + c->length) {
			r = c;
			break;
		}
	}
	if (!r || !r->device)
		return false;
	/* The tracked bit only ever withholds write from a mapping that HAS
	 * it; a region without PROT_WRITE is genuinely read-only and the
	 * fault is the caller's error. */
	if (!(r->prot & PROT_WRITE))
		return false;

	if (r->dev_dirty && r->dev_dirty->mkwrite && r->dev_obj)
		r->dev_dirty->mkwrite(r->dev_obj,
				      r->offset + (va - r->start));

	mm_dirty_pte_update(pte, 0, 0, PAGE_WRITABLE | PAGE_DIRTY);
	mm_flush_tlb(va);
	/* Local invalidation only: granting permission needs no broadcast.
	 * A processor still holding the protected translation faults
	 * spuriously, re-reads the entry, and carries on. */
	return true;
}

static bool mm_cow_fault_locked(uint64_t fault_addr)
{
	uint64_t page_addr = fault_addr & ~0xFFFULL;
	uint64_t *pte = mm_get_page_table(page_addr, false);

	if (!pte || !(*pte & PAGE_PRESENT)) {
		return false;
	}

	/* A tracked device entry whose write bit the dirty tracker is
	 * holding: bookkeeping, not a protection error.  Checked before the
	 * copy-on-write test because a device entry is never COW -- its
	 * pages belong to a driver's object, shared by design. */
	if ((*pte & (PAGE_DEVICE | PAGE_DIRTY_TRACKED)) ==
		    (PAGE_DEVICE | PAGE_DIRTY_TRACKED) &&
	    !(*pte & PAGE_WRITABLE))
		return mm_dirty_mkwrite_locked(page_addr, pte);

	// Check if this is a COW page
	if (!(*pte & PAGE_COW)) {
		// Not a COW page - but on SMP another CPU may have already resolved this
		// COW fault. If the page is now writable, just flush our TLB and succeed.
		if (*pte & PAGE_WRITABLE) {
			mm_flush_tlb(page_addr);
			return true;
		}
		return false; // Not a COW fault and not writable - genuine fault
	}

	// Extract physical address (bits 12-51, mask off flags and NX bit)
	uint64_t old_phys = *pte & PTE_ADDR_MASK;

	// Validate the physical address is in tracked range
	uint64_t page_idx = page_to_index(old_phys);

	if (page_idx == (uint64_t)-1) {
		// Page is outside the refcount-tracked memory range.
		// We MUST still copy it — just making it writable would allow the
		// parent and child to share the same physical page after fork,
		// breaking COW semantics (the parent's writes would be visible
		// to the child and vice versa).
		//
		// Untracked is not the same as arbitrary.  Device memory mapped
		// into a process is legitimately outside the counted range and
		// must be copied; an address the direct map cannot reach is not
		// memory at all, and the only way an entry comes to hold one is
		// that something wrote over the page table.  Following it is a
		// ring-0 fault on a non-canonical address -- see
		// mm_phys_is_mappable() -- so the entry is named and the fault
		// refused, which turns a halted machine into a signal for the
		// process that owns the corruption.
		if (!mm_phys_is_mappable(old_phys)) {
			WARN_RATELIMIT(
				1,
				"cow: bad page table entry for VA 0x%lx: entry 0x%lx names physical 0x%lx, outside the direct map",
				(unsigned long)page_addr,
				(unsigned long)*pte, (unsigned long)old_phys);
			return false;
		}
		uint64_t new_phys = mm_alloc_page_for_fault();
		if (!new_phys) {
			kprintf("mm_handle_cow_fault: untracked page copy alloc failed\n");
			return false;
		}
		uint64_t uflags;

		mm_memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys),
			  PAGE_SIZE);
		/* Same publish-with-re-check as the counted path below: two
		 * threads of one process can both reach here for the same page,
		 * and the loser's copy predates whatever the winner has already
		 * written into its own.  Nothing is released on this path --
		 * the source is not allocator-owned -- so losing costs only the
		 * copy. */
		spin_lock_irqsave(&mm_refcount_lock, &uflags);
		if (!(*pte & PAGE_COW) || (*pte & PTE_ADDR_MASK) != old_phys) {
			spin_unlock_irqrestore(&mm_refcount_lock, uflags);
			mm_free_physical_page(new_phys);
			mm_flush_tlb(page_addr);
			return true;
		}
		uint64_t flags = (*pte & 0xFFF) & ~PAGE_COW;
		flags |= PAGE_WRITABLE;
		*pte = (*pte & PAGE_NO_EXECUTE) | new_phys | flags;
		spin_unlock_irqrestore(&mm_refcount_lock, uflags);
		mm_flush_tlb(page_addr);
		return true;
	}

	uint64_t irq_flags;
	struct mm_tlb_gather gather;

	spin_lock_irqsave(&mm_refcount_lock, &irq_flags);

	// Re-check COW flag under lock (another CPU may have resolved it)
	if (!(*pte & PAGE_COW)) {
		// Another CPU already resolved this COW fault
		spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);
		mm_flush_tlb(page_addr);
		return true;
	}

	/*
	 * NOBODY ELSE HAS THIS PAGE: take it, do not copy it.
	 *
	 * One reference means one mapping, and that mapping is this one -- the
	 * count says so unconditionally now that every mapping holds a
	 * reference.  So the page is already private and copying it would
	 * produce a byte-identical duplicate and free the original.
	 *
	 * This is the common case, not a corner: fork marks parent and child
	 * COW, the child almost always exec's or exits within a few
	 * milliseconds, and from then on every page the parent touches is one
	 * it alone holds.  A shell or a GUI program that forks helpers
	 * repeatedly used to copy its entire working set back one fault at a
	 * time, every time.
	 */
	if (mm_get_page_refcount(old_phys) == 1) {
		*pte = (*pte & ~(uint64_t)PAGE_COW) | PAGE_WRITABLE;
		spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);
		mm_flush_tlb(page_addr);
		return true;
	}

	/* Shared with someone.  Pin it so it cannot be released while we copy:
	 * another CPU could otherwise drop the last other reference -- a forked
	 * child exec'ing, say -- and the copy would read a freed and poisoned
	 * page, or one already handed out as somebody else's page table. */
	mm_get_page(old_phys);

	spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);

	// Allocate a new physical page (outside lock for performance)
	uint64_t new_phys = mm_alloc_page_for_fault();
	if (!new_phys) {
		mm_put_page(old_phys); /* undo the pin */
		kprintf("mm_handle_cow_fault: Failed to allocate new page\n");
		return false;
	}

	// Copy contents from old page to new page via direct map.
	// old_phys is pinned, so it cannot be freed concurrently.
	mm_memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);

	/*
	 * PUBLISH UNDER THE LOCK, AND ONLY IF NOTHING CHANGED.
	 *
	 * Threads share a page table, so two of them can fault on the same page
	 * at once.  Without this re-check both allocated, both copied, and both
	 * stored their own copy into the entry -- the second store winning.
	 * Everything the winner's userspace wrote into the first copy in
	 * between was then discarded, because the loser's copy is a snapshot of
	 * the page as it was BEFORE either fault was resolved.
	 *
	 * The page does not become garbage; it silently reverts.  That is what
	 * made it so hard to read from the wreckage: an allocator's chunk
	 * headers go back a few writes and its free lists stop agreeing with
	 * each other, and an object that was fully constructed comes back with
	 * half its fields unset.
	 *
	 * Losing is not an error -- the winner's copy is live and correct -- so
	 * throw ours away and let the instruction run again.
	 */
	spin_lock_irqsave(&mm_refcount_lock, &irq_flags);
	if (!(*pte & PAGE_COW) || (*pte & PTE_ADDR_MASK) != old_phys) {
		spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);
		mm_free_physical_page(new_phys); /* never mapped; ours alone */
		mm_put_page(old_phys); /* the pin */
		mm_flush_tlb(page_addr);
		return true;
	}

	// Update PTE: remove COW, add writable, point to new page, preserve NX bit
	uint64_t flags = (*pte & 0xFFF) & ~PAGE_COW;
	flags |= PAGE_WRITABLE;
	*pte = (*pte & PAGE_NO_EXECUTE) | new_phys | flags;
	spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);

	mm_flush_tlb(page_addr);

	/* Give back the pin.  It can never be the last reference -- this
	 * mapping still holds one until the line below -- so this cannot
	 * release the page, and needs no invalidation of its own. */
	mm_put_page(old_phys);

	/* Now this mapping's own reference.  Through the gather, because other
	 * CPUs may still translate the faulting address to old_phys: releasing
	 * it before they are told otherwise would put a page they are still
	 * reading back into the allocator, which poisons it on the way out. */
	{
		/* This fault is against the running task's address space --
		 * mm_get_page_table() above resolved through it -- so that is
		 * the only one whose translations can need invalidating. */
		task_t *faulting = sched_current();

		mm_tlb_gather_init(&gather, faulting ? faulting->pml4 : NULL);
	}
	mm_tlb_gather_page(&gather, old_phys, page_addr);
	mm_tlb_gather_flush(&gather);

	/* new_phys came from the allocator holding one reference, and that is
	 * exactly the reference the new mapping needs -- nothing further to
	 * take. */

	return true;
}

bool mm_handle_cow_fault(uint64_t fault_addr)
{
	task_t *cur = sched_current();
	task_t *mm = task_mm_owner(cur);

	/* Stall detector, not a fix: one write to a private page must cost
	 * microseconds.  The two phases are timed apart because they fail
	 * differently -- the lock names a writer holding the address space,
	 * the body names the copy machinery or the remote invalidation.
	 *
	 * The threshold sits ABOVE what a busy hypervisor host can inflict:
	 * the resolve phase waits for invalidation acknowledgements, and a
	 * runnable-but-descheduled virtual CPU was measured taking up to
	 * ~300 ms to be given the time to answer.  That is the host's
	 * scheduler, not this kernel, so only a stall no amount of host load
	 * explains is worth a report. */
	uint64_t t0 = timer_get_precise_us();
	bool locked = mm_fault_lock(mm);
	uint64_t t1, t2;
	bool ret;

	/* Refusing costs this access a SIGSEGV or a fixup.  Resolving it
	 * against an address space somebody is in the middle of changing
	 * costs a wrong page, silently, and the crash arrives much later. */
	if (!locked)
		return false;
	t1 = timer_get_precise_us();
	ret = mm_cow_fault_locked(fault_addr);
	t2 = timer_get_precise_us();

	mm_fault_unlock(mm, locked);
	WARN_RATELIMIT(
		t2 - t0 > 500000,
		"cow fault on %llx took %llu ms (lock wait %llu ms, resolve %llu ms)",
		(unsigned long long)fault_addr,
		(unsigned long long)((t2 - t0) / 1000),
		(unsigned long long)((t1 - t0) / 1000),
		(unsigned long long)((t2 - t1) / 1000));
	return ret;
}

bool mm_make_writable_in(uint64_t *pml4, uint64_t vaddr)
{
	uint64_t page_addr = vaddr & ~0xFFFULL;

	if (!pml4)
		return false;

	uint64_t *pte = mm_get_page_table_from_pml4(pml4, page_addr, false);

	if (!pte || !(*pte & PAGE_PRESENT))
		return false;

	/* Already writable.  A writable mapping is never a shared read-only
	 * one here, so there is nothing to unshare. */
	if (*pte & PAGE_WRITABLE)
		return true;

	/* Not writable, for one of two reasons, and they are handled the same
	 * way: it is copy-on-write from a fork, or it is a read-only mapping
	 * such as program text.  Text is the case that matters for a debugger
	 * -- planting a breakpoint means writing into code, which is mapped
	 * read-only precisely because nothing normally writes to it -- and it
	 * carries no copy-on-write marker, so keying off that marker refused
	 * every breakpoint with a bad-address error.
	 *
	 * What actually has to be decided is whether anyone ELSE would see the
	 * write, and the reference count answers that on its own: one holder
	 * means the page is already private and only its permission is in the
	 * way, more than one means it must be copied first.  The marker only
	 * ever said "expect the count to be greater than one". */
	uint64_t old_phys = *pte & PTE_ADDR_MASK;
	uint64_t page_idx = page_to_index(old_phys);

	/* Outside the refcounted range: copy unconditionally.  Making it
	 * writable in place would leave both sharers on one page, which is the
	 * exact corruption this exists to prevent. */
	if (page_idx == (uint64_t)-1) {
		uint64_t new_phys = mm_alloc_page_for_fault();
		uint64_t uflags;

		if (!new_phys)
			return false;

		mm_memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys),
			  PAGE_SIZE);

		spin_lock_irqsave(&mm_refcount_lock, &uflags);
		if ((*pte & PAGE_WRITABLE) ||
		    (*pte & PTE_ADDR_MASK) != old_phys) {
			spin_unlock_irqrestore(&mm_refcount_lock, uflags);
			mm_free_physical_page(new_phys);
			return true; /* somebody else resolved it */
		}
		uint64_t f = ((*pte & 0xFFF) & ~PAGE_COW) | PAGE_WRITABLE;

		*pte = (*pte & PAGE_NO_EXECUTE) | new_phys | f;
		spin_unlock_irqrestore(&mm_refcount_lock, uflags);
		goto invalidate;
	}

	uint64_t irq_flags;

	spin_lock_irqsave(&mm_refcount_lock, &irq_flags);

	/* Re-checked under the lock: another CPU may have made it writable
	 * while this one was getting here.  Asked as "is it writable yet",
	 * not "is it still marked copy-on-write" -- a read-only text page
	 * never carried that marker, and testing for it would report success
	 * having changed nothing. */
	if (*pte & PAGE_WRITABLE) {
		spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);
		return true;
	}

	/* Sole owner: take it rather than duplicate it, exactly as the fault
	 * path does.  A tracee that forked long ago and has since touched most
	 * of its pages is the common case. */
	if (mm_get_page_refcount(old_phys) == 1) {
		*pte = (*pte & ~(uint64_t)PAGE_COW) | PAGE_WRITABLE;
		spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);
		goto invalidate;
	}

	/* Shared.  Pin the source so it cannot be released while it is being
	 * copied -- the other sharer could exit at any moment. */
	mm_get_page(old_phys);
	spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);

	uint64_t new_phys = mm_alloc_page_for_fault();

	if (!new_phys) {
		mm_put_page(old_phys);
		return false;
	}
	mm_memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);

	spin_lock_irqsave(&mm_refcount_lock, &irq_flags);
	if ((*pte & PAGE_WRITABLE) || (*pte & PTE_ADDR_MASK) != old_phys) {
		/* Lost a race with the target resolving it itself.  Its copy is
		 * the live one; discard ours rather than overwrite whatever has
		 * been written into it since. */
		spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);
		mm_free_physical_page(new_phys);
		mm_put_page(old_phys);
		return true;
	}
	{
		uint64_t f = ((*pte & 0xFFF) & ~PAGE_COW) | PAGE_WRITABLE;

		*pte = (*pte & PAGE_NO_EXECUTE) | new_phys | f;
	}
	spin_unlock_irqrestore(&mm_refcount_lock, irq_flags);

	mm_put_page(old_phys); /* the pin; the mapping's own ref goes below */

	/* Release this mapping's reference to the old page through the gather:
	 * a sibling thread of the target may still be running and still
	 * translating this address to old_phys, and handing the page back
	 * before it has been told otherwise puts poisoned memory under a live
	 * reader. */
	{
		struct mm_tlb_gather g;

		mm_tlb_gather_init(&g, pml4);
		mm_tlb_gather_page(&g, old_phys, page_addr);
		mm_tlb_gather_flush(&g);
	}
	return true;

invalidate:
	/* The entry changed without any page being released, so there is
	 * nothing for the gather to carry -- and it does nothing when handed an
	 * empty batch.  The invalidation is still required: a sibling thread of
	 * the target may be running right now with the old read-only
	 * translation cached, and it would keep using it.
	 *
	 * Exactly one page changed, so name it: the CPUs holding this address
	 * space invalidate that translation alone, and nobody else loses
	 * anything. */
	if (sched_is_smp())
		smp_tlb_shootdown_pages_sync(virt_to_phys(pml4), &page_addr, 1);
	mm_flush_tlb(page_addr); /* and this CPU, which the above skips */
	return true;
}

// ============================================================================
// Demand paging — lazy region materialisation
// ============================================================================

/* Serialises the check-PTE-then-map step so two threads faulting on the
 * same page cannot both install a page (the loser would leak its copy or,
 * worse, the two would see different contents).  Held only around the
 * final non-sleeping check+map. */
static spinlock_t g_lazy_map_lock = SPINLOCK_INIT("lazymap");

/* The per-handle page-in flag (vfs_file::pagein_busy) is not taken here any
 * more: the page-in below reads positionally and has no descriptor state to
 * protect.  The flag remains for vfs_pread()'s fallback and for pread(2), and
 * sched.c still clears one a dying owner abandoned. */


static int mm_demand_fault_mm(task_t *mm, uint64_t fault_addr,
			      int from_kernel_mode)
{
	if (fault_addr >= 0x0000800000000000ULL)
		return 0;
	if (!mm || !mm->pml4)
		return 0;
	uint64_t page = fault_addr & ~0xFFFULL;

	uint64_t map_flags = 0;
	struct vfs_file *file = NULL;
	uint64_t file_off = 0;
	int found = 0;

	// brk heap: everything in [brk_start, page-aligned brk) is lazy zeros
	if (mm->brk_start && page >= mm->brk_start &&
	    page < ((mm->brk + 0xFFFULL) & ~0xFFFULL)) {
		map_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
			    PAGE_NO_EXECUTE;
		found = 1;
	} else {
		/* Same bound and hint as mm_find_mmap_region(): this is THE
		 * hot path -- one pass of it per page a process touches for
		 * the first time -- and walking the whole capacity here is
		 * what made a long-running browser slower with every tab it
		 * had ever opened. */
		uint32_t end = mm->mmap_hwm;
		uint32_t hint = mm->mmap_hint;

		if (end > mm->mmap_capacity)
			end = mm->mmap_capacity;
		if (hint >= end)
			hint = 0; /* stale: the table shrank under it */
		for (uint32_t n = 0; n < end; n++) {
			/* Start at the hint and wrap, so a run of faults in
			 * one mapping answers on the first iteration. */
			uint32_t i = hint + n < end ? hint + n :
						     hint + n - end;
			mmap_region_t *r = &mm->mmap_regions[i];
			if (!r->in_use || !r->lazy)
				continue;
			if (page < r->start || page >= r->start + r->length)
				continue;
			mm->mmap_hint = i;
			if (!(r->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)))
				return 0; // PROT_NONE — genuine fault
			map_flags = PAGE_PRESENT | PAGE_USER;
			if (r->prot & PROT_WRITE)
				map_flags |= PAGE_WRITABLE;
			if (!(r->prot & PROT_EXEC))
				map_flags |= PAGE_NO_EXECUTE;
			file = r->file;
			file_off = r->offset + (page - r->start);
			found = 1;
			break;
		}
	}
	if (!found)
		return 0;

	uint64_t phys = mm_alloc_page_for_fault();
	if (!phys) {
		/* Out of physical pages is NOT a segfault, but failing this
		 * fault delivers SIGSEGV to a task touching a perfectly valid
		 * mapping -- under browser-grade memory pressure that reads
		 * as a random crash on a library page.  Until there is real
		 * reclaim-on-fault, at least say what actually happened. */
		WARN_RATELIMIT(1,
			       "demand fault: NO FREE PAGES for va=%llx (pid %d) - delivering SIGSEGV",
			       (unsigned long long)page,
			       mm ? (int)mm->id : -1);
		return 0;
	}

	if (file) {
		/* Page-in from the backing file.  This sleeps on disk I/O, so
		 * it must only happen in process context with no FS/socket
		 * locks held.  User-mode faults always qualify.  Kernel-mode
		 * faults are routine as well (copy_from_user of argv/rodata
		 * in lazy text segments, etc.) and equally safe when no such
		 * locks are held — the read/write/send/recv entry points
		 * pre-fault their buffers precisely so that lock-holding
		 * copy loops never reach this path. */
		(void)from_kernel_mode;
		/* Read the page WITHOUT going near the handle's position.
		 *
		 * The position belongs to the DESCRIPTOR, and this handle is
		 * that descriptor: mmap references the caller's open file, and
		 * fork shares it again.  Seeking it here, reading, and seeking
		 * back -- which is what this did -- races every read() and
		 * lseek() the program makes on the same descriptor, and a lost
		 * race fills the page from the wrong offset in the file.  A
		 * library's text page then holds code that does not belong
		 * there and the process dies branching into it, arbitrarily
		 * later and nowhere near the cause.  The per-handle page-in
		 * flag did not cover it: only page-ins and pread(2) ever took
		 * that flag, while read(), readv(), lseek() and sendfile()
		 * moved the position freely.
		 *
		 * The reference has no such race, because a fault there reads
		 * by index through the page cache and never consults the open
		 * file at all.  vfs_pread() is that property expressed as a
		 * call; the size comes from fstat for the same reason, since
		 * seeking to SEEK_END is a position change too. */
		struct kstat pst;
		long fsize = (vfs_fstat(file, &pst) == 0) ? (long)pst.st_size :
							    -1;
		long got = -1;

		if (fsize >= 0)
			got = vfs_pread(file, phys_to_virt(phys), PAGE_SIZE,
					(long)file_off);
		if (got < 0)
			got = 0;
		/* A short read is legitimate ONLY at EOF inside the mapping
		 * (e.g. an RO-BSS tail past the file bytes) — that tail reads
		 * as zeros.  A short read of bytes that DO exist in the file
		 * is an I/O error or a transient FS failure: mapping a
		 * zeroed/partial page over real file content would hand the
		 * CPU garbage text/data and produce wild-jump crashes far
		 * from the cause.  Fail the fault loudly instead. */
		long expected = 0;
		if (fsize > (long)file_off) {
			expected = fsize - (long)file_off;
			if (expected > (long)PAGE_SIZE)
				expected = (long)PAGE_SIZE;
		}
		if (got < expected || fsize < 0) {
			WARN_RATELIMIT(
				1,
				"pagein: short read %ld/%ld at file_off=%llu va=%llx - failing fault",
				got, expected, (unsigned long long)file_off,
				(unsigned long long)page);
			mm_free_physical_page(phys);
			return 0;
		}
		/* Zero the EOF tail (also covers DEBUG builds where fresh
		 * pages are poisoned). */
		if (got < (long)PAGE_SIZE)
			mm_memset((uint8_t *)phys_to_virt(phys) + got, 0,
				  PAGE_SIZE - (uint64_t)got);
	} else {
#if DEBUG
		/* Production pages come pre-zeroed from the allocator; DEBUG
		 * builds poison them, so zero explicitly (anon mappings must
		 * read as zeros). */
		mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
#endif
	}

	// Install — re-check under the lock in case another thread won.
	uint64_t lf;
	spin_lock_irqsave(&g_lazy_map_lock, &lf);
	uint64_t *pte = mm_get_page_table_from_pml4(mm->pml4, page, false);
	if (pte && (*pte & PAGE_PRESENT)) {
		spin_unlock_irqrestore(&g_lazy_map_lock, lf);
		mm_free_physical_page(phys);
		return 1; // already materialised by a concurrent fault
	}
	bool ok = mm_map_page_in_address_space(mm->pml4, page, phys, map_flags);
	spin_unlock_irqrestore(&g_lazy_map_lock, lf);
	if (!ok) {
		WARN_RATELIMIT(1,
			       "demand fault: page-table install failed for va=%llx (pid %d)",
			       (unsigned long long)page,
			       mm ? (int)mm->id : -1);
		mm_free_physical_page(phys);
		return 0;
	}
	return 1;
}

/* The running task's own lazy fault: resolve which address space that is, then
 * do the same work. */
static int mm_demand_fault_locked(uint64_t fault_addr, int from_kernel_mode)
{
	task_t *cur = sched_current();

	if (!cur || cur->privilege != TASK_USER || !cur->pml4)
		return 0;
	return mm_demand_fault_mm(task_mm_owner(cur), fault_addr,
				  from_kernel_mode);
}


int mm_handle_demand_fault(uint64_t fault_addr, int from_kernel_mode)
{
	task_t *cur = sched_current();
	task_t *mm = task_mm_owner(cur);
	bool locked = mm_fault_lock(mm);
	int ret;

	/* Same bargain as the COW path: a refused fault is one access
	 * failing, an unlocked one is a region record read out of memory that
	 * is being freed. */
	if (!locked)
		return 0;
	ret = mm_demand_fault_locked(fault_addr, from_kernel_mode);

	mm_fault_unlock(mm, locked);
	return ret;
}

bool mm_populate_in(task_t *mm, uint64_t vaddr)
{
	uint64_t page = vaddr & ~0xFFFULL;

	if (!mm || !mm->pml4)
		return false;

	/* Already there is success: this is asked before every cross-process
	 * read or write, and the overwhelmingly common case is a page that has
	 * been resident for a long time. */
	uint64_t *pte = mm_get_page_table_from_pml4(mm->pml4, page, false);

	if (pte && (*pte & PAGE_PRESENT))
		return true;

	return mm_demand_fault_mm(mm, page, 1) != 0;
}

void mm_prefault_user_range(uint64_t addr, uint64_t len, int for_write)
{
	if (!len || addr >= 0x0000800000000000ULL)
		return;
	task_t *cur = sched_current();
	if (!cur || cur->privilege != TASK_USER || !cur->pml4)
		return;
	uint64_t end = addr + len;
	if (end < addr || end > 0x0000800000000000ULL)
		end = 0x0000800000000000ULL;
	/* Bound the walk: no legitimate single I/O here exceeds this, and a
	 * hostile length must not turn into an unbounded loop.  Anything
	 * beyond the cap still works — anon faults resolve safely inline
	 * from any context; only file-backed lazy pages past the cap would
	 * hit the kernel-mode-fault warning above. */
	if (end - addr > (64ULL << 20))
		end = addr + (64ULL << 20);
	for (uint64_t p = addr & ~0xFFFULL; p < end; p += PAGE_SIZE) {
		uint64_t *pte = mm_get_page_table(p, false);
		if (!pte || !(*pte & PAGE_PRESENT))
			mm_handle_demand_fault(p, 0);
		else if (for_write && (*pte & PAGE_COW))
			mm_handle_cow_fault(p);
	}
}

// Clone an address space for fork() - uses COW for efficiency
uint64_t *mm_clone_address_space(uint64_t *src_pml4)
{
	if (!src_pml4) {
		return NULL;
	}

	// Create new address space (this sets up kernel mappings)
	uint64_t *new_pml4 = mm_create_user_address_space();
	if (!new_pml4) {
		return NULL;
	}
	AS_TRACK_RETAG(virt_to_phys(new_pml4),
		       (uint32_t)(uintptr_t)__builtin_return_address(0));

	// CRITICAL: Disable interrupts only for the narrow window where we
	// atomically mark each source PTE as COW and increment its refcount.
	// Keeping the window per-PTE (rather than for the entire 4-level walk)
	// prevents multi-millisecond IRQ-off periods that cause TLB shootdown
	// timeouts on other CPUs.
	//
	// Correctness: mm_handle_cow_fault ALWAYS makes a copy when PAGE_COW is
	// set, regardless of refcount.  Between any two PTEs (when IRQs are
	// briefly re-enabled) a concurrent COW fault can fire on an already-
	// processed entry without breaking coherence.  The per-PTE window only
	// needs to protect the {mark-COW-in-src, write-child-PTE, incref} trio.

	// Clone user-space mappings with COW from source PML4
	// User code lives in PML4[0] around virtual address 0x400000
	// We need to find user pages (those with PAGE_USER flag) in the source
	// and add them to the child's address space with COW semantics.

	// Handle PML4 entries 0-255 (user space)
	// User space starts fresh and we copy user pages with COW semantics
	for (int i = 0; i < 256; i++) {
		if (!(src_pml4[i] & PAGE_PRESENT))
			continue;

		uint64_t src_pdpt_phys = src_pml4[i] & PTE_ADDR_MASK;
		uint64_t *src_pdpt = (uint64_t *)phys_to_virt(src_pdpt_phys);

		// Allocate PDPT for new address space from safe PT pool
		uint64_t pdpt_phys = allocate_pt_page();
		if (!pdpt_phys)
			goto fail;
		// Page already zeroed by allocate_pt_page
		new_pml4[i] = pdpt_phys | (src_pml4[i] & PTE_FLAGS_MASK);
		uint64_t *new_pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

		for (int j = 0; j < 512; j++) {
			if (!(src_pdpt[j] & PAGE_PRESENT))
				continue;

			uint64_t src_pd_phys = src_pdpt[j] & PTE_ADDR_MASK;
			uint64_t *src_pd =
				(uint64_t *)phys_to_virt(src_pd_phys);

			uint64_t pd_phys = allocate_pt_page();
			if (!pd_phys)
				goto fail;
			// Page already zeroed by allocate_pt_page
			new_pdpt[j] = pd_phys | (src_pdpt[j] & PTE_FLAGS_MASK);
			uint64_t *new_pd = (uint64_t *)phys_to_virt(pd_phys);

			for (int k = 0; k < 512; k++) {
				if (!(src_pd[k] & PAGE_PRESENT))
					continue;

				if (src_pd[k] & PAGE_SIZE_FLAG) {
					// 2MB huge page - share with COW if it has user flag
					if (src_pd[k] & PAGE_USER) {
						uint64_t pte_irq =
							local_irq_save();
						uint64_t phys_2mb =
							src_pd[k] &
							0x000FFFFFFFE00000ULL;
						uint64_t cow_flags =
							(src_pd[k] &
							 ~PAGE_WRITABLE) |
							PAGE_COW;
						src_pd[k] = cow_flags;
						new_pd[k] = cow_flags;
						/* One reference for the mapping being created in the
						 * child.  No seeding: the parent's own mapping has
						 * always held one since the page was allocated.
						 *
						 * A 2MB mapping covers 512 frames,
						 * each counted separately, so the
						 * child's reference is taken on
						 * every one -- teardown drops one
						 * from every one to match. */
						for (int p2 = 0; p2 < 512; p2++)
							mm_get_page(
								phys_2mb +
								(uint64_t)p2 *
									PAGE_SIZE);
						local_irq_restore(pte_irq);
					} else {
						// Kernel page - just share
						new_pd[k] = src_pd[k];
					}
				} else {
					uint64_t src_pt_phys =
						src_pd[k] & PTE_ADDR_MASK;
					uint64_t *src_pt =
						(uint64_t *)phys_to_virt(
							src_pt_phys);

					uint64_t pt_phys = allocate_pt_page();
					if (!pt_phys)
						goto fail;
					// Page already zeroed by allocate_pt_page
					new_pd[k] = pt_phys | (src_pd[k] &
							       PTE_FLAGS_MASK);
					uint64_t *new_pt =
						(uint64_t *)phys_to_virt(
							pt_phys);

					for (int l = 0; l < 512; l++) {
						if (!(src_pt[l] & PAGE_PRESENT))
							continue;

						/* Device MMIO PTEs: share the
						 * mapping, never COW/refcount
						 * (phys not allocator-owned) */
						if (src_pt[l] & PAGE_DEVICE) {
							new_pt[l] = src_pt[l];
							continue;
						}

						if (src_pt[l] & PAGE_USER) {
							// User page — mark both parent and child COW.
							// Disable IRQs for only this PTE so the
							// {mark-COW, incref} pair is atomic w.r.t. any
							// interrupt handler on this CPU.
							uint64_t pte_irq =
								local_irq_save();
							uint64_t phys_page =
								src_pt[l] &
								0x000FFFFFFFFFF000ULL;
							uint64_t cow_flags =
								(src_pt[l] &
								 ~PAGE_WRITABLE) |
								PAGE_COW;
							src_pt[l] = cow_flags;
							new_pt[l] = cow_flags;

							// Increment page reference count
							/* One reference for the mapping being created in the
							 * child.  No seeding: the parent's own mapping has
							 * always held one since the page was allocated. */
							mm_get_page(phys_page);
							local_irq_restore(
								pte_irq);
						} else {
							// Kernel page - just copy mapping
							new_pt[l] = src_pt[l];
						}
					}
				}
			}
		}
	}

	// Flush TLB on this CPU (IRQs already enabled at this point)
	mm_flush_all_tlb();

	// CRITICAL: Flush TLB on ALL CPUs! We just marked the source (parent)
	// pages as read-only/COW. If the parent is running on another CPU with
	// stale TLB entries that still have write permission, it could write to
	// pages without triggering a COW fault, corrupting shared memory.
	if (sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}

	return new_pml4;

fail:
	mm_destroy_address_space(new_pml4);
	return NULL;
}

// Helper: check if virtual address falls in an in-use MAP_SHARED region
static bool is_in_shared_range(uint64_t vaddr, const struct mmap_region *regions,
			       int num_regions)
{
	if (!regions)
		return false;
	for (int i = 0; i < num_regions; i++) {
		if (!regions[i].in_use || !(regions[i].flags & MAP_SHARED))
			continue;
		if (vaddr >= regions[i].start &&
		    vaddr < regions[i].start + regions[i].length) {
			return true;
		}
	}
	return false;
}

// Clone an address space with support for shared memory regions
// For MAP_SHARED regions, we keep the same physical pages (no COW)
uint64_t *mm_clone_address_space_with_shared(uint64_t *src_pml4,
					     const struct mmap_region *regions,
					     int num_regions)
{
	if (!src_pml4) {
		return NULL;
	}

	// Create new address space (this sets up kernel mappings)
	uint64_t *new_pml4 = mm_create_user_address_space();
	if (!new_pml4) {
		return NULL;
	}
	AS_TRACK_RETAG(virt_to_phys(new_pml4),
		       (uint32_t)(uintptr_t)__builtin_return_address(0));

	// CRITICAL: Disable interrupts only for the narrow per-PTE window where
	// we atomically mark a source PTE as COW and increment its refcount.
	// See mm_clone_address_space() for the detailed rationale.

	// Handle PML4 entries 0-255 (user space)
	for (int i = 0; i < 256; i++) {
		if (!(src_pml4[i] & PAGE_PRESENT))
			continue;

		uint64_t src_pdpt_phys = src_pml4[i] & PTE_ADDR_MASK;
		uint64_t *src_pdpt = (uint64_t *)phys_to_virt(src_pdpt_phys);

		uint64_t pdpt_phys = allocate_pt_page();
		if (!pdpt_phys)
			goto fail;
		new_pml4[i] = pdpt_phys | (src_pml4[i] & PTE_FLAGS_MASK);
		uint64_t *new_pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

		for (int j = 0; j < 512; j++) {
			if (!(src_pdpt[j] & PAGE_PRESENT))
				continue;

			uint64_t src_pd_phys = src_pdpt[j] & PTE_ADDR_MASK;
			uint64_t *src_pd =
				(uint64_t *)phys_to_virt(src_pd_phys);

			uint64_t pd_phys = allocate_pt_page();
			if (!pd_phys)
				goto fail;
			new_pdpt[j] = pd_phys | (src_pdpt[j] & PTE_FLAGS_MASK);
			uint64_t *new_pd = (uint64_t *)phys_to_virt(pd_phys);

			for (int k = 0; k < 512; k++) {
				if (!(src_pd[k] & PAGE_PRESENT))
					continue;

				// Calculate virtual address for this PD entry
				uint64_t vaddr_base = ((uint64_t)i << 39) |
						      ((uint64_t)j << 30) |
						      ((uint64_t)k << 21);

				if (src_pd[k] & PAGE_SIZE_FLAG) {
					// 2MB huge page
					if (src_pd[k] & PAGE_USER) {
						if (is_in_shared_range(
							    vaddr_base,
							    regions,
							    num_regions)) {
							/* Shared — same physical
							 * page, still writable.
							 *
							 * It needs a reference
							 * exactly as much as the
							 * copy-on-write case
							 * below: this branch took
							 * none at all, so the
							 * first of the two
							 * address spaces to be
							 * torn down would drop
							 * the last reference and
							 * release 2MB of pages
							 * the other one still had
							 * mapped.  One per frame,
							 * matching how teardown
							 * releases them. */
							uint64_t pte_irq =
								local_irq_save();
							uint64_t sh_2mb =
								src_pd[k] &
								0x000FFFFFFFE00000ULL;

							new_pd[k] = src_pd[k];
							for (int p2 = 0; p2 < 512;
							     p2++)
								mm_get_page(
									sh_2mb +
									(uint64_t)p2 *
										PAGE_SIZE);
							local_irq_restore(pte_irq);
						} else {
							// Not shared - use COW
							uint64_t pte_irq =
								local_irq_save();
							uint64_t phys_2mb =
								src_pd[k] &
								0x000FFFFFFFE00000ULL;
							uint64_t cow_flags =
								(src_pd[k] &
								 ~PAGE_WRITABLE) |
								PAGE_COW;
							src_pd[k] = cow_flags;
							new_pd[k] = cow_flags;
							// Fix: track refcount for 2MB COW pages (same as 4KB)
							/* One reference for the mapping being created in the
							 * child.  No seeding: the parent's own mapping has
							 * always held one since the page was allocated. */
							/* All 512 frames of the
							 * 2MB mapping, matching
							 * teardown. */
							for (int p2 = 0; p2 < 512;
							     p2++)
								mm_get_page(
									phys_2mb +
									(uint64_t)p2 *
										PAGE_SIZE);
							local_irq_restore(
								pte_irq);
						}
					} else {
						new_pd[k] = src_pd[k];
					}
				} else {
					uint64_t src_pt_phys =
						src_pd[k] & PTE_ADDR_MASK;
					uint64_t *src_pt =
						(uint64_t *)phys_to_virt(
							src_pt_phys);

					uint64_t pt_phys = allocate_pt_page();
					if (!pt_phys)
						goto fail;
					new_pd[k] = pt_phys | (src_pd[k] &
							       PTE_FLAGS_MASK);
					uint64_t *new_pt =
						(uint64_t *)phys_to_virt(
							pt_phys);

					for (int l = 0; l < 512; l++) {
						if (!(src_pt[l] & PAGE_PRESENT))
							continue;

						/* Device MMIO PTEs: share the
						 * mapping, never COW/refcount
						 * (phys not allocator-owned) */
						if (src_pt[l] & PAGE_DEVICE) {
							new_pt[l] = src_pt[l];
							continue;
						}

						// Calculate full virtual address
						uint64_t vaddr =
							vaddr_base |
							((uint64_t)l << 12);

						if (src_pt[l] & PAGE_USER) {
							uint64_t phys_page =
								src_pt[l] &
								0x000FFFFFFFFFF000ULL;

							if (is_in_shared_range(
								    vaddr,
								    regions,
								    num_regions)) {
								// Shared region — map same physical page.
								// Briefly disable IRQs for the {write-PTE, incref} pair.
								uint64_t pte_irq =
									local_irq_save();
								new_pt[l] = src_pt
									[l];
								/* One reference for the mapping being created in the
								 * child.  No seeding: the parent's own mapping has
								 * always held one since the page was allocated. */
								mm_get_page(phys_page);
								local_irq_restore(
									pte_irq);
							} else {
								// Not shared — mark COW.
								// Disable IRQs for the {mark-COW-in-src, write-child-PTE, incref} trio.
								uint64_t pte_irq =
									local_irq_save();
								uint64_t cow_flags =
									(src_pt[l] &
									 ~PAGE_WRITABLE) |
									PAGE_COW;
								src_pt[l] =
									cow_flags;
								new_pt[l] =
									cow_flags;

								// Increment page reference count
								/* One reference for the mapping being created in the
								 * child.  No seeding: the parent's own mapping has
								 * always held one since the page was allocated. */
								mm_get_page(phys_page);
								local_irq_restore(
									pte_irq);
							}
						} else {
							new_pt[l] = src_pt[l];
						}
					}
				}
			}
		}
	}

	// Flush TLB on this CPU (IRQs already enabled at this point)
	mm_flush_all_tlb();

	// CRITICAL: Flush TLB on ALL CPUs! We just marked the source (parent)
	// pages as read-only/COW. If the parent is running on another CPU with
	// stale TLB entries that still have write permission, it could write to
	// pages without triggering a COW fault, corrupting shared memory.
	if (sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}

	return new_pml4;

fail:
	mm_destroy_address_space(new_pml4);
	return NULL;
}

// ============================================================================
// SYSCALL/SYSRET CONFIGURATION
// ============================================================================

// MSR definitions for SYSCALL/SYSRET
#define MSR_STAR 0xC0000081 // Segment selectors for SYSCALL
#define MSR_LSTAR 0xC0000082 // RIP for SYSCALL (64-bit)
#define MSR_CSTAR 0xC0000083 // RIP for SYSCALL (compat mode)
#define MSR_SFMASK 0xC0000084 // RFLAGS mask for SYSCALL
#define MSR_EFER 0xC0000080 // Extended Feature Enable Register

// Read MSR
static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t low = value & 0xFFFFFFFF;
	uint32_t high = value >> 32;
	__asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Syscall entry point - defined in syscall.asm
extern void syscall_entry(void);

// Global flag indicating SMAP is active (used by copy_from_user/copy_to_user)
bool g_smap_enabled = false;

// SMAP control: temporarily allow supervisor access to user pages
void smap_disable(void)
{
	if (g_smap_enabled) {
		__asm__ volatile("stac" ::: "cc");
	}
}

// SMAP control: re-enable SMAP protection
void smap_enable(void)
{
	if (g_smap_enabled) {
		__asm__ volatile("clac" ::: "cc");
	}
}

// Enable SMEP and SMAP if CPU supports them
__no_stack_protector void mm_enable_smep_smap(void)
{
	// Check CPUID leaf 7, subleaf 0 for SMEP/SMAP support
	uint32_t eax, ebx, ecx, edx;
	eax = 7;
	ecx = 0;
	__asm__ volatile("cpuid"
			 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
			 : "a"(eax), "c"(ecx));

	uint64_t cr4;
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

	// FSGSBASE: bit 0 of EBX from CPUID, enables CR4 bit 16
	// Must set this before any WRFSBASE/RDFSBASE use (e.g. task_load_tls),
	// otherwise #UD fires on CPUs (e.g. VMware) that expose the feature.
	if (ebx & (1 << 0)) {
		g_cpu_features_ext |= CPU_FEATURE_FSGSBASE;
		cr4 |= (1ULL << 16);
	}

	// SMEP: bit 7 of EBX from CPUID, enables CR4 bit 20
	if (ebx & (1 << 7)) {
		cr4 |= (1ULL << 20);
	}

	// SMAP: bit 20 of EBX from CPUID, enables CR4 bit 21
	if (ebx & (1 << 20)) {
		cr4 |= (1ULL << 21);
		g_smap_enabled = true;
	} else {
		g_smap_enabled = false;
	}

	__asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

// Kernel stack for after identity mapping removal
// This needs to be in the kernel's .bss section (which is in higher-half)
#define KERNEL_INIT_STACK_SIZE (64 * 1024) // 64KB kernel init stack
static uint8_t g_kernel_init_stack[KERNEL_INIT_STACK_SIZE]
	__attribute__((aligned(16)));

// Get the new kernel stack top address
uint64_t mm_get_kernel_stack_top(void)
{
	// Stack grows down, return top of stack array minus some space for safety
	return (uint64_t)&g_kernel_init_stack[KERNEL_INIT_STACK_SIZE - 16] &
	       ~0xFULL;
}

// Assembly helper to switch stacks - defined in stack_switch.asm
extern void switch_stack_and_call(uint64_t new_rsp, void (*func)(void));

// Internal function that runs on the new stack
static void continue_after_stack_switch(void);

// Switch to a kernel stack in higher-half space
// This must be called before mm_remove_identity_mapping() because the current
// stack is in low memory (set up by bootloader in identity-mapped region)
void mm_switch_to_kernel_stack(void)
{
	uint64_t new_rsp = mm_get_kernel_stack_top();

	// This function switches the stack and calls continue_after_stack_switch
	// It will NOT return - execution continues from continue_after_stack_switch
	switch_stack_and_call(new_rsp, continue_after_stack_switch);

	// Never reached
}

// This function is called on the new stack
static void continue_after_stack_switch(void)
{
	// Remove identity mapping now that we're on the new stack
	mm_remove_identity_mapping();

	// Continue with the rest of initialization
	extern void continue_system_startup(void);
	continue_system_startup();

	// Never returns
}

// Remove identity mapping from kernel PML4
// After this, physical memory can only be accessed via direct map at PHYS_MAP_BASE
void mm_remove_identity_mapping(void)
{
	// Disable interrupts during this critical section
	// Interrupt handlers may access identity-mapped memory
	__asm__ volatile("cli");

	// Get current PML4 via direct map
	uint64_t pml4_phys = get_cr3() & ~0xFFFULL;
	uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

	// Clear PML4 entry 0 - this removes the 0-512GB identity mapping
	if (pml4[0] & PAGE_PRESENT) {
		pml4[0] = 0;
	}

	// Clear any other low entries that were identity-mapped
	for (int i = 1; i < 4; i++) {
		if (pml4[i] & PAGE_PRESENT) {
			pml4[i] = 0;
		}
	}

	// Flush entire TLB
	uint64_t cr3_val = get_cr3();
	set_cr3(cr3_val);

	// Re-enable interrupts now that identity mapping is removed
	__asm__ volatile("sti");
}

// Enable NX (No-Execute) bit support
__no_stack_protector void mm_enable_nx(void)
{
	uint64_t efer = rdmsr(MSR_EFER);
	efer |= (1ULL << 11); // Set NXE (No-Execute Enable) bit
	wrmsr(MSR_EFER, efer);
}

// Identity-map physical memory for SMP AP trampoline
// Maps physical pages to the same virtual address (identity mapping)
bool mm_identity_map_for_smp(uint64_t physical_addr, size_t size)
{
	uint64_t page_aligned = physical_addr & ~0xFFFULL;
	uint64_t end_addr = (physical_addr + size + 0xFFF) & ~0xFFFULL;

	while (page_aligned < end_addr) {
		// Map physical address to same virtual address (identity map)
		// Use PAGE_PRESENT | PAGE_WRITABLE, no NX since code will execute here
		if (!mm_map_page(page_aligned, page_aligned,
				 PAGE_PRESENT | PAGE_WRITABLE)) {
			kprintf("mm_identity_map_for_smp: failed to map 0x%lx\n",
				page_aligned);
			return false;
		}
		page_aligned += 0x1000;
	}

	smp_dbg("SMP: Identity-mapped 0x%lx - 0x%lx for AP trampoline\n",
		physical_addr & ~0xFFFULL, end_addr);
	return true;
}

// Remove identity mapping for SMP AP trampoline
void mm_remove_smp_identity_map(uint64_t physical_addr, size_t size)
{
	uint64_t page_aligned = physical_addr & ~0xFFFULL;
	uint64_t end_addr = (physical_addr + size + 0xFFF) & ~0xFFFULL;

	// Unmap all pages without individual TLB shootdowns (batched for performance)
	while (page_aligned < end_addr) {
		mm_unmap_page_no_shootdown(page_aligned);
		page_aligned += 0x1000;
	}

	// CRITICAL: Single batched TLB shootdown. All CPUs (BSP and APs) share
	// the kernel page tables and may have TLB entries for the trampoline
	// identity mapping. We must invalidate them on ALL CPUs before proceeding,
	// otherwise a CPU could use a stale TLB entry and access the wrong memory
	// or page fault.
	if (sched_is_smp()) {
		smp_tlb_shootdown_sync();
	}

	smp_dbg("SMP: Removed identity mapping for AP trampoline\n");
}

// Initialize SYSCALL/SYSRET
void mm_initialize_syscall(void)
{
	// Enable SCE (System Call Extensions) in EFER
	uint64_t efer = rdmsr(MSR_EFER);
	efer |= 1; // Set SCE bit
	wrmsr(MSR_EFER, efer);

	// Configure STAR register for SYSCALL/SYSRET segment switching
	// STAR layout:
	//   Bits 63:48 = User segment base (for SYSRET)
	//   Bits 47:32 = Kernel segment base (for SYSCALL)
	//
	// SYSCALL sets: CS = STAR[47:32], SS = STAR[47:32] + 8
	// SYSRET sets:  CS = STAR[63:48] + 16 | 3, SS = STAR[63:48] + 8 | 3
	//
	// Our GDT layout:
	//   0x00: null
	//   0x08: kernel code
	//   0x10: kernel data
	//   0x18: user code
	//   0x20: user data
	//
	// For SYSCALL (entering kernel):
	//   STAR[47:32] = 0x08 → CS = 0x08 (kernel code), SS = 0x10 (kernel data)
	//
	// For SYSRET (returning to user):
	//   STAR[63:48] = 0x10 → CS = 0x10+16|3 = 0x23, SS = 0x10+8|3 = 0x1B
	//   This means CS = 0x23 (GDT[4] = user data) and SS = 0x1B (GDT[3] = user code)
	//   Note: Segments are reversed but work because 64-bit mode ignores most segment attributes

	uint64_t star = 0;
	star |= ((uint64_t)0x10 << 48); // User base: CS=0x23, SS=0x1B
	star |= ((uint64_t)0x08 << 32); // Kernel base: CS=0x08, SS=0x10
	wrmsr(MSR_STAR, star);

	// Set LSTAR to syscall entry point (64-bit mode)
	wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

	// Set CSTAR for compatibility mode (unused in 64-bit only kernel)
	wrmsr(MSR_CSTAR, (uint64_t)syscall_entry);

	/* SFMASK: the RFLAGS bits the processor clears when SYSCALL enters the
	 * kernel.  Everything a user thread can set for itself and the kernel
	 * must not inherit belongs here -- SYSCALL clears NOTHING that is not
	 * named, and the flags it leaves alone are the caller's.
	 *
	 * NT is the one with teeth.  POPF at CPL 3 cannot touch IF or IOPL,
	 * but it CAN set NT, and NT changes what IRETQ means: with it set the
	 * processor treats the IRET as a return from a nested task and
	 * switches through the TSS back-link instead of popping the frame.
	 * The syscall return path executes IRETQ (sigreturn restores the whole
	 * register file, which SYSRET cannot), so a program that sets NT and
	 * then takes a signal made the kernel task-switch into whatever the
	 * back-link happened to hold.  Nothing about that is recoverable and
	 * nothing about it looks like its cause.
	 *
	 * The rest: TF (no single-stepping the kernel), IF (entry must be with
	 * interrupts off), DF (kernel string ops run forward -- syscall_entry
	 * has a CLD as well, and this is why it should not have been needed),
	 * AC (a user thread that set AC would let every kernel access bypass
	 * SMAP), IOPL (never run kernel code at a borrowed I/O privilege),
	 * RF (SYSRET cannot restore it, so do not carry it in), ID, and the
	 * arithmetic flags so no user value survives into kernel code.
	 *
	 * This is the set the reference implementation masks, bit for bit. */
	wrmsr(MSR_SFMASK,
	      0x1UL /*CF*/ | 0x4UL /*PF*/ | 0x10UL /*AF*/ | 0x40UL /*ZF*/ |
		      0x80UL /*SF*/ | 0x100UL /*TF*/ | 0x200UL /*IF*/ |
		      0x400UL /*DF*/ | 0x800UL /*OF*/ | 0x3000UL /*IOPL*/ |
		      0x4000UL /*NT*/ | 0x10000UL /*RF*/ | 0x40000UL /*AC*/ |
		      0x200000UL /*ID*/);
}

// ============================================================================
// PAGE REFERENCE COUNTING (for COW fork) - SMP SAFE
// ============================================================================

// Get page index from physical address
static inline uint64_t page_to_index(uint64_t phys_addr)
{
	if (phys_addr < mm_state.memory_start ||
	    phys_addr >= mm_state.memory_end) {
		return (uint64_t)-1; // Out of tracked range
	}
	return (phys_addr - mm_state.memory_start) / PAGE_SIZE;
}

// Initialize page reference counts (called after physical memory init)
void mm_init_page_refcounts(void)
{
	// Already done in mm_initialize_physical_memory
	// This function exists for explicit re-initialization if needed
	if (mm_state.page_refcounts) {
		mm_memset(mm_state.page_refcounts, 0,
			  mm_state.refcount_array_size);
	}
}

/* ============================================================================
 * Physical page reference counting
 *
 * Every allocated page carries a count of the references to it, and it is
 * released when that count reaches zero.  mm_allocate_physical_page() returns a
 * page with a count of ONE: the reference the caller is holding.  Zero means
 * the page is free, and nothing else.
 *
 * That last sentence is the whole point of this rewrite.  The counter used to
 * mean two different things at once -- a page only ENTERED the refcount system
 * when it was first shared, so zero meant either "nobody has it" or "nobody is
 * counting".  Callers could not tell which, so every fork site open-coded
 *
 *	if (mm_get_page_refcount(p) == 0)
 *		mm_incref_page(p);	// pay for the mapping that already existed
 *	mm_incref_page(p);		// and for the new one
 *
 * which is a check-then-act on a counter other CPUs are modifying, and
 * mm_decref_page() returned "yes, free it" for an untracked page -- so a page
 * that was mapped twice but counted zero times was released by whichever
 * mapping went away first, while the other still pointed at it.  Freed pages
 * are poisoned, so the survivor then read 0xFEEDFACE out of its own memory.
 *
 * With a count that starts at one and is taken by every mapping, none of those
 * cases exist: there is nothing to seed, nothing to test before incrementing,
 * and no state in which a mapped page has no reference.
 * ========================================================================== */

/* Take a reference.  The caller must already hold one -- taking a reference to
 * a page nobody holds is a use-after-free, and says so. */
void mm_get_page(uint64_t phys_addr)
{
	uint64_t idx = page_to_index(phys_addr);
	uint16_t old;

	if (idx == (uint64_t)-1 || !mm_state.page_refcounts)
		return;

	old = __atomic_fetch_add(&mm_state.page_refcounts[idx], 1,
				 __ATOMIC_ACQ_REL);
	if (unlikely(old == 0)) {
		WARN(1, "mm_get_page: reference taken on free page 0x%lx (use-after-free)",
		     (unsigned long)phys_addr);
	} else if (unlikely(old == 0xFFFF)) {
		/* Saturated: undo rather than wrap to zero, which would free a
		 * page that is still mapped in tens of thousands of places. */
		__atomic_fetch_sub(&mm_state.page_refcounts[idx], 1,
				   __ATOMIC_ACQ_REL);
		WARN(1, "mm_get_page: refcount saturated on page 0x%lx",
		     (unsigned long)phys_addr);
	}
}

/* Drop a reference, releasing the page when the last one goes.
 *
 * Deliberately frees the page itself instead of returning "you should free
 * this now": the split version put the decision at every call site, and a
 * caller that got it wrong either leaked the page or freed one that was still
 * referenced.
 */
void mm_put_page(uint64_t phys_addr)
{
	uint64_t idx = page_to_index(phys_addr);
	uint16_t current;

	if (idx == (uint64_t)-1 || !mm_state.page_refcounts) {
		/* Outside the tracked range: nothing counts it, so the caller's
		 * reference is the only one there can be. */
		mm_free_physical_page(phys_addr);
		return;
	}

	/* CAS rather than a plain fetch_sub so the zero-test and the decrement
	 * are one step: two CPUs could otherwise both read 1, both decrement,
	 * and one would wrap to 65535. */
	current = __atomic_load_n(&mm_state.page_refcounts[idx],
				  __ATOMIC_ACQUIRE);
	for (;;) {
		if (unlikely(current == 0)) {
			WARN(1, "mm_put_page: reference dropped on free page 0x%lx (double free)",
			     (unsigned long)phys_addr);
			return;
		}
		if (__atomic_compare_exchange_n(&mm_state.page_refcounts[idx],
						&current, current - 1, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			break;
		/* CAS failed: `current` was reloaded; retry. */
	}
	if (current == 1)
		mm_free_physical_page(phys_addr);
}

// Get reference count for a physical page (SMP-safe)
uint16_t mm_get_page_refcount(uint64_t phys_addr)
{
	uint64_t idx = page_to_index(phys_addr);
	if (idx == (uint64_t)-1)
		return 0;

	return __atomic_load_n(&mm_state.page_refcounts[idx], __ATOMIC_ACQUIRE);
}
