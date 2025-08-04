// LikeOS-64 Memory Management - Implementation
// Complete Physical Memory Manager, Virtual Memory Manager, and Kernel Heap Allocator

#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"

// Magic numbers for heap validation
#define HEAP_MAGIC_ALLOCATED    0xDEADBEEF
#define HEAP_MAGIC_FREE         0xFEEDFACE
#define HEAP_MAGIC_HEADER       0xABCDEF12

// Memory management state
static struct {
    // Physical memory management
    uint32_t* physical_bitmap;      // Bitmap for physical pages
    uint64_t total_pages;           // Total number of pages
    uint64_t free_pages;            // Number of free pages
    uint64_t bitmap_size;           // Size of bitmap in bytes
    uint64_t memory_start;          // Start of manageable memory
    uint64_t memory_end;            // End of manageable memory
    
    // Virtual memory management
    uint64_t* pml4_table;           // Page Map Level 4 table
    uint64_t next_virtual_addr;     // Next available virtual address
    
    // Heap management
    heap_block_t* heap_start;       // Start of heap
    heap_block_t* heap_end;         // End of heap
    heap_block_t* free_list;        // Free blocks list
    uint64_t heap_size;             // Total heap size
    uint64_t heap_used;             // Used heap memory
    uint32_t allocation_count;      // Number of allocations
    uint32_t deallocation_count;    // Number of deallocations
    
    // Statistics
    memory_stats_t stats;           // Memory statistics
} mm_state = {0};

// I/O port functions
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Utility functions
static void mm_memset(void* dest, int val, size_t len) {
    uint8_t* ptr = (uint8_t*)dest;
    while (len--) {
        *ptr++ = val;
    }
}

static void mm_memcpy(void* dest, const void* src, size_t len) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (len--) {
        *d++ = *s++;
    }
}

// Get current CR3 (page table base)
static uint64_t get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Set CR3 (page table base)
static void set_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// Flush TLB for specific address
void MmFlushTLB(uint64_t virtual_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

// Flush all TLB entries
void MmFlushAllTLB(void) {
    uint64_t cr3 = get_cr3();
    set_cr3(cr3);
}

// PHYSICAL MEMORY MANAGER IMPLEMENTATION

// Find first free bit in bitmap
static uint64_t find_free_page(void) {
    for (uint64_t i = 0; i < mm_state.bitmap_size / sizeof(uint32_t); i++) {
        if (mm_state.physical_bitmap[i] != 0xFFFFFFFF) {
            // Found a uint32_t with free bits
            for (int bit = 0; bit < 32; bit++) {
                if (!(mm_state.physical_bitmap[i] & (1 << bit))) {
                    return i * 32 + bit;
                }
            }
        }
    }
    return (uint64_t)-1; // No free pages
}

// Set bit in bitmap
static void set_page_bit(uint64_t page) {
    uint64_t index = page / 32;
    uint64_t bit = page % 32;
    if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
        mm_state.physical_bitmap[index] |= (1 << bit);
    }
}

// Clear bit in bitmap
static void clear_page_bit(uint64_t page) {
    uint64_t index = page / 32;
    uint64_t bit = page % 32;
    if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
        mm_state.physical_bitmap[index] &= ~(1 << bit);
    }
}

// Check if bit is set in bitmap
static bool is_page_allocated(uint64_t page) {
    uint64_t index = page / 32;
    uint64_t bit = page % 32;
    if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
        return mm_state.physical_bitmap[index] & (1 << bit);
    }
    return true; // Assume allocated if out of range
}

// Initialize physical memory manager
void MmInitializePhysicalMemory(uint64_t memory_size) {
    kprintf("Initializing Physical Memory Manager...\n");
    
    // Calculate memory layout
    // Get kernel end virtual address and convert to physical
    uint64_t kernel_end_virt = (uint64_t)kernel_end;
    uint64_t kernel_end_phys = MmGetPhysicalAddress(kernel_end_virt);
    
    if (kernel_end_phys == 0) {
        // Fallback: assume identity mapping offset
        uint64_t kernel_virt_base = 0xFFFFFFFF80000000ULL;
        kernel_end_phys = kernel_end_virt - kernel_virt_base;
        kprintf("  Using fallback physical address calculation\n");
    }
    
    // Start manageable memory after the kernel, aligned to page boundary
    mm_state.memory_start = PAGE_ALIGN(kernel_end_phys);
    mm_state.memory_end = mm_state.memory_start + memory_size;
    mm_state.total_pages = (mm_state.memory_end - mm_state.memory_start) / PAGE_SIZE;
    mm_state.bitmap_size = PAGE_ALIGN((mm_state.total_pages + 7) / 8);
    
    // Place bitmap after kernel heap area
    mm_state.physical_bitmap = (uint32_t*)(KERNEL_HEAP_START + KERNEL_HEAP_SIZE);
    
    kprintf("  Kernel end virtual: %p\n", kernel_end);
    kprintf("  Kernel end physical: %p\n", (void*)kernel_end_phys);
    kprintf("  Memory range: %p - %p (%lu MB)\n", 
           (void*)mm_state.memory_start, (void*)mm_state.memory_end,
           (mm_state.memory_end - mm_state.memory_start) / (1024*1024));
    kprintf("  Total pages: %d\n", mm_state.total_pages);
    kprintf("  Heap: %p - %p\n", 
           (void*)KERNEL_HEAP_START, (void*)(KERNEL_HEAP_START + KERNEL_HEAP_SIZE));
    kprintf("  Bitmap at: %p (size: %d bytes)\n", 
           mm_state.physical_bitmap, mm_state.bitmap_size);
    kprintf("  Bitmap end: %p\n", 
           (void*)((uint64_t)mm_state.physical_bitmap + mm_state.bitmap_size));

    // Clear bitmap (all pages free initially)
    mm_memset(mm_state.physical_bitmap, 0, mm_state.bitmap_size);

    mm_state.free_pages = mm_state.total_pages;

    // Reserve kernel memory areas
    MmReserveKernelMemory();
    
    kprintf("Physical Memory Manager initialized\n");
}

