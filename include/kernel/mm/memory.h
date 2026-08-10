// LikeOS-64 Memory Management - Interface
// Physical Memory Manager, Paging, and Kernel Allocator (kalloc)

#ifndef MEMORY_H
#define MEMORY_H

#include <kernel/uapi/types.h>

// Memory constants
#define PAGE_SIZE 0x1000 // 4KB pages
#define PAGE_ALIGN(addr) ((addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr) (addr & ~(PAGE_SIZE - 1))
#define PAGES_PER_BITMAP_ENTRY 32 // 32 pages per uint32_t
#define KERNEL_HEAP_SIZE 0x800000 // 8MB kernel heap size

// User space virtual address constants
#define USER_SPACE_START 0x0000000000400000ULL // 4MB - typical ELF load address
#define USER_SPACE_END \
	0x00007FFFFFFFFFFFULL // End of user space (canonical low half)
#define USER_STACK_TOP 0x00007FFFFFF00000ULL // User stack top (grows down)
#define USER_STACK_SIZE (2 * 1024 * 1024) // 2MB default user stack
#define KERNEL_STACK_SIZE (16 * 1024) // 16KB kernel stack per task

// Kernel space virtual address constants
#define KERNEL_OFFSET 0xFFFFFFFF80000000ULL // Higher-half kernel base

// Direct map region - maps ALL physical memory to a high virtual address
// This is the conventional approach: no identity mapping after boot.
// Physical address 0 maps to PHYS_MAP_BASE, phys addr X maps to PHYS_MAP_BASE + X
// PML4 index 272 = 0xFFFF880000000000
#define PHYS_MAP_BASE 0xFFFF880000000000ULL
#define PHYS_MAP_PML4_INDEX 272 // (PHYS_MAP_BASE >> 39) & 0x1FF

// Convert physical address to its direct-mapped virtual address
static inline void *phys_to_virt(uint64_t phys_addr)
{
	return (void *)(PHYS_MAP_BASE + phys_addr);
}

// Convert direct-mapped virtual address back to physical address
static inline uint64_t virt_to_phys(void *virt_addr)
{
	return (uint64_t)virt_addr - PHYS_MAP_BASE;
}

// Check if an address is in the direct map region
static inline bool is_direct_map_addr(uint64_t addr)
{
	return (addr >= PHYS_MAP_BASE) &&
	       (addr < (PHYS_MAP_BASE + 0x400000000ULL)); // 16GB
}

/* Could this value plausibly be a pointer to a kernel object?
 *
 * The descriptor table stores several kinds of thing in one slot: small
 * integers tagging a console stream, a network socket or an epoll instance,
 * and real pointers to kernel objects.  Telling a pointer from a tag means
 * reading a magic field out of the candidate, and that read must never be
 * attempted on a value that is not a kernel address at all -- which is what
 * this answers, before any dereference.
 *
 * BOTH kernel ranges are accepted, and that is the point.  An allocation up to
 * the slab's size limit is returned from the slab's own virtual range, while a
 * larger one is served straight out of the direct map.  A predicate that knows
 * only about the first quietly stops recognising an object on the day its
 * structure grows past that limit -- a failure with no symptom at the point of
 * the mistake.
 */
static inline bool kptr_plausible(uint64_t addr)
{
	if (addr < 0x100000ULL)
		return false; /* a tagged marker, not a pointer */
	if (addr >= KERNEL_OFFSET)
		return true; /* higher-half kernel, including the slab */
	return is_direct_map_addr(addr);
}

// Check if a physical address is covered by the bootloader's direct map (16GB)
#define DIRECT_MAP_LIMIT_BYTES (16ULL * 1024 * 1024 * 1024)
static inline bool is_phys_in_direct_map(uint64_t phys_addr)
{
	return phys_addr < DIRECT_MAP_LIMIT_BYTES;
}

// Function to get dynamic kernel heap start address
uint64_t mm_get_kernel_heap_start(void);

