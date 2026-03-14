// LikeOS-64 Unified Page Cache
// Caches file data pages indexed by (start_cluster, page_index).
// Uses CLOCK eviction, write-back dirty tracking, and sequential read-ahead.
// SMP-safe with per-bucket spinlocks.

#ifndef _KERNEL_PAGECACHE_H_
#define _KERNEL_PAGECACHE_H_

#include "types.h"
#include "sched.h"

// ============================================================================
// Configuration
// ============================================================================

// Hash table size — must be power of 2.
// 4096 buckets allows efficient lookup for large file sets.
#define PC_HASH_BUCKETS         4096
#define PC_HASH_MASK            (PC_HASH_BUCKETS - 1)

// Memory pressure thresholds (in free pages).
// Below LOW_WATERMARK: aggressive eviction, block new allocations.
// Below HIGH_WATERMARK: background eviction tries to reclaim.
#define PC_LOW_WATERMARK_PAGES  512     // ~2MB — critical, must evict now
#define PC_HIGH_WATERMARK_PAGES 2048    // ~8MB — start gentle background eviction

// Read-ahead: max pages to prefetch on sequential access
#define PC_READAHEAD_MAX        16      // 64KB read-ahead

// Dirty writeback interval in timer ticks (~100 Hz, so 500 = ~5 seconds)
#define PC_WRITEBACK_INTERVAL   500

// ============================================================================
// Page flags
// ============================================================================

#define PC_PAGE_VALID           0x01    // Page contains valid data
#define PC_PAGE_DIRTY           0x02    // Page modified, needs writeback
#define PC_PAGE_REFERENCED      0x04    // Accessed since last CLOCK sweep
#define PC_PAGE_LOCKED          0x08    // Page locked for I/O (do not evict)
#define PC_PAGE_READAHEAD       0x10    // Page was fetched by read-ahead

// ============================================================================
// Structures
// ============================================================================

// Forward declaration
struct pc_page;

// A cached page of file data.
// Key: (cluster_id, page_index)
// cluster_id is the file's start_cluster (FAT32's de-facto inode number).
// page_index = file_offset / PAGE_SIZE.
typedef struct pc_page {
    // Cache key
    unsigned long   cluster_id;     // File identity (start_cluster)
    unsigned long   page_index;     // Page offset within file

    // Data
    uint64_t        phys_addr;      // Physical address of the 4KB data page
    uint8_t*        data;           // Virtual pointer (phys_to_virt of phys_addr)

    // State
    uint32_t        flags;          // PC_PAGE_* flags
    uint32_t        _pad;

    // Hash chain (per-bucket singly-linked list)
    struct pc_page* hash_next;

    // Global LRU doubly-linked list (for CLOCK eviction)
    struct pc_page* lru_prev;
    struct pc_page* lru_next;

    // Dirty list (doubly-linked, only if PC_PAGE_DIRTY is set)
    struct pc_page* dirty_prev;
    struct pc_page* dirty_next;
} pc_page_t;

// Per-file read-ahead state, embedded in fat32_file_t.
typedef struct pc_readahead {
    unsigned long   last_page_index;    // Last page accessed
    int             sequential_count;   // Consecutive sequential accesses
    int             ra_pages;           // Current read-ahead window size
} pc_readahead_t;

// ============================================================================
// API
// ============================================================================

// Initialization (call during kernel init after mm is ready)
void pagecache_init(void);

// --- Core lookup / insert ---

// Look up a cached page.  Returns NULL if not cached.
// Sets PC_PAGE_REFERENCED on hit.
pc_page_t* pagecache_lookup(unsigned long cluster_id, unsigned long page_index);

// Fetch a page: lookup + read-from-disk on miss.
// Caller must provide the FAT32 fs pointer and the file's cluster chain
// state so we can read the correct disk sector on miss.
// `file_size` is the file size in bytes (to avoid reading past EOF).
// Returns page with data, or NULL on error.
// This is the primary entry point for cached reads.
struct fat32_fs;  // forward declaration
pc_page_t* pagecache_get(unsigned long cluster_id, unsigned long page_index,
                         unsigned long file_size,
                         struct fat32_fs* fs, unsigned long start_cluster);

// Insert a page into the cache (used internally and by write path).
// The caller provides a page with data already filled in.
// If a page with the same key exists, returns the existing one.
pc_page_t* pagecache_insert(pc_page_t* page);

// --- Write-back ---

// Mark a page dirty (adds to dirty list if not already there).
void pagecache_mark_dirty(pc_page_t* page);

// Flush all dirty pages for a specific file (cluster_id) to disk.
// Called on close, fsync. Acquires fat32_io_lock internally.
int pagecache_flush_file(unsigned long cluster_id);

// Flush all dirty pages globally. Called by periodic writeback timer.
int pagecache_flush_all(void);

// Sync: flush all + block sync. Called by sync() syscall.
int pagecache_sync(void);

// --- Eviction ---

// Try to reclaim `nr_pages` pages. Returns number actually reclaimed.
// Skips dirty pages (flushes them first if `flush_dirty` is true).
unsigned long pagecache_shrink(unsigned long nr_pages, int flush_dirty);

// Check memory pressure and evict if necessary.
// Called from the page allocator when free pages are low.
void pagecache_reclaim_if_needed(void);

// --- Invalidation ---

// Invalidate all cached pages for a file. Used on unlink, truncate-to-0.
void pagecache_invalidate_file(unsigned long cluster_id);

// Invalidate pages beyond `new_size` bytes for a file. Used on truncate.
void pagecache_invalidate_range(unsigned long cluster_id, unsigned long new_size);

// Invalidate the entire cache (e.g., filesystem unmount).
void pagecache_invalidate_all(void);

// --- Read-ahead ---

// Trigger read-ahead based on file access pattern.
// `ra` is the per-file-handle read-ahead state.
// `current_page` is the page just accessed.
void pagecache_readahead(pc_readahead_t* ra, unsigned long cluster_id,
                         unsigned long current_page, unsigned long file_size,
                         struct fat32_fs* fs, unsigned long start_cluster);

// --- Statistics ---

typedef struct pc_stats {
    uint64_t hits;              // Cache hits
    uint64_t misses;            // Cache misses (disk reads)
    uint64_t readahead_pages;   // Pages fetched by read-ahead
    uint64_t evictions;         // Pages evicted
    uint64_t dirty_writebacks;  // Dirty pages written back
    uint64_t total_pages;       // Current number of cached pages
} pc_stats_t;

void pagecache_get_stats(pc_stats_t* stats);

// --- Timer callback ---

// Called from timer IRQ handler every PC_WRITEBACK_INTERVAL ticks.
// Schedules dirty writeback (deferred to a non-IRQ context).
void pagecache_timer_tick(uint64_t ticks);

#endif // _KERNEL_PAGECACHE_H_
