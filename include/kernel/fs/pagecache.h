// LikeOS-64 Unified Page Cache
//
// Caches file data pages indexed by (inode_id, page_index).  `inode_id` is the
// filesystem's native inode identifier (FAT32 start cluster, EXT4 inode no.);
// the cache treats it as an opaque key.  All disk translation (block_to_lba,
// block-chain walking, dirty writeback) goes through vfs_superblock_t->ops,
// so this file has no FS-specific knowledge.
//
// CLOCK eviction on a global LRU ring.  Write-back dirty tracking.
// Sequential read-ahead.  SMP-safe with per-bucket spinlocks.

#ifndef _KERNEL_PAGECACHE_H_
#define _KERNEL_PAGECACHE_H_

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>

struct vfs_superblock; /* forward — see vfs_sb.h                    */

// ============================================================================
// Configuration
// ============================================================================

// Hash table size — must be power of 2.
// 4096 buckets allows efficient lookup for large file sets.
#define PC_HASH_BUCKETS 4096
#define PC_HASH_MASK (PC_HASH_BUCKETS - 1)

// Memory pressure thresholds (in free pages).
// Below LOW_WATERMARK: aggressive eviction, block new allocations.
// Below HIGH_WATERMARK: background eviction tries to reclaim.
#define PC_LOW_WATERMARK_PAGES 512 // ~2MB — critical, must evict now
#define PC_HIGH_WATERMARK_PAGES 2048 // ~8MB — start gentle background eviction

// Read-ahead: max pages to prefetch on sequential access
#define PC_READAHEAD_MAX 16 // 64KB read-ahead

// Dirty writeback interval in timer ticks (~100 Hz, so 500 = ~5 seconds)
#define PC_WRITEBACK_INTERVAL 500

/* Most pages that may be waiting to be written before a writer is made to do
 * the writing itself.  8MB: enough that ordinary bursts never touch it, small
 * enough that un-reclaimable data cannot crowd out the rest of the system. */
#define PC_DIRTY_LIMIT_PAGES 2048

/* Pages a throttled writer stores before it is let go, even if more remain.
 * 256 (1MB) is a few times a typical write, so the limit is pushed back
 * without the writer disappearing for as long as the whole backlog takes --
 * in a graphical program the stalled thread is also answering the display. */
#define PC_DIRTY_WRITEBACK_BATCH 256

// ============================================================================
// Page flags
// ============================================================================

#define PC_PAGE_VALID 0x01 // Page contains valid data
#define PC_PAGE_DIRTY 0x02 // Page modified, needs writeback
#define PC_PAGE_REFERENCED 0x04 // Accessed since last CLOCK sweep
#define PC_PAGE_LOCKED 0x08 // Page locked for I/O (do not evict)
#define PC_PAGE_READAHEAD 0x10 // Page was fetched by read-ahead
/* Detached from the cache while somebody still held it.
 *
 * Invalidation (unlink, truncate, an open that truncates) must make a page
 * unreachable at once, but it must not release the frame while another
 * processor is still copying into or out of it -- that hands the frame to a new
 * owner with a write already in flight against it, and the damage surfaces far
 * away as corrupted data in an unrelated program.  Such a page is taken off
 * every list, marked with this, and freed by whoever releases it. */
#define PC_PAGE_DEAD 0x20

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
	unsigned long cluster_id; // File identity (start_cluster)
	unsigned long page_index; // Page offset within file

	// Data
	uint64_t phys_addr; // Physical address of the 4KB data page
	uint8_t *data; // Virtual pointer (phys_to_virt of phys_addr)

	// State
	uint32_t flags; // PC_PAGE_* flags
	uint32_t _pad;

	// Hash chain (per-bucket singly-linked list)
	struct pc_page *hash_next;

	// Global LRU doubly-linked list (for CLOCK eviction)
	struct pc_page *lru_prev;
	struct pc_page *lru_next;

	// Dirty list (doubly-linked, only if PC_PAGE_DIRTY is set)
	struct pc_page *dirty_prev;
	struct pc_page *dirty_next;
} pc_page_t;

// Per-file read-ahead state, embedded in fat32_file_t.
typedef struct pc_readahead {
	unsigned long last_page_index; // Last page accessed
	int sequential_count; // Consecutive sequential accesses
	int ra_pages; // Current read-ahead window size
} pc_readahead_t;

// ============================================================================
// API
// ============================================================================

// Initialization (call during kernel init after mm is ready)
void pagecache_init(void);

// --- Core lookup / insert ---

// Look up a cached page.  Returns NULL if not cached.
// Sets PC_PAGE_REFERENCED on hit.
pc_page_t *pagecache_lookup(unsigned long cluster_id, unsigned long page_index);

