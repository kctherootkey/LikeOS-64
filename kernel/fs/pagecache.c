// LikeOS-64 Unified Page Cache
//
// Caches file data pages indexed by (inode_id, page_index).  inode_id is the
// FS-native identifier (FAT32 start cluster, EXT4 inode number); the cache
// stores it as an opaque key.  All disk translation (block_to_lba, chain
// walking, writeback) goes through vfs_superblock_t->ops, so this file has
// no FAT32 (or EXT4) knowledge.
//
// CLOCK eviction on a global LRU ring.  Per-bucket spinlocks for the hash
// table; global spinlock for LRU/dirty lists.  Dirty writeback on the
// timer + close/sync.  Sequential read-ahead with adaptive window sizing.

#include <kernel/fs/pagecache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/dev/block/block.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/icache.h>
#include <kernel/uapi/bug.h>

// Read sectors from block device (chunked).
#define PC_MAX_SECTORS_PER_READ 128

static int pc_read_sectors(const block_device_t *bdev, unsigned long lba,
			   unsigned long count, void *buf)
{
	unsigned long offset = 0;
	while (count > 0) {
		unsigned long chunk = (count > PC_MAX_SECTORS_PER_READ) ?
					      PC_MAX_SECTORS_PER_READ :
					      count;
		int st = bdev->read((block_device_t *)bdev, lba, chunk,
				    (uint8_t *)buf + offset);
		if (st != 0)
			return st;
		lba += chunk;
		offset += chunk * 512;
		count -= chunk;
	}
	return 0; // ST_OK
}

static int pc_write_sectors(const block_device_t *bdev, unsigned long lba,
			    unsigned long count, const void *buf)
{
	if (!bdev || !bdev->write)
		return -5; // ST_UNSUPPORTED
	unsigned long offset = 0;
	while (count > 0) {
		unsigned long chunk = (count > PC_MAX_SECTORS_PER_READ) ?
					      PC_MAX_SECTORS_PER_READ :
					      count;
		int st = bdev->write((block_device_t *)bdev, lba, chunk,
				     (const uint8_t *)buf + offset);
		if (st != 0)
			return st;
		lba += chunk;
		offset += chunk * 512;
		count -= chunk;
	}
	return 0;
}

// ============================================================================
// Global page cache state
// ============================================================================

// Hash table: array of bucket heads, each with its own spinlock.
typedef struct pc_bucket {
	pc_page_t *head;
	spinlock_t lock;
} pc_bucket_t;

static pc_bucket_t pc_hash[PC_HASH_BUCKETS];

// Global LRU doubly-linked list (circular) with a sentinel node.
// CLOCK hand points into this list.
static pc_page_t pc_lru_sentinel; // sentinel (not a real page)
static pc_page_t *pc_clock_hand; // CLOCK eviction scan position
static spinlock_t pc_lru_lock = SPINLOCK_INIT("pc_lru");

// Dirty list: doubly-linked with a sentinel.
static pc_page_t pc_dirty_sentinel;
static spinlock_t pc_dirty_lock = SPINLOCK_INIT("pc_dirty");

/* How many pages are on that list.
 *
 * Counted by list membership rather than by the dirty flag: membership has
 * exactly two chokepoints, both below and both under pc_dirty_lock, whereas
 * the flag is cleared in half a dozen places and a count that drifts either
 * throttles writers that need not wait or fails to throttle the ones that
 * must.  Guarded by pc_dirty_lock. */
static unsigned long pc_dirty_pages;

// Statistics
static volatile uint64_t pc_stat_hits;
static volatile uint64_t pc_stat_misses;
static volatile uint64_t pc_stat_readahead;
static volatile uint64_t pc_stat_evictions;
static volatile uint64_t pc_stat_writebacks;
static volatile uint64_t pc_stat_total_pages;

// Writeback flag — set by timer, consumed by a deferred context.
// Since we don't have kernel threads yet, writeback is done synchronously
// on the next cache access after the timer sets the flag.
static volatile int pc_writeback_pending;

// Initialized flag
static int pc_initialized;

// ============================================================================
// Hash function
// ============================================================================

static inline unsigned long pc_hash_key(unsigned long cluster_id,
					unsigned long page_index)
{
	// FNV-1a inspired mix
	unsigned long h = cluster_id * 2654435761UL;
	h ^= page_index * 2246822519UL;
	h ^= (h >> 16);
	return h & PC_HASH_MASK;
}

// ============================================================================
// LRU list helpers (caller must hold pc_lru_lock)
// ============================================================================

static inline void lru_insert_head(pc_page_t *page)
{
	WARN_ON(page->lru_next != NULL ||
		page->lru_prev !=
			NULL); /* double lru_insert_head: page already on LRU list */
	page->lru_next = pc_lru_sentinel.lru_next;
	page->lru_prev = &pc_lru_sentinel;
	pc_lru_sentinel.lru_next->lru_prev = page;
	pc_lru_sentinel.lru_next = page;
}

static inline void lru_remove(pc_page_t *page)
{
	WARN_ON_ONCE(
		!page->lru_prev &&
		!page->lru_next); /* lru_remove on page not linked into LRU: double-remove or corruption */
	if (page->lru_prev)
		page->lru_prev->lru_next = page->lru_next;
	if (page->lru_next)
		page->lru_next->lru_prev = page->lru_prev;
	page->lru_prev = 0;
	page->lru_next = 0;
}

// ============================================================================
// Dirty list helpers (caller must hold pc_dirty_lock)
// ============================================================================

static inline void dirty_list_add(pc_page_t *page)
{
	if (page->dirty_next || page->dirty_prev)
		return; // already on dirty list
	page->dirty_next = pc_dirty_sentinel.dirty_next;
	page->dirty_prev = &pc_dirty_sentinel;
	pc_dirty_sentinel.dirty_next->dirty_prev = page;
	pc_dirty_sentinel.dirty_next = page;
	pc_dirty_pages++;
}

static inline void dirty_list_remove(pc_page_t *page)
{
	/* Membership is what the count tracks, so a remove that removes
	 * nothing must not decrement it. */
	if (!page->dirty_prev && !page->dirty_next)
		return;
	if (page->dirty_prev)
		page->dirty_prev->dirty_next = page->dirty_next;
	if (page->dirty_next)
		page->dirty_next->dirty_prev = page->dirty_prev;
	page->dirty_prev = 0;
	page->dirty_next = 0;
	WARN_ON(pc_dirty_pages == 0);
	if (pc_dirty_pages)
		pc_dirty_pages--;
}

// ============================================================================
// Page allocation / deallocation
// ============================================================================

// Allocate a pc_page_t descriptor + a physical data page.
static pc_page_t *pc_page_alloc(void)
{
	might_sleep();
	pc_page_t *pg = (pc_page_t *)kalloc(sizeof(pc_page_t));
	if (!pg)
		return 0;
	mm_memset(pg, 0, sizeof(*pg));

	uint64_t phys = mm_allocate_physical_page();
	if (!phys) {
		kfree(pg);
		return 0;
	}
	pg->phys_addr = phys;
	pg->data = (uint8_t *)phys_to_virt(phys);
	WARN_ON(phys &
		(PAGE_SIZE - 1)); /* allocated physical page not page-aligned */
	WARN_ON(pg->data ==
		NULL); /* phys_to_virt returned NULL for valid physical page */
	return pg;
}

// Free a pc_page_t descriptor + its physical data page.
static void pc_page_free(pc_page_t *pg)
{
	if (!pg)
		return;
	if (pg->phys_addr) {
		mm_free_physical_page(pg->phys_addr);
		pg->phys_addr = 0;
		pg->data = 0;
	}
	kfree(pg);
}

// ============================================================================
// Walk cluster chain to get the cluster at page_index
// ============================================================================

// Returns the FS-native block_id (cluster number on FAT32) that contains
// the data for page_index, given the file's first block_id and the
// filesystem's block size.
//
// A page is 4096 bytes; a block may be 4096, 8192, 16384 ... or smaller.
// pages_per_block = block_size / PAGE_SIZE.
// block_index = page_index / pages_per_block.
// sub_index   = page_index % pages_per_block.
//
// Uses the icache's chain cache for O(1) lookups; falls back to linear
// chain walk via sb->ops->next_block().  Returns 0 on error / past EOF.
static unsigned long pc_walk_chain(vfs_superblock_t *sb,
				   unsigned long start_block,
				   unsigned long block_index)
{
	unsigned long result = icache_chain_get(start_block, block_index, sb);
	if (result != 0 && result < vfs_sb_end_of_chain(sb))
		return result;
	return 0;
}

// ============================================================================
// Initialization
// ============================================================================

