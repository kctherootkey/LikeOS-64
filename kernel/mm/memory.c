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

// Utility functions (non-static, declared in memory.h)
void mm_memset(void* dest, int val, size_t len) {
    uint8_t* ptr = (uint8_t*)dest;
    while (len--) {
        *ptr++ = val;
    }
}

void mm_memcpy(void* dest, const void* src, size_t len) {
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
void mm_flush_tlb(uint64_t virtual_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

// Flush all TLB entries
void mm_flush_all_tlb(void) {
    uint64_t cr3 = get_cr3();
    set_cr3(cr3);
}

// Get dynamic kernel heap start address
uint64_t mm_get_kernel_heap_start(void) {
    // Align kernel_end to page boundary and add some padding
    uint64_t kernel_end_addr = (uint64_t)kernel_end;
    return PAGE_ALIGN(kernel_end_addr);
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
void mm_initialize_physical_memory(uint64_t memory_size) {
    kprintf("Initializing Physical Memory Manager...\n");
    
    // Calculate memory layout
    // Get kernel end virtual address and convert to physical
    uint64_t kernel_end_virt = (uint64_t)kernel_end;
    uint64_t kernel_end_phys = mm_get_physical_address(kernel_end_virt);
    
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
    uint64_t heap_start = mm_get_kernel_heap_start();
    mm_state.physical_bitmap = (uint32_t*)(heap_start + KERNEL_HEAP_SIZE);
    
    kprintf("  Kernel end virtual: %p\n", kernel_end);
    kprintf("  Kernel end physical: %p\n", (void*)kernel_end_phys);
    kprintf("  Memory range: %p - %p (%lu MB)\n", 
           (void*)mm_state.memory_start, (void*)mm_state.memory_end,
           (mm_state.memory_end - mm_state.memory_start) / (1024*1024));
    kprintf("  Total pages: %d\n", mm_state.total_pages);
    kprintf("  Heap: %p - %p\n", 
           (void*)heap_start, (void*)(heap_start + KERNEL_HEAP_SIZE));
    kprintf("  Bitmap at: %p (size: %d bytes)\n", 
           mm_state.physical_bitmap, mm_state.bitmap_size);
    kprintf("  Bitmap end: %p\n", 
           (void*)((uint64_t)mm_state.physical_bitmap + mm_state.bitmap_size));

    // Clear bitmap (all pages free initially)
    mm_memset(mm_state.physical_bitmap, 0, mm_state.bitmap_size);

    mm_state.free_pages = mm_state.total_pages;

    // Reserve kernel memory areas
    mm_reserve_kernel_memory();
    
    kprintf("Physical Memory Manager initialized\n");
}

// Reserve kernel memory areas
void mm_reserve_kernel_memory(void) {
    // Reserve kernel heap area (virtual heap area doesn't consume physical pages here)
    // The bitmap is what we need to reserve
    uint64_t heap_start = mm_get_kernel_heap_start();
    uint64_t bitmap_start_page = ((uint64_t)mm_state.physical_bitmap - heap_start) / PAGE_SIZE;
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
    kprintf("    Kernel: Virtual space (heap at %p)\n", (void*)heap_start);
    kprintf("    Bitmap: %d pages starting at virtual %p\n", 
           bitmap_pages, mm_state.physical_bitmap);
    kprintf("  Total reserved: %d pages\n", bitmap_pages);
}

// Allocate a physical page
uint64_t mm_allocate_physical_page(void) {
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
void mm_free_physical_page(uint64_t physical_address) {
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
uint64_t mm_allocate_contiguous_pages(size_t page_count) {
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
void mm_free_contiguous_pages(uint64_t physical_address, size_t page_count) {
    for (size_t i = 0; i < page_count; i++) {
        mm_free_physical_page(physical_address + (i * PAGE_SIZE));
    }
}

// VIRTUAL MEMORY MANAGER IMPLEMENTATION

// Get page table for virtual address
uint64_t* mm_get_page_table(uint64_t virtual_addr, bool create) {
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    
    // Use existing page tables (assume identity mapping for now)
    mm_state.pml4_table = (uint64_t*)(get_cr3() & ~0xFFF);
    
    uint64_t* pdpt = (uint64_t*)(mm_state.pml4_table[pml4_index] & ~0xFFF);
    if (!pdpt && create) {
        uint64_t pdpt_phys = mm_allocate_physical_page();
        if (!pdpt_phys) {
            return NULL;
        }
        mm_memset((void*)pdpt_phys, 0, PAGE_SIZE);
        mm_state.pml4_table[pml4_index] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pdpt = (uint64_t*)pdpt_phys;
    }
    if (!pdpt) {
        return NULL;
    }
    
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_index] & ~0xFFF);
    if (!pd && create) {
        uint64_t pd_phys = mm_allocate_physical_page();
        if (!pd_phys) {
            return NULL;
        }
        mm_memset((void*)pd_phys, 0, PAGE_SIZE);
        pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pd = (uint64_t*)pd_phys;
    }
    if (!pd) {
        return NULL;
    }
    
    uint64_t* pt = (uint64_t*)(pd[pd_index] & ~0xFFF);
    if (!pt && create) {
        uint64_t pt_phys = mm_allocate_physical_page();
        if (!pt_phys) {
            return NULL;
        }
        mm_memset((void*)pt_phys, 0, PAGE_SIZE);
        pd[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pt = (uint64_t*)pt_phys;
    }

    return pt ? &pt[pt_index] : NULL;
}

// Initialize virtual memory manager
void mm_initialize_virtual_memory(void) {
    kprintf("Initializing Virtual Memory Manager...\n");
    
    mm_state.pml4_table = (uint64_t*)(get_cr3() & ~0xFFF);
    uint64_t heap_start = mm_get_kernel_heap_start();
    mm_state.next_virtual_addr = heap_start + KERNEL_HEAP_SIZE + mm_state.bitmap_size;
    mm_state.next_virtual_addr = PAGE_ALIGN(mm_state.next_virtual_addr);
    
    kprintf("  Page tables at: %p\n", mm_state.pml4_table);
    kprintf("  Next virtual address: %p\n", (void*)mm_state.next_virtual_addr);
    
    kprintf("Virtual Memory Manager initialized\n");
}

// Map virtual page to physical page
bool mm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t* pte = mm_get_page_table(virtual_addr, true);
    if (!pte) {
        return false;
    }
    
    *pte = (physical_addr & ~0xFFF) | flags;
    mm_flush_tlb(virtual_addr);
    return true;
}

// Unmap virtual page
void mm_unmap_page(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        *pte = 0;
        mm_flush_tlb(virtual_addr);
    }
}

// Get physical address for virtual address
uint64_t mm_get_physical_address(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        return (*pte & ~0xFFF) | (virtual_addr & 0xFFF);
    }
    return 0;
}

// Check if page is mapped
bool mm_is_page_mapped(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    return pte && (*pte & PAGE_PRESENT);
}

// KERNEL HEAP ALLOCATOR IMPLEMENTATION

// Initialize heap
void mm_initialize_heap(void) {
    kprintf("Initializing Kernel Heap Allocator...\n");

    uint64_t heap_start = mm_get_kernel_heap_start();
    mm_state.heap_start = (heap_block_t*) heap_start;
    mm_state.heap_size = KERNEL_HEAP_SIZE;
    mm_state.heap_end = (heap_block_t*)(heap_start + KERNEL_HEAP_SIZE);    
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
void mm_get_memory_stats(memory_stats_t* stats) {
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
void mm_print_memory_stats(void) {
    memory_stats_t stats;
    mm_get_memory_stats(&stats);
    
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
bool mm_validate_heap(void) {
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
void mm_print_heap_stats(void) {
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
void mm_detect_memory(void) {
    // For now, we assume 256MB minimum as specified
    // In a real implementation, this would query BIOS memory map
    kprintf("Memory detection: Assuming 256MB minimum requirement\n");
}

// ============================================================================
// USER ADDRESS SPACE MANAGEMENT
// ============================================================================

// Get page table entry from a specific PML4, creating intermediate tables if needed
uint64_t* mm_get_page_table_from_pml4(uint64_t* pml4, uint64_t virtual_addr, bool create) {
    if (!pml4) {
        return NULL;
    }
    
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    
    bool is_user_space = (virtual_addr < KERNEL_OFFSET);
    
    // Get PDPT
    uint64_t* pdpt;
    if (!(pml4[pml4_index] & PAGE_PRESENT)) {
        if (!create) return NULL;
        uint64_t pdpt_phys = mm_allocate_physical_page();
        if (!pdpt_phys) return NULL;
        mm_memset((void*)pdpt_phys, 0, PAGE_SIZE);
        // For user space, set PAGE_USER on all levels
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            flags |= PAGE_USER;
        }
        pml4[pml4_index] = pdpt_phys | flags;
        pdpt = (uint64_t*)pdpt_phys;
    } else {
        // Entry exists - but for user space mapping, we may need to add PAGE_USER
        if (is_user_space && create && !(pml4[pml4_index] & PAGE_USER)) {
            pml4[pml4_index] |= PAGE_USER;
        }
        pdpt = (uint64_t*)(pml4[pml4_index] & ~0xFFFULL);
    }
    
    // Get PD
    uint64_t* pd;
    if (!(pdpt[pdpt_index] & PAGE_PRESENT)) {
        if (!create) return NULL;
        uint64_t pd_phys = mm_allocate_physical_page();
        if (!pd_phys) return NULL;
        mm_memset((void*)pd_phys, 0, PAGE_SIZE);
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            flags |= PAGE_USER;
        }
        pdpt[pdpt_index] = pd_phys | flags;
        pd = (uint64_t*)pd_phys;
    } else {
        // Entry exists - but for user space mapping, we may need to add PAGE_USER
        if (is_user_space && create && !(pdpt[pdpt_index] & PAGE_USER)) {
            pdpt[pdpt_index] |= PAGE_USER;
        }
        pd = (uint64_t*)(pdpt[pdpt_index] & ~0xFFFULL);
    }
    
    // Get PT
    uint64_t* pt;
    uint64_t pd_entry = pd[pd_index];
    
    // Check if this is a 2MB large page (PS bit set)
    if ((pd_entry & PAGE_PRESENT) && (pd_entry & 0x80)) {
        // This is a 2MB page - we need to split it into 4KB pages
        if (!create) return NULL;
        
        // Get the base physical address of the 2MB page
        uint64_t large_page_base = pd_entry & ~0x1FFFFFULL;  // Mask off lower 21 bits
        uint64_t old_flags = pd_entry & 0x1FFFFFULL;        // Keep old flags
        
        // Allocate a new page table
        uint64_t pt_phys = mm_allocate_physical_page();
        if (!pt_phys) return NULL;
        
        pt = (uint64_t*)pt_phys;
        
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
        if (!create) return NULL;
        uint64_t pt_phys = mm_allocate_physical_page();
        if (!pt_phys) return NULL;
        mm_memset((void*)pt_phys, 0, PAGE_SIZE);
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            flags |= PAGE_USER;
        }
        pd[pd_index] = pt_phys | flags;
        pt = (uint64_t*)pt_phys;
    } else {
        // Entry exists - but for user space mapping, we may need to add PAGE_USER
        if (is_user_space && create && !(pd[pd_index] & PAGE_USER)) {
            pd[pd_index] |= PAGE_USER;
        }
        pt = (uint64_t*)(pd[pd_index] & ~0xFFFULL);
    }
    
    return &pt[pt_index];
}

// Create a new user address space (PML4)
// Shares kernel higher-half mappings (PML4[511]) with current address space
// Also shares identity mapping (PML4[0-3]) for kernel stack access during interrupts
uint64_t* mm_create_user_address_space(void) {
    // Allocate PML4
    uint64_t pml4_phys = mm_allocate_physical_page();
    if (!pml4_phys) {
        kprintf("mm_create_user_address_space: Failed to allocate PML4\n");
        return NULL;
    }
    
    uint64_t* new_pml4 = (uint64_t*)pml4_phys;
    mm_memset(new_pml4, 0, PAGE_SIZE);
    
    uint64_t* current_pml4 = (uint64_t*)(get_cr3() & ~0xFFFULL);
    
    // We need identity mapping (PML4[0-3]) for kernel stack access when running
    // with user's CR3. However, we can't just share the kernel's page tables
    // because mapping user pages would corrupt them.
    //
    // Solution: Deep copy the page table hierarchy for PML4[0] (covers 0-512GB).
    // This allows the kernel to access low memory while user mappings get their
    // own page tables.
    //
    // For PML4[0], we copy the PDPT, and for each 1GB region (PDPT entries 0-3),
    // we copy the PD. This way user mappings in 0-4GB get their own PD/PT.
    
    if (current_pml4[0] & PAGE_PRESENT) {
        // Allocate new PDPT for user's address space
        uint64_t new_pdpt_phys = mm_allocate_physical_page();
        if (!new_pdpt_phys) {
            mm_free_physical_page(pml4_phys);
            return NULL;
        }
        uint64_t* new_pdpt = (uint64_t*)new_pdpt_phys;
        
        uint64_t* kernel_pdpt = (uint64_t*)(current_pml4[0] & ~0xFFFULL);
        
        // Copy all 512 PDPT entries initially (preserves identity mapping structure)
        mm_memcpy(new_pdpt, kernel_pdpt, PAGE_SIZE);
        
        // For the first 4 GB (PDPT entries 0-3), deep copy the PD structures
        // so user page mappings don't corrupt kernel's identity mapping
        for (int i = 0; i < 4; i++) {
            if (kernel_pdpt[i] & PAGE_PRESENT) {
                // Allocate new PD for this 1GB region
                uint64_t new_pd_phys = mm_allocate_physical_page();
                if (!new_pd_phys) {
                    // Cleanup on failure - simplified, just fail
                    kprintf("mm_create_user_address_space: Failed to allocate PD[%d]\n", i);
                    mm_free_physical_page(new_pdpt_phys);
                    mm_free_physical_page(pml4_phys);
                    return NULL;
                }
                uint64_t* new_pd = (uint64_t*)new_pd_phys;
                uint64_t* kernel_pd = (uint64_t*)(kernel_pdpt[i] & ~0xFFFULL);
                
                // Copy PD entries (these are 2MB pages with PS bit, or pointers to PTs)
                mm_memcpy(new_pd, kernel_pd, PAGE_SIZE);
                
                // Update PDPT to point to new PD (keep same flags)
                uint64_t flags = kernel_pdpt[i] & 0xFFFULL;
                new_pdpt[i] = new_pd_phys | flags;
            }
        }
        
        // Update PML4 to point to new PDPT (keep same flags, supervisor-only)
        uint64_t pml4_flags = current_pml4[0] & 0xFFFULL;
        new_pml4[0] = new_pdpt_phys | pml4_flags;
    }
    
    // Copy PML4[1-3] directly (these are less likely to have user mappings)
    // If user code is placed above 512GB this would need similar treatment
    for (int i = 1; i < 4; i++) {
        if (current_pml4[i] & PAGE_PRESENT) {
            new_pml4[i] = current_pml4[i];
        }
    }
    
    // Copy PML4[511] - kernel higher-half mapping (supervisor only)
    // This allows kernel code to run while in user's address space
    // It also provides access to kernel heap, stacks, etc.
    new_pml4[511] = current_pml4[511];
    
    return new_pml4;
}

// Destroy an address space and free all user pages
void mm_destroy_address_space(uint64_t* pml4) {
    if (!pml4) return;
    
    // Don't free if it's the kernel PML4
    uint64_t* kernel_pml4 = (uint64_t*)(get_cr3() & ~0xFFFULL);
    if (pml4 == kernel_pml4) {
        kprintf("mm_destroy_address_space: Cannot destroy kernel PML4!\n");
        return;
    }
    
    // Free user-space page tables (entries 0-510, excluding entry 0 identity mapping)
    for (int i = 1; i < 511; i++) {
        if (pml4[i] & PAGE_PRESENT) {
            uint64_t* pdpt = (uint64_t*)(pml4[i] & ~0xFFFULL);
            
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & PAGE_PRESENT) {
                    uint64_t* pd = (uint64_t*)(pdpt[j] & ~0xFFFULL);
                    
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & PAGE_PRESENT) {
                            // Check if it's a 2MB page or a page table
                            if (pd[k] & PAGE_SIZE_FLAG) {
                                // 2MB page - don't free, these are identity-mapped
                            } else {
                                uint64_t* pt = (uint64_t*)(pd[k] & ~0xFFFULL);
                                
                                // Free all physical pages in this PT
                                for (int l = 0; l < 512; l++) {
                                    if (pt[l] & PAGE_PRESENT) {
                                        uint64_t phys = pt[l] & ~0xFFFULL;
                                        mm_free_physical_page(phys);
                                    }
                                }
                                
                                // Free the PT itself
                                mm_free_physical_page((uint64_t)pt);
                            }
                        }
                    }
                    
                    // Free the PD
                    mm_free_physical_page((uint64_t)pd);
                }
            }
            
            // Free the PDPT
            mm_free_physical_page((uint64_t)pdpt);
        }
    }
    
    // Free the PML4 itself
    mm_free_physical_page((uint64_t)pml4);
}

