// LikeOS-64 SLAB Allocator Implementation
// Dynamic kernel heap using size-class caches for efficient allocation
// Maps physical pages to kernel virtual address space

#include "../../include/kernel/slab.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"

// External debug flag from memory.c
extern int mm_debug_pt;

// Size classes: 32, 64, 128, 256, 512, 1024, 2048 bytes
// Note: 4096 cannot fit in a single page with slab header, so it goes to large alloc
static const uint32_t size_classes[SLAB_NUM_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

// Slab caches for each size class
static slab_cache_t slab_caches[SLAB_NUM_CLASSES];

// Global statistics
static slab_stats_t slab_global_stats = {0};

// SLAB allocator initialized flag
static bool slab_initialized = false;

// Virtual address allocator for SLAB pages
// We use a range starting after the kernel heap area
// SLAB virtual space: 0xFFFFFFFF88000000 - 0xFFFFFFFF90000000 (128MB)
#define SLAB_VIRT_BASE      0xFFFFFFFF88000000ULL
#define SLAB_VIRT_END       0xFFFFFFFF90000000ULL
static uint64_t slab_next_virt_addr = SLAB_VIRT_BASE;

// Free virtual address range tracking for large allocations
// Linked list of freed ranges that can be reused (with coalescing)
#define SLAB_MAX_FREE_RANGES 1024
typedef struct free_virt_range {
    uint64_t start;
    uint64_t size;  // in bytes
} free_virt_range_t;

static free_virt_range_t slab_free_ranges[SLAB_MAX_FREE_RANGES];
static int slab_num_free_ranges = 0;

// Try to allocate from freed virtual ranges first
static uint64_t slab_alloc_virt_range(size_t size) {
    // Look for an exact or larger fit in free ranges
    for (int i = 0; i < slab_num_free_ranges; i++) {
        if (slab_free_ranges[i].size >= size) {
            uint64_t addr = slab_free_ranges[i].start;
            if (slab_free_ranges[i].size == size) {
                // Exact fit - remove this range
                slab_free_ranges[i] = slab_free_ranges[slab_num_free_ranges - 1];
                slab_num_free_ranges--;
            } else {
                // Split - shrink this range
                slab_free_ranges[i].start += size;
                slab_free_ranges[i].size -= size;
            }
            return addr;
        }
    }
    return 0;  // No suitable range found
}

// Add a freed virtual range to the free list (with coalescing)
static void slab_free_virt_range(uint64_t start, size_t size) {
    // Try to coalesce with an existing range
    for (int i = 0; i < slab_num_free_ranges; i++) {
        // Check if this range is immediately after an existing range
        if (slab_free_ranges[i].start + slab_free_ranges[i].size == start) {
            slab_free_ranges[i].size += size;
            
            // Check if we can also coalesce with the next range
            for (int j = 0; j < slab_num_free_ranges; j++) {
                if (j != i && slab_free_ranges[j].start == 
                    slab_free_ranges[i].start + slab_free_ranges[i].size) {
                    slab_free_ranges[i].size += slab_free_ranges[j].size;
                    slab_free_ranges[j] = slab_free_ranges[slab_num_free_ranges - 1];
                    slab_num_free_ranges--;
                    break;
                }
            }
            return;
        }
        // Check if this range is immediately before an existing range
        if (start + size == slab_free_ranges[i].start) {
            slab_free_ranges[i].start = start;
            slab_free_ranges[i].size += size;
            return;
        }
    }
    
    // No coalescing possible - add as new range
    if (slab_num_free_ranges >= SLAB_MAX_FREE_RANGES) {
        // Free list is full, just lose this range (unfortunate but safe)
        static int warned = 0;
        if (!warned) {
            kprintf("SLAB: WARNING - free range list full, leaking virtual address space\n");
            warned = 1;
        }
        return;
    }
    slab_free_ranges[slab_num_free_ranges].start = start;
    slab_free_ranges[slab_num_free_ranges].size = size;
    slab_num_free_ranges++;
}

// Allocate a virtual address for a slab page (single page)
static uint64_t slab_alloc_virt_addr(void) {
    // First try free list
    uint64_t addr = slab_alloc_virt_range(PAGE_SIZE);
    if (addr) {
        return addr;
    }
    
    if (slab_next_virt_addr >= SLAB_VIRT_END) {
        kprintf("SLAB: Virtual address space exhausted!\n");
        return 0;
    }
    addr = slab_next_virt_addr;
    slab_next_virt_addr += PAGE_SIZE;
    return addr;
}

// Free a single slab page's virtual address
static void slab_free_virt_addr(uint64_t addr) {
    slab_free_virt_range(addr, PAGE_SIZE);
}

// ============================================================================
// Helper Functions
// ============================================================================

// Get size class index for a given size (returns -1 if too large for slab)
int slab_get_size_class(size_t size) {
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (size <= size_classes[i]) {
            return i;
        }
    }
    return -1;  // Too large for slab allocator
}