void pagecache_init(void)
{
	if (pc_initialized)
		return;

	// Initialize hash buckets
	for (int i = 0; i < PC_HASH_BUCKETS; i++) {
		pc_hash[i].head = 0;
		pc_hash[i].lock = (spinlock_t)SPINLOCK_INIT("pc_bucket");
	}

	// Initialize LRU sentinel (circular: points to itself)
	pc_lru_sentinel.lru_next = &pc_lru_sentinel;
	pc_lru_sentinel.lru_prev = &pc_lru_sentinel;
	pc_clock_hand = &pc_lru_sentinel;

	// Initialize dirty sentinel
	pc_dirty_sentinel.dirty_next = &pc_dirty_sentinel;
	pc_dirty_sentinel.dirty_prev = &pc_dirty_sentinel;

	// Zero stats
	pc_stat_hits = 0;
	pc_stat_misses = 0;
	pc_stat_readahead = 0;
	pc_stat_evictions = 0;
	pc_stat_writebacks = 0;
	pc_stat_total_pages = 0;

	pc_writeback_pending = 0;
	pc_initialized = 1;

	kprintf("pagecache: initialized (%d hash buckets)\n", PC_HASH_BUCKETS);
}

// ============================================================================
// Lookup (cache-only, no disk I/O)
// ============================================================================

pc_page_t *pagecache_lookup(unsigned long cluster_id, unsigned long page_index)
{
	if (!pc_initialized)
		return 0;

	unsigned long bucket = pc_hash_key(cluster_id, page_index);
	uint64_t flags;
	spin_lock_irqsave(&pc_hash[bucket].lock, &flags);

	pc_page_t *pg = pc_hash[bucket].head;
	while (pg) {
		if (pg->cluster_id == cluster_id &&
		    pg->page_index == page_index) {
			pg->flags |= PC_PAGE_REFERENCED;
			spin_unlock_irqrestore(&pc_hash[bucket].lock, flags);
			return pg;
		}
		pg = pg->hash_next;
	}

	spin_unlock_irqrestore(&pc_hash[bucket].lock, flags);
	return 0;
}

// ============================================================================
// Insert a page into the hash table + LRU
// ============================================================================

pc_page_t *pagecache_insert(pc_page_t *page)
{
	BUG_ON(!page);
	WARN_ON(page->data ==
		NULL); /* inserting a page with no backing data buffer */
	WARN_ON(page->cluster_id <
		2); /* cluster_id < 2 is reserved in FAT32 - wrong page being cached */
	if (!pc_initialized || !page)
		return 0;

	unsigned long bucket = pc_hash_key(page->cluster_id, page->page_index);
	uint64_t flags;
	spin_lock_irqsave(&pc_hash[bucket].lock, &flags);

	// Check for duplicate
	pc_page_t *existing = pc_hash[bucket].head;
	while (existing) {
		if (existing->cluster_id == page->cluster_id &&
		    existing->page_index == page->page_index) {
			// Already cached — return existing, caller should free `page`
			existing->flags |= PC_PAGE_REFERENCED;
			spin_unlock_irqrestore(&pc_hash[bucket].lock, flags);
			return existing;
		}
		existing = existing->hash_next;
	}

	// Insert at head of hash chain
	page->hash_next = pc_hash[bucket].head;
	pc_hash[bucket].head = page;
	spin_unlock_irqrestore(&pc_hash[bucket].lock, flags);

	// Add to LRU
	uint64_t lru_flags;
	spin_lock_irqsave(&pc_lru_lock, &lru_flags);
	lru_insert_head(page);
	spin_unlock_irqrestore(&pc_lru_lock, lru_flags);

	__sync_fetch_and_add(&pc_stat_total_pages, 1);
	return page;
}

// ============================================================================
// Remove a page from hash table (caller must hold bucket lock)
// ============================================================================

static void hash_remove_locked(pc_page_t *page, unsigned long bucket)
{
	pc_page_t **pp = &pc_hash[bucket].head;
	while (*pp) {
		if (*pp == page) {
			*pp = page->hash_next;
			page->hash_next = 0;
			return;
		}
		pp = &(*pp)->hash_next;
	}
}

// ============================================================================
// CLOCK eviction — pagecache_shrink()
// ============================================================================

unsigned long pagecache_shrink(unsigned long nr_pages, int flush_dirty)
{
	if (!pc_initialized || nr_pages == 0)
		return 0;

	unsigned long reclaimed = 0;
	unsigned long scanned = 0;
	// Limit scan to 2 * total_pages to avoid infinite loops
	unsigned long max_scan = pc_stat_total_pages * 2;
	if (max_scan < 64)
		max_scan = 64;

	uint64_t lru_flags;
	spin_lock_irqsave(&pc_lru_lock, &lru_flags);

	while (reclaimed < nr_pages && scanned < max_scan) {
		// Advance clock hand
		if (pc_clock_hand == &pc_lru_sentinel)
			pc_clock_hand = pc_lru_sentinel.lru_next;
		if (pc_clock_hand == &pc_lru_sentinel)
			break; // empty list

		pc_page_t *pg = pc_clock_hand;
		pc_clock_hand = pg->lru_next;
		scanned++;

		// Skip locked pages
		if (pg->flags & PC_PAGE_LOCKED)
			continue;

		// CLOCK: if referenced, clear and give second chance
		if (pg->flags & PC_PAGE_REFERENCED) {
			pg->flags &= ~PC_PAGE_REFERENCED;
			continue;
		}

		// Skip dirty pages unless we're allowed to flush them
		if (pg->flags & PC_PAGE_DIRTY) {
			if (!flush_dirty)
				continue;
			// Write dirty page back to disk before evicting.
			// We need to drop the LRU lock to do I/O.
			// Mark locked to prevent concurrent eviction.
			pg->flags |= PC_PAGE_LOCKED;
			spin_unlock_irqrestore(&pc_lru_lock, lru_flags);

			// Flush this single dirty page inline through the generic sb
			// ops.  Everything FS-specific (cluster math, FAT chain walk)
			// lives behind block_to_lba() / next_block().
			int flush_ok = 0;
			if (g_root_sb) {
				vfs_superblock_t *sb = g_root_sb;
				const block_device_t *bdev = vfs_sb_bdev(sb);
				unsigned long bs = vfs_sb_block_size(sb);
				unsigned long ss = vfs_sb_sector_size(sb);
				unsigned long secs_per_block = bs / ss;
				unsigned long eoc = vfs_sb_end_of_chain(sb);
				vfs_sb_lock_io(sb);
				if (bs >= PAGE_SIZE) {
					unsigned long ppb = bs / PAGE_SIZE;
					if (ppb == 0)
						ppb = 1;
					unsigned long ci = pg->page_index / ppb;
					unsigned long sp = pg->page_index % ppb;
					unsigned long dc = pc_walk_chain(
						sb, pg->cluster_id, ci);
					unsigned long dc_lba =
						(dc >= 2 && dc < eoc) ?
							vfs_sb_block_to_lba(sb, dc) :
							0;
					/* lba 0 == a hole (block_to_lba sentinel)
					 * or an unmapped block.  Writing a dirty
					 * page there would clobber the superblock
					 * and needs block allocation we do not do;
					 * leave it dirty instead. */
					if (dc >= 2 && dc < eoc && dc_lba != 0) {
						unsigned long lba = dc_lba;
						if (bs == PAGE_SIZE) {
							pc_write_sectors(
								bdev, lba,
								secs_per_block,
								pg->data);
						} else {
							void *tmp = kalloc(bs);
							if (tmp) {
								pc_read_sectors(
									bdev,
									lba,
									secs_per_block,
									tmp);
								mm_memcpy(
									(uint8_t *)tmp +
										sp * PAGE_SIZE,
									pg->data,
									PAGE_SIZE);
								pc_write_sectors(
									bdev,
									lba,
									secs_per_block,
									tmp);
								kfree(tmp);
							}
						}
						flush_ok = 1;
					}
				} else {
					unsigned long bpp = PAGE_SIZE / bs;
					unsigned long fco =
						pg->page_index * bpp;
					unsigned long cur = pc_walk_chain(
						sb, pg->cluster_id, fco);
					unsigned off = 0;
					flush_ok = 1;
					for (unsigned long c = 0; c < bpp;
					     c++) {
						if (cur == 0 || cur >= eoc)
							break;
						unsigned long lba =
							vfs_sb_block_to_lba(
								sb, cur);
						/* Skip holes (lba 0): never
						 * write over the superblock. */
						if (lba != 0)
							pc_write_sectors(
								bdev, lba,
								secs_per_block,
								pg->data + off);
						off += bs;
						if (c + 1 < bpp)
							cur = vfs_sb_next_block(
								sb, cur);
					}
				}
				vfs_sb_unlock_io(sb);
			}
			if (flush_ok) {
				pg->flags &= ~PC_PAGE_DIRTY;
				uint64_t df;
				spin_lock_irqsave(&pc_dirty_lock, &df);
				dirty_list_remove(pg);
				spin_unlock_irqrestore(&pc_dirty_lock, df);
				__sync_fetch_and_add(&pc_stat_writebacks, 1);
			}
			pg->flags &= ~PC_PAGE_LOCKED;

			spin_lock_irqsave(&pc_lru_lock, &lru_flags);
			// If flush succeeded, the page is now clean and can be
			// evicted on the next pass. Don't count it yet.
			continue;
		}

		// Evict this page: remove from LRU
		lru_remove(pg);

		// Remove from hash table
		unsigned long bucket =
			pc_hash_key(pg->cluster_id, pg->page_index);
		// Drop LRU lock, take bucket lock
		spin_unlock_irqrestore(&pc_lru_lock, lru_flags);

		uint64_t bucket_flags;
		spin_lock_irqsave(&pc_hash[bucket].lock, &bucket_flags);
		hash_remove_locked(pg, bucket);
		spin_unlock_irqrestore(&pc_hash[bucket].lock, bucket_flags);

		// Remove from dirty list if present
		uint64_t dirty_flags;
		spin_lock_irqsave(&pc_dirty_lock, &dirty_flags);
		dirty_list_remove(pg);
		spin_unlock_irqrestore(&pc_dirty_lock, dirty_flags);

		// Free the page
		pc_page_free(pg);
		__sync_fetch_and_sub(&pc_stat_total_pages, 1);
		__sync_fetch_and_add(&pc_stat_evictions, 1);
		reclaimed++;

		// Re-acquire LRU lock for next iteration
		spin_lock_irqsave(&pc_lru_lock, &lru_flags);
	}

	spin_unlock_irqrestore(&pc_lru_lock, lru_flags);
	return reclaimed;
}