/* ---- Demand paging ----------------------------------------------------
 * Resolve a not-present fault on a user address against the current
 * task's lazy regions (anonymous mmap / brk heap / ELF BSS: zero-fill;
 * file-backed private mmap: page-in from the backing file).  Returns 1
 * if the fault was resolved and execution may resume, 0 if the address
 * is not covered by any lazy region (genuine fault).
 * `from_kernel_mode` is diagnostic: file page-in from a kernel-mode
 * fault means a pre-fault shield is missing at some user-copy site. */
int mm_handle_demand_fault(uint64_t fault_addr, int from_kernel_mode);

/* Pre-fault a user buffer range before copying to/from it while holding
 * FS/socket locks: materialises lazy pages (and resolves COW when the
 * copy will WRITE into the buffer) so no page fault needing file I/O can
 * fire inside a lock-holding copy loop.  Anonymous zero-fill faults are
 * safe from any context, so this shield only *needs* to run where the
 * buffer might overlap a file-backed lazy mapping — but it is cheap and
 * called unconditionally from the read/write/send/recv entry points. */
void mm_prefault_user_range(uint64_t addr, uint64_t len, int for_write);

// Page flags for virtual memory
#define PAGE_PRESENT 0x001
#define PAGE_WRITABLE 0x002
#define PAGE_USER 0x004
#define PAGE_WRITE_THROUGH 0x008
#define PAGE_CACHE_DISABLE 0x010
#define PAGE_ACCESSED 0x020
#define PAGE_DIRTY 0x040
#define PAGE_SIZE_FLAG 0x080
#define PAGE_GLOBAL 0x100
#define PAGE_COW 0x200 // Copy-on-Write marker (available bit)
// Device MMIO mapping marker (available bit 10): the physical page behind
// this PTE belongs to a device BAR (e.g. framebuffer VRAM), NOT to the
// physical page allocator.  Unmap/teardown/clone paths must never free,
// refcount or COW such pages — only clear or copy the PTE.
#define PAGE_DEVICE 0x400
#define PAGE_NO_EXECUTE 0x8000000000000000ULL

// Physical address mask for extracting physical address from page table entries
// Bits 12-51 contain the physical address, bits 0-11 are flags, bits 52-62 are available/reserved, bit 63 is NX
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

// Flag mask including NX bit (for preserving flags when copying PTEs)
#define PTE_FLAGS_MASK (0xFFFULL | PAGE_NO_EXECUTE)

// UEFI memory map entry (matching bootloader)
typedef struct {
	uint32_t type;
	uint32_t pad;
	uint64_t physical_start;
	uint64_t virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
} memory_map_entry_t;

// UEFI memory type constants (matching EfiMemoryType from UEFI spec)
#define EFI_RESERVED_MEMORY_TYPE 0 // Not usable
#define EFI_LOADER_CODE 1 // Usable after ExitBootServices
#define EFI_LOADER_DATA 2 // Usable after ExitBootServices
#define EFI_BOOT_SERVICES_CODE 3 // Usable after ExitBootServices
#define EFI_BOOT_SERVICES_DATA 4 // Usable after ExitBootServices
#define EFI_RUNTIME_SERVICES_CODE 5 // RESERVED - runtime firmware code
#define EFI_RUNTIME_SERVICES_DATA 6 // RESERVED - runtime firmware data
#define EFI_CONVENTIONAL_MEMORY 7 // Free usable memory
#define EFI_UNUSABLE_MEMORY 8 // Memory with errors, don't use
#define EFI_ACPI_RECLAIM_MEMORY 9 // ACPI tables, can reclaim after parsing
#define EFI_ACPI_MEMORY_NVS 10 // RESERVED - ACPI firmware needs this
#define EFI_MEMORY_MAPPED_IO 11 // RESERVED - MMIO regions
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12 // RESERVED - MMIO port space
#define EFI_PAL_CODE 13 // RESERVED - processor specific
#define EFI_PERSISTENT_MEMORY 14 // Persistent memory (NVDIMM)
#define EFI_MAX_MEMORY_TYPE 15

