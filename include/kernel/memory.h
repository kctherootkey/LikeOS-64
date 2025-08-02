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
#define KERNEL_HEAP_START       0xFFFFFFFF80200000ULL    // Higher half + 2MB - start of kernel heap
#define KERNEL_HEAP_SIZE        0x800000    // 8MB initial heap size

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
#define PAGE_NO_EXECUTE         0x8000000000000000ULL

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
uint64_t MmAllocatePhysicalPage(void);
void MmFreePhysicalPage(uint64_t physical_address);
uint64_t MmAllocateContiguousPages(size_t page_count);
void MmFreeContiguousPages(uint64_t physical_address, size_t page_count);

// Virtual Memory Manager
void MmInitializeVirtualMemory(void);
bool MmMapPage(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void MmUnmapPage(uint64_t virtual_addr);
uint64_t MmGetPhysicalAddress(uint64_t virtual_addr);
bool MmIsPageMapped(uint64_t virtual_addr);

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
void MmFlushTLB(uint64_t virtual_addr);
void MmFlushAllTLB(void);

// External linker symbols
extern char kernel_end[];

#endif // _KERNEL_MEMORY_H_