// Calculate how many objects fit in a slab page for given object size
static uint32_t calc_objects_per_slab(uint32_t object_size) {
    // Available space = PAGE_SIZE - sizeof(slab_page_t)
    uint32_t available = PAGE_SIZE - sizeof(slab_page_t);
    return available / object_size;
}

// Find first free bit in bitmap, returns object index or -1 if none
static int bitmap_find_free(uint64_t* bitmap, uint32_t total_objects) {
    uint32_t words = (total_objects + 63) / 64;
    
    for (uint32_t i = 0; i < words && i < 8; i++) {
        if (bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
            // Find first zero bit
            uint64_t word = bitmap[i];
            for (int bit = 0; bit < 64; bit++) {
                uint32_t obj_idx = i * 64 + bit;
                if (obj_idx >= total_objects) {
                    return -1;
                }
                if (!(word & (1ULL << bit))) {
                    return obj_idx;
                }
            }
        }
    }
    return -1;
}

// Set bit in bitmap (mark allocated)
static void bitmap_set(uint64_t* bitmap, uint32_t index) {
    uint32_t word = index / 64;
    uint32_t bit = index % 64;
    if (word < 8) {
        bitmap[word] |= (1ULL << bit);
    }
}

// Clear bit in bitmap (mark free)
static void bitmap_clear(uint64_t* bitmap, uint32_t index) {
    uint32_t word = index / 64;
    uint32_t bit = index % 64;
    if (word < 8) {
        bitmap[word] &= ~(1ULL << bit);
    }
}

// Check if bit is set
static bool bitmap_is_set(uint64_t* bitmap, uint32_t index) {
    uint32_t word = index / 64;
    uint32_t bit = index % 64;
    if (word < 8) {
        return (bitmap[word] & (1ULL << bit)) != 0;
    }
    return true;  // Assume allocated if out of range
}

// Get object pointer from slab page and index
static void* slab_get_object(slab_page_t* slab, uint32_t index) {
    uint8_t* base = (uint8_t*)slab + sizeof(slab_page_t);
    return base + (index * slab->object_size);
}

// Get object index from pointer (returns -1 if invalid)
static int slab_get_object_index(slab_page_t* slab, void* ptr) {
    uint8_t* base = (uint8_t*)slab + sizeof(slab_page_t);
    uint8_t* obj = (uint8_t*)ptr;
    
    if (obj < base) {
        return -1;
    }
    
    uint64_t offset = obj - base;
    uint32_t index = offset / slab->object_size;
    
    // Verify alignment
    if (offset % slab->object_size != 0) {
        return -1;
    }
    
    if (index >= slab->total_objects) {
        return -1;
    }
    
    return index;
}

// ============================================================================
// Slab Page Management
// ============================================================================