// Helper to check if memory type is usable (safe to allocate from)
static inline int mm_is_usable_memory_type(uint32_t type)
{
	switch (type) {
	case EFI_LOADER_CODE:
	case EFI_LOADER_DATA:
	case EFI_BOOT_SERVICES_CODE:
	case EFI_BOOT_SERVICES_DATA:
	case EFI_CONVENTIONAL_MEMORY:
		return 1; // These are safe to use after ExitBootServices
	default:
		return 0; // Everything else is reserved
	}
}

// Memory map information passed from bootloader
#define MAX_MEMORY_MAP_ENTRIES 256
typedef struct {
	uint32_t entry_count;
	uint32_t descriptor_size;
	uint64_t total_memory;
	memory_map_entry_t entries[MAX_MEMORY_MAP_ENTRIES];
} memory_map_info_t;

// Framebuffer information (matching bootloader)
typedef struct {
	void *framebuffer_base;
	uint32_t framebuffer_size;
	uint32_t horizontal_resolution;
	uint32_t vertical_resolution;
	uint32_t pixels_per_scanline;
	uint32_t bytes_per_pixel;
} boot_framebuffer_info_t;

// Boot information passed from bootloader
typedef struct {
	boot_framebuffer_info_t fb_info;
	memory_map_info_t mem_info;
	uint64_t rsdp_address; // ACPI RSDP physical address from UEFI
	uint64_t
		smp_trampoline_addr; // Reserved SMP AP trampoline address (4KB aligned, < 1MB)
	uint64_t
		boot_epoch; // Unix epoch seconds at boot (from UEFI GetTime before ExitBootServices)
	uint64_t
		direct_map_bytes; // Bytes of physical RAM covered by the PML4[272] direct map (0 = legacy bootloader, assume 16 GB)
} boot_info_t;

// Memory regions
typedef struct {
	uint64_t start;
	uint64_t end;
	uint32_t type;
} memory_region_t;

// Memory statistics
typedef struct {
	uint64_t total_memory;
	uint64_t free_memory;
	uint64_t used_memory;
	uint64_t total_pages;
	uint64_t free_pages;
	uint64_t used_pages;
	uint64_t heap_allocated;
	uint64_t heap_free;
	uint32_t allocations;
	uint32_t deallocations;
	/* Ownership breakdown of used pages — lets memstat attribute growth
	 * to a subsystem instead of showing one opaque "used" number. */
	uint64_t slab_pages; // pages held by the slab allocator (incl. large)
	uint64_t slab_large_active; // outstanding large allocations (count)
	uint64_t pagecache_pages; // file/page cache pages
} memory_stats_t;

// Heap block header
typedef struct heap_block {
	uint32_t magic;
	uint32_t size;
	uint8_t is_free;
	uint8_t padding[3];
	struct heap_block *next;
	struct heap_block *prev;
} heap_block_t;

// Physical Memory Manager
void mm_initialize_physical_memory(uint64_t memory_size);
void mm_initialize_from_boot_info(boot_info_t *boot_info);
uint64_t mm_allocate_physical_page(void);
void mm_free_physical_page(uint64_t physical_address);
uint64_t mm_allocate_contiguous_pages(size_t page_count);
void mm_free_contiguous_pages(uint64_t physical_address, size_t page_count);

// Virtual Memory Manager
void mm_initialize_virtual_memory(void);
void mm_remap_kernel_with_nx(void);
bool mm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
bool mm_map_page_no_shootdown(uint64_t virtual_addr, uint64_t physical_addr,
			      uint64_t flags);
bool mm_map_page_in_address_space(uint64_t *pml4, uint64_t virtual_addr,
				  uint64_t physical_addr, uint64_t flags);
void mm_unmap_page(uint64_t virtual_addr);
void mm_unmap_page_no_shootdown(uint64_t virtual_addr);
void mm_unmap_page_in_address_space(uint64_t *pml4, uint64_t virtual_addr);
uint64_t mm_get_physical_address(uint64_t virtual_addr);
uint64_t mm_get_physical_address_from_pml4(uint64_t *pml4,
					   uint64_t virtual_addr);
