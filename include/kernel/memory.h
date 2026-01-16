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
#define KERNEL_HEAP_SIZE        0x800000    // 8MB initial heap size

// User space virtual address constants
#define USER_SPACE_START        0x0000000000400000ULL  // 4MB - typical ELF load address
#define USER_SPACE_END          0x00007FFFFFFFFFFFULL  // End of user space (canonical low half)
#define USER_STACK_TOP          0x00007FFFFFF00000ULL  // User stack top (grows down)
#define USER_STACK_SIZE         (2 * 1024 * 1024)       // 2MB default user stack

// Kernel space virtual address constants  
#define KERNEL_OFFSET           0xFFFFFFFF80000000ULL  // Higher-half kernel base

// Function to get dynamic kernel heap start address
uint64_t MmGetKernelHeapStart(void);

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
void MmInitializePhysicalMemory(uint64_t memory_size);
void MmInitializeFromBootInfo(boot_info_t* boot_info);
uint64_t MmAllocatePhysicalPage(void);
void MmFreePhysicalPage(uint64_t physical_address);
uint64_t MmAllocateContiguousPages(size_t page_count);
void MmFreeContiguousPages(uint64_t physical_address, size_t page_count);

// Virtual Memory Manager
void MmInitializeVirtualMemory(void);
bool MmMapPage(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
bool MmMapPageInAddressSpace(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void MmUnmapPage(uint64_t virtual_addr);
uint64_t MmGetPhysicalAddress(uint64_t virtual_addr);
uint64_t MmGetPhysicalAddressFromPml4(uint64_t* pml4, uint64_t virtual_addr);
bool MmIsPageMapped(uint64_t virtual_addr);
uint64_t MmGetPageFlags(uint64_t virtual_addr);
bool MmSetPageFlags(uint64_t virtual_addr, uint64_t flags);

// User Address Space Management
uint64_t* MmCreateUserAddressSpace(void);
void MmDestroyAddressSpace(uint64_t* pml4);
void MmSwitchAddressSpace(uint64_t* pml4);
uint64_t* MmGetCurrentAddressSpace(void);
bool MmMapUserStack(uint64_t* pml4, uint64_t stack_top, size_t stack_size);
bool MmMapUserPage(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Copy-on-Write support
bool MmMarkPageCOW(uint64_t virtual_addr);
bool MmHandleCOWFault(uint64_t fault_addr);
uint64_t* MmCloneAddressSpace(uint64_t* src_pml4);

// Kernel Heap Allocator
void MmInitializeHeap(void);
void* kalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t new_size);
void* kcalloc(size_t count, size_t size);

// Memory utilities
void MmGetMemoryStats(memory_stats_t* stats);
void MmPrintMemoryStats(void);
void MmPrintHeapStats(void);
bool MmValidateHeap(void);

// Memory detection and initialization
void MmDetectMemory(void);
void MmReserveKernelMemory(void);

// Page table management
uint64_t* MmGetPageTable(uint64_t virtual_addr, bool create);
uint64_t* MmGetPageTableFromPml4(uint64_t* pml4, uint64_t virtual_addr, bool create);
void MmFlushTLB(uint64_t virtual_addr);
void MmFlushAllTLB(void);

// SYSCALL/SYSRET configuration
void MmInitializeSyscall(void);

// Memory utility functions
void mm_memset(void* dest, int val, size_t len);
void mm_memcpy(void* dest, const void* src, size_t len);

// External linker symbols
extern char kernel_end[];

#endif // _KERNEL_MEMORY_H_