// Allocate a new slab page for a cache
static slab_page_t* slab_alloc_page(slab_cache_t* cache) {
    // Allocate a physical page
    uint64_t phys_page = mm_allocate_physical_page();
    if (phys_page == 0) {
        kprintf("SLAB: Failed to allocate physical page for cache size %u\n", 
                cache->object_size);
        return NULL;
    }
    
    // Allocate a virtual address for this page
    uint64_t virt_addr = slab_alloc_virt_addr();
    if (virt_addr == 0) {
        mm_free_physical_page(phys_page);
        return NULL;
    }
    
    // Map physical page to virtual address
    if (!mm_map_page(virt_addr, phys_page, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE)) {
        kprintf("SLAB: Failed to map page phys=%p to virt=%p\n", 
                (void*)phys_page, (void*)virt_addr);
        mm_free_physical_page(phys_page);
        return NULL;
    }
    
    // Verify mapping before memset
    uint64_t check_phys = mm_get_physical_address(virt_addr);
    if (check_phys != phys_page) {
        kprintf("SLAB: MAPPING FAILED! virt=%p expected phys=%p got=%p\n",
                (void*)virt_addr, (void*)phys_page, (void*)check_phys);
    }
    
    // Initialize slab page header (now using virtual address)
    slab_page_t* slab = (slab_page_t*)virt_addr;
    mm_memset(slab, 0, PAGE_SIZE);
    
    slab->magic = SLAB_MAGIC;
    slab->object_size = cache->object_size;
    slab->total_objects = cache->objects_per_slab;
    slab->free_count = slab->total_objects;
    slab->cache = cache;
    slab->next = NULL;
    slab->prev = NULL;
    slab->phys_addr = phys_page;  // Store physical address for later unmapping
    
    // All objects start as free (bitmap = 0)
    
    cache->slab_count++;
    slab_global_stats.total_pages_used++;
    
    return slab;
}

// Free a slab page back to physical memory
static void slab_free_page(slab_page_t* slab) {
    if (!slab || slab->magic != SLAB_MAGIC) {
        kprintf("SLAB: Invalid slab page in free_page: %p\n", slab);
        return;
    }
    
    slab_cache_t* cache = slab->cache;
    if (cache) {
        cache->slab_count--;
    }
    
    slab_global_stats.total_pages_used--;
    
    // Get physical address before we unmap
    uint64_t phys_addr = slab->phys_addr;
    uint64_t virt_addr = (uint64_t)slab;
    
    // Unmap the virtual address
    mm_unmap_page(virt_addr);
    
    // Free the physical page
    mm_free_physical_page(phys_addr);
    
    // Reclaim the virtual address for reuse
    slab_free_virt_addr(virt_addr);
}

// Move slab between lists (partial <-> full <-> empty)
static void slab_move_to_list(slab_page_t* slab, slab_page_t** from_list, 
                               slab_page_t** to_list) {
    // Remove from current list
    if (slab->prev) {
        slab->prev->next = slab->next;
    } else if (from_list && *from_list == slab) {
        *from_list = slab->next;
    }
    
    if (slab->next) {
        slab->next->prev = slab->prev;
    }
    
    // Add to new list (at head)
    slab->prev = NULL;
    slab->next = *to_list;
    if (*to_list) {
        (*to_list)->prev = slab;
    }
    *to_list = slab;
}

// ============================================================================
// Core SLAB API Implementation
// ============================================================================

// Initialize the SLAB allocator
void slab_init(void) {
    kprintf("Initializing SLAB allocator...\n");
    
    // Initialize all size-class caches
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        slab_cache_t* cache = &slab_caches[i];
        
        cache->object_size = size_classes[i];
        cache->objects_per_slab = calc_objects_per_slab(size_classes[i]);
        cache->partial_slabs = NULL;
        cache->full_slabs = NULL;
        cache->empty_slabs = NULL;
        cache->total_allocs = 0;
        cache->total_frees = 0;
        cache->slab_count = 0;
        cache->empty_slab_count = 0;
        
        // Ensure we have at least 1 object per slab
        if (cache->objects_per_slab == 0) {
            cache->objects_per_slab = 1;
        }
        
        // Cap at 512 objects (bitmap size limit)
        if (cache->objects_per_slab > 512) {
            cache->objects_per_slab = 512;
        }
    }
    
    // Reset global statistics
    mm_memset(&slab_global_stats, 0, sizeof(slab_global_stats));
    
    slab_initialized = true;
    
    kprintf("  Size classes: ");
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        kprintf("%u ", size_classes[i]);
    }
    kprintf("\n");
    kprintf("  SLAB allocator ready (dynamic heap growth enabled)\n");
}