bool mm_is_page_mapped(uint64_t virtual_addr);
uint64_t mm_get_page_flags(uint64_t virtual_addr);
bool mm_set_page_flags(uint64_t virtual_addr, uint64_t flags);

// User Address Space Management
uint64_t *mm_create_user_address_space(void);
void mm_destroy_address_space(uint64_t *pml4);
void mm_switch_address_space(uint64_t *pml4);
uint64_t *mm_get_current_address_space(void);
bool mm_user_addr_mapped(uint64_t vaddr, size_t len);
bool mm_map_user_stack(uint64_t *pml4, uint64_t stack_top, size_t stack_size);
bool mm_map_user_page(uint64_t *pml4, uint64_t virtual_addr,
		      uint64_t physical_addr, uint64_t flags);

// Copy-on-Write support
bool mm_mark_page_cow(uint64_t virtual_addr);
bool mm_handle_cow_fault(uint64_t fault_addr);
uint64_t *mm_clone_address_space(uint64_t *src_pml4);

// Clone with shared memory support (for MAP_SHARED regions)
// Takes the caller's mmap region table directly and shares (rather than COWs)
// every in-use MAP_SHARED range in it.  The table is read, never modified.
// Passing it straight through avoids materialising a copy of the ranges on the
// kernel stack, which does not scale with TASK_MAX_MMAP.
struct mmap_region;
uint64_t *mm_clone_address_space_with_shared(uint64_t *src_pml4,
					     const struct mmap_region *regions,
					     int num_regions);

/* ---- batched unmap: flush first, free second ---------------------------
 *
 * Clearing a page-table entry ends the translation on the local CPU only.
 * Releasing the page before every other CPU has been told to forget it hands
 * a still-reachable page back to the allocator -- and freed pages are
 * poisoned, so the old owner starts reading 0xFEEDFACE out of its own memory.
 *
 * Collect the pages of a range here, invalidate once, then release the batch.
 * See the comment on the definitions in kernel/mm/memory.c for which paths
 * need this and which are already covered.
 */
#define MM_TLB_GATHER_BATCH 64
struct mm_tlb_gather {
	uint64_t pages[MM_TLB_GATHER_BATCH];
	unsigned n;
	/* Physical root of the address space being unmapped, so the flush can
	 * ask which CPUs actually have it loaded instead of interrupting all
	 * of them.  Zero means "unknown" and falls back to a broadcast. */
	uint64_t pml4_phys;
};

/* ---- batched release ----------------------------------------------------
 *
 * Releasing pages one at a time costs an acquire/release of a global lock and
 * an interrupt-off window each, which is the dominant cost of tearing down a
 * large address space -- and, because a CPU queued behind that lock waits with
 * interrupts disabled, it stalls the TLB shootdown acknowledgements other CPUs
 * are blocked on.  These take the lock once for the whole batch.
 *
 * Both are callable only from a preemptible context.
 */
#define MM_FREE_BATCH_MAX 64
/* Release pages outright: the caller holds the only reference to each. */
void mm_free_physical_pages_batch(const uint64_t *phys, unsigned n);
/* Drop one reference on each, releasing those that reach zero. */
void mm_put_pages_batch(const uint64_t *phys, unsigned n);

/* `pml4` is the address space the pages are being unmapped from; pass NULL
 * only if it is genuinely not known, which costs a broadcast per flush. */
void mm_tlb_gather_init(struct mm_tlb_gather *g, uint64_t *pml4);
/* Queue a page whose entry is already cleared; its reference is held until the
 * flush, which is what stops the page being reused too early. */
void mm_tlb_gather_page(struct mm_tlb_gather *g, uint64_t phys);
/* Invalidate on every CPU, then drop the batch's references. */
void mm_tlb_gather_flush(struct mm_tlb_gather *g);