// ============================================================================
// Memory pressure check
// ============================================================================

void pagecache_reclaim_if_needed(void)
{
	if (!pc_initialized)
		return;
	uint64_t free = mm_get_free_pages();

	/* Reclaim never writes dirty pages back from here, however short of
	 * memory we are.
	 *
	 * This runs from inside a cache lookup, and a lookup is reached from
	 * the read path, which holds the metadata lock SHARED.  Writing a page
	 * back takes the filesystem's I/O lock for writing, and asking for a
	 * writer while a reader is held is a deadlock -- reachable the moment
	 * anything puts dirty pages in this cache.
	 *
	 * Clean pages can still be dropped, which is what relieves the
	 * pressure; the dirty ones are handed to the thread whose job that is,
	 * and become reclaimable once it has written them. */
	if (free < PC_LOW_WATERMARK_PAGES) {
		unsigned long target = PC_HIGH_WATERMARK_PAGES - free;

		if (target > pc_stat_total_pages)
			target = pc_stat_total_pages;
		if (target > 0)
			pagecache_shrink(target, 0);
		/* Ask for writeback so the dirty pages stop being unreclaimable. */
		pc_writeback_pending = 1;
	} else if (free < PC_HIGH_WATERMARK_PAGES) {
		// Gentle: reclaim a small batch
		pagecache_shrink(32, 0);
	}
}

// ============================================================================
// Coalesced read — read up to 64KB of contiguous pages in one USB transfer
// ============================================================================

// Maximum pages to coalesce in one read: 64KB / 4KB = 16
#define PC_COALESCE_MAX 16

// Try to read page_index (and up to PC_COALESCE_MAX-1 subsequent pages)
// in a single I/O if their underlying FS blocks are physically contiguous
// on disk.  Must be called under sb->ops->lock_io().
// Returns the requested page on success, NULL if the first page's blocks
// are not contiguous (caller should fall back to per-block reads).
static pc_page_t *pc_coalesced_read(vfs_superblock_t *sb,
				    unsigned long cluster_id,
				    unsigned long start_cluster,
				    unsigned long page_index,
				    unsigned long file_size)
{
	might_sleep();
	VM_BUG_ON(sb == NULL);
	const block_device_t *bdev = vfs_sb_bdev(sb);
	unsigned long bs = vfs_sb_block_size(sb);
	unsigned long ss = vfs_sb_sector_size(sb);
	unsigned long eoc = vfs_sb_end_of_chain(sb);
	unsigned long file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned long spp = PAGE_SIZE / ss; // sectors per page

	unsigned long run_start_lba = 0;
	unsigned long run_sectors = 0;
	unsigned long run_count = 0;

	/* Mapping phase.  Drivers with a shared mapping lock let the transfer
	 * below run with no filesystem lock at all (they fence data-block
	 * lifetime per inode); drivers without keep lock_io across both. */
	int split = (sb->ops->lock_map != 0);
	if (split)
		sb->ops->lock_map(sb);
	else
		vfs_sb_lock_io(sb);

	for (unsigned long pi = page_index;
	     pi < file_pages && run_count < PC_COALESCE_MAX; pi++) {
		if (pi != page_index && pagecache_lookup(cluster_id, pi))
			break;

		unsigned long page_lba;

		if (bs >= PAGE_SIZE) {
			unsigned long ppb = bs / PAGE_SIZE;
			if (!ppb)
				ppb = 1;
			unsigned long ci = pi / ppb;
			unsigned long sub = pi % ppb;
			unsigned long cl = pc_walk_chain(sb, start_cluster, ci);
			if (!cl || cl >= eoc)
				break;
			page_lba = vfs_sb_block_to_lba(sb, cl) + sub * spp;
		} else {
			unsigned long bpp = PAGE_SIZE / bs;
			unsigned long fci = pi * bpp;
			unsigned long fc =
				pc_walk_chain(sb, start_cluster, fci);
			if (!fc || fc >= eoc)
				break;
			int ok = 1;
			for (unsigned long c = 1; c < bpp; c++) {
				unsigned long nc = pc_walk_chain(
					sb, start_cluster, fci + c);
				if (nc != fc + c) {
					ok = 0;
					break;
				}
			}
			if (!ok) {
				if (run_count == 0) {
					if (split)
						sb->ops->unlock_map(sb);
					else
						vfs_sb_unlock_io(sb);
					return 0;
				}
				break;
			}
			page_lba = vfs_sb_block_to_lba(sb, fc);
		}

		/* A hole maps to LBA 0 (block_to_lba's sentinel — LBA 0 is the
		 * boot/superblock, never file data).  It must read as zeros, not
		 * as whatever is on the disk there.  End any run in progress
		 * before the hole; if the requested page itself is the hole,
		 * hand back a freshly zeroed page. */
		if (page_lba == 0) {
			if (run_count > 0)
				break;
			if (split)
				sb->ops->unlock_map(sb);
			else
				vfs_sb_unlock_io(sb);
			pc_page_t *zpg = pc_page_alloc();
			if (!zpg)
				return 0;
			zpg->cluster_id = cluster_id;
			zpg->page_index = page_index;
			mm_memset(zpg->data, 0, PAGE_SIZE);
			zpg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
			pc_page_t *zr = pagecache_insert(zpg);
			if (zr != zpg)
				pc_page_free(zpg);
			return zr;
		}

		if (run_count == 0) {
			run_start_lba = page_lba;
		} else if (page_lba != run_start_lba + run_sectors) {
			break;
		}

		run_sectors += spp;
		run_count++;
	}

	/* Mapping complete.  With a shared mapping lock the transfer below is
	 * lock-free (the run cannot be freed/reused under us: the caller holds
	 * the file's inode lock, which every data-freeing op takes exclusive).
	 * Without one, lock_io stays held across the transfer (old behaviour;
	 * released by pc_io_done below). */
	if (split)
		sb->ops->unlock_map(sb);
#define pc_io_done(sb)                          \
	do {                                    \
		if (!split)                     \
			vfs_sb_unlock_io((sb)); \
	} while (0)

	if (run_count == 0) {
		pc_io_done(sb);
		return 0;
	}

	// --- Single page fast path: read directly into page data ---
	if (run_count == 1) {
		pc_page_t *pg = pc_page_alloc();
		if (!pg) {
			pc_io_done(sb);
			return 0;
		}
		pg->cluster_id = cluster_id;
		pg->page_index = page_index;
		if (pc_read_sectors(bdev, run_start_lba, run_sectors,
				    pg->data) != 0) {
			pc_page_free(pg);
			pc_io_done(sb);
			return 0;
		}
		pc_io_done(sb);
		unsigned long psb = page_index * PAGE_SIZE;
		if (psb + PAGE_SIZE > file_size) {
			unsigned long v = file_size - psb;
			mm_memset(pg->data + v, 0, PAGE_SIZE - v);
		}
		pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
		pc_page_t *r = pagecache_insert(pg);
		if (r != pg)
			pc_page_free(pg);
		return r;
	}

	// --- Multi-page path: one big I/O, then distribute into pages ---
	unsigned long total_bytes = run_sectors * ss;
	void *buf = kalloc(total_bytes);
	if (!buf) {
		pc_page_t *pg = pc_page_alloc();
		if (!pg) {
			pc_io_done(sb);
			return 0;
		}
		pg->cluster_id = cluster_id;
		pg->page_index = page_index;
		if (pc_read_sectors(bdev, run_start_lba, spp, pg->data) != 0) {
			pc_page_free(pg);
			pc_io_done(sb);
			return 0;
		}
		pc_io_done(sb);
		unsigned long psb = page_index * PAGE_SIZE;
		if (psb + PAGE_SIZE > file_size) {
			unsigned long v = file_size - psb;
			mm_memset(pg->data + v, 0, PAGE_SIZE - v);
		}
		pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
		pc_page_t *r = pagecache_insert(pg);
		if (r != pg)
			pc_page_free(pg);
		return r;
	}

	if (pc_read_sectors(bdev, run_start_lba, run_sectors, buf) != 0) {
		kfree(buf);
		pc_io_done(sb);
		return 0;
	}
	pc_io_done(sb);

	// Distribute the big buffer into individual cache pages
	pc_page_t *result = 0;
	for (unsigned long i = 0; i < run_count; i++) {
		unsigned long pi = page_index + i;
		pc_page_t *pg = pc_page_alloc();
		if (!pg)
			break;
		pg->cluster_id = cluster_id;
		pg->page_index = pi;
		mm_memcpy(pg->data, (uint8_t *)buf + i * PAGE_SIZE, PAGE_SIZE);

		unsigned long psb = pi * PAGE_SIZE;
		if (psb + PAGE_SIZE > file_size) {
			unsigned long v = file_size - psb;
			mm_memset(pg->data + v, 0, PAGE_SIZE - v);
		}

		pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
		if (i > 0)
			pg->flags |= PC_PAGE_READAHEAD;

		pc_page_t *ins = pagecache_insert(pg);
		if (ins != pg)
			pc_page_free(pg);

		if (i == 0)
			result = ins;
		else
			__sync_fetch_and_add(&pc_stat_readahead, 1);
	}

	kfree(buf);
	return result;
#undef pc_io_done
}

