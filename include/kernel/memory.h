// LikeOS-64 Memory Management - Interface
// Physical Memory Manager, Paging, and Kernel Allocator (kalloc)

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

// Memory constants
#define PAGE_SIZE               0x1000      // 4KB pages
#define PAGE_ALIGN(addr)        ((addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(addr)   (addr & ~(PAGE_SIZE - 1))
#define PAGES_PER_BITMAP_ENTRY  32          // 32 pages per uint32_t
#define KERNEL_HEAP_SIZE        0x800000    // 8MB kernel heap size

// User space virtual address constants
#define USER_SPACE_START        0x0000000000400000ULL  // 4MB - typical ELF load address
#define USER_SPACE_END          0x00007FFFFFFFFFFFULL  // End of user space (canonical low half)
#define USER_STACK_TOP          0x00007FFFFFF00000ULL  // User stack top (grows down)
#define USER_STACK_SIZE         (2 * 1024 * 1024)       // 2MB default user stack

// Kernel space virtual address constants  
#define KERNEL_OFFSET           0xFFFFFFFF80000000ULL  // Higher-half kernel base

// Direct map region - maps ALL physical memory to a high virtual address
// This is the Linux-style approach: no identity mapping after boot.
// Physical address 0 maps to PHYS_MAP_BASE, phys addr X maps to PHYS_MAP_BASE + X
// PML4 index 272 = 0xFFFF880000000000 (like Linux's direct map)
#define PHYS_MAP_BASE           0xFFFF880000000000ULL
#define PHYS_MAP_PML4_INDEX     272  // (PHYS_MAP_BASE >> 39) & 0x1FF

// Convert physical address to its direct-mapped virtual address
static inline void* phys_to_virt(uint64_t phys_addr) {
    return (void*)(PHYS_MAP_BASE + phys_addr);
}

// Convert direct-mapped virtual address back to physical address
static inline uint64_t virt_to_phys(void* virt_addr) {
    return (uint64_t)virt_addr - PHYS_MAP_BASE;
}

// Check if an address is in the direct map region
static inline bool is_direct_map_addr(uint64_t addr) {
    return (addr >= PHYS_MAP_BASE) && (addr < (PHYS_MAP_BASE + 0x400000000ULL));  // 16GB
}

// Function to get dynamic kernel heap start address
uint64_t mm_get_kernel_heap_start(void);

// Page flags for virtual memory
#define PAGE_PRESENT            0x001
#define PAGE_WRITABLE           0x002
#define PAGE_USER               0x004
#define PAGE_WRITE_THROUGH      0x008
#define PAGE_CACHE_DISABLE      0x010
#define PAGE_ACCESSED           0x020
#define PAGE_DIRTY              0x040
#define PAGE_SIZE_FLAG          0x080
#define PAGE_GLOBAL             0x100
#define PAGE_COW                0x200       // Copy-on-Write marker (available bit)
#define PAGE_NO_EXECUTE         0x8000000000000000ULL

// Physical address mask for extracting physical address from page table entries
// Bits 12-51 contain the physical address, bits 0-11 are flags, bits 52-62 are available/reserved, bit 63 is NX
#define PTE_ADDR_MASK           0x000FFFFFFFFFF000ULL

// Flag mask including NX bit (for preserving flags when copying PTEs)
#define PTE_FLAGS_MASK          (0xFFFULL | PAGE_NO_EXECUTE)

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
#define EFI_RESERVED_MEMORY_TYPE        0   // Not usable
#define EFI_LOADER_CODE                 1   // Usable after ExitBootServices
#define EFI_LOADER_DATA                 2   // Usable after ExitBootServices
#define EFI_BOOT_SERVICES_CODE          3   // Usable after ExitBootServices
#define EFI_BOOT_SERVICES_DATA          4   // Usable after ExitBootServices
#define EFI_RUNTIME_SERVICES_CODE       5   // RESERVED - runtime firmware code
#define EFI_RUNTIME_SERVICES_DATA       6   // RESERVED - runtime firmware data
#define EFI_CONVENTIONAL_MEMORY         7   // Free usable memory
#define EFI_UNUSABLE_MEMORY             8   // Memory with errors, don't use
#define EFI_ACPI_RECLAIM_MEMORY         9   // ACPI tables, can reclaim after parsing
#define EFI_ACPI_MEMORY_NVS             10  // RESERVED - ACPI firmware needs this
#define EFI_MEMORY_MAPPED_IO            11  // RESERVED - MMIO regions
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12  // RESERVED - MMIO port space
#define EFI_PAL_CODE                    13  // RESERVED - processor specific
#define EFI_PERSISTENT_MEMORY           14  // Persistent memory (NVDIMM)
#define EFI_MAX_MEMORY_TYPE             15