// Allocate memory from SLAB allocator
void* slab_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    if (!slab_initialized) {
        kprintf("SLAB: Allocator not initialized!\n");
        return NULL;
    }
    
    // For large allocations, use direct page allocation
    // Note: size >= SLAB_MAX_SIZE because max slab object (2048) needs room for header
    if (size > SLAB_MAX_SIZE) {
        // Calculate pages needed (including header)
        size_t total_size = size + sizeof(large_alloc_header_t);
        size_t page_count = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
        size_t alloc_bytes = page_count * PAGE_SIZE;
        
        uint64_t phys_pages = mm_allocate_contiguous_pages(page_count);
        if (phys_pages == 0) {
            kprintf("SLAB: Failed to allocate %lu pages for large allocation\n", 
                    (unsigned long)page_count);
            return NULL;
        }
        
        // Try to get virtual address from free list first
        uint64_t virt_base = slab_alloc_virt_range(alloc_bytes);
        if (!virt_base) {
            // Fall back to allocating new virtual address space
            if (slab_next_virt_addr + alloc_bytes > SLAB_VIRT_END) {
                kprintf("SLAB: Virtual address space exhausted for large alloc\n");
                kprintf("SLAB: requested=%lu pages, next_virt=0x%lx, end=0x%lx\n",
                        (unsigned long)page_count, slab_next_virt_addr, SLAB_VIRT_END);
                kprintf("SLAB: large_allocs=%lu, large_frees=%lu, active=%lu\n",
                        slab_global_stats.large_allocations, slab_global_stats.large_frees,
                        slab_global_stats.large_allocations - slab_global_stats.large_frees);
                kprintf("SLAB: free_ranges=%d (max=%d)\n", 
                        slab_num_free_ranges, SLAB_MAX_FREE_RANGES);
                mm_free_contiguous_pages(phys_pages, page_count);
                return NULL;
            }
            virt_base = slab_next_virt_addr;
            slab_next_virt_addr += alloc_bytes;
        }
        
        // Map all pages
        for (size_t i = 0; i < page_count; i++) {
            uint64_t vaddr = virt_base + (i * PAGE_SIZE);
            uint64_t paddr = phys_pages + (i * PAGE_SIZE);
            bool ok = mm_map_page(vaddr, paddr, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE);
            
            if (!ok) {
                kprintf("SLAB: Failed to map large alloc page %lu\n", (unsigned long)i);
                // Unmap what we mapped so far
                for (size_t j = 0; j < i; j++) {
                    mm_unmap_page(virt_base + (j * PAGE_SIZE));
                }
                mm_free_contiguous_pages(phys_pages, page_count);
                return NULL;
            }
        }
        
        // Set up header (now using virtual address)
        large_alloc_header_t* header = (large_alloc_header_t*)virt_base;
        header->magic = SLAB_LARGE_MAGIC;
        header->page_count = page_count;
        header->size = size;
        header->phys_addr = phys_pages;
        
        slab_global_stats.large_allocations++;
        slab_global_stats.total_allocations++;
        slab_global_stats.total_pages_used += page_count;
        
        return (uint8_t*)header + sizeof(large_alloc_header_t);
    }
    
    // Find appropriate size class
    int class_idx = slab_get_size_class(size);
    if (class_idx < 0) {
        kprintf("SLAB: No size class for size %lu\n", (unsigned long)size);
        return NULL;
    }
    
    slab_cache_t* cache = &slab_caches[class_idx];
    slab_page_t* slab = NULL;
    
    // Try to allocate from partial slabs first
    if (cache->partial_slabs) {
        slab = cache->partial_slabs;
        slab_global_stats.cache_hits++;
    }
    // Try empty slabs (cached for reuse)
    else if (cache->empty_slabs) {
        slab = cache->empty_slabs;
        slab_move_to_list(slab, &cache->empty_slabs, &cache->partial_slabs);
        cache->empty_slab_count--;
        slab_global_stats.cache_hits++;
    }
    // Need to allocate a new slab page
    else {
        slab = slab_alloc_page(cache);
        if (!slab) {
            return NULL;
        }
        // Add to partial list
        slab->next = cache->partial_slabs;
        if (cache->partial_slabs) {
            cache->partial_slabs->prev = slab;
        }
        cache->partial_slabs = slab;
        slab_global_stats.cache_misses++;
    }
    
    // Find free object in slab
    int obj_idx = bitmap_find_free(slab->bitmap, slab->total_objects);
    if (obj_idx < 0) {
        kprintf("SLAB: Corrupt slab - no free object but in partial list\n");
        return NULL;
    }
    
    // Mark object as allocated
    bitmap_set(slab->bitmap, obj_idx);
    slab->free_count--;
    
    // Move slab to full list if no more free objects
    if (slab->free_count == 0) {
        slab_move_to_list(slab, &cache->partial_slabs, &cache->full_slabs);
    }
    
    cache->total_allocs++;
    slab_global_stats.total_allocations++;
    
    return slab_get_object(slab, obj_idx);
}