// Reserve kernel memory areas
void MmReserveKernelMemory(void) {
    // Reserve kernel heap area (virtual heap area doesn't consume physical pages here)
    // The bitmap is what we need to reserve
    uint64_t bitmap_start_page = ((uint64_t)mm_state.physical_bitmap - KERNEL_HEAP_START) / PAGE_SIZE;
    uint64_t bitmap_pages = mm_state.bitmap_size / PAGE_SIZE + 1;
    
    // Note: We don't reserve the heap area in physical memory since it's in virtual space
    // and will be mapped as needed
    
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        if (bitmap_start_page + i < mm_state.total_pages) {
            set_page_bit(bitmap_start_page + i);
            mm_state.free_pages--;
        }
    }
    
    kprintf("  Reserved areas:\n");
    kprintf("    Kernel: Virtual space (heap at %p)\n", (void*)KERNEL_HEAP_START);
    kprintf("    Bitmap: %d pages starting at virtual %p\n", 
           bitmap_pages, mm_state.physical_bitmap);
    kprintf("  Total reserved: %d pages\n", bitmap_pages);
}

// Allocate a physical page
uint64_t MmAllocatePhysicalPage(void) {
    if (mm_state.free_pages == 0) {
        return 0; // Out of memory
    }
    
    uint64_t page = find_free_page();
    if (page == (uint64_t)-1) {
        return 0; // No free pages found
    }
    
    set_page_bit(page);
    mm_state.free_pages--;
    
    return mm_state.memory_start + (page * PAGE_SIZE);
}

// Free a physical page
void MmFreePhysicalPage(uint64_t physical_address) {
    if (physical_address < mm_state.memory_start || physical_address >= mm_state.memory_end) {
        return; // Invalid address
    }
    
    uint64_t page = (physical_address - mm_state.memory_start) / PAGE_SIZE;
    if (!is_page_allocated(page)) {
        return; // Already free
    }
    
    clear_page_bit(page);
    mm_state.free_pages++;
}

// Allocate contiguous physical pages
uint64_t MmAllocateContiguousPages(size_t page_count) {
    if (page_count == 0 || mm_state.free_pages < page_count) {
        return 0;
    }
    
    // Find contiguous free pages
    for (uint64_t start_page = 0; start_page <= mm_state.total_pages - page_count; start_page++) {
        bool found = true;
        
        // Check if all pages in range are free
        for (size_t i = 0; i < page_count; i++) {
            if (is_page_allocated(start_page + i)) {
                found = false;
                break;
            }
        }
        
        if (found) {
            // Allocate all pages in range
            for (size_t i = 0; i < page_count; i++) {
                set_page_bit(start_page + i);
                mm_state.free_pages--;
            }
            return mm_state.memory_start + (start_page * PAGE_SIZE);
        }
    }
    
    return 0; // No contiguous block found
}

// Free contiguous physical pages
void MmFreeContiguousPages(uint64_t physical_address, size_t page_count) {
    for (size_t i = 0; i < page_count; i++) {
        MmFreePhysicalPage(physical_address + (i * PAGE_SIZE));
    }
}

// VIRTUAL MEMORY MANAGER IMPLEMENTATION