// Helper to check if memory type is usable (safe to allocate from)
static inline int mm_is_usable_memory_type(uint32_t type) {
    switch (type) {
        case EFI_LOADER_CODE:
        case EFI_LOADER_DATA:
        case EFI_BOOT_SERVICES_CODE:
        case EFI_BOOT_SERVICES_DATA:
        case EFI_CONVENTIONAL_MEMORY:
            return 1;  // These are safe to use after ExitBootServices
        default:
            return 0;  // Everything else is reserved
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
    void* framebuffer_base;
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
} memory_stats_t;

// Heap block header
typedef struct heap_block {
    uint32_t magic;
    uint32_t size;
    uint8_t is_free;
    uint8_t padding[3];
    struct heap_block* next;
    struct heap_block* prev;
} heap_block_t;

// Physical Memory Manager
void mm_initialize_physical_memory(uint64_t memory_size);
void mm_initialize_from_boot_info(boot_info_t* boot_info);
uint64_t mm_allocate_physical_page(void);
void mm_free_physical_page(uint64_t physical_address);
uint64_t mm_allocate_contiguous_pages(size_t page_count);
void mm_free_contiguous_pages(uint64_t physical_address, size_t page_count);

// Virtual Memory Manager
void mm_initialize_virtual_memory(void);
void mm_remap_kernel_with_nx(void);
bool mm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
bool mm_map_page_in_address_space(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void mm_unmap_page(uint64_t virtual_addr);
void mm_unmap_page_in_address_space(uint64_t* pml4, uint64_t virtual_addr);
uint64_t mm_get_physical_address(uint64_t virtual_addr);
uint64_t mm_get_physical_address_from_pml4(uint64_t* pml4, uint64_t virtual_addr);
bool mm_is_page_mapped(uint64_t virtual_addr);
uint64_t mm_get_page_flags(uint64_t virtual_addr);
bool mm_set_page_flags(uint64_t virtual_addr, uint64_t flags);

// User Address Space Management
uint64_t* mm_create_user_address_space(void);
void mm_destroy_address_space(uint64_t* pml4);
void mm_switch_address_space(uint64_t* pml4);
uint64_t* mm_get_current_address_space(void);
bool mm_map_user_stack(uint64_t* pml4, uint64_t stack_top, size_t stack_size);
bool mm_map_user_page(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Copy-on-Write support
bool mm_mark_page_cow(uint64_t virtual_addr);
bool mm_handle_cow_fault(uint64_t fault_addr);
uint64_t* mm_clone_address_space(uint64_t* src_pml4);

// Clone with shared memory support (for MAP_SHARED regions)
// shared_regions: array of {start, end} pairs, null-terminated
// These ranges will NOT use COW - they'll share the same physical pages
uint64_t* mm_clone_address_space_with_shared(uint64_t* src_pml4, 
    uint64_t* shared_regions, int num_shared);

// Physical page refcounting (for COW)
void mm_init_page_refcounts(void);
void mm_incref_page(uint64_t physical_addr);
bool mm_decref_page(uint64_t physical_addr);  // Returns true if refcount reached 0
uint16_t mm_get_page_refcount(uint64_t physical_addr);

// Kernel Heap Allocator
void mm_initialize_heap(void);
void mm_init_pt_pool(void);  // Initialize page table page pool
void* kalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t new_size);
void* kcalloc(size_t count, size_t size);
int heap_validate(const char* caller);

// DMA-safe allocation (uses legacy heap which is in low physical memory)
// Use these for device DMA buffers that need physical addresses < 4GB
void* kalloc_dma(size_t size);
void* kcalloc_dma(size_t count, size_t size);
void kfree_dma(void* ptr);

// Memory utilities
void mm_get_memory_stats(memory_stats_t* stats);
void mm_print_memory_stats(void);
void mm_print_heap_stats(void);
bool mm_validate_heap(void);
uint64_t mm_get_free_pages(void);

// Page table management
uint64_t* mm_get_page_table(uint64_t virtual_addr, bool create);
uint64_t* mm_get_page_table_from_pml4(uint64_t* pml4, uint64_t virtual_addr, bool create);
void mm_flush_tlb(uint64_t virtual_addr);
void mm_flush_all_tlb(void);

// SYSCALL/SYSRET configuration
void mm_initialize_syscall(void);

// Memory utility functions
void mm_memset(void* dest, int val, size_t len);
void mm_memcpy(void* dest, const void* src, size_t len);

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

// Global flag indicating SMAP is active (use stac/clac only when true)
extern bool g_smap_enabled;

// SMAP control functions for user memory access
// Call smap_disable() before accessing user memory, smap_enable() after
void smap_disable(void);  // Execute STAC if SMAP is enabled
void smap_enable(void);   // Execute CLAC if SMAP is enabled

#endif // _KERNEL_MEMORY_H_