// Free memory allocated by slab_alloc
void slab_free(void* ptr) {
    if (!ptr) {
        return;
    }
    
    if (!slab_initialized) {
        kprintf("SLAB: Free before init: %p\n", ptr);
        return;
    }
    
    // Check if this is a large allocation
    large_alloc_header_t* large_header = 
        (large_alloc_header_t*)((uint8_t*)ptr - sizeof(large_alloc_header_t));
    
    if (large_header->magic == SLAB_LARGE_MAGIC) {
        // Free large allocation
        size_t page_count = large_header->page_count;
        uint64_t phys_addr = large_header->phys_addr;
        uint64_t virt_addr = (uint64_t)large_header;
        size_t alloc_bytes = page_count * PAGE_SIZE;
        
        // Unmap all pages
        for (size_t i = 0; i < page_count; i++) {
            mm_unmap_page(virt_addr + (i * PAGE_SIZE));
        }
        
        // Free the physical pages
        mm_free_contiguous_pages(phys_addr, page_count);
        
        // Reclaim the virtual address space
        slab_free_virt_range(virt_addr, alloc_bytes);
        
        slab_global_stats.large_frees++;
        slab_global_stats.total_frees++;
        slab_global_stats.total_pages_used -= page_count;
        return;
    }
    
    // Validate pointer is in SLAB virtual address range
    uint64_t addr = (uint64_t)ptr;
    if (addr < SLAB_VIRT_BASE || addr >= slab_next_virt_addr) {
        void* ra = __builtin_return_address(0);
        kprintf("SLAB: Invalid free - ptr %p not in SLAB range [%p-%p] (caller=%p)\n", 
                ptr, (void*)SLAB_VIRT_BASE, (void*)slab_next_virt_addr, ra);
        return;
    }
    
    // Must be a slab allocation - find the slab page
    // Slab page is at the page-aligned address below the object
    uint64_t page_addr = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    slab_page_t* slab = (slab_page_t*)page_addr;
    
    // Validate slab
    if (slab->magic != SLAB_MAGIC) {
        void* ra = __builtin_return_address(0);
        kprintf("SLAB: Invalid free - bad magic 0x%x at %p (ptr=%p, caller=%p)\n", 
                slab->magic, slab, ptr, ra);
        return;
    }
    
    // Validate cache pointer is in kernel space
    slab_cache_t* cache = slab->cache;
    if (!cache || (uint64_t)cache < 0xFFFFFFFF80000000ULL) {
        kprintf("SLAB: Invalid free - bad cache %p for slab %p\n", cache, slab);
        return;
    }
    
    // Find object index
    int obj_idx = slab_get_object_index(slab, ptr);
    if (obj_idx < 0) {
        kprintf("SLAB: Invalid free - bad object pointer %p in slab %p\n", 
                ptr, slab);
        return;
    }
    
    // Check if already free (double-free detection)
    if (!bitmap_is_set(slab->bitmap, obj_idx)) {
        void* ra = __builtin_return_address(0);
        kprintf("SLAB: Double free detected at %p (caller=%p)\n", ptr, ra);
        return;
    }
    
    // Was this slab full?
    bool was_full = (slab->free_count == 0);
    
    // Mark object as free
    bitmap_clear(slab->bitmap, obj_idx);
    slab->free_count++;
    
    // Move slab between lists as needed
    if (was_full) {
        // Move from full to partial
        slab_move_to_list(slab, &cache->full_slabs, &cache->partial_slabs);
    } else if (slab->free_count == slab->total_objects) {
        // Slab is now completely empty
        slab_move_to_list(slab, &cache->partial_slabs, &cache->empty_slabs);
        cache->empty_slab_count++;
        
        // Optional: Free empty slabs if we have too many cached
        // Keep at most 2 empty slabs per cache to reduce memory pressure
        if (cache->empty_slab_count > 2) {
            slab_page_t* to_free = cache->empty_slabs;
            if (to_free) {
                cache->empty_slabs = to_free->next;
                if (cache->empty_slabs) {
                    cache->empty_slabs->prev = NULL;
                }
                cache->empty_slab_count--;
                slab_free_page(to_free);
            }
        }
    }
    
    cache->total_frees++;
    slab_global_stats.total_frees++;
}