// ============================================================================
// pagecache_get() — the primary read path
// ============================================================================

pc_page_t *pagecache_get(unsigned long cluster_id, unsigned long page_index,
			 unsigned long file_size, struct vfs_superblock *sb,
			 unsigned long start_cluster)
{
	VM_BUG_ON(sb == NULL);
	VM_BUG_ON(start_cluster < 2);
	if (!pc_initialized || !sb || start_cluster < 2)
		return 0;

	/* Deferred writeback does NOT happen here.
	 *
	 * Writing back takes the filesystem's I/O lock exclusively, and this is
	 * reached from the read path, which holds the metadata lock SHARED.
	 * Taking a writer while holding a reader is a deadlock, and it was only
	 * ever dormant because nothing put dirty pages in this cache for it to
	 * find -- writes went straight to the device.  Once they stopped doing
	 * that, the first read after a write wedged the machine.
	 *
	 * The periodic flush belongs where no filesystem lock is held: the
	 * commit thread picks it up (see the ordered-data flush in the journal
	 * commit), which is a context that owns its locks from the outside. */

	// 1. Try cache lookup (no I/O lock needed)
	pc_page_t *pg = pagecache_lookup(cluster_id, page_index);
	if (pg) {
		WARN_ON_ONCE(!(
			pg->flags &
			PC_PAGE_VALID)); /* lookup returned a page without PC_PAGE_VALID: stale insertion */
		__sync_fetch_and_add(&pc_stat_hits, 1);
		return pg;
	}

	// 2. Cache miss — need to read from disk
	__sync_fetch_and_add(&pc_stat_misses, 1);

	unsigned long file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (page_index >= file_pages)
		return 0;

	pagecache_reclaim_if_needed();

	// 3. Try coalesced read: up to 64KB of contiguous pages in one transfer.
	//    (Handles its own locking: mapping under the driver's shared map
	//    lock when provided, transfer unlocked; else lock_io across both.)
	pc_page_t *result = pc_coalesced_read(sb, cluster_id, start_cluster,
					      page_index, file_size);
	if (result)
		return result;

	// 4. Fallback for fragmented files (block_size < PAGE_SIZE with
	//    non-contiguous blocks inside a single page).  Collect the LBAs
	//    under the mapping lock first, then read them with the same
	//    locking policy as above.
	{
		const block_device_t *bdev = vfs_sb_bdev(sb);
		unsigned long bs = vfs_sb_block_size(sb);
		unsigned long ss = vfs_sb_sector_size(sb);
		unsigned long eoc = vfs_sb_end_of_chain(sb);
		unsigned long blocks_per_page = PAGE_SIZE / bs;
		if (blocks_per_page == 0)
			blocks_per_page = 1;
		unsigned long first_ci = page_index * blocks_per_page;
		/* PAGE_SIZE / bs is at most 8 (512-byte blocks). */
		unsigned long lbas[8];
		unsigned long nlba = 0;

		pc_page_t *new_pg = pc_page_alloc();
		if (!new_pg)
			return 0;
		new_pg->cluster_id = cluster_id;
		new_pg->page_index = page_index;

		int split = (sb->ops->lock_map != 0);
		if (split)
			sb->ops->lock_map(sb);
		else
			vfs_sb_lock_io(sb);

		unsigned long cur_block =
			pc_walk_chain(sb, start_cluster, first_ci);
		if (cur_block == 0 || cur_block >= eoc) {
			if (split)
				sb->ops->unlock_map(sb);
			else
				vfs_sb_unlock_io(sb);
			pc_page_free(new_pg);
			return 0;
		}
		for (unsigned long c = 0; c < blocks_per_page && c < 8; c++) {
			if (cur_block == 0 || cur_block >= eoc)
				break;
			lbas[nlba++] = vfs_sb_block_to_lba(sb, cur_block);
			if (c + 1 < blocks_per_page)
				cur_block = vfs_sb_next_block(sb, cur_block);
		}
		if (split)
			sb->ops->unlock_map(sb);

		unsigned long secs_per_block = bs / ss;
		unsigned offset = 0;
		for (unsigned long c = 0; c < nlba; c++) {
			/* LBA 0 marks a hole (see block_to_lba): zero-fill the
			 * block rather than reading the on-disk superblock. */
			if (lbas[c] == 0) {
				mm_memset(new_pg->data + offset, 0, bs);
				offset += bs;
				continue;
			}
			int st = pc_read_sectors(bdev, lbas[c], secs_per_block,
						 new_pg->data + offset);
			if (st != 0) {
				if (!split)
					vfs_sb_unlock_io(sb);
				pc_page_free(new_pg);
				return 0;
			}
			offset += bs;
		}
		if (!split)
			vfs_sb_unlock_io(sb);

		if (offset < PAGE_SIZE)
			mm_memset(new_pg->data + offset, 0, PAGE_SIZE - offset);

		unsigned long page_start_byte = page_index * PAGE_SIZE;
		if (page_start_byte + PAGE_SIZE > file_size) {
			unsigned long valid_bytes = file_size - page_start_byte;
			mm_memset(new_pg->data + valid_bytes, 0,
				  PAGE_SIZE - valid_bytes);
		}

		new_pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
		result = pagecache_insert(new_pg);
		if (result != new_pg)
			pc_page_free(new_pg);
		return result;
	}
}

// ============================================================================
// Mark a page dirty
// ============================================================================

void pagecache_mark_dirty(pc_page_t *page)
{
	if (!page)
		return;
	if (page->flags & PC_PAGE_DIRTY)
		return; // already dirty

	page->flags |= PC_PAGE_DIRTY;

	uint64_t flags;
	spin_lock_irqsave(&pc_dirty_lock, &flags);
	dirty_list_add(page);
	spin_unlock_irqrestore(&pc_dirty_lock, flags);
}

// ============================================================================
// Flush dirty pages for a specific file
// ============================================================================

/* ---- Writeback: merge neighbouring pages into one device command ---------
 *
 * What a block device charges for is the command, not the bytes: a hundred
 * separate 4KB writes cost a hundred round trips, while the same hundred pages
 * written as one command cost one.  Dirty pages that sit next to each other on
 * the device are therefore gathered and written together.
 *
 * They have to be copied to do it.  Cache pages are individual frames scattered
 * through memory, and a transfer needs one buffer, so a run is assembled in
 * this staging buffer first.  A memory copy against a device round trip is not
 * a close contest.
 */
#define PC_WB_RUN_MAX 32 /* pages per command -- 128KB */
static uint8_t *pc_wb_bounce;
static unsigned long pc_wb_bounce_pages;

static void pc_wb_bounce_init(void)
{
	if (pc_wb_bounce || pc_wb_bounce_pages)
		return;
	pc_wb_bounce = (uint8_t *)kalloc(PC_WB_RUN_MAX * PAGE_SIZE);
	/* Recorded even on failure so this is attempted once: without the
	 * buffer writeback still works, one page per command. */
	pc_wb_bounce_pages = pc_wb_bounce ? PC_WB_RUN_MAX : 0;
}

/* Retire one written-back page: no longer dirty, no longer held for I/O. */
static void pc_wb_retire(pc_page_t *p)
{
	uint64_t flags;

	p->flags &= ~(PC_PAGE_DIRTY | PC_PAGE_LOCKED);
	__sync_fetch_and_add(&pc_stat_writebacks, 1);
	spin_lock_irqsave(&pc_dirty_lock, &flags);
	dirty_list_remove(p);
	spin_unlock_irqrestore(&pc_dirty_lock, flags);
	/* Detached while it was being written back: we are the last reference. */
	if (p->flags & PC_PAGE_DEAD)
		pc_page_free(p);
}

