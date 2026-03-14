// LikeOS-64 Unified Page Cache
//
// Caches file data pages indexed by (start_cluster, page_index).
// CLOCK eviction algorithm on a global LRU ring.
// Per-bucket spinlocks for the hash table; global spinlock for LRU/dirty lists.
// Dirty write-back: periodic timer + flush on close/sync.
// Sequential read-ahead with adaptive window sizing.

#include "../../include/kernel/pagecache.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/block.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/icache.h"

// ============================================================================
// External FAT32 internals we need (declared in fat32.h)
// ============================================================================

// Convert cluster number to LBA
static inline unsigned long pc_cluster_to_lba(fat32_fs_t *fs, unsigned long cluster)
{
    return fs->part_lba_offset + fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

// Read sectors from block device (chunked, like fat32.c's read_sectors)
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
    page->lru_next = pc_lru_sentinel.lru_next;
    page->lru_prev = &pc_lru_sentinel;
    pc_lru_sentinel.lru_next->lru_prev = page;
    pc_lru_sentinel.lru_next = page;
}

static inline void lru_remove(pc_page_t *page)
{
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

// Returns the cluster number that contains the data for page_index
// (considering the cluster size of the filesystem).
// A page is 4096 bytes.  A cluster may be 4096, 8192, 16384, etc.
// pages_per_cluster = cluster_size / PAGE_SIZE.
// cluster_index = page_index / pages_per_cluster.
// sub_index = page_index % pages_per_cluster (sector offset within cluster).
//
// Uses the inode cache's cluster chain map for O(1) lookups when available.
// Falls back to linear FAT chain walk if no inode is cached.
// Returns 0 on error.
static unsigned long pc_walk_chain(fat32_fs_t *fs, unsigned long start_cluster,
                                   unsigned long cluster_index)
{
    // Try the inode chain cache first (O(1) lookup)
    unsigned long result = icache_chain_get(start_cluster, cluster_index,
                                            (struct fat32_fs *)fs);
    if (result != 0 && result < 0x0FFFFFF8)
        return result;

    // Fallback: linear walk (icache_chain_get already does this internally,
    // so reaching here means the chain truly ends before cluster_index)
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

            // Flush this single dirty page inline.
            extern fat32_fs_t *g_root_fs;
            fat32_fs_t *flush_fs = g_root_fs;
            int flush_ok = 0;
            if (flush_fs) {
                unsigned cs = flush_fs->sectors_per_cluster * flush_fs->bytes_per_sector;
                fat32_io_lock();
                if (cs >= PAGE_SIZE) {
                    unsigned ppc = cs / PAGE_SIZE;
                    if (ppc == 0) ppc = 1;
                    unsigned long ci = pg->page_index / ppc;
                    unsigned long sp = pg->page_index % ppc;
                    unsigned long dc = pc_walk_chain(flush_fs, pg->cluster_id, ci);
                    if (dc >= 2 && dc < 0x0FFFFFF8) {
                        unsigned long lba = pc_cluster_to_lba(flush_fs, dc);
                        if (cs == PAGE_SIZE) {
                            pc_write_sectors(flush_fs->bdev, lba,
                                             flush_fs->sectors_per_cluster, pg->data);
                        } else {
                            void *tmp = kalloc(cs);
                            if (tmp) {
                                pc_read_sectors(flush_fs->bdev, lba,
                                                flush_fs->sectors_per_cluster, tmp);
                                mm_memcpy((uint8_t *)tmp + sp * PAGE_SIZE,
                                          pg->data, PAGE_SIZE);
                                pc_write_sectors(flush_fs->bdev, lba,
                                                 flush_fs->sectors_per_cluster, tmp);
                                kfree(tmp);
                            }
                        }
                        flush_ok = 1;
                    }
                } else {
                    unsigned cpp = PAGE_SIZE / cs;
                    unsigned long fco = pg->page_index * cpp;
                    unsigned long cur = pc_walk_chain(flush_fs, pg->cluster_id, fco);
                    unsigned off = 0;
                    flush_ok = 1;
                    for (unsigned c = 0; c < cpp; c++) {
                        if (cur == 0 || cur >= 0x0FFFFFF8) break;
                        unsigned long lba = pc_cluster_to_lba(flush_fs, cur);
                        pc_write_sectors(flush_fs->bdev, lba,
                                         flush_fs->sectors_per_cluster,
                                         pg->data + off);
                        off += cs;
                        if (c + 1 < cpp)
                            cur = fat32_next_cluster_cached(flush_fs, cur);
                    }
                }
                fat32_io_unlock();
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
// in a single I/O if their clusters are physically contiguous on disk.
// Must be called under fat32_io_lock.
// Returns the requested page on success, NULL if the first page's clusters
// are not contiguous (caller should fall back to per-cluster reads).
static pc_page_t* pc_coalesced_read(fat32_fs_t *fs, unsigned long cluster_id,
                                     unsigned long start_cluster,
                                     unsigned long page_index,
                                     unsigned long file_size)
{
    unsigned cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    unsigned long file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned long spp = PAGE_SIZE / fs->bytes_per_sector; // sectors per page

    unsigned long run_start_lba = 0;
    unsigned long run_sectors   = 0;
    unsigned long run_count     = 0;

    for (unsigned long pi = page_index;
         pi < file_pages && run_count < PC_COALESCE_MAX; pi++) {

        // After the first page, stop at already-cached pages
        if (pi != page_index && pagecache_lookup(cluster_id, pi))
            break;

        unsigned long page_lba;

        if (cluster_size >= PAGE_SIZE) {
            // One cluster ≥ one page — sub-page addressing within cluster
            unsigned ppc = cluster_size / PAGE_SIZE;
            if (!ppc) ppc = 1;
            unsigned long ci  = pi / ppc;
            unsigned long sub = pi % ppc;
            unsigned long cl  = pc_walk_chain(fs, start_cluster, ci);
            if (!cl || cl >= 0x0FFFFFF8) break;
            page_lba = pc_cluster_to_lba(fs, cl) + sub * spp;
        } else {
            // Multiple clusters per page — verify intra-page contiguity
            unsigned cpp = PAGE_SIZE / cluster_size;
            unsigned long fci = pi * cpp;
            unsigned long fc  = pc_walk_chain(fs, start_cluster, fci);
            if (!fc || fc >= 0x0FFFFFF8) break;
            int ok = 1;
            for (unsigned c = 1; c < cpp; c++) {
                unsigned long nc = pc_walk_chain(fs, start_cluster, fci + c);
                if (nc != fc + c) { ok = 0; break; }
            }
            if (!ok) {
                if (run_count == 0) return 0; // first page fragmented
                break; // stop coalescing, use what we have
            }
            page_lba = pc_cluster_to_lba(fs, fc);
        }

        // Check contiguity with the run so far
        if (run_count == 0) {
            run_start_lba = page_lba;
        } else if (page_lba != run_start_lba + run_sectors) {
            break; // gap — stop
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
        if (pc_read_sectors(fs->bdev, run_start_lba, run_sectors, pg->data) != 0) {
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
    unsigned long total_bytes = run_sectors * fs->bytes_per_sector;
    void *buf = kalloc(total_bytes);
    if (!buf) {
        // Can't allocate big buffer — fall back to single page
        pc_page_t *pg = pc_page_alloc();
        if (!pg) return 0;
        pg->cluster_id = cluster_id;
        pg->page_index = page_index;
        if (pc_read_sectors(fs->bdev, run_start_lba, spp, pg->data) != 0) {
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

    if (pc_read_sectors(fs->bdev, run_start_lba, run_sectors, buf) != 0) {
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
                         struct fat32_fs *fs_raw, unsigned long start_cluster)
{
    if (!pc_initialized)
        return 0;

    fat32_fs_t *fs = (fat32_fs_t *)fs_raw;
    if (!fs || start_cluster < 2)
        return 0;

    // Check memory pressure first and do deferred writeback
    if (pc_writeback_pending) {
        pc_writeback_pending = 0;
        pagecache_flush_all();
    }

    // 1. Try cache lookup (no I/O lock needed)
    pc_page_t *pg = pagecache_lookup(cluster_id, page_index);
    if (pg) {
        __sync_fetch_and_add(&pc_stat_hits, 1);
        return pg;
    }

    // 2. Cache miss — need to read from disk
    __sync_fetch_and_add(&pc_stat_misses, 1);

    // Check if page_index is within file bounds
    unsigned long file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (page_index >= file_pages)
        return 0;

    // Check memory pressure — evict if needed before allocating
    pagecache_reclaim_if_needed();

    // 3. Try coalesced read: reads up to 64KB of contiguous pages in one
    //    USB transfer for massively improved sequential I/O performance.
    fat32_io_lock();
    pc_page_t *result = pc_coalesced_read(fs, cluster_id, start_cluster,
                                           page_index, file_size);
    if (result) {
        fat32_io_unlock();
        return result;
    }

    // 4. Fallback for fragmented files (cluster_size < PAGE_SIZE with
    //    non-contiguous clusters within a single page).
    {
        unsigned cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
        unsigned clusters_per_page = PAGE_SIZE / cluster_size;
        if (clusters_per_page == 0) clusters_per_page = 1;
        unsigned long first_ci = page_index * clusters_per_page;

        pc_page_t *new_pg = pc_page_alloc();
        if (!new_pg) {
            fat32_io_unlock();
            return 0;
        }
        new_pg->cluster_id = cluster_id;
        new_pg->page_index = page_index;

        unsigned long cur_cluster = pc_walk_chain(fs, start_cluster, first_ci);
        if (cur_cluster == 0 || cur_cluster >= 0x0FFFFFF8) {
            fat32_io_unlock();
            pc_page_free(new_pg);
            return 0;
        }

        // Read each cluster individually (scattered I/O for fragmented files)
        unsigned offset = 0;
        for (unsigned c = 0; c < clusters_per_page; c++) {
            if (cur_cluster == 0 || cur_cluster >= 0x0FFFFFF8)
                break;
            unsigned long lba = pc_cluster_to_lba(fs, cur_cluster);
            int st = pc_read_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                     new_pg->data + offset);
            if (st != 0) {
                fat32_io_unlock();
                pc_page_free(new_pg);
                return 0;
            }
            offset += cluster_size;
            if (c + 1 < clusters_per_page)
                cur_cluster = fat32_next_cluster_cached(fs, cur_cluster);
        }
        fat32_io_unlock();

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

        // Flush the batch under fat32_io_lock
        fat32_io_lock();
        for (int i = 0; i < batch_count; i++) {
            pc_page_t *p = batch[i];
            // Find the fs pointer — we need it from the global root_fs.
            // Since all files are on the same FAT32 volume, use g_root_fs.
            extern fat32_fs_t *g_root_fs;
            fat32_fs_t *fs = g_root_fs;
            if (!fs) {
                p->flags &= ~PC_PAGE_LOCKED;
                continue;
            }

            unsigned cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

            if (cluster_size >= PAGE_SIZE) {
                unsigned pages_per_cluster = cluster_size / PAGE_SIZE;
                if (pages_per_cluster == 0) pages_per_cluster = 1;

                unsigned long cluster_index = p->page_index / pages_per_cluster;
                unsigned long sub_page      = p->page_index % pages_per_cluster;

                unsigned long disk_cluster = pc_walk_chain(fs, p->cluster_id,
                                                           cluster_index);
                if (disk_cluster == 0 || disk_cluster >= 0x0FFFFFF8) {
                    p->flags &= ~PC_PAGE_LOCKED;
                    continue;
                }

                unsigned long lba = pc_cluster_to_lba(fs, disk_cluster);

                if (cluster_size == PAGE_SIZE) {
                    // Write the page directly as one cluster
                    pc_write_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                     p->data);
                } else {
                    // Read-modify-write: read full cluster, overlay our page,
                    // write back
                    void *tmp = kalloc(cluster_size);
                    if (tmp) {
                        pc_read_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                        tmp);
                        mm_memcpy((uint8_t *)tmp + sub_page * PAGE_SIZE,
                                  p->data, PAGE_SIZE);
                        pc_write_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                         tmp);
                        kfree(tmp);
                    }
                }
            } else {
                // Multiple clusters per page (cluster_size < PAGE_SIZE).
                unsigned clusters_per_page = PAGE_SIZE / cluster_size;
                unsigned long first_cluster_offset =
                    p->page_index * clusters_per_page;

                unsigned long cur_cluster = pc_walk_chain(fs, p->cluster_id,
                                                          first_cluster_offset);
                unsigned offset = 0;
                for (unsigned c = 0; c < clusters_per_page; c++) {
                    if (cur_cluster == 0 || cur_cluster >= 0x0FFFFFF8)
                        break;
                    unsigned long lba = pc_cluster_to_lba(fs, cur_cluster);
                    pc_write_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                     p->data + offset);
                    offset += cluster_size;
                    if (c + 1 < clusters_per_page)
                        cur_cluster = fat32_next_cluster_cached(fs, cur_cluster);
                }
            }

            // Clear dirty flag
            p->flags &= ~(PC_PAGE_DIRTY | PC_PAGE_LOCKED);
            wrote++;
            __sync_fetch_and_add(&pc_stat_writebacks, 1);

            // Remove from dirty list
            spin_lock_irqsave(&pc_dirty_lock, &flags);
            dirty_list_remove(p);
            spin_unlock_irqrestore(&pc_dirty_lock, flags);
        }
        fat32_io_unlock();

    } while (batch_count == FLUSH_BATCH); // loop if we filled the batch

    #undef FLUSH_BATCH
    return wrote;
}

// ============================================================================
// Flush all dirty pages
// ============================================================================

int pagecache_flush_all(void)
{
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

        // Flush under fat32_io_lock
        extern fat32_fs_t *g_root_fs;
        fat32_fs_t *fs = g_root_fs;
        if (!fs) {
            for (int i = 0; i < batch_count; i++)
                batch[i]->flags &= ~PC_PAGE_LOCKED;
            break;
        }

        fat32_io_lock();
        for (int i = 0; i < batch_count; i++) {
            pc_page_t *p = batch[i];
            unsigned cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

            if (cluster_size >= PAGE_SIZE) {
                unsigned pages_per_cluster = cluster_size / PAGE_SIZE;
                if (pages_per_cluster == 0) pages_per_cluster = 1;

                unsigned long cluster_index = p->page_index / pages_per_cluster;
                unsigned long sub_page      = p->page_index % pages_per_cluster;

                unsigned long disk_cluster = pc_walk_chain(fs, p->cluster_id,
                                                           cluster_index);
                if (disk_cluster == 0 || disk_cluster >= 0x0FFFFFF8) {
                    p->flags &= ~PC_PAGE_LOCKED;
                    continue;
                }

                unsigned long lba = pc_cluster_to_lba(fs, disk_cluster);

                if (cluster_size == PAGE_SIZE) {
                    pc_write_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                     p->data);
                } else {
                    void *tmp = kalloc(cluster_size);
                    if (tmp) {
                        pc_read_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                        tmp);
                        mm_memcpy((uint8_t *)tmp + sub_page * PAGE_SIZE,
                                  p->data, PAGE_SIZE);
                        pc_write_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                         tmp);
                        kfree(tmp);
                    }
                }
            } else {
                // Multiple clusters per page (cluster_size < PAGE_SIZE).
                unsigned clusters_per_page = PAGE_SIZE / cluster_size;
                unsigned long first_cluster_offset =
                    p->page_index * clusters_per_page;

                unsigned long cur_cluster = pc_walk_chain(fs, p->cluster_id,
                                                          first_cluster_offset);
                unsigned offset = 0;
                for (unsigned c = 0; c < clusters_per_page; c++) {
                    if (cur_cluster == 0 || cur_cluster >= 0x0FFFFFF8)
                        break;
                    unsigned long lba = pc_cluster_to_lba(fs, cur_cluster);
                    pc_write_sectors(fs->bdev, lba, fs->sectors_per_cluster,
                                     p->data + offset);
                    offset += cluster_size;
                    if (c + 1 < clusters_per_page)
                        cur_cluster = fat32_next_cluster_cached(fs, cur_cluster);
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
        fat32_io_unlock();

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

    // Call block device sync if available
    extern fat32_fs_t *g_root_fs;
    if (g_root_fs && g_root_fs->bdev && g_root_fs->bdev->sync) {
        fat32_io_lock();
        g_root_fs->bdev->sync((block_device_t *)g_root_fs->bdev);
        fat32_io_unlock();
    }
    return wrote;
}

// ============================================================================
// Invalidation
// ============================================================================

void pagecache_invalidate_file(unsigned long cluster_id)
{
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
                         struct fat32_fs *fs_raw, unsigned long start_cluster)
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
                                       fs_raw, start_cluster);
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
