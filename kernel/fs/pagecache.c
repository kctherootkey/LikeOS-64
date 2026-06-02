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

#include "../../include/kernel/pagecache.h"
#include "../../include/kernel/vfs_sb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/block.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/icache.h"
#include "../../include/kernel/bug.h"

// Read sectors from block device (chunked).
#define PC_MAX_SECTORS_PER_READ 128

static int pc_read_sectors(const block_device_t *bdev, unsigned long lba,
                           unsigned long count, void *buf)
{
    unsigned long offset = 0;
    while (count > 0) {
        unsigned long chunk = (count > PC_MAX_SECTORS_PER_READ)
                            ? PC_MAX_SECTORS_PER_READ : count;
        int st = bdev->read((block_device_t *)bdev, lba, chunk,
                            (uint8_t *)buf + offset);
        if (st != 0)
            return st;
        lba    += chunk;
        offset += chunk * 512;
        count  -= chunk;
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
        unsigned long chunk = (count > PC_MAX_SECTORS_PER_READ)
                            ? PC_MAX_SECTORS_PER_READ : count;
        int st = bdev->write((block_device_t *)bdev, lba, chunk,
                             (const uint8_t *)buf + offset);
        if (st != 0)
            return st;
        lba    += chunk;
        offset += chunk * 512;
        count  -= chunk;
    }
    return 0;
}

// ============================================================================
// Global page cache state
// ============================================================================

// Hash table: array of bucket heads, each with its own spinlock.
typedef struct pc_bucket {
    pc_page_t*  head;
    spinlock_t  lock;
} pc_bucket_t;

static pc_bucket_t  pc_hash[PC_HASH_BUCKETS];

// Global LRU doubly-linked list (circular) with a sentinel node.
// CLOCK hand points into this list.
static pc_page_t    pc_lru_sentinel;    // sentinel (not a real page)
static pc_page_t*   pc_clock_hand;      // CLOCK eviction scan position
static spinlock_t   pc_lru_lock = SPINLOCK_INIT("pc_lru");

// Dirty list: doubly-linked with a sentinel.
static pc_page_t    pc_dirty_sentinel;
static spinlock_t   pc_dirty_lock = SPINLOCK_INIT("pc_dirty");

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
    WARN_ON(page->lru_next != NULL || page->lru_prev != NULL);  /* double lru_insert_head: page already on LRU list */
    page->lru_next = pc_lru_sentinel.lru_next;
    page->lru_prev = &pc_lru_sentinel;
    pc_lru_sentinel.lru_next->lru_prev = page;
    pc_lru_sentinel.lru_next = page;
}

static inline void lru_remove(pc_page_t *page)
{
    WARN_ON_ONCE(!page->lru_prev && !page->lru_next);  /* lru_remove on page not linked into LRU: double-remove or corruption */
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
}

static inline void dirty_list_remove(pc_page_t *page)
{
    if (page->dirty_prev)
        page->dirty_prev->dirty_next = page->dirty_next;
    if (page->dirty_next)
        page->dirty_next->dirty_prev = page->dirty_prev;
    page->dirty_prev = 0;
    page->dirty_next = 0;
}

// ============================================================================
// Page allocation / deallocation
// ============================================================================

// Allocate a pc_page_t descriptor + a physical data page.
static pc_page_t* pc_page_alloc(void)
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
    pg->data      = (uint8_t *)phys_to_virt(phys);
    WARN_ON(phys & (PAGE_SIZE - 1));  /* allocated physical page not page-aligned */
    WARN_ON(pg->data == NULL);  /* phys_to_virt returned NULL for valid physical page */
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
    pc_stat_hits        = 0;
    pc_stat_misses      = 0;
    pc_stat_readahead   = 0;
    pc_stat_evictions   = 0;
    pc_stat_writebacks  = 0;
    pc_stat_total_pages = 0;

    pc_writeback_pending = 0;
    pc_initialized = 1;

    kprintf("pagecache: initialized (%d hash buckets)\n", PC_HASH_BUCKETS);
}