/*
 * Write a batch of locked, dirty pages, merging neighbours.
 *
 * Only for the case where a block is exactly a page, which is what this
 * filesystem uses; the caller keeps its own per-page path for the others.
 * Returns the number of pages written.  Anything it could not place -- a page
 * with no block behind it -- is left dirty and unlocked for a later attempt.
 *
 * Must be called with the filesystem's I/O lock held: it walks the block
 * mapping, which the lock protects.
 */
static int pc_writeback_batch(vfs_superblock_t *sb, pc_page_t **batch, int n)
{
	unsigned long lbas[PC_WB_RUN_MAX];
	pc_page_t *run[PC_WB_RUN_MAX];
	const block_device_t *bdev = vfs_sb_bdev(sb);
	unsigned long ss = vfs_sb_sector_size(sb);
	unsigned long eoc = vfs_sb_end_of_chain(sb);
	unsigned long reserved_meta = vfs_sb_reserved_meta_block(sb);
	unsigned long spp = PAGE_SIZE / ss; /* sectors per page */
	int wrote = 0;
	int nrun = 0;

	/* This issues device commands and takes buffers, so it must be reached
	 * from a context that may block -- never from one with interrupts off. */
	might_sleep();
	BUG_ON(sb == NULL);
	BUG_ON(batch == NULL);
	VM_BUG_ON(n > PC_WB_RUN_MAX);

	pc_wb_bounce_init();

	/* Address order, so that pages which are adjacent on the device end up
	 * adjacent here.  The dirty list is in the order pages were first
	 * written, which is not the same thing -- and out of order, no two
	 * pages ever look mergeable.  Insertion sort: a batch is small and
	 * usually close to sorted already. */
	for (int i = 1; i < n; i++) {
		pc_page_t *key = batch[i];
		int j = i - 1;

		while (j >= 0 &&
		       (batch[j]->cluster_id > key->cluster_id ||
			(batch[j]->cluster_id == key->cluster_id &&
			 batch[j]->page_index > key->page_index))) {
			batch[j + 1] = batch[j];
			j--;
		}
		batch[j + 1] = key;
	}

	for (int i = 0; i <= n; i++) {
		pc_page_t *p = (i < n) ? batch[i] : 0;
		unsigned long lba = 0;
		int mergeable = 0;

		if (p) {
			unsigned long blk =
				pc_walk_chain(sb, p->cluster_id, p->page_index);

			if (blk == 0 || blk >= eoc ||
			    (reserved_meta && blk == reserved_meta)) {
				/* Nothing to write it to.  Leave it dirty --
				 * dropping it would lose the data silently --
				 * but SAY SO.
				 *
				 * A dirty page with no block behind it can
				 * never be stored and never becomes
				 * reclaimable, so it is not one lost write: it
				 * is a page held forever, and enough of them
				 * exhaust memory.  Whoever dirtied it should
				 * have established that the block existed.
				 * Silence here turns that mistake into an
				 * out-of-memory failure somewhere unrelated. */
				/* The id in hex as well as decimal: filesystems
				 * encode fields into it (ext4 packs a tag, an
				 * inode number and a logical index), and the
				 * decimal form of a tagged id is unreadable. */
				WARN_RATELIMIT(
					1,
					"pagecache: dirty page (file %lu = 0x%lx, page %lu, blk %lu, flags 0x%x) has no block behind it - cannot be stored",
					p->cluster_id, p->cluster_id,
					p->page_index, blk,
					(unsigned)p->flags);
				p->flags &= ~PC_PAGE_LOCKED;
				p = 0;
			} else {
				lba = vfs_sb_block_to_lba(sb, blk);
				/* LBA 0 is the boot block, used as the sentinel
				 * for a hole -- never file data. */
				if (lba == 0) {
					p->flags &= ~PC_PAGE_LOCKED;
					p = 0;
				}
			}
		}

		if (p && nrun > 0 && nrun < PC_WB_RUN_MAX &&
		    lba == lbas[nrun - 1] + spp)
			mergeable = 1;

		/* Flush the run whenever this page cannot extend it. */
		if (nrun > 0 && (!p || !mergeable)) {
			int ok;

			if (pc_wb_bounce && nrun > 1) {
				for (int k = 0; k < nrun; k++)
					mm_memcpy(pc_wb_bounce +
							  (unsigned long)k *
								  PAGE_SIZE,
						  run[k]->data, PAGE_SIZE);
				ok = (pc_write_sectors(bdev, lbas[0],
						       spp * (unsigned long)nrun,
						       pc_wb_bounce) == 0);
			} else {
				ok = 1;
				for (int k = 0; k < nrun; k++)
					if (pc_write_sectors(bdev, lbas[k], spp,
							     run[k]->data) != 0)
						ok = 0;
			}

			for (int k = 0; k < nrun; k++) {
				if (ok) {
					pc_wb_retire(run[k]);
					wrote++;
				} else {
					/* Still dirty: it did not reach the
					 * device, and forgetting that loses it. */
					run[k]->flags &= ~PC_PAGE_LOCKED;
				}
			}
			nrun = 0;
		}

		if (p) {
			run[nrun] = p;
			lbas[nrun] = lba;
			nrun++;
		}
	}
	VM_BUG_ON(nrun != 0);
	return wrote;
}

/*
 * Obtain the page covering `page_index` so the caller can write into it.
 *
 * The counterpart of pagecache_get() for the write path, and it differs in two
 * ways that matter:
 *
 *  - A write covering the WHOLE page needs no read first.  Every byte is about
 *    to be replaced, so fetching the old contents off the device is pure cost.
 *    A write covering only part of one must read them, or the bytes outside the
 *    written range would come back as whatever the page frame last held.
 *
 *  - A write may extend the file, so a page past the current end is legitimate.
 *    There is nothing on the device for it yet, and it starts as zeros -- which
 *    is what the unwritten part of a file reads as.
 *
 * The page is returned dirty-able but NOT yet dirty: the caller copies its data
 * in and then calls pagecache_mark_dirty(), so a page is never advertised as
 * needing writeback before it holds what is to be written back.
 */
pc_page_t *pagecache_get_for_write(unsigned long cluster_id,
				   unsigned long page_index,
				   unsigned long file_size,
				   struct vfs_superblock *sb,
				   unsigned long start_cluster, int full_page)
{
	pc_page_t *pg;
	unsigned long file_pages;

	might_sleep();
	VM_BUG_ON(sb == NULL);
	VM_BUG_ON(start_cluster < 2);
	if (!pc_initialized || !sb || start_cluster < 2)
		return 0;

	pg = pagecache_lookup(cluster_id, page_index);
	if (pg) {
		__sync_fetch_and_add(&pc_stat_hits, 1);
		pg->flags |= PC_PAGE_LOCKED;
		return pg;
	}

	__sync_fetch_and_add(&pc_stat_misses, 1);
	file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;

	if (full_page || page_index >= file_pages) {
		/* Nothing worth preserving: either the caller replaces every
		 * byte, or the page lies past anything the file has held. */
		pc_page_t *np;

		pagecache_reclaim_if_needed();
		np = pc_page_alloc();
		if (!np)
			return 0;
		np->cluster_id = cluster_id;
		np->page_index = page_index;
		/* Zeroed, not merely claimed.  A partial write beyond the end
		 * of the file must read back as zeros outside its own range,
		 * never as whatever the previous owner of the frame left. */
		mm_memset(np->data, 0, PAGE_SIZE);
		np->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
		pg = pagecache_insert(np);
		if (pg != np)
			pc_page_free(np);
		if (pg)
			pg->flags |= PC_PAGE_LOCKED;
		return pg;
	}

	/* Partial write inside the file: the existing contents are part of the
	 * result, so read them the ordinary way. */
	pg = pagecache_get(cluster_id, page_index, file_size, sb,
			   start_cluster);
	if (pg)
		pg->flags |= PC_PAGE_LOCKED;
	return pg;
}

/*
 * Finish a write into a page taken with pagecache_get_for_write().
 *
 * Publishes the data -- the page becomes dirty only now, never before it holds
 * what is to be stored -- and releases the hold taken at the start.
 *
 * That hold is not bookkeeping.  Between handing a page out for writing and the
 * caller finishing its copy, the page is a perfectly ordinary clean cache page,
 * and reclaim on another processor is entitled to take it: it is not dirty, so
 * nothing is lost by dropping it, and the frame goes back to the allocator.
 * The copy then lands in memory belonging to whoever was given that frame next.
 * The damage appears far from here and looks nothing like a filesystem problem
 * -- a corrupted network buffer, a program exiting without a word.
 */