// Reallocate memory
void* slab_realloc(void* ptr, size_t new_size) {
    if (!ptr) {
        return slab_alloc(new_size);
    }
    
    if (new_size == 0) {
        slab_free(ptr);
        return NULL;
    }
    
    // Get current size
    size_t old_size = 0;
    
    // Check if large allocation
    large_alloc_header_t* large_header = 
        (large_alloc_header_t*)((uint8_t*)ptr - sizeof(large_alloc_header_t));
    
    if (large_header->magic == SLAB_LARGE_MAGIC) {
        old_size = large_header->size;
    } else {
        // Slab allocation - size is the size class
        uint64_t page_addr = (uint64_t)ptr & ~(PAGE_SIZE - 1);
        slab_page_t* slab = (slab_page_t*)page_addr;
        
        if (slab->magic == SLAB_MAGIC) {
            old_size = slab->object_size;
        } else {
            kprintf("SLAB: realloc on invalid pointer %p\n", ptr);
            return NULL;
        }
    }
    
    // If new size fits in current allocation, return same pointer
    // (for slab allocations, check if same size class)
    if (new_size <= old_size) {
        int old_class = slab_get_size_class(old_size);
        int new_class = slab_get_size_class(new_size);
        if (old_class >= 0 && new_class >= 0 && old_class == new_class) {
            return ptr;
        }
        // For large allocations, only return if new_size > SLAB_MAX_SIZE too
        if (old_size > SLAB_MAX_SIZE && new_size > SLAB_MAX_SIZE) {
            // Could optimize by checking if fits in fewer pages
            return ptr;
        }
    }
    
    // Allocate new block and copy
    void* new_ptr = slab_alloc(new_size);
    if (new_ptr) {
        size_t copy_size = old_size < new_size ? old_size : new_size;
        mm_memcpy(new_ptr, ptr, copy_size);
        slab_free(ptr);
    }
    
    return new_ptr;
}

// Allocate and zero memory
void* slab_calloc(size_t count, size_t size) {
    size_t total_size = count * size;
    void* ptr = slab_alloc(total_size);
    if (ptr) {
        mm_memset(ptr, 0, total_size);
    }
    return ptr;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

// Get SLAB allocator statistics
void slab_get_stats(slab_stats_t* stats) {
    if (stats) {
        *stats = slab_global_stats;
    }
}

// Print SLAB allocator statistics
void slab_print_stats(void) {
    kprintf("\n=== SLAB Allocator Statistics ===\n");
    kprintf("Total allocations: %lu\n", slab_global_stats.total_allocations);
    kprintf("Total frees: %lu\n", slab_global_stats.total_frees);
    kprintf("Active allocations: %lu\n", 
            slab_global_stats.total_allocations - slab_global_stats.total_frees);
    kprintf("Large allocations: %lu (freed: %lu, active: %lu)\n", 
            slab_global_stats.large_allocations, slab_global_stats.large_frees,
            slab_global_stats.large_allocations - slab_global_stats.large_frees);
    kprintf("Total pages used: %lu (%lu KB)\n", 
            slab_global_stats.total_pages_used,
            slab_global_stats.total_pages_used * 4);
    kprintf("Cache hits: %lu, misses: %lu\n",
            slab_global_stats.cache_hits, slab_global_stats.cache_misses);
    
    // Virtual address space usage
    uint64_t virt_used = slab_next_virt_addr - SLAB_VIRT_BASE;
    uint64_t virt_total = SLAB_VIRT_END - SLAB_VIRT_BASE;
    kprintf("Virtual space: used=%lu KB / %lu KB (%lu%%), free_ranges=%d\n",
            virt_used / 1024, virt_total / 1024, 
            (virt_used * 100) / virt_total, slab_num_free_ranges);
    
    kprintf("\nPer-cache statistics:\n");
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        slab_cache_t* cache = &slab_caches[i];
        if (cache->total_allocs > 0 || cache->slab_count > 0) {
            kprintf("  %4u bytes: allocs=%lu frees=%lu slabs=%u empty=%u\n",
                    cache->object_size, cache->total_allocs, cache->total_frees,
                    cache->slab_count, cache->empty_slab_count);
        }
    }
    kprintf("=================================\n");
}