// Get page table for virtual address
uint64_t* MmGetPageTable(uint64_t virtual_addr, bool create) {
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    
    // Use existing page tables (assume identity mapping for now)
    mm_state.pml4_table = (uint64_t*)(get_cr3() & ~0xFFF);
    
    uint64_t* pdpt = (uint64_t*)(mm_state.pml4_table[pml4_index] & ~0xFFF);
    if (!pdpt && create) {
        uint64_t pdpt_phys = MmAllocatePhysicalPage();
        if (!pdpt_phys) return NULL;
        mm_memset((void*)pdpt_phys, 0, PAGE_SIZE);
        mm_state.pml4_table[pml4_index] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pdpt = (uint64_t*)pdpt_phys;
    }
    if (!pdpt) return NULL;
    
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_index] & ~0xFFF);
    if (!pd && create) {
        uint64_t pd_phys = MmAllocatePhysicalPage();
        if (!pd_phys) return NULL;
        mm_memset((void*)pd_phys, 0, PAGE_SIZE);
        pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pd = (uint64_t*)pd_phys;
    }
    if (!pd) return NULL;
    
    uint64_t* pt = (uint64_t*)(pd[pd_index] & ~0xFFF);
    if (!pt && create) {
        uint64_t pt_phys = MmAllocatePhysicalPage();
        if (!pt_phys) return NULL;
        mm_memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pt = (uint64_t*)pt_phys;
    }
    
    return pt ? &pt[pt_index] : NULL;
}

// Initialize virtual memory manager
void MmInitializeVirtualMemory(void) {
    kprintf("Initializing Virtual Memory Manager...\n");
    
    mm_state.pml4_table = (uint64_t*)(get_cr3() & ~0xFFF);
    mm_state.next_virtual_addr = KERNEL_HEAP_START + KERNEL_HEAP_SIZE + mm_state.bitmap_size;
    mm_state.next_virtual_addr = PAGE_ALIGN(mm_state.next_virtual_addr);
    
    kprintf("  Page tables at: %p\n", mm_state.pml4_table);
    kprintf("  Next virtual address: %p\n", (void*)mm_state.next_virtual_addr);
    
    kprintf("Virtual Memory Manager initialized\n");
}

// Map virtual page to physical page
bool MmMapPage(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t* pte = MmGetPageTable(virtual_addr, true);
    if (!pte) {
        return false;
    }
    
    *pte = (physical_addr & ~0xFFF) | flags;
    MmFlushTLB(virtual_addr);
    return true;
}

// Unmap virtual page
void MmUnmapPage(uint64_t virtual_addr) {
    uint64_t* pte = MmGetPageTable(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        *pte = 0;
        MmFlushTLB(virtual_addr);
    }
}

// Get physical address for virtual address
uint64_t MmGetPhysicalAddress(uint64_t virtual_addr) {
    uint64_t* pte = MmGetPageTable(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        return (*pte & ~0xFFF) | (virtual_addr & 0xFFF);
    }
    return 0;
}

// Check if page is mapped
bool MmIsPageMapped(uint64_t virtual_addr) {
    uint64_t* pte = MmGetPageTable(virtual_addr, false);
    return pte && (*pte & PAGE_PRESENT);
}

// KERNEL HEAP ALLOCATOR IMPLEMENTATION

// Initialize heap
void MmInitializeHeap(void) {
    kprintf("Initializing Kernel Heap Allocator...\n");
    
    mm_state.heap_start = (heap_block_t*)KERNEL_HEAP_START;
    mm_state.heap_size = KERNEL_HEAP_SIZE;
    mm_state.heap_end = (heap_block_t*)(KERNEL_HEAP_START + KERNEL_HEAP_SIZE);
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
    
    kprintf("  Heap range: %p - %p\n", mm_state.heap_start, mm_state.heap_end);
    kprintf("  Heap size: %d KB\n", mm_state.heap_size / 1024);
    
    kprintf("Kernel Heap Allocator initialized\n");
}

// Find suitable free block
static heap_block_t* find_free_block(size_t size) {
    heap_block_t* current = mm_state.free_list;
    
    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// Split block if it's too large
static void split_block(heap_block_t* block, size_t size) {
    if (block->size < size + sizeof(heap_block_t) + 32) {
        return; // Not worth splitting
    }
    
    heap_block_t* new_block = (heap_block_t*)((uint8_t*)block + sizeof(heap_block_t) + size);
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

// Coalesce adjacent free blocks
static void coalesce_blocks(heap_block_t* block) {
    // Coalesce with next block
    if (block->next && block->next->is_free) {
        block->size += block->next->size + sizeof(heap_block_t);
        if (block->next->next) {
            block->next->next->prev = block;
        }
        block->next = block->next->next;
    }
    
    // Coalesce with previous block
    if (block->prev && block->prev->is_free) {
        block->prev->size += block->size + sizeof(heap_block_t);
        if (block->next) {
            block->next->prev = block->prev;
        }
        block->prev->next = block->next;
    }
}

// Allocate memory
void* kalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    heap_block_t* block = find_free_block(size);
    if (!block) {
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
    
    return (uint8_t*)block + sizeof(heap_block_t);
}

// Free memory
void kfree(void* ptr) {
    if (!ptr) {
        return;
    }
    
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    
    // Validate block
    if (block->magic != HEAP_MAGIC_ALLOCATED || block->is_free) {
        kprintf("ERROR: Invalid free() call for address %p\n", ptr);
        return;
    }
    
    // Mark as free
    block->magic = HEAP_MAGIC_FREE;
    block->is_free = 1;
    
    // Update statistics
    mm_state.heap_used -= block->size + sizeof(heap_block_t);
    mm_state.deallocation_count++;
    
    // Coalesce with adjacent free blocks
    coalesce_blocks(block);
}

// Reallocate memory
void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) {
        return kalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC_ALLOCATED) {
        return NULL;
    }
    
    if (block->size >= new_size) {
        return ptr; // Current block is large enough
    }
    
    // Allocate new block and copy data
    void* new_ptr = kalloc(new_size);
    if (new_ptr) {
        mm_memcpy(new_ptr, ptr, block->size < new_size ? block->size : new_size);
        kfree(ptr);
    }
    
    return new_ptr;
}

