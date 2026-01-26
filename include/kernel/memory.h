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

// UEFI memory map entry (matching bootloader)
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} memory_map_entry_t;

// UEFI memory type constants
#define EFI_CONVENTIONAL_MEMORY 7

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

// Physical page refcounting (for COW)
void mm_init_page_refcounts(void);
void mm_incref_page(uint64_t physical_addr);
bool mm_decref_page(uint64_t physical_addr);  // Returns true if refcount reached 0
uint16_t mm_get_page_refcount(uint64_t physical_addr);

// Kernel Heap Allocator
void mm_initialize_heap(void);
void* kalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t new_size);
void* kcalloc(size_t count, size_t size);
int heap_validate(const char* caller);

// Memory utilities
void mm_get_memory_stats(memory_stats_t* stats);
void mm_print_memory_stats(void);
void mm_print_heap_stats(void);
bool mm_validate_heap(void);

// Memory detection and initialization
void mm_reserve_kernel_memory(void);

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

// Global flag indicating SMAP is active (use stac/clac only when true)
extern bool g_smap_enabled;

// SMAP control functions for user memory access
// Call smap_disable() before accessing user memory, smap_enable() after
void smap_disable(void);  // Execute STAC if SMAP is enabled
void smap_enable(void);   // Execute CLAC if SMAP is enabled

#endif // _KERNEL_MEMORY_H_