// Validate SLAB allocator integrity
int slab_validate(const char* caller) {
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        slab_cache_t* cache = &slab_caches[i];
        
        // Validate partial slabs
        for (slab_page_t* slab = cache->partial_slabs; slab; slab = slab->next) {
            if (slab->magic != SLAB_MAGIC) {
                kprintf("SLAB CORRUPT at %s: cache[%d] partial slab %p bad magic\n",
                        caller, i, slab);
                return -1;
            }
            if (slab->free_count == 0) {
                kprintf("SLAB CORRUPT at %s: cache[%d] partial slab %p has 0 free\n",
                        caller, i, slab);
                return -1;
            }
        }
        
        // Validate full slabs
        for (slab_page_t* slab = cache->full_slabs; slab; slab = slab->next) {
            if (slab->magic != SLAB_MAGIC) {
                kprintf("SLAB CORRUPT at %s: cache[%d] full slab %p bad magic\n",
                        caller, i, slab);
                return -1;
            }
            if (slab->free_count != 0) {
                kprintf("SLAB CORRUPT at %s: cache[%d] full slab %p has %u free\n",
                        caller, i, slab, slab->free_count);
                return -1;
            }
        }
        
        // Validate empty slabs
        for (slab_page_t* slab = cache->empty_slabs; slab; slab = slab->next) {
            if (slab->magic != SLAB_MAGIC) {
                kprintf("SLAB CORRUPT at %s: cache[%d] empty slab %p bad magic\n",
                        caller, i, slab);
                return -1;
            }
            if (slab->free_count != slab->total_objects) {
                kprintf("SLAB CORRUPT at %s: cache[%d] empty slab %p not empty\n",
                        caller, i, slab);
                return -1;
            }
        }
    }
    
    return 0;
}

// Shrink empty slabs to free memory
void slab_shrink(void) {
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        slab_cache_t* cache = &slab_caches[i];
        
        // Free all empty slabs except one
        while (cache->empty_slabs && cache->empty_slab_count > 1) {
            slab_page_t* slab = cache->empty_slabs;
            cache->empty_slabs = slab->next;
            if (cache->empty_slabs) {
                cache->empty_slabs->prev = NULL;
            }
            cache->empty_slab_count--;
            slab_free_page(slab);
        }
    }
}

// Check if a pointer is managed by SLAB allocator
bool slab_is_slab_ptr(void* ptr) {
    if (!ptr) return false;
    
    uint64_t page_addr = (uint64_t)ptr & ~(PAGE_SIZE - 1);
    slab_page_t* slab = (slab_page_t*)page_addr;
    
    return slab->magic == SLAB_MAGIC;
}

// Check if a pointer is a large allocation
bool slab_is_large_ptr(void* ptr) {
    if (!ptr) return false;
    
    large_alloc_header_t* header = 
        (large_alloc_header_t*)((uint8_t*)ptr - sizeof(large_alloc_header_t));
    
    return header->magic == SLAB_LARGE_MAGIC;
}