// Allocate and zero memory
void* kcalloc(size_t count, size_t size) {
    size_t total_size = count * size;
    void* ptr = kalloc(total_size);
    if (ptr) {
        mm_memset(ptr, 0, total_size);
    }
    return ptr;
}

// MEMORY STATISTICS AND DEBUGGING

// Get memory statistics
void MmGetMemoryStats(memory_stats_t* stats) {
    stats->total_memory = mm_state.memory_end - mm_state.memory_start;
    stats->free_memory = mm_state.free_pages * PAGE_SIZE;
    stats->used_memory = stats->total_memory - stats->free_memory;
    stats->total_pages = mm_state.total_pages;
    stats->free_pages = mm_state.free_pages;
    stats->used_pages = stats->total_pages - stats->free_pages;
    stats->heap_allocated = mm_state.heap_used;
    stats->heap_free = mm_state.heap_size - mm_state.heap_used;
    stats->allocations = mm_state.allocation_count;
    stats->deallocations = mm_state.deallocation_count;
}

// Print memory statistics
void MmPrintMemoryStats(void) {
    memory_stats_t stats;
    MmGetMemoryStats(&stats);
    
    kprintf("\n=== Memory Statistics ===\n");
    kprintf("Physical Memory:\n");
    kprintf("  Total: %d MB (%d pages)\n", 
           stats.total_memory / (1024*1024), stats.total_pages);
    kprintf("  Used:  %d MB (%d pages)\n", 
           stats.used_memory / (1024*1024), stats.used_pages);
    kprintf("  Free:  %d MB (%d pages)\n", 
           stats.free_memory / (1024*1024), stats.free_pages);
    
    kprintf("Kernel Heap:\n");
    kprintf("  Total: %d KB\n", mm_state.heap_size / 1024);
    kprintf("  Used:  %d KB\n", stats.heap_allocated / 1024);
    kprintf("  Free:  %d KB\n", stats.heap_free / 1024);
    kprintf("  Allocations: %d\n", stats.allocations);
    kprintf("  Deallocations: %d\n", stats.deallocations);
    kprintf("  Active allocations: %d\n", stats.allocations - stats.deallocations);
    kprintf("========================\n\n");
}

// Validate heap integrity
bool MmValidateHeap(void) {
    heap_block_t* current = mm_state.heap_start;
    uint32_t block_count = 0;
    
    while (current && (uint8_t*)current < (uint8_t*)mm_state.heap_end) {
        // Check magic numbers
        if (current->magic != HEAP_MAGIC_ALLOCATED && current->magic != HEAP_MAGIC_FREE) {
            kprintf("ERROR: Invalid magic in heap block %d at %p\n", block_count, current);
            return false;
        }
        
        // Check bounds
        if ((uint8_t*)current + sizeof(heap_block_t) + current->size > (uint8_t*)mm_state.heap_end) {
            kprintf("ERROR: Heap block %d extends beyond heap end\n", block_count);
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
void MmPrintHeapStats(void) {
    kprintf("\n=== Heap Block Information ===\n");
    
    heap_block_t* current = mm_state.heap_start;
    uint32_t block_count = 0;
    uint32_t free_blocks = 0;
    uint32_t allocated_blocks = 0;
    
    while (current && (uint8_t*)current < (uint8_t*)mm_state.heap_end && block_count < 20) {
        kprintf("Block %d: %p, size=%d, %s\n", 
               block_count, current, current->size, 
               current->is_free ? "FREE" : "ALLOCATED");
        
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

// Memory detection (simplified - assume 256MB minimum)
void MmDetectMemory(void) {
    // For now, we assume 256MB minimum as specified
    // In a real implementation, this would query BIOS memory map
    kprintf("Memory detection: Assuming 256MB minimum requirement\n");
}