/* ---- the task's mmap region table --------------------------------------
 *
 * The records describing a process's mappings, and the teardown that keeps
 * them in step with the page tables.  These live here rather than in the
 * syscall layer because they ARE address-space management: the demand-fault
 * handler above is their other main consumer, and mm_unmap_range_and_regions()
 * is the only correct way to remove a mapping -- pages and record together.
 */
struct task;
/* Find the in-use region covering `addr`, or NULL. */
struct mmap_region *mm_find_mmap_region(struct task *task, uint64_t addr);
/* Claim a free, zeroed region slot (in_use stays false -- the caller sets it
 * once the mapping is fully built), or NULL when the table is full. */
struct mmap_region *mm_alloc_mmap_region(struct task *task);
/* Tear down every mapping in [addr, addr+length): free the pages AND release
 * or trim the records covering them.  Returns 1 if anything was found.  Both
 * munmap and MAP_FIXED must use this -- freeing pages without releasing the
 * records leaks slots until every later mmap() fails with -ENOMEM. */
int mm_unmap_range_and_regions(struct task *task, uint64_t addr,
			       uint64_t length);
/* Release the physical pages backing [addr, addr+length) but KEEP the mapping
 * and its record.  The next access faults a fresh zero page.  This is what an
 * allocator wants when it returns memory it may need again shortly -- unmapping
 * would punch a hole in the mapping and cost a region record each time. */
void mm_dontneed_range(struct task *task, uint64_t addr, uint64_t length);
/* Fold adjacent records that describe one continuous mapping back into one.
 * Without this, repeated split/remap cycles spend a record each time until the
 * table is full and every later mmap fails. */
void mm_merge_mmap_regions(struct task *task);
/* Pages this address space actually has resident -- the real resident set,
 * as opposed to how much address space has been reserved. */
uint64_t mm_count_resident_pages(uint64_t *pml4);

/* Physical page reference counting.
 *
 * mm_allocate_physical_page() returns a page whose count is ONE -- the
 * reference the caller now holds.  Zero means the page is free and nothing
 * else; there is no "allocated but untracked" state, which is what the old
 * seed-on-first-share counter had and what made a page mapped twice but
 * counted once releasable by whichever mapping went away first.
 *
 * Take a reference for every new mapping of a page, drop one for every mapping
 * removed.  mm_put_page() releases the page itself when the last reference
 * goes, so no caller has to decide whether a free is due. */
void mm_init_page_refcounts(void);
void mm_get_page(uint64_t physical_addr);
void mm_put_page(uint64_t physical_addr);
uint16_t mm_get_page_refcount(uint64_t physical_addr);