void pagecache_write_end(pc_page_t *page)
{
	if (!page)
		return;
	/* Detached while we held it -- the file it belonged to was truncated or
	 * removed, so there is nothing to publish and nobody to publish it to.
	 * We are the last reference; the frame goes back here. */
	if (page->flags & PC_PAGE_DEAD) {
		page->flags &= ~PC_PAGE_LOCKED;
		pc_page_free(page);
		return;
	}
	pagecache_mark_dirty(page);
	page->flags &= ~PC_PAGE_LOCKED;
}

/* Release a page taken for writing WITHOUT publishing it: the write failed and
 * the page holds nothing worth storing. */
void pagecache_write_abort(pc_page_t *page)
{
	if (!page)
		return;
	if (page->flags & PC_PAGE_DEAD) {
		page->flags &= ~PC_PAGE_LOCKED;
		pc_page_free(page);
		return;
	}
	page->flags &= ~PC_PAGE_LOCKED;
}

/*
 * Store the dirty pages of a SET of files, in one pass over the dirty list.
 *
 * The per-file entry point below walks the whole list to find one file's
 * pages.  Calling it once per file therefore costs a full walk per file --
 * including for files that have nothing dirty -- which is worse than the
 * flush-everything it was meant to improve on.  A commit knows the whole set
 * up front, so it should pay for one walk, not one per member.
 */
int pagecache_flush_fileset(const unsigned long *ids, unsigned nids)
{
	int wrote = 0;
	uint64_t flags;

	might_sleep();
	BUG_ON(nids > 0 && ids == NULL);
	if (!pc_initialized || nids == 0)
		return 0;

#define FLUSH_SET_BATCH 32
	pc_page_t *batch[FLUSH_SET_BATCH];
	int batch_count;

	do {
		batch_count = 0;
		spin_lock_irqsave(&pc_dirty_lock, &flags);
		pc_page_t *pg = pc_dirty_sentinel.dirty_next;
		while (pg != &pc_dirty_sentinel &&
		       batch_count < FLUSH_SET_BATCH) {
			if ((pg->flags & PC_PAGE_DIRTY) &&
			    !(pg->flags & PC_PAGE_LOCKED)) {
				for (unsigned i = 0; i < nids; i++) {
					if (pg->cluster_id == ids[i]) {
						pg->flags |= PC_PAGE_LOCKED;
						batch[batch_count++] = pg;
						break;
					}
				}
			}
			pg = pg->dirty_next;
		}
		spin_unlock_irqrestore(&pc_dirty_lock, flags);

		if (batch_count == 0)
			break;

		vfs_superblock_t *sb = g_root_sb;
		if (!sb) {
			for (int i = 0; i < batch_count; i++)
				batch[i]->flags &= ~PC_PAGE_LOCKED;
			break;
		}
		vfs_sb_lock_io(sb);
		if (vfs_sb_block_size(sb) == PAGE_SIZE) {
			wrote += pc_writeback_batch(sb, batch, batch_count);
		} else {
			/* Block sizes that do not match a page keep the
			 * per-file path; this set walk is for the common one. */
			for (int i = 0; i < batch_count; i++)
				batch[i]->flags &= ~PC_PAGE_LOCKED;
			vfs_sb_unlock_io(sb);
			for (unsigned i = 0; i < nids; i++)
				wrote += pagecache_flush_file(ids[i]);
			break;
		}
		vfs_sb_unlock_io(sb);
	} while (batch_count == FLUSH_SET_BATCH);

#undef FLUSH_SET_BATCH
	return wrote;
}

int pagecache_flush_file(unsigned long cluster_id)
{
	might_sleep();
	VM_BUG_ON(cluster_id < 2);
	if (!pc_initialized || cluster_id < 2)
		return 0;

	// Collect dirty pages for this cluster_id from the dirty list.
	// We iterate the dirty list, pluck pages belonging to this file,
	// and write them back.
	uint64_t flags;
	int wrote = 0;

// We'll iterate the dirty list safely.  Since we need to do I/O,
// we collect pages first, then flush them.
// Use a small on-stack batch to avoid dynamic allocation.
#define FLUSH_BATCH 32
	pc_page_t *batch[FLUSH_BATCH];
	int batch_count;

	do {
		batch_count = 0;
		spin_lock_irqsave(&pc_dirty_lock, &flags);
		pc_page_t *pg = pc_dirty_sentinel.dirty_next;
		while (pg != &pc_dirty_sentinel && batch_count < FLUSH_BATCH) {
			if (pg->cluster_id == cluster_id &&
			    (pg->flags & PC_PAGE_DIRTY) &&
			    !(pg->flags & PC_PAGE_LOCKED)) {
				pg->flags |= PC_PAGE_LOCKED;
				batch[batch_count++] = pg;
			}
			pg = pg->dirty_next;
		}
		spin_unlock_irqrestore(&pc_dirty_lock, flags);

		if (batch_count == 0)
			break;

		// Flush the batch under the FS-wide I/O lock via sb ops.
		vfs_superblock_t *sb = g_root_sb;
		if (!sb) {
			for (int i = 0; i < batch_count; i++)
				batch[i]->flags &= ~PC_PAGE_LOCKED;
			break;
		}
		const block_device_t *bdev = vfs_sb_bdev(sb);
		unsigned long bs = vfs_sb_block_size(sb);
		unsigned long ss = vfs_sb_sector_size(sb);
		unsigned long secs_per_block = bs / ss;
		unsigned long eoc = vfs_sb_end_of_chain(sb);
		unsigned long reserved_meta = vfs_sb_reserved_meta_block(sb);

		vfs_sb_lock_io(sb);

		/* The ordinary case here: one block per page, so neighbouring
		 * pages sit on consecutive sectors and can go to the device
		 * together.  The per-page path below still covers the block
		 * sizes that do not divide evenly. */
		if (bs == PAGE_SIZE) {
			wrote += pc_writeback_batch(sb, batch, batch_count);
			vfs_sb_unlock_io(sb);
			continue;
		}

		for (int i = 0; i < batch_count; i++) {
			pc_page_t *p = batch[i];

			if (bs >= PAGE_SIZE) {
				unsigned long ppb = bs / PAGE_SIZE;
				if (ppb == 0)
					ppb = 1;

				unsigned long block_index = p->page_index / ppb;
				unsigned long sub_page = p->page_index % ppb;

				unsigned long disk_block = pc_walk_chain(
					sb, p->cluster_id, block_index);
				if (disk_block == 0 || disk_block >= eoc) {
					p->flags &= ~PC_PAGE_LOCKED;
					continue;
				}
				/* Guard against the FS-reserved metadata block (FAT32 root
                 * cluster) — writing user-data here would corrupt the root
                 * directory. */
				if (reserved_meta &&
				    disk_block == reserved_meta) {
					kprintf("pagecache: BUG: flush_file to reserved meta block! "
						"inode_id=%lu page_idx=%lu bi=%lu\n",
						p->cluster_id, p->page_index,
						block_index);
					p->flags &= ~PC_PAGE_LOCKED;
					continue;
				}

				unsigned long lba =
					vfs_sb_block_to_lba(sb, disk_block);

				/* lba 0 marks a hole: never persist a dirty
				 * page there (it is the superblock, and a hole
				 * needs allocation we do not do); leave dirty. */
				if (lba == 0) {
					p->flags &= ~PC_PAGE_LOCKED;
					continue;
				}
				if (bs == PAGE_SIZE) {
					pc_write_sectors(bdev, lba,
							 secs_per_block,
							 p->data);
				} else {
					void *tmp = kalloc(bs);
					if (tmp) {
						pc_read_sectors(bdev, lba,
								secs_per_block,
								tmp);
						mm_memcpy(
							(uint8_t *)tmp +
								sub_page *
									PAGE_SIZE,
							p->data, PAGE_SIZE);
						pc_write_sectors(bdev, lba,
								 secs_per_block,
								 tmp);
						kfree(tmp);
					}
				}
			} else {
				unsigned long bpp = PAGE_SIZE / bs;
				unsigned long first_block_offset =
					p->page_index * bpp;

				unsigned long cur_block = pc_walk_chain(
					sb, p->cluster_id, first_block_offset);
				unsigned offset = 0;
				for (unsigned long c = 0; c < bpp; c++) {
					if (cur_block == 0 || cur_block >= eoc)
						break;
					unsigned long lba = vfs_sb_block_to_lba(
						sb, cur_block);
					/* Skip holes (lba 0) — never write the
					 * superblock. */
					if (lba != 0)
						pc_write_sectors(bdev, lba,
								 secs_per_block,
								 p->data + offset);
					offset += bs;
					if (c + 1 < bpp)
						cur_block = vfs_sb_next_block(
							sb, cur_block);
				}
			}

			p->flags &= ~(PC_PAGE_DIRTY | PC_PAGE_LOCKED);
			wrote++;
			__sync_fetch_and_add(&pc_stat_writebacks, 1);

			spin_lock_irqsave(&pc_dirty_lock, &flags);
			dirty_list_remove(p);
			spin_unlock_irqrestore(&pc_dirty_lock, flags);
		}
		vfs_sb_unlock_io(sb);

	} while (batch_count == FLUSH_BATCH); // loop if we filled the batch

#undef FLUSH_BATCH
	return wrote;
}

