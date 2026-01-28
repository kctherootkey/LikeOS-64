// LikeOS-64 SLAB Allocator
// Dynamic kernel heap using size-class caches for efficient allocation
// Uses identity-mapped physical pages, spinlock-ready for future SMP

#ifndef _KERNEL_SLAB_H_
#define _KERNEL_SLAB_H_

#include "types.h"

// Configuration
#define SLAB_MIN_SIZE           32          // Minimum allocation size (32 bytes)
#define SLAB_MAX_SIZE           4096        // Maximum slab-managed size (4KB)
#define SLAB_NUM_CLASSES        8           // Number of size classes
#define SLAB_OBJECTS_PER_PAGE   ((PAGE_SIZE - sizeof(slab_page_t)) / 64)  // Approximate
#define SLAB_MAGIC              0x534C4142  // "SLAB" magic number
#define SLAB_LARGE_MAGIC        0x4C524745  // "LRGE" for large allocations

// Size classes: 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes
// Each class manages objects of up to that size

// Forward declarations
struct slab_page;
struct slab_cache;

// Per-page slab structure (placed at beginning of each slab page)
typedef struct slab_page {
    uint32_t magic;                     // SLAB_MAGIC for validation
    uint32_t object_size;               // Size of objects in this slab
    uint16_t total_objects;             // Total objects in this slab
    uint16_t free_count;                // Number of free objects
    uint64_t bitmap[8];                 // Bitmap for 512 objects max (512 bits = 8*64)
    struct slab_page* next;             // Next slab page in cache
    struct slab_page* prev;             // Previous slab page in cache
    struct slab_cache* cache;           // Back-pointer to parent cache
    uint64_t phys_addr;                 // Physical address of this page (for unmapping)
} slab_page_t;

// Slab cache for a size class
typedef struct slab_cache {
    uint32_t object_size;               // Size of objects in this cache
    uint32_t objects_per_slab;          // Objects per slab page
    slab_page_t* partial_slabs;         // Slabs with some free objects
    slab_page_t* full_slabs;            // Slabs with no free objects
    slab_page_t* empty_slabs;           // Completely free slabs (cache for reuse)
    uint64_t total_allocs;              // Statistics: total allocations
    uint64_t total_frees;               // Statistics: total frees
    uint32_t slab_count;                // Number of slab pages
    uint32_t empty_slab_count;          // Number of empty slabs (for cleanup)
    // Future: spinlock_t lock;         // For SMP support
} slab_cache_t;

// Large allocation header (for allocations > SLAB_MAX_SIZE)
typedef struct large_alloc_header {
    uint32_t magic;                     // SLAB_LARGE_MAGIC
    uint32_t page_count;                // Number of pages allocated
    uint64_t size;                      // Requested size
    uint64_t phys_addr;                 // Physical address (for unmapping)
} large_alloc_header_t;

// SLAB allocator statistics
typedef struct slab_stats {
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t total_pages_used;
    uint64_t large_allocations;
    uint64_t large_frees;
    uint64_t cache_hits;                // Allocations from partial slabs
    uint64_t cache_misses;              // Required new slab allocation
} slab_stats_t;

// ============================================================================
// SLAB Allocator API
// ============================================================================

// Initialize the SLAB allocator (call during kernel init)
void slab_init(void);

// Allocate memory from SLAB allocator
// For sizes <= SLAB_MAX_SIZE: uses size-class caches
// For sizes > SLAB_MAX_SIZE: uses mm_allocate_contiguous_pages directly
void* slab_alloc(size_t size);

// Free memory allocated by slab_alloc
void slab_free(void* ptr);

// Reallocate memory (grow or shrink)
void* slab_realloc(void* ptr, size_t new_size);

// Allocate and zero memory
void* slab_calloc(size_t count, size_t size);

// Get SLAB allocator statistics
void slab_get_stats(slab_stats_t* stats);

// Print SLAB allocator statistics
void slab_print_stats(void);

// Validate SLAB allocator integrity (returns 0 on success)
int slab_validate(const char* caller);

// Shrink empty slabs to free memory
void slab_shrink(void);

// ============================================================================
// Internal functions (for debugging/testing)
// ============================================================================

// Get size class index for a given size
int slab_get_size_class(size_t size);

// Check if a pointer is managed by SLAB allocator
bool slab_is_slab_ptr(void* ptr);

// Check if a pointer is a large allocation
bool slab_is_large_ptr(void* ptr);

#endif // _KERNEL_SLAB_H_