// Switch to a different address space
void mm_switch_address_space(uint64_t* pml4) {
    if (pml4) {
        set_cr3((uint64_t)pml4);
    }
}

// Get the current address space
uint64_t* mm_get_current_address_space(void) {
    return (uint64_t*)(get_cr3() & ~0xFFFULL);
}

// Map a page in a specific address space
bool mm_map_page_in_address_space(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t* pte = mm_get_page_table_from_pml4(pml4, virtual_addr, true);
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
bool mm_map_user_page(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    // Ensure user bit is set for user-space addresses
    if (virtual_addr < KERNEL_OFFSET) {
        flags |= PAGE_USER;
    }
    return mm_map_page_in_address_space(pml4, virtual_addr, physical_addr, flags);
}

// Map a user stack region
bool mm_map_user_stack(uint64_t* pml4, uint64_t stack_top, size_t stack_size) {
    if (!pml4 || stack_size == 0) {
        return false;
    }
    
    size_t pages = (stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t stack_bottom = stack_top - stack_size;
    
    for (size_t i = 0; i < pages; i++) {
        uint64_t vaddr = stack_bottom + (i * PAGE_SIZE);
        uint64_t phys = mm_allocate_physical_page();
        
        if (!phys) {
            kprintf("mm_map_user_stack: Failed to allocate physical page %zu\n", i);
            // TODO: Unmap already-mapped pages
            return false;
        }
        
        // Zero the stack page
        mm_memset((void*)phys, 0, PAGE_SIZE);
        
        // Map with user, writable flags (NX disabled until EFER.NXE is set)
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        if (!mm_map_page_in_address_space(pml4, vaddr, phys, flags)) {
            kprintf("mm_map_user_stack: Failed to map page at %p\n", (void*)vaddr);
            mm_free_physical_page(phys);
            return false;
        }
    }
    
    return true;
}

// Get physical address from a specific PML4
uint64_t mm_get_physical_address_from_pml4(uint64_t* pml4, uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table_from_pml4(pml4, virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        return (*pte & ~0xFFFULL) | (virtual_addr & 0xFFF);
    }
    return 0;
}

// Get page flags for a virtual address
uint64_t mm_get_page_flags(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte) {
        return *pte & 0xFFF;  // Return just the flag bits
    }
    return 0;
}

// Set page flags for a virtual address
bool mm_set_page_flags(uint64_t virtual_addr, uint64_t flags) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        uint64_t phys = *pte & ~0xFFFULL;
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
bool mm_mark_page_cow(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (!pte || !(*pte & PAGE_PRESENT)) {
        return false;
    }
    
    // Remove writable, add COW marker
    *pte = (*pte & ~PAGE_WRITABLE) | PAGE_COW;
    mm_flush_tlb(virtual_addr);
    return true;
}

// Handle a COW page fault - allocate new page and copy contents
bool mm_handle_cow_fault(uint64_t fault_addr) {
    uint64_t page_addr = fault_addr & ~0xFFFULL;
    uint64_t* pte = mm_get_page_table(page_addr, false);
    
    if (!pte || !(*pte & PAGE_PRESENT)) {
        return false;
    }
    
    // Check if this is a COW page
    if (!(*pte & PAGE_COW)) {
        return false;  // Not a COW fault
    }
    
    uint64_t old_phys = *pte & ~0xFFFULL;
    
    // Allocate a new physical page
    uint64_t new_phys = mm_allocate_physical_page();
    if (!new_phys) {
        kprintf("mm_handle_cow_fault: Failed to allocate new page\n");
        return false;
    }
    
    // Copy contents from old page to new page
    mm_memcpy((void*)new_phys, (void*)old_phys, PAGE_SIZE);
    
    // Update PTE: remove COW, add writable, point to new page
    uint64_t flags = (*pte & 0xFFF) & ~PAGE_COW;
    flags |= PAGE_WRITABLE;
    *pte = new_phys | flags;
    
    mm_flush_tlb(page_addr);
    
    // Note: In a real implementation, we'd track reference counts
    // and only free the old page when refcount reaches 0
    
    return true;
}

// Clone an address space for fork() - uses COW for efficiency
uint64_t* mm_clone_address_space(uint64_t* src_pml4) {
    if (!src_pml4) {
        return NULL;
    }
    
    // Create new address space
    uint64_t* new_pml4 = mm_create_user_address_space();
    if (!new_pml4) {
        return NULL;
    }
    
    // Clone user-space mappings with COW
    // For now, we only support entries 1-510 (skip identity mapping at 0 and kernel at 511)
    for (int i = 1; i < 511; i++) {
        if (src_pml4[i] & PAGE_PRESENT) {
            uint64_t* src_pdpt = (uint64_t*)(src_pml4[i] & ~0xFFFULL);
            
            // Allocate PDPT for new address space
            uint64_t pdpt_phys = mm_allocate_physical_page();
            if (!pdpt_phys) goto fail;
            mm_memset((void*)pdpt_phys, 0, PAGE_SIZE);
            new_pml4[i] = pdpt_phys | (src_pml4[i] & 0xFFF);
            uint64_t* new_pdpt = (uint64_t*)pdpt_phys;
            
            for (int j = 0; j < 512; j++) {
                if (src_pdpt[j] & PAGE_PRESENT) {
                    uint64_t* src_pd = (uint64_t*)(src_pdpt[j] & ~0xFFFULL);
                    
                    uint64_t pd_phys = mm_allocate_physical_page();
                    if (!pd_phys) goto fail;
                    mm_memset((void*)pd_phys, 0, PAGE_SIZE);
                    new_pdpt[j] = pd_phys | (src_pdpt[j] & 0xFFF);
                    uint64_t* new_pd = (uint64_t*)pd_phys;
                    
                    for (int k = 0; k < 512; k++) {
                        if (src_pd[k] & PAGE_PRESENT) {
                            if (src_pd[k] & PAGE_SIZE_FLAG) {
                                // 2MB huge page - just share for now
                                new_pd[k] = src_pd[k];
                            } else {
                                uint64_t* src_pt = (uint64_t*)(src_pd[k] & ~0xFFFULL);
                                
                                uint64_t pt_phys = mm_allocate_physical_page();
                                if (!pt_phys) goto fail;
                                mm_memset((void*)pt_phys, 0, PAGE_SIZE);
                                new_pd[k] = pt_phys | (src_pd[k] & 0xFFF);
                                uint64_t* new_pt = (uint64_t*)pt_phys;
                                
                                for (int l = 0; l < 512; l++) {
                                    if (src_pt[l] & PAGE_PRESENT) {
                                        // Mark both source and dest as COW
                                        uint64_t cow_flags = (src_pt[l] & ~PAGE_WRITABLE) | PAGE_COW;
                                        src_pt[l] = cow_flags;
                                        new_pt[l] = cow_flags;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Flush TLB for source address space
    mm_flush_all_tlb();
    
    return new_pml4;
    
fail:
    mm_destroy_address_space(new_pml4);
    return NULL;
}

// ============================================================================
// SYSCALL/SYSRET CONFIGURATION
// ============================================================================

// MSR definitions for SYSCALL/SYSRET
#define MSR_STAR        0xC0000081  // Segment selectors for SYSCALL
#define MSR_LSTAR       0xC0000082  // RIP for SYSCALL (64-bit)
#define MSR_CSTAR       0xC0000083  // RIP for SYSCALL (compat mode)
#define MSR_SFMASK      0xC0000084  // RFLAGS mask for SYSCALL
#define MSR_EFER        0xC0000080  // Extended Feature Enable Register

// Read MSR
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Syscall entry point - defined in syscall.asm
extern void syscall_entry(void);

// Initialize SYSCALL/SYSRET
void mm_initialize_syscall(void) {
    // Enable SCE (System Call Extensions) in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1;  // Set SCE bit
    wrmsr(MSR_EFER, efer);
    
    // Set up STAR register
    // Bits 63:48 = User CS/SS base selector (user CS = base + 16, user SS = base + 8)
    // Bits 47:32 = Kernel CS/SS base selector (kernel CS = base, kernel SS = base + 8)
    // GDT layout: 0=null, 1=kernel code, 2=kernel data, 3=user code, 4=user data
    // Kernel CS = 0x08, Kernel SS = 0x10
    // User CS = 0x18 | 3 = 0x1B, User SS = 0x20 | 3 = 0x23
    // But SYSRET adds 16 to get user CS and adds 8 to get user SS
    // So user selector base should be 0x18 - 16 = 0x08... no wait
    // SYSRET: CS = STAR[63:48] + 16, SS = STAR[63:48] + 8
    // We want CS = 0x1B (user code with RPL 3), SS = 0x23 (user data with RPL 3)
    // But SYSRET ORs with RPL 3, so we just need base = 0x10
    // CS = 0x10 + 16 = 0x20 | 3 = 0x23... that's wrong
    // Actually for 64-bit: CS = STAR[63:48] + 16 | 3, SS = STAR[63:48] + 8 | 3
    // We want user CS = 0x1B (selector 0x18 | RPL 3), user SS = 0x23 (selector 0x20 | RPL 3)
    // 0x1B = 0x18 | 3, so base + 16 = 0x18, base = 0x08
    // But 0x08 + 8 = 0x10 | 3 = 0x13, not 0x23
    // The GDT layout needs user data BEFORE user code for SYSRET to work correctly
    // Or we accept different selector values
    // Actually for AMD64 SYSRET: SS = STAR[63:48] + 8 | 3, CS = STAR[63:48] + 16 | 3
    // Current GDT: 0=null, 1=kcode, 2=kdata, 3=ucode, 4=udata
    // Selectors: 0x00, 0x08, 0x10, 0x18, 0x20
    // For SYSRET with base = 0x10: SS = 0x18|3=0x1B, CS = 0x20|3=0x23
    // That means user code = 0x23, user data = 0x1B, which is reversed!
    // 
    // The Intel/AMD way is: GDT order should be kcode, kdata, udata, ucode (32-bit), ucode (64-bit)
    // For simplicity now, we'll use STAR[63:48] = 0x10
    // This gives SS = 0x1B (will use GDT[3] as user data)
    //            CS = 0x23 (will use GDT[4] as user code)
    // We'll need to swap GDT entries 3 and 4 for correct SYSRET behavior
    // For now, just set it up - the GDT can be fixed later
    
    uint64_t star = 0;
    star |= ((uint64_t)0x10 << 48);  // User selector base (CS = 0x23, SS = 0x1B after SYSRET)
    star |= ((uint64_t)0x08 << 32);  // Kernel selector base (CS = 0x08, SS = 0x10)
    wrmsr(MSR_STAR, star);
    
    // Set LSTAR to syscall entry point
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    
    // Set CSTAR for compatibility mode (not used in 64-bit only kernel)
    wrmsr(MSR_CSTAR, (uint64_t)syscall_entry);
    
    // Set SFMASK - RFLAGS bits to clear on SYSCALL
    // Clear TF (no single-step) and DF (clear direction), keep IF enabled
    wrmsr(MSR_SFMASK, 0x100 | 0x400);  // TF | DF
    
    kprintf("SYSCALL/SYSRET initialized\n");
    kprintf("  STAR = 0x%016llx\n", star);
    kprintf("  LSTAR = 0x%016llx\n", (uint64_t)syscall_entry);
}