// ============================================================================
// Flush all dirty pages
// ============================================================================

static int pagecache_flush_all_batch(void);

/*
 * Store up to `max_pages` dirty pages and stop, whether or not any remain.
 *
 * The bound is the point.  A writer that is made to do writeback must be
 * charged a piece of work proportional to what it produced, not the whole
 * backlog: the thread paying it is an ordinary program in the middle of a
 * write(), and in a graphical program that same thread is also servicing its
 * display connection.  Handing it every dirty page in the system stalls it for
 * as long as that takes, which is not a slow write -- it is a program that
 * stops answering, and the far end eventually gives up on it.
 *
 * Returns the number of pages stored.
 */
int pagecache_flush_bounded(unsigned long max_pages)
{
	unsigned long done = 0;

	might_sleep();
	if (!pc_initialized || max_pages == 0)
		return 0;

	while (done < max_pages) {
		int n = pagecache_flush_all_batch();

		if (n <= 0)
			break;
		done += (unsigned long)n;
	}
	return (int)done;
}

/*
 * Store ONE batch of dirty pages, wherever they belong, and return how many.
 *
 * Split out so that callers can choose how much work to take on: the periodic
 * writeback wants everything, a writer being throttled wants a bounded amount.
 * Returns 0 when there is nothing left to store.
 */
static int pagecache_flush_all_batch(void)
{
	might_sleep();
	if (!pc_initialized)
		return 0;

	int wrote = 0;
#define FLUSH_ALL_BATCH 32
	pc_page_t *batch[FLUSH_ALL_BATCH];
	int batch_count;

	/* Runs once.  Kept as a loop so that the `break` and `continue` inside
	 * still mean "this batch is finished", which is what they meant when
	 * this was the body of the flush-everything loop. */
	do {
		batch_count = 0;
		uint64_t flags;
		spin_lock_irqsave(&pc_dirty_lock, &flags);
		pc_page_t *pg = pc_dirty_sentinel.dirty_next;
		while (pg != &pc_dirty_sentinel &&
		       batch_count < FLUSH_ALL_BATCH) {
			if ((pg->flags & PC_PAGE_DIRTY) &&
			    !(pg->flags & PC_PAGE_LOCKED)) {
				pg->flags |= PC_PAGE_LOCKED;
				batch[batch_count++] = pg;
			}
			pg = pg->dirty_next;
		}
		spin_unlock_irqrestore(&pc_dirty_lock, flags);

		if (batch_count == 0)
			break;

		vfs_superblock_t *sb = g_root_sb;
		if (!sb) {
			for (int i = 0; i < batch_count; i++)
				batch[i]->flags &= ~PC_PAGE_LOCKED;
			break;
		}
		const block_device_t *bdev = vfs_sb_bdev(sb);
		unsigned long bs = vfs_sb_block_size(sb);
		unsigned long ss = vfs_sb_sector_size(sb);
		unsigned long secs_per_block = bs / ss;
		unsigned long eoc = vfs_sb_end_of_chain(sb);
		unsigned long reserved_meta = vfs_sb_reserved_meta_block(sb);

		vfs_sb_lock_io(sb);

		/* The ordinary case here: one block per page, so neighbouring
		 * pages sit on consecutive sectors and can go to the device
		 * together.  The per-page path below still covers the block
		 * sizes that do not divide evenly. */
		if (bs == PAGE_SIZE) {
			wrote += pc_writeback_batch(sb, batch, batch_count);
			vfs_sb_unlock_io(sb);
			continue;
		}

		for (int i = 0; i < batch_count; i++) {
			pc_page_t *p = batch[i];

			if (bs >= PAGE_SIZE) {
				unsigned long ppb = bs / PAGE_SIZE;
				if (ppb == 0)
					ppb = 1;

				unsigned long block_index = p->page_index / ppb;
				unsigned long sub_page = p->page_index % ppb;

				unsigned long disk_block = pc_walk_chain(
					sb, p->cluster_id, block_index);
				if (disk_block == 0 || disk_block >= eoc) {
					p->flags &= ~PC_PAGE_LOCKED;
					continue;
				}
				if (reserved_meta &&
				    disk_block == reserved_meta) {
					kprintf("pagecache: BUG: flush_all to reserved meta block! "
						"inode_id=%lu page_idx=%lu bi=%lu\n",
						p->cluster_id, p->page_index,
						block_index);
					p->flags &= ~PC_PAGE_LOCKED;
					continue;
				}

				unsigned long lba =
					vfs_sb_block_to_lba(sb, disk_block);
				/* lba 0 marks a hole: never persist a dirty page
				 * there (superblock; needs allocation). */
				if (lba == 0) {
					p->flags &= ~PC_PAGE_LOCKED;
					continue;
				}
				if (bs == PAGE_SIZE) {
					pc_write_sectors(bdev, lba,
							 secs_per_block,
							 p->data);
				} else {
					void *tmp = kalloc(bs);
					if (tmp) {
						pc_read_sectors(bdev, lba,
								secs_per_block,
								tmp);
						mm_memcpy(
							(uint8_t *)tmp +
								sub_page *
									PAGE_SIZE,
							p->data, PAGE_SIZE);
						pc_write_sectors(bdev, lba,
								 secs_per_block,
								 tmp);
						kfree(tmp);
					}
				}
			} else {
				unsigned long bpp = PAGE_SIZE / bs;
				unsigned long first_block_offset =
					p->page_index * bpp;

				unsigned long cur_block = pc_walk_chain(
					sb, p->cluster_id, first_block_offset);
				unsigned offset = 0;
				for (unsigned long c = 0; c < bpp; c++) {
					if (cur_block == 0 || cur_block >= eoc)
						break;
					unsigned long lba = vfs_sb_block_to_lba(
						sb, cur_block);
					/* Skip holes (lba 0) — never write the
					 * superblock. */
					if (lba != 0)
						pc_write_sectors(bdev, lba,
								 secs_per_block,
								 p->data + offset);
					offset += bs;
					if (c + 1 < bpp)
						cur_block = vfs_sb_next_block(
							sb, cur_block);
				}
			}

			p->flags &= ~(PC_PAGE_DIRTY | PC_PAGE_LOCKED);
			wrote++;
			__sync_fetch_and_add(&pc_stat_writebacks, 1);

			uint64_t df;
			spin_lock_irqsave(&pc_dirty_lock, &df);
			dirty_list_remove(p);
			spin_unlock_irqrestore(&pc_dirty_lock, df);
		}
		vfs_sb_unlock_io(sb);
	} while (0);

#undef FLUSH_ALL_BATCH
	return wrote;
}

int pagecache_flush_all(void)
{
	int wrote = 0;

	might_sleep();
	for (;;) {
		int n = pagecache_flush_all_batch();

		if (n <= 0)
			break;
		wrote += n;
	}
	return wrote;
}

// ============================================================================
// Sync (flush all + block device sync)
// ============================================================================

int pagecache_sync(void)
{
	int wrote = pagecache_flush_all();

	// Flush dirty inode metadata (sizes, attributes, etc.)
	extern int icache_flush_all(void);
	wrote += icache_flush_all();

	// Call block device sync if available, via the generic superblock.
	if (g_root_sb) {
		const block_device_t *bdev = vfs_sb_bdev(g_root_sb);
		if (bdev && bdev->sync) {
			vfs_sb_lock_io(g_root_sb);
			bdev->sync((block_device_t *)bdev);
			vfs_sb_unlock_io(g_root_sb);
		}
	}
	return wrote;
}

// ============================================================================
// Invalidation
// ============================================================================

void pagecache_invalidate_file(unsigned long cluster_id)
{
	VM_BUG_ON(cluster_id < 2);
	if (!pc_initialized || cluster_id < 2)
		return;

	// Scan all hash buckets for pages with this cluster_id.
	// This is O(n) but invalidation is infrequent (unlink/truncate).
	for (int b = 0; b < PC_HASH_BUCKETS; b++) {
		uint64_t flags;
		spin_lock_irqsave(&pc_hash[b].lock, &flags);

		pc_page_t **pp = &pc_hash[b].head;
		while (*pp) {
			pc_page_t *pg = *pp;
			if (pg->cluster_id == cluster_id) {
				// Remove from hash chain
				*pp = pg->hash_next;
				pg->hash_next = 0;
				spin_unlock_irqrestore(&pc_hash[b].lock, flags);

				// Remove from LRU
				uint64_t lru_flags;
				spin_lock_irqsave(&pc_lru_lock, &lru_flags);
				// Fix clock hand if it points to this page
				if (pc_clock_hand == pg)
					pc_clock_hand = pg->lru_next;
				lru_remove(pg);
				spin_unlock_irqrestore(&pc_lru_lock, lru_flags);

				// Remove from dirty list
				uint64_t df;
				spin_lock_irqsave(&pc_dirty_lock, &df);
				dirty_list_remove(pg);
				spin_unlock_irqrestore(&pc_dirty_lock, df);

				/* Free it only if nobody is using it.  A held
				 * page is being copied into or out of right
				 * now; releasing the frame here would hand it
				 * to a new owner with that copy still in
				 * flight.  It is already off every list, so
				 * nothing can find it again -- the holder
				 * frees it when it lets go. */
				if (pg->flags & PC_PAGE_LOCKED)
					pg->flags |= PC_PAGE_DEAD;
				else
					pc_page_free(pg);
				__sync_fetch_and_sub(&pc_stat_total_pages, 1);

				// Re-acquire bucket lock and restart scan (chain modified)
				spin_lock_irqsave(&pc_hash[b].lock, &flags);
				pp = &pc_hash[b].head;
				continue;
			}
			pp = &(*pp)->hash_next;
		}
		spin_unlock_irqrestore(&pc_hash[b].lock, flags);
	}
}