// ============================================================================
// Lookup (cache-only, no disk I/O)
// ============================================================================

pc_page_t* pagecache_lookup(unsigned long cluster_id, unsigned long page_index)
{
    if (!pc_initialized)
        return 0;

    unsigned long bucket = pc_hash_key(cluster_id, page_index);
    uint64_t flags;
    spin_lock_irqsave(&pc_hash[bucket].lock, &flags);

    pc_page_t *pg = pc_hash[bucket].head;
    while (pg) {
        if (pg->cluster_id == cluster_id && pg->page_index == page_index) {
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

pc_page_t* pagecache_insert(pc_page_t *page)
{
    BUG_ON(!page);
    WARN_ON(page->data == NULL);       /* inserting a page with no backing data buffer */
    WARN_ON(page->cluster_id < 2);     /* cluster_id < 2 is reserved in FAT32 - wrong page being cached */
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
                unsigned long bs   = vfs_sb_block_size(sb);
                unsigned long ss   = vfs_sb_sector_size(sb);
                unsigned long secs_per_block = bs / ss;
                unsigned long eoc = vfs_sb_end_of_chain(sb);
                vfs_sb_lock_io(sb);
                if (bs >= PAGE_SIZE) {
                    unsigned long ppb = bs / PAGE_SIZE;
                    if (ppb == 0) ppb = 1;
                    unsigned long ci = pg->page_index / ppb;
                    unsigned long sp = pg->page_index % ppb;
                    unsigned long dc = pc_walk_chain(sb, pg->cluster_id, ci);
                    if (dc >= 2 && dc < eoc) {
                        unsigned long lba = vfs_sb_block_to_lba(sb, dc);
                        if (bs == PAGE_SIZE) {
                            pc_write_sectors(bdev, lba, secs_per_block, pg->data);
                        } else {
                            void *tmp = kalloc(bs);
                            if (tmp) {
                                pc_read_sectors(bdev, lba, secs_per_block, tmp);
                                mm_memcpy((uint8_t *)tmp + sp * PAGE_SIZE,
                                          pg->data, PAGE_SIZE);
                                pc_write_sectors(bdev, lba, secs_per_block, tmp);
                                kfree(tmp);
                            }
                        }
                        flush_ok = 1;
                    }
                } else {
                    unsigned long bpp = PAGE_SIZE / bs;
                    unsigned long fco = pg->page_index * bpp;
                    unsigned long cur = pc_walk_chain(sb, pg->cluster_id, fco);
                    unsigned off = 0;
                    flush_ok = 1;
                    for (unsigned long c = 0; c < bpp; c++) {
                        if (cur == 0 || cur >= eoc) break;
                        unsigned long lba = vfs_sb_block_to_lba(sb, cur);
                        pc_write_sectors(bdev, lba, secs_per_block,
                                         pg->data + off);
                        off += bs;
                        if (c + 1 < bpp)
                            cur = vfs_sb_next_block(sb, cur);
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
        unsigned long bucket = pc_hash_key(pg->cluster_id, pg->page_index);
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
    if (free < PC_LOW_WATERMARK_PAGES) {
        // Aggressive: reclaim enough to get above high watermark
        unsigned long target = PC_HIGH_WATERMARK_PAGES - free;
        if (target > pc_stat_total_pages)
            target = pc_stat_total_pages;
        if (target > 0)
            pagecache_shrink(target, 1);
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
static pc_page_t* pc_coalesced_read(vfs_superblock_t *sb,
                                     unsigned long cluster_id,
                                     unsigned long start_cluster,
                                     unsigned long page_index,
                                     unsigned long file_size)
{
    might_sleep();
    VM_BUG_ON(sb == NULL);
    const block_device_t *bdev = vfs_sb_bdev(sb);
    unsigned long bs  = vfs_sb_block_size(sb);
    unsigned long ss  = vfs_sb_sector_size(sb);
    unsigned long eoc = vfs_sb_end_of_chain(sb);
    unsigned long file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned long spp = PAGE_SIZE / ss;            // sectors per page

    unsigned long run_start_lba = 0;
    unsigned long run_sectors   = 0;
    unsigned long run_count     = 0;

    for (unsigned long pi = page_index;
         pi < file_pages && run_count < PC_COALESCE_MAX; pi++) {

        if (pi != page_index && pagecache_lookup(cluster_id, pi))
            break;

        unsigned long page_lba;

        if (bs >= PAGE_SIZE) {
            unsigned long ppb = bs / PAGE_SIZE;
            if (!ppb) ppb = 1;
            unsigned long ci  = pi / ppb;
            unsigned long sub = pi % ppb;
            unsigned long cl  = pc_walk_chain(sb, start_cluster, ci);
            if (!cl || cl >= eoc) break;
            page_lba = vfs_sb_block_to_lba(sb, cl) + sub * spp;
        } else {
            unsigned long bpp = PAGE_SIZE / bs;
            unsigned long fci = pi * bpp;
            unsigned long fc  = pc_walk_chain(sb, start_cluster, fci);
            if (!fc || fc >= eoc) break;
            int ok = 1;
            for (unsigned long c = 1; c < bpp; c++) {
                unsigned long nc = pc_walk_chain(sb, start_cluster, fci + c);
                if (nc != fc + c) { ok = 0; break; }
            }
            if (!ok) {
                if (run_count == 0) return 0;
                break;
            }
            page_lba = vfs_sb_block_to_lba(sb, fc);
        }

        if (run_count == 0) {
            run_start_lba = page_lba;
        } else if (page_lba != run_start_lba + run_sectors) {
            break;
        }

        run_sectors += spp;
        run_count++;
    }

    if (run_count == 0)
        return 0;

    // --- Single page fast path: read directly into page data ---
    if (run_count == 1) {
        pc_page_t *pg = pc_page_alloc();
        if (!pg) return 0;
        pg->cluster_id = cluster_id;
        pg->page_index = page_index;
        if (pc_read_sectors(bdev, run_start_lba, run_sectors, pg->data) != 0) {
            pc_page_free(pg);
            return 0;
        }
        unsigned long psb = page_index * PAGE_SIZE;
        if (psb + PAGE_SIZE > file_size) {
            unsigned long v = file_size - psb;
            mm_memset(pg->data + v, 0, PAGE_SIZE - v);
        }
        pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
        pc_page_t *r = pagecache_insert(pg);
        if (r != pg) pc_page_free(pg);
        return r;
    }

    // --- Multi-page path: one big I/O, then distribute into pages ---
    unsigned long total_bytes = run_sectors * ss;
    void *buf = kalloc(total_bytes);
    if (!buf) {
        pc_page_t *pg = pc_page_alloc();
        if (!pg) return 0;
        pg->cluster_id = cluster_id;
        pg->page_index = page_index;
        if (pc_read_sectors(bdev, run_start_lba, spp, pg->data) != 0) {
            pc_page_free(pg);
            return 0;
        }
        unsigned long psb = page_index * PAGE_SIZE;
        if (psb + PAGE_SIZE > file_size) {
            unsigned long v = file_size - psb;
            mm_memset(pg->data + v, 0, PAGE_SIZE - v);
        }
        pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
        pc_page_t *r = pagecache_insert(pg);
        if (r != pg) pc_page_free(pg);
        return r;
    }

    if (pc_read_sectors(bdev, run_start_lba, run_sectors, buf) != 0) {
        kfree(buf);
        return 0;
    }

    // Distribute the big buffer into individual cache pages
    pc_page_t *result = 0;
    for (unsigned long i = 0; i < run_count; i++) {
        unsigned long pi = page_index + i;
        pc_page_t *pg = pc_page_alloc();
        if (!pg) break;
        pg->cluster_id = cluster_id;
        pg->page_index = pi;
        mm_memcpy(pg->data, (uint8_t *)buf + i * PAGE_SIZE, PAGE_SIZE);

        unsigned long psb = pi * PAGE_SIZE;
        if (psb + PAGE_SIZE > file_size) {
            unsigned long v = file_size - psb;
            mm_memset(pg->data + v, 0, PAGE_SIZE - v);
        }

        pg->flags = PC_PAGE_VALID | PC_PAGE_REFERENCED;
        if (i > 0) pg->flags |= PC_PAGE_READAHEAD;

        pc_page_t *ins = pagecache_insert(pg);
        if (ins != pg) pc_page_free(pg);

        if (i == 0)
            result = ins;
        else
            __sync_fetch_and_add(&pc_stat_readahead, 1);
    }

    kfree(buf);
    return result;
}

// ============================================================================
// pagecache_get() — the primary read path
// ============================================================================

pc_page_t* pagecache_get(unsigned long cluster_id, unsigned long page_index,
                         unsigned long file_size,
                         struct vfs_superblock *sb, unsigned long start_cluster)
{
    VM_BUG_ON(sb == NULL);
    VM_BUG_ON(start_cluster < 2);
    if (!pc_initialized || !sb || start_cluster < 2)
        return 0;

    // Check memory pressure first and do deferred writeback
    if (pc_writeback_pending) {
        pc_writeback_pending = 0;
        pagecache_flush_all();
    }

    // 1. Try cache lookup (no I/O lock needed)
    pc_page_t *pg = pagecache_lookup(cluster_id, page_index);
    if (pg) {
        WARN_ON_ONCE(!(pg->flags & PC_PAGE_VALID));  /* lookup returned a page without PC_PAGE_VALID: stale insertion */
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
    vfs_sb_lock_io(sb);
    pc_page_t *result = pc_coalesced_read(sb, cluster_id, start_cluster,
                                           page_index, file_size);
    if (result) {
        vfs_sb_unlock_io(sb);
        return result;
    }

    // 4. Fallback for fragmented files (block_size < PAGE_SIZE with
    //    non-contiguous blocks inside a single page).
    {
        const block_device_t *bdev = vfs_sb_bdev(sb);
        unsigned long bs  = vfs_sb_block_size(sb);
        unsigned long ss  = vfs_sb_sector_size(sb);
        unsigned long eoc = vfs_sb_end_of_chain(sb);
        unsigned long blocks_per_page = PAGE_SIZE / bs;
        if (blocks_per_page == 0) blocks_per_page = 1;
        unsigned long first_ci = page_index * blocks_per_page;

        pc_page_t *new_pg = pc_page_alloc();
        if (!new_pg) {
            vfs_sb_unlock_io(sb);
            return 0;
        }
        new_pg->cluster_id = cluster_id;
        new_pg->page_index = page_index;

        unsigned long cur_block = pc_walk_chain(sb, start_cluster, first_ci);
        if (cur_block == 0 || cur_block >= eoc) {
            vfs_sb_unlock_io(sb);
            pc_page_free(new_pg);
            return 0;
        }

        unsigned long secs_per_block = bs / ss;
        unsigned offset = 0;
        for (unsigned long c = 0; c < blocks_per_page; c++) {
            if (cur_block == 0 || cur_block >= eoc)
                break;
            unsigned long lba = vfs_sb_block_to_lba(sb, cur_block);
            int st = pc_read_sectors(bdev, lba, secs_per_block,
                                     new_pg->data + offset);
            if (st != 0) {
                vfs_sb_unlock_io(sb);
                pc_page_free(new_pg);
                return 0;
            }
            offset += bs;
            if (c + 1 < blocks_per_page)
                cur_block = vfs_sb_next_block(sb, cur_block);
        }
        vfs_sb_unlock_io(sb);

        if (offset < PAGE_SIZE)
            mm_memset(new_pg->data + offset, 0, PAGE_SIZE - offset);

        unsigned long page_start_byte = page_index * PAGE_SIZE;
        if (page_start_byte + PAGE_SIZE > file_size) {
            unsigned long valid_bytes = file_size - page_start_byte;
            mm_memset(new_pg->data + valid_bytes, 0, PAGE_SIZE - valid_bytes);
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
            if (pg->cluster_id == cluster_id && (pg->flags & PC_PAGE_DIRTY) &&
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
        unsigned long bs            = vfs_sb_block_size(sb);
        unsigned long ss            = vfs_sb_sector_size(sb);
        unsigned long secs_per_block = bs / ss;
        unsigned long eoc           = vfs_sb_end_of_chain(sb);
        unsigned long reserved_meta = vfs_sb_reserved_meta_block(sb);

        vfs_sb_lock_io(sb);
        for (int i = 0; i < batch_count; i++) {
            pc_page_t *p = batch[i];

            if (bs >= PAGE_SIZE) {
                unsigned long ppb = bs / PAGE_SIZE;
                if (ppb == 0) ppb = 1;

                unsigned long block_index = p->page_index / ppb;
                unsigned long sub_page    = p->page_index % ppb;

                unsigned long disk_block = pc_walk_chain(sb, p->cluster_id,
                                                          block_index);
                if (disk_block == 0 || disk_block >= eoc) {
                    p->flags &= ~PC_PAGE_LOCKED;
                    continue;
                }
                /* Guard against the FS-reserved metadata block (FAT32 root
                 * cluster) — writing user-data here would corrupt the root
                 * directory. */
                if (reserved_meta && disk_block == reserved_meta) {
                    kprintf("pagecache: BUG: flush_file to reserved meta block! "
                            "inode_id=%lu page_idx=%lu bi=%lu\n",
                            p->cluster_id, p->page_index, block_index);
                    p->flags &= ~PC_PAGE_LOCKED;
                    continue;
                }

                unsigned long lba = vfs_sb_block_to_lba(sb, disk_block);

                if (bs == PAGE_SIZE) {
                    pc_write_sectors(bdev, lba, secs_per_block, p->data);
                } else {
                    void *tmp = kalloc(bs);
                    if (tmp) {
                        pc_read_sectors(bdev, lba, secs_per_block, tmp);
                        mm_memcpy((uint8_t *)tmp + sub_page * PAGE_SIZE,
                                  p->data, PAGE_SIZE);
                        pc_write_sectors(bdev, lba, secs_per_block, tmp);
                        kfree(tmp);
                    }
                }
            } else {
                unsigned long bpp = PAGE_SIZE / bs;
                unsigned long first_block_offset = p->page_index * bpp;

                unsigned long cur_block = pc_walk_chain(sb, p->cluster_id,
                                                         first_block_offset);
                unsigned offset = 0;
                for (unsigned long c = 0; c < bpp; c++) {
                    if (cur_block == 0 || cur_block >= eoc)
                        break;
                    unsigned long lba = vfs_sb_block_to_lba(sb, cur_block);
                    pc_write_sectors(bdev, lba, secs_per_block,
                                     p->data + offset);
                    offset += bs;
                    if (c + 1 < bpp)
                        cur_block = vfs_sb_next_block(sb, cur_block);
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

int pagecache_flush_all(void)
{
    might_sleep();
    if (!pc_initialized)
        return 0;

    int wrote = 0;
    #define FLUSH_ALL_BATCH 32
    pc_page_t *batch[FLUSH_ALL_BATCH];
    int batch_count;

    do {
        batch_count = 0;
        uint64_t flags;
        spin_lock_irqsave(&pc_dirty_lock, &flags);
        pc_page_t *pg = pc_dirty_sentinel.dirty_next;
        while (pg != &pc_dirty_sentinel && batch_count < FLUSH_ALL_BATCH) {
            if ((pg->flags & PC_PAGE_DIRTY) && !(pg->flags & PC_PAGE_LOCKED)) {
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
        const block_device_t *bdev   = vfs_sb_bdev(sb);
        unsigned long bs              = vfs_sb_block_size(sb);
        unsigned long ss              = vfs_sb_sector_size(sb);
        unsigned long secs_per_block  = bs / ss;
        unsigned long eoc             = vfs_sb_end_of_chain(sb);
        unsigned long reserved_meta   = vfs_sb_reserved_meta_block(sb);

        vfs_sb_lock_io(sb);
        for (int i = 0; i < batch_count; i++) {
            pc_page_t *p = batch[i];

            if (bs >= PAGE_SIZE) {
                unsigned long ppb = bs / PAGE_SIZE;
                if (ppb == 0) ppb = 1;

                unsigned long block_index = p->page_index / ppb;
                unsigned long sub_page    = p->page_index % ppb;

                unsigned long disk_block = pc_walk_chain(sb, p->cluster_id,
                                                          block_index);
                if (disk_block == 0 || disk_block >= eoc) {
                    p->flags &= ~PC_PAGE_LOCKED;
                    continue;
                }
                if (reserved_meta && disk_block == reserved_meta) {
                    kprintf("pagecache: BUG: flush_all to reserved meta block! "
                            "inode_id=%lu page_idx=%lu bi=%lu\n",
                            p->cluster_id, p->page_index, block_index);
                    p->flags &= ~PC_PAGE_LOCKED;
                    continue;
                }

                unsigned long lba = vfs_sb_block_to_lba(sb, disk_block);
                if (bs == PAGE_SIZE) {
                    pc_write_sectors(bdev, lba, secs_per_block, p->data);
                } else {
                    void *tmp = kalloc(bs);
                    if (tmp) {
                        pc_read_sectors(bdev, lba, secs_per_block, tmp);
                        mm_memcpy((uint8_t *)tmp + sub_page * PAGE_SIZE,
                                  p->data, PAGE_SIZE);
                        pc_write_sectors(bdev, lba, secs_per_block, tmp);
                        kfree(tmp);
                    }
                }
            } else {
                unsigned long bpp = PAGE_SIZE / bs;
                unsigned long first_block_offset = p->page_index * bpp;

                unsigned long cur_block = pc_walk_chain(sb, p->cluster_id,
                                                         first_block_offset);
                unsigned offset = 0;
                for (unsigned long c = 0; c < bpp; c++) {
                    if (cur_block == 0 || cur_block >= eoc)
                        break;
                    unsigned long lba = vfs_sb_block_to_lba(sb, cur_block);
                    pc_write_sectors(bdev, lba, secs_per_block,
                                     p->data + offset);
                    offset += bs;
                    if (c + 1 < bpp)
                        cur_block = vfs_sb_next_block(sb, cur_block);
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

    } while (batch_count == FLUSH_ALL_BATCH);

    #undef FLUSH_ALL_BATCH
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

void pagecache_invalidate_range(unsigned long cluster_id, unsigned long new_size)
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
            if (pg->cluster_id == cluster_id && pg->page_index >= first_invalid) {
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
        if (ra->sequential_count >= 2 && ra->ra_pages < PC_READAHEAD_MAX)
            ra->ra_pages = (ra->ra_pages < 1) ? 1 : ra->ra_pages * 2;
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
    stats->hits             = pc_stat_hits;
    stats->misses           = pc_stat_misses;
    stats->readahead_pages  = pc_stat_readahead;
    stats->evictions        = pc_stat_evictions;
    stats->dirty_writebacks = pc_stat_writebacks;
    stats->total_pages      = pc_stat_total_pages;
}

// ============================================================================
// Timer callback
// ============================================================================

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