// Kernel Heap Allocator
void mm_initialize_heap(void);
void mm_init_pt_pool(void); // Initialize page table page pool
void *kalloc(size_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc(size_t count, size_t size);
int heap_validate(const char *caller);

// DMA-safe allocation (uses legacy heap which is in low physical memory)
// Use these for device DMA buffers that need physical addresses < 4GB
void *kalloc_dma(size_t size);
void *kcalloc_dma(size_t count, size_t size);
void kfree_dma(void *ptr);

// Memory utilities
void mm_get_memory_stats(memory_stats_t *stats);

/* Block/metadata buffer-cache accounting.  Filesystem drivers report the
 * bytes they hold in reclaimable block buffers (e.g. metadata block caches)
 * so sysinfo can surface them as "buffers" without mm or the syscall layer
 * knowing any filesystem specifics. */
void mm_buffercache_account(long delta);
uint64_t mm_buffercache_bytes(void);
void mm_print_memory_stats(void);
void mm_print_heap_stats(void);
bool mm_validate_heap(void);
uint64_t mm_get_free_pages(void);

// Page table management
uint64_t *mm_get_page_table(uint64_t virtual_addr, bool create);
uint64_t *mm_get_page_table_from_pml4(uint64_t *pml4, uint64_t virtual_addr,
				      bool create);
void mm_flush_tlb(uint64_t virtual_addr);
void mm_flush_all_tlb(void);

// MMIO mapping for device BARs above the direct map (> 16GB physical)
// Maps 'num_pages' of device MMIO starting at 'phys_addr' into kernel virtual
// address space with uncacheable (write-through + cache-disable) flags.
// Returns the virtual address, or 0 on failure.
uint64_t mm_map_mmio(uint64_t phys_addr, size_t num_pages);

// Device MMIO mapping for PCI/SoC BARs.
// If the physical range is already covered by the kernel direct map, this
// rewrites the direct-map PTEs in place to use uncacheable attributes so we
// do not create a conflicting WB+UC alias for the same registers.
// If the range is outside the direct map, it falls back to mm_map_mmio().
uint64_t mm_map_device_mmio(uint64_t phys_addr, size_t num_pages);

// SYSCALL/SYSRET configuration
void mm_initialize_syscall(void);

// Memory utility functions
void mm_memset(void *dest, int val, size_t len);
void mm_memcpy(void *dest, const void *src, size_t len);

// External linker symbols
extern char kernel_text_start[];
extern char kernel_text_end[];
extern char kernel_rodata_start[];
extern char kernel_rodata_end[];
extern char kernel_data_start[];
extern char kernel_data_end[];
extern char kernel_bss_start[];
extern char kernel_bss_end[];
extern char kernel_end[];

// Enable NX bit support
void mm_enable_nx(void);

// Enable SMEP/SMAP if CPU supports them
void mm_enable_smep_smap(void);

// Switch to a kernel stack in higher-half space
// Must be called before mm_remove_identity_mapping()
void mm_switch_to_kernel_stack(void);

// Remove identity mapping from kernel PML4
// Call this after all boot-time initialization is complete
void mm_remove_identity_mapping(void);

// Temporarily restore identity mapping for SMP AP trampoline
// Maps physical address range to same virtual address
bool mm_identity_map_for_smp(uint64_t physical_addr, size_t size);
void mm_remove_smp_identity_map(uint64_t physical_addr, size_t size);

// Global flag indicating SMAP is active (use stac/clac only when true)
extern bool g_smap_enabled;

// SMAP control functions for user memory access
// Call smap_disable() before accessing user memory, smap_enable() after
void smap_disable(void); // Execute STAC if SMAP is enabled
void smap_enable(void); // Execute CLAC if SMAP is enabled

// ============================================================================
// KERNEL STACK GUARD PAGE ALLOCATOR
// ============================================================================
// Each guarded stack slot layout (x86-64 stacks grow downward):
//   [ slot_base + 0           .. slot_base + PAGE_SIZE - 1 ]  NOT PRESENT (guard)
//   [ slot_base + PAGE_SIZE   .. slot_base + PAGE_SIZE + usable_size - 1 ]  usable
//
// Virtual range just above the slab allocator region (SLAB_VIRT_END).
// 64 MB pool supports ~3200 fully-loaded 16 KB task stacks simultaneously.
#define KSTACK_VIRT_BASE 0xFFFFFFFF90000000ULL /* just above SLAB_VIRT_END */
#define KSTACK_VIRT_LIMIT 0xFFFFFFFF94000000ULL /* 64 MB pool              */

// Allocate a kernel stack with a guard page below the usable area.
// Returns a pointer to the first byte of the USABLE region (not the guard).
// usable_size must be a non-zero multiple of PAGE_SIZE.
// Returns NULL on allocation failure.
void *mm_alloc_guarded_kstack(size_t usable_size);

// Free a kernel stack previously returned by mm_alloc_guarded_kstack().
// stack_base must be exactly the pointer returned by mm_alloc_guarded_kstack()
// and usable_size must match the value passed at allocation time.
void mm_free_guarded_kstack(void *stack_base, size_t usable_size);

// Mark a single 4 KB page as not-present (guard page).
// If the page is currently covered by a 2 MB large mapping it is split into
// 4 KB entries first so only the requested page is affected.
// Safe to call from any context after mm_init() has returned.
void mm_mark_guard_page(uint64_t virt_addr);

#endif // _KERNEL_MEMORY_H_