void pagecache_invalidate_range(unsigned long cluster_id,
				unsigned long new_size)
{
	if (!pc_initialized || cluster_id < 2)
		return;

	// Invalidate pages with page_index >= ceil(new_size / PAGE_SIZE)
	unsigned long first_invalid = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

	for (int b = 0; b < PC_HASH_BUCKETS; b++) {
		uint64_t flags;
		spin_lock_irqsave(&pc_hash[b].lock, &flags);

		pc_page_t **pp = &pc_hash[b].head;
		while (*pp) {
			pc_page_t *pg = *pp;
			if (pg->cluster_id == cluster_id &&
			    pg->page_index >= first_invalid) {
				*pp = pg->hash_next;
				pg->hash_next = 0;
				spin_unlock_irqrestore(&pc_hash[b].lock, flags);

				uint64_t lru_flags;
				spin_lock_irqsave(&pc_lru_lock, &lru_flags);
				if (pc_clock_hand == pg)
					pc_clock_hand = pg->lru_next;
				lru_remove(pg);
				spin_unlock_irqrestore(&pc_lru_lock, lru_flags);

				uint64_t df;
				spin_lock_irqsave(&pc_dirty_lock, &df);
				dirty_list_remove(pg);
				spin_unlock_irqrestore(&pc_dirty_lock, df);

				/* Free it only if nobody is using it.  A held
				 * page is being copied into or out of right
				 * now; releasing the frame here would hand it
				 * to a new owner with that copy still in
				 * flight.  It is already off every list, so
				 * nothing can find it again -- the holder
				 * frees it when it lets go. */
				if (pg->flags & PC_PAGE_LOCKED)
					pg->flags |= PC_PAGE_DEAD;
				else
					pc_page_free(pg);
				__sync_fetch_and_sub(&pc_stat_total_pages, 1);

				spin_lock_irqsave(&pc_hash[b].lock, &flags);
				pp = &pc_hash[b].head;
				continue;
			}
			pp = &(*pp)->hash_next;
		}
		spin_unlock_irqrestore(&pc_hash[b].lock, flags);
	}
}

void pagecache_invalidate_all(void)
{
	if (!pc_initialized)
		return;

	for (int b = 0; b < PC_HASH_BUCKETS; b++) {
		uint64_t flags;
		spin_lock_irqsave(&pc_hash[b].lock, &flags);

		pc_page_t *pg = pc_hash[b].head;
		pc_hash[b].head = 0;
		spin_unlock_irqrestore(&pc_hash[b].lock, flags);

		while (pg) {
			pc_page_t *next = pg->hash_next;

			uint64_t lru_flags;
			spin_lock_irqsave(&pc_lru_lock, &lru_flags);
			if (pc_clock_hand == pg)
				pc_clock_hand = pg->lru_next;
			lru_remove(pg);
			spin_unlock_irqrestore(&pc_lru_lock, lru_flags);

			uint64_t df;
			spin_lock_irqsave(&pc_dirty_lock, &df);
			dirty_list_remove(pg);
			spin_unlock_irqrestore(&pc_dirty_lock, df);

			/* Same rule as the other invalidations: a held page is
			 * detached now and freed by whoever releases it. */
			if (pg->flags & PC_PAGE_LOCKED)
				pg->flags |= PC_PAGE_DEAD;
			else
				pc_page_free(pg);
			__sync_fetch_and_sub(&pc_stat_total_pages, 1);

			pg = next;
		}
	}
}

// ============================================================================
// Read-ahead
// ============================================================================

void pagecache_readahead(pc_readahead_t *ra, unsigned long cluster_id,
			 unsigned long current_page, unsigned long file_size,
			 struct vfs_superblock *sb, unsigned long start_cluster)
{
	if (!ra || !pc_initialized)
		return;

	// Detect sequential access
	if (current_page == ra->last_page_index + 1) {
		ra->sequential_count++;
		// Grow read-ahead window: 1, 2, 4, 8, 16
		if (ra->sequential_count >= 2 &&
		    ra->ra_pages < PC_READAHEAD_MAX)
			ra->ra_pages =
				(ra->ra_pages < 1) ? 1 : ra->ra_pages * 2;
		if (ra->ra_pages > PC_READAHEAD_MAX)
			ra->ra_pages = PC_READAHEAD_MAX;
	} else {
		// Non-sequential — reset
		ra->sequential_count = 0;
		ra->ra_pages = 0;
	}
	ra->last_page_index = current_page;

	// Issue read-ahead if window > 0
	if (ra->ra_pages <= 0)
		return;

	unsigned long file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;

	for (int i = 1; i <= ra->ra_pages; i++) {
		unsigned long ahead_page = current_page + (unsigned long)i;
		if (ahead_page >= file_pages)
			break;

		// Check if already cached
		pc_page_t *existing = pagecache_lookup(cluster_id, ahead_page);
		if (existing)
			continue;

		// Fetch the page (will do disk I/O on miss)
		pc_page_t *pg = pagecache_get(cluster_id, ahead_page, file_size,
					      sb, start_cluster);
		if (pg) {
			pg->flags |= PC_PAGE_READAHEAD;
			__sync_fetch_and_add(&pc_stat_readahead, 1);
		}
	}
}

// ============================================================================
// Statistics
// ============================================================================

void pagecache_get_stats(pc_stats_t *stats)
{
	if (!stats)
		return;
	stats->hits = pc_stat_hits;
	stats->misses = pc_stat_misses;
	stats->readahead_pages = pc_stat_readahead;
	stats->evictions = pc_stat_evictions;
	stats->dirty_writebacks = pc_stat_writebacks;
	stats->total_pages = pc_stat_total_pages;
}

// ============================================================================
// Timer callback
// ============================================================================

/*
 * Keep the amount of written-but-not-yet-stored data bounded.
 *
 * A cached write returns without touching the device, which is the point of it
 * -- but it also means a program can dirty pages faster than they are written
 * back, and every dirty page is a page that cannot be reclaimed until it has
 * been.  Left alone, a program writing steadily consumes all of memory: not as
 * cache, which would be given back, but as data that MUST be kept.  What
 * follows is allocation failures in whichever program asks next.
 *
 * So past a limit the writer is made to do the writing.  Charging it to the
 * program producing the data is the point -- it is what makes a fast writer
 * slow down instead of the machine running out.
 *
 * Callers must be able to block, and must not be holding a lock that writeback
 * needs from the outside; the write path qualifies, holding the filesystem's
 * I/O lock (which is recursive) for writing.
 */
int pagecache_balance_dirty(void)
{
	unsigned long dirty;
	uint64_t flags;

	if (!pc_initialized)
		return 0;

	spin_lock_irqsave(&pc_dirty_lock, &flags);
	dirty = pc_dirty_pages;
	spin_unlock_irqrestore(&pc_dirty_lock, flags);

	if (dirty < PC_DIRTY_LIMIT_PAGES)
		return 0;

	/* Enough to make progress against the limit, not enough to become a
	 * stall.  Whatever is still outstanding is the writeback thread's to
	 * finish; the next write() that is still over the limit takes another
	 * turn. */
	might_sleep();
	return pagecache_flush_bounded(PC_DIRTY_WRITEBACK_BATCH);
}

/*
 * Do the periodic writeback the timer asked for, if it is due.
 *
 * Split from the timer (which cannot block) and from the cache lookups (which
 * run holding filesystem locks, where taking the I/O lock exclusively would
 * deadlock).  The caller must hold no filesystem lock -- a thread whose whole
 * job is this, rather than one that happened to touch a file.
 */
int pagecache_writeback_if_due(void)
{
	if (!pc_initialized)
		return 0;
	if (!pc_writeback_pending)
		return 0;
	might_sleep();
	pc_writeback_pending = 0;
	return pagecache_flush_all();
}

void pagecache_timer_tick(uint64_t ticks)
{
	if (!pc_initialized)
		return;
	// Set the writeback pending flag; actual flush happens on next cache
	// access (we can't do blocking I/O in an IRQ handler).
	if ((ticks % PC_WRITEBACK_INTERVAL) == 0) {
		pc_writeback_pending = 1;
	}
}