// Fetch a page: lookup + read-from-disk on miss.
// Caller provides the filesystem superblock (for block translation +
// chain walking) and the file's first block_id (`start_cluster`).
// `file_size` is the file size in bytes (to avoid reading past EOF).
// Returns page with data, or NULL on error.
// This is the primary entry point for cached reads.
pc_page_t *pagecache_get(unsigned long cluster_id, unsigned long page_index,
			 unsigned long file_size, struct vfs_superblock *sb,
			 unsigned long start_cluster);

// Insert a page into the cache (used internally and by write path).
// The caller provides a page with data already filled in.
// If a page with the same key exists, returns the existing one.
pc_page_t *pagecache_insert(pc_page_t *page);

// --- Write-back ---

// Mark a page dirty (adds to dirty list if not already there).
void pagecache_mark_dirty(pc_page_t *page);

// Flush all dirty pages for a specific file (cluster_id) to disk.
// Called on close, fsync.  Acquires the FS-wide I/O lock through the
// globally-registered superblock (g_root_sb) internally.
/* Obtain a page in order to WRITE into it.
 *
 * `full_page` says the caller will replace every byte, which lets the read of
 * the old contents be skipped.  A page past the current end of file is allowed
 * -- a write may extend it -- and starts as zeros.
 *
 * The caller copies its data in and then calls pagecache_mark_dirty(): the page
 * must not be advertised for writeback before it holds what is to be written. */
/* The returned page is HELD: it cannot be reclaimed or written back until the
 * caller releases it.  Without that, reclaim on another processor is free to
 * take the frame -- the page is clean until the copy finishes -- and the copy
 * lands in memory belonging to somebody else.  Release with exactly one of
 * pagecache_write_end() (published) or pagecache_write_abort() (discarded). */
pc_page_t *pagecache_get_for_write(unsigned long cluster_id,
				   unsigned long page_index,
				   unsigned long file_size,
				   struct vfs_superblock *sb,
				   unsigned long start_cluster, int full_page);

/* Publish the write and release the hold. */
void pagecache_write_end(pc_page_t *page);
/* Release the hold without publishing: the write failed. */
void pagecache_write_abort(pc_page_t *page);

/* Store the dirty pages of a set of files in ONE pass over the dirty list.
 * Flushing a set one file at a time costs a full walk per file, including for
 * files with nothing dirty -- worse than flushing everything. */
int pagecache_flush_fileset(const unsigned long *ids, unsigned nids);

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
void pagecache_invalidate_range(unsigned long cluster_id,
				unsigned long new_size);

// Invalidate the entire cache (e.g., filesystem unmount).
void pagecache_invalidate_all(void);

// --- Read-ahead ---

// Trigger read-ahead based on file access pattern.
// `ra` is the per-file-handle read-ahead state.
// `current_page` is the page just accessed.
void pagecache_readahead(pc_readahead_t *ra, unsigned long cluster_id,
			 unsigned long current_page, unsigned long file_size,
			 struct vfs_superblock *sb,
			 unsigned long start_cluster);

// --- Statistics ---

typedef struct pc_stats {
	uint64_t hits; // Cache hits
	uint64_t misses; // Cache misses (disk reads)
	uint64_t readahead_pages; // Pages fetched by read-ahead
	uint64_t evictions; // Pages evicted
	uint64_t dirty_writebacks; // Dirty pages written back
	uint64_t total_pages; // Current number of cached pages
} pc_stats_t;

void pagecache_get_stats(pc_stats_t *stats);

// --- Timer callback ---

// Called from timer IRQ handler every PC_WRITEBACK_INTERVAL ticks.
// Schedules dirty writeback (deferred to a non-IRQ context).
void pagecache_timer_tick(uint64_t ticks);

/* Perform the periodic writeback the timer flagged, if one is due.
 *
 * Call only from a context holding NO filesystem lock: writing back takes the
 * I/O lock exclusively, and doing that while holding the metadata lock shared
 * -- which every read does -- deadlocks. */
int pagecache_writeback_if_due(void);

/* Bound the amount of written-but-not-yet-stored data: past PC_DIRTY_LIMIT_PAGES
 * the caller writes it back itself.  Every dirty page is un-reclaimable until
 * stored, so without this a steady writer consumes memory the system cannot get
 * back and later allocations fail.  Call from the write path, which can block. */
int pagecache_balance_dirty(void);

/* Store at most `max_pages` dirty pages and return, whether or not more
 * remain.  For callers that must not be held up by the size of the backlog. */
int pagecache_flush_bounded(unsigned long max_pages);

#endif // _KERNEL_PAGECACHE_H_
